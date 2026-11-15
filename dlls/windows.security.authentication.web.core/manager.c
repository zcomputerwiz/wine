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

static BOOL no_provider_fixme_done = FALSE;

static void no_provider_fixme( void )
{
    if (!no_provider_fixme_done)
    {
        no_provider_fixme_done = TRUE;
        FIXME( "no account provider exists in Wine; returning a not-available result.\n" );
    }
}

static HRESULT create_not_available_result( IWebTokenRequestResult **out )
{
    IVectorView_WebTokenResponse *response_data;
    IWebProviderError *response_error;
    HSTRING message;
    HRESULT hr;

    if (FAILED(hr = WindowsCreateString( L"No account provider is available.",
                                         ARRAY_SIZE(L"No account provider is available."), &message )))
        return hr;
    if (FAILED(hr = vector_view_webtokenresponse_create_empty( &response_data )))
    {
        WindowsDeleteString( message );
        return hr;
    }
    if (FAILED(hr = webprovidererror_create( 0x80070490, message, &response_error )))
    {
        WindowsDeleteString( message );
        IVectorView_WebTokenResponse_Release( response_data );
        return hr;
    }
    WindowsDeleteString( message );

    hr = webtokenrequestresult_create( WebTokenRequestStatus_AccountProviderNotAvailable, response_data, response_error, out );
    IVectorView_WebTokenResponse_Release( response_data );
    IWebProviderError_Release( response_error );
    return hr;
}

static HRESULT create_find_all_result( IFindAllAccountsResult **out )
{
    IVectorView_WebAccount *accounts;
    HRESULT hr;

    if (FAILED(hr = vector_view_webaccount_create_empty( &accounts ))) return hr;
    hr = findallaccountsresult_create( FindAllWebAccountsStatus_NotSupportedByProvider, accounts, NULL, out );
    IVectorView_WebAccount_Release( accounts );
    return hr;
}

/* Create a completed IAsyncOperation<T> (iid) and return its riid interface. */
static HRESULT create_completed_operation_riid( const GUID *iid, IInspectable *result, REFIID riid, void **out )
{
    IAsyncOperation_IInspectable *operation;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = async_operation_inspectable_create_completed( iid, result, &operation ))) return hr;
    hr = IUnknown_QueryInterface( (IUnknown *)operation, riid, out );
    IAsyncOperation_IInspectable_Release( operation );
    return hr;
}

static HRESULT create_completed_operation( const GUID *iid, IInspectable *result, void **out )
{
    return create_completed_operation_riid( iid, result, iid, out );
}

/* The result of every token request while no account provider exists. */
static HRESULT request_token_not_available( REFIID riid, void **out )
{
    IWebTokenRequestResult *result;
    HRESULT hr;

    no_provider_fixme();
    if (FAILED(hr = create_not_available_result( &result ))) return hr;
    hr = create_completed_operation_riid( &IID_IAsyncOperation_WebTokenRequestResult, (IInspectable *)result, riid, out );
    IWebTokenRequestResult_Release( result );
    return hr;
}

struct manager
{
    IActivationFactory IActivationFactory_iface;
    IWebAuthenticationCoreManagerStatics IWebAuthenticationCoreManagerStatics_iface;
    IWebAuthenticationCoreManagerStatics4 IWebAuthenticationCoreManagerStatics4_iface;
    IWebAuthenticationCoreManagerInterop IWebAuthenticationCoreManagerInterop_iface;
    LONG ref;
};

static inline struct manager *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct manager, IActivationFactory_iface );
}

static inline struct manager *impl_from_IWebAuthenticationCoreManagerStatics( IWebAuthenticationCoreManagerStatics *iface )
{
    return CONTAINING_RECORD( iface, struct manager, IWebAuthenticationCoreManagerStatics_iface );
}

static inline struct manager *impl_from_IWebAuthenticationCoreManagerStatics4( IWebAuthenticationCoreManagerStatics4 *iface )
{
    return CONTAINING_RECORD( iface, struct manager, IWebAuthenticationCoreManagerStatics4_iface );
}

