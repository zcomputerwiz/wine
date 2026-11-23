/*
 * Unit tests for bcp47mrm.dll
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
#include <math.h>

#include "windef.h"
#include "winbase.h"

#include "wine/test.h"

BOOL WINAPI IsWellFormedTag( const WCHAR *tag );
HRESULT WINAPI GetDistanceOfClosestLanguageInList( const WCHAR *language, const WCHAR *list, LONG delimiter, double *score );

/* the answers of Windows 11 (10.0.26100) */
static const struct
{
    const WCHAR *tag;
    BOOL expected;
} tags[] =
{
    { L"en", TRUE }, { L"en-US", TRUE }, { L"en-us", TRUE }, { L"EN-US", TRUE }, { L"zh-Hans-CN", TRUE },
    { L"x-private", TRUE }, { L"i-klingon", TRUE }, { L"en-", FALSE }, { L"e", FALSE }, { L"abcdefghi", FALSE },
    { L"abcde", FALSE }, { L"abcdefgh", FALSE }, { L"en-US-", FALSE }, { L"de-DE-1901", TRUE },
    { L"en-a-bbb-x-a-ccc", TRUE }, { L"", FALSE }, { L"en_US", FALSE }, { L"und", TRUE },
    { L"qaa-Qaaa-QM-x-southern", TRUE }, { L"sl-rozaj-biske", TRUE }, { L"en-GB-oed", TRUE }, { L"art-lojban", TRUE },
    { L"zh-min-nan", TRUE }, { L"sgn-BE-FR", TRUE }, { L"cmn-Hans-CN", TRUE }, { L"es-419", TRUE },
    { L"en-u-ca-gregory", TRUE }, { L"en-US-u-ca-x", FALSE }, { L"en-x", FALSE }, { L"x", FALSE }, { L"en-a", FALSE },
    { L"en-abc-abc-abc-abc", FALSE }, { L"en-abc-abc-abc", FALSE }, { L"de-1901-1901", TRUE }, { L"en-US-US", FALSE },
    { L"en-Latn-Latn", FALSE }, { L"de-CH-1901", TRUE }, { L"ja-Latn-hepburn-heploc", TRUE },
    { L"en-US-x-a-b-c-d-e-f-g-h-i-j", TRUE }, { L"zh-yue", TRUE }, { L"i-default", TRUE }, { L"no-bok", TRUE },
    { L"sr-Latn-RS", TRUE }, { L"1en", FALSE }, { L"en-1", FALSE }, { L"en-12", FALSE }, { L"en-123", TRUE },
    { L"en-1234", TRUE }, { L"en-12345", TRUE }, { L"a-b", FALSE }, { L"en-US_x", FALSE }, { L"en-us-posix", TRUE },
    { L"ar-001", TRUE }, { L"en--US", FALSE }, { L" en", FALSE }, { L"en ", FALSE }, { L"en-\x00e9", FALSE },
    { L"en-abc", TRUE }, { L"en-abc-def", FALSE }, { L"en-abc-def-ghi", FALSE }, { L"en-abc-def-ghi-jkl", FALSE },
    { L"zh-cmn-Hans", TRUE }, { L"en-Latn", TRUE }, { L"en-Latn-US-1901", TRUE }, { L"en-US-1901-1902", TRUE },
    { L"en-1901", TRUE }, { L"en-ab", TRUE }, { L"en-abcd-abcd", FALSE }, { L"en-u-ca", TRUE },
    { L"en-u-ca-gregory-x-a", TRUE }, { L"en-t-ja", TRUE }, { L"en-a-bbb-a-ccc", TRUE }, { L"en-x-a-x-b", TRUE },
    { L"en-US-x", FALSE }, { L"i-x", FALSE }, { L"x-a", TRUE }, { L"x-abcdefghi", FALSE }, { L"x-abcdefgh", TRUE },
    { L"i-enochian", TRUE }, { L"i-mingo", TRUE }, { L"zh-xiang", TRUE }, { L"en-US-u-nu-latn", TRUE },
    { L"en-a-b", FALSE }, { L"en-a-bb", TRUE }, { L"en-u-attr-ca-x", FALSE }, { L"de-1996", TRUE },
    { L"de-Latn-DE-1996-fonipa", TRUE }, { L"de-Qaaa", TRUE }, { L"en-QM", TRUE }, { L"en-AA", TRUE },
    { L"en-XX", TRUE }, { L"xx", TRUE }, { L"zz-ZZ", TRUE }, { L"aaa", TRUE }, { L"aaaa", FALSE }, { L"en-bbbb", TRUE },
    { L"en-US-a-ab-b-cd", TRUE }, { L"en-r-ab-r-cd", TRUE }, { L"en-US-U-CA-GREGORY", TRUE }, { L"I-KLINGON", TRUE },
    { L"Art-Lojban", TRUE }, { L"en-gb-oed", TRUE }, { L"en-GB-Oed", TRUE }, { L"sgn-be-fr", TRUE },
    { L"abc-def", TRUE }, { L"abc-def-ghi-jkl", FALSE }, { L"abc-def-ghi-jkl-mno", FALSE }, { L"ab-cd-ef", FALSE },
    { L"en-us-1901-1902-1903-1904-1905-1906-1907-1908", TRUE }, { L"en-a-bb-b-cc-c-dd-d-ee-e-ff-f-gg", TRUE },
    { L"EN-x-AbC", TRUE },
    /* the longest tag is 84 characters (eight subtags of eight, seven hyphens and the "en-x-" are 76) */
    { L"en-x-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh", TRUE },
    { L"en-x-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh-abcdefgh", FALSE },
};

