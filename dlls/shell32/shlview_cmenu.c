/*
 *	IContextMenu for items in the shellview
 *
 * Copyright 1998-2000 Juergen Schmied <juergen.schmied@debitel.net>,
 *                                     <juergen.schmied@metronet.de>
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

#include <string.h>

#define COBJMACROS
#include "winerror.h"

#include "windef.h"
#include "wingdi.h"
#include "pidl.h"
#include "shlobj.h"
#include "winreg.h"
#include "prsht.h"

#include "shell32_main.h"
#include "shellfolder.h"

#include "shresdef.h"
#include "shlwapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#define FCIDM_BASE 0x7000

#define VERB_ID_OFFSET 0x200

#define SENDTO_ID_OFFSET 0x300

#define OPENWITH_ID_OFFSET 0x380

enum send_to_kind { SEND_TO_SHORTCUT, SEND_TO_COPY, SEND_TO_PROGRAM };

struct send_to
{
    WCHAR *text;
    WCHAR *path;   /* folder to copy to, folder for shortcuts or program to run */
    enum send_to_kind kind;
};

struct verb
{
    WCHAR *desc;
    WCHAR *verb;
};

typedef struct
{
    IContextMenu3 IContextMenu3_iface;
    IShellExtInit IShellExtInit_iface;
    IObjectWithSite IObjectWithSite_iface;
    LONG ref;

    IShellFolder* parent;

    struct verb *verbs;
    size_t verb_count;
    WCHAR filetype[MAX_PATH];

    /* item menu data */
    LPITEMIDLIST  pidl;  /* root pidl */
    LPITEMIDLIST *apidl; /* array of child pidls */
    UINT cidl;

    /* Send To targets of the item menu */
    struct send_to *send_to;
    UINT send_to_count;

    /* Open With of a single file */
    struct open_with_menu *open_with;

    /* background menu data */
    BOOL desktop;
    IContextMenu3 *new_menu;  /* its New submenu */
} ContextMenu;

static inline ContextMenu *impl_from_IContextMenu3(IContextMenu3 *iface)
{
    return CONTAINING_RECORD(iface, ContextMenu, IContextMenu3_iface);
}

static inline ContextMenu *impl_from_IShellExtInit(IShellExtInit *iface)
{
    return CONTAINING_RECORD(iface, ContextMenu, IShellExtInit_iface);
}

static inline ContextMenu *impl_from_IObjectWithSite(IObjectWithSite *iface)
{
    return CONTAINING_RECORD(iface, ContextMenu, IObjectWithSite_iface);
}

static HRESULT WINAPI ContextMenu_QueryInterface(IContextMenu3 *iface, REFIID riid, LPVOID *ppvObj)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), ppvObj);

    *ppvObj = NULL;

    if (IsEqualIID(riid, &IID_IUnknown)      ||
        IsEqualIID(riid, &IID_IContextMenu)  ||
        IsEqualIID(riid, &IID_IContextMenu2) ||
        IsEqualIID(riid, &IID_IContextMenu3))
    {
        *ppvObj = &This->IContextMenu3_iface;
    }
    else if (IsEqualIID(riid, &IID_IShellExtInit))
    {
        *ppvObj = &This->IShellExtInit_iface;
    }
    else if (IsEqualIID(riid, &IID_IObjectWithSite))
    {
        *ppvObj = &This->IObjectWithSite_iface;
    }

    if(*ppvObj)
    {
        IContextMenu3_AddRef(iface);
        return S_OK;
    }

    TRACE("-- Interface: E_NOINTERFACE\n");
    return E_NOINTERFACE;
}

static ULONG WINAPI ContextMenu_AddRef(IContextMenu3 *iface)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%lu)\n", This, ref);
    return ref;
}

static ULONG WINAPI ContextMenu_Release(IContextMenu3 *iface)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(%lu)\n", This, ref);

    if (!ref)
    {
        if(This->parent)
            IShellFolder_Release(This->parent);

        SHFree(This->pidl);
        _ILFreeaPidl(This->apidl, This->cidl);
        if (This->new_menu) IContextMenu3_Release(This->new_menu);
        for (unsigned int i = 0; i < This->send_to_count; ++i)
        {
            free(This->send_to[i].text);
            free(This->send_to[i].path);
        }
        free(This->send_to);
        open_with_menu_free(This->open_with);

        for (unsigned int i = 0; i < This->verb_count; ++i)
        {
            free(This->verbs[i].desc);
            free(This->verbs[i].verb);
        }
        free(This->verbs);
        free(This);
    }

    return ref;
}

static BOOL can_paste(const ITEMIDLIST *dst_pidl)
{
    IDataObject *data;
    FORMATETC format;

    if (!(_ILIsFolder(dst_pidl) || _ILIsDrive(dst_pidl)))
        return FALSE;

    if (FAILED(OleGetClipboard(&data)))
        return FALSE;

    InitFormatEtc(format, RegisterClipboardFormatW(CFSTR_SHELLIDLISTW), TYMED_HGLOBAL);
    if (SUCCEEDED(IDataObject_QueryGetData(data, &format)))
    {
        IDataObject_Release(data);
        return TRUE;
    }

    InitFormatEtc(format, CF_HDROP, TYMED_HGLOBAL);
    if (SUCCEEDED(IDataObject_QueryGetData(data, &format)))
    {
        IDataObject_Release(data);
        return TRUE;
    }

    IDataObject_Release(data);
    return FALSE;
}

static UINT max_menu_id(HMENU hmenu, UINT offset, UINT last)
{
    int i;
    UINT max_id = 0;

    for (i = GetMenuItemCount(hmenu) - 1; i >= 0; i--)
    {
        MENUITEMINFOW item;
        memset(&item, 0, sizeof(MENUITEMINFOW));
        item.cbSize = sizeof(MENUITEMINFOW);
        item.fMask =  MIIM_ID | MIIM_SUBMENU | MIIM_TYPE;
        if (!GetMenuItemInfoW(hmenu, i, TRUE, &item))
            continue;
        if (!(item.fType & MFT_SEPARATOR))
        {
            if (item.hSubMenu)
            {
                UINT submenu_max_id = max_menu_id(item.hSubMenu, offset, last);
                if (max_id < submenu_max_id)
                    max_id = submenu_max_id;
            }
            if (item.wID + offset <= last)
            {
                if (max_id <= item.wID + offset)
                    max_id = item.wID + offset + 1;
            }
        }
    }
    return max_id;
}

static void add_send_to_menu(ContextMenu *This, HMENU hmenu, UINT pos, UINT first);

/* file system path of the first item */
static BOOL get_item_path(ContextMenu *This, WCHAR *path)
{
    ITEMIDLIST *pidl = ILCombine(This->pidl, This->apidl[0]);
    BOOL ret = pidl && SHGetPathFromIDListW(pidl, path);

    ILFree(pidl);
    return ret;
}
static void do_send_to(ContextMenu *This, HWND hwnd, UINT index);

static HRESULT WINAPI ItemMenu_QueryContextMenu(
	IContextMenu3 *iface,
	HMENU hmenu,
	UINT indexMenu,
	UINT idCmdFirst,
	UINT idCmdLast,
	UINT uFlags)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    MENUITEMINFOW mi;
    WCHAR path[MAX_PATH];
    INT uIDMax;
    UINT pos;

    TRACE("(%p)->(%p %d 0x%x 0x%x 0x%x )\n", This, hmenu, indexMenu, idCmdFirst, idCmdLast, uFlags);

    if(!(CMF_DEFAULTONLY & uFlags) && This->cidl > 0)
    {
        HMENU hmenures = LoadMenuW(shell32_hInstance, MAKEINTRESOURCEW(MENU_SHV_FILE));

        Shell_MergeMenus(hmenu, GetSubMenu(hmenures, 0), indexMenu, idCmdFirst - FCIDM_BASE, idCmdLast, MM_SUBMENUSHAVEIDS);
        uIDMax = max_menu_id(GetSubMenu(hmenures, 0), idCmdFirst - FCIDM_BASE, idCmdLast);

        DestroyMenu(hmenures);

        for (size_t i = 0; i < This->verb_count; ++i)
        {
            mi.cbSize = sizeof(mi);
            mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_STRING;
            mi.dwTypeData = This->verbs[i].desc;
            mi.fState = MFS_ENABLED;
            mi.wID = idCmdFirst + VERB_ID_OFFSET + i;
            mi.fType = MFT_STRING;
            InsertMenuItemW(hmenu, i, MF_BYPOSITION, &mi);
            uIDMax = max(uIDMax, mi.wID + 1);
        }
        if (This->verb_count)
        {
            mi.cbSize = sizeof(mi);
            mi.fMask = MIIM_FTYPE;
            mi.fType = MFT_SEPARATOR;
            InsertMenuItemW(hmenu, This->verb_count, MF_BYPOSITION, &mi);
        }

        /* Windows XP: Open With after the verbs of a single file */
        if (This->cidl == 1 && !This->open_with && !_ILIsFolder(This->apidl[0]) && get_item_path(This, path))
        {
            This->open_with = open_with_menu_add(hmenu, This->verb_count, idCmdFirst + OPENWITH_ID_OFFSET, path);
            uIDMax = max(uIDMax, idCmdFirst + OPENWITH_ID_OFFSET + open_with_menu_count(This->open_with));
        }

        /* Windows XP: Send To and a separator after the verbs */
        pos = This->verb_count + (This->open_with ? 1 : 0);
        add_send_to_menu(This, hmenu, pos ? pos + 1 : 0, idCmdFirst);
        if (This->send_to_count)
            uIDMax = max(uIDMax, idCmdFirst + SENDTO_ID_OFFSET + This->send_to_count);

        SetMenuDefaultItem(hmenu, 0, MF_BYPOSITION);

        if(uFlags & ~CMF_CANRENAME)
            RemoveMenu(hmenu, FCIDM_SHVIEW_RENAME - FCIDM_BASE + idCmdFirst, MF_BYCOMMAND);
        else
        {
            UINT enable = MF_BYCOMMAND;

            /* can't rename more than one item at a time*/
            if (!This->apidl || This->cidl > 1)
                enable |= MFS_DISABLED;
            else
            {
                DWORD attr = SFGAO_CANRENAME;

                IShellFolder_GetAttributesOf(This->parent, 1, (LPCITEMIDLIST*)This->apidl, &attr);
                enable |= (attr & SFGAO_CANRENAME) ? MFS_ENABLED : MFS_DISABLED;
            }

            EnableMenuItem(hmenu, FCIDM_SHVIEW_RENAME - FCIDM_BASE + idCmdFirst, enable);
        }

        /* It's legal to paste into more than one pidl at once. In that case
         * the first is used and the rest are ignored. */
        if (!can_paste(This->apidl[0]))
            RemoveMenu(hmenu, FCIDM_SHVIEW_INSERT - FCIDM_BASE + idCmdFirst, MF_BYCOMMAND);

        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, uIDMax-idCmdFirst);
    }
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
}

