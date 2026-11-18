/*
 * Tests for the small settings classes (EducationSettings, PlatformDiagnosticsAndUsageDataSettings, AssignedAccessSettings,
 * DiagnosticsSettings, ApplicationDataManager, PenDevice)
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

#include "windef.h"
#include "winbase.h"

#define COBJMACROS
#include "winstring.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_System
#define WIDL_using_Windows_Storage
#include "windows.foundation.h"
#include "windows.system.h"
#define WIDL_using_Windows_System_Profile
#include "windows.system.profile.h"
#define WIDL_using_Windows_System_UserProfile
#include "windows.system.userprofile.h"
#define WIDL_using_Windows_Storage
#include "windows.storage.h"
#define WIDL_using_Windows_Management_Core
#include "windows.management.core.h"
#define WIDL_using_Windows_Devices_Input
#include "windows.devices.input.h"

#include "wine/test.h"

static void *get_factory( const WCHAR *class_name, REFIID iid )
{
    HSTRING str;
    void *factory = NULL;
    HRESULT hr;

    WindowsCreateString( class_name, wcslen( class_name ), &str );
    hr = RoGetActivationFactory( str, iid, &factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "%s: got hr %#lx.\n", debugstr_w( class_name ), hr );
    return factory;
}

static void test_EducationSettings(void)
{
    IEducationSettingsStatics *statics;
    boolean value = TRUE;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.System.Profile.EducationSettings", &IID_IEducationSettingsStatics )))
    {
        win_skip( "EducationSettings runtimeclass not registered, skipping tests.\n" );
        return;
    }
    hr = IEducationSettingsStatics_get_IsEducationEnvironment( statics, &value );
    ok( hr == S_OK && !value, "got hr %#lx, value %d.\n", hr, value );
    IEducationSettingsStatics_Release( statics );
}

static void test_PlatformDiagnostics(void)
{
    IPlatformDiagnosticsAndUsageDataSettingsStatics *statics;
    PlatformDataCollectionLevel level;
    EventRegistrationToken token;
    boolean can;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.System.Profile.PlatformDiagnosticsAndUsageDataSettings",
                                 &IID_IPlatformDiagnosticsAndUsageDataSettingsStatics )))
    {
        win_skip( "PlatformDiagnosticsAndUsageDataSettings runtimeclass not registered, skipping tests.\n" );
        return;
    }
    hr = IPlatformDiagnosticsAndUsageDataSettingsStatics_get_CollectionLevel( statics, &level );
    ok( hr == S_OK && level >= PlatformDataCollectionLevel_Security && level <= PlatformDataCollectionLevel_Full,
        "got hr %#lx, level %d.\n", hr, level );
    /* a level is collectable when it is not above the current one */
    hr = IPlatformDiagnosticsAndUsageDataSettingsStatics_CanCollectDiagnostics( statics, level, &can );
    ok( hr == S_OK && can, "got hr %#lx, can %d.\n", hr, can );
    hr = IPlatformDiagnosticsAndUsageDataSettingsStatics_add_CollectionLevelChanged( statics, NULL, &token );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    IPlatformDiagnosticsAndUsageDataSettingsStatics_Release( statics );
}

static void test_AssignedAccess(void)
{
    IAssignedAccessSettingsStatics *statics;
    IAssignedAccessSettings *settings;
    boolean value = TRUE;
    IUser *user;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.System.UserProfile.AssignedAccessSettings", &IID_IAssignedAccessSettingsStatics )))
    {
        win_skip( "AssignedAccessSettings runtimeclass not registered, skipping tests.\n" );
        return;
    }
    hr = IAssignedAccessSettingsStatics_GetDefault( statics, &settings );
    ok( hr == S_OK && settings, "got hr %#lx.\n", hr );
    hr = IAssignedAccessSettings_get_IsEnabled( settings, &value );
    ok( hr == S_OK && !value, "got hr %#lx, value %d.\n", hr, value );
    value = TRUE;
    hr = IAssignedAccessSettings_get_IsSingleAppKioskMode( settings, &value );
    ok( hr == S_OK && !value, "got hr %#lx, value %d.\n", hr, value );
    user = (void *)0xdeadbeef;
    hr = IAssignedAccessSettings_get_User( settings, &user );
    ok( hr == S_OK && !user, "got hr %#lx, user %p.\n", hr, user );
    IAssignedAccessSettings_Release( settings );

    settings = (void *)0xdeadbeef;
    hr = IAssignedAccessSettingsStatics_GetForUser( statics, NULL, &settings );
    ok( hr == E_INVALIDARG && !settings, "got hr %#lx, settings %p.\n", hr, settings );
    IAssignedAccessSettingsStatics_Release( statics );
}

