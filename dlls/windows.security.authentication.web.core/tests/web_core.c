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

    /* the properties can be iterated (OneAuth reads them so) */
    {
        IIterable_IKeyValuePair_HSTRING_HSTRING *iterable;
        IIterator_IKeyValuePair_HSTRING_HSTRING *iterator;
        IKeyValuePair_HSTRING_HSTRING *pair;
        boolean has;

        hr = IMap_HSTRING_HSTRING_QueryInterface( props, &IID_IIterable_IKeyValuePair_HSTRING_HSTRING, (void **)&iterable );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        if (SUCCEEDED(hr))
        {
            hr = IIterable_IKeyValuePair_HSTRING_HSTRING_First( iterable, &iterator );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            hr = IIterator_IKeyValuePair_HSTRING_HSTRING_get_HasCurrent( iterator, &has );
            ok( hr == S_OK && has, "got hr %#lx, has %d.\n", hr, has );
            hr = IIterator_IKeyValuePair_HSTRING_HSTRING_get_Current( iterator, &pair );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            hr = IKeyValuePair_HSTRING_HSTRING_get_Key( pair, &got );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            key = hstring( L"authority" );
            WindowsCompareStringOrdinal( got, key, &cmp );
            ok( !cmp, "got key %s.\n", debugstr_hstring( got ) );
            WindowsDeleteString( key );
            WindowsDeleteString( got );
            hr = IKeyValuePair_HSTRING_HSTRING_get_Value( pair, &got );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            WindowsCompareStringOrdinal( got, value, &cmp );
            ok( !cmp, "got value %s.\n", debugstr_hstring( got ) );
            WindowsDeleteString( got );
            IKeyValuePair_HSTRING_HSTRING_Release( pair );
            hr = IIterator_IKeyValuePair_HSTRING_HSTRING_MoveNext( iterator, &has );
            ok( hr == S_OK && !has, "got hr %#lx, has %d.\n", hr, has );
            IIterator_IKeyValuePair_HSTRING_HSTRING_Release( iterator );
            IIterable_IKeyValuePair_HSTRING_HSTRING_Release( iterable );
        }
    }
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

static UINT32 last_error_code;

/* the result's status; its provider error's code in last_error_code (0xdead when there is none) */
static WebTokenRequestStatus request_status( IAsyncOperation_WebTokenRequestResult *op )
{
    WebTokenRequestStatus status = 0xdead;
    IWebTokenRequestResult *result;
    IWebProviderError *error;
    HRESULT hr;

    last_error_code = 0xdead;
    wait_async( op );
    hr = IAsyncOperation_WebTokenRequestResult_GetResults( op, &result );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        IWebTokenRequestResult_get_ResponseStatus( result, &status );
        if (SUCCEEDED(IWebTokenRequestResult_get_ResponseError( result, &error )) && error)
        {
            IWebProviderError_get_ErrorCode( error, &last_error_code );
            IWebProviderError_Release( error );
        }
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
        /* Windows 11's Microsoft account provider, consumers authority (its error is always there) */
        ok( last_error_code == 0x80860006, "got error %#x.\n", last_error_code );
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

/*
 * The token broker with its test provider (WineExplorer's tokenbroker.exe, tests/wam/run-broker-tests.sh): runs only
 * when WEBCORE_TEST_AUTHORITY names the local test authority (tests/wam/test_authority.py); the test sets
 * HKCU\Software\Wine\WebAuthentication "Broker" and "TestAuthority" for its run and removes them. Wine only.
 */

#define TEST_PROVIDER L"https://login.wine.test"
#define TEST_CLIENT   L"11111111-2222-3333-4444-555555555555"

/* wait up to a minute: the first request starts the broker */
static void wait_broker( IUnknown *op )
{
    AsyncStatus status = Started;
    IAsyncInfo *info;
    int i;

    if (FAILED(IUnknown_QueryInterface( op, &IID_IAsyncInfo, (void **)&info ))) return;
    for (i = 0; i < 6000 && status == Started; i++)
    {
        MSG msg;

        IAsyncInfo_get_Status( info, &status );
        if (status != Started) break;
        /* the calling window's thread keeps its messages going, as a program's would (the sign-in window is owned by
         * the test's window) */
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );
        Sleep( 10 );
    }
    IAsyncInfo_Release( info );
    ok( status == Completed, "got status %u.\n", status );
}

struct broker_result
{
    WebTokenRequestStatus status;
    UINT32 error;
    WCHAR message[256];
    WCHAR token[128];
    WCHAR user_name[128];
    WCHAR account_id[128];
    WCHAR scopes[256];
    BOOL id_token;
    UINT32 responses;
};

