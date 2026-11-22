/* WinRT Windows.Security.Authentication.Web.Core: the diagnostic account provider
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

/* Wine has no account provider. With HKCU\Software\Wine\WebAuthentication "DiagnosticProvider" (REG_DWORD 1),
 * FindAccountProvider* answers with a provider object for the id and authority asked for, and a token request
 * that names it is written to the log (client id, scope, prompt type, provider, properties) and finishes with
 * ProviderError; a silent request finishes with UserInteractionRequired and the error Windows gives for it. It shows
 * what a program asks a broker for; it never gives a token. Off by default: programs that
 * fall back to their own browser sign-in when there is no provider would otherwise stop at the error. */

#include "private.h"
#include "winreg.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(webcore);

/* read at each call: these calls are rare, and the switch can then be turned on and off without a restart */
BOOL diagnostic_provider_enabled( void )
{
    static LONG reported;
    DWORD value = 0, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication", L"DiagnosticProvider",
                      RRF_RT_REG_DWORD, NULL, &value, &size ) || value != 1)
        return FALSE;
    if (!InterlockedExchange( &reported, 1 ))
        FIXME( "the diagnostic account provider is on: token requests are logged and fail.\n" );
    return TRUE;
}

/* the error of a silent request that needs the user, as Windows 11's Microsoft account provider gives it (checked
 * with a client id that is not registered): 0x80860006 "Error" for the consumers authority, 0xcaa10001 "Need user
 * interaction to continue." for organizations. Programs read it without a check for NULL. */
void diagnostic_interaction_error( IInspectable *request, UINT32 *code, const WCHAR **message )
{
    IWebAccountProvider *provider = NULL;
    IWebAccountProvider2 *provider2;
    IWebTokenRequest *token_request;
    HSTRING authority = NULL;
    INT32 cmp = 1;

    if (request && SUCCEEDED(IInspectable_QueryInterface( request, &IID_IWebTokenRequest, (void **)&token_request )))
    {
        if (SUCCEEDED(IWebTokenRequest_get_WebAccountProvider( token_request, &provider )) && provider)
        {
            if (SUCCEEDED(IWebAccountProvider_QueryInterface( provider, &IID_IWebAccountProvider2, (void **)&provider2 )))
            {
                if (SUCCEEDED(IWebAccountProvider2_get_Authority( provider2, &authority )))
                {
                    HSTRING consumers;
                    if (SUCCEEDED(WindowsCreateString( L"consumers", 9, &consumers )))
                    {
                        WindowsCompareStringOrdinal( authority, consumers, &cmp );
                        WindowsDeleteString( consumers );
                    }
                    WindowsDeleteString( authority );
                }
                IWebAccountProvider2_Release( provider2 );
            }
            IWebAccountProvider_Release( provider );
        }
        IWebTokenRequest_Release( token_request );
    }
    if (!cmp)
    {
        *code = 0x80860006;
        *message = L"Error";
    }
    else
    {
        *code = 0xcaa10001;
        *message = L"Need user interaction to continue.";
    }
}

struct provider
{
    IWebAccountProvider IWebAccountProvider_iface;
    IWebAccountProvider2 IWebAccountProvider2_iface;
    LONG ref;
    HSTRING id;
    HSTRING authority;
    HSTRING display_name;
};

static inline struct provider *impl_from_IWebAccountProvider( IWebAccountProvider *iface )
{
    return CONTAINING_RECORD( iface, struct provider, IWebAccountProvider_iface );
}

static inline struct provider *impl_from_IWebAccountProvider2( IWebAccountProvider2 *iface )
{
    return CONTAINING_RECORD( iface, struct provider, IWebAccountProvider2_iface );
}

