/*
 * Dwmapi
 *
 * Copyright 2007 Andras Kovacs
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
 *
 */

#include <stdarg.h>
#include <stdio.h>

#include "winternl.h"
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winuser.h"
#include "dwmapi.h"
#include "wine/debug.h"
#include "wine/dwm_protocol.h"
#include "dwm_client.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwmapi);

#ifndef DWMWA_MICA_EFFECT
#define DWMWA_MICA_EFFECT 1029
#endif

#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

#define THUMBNAIL_TABLE_SIZE 64
#define ATTRIBUTE_TABLE_SIZE 256

typedef struct
{
    HTHUMBNAIL handle;
    HWND dest;
    HWND src;
    DWM_THUMBNAIL_PROPERTIES props;
} THUMBNAIL_ENTRY;

typedef struct
{
    HWND hwnd;
    DWORD attribute;
    DWORD value;
} ATTRIBUTE_ENTRY;

static THUMBNAIL_ENTRY thumbnail_table[THUMBNAIL_TABLE_SIZE];
/* A future DWM service will own this state; until then it is kept in a process-local table. */
static ATTRIBUTE_ENTRY attribute_table[ATTRIBUTE_TABLE_SIZE];
static DWORD thumbnail_next_handle;

static CRITICAL_SECTION thumbnail_cs;
static CRITICAL_SECTION_DEBUG thumbnail_cs_debug = {
    0, 0, &thumbnail_cs,
    { &thumbnail_cs_debug.ProcessLocksList,
      &thumbnail_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)"dwmapi thumbnail_cs" }
};
static CRITICAL_SECTION thumbnail_cs = { &thumbnail_cs_debug, -1, 0, 0, 0, 0 };

static BOOL get_stored_attribute(HWND hwnd, DWORD attribute, DWORD *value)
{
    DWORD stored;
    int i;
    BOOL found = FALSE;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < ATTRIBUTE_TABLE_SIZE; i++)
    {
        if (attribute_table[i].hwnd == hwnd && attribute_table[i].attribute == attribute)
        {
            stored = attribute_table[i].value;
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (found)
        *value = stored;
    return found;
}

/* The broker, when there is one, holds the canonical value, so a setter in another process is seen. The local
 * table is a mirror: it answers when there is no broker, and it refills a broker that lost its state. */
static BOOL get_attribute_value(HWND hwnd, DWORD attribute, DWORD *value)
{
    BOOL found = FALSE;
    DWORD brokered;

    if (!dwm_client_get_attr(hwnd, attribute, &found, &brokered))
    {
        if (found)
        {
            *value = brokered;
            return TRUE;
        }
        if (get_stored_attribute(hwnd, attribute, value))
        {
            dwm_client_set_attr(hwnd, attribute, *value);
            return TRUE;
        }
        return FALSE;
    }
    return get_stored_attribute(hwnd, attribute, value);
}


/* The development gates (docs/DWM_ARCHITECTURE.md), read once per process: DwmAlpha (glass), DwmFrameEffects (the
 * non-client frame is glass too) and DwmMaterials (blur and accents; without it they make nothing glass). */
static BOOL read_gate(const WCHAR *name)
{
    DWORD value = 0, cb = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", name, RRF_RT_REG_DWORD, NULL,
                        &value, &cb) == ERROR_SUCCESS && value != 0;
}

static BOOL dwm_composition_is_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        DWORD dwm_srv = 0, comp = 0, cb = sizeof(DWORD);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"DwmService",
                         RRF_RT_REG_DWORD, NULL, &dwm_srv, &cb) == ERROR_SUCCESS &&
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"Composition",
                         RRF_RT_REG_DWORD, NULL, &comp, &cb) == ERROR_SUCCESS)
        {
            enabled = (dwm_srv != 0 && comp != 0);
        }
        else
        {
            RTL_OSVERSIONINFOEXW version;
            version.dwOSVersionInfoSize = sizeof(version);
            if (!RtlGetVersion(&version))
                enabled = (version.dwMajorVersion > 6 || (version.dwMajorVersion == 6 && version.dwMinorVersion >= 3));
            else
                enabled = 0;
        }
    }
    return enabled != 0;
}

/**********************************************************************
 *           DwmIsCompositionEnabled         (DWMAPI.@)
 */
HRESULT WINAPI DwmIsCompositionEnabled(BOOL *enabled)
{
    TRACE("%p\n", enabled);

    if (!enabled)
        return E_INVALIDARG;

    *enabled = dwm_composition_is_enabled();

    return S_OK;
}

/**********************************************************************
 *           DwmEnableComposition         (DWMAPI.102)
 */
HRESULT WINAPI DwmEnableComposition(UINT uCompositionAction)
{
    FIXME("(%d) stub\n", uCompositionAction);
    SendNotifyMessageW(HWND_BROADCAST, WM_DWMCOMPOSITIONCHANGED, 0, 0);

    return S_OK;
}

static BOOL dwm_alpha_is_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = read_gate(L"DwmAlpha");
    return enabled;
}

static BOOL dwm_frame_is_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = read_gate(L"DwmFrameEffects");
    return enabled;
}

static BOOL dwm_materials_is_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = read_gate(L"DwmMaterials");
    return enabled;
}

/* DwmFrameStyle (P13): 0 the Windows 10/11 frame, 1 the Vista/7 Aero frame (every captioned window glass, blurred).
 * Read each time: the theme changes it while windows live. */
static DWORD dwm_frame_style(void)
{
    DWORD value = 0, cb = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"DwmFrameStyle",
                     RRF_RT_REG_DWORD, NULL, &value, &cb) != ERROR_SUCCESS || value > 2)
        return 0;
    return value;
}

/* Set or clear the glass props on a same-process window and force a repaint. With the DwmFrameEffects gate on,
 * the window stays glass (and the frame is glass too) even with all-zero margins. */
static const WCHAR margin_props[4][32] =
{
    L"__wine_dwm_margin_left",
    L"__wine_dwm_margin_right",
    L"__wine_dwm_margin_top",
    L"__wine_dwm_margin_bottom"
};

static void clear_auto_blur(HWND hwnd);

