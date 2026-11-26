/*
 * BCP 47 language tags for the Modern Resource Technology (bcp47mrm.dll)
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
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(bcp47mrm);

#define MAX_TAG_LENGTH 84   /* LOCALE_NAME_MAX_LENGTH without the terminator */

struct subtag
{
    const WCHAR *str;
    unsigned int len;
};

static BOOL is_alpha( WCHAR ch ) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
static BOOL is_digit( WCHAR ch ) { return ch >= '0' && ch <= '9'; }

static BOOL all_alpha( const struct subtag *tag )
{
    unsigned int i;
    for (i = 0; i < tag->len; i++) if (!is_alpha( tag->str[i] )) return FALSE;
    return TRUE;
}

static BOOL all_digit( const struct subtag *tag )
{
    unsigned int i;
    for (i = 0; i < tag->len; i++) if (!is_digit( tag->str[i] )) return FALSE;
    return TRUE;
}

static BOOL is_singleton( const struct subtag *tag, WCHAR ch )
{
    return tag->len == 1 && (tag->str[0] | 0x20) == ch;
}

/* The tags of the registry that do not follow the grammar (RFC 5646, section 2.2.8), and the ones that do and are
 * listed with them: whole tags, compared without regard to case. */
static const WCHAR *const grandfathered[] =
{
    L"art-lojban", L"cel-gaulish", L"en-gb-oed", L"i-ami", L"i-bnn", L"i-default", L"i-enochian", L"i-hak",
    L"i-klingon", L"i-lux", L"i-mingo", L"i-navajo", L"i-pwn", L"i-tao", L"i-tay", L"i-tsu", L"no-bok", L"no-nyn",
    L"sgn-be-fr", L"sgn-be-nl", L"sgn-ch-de", L"zh-guoyu", L"zh-hakka", L"zh-min", L"zh-min-nan", L"zh-xiang",
};

static BOOL is_grandfathered( const WCHAR *tag )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(grandfathered); i++)
        if (!wcsicmp( tag, grandfathered[i] )) return TRUE;
    return FALSE;
}

/***********************************************************************
 *      IsWellFormedTag (BCP47MRM.@)
 *
 * Whether a string has the form of a language tag of RFC 5646 (the syntax, not the registry: "xx-ZZ" is well formed).
 * Measured against Windows 11 (the tests list what was asked of it): a language is two or three letters (no four to eight
 * letter languages), at most one extended language subtag follows, then an optional script, an optional region, any number
 * of variants, extensions (a letter or digit but x, then subtags of two to eight characters, the same singleton may be
 * repeated) and a private use part; a tag that is private use only starts with x; a tag is at most 84 characters.
 */
BOOL WINAPI IsWellFormedTag( const WCHAR *tag )
{
    struct subtag parts[48];
    unsigned int count = 0, i, len = 0;
    const WCHAR *p;

    TRACE( "tag %s.\n", debugstr_w( tag ) );

    if (!tag) return FALSE;
    while (tag[len] && len <= MAX_TAG_LENGTH) len++;
    if (!len || len > MAX_TAG_LENGTH) return FALSE;
    if (is_grandfathered( tag )) return TRUE;

    /* the subtags: one to eight letters or digits, separated by one hyphen */
    p = tag;
    for (;;)
    {
        const WCHAR *start = p;

        while (is_alpha( *p ) || is_digit( *p )) p++;
        if (p == start || p - start > 8 || count >= ARRAY_SIZE(parts)) return FALSE;
        parts[count].str = start;
        parts[count].len = p - start;
        count++;
        if (!*p) break;
        if (*p != '-') return FALSE;
        p++;
    }

    i = 0;
    if (is_singleton( &parts[0], 'x' ))   /* private use only */
    {
        if (count < 2) return FALSE;
        return TRUE;   /* every subtag was checked to have one to eight characters */
    }

    /* language, with at most one extended language subtag */
    if (parts[0].len < 2 || parts[0].len > 3 || !all_alpha( &parts[0] )) return FALSE;
    i = 1;
    if (i < count && parts[i].len == 3 && all_alpha( &parts[i] )) i++;
    /* script */
    if (i < count && parts[i].len == 4 && all_alpha( &parts[i] )) i++;
    /* region */
    if (i < count && ((parts[i].len == 2 && all_alpha( &parts[i] )) || (parts[i].len == 3 && all_digit( &parts[i] )))) i++;
    /* variants */
    while (i < count && ((parts[i].len >= 5) || (parts[i].len == 4 && is_digit( parts[i].str[0] )))) i++;
    /* extensions */
    while (i < count && parts[i].len == 1 && !is_singleton( &parts[i], 'x' ))
    {
        unsigned int first = ++i;

        while (i < count && parts[i].len >= 2) i++;
        if (i == first) return FALSE;
    }
    /* private use */
    if (i < count && is_singleton( &parts[i], 'x' ))
    {
        if (++i >= count) return FALSE;
        i = count;
    }
    return i == count;
}

/* the language, script and region of a tag, in lower case; the script of Chinese is implied by the region */
struct lang_parts
{
    WCHAR lang[4], script[5], region[4];
};

