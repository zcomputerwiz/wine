/*
 * DirectComposition device implementation
 *
 * Copyright 2020 Nikolay Sivov for CodeWeavers
 * Copyright 2026 Giang Nguyen
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

#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

#define INITGUID
#define COBJMACROS
#include "initguid.h"
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "objidl.h"
#include "dxgi.h"
struct wined3d_resource;
struct wined3d_texture;
struct wined3d_device_parent;
#include "wine/winedxgi.h"
#include "dcomp.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/dcomp_layer.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

struct dcomp_device;
struct dcomp_target;
struct dcomp_visual;

static void dcomp_visual_unbind_tree(struct dcomp_visual *visual);
static const struct IDCompositionVisual2Vtbl dcomp_visual_vtbl;

struct dcomp_device;
struct dcomp_target;
struct dcomp_visual;

struct dcomp_effect_group
{
    IDCompositionEffectGroup IDCompositionEffectGroup_iface;
    LONG refcount;
    struct dcomp_device *device;
    float opacity;
    IDCompositionTransform3D *transform_3d;
};

struct dcomp_rectangle_clip
{
    IDCompositionRectangleClip IDCompositionRectangleClip_iface;
    LONG refcount;
    struct dcomp_device *device;
    float left, top, right, bottom;
    float top_left_radius_x, top_left_radius_y;
    float top_right_radius_x, top_right_radius_y;
    float bottom_left_radius_x, bottom_left_radius_y;
    float bottom_right_radius_x, bottom_right_radius_y;
};

struct dcomp_visual_state
{
    float offset_x;
    float offset_y;
    BOOL has_clip;
    D2D_RECT_F clip;
    IDCompositionClip *clip_obj;
    IUnknown *content;
    IDCompositionEffect *effect;
    float opacity;
    enum DCOMPOSITION_OPACITY_MODE opacity_mode;
    BOOL has_transform;
    D2D_MATRIX_3X2_F transform;
    float radius_tl_x, radius_tl_y;
    float radius_tr_x, radius_tr_y;
    float radius_bl_x, radius_bl_y;
    float radius_br_x, radius_br_y;
};

struct dcomp_visual
{
    IDCompositionVisual2 IDCompositionVisual2_iface;
    LONG refcount;
    struct dcomp_device *device;

    /* Pending state (modified by setters, staged for next Commit) */
    struct dcomp_visual_state pending;
    struct dcomp_visual *pending_parent;
    struct dcomp_visual *pending_children;
    struct dcomp_visual *pending_next_sibling;

    /* Active state (applied atomically on Commit) */
    struct dcomp_visual_state active;
    struct dcomp_visual *active_parent;
    struct dcomp_visual *active_children;
    struct dcomp_visual *active_next_sibling;
    HWND last_comp_wnd;
};

struct dcomp_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    LONG refcount;
    struct dcomp_device *device;
    struct list entry;
    HWND hwnd;
    BOOL topmost;
    struct dcomp_visual *pending_root;
    struct dcomp_visual *active_root;
};

struct dcomp_device
{
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDevice2 IDCompositionDevice2_iface;
    IDCompositionDevice3 IDCompositionDevice3_iface;
    IDCompositionDesktopDevice IDCompositionDesktopDevice_iface;
    LONG refcount;
    struct list targets;
    CRITICAL_SECTION cs;
};

static inline struct dcomp_device *impl_from_IDCompositionDevice(IDCompositionDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDevice2(IDCompositionDevice2 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice2_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDevice3(IDCompositionDevice3 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice3_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDesktopDevice(IDCompositionDesktopDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDesktopDevice_iface);
}

static inline struct dcomp_target *impl_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_target, IDCompositionTarget_iface);
}

static inline struct dcomp_visual *impl_from_IDCompositionVisual(IDCompositionVisual *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_visual, IDCompositionVisual2_iface);
}

static inline struct dcomp_visual *impl_from_IDCompositionVisual2(IDCompositionVisual2 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_visual, IDCompositionVisual2_iface);
}

static inline struct dcomp_effect_group *impl_from_IDCompositionEffectGroup(IDCompositionEffectGroup *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_effect_group, IDCompositionEffectGroup_iface);
}

static inline struct dcomp_rectangle_clip *impl_from_IDCompositionRectangleClip(IDCompositionRectangleClip *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_rectangle_clip, IDCompositionRectangleClip_iface);
}

/* =====================================================================
 * IDCompositionEffectGroup
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_QueryInterface(IDCompositionEffectGroup *iface,
        REFIID iid, void **out)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);

    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionEffect) ||
        IsEqualGUID(iid, &IID_IDCompositionEffectGroup))
    {
        *out = &group->IDCompositionEffectGroup_iface;
        IDCompositionEffectGroup_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_effect_group_AddRef(IDCompositionEffectGroup *iface)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    return InterlockedIncrement(&group->refcount);
}

static ULONG STDMETHODCALLTYPE dcomp_effect_group_Release(IDCompositionEffectGroup *iface)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    ULONG refcount = InterlockedDecrement(&group->refcount);
    if (!refcount)
    {
        if (group->transform_3d)
            IDCompositionTransform3D_Release(group->transform_3d);
        IDCompositionDevice_Release(&group->device->IDCompositionDevice_iface);
        free(group);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetOpacityAnimation(IDCompositionEffectGroup *iface,
        IDCompositionAnimation *animation)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetOpacity(IDCompositionEffectGroup *iface, float opacity)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    if (!isfinite(opacity) || opacity < 0.0f || opacity > 1.0f) return E_INVALIDARG;
    EnterCriticalSection(&group->device->cs);
    group->opacity = opacity;
    LeaveCriticalSection(&group->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetTransform3D(IDCompositionEffectGroup *iface,
        IDCompositionTransform3D *transform)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    if (transform)
        return E_NOTIMPL;
    EnterCriticalSection(&group->device->cs);
    if (group->transform_3d)
        IDCompositionTransform3D_Release(group->transform_3d);
    group->transform_3d = transform;
    LeaveCriticalSection(&group->device->cs);
    return S_OK;
}

static const struct IDCompositionEffectGroupVtbl dcomp_effect_group_vtbl =
{
    dcomp_effect_group_QueryInterface,
    dcomp_effect_group_AddRef,
    dcomp_effect_group_Release,
    dcomp_effect_group_SetOpacityAnimation,
    dcomp_effect_group_SetOpacity,
    dcomp_effect_group_SetTransform3D,
};

static HRESULT dcomp_effect_group_create(struct dcomp_device *device, IDCompositionEffectGroup **out)
{
    struct dcomp_effect_group *group;
    if (!out) return E_POINTER;
    if (!(group = calloc(1, sizeof(*group)))) return E_OUTOFMEMORY;
    group->IDCompositionEffectGroup_iface.lpVtbl = &dcomp_effect_group_vtbl;
    group->refcount = 1;
    group->device = device;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    group->opacity = 1.0f;
    *out = &group->IDCompositionEffectGroup_iface;
    return S_OK;
}

/* =====================================================================
 * IDCompositionRectangleClip
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_QueryInterface(IDCompositionRectangleClip *iface,
        REFIID iid, void **out)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);

    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionClip) ||
        IsEqualGUID(iid, &IID_IDCompositionRectangleClip))
    {
        *out = &clip->IDCompositionRectangleClip_iface;
        IDCompositionRectangleClip_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_rectangle_clip_AddRef(IDCompositionRectangleClip *iface)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return InterlockedIncrement(&clip->refcount);
}

static ULONG STDMETHODCALLTYPE dcomp_rectangle_clip_Release(IDCompositionRectangleClip *iface)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    ULONG refcount = InterlockedDecrement(&clip->refcount);
    if (!refcount)
    {
        IDCompositionDevice_Release(&clip->device->IDCompositionDevice_iface);
        free(clip);
    }
    return refcount;
}

static HRESULT dcomp_rectangle_clip_set_value(struct dcomp_rectangle_clip *clip,
        float *field, float value, BOOL radius)
{
    if (!isfinite(value) || (radius ? value < 0.0f : fabsf(value) > 1048576)) return E_INVALIDARG;
    EnterCriticalSection(&clip->device->cs);
    *field = value;
    LeaveCriticalSection(&clip->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetLeftAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetLeft(IDCompositionRectangleClip *iface, float left)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->left, left, FALSE);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTop(IDCompositionRectangleClip *iface, float top)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top, top, FALSE);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetRightAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetRight(IDCompositionRectangleClip *iface, float right)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->right, right, FALSE);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottom(IDCompositionRectangleClip *iface, float bottom)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom, bottom, FALSE);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_left_radius_x, r, TRUE);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_left_radius_y, r, TRUE);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_right_radius_x, r, TRUE);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_right_radius_y, r, TRUE);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_left_radius_x, r, TRUE);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_left_radius_y, r, TRUE);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_right_radius_x, r, TRUE);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_right_radius_y, r, TRUE);
}

static const struct IDCompositionRectangleClipVtbl dcomp_rectangle_clip_vtbl =
{
    dcomp_rectangle_clip_QueryInterface,
    dcomp_rectangle_clip_AddRef,
    dcomp_rectangle_clip_Release,
    dcomp_rectangle_clip_SetLeftAnimation,
    dcomp_rectangle_clip_SetLeft,
    dcomp_rectangle_clip_SetTopAnimation,
    dcomp_rectangle_clip_SetTop,
    dcomp_rectangle_clip_SetRightAnimation,
    dcomp_rectangle_clip_SetRight,
    dcomp_rectangle_clip_SetBottomAnimation,
    dcomp_rectangle_clip_SetBottom,
    dcomp_rectangle_clip_SetTopLeftRadiusXAnimation,
    dcomp_rectangle_clip_SetTopLeftRadiusX,
    dcomp_rectangle_clip_SetTopLeftRadiusYAnimation,
    dcomp_rectangle_clip_SetTopLeftRadiusY,
    dcomp_rectangle_clip_SetTopRightRadiusXAnimation,
    dcomp_rectangle_clip_SetTopRightRadiusX,
    dcomp_rectangle_clip_SetTopRightRadiusYAnimation,
    dcomp_rectangle_clip_SetTopRightRadiusY,
    dcomp_rectangle_clip_SetBottomLeftRadiusXAnimation,
    dcomp_rectangle_clip_SetBottomLeftRadiusX,
    dcomp_rectangle_clip_SetBottomLeftRadiusYAnimation,
    dcomp_rectangle_clip_SetBottomLeftRadiusY,
    dcomp_rectangle_clip_SetBottomRightRadiusXAnimation,
    dcomp_rectangle_clip_SetBottomRightRadiusX,
    dcomp_rectangle_clip_SetBottomRightRadiusYAnimation,
    dcomp_rectangle_clip_SetBottomRightRadiusY,
};

static HRESULT dcomp_rectangle_clip_create(struct dcomp_device *device, IDCompositionRectangleClip **out)
{
    struct dcomp_rectangle_clip *clip;
    if (!out) return E_POINTER;
    if (!(clip = calloc(1, sizeof(*clip)))) return E_OUTOFMEMORY;
    clip->IDCompositionRectangleClip_iface.lpVtbl = &dcomp_rectangle_clip_vtbl;
    clip->refcount = 1;
    clip->device = device;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    clip->left = clip->top = -FLT_MAX;
    clip->right = clip->bottom = FLT_MAX;
    *out = &clip->IDCompositionRectangleClip_iface;
    return S_OK;
}

/* =====================================================================
 * IDCompositionVisual & IDCompositionVisual2
 * ===================================================================== */

