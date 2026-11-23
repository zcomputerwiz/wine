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
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winstring.h"
#include "sddl.h"

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
        IWebProviderError *error = NULL;

        hr = IWebTokenRequestResult_get_ResponseStatus( result, &status );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( status == WebTokenRequestStatus_AccountProviderNotAvailable, "got status %u.\n", status );
        hr = IWebTokenRequestResult_get_ResponseError( result, &error );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        if (error)
        {
            const WCHAR *text;
            HSTRING message;
            UINT32 len;

            /* the message's length is its text's, without a terminator */
            hr = IWebProviderError_get_ErrorMessage( error, &message );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            text = WindowsGetStringRawBuffer( message, &len );
            ok( len == wcslen( text ), "got length %u for %s.\n", len, debugstr_w( text ) );
            WindowsDeleteString( message );
            IWebProviderError_Release( error );
        }
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

#define SETTINGS_KEY L"Software\\Wine\\WebAuthentication"

/* a switch under HKCU\Software\Wine\WebAuthentication as it was before a test, put back after it */
struct saved_setting
{
    const WCHAR *name;
    BOOL present;
    DWORD type;
    DWORD size;
    BYTE data[1024];
};

static void save_setting( struct saved_setting *saved, const WCHAR *name )
{
    saved->name = name;
    saved->size = sizeof(saved->data);
    saved->present = !RegGetValueW( HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_ANY | RRF_NOEXPAND, &saved->type,
                                    saved->data, &saved->size );
}

/* puts the switch back; the key goes when nothing else is left in it */
static void restore_setting( const struct saved_setting *saved )
{
    DWORD values = 0, subkeys = 0;
    HKEY key;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_ALL_ACCESS, &key )) return;
    if (saved->present) RegSetValueExW( key, saved->name, 0, saved->type, saved->data, saved->size );
    else RegDeleteValueW( key, saved->name );
    if (RegQueryInfoKeyW( key, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL ))
        values = 1;
    RegCloseKey( key );
    if (!values && !subkeys) RegDeleteKeyW( HKEY_CURRENT_USER, SETTINGS_KEY );
}

/* Wine's diagnostic provider (HKCU\Software\Wine\WebAuthentication "DiagnosticProvider" = 1): a provider object for
 * any id and authority, token requests that fail; Wine only */
static void test_diagnostic_provider(void)
{
    struct saved_setting saved;
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

    save_setting( &saved, L"DiagnosticProvider" );
    res = RegCreateKeyExW( HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL );
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
    restore_setting( &saved );
}

/*
 * The token broker with its test provider (WineExplorer's tokenbroker.exe, tests/wam/run-broker-tests.sh): runs only
 * when WEBCORE_TEST_AUTHORITY names the local test authority (tests/wam/test_authority.py); the test sets
 * HKCU\Software\Wine\WebAuthentication "Broker", "TestAuthority" and "TestWindow" for its run and puts back what was
 * there before. Wine only.
 */

#define TEST_PROVIDER L"https://login.wine.test"
#define TEST_CLIENT   L"11111111-2222-3333-4444-555555555555"
#define OTHER_CLIENT  L"66666666-7777-8888-9999-000000000000"   /* another client id of the same program */
#define CHILD_CLIENT  L"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"   /* a nested app inside TEST_CLIENT's program */

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
    WCHAR token[1024];
    WCHAR user_name[128];
    WCHAR account_id[128];
    WCHAR scopes[256];
    BOOL id_token;
    UINT32 responses;
    WebAccountState state;
    UINT32 account_props;               /* the WebAccount's Properties: how many */
    WCHAR display_name[64];             /* and some of them */
    WCHAR first_name[64];
    WCHAR last_name[64];
    WCHAR uid[64];
    WCHAR safe_id[64];
    WCHAR roaming[16];
    BOOL wamcompat;                     /* the response's wamcompat_expires_on is there */
    WCHAR wamcompat_account[64];        /* wamcompat_account_id */
    BOOL mats;                          /* and the telemetry property MATS */
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

