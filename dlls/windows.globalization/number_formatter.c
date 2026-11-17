/* WinRT Windows.Globalization.NumberFormatting.DecimalFormatter implementation
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
 * DecimalFormatter, as Windows 11 formats and parses (tests/globalization.c):
 * - the language is the first of the list the system knows, else the user's; the numeral system is Arab for
 *   Arabic-script languages whose native digits are Arabic-Indic, Latn otherwise; GeographicRegion is the region
 *   given (the user's for the default constructor), ResolvedGeographicRegion is "ZZ";
 * - IntegerDigits 1, FractionDigits 2, not grouped, no decimal point when there is no fraction;
 * - a double keeps its shortest round-trip digits, at least FractionDigits of fraction, never an exponent; an integer
 *   is padded to IntegerDigits; grouping (the locale's) covers the padding; negative zero has no sign unless
 *   IsZeroSigned; the minus sign is '-';
 * - separators are the locale's, or the Arabic ones (U+066B, U+066C) with the Arab and ArabExt numeral systems;
 * - parsing takes optional group separators, no spaces; ParseInt and ParseUInt no fraction, ParseUInt no sign.
 * SignificantDigits pads with zeros up to that many significant digits; a NumberRounder rounds the value first.
 */

#include "private.h"
#include "roapi.h"

#include "initguid.h"
#define WIDL_using_Windows_Globalization_NumberFormatting
#include "windows.globalization.numberformatting.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

WINE_DEFAULT_DEBUG_CHANNEL(locale);

static const struct numeral_system
{
    const WCHAR *name;
    WCHAR zero;
} numeral_systems[] =
{
    { L"Latn", '0' }, { L"Arab", 0x0660 }, { L"ArabExt", 0x06f0 }, { L"Beng", 0x09e6 }, { L"Deva", 0x0966 },
    { L"FullWide", 0xff10 }, { L"Gujr", 0x0ae6 }, { L"Guru", 0x0a66 }, { L"Khmr", 0x17e0 }, { L"Knda", 0x0ce6 },
    { L"Laoo", 0x0ed0 }, { L"Mlym", 0x0d66 }, { L"Mong", 0x1810 }, { L"Mymr", 0x1040 }, { L"Orya", 0x0b66 },
    { L"TamlDec", 0x0be6 }, { L"Telu", 0x0c66 }, { L"Thai", 0x0e50 }, { L"Tibt", 0x0f20 },
};

static const struct numeral_system *find_numeral_system( const WCHAR *name )
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(numeral_systems); i++)
        if (!wcsicmp( numeral_systems[i].name, name )) return &numeral_systems[i];
    return NULL;
}

struct decimal_formatter
{
    INumberFormatter INumberFormatter_iface;
    INumberFormatter2 INumberFormatter2_iface;
    INumberFormatterOptions INumberFormatterOptions_iface;
    INumberParser INumberParser_iface;
    ISignificantDigitsOption ISignificantDigitsOption_iface;
    INumberRounderOption INumberRounderOption_iface;
    ISignedZeroOption ISignedZeroOption_iface;
    ICurrencyFormatter ICurrencyFormatter_iface;
    ICurrencyFormatter2 ICurrencyFormatter2_iface;
    LONG ref;

    /* CurrencyFormatter only */
    BOOL is_currency;
    WCHAR currency[4];
    CurrencyFormatterMode mode;
    RoundingAlgorithm rounding;

    HSTRING *languages;
    UINT32 language_count;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    HSTRING region;
    const struct numeral_system *numerals;
    INT32 integer_digits, fraction_digits, significant_digits;
    boolean grouped, point, zero_signed;
    INumberRounder *rounder;
};

static inline struct decimal_formatter *impl_from_INumberFormatter( INumberFormatter *iface )
{
    return CONTAINING_RECORD( iface, struct decimal_formatter, INumberFormatter_iface );
}

/* ---- the text of a number ---- */

/* the locale's separators and group size, or the Arabic separators for the Arabic-Indic numeral systems */
static void separators( struct decimal_formatter *impl, WCHAR *decimal, WCHAR *group, unsigned int *size )
{
    WCHAR buffer[16];

    *decimal = '.';
    *group = ',';
    *size = 3;
    if (GetLocaleInfoEx( impl->locale, LOCALE_SDECIMAL, buffer, ARRAY_SIZE(buffer) ) && buffer[0]) *decimal = buffer[0];
    if (GetLocaleInfoEx( impl->locale, LOCALE_STHOUSAND, buffer, ARRAY_SIZE(buffer) ) && buffer[0]) *group = buffer[0];
    if (GetLocaleInfoEx( impl->locale, LOCALE_SGROUPING, buffer, ARRAY_SIZE(buffer) ) && buffer[0] >= '1' && buffer[0] <= '9')
        *size = buffer[0] - '0';
    if (impl->numerals->zero == 0x0660 || impl->numerals->zero == 0x06f0)
    {
        *decimal = 0x066b;
        *group = 0x066c;
    }
}

/* ---- currencies ---- */

static const struct currency_info
{
    const WCHAR *code;
    const WCHAR *symbol;
    int digits;
} currencies[] =
{
    { L"USD", L"$", 2 }, { L"EUR", L"\x20ac", 2 }, { L"GBP", L"\xa3", 2 }, { L"JPY", L"\xa5", 0 },
    { L"CNY", L"CN\xa5", 2 }, { L"INR", L"\x20b9", 2 }, { L"KRW", L"\x20a9", 0 }, { L"RUB", L"\x20bd", 2 },
    { L"CAD", L"CA$", 2 }, { L"AUD", L"A$", 2 }, { L"NZD", L"NZ$", 2 }, { L"BRL", L"R$", 2 },
    { L"MXN", L"MX$", 2 }, { L"HKD", L"HK$", 2 }, { L"ILS", L"\x20aa", 2 }, { L"VND", L"\x20ab", 0 },
    { L"TWD", L"NT$", 2 }, { L"PHP", L"\x20b1", 2 }, { L"XAF", L"FCFA", 0 }, { L"XOF", L"F\x202f" L"CFA", 0 },
    { L"CLP", L"CLP", 0 }, { L"ISK", L"ISK", 0 }, { L"UGX", L"UGX", 0 }, { L"PYG", L"PYG", 0 },
    { L"RWF", L"RWF", 0 }, { L"XPF", L"CFPF", 0 },
    { L"BHD", L"BHD", 3 }, { L"KWD", L"KWD", 3 }, { L"OMR", L"OMR", 3 }, { L"JOD", L"JOD", 3 },
    { L"TND", L"TND", 3 }, { L"IQD", L"IQD", 3 }, { L"LYD", L"LYD", 3 },
};