static void dcomp_visual_state_cleanup(struct dcomp_visual_state *state)
{
    if (state->content)
    {
        IUnknown_Release(state->content);
        state->content = NULL;
    }
    if (state->effect)
    {
        IDCompositionEffect_Release(state->effect);
        state->effect = NULL;
    }
    if (state->clip_obj)
    {
        IDCompositionClip_Release(state->clip_obj);
        state->clip_obj = NULL;
    }
}

static void dcomp_visual_state_copy(struct dcomp_visual_state *dst, const struct dcomp_visual_state *src)
{
    dcomp_visual_state_cleanup(dst);
    *dst = *src;
    if (dst->content)
        IUnknown_AddRef(dst->content);
    if (dst->effect)
        IDCompositionEffect_AddRef(dst->effect);
    if (dst->clip_obj)
        IDCompositionClip_AddRef(dst->clip_obj);
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_QueryInterface(IDCompositionVisual2 *iface,
        REFIID iid, void **out)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionVisual) ||
        IsEqualGUID(iid, &IID_IDCompositionVisual2))
    {
        *out = &visual->IDCompositionVisual2_iface;
        IDCompositionVisual2_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_visual_AddRef(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG refcount = InterlockedIncrement(&visual->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_visual_Release(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG refcount = InterlockedDecrement(&visual->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        struct dcomp_device *device = visual->device;
        EnterCriticalSection(&device->cs);
        if (visual->last_comp_wnd && IsWindow(visual->last_comp_wnd))
        {
            SendMessageW(visual->last_comp_wnd, WM_WINE_DCOMP_SET_TARGET, WINE_DCOMP_SET_TARGET_LEGACY, 0);
            visual->last_comp_wnd = NULL;
        }

        dcomp_visual_state_cleanup(&visual->pending);
        dcomp_visual_state_cleanup(&visual->active);

        while (visual->pending_children)
        {
            struct dcomp_visual *child = visual->pending_children;
            visual->pending_children = child->pending_next_sibling;
            child->pending_parent = NULL;
            child->pending_next_sibling = NULL;
            IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
        }

        while (visual->active_children)
        {
            struct dcomp_visual *child = visual->active_children;
            visual->active_children = child->active_next_sibling;
            child->active_parent = NULL;
            child->active_next_sibling = NULL;
            IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
        }

        LeaveCriticalSection(&device->cs);
        free(visual);
        IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetXAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    TRACE("iface %p, animation %p.\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetX_locked(IDCompositionVisual2 *iface, float offset_x)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, offset_x %f.\n", iface, offset_x);
    if (!isfinite(offset_x) || fabsf(offset_x) > 1048576) return E_INVALIDARG;
    visual->pending.offset_x = offset_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetX(IDCompositionVisual2 *iface, float offset_x)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetOffsetX_locked(iface, offset_x);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetYAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    TRACE("iface %p, animation %p.\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetY_locked(IDCompositionVisual2 *iface, float offset_y)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, offset_y %f.\n", iface, offset_y);
    if (!isfinite(offset_y) || fabsf(offset_y) > 1048576) return E_INVALIDARG;
    visual->pending.offset_y = offset_y;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetY(IDCompositionVisual2 *iface, float offset_y)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetOffsetY_locked(iface, offset_y);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransformObject(IDCompositionVisual2 *iface,
        IDCompositionTransform *transform)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    if (transform) return E_NOTIMPL;
    EnterCriticalSection(&visual->device->cs);
    visual->pending.has_transform = FALSE;
    LeaveCriticalSection(&visual->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransform_locked(IDCompositionVisual2 *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, matrix %p.\n", iface, matrix);
    if (matrix)
    {
        if (!isfinite(matrix->_11) || !isfinite(matrix->_12) || !isfinite(matrix->_21)
                || !isfinite(matrix->_22) || !isfinite(matrix->_31) || !isfinite(matrix->_32))
            return E_INVALIDARG;
        if (fabsf(matrix->_11) > 64.0f || fabsf(matrix->_12) > 64.0f || fabsf(matrix->_21) > 64.0f
                || fabsf(matrix->_22) > 64.0f || fabsf(matrix->_31) > 1048576.0f
                || fabsf(matrix->_32) > 1048576.0f)
            return E_INVALIDARG;
        visual->pending.has_transform = TRUE;
        visual->pending.transform = *matrix;
    }
    else
    {
        visual->pending.has_transform = FALSE;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransform(IDCompositionVisual2 *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetTransform_locked(iface, matrix);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransformParent(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    TRACE("iface %p, visual %p.\n", iface, visual);
    return visual ? E_NOTIMPL : S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetEffect_locked(IDCompositionVisual2 *iface,
        IDCompositionEffect *effect)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, effect %p.\n", iface, effect);

    if (effect)
    {
        IDCompositionEffectGroup *group = NULL;
        if (FAILED(IDCompositionEffect_QueryInterface(effect, &IID_IDCompositionEffectGroup, (void **)&group)))
            return E_NOTIMPL;
        if (group->lpVtbl != &dcomp_effect_group_vtbl)
        {
            IDCompositionEffectGroup_Release(group);
            return E_NOTIMPL;
        }
        if (impl_from_IDCompositionEffectGroup(group)->device != visual->device)
        {
            IDCompositionEffectGroup_Release(group);
            return E_INVALIDARG;
        }
        IDCompositionEffectGroup_Release(group);
        IDCompositionEffect_AddRef(effect);
        if (SUCCEEDED(IDCompositionEffect_QueryInterface(effect, &IID_IDCompositionEffectGroup, (void **)&group)))
        {
            struct dcomp_effect_group *grp = impl_from_IDCompositionEffectGroup(group);
            visual->pending.opacity = grp->opacity;
            IDCompositionEffectGroup_Release(group);
        }
    }
    else
    {
        visual->pending.opacity = 1.0f;
    }

    if (visual->pending.effect)
        IDCompositionEffect_Release(visual->pending.effect);
    visual->pending.effect = effect;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetEffect(IDCompositionVisual2 *iface,
        IDCompositionEffect *effect)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetEffect_locked(iface, effect);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetBitmapInterpolationMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode)
{
    TRACE("iface %p, interpolation_mode %d.\n", iface, interpolation_mode);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetBorderMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BORDER_MODE border_mode)
{
    TRACE("iface %p, border_mode %d.\n", iface, border_mode);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClipObject_locked(IDCompositionVisual2 *iface,
        IDCompositionClip *clip)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, clip %p.\n", iface, clip);

    if (clip)
    {
        IDCompositionRectangleClip *rclip = NULL;
        if (FAILED(IDCompositionClip_QueryInterface(clip, &IID_IDCompositionRectangleClip, (void **)&rclip)))
            return E_NOTIMPL;
        if (rclip->lpVtbl != &dcomp_rectangle_clip_vtbl)
        {
            IDCompositionRectangleClip_Release(rclip);
            return E_NOTIMPL;
        }
        if (impl_from_IDCompositionRectangleClip(rclip)->device != visual->device)
        {
            IDCompositionRectangleClip_Release(rclip);
            return E_INVALIDARG;
        }
        IDCompositionRectangleClip_Release(rclip);
        IDCompositionClip_AddRef(clip);
        if (SUCCEEDED(IDCompositionClip_QueryInterface(clip, &IID_IDCompositionRectangleClip, (void **)&rclip)))
        {
            struct dcomp_rectangle_clip *rc = impl_from_IDCompositionRectangleClip(rclip);
            visual->pending.has_clip = TRUE;
            visual->pending.clip.left = rc->left;
            visual->pending.clip.top = rc->top;
            visual->pending.clip.right = rc->right;
            visual->pending.clip.bottom = rc->bottom;
            visual->pending.radius_tl_x = rc->top_left_radius_x;
            visual->pending.radius_tl_y = rc->top_left_radius_y;
            visual->pending.radius_tr_x = rc->top_right_radius_x;
            visual->pending.radius_tr_y = rc->top_right_radius_y;
            visual->pending.radius_bl_x = rc->bottom_left_radius_x;
            visual->pending.radius_bl_y = rc->bottom_left_radius_y;
            visual->pending.radius_br_x = rc->bottom_right_radius_x;
            visual->pending.radius_br_y = rc->bottom_right_radius_y;
            IDCompositionRectangleClip_Release(rclip);
        }
    }
    else
    {
        visual->pending.has_clip = FALSE;
        visual->pending.radius_tl_x = visual->pending.radius_tl_y = 0.0f;
        visual->pending.radius_tr_x = visual->pending.radius_tr_y = 0.0f;
        visual->pending.radius_bl_x = visual->pending.radius_bl_y = 0.0f;
        visual->pending.radius_br_x = visual->pending.radius_br_y = 0.0f;
    }

    if (visual->pending.clip_obj)
        IDCompositionClip_Release(visual->pending.clip_obj);
    visual->pending.clip_obj = clip;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClipObject(IDCompositionVisual2 *iface,
        IDCompositionClip *clip)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetClipObject_locked(iface, clip);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClip_locked(IDCompositionVisual2 *iface,
        const D2D_RECT_F *rect)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, rect %p.\n", iface, rect);

    if (rect && (!isfinite(rect->left) || !isfinite(rect->top) || !isfinite(rect->right)
            || !isfinite(rect->bottom) || fabsf(rect->left) > 1048576
            || fabsf(rect->top) > 1048576 || fabsf(rect->right) > 1048576
            || fabsf(rect->bottom) > 1048576)) return E_INVALIDARG;
    if (visual->pending.clip_obj)
    {
        IDCompositionClip_Release(visual->pending.clip_obj);
        visual->pending.clip_obj = NULL;
    }

    visual->pending.radius_tl_x = visual->pending.radius_tl_y = 0.0f;
    visual->pending.radius_tr_x = visual->pending.radius_tr_y = 0.0f;
    visual->pending.radius_bl_x = visual->pending.radius_bl_y = 0.0f;
    visual->pending.radius_br_x = visual->pending.radius_br_y = 0.0f;

    if (rect)
    {
        visual->pending.has_clip = TRUE;
        visual->pending.clip = *rect;
    }
    else
    {
        visual->pending.has_clip = FALSE;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClip(IDCompositionVisual2 *iface,
        const D2D_RECT_F *rect)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetClip_locked(iface, rect);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetContent_locked(IDCompositionVisual2 *iface,
        IUnknown *content)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, content %p.\n", iface, content);

    if (content)
        IUnknown_AddRef(content);
    if (visual->pending.content)
        IUnknown_Release(visual->pending.content);

    visual->pending.content = content;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetContent(IDCompositionVisual2 *iface,
        IUnknown *content)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetContent_locked(iface, content);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_AddVisual_locked(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual, BOOL insert_above, IDCompositionVisual *reference_visual)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct dcomp_visual *child, *ref = NULL, *cur, **p;

    TRACE("iface %p, visual %p, insert_above %d, reference_visual %p.\n",
            iface, visual, insert_above, reference_visual);

    if (!visual || visual == (IDCompositionVisual *)iface)
        return E_INVALIDARG;

    if ((const void *)visual->lpVtbl != &dcomp_visual_vtbl) return E_NOTIMPL;

    child = impl_from_IDCompositionVisual(visual);
    if (child->device != parent->device) return E_INVALIDARG;

    /* Cycle detection: parent cannot be child or any descendant of child */
    for (cur = parent; cur; cur = cur->pending_parent)
    {
        if (cur == child)
            return E_INVALIDARG;
    }

    if (reference_visual)
    {
        if ((const void *)reference_visual->lpVtbl != &dcomp_visual_vtbl) return E_NOTIMPL;
        if (reference_visual == visual)
            return E_INVALIDARG;
        ref = impl_from_IDCompositionVisual(reference_visual);
        p = &parent->pending_children;
        while (*p && *p != ref)
            p = &(*p)->pending_next_sibling;
        if (!*p)
            return E_INVALIDARG;
    }

    /* Keep the child alive while its old parent drops its owning reference. */
    IDCompositionVisual2_AddRef(&child->IDCompositionVisual2_iface);
    /* If child is already in a parent's pending list, reparent it */
    if (child->pending_parent)
    {
        struct dcomp_visual *old_parent = child->pending_parent;
        p = &old_parent->pending_children;
        while (*p && *p != child)
            p = &(*p)->pending_next_sibling;
        if (*p)
        {
            *p = child->pending_next_sibling;
            child->pending_parent = NULL;
            child->pending_next_sibling = NULL;
            IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
        }
    }

    child->pending_parent = parent;

    if (!ref)
    {
        if (insert_above)
        {
            child->pending_next_sibling = parent->pending_children;
            parent->pending_children = child;
        }
        else
        {
            p = &parent->pending_children;
            while (*p)
                p = &(*p)->pending_next_sibling;
            *p = child;
            child->pending_next_sibling = NULL;
        }
    }
    else
    {
        p = &parent->pending_children;
        while (*p && *p != ref)
            p = &(*p)->pending_next_sibling;

        if (insert_above)
        {
            child->pending_next_sibling = *p;
            *p = child;
        }
        else
        {
            child->pending_next_sibling = (*p)->pending_next_sibling;
            (*p)->pending_next_sibling = child;
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_AddVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual, BOOL insert_above, IDCompositionVisual *reference_visual)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_AddVisual_locked(iface, visual, insert_above, reference_visual);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveVisual_locked(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct dcomp_visual *child, **p;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_INVALIDARG;

    child = impl_from_IDCompositionVisual(visual);
    p = &parent->pending_children;
    while (*p && *p != child)
        p = &(*p)->pending_next_sibling;

    if (*p)
    {
        *p = child->pending_next_sibling;
        child->pending_parent = NULL;
        child->pending_next_sibling = NULL;
        IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
        return S_OK;
    }

    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_RemoveVisual_locked(iface, visual);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveAllVisuals_locked(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p.\n", iface);

    while (parent->pending_children)
    {
        struct dcomp_visual *child = parent->pending_children;
        parent->pending_children = child->pending_next_sibling;
        child->pending_parent = NULL;
        child->pending_next_sibling = NULL;
        IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveAllVisuals(IDCompositionVisual2 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_RemoveAllVisuals_locked(iface);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetCompositeMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_COMPOSITE_MODE composite_mode)
{
    TRACE("iface %p, composite_mode %d.\n", iface, composite_mode);
    if (composite_mode != DCOMPOSITION_COMPOSITE_MODE_SOURCE_OVER) return E_NOTIMPL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOpacityMode_locked(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, opacity_mode %d.\n", iface, opacity_mode);

    if (opacity_mode != DCOMPOSITION_OPACITY_MODE_LAYER &&
        opacity_mode != DCOMPOSITION_OPACITY_MODE_MULTIPLY &&
        opacity_mode != DCOMPOSITION_OPACITY_MODE_INHERIT)
    {
        WARN("Invalid opacity mode %d.\n", opacity_mode);
        return E_INVALIDARG;
    }

    visual->pending.opacity_mode = opacity_mode;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOpacityMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    struct dcomp_device *device = impl_from_IDCompositionVisual2(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_visual_SetOpacityMode_locked(iface, opacity_mode);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetBackFaceVisibility(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BACKFACE_VISIBILITY visibility)
{
    TRACE("iface %p, visibility %d.\n", iface, visibility);
    return S_OK;
}

static const struct IDCompositionVisual2Vtbl dcomp_visual_vtbl =
{
    dcomp_visual_QueryInterface,
    dcomp_visual_AddRef,
    dcomp_visual_Release,
    dcomp_visual_SetOffsetXAnimation,
    dcomp_visual_SetOffsetX,
    dcomp_visual_SetOffsetYAnimation,
    dcomp_visual_SetOffsetY,
    dcomp_visual_SetTransformObject,
    dcomp_visual_SetTransform,
    dcomp_visual_SetTransformParent,
    dcomp_visual_SetEffect,
    dcomp_visual_SetBitmapInterpolationMode,
    dcomp_visual_SetBorderMode,
    dcomp_visual_SetClipObject,
    dcomp_visual_SetClip,
    dcomp_visual_SetContent,
    dcomp_visual_AddVisual,
    dcomp_visual_RemoveVisual,
    dcomp_visual_RemoveAllVisuals,
    dcomp_visual_SetCompositeMode,
    dcomp_visual_SetOpacityMode,
    dcomp_visual_SetBackFaceVisibility,
};

static HRESULT dcomp_visual_create(struct dcomp_device *device, struct dcomp_visual **out)
{
    struct dcomp_visual *visual;

    if (!(visual = calloc(1, sizeof(*visual))))
        return E_OUTOFMEMORY;

    visual->IDCompositionVisual2_iface.lpVtbl = &dcomp_visual_vtbl;
    visual->refcount = 1;
    visual->device = device;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    visual->pending.opacity = 1.0f;
    visual->active.opacity = 1.0f;
    visual->pending.opacity_mode = DCOMPOSITION_OPACITY_MODE_INHERIT;
    visual->active.opacity_mode = DCOMPOSITION_OPACITY_MODE_INHERIT;

    *out = visual;
    return S_OK;
}

/* =====================================================================
 * IDCompositionTarget
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_target_QueryInterface(IDCompositionTarget *iface,
        REFIID iid, void **out)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionTarget))
    {
        *out = &target->IDCompositionTarget_iface;
        IDCompositionTarget_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_target_AddRef(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    ULONG refcount = InterlockedIncrement(&target->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_target_Release(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    ULONG refcount = InterlockedDecrement(&target->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        struct dcomp_device *device = target->device;
        EnterCriticalSection(&device->cs);
        if (target->active_root) dcomp_visual_unbind_tree(target->active_root);
        if (target->pending_root)
            IDCompositionVisual2_Release(&target->pending_root->IDCompositionVisual2_iface);
        if (target->active_root)
            IDCompositionVisual2_Release(&target->active_root->IDCompositionVisual2_iface);

        list_remove(&target->entry);
        LeaveCriticalSection(&device->cs);

        free(target);
        IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_target_SetRoot_locked(IDCompositionTarget *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    struct dcomp_visual *new_root = NULL;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (visual)
    {
        if ((const void *)visual->lpVtbl != &dcomp_visual_vtbl) return E_NOTIMPL;
        new_root = impl_from_IDCompositionVisual(visual);
        if (new_root->device != target->device) return E_INVALIDARG;
        IDCompositionVisual2_AddRef(&new_root->IDCompositionVisual2_iface);
    }

    if (target->pending_root)
        IDCompositionVisual2_Release(&target->pending_root->IDCompositionVisual2_iface);

    target->pending_root = new_root;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_target_SetRoot(IDCompositionTarget *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_device *device = impl_from_IDCompositionTarget(iface)->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    hr = dcomp_target_SetRoot_locked(iface, visual);
    LeaveCriticalSection(&device->cs);
    return hr;
}

static const struct IDCompositionTargetVtbl dcomp_target_vtbl =
{
    dcomp_target_QueryInterface,
    dcomp_target_AddRef,
    dcomp_target_Release,
    dcomp_target_SetRoot,
};

static HRESULT dcomp_target_create(struct dcomp_device *device, HWND hwnd, BOOL topmost,
        struct dcomp_target **out)
{
    struct dcomp_target *target;

    if (!IsWindow(hwnd))
        return E_INVALIDARG;

    if (!(target = calloc(1, sizeof(*target))))
        return E_OUTOFMEMORY;

    target->IDCompositionTarget_iface.lpVtbl = &dcomp_target_vtbl;
    target->refcount = 1;
    target->device = device;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    target->hwnd = hwnd;
    target->topmost = topmost;

    EnterCriticalSection(&device->cs);
    list_add_tail(&device->targets, &target->entry);
    LeaveCriticalSection(&device->cs);

    *out = target;
    return S_OK;
}

/* =====================================================================
 * IDCompositionDevice Traversal and Commit
 * ===================================================================== */

static HRESULT dcomp_bind_content(IUnknown *content, HWND hwnd, const RECT *dst, const RECT *clip)
{
    IWineDCompSwapChain *binding;
    HRESULT hr;

    if (!content) return S_OK;
    if (FAILED(hr = IUnknown_QueryInterface(content, &IID_IWineDCompSwapChain, (void **)&binding)))
        return E_NOTIMPL;
    if (!hwnd)
        IWineDCompSwapChain_set_target_scene(binding, NULL, NULL);
    hr = IWineDCompSwapChain_set_target(binding, hwnd, dst, clip);
    IWineDCompSwapChain_Release(binding);
    return hr;
}

static void dcomp_visual_unbind_tree(struct dcomp_visual *visual)
{
    struct dcomp_visual *child;

    if (!visual) return;
    dcomp_bind_content(visual->active.content, NULL, NULL, NULL);
    for (child = visual->active_children; child; child = child->active_next_sibling)
        dcomp_visual_unbind_tree(child);
}

static void dcomp_visual_apply_commit(struct dcomp_visual *visual)
{
    struct dcomp_visual *child, **p;

    if (!visual) return;

    /* Detach while the old active content still owns its COM reference. */
    if (visual->active.content != visual->pending.content)
        dcomp_bind_content(visual->active.content, NULL, NULL, NULL);
    /* Apply visual state: pending -> active */
    dcomp_visual_state_copy(&visual->active, &visual->pending);

    /* Dynamically evaluate attached clip_obj and effect */
    if (visual->active.clip_obj)
    {
        IDCompositionRectangleClip *rclip = NULL;
        if (SUCCEEDED(IDCompositionClip_QueryInterface(visual->active.clip_obj,
                &IID_IDCompositionRectangleClip, (void **)&rclip)))
        {
            struct dcomp_rectangle_clip *rc = impl_from_IDCompositionRectangleClip(rclip);
            visual->active.has_clip = TRUE;
            visual->active.clip.left = rc->left;
            visual->active.clip.top = rc->top;
            visual->active.clip.right = rc->right;
            visual->active.clip.bottom = rc->bottom;
            visual->active.radius_tl_x = rc->top_left_radius_x;
            visual->active.radius_tl_y = rc->top_left_radius_y;
            visual->active.radius_tr_x = rc->top_right_radius_x;
            visual->active.radius_tr_y = rc->top_right_radius_y;
            visual->active.radius_bl_x = rc->bottom_left_radius_x;
            visual->active.radius_bl_y = rc->bottom_left_radius_y;
            visual->active.radius_br_x = rc->bottom_right_radius_x;
            visual->active.radius_br_y = rc->bottom_right_radius_y;
            IDCompositionRectangleClip_Release(rclip);
        }
    }
    if (visual->active.effect)
    {
        IDCompositionEffectGroup *egroup = NULL;
        if (SUCCEEDED(IDCompositionEffect_QueryInterface(visual->active.effect,
                &IID_IDCompositionEffectGroup, (void **)&egroup)))
        {
            struct dcomp_effect_group *eg = impl_from_IDCompositionEffectGroup(egroup);
            visual->active.opacity = eg->opacity;
            IDCompositionEffectGroup_Release(egroup);
        }
    }

    /* Release old active children list */
    while (visual->active_children)
    {
        struct dcomp_visual *old_child = visual->active_children;
        visual->active_children = old_child->active_next_sibling;
        old_child->active_parent = NULL;
        old_child->active_next_sibling = NULL;
        if (!old_child->pending_parent)
            dcomp_visual_unbind_tree(old_child);
        IDCompositionVisual2_Release(&old_child->IDCompositionVisual2_iface);
    }

    /* Build new active children list matching pending_children */
    p = &visual->active_children;
    for (child = visual->pending_children; child; child = child->pending_next_sibling)
    {
        IDCompositionVisual2_AddRef(&child->IDCompositionVisual2_iface);
        child->active_parent = visual;
        child->active_next_sibling = NULL;
        *p = child;
        p = &child->active_next_sibling;

        dcomp_visual_apply_commit(child);
    }
}

static HRESULT dcomp_validate_pending(struct dcomp_visual *visual, unsigned int *contents,
        unsigned int *visuals, IUnknown **content_device, enum DCOMPOSITION_OPACITY_MODE parent_mode,
        const D2D_MATRIX_3X2_F *parent_mat, unsigned int depth)
{
    struct dcomp_visual *child;
    IWineDCompSwapChain *binding;
    D2D_MATRIX_3X2_F loc_mat, cur_mat;
    HRESULT hr;
    enum DCOMPOSITION_OPACITY_MODE mode;

    if (!visual) return S_OK;
    if (depth > 128 || ++*visuals > 128) return E_INVALIDARG;
    mode = visual->pending.opacity_mode == DCOMPOSITION_OPACITY_MODE_INHERIT
            ? parent_mode : visual->pending.opacity_mode;

    if (visual->pending.has_transform)
        loc_mat = visual->pending.transform;
    else
        loc_mat = (D2D_MATRIX_3X2_F){1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

    loc_mat._31 += visual->pending.offset_x;
    loc_mat._32 += visual->pending.offset_y;

    cur_mat._11 = loc_mat._11 * parent_mat->_11 + loc_mat._12 * parent_mat->_21;
    cur_mat._12 = loc_mat._11 * parent_mat->_12 + loc_mat._12 * parent_mat->_22;
    cur_mat._21 = loc_mat._21 * parent_mat->_11 + loc_mat._22 * parent_mat->_21;
    cur_mat._22 = loc_mat._21 * parent_mat->_12 + loc_mat._22 * parent_mat->_22;
    cur_mat._31 = loc_mat._31 * parent_mat->_11 + loc_mat._32 * parent_mat->_21 + parent_mat->_31;
    cur_mat._32 = loc_mat._31 * parent_mat->_12 + loc_mat._32 * parent_mat->_22 + parent_mat->_32;

    if (!isfinite(cur_mat._11) || !isfinite(cur_mat._12) ||
        !isfinite(cur_mat._21) || !isfinite(cur_mat._22) ||
        !isfinite(cur_mat._31) || !isfinite(cur_mat._32) ||
        fabsf(cur_mat._11) > 64 || fabsf(cur_mat._12) > 64 ||
        fabsf(cur_mat._21) > 64 || fabsf(cur_mat._22) > 64 ||
        fabsf(cur_mat._31) > 1048576.0f || fabsf(cur_mat._32) > 1048576.0f)
        return E_INVALIDARG;


    if (visual->pending.content)
    {
        IDXGISwapChain1 *swapchain;
        IUnknown *device;
        DXGI_SWAP_CHAIN_DESC1 desc;
        if (++*contents > WINE_DCOMP_MAX_CONTENTS) return E_NOTIMPL;
        if (FAILED(hr = IUnknown_QueryInterface(visual->pending.content,
                &IID_IWineDCompSwapChain, (void **)&binding))) return E_NOTIMPL;
        IWineDCompSwapChain_Release(binding);
        if (FAILED(IUnknown_QueryInterface(visual->pending.content, &IID_IDXGISwapChain1,
                (void **)&swapchain))) return E_NOTIMPL;
        hr = IDXGISwapChain1_GetDesc1(swapchain, &desc);
        if (SUCCEEDED(hr))
        {
            hr = IDXGISwapChain1_GetDevice(swapchain, &IID_IUnknown, (void **)&device);
            if (SUCCEEDED(hr))
            {
                if (!*content_device) *content_device = device;
                else
                {
                    if (*content_device != device) hr = E_NOTIMPL;
                    IUnknown_Release(device);
                }
            }
        }
        IDXGISwapChain1_Release(swapchain);
        if (FAILED(hr)) return hr;
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
            return E_NOTIMPL;
        if (desc.AlphaMode != DXGI_ALPHA_MODE_PREMULTIPLIED && desc.AlphaMode != DXGI_ALPHA_MODE_IGNORE)
            return E_NOTIMPL;
    }

    for (child = visual->pending_children; child; child = child->pending_next_sibling)
        if (FAILED(hr = dcomp_validate_pending(child, contents, visuals, content_device, mode, &cur_mat, depth + 1)))
            return hr;

    return S_OK;
}

struct dcomp_collect_context
{
    struct wine_dcomp_target_scene scene;
};

static void dcomp_collect_children(struct dcomp_visual *child,
        const D2D_MATRIX_3X2_F *parent, float opacity, enum DCOMPOSITION_OPACITY_MODE mode,
        struct dcomp_collect_context *ctx);

static void dcomp_collect_pending(struct dcomp_visual *visual,
        const D2D_MATRIX_3X2_F *parent, float opacity, enum DCOMPOSITION_OPACITY_MODE mode,
        struct dcomp_collect_context *ctx)
{
    struct dcomp_visual_state state = visual->pending;
    D2D_MATRIX_3X2_F local = {1,0,0,1,0,0}, matrix;
    struct wine_dcomp_layer_desc *layer;
    IDXGISwapChain1 *sc;
    DXGI_SWAP_CHAIN_DESC1 desc;

    if (state.clip_obj)
    {
        struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(
                (IDCompositionRectangleClip *)state.clip_obj);
        state.clip = (D2D_RECT_F){clip->left, clip->top, clip->right, clip->bottom};
        state.radius_tl_x = clip->top_left_radius_x; state.radius_tl_y = clip->top_left_radius_y;
        state.radius_tr_x = clip->top_right_radius_x; state.radius_tr_y = clip->top_right_radius_y;
        state.radius_bl_x = clip->bottom_left_radius_x; state.radius_bl_y = clip->bottom_left_radius_y;
        state.radius_br_x = clip->bottom_right_radius_x; state.radius_br_y = clip->bottom_right_radius_y;
    }
    if (state.effect)
        state.opacity = impl_from_IDCompositionEffectGroup((IDCompositionEffectGroup *)state.effect)->opacity;
    if (state.has_transform) local = state.transform;
    local._31 += state.offset_x;
    local._32 += state.offset_y;
    matrix._11 = local._11 * parent->_11 + local._12 * parent->_21;
    matrix._12 = local._11 * parent->_12 + local._12 * parent->_22;
    matrix._21 = local._21 * parent->_11 + local._22 * parent->_21;
    matrix._22 = local._21 * parent->_12 + local._22 * parent->_22;
    matrix._31 = local._31 * parent->_11 + local._32 * parent->_21 + parent->_31;
    matrix._32 = local._31 * parent->_12 + local._32 * parent->_22 + parent->_32;
    if (state.opacity_mode != DCOMPOSITION_OPACITY_MODE_INHERIT) mode = state.opacity_mode;

    /* Children are listed front to back; content is behind all children. */
    {
        layer = &ctx->scene.layers[ctx->scene.layer_count++];
        layer->content = state.content;
        layer->has_transform = TRUE;
        layer->m11 = matrix._11; layer->m12 = matrix._12;
        layer->m21 = matrix._21; layer->m22 = matrix._22;
        layer->m31 = matrix._31; layer->m32 = matrix._32;
        if (state.content && SUCCEEDED(IUnknown_QueryInterface(state.content, &IID_IDXGISwapChain1, (void **)&sc)))
        {
            if (SUCCEEDED(IDXGISwapChain1_GetDesc1(sc, &desc)))
            {
                layer->width = desc.Width;
                layer->height = desc.Height;
                layer->ignore_alpha = desc.AlphaMode == DXGI_ALPHA_MODE_IGNORE;
            }
            IDXGISwapChain1_Release(sc);
        }
        layer->has_clip = state.has_clip;
        layer->clip_rect = (struct wine_dcomp_clip_rect){state.clip.left, state.clip.top,
                state.clip.right, state.clip.bottom};
        layer->radius_tl_x = state.radius_tl_x; layer->radius_tl_y = state.radius_tl_y;
        layer->radius_tr_x = state.radius_tr_x; layer->radius_tr_y = state.radius_tr_y;
        layer->radius_bl_x = state.radius_bl_x; layer->radius_bl_y = state.radius_bl_y;
        layer->radius_br_x = state.radius_br_x; layer->radius_br_y = state.radius_br_y;
        layer->opacity = state.opacity;
        layer->opacity_mode = mode;
    }
    dcomp_collect_children(visual->pending_children, &matrix, opacity, mode, ctx);
    layer->subtree_end = ctx->scene.layer_count;
}

static void dcomp_collect_children(struct dcomp_visual *child,
        const D2D_MATRIX_3X2_F *parent, float opacity, enum DCOMPOSITION_OPACITY_MODE mode,
        struct dcomp_collect_context *ctx)
{
    if (!child) return;
    dcomp_collect_children(child->pending_next_sibling, parent, opacity, mode, ctx);
    dcomp_collect_pending(child, parent, opacity, mode, ctx);
}

static HRESULT dcomp_device_internal_commit(struct dcomp_device *dev)
{
    struct dcomp_target *target, *prepared_target = NULL;
    D2D_MATRIX_3X2_F ident = {1,0,0,1,0,0};
    struct dcomp_collect_context ctx = {0};
    IWineDCompSwapChain *bindings[WINE_DCOMP_MAX_LAYERS];
    unsigned int contents = 0, visuals = 0, targets = 0, count = 0, i, j;
    IUnknown *content_device = NULL;
    HRESULT hr = S_OK;

    EnterCriticalSection(&dev->cs);
    LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
    {
        if (target->pending_root && ++targets > 1) { hr = E_NOTIMPL; goto done; }
        if (FAILED(hr = dcomp_validate_pending(target->pending_root, &contents, &visuals,
                &content_device, DCOMPOSITION_OPACITY_MODE_LAYER, &ident, 0))) goto done;
        if (target->pending_root)
        {
            if (!IsWindow(target->hwnd)) { hr = E_INVALIDARG; goto done; }
            prepared_target = target;
        }
    }
    if (prepared_target)
    {
        ctx.scene.target_hwnd = prepared_target->hwnd;
        dcomp_collect_pending(prepared_target->pending_root, &ident, 1,
                DCOMPOSITION_OPACITY_MODE_LAYER, &ctx);
        /* Allocate every scene/surface before touching any active visual state.
         * Deduplicate shared content so it is prepared and published once. */
        for (i = 0; i < ctx.scene.layer_count; ++i)
        {
            IWineDCompSwapChain *binding;
            if (!ctx.scene.layers[i].content) continue;
            if (FAILED(hr = IUnknown_QueryInterface(ctx.scene.layers[i].content,
                    &IID_IWineDCompSwapChain, (void **)&binding))) goto done;
            for (j = 0; j < count && bindings[j] != binding; ++j) {}
            if (j < count) { IWineDCompSwapChain_Release(binding); continue; }
            bindings[count++] = binding;
            if (FAILED(hr = IWineDCompSwapChain_prepare_target_scene(binding,
                    ctx.scene.target_hwnd, &ctx.scene))) goto done;
        }
    }
    LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
    {
        if (target->active_root != target->pending_root)
        {
            if (target->pending_root) IDCompositionVisual2_AddRef(&target->pending_root->IDCompositionVisual2_iface);
            if (target->active_root)
            {
                dcomp_visual_unbind_tree(target->active_root);
                IDCompositionVisual2_Release(&target->active_root->IDCompositionVisual2_iface);
            }
            target->active_root = target->pending_root;
        }
        if (target->active_root) dcomp_visual_apply_commit(target->active_root);
    }
    for (i = 0; i < count; ++i)
        if (FAILED(hr = IWineDCompSwapChain_set_target_scene(bindings[i], ctx.scene.target_hwnd, &ctx.scene)))
            break;
done:
    for (i = 0; i < count; ++i)
    {
        IWineDCompSwapChain_prepare_target_scene(bindings[i], NULL, NULL);
        IWineDCompSwapChain_Release(bindings[i]);
    }
    if (content_device) IUnknown_Release(content_device);
    LeaveCriticalSection(&dev->cs);
    return hr;
}

/* =====================================================================
 * IDCompositionDevice (1) Implementation
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_device_QueryInterface(IDCompositionDevice *iface,
        REFIID iid, void **out)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionDevice))
    {
        *out = &dev->IDCompositionDevice_iface;
        IDCompositionDevice_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IDCompositionDevice2))
    {
        *out = &dev->IDCompositionDevice2_iface;
        IDCompositionDevice_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IDCompositionDevice3))
    {
        *out = &dev->IDCompositionDevice3_iface;
        IDCompositionDevice_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IDCompositionDesktopDevice))
    {
        *out = &dev->IDCompositionDesktopDevice_iface;
        IDCompositionDevice_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_device_AddRef(IDCompositionDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);
    ULONG refcount = InterlockedIncrement(&dev->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_device_Release(IDCompositionDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);
    ULONG refcount = InterlockedDecrement(&dev->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        DeleteCriticalSection(&dev->cs);
        free(dev);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_Commit(IDCompositionDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);
    return dcomp_device_internal_commit(dev);
}

static HRESULT STDMETHODCALLTYPE dcomp_device_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);
    /* Direct binding has completed before Commit releases this lock. */
    EnterCriticalSection(&dev->cs);
    LeaveCriticalSection(&dev->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    if (!statistics)
        return E_POINTER;
    memset(statistics, 0, sizeof(*statistics));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTargetForHwnd(IDCompositionDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);
    struct dcomp_target *obj;
    HRESULT hr;

    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);

    if (!target)
        return E_POINTER;

    if (SUCCEEDED(hr = dcomp_target_create(dev, hwnd, topmost, &obj)))
        *target = &obj->IDCompositionTarget_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateVisual(IDCompositionDevice *iface,
        IDCompositionVisual **visual)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);
    struct dcomp_visual *obj;
    HRESULT hr;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_POINTER;

    if (SUCCEEDED(hr = dcomp_visual_create(dev, &obj)))
        *visual = (IDCompositionVisual *)&obj->IDCompositionVisual2_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p stub!\n",
            iface, width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateVirtualSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, surface %p stub!\n", iface, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSurfaceFromHandle(IDCompositionDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n",
            iface, transforms, elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n",
            iface, transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    TRACE("iface %p, effect_group %p.\n", iface, effect_group);
    return dcomp_effect_group_create(impl_from_IDCompositionDevice(iface), effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    TRACE("iface %p, clip %p.\n", iface, clip);
    return dcomp_rectangle_clip_create(impl_from_IDCompositionDevice(iface), clip);
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CheckDeviceState(IDCompositionDevice *iface,
        BOOL *valid)
{
    TRACE("iface %p, valid %p.\n", iface, valid);
    if (!valid)
        return E_POINTER;
    *valid = TRUE;
    return S_OK;
}

static const struct IDCompositionDeviceVtbl dcomp_device_vtbl =
{
    dcomp_device_QueryInterface,
    dcomp_device_AddRef,
    dcomp_device_Release,
    dcomp_device_Commit,
    dcomp_device_WaitForCommitCompletion,
    dcomp_device_GetFrameStatistics,
    dcomp_device_CreateTargetForHwnd,
    dcomp_device_CreateVisual,
    dcomp_device_CreateSurface,
    dcomp_device_CreateVirtualSurface,
    dcomp_device_CreateSurfaceFromHandle,
    dcomp_device_CreateSurfaceFromHwnd,
    dcomp_device_CreateTranslateTransform,
    dcomp_device_CreateScaleTransform,
    dcomp_device_CreateRotateTransform,
    dcomp_device_CreateSkewTransform,
    dcomp_device_CreateMatrixTransform,
    dcomp_device_CreateTransformGroup,
    dcomp_device_CreateTranslateTransform3D,
    dcomp_device_CreateScaleTransform3D,
    dcomp_device_CreateRotateTransform3D,
    dcomp_device_CreateMatrixTransform3D,
    dcomp_device_CreateTransform3DGroup,
    dcomp_device_CreateEffectGroup,
    dcomp_device_CreateRectangleClip,
    dcomp_device_CreateAnimation,
    dcomp_device_CheckDeviceState,
};

/* =====================================================================
 * IDCompositionDevice2 Implementation
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_device2_QueryInterface(IDCompositionDevice2 *iface,
        REFIID iid, void **out)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);
    return IDCompositionDevice_QueryInterface(&dev->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_device2_AddRef(IDCompositionDevice2 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);
    return IDCompositionDevice_AddRef(&dev->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_device2_Release(IDCompositionDevice2 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);
    return IDCompositionDevice_Release(&dev->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_Commit(IDCompositionDevice2 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);
    return dcomp_device_internal_commit(dev);
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_WaitForCommitCompletion(IDCompositionDevice2 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);
    /* Direct binding has completed before Commit releases this lock. */
    EnterCriticalSection(&dev->cs);
    LeaveCriticalSection(&dev->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_GetFrameStatistics(IDCompositionDevice2 *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    if (!statistics)
        return E_POINTER;
    memset(statistics, 0, sizeof(*statistics));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateVisual(IDCompositionDevice2 *iface,
        IDCompositionVisual2 **visual)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);
    struct dcomp_visual *obj;
    HRESULT hr;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_POINTER;

    if (SUCCEEDED(hr = dcomp_visual_create(dev, &obj)))
        *visual = &obj->IDCompositionVisual2_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateSurfaceFactory(IDCompositionDevice2 *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    FIXME("iface %p, rendering_device %p, surface_factory %p stub!\n",
            iface, rendering_device, surface_factory);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateSurface(IDCompositionDevice2 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p stub!\n",
            iface, width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateVirtualSurface(IDCompositionDevice2 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, surface %p stub!\n", iface, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateTranslateTransform(IDCompositionDevice2 *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateScaleTransform(IDCompositionDevice2 *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateRotateTransform(IDCompositionDevice2 *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateSkewTransform(IDCompositionDevice2 *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateMatrixTransform(IDCompositionDevice2 *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateTransformGroup(IDCompositionDevice2 *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n",
            iface, transforms, elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateTranslateTransform3D(IDCompositionDevice2 *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateScaleTransform3D(IDCompositionDevice2 *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateRotateTransform3D(IDCompositionDevice2 *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateMatrixTransform3D(IDCompositionDevice2 *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateTransform3DGroup(IDCompositionDevice2 *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n",
            iface, transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateEffectGroup(IDCompositionDevice2 *iface,
        IDCompositionEffectGroup **effect_group)
{
    TRACE("iface %p, effect_group %p.\n", iface, effect_group);
    return dcomp_effect_group_create(impl_from_IDCompositionDevice2(iface), effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateRectangleClip(IDCompositionDevice2 *iface,
        IDCompositionRectangleClip **clip)
{
    TRACE("iface %p, clip %p.\n", iface, clip);
    return dcomp_rectangle_clip_create(impl_from_IDCompositionDevice2(iface), clip);
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateAnimation(IDCompositionDevice2 *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static const struct IDCompositionDevice2Vtbl dcomp_device2_vtbl =
{
    dcomp_device2_QueryInterface,
    dcomp_device2_AddRef,
    dcomp_device2_Release,
    dcomp_device2_Commit,
    dcomp_device2_WaitForCommitCompletion,
    dcomp_device2_GetFrameStatistics,
    dcomp_device2_CreateVisual,
    dcomp_device2_CreateSurfaceFactory,
    dcomp_device2_CreateSurface,
    dcomp_device2_CreateVirtualSurface,
    dcomp_device2_CreateTranslateTransform,
    dcomp_device2_CreateScaleTransform,
    dcomp_device2_CreateRotateTransform,
    dcomp_device2_CreateSkewTransform,
    dcomp_device2_CreateMatrixTransform,
    dcomp_device2_CreateTransformGroup,
    dcomp_device2_CreateTranslateTransform3D,
    dcomp_device2_CreateScaleTransform3D,
    dcomp_device2_CreateRotateTransform3D,
    dcomp_device2_CreateMatrixTransform3D,
    dcomp_device2_CreateTransform3DGroup,
    dcomp_device2_CreateEffectGroup,
    dcomp_device2_CreateRectangleClip,
    dcomp_device2_CreateAnimation,
};

/* =====================================================================
 * IDCompositionDevice3 Implementation
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_device3_QueryInterface(IDCompositionDevice3 *iface,
        REFIID iid, void **out)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);
    return IDCompositionDevice_QueryInterface(&dev->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_device3_AddRef(IDCompositionDevice3 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);
    return IDCompositionDevice_AddRef(&dev->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_device3_Release(IDCompositionDevice3 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);
    return IDCompositionDevice_Release(&dev->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_Commit(IDCompositionDevice3 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);
    return dcomp_device_internal_commit(dev);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_WaitForCommitCompletion(IDCompositionDevice3 *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);
    /* Direct binding has completed before Commit releases this lock. */
    EnterCriticalSection(&dev->cs);
    LeaveCriticalSection(&dev->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_GetFrameStatistics(IDCompositionDevice3 *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    if (!statistics)
        return E_POINTER;
    memset(statistics, 0, sizeof(*statistics));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVisual(IDCompositionDevice3 *iface,
        IDCompositionVisual2 **visual)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);
    return IDCompositionDevice2_CreateVisual(&dev->IDCompositionDevice2_iface, visual);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSurfaceFactory(IDCompositionDevice3 *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    FIXME("iface %p, rendering_device %p, surface_factory %p stub!\n",
            iface, rendering_device, surface_factory);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSurface(IDCompositionDevice3 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, surface %p stub!\n", iface, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVirtualSurface(IDCompositionDevice3 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, surface %p stub!\n", iface, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTranslateTransform(IDCompositionDevice3 *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateScaleTransform(IDCompositionDevice3 *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRotateTransform(IDCompositionDevice3 *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSkewTransform(IDCompositionDevice3 *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateMatrixTransform(IDCompositionDevice3 *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransformGroup(IDCompositionDevice3 *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transform_group %p stub!\n", iface, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTranslateTransform3D(IDCompositionDevice3 *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateScaleTransform3D(IDCompositionDevice3 *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRotateTransform3D(IDCompositionDevice3 *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateMatrixTransform3D(IDCompositionDevice3 *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransform3DGroup(IDCompositionDevice3 *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transform_3d_group %p stub!\n", iface, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateEffectGroup(IDCompositionDevice3 *iface,
        IDCompositionEffectGroup **effect_group)
{
    TRACE("iface %p, effect_group %p.\n", iface, effect_group);
    return dcomp_effect_group_create(impl_from_IDCompositionDevice3(iface), effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRectangleClip(IDCompositionDevice3 *iface,
        IDCompositionRectangleClip **clip)
{
    TRACE("iface %p, clip %p.\n", iface, clip);
    return dcomp_rectangle_clip_create(impl_from_IDCompositionDevice3(iface), clip);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateAnimation(IDCompositionDevice3 *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateGaussianBlurEffect(IDCompositionDevice3 *iface,
        IDCompositionGaussianBlurEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateBrightnessEffect(IDCompositionDevice3 *iface,
        IDCompositionBrightnessEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateColorMatrixEffect(IDCompositionDevice3 *iface,
        IDCompositionColorMatrixEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateShadowEffect(IDCompositionDevice3 *iface,
        IDCompositionShadowEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateHueRotationEffect(IDCompositionDevice3 *iface,
        IDCompositionHueRotationEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSaturationEffect(IDCompositionDevice3 *iface,
        IDCompositionSaturationEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTurbulenceEffect(IDCompositionDevice3 *iface,
        IDCompositionTurbulenceEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateLinearTransferEffect(IDCompositionDevice3 *iface,
        IDCompositionLinearTransferEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTableTransferEffect(IDCompositionDevice3 *iface,
        IDCompositionTableTransferEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateCompositeEffect(IDCompositionDevice3 *iface,
        IDCompositionCompositeEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateBlendEffect(IDCompositionDevice3 *iface,
        IDCompositionBlendEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateArithmeticCompositeEffect(IDCompositionDevice3 *iface,
        IDCompositionArithmeticCompositeEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateAffineTransform2DEffect(IDCompositionDevice3 *iface,
        IDCompositionAffineTransform2DEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static const struct IDCompositionDevice3Vtbl dcomp_device3_vtbl =
{
    dcomp_device3_QueryInterface,
    dcomp_device3_AddRef,
    dcomp_device3_Release,
    dcomp_device3_Commit,
    dcomp_device3_WaitForCommitCompletion,
    dcomp_device3_GetFrameStatistics,
    dcomp_device3_CreateVisual,
    dcomp_device3_CreateSurfaceFactory,
    dcomp_device3_CreateSurface,
    dcomp_device3_CreateVirtualSurface,
    dcomp_device3_CreateTranslateTransform,
    dcomp_device3_CreateScaleTransform,
    dcomp_device3_CreateRotateTransform,
    dcomp_device3_CreateSkewTransform,
    dcomp_device3_CreateMatrixTransform,
    dcomp_device3_CreateTransformGroup,
    dcomp_device3_CreateTranslateTransform3D,
    dcomp_device3_CreateScaleTransform3D,
    dcomp_device3_CreateRotateTransform3D,
    dcomp_device3_CreateMatrixTransform3D,
    dcomp_device3_CreateTransform3DGroup,
    dcomp_device3_CreateEffectGroup,
    dcomp_device3_CreateRectangleClip,
    dcomp_device3_CreateAnimation,
    dcomp_device3_CreateGaussianBlurEffect,
    dcomp_device3_CreateBrightnessEffect,
    dcomp_device3_CreateColorMatrixEffect,
    dcomp_device3_CreateShadowEffect,
    dcomp_device3_CreateHueRotationEffect,
    dcomp_device3_CreateSaturationEffect,
    dcomp_device3_CreateTurbulenceEffect,
    dcomp_device3_CreateLinearTransferEffect,
    dcomp_device3_CreateTableTransferEffect,
    dcomp_device3_CreateCompositeEffect,
    dcomp_device3_CreateBlendEffect,
    dcomp_device3_CreateArithmeticCompositeEffect,
    dcomp_device3_CreateAffineTransform2DEffect,
};

/* =====================================================================
 * IDCompositionDesktopDevice Implementation
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_QueryInterface(IDCompositionDesktopDevice *iface,
        REFIID iid, void **out)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    return IDCompositionDevice_QueryInterface(&dev->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_desktop_device_AddRef(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    return IDCompositionDevice_AddRef(&dev->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_desktop_device_Release(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    return IDCompositionDevice_Release(&dev->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_Commit(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_internal_commit(dev);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_WaitForCommitCompletion(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    /* Direct binding has completed before Commit releases this lock. */
    EnterCriticalSection(&dev->cs);
    LeaveCriticalSection(&dev->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_GetFrameStatistics(IDCompositionDesktopDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    if (!statistics)
        return E_POINTER;
    memset(statistics, 0, sizeof(*statistics));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateVisual(IDCompositionDesktopDevice *iface,
        IDCompositionVisual2 **visual)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    return IDCompositionDevice2_CreateVisual(&dev->IDCompositionDevice2_iface, visual);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurfaceFactory(IDCompositionDesktopDevice *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    FIXME("iface %p, rendering_device %p, surface_factory %p stub!\n",
            iface, rendering_device, surface_factory);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, surface %p stub!\n", iface, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateVirtualSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, surface %p stub!\n", iface, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTranslateTransform(IDCompositionDesktopDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateScaleTransform(IDCompositionDesktopDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRotateTransform(IDCompositionDesktopDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSkewTransform(IDCompositionDesktopDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateMatrixTransform(IDCompositionDesktopDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransformGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transform_group %p stub!\n", iface, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTranslateTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateScaleTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRotateTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateMatrixTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransform3DGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transform_3d_group %p stub!\n", iface, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateEffectGroup(IDCompositionDesktopDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    TRACE("iface %p, effect_group %p.\n", iface, effect_group);
    return dcomp_effect_group_create(impl_from_IDCompositionDesktopDevice(iface), effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRectangleClip(IDCompositionDesktopDevice *iface,
        IDCompositionRectangleClip **clip)
{
    TRACE("iface %p, clip %p.\n", iface, clip);
    return dcomp_rectangle_clip_create(impl_from_IDCompositionDesktopDevice(iface), clip);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateAnimation(IDCompositionDesktopDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTargetForHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    struct dcomp_target *obj;
    HRESULT hr;

    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);

    if (!target)
        return E_POINTER;

    if (SUCCEEDED(hr = dcomp_target_create(dev, hwnd, topmost, &obj)))
        *target = &obj->IDCompositionTarget_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurfaceFromHandle(IDCompositionDesktopDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurfaceFromHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static const struct IDCompositionDesktopDeviceVtbl dcomp_desktop_device_vtbl =
{
    dcomp_desktop_device_QueryInterface,
    dcomp_desktop_device_AddRef,
    dcomp_desktop_device_Release,
    dcomp_desktop_device_Commit,
    dcomp_desktop_device_WaitForCommitCompletion,
    dcomp_desktop_device_GetFrameStatistics,
    dcomp_desktop_device_CreateVisual,
    dcomp_desktop_device_CreateSurfaceFactory,
    dcomp_desktop_device_CreateSurface,
    dcomp_desktop_device_CreateVirtualSurface,
    dcomp_desktop_device_CreateTranslateTransform,
    dcomp_desktop_device_CreateScaleTransform,
    dcomp_desktop_device_CreateRotateTransform,
    dcomp_desktop_device_CreateSkewTransform,
    dcomp_desktop_device_CreateMatrixTransform,
    dcomp_desktop_device_CreateTransformGroup,
    dcomp_desktop_device_CreateTranslateTransform3D,
    dcomp_desktop_device_CreateScaleTransform3D,
    dcomp_desktop_device_CreateRotateTransform3D,
    dcomp_desktop_device_CreateMatrixTransform3D,
    dcomp_desktop_device_CreateTransform3DGroup,
    dcomp_desktop_device_CreateEffectGroup,
    dcomp_desktop_device_CreateRectangleClip,
    dcomp_desktop_device_CreateAnimation,
    dcomp_desktop_device_CreateTargetForHwnd,
    dcomp_desktop_device_CreateSurfaceFromHandle,
    dcomp_desktop_device_CreateSurfaceFromHwnd,
};

static HRESULT dcomp_device_create(REFIID iid, void **device)
{
    struct dcomp_device *dev;
    HRESULT hr;

    if (!device)
        return E_POINTER;

    if (!(dev = calloc(1, sizeof(*dev))))
        return E_OUTOFMEMORY;

    dev->IDCompositionDevice_iface.lpVtbl = &dcomp_device_vtbl;
    dev->IDCompositionDevice2_iface.lpVtbl = &dcomp_device2_vtbl;
    dev->IDCompositionDevice3_iface.lpVtbl = &dcomp_device3_vtbl;
    dev->IDCompositionDesktopDevice_iface.lpVtbl = &dcomp_desktop_device_vtbl;
    dev->refcount = 1;
    list_init(&dev->targets);
    InitializeCriticalSection(&dev->cs);

    hr = IDCompositionDevice_QueryInterface(&dev->IDCompositionDevice_iface, iid, device);
    IDCompositionDevice_Release(&dev->IDCompositionDevice_iface);
    return hr;
}

/* =====================================================================
 * Exported API Entry Points
 * ===================================================================== */

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("dxgi_device %p, iid %s, device %p.\n", dxgi_device, debugstr_guid(iid), device);
    return dcomp_device_create(iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("rendering_device %p, iid %s, device %p.\n", rendering_device, debugstr_guid(iid), device);
    return dcomp_device_create(iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("rendering_device %p, iid %s, device %p.\n", rendering_device, debugstr_guid(iid), device);
    return dcomp_device_create(iid, device);
}