static void set_glass(HWND hwnd, const MARGINS *margins)
{
    int values[4] = { margins->cxLeftWidth, margins->cxRightWidth,
                      margins->cyTopHeight, margins->cyBottomHeight };
    DWORD pid;
    BOOL any = FALSE;
    int i;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return;

    for (i = 0; i < 4; i++)
    {
        if (values[i] < -1) values[i] = -1;
        if (values[i] > 32767) values[i] = 32767;
        if (values[i] != 0) any = TRUE;
    }

    if (dwm_frame_is_enabled() && dwm_frame_style() == 2)
    {
        /* flat8 is opaque: publish its compositor kind without making the surface a glass sheet. */
        for (i = 0; i < 4; i++) RemovePropW(hwnd, margin_props[i]);
        RemovePropW(hwnd, L"__wine_dwm_alpha");
        SetPropW(hwnd, L"__wine_dwm_frame", (HANDLE)3);
    }
    else if (any || dwm_frame_is_enabled())
    {
        for (i = 0; i < 4; i++)
        {
            /* -1 (sheet of glass) would store as 0, which reads as absent, so it is stored as 0x10000. */
            LONG_PTR stored = (values[i] == -1) ? 0x10000 : (values[i] + 1);
            SetPropW(hwnd, margin_props[i], (HANDLE)(LONG_PTR)stored);
        }
        SetPropW(hwnd, L"__wine_dwm_alpha", (HANDLE)1);
        if (dwm_frame_is_enabled())
            SetPropW(hwnd, L"__wine_dwm_frame", (HANDLE)(ULONG_PTR)(dwm_frame_style() == 1 ? 2 : 1));
    }
    else
    {
        for (i = 0; i < 4; i++)
            RemovePropW(hwnd, margin_props[i]);
        RemovePropW(hwnd, L"__wine_dwm_alpha");
    }

    /* Repaint the client: the previous opaque surface may have forced its alpha bits to 0xff. */
    SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    dwm_client_set_attr(hwnd, DWM_ATTR_WINE_FRAME, (DWORD)(ULONG_PTR)GetPropW(hwnd, L"__wine_dwm_frame"));

    /* Aero glass is blurred and tinted with the colorization color (again, for a new color), unless the window chose
     * its own blur, or is choosing it now (__wine_dwm_blur_glass: DwmSetWindowAttribute made the glass) */
    if (GetPropW(hwnd, L"__wine_dwm_frame") == (HANDLE)2 && dwm_materials_is_enabled())
    {
        DWORD blur = DWM_WINE_BLUR_NONE, color, tint;
        BOOL opaque;

        get_stored_attribute(hwnd, DWM_ATTR_WINE_BLUR, &blur);
        if ((!(blur & DWM_WINE_BLUR_MODE) || GetPropW(hwnd, L"__wine_dwm_blur_auto")) &&
            !GetPropW(hwnd, L"__wine_dwm_blur_glass") && SUCCEEDED(DwmGetColorizationColor(&color, &opaque)))
        {
            tint = (color & 0xff00ff00) | (color >> 16 & 0xff) | (color & 0xff) << 16;   /* ARGB to ABGR */
            blur = DWM_WINE_BLUR_BEHIND | DWM_WINE_BLUR_TINTED;
            DwmSetWindowAttribute(hwnd, DWM_ATTR_WINE_TINT, &tint, sizeof(tint));
            DwmSetWindowAttribute(hwnd, DWM_ATTR_WINE_BLUR, &blur, sizeof(blur));
            SetPropW(hwnd, L"__wine_dwm_blur_auto", (HANDLE)1);
        }
    }
    else clear_auto_blur(hwnd);   /* no longer Aero */
}

/* the Aero blur set_glass added, if it is still the window's */
static void clear_auto_blur(HWND hwnd)
{
    DWORD none = DWM_WINE_BLUR_NONE;
    if (RemovePropW(hwnd, L"__wine_dwm_blur_auto"))
        DwmSetWindowAttribute(hwnd, DWM_ATTR_WINE_BLUR, &none, sizeof(none));
}

/***********************************************************************
 *           WexDwmpRefreshFrame         (DWMAPI.@)
 *
 * The theme switched frame styles (DwmFrameStyle): a glass frame that uxtheme asked for only for Aero goes away
 * under the Windows 10/11 style; any other glass frame is marked again with its margins, gaining or losing the
 * Aero blur. uxtheme calls it for a window whose frame no longer matches the style.
 */
void WINAPI WexDwmpRefreshFrame(HWND hwnd)
{
    MARGINS margins;
    int values[4], i;

    if (!IsWindow(hwnd) || !GetPropW(hwnd, L"__wine_dwm_frame")) return;
    for (i = 0; i < 4; i++)
    {
        LONG_PTR stored = (LONG_PTR)GetPropW(hwnd, margin_props[i]);
        values[i] = stored == 0x10000 ? -1 : stored ? (int)stored - 1 : 0;
    }
    if (!dwm_frame_style() && RemovePropW(hwnd, L"__wine_aero_asked") &&
        !values[0] && !values[1] && !values[2] && !values[3])
    {
        for (i = 0; i < 4; i++) RemovePropW(hwnd, margin_props[i]);
        RemovePropW(hwnd, L"__wine_dwm_alpha");
        RemovePropW(hwnd, L"__wine_dwm_frame");
        dwm_client_set_attr(hwnd, DWM_ATTR_WINE_FRAME, 0);
        clear_auto_blur(hwnd);
        SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    margins.cxLeftWidth = values[0];
    margins.cxRightWidth = values[1];
    margins.cyTopHeight = values[2];
    margins.cyBottomHeight = values[3];
    set_glass(hwnd, &margins);
}

/**********************************************************************
 *           DwmExtendFrameIntoClientArea    (DWMAPI.@)
 */
HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND hwnd, const MARGINS* margins)
{
    TRACE("(%p, %p)\n", hwnd, margins);

    if (dwm_alpha_is_enabled() && margins && IsWindow(hwnd) &&
        !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD))
    {
        RemovePropW(hwnd, L"__wine_dwm_blur_glass");   /* the app's margins now; turning the blur off keeps them */
        RemovePropW(hwnd, L"__wine_aero_asked");        /* nor does a frame style switch take the glass away */
        set_glass(hwnd, margins);
    }

    return S_OK;
}

/**********************************************************************
 *           DwmGetColorizationColor      (DWMAPI.@)
 */
static void dwm_read_colorization_color(DWORD *color, BOOL *opaque)
{
    DWORD type, value, cbData;

    *color = 0xC40078D7; /* ARGB, as returned by Windows; compositor conversion is separate. */
    *opaque = FALSE;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
            L"ColorizationColor", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        *color = value;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
            L"ColorizationOpaqueBlend", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        *opaque = value != 0;
}

HRESULT WINAPI DwmGetColorizationColor(DWORD *colorization, BOOL *opaque_blend)
{
    TRACE("(%p, %p)\n", colorization, opaque_blend);

    if (!colorization || !opaque_blend)
        return E_INVALIDARG;

    dwm_read_colorization_color(colorization, opaque_blend);

    return S_OK;
}