static void execute_verb(ContextMenu *menu, HWND hwnd, const WCHAR *verb)
{
    unsigned int i;

    /* When the default "open" verb is invoked on a folder inside a hosting shell browser
     * (e.g. the common file dialog), navigate the browser into the folder instead of handing
     * the "Folder" class open verb to ShellExecuteEx.
     */
    if (!wcsicmp(verb, L"open") && !wcsicmp(menu->filetype, L"Folder"))
    {
        IShellBrowser *browser = (IShellBrowser *)SendMessageA(hwnd, CWM_GETISHELLBROWSER, 0, 0);
        if (browser)
        {
            for (i = 0; i < menu->cidl; ++i)
            {
                SFGAOF attributes = SFGAO_FOLDER;

                if (SUCCEEDED(IShellFolder_GetAttributesOf(menu->parent, 1, (LPCITEMIDLIST *)&menu->apidl[i],
                              &attributes)) && (attributes & SFGAO_FOLDER))
                {
                    IShellBrowser_BrowseObject(browser, menu->apidl[i], SBSP_DEFBROWSER | SBSP_RELATIVE);
                    return;
                }
            }
        }
    }

    for (i = 0; i < menu->cidl; ++i)
    {
        LPITEMIDLIST abs_pidl = ILCombine(menu->pidl, menu->apidl[i]);
        SHELLEXECUTEINFOW info = {0};

        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_IDLIST | SEE_MASK_CLASSNAME;
        info.lpIDList = abs_pidl;
        info.lpClass = menu->filetype;
        info.hwnd = hwnd;
        info.nShow = SW_SHOWNORMAL;
        info.lpVerb = verb;
        ShellExecuteExW(&info);
        ILFree(abs_pidl);
    }
}

/**************************************************************************
 * DoDelete
 *
 * deletes the currently selected items
 */
static void DoDelete(ContextMenu *This)
{
    ISFHelper *helper;

    IShellFolder_QueryInterface(This->parent, &IID_ISFHelper, (void**)&helper);
    if (helper)
    {
        ISFHelper_DeleteItems(helper, This->cidl, (LPCITEMIDLIST*)This->apidl);
        ISFHelper_Release(helper);
    }
}

static void do_copy(ContextMenu *This, HWND hwnd, DWORD drop_effect)
{
    IDataObject *dataobject;

    if (SUCCEEDED(IShellFolder_GetUIObjectOf(This->parent, hwnd, This->cidl, (LPCITEMIDLIST*)This->apidl, &IID_IDataObject, 0, (void**)&dataobject)))
    {
        FORMATETC format;
        STGMEDIUM medium;
        DWORD *effect_ptr;
        HRESULT hr;

        InitFormatEtc(format, RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECTW), TYMED_HGLOBAL);
        medium.tymed = TYMED_HGLOBAL;
        medium.pUnkForRelease = NULL;
        if (!(medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))))
        {
            IDataObject_Release(dataobject);
            return;
        }
        effect_ptr = GlobalLock(medium.hGlobal);
        *effect_ptr = drop_effect;
        GlobalUnlock(medium.hGlobal);

        if (FAILED(hr = IDataObject_SetData(dataobject, &format, &medium, TRUE)))
            ERR("Failed to set data, hr %#lx.\n", hr);

        OleSetClipboard(dataobject);
        IDataObject_Release(dataobject);
    }
}

static HRESULT get_data_format(IDataObject *data, UINT cf, STGMEDIUM *medium)
{
    FORMATETC format;

    InitFormatEtc(format, cf, TYMED_HGLOBAL);
    return IDataObject_GetData(data, &format, medium);
}

static WCHAR *build_source_paths(ITEMIDLIST *root_pidl, ITEMIDLIST **pidls, unsigned int count)
{
    WCHAR root_path[MAX_PATH], pidl_path[MAX_PATH];
    size_t size = 1, pos = 0, root_len;
    WCHAR *paths;

    if (!SHGetPathFromIDListW(root_pidl, root_path))
    {
        ERR("Failed to get source root path.\n");
        return NULL;
    }
    root_len = wcslen(root_path);

    for (unsigned int i = 0; i < count; ++i)
    {
        if (!_ILIsValue(pidls[i]) && !_ILIsFolder(pidls[i]))
            ERR("Unexpected child pidl type.\n");

        _ILSimpleGetTextW(pidls[i], pidl_path, ARRAY_SIZE(pidl_path));
        size += root_len + 1 + wcslen(pidl_path) + 1;
    }

    paths = malloc(size * sizeof(WCHAR));

    for (unsigned int i = 0; i < count; ++i)
    {
        if (!_ILIsValue(pidls[i]) && !_ILIsFolder(pidls[i]))
            ERR("Unexpected child pidl type.\n");

        memcpy(paths + pos, root_path, root_len * sizeof(WCHAR));
        pos += root_len;
        paths[pos++] = '\\';
        _ILSimpleGetTextW(pidls[i], paths + pos, size - pos);
        pos += wcslen(paths + pos) + 1;
    }
    paths[pos++] = 0;

    return paths;
}

/* Copies pasted where their sources are get a new name ("Copy of ..."), as Windows Explorer does */
static void rename_copies_in_place(SHFILEOPSTRUCTW *op, const WCHAR *first_source)
{
    WCHAR dir[MAX_PATH];

    if (op->wFunc != FO_COPY) return;
    lstrcpynW(dir, first_source, MAX_PATH);
    PathRemoveFileSpecW(dir);
    if (!wcsicmp(dir, op->pTo)) op->fFlags |= FOF_RENAMEONCOLLISION;
}

/* the folder of a background menu */
static HRESULT get_parent_pidl(ContextMenu *This, ITEMIDLIST **pidl)
{
    IPersistFolder2 *persist;
    HRESULT hr;

    if (FAILED(hr = IShellFolder_QueryInterface(This->parent, &IID_IPersistFolder2, (void **)&persist))) return hr;
    hr = IPersistFolder2_GetCurFolder(persist, pidl);
    IPersistFolder2_Release(persist);
    return hr;
}

/* "Shortcut to name.lnk" in dir, then "Shortcut (2) to name.lnk" ..., as Windows Explorer names them */
HRESULT create_shortcut(const WCHAR *target, const WCHAR *dir)
{
    WCHAR base[MAX_PATH], format[64], name[MAX_PATH], path[MAX_PATH];
    IPersistFile *file;
    IShellLinkW *link;
    DWORD_PTR args[2];
    HRESULT hr;
    UINT n;

    {
        /* named as the item is shown: without a known extension that is hidden, "Local Disk (C:)" for a drive */
        SHFILEINFOW info;
        if (SHGetFileInfoW(target, 0, &info, sizeof(info), SHGFI_DISPLAYNAME) && info.szDisplayName[0])
            lstrcpynW(base, info.szDisplayName, MAX_PATH);
        else
        {
            lstrcpynW(base, PathFindFileNameW(target), MAX_PATH);
            PathRemoveBackslashW(base);
        }
    }
    for (n = 1;; n++)
    {
        if (n == 1000) return E_FAIL;
        LoadStringW(shell32_hInstance, n == 1 ? IDS_SHORTCUT_TO : IDS_SHORTCUT_N_TO, format, ARRAY_SIZE(format));
        args[0] = (DWORD_PTR)base;
        args[1] = n;
        if (!FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY, format, 0, 0,
                            name, ARRAY_SIZE(name) - 4, (va_list *)args))
            return E_FAIL;
        wcscat(name, L".lnk");
        if (!PathCombineW(path, dir, name)) return E_FAIL;
        if (!PathFileExistsW(path)) break;
    }

    if (FAILED(hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)))
        return hr;
    if (SUCCEEDED(hr = IShellLinkW_SetPath(link, target)) &&
        SUCCEEDED(hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)))
    {
        hr = IPersistFile_Save(file, path, TRUE);
        IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
    if (SUCCEEDED(hr)) SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, path, NULL);
    return hr;
}

