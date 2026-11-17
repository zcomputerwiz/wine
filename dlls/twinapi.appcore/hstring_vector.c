/* A vector of strings: IVector<HSTRING>, IVectorView<HSTRING>, IIterable<HSTRING>
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

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct hvec
{
    IVectorView_HSTRING IVectorView_HSTRING_iface;
    IVector_HSTRING IVector_HSTRING_iface;
    IIterable_HSTRING IIterable_HSTRING_iface;
    LONG ref;
    BOOL mutable;
    CRITICAL_SECTION cs;
    HSTRING *items;
    UINT32 count;
    UINT32 capacity;
};

static inline struct hvec *impl_from_IVectorView_HSTRING( IVectorView_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hvec, IVectorView_HSTRING_iface );
}

/* ---- the iterator ---- */

struct hiter
{
    IIterator_HSTRING IIterator_HSTRING_iface;
    LONG ref;
    struct hvec *vec;
    UINT32 index;
};

static inline struct hiter *impl_from_IIterator_HSTRING( IIterator_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hiter, IIterator_HSTRING_iface );
}

static HRESULT WINAPI iter_QueryInterface( IIterator_HSTRING *iface, REFIID iid, void **out )
{
    struct hiter *impl = impl_from_IIterator_HSTRING( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_HSTRING ))
    {
        IInspectable_AddRef( (*out = &impl->IIterator_HSTRING_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iter_AddRef( IIterator_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_HSTRING( iface )->ref );
}

static ULONG WINAPI iter_Release( IIterator_HSTRING *iface )
{
    struct hiter *impl = impl_from_IIterator_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_HSTRING_Release( &impl->vec->IVectorView_HSTRING_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI iter_GetIids( IIterator_HSTRING *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IIterator_HSTRING;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI iter_GetRuntimeClassName( IIterator_HSTRING *iface, HSTRING *name )
{
    return create_name( L"Windows.Foundation.Collections.IIterator`1<String>", name );
}

static HRESULT WINAPI iter_GetTrustLevel( IIterator_HSTRING *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI iter_get_Current( IIterator_HSTRING *iface, HSTRING *value )
{
    struct hiter *impl = impl_from_IIterator_HSTRING( iface );

    return IVectorView_HSTRING_GetAt( &impl->vec->IVectorView_HSTRING_iface, impl->index, value );
}

static HRESULT WINAPI iter_get_HasCurrent( IIterator_HSTRING *iface, boolean *value )
{
    struct hiter *impl = impl_from_IIterator_HSTRING( iface );

    EnterCriticalSection( &impl->vec->cs );
    *value = impl->index < impl->vec->count;
    LeaveCriticalSection( &impl->vec->cs );
    return S_OK;
}

static HRESULT WINAPI iter_MoveNext( IIterator_HSTRING *iface, boolean *value )
{
    struct hiter *impl = impl_from_IIterator_HSTRING( iface );

    EnterCriticalSection( &impl->vec->cs );
    if (impl->index < impl->vec->count) impl->index++;
    *value = impl->index < impl->vec->count;
    LeaveCriticalSection( &impl->vec->cs );
    return S_OK;
}

static HRESULT WINAPI iter_GetMany( IIterator_HSTRING *iface, UINT32 size, HSTRING *items, UINT32 *copied )
{
    struct hiter *impl = impl_from_IIterator_HSTRING( iface );
    HRESULT hr = S_OK;
    UINT32 n = 0;

    EnterCriticalSection( &impl->vec->cs );
    while (n < size && impl->index < impl->vec->count)
    {
        if (FAILED( hr = WindowsDuplicateString( impl->vec->items[impl->index], &items[n] ) )) break;
        impl->index++;
        n++;
    }
    LeaveCriticalSection( &impl->vec->cs );
    if (FAILED( hr ))
    {
        while (n) WindowsDeleteString( items[--n] );
        return hr;
    }
    *copied = n;
    return S_OK;
}

static const IIterator_HSTRINGVtbl iter_vtbl =
{
    iter_QueryInterface,
    iter_AddRef,
    iter_Release,
    /* IInspectable methods */
    iter_GetIids,
    iter_GetRuntimeClassName,
    iter_GetTrustLevel,
    /* IIterator<HSTRING> methods */
    iter_get_Current,
    iter_get_HasCurrent,
    iter_MoveNext,
    iter_GetMany,
};

/* ---- the vector: the view is the base interface, the others share its reference count ---- */

static HRESULT WINAPI view_QueryInterface( IVectorView_HSTRING *iface, REFIID iid, void **out )
{
    struct hvec *impl = impl_from_IVectorView_HSTRING( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ))
        *out = impl->mutable ? (void *)&impl->IVector_HSTRING_iface : (void *)&impl->IVectorView_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IVectorView_HSTRING ) && !impl->mutable)
        *out = &impl->IVectorView_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IVector_HSTRING ) && impl->mutable)
        *out = &impl->IVector_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_HSTRING ))
        *out = &impl->IIterable_HSTRING_iface;
    else
        return E_NOINTERFACE;

    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI view_AddRef( IVectorView_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_HSTRING( iface )->ref );
}

