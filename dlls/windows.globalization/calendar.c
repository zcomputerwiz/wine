/* WinRT Windows.Globalization.Calendar implementation
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

/*
 * The Gregorian calendar (proleptic, years 1 to 9999 as on Windows) over a Windows.Foundation.DateTime (100 ns units
 * since 1601-01-01 UTC), seen in a time zone: UTC, or a Windows time zone found through its IANA name (the names this
 * API uses). Components are read and set in the time zone's local time; Add* of years, months, weeks, days and periods
 * keep the local time of day, Add* of hours and smaller units move the instant. Texts come from the locale of the
 * calendar's language. Other calendar systems are accepted by name and computed as Gregorian (FIXME).
 */

#include <limits.h>
#include <stdlib.h>

#include "private.h"
#include "winnls.h"

WINE_DEFAULT_DEBUG_CHANNEL(locale);

#define TICKS_PER_SECOND 10000000
#define TICKS_PER_DAY    ((INT64)86400 * TICKS_PER_SECOND)
#define DAYS_1601_TO_1970 134774
#define MIN_DATETIME (-504911232000000000)       /* 0001-01-01 00:00:00 UTC */
#define MAX_DATETIME  2650467743999999999        /* 9999-12-31 23:59:59.9999999 UTC */

/* ---- dates ---------------------------------------------------------------------------------------------------- */

struct fields
{
    INT32 year, month, day, hour, minute, second;
    INT64 ticks;          /* within the second */
};

