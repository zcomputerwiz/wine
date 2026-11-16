/*
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
#define COBJMACROS
#include "initguid.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winstring.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Security_Credentials
#include "windows.security.credentials.h"
#define WIDL_using_Windows_Security_Authentication_Web_Core
#include "windows.security.authentication.web.core.h"
#include "webauthenticationcoremanagerinterop.h"

#include "wine/test.h"

#define check_interface( obj, iid ) check_interface_( __LINE__, obj, iid )
static void check_interface_( unsigned int line, void *obj, const IID *iid )
{
    IUnknown *iface = obj;
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) IUnknown_Release( unk );
}

static HSTRING hstring( const WCHAR *str )
{
    HSTRING ret;
    HRESULT hr = WindowsCreateString( str, wcslen( str ), &ret );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    return ret;
}

/* wait until an IAsyncOperation<T> (any T) is no longer Started */
#define wait_async( op ) wait_async_( __LINE__, (IUnknown *)(op) )
static AsyncStatus wait_async_( unsigned int line, IUnknown *op )
{
    AsyncStatus status = Started;
    IAsyncInfo *info;
    HRESULT hr;
    int i;

    hr = IUnknown_QueryInterface( op, &IID_IAsyncInfo, (void **)&info );
    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return Error;
    for (i = 0; i < 500; i++)
    {
        hr = IAsyncInfo_get_Status( info, &status );
        ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
        if (status != Started) break;
        Sleep( 10 );
    }
    IAsyncInfo_Release( info );
    ok_(__FILE__, line)( status == Completed, "got status %u.\n", status );
    return status;
}

static IActivationFactory *get_factory( const WCHAR *name )
{
    IActivationFactory *factory = NULL;
    HSTRING str = hstring( name );
    HRESULT hr;

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "%s: got hr %#lx.\n", wine_dbgstr_w( name ), hr );
    return SUCCEEDED(hr) ? factory : NULL;
}

