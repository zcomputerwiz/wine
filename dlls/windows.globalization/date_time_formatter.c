/* WinRT Windows.Globalization.DateTimeFormatting.DateTimeFormatter implementation
 *
 * Copyright (C) 2026 WineExplorer contributors
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

/*
 * A template is a list of words: shortdate, longdate, shorttime, longtime, year, month, day, dayofweek, hour,
 * minute, second, each with an optional ".full", ".abbreviated" or ".numeric" (".integer") modifier. The text is
 * made from the locale's date and time patterns (Win32 NLS): the pattern of the locale (the long one when the template
 * has a month name or a day of week, else the short one) is kept in its order and separators with only the fields the
 * template asks for. Unknown words are ignored, a template without a known word is a short date. The calendar is the
 * Gregorian one; numerals are not converted.
 */

#include "private.h"

#include <wctype.h>

#include "initguid.h"
#define WIDL_using_Windows_Globalization_DateTimeFormatting
#include "windows.globalization.datetimeformatting.h"

WINE_DEFAULT_DEBUG_CHANNEL(locale);

struct date_time_formatter
{
    IDateTimeFormatter IDateTimeFormatter_iface;
    IDateTimeFormatter2 IDateTimeFormatter2_iface;
    LONG ref;

    HSTRING tmpl, region, calendar, clock, numerals;
    HSTRING *languages;
    UINT32 language_count;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    YearFormat year;
    MonthFormat month;
    DayFormat day;
    DayOfWeekFormat day_of_week;
    HourFormat hour;
    MinuteFormat minute;
    SecondFormat second;
    BOOL short_base;
};

static inline struct date_time_formatter *impl_from_IDateTimeFormatter( IDateTimeFormatter *iface )
{
    return CONTAINING_RECORD( iface, struct date_time_formatter, IDateTimeFormatter_iface );
}

/* ---- patterns ---- */

#define MAX_ITEMS 64
#define MAX_PATTERN 256

struct item
{
    WCHAR key;          /* the pattern letter of a field, 0 for a literal */
    unsigned int len;   /* the repeat count of a field */
    const WCHAR *text;  /* a literal */
    unsigned int text_len;
};

static BOOL is_field_letter( WCHAR c )
{
    return c == 'd' || c == 'M' || c == 'y' || c == 'g' || c == 'h' || c == 'H' || c == 'm' || c == 's' || c == 't';
}

/* Keeps the fields of an NLS pattern as spec says: spec[key] is -1 to drop the field (with the separator before it, or
 * after it for the first one), 0 to keep it as it is, else the new repeat count. The key of a field is its letter, 'D'
 * for d repeated three times or more (the day of week); H is h. */
static void filter_pattern( const WCHAR *base, const signed char *spec, BOOL force_24, WCHAR *out )
{
    struct item items[MAX_ITEMS];
    unsigned int n = 0, i, o = 0, last_literal = 0;
    BOOL previous_literal = FALSE, any_field = FALSE, skip_literal = FALSE;
    const WCHAR *p = base, *start;

    while (*p && n < MAX_ITEMS)
    {
        start = p;
        if (*p == '\'')
        {
            p++;
            while (*p && *p != '\'') p++;
            if (*p) p++;
            items[n].key = 0;
        }
        else if (is_field_letter( *p ))
        {
            WCHAR c = *p;
            while (*p == c) p++;
            items[n].key = c;
            items[n].len = p - start;
            n++;
            continue;
        }
        else
        {
            while (*p && *p != '\'' && !is_field_letter( *p )) p++;
            items[n].key = 0;
        }
        items[n].text = start;
        items[n].text_len = p - start;
        n++;
    }

    for (i = 0; i < n; i++)
    {
        if (!items[i].key)
        {
            if (skip_literal)
            {
                skip_literal = FALSE;
                continue;
            }
            last_literal = o;
            if (o + items[i].text_len < MAX_PATTERN - 1)
            {
                memcpy( out + o, items[i].text, items[i].text_len * sizeof(WCHAR) );
                o += items[i].text_len;
            }
            previous_literal = TRUE;
        }
        else
        {
            WCHAR key = items[i].key == 'd' && items[i].len >= 3 ? 'D' : items[i].key == 'H' ? 'h' : items[i].key;
            signed char width = spec[key];
            unsigned int len, k;

            if (width < 0)
            {
                if (previous_literal) o = last_literal;
                else if (!any_field) skip_literal = TRUE;
                previous_literal = FALSE;
                continue;
            }
            len = width ? width : items[i].len;
            if (o + len >= MAX_PATTERN - 1) break;
            for (k = 0; k < len; k++)
                out[o++] = key == 'D' ? 'd' : (items[i].key == 'h' || items[i].key == 'H') && force_24 ? 'H' : items[i].key;
            previous_literal = FALSE;
            any_field = TRUE;
        }
    }
    out[o] = 0;
}

