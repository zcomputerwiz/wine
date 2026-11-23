/* WinRT Windows.Security.Credentials.WebAccount, as the token broker's answers carry it
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

struct web_account
{
    IWebAccount IWebAccount_iface;
    IWebAccount2 IWebAccount2_iface;
    LONG ref;
    IWebAccountProvider *provider;
    HSTRING id;
    HSTRING user_name;
    WebAccountState state;
    IMap_HSTRING_HSTRING *properties;
};

static inline struct web_account *impl_from_IWebAccount( IWebAccount *iface )
{
    return CONTAINING_RECORD( iface, struct web_account, IWebAccount_iface );
}

static HRESULT WINAPI web_account_QueryInterface( IWebAccount *iface, REFIID iid, void **out )
{
    struct web_account *impl = impl_from_IWebAccount( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IWebAccount ))
    {
        IInspectable_AddRef( (*out = &impl->IWebAccount_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IWebAccount2 ))
    {
        IInspectable_AddRef( (*out = &impl->IWebAccount2_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI web_account_AddRef( IWebAccount *iface )
{
    struct web_account *impl = impl_from_IWebAccount( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI web_account_Release( IWebAccount *iface )
{
    struct web_account *impl = impl_from_IWebAccount( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->provider) IWebAccountProvider_Release( impl->provider );
        if (impl->properties) IMap_HSTRING_HSTRING_Release( impl->properties );
        WindowsDeleteString( impl->id );
        WindowsDeleteString( impl->user_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI web_account_GetIids( IWebAccount *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_account_GetRuntimeClassName( IWebAccount *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Security.Credentials.WebAccount";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI web_account_GetTrustLevel( IWebAccount *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_account_get_WebAccountProvider( IWebAccount *iface, IWebAccountProvider **value )
{
    struct web_account *impl = impl_from_IWebAccount( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if ((*value = impl->provider)) IWebAccountProvider_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI web_account_get_UserName( IWebAccount *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IWebAccount( iface )->user_name, value );
}

static HRESULT WINAPI web_account_get_State( IWebAccount *iface, WebAccountState *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl_from_IWebAccount( iface )->state;
    return S_OK;
}

static const struct IWebAccountVtbl web_account_vtbl =
{
    web_account_QueryInterface,
    web_account_AddRef,
    web_account_Release,
    /* IInspectable methods */
    web_account_GetIids,
    web_account_GetRuntimeClassName,
    web_account_GetTrustLevel,
    /* IWebAccount methods */
    web_account_get_WebAccountProvider,
    web_account_get_UserName,
    web_account_get_State,
};

DEFINE_IINSPECTABLE( web_account2, IWebAccount2, struct web_account, IWebAccount_iface )

static HRESULT WINAPI web_account2_get_Id( IWebAccount2 *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IWebAccount2( iface )->id, value );
}

static HRESULT WINAPI web_account2_get_Properties( IWebAccount2 *iface, IMapView_HSTRING_HSTRING **value )
{
    struct web_account *impl = impl_from_IWebAccount2( iface );
    IMap_HSTRING_HSTRING *map;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = map_hstring_get( &impl->properties, &map ))) return hr;
    hr = IMap_HSTRING_HSTRING_GetView( map, value );
    IMap_HSTRING_HSTRING_Release( map );
    return hr;
}

static HRESULT WINAPI web_account2_GetPictureAsync( IWebAccount2 *iface, WebAccountPictureSize size,
                                                    IAsyncOperation_IRandomAccessStream **operation )
{
    FIXME( "iface %p, size %d, operation %p stub!\n", iface, size, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI web_account2_SignOutAsync( IWebAccount2 *iface, IAsyncAction **operation )
{
    struct web_account *impl = impl_from_IWebAccount2( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    return sign_out_async( impl->provider, impl->id, NULL, operation );
}

static HRESULT WINAPI web_account2_SignOutWithClientIdAsync( IWebAccount2 *iface, HSTRING client_id,
                                                             IAsyncAction **operation )
{
    struct web_account *impl = impl_from_IWebAccount2( iface );

    TRACE( "iface %p, client_id %s, operation %p.\n", iface, debugstr_hstring( client_id ), operation );

    if (!operation) return E_POINTER;
    return sign_out_async( impl->provider, impl->id, client_id, operation );
}

static const struct IWebAccount2Vtbl web_account2_vtbl =
{
    web_account2_QueryInterface,
    web_account2_AddRef,
    web_account2_Release,
    /* IInspectable methods */
    web_account2_GetIids,
    web_account2_GetRuntimeClassName,
    web_account2_GetTrustLevel,
    /* IWebAccount2 methods */
    web_account2_get_Id,
    web_account2_get_Properties,
    web_account2_GetPictureAsync,
    web_account2_SignOutAsync,
    web_account2_SignOutWithClientIdAsync,
};

HRESULT web_account_create( IWebAccountProvider *provider, const WCHAR *id, UINT32 id_len, const WCHAR *user_name,
                            UINT32 user_name_len, WebAccountState state, IWebAccount **out )
{
    struct web_account *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebAccount_iface.lpVtbl = &web_account_vtbl;
    impl->IWebAccount2_iface.lpVtbl = &web_account2_vtbl;
    impl->ref = 1;
    impl->state = state;
    if (FAILED(hr = WindowsCreateString( id, id_len, &impl->id )) ||
        FAILED(hr = WindowsCreateString( user_name, user_name_len, &impl->user_name )))
    {
        web_account_Release( &impl->IWebAccount_iface );
        return hr;
    }
    if ((impl->provider = provider)) IWebAccountProvider_AddRef( provider );
    *out = &impl->IWebAccount_iface;
    TRACE( "created IWebAccount %p\n", *out );
    return S_OK;
}

/* the account's Properties (the account keeps a reference to PROPERTIES) */
void web_account_set_properties( IWebAccount *account, IMap_HSTRING_HSTRING *properties )
{
    struct web_account *impl = impl_from_IWebAccount( account );

    if (impl->properties) IMap_HSTRING_HSTRING_Release( impl->properties );
    if ((impl->properties = properties)) IMap_HSTRING_HSTRING_AddRef( properties );
}

/* the Id of any WebAccount (ours, or a program's own that has IWebAccount2), or NULL */
HSTRING web_account_id( IInspectable *account )
{
    IWebAccount2 *account2;
    HSTRING id = NULL;

    if (account && SUCCEEDED(IInspectable_QueryInterface( account, &IID_IWebAccount2, (void **)&account2 )))
    {
        if (FAILED(IWebAccount2_get_Id( account2, &id ))) id = NULL;
        IWebAccount2_Release( account2 );
    }
    return id;
}