static void test_manager(void)
{
    IWebAuthenticationCoreManagerStatics *statics;
    IWebAuthenticationCoreManagerInterop *interop;
    IAsyncOperation_WebAccountProvider *provider_op;
    IAsyncOperation_WebTokenRequestResult *result_op;
    IWebTokenRequestFactory *request_factory;
    IActivationFactory *factory;
    IWebAccountProvider *provider = (void *)0xdeadbeef;
    IWebTokenRequestResult *result;
    IWebTokenRequest *request;
    BOOL have_provider = FALSE;
    WebTokenRequestStatus status;
    WebTokenRequestPromptType prompt;
    IMap_HSTRING_HSTRING *props;
    HSTRING str, key, value, got;
    boolean replaced;
    HRESULT hr;
    INT32 cmp;

    if (!(factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager" )))
    {
        win_skip( "WebAuthenticationCoreManager not registered.\n" );
        return;
    }
    check_interface( factory, &IID_IUnknown );
    check_interface( factory, &IID_IInspectable );
    check_interface( factory, &IID_IAgileObject );
    check_interface( factory, &IID_IWebAuthenticationCoreManagerStatics4 );
    check_interface( factory, &IID_IWebAuthenticationCoreManagerInterop );

    hr = IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IWebAuthenticationCoreManagerStatics_FindAccountProviderAsync( statics, NULL, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    str = hstring( L"https://login.microsoft.com" );
    hr = IWebAuthenticationCoreManagerStatics_FindAccountProviderAsync( statics, str, &provider_op );
    WindowsDeleteString( str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    wait_async( provider_op );
    hr = IAsyncOperation_WebAccountProvider_GetResults( provider_op, &provider );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (provider)
    {
        /* Windows has the Microsoft account provider; Wine has none yet */
        trace( "an account provider exists.\n" );
        IWebAccountProvider_Release( provider );
        have_provider = TRUE;
    }
    IAsyncOperation_WebAccountProvider_Release( provider_op );

    IActivationFactory_Release( factory );

    if (!(factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebTokenRequest" )))
    {
        IWebAuthenticationCoreManagerStatics_Release( statics );
        return;
    }
    hr = IActivationFactory_QueryInterface( factory, &IID_IWebTokenRequestFactory, (void **)&request_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IActivationFactory_Release( factory );

    str = hstring( L"wl.basic" );
    value = hstring( L"0000000048170EF2" );
    hr = IWebTokenRequestFactory_Create( request_factory, NULL, str, value, &request );
    ok( hr == S_OK || broken( hr == E_INVALIDARG ), "got hr %#lx.\n", hr );
    IWebTokenRequestFactory_Release( request_factory );
    if (FAILED(hr))
    {
        WindowsDeleteString( str );
        WindowsDeleteString( value );
        IWebAuthenticationCoreManagerStatics_Release( statics );
        return;
    }

    hr = IWebTokenRequest_get_Scope( request, &got );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsCompareStringOrdinal( got, str, &cmp );
    ok( !cmp, "got scope %s.\n", debugstr_hstring( got ) );
    WindowsDeleteString( got );
    hr = IWebTokenRequest_get_ClientId( request, &got );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsCompareStringOrdinal( got, value, &cmp );
    ok( !cmp, "got client id %s.\n", debugstr_hstring( got ) );
    WindowsDeleteString( got );
    WindowsDeleteString( str );
    WindowsDeleteString( value );

    hr = IWebTokenRequest_get_PromptType( request, &prompt );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( prompt == WebTokenRequestPromptType_Default, "got prompt type %u.\n", prompt );
    provider = (void *)0xdeadbeef;
    hr = IWebTokenRequest_get_WebAccountProvider( request, &provider );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !provider, "got provider %p.\n", provider );

    hr = IWebTokenRequest_get_Properties( request, &props );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    key = hstring( L"authority" );
    value = hstring( L"https://login.microsoftonline.com/common" );
    replaced = TRUE;
    hr = IMap_HSTRING_HSTRING_Insert( props, key, value, &replaced );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !replaced, "got replaced %d.\n", replaced );
    hr = IMap_HSTRING_HSTRING_Insert( props, key, value, &replaced );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( replaced, "got replaced %d.\n", replaced );
    hr = IMap_HSTRING_HSTRING_Lookup( props, key, &got );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsCompareStringOrdinal( got, value, &cmp );
    ok( !cmp, "got value %s.\n", debugstr_hstring( got ) );
    WindowsDeleteString( got );
    WindowsDeleteString( key );
    key = hstring( L"missing" );
    hr = IMap_HSTRING_HSTRING_Lookup( props, key, &got );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    WindowsDeleteString( key );
    WindowsDeleteString( value );
    IMap_HSTRING_HSTRING_Release( props );

    if (have_provider)
    {
        /* a real provider would sign in (and may show UI); the rest checks the no-provider path */
        skip( "an account provider exists.\n" );
        IWebTokenRequest_Release( request );
        IWebAuthenticationCoreManagerStatics_Release( statics );
        return;
    }

    hr = IWebAuthenticationCoreManagerStatics_RequestTokenAsync( statics, request, &result_op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    wait_async( result_op );
    hr = IAsyncOperation_WebTokenRequestResult_GetResults( result_op, &result );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        hr = IWebTokenRequestResult_get_ResponseStatus( result, &status );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( status == WebTokenRequestStatus_AccountProviderNotAvailable, "got status %u.\n", status );
        IWebTokenRequestResult_Release( result );
    }
    IAsyncOperation_WebTokenRequestResult_Release( result_op );

    hr = IActivationFactory_QueryInterface( (IActivationFactory *)statics, &IID_IWebAuthenticationCoreManagerInterop,
                                            (void **)&interop );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IWebAuthenticationCoreManagerInterop_RequestTokenForWindowAsync( interop, GetDesktopWindow(),
            (IInspectable *)request, &IID_IAsyncOperation_WebTokenRequestResult, (void **)&result_op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        wait_async( result_op );
        IAsyncOperation_WebTokenRequestResult_Release( result_op );
    }
    IWebAuthenticationCoreManagerInterop_Release( interop );

    IWebTokenRequest_Release( request );
    IWebAuthenticationCoreManagerStatics_Release( statics );
}

static WebTokenRequestStatus request_status( IAsyncOperation_WebTokenRequestResult *op )
{
    WebTokenRequestStatus status = 0xdead;
    IWebTokenRequestResult *result;
    HRESULT hr;

    wait_async( op );
    hr = IAsyncOperation_WebTokenRequestResult_GetResults( op, &result );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        IWebTokenRequestResult_get_ResponseStatus( result, &status );
        IWebTokenRequestResult_Release( result );
    }
    IAsyncOperation_WebTokenRequestResult_Release( op );
    return status;
}

/* Wine's diagnostic provider (HKCU\Software\Wine\WebAuthentication "DiagnosticProvider" = 1): a provider object for
 * any id and authority, token requests that fail; Wine only */
static void test_diagnostic_provider(void)
{
    IWebAuthenticationCoreManagerStatics *statics;
    IAsyncOperation_WebAccountProvider *provider_op;
    IAsyncOperation_WebTokenRequestResult *result_op;
    IWebTokenRequestFactory *request_factory;
    IWebAccountProvider *provider = NULL;
    IWebAccountProvider2 *provider2;
    IActivationFactory *factory;
    IWebTokenRequest *request;
    HSTRING id, authority, got, scope, client;
    DWORD one = 1;
    HKEY key;
    INT32 cmp;
    HRESULT hr;
    LONG res;

    if (!winetest_platform_is_wine)
    {
        skip( "Wine's diagnostic provider.\n" );
        return;
    }
    if (!(factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager" ))) return;
    hr = IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IActivationFactory_Release( factory );

    res = RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                           &key, NULL );
    ok( !res, "got %ld.\n", res );
    res = RegSetValueExW( key, L"DiagnosticProvider", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    ok( !res, "got %ld.\n", res );

    id = hstring( L"https://login.microsoft.com" );
    authority = hstring( L"consumers" );
    hr = IWebAuthenticationCoreManagerStatics_FindAccountProviderWithAuthorityAsync( statics, id, authority, &provider_op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    wait_async( provider_op );
    hr = IAsyncOperation_WebAccountProvider_GetResults( provider_op, &provider );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( provider != NULL, "got no provider.\n" );
    IAsyncOperation_WebAccountProvider_Release( provider_op );
    if (provider)
    {
        hr = IWebAccountProvider_get_Id( provider, &got );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        WindowsCompareStringOrdinal( got, id, &cmp );
        ok( !cmp, "got id %s.\n", debugstr_hstring( got ) );
        WindowsDeleteString( got );
        hr = IWebAccountProvider_QueryInterface( provider, &IID_IWebAccountProvider2, (void **)&provider2 );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = IWebAccountProvider2_get_Authority( provider2, &got );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        WindowsCompareStringOrdinal( got, authority, &cmp );
        ok( !cmp, "got authority %s.\n", debugstr_hstring( got ) );
        WindowsDeleteString( got );
        IWebAccountProvider2_Release( provider2 );

        factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebTokenRequest" );
        hr = IActivationFactory_QueryInterface( factory, &IID_IWebTokenRequestFactory, (void **)&request_factory );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        IActivationFactory_Release( factory );
        scope = hstring( L"service::ssl.live.com::MBI_SSL" );
        client = hstring( L"00000000-0000-0000-0000-000000000000" );
        hr = IWebTokenRequestFactory_Create( request_factory, provider, scope, client, &request );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        WindowsDeleteString( scope );
        WindowsDeleteString( client );
        IWebTokenRequestFactory_Release( request_factory );

        hr = IWebAuthenticationCoreManagerStatics_GetTokenSilentlyAsync( statics, request, &result_op );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( request_status( result_op ) == WebTokenRequestStatus_UserInteractionRequired, "wrong silent status.\n" );
        hr = IWebAuthenticationCoreManagerStatics_RequestTokenAsync( statics, request, &result_op );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( request_status( result_op ) == WebTokenRequestStatus_ProviderError, "wrong interactive status.\n" );
        IWebTokenRequest_Release( request );
        IWebAccountProvider_Release( provider );
    }

    /* off again: no provider */
    RegDeleteValueW( key, L"DiagnosticProvider" );
    RegCloseKey( key );
    RegDeleteKeyW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication" );
    provider = (void *)0xdeadbeef;
    hr = IWebAuthenticationCoreManagerStatics_FindAccountProviderWithAuthorityAsync( statics, id, authority, &provider_op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    wait_async( provider_op );
    hr = IAsyncOperation_WebAccountProvider_GetResults( provider_op, &provider );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !provider, "got provider %p.\n", provider );
    IAsyncOperation_WebAccountProvider_Release( provider_op );

    WindowsDeleteString( id );
    WindowsDeleteString( authority );
    IWebAuthenticationCoreManagerStatics_Release( statics );
}

START_TEST(web_core)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_manager();
    test_diagnostic_provider();

    RoUninitialize();
}
