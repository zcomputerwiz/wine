/*
 * Open With: the program list, the dialog and the item menu's submenu
 *
 * Based on the ReactOS Open With context menu extension (dll/win32/shell32/COpenWithMenu.cpp)
 * Copyright 2007 Johannes Anderwald <johannes.anderwald@reactos.org>
 * Copyright 2009 Andrew Hill
 * Copyright 2012 Rafal Harabien
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

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winuser.h"
#include "commctrl.h"
#include "commdlg.h"
#include "shlobj.h"
#include "shlwapi.h"

#include "shell32_main.h"
#include "shresdef.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

BOOL WINAPI PathIsExeAW(LPCVOID path);

static const WCHAR file_exts_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\";

/* The programs that can open a file: HKCR\Applications, with those the file type names (its ProgID's open
 * command, OpenWithList, OpenWithProgids, the user's recently used list) marked as recommended. */
struct open_with_app
{
    WCHAR file[MAX_PATH];     /* "notepad.exe": the key under HKCR\Applications */
    WCHAR name[MAX_PATH];     /* its file description */
    WCHAR cmd[MAX_PATH * 2];  /* open command, "%1" for the file */
    BOOL recommended, hidden, recent;
    HICON icon;
};

struct open_with_list
{
    struct open_with_app *apps;
    UINT count, recommended;
};

static void list_free(struct open_with_list *list)
{
    UINT i;

    for (i = 0; i < list->count; i++)
        if (list->apps[i].icon) DestroyIcon(list->apps[i].icon);
    free(list->apps);
    memset(list, 0, sizeof(*list));
}

static struct open_with_app *list_find(struct open_with_list *list, const WCHAR *file)
{
    UINT i;

    for (i = 0; i < list->count; i++)
        if (!wcsicmp(list->apps[i].file, file)) return &list->apps[i];
    return NULL;
}

static struct open_with_app *list_add(struct open_with_list *list, const WCHAR *file)
{
    struct open_with_app *app;

    if ((app = list_find(list, file))) return app;
    if (!(app = realloc(list->apps, (list->count + 1) * sizeof(*app)))) return NULL;
    list->apps = app;
    app = &list->apps[list->count++];
    memset(app, 0, sizeof(*app));
    lstrcpynW(app->file, file, MAX_PATH);
    return app;
}

static void set_recommended(struct open_with_list *list, struct open_with_app *app)
{
    if (!app->recommended) list->recommended++;
    app->recommended = TRUE;
}

/* the program of a command line: arguments removed, environment expanded, searched in the path */
static BOOL path_from_cmd(WCHAR *path, const WCHAR *cmd)
{
    WCHAR buffer[MAX_PATH * 2];
    const WCHAR *end;
    UINT len;

    while (*cmd == ' ') cmd++;
    if (*cmd == '"')
    {
        cmd++;
        end = wcschr(cmd, '"');
    }
    else end = wcschr(cmd, ' ');
    len = end ? end - cmd : wcslen(cmd);
    if (len >= ARRAY_SIZE(buffer)) return FALSE;
    memcpy(buffer, cmd, len * sizeof(WCHAR));
    buffer[len] = 0;
    ExpandEnvironmentStringsW(buffer, path, MAX_PATH);
    if (PathFileExistsW(path)) return TRUE;
    len = SearchPathW(NULL, buffer, NULL, MAX_PATH, path, NULL);
    return len && len < MAX_PATH;
}

static void list_load(struct open_with_list *list)
{
    WCHAR name[MAX_PATH], subkey[MAX_PATH + 32];
    struct open_with_app *app;
    DWORD i, len, size;
    HKEY key, app_key;

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Applications", 0, KEY_READ, &key)) return;
    for (i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL); i++)
    {
        if (!(app = list_add(list, name))) break;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &app_key)) { app->hidden = TRUE; continue; }
        if (!RegQueryValueExW(app_key, L"NoOpenWith", NULL, NULL, NULL, NULL)) app->hidden = TRUE;
        RegCloseKey(app_key);
        swprintf(subkey, ARRAY_SIZE(subkey), L"%s\\shell\\open\\command", name);
        size = sizeof(app->cmd);
        if (RegGetValueW(key, subkey, NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, app->cmd, &size))
            app->hidden = TRUE;
    }
    RegCloseKey(key);
}

