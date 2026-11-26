/*
 * Task Manager: the Users page
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
 * XP's Users page lists the sessions of the computer. Under Wine each wineserver is one session, so the page shows
 * the one this Task Manager runs in: its user, its id, Active and the Console. Log Off works (the Shut Down menu's
 * way); Disconnect, Connect, Send Message and Remote Control need other sessions and are grayed.
 */

#include <stdio.h>
#include <stdlib.h>

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "taskmgr.h"

HWND hUsersPage;

static HWND hUserList, hDisconnectButton, hLogoffButton, hSendMessageButton;
static int nPageWidth, nPageHeight;
static int column_map[USER_COLUMN_COUNT], column_count;

enum { USERCOL_USER, USERCOL_ID, USERCOL_STATUS, USERCOL_CLIENT, USERCOL_SESSION };

void UsersPage_DefaultSettings(void)
{
    static const int widths[USER_COLUMN_COUNT] = { 120, 33, 95, 101, 90 };
    int i;

    TaskManagerSettings.UsersShowFullName = FALSE;
    for (i = 0; i < USER_COLUMN_COUNT; i++)
    {
        TaskManagerSettings.UserColumns[i] = TRUE;
        TaskManagerSettings.UserColumnWidths[i] = widths[i];
    }
}

static void add_columns(void)
{
    WCHAR text[64];
    LVCOLUMNW column;
    int i;

    while (SendMessageW(hUserList, LVM_DELETECOLUMN, 0, 0));
    column_count = 0;
    for (i = 0; i < USER_COLUMN_COUNT; i++)
    {
        if (i != USERCOL_USER && !TaskManagerSettings.UserColumns[i]) continue;
        LoadStringW(hInst, IDS_USER_COL_FIRST + i, text, ARRAY_SIZE(text));
        column.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
        column.fmt = i == USERCOL_ID ? LVCFMT_RIGHT : LVCFMT_LEFT;
        column.cx = max(TaskManagerSettings.UserColumnWidths[i], TaskManager_HeaderWidth(hUserList, text));
        column.pszText = text;
        SendMessageW(hUserList, LVM_INSERTCOLUMNW, column_count, (LPARAM)&column);
        column_map[column_count++] = i;
    }
}

static void save_column_widths(void)
{
    int i;

    for (i = 0; i < column_count; i++)
        TaskManagerSettings.UserColumnWidths[column_map[i]] = SendMessageW(hUserList, LVM_GETCOLUMNWIDTH, i, 0);
}

static void get_user(WCHAR *buf, DWORD size)
{
    WCHAR name[256], domain[256];
    DWORD name_size = ARRAY_SIZE(name), domain_size = ARRAY_SIZE(domain), len;
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)buffer;
    SID_NAME_USE use;
    HANDLE token;

    buf[0] = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    if (GetTokenInformation(token, TokenUser, user, sizeof(buffer), &len) &&
        LookupAccountSidW(NULL, user->User.Sid, name, &name_size, domain, &domain_size, &use))
    {
        if (TaskManagerSettings.UsersShowFullName && domain[0])
            swprintf(buf, size, L"%s\\%s", domain, name);
        else
            lstrcpynW(buf, name, size);
    }
    CloseHandle(token);
}

void RefreshUsersPage(void)
{
    WCHAR text[512];
    DWORD session = 0;
    LVITEMW item;
    int c;

    if (!hUsersPage) return;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);

    if (!SendMessageW(hUserList, LVM_GETITEMCOUNT, 0, 0))
    {
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.iSubItem = 0;
        item.pszText = (WCHAR *)L"";
        SendMessageW(hUserList, LVM_INSERTITEMW, 0, (LPARAM)&item);
    }
    for (c = 0; c < column_count; c++)
    {
        text[0] = 0;
        switch (column_map[c])
        {
        case USERCOL_USER:    get_user(text, ARRAY_SIZE(text)); break;
        case USERCOL_ID:      swprintf(text, ARRAY_SIZE(text), L"%lu", session); break;
        case USERCOL_STATUS:  LoadStringW(hInst, IDS_USERS_ACTIVE, text, ARRAY_SIZE(text)); break;
        case USERCOL_CLIENT:  break;
        case USERCOL_SESSION: LoadStringW(hInst, IDS_USERS_CONSOLE, text, ARRAY_SIZE(text)); break;
        }
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.iSubItem = c;
        item.pszText = text;
        SendMessageW(hUserList, LVM_SETITEMTEXTW, 0, (LPARAM)&item);
    }
}

static BOOL user_selected(void)
{
    return SendMessageW(hUserList, LVM_GETSELECTEDCOUNT, 0, 0) > 0;
}

static void update_buttons(void)
{
    /* one session: nothing to disconnect from or to send a message to */
    EnableWindow(hDisconnectButton, FALSE);
    EnableWindow(hSendMessageButton, FALSE);
    EnableWindow(hLogoffButton, SendMessageW(hUserList, LVM_GETITEMCOUNT, 0, 0) > 0);  /* XP: the one session is the target */
}

