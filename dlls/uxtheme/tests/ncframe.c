/*
 * WineExplorer Desktop Environment - ncframe.c
 *
 * Conformance tests for DWM non-client modern frame effects (Phase P10a).
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

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "dwmapi.h"
#include "wine/test.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

static HRESULT (WINAPI *pDwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
static HRESULT (WINAPI *pDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

static BOOL init_dwmapi(void)
{
    HMODULE hmod = LoadLibraryA("dwmapi.dll");
    if (!hmod) return FALSE;
    pDwmGetWindowAttribute = (void *)GetProcAddress(hmod, "DwmGetWindowAttribute");
    pDwmSetWindowAttribute = (void *)GetProcAddress(hmod, "DwmSetWindowAttribute");
    return pDwmGetWindowAttribute && pDwmSetWindowAttribute;
}

static BOOL get_gate_status(void)
{
    HKEY hkey;
    DWORD val = 0, size = sizeof(val), type;
    BOOL enabled = FALSE;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\WineExplorer\\Shell", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExA(hkey, "DwmFrameEffects", NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS && type == REG_DWORD)
            enabled = (val != 0);
        RegCloseKey(hkey);
    }
    return enabled;
}

static LRESULT WINAPI test_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void register_test_class(void)
{
    WNDCLASSA cls = {0};
    cls.lpfnWndProc = test_wndproc;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "NCFrameTestClass";
    cls.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&cls);
}

static void flush_events(void)
{
    MSG msg;
    int diff = 200;
    DWORD time = GetTickCount() + diff;

    while (diff > 0)
    {
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT) == WAIT_TIMEOUT) break;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        diff = time - GetTickCount();
    }
}

static void test_gate_off(void)
{
    HWND hwnd;
    RECT rc = {0, 0, 300, 200};
    int cy_frame = GetSystemMetrics(SM_CYFRAME);
    int cx_frame = GetSystemMetrics(SM_CXFRAME);
    int cy_caption = GetSystemMetrics(SM_CYCAPTION);
    LRESULT hit;

    ok(cy_frame > 0, "Expected SM_CYFRAME > 0\n");
    ok(cx_frame > 0, "Expected SM_CXFRAME > 0\n");
    ok(cy_caption > 0, "Expected SM_CYCAPTION > 0\n");

    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    ok(rc.top == -cy_frame - cy_caption, "Expected top = %d, got %ld\n", -cy_frame - cy_caption, rc.top);
    ok(rc.left == -cx_frame, "Expected left = %d, got %ld\n", -cx_frame, rc.left);
    ok(rc.right == 300 + cx_frame, "Expected right = %d, got %ld\n", 300 + cx_frame, rc.right);
    ok(rc.bottom == 200 + cy_frame, "Expected bottom = %d, got %ld\n", 200 + cy_frame, rc.bottom);

    hwnd = CreateWindowExA(0, "NCFrameTestClass", "GateOffTest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 400, 300, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window\n");
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    flush_events();

    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 50, 100 + cy_frame + cy_caption / 2));
    ok(hit == HTCAPTION, "Expected HTCAPTION with gate off, got %ld\n", (long)hit);

    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 1, 100 + 1));
    ok(hit == HTTOPLEFT, "Expected HTTOPLEFT with gate off, got %ld\n", (long)hit);

    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 200, 100 + cy_frame + cy_caption + 50));
    ok(hit == HTCLIENT, "Expected HTCLIENT with gate off, got %ld\n", (long)hit);

    DestroyWindow(hwnd);
}

static void test_gate_on_geometry(void)
{
    HWND hwnd;
    RECT btn_bounds = {0}, ext_bounds = {0};
    HRESULT hr;
    int btn_w = GetSystemMetrics(SM_CXSIZE);
    int top_h = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYCAPTION);

    hwnd = CreateWindowExA(0, "NCFrameTestClass", "GateOnGeomTest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 400, 300, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window\n");
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    flush_events();

    hr = pDwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &btn_bounds, sizeof(btn_bounds));
    ok(hr == S_OK, "DwmGetWindowAttribute DWMWA_CAPTION_BUTTON_BOUNDS failed: %08lx\n", hr);
    ok(btn_bounds.top == 0, "Expected btn top == 0, got %ld\n", btn_bounds.top);
    ok(btn_bounds.bottom == top_h, "Expected btn bottom == %d, got %ld\n", top_h, btn_bounds.bottom);
    ok(btn_bounds.right == 400, "Expected btn right == 400, got %ld\n", btn_bounds.right);
    ok(btn_bounds.left == 400 - 3 * btn_w, "Expected btn left == %d, got %ld\n", 400 - 3 * btn_w, btn_bounds.left);

    hr = pDwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &ext_bounds, sizeof(ext_bounds));
    ok(hr == S_OK, "DwmGetWindowAttribute DWMWA_EXTENDED_FRAME_BOUNDS failed: %08lx\n", hr);
    ok(ext_bounds.left == 100, "Expected ext left == 100, got %ld\n", ext_bounds.left);
    ok(ext_bounds.top == 100, "Expected ext top == 100, got %ld\n", ext_bounds.top);
    ok(ext_bounds.right == 500, "Expected ext right == 500, got %ld\n", ext_bounds.right);
    ok(ext_bounds.bottom == 400, "Expected ext bottom == 400, got %ld\n", ext_bounds.bottom);

    DestroyWindow(hwnd);
}

static void test_gate_on_hit_test(void)
{
    HWND hwnd;
    LRESULT hit;
    int btn_w = GetSystemMetrics(SM_CXSIZE);
    int top_h = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYCAPTION);

    hwnd = CreateWindowExA(0, "NCFrameTestClass", "GateOnHitTest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 400, 300, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window\n");
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    flush_events();

    /* Close button: rightmost [400 - btn_w, 400] */
    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 400 - btn_w / 2, 100 + top_h / 2));
    ok(hit == HTCLOSE, "Expected HTCLOSE, got %ld\n", (long)hit);

    /* Max button: [400 - 2 * btn_w, 400 - btn_w] */
    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 400 - btn_w - btn_w / 2, 100 + top_h / 2));
    ok(hit == HTMAXBUTTON, "Expected HTMAXBUTTON, got %ld\n", (long)hit);

    /* Min button: [400 - 3 * btn_w, 400 - 2 * btn_w] */
    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 400 - 2 * btn_w - btn_w / 2, 100 + top_h / 2));
    ok(hit == HTMINBUTTON, "Expected HTMINBUTTON, got %ld\n", (long)hit);

    /* Caption */
    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 50, 100 + top_h / 2));
    ok(hit == HTCAPTION, "Expected HTCAPTION, got %ld\n", (long)hit);

    /* Sizing borders */
    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 1, 100 + 1));
    ok(hit == HTTOPLEFT, "Expected HTTOPLEFT, got %ld\n", (long)hit);

    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 200, 100 + 1));
    ok(hit == HTTOP, "Expected HTTOP, got %ld\n", (long)hit);

    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 200, 100 + 300 - 1));
    ok(hit == HTBOTTOM, "Expected HTBOTTOM, got %ld\n", (long)hit);

    DestroyWindow(hwnd);

    /* Tool window */
    hwnd = CreateWindowExA(WS_EX_TOOLWINDOW, "NCFrameTestClass", "ToolWinTest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 400, 300, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create tool window\n");
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    flush_events();

    top_h = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYSMCAPTION);

    /* Tool window close button */
    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 400 - btn_w / 2, 100 + top_h / 2));
    ok(hit == HTCLOSE, "Expected HTCLOSE for tool window, got %ld\n", (long)hit);

    /* Position where max button would be is caption in tool window */
    hit = SendMessageA(hwnd, WM_NCHITTEST, 0, MAKELPARAM(100 + 400 - 2 * btn_w, 100 + top_h / 2));
    ok(hit == HTCAPTION, "Expected HTCAPTION for tool window max slot, got %ld\n", (long)hit);

    DestroyWindow(hwnd);
}