static HRESULT WINAPI provider_QueryInterface( IWebAccountProvider *iface, REFIID iid, void **out )
{
    struct provider *impl = impl_from_IWebAccountProvider( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IWebAccountProvider ))
    {
        IInspectable_AddRef( (*out = &impl->IWebAccountProvider_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IWebAccountProvider2 ))
    {
        IInspectable_AddRef( (*out = &impl->IWebAccountProvider2_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI provider_AddRef( IWebAccountProvider *iface )
{
    struct provider *impl = impl_from_IWebAccountProvider( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI provider_Release( IWebAccountProvider *iface )
{
    struct provider *impl = impl_from_IWebAccountProvider( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        WindowsDeleteString( impl->id );
        WindowsDeleteString( impl->authority );
        WindowsDeleteString( impl->display_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI provider_GetIids( IWebAccountProvider *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI provider_GetRuntimeClassName( IWebAccountProvider *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_Security_Credentials_WebAccountProvider,
                                wcslen( RuntimeClass_Windows_Security_Credentials_WebAccountProvider ), class_name );
}

static HRESULT WINAPI provider_GetTrustLevel( IWebAccountProvider *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI provider_get_Id( IWebAccountProvider *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IWebAccountProvider( iface )->id, value );
}

static HRESULT WINAPI provider_get_DisplayName( IWebAccountProvider *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IWebAccountProvider( iface )->display_name, value );
}

static HRESULT WINAPI provider_get_IconUri( IWebAccountProvider *iface, IUriRuntimeClass **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static const struct IWebAccountProviderVtbl provider_vtbl =
{
    provider_QueryInterface,
    provider_AddRef,
    provider_Release,
    provider_GetIids,
    provider_GetRuntimeClassName,
    provider_GetTrustLevel,
    provider_get_Id,
    provider_get_DisplayName,
    provider_get_IconUri,
};

static HRESULT WINAPI provider2_QueryInterface( IWebAccountProvider2 *iface, REFIID iid, void **out )
{
    return provider_QueryInterface( &impl_from_IWebAccountProvider2( iface )->IWebAccountProvider_iface, iid, out );
}

static ULONG WINAPI provider2_AddRef( IWebAccountProvider2 *iface )
{
    return provider_AddRef( &impl_from_IWebAccountProvider2( iface )->IWebAccountProvider_iface );
}

static ULONG WINAPI provider2_Release( IWebAccountProvider2 *iface )
{
    return provider_Release( &impl_from_IWebAccountProvider2( iface )->IWebAccountProvider_iface );
}

static HRESULT WINAPI provider2_GetIids( IWebAccountProvider2 *iface, ULONG *iid_count, IID **iids )
{
    return provider_GetIids( &impl_from_IWebAccountProvider2( iface )->IWebAccountProvider_iface, iid_count, iids );
}

static HRESULT WINAPI provider2_GetRuntimeClassName( IWebAccountProvider2 *iface, HSTRING *class_name )
{
    return provider_GetRuntimeClassName( &impl_from_IWebAccountProvider2( iface )->IWebAccountProvider_iface, class_name );
}

static HRESULT WINAPI provider2_GetTrustLevel( IWebAccountProvider2 *iface, TrustLevel *trust_level )
{
    return provider_GetTrustLevel( &impl_from_IWebAccountProvider2( iface )->IWebAccountProvider_iface, trust_level );
}

static HRESULT WINAPI provider2_get_DisplayPurpose( IWebAccountProvider2 *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI provider2_get_Authority( IWebAccountProvider2 *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IWebAccountProvider2( iface )->authority, value );
}

static const struct IWebAccountProvider2Vtbl provider2_vtbl =
{
    provider2_QueryInterface,
    provider2_AddRef,
    provider2_Release,
    provider2_GetIids,
    provider2_GetRuntimeClassName,
    provider2_GetTrustLevel,
    provider2_get_DisplayPurpose,
    provider2_get_Authority,
};

HRESULT diagnostic_provider_create( HSTRING id, HSTRING authority, IWebAccountProvider **out )
{
    HRESULT hr;

    if (SUCCEEDED(hr = provider_create( id, authority, L"Wine diagnostic account provider", out )))
        FIXME( "diagnostic provider for id %s, authority %s.\n", debugstr_hstring( id ), debugstr_hstring( authority ) );
    return hr;
}

/* a provider object for an id and authority (the token broker decides at each request whether it serves them) */
HRESULT provider_create( HSTRING id, HSTRING authority, const WCHAR *name, IWebAccountProvider **out )
{
    struct provider *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebAccountProvider_iface.lpVtbl = &provider_vtbl;
    impl->IWebAccountProvider2_iface.lpVtbl = &provider2_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( id, &impl->id )) ||
        FAILED(hr = WindowsDuplicateString( authority, &impl->authority )) ||
        FAILED(hr = WindowsCreateString( name, wcslen( name ), &impl->display_name )))
    {
        provider_Release( &impl->IWebAccountProvider_iface );
        return hr;
    }
    *out = &impl->IWebAccountProvider_iface;
    return S_OK;
}

/* what a program asks for: written to the log when the diagnostic provider is on */
void diagnostic_dump_request( const char *method, IInspectable *request )
{
    IWebAccountProvider *provider = NULL;
    IWebAccountProvider2 *provider2;
    IMap_HSTRING_HSTRING *properties = NULL;
    IWebTokenRequest2 *request2;
    IWebTokenRequest *token_request;
    WebTokenRequestPromptType prompt = 0;
    HSTRING string = NULL;

    if (!request || FAILED(IInspectable_QueryInterface( request, &IID_IWebTokenRequest, (void **)&token_request )))
    {
        FIXME( "%s: the request is not a WebTokenRequest.\n", method );
        return;
    }
    FIXME( "%s: token request %p\n", method, token_request );
    if (SUCCEEDED(IWebTokenRequest_get_ClientId( token_request, &string )))
    {
        FIXME( "  client id %s\n", debugstr_hstring( string ) );
        WindowsDeleteString( string );
    }
    if (SUCCEEDED(IWebTokenRequest_get_Scope( token_request, &string )))
    {
        FIXME( "  scope %s\n", debugstr_hstring( string ) );
        WindowsDeleteString( string );
    }
    if (SUCCEEDED(IWebTokenRequest_get_PromptType( token_request, &prompt ))) FIXME( "  prompt type %d\n", prompt );
    if (SUCCEEDED(IWebTokenRequest_get_WebAccountProvider( token_request, &provider )) && provider)
    {
        if (SUCCEEDED(IWebAccountProvider_get_Id( provider, &string )))
        {
            FIXME( "  provider id %s\n", debugstr_hstring( string ) );
            WindowsDeleteString( string );
        }
        if (SUCCEEDED(IWebAccountProvider_QueryInterface( provider, &IID_IWebAccountProvider2, (void **)&provider2 )))
        {
            if (SUCCEEDED(IWebAccountProvider2_get_Authority( provider2, &string )))
            {
                FIXME( "  provider authority %s\n", debugstr_hstring( string ) );
                WindowsDeleteString( string );
            }
            IWebAccountProvider2_Release( provider2 );
        }
        IWebAccountProvider_Release( provider );
    }
    if (SUCCEEDED(IWebTokenRequest_get_Properties( token_request, &properties )) && properties)
    {
        map_hstring_dump( properties, "  property " );
        IMap_HSTRING_HSTRING_Release( properties );
    }
    if (SUCCEEDED(IWebTokenRequest_QueryInterface( token_request, &IID_IWebTokenRequest2, (void **)&request2 )))
    {
        if (SUCCEEDED(IWebTokenRequest2_get_AppProperties( request2, &properties )) && properties)
        {
            map_hstring_dump( properties, "  app property " );
            IMap_HSTRING_HSTRING_Release( properties );
        }
        IWebTokenRequest2_Release( request2 );
    }
    IWebTokenRequest_Release( token_request );
}
