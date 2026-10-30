/*
 * Copyright 2015 Michael Müller
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

#include "shobjidl.h"

#include "shell32_main.h"
#include "shresdef.h"
#include "pidl.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

enum new_kind { NEW_DIRECTORY, NEW_NULL_FILE, NEW_DATA, NEW_TEMPLATE, NEW_COMMAND };

struct new_menu
{
    IShellExtInit IShellExtInit_iface;
    IContextMenu3 IContextMenu3_iface;
    IObjectWithSite IObjectWithSite_iface;
    LONG refcount;

    struct new_item
    {
        WCHAR *name;        /* the new item's name, without extension */
        WCHAR *text;        /* menu text */
        WCHAR *ext;         /* file extension; NULL for a folder */
        enum new_kind kind;
        BYTE *data;         /* NEW_DATA bytes, the NEW_TEMPLATE file name or the NEW_COMMAND command line */
        DWORD size;
    } *items;
    size_t item_count;

    ITEMIDLIST *pidl;
    IUnknown *site;         /* the shell view: it shows the new item for renaming */
};

static struct new_menu *impl_from_IShellExtInit(IShellExtInit *iface)
{
    return CONTAINING_RECORD(iface, struct new_menu, IShellExtInit_iface);
}

static HRESULT WINAPI ext_init_QueryInterface(IShellExtInit *iface, REFIID iid, void **out)
{
    struct new_menu *menu = impl_from_IShellExtInit(iface);

    TRACE("menu %p, iid %s, out %p.\n", menu, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IShellExtInit))
        *out = &menu->IShellExtInit_iface;
    else if (IsEqualGUID(iid, &IID_IContextMenu)
            || IsEqualGUID(iid, &IID_IContextMenu2)
            || IsEqualGUID(iid, &IID_IContextMenu3))
        *out = &menu->IContextMenu3_iface;
    else if (IsEqualGUID(iid, &IID_IObjectWithSite))
        *out = &menu->IObjectWithSite_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI ext_init_AddRef(IShellExtInit *iface)
{
    struct new_menu *menu = impl_from_IShellExtInit(iface);
    ULONG refcount = InterlockedIncrement(&menu->refcount);

    TRACE("%p increasing refcount to %lu.\n", menu, refcount);

    return refcount;
}

static ULONG WINAPI ext_init_Release(IShellExtInit *iface)
{
    struct new_menu *menu = impl_from_IShellExtInit(iface);
    ULONG refcount = InterlockedDecrement(&menu->refcount);

    TRACE("%p decreasing refcount to %lu.\n", menu, refcount);

    if (!refcount)
    {
        for (unsigned int i = 0; i < menu->item_count; ++i)
        {
            free(menu->items[i].name);
            free(menu->items[i].text);
            free(menu->items[i].ext);
            free(menu->items[i].data);
        }
        free(menu->items);
        if (menu->site) IUnknown_Release(menu->site);
        ILFree(menu->pidl);
        free(menu);
    }

    return refcount;
}

static HRESULT WINAPI ext_init_Initialize(IShellExtInit *iface, LPCITEMIDLIST pidl, IDataObject *obj, HKEY key)
{
    struct new_menu *menu = impl_from_IShellExtInit(iface);

    TRACE("menu %p, pidl %p, obj %p, key %p.\n", menu, pidl, obj, key);

    if (!pidl)
        return E_INVALIDARG;

    menu->pidl = ILClone(pidl);
    return S_OK;
}

static const IShellExtInitVtbl ext_init_vtbl =
{
    ext_init_QueryInterface,
    ext_init_AddRef,
    ext_init_Release,
    ext_init_Initialize,
};

static struct new_menu *impl_from_IContextMenu3(IContextMenu3 *iface)
{
    return CONTAINING_RECORD(iface, struct new_menu, IContextMenu3_iface);
}

static HRESULT WINAPI context_menu_QueryInterface(IContextMenu3 *iface, REFIID iid, void **out)
{
    struct new_menu *menu = impl_from_IContextMenu3(iface);

    return IShellExtInit_QueryInterface(&menu->IShellExtInit_iface, iid, out);
}

static ULONG WINAPI context_menu_AddRef(IContextMenu3 *iface)
{
    struct new_menu *menu = impl_from_IContextMenu3(iface);

    return IShellExtInit_AddRef(&menu->IShellExtInit_iface);
}

