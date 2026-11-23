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

struct web_token_request
{
    IWebTokenRequest IWebTokenRequest_iface;
    IWebTokenRequest2 IWebTokenRequest2_iface;
    IWebTokenRequest3 IWebTokenRequest3_iface;
    LONG ref;
    IWebAccountProvider *provider;
    HSTRING scope;
    HSTRING client_id;
    WebTokenRequestPromptType prompt_type;
    HSTRING correlation_id;
    IMap_HSTRING_HSTRING *properties;
    IMap_HSTRING_HSTRING *app_properties;
};

static inline struct web_token_request *impl_from_IWebTokenRequest( IWebTokenRequest *iface )
{
    return CONTAINING_RECORD( iface, struct web_token_request, IWebTokenRequest_iface );
}

static HRESULT WINAPI web_token_request_QueryInterface( IWebTokenRequest *iface, REFIID iid, void **out )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IWebTokenRequest ))
        *out = &impl->IWebTokenRequest_iface;
    else if (IsEqualGUID( iid, &IID_IWebTokenRequest2 ))
        *out = &impl->IWebTokenRequest2_iface;
    else if (IsEqualGUID( iid, &IID_IWebTokenRequest3 ))
        *out = &impl->IWebTokenRequest3_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    IInspectable_AddRef( *out );
    return S_OK;
}

static ULONG WINAPI web_token_request_AddRef( IWebTokenRequest *iface )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI web_token_request_Release( IWebTokenRequest *iface )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->app_properties) IMap_HSTRING_HSTRING_Release( impl->app_properties );
        if (impl->properties) IMap_HSTRING_HSTRING_Release( impl->properties );
        WindowsDeleteString( impl->correlation_id );
        WindowsDeleteString( impl->client_id );
        WindowsDeleteString( impl->scope );
        if (impl->provider) IWebAccountProvider_Release( impl->provider );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI web_token_request_GetIids( IWebTokenRequest *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_token_request_GetRuntimeClassName( IWebTokenRequest *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Security.Authentication.Web.Core.WebTokenRequest",
                                ARRAY_SIZE(L"Windows.Security.Authentication.Web.Core.WebTokenRequest") - 1,
                                class_name );
}

static HRESULT WINAPI web_token_request_GetTrustLevel( IWebTokenRequest *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_token_request_get_WebAccountProvider( IWebTokenRequest *iface, IWebAccountProvider **value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((*value = impl->provider)) IWebAccountProvider_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI web_token_request_get_Scope( IWebTokenRequest *iface, HSTRING *value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->scope, value );
}

static HRESULT WINAPI web_token_request_get_ClientId( IWebTokenRequest *iface, HSTRING *value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->client_id, value );
}

static HRESULT WINAPI web_token_request_get_PromptType( IWebTokenRequest *iface, WebTokenRequestPromptType *value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->prompt_type;
    return S_OK;
}

static HRESULT WINAPI web_token_request_get_Properties( IWebTokenRequest *iface, IMap_HSTRING_HSTRING **requestProperties )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );

    TRACE( "iface %p, requestProperties %p.\n", iface, requestProperties );

    if (!requestProperties) return E_POINTER;
    IMap_HSTRING_HSTRING_AddRef( (*requestProperties = impl->properties) );
    return S_OK;
}

static const struct IWebTokenRequestVtbl web_token_request_vtbl =
{
    /* IUnknown methods */
    web_token_request_QueryInterface,
    web_token_request_AddRef,
    web_token_request_Release,
    /* IInspectable methods */
    web_token_request_GetIids,
    web_token_request_GetRuntimeClassName,
    web_token_request_GetTrustLevel,
    /* IWebTokenRequest methods */
    web_token_request_get_WebAccountProvider,
    web_token_request_get_Scope,
    web_token_request_get_ClientId,
    web_token_request_get_PromptType,
    web_token_request_get_Properties,
};

DEFINE_IINSPECTABLE( web_token_request2, IWebTokenRequest2, struct web_token_request, IWebTokenRequest_iface )

