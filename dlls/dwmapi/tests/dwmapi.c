/*
 * Copyright 2020 Zhiyi Zhang for CodeWeavers
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

#include "dwmapi.h"
#include "wine/test.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMTRANSITION_OWNEDWINDOW_REPOSITION
#define DWMTRANSITION_OWNEDWINDOW_REPOSITION 0
#endif
#ifndef DWM_E_COMPOSITIONDISABLED
#define DWM_E_COMPOSITIONDISABLED _HRESULT_TYPEDEF_(0x80263001)
#endif

static HRESULT (WINAPI *pDwmGetGraphicsStreamTransformHint)(UINT, MilMatrix3x2D *);
static HRESULT (WINAPI *pDwmInvalidateIconicBitmaps)(HWND);
static HRESULT (WINAPI *pDwmModifyPreviousDxFrameDuration)(HWND, INT, BOOL);
static HRESULT (WINAPI *pDwmQueryThumbnailSourceSize)(HTHUMBNAIL, PSIZE);
static HRESULT (WINAPI *pDwmSetIconicLivePreviewBitmap)(HWND, HBITMAP, POINT *, DWORD);
static HRESULT (WINAPI *pDwmSetIconicThumbnail)(HWND, HBITMAP, DWORD);
static HRESULT (WINAPI *pDwmTransitionOwnedWindow)(HWND, DWORD);

static void test_DwmIsCompositionEnabled(void)
{
    BOOL enabled;
    HRESULT hr;

    hr = DwmIsCompositionEnabled(NULL);
    ok(hr == E_INVALIDARG, "Expected %#lx, got %#lx.\n", E_INVALIDARG, hr);

    enabled = -1;
    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Expected %#lx, got %#lx.\n", S_OK, hr);
    ok(enabled == TRUE || enabled == FALSE, "Got unexpected %#x.\n", enabled);
}

static void test_DwmGetCompositionTimingInfo(void)
{
    LARGE_INTEGER performance_frequency;
    int result, display_frequency;
    DWM_TIMING_INFO timing_info;
    QPC_TIME refresh_period;
    DEVMODEA mode;
    BOOL enabled, have_performance_frequency;
    HRESULT hr;

    enabled = FALSE;
    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    hr = DwmGetCompositionTimingInfo(NULL, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    memset(&timing_info, 0, sizeof(timing_info));
    hr = DwmGetCompositionTimingInfo(NULL, &timing_info);
    ok(hr == MILERR_MISMATCHED_SIZE, "Got hr %#lx.\n", hr);

    timing_info.cbSize = sizeof(timing_info);
    hr = DwmGetCompositionTimingInfo(NULL, &timing_info);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(timing_info.cbSize == sizeof(timing_info),
        "Got wrong struct size %d.\n", timing_info.cbSize);

    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    result = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode);
    ok(!!result, "Failed to get display mode %#lx.\n", GetLastError());
    have_performance_frequency = QueryPerformanceFrequency(&performance_frequency);
    ok(have_performance_frequency, "Failed to get performance counter frequency.\n");
    if (!result)
        return;
    if (!(mode.dmFields & DM_DISPLAYFREQUENCY) || mode.dmDisplayFrequency == 0)
    {
        skip("Display refresh rate is not available (fields %#lx, frequency %lu).\n",
            mode.dmFields, mode.dmDisplayFrequency);
        return;
    }
    if (!have_performance_frequency)
        return;
    display_frequency = mode.dmDisplayFrequency;
    refresh_period = performance_frequency.QuadPart / display_frequency;

    /* Windows 10 v1507 reports the display frequency over 1, newer versions the performance counter frequency
     * over the refresh period in counter ticks; both describe the same rate */
    ok(timing_info.rateRefresh.uiDenominator && timing_info.rateRefresh.uiNumerator
            && abs((int)(timing_info.rateRefresh.uiNumerator / timing_info.rateRefresh.uiDenominator)
                   - (int)display_frequency) <= 1,
            "Got wrong monitor refresh rate %lu/%lu.\n", (ULONG)timing_info.rateRefresh.uiNumerator,
            (ULONG)timing_info.rateRefresh.uiDenominator);
    ok(timing_info.rateCompose.uiDenominator && timing_info.rateCompose.uiNumerator
            && abs((int)(timing_info.rateCompose.uiNumerator / timing_info.rateCompose.uiDenominator)
                   - (int)display_frequency) <= 1,
            "Got wrong composition rate %lu/%lu.\n", (ULONG)timing_info.rateCompose.uiNumerator,
            (ULONG)timing_info.rateCompose.uiDenominator);
    ok(timing_info.qpcRefreshPeriod == refresh_period || timing_info.qpcRefreshPeriod == refresh_period + 1
            || broken(timing_info.qpcRefreshPeriod == display_frequency), /* win10 v1507 */
            "Got wrong monitor refresh period %s.\n", wine_dbgstr_longlong(timing_info.qpcRefreshPeriod));
}

