/*
 * Copyright (C) 2004 Mike Hearn, for CodeWeavers
 * Copyright (C) 2005 Robert Shearman
 * Copyright (C) 2008 Alexandre Julliard
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

#include <assert.h>

#include <windows.h>
#include <ntuser.h>
#include <commctrl.h>
#include <shellapi.h>

#include <wine/debug.h>
#include <wine/list.h>

#include "explorer_private.h"
#include "shellres.h"
#include "notifyprops.h"
#include "desktop_view.h"
#include "shellfolders.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(systray);

#define TRAY_MINIMIZE_ALL 419

/* the Windows key shortcuts of the taskbar, as on Windows XP (ids as in ReactOS) */
#define IDHK_RUN            0x1f4
#define IDHK_MINIMIZE_ALL   0x1f5
#define IDHK_RESTORE_ALL    0x1f6
#define IDHK_EXPLORE        0x1f8
#define IDHK_FIND           0x1f9
#define IDHK_SYS_PROPERTIES 0x1fd
#define IDHK_DESKTOP        0x1fe
#define IDHK_LOCK           0x1ff
#define TRAY_MINIMIZE_ALL_UNDO 416

struct notify_data_icon
{
    /* data for the icon bitmap */
    UINT width;
    UINT height;
    UINT planes;
    UINT bpp;
};

struct notify_data  /* platform-independent format for NOTIFYICONDATA */
{
    LONG  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    struct notify_data_icon icon_info; /* systray icon bitmap info */
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    } u;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID  guidItem;
    struct notify_data_icon balloon_icon_info; /* balloon icon bitmap info */
    BYTE icon_data[];
};

#define ICON_DISPLAY_HIDDEN -1
#define ICON_DISPLAY_DOCKED -2

/* an individual systray icon, unpacked from the NOTIFYICONDATA and always in unicode */
struct icon
{
    struct list    entry;
    HICON          image;    /* the image to render */
    HWND           owner;    /* the HWND passed in to the Shell_NotifyIcon call */
    HWND           window;   /* the adaptor window */
    BOOL           layered;  /* whether we are using a layered window */
    HWND           tooltip;  /* Icon tooltip */
    UINT           state;    /* state flags */
    UINT           id;       /* the unique id given by the app */
    UINT           callback_message;
    int            display;  /* index in display list, or -1 if hidden */
    WCHAR          tiptext[128]; /* Tooltip text. If empty => tooltip disabled */
    WCHAR          info_text[256];  /* info balloon text */
    WCHAR          info_title[64];  /* info balloon title */
    UINT           info_flags;      /* flags for info balloon */
    UINT           info_timeout;    /* timeout for info balloon */
    HICON          info_icon;       /* info balloon icon */
    UINT           version;         /* notify icon api version */
    DWORD          last_active;     /* tick of the last change by the program, for "hide inactive icons" */
    BOOL           auto_hidden;     /* hidden only because it is inactive */
    int            behavior;        /* NOTIFY_*, -1 until read from the registry */
};

static struct list icon_list = LIST_INIT( icon_list );

struct taskbar_button
{
    struct list entry;
    HWND        hwnd;
    HWND        button;
    BOOL        active;
    BOOL        visible;
};

static struct list taskbar_buttons = LIST_INIT( taskbar_buttons );

static HWND tray_window;

static unsigned int nb_displayed;

static BOOL enable_taskbar; /* show full taskbar, with dedicated systray area */
static BOOL show_systray; /* show a standalone systray window */
static BOOL enable_dock; /* allow systray icons to be docked in the host systray */
static BOOL no_tray_items; /* hide the systray and all systray icons */

static int icon_cx, icon_cy, tray_width, tray_height, tray_top;
static int start_button_width, taskbar_button_width;
static WCHAR start_label[50];

static struct icon *balloon_icon;
static HWND balloon_window;
static POINT balloon_pos;

#define MIN_DISPLAYED 8
#define ICON_BORDER  2

#define BALLOON_CREATE_TIMER 1
#define BALLOON_SHOW_TIMER   2

#define BALLOON_CREATE_TIMEOUT   2000
#define BALLOON_SHOW_MIN_TIMEOUT 10000
#define BALLOON_SHOW_MAX_TIMEOUT 30000

#define WM_POPUPSYSTEMMENU  0x0313

static LRESULT WINAPI shell_traywnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );
static LRESULT WINAPI tray_icon_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );

static void show_icon( struct icon *icon );
static void hide_icon( struct icon *icon );
static BOOL delete_icon( struct icon *icon );

static WNDCLASSEXW shell_traywnd_class =
{
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_DBLCLKS | CS_HREDRAW,
    .lpfnWndProc = shell_traywnd_proc,
    .hbrBackground = (HBRUSH)COLOR_WINDOW,
    .lpszClassName = L"Shell_TrayWnd",
};
static WNDCLASSEXW tray_icon_class =
{
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
    .lpfnWndProc = tray_icon_wndproc,
    .lpszClassName = L"__wine_tray_icon",
};

static void do_hide_systray(void);
static void do_show_systray(void);

/* Retrieves icon record by owner window and ID */
static struct icon *get_icon(HWND owner, UINT id)
{
    struct icon *this;

    /* search for the icon */
    LIST_FOR_EACH_ENTRY( this, &icon_list, struct icon, entry )
        if ((this->id == id) && (this->owner == owner)) return this;

    return NULL;
}

static void init_common_controls(void)
{
    static BOOL initialized = FALSE;

    if (!initialized)
    {
        INITCOMMONCONTROLSEX init_tooltip;

        init_tooltip.dwSize = sizeof(INITCOMMONCONTROLSEX);
        init_tooltip.dwICC = ICC_TAB_CLASSES|ICC_STANDARD_CLASSES;

        InitCommonControlsEx(&init_tooltip);
        initialized = TRUE;
    }
}

/* Creates tooltip window for icon. */
static void create_tooltip(struct icon *icon)
{
    TTTOOLINFOW ti;

    init_common_controls();
    icon->tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                     icon->window, NULL, NULL, NULL );

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;
    SendMessageW(icon->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void set_balloon_position( struct icon *icon )
{
    RECT rect;
    POINT pos;

    GetWindowRect( icon->window, &rect );
    pos.x = (rect.left + rect.right) / 2;
    pos.y = (rect.top + rect.bottom) / 2;
    SendMessageW( balloon_window, TTM_TRACKPOSITION, 0, MAKELONG( pos.x, pos.y ));
}

static void update_systray_balloon_position(void)
{
    RECT rect;
    POINT pos;

    if (!balloon_icon) return;
    GetWindowRect( balloon_icon->window, &rect );
    pos.x = (rect.left + rect.right) / 2;
    pos.y = (rect.top + rect.bottom) / 2;
    if (pos.x == balloon_pos.x && pos.y == balloon_pos.y) return; /* nothing changed */
    balloon_pos = pos;
    SendMessageW( balloon_window, TTM_TRACKPOSITION, 0, MAKELONG( pos.x, pos.y ));
}

static void balloon_create_timer( struct icon *icon )
{
    TTTOOLINFOW ti;

    init_common_controls();
    balloon_window = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_BALLOON | TTS_CLOSE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                      icon->window, NULL, NULL, NULL );

    memset( &ti, 0, sizeof(ti) );
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.uFlags = TTF_TRACK | TTF_IDISHWND;
    ti.lpszText = icon->info_text;
    SendMessageW( balloon_window, TTM_ADDTOOLW, 0, (LPARAM)&ti );
    if ((icon->info_flags & NIIF_ICONMASK) == NIIF_USER)
    {
        SendMessageW( balloon_window, TTM_SETTITLEW, (WPARAM)icon->info_icon, (LPARAM)icon->info_title );
    }
    else
    {
        UINT info_flags_wparam = icon->info_flags & NIIF_ERROR;

        if (icon->info_flags & NIIF_LARGEICON)
            info_flags_wparam += TTI_ERROR;
        SendMessageW( balloon_window, TTM_SETTITLEW, info_flags_wparam, (LPARAM)icon->info_title );
    }
    balloon_icon = icon;
    balloon_pos.x = balloon_pos.y = MAXLONG;
    update_systray_balloon_position();
    SendMessageW( balloon_window, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti );
    KillTimer( icon->window, BALLOON_CREATE_TIMER );
    SetTimer( icon->window, BALLOON_SHOW_TIMER, icon->info_timeout, NULL );
}

static BOOL show_balloon( struct icon *icon )
{
    if (!enable_taskbar && !show_systray) return FALSE;  /* systray has been hidden */
    if (icon->display == ICON_DISPLAY_HIDDEN) return FALSE;  /* not displayed */
    if (!icon->info_text[0]) return FALSE;  /* no balloon */
    balloon_icon = icon;
    SetTimer( icon->window, BALLOON_CREATE_TIMER, BALLOON_CREATE_TIMEOUT, NULL );
    return TRUE;
}

static void hide_balloon( struct icon *icon )
{
    if (!balloon_icon) return;
    if (balloon_window)
    {
        KillTimer( balloon_icon->window, BALLOON_SHOW_TIMER );
        DestroyWindow( balloon_window );
        balloon_window = 0;
    }
    else KillTimer( balloon_icon->window, BALLOON_CREATE_TIMER );
    balloon_icon = NULL;
}

static void show_next_balloon(void)
{
    struct icon *icon;

    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
        if (show_balloon( icon )) break;
}

static void update_balloon( struct icon *icon )
{
    if (balloon_icon == icon)
    {
        hide_balloon( icon );
        show_balloon( icon );
    }
    else if (!balloon_icon)
    {
        show_balloon( icon );
    }
}

static void balloon_timer( struct icon *icon )
{
    icon->info_text[0] = 0;  /* clear text now that balloon has been shown */
    hide_balloon( icon );
    show_next_balloon();
}

