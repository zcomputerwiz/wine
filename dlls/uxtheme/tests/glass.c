/*
 * WineExplorer Desktop Environment - glass.c
 *
 * Conformance tests for DWM/uxtheme glass-drawing (DTT_COMPOSITED, DTT_GLOWSIZE,
 * DTT_CALCRECT, buffered paint alpha, dwmapi colorization and peek attributes).
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdarg.h>
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>
#include "wine/test.h"

#ifndef BPPF_ERASE
#define BPPF_ERASE 0x0001
#endif
#ifndef BPPF_NOCLIP
#define BPPF_NOCLIP 0x0002
#endif

#ifndef DWMWA_FLIP3D_POLICY
#define DWMWA_FLIP3D_POLICY 8
#endif
#ifndef DWMWA_EXCLUDED_FROM_PEEK
#define DWMWA_EXCLUDED_FROM_PEEK 12
#endif

typedef struct _DWMCOLORIZATIONPARAMS
{
    COLORREF clrColor;
    COLORREF clrAfterGlow;
    UINT nIntensity;
    UINT clrAfterGlowBalance;
    UINT clrBlurBalance;
    UINT clrGlassReflectionIntensity;
    BOOL fOpaque;
} DWMCOLORIZATIONPARAMS;

static HRESULT (WINAPI *pDwmIsCompositionEnabled)(BOOL *);
static HRESULT (WINAPI *pDwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
static HRESULT (WINAPI *pDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
static HRESULT (WINAPI *pDwmpGetColorizationParameters)(void *);

static HDC create_test_dib(int w, int h, DWORD **bits, HBITMAP *hbmp)
{
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {{ sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB }};
    *hbmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
    SelectObject(dc, *hbmp);
    return dc;
}

static void test_draw_theme_text_ex_composited(HTHEME theme)
{
    HDC dc;
    HBITMAP bmp;
    DWORD *bits = NULL;
    RECT rc = { 20, 20, 100, 60 };
    DTTOPTS opts = { sizeof(opts) };
    HRESULT hr;
    int x, y;
    int text_pixels = 0;

    dc = create_test_dib(150, 100, &bits, &bmp);
    ok(dc != NULL && bits != NULL, "Failed to create test DIB\n");
    memset(bits, 0, 150 * 100 * sizeof(DWORD));

    opts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
    opts.crText = RGB(255, 0, 0);

    hr = DrawThemeTextEx(theme, dc, 0, 0, L"Testing", -1, DT_CENTER | DT_VCENTER | DT_SINGLELINE, &rc, &opts);
    ok(hr == S_OK, "DrawThemeTextEx failed: %#lx\n", hr);

    for (y = rc.top; y < rc.bottom; y++)
    {
        for (x = rc.left; x < rc.right; x++)
        {
            DWORD p = bits[y * 150 + x];
            if ((p >> 24) > 0)
                text_pixels++;
        }
    }

    ok(text_pixels > 0, "Expected text pixels with alpha > 0, got %d\n", text_pixels);
    ok(bits[0] == 0, "Expected far pixel at (0,0) to remain 0, got %#lx\n", bits[0]);
    ok(bits[149] == 0, "Expected far pixel at (149,0) to remain 0, got %#lx\n", bits[149]);

    DeleteDC(dc);
    DeleteObject(bmp);
}

static void test_draw_theme_text_ex_glow(HTHEME theme)
{
    HDC dc;
    HBITMAP bmp;
    DWORD *bits = NULL;
    RECT rc = { 30, 30, 120, 70 };
    RECT rc_text;
    DTTOPTS opts = { sizeof(opts) };
    HRESULT hr;
    int glow_size = 5;
    int glow_pixels = 0;
    int x, y;

    dc = create_test_dib(160, 100, &bits, &bmp);
    ok(dc != NULL && bits != NULL, "Failed to create test DIB\n");

    /* Measure glyph rectangle with DT_LEFT | DT_TOP */
    rc_text = rc;
    opts.dwFlags = DTT_CALCRECT;
    hr = DrawThemeTextEx(theme, dc, 0, 0, L"W", 1, DT_LEFT | DT_TOP | DT_CALCRECT, &rc_text, &opts);
    ok(hr == S_OK, "DrawThemeTextEx DT_CALCRECT failed: %#lx\n", hr);

    /* Clear DIB to transparent black */
    memset(bits, 0, 160 * 100 * sizeof(DWORD));

    /* Draw with glow */
    memset(&opts, 0, sizeof(opts));
    opts.dwSize = sizeof(opts);
    opts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR | DTT_GLOWSIZE;
    opts.crText = RGB(255, 255, 255);
    opts.iGlowSize = glow_size;

    hr = DrawThemeTextEx(theme, dc, 0, 0, L"W", 1, DT_LEFT | DT_TOP, &rc, &opts);
    ok(hr == S_OK, "DrawThemeTextEx with DTT_GLOWSIZE failed: %#lx\n", hr);

    /* Check pixels outside glyph box but within glow radius */
    for (y = 0; y < 100; y++)
    {
        for (x = 0; x < 160; x++)
        {
            BYTE alpha = (BYTE)(bits[y * 160 + x] >> 24);
            BOOL inside_glyph = (x >= rc_text.left && x <= rc_text.right &&
                                 y >= rc_text.top && y <= rc_text.bottom);
            int dist_x = 0, dist_y = 0;

            if (x < rc_text.left) dist_x = rc_text.left - x;
            else if (x > rc_text.right) dist_x = x - rc_text.right;

            if (y < rc_text.top) dist_y = rc_text.top - y;
            else if (y > rc_text.bottom) dist_y = y - rc_text.bottom;

            if (!inside_glyph && dist_x <= glow_size && dist_y <= glow_size)
            {
                if (alpha > 0 && alpha < 255)
                    glow_pixels++;
            }
            else if (dist_x > glow_size + 2 || dist_y > glow_size + 2)
            {
                ok(alpha == 0, "Far pixel at (%d,%d) has non-zero alpha %u\n", x, y, alpha);
            }
        }
    }

    ok(glow_pixels > 0, "Expected glow pixels with 0 < alpha < max, got %d\n", glow_pixels);

    DeleteDC(dc);
    DeleteObject(bmp);
}

