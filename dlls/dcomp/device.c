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
#include "wine/exception.h"
#include "wine/list.h"
#include "wine/dcomp_layer.h"
#include "wine/dcomp_underlay.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

struct dcomp_device;
struct dcomp_target;
struct dcomp_visual;

static void dcomp_visual_unbind_tree(struct dcomp_visual *visual);
/* the vtable of the visuals: the public methods, then the private slots from index 22 */
struct dcomp_visual_ext_vtbl
{
    struct IDCompositionVisual2Vtbl base;
    void *private_methods[42];   /* slots 22 to 63 */
};
static const struct dcomp_visual_ext_vtbl dcomp_visual_vtbl;
static const struct IDCompositionAnimationVtbl dcomp_animation_vtbl;
struct dcomp_animation;
static inline struct dcomp_animation *impl_from_IDCompositionAnimation(IDCompositionAnimation *iface);
static HRESULT dcomp_animation_check_for_property(IDCompositionAnimation *animation,
        struct dcomp_device *device);
struct dcomp_transform;
struct dcomp_animated_float;
static inline struct dcomp_transform *impl_from_IDCompositionTransform(IDCompositionTransform *iface);
static float dcomp_animated_float_sample(const struct dcomp_animated_float *prop, LONGLONG now_100ns);
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

/* A plain float property that may be driven by an attached animation. When an
 * animation is present it is sampled once at Commit (dcomp_animated_float_sample);
 * setting the plain value replaces the animation, as Windows does. */
struct dcomp_animated_float
{
    float value;
    IDCompositionAnimation *animation;
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
    IDCompositionAnimation *left_animation, *top_animation, *right_animation, *bottom_animation;
    IDCompositionAnimation *top_left_radius_x_animation, *top_left_radius_y_animation;
    IDCompositionAnimation *top_right_radius_x_animation, *top_right_radius_y_animation;
    IDCompositionAnimation *bottom_left_radius_x_animation, *bottom_left_radius_y_animation;
    IDCompositionAnimation *bottom_right_radius_x_animation, *bottom_right_radius_y_animation;
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
    /* Transform object attached with SetTransformObject. Not referenced (the
     * caller keeps it alive, matching Windows); its matrix is sampled at Commit. */
    IDCompositionTransform *transform_obj;
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
    BOOL shared;   /* listed in dcomp_shared_list: the list holds no reference, Release takes the list lock first */

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
};

enum dcomp_transform_type
{
    DCOMP_TRANSFORM_TRANSLATE,
    DCOMP_TRANSFORM_SCALE,
    DCOMP_TRANSFORM_ROTATE,
    DCOMP_TRANSFORM_SKEW,
    DCOMP_TRANSFORM_MATRIX,
};

/* A 2D transform object (IDCompositionTransform and derived). All five types
 * share this struct; only the fields matching `type` are meaningful. Each
 * property is a plain float plus an optional animation sampled at Commit. */
struct dcomp_transform
{
    IDCompositionTransform IDCompositionTransform_iface;
    LONG refcount;
    struct dcomp_device *device;
    enum dcomp_transform_type type;
    struct dcomp_animated_float offset_x, offset_y;   /* translate */
    struct dcomp_animated_float scale_x, scale_y;     /* scale */
    struct dcomp_animated_float center_x, center_y;   /* scale / rotate / skew */
    struct dcomp_animated_float angle;                /* rotate */
    struct dcomp_animated_float angle_x, angle_y;     /* skew */
    struct dcomp_animated_float matrix[6];            /* matrix: m11,m12,m21,m22,m31,m32 */
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
    const void *const *commit_iface;   /* the interface {6b556968-...}: dcomp_commit_vtbl */
    HANDLE commit_event;
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

typedef struct IDCompositionSurfaceUnknown IDCompositionSurfaceUnknown;
typedef struct IDCompositionSurfaceUnknownVtbl IDCompositionSurfaceUnknownVtbl;

/* Based on Wine-Staging patch 0063 by Zhiyi Zhang. */

struct IDCompositionSurfaceUnknown
{
    const IDCompositionSurfaceUnknownVtbl *lpVtbl;
};

struct IDCompositionSurfaceUnknownVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompositionSurfaceUnknown *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompositionSurfaceUnknown *);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompositionSurfaceUnknown *);
    HRESULT (STDMETHODCALLTYPE *BeginDraw)(IDCompositionSurfaceUnknown *, const RECT *, REFIID, void **, POINT *);
    HRESULT (STDMETHODCALLTYPE *EndDraw)(IDCompositionSurfaceUnknown *);
    HRESULT (STDMETHODCALLTYPE *SuspendDraw)(IDCompositionSurfaceUnknown *);
    HRESULT (STDMETHODCALLTYPE *ResumeDraw)(IDCompositionSurfaceUnknown *);
    HRESULT (STDMETHODCALLTYPE *Scroll)(IDCompositionSurfaceUnknown *, const RECT *, const RECT *, int, int);
    HRESULT (STDMETHODCALLTYPE *Unknown[99])();
};

static const GUID IID_IDCompositionSurfaceUnknown =
        {0x5680c979, 0x14c8, 0x48b5, {0x8b, 0x92, 0x32, 0x47, 0x37, 0x9f, 0xd9, 0x07}};

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