/* Synchronize tooltip text with tooltip window */
static void update_tooltip_text(struct icon *icon)
{
    TTTOOLINFOW ti;

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;

    SendMessageW(icon->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

static RECT get_tray_clock_rect(void)
{
    RECT rc;
    int clock_width = taskbar_settings_get()->show_clock ? 64 : 0;
    rc.right = tray_width - 2;
    rc.left = rc.right - clock_width;
    rc.top = tray_top + 2;
    rc.bottom = tray_top + tray_height - 2;
    return rc;
}

#define INACTIVE_AFTER (5 * 60 * 1000)
static BOOL tray_expanded;
static DWORD tray_expanded_hover_tick;

static void refresh_inactive_icons(void);

/* ---- Customize Notifications: per icon behavior ---- */

static const WCHAR behavior_key[] = L"Software\\WineExplorer\\TrayIcons";

static void icon_key( struct icon *icon, WCHAR *key )
{
    WCHAR path[MAX_PATH] = L"", *name;
    DWORD pid = 0, len = MAX_PATH;
    HANDLE process;

    GetWindowThreadProcessId( icon->owner, &pid );
    if ((process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid )))
    {
        QueryFullProcessImageNameW( process, 0, path, &len );
        CloseHandle( process );
    }
    name = wcsrchr( path, '\\' );
    swprintf( key, 300, L"%s|%u", name ? name + 1 : L"unknown", icon->id );
    CharLowerW( key );
}

static int icon_behavior( struct icon *icon )
{
    if (icon->behavior < 0)
    {
        WCHAR key[300];
        DWORD val = NOTIFY_HIDE_WHEN_INACTIVE, size = sizeof(val);

        icon_key( icon, key );
        if (RegGetValueW( HKEY_CURRENT_USER, behavior_key, key, RRF_RT_REG_DWORD, NULL, &val, &size )) val = NOTIFY_HIDE_WHEN_INACTIVE;
        icon->behavior = val <= NOTIFY_ALWAYS_SHOW ? val : NOTIFY_HIDE_WHEN_INACTIVE;
    }
    return icon->behavior;
}

int systray_enum_icons( TRAY_ICON_INFO *out, int max )
{
    struct icon *icon;
    int n = 0;

    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
    {
        if (n >= max) break;
        if (!icon->window || (icon->state & NIS_HIDDEN)) continue;
        icon_key( icon, out[n].key );
        lstrcpynW( out[n].text, icon->tiptext[0] ? icon->tiptext : out[n].key, ARRAY_SIZE(out[n].text) );
        out[n].icon = icon->image ? CopyIcon( icon->image ) : NULL;
        out[n].behavior = icon_behavior( icon );
        n++;
    }
    return n;
}

void systray_set_behavior( const WCHAR *key, int behavior )
{
    HKEY hkey;
    DWORD val = behavior;

    if (behavior < NOTIFY_HIDE_WHEN_INACTIVE || behavior > NOTIFY_ALWAYS_SHOW) return;

    if (!RegCreateKeyExW( HKEY_CURRENT_USER, behavior_key, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL ))
    {
        RegSetValueExW( hkey, key, 0, REG_DWORD, (BYTE *)&val, sizeof(val) );
        RegCloseKey( hkey );
    }
    systray_reset_behaviors();
}

void systray_reset_behaviors(void)
{
    struct icon *icon;

    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry ) icon->behavior = -1;
    if (tray_window) refresh_inactive_icons();
}

static BOOL setting_hide_inactive(void)
{
    return taskbar_settings_get()->hide_inactive_icons;
}

static BOOL chevron_visible(void)
{
    struct icon *icon;

    if (!setting_hide_inactive()) return FALSE;
    if (tray_expanded) return TRUE;
    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
        if (icon->auto_hidden && !(icon->state & NIS_HIDDEN)) return TRUE;
    return FALSE;
}

static SIZE get_tray_chevron_size(void)
{
    SIZE size = { GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ) };
    HTHEME theme = theme_open( tray_window, tray_expanded ? L"TrayNotifyHorizOpen::Button" : L"TrayNotifyHoriz::Button" );

    if (theme)
    {
        SIZE themed;
        if (SUCCEEDED( GetThemePartSize( theme, NULL, BP_PUSHBUTTON, PBS_DEFAULTED, NULL, TS_TRUE, &themed ) ) &&
            themed.cx > 0 && themed.cy > 0) size = themed;
        CloseThemeData( theme );
    }
    return size;
}

static RECT get_tray_notify_rect(void)
{
    RECT rc;
    RECT background = {0, 0, 200, 30}, content;
    HTHEME theme = theme_open( tray_window, L"TrayNotifyHoriz::TrayNotify" );
    int clock_width = taskbar_settings_get()->show_clock ? 64 : 0;
    int icons_width = nb_displayed > 0 ? (nb_displayed * icon_cx + 4) : 0;
    int left_padding = chevron_visible() ? get_tray_chevron_size().cx : 0;
    int total_width;

    /* The theme's left transition remains when the chevron is hidden.
     * Keep icons inside its content area rather than over that transition. */
    if (theme)
    {
        if (SUCCEEDED( GetThemeBackgroundContentRect( theme, NULL, TNP_BACKGROUND, 0,
                                                     &background, &content ) ))
            left_padding = max( left_padding, content.left - background.left );
        CloseThemeData( theme );
    }
    total_width = clock_width + icons_width + 4 + left_padding;

    rc.right = tray_width - 2;
    rc.left = rc.right - total_width;
    rc.top = tray_top + 2;
    rc.bottom = tray_top + tray_height - 2;
    return rc;
}

static RECT get_tray_chevron_rect(void)
{
    RECT notify = get_tray_notify_rect(), rect;
    SIZE size = get_tray_chevron_size();
    int height = min( size.cy, theme_taskbar_first_row() );
    int top = tray_top + (theme_taskbar_first_row() - height) / 2;

    SetRect( &rect, notify.left, top, notify.left + size.cx, top + height );
    return rect;
}

/* get the position of an icon in the tray */
static POINT get_icon_pos( struct icon *icon )
{
    POINT pos;

    if (enable_taskbar)
    {
        RECT rcClock = get_tray_clock_rect();
        int icons_width = nb_displayed * icon_cx;
        pos.x = rcClock.left - 4 - icons_width + (icon->display * icon_cx);
        pos.y = tray_top + (theme_taskbar_first_row() - icon_cy) / 2;   /* the icons stay in the first row */
    }
    else
    {
        pos.x = icon_cx * icon->display;
        pos.y = 0;
    }

    return pos;
}

#define IDT_AUTOHIDE 9998
#define IDT_THEMEREDRAW 9997
static BOOL taskbar_hidden;

static const WCHAR rows_key[] = L"Software\\WineExplorer\\Taskbar";
static int tray_rows;   /* rows of task buttons; an unlocked taskbar is made taller by dragging its top edge */

static int max_rows(void)
{
    return max( 1, (GetSystemMetrics( SM_CYSCREEN ) / 2 - theme_taskbar_first_row()) / theme_taskbar_row_pitch() + 1 );
}

/* an unlocked taskbar with a visual style has a sizing border of four pixels above the Start button */
static void update_tray_top(void)
{
    if (!tray_rows)
    {
        DWORD rows = 1, size = sizeof(rows);
        RegGetValueW( HKEY_CURRENT_USER, rows_key, L"Rows", RRF_RT_REG_DWORD, NULL, &rows, &size );
        tray_rows = rows;
    }
    tray_rows = max( 1, min( tray_rows, max_rows() ));
    tray_top = (theme_is_visual_styles_active() && !taskbar_settings_get()->lock_taskbar) ? 4 : 0;
    tray_height = theme_taskbar_first_row() + (tray_rows - 1) * theme_taskbar_row_pitch();
}

static BOOL sizing_rows;

/* the top edge of an unlocked taskbar changes its height */
static BOOL on_sizing_edge( int y )
{
    return !taskbar_settings_get()->lock_taskbar && y >= 0 && y < max( tray_top, 3 );
}

static void place_start_button(void)
{
    /* the Start button stays in the first row */
    if (theme_is_visual_styles_active()) startbutton_resize( 0, tray_top, start_button_width, theme_taskbar_first_row() );
    else startbutton_resize( 1, tray_top + 3, start_button_width, theme_taskbar_first_row() - 5 );
}

/* Always on top, auto-hide and the work area follow the taskbar settings */
static void position_taskbar(void)
{
    TASKBAR_SETTINGS *s = taskbar_settings_get();
    int screen_w = GetSystemMetrics( SM_CXSCREEN ), screen_h = GetSystemMetrics( SM_CYSCREEN );
    int total = tray_top + tray_height;
    int y = (s->autohide && taskbar_hidden) ? screen_h - 2 : screen_h - total;
    /* like Windows XP: the taskbar only takes its place out of the work area when it stays on top and does not hide */
    RECT work = { 0, 0, screen_w, (s->autohide || !s->always_on_top) ? screen_h : screen_h - total }, current;

    SetWindowPos( tray_window, (s->always_on_top || s->autohide) ? HWND_TOPMOST : HWND_NOTOPMOST, 0, y,
                  tray_width, total, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS );
    /* Off-screen auto-hide pixels cannot be reused when the tray is revealed. */
    RedrawWindow( tray_window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW );
    if (SystemParametersInfoW( SPI_GETWORKAREA, 0, &current, 0 ) && !EqualRect( &current, &work ))
        SystemParametersInfoW( SPI_SETWORKAREA, TRUE, &work, SPIF_SENDCHANGE );   /* TRUE: maximized windows follow */
}

static void autohide_tick(void)
{
    TASKBAR_SETTINGS *s = taskbar_settings_get();
    int screen_h = GetSystemMetrics( SM_CYSCREEN );
    POINT pt;
    BOOL over;

    if (!s->autohide)
    {
        if (taskbar_hidden) { taskbar_hidden = FALSE; position_taskbar(); }
        return;
    }
    GetCursorPos( &pt );
    over = pt.y >= (taskbar_hidden ? screen_h - 1 : screen_h - tray_top - tray_height);
    if (over && taskbar_hidden) { taskbar_hidden = FALSE; position_taskbar(); }
    else if (!over && !taskbar_hidden && !startmenu_view_is_open() &&
             GetAncestor( GetForegroundWindow(), GA_ROOT ) != tray_window && !GetCapture())
    {
        taskbar_hidden = TRUE;
        position_taskbar();
    }
}

static void update_tray_layout(void);

static void refresh_inactive_icons(void)
{
    struct icon *icon;
    BOOL changed = FALSE, want_hide;

    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
    {
        if (!icon->window || (icon->state & NIS_HIDDEN)) continue;
        {
            int behavior = icon_behavior( icon );
            want_hide = setting_hide_inactive() && !tray_expanded && behavior != NOTIFY_ALWAYS_SHOW &&
                        (behavior == NOTIFY_ALWAYS_HIDE || GetTickCount() - icon->last_active > INACTIVE_AFTER);
        }
        if (want_hide && !icon->auto_hidden && icon->display != ICON_DISPLAY_HIDDEN)
        {
            icon->auto_hidden = TRUE;
            hide_icon( icon );
            changed = TRUE;
        }
        else if (!want_hide && icon->auto_hidden)
        {
            icon->auto_hidden = FALSE;
            show_icon( icon );
            changed = TRUE;
        }
    }
    if (changed && enable_taskbar) update_tray_layout();
}

