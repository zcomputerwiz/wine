/*
 * Copyright 2008 Henri Verbeet for CodeWeavers
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
#include "wine/dcomp_underlay.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

static inline struct dxgi_factory *impl_from_IWineDXGIFactory(IWineDXGIFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_factory, IWineDXGIFactory_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_QueryInterface(IWineDXGIFactory *iface, REFIID iid, void **out)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IWineDXGIFactory)
            || IsEqualGUID(iid, &IID_IDXGIFactory7)
            || IsEqualGUID(iid, &IID_IDXGIFactory6)
            || IsEqualGUID(iid, &IID_IDXGIFactory5)
            || IsEqualGUID(iid, &IID_IDXGIFactory4)
            || IsEqualGUID(iid, &IID_IDXGIFactory3)
            || IsEqualGUID(iid, &IID_IDXGIFactory2)
            || (factory->extended && IsEqualGUID(iid, &IID_IDXGIFactory1))
            || IsEqualGUID(iid, &IID_IDXGIFactory)
            || IsEqualGUID(iid, &IID_IDXGIObject)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxgi_factory_AddRef(IWineDXGIFactory *iface)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    ULONG refcount = InterlockedIncrement(&factory->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE dxgi_factory_Release(IWineDXGIFactory *iface)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        struct dxgi_adapters_changed *entry, *next;

        LIST_FOR_EACH_ENTRY_SAFE(entry, next, &factory->adapters_changed, struct dxgi_adapters_changed, entry)
        {
            CloseHandle(entry->event);
            free(entry);
        }
        LIST_FOR_EACH_ENTRY_SAFE(entry, next, &factory->occlusion_events, struct dxgi_adapters_changed, entry)
        {
            CloseHandle(entry->event);
            free(entry);
        }

        if (factory->device_window)
            DestroyWindow(factory->device_window);

        wined3d_decref(factory->wined3d);
        wined3d_private_store_cleanup(&factory->private_store);
        free(factory);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_SetPrivateData(IWineDXGIFactory *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_set_private_data(&factory->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_SetPrivateDataInterface(IWineDXGIFactory *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);

    return dxgi_set_private_data_interface(&factory->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetPrivateData(IWineDXGIFactory *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_get_private_data(&factory->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetParent(IWineDXGIFactory *iface, REFIID iid, void **parent)
{
    WARN("iface %p, iid %s, parent %p.\n", iface, debugstr_guid(iid), parent);

    *parent = NULL;

    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapters1(IWineDXGIFactory *iface,
        UINT adapter_idx, IDXGIAdapter1 **adapter)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    struct dxgi_adapter *adapter_object;
    UINT adapter_count;
    HRESULT hr;

    TRACE("iface %p, adapter_idx %u, adapter %p.\n", iface, adapter_idx, adapter);

    if (!adapter)
        return DXGI_ERROR_INVALID_CALL;

    wined3d_mutex_lock();
    adapter_count = wined3d_get_adapter_count(factory->wined3d);
    wined3d_mutex_unlock();

    if (adapter_idx >= adapter_count)
    {
        *adapter = NULL;
        return DXGI_ERROR_NOT_FOUND;
    }

    if (FAILED(hr = dxgi_adapter_create(factory, adapter_idx, &adapter_object)))
    {
        *adapter = NULL;
        return hr;
    }

    *adapter = (IDXGIAdapter1 *)&adapter_object->IWineDXGIAdapter_iface;

    TRACE("Returning adapter %p.\n", *adapter);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapters(IWineDXGIFactory *iface,
        UINT adapter_idx, IDXGIAdapter **adapter)
{
    TRACE("iface %p, adapter_idx %u, adapter %p.\n", iface, adapter_idx, adapter);

    return dxgi_factory_EnumAdapters1(iface, adapter_idx, (IDXGIAdapter1 **)adapter);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_MakeWindowAssociation(IWineDXGIFactory *iface,
        HWND window, UINT flags)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, window %p, flags %#x.\n", iface, window, flags);

    if (flags > DXGI_MWA_VALID)
        return DXGI_ERROR_INVALID_CALL;

    if (!window)
    {
        wined3d_unregister_windows(factory->wined3d);
        return S_OK;
    }

    if (!wined3d_register_window(factory->wined3d, window, NULL, flags))
        return E_FAIL;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetWindowAssociation(IWineDXGIFactory *iface, HWND *window)
{
    TRACE("iface %p, window %p.\n", iface, window);

    if (!window)
        return DXGI_ERROR_INVALID_CALL;

    /* The tests show that this always returns NULL for some unknown reason. */
    *window = NULL;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChain(IWineDXGIFactory *iface,
        IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapchain)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;

    TRACE("iface %p, device %p, desc %p, swapchain %p.\n", iface, device, desc, swapchain);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    swapchain_desc.Width = desc->BufferDesc.Width;
    swapchain_desc.Height = desc->BufferDesc.Height;
    swapchain_desc.Format = desc->BufferDesc.Format;
    swapchain_desc.Stereo = FALSE;
    swapchain_desc.SampleDesc = desc->SampleDesc;
    swapchain_desc.BufferUsage = desc->BufferUsage;
    swapchain_desc.BufferCount = desc->BufferCount;
    swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
    swapchain_desc.SwapEffect = desc->SwapEffect;
    swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapchain_desc.Flags = desc->Flags;

    fullscreen_desc.RefreshRate = desc->BufferDesc.RefreshRate;
    fullscreen_desc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreen_desc.Scaling = desc->BufferDesc.Scaling;
    fullscreen_desc.Windowed = desc->Windowed;

    return IWineDXGIFactory_CreateSwapChainForHwnd(&factory->IWineDXGIFactory_iface,
            device, desc->OutputWindow, &swapchain_desc, &fullscreen_desc, NULL,
            (IDXGISwapChain1 **)swapchain);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSoftwareAdapter(IWineDXGIFactory *iface,
        HMODULE swrast, IDXGIAdapter **adapter)
{
    FIXME("iface %p, swrast %p, adapter %p stub!\n", iface, swrast, adapter);

    return E_NOTIMPL;
}

