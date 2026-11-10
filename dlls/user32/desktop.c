/*
 * Desktop window class.
 *
 * Copyright 1994 Alexandre Julliard
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
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winnls.h"
#include "controls.h"

static HBRUSH hbrushPattern;
static HBITMAP hbitmapWallPaper;
static SIZE bitmapSize;
static BOOL fTileWallPaper;


/***********************************************************************
 *           DESKTOP_LoadBitmap
 */
static HBITMAP DESKTOP_LoadBitmap( const WCHAR *filename )
{
    HBITMAP hbitmap;

    if (!filename[0]) return 0;
    hbitmap = LoadImageW( 0, filename, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION | LR_LOADFROMFILE );
    if (!hbitmap)
    {
        WCHAR buffer[MAX_PATH];
        UINT len = GetWindowsDirectoryW( buffer, MAX_PATH - 2 );
        if (buffer[len - 1] != '\\') buffer[len++] = '\\';
        lstrcpynW( buffer + len, filename, MAX_PATH - len );
        hbitmap = LoadImageW( 0, buffer, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION | LR_LOADFROMFILE );
    }
    return hbitmap;
}

/***********************************************************************
 *           init_wallpaper
 */
static void init_wallpaper( const WCHAR *wallpaper )
{
    HBITMAP hbitmap = DESKTOP_LoadBitmap( wallpaper );

    if (hbitmapWallPaper) DeleteObject( hbitmapWallPaper );
    hbitmapWallPaper = hbitmap;
    if (hbitmap)
    {
	BITMAP bmp;
	GetObjectA( hbitmap, sizeof(bmp), &bmp );
	bitmapSize.cx = (bmp.bmWidth != 0) ? bmp.bmWidth : 1;
	bitmapSize.cy = (bmp.bmHeight != 0) ? bmp.bmHeight : 1;
        fTileWallPaper = GetProfileIntA( "desktop", "TileWallPaper", 0 );
    }
}

/***********************************************************************
 *           DesktopWndProcA
 */
LRESULT WINAPI DesktopWndProcA( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    switch (message)
    {
    case WM_NCCREATE:
    case WM_NCCALCSIZE:
    case WM_PARENTNOTIFY:
    case WM_DISPLAYCHANGE:
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, TRUE );

    default:
        if (message < WM_USER)
            return DefWindowProcA( hwnd, message, wParam, lParam );
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, TRUE );
    }
}

/***********************************************************************
 *           DesktopWndProcW
 */
LRESULT WINAPI DesktopWndProcW( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    switch (message)
    {
    case WM_NCCREATE:
    case WM_NCCALCSIZE:
    case WM_PARENTNOTIFY:
    case WM_DISPLAYCHANGE:
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, FALSE );

    default:
        if (message < WM_USER)
            return DefWindowProcW( hwnd, message, wParam, lParam );
        return NtUserMessageCall( hwnd, message, wParam, lParam, 0, NtUserDesktopWindowProc, FALSE );
    }
}

/* WineExplorer Safe Mode (SM_CLEANBOOT): a black desktop with "Safe Mode" in the corners of the work area and the
 * Wine version centred on the top row, as Windows draws it */
static void paint_safe_mode( HDC hdc, const RECT *desktop )
{
    static const WCHAR safe_modeW[] = L"Safe Mode";
    const char * (CDECL *get_version)(void);
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    HFONT font = 0;
    char version[64];
    RECT work;
    int len = lstrlenW( safe_modeW ), state = SaveDC( hdc );

    FillRect( hdc, desktop, GetStockObject( BLACK_BRUSH ) );
    if (!SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 )) work = *desktop;
    /* the window caption font, smoothed in gray */
    if (SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0 ))
    {
        ncm.lfCaptionFont.lfQuality = ANTIALIASED_QUALITY;
        if ((font = CreateFontIndirectW( &ncm.lfCaptionFont ))) SelectObject( hdc, font );
    }
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, RGB(255, 255, 255) );
    SetTextAlign( hdc, TA_LEFT | TA_TOP );
    TextOutW( hdc, work.left, work.top, safe_modeW, len );
    SetTextAlign( hdc, TA_RIGHT | TA_TOP );
    TextOutW( hdc, work.right, work.top, safe_modeW, len );
    SetTextAlign( hdc, TA_LEFT | TA_BOTTOM );
    TextOutW( hdc, work.left, work.bottom, safe_modeW, len );
    SetTextAlign( hdc, TA_RIGHT | TA_BOTTOM );
    TextOutW( hdc, work.right, work.bottom, safe_modeW, len );
    if ((get_version = (void *)GetProcAddress( GetModuleHandleW( L"ntdll.dll" ), "wine_get_version" )))
    {
        snprintf( version, sizeof(version), "Wine %s", get_version() );
        SetTextAlign( hdc, TA_CENTER | TA_TOP );
        TextOutA( hdc, (work.left + work.right) / 2, work.top, version, strlen( version ) );
    }
    RestoreDC( hdc, state );
    if (font) DeleteObject( font );
}