static inline struct manager *impl_from_IWebAuthenticationCoreManagerInterop( IWebAuthenticationCoreManagerInterop *iface )
{
    return CONTAINING_RECORD( iface, struct manager, IWebAuthenticationCoreManagerInterop_iface );
}

static HRESULT manager_QueryInterface( struct manager *impl, REFIID iid, void **out )
{
    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebAuthenticationCoreManagerStatics ))
    {
        *out = &impl->IWebAuthenticationCoreManagerStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebAuthenticationCoreManagerStatics4 ))
    {
        *out = &impl->IWebAuthenticationCoreManagerStatics4_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebAuthenticationCoreManagerInterop ))
    {
        *out = &impl->IWebAuthenticationCoreManagerInterop_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG manager_AddRef( struct manager *impl )
{
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "impl %p, ref %lu.\n", impl, ref );
    return ref;
}

static ULONG manager_Release( struct manager *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "impl %p, ref %lu.\n", impl, ref );
    return ref;
}

static HRESULT manager_GetIids( struct manager *impl, ULONG *iid_count, IID **iids )
{
    FIXME( "impl %p, iid_count %p, iids %p stub!\n", impl, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT manager_GetRuntimeClassName( struct manager *impl, HSTRING *class_name )
{
    FIXME( "impl %p, class_name %p stub!\n", impl, class_name );
    return E_NOTIMPL;
}

static HRESULT manager_GetTrustLevel( struct manager *impl, TrustLevel *trust_level )
{
    FIXME( "impl %p, trust_level %p stub!\n", impl, trust_level );
    return E_NOTIMPL;
}

/*
 *
 * IActivationFactory
 *
 */

static HRESULT WINAPI manager_IActivationFactory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    return manager_QueryInterface( impl_from_IActivationFactory( iface ), iid, out );
}

static ULONG WINAPI manager_IActivationFactory_AddRef( IActivationFactory *iface )
{
    return manager_AddRef( impl_from_IActivationFactory( iface ) );
}

static ULONG WINAPI manager_IActivationFactory_Release( IActivationFactory *iface )
{
    return manager_Release( impl_from_IActivationFactory( iface ) );
}

static HRESULT WINAPI manager_IActivationFactory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    return manager_GetIids( impl_from_IActivationFactory( iface ), iid_count, iids );
}

static HRESULT WINAPI manager_IActivationFactory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return manager_GetRuntimeClassName( impl_from_IActivationFactory( iface ), class_name );
}

static HRESULT WINAPI manager_IActivationFactory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    return manager_GetTrustLevel( impl_from_IActivationFactory( iface ), trust_level );
}

static HRESULT WINAPI manager_IActivationFactory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl manager_activation_factory_vtbl =
{
    manager_IActivationFactory_QueryInterface,
    manager_IActivationFactory_AddRef,
    manager_IActivationFactory_Release,
    /* IInspectable methods */
    manager_IActivationFactory_GetIids,
    manager_IActivationFactory_GetRuntimeClassName,
    manager_IActivationFactory_GetTrustLevel,
    /* IActivationFactory methods */
    manager_IActivationFactory_ActivateInstance,
};

/*
 *
 * IWebAuthenticationCoreManagerStatics
 *
 */

static HRESULT WINAPI manager_statics_QueryInterface( IWebAuthenticationCoreManagerStatics *iface, REFIID iid, void **out )
{
    return manager_QueryInterface( impl_from_IWebAuthenticationCoreManagerStatics( iface ), iid, out );
}

static ULONG WINAPI manager_statics_AddRef( IWebAuthenticationCoreManagerStatics *iface )
{
    return manager_AddRef( impl_from_IWebAuthenticationCoreManagerStatics( iface ) );
}

static ULONG WINAPI manager_statics_Release( IWebAuthenticationCoreManagerStatics *iface )
{
    return manager_Release( impl_from_IWebAuthenticationCoreManagerStatics( iface ) );
}