/* the same for a read-only view (WebAccount.Properties) */
static void view_value( IMapView_HSTRING_HSTRING *view, const WCHAR *name, WCHAR *buf, UINT32 size )
{
    HSTRING key = hstring( name ), value = NULL;

    buf[0] = 0;
    if (SUCCEEDED(IMapView_HSTRING_HSTRING_Lookup( view, key, &value ))) hstring_copy( value, buf, size );
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
                IWebAccount_get_State( account, &out->state );
                hr = IWebAccount_QueryInterface( account, &IID_IWebAccount2, (void **)&account2 );
                ok( hr == S_OK, "got hr %#lx.\n", hr );
                if (SUCCEEDED(hr))
                {
                    IMapView_HSTRING_HSTRING *account_props;

                    IWebAccount2_get_Id( account2, &str );
                    hstring_copy( str, out->account_id, ARRAY_SIZE(out->account_id) );
                    WindowsDeleteString( str );
                    hr = IWebAccount2_get_Properties( account2, &account_props );
                    ok( hr == S_OK, "got hr %#lx.\n", hr );
                    if (SUCCEEDED(hr))
                    {
                        IMapView_HSTRING_HSTRING_get_Size( account_props, &out->account_props );
                        view_value( account_props, L"DisplayName", out->display_name, ARRAY_SIZE(out->display_name) );
                        view_value( account_props, L"FirstName", out->first_name, ARRAY_SIZE(out->first_name) );
                        view_value( account_props, L"LastName", out->last_name, ARRAY_SIZE(out->last_name) );
                        view_value( account_props, L"UID", out->uid, ARRAY_SIZE(out->uid) );
                        view_value( account_props, L"SafeCustomerId", out->safe_id, ARRAY_SIZE(out->safe_id) );
                        view_value( account_props, L"IsRoamingEncryptionKeyAvailable", out->roaming,
                                    ARRAY_SIZE(out->roaming) );
                        IMapView_HSTRING_HSTRING_Release( account_props );
                    }
                    IWebAccount2_Release( account2 );
                }
                IWebAccount_Release( account );
            }
            if (SUCCEEDED(IWebTokenResponse_get_Properties( response, &props )))
            {
                map_value( props, L"wamcompat_scopes", out->scopes, ARRAY_SIZE(out->scopes) );
                map_value( props, L"wamcompat_id_token", buf, ARRAY_SIZE(buf) );
                out->id_token = !!buf[0];
                map_value( props, L"wamcompat_expires_on", buf, ARRAY_SIZE(buf) );
                out->wamcompat = !!buf[0];
                map_value( props, L"wamcompat_account_id", out->wamcompat_account, ARRAY_SIZE(out->wamcompat_account) );
                map_value( props, L"MATS", buf, ARRAY_SIZE(buf) );
                out->mats = !!buf[0];
                IMap_HSTRING_HSTRING_Release( props );
            }
            IWebTokenResponse_Release( response );
        }
        IVectorView_WebTokenResponse_Release( responses );
    }
    IWebTokenRequestResult_Release( result );
}

/* a request for the client id CLIENT_TEXT (TEST_CLIENT when NULL), with request properties EXTRA (key, value, key,
 * value, ..., NULL) besides the login hint */
static IWebTokenRequest *broker_request_ex( IWebAccountProvider *provider, const WCHAR *scope_text, const WCHAR *hint,
                                            const WCHAR *client_text, const WCHAR **extra )
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
    client = hstring( client_text ? client_text : TEST_CLIENT );
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
    for (; extra && extra[0] && SUCCEEDED(IWebTokenRequest_get_Properties( request, &props )); extra += 2)
    {
        key = hstring( extra[0] );
        value = hstring( extra[1] );
        hr = IMap_HSTRING_HSTRING_Insert( props, key, value, &replaced );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        WindowsDeleteString( key );
        WindowsDeleteString( value );
        IMap_HSTRING_HSTRING_Release( props );
    }
    return request;
}

