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

/* WM_WINE_DCOMP_SET_TARGET wparam values.
 *   0: lparam is the target HWND, no placement (legacy, whole-window present).
 *   1: lparam points to a wine_dcomp_layer_params that the sender keeps alive
 *      for the duration of a synchronous SendMessage (same-thread delivery).
 *   2: lparam points to a wine_dcomp_layer_params allocated from the process
 *      heap (HeapAlloc) which the receiver frees; used with PostMessage when the
 *      swapchain window belongs to another thread, so a commit never blocks on,
 *      or dangles a pointer into, a thread that is not pumping messages. */
#define WINE_DCOMP_SET_TARGET_LEGACY          0
#define WINE_DCOMP_SET_TARGET_PARAMS          1
#define WINE_DCOMP_SET_TARGET_PARAMS_OWNED    2

/* Geometry is in the target window's client coordinates.
 * (offset_x, offset_y, width, height) is the *unclipped* destination rectangle
 * of the whole back buffer after the visual tree's offsets and scale.
 * clip_rect (when has_clip) is already in the same space; the receiver derives
 * the visible destination and the matching source rectangle from the two.
 * opacity is carried for completeness but is always 1.0 today: the window
 * delivery path has no per-layer alpha blending, so non-unity opacity is
 * rejected by dcomp before it can reach this message. */
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

#define WINE_DCOMP_TARGET_SCENE_PROP  L"__wine_dcomp_target_scene"
#define WINE_DCOMP_MAX_LAYERS         32

struct wine_dcomp_clip_rect { float left, top, right, bottom; };

struct wine_dcomp_layer_desc
{
    IUnknown *content;
    float offset_x;
    float offset_y;
    int width;
    int height;
    BOOL has_transform;
    float m11, m12;
    float m21, m22;
    float m31, m32;
    BOOL has_clip;
    struct wine_dcomp_clip_rect clip_rect;
    float radius_tl_x, radius_tl_y;
    float radius_tr_x, radius_tr_y;
    float radius_bl_x, radius_bl_y;
    float radius_br_x, radius_br_y;
    float opacity;
    int opacity_mode;
    BOOL ignore_alpha;
};

struct wine_dcomp_target_scene
{
    HWND target_hwnd;
    unsigned int layer_count;
    struct wine_dcomp_layer_desc layers[WINE_DCOMP_MAX_LAYERS];
};

#endif /* __WINE_DCOMP_LAYER_H */