static BOOL STDMETHODCALLTYPE dxgi_factory_IsCurrent(IWineDXGIFactory *iface)
{
    static BOOL once = FALSE;

    if (!once++)
        FIXME("iface %p stub!\n", iface);
    else
        WARN("iface %p stub!\n", iface);

    return TRUE;
}

static BOOL STDMETHODCALLTYPE dxgi_factory_IsWindowedStereoEnabled(IWineDXGIFactory *iface)
{
    FIXME("iface %p stub!\n", iface);

    return FALSE;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChainForHwnd(IWineDXGIFactory *iface,
        IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
        IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC windowed_fullscreen_desc = {0};
    IWineDXGISwapChainFactory *swapchain_factory;
    ID3D12CommandQueue *command_queue;
    HRESULT hr;

    TRACE("iface %p, device %p, window %p, desc %p, fullscreen_desc %p, output %p, swapchain %p.\n",
            iface, device, window, desc, fullscreen_desc, output, swapchain);

    if (!device || !window || !desc || !swapchain)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (desc->Stereo)
    {
        FIXME("Stereo swapchains are not supported.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    if (!dxgi_validate_swapchain_desc(desc))
        return DXGI_ERROR_INVALID_CALL;

    if (!fullscreen_desc || !dxgi_validate_swapchain_fullscreen_desc(fullscreen_desc))
    {
        if (fullscreen_desc)
            windowed_fullscreen_desc = *fullscreen_desc;
        windowed_fullscreen_desc.Windowed = TRUE;
        fullscreen_desc = &windowed_fullscreen_desc;
    }

    if (output)
        FIXME("Ignoring output %p.\n", output);

    if (SUCCEEDED(IUnknown_QueryInterface(device, &IID_IWineDXGISwapChainFactory, (void **)&swapchain_factory)))
    {
        hr = IWineDXGISwapChainFactory_create_swapchain(swapchain_factory,
                (IDXGIFactory *)iface, window, desc, fullscreen_desc, output, swapchain);
        IWineDXGISwapChainFactory_Release(swapchain_factory);
        return hr;
    }

    if (SUCCEEDED(IUnknown_QueryInterface(device, &IID_ID3D12CommandQueue, (void **)&command_queue)))
    {
        hr = d3d12_swapchain_create(iface, command_queue, window, desc, fullscreen_desc, swapchain);
        ID3D12CommandQueue_Release(command_queue);
        return hr;
    }

    ERR("This is not the device we're looking for.\n");
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChainForCoreWindow(IWineDXGIFactory *iface,
        IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
        IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    /* ICoreWindowInterop (windows.ui.core.corewindow.interop.idl) */
    static const GUID iid_core_window_interop =
            {0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};
    struct core_window_interop_vtbl
    {
        HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *iface, REFIID iid, void **out);
        ULONG (STDMETHODCALLTYPE *AddRef)(void *iface);
        ULONG (STDMETHODCALLTYPE *Release)(void *iface);
        HRESULT (STDMETHODCALLTYPE *get_WindowHandle)(void *iface, HWND *hwnd);
    } **interop;
    HWND hwnd = NULL;
    HRESULT hr;

    TRACE("iface %p, device %p, window %p, desc %p, output %p, swapchain %p.\n",
            iface, device, window, desc, output, swapchain);

    if (!window || !swapchain)
        return DXGI_ERROR_INVALID_CALL;

    /* the native window of a CoreWindow is the target of the swap chain */
    if (FAILED(hr = IUnknown_QueryInterface(window, &iid_core_window_interop, (void **)&interop)))
    {
        WARN("The window does not provide ICoreWindowInterop, hr %#lx.\n", hr);
        return DXGI_ERROR_INVALID_CALL;
    }
    hr = (*interop)->get_WindowHandle(interop, &hwnd);
    (*interop)->Release(interop);
    if (FAILED(hr) || !hwnd)
        return FAILED(hr) ? hr : DXGI_ERROR_INVALID_CALL;

    return dxgi_factory_CreateSwapChainForHwnd(iface, device, hwnd, desc, NULL, output, swapchain);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetSharedResourceAdapterLuid(IWineDXGIFactory *iface,
        HANDLE resource, LUID *luid)
{
    FIXME("iface %p, resource %p, luid %p stub!\n", iface, resource, luid);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterOcclusionStatusWindow(IWineDXGIFactory *iface,
        HWND window, UINT message, DWORD *cookie)
{
    FIXME("iface %p, window %p, message %#x, cookie %p stub!\n",
            iface, window, message, cookie);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterStereoStatusEvent(IWineDXGIFactory *iface,
        HANDLE event, DWORD *cookie)
{
    FIXME("iface %p, event %p, cookie %p stub!\n", iface, event, cookie);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE dxgi_factory_UnregisterStereoStatus(IWineDXGIFactory *iface, DWORD cookie)
{
    FIXME("iface %p, cookie %#lx stub!\n", iface, cookie);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterStereoStatusWindow(IWineDXGIFactory *iface,
        HWND window, UINT message, DWORD *cookie)
{
    FIXME("iface %p, window %p, message %#x, cookie %p stub!\n",
            iface, window, message, cookie);

    return E_NOTIMPL;
}

/* As measured on Windows 11 (WineExplorer tests/uwp/alpc/occlusion-kmt-probe.c): the event is set at once, reporting the current
 * status; cookies count from 1 and the smallest free one is used again; a NULL event or cookie, and an event already registered, give
 * DXGI_ERROR_INVALID_CALL. Windows are never reported as occluded here, so the event is not set again later. The compositor of the
 * Windows App SDK waits for this event before it renders. */
static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterOcclusionStatusEvent(IWineDXGIFactory *iface,
        HANDLE event, DWORD *cookie)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    struct dxgi_adapters_changed *entry, *other;
    HANDLE duplicate;
    DWORD free_cookie;
    BOOL taken;

    TRACE("iface %p, event %p, cookie %p.\n", iface, event, cookie);

    if (!event || !cookie)
        return DXGI_ERROR_INVALID_CALL;

    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return DXGI_ERROR_INVALID_CALL;

    if (!(entry = calloc(1, sizeof(*entry))))
    {
        CloseHandle(duplicate);
        return E_OUTOFMEMORY;
    }
    entry->event = duplicate;

    wined3d_mutex_lock();
    LIST_FOR_EACH_ENTRY(other, &factory->occlusion_events, struct dxgi_adapters_changed, entry)
    {
        if (!NtCompareObjects(other->event, duplicate))   /* the same event again */
        {
            wined3d_mutex_unlock();
            CloseHandle(duplicate);
            free(entry);
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    for (free_cookie = 1;; ++free_cookie)
    {
        taken = FALSE;
        LIST_FOR_EACH_ENTRY(other, &factory->occlusion_events, struct dxgi_adapters_changed, entry)
            if (other->cookie == free_cookie) taken = TRUE;
        if (!taken) break;
    }
    entry->cookie = free_cookie;
    list_add_tail(&factory->occlusion_events, &entry->entry);
    *cookie = entry->cookie;
    SetEvent(entry->event);
    wined3d_mutex_unlock();

    return S_OK;
}

static void STDMETHODCALLTYPE dxgi_factory_UnregisterOcclusionStatus(IWineDXGIFactory *iface, DWORD cookie)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    struct dxgi_adapters_changed *entry;

    TRACE("iface %p, cookie %#lx.\n", iface, cookie);

    wined3d_mutex_lock();
    LIST_FOR_EACH_ENTRY(entry, &factory->occlusion_events, struct dxgi_adapters_changed, entry)
    {
        if (entry->cookie != cookie) continue;
        list_remove(&entry->entry);
        wined3d_mutex_unlock();
        CloseHandle(entry->event);
        free(entry);
        return;
    }
    wined3d_mutex_unlock();
}

/* The swapchain is no longer a composition target's content: Present draws into its own window again. */
void dcomp_swapchain_clear_target(struct d3d11_swapchain *swapchain)
{
    wined3d_mutex_lock();
    swapchain->target_hwnd = NULL;
    wined3d_mutex_unlock();
}

static LRESULT CALLBACK dcomp_swapchain_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_WINE_DCOMP_REPAINT:
        {
            MSG pending;
            unsigned int count;
            /* Coalesce a bounded burst, then render current retained state,
             * never a stale target or a caller-owned packet. */
            for (count = 0; count < 64; ++count)
                if (!PeekMessageW(&pending, hwnd, WM_WINE_DCOMP_REPAINT, WM_WINE_DCOMP_REPAINT, PM_REMOVE)) break;
            dcomp_swapchain_repaint(hwnd);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static BOOL dcomp_register_window_class(void)
{
    static BOOL registered;
    WNDCLASSW wc;

    if (registered)
        return TRUE;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dcomp_swapchain_wndproc;
    wc.style = CS_OWNDC;
    wc.lpszClassName = L"WineDCompSwapchain";
    wc.hInstance = GetModuleHandleW(NULL);

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ERR("Failed to register WineDCompSwapchain class, error %lu.\n", GetLastError());
        return FALSE;
    }

    registered = TRUE;
    return TRUE;
}

/* The repaint sinks of composition targets live on a thread of dxgi's own that always handles messages:
 * WM_WINE_DCOMP_REPAINT, which asks for a target's frames again, must reach them while the thread that made the
 * swapchain handles none (Chromium's compositor thread, say). Only the message-only sinks live there; the
 * composition windows themselves stay with the swapchain's thread. */
#define WM_DCOMP_HELPER_CREATE_SINK (WM_USER + 1)

static HWND dcomp_helper;
static INIT_ONCE dcomp_helper_once = INIT_ONCE_STATIC_INIT;

static LRESULT CALLBACK dcomp_sink_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_WINE_DCOMP_REPAINT)
    {
        MSG pending;
        unsigned int count;

        for (count = 0; count < 64; ++count)
            if (!PeekMessageW(&pending, hwnd, WM_WINE_DCOMP_REPAINT, WM_WINE_DCOMP_REPAINT, PM_REMOVE)) break;
        dcomp_swapchain_repaint(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK dcomp_helper_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_DCOMP_HELPER_CREATE_SINK)
        return (LRESULT)CreateWindowExW(0, L"WineDCompRepaintSink", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                GetModuleHandleW(NULL), NULL);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI dcomp_helper_thread(void *ready)
{
    WNDCLASSW wc = {0};
    MSG msg;

    SetThreadDescription(GetCurrentThread(), L"wine_dxgi_dcomp");
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = dcomp_sink_wndproc;
    wc.lpszClassName = L"WineDCompRepaintSink";
    if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
    {
        wc.lpfnWndProc = dcomp_helper_wndproc;
        wc.lpszClassName = L"WineDCompHelper";
        if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            dcomp_helper = CreateWindowExW(0, L"WineDCompHelper", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                    GetModuleHandleW(NULL), NULL);
    }
    SetEvent(ready);
    if (!dcomp_helper) return 1;
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static BOOL WINAPI dcomp_helper_init(INIT_ONCE *once, void *param, void **context)
{
    HANDLE ready, thread;

    if (!(ready = CreateEventW(NULL, TRUE, FALSE, NULL))) return TRUE;
    if ((thread = CreateThread(NULL, 0, dcomp_helper_thread, ready, 0, NULL)))
    {
        WaitForSingleObject(ready, INFINITE);
        CloseHandle(thread);
    }
    CloseHandle(ready);
    if (!dcomp_helper) WARN("No repaint thread; the composition windows receive their repaints themselves.\n");
    return TRUE;
}

/* NULL without dxgi's thread: the composition window is its own sink then. */
HWND dcomp_create_repaint_sink(void)
{
    InitOnceExecuteOnce(&dcomp_helper_once, dcomp_helper_init, NULL, NULL);
    if (!dcomp_helper) return NULL;
    return (HWND)SendMessageW(dcomp_helper, WM_DCOMP_HELPER_CREATE_SINK, 0, 0);
}

BOOL dcomp_window_on_helper_thread(HWND window)
{
    return dcomp_helper && GetWindowThreadProcessId(window, NULL) == GetWindowThreadProcessId(dcomp_helper, NULL);
}

/* Only the window's own thread can destroy it. */
void dcomp_destroy_window(HWND window)
{
    if (GetWindowThreadProcessId(window, NULL) == GetCurrentThreadId()) DestroyWindow(window);
    else SendMessageW(window, WM_CLOSE, 0, 0);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChainForComposition(IWineDXGIFactory *iface,
        IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    HWND window;
    HRESULT hr;

    TRACE("iface %p, device %p, desc %p, output %p, swapchain %p.\n",
            iface, device, desc, output, swapchain);

    if (!device || !desc || !swapchain)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!dcomp_register_window_class())
        return E_FAIL;

    window = CreateWindowExW(WS_EX_NOPARENTNOTIFY,
            L"WineDCompSwapchain", L"DComp Swapchain",
            WS_CHILD, 0, 0, desc->Width, desc->Height,
            GetDesktopWindow(), NULL, GetModuleHandleW(NULL), NULL);
    if (!window)
    {
        ERR("Failed to create composition window, error %lu.\n", GetLastError());
        return E_FAIL;
    }

    hr = dxgi_factory_CreateSwapChainForHwnd(iface, device, window,
            desc, NULL, output, swapchain);
    if (SUCCEEDED(hr))
    {
        struct d3d11_swapchain *d3d11_sc = d3d11_swapchain_from_composition_candidate(*swapchain);
        if (!d3d11_sc)
        {
            /* A D3D12/custom swapchain is not a d3d11_swapchain structure. */
            IDXGISwapChain1_Release(*swapchain);
            *swapchain = NULL;
            DestroyWindow(window);
            return E_NOTIMPL;
        }
        d3d11_sc->comp_wnd = window;
        if (!(d3d11_sc->sink_wnd = dcomp_create_repaint_sink())) d3d11_sc->sink_wnd = window;
        d3d11_sc->composition_alpha_mode = desc->AlphaMode;

        SetPropW(window, L"__wine_dcomp_swapchain", (HANDLE)*swapchain);
        if (d3d11_sc->sink_wnd != window)
            SetPropW(d3d11_sc->sink_wnd, L"__wine_dcomp_swapchain", (HANDLE)*swapchain);
        TRACE("Created composition window %p (%ux%u) for swapchain %p.\n",
                window, desc->Width, desc->Height, *swapchain);
    }
    else
    {
        ERR("CreateSwapChainForHwnd failed, hr %#lx.\n", hr);
        DestroyWindow(window);
    }

    return hr;
}

static UINT STDMETHODCALLTYPE dxgi_factory_GetCreationFlags(IWineDXGIFactory *iface)
{
    FIXME("iface %p stub!\n", iface);

    return 0;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapterByLuid(IWineDXGIFactory *iface,
        LUID luid, REFIID iid, void **adapter)
{
    unsigned int adapter_index;
    DXGI_ADAPTER_DESC1 desc;
    IDXGIAdapter1 *adapter1;
    HRESULT hr;

    TRACE("iface %p, luid %08lx:%08lx, iid %s, adapter %p.\n",
            iface, luid.HighPart, luid.LowPart, debugstr_guid(iid), adapter);

    if (!adapter)
        return DXGI_ERROR_INVALID_CALL;

    adapter_index = 0;
    while ((hr = dxgi_factory_EnumAdapters1(iface, adapter_index, &adapter1)) == S_OK)
    {
        if (FAILED(hr = IDXGIAdapter1_GetDesc1(adapter1, &desc)))
        {
            WARN("Failed to get adapter %u desc, hr %#lx.\n", adapter_index, hr);
            ++adapter_index;
            continue;
        }

        if (desc.AdapterLuid.LowPart == luid.LowPart
                && desc.AdapterLuid.HighPart == luid.HighPart)
        {
            hr = IDXGIAdapter1_QueryInterface(adapter1, iid, adapter);
            IDXGIAdapter1_Release(adapter1);
            return hr;
        }

        IDXGIAdapter1_Release(adapter1);
        ++adapter_index;
    }
    if (hr != DXGI_ERROR_NOT_FOUND)
        WARN("Failed to enumerate adapters, hr %#lx.\n", hr);

    WARN("Adapter could not be found.\n");
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumWarpAdapter(IWineDXGIFactory *iface,
        REFIID iid, void **adapter)
{
    IDXGIAdapter1 *adapter_object;
    HRESULT hr;

    FIXME("iface %p, iid %s, adapter %p semi-stub, returning a hardware adapter.\n",
            iface, debugstr_guid(iid), adapter);

    if (!adapter)
        return DXGI_ERROR_INVALID_CALL;

    if (FAILED(hr = dxgi_factory_EnumAdapters1(iface, 0, &adapter_object)))
        return hr;

    hr = IDXGIAdapter1_QueryInterface(adapter_object, iid, adapter);
    IDXGIAdapter1_Release(adapter_object);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CheckFeatureSupport(IWineDXGIFactory *iface,
        DXGI_FEATURE feature, void *feature_data, UINT data_size)
{
    TRACE("iface %p, feature %#x, feature_data %p, data_size %u.\n",
            iface, feature, feature_data, data_size);

    switch (feature)
    {
        case DXGI_FEATURE_PRESENT_ALLOW_TEARING:
            if (data_size != sizeof(BOOL))
                return DXGI_ERROR_INVALID_CALL;
            *(BOOL *)feature_data = TRUE;
            return S_OK;

        default:
            WARN("Unsupported feature %#x.\n", feature);
            return DXGI_ERROR_INVALID_CALL;
    }
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapterByGpuPreference(IWineDXGIFactory *iface,
        UINT adapter_idx, DXGI_GPU_PREFERENCE gpu_preference, REFIID iid, void **adapter)
{
    IDXGIAdapter1 *adapter_object;
    HRESULT hr;

    TRACE("iface %p, adapter_idx %u, gpu_preference %#x, iid %s, adapter %p.\n",
            iface, adapter_idx, gpu_preference, debugstr_guid(iid), adapter);

    if (gpu_preference != DXGI_GPU_PREFERENCE_UNSPECIFIED)
        FIXME("Ignoring GPU preference %#x.\n", gpu_preference);

    if (FAILED(hr = dxgi_factory_EnumAdapters1(iface, adapter_idx, &adapter_object)))
        return hr;

    hr = IDXGIAdapter1_QueryInterface(adapter_object, iid, adapter);
    IDXGIAdapter1_Release(adapter_object);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterAdaptersChangedEvent(IWineDXGIFactory *iface,
        HANDLE event, DWORD *cookie)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    struct dxgi_adapters_changed *entry, *other;
    HANDLE duplicate;

    TRACE("iface %p, event %p, cookie %p.\n", iface, event, cookie);

    /* The adapters of this process never change, so the event is never signaled; as on Windows the registration keeps
     * a duplicate of the handle (the composition engine of the Windows App Runtime treats a failure here as fatal). */
    if (!event || !cookie)
        return DXGI_ERROR_INVALID_CALL;

    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return E_OUTOFMEMORY;

    if (!(entry = calloc(1, sizeof(*entry))))
    {
        CloseHandle(duplicate);
        return E_OUTOFMEMORY;
    }
    entry->event = duplicate;

    wined3d_mutex_lock();
    LIST_FOR_EACH_ENTRY(other, &factory->adapters_changed, struct dxgi_adapters_changed, entry)
    {
        if (!NtCompareObjects(other->event, duplicate))   /* the same event again */
        {
            wined3d_mutex_unlock();
            CloseHandle(duplicate);
            free(entry);
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    entry->cookie = ++factory->next_adapters_changed_cookie;
    list_add_tail(&factory->adapters_changed, &entry->entry);
    *cookie = entry->cookie;
    wined3d_mutex_unlock();

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_UnregisterAdaptersChangedEvent(IWineDXGIFactory *iface,
        DWORD cookie)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    struct dxgi_adapters_changed *entry;

    TRACE("iface %p, cookie %#lx.\n", iface, cookie);

    wined3d_mutex_lock();
    LIST_FOR_EACH_ENTRY(entry, &factory->adapters_changed, struct dxgi_adapters_changed, entry)
    {
        if (entry->cookie != cookie) continue;
        list_remove(&entry->entry);
        wined3d_mutex_unlock();
        CloseHandle(entry->event);
        free(entry);
        return S_OK;
    }
    wined3d_mutex_unlock();

    return DXGI_ERROR_NOT_FOUND;
}

static const struct IWineDXGIFactoryVtbl dxgi_factory_vtbl =
{
    dxgi_factory_QueryInterface,
    dxgi_factory_AddRef,
    dxgi_factory_Release,
    dxgi_factory_SetPrivateData,
    dxgi_factory_SetPrivateDataInterface,
    dxgi_factory_GetPrivateData,
    dxgi_factory_GetParent,
    dxgi_factory_EnumAdapters,
    dxgi_factory_MakeWindowAssociation,
    dxgi_factory_GetWindowAssociation,
    dxgi_factory_CreateSwapChain,
    dxgi_factory_CreateSoftwareAdapter,
    /* IDXGIFactory1 methods */
    dxgi_factory_EnumAdapters1,
    dxgi_factory_IsCurrent,
    /* IDXGIFactory2 methods */
    dxgi_factory_IsWindowedStereoEnabled,
    dxgi_factory_CreateSwapChainForHwnd,
    dxgi_factory_CreateSwapChainForCoreWindow,
    dxgi_factory_GetSharedResourceAdapterLuid,
    dxgi_factory_RegisterStereoStatusWindow,
    dxgi_factory_RegisterStereoStatusEvent,
    dxgi_factory_UnregisterStereoStatus,
    dxgi_factory_RegisterOcclusionStatusWindow,
    dxgi_factory_RegisterOcclusionStatusEvent,
    dxgi_factory_UnregisterOcclusionStatus,
    dxgi_factory_CreateSwapChainForComposition,
    /* IDXGIFactory3 methods */
    dxgi_factory_GetCreationFlags,
    /* IDXGIFactory4 methods */
    dxgi_factory_EnumAdapterByLuid,
    dxgi_factory_EnumWarpAdapter,
    /* IDXIGFactory5 methods */
    dxgi_factory_CheckFeatureSupport,
    /* IDXGIFactory6 methods */
    dxgi_factory_EnumAdapterByGpuPreference,
    /* IDXGIFactory7 methods */
    dxgi_factory_RegisterAdaptersChangedEvent,
    dxgi_factory_UnregisterAdaptersChangedEvent,
};

struct dxgi_factory *unsafe_impl_from_IDXGIFactory(IDXGIFactory *iface)
{
    IWineDXGIFactory *wine_factory;
    struct dxgi_factory *factory;
    HRESULT hr;

    if (!iface)
        return NULL;
    if (FAILED(hr = IDXGIFactory_QueryInterface(iface, &IID_IWineDXGIFactory, (void **)&wine_factory)))
    {
        ERR("Failed to get IWineDXGIFactory interface, hr %#lx.\n", hr);
        return NULL;
    }
    assert(wine_factory->lpVtbl == &dxgi_factory_vtbl);
    factory = CONTAINING_RECORD(wine_factory, struct dxgi_factory, IWineDXGIFactory_iface);
    IWineDXGIFactory_Release(wine_factory);
    return factory;
}

static HRESULT dxgi_factory_init(struct dxgi_factory *factory, BOOL extended)
{
    factory->IWineDXGIFactory_iface.lpVtbl = &dxgi_factory_vtbl;
    factory->refcount = 1;
    list_init(&factory->adapters_changed);
    list_init(&factory->occlusion_events);
    wined3d_private_store_init(&factory->private_store);

    wined3d_mutex_lock();
    factory->wined3d = wined3d_create(0);
    wined3d_mutex_unlock();
    if (!factory->wined3d)
    {
        wined3d_private_store_cleanup(&factory->private_store);
        return DXGI_ERROR_UNSUPPORTED;
    }

    factory->extended = extended;

    return S_OK;
}

HRESULT dxgi_factory_create(REFIID riid, void **factory, BOOL extended)
{
    struct dxgi_factory *object;
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = dxgi_factory_init(object, extended)))
    {
        WARN("Failed to initialize factory, hr %#lx.\n", hr);
        free(object);
        return hr;
    }

    TRACE("Created factory %p.\n", object);

    hr = IWineDXGIFactory_QueryInterface(&object->IWineDXGIFactory_iface, riid, factory);
    IWineDXGIFactory_Release(&object->IWineDXGIFactory_iface);
    return hr;
}

HWND dxgi_factory_get_device_window(struct dxgi_factory *factory)
{
    wined3d_mutex_lock();

    if (!factory->device_window)
    {
        if (!(factory->device_window = CreateWindowA("static", "DXGI device window",
                WS_DISABLED, 0, 0, 0, 0, NULL, NULL, NULL, NULL)))
        {
            wined3d_mutex_unlock();
            ERR("Failed to create a window.\n");
            return NULL;
        }
        SetWindowPos(factory->device_window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        TRACE("Created device window %p for factory %p.\n", factory->device_window, factory);
    }

    wined3d_mutex_unlock();

    return factory->device_window;
}
