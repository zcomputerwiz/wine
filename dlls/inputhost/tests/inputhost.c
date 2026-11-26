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

/* The classes have no metadata: the interfaces are called through their slots, as they were measured. Windows 10 and
 * Windows 11 name the site's factory interface and default interface differently; Wine follows Windows 10. */

#define COBJMACROS
#include "initguid.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "roapi.h"
#include "activation.h"

#define WIDL_using_Windows_Foundation
#include "windows.foundation.h"

#include "wine/test.h"

DEFINE_GUID( IID_site_statics, 0x8ca0442d, 0xae80, 0x47b0, 0xaf, 0x23, 0x81, 0x3f, 0xc1, 0xda, 0x9c, 0xd0 );
DEFINE_GUID( IID_site_statics_win11, 0x73f80911, 0x7a51, 0x5b0f, 0x90, 0x6b, 0xf6, 0x40, 0x2e, 0xa6, 0xc3, 0xcb );
DEFINE_GUID( IID_site, 0xf77d91d0, 0x46ca, 0x4464, 0x99, 0xc1, 0x89, 0x02, 0xae, 0xd1, 0x76, 0x80 );
DEFINE_GUID( IID_site_win11, 0xcfb32fa7, 0x5399, 0x5f3d, 0x82, 0xba, 0x96, 0x41, 0xf1, 0x06, 0xff, 0xcd );
DEFINE_GUID( IID_configuration_statics, 0x4c8421ca, 0x3863, 0x5236, 0xb0, 0x9a, 0x75, 0x7c, 0xf6, 0x8c, 0x5f, 0xa5 );
DEFINE_GUID( IID_configuration, 0x773ebb88, 0x99de, 0x4a39, 0xb9, 0xfa, 0x02, 0xad, 0xae, 0xc2, 0x5d, 0x4d );
/* asked of the host: slot 9 gives the input sink; Windows 11 also asks for an object (slot 9 of the second) */
DEFINE_GUID( IID_host_sink, 0x92179cc0, 0xae1f, 0x48bc, 0xa3, 0x54, 0x8f, 0x7a, 0x88, 0x00, 0xf2, 0xfb );
DEFINE_GUID( IID_host_object, 0x01dc794b, 0x4ff5, 0x4491, 0x99, 0x42, 0xb9, 0xe7, 0xb8, 0x89, 0x3b, 0xe4 );

typedef HRESULT (WINAPI *create_fn)( void *iface, void *arg, void **out );
typedef HRESULT (WINAPI *get_fn)( void *iface, INT32 *value );
typedef HRESULT (WINAPI *put_fn)( void *iface, INT32 value );
#define SLOT( obj, n, type ) ((type)(*(void ***)(obj))[n])

static BOOL is_win11;

/* the caller's object: two interfaces, each with a method in slot 9 */
static struct
{
    void **sink_vtbl;
    void **object_vtbl;
    LONG ref;
    HANDLE sink;
    BOOL refuse;
    HRESULT sink_hr;
    int sink_calls, object_calls, unexpected_calls;
} host;

static HRESULT WINAPI host_QueryInterface( void *iface, REFIID iid, void **out )
{
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject )) *out = &host.sink_vtbl;
    else if (IsEqualGUID( iid, &IID_host_sink ) && !host.refuse) *out = &host.sink_vtbl;
    else if (IsEqualGUID( iid, &IID_host_object ) && !host.refuse) *out = &host.object_vtbl;
    if (!*out) return E_NOINTERFACE;
    InterlockedIncrement( &host.ref );
    return S_OK;
}

static ULONG WINAPI host_AddRef( void *iface )
{
    return InterlockedIncrement( &host.ref );
}

static ULONG WINAPI host_Release( void *iface )
{
    return InterlockedDecrement( &host.ref );
}

static HRESULT WINAPI host_unexpected( void *iface )
{
    host.unexpected_calls++;
    return E_NOTIMPL;
}

static HRESULT WINAPI host_get_sink( void *iface, HANDLE *sink )
{
    host.sink_calls++;
    if (FAILED( host.sink_hr )) return host.sink_hr;
    *sink = host.sink;
    return S_OK;
}

static HRESULT WINAPI host_get_object( void *iface, void **object )
{
    host.object_calls++;
    *object = &host.sink_vtbl;
    InterlockedIncrement( &host.ref );
    return S_OK;
}

