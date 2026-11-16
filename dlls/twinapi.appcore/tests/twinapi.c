/*
 * Copyright 2023 Rémi Bernon for CodeWeavers
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

#include "windef.h"
#include "winbase.h"

#define COBJMACROS
#include "initguid.h"
#include "winstring.h"
#include "winternl.h"
#include "roapi.h"
#include "shobjidl_core.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Security_ExchangeActiveSyncProvisioning
#include "windows.security.exchangeactivesyncprovisioning.h"
#define WIDL_using_Windows_System
#include "windows.system.h"
#define WIDL_using_Windows_Management_Policies
#include "windows.management.policies.h"
#define WIDL_using_Windows_System_Profile
#include "windows.system.profile.h"
#define WIDL_using_Windows_System_UserProfile
#include "windows.system.userprofile.h"
#define WIDL_using_Windows_UI_ViewManagement
#include "windows.ui.viewmanagement.h"
#define WIDL_using_Windows_ApplicationModel_Core
#include "windows.applicationmodel.core.h"
#define WIDL_using_Windows_ApplicationModel_DataTransfer
#include "windows.applicationmodel.datatransfer.h"

#include "wine/test.h"

#define check_interface( a, b, c ) check_interface_( __LINE__, a, b, c, FALSE )
static void check_interface_( unsigned int line, void *iface_ptr, REFIID iid, BOOL supported, BOOL is_broken )
{
    HRESULT hr, expected_hr, broken_hr;
    IUnknown *iface = iface_ptr;
    IUnknown *unk;

    expected_hr = supported ? S_OK : E_NOINTERFACE;
    broken_hr = supported ? E_NOINTERFACE : S_OK;
    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_(__FILE__, line)( hr == expected_hr || broken( is_broken && hr == broken_hr ),
                         "got hr %#lx, expected %#lx.\n", hr, expected_hr );
    if (SUCCEEDED(hr)) IUnknown_Release( unk );
}

static void test_EasClientDeviceInformation(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_Security_ExchangeActiveSyncProvisioning_EasClientDeviceInformation;
    IEasClientDeviceInformation *client_device_information;
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, TRUE );
    check_interface( factory, &IID_IInspectable, TRUE );
    check_interface( factory, &IID_IAgileObject, FALSE );

    hr = IActivationFactory_ActivateInstance( factory, (IInspectable **)&client_device_information );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( client_device_information, &IID_IUnknown, TRUE );
    check_interface( client_device_information, &IID_IInspectable, TRUE );
    check_interface( client_device_information, &IID_IAgileObject, FALSE );

    hr = IEasClientDeviceInformation_get_OperatingSystem( client_device_information, &str );
    ok( hr == S_OK, "got hr %#lx\n", hr );
    WindowsDeleteString( str );

    hr = IEasClientDeviceInformation_get_FriendlyName( client_device_information, &str );
    ok( hr == S_OK, "got hr %#lx\n", hr );
    WindowsDeleteString( str );

    hr = IEasClientDeviceInformation_get_SystemManufacturer( client_device_information, &str );
    ok( hr == S_OK, "got hr %#lx\n", hr );
    WindowsDeleteString( str );

    hr = IEasClientDeviceInformation_get_SystemProductName( client_device_information, &str );
    ok( hr == S_OK, "got hr %#lx\n", hr );
    WindowsDeleteString( str );

    hr = IEasClientDeviceInformation_get_SystemSku( client_device_information, &str );
    ok( hr == S_OK, "got hr %#lx\n", hr );
    WindowsDeleteString( str );

    ref = IEasClientDeviceInformation_Release( client_device_information );
    ok( ref == 0, "got ref %ld.\n", ref );

    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

static void test_AnalyticsVersionInfo(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_System_Profile_AnalyticsInfo;
    IAnalyticsInfoStatics *analytics_info_statics;
    IAnalyticsVersionInfo *analytics_version_info;
    IActivationFactory *factory;
    HSTRING str, expect_str;
    DWORD revision, size;
    WCHAR buffer[32];
    UINT64 version;
    HRESULT hr;
    INT32 res;
    LONG ref;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, TRUE );
    check_interface( factory, &IID_IInspectable, TRUE );
    check_interface( factory, &IID_IAgileObject, TRUE );

    hr = IActivationFactory_QueryInterface( factory, &IID_IAnalyticsInfoStatics, (void **)&analytics_info_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IAnalyticsInfoStatics_get_VersionInfo( analytics_info_statics, &analytics_version_info );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( analytics_version_info, &IID_IUnknown, TRUE );
    check_interface( analytics_version_info, &IID_IInspectable, TRUE );
    check_interface( analytics_version_info, &IID_IAgileObject, TRUE );

    hr = WindowsCreateString( L"Windows.Desktop", wcslen( L"Windows.Desktop" ), &expect_str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAnalyticsVersionInfo_get_DeviceFamily( analytics_version_info, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = WindowsCompareStringOrdinal( str, expect_str, &res );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !res, "got unexpected string %s.\n", debugstr_hstring(str) );
    WindowsDeleteString( str );
    WindowsDeleteString( expect_str );

    size = sizeof(revision);
    if (RegGetValueW( HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                      RRF_RT_REG_DWORD, NULL, &revision, &size ))
        revision = 0;
    version = NtCurrentTeb()->Peb->OSMajorVersion & 0xffff;
    version = (version << 16) | (NtCurrentTeb()->Peb->OSMinorVersion & 0xffff);
    version = (version << 16) | (NtCurrentTeb()->Peb->OSBuildNumber & 0xffff);
    version = (version << 16) | revision;

    res = swprintf( buffer, ARRAY_SIZE(buffer), L"%I64u", version );
    hr = WindowsCreateString( buffer, res, &expect_str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAnalyticsVersionInfo_get_DeviceFamilyVersion( analytics_version_info, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = WindowsCompareStringOrdinal( str, expect_str, &res );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !res || broken(revision == 0) /* Win11 */, "got unexpected string %s.\n", debugstr_hstring(str) );
    WindowsDeleteString( str );
    WindowsDeleteString( expect_str );

    ref = IAnalyticsVersionInfo_Release( analytics_version_info );
    ok( ref == 0, "got ref %ld.\n", ref );

    ref = IAnalyticsInfoStatics_Release( analytics_info_statics );
    ok( ref == 2, "got ref %ld.\n", ref );
    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