static void hstring_copy( HSTRING str, WCHAR *buf, UINT32 size )
{
    UINT32 len;
    const WCHAR *text = WindowsGetStringRawBuffer( str, &len );

    if (len >= size) len = size - 1;
    memcpy( buf, text, len * sizeof(WCHAR) );
    buf[len] = 0;
}

static void map_value( IMap_HSTRING_HSTRING *map, const WCHAR *name, WCHAR *buf, UINT32 size )
{
    HSTRING key = hstring( name ), value = NULL;

    buf[0] = 0;
    if (SUCCEEDED(IMap_HSTRING_HSTRING_Lookup( map, key, &value ))) hstring_copy( value, buf, size );
    WindowsDeleteString( value );
    WindowsDeleteString( key );
}

static void broker_result( IAsyncOperation_WebTokenRequestResult *op, struct broker_result *out )
{
    IVectorView_WebTokenResponse *responses;
    IMap_HSTRING_HSTRING *props;
    IWebTokenRequestResult *result;
    IWebTokenResponse *response;
    IWebProviderError *error;
    IWebAccount2 *account2;
    IWebAccount *account;
    WCHAR buf[16];
    HSTRING str;
    HRESULT hr;

    memset( out, 0, sizeof(*out) );
    out->status = 0xdead;
    out->error = 0xdead;
    wait_broker( (IUnknown *)op );
    hr = IAsyncOperation_WebTokenRequestResult_GetResults( op, &result );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncOperation_WebTokenRequestResult_Release( op );
    if (FAILED(hr)) return;
    IWebTokenRequestResult_get_ResponseStatus( result, &out->status );
    if (SUCCEEDED(IWebTokenRequestResult_get_ResponseError( result, &error )) && error)
    {
        IWebProviderError_get_ErrorCode( error, &out->error );
        if (SUCCEEDED(IWebProviderError_get_ErrorMessage( error, &str )))
        {
            hstring_copy( str, out->message, ARRAY_SIZE(out->message) );
            WindowsDeleteString( str );
        }
        IWebProviderError_Release( error );
    }
    if (SUCCEEDED(IWebTokenRequestResult_get_ResponseData( result, &responses )) && responses)
    {
        IVectorView_WebTokenResponse_get_Size( responses, &out->responses );
        if (out->responses && SUCCEEDED(IVectorView_WebTokenResponse_GetAt( responses, 0, &response )))
        {
            if (SUCCEEDED(IWebTokenResponse_get_Token( response, &str )))
            {
                hstring_copy( str, out->token, ARRAY_SIZE(out->token) );
                WindowsDeleteString( str );
            }
            if (SUCCEEDED(IWebTokenResponse_get_WebAccount( response, &account )) && account)
            {
                IWebAccount_get_UserName( account, &str );
                hstring_copy( str, out->user_name, ARRAY_SIZE(out->user_name) );
                WindowsDeleteString( str );
                hr = IWebAccount_QueryInterface( account, &IID_IWebAccount2, (void **)&account2 );
                ok( hr == S_OK, "got hr %#lx.\n", hr );
                if (SUCCEEDED(hr))
                {
                    IWebAccount2_get_Id( account2, &str );
                    hstring_copy( str, out->account_id, ARRAY_SIZE(out->account_id) );
                    WindowsDeleteString( str );
                    IWebAccount2_Release( account2 );
                }
                IWebAccount_Release( account );
            }
            if (SUCCEEDED(IWebTokenResponse_get_Properties( response, &props )))
            {
                map_value( props, L"wamcompat_scopes", out->scopes, ARRAY_SIZE(out->scopes) );
                map_value( props, L"wamcompat_id_token", buf, ARRAY_SIZE(buf) );
                out->id_token = !!buf[0];
                IMap_HSTRING_HSTRING_Release( props );
            }
            IWebTokenResponse_Release( response );
        }
        IVectorView_WebTokenResponse_Release( responses );
    }
    IWebTokenRequestResult_Release( result );
}

