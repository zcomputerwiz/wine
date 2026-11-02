/*
 * WineExplorer Desktop Environment - dwm_clock.c
 *
 * Conformance tests for the compositor frame clock (Phase P6 slice 1): DwmFlush paces on composed
 * frames and DwmGetCompositionTimingInfo reports them; the frame counter never decreases.
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "dwmapi.h"
#include "wine/dwm_protocol.h"
#include "wine/test.h"

START_TEST(dwm_clock)
{
    DWM_TIMING_INFO info1, info2;
    HRESULT hr;
    BOOL have_clock;
    WCHAR desk_name[MAX_PATH], name[MAX_PATH + 64];
    HDESK desk;
    HANDLE mapping;

    /* DwmFlush must succeed whether or not a frame clock is present */
    hr = DwmFlush();
    ok(hr == S_OK, "DwmFlush failed: %#lx\n", hr);

    /* DwmGetCompositionTimingInfo must succeed whether or not a frame clock is present */
    memset(&info1, 0, sizeof(info1));
    info1.cbSize = sizeof(info1);
    hr = DwmGetCompositionTimingInfo(NULL, &info1);
    ok(hr == S_OK, "DwmGetCompositionTimingInfo failed: %#lx\n", hr);

    /* the frame checks need the per-desktop clock page the shell creates */
    have_clock = FALSE;
    if ((desk = GetThreadDesktop(GetCurrentThreadId())))
    {
        if (GetUserObjectInformationW(desk, UOI_NAME, desk_name, ARRAY_SIZE(desk_name), NULL))
        {
            lstrcpynW(name, DWM_CLOCK_NAME, ARRAY_SIZE(name));
            lstrcatW(name, desk_name);
            if ((mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name)))
            {
                have_clock = TRUE;
                CloseHandle(mapping);
            }
        }
    }
    if (!have_clock)
    {
        skip("no frame clock page (composition off or clock disabled)\n");
        return;
    }

    /* two calls 100 ms apart must never decrease cFrame */
    Sleep(100);
    memset(&info2, 0, sizeof(info2));
    info2.cbSize = sizeof(info2);
    hr = DwmGetCompositionTimingInfo(NULL, &info2);
    ok(hr == S_OK, "DwmGetCompositionTimingInfo failed: %#lx\n", hr);
    ok(info2.cFrame >= info1.cFrame, "cFrame decreased: %I64u -> %I64u\n",
       (unsigned long long)info1.cFrame, (unsigned long long)info2.cFrame);

    /* repainting a visible window makes the compositor compose frames; DwmFlush returns promptly */
    {
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"static", L"clock", WS_POPUP | WS_VISIBLE,
                                    50, 50, 200, 200, NULL, NULL, NULL, NULL);
        LARGE_INTEGER start, end, freq, now;
        HBRUSH brushes[2] = { GetStockObject(BLACK_BRUSH), GetStockObject(WHITE_BRUSH) };
        RECT rc = { 0, 0, 200, 200 };
        MSG msg;
        int i;

        QueryPerformanceFrequency(&freq);
        for (i = 0; i < 20; i++)
        {
            HDC dc = GetDC(hwnd);
            FillRect(dc, &rc, brushes[i & 1]);
            ReleaseDC(hwnd, dc);
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            QueryPerformanceCounter(&start);
            hr = DwmFlush();
            QueryPerformanceCounter(&end);
            ok(hr == S_OK, "DwmFlush failed: %#lx\n", hr);
            ok((end.QuadPart - start.QuadPart) * 1000 / freq.QuadPart < 200, "DwmFlush took %I64d ms\n",
               (end.QuadPart - start.QuadPart) * 1000 / freq.QuadPart);
        }
        hr = DwmGetCompositionTimingInfo(NULL, &info2);
        ok(hr == S_OK, "DwmGetCompositionTimingInfo failed: %#lx\n", hr);
        QueryPerformanceCounter(&now);
        ok(info2.cFrame > info1.cFrame, "no composed frames: %I64u -> %I64u\n",
           (unsigned long long)info1.cFrame, (unsigned long long)info2.cFrame);
        ok(info2.qpcCompose && info2.qpcCompose <= now.QuadPart, "qpcCompose %I64u, now %I64d\n",
           (unsigned long long)info2.qpcCompose, now.QuadPart);
        trace("frames %I64u -> %I64u, refresh period %I64u\n", (unsigned long long)info1.cFrame,
              (unsigned long long)info2.cFrame, (unsigned long long)info2.qpcRefreshPeriod);
        DestroyWindow(hwnd);
    }
}
