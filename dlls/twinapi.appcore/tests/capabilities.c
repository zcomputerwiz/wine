/*
 * Tests for Windows.Devices.Input capabilities and Windows.System.Display.DisplayRequest
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

#include <stdarg.h>
#include <stddef.h>

/* IAppCapability2 is of contract version 15 (the default of the headers is 14) */
#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0xf0000

#include "windef.h"
#include "winbase.h"
#include "winuser.h"

#define COBJMACROS
#include "winstring.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_System
#include "windows.foundation.h"
#define WIDL_using_Windows_Devices_Input
#include "windows.devices.input.h"
#define WIDL_using_Windows_System_Display
#include "windows.system.display.h"
#define WIDL_using_Windows_Web_Http
#include "windows.web.http.h"
#define WIDL_using_Windows_System_Power
#include "windows.system.power.h"
#define WIDL_using_Windows_Security_Authorization_AppCapabilityAccess
#include "windows.security.authorization.appcapabilityaccess.h"

#include "wine/test.h"

static IInspectable *activate( const WCHAR *class_name )
{
    IInspectable *instance = NULL;
    HSTRING str;
    HRESULT hr;

    WindowsCreateString( class_name, wcslen( class_name ), &str );
    hr = RoActivateInstance( str, &instance );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "%s: got hr %#lx.\n", debugstr_w( class_name ), hr );
    return instance;
}

static void test_capabilities(void)
{
    IKeyboardCapabilities *keyboard;
    IMouseCapabilities *mouse;
    ITouchCapabilities *touch;
    IInspectable *instance;
    UINT32 count;
    INT32 value;
    HSTRING name;
    HRESULT hr;

    if (!(instance = activate( L"Windows.Devices.Input.KeyboardCapabilities" )))
    {
        win_skip( "KeyboardCapabilities runtimeclass not registered, skipping tests.\n" );
        return;
    }
    hr = IInspectable_QueryInterface( instance, &IID_IKeyboardCapabilities, (void **)&keyboard );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( instance );
    hr = IKeyboardCapabilities_get_KeyboardPresent( keyboard, &value );
    ok( hr == S_OK && value == (GetKeyboardType( 0 ) != 0), "got hr %#lx, value %d.\n", hr, value );
    hr = IKeyboardCapabilities_GetRuntimeClassName( keyboard, &name );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( name, NULL ), L"Windows.Devices.Input.KeyboardCapabilities" ),
        "got hr %#lx.\n", hr );
    WindowsDeleteString( name );
    IKeyboardCapabilities_Release( keyboard );

    if (!(instance = activate( L"Windows.Devices.Input.MouseCapabilities" ))) return;
    hr = IInspectable_QueryInterface( instance, &IID_IMouseCapabilities, (void **)&mouse );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( instance );
    hr = IMouseCapabilities_get_MousePresent( mouse, &value );
    ok( hr == S_OK && value == (GetSystemMetrics( SM_MOUSEPRESENT ) != 0), "got hr %#lx, value %d.\n", hr, value );
    hr = IMouseCapabilities_get_NumberOfButtons( mouse, &count );
    ok( hr == S_OK && count == GetSystemMetrics( SM_CMOUSEBUTTONS ), "got hr %#lx, count %u.\n", hr, count );
    hr = IMouseCapabilities_get_SwapButtons( mouse, &value );
    ok( hr == S_OK && value == (GetSystemMetrics( SM_SWAPBUTTON ) != 0), "got hr %#lx, value %d.\n", hr, value );
    IMouseCapabilities_Release( mouse );

    if (!(instance = activate( L"Windows.Devices.Input.TouchCapabilities" ))) return;
    hr = IInspectable_QueryInterface( instance, &IID_ITouchCapabilities, (void **)&touch );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( instance );
    hr = ITouchCapabilities_get_Contacts( touch, &count );
    ok( hr == S_OK && count == GetSystemMetrics( SM_MAXIMUMTOUCHES ), "got hr %#lx, count %u.\n", hr, count );
    hr = ITouchCapabilities_get_TouchPresent( touch, &value );
    ok( hr == S_OK && (value == 0 || value == 1), "got hr %#lx, value %d.\n", hr, value );
    ITouchCapabilities_Release( touch );
}