static BOOL has_date( const struct date_time_formatter *impl )
{
    return impl->year != YearFormat_None || impl->month != MonthFormat_None || impl->day != DayFormat_None ||
           impl->day_of_week != DayOfWeekFormat_None;
}

static BOOL has_time( const struct date_time_formatter *impl )
{
    return impl->hour != HourFormat_None || impl->minute != MinuteFormat_None || impl->second != SecondFormat_None;
}

static BOOL clock_is_12( const struct date_time_formatter *impl )
{
    return !wcscmp( WindowsGetStringRawBuffer( impl->clock, NULL ), L"12HourClock" );
}

static void date_pattern( const struct date_time_formatter *impl, WCHAR *out )
{
    WCHAR base[MAX_PATTERN];
    signed char spec[128];
    BOOL use_short = impl->short_base || (impl->month == MonthFormat_Numeric && impl->day_of_week == DayOfWeekFormat_None);

    memset( spec, -1, sizeof(spec) );
    spec['d'] = impl->day != DayFormat_None ? 0 : -1;
    spec['y'] = impl->year == YearFormat_None ? -1 : impl->year == YearFormat_Full ? 4 : impl->year == YearFormat_Abbreviated ? 2 : 0;
    switch (impl->month)
    {
    case MonthFormat_None: spec['M'] = -1; break;
    case MonthFormat_Full: spec['M'] = 4; break;
    case MonthFormat_Abbreviated: spec['M'] = 3; break;
    case MonthFormat_Numeric: spec['M'] = use_short ? 0 : 1; break;
    default: spec['M'] = 0; break;
    }
    switch (impl->day_of_week)
    {
    case DayOfWeekFormat_None: spec['D'] = -1; break;
    case DayOfWeekFormat_Full: spec['D'] = 4; break;
    case DayOfWeekFormat_Abbreviated: spec['D'] = 3; break;
    default: spec['D'] = 0; break;
    }
    if (!GetLocaleInfoEx( impl->locale, use_short ? LOCALE_SSHORTDATE : LOCALE_SLONGDATE, base, ARRAY_SIZE(base) ))
        wcscpy( base, use_short ? L"M/d/yyyy" : L"dddd, MMMM d, yyyy" );
    filter_pattern( base, spec, FALSE, out );
    if (!out[0] && has_date( impl )) wcscpy( out, use_short ? L"M/d/yyyy" : L"dddd, MMMM d, yyyy" );
}

static void time_pattern( const struct date_time_formatter *impl, WCHAR *out )
{
    WCHAR base[MAX_PATTERN];
    signed char spec[128];
    BOOL twelve = clock_is_12( impl ), hour = impl->hour != HourFormat_None;

    memset( spec, -1, sizeof(spec) );
    spec['h'] = hour ? 0 : -1;
    spec['m'] = impl->minute != MinuteFormat_None ? 0 : -1;
    spec['s'] = impl->second != SecondFormat_None ? 0 : -1;
    spec['t'] = hour && twelve ? 0 : -1;
    if (!GetLocaleInfoEx( impl->locale, impl->second != SecondFormat_None ? LOCALE_STIMEFORMAT : LOCALE_SSHORTTIME,
                          base, ARRAY_SIZE(base) ))
        wcscpy( base, impl->second != SecondFormat_None ? L"h:mm:ss tt" : L"h:mm tt" );
    filter_pattern( base, spec, !twelve, out );
    if (twelve && hour && !wcschr( out, 't' ) && wcslen( out ) + 4 < MAX_PATTERN) wcscat( out, L" tt" );
    if (twelve)
    {
        WCHAR *c;
        for (c = out; *c; c++) if (*c == 'H') *c = 'h';
    }
}

/* ---- templates ---- */