/* days since 1970-01-01 of a proleptic Gregorian date, and back (H. Hinnant's algorithms) */
static INT64 days_from_civil( INT64 y, INT64 m, INT64 d )
{
    INT64 era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days( INT64 z, INT32 *year, INT32 *month, INT32 *day )
{
    INT64 era, doe, yoe, y, doy, mp, d, m;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    *year = y + (m <= 2);
    *month = m;
    *day = d;
}

static BOOL is_leap( INT32 year )
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static INT32 days_in_month( INT32 year, INT32 month )
{
    static const INT32 days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && is_leap( year ) ? 29 : days[month - 1];
}

static INT64 floor_div( INT64 a, INT64 b )
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static void fields_from_ticks( INT64 local, struct fields *f )
{
    INT64 days = floor_div( local, TICKS_PER_DAY ), rest = local - days * TICKS_PER_DAY;
    civil_from_days( days - DAYS_1601_TO_1970, &f->year, &f->month, &f->day );
    f->hour = rest / ((INT64)3600 * TICKS_PER_SECOND);
    rest -= (INT64)f->hour * 3600 * TICKS_PER_SECOND;
    f->minute = rest / (60 * TICKS_PER_SECOND);
    rest -= (INT64)f->minute * 60 * TICKS_PER_SECOND;
    f->second = rest / TICKS_PER_SECOND;
    f->ticks = rest - (INT64)f->second * TICKS_PER_SECOND;
}

static INT64 ticks_from_fields( const struct fields *f )
{
    INT64 days = days_from_civil( f->year, f->month, f->day ) + DAYS_1601_TO_1970;
    return days * TICKS_PER_DAY + (((INT64)f->hour * 60 + f->minute) * 60 + f->second) * TICKS_PER_SECOND + f->ticks;
}

/* ---- time zones ------------------------------------------------------------------------------------------------ */

/* Windows time zone key names and their IANA names (CLDR windowsZones, territory 001), with abbreviations where the
 * names are in common use */
static const struct zone_name
{
    const WCHAR *windows, *iana, *standard_abbrev, *daylight_abbrev;
}
zone_names[] =
{
    { L"Dateline Standard Time", L"Etc/GMT+12" },
    { L"Hawaiian Standard Time", L"Pacific/Honolulu", L"HST", L"HDT" },
    { L"Alaskan Standard Time", L"America/Anchorage", L"AKST", L"AKDT" },
    { L"Pacific Standard Time", L"America/Los_Angeles", L"PST", L"PDT" },
    { L"US Mountain Standard Time", L"America/Phoenix", L"MST", L"MDT" },
    { L"Mountain Standard Time", L"America/Denver", L"MST", L"MDT" },
    { L"Central America Standard Time", L"America/Guatemala" },
    { L"Central Standard Time", L"America/Chicago", L"CST", L"CDT" },
    { L"Canada Central Standard Time", L"America/Regina" },
    { L"Central Standard Time (Mexico)", L"America/Mexico_City" },
    { L"SA Pacific Standard Time", L"America/Bogota" },
    { L"Eastern Standard Time", L"America/New_York", L"EST", L"EDT" },
    { L"US Eastern Standard Time", L"America/Indianapolis", L"EST", L"EDT" },
    { L"Atlantic Standard Time", L"America/Halifax", L"AST", L"ADT" },
    { L"Venezuela Standard Time", L"America/Caracas" },
    { L"SA Western Standard Time", L"America/La_Paz" },
    { L"Pacific SA Standard Time", L"America/Santiago" },
    { L"Newfoundland Standard Time", L"America/St_Johns" },
    { L"E. South America Standard Time", L"America/Sao_Paulo" },
    { L"Argentina Standard Time", L"America/Buenos_Aires" },
    { L"Greenland Standard Time", L"America/Godthab" },
    { L"UTC-02", L"Etc/GMT+2" },
    { L"Azores Standard Time", L"Atlantic/Azores" },
    { L"Cape Verde Standard Time", L"Atlantic/Cape_Verde" },
    { L"GMT Standard Time", L"Europe/London", L"GMT", L"BST" },
    { L"Greenwich Standard Time", L"Atlantic/Reykjavik" },
    { L"Morocco Standard Time", L"Africa/Casablanca" },
    { L"W. Europe Standard Time", L"Europe/Berlin" },
    { L"Central Europe Standard Time", L"Europe/Budapest" },
    { L"Romance Standard Time", L"Europe/Paris" },
    { L"Central European Standard Time", L"Europe/Warsaw" },
    { L"W. Central Africa Standard Time", L"Africa/Lagos" },
    { L"GTB Standard Time", L"Europe/Bucharest" },
    { L"E. Europe Standard Time", L"Europe/Chisinau" },
    { L"FLE Standard Time", L"Europe/Kiev" },
    { L"Israel Standard Time", L"Asia/Jerusalem" },
    { L"Egypt Standard Time", L"Africa/Cairo" },
    { L"South Africa Standard Time", L"Africa/Johannesburg" },
    { L"Turkey Standard Time", L"Europe/Istanbul" },
    { L"Arabic Standard Time", L"Asia/Baghdad" },
    { L"Arab Standard Time", L"Asia/Riyadh" },
    { L"Russian Standard Time", L"Europe/Moscow", L"MSK", L"MSK" },
    { L"E. Africa Standard Time", L"Africa/Nairobi" },
    { L"Iran Standard Time", L"Asia/Tehran" },
    { L"Arabian Standard Time", L"Asia/Dubai" },
    { L"Afghanistan Standard Time", L"Asia/Kabul" },
    { L"Pakistan Standard Time", L"Asia/Karachi" },
    { L"West Asia Standard Time", L"Asia/Tashkent" },
    { L"India Standard Time", L"Asia/Calcutta", L"IST", L"IST" },
    { L"Nepal Standard Time", L"Asia/Katmandu" },
    { L"Bangladesh Standard Time", L"Asia/Dhaka" },
    { L"Myanmar Standard Time", L"Asia/Rangoon" },
    { L"SE Asia Standard Time", L"Asia/Bangkok" },
    { L"China Standard Time", L"Asia/Shanghai" },
    { L"Singapore Standard Time", L"Asia/Singapore" },
    { L"Taipei Standard Time", L"Asia/Taipei" },
    { L"W. Australia Standard Time", L"Australia/Perth" },
    { L"Tokyo Standard Time", L"Asia/Tokyo", L"JST", L"JDT" },
    { L"Korea Standard Time", L"Asia/Seoul", L"KST", L"KDT" },
    { L"Cen. Australia Standard Time", L"Australia/Adelaide" },
    { L"AUS Central Standard Time", L"Australia/Darwin" },
    { L"E. Australia Standard Time", L"Australia/Brisbane" },
    { L"AUS Eastern Standard Time", L"Australia/Sydney" },
    { L"West Pacific Standard Time", L"Pacific/Port_Moresby" },
    { L"Tasmania Standard Time", L"Australia/Hobart" },
    { L"Central Pacific Standard Time", L"Pacific/Guadalcanal" },
    { L"New Zealand Standard Time", L"Pacific/Auckland", L"NZST", L"NZDT" },
    { L"Fiji Standard Time", L"Pacific/Fiji" },
    { L"Tonga Standard Time", L"Pacific/Tongatapu" },
};

struct time_zone
{
    BOOL utc;
    WCHAR id[64];                               /* IANA name, or the Windows name when it has none in the table */
    const struct zone_name *names;
    DYNAMIC_TIME_ZONE_INFORMATION info;
};

static void time_zone_utc( struct time_zone *tz )
{
    memset( tz, 0, sizeof(*tz) );
    tz->utc = TRUE;
    wcscpy( tz->id, L"Etc/UTC" );
}

static void time_zone_local( struct time_zone *tz )
{
    unsigned int i;

    memset( tz, 0, sizeof(*tz) );
    if (GetDynamicTimeZoneInformation( &tz->info ) == TIME_ZONE_ID_INVALID || !tz->info.TimeZoneKeyName[0])
    {
        time_zone_utc( tz );
        return;
    }
    if (!wcscmp( tz->info.TimeZoneKeyName, L"UTC" ) || !wcscmp( tz->info.TimeZoneKeyName, L"Coordinated Universal Time" ))
    {
        time_zone_utc( tz );
        return;
    }
    lstrcpynW( tz->id, tz->info.TimeZoneKeyName, ARRAY_SIZE(tz->id) );
    for (i = 0; i < ARRAY_SIZE(zone_names); i++)
    {
        if (wcscmp( zone_names[i].windows, tz->info.TimeZoneKeyName )) continue;
        tz->names = &zone_names[i];
        wcscpy( tz->id, zone_names[i].iana );
        break;
    }
}

/* the Windows time zone of an IANA name (or UTC); FALSE when there is none */
static BOOL time_zone_from_id( struct time_zone *tz, const WCHAR *id )
{
    DYNAMIC_TIME_ZONE_INFORMATION info;
    const struct zone_name *names = NULL;
    unsigned int i;
    DWORD index;

    if (!wcscmp( id, L"UTC" ) || !wcscmp( id, L"Etc/UTC" ) || !wcscmp( id, L"Etc/GMT" ) || !wcscmp( id, L"GMT" ) ||
        !wcscmp( id, L"Etc/Universal" ) || !wcscmp( id, L"Etc/Zulu" ))
    {
        time_zone_utc( tz );
        return TRUE;
    }
    for (i = 0; i < ARRAY_SIZE(zone_names); i++)
        if (!wcscmp( zone_names[i].iana, id )) names = &zone_names[i];
    if (!names) return FALSE;
    for (index = 0; !EnumDynamicTimeZoneInformation( index, &info ); index++)
    {
        if (wcscmp( info.TimeZoneKeyName, names->windows )) continue;
        memset( tz, 0, sizeof(*tz) );
        tz->info = info;
        tz->names = names;
        wcscpy( tz->id, names->iana );
        return TRUE;
    }
    FIXME( "time zone %s (%s) not in the registry.\n", debugstr_w(id), debugstr_w(names->windows) );
    return FALSE;
}

/* the time zone's offset from UTC (ticks to add to UTC) at an instant */
static INT64 time_zone_offset( const struct time_zone *tz, INT64 utc )
{
    SYSTEMTIME st, local;
    FILETIME ft, lt;
    INT64 ms;

    if (tz->utc) return 0;
    /* the system's rules apply to the years it can express; before, the standard offset */
    if (utc >= TICKS_PER_DAY * 366 && utc <= MAX_DATETIME)
    {
        ms = utc - utc % 10000;
        ft.dwLowDateTime = (DWORD)ms;
        ft.dwHighDateTime = (DWORD)(ms >> 32);
        if (FileTimeToSystemTime( &ft, &st ) && SystemTimeToTzSpecificLocalTimeEx( &tz->info, &st, &local ) &&
            SystemTimeToFileTime( &local, &lt ))
            return (((INT64)lt.dwHighDateTime << 32) | lt.dwLowDateTime) - ms;
    }
    return -(INT64)(tz->info.Bias + tz->info.StandardBias) * 60 * TICKS_PER_SECOND;
}

static INT64 local_from_utc( const struct time_zone *tz, INT64 utc )
{
    return utc + time_zone_offset( tz, utc );
}

static INT64 utc_from_local( const struct time_zone *tz, INT64 local )
{
    INT64 utc = local - time_zone_offset( tz, local );
    return local - time_zone_offset( tz, utc );
}

static BOOL time_zone_daylight( const struct time_zone *tz, INT64 utc )
{
    if (tz->utc || !tz->info.DaylightDate.wMonth) return FALSE;
    return time_zone_offset( tz, utc ) != -(INT64)(tz->info.Bias + tz->info.StandardBias) * 60 * TICKS_PER_SECOND;
}

/* ---- the calendar ---------------------------------------------------------------------------------------------- */

static const WCHAR *calendar_systems[] =
{
    L"GregorianCalendar", L"HebrewCalendar", L"HijriCalendar", L"JapaneseCalendar", L"JulianCalendar",
    L"KoreanCalendar", L"PersianCalendar", L"TaiwanCalendar", L"ThaiCalendar", L"UmAlQuraCalendar",
    L"ChineseLunarCalendar", L"JapaneseLunarCalendar", L"KoreanLunarCalendar", L"TaiwanLunarCalendar",
    L"VietnameseLunarCalendar",
};

struct calendar
{
    ICalendar ICalendar_iface;
    ITimeZoneOnCalendar ITimeZoneOnCalendar_iface;
    LONG ref;

    INT64 datetime;                         /* UTC */
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];   /* the resolved language */
    HSTRING *languages;
    UINT32 language_count;
    const WCHAR *system;
    BOOL clock12;
    WCHAR numerals[16];
    struct time_zone tz;
};

