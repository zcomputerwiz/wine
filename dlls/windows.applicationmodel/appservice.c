/* Windows.ApplicationModel.AppService (WineExplorer UWP)
 *
 * An application service is a service another application (or a background task of the same package) offers to its
 * callers. No provider is hosted yet: a connection opens with the status AppServiceUnavailable, which an application
 * that uses a service of its own package has to be ready for (the service may be disabled), and a message sent through
 * it is answered with the response status Failure. The classes exist so that an application that constructs one does
 * not fail where it stands.
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
#include "asyncinfo.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(model);

/* An asynchronous operation that is complete when it is made: the result is known, the handler runs when it is set. */
#define DEFINE_COMPLETED_OPERATION( impl_type, iface_type, handler_type, result_type, copy_result, release_result, class_name, create_fn ) \
struct impl_type                                                                                                    \
{                                                                                                                   \
    iface_type iface_type##_iface;                                                                                  \
    IAsyncInfo IAsyncInfo_iface;                                                                                    \
    LONG ref;                                                                                                       \
    handler_type *handler;                                                                                          \
    result_type value;                                                                                              \
};                                                                                                                  \
static inline struct impl_type *impl_type##_from_op( iface_type *iface )                                            \
{                                                                                                                   \
    return CONTAINING_RECORD( iface, struct impl_type, iface_type##_iface );                                        \
}                                                                                                                   \
static inline struct impl_type *impl_type##_from_info( IAsyncInfo *iface )                                          \
{                                                                                                                   \
    return CONTAINING_RECORD( iface, struct impl_type, IAsyncInfo_iface );                                          \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_QueryInterface( iface_type *iface, REFIID iid, void **out )                       \
{                                                                                                                   \
    struct impl_type *impl = impl_type##_from_op( iface );                                                          \
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||                               \
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_##iface_type ))                             \
        *out = &impl->iface_type##_iface;                                                                           \
    else if (IsEqualGUID( iid, &IID_IAsyncInfo )) *out = &impl->IAsyncInfo_iface;                                   \
    else { *out = NULL; return E_NOINTERFACE; }                                                                     \
    IUnknown_AddRef( (IUnknown *)*out );                                                                            \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static ULONG WINAPI impl_type##_AddRef( iface_type *iface )                                                         \
{                                                                                                                   \
    return InterlockedIncrement( &impl_type##_from_op( iface )->ref );                                              \
}                                                                                                                   \
static ULONG WINAPI impl_type##_Release( iface_type *iface )                                                        \
{                                                                                                                   \
    struct impl_type *impl = impl_type##_from_op( iface );                                                          \
    ULONG ref = InterlockedDecrement( &impl->ref );                                                                 \
    if (!ref)                                                                                                       \
    {                                                                                                               \
        if (impl->handler) IUnknown_Release( (IUnknown *)impl->handler );                                           \
        release_result( impl->value );                                                                              \
        free( impl );                                                                                               \
    }                                                                                                               \
    return ref;                                                                                                     \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetIids( iface_type *iface, ULONG *count, IID **iids )                            \
{                                                                                                                   \
    IID *list;                                                                                                      \
    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;                                        \
    list[0] = IID_##iface_type;                                                                                     \
    list[1] = IID_IAsyncInfo;                                                                                       \
    *count = 2;                                                                                                     \
    *iids = list;                                                                                                   \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetRuntimeClassName( iface_type *iface, HSTRING *name )                           \
{                                                                                                                   \
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );                                     \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetTrustLevel( iface_type *iface, TrustLevel *level )                             \
{                                                                                                                   \
    *level = BaseTrust;                                                                                             \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_put_Completed( iface_type *iface, handler_type *handler )                         \
{                                                                                                                   \
    struct impl_type *impl = impl_type##_from_op( iface );                                                          \
    TRACE( "iface %p, handler %p.\n", iface, handler );                                                             \
    if (impl->handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;                                                        \
    if (!handler) return S_OK;                                                                                      \
    impl->handler = handler;                                                                                        \
    IUnknown_AddRef( (IUnknown *)handler );                                                                         \
    handler_type##_Invoke( handler, iface, Completed );                                                             \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_get_Completed( iface_type *iface, handler_type **handler )                        \
{                                                                                                                   \
    struct impl_type *impl = impl_type##_from_op( iface );                                                          \
    if ((*handler = impl->handler)) IUnknown_AddRef( (IUnknown *)*handler );                                        \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_GetResults( iface_type *iface, result_type *results )                             \
{                                                                                                                   \
    struct impl_type *impl = impl_type##_from_op( iface );                                                          \
    TRACE( "iface %p, results %p.\n", iface, results );                                                             \
    copy_result( results, impl->value );                                                                            \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static const struct iface_type##Vtbl impl_type##_vtbl =                                                             \
{                                                                                                                   \
    impl_type##_QueryInterface,                                                                                     \
    impl_type##_AddRef,                                                                                             \
    impl_type##_Release,                                                                                            \
    impl_type##_GetIids,                                                                                            \
    impl_type##_GetRuntimeClassName,                                                                                \
    impl_type##_GetTrustLevel,                                                                                      \
    impl_type##_put_Completed,                                                                                      \
    impl_type##_get_Completed,                                                                                      \
    impl_type##_GetResults,                                                                                         \
};                                                                                                                  \
static HRESULT WINAPI impl_type##_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )                  \
{                                                                                                                   \
    return impl_type##_QueryInterface( &impl_type##_from_info( iface )->iface_type##_iface, iid, out );             \
}                                                                                                                   \
static ULONG WINAPI impl_type##_info_AddRef( IAsyncInfo *iface )                                                    \
{                                                                                                                   \
    return impl_type##_AddRef( &impl_type##_from_info( iface )->iface_type##_iface );                               \
}                                                                                                                   \
static ULONG WINAPI impl_type##_info_Release( IAsyncInfo *iface )                                                   \
{                                                                                                                   \
    return impl_type##_Release( &impl_type##_from_info( iface )->iface_type##_iface );                              \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids )                       \
{                                                                                                                   \
    return impl_type##_GetIids( &impl_type##_from_info( iface )->iface_type##_iface, count, iids );                 \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name )                      \
{                                                                                                                   \
    return impl_type##_GetRuntimeClassName( &impl_type##_from_info( iface )->iface_type##_iface, name );            \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )                        \
{                                                                                                                   \
    *level = BaseTrust;                                                                                             \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_get_Id( IAsyncInfo *iface, UINT32 *id )                                      \
{                                                                                                                   \
    *id = 1;                                                                                                        \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )                         \
{                                                                                                                   \
    TRACE( "iface %p.\n", iface );                                                                                  \
    *status = Completed;                                                                                            \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *code )                            \
{                                                                                                                   \
    *code = S_OK;                                                                                                   \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_Cancel( IAsyncInfo *iface )                                                  \
{                                                                                                                   \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI impl_type##_info_Close( IAsyncInfo *iface )                                                   \
{                                                                                                                   \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static const struct IAsyncInfoVtbl impl_type##_info_vtbl =                                                          \
{                                                                                                                   \
    impl_type##_info_QueryInterface,                                                                                \
    impl_type##_info_AddRef,                                                                                        \
    impl_type##_info_Release,                                                                                       \
    impl_type##_info_GetIids,                                                                                       \
    impl_type##_info_GetRuntimeClassName,                                                                           \
    impl_type##_info_GetTrustLevel,                                                                                 \
    impl_type##_info_get_Id,                                                                                        \
    impl_type##_info_get_Status,                                                                                    \
    impl_type##_info_get_ErrorCode,                                                                                 \
    impl_type##_info_Cancel,                                                                                        \
    impl_type##_info_Close,                                                                                         \
};                                                                                                                  \
static HRESULT create_fn( result_type value, iface_type **out )                                                     \
{                                                                                                                   \
    struct impl_type *impl;                                                                                         \
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;                                                 \
    impl->iface_type##_iface.lpVtbl = &impl_type##_vtbl;                                                            \
    impl->IAsyncInfo_iface.lpVtbl = &impl_type##_info_vtbl;                                                         \
    impl->ref = 1;                                                                                                  \
    copy_result( &impl->value, value );                                                                             \
    *out = &impl->iface_type##_iface;                                                                               \
    return S_OK;                                                                                                    \
}

#define COPY_VALUE( dest, src ) (*(dest) = (src))
#define NO_RELEASE( value ) ((void)(value))
#define COPY_INTERFACE( dest, src ) do { if ((*(dest) = (src))) IUnknown_AddRef( (IUnknown *)(src) ); } while (0)
#define RELEASE_INTERFACE( value ) do { if (value) IUnknown_Release( (IUnknown *)(value) ); } while (0)

DEFINE_COMPLETED_OPERATION( status_operation, IAsyncOperation_AppServiceConnectionStatus,
                            IAsyncOperationCompletedHandler_AppServiceConnectionStatus, AppServiceConnectionStatus,
                            COPY_VALUE, NO_RELEASE,
                            L"Windows.Foundation.IAsyncOperation`1<Windows.ApplicationModel.AppService.AppServiceConnectionStatus>",
                            status_operation_create )

DEFINE_COMPLETED_OPERATION( response_operation, IAsyncOperation_AppServiceResponse,
                            IAsyncOperationCompletedHandler_AppServiceResponse, IAppServiceResponse *,
                            COPY_INTERFACE, RELEASE_INTERFACE,
                            L"Windows.Foundation.IAsyncOperation`1<Windows.ApplicationModel.AppService.AppServiceResponse>",
                            response_operation_create )

/* AppServiceResponse */

struct response
{
    IAppServiceResponse IAppServiceResponse_iface;
    LONG ref;
    AppServiceResponseStatus status;
};

static inline struct response *impl_from_IAppServiceResponse( IAppServiceResponse *iface )
{
    return CONTAINING_RECORD( iface, struct response, IAppServiceResponse_iface );
}

static HRESULT WINAPI response_QueryInterface( IAppServiceResponse *iface, REFIID iid, void **out )
{
    struct response *impl = impl_from_IAppServiceResponse( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppServiceResponse ))
    {
        *out = &impl->IAppServiceResponse_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI response_AddRef( IAppServiceResponse *iface )
{
    return InterlockedIncrement( &impl_from_IAppServiceResponse( iface )->ref );
}

static ULONG WINAPI response_Release( IAppServiceResponse *iface )
{
    struct response *impl = impl_from_IAppServiceResponse( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI response_GetIids( IAppServiceResponse *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IAppServiceResponse;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI response_GetRuntimeClassName( IAppServiceResponse *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.AppService.AppServiceResponse",
                                ARRAY_SIZE(L"Windows.ApplicationModel.AppService.AppServiceResponse") - 1, name );
}

static HRESULT WINAPI response_GetTrustLevel( IAppServiceResponse *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI response_get_Message( IAppServiceResponse *iface, IPropertySet **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI response_get_Status( IAppServiceResponse *iface, AppServiceResponseStatus *value )
{
    *value = impl_from_IAppServiceResponse( iface )->status;
    return S_OK;
}

static const struct IAppServiceResponseVtbl response_vtbl =
{
    response_QueryInterface,
    response_AddRef,
    response_Release,
    response_GetIids,
    response_GetRuntimeClassName,
    response_GetTrustLevel,
    response_get_Message,
    response_get_Status,
};

static HRESULT response_create( AppServiceResponseStatus status, IAppServiceResponse **out )
{
    struct response *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAppServiceResponse_iface.lpVtbl = &response_vtbl;
    impl->ref = 1;
    impl->status = status;
    *out = &impl->IAppServiceResponse_iface;
    return S_OK;
}

/* AppServiceConnection */

struct connection
{
    IAppServiceConnection IAppServiceConnection_iface;
    IClosable IClosable_iface;
    LONG ref;
    LONG next_token;
    HSTRING service_name;
    HSTRING family_name;
};

static inline struct connection *impl_from_IAppServiceConnection( IAppServiceConnection *iface )
{
    return CONTAINING_RECORD( iface, struct connection, IAppServiceConnection_iface );
}

static inline struct connection *impl_from_IClosable( IClosable *iface )
{
    return CONTAINING_RECORD( iface, struct connection, IClosable_iface );
}

static HRESULT WINAPI connection_QueryInterface( IAppServiceConnection *iface, REFIID iid, void **out )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppServiceConnection ))
        *out = &impl->IAppServiceConnection_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI connection_AddRef( IAppServiceConnection *iface )
{
    return InterlockedIncrement( &impl_from_IAppServiceConnection( iface )->ref );
}

static ULONG WINAPI connection_Release( IAppServiceConnection *iface )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->service_name );
        WindowsDeleteString( impl->family_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI connection_GetIids( IAppServiceConnection *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IAppServiceConnection;
    list[1] = IID_IClosable;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI connection_GetRuntimeClassName( IAppServiceConnection *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.AppService.AppServiceConnection",
                                ARRAY_SIZE(L"Windows.ApplicationModel.AppService.AppServiceConnection") - 1, name );
}

static HRESULT WINAPI connection_GetTrustLevel( IAppServiceConnection *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI connection_get_AppServiceName( IAppServiceConnection *iface, HSTRING *value )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl->service_name, value );
}

static HRESULT WINAPI connection_put_AppServiceName( IAppServiceConnection *iface, HSTRING value )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->service_name );
    impl->service_name = copy;
    return S_OK;
}

static HRESULT WINAPI connection_get_PackageFamilyName( IAppServiceConnection *iface, HSTRING *value )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl->family_name, value );
}

static HRESULT WINAPI connection_put_PackageFamilyName( IAppServiceConnection *iface, HSTRING value )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->family_name );
    impl->family_name = copy;
    return S_OK;
}

static HRESULT WINAPI connection_OpenAsync( IAppServiceConnection *iface, IAsyncOperation_AppServiceConnectionStatus **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    /* no provider is hosted: the service is not available (the name and the family are not even looked at) */
    return status_operation_create( AppServiceConnectionStatus_AppServiceUnavailable, operation );
}

static HRESULT WINAPI connection_SendMessageAsync( IAppServiceConnection *iface, IPropertySet *message,
                                                   IAsyncOperation_AppServiceResponse **operation )
{
    IAppServiceResponse *response;
    HRESULT hr;

    TRACE( "iface %p, message %p, operation %p.\n", iface, message, operation );
    if (FAILED(hr = response_create( AppServiceResponseStatus_Failure, &response ))) return hr;
    hr = response_operation_create( response, operation );
    IAppServiceResponse_Release( response );
    return hr;
}

static HRESULT WINAPI connection_add_RequestReceived( IAppServiceConnection *iface,
        ITypedEventHandler_AppServiceConnection_AppServiceRequestReceivedEventArgs *handler, EventRegistrationToken *token )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    token->value = InterlockedIncrement( &impl->next_token );
    return S_OK;
}

static HRESULT WINAPI connection_remove_RequestReceived( IAppServiceConnection *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %I64d.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI connection_add_ServiceClosed( IAppServiceConnection *iface,
        ITypedEventHandler_AppServiceConnection_AppServiceClosedEventArgs *handler, EventRegistrationToken *token )
{
    struct connection *impl = impl_from_IAppServiceConnection( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    token->value = InterlockedIncrement( &impl->next_token );
    return S_OK;
}

static HRESULT WINAPI connection_remove_ServiceClosed( IAppServiceConnection *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %I64d.\n", iface, token.value );
    return S_OK;
}

static const struct IAppServiceConnectionVtbl connection_vtbl =
{
    connection_QueryInterface,
    connection_AddRef,
    connection_Release,
    connection_GetIids,
    connection_GetRuntimeClassName,
    connection_GetTrustLevel,
    connection_get_AppServiceName,
    connection_put_AppServiceName,
    connection_get_PackageFamilyName,
    connection_put_PackageFamilyName,
    connection_OpenAsync,
    connection_SendMessageAsync,
    connection_add_RequestReceived,
    connection_remove_RequestReceived,
    connection_add_ServiceClosed,
    connection_remove_ServiceClosed,
};

static HRESULT WINAPI closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{
    return connection_QueryInterface( &impl_from_IClosable( iface )->IAppServiceConnection_iface, iid, out );
}

static ULONG WINAPI closable_AddRef( IClosable *iface )
{
    return connection_AddRef( &impl_from_IClosable( iface )->IAppServiceConnection_iface );
}

static ULONG WINAPI closable_Release( IClosable *iface )
{
    return connection_Release( &impl_from_IClosable( iface )->IAppServiceConnection_iface );
}

static HRESULT WINAPI closable_GetIids( IClosable *iface, ULONG *count, IID **iids )
{
    return connection_GetIids( &impl_from_IClosable( iface )->IAppServiceConnection_iface, count, iids );
}

static HRESULT WINAPI closable_GetRuntimeClassName( IClosable *iface, HSTRING *name )
{
    return connection_GetRuntimeClassName( &impl_from_IClosable( iface )->IAppServiceConnection_iface, name );
}

static HRESULT WINAPI closable_GetTrustLevel( IClosable *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI closable_Close( IClosable *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static const struct IClosableVtbl closable_vtbl =
{
    closable_QueryInterface,
    closable_AddRef,
    closable_Release,
    closable_GetIids,
    closable_GetRuntimeClassName,
    closable_GetTrustLevel,
    closable_Close,
};

struct factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct factory *impl = impl_from_IActivationFactory( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.AppService.AppServiceConnection",
                                ARRAY_SIZE(L"Windows.ApplicationModel.AppService.AppServiceConnection") - 1, name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct connection *impl;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    *instance = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAppServiceConnection_iface.lpVtbl = &connection_vtbl;
    impl->IClosable_iface.lpVtbl = &closable_vtbl;
    impl->ref = 1;
    *instance = (IInspectable *)&impl->IAppServiceConnection_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

static struct factory app_service_connection_statics = { {&factory_vtbl}, 1 };

IActivationFactory *app_service_connection_factory = &app_service_connection_statics.IActivationFactory_iface;
