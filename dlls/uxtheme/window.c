/*
 * Window theming support
 *
 * Copyright 2022 Zhiyi Zhang for CodeWeavers
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
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "dwmapi.h"
#include "uxthemedll.h"
#include "vssym32.h"
#include "wexwayland.h"

static void uxtheme_draw_menu_button(HTHEME theme, HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                     RECT rect, BOOL down, BOOL grayed)
{
    int part, state;

    switch (type)
    {
    case MENU_CLOSE_BUTTON:
        part = WP_MDICLOSEBUTTON;
        break;
    case MENU_MIN_BUTTON:
        part = WP_MDIMINBUTTON;
        break;
    case MENU_RESTORE_BUTTON:
        part = WP_MDIRESTOREBUTTON;
        break;
    case MENU_HELP_BUTTON:
        part = WP_MDIHELPBUTTON;
        break;
    /* There is no WP_MDIMAXBUTTON */
    default:
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    if (grayed)
        state = MINBS_DISABLED;
    else if (down)
        state = MINBS_PUSHED;
    else
        state = MINBS_NORMAL;

    if (IsThemeBackgroundPartiallyTransparent(theme, part, state))
        DrawThemeParentBackground(hwnd, hdc, &rect);
    DrawThemeBackground(theme, hdc, part, state, &rect, NULL);
}

void WINAPI UXTHEME_NonClientButtonDraw(HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                        RECT rect, BOOL down, BOOL grayed)
{
    HTHEME theme;

    theme = OpenThemeDataForDpi(NULL, L"Window", GetDpiForWindow(hwnd));
    if (!theme)
    {
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    switch (type)
    {
    case MENU_CLOSE_BUTTON:
    case MENU_MIN_BUTTON:
    case MENU_MAX_BUTTON:
    case MENU_RESTORE_BUTTON:
    case MENU_HELP_BUTTON:
        uxtheme_draw_menu_button(theme, hwnd, hdc, type, rect, down, grayed);
        break;
    }

    CloseThemeData(theme);
}

/* ---- themed window frame (caption, borders and caption buttons) ---- */

static const WCHAR theme_region_owned[] = L"__wine_theme_frame_region";
static const WCHAR theme_region_updating[] = L"__wine_theme_frame_region_updating";
static const WCHAR app_region_defined[] = L"__wine_app_frame_region";

INT WINAPI UXTHEME_SetWindowRgn(HWND hwnd, HRGN region, BOOL redraw)
{
    HANDLE owned = GetPropW(hwnd, theme_region_owned), defined = GetPropW(hwnd, app_region_defined);
    INT ret;

    /* Hosting clips the client out of the frame; it does not define a new
     * application shape or transfer ownership of the themed outer region. */
    if (GetPropW(hwnd, WEX_WAYLAND_FRAME_MASK_UPDATING_PROP))
        return user_api.pSetWindowRgn(hwnd, region, redraw);

    /* Explicit application shapes take precedence over the themed frame. */
    RemovePropW(hwnd, theme_region_owned);
    if (region) SetPropW(hwnd, app_region_defined, (HANDLE)1);
    else RemovePropW(hwnd, app_region_defined);
    ret = user_api.pSetWindowRgn(hwnd, region, redraw);
    if (!ret)
    {
        if (owned) SetPropW(hwnd, theme_region_owned, owned);
        if (defined) SetPropW(hwnd, app_region_defined, defined);
        else RemovePropW(hwnd, app_region_defined);
    }
    return ret;
}

static void nc_update_region(HWND hwnd, HTHEME theme, int part, int state, int width, int height, int caption_height)
{
    HRGN region = NULL, current, body;
    RECT caption = {0, 0, width, caption_height};
    BOOL owned = !!GetPropW(hwnd, theme_region_owned);
    int current_type;

    if (GetPropW(hwnd, theme_region_updating) || GetPropW(hwnd, app_region_defined)) return;
    if (!theme)
    {
        if (!owned) return;
        SetPropW(hwnd, theme_region_updating, (HANDLE)1);
        RemovePropW(hwnd, theme_region_owned);
        user_api.pSetWindowRgn(hwnd, NULL, FALSE);
        RemovePropW(hwnd, theme_region_updating);
        return;
    }
    current = CreateRectRgn(0, 0, 0, 0);
    if (!current) return;
    if (GetPropW(hwnd, WEX_WAYLAND_HOST_PROP))
    {
        HRGN outer = GetPropW(hwnd, WEX_WAYLAND_FRAME_OUTER_PROP);
        /* Borrowed only on the HWND's owning UI thread, never across IPC. */
        current_type = outer ? CombineRgn(current, outer, NULL, RGN_COPY) : ERROR;
    }
    else current_type = GetWindowRgn(hwnd, current);
    if (current_type != ERROR && !owned)
    {
        /* A region installed before uxtheme loaded also belongs to the app. */
        SetPropW(hwnd, app_region_defined, (HANDLE)1);
        DeleteObject(current);
        return;
    }
    if (FAILED(GetThemeBackgroundRegion(theme, NULL, part, state, &caption, &region)) || !region)
    {
        DeleteObject(current);
        return;
    }
    body = CreateRectRgn(0, caption_height, width, height);
    CombineRgn(region, region, body, RGN_OR);
    DeleteObject(body);
    if (EqualRgn(current, region)) DeleteObject(region);
    else
    {
        /* SetWindowRgn can synchronously repaint the non-client area. */
        SetPropW(hwnd, theme_region_updating, (HANDLE)1);
        SetPropW(hwnd, theme_region_owned, (HANDLE)1);
        if (!user_api.pSetWindowRgn(hwnd, region, FALSE))
        {
            RemovePropW(hwnd, theme_region_owned);
            DeleteObject(region);
        }
        RemovePropW(hwnd, theme_region_updating);
    }
    DeleteObject(current);
}

struct nc_button
{
    int part, state;
    int which;      /* NCFRAME_BTN_* */
    RECT rect;
};

static int nc_button_state(UINT flags, BOOL disabled, BOOL pressed_this)
{
    if (disabled) return 4;
    if (pressed_this && (flags & NCFRAME_DOWN)) return 3;
    if (!(flags & NCFRAME_ACTIVE)) return 5;
    return 1;
}

static void nc_draw_caption_text(HTHEME theme, HDC hdc, HWND hwnd, int part, int state, RECT rect, BOOL small_caption)
{
    NONCLIENTMETRICSW ncm = {.cbSize = sizeof(ncm)};
    WCHAR text[256];
    COLORREF color = RGB(255, 255, 255), shadow = 0;
    int shadow_type = 0, fallback;
    POINT offset = {1, 1};
    HFONT font, old_font;

    if (!GetWindowTextW(hwnd, text, ARRAY_SIZE(text)) || !SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return;

    /* Visual styles may omit the text properties on the maximized caption parts: use the restored part's */
    if (part == WP_MAXCAPTION) fallback = WP_CAPTION;
    else if (part == WP_SMALLMAXCAPTION) fallback = WP_SMALLCAPTION;
    else fallback = part;
    if (FAILED(GetThemeColor(theme, part, state, TMT_TEXTCOLOR, &color)))
        GetThemeColor(theme, fallback, state, TMT_TEXTCOLOR, &color);
    if (FAILED(GetThemeEnumValue(theme, part, state, TMT_TEXTSHADOWTYPE, &shadow_type)))
        GetThemeEnumValue(theme, fallback, state, TMT_TEXTSHADOWTYPE, &shadow_type);
    if (FAILED(GetThemeColor(theme, part, state, TMT_TEXTSHADOWCOLOR, &shadow)))
        GetThemeColor(theme, fallback, state, TMT_TEXTSHADOWCOLOR, &shadow);
    if (FAILED(GetThemePosition(theme, part, state, TMT_TEXTSHADOWOFFSET, &offset)))
        GetThemePosition(theme, fallback, state, TMT_TEXTSHADOWOFFSET, &offset);

    font = CreateFontIndirectW(small_caption ? &ncm.lfSmCaptionFont : &ncm.lfCaptionFont);
    old_font = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    if (shadow_type)
    {
        RECT sr = rect;
        OffsetRect(&sr, offset.x, offset.y);
        SetTextColor(hdc, shadow);
        DrawTextW(hdc, text, -1, &sr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_LEFT);
    }
    SetTextColor(hdc, color);
    DrawTextW(hdc, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_LEFT);
    SelectObject(hdc, old_font);
    DeleteObject(font);
}

static HICON nc_get_icon(HWND hwnd)
{
    HICON icon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0);
    if (!icon) icon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0);
    if (!icon) icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
    if (!icon) icon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
    return icon;
}

