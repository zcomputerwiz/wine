/*
 * X11DRV desktop window handling
 *
 * Copyright 2001 Alexandre Julliard
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

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/Xlib.h>

#include "x11drv.h"

/* avoid conflict with field names in included win32 headers */
#undef Status
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

static RECT host_primary_rect;
static BOOL retain_desktop( const WCHAR *name );

#define _NET_WM_STATE_REMOVE 0
#define _NET_WM_STATE_ADD 1

/* Return TRUE if Wine is currently in virtual desktop mode */
BOOL is_virtual_desktop(void)
{
    return root_window != DefaultRootWindow( gdi_display );
}

/***********************************************************************
 *		X11DRV_init_desktop
 *
 * Setup the desktop when not using the root window.
 */
void X11DRV_init_desktop( Window win )
{
    host_primary_rect = get_host_primary_monitor_rect();
    root_window = win;
    managed_mode = FALSE;  /* no managed windows in desktop mode */
}

/***********************************************************************
 *           X11DRV_CreateDesktop
 *
 * Create the X11 desktop window for the desktop mode.
 */
BOOL X11DRV_CreateDesktop( const WCHAR *name, UINT width, UINT height )
{
    XSetWindowAttributes win_attr;
    Window win;
    Display *display = thread_init_display();

    TRACE( "%s %ux%u\n", debugstr_w(name), width, height );

    /* Create window */
    win_attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | EnterWindowMask |
                          PointerMotionMask | ButtonPressMask | ButtonReleaseMask | FocusChangeMask |
                          StructureNotifyMask | PropertyChangeMask;
    win_attr.cursor = XCreateFontCursor( display, XC_top_left_arrow );

    if (default_visual.visual != DefaultVisual( display, DefaultScreen(display) ))
        win_attr.colormap = XCreateColormap( display, DefaultRootWindow(display),
                                             default_visual.visual, AllocNone );
    else
        win_attr.colormap = None;

    win = XCreateWindow( display, DefaultRootWindow(display),
                         0, 0, width, height, 0, default_visual.depth, InputOutput,
                         default_visual.visual, CWEventMask | CWCursor | CWColormap, &win_attr );
    if (!win) return FALSE;

    x11drv_xinput2_enable( display, win );
    if (retain_desktop( name )) XSetCloseDownMode( display, RetainTemporary );
    XFlush( display );

    X11DRV_init_desktop( win );
    return TRUE;
}

/* B121: the session shell's X desktop window outlives the shell. The programs that run across a shell restart (B120) have
 * windows inside it, and a destroyed desktop window takes them with it. With WINE_X11_RETAIN_DESKTOP set (the session
 * scripts do), the X server keeps the dead desktop manager's resources, and the new manager takes the window over. */
static BOOL retain_desktop( const WCHAR *name )
{
    static const char default_name[] = "Default";
    const char *env = getenv( "WINE_X11_RETAIN_DESKTOP" );
    unsigned int i;

    if (!env || !*env || *env == '0') return FALSE;
    if (!name) return TRUE;  /* the caller has no name: the desktop is the one that was retained */
    /* the session shell's desktop only: another desktop a program makes goes with it when it ends */
    for (i = 0; default_name[i]; i++)
        if (!name[i] || (name[i] | 0x20) != (default_name[i] | 0x20)) return FALSE;
    return !name[i];
}

static int ignore_x_error( Display *display, XErrorEvent *event, void *arg )
{
    return 1;
}

/* the dead manager's own windows (the taskbar and the like) stay in the retained desktop: their process is gone */
static void destroy_dead_children( Display *display, Window desktop )
{
    Window root, parent, *children;
    unsigned int i, count;

    if (!XQueryTree( display, desktop, &root, &parent, &children, &count )) return;
    for (i = 0; i < count; i++)
    {
        Atom type;
        int format;
        unsigned long nitems, left;
        unsigned char *data;
        long pid;

        X11DRV_expect_error( display, ignore_x_error, NULL );
        if (!XGetWindowProperty( display, children[i], x11drv_atom(_NET_WM_PID), 0, 1, False, XA_CARDINAL,
                                 &type, &format, &nitems, &left, &data ) && data)
        {
            pid = (nitems == 1 && format == 32) ? *(long *)data : 0;
            XFree( data );
            if (pid > 0 && pid != getpid() && kill( pid, 0 ) == -1 && errno == ESRCH)
            {
                TRACE( "destroying %lx of the dead process %ld\n", children[i], pid );
                XDestroyWindow( display, children[i] );
            }
        }
        XSync( display, False );
        X11DRV_check_error();
    }
    if (children) XFree( children );
}

