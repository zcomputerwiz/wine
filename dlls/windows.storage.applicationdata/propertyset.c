/*
 * PropertySet of the application data settings (WineExplorer UWP U1.4)
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

WINE_DEFAULT_DEBUG_CHANNEL(data);

/*
 * A map of strings to inspectable values that is also an IPropertySet: the values of an application data container
 * (LocalSettings.Values), and the ApplicationDataCompositeValue. Every change is reported to the owner through a
 * callback, which writes the settings to the registry; the callback may refuse a value.
 */

struct entry
{
    struct list entry;
    HSTRING key;
    IInspectable *value;
};

struct property_set
{
    IPropertySet IPropertySet_iface;
    IObservableMap_HSTRING_IInspectable IObservableMap_HSTRING_IInspectable_iface;
    IMap_HSTRING_IInspectable IMap_HSTRING_IInspectable_iface;
    IIterable_IKeyValuePair_HSTRING_IInspectable IIterable_IKeyValuePair_HSTRING_IInspectable_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    struct list entries;
    UINT32 size;
    const WCHAR *class_name;
    property_set_callback callback;
    void *context;
};

static inline struct property_set *impl_from_IPropertySet( IPropertySet *iface )
{
    return CONTAINING_RECORD( iface, struct property_set, IPropertySet_iface );
}

static HRESULT WINAPI property_set_QueryInterface( IPropertySet *iface, REFIID iid, void **out )
{
    struct property_set *impl = impl_from_IPropertySet( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPropertySet ))
        *out = &impl->IPropertySet_iface;
    else if (IsEqualGUID( iid, &IID_IObservableMap_HSTRING_IInspectable ))
        *out = &impl->IObservableMap_HSTRING_IInspectable_iface;
    else if (IsEqualGUID( iid, &IID_IMap_HSTRING_IInspectable ))
        *out = &impl->IMap_HSTRING_IInspectable_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable ))
        *out = &impl->IIterable_IKeyValuePair_HSTRING_IInspectable_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI property_set_AddRef( IPropertySet *iface )
{
    return InterlockedIncrement( &impl_from_IPropertySet( iface )->ref );
}

static void entries_clear( struct property_set *impl )
{
    struct entry *e, *next;

    LIST_FOR_EACH_ENTRY_SAFE( e, next, &impl->entries, struct entry, entry )
    {
        list_remove( &e->entry );
        WindowsDeleteString( e->key );
        if (e->value) IInspectable_Release( e->value );
        free( e );
    }
    impl->size = 0;
}

