/*
 *  ReactOS kernel
 *  Copyright (C) 2006 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/userenv/gpolicy.c
 * PURPOSE:         Group policy functions
 * PROGRAMMER:      Thomas Weidenmueller <w3seek@reactos.com>
 */
/*
 * Ported to Wine for WineExplorer (docs/GROUP_POLICY.md, step 3). Added here: the notification thread also
 * watches the serial numbers that the policy writer (wexpolicy, in session 0) leaves in the registry, because
 * the named events of this file live in each session's own server and session 0 cannot set them.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "sddl.h"
#include "userenv.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( userenv );

typedef struct _GP_NOTIFY
{
    struct _GP_NOTIFY *Next;
    HANDLE hEvent;
    BOOL bMachine;
} GP_NOTIFY, *PGP_NOTIFY;

typedef enum
{
    gpaUpdate = 0,
    gpaTerminate
} GP_ACTION;

static const WCHAR szLocalGPApplied[] = L"userenv: User Group Policy has been applied";
static const WCHAR szLocalGPMutex[] = L"userenv: user policy mutex";
static const WCHAR szLocalGPRefreshEvent[] = L"userenv: user policy refresh event";
static const WCHAR szLocalGPForceRefreshEvent[] = L"userenv: user policy force refresh event";
static const WCHAR szLocalGPDoneEvent[] = L"userenv: User Policy Foreground Done Event";
static const WCHAR szMachineGPApplied[] = L"Global\\userenv: Machine Group Policy has been applied";
static const WCHAR szMachineGPMutex[] = L"Global\\userenv: machine policy mutex";
static const WCHAR szMachineGPRefreshEvent[] = L"Global\\userenv: machine policy refresh event";
static const WCHAR szMachineGPForceRefreshEvent[] = L"Global\\userenv: machine policy force refresh event";
static const WCHAR szMachineGPDoneEvent[] = L"Global\\userenv: Machine Policy Foreground Done Event";

static CRITICAL_SECTION GPNotifyLock;
static PGP_NOTIFY NotificationList = NULL;
static GP_ACTION GPNotificationAction = gpaUpdate;
static HANDLE hNotificationThread = NULL;
static HANDLE hNotificationThreadEvent = NULL;
static HANDLE hLocalGPAppliedEvent = NULL;
static HANDLE hMachineGPAppliedEvent = NULL;
static HINSTANCE gp_hInstance;

/*
 * Build the security descriptor used for the group policy events and the
 * critical section mutex: SYSTEM and Administrators get full control,
 * Everyone may wait on and query the object.
 */
static PSECURITY_DESCRIPTOR
CreateDefaultSecurityDescriptor(void)
{
    PSECURITY_DESCRIPTOR pSD = NULL;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GXRC;;;WD)",
            SDDL_REVISION_1,
            &pSD,
            NULL))
    {
        ERR("Failed to convert the default security descriptor (error %lu)\n",
            GetLastError());
        return NULL;
    }

    return pSD;
}

/* WineExplorer: a serial number that the policy writer changes after it has written policy */
struct gp_watch
{
    HKEY         root;        /* HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER */
    const WCHAR *watch_path;  /* the key that is watched, with all below it; it exists before the serial does */
    const WCHAR *serial_path; /* the key of the value "Serial" */
    HKEY         key;
    HANDLE       event;
    DWORD        serial;
};

static DWORD
ReadGPSerial(const struct gp_watch *watch)
{
    DWORD serial = 0, size = sizeof(serial);
    HKEY key;

    if (RegOpenKeyExW(watch->root, watch->serial_path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key)) return 0;
    if (RegQueryValueExW(key, L"Serial", NULL, NULL, (BYTE *)&serial, &size)) serial = 0;
    RegCloseKey(key);
    return serial;
}

static void
ArmGPWatch(struct gp_watch *watch)
{
    if (watch->key != NULL && watch->event != NULL)
        RegNotifyChangeKeyValue(watch->key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                watch->event, TRUE);
}