static void update_tray_layout(void)
{
    if (!enable_taskbar || !tray_window) return;
    {
        RECT rcTray = get_tray_notify_rect();
        RECT rcClock = get_tray_clock_rect();
        int taskband_x, taskband_w;
        struct icon *icon;

        toolbars_layout( tray_window, start_button_width + (theme_is_visual_styles_active() ? 4 : 7), rcTray.left, tray_top, tray_height, &taskband_x, &taskband_w );
        tray_clock_resize( rcClock.left + 2, rcClock.top + 2, rcClock.right - rcClock.left - 4, rcClock.bottom - rcClock.top - 4 );
        taskband_resize( taskband_x, tray_top, taskband_w, tray_height );

        /* Relayout tray icons */
        LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
        {
            if (icon->display >= 0 && icon->window)
            {
                POINT pos = get_icon_pos( icon );
                SetWindowPos( icon->window, 0, pos.x, pos.y, 0, 0,
                              SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER );
                InvalidateRect( icon->window, NULL, TRUE );   /* the background under it may have changed */
            }
        }

        InvalidateRect( tray_window, NULL, FALSE );
    }
}

/* get the size of the stand-alone tray window */
static SIZE get_window_size(void)
{
    SIZE size;
    RECT rect;

    rect.left = 0;
    rect.top = 0;
    rect.right = icon_cx * max( nb_displayed, MIN_DISPLAYED );
    rect.bottom = icon_cy;
    AdjustWindowRect( &rect, WS_CAPTION, FALSE );
    size.cx = rect.right - rect.left;
    size.cy = rect.bottom - rect.top;
    return size;
}

/* synchronize tooltip position with tooltip window */
static void update_tooltip_position( struct icon *icon )
{
    TTTOOLINFOW ti;

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;
    SendMessageW( icon->tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti );
    if (balloon_icon == icon) set_balloon_position( icon );
}

static void paint_layered_icon( struct icon *icon )
{
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    int width = GetSystemMetrics( SM_CXSMICON );
    int height = GetSystemMetrics( SM_CYSMICON );
    BITMAPINFO *info;
    HBITMAP dib, mask;
    HDC hdc;
    RECT rc;
    SIZE size;
    POINT pos;
    int i, x, y;
    void *color_bits, *mask_bits;
    DWORD *ptr;
    BOOL has_alpha = FALSE;

    GetWindowRect( icon->window, &rc );
    size.cx = rc.right - rc.left;
    size.cy = rc.bottom - rc.top;
    pos.x = (size.cx - width) / 2;
    pos.y = (size.cy - height) / 2;

    if (!(info = calloc( 1, FIELD_OFFSET( BITMAPINFO, bmiColors[2] ) ))) return;
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = size.cx;
    info->bmiHeader.biHeight = size.cy;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC( 0 );
    if (!(dib = CreateDIBSection( 0, info, DIB_RGB_COLORS, &color_bits, NULL, 0 ))) goto done;
    SelectObject( hdc, dib );
    DrawIconEx( hdc, pos.x, pos.y, icon->image, width, height, 0, 0, DI_DEFAULTSIZE | DI_NORMAL );

    /* check if the icon was drawn with an alpha channel */
    for (i = 0, ptr = color_bits; i < size.cx * size.cy; i++)
        if ((has_alpha = (ptr[i] & 0xff000000) != 0)) break;

    if (!has_alpha)
    {
        unsigned int width_bytes = (size.cx + 31) / 32 * 4;

        info->bmiHeader.biBitCount = 1;
        info->bmiColors[0].rgbRed = 0;
        info->bmiColors[0].rgbGreen = 0;
        info->bmiColors[0].rgbBlue = 0;
        info->bmiColors[0].rgbReserved = 0;
        info->bmiColors[1].rgbRed = 0xff;
        info->bmiColors[1].rgbGreen = 0xff;
        info->bmiColors[1].rgbBlue = 0xff;
        info->bmiColors[1].rgbReserved = 0;

        if (!(mask = CreateDIBSection( 0, info, DIB_RGB_COLORS, &mask_bits, NULL, 0 ))) goto done;
        memset( mask_bits, 0xff, width_bytes * size.cy );
        SelectObject( hdc, mask );
        DrawIconEx( hdc, pos.x, pos.y, icon->image, width, height, 0, 0, DI_DEFAULTSIZE | DI_MASK );

        for (y = 0, ptr = color_bits; y < size.cy; y++)
            for (x = 0; x < size.cx; x++, ptr++)
                if (!((((BYTE *)mask_bits)[y * width_bytes + x / 8] << (x % 8)) & 0x80))
                    *ptr |= 0xff000000;

        SelectObject( hdc, dib );
        DeleteObject( mask );
    }

    UpdateLayeredWindow( icon->window, 0, NULL, NULL, hdc, NULL, 0, &blend, ULW_ALPHA );
done:
    free( info );
    if (hdc) DeleteDC( hdc );
    if (dib) DeleteObject( dib );
}

static BOOL notify_owner( struct icon *icon, UINT msg, LPARAM lparam )
{
    WPARAM wp = icon->id;
    LPARAM lp = msg;

    if (icon->version >= NOTIFYICON_VERSION_4)
    {
        POINT pt = {.x = (short)LOWORD(lparam), .y = (short)HIWORD(lparam)};
        ClientToScreen( icon->window, &pt );
        wp = MAKEWPARAM( pt.x, pt.y );
        lp = MAKELPARAM( msg, icon->id );
    }

    TRACE( "relaying 0x%x\n", msg );
    if (!SendNotifyMessageW( icon->owner, icon->callback_message, wp, lp ) &&
        (GetLastError() == ERROR_INVALID_WINDOW_HANDLE))
    {
        WARN( "application window was destroyed, removing icon %u\n", icon->id );
        delete_icon( icon );
        return FALSE;
    }
    return TRUE;
}

/* window procedure for the individual tray icon window */
/* the background of the taskbar and the notification area, also used behind the icons of the tray */
static void paint_tray_background( HWND hwnd, HDC hdc, RECT *rc )
{
    HTHEME theme = theme_open( hwnd, L"TaskBar" );
    RECT rcTop;

    if (theme)
    {
        HTHEME tray_theme = theme_open( hwnd, L"TrayNotifyHoriz::TrayNotify" );
        RECT body = *rc;

        /* XP keeps the background and controls fixed when unlocking: the
         * additional four pixels are the theme's separate sizing strip. */
        body.top += tray_top;
        DrawThemeBackground( theme, hdc, TBP_BACKGROUNDBOTTOM, 0, &body, NULL );
        if (tray_theme)
        {
            RECT rcTray = get_tray_notify_rect();
            rcTray.top = body.top;
            rcTray.bottom = rc->bottom;
            DrawThemeBackground( tray_theme, hdc, TNP_BACKGROUND, 0, &rcTray, NULL );
            CloseThemeData( tray_theme );
        }
        if (tray_top)
        {
            RECT sizing = *rc;
            sizing.bottom = sizing.top + tray_top;
            DrawThemeBackground( theme, hdc, TBP_SIZINGBARBOTTOM, 0, &sizing, NULL );
        }
        CloseThemeData( theme );
    }
    else
    {
        FillRect( hdc, rc, GetSysColorBrush( COLOR_BTNFACE ) );

        /* Continuous 1px highlight line across the top of the taskbar */
        rcTop = *rc;
        rcTop.top = 1;
        rcTop.bottom = 2;
        FillRect( hdc, &rcTop, GetSysColorBrush( COLOR_3DHIGHLIGHT ) );
    }
}

static LRESULT WINAPI tray_icon_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct icon *icon = (struct icon *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    TRACE( "hwnd %p, msg %#x, wparam %#Ix, lparam %#Ix\n", hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_NCCREATE:
    {
        /* set the icon data for the window from the data passed into CreateWindow */
        const CREATESTRUCTW *info = (const CREATESTRUCTW *)lparam;
        icon = info->lpCreateParams;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)icon );
        break;
    }

    case WM_CLOSE:
        if (icon->display == ICON_DISPLAY_DOCKED)
        {
            TRACE( "icon %u no longer embedded\n", icon->id );
            hide_icon( icon );
            show_icon( icon );
        }
        return 0;

    case WM_CREATE:
        icon->window = hwnd;
        create_tooltip( icon );
        break;

    case WM_SIZE:
    case WM_MOVE:
        if (icon->layered) paint_layered_icon( icon );
        break;

    case WM_ERASEBKGND:
        /* an icon on the taskbar shows the taskbar under it, not what was drawn there before */
        if (icon && !icon->layered && GetParent( hwnd ) == tray_window)
        {
            DrawThemeParentBackground( hwnd, (HDC)wparam, NULL );
            return 1;
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc;
        int cx, cy;

        if (icon->layered) break;

        cx = GetSystemMetrics( SM_CXSMICON );
        cy = GetSystemMetrics( SM_CYSMICON );
        hdc = BeginPaint( hwnd, &ps );
        GetClientRect( hwnd, &rc );
        TRACE( "painting rect %s\n", wine_dbgstr_rect( &rc ) );
        DrawIconEx( hdc, (rc.left + rc.right - cx) / 2, (rc.top + rc.bottom - cy) / 2,
                    icon->image, cx, cy, 0, 0, DI_DEFAULTSIZE | DI_NORMAL );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    {
        MSG message = {.hwnd = hwnd, .message = msg, .wParam = wparam, .lParam = lparam};
        if (msg != WM_MOUSEMOVE) icon->last_active = GetTickCount();
        SendMessageW( icon->tooltip, TTM_RELAYEVENT, 0, (LPARAM)&message );
        if (!notify_owner( icon, msg, lparam )) break;
        if (icon->version > 0)
        {
            if (msg == WM_LBUTTONUP) notify_owner( icon, NIN_SELECT, lparam );
            if (msg == WM_RBUTTONUP) notify_owner( icon, WM_CONTEXTMENU, lparam );
        }
        break;
    }

    case WM_WINDOWPOSCHANGING:
        if (icon->display == ICON_DISPLAY_HIDDEN)
        {
            /* Changing the icon's parent via SetParent would activate it, stealing the focus. */
            WINDOWPOS *wp = (WINDOWPOS*)lparam;
            wp->flags |= SWP_NOACTIVATE;
        }
        break;

    case WM_WINDOWPOSCHANGED:
        update_systray_balloon_position();
        break;

    case WM_TIMER:
        switch (wparam)
        {
        case BALLOON_CREATE_TIMER: balloon_create_timer( icon ); break;
        case BALLOON_SHOW_TIMER: balloon_timer( icon ); break;
        }
        return 0;
    }

    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* the icons of the notification area keep the order they were added in, whatever was hidden meanwhile */