static inline struct dcomp_surface *impl_from_IDCompositionSurfaceUnknown(IDCompositionSurfaceUnknown *iface)
{
    return CONTAINING_RECORD((IDCompositionSurface *)iface, struct dcomp_surface, IDCompositionSurface_iface);
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
    else if (IsEqualGUID(iid, &IID_IDCompositionSurfaceUnknown))
    {
        *out = iface;
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

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_QueryInterface(IDCompositionSurfaceUnknown *iface,
        REFIID iid, void **out)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_QueryInterface(&surface->IDCompositionSurface_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_surface_unknown_AddRef(IDCompositionSurfaceUnknown *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_AddRef(&surface->IDCompositionSurface_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_surface_unknown_Release(IDCompositionSurfaceUnknown *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_Release(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_BeginDraw(IDCompositionSurfaceUnknown *iface,
        const RECT *rect, REFIID iid, void **object, POINT *offset)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_BeginDraw(&surface->IDCompositionSurface_iface, rect, iid, object, offset);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_EndDraw(IDCompositionSurfaceUnknown *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_EndDraw(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_SuspendDraw(IDCompositionSurfaceUnknown *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_SuspendDraw(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_ResumeDraw(IDCompositionSurfaceUnknown *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_ResumeDraw(&surface->IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_Scroll(IDCompositionSurfaceUnknown *iface,
        const RECT *scroll, const RECT *clip, int offset_x, int offset_y)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    return dcomp_surface_Scroll(&surface->IDCompositionSurface_iface, scroll, clip, offset_x, offset_y);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_Stub(IDCompositionSurfaceUnknown *iface)
{
    FIXME("iface %p stub.\n", iface);
    return E_NOTIMPL;
}

static HRESULT dcomp_surface_resize(struct dcomp_surface *surface, UINT width, UINT height)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(surface->device);
    ID3D11Texture2D *texture;
    ID3D11Resource *source_resource = NULL, *destination_resource = NULL;
    ID3D11DeviceContext *context;
    ID3D11Device *d3d11_device;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_BOX box;
    UINT copy_width, copy_height;
    HRESULT hr;

    if (!width || !height || width > 16384 || height > 16384)
        return E_INVALIDARG;

    /* the whole resize under the device lock: a committing thread reads physical_surface and the size */
    EnterCriticalSection(&device->cs);
    if (device->drawing_surface)
    {
        LeaveCriticalSection(&device->cs);
        return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
    }
    if (width == surface->width && height == surface->height)
    {
        LeaveCriticalSection(&device->cs);
        return S_OK;
    }

    if (FAILED(hr = dcomp_get_d3d11_device(surface->rendering_device,
            &surface->rendering_device_iid, &d3d11_device)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
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
        if (SUCCEEDED(hr))
        {
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
        }
        if (source_resource) ID3D11Resource_Release(source_resource);
        if (destination_resource) ID3D11Resource_Release(destination_resource);
        if (SUCCEEDED(hr))
        {
            ID3D11Texture2D_Release(surface->physical_surface);
            surface->physical_surface = texture;
            surface->width = width;
            surface->height = height;
            surface->drawn = FALSE;
            surface->content_valid = FALSE;
            surface->suspended = FALSE;
            SetRectEmpty(&surface->draw_rect);
        }
        else
            ID3D11Texture2D_Release(texture);
    }
    LeaveCriticalSection(&device->cs);
    ID3D11Device_Release(d3d11_device);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_unknown_Unknown3(IDCompositionSurfaceUnknown *iface,
        UINT width, UINT height)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurfaceUnknown(iface);
    TRACE("iface %p, width %u, height %u.\n", iface, width, height);
    return dcomp_surface_resize(surface, width, height);
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

static const struct IDCompositionSurfaceUnknownVtbl dcomp_surface_unknown_vtbl =
{
    dcomp_surface_unknown_QueryInterface,
    dcomp_surface_unknown_AddRef,
    dcomp_surface_unknown_Release,
    dcomp_surface_unknown_BeginDraw,
    dcomp_surface_unknown_EndDraw,
    dcomp_surface_unknown_SuspendDraw,
    dcomp_surface_unknown_ResumeDraw,
    dcomp_surface_unknown_Scroll,
    .Unknown =
    {
        [0 ... 98] = dcomp_surface_unknown_Stub,
        [2] = (HRESULT (STDMETHODCALLTYPE *)())dcomp_surface_unknown_Unknown3,
    },
};

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_Resize(IDCompositionVirtualSurface *iface,
        UINT width, UINT height)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);

    TRACE("iface %p, width %u, height %u.\n", iface, width, height);
    return dcomp_surface_resize(surface, width, height);
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
    /* Native Windows exposes the surface texture as a render target only; the CPU composition copy uses
     * ID3D11DeviceContext_CopyResource, which does not require the shader resource bind flag. */
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
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

    obj->IDCompositionSurface_iface.lpVtbl = (const struct IDCompositionSurfaceVtbl *)&dcomp_surface_unknown_vtbl;
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
    /* Windows validates only finiteness (values outside [0,1] are accepted). */
    if (!isfinite(opacity)) return E_INVALIDARG;
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

static void dcomp_rectangle_clip_release_animations(struct dcomp_rectangle_clip *clip)
{
    IDCompositionAnimation *animations[12] =
    {
        clip->left_animation, clip->top_animation, clip->right_animation, clip->bottom_animation,
        clip->top_left_radius_x_animation, clip->top_left_radius_y_animation,
        clip->top_right_radius_x_animation, clip->top_right_radius_y_animation,
        clip->bottom_left_radius_x_animation, clip->bottom_left_radius_y_animation,
        clip->bottom_right_radius_x_animation, clip->bottom_right_radius_y_animation,
    };
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(animations); i++)
        if (animations[i])
            IDCompositionAnimation_Release(animations[i]);
}

static ULONG STDMETHODCALLTYPE dcomp_rectangle_clip_Release(IDCompositionRectangleClip *iface)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    ULONG refcount = InterlockedDecrement(&clip->refcount);
    if (!refcount)
    {
        dcomp_rectangle_clip_release_animations(clip);
        IDCompositionDevice_Release(&clip->device->IDCompositionDevice_iface);
        free(clip);
    }
    return refcount;
}

/* Windows validates only finiteness for these setters (no range, negative
 * radii allowed); a plain value replaces any attached animation. */
static HRESULT dcomp_rectangle_clip_set_value(struct dcomp_rectangle_clip *clip,
        float *field, IDCompositionAnimation **animation_field, float value)
{
    if (!isfinite(value)) return E_INVALIDARG;
    EnterCriticalSection(&clip->device->cs);
    *field = value;
    if (*animation_field)
    {
        IDCompositionAnimation_Release(*animation_field);
        *animation_field = NULL;
    }
    LeaveCriticalSection(&clip->device->cs);
    return S_OK;
}

static HRESULT dcomp_rectangle_clip_set_animation(struct dcomp_rectangle_clip *clip,
        IDCompositionAnimation **animation_field, IDCompositionAnimation *animation)
{
    struct dcomp_device *device = clip->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_check_for_property(animation, device)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    if (*animation_field)
        IDCompositionAnimation_Release(*animation_field);
    *animation_field = animation;
    IDCompositionAnimation_AddRef(animation);
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetLeftAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->left_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetLeft(IDCompositionRectangleClip *iface, float left)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->left, &clip->left_animation, left);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->top_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTop(IDCompositionRectangleClip *iface, float top)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top, &clip->top_animation, top);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetRightAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->right_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetRight(IDCompositionRectangleClip *iface, float right)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->right, &clip->right_animation, right);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->bottom_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottom(IDCompositionRectangleClip *iface, float bottom)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom, &clip->bottom_animation, bottom);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->top_left_radius_x_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_left_radius_x, &clip->top_left_radius_x_animation, r);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->top_left_radius_y_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopLeftRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_left_radius_y, &clip->top_left_radius_y_animation, r);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->top_right_radius_x_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_right_radius_x, &clip->top_right_radius_x_animation, r);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->top_right_radius_y_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetTopRightRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->top_right_radius_y, &clip->top_right_radius_y_animation, r);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->bottom_left_radius_x_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_left_radius_x, &clip->bottom_left_radius_x_animation, r);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->bottom_left_radius_y_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomLeftRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_left_radius_y, &clip->bottom_left_radius_y_animation, r);
}

static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusXAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->bottom_right_radius_x_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusX(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_right_radius_x, &clip->bottom_right_radius_x_animation, r);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusYAnimation(IDCompositionRectangleClip *iface, IDCompositionAnimation *animation)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_animation(clip, &clip->bottom_right_radius_y_animation, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rectangle_clip_SetBottomRightRadiusY(IDCompositionRectangleClip *iface, float r)
{
    struct dcomp_rectangle_clip *clip = impl_from_IDCompositionRectangleClip(iface);
    return dcomp_rectangle_clip_set_value(clip, &clip->bottom_right_radius_y, &clip->bottom_right_radius_y_animation, r);
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
    /* Offset of the last added segment (0 if none). Segments must be added in
     * non-decreasing offset order; a repeat must start strictly after it. */
    double last_offset;
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

/* Caller holds the device lock. Appends a segment starting at `offset`.
 * Segments must be added in non-decreasing offset order. */
static HRESULT dcomp_animation_begin_segment_locked(struct dcomp_animation *anim,
        enum dcomp_animation_segment_type type, double offset)
{
    struct dcomp_animation_segment *seg;
    HRESULT hr;

    if (anim->ended)
        return E_INVALIDARG;
    if (!isfinite(offset) || offset < anim->last_offset)
        return E_INVALIDARG;
    if (FAILED(hr = dcomp_animation_reserve(anim)))
        return hr;
    seg = &anim->segments[anim->segment_count++];
    seg->type = type;
    seg->begin_offset = offset;
    anim->last_offset = offset;
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
    anim->last_offset = 0.0;
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
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_CUBIC, offset)))
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
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_SINUSOIDAL, offset)))
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

    /* A repeat must start strictly after the previous segment, have a positive
     * duration, and may not last longer than its start offset. */
    if (!isfinite(offset) || !isfinite(duration) || duration <= 0.0 || duration > offset)
        return E_INVALIDARG;

    EnterCriticalSection(&device->cs);
    if (!anim->segment_count || offset <= anim->last_offset)
    {
        LeaveCriticalSection(&device->cs);
        return E_INVALIDARG;
    }
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_REPEAT, offset)))
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
    if (FAILED(hr = dcomp_animation_begin_segment_locked(anim, DCOMP_ANIM_SEGMENT_END, offset)))
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
    anim->last_offset = 0.0;
    anim->ended = FALSE;

    *out = &anim->IDCompositionAnimation_iface;
    return S_OK;
}

/* The value of a property at `now_100ns`: its animation's value when one is attached, else the plain value.
 * Caller holds the device lock. */
static float dcomp_sample_animation(IDCompositionAnimation *animation, float value, LONGLONG now_100ns)
{
    struct dcomp_animation *anim;

    if (!animation) return value;
    anim = impl_from_IDCompositionAnimation(animation);
    return dcomp_animation_value_at(anim, (now_100ns - anim->begin_time.QuadPart) / 10000000.0);
}

/* Sample one clip property (plain value or attached animation) at
 * `now_100ns`. Caller holds the device lock. */
static float dcomp_clip_sample_value(struct dcomp_rectangle_clip *clip, float value,
        IDCompositionAnimation *animation, LONGLONG now_100ns)
{
    return dcomp_sample_animation(animation, value, now_100ns);
}

/* Sample all clip properties at `now_100ns` into the visual state's clip and
 * radius fields. Caller holds the device lock. */
static void dcomp_rectangle_clip_sample(struct dcomp_rectangle_clip *clip, LONGLONG now_100ns,
        struct dcomp_visual_state *state)
{
    state->has_clip = TRUE;
    state->clip.left = dcomp_clip_sample_value(clip, clip->left, clip->left_animation, now_100ns);
    state->clip.top = dcomp_clip_sample_value(clip, clip->top, clip->top_animation, now_100ns);
    state->clip.right = dcomp_clip_sample_value(clip, clip->right, clip->right_animation, now_100ns);
    state->clip.bottom = dcomp_clip_sample_value(clip, clip->bottom, clip->bottom_animation, now_100ns);
    state->radius_tl_x = dcomp_clip_sample_value(clip, clip->top_left_radius_x, clip->top_left_radius_x_animation, now_100ns);
    state->radius_tl_y = dcomp_clip_sample_value(clip, clip->top_left_radius_y, clip->top_left_radius_y_animation, now_100ns);
    state->radius_tr_x = dcomp_clip_sample_value(clip, clip->top_right_radius_x, clip->top_right_radius_x_animation, now_100ns);
    state->radius_tr_y = dcomp_clip_sample_value(clip, clip->top_right_radius_y, clip->top_right_radius_y_animation, now_100ns);
    state->radius_bl_x = dcomp_clip_sample_value(clip, clip->bottom_left_radius_x, clip->bottom_left_radius_x_animation, now_100ns);
    state->radius_bl_y = dcomp_clip_sample_value(clip, clip->bottom_left_radius_y, clip->bottom_left_radius_y_animation, now_100ns);
    state->radius_br_x = dcomp_clip_sample_value(clip, clip->bottom_right_radius_x, clip->bottom_right_radius_x_animation, now_100ns);
    state->radius_br_y = dcomp_clip_sample_value(clip, clip->bottom_right_radius_y, clip->bottom_right_radius_y_animation, now_100ns);
}

/* Sample the effect group's opacity at `now_100ns`, evaluating the attached
 * animation if present. Caller holds the device lock. */