static ULONG WINAPI context_menu_Release(IContextMenu3 *iface)
{
    struct new_menu *menu = impl_from_IContextMenu3(iface);

    return IShellExtInit_Release(&menu->IShellExtInit_iface);
}

static WCHAR *load_mui_string(HKEY key, const WCHAR *value)
{
    WCHAR *string;
    DWORD size;

    if (RegLoadMUIStringW(key, value, NULL, 0, &size, 0, NULL) != ERROR_MORE_DATA)
        return NULL;
    string = malloc(size + sizeof(WCHAR));
    RegLoadMUIStringW(key, value, string, size + sizeof(WCHAR), NULL, 0, NULL);
    string[size / sizeof(WCHAR)] = 0;
    return string;
}

/* a value that may be a "@dll,-id" resource string or plain text */
static WCHAR *get_string(HKEY key, const WCHAR *value)
{
    WCHAR *string;
    DWORD size = 0;

    if ((string = load_mui_string(key, value))) return string;
    if (RegGetValueW(key, NULL, value, RRF_RT_REG_SZ, NULL, NULL, &size) || !size) return NULL;
    string = malloc(size);
    if (RegGetValueW(key, NULL, value, RRF_RT_REG_SZ, NULL, string, &size)) { free(string); return NULL; }
    return string;
}

static WCHAR *format_resource(UINT id, const WCHAR *arg)
{
    WCHAR format[64], *ret = NULL;
    DWORD_PTR args[1] = { (DWORD_PTR)arg };

    LoadStringW(shell32_hInstance, id, format, ARRAY_SIZE(format));
    FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                   format, 0, 0, (WCHAR *)&ret, 0, (va_list *)args);
    if (!ret) return NULL;
    {
        WCHAR *copy = wcsdup(ret);
        LocalFree(ret);
        return copy;
    }
}

/* The folder: HKCR\Folder\ShellNew names it */
static void add_folder(struct new_menu *menu)
{
    struct new_item *item;
    HKEY key;

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Folder\\ShellNew", 0, KEY_READ, &key)) return;
    menu->items = realloc(menu->items, (menu->item_count + 1) * sizeof(*menu->items));
    item = &menu->items[menu->item_count];
    memset(item, 0, sizeof(*item));
    item->kind = NEW_DIRECTORY;
    item->name = get_string(key, L"ItemName");
    item->text = get_string(key, L"MenuText");
    RegCloseKey(key);
    if (!item->name || !item->text)
    {
        free(item->name);
        free(item->text);
        return;
    }
    menu->item_count++;
}

/* A file type: ".ext\ShellNew" or ".ext\progid\ShellNew" with NullFile, Data or FileName, as Windows reads them.
 * The menu shows the type's description, the new file is "New <description>.ext". */
static void add_file_type(struct new_menu *menu, const WCHAR *ext)
{
    WCHAR progid[MAX_PATH], path[MAX_PATH * 2];
    struct new_item item = { 0 };
    DWORD size = sizeof(progid), type;
    HKEY key = NULL, progid_key;

    if (RegGetValueW(HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, NULL, progid, &size)) progid[0] = 0;
    swprintf(path, ARRAY_SIZE(path), L"%s\\%s\\ShellNew", ext, progid);
    if (!progid[0] || RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &key))
    {
        swprintf(path, ARRAY_SIZE(path), L"%s\\ShellNew", ext);
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &key)) return;
    }

    if (!RegQueryValueExW(key, L"NullFile", NULL, NULL, NULL, NULL)) item.kind = NEW_NULL_FILE;
    else if (!RegQueryValueExW(key, L"Data", NULL, &type, NULL, &item.size))
    {
        item.kind = NEW_DATA;
        item.data = malloc(item.size ? item.size : 1);
        RegQueryValueExW(key, L"Data", NULL, NULL, item.data, &item.size);
        if (type == REG_SZ || type == REG_EXPAND_SZ)   /* a string is stored as ANSI text */
        {
            DWORD len = WideCharToMultiByte(CP_ACP, 0, (WCHAR *)item.data, -1, NULL, 0, NULL, NULL);
            char *text = malloc(len);
            WideCharToMultiByte(CP_ACP, 0, (WCHAR *)item.data, -1, text, len, NULL, NULL);
            free(item.data);
            item.data = (BYTE *)text;
            item.size = len ? len - 1 : 0;
        }
    }
    else if ((item.data = (BYTE *)get_string(key, L"FileName"))) item.kind = NEW_TEMPLATE;
    else if ((item.data = (BYTE *)get_string(key, L"Command"))) item.kind = NEW_COMMAND;   /* a wizard: Shortcut */
    else
    {
        RegCloseKey(key);
        return;
    }

    item.text = get_string(key, L"MenuText");
    RegCloseKey(key);
    if (!item.text && progid[0] && !RegOpenKeyExW(HKEY_CLASSES_ROOT, progid, 0, KEY_READ, &progid_key))
    {
        if (!(item.text = get_string(progid_key, L"FriendlyTypeName")))
            item.text = get_string(progid_key, NULL);
        RegCloseKey(progid_key);
    }
    if (!item.text || !item.text[0] || !(item.name = format_resource(IDS_NEW_ITEM, item.text)))
    {
        free(item.text);
        free(item.data);
        return;
    }
    item.ext = wcsdup(ext);
    menu->items = realloc(menu->items, (menu->item_count + 1) * sizeof(*menu->items));
    menu->items[menu->item_count++] = item;
}

