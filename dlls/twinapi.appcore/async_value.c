/* Completed asynchronous operations with a result (string, object, Uri)
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
 * Like async.c, the work is done when the operation is made: the status is Completed (or Error), the result is
 * known, and a Completed handler is called as it is set.
 *
 * IAsyncOperation<HSTRING>, IAsyncOperation<IInspectable *> and IAsyncOperation<Uri *> differ only in the IID and in
 * the type of a pointer in two methods, and every pointer has the same calling convention, so one set of methods
 * (written for the IInspectable one) serves the three.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

enum async_kind { ASYNC_HSTRING, ASYNC_OBJECT, ASYNC_URI };

struct async_value
{
    IAsyncOperation_IInspectable IAsyncOperation_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    enum async_kind kind;
    HRESULT hr;
    HSTRING string;
    IUnknown *object;
    LONG handler_set;
    LONG closed;
    UINT32 id;
};

static inline struct async_value *impl_from_IAsyncOperation( IAsyncOperation_IInspectable *iface )
{
    return CONTAINING_RECORD( iface, struct async_value, IAsyncOperation_iface );
}

static const GUID *async_iid( enum async_kind kind )
{
    switch (kind)
    {
    case ASYNC_HSTRING: return &IID_IAsyncOperation_HSTRING;
    case ASYNC_URI: return &IID_IAsyncOperation_Uri;
    default: return &IID_IAsyncOperation_IInspectable;
    }
}

static HRESULT WINAPI async_QueryInterface( IAsyncOperation_IInspectable *iface, REFIID iid, void **out )
{
    struct async_value *impl = impl_from_IAsyncOperation( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, async_iid( impl->kind ) ))
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

static ULONG WINAPI async_AddRef( IAsyncOperation_IInspectable *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncOperation( iface )->ref );
}

static ULONG WINAPI async_Release( IAsyncOperation_IInspectable *iface )
{
    struct async_value *impl = impl_from_IAsyncOperation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->string );
        if (impl->object) IUnknown_Release( impl->object );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_GetIids( IAsyncOperation_IInspectable *iface, ULONG *count, IID **iids )
{
    struct async_value *impl = impl_from_IAsyncOperation( iface );
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = *async_iid( impl->kind );
    list[1] = IID_IAsyncInfo;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI async_GetRuntimeClassName( IAsyncOperation_IInspectable *iface, HSTRING *name )
{
    struct async_value *impl = impl_from_IAsyncOperation( iface );

    switch (impl->kind)
    {
    case ASYNC_HSTRING:
        return create_name( L"Windows.Foundation.IAsyncOperation`1<String>", name );
    case ASYNC_URI:
        return create_name( L"Windows.Foundation.IAsyncOperation`1<Windows.Foundation.Uri>", name );
    default:
        return create_name( L"Windows.Foundation.IAsyncOperation`1<Object>", name );
    }
}

static HRESULT WINAPI async_GetTrustLevel( IAsyncOperation_IInspectable *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* the operation is done: the handler is called now, and only one may be set */
static HRESULT WINAPI async_put_Completed( IAsyncOperation_IInspectable *iface,
                                           IAsyncOperationCompletedHandler_IInspectable *handler )
{
    struct async_value *impl = impl_from_IAsyncOperation( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    if (!handler) return E_INVALIDARG;
    if (InterlockedExchange( &impl->handler_set, 1 )) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    IAsyncOperationCompletedHandler_IInspectable_Invoke( handler, iface, SUCCEEDED( impl->hr ) ? Completed : Error );
    return S_OK;
}

static HRESULT WINAPI async_get_Completed( IAsyncOperation_IInspectable *iface,
                                           IAsyncOperationCompletedHandler_IInspectable **handler )
{
    TRACE( "iface %p, handler %p.\n", iface, handler );
    *handler = NULL;
    return S_OK;
}

static HRESULT WINAPI async_GetResults( IAsyncOperation_IInspectable *iface, IInspectable **results )
{
    struct async_value *impl = impl_from_IAsyncOperation( iface );

    TRACE( "iface %p, results %p.\n", iface, results );
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    if (FAILED( impl->hr )) return impl->hr;

    if (impl->kind == ASYNC_HSTRING) return WindowsDuplicateString( impl->string, (HSTRING *)results );
    if ((*results = (IInspectable *)impl->object)) IUnknown_AddRef( impl->object );
    return S_OK;
}

static const IAsyncOperation_IInspectableVtbl async_vtbl =
{
    async_QueryInterface,
    async_AddRef,
    async_Release,
    /* IInspectable methods */
    async_GetIids,
    async_GetRuntimeClassName,
    async_GetTrustLevel,
    /* IAsyncOperation methods */
    async_put_Completed,
    async_get_Completed,
    async_GetResults,
};

DEFINE_IINSPECTABLE( async_info, IAsyncInfo, struct async_value, IAsyncOperation_iface )

static HRESULT WINAPI async_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = impl_from_IAsyncInfo( iface )->id;
    return S_OK;
}

static HRESULT WINAPI async_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_value *impl = impl_from_IAsyncInfo( iface );

    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    *status = SUCCEEDED( impl->hr ) ? Completed : Error;
    return S_OK;
}

static HRESULT WINAPI async_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    struct async_value *impl = impl_from_IAsyncInfo( iface );

    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    *error_code = impl->hr;
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

static HRESULT async_create( enum async_kind kind, HSTRING string, IUnknown *object, HRESULT hr, void **out )
{
    static LONG next_id;
    struct async_value *impl;
    HRESULT result;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_iface.lpVtbl = &async_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_info_vtbl;
    impl->ref = 1;
    impl->kind = kind;
    impl->hr = hr;
    impl->id = InterlockedIncrement( &next_id );
    if (kind == ASYNC_HSTRING)
    {
        if (SUCCEEDED( hr ) && FAILED( result = WindowsDuplicateString( string, &impl->string )))
        {
            free( impl );
            return result;
        }
    }
    else if (object && SUCCEEDED( hr ))
    {
        impl->object = object;
        IUnknown_AddRef( object );
    }
    *out = &impl->IAsyncOperation_iface;
    return S_OK;
}

HRESULT async_hstring_completed( HSTRING value, HRESULT hr, IAsyncOperation_HSTRING **out )
{
    return async_create( ASYNC_HSTRING, value, NULL, hr, (void **)out );
}

HRESULT async_inspectable_completed( IInspectable *value, HRESULT hr, IAsyncOperation_IInspectable **out )
{
    return async_create( ASYNC_OBJECT, NULL, (IUnknown *)value, hr, (void **)out );
}

HRESULT async_uri_completed( IUriRuntimeClass *value, HRESULT hr, IAsyncOperation_Uri **out )
{
    return async_create( ASYNC_URI, NULL, (IUnknown *)value, hr, (void **)out );
}
