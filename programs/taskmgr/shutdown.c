/*
 * Task Manager: the Shut Down menu
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * XP's Shut Down menu: Stand By, Hibernate, Turn Off, Restart, Log Off <user> and Switch User (or Lock Computer
 * where users cannot be switched). The actions take the Start menu's way: msgina.dll's power_mgmt_execute_action
 * (it asks the programs whether the session may end, plays the sound and asks the host through wexhost.dll), and
 * wexhost.dll itself says which of them this computer can do; what it cannot do is grayed. Without those DLLs
 * (stock Wine) Turn Off, Restart and Log Off go through ExitWindowsEx and the others are grayed. XP's Disconnect
 * and Eject Computer appear only in a remote session or on a docked laptop, so they are not offered.
 */

#include <stdio.h>
#include <windows.h>
#include <lmcons.h>
#include <commctrl.h>

#include "taskmgr.h"

/* wexhost.h */
#define WEX_POWER_SHUTDOWN   0
#define WEX_POWER_REBOOT     1
#define WEX_POWER_SUSPEND    2
#define WEX_POWER_HIBERNATE  3
#define WEX_POWER_LOGOFF     4
#define WEX_POWER_SWITCHUSER 5
#define WEX_POWER_LOCK       6
#define WEX_POWER_NO         0

/* msgina's wexpower.h */
#define POWER_ACTION_SHUTDOWN   0
#define POWER_ACTION_RESTART    1
#define POWER_ACTION_LOGOFF     2
#define POWER_ACTION_STANDBY    3
#define POWER_ACTION_SWITCHUSER 4
#define POWER_ACTION_LOCK       5

typedef DWORD (WINAPI *WEXPOWERCAN)(DWORD action, DWORD *result);
typedef DWORD (WINAPI *WEXPOWERDO)(HWND owner, DWORD action);
typedef void (__cdecl *POWERMGMTEXECUTE)(int action);

static HMODULE wexhost, msgina;
static WEXPOWERCAN pWexPowerCan;
static WEXPOWERDO pWexPowerDo;
static POWERMGMTEXECUTE pPowerMgmtExecute;

static void load_power_dlls(void)
{
    static BOOL loaded;

    if (loaded) return;
    loaded = TRUE;
    if ((wexhost = LoadLibraryExW(L"wexhost.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)))
    {
        pWexPowerCan = (WEXPOWERCAN)GetProcAddress(wexhost, "WexPowerCan");
        pWexPowerDo = (WEXPOWERDO)GetProcAddress(wexhost, "WexPowerDo");
    }
    if ((msgina = LoadLibraryExW(L"msgina.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)))
        pPowerMgmtExecute = (POWERMGMTEXECUTE)GetProcAddress(msgina, "power_mgmt_execute_action");
}

static BOOL host_can(DWORD action)
{
    DWORD can = WEX_POWER_NO;

    if (!pWexPowerCan) return FALSE;
    return !pWexPowerCan(action, &can) && can != WEX_POWER_NO;
}

static BOOL can_switch_user(void)
{
    return pPowerMgmtExecute && host_can(WEX_POWER_SWITCHUSER);
}

static void append_item(HMENU menu, UINT id, UINT string, BOOL enabled)
{
    WCHAR text[64];

    LoadStringW(hInst, string, text, ARRAY_SIZE(text));
    AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), id, text);
}

/* Fill the Shut Down menu for the current state; called each time before it opens. */
void ShutDown_InitMenu(HMENU hShutdownMenu)
{
    WCHAR format[64], user[UNLEN + 1], text[64 + UNLEN];
    DWORD size = ARRAY_SIZE(user);
    BOOL full;

    load_power_dlls();
    full = pPowerMgmtExecute != NULL;

    while (GetMenuItemCount(hShutdownMenu) > 0)
        DeleteMenu(hShutdownMenu, 0, MF_BYPOSITION);

    append_item(hShutdownMenu, ID_SHUTDOWN_STANDBY, IDS_SHUTDOWN_STANDBY, full && host_can(WEX_POWER_SUSPEND));
    append_item(hShutdownMenu, ID_SHUTDOWN_HIBERNATE, IDS_SHUTDOWN_HIBERNATE,
                pWexPowerDo && host_can(WEX_POWER_HIBERNATE));
    append_item(hShutdownMenu, ID_SHUTDOWN_POWEROFF, IDS_SHUTDOWN_POWEROFF, TRUE);
    append_item(hShutdownMenu, ID_SHUTDOWN_REBOOT, IDS_SHUTDOWN_REBOOT, TRUE);

    /* "Log Off <user>" */
    LoadStringW(hInst, IDS_SHUTDOWN_LOGOFF, format, ARRAY_SIZE(format));
    if (!GetUserNameW(user, &size)) user[0] = 0;
    swprintf(text, ARRAY_SIZE(text), format, user);
    AppendMenuW(hShutdownMenu, MF_STRING, ID_SHUTDOWN_LOGOFF, text);

    /* XP shows Switch User where users can be switched, otherwise Lock Computer */
    if (can_switch_user())
        append_item(hShutdownMenu, ID_SHUTDOWN_SWITCHUSER, IDS_SHUTDOWN_SWITCHUSER, TRUE);
    else
        append_item(hShutdownMenu, ID_SHUTDOWN_LOCKCOMPUTER, IDS_SHUTDOWN_LOCKCOMPUTER,
                    full && host_can(WEX_POWER_LOCK));
}

void ShutDown_OnCommand(UINT id)
{
    load_power_dlls();

    switch (id)
    {
    case ID_SHUTDOWN_STANDBY:
        if (pPowerMgmtExecute) pPowerMgmtExecute(POWER_ACTION_STANDBY);
        break;
    case ID_SHUTDOWN_HIBERNATE:
        if (pWexPowerDo) pWexPowerDo(hMainWnd, WEX_POWER_HIBERNATE);
        break;
    case ID_SHUTDOWN_POWEROFF:
        if (pPowerMgmtExecute) pPowerMgmtExecute(POWER_ACTION_SHUTDOWN);
        else ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        break;
    case ID_SHUTDOWN_REBOOT:
        if (pPowerMgmtExecute) pPowerMgmtExecute(POWER_ACTION_RESTART);
        else ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
        break;
    case ID_SHUTDOWN_LOGOFF:
        if (pPowerMgmtExecute) pPowerMgmtExecute(POWER_ACTION_LOGOFF);
        else ExitWindowsEx(EWX_LOGOFF, 0);
        break;
    case ID_SHUTDOWN_SWITCHUSER:
        if (pPowerMgmtExecute) pPowerMgmtExecute(POWER_ACTION_SWITCHUSER);
        break;
    case ID_SHUTDOWN_LOCKCOMPUTER:
        if (pPowerMgmtExecute) pPowerMgmtExecute(POWER_ACTION_LOCK);
        break;
    }
}
