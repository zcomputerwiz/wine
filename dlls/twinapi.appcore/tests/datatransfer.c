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
#define WIDL_using_Windows_ApplicationModel_DataTransfer
#include "windows.applicationmodel.datatransfer.h"

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

static HSTRING hstr( const WCHAR *str )
{
    HSTRING ret;
    WindowsCreateString( str, wcslen( str ), &ret );
    return ret;
}

static void check_string( HSTRING value, const WCHAR *expected, const char *what )
{
    ok( !wcscmp( WindowsGetStringRawBuffer( value, NULL ), expected ), "%s: got %s.\n", what,
        debugstr_w( WindowsGetStringRawBuffer( value, NULL ) ) );
    WindowsDeleteString( value );
}

static void test_StandardDataFormats(void)
{
    IStandardDataFormatsStatics *statics;
    IStandardDataFormatsStatics2 *statics2;
    IStandardDataFormatsStatics3 *statics3;
    IActivationFactory *factory;
    HSTRING value;
    HRESULT hr;

    if (!(factory = get_factory( RuntimeClass_Windows_ApplicationModel_DataTransfer_StandardDataFormats,
                                 &IID_IActivationFactory ))) return;

    hr = IActivationFactory_QueryInterface( factory, &IID_IStandardDataFormatsStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IStandardDataFormatsStatics_get_Text( statics, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"Text", "Text" );
    hr = IStandardDataFormatsStatics_get_Html( statics, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"HTML Format", "Html" );
    hr = IStandardDataFormatsStatics_get_Rtf( statics, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"Rich Text Format", "Rtf" );
    hr = IStandardDataFormatsStatics_get_Uri( statics, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"UniformResourceLocatorW", "Uri" );
    IStandardDataFormatsStatics_Release( statics );

    hr = IActivationFactory_QueryInterface( factory, &IID_IStandardDataFormatsStatics2, (void **)&statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IStandardDataFormatsStatics2_get_ApplicationLink( statics2, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"ApplicationLink", "ApplicationLink" );
    hr = IStandardDataFormatsStatics2_get_WebLink( statics2, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"WebLink", "WebLink" );
    IStandardDataFormatsStatics2_Release( statics2 );

    hr = IActivationFactory_QueryInterface( factory, &IID_IStandardDataFormatsStatics3, (void **)&statics3 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IStandardDataFormatsStatics3_get_UserActivityJsonArray( statics3, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"UserActivityJsonArray", "UserActivityJsonArray" );
    IStandardDataFormatsStatics3_Release( statics3 );

    IActivationFactory_Release( factory );
}

static void test_HtmlFormatHelper(void)
{
    static const WCHAR fragment[] = L"<b>caf\x00e9</b>";
    IHtmlFormatHelperStatics *statics;
    HSTRING input, format, result;
    const WCHAR *buffer;
    HRESULT hr;

    if (!(statics = get_factory( RuntimeClass_Windows_ApplicationModel_DataTransfer_HtmlFormatHelper,
                                 &IID_IHtmlFormatHelperStatics ))) return;

    input = hstr( fragment );
    hr = IHtmlFormatHelperStatics_CreateHtmlFormat( statics, input, &format );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    buffer = WindowsGetStringRawBuffer( format, NULL );
    ok( !wcsncmp( buffer, L"Version:1.0\r\nStartHTML:", 23 ), "got %s.\n", debugstr_w( buffer ) );
    ok( !!wcsstr( buffer, L"<!--StartFragment-->" ), "got %s.\n", debugstr_w( buffer ) );
    ok( !!wcsstr( buffer, L"<!--EndFragment-->" ), "got %s.\n", debugstr_w( buffer ) );

    hr = IHtmlFormatHelperStatics_GetStaticFragment( statics, format, &result );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( result, fragment, "fragment" );
    WindowsDeleteString( format );
    WindowsDeleteString( input );

    input = hstr( L"<html><body><!--StartFragment-->x<!--EndFragment--></body></html>" );
    hr = IHtmlFormatHelperStatics_GetStaticFragment( statics, input, &result );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( result, L"x", "marker fragment" );
    WindowsDeleteString( input );

    input = hstr( L"nothing here" );
    hr = IHtmlFormatHelperStatics_GetStaticFragment( statics, input, &result );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsDeleteString( input );

    IHtmlFormatHelperStatics_Release( statics );
}

static HSTRING get_text( IDataPackageView *view )
{
    IAsyncOperation_HSTRING *operation;
    AsyncStatus status;
    IAsyncInfo *info;
    HSTRING value = NULL;
    HRESULT hr;

    hr = IDataPackageView_GetTextAsync( view, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED( hr )) return NULL;
    hr = IAsyncOperation_HSTRING_QueryInterface( operation, &IID_IAsyncInfo, (void **)&info );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncInfo_get_Status( info, &status );
    ok( hr == S_OK && status == Completed, "got hr %#lx, status %d.\n", hr, status );
    IAsyncInfo_Release( info );
    hr = IAsyncOperation_HSTRING_GetResults( operation, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncOperation_HSTRING_Release( operation );
    return value;
}

static void test_DataPackage(void)
{
    IDataPackagePropertySetView *props_view;
    IDataPackagePropertySet *props;
    IVectorView_HSTRING *formats;
    IDataPackageView *view;
    IActivationFactory *factory;
    IDataPackage *package;
    IInspectable *inspectable;
    HSTRING str, value;
    boolean found;
    UINT32 size;
    HRESULT hr;
    ULONG count;
    IID *iids;

    if (!(factory = get_factory( RuntimeClass_Windows_ApplicationModel_DataTransfer_DataPackage,
                                 &IID_IActivationFactory ))) return;

    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( inspectable, &IID_IDataPackage, (void **)&package );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_GetIids( inspectable, &count, &iids );
    ok( hr == S_OK && count == 4, "got hr %#lx, count %lu.\n", hr, count );
    CoTaskMemFree( iids );
    hr = IInspectable_GetRuntimeClassName( inspectable, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"Windows.ApplicationModel.DataTransfer.DataPackage", "class name" );
    IInspectable_Release( inspectable );

    str = hstr( L"hello" );
    hr = IDataPackage_SetText( package, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    str = hstr( L"<b>hi</b>" );
    hr = IDataPackage_SetHtmlFormat( package, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    hr = IDataPackage_GetView( package, &view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    str = hstr( L"text" );
    hr = IDataPackageView_Contains( view, str, &found );
    ok( hr == S_OK && found, "got hr %#lx, found %d.\n", hr, found );
    WindowsDeleteString( str );
    str = hstr( L"Bitmap" );
    hr = IDataPackageView_Contains( view, str, &found );
    ok( hr == S_OK && !found, "got hr %#lx, found %d.\n", hr, found );
    WindowsDeleteString( str );

    check_string( get_text( view ), L"hello", "text" );

    hr = IDataPackageView_get_AvailableFormats( view, &formats );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVectorView_HSTRING_get_Size( formats, &size );
    ok( hr == S_OK && size == 2, "got hr %#lx, size %u.\n", hr, size );
    hr = IVectorView_HSTRING_GetAt( formats, 0, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"Text", "format 0" );
    hr = IVectorView_HSTRING_GetAt( formats, 2, &value );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    IVectorView_HSTRING_Release( formats );

    hr = IDataPackage_get_Properties( package, &props );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    str = hstr( L"A title" );
    hr = IDataPackagePropertySet_put_Title( props, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = IDataPackageView_get_Properties( view, &props_view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IDataPackagePropertySetView_get_Title( props_view, &value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_string( value, L"A title", "title" );
    IDataPackagePropertySetView_Release( props_view );
    IDataPackagePropertySet_Release( props );

    IDataPackageView_Release( view );
    IDataPackage_Release( package );
    IActivationFactory_Release( factory );
}

static void test_Clipboard(void)
{
    IClipboardStatics *statics;
    IClipboardStatics2 *statics2;
    IDataPackageView *view;
    IDataPackage *package;
    IInspectable *inspectable;
    IActivationFactory *factory;
    HSTRING str, value;
    boolean found, flag;
    HRESULT hr;

    if (!(statics = get_factory( RuntimeClass_Windows_ApplicationModel_DataTransfer_Clipboard,
                                 &IID_IClipboardStatics ))) return;
    hr = IClipboardStatics_QueryInterface( statics, &IID_IClipboardStatics2, (void **)&statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    flag = TRUE;
    hr = IClipboardStatics2_IsHistoryEnabled( statics2, &flag );
    ok( hr == S_OK && !flag, "got hr %#lx, flag %d.\n", hr, flag );
    flag = TRUE;
    hr = IClipboardStatics2_IsRoamingEnabled( statics2, &flag );
    ok( hr == S_OK && !flag, "got hr %#lx, flag %d.\n", hr, flag );
    IClipboardStatics2_Release( statics2 );

    factory = get_factory( RuntimeClass_Windows_ApplicationModel_DataTransfer_DataPackage, &IID_IActivationFactory );
    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IInspectable_QueryInterface( inspectable, &IID_IDataPackage, (void **)&package );
    IInspectable_Release( inspectable );
    IActivationFactory_Release( factory );

    str = hstr( L"clip \x00e9 text" );
    hr = IDataPackage_SetText( package, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    str = hstr( L"Version:1.0\r\n<b>x</b>" );
    hr = IDataPackage_SetHtmlFormat( package, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    hr = IClipboardStatics_SetContent( statics, package );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IClipboardStatics_Flush( statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IDataPackage_Release( package );

    hr = IClipboardStatics_GetContent( statics, &view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    str = hstr( L"Text" );
    hr = IDataPackageView_Contains( view, str, &found );
    ok( hr == S_OK && found, "got hr %#lx, found %d.\n", hr, found );
    WindowsDeleteString( str );
    str = hstr( L"HTML Format" );
    hr = IDataPackageView_Contains( view, str, &found );
    ok( hr == S_OK && found, "got hr %#lx, found %d.\n", hr, found );
    WindowsDeleteString( str );
    value = get_text( view );
    check_string( value, L"clip \x00e9 text", "clipboard text" );
    IDataPackageView_Release( view );

    hr = IClipboardStatics_Clear( statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IClipboardStatics_GetContent( statics, &view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    str = hstr( L"Text" );
    hr = IDataPackageView_Contains( view, str, &found );
    ok( hr == S_OK && !found, "got hr %#lx, found %d.\n", hr, found );
    WindowsDeleteString( str );
    IDataPackageView_Release( view );

    IClipboardStatics_Release( statics );
}

START_TEST(datatransfer)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_StandardDataFormats();
    test_HtmlFormatHelper();
    test_DataPackage();
    test_Clipboard();

    RoUninitialize();
}