static inline struct calendar *impl_from_ICalendar( ICalendar *iface )
{
    return CONTAINING_RECORD( iface, struct calendar, ICalendar_iface );
}

static const ICalendarVtbl calendar_vtbl;

static struct calendar *unsafe_impl_from_ICalendar( ICalendar *iface )
{
    if (!iface || iface->lpVtbl != &calendar_vtbl) return NULL;
    return impl_from_ICalendar( iface );
}

static void calendar_local( struct calendar *impl, struct fields *f )
{
    fields_from_ticks( local_from_utc( &impl->tz, impl->datetime ), f );
}

/* a new local date and time: E_INVALIDARG when it falls outside years 1 to 9999 */
static HRESULT calendar_set_local( struct calendar *impl, const struct fields *f )
{
    INT64 utc;
    if (f->year < 1 || f->year > 9999) return E_INVALIDARG;
    utc = utc_from_local( &impl->tz, ticks_from_fields( f ) );
    if (utc < MIN_DATETIME || utc > MAX_DATETIME) return E_INVALIDARG;
    impl->datetime = utc;
    return S_OK;
}

static HRESULT calendar_set_utc( struct calendar *impl, INT64 utc )
{
    if (utc < MIN_DATETIME || utc > MAX_DATETIME) return E_INVALIDARG;
    impl->datetime = utc;
    return S_OK;
}

static HRESULT create_string( const WCHAR *str, HSTRING *out )
{
    return WindowsCreateString( str, wcslen( str ), out );
}

static HRESULT number_string( INT64 value, INT32 min_digits, HSTRING *out )
{
    WCHAR buffer[32];
    if (min_digits > 20) min_digits = 20;
    swprintf( buffer, ARRAY_SIZE(buffer), L"%0*I64d", min_digits < 1 ? 1 : min_digits, value );
    return create_string( buffer, out );
}

static void locale_string( struct calendar *impl, LCTYPE type, WCHAR *buffer, int size )
{
    if (!GetLocaleInfoEx( impl->locale, type, buffer, size ) && !GetLocaleInfoEx( L"en-US", type, buffer, size ))
        buffer[0] = 0;
}

/* the longest form that fits the ideal length, else the shortest one */
static HRESULT choose_string( const WCHAR *full, const WCHAR *abbrev, const WCHAR *shortest, INT32 ideal, HSTRING *out )
{
    const WCHAR *forms[] = { full, abbrev, shortest }, *best = NULL, *least = NULL;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(forms); i++)
    {
        INT32 len;
        if (!forms[i] || !forms[i][0]) continue;
        len = wcslen( forms[i] );
        if (len <= ideal && (!best || len > (INT32)wcslen( best ))) best = forms[i];
        if (!least || len < (INT32)wcslen( least )) least = forms[i];
    }
    return create_string( best ? best : least ? least : L"", out );
}

static void calendar_month_strings( struct calendar *impl, INT32 month, WCHAR *full, WCHAR *abbrev )
{
    locale_string( impl, LOCALE_SMONTHNAME1 + month - 1, full, 80 );
    locale_string( impl, LOCALE_SABBREVMONTHNAME1 + month - 1, abbrev, 80 );
}

/* DayOfWeek: Sunday is 0; the locale's day names begin with Monday */
static void calendar_day_strings( struct calendar *impl, DayOfWeek dow, WCHAR *full, WCHAR *abbrev, WCHAR *shortest )
{
    int index = (dow + 6) % 7;
    locale_string( impl, LOCALE_SDAYNAME1 + index, full, 80 );
    locale_string( impl, LOCALE_SABBREVDAYNAME1 + index, abbrev, 80 );
    locale_string( impl, LOCALE_SSHORTESTDAYNAME1 + index, shortest, 80 );
}

static DayOfWeek calendar_day_of_week( struct calendar *impl )
{
    INT64 local = local_from_utc( &impl->tz, impl->datetime ), days = floor_div( local, TICKS_PER_DAY );
    /* 1601-01-01 was a Monday */
    return (DayOfWeek)((days % 7 + 7 + 1) % 7);
}

static INT32 calendar_periods( struct calendar *impl )
{
    return impl->clock12 ? 2 : 1;
}

/* ---- IUnknown, IInspectable ---- */