/* the program named by a ProgID's open command is recommended */
static void load_from_progid(struct open_with_list *list, HKEY key)
{
    WCHAR cmd[MAX_PATH * 2], path[MAX_PATH];
    struct open_with_app *app;
    DWORD size = sizeof(cmd);

    if (RegGetValueW(key, L"shell\\open\\command", NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     NULL, cmd, &size))
        return;
    if (!path_from_cmd(path, cmd) || !(app = list_add(list, PathFindFileNameW(path)))) return;
    if (!app->cmd[0]) lstrcpynW(app->cmd, cmd, ARRAY_SIZE(app->cmd));
    set_recommended(list, app);
}

static void load_progid_list(struct open_with_list *list, HKEY key)
{
    WCHAR progid[MAX_PATH];
    DWORD i, len;
    HKEY sub, progid_key;

    if (RegOpenKeyExW(key, L"OpenWithProgids", 0, KEY_READ, &sub)) return;
    for (i = 0; len = ARRAY_SIZE(progid), !RegEnumValueW(sub, i, progid, &len, NULL, NULL, NULL, NULL); i++)
    {
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, progid, 0, KEY_READ, &progid_key)) continue;
        load_from_progid(list, progid_key);
        RegCloseKey(progid_key);
    }
    RegCloseKey(sub);
}

/* OpenWithList: programs as subkeys (HKCR\.ext) or values "a", "b" ... (the user's recently used list) */
static void load_app_list(struct open_with_list *list, HKEY key, BOOL recent)
{
    WCHAR name[MAX_PATH], value[MAX_PATH];
    struct open_with_app *app;
    DWORD i, len, size;
    HKEY sub;

    if (RegOpenKeyExW(key, L"OpenWithList", 0, KEY_READ, &sub)) return;
    for (i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW(sub, i, name, &len, NULL, NULL, NULL, NULL); i++)
        if ((app = list_find(list, name))) set_recommended(list, app);
    for (i = 0; len = ARRAY_SIZE(name), size = sizeof(value),
         !RegEnumValueW(sub, i, name, &len, NULL, NULL, (BYTE *)value, &size); i++)
    {
        if (!wcscmp(name, L"MRUList") || !(app = list_find(list, value))) continue;
        set_recommended(list, app);
        if (recent) app->recent = TRUE;
    }
    RegCloseKey(sub);
}

static void list_load_recommended(struct open_with_list *list, const WCHAR *file)
{
    WCHAR buffer[MAX_PATH + 64], progid[MAX_PATH];
    const WCHAR *ext = PathFindExtensionW(file);
    DWORD size;
    HKEY key, progid_key;

    if (!ext[0]) return;

    swprintf(buffer, ARRAY_SIZE(buffer), L"%s%s", file_exts_key, ext);
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, buffer, 0, KEY_READ, &key))
    {
        load_app_list(list, key, TRUE);
        load_progid_list(list, key);
        RegCloseKey(key);
    }

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, ext, 0, KEY_READ, &key)) return;
    size = sizeof(progid);
    if (!RegGetValueW(key, NULL, NULL, RRF_RT_REG_SZ, NULL, progid, &size) &&
        !RegOpenKeyExW(HKEY_CLASSES_ROOT, progid, 0, KEY_READ, &progid_key))
    {
        load_from_progid(list, progid_key);
        RegCloseKey(progid_key);
    }
    else load_from_progid(list, key);
    load_app_list(list, key, FALSE);
    load_progid_list(list, key);
    RegCloseKey(key);
}

