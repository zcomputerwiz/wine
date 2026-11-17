/*
 * Copyright 2021 Rémi Bernon for CodeWeavers
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
#include <stdarg.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winstring.h"

#include "initguid.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Globalization
#include "windows.globalization.h"
#define WIDL_using_Windows_Globalization_NumberFormatting
#include "windows.globalization.numberformatting.h"
#define WIDL_using_Windows_Globalization_DateTimeFormatting
#include "windows.globalization.datetimeformatting.h"
#define WIDL_using_Windows_Globalization_Fonts
#define WIDL_using_Windows_UI_Text
#include "windows.globalization.fonts.h"
#define WIDL_using_Windows_System_UserProfile
#include "windows.system.userprofile.h"

#include "wine/test.h"

/* kernel32.dll */
static INT (WINAPI *pGetUserDefaultGeoName)(LPWSTR, int);

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

static void test_GlobalizationPreferences(void)
{
    static const WCHAR *class_name = L"Windows.System.UserProfile.GlobalizationPreferences";

    IGlobalizationPreferencesStatics *preferences_statics = NULL;
    IVectorView_HSTRING *languages = NULL, *calendars, *clocks, *currencies;
    IActivationFactory *factory = NULL;
    IInspectable *inspectable = NULL, *tmp_inspectable = NULL;
    IAgileObject *agile_object = NULL, *tmp_agile_object = NULL;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    HSTRING str, tmp_str;
    const WCHAR *buf;
    BOOLEAN found;
    HRESULT hr;
    UINT32 len;
    UINT32 i, size;

    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

    hr = WindowsCreateString(class_name, wcslen(class_name), &str);
    ok(hr == S_OK, "WindowsCreateString failed, hr %#lx\n", hr);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == S_OK || broken(hr == REGDB_E_CLASSNOTREG), "RoGetActivationFactory failed, hr %#lx\n", hr);
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip("%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w(class_name));
        WindowsDeleteString(str);
        return;
    }

    hr = IActivationFactory_QueryInterface(factory, &IID_IInspectable, (void **)&inspectable);
    ok(hr == S_OK, "IActivationFactory_QueryInterface IID_IInspectable failed, hr %#lx\n", hr);

    hr = IActivationFactory_QueryInterface(factory, &IID_IAgileObject, (void **)&agile_object);
    ok(hr == S_OK, "IActivationFactory_QueryInterface IID_IAgileObject failed, hr %#lx\n", hr);

    hr = IActivationFactory_QueryInterface(factory, &IID_IGlobalizationPreferencesStatics, (void **)&preferences_statics);
    ok(hr == S_OK, "IActivationFactory_QueryInterface IID_IGlobalizationPreferencesStatics failed, hr %#lx\n", hr);

    hr = IGlobalizationPreferencesStatics_QueryInterface(preferences_statics, &IID_IInspectable, (void **)&tmp_inspectable);
    ok(hr == S_OK, "IGlobalizationPreferencesStatics_QueryInterface IID_IInspectable failed, hr %#lx\n", hr);
    ok(tmp_inspectable == inspectable, "IGlobalizationPreferencesStatics_QueryInterface IID_IInspectable returned %p, expected %p\n", tmp_inspectable, inspectable);
    IInspectable_Release(tmp_inspectable);

    hr = IGlobalizationPreferencesStatics_QueryInterface(preferences_statics, &IID_IAgileObject, (void **)&tmp_agile_object);
    ok(hr == S_OK, "IGlobalizationPreferencesStatics_QueryInterface IID_IAgileObject failed, hr %#lx\n", hr);
    ok(tmp_agile_object == agile_object, "IGlobalizationPreferencesStatics_QueryInterface IID_IAgileObject returned %p, expected %p\n", tmp_agile_object, agile_object);
    IAgileObject_Release(tmp_agile_object);

    hr = IGlobalizationPreferencesStatics_get_HomeGeographicRegion(preferences_statics, &tmp_str);
    ok(hr == S_OK, "IGlobalizationPreferencesStatics_get_HomeGeographicRegion failed, hr %#lx\n", hr);

    buf = WindowsGetStringRawBuffer(tmp_str, &len);
    ok(buf != NULL && len > 0, "WindowsGetStringRawBuffer returned buf %p, len %u\n", buf, len);
    if (pGetUserDefaultGeoName)
    {
        WCHAR country[16];
        DWORD geolen;
        geolen = pGetUserDefaultGeoName(country, ARRAY_SIZE(country));
        ok(broken(geolen == 1) || /* Win10 1709 */
           (wcslen(country) == len && !memcmp(buf, country, len)),
           "IGlobalizationPreferencesStatics_get_HomeGeographicRegion returned len %u, str %s, expected %s\n",
           len, wine_dbgstr_w(buf), wine_dbgstr_w(country));
    }

    WindowsDeleteString(tmp_str);

    hr = IGlobalizationPreferencesStatics_get_Languages(preferences_statics, &languages);
    ok(hr == S_OK, "IGlobalizationPreferencesStatics_get_Languages failed, hr %#lx\n", hr);

    hr = IVectorView_HSTRING_QueryInterface(languages, &IID_IInspectable, (void **)&tmp_inspectable);
    ok(hr == S_OK, "IVectorView_HSTRING_QueryInterface failed, hr %#lx\n", hr);
    ok(tmp_inspectable != inspectable, "IVectorView_HSTRING_QueryInterface returned %p, expected %p\n", tmp_inspectable, inspectable);
    IInspectable_Release(tmp_inspectable);

    hr = IVectorView_HSTRING_QueryInterface(languages, &IID_IAgileObject, (void **)&tmp_agile_object);
    ok(hr == S_OK, "IVectorView_HSTRING_QueryInterface failed, hr %#lx\n", hr);
    ok(tmp_agile_object != agile_object, "IVectorView_HSTRING_QueryInterface IID_IAgileObject returned agile_object\n");
    IAgileObject_Release(tmp_agile_object);

    size = 0xdeadbeef;
    hr = IVectorView_HSTRING_get_Size(languages, &size);
    ok(hr == S_OK, "IVectorView_HSTRING_get_Size failed, hr %#lx\n", hr);
    ok(size != 0 && size != 0xdeadbeef, "IVectorView_HSTRING_get_Size returned %u\n", size);

    hr = IVectorView_HSTRING_GetAt(languages, 0, &tmp_str);
    ok(hr == S_OK, "IVectorView_HSTRING_GetAt failed, hr %#lx\n", hr);
    buf = WindowsGetStringRawBuffer(tmp_str, &len);
    ok(buf != NULL && len > 0, "WindowsGetStringRawBuffer returned buf %p, len %u\n", buf, len);

    ok(wcslen(locale) == len && !memcmp(buf, locale, len),
       "IGlobalizationPreferencesStatics_get_Languages 0 returned len %u, str %s, expected %s\n",
       len, wine_dbgstr_w(buf), wine_dbgstr_w(locale));

    i = 0xdeadbeef;
    found = FALSE;
    hr = IVectorView_HSTRING_IndexOf(languages, tmp_str, &i, &found);
    ok(hr == S_OK, "IVectorView_HSTRING_IndexOf failed, hr %#lx\n", hr);
    ok(i == 0 && found == TRUE, "IVectorView_HSTRING_IndexOf returned size %d, found %d\n", size, found);

    WindowsDeleteString(tmp_str);

    hr = WindowsCreateString(L"deadbeef", 8, &tmp_str);
    ok(hr == S_OK, "WindowsCreateString failed, hr %#lx\n", hr);

    i = 0xdeadbeef;
    found = TRUE;
    hr = IVectorView_HSTRING_IndexOf(languages, tmp_str, &i, &found);
    ok(hr == S_OK, "IVectorView_HSTRING_IndexOf failed, hr %#lx\n", hr);
    ok(i == 0 && found == FALSE, "IVectorView_HSTRING_IndexOf returned size %d, found %d\n", size, found);

    WindowsDeleteString(tmp_str);

    tmp_str = (HSTRING)0xdeadbeef;
    hr = IVectorView_HSTRING_GetAt(languages, size, &tmp_str);
    ok(hr == E_BOUNDS, "IVectorView_HSTRING_GetAt failed, hr %#lx\n", hr);
    ok(tmp_str == NULL, "IVectorView_HSTRING_GetAt returned %p\n", tmp_str);

    tmp_str = (HSTRING)0xdeadbeef;
    hr = IVectorView_HSTRING_GetMany(languages, size, 1, &tmp_str, &i);
    ok(hr == S_OK, "IVectorView_HSTRING_GetAt failed, hr %#lx\n", hr);
    ok(i == 0 && tmp_str == NULL, "IVectorView_HSTRING_GetMany returned count %u, str %p\n", i, tmp_str);

    hr = IVectorView_HSTRING_GetMany(languages, 0, 1, &tmp_str, &i);
    ok(hr == S_OK, "IVectorView_HSTRING_GetAt failed, hr %#lx\n", hr);
    ok(i == 1, "IVectorView_HSTRING_GetMany returned count %u, expected 1\n", i);

    buf = WindowsGetStringRawBuffer(tmp_str, &len);
    ok(buf != NULL && len > 0, "WindowsGetStringRawBuffer returned buf %p, len %u\n", buf, len);

    ok(wcslen(locale) == len && !memcmp(buf, locale, len),
       "IGlobalizationPreferencesStatics_get_Languages 0 returned len %u, str %s, expected %s\n",
       len, wine_dbgstr_w(buf), wine_dbgstr_w(locale));

    WindowsDeleteString(tmp_str);

    IVectorView_HSTRING_Release(languages);


    hr = IGlobalizationPreferencesStatics_get_Calendars(preferences_statics, &calendars);
    ok(hr == S_OK, "IGlobalizationPreferencesStatics_get_Calendars failed, hr %#lx\n", hr);

    size = 0xdeadbeef;
    hr = IVectorView_HSTRING_get_Size(calendars, &size);
    ok(hr == S_OK, "IVectorView_HSTRING_get_Size failed, hr %#lx\n", hr);
    todo_wine ok(size != 0 && size != 0xdeadbeef, "IVectorView_HSTRING_get_Size returned %u\n", size);

    IVectorView_HSTRING_Release(calendars);


    hr = IGlobalizationPreferencesStatics_get_Clocks(preferences_statics, &clocks);
    ok(hr == S_OK, "IGlobalizationPreferencesStatics_get_Clocks failed, hr %#lx\n", hr);

    size = 0xdeadbeef;
    hr = IVectorView_HSTRING_get_Size(clocks, &size);
    ok(hr == S_OK, "IVectorView_HSTRING_get_Size failed, hr %#lx\n", hr);
    todo_wine ok(size != 0 && size != 0xdeadbeef, "IVectorView_HSTRING_get_Size returned %u\n", size);

    IVectorView_HSTRING_Release(clocks);


    hr = IGlobalizationPreferencesStatics_get_Currencies(preferences_statics, &currencies);
    ok(hr == S_OK, "IGlobalizationPreferencesStatics_get_Currencies failed, hr %#lx\n", hr);

    size = 0xdeadbeef;
    hr = IVectorView_HSTRING_get_Size(currencies, &size);
    ok(hr == S_OK, "IVectorView_HSTRING_get_Size failed, hr %#lx\n", hr);
    todo_wine ok(size != 0 && size != 0xdeadbeef, "IVectorView_HSTRING_get_Size returned %u\n", size);

    IVectorView_HSTRING_Release(currencies);


    IGlobalizationPreferencesStatics_Release(preferences_statics);

    IAgileObject_Release(agile_object);
    IInspectable_Release(inspectable);
    IActivationFactory_Release(factory);

    WindowsDeleteString(str);
}