static void renumber_icons(void)
{
    struct icon *ptr;
    int n = 0;

    LIST_FOR_EACH_ENTRY( ptr, &icon_list, struct icon, entry )
    {
        POINT pos;
        if (ptr->display < 0) continue;
        ptr->display = n++;
        if (!ptr->window) continue;
        update_tooltip_position( ptr );
        pos = get_icon_pos( ptr );
        SetWindowPos( ptr->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER );
    }
}

/* add an icon to the system tray window */
static void systray_add_icon( struct icon *icon )
{
    POINT pos;

    if (icon->display != ICON_DISPLAY_HIDDEN) return;  /* already added */

    SetWindowLongW( icon->window, GWL_STYLE, (GetWindowLongW( icon->window, GWL_STYLE ) & ~WS_POPUP) | WS_CHILD );
    SetParent( icon->window, tray_window );
    icon->display = nb_displayed++;
    renumber_icons();   /* icons that were hidden come back where they were */
    pos = get_icon_pos( icon );
    SetWindowPos( icon->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW );

    if (enable_taskbar) update_tray_layout();
    else if (nb_displayed == 1 && show_systray) do_show_systray();
    TRACE( "added %u now %d icons\n", icon->id, nb_displayed );
}

/* remove an icon from the stand-alone tray */
static void systray_remove_icon( struct icon *icon )
{
    struct icon *ptr;
    POINT pos;

    if (icon->display == ICON_DISPLAY_HIDDEN) return;  /* already removed */

    assert( nb_displayed );
    LIST_FOR_EACH_ENTRY( ptr, &icon_list, struct icon, entry )
    {
        if (ptr == icon) continue;
        if (ptr->display < icon->display) continue;
        ptr->display--;
        update_tooltip_position( ptr );
        pos = get_icon_pos( ptr );
        SetWindowPos( ptr->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER );
    }

    if (!--nb_displayed && !enable_taskbar) do_hide_systray();
    if (enable_taskbar) update_tray_layout();
    TRACE( "removed %u now %d icons\n", icon->id, nb_displayed );

    icon->display = ICON_DISPLAY_HIDDEN;
    SetParent( icon->window, GetDesktopWindow() );
    SetWindowLongW( icon->window, GWL_STYLE, (GetWindowLongW( icon->window, GWL_STYLE ) & ~WS_CHILD) | WS_POPUP );
}

/* make an icon visible */
static void show_icon(struct icon *icon)
{
    TRACE( "id=0x%x, hwnd=%p\n", icon->id, icon->owner );

    if (no_tray_items) return;

    if (icon->display != ICON_DISPLAY_HIDDEN) return;  /* already displayed */

    if (enable_dock)
    {
        DWORD old_exstyle = GetWindowLongW( icon->window, GWL_EXSTYLE );

        /* make sure it is layered before calling into the driver */
        SetWindowLongW( icon->window, GWL_EXSTYLE, old_exstyle | WS_EX_LAYERED );
        paint_layered_icon( icon );

        if (!NtUserMessageCall( icon->window, WINE_SYSTRAY_DOCK_INSERT, icon_cx, icon_cy,
                                icon, NtUserSystemTrayCall, FALSE ))
            SetWindowLongW( icon->window, GWL_EXSTYLE, old_exstyle );
        else
        {
            icon->display = ICON_DISPLAY_DOCKED;
            icon->layered = TRUE;
            SendMessageW( icon->window, WM_SIZE, SIZE_RESTORED, MAKELONG( icon_cx, icon_cy ) );
        }
    }
    systray_add_icon( icon );

    update_tooltip_position( icon );
    update_balloon( icon );
}

/* make an icon invisible */
static void hide_icon(struct icon *icon)
{
    TRACE( "id=0x%x, hwnd=%p\n", icon->id, icon->owner );

    if (icon->display == ICON_DISPLAY_HIDDEN) return;  /* already hidden */

    if (enable_dock && NtUserMessageCall( icon->window, WINE_SYSTRAY_DOCK_REMOVE, 0, 0,
                                          NULL, NtUserSystemTrayCall, FALSE ))
    {
        icon->display = ICON_DISPLAY_HIDDEN;
        icon->layered = FALSE;
        SetWindowLongW( icon->window, GWL_EXSTYLE, GetWindowLongW( icon->window, GWL_EXSTYLE ) & ~WS_EX_LAYERED );
    }
    ShowWindow( icon->window, SW_HIDE );
    systray_remove_icon( icon );

    update_balloon( icon );
    update_tooltip_position( icon );
}

/* Modifies an existing icon record */
/* TRUE if two icon handles show the same picture */
static BOOL same_icon( HICON a, HICON b )
{
    ICONINFO ia, ib;
    BITMAP ba, bb;
    BOOL same = FALSE;

    if (!GetIconInfo( a, &ia )) return FALSE;
    if (!GetIconInfo( b, &ib )) { DeleteObject( ia.hbmColor ); DeleteObject( ia.hbmMask ); return FALSE; }
    if (GetObjectW( ia.hbmMask, sizeof(ba), &ba ) && GetObjectW( ib.hbmMask, sizeof(bb), &bb ) &&
        ba.bmWidth == bb.bmWidth && ba.bmHeight == bb.bmHeight)
    {
        int size = ba.bmWidthBytes * ba.bmHeight, cmp = 0;
        BYTE *x = malloc( 2 * size ), *y;

        if (x)
        {
            y = x + size;
            GetBitmapBits( ia.hbmMask, size, x );
            GetBitmapBits( ib.hbmMask, size, y );
            cmp = memcmp( x, y, size );
            free( x );
        }
        if (!cmp && ia.hbmColor && ib.hbmColor && GetObjectW( ia.hbmColor, sizeof(ba), &ba ) &&
            GetObjectW( ib.hbmColor, sizeof(bb), &bb ) && ba.bmWidthBytes == bb.bmWidthBytes && ba.bmHeight == bb.bmHeight)
        {
            size = ba.bmWidthBytes * ba.bmHeight;
            x = malloc( 2 * size );
            if (x)
            {
                y = x + size;
                GetBitmapBits( ia.hbmColor, size, x );
                GetBitmapBits( ib.hbmColor, size, y );
                same = !memcmp( x, y, size );
                free( x );
            }
        }
    }
    DeleteObject( ia.hbmColor ); DeleteObject( ia.hbmMask );
    DeleteObject( ib.hbmColor ); DeleteObject( ib.hbmMask );
    return same;
}

static BOOL modify_icon( struct icon *icon, NOTIFYICONDATAW *nid )
{
    BOOL activity = FALSE;
    BOOL had_chevron = chevron_visible();

    TRACE( "id=0x%x, hwnd=%p\n", nid->uID, nid->hWnd );

    /* demarshal the request from the NID */
    if (!icon)
    {
        WARN( "Invalid icon ID (0x%x) for HWND %p\n", nid->uID, nid->hWnd );
        return FALSE;
    }

    if (nid->uFlags & NIF_STATE)
    {
        icon->state = (icon->state & ~nid->dwStateMask) | (nid->dwState & nid->dwStateMask);
    }

    if (nid->uFlags & NIF_ICON)
    {
        /* a new picture counts as activity, the same picture again (a tooltip refresh) does not */
        if (!icon->image || !nid->hIcon || !same_icon( icon->image, nid->hIcon )) activity = TRUE;
        if (icon->image) DestroyIcon(icon->image);
        icon->image = CopyIcon(nid->hIcon);

        if (icon->display >= 0)
            InvalidateRect( icon->window, NULL, TRUE );
        else if (icon->layered)
            paint_layered_icon( icon );
        else if (enable_dock)
            NtUserMessageCall( icon->window, WINE_SYSTRAY_DOCK_CLEAR, 0, 0,
                               NULL, NtUserSystemTrayCall, FALSE );
    }

    if (nid->uFlags & NIF_MESSAGE)
    {
        icon->callback_message = nid->uCallbackMessage;
    }
    if (nid->uFlags & NIF_TIP)
    {
        lstrcpynW( icon->tiptext, nid->szTip, ARRAY_SIZE( icon->tiptext ));
        update_tooltip_text( icon );
    }
    if (nid->uFlags & NIF_INFO && nid->cbSize >= NOTIFYICONDATAA_V2_SIZE)
    {
        lstrcpynW( icon->info_text, nid->szInfo, ARRAY_SIZE( icon->info_text ));
        lstrcpynW( icon->info_title, nid->szInfoTitle, ARRAY_SIZE( icon->info_title ));
        icon->info_flags = nid->dwInfoFlags;
        icon->info_timeout = max(min(nid->uTimeout, BALLOON_SHOW_MAX_TIMEOUT), BALLOON_SHOW_MIN_TIMEOUT);

        if (icon->info_icon) DestroyIcon( icon->info_icon );
        icon->info_icon = CopyIcon( nid->hBalloonIcon );

        update_balloon( icon );
    }
    if ((nid->uFlags & NIF_INFO) && nid->szInfo[0]) activity = TRUE;
    if (activity) icon->last_active = GetTickCount();
    icon->auto_hidden = setting_hide_inactive() && !tray_expanded &&
                        icon_behavior( icon ) != NOTIFY_ALWAYS_SHOW &&
                        (icon_behavior( icon ) == NOTIFY_ALWAYS_HIDE ||
                         GetTickCount() - icon->last_active > INACTIVE_AFTER);
    if ((icon->state & NIS_HIDDEN) || icon->auto_hidden) hide_icon( icon );
    else show_icon( icon );
    /* An initially hidden icon changes the chevron without changing the
     * visible icon count. Reserve its space before the task band paints it. */
    if (enable_taskbar && had_chevron != chevron_visible()) update_tray_layout();
    return TRUE;
}