static void
OpenGPWatch(struct gp_watch *watch)
{
    watch->key = NULL;
    watch->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (RegOpenKeyExW(watch->root, watch->watch_path, 0, KEY_NOTIFY | KEY_WOW64_64KEY, &watch->key))
        watch->key = NULL;
    ArmGPWatch(watch);
    watch->serial = ReadGPSerial(watch);
}

static void
CloseGPWatch(struct gp_watch *watch)
{
    if (watch->key != NULL) RegCloseKey(watch->key);
    if (watch->event != NULL) CloseHandle(watch->event);
}

/* the watched key changed: has the writer finished a round of policy? */
static BOOL
GPWatchFired(struct gp_watch *watch)
{
    DWORD serial;

    ArmGPWatch(watch);
    serial = ReadGPSerial(watch);
    if (serial == watch->serial) return FALSE;
    watch->serial = serial;
    return TRUE;
}

static void
InitializeGPNotifications(void)
{
    InitializeCriticalSection(&GPNotifyLock);
}

static void
UninitializeGPNotifications(void)
{
    EnterCriticalSection(&GPNotifyLock);

    /* rundown the notification thread */
    if (hNotificationThread != NULL)
    {
        /* notify the thread */
        GPNotificationAction = gpaTerminate;
        SetEvent(hNotificationThreadEvent);

        LeaveCriticalSection(&GPNotifyLock);

        /* wait for the thread to terminate itself */
        WaitForSingleObject(hNotificationThread,
                            INFINITE);

        EnterCriticalSection(&GPNotifyLock);

        if (hNotificationThread != NULL)
        {
            /* the handle should be closed by the thread,
               just in case that didn't happen for an unknown reason */
            CloseHandle(hNotificationThread);
            hNotificationThread = NULL;
        }
    }

    if (hNotificationThreadEvent != NULL)
    {
        CloseHandle(hNotificationThreadEvent);
        hNotificationThreadEvent = NULL;
    }

    LeaveCriticalSection(&GPNotifyLock);

    DeleteCriticalSection(&GPNotifyLock);
}

static
void
NotifyGPEvents(BOOL bMachine)
{
    PGP_NOTIFY Notify = NotificationList;

    while (Notify != NULL)
    {
        if (Notify->bMachine == bMachine)
        {
            SetEvent(Notify->hEvent);
        }

        Notify = Notify->Next;
    }
}