#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

static int dwm_frame_effects = -1, dwm_aero_frames = -1;
static COLORREF dwm_accent_color = CLR_INVALID;
static BOOL dwm_accent_color_cached = FALSE;

void uxtheme_reset_dwm_settings(void)
{
    dwm_frame_effects = dwm_aero_frames = -1;
    dwm_accent_color_cached = FALSE;
}

/* DwmFrameStyle 1 (P13): Vista/7 Aero glass frames instead of the Windows 10/11 frame */
static BOOL get_dwm_aero_frames(void)
{
    if (dwm_aero_frames == -1)
    {
        DWORD val = 0, size = sizeof(val);
        dwm_aero_frames = !RegGetValueW(HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"DwmFrameStyle",
                                        RRF_RT_REG_DWORD, NULL, &val, &size) && val;
    }
    return dwm_aero_frames;
}

/* Every captioned top-level window has an Aero glass frame, as with Vista/7's DWM: ask dwmapi once (it makes the
 * frame glass only with DwmAlpha and DwmFrameEffects on) */
static void aero_glass(HWND hwnd, DWORD style)
{
    static HRESULT (WINAPI *extend)(HWND, const MARGINS *);
    static BOOL loaded;
    static const MARGINS none;

    if ((style & WS_CHILD) || (style & WS_CAPTION) != WS_CAPTION || GetPropW(hwnd, L"__wine_dwm_frame") ||
        GetPropW(hwnd, L"__wine_aero_asked"))
        return;
    SetPropW(hwnd, L"__wine_aero_asked", (HANDLE)1);
    if (!loaded)
    {
        HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");   /* kept loaded: the pointer is cached */
        if (dwmapi) extend = (void *)GetProcAddress(dwmapi, "DwmExtendFrameIntoClientArea");
        loaded = TRUE;
    }
    if (extend) extend(hwnd, &none);
}