/* Windows XP: the wizards (Shortcut) come before the file types */
static int __cdecl compare_items(const void *a, const void *b)
{
    const struct new_item *item1 = a, *item2 = b;

    if ((item1->kind == NEW_COMMAND) != (item2->kind == NEW_COMMAND)) return item1->kind == NEW_COMMAND ? -1 : 1;
    return lstrcmpiW(item1->text, item2->text);
}

static void load_items(struct new_menu *menu)
{
    WCHAR name[MAX_PATH];
    size_t first;
    DWORD i, len;

    if (menu->item_count) return;
    add_folder(menu);
    first = menu->item_count;
    for (i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW(HKEY_CLASSES_ROOT, i, name, &len, NULL, NULL, NULL, NULL); i++)
        if (name[0] == '.') add_file_type(menu, name);
    qsort(menu->items + first, menu->item_count - first, sizeof(*menu->items), compare_items);
    for (i = 0; i < menu->item_count; i++)
        TRACE("item %s: %s kind %u\n", debugstr_w(menu->items[i].text), debugstr_w(menu->items[i].name), menu->items[i].kind);
}

static HRESULT WINAPI context_menu_QueryContextMenu(IContextMenu3 *iface,
        HMENU hmenu, UINT index, UINT min_id, UINT max_id, UINT flags)
{
    struct new_menu *menu = impl_from_IContextMenu3(iface);
    MENUITEMINFOW info;
    WCHAR *new_string;
    HMENU submenu;
    unsigned int i;

    TRACE("menu %p, hmenu %p, index %u, min_id %u, max_id %u, flags %#x.\n",
            menu, hmenu, index, min_id, max_id, flags);

    if (!_ILIsFolder(ILFindLastID(menu->pidl)) && !_ILIsDrive(ILFindLastID(menu->pidl)))
    {
        TRACE("Not returning a New item for this pidl type.\n");
        return S_OK;
    }

    load_items(menu);
    submenu = CreatePopupMenu();
    for (i = 0; i < menu->item_count && min_id + 1 + i <= max_id && i < 0x3e; ++i)
    {
        /* Windows XP: Folder, Shortcut, a separator, then the file types */
        if (i && menu->items[i].kind != NEW_COMMAND &&
            (menu->items[i - 1].kind == NEW_DIRECTORY || menu->items[i - 1].kind == NEW_COMMAND))
            AppendMenuW(submenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(submenu, MF_STRING, min_id + 1 + i, menu->items[i].text);
    }

    new_string = shell_get_resource_string(IDS_NEW_MENU);
    info.cbSize = sizeof(info);
    info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_STRING | MIIM_SUBMENU;
    info.dwTypeData = new_string;
    info.fState = MFS_ENABLED;
    info.wID = min_id;
    info.fType = MFT_STRING;
    info.hSubMenu = submenu;
    InsertMenuItemW(hmenu, index, MF_BYPOSITION, &info);

    free(new_string);

    /* Native doesn't return the highest ID; it instead just returns 0x40. */
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0x40);
}

