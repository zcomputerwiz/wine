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

#define INITGUID
#define COBJMACROS
#include "initguid.h"
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "objidl.h"
#include "dxgi.h"
#include "dcomp.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/dcomp_layer.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

struct dcomp_device;
struct dcomp_target;
struct dcomp_visual;

struct dcomp_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    LONG refcount;
    struct dcomp_device *device;
    struct list entry;
    HWND hwnd;
    BOOL topmost;
    struct dcomp_visual *root;
};

struct dcomp_visual
{
    IDCompositionVisual2 IDCompositionVisual2_iface;
    LONG refcount;
    struct dcomp_device *device;
    IUnknown *content;
    float offset_x;
    float offset_y;
    struct dcomp_visual *parent;
    struct dcomp_visual *children;
    struct dcomp_visual *next_sibling;
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

/* =====================================================================
 * IDCompositionVisual & IDCompositionVisual2
 * ===================================================================== */

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
        if (visual->content)
            IUnknown_Release(visual->content);

        while (visual->children)
        {
            struct dcomp_visual *child = visual->children;
            visual->children = child->next_sibling;
            child->parent = NULL;
            IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
        }

        free(visual);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetXAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetX(IDCompositionVisual2 *iface, float offset_x)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, offset_x %f.\n", iface, offset_x);
    visual->offset_x = offset_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetYAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetY(IDCompositionVisual2 *iface, float offset_y)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, offset_y %f.\n", iface, offset_y);
    visual->offset_y = offset_y;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransformObject(IDCompositionVisual2 *iface,
        IDCompositionTransform *transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransform(IDCompositionVisual2 *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransformParent(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    FIXME("iface %p, visual %p stub!\n", iface, visual);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetEffect(IDCompositionVisual2 *iface,
        IDCompositionEffect *effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return S_OK;
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

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClipObject(IDCompositionVisual2 *iface,
        IDCompositionClip *clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClip(IDCompositionVisual2 *iface,
        const D2D_RECT_F *rect)
{
    TRACE("iface %p, rect %p.\n", iface, rect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetContent(IDCompositionVisual2 *iface,
        IUnknown *content)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, content %p.\n", iface, content);

    if (content)
        IUnknown_AddRef(content);
    if (visual->content)
        IUnknown_Release(visual->content);

    visual->content = content;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_AddVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual, BOOL insert_above, IDCompositionVisual *reference_visual)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct dcomp_visual *child, *ref = NULL;

    TRACE("iface %p, visual %p, insert_above %d, reference_visual %p.\n",
            iface, visual, insert_above, reference_visual);

    if (!visual)
        return E_INVALIDARG;

    child = impl_from_IDCompositionVisual(visual);
    if (reference_visual)
        ref = impl_from_IDCompositionVisual(reference_visual);

    IDCompositionVisual2_AddRef(&child->IDCompositionVisual2_iface);
    child->parent = parent;

    if (!ref)
    {
        if (insert_above)
        {
            child->next_sibling = parent->children;
            parent->children = child;
        }
        else
        {
            struct dcomp_visual **p = &parent->children;
            while (*p)
                p = &(*p)->next_sibling;
            *p = child;
            child->next_sibling = NULL;
        }
    }
    else
    {
        struct dcomp_visual **p = &parent->children;
        while (*p && *p != ref)
            p = &(*p)->next_sibling;

        if (insert_above)
        {
            child->next_sibling = *p;
            *p = child;
        }
        else if (*p)
        {
            child->next_sibling = (*p)->next_sibling;
            (*p)->next_sibling = child;
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct dcomp_visual *child, **p;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_INVALIDARG;

    child = impl_from_IDCompositionVisual(visual);
    p = &parent->children;
    while (*p && *p != child)
        p = &(*p)->next_sibling;

    if (*p)
    {
        *p = child->next_sibling;
        child->parent = NULL;
        child->next_sibling = NULL;
        IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
        return S_OK;
    }

    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveAllVisuals(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p.\n", iface);

    while (parent->children)
    {
        struct dcomp_visual *child = parent->children;
        parent->children = child->next_sibling;
        child->parent = NULL;
        child->next_sibling = NULL;
        IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetCompositeMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_COMPOSITE_MODE composite_mode)
{
    TRACE("iface %p, composite_mode %d.\n", iface, composite_mode);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOpacityMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    TRACE("iface %p, opacity_mode %d.\n", iface, opacity_mode);
    return S_OK;
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
        if (target->root)
            IDCompositionVisual2_Release(&target->root->IDCompositionVisual2_iface);

        EnterCriticalSection(&target->device->cs);
        list_remove(&target->entry);
        LeaveCriticalSection(&target->device->cs);

        free(target);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_target_SetRoot(IDCompositionTarget *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    struct dcomp_visual *new_root = NULL;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (visual)
    {
        new_root = impl_from_IDCompositionVisual(visual);
        IDCompositionVisual2_AddRef(&new_root->IDCompositionVisual2_iface);
    }

    if (target->root)
        IDCompositionVisual2_Release(&target->root->IDCompositionVisual2_iface);

    target->root = new_root;
    return S_OK;
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

static void dcomp_visual_commit_content(struct dcomp_visual *visual, HWND target_hwnd)
{
    struct dcomp_visual *child;

    if (!visual)
        return;

    if (visual->content && target_hwnd)
    {
        WCHAR prop_name[64];
        HWND comp_wnd;

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)visual->content);
        comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);
        if (comp_wnd)
        {
            TRACE("Commit: notifying comp_wnd %p of target_hwnd %p.\n", comp_wnd, target_hwnd);
            SendMessageW(comp_wnd, WM_WINE_DCOMP_SET_TARGET, 0, (LPARAM)target_hwnd);
        }
    }

    for (child = visual->children; child; child = child->next_sibling)
        dcomp_visual_commit_content(child, target_hwnd);
}

static HRESULT dcomp_device_internal_commit(struct dcomp_device *dev)
{
    struct dcomp_target *target;

    TRACE("device %p.\n", dev);

    EnterCriticalSection(&dev->cs);
    LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
    {
        if (target->root && target->hwnd)
            dcomp_visual_commit_content(target->root, target->hwnd);
    }
    LeaveCriticalSection(&dev->cs);

    return S_OK;
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
    TRACE("iface %p.\n", iface);
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
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
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
    TRACE("iface %p.\n", iface);
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
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateRectangleClip(IDCompositionDevice2 *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
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
    TRACE("iface %p.\n", iface);
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
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRectangleClip(IDCompositionDevice3 *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
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
    TRACE("iface %p.\n", iface);
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
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRectangleClip(IDCompositionDesktopDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
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
