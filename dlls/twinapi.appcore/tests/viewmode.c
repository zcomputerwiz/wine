/*
 * Tests for Windows.UI.ViewManagement.ViewModePreferences and Windows.System.Launcher
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
#include "windows.foundation.h"
#define WIDL_using_Windows_System
#define WIDL_using_Windows_UI_ViewManagement
#include "windows.ui.viewmanagement.h"
#include "windows.system.h"

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

static void test_ViewModePreferences(void)
{
    IViewModePreferencesStatics *statics;
    IViewModePreferences *preferences;
    ViewSizePreference preference;
    IInspectable *inspectable;
    Size size;
    ULONG count;
    IID *iids;
    HSTRING name;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.UI.ViewManagement.ViewModePreferences", &IID_IViewModePreferencesStatics )))
    {
        win_skip( "ViewModePreferences runtimeclass not registered, skipping tests.\n" );
        return;
    }

    hr = IViewModePreferencesStatics_CreateDefault( statics, ApplicationViewMode_CompactOverlay, &preferences );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IViewModePreferences_QueryInterface( preferences, &IID_IInspectable, (void **)&inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_GetIids( inspectable, &count, &iids );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( count == 1 && IsEqualGUID( &iids[0], &IID_IViewModePreferences ), "got %lu iids.\n", count );
    CoTaskMemFree( iids );
    hr = IInspectable_GetRuntimeClassName( inspectable, &name );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !wcscmp( WindowsGetStringRawBuffer( name, NULL ), L"Windows.UI.ViewManagement.ViewModePreferences" ),
        "got class name %s.\n", debugstr_w( WindowsGetStringRawBuffer( name, NULL ) ) );
    WindowsDeleteString( name );
    IInspectable_Release( inspectable );

    hr = IViewModePreferences_get_ViewSizePreference( preferences, &preference );
    ok( hr == S_OK && preference == ViewSizePreference_Default, "got hr %#lx, preference %d.\n", hr, preference );
    hr = IViewModePreferences_put_ViewSizePreference( preferences, ViewSizePreference_Custom );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IViewModePreferences_get_ViewSizePreference( preferences, &preference );
    ok( hr == S_OK && preference == ViewSizePreference_Custom, "got hr %#lx, preference %d.\n", hr, preference );

    size.Width = 320.0f;
    size.Height = 240.0f;
    hr = IViewModePreferences_put_CustomSize( preferences, size );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    size.Width = size.Height = 0.0f;
    hr = IViewModePreferences_get_CustomSize( preferences, &size );
    ok( hr == S_OK && size.Width == 320.0f && size.Height == 240.0f, "got hr %#lx, size %fx%f.\n", hr, size.Width,
        size.Height );

    IViewModePreferences_Release( preferences );
    IViewModePreferencesStatics_Release( statics );
}

static void test_Launcher(void)
{
    IAsyncOperation_boolean *operation;
    ILauncherStatics *statics;
    ULONG count;
    IID *iids;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.System.Launcher", &IID_ILauncherStatics )))
    {
        win_skip( "Launcher runtimeclass not registered, skipping tests.\n" );
        return;
    }

    hr = ILauncherStatics_GetIids( statics, &count, &iids );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( count == 1 && IsEqualGUID( &iids[0], &IID_ILauncherStatics ), "got %lu iids.\n", count );
    CoTaskMemFree( iids );

    /* the arguments are checked; nothing is launched */
    operation = (void *)0xdeadbeef;
    hr = ILauncherStatics_LaunchUriAsync( statics, NULL, &operation );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( !operation, "got operation %p.\n", operation );
    operation = (void *)0xdeadbeef;
    hr = ILauncherStatics_LaunchFileAsync( statics, NULL, &operation );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( !operation, "got operation %p.\n", operation );

    ILauncherStatics_Release( statics );
}

START_TEST(viewmode)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_ViewModePreferences();
    test_Launcher();

    RoUninitialize();
}
