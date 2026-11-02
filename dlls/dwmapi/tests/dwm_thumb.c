/*
 * WineExplorer Desktop Environment - dwm_thumb.c
 *
 * Conformance tests for live thumbnails held by the DWM broker (Phase P8, slice 1).
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

static HRESULT (WINAPI *pWexDwmpThumbList)(UINT64 since, void *out, DWORD *out_len);

/* list the broker's thumbnails; *out_len must hold the buffer size on entry; returns the reply (or NULL) */
static struct dwm_thumb_list_reply *list_thumbs( unsigned char *buf, DWORD *out_len, struct dwm_thumb **thumbs,
                                                 UINT64 since )
{
    HRESULT hr;

    hr = pWexDwmpThumbList( since, buf, out_len );
    if (hr != S_OK) return NULL;
    *thumbs = (struct dwm_thumb *)(buf + sizeof(struct dwm_thumb_list_reply));
    return (struct dwm_thumb_list_reply *)buf;
}

/* find our thumbnail (matched by dest and src); returns its index or -1 */
static int find_thumb( struct dwm_thumb_list_reply *rep, struct dwm_thumb *thumbs, HWND dest, HWND src )
{
    uint32_t d = (uint32_t)HandleToUlong( dest ), s = (uint32_t)HandleToUlong( src );
    int i;

    for (i = 0; i < (int)rep->count; i++)
        if (thumbs[i].dest == d && thumbs[i].src == s) return i;
    return -1;
}

START_TEST(dwm_thumb)
{
    HMODULE dwmapi;
    static unsigned char buf[sizeof(struct dwm_thumb_list_reply) + DWM_THUMB_MAX * sizeof(struct dwm_thumb)];
    struct dwm_thumb_list_reply *rep;
    struct dwm_thumb *thumbs;
    DWORD out_len;
    UINT64 revision = 0;
    HRESULT hr;
    HWND dest, src;
    HTHUMBNAIL thumb;
    DWM_THUMBNAIL_PROPERTIES props;
    int idx, tries;

    dwmapi = GetModuleHandleW( L"dwmapi.dll" );
    if (!dwmapi) dwmapi = LoadLibraryW( L"dwmapi.dll" );
    pWexDwmpThumbList = (void *)GetProcAddress( dwmapi, "WexDwmpThumbList" );
    if (!pWexDwmpThumbList)
    {
        win_skip( "WexDwmpThumbList not available (not a Wine build).\n" );
        return;
    }

    /* no broker: the list call fails */
    out_len = sizeof(buf);
    hr = pWexDwmpThumbList( 0, buf, &out_len );
    if (hr != S_OK)
    {
        skip( "needs a desktop with a DWM broker\n" );
        return;
    }

    dest = CreateWindowExW( 0, L"static", L"dwm_thumb_dest", WS_POPUP | WS_VISIBLE,
                            0, 0, 200, 200, 0, 0, 0, NULL );
    ok( !!dest, "CreateWindowExW dest failed, error %lu\n", GetLastError() );
    src = CreateWindowExW( 0, L"static", L"dwm_thumb_src", WS_POPUP | WS_VISIBLE,
                           300, 0, 200, 200, 0, 0, 0, NULL );
    ok( !!src, "CreateWindowExW src failed, error %lu\n", GetLastError() );

    hr = DwmRegisterThumbnail( dest, src, &thumb );
    ok( hr == S_OK, "DwmRegisterThumbnail failed: %#lx\n", hr );

    memset( &props, 0, sizeof(props) );
    props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY;
    props.fVisible = TRUE;
    props.rcDestination.left   = 10;
    props.rcDestination.top    = 10;
    props.rcDestination.right  = 110;
    props.rcDestination.bottom = 90;
    props.opacity = 200;
    hr = DwmUpdateThumbnailProperties( thumb, &props );
    ok( hr == S_OK, "DwmUpdateThumbnailProperties failed: %#lx\n", hr );

    /* the broker must now list our thumbnail, with the merged properties */
    out_len = sizeof(buf);
    rep = list_thumbs( buf, &out_len, &thumbs, 0 );
    ok( rep != NULL, "WexDwmpThumbList failed\n" );
    if (rep)
    {
        idx = find_thumb( rep, thumbs, dest, src );
        ok( idx >= 0, "our thumbnail not in the list\n" );
        if (idx >= 0)
        {
            ok( thumbs[idx].flags == DWM_THUMB_VISIBLE, "flags %#x, expected %#x\n",
                thumbs[idx].flags, DWM_THUMB_VISIBLE );
            ok( thumbs[idx].opacity == 200, "opacity %u, expected 200\n", thumbs[idx].opacity );
            ok( thumbs[idx].dest_rect[0] == 10 && thumbs[idx].dest_rect[1] == 10 &&
                thumbs[idx].dest_rect[2] == 110 && thumbs[idx].dest_rect[3] == 90,
                "dest_rect {%d,%d,%d,%d}, expected {10,10,110,90}\n",
                thumbs[idx].dest_rect[0], thumbs[idx].dest_rect[1],
                thumbs[idx].dest_rect[2], thumbs[idx].dest_rect[3] );
            ok( thumbs[idx].source[0] == 0 && thumbs[idx].source[1] == 0 &&
                thumbs[idx].source[2] == 0 && thumbs[idx].source[3] == 0,
                "source rect should be zero (no DWM_TNP_RECTSOURCE)\n" );
        }
        revision = rep->revision;
    }

    /* a list at the current revision reports no changes */
    out_len = sizeof(buf);
    rep = list_thumbs( buf, &out_len, &thumbs, revision );
    ok( rep != NULL, "WexDwmpThumbList (since revision) failed\n" );
    if (rep)
    {
        ok( rep->count == 0, "expected 0 thumbnails at the current revision, got %u\n", rep->count );
        ok( rep->revision == revision, "revision changed unexpectedly (%llu vs %llu)\n",
            (unsigned long long)rep->revision, (unsigned long long)revision );
    }

    /* unregister removes it from the broker */
    hr = DwmUnregisterThumbnail( thumb );
    ok( hr == S_OK, "DwmUnregisterThumbnail failed: %#lx\n", hr );
    out_len = sizeof(buf);
    rep = list_thumbs( buf, &out_len, &thumbs, 0 );
    ok( rep != NULL, "WexDwmpThumbList after unregister failed\n" );
    if (rep)
        ok( find_thumb( rep, thumbs, dest, src ) < 0, "thumbnail still present after unregister\n" );

    /* destroying the source window removes it once the broker sweeps */
    hr = DwmRegisterThumbnail( dest, src, &thumb );
    ok( hr == S_OK, "DwmRegisterThumbnail (2nd) failed: %#lx\n", hr );
    hr = DwmUpdateThumbnailProperties( thumb, &props );
    ok( hr == S_OK, "DwmUpdateThumbnailProperties (2nd) failed: %#lx\n", hr );
    DestroyWindow( src );

    idx = 0;
    for (tries = 0; tries < 28; tries++)  /* up to 7 s, polling every 250 ms */
    {
        Sleep( 250 );
        out_len = sizeof(buf);
        rep = list_thumbs( buf, &out_len, &thumbs, 0 );
        if (!rep) continue;
        idx = find_thumb( rep, thumbs, dest, src );
        if (idx < 0) break;
    }
    ok( idx < 0, "thumbnail not removed after the source window was destroyed (sweep)\n" );

    DestroyWindow( dest );
}