static void add_send_to(ContextMenu *This, const WCHAR *text, const WCHAR *path, enum send_to_kind kind)
{
    struct send_to *entry;

    if (!(entry = realloc(This->send_to, (This->send_to_count + 1) * sizeof(*entry)))) return;
    This->send_to = entry;
    entry += This->send_to_count++;
    entry->text = wcsdup(text);
    entry->path = wcsdup(path);
    entry->kind = kind;
}

/* Send To, as on Windows XP: Desktop (create shortcut), My Documents, the shortcuts of the user's SendTo folder (to
 * folders or programs) and the removable drives with media.  Mail Recipient and Compressed (zipped) Folder have no
 * handler here. */
static void add_send_to_menu(ContextMenu *This, HMENU hmenu, UINT pos, UINT first)
{
    WCHAR path[MAX_PATH], text[MAX_PATH], pattern[MAX_PATH], target[MAX_PATH], drive[] = L"A:\\";
    WIN32_FIND_DATAW data;
    SHFILEINFOW info;
    HANDLE find;
    HMENU submenu;
    UINT i;

    if (This->send_to_count || !This->cidl || !SHGetPathFromIDListW(This->pidl, path)) return;

    LoadStringW(shell32_hInstance, IDS_SEND_TO_DESKTOP, text, ARRAY_SIZE(text));
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, path)))
        add_send_to(This, text, path, SEND_TO_SHORTCUT);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, path)) &&
        SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_DISPLAYNAME))
        add_send_to(This, info.szDisplayName, path, SEND_TO_COPY);

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_SENDTO, NULL, SHGFP_TYPE_CURRENT, path)))
    {
        swprintf(pattern, ARRAY_SIZE(pattern), L"%s\\*.lnk", path);
        if ((find = FindFirstFileW(pattern, &data)) != INVALID_HANDLE_VALUE)
        {
            do
            {
                IShellLinkW *link;
                IPersistFile *file;

                swprintf(pattern, ARRAY_SIZE(pattern), L"%s\\%s", path, data.cFileName);
                if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)))
                    continue;
                target[0] = 0;
                if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)))
                {
                    if (SUCCEEDED(IPersistFile_Load(file, pattern, STGM_READ)))
                        IShellLinkW_GetPath(link, target, MAX_PATH, NULL, 0);
                    IPersistFile_Release(file);
                }
                IShellLinkW_Release(link);
                if (!target[0]) continue;
                lstrcpynW(text, data.cFileName, MAX_PATH);
                PathRemoveExtensionW(text);
                add_send_to(This, text, target, PathIsDirectoryW(target) ? SEND_TO_COPY : SEND_TO_PROGRAM);
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }

    for (drive[0] = 'A'; drive[0] <= 'Z'; drive[0]++)
    {
        if (GetDriveTypeW(drive) != DRIVE_REMOVABLE || GetFileAttributesW(drive) == INVALID_FILE_ATTRIBUTES) continue;
        if (SHGetFileInfoW(drive, 0, &info, sizeof(info), SHGFI_DISPLAYNAME))
            add_send_to(This, info.szDisplayName, drive, SEND_TO_COPY);
    }

    if (!This->send_to_count) return;
    submenu = CreatePopupMenu();
    for (i = 0; i < This->send_to_count; i++)
        AppendMenuW(submenu, MF_STRING, first + SENDTO_ID_OFFSET + i, This->send_to[i].text);
    LoadStringW(shell32_hInstance, IDS_SEND_TO, text, ARRAY_SIZE(text));
    InsertMenuW(hmenu, pos, MF_BYPOSITION | MF_POPUP, (UINT_PTR)submenu, text);
    InsertMenuW(hmenu, pos + 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
}

static void do_send_to(ContextMenu *This, HWND hwnd, UINT index)
{
    const struct send_to *target = &This->send_to[index];
    WCHAR *paths, *p, *args;
    SHFILEOPSTRUCTW op = { 0 };
    size_t len = 0;

    if (!(paths = build_source_paths(This->pidl, This->apidl, This->cidl))) return;
    switch (target->kind)
    {
    case SEND_TO_SHORTCUT:
        for (p = paths; *p; p += wcslen(p) + 1) create_shortcut(p, target->path);
        break;
    case SEND_TO_COPY:
        op.hwnd = hwnd;
        op.wFunc = FO_COPY;
        op.pFrom = paths;
        op.pTo = target->path;
        op.fFlags = FOF_ALLOWUNDO;
        SHFileOperationW(&op);
        break;
    case SEND_TO_PROGRAM:
        /* the program gets the items as arguments */
        for (p = paths; *p; p += wcslen(p) + 1) len += wcslen(p) + 3;
        if ((args = calloc(len + 1, sizeof(WCHAR))))
        {
            for (p = paths; *p; p += wcslen(p) + 1)
            {
                if (args[0]) wcscat(args, L" ");
                wcscat(args, L"\"");
                wcscat(args, p);
                wcscat(args, L"\"");
            }
            ShellExecuteW(hwnd, NULL, target->path, args, NULL, SW_SHOWNORMAL);
            free(args);
        }
        break;
    }
    free(paths);
}

/* Create Shortcut: next to each selected item */
static void do_create_shortcuts(ContextMenu *menu)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    ITEMIDLIST *pidl;
    UINT i;

    if (!SHGetPathFromIDListW(menu->pidl, dir)) return;
    for (i = 0; i < menu->cidl; i++)
    {
        if (!(pidl = ILCombine(menu->pidl, menu->apidl[i]))) continue;
        if (SHGetPathFromIDListW(pidl, path)) create_shortcut(path, dir);
        ILFree(pidl);
    }
}

/* Paste Shortcut: shortcuts to the files on the clipboard, in the folder of the background menu */
static HRESULT do_paste_shortcut(ContextMenu *menu)
{
    WCHAR dir[MAX_PATH], *paths = NULL;
    const WCHAR *path;
    ITEMIDLIST *pidl;
    IDataObject *data;
    STGMEDIUM medium;
    HRESULT hr;

    if (FAILED(hr = get_parent_pidl(menu, &pidl))) return hr;
    if (!SHGetPathFromIDListW(pidl, dir)) { ILFree(pidl); return E_FAIL; }
    ILFree(pidl);
    if (FAILED(hr = OleGetClipboard(&data))) return hr;

    if (SUCCEEDED(hr = get_data_format(data, RegisterClipboardFormatW(CFSTR_SHELLIDLISTW), &medium)))
    {
        const CIDA *cida = GlobalLock(medium.hGlobal);
        ITEMIDLIST **pidls, *root_pidl;

        pidls = _ILCopyCidaToaPidl(&root_pidl, cida);
        paths = build_source_paths(root_pidl, pidls, cida->cidl);
        _ILFreeaPidl(pidls, cida->cidl);
        ILFree(root_pidl);
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        for (path = paths; path && *path; path += wcslen(path) + 1) create_shortcut(path, dir);
        free(paths);
    }
    else if (SUCCEEDED(hr = get_data_format(data, CF_HDROP, &medium)))
    {
        const DROPFILES *dropfiles = GlobalLock(medium.hGlobal);

        for (path = (const WCHAR *)((const char *)dropfiles + dropfiles->pFiles); *path; path += wcslen(path) + 1)
            create_shortcut(path, dir);
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    IDataObject_Release(data);
    return hr;
}

static HRESULT do_paste(ContextMenu *menu, HWND hwnd)
{
    IPersistFolder2 *dst_persist;
    const DWORD *drop_effect;
    IShellFolder *dst_folder;
    WCHAR dst_path[MAX_PATH];
    SHFILEOPSTRUCTW op = {0};
    ITEMIDLIST *dst_pidl;
    IDataObject *data;
    HRESULT hr;
    STGMEDIUM medium;
    int ret;

    if (menu->cidl)
    {
        if (FAILED(hr = IShellFolder_BindToObject(menu->parent, menu->apidl[0],
                NULL, &IID_IShellFolder, (void **)&dst_folder)))
        {
            WARN("Failed to get destination folder, hr %#lx.\n", hr);
            return hr;
        }
    }
    else
    {
        dst_folder = menu->parent;
        IShellFolder_AddRef(dst_folder);
    }

    if (FAILED(hr = IShellFolder_QueryInterface(dst_folder, &IID_IPersistFolder2, (void **)&dst_persist)))
    {
        WARN("Failed to get IPersistFolder2, hr %#lx.\n", hr);
        return hr;
    }

    hr = IPersistFolder2_GetCurFolder(dst_persist, &dst_pidl);
    IPersistFolder2_Release(dst_persist);
    if (FAILED(hr))
    {
        ERR("Failed to get dst folder pidl, hr %#lx.\n", hr);
        return hr;
    }

    if (!SHGetPathFromIDListW(dst_pidl, dst_path))
    {
        ERR("Failed to get path, hr %#lx.\n", hr);
        ILFree(dst_pidl);
        return E_FAIL;
    }
    ILFree(dst_pidl);

    op.hwnd = hwnd;
    op.pTo = dst_path;
    op.fFlags = FOF_ALLOWUNDO;

    if (FAILED(hr = OleGetClipboard(&data)))
        return hr;

    if (FAILED(hr = get_data_format(data, RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECTW), &medium)))
    {
        ERR("Failed to get drop effect.\n");
        IDataObject_Release(data);
        return hr;
    }
    drop_effect = GlobalLock(medium.hGlobal);
    if (*drop_effect & DROPEFFECT_COPY)
        op.wFunc = FO_COPY;
    else if (*drop_effect & DROPEFFECT_MOVE)
        op.wFunc = FO_MOVE;
    else
        FIXME("Unhandled drop effect %#lx.\n", *drop_effect);
    GlobalUnlock(medium.hGlobal);

    if (SUCCEEDED(get_data_format(data, RegisterClipboardFormatW(CFSTR_SHELLIDLISTW), &medium)))
    {
        const CIDA *cida = GlobalLock(medium.hGlobal);
        ITEMIDLIST **pidls, *root_pidl;
        WCHAR *src_paths;

        pidls = _ILCopyCidaToaPidl(&root_pidl, cida);

        if ((src_paths = build_source_paths(root_pidl, pidls, cida->cidl)))
        {
            op.pFrom = src_paths;
            rename_copies_in_place(&op, src_paths);
            if ((ret = SHFileOperationW(&op)))
            {
                WARN("Failed to copy, ret %d.\n", ret);
                hr = E_FAIL;
            }
        }

        free(src_paths);
        _ILFreeaPidl(pidls, cida->cidl);
        ILFree(root_pidl);

        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    else if (SUCCEEDED(get_data_format(data, CF_HDROP, &medium)))
    {
        const DROPFILES *dropfiles = GlobalLock(medium.hGlobal);

        op.pFrom = (const WCHAR *)((const char *)dropfiles + dropfiles->pFiles);
        rename_copies_in_place(&op, op.pFrom);
        if ((ret = SHFileOperationW(&op)))
        {
            WARN("Failed to copy, ret %d.\n", ret);
            hr = E_FAIL;
        }

        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    else
    {
        ERR("Cannot paste any clipboard formats.\n");
        hr = E_FAIL;
    }

    IDataObject_Release(data);
    return hr;
}

/**************************************************************************
 * Properties_AddPropSheetCallback
 *
 * Used by DoOpenProperties through SHCreatePropSheetExtArrayEx to add
 * propertysheet pages from shell extensions.
 */
static BOOL CALLBACK Properties_AddPropSheetCallback(HPROPSHEETPAGE hpage, LPARAM lparam)
{
	LPPROPSHEETHEADERW psh = (LPPROPSHEETHEADERW) lparam;
	psh->phpage[psh->nPages++] = hpage;

	return TRUE;
}

static BOOL format_date(FILETIME *time, WCHAR *buffer, DWORD size)
{
    FILETIME ft;
    SYSTEMTIME st;
    int ret;

    if (!FileTimeToLocalFileTime(time, &ft))
        return FALSE;

    if (!FileTimeToSystemTime(&ft, &st))
        return FALSE;

    /* Windows XP: "Friday, October 09, 2026, 8:52:30 PM" */
    ret = GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, L"dddd, MMMM dd, yyyy", buffer, size);
    if (ret && ret + 1 < size)
    {
        buffer[ret - 1] = ',';
        buffer[ret] = ' ';
        ret = GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, buffer + ret + 1, size - ret - 1);
    }
    return ret != 0;
}