static float dcomp_effect_group_sample_opacity(struct dcomp_effect_group *eg, LONGLONG now_100ns)
{
    return dcomp_sample_animation(eg->opacity_animation, eg->opacity, now_100ns);
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
    /* transform_obj is not referenced (native parity); just drop the pointer. */
    state->transform_obj = NULL;
}

/* =====================================================================
 * IDCompositionTransform and derived 2D transform objects
 *
 * All five 2D transform types (translate, scale, rotate, skew, matrix)
 * share one struct and per-type vtables. Each property is a plain float
 * plus an optional animation, sampled once at Commit (like the visual
 * offset animations). A visual that uses a transform object does not hold
 * a reference to it (native parity); the caller keeps it alive.
 * ===================================================================== */

static inline struct dcomp_transform *impl_from_IDCompositionTransform(IDCompositionTransform *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_transform, IDCompositionTransform_iface);
}

static ULONG dcomp_transform_addref_impl(struct dcomp_transform *t);

static HRESULT dcomp_transform_qi_impl(struct dcomp_transform *t, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionEffect) ||
        IsEqualGUID(iid, &IID_IDCompositionTransform3D) ||
        IsEqualGUID(iid, &IID_IDCompositionTransform) ||
        (IsEqualGUID(iid, &IID_IDCompositionTranslateTransform) && t->type == DCOMP_TRANSFORM_TRANSLATE) ||
        (IsEqualGUID(iid, &IID_IDCompositionScaleTransform) && t->type == DCOMP_TRANSFORM_SCALE) ||
        (IsEqualGUID(iid, &IID_IDCompositionRotateTransform) && t->type == DCOMP_TRANSFORM_ROTATE) ||
        (IsEqualGUID(iid, &IID_IDCompositionSkewTransform) && t->type == DCOMP_TRANSFORM_SKEW) ||
        (IsEqualGUID(iid, &IID_IDCompositionMatrixTransform) && t->type == DCOMP_TRANSFORM_MATRIX))
    {
        *out = &t->IDCompositionTransform_iface;
        dcomp_transform_addref_impl(t);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG dcomp_transform_addref_impl(struct dcomp_transform *t)
{
    return InterlockedIncrement(&t->refcount);
}

static void dcomp_transform_release_animations(struct dcomp_transform *t)
{
    struct dcomp_animated_float *props[15];
    unsigned int i, count = 0;

    props[count++] = &t->offset_x;
    props[count++] = &t->offset_y;
    props[count++] = &t->scale_x;
    props[count++] = &t->scale_y;
    props[count++] = &t->center_x;
    props[count++] = &t->center_y;
    props[count++] = &t->angle;
    props[count++] = &t->angle_x;
    props[count++] = &t->angle_y;
    for (i = 0; i < 6; i++)
        props[count++] = &t->matrix[i];
    for (i = 0; i < count; i++)
    {
        if (props[i]->animation)
        {
            IDCompositionAnimation_Release(props[i]->animation);
            props[i]->animation = NULL;
        }
    }
}

static ULONG dcomp_transform_release_impl(struct dcomp_transform *t)
{
    ULONG refcount = InterlockedDecrement(&t->refcount);

    if (!refcount)
    {
        dcomp_transform_release_animations(t);
        IDCompositionDevice_Release(&t->device->IDCompositionDevice_iface);
        free(t);
    }

    return refcount;
}

/* Windows validates only finiteness for these setters (no range checks);
 * a plain value replaces any attached animation. */
static HRESULT dcomp_transform_set_value(struct dcomp_transform *t, struct dcomp_animated_float *prop,
        float value)
{
    if (!isfinite(value))
        return E_INVALIDARG;
    EnterCriticalSection(&t->device->cs);
    prop->value = value;
    if (prop->animation)
    {
        IDCompositionAnimation_Release(prop->animation);
        prop->animation = NULL;
    }
    LeaveCriticalSection(&t->device->cs);
    return S_OK;
}

static HRESULT dcomp_transform_set_animation(struct dcomp_transform *t, struct dcomp_animated_float *prop,
        IDCompositionAnimation *animation)
{
    struct dcomp_device *device = t->device;
    HRESULT hr;

    EnterCriticalSection(&device->cs);
    if (FAILED(hr = dcomp_animation_check_for_property(animation, device)))
    {
        LeaveCriticalSection(&device->cs);
        return hr;
    }
    if (prop->animation)
        IDCompositionAnimation_Release(prop->animation);
    prop->animation = animation;
    IDCompositionAnimation_AddRef(animation);
    LeaveCriticalSection(&device->cs);
    return S_OK;
}

static float dcomp_animated_float_sample(const struct dcomp_animated_float *prop, LONGLONG now_100ns)
{
    return dcomp_sample_animation(prop->animation, prop->value, now_100ns);
}

/* Build the 2D matrix of a transform object, sampling its animations at
 * `now_100ns`. Caller holds the device lock. */
static void dcomp_transform_matrix(const struct dcomp_transform *t, LONGLONG now_100ns,
        D2D_MATRIX_3X2_F *out)
{
    float ox, oy, sx, sy, cx, cy, angle, ax, ay;

    switch (t->type)
    {
    case DCOMP_TRANSFORM_TRANSLATE:
        ox = dcomp_animated_float_sample(&t->offset_x, now_100ns);
        oy = dcomp_animated_float_sample(&t->offset_y, now_100ns);
        *out = (D2D_MATRIX_3X2_F){1.0f, 0.0f, 0.0f, 1.0f, ox, oy};
        break;
    case DCOMP_TRANSFORM_SCALE:
        sx = dcomp_animated_float_sample(&t->scale_x, now_100ns);
        sy = dcomp_animated_float_sample(&t->scale_y, now_100ns);
        cx = dcomp_animated_float_sample(&t->center_x, now_100ns);
        cy = dcomp_animated_float_sample(&t->center_y, now_100ns);
        *out = (D2D_MATRIX_3X2_F){sx, 0.0f, 0.0f, sy, cx * (1.0f - sx), cy * (1.0f - sy)};
        break;
    case DCOMP_TRANSFORM_ROTATE:
        angle = dcomp_animated_float_sample(&t->angle, now_100ns);
        cx = dcomp_animated_float_sample(&t->center_x, now_100ns);
        cy = dcomp_animated_float_sample(&t->center_y, now_100ns);
        {
            float rad = angle * (3.14159265358979323846f / 180.0f);
            float c = cosf(rad), s = sinf(rad);
            *out = (D2D_MATRIX_3X2_F){c, s, -s, c, cx * (1.0f - c) + cy * s, cy * (1.0f - c) - cx * s};
        }
        break;
    case DCOMP_TRANSFORM_SKEW:
        ax = dcomp_animated_float_sample(&t->angle_x, now_100ns);
        ay = dcomp_animated_float_sample(&t->angle_y, now_100ns);
        cx = dcomp_animated_float_sample(&t->center_x, now_100ns);
        cy = dcomp_animated_float_sample(&t->center_y, now_100ns);
        {
            float radx = ax * (3.14159265358979323846f / 180.0f);
            float rady = ay * (3.14159265358979323846f / 180.0f);
            float tx = tanf(radx), ty = tanf(rady);
            *out = (D2D_MATRIX_3X2_F){1.0f, ty, tx, 1.0f, -cx * tx, -cy * ty};
        }
        break;
    case DCOMP_TRANSFORM_MATRIX:
        out->_11 = dcomp_animated_float_sample(&t->matrix[0], now_100ns);
        out->_12 = dcomp_animated_float_sample(&t->matrix[1], now_100ns);
        out->_21 = dcomp_animated_float_sample(&t->matrix[2], now_100ns);
        out->_22 = dcomp_animated_float_sample(&t->matrix[3], now_100ns);
        out->_31 = dcomp_animated_float_sample(&t->matrix[4], now_100ns);
        out->_32 = dcomp_animated_float_sample(&t->matrix[5], now_100ns);
        break;
    }
}

static HRESULT STDMETHODCALLTYPE dcomp_translate_SetOffsetXAnimation(IDCompositionTranslateTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->offset_x, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_translate_SetOffsetX(IDCompositionTranslateTransform *iface, float offset)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->offset_x, offset);
}
static HRESULT STDMETHODCALLTYPE dcomp_translate_SetOffsetYAnimation(IDCompositionTranslateTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->offset_y, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_translate_SetOffsetY(IDCompositionTranslateTransform *iface, float offset)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->offset_y, offset);
}

static HRESULT STDMETHODCALLTYPE dcomp_scale_SetScaleXAnimation(IDCompositionScaleTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->scale_x, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_scale_SetScaleX(IDCompositionScaleTransform *iface, float scale)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->scale_x, scale);
}
static HRESULT STDMETHODCALLTYPE dcomp_scale_SetScaleYAnimation(IDCompositionScaleTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->scale_y, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_scale_SetScaleY(IDCompositionScaleTransform *iface, float scale)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->scale_y, scale);
}
static HRESULT STDMETHODCALLTYPE dcomp_scale_SetCenterXAnimation(IDCompositionScaleTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->center_x, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_scale_SetCenterX(IDCompositionScaleTransform *iface, float center)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->center_x, center);
}
static HRESULT STDMETHODCALLTYPE dcomp_scale_SetCenterYAnimation(IDCompositionScaleTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->center_y, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_scale_SetCenterY(IDCompositionScaleTransform *iface, float center)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->center_y, center);
}