static void test_draw_theme_text_ex_calcrect(HTHEME theme)
{
    HDC dc;
    HBITMAP bmp;
    DWORD *bits = NULL;
    RECT rc = { 10, 10, 200, 100 };
    RECT rc_orig = rc;
    DTTOPTS opts = { sizeof(opts) };
    HRESULT hr;
    int i;
    BOOL unchanged = TRUE;

    dc = create_test_dib(150, 100, &bits, &bmp);
    ok(dc != NULL && bits != NULL, "Failed to create test DIB\n");
    memset(bits, 0, 150 * 100 * sizeof(DWORD));

    opts.dwFlags = DTT_CALCRECT;
    hr = DrawThemeTextEx(theme, dc, 0, 0, L"MeasureOnly", -1, DT_LEFT | DT_TOP, &rc, &opts);
    ok(hr == S_OK, "DrawThemeTextEx DTT_CALCRECT failed: %#lx\n", hr);
    ok(rc.right > rc.left && rc.bottom > rc.top, "Expected non-empty rect, got %s\n", wine_dbgstr_rect(&rc));
    ok(!EqualRect(&rc, &rc_orig), "Expected rect to be updated to text bounds\n");

    for (i = 0; i < 150 * 100; i++)
    {
        if (bits[i] != 0)
        {
            unchanged = FALSE;
            break;
        }
    }
    ok(unchanged, "DIB was modified during DTT_CALCRECT\n");

    DeleteDC(dc);
    DeleteObject(bmp);
}