static void test_IsWellFormedTag(void)
{
    unsigned int i;
    BOOL ret;

    for (i = 0; i < ARRAY_SIZE(tags); i++)
    {
        ret = IsWellFormedTag( tags[i].tag );
        ok( !!ret == tags[i].expected, "%s: got %d, expected %d.\n", debugstr_w( tags[i].tag ), ret, tags[i].expected );
    }
}

/* the answers of Windows 11: the score is 1 for the same tag, 0.1 less for every position in the list; no match gives ERROR_NO_MATCH */
static const struct
{
    const WCHAR *language, *list;
    double score;   /* 0: no match */
} scores[] =
{
    { L"en-US", L"en-US", 1.0 }, { L"en-US", L"en", 0.98 }, { L"en", L"en-US", 0.98 }, { L"fr", L"fr-CA", 0.98 },
    { L"en-US", L"fr-FR", 0.0 }, { L"en-US", L"fr-FR;en-US", 0.9 }, { L"en-US", L"", 0.0 }, { L"", L"en-US", 0.0 },
    { L"zh-Hans-CN", L"zh-CN", 1.0 }, { L"zh-Hans-CN", L"zh-TW", 0.0 }, { L"EN-us", L"en-US", 1.0 },
    { L"und", L"en-US", 0.96 }, { L"en-US", L"und", 0.0 }, { L"x-private", L"x-private", 0.0 }, { L"en-US", L"en-US,fr-FR", 0.0 },
};

static void test_GetDistanceOfClosestLanguageInList(void)
{
    double score;
    HRESULT hr;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(scores); i++)
    {
        score = -7.0;
        hr = GetDistanceOfClosestLanguageInList( scores[i].language, scores[i].list, ';', &score );
        if (scores[i].score)
        {
            ok( hr == S_OK, "%s, %s: got %#lx.\n", debugstr_w( scores[i].language ), debugstr_w( scores[i].list ), hr );
            ok( fabs( score - scores[i].score ) < 0.0001, "%s, %s: got %g, expected %g.\n", debugstr_w( scores[i].language ),
                debugstr_w( scores[i].list ), score, scores[i].score );
        }
        else
        {
            ok( hr == HRESULT_FROM_WIN32( ERROR_NO_MATCH ), "%s, %s: got %#lx.\n", debugstr_w( scores[i].language ),
                debugstr_w( scores[i].list ), hr );
            ok( score == 0.0, "%s, %s: got %g.\n", debugstr_w( scores[i].language ), debugstr_w( scores[i].list ), score );
        }
    }

    score = -7.0;
    hr = GetDistanceOfClosestLanguageInList( L"en-US", L"en-US,fr-FR", ',', &score );
    ok( hr == S_OK && score == 1.0, "got %#lx, %g.\n", hr, score );
    score = -7.0;
    hr = GetDistanceOfClosestLanguageInList( NULL, L"en-US", ';', &score );
    ok( hr == E_POINTER && score == -7.0, "got %#lx, %g.\n", hr, score );
    hr = GetDistanceOfClosestLanguageInList( L"en-US", NULL, ';', &score );
    ok( hr == E_POINTER, "got %#lx.\n", hr );
    hr = GetDistanceOfClosestLanguageInList( L"en-US", L"en-US", ';', NULL );
    ok( hr == E_POINTER, "got %#lx.\n", hr );
}

START_TEST(bcp47mrm)
{
    HMODULE module = LoadLibraryW( L"bcp47mrm.dll" );

    if (!module)
    {
        win_skip( "bcp47mrm.dll is not available.\n" );
        return;
    }
    FreeLibrary( module );

    test_IsWellFormedTag();
    test_GetDistanceOfClosestLanguageInList();
}