static HRESULT STDMETHODCALLTYPE dcomp_rotate_SetAngleAnimation(IDCompositionRotateTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->angle, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rotate_SetAngle(IDCompositionRotateTransform *iface, float angle)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->angle, angle);
}
static HRESULT STDMETHODCALLTYPE dcomp_rotate_SetCenterXAnimation(IDCompositionRotateTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->center_x, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rotate_SetCenterX(IDCompositionRotateTransform *iface, float center)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->center_x, center);
}
static HRESULT STDMETHODCALLTYPE dcomp_rotate_SetCenterYAnimation(IDCompositionRotateTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->center_y, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_rotate_SetCenterY(IDCompositionRotateTransform *iface, float center)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->center_y, center);
}

static HRESULT STDMETHODCALLTYPE dcomp_skew_SetAngleXAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->angle_x, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_skew_SetAngleX(IDCompositionSkewTransform *iface, float angle)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->angle_x, angle);
}
static HRESULT STDMETHODCALLTYPE dcomp_skew_SetAngleYAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->angle_y, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_skew_SetAngleY(IDCompositionSkewTransform *iface, float angle)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->angle_y, angle);
}
static HRESULT STDMETHODCALLTYPE dcomp_skew_SetCenterXAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->center_x, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_skew_SetCenterX(IDCompositionSkewTransform *iface, float center)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->center_x, center);
}
static HRESULT STDMETHODCALLTYPE dcomp_skew_SetCenterYAnimation(IDCompositionSkewTransform *iface,
        IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_animation(t, &t->center_y, animation);
}
static HRESULT STDMETHODCALLTYPE dcomp_skew_SetCenterY(IDCompositionSkewTransform *iface, float center)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    return dcomp_transform_set_value(t, &t->center_y, center);
}