static ULONG WINAPI view_Release( IVectorView_HSTRING *iface )
{
    struct hvec *impl = impl_from_IVectorView_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) WindowsDeleteString( impl->items[i] );
        free( impl->items );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI view_GetIids( IVectorView_HSTRING *iface, ULONG *count, IID **iids )
{
    struct hvec *impl = impl_from_IVectorView_HSTRING( iface );
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = impl->mutable ? IID_IVector_HSTRING : IID_IVectorView_HSTRING;
    list[1] = IID_IIterable_HSTRING;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI view_GetRuntimeClassName( IVectorView_HSTRING *iface, HSTRING *name )
{
    if (impl_from_IVectorView_HSTRING( iface )->mutable)
        return create_name( L"Windows.Foundation.Collections.IVector`1<String>", name );
    return create_name( L"Windows.Foundation.Collections.IVectorView`1<String>", name );
}

static HRESULT WINAPI view_GetTrustLevel( IVectorView_HSTRING *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT hvec_get_at( struct hvec *impl, UINT32 index, HSTRING *value )
{
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (index >= impl->count) hr = E_BOUNDS;
    else hr = WindowsDuplicateString( impl->items[index], value );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT hvec_index_of( struct hvec *impl, HSTRING element, UINT32 *index, boolean *found )
{
    UINT32 i;
    INT32 order;

    *found = FALSE;
    *index = 0;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++)
    {
        WindowsCompareStringOrdinal( impl->items[i], element, &order );
        if (order) continue;
        *found = TRUE;
        *index = i;
        break;
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT hvec_get_many( struct hvec *impl, UINT32 start, UINT32 size, HSTRING *items, UINT32 *copied )
{
    HRESULT hr = S_OK;
    UINT32 n = 0;

    EnterCriticalSection( &impl->cs );
    if (start > impl->count) hr = E_BOUNDS;
    while (SUCCEEDED( hr ) && n < size && start + n < impl->count)
    {
        hr = WindowsDuplicateString( impl->items[start + n], &items[n] );
        if (SUCCEEDED( hr )) n++;
    }
    LeaveCriticalSection( &impl->cs );
    if (FAILED( hr ))
    {
        while (n) WindowsDeleteString( items[--n] );
        return hr;
    }
    *copied = n;
    return S_OK;
}

static HRESULT WINAPI view_GetAt( IVectorView_HSTRING *iface, UINT32 index, HSTRING *value )
{
    return hvec_get_at( impl_from_IVectorView_HSTRING( iface ), index, value );
}

static HRESULT WINAPI view_get_Size( IVectorView_HSTRING *iface, UINT32 *value )
{
    *value = impl_from_IVectorView_HSTRING( iface )->count;
    return S_OK;
}

static HRESULT WINAPI view_IndexOf( IVectorView_HSTRING *iface, HSTRING element, UINT32 *index, boolean *found )
{
    return hvec_index_of( impl_from_IVectorView_HSTRING( iface ), element, index, found );
}

static HRESULT WINAPI view_GetMany( IVectorView_HSTRING *iface, UINT32 start, UINT32 size, HSTRING *items, UINT32 *copied )
{
    return hvec_get_many( impl_from_IVectorView_HSTRING( iface ), start, size, items, copied );
}

static const IVectorView_HSTRINGVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    /* IInspectable methods */
    view_GetIids,
    view_GetRuntimeClassName,
    view_GetTrustLevel,
    /* IVectorView<HSTRING> methods */
    view_GetAt,
    view_get_Size,
    view_IndexOf,
    view_GetMany,
};

DEFINE_IINSPECTABLE( vec, IVector_HSTRING, struct hvec, IVectorView_HSTRING_iface )

static HRESULT hvec_reserve( struct hvec *impl, UINT32 count )
{
    HSTRING *items;
    UINT32 capacity;

    if (count <= impl->capacity) return S_OK;
    capacity = max( 4, max( count, impl->capacity * 2 ) );
    if (!(items = realloc( impl->items, capacity * sizeof(*items) ))) return E_OUTOFMEMORY;
    impl->items = items;
    impl->capacity = capacity;
    return S_OK;
}

static HRESULT hvec_insert( struct hvec *impl, UINT32 index, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (index > impl->count) hr = E_BOUNDS;
    else if (SUCCEEDED( hr = hvec_reserve( impl, impl->count + 1 ) ) &&
             SUCCEEDED( hr = WindowsDuplicateString( value, &copy ) ))
    {
        memmove( impl->items + index + 1, impl->items + index, (impl->count - index) * sizeof(*impl->items) );
        impl->items[index] = copy;
        impl->count++;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT hvec_remove( struct hvec *impl, UINT32 index )
{
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (index >= impl->count) hr = E_BOUNDS;
    else
    {
        WindowsDeleteString( impl->items[index] );
        memmove( impl->items + index, impl->items + index + 1, (impl->count - index - 1) * sizeof(*impl->items) );
        impl->count--;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vec_GetAt( IVector_HSTRING *iface, UINT32 index, HSTRING *value )
{
    return hvec_get_at( impl_from_IVector_HSTRING( iface ), index, value );
}

static HRESULT WINAPI vec_get_Size( IVector_HSTRING *iface, UINT32 *value )
{
    *value = impl_from_IVector_HSTRING( iface )->count;
    return S_OK;
}

static HRESULT WINAPI vec_GetView( IVector_HSTRING *iface, IVectorView_HSTRING **value )
{
    struct hvec *impl = impl_from_IVector_HSTRING( iface );

    return hstring_vector_create( FALSE, impl->items, impl->count, (void **)value );
}

static HRESULT WINAPI vec_IndexOf( IVector_HSTRING *iface, HSTRING element, UINT32 *index, boolean *found )
{
    return hvec_index_of( impl_from_IVector_HSTRING( iface ), element, index, found );
}

static HRESULT WINAPI vec_SetAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    struct hvec *impl = impl_from_IVector_HSTRING( iface );
    HSTRING copy;
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (index >= impl->count) hr = E_BOUNDS;
    else if (SUCCEEDED( hr = WindowsDuplicateString( value, &copy ) ))
    {
        WindowsDeleteString( impl->items[index] );
        impl->items[index] = copy;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vec_InsertAt( IVector_HSTRING *iface, UINT32 index, HSTRING value )
{
    return hvec_insert( impl_from_IVector_HSTRING( iface ), index, value );
}

static HRESULT WINAPI vec_RemoveAt( IVector_HSTRING *iface, UINT32 index )
{
    return hvec_remove( impl_from_IVector_HSTRING( iface ), index );
}

static HRESULT WINAPI vec_Append( IVector_HSTRING *iface, HSTRING value )
{
    struct hvec *impl = impl_from_IVector_HSTRING( iface );

    return hvec_insert( impl, impl->count, value );
}

static HRESULT WINAPI vec_RemoveAtEnd( IVector_HSTRING *iface )
{
    struct hvec *impl = impl_from_IVector_HSTRING( iface );

    return hvec_remove( impl, impl->count ? impl->count - 1 : 0 );
}

static HRESULT WINAPI vec_Clear( IVector_HSTRING *iface )
{
    struct hvec *impl = impl_from_IVector_HSTRING( iface );
    UINT32 i;

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++) WindowsDeleteString( impl->items[i] );
    impl->count = 0;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI vec_GetMany( IVector_HSTRING *iface, UINT32 start, UINT32 size, HSTRING *items, UINT32 *copied )
{
    return hvec_get_many( impl_from_IVector_HSTRING( iface ), start, size, items, copied );
}

static HRESULT WINAPI vec_ReplaceAll( IVector_HSTRING *iface, UINT32 count, HSTRING *items )
{
    struct hvec *impl = impl_from_IVector_HSTRING( iface );
    HRESULT hr;
    UINT32 i;

    vec_Clear( iface );
    for (i = 0; i < count; i++)
        if (FAILED( hr = hvec_insert( impl, i, items[i] ) )) return hr;
    return S_OK;
}

static const IVector_HSTRINGVtbl vec_vtbl =
{
    vec_QueryInterface,
    vec_AddRef,
    vec_Release,
    /* IInspectable methods */
    vec_GetIids,
    vec_GetRuntimeClassName,
    vec_GetTrustLevel,
    /* IVector<HSTRING> methods */
    vec_GetAt,
    vec_get_Size,
    vec_GetView,
    vec_IndexOf,
    vec_SetAt,
    vec_InsertAt,
    vec_RemoveAt,
    vec_Append,
    vec_RemoveAtEnd,
    vec_Clear,
    vec_GetMany,
    vec_ReplaceAll,
};

DEFINE_IINSPECTABLE( iterable, IIterable_HSTRING, struct hvec, IVectorView_HSTRING_iface )

static HRESULT WINAPI iterable_First( IIterable_HSTRING *iface, IIterator_HSTRING **value )
{
    struct hvec *impl = impl_from_IIterable_HSTRING( iface );
    struct hiter *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_HSTRING_iface.lpVtbl = &iter_vtbl;
    iter->ref = 1;
    iter->vec = impl;
    IVectorView_HSTRING_AddRef( &impl->IVectorView_HSTRING_iface );
    *value = &iter->IIterator_HSTRING_iface;
    return S_OK;
}

static const IIterable_HSTRINGVtbl iterable_vtbl =
{
    iterable_QueryInterface,
    iterable_AddRef,
    iterable_Release,
    /* IInspectable methods */
    iterable_GetIids,
    iterable_GetRuntimeClassName,
    iterable_GetTrustLevel,
    /* IIterable<HSTRING> methods */
    iterable_First,
};

HRESULT hstring_vector_create( BOOL mutable, const HSTRING *items, UINT32 count, void **out )
{
    struct hvec *impl;
    UINT32 i;
    HRESULT hr;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_HSTRING_iface.lpVtbl = &view_vtbl;
    impl->IVector_HSTRING_iface.lpVtbl = &vec_vtbl;
    impl->IIterable_HSTRING_iface.lpVtbl = &iterable_vtbl;
    impl->ref = 1;
    impl->mutable = mutable;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": hvec.cs");

    for (i = 0; i < count; i++)
    {
        if (FAILED( hr = hvec_insert( impl, i, items[i] ) ))
        {
            IVectorView_HSTRING_Release( &impl->IVectorView_HSTRING_iface );
            return hr;
        }
    }

    *out = mutable ? (void *)&impl->IVector_HSTRING_iface : (void *)&impl->IVectorView_HSTRING_iface;
    return S_OK;
}
