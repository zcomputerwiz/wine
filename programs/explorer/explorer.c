/*
 * explorer.exe
 *
 * Copyright 2004 CodeWeavers, Mike Hearn
 * Copyright 2005,2006 CodeWeavers, Aric Stewart
 * Copyright 2011 Jay Yang
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

/* must precede every other header so the GUIDs used across explorer get defined here */
#include <initguid.h>
#include "wine/debug.h"
#include "explorer_private.h"
#include "resource.h"
#include "browser.h"
#include "search_ui.h"
#include "shellres.h"
#include "wlan_dialog.h"
#include "wlan_status.h"
#include "net_props.h"
#include "power_mgmt.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commoncontrols.h>
#include <commctrl.h>

WINE_DEFAULT_DEBUG_CHANNEL(explorer);

#define EXPLORER_INFO_INDEX 0

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
static int default_width;
static int default_height;


HINSTANCE explorer_hInstance;

typedef struct parametersTAG {
    BOOL    explorer_mode;
    WCHAR   root[MAX_PATH];
    WCHAR   selection[MAX_PATH];
} parameters_struct;

static void variant_from_pidl(VARIANT *var, const ITEMIDLIST *pidl)
{
    V_VT(var) = VT_ARRAY | VT_UI1;
    V_ARRAY(var) = SafeArrayCreateVector(VT_UI1, 0, ILGetSize(pidl));
    memcpy(V_ARRAY(var)->pvData, pidl, ILGetSize(pidl));
}

typedef struct
{
    IExplorerBrowserEvents IExplorerBrowserEvents_iface;
    explorer_info* info;
    LONG ref;
} IExplorerBrowserEventsImpl;