/**********************************************************************
 *        DwmInvalidateIconicBitmaps      (DWMAPI.@)
 */
static void invalidate_iconic(HWND hwnd);

HRESULT WINAPI DwmInvalidateIconicBitmaps(HWND hwnd)
{
    TRACE("(%p)\n", hwnd);

    if (!IsWindow(hwnd))
        return E_HANDLE;

    invalidate_iconic(hwnd);
    return S_OK;
}

/**********************************************************************
 *           DwmSetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD attributenum, LPCVOID attribute, DWORD size)
{
    DWORD value;
    int i;

    TRACE("(%p, %lx, %p, %lx)\n", hwnd, attributenum, attribute, size);

    if (!IsWindow(hwnd))
        return E_HANDLE;
    if (!attribute)
        return E_INVALIDARG;
    if (size != 4)
        return E_INVALIDARG;
    if (attributenum == 0 || (attributenum >= DWMWA_LAST && attributenum != DWMWA_MICA_EFFECT &&
                              (attributenum < DWM_ATTR_WINE_BLUR || attributenum > DWM_ATTR_WINE_LAST)))
        return E_INVALIDARG;

    value = *(const DWORD *)attribute;
    if (attributenum == DWM_ATTR_WINE_BLUR &&
        ((value & DWM_WINE_BLUR_MODE) > DWM_WINE_BLUR_GRADIENT ||
         (value & ~(DWM_WINE_BLUR_MODE | DWM_WINE_BLUR_TINTED | DWM_WINE_BLUR_REGION))))
        return E_INVALIDARG;

    dwm_client_set_attr(hwnd, attributenum, value);  /* best effort: the local table is the mirror */
    if (attributenum == DWM_ATTR_WINE_BLUR) RemovePropW(hwnd, L"__wine_dwm_blur_auto");   /* the app's blur now */

    /* Blur behind (P12b) needs an alpha surface: the whole window becomes glass, unless the app already chose
     * margins; it is cleared again only if it was set here. */
    if (attributenum == DWM_ATTR_WINE_BLUR && dwm_alpha_is_enabled() && dwm_materials_is_enabled() &&
        !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD))
    {
        static const MARGINS sheet = { -1, -1, -1, -1 }, zero_margins = { 0, 0, 0, 0 };
        BOOL on = (value & DWM_WINE_BLUR_MODE) != DWM_WINE_BLUR_NONE;
        if (on && !GetPropW(hwnd, L"__wine_dwm_alpha"))
        {
            SetPropW(hwnd, L"__wine_dwm_blur_glass", (HANDLE)1);   /* first: set_glass adds no Aero blur then */
            set_glass(hwnd, &sheet);
        }
        else if (!on && RemovePropW(hwnd, L"__wine_dwm_blur_glass"))
            set_glass(hwnd, &zero_margins);
    }

    /* Windows 11 draws the backdrop behind the caption without any extended margins. */
    if ((attributenum == DWMWA_SYSTEMBACKDROP_TYPE && (value == 2 || value == 3 || value == 4)) ||
        (attributenum == DWMWA_MICA_EFFECT && value != 0))
    {
        static const MARGINS zero_margins = { 0, 0, 0, 0 };
        if (dwm_alpha_is_enabled() && dwm_frame_is_enabled() &&
            !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) &&
            !GetPropW(hwnd, L"__wine_dwm_alpha"))
            set_glass(hwnd, &zero_margins);
    }

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < ATTRIBUTE_TABLE_SIZE; i++)
    {
        if (attribute_table[i].hwnd == hwnd && attribute_table[i].attribute == attributenum)
        {
            attribute_table[i].value = value;
            LeaveCriticalSection(&thumbnail_cs);
            return S_OK;
        }
    }
    /* no existing entry; take a free slot, reclaiming dead windows first */
    for (i = 0; i < ATTRIBUTE_TABLE_SIZE; i++)
    {
        if (!attribute_table[i].hwnd || !IsWindow(attribute_table[i].hwnd))
        {
            attribute_table[i].hwnd = hwnd;
            attribute_table[i].attribute = attributenum;
            attribute_table[i].value = value;
            LeaveCriticalSection(&thumbnail_cs);
            return S_OK;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    return E_OUTOFMEMORY;
}

/**********************************************************************
 *           DwmGetGraphicsStreamClient         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamClient(UINT uIndex, UUID *pClientUuid)
{
    FIXME("(%d, %p) stub\n", uIndex, pClientUuid);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmGetTransportAttributes         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetTransportAttributes(BOOL *pfIsRemoting, BOOL *pfIsConnected, DWORD *pDwGeneration)
{
    FIXME("(%p, %p, %p) stub\n", pfIsRemoting, pfIsConnected, pDwGeneration);

    return DWM_E_COMPOSITIONDISABLED;
}

/**********************************************************************
 *           DwmUnregisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmUnregisterThumbnail(HTHUMBNAIL thumbnail)
{
    THUMBNAIL_ENTRY entry;
    BOOL found = FALSE;
    int i;

    TRACE("(%p)\n", thumbnail);

    if (!thumbnail)
        return E_INVALIDARG;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (thumbnail_table[i].handle == thumbnail)
        {
            entry = thumbnail_table[i];
            memset(&thumbnail_table[i], 0, sizeof(thumbnail_table[i]));
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (!found)
        return E_INVALIDARG;

    /* tell the broker to drop the thumbnail (best effort; fails quietly without one) */
    dwm_client_thumb_del(entry.dest, (UINT)(ULONG_PTR)entry.handle);
    return S_OK;
}