static IWebTokenRequest *broker_request( IWebAccountProvider *provider, const WCHAR *scope_text, const WCHAR *hint )
{
    return broker_request_ex( provider, scope_text, hint, NULL, NULL );
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

static void broker_silent_ex( IWebAuthenticationCoreManagerStatics *statics, IWebAccountProvider *provider,
                              IWebAccount *account, const WCHAR *scope, const WCHAR *client, const WCHAR **extra,
                              struct broker_result *out )
{
    IAsyncOperation_WebTokenRequestResult *op;
    IWebTokenRequest *request = broker_request_ex( provider, scope, NULL, client, extra );
    HRESULT hr;

    if (account)
        hr = IWebAuthenticationCoreManagerStatics_GetTokenSilentlyWithWebAccountAsync( statics, request, account, &op );
    else
        hr = IWebAuthenticationCoreManagerStatics_GetTokenSilentlyAsync( statics, request, &op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IWebTokenRequest_Release( request );
    if (SUCCEEDED(hr)) broker_result( op, out );
}

static void broker_silent( IWebAuthenticationCoreManagerStatics *statics, IWebAccountProvider *provider,
                           IWebAccount *account, const WCHAR *scope, struct broker_result *out )
{
    broker_silent_ex( statics, provider, account, scope, NULL, NULL, out );
}

/* FindAccountAsync by id: whether the broker answered with an account */
static BOOL broker_find_account( IWebAuthenticationCoreManagerStatics *statics, IWebAccountProvider *provider,
                                 const WCHAR *id_text, WCHAR *found_id, UINT32 found_size )
{
    IAsyncOperation_WebAccount *op;
    IWebAccount2 *account2;
    IWebAccount *found = NULL;
    HSTRING id = hstring( id_text ), str;
    HRESULT hr;

    if (found_id) found_id[0] = 0;
    hr = IWebAuthenticationCoreManagerStatics_FindAccountAsync( statics, provider, id, &op );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( id );
    if (FAILED(hr)) return FALSE;
    wait_broker( (IUnknown *)op );
    IAsyncOperation_WebAccount_GetResults( op, &found );
    IAsyncOperation_WebAccount_Release( op );
    if (!found) return FALSE;
    if (found_id && SUCCEEDED(IWebAccount_QueryInterface( found, &IID_IWebAccount2, (void **)&account2 )))
    {
        IWebAccount2_get_Id( account2, &str );
        hstring_copy( str, found_id, found_size );
        WindowsDeleteString( str );
        IWebAccount2_Release( account2 );
    }
    IWebAccount_Release( found );
    return TRUE;
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
        IIterable_WebAccount *iterable;
        IIterator_WebAccount *iterator;
        boolean has_current = 2;

        IVectorView_WebAccount_get_Size( accounts, &count );
        if (count && first) IVectorView_WebAccount_GetAt( accounts, 0, first );
        /* the list can be walked as programs' foreach loops do */
        hr = IVectorView_WebAccount_QueryInterface( accounts, &IID_IIterable_WebAccount, (void **)&iterable );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        if (SUCCEEDED(hr))
        {
            hr = IIterable_WebAccount_First( iterable, &iterator );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            if (SUCCEEDED(hr))
            {
                IIterator_WebAccount_get_HasCurrent( iterator, &has_current );
                ok( has_current == (count != 0), "got HasCurrent %d for %u accounts.\n", has_current, count );
                IIterator_WebAccount_Release( iterator );
            }
            IIterable_WebAccount_Release( iterable );
        }
        IVectorView_WebAccount_Release( accounts );
    }
    IFindAllAccountsResult_Release( result );
    return count;
}

/* the child of test_broker_sandboxed: a silent request for the test provider's account; the exit code is the
 * request's status in the high word and the low word of its error code */
static void broker_child( BOOL other_client )
{
    IWebAuthenticationCoreManagerStatics *statics;
    IWebAccountProvider *provider;
    IActivationFactory *factory;
    struct broker_result r = { 0xdead, 0xdead };

    if ((factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager" )))
    {
        IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics, (void **)&statics );
        IActivationFactory_Release( factory );
        if ((provider = broker_provider( statics, TEST_PROVIDER, L"consumers" )))
        {
            broker_silent_ex( statics, provider, NULL, L"user.read", other_client ? OTHER_CLIENT : NULL, NULL, &r );
            IWebAccountProvider_Release( provider );
        }
        IWebAuthenticationCoreManagerStatics_Release( statics );
    }
    RoUninitialize();
    ExitProcess( (r.status << 16) | (r.error & 0xffff) );
}

/* runs this program as a child with the arguments ARGS (after "web_core"), with a token or, with none, as the test's
 * own user; its exit code */
static DWORD run_child_args( HANDLE token, const char *args )
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH * 2], **argv;
    DWORD code = 0xdeadbeef;
    BOOL ret;

    winetest_get_mainargs( &argv );
    sprintf( cmd, "\"%s\" web_core %s", argv[0], args );
    if (token) ret = CreateProcessAsUserA( token, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi );
    else ret = CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi );
    if (!ret)
    {
        ok( 0, "CreateProcess failed: %lu.\n", GetLastError() );
        return code;
    }
    if (WaitForSingleObject( pi.hProcess, 120000 ))
    {
        ok( 0, "the child did not finish.\n" );
        TerminateProcess( pi.hProcess, code );   /* the test's own child: not left running */
        WaitForSingleObject( pi.hProcess, 5000 );
    }
    GetExitCodeProcess( pi.hProcess, &code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    return code;
}

/* runs the child with a token; its exit code */
static DWORD run_child( HANDLE token )
{
    return run_child_args( token, "broker_child" );
}

/* The child of the process-scope test: asks FindAccountAsync for the account ID (a narrow string, the argument),
 * after using the test's client id itself (FindAllAccounts) when USE_CLIENT; the exit code is 1 when the broker
 * answered with the account and 0 when with none. */
static void broker_find_child( const char *id_text, BOOL use_client )
{
    IWebAuthenticationCoreManagerStatics4 *statics4;
    IWebAuthenticationCoreManagerStatics *statics;
    IWebAccountProvider *provider;
    IActivationFactory *factory;
    WCHAR id[64];
    DWORD code = 2;

    MultiByteToWideChar( CP_ACP, 0, id_text, -1, id, ARRAY_SIZE(id) );
    if ((factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager" )))
    {
        IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics, (void **)&statics );
        IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics4, (void **)&statics4 );
        IActivationFactory_Release( factory );
        if ((provider = broker_provider( statics, TEST_PROVIDER, L"consumers" )))
        {
            if (use_client) broker_accounts( statics4, provider, NULL );
            code = broker_find_account( statics, provider, id, NULL, 0 ) ? 1 : 0;
            IWebAccountProvider_Release( provider );
        }
        IWebAuthenticationCoreManagerStatics4_Release( statics4 );
        IWebAuthenticationCoreManagerStatics_Release( statics );
    }
    RoUninitialize();
    ExitProcess( code );
}

/* the user's broker pipe name */
static void broker_pipe_name( WCHAR *name, UINT size )
{
    char buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)buffer;
    WCHAR *sid = NULL;
    HANDLE token;
    DWORD len;

    name[0] = 0;
    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token )) return;
    if (GetTokenInformation( token, TokenUser, buffer, sizeof(buffer), &len ) &&
        ConvertSidToStringSidW( user->User.Sid, &sid ))
        swprintf( name, size, L"\\\\.\\pipe\\WineTokenBroker\\%s", sid );
    LocalFree( sid );
    CloseHandle( token );
}