static HRESULT WINAPI web_token_request_get_AppProperties( IWebTokenRequest2 *iface, IMap_HSTRING_HSTRING **requestProperties )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest2( iface );

    TRACE( "iface %p, requestProperties %p.\n", iface, requestProperties );

    if (!requestProperties) return E_POINTER;
    IMap_HSTRING_HSTRING_AddRef( (*requestProperties = impl->app_properties) );
    return S_OK;
}

static const struct IWebTokenRequest2Vtbl web_token_request2_vtbl =
{
    /* IUnknown methods */
    web_token_request2_QueryInterface,
    web_token_request2_AddRef,
    web_token_request2_Release,
    /* IInspectable methods */
    web_token_request2_GetIids,
    web_token_request2_GetRuntimeClassName,
    web_token_request2_GetTrustLevel,
    /* IWebTokenRequest2 methods */
    web_token_request_get_AppProperties,
};

DEFINE_IINSPECTABLE( web_token_request3, IWebTokenRequest3, struct web_token_request, IWebTokenRequest_iface )

static HRESULT WINAPI web_token_request_get_CorrelationId( IWebTokenRequest3 *iface, HSTRING *value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest3( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->correlation_id, value );
}

static HRESULT WINAPI web_token_request_put_CorrelationId( IWebTokenRequest3 *iface, HSTRING value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest3( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->correlation_id );
    impl->correlation_id = copy;
    return S_OK;
}

static const struct IWebTokenRequest3Vtbl web_token_request3_vtbl =
{
    /* IUnknown methods */
    web_token_request3_QueryInterface,
    web_token_request3_AddRef,
    web_token_request3_Release,
    /* IInspectable methods */
    web_token_request3_GetIids,
    web_token_request3_GetRuntimeClassName,
    web_token_request3_GetTrustLevel,
    /* IWebTokenRequest3 methods */
    web_token_request_get_CorrelationId,
    web_token_request_put_CorrelationId,
};

static HRESULT web_token_request_create_internal( IWebAccountProvider *provider, HSTRING scope, HSTRING client_id,
                                                  WebTokenRequestPromptType prompt_type, IWebTokenRequest **out )
{
    struct web_token_request *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebTokenRequest_iface.lpVtbl = &web_token_request_vtbl;
    impl->IWebTokenRequest2_iface.lpVtbl = &web_token_request2_vtbl;
    impl->IWebTokenRequest3_iface.lpVtbl = &web_token_request3_vtbl;
    impl->ref = 1;
    impl->prompt_type = prompt_type;

    if ((impl->provider = provider)) IWebAccountProvider_AddRef( impl->provider );
    /* the maps exist from the start: no lazy creation that two threads could race */
    if (FAILED(hr = WindowsDuplicateString( scope, &impl->scope )) ||
        FAILED(hr = WindowsDuplicateString( client_id, &impl->client_id )) ||
        FAILED(hr = map_hstring_create( &impl->properties )) ||
        FAILED(hr = map_hstring_create( &impl->app_properties )))
    {
        web_token_request_Release( &impl->IWebTokenRequest_iface );
        return hr;
    }

    *out = &impl->IWebTokenRequest_iface;
    TRACE( "created IWebTokenRequest %p\n", *out );
    return S_OK;
}

struct web_token_request_factory
{
    IActivationFactory IActivationFactory_iface;
    IWebTokenRequestFactory IWebTokenRequestFactory_iface;
    LONG ref;
};

static inline struct web_token_request_factory *impl_from_IActivationFactory_wtr( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct web_token_request_factory, IActivationFactory_iface );
}