static HRESULT WINAPI calendar_QueryInterface( ICalendar *iface, REFIID iid, void **out )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICalendar ))
    {
        *out = &impl->ICalendar_iface;
        ICalendar_AddRef( iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_ITimeZoneOnCalendar ))
    {
        *out = &impl->ITimeZoneOnCalendar_iface;
        ICalendar_AddRef( iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI calendar_AddRef( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI calendar_Release( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedDecrement( &impl->ref ), i;

    TRACE( "iface %p, ref %lu.\n", iface, ref );
    if (!ref)
    {
        for (i = 0; i < impl->language_count; i++) WindowsDeleteString( impl->languages[i] );
        free( impl->languages );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI calendar_GetIids( ICalendar *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI calendar_GetRuntimeClassName( ICalendar *iface, HSTRING *class_name )
{
    return create_string( RuntimeClass_Windows_Globalization_Calendar, class_name );
}

static HRESULT WINAPI calendar_GetTrustLevel( ICalendar *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* ---- creation, copies ---- */

static HRESULT calendar_create( HSTRING *languages, UINT32 count, const WCHAR *system, int clock12,
                                const struct time_zone *tz, ICalendar **out );

static HRESULT copy_languages( struct calendar *impl, HSTRING **out )
{
    HSTRING *copy;
    UINT32 i;

    if (!(copy = calloc( impl->language_count ? impl->language_count : 1, sizeof(*copy) ))) return E_OUTOFMEMORY;
    for (i = 0; i < impl->language_count; i++) WindowsDuplicateString( impl->languages[i], &copy[i] );
    *out = copy;
    return S_OK;
}

static HRESULT WINAPI calendar_Clone( ICalendar *iface, ICalendar **value )
{
    struct calendar *impl = impl_from_ICalendar( iface ), *clone;
    HSTRING *languages;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (FAILED(hr = copy_languages( impl, &languages ))) return hr;
    if (FAILED(hr = calendar_create( languages, impl->language_count, impl->system, impl->clock12, &impl->tz, value )))
    {
        free( languages );
        return hr;
    }
    clone = impl_from_ICalendar( *value );
    clone->datetime = impl->datetime;
    wcscpy( clone->numerals, impl->numerals );
    return S_OK;
}

static HRESULT WINAPI calendar_SetToMin( ICalendar *iface )
{
    impl_from_ICalendar( iface )->datetime = MIN_DATETIME;
    return S_OK;
}

static HRESULT WINAPI calendar_SetToMax( ICalendar *iface )
{
    impl_from_ICalendar( iface )->datetime = MAX_DATETIME;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Languages( ICalendar *iface, IVectorView_HSTRING **value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    HSTRING *languages;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (FAILED(hr = copy_languages( impl, &languages ))) return hr;
    hr = hstring_vector_create( languages, impl->language_count, value );
    free( languages );
    return hr;
}

static HRESULT WINAPI calendar_get_NumeralSystem( ICalendar *iface, HSTRING *value )
{
    return create_string( impl_from_ICalendar( iface )->numerals, value );
}

static HRESULT WINAPI calendar_put_NumeralSystem( ICalendar *iface, HSTRING value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    const WCHAR *name = WindowsGetStringRawBuffer( value, NULL );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (!name[0] || wcslen( name ) >= ARRAY_SIZE(impl->numerals)) return E_INVALIDARG;
    if (wcscmp( name, L"Latn" )) FIXME( "numeral system %s: Latin digits are used.\n", debugstr_w(name) );
    wcscpy( impl->numerals, name );
    return S_OK;
}

static HRESULT WINAPI calendar_GetCalendarSystem( ICalendar *iface, HSTRING *value )
{
    return create_string( impl_from_ICalendar( iface )->system, value );
}

static const WCHAR *find_calendar_system( const WCHAR *name )
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(calendar_systems); i++)
        if (!wcscmp( calendar_systems[i], name )) return calendar_systems[i];
    return NULL;
}

static HRESULT WINAPI calendar_ChangeCalendarSystem( ICalendar *iface, HSTRING value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    const WCHAR *system = find_calendar_system( WindowsGetStringRawBuffer( value, NULL ) );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (!system) return E_INVALIDARG;
    if (system != calendar_systems[0]) FIXME( "calendar system %s: computed as Gregorian.\n", debugstr_w(system) );
    impl->system = system;
    return S_OK;
}

static HRESULT WINAPI calendar_GetClock( ICalendar *iface, HSTRING *value )
{
    return create_string( impl_from_ICalendar( iface )->clock12 ? L"12HourClock" : L"24HourClock", value );
}

static int clock_from_name( const WCHAR *name )
{
    if (!wcscmp( name, L"12HourClock" )) return 1;
    if (!wcscmp( name, L"24HourClock" )) return 0;
    return -1;
}

static HRESULT WINAPI calendar_ChangeClock( ICalendar *iface, HSTRING value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    int clock12 = clock_from_name( WindowsGetStringRawBuffer( value, NULL ) );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (clock12 < 0) return E_INVALIDARG;
    impl->clock12 = clock12;
    return S_OK;
}

static HRESULT WINAPI calendar_GetDateTime( ICalendar *iface, DateTime *result )
{
    result->UniversalTime = impl_from_ICalendar( iface )->datetime;
    return S_OK;
}

static HRESULT WINAPI calendar_SetDateTime( ICalendar *iface, DateTime value )
{
    TRACE( "iface %p, value %I64d.\n", iface, value.UniversalTime );
    return calendar_set_utc( impl_from_ICalendar( iface ), value.UniversalTime );
}

static HRESULT WINAPI calendar_SetToNow( ICalendar *iface )
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime( &ft );
    impl_from_ICalendar( iface )->datetime = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return S_OK;
}

/* ---- eras ---- */

#define CALENDAR_CONSTANT( name, expr ) \
    static HRESULT WINAPI calendar_get_##name( ICalendar *iface, INT32 *value ) \
    { \
        struct calendar *impl = impl_from_ICalendar( iface ); \
        struct fields f; \
        calendar_local( impl, &f ); \
        (void)f; \
        *value = (expr); \
        return S_OK; \
    }

CALENDAR_CONSTANT( FirstEra, 1 )
CALENDAR_CONSTANT( LastEra, 1 )
CALENDAR_CONSTANT( NumberOfEras, 1 )
CALENDAR_CONSTANT( Era, 1 )

static HRESULT WINAPI calendar_put_Era( ICalendar *iface, INT32 value )
{
    return value == 1 ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI calendar_AddEras( ICalendar *iface, INT32 eras )
{
    return eras ? E_INVALIDARG : S_OK;
}

static HRESULT era_string( struct calendar *impl, INT32 ideal, HSTRING *result )
{
    WCHAR full[80], abbrev[80];
    if (!GetCalendarInfoEx( impl->locale, CAL_GREGORIAN, NULL, CAL_SERASTRING, full, ARRAY_SIZE(full), NULL ))
        wcscpy( full, L"A.D." );
    if (!GetCalendarInfoEx( impl->locale, CAL_GREGORIAN, NULL, CAL_SABBREVERASTRING, abbrev, ARRAY_SIZE(abbrev), NULL ))
        wcscpy( abbrev, full );
    return choose_string( full, abbrev, NULL, ideal, result );
}

static HRESULT WINAPI calendar_EraAsFullString( ICalendar *iface, HSTRING *result )
{
    return era_string( impl_from_ICalendar( iface ), INT_MAX, result );
}

static HRESULT WINAPI calendar_EraAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    return era_string( impl_from_ICalendar( iface ), ideal_length, result );
}

CALENDAR_CONSTANT( FirstYearInThisEra, 1 )
CALENDAR_CONSTANT( LastYearInThisEra, 9999 )
CALENDAR_CONSTANT( NumberOfYearsInThisEra, 9999 )

/* ---- years ---- */

CALENDAR_CONSTANT( Year, f.year )

static HRESULT add_months( struct calendar *impl, INT64 months )
{
    struct fields f;
    INT64 total;
    calendar_local( impl, &f );
    total = (INT64)f.year * 12 + (f.month - 1) + months;
    if (total < 12 || total >= (INT64)10000 * 12) return E_INVALIDARG;
    f.year = total / 12;
    f.month = total % 12 + 1;
    if (f.day > days_in_month( f.year, f.month )) f.day = days_in_month( f.year, f.month );
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_put_Year( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > 9999) return E_INVALIDARG;
    calendar_local( impl, &f );
    f.year = value;
    if (f.day > days_in_month( f.year, f.month )) f.day = days_in_month( f.year, f.month );
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddYears( ICalendar *iface, INT32 years )
{
    return add_months( impl_from_ICalendar( iface ), (INT64)years * 12 );
}

static HRESULT WINAPI calendar_YearAsString( ICalendar *iface, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.year, 1, result );
}

static HRESULT WINAPI calendar_YearAsTruncatedString( ICalendar *iface, INT32 remaining_digits, HSTRING *result )
{
    INT64 modulus = 1;
    struct fields f;
    INT32 i;

    calendar_local( impl_from_ICalendar( iface ), &f );
    if (remaining_digits < 1) return E_INVALIDARG;
    for (i = 0; i < remaining_digits && i < 18; i++) modulus *= 10;
    return number_string( f.year % modulus, 1, result );
}

static HRESULT WINAPI calendar_YearAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.year, min_digits, result );
}

/* ---- months ---- */

CALENDAR_CONSTANT( FirstMonthInThisYear, 1 )
CALENDAR_CONSTANT( LastMonthInThisYear, 12 )
CALENDAR_CONSTANT( NumberOfMonthsInThisYear, 12 )
CALENDAR_CONSTANT( Month, f.month )

static HRESULT WINAPI calendar_put_Month( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > 12) return E_INVALIDARG;
    calendar_local( impl, &f );
    f.month = value;
    if (f.day > days_in_month( f.year, f.month )) f.day = days_in_month( f.year, f.month );
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddMonths( ICalendar *iface, INT32 months )
{
    return add_months( impl_from_ICalendar( iface ), months );
}

static HRESULT month_string( ICalendar *iface, INT32 ideal, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    WCHAR full[80], abbrev[80];
    struct fields f;

    calendar_local( impl, &f );
    calendar_month_strings( impl, f.month, full, abbrev );
    return choose_string( full, abbrev, NULL, ideal, result );
}

static HRESULT WINAPI calendar_MonthAsFullString( ICalendar *iface, HSTRING *result )
{
    return month_string( iface, INT_MAX, result );
}

static HRESULT WINAPI calendar_MonthAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    return month_string( iface, ideal_length, result );
}

static HRESULT WINAPI calendar_MonthAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    return month_string( iface, INT_MAX, result );
}

static HRESULT WINAPI calendar_MonthAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    return month_string( iface, ideal_length, result );
}