static void test_buffered_paint_set_alpha(void)
{
    HDC targetdc;
    HBITMAP bmp;
    DWORD *bits = NULL;
    RECT rc_buffer = { 20, 20, 60, 60 };
    RECT rc_alpha = { 20, 20, 60, 60 };
    HDC hdcPaint = NULL;
    HPAINTBUFFER hbp;
    HRESULT hr;
    int x, y;
    int alpha_255_count = 0;

    targetdc = create_test_dib(100, 100, &bits, &bmp);
    ok(targetdc != NULL && bits != NULL, "Failed to create target DIB\n");
    memset(bits, 0, 100 * 100 * sizeof(DWORD));

    hbp = BeginBufferedPaint(targetdc, &rc_buffer, BPBF_TOPDOWNDIB, NULL, &hdcPaint);
    ok(hbp != NULL, "BeginBufferedPaint failed\n");
    ok(hdcPaint != NULL, "Expected non-NULL hdcPaint\n");

    hr = BufferedPaintSetAlpha(hbp, &rc_alpha, 255);
    ok(hr == S_OK, "BufferedPaintSetAlpha failed: %#lx\n", hr);

    hr = EndBufferedPaint(hbp, TRUE);
    ok(hr == S_OK, "EndBufferedPaint failed: %#lx\n", hr);

    for (y = 20; y < 60; y++)
    {
        for (x = 20; x < 60; x++)
        {
            BYTE a = (BYTE)(bits[y * 100 + x] >> 24);
            if (a == 255)
                alpha_255_count++;
        }
    }

    ok(alpha_255_count == 40 * 40, "Expected all 1600 pixels in rect to have alpha 255, got %d\n", alpha_255_count);
    ok((bits[0] >> 24) == 0, "Expected pixel outside rect (0,0) to have alpha 0, got %lu\n", bits[0] >> 24);
    ok((bits[99 * 100 + 99] >> 24) == 0, "Expected pixel outside rect (99,99) to have alpha 0, got %lu\n", bits[99 * 100 + 99] >> 24);

    DeleteDC(targetdc);
    DeleteObject(bmp);
}

static void test_buffered_paint_erase(void)
{
    HDC targetdc;
    HBITMAP bmp;
    DWORD *bits = NULL;
    RECT rc = { 10, 10, 50, 50 };
    BP_PAINTPARAMS params = { sizeof(params), BPPF_ERASE };
    HDC hdcPaint = NULL;
    HPAINTBUFFER hbp;
    RGBQUAD *buf_bits = NULL;
    int width = 0;
    HRESULT hr;
    BOOL all_zero = TRUE;
    int i;

    targetdc = create_test_dib(100, 100, &bits, &bmp);
    ok(targetdc != NULL && bits != NULL, "Failed to create target DIB\n");
    memset(bits, 0xFF, 100 * 100 * sizeof(DWORD));

    hbp = BeginBufferedPaint(targetdc, &rc, BPBF_TOPDOWNDIB, &params, &hdcPaint);
    ok(hbp != NULL, "BeginBufferedPaint failed\n");

    hr = GetBufferedPaintBits(hbp, &buf_bits, &width);
    ok(hr == S_OK, "GetBufferedPaintBits failed: %#lx\n", hr);
    ok(buf_bits != NULL, "Expected non-NULL buf_bits\n");
    ok(width == 40, "Expected width 40, got %d\n", width);

    if (buf_bits)
    {
        DWORD *dw_bits = (DWORD *)buf_bits;
        for (i = 0; i < 40 * 40; i++)
        {
            if (dw_bits[i] != 0)
            {
                all_zero = FALSE;
                break;
            }
        }
        ok(all_zero, "Expected BPPF_ERASE to zero the buffer\n");
    }

    EndBufferedPaint(hbp, FALSE);
    DeleteDC(targetdc);
    DeleteObject(bmp);
}

