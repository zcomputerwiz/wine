/* WinRT Windows.Security.Authentication.Web.Core Implementation
 *
 * Copyright 2026 WineExplorer contributors
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

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(webcore);

struct hstring_map_entry
{
    HSTRING key;
    HSTRING value;
};

struct hstring_map
{
    IMap_HSTRING_HSTRING IMap_HSTRING_HSTRING_iface;
    IIterable_IKeyValuePair_HSTRING_HSTRING IIterable_IKeyValuePair_HSTRING_HSTRING_iface;
    IMapView_HSTRING_HSTRING IMapView_HSTRING_HSTRING_iface;
    LONG ref;
    /* the entries: a request's maps are read on the broker's thread while the program may change them */
    CRITICAL_SECTION cs;
    struct hstring_map_entry *entries;
    UINT32 count;
    UINT32 capacity;
};

static inline struct hstring_map *impl_from_IMap_HSTRING_HSTRING( IMap_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_map, IMap_HSTRING_HSTRING_iface );
}

/* room for another entry; the caller holds the lock */
static BOOL hstring_map_grow( struct hstring_map *impl )
{
    UINT32 capacity = impl->capacity ? impl->capacity * 2 : 4;
    struct hstring_map_entry *entries = realloc( impl->entries, capacity * sizeof(*entries) );

    if (!entries) return FALSE;
    impl->entries = entries;
    impl->capacity = capacity;
    return TRUE;
}

/* the caller holds the lock */
static int hstring_map_find( struct hstring_map *impl, HSTRING key )
{
    UINT32 i;
    INT32 order;

    for (i = 0; i < impl->count; i++)
        if (SUCCEEDED(WindowsCompareStringOrdinal( impl->entries[i].key, key, &order )) && !order) return (int)i;
    return -1;
}

static HRESULT WINAPI hstring_map_QueryInterface( IMap_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IMap_HSTRING_HSTRING ))
    {
        *out = &impl->IMap_HSTRING_HSTRING_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_HSTRING ))
    {
        *out = &impl->IIterable_IKeyValuePair_HSTRING_HSTRING_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IMapView_HSTRING_HSTRING ))
    {
        *out = &impl->IMapView_HSTRING_HSTRING_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hstring_map_AddRef( IMap_HSTRING_HSTRING *iface )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI hstring_map_Release( IMap_HSTRING_HSTRING *iface )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        UINT32 i;
        for (i = 0; i < impl->count; i++)
        {
            WindowsDeleteString( impl->entries[i].key );
            WindowsDeleteString( impl->entries[i].value );
        }
        free( impl->entries );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI hstring_map_GetIids( IMap_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_map_GetRuntimeClassName( IMap_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IMap`2<System.String, System.String>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IMap`2<System.String, System.String>") - 1,
                                class_name );
}

static HRESULT WINAPI hstring_map_GetTrustLevel( IMap_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_map_Lookup( IMap_HSTRING_HSTRING *iface, HSTRING key, HSTRING *value )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    HRESULT hr = E_BOUNDS;
    int index;

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    *value = NULL;

    EnterCriticalSection( &impl->cs );
    if ((index = hstring_map_find( impl, key )) >= 0)
        hr = WindowsDuplicateString( impl->entries[index].value, value );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI hstring_map_get_Size( IMap_HSTRING_HSTRING *iface, UINT32 *value )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *value = impl->count;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI hstring_map_HasKey( IMap_HSTRING_HSTRING *iface, HSTRING key, boolean *value )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *value = hstring_map_find( impl, key ) >= 0;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI hstring_map_GetView( IMap_HSTRING_HSTRING *iface, IMapView_HSTRING_HSTRING **value )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    /* the view is the map itself, read-only through its interface, and follows later changes */
    *value = &impl->IMapView_HSTRING_HSTRING_iface;
    IMapView_HSTRING_HSTRING_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI hstring_map_Insert( IMap_HSTRING_HSTRING *iface, HSTRING key, HSTRING value, boolean *replaced )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    HSTRING new_key = NULL, new_value;
    int index;
    HRESULT hr;

    TRACE( "iface %p, key %s, value %s, replaced %p.\n", iface, debugstr_hstring( key ), debugstr_hstring( value ), replaced );

    if (!replaced) return E_POINTER;
    *replaced = FALSE;
    if (FAILED(hr = WindowsDuplicateString( value, &new_value ))) return hr;

    EnterCriticalSection( &impl->cs );
    if ((index = hstring_map_find( impl, key )) >= 0)
    {
        WindowsDeleteString( impl->entries[index].value );
        impl->entries[index].value = new_value;
        *replaced = TRUE;
    }
    else if (impl->count == impl->capacity && !hstring_map_grow( impl ))
        hr = E_OUTOFMEMORY;
    else if (SUCCEEDED(hr = WindowsDuplicateString( key, &new_key )))
    {
        impl->entries[impl->count].key = new_key;
        impl->entries[impl->count].value = new_value;
        impl->count++;
    }
    LeaveCriticalSection( &impl->cs );

    if (FAILED(hr)) WindowsDeleteString( new_value );
    return hr;
}

static HRESULT WINAPI hstring_map_Remove( IMap_HSTRING_HSTRING *iface, HSTRING key )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    HSTRING old_key = NULL, old_value = NULL;
    int index;

    TRACE( "iface %p, key %s.\n", iface, debugstr_hstring( key ) );

    EnterCriticalSection( &impl->cs );
    if ((index = hstring_map_find( impl, key )) >= 0)
    {
        old_key = impl->entries[index].key;
        old_value = impl->entries[index].value;
        memmove( &impl->entries[index], &impl->entries[index + 1], (impl->count - index - 1) * sizeof(*impl->entries) );
        impl->count--;
    }
    LeaveCriticalSection( &impl->cs );

    if (index < 0) return E_BOUNDS;
    WindowsDeleteString( old_key );
    WindowsDeleteString( old_value );
    return S_OK;
}