static HRESULT STDMETHODCALLTYPE dcomp_matrix_SetMatrix(IDCompositionMatrixTransform *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    unsigned int i;

    /* Windows does not validate the matrix (NaN accepted); a NULL pointer
     * crashes natively, so we reject it instead. */
    if (!matrix)
        return E_INVALIDARG;
    EnterCriticalSection(&t->device->cs);
    t->matrix[0].value = matrix->_11;
    t->matrix[1].value = matrix->_12;
    t->matrix[2].value = matrix->_21;
    t->matrix[3].value = matrix->_22;
    t->matrix[4].value = matrix->_31;
    t->matrix[5].value = matrix->_32;
    /* A plain matrix replaces any attached element animations. */
    for (i = 0; i < 6; i++)
    {
        if (t->matrix[i].animation)
        {
            IDCompositionAnimation_Release(t->matrix[i].animation);
            t->matrix[i].animation = NULL;
        }
    }
    LeaveCriticalSection(&t->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_matrix_SetMatrixElementAnimation(IDCompositionMatrixTransform *iface,
        int row, int column, IDCompositionAnimation *animation)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    if (row < 0 || row > 2 || column < 0 || column > 1)
        return E_INVALIDARG;
    return dcomp_transform_set_animation(t, &t->matrix[row * 2 + column], animation);
}

static HRESULT STDMETHODCALLTYPE dcomp_matrix_SetMatrixElement(IDCompositionMatrixTransform *iface,
        int row, int column, float value)
{
    struct dcomp_transform *t = impl_from_IDCompositionTransform((IDCompositionTransform *)iface);
    if (row < 0 || row > 2 || column < 0 || column > 1)
        return E_INVALIDARG;
    return dcomp_transform_set_value(t, &t->matrix[row * 2 + column], value);
}

/* Per-type IUnknown wrappers so each vtable's first three slots match its interface's exact function-pointer types. */
#define DCOMP_TRANSFORM_IUNKNOWN(name, type) \
static HRESULT STDMETHODCALLTYPE dcomp_##name##_qi(type *iface, REFIID iid, void **out) \
{ \
    return dcomp_transform_qi_impl(impl_from_IDCompositionTransform((IDCompositionTransform *)iface), iid, out); \
} \
static ULONG STDMETHODCALLTYPE dcomp_##name##_addref(type *iface) \
{ \
    return dcomp_transform_addref_impl(impl_from_IDCompositionTransform((IDCompositionTransform *)iface)); \
} \
static ULONG STDMETHODCALLTYPE dcomp_##name##_release(type *iface) \
{ \
    return dcomp_transform_release_impl(impl_from_IDCompositionTransform((IDCompositionTransform *)iface)); \
}

DCOMP_TRANSFORM_IUNKNOWN(translate, IDCompositionTranslateTransform)
DCOMP_TRANSFORM_IUNKNOWN(scale, IDCompositionScaleTransform)
DCOMP_TRANSFORM_IUNKNOWN(rotate, IDCompositionRotateTransform)
DCOMP_TRANSFORM_IUNKNOWN(skew, IDCompositionSkewTransform)
DCOMP_TRANSFORM_IUNKNOWN(matrix, IDCompositionMatrixTransform)

static const IDCompositionTranslateTransformVtbl dcomp_translate_transform_vtbl =
{
    dcomp_translate_qi,
    dcomp_translate_addref,
    dcomp_translate_release,
    dcomp_translate_SetOffsetXAnimation,
    dcomp_translate_SetOffsetX,
    dcomp_translate_SetOffsetYAnimation,
    dcomp_translate_SetOffsetY,
};

static const IDCompositionScaleTransformVtbl dcomp_scale_transform_vtbl =
{
    dcomp_scale_qi,
    dcomp_scale_addref,
    dcomp_scale_release,
    dcomp_scale_SetScaleXAnimation,
    dcomp_scale_SetScaleX,
    dcomp_scale_SetScaleYAnimation,
    dcomp_scale_SetScaleY,
    dcomp_scale_SetCenterXAnimation,
    dcomp_scale_SetCenterX,
    dcomp_scale_SetCenterYAnimation,
    dcomp_scale_SetCenterY,
};

static const IDCompositionRotateTransformVtbl dcomp_rotate_transform_vtbl =
{
    dcomp_rotate_qi,
    dcomp_rotate_addref,
    dcomp_rotate_release,
    dcomp_rotate_SetAngleAnimation,
    dcomp_rotate_SetAngle,
    dcomp_rotate_SetCenterXAnimation,
    dcomp_rotate_SetCenterX,
    dcomp_rotate_SetCenterYAnimation,
    dcomp_rotate_SetCenterY,
};

static const IDCompositionSkewTransformVtbl dcomp_skew_transform_vtbl =
{
    dcomp_skew_qi,
    dcomp_skew_addref,
    dcomp_skew_release,
    dcomp_skew_SetAngleXAnimation,
    dcomp_skew_SetAngleX,
    dcomp_skew_SetAngleYAnimation,
    dcomp_skew_SetAngleY,
    dcomp_skew_SetCenterXAnimation,
    dcomp_skew_SetCenterX,
    dcomp_skew_SetCenterYAnimation,
    dcomp_skew_SetCenterY,
};

static const IDCompositionMatrixTransformVtbl dcomp_matrix_transform_vtbl =
{
    dcomp_matrix_qi,
    dcomp_matrix_addref,
    dcomp_matrix_release,
    dcomp_matrix_SetMatrix,
    dcomp_matrix_SetMatrixElementAnimation,
    dcomp_matrix_SetMatrixElement,
};

static BOOL dcomp_transform_is_ours(IDCompositionTransform *transform)
{
    const void *vtbl = transform->lpVtbl;
    return vtbl == &dcomp_translate_transform_vtbl ||
           vtbl == &dcomp_scale_transform_vtbl ||
           vtbl == &dcomp_rotate_transform_vtbl ||
           vtbl == &dcomp_skew_transform_vtbl ||
           vtbl == &dcomp_matrix_transform_vtbl;
}

static HRESULT dcomp_transform_create(struct dcomp_device *device, enum dcomp_transform_type type,
        IDCompositionTransform **out)
{
    struct dcomp_transform *t;
    const void *vtbl;

    if (!out)
        return E_INVALIDARG;

    switch (type)
    {
    case DCOMP_TRANSFORM_TRANSLATE: vtbl = &dcomp_translate_transform_vtbl; break;
    case DCOMP_TRANSFORM_SCALE:     vtbl = &dcomp_scale_transform_vtbl; break;
    case DCOMP_TRANSFORM_ROTATE:    vtbl = &dcomp_rotate_transform_vtbl; break;
    case DCOMP_TRANSFORM_SKEW:      vtbl = &dcomp_skew_transform_vtbl; break;
    case DCOMP_TRANSFORM_MATRIX:    vtbl = &dcomp_matrix_transform_vtbl; break;
    default: return E_INVALIDARG;
    }

    if (!(t = calloc(1, sizeof(*t))))
        return E_OUTOFMEMORY;

    t->IDCompositionTransform_iface.lpVtbl = vtbl;
    t->refcount = 1;
    t->device = device;
    t->type = type;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    *out = &t->IDCompositionTransform_iface;
    return S_OK;
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

/*
 * {2c4eef28-1bc0-4736-b7dd-b62692f9bd67}: the private interface of the visuals. Measured on Windows 11 (tests/uwp/alpc/dcomp-visualpriv-probe.cpp,
 * both versions of the device): QueryInterface for it returns the visual itself, the same pointer as for IUnknown and IDCompositionVisual2, with
 * the reference count of the visual. The private methods are therefore further slots of the vtable of the visual, after the public ones; the
 * Windows App Runtime calls slots 40 and 41, always as a pair on one visual, each with one pointer argument (32-bit Windows removes 8 bytes: the
 * interface pointer and one argument), and both return S_OK without any effect that a caller can see (no output, no change of the reference
 * count). The meaning of the argument is not known. The other private slots are not known at all: their argument counts cannot be guessed on
 * i386, so calling one raises the stub exception instead of returning.
 */
static const GUID IID_dcomp_visual_private = {0x2c4eef28, 0x1bc0, 0x4736, {0xb7, 0xdd, 0xb6, 0x26, 0x92, 0xf9, 0xbd, 0x67}};

static HRESULT STDMETHODCALLTYPE dcomp_visual_private_method40(IDCompositionVisual2 *iface, void *arg)
{
    FIXME("iface %p, visual private method 40: %p semi-stub\n", iface, arg);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_private_method41(IDCompositionVisual2 *iface, void *arg)
{
    FIXME("iface %p, visual private method 41: %p semi-stub\n", iface, arg);
    return S_OK;
}

#define DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(n) \
static void STDMETHODCALLTYPE dcomp_visual_private_unimplemented##n(void) \
{ \
    ULONG_PTR args[2] = {(ULONG_PTR)L"dcomp.dll", (ULONG_PTR)"visual private method " #n}; \
    ERR("visual private method " #n " is not implemented\n"); \
    RaiseException(0x80000100 /* EXCEPTION_WINE_STUB */, EXCEPTION_NONCONTINUABLE, 2, args); \
}
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(22)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(23)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(24)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(25)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(26)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(27)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(28)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(29)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(30)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(31)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(32)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(33)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(34)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(35)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(36)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(37)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(38)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(39)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(42)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(43)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(44)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(45)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(46)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(47)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(48)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(49)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(50)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(51)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(52)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(53)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(54)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(55)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(56)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(57)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(58)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(59)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(60)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(61)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(62)
DCOMP_VISUAL_PRIVATE_UNIMPLEMENTED(63)

static HRESULT STDMETHODCALLTYPE dcomp_visual_QueryInterface(IDCompositionVisual2 *iface,
        REFIID iid, void **out)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IDCompositionVisual) ||
        IsEqualGUID(iid, &IID_dcomp_visual_private) ||
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

static void dcomp_shared_lock_acquire(void);
static void dcomp_shared_lock_release(void);
static void dcomp_shared_remove_visual(struct dcomp_visual *visual);

static ULONG STDMETHODCALLTYPE dcomp_visual_Release(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG refcount;

    if (visual->shared)
    {
        dcomp_shared_lock_acquire();
        refcount = InterlockedDecrement(&visual->refcount);
        if (!refcount) dcomp_shared_remove_visual(visual);
        dcomp_shared_lock_release();
    }
    else refcount = InterlockedDecrement(&visual->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        struct dcomp_device *device = visual->device;
        EnterCriticalSection(&device->cs);
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
    struct dcomp_device *device = visual->device;

    TRACE("iface %p, transform %p.\n", iface, transform);

    EnterCriticalSection(&device->cs);
    if (transform)
    {
        struct dcomp_transform *t;
        if (!dcomp_transform_is_ours(transform))
        {
            LeaveCriticalSection(&device->cs);
            return E_NOTIMPL;
        }
        t = impl_from_IDCompositionTransform(transform);
        if (t->device != device)
        {
            LeaveCriticalSection(&device->cs);
            return E_INVALIDARG;
        }
        /* Store without referencing (native parity); the caller keeps it
         * alive. A transform object replaces a plain matrix transform. */
        visual->pending.transform_obj = transform;
        visual->pending.has_transform = FALSE;
    }
    else
    {
        visual->pending.transform_obj = NULL;
        visual->pending.has_transform = FALSE;
    }
    LeaveCriticalSection(&device->cs);
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
        visual->pending.has_transform = TRUE;
        visual->pending.transform = *matrix;
        /* A plain matrix replaces an attached transform object. */
        visual->pending.transform_obj = NULL;
    }
    else
    {
        visual->pending.has_transform = FALSE;
        visual->pending.transform_obj = NULL;
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

    if ((const void *)visual->lpVtbl != &dcomp_visual_vtbl.base) return E_NOTIMPL;

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
        if ((const void *)reference_visual->lpVtbl != &dcomp_visual_vtbl.base) return E_NOTIMPL;
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

static const struct dcomp_visual_ext_vtbl dcomp_visual_vtbl =
{
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
},
{
    dcomp_visual_private_unimplemented22,
    dcomp_visual_private_unimplemented23,
    dcomp_visual_private_unimplemented24,
    dcomp_visual_private_unimplemented25,
    dcomp_visual_private_unimplemented26,
    dcomp_visual_private_unimplemented27,
    dcomp_visual_private_unimplemented28,
    dcomp_visual_private_unimplemented29,
    dcomp_visual_private_unimplemented30,
    dcomp_visual_private_unimplemented31,
    dcomp_visual_private_unimplemented32,
    dcomp_visual_private_unimplemented33,
    dcomp_visual_private_unimplemented34,
    dcomp_visual_private_unimplemented35,
    dcomp_visual_private_unimplemented36,
    dcomp_visual_private_unimplemented37,
    dcomp_visual_private_unimplemented38,
    dcomp_visual_private_unimplemented39,
    dcomp_visual_private_method40,
    dcomp_visual_private_method41,
    dcomp_visual_private_unimplemented42,
    dcomp_visual_private_unimplemented43,
    dcomp_visual_private_unimplemented44,
    dcomp_visual_private_unimplemented45,
    dcomp_visual_private_unimplemented46,
    dcomp_visual_private_unimplemented47,
    dcomp_visual_private_unimplemented48,
    dcomp_visual_private_unimplemented49,
    dcomp_visual_private_unimplemented50,
    dcomp_visual_private_unimplemented51,
    dcomp_visual_private_unimplemented52,
    dcomp_visual_private_unimplemented53,
    dcomp_visual_private_unimplemented54,
    dcomp_visual_private_unimplemented55,
    dcomp_visual_private_unimplemented56,
    dcomp_visual_private_unimplemented57,
    dcomp_visual_private_unimplemented58,
    dcomp_visual_private_unimplemented59,
    dcomp_visual_private_unimplemented60,
    dcomp_visual_private_unimplemented61,
    dcomp_visual_private_unimplemented62,
    dcomp_visual_private_unimplemented63,
},
};

static HRESULT dcomp_visual_create(struct dcomp_device *device, struct dcomp_visual **out)
{
    struct dcomp_visual *visual;

    if (!out)
        return E_INVALIDARG;

    if (!(visual = calloc(1, sizeof(*visual))))
        return E_OUTOFMEMORY;

    visual->IDCompositionVisual2_iface.lpVtbl = &dcomp_visual_vtbl.base;
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
        if ((const void *)visual->lpVtbl != &dcomp_visual_vtbl.base) return E_NOTIMPL;
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

/* the content no longer shows in a target: revoke its swapchain's scene */
static void dcomp_unbind_content(IUnknown *content)
{
    IWineDCompSwapChain *binding;

    if (!content || FAILED(IUnknown_QueryInterface(content, &IID_IWineDCompSwapChain, (void **)&binding)))
        return;
    IWineDCompSwapChain_set_target_scene(binding, NULL, NULL);
    IWineDCompSwapChain_Release(binding);
}

static void dcomp_visual_unbind_tree(struct dcomp_visual *visual)
{
    struct dcomp_visual *child;

    if (!visual) return;
    dcomp_unbind_content(visual->active.content);
    for (child = visual->active_children; child; child = child->active_next_sibling)
        dcomp_visual_unbind_tree(child);
}

static void dcomp_visual_apply_commit(struct dcomp_visual *visual, LONGLONG now_100ns)
{
    struct dcomp_visual *child, **p;

    if (!visual) return;

    /* Detach while the old active content still owns its COM reference. */
    if (visual->active.content != visual->pending.content)
        dcomp_unbind_content(visual->active.content);
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
            dcomp_rectangle_clip_sample(rc, now_100ns, &visual->active);
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
        const D2D_MATRIX_3X2_F *parent_mat, unsigned int depth, LONGLONG now_100ns)
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

    if (visual->pending.transform_obj)
        dcomp_transform_matrix(impl_from_IDCompositionTransform(visual->pending.transform_obj), now_100ns, &loc_mat);
    else if (visual->pending.has_transform)
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
        !isfinite(cur_mat._31) || !isfinite(cur_mat._32))
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
        if (FAILED(hr = dcomp_validate_pending(child, contents, visuals, seen, content_device, mode, &cur_mat, depth + 1,
                now_100ns)))
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
        dcomp_rectangle_clip_sample(clip, now_100ns, &state);
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
    if (state.transform_obj)
        dcomp_transform_matrix(impl_from_IDCompositionTransform(state.transform_obj), now_100ns, &local);
    else if (state.has_transform)
        local = state.transform;
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
                &content_device, DCOMPOSITION_OPACITY_MODE_LAYER, &ident, 0, now_100ns))) goto done;
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
    }
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
            if (j == visuals) dcomp_unbind_content(visual->active.content);
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
        /* The commit succeeded: now revoke the auxiliary producers this commit
         * does not present through (a real producer, no content, or no scene).
         * A failed commit keeps them, with the previous scene. */
        LIST_FOR_EACH_ENTRY(target, &dev->targets, struct dcomp_target, entry)
        {
            if (!target->aux_binding) continue;
            for (j = 0; j < count && bindings[j] != target->aux_binding; ++j) {}
            if (j == count) dcomp_target_release_aux_producer(target);
        }
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

/* Methods with the same body in the four device interfaces, written once. */
#define DCOMP_DEVICE_QUERY_INTERFACE(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_QueryInterface(type *iface, \
        REFIID iid, void **out) \
{ \
    struct dcomp_device *dev = impl(iface); \
    return IDCompositionDevice_QueryInterface(&dev->IDCompositionDevice_iface, iid, out); \
}