static void *host_sink_vtbl[] =
{
    host_QueryInterface, host_AddRef, host_Release,
    host_unexpected, host_unexpected, host_unexpected, host_unexpected, host_unexpected, host_unexpected,
    host_get_sink,
};

static void *host_object_vtbl[] =
{
    host_QueryInterface, host_AddRef, host_Release,
    host_unexpected, host_unexpected, host_unexpected, host_unexpected, host_unexpected, host_unexpected,
    host_get_object,
};

static BOOL has_iid( IInspectable *obj, const GUID *iid )
{
    ULONG count = 0, i;
    IID *iids = NULL;
    BOOL found = FALSE;
    HRESULT hr;

    hr = IInspectable_GetIids( obj, &count, &iids );
    ok( hr == S_OK, "GetIids returned %#lx.\n", hr );
    for (i = 0; SUCCEEDED( hr ) && i < count; i++) if (IsEqualGUID( &iids[i], iid )) found = TRUE;
    CoTaskMemFree( iids );
    return found;
}

static void check_class_name( IInspectable *obj, const WCHAR *expect )
{
    HSTRING name = NULL;
    HRESULT hr;

    hr = IInspectable_GetRuntimeClassName( obj, &name );
    ok( hr == S_OK, "GetRuntimeClassName returned %#lx.\n", hr );
    ok( name && !wcscmp( WindowsGetStringRawBuffer( name, NULL ), expect ), "Got class name %s.\n",
        debugstr_w( name ? WindowsGetStringRawBuffer( name, NULL ) : NULL ) );
    WindowsDeleteString( name );
}

static IActivationFactory *get_factory( const WCHAR *class_name )
{
    IActivationFactory *factory = NULL;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "WindowsCreateString returned %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "RoGetActivationFactory returned %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s is not registered.\n", debugstr_w( class_name ) );
        return NULL;
    }
    return factory;
}

static void test_factory( const WCHAR *class_name, const GUID *statics_iid, const GUID *statics_iid_win11 )
{
    IActivationFactory *factory;
    IInspectable *instance = (IInspectable *)0xdeadbeef, *statics = NULL;
    HSTRING name = (HSTRING)0xdeadbeef;
    TrustLevel trust = 0xdead;
    IUnknown *unk;
    HRESULT hr;

    winetest_push_context( "%s", debugstr_w( class_name ) );
    if (!(factory = get_factory( class_name )))
    {
        winetest_pop_context();
        return;
    }

    hr = IActivationFactory_QueryInterface( factory, &IID_IAgileObject, (void **)&unk );
    ok( hr == S_OK, "QueryInterface IAgileObject returned %#lx.\n", hr );
    if (hr == S_OK) IUnknown_Release( unk );

    hr = IActivationFactory_GetTrustLevel( factory, &trust );
    ok( hr == S_OK, "GetTrustLevel returned %#lx.\n", hr );
    ok( trust == BaseTrust, "Got trust level %d.\n", trust );

    hr = IActivationFactory_GetRuntimeClassName( factory, &name );
    ok( hr == E_ILLEGAL_METHOD_CALL, "GetRuntimeClassName returned %#lx.\n", hr );
    ok( !name, "Got class name %p.\n", name );

    hr = IActivationFactory_ActivateInstance( factory, &instance );
    ok( hr == E_NOTIMPL, "ActivateInstance returned %#lx.\n", hr );
    ok( !instance, "Got instance %p.\n", instance );

    hr = IActivationFactory_QueryInterface( factory, statics_iid, (void **)&statics );
    ok( hr == S_OK || broken( hr == E_NOINTERFACE && statics_iid_win11 ) /* Windows 11 */, "QueryInterface returned %#lx.\n", hr );
    if (hr == E_NOINTERFACE && statics_iid_win11)
    {
        hr = IActivationFactory_QueryInterface( factory, statics_iid_win11, (void **)&statics );
        ok( hr == S_OK, "QueryInterface for the Windows 11 interface returned %#lx.\n", hr );
        statics_iid = statics_iid_win11;
    }
    else if (statics_iid_win11)
    {
        hr = IActivationFactory_QueryInterface( factory, statics_iid_win11, (void **)&unk );
        ok( hr == E_NOINTERFACE, "QueryInterface for the Windows 11 interface returned %#lx.\n", hr );
    }
    if (statics) IInspectable_Release( statics );

    ok( has_iid( (IInspectable *)factory, &IID_IActivationFactory ), "IActivationFactory is not listed.\n" );
    ok( has_iid( (IInspectable *)factory, statics_iid ), "The creation interface is not listed.\n" );

    IActivationFactory_Release( factory );
    winetest_pop_context();
}