/* Adds a new icon record to the list */
static BOOL add_icon(NOTIFYICONDATAW *nid)
{
    struct icon  *icon;

    TRACE( "id=0x%x, hwnd=%p\n", nid->uID, nid->hWnd );

    if ((icon = get_icon(nid->hWnd, nid->uID)))
    {
        WARN( "duplicate tray icon add, buggy app?\n" );
        return FALSE;
    }

    if (!(icon = calloc( 1, sizeof(*icon) )))
    {
        ERR( "out of memory\n" );
        return FALSE;
    }

    ZeroMemory(icon, sizeof(struct icon));
    icon->id     = nid->uID;
    icon->owner  = nid->hWnd;
    icon->display = ICON_DISPLAY_HIDDEN;
    icon->behavior = -1;
    icon->last_active = GetTickCount();

    CreateWindowExW( 0, tray_icon_class.lpszClassName, NULL, WS_CLIPSIBLINGS | WS_POPUP,
                     0, 0, icon_cx, icon_cy, 0, NULL, NULL, icon );
    if (!icon->window) ERR( "Failed to create systray icon window\n" );

    list_add_tail(&icon_list, &icon->entry);

    return modify_icon( icon, nid );
}

/* Deletes tray icon window and icon record */
static BOOL delete_icon( struct icon *icon )
{
    BOOL had_chevron = chevron_visible();
    hide_icon( icon );
    list_remove( &icon->entry );
    if (enable_taskbar && had_chevron != chevron_visible()) update_tray_layout();
    DestroyWindow( icon->tooltip );
    DestroyWindow( icon->window );
    DestroyIcon( icon->image );
    free( icon );
    return TRUE;
}

/* cleanup icons belonging to a window that has been destroyed */
static void cleanup_systray_window( HWND hwnd )
{
    NOTIFYICONDATAW nid = {.cbSize = sizeof(nid), .hWnd = hwnd};
    struct icon *icon, *next;

    LIST_FOR_EACH_ENTRY_SAFE( icon, next, &icon_list, struct icon, entry )
        if (icon->owner == hwnd) delete_icon( icon );

    NtUserMessageCall( hwnd, WINE_SYSTRAY_CLEANUP_ICONS, 0, 0, NULL, NtUserSystemTrayCall, FALSE );
}

/* update the taskbar buttons when something changed */
static void sync_taskbar_buttons(void)
{
    if (!enable_taskbar) return;
    taskband_sync_windows();
}

static BOOL handle_incoming(HWND hwndSource, COPYDATASTRUCT *cds)
{
    struct icon *icon = NULL;
    const struct notify_data *data;
    const BYTE *icon_data;
    NOTIFYICONDATAW nid;
    int ret = FALSE;

    if (cds->cbData < sizeof(*data)) return FALSE;
    data = cds->lpData;
    icon_data = data->icon_data;

    nid.cbSize           = sizeof(nid);
    nid.hWnd             = LongToHandle( data->hWnd );
    nid.uID              = data->uID;
    nid.uFlags           = data->uFlags;
    nid.uCallbackMessage = data->uCallbackMessage;
    nid.hIcon            = 0;
    nid.dwState          = data->dwState;
    nid.dwStateMask      = data->dwStateMask;
    nid.uTimeout         = data->u.uTimeout;
    nid.dwInfoFlags      = data->dwInfoFlags;
    nid.guidItem         = data->guidItem;
    lstrcpyW( nid.szTip, data->szTip );
    lstrcpyW( nid.szInfo, data->szInfo );
    lstrcpyW( nid.szInfoTitle, data->szInfoTitle );
    nid.hBalloonIcon     = 0;

    /* FIXME: if statement only needed because we don't support interprocess
     * icon handles */
    if (nid.uFlags & NIF_ICON)
    {
        LONG cbMaskBits;
        LONG cbColourBits;

        cbMaskBits = (data->icon_info.width * data->icon_info.height + 15) / 16 * 2;
        cbColourBits = (data->icon_info.planes * data->icon_info.width * data->icon_info.height * data->icon_info.bpp + 15) / 16 * 2;

        if (cds->cbData < sizeof(*data) + cbMaskBits + cbColourBits)
        {
            ERR( "buffer underflow\n" );
            return FALSE;
        }
        nid.hIcon = CreateIcon(NULL, data->icon_info.width, data->icon_info.height, data->icon_info.planes, data->icon_info.bpp,
                               icon_data, icon_data + cbMaskBits);
        icon_data += cbMaskBits + cbColourBits;
    }

    if ((nid.uFlags & NIF_INFO) && (nid.dwInfoFlags & NIIF_ICONMASK) == NIIF_USER)
    {
        /* Balloon icon */
        LONG cbMaskBits;
        LONG cbColourBits;

        cbMaskBits = (data->balloon_icon_info.width * data->balloon_icon_info.height + 15) / 16 * 2;
        cbColourBits = (data->balloon_icon_info.planes * data->balloon_icon_info.width * data->balloon_icon_info.height * data->balloon_icon_info.bpp + 15) / 16 * 2;

        if (cds->cbData < ((char*)icon_data - (char*)data) + cbMaskBits + cbColourBits)
        {
            ERR( "buffer underflow\n" );
            return FALSE;
        }
        nid.hBalloonIcon = CreateIcon(NULL, data->balloon_icon_info.width, data->balloon_icon_info.height, data->balloon_icon_info.planes, data->balloon_icon_info.bpp,
                                      icon_data, icon_data + cbMaskBits);
    }
    /* try forwarding to the display driver first */
    if (cds->dwData == NIM_ADD || !(icon = get_icon( nid.hWnd, nid.uID )))
    {
        if ((ret = NtUserMessageCall( hwndSource, WINE_SYSTRAY_NOTIFY_ICON, cds->dwData, 0,
                                      &nid, NtUserSystemTrayCall, FALSE )) != -1)
            goto done;
        ret = FALSE;
    }

    switch (cds->dwData)
    {
    case NIM_ADD:
        ret = add_icon(&nid);
        break;
    case NIM_DELETE:
        if (icon) ret = delete_icon( icon );
        break;
    case NIM_MODIFY:
        if (icon) ret = modify_icon( icon, &nid );
        break;
    case NIM_SETVERSION:
        if (icon)
        {
            icon->version = nid.uVersion;
            ret = TRUE;
        }
        break;
    default:
        FIXME( "unhandled tray message: %Id\n", cds->dwData );
        break;
    }

done:
    if (nid.hIcon) DestroyIcon( nid.hIcon );
    if (nid.hBalloonIcon) DestroyIcon( nid.hBalloonIcon );
    sync_taskbar_buttons();
    return ret;
}

static void add_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win;

    if (!enable_taskbar) return;

    /* ignore our own windows */
    if (hwnd)
    {
        DWORD process;
        if (!GetWindowThreadProcessId( hwnd, &process ) || process == GetCurrentProcessId()) return;
    }

    if (!(win = malloc( sizeof(*win) ))) return;
    win->hwnd = hwnd;
    win->button = CreateWindowW( WC_BUTTONW, NULL, WS_CHILD | BS_OWNERDRAW,
                                 0, 0, 0, 0, tray_window, (HMENU)hwnd, 0, 0 );
    list_add_tail( &taskbar_buttons, &win->entry );
}

static struct taskbar_button *find_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win;

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
        if (win->hwnd == hwnd) return win;

    return NULL;
}

static void remove_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win = find_taskbar_button( hwnd );

    if (!win) return;
    list_remove( &win->entry );
    DestroyWindow( win->button );
    free( win );
}

static void paint_taskbar_button( const DRAWITEMSTRUCT *dis )
{
    RECT rect;
    UINT flags = DC_TEXT;
    struct taskbar_button *win = NULL, *it;

    LIST_FOR_EACH_ENTRY( it, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (it->button == dis->hwndItem) { win = it; break; }
    }
    if (!win) return;

    GetClientRect( dis->hwndItem, &rect );
    DrawFrameControl( dis->hDC, &rect, DFC_BUTTON, DFCS_BUTTONPUSH | DFCS_ADJUSTRECT |
                      (((dis->itemState & ODS_SELECTED) || win->active) ? DFCS_PUSHED : 0 ));
    if (win->hwnd)
    {
        flags |= win->active ? DC_ACTIVE : DC_INBUTTON;
        DrawCaptionTempW( win->hwnd, dis->hDC, &rect, 0, 0, NULL, flags );
    }
    else  /* start button */
        DrawCaptionTempW( 0, dis->hDC, &rect, 0, 0, start_label, flags | DC_INBUTTON | DC_ICON );
}

static void click_taskbar_button( HWND button )
{
    struct taskbar_button *win = NULL, *it;
    HWND hwnd;

    LIST_FOR_EACH_ENTRY( it, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (it->button == button || (button && it->hwnd == (HWND)GetWindowLongPtrW(button, GWLP_ID)))
        {
            win = it;
            break;
        }
    }
    if (!win)
    {
        POINT pt;
        GetCursorPos( &pt );
        LIST_FOR_EACH_ENTRY( it, &taskbar_buttons, struct taskbar_button, entry )
        {
            RECT rc;
            if (it->button && it->visible)
            {
                GetWindowRect( it->button, &rc );
                if (PtInRect( &rc, pt )) { win = it; break; }
            }
        }
    }
    if (!win) return;
    hwnd = win->hwnd;

    if (!hwnd)  /* start button */
    {
        do_startmenu( tray_window );
        return;
    }

    if (IsIconic( hwnd ))
    {
        ShowWindow( hwnd, SW_RESTORE );
        SetForegroundWindow( hwnd );
        win->active = TRUE;
        sync_taskbar_buttons();
        return;
    }

    if (IsWindowEnabled( hwnd ))
    {
        HWND fg = GetAncestor( GetForegroundWindow(), GA_ROOTOWNER );
        if (hwnd == fg || win->active)
        {
            ShowWindow( hwnd, SW_MINIMIZE );
            win->active = FALSE;
            sync_taskbar_buttons();
            return;
        }
    }
    else  /* look for an enabled window owned by this one */
    {
        HWND owned = GetWindow( GetDesktopWindow(), GW_CHILD );
        while (owned && owned != hwnd)
        {
            if (IsWindowVisible( owned ) &&
                IsWindowEnabled( owned ) &&
                (GetWindow( owned, GW_OWNER ) == hwnd))
                break;
            owned = GetWindow( owned, GW_HWNDNEXT );
        }
        if (owned) hwnd = owned;
    }
    ShowWindow( hwnd, SW_SHOWNORMAL );
    SetForegroundWindow( hwnd );
    win->active = TRUE;
    sync_taskbar_buttons();
}