#define DCOMP_DEVICE_ADDREF(prefix, type, impl) \
static ULONG STDMETHODCALLTYPE dcomp_##prefix##_AddRef(type *iface) \
{ \
    struct dcomp_device *dev = impl(iface); \
    return IDCompositionDevice_AddRef(&dev->IDCompositionDevice_iface); \
}

#define DCOMP_DEVICE_RELEASE(prefix, type, impl) \
static ULONG STDMETHODCALLTYPE dcomp_##prefix##_Release(type *iface) \
{ \
    struct dcomp_device *dev = impl(iface); \
    return IDCompositionDevice_Release(&dev->IDCompositionDevice_iface); \
}

/* A commit has completed when dcomp_device_internal_commit returns, with success or not: the event of SetCommitCompletionEvent is signaled then. */
static HRESULT dcomp_device_commit(struct dcomp_device *dev)
{
    HRESULT hr = dcomp_device_internal_commit(dev);

    EnterCriticalSection(&dev->cs);
    if (dev->commit_event) SetEvent(dev->commit_event);
    LeaveCriticalSection(&dev->cs);
    return hr;
}

#define DCOMP_DEVICE_COMMIT(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_Commit(type *iface) \
{ \
    struct dcomp_device *dev = impl(iface); \
    return dcomp_device_commit(dev); \
}

#define DCOMP_DEVICE_WAIT_FOR_COMMIT_COMPLETION(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_WaitForCommitCompletion(type *iface) \
{ \
    struct dcomp_device *dev = impl(iface); \
    /* Direct binding has completed before Commit releases this lock. */ \
    EnterCriticalSection(&dev->cs); \
    LeaveCriticalSection(&dev->cs); \
    return S_OK; \
}

#define DCOMP_DEVICE_GET_FRAME_STATISTICS(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_GetFrameStatistics(type *iface, \
        DCOMPOSITION_FRAME_STATISTICS *statistics) \
{ \
    FIXME("iface %p, statistics %p stub!\n", iface, statistics); \
    if (!statistics) \
        return E_POINTER; \
    memset(statistics, 0, sizeof(*statistics)); \
    return S_OK; \
}

#define DCOMP_DEVICE_CREATE_VIRTUAL_SURFACE(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateVirtualSurface(type *iface, \
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, \
        IDCompositionVirtualSurface **surface) \
{ \
    struct dcomp_device *dev = impl(iface); \
    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n", \
            iface, width, height, pixel_format, alpha_mode, surface); \
    return dcomp_device_create_virtual_surface(dev, width, height, pixel_format, alpha_mode, surface); \
}

#define DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateTranslateTransform(type *iface, \
        IDCompositionTranslateTransform **transform) \
{ \
    TRACE("iface %p, transform %p.\n", iface, transform); \
    return dcomp_transform_create(impl(iface), DCOMP_TRANSFORM_TRANSLATE, \
            (IDCompositionTransform **)transform); \
}

#define DCOMP_DEVICE_CREATE_SCALE_TRANSFORM(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateScaleTransform(type *iface, \
        IDCompositionScaleTransform **transform) \
{ \
    TRACE("iface %p, transform %p.\n", iface, transform); \
    return dcomp_transform_create(impl(iface), DCOMP_TRANSFORM_SCALE, \
            (IDCompositionTransform **)transform); \
}

#define DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateRotateTransform(type *iface, \
        IDCompositionRotateTransform **transform) \
{ \
    TRACE("iface %p, transform %p.\n", iface, transform); \
    return dcomp_transform_create(impl(iface), DCOMP_TRANSFORM_ROTATE, \
            (IDCompositionTransform **)transform); \
}

#define DCOMP_DEVICE_CREATE_SKEW_TRANSFORM(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateSkewTransform(type *iface, \
        IDCompositionSkewTransform **transform) \
{ \
    TRACE("iface %p, transform %p.\n", iface, transform); \
    return dcomp_transform_create(impl(iface), DCOMP_TRANSFORM_SKEW, \
            (IDCompositionTransform **)transform); \
}

#define DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateMatrixTransform(type *iface, \
        IDCompositionMatrixTransform **transform) \
{ \
    TRACE("iface %p, transform %p.\n", iface, transform); \
    return dcomp_transform_create(impl(iface), DCOMP_TRANSFORM_MATRIX, \
            (IDCompositionTransform **)transform); \
}

#define DCOMP_DEVICE_CREATE_TRANSFORM_GROUP(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateTransformGroup(type *iface, \
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group) \
{ \
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", \
            iface, transforms, elements, transform_group); \
    return E_NOTIMPL; \
}

#define DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM_3D(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateTranslateTransform3D(type *iface, \
        IDCompositionTranslateTransform3D **transform_3d) \
{ \
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d); \
    return E_NOTIMPL; \
}

#define DCOMP_DEVICE_CREATE_SCALE_TRANSFORM_3D(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateScaleTransform3D(type *iface, \
        IDCompositionScaleTransform3D **transform_3d) \
{ \
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d); \
    return E_NOTIMPL; \
}

#define DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM_3D(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateRotateTransform3D(type *iface, \
        IDCompositionRotateTransform3D **transform_3d) \
{ \
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d); \
    return E_NOTIMPL; \
}

#define DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM_3D(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateMatrixTransform3D(type *iface, \
        IDCompositionMatrixTransform3D **transform_3d) \
{ \
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d); \
    return E_NOTIMPL; \
}

#define DCOMP_DEVICE_CREATE_TRANSFORM_3D_GROUP(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateTransform3DGroup(type *iface, \
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group) \
{ \
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", \
            iface, transforms_3d, elements, transform_3d_group); \
    return E_NOTIMPL; \
}

#define DCOMP_DEVICE_CREATE_EFFECT_GROUP(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateEffectGroup(type *iface, \
        IDCompositionEffectGroup **effect_group) \
{ \
    TRACE("iface %p, effect_group %p.\n", iface, effect_group); \
    return dcomp_effect_group_create(impl(iface), effect_group); \
}

#define DCOMP_DEVICE_CREATE_RECTANGLE_CLIP(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateRectangleClip(type *iface, \
        IDCompositionRectangleClip **clip) \
{ \
    TRACE("iface %p, clip %p.\n", iface, clip); \
    return dcomp_rectangle_clip_create(impl(iface), clip); \
}

#define DCOMP_DEVICE_CREATE_ANIMATION(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateAnimation(type *iface, \
        IDCompositionAnimation **animation) \
{ \
    TRACE("iface %p, animation %p.\n", iface, animation); \
    return dcomp_animation_create(impl(iface), animation); \
}

#define DCOMP_DEVICE_CREATE_SURFACE_FACTORY(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateSurfaceFactory(type *iface, \
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory) \
{ \
    struct dcomp_device *dev = impl(iface); \
    TRACE("iface %p, rendering_device %p, surface_factory %p.\n", iface, rendering_device, \
            surface_factory); \
    return dcomp_surface_factory_create(&dev->IDCompositionDevice_iface, rendering_device, \
            surface_factory); \
}

#define DCOMP_DEVICE_CREATE_SURFACE(prefix, type, impl) \
static HRESULT STDMETHODCALLTYPE dcomp_##prefix##_CreateSurface(type *iface, \
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, \
        IDCompositionSurface **surface) \
{ \
    struct dcomp_device *dev = impl(iface); \
    TRACE("iface %p, width %u, height %u, pixel_format %#x, alpha_mode %#x, surface %p.\n", \
            iface, width, height, pixel_format, alpha_mode, surface); \
    return dcomp_surface_create(&dev->IDCompositionDevice_iface, dev->rendering_device, \
            &dev->rendering_device_iid, width, height, pixel_format, alpha_mode, surface); \
}

static const GUID IID_dcomp_alias_a = {0xd14b6158, 0xc3fa, 0x4bce, {0x9c, 0x1f, 0xb6, 0x1d, 0x86, 0x65, 0xea, 0xb0}};
static const GUID IID_dcomp_alias_b = {0x28d6ad3d, 0xee2a, 0x4bcd, {0x94, 0x19, 0x7d, 0x54, 0x80, 0x04, 0x35, 0xb1}};

/* The interface {6b556968-2d25-11e2-99f6-8bc36088709b} of the device. FrameworkUdk of the Windows App Runtime (its helper for the Windows
 * releases up to UniversalApiContract 17, the one that runs on Wine) asks for it in DCompPrivates_SetCommitCompletionEvent and calls slot 8
 * with the event handle (traced on Wine with a logging object). Windows 11 build 26300 answers the same slot differently (it removes 12
 * bytes on 32-bit Windows, two arguments, and returns E_INVALIDARG for an event handle: tests/uwp/alpc/dcomp-dslot32b.cpp), because there the
 * runtime goes through a system-side module instead; so the slot is implemented for the helper that calls it here, not for that Windows.
 * The event is duplicated and signaled when a Commit has completed, which here is when Commit returns, successful or not, so a waiter
 * cannot hang on a failed commit. A NULL handle clears the event. The other slots raise the stub exception: their argument counts are not known. */
static const GUID IID_dcomp_commit = {0x6b556968, 0x2d25, 0x11e2, {0x99, 0xf6, 0x8b, 0xc3, 0x60, 0x88, 0x70, 0x9b}};