static const WCHAR *app_name(struct open_with_app *app)
{
    WCHAR path[MAX_PATH], query[64], *value;
    struct { WORD lang, code; } *translation;
    UINT len;
    DWORD size;
    void *data;

    if (app->name[0]) return app->name;
    swprintf(path, ARRAY_SIZE(path), L"Applications\\%s", app->file);
    size = sizeof(app->name);
    if (!RegGetValueW(HKEY_CLASSES_ROOT, path, L"FriendlyAppName", RRF_RT_REG_SZ, NULL, app->name, &size))
        return app->name;
    lstrcpynW(app->name, app->file, MAX_PATH);
    PathRemoveExtensionW(app->name);
    if (!path_from_cmd(path, app->cmd) || !(size = GetFileVersionInfoSizeW(path, NULL))) return app->name;
    if (!(data = malloc(size))) return app->name;
    if (GetFileVersionInfoW(path, 0, size, data) &&
        VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&translation, &len) && len >= 4)
    {
        swprintf(query, ARRAY_SIZE(query), L"\\StringFileInfo\\%04x%04x\\FileDescription",
                 translation->lang, translation->code);
        if (VerQueryValueW(data, query, (void **)&value, &len) && len > 1)
            lstrcpynW(app->name, value, MAX_PATH);
    }
    free(data);
    return app->name;
}

static HICON app_icon(struct open_with_app *app)
{
    WCHAR path[MAX_PATH];
    SHFILEINFOW info;

    if (app->icon || !path_from_cmd(path, app->cmd)) return app->icon;
    if (!ExtractIconExW(path, 0, NULL, &app->icon, 1) &&
        SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON))
        app->icon = info.hIcon;
    return app->icon;
}

/* the program goes to the front of the user's recently used list for the extension */
static void add_to_recent(struct open_with_app *app, const WCHAR *file)
{
    WCHAR buffer[MAX_PATH + 64], order[32], name[2] = { 0 }, value[MAX_PATH];
    const WCHAR *ext = PathFindExtensionW(file);
    DWORD size;
    HKEY key;
    UINT i;

    if (!ext[0]) return;
    swprintf(buffer, ARRAY_SIZE(buffer), L"%s%s\\OpenWithList", file_exts_key, ext);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, buffer, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL)) return;
    size = sizeof(order);
    if (RegGetValueW(key, NULL, L"MRUList", RRF_RT_REG_SZ, NULL, order, &size)) order[0] = 0;

    /* an entry for this program already, else the first free letter */
    for (i = 0; order[i]; i++)
    {
        name[0] = order[i];
        size = sizeof(value);
        if (!RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, value, &size) && !wcsicmp(value, app->file)) break;
    }
    if (order[i]) memmove(order + i, order + i + 1, wcslen(order + i) * sizeof(WCHAR));
    else
    {
        for (name[0] = 'a'; name[0] <= 'z' && wcschr(order, name[0]); name[0]++) ;
        if (name[0] > 'z') name[0] = order[wcslen(order) - 1], order[wcslen(order) - 1] = 0;
        RegSetValueExW(key, name, 0, REG_SZ, (BYTE *)app->file, (wcslen(app->file) + 1) * sizeof(WCHAR));
    }
    memmove(order + 1, order, (wcslen(order) + 1) * sizeof(WCHAR));
    order[0] = name[0];
    RegSetValueExW(key, L"MRUList", 0, REG_SZ, (BYTE *)order, (wcslen(order) + 1) * sizeof(WCHAR));
    RegCloseKey(key);
    app->recent = TRUE;
}

/* HKCR\Applications\<file> gets the program's open command, so it can be found and used later */
static void save_app(const struct open_with_app *app)
{
    WCHAR subkey[MAX_PATH + 64];

    swprintf(subkey, ARRAY_SIZE(subkey), L"Applications\\%s\\shell\\open\\command", app->file);
    RegSetKeyValueW(HKEY_CLASSES_ROOT, subkey, NULL, REG_SZ, app->cmd, (wcslen(app->cmd) + 1) * sizeof(WCHAR));
}