static IExplorerBrowserEventsImpl *impl_from_IExplorerBrowserEvents(IExplorerBrowserEvents *iface)
{
    return CONTAINING_RECORD(iface, IExplorerBrowserEventsImpl, IExplorerBrowserEvents_iface);
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnQueryInterface(IExplorerBrowserEvents *iface, REFIID riid, void **ppvObject)
{
    return E_NOINTERFACE;
}

static ULONG WINAPI IExplorerBrowserEventsImpl_fnAddRef(IExplorerBrowserEvents *iface)
{
    IExplorerBrowserEventsImpl *This = impl_from_IExplorerBrowserEvents(iface);
    return InterlockedIncrement(&This->ref);
}

static ULONG WINAPI IExplorerBrowserEventsImpl_fnRelease(IExplorerBrowserEvents *iface)
{
    IExplorerBrowserEventsImpl *This = impl_from_IExplorerBrowserEvents(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    if(!ref) free(This);
    return ref;
}

static BOOL create_combobox_item(IShellFolder *folder, LPCITEMIDLIST child_pidl, IImageList *icon_list, COMBOBOXEXITEMW *item)
{
    STRRET strret;
    HRESULT hres;
    PIDLIST_ABSOLUTE parent_pidl, pidl;
    SHFILEINFOW info;
    IImageList *list;

    strret.uType=STRRET_WSTR;
    hres = IShellFolder_GetDisplayNameOf( folder, child_pidl, SHGDN_FORADDRESSBAR, &strret );
    if(SUCCEEDED(hres))
        hres = StrRetToStrW(&strret, child_pidl, &item->pszText);
    if(FAILED(hres))
    {
        WARN( "Could not get name for pidl\n" );
        return FALSE;
    }

    item->mask &= ~CBEIF_IMAGE;
    hres = SHGetIDListFromObject( (IUnknown *)folder, &parent_pidl );
    if (FAILED(hres)) return FALSE;

    pidl = ILCombine( parent_pidl, child_pidl );
    if (pidl)
    {
        list = (IImageList *)SHGetFileInfoW( (WCHAR *)pidl, 0, &info, sizeof(info),
                                             SHGFI_PIDL | SHGFI_SMALLICON | SHGFI_SYSICONINDEX );
        if (list)
        {
            IImageList_Release( list );
            item->iImage = info.iIcon;
            item->mask |= CBEIF_IMAGE;
        }
        ILFree( pidl );
    }
    ILFree( parent_pidl );

    return TRUE;
}

static void update_path_box(explorer_info *info)
{
    COMBOBOXEXITEMW item;
    COMBOBOXEXITEMW main_item;
    IShellFolder *desktop;
    IPersistFolder2 *persist;
    LPITEMIDLIST desktop_pidl;
    IEnumIDList *ids;

    SendMessageW(info->path_box,CB_RESETCONTENT,0,0);
    SHGetDesktopFolder(&desktop);
    IShellFolder_QueryInterface(desktop,&IID_IPersistFolder2,(void**)&persist);
    IPersistFolder2_GetCurFolder(persist,&desktop_pidl);
    IPersistFolder2_Release(persist);
    persist = NULL;
    /*Add Desktop*/
    item.iItem = -1;
    item.mask = CBEIF_TEXT | CBEIF_INDENT | CBEIF_LPARAM;
    item.iIndent = 0;
    create_combobox_item(desktop,desktop_pidl,info->icon_list,&item);
    item.lParam = (LPARAM)desktop_pidl;
    SendMessageW(info->path_box,CBEM_INSERTITEMW,0,(LPARAM)&item);
    if(ILIsEqual(info->pidl,desktop_pidl))
        main_item = item;
    else
        CoTaskMemFree(item.pszText);
    /*Add all direct subfolders of Desktop*/
    if(SUCCEEDED(IShellFolder_EnumObjects(desktop,NULL,SHCONTF_FOLDERS,&ids))
       && ids!=NULL)
    {
        LPITEMIDLIST curr_pidl=NULL;
        HRESULT hres;

        item.iIndent = 1;
        while(1)
        {
            ILFree(curr_pidl);
            curr_pidl=NULL;
            hres = IEnumIDList_Next(ids,1,&curr_pidl,NULL);
            if(FAILED(hres) || hres == S_FALSE)
                break;
            if (!create_combobox_item( desktop, curr_pidl, info->icon_list, &item ))
                WARN( "Could not create a combobox item\n" );
            else
            {
                LPITEMIDLIST full_pidl = ILCombine(desktop_pidl,curr_pidl);
                item.lParam = (LPARAM)full_pidl;
                SendMessageW(info->path_box,CBEM_INSERTITEMW,0,(LPARAM)&item);
                if(ILIsEqual(full_pidl,info->pidl))
                    main_item = item;
                else if(ILIsParent(full_pidl,info->pidl,FALSE))
                {
                    /*add all parents of the pidl passed in*/
                    LPITEMIDLIST next_pidl = ILFindChild(full_pidl,info->pidl);
                    IShellFolder *curr_folder = NULL, *temp;
                    hres = IShellFolder_BindToObject(desktop,curr_pidl,NULL,
                                                     &IID_IShellFolder,
                                                     (void**)&curr_folder);
                    if (FAILED(hres)) WARN( "Could not get an IShellFolder\n" );
                    while(!ILIsEmpty(next_pidl))
                    {
                        LPITEMIDLIST first = ILCloneFirst(next_pidl);
                        CoTaskMemFree(item.pszText);
                        if(!create_combobox_item(curr_folder,first,
                                                 info->icon_list,&item))
                        {
                            WARN( "Could not create a combobox item\n" );
                            break;
                        }
                        ++item.iIndent;
                        full_pidl = ILCombine(full_pidl,first);
                        item.lParam = (LPARAM)full_pidl;
                        SendMessageW(info->path_box,CBEM_INSERTITEMW,0,(LPARAM)&item);
                        temp=NULL;
                        hres = IShellFolder_BindToObject(curr_folder,first,NULL,
                                                         &IID_IShellFolder,
                                                         (void**)&temp);
                        if(FAILED(hres))
                        {
                            WARN( "Could not get an IShellFolder\n" );
                            break;
                        }
                        IShellFolder_Release(curr_folder);
                        curr_folder = temp;

                        ILFree(first);
                        next_pidl = ILGetNext(next_pidl);
                    }
                    memcpy(&main_item,&item,sizeof(item));
                    if(curr_folder)
                        IShellFolder_Release(curr_folder);
                    item.iIndent = 1;
                }
                else
                    CoTaskMemFree(item.pszText);
            }
        }
        ILFree(curr_pidl);
        IEnumIDList_Release(ids);
    }
    else WARN( "Could not enumerate the desktop\n" );
    SendMessageW(info->path_box,CBEM_SETITEMW,0,(LPARAM)&main_item);
    CoTaskMemFree(main_item.pszText);
}

int WINAPI Shell_GetCachedImageIndexW(const WCHAR *path, int index, UINT flags);

/* "Search Results" in the title, the address box and the window icon, like Windows' search folder */
static void show_search_title(explorer_info *info)
{
    COMBOBOXEXITEMW item = { CBEIF_TEXT | CBEIF_IMAGE | CBEIF_SELECTEDIMAGE };
    WCHAR srchui[MAX_PATH];
    HICON icon = NULL, old;
    HMODULE module;
    int image = -1;

    GetWindowsDirectoryW(srchui, MAX_PATH);
    lstrcatW(srchui, L"\\srchasst\\srchui.dll");
    if (GetFileAttributesW(srchui) != INVALID_FILE_ATTRIBUTES) image = Shell_GetCachedImageIndexW(srchui, -24000, 0);
    if (image < 0) image = Shell_GetCachedImageIndexW(L"shell32.dll", -23, 0);
    SetWindowTextW(info->main_window, L"Search Results");
    item.iItem = -1;
    item.pszText = (WCHAR *)L"Search Results";
    item.iImage = item.iSelectedImage = max(image, 0);
    SendMessageW(info->path_box, CBEM_SETITEMW, 0, (LPARAM)&item);
    if ((module = LoadLibraryExW(srchui, NULL, LOAD_LIBRARY_AS_DATAFILE)))
    {
        icon = LoadImageW(module, MAKEINTRESOURCEW(24000), IMAGE_ICON, 16, 16, 0);
        FreeLibrary(module);
    }
    if (!icon) icon = shellres_icon(23, 16);
    if (icon && (old = (HICON)SendMessageW(info->main_window, WM_SETICON, ICON_SMALL, (LPARAM)icon))) DestroyIcon(old);
}

/* the title, address and icon of the folder being shown */
static void show_folder_title(explorer_info *info)
{
    IShellFolder *parent;
    PCUITEMID_CHILD child_pidl;
    SHFILEINFOW sfi = {0};
    STRRET strret;
    WCHAR *name;

    if (!info->pidl) return;
    update_path_box(info);
    if (SHGetFileInfoW((const WCHAR *)info->pidl, 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON))
    {
        HICON old = (HICON)SendMessageW(info->main_window, WM_SETICON, ICON_SMALL, (LPARAM)sfi.hIcon);
        if (old) DestroyIcon(old);
    }
    if (SUCCEEDED(SHBindToParent(info->pidl, &IID_IShellFolder, (void **)&parent, &child_pidl)))
    {
        if (SUCCEEDED(IShellFolder_GetDisplayNameOf(parent, child_pidl, SHGDN_FORADDRESSBAR, &strret)) &&
            SUCCEEDED(StrRetToStrW(&strret, child_pidl, &name)))
        {
            SetWindowTextW(info->main_window, name);
            CoTaskMemFree(name);
        }
        IShellFolder_Release(parent);
    }
}

static void relayout_window(explorer_info *info)
{
    RECT rc;
    GetClientRect(info->main_window, &rc);
    browser_layout(info, rc.right, rc.bottom);
}

/* The Search button: the Search Companion on the left, looking in the folder shown */
void browser_search(explorer_info *info, BOOL show)
{
    WCHAR folder[MAX_PATH];

    if (!info->search)
    {
        if (!show) return;
        if (!info->pidl || !view_folder_path(info, folder)) folder[0] = 0;
        info->search = search_ui_create(info->main_window, folder[0] ? folder : NULL, FALSE);
        if (info->show_folders) browser_command(info, FOLDERS_BUTTON);   /* one Explorer bar at a time */
    }
    else search_ui_show_pane(info->search, show);
    relayout_window(info);
    browser_update_ui(info);
}

/* Leave the search: the folder comes back with its title */
void browser_exit_search(explorer_info *info)
{
    if (!info->search) return;
    search_ui_destroy(info->search);
    info->search = NULL;
    show_folder_title(info);
    relayout_window(info);
    browser_update_ui(info);
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnNavigationComplete(IExplorerBrowserEvents *iface, PCIDLIST_ABSOLUTE pidl)
{
    IExplorerBrowserEventsImpl *This = impl_from_IExplorerBrowserEvents(iface);
    IShellFolder *parent;
    PCUITEMID_CHILD child_pidl;
    HRESULT hres;
    STRRET strret;
    WCHAR *name;

    if (This->info->sw)
    {
        VARIANT var;

        variant_from_pidl(&var, pidl);
        IShellWindows_OnNavigate(This->info->sw, This->info->sw_cookie, &var);
        VariantClear(&var);
    }

    ILFree(This->info->pidl);
    This->info->pidl = ILClone(pidl);
    if (This->info->search_started)
    {
        /* opened as Search Results: the folder behind it is not shown */
        This->info->search_started = FALSE;
        update_path_box(This->info);
        browser_navigation_state(This->info, FALSE);
        show_search_title(This->info);
        return S_OK;
    }
    /* going to a folder while the results show: the results make way, the Search Companion stays */
    if (search_ui_results_visible(This->info->search))
    {
        if (search_ui_pane_visible(This->info->search))
        {
            SEARCH_UI *ui = This->info->search;
            This->info->search = NULL;
            search_ui_destroy(ui);
            browser_search(This->info, TRUE);
        }
        else browser_exit_search(This->info);
    }
    update_path_box(This->info);
    browser_navigation_state(This->info, FALSE);
    browser_apply_folder_view(This->info);
    taskpane_set_watermark(This->info);
    if (This->info->search)
    {
        /* the Search Companion looks in the folder shown */
        WCHAR folder[MAX_PATH];
        search_ui_set_scope(This->info->search, view_folder_path(This->info, folder) ? folder : NULL);
    }
    {
        /* the window shows the icon of the folder it is in */
        SHFILEINFOW sfi = {0};
        if (SHGetFileInfoW((const WCHAR *)pidl, 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON))
        {
            HICON old = (HICON)SendMessageW(This->info->main_window, WM_SETICON, ICON_SMALL, (LPARAM)sfi.hIcon);
            if (old) DestroyIcon(old);
        }
    }

    hres = SHBindToParent(pidl, &IID_IShellFolder, (void **)&parent, &child_pidl);
    if (SUCCEEDED(hres))
    {
        hres = IShellFolder_GetDisplayNameOf(parent, child_pidl, SHGDN_FORADDRESSBAR, &strret);
        if (SUCCEEDED(hres))
            hres = StrRetToStrW(&strret, child_pidl, &name);
        if (SUCCEEDED(hres))
        {
            SetWindowTextW(This->info->main_window, name);
            CoTaskMemFree(name);
        }

        IShellFolder_Release(parent);
    }

    return hres;
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnNavigationFailed(IExplorerBrowserEvents *iface, PCIDLIST_ABSOLUTE pidl)
{
    return S_OK;
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnNavigationPending(IExplorerBrowserEvents *iface, PCIDLIST_ABSOLUTE pidl)
{
    IExplorerBrowserEventsImpl *This = impl_from_IExplorerBrowserEvents(iface);
    browser_navigation_state(This->info, TRUE);
    return S_OK;
}

static HRESULT WINAPI IExplorerBrowserEventsImpl_fnOnViewCreated(IExplorerBrowserEvents *iface, IShellView *psv)
{
    return S_OK;
}

static IExplorerBrowserEventsVtbl vt_IExplorerBrowserEvents =
{
    IExplorerBrowserEventsImpl_fnQueryInterface,
    IExplorerBrowserEventsImpl_fnAddRef,
    IExplorerBrowserEventsImpl_fnRelease,
    IExplorerBrowserEventsImpl_fnOnNavigationPending,
    IExplorerBrowserEventsImpl_fnOnViewCreated,
    IExplorerBrowserEventsImpl_fnOnNavigationComplete,
    IExplorerBrowserEventsImpl_fnOnNavigationFailed
};

static IExplorerBrowserEvents *make_explorer_events(explorer_info *info)
{
    IExplorerBrowserEventsImpl *ret = malloc( sizeof(IExplorerBrowserEventsImpl) );
    ret->IExplorerBrowserEvents_iface.lpVtbl = &vt_IExplorerBrowserEvents;
    ret->info = info;
    ret->ref = 1;
    SHGetImageList(SHIL_SMALL,&IID_IImageList,(void**)&(ret->info->icon_list));
    SendMessageW(info->path_box,CBEM_SETIMAGELIST,0,(LPARAM)ret->info->icon_list);
    return &ret->IExplorerBrowserEvents_iface;
}

static IShellFolder *get_starting_shell_folder(WCHAR *path)
{
    IShellFolder* desktop,*folder;
    LPITEMIDLIST root_pidl;
    HRESULT hres;

    SHGetDesktopFolder(&desktop);

    if (!path)
        return desktop;

    hres = IShellFolder_ParseDisplayName(desktop, NULL, NULL, path, NULL, &root_pidl, NULL);
    if(FAILED(hres))
    {
        return desktop;
    }
    hres = IShellFolder_BindToObject(desktop,root_pidl,NULL,
                                     &IID_IShellFolder,
                                     (void**)&folder);
    ILFree(root_pidl);
    if(FAILED(hres))
    {
        return desktop;
    }
    IShellFolder_Release(desktop);
    return folder;
}

static void make_explorer_window(parameters_struct *params)
{
    RECT rect;
    FOLDERSETTINGS fs;
    IExplorerBrowserEvents *events;
    explorer_info *info;
    HRESULT hres;
    WCHAR explorer_title[100];
    WCHAR pathbox_label[50];
    VARIANT var, empty_var;
    IShellFolder *folder;
    IDispatch *dispatch;
    WCHAR *path = NULL;
    BOOL search_window = FALSE;
    IShellWindows *sw;
    ITEMIDLIST *pidl;
    UINT dpix, dpiy;
    DWORD size;
    LONG hwnd;
    HDC hdc;
    MSG msg;

    CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER,
            &IID_IShellWindows, (void **)&sw);

    if (!wcsicmp(params->root, FIND_FOLDER_PATH))
    {
        /* "Search Results" (SHFindFiles, Start > Search): the Search Companion over the desktop */
        params->root[0] = 0;
        search_window = TRUE;
    }
    if (params->root[0])
    {
        if (params->root[0] == ':' && params->root[1] == ':')
        {
            path = wcsdup(params->root);
        }
        else
        {
            size = GetFullPathNameW(params->root, 0, NULL, NULL);
            path = malloc( size * sizeof(WCHAR) );
            GetFullPathNameW(params->root, size, path, NULL);
        }
    }

    if (sw && path)
    {
        if (!(pidl = ILCreateFromPathW(path)))
        {
            ERR("Failed to create PIDL for %s.\n", debugstr_w(path));
            IShellWindows_Release(sw);
            free(path);
            return;
        }

        variant_from_pidl(&var, pidl);
        V_VT(&empty_var) = VT_EMPTY;
        hres = IShellWindows_FindWindowSW(sw, &var, &empty_var, SWC_EXPLORER, &hwnd, 0, &dispatch);
        VariantClear(&var);
        ILFree(pidl);
        if (hres == S_OK)
        {
            TRACE("Found window %#lx already browsing path %s.\n", hwnd, debugstr_w(path));
            SetForegroundWindow((HWND)(LONG_PTR)hwnd);
            IShellWindows_Release(sw);
            free(path);
            return;
        }
    }

    LoadStringW(explorer_hInstance,IDS_EXPLORER_TITLE,explorer_title, ARRAY_SIZE( explorer_title ));
    LoadStringW(explorer_hInstance,IDS_PATHBOX_LABEL,pathbox_label, ARRAY_SIZE( pathbox_label ));

    hdc = GetDC(0);
    dpix = GetDeviceCaps(hdc, LOGPIXELSX);
    dpiy = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(0, hdc);
    default_width = MulDiv(DEFAULT_WIDTH, dpix, USER_DEFAULT_SCREEN_DPI);
    default_height = MulDiv(DEFAULT_HEIGHT, dpiy, USER_DEFAULT_SCREEN_DPI);

    info = calloc( 1, sizeof(explorer_info) );
    if(!info)
    {
        ERR( "Could not allocate an explorer_info struct\n" );
        IShellWindows_Release(sw);
        free(path);
        return;
    }
    hres = CoCreateInstance(&CLSID_ExplorerBrowser,NULL,CLSCTX_INPROC_SERVER,
                            &IID_IExplorerBrowser,(LPVOID*)&info->browser);
    if(FAILED(hres))
    {
        ERR( "Could not obtain an instance of IExplorerBrowser\n" );
        free(info);
        IShellWindows_Release(sw);
        free(path);
        return;
    }
    info->rebar_height=0;
    info->main_window
        = CreateWindowW(L"CabinetWClass",explorer_title,WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT,CW_USEDEFAULT,default_width,
                        default_height,NULL,browser_create_menu(),explorer_hInstance,NULL);

    if (sw)
    {
        IShellWindows_Register(sw, NULL, (LONG_PTR)info->main_window, SWC_EXPLORER, &info->sw_cookie);
        info->sw = sw;
    }

    fs.ViewMode = FVM_DETAILS;
    fs.fFlags = FWF_AUTOARRANGE;

    SetRect(&rect, 0, 0, default_width, default_height);
    IExplorerBrowser_Initialize(info->browser,info->main_window,&rect,&fs);
    IExplorerBrowser_SetOptions(info->browser,EBO_NONE);
    SetWindowLongPtrW(info->main_window,EXPLORER_INFO_INDEX,(LONG_PTR)info);

    browser_create_bars(info, pathbox_label);
    events = make_explorer_events(info);
    IExplorerBrowser_Advise(info->browser,events,&info->advise_cookie);

    if (search_window)
    {
        info->search_started = TRUE;
        info->search = search_ui_create(info->main_window, NULL, TRUE);
    }
    folder = get_starting_shell_folder(path);
    IExplorerBrowser_BrowseToObject(info->browser, (IUnknown *)folder, SBSP_ABSOLUTE);
    IShellFolder_Release(folder);
    free(path);
    if (search_window)
    {
        show_search_title(info);
        relayout_window(info);
    }

    ShowWindow(info->main_window,SW_SHOWDEFAULT);
    UpdateWindow(info->main_window);
    IExplorerBrowserEvents_Release(events);

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        /* F3 and Ctrl+F open the Search Companion, like in Windows Explorer */
        if (msg.message == WM_KEYDOWN && GetAncestor(msg.hwnd, GA_ROOT) == info->main_window &&
            (msg.wParam == VK_F3 || (msg.wParam == 'F' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0)))
        {
            browser_search(info, TRUE);
            continue;
        }
        if (search_ui_translate(info->search, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void update_window_size(explorer_info *info, int height, int width)
{
    browser_layout(info, width, height);
}

static void do_exit(int code)
{
    OleUninitialize();
    ExitProcess(code);
}

static LRESULT explorer_on_end_edit(explorer_info *info,NMCBEENDEDITW *edit_info)
{
    LPITEMIDLIST pidl = NULL;

    TRACE( "iWhy=%x\n", edit_info->iWhy );
    switch(edit_info->iWhy)
    {
    case CBENF_DROPDOWN:
        if(edit_info->iNewSelection!=CB_ERR)
            pidl = (LPITEMIDLIST)SendMessageW(edit_info->hdr.hwndFrom,
                                              CB_GETITEMDATA,
                                              edit_info->iNewSelection,0);
        break;
    case CBENF_RETURN:
        {
            WCHAR path[MAX_PATH];
            HWND edit_ctrl = (HWND)SendMessageW(edit_info->hdr.hwndFrom,
                                                CBEM_GETEDITCONTROL,0,0);
            *((WORD*)path)=MAX_PATH;
            SendMessageW(edit_ctrl,EM_GETLINE,0,(LPARAM)path);
            pidl = ILCreateFromPathW(path);
            break;
        }
    case CBENF_ESCAPE:
        /*make sure the that the path box resets*/
        update_path_box(info);
        return 0;
    default:
        return 0;
    }
    if(pidl)
        IExplorerBrowser_BrowseToIDList(info->browser,pidl,SBSP_ABSOLUTE);
    if(edit_info->iWhy==CBENF_RETURN)
        ILFree(pidl);
    return 0;
}

static LRESULT update_rebar_size(explorer_info* info,NMRBAUTOSIZE *size_info)
{
    RECT client;
    info->rebar_height = size_info->rcTarget.bottom-size_info->rcTarget.top;
    GetClientRect(info->main_window,&client);
    browser_layout(info, client.right, client.bottom);
    return 0;
}

static LRESULT explorer_on_notify(explorer_info* info,NMHDR* notification)
{
    TRACE( "code=%i\n", notification->code );
    switch(notification->code)
    {
    case CBEN_BEGINEDIT:
        {
            WCHAR path[MAX_PATH];
            HWND edit_ctrl = (HWND)SendMessageW(notification->hwndFrom,
                                                CBEM_GETEDITCONTROL,0,0);
            SHGetPathFromIDListW(info->pidl,path);
            SetWindowTextW(edit_ctrl,path);
            break;
        }
    case CBEN_ENDEDITA:
        {
            NMCBEENDEDITA *edit_info_a = (NMCBEENDEDITA*)notification;
            NMCBEENDEDITW edit_info_w;
            edit_info_w.hdr = edit_info_a->hdr;
            edit_info_w.fChanged = edit_info_a->fChanged;
            edit_info_w.iNewSelection = edit_info_a->iNewSelection;
            MultiByteToWideChar(CP_ACP,0,edit_info_a->szText,-1,
                                edit_info_w.szText,CBEMAXSTRLEN);
            edit_info_w.iWhy = edit_info_a->iWhy;
            return explorer_on_end_edit(info,&edit_info_w);
        }
    case CBEN_ENDEDITW:
        return explorer_on_end_edit(info,(NMCBEENDEDITW*)notification);
    case CBEN_DELETEITEM:
        {
            NMCOMBOBOXEXW *entry = (NMCOMBOBOXEXW*)notification;
            if(entry->ceItem.lParam)
                ILFree((LPITEMIDLIST)entry->ceItem.lParam);
            break;
        }
    case RBN_AUTOSIZE:
        return update_rebar_size(info,(NMRBAUTOSIZE*)notification);
    case TBN_DROPDOWN:
    case TTN_GETDISPINFOW:
        return browser_toolbar_notify(info, notification);
    default:
        break;
    }
    return 0;
}

static BOOL handle_copydata(const explorer_info *info, const COPYDATASTRUCT *cds)
{
    static const unsigned int magic = 0xe32ee32e;
    unsigned int i, flags, count;
    const ITEMIDLIST *child;
    unsigned char *ptr;
    IShellView *sv;
    SVSIF sv_flags;

    TRACE("\n");

    /* For SHOpenFolderAndSelectItems() */
    if (cds->dwData != magic)
        return FALSE;

    ptr = cds->lpData;
    memcpy(&count, ptr, sizeof(count));
    ptr += sizeof(count);
    memcpy(&flags, ptr, sizeof(flags));
    ptr += sizeof(flags);

    sv_flags = flags & OFASI_EDIT ? SVSI_EDIT : SVSI_SELECT;

    IExplorerBrowser_GetCurrentView(info->browser, &IID_IShellView, (void **)&sv);
    for (i = 0; i < count; ++i)
    {
        child = (const ITEMIDLIST *)ptr;
        if (i == 0)
            IShellView_SelectItem(sv, child, sv_flags | SVSI_ENSUREVISIBLE | SVSI_FOCUSED | SVSI_DESELECTOTHERS);
        else
            IShellView_SelectItem(sv, child, sv_flags);
        ptr += ILGetSize(child);
    }
    IShellView_Release(sv);
    return TRUE;
}

static LRESULT CALLBACK explorer_wnd_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    explorer_info *info
        = (explorer_info*)GetWindowLongPtrW(hwnd,EXPLORER_INFO_INDEX);
    IExplorerBrowser *browser = NULL;

    TRACE( "(hwnd=%p,uMsg=%u,wParam=%Ix,lParam=%Ix)\n", hwnd, uMsg, wParam, lParam );
    if(info)
        browser = info->browser;
    switch(uMsg)
    {
    case WM_SEARCHUI_CHANGED:
        if (!info) break;
        if (wParam == SEARCHUI_RESULTS_SHOWN && search_ui_results_visible(info->search)) show_search_title(info);
        if (wParam == SEARCHUI_PANE_CLOSED && !search_ui_results_visible(info->search))
        {
            browser_exit_search(info);
            break;
        }
        relayout_window(info);
        browser_update_ui(info);
        break;
    case WM_DESTROY:
        search_ui_destroy(info->search);
        info->search = NULL;
        if(info->sw)
        {
            IShellWindows_Revoke(info->sw, info->sw_cookie);
            IShellWindows_Release(info->sw);
        }

        IExplorerBrowser_Unadvise(browser,info->advise_cookie);
        IExplorerBrowser_Destroy(browser);
        IExplorerBrowser_Release(browser);
        ILFree(info->pidl);
        IImageList_Release(info->icon_list);
        free(info);
        SetWindowLongPtrW(hwnd,EXPLORER_INFO_INDEX,0);
        PostQuitMessage(0);
        break;
    case WM_QUIT:
        do_exit(wParam);
    case WM_NOTIFY:
        return explorer_on_notify(info,(NMHDR*)lParam);
    case WM_COMMAND:
        if (LOWORD(wParam) >= BACK_BUTTON && HIWORD(wParam) == BN_CLICKED)   /* also menus: both are 0 */
            browser_command(info, LOWORD(wParam));
        break;
    case WM_INITMENUPOPUP:
        browser_init_menu_popup(info, (HMENU)wParam);
        break;
    case WM_TIMER:
        if (wParam == IDT_BROWSER_STATUS) browser_update_ui(info);
        else if (wParam == IDT_BROWSER_BRAND && info->navigating)
        {
            info->brand_frame++;
            InvalidateRect(info->brand, NULL, FALSE);
        }
        break;
    case WM_APPCOMMAND:
        switch(GET_APPCOMMAND_LPARAM(lParam))
        {
            case APPCOMMAND_BROWSER_BACKWARD:
                IExplorerBrowser_BrowseToObject(browser,NULL,SBSP_NAVIGATEBACK);
                break;
            case APPCOMMAND_BROWSER_FORWARD:
                IExplorerBrowser_BrowseToObject(browser,NULL,SBSP_NAVIGATEFORWARD);
                break;
        }
        break;
    case WM_SIZE:
        if (info) update_window_size(info,HIWORD(lParam),LOWORD(lParam));
        break;
    case WM_THEMECHANGED:
        /* a new theme can bring other shell icons (Shell Icons key): reload them in place and show them */
        shellres_reset();
        shellres_reload_shell_icons();
        if (!info) {}
        else if (info->search && search_ui_results_visible(info->search)) show_search_title(info);
        else show_folder_title(info);
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    case WM_COPYDATA:
        return handle_copydata(info, (const COPYDATASTRUCT *)lParam);
    default:
        return DefWindowProcW(hwnd,uMsg,wParam,lParam);
    }
    return 0;
}

static void register_explorer_window_class(void)
{
    WNDCLASSEXW window_class;
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.style = 0;
    window_class.cbClsExtra = 0;
    window_class.cbWndExtra = sizeof(LONG_PTR);
    window_class.lpfnWndProc = explorer_wnd_proc;
    window_class.hInstance = explorer_hInstance;
    window_class.hIcon = NULL;
    window_class.hCursor = NULL;
    window_class.hbrBackground = (HBRUSH)COLOR_BACKGROUND;
    window_class.lpszMenuName = NULL;
    window_class.lpszClassName = L"CabinetWClass";
    window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    window_class.hIconSm = NULL;
    RegisterClassExW(&window_class);
}

static WCHAR *copy_path_string(WCHAR *target, WCHAR *source)
{
    INT i = 0;

    while (iswspace(*source)) source++;

    if (*source == '\"')
    {
        source ++;
        while (*source && *source != '\"') target[i++] = *source++;
        target[i] = 0;
        if (*source) source++;
    }
    else
    {
        while (*source && *source != ',') target[i++] = *source++;
        target[i] = 0;
    }
    PathRemoveBackslashW(target);
    return source;
}


static void copy_path_root(LPWSTR root, LPWSTR path)
{
    LPWSTR p,p2;
    INT i = 0;

    p = path;
    while (*p!=0)
        p++;

    while (*p!='\\' && p > path)
        p--;

    if (p == path)
        return;

    p2 = path;
    while (p2 != p)
    {
        root[i] = *p2;
        i++;
        p2++;
    }
    root[i] = 0;
}

/*
 * Command Line parameters are:
 * [/n]  Opens in single-paned view for each selected items. This is default
 * [/e,] Uses Windows Explorer View
 * [/cd,object] Specifies the root level of the view
 * [/root,object] Specifies the root level of the view
 * [/select,object] parent folder is opened and specified object is selected
 */
static void parse_command_line(LPWSTR commandline,parameters_struct *parameters)
{
    static const WCHAR arg_n[] = L"/n";
    static const WCHAR arg_e[] = L"/e,";
    static const WCHAR arg_cd[] = L"/cd,";
    static const WCHAR arg_root[] = L"/root,";
    static const WCHAR arg_select[] = L"/select,";
    static const WCHAR arg_desktop[] = L"/desktop";
    static const WCHAR arg_desktop_quotes[] = L"\"/desktop";
    const size_t len_n = wcslen(arg_n);
    const size_t len_e = wcslen(arg_e);
    const size_t len_cd = wcslen(arg_cd);
    const size_t len_root = wcslen(arg_root);
    const size_t len_select = wcslen(arg_select);
    const size_t len_desktop = wcslen(arg_desktop);
    const size_t len_desktop_quotes = wcslen(arg_desktop_quotes);

    LPWSTR p = commandline;

    while (*p)
    {
        while (iswspace(*p)) p++;
        if (wcsnicmp(p, arg_n, len_n )==0)
        {
            parameters->explorer_mode = FALSE;
            p += len_n;
            if (*p == ',') p++;
        }
        else if (wcsnicmp(p, arg_e, len_e )==0)
        {
            parameters->explorer_mode = TRUE;
            p += len_e;
        }
        else if (wcsnicmp(p, L"/e", 2) == 0 && (p[2] == 0 || iswspace(p[2])))
        {
            parameters->explorer_mode = TRUE;
            p += 2;
        }
        else if (wcsnicmp(p, arg_cd, len_cd )==0)
        {
            p += len_cd;
            p = copy_path_string(parameters->root,p);
        }
        else if (wcsnicmp(p, arg_root, len_root )==0)
        {
            p += len_root;
            p = copy_path_string(parameters->root,p);
        }
        else if (wcsnicmp(p, arg_select, len_select )==0)
        {
            p += len_select;
            p = copy_path_string(parameters->selection,p);
            if (!parameters->root[0])
                copy_path_root(parameters->root,
                               parameters->selection);
        }
        else if (wcsnicmp(p, arg_desktop, len_desktop )==0)
        {
            p += len_desktop;
            manage_desktop( p );  /* the rest of the command line is handled by desktop mode */
        }
        /* workaround for Worms Armageddon that hardcodes a /desktop option with quotes */
        else if (wcsnicmp(p, arg_desktop_quotes, len_desktop_quotes )==0)
        {
            p += len_desktop_quotes;
            manage_desktop( p );  /* the rest of the command line is handled by desktop mode */
        }
        else if (wcsnicmp(p, L"/theme", 6) == 0 && (p[6] == ' ' || p[6] == '=' || p[6] == ':'))
        {
            /* explorer /theme <file.theme>: apply a Windows theme file (visual style, colors, sounds, icons) */
            THEME_CONFIG cfg;
            WCHAR file[MAX_PATH];
            p += 7;
            while (*p == ' ') p++;
            copy_path_string(file, p);
            if (theme_load_file(file, &cfg)) theme_apply(&cfg);
            ExitProcess(0);
        }
        else if (wcsnicmp(p, L"/turnoff", 8) == 0 || wcsnicmp(p, L"-turnoff", 8) == 0)
        {
            /* the Welcome screen's Turn off computer (logonui.exe) */
            power_mgmt_show_shutdown_dialog(NULL);
            ExitProcess(0);
        }
        else if (wcsnicmp(p, L"/wlan", 5) == 0 || wcsnicmp(p, L"-wlan", 5) == 0)
        {
            wlan_dialog_show(NULL);
            ExitProcess(0);
        }
        else if (wcsnicmp(p, L"/netstatus", 10) == 0 || wcsnicmp(p, L"-netstatus", 10) == 0)
        {
            wlan_status_show(NULL);
            ExitProcess(0);
        }
        else if (wcsnicmp(p, L"/netprops", 9) == 0 || wcsnicmp(p, L"-netprops", 9) == 0 ||
                 wcsnicmp(p, L"/lanprops", 9) == 0 || wcsnicmp(p, L"-lanprops", 9) == 0)
        {
            net_props_show(NULL);
            ExitProcess(0);
        }
        else
        {
            /* left over command line is generally the path to be opened */
            copy_path_string(parameters->root,p);
            break;
        }
    }
}

int WINAPI wWinMain(HINSTANCE hinstance,
                    HINSTANCE previnstance,
                    LPWSTR cmdline,
                    int cmdshow)
{

    parameters_struct   parameters;
    HRESULT hres;
    INITCOMMONCONTROLSEX init_info;

    memset(&parameters,0,sizeof(parameters));
    explorer_hInstance = hinstance;
    parse_command_line(cmdline,&parameters);
    hres = OleInitialize(NULL);
    if(FAILED(hres))
    {
        ERR( "Could not initialize COM\n" );
        ExitProcess(EXIT_FAILURE);
    }
    if(parameters.root[0] && !PathIsDirectoryW(parameters.root))
        if(ShellExecuteW(NULL,NULL,parameters.root,NULL,NULL,SW_SHOWDEFAULT) > (HINSTANCE)32)
            ExitProcess(EXIT_SUCCESS);
    init_info.dwSize = sizeof(INITCOMMONCONTROLSEX);
    init_info.dwICC = ICC_USEREX_CLASSES | ICC_BAR_CLASSES | ICC_COOL_CLASSES | ICC_DATE_CLASSES | ICC_UPDOWN_CLASS |
                      ICC_LISTVIEW_CLASSES;
    if(!InitCommonControlsEx(&init_info))
    {
        ERR( "Could not initialize Comctl\n" );
        ExitProcess(EXIT_FAILURE);
    }
    register_explorer_window_class();
    make_explorer_window(&parameters);
    return 0;
}
