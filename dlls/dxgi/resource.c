/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
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
 *
 */

#include "dxgi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

/* Inner IUnknown methods */

static inline struct dxgi_resource *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IUnknown_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_inner_QueryInterface(IUnknown *iface, REFIID riid, void **out)
{
    struct dxgi_resource *resource = impl_from_IUnknown(iface);
    bool is_subresource = !!resource->parent_resource;

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if ((IsEqualGUID(riid, &IID_IDXGISurface2)
            || IsEqualGUID(riid, &IID_IDXGISurface1)
            || IsEqualGUID(riid, &IID_IDXGISurface)) && resource->IDXGISurface2_iface.lpVtbl != NULL)
    {
        IDXGISurface2_AddRef(&resource->IDXGISurface2_iface);
        *out = &resource->IDXGISurface2_iface;
        return S_OK;
    }
    else if (!is_subresource && IsEqualGUID(riid, &IID_IDXGIKeyedMutex))
    {
        if (resource->has_keyed_mutex)
        {
            IDXGIKeyedMutex_AddRef(&resource->IDXGIKeyedMutex_iface);
            *out = &resource->IDXGIKeyedMutex_iface;
            return S_OK;
        }
        WARN("Resource does not support keyed mutex\n");
        *out = NULL;
        return E_NOINTERFACE;
    }
    else if ((!is_subresource && (IsEqualGUID(riid, &IID_IDXGIResource)
            || IsEqualGUID(riid, &IID_IDXGIResource1)))
            || IsEqualGUID(riid, &IID_IDXGIDeviceSubObject)
            || IsEqualGUID(riid, &IID_IDXGIObject)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        IDXGIResource1_AddRef(&resource->IDXGIResource1_iface);
        *out = &resource->IDXGIResource1_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE\n", debugstr_guid(riid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxgi_resource_inner_AddRef(IUnknown *iface)
{
    struct dxgi_resource *resource = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&resource->refcount);

    TRACE("%p increasing refcount to %lu.\n", resource, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE dxgi_resource_inner_Release(IUnknown *iface)
{
    struct dxgi_resource *resource = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&resource->refcount);

    TRACE("%p decreasing refcount to %lu.\n", resource, refcount);

    if (!refcount)
    {
        if (resource->has_keyed_mutex)
            DeleteCriticalSection(&resource->keyed_mutex_cs);
        if (resource->parent_resource)
            IDXGIResource1_Release(resource->parent_resource);
        wined3d_private_store_cleanup(&resource->private_store);
        free(resource);
    }

    return refcount;
}

static inline struct dxgi_resource *impl_from_IDXGISurface2(IDXGISurface2 *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IDXGISurface2_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE dxgi_surface_QueryInterface(IDXGISurface2 *iface, REFIID riid,
        void **object)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_QueryInterface(&resource->IDXGIResource1_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE dxgi_surface_AddRef(IDXGISurface2 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_AddRef(&resource->IDXGIResource1_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_surface_Release(IDXGISurface2 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_Release(&resource->IDXGIResource1_iface);
}

/* IDXGIObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_surface_SetPrivateData(IDXGISurface2 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_SetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_SetPrivateDataInterface(IDXGISurface2 *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_SetPrivateDataInterface(&resource->IDXGIResource1_iface, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_GetPrivateData(IDXGISurface2 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_GetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_GetParent(IDXGISurface2 *iface, REFIID riid, void **parent)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_GetParent(&resource->IDXGIResource1_iface, riid, parent);
}

/* IDXGIDeviceSubObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_surface_GetDevice(IDXGISurface2 *iface, REFIID riid, void **device)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_GetDevice(&resource->IDXGIResource1_iface, riid, device);
}

/* IDXGISurface methods */
static HRESULT STDMETHODCALLTYPE dxgi_surface_GetDesc(IDXGISurface2 *iface, DXGI_SURFACE_DESC *desc)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    bool is_subresource = !!resource->parent_resource, is_buffer = false;
    struct wined3d_sub_resource_desc subresource_desc;
    struct wined3d_resource_desc resource_desc;

    TRACE("iface %p, desc %p.\n", iface, desc);

    wined3d_mutex_lock();
    wined3d_resource_get_sub_resource_desc(resource->wined3d_resource, resource->subresource_idx, &subresource_desc);
    if (is_subresource)
    {
        wined3d_resource_get_desc(resource->wined3d_resource, &resource_desc);
        is_buffer = resource_desc.resource_type == WINED3D_RTYPE_BUFFER;
    }
    wined3d_mutex_unlock();
    desc->Width = subresource_desc.width;
    desc->Height = subresource_desc.height;
    desc->Format = is_buffer ? DXGI_FORMAT_UNKNOWN : dxgi_format_from_wined3dformat(subresource_desc.format);
    dxgi_sample_desc_from_wined3d(&desc->SampleDesc, subresource_desc.multisample_type, subresource_desc.multisample_quality);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_Map(IDXGISurface2 *iface, DXGI_MAPPED_RECT *mapped_rect, UINT flags)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    struct wined3d_map_desc wined3d_map_desc;
    DWORD wined3d_map_flags = 0;
    HRESULT hr;

    TRACE("iface %p, mapped_rect %p, flags %#x.\n", iface, mapped_rect, flags);

    if (flags & DXGI_MAP_READ)
        wined3d_map_flags |= WINED3D_MAP_READ;
    if (flags & DXGI_MAP_WRITE)
        wined3d_map_flags |= WINED3D_MAP_WRITE;
    if (flags & DXGI_MAP_DISCARD)
        wined3d_map_flags |= WINED3D_MAP_DISCARD;

    if (SUCCEEDED(hr = wined3d_resource_map(resource->wined3d_resource, resource->subresource_idx,
            &wined3d_map_desc, NULL, wined3d_map_flags)))
    {
        mapped_rect->Pitch = wined3d_map_desc.row_pitch;
        mapped_rect->pBits = wined3d_map_desc.data;
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_Unmap(IDXGISurface2 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);

    TRACE("iface %p.\n", iface);
    wined3d_resource_unmap(resource->wined3d_resource, resource->subresource_idx);
    return S_OK;
}

/* IDXGISurface1 methods */
static HRESULT STDMETHODCALLTYPE dxgi_surface_GetDC(IDXGISurface2 *iface, BOOL discard, HDC *hdc)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    HRESULT hr;

    FIXME("iface %p, discard %d, hdc %p semi-stub!\n", iface, discard, hdc);

    if (!hdc)
        return E_INVALIDARG;

    wined3d_mutex_lock();
    hr = wined3d_texture_get_dc(wined3d_texture_from_resource(resource->wined3d_resource),
            resource->subresource_idx, hdc);
    wined3d_mutex_unlock();

    if (SUCCEEDED(hr))
       resource->dc = *hdc;

    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_ReleaseDC(IDXGISurface2 *iface, RECT *dirty_rect)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    HRESULT hr;

    TRACE("iface %p, rect %s\n", iface, wine_dbgstr_rect(dirty_rect));

    if (!IsRectEmpty(dirty_rect))
        FIXME("dirty rectangle is ignored.\n");

    wined3d_mutex_lock();
    hr = wined3d_texture_release_dc(wined3d_texture_from_resource(resource->wined3d_resource),
            resource->subresource_idx, resource->dc);
    wined3d_mutex_unlock();

    return hr;
}

/* IDXGISurface2 methods */
static HRESULT STDMETHODCALLTYPE dxgi_surface_GetResource(IDXGISurface2 *iface, REFIID iid,
        void **parent_resource, UINT *subresource_idx)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    HRESULT hr;

    TRACE("iface %p, iid %s, parent_resource %p, subresource_idx %p stub!\n", iface,
            wine_dbgstr_guid(iid), parent_resource, subresource_idx);

    if (!parent_resource)
        return E_POINTER;

    if (resource->parent_resource)
        hr = IDXGIResource1_QueryInterface(resource->parent_resource, iid, parent_resource);
    else
        hr = IDXGIResource1_QueryInterface(&resource->IDXGIResource1_iface, iid, parent_resource);

    if (SUCCEEDED(hr))
        *subresource_idx = resource->subresource_idx;
    else
        *parent_resource = NULL;
    return hr;
}

static const struct IDXGISurface2Vtbl dxgi_surface_vtbl =
{
    /* IUnknown methods */
    dxgi_surface_QueryInterface,
    dxgi_surface_AddRef,
    dxgi_surface_Release,
    /* IDXGIObject methods */
    dxgi_surface_SetPrivateData,
    dxgi_surface_SetPrivateDataInterface,
    dxgi_surface_GetPrivateData,
    dxgi_surface_GetParent,
    /* IDXGIDeviceSubObject methods */
    dxgi_surface_GetDevice,
    /* IDXGISurface methods */
    dxgi_surface_GetDesc,
    dxgi_surface_Map,
    dxgi_surface_Unmap,
    /* IDXGISurface1 methods */
    dxgi_surface_GetDC,
    dxgi_surface_ReleaseDC,
    /* IDXGISurface2 methods */
    dxgi_surface_GetResource,
};

static inline struct dxgi_resource *impl_from_IDXGIResource1(IDXGIResource1 *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IDXGIResource1_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE dxgi_resource_QueryInterface(IDXGIResource1 *iface, REFIID riid,
        void **object)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    TRACE("Forwarding to outer IUnknown\n");
    return IUnknown_QueryInterface(resource->outer_unknown, riid, object);
}

static ULONG STDMETHODCALLTYPE dxgi_resource_AddRef(IDXGIResource1 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    TRACE("Forwarding to outer IUnknown\n");
    return IUnknown_AddRef(resource->outer_unknown);
}

static ULONG STDMETHODCALLTYPE dxgi_resource_Release(IDXGIResource1 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    TRACE("Forwarding to outer IUnknown\n");
    return IUnknown_Release(resource->outer_unknown);
}

/* IDXGIObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_resource_SetPrivateData(IDXGIResource1 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_set_private_data(&resource->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_SetPrivateDataInterface(IDXGIResource1 *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);

    return dxgi_set_private_data_interface(&resource->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetPrivateData(IDXGIResource1 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_get_private_data(&resource->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetParent(IDXGIResource1 *iface, REFIID riid, void **parent)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, riid %s, parent %p.\n", iface, debugstr_guid(riid), parent);

    return IDXGIDevice_QueryInterface(resource->device, riid, parent);
}

/* IDXGIDeviceSubObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetDevice(IDXGIResource1 *iface, REFIID riid, void **device)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return IDXGIDevice_QueryInterface(resource->device, riid, device);
}

static inline struct dxgi_resource *impl_from_IDXGIKeyedMutex(IDXGIKeyedMutex *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IDXGIKeyedMutex_iface);
}

/* IDXGIKeyedMutex methods */
static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_QueryInterface(IDXGIKeyedMutex *iface, REFIID riid, void **object)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return resource->outer_unknown->lpVtbl->QueryInterface(resource->outer_unknown, riid, object);
}

static ULONG STDMETHODCALLTYPE dxgi_keyed_mutex_AddRef(IDXGIKeyedMutex *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return resource->outer_unknown->lpVtbl->AddRef(resource->outer_unknown);
}

static ULONG STDMETHODCALLTYPE dxgi_keyed_mutex_Release(IDXGIKeyedMutex *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return resource->outer_unknown->lpVtbl->Release(resource->outer_unknown);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_SetPrivateData(IDXGIKeyedMutex *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return IDXGIResource1_SetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_SetPrivateDataInterface(IDXGIKeyedMutex *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return IDXGIResource1_SetPrivateDataInterface(&resource->IDXGIResource1_iface, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_GetPrivateData(IDXGIKeyedMutex *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return IDXGIResource1_GetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_GetParent(IDXGIKeyedMutex *iface, REFIID riid, void **parent)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return IDXGIResource1_GetParent(&resource->IDXGIResource1_iface, riid, parent);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_GetDevice(IDXGIKeyedMutex *iface, REFIID riid, void **device)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    return IDXGIResource1_GetDevice(&resource->IDXGIResource1_iface, riid, device);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_AcquireSync(IDXGIKeyedMutex *iface, UINT64 key, DWORD dwMilliseconds)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    NTSTATUS status;

    TRACE("iface %p, key %I64u, dwMilliseconds %lu.\n", iface, key, dwMilliseconds);

    EnterCriticalSection(&resource->keyed_mutex_cs);

    if (resource->keyed_mutex_owned)
    {
        LeaveCriticalSection(&resource->keyed_mutex_cs);
        WARN("Keyed mutex is already owned on this device/interface.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (resource->kmt_keyed_mutex)
    {
        LARGE_INTEGER timeout;
        D3DKMT_ACQUIREKEYEDMUTEX2 params = {0};

        params.hKeyedMutex = resource->kmt_keyed_mutex;
        params.Key = key;

        if (dwMilliseconds == INFINITE)
        {
            params.pTimeout = NULL;
        }
        else
        {
            timeout.QuadPart = (LONGLONG)dwMilliseconds * -10000;
            params.pTimeout = &timeout;
        }

        status = NtGdiDdDDIAcquireKeyedMutex2(&params);
        if (status == STATUS_SUCCESS)
        {
            resource->keyed_mutex_owned = TRUE;
            resource->keyed_mutex_key = key;
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return S_OK;
        }
        else if (status == STATUS_TIMEOUT)
        {
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return WAIT_TIMEOUT;
        }
        else if (status == STATUS_ABANDONED)
        {
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return WAIT_ABANDONED;
        }

        LeaveCriticalSection(&resource->keyed_mutex_cs);
        return HRESULT_FROM_NT(status);
    }

    /* Fallback intra-process synchronization */
    while (resource->keyed_mutex_owned || resource->keyed_mutex_key != key)
    {
        if (dwMilliseconds == 0)
        {
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return WAIT_TIMEOUT;
        }
        if (!SleepConditionVariableCS(&resource->keyed_mutex_cv, &resource->keyed_mutex_cs, dwMilliseconds))
        {
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return WAIT_TIMEOUT;
        }
    }

    resource->keyed_mutex_owned = TRUE;
    LeaveCriticalSection(&resource->keyed_mutex_cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_ReleaseSync(IDXGIKeyedMutex *iface, UINT64 key)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    NTSTATUS status;

    TRACE("iface %p, key %I64u.\n", iface, key);

    EnterCriticalSection(&resource->keyed_mutex_cs);

    if (!resource->keyed_mutex_owned)
    {
        LeaveCriticalSection(&resource->keyed_mutex_cs);
        WARN("Keyed mutex is not currently owned.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (resource->kmt_keyed_mutex)
    {
        D3DKMT_RELEASEKEYEDMUTEX2 params = {0};

        params.hKeyedMutex = resource->kmt_keyed_mutex;
        params.Key = key;

        status = NtGdiDdDDIReleaseKeyedMutex2(&params);
        if (status == STATUS_SUCCESS)
        {
            resource->keyed_mutex_owned = FALSE;
            resource->keyed_mutex_key = key;
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return S_OK;
        }

        LeaveCriticalSection(&resource->keyed_mutex_cs);
        return HRESULT_FROM_NT(status);
    }

    /* Fallback intra-process synchronization */
    resource->keyed_mutex_owned = FALSE;
    resource->keyed_mutex_key = key;
    WakeAllConditionVariable(&resource->keyed_mutex_cv);
    LeaveCriticalSection(&resource->keyed_mutex_cs);
    return S_OK;
}

static const struct IDXGIKeyedMutexVtbl dxgi_keyed_mutex_vtbl =
{
    /* IUnknown methods */
    dxgi_keyed_mutex_QueryInterface,
    dxgi_keyed_mutex_AddRef,
    dxgi_keyed_mutex_Release,
    /* IDXGIObject methods */
    dxgi_keyed_mutex_SetPrivateData,
    dxgi_keyed_mutex_SetPrivateDataInterface,
    dxgi_keyed_mutex_GetPrivateData,
    dxgi_keyed_mutex_GetParent,
    /* IDXGIDeviceSubObject methods */
    dxgi_keyed_mutex_GetDevice,
    /* IDXGIKeyedMutex methods */
    dxgi_keyed_mutex_AcquireSync,
    dxgi_keyed_mutex_ReleaseSync,
};

/* IDXGIResource methods */
static HRESULT STDMETHODCALLTYPE dxgi_resource_GetSharedHandle(IDXGIResource1 *iface, HANDLE *shared_handle)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, shared_handle %p.\n", iface, shared_handle);

    if (!shared_handle)
        return E_INVALIDARG;

    if (resource->misc_flags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
    {
        WARN("GetSharedHandle called on NT handle resource, returning E_INVALIDARG.\n");
        return E_INVALIDARG;
    }

    if (!(resource->misc_flags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)))
    {
        *shared_handle = NULL;
        return S_OK;
    }

    if (!resource->kmt_global)
    {
        D3DKMT_CREATEALLOCATION alloc = {0};
        NTSTATUS status;

        alloc.Flags.CreateResource = 1;
        alloc.Flags.CreateShared = 1;
        alloc.Flags.NtSecuritySharing = 0;
        status = NtGdiDdDDICreateAllocation2(&alloc);
        if (status)
            return HRESULT_FROM_NT(status);

        resource->kmt_resource = alloc.hResource;
        resource->kmt_global = alloc.hGlobalShare;
    }

    *shared_handle = ULongToHandle(resource->kmt_global);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetUsage(IDXGIResource1 *iface, DXGI_USAGE *usage)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    struct wined3d_resource_desc resource_desc;

    TRACE("iface %p, usage %p.\n", iface, usage);

    wined3d_resource_get_desc(resource->wined3d_resource, &resource_desc);

    *usage = dxgi_usage_from_wined3d_bind_flags(resource_desc.bind_flags);

    if (resource_desc.resource_type != WINED3D_RTYPE_BUFFER)
    {
        struct wined3d_texture *texture = wined3d_texture_from_resource(resource->wined3d_resource);
        struct wined3d_swapchain_desc swapchain_desc;
        struct wined3d_swapchain *swapchain;

        if ((swapchain = wined3d_texture_get_swapchain(texture)))
        {
            *usage |= DXGI_USAGE_BACK_BUFFER;

            wined3d_swapchain_get_desc(swapchain, &swapchain_desc);
            if (swapchain_desc.swap_effect == WINED3D_SWAP_EFFECT_DISCARD)
                *usage |= DXGI_USAGE_DISCARD_ON_PRESENT;

            if (wined3d_swapchain_get_back_buffer(swapchain, 0) != texture)
                *usage |= DXGI_USAGE_READ_ONLY;
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_SetEvictionPriority(IDXGIResource1 *iface, UINT eviction_priority)
{
    FIXME("iface %p, eviction_priority %u stub!\n", iface, eviction_priority);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetEvictionPriority(IDXGIResource1 *iface, UINT *eviction_priority)
{
    FIXME("iface %p, eviction_priority %p stub!\n", iface, eviction_priority);

    return E_NOTIMPL;
}

/* IDXGIResource1 methods */
static HRESULT STDMETHODCALLTYPE dxgi_resource_CreateSubresourceSurface(IDXGIResource1 *iface,
        UINT index, IDXGISurface2 **surface)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface), *subresource;
    struct wined3d_resource_desc desc;
    unsigned int subresource_count;
    HRESULT hr;

    TRACE("iface %p, index %u, surface %p.\n", iface, index, surface);

    wined3d_mutex_lock();
    wined3d_resource_get_desc(resource->wined3d_resource, &desc);
    subresource_count = wined3d_resource_get_sub_resource_count(resource->wined3d_resource);
    wined3d_mutex_unlock();

    if ((desc.resource_type == WINED3D_RTYPE_TEXTURE_3D && desc.depth > 1)
            || index >= subresource_count)
        return E_INVALIDARG;

    if (!(subresource = calloc(1, sizeof(*subresource))))
    {
        ERR("Failed to allocate DXGI subresource surface object memory.\n");
        return E_OUTOFMEMORY;
    }

    if (FAILED(hr = dxgi_resource_init(subresource, resource->device, NULL, TRUE,
            resource->wined3d_resource, iface, index)))
    {
        WARN("Failed to initialise resource, hr %#lx.\n", hr);
        free(subresource);
        return hr;
    }

    *surface = &subresource->IDXGISurface2_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_CreateSharedHandle(IDXGIResource1 *iface,
        const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    D3DKMT_HANDLE handles[2];
    UINT count = 1;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name_str = {0};
    WCHAR bufferW[MAX_PATH * 2];
    NTSTATUS status;

    TRACE("iface %p, attributes %p, access %#lx, name %s, handle %p.\n", iface, attributes,
            access, wine_dbgstr_w(name), handle);

    if (!handle)
        return E_INVALIDARG;

    *handle = NULL;

    if (!(resource->misc_flags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
    {
        WARN("CreateSharedHandle called on non-NTHANDLE resource, returning E_INVALIDARG.\n");
        return E_INVALIDARG;
    }

    if (!resource->kmt_resource)
    {
        struct d3dkmt_d3d11_desc desc = {0};
        D3DKMT_CREATEALLOCATION alloc = {0};

        desc.dxgi.size = sizeof(desc);
        desc.dxgi.keyed_mutex = resource->has_keyed_mutex;
        desc.dxgi.nt_shared = 1;

        if (resource->outer_unknown)
        {
            ID3D11Texture2D *tex2d;
            if (SUCCEEDED(IUnknown_QueryInterface(resource->outer_unknown, &IID_ID3D11Texture2D, (void **)&tex2d)))
            {
                ID3D11Texture2D_GetDesc(tex2d, &desc.d3d11_2d);
                desc.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
                desc.dxgi.width = desc.d3d11_2d.Width;
                desc.dxgi.height = desc.d3d11_2d.Height;
                desc.dxgi.format = desc.d3d11_2d.Format;
                ID3D11Texture2D_Release(tex2d);
            }
        }

        alloc.Flags.CreateResource = 1;
        alloc.Flags.CreateShared = 1;
        alloc.Flags.NtSecuritySharing = 1;
        alloc.pPrivateRuntimeData = &desc;
        alloc.PrivateRuntimeDataSize = sizeof(desc);

        status = NtGdiDdDDICreateAllocation2(&alloc);
        if (status)
        {
            WARN("Failed to create D3DKMT allocation, status %#lx.\n", status);
            return HRESULT_FROM_NT(status);
        }

        resource->kmt_resource = alloc.hResource;
    }

    if (resource->has_keyed_mutex && !resource->kmt_keyed_mutex)
    {
        D3DKMT_CREATEKEYEDMUTEX2 km_params = {0};

        km_params.InitialValue = 0;
        km_params.Flags.NtSecuritySharing = 1;
        status = NtGdiDdDDICreateKeyedMutex2(&km_params);
        if (status)
        {
            WARN("Failed to create D3DKMT keyed mutex, status %#lx.\n", status);
            return HRESULT_FROM_NT(status);
        }

        resource->kmt_keyed_mutex = km_params.hKeyedMutex;
    }

    handles[0] = resource->kmt_resource;
    if (resource->kmt_keyed_mutex)
    {
        handles[1] = resource->kmt_keyed_mutex;
        count = 2;
    }

    if (name)
    {
        char s_buf[MAX_PATH];
        snprintf(s_buf, sizeof(s_buf), "\\Sessions\\%lu\\BaseNamedObjects\\",
                 (unsigned long)NtCurrentTeb()->Peb->SessionId);
        name_str.Buffer = bufferW;
        name_str.MaximumLength = sizeof(bufferW);
        RtlCreateUnicodeStringFromAsciiz(&name_str, s_buf);
        RtlAppendUnicodeToString(&name_str, name);
        InitializeObjectAttributes(&attr, &name_str, OBJ_CASE_INSENSITIVE | (attributes && attributes->bInheritHandle ? OBJ_INHERIT : 0),
                                   NULL, attributes ? attributes->lpSecurityDescriptor : NULL);
    }
    else
    {
        InitializeObjectAttributes(&attr, NULL, (attributes && attributes->bInheritHandle ? OBJ_INHERIT : 0),
                                   NULL, attributes ? attributes->lpSecurityDescriptor : NULL);
    }

    status = NtGdiDdDDIShareObjects(count, handles, &attr, access, handle);
    if (name)
        RtlFreeUnicodeString(&name_str);

    if (status)
    {
        WARN("NtGdiDdDDIShareObjects failed, status %#lx.\n", status);
        return HRESULT_FROM_NT(status);
    }

    TRACE("Created shared NT handle %p for resource %p.\n", *handle, resource);
    return S_OK;
}

static const struct IDXGIResource1Vtbl dxgi_resource_vtbl =
{
    /* IUnknown methods */
    dxgi_resource_QueryInterface,
    dxgi_resource_AddRef,
    dxgi_resource_Release,
    /* IDXGIObject methods */
    dxgi_resource_SetPrivateData,
    dxgi_resource_SetPrivateDataInterface,
    dxgi_resource_GetPrivateData,
    dxgi_resource_GetParent,
    /* IDXGIDeviceSubObject methods */
    dxgi_resource_GetDevice,
    /* IDXGIResource methods */
    dxgi_resource_GetSharedHandle,
    dxgi_resource_GetUsage,
    dxgi_resource_SetEvictionPriority,
    dxgi_resource_GetEvictionPriority,
    /* IDXGIResource1 methods */
    dxgi_resource_CreateSubresourceSurface,
    dxgi_resource_CreateSharedHandle,
};

static const struct IUnknownVtbl dxgi_resource_inner_unknown_vtbl =
{
    /* IUnknown methods */
    dxgi_resource_inner_QueryInterface,
    dxgi_resource_inner_AddRef,
    dxgi_resource_inner_Release,
};

HRESULT dxgi_resource_init(struct dxgi_resource *resource, IDXGIDevice *device,
        IUnknown *outer, BOOL needs_surface, struct wined3d_resource *wined3d_resource,
        IDXGIResource1 *parent_resource, unsigned int subresource_index)
{
    struct wined3d_resource_desc desc;
    bool is_subresource;

    is_subresource = !!parent_resource;
    wined3d_resource_get_desc(wined3d_resource, &desc);
    if (((desc.resource_type == WINED3D_RTYPE_TEXTURE_1D || desc.resource_type == WINED3D_RTYPE_TEXTURE_2D)
            && needs_surface) || is_subresource)
    {
        resource->IDXGISurface2_iface.lpVtbl = &dxgi_surface_vtbl;
    }
    else
        resource->IDXGISurface2_iface.lpVtbl = NULL;
    resource->IDXGIResource1_iface.lpVtbl = &dxgi_resource_vtbl;
    resource->IDXGIKeyedMutex_iface.lpVtbl = &dxgi_keyed_mutex_vtbl;
    resource->IUnknown_iface.lpVtbl = &dxgi_resource_inner_unknown_vtbl;
    resource->refcount = 1;
    wined3d_private_store_init(&resource->private_store);
    resource->outer_unknown = outer ? outer : &resource->IUnknown_iface;
    resource->device = device;
    resource->wined3d_resource = wined3d_resource;
    resource->dc = NULL;
    resource->has_keyed_mutex = FALSE;
    resource->keyed_mutex_owned = FALSE;
    resource->keyed_mutex_key = 0;
    InitializeCriticalSection(&resource->keyed_mutex_cs);
    InitializeConditionVariable(&resource->keyed_mutex_cv);
    resource->kmt_resource = 0;
    resource->kmt_keyed_mutex = 0;
    resource->kmt_global = 0;
    resource->misc_flags = 0;

    if (outer && !is_subresource)
    {
        ID3D11Texture2D *tex2d;
        ID3D11Buffer *buf;
        ID3D11Texture1D *tex1d;
        ID3D11Texture3D *tex3d;

        if (SUCCEEDED(IUnknown_QueryInterface(outer, &IID_ID3D11Texture2D, (void **)&tex2d)))
        {
            D3D11_TEXTURE2D_DESC d3d_desc;
            ID3D11Texture2D_GetDesc(tex2d, &d3d_desc);
            resource->misc_flags = d3d_desc.MiscFlags;
            if (d3d_desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
                resource->has_keyed_mutex = TRUE;
            ID3D11Texture2D_Release(tex2d);
        }
        else if (SUCCEEDED(IUnknown_QueryInterface(outer, &IID_ID3D11Buffer, (void **)&buf)))
        {
            D3D11_BUFFER_DESC d3d_desc;
            ID3D11Buffer_GetDesc(buf, &d3d_desc);
            resource->misc_flags = d3d_desc.MiscFlags;
            if (d3d_desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
                resource->has_keyed_mutex = TRUE;
            ID3D11Buffer_Release(buf);
        }
        else if (SUCCEEDED(IUnknown_QueryInterface(outer, &IID_ID3D11Texture1D, (void **)&tex1d)))
        {
            D3D11_TEXTURE1D_DESC d3d_desc;
            ID3D11Texture1D_GetDesc(tex1d, &d3d_desc);
            resource->misc_flags = d3d_desc.MiscFlags;
            if (d3d_desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
                resource->has_keyed_mutex = TRUE;
            ID3D11Texture1D_Release(tex1d);
        }
        else if (SUCCEEDED(IUnknown_QueryInterface(outer, &IID_ID3D11Texture3D, (void **)&tex3d)))
        {
            D3D11_TEXTURE3D_DESC d3d_desc;
            ID3D11Texture3D_GetDesc(tex3d, &d3d_desc);
            resource->misc_flags = d3d_desc.MiscFlags;
            if (d3d_desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
                resource->has_keyed_mutex = TRUE;
            ID3D11Texture3D_Release(tex3d);
        }
    }

    if (is_subresource)
    {
        resource->parent_resource = parent_resource;
        IDXGIResource1_AddRef(parent_resource);
        resource->subresource_idx = subresource_index;
    }
    else
    {
        resource->parent_resource = NULL;
        resource->subresource_idx = 0;
    }

    return S_OK;
}
