/* WinRT IAsyncAction that is done when made
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
 * Like async.c, the work is done when the action is made: the status is Completed (or Error) and a Completed
 * handler is called as it is set. IAsyncAction has no result, so GetResults takes no argument and cannot share
 * the methods of the operations.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct async_action
{
    IAsyncAction IAsyncAction_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    HRESULT hr;
    LONG handler_set;
    LONG closed;
    UINT32 id;
};

static inline struct async_action *impl_from_IAsyncAction( IAsyncAction *iface )
{
    return CONTAINING_RECORD( iface, struct async_action, IAsyncAction_iface );
}

static HRESULT WINAPI action_QueryInterface( IAsyncAction *iface, REFIID iid, void **out )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncAction ))
        *out = &impl->IAsyncAction_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo ))
        *out = &impl->IAsyncInfo_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI action_AddRef( IAsyncAction *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncAction( iface )->ref );
}

static ULONG WINAPI action_Release( IAsyncAction *iface )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI action_GetIids( IAsyncAction *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IAsyncAction;
    list[1] = IID_IAsyncInfo;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI action_GetRuntimeClassName( IAsyncAction *iface, HSTRING *name )
{
    return create_name( L"Windows.Foundation.IAsyncAction", name );
}

static HRESULT WINAPI action_GetTrustLevel( IAsyncAction *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI action_put_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler *handler )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    if (!handler) return E_INVALIDARG;
    if (InterlockedExchange( &impl->handler_set, 1 )) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    IAsyncActionCompletedHandler_Invoke( handler, iface, SUCCEEDED( impl->hr ) ? Completed : Error );
    return S_OK;
}

static HRESULT WINAPI action_get_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler **handler )
{
    TRACE( "iface %p, handler %p.\n", iface, handler );
    *handler = NULL;
    return S_OK;
}

static HRESULT WINAPI action_GetResults( IAsyncAction *iface )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );

    TRACE( "iface %p.\n", iface );
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    return impl->hr;
}

static const IAsyncActionVtbl action_vtbl =
{
    action_QueryInterface,
    action_AddRef,
    action_Release,
    /* IInspectable methods */
    action_GetIids,
    action_GetRuntimeClassName,
    action_GetTrustLevel,
    /* IAsyncAction methods */
    action_put_Completed,
    action_get_Completed,
    action_GetResults,
};

DEFINE_IINSPECTABLE( action_info, IAsyncInfo, struct async_action, IAsyncAction_iface )

static HRESULT WINAPI action_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = impl_from_IAsyncInfo( iface )->id;
    return S_OK;
}

static HRESULT WINAPI action_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_action *impl = impl_from_IAsyncInfo( iface );

    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    *status = SUCCEEDED( impl->hr ) ? Completed : Error;
    return S_OK;
}

static HRESULT WINAPI action_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    struct async_action *impl = impl_from_IAsyncInfo( iface );

    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    *error_code = impl->hr;
    return S_OK;
}

static HRESULT WINAPI action_info_Cancel( IAsyncInfo *iface )
{
    if (impl_from_IAsyncInfo( iface )->closed) return E_ILLEGAL_METHOD_CALL;
    return S_OK;
}

static HRESULT WINAPI action_info_Close( IAsyncInfo *iface )
{
    impl_from_IAsyncInfo( iface )->closed = 1;
    return S_OK;
}

static const IAsyncInfoVtbl action_info_vtbl =
{
    action_info_QueryInterface,
    action_info_AddRef,
    action_info_Release,
    /* IInspectable methods */
    action_info_GetIids,
    action_info_GetRuntimeClassName,
    action_info_GetTrustLevel,
    /* IAsyncInfo methods */
    action_info_get_Id,
    action_info_get_Status,
    action_info_get_ErrorCode,
    action_info_Cancel,
    action_info_Close,
};

HRESULT async_action_completed( HRESULT hr, IAsyncAction **out )
{
    static LONG next_id;
    struct async_action *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncAction_iface.lpVtbl = &action_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &action_info_vtbl;
    impl->ref = 1;
    impl->hr = hr;
    impl->id = InterlockedIncrement( &next_id );
    *out = &impl->IAsyncAction_iface;
    return S_OK;
}