/* the child of test_broker_impostor (B86): creates the broker's pipe before any broker runs, waits for a client and
 * then for its request; the exit code is 3 when a request came (the client took the impostor for the broker), 1 when
 * the client connected and left without one (it refused the server), 0 when no client came, 2 when the pipe could
 * not be made */
static void broker_impostor(void)
{
    OVERLAPPED ov = { 0 };
    WCHAR name[256];
    BYTE request[4096];
    BOOL connected = FALSE;
    HANDLE pipe;
    DWORD code = 2, n;

    broker_pipe_name( name, ARRAY_SIZE(name) );
    ov.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL );
    pipe = CreateNamedPipeW( name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
                             PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL );
    if (name[0] && pipe != INVALID_HANDLE_VALUE)
    {
        code = 0;
        if (ConnectNamedPipe( pipe, &ov ) || GetLastError() == ERROR_PIPE_CONNECTED) connected = TRUE;
        else if (GetLastError() == ERROR_IO_PENDING && !WaitForSingleObject( ov.hEvent, 90000 ) &&
                 GetOverlappedResult( pipe, &ov, &n, FALSE )) connected = TRUE;
        if (connected)
        {
            /* a refusing client closes the pipe without writing: a broken pipe, or nothing within 5 seconds */
            code = 1;
            ResetEvent( ov.hEvent );
            if (ReadFile( pipe, request, sizeof(request), &n, &ov ) ? n > 0 : GetLastError() == ERROR_MORE_DATA)
                code = 3;
            else if (GetLastError() == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject( ov.hEvent, 5000 ))
                {
                    CancelIo( pipe );
                    WaitForSingleObject( ov.hEvent, 5000 );
                }
                else if (GetOverlappedResult( pipe, &ov, &n, FALSE ) ? n > 0 : GetLastError() == ERROR_MORE_DATA)
                    code = 3;
            }
        }
        CloseHandle( pipe );
    }
    ExitProcess( code );
}

/* waits up to 10 seconds for the user's broker pipe to exist */
static BOOL wait_pipe( const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < 200; i++)
    {
        if (WaitNamedPipeW( name, 0 ) || GetLastError() != ERROR_FILE_NOT_FOUND) return TRUE;
        Sleep( 50 );
    }
    return FALSE;
}

/* B86: a sandboxed process of the user (here below medium integrity) that creates the broker's pipe before the broker
 * runs is refused by the client: its request is not answered by the impostor */
