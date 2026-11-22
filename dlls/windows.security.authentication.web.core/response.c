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

/*
 *
 * Empty IVectorView<WebTokenResponse>
 *
 */

struct empty_vecview_webtokenresponse
{
    IVectorView_WebTokenResponse IVectorView_WebTokenResponse_iface;
    LONG ref;
    UINT32 count;
    IWebTokenResponse **items;
};

static inline struct empty_vecview_webtokenresponse *impl_from_IVectorView_WebTokenResponse( IVectorView_WebTokenResponse *iface )
{
    return CONTAINING_RECORD( iface, struct empty_vecview_webtokenresponse, IVectorView_WebTokenResponse_iface );
}

static HRESULT WINAPI vecview_wtr_QueryInterface( IVectorView_WebTokenResponse *iface, REFIID iid, void **out )
{
    struct empty_vecview_webtokenresponse *impl = impl_from_IVectorView_WebTokenResponse( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_WebTokenResponse ))
    {
        *out = &impl->IVectorView_WebTokenResponse_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI vecview_wtr_AddRef( IVectorView_WebTokenResponse *iface )
{
    struct empty_vecview_webtokenresponse *impl = impl_from_IVectorView_WebTokenResponse( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI vecview_wtr_Release( IVectorView_WebTokenResponse *iface )
{
    struct empty_vecview_webtokenresponse *impl = impl_from_IVectorView_WebTokenResponse( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        UINT32 i;
        for (i = 0; i < impl->count; i++) IWebTokenResponse_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI vecview_wtr_GetIids( IVectorView_WebTokenResponse *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI vecview_wtr_GetRuntimeClassName( IVectorView_WebTokenResponse *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Authentication.Web.Core.WebTokenResponse>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Authentication.Web.Core.WebTokenResponse>"),
                                class_name );
}

static HRESULT WINAPI vecview_wtr_GetTrustLevel( IVectorView_WebTokenResponse *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI vecview_wtr_GetAt( IVectorView_WebTokenResponse *iface, UINT32 index, IWebTokenResponse **value )
{
    struct empty_vecview_webtokenresponse *impl = impl_from_IVectorView_WebTokenResponse( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IWebTokenResponse_AddRef( (*value = impl->items[index]) );
    return S_OK;
}

static HRESULT WINAPI vecview_wtr_get_Size( IVectorView_WebTokenResponse *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl_from_IVectorView_WebTokenResponse( iface )->count;
    return S_OK;
}

static HRESULT WINAPI vecview_wtr_IndexOf( IVectorView_WebTokenResponse *iface, IWebTokenResponse *element, UINT32 *index, boolean *found )
{
    struct empty_vecview_webtokenresponse *impl = impl_from_IVectorView_WebTokenResponse( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );

    if (!found) return E_POINTER;
    *found = FALSE;
    if (index) *index = 0;
    for (i = 0; i < impl->count; i++)
    {
        if (impl->items[i] != element) continue;
        *found = TRUE;
        if (index) *index = i;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI vecview_wtr_GetMany( IVectorView_WebTokenResponse *iface, UINT32 start_index, UINT32 items_size,
                                           IWebTokenResponse **items, UINT32 *count )
{
    struct empty_vecview_webtokenresponse *impl = impl_from_IVectorView_WebTokenResponse( iface );
    UINT32 i;

    TRACE( "iface %p, start_index %u, items_size %u, items %p, count %p.\n", iface, start_index, items_size, items, count );

    if (!count) return E_POINTER;
    *count = 0;
    if (start_index > impl->count) return E_BOUNDS;
    for (i = start_index; i < impl->count && i - start_index < items_size; i++)
        IWebTokenResponse_AddRef( (items[i - start_index] = impl->items[i]) );
    *count = i - start_index;
    return S_OK;
}

static const struct IVectorView_WebTokenResponseVtbl vecview_wtr_vtbl =
{
    /* IUnknown methods */
    vecview_wtr_QueryInterface,
    vecview_wtr_AddRef,
    vecview_wtr_Release,
    /* IInspectable methods */
    vecview_wtr_GetIids,
    vecview_wtr_GetRuntimeClassName,
    vecview_wtr_GetTrustLevel,
    /* IVectorView methods */
    vecview_wtr_GetAt,
    vecview_wtr_get_Size,
    vecview_wtr_IndexOf,
    vecview_wtr_GetMany,
};

/* a view of count items (each one AddRef'ed), or an empty one */
HRESULT vector_view_webtokenresponse_create( IWebTokenResponse **items, UINT32 count, IVectorView_WebTokenResponse **out )
{
    struct empty_vecview_webtokenresponse *impl;
    UINT32 i;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_WebTokenResponse_iface.lpVtbl = &vecview_wtr_vtbl;
    impl->ref = 1;
    if (count && !(impl->items = calloc( count, sizeof(*impl->items) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++) IWebTokenResponse_AddRef( (impl->items[i] = items[i]) );
    impl->count = count;

    *out = &impl->IVectorView_WebTokenResponse_iface;
    TRACE( "created IVectorView_WebTokenResponse %p\n", *out );
    return S_OK;
}

HRESULT vector_view_webtokenresponse_create_empty( IVectorView_WebTokenResponse **out )
{
    return vector_view_webtokenresponse_create( NULL, 0, out );
}

/*
 *
 * Empty IVectorView<WebAccount>
 *
 */

struct empty_vecview_webaccount
{
    IVectorView_WebAccount IVectorView_WebAccount_iface;
    LONG ref;
    UINT32 count;
    IWebAccount **items;
};

static inline struct empty_vecview_webaccount *impl_from_IVectorView_WebAccount( IVectorView_WebAccount *iface )
{
    return CONTAINING_RECORD( iface, struct empty_vecview_webaccount, IVectorView_WebAccount_iface );
}

static HRESULT WINAPI vecview_wa_QueryInterface( IVectorView_WebAccount *iface, REFIID iid, void **out )
{
    struct empty_vecview_webaccount *impl = impl_from_IVectorView_WebAccount( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_WebAccount ))
    {
        *out = &impl->IVectorView_WebAccount_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI vecview_wa_AddRef( IVectorView_WebAccount *iface )
{
    struct empty_vecview_webaccount *impl = impl_from_IVectorView_WebAccount( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI vecview_wa_Release( IVectorView_WebAccount *iface )
{
    struct empty_vecview_webaccount *impl = impl_from_IVectorView_WebAccount( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        UINT32 i;
        for (i = 0; i < impl->count; i++) IWebAccount_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI vecview_wa_GetIids( IVectorView_WebAccount *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI vecview_wa_GetRuntimeClassName( IVectorView_WebAccount *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Credentials.WebAccount>",
                                ARRAY_SIZE(L"Windows.Foundation.Collections.IVectorView`1<Windows.Security.Credentials.WebAccount>"),
                                class_name );
}

static HRESULT WINAPI vecview_wa_GetTrustLevel( IVectorView_WebAccount *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI vecview_wa_GetAt( IVectorView_WebAccount *iface, UINT32 index, IWebAccount **value )
{
    struct empty_vecview_webaccount *impl = impl_from_IVectorView_WebAccount( iface );

    TRACE( "iface %p, index %u, value %p.\n", iface, index, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IWebAccount_AddRef( (*value = impl->items[index]) );
    return S_OK;
}

static HRESULT WINAPI vecview_wa_get_Size( IVectorView_WebAccount *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl_from_IVectorView_WebAccount( iface )->count;
    return S_OK;
}

static HRESULT WINAPI vecview_wa_IndexOf( IVectorView_WebAccount *iface, IWebAccount *element, UINT32 *index, boolean *found )
{
    struct empty_vecview_webaccount *impl = impl_from_IVectorView_WebAccount( iface );
    UINT32 i;

    TRACE( "iface %p, element %p, index %p, found %p.\n", iface, element, index, found );

    if (!found) return E_POINTER;
    *found = FALSE;
    if (index) *index = 0;
    for (i = 0; i < impl->count; i++)
    {
        if (impl->items[i] != element) continue;
        *found = TRUE;
        if (index) *index = i;
        break;
    }
    return S_OK;
}

static HRESULT WINAPI vecview_wa_GetMany( IVectorView_WebAccount *iface, UINT32 start_index, UINT32 items_size,
                                          IWebAccount **items, UINT32 *count )
{
    struct empty_vecview_webaccount *impl = impl_from_IVectorView_WebAccount( iface );
    UINT32 i;

    TRACE( "iface %p, start_index %u, items_size %u, items %p, count %p.\n", iface, start_index, items_size, items, count );

    if (!count) return E_POINTER;
    *count = 0;
    if (start_index > impl->count) return E_BOUNDS;
    for (i = start_index; i < impl->count && i - start_index < items_size; i++)
        IWebAccount_AddRef( (items[i - start_index] = impl->items[i]) );
    *count = i - start_index;
    return S_OK;
}

static const struct IVectorView_WebAccountVtbl vecview_wa_vtbl =
{
    /* IUnknown methods */
    vecview_wa_QueryInterface,
    vecview_wa_AddRef,
    vecview_wa_Release,
    /* IInspectable methods */
    vecview_wa_GetIids,
    vecview_wa_GetRuntimeClassName,
    vecview_wa_GetTrustLevel,
    /* IVectorView methods */
    vecview_wa_GetAt,
    vecview_wa_get_Size,
    vecview_wa_IndexOf,
    vecview_wa_GetMany,
};

/* a view of count items (each one AddRef'ed), or an empty one */
HRESULT vector_view_webaccount_create( IWebAccount **items, UINT32 count, IVectorView_WebAccount **out )
{
    struct empty_vecview_webaccount *impl;
    UINT32 i;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_WebAccount_iface.lpVtbl = &vecview_wa_vtbl;
    impl->ref = 1;
    if (count && !(impl->items = calloc( count, sizeof(*impl->items) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++) IWebAccount_AddRef( (impl->items[i] = items[i]) );
    impl->count = count;

    *out = &impl->IVectorView_WebAccount_iface;
    TRACE( "created IVectorView_WebAccount %p\n", *out );
    return S_OK;
}

HRESULT vector_view_webaccount_create_empty( IVectorView_WebAccount **out )
{
    return vector_view_webaccount_create( NULL, 0, out );
}

/*
 *
 * WebProviderError
 *
 */

struct web_provider_error
{
    IWebProviderError IWebProviderError_iface;
    LONG ref;
    UINT32 error_code;
    HSTRING error_message;
    IMap_HSTRING_HSTRING *properties;
};

static inline struct web_provider_error *impl_from_IWebProviderError( IWebProviderError *iface )
{
    return CONTAINING_RECORD( iface, struct web_provider_error, IWebProviderError_iface );
}

static HRESULT WINAPI web_provider_error_QueryInterface( IWebProviderError *iface, REFIID iid, void **out )
{
    struct web_provider_error *impl = impl_from_IWebProviderError( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IWebProviderError ))
    {
        *out = &impl->IWebProviderError_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI web_provider_error_AddRef( IWebProviderError *iface )
{
    struct web_provider_error *impl = impl_from_IWebProviderError( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI web_provider_error_Release( IWebProviderError *iface )
{
    struct web_provider_error *impl = impl_from_IWebProviderError( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->properties) IMap_HSTRING_HSTRING_Release( impl->properties );
        if (impl->error_message) WindowsDeleteString( impl->error_message );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI web_provider_error_GetIids( IWebProviderError *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_provider_error_GetRuntimeClassName( IWebProviderError *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Security.Authentication.Web.Core.WebProviderError",
                                ARRAY_SIZE(L"Windows.Security.Authentication.Web.Core.WebProviderError"),
                                class_name );
}

static HRESULT WINAPI web_provider_error_GetTrustLevel( IWebProviderError *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_provider_error_get_ErrorCode( IWebProviderError *iface, UINT32 *value )
{
    struct web_provider_error *impl = impl_from_IWebProviderError( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->error_code;
    return S_OK;
}

static HRESULT WINAPI web_provider_error_get_ErrorMessage( IWebProviderError *iface, HSTRING *value )
{
    struct web_provider_error *impl = impl_from_IWebProviderError( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    if (!impl->error_message) return S_OK;

    {
        UINT32 length;
        const WCHAR *buffer = WindowsGetStringRawBuffer( impl->error_message, &length );
        if (SUCCEEDED(hr = WindowsCreateString( buffer, length, value ))) return S_OK;
    }
    return hr;
}

static HRESULT WINAPI web_provider_error_get_Properties( IWebProviderError *iface, IMap_HSTRING_HSTRING **value )
{
    struct web_provider_error *impl = impl_from_IWebProviderError( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;

    if (!impl->properties && FAILED(hr = map_hstring_create( &impl->properties ))) return hr;
    IMap_HSTRING_HSTRING_AddRef( (*value = impl->properties) );
    return S_OK;
}

static const struct IWebProviderErrorVtbl web_provider_error_vtbl =
{
    /* IUnknown methods */
    web_provider_error_QueryInterface,
    web_provider_error_AddRef,
    web_provider_error_Release,
    /* IInspectable methods */
    web_provider_error_GetIids,
    web_provider_error_GetRuntimeClassName,
    web_provider_error_GetTrustLevel,
    /* IWebProviderError methods */
    web_provider_error_get_ErrorCode,
    web_provider_error_get_ErrorMessage,
    web_provider_error_get_Properties,
};

static HRESULT web_provider_error_create_internal( UINT32 error_code, HSTRING error_message, IWebProviderError **out )
{
    struct web_provider_error *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebProviderError_iface.lpVtbl = &web_provider_error_vtbl;
    impl->ref = 1;
    impl->error_code = error_code;

    if (error_message)
    {
        UINT32 length;
        const WCHAR *buffer = WindowsGetStringRawBuffer( error_message, &length );
        if (FAILED(hr = WindowsCreateString( buffer, length, &impl->error_message )))
        {
            free( impl );
            return hr;
        }
    }

    *out = &impl->IWebProviderError_iface;
    TRACE( "created IWebProviderError %p\n", *out );
    return S_OK;
}

HRESULT webprovidererror_create( UINT32 error_code, HSTRING error_message, IWebProviderError **out )
{
    return web_provider_error_create_internal( error_code, error_message, out );
}

struct web_provider_error_factory
{
    IActivationFactory IActivationFactory_iface;
    IWebProviderErrorFactory IWebProviderErrorFactory_iface;
    LONG ref;
};

static inline struct web_provider_error_factory *impl_from_IActivationFactory_wpe( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct web_provider_error_factory, IActivationFactory_iface );
}

static HRESULT WINAPI wpe_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct web_provider_error_factory *impl = impl_from_IActivationFactory_wpe( iface );

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

    if (IsEqualGUID( iid, &IID_IWebProviderErrorFactory ))
    {
        *out = &impl->IWebProviderErrorFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI wpe_factory_AddRef( IActivationFactory *iface )
{
    struct web_provider_error_factory *impl = impl_from_IActivationFactory_wpe( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI wpe_factory_Release( IActivationFactory *iface )
{
    struct web_provider_error_factory *impl = impl_from_IActivationFactory_wpe( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI wpe_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI wpe_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI wpe_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI wpe_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl wpe_factory_vtbl =
{
    wpe_factory_QueryInterface,
    wpe_factory_AddRef,
    wpe_factory_Release,
    /* IInspectable methods */
    wpe_factory_GetIids,
    wpe_factory_GetRuntimeClassName,
    wpe_factory_GetTrustLevel,
    /* IActivationFactory methods */
    wpe_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( wpe_statics, IWebProviderErrorFactory, struct web_provider_error_factory, IActivationFactory_iface )

static HRESULT WINAPI wpe_factory_Create( IWebProviderErrorFactory *iface, UINT32 error_code, HSTRING error_message,
                                          IWebProviderError **webProviderError )
{
    TRACE( "iface %p, error_code %u, error_message %s, webProviderError %p.\n",
           iface, error_code, debugstr_hstring( error_message ), webProviderError );

    if (!webProviderError) return E_POINTER;
    return web_provider_error_create_internal( error_code, error_message, webProviderError );
}

static const struct IWebProviderErrorFactoryVtbl wpe_factory_statics_vtbl =
{
    wpe_statics_QueryInterface,
    wpe_statics_AddRef,
    wpe_statics_Release,
    /* IInspectable methods */
    wpe_statics_GetIids,
    wpe_statics_GetRuntimeClassName,
    wpe_statics_GetTrustLevel,
    /* IWebProviderErrorFactory methods */
    wpe_factory_Create,
};

static struct web_provider_error_factory web_provider_error_factory_statics =
{
    {&wpe_factory_vtbl},
    {&wpe_factory_statics_vtbl},
    1,
};

IActivationFactory *providererror_factory = &web_provider_error_factory_statics.IActivationFactory_iface;

/*
 *
 * WebTokenResponse
 *
 */

struct web_token_response
{
    IWebTokenResponse IWebTokenResponse_iface;
    LONG ref;
    HSTRING token;
    IWebProviderError *provider_error;
    IWebAccount *web_account;
    IMap_HSTRING_HSTRING *properties;
};

static inline struct web_token_response *impl_from_IWebTokenResponse( IWebTokenResponse *iface )
{
    return CONTAINING_RECORD( iface, struct web_token_response, IWebTokenResponse_iface );
}

static HRESULT WINAPI web_token_response_QueryInterface( IWebTokenResponse *iface, REFIID iid, void **out )
{
    struct web_token_response *impl = impl_from_IWebTokenResponse( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IWebTokenResponse ))
    {
        *out = &impl->IWebTokenResponse_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI web_token_response_AddRef( IWebTokenResponse *iface )
{
    struct web_token_response *impl = impl_from_IWebTokenResponse( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI web_token_response_Release( IWebTokenResponse *iface )
{
    struct web_token_response *impl = impl_from_IWebTokenResponse( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->properties) IMap_HSTRING_HSTRING_Release( impl->properties );
        if (impl->provider_error) IWebProviderError_Release( impl->provider_error );
        if (impl->web_account) IWebAccount_Release( impl->web_account );
        if (impl->token) WindowsDeleteString( impl->token );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI web_token_response_GetIids( IWebTokenResponse *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_token_response_GetRuntimeClassName( IWebTokenResponse *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Security.Authentication.Web.Core.WebTokenResponse",
                                ARRAY_SIZE(L"Windows.Security.Authentication.Web.Core.WebTokenResponse"),
                                class_name );
}

static HRESULT WINAPI web_token_response_GetTrustLevel( IWebTokenResponse *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_token_response_get_Token( IWebTokenResponse *iface, HSTRING *value )
{
    struct web_token_response *impl = impl_from_IWebTokenResponse( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;

    if (!impl->token) return S_OK;

    {
        UINT32 length;
        const WCHAR *buffer = WindowsGetStringRawBuffer( impl->token, &length );
        if (SUCCEEDED(hr = WindowsCreateString( buffer, length, value ))) return S_OK;
    }
    return hr;
}

static HRESULT WINAPI web_token_response_get_ProviderError( IWebTokenResponse *iface, IWebProviderError **value )
{
    struct web_token_response *impl = impl_from_IWebTokenResponse( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->provider_error) IWebProviderError_AddRef( (*value = impl->provider_error) );
    else *value = NULL;
    return S_OK;
}

static HRESULT WINAPI web_token_response_get_WebAccount( IWebTokenResponse *iface, IWebAccount **value )
{
    struct web_token_response *impl = impl_from_IWebTokenResponse( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->web_account) IWebAccount_AddRef( (*value = impl->web_account) );
    else *value = NULL;
    return S_OK;
}

static HRESULT WINAPI web_token_response_get_Properties( IWebTokenResponse *iface, IMap_HSTRING_HSTRING **value )
{
    struct web_token_response *impl = impl_from_IWebTokenResponse( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;

    if (!impl->properties && FAILED(hr = map_hstring_create( &impl->properties ))) return hr;
    IMap_HSTRING_HSTRING_AddRef( (*value = impl->properties) );
    return S_OK;
}

static const struct IWebTokenResponseVtbl web_token_response_vtbl =
{
    /* IUnknown methods */
    web_token_response_QueryInterface,
    web_token_response_AddRef,
    web_token_response_Release,
    /* IInspectable methods */
    web_token_response_GetIids,
    web_token_response_GetRuntimeClassName,
    web_token_response_GetTrustLevel,
    /* IWebTokenResponse methods */
    web_token_response_get_Token,
    web_token_response_get_ProviderError,
    web_token_response_get_WebAccount,
    web_token_response_get_Properties,
};

static HRESULT web_token_response_create_internal( HSTRING token, IWebAccount *web_account, IWebProviderError *error,
                                                   IWebTokenResponse **out );

/* a response of the token broker: its token, account and properties (the map is kept, not copied) */
HRESULT webtokenresponse_create( HSTRING token, IWebAccount *web_account, IMap_HSTRING_HSTRING *properties,
                                 IWebTokenResponse **out )
{
    struct web_token_response *impl;
    HRESULT hr;

    if (FAILED(hr = web_token_response_create_internal( token, web_account, NULL, out ))) return hr;
    impl = impl_from_IWebTokenResponse( *out );
    if ((impl->properties = properties)) IMap_HSTRING_HSTRING_AddRef( properties );
    return S_OK;
}

static HRESULT web_token_response_create_internal( HSTRING token, IWebAccount *web_account, IWebProviderError *error,
                                                   IWebTokenResponse **out )
{
    struct web_token_response *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebTokenResponse_iface.lpVtbl = &web_token_response_vtbl;
    impl->ref = 1;

    if (token)
    {
        UINT32 length;
        const WCHAR *buffer = WindowsGetStringRawBuffer( token, &length );
        if (FAILED(hr = WindowsCreateString( buffer, length, &impl->token )))
        {
            free( impl );
            return hr;
        }
    }
    if ((impl->web_account = web_account)) IWebAccount_AddRef( impl->web_account );
    if ((impl->provider_error = error)) IWebProviderError_AddRef( impl->provider_error );

    *out = &impl->IWebTokenResponse_iface;
    TRACE( "created IWebTokenResponse %p\n", *out );
    return S_OK;
}

struct web_token_response_factory
{
    IActivationFactory IActivationFactory_iface;
    IWebTokenResponseFactory IWebTokenResponseFactory_iface;
    LONG ref;
};

static inline struct web_token_response_factory *impl_from_IActivationFactory_wtr( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct web_token_response_factory, IActivationFactory_iface );
}

static HRESULT WINAPI wtr_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct web_token_response_factory *impl = impl_from_IActivationFactory_wtr( iface );

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

    if (IsEqualGUID( iid, &IID_IWebTokenResponseFactory ))
    {
        *out = &impl->IWebTokenResponseFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI wtr_factory_AddRef( IActivationFactory *iface )
{
    struct web_token_response_factory *impl = impl_from_IActivationFactory_wtr( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI wtr_factory_Release( IActivationFactory *iface )
{
    struct web_token_response_factory *impl = impl_from_IActivationFactory_wtr( iface );
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

DEFINE_IINSPECTABLE( wtrsp_statics, IWebTokenResponseFactory, struct web_token_response_factory, IActivationFactory_iface )

static HRESULT WINAPI wtr_factory_CreateWithToken( IWebTokenResponseFactory *iface, HSTRING token,
                                                   IWebTokenResponse **webTokenResponse )
{
    TRACE( "iface %p, token %s, webTokenResponse %p.\n", iface, debugstr_hstring( token ), webTokenResponse );

    if (!webTokenResponse) return E_POINTER;
    return web_token_response_create_internal( token, NULL, NULL, webTokenResponse );
}

static HRESULT WINAPI wtr_factory_CreateWithTokenAndAccount( IWebTokenResponseFactory *iface, HSTRING token,
                                                             IWebAccount *webAccount, IWebTokenResponse **webTokenResponse )
{
    TRACE( "iface %p, token %s, webAccount %p, webTokenResponse %p.\n",
           iface, debugstr_hstring( token ), webAccount, webTokenResponse );

    if (!webTokenResponse) return E_POINTER;
    return web_token_response_create_internal( token, webAccount, NULL, webTokenResponse );
}

static HRESULT WINAPI wtr_factory_CreateWithTokenAccountAndError( IWebTokenResponseFactory *iface, HSTRING token,
                                                                  IWebAccount *webAccount, IWebProviderError *error,
                                                                  IWebTokenResponse **webTokenResponse )
{
    TRACE( "iface %p, token %s, webAccount %p, error %p, webTokenResponse %p.\n",
           iface, debugstr_hstring( token ), webAccount, error, webTokenResponse );

    if (!webTokenResponse) return E_POINTER;
    return web_token_response_create_internal( token, webAccount, error, webTokenResponse );
}

static const struct IWebTokenResponseFactoryVtbl wtr_factory_statics_vtbl =
{
    wtrsp_statics_QueryInterface,
    wtrsp_statics_AddRef,
    wtrsp_statics_Release,
    /* IInspectable methods */
    wtrsp_statics_GetIids,
    wtrsp_statics_GetRuntimeClassName,
    wtrsp_statics_GetTrustLevel,
    /* IWebTokenResponseFactory methods */
    wtr_factory_CreateWithToken,
    wtr_factory_CreateWithTokenAndAccount,
    wtr_factory_CreateWithTokenAccountAndError,
};

static struct web_token_response_factory web_token_response_factory_statics =
{
    {&wtr_factory_vtbl},
    {&wtr_factory_statics_vtbl},
    1,
};

IActivationFactory *response_factory = &web_token_response_factory_statics.IActivationFactory_iface;

/*
 *
 * WebTokenRequestResult
 *
 */

struct web_token_request_result
{
    IWebTokenRequestResult IWebTokenRequestResult_iface;
    LONG ref;
    IVectorView_WebTokenResponse *response_data;
    WebTokenRequestStatus response_status;
    IWebProviderError *response_error;
};

static inline struct web_token_request_result *impl_from_IWebTokenRequestResult( IWebTokenRequestResult *iface )
{
    return CONTAINING_RECORD( iface, struct web_token_request_result, IWebTokenRequestResult_iface );
}

static HRESULT WINAPI web_token_request_result_QueryInterface( IWebTokenRequestResult *iface, REFIID iid, void **out )
{
    struct web_token_request_result *impl = impl_from_IWebTokenRequestResult( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IWebTokenRequestResult ))
    {
        *out = &impl->IWebTokenRequestResult_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI web_token_request_result_AddRef( IWebTokenRequestResult *iface )
{
    struct web_token_request_result *impl = impl_from_IWebTokenRequestResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI web_token_request_result_Release( IWebTokenRequestResult *iface )
{
    struct web_token_request_result *impl = impl_from_IWebTokenRequestResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->response_data) IVectorView_WebTokenResponse_Release( impl->response_data );
        if (impl->response_error) IWebProviderError_Release( impl->response_error );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI web_token_request_result_GetIids( IWebTokenRequestResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_token_request_result_GetRuntimeClassName( IWebTokenRequestResult *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Security.Authentication.Web.Core.WebTokenRequestResult",
                                ARRAY_SIZE(L"Windows.Security.Authentication.Web.Core.WebTokenRequestResult"),
                                class_name );
}

static HRESULT WINAPI web_token_request_result_GetTrustLevel( IWebTokenRequestResult *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_token_request_result_get_ResponseData( IWebTokenRequestResult *iface,
                                                                 IVectorView_WebTokenResponse **value )
{
    struct web_token_request_result *impl = impl_from_IWebTokenRequestResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->response_data) IVectorView_WebTokenResponse_AddRef( (*value = impl->response_data) );
    else *value = NULL;
    return S_OK;
}

static HRESULT WINAPI web_token_request_result_get_ResponseStatus( IWebTokenRequestResult *iface,
                                                                   WebTokenRequestStatus *value )
{
    struct web_token_request_result *impl = impl_from_IWebTokenRequestResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->response_status;
    return S_OK;
}

static HRESULT WINAPI web_token_request_result_get_ResponseError( IWebTokenRequestResult *iface, IWebProviderError **value )
{
    struct web_token_request_result *impl = impl_from_IWebTokenRequestResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->response_error) IWebProviderError_AddRef( (*value = impl->response_error) );
    else *value = NULL;
    return S_OK;
}

static HRESULT WINAPI web_token_request_result_InvalidateCacheAsync( IWebTokenRequestResult *iface, IAsyncAction **asyncInfo )
{
    FIXME( "iface %p, asyncInfo %p stub!\n", iface, asyncInfo );
    return E_NOTIMPL;
}

static const struct IWebTokenRequestResultVtbl web_token_request_result_vtbl =
{
    /* IUnknown methods */
    web_token_request_result_QueryInterface,
    web_token_request_result_AddRef,
    web_token_request_result_Release,
    /* IInspectable methods */
    web_token_request_result_GetIids,
    web_token_request_result_GetRuntimeClassName,
    web_token_request_result_GetTrustLevel,
    /* IWebTokenRequestResult methods */
    web_token_request_result_get_ResponseData,
    web_token_request_result_get_ResponseStatus,
    web_token_request_result_get_ResponseError,
    web_token_request_result_InvalidateCacheAsync,
};

HRESULT webtokenrequestresult_create( WebTokenRequestStatus status, IVectorView_WebTokenResponse *response_data,
                                      IWebProviderError *response_error, IWebTokenRequestResult **out )
{
    struct web_token_request_result *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebTokenRequestResult_iface.lpVtbl = &web_token_request_result_vtbl;
    impl->ref = 1;
    impl->response_status = status;
    if ((impl->response_data = response_data)) IVectorView_WebTokenResponse_AddRef( impl->response_data );
    if ((impl->response_error = response_error)) IWebProviderError_AddRef( impl->response_error );

    *out = &impl->IWebTokenRequestResult_iface;
    TRACE( "created IWebTokenRequestResult %p\n", *out );
    return S_OK;
}

/*
 *
 * FindAllAccountsResult
 *
 */

struct find_all_accounts_result
{
    IFindAllAccountsResult IFindAllAccountsResult_iface;
    LONG ref;
    IVectorView_WebAccount *accounts;
    FindAllWebAccountsStatus status;
    IWebProviderError *provider_error;
};

static inline struct find_all_accounts_result *impl_from_IFindAllAccountsResult( IFindAllAccountsResult *iface )
{
    return CONTAINING_RECORD( iface, struct find_all_accounts_result, IFindAllAccountsResult_iface );
}

static HRESULT WINAPI find_all_accounts_result_QueryInterface( IFindAllAccountsResult *iface, REFIID iid, void **out )
{
    struct find_all_accounts_result *impl = impl_from_IFindAllAccountsResult( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IFindAllAccountsResult ))
    {
        *out = &impl->IFindAllAccountsResult_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI find_all_accounts_result_AddRef( IFindAllAccountsResult *iface )
{
    struct find_all_accounts_result *impl = impl_from_IFindAllAccountsResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI find_all_accounts_result_Release( IFindAllAccountsResult *iface )
{
    struct find_all_accounts_result *impl = impl_from_IFindAllAccountsResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->accounts) IVectorView_WebAccount_Release( impl->accounts );
        if (impl->provider_error) IWebProviderError_Release( impl->provider_error );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI find_all_accounts_result_GetIids( IFindAllAccountsResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI find_all_accounts_result_GetRuntimeClassName( IFindAllAccountsResult *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Security.Authentication.Web.Core.FindAllAccountsResult",
                                ARRAY_SIZE(L"Windows.Security.Authentication.Web.Core.FindAllAccountsResult"),
                                class_name );
}

static HRESULT WINAPI find_all_accounts_result_GetTrustLevel( IFindAllAccountsResult *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI find_all_accounts_result_get_Accounts( IFindAllAccountsResult *iface, IVectorView_WebAccount **value )
{
    struct find_all_accounts_result *impl = impl_from_IFindAllAccountsResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->accounts) IVectorView_WebAccount_AddRef( (*value = impl->accounts) );
    else *value = NULL;
    return S_OK;
}

static HRESULT WINAPI find_all_accounts_result_get_Status( IFindAllAccountsResult *iface, FindAllWebAccountsStatus *value )
{
    struct find_all_accounts_result *impl = impl_from_IFindAllAccountsResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI find_all_accounts_result_get_ProviderError( IFindAllAccountsResult *iface, IWebProviderError **value )
{
    struct find_all_accounts_result *impl = impl_from_IFindAllAccountsResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->provider_error) IWebProviderError_AddRef( (*value = impl->provider_error) );
    else *value = NULL;
    return S_OK;
}

static const struct IFindAllAccountsResultVtbl find_all_accounts_result_vtbl =
{
    /* IUnknown methods */
    find_all_accounts_result_QueryInterface,
    find_all_accounts_result_AddRef,
    find_all_accounts_result_Release,
    /* IInspectable methods */
    find_all_accounts_result_GetIids,
    find_all_accounts_result_GetRuntimeClassName,
    find_all_accounts_result_GetTrustLevel,
    /* IFindAllAccountsResult methods */
    find_all_accounts_result_get_Accounts,
    find_all_accounts_result_get_Status,
    find_all_accounts_result_get_ProviderError,
};

HRESULT findallaccountsresult_create( FindAllWebAccountsStatus status, IVectorView_WebAccount *accounts,
                                      IWebProviderError *provider_error, IFindAllAccountsResult **out )
{
    struct find_all_accounts_result *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IFindAllAccountsResult_iface.lpVtbl = &find_all_accounts_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    if ((impl->accounts = accounts)) IVectorView_WebAccount_AddRef( impl->accounts );
    if ((impl->provider_error = provider_error)) IWebProviderError_AddRef( impl->provider_error );

    *out = &impl->IFindAllAccountsResult_iface;
    TRACE( "created IFindAllAccountsResult %p\n", *out );
    return S_OK;
}
