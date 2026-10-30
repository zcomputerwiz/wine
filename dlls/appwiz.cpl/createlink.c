/*
 * The Create Shortcut wizard (NewLinkHere), as Windows XP shows it for New > Shortcut
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
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

#define COBJMACROS

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "commctrl.h"
#include "shlobj.h"
#include "shellapi.h"
#include "shlwapi.h"
#include "prsht.h"

#include "appwiz.h"
#include "res.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appwizcpl);

struct link_wizard
{
    WCHAR folder[MAX_PATH];       /* where the shortcut goes */
    WCHAR placeholder[MAX_PATH];  /* the "New Shortcut.lnk" the New menu named, removed when it is empty */
    WCHAR target[MAX_PATH * 2];   /* the item, or an Internet address */
    WCHAR name[MAX_PATH];
    BOOL url;
    HBITMAP art;
};

static WCHAR *load_string(UINT id)
{
    static WCHAR buffer[4][512];
    static int next;
    WCHAR *s = buffer[next++ % ARRAY_SIZE(buffer)];

    if (!LoadStringW(hInst, id, s, ARRAY_SIZE(buffer[0]))) s[0] = 0;
    return s;
}

/* The picture at the left of the pages: Windows XP's from the genuine resource pack, else our own */
static HBITMAP load_art(void)
{
    WCHAR path[MAX_PATH];
    HMODULE pack;
    HBITMAP bitmap = NULL;

    GetWindowsDirectoryW(path, ARRAY_SIZE(path));
    PathAppendW(path, L"Resources\\Icons\\xpres_appwiz.dll");
    if ((pack = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE)))
    {
        bitmap = LoadImageW(pack, MAKEINTRESOURCEW(IDB_SHORTCUT), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        FreeLibrary(pack);
    }
    if (!bitmap) bitmap = LoadImageW(hInst, MAKEINTRESOURCEW(IDB_SHORTCUT), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    return bitmap;
}

static void message(HWND hwnd, UINT id, const WCHAR *arg)
{
    WCHAR text[1024];

    swprintf(text, ARRAY_SIZE(text), load_string(id), arg);
    MessageBoxW(hwnd, text, load_string(IDS_CREATE_SHORTCUT), MB_OK | MB_ICONEXCLAMATION);
}

static BOOL is_url(const WCHAR *text)
{
    return PathIsURLW(text) || !wcsnicmp(text, L"www.", 4) || wcsstr(text, L"://");
}

/* Next: the item must exist (or be found on the path), or be an Internet address */
static BOOL check_location(HWND hwnd, struct link_wizard *wizard)
{
    WCHAR text[MAX_PATH * 2], expanded[MAX_PATH * 2], found[MAX_PATH];
    SHFILEINFOW info;
    WCHAR *start = text, *end;

    GetDlgItemTextW(hwnd, IDC_LINK_LOCATION, text, ARRAY_SIZE(text));
    while (*start == ' ') start++;
    if (*start == '"' && (end = wcschr(start + 1, '"'))) { *end = 0; start++; }
    for (end = start + wcslen(start); end > start && end[-1] == ' '; end--) *(end - 1) = 0;
    if (!*start) return FALSE;

    ExpandEnvironmentStringsW(start, expanded, ARRAY_SIZE(expanded));
    wizard->url = FALSE;
    if (PathFileExistsW(expanded))
    {
        if (!GetFullPathNameW(expanded, ARRAY_SIZE(wizard->target), wizard->target, NULL))
            lstrcpynW(wizard->target, expanded, ARRAY_SIZE(wizard->target));
    }
    else if (SearchPathW(NULL, expanded, L".exe", ARRAY_SIZE(found), found, NULL))
        lstrcpyW(wizard->target, found);
    else if (is_url(expanded))
    {
        wizard->url = TRUE;
        if (!wcsnicmp(expanded, L"www.", 4)) swprintf(wizard->target, ARRAY_SIZE(wizard->target), L"http://%s", expanded);
        else lstrcpynW(wizard->target, expanded, ARRAY_SIZE(wizard->target));
    }
    else
    {
        message(hwnd, IDS_LINK_NOT_FOUND, start);
        return FALSE;
    }

    /* the name the item is shown with, without an extension that is hidden */
    if (wizard->url) lstrcpyW(wizard->name, load_string(IDS_NEW_INTERNET_SHORTCUT));
    else if (SHGetFileInfoW(wizard->target, 0, &info, sizeof(info), SHGFI_DISPLAYNAME) && info.szDisplayName[0])
        lstrcpynW(wizard->name, info.szDisplayName, ARRAY_SIZE(wizard->name));
    else
    {
        lstrcpynW(wizard->name, PathFindFileNameW(wizard->target), ARRAY_SIZE(wizard->name));
        PathRemoveExtensionW(wizard->name);
    }
    return TRUE;
}

static void set_buttons(HWND page, DWORD buttons)
{
    PostMessageW(GetParent(page), PSM_SETWIZBUTTONS, 0, buttons);
}

/* Windows XP's picture touches the window's edges: the page moves to the corner of the sheet, its other controls
 * stay where they were */
static void art_to_corner(HWND page)
{
    RECT rc;
    POINT corner = { 0, 0 };
    HWND child;

    GetWindowRect(page, &rc);
    MapWindowPoints(NULL, GetParent(page), (POINT *)&rc, 2);
    if (!rc.left && !rc.top) return;
    {
        /* tall enough for the whole picture */
        BITMAP bm = { 0 };
        HBITMAP art = (HBITMAP)SendDlgItemMessageW(page, IDC_LINK_ART, STM_GETIMAGE, IMAGE_BITMAP, 0);

        if (art) GetObjectW(art, sizeof(bm), &bm);
        SetWindowPos(page, NULL, 0, 0, rc.right, max(rc.bottom, bm.bmHeight), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        RECT pos;

        if (GetDlgCtrlID(child) == IDC_LINK_ART) continue;
        GetWindowRect(child, &pos);
        MapWindowPoints(NULL, page, (POINT *)&pos, 2);
        SetWindowPos(child, NULL, pos.left + rc.left, pos.top + rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowPos(GetDlgItem(page, IDC_LINK_ART), NULL, corner.x, corner.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* the wizard opens in the middle of the screen */
static int CALLBACK sheet_callback(HWND hwnd, UINT msg, LPARAM lparam)
{
    if (msg == PSCB_INITIALIZED)
    {
        RECT rc, area;

        GetWindowRect(hwnd, &rc);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        SetWindowPos(hwnd, NULL, area.left + (area.right - area.left - (rc.right - rc.left)) / 2,
                     area.top + (area.bottom - area.top - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    return 0;
}

static void set_art(HWND hwnd, struct link_wizard *wizard)
{
    SendDlgItemMessageW(hwnd, IDC_LINK_ART, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)wizard->art);
}

static INT_PTR CALLBACK location_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct link_wizard *wizard = (struct link_wizard *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
        wizard = (struct link_wizard *)((PROPSHEETPAGEW *)lparam)->lParam;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)wizard);
        set_art(hwnd, wizard);
        SHAutoComplete(GetDlgItem(hwnd, IDC_LINK_LOCATION), SHACF_FILESYSTEM | SHACF_URLALL);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_LINK_LOCATION && HIWORD(wparam) == EN_CHANGE)
            set_buttons(hwnd,
                                    GetWindowTextLengthW(GetDlgItem(hwnd, IDC_LINK_LOCATION)) ? PSWIZB_NEXT : 0);
        else if (LOWORD(wparam) == IDC_LINK_BROWSE)
        {
            BROWSEINFOW bi = { hwnd };
            WCHAR path[MAX_PATH];
            LPITEMIDLIST pidl;

            bi.lpszTitle = load_string(IDS_LINK_BROWSE_TITLE);
            bi.ulFlags = BIF_BROWSEINCLUDEFILES | BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
            if ((pidl = SHBrowseForFolderW(&bi)))
            {
                if (SHGetPathFromIDListW(pidl, path)) SetDlgItemTextW(hwnd, IDC_LINK_LOCATION, path);
                ILFree(pidl);
            }
        }
        break;

    case WM_NOTIFY:
        switch (((NMHDR *)lparam)->code)
        {
        case PSN_SETACTIVE:
            art_to_corner(hwnd);
            set_buttons(hwnd,
                                    GetWindowTextLengthW(GetDlgItem(hwnd, IDC_LINK_LOCATION)) ? PSWIZB_NEXT : 0);
            SetWindowTextW(GetParent(hwnd), load_string(IDS_CREATE_SHORTCUT));
            return TRUE;
        case PSN_WIZNEXT:
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, check_location(hwnd, wizard) ? 0 : -1);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static BOOL write_link(struct link_wizard *wizard, const WCHAR *path)
{
    IShellLinkW *link;
    IPersistFile *file;
    WCHAR dir[MAX_PATH];
    HRESULT hr;

    if (wizard->url) return WritePrivateProfileStringW(L"InternetShortcut", L"URL", wizard->target, path);

    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)))
        return FALSE;
    hr = IShellLinkW_SetPath(link, wizard->target);
    /* a program starts in its folder */
    lstrcpynW(dir, wizard->target, ARRAY_SIZE(dir));
    if (!PathIsDirectoryW(dir) && PathRemoveFileSpecW(dir)) IShellLinkW_SetWorkingDirectory(link, dir);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)))
    {
        hr = IPersistFile_Save(file, path, TRUE);
        IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
    return SUCCEEDED(hr);
}

/* Finish: "name.lnk" in the folder, "name (2).lnk" and so on when it is taken */
static BOOL create_link(HWND hwnd, struct link_wizard *wizard)
{
    WCHAR name[MAX_PATH], path[MAX_PATH];
    const WCHAR *ext = wizard->url ? L".url" : L".lnk";
    UINT i;

    GetDlgItemTextW(hwnd, IDC_LINK_NAME, name, ARRAY_SIZE(name));
    StrTrimW(name, L" ");
    if (!name[0]) return FALSE;
    if (wcspbrk(name, L"\\/:*?\"<>|"))
    {
        MessageBoxW(hwnd, load_string(IDS_LINK_BAD_NAME), load_string(IDS_CREATE_SHORTCUT), MB_OK | MB_ICONEXCLAMATION);
        return FALSE;
    }

    /* the empty file the New menu may have made in the folder goes away */
    if (wizard->placeholder[0])
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(wizard->placeholder, GetFileExInfoStandard, &data) && !data.nFileSizeLow && !data.nFileSizeHigh)
            DeleteFileW(wizard->placeholder);
    }

    for (i = 1; i < 1000; i++)
    {
        if (i == 1) swprintf(path, ARRAY_SIZE(path), L"%s\\%s%s", wizard->folder, name, ext);
        else swprintf(path, ARRAY_SIZE(path), L"%s\\%s (%u)%s", wizard->folder, name, i, ext);
        if (!PathFileExistsW(path)) break;
    }
    if (!write_link(wizard, path))
    {
        message(hwnd, IDS_LINK_FAILED, path);
        return FALSE;
    }
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, path, NULL);
    return TRUE;
}