static void parse_template( struct date_time_formatter *impl, const WCHAR *text )
{
    WCHAR word[64];
    BOOL known = FALSE;
    const WCHAR *p = text;

    impl->year = YearFormat_None;
    impl->month = MonthFormat_None;
    impl->day = DayFormat_None;
    impl->day_of_week = DayOfWeekFormat_None;
    impl->hour = HourFormat_None;
    impl->minute = MinuteFormat_None;
    impl->second = SecondFormat_None;
    impl->short_base = FALSE;

    for (;;)
    {
        unsigned int len = 0;
        WCHAR *dot;
        BOOL full, abbreviated, numeric;

        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        while (*p && *p != ' ' && *p != '\t')
        {
            if (len < ARRAY_SIZE(word) - 1) word[len++] = towlower( *p );
            p++;
        }
        word[len] = 0;
        if ((dot = wcschr( word, '.' ))) *dot++ = 0;
        full = dot && wcsstr( dot, L"full" );
        abbreviated = dot && wcsstr( dot, L"abbreviated" );
        numeric = dot && (wcsstr( dot, L"numeric" ) || wcsstr( dot, L"integer" ));

        if (!wcscmp( word, L"shortdate" ))
        {
            impl->year = YearFormat_Default;
            impl->month = MonthFormat_Default;
            impl->day = DayFormat_Default;
            impl->short_base = TRUE;
        }
        else if (!wcscmp( word, L"longdate" ))
        {
            impl->year = YearFormat_Full;
            impl->month = MonthFormat_Full;
            impl->day = DayFormat_Default;
            impl->day_of_week = DayOfWeekFormat_Full;
        }
        else if (!wcscmp( word, L"shorttime" ))
        {
            impl->hour = HourFormat_Default;
            impl->minute = MinuteFormat_Default;
        }
        else if (!wcscmp( word, L"longtime" ))
        {
            impl->hour = HourFormat_Default;
            impl->minute = MinuteFormat_Default;
            impl->second = SecondFormat_Default;
        }
        else if (!wcscmp( word, L"year" ))
            impl->year = full ? YearFormat_Full : abbreviated ? YearFormat_Abbreviated : YearFormat_Default;
        else if (!wcscmp( word, L"month" ))
            impl->month = full ? MonthFormat_Full : abbreviated ? MonthFormat_Abbreviated :
                          numeric ? MonthFormat_Numeric : MonthFormat_Default;
        else if (!wcscmp( word, L"day" )) impl->day = DayFormat_Default;
        else if (!wcscmp( word, L"dayofweek" ))
            impl->day_of_week = full ? DayOfWeekFormat_Full : abbreviated ? DayOfWeekFormat_Abbreviated : DayOfWeekFormat_Default;
        else if (!wcscmp( word, L"hour" )) impl->hour = HourFormat_Default;
        else if (!wcscmp( word, L"minute" )) impl->minute = MinuteFormat_Default;
        else if (!wcscmp( word, L"second" )) impl->second = SecondFormat_Default;
        else
        {
            WARN( "unknown template word %s.\n", debugstr_w( word ) );
            continue;
        }
        known = TRUE;
    }

    if (!known)
    {
        impl->year = YearFormat_Default;
        impl->month = MonthFormat_Default;
        impl->day = DayFormat_Default;
        impl->short_base = TRUE;
    }
}

/* ---- formatting ---- */

static HRESULT format_value( struct date_time_formatter *impl, INT64 ticks, BOOL utc, HSTRING *result )
{
    WCHAR pattern[MAX_PATTERN], text[2 * MAX_PATTERN], *p = text;
    SYSTEMTIME universal, local;
    FILETIME ft;
    int len;

    if (ticks < 0) return E_INVALIDARG;
    ft.dwLowDateTime = (UINT32)ticks;
    ft.dwHighDateTime = ticks >> 32;
    if (!FileTimeToSystemTime( &ft, &universal )) return E_INVALIDARG;
    if (utc) local = universal;
    else if (!SystemTimeToTzSpecificLocalTime( NULL, &universal, &local )) local = universal;

    text[0] = 0;
    if (has_date( impl ))
    {
        date_pattern( impl, pattern );
        if (!(len = GetDateFormatEx( impl->locale, 0, &local, pattern, p, MAX_PATTERN, NULL ))) return E_FAIL;
        p += len - 1;
    }
    if (has_time( impl ))
    {
        if (p != text) *p++ = ' ';
        time_pattern( impl, pattern );
        if (!(len = GetTimeFormatEx( impl->locale, 0, &local, pattern, p, MAX_PATTERN )))
            return E_FAIL;
        p += len - 1;
    }
    TRACE( "formatted %s.\n", debugstr_w( text ) );
    return WindowsCreateString( text, p - text, result );
}

/* ---- IDateTimeFormatter ---- */

static HRESULT WINAPI formatter_QueryInterface( IDateTimeFormatter *iface, REFIID iid, void **out )
{
    struct date_time_formatter *impl = impl_from_IDateTimeFormatter( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDateTimeFormatter ))
        *out = &impl->IDateTimeFormatter_iface;
    else if (IsEqualGUID( iid, &IID_IDateTimeFormatter2 )) *out = &impl->IDateTimeFormatter2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI formatter_AddRef( IDateTimeFormatter *iface )
{
    return InterlockedIncrement( &impl_from_IDateTimeFormatter( iface )->ref );
}