static HRESULT WINAPI manager_statics_GetIids( IWebAuthenticationCoreManagerStatics *iface, ULONG *iid_count, IID **iids )
{
    return manager_GetIids( impl_from_IWebAuthenticationCoreManagerStatics( iface ), iid_count, iids );
}

static HRESULT WINAPI manager_statics_GetRuntimeClassName( IWebAuthenticationCoreManagerStatics *iface, HSTRING *class_name )
{
    return manager_GetRuntimeClassName( impl_from_IWebAuthenticationCoreManagerStatics( iface ), class_name );
}

static HRESULT WINAPI manager_statics_GetTrustLevel( IWebAuthenticationCoreManagerStatics *iface, TrustLevel *trust_level )
{
    return manager_GetTrustLevel( impl_from_IWebAuthenticationCoreManagerStatics( iface ), trust_level );
}

static HRESULT WINAPI manager_statics_GetTokenSilentlyAsync( IWebAuthenticationCoreManagerStatics *iface,
                                                             IWebTokenRequest *request,
                                                             IAsyncOperation_WebTokenRequestResult **asyncInfo )
{
    TRACE( "iface %p, request %p, asyncInfo %p.\n", iface, request, asyncInfo );

    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;
    if (!request) return E_INVALIDARG;

    return request_token_not_available( &IID_IAsyncOperation_WebTokenRequestResult, (void **)asyncInfo );
}

static HRESULT WINAPI manager_statics_GetTokenSilentlyWithWebAccountAsync( IWebAuthenticationCoreManagerStatics *iface,
                                                                           IWebTokenRequest *request, IWebAccount *webAccount,
                                                                           IAsyncOperation_WebTokenRequestResult **asyncInfo )
{
    TRACE( "iface %p, request %p, webAccount %p, asyncInfo %p.\n", iface, request, webAccount, asyncInfo );

    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;
    if (!request) return E_INVALIDARG;

    return request_token_not_available( &IID_IAsyncOperation_WebTokenRequestResult, (void **)asyncInfo );
}

static HRESULT WINAPI manager_statics_RequestTokenAsync( IWebAuthenticationCoreManagerStatics *iface,
                                                         IWebTokenRequest *request,
                                                         IAsyncOperation_WebTokenRequestResult **asyncInfo )
{
    TRACE( "iface %p, request %p, asyncInfo %p.\n", iface, request, asyncInfo );

    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;
    if (!request) return E_INVALIDARG;

    return request_token_not_available( &IID_IAsyncOperation_WebTokenRequestResult, (void **)asyncInfo );
}

static HRESULT WINAPI manager_statics_RequestTokenWithWebAccountAsync( IWebAuthenticationCoreManagerStatics *iface,
                                                                       IWebTokenRequest *request, IWebAccount *webAccount,
                                                                       IAsyncOperation_WebTokenRequestResult **asyncInfo )
{
    TRACE( "iface %p, request %p, webAccount %p, asyncInfo %p.\n", iface, request, webAccount, asyncInfo );

    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;
    if (!request) return E_INVALIDARG;

    return request_token_not_available( &IID_IAsyncOperation_WebTokenRequestResult, (void **)asyncInfo );
}

static HRESULT WINAPI manager_statics_FindAccountAsync( IWebAuthenticationCoreManagerStatics *iface,
                                                        IWebAccountProvider *provider, HSTRING webAccountId,
                                                        IAsyncOperation_WebAccount **asyncInfo )
{
    TRACE( "iface %p, provider %p, webAccountId %s, asyncInfo %p.\n", iface, provider, debugstr_hstring( webAccountId ), asyncInfo );

    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;

    no_provider_fixme();

    return create_completed_operation( &IID_IAsyncOperation_WebAccount, NULL, (void **)asyncInfo );
}