static INT_PTR CALLBACK name_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct link_wizard *wizard = (struct link_wizard *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
        wizard = (struct link_wizard *)((PROPSHEETPAGEW *)lparam)->lParam;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)wizard);
        set_art(hwnd, wizard);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_LINK_NAME && HIWORD(wparam) == EN_CHANGE)
            set_buttons(hwnd, PSWIZB_BACK |
                                    (GetWindowTextLengthW(GetDlgItem(hwnd, IDC_LINK_NAME)) ? PSWIZB_FINISH : PSWIZB_DISABLEDFINISH));
        break;

    case WM_NOTIFY:
        switch (((NMHDR *)lparam)->code)
        {
        case PSN_SETACTIVE:
            art_to_corner(hwnd);
            SetDlgItemTextW(hwnd, IDC_LINK_NAME, wizard->name);
            SendDlgItemMessageW(hwnd, IDC_LINK_NAME, EM_SETSEL, 0, -1);
            set_buttons(hwnd, PSWIZB_BACK | PSWIZB_FINISH);
            SetWindowTextW(GetParent(hwnd), load_string(IDS_LINK_TITLE_PAGE));
            return TRUE;
        case PSN_WIZFINISH:
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, create_link(hwnd, wizard) ? 0 : 1);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void show_wizard(HWND parent, const WCHAR *where)
{
    struct link_wizard wizard = { { 0 } };
    PROPSHEETPAGEW pages[2] = { { sizeof(PROPSHEETPAGEW) }, { sizeof(PROPSHEETPAGEW) } };
    PROPSHEETHEADERW header = { sizeof(header) };
    WCHAR path[MAX_PATH];
    const WCHAR *p = where;
    size_t len;

    /* the folder, or the new item the New menu would make in it */
    while (*p == ' ') p++;
    lstrcpynW(path, *p == '"' ? p + 1 : p, ARRAY_SIZE(path));
    len = wcslen(path);
    while (len && (path[len - 1] == '"' || path[len - 1] == ' ')) path[--len] = 0;
    if (!path[0]) GetCurrentDirectoryW(ARRAY_SIZE(path), path);
    if (PathIsDirectoryW(path)) lstrcpyW(wizard.folder, path);
    else
    {
        lstrcpyW(wizard.placeholder, path);
        lstrcpyW(wizard.folder, path);
        PathRemoveFileSpecW(wizard.folder);
    }
    TRACE("folder %s, placeholder %s\n", debugstr_w(wizard.folder), debugstr_w(wizard.placeholder));

    wizard.art = load_art();
    pages[0].hInstance = pages[1].hInstance = hInst;
    pages[0].pszTemplate = MAKEINTRESOURCEW(IDD_LINK_LOCATION);
    pages[0].pfnDlgProc = location_proc;
    pages[0].lParam = (LPARAM)&wizard;
    pages[1].pszTemplate = MAKEINTRESOURCEW(IDD_LINK_NAME);
    pages[1].pfnDlgProc = name_proc;
    pages[1].lParam = (LPARAM)&wizard;

    header.dwFlags = PSH_PROPSHEETPAGE | PSH_WIZARD | PSH_NOAPPLYNOW | PSH_USECALLBACK;
    header.pfnCallback = sheet_callback;
    header.hwndParent = parent;
    header.hInstance = hInst;
    header.pszCaption = load_string(IDS_CREATE_SHORTCUT);
    header.nPages = ARRAY_SIZE(pages);
    header.ppsp = pages;
    PropertySheetW(&header);
    if (wizard.art) DeleteObject(wizard.art);
}

/***********************************************************************
 *             NewLinkHereW  (appwiz.cpl.@)
 *
 * rundll32 appwiz.cpl,NewLinkHere <folder or new shortcut's path>: the HKCR\.lnk\ShellNew command.
 */
void WINAPI NewLinkHereW(HWND hwnd, HINSTANCE instance, WCHAR *cmdline, int show)
{
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };

    InitCommonControlsEx(&icc);
    show_wizard(hwnd, cmdline ? cmdline : L"");
    if (SUCCEEDED(init)) CoUninitialize();
}

/***********************************************************************
 *             NewLinkHereA  (appwiz.cpl.@)
 */
void WINAPI NewLinkHereA(HWND hwnd, HINSTANCE instance, char *cmdline, int show)
{
    WCHAR path[MAX_PATH] = { 0 };

    if (cmdline) MultiByteToWideChar(CP_ACP, 0, cmdline, -1, path, ARRAY_SIZE(path));
    NewLinkHereW(hwnd, instance, path, show);
}
