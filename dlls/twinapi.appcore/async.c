/* Completed asynchronous operations
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

/*
 * An IAsyncOperation<boolean> whose work is done when it is made: its status is Completed, its result is known, and a
 * Completed handler is called as it is set, as for an operation that has finished.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct async_bool
{
    IAsyncOperation_boolean IAsyncOperation_boolean_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    boolean result;
    LONG handler_set;
    LONG closed;
    UINT32 id;
};

static inline struct async_bool *impl_from_IAsyncOperation_boolean( IAsyncOperation_boolean *iface )
{
    return CONTAINING_RECORD( iface, struct async_bool, IAsyncOperation_boolean_iface );
}

static HRESULT WINAPI async_bool_QueryInterface( IAsyncOperation_boolean *iface, REFIID iid, void **out )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperation_boolean ))
        *out = &impl->IAsyncOperation_boolean_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo ))
        *out = &impl->IAsyncInfo_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI async_bool_AddRef( IAsyncOperation_boolean *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncOperation_boolean( iface )->ref );
}

static ULONG WINAPI async_bool_Release( IAsyncOperation_boolean *iface )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI async_bool_GetIids( IAsyncOperation_boolean *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_bool_GetRuntimeClassName( IAsyncOperation_boolean *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.Foundation.IAsyncOperation`1<Boolean>";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI async_bool_GetTrustLevel( IAsyncOperation_boolean *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* the operation has completed: the handler is called now, and only one may be set */
static HRESULT WINAPI async_bool_put_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean *handler )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    if (!handler) return E_INVALIDARG;
    if (InterlockedExchange( &impl->handler_set, 1 )) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    IAsyncOperationCompletedHandler_boolean_Invoke( handler, iface, Completed );
    return S_OK;
}

static HRESULT WINAPI async_bool_get_Completed( IAsyncOperation_boolean *iface, IAsyncOperationCompletedHandler_boolean **handler )
{
    TRACE( "iface %p, handler %p.\n", iface, handler );
    *handler = NULL;
    return S_OK;
}

static HRESULT WINAPI async_bool_GetResults( IAsyncOperation_boolean *iface, boolean *results )
{
    struct async_bool *impl = impl_from_IAsyncOperation_boolean( iface );

    TRACE( "iface %p, results %p.\n", iface, results );
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    *results = impl->result;
    return S_OK;
}

static const IAsyncOperation_booleanVtbl async_bool_vtbl =
{
    async_bool_QueryInterface,
    async_bool_AddRef,
    async_bool_Release,
    /* IInspectable methods */
    async_bool_GetIids,
    async_bool_GetRuntimeClassName,
    async_bool_GetTrustLevel,
    /* IAsyncOperation<boolean> methods */
    async_bool_put_Completed,
    async_bool_get_Completed,
    async_bool_GetResults,
};

DEFINE_IINSPECTABLE( async_info, IAsyncInfo, struct async_bool, IAsyncOperation_boolean_iface )

static HRESULT WINAPI async_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = impl_from_IAsyncInfo( iface )->id;
    return S_OK;
}

static HRESULT WINAPI async_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    if (impl_from_IAsyncInfo( iface )->closed) return E_ILLEGAL_METHOD_CALL;
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI async_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    if (impl_from_IAsyncInfo( iface )->closed) return E_ILLEGAL_METHOD_CALL;
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_info_Cancel( IAsyncInfo *iface )
{
    if (impl_from_IAsyncInfo( iface )->closed) return E_ILLEGAL_METHOD_CALL;
    return S_OK;
}

static HRESULT WINAPI async_info_Close( IAsyncInfo *iface )
{
    impl_from_IAsyncInfo( iface )->closed = 1;
    return S_OK;
}

static const IAsyncInfoVtbl async_info_vtbl =
{
    async_info_QueryInterface,
    async_info_AddRef,
    async_info_Release,
    /* IInspectable methods */
    async_info_GetIids,
    async_info_GetRuntimeClassName,
    async_info_GetTrustLevel,
    /* IAsyncInfo methods */
    async_info_get_Id,
    async_info_get_Status,
    async_info_get_ErrorCode,
    async_info_Cancel,
    async_info_Close,
};

HRESULT async_bool_completed( boolean result, IAsyncOperation_boolean **out )
{
    static LONG next_id;
    struct async_bool *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_boolean_iface.lpVtbl = &async_bool_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_info_vtbl;
    impl->ref = 1;
    impl->result = result;
    impl->id = InterlockedIncrement( &next_id );
    *out = &impl->IAsyncOperation_boolean_iface;
    return S_OK;
}