#define WM_TRAY_CLOCKMENU (WM_APP + 31)

/* the positions of the windows before Cascade, Tile or Minimize All, for the Undo item */
static struct
{
    HWND hwnd[64];
    WINDOWPLACEMENT place[64];
    int count;
    const WCHAR *label;
} undo_state;

static void undo_save( const WCHAR *label )
{
    struct taskbar_button *it;

    undo_state.count = 0;
    undo_state.label = label;
    LIST_FOR_EACH_ENTRY( it, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (!it->hwnd || !IsWindow( it->hwnd ) || undo_state.count >= 64) continue;
        undo_state.hwnd[undo_state.count] = it->hwnd;
        undo_state.place[undo_state.count].length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement( it->hwnd, &undo_state.place[undo_state.count] );
        undo_state.count++;
    }
}

static void undo_apply(void)
{
    int i;

    for (i = 0; i < undo_state.count; i++)
        if (IsWindow( undo_state.hwnd[i] )) SetWindowPlacement( undo_state.hwnd[i], &undo_state.place[i] );
    undo_state.count = 0;
}

static int count_tileable_windows(void)
{
    struct taskbar_button *it;
    int n = 0;

    LIST_FOR_EACH_ENTRY( it, &taskbar_buttons, struct taskbar_button, entry )
        if (it->hwnd && IsWindowVisible( it->hwnd ) && !IsIconic( it->hwnd )) n++;
    return n;
}

/* The taskbar's own menu (MENU_TRAYCONTEXT of the Windows shell); the notification area adds the clock items */
static void track_tray_menu( POINT pt, BOOL include_time )
{
    HMENU hMenu = CreatePopupMenu();
    HMENU hToolbars = CreatePopupMenu();
    TASKBAR_SETTINGS *settings = taskbar_settings_get();
    UINT gray = count_tileable_windows() > 0 ? 0 : MF_GRAYED;
    UINT cmd;

    if (include_time)
    {
        AppendMenuW( hMenu, MF_STRING, IDM_TASKBAR_SETTIME, L"&Adjust Date/Time" );
        SetMenuDefaultItem( hMenu, IDM_TASKBAR_SETTIME, FALSE );
        AppendMenuW( hMenu, MF_STRING, IDM_TASKBAR_NOTIFYCUST, L"&Customize Notifications..." );
        AppendMenuW( hMenu, MF_SEPARATOR, 0, NULL );
    }
    else
    {
        AppendMenuW( hToolbars, MF_STRING | (toolbars_is_visible( TOOLBAR_ADDRESS ) ? MF_CHECKED : 0), 3001, L"&Address" );
        AppendMenuW( hToolbars, MF_STRING | (toolbars_is_visible( TOOLBAR_LINKS ) ? MF_CHECKED : 0), 3002, L"&Links" );
        AppendMenuW( hToolbars, MF_STRING | (toolbars_is_visible( TOOLBAR_DESKTOP ) ? MF_CHECKED : 0), 3003, L"&Desktop" );
        AppendMenuW( hToolbars, MF_STRING | (toolbars_is_visible( TOOLBAR_QUICKLAUNCH ) ? MF_CHECKED : 0), 3004, L"&Quick Launch" );
        AppendMenuW( hToolbars, MF_STRING | (toolbars_is_visible( TOOLBAR_SEARCH ) ? MF_CHECKED : 0), 3006, L"&Search" );
        {
            int c;
            WCHAR cname[MAX_PATH];
            for (c = 0; c < TOOLBAR_CUSTOM_MAX; c++)
                if (toolbars_custom_name( TOOLBAR_CUSTOM + c, cname, MAX_PATH ))
                    AppendMenuW( hToolbars, MF_STRING | MF_CHECKED, 3010 + c, cname );
        }
        AppendMenuW( hToolbars, MF_SEPARATOR, 0, NULL );
        AppendMenuW( hToolbars, MF_STRING, 3005, L"&New Toolbar..." );
        AppendMenuW( hMenu, MF_POPUP, (UINT_PTR)hToolbars, L"&Toolbars" );
        AppendMenuW( hMenu, MF_SEPARATOR, 0, NULL );
    }
    AppendMenuW( hMenu, MF_STRING | gray, IDM_TASKBAR_CASCADE, L"Ca&scade Windows" );
    AppendMenuW( hMenu, MF_STRING | gray, IDM_TASKBAR_TILE_H, L"Tile Windows &Horizontally" );
    AppendMenuW( hMenu, MF_STRING | gray, IDM_TASKBAR_TILE_V, L"Tile Windows V&ertically" );
    AppendMenuW( hMenu, MF_STRING, IDM_TASKBAR_SHOWDESKTOP, toolbars_desktop_shown() ? L"&Show Open Windows" : L"&Show the Desktop" );
    if (undo_state.count)
    {
        WCHAR text[64];
        swprintf( text, ARRAY_SIZE(text), L"&Undo %s", undo_state.label );
        AppendMenuW( hMenu, MF_STRING, IDM_TASKBAR_UNDO, text );
    }
    AppendMenuW( hMenu, MF_SEPARATOR, 0, NULL );
    AppendMenuW( hMenu, MF_STRING, IDM_TASKBAR_TASKMGR, L"Tas&k Manager" );
    AppendMenuW( hMenu, MF_SEPARATOR, 0, NULL );
    AppendMenuW( hMenu, MF_STRING | (settings->lock_taskbar ? MF_CHECKED : 0), IDM_TASKBAR_LOCK, L"&Lock the Taskbar" );
    AppendMenuW( hMenu, MF_STRING, IDM_TASKBAR_PROPERTIES, L"P&roperties" );

    SetForegroundWindow( tray_window );
    cmd = TrackPopupMenu( hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, tray_window, NULL );
    DestroyMenu( hMenu );

    switch (cmd)
    {
    case IDM_TASKBAR_SETTIME:
        ShellExecuteW( NULL, L"open", L"control.exe", L"timedate.cpl", NULL, SW_SHOWNORMAL );
        break;
    case IDM_TASKBAR_NOTIFYCUST:
        taskbar_props_show( tray_window );
        break;
    case IDM_TASKBAR_CASCADE:
        undo_save( L"Cascade" );
        CascadeWindows( NULL, MDITILE_SKIPDISABLED, NULL, 0, NULL );
        break;
    case IDM_TASKBAR_TILE_H:
        undo_save( L"Tile" );
        TileWindows( NULL, MDITILE_HORIZONTAL | MDITILE_SKIPDISABLED, NULL, 0, NULL );
        break;
    case IDM_TASKBAR_TILE_V:
        undo_save( L"Tile" );
        TileWindows( NULL, MDITILE_VERTICAL | MDITILE_SKIPDISABLED, NULL, 0, NULL );
        break;
    case IDM_TASKBAR_SHOWDESKTOP:
        if (!toolbars_desktop_shown()) undo_save( L"Minimize All" );
        show_desktop_toggle();
        break;
    case IDM_TASKBAR_UNDO:
        undo_apply();
        break;
    case IDM_TASKBAR_TASKMGR:
        ShellExecuteW( NULL, L"open", L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL );
        break;
    case IDM_TASKBAR_LOCK:
        settings->lock_taskbar = !settings->lock_taskbar;
        taskbar_settings_save();
        toolbars_refresh();
        SendMessageW( tray_window, WM_SETTINGCHANGE, 0, 0 );
        break;
    case IDM_TASKBAR_PROPERTIES:
        taskbar_props_show( tray_window );
        break;
    case 3001: toolbars_set_visible( tray_window, TOOLBAR_ADDRESS, !toolbars_is_visible( TOOLBAR_ADDRESS ) ); break;
    case 3002: toolbars_set_visible( tray_window, TOOLBAR_LINKS, !toolbars_is_visible( TOOLBAR_LINKS ) ); break;
    case 3003: toolbars_set_visible( tray_window, TOOLBAR_DESKTOP, !toolbars_is_visible( TOOLBAR_DESKTOP ) ); break;
    case 3004: toolbars_set_visible( tray_window, TOOLBAR_QUICKLAUNCH, !toolbars_is_visible( TOOLBAR_QUICKLAUNCH ) ); break;
    case 3005: toolbars_choose_new( tray_window ); break;
    case 3006: toolbars_set_visible( tray_window, TOOLBAR_SEARCH, !toolbars_is_visible( TOOLBAR_SEARCH ) ); break;
    case 3010: case 3011: case 3012: case 3013: toolbars_set_visible( tray_window, TOOLBAR_CUSTOM + cmd - 3010, FALSE ); break;
    }
}

static void show_taskbar_contextmenu( HWND button, LPARAM lparam )
{
    struct taskbar_button *win = NULL, *it;
    POINT pt = { (short)LOWORD(lparam), (short)HIWORD(lparam) };

    LIST_FOR_EACH_ENTRY( it, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (it->button == button || (button && it->hwnd == (HWND)GetWindowLongPtrW(button, GWLP_ID)))
        {
            win = it;
            break;
        }
    }
    if (!win)
    {
        LIST_FOR_EACH_ENTRY( it, &taskbar_buttons, struct taskbar_button, entry )
        {
            RECT rc;
            if (it->button && it->visible)
            {
                GetWindowRect( it->button, &rc );
                if (PtInRect( &rc, pt )) { win = it; break; }
            }
        }
    }

    if (win && win->hwnd)
    {
        HMENU hMenu = CreatePopupMenu();
        BOOL iconic = IsIconic( win->hwnd );
        BOOL zoomed = IsZoomed( win->hwnd );
        DWORD style = GetWindowLongW( win->hwnd, GWL_STYLE );
        UINT cmd;

        AppendMenuW( hMenu, (iconic || zoomed) ? MF_STRING : (MF_STRING | MF_GRAYED),
                    1, L"&Restore" );
        AppendMenuW( hMenu, (!iconic) ? MF_STRING : (MF_STRING | MF_GRAYED),
                    2, L"&Move" );
        AppendMenuW( hMenu, (!iconic && !zoomed && (style & WS_THICKFRAME)) ? MF_STRING : (MF_STRING | MF_GRAYED),
                    3, L"&Size" );
        AppendMenuW( hMenu, (!iconic) ? MF_STRING : (MF_STRING | MF_GRAYED),
                    4, L"Mi&nimize" );
        AppendMenuW( hMenu, (!zoomed && (style & WS_MAXIMIZEBOX)) ? MF_STRING : (MF_STRING | MF_GRAYED),
                    5, L"Ma&ximize" );
        AppendMenuW( hMenu, MF_SEPARATOR, 0, NULL );
        AppendMenuW( hMenu, MF_STRING, 6, L"&Close\tAlt+F4" );

        SetForegroundWindow( tray_window );
        cmd = TrackPopupMenu( hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, tray_window, NULL );
        DestroyMenu( hMenu );

        switch (cmd)
        {
        case 1:
            ShowWindow( win->hwnd, SW_RESTORE );
            SetForegroundWindow( win->hwnd );
            break;
        case 2:
            PostMessageW( win->hwnd, WM_SYSCOMMAND, SC_MOVE, 0 );
            break;
        case 3:
            PostMessageW( win->hwnd, WM_SYSCOMMAND, SC_SIZE, 0 );
            break;
        case 4:
            ShowWindow( win->hwnd, SW_MINIMIZE );
            break;
        case 5:
            ShowWindow( win->hwnd, SW_MAXIMIZE );
            SetForegroundWindow( win->hwnd );
            break;
        case 6:
            PostMessageW( win->hwnd, WM_SYSCOMMAND, SC_CLOSE, 0 );
            break;
        }
        sync_taskbar_buttons();
        return;
    }

    track_tray_menu( pt, FALSE );
}