static struct dcomp_device *impl_from_commit_iface(const void *const **iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, commit_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_commit_QueryInterface(const void *const **iface, REFIID iid, void **out)
{
    struct dcomp_device *dev = impl_from_commit_iface(iface);

    return IDCompositionDevice_QueryInterface(&dev->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_commit_AddRef(const void *const **iface)
{
    struct dcomp_device *dev = impl_from_commit_iface(iface);

    return IDCompositionDevice_AddRef(&dev->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_commit_Release(const void *const **iface)
{
    struct dcomp_device *dev = impl_from_commit_iface(iface);

    return IDCompositionDevice_Release(&dev->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_commit_SetCommitCompletionEvent(const void *const **iface, HANDLE event, void *arg)
{
    struct dcomp_device *dev = impl_from_commit_iface(iface);
    HANDLE copy = NULL, old;

    TRACE("iface %p, event %p, arg %p.\n", iface, event, arg);

    if (event && !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return HRESULT_FROM_WIN32(GetLastError());

    EnterCriticalSection(&dev->cs);
    old = dev->commit_event;
    dev->commit_event = copy;
    LeaveCriticalSection(&dev->cs);
    if (old) CloseHandle(old);
    return S_OK;
}

#define DCOMP_COMMIT_STUB(n) \
static void STDMETHODCALLTYPE dcomp_commit_method##n(void) \
{ \
    ULONG_PTR args[2] = {(ULONG_PTR)L"dcomp.dll", (ULONG_PTR)"commit interface method " #n}; \
    ERR("commit interface method " #n " is not implemented\n"); \
    RaiseException(0x80000100 /* EXCEPTION_WINE_STUB */, EXCEPTION_NONCONTINUABLE, 2, args); \
}
DCOMP_COMMIT_STUB(3) DCOMP_COMMIT_STUB(4) DCOMP_COMMIT_STUB(5) DCOMP_COMMIT_STUB(6) DCOMP_COMMIT_STUB(7)
DCOMP_COMMIT_STUB(9) DCOMP_COMMIT_STUB(10) DCOMP_COMMIT_STUB(11) DCOMP_COMMIT_STUB(12) DCOMP_COMMIT_STUB(13)
DCOMP_COMMIT_STUB(14) DCOMP_COMMIT_STUB(15)

static const void *const dcomp_commit_vtbl[] =
{
    dcomp_commit_QueryInterface, dcomp_commit_AddRef, dcomp_commit_Release,
    dcomp_commit_method3, dcomp_commit_method4, dcomp_commit_method5, dcomp_commit_method6, dcomp_commit_method7,
    dcomp_commit_SetCommitCompletionEvent,
    dcomp_commit_method9, dcomp_commit_method10, dcomp_commit_method11, dcomp_commit_method12, dcomp_commit_method13,
    dcomp_commit_method14, dcomp_commit_method15,
};

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
    else if (dev->version >= 2 && (IsEqualGUID(iid, &IID_IDCompositionDesktopDevice) ||
             IsEqualGUID(iid, &IID_dcomp_alias_a) || IsEqualGUID(iid, &IID_dcomp_alias_b)))
    {
        /* Windows 11 answers {d14b6158-...} and {28d6ad3d-...}, which dcompi.dll of the Windows App Runtime asks the device for
         * (tests/uwp/alpc/dcomp-privqi-probe.c), with the very interface of IDCompositionDesktopDevice */
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
    else if (dev->version >= 2 && IsEqualGUID(iid, &IID_dcomp_commit))
    {
        *out = &dev->commit_iface;
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
        if (dev->commit_event)
            CloseHandle(dev->commit_event);
        DeleteCriticalSection(&dev->cs);
        free(dev);
    }

    return refcount;
}

DCOMP_DEVICE_COMMIT(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_WAIT_FOR_COMMIT_COMPLETION(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_GET_FRAME_STATISTICS(device, IDCompositionDevice, impl_from_IDCompositionDevice)

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

DCOMP_DEVICE_CREATE_VIRTUAL_SURFACE(device, IDCompositionDevice, impl_from_IDCompositionDevice)

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

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_SKEW_TRANSFORM(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_TRANSFORM_GROUP(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM_3D(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM_3D(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM_3D(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM_3D(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_TRANSFORM_3D_GROUP(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_EFFECT_GROUP(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_RECTANGLE_CLIP(device, IDCompositionDevice, impl_from_IDCompositionDevice)

DCOMP_DEVICE_CREATE_ANIMATION(device, IDCompositionDevice, impl_from_IDCompositionDevice)

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

DCOMP_DEVICE_QUERY_INTERFACE(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_ADDREF(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_RELEASE(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_COMMIT(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_WAIT_FOR_COMMIT_COMPLETION(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_GET_FRAME_STATISTICS(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

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

DCOMP_DEVICE_CREATE_SURFACE_FACTORY(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_SURFACE(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_VIRTUAL_SURFACE(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_SKEW_TRANSFORM(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_TRANSFORM_GROUP(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM_3D(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM_3D(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM_3D(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM_3D(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_TRANSFORM_3D_GROUP(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_EFFECT_GROUP(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_RECTANGLE_CLIP(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

DCOMP_DEVICE_CREATE_ANIMATION(device2, IDCompositionDevice2, impl_from_IDCompositionDevice2)

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

DCOMP_DEVICE_QUERY_INTERFACE(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_ADDREF(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_RELEASE(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_COMMIT(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_WAIT_FOR_COMMIT_COMPLETION(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_GET_FRAME_STATISTICS(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVisual(IDCompositionDevice3 *iface,
        IDCompositionVisual2 **visual)
{
    struct dcomp_device *dev = impl_from_IDCompositionDevice3(iface);
    return IDCompositionDevice2_CreateVisual(&dev->IDCompositionDevice2_iface, visual);
}

DCOMP_DEVICE_CREATE_SURFACE_FACTORY(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_SURFACE(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_VIRTUAL_SURFACE(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_SKEW_TRANSFORM(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransformGroup(IDCompositionDevice3 *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transform_group %p stub!\n", iface, transform_group);
    return E_NOTIMPL;
}

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM_3D(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM_3D(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM_3D(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM_3D(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransform3DGroup(IDCompositionDevice3 *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transform_3d_group %p stub!\n", iface, transform_3d_group);
    return E_NOTIMPL;
}

DCOMP_DEVICE_CREATE_EFFECT_GROUP(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_RECTANGLE_CLIP(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

DCOMP_DEVICE_CREATE_ANIMATION(device3, IDCompositionDevice3, impl_from_IDCompositionDevice3)

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

DCOMP_DEVICE_QUERY_INTERFACE(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_ADDREF(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_RELEASE(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_COMMIT(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_WAIT_FOR_COMMIT_COMPLETION(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_GET_FRAME_STATISTICS(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateVisual(IDCompositionDesktopDevice *iface,
        IDCompositionVisual2 **visual)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    return IDCompositionDevice2_CreateVisual(&dev->IDCompositionDevice2_iface, visual);
}

DCOMP_DEVICE_CREATE_SURFACE_FACTORY(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_SURFACE(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_VIRTUAL_SURFACE(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_SKEW_TRANSFORM(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransformGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transform_group %p stub!\n", iface, transform_group);
    return E_NOTIMPL;
}

DCOMP_DEVICE_CREATE_TRANSLATE_TRANSFORM_3D(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_SCALE_TRANSFORM_3D(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_ROTATE_TRANSFORM_3D(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_MATRIX_TRANSFORM_3D(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransform3DGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transform_3d_group %p stub!\n", iface, transform_3d_group);
    return E_NOTIMPL;
}

DCOMP_DEVICE_CREATE_EFFECT_GROUP(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_RECTANGLE_CLIP(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

DCOMP_DEVICE_CREATE_ANIMATION(desktop_device, IDCompositionDesktopDevice, impl_from_IDCompositionDesktopDevice)

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

/* =====================================================================
 * Shared visuals, as the Windows App Runtime uses them
 *
 * The runtime (Microsoft.Internal.FrameworkUdk, for Microsoft.UI.Input and others) makes a visual that several parties see in two steps:
 * DCompositionCreateSharedVisualHandle (export 1040) gives a handle, and a private method of the device, slot 29 of the interface of
 * IDCompositionDesktopDevice (after the three methods the public interface ends with), opens that handle as an IDCompositionVisual or
 * as an IDCompositionTarget. Observed on Windows 11 (tests/uwp/alpc/wintrace.c): the handle is a kernel object, the device is the one
 * DCompositionCreateDevice3 returns, the same handle opened twice gives a visual and then a target. There is no system compositor to
 * share with here: a handle stands for one visual of this process (made on the first open and kept until the handle value is made
 * again), and a target opened from a handle has no window (SetRoot only keeps the root).
 * ===================================================================== */

struct dcomp_shared
{
    struct list entry;
    HANDLE handle;
    struct dcomp_device *device;   /* the device the visual was made for; no reference */
    struct dcomp_visual *visual;   /* no reference: the entry goes when the last holder releases the visual */
};

static struct list dcomp_shared_list = LIST_INIT(dcomp_shared_list);
static SRWLOCK dcomp_shared_lock = SRWLOCK_INIT;

static void dcomp_shared_lock_acquire(void)
{
    AcquireSRWLockExclusive(&dcomp_shared_lock);
}

static void dcomp_shared_lock_release(void)
{
    ReleaseSRWLockExclusive(&dcomp_shared_lock);
}

/* with the lock held */
static void dcomp_shared_remove_visual(struct dcomp_visual *visual)
{
    struct dcomp_shared *shared, *next;

    LIST_FOR_EACH_ENTRY_SAFE(shared, next, &dcomp_shared_list, struct dcomp_shared, entry)
    {
        if (shared->visual != visual) continue;
        list_remove(&shared->entry);
        free(shared);
    }
}

/* a new handle value is a new shared visual: forget what an older handle of the same value stood for (the visual lives on
 * for whoever holds it, it is only no longer found by the handle) */
static void dcomp_shared_forget(HANDLE handle)
{
    struct dcomp_shared *shared, *next;

    AcquireSRWLockExclusive(&dcomp_shared_lock);
    LIST_FOR_EACH_ENTRY_SAFE(shared, next, &dcomp_shared_list, struct dcomp_shared, entry)
    {
        if (shared->handle != handle) continue;
        list_remove(&shared->entry);
        free(shared);
    }
    ReleaseSRWLockExclusive(&dcomp_shared_lock);
}

/* One visual per handle and device: a second device opening the handle makes its own visual, because a visual of another device
 * cannot be put into a target of this one. A visual nobody holds any more is gone, and the handle then makes a new one. */
static HRESULT dcomp_shared_open_visual(struct dcomp_device *dev, HANDLE handle, REFIID iid, void **out)
{
    struct dcomp_shared *shared, *found = NULL;
    struct dcomp_visual *visual;
    HRESULT hr;

    AcquireSRWLockExclusive(&dcomp_shared_lock);
    LIST_FOR_EACH_ENTRY(shared, &dcomp_shared_list, struct dcomp_shared, entry)
        if (shared->handle == handle && shared->device == dev) { found = shared; break; }
    if (found)
    {
        hr = IDCompositionVisual2_QueryInterface(&found->visual->IDCompositionVisual2_iface, iid, out);
        ReleaseSRWLockExclusive(&dcomp_shared_lock);
        return hr;
    }
    ReleaseSRWLockExclusive(&dcomp_shared_lock);

    if (!(found = calloc(1, sizeof(*found)))) return E_OUTOFMEMORY;
    if (FAILED(hr = dcomp_visual_create(dev, &visual)))
    {
        free(found);
        return hr;
    }
    visual->shared = TRUE;
    found->handle = handle;
    found->device = dev;
    found->visual = visual;

    AcquireSRWLockExclusive(&dcomp_shared_lock);
    list_add_tail(&dcomp_shared_list, &found->entry);
    ReleaseSRWLockExclusive(&dcomp_shared_lock);

    hr = IDCompositionVisual2_QueryInterface(&visual->IDCompositionVisual2_iface, iid, out);
    /* the reference of dcomp_visual_create belongs to the caller of this function now */
    IDCompositionVisual2_Release(&visual->IDCompositionVisual2_iface);
    return hr;
}

/* a target that no window backs: SetRoot keeps the root, nothing is shown */
static HRESULT dcomp_shared_open_target(struct dcomp_device *dev, REFIID iid, void **out)
{
    struct dcomp_target *target;
    HRESULT hr;

    if (!(target = calloc(1, sizeof(*target)))) return E_OUTOFMEMORY;
    target->IDCompositionTarget_iface.lpVtbl = &dcomp_target_vtbl;
    target->refcount = 1;
    target->device = dev;
    IDCompositionDevice_AddRef(&dev->IDCompositionDevice_iface);
    list_init(&target->entry);
    hr = IDCompositionTarget_QueryInterface(&target->IDCompositionTarget_iface, iid, out);
    IDCompositionTarget_Release(&target->IDCompositionTarget_iface);
    return hr;
}

/* Methods 29 (opens a shared object) and 46 are known. The argument counts of the others are not: guessed, a stdcall stub corrupts the
 * stack on i386, so calling one raises the stub exception instead of returning. */
#define DCOMP_PRIVATE_STUB(n) \
static void STDMETHODCALLTYPE dcomp_desktop_device_private##n(void) \
{ \
    ULONG_PTR args[2] = {(ULONG_PTR)L"dcomp.dll", (ULONG_PTR)"desktop device private method " #n}; \
    ERR("desktop device private method " #n " is not implemented\n"); \
    RaiseException(0x80000100 /* EXCEPTION_WINE_STUB */, EXCEPTION_NONCONTINUABLE, 2, args); \
}
DCOMP_PRIVATE_STUB(27) DCOMP_PRIVATE_STUB(28) DCOMP_PRIVATE_STUB(30) DCOMP_PRIVATE_STUB(31) DCOMP_PRIVATE_STUB(32)
DCOMP_PRIVATE_STUB(33) DCOMP_PRIVATE_STUB(34) DCOMP_PRIVATE_STUB(35) DCOMP_PRIVATE_STUB(36) DCOMP_PRIVATE_STUB(37)
DCOMP_PRIVATE_STUB(38) DCOMP_PRIVATE_STUB(39) DCOMP_PRIVATE_STUB(40) DCOMP_PRIVATE_STUB(41) DCOMP_PRIVATE_STUB(42)
DCOMP_PRIVATE_STUB(43) DCOMP_PRIVATE_STUB(44) DCOMP_PRIVATE_STUB(45) DCOMP_PRIVATE_STUB(47)

/* Method 46 of the batch-query interface ({28d6ad3d-...}, the very interface of the desktop device) takes one pointer (on 32-bit Windows 11 it
 * removes 8 bytes: the interface and one argument) and stores a dword in it: the id of the newest batch of changes that the composition has
 * completed. Measured on Windows 11 (tests/uwp/alpc/dcomp-dslot32.cpp; a trace of the composition engine of the Windows App Runtime shows the
 * value rise 1, 1, 2, 3, 3, 4, 4 as it submits batches): 0 on a device nothing was submitted to; the second dword of an eight-byte block is
 * not touched. The submit methods are not known and not implemented, so no batch is ever submitted here and the answer is 0.
 * (The other registers of the call that a trace shows belong to the caller; they are not arguments.) */
static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_private46(IDCompositionDesktopDevice *iface, DWORD *completed)
{
    HRESULT hr = S_OK;

    TRACE("iface %p, completed %p.\n", iface, completed);

    if (!completed) return E_POINTER;

    __TRY
    {
        *completed = 0;
    }
    __EXCEPT_PAGE_FAULT
    {
        hr = E_POINTER;
    }
    __ENDTRY

    return hr;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_OpenSharedObject(IDCompositionDesktopDevice *iface, HANDLE handle,
        REFIID iid, void **out)
{
    struct dcomp_device *dev = impl_from_IDCompositionDesktopDevice(iface);
    DWORD flags;

    TRACE("iface %p, handle %p, iid %s, out %p.\n", iface, handle, debugstr_guid(iid), out);

    if (!out) return E_POINTER;
    *out = NULL;
    if (!iid) return E_INVALIDARG;
    if (!handle || handle == INVALID_HANDLE_VALUE || !GetHandleInformation(handle, &flags))
        return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

    if (IsEqualGUID(iid, &IID_IDCompositionTarget)) return dcomp_shared_open_target(dev, iid, out);
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDCompositionVisual)
            || IsEqualGUID(iid, &IID_IDCompositionVisual2))
        return dcomp_shared_open_visual(dev, handle, iid, out);

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

/* the interface of IDCompositionDesktopDevice and its private methods, in the order of the vtable */
struct dcomp_desktop_device_ext_vtbl
{
    struct IDCompositionDesktopDeviceVtbl base;
    void *private_methods[21];   /* slots 27 to 47 */
};

static const struct dcomp_desktop_device_ext_vtbl dcomp_desktop_device_ext_vtbl =
{
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
},
{
    dcomp_desktop_device_private27, dcomp_desktop_device_private28, dcomp_desktop_device_OpenSharedObject,
    dcomp_desktop_device_private30, dcomp_desktop_device_private31, dcomp_desktop_device_private32,
    dcomp_desktop_device_private33, dcomp_desktop_device_private34, dcomp_desktop_device_private35,
    dcomp_desktop_device_private36, dcomp_desktop_device_private37, dcomp_desktop_device_private38,
    dcomp_desktop_device_private39, dcomp_desktop_device_private40, dcomp_desktop_device_private41,
    dcomp_desktop_device_private42, dcomp_desktop_device_private43, dcomp_desktop_device_private44,
    dcomp_desktop_device_private45, dcomp_desktop_device_private46, dcomp_desktop_device_private47,
},
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
    dev->IDCompositionDesktopDevice_iface.lpVtbl = &dcomp_desktop_device_ext_vtbl.base;
    dev->commit_iface = dcomp_commit_vtbl;
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

/*
 * Export 1040 of the real dcomp.dll, named DCompositionCreateSharedVisualHandle (tests/uwp/alpc/dcomp-exports-probe.c): imported
 * by ordinal by dcompi.dll of the Windows App Runtime. The signature is HRESULT (HANDLE *out), measured on Windows 11 with
 * tests/uwp/alpc/dcomp-visualhandle-probe.c: a NULL out is E_INVALIDARG, otherwise the handle is an object of type "Composition".
 * The compositor passes the handle on in a property of a visual; here the handle is an
 * anonymous event, which is what a caller can do with it (duplicate it, close it) until there is a system compositor to share
 * the visual with.
 */
HRESULT WINAPI DCompositionCreateSharedVisualHandle(HANDLE *out)
{
    HANDLE handle;

    TRACE("(%p)\n", out);

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!(handle = CreateEventW(NULL, TRUE, FALSE, NULL))) return HRESULT_FROM_WIN32(GetLastError());
    dcomp_shared_forget(handle);
    *out = handle;
    return S_OK;
}