static BOOL create_item(const struct new_item *item, const WCHAR *path)
{
    WCHAR template[MAX_PATH];
    HANDLE file;
    DWORD written;
    BOOL ret;

    switch (item->kind)
    {
    case NEW_DIRECTORY:
        return CreateDirectoryW(path, NULL);
    case NEW_TEMPLATE:
        /* a bare name is in the ShellNew folder of Windows */
        if (PathIsFileSpecW((WCHAR *)item->data))
        {
            GetWindowsDirectoryW(template, ARRAY_SIZE(template));
            PathAppendW(template, L"ShellNew");
            PathAppendW(template, (WCHAR *)item->data);
        }
        else ExpandEnvironmentStringsW((WCHAR *)item->data, template, ARRAY_SIZE(template));
        return CopyFileW(template, path, TRUE);
    default:
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return FALSE;
        ret = item->kind == NEW_NULL_FILE || WriteFile(file, item->data, item->size, &written, NULL);
        CloseHandle(file);
        return ret;
    }
}

/* the view shows the new item ready to be renamed, as on Windows */
static void edit_new_item(struct new_menu *menu, const WCHAR *name)
{
    IShellFolder *folder;
    IShellView *view;
    ITEMIDLIST *child;

    if (!menu->site || FAILED(IUnknown_QueryInterface(menu->site, &IID_IShellView, (void **)&view))) return;
    IShellView_Refresh(view);
    if (SUCCEEDED(SHBindToObject(NULL, menu->pidl, NULL, &IID_IShellFolder, (void **)&folder)))
    {
        if (SUCCEEDED(IShellFolder_ParseDisplayName(folder, NULL, NULL, (WCHAR *)name, NULL, &child, NULL)))
        {
            IShellView_SelectItem(view, child, SVSI_DESELECTOTHERS | SVSI_EDIT | SVSI_ENSUREVISIBLE |
                                  SVSI_FOCUSED | SVSI_SELECT);
            ILFree(child);
        }
        IShellFolder_Release(folder);
    }
    IShellView_Release(view);
}

static HRESULT WINAPI context_menu_InvokeCommand(IContextMenu3 *iface, CMINVOKECOMMANDINFO *info)
{
    struct new_menu *menu = impl_from_IContextMenu3(iface);
    WCHAR path[MAX_PATH], name[MAX_PATH];
    const struct new_item *item;
    unsigned int id;

    TRACE("menu %p, info %p.\n", menu, info);

    id = (UINT_PTR)info->lpVerb - 1;

    if (id >= menu->item_count)
    {
        ERR("Invalid verb %p.\n", info->lpVerb);
        return E_FAIL;
    }
    item = &menu->items[id];

    if (!SHGetPathFromIDListW(menu->pidl, path))
    {
        ERR("Failed to get path.\n");
        return E_FAIL;
    }

    if (item->kind == NEW_COMMAND)
    {
        /* the wizard gets the name the new item would have, "%1", and makes it itself */
        WCHAR full[MAX_PATH], command[MAX_PATH * 3], expanded[MAX_PATH * 3], *p;
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;

        swprintf(full, ARRAY_SIZE(full), L"%s\\%s%s", path, item->name, item->ext ? item->ext : L"");
        ExpandEnvironmentStringsW((WCHAR *)item->data, expanded, ARRAY_SIZE(expanded));
        if ((p = wcsstr(expanded, L"%1")))
        {
            *p = 0;
            swprintf(command, ARRAY_SIZE(command), L"%s%s%s", expanded, full, p + 2);
        }
        else lstrcpynW(command, expanded, ARRAY_SIZE(command));
        TRACE("running %s\n", debugstr_w(command));
        if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, path, &si, &pi))
            return HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return S_OK;
    }

    for (unsigned int i = 0; i < 1000; ++i)
    {
        WCHAR full[MAX_PATH];

        if (!i)
            swprintf(name, ARRAY_SIZE(name), L"%s%s", item->name, item->ext ? item->ext : L"");
        else
            swprintf(name, ARRAY_SIZE(name), L"%s (%u)%s", item->name, i + 1, item->ext ? item->ext : L"");
        swprintf(full, ARRAY_SIZE(full), L"%s\\%s", path, name);

        if (create_item(item, full))
        {
            SHChangeNotify(item->kind == NEW_DIRECTORY ? SHCNE_MKDIR : SHCNE_CREATE, SHCNF_PATHW, full, NULL);
            edit_new_item(menu, name);
            return S_OK;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS)
        {
            WARN("Failed to create %s, error %lu.\n", debugstr_w(full), GetLastError());
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }
    return E_FAIL;
}