static void test_dwmapi_acceptance(void)
{
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    HWND hwnd;
    HRESULT hr;
    DWMCOLORIZATIONPARAMS params;
    DWORD val, get_val;
    BOOL comp_enabled = FALSE;

    if (!hDwm)
    {
        win_skip("dwmapi.dll not available\n");
        return;
    }

    pDwmIsCompositionEnabled = (void *)GetProcAddress(hDwm, "DwmIsCompositionEnabled");
    pDwmGetWindowAttribute = (void *)GetProcAddress(hDwm, "DwmGetWindowAttribute");
    pDwmSetWindowAttribute = (void *)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    pDwmpGetColorizationParameters = (void *)GetProcAddress(hDwm, (LPCSTR)127);

    ok(pDwmIsCompositionEnabled != NULL, "DwmIsCompositionEnabled missing\n");
    ok(pDwmGetWindowAttribute != NULL, "DwmGetWindowAttribute missing\n");
    ok(pDwmSetWindowAttribute != NULL, "DwmSetWindowAttribute missing\n");
    ok(pDwmpGetColorizationParameters != NULL, "DwmpGetColorizationParameters (ord 127) missing\n");

    if (pDwmIsCompositionEnabled)
    {
        hr = pDwmIsCompositionEnabled(&comp_enabled);
        ok(hr == S_OK, "DwmIsCompositionEnabled failed: %#lx\n", hr);
    }

    if (pDwmpGetColorizationParameters)
    {
        memset(&params, 0x55, sizeof(params));
        hr = pDwmpGetColorizationParameters(&params);
        ok(hr == S_OK, "DwmpGetColorizationParameters failed: %#lx\n", hr);
        ok(params.clrColor != 0x55555555, "Expected valid clrColor, got %#lx\n", params.clrColor);
        ok(params.nIntensity <= 100, "Expected valid intensity <= 100, got %u\n", params.nIntensity);
    }

    hwnd = CreateWindowA("static", "glass_test_window", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindow failed\n");

    if (pDwmSetWindowAttribute && pDwmGetWindowAttribute)
    {
        val = 1;
        get_val = 0xdeadbeef;
        hr = pDwmSetWindowAttribute(hwnd, DWMWA_FLIP3D_POLICY, &val, sizeof(val));
        ok(hr == S_OK, "DwmSetWindowAttribute(FLIP3D_POLICY) failed: %#lx\n", hr);
        hr = pDwmGetWindowAttribute(hwnd, DWMWA_FLIP3D_POLICY, &get_val, sizeof(get_val));
        ok(hr == S_OK, "DwmGetWindowAttribute(FLIP3D_POLICY) failed: %#lx\n", hr);
        ok(get_val == 1, "Expected FLIP3D_POLICY 1, got %#lx\n", get_val);

        val = 2;
        pDwmSetWindowAttribute(hwnd, DWMWA_FLIP3D_POLICY, &val, sizeof(val));
        pDwmGetWindowAttribute(hwnd, DWMWA_FLIP3D_POLICY, &get_val, sizeof(get_val));
        ok(get_val == 2, "Expected FLIP3D_POLICY 2, got %#lx\n", get_val);

        val = 1;
        get_val = 0xdeadbeef;
        hr = pDwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &val, sizeof(val));
        ok(hr == S_OK, "DwmSetWindowAttribute(EXCLUDED_FROM_PEEK) failed: %#lx\n", hr);
        hr = pDwmGetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &get_val, sizeof(get_val));
        ok(hr == S_OK, "DwmGetWindowAttribute(EXCLUDED_FROM_PEEK) failed: %#lx\n", hr);
        ok(get_val == 1, "Expected EXCLUDED_FROM_PEEK 1, got %#lx\n", get_val);

        val = 0;
        pDwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &val, sizeof(val));
        pDwmGetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &get_val, sizeof(get_val));
        ok(get_val == 0, "Expected EXCLUDED_FROM_PEEK 0, got %#lx\n", get_val);
    }

    DestroyWindow(hwnd);
    FreeLibrary(hDwm);
}

START_TEST(glass)
{
    HTHEME theme;

    test_buffered_paint_set_alpha();
    test_buffered_paint_erase();
    test_dwmapi_acceptance();

    theme = OpenThemeData(NULL, L"Window");
    if (!theme)
        theme = OpenThemeData(NULL, L"TextStyle");
    if (!theme)
        theme = OpenThemeData(NULL, L"Button");

    if (!theme)
    {
        skip("No active theme for DrawThemeTextEx tests\n");
        return;
    }

    test_draw_theme_text_ex_composited(theme);
    test_draw_theme_text_ex_glow(theme);
    test_draw_theme_text_ex_calcrect(theme);

    CloseThemeData(theme);
}