static
DWORD
WINAPI
GPNotificationThreadProc(LPVOID lpParameter)
{
    HMODULE hModule;
    DWORD WaitResult, WaitCount;
    HANDLE WaitHandles[5];
    struct gp_watch MachineWatch = { HKEY_LOCAL_MACHINE,
                                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Group Policy",
                                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Group Policy\\WineExplorer Applied" };
    struct gp_watch UserWatch = { HKEY_CURRENT_USER, L"Software\\Policies",
                                  L"Software\\Policies\\WineExplorer\\Policy Applied" };

    /* reference the library so we don't screw up if the application
       causes the DLL to unload while this thread is still running */
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)gp_hInstance,
                           &hModule))
    {
        OpenGPWatch(&MachineWatch);
        OpenGPWatch(&UserWatch);
        if (MachineWatch.event == NULL || UserWatch.event == NULL)
        {
            ERR("No events for the policy serial numbers\n");
            CloseGPWatch(&MachineWatch);
            CloseGPWatch(&UserWatch);
            MachineWatch.event = UserWatch.event = NULL;
        }

        EnterCriticalSection(&GPNotifyLock);

        WaitHandles[0] = hNotificationThreadEvent;
        for (;;)
        {
            if (NotificationList == NULL || MachineWatch.event == NULL)
                break;

            /* WineExplorer: the two serial numbers come before the local event, which is not always there */
            WaitCount = 4;
            WaitHandles[1] = hMachineGPAppliedEvent;
            WaitHandles[2] = MachineWatch.event;
            WaitHandles[3] = UserWatch.event;

            if (hLocalGPAppliedEvent != NULL)
            {
                WaitHandles[4] = hLocalGPAppliedEvent;
                WaitCount++;
            }

            LeaveCriticalSection(&GPNotifyLock);

            WaitResult = WaitForMultipleObjects(WaitCount,
                                                WaitHandles,
                                                FALSE,
                                                INFINITE);

            EnterCriticalSection(&GPNotifyLock);

            if (WaitResult != WAIT_FAILED)
            {
                if (WaitResult == WAIT_OBJECT_0)
                {
                    ResetEvent(hNotificationThreadEvent);

                    if (GPNotificationAction == gpaTerminate)
                    {
                        /* terminate the thread */
                        break;
                    }
                }
                else if (WaitResult == WAIT_OBJECT_0 + 1 || WaitResult == WAIT_OBJECT_0 + 4)
                {
                    /* group policies have been applied */
                    if (NotificationList != NULL)
                    {
                        NotifyGPEvents((WaitResult == WAIT_OBJECT_0 + 1));
                    }
                }
                else if (WaitResult == WAIT_OBJECT_0 + 2)
                {
                    if (GPWatchFired(&MachineWatch) && NotificationList != NULL) NotifyGPEvents(TRUE);
                }
                else if (WaitResult == WAIT_OBJECT_0 + 3)
                {
                    if (GPWatchFired(&UserWatch) && NotificationList != NULL) NotifyGPEvents(FALSE);
                }
                else if (WaitResult == WAIT_ABANDONED_0 + 4)
                {
                    /* In case the local group policies event was abandoned, keep watching!
                       But close the handle as it's no longer of any use. */
                    if (hLocalGPAppliedEvent != NULL)
                    {
                        CloseHandle(hLocalGPAppliedEvent);
                        hLocalGPAppliedEvent = NULL;
                    }
                }
                else if (WaitResult == WAIT_ABANDONED_0 || WaitResult == WAIT_ABANDONED_0 + 1)
                {
                    /* terminate the thread if the machine group policies event was abandoned
                       or for some reason the rundown event got abandoned. */
                    break;
                }
                else
                {
                    WARN("Unexpected wait result watching the group policy events: 0x%lx\n", WaitResult);
                    break;
                }

                if (NotificationList == NULL)
                    break;
            }
            else
                break;

        }

        /* cleanup handles no longer used */
        CloseHandle(hNotificationThread);
        CloseHandle(hNotificationThreadEvent);
        hNotificationThread = NULL;
        hNotificationThreadEvent = NULL;

        if (hLocalGPAppliedEvent != NULL)
        {
            CloseHandle(hLocalGPAppliedEvent);
            hLocalGPAppliedEvent = NULL;
        }
        if (hMachineGPAppliedEvent != NULL)
        {
            CloseHandle(hMachineGPAppliedEvent);
            hMachineGPAppliedEvent = NULL;
        }

        LeaveCriticalSection(&GPNotifyLock);

        if (MachineWatch.event != NULL)
        {
            CloseGPWatch(&MachineWatch);
            CloseGPWatch(&UserWatch);
        }

        /* dereference the library and exit */
        FreeLibraryAndExitThread(hModule,
                                 0);
    }
    else
    {
        ERR("Referencing the library failed!\n");
    }

    return 1;
}

static
HANDLE
CreateGPEvent(BOOL bMachine,
              PSECURITY_DESCRIPTOR lpSecurityDescriptor)
{
    HANDLE hEvent;
    SECURITY_ATTRIBUTES SecurityAttributes;

    SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    SecurityAttributes.lpSecurityDescriptor = lpSecurityDescriptor;
    SecurityAttributes.bInheritHandle = FALSE;

    hEvent = CreateEventW(&SecurityAttributes,
                          TRUE,
                          FALSE,
                          (bMachine ? szMachineGPApplied : szLocalGPApplied));

    return hEvent;
}