static void app_execute(struct open_with_app *app, const WCHAR *file)
{
    SHELLEXECUTEINFOW info = { sizeof(info), SEE_MASK_CLASSKEY };
    WCHAR subkey[MAX_PATH + 32];
    HKEY key;

    save_app(app);
    if (!app->recent) add_to_recent(app, file);
    swprintf(subkey, ARRAY_SIZE(subkey), L"Applications\\%s", app->file);
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, subkey, 0, KEY_READ, &key)) return;
    info.hkeyClass = key;
    info.lpFile = file;
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
    RegCloseKey(key);
}

/* "Always use the selected program": the extension's ProgID ("ext_auto_file" if it had none) opens with it */
static void set_default_handler(struct open_with_app *app, const WCHAR *file)
{
    WCHAR progid[MAX_PATH], subkey[MAX_PATH + 32];
    const WCHAR *ext = PathFindExtensionW(file);
    DWORD size = sizeof(progid);
    HKEY src, dst;

    if (!ext[0] || !ext[1]) return;
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, NULL, progid, &size) || !progid[0])
    {
        swprintf(progid, ARRAY_SIZE(progid), L"%s_auto_file", ext + 1);
        RegSetKeyValueW(HKEY_CLASSES_ROOT, ext, NULL, REG_SZ, progid, (wcslen(progid) + 1) * sizeof(WCHAR));
    }
    swprintf(subkey, ARRAY_SIZE(subkey), L"Applications\\%s\\shell", app->file);
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, subkey, 0, KEY_READ, &src)) return;
    swprintf(subkey, ARRAY_SIZE(subkey), L"%s\\shell", progid);
    SHDeleteKeyW(HKEY_CLASSES_ROOT, subkey);
    if (!RegCreateKeyExW(HKEY_CLASSES_ROOT, subkey, 0, NULL, 0, KEY_WRITE, NULL, &dst, NULL))
    {
        SHCopyKeyW(src, NULL, dst, 0);
        RegCloseKey(dst);
    }
    RegCloseKey(src);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSHNOWAIT, NULL, NULL);
}

struct open_with_dialog
{
    const OPENASINFO *info;
    struct open_with_list list;
    HWND hwnd, tree;
    HTREEITEM recommended, other;
    HIMAGELIST images;
    UINT flags;
};

static void dialog_add_app(struct open_with_dialog *dlg, struct open_with_app *app, BOOL select)
{
    TVINSERTSTRUCTW insert = { 0 };
    HTREEITEM item;
    HICON icon;

    insert.hParent = app->recommended ? dlg->recommended : dlg->other;
    insert.hInsertAfter = TVI_SORT;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = (WCHAR *)app_name(app);
    insert.item.lParam = (LPARAM)app;
    if ((icon = app_icon(app)))
    {
        insert.item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        insert.item.iImage = insert.item.iSelectedImage = ImageList_AddIcon(dlg->images, icon);
    }
    item = (HTREEITEM)SendMessageW(dlg->tree, TVM_INSERTITEMW, 0, (LPARAM)&insert);
    if (select) SendMessageW(dlg->tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)item);
}

static struct open_with_app *dialog_current(struct open_with_dialog *dlg)
{
    TVITEMW item = { TVIF_PARAM };

    if (!(item.hItem = (HTREEITEM)SendMessageW(dlg->tree, TVM_GETNEXTITEM, TVGN_CARET, 0))) return NULL;
    if (!SendMessageW(dlg->tree, TVM_GETITEMW, 0, (LPARAM)&item)) return NULL;
    return (struct open_with_app *)item.lParam;
}

static HTREEITEM dialog_add_group(struct open_with_dialog *dlg, UINT id, int image)
{
    TVINSERTSTRUCTW insert = { 0 };
    WCHAR text[64];

    LoadStringW(shell32_hInstance, id, text, ARRAY_SIZE(text));
    insert.hParent = TVI_ROOT;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_STATE | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText = text;
    insert.item.state = insert.item.stateMask = TVIS_EXPANDED;
    insert.item.iImage = insert.item.iSelectedImage = image;
    return (HTREEITEM)SendMessageW(dlg->tree, TVM_INSERTITEMW, 0, (LPARAM)&insert);
}