static BOOL get_dwm_frame_effects(void)
{
    if (dwm_frame_effects == -1)
    {
        HKEY hkey;
        dwm_frame_effects = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
        {
            DWORD val = 0, size = sizeof(val), type;
            if (RegQueryValueExW(hkey, L"DwmFrameEffects", NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS && type == REG_DWORD)
                dwm_frame_effects = (val != 0);
            RegCloseKey(hkey);
        }
    }
    return dwm_frame_effects != 0;
}

static COLORREF get_dwm_accent_color(void)
{
    if (!dwm_accent_color_cached)
    {
        HKEY hkey;
        dwm_accent_color = CLR_INVALID;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_READ, &hkey) == ERROR_SUCCESS)
        {
            DWORD val = 0, size = sizeof(val), type;
            if (RegQueryValueExW(hkey, L"ColorizationColor", NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS && type == REG_DWORD && val != 0)
            {
                dwm_accent_color = RGB((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
            }
            else
            {
                size = sizeof(val);
                if (RegQueryValueExW(hkey, L"AccentColor", NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS && type == REG_DWORD && val != 0)
                    dwm_accent_color = RGB((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
            }
            RegCloseKey(hkey);
        }
        dwm_accent_color_cached = TRUE;
    }
    return dwm_accent_color;
}

static HRESULT query_dwm_window_attribute(HWND hwnd, DWORD attribute, void *pv, DWORD size)
{
    static HRESULT (WINAPI *pDwmGetWindowAttribute)(HWND, DWORD, void *, DWORD);
    static BOOL initialized = FALSE;

    if (!initialized)
    {
        HMODULE hmod = LoadLibraryW(L"dwmapi.dll");   /* kept loaded: the pointer is cached */
        if (hmod) pDwmGetWindowAttribute = (void *)GetProcAddress(hmod, "DwmGetWindowAttribute");
        initialized = TRUE;
    }
    if (!pDwmGetWindowAttribute) return E_NOTIMPL;
    return pDwmGetWindowAttribute(hwnd, attribute, pv, size);
}

static void draw_modern_button(HDC hdc, const struct nc_button *btn, BOOL dark_mode,
                               COLORREF text_color, COLORREF bg_color, UINT dpi,
                               BOOL maximized, BOOL disabled, BOOL pressed, BOOL hot)
{
    int cx = (btn->rect.left + btn->rect.right) / 2;
    int cy = (btn->rect.top + btn->rect.bottom) / 2;
    int pen_w = MulDiv(1, dpi, 96);
    COLORREF btn_bg, pen_color;
    HBRUSH br;
    HPEN pen, old_pen;

    if (pen_w < 1) pen_w = 1;

    hot = hot && !disabled;
    if (btn->which == NCFRAME_BTN_CLOSE)
    {
        if (pressed || hot)
        {
            btn_bg = pressed ? RGB(241, 112, 122) : RGB(232, 17, 35);
            pen_color = RGB(255, 255, 255);
        }
        else
        {
            btn_bg = bg_color;
            pen_color = disabled ? (dark_mode ? RGB(100, 100, 100) : RGB(160, 160, 160)) : text_color;
        }
    }
    else
    {
        if (pressed)
            btn_bg = dark_mode ? RGB(70, 70, 70) : RGB(204, 204, 204);
        else if (hot)
            btn_bg = dark_mode ? RGB(51, 51, 51) : RGB(229, 229, 229);
        else
            btn_bg = bg_color;
        pen_color = disabled ? (dark_mode ? RGB(100, 100, 100) : RGB(160, 160, 160)) : text_color;
    }

    /* CLR_INVALID: a glass caption, nothing behind the glyph */
    if (btn_bg != CLR_INVALID)
    {
        br = CreateSolidBrush(btn_bg);
        FillRect(hdc, &btn->rect, br);
        DeleteObject(br);
    }

    pen = CreatePen(PS_SOLID, pen_w, pen_color);
    old_pen = SelectObject(hdc, pen);

    switch (btn->which)
    {
    case NCFRAME_BTN_CLOSE:
    {
        int sz = MulDiv(10, dpi, 96);
        MoveToEx(hdc, cx - sz / 2, cy - sz / 2, NULL);
        LineTo(hdc, cx + sz / 2 + 1, cy + sz / 2 + 1);
        MoveToEx(hdc, cx + sz / 2, cy - sz / 2, NULL);
        LineTo(hdc, cx - sz / 2 - 1, cy + sz / 2 + 1);
        break;
    }
    case NCFRAME_BTN_MAX:
    {
        if (maximized)
        {
            int sz = MulDiv(8, dpi, 96);
            int off = MulDiv(2, dpi, 96);
            HBRUSH hollow_br = GetStockObject(NULL_BRUSH);
            HBRUSH fill_br;
            RECT front_rect;

            if (off < 2) off = 2;
            SelectObject(hdc, hollow_br);
            Rectangle(hdc, cx - sz / 2 + off, cy - sz / 2 - off, cx + sz / 2 + off + 1, cy + sz / 2 - off + 1);

            SetRect(&front_rect, cx - sz / 2, cy - sz / 2 + off, cx + sz / 2 + 1, cy + sz / 2 + off + 1);
            if (btn_bg != CLR_INVALID)
            {
                fill_br = CreateSolidBrush(btn_bg);
                FillRect(hdc, &front_rect, fill_br);
                DeleteObject(fill_br);
            }

            Rectangle(hdc, front_rect.left, front_rect.top, front_rect.right, front_rect.bottom);
        }
        else
        {
            int sz = MulDiv(10, dpi, 96);
            HBRUSH hollow_br = GetStockObject(NULL_BRUSH);
            SelectObject(hdc, hollow_br);
            Rectangle(hdc, cx - sz / 2, cy - sz / 2, cx + sz / 2 + 1, cy + sz / 2 + 1);
        }
        break;
    }
    case NCFRAME_BTN_MIN:
    {
        int sz = MulDiv(10, dpi, 96);
        MoveToEx(hdc, cx - sz / 2, cy, NULL);
        LineTo(hdc, cx + sz / 2 + 1, cy);
        break;
    }
    }

    SelectObject(hdc, old_pen);
    DeleteObject(pen);
}

static void nc_update_modern_region(HWND hwnd, int width, int height)
{
    BOOL owned = !!GetPropW(hwnd, theme_region_owned);
    HRGN current, region;
    int current_type;

    if (GetPropW(hwnd, theme_region_updating) || GetPropW(hwnd, app_region_defined)) return;
    current = CreateRectRgn(0, 0, 0, 0);
    if (!current) return;
    if (GetPropW(hwnd, WEX_WAYLAND_HOST_PROP))
    {
        HRGN outer = GetPropW(hwnd, WEX_WAYLAND_FRAME_OUTER_PROP);
        current_type = outer ? CombineRgn(current, outer, NULL, RGN_COPY) : ERROR;
    }
    else current_type = GetWindowRgn(hwnd, current);
    if (current_type != ERROR && !owned)
    {
        SetPropW(hwnd, app_region_defined, (HANDLE)1);
        DeleteObject(current);
        return;
    }

    /* Like a themed frame, the flat frame needs the whole window surface.
     * Without a region win32u may crop off native decorations, including all
     * of the non-client pixels that we are about to paint. */
    region = CreateRectRgn(0, 0, width, height);
    if (region)
    {
        if (EqualRgn(current, region)) DeleteObject(region);
        else
        {
            SetPropW(hwnd, theme_region_updating, (HANDLE)1);
            SetPropW(hwnd, theme_region_owned, (HANDLE)1);
            if (!user_api.pSetWindowRgn(hwnd, region, FALSE))
            {
                if (!owned) RemovePropW(hwnd, theme_region_owned);
                DeleteObject(region);
            }
            RemovePropW(hwnd, theme_region_updating);
        }
    }
    DeleteObject(current);
}

/* glass: the caption and borders keep the backdrop (no background fill) unless the app or the accent colors them;
 * pressed: the button drawn pressed in a full repaint, for glass frames that cannot repaint a single button */
static BOOL paint_modern_nc(HWND hwnd, HDC hdc, RECT rect, DWORD style, DWORD ex_style, UINT flags,
                            BOOL glass, UINT pressed)
{
    UINT target = flags >> NCFRAME_PART_SHIFT;
    BOOL active = !!(flags & NCFRAME_ACTIVE);
    BOOL tool = !!(ex_style & WS_EX_TOOLWINDOW);
    BOOL max = !!(style & WS_MAXIMIZE);
    BOOL rtl = !!(ex_style & WS_EX_LAYOUTRTL);
    int w = rect.right - rect.left, h = rect.bottom - rect.top;
    UINT dpi;
    int bw, bh, cap_h, top_h, btn_w, btn_off;
    int nbtn = 0, i;
    struct nc_button btn[3];
    BOOL dark_mode = FALSE;
    DWORD attr_val = 0;
    COLORREF caption_attr = DWMWA_COLOR_DEFAULT;
    COLORREF text_attr = DWMWA_COLOR_DEFAULT;
    COLORREF border_attr = DWMWA_COLOR_DEFAULT;
    COLORREF caption_bg, text_color, border_color, button_bg;
    BOOL clear_caption = FALSE;
    RECT rc_win, rc_client;

    if ((style & WS_CAPTION) != WS_CAPTION) return FALSE;
    if (style & WS_MINIMIZE) return FALSE;

    /* Custom NC (0 client inset): leave alone */
    if (GetWindowRect(hwnd, &rc_win) && GetClientRect(hwnd, &rc_client))
    {
        if ((rc_win.right - rc_win.left == rc_client.right - rc_client.left) &&
            (rc_win.bottom - rc_win.top == rc_client.bottom - rc_client.top))
            return FALSE;
    }

    dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;

    bw = max ? 0 : GetSystemMetricsForDpi((style & WS_THICKFRAME) ? SM_CXFRAME : SM_CXDLGFRAME, dpi);
    bh = max ? 0 : GetSystemMetricsForDpi((style & WS_THICKFRAME) ? SM_CYFRAME : SM_CYDLGFRAME, dpi);
    cap_h = GetSystemMetricsForDpi(tool ? SM_CYSMCAPTION : SM_CYCAPTION, dpi);
    top_h = bh + cap_h;
    btn_w = GetSystemMetricsForDpi(SM_CXSIZE, dpi);
    /* Glass frames keep the outer bands as an invisible resize area; the visible frame and its buttons are inset. */
    btn_off = (glass && !max) ? bw : 0;

    if (!target)
        nc_update_modern_region(hwnd, w, h);

    /* Read DWM window attributes */
    if (SUCCEEDED(query_dwm_window_attribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &attr_val, sizeof(attr_val))) && attr_val)
        dark_mode = TRUE;
    if (SUCCEEDED(query_dwm_window_attribute(hwnd, DWMWA_CAPTION_COLOR, &attr_val, sizeof(attr_val))))
        caption_attr = (COLORREF)attr_val;
    if (SUCCEEDED(query_dwm_window_attribute(hwnd, DWMWA_TEXT_COLOR, &attr_val, sizeof(attr_val))))
        text_attr = (COLORREF)attr_val;
    if (SUCCEEDED(query_dwm_window_attribute(hwnd, DWMWA_BORDER_COLOR, &attr_val, sizeof(attr_val))))
        border_attr = (COLORREF)attr_val;

    /* Determine caption background */
    if (caption_attr != DWMWA_COLOR_DEFAULT && caption_attr != DWMWA_COLOR_NONE)
    {
        caption_bg = caption_attr;
    }
    else
    {
        COLORREF accent = get_dwm_accent_color();
        if (accent != CLR_INVALID && active)
        {
            caption_bg = accent;
        }
        else if (dark_mode)
        {
            caption_bg = active ? RGB(32, 32, 32) : RGB(45, 45, 45);
            clear_caption = glass;
        }
        else
        {
            caption_bg = active ? RGB(240, 240, 240) : RGB(245, 245, 245);
            clear_caption = glass;
        }
    }
    button_bg = clear_caption ? CLR_INVALID : caption_bg;

    /* Determine text color */
    if (text_attr != DWMWA_COLOR_DEFAULT && text_attr != DWMWA_COLOR_NONE)
    {
        text_color = text_attr;
    }
    else if (dark_mode)
    {
        text_color = active ? RGB(255, 255, 255) : RGB(160, 160, 160);
    }
    else
    {
        int lum = (GetRValue(caption_bg) * 299 + GetGValue(caption_bg) * 587 + GetBValue(caption_bg) * 114) / 1000;
        if (lum < 128)
            text_color = active ? RGB(255, 255, 255) : RGB(180, 180, 180);
        else
            text_color = active ? RGB(0, 0, 0) : RGB(128, 128, 128);
    }

    /* Determine border color */
    if (border_attr != DWMWA_COLOR_DEFAULT && border_attr != DWMWA_COLOR_NONE)
    {
        border_color = border_attr;
    }
    else
    {
        COLORREF accent = get_dwm_accent_color();
        if (accent != CLR_INVALID && active)
            border_color = accent;
        else if (dark_mode)
            border_color = active ? RGB(80, 80, 80) : RGB(60, 60, 60);
        else
            border_color = active ? RGB(180, 180, 180) : RGB(210, 210, 210);
    }

    /* Setup buttons */
    if (style & WS_SYSMENU)
    {
        if (rtl)
        {
            btn[nbtn].which = NCFRAME_BTN_CLOSE;
            SetRect(&btn[nbtn].rect, btn_off, 0, btn_off + btn_w, top_h);
            nbtn++;
            if (!tool && (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)))
            {
                btn[nbtn].which = NCFRAME_BTN_MAX;
                SetRect(&btn[nbtn].rect, btn_off + btn_w, 0, btn_off + 2 * btn_w, top_h);
                nbtn++;
                btn[nbtn].which = NCFRAME_BTN_MIN;
                SetRect(&btn[nbtn].rect, btn_off + 2 * btn_w, 0, btn_off + 3 * btn_w, top_h);
                nbtn++;
            }
        }
        else
        {
            btn[nbtn].which = NCFRAME_BTN_CLOSE;
            SetRect(&btn[nbtn].rect, w - btn_off - btn_w, 0, w - btn_off, top_h);
            nbtn++;
            if (!tool && (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)))
            {
                btn[nbtn].which = NCFRAME_BTN_MAX;
                SetRect(&btn[nbtn].rect, w - btn_off - 2 * btn_w, 0, w - btn_off - btn_w, top_h);
                nbtn++;
                btn[nbtn].which = NCFRAME_BTN_MIN;
                SetRect(&btn[nbtn].rect, w - btn_off - 3 * btn_w, 0, w - btn_off - 2 * btn_w, top_h);
                nbtn++;
            }
        }
    }

    if (target)
    {
        for (i = 0; i < nbtn; i++)
        {
            if (btn[i].which == target)
            {
                BOOL disabled = FALSE;
                if (btn[i].which == NCFRAME_BTN_MAX && !(style & WS_MAXIMIZEBOX)) disabled = TRUE;
                if (btn[i].which == NCFRAME_BTN_MIN && !(style & WS_MINIMIZEBOX)) disabled = TRUE;
                if (btn[i].which == NCFRAME_BTN_CLOSE && (flags & NCFRAME_GRAYED)) disabled = TRUE;
                draw_modern_button(hdc, &btn[i], dark_mode, text_color, button_bg, dpi, max, disabled,
                                   !!(flags & NCFRAME_DOWN), FALSE);
                return TRUE;
            }
        }
        return TRUE;
    }

    /* 1. Caption background */
    if (!clear_caption)
    {
        RECT r_cap = {0, 0, w, top_h};
        HBRUSH br_cap = CreateSolidBrush(caption_bg);
        FillRect(hdc, &r_cap, br_cap);
        DeleteObject(br_cap);
    }

    /* 2. Side and bottom borders (restored only) */
    if (!max && !clear_caption && (bw > 0 || bh > 0))
    {
        COLORREF frame_bg = dark_mode ? (active ? RGB(32, 32, 32) : RGB(45, 45, 45))
                                      : (active ? RGB(240, 240, 240) : RGB(245, 245, 245));
        HBRUSH br_frame = CreateSolidBrush(frame_bg);
        RECT r_side;
        if (bw > 0)
        {
            SetRect(&r_side, 0, top_h, bw, h - bh);
            FillRect(hdc, &r_side, br_frame);
            SetRect(&r_side, w - bw, top_h, w, h - bh);
            FillRect(hdc, &r_side, br_frame);
        }
        if (bh > 0)
        {
            SetRect(&r_side, 0, h - bh, w, h);
            FillRect(hdc, &r_side, br_frame);
        }
        DeleteObject(br_frame);
    }

    /* 3. 1px outer frame (restored only, unless DWMWA_COLOR_NONE); glass frames draw it along the visible frame */
    if (!max && border_attr != DWMWA_COLOR_NONE)
    {
        HBRUSH br_border = CreateSolidBrush(border_color);
        RECT r_edge;
        if (glass)
        {
            SetRect(&r_edge, bw - 1, 0, w - bw + 1, 1);
            FillRect(hdc, &r_edge, br_border);
            SetRect(&r_edge, bw - 1, 0, bw, h - bh + 1);
            FillRect(hdc, &r_edge, br_border);
            SetRect(&r_edge, w - bw, 0, w - bw + 1, h - bh + 1);
            FillRect(hdc, &r_edge, br_border);
            SetRect(&r_edge, bw - 1, h - bh, w - bw + 1, h - bh + 1);
            FillRect(hdc, &r_edge, br_border);
        }
        else
        {
            SetRect(&r_edge, 0, 0, w, 1);
            FillRect(hdc, &r_edge, br_border);
            SetRect(&r_edge, 0, 0, 1, h);
            FillRect(hdc, &r_edge, br_border);
            SetRect(&r_edge, w - 1, 0, w, h);
            FillRect(hdc, &r_edge, br_border);
            SetRect(&r_edge, 0, h - 1, w, h);
            FillRect(hdc, &r_edge, br_border);
        }
        DeleteObject(br_border);
    }

    /* 4. Icon and window title */
    {
        int text_left = bw + 6;
        int text_right = w - bw;

        if (rtl)
        {
            text_left = nbtn ? btn[nbtn - 1].rect.right + 6 : bw + 6;
            text_right = w - bw - 6;
            if ((style & WS_SYSMENU) && !tool)
            {
                HICON icon = nc_get_icon(hwnd);
                int isz = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
                if (icon)
                {
                    DrawIconEx(hdc, text_right - isz, (top_h - isz) / 2, icon, isz, isz, 0, NULL, DI_NORMAL);
                    text_right -= isz + 4;
                }
            }
        }
        else
        {
            if ((style & WS_SYSMENU) && !tool)
            {
                HICON icon = nc_get_icon(hwnd);
                int isz = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
                if (icon)
                {
                    DrawIconEx(hdc, text_left, (top_h - isz) / 2, icon, isz, isz, 0, NULL, DI_NORMAL);
                    text_left += isz + 6;
                }
            }
            if (nbtn > 0)
                text_right = btn[nbtn - 1].rect.left - 6;
        }

        if (text_right > text_left)
        {
            NONCLIENTMETRICSW ncm = {.cbSize = sizeof(ncm)};
            WCHAR text[256];
            if (GetWindowTextW(hwnd, text, ARRAY_SIZE(text)) &&
                SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            {
                HFONT font = CreateFontIndirectW(tool ? &ncm.lfSmCaptionFont : &ncm.lfCaptionFont);
                HFONT old_font = SelectObject(hdc, font);
                RECT r_text;
                SetRect(&r_text, text_left, 0, text_right, top_h);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, text_color);
                DrawTextW(hdc, text, -1, &r_text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_LEFT);
                SelectObject(hdc, old_font);
                DeleteObject(font);
            }
        }
    }

    /* 5. Caption buttons */
    for (i = 0; i < nbtn; i++)
    {
        BOOL disabled = FALSE;
        if (btn[i].which == NCFRAME_BTN_MAX && !(style & WS_MAXIMIZEBOX)) disabled = TRUE;
        if (btn[i].which == NCFRAME_BTN_MIN && !(style & WS_MINIMIZEBOX)) disabled = TRUE;
        if (btn[i].which == NCFRAME_BTN_CLOSE && (flags & NCFRAME_GRAYED)) disabled = TRUE;
        draw_modern_button(hdc, &btn[i], dark_mode, text_color, button_bg, dpi, max, disabled,
                           pressed && btn[i].which == pressed,
                           btn[i].which == HandleToUlong(GetPropW(hwnd, L"__wine_nc_hot")));
    }

    return TRUE;
}

/* DWMWindow parts as Windows 7 numbers them; the glyphs come in 96, 120, 144 and 192 dpi variants in a row */
enum
{
    DWMW_BOTTOMFRAME = 1, DWMW_BUTTONACTIVECAPTION = 3, DWMW_BUTTONINACTIVECAPTION = 4,
    DWMW_BUTTONACTIVECLOSE = 7, DWMW_BUTTONINACTIVECLOSE = 8, DWMW_BUTTONACTIVECLOSEALONE = 9,
    DWMW_BUTTONINACTIVECLOSEALONE = 10, DWMW_BUTTONCLOSEGLYPH96 = 12, DWMW_BUTTONMAXGLYPH96 = 21,
    DWMW_BUTTONMINGLYPH96 = 25, DWMW_BUTTONRESTOREGLYPH96 = 29, DWMW_LEFTFRAME = 33, DWMW_RIGHTFRAME = 41,
    DWMW_TEXTGLOW = 55, DWMW_TOPFRAME = 56,
};

/* Vista/7 Aero glass frame (DwmFrameStyle 1): the theme's DWMWindow frame pieces over the glass, the title over its
 * glow and the caption buttons hanging from the top edge. Their geometry goes to the "__wine_nc_buttons" property
 * for hit testing. ponytail: button sizes and places are Windows 7's at 96 dpi as estimated, unmeasured. Returns
 * FALSE without DWMWindow parts. */
static BOOL paint_aero_nc(HWND hwnd, HDC hdc, RECT rect, DWORD style, DWORD ex_style, UINT flags, UINT pressed)
{
    static const int glyph_dpi[4] = { 96, 120, 144, 192 };
    BOOL active = !!(flags & NCFRAME_ACTIVE), tool = !!(ex_style & WS_EX_TOOLWINDOW);
    BOOL max = !!(style & WS_MAXIMIZE), rtl = !!(ex_style & WS_EX_LAYOUTRTL);
    int w = rect.right - rect.left, h = rect.bottom - rect.top, state = active ? 1 : 2, i, glyph = 0;
    UINT dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
    int bw = max ? 0 : GetSystemMetricsForDpi((style & WS_THICKFRAME) ? SM_CXFRAME : SM_CXDLGFRAME, dpi);
    int bh = max ? 0 : GetSystemMetricsForDpi((style & WS_THICKFRAME) ? SM_CYFRAME : SM_CYDLGFRAME, dpi);
    int top_h = bh + GetSystemMetricsForDpi(tool ? SM_CYSMCAPTION : SM_CYCAPTION, dpi);
    int close_w = MulDiv(43, dpi, 96), btn_w = MulDiv(26, dpi, 96), btn_h = MulDiv(19, dpi, 96);
    int right = max ? MulDiv(2, dpi, 96) : bw - MulDiv(2, dpi, 96), top = max ? GetSystemMetricsForDpi(SM_CYFRAME, dpi) : 0;
    BOOL boxes = !tool && (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
    UINT hot = HandleToUlong(GetPropW(hwnd, L"__wine_nc_hot"));   /* win32u: the button under the pointer */
    int text_left, text_right;
    HTHEME theme;
    RECT r;

    if ((style & WS_CAPTION) != WS_CAPTION || (style & WS_MINIMIZE)) return FALSE;
    if (!(theme = OpenThemeDataForDpi(NULL, L"DWMWindow", dpi))) return FALSE;
    if (!IsThemePartDefined(theme, DWMW_BUTTONACTIVECLOSE, 0))
    {
        CloseThemeData(theme);
        return FALSE;
    }
    nc_update_modern_region(hwnd, w, h);
    if (right < 0) right = 0;
    while (glyph < 3 && dpi >= (UINT)glyph_dpi[glyph + 1]) glyph++;

    /* 1. The frame pieces: highlights and edges with alpha over the glass */
    SetRect(&r, 0, 0, w, top_h);
    DrawThemeBackground(theme, hdc, DWMW_TOPFRAME, state, &r, NULL);
    if (!max)
    {
        SetRect(&r, 0, top_h, bw, h - bh);
        DrawThemeBackground(theme, hdc, DWMW_LEFTFRAME, state, &r, NULL);
        SetRect(&r, w - bw, top_h, w, h - bh);
        DrawThemeBackground(theme, hdc, DWMW_RIGHTFRAME, state, &r, NULL);
        SetRect(&r, 0, h - bh, w, h);
        DrawThemeBackground(theme, hdc, DWMW_BOTTOMFRAME, state, &r, NULL);
    }

    /* 2. The caption buttons, right to left: close, maximize or restore, minimize */
    if (style & WS_SYSMENU)
    {
        static const struct { UINT which; int glyph; } order[] =
            { { NCFRAME_BTN_CLOSE, DWMW_BUTTONCLOSEGLYPH96 }, { NCFRAME_BTN_MAX, DWMW_BUTTONMAXGLYPH96 },
              { NCFRAME_BTN_MIN, DWMW_BUTTONMINGLYPH96 } };
        int x = w - right;

        for (i = 0; i < (boxes ? 3 : 1); i++)
        {
            int bwidth = i ? btn_w : close_w, part, bstate, glyph_part = order[i].glyph;
            BOOL disabled = (order[i].which == NCFRAME_BTN_MAX && !(style & WS_MAXIMIZEBOX)) ||
                            (order[i].which == NCFRAME_BTN_MIN && !(style & WS_MINIMIZEBOX)) ||
                            (order[i].which == NCFRAME_BTN_CLOSE && (flags & NCFRAME_GRAYED));
            SIZE size;
            RECT g;

            if (!i) part = boxes ? (active ? DWMW_BUTTONACTIVECLOSE : DWMW_BUTTONINACTIVECLOSE)
                                 : (active ? DWMW_BUTTONACTIVECLOSEALONE : DWMW_BUTTONINACTIVECLOSEALONE);
            else part = active ? DWMW_BUTTONACTIVECAPTION : DWMW_BUTTONINACTIVECAPTION;
            if (order[i].which == NCFRAME_BTN_MAX && max) glyph_part = DWMW_BUTTONRESTOREGLYPH96;
            bstate = disabled ? 4 : pressed == order[i].which ? 3 : hot == order[i].which ? 2 : 1;
            SetRect(&r, x - bwidth, top, x, top + btn_h);
            if (rtl) SetRect(&r, w - r.right, r.top, w - r.left, r.bottom);
            DrawThemeBackground(theme, hdc, part, bstate, &r, NULL);
            if (SUCCEEDED(GetThemePartSize(theme, hdc, glyph_part + glyph, bstate, NULL, TS_TRUE, &size)))
            {
                SetRect(&g, (r.left + r.right - size.cx) / 2, (r.top + r.bottom - size.cy) / 2, 0, 0);
                g.right = g.left + size.cx;
                g.bottom = g.top + size.cy;
                DrawThemeBackground(theme, hdc, glyph_part + glyph, bstate, &g, NULL);
            }
            x -= bwidth;
        }
        SetPropW(hwnd, L"__wine_nc_buttons",
                 ULongToHandle(NCFRAME_BUTTONS(min(close_w, 127), min(btn_w, 127), min(right, 63), min(top, 31),
                                               min(btn_h, 127))));
        text_right = x - MulDiv(6, dpi, 96);
    }
    else
    {
        RemovePropW(hwnd, L"__wine_nc_buttons");
        text_right = w - bw;
    }

    /* 3. Icon and title over the glow */
    text_left = bw + MulDiv(6, dpi, 96);
    if ((style & WS_SYSMENU) && !tool)
    {
        HICON icon = nc_get_icon(hwnd);
        int isz = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        if (icon)
        {
            DrawIconEx(hdc, text_left, max(bh, (top_h + bh - isz) / 2), icon, isz, isz, 0, NULL, DI_NORMAL);
            text_left += isz + MulDiv(6, dpi, 96);
        }
    }
    if (text_right > text_left)
    {
        NONCLIENTMETRICSW ncm = { .cbSize = sizeof(ncm) };
        WCHAR text[256];
        if (GetWindowTextW(hwnd, text, ARRAY_SIZE(text)) &&
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        {
            HFONT font = CreateFontIndirectW(tool ? &ncm.lfSmCaptionFont : &ncm.lfCaptionFont);
            HFONT old_font = SelectObject(hdc, font);
            RECT glow, text_rect;
            SetRect(&text_rect, text_left, bh, text_right, top_h);
            if (rtl) SetRect(&text_rect, w - text_right, bh, w - text_left, top_h);
            glow = text_rect;
            DrawTextW(hdc, text, -1, &glow, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS |
                      DT_CALCRECT | (rtl ? DT_RIGHT : DT_LEFT));
            glow.right = min(glow.right, text_rect.right);
            OffsetRect(&glow, 0, (text_rect.bottom - text_rect.top - (glow.bottom - glow.top)) / 2);
            InflateRect(&glow, MulDiv(10, dpi, 96), MulDiv(4, dpi, 96));
            DrawThemeBackground(theme, hdc, DWMW_TEXTGLOW, 1, &glow, NULL);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 0, 0));
            DrawTextW(hdc, text, -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS |
                      (rtl ? DT_RIGHT : DT_LEFT));
            SelectObject(hdc, old_font);
            DeleteObject(font);
        }
    }
    CloseThemeData(theme);
    return TRUE;
}

/* Glass frame (DWM P10b; dwmapi sets __wine_dwm_frame when DwmAlpha and DwmFrameEffects are on): the surface keeps
 * per-pixel alpha and the compositor blends the whole frame. The frame is painted twice, over black and over white;
 * the difference is each pixel's coverage and the black pass its premultiplied color, so antialiased text and icons
 * keep their edges over the backdrop. A pressed button repaints the whole frame. */
static BOOL draw_glass_nc_frame(HWND hwnd, HDC hdc, RECT rect, DWORD style, DWORD ex_style, UINT flags)
{
    int w = rect.right - rect.left, h = rect.bottom - rect.top, i, n;
    BITMAPINFO info = {{ sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB }};
    UINT pressed = (flags & NCFRAME_DOWN) ? flags >> NCFRAME_PART_SHIFT : 0;
    RECT local = { 0, 0, w, h }, client;
    HBITMAP bmp[2] = { 0 };
    HDC dc[2] = { 0 };
    DWORD *bits[2];
    BOOL ret = FALSE;

    if (w <= 0 || h <= 0) return FALSE;
    for (i = 0; i < 2; i++)
    {
        if (!(dc[i] = CreateCompatibleDC(hdc)) ||
            !(bmp[i] = CreateDIBSection(dc[i], &info, DIB_RGB_COLORS, (void **)&bits[i], NULL, 0)))
            goto done;
        SelectObject(dc[i], bmp[i]);
        FillRect(dc[i], &local, GetStockObject(i ? WHITE_BRUSH : BLACK_BRUSH));
        /* an Aero frame without DWMWindow parts falls back to the modern one */
        if (GetPropW(hwnd, L"__wine_dwm_frame") == (HANDLE)2 &&
            paint_aero_nc(hwnd, dc[i], local, style, ex_style, flags & ((1u << NCFRAME_PART_SHIFT) - 1), pressed))
            continue;
        RemovePropW(hwnd, L"__wine_nc_buttons");
        if (!paint_modern_nc(hwnd, dc[i], local, style, ex_style, flags & ((1u << NCFRAME_PART_SHIFT) - 1),
                             TRUE, pressed))
            goto done;
    }
    GdiFlush();
    for (n = 0; n < w * h; n++)
    {
        DWORD black = bits[0][n], white = bits[1][n];
        int alpha = 255 - (int)((white >> 8) & 0xff) + (int)((black >> 8) & 0xff);
        DWORD r = black >> 16 & 0xff, g = black >> 8 & 0xff, b = black & 0xff;

        alpha = max(0, min(255, alpha));
        r = min(r, (DWORD)alpha);
        g = min(g, (DWORD)alpha);
        b = min(b, (DWORD)alpha);
        bits[0][n] = (DWORD)alpha << 24 | r << 16 | g << 8 | b;
    }
    /* only the frame: the client keeps what the application painted */
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, 0, (POINT *)&client, 2);
    GetWindowRect(hwnd, &local);
    OffsetRect(&client, rect.left - local.left, rect.top - local.top);
    SaveDC(hdc);
    ExcludeClipRect(hdc, client.left, client.top, client.right, client.bottom);
    ret = BitBlt(hdc, rect.left, rect.top, w, h, dc[0], 0, 0, SRCCOPY);
    RestoreDC(hdc, -1);

done:
    for (i = 0; i < 2; i++)
    {
        if (dc[i]) DeleteDC(dc[i]);
        if (bmp[i]) DeleteObject(bmp[i]);
    }
    return ret;
}

static BOOL draw_modern_nc_frame(HWND hwnd, HDC hdc, RECT rect, DWORD style, DWORD ex_style, UINT flags)
{
    if (GetPropW(hwnd, L"__wine_dwm_frame") && (style & WS_CAPTION) == WS_CAPTION && !(style & WS_MINIMIZE) &&
        draw_glass_nc_frame(hwnd, hdc, rect, style, ex_style, flags))
        return TRUE;
    return paint_modern_nc(hwnd, hdc, rect, style, ex_style, flags, FALSE, 0);
}

BOOL WINAPI UXTHEME_NonClientFrameDraw(HWND hwnd, HDC hdc, RECT rect, DWORD style, DWORD ex_style, UINT flags)
{
    if (((flags & NCFRAME_DWM_FRAME_EFFECTS) || get_dwm_frame_effects()) && !get_dwm_aero_frames())
        return draw_modern_nc_frame(hwnd, hdc, rect, style, ex_style, flags);
    if (((flags & NCFRAME_DWM_FRAME_EFFECTS) || get_dwm_frame_effects()))
    {
        /* Aero: a glass frame, else (no glass, or no DWMWindow parts) the themed frame below, as Vista Basic */
        aero_glass(hwnd, style);
        if (GetPropW(hwnd, L"__wine_dwm_frame") == (HANDLE)2 && (style & WS_CAPTION) == WS_CAPTION &&
            !(style & WS_MINIMIZE) && draw_glass_nc_frame(hwnd, hdc, rect, style, ex_style, flags))
            return TRUE;
        RemovePropW(hwnd, L"__wine_nc_buttons");
    }

    UINT target = flags >> NCFRAME_PART_SHIFT;
    BOOL active = flags & NCFRAME_ACTIVE, tool = ex_style & WS_EX_TOOLWINDOW, thick = style & WS_THICKFRAME;
    int w = rect.right - rect.left, h = rect.bottom - rect.top;
    int bw = GetSystemMetrics(thick ? SM_CXFRAME : SM_CXDLGFRAME);
    int bh = GetSystemMetrics(thick ? SM_CYFRAME : SM_CYDLGFRAME);
    int cap_h = GetSystemMetrics(tool ? SM_CYSMCAPTION : SM_CYCAPTION);
    int top_h = bh + cap_h, state = active ? 1 : 2;
    int cap_part, left_part, right_part, bottom_part, i, nbtn = 0, x, btn_h, btn_w, text_left, text_right;
    struct nc_button btn[3];
    HTHEME theme;
    RECT r;
    SIZE size;

    if (!IsThemeActive() || !(GetThemeAppProperties() & STAP_ALLOW_NONCLIENT))
    {
        nc_update_region(hwnd, NULL, 0, 0, w, h, top_h);
        return FALSE;
    }
    theme = UXTHEME_OpenNonClientTheme(GetDpiForWindow(hwnd));
    if (!theme) return FALSE;

    if (tool)
    {
        cap_part = (style & WS_MAXIMIZE) ? WP_SMALLMAXCAPTION : WP_SMALLCAPTION;
        left_part = WP_SMALLFRAMELEFT; right_part = WP_SMALLFRAMERIGHT; bottom_part = WP_SMALLFRAMEBOTTOM;
    }
    else
    {
        cap_part = (style & WS_MAXIMIZE) ? WP_MAXCAPTION : WP_CAPTION;
        left_part = WP_FRAMELEFT; right_part = WP_FRAMERIGHT; bottom_part = WP_FRAMEBOTTOM;
    }

    if (!target) nc_update_region(hwnd, theme, cap_part, state, w, h, top_h);

    /* Use the same caption slots as win32u's handle_nc_hit_test. A theme's
     * natural bitmap width need not equal SM_CXSIZE (foot's maximize glyph
     * otherwise occupied the minimize hit rectangle). Scale art into its slot. */
    btn_h = cap_h - 3;
    btn_w = GetSystemMetrics(SM_CXSIZE);
    size.cx = size.cy = 0;
    if (SUCCEEDED(GetThemePartSize(theme, hdc, tool ? WP_SMALLCLOSEBUTTON : WP_CLOSEBUTTON, 1, NULL, TS_TRUE, &size)) && size.cy > 0)
        btn_h = min(btn_h, size.cy);
    x = w - bw;
    if (style & WS_SYSMENU)
    {
        BOOL has_minmax = style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        int y = bh + (cap_h - 1 - btn_h) / 2;

        btn[nbtn].which = NCFRAME_BTN_CLOSE;
        btn[nbtn].part = tool ? WP_SMALLCLOSEBUTTON : WP_CLOSEBUTTON;
        btn[nbtn].state = nc_button_state(flags, (flags & NCFRAME_GRAYED) && target == NCFRAME_BTN_CLOSE, target == NCFRAME_BTN_CLOSE);
        SetRect(&btn[nbtn].rect, x - cap_h + 1, y, x - 1, y + btn_h);
        x -= cap_h;
        nbtn++;
        if (!tool && has_minmax)
        {
            btn[nbtn].which = NCFRAME_BTN_MAX;
            btn[nbtn].part = (style & WS_MAXIMIZE) ? WP_RESTOREBUTTON : WP_MAXBUTTON;
            btn[nbtn].state = nc_button_state(flags, !(style & WS_MAXIMIZEBOX), target == NCFRAME_BTN_MAX);
            SetRect(&btn[nbtn].rect, x - btn_w + 1, y, x - 1, y + btn_h);
            x -= btn_w;
            nbtn++;
            btn[nbtn].which = NCFRAME_BTN_MIN;
            btn[nbtn].part = WP_MINBUTTON;
            btn[nbtn].state = nc_button_state(flags, !(style & WS_MINIMIZEBOX), target == NCFRAME_BTN_MIN);
            SetRect(&btn[nbtn].rect, x - btn_w + 1, y, x - 1, y + btn_h);
            x -= btn_w;
            nbtn++;
        }
    }

    if (target)
    {
        HRGN clip = NULL;
        for (i = 0; i < nbtn; i++)
            if (btn[i].which == target) { clip = CreateRectRgnIndirect(&btn[i].rect); break; }
        if (!clip) { CloseThemeData(theme); return TRUE; }
        ExtSelectClipRgn(hdc, clip, RGN_AND);
        DeleteObject(clip);
    }

    /* frame and caption background */
    SetRect(&r, 0, 0, w, top_h);
    if (IsThemeBackgroundPartiallyTransparent(theme, cap_part, state)) FillRect(hdc, &r, GetSysColorBrush(COLOR_BTNFACE));
    DrawThemeBackground(theme, hdc, cap_part, state, &r, NULL);
    SetRect(&r, 0, top_h, bw, h - bh);
    DrawThemeBackground(theme, hdc, left_part, state, &r, NULL);
    SetRect(&r, w - bw, top_h, w, h - bh);
    DrawThemeBackground(theme, hdc, right_part, state, &r, NULL);
    SetRect(&r, 0, h - bh, w, h);
    DrawThemeBackground(theme, hdc, bottom_part, state, &r, NULL);

    /* icon and title */
    text_left = bw + 4;
    if ((style & WS_SYSMENU) && !tool)
    {
        HICON icon = nc_get_icon(hwnd);
        int isz = GetSystemMetrics(SM_CXSMICON);
        if (icon) DrawIconEx(hdc, bw + 2, (top_h - isz) / 2, icon, isz, isz, 0, NULL, DI_NORMAL);
        text_left = bw + 2 + isz + 4;
    }
    text_right = nbtn ? btn[nbtn - 1].rect.left - 4 : w - bw;
    SetRect(&r, text_left, 0, text_right, top_h - 2);
    nc_draw_caption_text(theme, hdc, hwnd, cap_part, state, r, tool);

    for (i = 0; i < nbtn; i++)
        DrawThemeBackground(theme, hdc, btn[i].part, btn[i].state, &btn[i].rect, NULL);

    CloseThemeData(theme);
    return TRUE;
}