static void test_Language(void)
{
    static const WCHAR *class_name = L"Windows.Globalization.Language";
    static const struct
    {
        const WCHAR *tag;
        enum LanguageLayoutDirection direction;
    }
    lang_layout_direction_tests[] =
    {
        {L"en-US", LanguageLayoutDirection_Ltr},
        {L"ar-SA", LanguageLayoutDirection_Rtl},
    };
    IAgileObject *agile_object, *tmp_agile_object;
    IInspectable *inspectable, *tmp_inspectable;
    enum LanguageLayoutDirection direction;
    WCHAR buffer[LOCALE_NAME_MAX_LENGTH];
    ILanguageFactory *language_factory;
    IActivationFactory *factory;
    ILanguage2 *language2;
    ILanguage *language;
    HSTRING tag, str;
    const WCHAR *buf;
    unsigned int i;
    HRESULT hr;

    hr = WindowsCreateString(class_name, wcslen(class_name), &str);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == S_OK || broken(hr == REGDB_E_CLASSNOTREG), "Unexpected hr %#lx.\n", hr);
    WindowsDeleteString(str);
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip("%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w(class_name));
        return;
    }

    hr = IActivationFactory_QueryInterface(factory, &IID_IInspectable, (void **)&inspectable);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = IActivationFactory_QueryInterface(factory, &IID_IAgileObject, (void **)&agile_object);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = IActivationFactory_QueryInterface(factory, &IID_ILanguageFactory, (void **)&language_factory);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = ILanguageFactory_QueryInterface(language_factory, &IID_IInspectable, (void **)&tmp_inspectable);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    ok(tmp_inspectable == inspectable, "Unexpected interface pointer %p, expected %p.\n", tmp_inspectable, inspectable);
    IInspectable_Release(tmp_inspectable);

    hr = ILanguageFactory_QueryInterface(language_factory, &IID_IAgileObject, (void **)&tmp_agile_object);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    ok(tmp_agile_object == agile_object, "Unexpected interface pointer %p, expected %p.\n", tmp_agile_object, agile_object);
    IAgileObject_Release(tmp_agile_object);

    /* Invalid language tag */
    hr = WindowsCreateString(L"test-tag", 8, &str);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = ILanguageFactory_CreateLanguage(language_factory, str, &language);
    ok(hr == E_INVALIDARG, "Unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        ILanguage_Release(language);
    WindowsDeleteString(str);

    hr = WindowsCreateString(L"en-us", 5, &str);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    hr = ILanguageFactory_CreateLanguage(language_factory, str, &language);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    WindowsDeleteString(str);

    hr = ILanguage_get_LanguageTag(language, &tag);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    buf = WindowsGetStringRawBuffer(tag, NULL);
    ok(!wcscmp(buf, L"en-US"), "Unexpected tag %s.\n", debugstr_w(buf));
    GetLocaleInfoEx(L"en-us", LOCALE_SNAME, buffer, ARRAY_SIZE(buffer));
    ok(!wcscmp(buf, buffer), "Unexpected tag %s, locale name %s.\n", debugstr_w(buf), debugstr_w(buffer));

    WindowsDeleteString(tag);

    ILanguage_Release(language);

    /* ILanguage2 */
    for (i = 0; i < ARRAY_SIZE(lang_layout_direction_tests); i++)
    {
        winetest_push_context("%s", wine_dbgstr_w(lang_layout_direction_tests[i].tag));

        hr = WindowsCreateString(lang_layout_direction_tests[i].tag, wcslen(lang_layout_direction_tests[i].tag), &str);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
        hr = ILanguageFactory_CreateLanguage(language_factory, str, &language);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
        WindowsDeleteString(str);

        hr = ILanguage_QueryInterface(language, &IID_ILanguage2, (void **)&language2);
        ok(hr == S_OK || broken(hr == E_NOINTERFACE) /* <= Windows 10 1507 */, "Unexpected hr %#lx.\n", hr);
        if (FAILED(hr))
        {
            ILanguage_Release(language);
            winetest_pop_context();
            break;
        }

        hr = ILanguage2_get_LayoutDirection(language2, &direction);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
        ok(direction == lang_layout_direction_tests[i].direction, "Expected direction %d, got %d.\n",
           lang_layout_direction_tests[i].direction, direction);

        ILanguage2_Release(language2);
        ILanguage_Release(language);
        winetest_pop_context();
    }

    ILanguageFactory_Release(language_factory);

    IAgileObject_Release(agile_object);
    IInspectable_Release(inspectable);
    IActivationFactory_Release(factory);
}

