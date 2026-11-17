/* WinRT Windows.Web.Http.HttpClient Implementation
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
 * The object exists and can be closed, so that an application that makes one (the Calculator keeps one in a static
 * field and uses canned data) runs. The requests are not implemented: they fail with E_NOTIMPL, and so do the
 * default request headers.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct http_client
{
    struct simple_object base;
    IClosable IClosable_iface;
};

static inline struct http_client *impl_from_IClosable( IClosable *iface )
{
    return CONTAINING_RECORD( iface, struct http_client, IClosable_iface );
}

DEFINE_SIMPLE_IINSPECTABLE( client_base, IHttpClient )

static HRESULT WINAPI client_QueryInterface( IHttpClient *iface, REFIID iid, void **out )
{
    struct http_client *impl = (struct http_client *)iface;

    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IClosable_AddRef( (*out = &impl->IClosable_iface) );
        return S_OK;
    }
    return client_base_QueryInterface( iface, iid, out );
}

#define NOT_IMPLEMENTED( name ) \
    do { FIXME( "iface %p: " #name " is not implemented.\n", iface ); *operation = NULL; return E_NOTIMPL; } while (0)

static HRESULT WINAPI client_DeleteAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{
    NOT_IMPLEMENTED( DeleteAsync );
}

static HRESULT WINAPI client_GetAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{
    NOT_IMPLEMENTED( GetAsync );
}

static HRESULT WINAPI client_GetWithOptionAsync( IHttpClient *iface, IUriRuntimeClass *uri, INT32 option, IInspectable **operation )
{
    NOT_IMPLEMENTED( GetWithOptionAsync );
}

static HRESULT WINAPI client_GetBufferAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{
    NOT_IMPLEMENTED( GetBufferAsync );
}

static HRESULT WINAPI client_GetInputStreamAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{
    NOT_IMPLEMENTED( GetInputStreamAsync );
}

static HRESULT WINAPI client_GetStringAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{
    NOT_IMPLEMENTED( GetStringAsync );
}

static HRESULT WINAPI client_PostAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable *content, IInspectable **operation )
{
    NOT_IMPLEMENTED( PostAsync );
}

static HRESULT WINAPI client_PutAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable *content, IInspectable **operation )
{
    NOT_IMPLEMENTED( PutAsync );
}

static HRESULT WINAPI client_SendRequestAsync( IHttpClient *iface, IInspectable *request, IInspectable **operation )
{
    NOT_IMPLEMENTED( SendRequestAsync );
}

static HRESULT WINAPI client_SendRequestWithOptionAsync( IHttpClient *iface, IInspectable *request, INT32 option,
                                                         IInspectable **operation )
{
    NOT_IMPLEMENTED( SendRequestWithOptionAsync );
}

static HRESULT WINAPI client_get_DefaultRequestHeaders( IHttpClient *iface, IInspectable **operation )
{
    NOT_IMPLEMENTED( DefaultRequestHeaders );
}

static const IHttpClientVtbl client_vtbl =
{
    client_QueryInterface,
    client_base_AddRef,
    client_base_Release,
    /* IInspectable methods */
    client_base_GetIids,
    client_base_GetRuntimeClassName,
    client_base_GetTrustLevel,
    /* IHttpClient methods */
    client_DeleteAsync,
    client_GetAsync,
    client_GetWithOptionAsync,
    client_GetBufferAsync,
    client_GetInputStreamAsync,
    client_GetStringAsync,
    client_PostAsync,
    client_PutAsync,
    client_SendRequestAsync,
    client_SendRequestWithOptionAsync,
    client_get_DefaultRequestHeaders,
};

static HRESULT WINAPI closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{
    struct http_client *impl = impl_from_IClosable( iface );

    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IClosable_AddRef( (*out = &impl->IClosable_iface) );
        return S_OK;
    }
    return simple_object_QueryInterface( (IInspectable *)impl, iid, out );
}

static ULONG WINAPI closable_AddRef( IClosable *iface )
{
    return simple_object_AddRef( (IInspectable *)impl_from_IClosable( iface ) );
}

static ULONG WINAPI closable_Release( IClosable *iface )
{
    return simple_object_Release( (IInspectable *)impl_from_IClosable( iface ) );
}

static HRESULT WINAPI closable_GetIids( IClosable *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IHttpClient;
    list[1] = IID_IClosable;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI closable_GetRuntimeClassName( IClosable *iface, HSTRING *name )
{
    return create_name( L"Windows.Web.Http.HttpClient", name );
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

static const IClosableVtbl closable_vtbl =
{
    closable_QueryInterface,
    closable_AddRef,
    closable_Release,
    /* IInspectable methods */
    closable_GetIids,
    closable_GetRuntimeClassName,
    closable_GetTrustLevel,
    /* IClosable methods */
    closable_Close,
};

static HRESULT client_create( IInspectable **out )
{
    struct simple_object *object;
    struct http_client *impl;
    HRESULT hr;

    if (FAILED(hr = simple_object_create( sizeof(*impl), &client_vtbl, &IID_IHttpClient, L"Windows.Web.Http.HttpClient",
                                          NULL, &object ))) return hr;
    impl = (struct http_client *)object;
    impl->IClosable_iface.lpVtbl = &closable_vtbl;
    *out = (IInspectable *)object;
    return S_OK;
}

static HRESULT WINAPI factory_Create( IHttpClientFactory *iface, IInspectable *filter, IHttpClient **value )
{
    TRACE( "iface %p, filter %p, value %p.\n", iface, filter, value );
    if (filter) FIXME( "the filter is ignored.\n" );
    return client_create( (IInspectable **)value );
}

DEFINE_STATICS_IINSPECTABLE( factory, IHttpClientFactory )

static const IHttpClientFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IHttpClientFactory methods */
    factory_Create,
};

static struct static_factory http_client_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.Web.Http.HttpClient", client_create, 1,
    {{ &IID_IHttpClientFactory, { &factory_vtbl, &http_client_static_factory } }},
};

IActivationFactory *http_client_factory = &http_client_static_factory.IActivationFactory_iface;