/* whether the X window still exists */
static BOOL x_window_exists( Display *display, Window win, XWindowAttributes *attrs )
{
    int ok;

    X11DRV_expect_error( display, ignore_x_error, NULL );
    ok = XGetWindowAttributes( display, win, attrs );
    XSync( display, False );
    return !X11DRV_check_error() && ok;
}

/* The old desktop window is left behind and not taken over. It must not stay: it is a full-screen window of the dead
 * manager, covering the new desktop (a blank screen) and listed by the window manager as a second "Wine Desktop". It goes
 * with the dead manager's taskbar and the windows of the programs inside it, as before B121, when the manager's
 * connection closing took them. */
static void destroy_retained_desktop( Display *display, Window old )
{
    XWindowAttributes attrs;

    if (!x_window_exists( display, old, &attrs )) return;
    WARN( "destroying the retained desktop window %lx\n", old );
    X11DRV_expect_error( display, ignore_x_error, NULL );
    XDestroyWindow( display, old );
    XSync( display, False );
    X11DRV_check_error();
}

/* Wine tests only (a test of the failed takeover): WINE_X11_RETAIN_TEST_REFUSE set refuses the takeover as if another
 * client held the window's events */
static BOOL test_refuse_takeover(void)
{
    const char *env = getenv( "WINE_X11_RETAIN_TEST_REFUSE" );
    return env && *env && *env != '0';
}

/* The old connection can still be closing when the replacement starts (the manager was killed): the window is not there
 * yet or still held. A short wait, 20 tries of 50 ms. */
#define ADOPT_TRIES 20

/***********************************************************************
 *           X11DRV_adopt_desktop
 *
 * The desktop manager that replaces a dead one takes over the X window the dead one left (the stored id): returns FALSE
 * when there is none (not retained, or gone), and the window this process made is used. When the window is there and
 * cannot be taken over, it is destroyed (see destroy_retained_desktop), so that the window made here is the only one.
 */
BOOL X11DRV_adopt_desktop( Window old )
{
    Display *display = thread_init_display();
    XWindowAttributes attrs;
    XSetWindowAttributes win_attr;
    Window ours = root_window;
    unsigned int tries;
    BOOL taken = FALSE;

    if (!old || old == ours || !is_virtual_desktop() || old == DefaultRootWindow( display )) return FALSE;

    if (!retain_desktop( NULL ))
    {
        WARN( "the desktop window %lx is not taken over: WINE_X11_RETAIN_DESKTOP is not set\n", old );
        destroy_retained_desktop( display, old );
        return FALSE;
    }

    for (tries = 0; !x_window_exists( display, old, &attrs ); tries++)
    {
        if (tries >= ADOPT_TRIES)
        {
            WARN( "the desktop window %lx is gone\n", old );
            return FALSE;
        }
        usleep( 50000 );
    }

    TRACE( "taking over the desktop window %lx from %lx\n", old, ours );
    /* the dead manager's ButtonPress selection is still held by the X server (one client only), and the shell's own windows cover the desktop */
    win_attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | EnterWindowMask |
                          PointerMotionMask | ButtonReleaseMask | FocusChangeMask |
                          StructureNotifyMask | PropertyChangeMask;
    win_attr.cursor = XCreateFontCursor( display, XC_top_left_arrow );
    for (tries = 0; tries < ADOPT_TRIES && !taken; tries++)
    {
        if (tries) usleep( 50000 );
        if (test_refuse_takeover()) break;
        X11DRV_expect_error( display, ignore_x_error, NULL );
        XChangeWindowAttributes( display, old, CWEventMask | CWCursor, &win_attr );
        XSync( display, False );
        taken = !X11DRV_check_error();
    }
    if (!taken)  /* another client refuses part of the mask: the window made here is the desktop, and the old one goes */
    {
        WARN( "cannot take over the desktop window %lx\n", old );
        destroy_retained_desktop( display, old );
        return FALSE;
    }
    if (XGetWindowAttributes( display, ours, &attrs ))
        XResizeWindow( display, old, attrs.width, attrs.height );
    X11DRV_expect_error( display, ignore_x_error, NULL );  /* a button selection of the dead manager may remain */
    x11drv_xinput2_enable( display, old );
    XSync( display, False );
    X11DRV_check_error();
    X11DRV_init_desktop( old );
    XDestroyWindow( display, ours );
    destroy_dead_children( display, old );
    XFlush( display );
    return TRUE;
}

BOOL is_desktop_fullscreen(void)
{
    RECT primary_rect = NtUserGetPrimaryMonitorRect();
    return (primary_rect.right - primary_rect.left == host_primary_rect.right - host_primary_rect.left &&
            primary_rect.bottom - primary_rect.top == host_primary_rect.bottom - host_primary_rect.top);
}