static void test_DWMWA_EXTENDED_FRAME_BOUNDS(void)
{
    DPI_AWARENESS_CONTEXT (WINAPI *pSetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    DPI_AWARENESS_CONTEXT old_context = NULL;
    BOOL enabled;
    HRESULT hr;
    RECT rect, window_rect, intersection;
    HWND hwnd, child;

    pSetThreadDpiAwarenessContext = (void *)GetProcAddress(GetModuleHandleA("user32.dll"),
            "SetThreadDpiAwarenessContext");
    if (pSetThreadDpiAwarenessContext)
        old_context = pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

    hwnd = CreateWindowW(L"static", L"static", WS_OVERLAPPEDWINDOW | WS_POPUP | WS_VISIBLE, 10, 10, 200, 200, NULL, NULL, NULL, NULL);
    child = CreateWindowW(L"edit", L"edit", WS_CHILD | WS_VISIBLE, 0, 0, 50, 50, hwnd, NULL, NULL, NULL);

    DwmIsCompositionEnabled(&enabled);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (!enabled) {
        ok(hr == E_HANDLE, "Got hr %#lx.\n", hr);
        skip("DWM is disabled.\n");
        goto cleanup;
    }

    hr = DwmGetWindowAttribute(NULL, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    ok(hr == E_HANDLE, "Got hr %#lx.\n", hr);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &enabled, sizeof(enabled));
    ok(hr == E_NOT_SUFFICIENT_BUFFER, "Got hr %#lx.\n", hr);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, NULL, 0);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = DwmGetWindowAttribute(child, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    ok(hr == E_HANDLE, "Got hr %#lx.\n", hr);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    /* Window rectangle covers extended frame */
    GetWindowRect(hwnd, &window_rect);
    IntersectRect(&intersection, &window_rect, &rect);
    ok(EqualRect(&intersection, &rect), "Got wrong frame %s, window %s.\n", wine_dbgstr_rect(&rect), wine_dbgstr_rect(&window_rect));

    /* Extended frame bounds aren't adjusted for DPI */
    if (pSetThreadDpiAwarenessContext) {
        RECT unaware_rect;
        pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
        hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &unaware_rect, sizeof(unaware_rect));
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(EqualRect(&rect, &unaware_rect), "Expected %s, got %s.\n", wine_dbgstr_rect(&rect), wine_dbgstr_rect(&unaware_rect));
    }

cleanup:
    if (pSetThreadDpiAwarenessContext)
        pSetThreadDpiAwarenessContext(old_context);
    DestroyWindow(child);
    DestroyWindow(hwnd);
}

static void WINAPI apc_func(ULONG_PTR apc_count)
{
    ++*(unsigned int *)apc_count;
}

static void test_DwmFlush(void)
{
    unsigned int apc_count;
    BOOL enabled = FALSE;
    HRESULT hr;
    BOOL ret;

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }
    ret = QueueUserAPC(apc_func, GetCurrentThread(), (ULONG_PTR)&apc_count);
    ok(ret, "QueueUserAPC returned %d\n", ret);
    apc_count = 0;
    hr = DwmFlush();
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(!apc_count, "got apc_count %d.\n", apc_count);
    SleepEx(0, TRUE);
    ok(apc_count == 1, "got apc_count %d.\n", apc_count);
}

