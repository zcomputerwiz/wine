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
#include "winstring.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Security_EnterpriseData
#include "windows.security.enterprisedata.h"

#include "wine/test.h"

static HRESULT WINAPI handler_QueryInterface( IEventHandler_IInspectable *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IEventHandler_IInspectable ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI handler_AddRef( IEventHandler_IInspectable *iface ) { return 2; }
static ULONG WINAPI handler_Release( IEventHandler_IInspectable *iface ) { return 1; }

static HRESULT WINAPI handler_Invoke( IEventHandler_IInspectable *iface, IInspectable *sender, IInspectable *args )
{
    return S_OK;
}

static const IEventHandler_IInspectableVtbl handler_vtbl =
{
    handler_QueryInterface,
    handler_AddRef,
    handler_Release,
    handler_Invoke,
};

static IEventHandler_IInspectable handler = { &handler_vtbl };

static HSTRING hstr( const WCHAR *str )
{
    HSTRING ret = NULL;
    HRESULT hr = WindowsCreateString( str, wcslen( str ), &ret );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    return ret;
}

static void test_factory( const WCHAR *name, BOOL private_pt )
{
    IProtectionPolicyManagerStatics4 *statics4;
    IProtectionPolicyManagerStatics3 *statics3;
    IProtectionPolicyManagerStatics2 *statics2;
    IProtectionPolicyManagerStatics *statics;
    IActivationFactory *factory = NULL;
    ProtectionPolicyEvaluationResult access;
    EnforcementLevel level;
    EventRegistrationToken token;
    HSTRING id, other, empty;
    TrustLevel trust;
    DateTime since;
    boolean value;
    ULONG count, i;
    HSTRING res;
    IID *iids;
    HRESULT hr;
    BOOL found[5] = {0};
    HRESULT unsupported = private_pt ? HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED ) : S_OK;

    winetest_push_context( "%s", wine_dbgstr_w( name ) );

    id = hstr( name );
    hr = RoGetActivationFactory( id, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( id );
    if (FAILED( hr ))
    {
        win_skip( "%s is not available, hr %#lx.\n", debugstr_w( name ), hr );
        winetest_pop_context();
        return;
    }

    id = hstr( L"someone@example.com" );
    other = hstr( L"other@example.com" );
    empty = hstr( L"" );

    hr = IActivationFactory_GetTrustLevel( factory, &trust );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( trust == (private_pt ? PartialTrust : BaseTrust), "got trust %d.\n", trust );

    hr = IActivationFactory_GetIids( factory, &count, &iids );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( count == (private_pt ? 3 : 5), "got count %lu.\n", count );
    for (i = 0; i < count; i++)
    {
        if (IsEqualGUID( &iids[i], &IID_IActivationFactory )) found[0] = TRUE;
        if (IsEqualGUID( &iids[i], &IID_IProtectionPolicyManagerStatics )) found[1] = TRUE;
        if (IsEqualGUID( &iids[i], &IID_IProtectionPolicyManagerStatics2 )) found[2] = TRUE;
        if (IsEqualGUID( &iids[i], &IID_IProtectionPolicyManagerStatics3 )) found[3] = TRUE;
        if (IsEqualGUID( &iids[i], &IID_IProtectionPolicyManagerStatics4 )) found[4] = TRUE;
    }
    CoTaskMemFree( iids );
    ok( found[0] && found[1] && found[2], "got %d %d %d.\n", found[0], found[1], found[2] );
    ok( found[3] == !private_pt && found[4] == !private_pt, "got %d %d.\n", found[3], found[4] );

    hr = IActivationFactory_QueryInterface( factory, &IID_IProtectionPolicyManagerStatics3, (void **)&statics3 );
    ok( hr == (private_pt ? E_NOINTERFACE : S_OK), "got hr %#lx.\n", hr );
    if (SUCCEEDED( hr )) IProtectionPolicyManagerStatics3_Release( statics3 );
    hr = IActivationFactory_QueryInterface( factory, &IID_IProtectionPolicyManagerStatics4, (void **)&statics4 );
    ok( hr == (private_pt ? E_NOINTERFACE : S_OK), "got hr %#lx.\n", hr );
    if (SUCCEEDED( hr )) IProtectionPolicyManagerStatics4_Release( statics4 );

    hr = IActivationFactory_QueryInterface( factory, &IID_IProtectionPolicyManagerStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    /* a machine without enterprise data protection policy */
    value = 0xcc;
    hr = IProtectionPolicyManagerStatics_IsIdentityManaged( statics, id, &value );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( value == (private_pt ? 0xcc : FALSE), "got %d.\n", value );
    hr = IProtectionPolicyManagerStatics_IsIdentityManaged( statics, empty, &value );
    ok( hr == (private_pt ? unsupported : E_INVALIDARG), "got hr %#lx.\n", hr );
    hr = IProtectionPolicyManagerStatics_TryApplyProcessUIPolicy( statics, id, &value );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( value == (private_pt ? 0xcc : FALSE), "got %d.\n", value );
    hr = IProtectionPolicyManagerStatics_TryApplyProcessUIPolicy( statics, empty, &value );
    ok( hr == (private_pt ? unsupported : E_INVALIDARG), "got hr %#lx.\n", hr );
    hr = IProtectionPolicyManagerStatics_ClearProcessUIPolicy( statics );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    hr = IProtectionPolicyManagerStatics_RevokeContent( statics, id );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IProtectionPolicyManagerStatics_RevokeContent( statics, empty );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    access = 0xcc;
    hr = IProtectionPolicyManagerStatics_CheckAccess( statics, id, other, &access );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( access == (private_pt ? 0xcc : ProtectionPolicyEvaluationResult_Allowed), "got %d.\n", access );
    access = 0xcc;
    hr = IProtectionPolicyManagerStatics_CheckAccess( statics, empty, empty, &access );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( access == (private_pt ? 0xcc : ProtectionPolicyEvaluationResult_Allowed), "got %d.\n", access );
    token.value = 0;
    hr = IProtectionPolicyManagerStatics_add_ProtectedContentRevoked( statics, &handler, &token );
    ok( hr == S_OK || broken( hr == E_NOINTERFACE ), "got hr %#lx.\n", hr );
    hr = IProtectionPolicyManagerStatics_remove_ProtectedContentRevoked( statics, token );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IProtectionPolicyManagerStatics_Release( statics );

    hr = IActivationFactory_QueryInterface( factory, &IID_IProtectionPolicyManagerStatics2, (void **)&statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    value = 0xcc;
    hr = IProtectionPolicyManagerStatics2_get_IsProtectionEnabled( statics2, &value );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( value == (private_pt ? 0xcc : FALSE), "got %d.\n", value );
    level = 0xcc;
    hr = IProtectionPolicyManagerStatics2_GetEnforcementLevel( statics2, id, &level );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( level == (private_pt ? 0xcc : EnforcementLevel_NoProtection), "got %d.\n", level );
    value = 0xcc;
    hr = IProtectionPolicyManagerStatics2_IsUserDecryptionAllowed( statics2, id, &value );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( value == (private_pt ? 0xcc : TRUE), "got %d.\n", value );
    value = 0xcc;
    hr = IProtectionPolicyManagerStatics2_IsProtectionUnderLockRequired( statics2, id, &value );
    ok( hr == unsupported, "got hr %#lx.\n", hr );
    ok( value == (private_pt ? 0xcc : FALSE), "got %d.\n", value );
    access = 0xcc;
    hr = IProtectionPolicyManagerStatics2_CheckAccessForApp( statics2, id, hstr( L"Microsoft.WindowsNotepad_8wekyb3d8bbwe" ), &access );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( access == ProtectionPolicyEvaluationResult_Allowed, "got %d.\n", access );
    hr = IProtectionPolicyManagerStatics2_CheckAccessForApp( statics2, id, empty, &access );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    since.UniversalTime = 0x01d9a2c3d3a3c000ll;
    value = 0xcc;
    hr = IProtectionPolicyManagerStatics2_HasContentBeenRevokedSince( statics2, id, since, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( value == TRUE, "got %d.\n", value );
    token.value = 0;
    hr = IProtectionPolicyManagerStatics2_add_PolicyChanged( statics2, &handler, &token );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( token.value != 0, "got a zero token.\n" );
    hr = IProtectionPolicyManagerStatics2_remove_PolicyChanged( statics2, token );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IProtectionPolicyManagerStatics2_Release( statics2 );

    if (!private_pt)
    {
        hr = IActivationFactory_QueryInterface( factory, &IID_IProtectionPolicyManagerStatics4, (void **)&statics4 );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        value = 0xcc;
        hr = IProtectionPolicyManagerStatics4_IsRoamableProtectionEnabled( statics4, id, &value );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( value == FALSE, "got %d.\n", value );
        hr = IProtectionPolicyManagerStatics4_IsRoamableProtectionEnabled( statics4, empty, &value );
        ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
        res = NULL;
        hr = IProtectionPolicyManagerStatics4_get_PrimaryManagedIdentity( statics4, &res );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( WindowsIsStringEmpty( res ), "got %s.\n", debugstr_hstring( res ) );
        WindowsDeleteString( res );
        IProtectionPolicyManagerStatics4_Release( statics4 );
    }

    IActivationFactory_Release( factory );
    WindowsDeleteString( id );
    WindowsDeleteString( other );
    WindowsDeleteString( empty );
    winetest_pop_context();
}

START_TEST(enterprisedata)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_factory( L"Windows.Security.EnterpriseData.ProtectionPolicyManager", FALSE );
    test_factory( L"Windows.Security.EnterpriseData.ProtectionPolicyManagerPrivatePT", TRUE );

    RoUninitialize();
}