static HRESULT WINAPI context_menu_GetCommandString(IContextMenu3 *iface,
        UINT_PTR id, UINT type, UINT *reserved, char *string, UINT size)
{
    FIXME("iface %p, id %Iu, type %#x, reserved %p, string %p, size %u, stub!\n",
            iface, id, type, reserved, string, size);

    return E_NOTIMPL;
}

static HRESULT WINAPI context_menu_HandleMenuMsg(IContextMenu3 *iface, UINT msg, WPARAM wparam, LPARAM lparam)
{
    FIXME("iface %p, msg %#x, wparam %#Ix, lparam %#Ix, stub!\n", iface, msg, wparam, lparam);

    return E_NOTIMPL;
}

static HRESULT WINAPI context_menu_HandleMenuMsg2(IContextMenu3 *iface,
        UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    FIXME("iface %p, msg %#x, wparam %#Ix, lparam %#Ix, result %p, stub!\n",
            iface, msg, wparam, lparam, result);

    return E_NOTIMPL;
}

static const IContextMenu3Vtbl context_menu_vtbl =
{
    context_menu_QueryInterface,
    context_menu_AddRef,
    context_menu_Release,
    context_menu_QueryContextMenu,
    context_menu_InvokeCommand,
    context_menu_GetCommandString,
    context_menu_HandleMenuMsg,
    context_menu_HandleMenuMsg2,
};

static struct new_menu *impl_from_IObjectWithSite(IObjectWithSite *iface)
{
    return CONTAINING_RECORD(iface, struct new_menu, IObjectWithSite_iface);
}

static HRESULT WINAPI object_with_site_QueryInterface(IObjectWithSite *iface, REFIID iid, void **out)
{
    struct new_menu *menu = impl_from_IObjectWithSite(iface);

    return IShellExtInit_QueryInterface(&menu->IShellExtInit_iface, iid, out);
}

static ULONG WINAPI object_with_site_AddRef(IObjectWithSite *iface)
{
    struct new_menu *menu = impl_from_IObjectWithSite(iface);

    return IShellExtInit_AddRef(&menu->IShellExtInit_iface);
}

static ULONG WINAPI object_with_site_Release(IObjectWithSite *iface)
{
    struct new_menu *menu = impl_from_IObjectWithSite(iface);

    return IShellExtInit_Release(&menu->IShellExtInit_iface);
}

static HRESULT WINAPI object_with_site_SetSite(IObjectWithSite *iface, IUnknown *site)
{
    struct new_menu *menu = impl_from_IObjectWithSite(iface);

    TRACE("iface %p, site %p.\n", iface, site);

    if (site) IUnknown_AddRef(site);
    if (menu->site) IUnknown_Release(menu->site);
    menu->site = site;
    return S_OK;
}

static HRESULT WINAPI object_with_site_GetSite(IObjectWithSite *iface, REFIID iid, void **out)
{
    struct new_menu *menu = impl_from_IObjectWithSite(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    *out = NULL;
    if (!menu->site) return E_FAIL;
    return IUnknown_QueryInterface(menu->site, iid, out);
}

static const IObjectWithSiteVtbl object_with_site_vtbl =
{
    object_with_site_QueryInterface,
    object_with_site_AddRef,
    object_with_site_Release,
    object_with_site_SetSite,
    object_with_site_GetSite,
};

HRESULT WINAPI new_menu_create(IUnknown *outer, REFIID iid, void **out)
{
    struct new_menu *menu;
    HRESULT hr;

    if (outer)
        return CLASS_E_NOAGGREGATION;

    if (!(menu = calloc(1, sizeof(*menu))))
        return E_OUTOFMEMORY;

    menu->IShellExtInit_iface.lpVtbl = &ext_init_vtbl;
    menu->IContextMenu3_iface.lpVtbl = &context_menu_vtbl;
    menu->IObjectWithSite_iface.lpVtbl = &object_with_site_vtbl;
    menu->refcount = 1;

    TRACE("Created New menu %p.\n", menu);

    hr = IShellExtInit_QueryInterface(&menu->IShellExtInit_iface, iid, out);
    IShellExtInit_Release(&menu->IShellExtInit_iface);
    return hr;
}