static HRESULT WINAPI manager_statics_FindAccountProviderAsync( IWebAuthenticationCoreManagerStatics *iface,
                                                                HSTRING webAccountProviderId,
                                                                IAsyncOperation_WebAccountProvider **asyncInfo )
{
    TRACE( "iface %p, webAccountProviderId %s, asyncInfo %p.\n", iface, debugstr_hstring( webAccountProviderId ), asyncInfo );

    /* Windows checks the provider id first */
    if (!WindowsGetStringLen( webAccountProviderId )) return E_INVALIDARG;
    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;

    no_provider_fixme();

    return create_completed_operation( &IID_IAsyncOperation_WebAccountProvider, NULL, (void **)asyncInfo );
}

static HRESULT WINAPI manager_statics_FindAccountProviderWithAuthorityAsync( IWebAuthenticationCoreManagerStatics *iface,
                                                                             HSTRING webAccountProviderId, HSTRING authority,
                                                                             IAsyncOperation_WebAccountProvider **asyncInfo )
{
    TRACE( "iface %p, webAccountProviderId %s, authority %s, asyncInfo %p.\n",
           iface, debugstr_hstring( webAccountProviderId ), debugstr_hstring( authority ), asyncInfo );

    if (!WindowsGetStringLen( webAccountProviderId )) return E_INVALIDARG;
    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;

    no_provider_fixme();

    return create_completed_operation( &IID_IAsyncOperation_WebAccountProvider, NULL, (void **)asyncInfo );
}

static const struct IWebAuthenticationCoreManagerStaticsVtbl manager_statics_vtbl =
{
    manager_statics_QueryInterface,
    manager_statics_AddRef,
    manager_statics_Release,
    /* IInspectable methods */
    manager_statics_GetIids,
    manager_statics_GetRuntimeClassName,
    manager_statics_GetTrustLevel,
    /* IWebAuthenticationCoreManagerStatics methods */
    manager_statics_GetTokenSilentlyAsync,
    manager_statics_GetTokenSilentlyWithWebAccountAsync,
    manager_statics_RequestTokenAsync,
    manager_statics_RequestTokenWithWebAccountAsync,
    manager_statics_FindAccountAsync,
    manager_statics_FindAccountProviderAsync,
    manager_statics_FindAccountProviderWithAuthorityAsync,
};

/*
 *
 * IWebAuthenticationCoreManagerStatics4
 *
 */

static HRESULT WINAPI manager_statics4_QueryInterface( IWebAuthenticationCoreManagerStatics4 *iface, REFIID iid, void **out )
{
    return manager_QueryInterface( impl_from_IWebAuthenticationCoreManagerStatics4( iface ), iid, out );
}

static ULONG WINAPI manager_statics4_AddRef( IWebAuthenticationCoreManagerStatics4 *iface )
{
    return manager_AddRef( impl_from_IWebAuthenticationCoreManagerStatics4( iface ) );
}

static ULONG WINAPI manager_statics4_Release( IWebAuthenticationCoreManagerStatics4 *iface )
{
    return manager_Release( impl_from_IWebAuthenticationCoreManagerStatics4( iface ) );
}

static HRESULT WINAPI manager_statics4_GetIids( IWebAuthenticationCoreManagerStatics4 *iface, ULONG *iid_count, IID **iids )
{
    return manager_GetIids( impl_from_IWebAuthenticationCoreManagerStatics4( iface ), iid_count, iids );
}

static HRESULT WINAPI manager_statics4_GetRuntimeClassName( IWebAuthenticationCoreManagerStatics4 *iface, HSTRING *class_name )
{
    return manager_GetRuntimeClassName( impl_from_IWebAuthenticationCoreManagerStatics4( iface ), class_name );
}

static HRESULT WINAPI manager_statics4_GetTrustLevel( IWebAuthenticationCoreManagerStatics4 *iface, TrustLevel *trust_level )
{
    return manager_GetTrustLevel( impl_from_IWebAuthenticationCoreManagerStatics4( iface ), trust_level );
}