static BOOL get_program_description(WCHAR *path, WCHAR *buffer, DWORD size)
{
    WCHAR fileDescW[MAX_PATH + 32], *desc;
    DWORD versize, *lang;
    UINT dlen, llen, i;
    BOOL ret = FALSE;
    PVOID data;

    /* the name the program is registered with, as Open With shows it */
    swprintf(fileDescW, ARRAY_SIZE(fileDescW), L"Applications\\%s", PathFindFileNameW(path));
    dlen = size * sizeof(WCHAR);
    if (!RegGetValueW(HKEY_CLASSES_ROOT, fileDescW, L"FriendlyAppName", RRF_RT_REG_SZ, NULL, buffer, (DWORD *)&dlen))
        return TRUE;

    versize = GetFileVersionInfoSizeW(path, NULL);
    if (!versize) return FALSE;

    data = malloc(versize);
    if (!data) return FALSE;

    if (!GetFileVersionInfoW(path, 0, versize, data))
        goto out;

    if (!VerQueryValueW(data, L"\\VarFileInfo\\Translation", (LPVOID *)&lang, &llen))
        goto out;

    for (i = 0; i < llen / sizeof(DWORD); i++)
    {
        swprintf(fileDescW, ARRAY_SIZE(fileDescW), L"\\StringFileInfo\\%04x%04x\\FileDescription",
                 LOWORD(lang[i]), HIWORD(lang[i]));
        if (VerQueryValueW(data, fileDescW, (LPVOID *)&desc, &dlen))
        {
            if (dlen > size - 1) dlen = size - 1;
            memcpy(buffer, desc, dlen * sizeof(WCHAR));
            buffer[dlen] = 0;
            ret = TRUE;
            break;
        }
    }

out:
    free(data);
    return ret;
}

/* "6 bytes (6 bytes)", "1.37 MB (1,441,792 bytes)" */
static void format_size(ULONGLONG size, WCHAR *buffer, UINT len)
{
    WCHAR bytes[32], number[64], *short_size = malloc(len * sizeof(WCHAR));
    NUMBERFMTW format = { 0, 0, 3, (WCHAR *)L".", (WCHAR *)L",", 0 };

    swprintf(bytes, ARRAY_SIZE(bytes), L"%I64u", size);
    if (!GetNumberFormatW(LOCALE_USER_DEFAULT, 0, bytes, &format, number, ARRAY_SIZE(number)))
        lstrcpyW(number, bytes);
    if (!short_size) return;
    StrFormatByteSizeW(size, short_size, len);
    swprintf(buffer, len, L"%s (%s bytes)", short_size, number);
    free(short_size);
}

struct file_properties_info
{
    WCHAR path[MAX_PATH];
    WCHAR dir[MAX_PATH];
    WCHAR *filename;
    DWORD attrib;
    HICON icon, program_icon;   /* shown by the page, destroyed with it */
};