static ULONG WINAPI formatter_Release( IDateTimeFormatter *iface )
{
    struct date_time_formatter *impl = impl_from_IDateTimeFormatter( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->language_count; i++) WindowsDeleteString( impl->languages[i] );
        free( impl->languages );
        WindowsDeleteString( impl->tmpl );
        WindowsDeleteString( impl->region );
        WindowsDeleteString( impl->calendar );
        WindowsDeleteString( impl->clock );
        WindowsDeleteString( impl->numerals );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI formatter_GetIids( IDateTimeFormatter *iface, ULONG *iid_count, IID **iids )
{
    static const GUID *const list[] = { &IID_IDateTimeFormatter, &IID_IDateTimeFormatter2 };

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    return inspectable_get_iids( list, ARRAY_SIZE(list), iid_count, iids );
}

static HRESULT WINAPI formatter_GetRuntimeClassName( IDateTimeFormatter *iface, HSTRING *class_name )
{
    return inspectable_get_class_name( L"Windows.Globalization.DateTimeFormatting.DateTimeFormatter", class_name );
}

static HRESULT WINAPI formatter_GetTrustLevel( IDateTimeFormatter *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI formatter_get_Languages( IDateTimeFormatter *iface, IVectorView_HSTRING **value )
{
    struct date_time_formatter *impl = impl_from_IDateTimeFormatter( iface );
    HSTRING *copy;
    UINT32 i;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (!(copy = calloc( impl->language_count ? impl->language_count : 1, sizeof(*copy) ))) return E_OUTOFMEMORY;
    for (i = 0; i < impl->language_count; i++) WindowsDuplicateString( impl->languages[i], &copy[i] );
    hr = hstring_vector_create( copy, impl->language_count, value );
    if (FAILED(hr)) for (i = 0; i < impl->language_count; i++) WindowsDeleteString( copy[i] );
    free( copy );
    return hr;
}

static HRESULT WINAPI formatter_get_GeographicRegion( IDateTimeFormatter *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IDateTimeFormatter( iface )->region, value );
}

static HRESULT WINAPI formatter_get_Calendar( IDateTimeFormatter *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IDateTimeFormatter( iface )->calendar, value );
}

static HRESULT WINAPI formatter_get_Clock( IDateTimeFormatter *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IDateTimeFormatter( iface )->clock, value );
}

static HRESULT WINAPI formatter_get_NumeralSystem( IDateTimeFormatter *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IDateTimeFormatter( iface )->numerals, value );
}

static HRESULT WINAPI formatter_put_NumeralSystem( IDateTimeFormatter *iface, HSTRING value )
{
    struct date_time_formatter *impl = impl_from_IDateTimeFormatter( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (!WindowsGetStringLen( value )) return E_INVALIDARG;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->numerals );
    impl->numerals = copy;
    return S_OK;
}

static HRESULT WINAPI formatter_get_Patterns( IDateTimeFormatter *iface, IVectorView_HSTRING **value )
{
    struct date_time_formatter *impl = impl_from_IDateTimeFormatter( iface );
    WCHAR date[MAX_PATTERN] = {0}, time[MAX_PATTERN] = {0}, text[2 * MAX_PATTERN];
    HSTRING pattern;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (has_date( impl )) date_pattern( impl, date );
    if (has_time( impl )) time_pattern( impl, time );
    wcscpy( text, date );
    if (date[0] && time[0]) wcscat( text, L" " );
    wcscat( text, time );
    if (FAILED(hr = WindowsCreateString( text, wcslen( text ), &pattern ))) return hr;
    if (FAILED(hr = hstring_vector_create( &pattern, 1, value ))) WindowsDeleteString( pattern );
    return hr;
}

static HRESULT WINAPI formatter_get_Template( IDateTimeFormatter *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IDateTimeFormatter( iface )->tmpl, value );
}

static HRESULT WINAPI formatter_Format( IDateTimeFormatter *iface, DateTime value, HSTRING *result )
{
    TRACE( "iface %p, value %I64d, result %p.\n", iface, value.UniversalTime, result );
    if (!result) return E_POINTER;
    return format_value( impl_from_IDateTimeFormatter( iface ), value.UniversalTime, FALSE, result );
}

static HRESULT WINAPI formatter_get_IncludeYear( IDateTimeFormatter *iface, YearFormat *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDateTimeFormatter( iface )->year;
    return S_OK;
}

static HRESULT WINAPI formatter_get_IncludeMonth( IDateTimeFormatter *iface, MonthFormat *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDateTimeFormatter( iface )->month;
    return S_OK;
}

static HRESULT WINAPI formatter_get_IncludeDayOfWeek( IDateTimeFormatter *iface, DayOfWeekFormat *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDateTimeFormatter( iface )->day_of_week;
    return S_OK;
}

static HRESULT WINAPI formatter_get_IncludeDay( IDateTimeFormatter *iface, DayFormat *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDateTimeFormatter( iface )->day;
    return S_OK;
}

static HRESULT WINAPI formatter_get_IncludeHour( IDateTimeFormatter *iface, HourFormat *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDateTimeFormatter( iface )->hour;
    return S_OK;
}