static HRESULT WINAPI manager_statics4_FindAllAccountsAsync( IWebAuthenticationCoreManagerStatics4 *iface,
                                                             IWebAccountProvider *provider,
                                                             IAsyncOperation_FindAllAccountsResult **operation )
{
    IFindAllAccountsResult *result;
    HRESULT hr;

    TRACE( "iface %p, provider %p, operation %p.\n", iface, provider, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;

    no_provider_fixme();

    if (FAILED(hr = create_find_all_result( &result ))) return hr;
    hr = create_completed_operation( &IID_IAsyncOperation_FindAllAccountsResult, (IInspectable *)result, (void **)operation );
    IFindAllAccountsResult_Release( result );
    return hr;
}

static HRESULT WINAPI manager_statics4_FindAllAccountsWithClientIdAsync( IWebAuthenticationCoreManagerStatics4 *iface,
                                                                         IWebAccountProvider *provider, HSTRING clientId,
                                                                         IAsyncOperation_FindAllAccountsResult **operation )
{
    IFindAllAccountsResult *result;
    HRESULT hr;

    TRACE( "iface %p, provider %p, clientId %s, operation %p.\n", iface, provider, debugstr_hstring( clientId ), operation );

    if (!operation) return E_POINTER;
    *operation = NULL;

    no_provider_fixme();

    if (FAILED(hr = create_find_all_result( &result ))) return hr;
    hr = create_completed_operation( &IID_IAsyncOperation_FindAllAccountsResult, (IInspectable *)result, (void **)operation );
    IFindAllAccountsResult_Release( result );
    return hr;
}

static HRESULT WINAPI manager_statics4_FindSystemAccountProviderAsync( IWebAuthenticationCoreManagerStatics4 *iface,
                                                                       HSTRING webAccountProviderId,
                                                                       IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, webAccountProviderId %s, operation %p.\n", iface, debugstr_hstring( webAccountProviderId ), operation );

    if (!operation) return E_POINTER;
    *operation = NULL;

    no_provider_fixme();

    return create_completed_operation( &IID_IAsyncOperation_WebAccountProvider, NULL, (void **)operation );
}

static HRESULT WINAPI manager_statics4_FindSystemAccountProviderWithAuthorityAsync( IWebAuthenticationCoreManagerStatics4 *iface,
                                                                                    HSTRING webAccountProviderId, HSTRING authority,
                                                                                    IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, webAccountProviderId %s, authority %s, operation %p.\n",
           iface, debugstr_hstring( webAccountProviderId ), debugstr_hstring( authority ), operation );

    if (!operation) return E_POINTER;
    *operation = NULL;

    no_provider_fixme();

    return create_completed_operation( &IID_IAsyncOperation_WebAccountProvider, NULL, (void **)operation );
}

static HRESULT WINAPI manager_statics4_FindSystemAccountProviderWithAuthorityForUserAsync( IWebAuthenticationCoreManagerStatics4 *iface,
                                                                                           HSTRING webAccountProviderId, HSTRING authority,
                                                                                           IUser *user,
                                                                                           IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, webAccountProviderId %s, authority %s, user %p, operation %p.\n",
           iface, debugstr_hstring( webAccountProviderId ), debugstr_hstring( authority ), user, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;

    no_provider_fixme();

    return create_completed_operation( &IID_IAsyncOperation_WebAccountProvider, NULL, (void **)operation );
}

static const struct IWebAuthenticationCoreManagerStatics4Vtbl manager_statics4_vtbl =
{
    manager_statics4_QueryInterface,
    manager_statics4_AddRef,
    manager_statics4_Release,
    /* IInspectable methods */
    manager_statics4_GetIids,
    manager_statics4_GetRuntimeClassName,
    manager_statics4_GetTrustLevel,
    /* IWebAuthenticationCoreManagerStatics4 methods */
    manager_statics4_FindAllAccountsAsync,
    manager_statics4_FindAllAccountsWithClientIdAsync,
    manager_statics4_FindSystemAccountProviderAsync,
    manager_statics4_FindSystemAccountProviderWithAuthorityAsync,
    manager_statics4_FindSystemAccountProviderWithAuthorityForUserAsync,
};