static void dialog_init(struct open_with_dialog *dlg, HWND hwnd)
{
    const WCHAR *ext = PathFindExtensionW(dlg->info->pcszFile);
    WCHAR text[MAX_PATH * 2];
    BOOL selected = FALSE;
    int folder = -1;
    UINT i, len;
    HICON icon;

    dlg->hwnd = hwnd;
    dlg->flags = dlg->info->oaifInFlags;
    /* no association for files without an extension or for programs */
    if (!ext[0] || !ext[1] || PathIsExeAW(dlg->info->pcszFile))
        dlg->flags &= ~(OAIF_ALLOW_REGISTRATION | OAIF_FORCE_REGISTRATION | OAIF_REGISTER_EXT);
    if (!(dlg->flags & OAIF_ALLOW_REGISTRATION)) EnableWindow(GetDlgItem(hwnd, IDC_OPENWITH_ALWAYS), FALSE);
    if (dlg->flags & OAIF_FORCE_REGISTRATION)
    {
        /* Change... of the file's properties: always, as Windows XP shows it */
        CheckDlgButton(hwnd, IDC_OPENWITH_ALWAYS, BST_CHECKED);
        EnableWindow(GetDlgItem(hwnd, IDC_OPENWITH_ALWAYS), FALSE);
    }
    if (dlg->flags & OAIF_HIDE_REGISTRATION) ShowWindow(GetDlgItem(hwnd, IDC_OPENWITH_ALWAYS), SW_HIDE);

    len = GetDlgItemTextW(hwnd, IDC_OPENWITH_FILE, text, ARRAY_SIZE(text));
    lstrcpynW(text + len, PathFindFileNameW(dlg->info->pcszFile), ARRAY_SIZE(text) - len);
    SetDlgItemTextW(hwnd, IDC_OPENWITH_FILE, text);

    list_load(&dlg->list);
    list_load_recommended(&dlg->list, dlg->info->pcszFile);

    dlg->tree = GetDlgItem(hwnd, IDC_OPENWITH_PROGRAMS);
    dlg->images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, dlg->list.count + 1, 4);
    SendMessageW(dlg->tree, TVM_SETIMAGELIST, TVSIL_NORMAL, (LPARAM)dlg->images);
    if ((icon = LoadImageW(shell32_hInstance, MAKEINTRESOURCEW(IDI_SHELL_APP_FOLDER), IMAGE_ICON, 16, 16, 0)))
    {
        folder = ImageList_AddIcon(dlg->images, icon);
        DestroyIcon(icon);
    }
    if (dlg->list.recommended)
    {
        dlg->recommended = dialog_add_group(dlg, IDS_OPEN_WITH_RECOMMENDED, folder);
        dlg->other = dialog_add_group(dlg, IDS_OPEN_WITH_OTHER, folder);
    }
    else dlg->recommended = dlg->other = TVI_ROOT;

    for (i = 0; i < dlg->list.count; i++)
    {
        struct open_with_app *app = &dlg->list.apps[i];

        if (app->hidden) continue;
        dialog_add_app(dlg, app, !selected && (app->recommended || !dlg->list.recommended));
        if (app->recommended || !dlg->list.recommended) selected = TRUE;
    }
    EnableWindow(GetDlgItem(hwnd, IDOK), dialog_current(dlg) != NULL);
}

