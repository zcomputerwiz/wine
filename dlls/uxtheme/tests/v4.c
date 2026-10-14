/*
 * Tests for Vista-format visual styles.
 * Copyright 2026 zcomputerwiz
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>
#include "windows.h"
#include "uxtheme.h"
#include "vssym32.h"
#include "wine/test.h"

static HRESULT (WINAPI *pOpenThemeFile)(const WCHAR *, const WCHAR *, const WCHAR *, HANDLE *, DWORD);
static HRESULT (WINAPI *pCloseThemeFile)(HANDLE);
static HRESULT (WINAPI *pApplyTheme)(HANDLE, char *, HWND);
static HTHEME (WINAPI *pOpenThemeDataForDpi)(HWND, LPCWSTR, UINT);
static HRESULT (WINAPI *pGetThemeIntList)(HTHEME, int, int, int, INTLIST *);

static void test_msstyles_v4(void)
{
    WCHAR temp_path[MAX_PATH], temp_file[MAX_PATH];
    WCHAR cur_theme[MAX_PATH], cur_color[MAX_PATH], cur_size[MAX_PATH];
    WCHAR old_theme[MAX_PATH], old_color[MAX_PATH], old_size[MAX_PATH];
    HANDLE hFile, old_htf;
    DWORD written;
    HANDLE htf;
    HRESULT hr;
    HTHEME hButton, hWindow;
    int int_val;
    BOOL bool_val;
    COLORREF color_val;
    POINT pt;
    MARGINS margins;
    LOGFONTW font;
    INTLIST intlist;
    WCHAR str_val[64];
    RECT rect_val;
    HRSRC resource;
    HMODULE module;
    const BYTE *fixture;
    BYTE *corrupted;
    DWORD fixture_size, variant_offset;

    if (!pOpenThemeFile || !pCloseThemeFile || !pApplyTheme)
    {
        win_skip("Theme file entry points are unavailable.\n");
        return;
    }

    resource = FindResourceA(NULL, "v4_style.dll", "TESTDLL");
    ok(resource != NULL, "Style resource not found, error %lu.\n", GetLastError());
    if (!resource) return;
    fixture = LockResource(LoadResource(NULL, resource));
    fixture_size = SizeofResource(NULL, resource);

    GetTempPathW(ARRAY_SIZE(temp_path), temp_path);
    GetTempFileNameW(temp_path, L"ms4", 0, temp_file);
    hFile = CreateFileW(temp_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "Failed to create temp file: %ld\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE) return;
    ok(WriteFile(hFile, fixture, fixture_size, &written, NULL) && written == fixture_size,
       "Failed to write style resource, error %lu.\n", GetLastError());
    CloseHandle(hFile);

    /* Open the style without assuming platform-specific scheme names. */
    hr = pOpenThemeFile(temp_file, NULL, NULL, &htf, 0);
    /* Windows can reject unsigned or unsupported visual style formats. */
    if (FAILED(hr))
    {
        win_skip("Synthetic unsigned style is unsupported, hr %#lx.\n", hr);
        DeleteFileW(temp_file);
        return;
    }
    ok(htf != NULL, "Expected non-NULL htf\n");
    if (!htf) { DeleteFileW(temp_file); return; }

    module = LoadLibraryExW(temp_file, NULL, LOAD_LIBRARY_AS_DATAFILE);
    resource = FindResourceW(module, L"NORMAL", L"VARIANT");
    ok(module && resource, "Failed to locate VARIANT, error %lu.\n", GetLastError());
    if (!module || !resource)
    {
        if (module) FreeLibrary(module);
        pCloseThemeFile(htf);
        DeleteFileW(temp_file);
        return;
    }
    variant_offset = (const BYTE *)LockResource(LoadResource(module, resource)) -
                     (const BYTE *)((ULONG_PTR)module & ~(ULONG_PTR)3);
    FreeLibrary(module);
    ok(variant_offset + 32 <= fixture_size, "Invalid VARIANT offset %lu.\n", variant_offset);

    /* Test 4: Apply theme and verify properties */
    if (FAILED(GetCurrentThemeName(old_theme, ARRAY_SIZE(old_theme), old_color, ARRAY_SIZE(old_color),
                                   old_size, ARRAY_SIZE(old_size))))
        old_theme[0] = 0;
    if (pApplyTheme)
    {
        hr = pApplyTheme(htf, NULL, NULL);
        ok(hr == S_OK, "ApplyTheme failed: 0x%08lx\n", hr);
    }

    hr = GetCurrentThemeName(cur_theme, ARRAY_SIZE(cur_theme),
                             cur_color, ARRAY_SIZE(cur_color),
                             cur_size, ARRAY_SIZE(cur_size));
    ok(hr == S_OK, "GetCurrentThemeName failed: 0x%08lx\n", hr);
    ok(!lstrcmpiW(cur_theme, temp_file), "Unexpected active style %s.\n", wine_dbgstr_w(cur_theme));

    hButton = OpenThemeData(NULL, L"Button");
    ok(hButton != NULL, "OpenThemeData(Button) failed\n");
    if (hButton)
    {
        /* INT: BORDERSIZE (2403) = 2 */
        hr = GetThemeInt(hButton, 1, 0, 2403, &int_val);
        ok(hr == S_OK, "GetThemeInt(BORDERSIZE) failed: 0x%08lx\n", hr);
        ok(int_val == 2, "Expected BORDERSIZE=2, got %d\n", int_val);

        /* BOOL: TRANSPARENT (2201) = TRUE */
        hr = GetThemeBool(hButton, 1, 0, 2201, &bool_val);
        ok(hr == S_OK, "GetThemeBool(TRANSPARENT) failed: 0x%08lx\n", hr);
        ok(bool_val == TRUE, "Expected TRANSPARENT=TRUE, got %d\n", bool_val);

        /* IsThemeBackgroundPartiallyTransparent */
        bool_val = IsThemeBackgroundPartiallyTransparent(hButton, 1, 0);
        ok(bool_val == TRUE, "Expected IsThemeBackgroundPartiallyTransparent(Button) == TRUE\n");

        /* COLOR: BORDERCOLOR (3801) at state 0 = RGB(10, 20, 30) */
        hr = GetThemeColor(hButton, 1, 0, 3801, &color_val);
        ok(hr == S_OK, "GetThemeColor(BORDERCOLOR state 0) failed: 0x%08lx\n", hr);
        ok(color_val == RGB(10, 20, 30), "Expected RGB(10, 20, 30), got 0x%08lx\n", (unsigned long)color_val);

        /* COLOR: BORDERCOLOR (3801) override at state 1 = RGB(50, 60, 70) */
        hr = GetThemeColor(hButton, 1, 1, 3801, &color_val);
        ok(hr == S_OK, "GetThemeColor(BORDERCOLOR state 1) failed: 0x%08lx\n", hr);
        ok(color_val == RGB(50, 60, 70), "Expected RGB(50, 60, 70), got 0x%08lx\n", (unsigned long)color_val);

        /* COLOR: BORDERCOLOR (3801) override at state 2 = RGB(255, 128, 0) */
        hr = GetThemeColor(hButton, 1, 2, 3801, &color_val);
        ok(hr == S_OK, "GetThemeColor(BORDERCOLOR state 2) failed: 0x%08lx\n", hr);
        ok(color_val == RGB(255, 128, 0), "Expected RGB(255, 128, 0), got 0x%08lx\n", (unsigned long)color_val);

        /* COLOR: BORDERCOLOR (3801) override at state 3 = RGB(0, 128, 255) */
        hr = GetThemeColor(hButton, 1, 3, 3801, &color_val);
        ok(hr == S_OK, "GetThemeColor(BORDERCOLOR state 3) failed: 0x%08lx\n", hr);
        ok(color_val == RGB(0, 128, 255), "Expected RGB(0, 128, 255), got 0x%08lx\n", (unsigned long)color_val);

        /* ENUM: BGTYPE (4001) = 0 */
        hr = GetThemeEnumValue(hButton, 1, 0, 4001, &int_val);
        ok(hr == S_OK, "GetThemeEnumValue(BGTYPE) failed: 0x%08lx\n", hr);
        ok(int_val == 0, "Expected BGTYPE=0, got %d\n", int_val);

        /* ENUM: SIZINGTYPE (4004) = 1 */
        hr = GetThemeEnumValue(hButton, 1, 0, 4004, &int_val);
        ok(hr == S_OK, "GetThemeEnumValue(SIZINGTYPE) failed: 0x%08lx\n", hr);
        ok(int_val == 1, "Expected SIZINGTYPE=1, got %d\n", int_val);

        /* POSITION: OFFSET (3401) = (1, 1) */
        hr = GetThemePosition(hButton, 1, 0, 3401, &pt);
        ok(hr == S_OK, "GetThemePosition(OFFSET) failed: 0x%08lx\n", hr);
        ok(pt.x == 1 && pt.y == 1, "Expected (1, 1), got (%ld, %ld)\n", pt.x, pt.y);

        /* MARGINS: SIZINGMARGINS (3601) = (4, 4, 4, 4) */
        hr = GetThemeMargins(hButton, NULL, 1, 0, 3601, NULL, &margins);
        ok(hr == S_OK, "GetThemeMargins(SIZINGMARGINS) failed: 0x%08lx\n", hr);
        ok(margins.cxLeftWidth == 4 && margins.cxRightWidth == 4 &&
           margins.cyTopHeight == 4 && margins.cyBottomHeight == 4,
           "Expected SIZINGMARGINS=(4, 4, 4, 4), got (%d, %d, %d, %d)\n",
           margins.cxLeftWidth, margins.cxRightWidth, margins.cyTopHeight, margins.cyBottomHeight);

        /* MARGINS: CONTENTMARGINS (3602) = (2, 2, 2, 2) */
        hr = GetThemeMargins(hButton, NULL, 1, 0, 3602, NULL, &margins);
        ok(hr == S_OK, "GetThemeMargins(CONTENTMARGINS) failed: 0x%08lx\n", hr);
        ok(margins.cxLeftWidth == 2 && margins.cxRightWidth == 2 &&
           margins.cyTopHeight == 2 && margins.cyBottomHeight == 2,
           "Expected CONTENTMARGINS=(2, 2, 2, 2), got (%d, %d, %d, %d)\n",
           margins.cxLeftWidth, margins.cxRightWidth, margins.cyTopHeight, margins.cyBottomHeight);

        /* FONT: CAPTIONFONT (801) -> Arial bold */
        hr = GetThemeFont(hButton, NULL, 1, 0, 801, &font);
        ok(hr == S_OK, "GetThemeFont(CAPTIONFONT) failed: 0x%08lx\n", hr);
        ok(!lstrcmpiW(font.lfFaceName, L"Arial"), "Expected font face Arial, got %s\n", wine_dbgstr_w(font.lfFaceName));
        ok(font.lfWeight == FW_BOLD, "Expected font weight FW_BOLD, got %ld\n", font.lfWeight);

        /* INTLIST: TRANSITIONDURATIONS (6000) = [100, 200, 300] */
        if (pGetThemeIntList)
        {
            hr = pGetThemeIntList(hButton, 1, 0, 6000, &intlist);
            ok(hr == S_OK, "GetThemeIntList failed: 0x%08lx\n", hr);
            ok(intlist.iValueCount == 3, "Expected iValueCount=3, got %d\n", intlist.iValueCount);
            ok(intlist.iValues[0] == 100 && intlist.iValues[1] == 200 && intlist.iValues[2] == 300,
               "Expected [100, 200, 300], got [%d, %d, %d]\n",
               intlist.iValues[0], intlist.iValues[1], intlist.iValues[2]);
        }

        CloseThemeData(hButton);
    }

    hWindow = OpenThemeData(NULL, L"Window");
    ok(hWindow != NULL, "OpenThemeData(Window) failed\n");
    if (hWindow)
    {
        /* STRING: ATLASINPUTIMAGE (8001) = "test_atlas.png" */
        hr = GetThemeString(hWindow, 1, 1, 8001, str_val, ARRAY_SIZE(str_val));
        ok(hr == S_OK, "GetThemeString(ATLASINPUTIMAGE) failed: 0x%08lx\n", hr);
        ok(!lstrcmpiW(str_val, L"test_atlas.png"), "Expected test_atlas.png, got %s\n", wine_dbgstr_w(str_val));

        /* RECT: ATLASRECT (8002) at state 1 = {0, 0, 16, 16} */
        hr = GetThemeRect(hWindow, 1, 1, 8002, &rect_val);
        ok(hr == S_OK, "GetThemeRect(ATLASRECT state 1) failed: 0x%08lx\n", hr);
        ok(rect_val.left == 0 && rect_val.top == 0 && rect_val.right == 16 && rect_val.bottom == 16,
           "Expected {0, 0, 16, 16}, got {%ld, %ld, %ld, %ld}\n",
           rect_val.left, rect_val.top, rect_val.right, rect_val.bottom);

        /* RECT: ATLASRECT (8002) at state 2 = {16, 0, 32, 16} */
        hr = GetThemeRect(hWindow, 1, 2, 8002, &rect_val);
        ok(hr == S_OK, "GetThemeRect(ATLASRECT state 2) failed: 0x%08lx\n", hr);
        ok(rect_val.left == 16 && rect_val.top == 0 && rect_val.right == 32 && rect_val.bottom == 16,
           "Expected {16, 0, 32, 16}, got {%ld, %ld, %ld, %ld}\n",
           rect_val.left, rect_val.top, rect_val.right, rect_val.bottom);

        /* Drawing: Atlas stream and DPI variant rendering */
        {
            HDC hdc_screen = GetDC(NULL);
            HDC hdc_mem = CreateCompatibleDC(hdc_screen);
            BITMAPINFO bi = {0};
            DWORD *bits = NULL;
            HBITMAP hbmp;
            HGDIOBJ old_bmp;
            RECT rc = {0, 0, 16, 16};
            COLORREF px;

            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = 64;
            bi.bmiHeader.biHeight = -64;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            hbmp = CreateDIBSection(hdc_mem, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
            old_bmp = SelectObject(hdc_mem, hbmp);

            /* Window Part 1 State 1 (Active caption) -> Atlas Green tile */
            memset(bits, 0, 64 * 64 * sizeof(DWORD));
            hr = DrawThemeBackground(hWindow, hdc_mem, 1, 1, &rc, NULL);
            ok(hr == S_OK, "DrawThemeBackground(Window state 1) failed: 0x%08lx\n", hr);
            px = GetPixel(hdc_mem, 8, 8);
            ok(px == RGB(50, 200, 80), "Expected Green RGB(50, 200, 80), got 0x%08lx\n", px);

            /* Window Part 1 State 2 (Inactive caption) -> Atlas Gray tile */
            memset(bits, 0, 64 * 64 * sizeof(DWORD));
            hr = DrawThemeBackground(hWindow, hdc_mem, 1, 2, &rc, NULL);
            ok(hr == S_OK, "DrawThemeBackground(Window state 2) failed: 0x%08lx\n", hr);
            px = GetPixel(hdc_mem, 8, 8);
            ok(px == RGB(190, 190, 190), "Expected Gray RGB(190, 190, 190), got 0x%08lx\n", px);

            /* Button DPI variant selection */
            if (pOpenThemeDataForDpi)
            {
                HTHEME hbtn_96, hbtn_144;

                hbtn_96 = pOpenThemeDataForDpi(NULL, L"Button", 96);
                ok(hbtn_96 != NULL, "OpenThemeDataForDpi(Button, 96) failed\n");
                if (hbtn_96)
                {
                    RECT rc_btn = {0, 0, 20, 20};
                    memset(bits, 0, 64 * 64 * sizeof(DWORD));
                    hr = DrawThemeBackground(hbtn_96, hdc_mem, 1, 1, &rc_btn, NULL);
                    ok(hr == S_OK, "DrawThemeBackground(Button 96 DPI) failed: 0x%08lx\n", hr);
                    px = GetPixel(hdc_mem, 10, 10);
                    ok(px == RGB(120, 180, 250), "Expected 96 DPI center RGB(120, 180, 250), got 0x%08lx\n", px);
                    CloseThemeData(hbtn_96);
                }

                hbtn_144 = pOpenThemeDataForDpi(NULL, L"Button", 144);
                ok(hbtn_144 != NULL, "OpenThemeDataForDpi(Button, 144) failed\n");
                if (hbtn_144)
                {
                    RECT rc_btn = {0, 0, 24, 24};
                    memset(bits, 0, 64 * 64 * sizeof(DWORD));
                    hr = DrawThemeBackground(hbtn_144, hdc_mem, 1, 1, &rc_btn, NULL);
                    ok(hr == S_OK, "DrawThemeBackground(Button 144 DPI) failed: 0x%08lx\n", hr);
                    px = GetPixel(hdc_mem, 12, 12);
                    ok(px == RGB(140, 200, 255), "Expected 144 DPI center RGB(140, 200, 255), got 0x%08lx\n", px);
                    CloseThemeData(hbtn_144);
                }
            }

            SelectObject(hdc_mem, old_bmp);
            DeleteObject(hbmp);
            DeleteDC(hdc_mem);
            ReleaseDC(NULL, hdc_screen);
        }

        CloseThemeData(hWindow);
    }

    /* Restore the theme that was active before, so the other tests are unaffected */
    if (pApplyTheme)
    {
        if (old_theme[0] && SUCCEEDED(pOpenThemeFile(old_theme, old_color, old_size, &old_htf, 0)))
        {
            pApplyTheme(old_htf, NULL, NULL);
            pCloseThemeFile(old_htf);
        }
        else
            pApplyTheme(NULL, NULL, NULL);
    }

    pCloseThemeFile(htf);
    DeleteFileW(temp_file);

    /* Test 5: Malformed / truncated file bounds checks */
    GetTempFileNameW(temp_path, L"fuz", 0, temp_file);
    hFile = CreateFileW(temp_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(hFile, fixture, min(fixture_size, 512), &written, NULL);
    CloseHandle(hFile);

    htf = (void *)0xdeadbeef;
    hr = pOpenThemeFile(temp_file, NULL, NULL, &htf, 0);
    ok(FAILED(hr), "Truncated style must fail: got 0x%08lx\n", hr);
    if (SUCCEEDED(hr) && htf) pCloseThemeFile(htf);
    DeleteFileW(temp_file);

    /* Test 6: Corrupted VARIANT stream fails cleanly without crashing */
    if (variant_offset + 32 > fixture_size) return;
    corrupted = malloc(fixture_size);
    ok(corrupted != NULL, "Failed to allocate corrupted fixture.\n");
    if (!corrupted) return;
    memcpy(corrupted, fixture, fixture_size);
    memset(corrupted + variant_offset, 0xff, 32);

    GetTempFileNameW(temp_path, L"fuz", 0, temp_file);
    hFile = CreateFileW(temp_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(hFile, corrupted, fixture_size, &written, NULL);
    free(corrupted);
    CloseHandle(hFile);

    htf = (void *)0xdeadbeef;
    hr = pOpenThemeFile(temp_file, NULL, NULL, &htf, 0);
    ok(FAILED(hr), "Corrupted style must fail: got 0x%08lx\n", hr);
    if (SUCCEEDED(hr) && htf) pCloseThemeFile(htf);
    DeleteFileW(temp_file);
}

START_TEST(v4)
{
    HMODULE uxtheme = GetModuleHandleA("uxtheme.dll");
    pOpenThemeFile = (void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(2));
    pCloseThemeFile = (void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(3));
    pApplyTheme = (void *)GetProcAddress(uxtheme, MAKEINTRESOURCEA(4));
    pOpenThemeDataForDpi = (void *)GetProcAddress(uxtheme, "OpenThemeDataForDpi");
    pGetThemeIntList = (void *)GetProcAddress(uxtheme, "GetThemeIntList");
    test_msstyles_v4();
}