/*
 *
 * IWebAuthenticationCoreManagerInterop
 *
 */

static HRESULT WINAPI manager_interop_QueryInterface( IWebAuthenticationCoreManagerInterop *iface, REFIID iid, void **out )
{
    return manager_QueryInterface( impl_from_IWebAuthenticationCoreManagerInterop( iface ), iid, out );
}

static ULONG WINAPI manager_interop_AddRef( IWebAuthenticationCoreManagerInterop *iface )
{
    return manager_AddRef( impl_from_IWebAuthenticationCoreManagerInterop( iface ) );
}

static ULONG WINAPI manager_interop_Release( IWebAuthenticationCoreManagerInterop *iface )
{
    return manager_Release( impl_from_IWebAuthenticationCoreManagerInterop( iface ) );
}

static HRESULT WINAPI manager_interop_GetIids( IWebAuthenticationCoreManagerInterop *iface, ULONG *iid_count, IID **iids )
{
    return manager_GetIids( impl_from_IWebAuthenticationCoreManagerInterop( iface ), iid_count, iids );
}

static HRESULT WINAPI manager_interop_GetRuntimeClassName( IWebAuthenticationCoreManagerInterop *iface, HSTRING *class_name )
{
    return manager_GetRuntimeClassName( impl_from_IWebAuthenticationCoreManagerInterop( iface ), class_name );
}

static HRESULT WINAPI manager_interop_GetTrustLevel( IWebAuthenticationCoreManagerInterop *iface, TrustLevel *trust_level )
{
    return manager_GetTrustLevel( impl_from_IWebAuthenticationCoreManagerInterop( iface ), trust_level );
}

static HRESULT WINAPI manager_interop_RequestTokenForWindowAsync( IWebAuthenticationCoreManagerInterop *iface, HWND appWindow,
                                                                  IInspectable *request, REFIID riid, void **asyncInfo )
{
    TRACE( "iface %p, appWindow %p, request %p, riid %s, asyncInfo %p.\n",
           iface, appWindow, request, debugstr_guid( riid ), asyncInfo );

    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;
    if (!request) return E_INVALIDARG;

    return request_token_not_available( riid, asyncInfo );
}

static HRESULT WINAPI manager_interop_RequestTokenWithWebAccountForWindowAsync( IWebAuthenticationCoreManagerInterop *iface,
                                                                                HWND appWindow, IInspectable *request,
                                                                                IInspectable *webAccount, REFIID riid,
                                                                                void **asyncInfo )
{
    TRACE( "iface %p, appWindow %p, request %p, webAccount %p, riid %s, asyncInfo %p.\n",
           iface, appWindow, request, webAccount, debugstr_guid( riid ), asyncInfo );

    if (!asyncInfo) return E_POINTER;
    *asyncInfo = NULL;
    if (!request) return E_INVALIDARG;

    return request_token_not_available( riid, asyncInfo );
}

static const struct IWebAuthenticationCoreManagerInteropVtbl manager_interop_vtbl =
{
    manager_interop_QueryInterface,
    manager_interop_AddRef,
    manager_interop_Release,
    /* IInspectable methods */
    manager_interop_GetIids,
    manager_interop_GetRuntimeClassName,
    manager_interop_GetTrustLevel,
    /* IWebAuthenticationCoreManagerInterop methods */
    manager_interop_RequestTokenForWindowAsync,
    manager_interop_RequestTokenWithWebAccountForWindowAsync,
};

static struct manager manager_statics =
{
    {&manager_activation_factory_vtbl},
    {&manager_statics_vtbl},
    {&manager_statics4_vtbl},
    {&manager_interop_vtbl},
    1,
};

IActivationFactory *manager_factory = &manager_statics.IActivationFactory_iface;