static HRESULT WINAPI hstring_map_Clear( IMap_HSTRING_HSTRING *iface )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    UINT32 i;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++)
    {
        WindowsDeleteString( impl->entries[i].key );
        WindowsDeleteString( impl->entries[i].value );
    }
    impl->count = 0;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static const struct IMap_HSTRING_HSTRINGVtbl hstring_map_vtbl =
{
    /* IUnknown methods */
    hstring_map_QueryInterface,
    hstring_map_AddRef,
    hstring_map_Release,
    /* IInspectable methods */
    hstring_map_GetIids,
    hstring_map_GetRuntimeClassName,
    hstring_map_GetTrustLevel,
    /* IMap methods */
    hstring_map_Lookup,
    hstring_map_get_Size,
    hstring_map_HasKey,
    hstring_map_GetView,
    hstring_map_Insert,
    hstring_map_Remove,
    hstring_map_Clear,
};

/* IMapView<HSTRING, HSTRING>: the same object as the map, so one reference count */
static inline struct hstring_map *impl_from_IMapView_HSTRING_HSTRING( IMapView_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_map, IMapView_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI hstring_view_QueryInterface( IMapView_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    return hstring_map_QueryInterface( &impl_from_IMapView_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, iid, out );
}

static ULONG WINAPI hstring_view_AddRef( IMapView_HSTRING_HSTRING *iface )
{
    return hstring_map_AddRef( &impl_from_IMapView_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface );
}

static ULONG WINAPI hstring_view_Release( IMapView_HSTRING_HSTRING *iface )
{
    return hstring_map_Release( &impl_from_IMapView_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI hstring_view_GetIids( IMapView_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_view_GetRuntimeClassName( IMapView_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IMapView`2<System.String, System.String>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IMapView`2<System.String, System.String>") - 1,
                                class_name );
}

static HRESULT WINAPI hstring_view_GetTrustLevel( IMapView_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_view_Lookup( IMapView_HSTRING_HSTRING *iface, HSTRING key, HSTRING *value )
{
    return hstring_map_Lookup( &impl_from_IMapView_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, key, value );
}

static HRESULT WINAPI hstring_view_get_Size( IMapView_HSTRING_HSTRING *iface, UINT32 *value )
{
    return hstring_map_get_Size( &impl_from_IMapView_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, value );
}

static HRESULT WINAPI hstring_view_HasKey( IMapView_HSTRING_HSTRING *iface, HSTRING key, boolean *value )
{
    return hstring_map_HasKey( &impl_from_IMapView_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, key, value );
}

static HRESULT WINAPI hstring_view_Split( IMapView_HSTRING_HSTRING *iface, IMapView_HSTRING_HSTRING **first,
                                          IMapView_HSTRING_HSTRING **second )
{
    FIXME( "iface %p, first %p, second %p stub!\n", iface, first, second );
    return E_NOTIMPL;
}

static const struct IMapView_HSTRING_HSTRINGVtbl hstring_view_vtbl =
{
    /* IUnknown methods */
    hstring_view_QueryInterface,
    hstring_view_AddRef,
    hstring_view_Release,
    /* IInspectable methods */
    hstring_view_GetIids,
    hstring_view_GetRuntimeClassName,
    hstring_view_GetTrustLevel,
    /* IMapView methods */
    hstring_view_Lookup,
    hstring_view_get_Size,
    hstring_view_HasKey,
    hstring_view_Split,
};

/* IKeyValuePair<HSTRING, HSTRING>: one entry, copied */
struct hstring_pair
{
    IKeyValuePair_HSTRING_HSTRING IKeyValuePair_HSTRING_HSTRING_iface;
    LONG ref;
    HSTRING key;
    HSTRING value;
};

static inline struct hstring_pair *impl_from_IKeyValuePair_HSTRING_HSTRING( IKeyValuePair_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_pair, IKeyValuePair_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI hstring_pair_QueryInterface( IKeyValuePair_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IKeyValuePair_HSTRING_HSTRING ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hstring_pair_AddRef( IKeyValuePair_HSTRING_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IKeyValuePair_HSTRING_HSTRING( iface )->ref );
}

static ULONG WINAPI hstring_pair_Release( IKeyValuePair_HSTRING_HSTRING *iface )
{
    struct hstring_pair *impl = impl_from_IKeyValuePair_HSTRING_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->key );
        WindowsDeleteString( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI hstring_pair_GetIids( IKeyValuePair_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_pair_GetRuntimeClassName( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_pair_GetTrustLevel( IKeyValuePair_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_pair_get_Key( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *key )
{
    TRACE( "iface %p, key %p.\n", iface, key );
    if (!key) return E_POINTER;
    return WindowsDuplicateString( impl_from_IKeyValuePair_HSTRING_HSTRING( iface )->key, key );
}

static HRESULT WINAPI hstring_pair_get_Value( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IKeyValuePair_HSTRING_HSTRING( iface )->value, value );
}

static const struct IKeyValuePair_HSTRING_HSTRINGVtbl hstring_pair_vtbl =
{
    hstring_pair_QueryInterface,
    hstring_pair_AddRef,
    hstring_pair_Release,
    hstring_pair_GetIids,
    hstring_pair_GetRuntimeClassName,
    hstring_pair_GetTrustLevel,
    hstring_pair_get_Key,
    hstring_pair_get_Value,
};

static HRESULT hstring_pair_create( HSTRING key, HSTRING value, IKeyValuePair_HSTRING_HSTRING **out )
{
    struct hstring_pair *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &hstring_pair_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( key, &impl->key )) ||
        FAILED(hr = WindowsDuplicateString( value, &impl->value )))
    {
        hstring_pair_Release( &impl->IKeyValuePair_HSTRING_HSTRING_iface );
        return hr;
    }
    *out = &impl->IKeyValuePair_HSTRING_HSTRING_iface;
    return S_OK;
}

/* IIterator<IKeyValuePair<HSTRING, HSTRING>>: over a copy of the map's entries taken by First */
struct hstring_iterator
{
    IIterator_IKeyValuePair_HSTRING_HSTRING IIterator_IKeyValuePair_HSTRING_HSTRING_iface;
    LONG ref;
    IKeyValuePair_HSTRING_HSTRING **pairs;
    UINT32 count;
    UINT32 index;
};

static inline struct hstring_iterator *impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( IIterator_IKeyValuePair_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterator, IIterator_IKeyValuePair_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI hstring_iterator_QueryInterface( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_IKeyValuePair_HSTRING_HSTRING ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI hstring_iterator_AddRef( IIterator_IKeyValuePair_HSTRING_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface )->ref );
}

static ULONG WINAPI hstring_iterator_Release( IIterator_IKeyValuePair_HSTRING_HSTRING *iface )
{
    struct hstring_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IKeyValuePair_HSTRING_HSTRING_Release( impl->pairs[i] );
        free( impl->pairs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI hstring_iterator_GetIids( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_iterator_GetRuntimeClassName( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_iterator_GetTrustLevel( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_iterator_get_Current( IIterator_IKeyValuePair_HSTRING_HSTRING *iface,
                                                    IKeyValuePair_HSTRING_HSTRING **value )
{
    struct hstring_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (impl->index >= impl->count) return E_BOUNDS;
    IKeyValuePair_HSTRING_HSTRING_AddRef( (*value = impl->pairs[impl->index]) );
    return S_OK;
}

static HRESULT WINAPI hstring_iterator_get_HasCurrent( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->index < impl->count;
    return S_OK;
}

static HRESULT WINAPI hstring_iterator_MoveNext( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, boolean *value )
{
    struct hstring_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->index < impl->count) impl->index++;
    *value = impl->index < impl->count;
    return S_OK;
}

static HRESULT WINAPI hstring_iterator_GetMany( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, UINT32 items_size,
                                                IKeyValuePair_HSTRING_HSTRING **items, UINT32 *count )
{
    struct hstring_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );
    UINT32 i;

    TRACE( "iface %p, items_size %u, items %p, count %p.\n", iface, items_size, items, count );

    if (!count) return E_POINTER;
    for (i = 0; i < items_size && impl->index < impl->count; i++, impl->index++)
        IKeyValuePair_HSTRING_HSTRING_AddRef( (items[i] = impl->pairs[impl->index]) );
    *count = i;
    return S_OK;
}

static const struct IIterator_IKeyValuePair_HSTRING_HSTRINGVtbl hstring_iterator_vtbl =
{
    hstring_iterator_QueryInterface,
    hstring_iterator_AddRef,
    hstring_iterator_Release,
    hstring_iterator_GetIids,
    hstring_iterator_GetRuntimeClassName,
    hstring_iterator_GetTrustLevel,
    hstring_iterator_get_Current,
    hstring_iterator_get_HasCurrent,
    hstring_iterator_MoveNext,
    hstring_iterator_GetMany,
};

/* IIterable<IKeyValuePair<HSTRING, HSTRING>> of the map */
static inline struct hstring_map *impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( IIterable_IKeyValuePair_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_map, IIterable_IKeyValuePair_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI hstring_map_iterable_QueryInterface( IIterable_IKeyValuePair_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    return hstring_map_QueryInterface( &impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, iid, out );
}

static ULONG WINAPI hstring_map_iterable_AddRef( IIterable_IKeyValuePair_HSTRING_HSTRING *iface )
{
    return hstring_map_AddRef( &impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface );
}

static ULONG WINAPI hstring_map_iterable_Release( IIterable_IKeyValuePair_HSTRING_HSTRING *iface )
{
    return hstring_map_Release( &impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI hstring_map_iterable_GetIids( IIterable_IKeyValuePair_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    return hstring_map_GetIids( &impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, iid_count, iids );
}

static HRESULT WINAPI hstring_map_iterable_GetRuntimeClassName( IIterable_IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    return hstring_map_GetRuntimeClassName( &impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, class_name );
}

static HRESULT WINAPI hstring_map_iterable_GetTrustLevel( IIterable_IKeyValuePair_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    return hstring_map_GetTrustLevel( &impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface )->IMap_HSTRING_HSTRING_iface, trust_level );
}

static HRESULT WINAPI hstring_map_iterable_First( IIterable_IKeyValuePair_HSTRING_HSTRING *iface,
                                                  IIterator_IKeyValuePair_HSTRING_HSTRING **value )
{
    struct hstring_map *map = impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface );
    struct hstring_iterator *impl;
    HRESULT hr = S_OK;
    UINT32 i;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IIterator_IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &hstring_iterator_vtbl;
    impl->ref = 1;
    EnterCriticalSection( &map->cs );
    if (map->count && !(impl->pairs = calloc( map->count, sizeof(*impl->pairs) ))) hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED(hr) && i < map->count; i++)
        if (SUCCEEDED(hr = hstring_pair_create( map->entries[i].key, map->entries[i].value, &impl->pairs[i] )))
            impl->count++;
    LeaveCriticalSection( &map->cs );
    if (FAILED(hr))
    {
        hstring_iterator_Release( &impl->IIterator_IKeyValuePair_HSTRING_HSTRING_iface );
        return hr;
    }
    *value = &impl->IIterator_IKeyValuePair_HSTRING_HSTRING_iface;
    return S_OK;
}

static const struct IIterable_IKeyValuePair_HSTRING_HSTRINGVtbl hstring_map_iterable_vtbl =
{
    hstring_map_iterable_QueryInterface,
    hstring_map_iterable_AddRef,
    hstring_map_iterable_Release,
    hstring_map_iterable_GetIids,
    hstring_map_iterable_GetRuntimeClassName,
    hstring_map_iterable_GetTrustLevel,
    hstring_map_iterable_First,
};

/* the pairs of a map made by map_hstring_create, one line each (the diagnostic provider's request trace) */
void map_hstring_dump( IMap_HSTRING_HSTRING *iface, const char *prefix )
{
    struct hstring_map *impl;
    UINT32 i;

    if (!iface || iface->lpVtbl != &hstring_map_vtbl) return;
    impl = impl_from_IMap_HSTRING_HSTRING( iface );
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++)
        FIXME( "%s%s = %s\n", prefix, debugstr_hstring( impl->entries[i].key ), debugstr_hstring( impl->entries[i].value ) );
    LeaveCriticalSection( &impl->cs );
}

/* the pairs of a map made by map_hstring_create, as the token broker's protocol carries them ("key\0value\0...",
 * allocated; len in characters); pairs with a NUL character in them are left out */
HRESULT map_hstring_pairs( IMap_HSTRING_HSTRING *iface, WCHAR **text, UINT32 *len )
{
    struct hstring_map *impl;
    UINT32 i, key_len, value_len, total = 0;
    const WCHAR *key, *value;
    WCHAR *buffer, *pos;

    *text = NULL;
    *len = 0;
    if (!iface || iface->lpVtbl != &hstring_map_vtbl) return S_OK;
    impl = impl_from_IMap_HSTRING_HSTRING( iface );
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++)
    {
        WindowsGetStringRawBuffer( impl->entries[i].key, &key_len );
        WindowsGetStringRawBuffer( impl->entries[i].value, &value_len );
        total += key_len + value_len + 2;
    }
    if (!total || !(buffer = malloc( total * sizeof(WCHAR) )))
    {
        LeaveCriticalSection( &impl->cs );
        return total ? E_OUTOFMEMORY : S_OK;
    }
    for (i = 0, pos = buffer; i < impl->count; i++)
    {
        key = WindowsGetStringRawBuffer( impl->entries[i].key, &key_len );
        value = WindowsGetStringRawBuffer( impl->entries[i].value, &value_len );
        if (wcslen( key ) < key_len || wcslen( value ) < value_len) continue;
        memcpy( pos, key, key_len * sizeof(WCHAR) );
        pos += key_len;
        *pos++ = 0;
        memcpy( pos, value, value_len * sizeof(WCHAR) );
        pos += value_len;
        *pos++ = 0;
    }
    LeaveCriticalSection( &impl->cs );
    *text = buffer;
    *len = pos - buffer;
    return S_OK;
}

HRESULT map_hstring_create( IMap_HSTRING_HSTRING **out )
{
    struct hstring_map *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMap_HSTRING_HSTRING_iface.lpVtbl = &hstring_map_vtbl;
    impl->IIterable_IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &hstring_map_iterable_vtbl;
    impl->IMapView_HSTRING_HSTRING_iface.lpVtbl = &hstring_view_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": hstring_map.cs");

    *out = &impl->IMap_HSTRING_HSTRING_iface;
    TRACE( "created IMap_HSTRING_HSTRING %p\n", *out );
    return S_OK;
}

/* the map in *slot, created on first use (when two threads race, one map is kept and the other freed), with a
 * reference for the caller */
HRESULT map_hstring_get( IMap_HSTRING_HSTRING **slot, IMap_HSTRING_HSTRING **out )
{
    IMap_HSTRING_HSTRING *map;
    HRESULT hr;

    if (!*slot)
    {
        if (FAILED(hr = map_hstring_create( &map ))) return hr;
        if (InterlockedCompareExchangePointer( (void **)slot, map, NULL )) IMap_HSTRING_HSTRING_Release( map );
    }
    IMap_HSTRING_HSTRING_AddRef( (*out = *slot) );
    return S_OK;
}