static const struct currency_info *find_currency( const WCHAR *code )
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(currencies); i++)
        if (!wcsicmp( currencies[i].code, code )) return &currencies[i];
    return NULL;
}

static int currency_digits( const WCHAR *code )
{
    const struct currency_info *info = find_currency( code );
    return info ? info->digits : 2;
}

/* the text that stands for the currency: its code, the locale's symbol when the locale's currency is it, else the
 * well-known symbol, else the code */
static void currency_symbol( struct decimal_formatter *impl, WCHAR *symbol, unsigned int size )
{
    const struct currency_info *info;
    WCHAR code[16];

    wcscpy( symbol, impl->currency );
    if (impl->mode == CurrencyFormatterMode_UseCurrencyCode) return;
    if (GetLocaleInfoEx( impl->locale, LOCALE_SINTLSYMBOL, code, ARRAY_SIZE(code) ) && !wcsicmp( code, impl->currency ) &&
        GetLocaleInfoEx( impl->locale, LOCALE_SCURRENCY, code, ARRAY_SIZE(code) ) && code[0])
    {
        wcscpy( symbol, code );
        return;
    }
    if ((info = find_currency( impl->currency ))) wcscpy( symbol, info->symbol );
}

/* the text of a formatted number (optional leading '-') with the currency placed as the locale does */
static HRESULT currency_text( struct decimal_formatter *impl, const WCHAR *number, HSTRING *result )
{
    WCHAR symbol[16], text[256], *p = text;
    DWORD pattern = 0;
    BOOL negative = number[0] == '-', space, suffix;

    if (negative) number++;
    currency_symbol( impl, symbol, ARRAY_SIZE(symbol) );
    GetLocaleInfoEx( impl->locale, LOCALE_ICURRENCY | LOCALE_RETURN_NUMBER, (WCHAR *)&pattern, sizeof(pattern) / sizeof(WCHAR) );
    suffix = pattern == 1 || pattern == 3;
    space = pattern >= 2 || impl->mode == CurrencyFormatterMode_UseCurrencyCode || iswalpha( symbol[wcslen( symbol ) - 1] );
    if (wcslen( number ) + wcslen( symbol ) + 4 > ARRAY_SIZE(text)) return E_FAIL;

    if (negative) *p++ = '-';
    if (!suffix)
    {
        wcscpy( p, symbol );
        p += wcslen( p );
        if (space) *p++ = 0xa0;
    }
    wcscpy( p, number );
    p += wcslen( p );
    if (suffix)
    {
        if (space) *p++ = 0xa0;
        wcscpy( p, symbol );
        p += wcslen( p );
    }
    return WindowsCreateString( text, p - text, result );
}

/* rounds to a number of fraction digits with a RoundingAlgorithm */
static double round_to_digits( double value, RoundingAlgorithm algorithm, int digits )
{
    double scale = pow( 10, digits ), y = value * scale, f = floor( y ), diff = y - f, r;
    BOOL positive = y > 0;

    switch (algorithm)
    {
    case RoundingAlgorithm_RoundDown: r = f; break;
    case RoundingAlgorithm_RoundUp: r = ceil( y ); break;
    case RoundingAlgorithm_RoundTowardsZero: r = trunc( y ); break;
    case RoundingAlgorithm_RoundAwayFromZero: r = positive ? ceil( y ) : f; break;
    case RoundingAlgorithm_RoundHalfDown:
    case RoundingAlgorithm_RoundHalfUp:
    case RoundingAlgorithm_RoundHalfTowardsZero:
    case RoundingAlgorithm_RoundHalfAwayFromZero:
    case RoundingAlgorithm_RoundHalfToEven:
    case RoundingAlgorithm_RoundHalfToOdd:
        if (diff < 0.5) r = f;
        else if (diff > 0.5) r = f + 1;
        else if (algorithm == RoundingAlgorithm_RoundHalfDown) r = f;
        else if (algorithm == RoundingAlgorithm_RoundHalfUp) r = f + 1;
        else if (algorithm == RoundingAlgorithm_RoundHalfTowardsZero) r = positive ? f : f + 1;
        else if (algorithm == RoundingAlgorithm_RoundHalfAwayFromZero) r = positive ? f + 1 : f;
        else if (algorithm == RoundingAlgorithm_RoundHalfToEven) r = fmod( f, 2 ) == 0 ? f : f + 1;
        else r = fmod( f, 2 ) == 0 ? f + 1 : f;
        break;
    default: return value;
    }
    return r / scale;
}

/* the text of a currency amount without its currency: the symbol, code and spaces are removed */
static UINT32 strip_currency( struct decimal_formatter *impl, const WCHAR *text, UINT32 len, WCHAR *out, UINT32 size )
{
    WCHAR symbol[16], locale_symbol[16] = {0};
    UINT32 i, n = 0, l1, l2, l3 = wcslen( impl->currency );

    currency_symbol( impl, symbol, ARRAY_SIZE(symbol) );
    GetLocaleInfoEx( impl->locale, LOCALE_SCURRENCY, locale_symbol, ARRAY_SIZE(locale_symbol) );
    l1 = wcslen( symbol );
    l2 = wcslen( locale_symbol );
    for (i = 0; i < len && n < size - 1; )
    {
        if (l1 && i + l1 <= len && !wcsncmp( text + i, symbol, l1 )) { i += l1; continue; }
        if (l2 && i + l2 <= len && !wcsncmp( text + i, locale_symbol, l2 )) { i += l2; continue; }
        if (i + l3 <= len && !wcsnicmp( text + i, impl->currency, l3 )) { i += l3; continue; }
        if (text[i] == ' ' || text[i] == 0xa0 || text[i] == 0x202f) { i++; continue; }
        out[n++] = text[i++];
    }
    out[n] = 0;
    return n;
}

/* the shortest decimal digits of a double that read back as it: digits and the position of the decimal point
 * (value = 0.digits * 10^point) */
static void shortest_digits( double value, char *digits, int *point )
{
    char buffer[64], *e, *c;
    int precision, exponent, len = 0;

    for (precision = 1; precision <= 17; precision++)
    {
        snprintf( buffer, sizeof(buffer), "%.*e", precision - 1, value );
        if (strtod( buffer, NULL ) == value) break;
    }
    exponent = strtol( (e = strchr( buffer, 'e' )) + 1, NULL, 10 );
    for (c = buffer; c < e; c++) if (*c >= '0' && *c <= '9') digits[len++] = *c;
    while (len > 1 && digits[len - 1] == '0') len--;
    digits[len] = 0;
    *point = exponent + 1;
}