static HRESULT WINAPI calendar_MonthAsNumericString( ICalendar *iface, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.month, 1, result );
}

static HRESULT WINAPI calendar_MonthAsPaddedNumericString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.month, min_digits, result );
}

/* ---- weeks, days ---- */

static HRESULT add_days( struct calendar *impl, INT64 days )
{
    struct fields f;
    INT64 local;

    calendar_local( impl, &f );
    local = ticks_from_fields( &f ) + days * TICKS_PER_DAY;
    fields_from_ticks( local, &f );
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddWeeks( ICalendar *iface, INT32 weeks )
{
    return add_days( impl_from_ICalendar( iface ), (INT64)weeks * 7 );
}

CALENDAR_CONSTANT( FirstDayInThisMonth, 1 )
CALENDAR_CONSTANT( LastDayInThisMonth, days_in_month( f.year, f.month ) )
CALENDAR_CONSTANT( NumberOfDaysInThisMonth, days_in_month( f.year, f.month ) )
CALENDAR_CONSTANT( Day, f.day )

static HRESULT WINAPI calendar_put_Day( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    calendar_local( impl, &f );
    if (value < 1 || value > days_in_month( f.year, f.month )) return E_INVALIDARG;
    f.day = value;
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddDays( ICalendar *iface, INT32 days )
{
    return add_days( impl_from_ICalendar( iface ), days );
}

static HRESULT WINAPI calendar_DayAsString( ICalendar *iface, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.day, 1, result );
}

static HRESULT WINAPI calendar_DayAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.day, min_digits, result );
}

static HRESULT WINAPI calendar_get_DayOfWeek( ICalendar *iface, DayOfWeek *value )
{
    *value = calendar_day_of_week( impl_from_ICalendar( iface ) );
    return S_OK;
}

static HRESULT day_of_week_string( ICalendar *iface, INT32 ideal, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    WCHAR full[80], abbrev[80], shortest[80];

    calendar_day_strings( impl, calendar_day_of_week( impl ), full, abbrev, shortest );
    return choose_string( full, abbrev, shortest, ideal, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsFullString( ICalendar *iface, HSTRING *result )
{
    return day_of_week_string( iface, INT_MAX, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    return day_of_week_string( iface, ideal_length, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    return day_of_week_string( iface, INT_MAX, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    return day_of_week_string( iface, ideal_length, result );
}

/* ---- periods (AM and PM on a 12-hour clock; one period on a 24-hour clock) ---- */

CALENDAR_CONSTANT( FirstPeriodInThisDay, 1 )
CALENDAR_CONSTANT( LastPeriodInThisDay, calendar_periods( impl ) )
CALENDAR_CONSTANT( NumberOfPeriodsInThisDay, calendar_periods( impl ) )
CALENDAR_CONSTANT( Period, impl->clock12 && f.hour >= 12 ? 2 : 1 )

static HRESULT WINAPI calendar_put_Period( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > calendar_periods( impl )) return E_INVALIDARG;
    if (!impl->clock12) return S_OK;
    calendar_local( impl, &f );
    f.hour = f.hour % 12 + (value == 2 ? 12 : 0);
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddPeriods( ICalendar *iface, INT32 periods )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;
    INT64 local;

    if (!impl->clock12) return add_days( impl, periods );
    calendar_local( impl, &f );
    local = ticks_from_fields( &f ) + (INT64)periods * 12 * 3600 * TICKS_PER_SECOND;
    fields_from_ticks( local, &f );
    return calendar_set_local( impl, &f );
}

static HRESULT period_string( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    WCHAR buffer[80];
    struct fields f;

    if (!impl->clock12) return create_string( L"", result );
    calendar_local( impl, &f );
    locale_string( impl, f.hour >= 12 ? LOCALE_S2359 : LOCALE_S1159, buffer, ARRAY_SIZE(buffer) );
    return create_string( buffer, result );
}

static HRESULT WINAPI calendar_PeriodAsFullString( ICalendar *iface, HSTRING *result )
{
    return period_string( iface, result );
}

static HRESULT WINAPI calendar_PeriodAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    return period_string( iface, result );
}

/* ---- hours ---- */

CALENDAR_CONSTANT( FirstHourInThisPeriod, impl->clock12 ? 12 : 0 )
CALENDAR_CONSTANT( LastHourInThisPeriod, impl->clock12 ? 11 : 23 )
CALENDAR_CONSTANT( NumberOfHoursInThisPeriod, impl->clock12 ? 12 : 24 )
CALENDAR_CONSTANT( Hour, impl->clock12 ? (f.hour % 12 ? f.hour % 12 : 12) : f.hour )

static HRESULT WINAPI calendar_put_Hour( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    calendar_local( impl, &f );
    if (impl->clock12)
    {
        if (value < 1 || value > 12) return E_INVALIDARG;
        f.hour = value % 12 + (f.hour >= 12 ? 12 : 0);
    }
    else
    {
        if (value < 0 || value > 23) return E_INVALIDARG;
        f.hour = value;
    }
    return calendar_set_local( impl, &f );
}

static HRESULT add_ticks( struct calendar *impl, INT64 ticks )
{
    return calendar_set_utc( impl, impl->datetime + ticks );
}

static HRESULT WINAPI calendar_AddHours( ICalendar *iface, INT32 hours )
{
    return add_ticks( impl_from_ICalendar( iface ), (INT64)hours * 3600 * TICKS_PER_SECOND );
}

static HRESULT WINAPI calendar_HourAsString( ICalendar *iface, HSTRING *result )
{
    INT32 hour;
    calendar_get_Hour( iface, &hour );
    return number_string( hour, 1, result );
}

static HRESULT WINAPI calendar_HourAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    INT32 hour;
    calendar_get_Hour( iface, &hour );
    return number_string( hour, min_digits, result );
}

/* ---- minutes, seconds, nanoseconds ---- */

CALENDAR_CONSTANT( Minute, f.minute )

static HRESULT WINAPI calendar_put_Minute( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    if (value < 0 || value > 59) return E_INVALIDARG;
    calendar_local( impl, &f );
    f.minute = value;
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddMinutes( ICalendar *iface, INT32 minutes )
{
    return add_ticks( impl_from_ICalendar( iface ), (INT64)minutes * 60 * TICKS_PER_SECOND );
}

static HRESULT WINAPI calendar_MinuteAsString( ICalendar *iface, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.minute, 1, result );
}

static HRESULT WINAPI calendar_MinuteAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.minute, min_digits, result );
}

