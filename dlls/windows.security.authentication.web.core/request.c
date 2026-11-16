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

static HRESULT hstring_copy( HSTRING source, HSTRING *dest )
{
    UINT32 length;
    const WCHAR *buffer = WindowsGetStringRawBuffer( source, &length );
    return WindowsCreateString( buffer, length, dest );
}

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

static inline struct web_token_request *impl_from_IWebTokenRequest2( IWebTokenRequest2 *iface )
{
    return CONTAINING_RECORD( iface, struct web_token_request, IWebTokenRequest2_iface );
}

static inline struct web_token_request *impl_from_IWebTokenRequest3( IWebTokenRequest3 *iface )
{
    return CONTAINING_RECORD( iface, struct web_token_request, IWebTokenRequest3_iface );
}

static HRESULT web_token_request_QueryInterface( struct web_token_request *impl, REFIID iid, void **out )
{
    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IWebTokenRequest ))
    {
        *out = &impl->IWebTokenRequest_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebTokenRequest2 ))
    {
        *out = &impl->IWebTokenRequest2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebTokenRequest3 ))
    {
        *out = &impl->IWebTokenRequest3_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG web_token_request_AddRef( struct web_token_request *impl )
{
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "impl %p, ref %lu.\n", impl, ref );
    return ref;
}

static ULONG web_token_request_Release( struct web_token_request *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "impl %p, ref %lu.\n", impl, ref );

    if (!ref)
    {
        if (impl->app_properties) IMap_HSTRING_HSTRING_Release( impl->app_properties );
        if (impl->properties) IMap_HSTRING_HSTRING_Release( impl->properties );
        if (impl->correlation_id) WindowsDeleteString( impl->correlation_id );
        if (impl->client_id) WindowsDeleteString( impl->client_id );
        if (impl->scope) WindowsDeleteString( impl->scope );
        if (impl->provider) IWebAccountProvider_Release( impl->provider );
        free( impl );
    }

    return ref;
}