static HRESULT WINAPI formatter_get_IncludeMinute( IDateTimeFormatter *iface, MinuteFormat *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDateTimeFormatter( iface )->minute;
    return S_OK;
}

static HRESULT WINAPI formatter_get_IncludeSecond( IDateTimeFormatter *iface, SecondFormat *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDateTimeFormatter( iface )->second;
    return S_OK;
}

static HRESULT WINAPI formatter_get_ResolvedLanguage( IDateTimeFormatter *iface, HSTRING *value )
{
    const WCHAR *locale = impl_from_IDateTimeFormatter( iface )->locale;
    if (!value) return E_POINTER;
    return WindowsCreateString( locale, wcslen( locale ), value );
}

static HRESULT WINAPI formatter_get_ResolvedGeographicRegion( IDateTimeFormatter *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsCreateString( L"ZZ", 2, value );
}

static const IDateTimeFormatterVtbl formatter_vtbl =
{
    formatter_QueryInterface,
    formatter_AddRef,
    formatter_Release,
    /* IInspectable methods */
    formatter_GetIids,
    formatter_GetRuntimeClassName,
    formatter_GetTrustLevel,
    /* IDateTimeFormatter methods */
    formatter_get_Languages,
    formatter_get_GeographicRegion,
    formatter_get_Calendar,
    formatter_get_Clock,
    formatter_get_NumeralSystem,
    formatter_put_NumeralSystem,
    formatter_get_Patterns,
    formatter_get_Template,
    formatter_Format,
    formatter_get_IncludeYear,
    formatter_get_IncludeMonth,
    formatter_get_IncludeDayOfWeek,
    formatter_get_IncludeDay,
    formatter_get_IncludeHour,
    formatter_get_IncludeMinute,
    formatter_get_IncludeSecond,
    formatter_get_ResolvedLanguage,
    formatter_get_ResolvedGeographicRegion,
};

/* ---- IDateTimeFormatter2 ---- */

DEFINE_IINSPECTABLE( formatter2, IDateTimeFormatter2, struct date_time_formatter, IDateTimeFormatter_iface )

static HRESULT WINAPI formatter2_FormatUsingTimeZone( IDateTimeFormatter2 *iface, DateTime value, HSTRING zone,
                                                      HSTRING *result )
{
    const WCHAR *id = WindowsGetStringRawBuffer( zone, NULL );
    BOOL utc = !wcsicmp( id, L"UTC" ) || !wcsicmp( id, L"Etc/UTC" ) || !wcsicmp( id, L"Z" );

    TRACE( "iface %p, value %I64d, zone %s, result %p.\n", iface, value.UniversalTime, debugstr_hstring( zone ), result );
    if (!result) return E_POINTER;
    if (!utc) FIXME( "time zone %s, using the local one.\n", debugstr_w( id ) );
    return format_value( impl_from_IDateTimeFormatter2( iface ), value.UniversalTime, utc, result );
}

static const IDateTimeFormatter2Vtbl formatter2_vtbl =
{
    formatter2_QueryInterface,
    formatter2_AddRef,
    formatter2_Release,
    /* IInspectable methods */
    formatter2_GetIids,
    formatter2_GetRuntimeClassName,
    formatter2_GetTrustLevel,
    /* IDateTimeFormatter2 methods */
    formatter2_FormatUsingTimeZone,
};

/* ---- creation ---- */

/* the user's region (ISO 3166 two letters) */
static HRESULT user_region( HSTRING *out )
{
    WCHAR iso[8];

    if (!GetGeoInfoW( GetUserGeoID( GEOCLASS_NATION ), GEO_ISO2, iso, ARRAY_SIZE(iso), 0 )) wcscpy( iso, L"US" );
    return WindowsCreateString( iso, wcslen( iso ), out );
}