static void init_file_properties_dlg(HWND hwndDlg, struct file_properties_info *props)
{
    WCHAR buffer[MAX_PATH], buffer2[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA exinfo;
    SHFILEINFOW shinfo;

    SetDlgItemTextW(hwndDlg, IDC_FPROP_PATH, props->filename);
    SetDlgItemTextW(hwndDlg, IDC_FPROP_LOCATION, props->dir);

    if (SHGetFileInfoW(props->path, 0, &shinfo, sizeof(shinfo), SHGFI_TYPENAME|SHGFI_ICON))
    {
        if (shinfo.hIcon)
        {
            /* the static keeps the handle: it is destroyed with the page */
            SendDlgItemMessageW(hwndDlg, IDC_FPROP_ICON, STM_SETICON, (WPARAM)shinfo.hIcon, 0);
            if (props->icon) DestroyIcon(props->icon);
            props->icon = shinfo.hIcon;
        }
        if (shinfo.szTypeName[0])
            SetDlgItemTextW(hwndDlg, IDC_FPROP_TYPE, shinfo.szTypeName);
    }

    if (!GetFileAttributesExW(props->path, GetFileExInfoStandard, &exinfo))
        return;

    if (format_date(&exinfo.ftCreationTime, buffer, ARRAY_SIZE(buffer)))
        SetDlgItemTextW(hwndDlg, IDC_FPROP_CREATED, buffer);

    if (exinfo.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        SendDlgItemMessageW(hwndDlg, IDC_FPROP_READONLY, BM_SETCHECK, BST_CHECKED, 0);
    if (exinfo.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
        SendDlgItemMessageW(hwndDlg, IDC_FPROP_HIDDEN, BM_SETCHECK, BST_CHECKED, 0);
    if (exinfo.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
        SendDlgItemMessageW(hwndDlg, IDC_FPROP_ARCHIVE, BM_SETCHECK, BST_CHECKED, 0);

    if (exinfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        SetDlgItemTextW(hwndDlg, IDC_FPROP_SIZE, L"(unknown)");
        /* TODO: Implement counting for directories */
        return;
    }

    /* Information about files only: "1.37 MB (1,441,792 bytes)" and the clusters it takes */
    {
        ULONGLONG size = ((ULONGLONG)exinfo.nFileSizeHigh << 32) | exinfo.nFileSizeLow, disk = size;
        DWORD sectors, bytes, free_clusters, clusters;
        WCHAR root[MAX_PATH];

        format_size(size, buffer, ARRAY_SIZE(buffer));
        SetDlgItemTextW(hwndDlg, IDC_FPROP_SIZE, buffer);
        lstrcpynW(root, props->path, ARRAY_SIZE(root));
        PathStripToRootW(root);
        PathAddBackslashW(root);
        if (GetDiskFreeSpaceW(root, &sectors, &bytes, &free_clusters, &clusters) && sectors && bytes)
            disk = (size + sectors * bytes - 1) / (sectors * bytes) * (sectors * bytes);
        format_size(disk, buffer, ARRAY_SIZE(buffer));
        SetDlgItemTextW(hwndDlg, IDC_FPROP_SIZEONDISK, buffer);
    }

    if (format_date(&exinfo.ftLastWriteTime, buffer, ARRAY_SIZE(buffer)))
        SetDlgItemTextW(hwndDlg, IDC_FPROP_MODIFIED, buffer);
    if (format_date(&exinfo.ftLastAccessTime, buffer, ARRAY_SIZE(buffer)))
        SetDlgItemTextW(hwndDlg, IDC_FPROP_ACCESSED, buffer);

    if (FindExecutableW(props->path, NULL, buffer) <= (HINSTANCE)32)
        return;

    /* Information about executables */
    if (SHGetFileInfoW(buffer, 0, &shinfo, sizeof(shinfo), SHGFI_ICON | SHGFI_SMALLICON) && shinfo.hIcon)
    {
        SendDlgItemMessageW(hwndDlg, IDC_FPROP_PROG_ICON, STM_SETICON, (WPARAM)shinfo.hIcon, 0);
        if (props->program_icon) DestroyIcon(props->program_icon);
        props->program_icon = shinfo.hIcon;
    }

    if (get_program_description(buffer, buffer2, ARRAY_SIZE(buffer2)))
        SetDlgItemTextW(hwndDlg, IDC_FPROP_PROG_NAME, buffer2);
    else
    {
        WCHAR *p = wcsrchr(buffer, '\\');
        SetDlgItemTextW(hwndDlg, IDC_FPROP_PROG_NAME, p ? ++p : buffer);
    }
}

static INT_PTR CALLBACK file_properties_proc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            LPPROPSHEETPAGEW page = (LPPROPSHEETPAGEW)lParam;
            SetWindowLongPtrW(hwndDlg, DWLP_USER, (LONG_PTR)page->lParam);
            init_file_properties_dlg(hwndDlg, (struct file_properties_info *)page->lParam);
            break;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_FPROP_PROG_CHANGE)
            {
                /* Windows XP's Change...: Open With, always used for this kind of file */
                struct file_properties_info *props = (struct file_properties_info *)GetWindowLongPtrW(hwndDlg, DWLP_USER);
                OPENASINFO info = { props->path, NULL, OAIF_ALLOW_REGISTRATION | OAIF_FORCE_REGISTRATION | OAIF_REGISTER_EXT };

                if (SHOpenWithDialog(hwndDlg, &info) == S_OK)
                    init_file_properties_dlg(hwndDlg, props);
            }
            else if (LOWORD(wParam) == IDC_FPROP_READONLY ||
                     LOWORD(wParam) == IDC_FPROP_HIDDEN ||
                     LOWORD(wParam) == IDC_FPROP_ARCHIVE)
            {
                SendMessageW(GetParent(hwndDlg), PSM_CHANGED, (WPARAM)hwndDlg, 0);
            }
            else if (LOWORD(wParam) == IDC_FPROP_PATH && HIWORD(wParam) == EN_CHANGE)
            {
                SendMessageW(GetParent(hwndDlg), PSM_CHANGED, (WPARAM)hwndDlg, 0);
            }
            break;

        case WM_NOTIFY:
            {
                LPPSHNOTIFY notify = (LPPSHNOTIFY)lParam;
                if (notify->hdr.code == PSN_APPLY)
                {
                    struct file_properties_info *props = (struct file_properties_info *)GetWindowLongPtrW(hwndDlg, DWLP_USER);
                    WCHAR newname[MAX_PATH], newpath[MAX_PATH];
                    DWORD attributes;

                    attributes = GetFileAttributesW(props->path);
                    if (attributes != INVALID_FILE_ATTRIBUTES)
                    {
                        attributes &= ~(FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_ARCHIVE);

                        if (SendDlgItemMessageW(hwndDlg, IDC_FPROP_READONLY, BM_GETCHECK, 0, 0) == BST_CHECKED)
                            attributes |= FILE_ATTRIBUTE_READONLY;
                        if (SendDlgItemMessageW(hwndDlg, IDC_FPROP_HIDDEN, BM_GETCHECK, 0, 0) == BST_CHECKED)
                            attributes |= FILE_ATTRIBUTE_HIDDEN;
                        if (SendDlgItemMessageW(hwndDlg, IDC_FPROP_ARCHIVE, BM_GETCHECK, 0, 0) == BST_CHECKED)
                            attributes |= FILE_ATTRIBUTE_ARCHIVE;

                        if (!SetFileAttributesW(props->path, attributes))
                            ERR("failed to update file attributes of %s\n", debugstr_w(props->path));
                    }

                    /* Update filename if it was changed */
                    if (GetDlgItemTextW(hwndDlg, IDC_FPROP_PATH, newname, ARRAY_SIZE(newname)) &&
                        wcscmp(props->filename, newname) &&
                        lstrlenW(props->dir) + lstrlenW(newname) + 2 < ARRAY_SIZE(newpath))
                    {
                        lstrcpyW(newpath, props->dir);
                        lstrcatW(newpath, L"\\");
                        lstrcatW(newpath, newname);

                        if (!MoveFileW(props->path, newpath))
                            ERR("failed to move file %s to %s\n", debugstr_w(props->path), debugstr_w(newpath));
                        else
                        {
                            WCHAR *p;
                            lstrcpyW(props->path, newpath);
                            lstrcpyW(props->dir, newpath);
                            if ((p = wcsrchr(props->dir, '\\')))
                            {
                                *p = 0;
                                props->filename = p + 1;
                            }
                            else
                                props->filename = props->dir;
                            SetDlgItemTextW(hwndDlg, IDC_FPROP_LOCATION, props->dir);
                        }
                    }

                    return TRUE;
                }
            }
            break;

        default:
            break;
    }
    return FALSE;
}

static UINT CALLBACK file_properties_callback(HWND hwnd, UINT uMsg, LPPROPSHEETPAGEW page)
{
    struct file_properties_info *props = (struct file_properties_info *)page->lParam;
    if (uMsg == PSPCB_RELEASE)
    {
        if (props->icon) DestroyIcon(props->icon);
        if (props->program_icon) DestroyIcon(props->program_icon);
        free(props);
    }
    return 1;
}

static void init_file_properties_pages(IDataObject *dataobject, LPFNADDPROPSHEETPAGE lpfnAddPage, LPARAM lParam)
{
    struct file_properties_info *props;
    HPROPSHEETPAGE general_page;
    PROPSHEETPAGEW propsheet;
    FORMATETC format;
    STGMEDIUM stgm;
    HRESULT hr;
    WCHAR *p;

    props = calloc(1, sizeof(*props));
    if (!props) return;

    format.cfFormat = CF_HDROP;
    format.ptd      = NULL;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex   = -1;
    format.tymed    = TYMED_HGLOBAL;

    hr = IDataObject_GetData(dataobject, &format, &stgm);
    if (FAILED(hr)) goto error;

    if (!DragQueryFileW((HDROP)stgm.hGlobal, 0, props->path, ARRAY_SIZE(props->path)))
    {
        ReleaseStgMedium(&stgm);
        goto error;
    }

    ReleaseStgMedium(&stgm);

    props->attrib = GetFileAttributesW(props->path);
    if (props->attrib == INVALID_FILE_ATTRIBUTES)
        goto error;

    lstrcpyW(props->dir, props->path);
    if ((p = wcsrchr(props->dir, '\\')))
    {
        *p = 0;
        props->filename = p + 1;
    }
    else
        props->filename = props->dir;

    memset(&propsheet, 0, sizeof(propsheet));
    propsheet.dwSize        = sizeof(propsheet);
    propsheet.dwFlags       = PSP_DEFAULT | PSP_USECALLBACK;
    propsheet.hInstance     = shell32_hInstance;
    if (props->attrib & FILE_ATTRIBUTE_DIRECTORY)
        propsheet.pszTemplate = (LPWSTR)MAKEINTRESOURCE(IDD_FOLDER_PROPERTIES);
    else
        propsheet.pszTemplate = (LPWSTR)MAKEINTRESOURCE(IDD_FILE_PROPERTIES);
    propsheet.pfnDlgProc    = file_properties_proc;
    propsheet.pfnCallback   = file_properties_callback;
    propsheet.lParam        = (LPARAM)props;

    general_page = CreatePropertySheetPageW(&propsheet);
    if (general_page)
        lpfnAddPage(general_page, lParam);
    return;

error:
    free(props);
}

static void get_filetype(LPCITEMIDLIST pidl, WCHAR filetype[MAX_PATH])
{
    if (_ILIsValue(pidl))
    {
        char ext[64], filetypeA[64];

        if (_ILGetExtension(pidl, ext, 64))
        {
            HCR_MapTypeToValueA(ext, filetypeA, 64, TRUE);
            MultiByteToWideChar(CP_ACP, 0, filetypeA, -1, filetype, MAX_PATH);
        }
        else
        {
            filetype[0] = 0;
        }
    }
    else if (_ILIsFolder(pidl))
    {
        wcscpy(filetype, L"Folder");
    }
    else if (_ILIsSpecialFolder(pidl))
    {
        GUID *guid = _ILGetGUIDPointer(pidl);

        wcscpy(filetype, L"CLSID\\");
        StringFromGUID2(guid, &filetype[6], MAX_PATH - 6);
    }
    else
    {
        FIXME("Unknown pidl type.\n");
    }
}

#define MAX_PROP_PAGES 99

static void DoOpenProperties(ContextMenu *This, HWND hwnd)
{
	LPSHELLFOLDER lpDesktopSF;
	LPSHELLFOLDER lpSF;
	LPDATAOBJECT lpDo;
	WCHAR wszFiletype[MAX_PATH];
	WCHAR wszFilename[MAX_PATH];
	PROPSHEETHEADERW psh;
	HPROPSHEETPAGE hpages[MAX_PROP_PAGES];
	HPSXA hpsxa;
	UINT ret;

	TRACE("(%p)->(wnd=%p)\n", This, hwnd);

	ZeroMemory(&psh, sizeof(PROPSHEETHEADERW));
	psh.dwSize = sizeof (PROPSHEETHEADERW);
	psh.hwndParent = hwnd;
	psh.dwFlags = PSH_PROPTITLE;
	psh.nPages = 0;
	psh.phpage = hpages;
	psh.nStartPage = 0;

	/* "test Properties": the name the shell shows */
	{
	    STRRET name;

	    if (FAILED(IShellFolder_GetDisplayNameOf(This->parent, This->apidl[0], SHGDN_INFOLDER, &name)) ||
	        FAILED(StrRetToBufW(&name, This->apidl[0], wszFilename, MAX_PATH)))
	        _ILSimpleGetTextW(This->apidl[0], (LPVOID)wszFilename, MAX_PATH);
	}
	psh.pszCaption = (LPCWSTR)wszFilename;

    get_filetype(This->apidl[0], wszFiletype);

	/* Get a suitable DataObject for accessing the files */
	SHGetDesktopFolder(&lpDesktopSF);
	if (_ILIsPidlSimple(This->pidl))
	{
	    ret = IShellFolder_GetUIObjectOf(lpDesktopSF, hwnd, This->cidl, (LPCITEMIDLIST*)This->apidl,
					     &IID_IDataObject, NULL, (LPVOID *)&lpDo);
	    IShellFolder_Release(lpDesktopSF);
	}
	else
	{
	    IShellFolder_BindToObject(lpDesktopSF, This->pidl, NULL, &IID_IShellFolder, (LPVOID*) &lpSF);
	    ret = IShellFolder_GetUIObjectOf(lpSF, hwnd, This->cidl, (LPCITEMIDLIST*)This->apidl,
					     &IID_IDataObject, NULL, (LPVOID *)&lpDo);
	    IShellFolder_Release(lpSF);
	    IShellFolder_Release(lpDesktopSF);
	}

	if (SUCCEEDED(ret))
	{
            init_file_properties_pages(lpDo, Properties_AddPropSheetCallback, (LPARAM)&psh);

	    hpsxa = SHCreatePropSheetExtArrayEx(HKEY_CLASSES_ROOT, wszFiletype, MAX_PROP_PAGES - psh.nPages, lpDo);
	    if (hpsxa != NULL)
	    {
		SHAddFromPropSheetExtArray(hpsxa, Properties_AddPropSheetCallback, (LPARAM)&psh);
		SHDestroyPropSheetExtArray(hpsxa);
	    }
	    hpsxa = SHCreatePropSheetExtArrayEx(HKEY_CLASSES_ROOT, L"*", MAX_PROP_PAGES - psh.nPages, lpDo);
	    if (hpsxa != NULL)
	    {
		SHAddFromPropSheetExtArray(hpsxa, Properties_AddPropSheetCallback, (LPARAM)&psh);
		SHDestroyPropSheetExtArray(hpsxa);
	    }
	    IDataObject_Release(lpDo);
	}

	if (psh.nPages)
	    PropertySheetW(&psh);
	else
	    FIXME("No property pages found.\n");
}

static HRESULT WINAPI ItemMenu_InvokeCommand(
	IContextMenu3 *iface,
	LPCMINVOKECOMMANDINFO lpcmi)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);

    if (lpcmi->cbSize != sizeof(CMINVOKECOMMANDINFO))
        FIXME("Is an EX structure\n");

    TRACE("(%p)->(invcom=%p verb=%p wnd=%p)\n",This,lpcmi,lpcmi->lpVerb, lpcmi->hwnd);

    if (IS_INTRESOURCE(lpcmi->lpVerb) && LOWORD(lpcmi->lpVerb) > FCIDM_SHVIEWLAST)
    {
        TRACE("Invalid Verb %x\n", LOWORD(lpcmi->lpVerb));
        return E_INVALIDARG;
    }

    if (IS_INTRESOURCE(lpcmi->lpVerb))
    {
        unsigned int id = LOWORD(lpcmi->lpVerb);

        if (id >= OPENWITH_ID_OFFSET && id - OPENWITH_ID_OFFSET < open_with_menu_count(This->open_with))
        {
            WCHAR path[MAX_PATH];

            if (get_item_path(This, path))
                open_with_menu_invoke(This->open_with, lpcmi->hwnd, id - OPENWITH_ID_OFFSET, path);
            return S_OK;
        }
        if (id >= SENDTO_ID_OFFSET && id - SENDTO_ID_OFFSET < This->send_to_count)
        {
            do_send_to(This, lpcmi->hwnd, id - SENDTO_ID_OFFSET);
            return S_OK;
        }

        if (id >= VERB_ID_OFFSET && id - VERB_ID_OFFSET < This->verb_count)
        {
            execute_verb(This, lpcmi->hwnd, This->verbs[id - VERB_ID_OFFSET].verb);
            return S_OK;
        }

        switch (id + FCIDM_BASE)
        {
        case FCIDM_SHVIEW_RENAME:
        {
            IShellBrowser *browser;

            /* get the active IShellView */
            browser = (IShellBrowser*)SendMessageA(lpcmi->hwnd, CWM_GETISHELLBROWSER, 0, 0);
            if (browser)
            {
                IShellView *view;

                if(SUCCEEDED(IShellBrowser_QueryActiveShellView(browser, &view)))
                {
                    TRACE("(shellview=%p)\n", view);
                    IShellView_SelectItem(view, This->apidl[0],
                         SVSI_DESELECTOTHERS|SVSI_EDIT|SVSI_ENSUREVISIBLE|SVSI_FOCUSED|SVSI_SELECT);
                    IShellView_Release(view);
                }
            }
            break;
        }
        case FCIDM_SHVIEW_DELETE:
            TRACE("Verb FCIDM_SHVIEW_DELETE\n");
            DoDelete(This);
            break;
        case FCIDM_SHVIEW_COPY:
            TRACE("Verb FCIDM_SHVIEW_COPY\n");
            do_copy(This, lpcmi->hwnd, DROPEFFECT_COPY | DROPEFFECT_LINK);
            break;
        case FCIDM_SHVIEW_CUT:
            TRACE("Verb FCIDM_SHVIEW_CUT\n");
            do_copy(This, lpcmi->hwnd, DROPEFFECT_MOVE);
            break;
        case FCIDM_SHVIEW_INSERT:
            do_paste(This, lpcmi->hwnd);
            break;
        case FCIDM_SHVIEW_CREATELINK:
            do_create_shortcuts(This);
            break;
        case FCIDM_SHVIEW_PROPERTIES:
            TRACE("Verb FCIDM_SHVIEW_PROPERTIES\n");
            DoOpenProperties(This, lpcmi->hwnd);
            break;
        default:
            FIXME("Unhandled verb %#x.\n", id);
            return E_INVALIDARG;
        }
    }
    else
    {
        TRACE("Verb is %s\n",debugstr_a(lpcmi->lpVerb));
        if (strcmp(lpcmi->lpVerb,"delete")==0)
            DoDelete(This);
        else if (strcmp(lpcmi->lpVerb,"copy")==0)
            do_copy(This, lpcmi->hwnd, DROPEFFECT_COPY | DROPEFFECT_LINK);
        else if (strcmp(lpcmi->lpVerb,"cut")==0)
            do_copy(This, lpcmi->hwnd, DROPEFFECT_MOVE);
        else if (!strcmp(lpcmi->lpVerb, "paste"))
            do_paste(This, lpcmi->hwnd);
        else if (!strcmp(lpcmi->lpVerb, "link"))
            do_create_shortcuts(This);
        else if (strcmp(lpcmi->lpVerb,"properties")==0)
            DoOpenProperties(This, lpcmi->hwnd);
        else {
            FIXME("Unhandled string verb %s\n",debugstr_a(lpcmi->lpVerb));
            return E_FAIL;
        }
    }
    return S_OK;
}

