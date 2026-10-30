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

BOOL WINAPI UXTHEME_NonClientFrameDraw(HWND hwnd, HDC hdc, RECT rect, DWORD style, DWORD ex_style, UINT flags)
{
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
