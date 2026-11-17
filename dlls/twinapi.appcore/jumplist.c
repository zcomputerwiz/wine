/* WinRT Windows.UI.StartScreen.JumpList and JumpListItem Implementation
 *
 * Copyright (C) 2026 WineExplorer contributors
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

/*
 * A jump list is a list of items a shell shows beside the application's tile. The shell of WineExplorer has no
 * jump lists to show yet, so SaveAsync keeps the list of the process (as the items' public properties) and
 * LoadCurrentAsync builds a new list from it, which is what an application sees on Windows within one run.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/* ---- JumpListItem ---- */

struct jump_list_item
{
    IJumpListItem IJumpListItem_iface;
    LONG ref;
    JumpListItemKind kind;
    HSTRING arguments;
    HSTRING description;
    HSTRING display_name;
    HSTRING group_name;
    IUriRuntimeClass *logo;
};

static inline struct jump_list_item *impl_from_IJumpListItem( IJumpListItem *iface )
{
    return CONTAINING_RECORD( iface, struct jump_list_item, IJumpListItem_iface );
}

static HRESULT WINAPI item_QueryInterface( IJumpListItem *iface, REFIID iid, void **out )
{
    struct jump_list_item *impl = impl_from_IJumpListItem( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IJumpListItem ))
    {
        IInspectable_AddRef( (*out = &impl->IJumpListItem_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI item_AddRef( IJumpListItem *iface )
{
    return InterlockedIncrement( &impl_from_IJumpListItem( iface )->ref );
}

static ULONG WINAPI item_Release( IJumpListItem *iface )
{
    struct jump_list_item *impl = impl_from_IJumpListItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->arguments );
        WindowsDeleteString( impl->description );
        WindowsDeleteString( impl->display_name );
        WindowsDeleteString( impl->group_name );
        if (impl->logo) IUriRuntimeClass_Release( impl->logo );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI item_GetIids( IJumpListItem *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IJumpListItem;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI item_GetRuntimeClassName( IJumpListItem *iface, HSTRING *name )
{
    return create_name( L"Windows.UI.StartScreen.JumpListItem", name );
}

static HRESULT WINAPI item_GetTrustLevel( IJumpListItem *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI item_get_Kind( IJumpListItem *iface, JumpListItemKind *value )
{
    *value = impl_from_IJumpListItem( iface )->kind;
    return S_OK;
}

static HRESULT WINAPI item_get_Arguments( IJumpListItem *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IJumpListItem( iface )->arguments, value );
}

static HRESULT WINAPI item_get_RemovedByUser( IJumpListItem *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT item_set_string( struct jump_list_item *impl, HSTRING *field, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( *field );
    *field = copy;
    return S_OK;
}

static HRESULT WINAPI item_get_Description( IJumpListItem *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IJumpListItem( iface )->description, value );
}

static HRESULT WINAPI item_put_Description( IJumpListItem *iface, HSTRING value )
{
    struct jump_list_item *impl = impl_from_IJumpListItem( iface );
    return item_set_string( impl, &impl->description, value );
}

static HRESULT WINAPI item_get_DisplayName( IJumpListItem *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IJumpListItem( iface )->display_name, value );
}

static HRESULT WINAPI item_put_DisplayName( IJumpListItem *iface, HSTRING value )
{
    struct jump_list_item *impl = impl_from_IJumpListItem( iface );
    return item_set_string( impl, &impl->display_name, value );
}

static HRESULT WINAPI item_get_GroupName( IJumpListItem *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IJumpListItem( iface )->group_name, value );
}

static HRESULT WINAPI item_put_GroupName( IJumpListItem *iface, HSTRING value )
{
    struct jump_list_item *impl = impl_from_IJumpListItem( iface );
    return item_set_string( impl, &impl->group_name, value );
}

static HRESULT WINAPI item_get_Logo( IJumpListItem *iface, IUriRuntimeClass **value )
{
    if ((*value = impl_from_IJumpListItem( iface )->logo)) IUriRuntimeClass_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI item_put_Logo( IJumpListItem *iface, IUriRuntimeClass *value )
{
    struct jump_list_item *impl = impl_from_IJumpListItem( iface );

    if (value) IUriRuntimeClass_AddRef( value );
    if (impl->logo) IUriRuntimeClass_Release( impl->logo );
    impl->logo = value;
    return S_OK;
}

static const IJumpListItemVtbl item_vtbl =
{
    item_QueryInterface,
    item_AddRef,
    item_Release,
    /* IInspectable methods */
    item_GetIids,
    item_GetRuntimeClassName,
    item_GetTrustLevel,
    /* IJumpListItem methods */
    item_get_Kind,
    item_get_Arguments,
    item_get_RemovedByUser,
    item_get_Description,
    item_put_Description,
    item_get_DisplayName,
    item_put_DisplayName,
    item_get_GroupName,
    item_put_GroupName,
    item_get_Logo,
    item_put_Logo,
};

static HRESULT item_create( JumpListItemKind kind, HSTRING arguments, HSTRING display_name, IJumpListItem **out )
{
    struct jump_list_item *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IJumpListItem_iface.lpVtbl = &item_vtbl;
    impl->ref = 1;
    impl->kind = kind;
    if (FAILED(hr = WindowsDuplicateString( arguments, &impl->arguments )) ||
        FAILED(hr = WindowsDuplicateString( display_name, &impl->display_name )))
    {
        IJumpListItem_Release( &impl->IJumpListItem_iface );
        return hr;
    }
    *out = &impl->IJumpListItem_iface;
    return S_OK;
}

static HRESULT WINAPI item_statics_CreateWithArguments( IJumpListItemStatics *iface, HSTRING arguments, HSTRING display_name,
                                                        IJumpListItem **result )
{
    TRACE( "iface %p, arguments %s, display_name %s, result %p.\n", iface, debugstr_hstring( arguments ),
           debugstr_hstring( display_name ), result );

    *result = NULL;
    return item_create( JumpListItemKind_Arguments, arguments, display_name, result );
}

static HRESULT WINAPI item_statics_CreateSeparator( IJumpListItemStatics *iface, IJumpListItem **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return item_create( JumpListItemKind_Separator, NULL, NULL, result );
}

DEFINE_STATICS_IINSPECTABLE( item_statics, IJumpListItemStatics )

static const IJumpListItemStaticsVtbl item_statics_vtbl =
{
    item_statics_QueryInterface,
    item_statics_AddRef,
    item_statics_Release,
    /* IInspectable methods */
    item_statics_GetIids,
    item_statics_GetRuntimeClassName,
    item_statics_GetTrustLevel,
    /* IJumpListItemStatics methods */
    item_statics_CreateWithArguments,
    item_statics_CreateSeparator,
};

static struct static_factory item_factory =
{
    {&static_factory_vtbl},
    1,
    L"Windows.UI.StartScreen.JumpListItem",
    NULL,
    1,
    {{ &IID_IJumpListItemStatics, { &item_statics_vtbl, &item_factory } }},
};

IActivationFactory *jump_list_item_factory = &item_factory.IActivationFactory_iface;

/* ---- the list kept by SaveAsync ---- */

struct saved_item
{
    JumpListItemKind kind;
    HSTRING arguments;
    HSTRING description;
    HSTRING display_name;
    HSTRING group_name;
    IUriRuntimeClass *logo;
};

static CRITICAL_SECTION saved_cs;
static CRITICAL_SECTION_DEBUG saved_cs_debug =
{
    0, 0, &saved_cs,
    { &saved_cs_debug.ProcessLocksList, &saved_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": saved_cs") }
};
static CRITICAL_SECTION saved_cs = { &saved_cs_debug, -1, 0, 0, 0, 0 };
static struct saved_item *saved_items;
static UINT32 saved_count;
/* measured on Windows 11: a list never saved has the Recent group */
static JumpListSystemGroupKind saved_group_kind = JumpListSystemGroupKind_Recent;

static void saved_item_clear( struct saved_item *saved )
{
    WindowsDeleteString( saved->arguments );
    WindowsDeleteString( saved->description );
    WindowsDeleteString( saved->display_name );
    WindowsDeleteString( saved->group_name );
    if (saved->logo) IUriRuntimeClass_Release( saved->logo );
    memset( saved, 0, sizeof(*saved) );
}

static void saved_clear(void)
{
    UINT32 i;

    for (i = 0; i < saved_count; i++) saved_item_clear( &saved_items[i] );
    free( saved_items );
    saved_items = NULL;
    saved_count = 0;
}

static HRESULT saved_item_read( IJumpListItem *item, struct saved_item *saved )
{
    HRESULT hr;

    memset( saved, 0, sizeof(*saved) );
    if (FAILED(hr = IJumpListItem_get_Kind( item, &saved->kind ))) return hr;
    if (FAILED(hr = IJumpListItem_get_Arguments( item, &saved->arguments ))) goto failed;
    if (FAILED(hr = IJumpListItem_get_Description( item, &saved->description ))) goto failed;
    if (FAILED(hr = IJumpListItem_get_DisplayName( item, &saved->display_name ))) goto failed;
    if (FAILED(hr = IJumpListItem_get_GroupName( item, &saved->group_name ))) goto failed;
    if (FAILED(hr = IJumpListItem_get_Logo( item, &saved->logo ))) goto failed;
    return S_OK;
failed:
    saved_item_clear( saved );
    return hr;
}

static HRESULT saved_item_make( const struct saved_item *saved, IJumpListItem **out )
{
    struct jump_list_item *impl;
    HRESULT hr;

    if (FAILED(hr = item_create( saved->kind, saved->arguments, saved->display_name, out ))) return hr;
    impl = impl_from_IJumpListItem( *out );
    if (FAILED(hr = WindowsDuplicateString( saved->description, &impl->description )) ||
        FAILED(hr = WindowsDuplicateString( saved->group_name, &impl->group_name )))
    {
        IJumpListItem_Release( *out );
        *out = NULL;
        return hr;
    }
    if ((impl->logo = saved->logo)) IUriRuntimeClass_AddRef( impl->logo );
    return S_OK;
}

/* ---- JumpList ---- */

struct jump_list
{
    IJumpList IJumpList_iface;
    LONG ref;
    IVector_JumpListItem *items;
    JumpListSystemGroupKind group_kind;
};

static inline struct jump_list *impl_from_IJumpList( IJumpList *iface )
{
    return CONTAINING_RECORD( iface, struct jump_list, IJumpList_iface );
}

static HRESULT WINAPI list_QueryInterface( IJumpList *iface, REFIID iid, void **out )
{
    struct jump_list *impl = impl_from_IJumpList( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IJumpList ))
    {
        IInspectable_AddRef( (*out = &impl->IJumpList_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI list_AddRef( IJumpList *iface )
{
    return InterlockedIncrement( &impl_from_IJumpList( iface )->ref );
}

static ULONG WINAPI list_Release( IJumpList *iface )
{
    struct jump_list *impl = impl_from_IJumpList( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVector_JumpListItem_Release( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI list_GetIids( IJumpList *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IJumpList;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI list_GetRuntimeClassName( IJumpList *iface, HSTRING *name )
{
    return create_name( L"Windows.UI.StartScreen.JumpList", name );
}

static HRESULT WINAPI list_GetTrustLevel( IJumpList *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI list_get_Items( IJumpList *iface, IVector_JumpListItem **value )
{
    struct jump_list *impl = impl_from_IJumpList( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    IVector_JumpListItem_AddRef( (*value = impl->items) );
    return S_OK;
}

static HRESULT WINAPI list_get_SystemGroupKind( IJumpList *iface, JumpListSystemGroupKind *value )
{
    *value = impl_from_IJumpList( iface )->group_kind;
    return S_OK;
}

static HRESULT WINAPI list_put_SystemGroupKind( IJumpList *iface, JumpListSystemGroupKind value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    if (value < JumpListSystemGroupKind_None || value > JumpListSystemGroupKind_Recent) return E_INVALIDARG;
    impl_from_IJumpList( iface )->group_kind = value;
    return S_OK;
}

static HRESULT WINAPI list_SaveAsync( IJumpList *iface, IAsyncAction **result )
{
    struct jump_list *impl = impl_from_IJumpList( iface );
    struct saved_item *items = NULL;
    UINT32 i, count = 0;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = IVector_JumpListItem_get_Size( impl->items, &count ))) return hr;
    if (count && !(items = calloc( count, sizeof(*items) ))) return E_OUTOFMEMORY;
    for (i = 0; i < count; i++)
    {
        IJumpListItem *item;

        if (FAILED(hr = IVector_JumpListItem_GetAt( impl->items, i, &item ))) break;
        hr = saved_item_read( item, &items[i] );
        IJumpListItem_Release( item );
        if (FAILED(hr)) break;
    }
    if (FAILED(hr))
    {
        while (i--) saved_item_clear( &items[i] );
        free( items );
        return hr;
    }

    EnterCriticalSection( &saved_cs );
    saved_clear();
    saved_items = items;
    saved_count = count;
    saved_group_kind = impl->group_kind;
    LeaveCriticalSection( &saved_cs );
    return async_action_completed( S_OK, result );
}

static const IJumpListVtbl list_vtbl =
{
    list_QueryInterface,
    list_AddRef,
    list_Release,
    /* IInspectable methods */
    list_GetIids,
    list_GetRuntimeClassName,
    list_GetTrustLevel,
    /* IJumpList methods */
    list_get_Items,
    list_get_SystemGroupKind,
    list_put_SystemGroupKind,
    list_SaveAsync,
};

static HRESULT list_create_from_saved( IJumpList **out )
{
    struct jump_list *impl;
    UINT32 i;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IJumpList_iface.lpVtbl = &list_vtbl;
    impl->ref = 1;
    if (FAILED(hr = jump_list_item_vector_create( TRUE, NULL, 0, (void **)&impl->items )))
    {
        free( impl );
        return hr;
    }

    EnterCriticalSection( &saved_cs );
    impl->group_kind = saved_group_kind;
    for (i = 0; i < saved_count; i++)
    {
        IJumpListItem *item;

        if (FAILED(hr = saved_item_make( &saved_items[i], &item ))) break;
        hr = IVector_JumpListItem_Append( impl->items, item );
        IJumpListItem_Release( item );
        if (FAILED(hr)) break;
    }
    LeaveCriticalSection( &saved_cs );

    if (FAILED(hr))
    {
        IJumpList_Release( &impl->IJumpList_iface );
        return hr;
    }
    *out = &impl->IJumpList_iface;
    return S_OK;
}

static HRESULT WINAPI list_statics_LoadCurrentAsync( IJumpListStatics *iface, IAsyncOperation_JumpList **result )
{
    IJumpList *list;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    hr = list_create_from_saved( &list );
    if (FAILED(hr)) return hr;
    hr = async_jump_list_completed( list, S_OK, result );
    IJumpList_Release( list );
    return hr;
}

static HRESULT WINAPI list_statics_IsSupported( IJumpListStatics *iface, boolean *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    *result = TRUE;
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( list_statics, IJumpListStatics )

static const IJumpListStaticsVtbl list_statics_vtbl =
{
    list_statics_QueryInterface,
    list_statics_AddRef,
    list_statics_Release,
    /* IInspectable methods */
    list_statics_GetIids,
    list_statics_GetRuntimeClassName,
    list_statics_GetTrustLevel,
    /* IJumpListStatics methods */
    list_statics_LoadCurrentAsync,
    list_statics_IsSupported,
};

static struct static_factory list_factory =
{
    {&static_factory_vtbl},
    1,
    L"Windows.UI.StartScreen.JumpList",
    NULL,
    1,
    {{ &IID_IJumpListStatics, { &list_statics_vtbl, &list_factory } }},
};

IActivationFactory *jump_list_factory = &list_factory.IActivationFactory_iface;