static HRESULT WINAPI ItemMenu_GetCommandString(IContextMenu3 *iface, UINT_PTR cmdid, UINT flags,
    UINT *reserved, LPSTR name, UINT maxlen)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    const WCHAR *cmdW = NULL;
    HRESULT hr = S_OK;

    TRACE("(%p)->(%Ix, %#x, %p, %p, %u)\n", This, cmdid, flags, reserved, name, maxlen);

    switch (flags)
    {
    case GCS_HELPTEXTA:
    case GCS_HELPTEXTW:
        hr = E_NOTIMPL;
        break;

    case GCS_VERBA:
    case GCS_VERBW:
        switch (cmdid + FCIDM_BASE)
        {
        case FCIDM_SHVIEW_CUT:
            cmdW = L"cut";
            break;
        case FCIDM_SHVIEW_COPY:
            cmdW = L"copy";
            break;
        case FCIDM_SHVIEW_CREATELINK:
            cmdW = L"link";
            break;
        case FCIDM_SHVIEW_DELETE:
            cmdW = L"delete";
            break;
        case FCIDM_SHVIEW_INSERT:
            cmdW = L"paste";
            break;
        case FCIDM_SHVIEW_PROPERTIES:
            cmdW = L"properties";
            break;
        case FCIDM_SHVIEW_RENAME:
            cmdW = L"rename";
            break;
        }

        if (cmdid >= VERB_ID_OFFSET && cmdid - VERB_ID_OFFSET < This->verb_count)
            cmdW = This->verbs[cmdid - VERB_ID_OFFSET].verb;

        if (!cmdW)
        {
            hr = E_INVALIDARG;
            break;
        }

        if (flags == GCS_VERBA)
            WideCharToMultiByte(CP_ACP, 0, cmdW, -1, name, maxlen, NULL, NULL);
        else
            lstrcpynW((WCHAR *)name, cmdW, maxlen);

        TRACE("name %s\n", flags == GCS_VERBA ? debugstr_a(name) : debugstr_w((WCHAR *)name));
        break;

    case GCS_VALIDATEA:
    case GCS_VALIDATEW:
        break;
    }

    return hr;
}