BOOL
WINAPI
RegisterGPNotification(HANDLE hEvent,
                       BOOL bMachine)
{
    PGP_NOTIFY Notify;
    PSECURITY_DESCRIPTOR lpSecurityDescriptor = NULL;
    BOOL Ret = FALSE;

    EnterCriticalSection(&GPNotifyLock);

    /* create the thread notification event */
    if (hNotificationThreadEvent == NULL)
    {
        hNotificationThreadEvent = CreateEventW(NULL,
                                               TRUE,
                                               FALSE,
                                               NULL);
        if (hNotificationThreadEvent == NULL)
        {
            goto Cleanup;
        }
    }

    /* create or open the machine group policy event */
    if (hMachineGPAppliedEvent == NULL)
    {
        lpSecurityDescriptor = CreateDefaultSecurityDescriptor();
        if (lpSecurityDescriptor == NULL)
        {
            goto Cleanup;
        }

        hMachineGPAppliedEvent = CreateGPEvent(TRUE,
                                               lpSecurityDescriptor);
        if (hMachineGPAppliedEvent == NULL)
        {
            goto Cleanup;
        }
    }

    /* create or open the local group policy event only if necessary */
    if (!bMachine && hLocalGPAppliedEvent == NULL)
    {
        if (lpSecurityDescriptor == NULL)
        {
            lpSecurityDescriptor = CreateDefaultSecurityDescriptor();
            if (lpSecurityDescriptor == NULL)
            {
                goto Cleanup;
            }
        }

        hLocalGPAppliedEvent = CreateGPEvent(FALSE,
                                             lpSecurityDescriptor);
        if (hLocalGPAppliedEvent == NULL)
        {
            goto Cleanup;
        }
    }

    if (hNotificationThread == NULL)
    {
        hNotificationThread = CreateThread(NULL,
                                           0,
                                           GPNotificationThreadProc,
                                           NULL,
                                           0,
                                           NULL);
    }

    if (hNotificationThread != NULL)
    {
        Notify = (PGP_NOTIFY)LocalAlloc(LMEM_FIXED,
                                        sizeof(GP_NOTIFY));
        if (Notify != NULL)
        {
            /* add the item to the beginning of the list */
            Notify->Next = NotificationList;
            Notify->hEvent = hEvent;
            Notify->bMachine = bMachine;

            NotificationList = Notify;

            /* notify the thread */
            GPNotificationAction = gpaUpdate;
            SetEvent(hNotificationThreadEvent);

            Ret = TRUE;
        }
    }

Cleanup:
    LeaveCriticalSection(&GPNotifyLock);

    if (lpSecurityDescriptor != NULL)
    {
        LocalFree((HLOCAL)lpSecurityDescriptor);
    }

    /* NOTE: don't delete the events or close the handles created */

    return Ret;
}

BOOL
WINAPI
UnregisterGPNotification(HANDLE hEvent)
{
    PGP_NOTIFY Notify = NULL, *NotifyLink;
    BOOL Ret = FALSE;

    EnterCriticalSection(&GPNotifyLock);

    Notify = NotificationList;
    NotifyLink = &NotificationList;

    while (Notify != NULL)
    {
        if (Notify->hEvent == hEvent)
        {
            /* remove and free the item */
            *NotifyLink = Notify->Next;
            LocalFree((HLOCAL)Notify);

            /* notify the thread */
            if (hNotificationThread != NULL &&
                hNotificationThreadEvent != NULL)
            {
                GPNotificationAction = gpaUpdate;
                SetEvent(hNotificationThreadEvent);
            }

            Ret = TRUE;
            break;
        }

        NotifyLink = &Notify->Next;
        Notify = Notify->Next;
    }

    LeaveCriticalSection(&GPNotifyLock);

    return Ret;
}