static void test_window_attributes(void)
{
    RECT rect, window_rect;
    BOOL enabled = FALSE;
    DWORD val;
    HWND hwnd;
    HRESULT hr;

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    hwnd = CreateWindowExA(0, "static", "dwmapi test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 300, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");

    val = 1;
    hr = DwmSetWindowAttribute(NULL, DWMWA_USE_IMMERSIVE_DARK_MODE, &val, sizeof(val));
    ok(FAILED(hr), "Got hr %#lx.\n", hr);
    hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, NULL, sizeof(val));
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &val, sizeof(val) - 1);
    ok(FAILED(hr), "Got hr %#lx.\n", hr);

    hr = DwmGetWindowAttribute(NULL, DWMWA_NCRENDERING_ENABLED, &enabled, sizeof(enabled));
    ok(FAILED(hr), "Got hr %#lx.\n", hr);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED, NULL, sizeof(enabled));
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED, &enabled, sizeof(enabled) - 1);
    ok(FAILED(hr), "Got hr %#lx.\n", hr);
    hr = DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &rect, sizeof(rect) - 1);
    ok(FAILED(hr), "Got hr %#lx.\n", hr);

    val = 1;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    val = 0;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(val == 1, "Got %lu.\n", val);

    val = 2;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    val = 0;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(val == 2, "Got %lu.\n", val);

    val = 0xdeadbeef;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(val == 0, "Got %lu.\n", val);
    val = 2;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    val = 0;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(val == 2, "Got %lu.\n", val);

    val = 2;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    val = 0xdeadbeef;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &val, sizeof(val));
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    ok(val == 0xdeadbeef, "Got %#lx.\n", val);

    val = 1;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_ALLOW_NCPAINT, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    val = 0xdeadbeef;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_ALLOW_NCPAINT, &val, sizeof(val));
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    ok(val == 0xdeadbeef, "Got %#lx.\n", val);

    enabled = FALSE;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED, &enabled, sizeof(enabled));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(enabled == TRUE, "Got %d.\n", enabled);

    val = 0xdeadbeef;
    hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &val, sizeof(val));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(val == 0, "Got %#lx.\n", val);

    GetWindowRect(hwnd, &window_rect);
    memset(&rect, 0, sizeof(rect));
    hr = DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &rect, sizeof(rect));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(rect.right > rect.left && rect.bottom > rect.top,
            "Got empty caption button bounds %s.\n", wine_dbgstr_rect(&rect));
    ok(rect.left >= 0 && rect.top >= 0
            && rect.right <= window_rect.right - window_rect.left
            && rect.bottom <= window_rect.bottom - window_rect.top,
            "Got caption button bounds %s outside window %s.\n",
            wine_dbgstr_rect(&rect), wine_dbgstr_rect(&window_rect));

    DestroyWindow(hwnd);
}

static void test_colorization(void)
{
    typedef struct
    {
        COLORREF color, afterglow;
        UINT intensity, afterglow_balance, blur_balance, reflection_intensity;
        BOOL opaque;
    } test_colorization_params;
    HRESULT (WINAPI *get_params)(void *);
    test_colorization_params params;
    BOOL enabled = FALSE, opaque = -1;
    DWORD color = 0;
    HRESULT hr;

    ok(DwmGetColorizationColor(NULL, &opaque) == E_INVALIDARG, "Expected E_INVALIDARG for NULL color.\n");
    ok(DwmGetColorizationColor(&color, NULL) == E_INVALIDARG, "Expected E_INVALIDARG for NULL opaque flag.\n");
    get_params = (void *)GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), (LPCSTR)127);  /* exported by ordinal only, also natively */
    ok(get_params != NULL, "DwmGetColorizationParameters export is missing.\n");
    if (get_params)
    {
        hr = get_params(&params);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        hr = DwmGetColorizationColor(&color, &opaque);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
    }

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    hr = DwmGetColorizationColor(&color, &opaque);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(opaque == TRUE || opaque == FALSE, "Got opaque %d.\n", opaque);
    ok(color != 0, "Got color %#lx.\n", color);
}

static void test_blur_behind(void)
{
    DWM_BLURBEHIND bb;
    BOOL enabled = FALSE;
    HWND hwnd;
    HRESULT hr;

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    hwnd = CreateWindowExA(0, "static", "dwmapi test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 300, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");

    memset(&bb, 0, sizeof(bb));
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = TRUE;
    hr = DwmEnableBlurBehindWindow(hwnd, &bb);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.hRgnBlur = CreateRectRgn(0, 0, 50, 50);
    hr = DwmEnableBlurBehindWindow(hwnd, &bb);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    DeleteObject(bb.hRgnBlur);

    bb.dwFlags = DWM_BB_BLURREGION;
    bb.hRgnBlur = NULL;
    hr = DwmEnableBlurBehindWindow(hwnd, &bb);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = FALSE;
    hr = DwmEnableBlurBehindWindow(hwnd, &bb);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    DestroyWindow(hwnd);
}

