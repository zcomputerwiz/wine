/*
 * DirectComposition device implementation
 *
 * Copyright 2020 Nikolay Sivov for CodeWeavers
 * Copyright 2025 Zhiyi Zhang for CodeWeavers
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
#include "uxtheme.h"
#include "objidl.h"
#include "dxgi.h"
#include "d3d11.h"
#include "d2d1_3.h"
struct wined3d_resource;
struct wined3d_texture;
struct wined3d_device_parent;
#include "wine/winedxgi.h"
#include "dcomp.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/dcomp_layer.h"
#include "wine/dcomp_underlay.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

struct dcomp_device;
struct dcomp_target;
struct dcomp_visual;

static void dcomp_visual_unbind_tree(struct dcomp_visual *visual);
static const struct IDCompositionVisual2Vtbl dcomp_visual_vtbl;
static const struct IDCompositionAnimationVtbl dcomp_animation_vtbl;
struct dcomp_animation;
static inline struct dcomp_animation *impl_from_IDCompositionAnimation(IDCompositionAnimation *iface);
static HRESULT dcomp_animation_check_for_property(IDCompositionAnimation *animation,
        struct dcomp_device *device);
static SRWLOCK target_registration_lock = SRWLOCK_INIT;

struct dcomp_device;
struct dcomp_target;
struct dcomp_visual;

struct dcomp_effect_group
{
    IDCompositionEffectGroup IDCompositionEffectGroup_iface;
    LONG refcount;
    struct dcomp_device *device;
    float opacity;
    IDCompositionAnimation *opacity_animation;
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
    IDCompositionAnimation *offset_x_animation;
    IDCompositionAnimation *offset_y_animation;
    BOOL has_clip;
    D2D_RECT_F clip;
    IDCompositionClip *clip_obj;
    IUnknown *content;
    IDCompositionEffect *effect;
    float opacity;
    enum DCOMPOSITION_OPACITY_MODE opacity_mode;
    enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode;
    enum DCOMPOSITION_BORDER_MODE border_mode;
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
    BOOL is_root;

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
    /* Lazily created auxiliary producer for scenes whose content is only
     * IDCompositionSurface layers (which have no IWineDCompSwapChain binding
     * of their own). Created from the first surface layer's rendering device. */
    IDXGISwapChain1 *aux_swapchain;
    IWineDCompSwapChain *aux_binding;
    ID3D11Device *aux_device;
};

struct dcomp_device
{
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDevice2 IDCompositionDevice2_iface;
    IDCompositionDevice3 IDCompositionDevice3_iface;
    IDCompositionDesktopDevice IDCompositionDesktopDevice_iface;
    int version;
    LONG refcount;
    struct list targets;
    CRITICAL_SECTION cs;
    IUnknown *rendering_device;
    GUID rendering_device_iid;
    IDCompositionSurface *drawing_surface;
};

struct dcomp_surface_factory
{
    IDCompositionSurfaceFactory IDCompositionSurfaceFactory_iface;
    IDCompositionDevice *device;
    IUnknown *rendering_device;
    GUID rendering_device_iid;
    LONG refcount;
};

struct dcomp_surface
{
    IDCompositionSurface IDCompositionSurface_iface;
    IDCompositionVirtualSurface IDCompositionVirtualSurface_iface;
    IWineDCompSurface IWineDCompSurface_iface;
    IDCompositionDevice *device;
    IUnknown *rendering_device;
    GUID rendering_device_iid;
    ID3D11Texture2D *physical_surface;
    UINT width;
    UINT height;
    DXGI_FORMAT pixel_format;
    DXGI_ALPHA_MODE alpha_mode;
    RECT draw_rect;
    BOOL drawn;
    BOOL virtual_surface;
    BOOL suspended;
    /* TRUE once a BeginDraw/EndDraw pair has completed; the composed content
     * is the last ended draw. A never-drawn surface (or one with a draw in
     * progress) composes as fully transparent. */
    BOOL content_valid;
    LONG refcount;
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

static inline struct dcomp_surface_factory *impl_from_IDCompositionSurfaceFactory(IDCompositionSurfaceFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface_factory, IDCompositionSurfaceFactory_iface);
}

static inline struct dcomp_surface *impl_from_IDCompositionSurface(IDCompositionSurface *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface, IDCompositionSurface_iface);
}

static inline struct dcomp_surface *impl_from_IDCompositionVirtualSurface(IDCompositionVirtualSurface *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface, IDCompositionVirtualSurface_iface);
}

static HRESULT dcomp_target_ensure_aux_producer(struct dcomp_target *target,
        IUnknown *rendering_device, const GUID *rendering_device_iid);
static void dcomp_target_release_aux_producer(struct dcomp_target *target);

static const struct IDCompositionVirtualSurfaceVtbl dcomp_virtual_surface_vtbl;

/* =====================================================================
 * Surface helpers
 * ===================================================================== */

static HRESULT dcomp_hold_rendering_device(IUnknown *rendering_device, IUnknown **held_device, GUID *held_iid)
{
    IDXGIDevice *dxgi_device;
    ID3D11Device *d3d11_device;
    ID2D1Device *d2d_device;

    if (SUCCEEDED(IUnknown_QueryInterface(rendering_device, &IID_IDXGIDevice, (void **)&dxgi_device)))
    {
        *held_device = (IUnknown *)dxgi_device;
        *held_iid = IID_IDXGIDevice;
        return S_OK;
    }
    if (SUCCEEDED(IUnknown_QueryInterface(rendering_device, &IID_ID3D11Device, (void **)&d3d11_device)))
    {
        *held_device = (IUnknown *)d3d11_device;
        *held_iid = IID_ID3D11Device;
        return S_OK;
    }
    if (SUCCEEDED(IUnknown_QueryInterface(rendering_device, &IID_ID2D1Device, (void **)&d2d_device)))
    {
        *held_device = (IUnknown *)d2d_device;
        *held_iid = IID_ID2D1Device;
        return S_OK;
    }

    return E_NOINTERFACE;
}

static HRESULT dcomp_get_d3d11_device(IUnknown *rendering_device, const GUID *rendering_device_iid,
        ID3D11Device **d3d11_device)
{
    HRESULT hr;

    if (IsEqualGUID(rendering_device_iid, &IID_ID2D1Device))
    {
        ID2D1Device2 *d2d_device;
        IDXGIDevice *dxgi_device;

        if (FAILED(hr = IUnknown_QueryInterface(rendering_device, &IID_ID2D1Device2, (void **)&d2d_device)))
            return hr;
        hr = ID2D1Device2_GetDxgiDevice(d2d_device, &dxgi_device);
        ID2D1Device2_Release(d2d_device);
        if (FAILED(hr))
            return hr;
        hr = IUnknown_QueryInterface((IUnknown *)dxgi_device, &IID_ID3D11Device, (void **)d3d11_device);
        IDXGIDevice_Release(dxgi_device);
        return hr;
    }

    return IUnknown_QueryInterface(rendering_device, &IID_ID3D11Device, (void **)d3d11_device);
}

/* =====================================================================
 * Auxiliary producer (surface-only scenes)
 * ===================================================================== */

static HRESULT dcomp_target_ensure_aux_producer(struct dcomp_target *target,
        IUnknown *rendering_device, const GUID *rendering_device_iid)
{
    ID3D11Device *d3d11_device;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    DXGI_SWAP_CHAIN_DESC1 desc;
    HRESULT hr;

    if (target->aux_binding)
        return S_OK;

    if (FAILED(hr = dcomp_get_d3d11_device(rendering_device, rendering_device_iid, &d3d11_device)))
        return hr;

    hr = IUnknown_QueryInterface((IUnknown *)d3d11_device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (SUCCEEDED(hr))
        hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    if (SUCCEEDED(hr))
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);

    if (SUCCEEDED(hr))
    {
        memset(&desc, 0, sizeof(desc));
        desc.Width = 1;
        desc.Height = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = IDXGIFactory2_CreateSwapChainForComposition(factory, (IUnknown *)d3d11_device,
                &desc, NULL, &target->aux_swapchain);
    }
    if (SUCCEEDED(hr))
        hr = IUnknown_QueryInterface((IUnknown *)target->aux_swapchain, &IID_IWineDCompSwapChain,
                (void **)&target->aux_binding);
    if (SUCCEEDED(hr))
        target->aux_device = d3d11_device;
    else
    {
        if (target->aux_swapchain)
        {
            IDXGISwapChain1_Release(target->aux_swapchain);
            target->aux_swapchain = NULL;
        }
        target->aux_binding = NULL;
        ID3D11Device_Release(d3d11_device);
    }
    if (factory)
        IDXGIFactory2_Release(factory);
    if (adapter)
        IDXGIAdapter_Release(adapter);
    if (dxgi_device)
        IDXGIDevice_Release(dxgi_device);
    return hr;
}

static void dcomp_target_release_aux_producer(struct dcomp_target *target)
{
    if (target->aux_binding)
    {
        IWineDCompSwapChain_set_target_scene(target->aux_binding, NULL, NULL);
        IWineDCompSwapChain_Release(target->aux_binding);
        target->aux_binding = NULL;
    }
    if (target->aux_swapchain)
    {
        IDXGISwapChain1_Release(target->aux_swapchain);
        target->aux_swapchain = NULL;
    }
    if (target->aux_device)
    {
        ID3D11Device_Release(target->aux_device);
        target->aux_device = NULL;
    }
}