BOOL
WINAPI
RefreshPolicy(BOOL bMachine)
{
    HANDLE hEvent;
    BOOL Ret = TRUE;

    hEvent = OpenEventW(EVENT_MODIFY_STATE,
                        FALSE,
                        (bMachine ? szMachineGPRefreshEvent : szLocalGPRefreshEvent));
    if (hEvent != NULL)
    {
        Ret = SetEvent(hEvent);
        CloseHandle(hEvent);
    }

    /* return TRUE even if the mutex doesn't exist! */
    return Ret;
}

BOOL
WINAPI
RefreshPolicyEx(BOOL bMachine,
                DWORD dwOptions)
{
    if (dwOptions & ~RP_FORCE)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (dwOptions & RP_FORCE)
    {
        HANDLE hEvent;
        BOOL Ret = TRUE;

        hEvent = OpenEventW(EVENT_MODIFY_STATE,
                            FALSE,
                            (bMachine ? szMachineGPForceRefreshEvent : szLocalGPForceRefreshEvent));
        if (hEvent != NULL)
        {
            Ret = SetEvent(hEvent);
            CloseHandle(hEvent);
        }

        /* return TRUE even if the mutex doesn't exist! */
        return Ret;
    }
    else
    {
        return RefreshPolicy(bMachine);
    }
}

HANDLE
WINAPI
EnterCriticalPolicySection(BOOL bMachine)
{
    SECURITY_ATTRIBUTES SecurityAttributes;
    PSECURITY_DESCRIPTOR lpSecurityDescriptor;
    HANDLE hSection;

    /* create or open the mutex */
    lpSecurityDescriptor = CreateDefaultSecurityDescriptor();
    if (lpSecurityDescriptor != NULL)
    {
        SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        SecurityAttributes.lpSecurityDescriptor = lpSecurityDescriptor;
        SecurityAttributes.bInheritHandle = FALSE;

        hSection = CreateMutexW(&SecurityAttributes,
                                FALSE,
                                (bMachine ? szMachineGPMutex : szLocalGPMutex));

        LocalFree((HLOCAL)lpSecurityDescriptor);

        if (hSection != NULL)
        {
            /* wait up to 10 minutes */
            if (WaitForSingleObject(hSection,
                                    600000) != WAIT_FAILED)
            {
                return hSection;
            }

            CloseHandle(hSection);
        }
    }

    return NULL;
}

BOOL
WINAPI
LeaveCriticalPolicySection(HANDLE hSection)
{
    BOOL Ret;

    if (hSection == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Ret = ReleaseMutex(hSection);
    CloseHandle(hSection);

    return Ret;
}

BOOL
WINAPI
WaitForUserPolicyForegroundProcessing(void)
{
    HANDLE hEvent;
    BOOL Ret = FALSE;

    hEvent = OpenEventW(SYNCHRONIZE,
                        FALSE,
                        szLocalGPDoneEvent);
    if (hEvent != NULL)
    {
        Ret = WaitForSingleObject(hEvent,
                                  INFINITE) != WAIT_FAILED;
        CloseHandle(hEvent);
    }

    return Ret;
}

BOOL
WINAPI
WaitForMachinePolicyForegroundProcessing(void)
{
    HANDLE hEvent;
    BOOL Ret = FALSE;

    hEvent = OpenEventW(SYNCHRONIZE,
                        FALSE,
                        szMachineGPDoneEvent);
    if (hEvent != NULL)
    {
        Ret = WaitForSingleObject(hEvent,
                                  INFINITE) != WAIT_FAILED;
        CloseHandle(hEvent);
    }

    return Ret;
}

BOOL
WINAPI
DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        gp_hInstance = inst;
        InitializeGPNotifications();
        break;
    case DLL_PROCESS_DETACH:
        if (!reserved)
            UninitializeGPNotifications();
        break;
    }
    return TRUE;
}