CALENDAR_CONSTANT( Second, f.second )

static HRESULT WINAPI calendar_put_Second( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    if (value < 0 || value > 59) return E_INVALIDARG;
    calendar_local( impl, &f );
    f.second = value;
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddSeconds( ICalendar *iface, INT32 seconds )
{
    return add_ticks( impl_from_ICalendar( iface ), (INT64)seconds * TICKS_PER_SECOND );
}

static HRESULT WINAPI calendar_SecondAsString( ICalendar *iface, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.second, 1, result );
}

static HRESULT WINAPI calendar_SecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct fields f;
    calendar_local( impl_from_ICalendar( iface ), &f );
    return number_string( f.second, min_digits, result );
}

/* Windows.Foundation.DateTime keeps 100 ns: smaller parts are dropped */
CALENDAR_CONSTANT( Nanosecond, (INT32)(f.ticks * 100) )

static HRESULT WINAPI calendar_put_Nanosecond( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    if (value < 0 || value > 999999999) return E_INVALIDARG;
    calendar_local( impl, &f );
    f.ticks = value / 100;
    return calendar_set_local( impl, &f );
}

static HRESULT WINAPI calendar_AddNanoseconds( ICalendar *iface, INT32 nanoseconds )
{
    return add_ticks( impl_from_ICalendar( iface ), nanoseconds / 100 );
}

static HRESULT WINAPI calendar_NanosecondAsString( ICalendar *iface, HSTRING *result )
{
    INT32 ns;
    calendar_get_Nanosecond( iface, &ns );
    return number_string( ns, 1, result );
}

static HRESULT WINAPI calendar_NanosecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    INT32 ns;
    calendar_get_Nanosecond( iface, &ns );
    return number_string( ns, min_digits, result );
}

/* ---- comparisons ---- */

static INT32 compare( INT64 a, INT64 b )
{
    return a < b ? -1 : a > b ? 1 : 0;
}

static HRESULT WINAPI calendar_Compare( ICalendar *iface, ICalendar *other, INT32 *result )
{
    DateTime when;
    HRESULT hr;

    if (!other) return E_POINTER;
    if (FAILED(hr = ICalendar_GetDateTime( other, &when ))) return hr;
    *result = compare( impl_from_ICalendar( iface )->datetime, when.UniversalTime );
    return S_OK;
}

static HRESULT WINAPI calendar_CompareDateTime( ICalendar *iface, DateTime other, INT32 *result )
{
    *result = compare( impl_from_ICalendar( iface )->datetime, other.UniversalTime );
    return S_OK;
}

static HRESULT WINAPI calendar_CopyTo( ICalendar *iface, ICalendar *other )
{
    struct calendar *impl = impl_from_ICalendar( iface ), *target = unsafe_impl_from_ICalendar( other );
    HSTRING *languages;
    UINT32 i;
    HRESULT hr;

    TRACE( "iface %p, other %p.\n", iface, other );

    if (!target) return E_INVALIDARG;
    if (target == impl) return S_OK;
    if (FAILED(hr = copy_languages( impl, &languages ))) return hr;
    for (i = 0; i < target->language_count; i++) WindowsDeleteString( target->languages[i] );
    free( target->languages );
    target->languages = languages;
    target->language_count = impl->language_count;
    target->datetime = impl->datetime;
    wcscpy( target->locale, impl->locale );
    target->system = impl->system;
    target->clock12 = impl->clock12;
    wcscpy( target->numerals, impl->numerals );
    target->tz = impl->tz;
    return S_OK;
}

CALENDAR_CONSTANT( FirstMinuteInThisHour, 0 )
CALENDAR_CONSTANT( LastMinuteInThisHour, 59 )
CALENDAR_CONSTANT( NumberOfMinutesInThisHour, 60 )
CALENDAR_CONSTANT( FirstSecondInThisMinute, 0 )
CALENDAR_CONSTANT( LastSecondInThisMinute, 59 )
CALENDAR_CONSTANT( NumberOfSecondsInThisMinute, 60 )

static HRESULT WINAPI calendar_get_ResolvedLanguage( ICalendar *iface, HSTRING *value )
{
    return create_string( impl_from_ICalendar( iface )->locale, value );
}

static HRESULT WINAPI calendar_get_IsDaylightSavingTime( ICalendar *iface, boolean *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    *value = time_zone_daylight( &impl->tz, impl->datetime );
    return S_OK;
}

static const ICalendarVtbl calendar_vtbl =
{
    calendar_QueryInterface,
    calendar_AddRef,
    calendar_Release,
    /* IInspectable methods */
    calendar_GetIids,
    calendar_GetRuntimeClassName,
    calendar_GetTrustLevel,
    /* ICalendar methods */
    calendar_Clone,
    calendar_SetToMin,
    calendar_SetToMax,
    calendar_get_Languages,
    calendar_get_NumeralSystem,
    calendar_put_NumeralSystem,
    calendar_GetCalendarSystem,
    calendar_ChangeCalendarSystem,
    calendar_GetClock,
    calendar_ChangeClock,
    calendar_GetDateTime,
    calendar_SetDateTime,
    calendar_SetToNow,
    calendar_get_FirstEra,
    calendar_get_LastEra,
    calendar_get_NumberOfEras,
    calendar_get_Era,
    calendar_put_Era,
    calendar_AddEras,
    calendar_EraAsFullString,
    calendar_EraAsString,
    calendar_get_FirstYearInThisEra,
    calendar_get_LastYearInThisEra,
    calendar_get_NumberOfYearsInThisEra,
    calendar_get_Year,
    calendar_put_Year,
    calendar_AddYears,
    calendar_YearAsString,
    calendar_YearAsTruncatedString,
    calendar_YearAsPaddedString,
    calendar_get_FirstMonthInThisYear,
    calendar_get_LastMonthInThisYear,
    calendar_get_NumberOfMonthsInThisYear,
    calendar_get_Month,
    calendar_put_Month,
    calendar_AddMonths,
    calendar_MonthAsFullString,
    calendar_MonthAsString,
    calendar_MonthAsFullSoloString,
    calendar_MonthAsSoloString,
    calendar_MonthAsNumericString,
    calendar_MonthAsPaddedNumericString,
    calendar_AddWeeks,
    calendar_get_FirstDayInThisMonth,
    calendar_get_LastDayInThisMonth,
    calendar_get_NumberOfDaysInThisMonth,
    calendar_get_Day,
    calendar_put_Day,
    calendar_AddDays,
    calendar_DayAsString,
    calendar_DayAsPaddedString,
    calendar_get_DayOfWeek,
    calendar_DayOfWeekAsFullString,
    calendar_DayOfWeekAsString,
    calendar_DayOfWeekAsFullSoloString,
    calendar_DayOfWeekAsSoloString,
    calendar_get_FirstPeriodInThisDay,
    calendar_get_LastPeriodInThisDay,
    calendar_get_NumberOfPeriodsInThisDay,
    calendar_get_Period,
    calendar_put_Period,
    calendar_AddPeriods,
    calendar_PeriodAsFullString,
    calendar_PeriodAsString,
    calendar_get_FirstHourInThisPeriod,
    calendar_get_LastHourInThisPeriod,
    calendar_get_NumberOfHoursInThisPeriod,
    calendar_get_Hour,
    calendar_put_Hour,
    calendar_AddHours,
    calendar_HourAsString,
    calendar_HourAsPaddedString,
    calendar_get_Minute,
    calendar_put_Minute,
    calendar_AddMinutes,
    calendar_MinuteAsString,
    calendar_MinuteAsPaddedString,
    calendar_get_Second,
    calendar_put_Second,
    calendar_AddSeconds,
    calendar_SecondAsString,
    calendar_SecondAsPaddedString,
    calendar_get_Nanosecond,
    calendar_put_Nanosecond,
    calendar_AddNanoseconds,
    calendar_NanosecondAsString,
    calendar_NanosecondAsPaddedString,
    calendar_Compare,
    calendar_CompareDateTime,
    calendar_CopyTo,
    calendar_get_FirstMinuteInThisHour,
    calendar_get_LastMinuteInThisHour,
    calendar_get_NumberOfMinutesInThisHour,
    calendar_get_FirstSecondInThisMinute,
    calendar_get_LastSecondInThisMinute,
    calendar_get_NumberOfSecondsInThisMinute,
    calendar_get_ResolvedLanguage,
    calendar_get_IsDaylightSavingTime,
};