/**********************************************************************
 *           DwmEnableMMCSS         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableMMCSS(BOOL enableMMCSS)
{
    FIXME("(%d) stub\n", enableMMCSS);

    return S_OK;
}

/**********************************************************************
 *           DwmGetGraphicsStreamTransformHint         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamTransformHint(UINT uIndex, MilMatrix3x2D *pTransform)
{
    TRACE("(%d, %p)\n", uIndex, pTransform);

    return DWM_E_COMPOSITIONDISABLED;
}

/**********************************************************************
 *           DwmEnableBlurBehindWindow         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableBlurBehindWindow(HWND hWnd, const DWM_BLURBEHIND *pBlurBuf)
{
    TRACE("%p %p\n", hWnd, pBlurBuf);

    if (!IsWindow(hWnd))
        return E_HANDLE;
    if (!pBlurBuf)
        return E_INVALIDARG;

    if (pBlurBuf->dwFlags & (DWM_BB_ENABLE | DWM_BB_BLURREGION))
    {
        DWORD value = DWM_WINE_BLUR_NONE;

        get_stored_attribute(hWnd, DWM_ATTR_WINE_BLUR, &value);
        if (pBlurBuf->dwFlags & DWM_BB_ENABLE)   /* the region stays until replaced, as on Windows */
            value = (value & DWM_WINE_BLUR_REGION) | (pBlurBuf->fEnable ? DWM_WINE_BLUR_BEHIND : DWM_WINE_BLUR_NONE);
        if (pBlurBuf->dwFlags & DWM_BB_BLURREGION)
        {
            /* ponytail: the region's bounding box; a region of several rectangles is blurred as one */
            RECT box;
            value &= ~DWM_WINE_BLUR_REGION;
            if (pBlurBuf->hRgnBlur && GetRgnBox(pBlurBuf->hRgnBlur, &box) != ERROR)
            {
                DWORD lt = MAKELONG((SHORT)box.left, (SHORT)box.top), rb = MAKELONG((SHORT)box.right, (SHORT)box.bottom);
                DwmSetWindowAttribute(hWnd, DWM_ATTR_WINE_RGN_LT, &lt, sizeof(lt));
                DwmSetWindowAttribute(hWnd, DWM_ATTR_WINE_RGN_RB, &rb, sizeof(rb));
                value |= DWM_WINE_BLUR_REGION;
            }
        }
        DwmSetWindowAttribute(hWnd, DWM_ATTR_WINE_BLUR, &value, sizeof(value));
    }
    return S_OK;
}

/**********************************************************************
 *           DwmDefWindowProc         (DWMAPI.@)
 */
BOOL WINAPI DwmDefWindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, LRESULT *plResult)
{
    static int i;

    if (!i++) FIXME("stub\n");

    return FALSE;
}

/**********************************************************************
 *           DwmGetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd, DWORD attribute, PVOID pv_attribute, DWORD size)
{
    HRESULT hr;

    TRACE("(%p %ld %p %ld)\n", hwnd, attribute, pv_attribute, size);

    if (!IsWindow(hwnd))
        return E_HANDLE;

    switch (attribute) {
    case DWMWA_EXTENDED_FRAME_BOUNDS:
    {
        RECT *rect = (RECT *)pv_attribute;
        DPI_AWARENESS_CONTEXT context;

        if (!rect)
            return E_INVALIDARG;
        if (size < sizeof(*rect))
            return E_NOT_SUFFICIENT_BUFFER;
        if (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)
            return E_HANDLE;

        /* DWM frame bounds are always in physical coords */
        context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        if (GetWindowRect(hwnd, rect))
            hr = S_OK;
        else
            hr = HRESULT_FROM_WIN32(GetLastError());

        SetThreadDpiAwarenessContext(context);

        /* Windows 10/11 glass frames keep the outer bands as an invisible resize area; report the visible frame.
         * An Aero frame (2) is visible to its edges. */
        if (hr == S_OK && GetPropW(hwnd, L"__wine_dwm_frame") == (HANDLE)1)
        {
            DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
            if (!(style & (WS_MAXIMIZE | WS_MINIMIZE)))
            {
                UINT dpi = GetDpiForWindow(hwnd);
                int bw = GetSystemMetricsForDpi((style & WS_THICKFRAME) ? SM_CXFRAME : SM_CXDLGFRAME, dpi);
                int bh = GetSystemMetricsForDpi((style & WS_THICKFRAME) ? SM_CYFRAME : SM_CYDLGFRAME, dpi);
                rect->left += bw - 1;
                rect->right -= bw - 1;
                rect->bottom -= bh - 1;
            }
        }
        break;
    }
    case DWMWA_NCRENDERING_ENABLED:
    {
        BOOL *value = (BOOL *)pv_attribute;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        *value = TRUE;
        hr = S_OK;
        break;
    }
    case DWMWA_CLOAKED:
    {
        DWORD *value = (DWORD *)pv_attribute;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        *value = 0;
        hr = S_OK;
        break;
    }
    case DWMWA_HAS_ICONIC_BITMAP:
    {
        BOOL *value = (BOOL *)pv_attribute;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        *value = FALSE;
        hr = S_OK;
        break;
    }
    case DWMWA_CAPTION_BUTTON_BOUNDS:
    {
        RECT *rect = (RECT *)pv_attribute;
        RECT window_rect;
        int width;

        if (!rect)
            return E_INVALIDARG;
        if (size < sizeof(*rect))
            return E_INVALIDARG;
        if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION))
        {
            rect->left = rect->top = rect->right = rect->bottom = 0;
            hr = S_OK;
            break;
        }
        if (!GetWindowRect(hwnd, &window_rect))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        width = window_rect.right - window_rect.left;
        rect->top = 0;
        rect->bottom = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
        rect->right = width;
        rect->left = width - 3 * GetSystemMetrics(SM_CXSIZE);
        if (rect->left < 0)
            rect->left = 0;
        hr = S_OK;
        break;
    }
    case DWMWA_NCRENDERING_POLICY:
    case DWMWA_ALLOW_NCPAINT:
    case DWMWA_FORCE_ICONIC_REPRESENTATION:
    case DWMWA_DISALLOW_PEEK:
    case DWMWA_FREEZE_REPRESENTATION:
    case DWMWA_PASSIVE_UPDATE_MODE:
        hr = E_INVALIDARG;
        break;
    case DWMWA_FLIP3D_POLICY:
    case DWMWA_EXCLUDED_FROM_PEEK:
    case DWMWA_USE_IMMERSIVE_DARK_MODE:
    case DWMWA_WINDOW_CORNER_PREFERENCE:
    case DWMWA_BORDER_COLOR:
    case DWMWA_CAPTION_COLOR:
    case DWMWA_TEXT_COLOR:
    case DWMWA_SYSTEMBACKDROP_TYPE:
    case DWMWA_USE_HOSTBACKDROPBRUSH:
    case DWMWA_MICA_EFFECT:
    {
        DWORD *value = (DWORD *)pv_attribute;
        DWORD default_value;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        if (attribute == DWMWA_BORDER_COLOR || attribute == DWMWA_CAPTION_COLOR ||
            attribute == DWMWA_TEXT_COLOR)
            default_value = DWMWA_COLOR_DEFAULT;
        else
            default_value = 0;
        if (!get_attribute_value(hwnd, attribute, value))
            *value = default_value;
        hr = S_OK;
        break;
    }
    default:
        FIXME("attribute %ld not implemented.\n", attribute);
        hr = E_INVALIDARG;
        break;
    }

    return hr;
}

