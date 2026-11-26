/*
 * Copyright (C) 2025 Mohamad Al-Jaf
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
#define WIDL_using_Windows_UI
#include "windows.ui.h"

#include "wine/test.h"

#define check_interface( obj, iid, is_broken ) check_interface_( __LINE__, obj, iid, is_broken )
static void check_interface_( unsigned int line, void *obj, const IID *iid, BOOL is_broken )
{
    IUnknown *iface = obj;
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_(__FILE__, line)( hr == S_OK || broken( is_broken && hr == E_NOINTERFACE ), "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
        IUnknown_Release( unk );
}

static void test_ColorHelper(void)
{
    static const WCHAR *color_helper_statics_name = L"Windows.UI.ColorHelper";
    IColorHelperStatics *color_helper_statics = (void *)0xdeadbeef;
    IActivationFactory *factory = (void *)0xdeadbeef;
    Color color;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString( color_helper_statics_name, wcslen( color_helper_statics_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( color_helper_statics_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, FALSE );
    check_interface( factory, &IID_IInspectable, FALSE );
    check_interface( factory, &IID_IAgileObject, TRUE /* Missing on Windows older than 1809v2 */ );

    hr = IActivationFactory_QueryInterface( factory, &IID_IColorHelperStatics, (void **)&color_helper_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    memset( &color, 0, sizeof( color ) );
    hr = IColorHelperStatics_FromArgb( color_helper_statics, 255, 255, 255, 255, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IColorHelperStatics_FromArgb( color_helper_statics, 255, 255, 255, 255, &color );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( color.A == 255 && color.R == 255 && color.G == 255 && color.B == 255,
        "got color.A = %u, color.R = %u, color.G = %u, color.B = %u,\n", color.A, color.R, color.G, color.B );

    IColorHelperStatics_Release( color_helper_statics );
    IActivationFactory_Release( factory );
}

static void test_Colors(void)
{
    /* ARGB of every named colour, in the order of IColorsStatics (AliceBlue to YellowGreen) */
    static const UINT32 colors[] =
    {
        0xfff0f8ff, 0xfffaebd7, 0xff00ffff, 0xff7fffd4, 0xfff0ffff, 0xfff5f5dc, 0xffffe4c4, 0xff000000, 0xffffebcd,
        0xff0000ff, 0xff8a2be2, 0xffa52a2a, 0xffdeb887, 0xff5f9ea0, 0xff7fff00, 0xffd2691e, 0xffff7f50, 0xff6495ed,
        0xfffff8dc, 0xffdc143c, 0xff00ffff, 0xff00008b, 0xff008b8b, 0xffb8860b, 0xffa9a9a9, 0xff006400, 0xffbdb76b,
        0xff8b008b, 0xff556b2f, 0xffff8c00, 0xff9932cc, 0xff8b0000, 0xffe9967a, 0xff8fbc8f, 0xff483d8b, 0xff2f4f4f,
        0xff00ced1, 0xff9400d3, 0xffff1493, 0xff00bfff, 0xff696969, 0xff1e90ff, 0xffb22222, 0xfffffaf0, 0xff228b22,
        0xffff00ff, 0xffdcdcdc, 0xfff8f8ff, 0xffffd700, 0xffdaa520, 0xff808080, 0xff008000, 0xffadff2f, 0xfff0fff0,
        0xffff69b4, 0xffcd5c5c, 0xff4b0082, 0xfffffff0, 0xfff0e68c, 0xffe6e6fa, 0xfffff0f5, 0xff7cfc00, 0xfffffacd,
        0xffadd8e6, 0xfff08080, 0xffe0ffff, 0xfffafad2, 0xff90ee90, 0xffd3d3d3, 0xffffb6c1, 0xffffa07a, 0xff20b2aa,
        0xff87cefa, 0xff778899, 0xffb0c4de, 0xffffffe0, 0xff00ff00, 0xff32cd32, 0xfffaf0e6, 0xffff00ff, 0xff800000,
        0xff66cdaa, 0xff0000cd, 0xffba55d3, 0xff9370db, 0xff3cb371, 0xff7b68ee, 0xff00fa9a, 0xff48d1cc, 0xffc71585,
        0xff191970, 0xfff5fffa, 0xffffe4e1, 0xffffe4b5, 0xffffdead, 0xff000080, 0xfffdf5e6, 0xff808000, 0xff6b8e23,
        0xffffa500, 0xffff4500, 0xffda70d6, 0xffeee8aa, 0xff98fb98, 0xffafeeee, 0xffdb7093, 0xffffefd5, 0xffffdab9,
        0xffcd853f, 0xffffc0cb, 0xffdda0dd, 0xffb0e0e6, 0xff800080, 0xffff0000, 0xffbc8f8f, 0xff4169e1, 0xff8b4513,
        0xfffa8072, 0xfff4a460, 0xff2e8b57, 0xfffff5ee, 0xffa0522d, 0xffc0c0c0, 0xff87ceeb, 0xff6a5acd, 0xff708090,
        0xfffffafa, 0xff00ff7f, 0xff4682b4, 0xffd2b48c, 0xff008080, 0xffd8bfd8, 0xffff6347, 0x00ffffff, 0xff40e0d0,
        0xffee82ee, 0xfff5deb3, 0xffffffff, 0xfff5f5f5, 0xffffff00, 0xff9acd32,
    };
    static const WCHAR *class_name = L"Windows.UI.Colors";
    IColorsStatics *statics = (void *)0xdeadbeef;
    IActivationFactory *factory;
    unsigned int i;
    Color color;
    HSTRING str;
    HRESULT hr;

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
    check_interface( factory, &IID_IAgileObject, TRUE );
    hr = IActivationFactory_QueryInterface( factory, &IID_IColorsStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IColorsStatics_get_Transparent( statics, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( sizeof(*statics->lpVtbl) / sizeof(void *) == 6 + ARRAY_SIZE(colors), "got %Iu methods.\n",
        sizeof(*statics->lpVtbl) / sizeof(void *) );
    for (i = 0; i < ARRAY_SIZE(colors); i++)
    {
        HRESULT (WINAPI *get)( IColorsStatics *, Color * ) = ((void **)statics->lpVtbl)[6 + i];

        memset( &color, 0xcc, sizeof(color) );
        hr = get( statics, &color );
        ok( hr == S_OK, "%u: got hr %#lx.\n", i, hr );
        ok( color.A == (BYTE)(colors[i] >> 24) && color.R == (BYTE)(colors[i] >> 16) &&
            color.G == (BYTE)(colors[i] >> 8) && color.B == (BYTE)colors[i],
            "%u: got %02x%02x%02x%02x, expected %08x.\n", i, color.A, color.R, color.G, color.B, colors[i] );
    }

    IColorsStatics_Release( statics );
    IActivationFactory_Release( factory );
}

START_TEST(xaml)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_ColorHelper();
    test_Colors();

    RoUninitialize();
}