static void test_DisplayRequest(void)
{
    IDisplayRequest *request;
    IInspectable *instance;
    HRESULT hr;

    if (!(instance = activate( L"Windows.System.Display.DisplayRequest" )))
    {
        win_skip( "DisplayRequest runtimeclass not registered, skipping tests.\n" );
        return;
    }
    hr = IInspectable_QueryInterface( instance, &IID_IDisplayRequest, (void **)&request );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( instance );

    hr = IDisplayRequest_RequestRelease( request );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IDisplayRequest_RequestActive( request );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IDisplayRequest_RequestActive( request );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IDisplayRequest_RequestRelease( request );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IDisplayRequest_RequestRelease( request );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IDisplayRequest_RequestRelease( request );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

    /* a request that is still active when the object goes away is released with it */
    hr = IDisplayRequest_RequestActive( request );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IDisplayRequest_Release( request );
}

static void test_PowerManager(void)
{
    IPowerManagerStatics *statics = NULL;
    EventRegistrationToken token;
    SYSTEM_POWER_STATUS status;
    EnergySaverStatus saver;
    PowerSupplyStatus supply;
    BatteryStatus battery;
    HSTRING name;
    TimeSpan span;
    INT32 percent;
    HRESULT hr;

    WindowsCreateString( L"Windows.System.Power.PowerManager", 33, &name );
    hr = RoGetActivationFactory( name, &IID_IPowerManagerStatics, (void **)&statics );
    WindowsDeleteString( name );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (!statics)
    {
        win_skip( "PowerManager runtimeclass not registered, skipping tests.\n" );
        return;
    }

    ok( GetSystemPowerStatus( &status ), "GetSystemPowerStatus failed.\n" );
    hr = IPowerManagerStatics_get_BatteryStatus( statics, &battery );
    ok( hr == S_OK && battery >= BatteryStatus_NotPresent && battery <= BatteryStatus_Charging, "got hr %#lx, %d.\n", hr, battery );
    hr = IPowerManagerStatics_get_EnergySaverStatus( statics, &saver );
    ok( hr == S_OK && saver >= EnergySaverStatus_Disabled && saver <= EnergySaverStatus_On, "got hr %#lx, %d.\n", hr, saver );
    hr = IPowerManagerStatics_get_PowerSupplyStatus( statics, &supply );
    ok( hr == S_OK && (supply == PowerSupplyStatus_Adequate) == (status.ACLineStatus == 1), "got hr %#lx, %d.\n", hr, supply );
    hr = IPowerManagerStatics_get_RemainingChargePercent( statics, &percent );
    ok( hr == S_OK && percent >= 0 && percent <= 100, "got hr %#lx, %d.\n", hr, percent );
    hr = IPowerManagerStatics_get_RemainingDischargeTime( statics, &span );
    ok( hr == S_OK && span.Duration > 0, "got hr %#lx.\n", hr );

    /* the events are accepted and removed again */
    hr = IPowerManagerStatics_add_BatteryStatusChanged( statics, NULL, &token );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    IPowerManagerStatics_Release( statics );
}