static void test_broker_impostor(void)
{
    static SID low = { SID_REVISION, 1, { SECURITY_MANDATORY_LABEL_AUTHORITY }, { SECURITY_MANDATORY_LOW_RID } };
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    TOKEN_MANDATORY_LABEL label;
    HANDLE token, low_token;
    char cmd[MAX_PATH * 2], **argv;
    WCHAR name[256];
    DWORD code = 0xdeadbeef, i;

    broker_pipe_name( name, ARRAY_SIZE(name) );
    if (!name[0] || WaitNamedPipeW( name, 0 ) || GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        skip( "the impostor test needs a user without a running token broker.\n" );
        return;
    }
    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_ALL_ACCESS, &token )) return;
    if (!DuplicateTokenEx( token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &low_token ))
    {
        ok( 0, "DuplicateTokenEx failed: %lu.\n", GetLastError() );
        CloseHandle( token );
        return;
    }
    CloseHandle( token );
    label.Label.Sid = &low;
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    ok( SetTokenInformation( low_token, TokenIntegrityLevel, &label, sizeof(label) ), "got error %lu.\n",
        GetLastError() );
    winetest_get_mainargs( &argv );
    sprintf( cmd, "\"%s\" web_core broker_impostor", argv[0] );
    if (!CreateProcessAsUserA( low_token, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        ok( 0, "CreateProcessAsUser failed: %lu.\n", GetLastError() );
        CloseHandle( low_token );
        return;
    }
    /* another image: the client connects and leaves without writing its request (1); 3 is the bug */
    ok( wait_pipe( name ), "the impostor's pipe did not appear.\n" );
    run_child( NULL );
    if (WaitForSingleObject( pi.hProcess, 120000 ))
    {
        ok( 0, "the impostor did not finish.\n" );
        TerminateProcess( pi.hProcess, 0 );
        WaitForSingleObject( pi.hProcess, 5000 );
    }
    GetExitCodeProcess( pi.hProcess, &code );
    ok( code == 1, "the impostor's pipe: got %lu (3: the client sent its request to it).\n", code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );

    /* the real image with a sandbox's token: only the token check refuses it (the client logs "with a sandbox's
     * token", which the suite looks for with WINEDEBUG=warn+webcore) */
    i = GetSystemDirectoryA( cmd, MAX_PATH );
    strcpy( cmd + i, "\\tokenbroker.exe" );
    if (!CreateProcessAsUserA( low_token, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
        skip( "tokenbroker.exe does not start with a low integrity token: %lu.\n", GetLastError() );
    else
    {
        if (!wait_pipe( name )) skip( "tokenbroker.exe made no pipe with a low integrity token.\n" );
        else
        {
            /* the suite requires the client's "with a sandbox's token" warning after this line */
            trace( "B86: the low integrity token broker runs\n" );
            code = run_child( NULL );
            ok( (code >> 16) != WebTokenRequestStatus_Success, "the low integrity broker served the request: %#lx.\n",
                code );
        }
        TerminateProcess( pi.hProcess, 0 );
        WaitForSingleObject( pi.hProcess, 10000 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
        for (i = 0; i < 100 && (WaitNamedPipeW( name, 0 ) || GetLastError() != ERROR_FILE_NOT_FOUND); i++) Sleep( 50 );
    }
    CloseHandle( low_token );
}

/* sandboxed callers get no token, even of an account the user has: a restricted token, a low integrity token */
static void test_broker_sandboxed(void)
{
    static SID low = { SID_REVISION, 1, { SECURITY_MANDATORY_LABEL_AUTHORITY }, { SECURITY_MANDATORY_LOW_RID } };
    char buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)buffer;
    static SID_IDENTIFIER_AUTHORITY world = { SECURITY_WORLD_SID_AUTHORITY }, nt = { SECURITY_NT_AUTHORITY };
    TOKEN_MANDATORY_LABEL label;
    SID_AND_ATTRIBUTES restrict_sids[5];
    HANDLE token, restricted, low_token;
    DWORD len, code, i;
    BOOL ok_restricted;

    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_ALL_ACCESS, &token ))
    {
        ok( 0, "OpenProcessToken failed: %lu.\n", GetLastError() );
        return;
    }
    GetTokenInformation( token, TokenUser, buffer, sizeof(buffer), &len );

    /* restricted: its restricting SIDs are the user's own and the groups the machine's registry grants reading to
     * (Everyone, Users, Authenticated Users, RESTRICTED), as a sandbox's token carries, so that the child can still
     * activate the class (the activation registry is readable by Users, not by the user's own SID) and only the
     * broker's own check refuses it */
    restrict_sids[0].Sid = user->User.Sid;
    AllocateAndInitializeSid( &world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &restrict_sids[1].Sid );
    AllocateAndInitializeSid( &nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0,
                              &restrict_sids[2].Sid );
    AllocateAndInitializeSid( &nt, 1, SECURITY_AUTHENTICATED_USER_RID, 0, 0, 0, 0, 0, 0, 0, &restrict_sids[3].Sid );
    AllocateAndInitializeSid( &nt, 1, SECURITY_RESTRICTED_CODE_RID, 0, 0, 0, 0, 0, 0, 0, &restrict_sids[4].Sid );
    for (i = 0; i < ARRAY_SIZE(restrict_sids); i++) restrict_sids[i].Attributes = 0;
    ok_restricted = CreateRestrictedToken( token, 0, 0, NULL, 0, NULL, ARRAY_SIZE(restrict_sids), restrict_sids,
                                           &restricted );
    for (i = 1; i < ARRAY_SIZE(restrict_sids); i++) FreeSid( restrict_sids[i].Sid );
    if (ok_restricted)
    {
        ok( IsTokenRestricted( restricted ), "not restricted.\n" );
        code = run_child( restricted );
        ok( code == ((WebTokenRequestStatus_ProviderError << 16) | 0x0005), "restricted caller: got %#lx.\n", code );
        CloseHandle( restricted );
    }
    else ok( 0, "CreateRestrictedToken failed: %lu.\n", GetLastError() );

    /* below medium integrity */
    if (DuplicateTokenEx( token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &low_token ))
    {
        label.Label.Sid = &low;
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        ok( SetTokenInformation( low_token, TokenIntegrityLevel, &label, sizeof(label) ), "got error %lu.\n",
            GetLastError() );
        code = run_child( low_token );
        ok( code == ((WebTokenRequestStatus_ProviderError << 16) | 0x0005), "low integrity caller: got %#lx.\n", code );
        CloseHandle( low_token );
    }
    else ok( 0, "DuplicateTokenEx failed: %lu.\n", GetLastError() );

    /* the same user's ordinary token is served */
    code = run_child( token );
    ok( (code >> 16) == WebTokenRequestStatus_Success, "medium caller: got %#lx.\n", code );
    CloseHandle( token );
}

