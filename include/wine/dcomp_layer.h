/*
 * Copyright 2026 WineExplorer contributors
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

#ifndef __WINE_DCOMP_LAYER_H
#define __WINE_DCOMP_LAYER_H

#define WM_WINE_DCOMP_SET_TARGET      (WM_USER + 0x100)
#define WM_WINE_DCOMP_SET_CHILD_MODE  (WM_USER + 0x101)
#define WM_WINE_DCOMP_PRESENT_FLUSH   (WM_USER + 0x102)

#define WINE_DCOMP_WND_PROP_FMT       L"__wine_dcomp_wnd_%lx_%Ix"

struct wine_dcomp_layer_params
{
    HWND target_hwnd;
    int offset_x;
    int offset_y;
    int width;
    int height;
    BOOL has_clip;
    RECT clip_rect;
    float opacity;
};

#endif /* __WINE_DCOMP_LAYER_H */