/* ---- ITimeZoneOnCalendar ---- */

DEFINE_IINSPECTABLE( time_zone_on_calendar, ITimeZoneOnCalendar, struct calendar, ICalendar_iface )

static HRESULT WINAPI time_zone_on_calendar_GetTimeZone( ITimeZoneOnCalendar *iface, HSTRING *value )
{
    return create_string( impl_from_ITimeZoneOnCalendar( iface )->tz.id, value );
}

static HRESULT WINAPI time_zone_on_calendar_ChangeTimeZone( ITimeZoneOnCalendar *iface, HSTRING time_zone_id )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );
    struct time_zone tz;

    TRACE( "iface %p, time_zone_id %s.\n", iface, debugstr_hstring( time_zone_id ) );

    if (!time_zone_from_id( &tz, WindowsGetStringRawBuffer( time_zone_id, NULL ) )) return E_INVALIDARG;
    impl->tz = tz;
    return S_OK;
}

static HRESULT time_zone_string( struct calendar *impl, BOOL full, HSTRING *result )
{
    const struct time_zone *tz = &impl->tz;
    BOOL daylight = time_zone_daylight( tz, impl->datetime );
    WCHAR buffer[64];
    INT64 minutes;

    if (tz->utc) return create_string( full ? L"Coordinated Universal Time" : L"UTC", result );
    if (full) return create_string( daylight ? tz->info.DaylightName : tz->info.StandardName, result );
    if (tz->names && tz->names->standard_abbrev)
        return create_string( daylight ? tz->names->daylight_abbrev : tz->names->standard_abbrev, result );
    /* GMT and the offset, as ICU writes zones without an abbreviation */
    minutes = time_zone_offset( tz, impl->datetime ) / (60 * TICKS_PER_SECOND);
    if (minutes % 60)
        swprintf( buffer, ARRAY_SIZE(buffer), L"GMT%c%I64d:%02I64d", minutes < 0 ? '-' : '+', llabs( minutes ) / 60, llabs( minutes ) % 60 );
    else
        swprintf( buffer, ARRAY_SIZE(buffer), L"GMT%c%I64d", minutes < 0 ? '-' : '+', llabs( minutes ) / 60 );
    return create_string( minutes ? buffer : L"GMT", result );
}

static HRESULT WINAPI time_zone_on_calendar_TimeZoneAsFullString( ITimeZoneOnCalendar *iface, HSTRING *result )
{
    return time_zone_string( impl_from_ITimeZoneOnCalendar( iface ), TRUE, result );
}

static HRESULT WINAPI time_zone_on_calendar_TimeZoneAsString( ITimeZoneOnCalendar *iface, INT32 ideal_length, HSTRING *result )
{
    return time_zone_string( impl_from_ITimeZoneOnCalendar( iface ), FALSE, result );
}

static const ITimeZoneOnCalendarVtbl time_zone_on_calendar_vtbl =
{
    time_zone_on_calendar_QueryInterface,
    time_zone_on_calendar_AddRef,
    time_zone_on_calendar_Release,
    /* IInspectable methods */
    time_zone_on_calendar_GetIids,
    time_zone_on_calendar_GetRuntimeClassName,
    time_zone_on_calendar_GetTrustLevel,
    /* ITimeZoneOnCalendar methods */
    time_zone_on_calendar_GetTimeZone,
    time_zone_on_calendar_ChangeTimeZone,
    time_zone_on_calendar_TimeZoneAsFullString,
    time_zone_on_calendar_TimeZoneAsString,
};

/* ---- creation ---- */

/* takes the languages (an array of count strings) */
static HRESULT calendar_create( HSTRING *languages, UINT32 count, const WCHAR *system, int clock12,
                                const struct time_zone *tz, ICalendar **out )
{
    struct calendar *impl;
    WCHAR format[80];
    UINT32 i;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICalendar_iface.lpVtbl = &calendar_vtbl;
    impl->ITimeZoneOnCalendar_iface.lpVtbl = &time_zone_on_calendar_vtbl;
    impl->ref = 1;
    impl->languages = languages;
    impl->language_count = count;

    /* the first language Windows knows resolves the calendar's texts */
    for (i = 0; i < count; i++)
    {
        const WCHAR *name = WindowsGetStringRawBuffer( languages[i], NULL );
        if (IsValidLocaleName( name ))
        {
            lstrcpynW( impl->locale, name, ARRAY_SIZE(impl->locale) );
            break;
        }
    }
    if (!impl->locale[0] && !GetUserDefaultLocaleName( impl->locale, ARRAY_SIZE(impl->locale) ))
        wcscpy( impl->locale, L"en-US" );

    impl->system = system ? system : calendar_systems[0];
    if (clock12 < 0)
    {
        /* the language's time format: H is a 24-hour clock */
        if (!GetLocaleInfoEx( impl->locale, LOCALE_STIMEFORMAT, format, ARRAY_SIZE(format) )) format[0] = 0;
        clock12 = !wcschr( format, 'H' );
    }
    impl->clock12 = clock12;
    wcscpy( impl->numerals, L"Latn" );
    if (tz) impl->tz = *tz;
    else time_zone_local( &impl->tz );
    calendar_SetToNow( &impl->ICalendar_iface );

    *out = &impl->ICalendar_iface;
    TRACE( "created %p, locale %s, system %s, clock12 %d, time zone %s.\n", *out, debugstr_w(impl->locale),
           debugstr_w(impl->system), clock12, debugstr_w(impl->tz.id) );
    return S_OK;
}