static void test_AdvertisingManager(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_System_UserProfile_AdvertisingManager;
    IAdvertisingManagerStatics *advertising_manager_statics;
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, TRUE );
    check_interface( factory, &IID_IInspectable, TRUE );
    check_interface_( __LINE__, factory, &IID_IAgileObject, TRUE, TRUE );

    hr = IActivationFactory_QueryInterface( factory, &IID_IAdvertisingManagerStatics, (void **)&advertising_manager_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IAdvertisingManagerStatics_get_AdvertisingId( advertising_manager_statics, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    ref = IAdvertisingManagerStatics_Release( advertising_manager_statics );
    ok( ref == 2, "got ref %ld.\n", ref );
    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

static void test_ApplicationView(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_UI_ViewManagement_ApplicationView;
    IApplicationViewStatics2 *app_view_statics2;
    IActivationFactory *factory;
    IApplicationView *app_view;
    HSTRING str;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (FAILED( hr ))
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, TRUE );
    check_interface( factory, &IID_IInspectable, TRUE );
    check_interface( factory, &IID_IAgileObject, TRUE );
    check_interface( factory, &IID_IActivationFactory, TRUE );
    check_interface( factory, &IID_IApplicationViewStatics, TRUE );
    check_interface( factory, &IID_IApplicationViewStatics2, TRUE );

    /* Test IApplicationViewStatics2 */
    hr = IActivationFactory_QueryInterface( factory, &IID_IApplicationViewStatics2,
                                            (void **)&app_view_statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IApplicationViewStatics2_GetForCurrentView( app_view_statics2, &app_view );
    ok( hr == 0x80070490, "got hr %#lx.\n", hr );

    IApplicationViewStatics2_Release( app_view_statics2 );

    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

static void test_CoreApplication(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_ApplicationModel_Core_CoreApplication;
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (FAILED( hr ))
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, TRUE );
    check_interface( factory, &IID_IInspectable, TRUE );
    check_interface( factory, &IID_IAgileObject, TRUE );
    check_interface( factory, &IID_IActivationFactory, TRUE );

    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

static void test_DataTransferManager(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager;
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (FAILED( hr ))
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, TRUE );
    check_interface( factory, &IID_IInspectable, TRUE );
    check_interface( factory, &IID_IAgileObject, FALSE );
    check_interface( factory, &IID_IActivationFactory, TRUE );
    check_interface( factory, &IID_IDataTransferManagerInterop, TRUE );
    check_interface( factory, &IID_IDataTransferManagerStatics, TRUE );

    IActivationFactory_Release( factory );
}

static void *get_statics( const WCHAR *class_name, REFIID iid )
{
    IActivationFactory *factory;
    HSTRING str;
    void *out = NULL;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "%s: got hr %#lx.\n", debugstr_w( class_name ), hr );
    if (hr != S_OK) return NULL;
    hr = IActivationFactory_QueryInterface( factory, iid, &out );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IActivationFactory_Release( factory );
    return out;
}

/* Education/AllowGraphingCalculator, unmanaged: a user policy (Int32, 1), without a value when read without a user */
static void check_graphing_policy( INamedPolicyData *data, IUser *expect_user )
{
    NamedPolicyKind kind = -1;
    boolean flag = 2;
    HSTRING str;
    IUser *user = (void *)0xdeadbeef;
    INT32 number = -1;
    HRESULT hr;

    hr = INamedPolicyData_get_Area( data, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Education" ), "got area %s.\n", debugstr_hstring( str ) );
    WindowsDeleteString( str );
    hr = INamedPolicyData_get_Name( data, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"AllowGraphingCalculator" ), "got name %s.\n", debugstr_hstring( str ) );
    WindowsDeleteString( str );
    hr = INamedPolicyData_get_Kind( data, &kind );
    ok( hr == S_OK && kind == NamedPolicyKind_Int32, "got hr %#lx, kind %d.\n", hr, kind );
    /* without a value, IsManaged is false but fails */
    hr = INamedPolicyData_get_IsManaged( data, &flag );
    ok( hr == (expect_user ? S_OK : E_UNEXPECTED) && !flag, "got hr %#lx, managed %d.\n", hr, flag );
    hr = INamedPolicyData_get_IsUserPolicy( data, &flag );
    ok( hr == S_OK && flag == !!expect_user, "got hr %#lx, user policy %d.\n", hr, flag );
    hr = INamedPolicyData_get_User( data, &user );
    ok( hr == S_OK && user == expect_user, "got hr %#lx, user %p.\n", hr, user );
    if (user) IUser_Release( user );
    flag = 2;
    hr = INamedPolicyData_GetBoolean( data, &flag );
    if (expect_user) ok( hr == S_OK && flag == TRUE, "got hr %#lx, value %d.\n", hr, flag );
    else ok( hr == E_UNEXPECTED, "got hr %#lx.\n", hr );
    hr = INamedPolicyData_GetInt32( data, &number );
    if (expect_user) ok( hr == S_OK && number == 1, "got hr %#lx, value %d.\n", hr, number );
    else ok( hr == E_UNEXPECTED, "got hr %#lx.\n", hr );
    INamedPolicyData_Release( data );
}

static void test_User(void)
{
    INamedPolicyStatics *policies;
    IUserStatics2 *statics2;
    IUserStatics *statics;
    IUser *user = NULL, *found;
    INamedPolicyData *data;
    HSTRING id = NULL, str, area, name;
    HRESULT hr;

    if (!(statics = get_statics( L"Windows.System.User", &IID_IUserStatics ))) return;
    statics2 = get_statics( L"Windows.System.User", &IID_IUserStatics2 );
    ok( !!statics2, "no IUserStatics2.\n" );
    if (!statics2) { IUserStatics_Release( statics ); return; }
    hr = IUserStatics2_GetDefault( statics2, &user );
    ok( hr == S_OK && user, "got hr %#lx, user %p.\n", hr, user );
    IUserStatics2_Release( statics2 );
    if (!user) { IUserStatics_Release( statics ); return; }

    /* the user of an id is the same object; another or no id has none */
    hr = IUser_get_NonRoamableId( user, &id );
    ok( hr == S_OK && WindowsGetStringLen( id ), "got hr %#lx, id %s.\n", hr, debugstr_hstring( id ) );
    found = (void *)0xdeadbeef;
    hr = IUserStatics_GetFromId( statics, id, &found );
    ok( hr == S_OK && found == user, "got hr %#lx, user %p, expected %p.\n", hr, found, user );
    if (hr == S_OK && found) IUser_Release( found );
    WindowsCreateString( L"bogus", 5, &str );
    found = (void *)0xdeadbeef;
    hr = IUserStatics_GetFromId( statics, str, &found );
    ok( hr == S_OK && !found, "got hr %#lx, user %p.\n", hr, found );
    WindowsDeleteString( str );
    found = (void *)0xdeadbeef;
    hr = IUserStatics_GetFromId( statics, NULL, &found );
    ok( hr == S_OK && !found, "got hr %#lx, user %p.\n", hr, found );
    IUserStatics_Release( statics );

    if (!(policies = get_statics( L"Windows.Management.Policies.NamedPolicy", &IID_INamedPolicyStatics ))) goto done;
    WindowsCreateString( L"Education", 9, &area );
    WindowsCreateString( L"AllowGraphingCalculator", 23, &name );
    data = NULL;
    hr = INamedPolicyStatics_GetPolicyFromPath( policies, area, name, &data );
    ok( hr == S_OK && data, "got hr %#lx, data %p.\n", hr, data );
    if (data) check_graphing_policy( data, NULL );
    data = NULL;
    hr = INamedPolicyStatics_GetPolicyFromPathForUser( policies, user, area, name, &data );
    ok( hr == S_OK && data, "got hr %#lx, data %p.\n", hr, data );
    if (data) check_graphing_policy( data, user );
    data = (void *)0xdeadbeef;
    hr = INamedPolicyStatics_GetPolicyFromPathForUser( policies, NULL, area, name, &data );
    ok( hr == E_INVALIDARG && data == (void *)0xdeadbeef, "got hr %#lx, data %p.\n", hr, data );
    WindowsDeleteString( area );
    WindowsDeleteString( name );
    WindowsCreateString( L"Bogus", 5, &area );
    WindowsCreateString( L"Nothing", 7, &name );
    data = (void *)0xdeadbeef;
    hr = INamedPolicyStatics_GetPolicyFromPath( policies, area, name, &data );
    ok( hr == E_UNEXPECTED && !data, "got hr %#lx, data %p.\n", hr, data );
    WindowsDeleteString( area );
    WindowsDeleteString( name );
    INamedPolicyStatics_Release( policies );
done:
    WindowsDeleteString( id );
    IUser_Release( user );
}

static void test_MemoryManager(void)
{
    UINT64 usage = 0, limit = 0, expected = 0;
    IMemoryManagerStatics2 *statics2;
    IMemoryManagerStatics3 *statics3;
    IMemoryManagerStatics4 *statics4;
    IMemoryManagerStatics *statics;
    IProcessMemoryReport *process;
    IAppMemoryReport *app;
    AppMemoryUsageLevel level = 0x55;
    boolean done = 2;
    HRESULT hr;

    UINT64 private_usage = 0, peak = 0, total = 0, total_limit = 0, working = 0, private_working = 0;

    /* a process without a resource policy: its commit charge against the system's commit limit */
    if (!(statics = get_statics( L"Windows.System.MemoryManager", &IID_IMemoryManagerStatics ))) return;
    hr = IMemoryManagerStatics_get_AppMemoryUsage( statics, &usage );
    ok( hr == S_OK && usage, "got hr %#lx, usage %I64u.\n", hr, usage );
    hr = IMemoryManagerStatics_get_AppMemoryUsageLimit( statics, &limit );
    ok( hr == S_OK && limit > usage, "got hr %#lx, limit %I64u.\n", hr, limit );
    /* a usage level only for a packaged process */
    hr = IMemoryManagerStatics_get_AppMemoryUsageLevel( statics, &level );
    ok( hr == HRESULT_FROM_NT( 0xc0000225 /* STATUS_NOT_FOUND */ ), "got hr %#lx, level %d.\n", hr, level );
    IMemoryManagerStatics_Release( statics );

    if (!(statics2 = get_statics( L"Windows.System.MemoryManager", &IID_IMemoryManagerStatics2 ))) return;
    hr = IMemoryManagerStatics2_GetAppMemoryReport( statics2, &app );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        IAppMemoryReport_get_PrivateCommitUsage( app, &private_usage );
        IAppMemoryReport_get_PeakPrivateCommitUsage( app, &peak );
        IAppMemoryReport_get_TotalCommitUsage( app, &total );
        IAppMemoryReport_get_TotalCommitLimit( app, &total_limit );
        ok( private_usage && private_usage <= peak && private_usage <= total, "got private %I64u, peak %I64u, total %I64u.\n",
            private_usage, peak, total );
        ok( total_limit == limit, "got limit %I64u, expected %I64u.\n", total_limit, limit );
        IAppMemoryReport_Release( app );
    }
    hr = IMemoryManagerStatics2_GetProcessMemoryReport( statics2, &process );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        IProcessMemoryReport_get_PrivateWorkingSetUsage( process, &private_working );
        IProcessMemoryReport_get_TotalWorkingSetUsage( process, &working );
        ok( private_working && private_working <= working, "got private %I64u, total %I64u.\n", private_working, working );
        IProcessMemoryReport_Release( process );
    }
    IMemoryManagerStatics2_Release( statics2 );

    /* the limit cannot be set, and none is expected */
    if ((statics3 = get_statics( L"Windows.System.MemoryManager", &IID_IMemoryManagerStatics3 )))
    {
        hr = IMemoryManagerStatics3_TrySetAppMemoryUsageLimit( statics3, 1024 * 1024 * 1024, &done );
        ok( hr == S_OK && !done, "got hr %#lx, result %d.\n", hr, done );
        IMemoryManagerStatics3_Release( statics3 );
    }
    if ((statics4 = get_statics( L"Windows.System.MemoryManager", &IID_IMemoryManagerStatics4 )))
    {
        expected = 0xdeadbeef;
        hr = IMemoryManagerStatics4_get_ExpectedAppMemoryUsageLimit( statics4, &expected );
        ok( hr == S_OK && !expected, "got hr %#lx, limit %I64u.\n", hr, expected );
        IMemoryManagerStatics4_Release( statics4 );
    }
}

START_TEST(twinapi)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_EasClientDeviceInformation();
    test_AnalyticsVersionInfo();
    test_AdvertisingManager();
    test_ApplicationView();
    test_CoreApplication();
    test_DataTransferManager();
    test_User();
    test_MemoryManager();

    RoUninitialize();
}