static void on_logoff(void)
{
    WCHAR title[128], text[256];

    if (!SendMessageW(hUserList, LVM_GETITEMCOUNT, 0, 0)) return;
    LoadStringW(hInst, IDS_APP_TITLE, title, ARRAY_SIZE(title));
    LoadStringW(hInst, IDS_USERS_LOGOFF_CONFIRM, text, ARRAY_SIZE(text));
    if (MessageBoxW(hMainWnd, text, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    ShutDown_OnCommand(ID_SHUTDOWN_LOGOFF);
}

static void show_context_menu(void)
{
    HMENU menu = LoadMenuW(hInst, MAKEINTRESOURCEW(IDR_USERS_PAGE_CONTEXT)), popup = GetSubMenu(menu, 0);
    POINT pt;

    GetCursorPos(&pt);
    SetMenuDefaultItem(popup, ID_USERS_SENDMESSAGE, FALSE);
    TrackPopupMenu(popup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hMainWnd, NULL);
    DestroyMenu(menu);
}

/* ---- Select Columns ---- */

static INT_PTR CALLBACK columns_dlg_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    int i;

    switch (msg)
    {
    case WM_INITDIALOG:
        for (i = 0; i < USER_COLUMN_COUNT; i++)
            CheckDlgButton(hDlg, IDC_USERCOL_FIRST + i,
                           !i || TaskManagerSettings.UserColumns[i] ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            save_column_widths();
            for (i = 0; i < USER_COLUMN_COUNT; i++)
                TaskManagerSettings.UserColumns[i] = IsDlgButtonChecked(hDlg, IDC_USERCOL_FIRST + i) == BST_CHECKED;
            EndDialog(hDlg, IDOK);
        }
        else if (LOWORD(wParam) == IDCANCEL) EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* ---- menus ---- */

void UsersPage_InitMenus(HMENU hOptionsMenu, HMENU hViewMenu)
{
    WCHAR text[64];

    AppendMenuW(hOptionsMenu, MF_SEPARATOR, 0, NULL);
    LoadStringW(hInst, IDS_OPT_USERS_FULLNAME, text, ARRAY_SIZE(text));
    AppendMenuW(hOptionsMenu, MF_STRING | (TaskManagerSettings.UsersShowFullName ? MF_CHECKED : 0),
                ID_OPTIONS_USERS_FULLNAME, text);
    LoadStringW(hInst, IDS_VIEW_SELECTCOLUMNS, text, ARRAY_SIZE(text));
    AppendMenuW(hViewMenu, MF_STRING, ID_VIEW_SELECTCOLUMNS, text);
}

void UsersPage_OnCommand(UINT id)
{
    switch (id)
    {
    case ID_OPTIONS_USERS_FULLNAME:
        TaskManagerSettings.UsersShowFullName = !TaskManagerSettings.UsersShowFullName;
        CheckMenuItem(GetMenu(hMainWnd), id,
                      MF_BYCOMMAND | (TaskManagerSettings.UsersShowFullName ? MF_CHECKED : MF_UNCHECKED));
        RefreshUsersPage();
        break;
    case ID_VIEW_SELECTCOLUMNS:
        if (DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_USER_COLUMNS_DIALOG), hMainWnd, columns_dlg_proc) == IDOK)
        {
            add_columns();
            RefreshUsersPage();
        }
        break;
    case ID_USERS_LOGOFF:
        on_logoff();
        break;
    }
}

/* ---- the page ---- */

static void move_control(HWND hDlg, HWND ctl, int dx, int dy, int dcx, int dcy)
{
    RECT rc;

    GetWindowRect(ctl, &rc);
    MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
    SetWindowPos(ctl, NULL, rc.left + dx, rc.top + dy, rc.right - rc.left + dcx, rc.bottom - rc.top + dcy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(ctl, NULL, TRUE);
}

INT_PTR CALLBACK UsersPageWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    RECT rc;
    int dx, dy;

    switch (message)
    {
    case WM_INITDIALOG:
    {
        HIMAGELIST images;
        HICON icon;

        hUsersPage = hDlg;
        GetClientRect(hDlg, &rc);
        nPageWidth = rc.right;
        nPageHeight = rc.bottom;
        SetWindowPos(hDlg, NULL, 15, 30, 0, 0, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSIZE | SWP_NOZORDER);

        hUserList = GetDlgItem(hDlg, IDC_USERLIST);
        hDisconnectButton = GetDlgItem(hDlg, IDC_DISCONNECT);
        hLogoffButton = GetDlgItem(hDlg, IDC_LOGOFF);
        hSendMessageButton = GetDlgItem(hDlg, IDC_SENDMESSAGE);
        SendMessageW(hUserList, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);

        /* a picture in front of the user's name: the shell's users icon (shell32 IDI_SHELL_USERS) */
        images = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                  ILC_COLOR32 | ILC_MASK, 1, 1);
        if (ExtractIconExW(L"shell32.dll", -220, NULL, &icon, 1) == 1 && icon)
        {
            ImageList_AddIcon(images, icon);
            DestroyIcon(icon);
            SendMessageW(hUserList, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)images);
        }
        else ImageList_Destroy(images);

        add_columns();
        RefreshUsersPage();
        update_buttons();
        return TRUE;
    }

    case WM_DESTROY:
        save_column_widths();
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_LOGOFF: on_logoff(); break;
        }
        break;

    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lParam;

        if (hdr->hwndFrom != hUserList) break;
        if (hdr->code == LVN_ITEMCHANGED) update_buttons();
        else if (hdr->code == NM_RCLICK && user_selected()) show_context_menu();
        break;
    }

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        dx = LOWORD(lParam) - nPageWidth;
        dy = HIWORD(lParam) - nPageHeight;
        nPageWidth = LOWORD(lParam);
        nPageHeight = HIWORD(lParam);
        move_control(hDlg, hUserList, 0, 0, dx, dy);
        move_control(hDlg, hDisconnectButton, dx, dy, 0, 0);
        move_control(hDlg, hLogoffButton, dx, dy, 0, 0);
        move_control(hDlg, hSendMessageButton, dx, dy, 0, 0);
        break;
    }
    return 0;
}