/* =====================================================================
 * IDCompositionSurface
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_surface_QueryInterface(IDCompositionSurface *iface,
        REFIID iid, void **out)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionSurface))
    {
        *out = &surface->IDCompositionSurface_iface;
        IDCompositionSurface_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IWineDCompSurface))
    {
        *out = &surface->IWineDCompSurface_iface;
        IDCompositionSurface_AddRef(iface);
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IDCompositionVirtualSurface) && surface->virtual_surface)
    {
        *out = &surface->IDCompositionVirtualSurface_iface;
        IDCompositionSurface_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_AddRef(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    ULONG refcount = InterlockedIncrement(&surface->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_Release(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    ULONG refcount = InterlockedDecrement(&surface->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);

        EnterCriticalSection(&device->cs);
        if (device->drawing_surface == &surface->IDCompositionSurface_iface)
            device->drawing_surface = NULL;
        LeaveCriticalSection(&device->cs);

        if (surface->physical_surface)
            ID3D11Texture2D_Release(surface->physical_surface);
        IUnknown_Release(surface->rendering_device);
        IDCompositionDevice_Release(surface->device);
        free(surface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_BeginDraw(IDCompositionSurface *iface,
        const RECT *rect, REFIID iid, void **object, POINT *offset)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);
    RECT whole_rect;
    HRESULT hr;

    TRACE("iface %p, rect %s, iid %s, object %p, offset %p.\n", iface, wine_dbgstr_rect(rect),
            debugstr_guid(iid), object, offset);

    if (!object || !offset)
        return E_INVALIDARG;

    /* Another surface of the device being drawn to is reported before the rectangle is validated. */
    EnterCriticalSection(&device->cs);
    hr = device->drawing_surface && device->drawing_surface != &surface->IDCompositionSurface_iface
            ? DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED : S_OK;
    LeaveCriticalSection(&device->cs);
    if (FAILED(hr))
        return hr;

    if (!rect)
    {
        SetRect(&whole_rect, 0, 0, surface->width, surface->height);
        rect = &whole_rect;
    }

    /* The first BeginDraw of a non-virtual surface must cover the whole surface */
    if (!surface->virtual_surface && !surface->drawn && !(rect->left == 0 && rect->top == 0
            && rect->right == surface->width && rect->bottom == surface->height))
        return E_INVALIDARG;

    if (rect->left < 0 || rect->top < 0 || rect->right > surface->width
            || rect->bottom > surface->height)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_ID2D1DeviceContext))
    {
        ID2D1Device *d2d_device;

        if (!IsEqualGUID(&surface->rendering_device_iid, &IID_ID2D1Device))
            return E_NOINTERFACE;

        if (FAILED(hr = IUnknown_QueryInterface(surface->rendering_device, &IID_ID2D1Device,
                (void **)&d2d_device)))
            return hr;
        hr = ID2D1Device_CreateDeviceContext(d2d_device, D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                (ID2D1DeviceContext **)object);
        ID2D1Device_Release(d2d_device);
        if (FAILED(hr))
            return hr;
    }
    else if (FAILED(hr = IUnknown_QueryInterface((IUnknown *)surface->physical_surface, iid, object)))
    {
        return hr;
    }

    EnterCriticalSection(&device->cs);
    if (device->drawing_surface && device->drawing_surface != &surface->IDCompositionSurface_iface)
    {
        LeaveCriticalSection(&device->cs);
        IUnknown_Release((IUnknown *)*object);
        return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
    }
    device->drawing_surface = &surface->IDCompositionSurface_iface;
    surface->drawn = TRUE;
    surface->suspended = FALSE;
    surface->content_valid = FALSE;
    surface->draw_rect = *rect;
    LeaveCriticalSection(&device->cs);

    offset->x = rect->left;
    offset->y = rect->top;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_EndDraw(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&device->cs);
    if (!(device->drawing_surface && device->drawing_surface == &surface->IDCompositionSurface_iface))
    {
        LeaveCriticalSection(&device->cs);
        return DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED;
    }
    device->drawing_surface = NULL;
    surface->content_valid = TRUE;
    surface->suspended = FALSE;
    LeaveCriticalSection(&device->cs);

    SetRectEmpty(&surface->draw_rect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_SuspendDraw(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&device->cs);
    if (device->drawing_surface != iface)
    {
        LeaveCriticalSection(&device->cs);
        return DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED;
    }
    device->drawing_surface = NULL;
    surface->suspended = TRUE;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_ResumeDraw(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&device->cs);
    if (!surface->suspended)
    {
        LeaveCriticalSection(&device->cs);
        return DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED;
    }
    if (device->drawing_surface)
    {
        LeaveCriticalSection(&device->cs);
        return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
    }
    device->drawing_surface = iface;
    surface->suspended = FALSE;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_Scroll(IDCompositionSurface *iface, const RECT *scroll,
        const RECT *clip, int offset_x, int offset_y)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);
    ID3D11DeviceContext *context;
    ID3D11Device *d3d11_device;
    ID3D11Resource *source_resource = NULL, *destination_resource = NULL, *temporary_resource = NULL;
    ID3D11Texture2D *temporary;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_BOX source_box;
    RECT whole, source, destination;
    HRESULT hr;

    TRACE("iface %p, scroll %s, clip %s, offset_x %d, offset_y %d.\n", iface,
            wine_dbgstr_rect(scroll), wine_dbgstr_rect(clip), offset_x, offset_y);

    EnterCriticalSection(&device->cs);
    if (device->drawing_surface == iface)
    {
        LeaveCriticalSection(&device->cs);
        return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
    }
    LeaveCriticalSection(&device->cs);

    SetRect(&whole, 0, 0, surface->width, surface->height);
    if (!scroll)
        scroll = &whole;
    if (!clip)
        clip = &whole;
    if (scroll->left < 0 || scroll->top < 0 || scroll->right > (LONG)surface->width
            || scroll->bottom > (LONG)surface->height || scroll->left >= scroll->right
            || scroll->top >= scroll->bottom || clip->left < 0 || clip->top < 0
            || clip->right > (LONG)surface->width || clip->bottom > (LONG)surface->height
            || clip->left >= clip->right || clip->top >= clip->bottom)
        return E_INVALIDARG;

    destination = *scroll;
    OffsetRect(&destination, offset_x, offset_y);
    if (!IntersectRect(&destination, &destination, clip)
            || !IntersectRect(&destination, &destination, &whole))
        return S_OK;
    source = destination;
    OffsetRect(&source, -offset_x, -offset_y);
    if (!IntersectRect(&source, &source, scroll))
        return S_OK;

    if (FAILED(hr = dcomp_get_d3d11_device(surface->rendering_device,
            &surface->rendering_device_iid, &d3d11_device)))
        return hr;
    ID3D11Texture2D_GetDesc(surface->physical_surface, &desc);
    hr = ID3D11Device_CreateTexture2D(d3d11_device, &desc, NULL, &temporary);
    if (SUCCEEDED(hr))
    {
        hr = ID3D11Texture2D_QueryInterface(surface->physical_surface, &IID_ID3D11Resource,
                (void **)&source_resource);
        if (SUCCEEDED(hr))
            hr = ID3D11Texture2D_QueryInterface(surface->physical_surface, &IID_ID3D11Resource,
                    (void **)&destination_resource);
        if (SUCCEEDED(hr))
            hr = ID3D11Texture2D_QueryInterface(temporary, &IID_ID3D11Resource,
                    (void **)&temporary_resource);
        if (FAILED(hr))
        {
            if (destination_resource) ID3D11Resource_Release(destination_resource);
            if (source_resource) ID3D11Resource_Release(source_resource);
            ID3D11Texture2D_Release(temporary);
            ID3D11Device_Release(d3d11_device);
            return hr;
        }
        ID3D11Device_GetImmediateContext(d3d11_device, &context);
        ID3D11DeviceContext_CopyResource(context, temporary_resource, source_resource);
        source_box.left = source.left;
        source_box.top = source.top;
        source_box.front = 0;
        source_box.right = source.right;
        source_box.bottom = source.bottom;
        source_box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(context, destination_resource, 0,
                destination.left, destination.top, 0, temporary_resource, 0, &source_box);
        ID3D11DeviceContext_Release(context);
        ID3D11Resource_Release(temporary_resource);
        ID3D11Resource_Release(destination_resource);
        ID3D11Resource_Release(source_resource);
        ID3D11Texture2D_Release(temporary);
        surface->content_valid = TRUE;
    }
    ID3D11Device_Release(d3d11_device);
    return hr;
}

/* =====================================================================
 * IWineDCompSurface
 * ===================================================================== */