static void test_dark_mode_and_caption_color(void)
{
    HWND hwnd;
    HDC hdc;
    RECT rect;
    HRESULT hr;
    COLORREF col, pixel;
    BOOL dark;

    hwnd = CreateWindowExA(0, "NCFrameTestClass", "", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 400, 300, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create visible window\n");

    SetForegroundWindow(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    flush_events();

    /* 1. Set explicit red caption color */
    col = RGB(255, 0, 0);
    hr = pDwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &col, sizeof(col));
    ok(hr == S_OK, "DwmSetWindowAttribute DWMWA_CAPTION_COLOR failed: %08lx\n", hr);

    SendMessageA(hwnd, WM_NCPAINT, 1, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    UpdateWindow(hwnd);
    flush_events();

    /* A window DC can contain caption pixels outside the presented surface. */
    ok(GetWindowRect(hwnd, &rect), "GetWindowRect failed\n");
    hdc = GetDC(NULL);
    ok(hdc != NULL, "GetDC(NULL) failed\n");
    pixel = GetPixel(hdc, rect.left + 150, rect.top + 10);
    ReleaseDC(NULL, hdc);
    trace("Red caption test: screen pixel at caption (150,10) = %08lx (R=%d G=%d B=%d)\n",
          pixel, GetRValue(pixel), GetGValue(pixel), GetBValue(pixel));
    ok(pixel == RGB(255, 0, 0), "Expected red caption RGB(255,0,0), got %08lx\n", pixel);

    /* 2. Reset caption color to default and enable dark mode */
    col = DWMWA_COLOR_DEFAULT;
    pDwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &col, sizeof(col));

    dark = TRUE;
    hr = pDwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ok(hr == S_OK, "DwmSetWindowAttribute DWMWA_USE_IMMERSIVE_DARK_MODE failed: %08lx\n", hr);

    SendMessageA(hwnd, WM_NCPAINT, 1, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    UpdateWindow(hwnd);
    flush_events();

    ok(GetWindowRect(hwnd, &rect), "GetWindowRect failed\n");
    hdc = GetDC(NULL);
    ok(hdc != NULL, "GetDC(NULL) failed\n");
    pixel = GetPixel(hdc, rect.left + 150, rect.top + 10);
    ReleaseDC(NULL, hdc);
    trace("Dark mode test: screen pixel at caption (150,10) = %08lx (R=%d G=%d B=%d)\n",
          pixel, GetRValue(pixel), GetGValue(pixel), GetBValue(pixel));
    ok(pixel != CLR_INVALID && GetRValue(pixel) < 100 && GetGValue(pixel) < 100 && GetBValue(pixel) < 100,
       "Expected dark caption pixel (RGB < 100), got %08lx (R=%d G=%d B=%d)\n",
       pixel, GetRValue(pixel), GetGValue(pixel), GetBValue(pixel));

    DestroyWindow(hwnd);
}

START_TEST(ncframe)
{
    if (!init_dwmapi())
    {
        skip("dwmapi.dll functions not available\n");
        return;
    }

    register_test_class();

    if (!get_gate_status())
    {
        test_gate_off();
        skip("DwmFrameEffects is disabled: skipping modern NC tests\n");
        return;
    }

    test_gate_on_geometry();
    test_gate_on_hit_test();
    test_dark_mode_and_caption_color();
}
