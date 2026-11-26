/* IVector<JumpListItem>, its view and iterator (made from hstring_vector.c; elements are counted references)
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

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct jvec
{
    IVectorView_JumpListItem IVectorView_JumpListItem_iface;
    IVector_JumpListItem IVector_JumpListItem_iface;
    IIterable_JumpListItem IIterable_JumpListItem_iface;
    LONG ref;
    BOOL mutable;
    CRITICAL_SECTION cs;
    IJumpListItem **items;
    UINT32 count;
    UINT32 capacity;
};

static inline struct jvec *impl_from_IVectorView_JumpListItem( IVectorView_JumpListItem *iface )
{
    return CONTAINING_RECORD( iface, struct jvec, IVectorView_JumpListItem_iface );
}

/* ---- the iterator ---- */

struct jiter
{
    IIterator_JumpListItem IIterator_JumpListItem_iface;
    LONG ref;
    struct jvec *vec;
    UINT32 index;
};

static inline struct jiter *impl_from_IIterator_JumpListItem( IIterator_JumpListItem *iface )
{
    return CONTAINING_RECORD( iface, struct jiter, IIterator_JumpListItem_iface );
}

static HRESULT WINAPI iter_QueryInterface( IIterator_JumpListItem *iface, REFIID iid, void **out )
{
    struct jiter *impl = impl_from_IIterator_JumpListItem( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_JumpListItem ))
    {
        IInspectable_AddRef( (*out = &impl->IIterator_JumpListItem_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iter_AddRef( IIterator_JumpListItem *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_JumpListItem( iface )->ref );
}

static ULONG WINAPI iter_Release( IIterator_JumpListItem *iface )
{
    struct jiter *impl = impl_from_IIterator_JumpListItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IVectorView_JumpListItem_Release( &impl->vec->IVectorView_JumpListItem_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI iter_GetIids( IIterator_JumpListItem *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IIterator_JumpListItem;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI iter_GetRuntimeClassName( IIterator_JumpListItem *iface, HSTRING *name )
{
    return create_name( L"Windows.Foundation.Collections.IIterator`1<Windows.UI.StartScreen.JumpListItem>", name );
}

static HRESULT WINAPI iter_GetTrustLevel( IIterator_JumpListItem *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI iter_get_Current( IIterator_JumpListItem *iface, IJumpListItem **value )
{
    struct jiter *impl = impl_from_IIterator_JumpListItem( iface );

    return IVectorView_JumpListItem_GetAt( &impl->vec->IVectorView_JumpListItem_iface, impl->index, value );
}

static HRESULT WINAPI iter_get_HasCurrent( IIterator_JumpListItem *iface, boolean *value )
{
    struct jiter *impl = impl_from_IIterator_JumpListItem( iface );

    EnterCriticalSection( &impl->vec->cs );
    *value = impl->index < impl->vec->count;
    LeaveCriticalSection( &impl->vec->cs );
    return S_OK;
}

static HRESULT WINAPI iter_MoveNext( IIterator_JumpListItem *iface, boolean *value )
{
    struct jiter *impl = impl_from_IIterator_JumpListItem( iface );

    EnterCriticalSection( &impl->vec->cs );
    if (impl->index < impl->vec->count) impl->index++;
    *value = impl->index < impl->vec->count;
    LeaveCriticalSection( &impl->vec->cs );
    return S_OK;
}

static HRESULT WINAPI iter_GetMany( IIterator_JumpListItem *iface, UINT32 size, IJumpListItem **items, UINT32 *copied )
{
    struct jiter *impl = impl_from_IIterator_JumpListItem( iface );
    HRESULT hr = S_OK;
    UINT32 n = 0;

    EnterCriticalSection( &impl->vec->cs );
    while (n < size && impl->index < impl->vec->count)
    {
        items[n] = impl->vec->items[impl->index];
        IJumpListItem_AddRef( items[n] );
        impl->index++;
        n++;
    }
    LeaveCriticalSection( &impl->vec->cs );
    if (FAILED( hr ))
    {
        while (n) IJumpListItem_Release( items[--n] );
        return hr;
    }
    *copied = n;
    return S_OK;
}

static const IIterator_JumpListItemVtbl iter_vtbl =
{
    iter_QueryInterface,
    iter_AddRef,
    iter_Release,
    /* IInspectable methods */
    iter_GetIids,
    iter_GetRuntimeClassName,
    iter_GetTrustLevel,
    /* IIterator<JumpListItem> methods */
    iter_get_Current,
    iter_get_HasCurrent,
    iter_MoveNext,
    iter_GetMany,
};

/* ---- the vector: the view is the base interface, the others share its reference count ---- */

static HRESULT WINAPI view_QueryInterface( IVectorView_JumpListItem *iface, REFIID iid, void **out )
{
    struct jvec *impl = impl_from_IVectorView_JumpListItem( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ))
        *out = impl->mutable ? (void *)&impl->IVector_JumpListItem_iface : (void *)&impl->IVectorView_JumpListItem_iface;
    else if (IsEqualGUID( iid, &IID_IVectorView_JumpListItem ) && !impl->mutable)
        *out = &impl->IVectorView_JumpListItem_iface;
    else if (IsEqualGUID( iid, &IID_IVector_JumpListItem ) && impl->mutable)
        *out = &impl->IVector_JumpListItem_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_JumpListItem ))
        *out = &impl->IIterable_JumpListItem_iface;
    else
        return E_NOINTERFACE;

    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI view_AddRef( IVectorView_JumpListItem *iface )
{
    return InterlockedIncrement( &impl_from_IVectorView_JumpListItem( iface )->ref );
}

static ULONG WINAPI view_Release( IVectorView_JumpListItem *iface )
{
    struct jvec *impl = impl_from_IVectorView_JumpListItem( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IJumpListItem_Release( impl->items[i] );
        free( impl->items );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI view_GetIids( IVectorView_JumpListItem *iface, ULONG *count, IID **iids )
{
    struct jvec *impl = impl_from_IVectorView_JumpListItem( iface );
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = impl->mutable ? IID_IVector_JumpListItem : IID_IVectorView_JumpListItem;
    list[1] = IID_IIterable_JumpListItem;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI view_GetRuntimeClassName( IVectorView_JumpListItem *iface, HSTRING *name )
{
    if (impl_from_IVectorView_JumpListItem( iface )->mutable)
        return create_name( L"Windows.Foundation.Collections.IVector`1<Windows.UI.StartScreen.JumpListItem>", name );
    return create_name( L"Windows.Foundation.Collections.IVectorView`1<Windows.UI.StartScreen.JumpListItem>", name );
}

static HRESULT WINAPI view_GetTrustLevel( IVectorView_JumpListItem *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT jvec_get_at( struct jvec *impl, UINT32 index, IJumpListItem **value )
{
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (index >= impl->count) hr = E_BOUNDS;
    else
    {
        *value = impl->items[index];
        IJumpListItem_AddRef( *value );
        hr = S_OK;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT jvec_index_of( struct jvec *impl, IJumpListItem *element, UINT32 *index, boolean *found )
{
    UINT32 i;

    *found = FALSE;
    *index = 0;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++)
    {
        if (impl->items[i] != element) continue;
        *found = TRUE;
        *index = i;
        break;
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT jvec_get_many( struct jvec *impl, UINT32 start, UINT32 size, IJumpListItem **items, UINT32 *copied )
{
    HRESULT hr = S_OK;
    UINT32 n = 0;

    EnterCriticalSection( &impl->cs );
    if (start > impl->count) hr = E_BOUNDS;
    while (SUCCEEDED( hr ) && n < size && start + n < impl->count)
    {
        items[n] = impl->items[start + n];
        IJumpListItem_AddRef( items[n] );
        n++;
    }
    LeaveCriticalSection( &impl->cs );
    if (FAILED( hr ))
    {
        while (n) IJumpListItem_Release( items[--n] );
        return hr;
    }
    *copied = n;
    return S_OK;
}

static HRESULT WINAPI view_GetAt( IVectorView_JumpListItem *iface, UINT32 index, IJumpListItem **value )
{
    return jvec_get_at( impl_from_IVectorView_JumpListItem( iface ), index, value );
}

static HRESULT WINAPI view_get_Size( IVectorView_JumpListItem *iface, UINT32 *value )
{
    *value = impl_from_IVectorView_JumpListItem( iface )->count;
    return S_OK;
}

static HRESULT WINAPI view_IndexOf( IVectorView_JumpListItem *iface, IJumpListItem *element, UINT32 *index, boolean *found )
{
    return jvec_index_of( impl_from_IVectorView_JumpListItem( iface ), element, index, found );
}

static HRESULT WINAPI view_GetMany( IVectorView_JumpListItem *iface, UINT32 start, UINT32 size, IJumpListItem **items, UINT32 *copied )
{
    return jvec_get_many( impl_from_IVectorView_JumpListItem( iface ), start, size, items, copied );
}

static const IVectorView_JumpListItemVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    /* IInspectable methods */
    view_GetIids,
    view_GetRuntimeClassName,
    view_GetTrustLevel,
    /* IVectorView<JumpListItem> methods */
    view_GetAt,
    view_get_Size,
    view_IndexOf,
    view_GetMany,
};

DEFINE_IINSPECTABLE( vec, IVector_JumpListItem, struct jvec, IVectorView_JumpListItem_iface )

static HRESULT jvec_reserve( struct jvec *impl, UINT32 count )
{
    IJumpListItem **items;
    UINT32 capacity;

    if (count <= impl->capacity) return S_OK;
    capacity = max( 4, max( count, impl->capacity * 2 ) );
    if (!(items = realloc( impl->items, capacity * sizeof(*items) ))) return E_OUTOFMEMORY;
    impl->items = items;
    impl->capacity = capacity;
    return S_OK;
}

static HRESULT jvec_insert( struct jvec *impl, UINT32 index, IJumpListItem *value )
{
    IJumpListItem *copy;
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (index > impl->count) hr = E_BOUNDS;
    else if (!value) hr = E_POINTER;
    else if (SUCCEEDED( hr = jvec_reserve( impl, impl->count + 1 ) ))
    {
        copy = value;
        IJumpListItem_AddRef( copy );
        memmove( impl->items + index + 1, impl->items + index, (impl->count - index) * sizeof(*impl->items) );
        impl->items[index] = copy;
        impl->count++;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT jvec_remove( struct jvec *impl, UINT32 index )
{
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (index >= impl->count) hr = E_BOUNDS;
    else
    {
        IJumpListItem_Release( impl->items[index] );
        memmove( impl->items + index, impl->items + index + 1, (impl->count - index - 1) * sizeof(*impl->items) );
        impl->count--;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vec_GetAt( IVector_JumpListItem *iface, UINT32 index, IJumpListItem **value )
{
    return jvec_get_at( impl_from_IVector_JumpListItem( iface ), index, value );
}

static HRESULT WINAPI vec_get_Size( IVector_JumpListItem *iface, UINT32 *value )
{
    *value = impl_from_IVector_JumpListItem( iface )->count;
    return S_OK;
}

static HRESULT WINAPI vec_GetView( IVector_JumpListItem *iface, IVectorView_JumpListItem **value )
{
    struct jvec *impl = impl_from_IVector_JumpListItem( iface );

    return jump_list_item_vector_create( FALSE, impl->items, impl->count, (void **)value );
}

static HRESULT WINAPI vec_IndexOf( IVector_JumpListItem *iface, IJumpListItem *element, UINT32 *index, boolean *found )
{
    return jvec_index_of( impl_from_IVector_JumpListItem( iface ), element, index, found );
}

static HRESULT WINAPI vec_SetAt( IVector_JumpListItem *iface, UINT32 index, IJumpListItem *value )
{
    struct jvec *impl = impl_from_IVector_JumpListItem( iface );
    IJumpListItem *copy;
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (index >= impl->count) hr = E_BOUNDS;
    else if (!value) hr = E_POINTER;
    else
    {
        copy = value;
        IJumpListItem_AddRef( copy );
        IJumpListItem_Release( impl->items[index] );
        impl->items[index] = copy;
        hr = S_OK;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI vec_InsertAt( IVector_JumpListItem *iface, UINT32 index, IJumpListItem *value )
{
    return jvec_insert( impl_from_IVector_JumpListItem( iface ), index, value );
}

static HRESULT WINAPI vec_RemoveAt( IVector_JumpListItem *iface, UINT32 index )
{
    return jvec_remove( impl_from_IVector_JumpListItem( iface ), index );
}

static HRESULT WINAPI vec_Append( IVector_JumpListItem *iface, IJumpListItem *value )
{
    struct jvec *impl = impl_from_IVector_JumpListItem( iface );

    return jvec_insert( impl, impl->count, value );
}

static HRESULT WINAPI vec_RemoveAtEnd( IVector_JumpListItem *iface )
{
    struct jvec *impl = impl_from_IVector_JumpListItem( iface );

    return jvec_remove( impl, impl->count ? impl->count - 1 : 0 );
}

static HRESULT WINAPI vec_Clear( IVector_JumpListItem *iface )
{
    struct jvec *impl = impl_from_IVector_JumpListItem( iface );
    UINT32 i;

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < impl->count; i++) IJumpListItem_Release( impl->items[i] );
    impl->count = 0;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI vec_GetMany( IVector_JumpListItem *iface, UINT32 start, UINT32 size, IJumpListItem **items, UINT32 *copied )
{
    return jvec_get_many( impl_from_IVector_JumpListItem( iface ), start, size, items, copied );
}

static HRESULT WINAPI vec_ReplaceAll( IVector_JumpListItem *iface, UINT32 count, IJumpListItem **items )
{
    struct jvec *impl = impl_from_IVector_JumpListItem( iface );
    HRESULT hr;
    UINT32 i;

    vec_Clear( iface );
    for (i = 0; i < count; i++)
        if (FAILED( hr = jvec_insert( impl, i, items[i] ) )) return hr;
    return S_OK;
}

static const IVector_JumpListItemVtbl vec_vtbl =
{
    vec_QueryInterface,
    vec_AddRef,
    vec_Release,
    /* IInspectable methods */
    vec_GetIids,
    vec_GetRuntimeClassName,
    vec_GetTrustLevel,
    /* IVector<JumpListItem> methods */
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

DEFINE_IINSPECTABLE( iterable, IIterable_JumpListItem, struct jvec, IVectorView_JumpListItem_iface )

static HRESULT WINAPI iterable_First( IIterable_JumpListItem *iface, IIterator_JumpListItem **value )
{
    struct jvec *impl = impl_from_IIterable_JumpListItem( iface );
    struct jiter *iter;

    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_JumpListItem_iface.lpVtbl = &iter_vtbl;
    iter->ref = 1;
    iter->vec = impl;
    IVectorView_JumpListItem_AddRef( &impl->IVectorView_JumpListItem_iface );
    *value = &iter->IIterator_JumpListItem_iface;
    return S_OK;
}

static const IIterable_JumpListItemVtbl iterable_vtbl =
{
    iterable_QueryInterface,
    iterable_AddRef,
    iterable_Release,
    /* IInspectable methods */
    iterable_GetIids,
    iterable_GetRuntimeClassName,
    iterable_GetTrustLevel,
    /* IIterable<JumpListItem> methods */
    iterable_First,
};

HRESULT jump_list_item_vector_create( BOOL mutable, IJumpListItem *const *items, UINT32 count, void **out )
{
    struct jvec *impl;
    UINT32 i;
    HRESULT hr;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_JumpListItem_iface.lpVtbl = &view_vtbl;
    impl->IVector_JumpListItem_iface.lpVtbl = &vec_vtbl;
    impl->IIterable_JumpListItem_iface.lpVtbl = &iterable_vtbl;
    impl->ref = 1;
    impl->mutable = mutable;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": jvec.cs");

    for (i = 0; i < count; i++)
    {
        if (FAILED( hr = jvec_insert( impl, i, items[i] ) ))
        {
            IVectorView_JumpListItem_Release( &impl->IVectorView_JumpListItem_iface );
            return hr;
        }
    }

    *out = mutable ? (void *)&impl->IVector_JumpListItem_iface : (void *)&impl->IVectorView_JumpListItem_iface;
    return S_OK;
}