/**********************************************************************
 *           DwmRegisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmRegisterThumbnail(HWND dest, HWND src, PHTHUMBNAIL thumbnail_id)
{
    int i, j;
    HTHUMBNAIL handle;

    TRACE("(%p %p %p)\n", dest, src, thumbnail_id);

    if (!thumbnail_id)
        return E_INVALIDARG;
    if (!dest)
        return E_INVALIDARG;
    if (!src)
        return E_INVALIDARG;
    if (!IsWindow(dest) || !IsWindow(src))
        return E_HANDLE;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (!thumbnail_table[i].handle)
            break;
    }
    if (i == THUMBNAIL_TABLE_SIZE)
    {
        /* table full; reclaim a slot whose dest or src is no longer a window */
        for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
        {
            if (!IsWindow(thumbnail_table[i].dest) || !IsWindow(thumbnail_table[i].src))
            {
                memset(&thumbnail_table[i], 0, sizeof(thumbnail_table[i]));
                break;
            }
        }
        if (i == THUMBNAIL_TABLE_SIZE)
        {
            LeaveCriticalSection(&thumbnail_cs);
            return E_OUTOFMEMORY;
        }
    }

    /* allocate a handle from the counter, skipping 0 and any value currently in the table */
    do
    {
        if (++thumbnail_next_handle == 0)
            thumbnail_next_handle = 1;
        handle = (HTHUMBNAIL)(ULONG_PTR)thumbnail_next_handle;
        for (j = 0; j < THUMBNAIL_TABLE_SIZE; j++)
        {
            if (thumbnail_table[j].handle == handle)
                break;
        }
    } while (j != THUMBNAIL_TABLE_SIZE);

    thumbnail_table[i].handle = handle;
    thumbnail_table[i].dest = dest;
    thumbnail_table[i].src = src;
    memset(&thumbnail_table[i].props, 0, sizeof(thumbnail_table[i].props));
    thumbnail_table[i].props.opacity = 255;  /* fully opaque until DWM_TNP_OPACITY says otherwise */
    LeaveCriticalSection(&thumbnail_cs);

    *thumbnail_id = handle;
    return S_OK;
}

static int get_display_frequency(void)
{
    DEVMODEW mode;
    BOOL ret;

    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    ret = EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &mode, 0);
    if (ret && (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency > 1)
    {
        return mode.dmDisplayFrequency;
    }
    else
    {
        WARN("Failed to query display frequency, returning a fallback value.\n");
        return 60;
    }
}

/* Frame clock (docs/DWM_ARCHITECTURE.md section 2.3, P6 slice 1): the compositor publishes one record per
 * composed frame into a per-desktop shared page and pulses an event. We open both read-only, once per
 * process, and read a consistent snapshot with the sequence lock. Never wait while holding any lock. */
static HANDLE clock_mapping, clock_event;
static struct dwm_clock_page *clock_page;
static BOOL clock_opened;
static INT64 clock_last_failed;   /* QPC time of the last failed open */

static BOOL clock_open(void)
{
    WCHAR desk_name[MAX_PATH], name[MAX_PATH + 64], event_name[MAX_PATH + 64];
    HDESK desk;
    LARGE_INTEGER li;
    INT64 now;

    if (clock_opened) return clock_page != NULL;

    /* retry the open at most once per second when it failed */
    NtQueryPerformanceCounter(&li, NULL);
    now = li.QuadPart;
    if (clock_last_failed && now - clock_last_failed < 10000000) return FALSE;

    if (!(desk = GetThreadDesktop(GetCurrentThreadId()))) return FALSE;
    if (!GetUserObjectInformationW(desk, UOI_NAME, desk_name, ARRAY_SIZE(desk_name), NULL)) return FALSE;
    lstrcpynW(name, DWM_CLOCK_NAME, ARRAY_SIZE(name));
    lstrcatW(name, desk_name);
    lstrcpynW(event_name, DWM_CLOCK_EVENT_NAME, ARRAY_SIZE(event_name));
    lstrcatW(event_name, desk_name);

    clock_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!clock_mapping)
    {
        clock_last_failed = now;
        return FALSE;
    }
    clock_event = OpenEventW(SYNCHRONIZE, FALSE, event_name);
    clock_page = MapViewOfFile(clock_mapping, FILE_MAP_READ, 0, 0, sizeof(*clock_page));
    clock_opened = TRUE;
    return clock_page != NULL;
}

/* read a consistent snapshot with the sequence lock: retry while seq is odd or changed */
static BOOL clock_read(struct dwm_clock_page *out)
{
    LONG seq1, seq2;
    int tries;

    if (!clock_open()) return FALSE;
    /* bounded: a writer that died in the middle of an update must not hang the reader */
    for (tries = 0; tries < 1000; tries++)
    {
        seq1 = __atomic_load_n(&clock_page->seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1) continue;
        out->flags = clock_page->flags;
        out->frame = clock_page->frame;
        out->qpc_composed = clock_page->qpc_composed;
        out->qpc_period = clock_page->qpc_period;
        seq2 = __atomic_load_n(&clock_page->seq, __ATOMIC_ACQUIRE);
        if (seq1 == seq2) return TRUE;
    }
    return FALSE;
}

/**********************************************************************
 *           DwmGetCompositionTimingInfo         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetCompositionTimingInfo(HWND hwnd, DWM_TIMING_INFO *info)
{
    struct dwm_clock_page snapshot;
    LARGE_INTEGER performance_frequency, qpc;
    static int i;
    int display_frequency;
    QPC_TIME qpc_refresh_period;
    ULONGLONG numerator, denominator;

    if (!info)
        return E_INVALIDARG;

    if (info->cbSize != sizeof(DWM_TIMING_INFO))
        return MILERR_MISMATCHED_SIZE;

    if(!i++) FIXME("(%p %p)\n", hwnd, info);

    memset(info, 0, info->cbSize);
    info->cbSize = sizeof(DWM_TIMING_INFO);

    display_frequency = get_display_frequency();

    QueryPerformanceFrequency(&performance_frequency);
    qpc_refresh_period = performance_frequency.QuadPart / display_frequency;
    if (!qpc_refresh_period)
        qpc_refresh_period = 1;
    info->qpcRefreshPeriod = qpc_refresh_period;

    /* like Windows 10 and later: the rates are the counter frequency over the refresh period in counter ticks */
    numerator = (ULONGLONG)performance_frequency.QuadPart;
    denominator = qpc_refresh_period;
    while ((numerator > 0xFFFFFFFFULL) || (denominator > 0xFFFFFFFFULL))
    {
        numerator >>= 1;
        denominator >>= 1;
    }
    info->rateRefresh.uiNumerator = (UINT32)numerator;
    info->rateRefresh.uiDenominator = (UINT32)denominator;
    info->rateCompose = info->rateRefresh;

    QueryPerformanceCounter(&qpc);
    info->qpcVBlank = (qpc.QuadPart / info->qpcRefreshPeriod) * info->qpcRefreshPeriod;

    /* when the compositor publishes a frame clock, use its composed frame and refresh period */
    if (clock_read(&snapshot) && (snapshot.flags & DWM_CLOCK_COMPOSED))
    {
        info->cFrame = snapshot.frame;
        info->cRefresh = snapshot.frame;
        info->qpcCompose = snapshot.qpc_composed;
        info->qpcRefreshPeriod = snapshot.qpc_period;
        numerator = (ULONGLONG)performance_frequency.QuadPart;
        denominator = (ULONGLONG)snapshot.qpc_period;
        while ((numerator > 0xFFFFFFFFULL) || (denominator > 0xFFFFFFFFULL))
        {
            numerator >>= 1;
            denominator >>= 1;
        }
        info->rateRefresh.uiNumerator = (UINT32)numerator;
        info->rateRefresh.uiDenominator = (UINT32)denominator;
        info->rateCompose = info->rateRefresh;
    }

    return S_OK;
}