/* the user's language, for a calendar made without languages */
static HRESULT default_languages( HSTRING **out, UINT32 *count )
{
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    HRESULT hr;

    if (!GetUserDefaultLocaleName( locale, ARRAY_SIZE(locale) )) wcscpy( locale, L"en-US" );
    if (!(*out = calloc( 1, sizeof(**out) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = create_string( locale, *out )))
    {
        free( *out );
        return hr;
    }
    *count = 1;
    return S_OK;
}

HRESULT languages_from_iterable( IIterable_HSTRING *iterable, HSTRING **out, UINT32 *count )
{
    IIterator_HSTRING *iterator;
    HSTRING *list = NULL, *grown;
    boolean more = FALSE;
    UINT32 n = 0, size = 0;
    HRESULT hr;

    if (!iterable) return E_INVALIDARG;
    if (FAILED(hr = IIterable_HSTRING_First( iterable, &iterator ))) return hr;
    hr = IIterator_HSTRING_get_HasCurrent( iterator, &more );
    while (SUCCEEDED(hr) && more)
    {
        if (n == size)
        {
            size = size ? size * 2 : 4;
            if (!(grown = realloc( list, size * sizeof(*list) )))
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            list = grown;
        }
        if (FAILED(hr = IIterator_HSTRING_get_Current( iterator, &list[n] ))) break;
        n++;
        hr = IIterator_HSTRING_MoveNext( iterator, &more );
    }
    IIterator_HSTRING_Release( iterator );
    if (SUCCEEDED(hr) && !n) hr = E_INVALIDARG;
    if (FAILED(hr))
    {
        while (n) WindowsDeleteString( list[--n] );
        free( list );
        return hr;
    }
    *out = list;
    *count = n;
    return S_OK;
}

static HRESULT create_with( IIterable_HSTRING *languages, HSTRING calendar, HSTRING clock, HSTRING time_zone,
                            ICalendar **result )
{
    const WCHAR *system = NULL;
    struct time_zone tz;
    HSTRING *list;
    UINT32 count;
    int clock12 = -1;
    HRESULT hr;

    if (!result) return E_POINTER;
    if (calendar && !(system = find_calendar_system( WindowsGetStringRawBuffer( calendar, NULL ) ))) return E_INVALIDARG;
    if (clock && (clock12 = clock_from_name( WindowsGetStringRawBuffer( clock, NULL ) )) < 0) return E_INVALIDARG;
    if (time_zone && !time_zone_from_id( &tz, WindowsGetStringRawBuffer( time_zone, NULL ) )) return E_INVALIDARG;
    if (system && system != calendar_systems[0]) FIXME( "calendar system %s: computed as Gregorian.\n", debugstr_w(system) );
    if (FAILED(hr = languages_from_iterable( languages, &list, &count ))) return hr;
    if (FAILED(hr = calendar_create( list, count, system, clock12, time_zone ? &tz : NULL, result )))
    {
        while (count) WindowsDeleteString( list[--count] );
        free( list );
    }
    return hr;
}

struct calendar_factory
{
    IActivationFactory IActivationFactory_iface;
    ICalendarFactory ICalendarFactory_iface;
    ICalendarFactory2 ICalendarFactory2_iface;
    LONG ref;
};

static inline struct calendar_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct calendar_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ICalendarFactory ))
        *out = &impl->ICalendarFactory_iface;
    else if (IsEqualGUID( iid, &IID_ICalendarFactory2 ))
        *out = &impl->ICalendarFactory2_iface;
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
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    HSTRING *languages;
    UINT32 count;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (FAILED(hr = default_languages( &languages, &count ))) return hr;
    if (FAILED(hr = calendar_create( languages, count, NULL, -1, NULL, (ICalendar **)instance )))
    {
        WindowsDeleteString( languages[0] );
        free( languages );
    }
    return hr;
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

DEFINE_IINSPECTABLE( calendar_factory1, ICalendarFactory, struct calendar_factory, IActivationFactory_iface )

static HRESULT WINAPI calendar_factory1_CreateCalendarDefaultCalendarAndClock( ICalendarFactory *iface,
        IIterable_HSTRING *languages, ICalendar **result )
{
    TRACE( "iface %p, languages %p, result %p.\n", iface, languages, result );
    return create_with( languages, NULL, NULL, NULL, result );
}

static HRESULT WINAPI calendar_factory1_CreateCalendar( ICalendarFactory *iface, IIterable_HSTRING *languages,
        HSTRING calendar, HSTRING clock, ICalendar **result )
{
    TRACE( "iface %p, languages %p, calendar %s, clock %s, result %p.\n", iface, languages,
           debugstr_hstring( calendar ), debugstr_hstring( clock ), result );
    if (!calendar || !clock) return E_INVALIDARG;
    return create_with( languages, calendar, clock, NULL, result );
}

static const ICalendarFactoryVtbl calendar_factory1_vtbl =
{
    calendar_factory1_QueryInterface,
    calendar_factory1_AddRef,
    calendar_factory1_Release,
    /* IInspectable methods */
    calendar_factory1_GetIids,
    calendar_factory1_GetRuntimeClassName,
    calendar_factory1_GetTrustLevel,
    /* ICalendarFactory methods */
    calendar_factory1_CreateCalendarDefaultCalendarAndClock,
    calendar_factory1_CreateCalendar,
};

DEFINE_IINSPECTABLE( calendar_factory2, ICalendarFactory2, struct calendar_factory, IActivationFactory_iface )

static HRESULT WINAPI calendar_factory2_CreateCalendarWithTimeZone( ICalendarFactory2 *iface,
        IIterable_HSTRING *languages, HSTRING calendar, HSTRING clock, HSTRING time_zone_id, ICalendar **result )
{
    TRACE( "iface %p, languages %p, calendar %s, clock %s, time_zone_id %s, result %p.\n", iface, languages,
           debugstr_hstring( calendar ), debugstr_hstring( clock ), debugstr_hstring( time_zone_id ), result );
    if (!calendar || !clock || !time_zone_id) return E_INVALIDARG;
    return create_with( languages, calendar, clock, time_zone_id, result );
}

static const ICalendarFactory2Vtbl calendar_factory2_vtbl =
{
    calendar_factory2_QueryInterface,
    calendar_factory2_AddRef,
    calendar_factory2_Release,
    /* IInspectable methods */
    calendar_factory2_GetIids,
    calendar_factory2_GetRuntimeClassName,
    calendar_factory2_GetTrustLevel,
    /* ICalendarFactory2 methods */
    calendar_factory2_CreateCalendarWithTimeZone,
};

static struct calendar_factory calendar_factory =
{
    {&factory_vtbl},
    {&calendar_factory1_vtbl},
    {&calendar_factory2_vtbl},
    1,
};

IActivationFactory *calendar_factory_iface = &calendar_factory.IActivationFactory_iface;