static void test_extend_frame(void)
{
    MARGINS margins = { 0, 0, 0, 0 };
    BOOL enabled = FALSE;
    HWND hwnd;
    HRESULT hr;

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    hwnd = CreateWindowExA(0, "static", "dwmapi test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 300, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");

    hr = DwmExtendFrameIntoClientArea(hwnd, &margins);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    margins.cxLeftWidth = -1;
    margins.cxRightWidth = -1;
    margins.cyTopHeight = -1;
    margins.cyBottomHeight = -1;
    hr = DwmExtendFrameIntoClientArea(hwnd, &margins);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    DestroyWindow(hwnd);
}

static void test_thumbnails(void)
{
    DWM_THUMBNAIL_PROPERTIES props;
    RECT win_rect;
    BOOL enabled = FALSE;
    HTHUMBNAIL thumb;
    HWND dest, src;
    HRESULT hr;
    SIZE size;

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    dest = CreateWindowExA(0, "static", "dwmapi dest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 300, 200, NULL, NULL, NULL, NULL);
    src = CreateWindowExA(0, "static", "dwmapi src", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            150, 150, 240, 160, NULL, NULL, NULL, NULL);
    ok(dest != NULL && src != NULL, "Failed to create windows.\n");

    thumb = NULL;
    hr = DwmRegisterThumbnail(NULL, src, &thumb);
    ok(FAILED(hr), "Got hr %#lx.\n", hr);

    thumb = NULL;
    hr = DwmRegisterThumbnail(dest, src, &thumb);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(thumb != NULL, "Got NULL thumbnail handle.\n");

    memset(&props, 0, sizeof(props));
    props.dwFlags = DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
    props.opacity = 200;
    props.fVisible = TRUE;
    hr = DwmUpdateThumbnailProperties(thumb, &props);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    if (pDwmQueryThumbnailSourceSize)
    {
        /* the source size is the visible frame of the window */
        hr = DwmGetWindowAttribute(src, DWMWA_EXTENDED_FRAME_BOUNDS, &win_rect, sizeof(win_rect));
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        memset(&size, 0, sizeof(size));
        hr = pDwmQueryThumbnailSourceSize(thumb, &size);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(size.cx > 0 && size.cy > 0, "Got size %ldx%ld.\n", size.cx, size.cy);
        ok(size.cx == win_rect.right - win_rect.left && size.cy == win_rect.bottom - win_rect.top,
                "Got size %ldx%ld, frame bounds %s.\n", size.cx, size.cy, wine_dbgstr_rect(&win_rect));
    }
    else
    {
        win_skip("DwmQueryThumbnailSourceSize is not available.\n");
    }

    hr = DwmUnregisterThumbnail(thumb);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hr = DwmUpdateThumbnailProperties(thumb, &props);
    ok(FAILED(hr), "Got hr %#lx.\n", hr);

    DestroyWindow(src);
    DestroyWindow(dest);
}

static void test_iconic(void)
{
    HBITMAP (WINAPI *pCreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
    BOOL (WINAPI *pDeleteObject)(HGDIOBJ);
    BOOL enabled = FALSE;
    BITMAPINFO bmi;
    HMODULE gdi32;
    HBITMAP hbmp;
    void *bits;
    HWND hwnd;
    HRESULT hr;

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    if (!pDwmInvalidateIconicBitmaps || !pDwmSetIconicThumbnail || !pDwmSetIconicLivePreviewBitmap)
    {
        win_skip("Iconic bitmap functions are not available.\n");
        return;
    }

    gdi32 = GetModuleHandleA("gdi32.dll");
    pCreateDIBSection = (void *)GetProcAddress(gdi32, "CreateDIBSection");
    pDeleteObject = (void *)GetProcAddress(gdi32, "DeleteObject");
    if (!pCreateDIBSection || !pDeleteObject)
    {
        win_skip("GDI bitmap functions are not available.\n");
        return;
    }

    hwnd = CreateWindowExA(0, "static", "dwmapi test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 300, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");

    hr = pDwmInvalidateIconicBitmaps(hwnd);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bits = NULL;
    hbmp = pCreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ok(hbmp != NULL, "Failed to create DIB section.\n");

    hr = pDwmSetIconicThumbnail(hwnd, hbmp, 0);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    hr = pDwmSetIconicLivePreviewBitmap(hwnd, hbmp, NULL, 0);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    /* a window that asked for iconic representation may supply its picture */
    enabled = TRUE;
    hr = DwmSetWindowAttribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &enabled, sizeof(enabled));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = DwmSetWindowAttribute(hwnd, DWMWA_HAS_ICONIC_BITMAP, &enabled, sizeof(enabled));
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = pDwmSetIconicThumbnail(hwnd, hbmp, 0);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = pDwmSetIconicLivePreviewBitmap(hwnd, hbmp, NULL, DWM_SIT_DISPLAYFRAME);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = pDwmInvalidateIconicBitmaps(hwnd);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    pDeleteObject(hbmp);
    DestroyWindow(hwnd);
}

static void test_misc(void)
{
    MilMatrix3x2D matrix;
    BOOL enabled = FALSE;
    HWND hwnd;
    HRESULT hr;

    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (!enabled)
    {
        skip("DWM is disabled.\n");
        return;
    }

    hwnd = CreateWindowExA(0, "static", "dwmapi test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 300, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");

    if (pDwmGetGraphicsStreamTransformHint)
    {
        memset(&matrix, 0, sizeof(matrix));
        hr = pDwmGetGraphicsStreamTransformHint(0, &matrix);
        ok(hr == DWM_E_COMPOSITIONDISABLED, "Got hr %#lx.\n", hr);
    }
    else
    {
        win_skip("DwmGetGraphicsStreamTransformHint is not available.\n");
    }

    if (pDwmModifyPreviousDxFrameDuration)
    {
        hr = pDwmModifyPreviousDxFrameDuration(hwnd, 1, FALSE);
        ok(hr == E_NOTIMPL, "Got hr %#lx.\n", hr);
    }
    else
    {
        win_skip("DwmModifyPreviousDxFrameDuration is not available.\n");
    }

    if (pDwmTransitionOwnedWindow)
    {
        hr = pDwmTransitionOwnedWindow(hwnd, DWMTRANSITION_OWNEDWINDOW_REPOSITION);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    }
    else
    {
        win_skip("DwmTransitionOwnedWindow is not available.\n");
    }

    hr = DwmEnableComposition(DWM_EC_DISABLECOMPOSITION);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    enabled = FALSE;
    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(enabled == TRUE, "Got enabled %d.\n", enabled);

    hr = DwmEnableComposition(DWM_EC_ENABLECOMPOSITION);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    enabled = FALSE;
    hr = DwmIsCompositionEnabled(&enabled);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(enabled == TRUE, "Got enabled %d.\n", enabled);

    DestroyWindow(hwnd);
}

START_TEST(dwmapi)
{
    HMODULE dwmapi = GetModuleHandleA("dwmapi.dll");

    pDwmGetGraphicsStreamTransformHint = (void *)GetProcAddress(dwmapi, "DwmGetGraphicsStreamTransformHint");
    pDwmInvalidateIconicBitmaps = (void *)GetProcAddress(dwmapi, "DwmInvalidateIconicBitmaps");
    pDwmModifyPreviousDxFrameDuration = (void *)GetProcAddress(dwmapi, "DwmModifyPreviousDxFrameDuration");
    pDwmQueryThumbnailSourceSize = (void *)GetProcAddress(dwmapi, "DwmQueryThumbnailSourceSize");
    pDwmSetIconicLivePreviewBitmap = (void *)GetProcAddress(dwmapi, "DwmSetIconicLivePreviewBitmap");
    pDwmSetIconicThumbnail = (void *)GetProcAddress(dwmapi, "DwmSetIconicThumbnail");
    pDwmTransitionOwnedWindow = (void *)GetProcAddress(dwmapi, "DwmTransitionOwnedWindow");

    test_DwmIsCompositionEnabled();
    test_DwmGetCompositionTimingInfo();
    test_DWMWA_EXTENDED_FRAME_BOUNDS();
    test_DwmFlush();
    test_window_attributes();
    test_colorization();
    test_blur_behind();
    test_extend_frame();
    test_thumbnails();
    test_iconic();
    test_misc();
}