static void do_hide_systray(void)
{
    ShowWindow( tray_window, SW_HIDE );
}

static void do_show_systray(void)
{
    SIZE size;
    NONCLIENTMETRICSW ncm;
    HFONT font;
    HDC hdc;

    if (!enable_taskbar)
    {
        size = get_window_size();
        SetWindowPos( tray_window, 0, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW );
        return;
    }

    hdc = GetDC( 0 );

    ncm.cbSize = sizeof(NONCLIENTMETRICSW);
    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0 );
    font = CreateFontIndirectW( &ncm.lfCaptionFont );
    /* FIXME: Implement BCM_GETIDEALSIZE and use that instead. */
    SelectObject( hdc, font );
    GetTextExtentPointA( hdc, "abcdefghijklmnopqrstuvwxyz", 26, &size );
    taskbar_button_width = size.cx;
    GetTextExtentPointW( hdc, start_label, lstrlenW(start_label), &size );
    /* add some margins (FIXME) */
    size.cx += 12 + GetSystemMetrics( SM_CXSMICON );
    size.cy += 4;
    ReleaseDC( 0, hdc );
    DeleteObject( font );

    tray_width = GetSystemMetrics( SM_CXSCREEN );
    /* an icon pack installed since the last session: shell32's icons come from it from now on */
    if (shellres_sync_shell_icons()) shellres_reload_shell_icons();
    taskbar_settings_load();
    update_tray_top();
    start_button_width = startbutton_get_desired_width();
    position_taskbar();
    SetTimer( tray_window, IDT_AUTOHIDE, 150, NULL );

    /* Instantiate complete desktop taskbar components */
    {
        RECT rcTray = get_tray_notify_rect();
        RECT rcClock = get_tray_clock_rect();
        int taskband_x, taskband_w;

        startbutton_create( tray_window, 2, 2, start_button_width, 24 );
        place_start_button();

        toolbars_create( tray_window );
        toolbars_layout( tray_window, start_button_width + (theme_is_visual_styles_active() ? 4 : 7), rcTray.left, tray_top, tray_height, &taskband_x, &taskband_w );

        tray_clock_create( tray_window, rcClock.left + 2, rcClock.top + 2, rcClock.right - rcClock.left - 4, rcClock.bottom - rcClock.top - 4 );
        tray_clock_resize( rcClock.left + 2, rcClock.top + 2, rcClock.right - rcClock.left - 4, rcClock.bottom - rcClock.top - 4 );

        taskband_create( tray_window, taskband_x, tray_top, taskband_w, tray_height );
        taskband_resize( taskband_x, tray_top, taskband_w, tray_height );

        nettray_init( tray_window );
        volumetray_init( tray_window );
        powertray_init( tray_window );
    }

    sync_taskbar_buttons();
}

static void update_taskbar_layout(void)
{
    TASKBAR_SETTINGS *settings;
    RECT rcTray;
    RECT rcClock;
    int taskband_x;
    int taskband_w;

    taskbar_settings_load();
    update_tray_top();
    settings = taskbar_settings_get();
    rcTray = get_tray_notify_rect();
    rcClock = get_tray_clock_rect();

    start_button_width = startbutton_get_desired_width();
    place_start_button();

    toolbars_layout( tray_window, start_button_width + (theme_is_visual_styles_active() ? 4 : 7), rcTray.left, tray_top, tray_height, &taskband_x, &taskband_w );
    tray_clock_resize( rcClock.left + 2, rcClock.top + 2, rcClock.right - rcClock.left - 4, rcClock.bottom - rcClock.top - 4 );
    taskband_resize( taskband_x, tray_top, taskband_w, tray_height );
    position_taskbar();
    tray_clock_show( settings->show_clock );
    update_tray_layout();   /* the icons follow the height of the taskbar */
}