/**********************************************************************
 *                  DwmFlush              (DWMAPI.@)
 */
HRESULT WINAPI DwmFlush(void)
{
    struct dwm_clock_page snapshot;
    LARGE_INTEGER qpf, qpc, delay;
    LONG64 qpc_refresh_period;
    int display_frequency;
    static BOOL once;

    if (!once++)
        FIXME("stub.\n");
    else
        TRACE("stub.\n");

    /* when the compositor publishes a frame clock, wait for a new composed frame (or two refresh periods) */
    if (clock_read(&snapshot) && (snapshot.flags & DWM_CLOCK_COMPOSED))
    {
        UINT64 frame = snapshot.frame;
        DWORD period_ms = (DWORD)(snapshot.qpc_period / 10000);
        int i;

        if (period_ms < 1) period_ms = 1;
        for (i = 0; i < 2; i++)
        {
            WaitForSingleObject(clock_event, period_ms);
            if (clock_read(&snapshot) && snapshot.frame != frame)
                return S_OK;
        }
        /* nothing changed on screen: the compositor idles */
        return S_OK;
    }

    display_frequency = get_display_frequency();
    NtQueryPerformanceCounter(&qpc, &qpf);
    qpc_refresh_period = qpf.QuadPart / display_frequency;
    if (!qpc_refresh_period)
        qpc_refresh_period = 1;
    delay.QuadPart = (qpc.QuadPart - ((qpc.QuadPart + qpc_refresh_period - 1) / qpc_refresh_period) * qpc_refresh_period)
            * 10000000 / qpf.QuadPart;
    NtDelayExecution(FALSE, &delay);

    return S_OK;
}

/**********************************************************************
 *           DwmAttachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmAttachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmDetachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmDetachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmUpdateThumbnailProperties         (DWMAPI.@)
 */