/**************************************************************************
* NOTES
*  should be only in IContextMenu2 and IContextMenu3
*  is nevertheless called from word95
*/
static HRESULT WINAPI ContextMenu_HandleMenuMsg(IContextMenu3 *iface, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    FIXME("(%p)->(0x%x 0x%Ix 0x%Ix): stub\n", This, msg, wParam, lParam);
    return E_NOTIMPL;
}

static HRESULT WINAPI ContextMenu_HandleMenuMsg2(IContextMenu3 *iface, UINT msg,
    WPARAM wParam, LPARAM lParam, LRESULT *result)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    FIXME("(%p)->(0x%x 0x%Ix 0x%Ix %p): stub\n", This, msg, wParam, lParam, result);
    return E_NOTIMPL;
}

static const IContextMenu3Vtbl ItemContextMenuVtbl =
{
    ContextMenu_QueryInterface,
    ContextMenu_AddRef,
    ContextMenu_Release,
    ItemMenu_QueryContextMenu,
    ItemMenu_InvokeCommand,
    ItemMenu_GetCommandString,
    ContextMenu_HandleMenuMsg,
    ContextMenu_HandleMenuMsg2
};

static HRESULT WINAPI ShellExtInit_QueryInterface(IShellExtInit *iface, REFIID riid, void **obj)
{
    ContextMenu *This = impl_from_IShellExtInit(iface);
    return IContextMenu3_QueryInterface(&This->IContextMenu3_iface, riid, obj);
}

static ULONG WINAPI ShellExtInit_AddRef(IShellExtInit *iface)
{
    ContextMenu *This = impl_from_IShellExtInit(iface);
    return IContextMenu3_AddRef(&This->IContextMenu3_iface);
}

static ULONG WINAPI ShellExtInit_Release(IShellExtInit *iface)
{
    ContextMenu *This = impl_from_IShellExtInit(iface);
    return IContextMenu3_Release(&This->IContextMenu3_iface);
}

static HRESULT WINAPI ShellExtInit_Initialize(IShellExtInit *iface, LPCITEMIDLIST folder,
    IDataObject *dataobj, HKEY progidkey)
{
    ContextMenu *This = impl_from_IShellExtInit(iface);

    FIXME("(%p)->(%p %p %p): stub\n", This, folder, dataobj, progidkey);

    return E_NOTIMPL;
}

static const IShellExtInitVtbl ShellExtInitVtbl =
{
    ShellExtInit_QueryInterface,
    ShellExtInit_AddRef,
    ShellExtInit_Release,
    ShellExtInit_Initialize
};

static HRESULT WINAPI ObjectWithSite_QueryInterface(IObjectWithSite *iface, REFIID riid, void **obj)
{
    ContextMenu *This = impl_from_IObjectWithSite(iface);
    return IContextMenu3_QueryInterface(&This->IContextMenu3_iface, riid, obj);
}

static ULONG WINAPI ObjectWithSite_AddRef(IObjectWithSite *iface)
{
    ContextMenu *This = impl_from_IObjectWithSite(iface);
    return IContextMenu3_AddRef(&This->IContextMenu3_iface);
}

static ULONG WINAPI ObjectWithSite_Release(IObjectWithSite *iface)
{
    ContextMenu *This = impl_from_IObjectWithSite(iface);
    return IContextMenu3_Release(&This->IContextMenu3_iface);
}

static HRESULT WINAPI ObjectWithSite_SetSite(IObjectWithSite *iface, IUnknown *site)
{
    ContextMenu *This = impl_from_IObjectWithSite(iface);

    FIXME("(%p)->(%p): stub\n", This, site);

    return E_NOTIMPL;
}

static HRESULT WINAPI ObjectWithSite_GetSite(IObjectWithSite *iface, REFIID riid, void **site)
{
    ContextMenu *This = impl_from_IObjectWithSite(iface);

    FIXME("(%p)->(%s %p): stub\n", This, debugstr_guid(riid), site);

    return E_NOTIMPL;
}

static const IObjectWithSiteVtbl ObjectWithSiteVtbl =
{
    ObjectWithSite_QueryInterface,
    ObjectWithSite_AddRef,
    ObjectWithSite_Release,
    ObjectWithSite_SetSite,
    ObjectWithSite_GetSite,
};

static WCHAR *get_verb_desc(HKEY key, const WCHAR *verb)
{
    DWORD size = 0;
    WCHAR *desc;
    DWORD ret;

    static const struct
    {
        const WCHAR *verb;
        unsigned int id;
    }
    builtin_verbs[] =
    {
        {L"explore", IDS_VERB_EXPLORE},
        {L"open", IDS_VERB_OPEN},
        {L"print", IDS_VERB_PRINT},
        {L"runas", IDS_VERB_RUNAS},
    };

    if ((ret = RegGetValueW(key, verb, NULL, RRF_RT_REG_SZ, NULL, NULL, &size)) == ERROR_MORE_DATA)
    {
        desc = malloc(size);
        RegGetValueW(key, verb, NULL, RRF_RT_REG_SZ, NULL, desc, &size);
        return desc;
    }

    /* Some verbs appear to have builtin descriptions on Windows, which aren't
     * stored in the registry. */

    for (unsigned int i = 0; i < ARRAY_SIZE(builtin_verbs); ++i)
    {
        if (!wcscmp(verb, builtin_verbs[i].verb))
            return shell_get_resource_string(builtin_verbs[i].id);
    }

    return wcsdup(verb);
}

static void build_verb_list(ContextMenu *menu)
{
    HKEY type_key, shell_key;
    size_t verb_capacity;
    WCHAR *verb_buffer;
    DWORD ret;

    if (!menu->cidl)
        return;

    get_filetype(menu->apidl[0], menu->filetype);

    /* If all of the files are not of the same type, we can't do anything
     * with them. */
    if (menu->cidl > 1)
    {
        WCHAR other_filetype[MAX_PATH];

        for (unsigned int i = 1; i < menu->cidl; ++i)
        {
            get_filetype(menu->apidl[i], other_filetype);
            if (wcscmp(menu->filetype, other_filetype))
                return;
        }
    }

    if ((ret = RegOpenKeyExW(HKEY_CLASSES_ROOT, menu->filetype, 0, KEY_READ, &type_key)))
        return;

    if ((ret = RegOpenKeyExW(type_key, L"shell", 0, KEY_READ, &shell_key)))
    {
        RegCloseKey(type_key);
        return;
    }

    verb_capacity = 256;
    verb_buffer = malloc(verb_capacity * sizeof(WCHAR));
    for (unsigned int i = 0; ; ++i)
    {
        DWORD size = verb_capacity;
        WCHAR *desc;

        ret = RegEnumKeyExW(shell_key, i, verb_buffer, &size, NULL, NULL, NULL, NULL);
        if (ret == ERROR_MORE_DATA)
        {
            verb_capacity = size;
            verb_buffer = realloc(verb_buffer, verb_capacity * sizeof(WCHAR));
            ret = RegEnumKeyExW(shell_key, i, verb_buffer, &size, NULL, NULL, NULL, NULL);
        }
        if (ret)
            break;

        if (!(desc = get_verb_desc(shell_key, verb_buffer)))
            continue;

        menu->verbs = realloc(menu->verbs, (menu->verb_count + 1) * sizeof(*menu->verbs));
        menu->verbs[menu->verb_count].verb = wcsdup(verb_buffer);
        menu->verbs[menu->verb_count].desc = desc;
        ++menu->verb_count;

        TRACE("Found verb %s, description %s.\n", debugstr_w(verb_buffer), debugstr_w(desc));
    }

    RegCloseKey(shell_key);

    /* TODO: Enumerate the shellex key as well. */

    RegCloseKey(type_key);
}

HRESULT ItemMenu_Constructor(IShellFolder *parent, LPCITEMIDLIST pidl, const LPCITEMIDLIST *apidl, UINT cidl,
    REFIID riid, void **pObj)
{
    ContextMenu* This;
    HRESULT hr;

    if (!(This = calloc(1, sizeof(*This))))
        return E_OUTOFMEMORY;

    This->IContextMenu3_iface.lpVtbl = &ItemContextMenuVtbl;
    This->IShellExtInit_iface.lpVtbl = &ShellExtInitVtbl;
    This->IObjectWithSite_iface.lpVtbl = &ObjectWithSiteVtbl;
    This->ref = 1;
    This->parent = parent;
    if (parent) IShellFolder_AddRef(parent);

    This->pidl = ILClone(pidl);
    This->apidl = _ILCopyaPidl(apidl, cidl);
    This->cidl = cidl;

    This->desktop = FALSE;

    build_verb_list(This);

    hr = IContextMenu3_QueryInterface(&This->IContextMenu3_iface, riid, pObj);
    IContextMenu3_Release(&This->IContextMenu3_iface);

    return hr;
}

