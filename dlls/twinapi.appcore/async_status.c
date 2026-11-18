/* WinRT IAsyncOperation<AppCapabilityAccessStatus> that is done when made
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

/* Like async.c: the work is done when the operation is made; the Completed handler is called as it is set. */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct async_status
{
    IAsyncOperation_AppCapabilityAccessStatus IAsyncOperation_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    AppCapabilityAccessStatus value;
    LONG handler_set;
    LONG closed;
    UINT32 id;
};

static inline struct async_status *impl_from_IAsyncOperation( IAsyncOperation_AppCapabilityAccessStatus *iface )
{
    return CONTAINING_RECORD( iface, struct async_status, IAsyncOperation_iface );
}

static HRESULT WINAPI status_QueryInterface( IAsyncOperation_AppCapabilityAccessStatus *iface, REFIID iid, void **out )
{
    struct async_status *impl = impl_from_IAsyncOperation( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_AppCapabilityAccessStatus ))
        *out = &impl->IAsyncOperation_iface;
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

static ULONG WINAPI status_AddRef( IAsyncOperation_AppCapabilityAccessStatus *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncOperation( iface )->ref );
}

static ULONG WINAPI status_Release( IAsyncOperation_AppCapabilityAccessStatus *iface )
{
    struct async_status *impl = impl_from_IAsyncOperation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI status_GetIids( IAsyncOperation_AppCapabilityAccessStatus *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IAsyncOperation_AppCapabilityAccessStatus;
    list[1] = IID_IAsyncInfo;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI status_GetRuntimeClassName( IAsyncOperation_AppCapabilityAccessStatus *iface, HSTRING *name )
{
    return create_name( L"Windows.Foundation.IAsyncOperation`1<Windows.Security.Authorization.AppCapabilityAccess.AppCapabilityAccessStatus>", name );
}

static HRESULT WINAPI status_GetTrustLevel( IAsyncOperation_AppCapabilityAccessStatus *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI status_put_Completed( IAsyncOperation_AppCapabilityAccessStatus *iface,
                                            IAsyncOperationCompletedHandler_AppCapabilityAccessStatus *handler )
{
    struct async_status *impl = impl_from_IAsyncOperation( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    if (!handler) return E_INVALIDARG;
    if (InterlockedExchange( &impl->handler_set, 1 )) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    IAsyncOperationCompletedHandler_AppCapabilityAccessStatus_Invoke( handler, iface, Completed );
    return S_OK;
}

static HRESULT WINAPI status_get_Completed( IAsyncOperation_AppCapabilityAccessStatus *iface,
                                            IAsyncOperationCompletedHandler_AppCapabilityAccessStatus **handler )
{
    *handler = NULL;
    return S_OK;
}

static HRESULT WINAPI status_GetResults( IAsyncOperation_AppCapabilityAccessStatus *iface, AppCapabilityAccessStatus *results )
{
    struct async_status *impl = impl_from_IAsyncOperation( iface );

    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    *results = impl->value;
    return S_OK;
}

static const IAsyncOperation_AppCapabilityAccessStatusVtbl status_vtbl =
{
    status_QueryInterface,
    status_AddRef,
    status_Release,
    /* IInspectable methods */
    status_GetIids,
    status_GetRuntimeClassName,
    status_GetTrustLevel,
    /* IAsyncOperation methods */
    status_put_Completed,
    status_get_Completed,
    status_GetResults,
};

DEFINE_IINSPECTABLE( status_info, IAsyncInfo, struct async_status, IAsyncOperation_iface )

static HRESULT WINAPI status_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = impl_from_IAsyncInfo( iface )->id;
    return S_OK;
}

static HRESULT WINAPI status_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    if (impl_from_IAsyncInfo( iface )->closed) return E_ILLEGAL_METHOD_CALL;
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI status_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    if (impl_from_IAsyncInfo( iface )->closed) return E_ILLEGAL_METHOD_CALL;
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI status_info_Cancel( IAsyncInfo *iface )
{
    if (impl_from_IAsyncInfo( iface )->closed) return E_ILLEGAL_METHOD_CALL;
    return S_OK;
}

static HRESULT WINAPI status_info_Close( IAsyncInfo *iface )
{
    impl_from_IAsyncInfo( iface )->closed = 1;
    return S_OK;
}

static const IAsyncInfoVtbl status_info_vtbl =
{
    status_info_QueryInterface,
    status_info_AddRef,
    status_info_Release,
    /* IInspectable methods */
    status_info_GetIids,
    status_info_GetRuntimeClassName,
    status_info_GetTrustLevel,
    /* IAsyncInfo methods */
    status_info_get_Id,
    status_info_get_Status,
    status_info_get_ErrorCode,
    status_info_Cancel,
    status_info_Close,
};

HRESULT async_access_status_completed( AppCapabilityAccessStatus value, IAsyncOperation_AppCapabilityAccessStatus **out )
{
    static LONG next_id;
    struct async_status *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_iface.lpVtbl = &status_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &status_info_vtbl;
    impl->ref = 1;
    impl->value = value;
    impl->id = InterlockedIncrement( &next_id );
    *out = &impl->IAsyncOperation_iface;
    return S_OK;
}
