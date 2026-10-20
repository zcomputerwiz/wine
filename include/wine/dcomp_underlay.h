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

#endif