static LRESULT WINAPI shell_traywnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (enable_taskbar && toolbars_task_gripper_message(hwnd, msg, wparam, lparam))
        return msg == WM_SETCURSOR ? TRUE : 0;
    switch (msg)
    {
    case WM_DESTROY:
        nettray_shutdown();
        volumetray_shutdown();
        powertray_shutdown();
        break;

    case WM_COPYDATA:
    {
        COPYDATASTRUCT *data = (COPYDATASTRUCT *)lparam;
        if (data->dwData == 0x57455448) /* Display Properties: apply a .theme file */
        {
            THEME_CONFIG config;
            DWORD attrs;
            if (!data->lpData || data->cbData < sizeof(WCHAR) || data->cbData > MAX_PATH * sizeof(WCHAR) ||
                data->cbData % sizeof(WCHAR) ||
                ((const WCHAR *)data->lpData)[data->cbData / sizeof(WCHAR) - 1]) return FALSE;
            attrs = GetFileAttributesW(data->lpData);
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
            if (!theme_load_file(data->lpData, &config) || !theme_apply(&config)) return FALSE;
            PostMessageW(hwnd, WM_THEMECHANGED, 0, 0);
            return TRUE;
        }
        return handle_incoming((HWND)wparam, data);
    }

    case WM_DISPLAYCHANGE:
        if (!enable_taskbar && (!show_systray || !nb_displayed)) do_hide_systray();
        else do_show_systray();
        break;

    case WM_WINDOWPOSCHANGING:
    {
        WINDOWPOS *p = (WINDOWPOS *)lparam;

        if (p->flags & SWP_SHOWWINDOW && (!show_systray || !nb_displayed) && !enable_taskbar)
        {
            TRACE( "WM_WINDOWPOSCHANGING clearing SWP_SHOWWINDOW.\n" );
            p->flags &= ~SWP_SHOWWINDOW;
        }
        break;
    }

    case WM_MOVE:
        update_systray_balloon_position();
        break;

    case WM_CLOSE:
        /* don't destroy the tray window, just hide it */
        ShowWindow( hwnd, SW_HIDE );
        hide_balloon( balloon_icon );
        show_systray = FALSE;
        return 0;

    case WM_DRAWITEM:
        if (startmenu_view_menu_icon(msg, lparam)) return TRUE;
        if (((MEASUREITEMSTRUCT*)lparam)->CtlType == ODT_MENU)
            return menu_wndproc(hwnd, msg, wparam, lparam);
        paint_taskbar_button( (const DRAWITEMSTRUCT *)lparam );
        break;

    case WM_MEASUREITEM:
        if (startmenu_view_menu_icon(msg, lparam)) return TRUE;
        if (((MEASUREITEMSTRUCT*)lparam)->CtlType == ODT_MENU)
            return menu_wndproc(hwnd, msg, wparam, lparam);
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC real = BeginPaint( hwnd, &ps ), hdc;
        /* paint into a buffer and copy it at once: no flashing while the bands are moved or resized */
        HPAINTBUFFER buffer = BeginBufferedPaint( real, &ps.rcPaint, BPBF_COMPATIBLEBITMAP, NULL, &hdc );
        RECT rc;

        if (!buffer) hdc = real;
        GetClientRect( hwnd, &rc );
        paint_tray_background( hwnd, hdc, &rc );

        if (chevron_visible())
        {
            RECT chev = get_tray_chevron_rect();
            HTHEME button = theme_open( hwnd, tray_expanded ? L"TrayNotifyHorizOpen::Button" : L"TrayNotifyHoriz::Button" );
            if (button)
            {
                DrawThemeBackground( button, hdc, 1, 1, &chev, NULL );
                CloseThemeData( button );
            }
            else
            {
                /* Windows Classic: the two arrows are plain text, no button */
                HFONT font = CreateFontW( -11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, L"Tahoma" );
                HFONT prev = SelectObject( hdc, font );
                SetBkMode( hdc, TRANSPARENT );
                SetTextColor( hdc, GetSysColor( COLOR_BTNTEXT ) );
                DrawTextW( hdc, tray_expanded ? L"\u00bb" : L"\u00ab", -1, &chev, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
                SelectObject( hdc, prev );
                DeleteObject( font );
            }
        }

        /* Draw sunken border around entire notification area in Classic mode */
        if (enable_taskbar)
        {
            if (!theme_is_visual_styles_active())
            {
                RECT rcTray = get_tray_notify_rect();
                DrawEdge( hdc, &rcTray, EDGE_SUNKEN, BF_RECT );
            }
        }

        toolbars_draw_task_gripper( hwnd, hdc, tray_top, tray_height );

        if (buffer) EndBufferedPaint( buffer, TRUE );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_PRINTCLIENT:
    {
        /* the background the bands, the clock, the Start button and the tray icons draw under themselves
         * (DrawThemeParentBackground) */
        RECT rc;
        GetClientRect( hwnd, &rc );
        paint_tray_background( hwnd, (HDC)wparam, &rc );
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED)
        {
            if (LOWORD(wparam) == TRAY_MINIMIZE_ALL || LOWORD(wparam) == TRAY_MINIMIZE_ALL_UNDO)
            {
                show_desktop_toggle();
                break;
            }
            click_taskbar_button( (HWND)lparam );
        }
        break;

    case WM_TRAY_CLOCKMENU:
    {
        POINT pt = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
        track_tray_menu( pt, TRUE );
        return 0;
    }

    case WM_CONTEXTMENU:
        show_taskbar_contextmenu( (HWND)wparam, lparam );
        break;

    case WM_HOTKEY:
        switch (wparam)
        {
        case IDHK_RUN: startmenu_run( STARTMENU_CMD_RUN, hwnd ); break;
        case IDHK_FIND: startmenu_run( STARTMENU_CMD_SEARCH, hwnd ); break;
        case IDHK_EXPLORE: shellfolder_open( CSIDL_DRIVES, TRUE ); break;
        case IDHK_SYS_PROPERTIES: ShellExecuteW( NULL, L"open", L"control.exe", L"sysdm.cpl", NULL, SW_SHOWNORMAL ); break;
        case IDHK_RESTORE_ALL: undo_apply(); break;
        case IDHK_LOCK: power_mgmt_execute_action( POWER_ACTION_LOCK ); break;
        case IDHK_MINIMIZE_ALL:
        case IDHK_DESKTOP:
            if (wparam == IDHK_MINIMIZE_ALL && toolbars_desktop_shown()) break;
            if (!toolbars_desktop_shown()) undo_save( L"Minimize All" );
            show_desktop_toggle();
            break;
        }
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_INITMENUPOPUP:
    case WM_MENUCOMMAND:
        return menu_wndproc(hwnd, msg, wparam, lparam);

    case WM_ENTERMENULOOP:
        startmenu_on_enter_menu_loop();
        break;

    case WM_THEMECHANGED:
        /* the icon packs belong to the theme: reload them everywhere */
        theme_reload();
        theme_sync_colors();
        shellres_reset();
        shellres_sync_shell_icons();
        shellres_reload_shell_icons();
        desktop_view_refresh();
        toolbars_refresh();
        /* repaint everything once the change has settled (children paint late after a live switch) */
        SetTimer( hwnd, IDT_THEMEREDRAW, 300, NULL );
        /* fall through */
    case WM_SETTINGCHANGE:
        update_taskbar_layout();
        RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN );
        break;

    case WM_TIMECHANGE:
        tray_clock_update();
        break;

    case WM_SETCURSOR:
    {
        POINT pt;
        GetCursorPos( &pt );
        ScreenToClient( hwnd, &pt );
        if (LOWORD(lparam) == HTCLIENT && on_sizing_edge( pt.y ))
        {
            SetCursor( LoadCursorW( 0, (const WCHAR *)IDC_SIZENS ) );
            return TRUE;
        }
        return DefWindowProcW( hwnd, msg, wparam, lparam );
    }

    case WM_MOUSEMOVE:
        if (sizing_rows && GetCapture() == hwnd)
        {
            /* the height snaps to whole rows of buttons, up to half the screen */
            POINT pt;
            int rows;

            GetCursorPos( &pt );
            rows = (GetSystemMetrics( SM_CYSCREEN ) - pt.y - tray_top - theme_taskbar_first_row() + theme_taskbar_row_pitch() / 2) / theme_taskbar_row_pitch() + 1;
            rows = max( 1, min( rows, max_rows() ));
            if (rows != tray_rows)
            {
                tray_rows = rows;
                update_taskbar_layout();
            }
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (sizing_rows)
        {
            DWORD rows = tray_rows;
            sizing_rows = FALSE;
            if (msg == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
            RegSetKeyValueW( HKEY_CURRENT_USER, rows_key, L"Rows", REG_DWORD, &rows, sizeof(rows) );
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (on_sizing_edge( (short)HIWORD(lparam) ))
        {
            sizing_rows = TRUE;
            SetCapture( hwnd );
            return 0;
        }
        if (chevron_visible())
        {
            RECT chev = get_tray_chevron_rect();
            POINT pt = { (short)LOWORD(lparam), (short)HIWORD(lparam) };
            if (PtInRect( &chev, pt ))
            {
                tray_expanded = !tray_expanded;
                tray_expanded_hover_tick = GetTickCount();
                refresh_inactive_icons();
                update_taskbar_layout();
                InvalidateRect( hwnd, NULL, FALSE );
                return 0;
            }
        }
        break;

    case WM_TIMER:
        if (wparam == IDT_THEMEREDRAW)
        {
            KillTimer( hwnd, IDT_THEMEREDRAW );
            update_taskbar_layout();
            RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW );
            break;
        }
        if (wparam == IDT_AUTOHIDE)
        {
            static DWORD last_inactive_check;
            static BOOL was_down;
            BOOL down = (GetAsyncKeyState( VK_LBUTTON ) & 0x8000) != 0;

            autohide_tick();
            /* the expanded notification area closes when something else is clicked */
            if (tray_expanded && down && !was_down)
            {
                POINT pt;
                RECT area = get_tray_notify_rect();
                GetCursorPos( &pt );
                ScreenToClient( tray_window, &pt );
                if (!PtInRect( &area, pt ))
                {
                    tray_expanded = FALSE;
                    refresh_inactive_icons();
                    update_taskbar_layout();
                }
            }
            if (tray_expanded)
            {
                POINT pt;
                RECT area = get_tray_notify_rect();
                GetCursorPos( &pt );
                ScreenToClient( tray_window, &pt );
                if (PtInRect( &area, pt ) || GetCapture()) tray_expanded_hover_tick = GetTickCount();
                else if (GetTickCount() - tray_expanded_hover_tick >= 5000)
                {
                    tray_expanded = FALSE;
                    refresh_inactive_icons();
                    update_taskbar_layout();
                }
            }
            was_down = down;
            if (GetTickCount() - last_inactive_check > 1000)
            {
                last_inactive_check = GetTickCount();
                refresh_inactive_icons();
            }
            return 0;
        }
        if (wparam == 9999)
        {
            KillTimer(hwnd, 9999);
            startmenu_on_enter_menu_loop();
            return 0;
        }
        break;

    case WM_USER + 0:
        update_systray_balloon_position();
        return 0;

    case WM_USER + 1:
    {
        struct icon *icon;

        LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
        {
            if (!icon->window) continue;
            hide_icon( icon );
            show_icon( icon );
        }

        return 0;
    }

    default:
        return DefWindowProcW( hwnd, msg, wparam, lparam );
    }
    return 0;
}

/* notification posted to the desktop window */
void handle_parent_notify( HWND hwnd, WPARAM wp )
{
    switch (LOWORD(wp))
    {
    case WM_CREATE:
        if (enable_taskbar) taskband_sync_windows();
        else add_taskbar_button( hwnd );
        break;
    case WM_DESTROY:
        if (enable_taskbar) taskband_sync_windows();
        else remove_taskbar_button( hwnd );
        cleanup_systray_window( hwnd );
        break;
    }
    sync_taskbar_buttons();
}

/* this function creates the listener window */
void initialize_systray( BOOL arg_using_root, BOOL arg_enable_shell, BOOL arg_show_systray, BOOL arg_no_tray_items )
{
    RECT work_rect, primary_rect, taskbar_rect;

    shell_traywnd_class.hIcon = LoadIconW( 0, (const WCHAR *)IDI_WINLOGO );
    shell_traywnd_class.hCursor = LoadCursorW( 0, (const WCHAR *)IDC_ARROW );
    tray_icon_class.hIcon = shell_traywnd_class.hIcon;
    tray_icon_class.hCursor = shell_traywnd_class.hCursor;

    icon_cx = GetSystemMetrics( SM_CXSMICON ) + 2*ICON_BORDER;
    icon_cy = GetSystemMetrics( SM_CYSMICON ) + 2*ICON_BORDER;

    if (arg_using_root)
    {
        show_systray = arg_show_systray;
        enable_taskbar = FALSE;
        enable_dock = TRUE;
    }
    else
    {
        show_systray = arg_show_systray && !arg_enable_shell;
        enable_taskbar = arg_enable_shell;
        enable_dock = FALSE;
    }

    no_tray_items = arg_no_tray_items;

    /* register the systray listener window class */
    if (!RegisterClassExW( &shell_traywnd_class ))
    {
        ERR( "Could not register SysTray window class\n" );
        return;
    }
    if (!RegisterClassExW( &tray_icon_class ))
    {
        ERR( "Could not register Wine SysTray window classes\n" );
        return;
    }

    if (enable_taskbar)
    {
        SystemParametersInfoW( SPI_GETWORKAREA, 0, &work_rect, 0 );
        SetRect( &primary_rect, 0, 0, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
        SubtractRect( &taskbar_rect, &primary_rect, &work_rect );

        tray_window = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW, shell_traywnd_class.lpszClassName, NULL, WS_POPUP | WS_CLIPCHILDREN,
                                       taskbar_rect.left, taskbar_rect.top, taskbar_rect.right - taskbar_rect.left,
                                       taskbar_rect.bottom - taskbar_rect.top, 0, 0, 0, 0 );
    }
    else
    {
        SIZE size = get_window_size();
        tray_window = CreateWindowExW( 0, shell_traywnd_class.lpszClassName, L"", WS_CAPTION | WS_SYSMENU,
                                       CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy, 0, 0, 0, 0 );
        NtUserMessageCall( tray_window, WINE_SYSTRAY_DOCK_INIT, 0, 0, NULL, NtUserSystemTrayCall, FALSE );
    }

    if (!tray_window)
    {
        ERR( "Could not create tray window\n" );
        return;
    }

    LoadStringW( NULL, IDS_START_LABEL, start_label, ARRAY_SIZE( start_label ));

    if (!enable_taskbar) add_taskbar_button( 0 );

    if (enable_taskbar)
    {
        static const struct { int id; UINT mod, key; } hotkeys[] =
        {
            { IDHK_RUN, MOD_WIN, 'R' }, { IDHK_MINIMIZE_ALL, MOD_WIN, 'M' }, { IDHK_RESTORE_ALL, MOD_WIN | MOD_SHIFT, 'M' },
            { IDHK_EXPLORE, MOD_WIN, 'E' }, { IDHK_FIND, MOD_WIN, 'F' }, { IDHK_SYS_PROPERTIES, MOD_WIN, VK_PAUSE },
            { IDHK_DESKTOP, MOD_WIN, 'D' }, { IDHK_LOCK, MOD_WIN, 'L' },
        };
        unsigned int i;
        for (i = 0; i < ARRAY_SIZE(hotkeys); i++) RegisterHotKey( tray_window, hotkeys[i].id, hotkeys[i].mod, hotkeys[i].key );
        do_show_systray();
    }
    else do_hide_systray();
}
