/*
 * X11DRV: the secure desktop's presentation (docs/ELEVATION_SECURE_DESKTOP_WORKORDER.md, phase 3)
 *
 * A process on the desktop WinSta0\Winlogon (the consent window of the elevation broker) makes itself exclusive on
 * the X display: a full-screen override-redirect window over a dimmed snapshot of the screen, taken when the driver
 * loads (before the process maps a window of its own), and keyboard and pointer grabs. Events for windows of this X
 * client are delivered normally during a grab, every other client gets none. X drops the grabs when the process
 * dies, so a crash cannot leave the display grabbed. The server decides who may be on that desktop and who may
 * switch to it; this is only what the user sees and what the host keyboard and pointer reach.
 *
 * The desktop manager that Wine starts for the desktop (explorer.exe) is left alone: it would otherwise take the
 * grabs before the program that owns the dialog.
 *
 * Limits: native X11 clients of the same user (for example xdotool or an X keylogger) still reach the display;
 * another client may map a window above the overlay while the desktop is up (it gets no input).
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <string.h>
#include <unistd.h>

#include "x11drv.h"
#include "ntuser.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

static int secure_state;    /* 0: not looked at yet, 1: an ordinary desktop (or its manager), 2: the secure desktop */
static XImage *snapshot;    /* the dimmed screen, until the overlay is made */

/* the desktop of this process is the secure desktop: its name is Winlogon. The server lets only the system role create
 * or open it; a program that names a desktop that way elsewhere only grabs its own display, as any X client can. */
static BOOL on_secure_desktop(void)
{
    static const WCHAR winlogon[] = { 'W','i','n','l','o','g','o','n',0 };
    WCHAR name[64];
    HDESK desktop;
    DWORD needed;

    if (!(desktop = NtUserGetThreadDesktop( GetCurrentThreadId() ))) return FALSE;
    if (!NtUserGetObjectInformation( desktop, UOI_NAME, name, sizeof(name), &needed )) return FALSE;
    return !wcsicmp( name, winlogon );
}

/* explorer.exe is what Wine starts to own the desktop window of a desktop */
static BOOL is_desktop_manager(void)
{
    static const WCHAR explorer[] = { 'e','x','p','l','o','r','e','r','.','e','x','e',0 };
    const UNICODE_STRING *path = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
    unsigned int len = path->Length / sizeof(WCHAR), name_len = ARRAY_SIZE(explorer) - 1;

    return len >= name_len && !wcsnicmp( path->Buffer + len - name_len, explorer, name_len ) &&
           (len == name_len || path->Buffer[len - name_len - 1] == '\\');
}

static unsigned long high_bit( unsigned long mask )
{
    unsigned long bit = 1;

    while (mask >>= 1) bit <<= 1;
    return bit;
}

/* halve every colour channel of a 32 bits per pixel image (the dimmed snapshot); other formats stay as they are */
static void dim_image( XImage *image )
{
    unsigned long mask = image->red_mask | image->green_mask | image->blue_mask;
    unsigned long top = high_bit( image->red_mask ) | high_bit( image->green_mask ) | high_bit( image->blue_mask );
    int x, y;

    if (image->bits_per_pixel != 32) return;
    for (y = 0; y < image->height; y++)
    {
        unsigned int *row = (unsigned int *)(image->data + y * image->bytes_per_line);
        for (x = 0; x < image->width; x++) row[x] = ((row[x] & mask) >> 1) & (mask & ~top);
    }
}

/* Called when the driver loads, before the process has a window: on the secure desktop, take the snapshot. */
void x11drv_secure_init( Display *display )
{
    secure_state = 1;
    if (!on_secure_desktop() || is_desktop_manager()) return;
    secure_state = 2;
    snapshot = XGetImage( display, DefaultRootWindow( display ), 0, 0, DisplayWidth( display, DefaultScreen( display )),
                          DisplayHeight( display, DefaultScreen( display )), AllPlanes, ZPixmap );
    if (snapshot) dim_image( snapshot );
}

/* Is the process on the secure desktop? Its windows are then not managed by a window manager: they are stacked in the
 * order they are mapped, over the overlay. */
BOOL x11drv_secure_desktop(void)
{
    return secure_state == 2;
}

/* cover the screen with the snapshot, and grab the keyboard and the pointer */
static void make_exclusive( Display *display )
{
    Window root = DefaultRootWindow( display ), overlay;
    int screen = DefaultScreen( display ), width = DisplayWidth( display, screen ), height = DisplayHeight( display, screen );
    XSetWindowAttributes attr = { .override_redirect = True, .event_mask = 0 };
    unsigned long mask = CWOverrideRedirect | CWEventMask;
    Pixmap pixmap = None;
    int i;

    if (snapshot && (pixmap = XCreatePixmap( display, root, width, height, snapshot->depth )))
    {
        GC gc = XCreateGC( display, pixmap, 0, NULL );
        XPutImage( display, pixmap, gc, snapshot, 0, 0, 0, 0, width, height );
        XFreeGC( display, gc );
        attr.background_pixmap = pixmap;
        mask |= CWBackPixmap;
    }
    if (snapshot) XDestroyImage( snapshot );
    snapshot = NULL;
    if (!pixmap)
    {
        attr.background_pixel = BlackPixel( display, screen );
        mask |= CWBackPixel;
    }
    overlay = XCreateWindow( display, root, 0, 0, width, height, 0, CopyFromParent, InputOutput, CopyFromParent, mask, &attr );
    if (pixmap) XFreePixmap( display, pixmap );
    XMapRaised( display, overlay );
    XSync( display, False );

    for (i = 0; i < 40; i++)
    {
        if (XGrabKeyboard( display, overlay, True, GrabModeAsync, GrabModeAsync, CurrentTime ) == GrabSuccess) break;
        usleep( 50000 );
    }
    if (i == 40) WARN( "could not grab the keyboard\n" );
    for (i = 0; i < 40; i++)
    {
        if (XGrabPointer( display, overlay, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                          GrabModeAsync, GrabModeAsync, None, None, CurrentTime ) == GrabSuccess) break;
        usleep( 50000 );
    }
    if (i == 40) WARN( "could not grab the pointer\n" );
    XSync( display, False );
}

/* Called before the first X window of a thread is created, with that thread's display (the grabs must belong to the
 * X client that owns the windows): on the secure desktop, cover the screen and grab, once. */
void x11drv_secure_exclusive( Display *display )
{
    static BOOL done;

    if (done || secure_state != 2) return;
    done = TRUE;
    make_exclusive( display );
}