static HRESULT web_token_request_GetIids( struct web_token_request *impl, ULONG *iid_count, IID **iids )
{
    FIXME( "impl %p, iid_count %p, iids %p stub!\n", impl, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT web_token_request_GetRuntimeClassName( struct web_token_request *impl, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Security.Authentication.Web.Core.WebTokenRequest",
                                ARRAY_SIZE(L"Windows.Security.Authentication.Web.Core.WebTokenRequest"),
                                class_name );
}

static HRESULT web_token_request_GetTrustLevel( struct web_token_request *impl, TrustLevel *trust_level )
{
    FIXME( "impl %p, trust_level %p stub!\n", impl, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_token_request_IWebTokenRequest_QueryInterface( IWebTokenRequest *iface, REFIID iid, void **out )
{
    return web_token_request_QueryInterface( impl_from_IWebTokenRequest( iface ), iid, out );
}

static ULONG WINAPI web_token_request_IWebTokenRequest_AddRef( IWebTokenRequest *iface )
{
    return web_token_request_AddRef( impl_from_IWebTokenRequest( iface ) );
}

static ULONG WINAPI web_token_request_IWebTokenRequest_Release( IWebTokenRequest *iface )
{
    return web_token_request_Release( impl_from_IWebTokenRequest( iface ) );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest_GetIids( IWebTokenRequest *iface, ULONG *iid_count, IID **iids )
{
    return web_token_request_GetIids( impl_from_IWebTokenRequest( iface ), iid_count, iids );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest_GetRuntimeClassName( IWebTokenRequest *iface, HSTRING *class_name )
{
    return web_token_request_GetRuntimeClassName( impl_from_IWebTokenRequest( iface ), class_name );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest_GetTrustLevel( IWebTokenRequest *iface, TrustLevel *trust_level )
{
    return web_token_request_GetTrustLevel( impl_from_IWebTokenRequest( iface ), trust_level );
}

static HRESULT WINAPI web_token_request_get_WebAccountProvider( IWebTokenRequest *iface, IWebAccountProvider **value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->provider) IWebAccountProvider_AddRef( (*value = impl->provider) );
    else *value = NULL;
    return S_OK;
}

static HRESULT WINAPI web_token_request_get_Scope( IWebTokenRequest *iface, HSTRING *value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    if (!impl->scope) return S_OK;
    return hstring_copy( impl->scope, value );
}

static HRESULT WINAPI web_token_request_get_ClientId( IWebTokenRequest *iface, HSTRING *value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    if (!impl->client_id) return S_OK;
    return hstring_copy( impl->client_id, value );
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
    HRESULT hr;

    TRACE( "iface %p, requestProperties %p.\n", iface, requestProperties );

    if (!requestProperties) return E_POINTER;

    if (!impl->properties && FAILED(hr = map_hstring_create( &impl->properties ))) return hr;
    IMap_HSTRING_HSTRING_AddRef( (*requestProperties = impl->properties) );
    return S_OK;
}

static const struct IWebTokenRequestVtbl web_token_request_vtbl =
{
    /* IUnknown methods */
    web_token_request_IWebTokenRequest_QueryInterface,
    web_token_request_IWebTokenRequest_AddRef,
    web_token_request_IWebTokenRequest_Release,
    /* IInspectable methods */
    web_token_request_IWebTokenRequest_GetIids,
    web_token_request_IWebTokenRequest_GetRuntimeClassName,
    web_token_request_IWebTokenRequest_GetTrustLevel,
    /* IWebTokenRequest methods */
    web_token_request_get_WebAccountProvider,
    web_token_request_get_Scope,
    web_token_request_get_ClientId,
    web_token_request_get_PromptType,
    web_token_request_get_Properties,
};

static HRESULT WINAPI web_token_request_IWebTokenRequest2_QueryInterface( IWebTokenRequest2 *iface, REFIID iid, void **out )
{
    return web_token_request_QueryInterface( impl_from_IWebTokenRequest2( iface ), iid, out );
}

static ULONG WINAPI web_token_request_IWebTokenRequest2_AddRef( IWebTokenRequest2 *iface )
{
    return web_token_request_AddRef( impl_from_IWebTokenRequest2( iface ) );
}

static ULONG WINAPI web_token_request_IWebTokenRequest2_Release( IWebTokenRequest2 *iface )
{
    return web_token_request_Release( impl_from_IWebTokenRequest2( iface ) );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest2_GetIids( IWebTokenRequest2 *iface, ULONG *iid_count, IID **iids )
{
    return web_token_request_GetIids( impl_from_IWebTokenRequest2( iface ), iid_count, iids );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest2_GetRuntimeClassName( IWebTokenRequest2 *iface, HSTRING *class_name )
{
    return web_token_request_GetRuntimeClassName( impl_from_IWebTokenRequest2( iface ), class_name );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest2_GetTrustLevel( IWebTokenRequest2 *iface, TrustLevel *trust_level )
{
    return web_token_request_GetTrustLevel( impl_from_IWebTokenRequest2( iface ), trust_level );
}

static HRESULT WINAPI web_token_request_get_AppProperties( IWebTokenRequest2 *iface, IMap_HSTRING_HSTRING **requestProperties )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest2( iface );
    HRESULT hr;

    TRACE( "iface %p, requestProperties %p.\n", iface, requestProperties );

    if (!requestProperties) return E_POINTER;

    if (!impl->app_properties && FAILED(hr = map_hstring_create( &impl->app_properties ))) return hr;
    IMap_HSTRING_HSTRING_AddRef( (*requestProperties = impl->app_properties) );
    return S_OK;
}

static const struct IWebTokenRequest2Vtbl web_token_request2_vtbl =
{
    /* IUnknown methods */
    web_token_request_IWebTokenRequest2_QueryInterface,
    web_token_request_IWebTokenRequest2_AddRef,
    web_token_request_IWebTokenRequest2_Release,
    /* IInspectable methods */
    web_token_request_IWebTokenRequest2_GetIids,
    web_token_request_IWebTokenRequest2_GetRuntimeClassName,
    web_token_request_IWebTokenRequest2_GetTrustLevel,
    /* IWebTokenRequest2 methods */
    web_token_request_get_AppProperties,
};

static HRESULT WINAPI web_token_request_IWebTokenRequest3_QueryInterface( IWebTokenRequest3 *iface, REFIID iid, void **out )
{
    return web_token_request_QueryInterface( impl_from_IWebTokenRequest3( iface ), iid, out );
}

static ULONG WINAPI web_token_request_IWebTokenRequest3_AddRef( IWebTokenRequest3 *iface )
{
    return web_token_request_AddRef( impl_from_IWebTokenRequest3( iface ) );
}

static ULONG WINAPI web_token_request_IWebTokenRequest3_Release( IWebTokenRequest3 *iface )
{
    return web_token_request_Release( impl_from_IWebTokenRequest3( iface ) );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest3_GetIids( IWebTokenRequest3 *iface, ULONG *iid_count, IID **iids )
{
    return web_token_request_GetIids( impl_from_IWebTokenRequest3( iface ), iid_count, iids );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest3_GetRuntimeClassName( IWebTokenRequest3 *iface, HSTRING *class_name )
{
    return web_token_request_GetRuntimeClassName( impl_from_IWebTokenRequest3( iface ), class_name );
}

static HRESULT WINAPI web_token_request_IWebTokenRequest3_GetTrustLevel( IWebTokenRequest3 *iface, TrustLevel *trust_level )
{
    return web_token_request_GetTrustLevel( impl_from_IWebTokenRequest3( iface ), trust_level );
}

static HRESULT WINAPI web_token_request_get_CorrelationId( IWebTokenRequest3 *iface, HSTRING *value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest3( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    if (!impl->correlation_id) return S_OK;
    return hstring_copy( impl->correlation_id, value );
}

static HRESULT WINAPI web_token_request_put_CorrelationId( IWebTokenRequest3 *iface, HSTRING value )
{
    struct web_token_request *impl = impl_from_IWebTokenRequest3( iface );
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (impl->correlation_id)
    {
        WindowsDeleteString( impl->correlation_id );
        impl->correlation_id = NULL;
    }

    if (value)
    {
        if (FAILED(hr = hstring_copy( value, &impl->correlation_id ))) return hr;
    }
    return S_OK;
}

static const struct IWebTokenRequest3Vtbl web_token_request3_vtbl =
{
    /* IUnknown methods */
    web_token_request_IWebTokenRequest3_QueryInterface,
    web_token_request_IWebTokenRequest3_AddRef,
    web_token_request_IWebTokenRequest3_Release,
    /* IInspectable methods */
    web_token_request_IWebTokenRequest3_GetIids,
    web_token_request_IWebTokenRequest3_GetRuntimeClassName,
    web_token_request_IWebTokenRequest3_GetTrustLevel,
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
    if (scope && FAILED(hr = hstring_copy( scope, &impl->scope ))) goto fail;
    if (client_id && FAILED(hr = hstring_copy( client_id, &impl->client_id ))) goto fail;

    *out = &impl->IWebTokenRequest_iface;
    TRACE( "created IWebTokenRequest %p\n", *out );
    return S_OK;

fail:
    web_token_request_Release( impl );
    return hr;
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
