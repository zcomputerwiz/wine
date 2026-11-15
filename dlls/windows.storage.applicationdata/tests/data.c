/*
 * Copyright (C) 2023 Mohamad Al-Jaf
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
#define WIDL_using_Windows_Storage
#include "windows.storage.h"

#include "wine/test.h"

#define check_interface( obj, iid ) check_interface_( __LINE__, obj, iid )
static void check_interface_( unsigned int line, void *obj, const IID *iid )
{
    IUnknown *iface = obj;
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    IUnknown_Release( unk );
}

static void test_ApplicationDataStatics(void)
{
    static const WCHAR *application_data_statics_name = L"Windows.Storage.ApplicationData";
    IApplicationDataStatics *application_data_statics;
    IApplicationData *application_data = NULL;
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( application_data_statics_name, wcslen( application_data_statics_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( application_data_statics_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown );
    check_interface( factory, &IID_IInspectable );
    check_interface( factory, &IID_IAgileObject );

    hr = IActivationFactory_QueryInterface( factory, &IID_IApplicationDataStatics, (void **)&application_data_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IApplicationDataStatics_get_Current( application_data_statics, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IApplicationDataStatics_get_Current( application_data_statics, &application_data );
    ok( hr == 0x80073d54, "got hr %#lx.\n", hr );
    ok( !application_data, "got application_data %p.\n", application_data );
    if (application_data) IApplicationData_Release( application_data );

    ref = IApplicationDataStatics_Release( application_data_statics );
    ok( ref == 2, "got ref %ld.\n", ref );
    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

/* a composite value (and a settings container, the same on Windows) gives null for a key never set, not E_BOUNDS */
static void test_ApplicationDataCompositeValue(void)
{
    static const WCHAR *class_name = L"Windows.Storage.ApplicationDataCompositeValue";
    IMap_HSTRING_IInspectable *map;
    IInspectable *inspectable, *value;
    boolean found;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoActivateInstance( str, &inspectable );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr != S_OK) return;
    hr = IInspectable_QueryInterface( inspectable, &IID_IMap_HSTRING_IInspectable, (void **)&map );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_Release( inspectable );
    if (hr != S_OK) return;

    WindowsCreateString( L"never set", 9, &str );
    found = TRUE;
    hr = IMap_HSTRING_IInspectable_HasKey( map, str, &found );
    ok( hr == S_OK && !found, "got hr %#lx, found %d.\n", hr, found );
    value = (void *)0xdeadbeef;
    hr = IMap_HSTRING_IInspectable_Lookup( map, str, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !value, "got value %p.\n", value );
    WindowsDeleteString( str );
    IMap_HSTRING_IInspectable_Release( map );
}

START_TEST(data)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_ApplicationDataStatics();
    test_ApplicationDataCompositeValue();

    RoUninitialize();
}