static IWebTokenRequest *broker_request( IWebAccountProvider *provider, const WCHAR *scope_text, const WCHAR *hint )
{
    IWebTokenRequestFactory *request_factory;
    IMap_HSTRING_HSTRING *props;
    IActivationFactory *factory;
    IWebTokenRequest *request = NULL;
    HSTRING scope, client, key, value;
    boolean replaced;
    HRESULT hr;

    factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebTokenRequest" );
    hr = IActivationFactory_QueryInterface( factory, &IID_IWebTokenRequestFactory, (void **)&request_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IActivationFactory_Release( factory );
    scope = hstring( scope_text );
    client = hstring( TEST_CLIENT );
    hr = IWebTokenRequestFactory_CreateWithPromptType( request_factory, provider, scope, client,
                                                       WebTokenRequestPromptType_ForceAuthentication, &request );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( scope );
    WindowsDeleteString( client );
    IWebTokenRequestFactory_Release( request_factory );
    if (hint && SUCCEEDED(IWebTokenRequest_get_Properties( request, &props )))
    {
        key = hstring( L"LoginHint" );
        value = hstring( hint );
        hr = IMap_HSTRING_HSTRING_Insert( props, key, value, &replaced );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        WindowsDeleteString( key );
        WindowsDeleteString( value );
        IMap_HSTRING_HSTRING_Release( props );
    }
    return request;
}

static IWebAccountProvider *broker_provider( IWebAuthenticationCoreManagerStatics *statics, const WCHAR *id_text,
                                             const WCHAR *authority_text )
{
    IAsyncOperation_WebAccountProvider *op;
    IWebAccountProvider *provider = NULL;
    HSTRING id = hstring( id_text ), authority = hstring( authority_text );
    HRESULT hr;

    hr = IWebAuthenticationCoreManagerStatics_FindAccountProviderWithAuthorityAsync( statics, id, authority, &op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    wait_async( op );
    hr = IAsyncOperation_WebAccountProvider_GetResults( op, &provider );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( provider != NULL, "got no provider.\n" );
    IAsyncOperation_WebAccountProvider_Release( op );
    WindowsDeleteString( id );
    WindowsDeleteString( authority );
    return provider;
}

static void broker_interactive( IWebAuthenticationCoreManagerInterop *interop, HWND window,
                                IWebAccountProvider *provider, const WCHAR *scope, const WCHAR *hint,
                                struct broker_result *out )
{
    IAsyncOperation_WebTokenRequestResult *op;
    IWebTokenRequest *request = broker_request( provider, scope, hint );
    HRESULT hr;

    hr = IWebAuthenticationCoreManagerInterop_RequestTokenForWindowAsync( interop, window, (IInspectable *)request,
            &IID_IAsyncOperation_WebTokenRequestResult, (void **)&op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IWebTokenRequest_Release( request );
    if (SUCCEEDED(hr)) broker_result( op, out );
}

static void broker_silent( IWebAuthenticationCoreManagerStatics *statics, IWebAccountProvider *provider,
                           IWebAccount *account, const WCHAR *scope, struct broker_result *out )
{
    IAsyncOperation_WebTokenRequestResult *op;
    IWebTokenRequest *request = broker_request( provider, scope, NULL );
    HRESULT hr;

    if (account)
        hr = IWebAuthenticationCoreManagerStatics_GetTokenSilentlyWithWebAccountAsync( statics, request, account, &op );
    else
        hr = IWebAuthenticationCoreManagerStatics_GetTokenSilentlyAsync( statics, request, &op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IWebTokenRequest_Release( request );
    if (SUCCEEDED(hr)) broker_result( op, out );
}

/* the accounts of the provider: their number, the first one */
static UINT32 broker_accounts( IWebAuthenticationCoreManagerStatics4 *statics4, IWebAccountProvider *provider,
                               IWebAccount **first )
{
    IAsyncOperation_FindAllAccountsResult *op;
    IVectorView_WebAccount *accounts;
    FindAllWebAccountsStatus status = 0xdead;
    IFindAllAccountsResult *result;
    UINT32 count = 0xdead;
    HSTRING client = hstring( TEST_CLIENT );
    HRESULT hr;

    if (first) *first = NULL;
    hr = IWebAuthenticationCoreManagerStatics4_FindAllAccountsWithClientIdAsync( statics4, provider, client, &op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( client );
    if (FAILED(hr)) return count;
    wait_broker( (IUnknown *)op );
    hr = IAsyncOperation_FindAllAccountsResult_GetResults( op, &result );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncOperation_FindAllAccountsResult_Release( op );
    if (FAILED(hr)) return count;
    IFindAllAccountsResult_get_Status( result, &status );
    ok( status == FindAllWebAccountsStatus_Success, "got status %d.\n", status );
    if (SUCCEEDED(IFindAllAccountsResult_get_Accounts( result, &accounts )))
    {
        IVectorView_WebAccount_get_Size( accounts, &count );
        if (count && first) IVectorView_WebAccount_GetAt( accounts, 0, first );
        IVectorView_WebAccount_Release( accounts );
    }
    IFindAllAccountsResult_Release( result );
    return count;
}

static void test_broker(void)
{
    IWebAuthenticationCoreManagerStatics4 *statics4;
    IWebAuthenticationCoreManagerStatics *statics;
    IWebAuthenticationCoreManagerInterop *interop;
    IAsyncOperation_WebAccount *account_op;
    IWebAccountProvider *provider, *other;
    IWebAccount *account = NULL, *found;
    IActivationFactory *factory;
    struct broker_result r;
    WCHAR authority[256], first_token[128];
    DWORD size = sizeof(authority), one = 1;
    HSTRING id;
    HWND window;
    HKEY key;
    HRESULT hr;

    if (!winetest_platform_is_wine || !(size = GetEnvironmentVariableW( L"WEBCORE_TEST_AUTHORITY", authority,
                                                                        ARRAY_SIZE(authority) )) ||
        size >= ARRAY_SIZE(authority))
    {
        skip( "the token broker's test provider: no WEBCORE_TEST_AUTHORITY.\n" );
        return;
    }
    if (RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                         &key, NULL ))
        return;
    RegSetValueExW( key, L"Broker", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    RegSetValueExW( key, L"TestAuthority", 0, REG_SZ, (BYTE *)authority, (wcslen( authority ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
    if (!(factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager" ))) return;
    IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics, (void **)&statics );
    IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics4, (void **)&statics4 );
    IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerInterop, (void **)&interop );
    IActivationFactory_Release( factory );
    window = CreateWindowExW( 0, L"static", L"web_core broker test", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                              NULL, NULL, NULL, NULL );
    ok( window != NULL, "no window.\n" );

    provider = broker_provider( statics, TEST_PROVIDER, L"consumers" );
    if (!provider) goto done;

    if (GetEnvironmentVariableW( L"WEBCORE_TEST_WINDOW", NULL, 0 ))
    {
        /* the broker's WebView2 sign-in window against the test authority: the sign-in alone */
        if (!RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication", 0, KEY_ALL_ACCESS, &key ))
        {
            RegSetValueExW( key, L"TestWindow", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
            RegCloseKey( key );
        }
        broker_interactive( interop, window, provider, L"user.read", L"ok@wine.test", &r );
        ok( r.status == WebTokenRequestStatus_Success, "window: got status %d, error %#x %s.\n", r.status, r.error,
            wine_dbgstr_w( r.message ) );
        ok( !wcscmp( r.user_name, L"ok@wine.test" ), "window: got user name %s.\n", wine_dbgstr_w( r.user_name ) );
        IWebAccountProvider_Release( provider );
        goto done;
    }

    /* a new user: no account, a silent request needs the user */
    ok( broker_accounts( statics4, provider, NULL ) == 0, "accounts already.\n" );
    broker_silent( statics, provider, NULL, L"user.read", &r );
    ok( r.status == WebTokenRequestStatus_UserInteractionRequired, "got status %d.\n", r.status );
    ok( r.error == 0x80860006, "got error %#x.\n", r.error );

    /* the interactive sign-in through the test hook */
    broker_interactive( interop, window, provider, L"user.read", L"ok@wine.test", &r );
    ok( r.status == WebTokenRequestStatus_Success, "got status %d, error %#x.\n", r.status, r.error );
    ok( r.responses == 1, "got %u responses.\n", r.responses );
    ok( !wcsncmp( r.token, L"at-", 3 ), "got token %s.\n", wine_dbgstr_w( r.token ) );
    ok( !wcscmp( r.user_name, L"ok@wine.test" ), "got user name %s.\n", wine_dbgstr_w( r.user_name ) );
    ok( !wcscmp( r.account_id, L"oid-ok" ), "got account id %s.\n", wine_dbgstr_w( r.account_id ) );
    ok( r.id_token, "no wamcompat_id_token.\n" );
    ok( !!wcsstr( r.scopes, L"user.read" ), "got scopes %s.\n", wine_dbgstr_w( r.scopes ) );
    wcscpy( first_token, r.token );

    /* the account is listed and found */
    ok( broker_accounts( statics4, provider, &account ) == 1, "wrong account count.\n" );
    ok( account != NULL, "no account.\n" );
    id = hstring( L"oid-ok" );
    hr = IWebAuthenticationCoreManagerStatics_FindAccountAsync( statics, provider, id, &account_op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( id );
    found = NULL;
    wait_broker( (IUnknown *)account_op );
    IAsyncOperation_WebAccount_GetResults( account_op, &found );
    ok( found != NULL, "account not found.\n" );
    if (found) IWebAccount_Release( found );
    IAsyncOperation_WebAccount_Release( account_op );

    /* silent: the same scope from the cache, another one by refresh token */
    broker_silent( statics, provider, NULL, L"user.read", &r );
    ok( r.status == WebTokenRequestStatus_Success, "got status %d, error %#x.\n", r.status, r.error );
    ok( !wcscmp( r.token, first_token ), "got token %s, not the cached %s.\n", wine_dbgstr_w( r.token ),
        wine_dbgstr_w( first_token ) );
    broker_silent( statics, provider, account, L"files.read", &r );
    ok( r.status == WebTokenRequestStatus_Success, "got status %d, error %#x.\n", r.status, r.error );
    ok( r.token[0] && wcscmp( r.token, first_token ), "got token %s.\n", wine_dbgstr_w( r.token ) );
    ok( !wcscmp( r.user_name, L"ok@wine.test" ), "got user name %s.\n", wine_dbgstr_w( r.user_name ) );

    /* cancelled: a page waits for the user and the window is closed; the authority's access_denied */
    broker_interactive( interop, window, provider, L"user.read", L"cancel@wine.test", &r );
    ok( r.status == WebTokenRequestStatus_UserCancel, "got status %d.\n", r.status );
    broker_interactive( interop, window, provider, L"user.read", L"denied@wine.test", &r );
    ok( r.status == WebTokenRequestStatus_UserCancel, "got status %d.\n", r.status );

    /* refused: a wrong state, a redirect to another host with the same path */
    broker_interactive( interop, window, provider, L"user.read", L"badstate@wine.test", &r );
    ok( r.status == WebTokenRequestStatus_ProviderError, "got status %d.\n", r.status );
    ok( !r.token[0], "got a token.\n" );
    broker_interactive( interop, window, provider, L"user.read", L"hostile@wine.test", &r );
    ok( r.status == WebTokenRequestStatus_ProviderError, "got status %d.\n", r.status );
    ok( !r.token[0], "got a token.\n" );

    /* the refresh token refused: the user is needed again, and the account is gone for the client */
    broker_silent( statics, provider, account, L"refuse", &r );
    ok( r.status == WebTokenRequestStatus_UserInteractionRequired, "got status %d.\n", r.status );
    ok( broker_accounts( statics4, provider, NULL ) == 0, "the account is still there.\n" );
    if (account) IWebAccount_Release( account );
    IWebAccountProvider_Release( provider );

    /* an authority that is not a plain name */
    if ((other = broker_provider( statics, TEST_PROVIDER, L"../consumers" )))
    {
        broker_silent( statics, other, NULL, L"user.read", &r );
        ok( r.status == WebTokenRequestStatus_ProviderError, "got status %d.\n", r.status );
        IWebAccountProvider_Release( other );
    }
    /* a provider the broker does not serve */
    if ((other = broker_provider( statics, L"https://login.windows.local", L"" )))
    {
        broker_silent( statics, other, NULL, L"user.read", &r );
        ok( r.status == WebTokenRequestStatus_AccountProviderNotAvailable, "got status %d.\n", r.status );
        IWebAccountProvider_Release( other );
    }
    /* a test authority that is not on this machine is refused, and with it the test provider */
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication", 0, KEY_ALL_ACCESS, &key ))
    {
        RegSetValueExW( key, L"TestAuthority", 0, REG_SZ, (BYTE *)L"https://login.microsoftonline.com/",
                        sizeof(L"https://login.microsoftonline.com/") );
        if ((other = broker_provider( statics, TEST_PROVIDER, L"consumers" )))
        {
            broker_silent( statics, other, NULL, L"user.read", &r );
            ok( r.status == WebTokenRequestStatus_AccountProviderNotAvailable, "got status %d.\n", r.status );
            IWebAccountProvider_Release( other );
        }
        RegCloseKey( key );
    }

done:
    RegDeleteKeyW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication" );
    DestroyWindow( window );
    IWebAuthenticationCoreManagerInterop_Release( interop );
    IWebAuthenticationCoreManagerStatics4_Release( statics4 );
    IWebAuthenticationCoreManagerStatics_Release( statics );
}

START_TEST(web_core)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_manager();
    test_diagnostic_provider();
    test_broker();

    RoUninitialize();
}