static BOOL parse_language( const WCHAR *tag, unsigned int len, struct lang_parts *out )
{
    WCHAR copy[MAX_TAG_LENGTH + 1];
    unsigned int i, pos = 0, start;

    memset( out, 0, sizeof(*out) );
    if (!len || len > MAX_TAG_LENGTH) return FALSE;
    memcpy( copy, tag, len * sizeof(WCHAR) );
    copy[len] = 0;
    if (!IsWellFormedTag( copy )) return FALSE;
    if (copy[0] == 'x' || copy[0] == 'X') return FALSE;   /* private use only */
    for (i = 0; i < len; i++) if (copy[i] >= 'A' && copy[i] <= 'Z') copy[i] |= 0x20;

    /* language */
    for (start = pos; pos < len && copy[pos] != '-'; pos++) ;
    if (pos - start > 3) return FALSE;   /* a grandfathered tag */
    memcpy( out->lang, copy + start, (pos - start) * sizeof(WCHAR) );
    if (pos < len) pos++;
    /* an extended language subtag */
    if (pos + 3 <= len && (pos + 3 == len || copy[pos + 3] == '-') && is_alpha( copy[pos] ) &&
        is_alpha( copy[pos + 1] ) && is_alpha( copy[pos + 2] ))
        pos = min( len, pos + 4 );
    /* script */
    if (pos + 4 <= len && (pos + 4 == len || copy[pos + 4] == '-') && is_alpha( copy[pos] ) && is_alpha( copy[pos + 1] ) &&
        is_alpha( copy[pos + 2] ) && is_alpha( copy[pos + 3] ))
    {
        memcpy( out->script, copy + pos, 4 * sizeof(WCHAR) );
        pos = min( len, pos + 5 );
    }
    /* region */
    if (pos + 2 <= len && (pos + 2 == len || copy[pos + 2] == '-') && is_alpha( copy[pos] ) && is_alpha( copy[pos + 1] ))
        memcpy( out->region, copy + pos, 2 * sizeof(WCHAR) );
    else if (pos + 3 <= len && (pos + 3 == len || copy[pos + 3] == '-') && is_digit( copy[pos] ) && is_digit( copy[pos + 1] ) &&
             is_digit( copy[pos + 2] ))
        memcpy( out->region, copy + pos, 3 * sizeof(WCHAR) );

    if (!out->script[0] && !wcscmp( out->lang, L"zh" ))
        wcscpy( out->script, (!wcscmp( out->region, L"tw" ) || !wcscmp( out->region, L"hk" ) || !wcscmp( out->region, L"mo" ))
                ? L"hant" : L"hans" );
    return TRUE;
}

/* how well a language tag fits another one: 1 for the same, a little less for the same language with another region or
 * without one, 0 for another language or script (measured on Windows 11, see the test; the values for different regions
 * vary a little by pair there, one value is used here); "und" as the wanted language fits every language a little */
static double language_score( const struct lang_parts *wanted, const struct lang_parts *have )
{
    if (!wcscmp( have->lang, L"und" )) return 0.0;
    if (!wcscmp( wanted->lang, L"und" )) return 0.96;
    if (wcscmp( wanted->lang, have->lang )) return 0.0;
    if (wanted->script[0] && have->script[0] && wcscmp( wanted->script, have->script )) return 0.0;
    if (!wanted->region[0] && !have->region[0]) return 1.0;
    if (!wanted->region[0] || !have->region[0]) return 0.98;
    if (!wcscmp( wanted->region, have->region )) return 1.0;
    return !wcscmp( wanted->lang, L"en" ) ? 0.976 : 0.975;
}

/***********************************************************************
 *      GetDistanceOfClosestLanguageInList (BCP47MRM.@)
 *
 * The best score of a language tag against the tags of a list (separated by the delimiter); a tag later in the list counts 0.1
 * less for every position. S_OK and the score, or HRESULT_FROM_WIN32(ERROR_NO_MATCH) and 0 when no tag of the list fits.
 */
HRESULT WINAPI GetDistanceOfClosestLanguageInList( const WCHAR *language, const WCHAR *list, LONG delimiter, double *score )
{
    struct lang_parts wanted, have;
    const WCHAR *p, *end;
    WCHAR sep = delimiter;
    double best = 0.0;
    unsigned int position = 0;

    TRACE( "language %s, list %s, delimiter %#x, score %p.\n", debugstr_w( language ), debugstr_w( list ), sep, score );

    if (!language || !list || !score) return E_POINTER;
    *score = 0.0;

    if (!parse_language( language, wcslen( language ), &wanted )) return HRESULT_FROM_WIN32( ERROR_NO_MATCH );
    for (p = list; ; p = end + 1, position++)
    {
        end = p;
        while (*end && *end != sep) end++;
        if (parse_language( p, end - p, &have ))
        {
            double value = language_score( &wanted, &have );

            if (value > 0.0) value -= 0.1 * position;
            if (value > best) best = value;
        }
        if (!*end) break;
    }
    if (best <= 0.0) return HRESULT_FROM_WIN32( ERROR_NO_MATCH );
    *score = best;
    return S_OK;
}