static inline struct dcomp_surface *impl_from_IWineDCompSurface(IWineDCompSurface *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface, IWineDCompSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_wine_surface_QueryInterface(IWineDCompSurface *iface,
        REFIID riid, void **out)
{
    struct dcomp_surface *surface = impl_from_IWineDCompSurface(iface);
    return IDCompositionSurface_QueryInterface(&surface->IDCompositionSurface_iface, riid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_wine_surface_AddRef(IWineDCompSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IWineDCompSurface(iface);
    return IDCompositionSurface_AddRef(&surface->IDCompositionSurface_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_wine_surface_Release(IWineDCompSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IWineDCompSurface(iface);
    return IDCompositionSurface_Release(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_wine_surface_get_physical_texture(IWineDCompSurface *iface,
        ID3D11Texture2D **texture)
{
    struct dcomp_surface *surface = impl_from_IWineDCompSurface(iface);
    *texture = surface->physical_surface;
    ID3D11Texture2D_AddRef(*texture);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_wine_surface_get_content_valid(IWineDCompSurface *iface, BOOL *valid)
{
    struct dcomp_surface *surface = impl_from_IWineDCompSurface(iface);
    *valid = surface->content_valid;
    return S_OK;
}

static const struct IWineDCompSurfaceVtbl dcomp_wine_surface_vtbl =
{
    /* IUnknown methods */
    dcomp_wine_surface_QueryInterface,
    dcomp_wine_surface_AddRef,
    dcomp_wine_surface_Release,
    /* IWineDCompSurface methods */
    dcomp_wine_surface_get_physical_texture,
    dcomp_wine_surface_get_content_valid,
};

static const struct IDCompositionSurfaceVtbl dcomp_surface_vtbl =
{
    /* IUnknown methods */
    dcomp_surface_QueryInterface,
    dcomp_surface_AddRef,
    dcomp_surface_Release,
    /* IDCompositionSurface methods */
    dcomp_surface_BeginDraw,
    dcomp_surface_EndDraw,
    dcomp_surface_SuspendDraw,
    dcomp_surface_ResumeDraw,
    dcomp_surface_Scroll,
};

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_Resize(IDCompositionVirtualSurface *iface,
        UINT width, UINT height)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);
    ID3D11Texture2D *texture;
    ID3D11Resource *source_resource = NULL, *destination_resource = NULL;
    ID3D11DeviceContext *context;
    ID3D11Device *d3d11_device;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_BOX box;
    UINT copy_width, copy_height;
    HRESULT hr;

    TRACE("iface %p, width %u, height %u.\n", iface, width, height);

    if (!width || !height || width > 16384 || height > 16384)
        return E_INVALIDARG;
    EnterCriticalSection(&device->cs);
    if (device->drawing_surface == &surface->IDCompositionSurface_iface)
    {
        LeaveCriticalSection(&device->cs);
        return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
    }
    LeaveCriticalSection(&device->cs);

    if (FAILED(hr = dcomp_get_d3d11_device(surface->rendering_device,
            &surface->rendering_device_iid, &d3d11_device)))
        return hr;
    ID3D11Texture2D_GetDesc(surface->physical_surface, &desc);
    desc.Width = width;
    desc.Height = height;
    hr = ID3D11Device_CreateTexture2D(d3d11_device, &desc, NULL, &texture);
    if (SUCCEEDED(hr))
    {
        hr = ID3D11Texture2D_QueryInterface(texture, &IID_ID3D11Resource,
                (void **)&destination_resource);
        if (SUCCEEDED(hr))
            hr = ID3D11Texture2D_QueryInterface(surface->physical_surface, &IID_ID3D11Resource,
                    (void **)&source_resource);
        if (FAILED(hr))
        {
            if (source_resource) ID3D11Resource_Release(source_resource);
            if (destination_resource) ID3D11Resource_Release(destination_resource);
            ID3D11Texture2D_Release(texture);
            ID3D11Device_Release(d3d11_device);
            return hr;
        }
        copy_width = min(surface->width, width);
        copy_height = min(surface->height, height);
        ID3D11Device_GetImmediateContext(d3d11_device, &context);
        box.left = 0;
        box.top = 0;
        box.front = 0;
        box.right = copy_width;
        box.bottom = copy_height;
        box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(context, destination_resource, 0,
                0, 0, 0, source_resource, 0, &box);
        ID3D11DeviceContext_Release(context);
        ID3D11Resource_Release(source_resource);
        ID3D11Resource_Release(destination_resource);
        ID3D11Texture2D_Release(surface->physical_surface);
        surface->physical_surface = texture;
        surface->width = width;
        surface->height = height;
    }
    ID3D11Device_Release(d3d11_device);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_Trim(IDCompositionVirtualSurface *iface,
        const RECT *rectangles, UINT count)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    UINT i;

    TRACE("iface %p, rectangles %p, count %u.\n", iface, rectangles, count);

    if (count && !rectangles)
        return E_INVALIDARG;
    for (i = 0; i < count; ++i)
    {
        const RECT *rect = &rectangles[i];
        if (rect->left < 0 || rect->top < 0 || rect->right > (LONG)surface->width
                || rect->bottom > (LONG)surface->height || rect->left >= rect->right
                || rect->top >= rect->bottom)
            return E_INVALIDARG;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_QueryInterface(IDCompositionVirtualSurface *iface,
        REFIID iid, void **out)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_QueryInterface(&surface->IDCompositionSurface_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_virtual_surface_AddRef(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_AddRef(&surface->IDCompositionSurface_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_virtual_surface_Release(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_Release(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_BeginDraw(IDCompositionVirtualSurface *iface,
        const RECT *rect, REFIID iid, void **object, POINT *offset)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_BeginDraw(&surface->IDCompositionSurface_iface, rect, iid, object, offset);
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_EndDraw(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_EndDraw(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_SuspendDraw(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_SuspendDraw(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_ResumeDraw(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_ResumeDraw(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_Scroll(IDCompositionVirtualSurface *iface,
        const RECT *scroll, const RECT *clip, int offset_x, int offset_y)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return dcomp_surface_Scroll(&surface->IDCompositionSurface_iface, scroll, clip, offset_x, offset_y);
}

static const struct IDCompositionVirtualSurfaceVtbl dcomp_virtual_surface_vtbl =
{
    dcomp_virtual_surface_QueryInterface,
    dcomp_virtual_surface_AddRef,
    dcomp_virtual_surface_Release,
    dcomp_virtual_surface_BeginDraw,
    dcomp_virtual_surface_EndDraw,
    dcomp_virtual_surface_SuspendDraw,
    dcomp_virtual_surface_ResumeDraw,
    dcomp_virtual_surface_Scroll,
    dcomp_virtual_surface_Resize,
    dcomp_virtual_surface_Trim,
};

static HRESULT dcomp_surface_create(IDCompositionDevice *device, IUnknown *rendering_device,
        const GUID *rendering_device_iid, UINT width, UINT height, DXGI_FORMAT pixel_format,
        DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **surface)
{
    struct dcomp_surface *obj;
    ID3D11Device *d3d11_device;
    ID3D11Texture2D *texture;
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;

    if (!surface)
        return E_INVALIDARG;

    if (!width || !height)
        return E_INVALIDARG;

    if (pixel_format != DXGI_FORMAT_B8G8R8A8_UNORM && pixel_format != DXGI_FORMAT_R8G8B8A8_UNORM
            && pixel_format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return E_INVALIDARG;

    if (alpha_mode == DXGI_ALPHA_MODE_UNSPECIFIED)
        alpha_mode = DXGI_ALPHA_MODE_IGNORE;

    if (alpha_mode != DXGI_ALPHA_MODE_PREMULTIPLIED && alpha_mode != DXGI_ALPHA_MODE_IGNORE)
        return E_INVALIDARG;

    if (!rendering_device)
        return E_INVALIDARG;

    if (FAILED(hr = dcomp_get_d3d11_device(rendering_device, rendering_device_iid, &d3d11_device)))
        return hr;

    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = pixel_format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = (pixel_format == DXGI_FORMAT_B8G8R8A8_UNORM) ? D3D11_RESOURCE_MISC_GDI_COMPATIBLE : 0;

    hr = ID3D11Device_CreateTexture2D(d3d11_device, &desc, NULL, &texture);
    ID3D11Device_Release(d3d11_device);
    if (FAILED(hr))
    {
        ERR("Failed to create the surface texture, hr %#lx.\n", hr);
        return hr;
    }

    if (!(obj = calloc(1, sizeof(*obj))))
    {
        ID3D11Texture2D_Release(texture);
        return E_OUTOFMEMORY;
    }

    obj->IDCompositionSurface_iface.lpVtbl = &dcomp_surface_vtbl;
    obj->IDCompositionVirtualSurface_iface.lpVtbl = &dcomp_virtual_surface_vtbl;
    obj->IWineDCompSurface_iface.lpVtbl = &dcomp_wine_surface_vtbl;
    obj->refcount = 1;
    obj->device = device;
    IDCompositionDevice_AddRef(device);
    obj->rendering_device = rendering_device;
    IUnknown_AddRef(rendering_device);
    obj->rendering_device_iid = *rendering_device_iid;
    obj->physical_surface = texture;
    obj->width = width;
    obj->height = height;
    obj->pixel_format = pixel_format;
    obj->alpha_mode = alpha_mode;
    obj->drawn = FALSE;
    obj->virtual_surface = FALSE;
    obj->content_valid = FALSE;

    *surface = &obj->IDCompositionSurface_iface;
    return S_OK;
}

static HRESULT dcomp_virtual_surface_create(IDCompositionDevice *device, IUnknown *rendering_device,
        const GUID *rendering_device_iid, UINT width, UINT height, DXGI_FORMAT pixel_format,
        DXGI_ALPHA_MODE alpha_mode, IDCompositionVirtualSurface **surface)
{
    IDCompositionSurface *base_surface;
    struct dcomp_surface *obj;
    HRESULT hr;

    if (!surface)
        return E_INVALIDARG;
    if (width > 16384 || height > 16384)
        return E_NOTIMPL;
    if (FAILED(hr = dcomp_surface_create(device, rendering_device, rendering_device_iid,
            width, height, pixel_format, alpha_mode, &base_surface)))
        return hr;

    obj = impl_from_IDCompositionSurface(base_surface);
    obj->virtual_surface = TRUE;
    hr = IDCompositionSurface_QueryInterface(base_surface, &IID_IDCompositionVirtualSurface,
            (void **)surface);
    IDCompositionSurface_Release(base_surface);
    return hr;
}

static HRESULT dcomp_device_create_virtual_surface(struct dcomp_device *device, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionVirtualSurface **surface)
{
    TRACE("device %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            device, width, height, pixel_format, alpha_mode, surface);

    return dcomp_virtual_surface_create(&device->IDCompositionDevice_iface,
            device->rendering_device, &device->rendering_device_iid, width, height,
            pixel_format, alpha_mode, surface);
}

/* =====================================================================
 * IDCompositionSurfaceFactory
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_surface_factory_QueryInterface(IDCompositionSurfaceFactory *iface,
        REFIID iid, void **out)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionSurfaceFactory))
    {
        *out = &factory->IDCompositionSurfaceFactory_iface;
        IDCompositionSurfaceFactory_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_factory_AddRef(IDCompositionSurfaceFactory *iface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    ULONG refcount = InterlockedIncrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_factory_Release(IDCompositionSurfaceFactory *iface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        IUnknown_Release(factory->rendering_device);
        IDCompositionDevice_Release(factory->device);
        free(factory);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_factory_CreateSurface(IDCompositionSurfaceFactory *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    return dcomp_surface_create(factory->device, factory->rendering_device,
            &factory->rendering_device_iid, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_factory_CreateVirtualSurface(IDCompositionSurfaceFactory *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    return dcomp_virtual_surface_create(factory->device, factory->rendering_device,
            &factory->rendering_device_iid, width, height, pixel_format, alpha_mode, surface);
}

static const struct IDCompositionSurfaceFactoryVtbl dcomp_surface_factory_vtbl =
{
    /* IUnknown methods */
    dcomp_surface_factory_QueryInterface,
    dcomp_surface_factory_AddRef,
    dcomp_surface_factory_Release,
    /* IDCompositionSurfaceFactory methods */
    dcomp_surface_factory_CreateSurface,
    dcomp_surface_factory_CreateVirtualSurface,
};

static HRESULT dcomp_surface_factory_create(IDCompositionDevice *device, IUnknown *rendering_device,
        IDCompositionSurfaceFactory **factory)
{
    struct dcomp_surface_factory *obj;
    IUnknown *held_device;
    GUID held_iid;
    HRESULT hr;

    if (!factory)
        return E_INVALIDARG;

    if (!rendering_device)
        return E_INVALIDARG;

    if (FAILED(hr = dcomp_hold_rendering_device(rendering_device, &held_device, &held_iid)))
        return hr;

    if (!(obj = calloc(1, sizeof(*obj))))
    {
        IUnknown_Release(held_device);
        return E_OUTOFMEMORY;
    }

    obj->IDCompositionSurfaceFactory_iface.lpVtbl = &dcomp_surface_factory_vtbl;
    obj->refcount = 1;
    obj->device = device;
    IDCompositionDevice_AddRef(device);
    obj->rendering_device = held_device;
    obj->rendering_device_iid = held_iid;

    *factory = &obj->IDCompositionSurfaceFactory_iface;
    return S_OK;
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
        if (group->opacity_animation)
            IDCompositionAnimation_Release(group->opacity_animation);
        IDCompositionDevice_Release(&group->device->IDCompositionDevice_iface);
        free(group);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetOpacityAnimation(IDCompositionEffectGroup *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    struct dcomp_device *device = group->device;
    HRESULT hr;

    TRACE("iface %p, animation %p.\n", iface, animation);

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_check_for_property(animation, device)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    if (group->opacity_animation)
        IDCompositionAnimation_Release(group->opacity_animation);
    group->opacity_animation = animation;
    IDCompositionAnimation_AddRef(animation);
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetOpacity(IDCompositionEffectGroup *iface, float opacity)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    struct dcomp_device *device = group->device;
    if (!isfinite(opacity) || opacity < 0.0f || opacity > 1.0f) return E_INVALIDARG;
    EnterCriticalSection(&device->cs);
    group->opacity = opacity;
    /* A plain value replaces any attached animation. */
    if (group->opacity_animation)
    {
        IDCompositionAnimation_Release(group->opacity_animation);
        group->opacity_animation = NULL;
    }
    LeaveCriticalSection(&device->cs);
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
 * IDCompositionAnimation
 *
 * A keyframe animation: an ordered list of segments (cubic polynomial,
 * sinusoidal, repeat, end) over a relative timeline in seconds. The
 * absolute begin time is set with SetAbsoluteBeginTime (100 ns units).
 * The value is sampled once at Commit (dcomp_animation_value_at); there
 * is no per-frame animation engine.
 * ===================================================================== */

enum dcomp_animation_segment_type
{
    DCOMP_ANIM_SEGMENT_CUBIC,
    DCOMP_ANIM_SEGMENT_SINUSOIDAL,
    DCOMP_ANIM_SEGMENT_REPEAT,
    DCOMP_ANIM_SEGMENT_END,
};

struct dcomp_animation_segment
{
    enum dcomp_animation_segment_type type;
    double begin_offset;
    union
    {
        struct
        {
            float c0, c1, c2, c3;
        } cubic;
        struct
        {
            float bias, amplitude, frequency, phase;
        } sinusoidal;
        struct
        {
            double duration;
            int prev_index;
        } repeat;
        struct
        {
            float value;
        } end;
    };
};

struct dcomp_animation
{
    IDCompositionAnimation IDCompositionAnimation_iface;
    LONG refcount;
    struct dcomp_device *device;
    LARGE_INTEGER begin_time;
    struct dcomp_animation_segment *segments;
    unsigned int segment_count;
    unsigned int segment_capacity;
    double last_end;
    BOOL ended;
};

static inline struct dcomp_animation *impl_from_IDCompositionAnimation(IDCompositionAnimation *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_animation, IDCompositionAnimation_iface);
}

static HRESULT dcomp_animation_reserve(struct dcomp_animation *anim)
{
    unsigned int new_capacity;
    struct dcomp_animation_segment *new_segments;

    if (anim->segment_count < anim->segment_capacity)
        return S_OK;
    new_capacity = anim->segment_capacity ? anim->segment_capacity * 2 : 8;
    new_segments = realloc(anim->segments, new_capacity * sizeof(*new_segments));
    if (!new_segments)
        return E_OUTOFMEMORY;
    anim->segments = new_segments;
    anim->segment_capacity = new_capacity;
    return S_OK;
}

/* Caller holds the device lock. Appends a segment starting at `offset`;
 * `end_offset` is the earliest time the next segment may start. */
static HRESULT dcomp_animation_begin_segment_locked(struct dcomp_animation *anim,
        enum dcomp_animation_segment_type type, double offset, double end_offset)
{
    struct dcomp_animation_segment *seg;
    HRESULT hr;

    if (anim->ended)
        return E_INVALIDARG;
    if (!isfinite(offset) || offset < anim->last_end)
        return E_INVALIDARG;
    if (FAILED(hr = dcomp_animation_reserve(anim)))
        return hr;
    seg = &anim->segments[anim->segment_count++];
    seg->type = type;
    seg->begin_offset = offset;
    anim->last_end = end_offset;
    return S_OK;
}

static float dcomp_animation_eval_segment(const struct dcomp_animation_segment *seg, double time)
{
    double t = time - seg->begin_offset;

    switch (seg->type)
    {
    case DCOMP_ANIM_SEGMENT_CUBIC:
        return seg->cubic.c0 + seg->cubic.c1 * t + seg->cubic.c2 * t * t + seg->cubic.c3 * t * t * t;
    case DCOMP_ANIM_SEGMENT_SINUSOIDAL:
        return seg->sinusoidal.bias + seg->sinusoidal.amplitude *
                sin(seg->sinusoidal.frequency * t + seg->sinusoidal.phase);
    default:
        return 0.0f;
    }
}

/* Evaluate the animation at `time`, the elapsed seconds since the begin
 * time. Cubic/sinusoidal segments run until the next segment; a repeat
 * replays the previous segment; after End the value is clamped to the
 * end value. Caller holds the device lock. */
static float dcomp_animation_value_at(struct dcomp_animation *anim, double time)
{
    unsigned int i;

    if (!anim->segment_count)
        return 0.0f;

    if (time < anim->segments[0].begin_offset)
        return dcomp_animation_eval_segment(&anim->segments[0], anim->segments[0].begin_offset);

    for (i = 0; i < anim->segment_count; i++)
    {
        const struct dcomp_animation_segment *seg = &anim->segments[i];
        double seg_end;

        switch (seg->type)
        {
        case DCOMP_ANIM_SEGMENT_END:
            seg_end = INFINITY;
            break;
        case DCOMP_ANIM_SEGMENT_REPEAT:
            seg_end = seg->begin_offset + seg->repeat.duration;
            break;
        default:
            seg_end = (i + 1 < anim->segment_count) ? anim->segments[i + 1].begin_offset : INFINITY;
            break;
        }
        if (time < seg->begin_offset)
            break;
        if (time >= seg_end)
            continue;
        switch (seg->type)
        {
        case DCOMP_ANIM_SEGMENT_END:
            return seg->end.value;
        case DCOMP_ANIM_SEGMENT_REPEAT:
            {
                const struct dcomp_animation_segment *prev = &anim->segments[seg->repeat.prev_index];
                double prev_begin = prev->begin_offset;
                double prev_duration = (prev->type == DCOMP_ANIM_SEGMENT_REPEAT)
                        ? prev->repeat.duration : seg->begin_offset - prev_begin;
                double t = time - seg->begin_offset;
                if (prev_duration <= 0.0)
                    prev_duration = 1.0;
                return dcomp_animation_eval_segment(prev, prev_begin + fmod(t, prev_duration));
            }
        default:
            return dcomp_animation_eval_segment(seg, time);
        }
    }

    if (anim->ended)
        return anim->segments[anim->segment_count - 1].end.value;
    return 0.0f;
}

/* Validate that `animation` is one of ours, belongs to `device`, and has
 * been ended, so it may be attached to a property. Caller holds the device
 * lock (the ended flag is read under it). */
static HRESULT dcomp_animation_check_for_property(IDCompositionAnimation *animation,
        struct dcomp_device *device)
{
    struct dcomp_animation *anim;

    if (!animation)
        return E_INVALIDARG;
    if ((const void *)animation->lpVtbl != &dcomp_animation_vtbl)
        return E_NOTIMPL;
    anim = impl_from_IDCompositionAnimation(animation);
    if (anim->device != device)
        return E_INVALIDARG;
    if (!anim->ended)
        return E_INVALIDARG;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_animation_QueryInterface(IDCompositionAnimation *iface,
        REFIID iid, void **out)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionAnimation))
    {
        *out = &anim->IDCompositionAnimation_iface;
        IDCompositionAnimation_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_animation_AddRef(IDCompositionAnimation *iface)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    ULONG refcount = InterlockedIncrement(&anim->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_animation_Release(IDCompositionAnimation *iface)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    ULONG refcount = InterlockedDecrement(&anim->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        struct dcomp_device *device = anim->device;
        free(anim->segments);
        IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
        free(anim);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_animation_Reset(IDCompositionAnimation *iface)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    struct dcomp_device *device = anim->device;

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&device->cs);
    free(anim->segments);
    anim->segments = NULL;
    anim->segment_count = 0;
    anim->segment_capacity = 0;
    anim->last_end = 0.0;
    anim->ended = FALSE;
    anim->begin_time.QuadPart = 0;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_animation_SetAbsoluteBeginTime(IDCompositionAnimation *iface,
        LARGE_INTEGER time)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    struct dcomp_device *device = anim->device;

    TRACE("iface %p, time %lld.\n", iface, (LONGLONG)time.QuadPart);

    EnterCriticalSection(&device->cs);
    anim->begin_time = time;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_animation_AddCubic(IDCompositionAnimation *iface,
        double offset, float c0, float c1, float c2, float c3)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    struct dcomp_device *device = anim->device;
    struct dcomp_animation_segment *seg;
    HRESULT hr;

    TRACE("iface %p, offset %f, c0 %f, c1 %f, c2 %f, c3 %f.\n", iface, offset, c0, c1, c2, c3);

    if (!isfinite(offset) || !isfinite(c0) || !isfinite(c1) || !isfinite(c2) || !isfinite(c3))
        return E_INVALIDARG;

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_CUBIC, offset, offset)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    seg = &anim->segments[anim->segment_count - 1];
    seg->cubic.c0 = c0;
    seg->cubic.c1 = c1;
    seg->cubic.c2 = c2;
    seg->cubic.c3 = c3;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_animation_AddSinusoidal(IDCompositionAnimation *iface,
        double offset, float bias, float amplitude, float frequency, float phase)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    struct dcomp_device *device = anim->device;
    struct dcomp_animation_segment *seg;
    HRESULT hr;

    TRACE("iface %p, offset %f, bias %f, amplitude %f, frequency %f, phase %f.\n",
            iface, offset, bias, amplitude, frequency, phase);

    if (!isfinite(offset) || !isfinite(bias) || !isfinite(amplitude)
            || !isfinite(frequency) || !isfinite(phase))
        return E_INVALIDARG;

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_SINUSOIDAL,
            offset, offset)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    seg = &anim->segments[anim->segment_count - 1];
    seg->sinusoidal.bias = bias;
    seg->sinusoidal.amplitude = amplitude;
    seg->sinusoidal.frequency = frequency;
    seg->sinusoidal.phase = phase;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_animation_AddRepeat(IDCompositionAnimation *iface,
        double offset, double duration)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    struct dcomp_device *device = anim->device;
    struct dcomp_animation_segment *seg;
    HRESULT hr;

    TRACE("iface %p, offset %f, duration %f.\n", iface, offset, duration);

    if (!isfinite(offset) || !isfinite(duration) || duration < 0.0)
        return E_INVALIDARG;

    EnterCriticalSection(&device->cs);
    if (!anim->segment_count)
    {
        LeaveCriticalSection(&device->cs);
        return E_INVALIDARG;
    }
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_REPEAT,
            offset, offset + duration)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    seg = &anim->segments[anim->segment_count - 1];
    seg->repeat.duration = duration;
    seg->repeat.prev_index = (int)(anim->segment_count - 2);
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_animation_End(IDCompositionAnimation *iface,
        double offset, float value)
{
    struct dcomp_animation *anim = impl_from_IDCompositionAnimation(iface);
    struct dcomp_device *device = anim->device;
    struct dcomp_animation_segment *seg;
    HRESULT hr;

    TRACE("iface %p, offset %f, value %f.\n", iface, offset, value);

    if (!isfinite(offset) || !isfinite(value))
        return E_INVALIDARG;

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_END, offset, offset)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    seg = &anim->segments[anim->segment_count - 1];
    seg->end.value = value;
    anim->ended = TRUE;
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static const struct IDCompositionAnimationVtbl dcomp_animation_vtbl =
{
    dcomp_animation_QueryInterface,
    dcomp_animation_AddRef,
    dcomp_animation_Release,
    dcomp_animation_Reset,
    dcomp_animation_SetAbsoluteBeginTime,
    dcomp_animation_AddCubic,
    dcomp_animation_AddSinusoidal,
    dcomp_animation_AddRepeat,
    dcomp_animation_End,
};

static HRESULT dcomp_animation_create(struct dcomp_device *device, IDCompositionAnimation **out)
{
    struct dcomp_animation *anim;

    if (!out)
        return E_INVALIDARG;

    if (!(anim = calloc(1, sizeof(*anim))))
        return E_OUTOFMEMORY;

    anim->IDCompositionAnimation_iface.lpVtbl = &dcomp_animation_vtbl;
    anim->refcount = 1;
    anim->device = device;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    anim->begin_time.QuadPart = 0;
    anim->last_end = 0.0;
    anim->ended = FALSE;

    *out = &anim->IDCompositionAnimation_iface;
    return S_OK;
}

/* Sample the effect group's opacity at `now_100ns`, evaluating the attached
 * animation if present. Caller holds the device lock. */
static float dcomp_effect_group_sample_opacity(struct dcomp_effect_group *eg, LONGLONG now_100ns)
{
    if (eg->opacity_animation)
    {
        struct dcomp_animation *anim = impl_from_IDCompositionAnimation(eg->opacity_animation);
        double elapsed = (now_100ns - anim->begin_time.QuadPart) / 10000000.0;
        return dcomp_animation_value_at(anim, elapsed);
    }
    return eg->opacity;
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
    if (state->offset_x_animation)
    {
        IDCompositionAnimation_Release(state->offset_x_animation);
        state->offset_x_animation = NULL;
    }
    if (state->offset_y_animation)
    {
        IDCompositionAnimation_Release(state->offset_y_animation);
        state->offset_y_animation = NULL;
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
    if (dst->offset_x_animation)
        IDCompositionAnimation_AddRef(dst->offset_x_animation);
    if (dst->offset_y_animation)
        IDCompositionAnimation_AddRef(dst->offset_y_animation);
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
        (visual->device->version >= 2 && IsEqualGUID(iid, &IID_IDCompositionVisual2)))
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

        if (visual->pending_parent)
        {
            struct dcomp_visual **p = &visual->pending_parent->pending_children;
            while (*p && *p != visual)
                p = &(*p)->pending_next_sibling;
            if (*p)
                *p = visual->pending_next_sibling;
            visual->pending_parent = NULL;
            visual->pending_next_sibling = NULL;
        }

        while (visual->pending_children)
        {
            struct dcomp_visual *child = visual->pending_children;
            visual->pending_children = child->pending_next_sibling;
            child->pending_parent = NULL;
            child->pending_next_sibling = NULL;
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
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    struct dcomp_device *device = visual->device;
    HRESULT hr;

    TRACE("iface %p, animation %p.\n", iface, animation);

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_check_for_property(animation, device)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    if (visual->pending.offset_x_animation)
        IDCompositionAnimation_Release(visual->pending.offset_x_animation);
    visual->pending.offset_x_animation = animation;
    IDCompositionAnimation_AddRef(animation);
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetX_locked(IDCompositionVisual2 *iface, float offset_x)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, offset_x %f.\n", iface, offset_x);
    visual->pending.offset_x = offset_x;
    /* A plain value replaces any attached animation. */
    if (visual->pending.offset_x_animation)
    {
        IDCompositionAnimation_Release(visual->pending.offset_x_animation);
        visual->pending.offset_x_animation = NULL;
    }
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
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    struct dcomp_device *device = visual->device;
    HRESULT hr;

    TRACE("iface %p, animation %p.\n", iface, animation);

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_check_for_property(animation, device)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    if (visual->pending.offset_y_animation)
        IDCompositionAnimation_Release(visual->pending.offset_y_animation);
    visual->pending.offset_y_animation = animation;
    IDCompositionAnimation_AddRef(animation);
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetY_locked(IDCompositionVisual2 *iface, float offset_y)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, offset_y %f.\n", iface, offset_y);
    visual->pending.offset_y = offset_y;
    /* A plain value replaces any attached animation. */
    if (visual->pending.offset_y_animation)
    {
        IDCompositionAnimation_Release(visual->pending.offset_y_animation);
        visual->pending.offset_y_animation = NULL;
    }
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
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, interpolation_mode %d.\n", iface, interpolation_mode);
    if (interpolation_mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            && interpolation_mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR
            && interpolation_mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT)
        return E_INVALIDARG;
    EnterCriticalSection(&visual->device->cs);
    visual->pending.interpolation_mode = interpolation_mode;
    LeaveCriticalSection(&visual->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetBorderMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BORDER_MODE border_mode)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, border_mode %d.\n", iface, border_mode);
    if (border_mode != DCOMPOSITION_BORDER_MODE_SOFT && border_mode != DCOMPOSITION_BORDER_MODE_HARD
            && border_mode != DCOMPOSITION_BORDER_MODE_INHERIT) return E_INVALIDARG;
    EnterCriticalSection(&visual->device->cs);
    visual->pending.border_mode = border_mode;
    LeaveCriticalSection(&visual->device->cs);
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
    {
        void *dummy;
        if (FAILED(IUnknown_QueryInterface(content, &IID_IDXGISwapChain1, &dummy))
                && FAILED(IUnknown_QueryInterface(content, &IID_IDCompositionSurface, &dummy))
                && FAILED(IUnknown_QueryInterface(content, &IID_IWineDCompSwapChain, &dummy)))
        {
            return E_INVALIDARG;
        }
        IUnknown_Release((IUnknown *)dummy);
    }

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

    if (child->is_root || child->pending_parent)
        return E_INVALIDARG;

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

    if (visibility != DCOMPOSITION_BACKFACE_VISIBILITY_VISIBLE
            && visibility != DCOMPOSITION_BACKFACE_VISIBILITY_HIDDEN
            && visibility != DCOMPOSITION_BACKFACE_VISIBILITY_INHERIT)
        return E_INVALIDARG;

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

    if (!out)
        return E_INVALIDARG;

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
    visual->pending.interpolation_mode = DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT;
    visual->active.interpolation_mode = DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT;
    visual->pending.border_mode = visual->active.border_mode = DCOMPOSITION_BORDER_MODE_INHERIT;

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
        dcomp_target_release_aux_producer(target);
        if (target->pending_root)
        {
            target->pending_root->is_root = FALSE;
            IDCompositionVisual2_Release(&target->pending_root->IDCompositionVisual2_iface);
        }
        if (target->active_root)
            IDCompositionVisual2_Release(&target->active_root->IDCompositionVisual2_iface);

        list_remove(&target->entry);
        LeaveCriticalSection(&device->cs);

        AcquireSRWLockExclusive(&target_registration_lock);
        if (GetPropW(target->hwnd, target->topmost ? WINE_DCOMP_UNDERLAY_TOP_PROP
                : WINE_DCOMP_UNDERLAY_BOTTOM_PROP) == (HANDLE)1)
            RemovePropW(target->hwnd, target->topmost ? WINE_DCOMP_UNDERLAY_TOP_PROP
                    : WINE_DCOMP_UNDERLAY_BOTTOM_PROP);
        if (!GetPropW(target->hwnd, WINE_DCOMP_UNDERLAY_TOP_PROP)
                && !GetPropW(target->hwnd, WINE_DCOMP_UNDERLAY_BOTTOM_PROP))
            RemovePropW(target->hwnd, WINE_DCOMP_UNDERLAY_PROP);
        ReleaseSRWLockExclusive(&target_registration_lock);
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
        if (new_root->is_root) return E_INVALIDARG;
        new_root->is_root = TRUE;
        IDCompositionVisual2_AddRef(&new_root->IDCompositionVisual2_iface);
    }

    if (target->pending_root)
    {
        target->pending_root->is_root = FALSE;
        IDCompositionVisual2_Release(&target->pending_root->IDCompositionVisual2_iface);
    }

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
    const WCHAR *property = topmost ? WINE_DCOMP_UNDERLAY_TOP_PROP : WINE_DCOMP_UNDERLAY_BOTTOM_PROP;
    DWORD process;

    if (!hwnd || hwnd == GetDesktopWindow() || !out) return E_INVALIDARG;
    if (!GetWindowThreadProcessId(hwnd, &process)) return E_INVALIDARG;
    if (process != GetCurrentProcessId()) return E_ACCESSDENIED;

    if (!(target = calloc(1, sizeof(*target))))
        return E_OUTOFMEMORY;

    AcquireSRWLockExclusive(&target_registration_lock);
    if (GetPropW(hwnd, property))
    {
        ReleaseSRWLockExclusive(&target_registration_lock);
        free(target);
        return DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED;
    }
    if (!SetPropW(hwnd, property, (HANDLE)1))
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        ReleaseSRWLockExclusive(&target_registration_lock);
        free(target);
        return FAILED(hr) ? hr : E_FAIL;
    }
    if (!GetPropW(hwnd, WINE_DCOMP_UNDERLAY_PROP)
            && !SetPropW(hwnd, WINE_DCOMP_UNDERLAY_PROP, (HANDLE)1))
    {
        RemovePropW(hwnd, property);
        ReleaseSRWLockExclusive(&target_registration_lock);
        free(target);
        return E_OUTOFMEMORY;
    }
    ReleaseSRWLockExclusive(&target_registration_lock);

    /* A top-level window without a redirection bitmap shows only its composition content, transparent where the
     * content is: make it a sheet of glass, as WineExplorer's DwmAlpha gate in dwmapi allows (P7b). */
    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD))
    {
        static const MARGINS sheet = { -1, -1, -1, -1 };
        HRESULT (WINAPI *extend_frame)(HWND, const MARGINS *);
        HMODULE dwmapi;

        if ((dwmapi = LoadLibraryW(L"dwmapi.dll")))
        {
            if ((extend_frame = (void *)GetProcAddress(dwmapi, "DwmExtendFrameIntoClientArea")))
                extend_frame(hwnd, &sheet);
            FreeLibrary(dwmapi);
        }
    }

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

static void dcomp_visual_apply_commit(struct dcomp_visual *visual, LONGLONG now_100ns)
{
    struct dcomp_visual *child, **p;

    if (!visual) return;

    /* Detach while the old active content still owns its COM reference. */
    if (visual->active.content != visual->pending.content)
        dcomp_bind_content(visual->active.content, NULL, NULL, NULL);
    /* Apply visual state: pending -> active */
    dcomp_visual_state_copy(&visual->active, &visual->pending);

    /* Sample attached offset animations once at the commit reference time so
     * the retained-scene renderer uses the plain floats unchanged. */
    if (visual->active.offset_x_animation)
    {
        struct dcomp_animation *anim = impl_from_IDCompositionAnimation(visual->active.offset_x_animation);
        double elapsed = (now_100ns - anim->begin_time.QuadPart) / 10000000.0;
        visual->active.offset_x = dcomp_animation_value_at(anim, elapsed);
    }
    if (visual->active.offset_y_animation)
    {
        struct dcomp_animation *anim = impl_from_IDCompositionAnimation(visual->active.offset_y_animation);
        double elapsed = (now_100ns - anim->begin_time.QuadPart) / 10000000.0;
        visual->active.offset_y = dcomp_animation_value_at(anim, elapsed);
    }

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
            visual->active.opacity = dcomp_effect_group_sample_opacity(eg, now_100ns);
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

        dcomp_visual_apply_commit(child, now_100ns);
    }
}

static HRESULT dcomp_validate_pending(struct dcomp_visual *visual, unsigned int *contents,
        unsigned int *visuals, struct dcomp_visual **seen, IUnknown **content_device, enum DCOMPOSITION_OPACITY_MODE parent_mode,
        const D2D_MATRIX_3X2_F *parent_mat, unsigned int depth)
{
    struct dcomp_visual *child;
    IWineDCompSwapChain *binding;
    D2D_MATRIX_3X2_F loc_mat, cur_mat;
    HRESULT hr;
    enum DCOMPOSITION_OPACITY_MODE mode;
    unsigned int i;

    if (!visual) return S_OK;
    if (depth > WINE_DCOMP_MAX_LAYERS || *visuals == WINE_DCOMP_MAX_LAYERS) return E_INVALIDARG;
    for (i = 0; i < *visuals; ++i) if (seen[i] == visual) return E_NOTIMPL;
    seen[(*visuals)++] = visual;
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
        IDCompositionSurface *surface;
        if (++*contents > WINE_DCOMP_MAX_CONTENTS) return E_NOTIMPL;
        if (SUCCEEDED(IUnknown_QueryInterface(visual->pending.content,
                &IID_IDCompositionSurface, (void **)&surface)))
        {
            struct dcomp_surface *impl = impl_from_IDCompositionSurface(surface);
            IDCompositionSurface_Release(surface);
            /* Only B8G8R8A8 and R8G8B8A8 are composed; R16G16B16A16_FLOAT
             * stays rejected with E_NOTIMPL. */
            if (impl->pixel_format != DXGI_FORMAT_B8G8R8A8_UNORM
                    && impl->pixel_format != DXGI_FORMAT_R8G8B8A8_UNORM)
                return E_NOTIMPL;
        }
        else
        {
            IDXGISwapChain1 *swapchain;
            IUnknown *device;
            DXGI_SWAP_CHAIN_DESC1 desc;
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
    }

    for (child = visual->pending_children; child; child = child->pending_next_sibling)
        if (FAILED(hr = dcomp_validate_pending(child, contents, visuals, seen, content_device, mode, &cur_mat, depth + 1)))
            return hr;

    return S_OK;
}

struct dcomp_collect_context
{
    struct dcomp_target *target;
    struct wine_dcomp_target_scene scene;
};

static void dcomp_collect_children(struct dcomp_visual *child,
        const D2D_MATRIX_3X2_F *parent, float opacity, enum DCOMPOSITION_OPACITY_MODE mode,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation,
        enum DCOMPOSITION_BORDER_MODE border,
        LONGLONG now_100ns,
        struct dcomp_collect_context *ctx);

static void dcomp_collect_pending(struct dcomp_visual *visual,
        const D2D_MATRIX_3X2_F *parent, float opacity, enum DCOMPOSITION_OPACITY_MODE mode,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation,
        enum DCOMPOSITION_BORDER_MODE border,
        LONGLONG now_100ns,
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
    /* Sample attached offset animations once at the commit reference time. */
    if (state.offset_x_animation)
    {
        struct dcomp_animation *anim = impl_from_IDCompositionAnimation(state.offset_x_animation);
        double elapsed = (now_100ns - anim->begin_time.QuadPart) / 10000000.0;
        state.offset_x = dcomp_animation_value_at(anim, elapsed);
    }
    if (state.offset_y_animation)
    {
        struct dcomp_animation *anim = impl_from_IDCompositionAnimation(state.offset_y_animation);
        double elapsed = (now_100ns - anim->begin_time.QuadPart) / 10000000.0;
        state.offset_y = dcomp_animation_value_at(anim, elapsed);
    }
    if (state.effect)
        state.opacity = dcomp_effect_group_sample_opacity(
                impl_from_IDCompositionEffectGroup((IDCompositionEffectGroup *)state.effect), now_100ns);
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
    if (state.interpolation_mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT)
        interpolation = state.interpolation_mode;
    if (state.border_mode != DCOMPOSITION_BORDER_MODE_INHERIT) border = state.border_mode;

    /* Children are listed front to back; content is behind all children. */
    {
        layer = &ctx->scene.layers[ctx->scene.layer_count++];
        layer->content = state.content;
        layer->has_transform = TRUE;
        layer->m11 = matrix._11; layer->m12 = matrix._12;
        layer->m21 = matrix._21; layer->m22 = matrix._22;
        layer->m31 = matrix._31; layer->m32 = matrix._32;
        if (state.content)
        {
            IDCompositionSurface *surface;
            if (SUCCEEDED(IUnknown_QueryInterface(state.content, &IID_IDCompositionSurface, (void **)&surface)))
            {
                struct dcomp_surface *impl = impl_from_IDCompositionSurface(surface);
                layer->width = impl->width;
                layer->height = impl->height;
                layer->ignore_alpha = impl->alpha_mode == DXGI_ALPHA_MODE_IGNORE;
                IDCompositionSurface_Release(surface);
            }
            else if (SUCCEEDED(IUnknown_QueryInterface(state.content, &IID_IDXGISwapChain1, (void **)&sc)))
            {
                if (SUCCEEDED(IDXGISwapChain1_GetDesc1(sc, &desc)))
                {
                    layer->width = desc.Width;
                    layer->height = desc.Height;
                    layer->ignore_alpha = desc.AlphaMode == DXGI_ALPHA_MODE_IGNORE;
                }
                IDXGISwapChain1_Release(sc);
            }
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
        layer->interpolation_mode = interpolation;
        layer->soft_border = border == DCOMPOSITION_BORDER_MODE_SOFT;
    }
    dcomp_collect_children(visual->pending_children, &matrix, opacity, mode, interpolation, border,
            now_100ns, ctx);
    layer->subtree_end = ctx->scene.layer_count;
}

static void dcomp_collect_children(struct dcomp_visual *child,
        const D2D_MATRIX_3X2_F *parent, float opacity, enum DCOMPOSITION_OPACITY_MODE mode,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation,
        enum DCOMPOSITION_BORDER_MODE border,
        LONGLONG now_100ns,
        struct dcomp_collect_context *ctx)
{
    if (!child) return;
    dcomp_collect_children(child->pending_next_sibling, parent, opacity, mode, interpolation, border,
            now_100ns, ctx);
    dcomp_collect_pending(child, parent, opacity, mode, interpolation, border, now_100ns, ctx);
}

static HRESULT dcomp_retain_active_tree(struct dcomp_visual *visual, struct dcomp_visual **nodes, unsigned int *count)
{
    struct dcomp_visual *child;
    unsigned int i;
    HRESULT hr;

    if (!visual) return S_OK;
    for (i = 0; i < *count; ++i) if (nodes[i] == visual) return S_OK;
    if (*count == WINE_DCOMP_MAX_LAYERS) return E_UNEXPECTED;
    IDCompositionVisual2_AddRef(&visual->IDCompositionVisual2_iface);
    nodes[(*count)++] = visual;
    for (child = visual->active_children; child; child = child->active_next_sibling)
        if (FAILED(hr = dcomp_retain_active_tree(child, nodes, count))) return hr;
    return S_OK;
}

static HRESULT dcomp_device_internal_commit(struct dcomp_device *dev)
{
    struct dcomp_target *target;
    D2D_MATRIX_3X2_F ident = {1,0,0,1,0,0};
    struct dcomp_collect_context *contexts = NULL;
    IWineDCompSwapChain *bindings[WINE_DCOMP_MAX_CONTENTS];
    unsigned int binding_context[WINE_DCOMP_MAX_CONTENTS];
    /* Content IUnknown each binding was prepared from (no ref held); used to
     * dedupe duplicate producers by the underlying object, not the interface. */
    IUnknown *binding_content[WINE_DCOMP_MAX_CONTENTS];
    /* Targets whose auxiliary producer must be revoked, deferred until the
     * commit is known to succeed so a failed transaction keeps it intact. */
    struct dcomp_target *aux_release[WINE_DCOMP_MAX_CONTENTS];
    unsigned int aux_release_count = 0;
    struct dcomp_visual *pending_nodes[WINE_DCOMP_MAX_LAYERS], *old_nodes[WINE_DCOMP_MAX_LAYERS];
    unsigned int old_count = 0;
    unsigned int contents = 0, visuals = 0, targets = 0, count = 0, i, j, c = 0;
    IUnknown *content_device = NULL;
    /* Commit reference time (100 ns units); attached animations are sampled
     * once at this instant, not per frame. */
    FILETIME commit_time;
    LONGLONG now_100ns;
    HRESULT hr = S_OK;

    GetSystemTimeAsFileTime(&commit_time);
    now_100ns = ((LONGLONG)commit_time.dwHighDateTime << 32) | commit_time.dwLowDateTime;

    EnterCriticalSection(&dev->cs);
    LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
    {
        if (target->pending_root && ++targets > WINE_DCOMP_MAX_CONTENTS)
        {
            hr = E_NOTIMPL;
            goto done;
        }
    }
    if (targets && !(contexts = calloc(targets, sizeof(*contexts))))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
    {
        if (!target->pending_root) continue;
        if (!IsWindow(target->hwnd)) { hr = E_INVALIDARG; goto done; }
        for (j = 0; j < c; ++j)
            if (contexts[j].scene.target_hwnd == target->hwnd) { hr = E_NOTIMPL; goto done; }
        /* Distinct targets may use different D3D devices. A scene still cannot
         * mix devices until the backend provides a cross-device image route. */
        if (FAILED(hr = dcomp_validate_pending(target->pending_root, &contents, &visuals, pending_nodes,
                &content_device, DCOMPOSITION_OPACITY_MODE_LAYER, &ident, 0))) goto done;
        if (content_device) IUnknown_Release(content_device);
        content_device = NULL;
        contexts[c].target = target;
        contexts[c].scene.target_hwnd = target->hwnd;
        dcomp_collect_pending(target->pending_root, &ident, 1, DCOMPOSITION_OPACITY_MODE_LAYER,
                DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, DCOMPOSITION_BORDER_MODE_HARD,
                now_100ns, &contexts[c]);
        ++c;
    }
    /* Prepare ALL targets before changing active state. Keep one prepared scene
     * per producer, and reject ambiguous cross-target producer routing before
     * any active edits are applied (the current backend has one output HWND).
     * Surface layers carry no IWineDCompSwapChain of their own; a scene made up
     * only of them is presented through the target's auxiliary producer. */
    for (c = 0; c < targets; ++c)
    {
        const struct wine_dcomp_target_scene *scene = &contexts[c].scene;
        struct dcomp_target *target = contexts[c].target;
        struct dcomp_surface *first_surface = NULL;
        BOOL has_surface = FALSE, has_swapchain = FALSE;

        for (i = 0; i < scene->layer_count; ++i)
        {
            IWineDCompSwapChain *binding;
            IDCompositionSurface *surface;
            if (!scene->layers[i].content) continue;
            if (SUCCEEDED(IUnknown_QueryInterface(scene->layers[i].content,
                    &IID_IDCompositionSurface, (void **)&surface)))
            {
                struct dcomp_surface *impl = impl_from_IDCompositionSurface(surface);
                IDCompositionSurface_Release(surface);
                has_surface = TRUE;
                if (!first_surface)
                    first_surface = impl;
                continue;
            }
            if (FAILED(hr = IUnknown_QueryInterface(scene->layers[i].content,
                    &IID_IWineDCompSwapChain, (void **)&binding))) goto done;
            has_swapchain = TRUE;
            for (j = 0; j < count && binding_content[j] != scene->layers[i].content; ++j) {}
            if (j < count)
            {
                IWineDCompSwapChain_Release(binding);
                if (binding_context[j] != c) { hr = E_NOTIMPL; goto done; }
                continue;
            }
            binding_context[count] = c;
            binding_content[count] = scene->layers[i].content;
            bindings[count++] = binding;
            if (FAILED(hr = IWineDCompSwapChain_prepare_target_scene(binding, scene->target_hwnd, scene)))
                goto done;
        }

        if (has_surface && !has_swapchain)
        {
            ID3D11Device *layer_device;
            if (FAILED(hr = dcomp_target_ensure_aux_producer(target,
                    first_surface->rendering_device, &first_surface->rendering_device_iid)))
                goto done;
            /* dxgi rejects cross-device content: every surface layer must share
             * the auxiliary producer's device. */
            for (i = 0; i < scene->layer_count; ++i)
            {
                IDCompositionSurface *surface;
                if (!scene->layers[i].content) continue;
                if (FAILED(IUnknown_QueryInterface(scene->layers[i].content,
                        &IID_IDCompositionSurface, (void **)&surface)))
                    continue;
                {
                    struct dcomp_surface *impl = impl_from_IDCompositionSurface(surface);
                    IDCompositionSurface_Release(surface);
                    if (FAILED(hr = dcomp_get_d3d11_device(impl->rendering_device,
                            &impl->rendering_device_iid, &layer_device)))
                        goto done;
                    if (layer_device != target->aux_device)
                    {
                        ID3D11Device_Release(layer_device);
                        hr = E_NOTIMPL;
                        goto done;
                    }
                    ID3D11Device_Release(layer_device);
                }
            }
            /* Present the scene through the auxiliary producer exactly like an
             * ordinary binding. */
            binding_context[count] = c;
            binding_content[count] = NULL; /* The aux producer has no content IUnknown. */
            bindings[count] = target->aux_binding;
            IWineDCompSwapChain_AddRef(target->aux_binding);
            ++count;
            if (FAILED(hr = IWineDCompSwapChain_prepare_target_scene(target->aux_binding,
                    scene->target_hwnd, scene)))
                goto done;
        }
        else if (target->aux_binding)
        {
            /* The scene has a real producer (or no content): the auxiliary
             * producer is revoked, but only once the commit is known to succeed. */
            aux_release[aux_release_count++] = target;
        }
    }
    /* A target whose scene went away keeps no auxiliary producer (deferred). */
    LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
        if (!target->pending_root && target->aux_binding)
            aux_release[aux_release_count++] = target;
    /* Retain the old active tree before any activation so its nodes stay alive
     * while the new graph is built; a failed commit releases these extra refs
     * and leaves the old tree untouched. */
    LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
        if (FAILED(hr = dcomp_retain_active_tree(target->active_root, old_nodes, &old_count))) goto done;
    /* Activate every producer before switching the visual tree, so a failure
     * leaves the previous scene and tree intact. */
    for (i = 0; i < count; ++i)
    {
        const struct wine_dcomp_target_scene *scene = &contexts[binding_context[i]].scene;
        if (FAILED(hr = IWineDCompSwapChain_set_target_scene(bindings[i], scene->target_hwnd, scene)))
        {
            /* Keep the commit all-or-nothing: revoke the bindings already
             * activated so none of the new scenes are published. */
            for (j = 0; j < i; ++j)
                IWineDCompSwapChain_set_target_scene(bindings[j], NULL, NULL);
            break;
        }
    }
    if (SUCCEEDED(hr))
    {
        /* Reparenting must not reuse active sibling links while a different old
         * parent still traverses them. Remove ALL old edges, then build the new
         * graph. Pending ownership alone cannot retain detached old-only nodes
         * until their bindings have been revoked. */
        for (i = 0; i < old_count; ++i)
        {
            struct dcomp_visual *visual = old_nodes[i];
            for (j = 0; j < visuals && pending_nodes[j] != visual; ++j) {}
            if (j == visuals) dcomp_bind_content(visual->active.content, NULL, NULL, NULL);
            while (visual->active_children)
            {
                struct dcomp_visual *child = visual->active_children;
                visual->active_children = child->active_next_sibling;
                child->active_parent = NULL;
                child->active_next_sibling = NULL;
                IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
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
            if (target->active_root) dcomp_visual_apply_commit(target->active_root, now_100ns);
        }
        /* The commit succeeded: now revoke the auxiliary producers that are no
         * longer needed. */
        for (i = 0; i < aux_release_count; ++i)
            dcomp_target_release_aux_producer(aux_release[i]);
    }
done:
    for (i = 0; i < old_count; ++i) IDCompositionVisual2_Release(&old_nodes[i]->IDCompositionVisual2_iface);
    for (i = 0; i < count; ++i)
    {
        IWineDCompSwapChain_prepare_target_scene(bindings[i], NULL, NULL);
        IWineDCompSwapChain_Release(bindings[i]);
    }
    if (content_device) IUnknown_Release(content_device);
    free(contexts);
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
    else if (dev->version >= 2 && IsEqualGUID(iid, &IID_IDCompositionDevice2))
    {
        *out = &dev->IDCompositionDevice2_iface;
        IDCompositionDevice_AddRef(iface);
        return S_OK;
    }
    else if (dev->version >= 2 && IsEqualGUID(iid, &IID_IDCompositionDesktopDevice))
    {
        *out = &dev->IDCompositionDesktopDevice_iface;
        IDCompositionDevice_AddRef(iface);
        return S_OK;
    }
    else if (dev->version >= 3 && IsEqualGUID(iid, &IID_IDCompositionDevice3))
    {
        *out = &dev->IDCompositionDevice3_iface;
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
        if (dev->rendering_device)
            IUnknown_Release(dev->rendering_device);
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

    if (!target || !hwnd || hwnd == GetDesktopWindow())
        return E_INVALIDARG;

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
        return E_INVALIDARG;

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
    struct dcomp_device *dev = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);
    return dcomp_device_create_virtual_surface(dev, width, height, pixel_format, alpha_mode, surface);
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
    TRACE("iface %p, animation %p.\n", iface, animation);
    return dcomp_animation_create(impl_from_IDCompositionDevice(iface), animation);
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
        return E_INVALIDARG;

    if (SUCCEEDED(hr = dcomp_visual_create(dev, &obj)))
        *visual = &obj->IDCompositionVisual2_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateSurfaceFactory(IDCompositionDevice2 *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);

    TRACE("iface %p, rendering_device %p, surface_factory %p.\n", iface, rendering_device,
            surface_factory);

    return dcomp_surface_factory_create(&dev->IDCompositionDevice_iface, rendering_device,
            surface_factory);
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateSurface(IDCompositionDevice2 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    return dcomp_surface_create(&dev->IDCompositionDevice_iface, dev->rendering_device,
            &dev->rendering_device_iid, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device2_CreateVirtualSurface(IDCompositionDevice2 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice2(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);
    return dcomp_device_create_virtual_surface(dev, width, height, pixel_format, alpha_mode, surface);
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
    TRACE("iface %p, animation %p.\n", iface, animation);
    return dcomp_animation_create(impl_from_IDCompositionDevice2(iface), animation);
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
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);

    TRACE("iface %p, rendering_device %p, surface_factory %p.\n", iface, rendering_device,
            surface_factory);

    return dcomp_surface_factory_create(&dev->IDCompositionDevice_iface, rendering_device,
            surface_factory);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSurface(IDCompositionDevice3 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    return dcomp_surface_create(&dev->IDCompositionDevice_iface, dev->rendering_device,
            &dev->rendering_device_iid, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVirtualSurface(IDCompositionDevice3 *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);
    return dcomp_device_create_virtual_surface(dev, width, height, pixel_format, alpha_mode, surface);
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
    TRACE("iface %p, animation %p.\n", iface, animation);
    return dcomp_animation_create(impl_from_IDCompositionDevice3(iface), animation);
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
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, rendering_device %p, surface_factory %p.\n", iface, rendering_device,
            surface_factory);

    return dcomp_surface_factory_create(&dev->IDCompositionDevice_iface, rendering_device,
            surface_factory);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    return dcomp_surface_create(&dev->IDCompositionDevice_iface, dev->rendering_device,
            &dev->rendering_device_iid, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateVirtualSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);
    return dcomp_device_create_virtual_surface(dev, width, height, pixel_format, alpha_mode, surface);
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
    TRACE("iface %p, animation %p.\n", iface, animation);
    return dcomp_animation_create(impl_from_IDCompositionDesktopDevice(iface), animation);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTargetForHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    struct dcomp_target *obj;
    HRESULT hr;

    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);

    if (!target || !hwnd || hwnd == GetDesktopWindow())
        return E_INVALIDARG;

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

static HRESULT dcomp_device_create(int version, IUnknown *rendering_device, REFIID iid, void **device)
{
    struct dcomp_device *dev;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;

    if (!(dev = calloc(1, sizeof(*dev))))
        return E_OUTOFMEMORY;

    dev->IDCompositionDevice_iface.lpVtbl = &dcomp_device_vtbl;
    dev->IDCompositionDevice2_iface.lpVtbl = &dcomp_device2_vtbl;
    dev->IDCompositionDevice3_iface.lpVtbl = &dcomp_device3_vtbl;
    dev->IDCompositionDesktopDevice_iface.lpVtbl = &dcomp_desktop_device_vtbl;
    dev->version = version;
    dev->refcount = 1;
    list_init(&dev->targets);
    InitializeCriticalSection(&dev->cs);

    if (rendering_device)
        dcomp_hold_rendering_device(rendering_device, &dev->rendering_device, &dev->rendering_device_iid);

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

    if (!device)
        return E_INVALIDARG;

    if (!IsEqualIID(iid, &IID_IDCompositionDevice))
        return E_NOINTERFACE;

    return dcomp_device_create(1, (IUnknown *)dxgi_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("rendering_device %p, iid %s, device %p.\n", rendering_device, debugstr_guid(iid), device);

    if (!device)
        return E_INVALIDARG;

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return dcomp_device_create(2, rendering_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("rendering_device %p, iid %s, device %p.\n", rendering_device, debugstr_guid(iid), device);

    if (!device)
        return E_INVALIDARG;

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return dcomp_device_create(3, rendering_device, iid, device);
}