/* Background menu implementation */
static HRESULT WINAPI BackgroundMenu_QueryContextMenu(
	IContextMenu3 *iface,
	HMENU hMenu,
	UINT indexMenu,
	UINT idCmdFirst,
	UINT idCmdLast,
	UINT uFlags)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    HMENU hMyMenu;
    UINT idMax;
    HRESULT hr;

    TRACE("(%p)->(hmenu=%p indexmenu=%x cmdfirst=%x cmdlast=%x flags=%x )\n",
          This, hMenu, indexMenu, idCmdFirst, idCmdLast, uFlags);

    hMyMenu = LoadMenuA(shell32_hInstance, "MENU_002");
    if (uFlags & CMF_DEFAULTONLY)
    {
        HMENU ourMenu = GetSubMenu(hMyMenu,0);
        UINT oldDef = GetMenuDefaultItem(hMenu,TRUE,GMDI_USEDISABLED);
        UINT newDef = GetMenuDefaultItem(ourMenu,TRUE,GMDI_USEDISABLED);
        if (newDef != oldDef)
            SetMenuDefaultItem(hMenu,newDef,TRUE);
        if (newDef!=0xFFFFFFFF)
            hr =  MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, newDef+1);
        else
            hr =  MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
    }
    else
    {
        Shell_MergeMenus (hMenu, GetSubMenu(hMyMenu,0), indexMenu,
                          idCmdFirst - FCIDM_BASE, idCmdLast, MM_SUBMENUSHAVEIDS);
        idMax = max_menu_id(GetSubMenu(hMyMenu, 0), idCmdFirst - FCIDM_BASE, idCmdLast);
        hr =  MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, idMax-idCmdFirst);
    }
    DestroyMenu(hMyMenu);

    TRACE("(%p)->returning 0x%lx\n",This,hr);
    return hr;
}

static void DoNewFolder(ContextMenu *This, IShellView *view)
{
    ISFHelper *helper;

    IShellFolder_QueryInterface(This->parent, &IID_ISFHelper, (void**)&helper);
    if (helper)
    {
        WCHAR nameW[MAX_PATH];
        LPITEMIDLIST pidl;

        ISFHelper_GetUniqueName(helper, nameW, MAX_PATH);
        ISFHelper_AddFolder(helper, 0, nameW, &pidl);

        if (view)
        {
	    /* if we are in a shellview do labeledit */
	    IShellView_SelectItem(view,
                    pidl,(SVSI_DESELECTOTHERS | SVSI_EDIT | SVSI_ENSUREVISIBLE
                    |SVSI_FOCUSED|SVSI_SELECT));
        }

        SHFree(pidl);
        ISFHelper_Release(helper);
    }
}

static HRESULT WINAPI BackgroundMenu_InvokeCommand(
	IContextMenu3 *iface,
	LPCMINVOKECOMMANDINFO lpcmi)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    IShellBrowser *browser;
    IShellView *view = NULL;
    HWND hWnd = NULL;

    TRACE("(%p)->(invcom=%p verb=%p wnd=%p)\n", This, lpcmi, lpcmi->lpVerb, lpcmi->hwnd);

    /* get the active IShellView */
    if ((browser = (IShellBrowser*)SendMessageA(lpcmi->hwnd, CWM_GETISHELLBROWSER, 0, 0)))
    {
        if (SUCCEEDED(IShellBrowser_QueryActiveShellView(browser, &view)))
	    IShellView_GetWindow(view, &hWnd);
    }

    if(HIWORD(lpcmi->lpVerb))
    {
        TRACE("%s\n", debugstr_a(lpcmi->lpVerb));

        if (!strcmp(lpcmi->lpVerb, CMDSTR_NEWFOLDERA))
        {
            DoNewFolder(This, view);
        }
        else if (!strcmp(lpcmi->lpVerb, CMDSTR_VIEWLISTA))
        {
            if (hWnd) SendMessageA(hWnd, WM_COMMAND, MAKEWPARAM(FCIDM_SHVIEW_LISTVIEW, 0), 0);
        }
        else if (!strcmp(lpcmi->lpVerb, CMDSTR_VIEWDETAILSA))
        {
	    if (hWnd) SendMessageA(hWnd, WM_COMMAND, MAKEWPARAM(FCIDM_SHVIEW_REPORTVIEW, 0), 0);
        }
        else if (!strcmp(lpcmi->lpVerb, "paste"))
        {
            do_paste(This, lpcmi->hwnd);
        }
        else
        {
            FIXME("please report: unknown verb %s\n", debugstr_a(lpcmi->lpVerb));
        }
    }
    else
    {
        switch (LOWORD(lpcmi->lpVerb) + FCIDM_BASE)
        {
	    case FCIDM_SHVIEW_REFRESH:
	        if (view) IShellView_Refresh(view);
                break;

            case FCIDM_SHVIEW_NEWFOLDER:
                DoNewFolder(This, view);
                break;

            case FCIDM_SHVIEW_INSERT:
                do_paste(This, lpcmi->hwnd);
                break;

            case FCIDM_SHVIEW_PROPERTIES:
                if (This->desktop) {
		    ShellExecuteA(lpcmi->hwnd, "open", "rundll32.exe shell32.dll,Control_RunDLL desk.cpl", NULL, NULL, SW_SHOWNORMAL);
		} else {
		    FIXME("launch item properties dialog\n");
		}
		break;

            default:
                /* if it's an id just pass it to the parent shv */
                if (hWnd) SendMessageA(hWnd, WM_COMMAND, MAKEWPARAM(LOWORD(lpcmi->lpVerb), 0), 0);
                break;
         }
    }

    if (view)
        IShellView_Release(view);

    return S_OK;
}

static HRESULT WINAPI BackgroundMenu_GetCommandString(
	IContextMenu3 *iface,
	UINT_PTR idCommand,
	UINT uFlags,
	UINT* lpReserved,
	LPSTR lpszName,
	UINT uMaxNameLen)
{
    ContextMenu *This = impl_from_IContextMenu3(iface);
    const WCHAR *cmdW = NULL;
    HRESULT hr = E_FAIL;

    TRACE("(%p)->(idcom=%Ix flags=%x %p name=%p len=%x)\n",This, idCommand, uFlags, lpReserved, lpszName, uMaxNameLen);

    switch (uFlags)
    {
    case GCS_HELPTEXTA:
    case GCS_HELPTEXTW:
        hr = E_NOTIMPL;
        break;

    case GCS_VERBA:
    case GCS_VERBW:
        switch (idCommand + FCIDM_BASE)
        {
        case FCIDM_SHVIEW_INSERT:
            cmdW = L"paste";
            break;
        case FCIDM_SHVIEW_PROPERTIES:
            cmdW = L"properties";
            break;
        }

        if (!cmdW)
        {
            hr = E_INVALIDARG;
            break;
        }

        if (uFlags == GCS_VERBA)
            WideCharToMultiByte(CP_ACP, 0, cmdW, -1, lpszName, uMaxNameLen, NULL, NULL);
        else
            lstrcpynW((WCHAR *)lpszName, cmdW, uMaxNameLen);
        TRACE("name %s\n", uFlags == GCS_VERBA ? debugstr_a(lpszName) : debugstr_w((WCHAR *)lpszName));
        hr = S_OK;
        break;

    case GCS_VALIDATEA:
    case GCS_VALIDATEW:
        /* test the existence of the menu items, the file dialog enables
           the buttons according to this */
        if (HIWORD(idCommand))
        {
            if (!strcmp((LPSTR)idCommand, CMDSTR_VIEWLISTA) ||
                !strcmp((LPSTR)idCommand, CMDSTR_VIEWDETAILSA) ||
                !strcmp((LPSTR)idCommand, CMDSTR_NEWFOLDERA))
                hr = S_OK;
            else
            {
                FIXME("unknown command string %s\n", uFlags == GCS_VALIDATEA ? debugstr_a((LPSTR)idCommand) : debugstr_w((WCHAR*)idCommand));
                hr = E_FAIL;
            }
        }
        break;
    }
    return hr;
}

static const IContextMenu3Vtbl BackgroundContextMenuVtbl =
{
    ContextMenu_QueryInterface,
    ContextMenu_AddRef,
    ContextMenu_Release,
    BackgroundMenu_QueryContextMenu,
    BackgroundMenu_InvokeCommand,
    BackgroundMenu_GetCommandString,
    ContextMenu_HandleMenuMsg,
    ContextMenu_HandleMenuMsg2
};

HRESULT BackgroundMenu_Constructor(IShellFolder *parent, BOOL desktop, REFIID riid, void **pObj)
{
    ContextMenu *This;
    HRESULT hr;

    if (!(This = calloc(1, sizeof(*This))))
        return E_OUTOFMEMORY;

    This->IContextMenu3_iface.lpVtbl = &BackgroundContextMenuVtbl;
    This->IShellExtInit_iface.lpVtbl = &ShellExtInitVtbl;
    This->IObjectWithSite_iface.lpVtbl = &ObjectWithSiteVtbl;
    This->ref = 1;
    This->parent = parent;

    This->desktop = desktop;
    if (parent) IShellFolder_AddRef(parent);

    hr = IContextMenu3_QueryInterface(&This->IContextMenu3_iface, riid, pObj);
    IContextMenu3_Release(&This->IContextMenu3_iface);

    return hr;
}
