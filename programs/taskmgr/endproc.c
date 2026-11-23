/*
 *  ReactOS Task Manager
 *
 *  endproc.c
 *
 *  Copyright (C) 1999 - 2001  Brian Palmer  <brianp@reactos.org>
 *  Copyright (C) 2008  Vladimir Pankratov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <stdlib.h>

#include <windows.h>
#include <commctrl.h>
#include <winnt.h>
#include <tlhelp32.h>

#include "taskmgr.h"
#include "perfdata.h"

static WCHAR wszWarnMsg[511];
static WCHAR wszWarnTitle[255];
static WCHAR wszUnable2Terminate[255];

static void load_message_strings(void)
{
    LoadStringW(hInst, IDS_TERMINATE_MESSAGE, wszWarnMsg, ARRAY_SIZE(wszWarnMsg));
    LoadStringW(hInst, IDS_TERMINATE_UNABLE2TERMINATE, wszUnable2Terminate, ARRAY_SIZE(wszUnable2Terminate));
    LoadStringW(hInst, IDS_WARNING_TITLE, wszWarnTitle, ARRAY_SIZE(wszWarnTitle));
}

void ProcessPage_OnEndProcess(void)
{
    LVITEMW          lvitem;
    ULONG            Index, Count;
    DWORD            dwProcessId;
    HANDLE           hProcess;
    WCHAR            wstrErrorText[256];

    load_message_strings();

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETITEMCOUNT, 0, 0);
    for (Index=0; Index<Count; Index++)
    {
        lvitem.mask = LVIF_STATE;
        lvitem.stateMask = LVIS_SELECTED;
        lvitem.iItem = Index;
        lvitem.iSubItem = 0;

        SendMessageW(hProcessPageListCtrl, LVM_GETITEMW, 0, (LPARAM) &lvitem);

        if (lvitem.state & LVIS_SELECTED)
            break;
    }

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETSELECTEDCOUNT, 0, 0);
    dwProcessId = PerfDataGetProcessId(Index);
    if ((Count != 1) || (dwProcessId == 0))
        return;

    if (MessageBoxW(hMainWnd, wszWarnMsg, wszWarnTitle, MB_YESNO|MB_ICONWARNING) != IDYES)
        return;

    hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, dwProcessId);

    if (!hProcess)
    {
        GetLastErrorText(wstrErrorText, ARRAY_SIZE(wstrErrorText));
        MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
        return;
    }

    if (!TerminateProcess(hProcess, 1))
    {
        GetLastErrorText(wstrErrorText, ARRAY_SIZE(wstrErrorText));
        MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
    }

    CloseHandle(hProcess);
}

/* End the descendants of a process, the deepest first, as XP's End Process Tree does */
static void terminate_children(DWORD parent, int depth)
{
    PROCESSENTRY32W entry = { sizeof(entry) };
    HANDLE snapshot, process;
    DWORD children[256];
    int count = 0, i;

    if (depth > 32) return;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ParentProcessID == parent && entry.th32ProcessID != parent &&
                entry.th32ProcessID != GetCurrentProcessId() && count < ARRAY_SIZE(children))
                children[count++] = entry.th32ProcessID;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    for (i = 0; i < count; i++)
    {
        terminate_children(children[i], depth + 1);
        if ((process = OpenProcess(PROCESS_TERMINATE, FALSE, children[i])))
        {
            TerminateProcess(process, 1);
            CloseHandle(process);
        }
    }
}

void ProcessPage_OnEndProcessTree(void)
{
    LVITEMW          lvitem;
    ULONG            Index, Count;
    DWORD            dwProcessId;
    HANDLE           hProcess;
    WCHAR            wstrErrorText[256];

    load_message_strings();

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETITEMCOUNT, 0, 0);
    for (Index=0; Index<Count; Index++)
    {
        lvitem.mask = LVIF_STATE;
        lvitem.stateMask = LVIS_SELECTED;
        lvitem.iItem = Index;
        lvitem.iSubItem = 0;

        SendMessageW(hProcessPageListCtrl, LVM_GETITEMW, 0, (LPARAM) &lvitem);

        if (lvitem.state & LVIS_SELECTED)
            break;
    }

    Count = SendMessageW(hProcessPageListCtrl, LVM_GETSELECTEDCOUNT, 0, 0);
    dwProcessId = PerfDataGetProcessId(Index);
    if ((Count != 1) || (dwProcessId == 0))
        return;

    if (MessageBoxW(hMainWnd, wszWarnMsg, wszWarnTitle, MB_YESNO|MB_ICONWARNING) != IDYES)
        return;

    terminate_children(dwProcessId, 0);
    hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, dwProcessId);

    if (!hProcess)
    {
        GetLastErrorText(wstrErrorText, ARRAY_SIZE(wstrErrorText));
        MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
        return;
    }

    if (!TerminateProcess(hProcess, 1))
    {
        GetLastErrorText(wstrErrorText, ARRAY_SIZE(wstrErrorText));
        MessageBoxW(hMainWnd, wstrErrorText,wszUnable2Terminate, MB_OK|MB_ICONSTOP);
    }

    CloseHandle(hProcess);
}