static ULONG WINAPI property_set_Release( IPropertySet *iface )
{
    struct property_set *impl = impl_from_IPropertySet( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        entries_clear( impl );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI property_set_GetIids( IPropertySet *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI property_set_GetRuntimeClassName( IPropertySet *iface, HSTRING *name )
{
    struct property_set *impl = impl_from_IPropertySet( iface );

    return WindowsCreateString( impl->class_name, wcslen( impl->class_name ), name );
}

static HRESULT WINAPI property_set_GetTrustLevel( IPropertySet *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static const IPropertySetVtbl property_set_vtbl =
{
    property_set_QueryInterface,
    property_set_AddRef,
    property_set_Release,
    property_set_GetIids,
    property_set_GetRuntimeClassName,
    property_set_GetTrustLevel,
};

/* ---- the entries ------------------------------------------------------------------------------------- */

static struct entry *find_entry( struct property_set *impl, HSTRING key )
{
    struct entry *e;
    const WCHAR *wanted = WindowsGetStringRawBuffer( key, NULL );

    LIST_FOR_EACH_ENTRY( e, &impl->entries, struct entry, entry )
        if (!wcscmp( WindowsGetStringRawBuffer( e->key, NULL ), wanted )) return e;
    return NULL;
}

static void notify( struct property_set *impl )
{
    if (impl->callback) impl->callback( impl->context, &impl->IPropertySet_iface );
}

/* IObservableMap<HSTRING, IInspectable *> */

DEFINE_IINSPECTABLE( observable_map, IObservableMap_HSTRING_IInspectable, struct property_set, IPropertySet_iface )

static HRESULT WINAPI observable_map_add_MapChanged( IObservableMap_HSTRING_IInspectable *iface,
                                                     IMapChangedEventHandler_HSTRING_IInspectable *handler,
                                                     EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p: registered, never raised.\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 1;
    return S_OK;
}

static HRESULT WINAPI observable_map_remove_MapChanged( IObservableMap_HSTRING_IInspectable *iface, EventRegistrationToken token )
{
    return S_OK;
}

static const IObservableMap_HSTRING_IInspectableVtbl observable_map_vtbl =
{
    observable_map_QueryInterface,
    observable_map_AddRef,
    observable_map_Release,
    observable_map_GetIids,
    observable_map_GetRuntimeClassName,
    observable_map_GetTrustLevel,
    observable_map_add_MapChanged,
    observable_map_remove_MapChanged,
};

/* IMap<HSTRING, IInspectable *> */

DEFINE_IINSPECTABLE( map, IMap_HSTRING_IInspectable, struct property_set, IPropertySet_iface )

static HRESULT WINAPI map_Lookup( IMap_HSTRING_IInspectable *iface, HSTRING key, IInspectable **value )
{
    struct property_set *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    struct entry *e;
    HRESULT hr = E_BOUNDS;

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    EnterCriticalSection( &impl->cs );
    if ((e = find_entry( impl, key )))
    {
        if ((*value = e->value)) IInspectable_AddRef( *value );
        hr = S_OK;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI map_get_Size( IMap_HSTRING_IInspectable *iface, UINT32 *size )
{
    struct property_set *impl = impl_from_IMap_HSTRING_IInspectable( iface );

    if (!size) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *size = impl->size;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI map_HasKey( IMap_HSTRING_IInspectable *iface, HSTRING key, boolean *found )
{
    struct property_set *impl = impl_from_IMap_HSTRING_IInspectable( iface );

    if (!found) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *found = find_entry( impl, key ) != NULL;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI map_GetView( IMap_HSTRING_IInspectable *iface, IMapView_HSTRING_IInspectable **view )
{
    FIXME( "iface %p, view %p stub!\n", iface, view );
    return E_NOTIMPL;
}

static HRESULT WINAPI map_Insert( IMap_HSTRING_IInspectable *iface, HSTRING key, IInspectable *value, boolean *replaced )
{
    struct property_set *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    struct entry *e;
    IInspectable *old = NULL;
    HRESULT hr = S_OK;
    boolean was_there;

    TRACE( "iface %p, key %s, value %p, replaced %p.\n", iface, debugstr_hstring( key ), value, replaced );

    if (!WindowsGetStringRawBuffer( key, NULL )) return E_INVALIDARG;
    EnterCriticalSection( &impl->cs );
    if ((e = find_entry( impl, key )))
    {
        old = e->value;
        e->value = value;
        if (value) IInspectable_AddRef( value );
        was_there = TRUE;
    }
    else
    {
        if (!(e = calloc( 1, sizeof(*e) ))) hr = E_OUTOFMEMORY;
        else if (FAILED(hr = WindowsDuplicateString( key, &e->key ))) free( e );
        else
        {
            if ((e->value = value)) IInspectable_AddRef( value );
            list_add_tail( &impl->entries, &e->entry );
            impl->size++;
        }
        was_there = FALSE;
    }
    if (SUCCEEDED(hr))
    {
        /* the owner can refuse the value (a type that cannot be stored); then the map is as it was */
        if (impl->callback && FAILED(hr = impl->callback( impl->context, &impl->IPropertySet_iface )))
        {
            if (was_there)
            {
                if (value) IInspectable_Release( value );
                e->value = old;
                old = NULL;
            }
            else
            {
                list_remove( &e->entry );
                impl->size--;
                WindowsDeleteString( e->key );
                if (e->value) IInspectable_Release( e->value );
                free( e );
            }
        }
        else if (replaced) *replaced = was_there;
    }
    LeaveCriticalSection( &impl->cs );
    if (old) IInspectable_Release( old );
    return hr;
}

static HRESULT WINAPI map_Remove( IMap_HSTRING_IInspectable *iface, HSTRING key )
{
    struct property_set *impl = impl_from_IMap_HSTRING_IInspectable( iface );
    struct entry *e;
    HRESULT hr = S_OK;

    TRACE( "iface %p, key %s.\n", iface, debugstr_hstring( key ) );

    EnterCriticalSection( &impl->cs );
    if (!(e = find_entry( impl, key ))) hr = E_BOUNDS;
    else
    {
        list_remove( &e->entry );
        impl->size--;
        WindowsDeleteString( e->key );
        if (e->value) IInspectable_Release( e->value );
        free( e );
        notify( impl );
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI map_Clear( IMap_HSTRING_IInspectable *iface )
{
    struct property_set *impl = impl_from_IMap_HSTRING_IInspectable( iface );

    EnterCriticalSection( &impl->cs );
    entries_clear( impl );
    notify( impl );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static const IMap_HSTRING_IInspectableVtbl map_vtbl =
{
    map_QueryInterface,
    map_AddRef,
    map_Release,
    map_GetIids,
    map_GetRuntimeClassName,
    map_GetTrustLevel,
    map_Lookup,
    map_get_Size,
    map_HasKey,
    map_GetView,
    map_Insert,
    map_Remove,
    map_Clear,
};

/* IKeyValuePair<HSTRING, IInspectable *> and the iterator over a snapshot of the entries */

struct pair
{
    IKeyValuePair_HSTRING_IInspectable IKeyValuePair_iface;
    LONG ref;
    HSTRING key;
    IInspectable *value;
};

static inline struct pair *impl_from_IKeyValuePair( IKeyValuePair_HSTRING_IInspectable *iface )
{
    return CONTAINING_RECORD( iface, struct pair, IKeyValuePair_iface );
}

static HRESULT WINAPI pair_QueryInterface( IKeyValuePair_HSTRING_IInspectable *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IKeyValuePair_HSTRING_IInspectable ))
    {
        *out = iface;
        IKeyValuePair_HSTRING_IInspectable_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI pair_AddRef( IKeyValuePair_HSTRING_IInspectable *iface )
{
    return InterlockedIncrement( &impl_from_IKeyValuePair( iface )->ref );
}

static ULONG WINAPI pair_Release( IKeyValuePair_HSTRING_IInspectable *iface )
{
    struct pair *impl = impl_from_IKeyValuePair( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->key );
        if (impl->value) IInspectable_Release( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI pair_GetIids( IKeyValuePair_HSTRING_IInspectable *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI pair_GetRuntimeClassName( IKeyValuePair_HSTRING_IInspectable *iface, HSTRING *name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI pair_GetTrustLevel( IKeyValuePair_HSTRING_IInspectable *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI pair_get_Key( IKeyValuePair_HSTRING_IInspectable *iface, HSTRING *key )
{
    return WindowsDuplicateString( impl_from_IKeyValuePair( iface )->key, key );
}

static HRESULT WINAPI pair_get_Value( IKeyValuePair_HSTRING_IInspectable *iface, IInspectable **value )
{
    struct pair *impl = impl_from_IKeyValuePair( iface );

    if ((*value = impl->value)) IInspectable_AddRef( *value );
    return S_OK;
}

static const IKeyValuePair_HSTRING_IInspectableVtbl pair_vtbl =
{
    pair_QueryInterface,
    pair_AddRef,
    pair_Release,
    pair_GetIids,
    pair_GetRuntimeClassName,
    pair_GetTrustLevel,
    pair_get_Key,
    pair_get_Value,
};

struct iterator
{
    IIterator_IKeyValuePair_HSTRING_IInspectable IIterator_iface;
    LONG ref;
    struct pair **pairs;
    UINT32 count, position;
};

static inline struct iterator *impl_from_IIterator( IIterator_IKeyValuePair_HSTRING_IInspectable *iface )
{
    return CONTAINING_RECORD( iface, struct iterator, IIterator_iface );
}

static HRESULT WINAPI iterator_QueryInterface( IIterator_IKeyValuePair_HSTRING_IInspectable *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_IKeyValuePair_HSTRING_IInspectable ))
    {
        *out = iface;
        IIterator_IKeyValuePair_HSTRING_IInspectable_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterator_AddRef( IIterator_IKeyValuePair_HSTRING_IInspectable *iface )
{
    return InterlockedIncrement( &impl_from_IIterator( iface )->ref );
}

static ULONG WINAPI iterator_Release( IIterator_IKeyValuePair_HSTRING_IInspectable *iface )
{
    struct iterator *impl = impl_from_IIterator( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IKeyValuePair_HSTRING_IInspectable_Release( &impl->pairs[i]->IKeyValuePair_iface );
        free( impl->pairs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI iterator_GetIids( IIterator_IKeyValuePair_HSTRING_IInspectable *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI iterator_GetRuntimeClassName( IIterator_IKeyValuePair_HSTRING_IInspectable *iface, HSTRING *name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI iterator_GetTrustLevel( IIterator_IKeyValuePair_HSTRING_IInspectable *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI iterator_get_Current( IIterator_IKeyValuePair_HSTRING_IInspectable *iface,
                                            IKeyValuePair_HSTRING_IInspectable **value )
{
    struct iterator *impl = impl_from_IIterator( iface );

    if (impl->position >= impl->count) return E_BOUNDS;
    IKeyValuePair_HSTRING_IInspectable_AddRef( *value = &impl->pairs[impl->position]->IKeyValuePair_iface );
    return S_OK;
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_IKeyValuePair_HSTRING_IInspectable *iface, boolean *value )
{
    struct iterator *impl = impl_from_IIterator( iface );

    *value = impl->position < impl->count;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_IKeyValuePair_HSTRING_IInspectable *iface, boolean *value )
{
    struct iterator *impl = impl_from_IIterator( iface );

    if (impl->position < impl->count) impl->position++;
    *value = impl->position < impl->count;
    return S_OK;
}

static HRESULT WINAPI iterator_GetMany( IIterator_IKeyValuePair_HSTRING_IInspectable *iface, UINT32 size,
                                        IKeyValuePair_HSTRING_IInspectable **items, UINT32 *actual )
{
    struct iterator *impl = impl_from_IIterator( iface );
    UINT32 n = 0;

    while (n < size && impl->position < impl->count)
    {
        IKeyValuePair_HSTRING_IInspectable_AddRef( items[n++] = &impl->pairs[impl->position++]->IKeyValuePair_iface );
    }
    *actual = n;
    return S_OK;
}

static const IIterator_IKeyValuePair_HSTRING_IInspectableVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    iterator_GetIids,
    iterator_GetRuntimeClassName,
    iterator_GetTrustLevel,
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

/* IIterable<IKeyValuePair<HSTRING, IInspectable *> *> */

DEFINE_IINSPECTABLE( iterable, IIterable_IKeyValuePair_HSTRING_IInspectable, struct property_set, IPropertySet_iface )

static HRESULT WINAPI iterable_First( IIterable_IKeyValuePair_HSTRING_IInspectable *iface,
                                      IIterator_IKeyValuePair_HSTRING_IInspectable **iterator )
{
    struct property_set *impl = impl_from_IIterable_IKeyValuePair_HSTRING_IInspectable( iface );
    struct iterator *it;
    struct entry *e;
    HRESULT hr = S_OK;

    if (!(it = calloc( 1, sizeof(*it) ))) return E_OUTOFMEMORY;
    it->IIterator_iface.lpVtbl = &iterator_vtbl;
    it->ref = 1;
    EnterCriticalSection( &impl->cs );
    it->pairs = calloc( impl->size ? impl->size : 1, sizeof(*it->pairs) );
    LIST_FOR_EACH_ENTRY( e, &impl->entries, struct entry, entry )
    {
        struct pair *pair;

        if (!it->pairs || !(pair = calloc( 1, sizeof(*pair) ))) { hr = E_OUTOFMEMORY; break; }
        pair->IKeyValuePair_iface.lpVtbl = &pair_vtbl;
        pair->ref = 1;
        WindowsDuplicateString( e->key, &pair->key );
        if ((pair->value = e->value)) IInspectable_AddRef( pair->value );
        it->pairs[it->count++] = pair;
    }
    LeaveCriticalSection( &impl->cs );
    if (FAILED(hr))
    {
        IIterator_IKeyValuePair_HSTRING_IInspectable_Release( &it->IIterator_iface );
        return hr;
    }
    *iterator = &it->IIterator_iface;
    return S_OK;
}

static const IIterable_IKeyValuePair_HSTRING_IInspectableVtbl iterable_vtbl =
{
    iterable_QueryInterface,
    iterable_AddRef,
    iterable_Release,
    iterable_GetIids,
    iterable_GetRuntimeClassName,
    iterable_GetTrustLevel,
    iterable_First,
};

/* ---- creation and the access of the owner ------------------------------------------------------------ */

HRESULT property_set_create( const WCHAR *class_name, property_set_callback callback, void *context, IPropertySet **out )
{
    struct property_set *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPropertySet_iface.lpVtbl = &property_set_vtbl;
    impl->IObservableMap_HSTRING_IInspectable_iface.lpVtbl = &observable_map_vtbl;
    impl->IMap_HSTRING_IInspectable_iface.lpVtbl = &map_vtbl;
    impl->IIterable_IKeyValuePair_HSTRING_IInspectable_iface.lpVtbl = &iterable_vtbl;
    impl->ref = 1;
    impl->class_name = class_name;
    impl->callback = callback;
    impl->context = context;
    list_init( &impl->entries );
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": property_set.cs");
    *out = &impl->IPropertySet_iface;
    return S_OK;
}

/* the callback is not called: the owner fills the set while it loads it */
HRESULT property_set_load( IPropertySet *iface, HSTRING key, IInspectable *value )
{
    struct property_set *impl = impl_from_IPropertySet( iface );
    property_set_callback callback = impl->callback;
    boolean replaced;
    HRESULT hr;

    impl->callback = NULL;
    hr = IMap_HSTRING_IInspectable_Insert( &impl->IMap_HSTRING_IInspectable_iface, key, value, &replaced );
    impl->callback = callback;
    return hr;
}

/* the entries at this moment, for the owner to write them; the keys and values are the set's own, not referenced */
void property_set_enum( IPropertySet *iface, property_set_enum_callback enumerate, void *context )
{
    struct property_set *impl = impl_from_IPropertySet( iface );
    struct entry *e;

    EnterCriticalSection( &impl->cs );
    LIST_FOR_EACH_ENTRY( e, &impl->entries, struct entry, entry )
        enumerate( context, WindowsGetStringRawBuffer( e->key, NULL ), e->value );
    LeaveCriticalSection( &impl->cs );
}