static void test_broker(void)
{
    IWebAuthenticationCoreManagerStatics4 *statics4;
    IWebAuthenticationCoreManagerStatics *statics;
    IWebAuthenticationCoreManagerInterop *interop;
    IWebAccountProvider *provider, *other;
    IWebAccount *account = NULL;
    IActivationFactory *factory;
    struct broker_result r;
    static const WCHAR *saved_names[] = { L"Broker", L"TestAuthority", L"TestWindow" };
    struct saved_setting saved[ARRAY_SIZE(saved_names)];
    WCHAR authority[256], first_token[128], nested_token[1024], found_id[128];
    DWORD size = sizeof(authority), one = 1, code;
    const WCHAR *p1, *p2, *p3, *p4;
    unsigned int i;
    HWND window;
    HRESULT hr;
    HKEY key;

    if (!winetest_platform_is_wine || !(size = GetEnvironmentVariableW( L"WEBCORE_TEST_AUTHORITY", authority,
                                                                        ARRAY_SIZE(authority) )) ||
        size >= ARRAY_SIZE(authority))
    {
        skip( "the token broker's test provider: no WEBCORE_TEST_AUTHORITY.\n" );
        return;
    }
    for (i = 0; i < ARRAY_SIZE(saved); i++) save_setting( &saved[i], saved_names[i] );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL ))
        return;
    RegSetValueExW( key, L"Broker", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    RegSetValueExW( key, L"TestAuthority", 0, REG_SZ, (BYTE *)authority, (wcslen( authority ) + 1) * sizeof(WCHAR) );
    RegDeleteValueW( key, L"TestWindow" );
    RegCloseKey( key );
    if (!(factory = get_factory( L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager" )))
        goto restore;
    hr = IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerStatics4, (void **)&statics4 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IActivationFactory_QueryInterface( factory, &IID_IWebAuthenticationCoreManagerInterop, (void **)&interop );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IActivationFactory_Release( factory );
    window = CreateWindowExW( 0, L"static", L"web_core broker test", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                              NULL, NULL, NULL, NULL );
    ok( window != NULL, "no window.\n" );

    provider = broker_provider( statics, TEST_PROVIDER, L"consumers" );
    if (!provider) goto done;
    test_broker_impostor();

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
    /* a Microsoft account (the consumers authority) is not "Connected"; without the batch property the answer has the
     * wamcompat_* properties, no telemetry (nobody asked for it) and the account's own properties */
    ok( r.state == WebAccountState_None, "got account state %d.\n", r.state );
    ok( r.wamcompat, "no wamcompat_expires_on.\n" );
    ok( !wcscmp( r.wamcompat_account, L"oid-ok" ), "got wamcompat_account_id %s.\n", wine_dbgstr_w( r.wamcompat_account ) );
    ok( !r.mats, "got the telemetry property MATS without being asked.\n" );
    ok( r.account_props == 6, "got %u account properties.\n", r.account_props );
    ok( !wcscmp( r.display_name, L"Test ok" ), "got DisplayName %s.\n", wine_dbgstr_w( r.display_name ) );
    ok( !wcscmp( r.first_name, L"Test" ), "got FirstName %s.\n", wine_dbgstr_w( r.first_name ) );
    ok( !wcscmp( r.last_name, L"ok" ), "got LastName %s.\n", wine_dbgstr_w( r.last_name ) );
    ok( !wcscmp( r.uid, L"oid-ok" ), "got UID %s.\n", wine_dbgstr_w( r.uid ) );
    ok( !wcscmp( r.roaming, L"false" ), "got IsRoamingEncryptionKeyAvailable %s.\n", wine_dbgstr_w( r.roaming ) );
    wcscpy( first_token, r.token );

    /* sandboxed callers are refused although the account exists; the same user's ordinary token is served */
    test_broker_sandboxed();

    /* the account is listed and found */
    ok( broker_accounts( statics4, provider, &account ) == 1, "wrong account count.\n" );
    ok( account != NULL, "no account.\n" );
    /* FindAccountAsync names no client id: this process has used the test's client id, so it finds the account; a
     * process that has not used it finds nothing, one that has used it finds the account too (the accounts of a client
     * id are the processes' that used it, not every process's) */
    ok( broker_find_account( statics, provider, L"oid-ok", found_id, ARRAY_SIZE(found_id) ),
        "FindAccountAsync did not find the account of a client id this process used.\n" );
    ok( !wcscmp( found_id, L"oid-ok" ), "got account id %s.\n", wine_dbgstr_w( found_id ) );
    ok( !broker_find_account( statics, provider, L"oid-nobody", NULL, 0 ), "found an account that is not there.\n" );
    code = run_child_args( NULL, "broker_find_child oid-ok no_client" );
    ok( code == 0, "another process that never used the client id: got %#lx.\n", code );
    code = run_child_args( NULL, "broker_find_child oid-ok use_client" );
    ok( code == 1, "another process that used the client id itself: got %#lx.\n", code );

    /* silent: the same scope from the cache, another one by refresh token */
    broker_silent( statics, provider, NULL, L"user.read", &r );
    ok( r.status == WebTokenRequestStatus_Success, "got status %d, error %#x.\n", r.status, r.error );
    ok( !wcscmp( r.token, first_token ), "got token %s, not the cached %s.\n", wine_dbgstr_w( r.token ),
        wine_dbgstr_w( first_token ) );
    broker_silent( statics, provider, account, L"files.read", &r );
    ok( r.status == WebTokenRequestStatus_Success, "got status %d, error %#x.\n", r.status, r.error );
    ok( r.token[0] && wcscmp( r.token, first_token ), "got token %s.\n", wine_dbgstr_w( r.token ) );
    ok( !wcscmp( r.user_name, L"ok@wine.test" ), "got user name %s.\n", wine_dbgstr_w( r.user_name ) );

    /* oauth2_batch=1: the Token is the token endpoint's answer as a form body, in this order; no wamcompat_*
     * properties; the telemetry property MATS when asked for (telemetry=MATS) */
    {
        const WCHAR *batch[] = { L"api-version", L"2.0", L"oauth2_batch", L"1", L"telemetry", L"MATS", NULL };
        const WCHAR *nobatch[] = { L"api-version", L"2.0", L"telemetry", L"MATS", NULL };

        broker_silent_ex( statics, provider, NULL, L"user.read", NULL, batch, &r );
        ok( r.status == WebTokenRequestStatus_Success, "batch: got status %d, error %#x.\n", r.status, r.error );
        ok( !wcsncmp( r.token, L"access_token=at-", 16 ), "batch: got token %s.\n", wine_dbgstr_w( r.token ) );
        p1 = wcsstr( r.token, L"&token_type=Bearer&expires_in=" );
        p2 = p1 ? wcsstr( p1, L"&scope=" ) : NULL;
        p3 = p2 ? wcsstr( p2, L"&id_token=" ) : NULL;
        p4 = p3 ? wcsstr( p3, L"&client_info=" ) : NULL;
        ok( p1 && p2 && p3 && p4, "batch: the fields are missing or out of order in %s.\n", wine_dbgstr_w( r.token ) );
        ok( !r.wamcompat && !r.id_token, "batch: got wamcompat_* properties.\n" );
        ok( r.mats, "batch: no MATS.\n" );
        ok( r.account_props == 6, "batch: got %u account properties.\n", r.account_props );

        /* the same without the batch property: the bare token and the wamcompat_* properties, MATS still on request */
        broker_silent_ex( statics, provider, NULL, L"user.read", NULL, nobatch, &r );
        ok( r.status == WebTokenRequestStatus_Success, "no batch: got status %d.\n", r.status );
        ok( !wcsncmp( r.token, L"at-", 3 ), "no batch: got token %s.\n", wine_dbgstr_w( r.token ) );
        ok( r.wamcompat && r.id_token, "no batch: no wamcompat_* properties.\n" );
        ok( r.mats, "no batch: no MATS.\n" );
    }

    /* Nested App Authentication: a token for the child app, through the parent: the authority names it "nat-N"; it is
     * cached under the child's client id (the same token again), and the parent's refresh token is untouched (the
     * parent's own silent request for the same scope still works, with a token of its own) */
    {
        const WCHAR *naa[] = { L"child_client_id", CHILD_CLIENT, L"child_redirect_uri", L"brk-test://child", NULL };

        broker_silent_ex( statics, provider, account, L"child.read", NULL, naa, &r );
        ok( r.status == WebTokenRequestStatus_Success, "nested: got status %d, error %#x.\n", r.status, r.error );
        ok( !wcsncmp( r.token, L"nat-", 4 ), "nested: got token %s.\n", wine_dbgstr_w( r.token ) );
        wcscpy( nested_token, r.token );
        broker_silent_ex( statics, provider, account, L"child.read", NULL, naa, &r );
        ok( r.status == WebTokenRequestStatus_Success, "nested again: got status %d.\n", r.status );
        ok( !wcscmp( r.token, nested_token ), "nested again: got token %s, not the cached %s.\n",
            wine_dbgstr_w( r.token ), wine_dbgstr_w( nested_token ) );
        broker_silent( statics, provider, account, L"child.read", &r );
        ok( r.status == WebTokenRequestStatus_Success, "parent after nested: got status %d, error %#x.\n", r.status, r.error );
        ok( !wcsncmp( r.token, L"at-", 3 ), "parent after nested: got token %s.\n", wine_dbgstr_w( r.token ) );
        ok( !wcscmp( r.account_id, L"oid-ok" ), "parent after nested: got account id %s.\n", wine_dbgstr_w( r.account_id ) );
    }

    /* another client id of the same program, which has no account of its own: it uses the account of the client id the
     * program has used, through it (a brokered request: the authority names the token "nat-N") */
    broker_silent_ex( statics, provider, NULL, L"user.read", OTHER_CLIENT, NULL, &r );
    ok( r.status == WebTokenRequestStatus_Success, "other client: got status %d, error %#x.\n", r.status, r.error );
    ok( !wcsncmp( r.token, L"nat-", 4 ), "other client: got token %s.\n", wine_dbgstr_w( r.token ) );
    ok( !wcscmp( r.account_id, L"oid-ok" ), "other client: got account id %s.\n", wine_dbgstr_w( r.account_id ) );
    /* but another process that asks for that other client id, without ever having used the account's, is not served
     * through this process's use of it: the user is needed */
    code = run_child_args( NULL, "broker_child_other" );
    ok( code == ((WebTokenRequestStatus_UserInteractionRequired << 16) | 0x0006), "another process, other client id: got %#lx.\n",
        code );

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

    /* a Microsoft account: its object id is zeros and the 16 digits of its customer id; WAM names the account by those
     * digits, upper case (WebAccount.Id and its UID), and keeps the customer id, lower case, as SafeCustomerId; the
     * wamcompat_account_id keeps the object id's form; the account has seven properties (no country or age group:
     * the id token has none) and is found by its id */
    broker_interactive( interop, window, provider, L"user.read", L"msa@wine.test", &r );
    ok( r.status == WebTokenRequestStatus_Success, "msa: got status %d, error %#x.\n", r.status, r.error );
    ok( !wcscmp( r.account_id, L"E659DC1079E4A963" ), "msa: got account id %s.\n", wine_dbgstr_w( r.account_id ) );
    ok( !wcscmp( r.uid, L"E659DC1079E4A963" ), "msa: got UID %s.\n", wine_dbgstr_w( r.uid ) );
    ok( !wcscmp( r.safe_id, L"e659dc1079e4a963" ), "msa: got SafeCustomerId %s.\n", wine_dbgstr_w( r.safe_id ) );
    ok( !wcscmp( r.wamcompat_account, L"00000000-0000-0000-e659-dc1079e4a963" ), "msa: got wamcompat_account_id %s.\n",
        wine_dbgstr_w( r.wamcompat_account ) );
    ok( !wcscmp( r.display_name, L"Test msa" ), "msa: got DisplayName %s.\n", wine_dbgstr_w( r.display_name ) );
    ok( !wcscmp( r.first_name, L"Test" ), "msa: got FirstName %s.\n", wine_dbgstr_w( r.first_name ) );
    ok( !wcscmp( r.last_name, L"msa" ), "msa: got LastName %s.\n", wine_dbgstr_w( r.last_name ) );
    ok( r.account_props == 7, "msa: got %u account properties.\n", r.account_props );
    ok( r.state == WebAccountState_None, "msa: got account state %d.\n", r.state );
    ok( broker_find_account( statics, provider, L"E659DC1079E4A963", found_id, ARRAY_SIZE(found_id) ),
        "msa: the account is not found by its id.\n" );
    ok( !wcscmp( found_id, L"E659DC1079E4A963" ), "msa: found account id %s.\n", wine_dbgstr_w( found_id ) );
    ok( broker_accounts( statics4, provider, &account ) == 1, "msa: wrong account count.\n" );
    if (account) IWebAccount_Release( account );
    /* silent requests for it work with the id the program got */
    broker_silent( statics, provider, NULL, L"files.read", &r );
    ok( r.status == WebTokenRequestStatus_Success, "msa silent: got status %d, error %#x.\n", r.status, r.error );
    ok( !wcscmp( r.account_id, L"E659DC1079E4A963" ), "msa silent: got account id %s.\n", wine_dbgstr_w( r.account_id ) );

    /* an organization's account is Connected */
    if ((other = broker_provider( statics, TEST_PROVIDER, L"organizations" )))
    {
        broker_interactive( interop, window, other, L"user.read", L"org@wine.test", &r );
        ok( r.status == WebTokenRequestStatus_Success, "organization: got status %d, error %#x.\n", r.status, r.error );
        ok( r.state == WebAccountState_Connected, "organization: got account state %d.\n", r.state );
        ok( !wcscmp( r.account_id, L"oid-org" ), "organization: got account id %s.\n", wine_dbgstr_w( r.account_id ) );

        /* WebAccount.SignOutAsync without a client id: the account's tokens of the client ids this program used go,
         * and the account is no longer found */
        ok( broker_accounts( statics4, other, &account ) == 1, "organization: wrong account count.\n" );
        if (account)
        {
            IAsyncAction *action;
            IWebAccount2 *account2;

            hr = IWebAccount_QueryInterface( account, &IID_IWebAccount2, (void **)&account2 );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            hr = IWebAccount2_SignOutAsync( account2, &action );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            if (SUCCEEDED(hr))
            {
                wait_broker( (IUnknown *)action );
                hr = IAsyncAction_GetResults( action );
                ok( hr == S_OK, "got hr %#lx.\n", hr );
                IAsyncAction_Release( action );
            }
            IWebAccount2_Release( account2 );
            IWebAccount_Release( account );
            ok( !broker_find_account( statics, other, L"oid-org", NULL, 0 ),
                "organization: the account is still found after the sign-out.\n" );
            ok( broker_accounts( statics4, other, NULL ) == 0, "organization: the account is still listed.\n" );
            broker_silent( statics, other, NULL, L"user.read", &r );
            ok( r.status == WebTokenRequestStatus_UserInteractionRequired,
                "organization after the sign-out: got status %d.\n", r.status );
        }
        IWebAccountProvider_Release( other );
    }
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
    DestroyWindow( window );
    IWebAuthenticationCoreManagerInterop_Release( interop );
    IWebAuthenticationCoreManagerStatics4_Release( statics4 );
    IWebAuthenticationCoreManagerStatics_Release( statics );
restore:
    for (i = 0; i < ARRAY_SIZE(saved); i++) restore_setting( &saved[i] );
}

START_TEST(web_core)
{
    char **argv;
    HRESULT hr;
    int argc;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    argc = winetest_get_mainargs( &argv );
    if (argc >= 3 && !strcmp( argv[2], "broker_child" ))
    {
        broker_child( FALSE );
        return;
    }
    if (argc >= 3 && !strcmp( argv[2], "broker_child_other" ))
    {
        broker_child( TRUE );
        return;
    }
    if (argc >= 3 && !strcmp( argv[2], "broker_impostor" ))
    {
        broker_impostor();
        return;
    }
    if (argc >= 5 && !strcmp( argv[2], "broker_find_child" ))
    {
        broker_find_child( argv[3], !strcmp( argv[4], "use_client" ) );
        return;
    }

    test_manager();
    test_diagnostic_provider();
    test_broker();

    RoUninitialize();
}