/* the user's language (one string, created here) and region */
static HRESULT user_context( HSTRING **languages, HSTRING *region )
{
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    HRESULT hr;

    if (!GetUserDefaultLocaleName( locale, ARRAY_SIZE(locale) )) wcscpy( locale, L"en-US" );
    if (!(*languages = calloc( 1, sizeof(**languages) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = WindowsCreateString( locale, wcslen( locale ), &(*languages)[0] )))
    {
        free( *languages );
        return hr;
    }
    if (FAILED(hr = user_region( region )))
    {
        WindowsDeleteString( (*languages)[0] );
        free( *languages );
    }
    return hr;
}

/* takes the languages (an array of count strings, count 0 for the user's) and the region (NULL for the user's);
 * calendar and clock may be NULL or empty for the defaults, and are copied */
static HRESULT formatter_create( const WCHAR *text, UINT32 length, HSTRING *languages, UINT32 count, HSTRING region,
                                 HSTRING calendar, HSTRING clock, IDateTimeFormatter **out )
{
    struct date_time_formatter *impl;
    WCHAR name[LOCALE_NAME_MAX_LENGTH], format[MAX_PATTERN];
    const WCHAR *clock_name;
    BOOL made = FALSE;
    HRESULT hr;
    UINT32 i;

    if (!length) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDateTimeFormatter_iface.lpVtbl = &formatter_vtbl;
    impl->IDateTimeFormatter2_iface.lpVtbl = &formatter2_vtbl;
    impl->ref = 1;

    if (!count)
    {
        if (FAILED(hr = user_context( &languages, &region ))) goto failed;
        count = 1;
        made = TRUE;
    }
    impl->languages = languages;
    impl->language_count = count;
    impl->region = region;

    /* the first language the system knows, else the user's */
    for (i = 0; i < count; i++)
        if (GetLocaleInfoEx( WindowsGetStringRawBuffer( languages[i], NULL ), LOCALE_SNAME, name, ARRAY_SIZE(name) ) &&
            name[0] && wcscmp( name, L"!x-sys-default-locale" ))
            break;
    if (i == count && !GetUserDefaultLocaleName( name, ARRAY_SIZE(name) )) wcscpy( name, L"en-US" );
    wcscpy( impl->locale, name );

    if (FAILED(hr = WindowsCreateString( text, length, &impl->tmpl ))) goto failed;
    if (FAILED(hr = WindowsCreateString( L"Latn", 4, &impl->numerals ))) goto failed;
    if (WindowsGetStringLen( calendar )) hr = WindowsDuplicateString( calendar, &impl->calendar );
    else hr = WindowsCreateString( L"GregorianCalendar", 17, &impl->calendar );
    if (FAILED(hr)) goto failed;
    if (WindowsGetStringLen( clock )) hr = WindowsDuplicateString( clock, &impl->clock );
    else
    {
        clock_name = L"12HourClock";
        if (GetLocaleInfoEx( impl->locale, LOCALE_STIMEFORMAT, format, ARRAY_SIZE(format) ) && wcschr( format, 'H' ))
            clock_name = L"24HourClock";
        hr = WindowsCreateString( clock_name, wcslen( clock_name ), &impl->clock );
    }
    if (FAILED(hr)) goto failed;

    parse_template( impl, text );
    *out = &impl->IDateTimeFormatter_iface;
    TRACE( "created %p, template %s, locale %s.\n", impl, debugstr_wn( text, length ), debugstr_w( impl->locale ) );
    return S_OK;

failed:
    /* the languages and the region stay with the caller unless they were made here */
    if (made)
    {
        for (i = 0; i < count; i++) WindowsDeleteString( languages[i] );
        free( languages );
        WindowsDeleteString( region );
    }
    WindowsDeleteString( impl->tmpl );
    WindowsDeleteString( impl->calendar );
    WindowsDeleteString( impl->clock );
    WindowsDeleteString( impl->numerals );
    free( impl );
    return hr;
}

/* a formatter for a template, the user's context */
static HRESULT formatter_create_default( const WCHAR *text, IDateTimeFormatter **out )
{
    return formatter_create( text, wcslen( text ), NULL, 0, NULL, NULL, NULL, out );
}

/* the template words of the format enumerations */
static UINT32 enum_template( WCHAR *out, YearFormat year, MonthFormat month, DayFormat day, DayOfWeekFormat day_of_week,
                             HourFormat hour, MinuteFormat minute, SecondFormat second )
{
    static const WCHAR *const year_words[] = { NULL, L"year", L"year.abbreviated", L"year.full" };
    static const WCHAR *const month_words[] = { NULL, L"month", L"month.abbreviated", L"month.full", L"month.numeric" };
    static const WCHAR *const week_words[] = { NULL, L"dayofweek", L"dayofweek.abbreviated", L"dayofweek.full" };
    const WCHAR *words[7] = {0};
    UINT32 i, n = 0;

    if (year >= 0 && year < ARRAY_SIZE(year_words)) words[0] = year_words[year];
    if (month >= 0 && month < ARRAY_SIZE(month_words)) words[1] = month_words[month];
    if (day == DayFormat_Default) words[2] = L"day";
    if (day_of_week >= 0 && day_of_week < ARRAY_SIZE(week_words)) words[3] = week_words[day_of_week];
    if (hour == HourFormat_Default) words[4] = L"hour";
    if (minute == MinuteFormat_Default) words[5] = L"minute";
    if (second == SecondFormat_Default) words[6] = L"second";

    out[0] = 0;
    for (i = 0; i < ARRAY_SIZE(words); i++)
    {
        if (!words[i]) continue;
        if (n++) wcscat( out, L" " );
        wcscat( out, words[i] );
    }
    return wcslen( out );
}

struct factory
{
    IActivationFactory IActivationFactory_iface;
    IDateTimeFormatterFactory IDateTimeFormatterFactory_iface;
    IDateTimeFormatterStatics IDateTimeFormatterStatics_iface;
    LONG ref;
};

static inline struct factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IDateTimeFormatterFactory ))
        *out = &impl->IDateTimeFormatterFactory_iface;
    else if (IsEqualGUID( iid, &IID_IDateTimeFormatterStatics ))
        *out = &impl->IDateTimeFormatterStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const GUID *const list[] = { &IID_IDateTimeFormatterFactory, &IID_IDateTimeFormatterStatics };

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    return inspectable_get_iids( list, ARRAY_SIZE(list), iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return inspectable_get_class_name( L"Windows.Globalization.DateTimeFormatting.DateTimeFormatter", class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
    return formatter_create_default( L"shortdate shorttime", (IDateTimeFormatter **)instance );
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

/* ---- IDateTimeFormatterFactory ---- */

DEFINE_IINSPECTABLE( creator, IDateTimeFormatterFactory, struct factory, IActivationFactory_iface )

static HRESULT WINAPI creator_CreateDateTimeFormatter( IDateTimeFormatterFactory *iface, HSTRING text,
                                                       IDateTimeFormatter **result )
{
    UINT32 len;
    const WCHAR *str = WindowsGetStringRawBuffer( text, &len );

    TRACE( "iface %p, text %s, result %p.\n", iface, debugstr_hstring( text ), result );
    if (!result) return E_POINTER;
    *result = NULL;
    return formatter_create( str, len, NULL, 0, NULL, NULL, NULL, (IDateTimeFormatter **)result );
}

/* the template, languages (an iterable, may be NULL for the user's), region, calendar and clock */
static HRESULT create_with_context( HSTRING text, IIterable_HSTRING *iterable, HSTRING region, HSTRING calendar,
                                    HSTRING clock, IDateTimeFormatter **result )
{
    HSTRING *list = NULL, copy = NULL;
    UINT32 count = 0, len, i;
    const WCHAR *str = WindowsGetStringRawBuffer( text, &len );
    HRESULT hr;

    if (!result) return E_POINTER;
    *result = NULL;
    if (iterable)
    {
        if (FAILED(hr = languages_from_iterable( iterable, &list, &count ))) return hr;
        if (region && !WindowsGetStringLen( region )) hr = E_INVALIDARG;
        else if (region) hr = WindowsDuplicateString( region, &copy );
        else hr = user_region( &copy );
        if (FAILED(hr)) goto failed;
    }
    if (FAILED(hr = formatter_create( str, len, list, count, copy, calendar, clock, (IDateTimeFormatter **)result )))
    {
        WindowsDeleteString( copy );
        goto failed;
    }
    return S_OK;

failed:
    for (i = 0; i < count; i++) WindowsDeleteString( list[i] );
    free( list );
    return hr;
}

static HRESULT WINAPI creator_CreateDateTimeFormatterLanguages( IDateTimeFormatterFactory *iface, HSTRING text,
                                                                IIterable_HSTRING *languages, IDateTimeFormatter **result )
{
    TRACE( "iface %p, text %s, languages %p, result %p.\n", iface, debugstr_hstring( text ), languages, result );
    if (!languages) return E_INVALIDARG;
    return create_with_context( text, languages, NULL, NULL, NULL, result );
}

static HRESULT WINAPI creator_CreateDateTimeFormatterContext( IDateTimeFormatterFactory *iface, HSTRING text,
                                                              IIterable_HSTRING *languages, HSTRING region,
                                                              HSTRING calendar, HSTRING clock, IDateTimeFormatter **result )
{
    TRACE( "iface %p, text %s, languages %p, region %s, calendar %s, clock %s, result %p.\n", iface,
           debugstr_hstring( text ), languages, debugstr_hstring( region ), debugstr_hstring( calendar ),
           debugstr_hstring( clock ), result );
    if (!languages) return E_INVALIDARG;
    return create_with_context( text, languages, region, calendar, clock, result );
}

static HRESULT WINAPI creator_CreateDateTimeFormatterDate( IDateTimeFormatterFactory *iface, YearFormat year,
                                                           MonthFormat month, DayFormat day, DayOfWeekFormat day_of_week,
                                                           IDateTimeFormatter **result )
{
    WCHAR text[128];
    UINT32 len;

    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = NULL;
    len = enum_template( text, year, month, day, day_of_week, HourFormat_None, MinuteFormat_None, SecondFormat_None );
    return formatter_create( text, len, NULL, 0, NULL, NULL, NULL, (IDateTimeFormatter **)result );
}

static HRESULT WINAPI creator_CreateDateTimeFormatterTime( IDateTimeFormatterFactory *iface, HourFormat hour,
                                                           MinuteFormat minute, SecondFormat second,
                                                           IDateTimeFormatter **result )
{
    WCHAR text[128];
    UINT32 len;

    TRACE( "iface %p, result %p.\n", iface, result );
    if (!result) return E_POINTER;
    *result = NULL;
    len = enum_template( text, YearFormat_None, MonthFormat_None, DayFormat_None, DayOfWeekFormat_None, hour, minute,
                         second );
    return formatter_create( text, len, NULL, 0, NULL, NULL, NULL, (IDateTimeFormatter **)result );
}

static HRESULT WINAPI creator_CreateDateTimeFormatterDateTimeLanguages( IDateTimeFormatterFactory *iface, YearFormat year,
                                                                        MonthFormat month, DayFormat day,
                                                                        DayOfWeekFormat day_of_week, HourFormat hour,
                                                                        MinuteFormat minute, SecondFormat second,
                                                                        IIterable_HSTRING *languages,
                                                                        IDateTimeFormatter **result )
{
    WCHAR text[128];
    HSTRING str;
    HRESULT hr;

    TRACE( "iface %p, languages %p, result %p.\n", iface, languages, result );
    if (!languages) return E_INVALIDARG;
    enum_template( text, year, month, day, day_of_week, hour, minute, second );
    if (FAILED(hr = WindowsCreateString( text, wcslen( text ), &str ))) return hr;
    hr = create_with_context( str, languages, NULL, NULL, NULL, result );
    WindowsDeleteString( str );
    return hr;
}

static HRESULT WINAPI creator_CreateDateTimeFormatterDateTimeContext( IDateTimeFormatterFactory *iface, YearFormat year,
                                                                      MonthFormat month, DayFormat day,
                                                                      DayOfWeekFormat day_of_week, HourFormat hour,
                                                                      MinuteFormat minute, SecondFormat second,
                                                                      IIterable_HSTRING *languages, HSTRING region,
                                                                      HSTRING calendar, HSTRING clock,
                                                                      IDateTimeFormatter **result )
{
    WCHAR text[128];
    HSTRING str;
    HRESULT hr;

    TRACE( "iface %p, languages %p, region %s, result %p.\n", iface, languages, debugstr_hstring( region ), result );
    if (!languages) return E_INVALIDARG;
    enum_template( text, year, month, day, day_of_week, hour, minute, second );
    if (FAILED(hr = WindowsCreateString( text, wcslen( text ), &str ))) return hr;
    hr = create_with_context( str, languages, region, calendar, clock, result );
    WindowsDeleteString( str );
    return hr;
}

static const IDateTimeFormatterFactoryVtbl creator_vtbl =
{
    creator_QueryInterface,
    creator_AddRef,
    creator_Release,
    /* IInspectable methods */
    creator_GetIids,
    creator_GetRuntimeClassName,
    creator_GetTrustLevel,
    /* IDateTimeFormatterFactory methods */
    creator_CreateDateTimeFormatter,
    creator_CreateDateTimeFormatterLanguages,
    creator_CreateDateTimeFormatterContext,
    creator_CreateDateTimeFormatterDate,
    creator_CreateDateTimeFormatterTime,
    creator_CreateDateTimeFormatterDateTimeLanguages,
    creator_CreateDateTimeFormatterDateTimeContext,
};

/* ---- IDateTimeFormatterStatics ---- */

DEFINE_IINSPECTABLE( statics, IDateTimeFormatterStatics, struct factory, IActivationFactory_iface )

static HRESULT WINAPI statics_get_LongDate( IDateTimeFormatterStatics *iface, IDateTimeFormatter **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return formatter_create_default( L"longdate", (IDateTimeFormatter **)value );
}

static HRESULT WINAPI statics_get_LongTime( IDateTimeFormatterStatics *iface, IDateTimeFormatter **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return formatter_create_default( L"longtime", (IDateTimeFormatter **)value );
}

static HRESULT WINAPI statics_get_ShortDate( IDateTimeFormatterStatics *iface, IDateTimeFormatter **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return formatter_create_default( L"shortdate", (IDateTimeFormatter **)value );
}

static HRESULT WINAPI statics_get_ShortTime( IDateTimeFormatterStatics *iface, IDateTimeFormatter **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return formatter_create_default( L"shorttime", (IDateTimeFormatter **)value );
}

static const IDateTimeFormatterStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IDateTimeFormatterStatics methods */
    statics_get_LongDate,
    statics_get_LongTime,
    statics_get_ShortDate,
    statics_get_ShortTime,
};

static struct factory factory =
{
    {&factory_vtbl},
    {&creator_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *date_time_formatter_factory = &factory.IActivationFactory_iface;