static void dialog_browse(struct open_with_dialog *dlg)
{
    WCHAR path[MAX_PATH] = { 0 }, title[64], filter[128], all[32], programs[32];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    struct open_with_app *app;
    int len;

    LoadStringW(shell32_hInstance, IDS_OPEN_WITH_PROGRAMS, programs, ARRAY_SIZE(programs));
    LoadStringW(shell32_hInstance, IDS_ALL_FILES, all, ARRAY_SIZE(all));
    LoadStringW(shell32_hInstance, IDS_OPEN_WITH, title, ARRAY_SIZE(title));
    len = swprintf(filter, ARRAY_SIZE(filter), L"%s%c*.exe;*.com;*.bat;*.cmd%c%s%c*.*%c", programs, 0, 0, all, 0, 0);
    filter[len] = 0;

    ofn.hwndOwner = dlg->hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAY_SIZE(path);
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    if (!(app = list_add(&dlg->list, PathFindFileNameW(path)))) return;
    swprintf(app->cmd, ARRAY_SIZE(app->cmd), L"\"%s\" \"%%1\"", path);
    app->name[0] = 0;
    app->hidden = FALSE;
    save_app(app);
    /* the list may have moved: rebuild the tree */
    SendMessageW(dlg->tree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    ImageList_Remove(dlg->images, -1);
    {
        WCHAR file[MAX_PATH];
        UINT i;

        lstrcpynW(file, app->file, MAX_PATH);
        if (dlg->list.recommended)
        {
            HICON icon = LoadImageW(shell32_hInstance, MAKEINTRESOURCEW(IDI_SHELL_APP_FOLDER), IMAGE_ICON, 16, 16, 0);
            int folder = icon ? ImageList_AddIcon(dlg->images, icon) : -1;

            if (icon) DestroyIcon(icon);
            dlg->recommended = dialog_add_group(dlg, IDS_OPEN_WITH_RECOMMENDED, folder);
            dlg->other = dialog_add_group(dlg, IDS_OPEN_WITH_OTHER, folder);
        }
        for (i = 0; i < dlg->list.count; i++)
        {
            if (dlg->list.apps[i].hidden) continue;
            if (dlg->list.apps[i].icon) { DestroyIcon(dlg->list.apps[i].icon); dlg->list.apps[i].icon = NULL; }
            dialog_add_app(dlg, &dlg->list.apps[i], !wcsicmp(dlg->list.apps[i].file, file));
        }
    }
}

static void dialog_accept(struct open_with_dialog *dlg)
{
    struct open_with_app *app = dialog_current(dlg);

    if (!app) return;
    if (IsDlgButtonChecked(dlg->hwnd, IDC_OPENWITH_ALWAYS) == BST_CHECKED && (dlg->flags & OAIF_REGISTER_EXT))
        set_default_handler(app, dlg->info->pcszFile);
    if (dlg->flags & OAIF_EXEC) app_execute(app, dlg->info->pcszFile);
    EndDialog(dlg->hwnd, IDOK);
}

static INT_PTR CALLBACK open_with_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct open_with_dialog *dlg = (struct open_with_dialog *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
        dlg = (struct open_with_dialog *)lparam;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)dlg);
        dialog_init(dlg, hwnd);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_OPENWITH_BROWSE: dialog_browse(dlg); return TRUE;
        case IDOK: dialog_accept(dlg); return TRUE;
        case IDCANCEL: EndDialog(hwnd, IDCANCEL); return TRUE;
        }
        break;
    case WM_NOTIFY:
        switch (((NMHDR *)lparam)->code)
        {
        case TVN_SELCHANGEDW:
            EnableWindow(GetDlgItem(hwnd, IDOK), dialog_current(dlg) != NULL);
            break;
        case NM_DBLCLK:
        case NM_RETURN:
            dialog_accept(dlg);
            break;
        }
        break;
    }
    return FALSE;
}

/*************************************************************************
 * SHOpenWithDialog  [SHELL32.@]
 */
HRESULT WINAPI SHOpenWithDialog(HWND parent, const OPENASINFO *info)
{
    struct open_with_dialog dlg = { 0 };
    INT_PTR ret;

    TRACE("%p, %p\n", parent, info);

    if (!info || !info->pcszFile) return E_INVALIDARG;
    dlg.info = info;
    ret = DialogBoxParamW(shell32_hInstance, MAKEINTRESOURCEW(IDD_OPEN_WITH), parent, open_with_proc, (LPARAM)&dlg);
    list_free(&dlg.list);
    if (dlg.images) ImageList_Destroy(dlg.images);
    return ret == IDOK ? S_OK : HRESULT_FROM_WIN32(ERROR_CANCELLED);
}

