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
    LONG ref;
    struct hstring_map_entry *entries;
    UINT32 count;
    UINT32 capacity;
};

static inline struct hstring_map *impl_from_IMap_HSTRING_HSTRING( IMap_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_map, IMap_HSTRING_HSTRING_iface );
}

static HRESULT hstring_copy( HSTRING source, HSTRING *dest )
{
    UINT32 length;
    const WCHAR *buffer = WindowsGetStringRawBuffer( source, &length );
    return WindowsCreateString( buffer, length, dest );
}

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
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IMap`2<System.String, System.String>"),
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
    int index;

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    *value = NULL;

    index = hstring_map_find( impl, key );
    if (index < 0) return E_BOUNDS;

    return hstring_copy( impl->entries[index].value, value );
}

static HRESULT WINAPI hstring_map_get_Size( IMap_HSTRING_HSTRING *iface, UINT32 *value )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI hstring_map_HasKey( IMap_HSTRING_HSTRING *iface, HSTRING key, boolean *value )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    int index;

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    index = hstring_map_find( impl, key );
    *value = index >= 0;
    return S_OK;
}

static HRESULT WINAPI hstring_map_GetView( IMap_HSTRING_HSTRING *iface, IMapView_HSTRING_HSTRING **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI hstring_map_Insert( IMap_HSTRING_HSTRING *iface, HSTRING key, HSTRING value, boolean *replaced )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    int index;
    HRESULT hr;

    TRACE( "iface %p, key %s, value %s, replaced %p.\n", iface, debugstr_hstring( key ), debugstr_hstring( value ), replaced );

    if (!replaced) return E_POINTER;
    *replaced = FALSE;

    index = hstring_map_find( impl, key );
    if (index >= 0)
    {
        HSTRING new_value;
        if (FAILED(hr = hstring_copy( value, &new_value ))) return hr;
        WindowsDeleteString( impl->entries[index].value );
        impl->entries[index].value = new_value;
        *replaced = TRUE;
        return S_OK;
    }

    if (impl->count == impl->capacity)
    {
        UINT32 new_capacity = impl->capacity ? impl->capacity * 2 : 4;
        struct hstring_map_entry *new_entries = realloc( impl->entries, new_capacity * sizeof(*new_entries) );
        if (!new_entries) return E_OUTOFMEMORY;
        impl->entries = new_entries;
        impl->capacity = new_capacity;
    }

    if (FAILED(hr = hstring_copy( key, &impl->entries[impl->count].key ))) return hr;
    if (FAILED(hr = hstring_copy( value, &impl->entries[impl->count].value )))
    {
        WindowsDeleteString( impl->entries[impl->count].key );
        return hr;
    }
    impl->count++;
    return S_OK;
}

static HRESULT WINAPI hstring_map_Remove( IMap_HSTRING_HSTRING *iface, HSTRING key )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    int index;

    TRACE( "iface %p, key %s.\n", iface, debugstr_hstring( key ) );

    index = hstring_map_find( impl, key );
    if (index < 0) return E_BOUNDS;

    WindowsDeleteString( impl->entries[index].key );
    WindowsDeleteString( impl->entries[index].value );
    memmove( &impl->entries[index], &impl->entries[index + 1], (impl->count - index - 1) * sizeof(*impl->entries) );
    impl->count--;
    return S_OK;
}

static HRESULT WINAPI hstring_map_Clear( IMap_HSTRING_HSTRING *iface )
{
    struct hstring_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    UINT32 i;

    TRACE( "iface %p.\n", iface );

    for (i = 0; i < impl->count; i++)
    {
        WindowsDeleteString( impl->entries[i].key );
        WindowsDeleteString( impl->entries[i].value );
    }
    impl->count = 0;
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
    if (FAILED(hr = hstring_copy( key, &impl->key )) || FAILED(hr = hstring_copy( value, &impl->value )))
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
    HRESULT hr;
    UINT32 i;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IIterator_IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &hstring_iterator_vtbl;
    impl->ref = 1;
    if (map->count && !(impl->pairs = calloc( map->count, sizeof(*impl->pairs) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < map->count; i++)
    {
        if (FAILED(hr = hstring_pair_create( map->entries[i].key, map->entries[i].value, &impl->pairs[i] )))
        {
            hstring_iterator_Release( &impl->IIterator_IKeyValuePair_HSTRING_HSTRING_iface );
            return hr;
        }
        impl->count++;
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
    for (i = 0; i < impl->count; i++)
        FIXME( "%s%s = %s\n", prefix, debugstr_hstring( impl->entries[i].key ), debugstr_hstring( impl->entries[i].value ) );
}

HRESULT map_hstring_create( IMap_HSTRING_HSTRING **out )
{
    struct hstring_map *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IMap_HSTRING_HSTRING_iface.lpVtbl = &hstring_map_vtbl;
    impl->IIterable_IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &hstring_map_iterable_vtbl;
    impl->ref = 1;

    *out = &impl->IMap_HSTRING_HSTRING_iface;
    TRACE( "created IMap_HSTRING_HSTRING %p\n", *out );
    return S_OK;
}