HRESULT WINAPI DwmUpdateThumbnailProperties(HTHUMBNAIL thumbnail, const DWM_THUMBNAIL_PROPERTIES *props)
{
    DWM_THUMBNAIL_PROPERTIES local_props;
    THUMBNAIL_ENTRY entry;
    BOOL found = FALSE;
    int i;

    TRACE("(%p, %p)\n", thumbnail, props);

    if (!thumbnail)
        return E_INVALIDARG;
    if (!props)
        return E_INVALIDARG;

    /* copy *props into a local first, validate, then lock */
    local_props = *props;
    if (local_props.dwFlags & ~(DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY |
                                DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY))
        return E_INVALIDARG;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (thumbnail_table[i].handle == thumbnail)
        {
            if (!IsWindow(thumbnail_table[i].dest) || !IsWindow(thumbnail_table[i].src))
            {
                LeaveCriticalSection(&thumbnail_cs);
                return E_HANDLE;
            }
            if (local_props.dwFlags & DWM_TNP_RECTDESTINATION)
                thumbnail_table[i].props.rcDestination = local_props.rcDestination;
            if (local_props.dwFlags & DWM_TNP_RECTSOURCE)
                thumbnail_table[i].props.rcSource = local_props.rcSource;
            if (local_props.dwFlags & DWM_TNP_OPACITY)
                thumbnail_table[i].props.opacity = local_props.opacity;
            if (local_props.dwFlags & DWM_TNP_VISIBLE)
                thumbnail_table[i].props.fVisible = local_props.fVisible;
            if (local_props.dwFlags & DWM_TNP_SOURCECLIENTAREAONLY)
                thumbnail_table[i].props.fSourceClientAreaOnly = local_props.fSourceClientAreaOnly;
            /* copy the merged state out before leaving the lock; the broker is called unlocked */
            entry = thumbnail_table[i];
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (!found)
        return E_INVALIDARG;

    /* publish the merged state to the broker (best effort; fails quietly without one) */
    {
        struct dwm_thumb_set set;
        DWORD flags = 0;

        memset(&set, 0, sizeof(set));
        if (entry.props.fVisible) flags |= DWM_THUMB_VISIBLE;
        if (entry.props.fSourceClientAreaOnly) flags |= DWM_THUMB_CLIENT_ONLY;
        if (entry.props.rcSource.left || entry.props.rcSource.top ||
            entry.props.rcSource.right || entry.props.rcSource.bottom)
            flags |= DWM_THUMB_HAS_SOURCE;
        set.flags = flags;
        set.opacity = entry.props.opacity;
        set.dest[0] = entry.props.rcDestination.left;
        set.dest[1] = entry.props.rcDestination.top;
        set.dest[2] = entry.props.rcDestination.right;
        set.dest[3] = entry.props.rcDestination.bottom;
        set.source[0] = entry.props.rcSource.left;
        set.source[1] = entry.props.rcSource.top;
        set.source[2] = entry.props.rcSource.right;
        set.source[3] = entry.props.rcSource.bottom;
        dwm_client_thumb_set(entry.dest, (UINT)(ULONG_PTR)entry.handle, entry.src, &set);
    }
    return S_OK;
}

/**********************************************************************
 *           DwmSetPresentParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetPresentParameters(HWND hwnd, DWM_PRESENT_PARAMETERS *params)
{
    FIXME("(%p %p) stub\n", hwnd, params);
    return S_OK;
};

/* Iconic bitmaps (P9): two sections per window (thumbnail, live preview), see include/wine/dwm_protocol.h. Kept
 * open while the window lives, so the shell finds the last picture; a dead window's slot is reused, and all are
 * closed when dwmapi is unloaded. A section is made again when a larger picture comes. */
#define ICONIC_TABLE_SIZE 32
static struct
{
    HWND hwnd;
    HANDLE section[2];
    SIZE_T size[2];
} iconic_table[ICONIC_TABLE_SIZE];
static DWORD iconic_serial;

static int iconic_slot(HWND hwnd, BOOL create)
{
    int i, free_slot = -1;

    for (i = 0; i < ICONIC_TABLE_SIZE; i++)
    {
        if (iconic_table[i].hwnd == hwnd) return i;
        if (free_slot < 0 && (!iconic_table[i].hwnd || !IsWindow(iconic_table[i].hwnd))) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    for (i = 0; i < 2; i++)
    {
        if (iconic_table[free_slot].section[i]) CloseHandle(iconic_table[free_slot].section[i]);
        iconic_table[free_slot].section[i] = NULL;
        iconic_table[free_slot].size[i] = 0;
    }
    iconic_table[free_slot].hwnd = hwnd;
    return free_slot;
}

/* kind 0: thumbnail, 1: live preview */
static HANDLE iconic_section(HWND hwnd, int kind, SIZE_T size)
{
    WCHAR name[64];
    int slot;

    if ((slot = iconic_slot(hwnd, TRUE)) < 0) return NULL;
    if (iconic_table[slot].size[kind] >= size) return iconic_table[slot].section[kind];
    if (iconic_table[slot].section[kind]) CloseHandle(iconic_table[slot].section[kind]);
    iconic_table[slot].size[kind] = 0;
    swprintf(name, ARRAY_SIZE(name), L"%s%lx", kind ? DWM_ICONIC_LIVE_NAME : DWM_ICONIC_NAME, HandleToUlong(hwnd));
    if (!(iconic_table[slot].section[kind] = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                                                0, size, name)))
        return NULL;
    /* an existing section (still held by the shell, or made by someone else) has an unknown size: mapping it fails
     * when it is too small, and the next picture makes it again */
    iconic_table[slot].size[kind] = GetLastError() == ERROR_ALREADY_EXISTS ? 0 : size;
    return iconic_table[slot].section[kind];
}

/* Copy the bitmap as premultiplied top-down BGRA before returning, so the caller may delete it at once. */
static HRESULT store_iconic(HWND hwnd, int kind, HBITMAP hbmp, const POINT *pos, DWORD flags)
{
    BITMAPINFO info = {{ sizeof(BITMAPINFOHEADER) }};
    struct dwm_iconic *header;
    HANDLE section;
    SIZE_T size;
    BITMAP bm;
    HDC dc;
    HRESULT hr = E_INVALIDARG;

    if (!GetObjectW(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0 ||
        bm.bmWidth > DWM_ICONIC_MAX || bm.bmHeight > DWM_ICONIC_MAX)
        return E_INVALIDARG;
    size = sizeof(*header) + (SIZE_T)bm.bmWidth * bm.bmHeight * 4;
    EnterCriticalSection(&thumbnail_cs);
    if (!(section = iconic_section(hwnd, kind, size)) ||
        !(header = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size)))
    {
        LeaveCriticalSection(&thumbnail_cs);
        return E_OUTOFMEMORY;
    }
    info.bmiHeader.biWidth = bm.bmWidth;
    info.bmiHeader.biHeight = -bm.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    header->width = header->height = 0;   /* readers skip a picture that is being written */
    dc = GetDC(0);
    if (GetDIBits(dc, hbmp, 0, bm.bmHeight, header + 1, &info, DIB_RGB_COLORS) == bm.bmHeight)
    {
        WINDOWPLACEMENT placement = { sizeof(placement) };
        RECT window;

        header->flags = flags | (pos ? DWM_ICONIC_HAS_POS : 0);
        header->x = pos ? pos->x : 0;
        header->y = pos ? pos->y : 0;
        if (IsIconic(hwnd) && GetWindowPlacement(hwnd, &placement))
        {
            /* workarea coordinates; the work area of a bottom or right taskbar starts at the screen's origin */
            MONITORINFO info = { sizeof(info) };
            window = placement.rcNormalPosition;
            if (GetMonitorInfoW(MonitorFromRect(&window, MONITOR_DEFAULTTOPRIMARY), &info))
                OffsetRect(&window, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
        }
        else GetWindowRect(hwnd, &window);
        header->window[0] = window.left;
        header->window[1] = window.top;
        header->window[2] = window.right;
        header->window[3] = window.bottom;
        header->serial = ++iconic_serial;
        header->height = bm.bmHeight;
        header->width = bm.bmWidth;
        hr = S_OK;
    }
    ReleaseDC(0, dc);
    UnmapViewOfFile(header);
    LeaveCriticalSection(&thumbnail_cs);
    return hr;
}

/* DwmInvalidateIconicBitmaps: the stored pictures are stale; the shell asks for new ones */
static void invalidate_iconic(HWND hwnd)
{
    struct dwm_iconic *header;
    int slot, i;

    EnterCriticalSection(&thumbnail_cs);
    if ((slot = iconic_slot(hwnd, FALSE)) >= 0)
        for (i = 0; i < 2; i++)
            if (iconic_table[slot].section[i] &&
                (header = MapViewOfFile(iconic_table[slot].section[i], FILE_MAP_WRITE, 0, 0, sizeof(*header))))
            {
                header->width = header->height = 0;
                UnmapViewOfFile(header);
            }
    LeaveCriticalSection(&thumbnail_cs);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    int i;

    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    else if (reason == DLL_PROCESS_DETACH && !reserved)   /* FreeLibrary: the iconic sections go with it */
        for (i = 0; i < ICONIC_TABLE_SIZE * 2; i++)
            if (iconic_table[i / 2].section[i % 2]) CloseHandle(iconic_table[i / 2].section[i % 2]);
    return TRUE;
}

/**********************************************************************
 *           DwmSetIconicLivePreviewBitmap         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicLivePreviewBitmap(HWND hwnd, HBITMAP hbmp, POINT *pos, DWORD flags)
{
    DWORD value;

    TRACE("(%p %p %p %lx)\n", hwnd, hbmp, pos, flags);

    if (!IsWindow(hwnd))
        return E_HANDLE;
    if (!get_stored_attribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &value) || !value)
        return E_INVALIDARG;
    if (!hbmp)
        return E_INVALIDARG;
    if (GetObjectType(hbmp) != OBJ_BITMAP)
        return E_INVALIDARG;
    if (flags & ~DWM_SIT_DISPLAYFRAME)
        return E_INVALIDARG;

    return store_iconic(hwnd, 1, hbmp, pos, flags);
}

/**********************************************************************
 *           DwmSetIconicThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicThumbnail(HWND hwnd, HBITMAP hbmp, DWORD flags)
{
    DWORD value;

    TRACE("(%p %p %lx)\n", hwnd, hbmp, flags);

    if (!IsWindow(hwnd))
        return E_HANDLE;
    if (!get_stored_attribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &value) || !value)
        return E_INVALIDARG;
    if (!hbmp)
        return E_INVALIDARG;
    if (GetObjectType(hbmp) != OBJ_BITMAP)
        return E_INVALIDARG;
    if (flags & ~DWM_SIT_DISPLAYFRAME)
        return E_INVALIDARG;

    return store_iconic(hwnd, 0, hbmp, NULL, flags);
}

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

/**********************************************************************
 *           DwmpGetColorizationParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmpGetColorizationParameters(void *params)
{
    DWMCOLORIZATIONPARAMS *p = params;
    DWORD type, value, cbData;

    TRACE("(%p)\n", params);

    if (!p)
        return E_INVALIDARG;

    dwm_read_colorization_color(&p->clrColor, &p->fOpaque);
    p->clrAfterGlow = p->clrColor;
    p->nIntensity = 54;
    p->clrAfterGlowBalance = 43;
    p->clrBlurBalance = 49;
    p->clrGlassReflectionIntensity = 50;
    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"ColorizationAfterglow", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        p->clrAfterGlow = value;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"ColorizationColorBalance", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        p->nIntensity = value;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"ColorizationAfterglowBalance", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        p->clrAfterGlowBalance = value;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"ColorizationBlurBalance", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        p->clrBlurBalance = value;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"ColorizationGlassReflectionIntensity", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        p->clrGlassReflectionIntensity = value;

    return S_OK;
}

HRESULT WINAPI DwmGetColorizationParameters(DWMCOLORIZATIONPARAMS *params)
{
    return DwmpGetColorizationParameters(params);
}

/**********************************************************************
 *           DwmpSetColorizationParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmpSetColorizationParameters(DWMCOLORIZATIONPARAMS *params, UINT unknown)
{
    HKEY hkey;

    TRACE("(%p, %u)\n", params, unknown);

    if (!params)
        return E_INVALIDARG;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                        0, NULL, 0, KEY_SET_VALUE, NULL, &hkey, NULL) == ERROR_SUCCESS)
    {
        DWORD opaque = params->fOpaque ? 1 : 0;
        RegSetValueExW(hkey, L"ColorizationColor", 0, REG_DWORD, (const BYTE *)&params->clrColor, sizeof(DWORD));
        RegSetValueExW(hkey, L"ColorizationAfterglow", 0, REG_DWORD, (const BYTE *)&params->clrAfterGlow, sizeof(DWORD));
        RegSetValueExW(hkey, L"ColorizationColorBalance", 0, REG_DWORD, (const BYTE *)&params->nIntensity, sizeof(DWORD));
        RegSetValueExW(hkey, L"ColorizationAfterglowBalance", 0, REG_DWORD, (const BYTE *)&params->clrAfterGlowBalance, sizeof(DWORD));
        RegSetValueExW(hkey, L"ColorizationBlurBalance", 0, REG_DWORD, (const BYTE *)&params->clrBlurBalance, sizeof(DWORD));
        RegSetValueExW(hkey, L"ColorizationGlassReflectionIntensity", 0, REG_DWORD, (const BYTE *)&params->clrGlassReflectionIntensity, sizeof(DWORD));
        RegSetValueExW(hkey, L"ColorizationOpaqueBlend", 0, REG_DWORD, (const BYTE *)&opaque, sizeof(DWORD));
        RegCloseKey(hkey);
    }

    SendNotifyMessageW(HWND_BROADCAST, WM_DWMCOLORIZATIONCOLORCHANGED, params->clrColor, params->fOpaque ? 1 : 0);
    return S_OK;
}

HRESULT WINAPI DwmSetColorizationParameters(DWMCOLORIZATIONPARAMS *params, UINT unknown)
{
    return DwmpSetColorizationParameters(params, unknown);
}

/**********************************************************************
 *           DwmShowContact         (DWMAPI.@)
 */
HRESULT WINAPI DwmShowContact(DWORD pointer_id, enum DWM_SHOWCONTACT showcontact)
{
    FIXME("pointer_id %#lx, showcontact %#x stub\n", pointer_id, showcontact);
    return S_OK;
}

/**********************************************************************
 *           DwmQueryThumbnailSourceSize         (DWMAPI.@)
 */
HRESULT WINAPI DwmQueryThumbnailSourceSize(HTHUMBNAIL thumbnail, SIZE *size)
{
    HWND src = NULL;
    RECT rect;
    int i;
    BOOL found = FALSE;

    TRACE("(%p, %p)\n", thumbnail, size);

    if (!thumbnail)
        return E_INVALIDARG;
    if (!size)
        return E_INVALIDARG;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (thumbnail_table[i].handle == thumbnail)
        {
            found = TRUE;
            if (!IsWindow(thumbnail_table[i].dest) || !IsWindow(thumbnail_table[i].src))
            {
                LeaveCriticalSection(&thumbnail_cs);
                return E_HANDLE;
            }
            src = thumbnail_table[i].src;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (!found)
        return E_INVALIDARG;

    /* Windows reports the visible frame of the source, the same rectangle as DWMWA_EXTENDED_FRAME_BOUNDS */
    if (DwmGetWindowAttribute(src, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)) != S_OK)
        memset(&rect, 0, sizeof(rect));
    size->cx = rect.right - rect.left;
    size->cy = rect.bottom - rect.top;
    return S_OK;
}

/**********************************************************************
 *           DwmModifyPreviousDxFrameDuration         (DWMAPI.@)
 */
HRESULT WINAPI DwmModifyPreviousDxFrameDuration(HWND hwnd, INT refreshes, BOOL relative)
{
    TRACE("(%p, %d, %d)\n", hwnd, refreshes, relative);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmTransitionOwnedWindow         (DWMAPI.@)
 */
HRESULT WINAPI DwmTransitionOwnedWindow(HWND hwnd, enum DWMTRANSITION_OWNEDWINDOW_TARGET target)
{
    TRACE("(%p, %d)\n", hwnd, target);

    return E_INVALIDARG;
}