/* The dialog to choose a program and open the file with it */
HRESULT open_with_choose(HWND parent, const WCHAR *file)
{
    OPENASINFO info = { file, NULL, OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT | OAIF_EXEC };

    return SHOpenWithDialog(parent, &info);
}

/*************************************************************************
 * OpenAs_RunDLLW  [SHELL32.@]
 */
void WINAPI OpenAs_RunDLLW(HWND hwnd, HINSTANCE hinst, LPCWSTR cmdline, int cmdshow)
{
    WCHAR file[MAX_PATH];

    TRACE("%p, %p, %s, %d\n", hwnd, hinst, debugstr_w(cmdline), cmdshow);

    lstrcpynW(file, cmdline, MAX_PATH);
    PathUnquoteSpacesW(file);
    open_with_choose(hwnd, file);
}

/*************************************************************************
 * OpenAs_RunDLLA  [SHELL32.@]
 */
void WINAPI OpenAs_RunDLLA(HWND hwnd, HINSTANCE hinst, LPCSTR cmdline, int cmdshow)
{
    WCHAR file[MAX_PATH];

    MultiByteToWideChar(CP_ACP, 0, cmdline, -1, file, MAX_PATH);
    OpenAs_RunDLLW(hwnd, hinst, file, cmdshow);
}

/* The item menu's Open With: a submenu of the recommended programs and Choose Program..., or only Open With...
 * when there are not two of them.  Commands from first: one per program, then Choose Program. */
struct open_with_menu
{
    struct open_with_list list;
    UINT apps[32];
    UINT count;
};

struct open_with_menu *open_with_menu_add(HMENU hmenu, UINT pos, UINT first, const WCHAR *file)
{
    struct open_with_menu *menu;
    WCHAR text[64];
    HMENU submenu;
    UINT i;

    if (!(menu = calloc(1, sizeof(*menu)))) return NULL;
    list_load(&menu->list);
    list_load_recommended(&menu->list, file);
    for (i = 0; i < menu->list.count && menu->count < ARRAY_SIZE(menu->apps); i++)
        if (menu->list.apps[i].recommended && !menu->list.apps[i].hidden) menu->apps[menu->count++] = i;

    if (menu->count > 1)
    {
        submenu = CreatePopupMenu();
        for (i = 0; i < menu->count; i++)
            AppendMenuW(submenu, MF_STRING, first + i, app_name(&menu->list.apps[menu->apps[i]]));
        AppendMenuW(submenu, MF_SEPARATOR, 0, NULL);
        LoadStringW(shell32_hInstance, IDS_OPEN_WITH_CHOOSE, text, ARRAY_SIZE(text));
        AppendMenuW(submenu, MF_STRING, first + menu->count, text);
        LoadStringW(shell32_hInstance, IDS_OPEN_WITH, text, ARRAY_SIZE(text));
        InsertMenuW(hmenu, pos, MF_BYPOSITION | MF_POPUP, (UINT_PTR)submenu, text);
    }
    else
    {
        menu->count = 0;
        LoadStringW(shell32_hInstance, IDS_OPEN_WITH_DOTS, text, ARRAY_SIZE(text));
        InsertMenuW(hmenu, pos, MF_BYPOSITION | MF_STRING, first, text);
    }
    return menu;
}

UINT open_with_menu_count(const struct open_with_menu *menu)
{
    return menu ? menu->count + 1 : 0;
}

void open_with_menu_invoke(struct open_with_menu *menu, HWND hwnd, UINT index, const WCHAR *file)
{
    if (index < menu->count) app_execute(&menu->list.apps[menu->apps[index]], file);
    else open_with_choose(hwnd, file);
}

void open_with_menu_free(struct open_with_menu *menu)
{
    if (!menu) return;
    list_free(&menu->list);
    free(menu);
}
