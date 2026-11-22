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

#include "windef.h"
#include "winbase.h"

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