/***********************************************************************
 *           PaintDesktop   (USER32.@)
 *
 */
BOOL WINAPI PaintDesktop(HDC hdc)
{
    HWND hwnd = GetDesktopWindow();

    /* check for an owning thread; otherwise don't paint anything (non-desktop mode) */
    if (GetWindowThreadProcessId( hwnd, NULL ))
    {
        RECT rect;

        GetClientRect( hwnd, &rect );

        if (GetSystemMetrics( SM_CLEANBOOT ))
        {
            paint_safe_mode( hdc, &rect );
            return TRUE;
        }

        /* Paint desktop pattern (only if wall paper does not cover everything) */

        if (!hbitmapWallPaper ||
            (!fTileWallPaper && ((bitmapSize.cx < rect.right) || (bitmapSize.cy < rect.bottom))))
        {
            HBRUSH brush = hbrushPattern;
            if (!brush) brush = (HBRUSH)GetClassLongPtrW( hwnd, GCLP_HBRBACKGROUND );
            /* Set colors in case pattern is a monochrome bitmap */
            SetBkColor( hdc, RGB(0,0,0) );
            SetTextColor( hdc, GetSysColor(COLOR_BACKGROUND) );
            FillRect( hdc, &rect, brush );
        }

        /* Paint wall paper */

        if (hbitmapWallPaper)
        {
            INT x, y;
            HDC hMemDC = CreateCompatibleDC( hdc );

            SelectObject( hMemDC, hbitmapWallPaper );

            if (fTileWallPaper)
            {
                for (y = 0; y < rect.bottom; y += bitmapSize.cy)
                    for (x = 0; x < rect.right; x += bitmapSize.cx)
                        BitBlt( hdc, x, y, bitmapSize.cx, bitmapSize.cy, hMemDC, 0, 0, SRCCOPY );
            }
            else
            {
                x = (rect.left + rect.right - bitmapSize.cx) / 2;
                y = (rect.top + rect.bottom - bitmapSize.cy) / 2;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                BitBlt( hdc, x, y, bitmapSize.cx, bitmapSize.cy, hMemDC, 0, 0, SRCCOPY );
            }
            DeleteDC( hMemDC );
        }
    }
    return TRUE;
}

/***********************************************************************
 *           SetDeskWallpaper   (USER32.@)
 */
BOOL WINAPI SetDeskWallpaper( const char *filename )
{
    return SystemParametersInfoA( SPI_SETDESKWALLPAPER, MAX_PATH, (void *)filename, SPIF_UPDATEINIFILE );
}

/***********************************************************************
 *           update_wallpaper
 */
BOOL update_wallpaper( const WCHAR *wallpaper, const WCHAR *pattern )
{
    int pat[8];

    if (hbrushPattern) DeleteObject( hbrushPattern );
    hbrushPattern = 0;
    memset( pat, 0, sizeof(pat) );
    if (pattern)
    {
        char buffer[64];
        WideCharToMultiByte( CP_ACP, 0, pattern, -1, buffer, sizeof(buffer), NULL, NULL );
        if (sscanf( buffer, " %d %d %d %d %d %d %d %d",
                    &pat[0], &pat[1], &pat[2], &pat[3],
                    &pat[4], &pat[5], &pat[6], &pat[7] ))
        {
            WORD ptrn[8];
            HBITMAP hbitmap;
            int i;

            for (i = 0; i < 8; i++) ptrn[i] = pat[i] & 0xffff;
            hbitmap = CreateBitmap( 8, 8, 1, 1, ptrn );
            hbrushPattern = CreatePatternBrush( hbitmap );
            DeleteObject( hbitmap );
        }
    }
    init_wallpaper( wallpaper );
    NtUserRedrawWindow( GetDesktopWindow(), 0, 0, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN );
    return TRUE;
}