static void test_GeographicRegion(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_Globalization_GeographicRegion;
    IGeographicRegionFactory *geographic_region_factory;
    IGeographicRegion *geographic_region;
    IActivationFactory *factory;
    HSTRING str, expect_str;
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

    check_interface( factory, &IID_IUnknown );
    check_interface( factory, &IID_IInspectable );
    check_interface( factory, &IID_IAgileObject );

    hr = IActivationFactory_ActivateInstance( factory, (IInspectable **)&geographic_region );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( geographic_region, &IID_IUnknown );
    check_interface( geographic_region, &IID_IInspectable );
    check_interface( geographic_region, &IID_IAgileObject );

    hr = IGeographicRegion_get_CodeTwoLetter( geographic_region, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    ref = IGeographicRegion_Release( geographic_region );
    ok( ref == 0, "got ref %ld.\n", ref );

    hr = IActivationFactory_QueryInterface( factory, &IID_IGeographicRegionFactory, (void **)&geographic_region_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = WindowsCreateString( L"US", wcslen( L"US" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IGeographicRegionFactory_CreateGeographicRegion( geographic_region_factory, str, &geographic_region );
    todo_wine ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    if (hr == S_OK)
    {
    hr = WindowsCreateString( L"US", wcslen( L"US" ), &expect_str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IGeographicRegion_get_CodeTwoLetter( geographic_region, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = WindowsCompareStringOrdinal( str, expect_str, &res );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !res, "got unexpected string %s.\n", debugstr_hstring(str) );
    WindowsDeleteString( str );
    WindowsDeleteString( expect_str );

    ref = IGeographicRegion_Release( geographic_region );
    ok( ref == 0, "got ref %ld.\n", ref );
    }

    ref = IGeographicRegionFactory_Release( geographic_region_factory );
    ok( ref == 2, "got ref %ld.\n", ref );

    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

static void test_ApplicationLanguages(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_Globalization_ApplicationLanguages;
    IApplicationLanguagesStatics *application_languages_statics;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    IVectorView_HSTRING *languages;
    IActivationFactory *factory;
    BOOL found = FALSE;
    const WCHAR *ptr;
    UINT32 size, i;
    HSTRING str;
    HRESULT hr;
    LONG ref;
    int ret;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (FAILED( hr ))
    {
        win_skip( "%s runtimeclass not found, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown );
    check_interface( factory, &IID_IInspectable );
    check_interface( factory, &IID_IAgileObject );
    check_interface( factory, &IID_IActivationFactory );
    check_interface( factory, &IID_IApplicationLanguagesStatics );

    hr = IActivationFactory_QueryInterface( factory, &IID_IApplicationLanguagesStatics, (void **)&application_languages_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IApplicationLanguagesStatics_get_Languages( application_languages_statics, &languages );
    ok( hr == S_OK || broken( hr == 0x80073d54 ) /* Win8 */, "got hr %#lx.\n", hr );
    if (FAILED( hr ))
        goto done;

    hr = IVectorView_HSTRING_get_Size( languages, &size );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( size > 0, "Got unexpected %u.\n", size );

    ret = GetUserDefaultLocaleName( locale, LOCALE_NAME_MAX_LENGTH );
    ok( ret > 0, "GetUserDefaultLocaleName failed, error %lu.\n", GetLastError() );

    for (i = 0; i < size; i++)
    {
        hr = IVectorView_HSTRING_GetAt( languages, i, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ptr = WindowsGetStringRawBuffer( str, NULL );
        found = !wcscmp( ptr, locale );
        WindowsDeleteString( str );
        if (found)
            break;
    }
    ok( found, "Language not found.\n" );

done:
    ref = IApplicationLanguagesStatics_Release( application_languages_statics );
    ok( ref == 2, "got ref %ld.\n", ref );
    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

/* IIterable<HSTRING> over a list of strings, for the Calendar factories */
struct hstring_iterable
{
    IIterable_HSTRING IIterable_HSTRING_iface;
    IIterator_HSTRING IIterator_HSTRING_iface;
    const WCHAR **items;
    UINT32 count, pos;
};

static struct hstring_iterable *iterable_from_IIterable( IIterable_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterable, IIterable_HSTRING_iface );
}

static struct hstring_iterable *iterable_from_IIterator( IIterator_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterable, IIterator_HSTRING_iface );
}

static HRESULT WINAPI iterable_QueryInterface( IIterable_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterable_HSTRING ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterable_AddRef( IIterable_HSTRING *iface ) { return 2; }
static ULONG WINAPI iterable_Release( IIterable_HSTRING *iface ) { return 1; }
static HRESULT WINAPI iterable_GetIids( IIterable_HSTRING *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI iterable_GetRuntimeClassName( IIterable_HSTRING *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI iterable_GetTrustLevel( IIterable_HSTRING *iface, TrustLevel *level ) { return E_NOTIMPL; }

static HRESULT WINAPI iterable_First( IIterable_HSTRING *iface, IIterator_HSTRING **value )
{
    struct hstring_iterable *impl = iterable_from_IIterable( iface );
    impl->pos = 0;
    *value = &impl->IIterator_HSTRING_iface;
    return S_OK;
}

static const IIterable_HSTRINGVtbl iterable_vtbl =
{
    iterable_QueryInterface, iterable_AddRef, iterable_Release,
    iterable_GetIids, iterable_GetRuntimeClassName, iterable_GetTrustLevel,
    iterable_First,
};

static HRESULT WINAPI iterator_QueryInterface( IIterator_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_HSTRING ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterator_AddRef( IIterator_HSTRING *iface ) { return 2; }
static ULONG WINAPI iterator_Release( IIterator_HSTRING *iface ) { return 1; }
static HRESULT WINAPI iterator_GetIids( IIterator_HSTRING *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI iterator_GetRuntimeClassName( IIterator_HSTRING *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI iterator_GetTrustLevel( IIterator_HSTRING *iface, TrustLevel *level ) { return E_NOTIMPL; }

static HRESULT WINAPI iterator_get_Current( IIterator_HSTRING *iface, HSTRING *value )
{
    struct hstring_iterable *impl = iterable_from_IIterator( iface );
    if (impl->pos >= impl->count) return E_BOUNDS;
    return WindowsCreateString( impl->items[impl->pos], wcslen( impl->items[impl->pos] ), value );
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterable *impl = iterable_from_IIterator( iface );
    *value = impl->pos < impl->count;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterable *impl = iterable_from_IIterator( iface );
    if (impl->pos < impl->count) impl->pos++;
    *value = impl->pos < impl->count;
    return S_OK;
}

static HRESULT WINAPI iterator_GetMany( IIterator_HSTRING *iface, UINT32 size, HSTRING *items, UINT32 *count )
{
    return E_NOTIMPL;
}

static const IIterator_HSTRINGVtbl iterator_vtbl =
{
    iterator_QueryInterface, iterator_AddRef, iterator_Release,
    iterator_GetIids, iterator_GetRuntimeClassName, iterator_GetTrustLevel,
    iterator_get_Current, iterator_get_HasCurrent, iterator_MoveNext, iterator_GetMany,
};

static void hstring_iterable_init( struct hstring_iterable *iterable, const WCHAR **items, UINT32 count )
{
    iterable->IIterable_HSTRING_iface.lpVtbl = &iterable_vtbl;
    iterable->IIterator_HSTRING_iface.lpVtbl = &iterator_vtbl;
    iterable->items = items;
    iterable->count = count;
    iterable->pos = 0;
}

/* 100 ns units since 1601-01-01 UTC (Windows.Foundation.DateTime) */
static INT64 datetime( WORD year, WORD month, WORD day, WORD hour, WORD minute, WORD second, INT64 extra )
{
    SYSTEMTIME st = { year, month, 0, day, hour, minute, second, 0 };
    FILETIME ft;
    SystemTimeToFileTime( &st, &ft );
    return (((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + extra;
}

static HSTRING hstr( const WCHAR *s )
{
    HSTRING h = NULL;
    WindowsCreateString( s, wcslen( s ), &h );
    return h;
}

#define check_str( call, expect ) check_str_( __LINE__, #call, call, &str, expect )
static void check_str_( int line, const char *what, HRESULT hr, HSTRING *str, const WCHAR *expect )
{
    ok_( __FILE__, line )( hr == S_OK, "%s: got hr %#lx.\n", what, hr );
    if (FAILED(hr)) return;
    ok_( __FILE__, line )( !wcscmp( WindowsGetStringRawBuffer( *str, NULL ), expect ), "%s: got %s, expected %s.\n",
                           what, debugstr_hstring( *str ), debugstr_w( expect ) );
    WindowsDeleteString( *str );
}

#define trace_str( call ) trace_str_( #call, call, &str )
static void trace_str_( const char *what, HRESULT hr, HSTRING *str )
{
    trace( "%s: %#lx %s\n", what, hr, SUCCEEDED(hr) ? debugstr_hstring( *str ) : "" );
    if (SUCCEEDED(hr)) WindowsDeleteString( *str );
}

#define check_int( call, expect ) check_int_( __LINE__, #call, (call), expect )
static INT32 int_value;
#define get_int( getter ) (int_value = -12345, getter( calendar, &int_value ), int_value)
static void check_int_( int line, const char *what, INT32 value, INT32 expect )
{
    ok_( __FILE__, line )( value == expect, "%s: got %d, expected %d.\n", what, value, expect );
}

#define check_ymd( calendar, y, mo, d, h, mi, s, ns ) check_ymd_( __LINE__, calendar, y, mo, d, h, mi, s, ns )
static void check_ymd_( int line, ICalendar *calendar, INT32 ey, INT32 emo, INT32 ed, INT32 eh, INT32 emi, INT32 es, INT32 ens )
{
    INT32 y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ns = 0;
    ICalendar_get_Year( calendar, &y );
    ICalendar_get_Month( calendar, &mo );
    ICalendar_get_Day( calendar, &d );
    ICalendar_get_Hour( calendar, &h );
    ICalendar_get_Minute( calendar, &mi );
    ICalendar_get_Second( calendar, &s );
    ICalendar_get_Nanosecond( calendar, &ns );
    ok_( __FILE__, line )( y == ey && mo == emo && d == ed && h == eh && mi == emi && s == es && ns == ens,
                           "got %04d-%02d-%02d %02d:%02d:%02d.%09d, expected %04d-%02d-%02d %02d:%02d:%02d.%09d.\n",
                           y, mo, d, h, mi, s, ns, ey, emo, ed, eh, emi, es, ens );
}

static void test_Calendar(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_Globalization_Calendar;
    static const WCHAR *en_us[] = { L"en-US" };
    static const WCHAR *de_de[] = { L"de-DE" };
    struct hstring_iterable languages, german;
    ICalendarFactory2 *factory2;
    ICalendarFactory *factory1;
    ICalendar *calendar, *other;
    ITimeZoneOnCalendar *tz;
    IActivationFactory *factory;
    IVectorView_HSTRING *view;
    HSTRING str, s1, s2, s3, s4;
    DateTime when;
    DayOfWeek dow;
    boolean dst;
    UINT32 size;
    INT32 res, ns;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (FAILED( hr ))
    {
        win_skip( "%s runtimeclass not found, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }
    check_interface( factory, &IID_IAgileObject );
    check_interface( factory, &IID_ICalendarFactory );
    check_interface( factory, &IID_ICalendarFactory2 );

    /* the default calendar: the user's settings */
    hr = IActivationFactory_ActivateInstance( factory, (IInspectable **)&calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_interface( calendar, &IID_ICalendar );
    check_interface( calendar, &IID_ITimeZoneOnCalendar );
    check_interface( calendar, &IID_IAgileObject );
    check_str( ICalendar_GetCalendarSystem( calendar, &str ), L"GregorianCalendar" );
    check_str( ICalendar_get_NumeralSystem( calendar, &str ), L"Latn" );
    trace_str( ICalendar_GetClock( calendar, &str ) );
    trace_str( ICalendar_get_ResolvedLanguage( calendar, &str ) );
    hr = ICalendar_get_Languages( calendar, &view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    size = 0;
    if (SUCCEEDED(hr))
    {
        IVectorView_HSTRING_get_Size( view, &size );
        IVectorView_HSTRING_Release( view );
    }
    ok( size >= 1, "got %u languages.\n", size );
    hr = ICalendar_QueryInterface( calendar, &IID_ITimeZoneOnCalendar, (void **)&tz );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    trace_str( ITimeZoneOnCalendar_GetTimeZone( tz, &str ) );
    ITimeZoneOnCalendar_Release( tz );
    ICalendar_Release( calendar );

    /* en-US, Gregorian, 24 hours, UTC: deterministic */
    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory2, (void **)&factory2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hstring_iterable_init( &languages, en_us, 1 );
    s1 = hstr( L"GregorianCalendar" );
    s2 = hstr( L"24HourClock" );
    s3 = hstr( L"UTC" );
    hr = ICalendarFactory2_CreateCalendarWithTimeZone( factory2, &languages.IIterable_HSTRING_iface, s1, s2, s3, &calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) goto done;

    when.UniversalTime = datetime( 2024, 2, 29, 13, 45, 30, 1234567 );
    hr = ICalendar_SetDateTime( calendar, when );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_ymd( calendar, 2024, 2, 29, 13, 45, 30, 123456700 );
    check_int( get_int( ICalendar_get_Era ), 1 );
    check_int( get_int( ICalendar_get_FirstEra ), 1 );
    check_int( get_int( ICalendar_get_LastEra ), 1 );
    check_int( get_int( ICalendar_get_NumberOfEras ), 1 );
    check_int( get_int( ICalendar_get_FirstYearInThisEra ), 1 );
    check_int( get_int( ICalendar_get_LastYearInThisEra ), 9999 );
    check_int( get_int( ICalendar_get_NumberOfYearsInThisEra ), 9999 );
    check_int( get_int( ICalendar_get_FirstMonthInThisYear ), 1 );
    check_int( get_int( ICalendar_get_LastMonthInThisYear ), 12 );
    check_int( get_int( ICalendar_get_NumberOfMonthsInThisYear ), 12 );
    check_int( get_int( ICalendar_get_FirstDayInThisMonth ), 1 );
    check_int( get_int( ICalendar_get_LastDayInThisMonth ), 29 );
    check_int( get_int( ICalendar_get_NumberOfDaysInThisMonth ), 29 );
    check_int( get_int( ICalendar_get_Period ), 1 );
    check_int( get_int( ICalendar_get_FirstPeriodInThisDay ), 1 );
    check_int( get_int( ICalendar_get_LastPeriodInThisDay ), 1 );
    check_int( get_int( ICalendar_get_NumberOfPeriodsInThisDay ), 1 );
    check_int( get_int( ICalendar_get_FirstHourInThisPeriod ), 0 );
    check_int( get_int( ICalendar_get_LastHourInThisPeriod ), 23 );
    check_int( get_int( ICalendar_get_NumberOfHoursInThisPeriod ), 24 );
    check_int( get_int( ICalendar_get_FirstMinuteInThisHour ), 0 );
    check_int( get_int( ICalendar_get_LastMinuteInThisHour ), 59 );
    check_int( get_int( ICalendar_get_NumberOfMinutesInThisHour ), 60 );
    check_int( get_int( ICalendar_get_FirstSecondInThisMinute ), 0 );
    check_int( get_int( ICalendar_get_LastSecondInThisMinute ), 59 );
    check_int( get_int( ICalendar_get_NumberOfSecondsInThisMinute ), 60 );
    dow = 99;
    hr = ICalendar_get_DayOfWeek( calendar, &dow );
    ok( hr == S_OK && dow == DayOfWeek_Thursday, "got hr %#lx, day %d.\n", hr, dow );
    dst = 9;
    hr = ICalendar_get_IsDaylightSavingTime( calendar, &dst );
    ok( hr == S_OK && !dst, "got hr %#lx, dst %d.\n", hr, dst );
    check_str( ICalendar_get_ResolvedLanguage( calendar, &str ), L"en-US" );
    check_str( ICalendar_get_NumeralSystem( calendar, &str ), L"Latn" );
    check_str( ICalendar_GetCalendarSystem( calendar, &str ), L"GregorianCalendar" );
    check_str( ICalendar_GetClock( calendar, &str ), L"24HourClock" );
    hr = ICalendar_EraAsFullString( calendar, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    /* Wine's locale data has "AD" */
    todo_wine ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"A.D." ), "got %s.\n", debugstr_hstring( str ) );
    WindowsDeleteString( str );
    check_str( ICalendar_EraAsString( calendar, 1, &str ), L"AD" );
    check_str( ICalendar_YearAsString( calendar, &str ), L"2024" );
    check_str( ICalendar_YearAsTruncatedString( calendar, 2, &str ), L"24" );
    check_str( ICalendar_YearAsPaddedString( calendar, 6, &str ), L"002024" );
    check_str( ICalendar_MonthAsFullString( calendar, &str ), L"February" );
    check_str( ICalendar_MonthAsString( calendar, 3, &str ), L"Feb" );
    check_str( ICalendar_MonthAsFullSoloString( calendar, &str ), L"February" );
    check_str( ICalendar_MonthAsSoloString( calendar, 3, &str ), L"Feb" );
    check_str( ICalendar_MonthAsNumericString( calendar, &str ), L"2" );
    check_str( ICalendar_MonthAsPaddedNumericString( calendar, 2, &str ), L"02" );
    check_str( ICalendar_DayAsString( calendar, &str ), L"29" );
    check_str( ICalendar_DayAsPaddedString( calendar, 3, &str ), L"029" );
    check_str( ICalendar_DayOfWeekAsFullString( calendar, &str ), L"Thursday" );
    check_str( ICalendar_DayOfWeekAsString( calendar, 3, &str ), L"Thu" );
    check_str( ICalendar_DayOfWeekAsFullSoloString( calendar, &str ), L"Thursday" );
    check_str( ICalendar_DayOfWeekAsSoloString( calendar, 2, &str ), L"Th" );
    check_str( ICalendar_PeriodAsFullString( calendar, &str ), L"" );
    check_str( ICalendar_PeriodAsString( calendar, 1, &str ), L"" );
    check_str( ICalendar_HourAsString( calendar, &str ), L"13" );
    check_str( ICalendar_HourAsPaddedString( calendar, 3, &str ), L"013" );
    check_str( ICalendar_MinuteAsString( calendar, &str ), L"45" );
    check_str( ICalendar_MinuteAsPaddedString( calendar, 3, &str ), L"045" );
    check_str( ICalendar_SecondAsString( calendar, &str ), L"30" );
    check_str( ICalendar_SecondAsPaddedString( calendar, 3, &str ), L"030" );
    check_str( ICalendar_NanosecondAsString( calendar, &str ), L"123456700" );
    check_str( ICalendar_NanosecondAsPaddedString( calendar, 12, &str ), L"000123456700" );
    hr = ICalendar_QueryInterface( calendar, &IID_ITimeZoneOnCalendar, (void **)&tz );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_str( ITimeZoneOnCalendar_GetTimeZone( tz, &str ), L"Etc/UTC" );
    check_str( ITimeZoneOnCalendar_TimeZoneAsFullString( tz, &str ), L"Coordinated Universal Time" );
    check_str( ITimeZoneOnCalendar_TimeZoneAsString( tz, 3, &str ), L"UTC" );

    /* arithmetic: dates keep the time of day and clamp the day; times move the instant */
    ICalendar_AddYears( calendar, 1 );
    check_ymd( calendar, 2025, 2, 28, 13, 45, 30, 123456700 );
    ICalendar_SetDateTime( calendar, when );
    ICalendar_AddMonths( calendar, -1 );
    ICalendar_AddDays( calendar, 2 );
    check_ymd( calendar, 2024, 1, 31, 13, 45, 30, 123456700 );
    when.UniversalTime = datetime( 2024, 1, 31, 23, 30, 0, 0 );
    ICalendar_SetDateTime( calendar, when );
    ICalendar_AddMonths( calendar, 1 );
    check_ymd( calendar, 2024, 2, 29, 23, 30, 0, 0 );
    ICalendar_AddHours( calendar, 25 );
    check_ymd( calendar, 2024, 3, 2, 0, 30, 0, 0 );
    ICalendar_AddMinutes( calendar, -90 );
    ICalendar_AddSeconds( calendar, 3601 );
    ICalendar_AddNanoseconds( calendar, 1500000000 );
    check_ymd( calendar, 2024, 3, 2, 0, 0, 2, 500000000 );
    ICalendar_AddWeeks( calendar, 2 );
    hr = ICalendar_AddEras( calendar, 0 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_ymd( calendar, 2024, 3, 16, 0, 0, 2, 500000000 );
    ICalendar_AddPeriods( calendar, 1 );
    check_ymd( calendar, 2024, 3, 17, 0, 0, 2, 500000000 );

    /* setters */
    when.UniversalTime = datetime( 2024, 1, 31, 10, 0, 0, 0 );
    ICalendar_SetDateTime( calendar, when );
    hr = ICalendar_put_Month( calendar, 2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_ymd( calendar, 2024, 2, 29, 10, 0, 0, 0 );
    hr = ICalendar_put_Month( calendar, 13 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Day( calendar, 31 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Day( calendar, 0 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Hour( calendar, 24 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Minute( calendar, 60 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Nanosecond( calendar, 1000000000 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Year( calendar, 0 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    check_ymd( calendar, 2024, 2, 29, 10, 0, 0, 0 );
    hr = ICalendar_put_Year( calendar, 2023 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ICalendar_put_Day( calendar, 15 );
    ICalendar_put_Hour( calendar, 7 );
    ICalendar_put_Minute( calendar, 8 );
    ICalendar_put_Second( calendar, 9 );
    hr = ICalendar_put_Nanosecond( calendar, 100 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_ymd( calendar, 2023, 2, 15, 7, 8, 9, 100 );
    hr = ICalendar_GetDateTime( calendar, &when );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( when.UniversalTime == datetime( 2023, 2, 15, 7, 8, 9, 1 ), "got %I64d.\n", when.UniversalTime );
    hr = ICalendar_put_Era( calendar, 1 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Era( calendar, 2 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Period( calendar, 2 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    /* min, max */
    ICalendar_SetToMin( calendar );
    check_ymd( calendar, 1, 1, 1, 0, 0, 0, 0 );
    ICalendar_GetDateTime( calendar, &when );
    ok( when.UniversalTime == -504911232000000000, "got %I64d.\n", when.UniversalTime );
    ICalendar_SetToMax( calendar );
    ICalendar_GetDateTime( calendar, &when );
    ok( when.UniversalTime == 2650467743999999999, "got %I64d.\n", when.UniversalTime );
    ns = get_int( ICalendar_get_Nanosecond );
    todo_wine ok( ns == 999999999, "got %d.\n", ns );
    check_int( get_int( ICalendar_get_Year ), 9999 );
    check_int( get_int( ICalendar_get_Month ), 12 );
    check_int( get_int( ICalendar_get_Day ), 31 );
    check_int( get_int( ICalendar_get_Second ), 59 );

    /* compare, clone, copy */
    when.UniversalTime = datetime( 2024, 6, 1, 0, 0, 0, 0 );
    ICalendar_SetDateTime( calendar, when );
    hr = ICalendar_Clone( calendar, &other );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    res = 9;
    hr = ICalendar_Compare( calendar, other, &res );
    ok( hr == S_OK && res == 0, "got hr %#lx, %d.\n", hr, res );
    ICalendar_AddDays( other, 1 );
    hr = ICalendar_Compare( calendar, other, &res );
    ok( hr == S_OK && res == -1, "got hr %#lx, %d.\n", hr, res );
    when.UniversalTime -= 1;
    hr = ICalendar_CompareDateTime( calendar, when, &res );
    ok( hr == S_OK && res == 1, "got hr %#lx, %d.\n", hr, res );
    hr = ICalendar_CopyTo( calendar, other );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ICalendar_Compare( calendar, other, &res );
    ok( hr == S_OK && res == 0, "got hr %#lx, %d.\n", hr, res );
    ICalendar_Release( other );

    /* 12 hours */
    s4 = hstr( L"12HourClock" );
    hr = ICalendar_ChangeClock( calendar, s4 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( s4 );
    when.UniversalTime = datetime( 2024, 6, 1, 0, 30, 0, 0 );
    ICalendar_SetDateTime( calendar, when );
    check_ymd( calendar, 2024, 6, 1, 12, 30, 0, 0 );
    check_int( get_int( ICalendar_get_Period ), 1 );
    check_int( get_int( ICalendar_get_NumberOfPeriodsInThisDay ), 2 );
    check_int( get_int( ICalendar_get_FirstHourInThisPeriod ), 12 );
    check_int( get_int( ICalendar_get_LastHourInThisPeriod ), 11 );
    check_int( get_int( ICalendar_get_NumberOfHoursInThisPeriod ), 12 );
    check_str( ICalendar_PeriodAsFullString( calendar, &str ), L"AM" );
    check_str( ICalendar_HourAsString( calendar, &str ), L"12" );
    when.UniversalTime = datetime( 2024, 6, 1, 13, 5, 0, 0 );
    ICalendar_SetDateTime( calendar, when );
    check_ymd( calendar, 2024, 6, 1, 1, 5, 0, 0 );
    check_int( get_int( ICalendar_get_Period ), 2 );
    check_str( ICalendar_PeriodAsFullString( calendar, &str ), L"PM" );
    hr = ICalendar_put_Hour( calendar, 12 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ICalendar_GetDateTime( calendar, &when );
    ok( when.UniversalTime == datetime( 2024, 6, 1, 12, 5, 0, 0 ), "got %I64d.\n", when.UniversalTime );
    hr = ICalendar_put_Hour( calendar, 0 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = ICalendar_put_Period( calendar, 1 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ICalendar_GetDateTime( calendar, &when );
    ok( when.UniversalTime == datetime( 2024, 6, 1, 0, 5, 0, 0 ), "got %I64d.\n", when.UniversalTime );
    check_ymd( calendar, 2024, 6, 1, 12, 5, 0, 0 );
    hr = ICalendar_put_Period( calendar, 3 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    /* other calendar systems, clocks and time zones */
    s4 = hstr( L"JapaneseCalendar" );
    hr = ICalendar_ChangeCalendarSystem( calendar, s4 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( s4 );
    check_str( ICalendar_GetCalendarSystem( calendar, &str ), L"JapaneseCalendar" );
    res = get_int( ICalendar_get_Era );
    todo_wine ok( res == 5, "got era %d.\n", res );
    res = get_int( ICalendar_get_Year );
    todo_wine ok( res == 6, "got year %d.\n", res );
    s4 = hstr( L"NoSuchCalendar" );
    hr = ICalendar_ChangeCalendarSystem( calendar, s4 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsDeleteString( s4 );
    s4 = hstr( L"NoSuchClock" );
    hr = ICalendar_ChangeClock( calendar, s4 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsDeleteString( s4 );
    s4 = hstr( L"GregorianCalendar" );
    ICalendar_ChangeCalendarSystem( calendar, s4 );
    WindowsDeleteString( s4 );
    s4 = hstr( L"America/Los_Angeles" );
    hr = ITimeZoneOnCalendar_ChangeTimeZone( tz, s4 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( s4 );
    /* 2024-06-01 00:05 UTC is 17:05 the day before in Los Angeles (UTC-7 in summer) */
    check_ymd( calendar, 2024, 5, 31, 5, 5, 0, 0 );
    check_int( get_int( ICalendar_get_Period ), 2 );
    dst = 9;
    hr = ICalendar_get_IsDaylightSavingTime( calendar, &dst );
    ok( hr == S_OK && dst == 1, "got hr %#lx, dst %d.\n", hr, dst );
    check_str( ITimeZoneOnCalendar_GetTimeZone( tz, &str ), L"America/Los_Angeles" );
    check_str( ITimeZoneOnCalendar_TimeZoneAsFullString( tz, &str ), L"Pacific Daylight Time" );
    check_str( ITimeZoneOnCalendar_TimeZoneAsString( tz, 3, &str ), L"PDT" );
    /* local fields are set in the calendar's time zone */
    hr = ICalendar_put_Hour( calendar, 6 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ICalendar_GetDateTime( calendar, &when );
    ok( when.UniversalTime == datetime( 2024, 6, 1, 1, 5, 0, 0 ), "got %I64d.\n", when.UniversalTime );
    s4 = hstr( L"Pacific Standard Time" );
    hr = ITimeZoneOnCalendar_ChangeTimeZone( tz, s4 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsDeleteString( s4 );
    s4 = hstr( L"No/Such_Zone" );
    hr = ITimeZoneOnCalendar_ChangeTimeZone( tz, s4 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsDeleteString( s4 );
    ITimeZoneOnCalendar_Release( tz );
    ICalendar_Release( calendar );

    /* the factory without a time zone, and other languages */
    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory, (void **)&factory1 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hstring_iterable_init( &languages, en_us, 1 );
    hr = ICalendarFactory_CreateCalendar( factory1, &languages.IIterable_HSTRING_iface, s1, s2, &calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        check_str( ICalendar_GetClock( calendar, &str ), L"24HourClock" );
        ICalendar_Release( calendar );
    }
    hstring_iterable_init( &german, de_de, 1 );
    hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, &german.IIterable_HSTRING_iface, &calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        when.UniversalTime = datetime( 2024, 3, 5, 12, 0, 0, 0 );
        ICalendar_SetDateTime( calendar, when );
        check_str( ICalendar_get_ResolvedLanguage( calendar, &str ), L"de-DE" );
        check_str( ICalendar_GetClock( calendar, &str ), L"24HourClock" );
        check_str( ICalendar_MonthAsFullString( calendar, &str ), L"März" );
        check_str( ICalendar_DayOfWeekAsFullString( calendar, &str ), L"Dienstag" );
        hr = ICalendar_MonthAsString( calendar, 3, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        /* Wine's locale data has "Mär" */
        todo_wine ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Mrz" ), "got %s.\n", debugstr_hstring( str ) );
        WindowsDeleteString( str );
        hr = ICalendar_DayOfWeekAsString( calendar, 2, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        /* Wine's locale data has no form shorter than "Di." */
        todo_wine ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Di" ), "got %s.\n", debugstr_hstring( str ) );
        WindowsDeleteString( str );
        check_str( ICalendar_EraAsFullString( calendar, &str ), L"n. Chr." );
        ICalendar_Release( calendar );
    }
    ICalendarFactory_Release( factory1 );

done:
    WindowsDeleteString( s1 );
    WindowsDeleteString( s2 );
    WindowsDeleteString( s3 );
    ICalendarFactory2_Release( factory2 );
    IActivationFactory_Release( factory );
}

static void test_LanguageStatics(void)
{
    static const struct { const WCHAR *tag; BOOLEAN well_formed; } tags[] =
    {
        { L"en-US", TRUE }, { L"en", TRUE }, { L"zh-Hans-CN", TRUE }, { L"xx", TRUE }, { L"x-private", TRUE },
        { L"de-DE-1996", TRUE }, { L"", FALSE }, { L"not a tag", FALSE }, { L"en_US", FALSE }, { L"e", FALSE },
        { L"toolongtag", FALSE }, { L"en--US", FALSE }, { L"en-", FALSE }, { L"-en", FALSE },
    };
    static const WCHAR *class_name = L"Windows.Globalization.Language";
    IActivationFactory *factory;
    ILanguageStatics *statics;
    BOOLEAN result;
    HSTRING str;
    unsigned int i;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (hr != S_OK) return;
    hr = IActivationFactory_QueryInterface( factory, &IID_ILanguageStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IActivationFactory_Release( factory );
    if (hr != S_OK) return;

    for (i = 0; i < ARRAY_SIZE(tags); i++)
    {
        WindowsCreateString( tags[i].tag, wcslen( tags[i].tag ), &str );
        result = !tags[i].well_formed;
        hr = ILanguageStatics_IsWellFormed( statics, str, &result );
        ok( hr == S_OK, "%s: got hr %#lx.\n", wine_dbgstr_w( tags[i].tag ), hr );
        ok( result == tags[i].well_formed, "%s: got %d.\n", wine_dbgstr_w( tags[i].tag ), result );
        WindowsDeleteString( str );
    }

    /* the keyboard layout's language: a well-formed tag */
    str = NULL;
    hr = ILanguageStatics_get_CurrentInputMethodLanguageTag( statics, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    result = FALSE;
    ILanguageStatics_IsWellFormed( statics, str, &result );
    ok( result, "got %s.\n", wine_dbgstr_hstring( str ) );
    WindowsDeleteString( str );
    ILanguageStatics_Release( statics );
}

#define check_format( a, b, c ) check_format_( __LINE__, a, b, c )
static void check_format_( int line, INumberFormatter *formatter, double value, const WCHAR *expected )
{
    HSTRING s = NULL;
    HRESULT hr = INumberFormatter_FormatDouble( formatter, value, &s );
    ok_(__FILE__, line)( hr == S_OK, "%g: got hr %#lx.\n", value, hr );
    ok_(__FILE__, line)( !wcscmp( WindowsGetStringRawBuffer( s, NULL ), expected ), "%g: got %s, expected %s.\n", value,
                         wine_dbgstr_hstring( s ), wine_dbgstr_w( expected ) );
    WindowsDeleteString( s );
}

#define check_format_int( a, b, c ) check_format_int_( __LINE__, a, b, c )
static void check_format_int_( int line, INumberFormatter *formatter, INT64 value, const WCHAR *expected )
{
    HSTRING s = NULL;
    HRESULT hr = INumberFormatter_FormatInt( formatter, value, &s );
    ok_(__FILE__, line)( hr == S_OK, "%I64d: got hr %#lx.\n", value, hr );
    ok_(__FILE__, line)( !wcscmp( WindowsGetStringRawBuffer( s, NULL ), expected ), "%I64d: got %s, expected %s.\n", value,
                         wine_dbgstr_hstring( s ), wine_dbgstr_w( expected ) );
    WindowsDeleteString( s );
}

/* ParseDouble, ParseInt and ParseUInt of text; NAN where the result is null */
#define check_parse( a, b, c, d, e ) check_parse_( __LINE__, a, b, c, d, e )
static void check_parse_( int line, INumberFormatter *formatter, const WCHAR *text, double d, BOOL has_int, INT64 i )
{
    IReference_DOUBLE *rd = NULL;
    IReference_INT64 *ri = NULL;
    IReference_UINT64 *ru = NULL;
    INumberParser *parser;
    double dv;
    INT64 iv;
    HSTRING s;
    HRESULT hr;

    INumberFormatter_QueryInterface( formatter, &IID_INumberParser, (void **)&parser );
    WindowsCreateString( text, wcslen( text ), &s );
    hr = INumberParser_ParseDouble( parser, s, &rd );
    ok_(__FILE__, line)( hr == S_OK, "%s: got hr %#lx.\n", wine_dbgstr_w( text ), hr );
    ok_(__FILE__, line)( (rd == NULL) == (isnan( d ) != 0), "%s: got %p.\n", wine_dbgstr_w( text ), rd );
    if (rd)
    {
        IReference_DOUBLE_get_Value( rd, &dv );
        ok_(__FILE__, line)( dv == d, "%s: got %g.\n", wine_dbgstr_w( text ), dv );
        IReference_DOUBLE_Release( rd );
    }
    hr = INumberParser_ParseInt( parser, s, &ri );
    ok_(__FILE__, line)( hr == S_OK && !ri == !has_int, "%s: got hr %#lx, %p.\n", wine_dbgstr_w( text ), hr, ri );
    if (ri)
    {
        IReference_INT64_get_Value( ri, &iv );
        ok_(__FILE__, line)( iv == i, "%s: got %I64d.\n", wine_dbgstr_w( text ), iv );
        IReference_INT64_Release( ri );
    }
    hr = INumberParser_ParseUInt( parser, s, &ru );
    ok_(__FILE__, line)( hr == S_OK && !ru == !(has_int && i >= 0), "%s: got hr %#lx, %p.\n", wine_dbgstr_w( text ), hr, ru );
    if (ru) IReference_UINT64_Release( ru );
    WindowsDeleteString( s );
    INumberParser_Release( parser );
}

static INumberFormatter *create_decimal_formatter( IDecimalFormatterFactory *factory, const WCHAR *language,
                                                   const WCHAR *region )
{
    struct hstring_iterable languages;
    INumberFormatter *formatter = NULL;
    HSTRING s = NULL;
    HRESULT hr;

    hstring_iterable_init( &languages, &language, 1 );
    if (region) WindowsCreateString( region, wcslen( region ), &s );
    hr = IDecimalFormatterFactory_CreateDecimalFormatter( factory, &languages.IIterable_HSTRING_iface, s, &formatter );
    WindowsDeleteString( s );
    ok( hr == (region ? S_OK : E_INVALIDARG), "%s %s: got hr %#lx.\n", wine_dbgstr_w( language ), wine_dbgstr_w( region ), hr );
    return formatter;
}

/* values as Windows 11 gives them (x64 and x86) */
static void test_DecimalFormatter(void)
{
    static const WCHAR *class_name = L"Windows.Globalization.NumberFormatting.DecimalFormatter";
    IDecimalFormatterFactory *decimal_factory;
    ISignificantDigitsOption *significant;
    INumberRounderOption *rounder_option;
    INumberFormatterOptions *options;
    ISignedZeroOption *signed_zero;
    IActivationFactory *factory;
    INumberFormatter *formatter;
    INumberRounder *rounder;
    INT32 value;
    boolean flag;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (hr != S_OK) return;
    check_interface( factory, &IID_IAgileObject );
    hr = IActivationFactory_QueryInterface( factory, &IID_IDecimalFormatterFactory, (void **)&decimal_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    ok( !create_decimal_formatter( decimal_factory, L"de-DE", NULL ), "got a formatter without a region.\n" );

    /* en-US: the defaults */
    formatter = create_decimal_formatter( decimal_factory, L"en-US", L"US" );
    INumberFormatter_QueryInterface( formatter, &IID_INumberFormatterOptions, (void **)&options );
    value = -1;
    INumberFormatterOptions_get_IntegerDigits( options, &value );
    ok( value == 1, "got integer digits %d.\n", value );
    INumberFormatterOptions_get_FractionDigits( options, &value );
    ok( value == 2, "got fraction digits %d.\n", value );
    flag = TRUE;
    INumberFormatterOptions_get_IsGrouped( options, &flag );
    ok( !flag, "got grouped %d.\n", flag );
    flag = TRUE;
    INumberFormatterOptions_get_IsDecimalPointAlwaysDisplayed( options, &flag );
    ok( !flag, "got point %d.\n", flag );
    INumberFormatterOptions_get_NumeralSystem( options, &str );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Latn" ), "got %s.\n", wine_dbgstr_hstring( str ) );
    WindowsDeleteString( str );
    INumberFormatterOptions_get_ResolvedLanguage( options, &str );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"en-US" ), "got %s.\n", wine_dbgstr_hstring( str ) );
    WindowsDeleteString( str );
    INumberFormatterOptions_get_GeographicRegion( options, &str );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"US" ), "got %s.\n", wine_dbgstr_hstring( str ) );
    WindowsDeleteString( str );
    INumberFormatterOptions_get_ResolvedGeographicRegion( options, &str );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"ZZ" ), "got %s.\n", wine_dbgstr_hstring( str ) );
    WindowsDeleteString( str );
    INumberFormatter_QueryInterface( formatter, &IID_ISignificantDigitsOption, (void **)&significant );
    value = -1;
    ISignificantDigitsOption_get_SignificantDigits( significant, &value );
    ok( !value, "got significant digits %d.\n", value );
    ISignificantDigitsOption_Release( significant );
    INumberFormatter_QueryInterface( formatter, &IID_ISignedZeroOption, (void **)&signed_zero );
    flag = TRUE;
    ISignedZeroOption_get_IsZeroSigned( signed_zero, &flag );
    ok( !flag, "got signed zero %d.\n", flag );
    ISignedZeroOption_Release( signed_zero );
    INumberFormatter_QueryInterface( formatter, &IID_INumberRounderOption, (void **)&rounder_option );
    rounder = (void *)0xdeadbeef;
    INumberRounderOption_get_NumberRounder( rounder_option, &rounder );
    ok( !rounder, "got rounder %p.\n", rounder );
    INumberRounderOption_Release( rounder_option );

    check_format_int( formatter, 0, L"0.00" );
    check_format_int( formatter, -1234567, L"-1234567.00" );
    check_format( formatter, 1234.5678, L"1234.5678" );
    check_format( formatter, -0.0, L"0.00" );
    check_format( formatter, 0.5, L"0.50" );
    check_format( formatter, 1e20, L"100000000000000000000.00" );
    check_format( formatter, 0.000123, L"0.000123" );
    check_format( formatter, -2.5, L"-2.50" );

    check_parse( formatter, L"1234.5", 1234.5, FALSE, 0 );
    check_parse( formatter, L"1,234.5", 1234.5, FALSE, 0 );
    check_parse( formatter, L"1.234,5", NAN, FALSE, 0 );
    check_parse( formatter, L"-12", -12, TRUE, -12 );
    check_parse( formatter, L"12", 12, TRUE, 12 );
    check_parse( formatter, L"abc", NAN, FALSE, 0 );
    check_parse( formatter, L" 12", NAN, FALSE, 0 );

    INumberFormatterOptions_put_FractionDigits( options, 0 );
    check_format_int( formatter, 7, L"7" );
    check_format( formatter, 0.5, L"0.5" );
    check_format( formatter, -0.0, L"0" );

    INumberFormatterOptions_put_IsGrouped( options, TRUE );
    INumberFormatterOptions_put_IntegerDigits( options, 3 );
    INumberFormatterOptions_put_FractionDigits( options, 2 );
    INumberFormatterOptions_put_IsDecimalPointAlwaysDisplayed( options, TRUE );
    check_format_int( formatter, 7, L"007.00" );
    check_format_int( formatter, -1234567, L"-1,234,567.00" );
    check_format( formatter, -2.5, L"-002.50" );
    check_format( formatter, 1e20, L"100,000,000,000,000,000,000.00" );

    hr = INumberFormatterOptions_put_FractionDigits( options, -1 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsCreateString( L"bogus", 5, &str );
    hr = INumberFormatterOptions_put_NumeralSystem( options, str );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    /* the Arab numeral system brings the Arabic separators */
    INumberFormatterOptions_put_IntegerDigits( options, 1 );
    WindowsCreateString( L"Arab", 4, &str );
    hr = INumberFormatterOptions_put_NumeralSystem( options, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    check_format_int( formatter, 1234567, L"\x0661\x066c\x0662\x0663\x0664\x066c\x0665\x0666\x0667\x066b\x0660\x0660" );
    INumberFormatterOptions_Release( options );
    INumberFormatter_Release( formatter );

    /* de-DE: its separators */
    formatter = create_decimal_formatter( decimal_factory, L"de-DE", L"DE" );
    check_format( formatter, 1234.5678, L"1234,5678" );
    check_format( formatter, -2.5, L"-2,50" );
    check_parse( formatter, L"1.234,5", 1234.5, FALSE, 0 );
    check_parse( formatter, L"1,234.5", NAN, FALSE, 0 );
    INumberFormatter_Release( formatter );

    /* ar-SA: Arabic-Indic digits */
    formatter = create_decimal_formatter( decimal_factory, L"ar-SA", L"SA" );
    INumberFormatter_QueryInterface( formatter, &IID_INumberFormatterOptions, (void **)&options );
    INumberFormatterOptions_get_NumeralSystem( options, &str );
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Arab" ), "got %s.\n", wine_dbgstr_hstring( str ) );
    WindowsDeleteString( str );
    INumberFormatterOptions_Release( options );
    check_format_int( formatter, 7, L"\x0667\x066b\x0660\x0660" );
    check_format( formatter, -2.5, L"-\x0662\x066b\x0665\x0660" );
    INumberFormatter_Release( formatter );

    IDecimalFormatterFactory_Release( decimal_factory );
    IActivationFactory_Release( factory );
}

static const char *font_names[] =
{
    "UIText", "UIHeading", "UITitle", "UICaption", "UINotificationHeading", "TraditionalDocument", "ModernDocument",
    "DocumentHeading", "FixedWidthText", "DocumentAlternate1", "DocumentAlternate2",
};

static HRESULT get_language_font( ILanguageFontGroup *group, unsigned int i, ILanguageFont **font )
{
    switch (i)
    {
    case 0: return ILanguageFontGroup_get_UITextFont( group, font );
    case 1: return ILanguageFontGroup_get_UIHeadingFont( group, font );
    case 2: return ILanguageFontGroup_get_UITitleFont( group, font );
    case 3: return ILanguageFontGroup_get_UICaptionFont( group, font );
    case 4: return ILanguageFontGroup_get_UINotificationHeadingFont( group, font );
    case 5: return ILanguageFontGroup_get_TraditionalDocumentFont( group, font );
    case 6: return ILanguageFontGroup_get_ModernDocumentFont( group, font );
    case 7: return ILanguageFontGroup_get_DocumentHeadingFont( group, font );
    case 8: return ILanguageFontGroup_get_FixedWidthTextFont( group, font );
    case 9: return ILanguageFontGroup_get_DocumentAlternate1Font( group, font );
    default: return ILanguageFontGroup_get_DocumentAlternate2Font( group, font );
    }
}

static void test_LanguageFontGroup(void)
{
    static const WCHAR *class_name = RuntimeClass_Windows_Globalization_Fonts_LanguageFontGroup;
    static const WCHAR *tags[] =
    {
        L"en-US", L"en", L"de-DE", L"ru-RU", L"el-GR", L"ja-JP", L"ko-KR", L"zh-Hans", L"zh-CN", L"zh-Hant", L"zh-TW",
        L"ar-SA", L"he-IL", L"th-TH", L"hi-IN", L"ka-GE", L"am-ET", L"xx", L"", L"not a tag",
    };
    ILanguageFontGroupFactory *factory;
    IActivationFactory *activation;
    ILanguageFontGroup *group;
    struct FontWeight weight;
    enum FontStretch stretch;
    enum FontStyle style;
    ILanguageFont *font;
    unsigned int t, i;
    HSTRING str, family;
    double scale;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&activation );
    WindowsDeleteString( str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (hr != S_OK) return;
    hr = IActivationFactory_QueryInterface( activation, &IID_ILanguageFontGroupFactory, (void **)&factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    group = (void *)0xdeadbeef;
    hr = IActivationFactory_ActivateInstance( activation, (IInspectable **)&group );
    ok( hr == E_NOTIMPL, "got hr %#lx.\n", hr );

    /* the fonts differ by script, Windows version and bitness (64-bit Windows 11 gives newer choices than its 32-bit
     * side); the UI fonts of Latin-script languages are the same, and every language has its five UI fonts */
    for (t = 0; t < ARRAY_SIZE(tags); t++)
    {
        BOOL known = t < ARRAY_SIZE(tags) - 3, latin = t < 5;

        winetest_push_context( "%s", wine_dbgstr_w( tags[t] ) );
        WindowsCreateString( tags[t], wcslen( tags[t] ), &str );
        group = NULL;
        hr = ILanguageFontGroupFactory_CreateLanguageFontGroup( factory, str, &group );
        WindowsDeleteString( str );
        ok( hr == (known ? S_OK : HRESULT_FROM_WIN32( ERROR_NOT_FOUND )), "got hr %#lx.\n", hr );
        if (hr != S_OK) { winetest_pop_context(); continue; }
        check_interface( group, &IID_IAgileObject );
        for (i = 0; i < ARRAY_SIZE(font_names); i++)
        {
            font = NULL;
            hr = get_language_font( group, i, &font );
            ok( hr == S_OK, "%s: got hr %#lx.\n", font_names[i], hr );
            ok( font != NULL || i > 4, "%s: no font.\n", font_names[i] );
            if (!font) continue;
            family = NULL;
            hr = ILanguageFont_get_FontFamily( font, &family );
            ok( hr == S_OK && WindowsGetStringLen( family ), "%s: got hr %#lx, family %s.\n", font_names[i], hr,
                wine_dbgstr_hstring( family ) );
            weight.Weight = 0;
            hr = ILanguageFont_get_FontWeight( font, &weight );
            ok( hr == S_OK && weight.Weight >= 100 && weight.Weight <= 900, "%s: got hr %#lx, weight %u.\n",
                font_names[i], hr, weight.Weight );
            hr = ILanguageFont_get_FontStretch( font, &stretch );
            ok( hr == S_OK && stretch == FontStretch_Normal, "%s: got hr %#lx, stretch %d.\n", font_names[i], hr,
                stretch );
            hr = ILanguageFont_get_FontStyle( font, &style );
            ok( hr == S_OK && style == FontStyle_Normal, "%s: got hr %#lx, style %d.\n", font_names[i], hr, style );
            scale = 0;
            hr = ILanguageFont_get_ScaleFactor( font, &scale );
            ok( hr == S_OK && scale >= 50 && scale <= 200, "%s: got hr %#lx, scale %g.\n", font_names[i], hr, scale );
            if (latin && i <= 4)
            {
                static const UINT16 weights[] = {350, 300, 300, 400, 600};
                ok( !wcscmp( WindowsGetStringRawBuffer( family, NULL ), L"Segoe UI" ), "%s: got family %s.\n",
                    font_names[i], wine_dbgstr_hstring( family ) );
                ok( weight.Weight == weights[i], "%s: got weight %u.\n", font_names[i], weight.Weight );
                ok( scale == 100, "%s: got scale %g.\n", font_names[i], scale );
            }
            if (winetest_debug > 1)
                trace( "%s: %s, weight %u, scale %g\n", font_names[i], wine_dbgstr_hstring( family ), weight.Weight,
                       scale );
            WindowsDeleteString( family );
            check_interface( font, &IID_IAgileObject );
            ILanguageFont_Release( font );
        }
        ILanguageFontGroup_Release( group );
        winetest_pop_context();
    }

    ILanguageFontGroupFactory_Release( factory );
    IActivationFactory_Release( activation );
}

#define check_identifier( hr, str, expect ) check_identifier_( __LINE__, hr, str, expect )
static void check_identifier_( unsigned int line, HRESULT hr, HSTRING str, const WCHAR *expect )
{
    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    if (hr != S_OK) return;
    ok_(__FILE__, line)( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), expect ), "got %s, expected %s.\n",
                         debugstr_hstring( str ), debugstr_w( expect ) );
    WindowsDeleteString( str );
}

static IActivationFactory *identifier_factory( const WCHAR *class_name )
{
    IActivationFactory *factory;
    IInspectable *instance;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString( class_name, wcslen( class_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (hr != S_OK) return NULL;
    instance = (void *)0xdeadbeef;
    hr = IActivationFactory_ActivateInstance( factory, &instance );
    ok( hr == E_NOTIMPL, "got hr %#lx.\n", hr );
    return factory;
}

static void test_CalendarIdentifiers(void)
{
    ICalendarIdentifiersStatics3 *statics3;
    ICalendarIdentifiersStatics2 *statics2;
    ICalendarIdentifiersStatics *statics;
    IClockIdentifiersStatics *clock;
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;

    if (!(factory = identifier_factory( RuntimeClass_Windows_Globalization_CalendarIdentifiers ))) return;
    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarIdentifiersStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ICalendarIdentifiersStatics_get_Gregorian( statics, &str );
    check_identifier( hr, str, L"GregorianCalendar" );
    hr = ICalendarIdentifiersStatics_get_Hebrew( statics, &str );
    check_identifier( hr, str, L"HebrewCalendar" );
    hr = ICalendarIdentifiersStatics_get_Hijri( statics, &str );
    check_identifier( hr, str, L"HijriCalendar" );
    hr = ICalendarIdentifiersStatics_get_Japanese( statics, &str );
    check_identifier( hr, str, L"JapaneseCalendar" );
    hr = ICalendarIdentifiersStatics_get_Julian( statics, &str );
    check_identifier( hr, str, L"JulianCalendar" );
    hr = ICalendarIdentifiersStatics_get_Korean( statics, &str );
    check_identifier( hr, str, L"KoreanCalendar" );
    hr = ICalendarIdentifiersStatics_get_Taiwan( statics, &str );
    check_identifier( hr, str, L"TaiwanCalendar" );
    hr = ICalendarIdentifiersStatics_get_Thai( statics, &str );
    check_identifier( hr, str, L"ThaiCalendar" );
    hr = ICalendarIdentifiersStatics_get_UmAlQura( statics, &str );
    check_identifier( hr, str, L"UmAlQuraCalendar" );
    ICalendarIdentifiersStatics_Release( statics );

    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarIdentifiersStatics2, (void **)&statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ICalendarIdentifiersStatics2_get_Persian( statics2, &str );
    check_identifier( hr, str, L"PersianCalendar" );
    ICalendarIdentifiersStatics2_Release( statics2 );

    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarIdentifiersStatics3, (void **)&statics3 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ICalendarIdentifiersStatics3_get_ChineseLunar( statics3, &str );
    check_identifier( hr, str, L"ChineseLunarCalendar" );
    hr = ICalendarIdentifiersStatics3_get_JapaneseLunar( statics3, &str );
    check_identifier( hr, str, L"JapaneseLunarCalendar" );
    hr = ICalendarIdentifiersStatics3_get_KoreanLunar( statics3, &str );
    check_identifier( hr, str, L"KoreanLunarCalendar" );
    hr = ICalendarIdentifiersStatics3_get_TaiwanLunar( statics3, &str );
    check_identifier( hr, str, L"TaiwanLunarCalendar" );
    hr = ICalendarIdentifiersStatics3_get_VietnameseLunar( statics3, &str );
    check_identifier( hr, str, L"VietnameseLunarCalendar" );
    ICalendarIdentifiersStatics3_Release( statics3 );
    IActivationFactory_Release( factory );

    if (!(factory = identifier_factory( RuntimeClass_Windows_Globalization_ClockIdentifiers ))) return;
    hr = IActivationFactory_QueryInterface( factory, &IID_IClockIdentifiersStatics, (void **)&clock );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IClockIdentifiersStatics_get_TwelveHour( clock, &str );
    check_identifier( hr, str, L"12HourClock" );
    hr = IClockIdentifiersStatics_get_TwentyFourHour( clock, &str );
    check_identifier( hr, str, L"24HourClock" );
    IClockIdentifiersStatics_Release( clock );
    IActivationFactory_Release( factory );
}

static void check_inspectable( IUnknown *unk, ULONG count, const WCHAR *class_name )
{
    IInspectable *inspectable;
    HSTRING name;
    IID *iids;
    ULONG n;
    HRESULT hr;

    hr = IUnknown_QueryInterface( unk, &IID_IInspectable, (void **)&inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    n = 0;
    hr = IInspectable_GetIids( inspectable, &n, &iids );
    ok( hr == S_OK, "%s: GetIids got hr %#lx.\n", wine_dbgstr_w( class_name ), hr );
    if (SUCCEEDED(hr))
    {
        ok( n == count, "%s: got %lu iids.\n", wine_dbgstr_w( class_name ), n );
        CoTaskMemFree( iids );
    }
    hr = IInspectable_GetRuntimeClassName( inspectable, &name );
    ok( hr == S_OK, "%s: GetRuntimeClassName got hr %#lx.\n", wine_dbgstr_w( class_name ), hr );
    if (SUCCEEDED(hr))
    {
        ok( !wcscmp( WindowsGetStringRawBuffer( name, NULL ), class_name ), "got %s.\n", debugstr_hstring( name ) );
        WindowsDeleteString( name );
    }
    IInspectable_Release( inspectable );
}

static IDateTimeFormatter *create_date_time_formatter( IDateTimeFormatterFactory *factory, const WCHAR *template,
                                                       const WCHAR *clock )
{
    static const WCHAR *en_us[] = { L"en-US" };
    struct hstring_iterable languages;
    IDateTimeFormatter *formatter = NULL;
    HSTRING t = hstr( template ), region = hstr( L"US" ), calendar = hstr( L"GregorianCalendar" ), c = hstr( clock );
    HRESULT hr;

    hstring_iterable_init( &languages, en_us, 1 );
    hr = IDateTimeFormatterFactory_CreateDateTimeFormatterContext( factory, t, &languages.IIterable_HSTRING_iface, region,
                                                                   calendar, c, &formatter );
    ok( hr == S_OK, "%s: got hr %#lx.\n", wine_dbgstr_w( template ), hr );
    WindowsDeleteString( t );
    WindowsDeleteString( region );
    WindowsDeleteString( calendar );
    WindowsDeleteString( c );
    return formatter;
}

#define check_datetime( formatter, template, expect ) check_datetime_( __LINE__, formatter, template, expect )
static void check_datetime_( int line, IDateTimeFormatterFactory *factory, const WCHAR *template, const WCHAR *expect )
{
    IDateTimeFormatter2 *formatter2;
    IDateTimeFormatter *formatter;
    DateTime when;
    HSTRING zone, str;
    HRESULT hr;

    formatter = create_date_time_formatter( factory, template, L"12HourClock" );
    if (!formatter) return;
    hr = IDateTimeFormatter_QueryInterface( formatter, &IID_IDateTimeFormatter2, (void **)&formatter2 );
    ok_( __FILE__, line )( hr == S_OK, "got hr %#lx.\n", hr );
    when.UniversalTime = datetime( 2026, 11, 17, 15, 45, 30, 0 );
    zone = hstr( L"UTC" );
    hr = IDateTimeFormatter2_FormatUsingTimeZone( formatter2, when, zone, &str );
    ok_( __FILE__, line )( hr == S_OK, "%s: got hr %#lx.\n", wine_dbgstr_w( template ), hr );
    if (SUCCEEDED(hr))
    {
        ok_( __FILE__, line )( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), expect ), "%s: got %s, expected %s.\n",
                               wine_dbgstr_w( template ), debugstr_hstring( str ), debugstr_w( expect ) );
        WindowsDeleteString( str );
    }
    WindowsDeleteString( zone );
    IDateTimeFormatter2_Release( formatter2 );
    IDateTimeFormatter_Release( formatter );
}

static void test_DateTimeFormatter(void)
{
    static const WCHAR *class_name = L"Windows.Globalization.DateTimeFormatting.DateTimeFormatter";
    IDateTimeFormatterStatics *statics;
    IDateTimeFormatterFactory *creator;
    IDateTimeFormatter *formatter;
    IActivationFactory *factory;
    IInspectable *instance;
    DateTime when;
    HSTRING str;
    HRESULT hr;
    YearFormat year;

    str = hstr( class_name );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken(hr == REGDB_E_CLASSNOTREG), "got hr %#lx.\n", hr );
    if (hr != S_OK) return;
    check_interface( factory, &IID_IAgileObject );
    check_inspectable( (IUnknown *)factory, 2, class_name );
    hr = IActivationFactory_QueryInterface( factory, &IID_IDateTimeFormatterFactory, (void **)&creator );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IActivationFactory_QueryInterface( factory, &IID_IDateTimeFormatterStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    /* en-US, 12 hours, UTC */
    check_datetime( creator, L"shortdate", L"11/17/2026" );
    check_datetime( creator, L"longdate", L"Tuesday, November 17, 2026" );
    check_datetime( creator, L"shorttime", L"3:45 PM" );
    check_datetime( creator, L"longtime", L"3:45:30 PM" );
    check_datetime( creator, L"year", L"2026" );
    check_datetime( creator, L"year.abbreviated", L"26" );
    check_datetime( creator, L"month.full", L"November" );
    check_datetime( creator, L"month.abbreviated day", L"Nov 17" );
    check_datetime( creator, L"month.numeric day", L"11/17" );
    check_datetime( creator, L"day", L"17" );
    check_datetime( creator, L"dayofweek.full", L"Tuesday" );
    check_datetime( creator, L"dayofweek.abbreviated", L"Tue" );
    check_datetime( creator, L"day dayofweek.full month year", L"Tuesday, November 17, 2026" );
    check_datetime( creator, L"shortdate shorttime", L"11/17/2026 3:45 PM" );
    check_datetime( creator, L"hour", L"3 PM" );
    check_datetime( creator, L"minute second", L"45:30" );
    check_datetime( creator, L"nonsense", L"11/17/2026" );

    formatter = create_date_time_formatter( creator, L"shorttime", L"24HourClock" );
    if (formatter)
    {
        IDateTimeFormatter2 *formatter2;
        HSTRING zone = hstr( L"UTC" );

        IDateTimeFormatter_QueryInterface( formatter, &IID_IDateTimeFormatter2, (void **)&formatter2 );
        when.UniversalTime = datetime( 2026, 11, 17, 15, 45, 30, 0 );
        check_str( IDateTimeFormatter2_FormatUsingTimeZone( formatter2, when, zone, &str ), L"15:45" );
        check_str( IDateTimeFormatter_get_Clock( formatter, &str ), L"24HourClock" );
        check_str( IDateTimeFormatter_get_Calendar( formatter, &str ), L"GregorianCalendar" );
        check_str( IDateTimeFormatter_get_Template( formatter, &str ), L"shorttime" );
        check_str( IDateTimeFormatter_get_ResolvedLanguage( formatter, &str ), L"en-US" );
        check_str( IDateTimeFormatter_get_GeographicRegion( formatter, &str ), L"US" );
        check_inspectable( (IUnknown *)formatter, 2, class_name );
        WindowsDeleteString( zone );
        IDateTimeFormatter2_Release( formatter2 );
        IDateTimeFormatter_Release( formatter );
    }

    /* local time, user locale */
    hr = IDateTimeFormatterFactory_CreateDateTimeFormatter( creator, hstr( L"year month day" ), (IDateTimeFormatter **)&formatter );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        MonthFormat month;

        when.UniversalTime = datetime( 2026, 6, 15, 12, 0, 0, 0 );
        hr = IDateTimeFormatter_Format( formatter, when, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        if (SUCCEEDED(hr))
        {
            ok( !!wcsstr( WindowsGetStringRawBuffer( str, NULL ), L"2026" ), "got %s.\n", debugstr_hstring( str ) );
            WindowsDeleteString( str );
        }
        hr = IDateTimeFormatter_get_IncludeYear( formatter, &year );
        ok( hr == S_OK && year == YearFormat_Default, "got hr %#lx, %d.\n", hr, year );
        hr = IDateTimeFormatter_get_IncludeMonth( formatter, &month );
        ok( hr == S_OK && month == MonthFormat_Default, "got hr %#lx, %d.\n", hr, month );
        IDateTimeFormatter_Release( formatter );
    }

    hr = IDateTimeFormatterFactory_CreateDateTimeFormatterDate( creator, YearFormat_Full, MonthFormat_Full,
                                                                DayFormat_Default, DayOfWeekFormat_None,
                                                                (IDateTimeFormatter **)&formatter );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        check_str( IDateTimeFormatter_get_Template( formatter, &str ), L"year.full month.full day" );
        IDateTimeFormatter_Release( formatter );
    }

    hr = IDateTimeFormatterStatics_get_LongDate( statics, (IDateTimeFormatter **)&formatter );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        check_str( IDateTimeFormatter_get_Template( formatter, &str ), L"longdate" );
        IDateTimeFormatter_Release( formatter );
    }
    hr = IDateTimeFormatterStatics_get_ShortTime( statics, (IDateTimeFormatter **)&formatter );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        check_str( IDateTimeFormatter_get_Template( formatter, &str ), L"shorttime" );
        IDateTimeFormatter_Release( formatter );
    }

    hr = IActivationFactory_ActivateInstance( factory, &instance );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) IInspectable_Release( instance );

    IDateTimeFormatterStatics_Release( statics );
    IDateTimeFormatterFactory_Release( creator );
    IActivationFactory_Release( factory );
}

static void check_currency( INumberFormatter *formatter, double value, const WCHAR *expect )
{
    HSTRING str;
    HRESULT hr;

    hr = INumberFormatter_FormatDouble( formatter, value, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), expect ), "%g: got %s, expected %s.\n", value,
        debugstr_hstring( str ), debugstr_w( expect ) );
    WindowsDeleteString( str );
}

static ICurrencyFormatter *create_currency_formatter( ICurrencyFormatterFactory *creator, const WCHAR *code )
{
    static const WCHAR *en_us[] = { L"en-US" };
    struct hstring_iterable languages;
    ICurrencyFormatter *formatter = NULL;
    HSTRING c = hstr( code ), region = hstr( L"US" );
    HRESULT hr;

    hstring_iterable_init( &languages, en_us, 1 );
    hr = ICurrencyFormatterFactory_CreateCurrencyFormatterCodeContext( creator, c, &languages.IIterable_HSTRING_iface, region,
                                                                       (ICurrencyFormatter **)&formatter );
    ok( hr == S_OK || (wcslen( code ) != 3 && hr == E_INVALIDARG), "%s: got hr %#lx.\n", wine_dbgstr_w( code ), hr );
    WindowsDeleteString( c );
    WindowsDeleteString( region );
    return formatter;
}

static void test_CurrencyFormatter(void)
{
    static const WCHAR *class_name = L"Windows.Globalization.NumberFormatting.CurrencyFormatter";
    ICurrencyFormatterFactory *creator;
    ICurrencyFormatter2 *currency2;
    IActivationFactory *factory;
    ICurrencyFormatter *currency;
    INumberFormatterOptions *options;
    INumberFormatter *formatter;
    INumberParser *parser;
    IReference_DOUBLE *number;
    CurrencyFormatterMode mode;
    IInspectable *instance;
    boolean flag;
    INT32 digits;
    double dbl;
    HSTRING str;
    HRESULT hr;

    str = hstr( class_name );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken(hr == REGDB_E_CLASSNOTREG), "got hr %#lx.\n", hr );
    if (hr != S_OK) return;
    check_interface( factory, &IID_IAgileObject );
    check_inspectable( (IUnknown *)factory, 1, class_name );
    hr = IActivationFactory_QueryInterface( factory, &IID_ICurrencyFormatterFactory, (void **)&creator );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IActivationFactory_ActivateInstance( factory, &instance );
    ok( hr == E_NOTIMPL, "got hr %#lx.\n", hr );

    ok( !create_currency_formatter( creator, L"US" ), "got a formatter for a bad code.\n" );

    currency = create_currency_formatter( creator, L"USD" );
    if (currency)
    {
        check_interface( currency, &IID_INumberFormatter );
        check_interface( currency, &IID_INumberFormatter2 );
        check_interface( currency, &IID_INumberFormatterOptions );
        check_interface( currency, &IID_INumberParser );
        check_interface( currency, &IID_ICurrencyFormatter2 );
        check_interface( currency, &IID_ISignificantDigitsOption );
        check_inspectable( (IUnknown *)currency, 9, class_name );
        ICurrencyFormatter_QueryInterface( currency, &IID_INumberFormatter, (void **)&formatter );
        ICurrencyFormatter_QueryInterface( currency, &IID_INumberFormatterOptions, (void **)&options );
        ICurrencyFormatter_QueryInterface( currency, &IID_ICurrencyFormatter2, (void **)&currency2 );
        ICurrencyFormatter_QueryInterface( currency, &IID_INumberParser, (void **)&parser );

        check_str( ICurrencyFormatter_get_Currency( currency, &str ), L"USD" );
        INumberFormatterOptions_get_FractionDigits( options, &digits );
        ok( digits == 2, "got %d.\n", digits );
        INumberFormatterOptions_get_IsGrouped( options, &flag );
        ok( flag, "not grouped.\n" );
        check_currency( formatter, 1234.5, L"$1,234.50" );
        check_currency( formatter, -1.5, L"-$1.50" );
        check_currency( formatter, 0, L"$0.00" );

        str = hstr( L"$1,234.50" );
        hr = INumberParser_ParseDouble( parser, str, &number );
        ok( hr == S_OK && number, "got hr %#lx.\n", hr );
        if (number)
        {
            IReference_DOUBLE_get_Value( number, &dbl );
            ok( dbl == 1234.5, "got %g.\n", dbl );
            IReference_DOUBLE_Release( number );
        }
        WindowsDeleteString( str );

        hr = ICurrencyFormatter2_get_Mode( currency2, &mode );
        ok( hr == S_OK && mode == CurrencyFormatterMode_UseSymbol, "got hr %#lx, %d.\n", hr, mode );
        hr = ICurrencyFormatter2_put_Mode( currency2, CurrencyFormatterMode_UseCurrencyCode );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        check_currency( formatter, 1234.5, L"USD\xa0" L"1,234.50" );
        ICurrencyFormatter2_put_Mode( currency2, CurrencyFormatterMode_UseSymbol );

        str = hstr( L"eur" );
        hr = ICurrencyFormatter_put_Currency( currency, str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        WindowsDeleteString( str );
        check_str( ICurrencyFormatter_get_Currency( currency, &str ), L"EUR" );
        check_currency( formatter, 1234.5, L"\x20ac" L"1,234.50" );
        str = hstr( L"EU" );
        hr = ICurrencyFormatter_put_Currency( currency, str );
        ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
        WindowsDeleteString( str );

        INumberParser_Release( parser );
        ICurrencyFormatter2_Release( currency2 );
        INumberFormatterOptions_Release( options );
        INumberFormatter_Release( formatter );
        ICurrencyFormatter_Release( currency );
    }

    currency = create_currency_formatter( creator, L"JPY" );
    if (currency)
    {
        ICurrencyFormatter_QueryInterface( currency, &IID_INumberFormatter, (void **)&formatter );
        ICurrencyFormatter_QueryInterface( currency, &IID_ICurrencyFormatter2, (void **)&currency2 );
        check_currency( formatter, 1234, L"\xa5" L"1,234" );
        hr = ICurrencyFormatter2_ApplyRoundingForCurrency( currency2, RoundingAlgorithm_RoundHalfAwayFromZero );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        check_currency( formatter, 2.5, L"\xa5" L"3" );
        ICurrencyFormatter2_ApplyRoundingForCurrency( currency2, RoundingAlgorithm_RoundHalfToEven );
        check_currency( formatter, 2.5, L"\xa5" L"2" );
        ICurrencyFormatter2_Release( currency2 );
        INumberFormatter_Release( formatter );
        ICurrencyFormatter_Release( currency );
    }

    ICurrencyFormatterFactory_Release( creator );
    IActivationFactory_Release( factory );
}

START_TEST(globalization)
{
    HMODULE kernel32;
    HRESULT hr;

    kernel32 = GetModuleHandleA("kernel32");
    pGetUserDefaultGeoName = (void*)GetProcAddress(kernel32, "GetUserDefaultGeoName");

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_GlobalizationPreferences();
    test_Language();
    test_GeographicRegion();
    test_ApplicationLanguages();
    test_Calendar();
    test_LanguageFontGroup();
    test_LanguageStatics();
    test_DecimalFormatter();
    test_CalendarIdentifiers();
    test_DateTimeFormatter();
    test_CurrencyFormatter();

    RoUninitialize();
}