static void test_input_site(void)
{
    IActivationFactory *site_factory, *configuration_factory;
    IInspectable *statics = NULL, *configuration_statics = NULL, *site = (IInspectable *)0xdeadbeef, *configuration = NULL, *insp;
    const GUID *site_iid = &IID_site;
    IClosable *closable;
    TrustLevel trust;
    INT32 value;
    LONG refs;
    HRESULT hr;

    if (!(site_factory = get_factory( L"Windows.UI.Internal.Input.InputSite" ))) return;
    hr = IActivationFactory_QueryInterface( site_factory, &IID_site_statics, (void **)&statics );
    if (hr == E_NOINTERFACE)
    {
        hr = IActivationFactory_QueryInterface( site_factory, &IID_site_statics_win11, (void **)&statics );
        is_win11 = TRUE;
        site_iid = &IID_site_win11;
    }
    ok( hr == S_OK, "QueryInterface returned %#lx.\n", hr );
    IActivationFactory_Release( site_factory );
    if (!statics) return;

    host.sink_vtbl = host_sink_vtbl;
    host.object_vtbl = host_object_vtbl;
    host.ref = 1;
    host.sink = CreateEventW( NULL, FALSE, FALSE, NULL );   /* Windows does not look at what the handle is */
    ok( !!host.sink, "CreateEvent failed.\n" );

    /* Windows ends the process when the host refuses or fails: only Wine can be asked */
    if (!strcmp( winetest_platform, "wine" ))
    {
        host.refuse = TRUE;
        hr = SLOT( statics, 6, create_fn )( statics, &host, (void **)&site );
        ok( hr == E_NOINTERFACE, "Creation with a refusing host returned %#lx.\n", hr );
        ok( !site, "Got site %p.\n", site );
        ok( !host.sink_calls, "The sink was asked for %d times.\n", host.sink_calls );
        host.refuse = FALSE;

        host.sink_hr = E_FAIL;
        site = (IInspectable *)0xdeadbeef;
        hr = SLOT( statics, 6, create_fn )( statics, &host, (void **)&site );
        ok( hr == E_FAIL, "Creation with a failing host returned %#lx.\n", hr );
        ok( !site, "Got site %p.\n", site );
        ok( host.sink_calls == 1, "The sink was asked for %d times.\n", host.sink_calls );
        ok( host.ref == 1, "Got %ld references on the host.\n", host.ref );
        host.sink_hr = S_OK;
        host.sink_calls = 0;
    }

    site = NULL;
    hr = SLOT( statics, 6, create_fn )( statics, &host, (void **)&site );
    ok( hr == S_OK, "Creation returned %#lx.\n", hr );
    ok( !!site, "Got no site.\n" );
    ok( host.sink_calls == 1, "The sink was asked for %d times.\n", host.sink_calls );
    ok( host.object_calls == is_win11, "The object was asked for %d times.\n", host.object_calls );
    ok( !host.unexpected_calls, "%d calls of other methods of the host.\n", host.unexpected_calls );
    ok( host.ref > 1, "Got %ld references on the host.\n", host.ref );
    IInspectable_Release( statics );
    if (!site) goto done;

    check_class_name( site, L"Windows.UI.Internal.Input.InputSite" );
    trust = 0xdead;
    hr = IInspectable_GetTrustLevel( site, &trust );
    ok( hr == S_OK, "GetTrustLevel returned %#lx.\n", hr );
    ok( trust == BaseTrust, "Got trust level %d.\n", trust );

    hr = IInspectable_QueryInterface( site, site_iid, (void **)&insp );
    ok( hr == S_OK, "QueryInterface for the default interface returned %#lx.\n", hr );
    if (hr == S_OK)
    {
        ok( insp == site, "The default interface is not the pointer that was returned.\n" );
        IInspectable_Release( insp );
    }
    hr = IInspectable_QueryInterface( site, &IID_IAgileObject, (void **)&insp );
    ok( hr == S_OK, "QueryInterface IAgileObject returned %#lx.\n", hr );
    if (hr == S_OK) IInspectable_Release( insp );
    ok( has_iid( site, site_iid ), "The default interface is not listed.\n" );
    ok( has_iid( site, &IID_IClosable ), "IClosable is not listed.\n" );

    /* the configuration object */
    if ((configuration_factory = get_factory( L"Windows.UI.Internal.Input.ActivationConfigurationInputObject" )))
    {
        hr = IActivationFactory_QueryInterface( configuration_factory, &IID_configuration_statics, (void **)&configuration_statics );
        ok( hr == S_OK, "QueryInterface returned %#lx.\n", hr );
        IActivationFactory_Release( configuration_factory );
    }
    if (configuration_statics)
    {
        hr = SLOT( configuration_statics, 6, create_fn )( configuration_statics, site, (void **)&configuration );
        ok( hr == S_OK, "Creation returned %#lx.\n", hr );
        ok( !!configuration, "Got no object.\n" );
        IInspectable_Release( configuration_statics );
    }
    if (configuration)
    {
        check_class_name( configuration, L"Windows.UI.Internal.Input.ActivationConfigurationInputObject" );
        hr = IInspectable_QueryInterface( configuration, &IID_configuration, (void **)&insp );
        ok( hr == S_OK, "QueryInterface for the default interface returned %#lx.\n", hr );
        if (hr == S_OK)
        {
            ok( insp == configuration, "The default interface is not the pointer that was returned.\n" );
            IInspectable_Release( insp );
        }
        ok( has_iid( configuration, &IID_configuration ), "The default interface is not listed.\n" );
        ok( has_iid( configuration, &IID_IClosable ), "IClosable is not listed.\n" );

        /* two settings: slots 6 and 8 read, 7 and 9 write */
        value = 0xdead;
        hr = SLOT( configuration, 6, get_fn )( configuration, &value );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        ok( !value, "Got %d.\n", value );
        value = 0xdead;
        hr = SLOT( configuration, 8, get_fn )( configuration, &value );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        ok( !value, "Got %d.\n", value );

        hr = SLOT( configuration, 7, put_fn )( configuration, 1 );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        value = 0xdead;
        hr = SLOT( configuration, 6, get_fn )( configuration, &value );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        ok( value == 1, "Got %d.\n", value );
        value = 0xdead;
        hr = SLOT( configuration, 8, get_fn )( configuration, &value );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        ok( !value, "The second setting followed the first: %d.\n", value );

        hr = SLOT( configuration, 9, put_fn )( configuration, 1 );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        hr = SLOT( configuration, 7, put_fn )( configuration, 0 );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        value = 0xdead;
        hr = SLOT( configuration, 8, get_fn )( configuration, &value );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        ok( value == 1, "Got %d.\n", value );
        value = 0xdead;
        hr = SLOT( configuration, 6, get_fn )( configuration, &value );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );
        ok( !value, "Got %d.\n", value );
        hr = SLOT( configuration, 9, put_fn )( configuration, 0 );
        ok( hr == S_OK, "Got hr %#lx.\n", hr );

        hr = IInspectable_QueryInterface( configuration, &IID_IClosable, (void **)&closable );
        ok( hr == S_OK, "QueryInterface IClosable returned %#lx.\n", hr );
        if (hr == S_OK)
        {
            hr = IClosable_Close( closable );
            ok( hr == S_OK, "Close returned %#lx.\n", hr );
            IClosable_Release( closable );
        }
        IInspectable_Release( configuration );
    }

    hr = IInspectable_QueryInterface( site, &IID_IClosable, (void **)&closable );
    ok( hr == S_OK, "QueryInterface IClosable returned %#lx.\n", hr );
    if (hr == S_OK)
    {
        hr = IClosable_Close( closable );
        ok( hr == S_OK, "Close returned %#lx.\n", hr );
        hr = IClosable_Close( closable );
        ok( hr == S_OK, "A second Close returned %#lx.\n", hr );
        IClosable_Release( closable );
    }
    refs = IInspectable_Release( site );
    trace( "site: %ld references left after the last Release, host: %ld references.\n", refs, host.ref );
    if (!strcmp( winetest_platform, "wine" ))
    {
        ok( !refs, "Got %ld references on the site.\n", refs );
        ok( host.ref == 1, "Got %ld references on the host.\n", host.ref );
    }

done:
    CloseHandle( host.sink );
}

START_TEST(inputhost)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize returned %#lx.\n", hr );

    test_factory( L"Windows.UI.Internal.Input.InputSite", &IID_site_statics, &IID_site_statics_win11 );
    test_factory( L"Windows.UI.Internal.Input.ActivationConfigurationInputObject", &IID_configuration_statics, NULL );
    test_input_site();

    RoUninitialize();
}