/* measured on Windows 11, unpackaged: every capability name is Allowed, an empty name and process id 0 are E_INVALIDARG */
static void test_AppCapability(void)
{
    IAsyncOperation_AppCapabilityAccessStatus *operation;
    IAppCapabilityStatics *statics = NULL;
    AppCapabilityAccessStatus status;
    IAppCapability2 *capability2;
    IAppCapability *capability;
    HSTRING name, text;
    IUser *user;
    HRESULT hr;

    WindowsCreateString( L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability", 64, &name );
    hr = RoGetActivationFactory( name, &IID_IAppCapabilityStatics, (void **)&statics );
    WindowsDeleteString( name );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (!statics)
    {
        win_skip( "AppCapability runtimeclass not registered, skipping tests.\n" );
        return;
    }

    WindowsCreateString( L"webcam", 6, &name );
    hr = IAppCapabilityStatics_Create( statics, name, &capability );
    ok( hr == S_OK && capability, "got hr %#lx.\n", hr );
    hr = IAppCapability_get_CapabilityName( capability, &text );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( text, NULL ), L"webcam" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    hr = IAppCapability_get_User( capability, &user );
    ok( hr == S_OK && user, "got hr %#lx.\n", hr );
    if (user) IUser_Release( user );
    status = 0xdeadbeef;
    hr = IAppCapability_CheckAccess( capability, &status );
    ok( hr == S_OK && status == AppCapabilityAccessStatus_Allowed, "got hr %#lx, status %d.\n", hr, status );
    hr = IAppCapability_RequestAccessAsync( capability, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    status = 0xdeadbeef;
    hr = IAsyncOperation_AppCapabilityAccessStatus_GetResults( operation, &status );
    ok( hr == S_OK && status == AppCapabilityAccessStatus_Allowed, "got hr %#lx, status %d.\n", hr, status );
    IAsyncOperation_AppCapabilityAccessStatus_Release( operation );

    hr = IAppCapability_QueryInterface( capability, &IID_IAppCapability2, (void **)&capability2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsCreateString( L"hi", 2, &text );
    hr = IAppCapability2_put_DisplayMessage( capability2, text );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    hr = IAppCapability2_get_DisplayMessage( capability2, &text );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( text, NULL ), L"hi" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    IAppCapability2_Release( capability2 );

    hr = IAppCapability_add_AccessChanged( capability, NULL, (EventRegistrationToken *)&status );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    IAppCapability_Release( capability );

    /* a name that is no capability is allowed as well; an empty name and process id 0 are not accepted */
    WindowsDeleteString( name );
    WindowsCreateString( L"bogusCapabilityName", 19, &name );
    hr = IAppCapabilityStatics_Create( statics, name, &capability );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAppCapability_CheckAccess( capability, &status );
    ok( hr == S_OK && status == AppCapabilityAccessStatus_Allowed, "got hr %#lx, status %d.\n", hr, status );
    IAppCapability_Release( capability );
    capability = (void *)0xdeadbeef;
    hr = IAppCapabilityStatics_CreateWithProcessIdForUser( statics, NULL, name, 0, &capability );
    ok( hr == E_INVALIDARG && !capability, "got hr %#lx, capability %p.\n", hr, capability );
    hr = IAppCapabilityStatics_CreateWithProcessIdForUser( statics, NULL, name, GetCurrentProcessId(), &capability );
    ok( hr == S_OK && capability, "got hr %#lx.\n", hr );
    IAppCapability_Release( capability );
    WindowsDeleteString( name );
    capability = (void *)0xdeadbeef;
    hr = IAppCapabilityStatics_Create( statics, NULL, &capability );
    ok( hr == E_INVALIDARG && !capability, "got hr %#lx, capability %p.\n", hr, capability );

    IAppCapabilityStatics_Release( statics );
}

static void test_HttpClient(void)
{
    IInspectable *instance, *operation = (void *)0xdeadbeef;
    IClosable *closable;
    IHttpClient *client;
    HSTRING name;
    HRESULT hr;

    if (!(instance = activate( L"Windows.Web.Http.HttpClient" )))
    {
        win_skip( "HttpClient runtimeclass not registered, skipping tests.\n" );
        return;
    }
    hr = IInspectable_QueryInterface( instance, &IID_IHttpClient, (void **)&client );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( instance, &IID_IClosable, (void **)&closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IClosable_Close( closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IClosable_Release( closable );

    hr = IHttpClient_GetRuntimeClassName( client, &name );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( name, NULL ), L"Windows.Web.Http.HttpClient" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( name );

    /* the requests are not implemented */
    hr = IHttpClient_GetStringAsync( client, NULL, &operation );
    ok( hr == E_NOTIMPL && !operation, "got hr %#lx, operation %p.\n", hr, operation );
    IHttpClient_Release( client );
    IInspectable_Release( instance );
}

START_TEST(capabilities)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_capabilities();
    test_DisplayRequest();
    test_HttpClient();
    test_PowerManager();
    test_AppCapability();

    RoUninitialize();
}