/* the text of a number given as an integer part and fraction digits (ASCII), with the formatter's options */
static HRESULT format_digits( struct decimal_formatter *impl, BOOL negative, const char *integer, const char *fraction,
                              HSTRING *result )
{
    WCHAR decimal, group, *text, *p;
    unsigned int size, i, len, int_len, frac_len, pad, significant = 0, total;
    BOOL zero = TRUE;
    HRESULT hr;

    separators( impl, &decimal, &group, &size );
    while (integer[0] == '0' && integer[1]) integer++;
    int_len = strlen( integer );
    frac_len = strlen( fraction );
    for (i = 0; integer[i]; i++) if (integer[i] != '0') zero = FALSE;
    for (i = 0; fraction[i]; i++) if (fraction[i] != '0') zero = FALSE;

    /* digits shown: the integer part padded to IntegerDigits, the fraction to FractionDigits, then to
     * SignificantDigits significant digits */
    pad = int_len < (unsigned int)impl->integer_digits ? impl->integer_digits - int_len : 0;
    total = frac_len < (unsigned int)impl->fraction_digits ? impl->fraction_digits : frac_len;
    if (impl->significant_digits > 0)
    {
        for (i = 0; integer[i] && integer[i] == '0'; i++) ;
        significant = int_len - i;
        if (!significant) for (i = 0; fraction[i] == '0'; i++) ;
        else i = 0;
        significant += significant ? frac_len : frac_len - i;
        if (significant < (unsigned int)impl->significant_digits) total += impl->significant_digits - significant;
        if (total < frac_len) total = frac_len;
    }

    len = 2 + 2 * (int_len + pad) + total + 2;
    if (!(text = malloc( len * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    p = text;
    if (negative && (!zero || impl->zero_signed)) *p++ = '-';
    for (i = 0; i < int_len + pad; i++)
    {
        unsigned int left = int_len + pad - i;
        char digit = i < pad ? '0' : integer[i - pad];
        *p++ = impl->numerals->zero + (digit - '0');
        if (impl->grouped && size && left > 1 && (left - 1) % size == 0) *p++ = group;
    }
    if (total || impl->point) *p++ = decimal;
    for (i = 0; i < total; i++) *p++ = impl->numerals->zero + ((i < frac_len ? fraction[i] : '0') - '0');
    *p = 0;
    if (impl->is_currency) hr = currency_text( impl, text, result );
    else hr = WindowsCreateString( text, p - text, result );
    free( text );
    return hr;
}

static HRESULT format_int64( struct decimal_formatter *impl, INT64 value, HSTRING *result )
{
    char digits[32];
    UINT64 magnitude = value < 0 ? (UINT64)0 - (UINT64)value : (UINT64)value;

    if (impl->rounder)
    {
        INT64 rounded;
        if (SUCCEEDED(INumberRounder_RoundInt64( impl->rounder, value, &rounded )))
            magnitude = rounded < 0 ? (UINT64)0 - (UINT64)rounded : (UINT64)rounded;
    }
    sprintf( digits, "%I64u", magnitude );
    return format_digits( impl, value < 0, digits, "", result );
}

static HRESULT format_uint64( struct decimal_formatter *impl, UINT64 value, HSTRING *result )
{
    char digits[32];

    if (impl->rounder) INumberRounder_RoundUInt64( impl->rounder, value, &value );
    sprintf( digits, "%I64u", value );
    return format_digits( impl, FALSE, digits, "", result );
}

static HRESULT format_double( struct decimal_formatter *impl, double value, HSTRING *result )
{
    char digits[32], *integer, *fraction;
    int point, len, i;
    HRESULT hr;
    BOOL negative = signbit( value ) != 0;

    if (isnan( value )) return WindowsCreateString( L"NaN", 3, result );
    if (isinf( value )) return WindowsCreateString( negative ? L"-\x221e" : L"\x221e", negative ? 2 : 1, result );
    if (impl->rounder) INumberRounder_RoundDouble( impl->rounder, value, &value );
    if (impl->is_currency && impl->rounding != RoundingAlgorithm_None)
        value = round_to_digits( value, impl->rounding, currency_digits( impl->currency ) );
    if (value == 0)
        return format_digits( impl, negative, "0", "", result );

    shortest_digits( fabs( value ), digits, &point );
    len = strlen( digits );
    if (!(integer = malloc( (point > 0 ? point : 0) + 2 ))) return E_OUTOFMEMORY;
    if (!(fraction = malloc( len + (point < 0 ? -point : 0) + 2 ))) { free( integer ); return E_OUTOFMEMORY; }
    if (point <= 0)
    {
        strcpy( integer, "0" );
        for (i = 0; i < -point; i++) fraction[i] = '0';
        strcpy( fraction + i, digits );
    }
    else
    {
        for (i = 0; i < point; i++) integer[i] = i < len ? digits[i] : '0';
        integer[i] = 0;
        strcpy( fraction, point < len ? digits + point : "" );
    }
    hr = format_digits( impl, negative, integer, fraction, result );
    free( fraction );
    free( integer );
    return hr;
}

/* ---- parsing ---- */

/* the digits of text in the locale's form (optional '-', digits, optional group separators, optional decimal
 * separator and digits) as an ASCII number for strtod/strtoll; FALSE when it is not one */
static BOOL normalize( struct decimal_formatter *impl, const WCHAR *text, UINT32 len, BOOL integer, BOOL sign,
                       char *out, size_t size )
{
    WCHAR decimal, group, locale_decimal = '.', locale_group = ',', buffer[16];
    unsigned int group_size, i, n = 0, digits = 0;
    BOOL seen_point = FALSE;
    WCHAR stripped[128];

    if (impl->is_currency)
    {
        len = strip_currency( impl, text, len, stripped, ARRAY_SIZE(stripped) );
        text = stripped;
    }
    separators( impl, &decimal, &group, &group_size );
    if (GetLocaleInfoEx( impl->locale, LOCALE_SDECIMAL, buffer, ARRAY_SIZE(buffer) ) && buffer[0]) locale_decimal = buffer[0];
    if (GetLocaleInfoEx( impl->locale, LOCALE_STHOUSAND, buffer, ARRAY_SIZE(buffer) ) && buffer[0]) locale_group = buffer[0];
    if (!len || len >= size - 1) return FALSE;
    for (i = 0; i < len; i++)
    {
        WCHAR c = text[i];
        if (c == '-' && i == 0 && sign) { out[n++] = '-'; continue; }
        if (c >= '0' && c <= '9') { out[n++] = c; digits++; continue; }
        if (c >= impl->numerals->zero && c <= impl->numerals->zero + 9) { out[n++] = '0' + c - impl->numerals->zero; digits++; continue; }
        if ((c == decimal || c == locale_decimal) && !seen_point && !integer) { out[n++] = '.'; seen_point = TRUE; continue; }
        if ((c == group || c == locale_group) && !seen_point && digits) continue;
        return FALSE;
    }
    out[n] = 0;
    return digits > 0;
}

static HRESULT property_value_statics( IPropertyValueStatics **statics )
{
    static const WCHAR name[] = L"Windows.Foundation.PropertyValue";
    HSTRING str;
    HRESULT hr;

    if (FAILED(hr = WindowsCreateString( name, ARRAY_SIZE(name) - 1, &str ))) return hr;
    hr = RoGetActivationFactory( str, &IID_IPropertyValueStatics, (void **)statics );
    WindowsDeleteString( str );
    return hr;
}

/* ---- INumberFormatter ---- */

static HRESULT WINAPI formatter_QueryInterface( INumberFormatter *iface, REFIID iid, void **out )
{
    struct decimal_formatter *impl = impl_from_INumberFormatter( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_INumberFormatter ))
        *out = &impl->INumberFormatter_iface;
    else if (IsEqualGUID( iid, &IID_INumberFormatter2 )) *out = &impl->INumberFormatter2_iface;
    else if (IsEqualGUID( iid, &IID_INumberFormatterOptions )) *out = &impl->INumberFormatterOptions_iface;
    else if (IsEqualGUID( iid, &IID_INumberParser )) *out = &impl->INumberParser_iface;
    else if (IsEqualGUID( iid, &IID_ISignificantDigitsOption )) *out = &impl->ISignificantDigitsOption_iface;
    else if (IsEqualGUID( iid, &IID_INumberRounderOption )) *out = &impl->INumberRounderOption_iface;
    else if (IsEqualGUID( iid, &IID_ISignedZeroOption )) *out = &impl->ISignedZeroOption_iface;
    else if (impl->is_currency && IsEqualGUID( iid, &IID_ICurrencyFormatter )) *out = &impl->ICurrencyFormatter_iface;
    else if (impl->is_currency && IsEqualGUID( iid, &IID_ICurrencyFormatter2 )) *out = &impl->ICurrencyFormatter2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI formatter_AddRef( INumberFormatter *iface )
{
    return InterlockedIncrement( &impl_from_INumberFormatter( iface )->ref );
}

static ULONG WINAPI formatter_Release( INumberFormatter *iface )
{
    struct decimal_formatter *impl = impl_from_INumberFormatter( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->language_count; i++) WindowsDeleteString( impl->languages[i] );
        free( impl->languages );
        WindowsDeleteString( impl->region );
        if (impl->rounder) INumberRounder_Release( impl->rounder );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI formatter_GetIids( INumberFormatter *iface, ULONG *iid_count, IID **iids )
{
    static const GUID *const decimal_iids[] =
    {
        &IID_INumberFormatterOptions, &IID_INumberFormatter, &IID_INumberFormatter2, &IID_INumberParser,
        &IID_ISignificantDigitsOption, &IID_INumberRounderOption, &IID_ISignedZeroOption,
    };
    static const GUID *const currency_iids[] =
    {
        &IID_ICurrencyFormatter, &IID_INumberParser, &IID_INumberFormatter2, &IID_INumberFormatter,
        &IID_INumberFormatterOptions, &IID_ICurrencyFormatter2, &IID_ISignificantDigitsOption,
        &IID_INumberRounderOption, &IID_ISignedZeroOption,
    };
    struct decimal_formatter *impl = impl_from_INumberFormatter( iface );

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    if (impl->is_currency) return inspectable_get_iids( currency_iids, ARRAY_SIZE(currency_iids), iid_count, iids );
    return inspectable_get_iids( decimal_iids, ARRAY_SIZE(decimal_iids), iid_count, iids );
}

static HRESULT WINAPI formatter_GetRuntimeClassName( INumberFormatter *iface, HSTRING *class_name )
{
    struct decimal_formatter *impl = impl_from_INumberFormatter( iface );

    if (impl->is_currency)
        return inspectable_get_class_name( L"Windows.Globalization.NumberFormatting.CurrencyFormatter", class_name );
    return inspectable_get_class_name( RuntimeClass_Windows_Globalization_NumberFormatting_DecimalFormatter, class_name );
}

static HRESULT WINAPI formatter_GetTrustLevel( INumberFormatter *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI formatter_FormatInt( INumberFormatter *iface, INT64 value, HSTRING *result )
{
    TRACE( "iface %p, value %I64d, result %p.\n", iface, value, result );
    if (!result) return E_POINTER;
    return format_int64( impl_from_INumberFormatter( iface ), value, result );
}

static HRESULT WINAPI formatter_FormatUInt( INumberFormatter *iface, UINT64 value, HSTRING *result )
{
    TRACE( "iface %p, value %I64u, result %p.\n", iface, value, result );
    if (!result) return E_POINTER;
    return format_uint64( impl_from_INumberFormatter( iface ), value, result );
}

static HRESULT WINAPI formatter_FormatDouble( INumberFormatter *iface, DOUBLE value, HSTRING *result )
{
    TRACE( "iface %p, value %f, result %p.\n", iface, value, result );
    if (!result) return E_POINTER;
    return format_double( impl_from_INumberFormatter( iface ), value, result );
}

static const INumberFormatterVtbl formatter_vtbl =
{
    formatter_QueryInterface,
    formatter_AddRef,
    formatter_Release,
    /* IInspectable methods */
    formatter_GetIids,
    formatter_GetRuntimeClassName,
    formatter_GetTrustLevel,
    /* INumberFormatter methods */
    formatter_FormatInt,
    formatter_FormatUInt,
    formatter_FormatDouble,
};

/* ---- INumberFormatter2 ---- */

DEFINE_IINSPECTABLE( formatter2, INumberFormatter2, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI formatter2_FormatInt( INumberFormatter2 *iface, INT64 value, HSTRING *result )
{
    if (!result) return E_POINTER;
    return format_int64( impl_from_INumberFormatter2( iface ), value, result );
}

static HRESULT WINAPI formatter2_FormatUInt( INumberFormatter2 *iface, UINT64 value, HSTRING *result )
{
    if (!result) return E_POINTER;
    return format_uint64( impl_from_INumberFormatter2( iface ), value, result );
}

static HRESULT WINAPI formatter2_FormatDouble( INumberFormatter2 *iface, DOUBLE value, HSTRING *result )
{
    if (!result) return E_POINTER;
    return format_double( impl_from_INumberFormatter2( iface ), value, result );
}

static const INumberFormatter2Vtbl formatter2_vtbl =
{
    formatter2_QueryInterface,
    formatter2_AddRef,
    formatter2_Release,
    /* IInspectable methods */
    formatter2_GetIids,
    formatter2_GetRuntimeClassName,
    formatter2_GetTrustLevel,
    /* INumberFormatter2 methods */
    formatter2_FormatInt,
    formatter2_FormatUInt,
    formatter2_FormatDouble,
};

/* ---- INumberFormatterOptions ---- */

DEFINE_IINSPECTABLE( options, INumberFormatterOptions, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI options_get_Languages( INumberFormatterOptions *iface, IVectorView_HSTRING **value )
{
    struct decimal_formatter *impl = impl_from_INumberFormatterOptions( iface );
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

static HRESULT WINAPI options_get_GeographicRegion( INumberFormatterOptions *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_INumberFormatterOptions( iface )->region, value );
}

static HRESULT WINAPI options_get_IntegerDigits( INumberFormatterOptions *iface, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_INumberFormatterOptions( iface )->integer_digits;
    return S_OK;
}

static HRESULT WINAPI options_put_IntegerDigits( INumberFormatterOptions *iface, INT32 value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    if (value < 0) return E_INVALIDARG;
    impl_from_INumberFormatterOptions( iface )->integer_digits = value;
    return S_OK;
}

static HRESULT WINAPI options_get_FractionDigits( INumberFormatterOptions *iface, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_INumberFormatterOptions( iface )->fraction_digits;
    return S_OK;
}

static HRESULT WINAPI options_put_FractionDigits( INumberFormatterOptions *iface, INT32 value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    if (value < 0) return E_INVALIDARG;
    impl_from_INumberFormatterOptions( iface )->fraction_digits = value;
    return S_OK;
}

static HRESULT WINAPI options_get_IsGrouped( INumberFormatterOptions *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_INumberFormatterOptions( iface )->grouped;
    return S_OK;
}

static HRESULT WINAPI options_put_IsGrouped( INumberFormatterOptions *iface, boolean value )
{
    impl_from_INumberFormatterOptions( iface )->grouped = !!value;
    return S_OK;
}

static HRESULT WINAPI options_get_IsDecimalPointAlwaysDisplayed( INumberFormatterOptions *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_INumberFormatterOptions( iface )->point;
    return S_OK;
}

static HRESULT WINAPI options_put_IsDecimalPointAlwaysDisplayed( INumberFormatterOptions *iface, boolean value )
{
    impl_from_INumberFormatterOptions( iface )->point = !!value;
    return S_OK;
}

static HRESULT WINAPI options_get_NumeralSystem( INumberFormatterOptions *iface, HSTRING *value )
{
    const WCHAR *name = impl_from_INumberFormatterOptions( iface )->numerals->name;
    if (!value) return E_POINTER;
    return WindowsCreateString( name, wcslen( name ), value );
}

static HRESULT WINAPI options_put_NumeralSystem( INumberFormatterOptions *iface, HSTRING value )
{
    const struct numeral_system *numerals = find_numeral_system( WindowsGetStringRawBuffer( value, NULL ) );

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (!numerals) return E_INVALIDARG;
    impl_from_INumberFormatterOptions( iface )->numerals = numerals;
    return S_OK;
}

static HRESULT WINAPI options_get_ResolvedLanguage( INumberFormatterOptions *iface, HSTRING *value )
{
    const WCHAR *locale = impl_from_INumberFormatterOptions( iface )->locale;
    if (!value) return E_POINTER;
    return WindowsCreateString( locale, wcslen( locale ), value );
}

static HRESULT WINAPI options_get_ResolvedGeographicRegion( INumberFormatterOptions *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsCreateString( L"ZZ", 2, value );
}

static const INumberFormatterOptionsVtbl options_vtbl =
{
    options_QueryInterface,
    options_AddRef,
    options_Release,
    /* IInspectable methods */
    options_GetIids,
    options_GetRuntimeClassName,
    options_GetTrustLevel,
    /* INumberFormatterOptions methods */
    options_get_Languages,
    options_get_GeographicRegion,
    options_get_IntegerDigits,
    options_put_IntegerDigits,
    options_get_FractionDigits,
    options_put_FractionDigits,
    options_get_IsGrouped,
    options_put_IsGrouped,
    options_get_IsDecimalPointAlwaysDisplayed,
    options_put_IsDecimalPointAlwaysDisplayed,
    options_get_NumeralSystem,
    options_put_NumeralSystem,
    options_get_ResolvedLanguage,
    options_get_ResolvedGeographicRegion,
};

/* ---- INumberParser ---- */

DEFINE_IINSPECTABLE( parser, INumberParser, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI parser_ParseInt( INumberParser *iface, HSTRING text, IReference_INT64 **result )
{
    struct decimal_formatter *impl = impl_from_INumberParser( iface );
    IPropertyValueStatics *statics;
    IInspectable *value;
    char buffer[64], *end;
    UINT32 len;
    const WCHAR *str = WindowsGetStringRawBuffer( text, &len );
    INT64 number;
    HRESULT hr;

    TRACE( "iface %p, text %s, result %p.\n", iface, debugstr_hstring( text ), result );
    if (!result) return E_POINTER;
    *result = NULL;
    if (!normalize( impl, str, len, TRUE, TRUE, buffer, sizeof(buffer) )) return S_OK;
    number = _strtoi64( buffer, &end, 10 );
    if (*end) return S_OK;
    if (FAILED(hr = property_value_statics( &statics ))) return hr;
    hr = IPropertyValueStatics_CreateInt64( statics, number, &value );
    IPropertyValueStatics_Release( statics );
    if (FAILED(hr)) return hr;
    hr = IInspectable_QueryInterface( value, &IID_IReference_INT64, (void **)result );
    IInspectable_Release( value );
    return hr;
}

static HRESULT WINAPI parser_ParseUInt( INumberParser *iface, HSTRING text, IReference_UINT64 **result )
{
    struct decimal_formatter *impl = impl_from_INumberParser( iface );
    IPropertyValueStatics *statics;
    IInspectable *value;
    char buffer[64], *end;
    UINT32 len;
    const WCHAR *str = WindowsGetStringRawBuffer( text, &len );
    UINT64 number;
    HRESULT hr;

    TRACE( "iface %p, text %s, result %p.\n", iface, debugstr_hstring( text ), result );
    if (!result) return E_POINTER;
    *result = NULL;
    if (!normalize( impl, str, len, TRUE, FALSE, buffer, sizeof(buffer) )) return S_OK;
    number = _strtoui64( buffer, &end, 10 );
    if (*end) return S_OK;
    if (FAILED(hr = property_value_statics( &statics ))) return hr;
    hr = IPropertyValueStatics_CreateUInt64( statics, number, &value );
    IPropertyValueStatics_Release( statics );
    if (FAILED(hr)) return hr;
    hr = IInspectable_QueryInterface( value, &IID_IReference_UINT64, (void **)result );
    IInspectable_Release( value );
    return hr;
}

static HRESULT WINAPI parser_ParseDouble( INumberParser *iface, HSTRING text, IReference_DOUBLE **result )
{
    struct decimal_formatter *impl = impl_from_INumberParser( iface );
    IPropertyValueStatics *statics;
    IInspectable *value;
    char buffer[64], *end;
    UINT32 len;
    const WCHAR *str = WindowsGetStringRawBuffer( text, &len );
    double number;
    HRESULT hr;

    TRACE( "iface %p, text %s, result %p.\n", iface, debugstr_hstring( text ), result );
    if (!result) return E_POINTER;
    *result = NULL;
    if (!normalize( impl, str, len, FALSE, TRUE, buffer, sizeof(buffer) )) return S_OK;
    number = strtod( buffer, &end );
    if (*end) return S_OK;
    if (FAILED(hr = property_value_statics( &statics ))) return hr;
    hr = IPropertyValueStatics_CreateDouble( statics, number, &value );
    IPropertyValueStatics_Release( statics );
    if (FAILED(hr)) return hr;
    hr = IInspectable_QueryInterface( value, &IID_IReference_DOUBLE, (void **)result );
    IInspectable_Release( value );
    return hr;
}

static const INumberParserVtbl parser_vtbl =
{
    parser_QueryInterface,
    parser_AddRef,
    parser_Release,
    /* IInspectable methods */
    parser_GetIids,
    parser_GetRuntimeClassName,
    parser_GetTrustLevel,
    /* INumberParser methods */
    parser_ParseInt,
    parser_ParseUInt,
    parser_ParseDouble,
};

/* ---- ISignificantDigitsOption, INumberRounderOption, ISignedZeroOption ---- */

DEFINE_IINSPECTABLE( significant, ISignificantDigitsOption, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI significant_get_SignificantDigits( ISignificantDigitsOption *iface, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_ISignificantDigitsOption( iface )->significant_digits;
    return S_OK;
}

static HRESULT WINAPI significant_put_SignificantDigits( ISignificantDigitsOption *iface, INT32 value )
{
    if (value < 0) return E_INVALIDARG;
    impl_from_ISignificantDigitsOption( iface )->significant_digits = value;
    return S_OK;
}

static const ISignificantDigitsOptionVtbl significant_vtbl =
{
    significant_QueryInterface,
    significant_AddRef,
    significant_Release,
    /* IInspectable methods */
    significant_GetIids,
    significant_GetRuntimeClassName,
    significant_GetTrustLevel,
    /* ISignificantDigitsOption methods */
    significant_get_SignificantDigits,
    significant_put_SignificantDigits,
};

DEFINE_IINSPECTABLE( rounder_option, INumberRounderOption, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI rounder_option_get_NumberRounder( INumberRounderOption *iface, INumberRounder **value )
{
    struct decimal_formatter *impl = impl_from_INumberRounderOption( iface );
    if (!value) return E_POINTER;
    if ((*value = impl->rounder)) INumberRounder_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI rounder_option_put_NumberRounder( INumberRounderOption *iface, INumberRounder *value )
{
    struct decimal_formatter *impl = impl_from_INumberRounderOption( iface );
    if (value) INumberRounder_AddRef( value );
    if (impl->rounder) INumberRounder_Release( impl->rounder );
    impl->rounder = value;
    return S_OK;
}

static const INumberRounderOptionVtbl rounder_option_vtbl =
{
    rounder_option_QueryInterface,
    rounder_option_AddRef,
    rounder_option_Release,
    /* IInspectable methods */
    rounder_option_GetIids,
    rounder_option_GetRuntimeClassName,
    rounder_option_GetTrustLevel,
    /* INumberRounderOption methods */
    rounder_option_get_NumberRounder,
    rounder_option_put_NumberRounder,
};

DEFINE_IINSPECTABLE( signed_zero, ISignedZeroOption, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI signed_zero_get_IsZeroSigned( ISignedZeroOption *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_ISignedZeroOption( iface )->zero_signed;
    return S_OK;
}

static HRESULT WINAPI signed_zero_put_IsZeroSigned( ISignedZeroOption *iface, boolean value )
{
    impl_from_ISignedZeroOption( iface )->zero_signed = !!value;
    return S_OK;
}

static const ISignedZeroOptionVtbl signed_zero_vtbl =
{
    signed_zero_QueryInterface,
    signed_zero_AddRef,
    signed_zero_Release,
    /* IInspectable methods */
    signed_zero_GetIids,
    signed_zero_GetRuntimeClassName,
    signed_zero_GetTrustLevel,
    /* ISignedZeroOption methods */
    signed_zero_get_IsZeroSigned,
    signed_zero_put_IsZeroSigned,
};

/* ---- ICurrencyFormatter, ICurrencyFormatter2 ---- */

DEFINE_IINSPECTABLE( currency, ICurrencyFormatter, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI currency_get_Currency( ICurrencyFormatter *iface, HSTRING *value )
{
    struct decimal_formatter *impl = impl_from_ICurrencyFormatter( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( impl->currency, wcslen( impl->currency ), value );
}

static BOOL parse_currency_code( HSTRING code, WCHAR out[4] )
{
    UINT32 len, i;
    const WCHAR *str = WindowsGetStringRawBuffer( code, &len );

    if (len != 3) return FALSE;
    for (i = 0; i < 3; i++)
    {
        if (!iswalpha( str[i] ) || str[i] > 0x7f) return FALSE;
        out[i] = towupper( str[i] );
    }
    out[3] = 0;
    return TRUE;
}

static HRESULT WINAPI currency_put_Currency( ICurrencyFormatter *iface, HSTRING value )
{
    struct decimal_formatter *impl = impl_from_ICurrencyFormatter( iface );
    WCHAR code[4];

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (!parse_currency_code( value, code )) return E_INVALIDARG;
    wcscpy( impl->currency, code );
    impl->fraction_digits = currency_digits( code );
    return S_OK;
}

static const ICurrencyFormatterVtbl currency_vtbl =
{
    currency_QueryInterface,
    currency_AddRef,
    currency_Release,
    /* IInspectable methods */
    currency_GetIids,
    currency_GetRuntimeClassName,
    currency_GetTrustLevel,
    /* ICurrencyFormatter methods */
    currency_get_Currency,
    currency_put_Currency,
};

DEFINE_IINSPECTABLE( currency2, ICurrencyFormatter2, struct decimal_formatter, INumberFormatter_iface )

static HRESULT WINAPI currency2_get_Mode( ICurrencyFormatter2 *iface, CurrencyFormatterMode *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_ICurrencyFormatter2( iface )->mode;
    return S_OK;
}

static HRESULT WINAPI currency2_put_Mode( ICurrencyFormatter2 *iface, CurrencyFormatterMode value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    if (value != CurrencyFormatterMode_UseSymbol && value != CurrencyFormatterMode_UseCurrencyCode) return E_INVALIDARG;
    impl_from_ICurrencyFormatter2( iface )->mode = value;
    return S_OK;
}

static HRESULT WINAPI currency2_ApplyRoundingForCurrency( ICurrencyFormatter2 *iface, RoundingAlgorithm algorithm )
{
    TRACE( "iface %p, algorithm %d.\n", iface, algorithm );
    if (algorithm < RoundingAlgorithm_None || algorithm > RoundingAlgorithm_RoundHalfToOdd) return E_INVALIDARG;
    impl_from_ICurrencyFormatter2( iface )->rounding = algorithm;
    return S_OK;
}

static const ICurrencyFormatter2Vtbl currency2_vtbl =
{
    currency2_QueryInterface,
    currency2_AddRef,
    currency2_Release,
    /* IInspectable methods */
    currency2_GetIids,
    currency2_GetRuntimeClassName,
    currency2_GetTrustLevel,
    /* ICurrencyFormatter2 methods */
    currency2_get_Mode,
    currency2_put_Mode,
    currency2_ApplyRoundingForCurrency,
};

/* ---- creation ---- */

/* the numeral system of a locale: Arab when an Arabic-script language's native digits are Arabic-Indic */
static const struct numeral_system *default_numerals( const WCHAR *locale )
{
    WCHAR digits[16], script[16];

    if (GetLocaleInfoEx( locale, LOCALE_SSCRIPTS, script, ARRAY_SIZE(script) ) && !wcsncmp( script, L"Arab", 4 ) &&
        GetLocaleInfoEx( locale, LOCALE_SNATIVEDIGITS, digits, ARRAY_SIZE(digits) ))
    {
        if (digits[0] == 0x0660) return find_numeral_system( L"Arab" );
        if (digits[0] == 0x06f0) return find_numeral_system( L"ArabExt" );
    }
    return &numeral_systems[0];
}

static HRESULT decimal_formatter_create( HSTRING *languages, UINT32 count, HSTRING region, INumberFormatter **out )
{
    struct decimal_formatter *impl;
    WCHAR name[LOCALE_NAME_MAX_LENGTH];
    UINT32 i;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->INumberFormatter_iface.lpVtbl = &formatter_vtbl;
    impl->INumberFormatter2_iface.lpVtbl = &formatter2_vtbl;
    impl->INumberFormatterOptions_iface.lpVtbl = &options_vtbl;
    impl->INumberParser_iface.lpVtbl = &parser_vtbl;
    impl->ISignificantDigitsOption_iface.lpVtbl = &significant_vtbl;
    impl->INumberRounderOption_iface.lpVtbl = &rounder_option_vtbl;
    impl->ISignedZeroOption_iface.lpVtbl = &signed_zero_vtbl;
    impl->ICurrencyFormatter_iface.lpVtbl = &currency_vtbl;
    impl->ICurrencyFormatter2_iface.lpVtbl = &currency2_vtbl;
    impl->ref = 1;
    impl->languages = languages;
    impl->language_count = count;
    impl->region = region;
    impl->integer_digits = 1;
    impl->fraction_digits = 2;

    /* the first language the system knows, else the user's */
    for (i = 0; i < count; i++)
        if (GetLocaleInfoEx( WindowsGetStringRawBuffer( languages[i], NULL ), LOCALE_SNAME, name, ARRAY_SIZE(name) ) &&
            name[0] && wcscmp( name, L"!x-sys-default-locale" ))
            break;
    if (i == count && !GetUserDefaultLocaleName( name, ARRAY_SIZE(name) )) wcscpy( name, L"en-US" );
    wcscpy( impl->locale, name );
    impl->numerals = default_numerals( impl->locale );

    *out = &impl->INumberFormatter_iface;
    TRACE( "created %p, locale %s, region %s.\n", impl, debugstr_w( impl->locale ), debugstr_hstring( region ) );
    return S_OK;
}

/* the user's region (ISO 3166 two letters) */
static HRESULT user_region( HSTRING *out )
{
    WCHAR iso[8];

    if (!GetGeoInfoW( GetUserGeoID( GEOCLASS_NATION ), GEO_ISO2, iso, ARRAY_SIZE(iso), 0 )) wcscpy( iso, L"US" );
    return WindowsCreateString( iso, wcslen( iso ), out );
}

struct factory
{
    IActivationFactory IActivationFactory_iface;
    IDecimalFormatterFactory IDecimalFormatterFactory_iface;
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
    else if (IsEqualGUID( iid, &IID_IDecimalFormatterFactory ))
        *out = &impl->IDecimalFormatterFactory_iface;
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
    static const GUID *const list[] = { &IID_IDecimalFormatterFactory };

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    return inspectable_get_iids( list, ARRAY_SIZE(list), iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return inspectable_get_class_name( RuntimeClass_Windows_Globalization_NumberFormatting_DecimalFormatter, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
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

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    HSTRING *languages, region;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (FAILED(hr = user_context( &languages, &region ))) return hr;
    if (FAILED(hr = decimal_formatter_create( languages, 1, region, (INumberFormatter **)instance )))
    {
        WindowsDeleteString( languages[0] );
        free( languages );
        WindowsDeleteString( region );
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

DEFINE_IINSPECTABLE( decimal_factory, IDecimalFormatterFactory, struct factory, IActivationFactory_iface )

static HRESULT WINAPI decimal_factory_CreateDecimalFormatter( IDecimalFormatterFactory *iface, IIterable_HSTRING *languages,
                                                              HSTRING region, INumberFormatter **result )
{
    HSTRING *list, copy;
    UINT32 count, i;
    HRESULT hr;

    TRACE( "iface %p, languages %p, region %s, result %p.\n", iface, languages, debugstr_hstring( region ), result );

    if (!result) return E_POINTER;
    *result = NULL;
    if (!languages || !WindowsGetStringLen( region )) return E_INVALIDARG;
    if (FAILED(hr = languages_from_iterable( languages, &list, &count ))) return hr;
    if (FAILED(hr = WindowsDuplicateString( region, &copy ))) goto failed;
    if (FAILED(hr = decimal_formatter_create( list, count, copy, result )))
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

static const IDecimalFormatterFactoryVtbl decimal_factory_vtbl =
{
    decimal_factory_QueryInterface,
    decimal_factory_AddRef,
    decimal_factory_Release,
    /* IInspectable methods */
    decimal_factory_GetIids,
    decimal_factory_GetRuntimeClassName,
    decimal_factory_GetTrustLevel,
    /* IDecimalFormatterFactory methods */
    decimal_factory_CreateDecimalFormatter,
};

static struct factory factory =
{
    {&factory_vtbl},
    {&decimal_factory_vtbl},
    1,
};

IActivationFactory *decimal_formatter_factory = &factory.IActivationFactory_iface;

/* ---- CurrencyFormatter factory ---- */

/* takes the language strings and the region */
static HRESULT currency_formatter_create( HSTRING code, HSTRING *languages, UINT32 count, HSTRING region,
                                          ICurrencyFormatter **out )
{
    struct decimal_formatter *impl;
    INumberFormatter *formatter;
    WCHAR currency[4];
    HRESULT hr;

    if (!parse_currency_code( code, currency )) return E_INVALIDARG;
    if (FAILED(hr = decimal_formatter_create( languages, count, region, &formatter ))) return hr;
    impl = impl_from_INumberFormatter( formatter );
    impl->is_currency = TRUE;
    wcscpy( impl->currency, currency );
    impl->fraction_digits = currency_digits( currency );
    impl->grouped = TRUE;
    *out = (ICurrencyFormatter *)&impl->ICurrencyFormatter_iface;
    return S_OK;
}

struct currency_factory
{
    IActivationFactory IActivationFactory_iface;
    ICurrencyFormatterFactory ICurrencyFormatterFactory_iface;
    LONG ref;
};

static inline struct currency_factory *impl_from_currency_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct currency_factory, IActivationFactory_iface );
}

static HRESULT WINAPI currency_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct currency_factory *impl = impl_from_currency_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ICurrencyFormatterFactory ))
        *out = &impl->ICurrencyFormatterFactory_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI currency_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_currency_IActivationFactory( iface )->ref );
}

static ULONG WINAPI currency_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_currency_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI currency_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const GUID *const list[] = { &IID_ICurrencyFormatterFactory };

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );
    return inspectable_get_iids( list, ARRAY_SIZE(list), iid_count, iids );
}

static HRESULT WINAPI currency_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return inspectable_get_class_name( L"Windows.Globalization.NumberFormatting.CurrencyFormatter", class_name );
}

static HRESULT WINAPI currency_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* the class has no default constructor */
static HRESULT WINAPI currency_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl currency_factory_vtbl =
{
    currency_factory_QueryInterface,
    currency_factory_AddRef,
    currency_factory_Release,
    /* IInspectable methods */
    currency_factory_GetIids,
    currency_factory_GetRuntimeClassName,
    currency_factory_GetTrustLevel,
    /* IActivationFactory methods */
    currency_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( currency_creator, ICurrencyFormatterFactory, struct currency_factory, IActivationFactory_iface )

static HRESULT WINAPI currency_creator_CreateCurrencyFormatterCode( ICurrencyFormatterFactory *iface, HSTRING code,
                                                                    ICurrencyFormatter **result )
{
    HSTRING *languages, region;
    HRESULT hr;

    TRACE( "iface %p, code %s, result %p.\n", iface, debugstr_hstring( code ), result );

    if (!result) return E_POINTER;
    *result = NULL;
    if (FAILED(hr = user_context( &languages, &region ))) return hr;
    if (FAILED(hr = currency_formatter_create( code, languages, 1, region, result )))
    {
        WindowsDeleteString( languages[0] );
        free( languages );
        WindowsDeleteString( region );
    }
    return hr;
}

static HRESULT WINAPI currency_creator_CreateCurrencyFormatterCodeContext( ICurrencyFormatterFactory *iface, HSTRING code,
                                                                           IIterable_HSTRING *languages, HSTRING region,
                                                                           ICurrencyFormatter **result )
{
    HSTRING *list, copy;
    UINT32 count, i;
    HRESULT hr;

    TRACE( "iface %p, code %s, languages %p, region %s, result %p.\n", iface, debugstr_hstring( code ), languages,
           debugstr_hstring( region ), result );

    if (!result) return E_POINTER;
    *result = NULL;
    if (!languages || !WindowsGetStringLen( region )) return E_INVALIDARG;
    if (FAILED(hr = languages_from_iterable( languages, &list, &count ))) return hr;
    if (FAILED(hr = WindowsDuplicateString( region, &copy ))) goto failed;
    if (FAILED(hr = currency_formatter_create( code, list, count, copy, result )))
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

static const ICurrencyFormatterFactoryVtbl currency_creator_vtbl =
{
    currency_creator_QueryInterface,
    currency_creator_AddRef,
    currency_creator_Release,
    /* IInspectable methods */
    currency_creator_GetIids,
    currency_creator_GetRuntimeClassName,
    currency_creator_GetTrustLevel,
    /* ICurrencyFormatterFactory methods */
    currency_creator_CreateCurrencyFormatterCode,
    currency_creator_CreateCurrencyFormatterCodeContext,
};

static struct currency_factory currency_factory =
{
    {&currency_factory_vtbl},
    {&currency_creator_vtbl},
    1,
};

IActivationFactory *currency_formatter_factory = &currency_factory.IActivationFactory_iface;