static void test_Diagnostics(void)
{
    IDiagnosticsSettingsStatics *statics;
    IDiagnosticsSettings *settings;
    boolean value = TRUE;
    IUser *user;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.System.UserProfile.DiagnosticsSettings", &IID_IDiagnosticsSettingsStatics )))
    {
        win_skip( "DiagnosticsSettings runtimeclass not registered, skipping tests.\n" );
        return;
    }
    hr = IDiagnosticsSettingsStatics_GetDefault( statics, &settings );
    ok( hr == S_OK && settings, "got hr %#lx.\n", hr );
    hr = IDiagnosticsSettings_get_CanUseDiagnosticsToTailorExperiences( settings, &value );
    ok( hr == S_OK && !value, "got hr %#lx, value %d.\n", hr, value );
    user = (void *)0xdeadbeef;
    hr = IDiagnosticsSettings_get_User( settings, &user );
    ok( hr == S_OK && !user, "got hr %#lx, user %p.\n", hr, user );
    IDiagnosticsSettings_Release( settings );

    settings = (void *)0xdeadbeef;
    hr = IDiagnosticsSettingsStatics_GetForUser( statics, NULL, &settings );
    ok( hr == E_INVALIDARG && !settings, "got hr %#lx, settings %p.\n", hr, settings );
    IDiagnosticsSettingsStatics_Release( statics );
}

static void test_ApplicationDataManager(void)
{
    IApplicationDataManagerStatics *statics;
    IApplicationData *data;
    HSTRING family;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.Management.Core.ApplicationDataManager", &IID_IApplicationDataManagerStatics )))
    {
        win_skip( "ApplicationDataManager runtimeclass not registered, skipping tests.\n" );
        return;
    }
    data = (void *)0xdeadbeef;
    hr = IApplicationDataManagerStatics_CreateForPackageFamily( statics, NULL, &data );
    ok( hr == E_INVALIDARG && !data, "got hr %#lx, data %p.\n", hr, data );

    /* a desktop application has no package; another family is not found */
    WindowsCreateString( L"Bogus.Family_0123456789abc", 26, &family );
    data = (void *)0xdeadbeef;
    hr = IApplicationDataManagerStatics_CreateForPackageFamily( statics, family, &data );
    ok( hr == HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ) && !data, "got hr %#lx, data %p.\n", hr, data );
    WindowsDeleteString( family );
    IApplicationDataManagerStatics_Release( statics );
}

static void test_PenDevice(void)
{
    IPenDeviceStatics *statics;
    IPenDevice *pen;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.Devices.Input.PenDevice", &IID_IPenDeviceStatics )))
    {
        win_skip( "PenDevice runtimeclass not registered, skipping tests.\n" );
        return;
    }
    pen = (void *)0xdeadbeef;
    hr = IPenDeviceStatics_GetFromPointerId( statics, 1, &pen );
    ok( hr == S_OK && !pen, "got hr %#lx, pen %p.\n", hr, pen );
    IPenDeviceStatics_Release( statics );
}

START_TEST(settings)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_EducationSettings();
    test_PlatformDiagnostics();
    test_AssignedAccess();
    test_Diagnostics();
    test_ApplicationDataManager();
    test_PenDevice();

    RoUninitialize();
}