static HRESULT WINAPI wtr_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct web_token_request_factory *impl = impl_from_IActivationFactory_wtr( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebTokenRequestFactory ))
    {
        *out = &impl->IWebTokenRequestFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI wtr_factory_AddRef( IActivationFactory *iface )
{
    struct web_token_request_factory *impl = impl_from_IActivationFactory_wtr( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI wtr_factory_Release( IActivationFactory *iface )
{
    struct web_token_request_factory *impl = impl_from_IActivationFactory_wtr( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI wtr_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI wtr_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI wtr_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI wtr_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl wtr_factory_vtbl =
{
    wtr_factory_QueryInterface,
    wtr_factory_AddRef,
    wtr_factory_Release,
    /* IInspectable methods */
    wtr_factory_GetIids,
    wtr_factory_GetRuntimeClassName,
    wtr_factory_GetTrustLevel,
    /* IActivationFactory methods */
    wtr_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( wtr_statics, IWebTokenRequestFactory, struct web_token_request_factory, IActivationFactory_iface )

static HRESULT WINAPI wtr_factory_Create( IWebTokenRequestFactory *iface, IWebAccountProvider *provider, HSTRING scope,
                                          HSTRING clientId, IWebTokenRequest **webTokenRequest )
{
    TRACE( "iface %p, provider %p, scope %s, clientId %s, webTokenRequest %p.\n",
           iface, provider, debugstr_hstring( scope ), debugstr_hstring( clientId ), webTokenRequest );

    if (!webTokenRequest) return E_POINTER;
    return web_token_request_create_internal( provider, scope, clientId, WebTokenRequestPromptType_Default, webTokenRequest );
}

static HRESULT WINAPI wtr_factory_CreateWithPromptType( IWebTokenRequestFactory *iface, IWebAccountProvider *provider,
                                                        HSTRING scope, HSTRING clientId,
                                                        WebTokenRequestPromptType promptType,
                                                        IWebTokenRequest **webTokenRequest )
{
    TRACE( "iface %p, provider %p, scope %s, clientId %s, promptType %u, webTokenRequest %p.\n",
           iface, provider, debugstr_hstring( scope ), debugstr_hstring( clientId ), promptType, webTokenRequest );

    if (!webTokenRequest) return E_POINTER;
    return web_token_request_create_internal( provider, scope, clientId, promptType, webTokenRequest );
}

static HRESULT WINAPI wtr_factory_CreateWithProvider( IWebTokenRequestFactory *iface, IWebAccountProvider *provider,
                                                      IWebTokenRequest **webTokenRequest )
{
    TRACE( "iface %p, provider %p, webTokenRequest %p.\n", iface, provider, webTokenRequest );

    if (!webTokenRequest) return E_POINTER;
    return web_token_request_create_internal( provider, NULL, NULL, WebTokenRequestPromptType_Default, webTokenRequest );
}

static HRESULT WINAPI wtr_factory_CreateWithScope( IWebTokenRequestFactory *iface, IWebAccountProvider *provider,
                                                   HSTRING scope, IWebTokenRequest **webTokenRequest )
{
    TRACE( "iface %p, provider %p, scope %s, webTokenRequest %p.\n", iface, provider, debugstr_hstring( scope ), webTokenRequest );

    if (!webTokenRequest) return E_POINTER;
    return web_token_request_create_internal( provider, scope, NULL, WebTokenRequestPromptType_Default, webTokenRequest );
}

static const struct IWebTokenRequestFactoryVtbl wtr_factory_statics_vtbl =
{
    wtr_statics_QueryInterface,
    wtr_statics_AddRef,
    wtr_statics_Release,
    /* IInspectable methods */
    wtr_statics_GetIids,
    wtr_statics_GetRuntimeClassName,
    wtr_statics_GetTrustLevel,
    /* IWebTokenRequestFactory methods */
    wtr_factory_Create,
    wtr_factory_CreateWithPromptType,
    wtr_factory_CreateWithProvider,
    wtr_factory_CreateWithScope,
};

static struct web_token_request_factory web_token_request_factory_statics =
{
    {&wtr_factory_vtbl},
    {&wtr_factory_statics_vtbl},
    1,
};

IActivationFactory *request_factory = &web_token_request_factory_statics.IActivationFactory_iface;
