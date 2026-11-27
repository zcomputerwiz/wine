/* Private Wine composition / GDI coordination. Not a security boundary.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef __WINE_DCOMP_UNDERLAY_H
#define __WINE_DCOMP_UNDERLAY_H

static const WCHAR wine_dcomp_underlay_topW[] =
        {'_','_','w','i','n','e','_','d','c','o','m','p','_','u','n','d','e','r','l','a','y','_','t','o','p',0};
static const WCHAR wine_dcomp_underlay_bottomW[] =
        {'_','_','w','i','n','e','_','d','c','o','m','p','_','u','n','d','e','r','l','a','y','_','b','o','t','t','o','m',0};
/* Window-owned surface policy survives target recreation and dies with HWND. */
static const WCHAR wine_dcomp_underlayW[] =
        {'_','_','w','i','n','e','_','d','c','o','m','p','_','u','n','d','e','r','l','a','y',0};
#define WINE_DCOMP_UNDERLAY_TOP_PROP wine_dcomp_underlay_topW
#define WINE_DCOMP_UNDERLAY_BOTTOM_PROP wine_dcomp_underlay_bottomW
#define WINE_DCOMP_UNDERLAY_PROP wine_dcomp_underlayW
static const WCHAR wine_dcomp_repaint_sinkW[] =
        {'_','_','w','i','n','e','_','d','c','o','m','p','_','r','e','p','a','i','n','t','_','s','i','n','k',0};
#define WINE_DCOMP_REPAINT_SINK_PROP wine_dcomp_repaint_sinkW
#define WM_WINE_DCOMP_REPAINT (WM_USER + 0x103)

/* A system backdrop (DWMWA_SYSTEMBACKDROP_TYPE, DWMWA_MICA_EFFECT) in a window's extended frame where nothing composes
 * alpha: dwmapi publishes the backdrop colour (0x01000000 | COLORREF) and the app's margins (each + 0x10000, -1 for a
 * sheet), and win32u's window surface flush shows the colour where GDI left black inside them, which is what DWM shows
 * through GDI's zero alpha. */
static const WCHAR wine_dwm_backdropW[] =
        {'_','_','w','i','n','e','_','d','w','m','_','b','a','c','k','d','r','o','p',0};
static const WCHAR wine_dwm_backdrop_leftW[] =
        {'_','_','w','i','n','e','_','d','w','m','_','b','a','c','k','d','r','o','p','_','l','e','f','t',0};
static const WCHAR wine_dwm_backdrop_rightW[] =
        {'_','_','w','i','n','e','_','d','w','m','_','b','a','c','k','d','r','o','p','_','r','i','g','h','t',0};
static const WCHAR wine_dwm_backdrop_topW[] =
        {'_','_','w','i','n','e','_','d','w','m','_','b','a','c','k','d','r','o','p','_','t','o','p',0};
static const WCHAR wine_dwm_backdrop_bottomW[] =
        {'_','_','w','i','n','e','_','d','w','m','_','b','a','c','k','d','r','o','p','_','b','o','t','t','o','m',0};
#define WINE_DWM_BACKDROP_PROP wine_dwm_backdropW
#define WINE_DWM_BACKDROP_LEFT_PROP wine_dwm_backdrop_leftW
#define WINE_DWM_BACKDROP_RIGHT_PROP wine_dwm_backdrop_rightW
#define WINE_DWM_BACKDROP_TOP_PROP wine_dwm_backdrop_topW
#define WINE_DWM_BACKDROP_BOTTOM_PROP wine_dwm_backdrop_bottomW

#endif
