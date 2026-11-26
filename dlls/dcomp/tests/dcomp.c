/*
 * Unit test for DirectComposition
 *
 * Copyright 2023 Zhiyi Zhang for CodeWeavers
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

#define COBJMACROS
#include "initguid.h"
#include <math.h>
#include <d2d1_1.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include "dcomp.h"
#include "wine/test.h"

static HRESULT (WINAPI *pDCompositionCreateDevice)(IDXGIDevice *dxgi_device, REFIID iid, void **device);
static HRESULT (WINAPI *pDCompositionCreateDevice2)(IUnknown *rendering_device, REFIID iid, void **device);
static HRESULT (WINAPI *pDCompositionCreateDevice3)(IUnknown *rendering_device, REFIID iid, void **device);

typedef struct IDCompositionSurfaceUnknown IDCompositionSurfaceUnknown;
typedef struct IDCompositionSurfaceUnknownVtbl IDCompositionSurfaceUnknownVtbl;
struct IDCompositionSurfaceUnknown { const IDCompositionSurfaceUnknownVtbl *lpVtbl; };
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
#define IDCompositionSurfaceUnknown_Unknown3(iface, width, height) \
    ((iface)->lpVtbl->Unknown[2]((iface), (width), (height)))


#define expect_ref(obj,ref) _expect_ref((IUnknown *)obj, ref, __LINE__)
static void _expect_ref(IUnknown *obj, ULONG ref, int line)
{
    ULONG rc;
    IUnknown_AddRef(obj);
    rc = IUnknown_Release(obj);
    ok_(__FILE__, line)(rc == ref, "expected refcount %ld, got %ld\n", ref, rc);
}

#define check_interface(a, b, c) check_interface_(__LINE__, a, b, c)
static void check_interface_(unsigned int line, void *iface_ptr, REFIID iid, BOOL supported)
{
    IUnknown *iface = iface_ptr;
    HRESULT hr, expected;
    IUnknown *unk;

    expected = supported ? S_OK : E_NOINTERFACE;
    hr = IUnknown_QueryInterface(iface, iid, (void **)&unk);
    ok_(__FILE__, line)(hr == expected, "got hr %#lx, expected %#lx.\n", hr, expected);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
}

#define check_inherited_interface(a, b, c) _check_inherited_interface(__LINE__, a, b, c)
static void _check_inherited_interface(unsigned int line, void *iface, REFIID inherited_iid, REFIID iid)
{
    IUnknown *unknown, *unknown2;
    HRESULT hr, hr2;

    hr = IUnknown_QueryInterface((IUnknown *)iface, inherited_iid, (void **)&unknown);
    ok_(__FILE__, line)(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(unknown);
    hr2 = IUnknown_QueryInterface((IUnknown *)iface, iid, (void **)&unknown2);
    ok_(__FILE__, line)(hr2 == S_OK, "Got unexpected hr %#lx.\n", hr2);
    if (SUCCEEDED(hr2))
        IUnknown_Release(unknown2);
    ok_(__FILE__, line)(unknown2 == unknown, "Interface is not inherited.\n");
}

static HWND create_window(void)
{
    RECT r = {0, 0, 640, 480};

    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW | WS_VISIBLE, FALSE);
    return CreateWindowW(L"static", L"dcomp_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
            r.right - r.left, r.bottom - r.top, NULL, NULL, NULL, NULL);
}

static IDXGIDevice *create_device(unsigned int flags)
{
    IDXGIDevice *dxgi_device;
    ID3D10Device1 *device;
    HRESULT hr;

    hr = D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, flags, D3D10_FEATURE_LEVEL_10_0,
            D3D10_1_SDK_VERSION, &device);
    if (SUCCEEDED(hr))
        goto success;
    if (SUCCEEDED(D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_WARP, NULL, flags,
            D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &device)))
        goto success;
    if (SUCCEEDED(D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_REFERENCE, NULL, flags,
            D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &device)))
        goto success;

    return NULL;

success:
    hr = ID3D10Device1_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(SUCCEEDED(hr), "Created device does not implement IDXGIDevice.\n");
    ID3D10Device1_Release(device);
    return dxgi_device;
}

#define get_factory(a, b) get_factory_(__LINE__, a, b)
static void get_factory_(unsigned int line, IDXGIDevice *device, IDXGIFactory **factory)
{
    IDXGIAdapter *adapter;
    HRESULT hr;

    hr = IDXGIDevice_GetAdapter(device, &adapter);
    ok_(__FILE__, line)(hr == S_OK, "Failed to get adapter, hr %#lx.\n", hr);
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)factory);
    ok_(__FILE__, line)(hr == S_OK, "Failed to get parent, hr %#lx.\n", hr);
    IDXGIAdapter_Release(adapter);
}

#define create_swapchain(a, b) create_swapchain_(__LINE__, a, b)
static IDXGISwapChain *create_swapchain_(unsigned int line, IDXGIDevice *device, HWND window)
{
    DXGI_SWAP_CHAIN_DESC desc;
    IDXGISwapChain *swapchain;
    IDXGIFactory *factory;
    HRESULT hr;

    desc.BufferDesc.Width = 640;
    desc.BufferDesc.Height = 480;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    desc.Flags = 0;

    get_factory(device, &factory);
    hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc, &swapchain);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create swapchain, hr %#lx.\n", hr);
    IDXGIFactory_Release(factory);

    return swapchain;
}

/* Based on Wine-Staging patch 0002 by Zhiyi Zhang */
static void test_DCompositionCreateDevice(void)
{
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    /* D3D device created without BGRA support */
    if (!(dxgi_device = create_device(0)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* D3D device created with BGRA support */
    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        check_interface(dcomp_device, &IID_IDCompositionDevice, TRUE);
        /* Device created from DCompositionCreateDevice() doesn't support IDCompositionDevice2 */
        check_interface(dcomp_device, &IID_IDCompositionDevice2, FALSE);

        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    /* Parameter checks */
    hr = pDCompositionCreateDevice(NULL, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice2, (void **)&dcomp_device);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0004 by Zhiyi Zhang */
static void test_DCompositionCreateDevice2(void)
{
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice2 *dcomp_device2;
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    /* D3D device created without BGRA support */
    if (!(dxgi_device = create_device(0)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* D3D device created with BGRA support */
    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        check_interface(dcomp_device, &IID_IUnknown, TRUE);
        check_interface(dcomp_device, &IID_IDCompositionDevice, TRUE);
        check_interface(dcomp_device, &IID_IDCompositionDevice2, TRUE);
        check_interface(dcomp_device, &IID_IDCompositionDesktopDevice, TRUE);

        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    /* Parameter checks */
    hr = pDCompositionCreateDevice2(NULL, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    /* IDCompositionDevice2 needs to be queried from the device instance */
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice2,
            (void **)&dcomp_device2);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        refcount = IDCompositionDesktopDevice_Release(desktop_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_DCompositionCreateDevice3(void)
{
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    /* D3D device created without BGRA support */
    if (!(dxgi_device = create_device(0)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice3((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* D3D device created with BGRA support */
    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice3((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        /* Interface checks */
        check_interface(dcomp_device, &IID_IUnknown, TRUE);
        check_interface(dcomp_device, &IID_IDCompositionDevice, TRUE);
        check_interface(dcomp_device, &IID_IDCompositionDevice2, TRUE);
        check_interface(dcomp_device, &IID_IDCompositionDesktopDevice, TRUE);

        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    /* Parameter checks */
    hr = pDCompositionCreateDevice3(NULL, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        refcount = IDCompositionDevice_Release(dcomp_device);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* The shared visuals of the Windows App Runtime: DCompositionCreateSharedVisualHandle, the private interfaces of the device and its
 * method that opens a handle as a visual or a target (slot 29 of the vtable of IDCompositionDesktopDevice, after the public methods).
 * Observed on Windows 11. */
static void test_shared_visual(void)
{
    static const GUID IID_alias_a = {0xd14b6158, 0xc3fa, 0x4bce, {0x9c, 0x1f, 0xb6, 0x1d, 0x86, 0x65, 0xea, 0xb0}};
    static const GUID IID_alias_b = {0x28d6ad3d, 0xee2a, 0x4bcd, {0x94, 0x19, 0x7d, 0x54, 0x80, 0x04, 0x35, 0xb1}};
    static const GUID IID_visual_private = {0x2c4eef28, 0x1bc0, 0x4736, {0xb7, 0xdd, 0xb6, 0x26, 0x92, 0xf9, 0xbd, 0x67}};
    typedef HRESULT (WINAPI *create_handle_t)(HANDLE *);
    typedef HRESULT (STDMETHODCALLTYPE *open_t)(void *iface, HANDLE handle, REFIID iid, void **out);
    create_handle_t create_handle;
    IDCompositionDesktopDevice *desktop;
    IDCompositionDevice *device;
    IDCompositionVisual *visual;
    IDCompositionTarget *target;
    IUnknown *alias_a, *alias_b;
    HANDLE handle, handle2;
    open_t open;
    void *out;
    HRESULT hr;

    create_handle = (void *)GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionCreateSharedVisualHandle");
    if (!create_handle || !pDCompositionCreateDevice3)
    {
        win_skip("DCompositionCreateSharedVisualHandle() or DCompositionCreateDevice3() is unavailable.\n");
        return;
    }

    hr = create_handle(NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    handle = NULL;
    hr = create_handle(&handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(handle != NULL && handle != INVALID_HANDLE_VALUE, "Got handle %p.\n", handle);
    hr = create_handle(&handle2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = pDCompositionCreateDevice3(NULL, &IID_IDCompositionDesktopDevice, (void **)&desktop);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    /* two other IIDs answer with the very interface of IDCompositionDesktopDevice */
    hr = IDCompositionDesktopDevice_QueryInterface(desktop, &IID_alias_a, (void **)&alias_a);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(alias_a == (IUnknown *)desktop, "Got %p, expected %p.\n", alias_a, desktop);
    if (SUCCEEDED(hr)) IUnknown_Release(alias_a);
    hr = IDCompositionDesktopDevice_QueryInterface(desktop, &IID_alias_b, (void **)&alias_b);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(alias_b == (IUnknown *)desktop, "Got %p, expected %p.\n", alias_b, desktop);
    if (SUCCEEDED(hr)) IUnknown_Release(alias_b);

    /* Private method 46 (one pointer argument: 8 bytes removed on 32-bit Windows 11): stores the id of the newest completed batch of
     * changes in a dword, 0 when nothing was submitted; the second dword of an eight-byte block is not touched. Observed on Windows
     * 11 only: older Windows may not have the method. The NULL pointer is checked on Wine, where it was not measured. */
    {
        typedef HRESULT (STDMETHODCALLTYPE *method46_t)(void *iface, void *completed);
        typedef LONG (WINAPI *rtl_get_version_t)(RTL_OSVERSIONINFOW *);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        BOOL is_wine = !!GetProcAddress(ntdll, "wine_get_version");
        rtl_get_version_t get_version = (void *)GetProcAddress(ntdll, "RtlGetVersion");
        RTL_OSVERSIONINFOW version = {sizeof(version)};
        method46_t method46 = (*(method46_t **)desktop)[46];
        UINT64 block;

        if (get_version) get_version(&version);
        if (!is_wine && version.dwBuildNumber < 22000)
        {
            win_skip("Private method 46 is only known on Windows 11.\n");
        }
        else
        {
            block = 0xdeadbeef00000000ull;
            hr = method46(desktop, &block);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            /* Windows 11 may read 1 already (Notepad's trace shows 1 before its first Commit; the system compositor's own
             * count, not this device's) */
            ok(block == 0xdeadbeef00000000ull || broken(block == 0xdeadbeef00000001ull), "Got block %#I64x.\n", block);

#ifdef __x86_64__
            /* method 45 (the newest submitted batch; its stack size on i386 is not measured): 0 on a fresh device too; after a
             * Commit that completed, the completed batch is at least 1 and the submitted one is not behind it */
            {
                method46_t method45 = (*(method46_t **)desktop)[45];
                DWORD submitted = 0xdeadbeef, completed = 0xdeadbeef;

                hr = method45(desktop, &submitted);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                ok(!submitted || broken(submitted == 1), "Got submitted %lu.\n", submitted);

                hr = IDCompositionDesktopDevice_Commit(desktop);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                hr = IDCompositionDesktopDevice_WaitForCommitCompletion(desktop);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                hr = method46(desktop, &completed);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                hr = method45(desktop, &submitted);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                /* Windows 11 sometimes still reads 0 (two runs out of three) */
                ok(completed >= 1 || broken(!completed), "Got completed %lu.\n", completed);
                ok(submitted >= completed, "Got submitted %lu, completed %lu.\n", submitted, completed);
            }
#endif

            if (is_wine)
            {
                hr = method46(desktop, NULL);
                ok(hr == E_POINTER, "Got unexpected hr %#lx.\n", hr);
            }
        }
    }

    /* The interface {6b556968-...} of the device. Windows 11 answers the query; its slot 8 is not the one the runtime's helper
     * calls there (it removes 12 bytes and returns E_INVALIDARG for an event handle), so the slot is checked on Wine only: it takes
     * the commit completion event, which Commit signals, also when it fails. */
    {
        static const GUID IID_commit = {0x6b556968, 0x2d25, 0x11e2, {0x99, 0xf6, 0x8b, 0xc3, 0x60, 0x88, 0x70, 0x9b}};
        typedef HRESULT (STDMETHODCALLTYPE *set_event_t)(void *iface, HANDLE event, void *arg);
        BOOL is_wine = !!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
        HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
        set_event_t set_event;
        void *commit = NULL;

        hr = IDCompositionDesktopDevice_QueryInterface(desktop, &IID_commit, &commit);
        ok(hr == S_OK || broken(hr == E_NOINTERFACE), "Got unexpected hr %#lx.\n", hr);
        if (hr == S_OK && is_wine)
        {
            IDCompositionTarget *failing_target;
            IDCompositionVisual2 *root;
            HWND window;

            set_event = (*(set_event_t **)commit)[8];
            hr = set_event(commit, event, NULL);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "The event is signaled before a commit.\n");
            hr = IDCompositionDesktopDevice_Commit(desktop);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "The event is not signaled after a commit.\n");
            ResetEvent(event);

            /* a commit that fails (the window of a target is gone) signals it as well */
            window = CreateWindowW(L"static", L"test", WS_POPUP, 0, 0, 1, 1, 0, 0, 0, 0);
            hr = IDCompositionDesktopDevice_CreateVisual(desktop, &root);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop, window, TRUE, &failing_target);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            hr = IDCompositionTarget_SetRoot(failing_target, (IDCompositionVisual *)root);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            DestroyWindow(window);
            hr = IDCompositionDesktopDevice_Commit(desktop);
            ok(FAILED(hr), "Got unexpected hr %#lx.\n", hr);
            ok(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "The event is not signaled after a failed commit.\n");
            ResetEvent(event);
            IDCompositionTarget_SetRoot(failing_target, NULL);
            IDCompositionTarget_Release(failing_target);
            IDCompositionVisual2_Release(root);

            hr = set_event(commit, NULL, NULL);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            IDCompositionDesktopDevice_Commit(desktop);
            ok(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "The cleared event is signaled.\n");
        }
        if (commit) IUnknown_Release((IUnknown *)commit);
        CloseHandle(event);
    }

    open = (*(open_t **)desktop)[29];

    hr = open(desktop, handle, &IID_IDCompositionVisual, (void **)&visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        /* the visuals of the system dcomp answer a private interface: the visual itself, the same pointer as for IUnknown, with its
         * reference count (observed on Windows 11) */
        IUnknown *unknown = NULL;

        out = NULL;
        hr = IDCompositionVisual_QueryInterface(visual, &IID_visual_private, &out);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ok(out == (void *)visual, "Got %p, expected %p.\n", out, visual);
        if (SUCCEEDED(hr))
        {
            hr = IUnknown_QueryInterface((IUnknown *)out, &IID_IUnknown, (void **)&unknown);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(unknown == (IUnknown *)visual, "Got %p, expected %p.\n", unknown, visual);
            if (SUCCEEDED(hr)) IUnknown_Release(unknown);
            IUnknown_Release((IUnknown *)out);
        }
        IDCompositionVisual_Release(visual);
    }

    /* An object has one visual at a time, on the device that opened it first: another open is E_ACCESSDENIED while it lives, another
     * device stays refused after it is released, and the first device opens it again (Windows 11). The visual is not kept alive,
     * and does not keep the device alive, when nobody holds it (the reference counts are checked on Wine). */
    {
        BOOL is_wine = !!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
        IDCompositionDesktopDevice *desktop2;
        IDCompositionVisual *visual1, *visual2;
        ULONG refcount;

        if (is_wine)
        {
            refcount = IDCompositionDesktopDevice_AddRef(desktop);
            ok(refcount == 2, "Got refcount %lu.\n", refcount);
            IDCompositionDesktopDevice_Release(desktop);
        }

        hr = pDCompositionCreateDevice3(NULL, &IID_IDCompositionDesktopDevice, (void **)&desktop2);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = open(desktop, handle, &IID_IDCompositionVisual, (void **)&visual1);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        visual2 = (void *)0xdeadbeef;
        hr = ((open_t)(*(open_t **)desktop2)[29])(desktop2, handle, &IID_IDCompositionVisual, (void **)&visual2);
        ok(hr == E_ACCESSDENIED, "Got unexpected hr %#lx.\n", hr);
        ok(!visual2, "Got %p.\n", visual2);
        if (SUCCEEDED(hr)) IDCompositionVisual_Release(visual2);
        if (visual1) IDCompositionVisual_Release(visual1);
        visual2 = (void *)0xdeadbeef;
        hr = ((open_t)(*(open_t **)desktop2)[29])(desktop2, handle, &IID_IDCompositionVisual, (void **)&visual2);
        ok(hr == E_ACCESSDENIED, "Got unexpected hr %#lx.\n", hr);
        if (SUCCEEDED(hr)) IDCompositionVisual_Release(visual2);
        hr = open(desktop, handle, &IID_IDCompositionVisual, (void **)&visual1);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        if (SUCCEEDED(hr)) IDCompositionVisual_Release(visual1);
        /* another device opens another object (an object opens as a target once: handle stays for the target opened below) */
        hr = ((open_t)(*(open_t **)desktop2)[29])(desktop2, handle2, &IID_IDCompositionVisual, (void **)&visual2);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = ((open_t)(*(open_t **)desktop2)[29])(desktop2, handle2, &IID_IDCompositionTarget, (void **)&target);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDCompositionTarget_SetRoot(target, visual2);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            IDCompositionTarget_SetRoot(target, NULL);
            IDCompositionTarget_Release(target);
        }
        if (visual2 && visual2 != (void *)0xdeadbeef) IDCompositionVisual_Release(visual2);
        refcount = IDCompositionDesktopDevice_Release(desktop2);
        ok(!refcount, "Device 2 has %lu references left.\n", refcount);
        if (is_wine)
        {
            refcount = IDCompositionDesktopDevice_AddRef(desktop);
            ok(refcount == 2, "Got refcount %lu.\n", refcount);
            IDCompositionDesktopDevice_Release(desktop);
        }
    }

    hr = open(desktop, handle, &IID_IDCompositionTarget, (void **)&target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionTarget_Release(target);

    out = (void *)0xdeadbeef;
    hr = open(desktop, (HANDLE)0x12345678, &IID_IDCompositionVisual, &out);
    ok(FAILED(hr), "Got unexpected hr %#lx.\n", hr);
    ok(!out, "Got %p.\n", out);

    out = (void *)0xdeadbeef;
    hr = open(desktop, handle2, &IID_IDispatch, &out);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    ok(!out, "Got %p.\n", out);

    hr = IDCompositionDesktopDevice_QueryInterface(desktop, &IID_IDCompositionDevice, (void **)&device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionDevice_Release(device);
    IDCompositionDesktopDevice_Release(desktop);

done:
    CloseHandle(handle);
    CloseHandle(handle2);
}

/* Based on Wine-Staging patch 0007 by Zhiyi Zhang */
static void test_device_CreateTargetForHwnd(void)
{
    IDCompositionTarget *target, *target2, *target3;
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    HRESULT hr, hr2, hr3;
    ULONG refcount;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = CreateWindowW(L"static", L"test", WS_POPUP, 0, 0, 1, 1, 0, 0, 0, 0);
    ok(!!hwnd, "Failed to create a test window.\n");

    /* Test CreateTargetForHwnd() with IDCompositionDevice */
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, NULL, FALSE, &target);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, GetDesktopWindow(), FALSE, &target);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, FALSE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr2 = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, TRUE, &target2);
    ok(hr2 == S_OK, "Got unexpected hr %#lx.\n", hr2);

    hr3 = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, FALSE, &target3);
    ok(hr3 == DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED, "Got unexpected hr %#lx.\n", hr3);

    hr3 = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, TRUE, &target3);
    ok(hr3 == DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED, "Got unexpected hr %#lx.\n", hr3);

    if (SUCCEEDED(hr))
        IDCompositionTarget_Release(target);
    if (SUCCEEDED(hr2))
        IDCompositionTarget_Release(target2);

    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Test CreateTargetForHwnd() with IDCompositionDesktopDevice */
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, NULL, FALSE, &target);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, hwnd, FALSE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        IDCompositionTarget_Release(target);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, hwnd, TRUE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        IDCompositionTarget_Release(target);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, hwnd, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    refcount = IDCompositionDesktopDevice_Release(desktop_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0009 by Zhiyi Zhang */
static void test_device_CreateVisual(void)
{
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual2 *visual2;
    IDCompositionVisual *visual;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    /* Test CreateVisual() with IDCompositionDevice */
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDevice_CreateVisual(dcomp_device, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        /* IDCompositionVisual objects created from a device from DCompositionCreateDevice() doesn't
         * support IDCompositionVisual2 */
        hr = IDCompositionVisual_QueryInterface(visual, &IID_IDCompositionVisual2, (void **)&visual2);
        ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);
        IDCompositionVisual_Release(visual);
    }

    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Test CreateVisual() with IDCompositionDesktopDevice */
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Parameter checks */
    hr = IDCompositionDesktopDevice_CreateVisual(desktop_device, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateVisual(desktop_device, &visual2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDCompositionVisual2_QueryInterface(visual2, &IID_IDCompositionVisual, (void **)&visual);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
            IDCompositionVisual_Release(visual);
        IDCompositionVisual2_Release(visual2);
    }

    refcount = IDCompositionDesktopDevice_Release(desktop_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_device_CreateSurfaceFactory(void)
{
    IDCompositionSurfaceFactory *surface_factory;
    IDCompositionDevice2 *dcomp_device2;
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    ID2D1Device *d2d_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_QueryInterface(dcomp_device, &IID_IDCompositionDevice2, (void *)&dcomp_device2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* NULL rendering device pointer */
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, NULL, &surface_factory);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Rendering device pointer is not an IDXGIDevice, ID3D11Device or ID2D1Device */
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)dcomp_device, &surface_factory);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);

    /* Rendering device pointer is an IDXGIDevice */
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)dxgi_device, &surface_factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (hr == S_OK)
        IDCompositionSurfaceFactory_Release(surface_factory);

    /* Rendering device pointer is an ID2D1Device */
    hr = D2D1CreateDevice(dxgi_device, NULL, &d2d_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)d2d_device, &surface_factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (hr == S_OK)
        IDCompositionSurfaceFactory_Release(surface_factory);
    ID2D1Device_Release(d2d_device);

    IDCompositionDevice2_Release(dcomp_device2);
    IDCompositionDevice_Release(dcomp_device);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_surface_factory_CreateSurface(void)
{
    IDCompositionSurfaceFactory *surface_factories[2];
    IDCompositionDevice2 *dcomp_device2;
    IDCompositionDevice *dcomp_device;
    IDCompositionSurface *surface;
    IDXGIDevice *dxgi_device;
    ID2D1Device *d2d_device;
    ULONG refcount;
    unsigned int i, j;
    HRESULT hr;

    static const struct
    {
        UINT width;
        UINT height;
        DXGI_FORMAT pixel_format;
        DXGI_ALPHA_MODE alpha_mode;
        HRESULT hr;
    }
    tests[] =
    {
        /* Invalid width */
        {0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, E_INVALIDARG},
        /* Invalid height */
        {1, 0, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, E_INVALIDARG},
        /* Invalid pixel format */
        {1, 1, DXGI_FORMAT_UNKNOWN, DXGI_ALPHA_MODE_IGNORE, E_INVALIDARG},
        /* Invalid alpha mode */
        {1, 1, DXGI_FORMAT_UNKNOWN, DXGI_ALPHA_MODE_STRAIGHT, E_INVALIDARG},
        /* Valid pixel formats */
        {1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, S_OK},
        {1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, S_OK},
        {1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_ALPHA_MODE_IGNORE, S_OK},
        /* Valid alpha modes */
        {1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_UNSPECIFIED, S_OK},
        {1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, S_OK},
        {1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, S_OK},
    };

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_QueryInterface(dcomp_device, &IID_IDCompositionDevice2, (void *)&dcomp_device2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Rendering device pointer is an IDXGIDevice */
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)dxgi_device, &surface_factories[0]);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Rendering device pointer is an ID2D1Device */
    hr = D2D1CreateDevice(dxgi_device, NULL, &d2d_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)d2d_device, &surface_factories[1]);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ID2D1Device_Release(d2d_device);

    for (i = 0; i < ARRAY_SIZE(surface_factories); i++)
    {
        winetest_push_context("%d", i);

        for (j = 0; j < ARRAY_SIZE(tests); j++)
        {
            winetest_push_context("%d", j);

            hr = IDCompositionSurfaceFactory_CreateSurface(surface_factories[i], tests[j].width,
                    tests[j].height, tests[j].pixel_format, tests[j].alpha_mode, &surface);
            ok(hr == tests[j].hr, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
                IDCompositionSurface_Release(surface);
            winetest_pop_context();
        }

        IDCompositionSurfaceFactory_Release(surface_factories[i]);
        winetest_pop_context();
    }

    IDCompositionDevice2_Release(dcomp_device2);
    IDCompositionDevice_Release(dcomp_device);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0064 by Zhiyi Zhang */
static void test_surface_interface(void)
{
    static const unsigned int width = 640, height = 480;
    IDCompositionSurfaceFactory *surface_factory;
    IDCompositionSurfaceUnknown *surface_unknown;
    IDCompositionDevice2 *dcomp_device2;
    IDCompositionDevice *dcomp_device;
    IDCompositionSurface *surface;
    IDXGISurface *dxgi_surface;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    POINT offset;
    HRESULT hr;
    RECT rect;

    /* IDCompositionSurfaceUnknown is undocumented and its layout changed on current Windows builds (the call below
     * crashes on Windows 11 build 26300): only Wine implements the layout from Wine-Staging patch 0063. */
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))
    {
        skip("IDCompositionSurfaceUnknown layout is not stable across Windows builds.\n");
        return;
    }

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_QueryInterface(dcomp_device, &IID_IDCompositionDevice2, (void *)&dcomp_device2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)dxgi_device, &surface_factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionSurfaceFactory_CreateSurface(surface_factory, width, height,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, &surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    check_interface(surface, &IID_IDCompositionSurface, TRUE);
    check_interface(surface, &IID_IDCompositionSurfaceUnknown, TRUE);
    check_inherited_interface(surface, &IID_IDCompositionSurfaceUnknown, &IID_IDCompositionSurface);

    /* Test IDCompositionSurfaceUnknown. */
    hr = IDCompositionSurface_QueryInterface(surface, &IID_IDCompositionSurfaceUnknown,
            (void **)&surface_unknown);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Test IDCompositionSurfaceUnknown::Unknown3. */
    hr = IDCompositionSurfaceUnknown_Unknown3(surface_unknown, 0, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionSurfaceUnknown_Unknown3(surface_unknown, 0, 1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionSurfaceUnknown_Unknown3(surface_unknown, 1, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionSurfaceUnknown_Unknown3(surface_unknown, 1, 1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Test resize while a draw is active. */
    SetRect(&rect, 0, 0, 1, 1);
    hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface, (void **)&dxgi_surface,
            &offset);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionSurfaceUnknown_Unknown3(surface_unknown, 2, 2);
    ok(hr == DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED, "Got unexpected hr %#lx.\n", hr);
    IDXGISurface_Release(dxgi_surface);
    hr = IDCompositionSurface_EndDraw(surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Unknown3 resizes the physical surface and invalidates the old bounds. */
    hr = IDCompositionSurfaceUnknown_Unknown3(surface_unknown, 100, 100);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    SetRect(&rect, 0, 0, 100, 101);
    hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface, (void **)&dxgi_surface,
            &offset);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    IUnknown_Release((IUnknown *)surface_unknown);
    IDCompositionSurface_Release(surface);
    IDCompositionSurfaceFactory_Release(surface_factory);
    IDCompositionDevice2_Release(dcomp_device2);
    IDCompositionDevice_Release(dcomp_device);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_surface_begin_end_Draw(void)
{
    static const unsigned int width = 640, height = 480;
    IDCompositionSurfaceFactory *surface_factories[2];
    IDXGISurface *dxgi_surface, *dxgi_surface2;
    IDCompositionSurface *surface = NULL, *surface2 = NULL;
    ID2D1DeviceContext *d2d_device_context;
    IDCompositionDevice2 *dcomp_device2;
    IDCompositionDevice *dcomp_device;
    ID3D10Texture2D *d3d10_texture;
    ID3D11Texture2D *d3d11_texture;
    IDXGIDevice *dxgi_device;
    ID2D1Device *d2d_device;
    DXGI_SURFACE_DESC dxgi_surface_desc;
    D3D11_TEXTURE2D_DESC d3d11_texture_desc;
    D3D10_TEXTURE2D_DESC d3d10_texture_desc;
    unsigned int i, j;
    ULONG refcount;
    POINT offset;
    HRESULT hr;
    RECT rect;

    static const struct
    {
        DXGI_FORMAT pixel_format;
        DXGI_ALPHA_MODE alpha_mode;
    }
    formats[] =
    {
        {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE},
        {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED},
        {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ALPHA_MODE_IGNORE},
        {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_ALPHA_MODE_IGNORE},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_ALPHA_MODE_PREMULTIPLIED},
    };

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_QueryInterface(dcomp_device, &IID_IDCompositionDevice2, (void *)&dcomp_device2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Rendering device pointer is an IDXGIDevice */
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)dxgi_device, &surface_factories[0]);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Rendering device pointer is an ID2D1Device */
    hr = D2D1CreateDevice(dxgi_device, NULL, &d2d_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice2_CreateSurfaceFactory(dcomp_device2, (IUnknown *)d2d_device, &surface_factories[1]);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ID2D1Device_Release(d2d_device);

    for (i = 0; i < ARRAY_SIZE(surface_factories); i++)
    {
        winetest_push_context("%d", i);

        for (j = 0; j < ARRAY_SIZE(formats); j++)
        {
            winetest_push_context("%d", j);

            hr = IDCompositionSurfaceFactory_CreateSurface(surface_factories[i], width, height,
                    formats[j].pixel_format, formats[j].alpha_mode, &surface);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr != S_OK)
            {
                winetest_pop_context();
                continue;
            }

            hr = IDCompositionSurfaceFactory_CreateSurface(surface_factories[i], width, height,
                    formats[j].pixel_format, formats[j].alpha_mode, &surface2);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

            /* Width out of range */
            SetRect(&rect, 0, 0, width + 1, height);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface, &offset);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* Height out of range */
            SetRect(&rect, 0, 0, width, height + 1);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface, &offset);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* Not full width for the first BeginDraw() */
            SetRect(&rect, 0, 0, width - 1, height);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface, &offset);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* Not full height for the first BeginDraw() */
            SetRect(&rect, 0, 0, width, height - 1);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface, &offset);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* NULL object pointer */
            SetRect(&rect, 0, 0, width, height);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface, NULL, &offset);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* NULL offset pointer */
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface, NULL);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* Normal call with NULL rect pointer */
            dxgi_surface = NULL;
            hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface,
                    (void **)&dxgi_surface, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
            {
                hr = IDXGISurface_GetDesc(dxgi_surface, &dxgi_surface_desc);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                ok(dxgi_surface_desc.Width >= width, "Got width %d < %d.\n",
                        dxgi_surface_desc.Width, width);
                ok(dxgi_surface_desc.Height >= height, "Got height %d < %d.\n",
                        dxgi_surface_desc.Height, height);
                ok(dxgi_surface_desc.Format == formats[j].pixel_format, "Expected format %d, got %d.\n",
                        formats[j].pixel_format, dxgi_surface_desc.Format);
                ok(dxgi_surface_desc.SampleDesc.Count == 1, "Got unexpected sample count %d.\n",
                        dxgi_surface_desc.SampleDesc.Count);
                ok(dxgi_surface_desc.SampleDesc.Quality == 0, "Got unexpected sample quality %d.\n",
                        dxgi_surface_desc.SampleDesc.Quality);
            }

            /* Second BeginDraw() for the same surface without EndDraw(). This is supposed to fail
             * according to MSDN */
            hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface,
                    (void **)&dxgi_surface2, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
            {
                ok(dxgi_surface2 == dxgi_surface, "Got unexpected surface.\n");
                IDXGISurface_Release(dxgi_surface2);
            }

            /* Second BeginDraw() for the same surface without EndDraw() with a different rectangle.
             * This is supposed to fail according to MSDN */
            SetRect(&rect, 10, 10, width, height);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface2, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
            {
                ok(dxgi_surface2 == dxgi_surface, "Got unexpected surface.\n");
                IDXGISurface_Release(dxgi_surface2);
            }

            if (dxgi_surface)
                IDXGISurface_Release(dxgi_surface);

            /* Second BeginDraw() for the another surface without EndDraw() */
            hr = IDCompositionSurface_BeginDraw(surface2, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface2, &offset);
            ok(hr == DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
                IDXGISurface_Release(dxgi_surface2);

            hr = IDCompositionSurface_EndDraw(surface);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

            /* Second BeginDraw() for the same surface with overlapping rectangles */
            dxgi_surface = NULL;
            SetRect(&rect, 0, 0, 100, 100);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

            SetRect(&rect, 50, 50, 150, 150);
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface,
                    (void **)&dxgi_surface2, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
            {
                ok(dxgi_surface2 == dxgi_surface, "Got unexpected surface.\n");
                IDXGISurface_Release(dxgi_surface2);
            }

            if (dxgi_surface)
                IDXGISurface_Release(dxgi_surface);

            hr = IDCompositionSurface_EndDraw(surface);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

            /* Normal call with IID_ID3D10Texture2D */
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_ID3D10Texture2D,
                    (void **)&d3d10_texture, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
            {
                ID3D10Texture2D_GetDesc(d3d10_texture, &d3d10_texture_desc);

                ok(d3d10_texture_desc.Width >= width, "Got width %d < %d.\n",
                        d3d10_texture_desc.Width, width);
                ok(d3d10_texture_desc.Height >= height, "Got height %d < %d.\n",
                        d3d10_texture_desc.Height, height);
                ok(d3d10_texture_desc.MipLevels == 1, "Got mipmap levels %d.\n",
                        d3d10_texture_desc.MipLevels);
                ok(d3d10_texture_desc.ArraySize == 1, "Got array size %d.\n",
                        d3d10_texture_desc.ArraySize);
                ok(d3d10_texture_desc.Format == formats[j].pixel_format,
                        "Expected format %d, got %d.\n", formats[j].pixel_format,
                        d3d10_texture_desc.Format);
                ok(d3d10_texture_desc.SampleDesc.Count == 1, "Got sample count %d.\n",
                        d3d10_texture_desc.SampleDesc.Count);
                ok(d3d10_texture_desc.SampleDesc.Quality == 0, "Got sample quality %d.\n",
                        d3d10_texture_desc.SampleDesc.Quality);
                ok(d3d10_texture_desc.Usage == D3D10_USAGE_DEFAULT, "Got usage %d.\n",
                        d3d10_texture_desc.Usage);
                ok(d3d10_texture_desc.BindFlags == D3D10_BIND_RENDER_TARGET,
                        "Got bind flags %#x.\n", d3d10_texture_desc.BindFlags);
                ok(d3d10_texture_desc.CPUAccessFlags == 0, "Got CPU access flags %#x.\n",
                        d3d10_texture_desc.CPUAccessFlags);
                if (formats[j].pixel_format == DXGI_FORMAT_B8G8R8A8_UNORM)
                    ok(d3d10_texture_desc.MiscFlags == D3D10_RESOURCE_MISC_GDI_COMPATIBLE,
                            "Got misc flags %#x.\n", d3d10_texture_desc.MiscFlags);
                else
                    ok(d3d10_texture_desc.MiscFlags == 0, "Got misc flags %#x.\n",
                            d3d10_texture_desc.MiscFlags);

                ID3D10Texture2D_Release(d3d10_texture);
                hr = IDCompositionSurface_EndDraw(surface);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            }

            /* Normal call with IID_ID3D11Texture2D */
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_ID3D11Texture2D,
                    (void **)&d3d11_texture, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
            {
                ID3D11Texture2D_GetDesc(d3d11_texture, &d3d11_texture_desc);

                ok(d3d11_texture_desc.Width >= width, "Got width %d < %d.\n",
                        d3d11_texture_desc.Width, width);
                ok(d3d11_texture_desc.Height >= height, "Got height %d < %d.\n",
                        d3d11_texture_desc.Height, height);
                ok(d3d11_texture_desc.MipLevels == 1, "Got mipmap levels %d.\n",
                        d3d11_texture_desc.MipLevels);
                ok(d3d11_texture_desc.ArraySize == 1, "Got array size %d.\n",
                        d3d11_texture_desc.ArraySize);
                ok(d3d11_texture_desc.Format == formats[j].pixel_format,
                        "Expected format %d, got %d.\n", formats[j].pixel_format,
                        d3d11_texture_desc.Format);
                ok(d3d11_texture_desc.SampleDesc.Count == 1, "Got sample count %d.\n",
                        d3d11_texture_desc.SampleDesc.Count);
                ok(d3d11_texture_desc.SampleDesc.Quality == 0, "Got sample quality %d.\n",
                        d3d11_texture_desc.SampleDesc.Quality);
                ok(d3d11_texture_desc.Usage == D3D11_USAGE_DEFAULT, "Got usage %d.\n",
                        d3d11_texture_desc.Usage);
                ok(d3d11_texture_desc.BindFlags == D3D11_BIND_RENDER_TARGET,
                        "Got bind flags %#x.\n", d3d11_texture_desc.BindFlags);
                ok(d3d11_texture_desc.CPUAccessFlags == 0, "Got CPU access flags %#x.\n",
                        d3d11_texture_desc.CPUAccessFlags);
                if (formats[j].pixel_format == DXGI_FORMAT_B8G8R8A8_UNORM)
                    /* native Windows returns GDI_COMPATIBLE | SHARED_NTHANDLE */
                    todo_wine(d3d11_texture_desc.MiscFlags == (D3D11_RESOURCE_MISC_GDI_COMPATIBLE |
                                    D3D11_RESOURCE_MISC_SHARED_NTHANDLE),
                            "Got misc flags %#x.\n", d3d11_texture_desc.MiscFlags);
                else
                    /* native Windows returns SHARED_NTHANDLE */
                    todo_wine(d3d11_texture_desc.MiscFlags == D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
                            "Got misc flags %#x.\n", d3d11_texture_desc.MiscFlags);

                ID3D11Texture2D_Release(d3d11_texture);
                hr = IDCompositionSurface_EndDraw(surface);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            }

            /* Normal call with ID2D1DeviceContext */
            hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_ID2D1DeviceContext,
                    (void **)&d2d_device_context, &offset);
            if (i == 0)
            {
                ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);
            }
            else
            {
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                if (hr == S_OK)
                {
                    ID2D1DeviceContext_Release(d2d_device_context);
                    hr = IDCompositionSurface_EndDraw(surface);
                    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                }
            }

            /* Call EndDraw() without BeginDraw() */
            hr = IDCompositionSurface_EndDraw(surface);
            ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED, "Got unexpected hr %#lx.\n", hr);

            /* Call EndDraw() twice after BeginDraw() twice for the same surface */
            hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface,
                    (void **)&dxgi_surface, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (hr == S_OK)
            {
                hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface,
                        (void **)&dxgi_surface2, &offset);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                IDXGISurface_Release(dxgi_surface2);
                IDXGISurface_Release(dxgi_surface);
                hr = IDCompositionSurface_EndDraw(surface);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
                hr = IDCompositionSurface_EndDraw(surface);
                ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED, "Got unexpected hr %#lx.\n", hr);
            }

            IDCompositionSurface_Release(surface2);
            IDCompositionSurface_Release(surface);
            winetest_pop_context();
        }

        IDCompositionSurfaceFactory_Release(surface_factories[i]);
        winetest_pop_context();
    }

    IDCompositionDevice2_Release(dcomp_device2);
    IDCompositionDevice_Release(dcomp_device);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

#define expect_rendered_color(a, b, c, d) _expect_rendered_color(__LINE__, a, b, c, d)
static void _expect_rendered_color(int line, HWND hwnd, int x, int y, COLORREF expected_color)
{
    COLORREF color = CLR_INVALID;
    DWORD start = GetTickCount();
    HDC hdc;
    POINT point;

    /* DWM composites the surface to the screen, not to the window DC, so read the screen DC at the
     * window's client position converted to screen coordinates. */
    point.x = x;
    point.y = y;
    ClientToScreen(hwnd, &point);
    hdc = GetDC(NULL);
    while (GetTickCount() - start < 1000)
    {
        color = GetPixel(hdc, point.x, point.y);
        if (color == expected_color)
            break;
        Sleep(20);
    }
    ReleaseDC(NULL, hdc);
    ok_(__FILE__, line)(color == expected_color, "Expected color %#06lx at (%d, %d), got %#06lx.\n",
            expected_color, x, y, color);
}

static void render_surface_color(ID3D11Device *d3d11_device, IDCompositionSurface *surface,
        float r, float g, float b)
{
    ID3D11DeviceContext *context;
    ID3D11RenderTargetView *rtv;
    ID3D11Texture2D *texture;
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
    D3D11_TEXTURE2D_DESC texture_desc;
    float clear_color[4];
    POINT offset;
    HRESULT hr;

    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_ID3D11Texture2D,
            (void **)&texture, &offset);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        return;

    ID3D11Texture2D_GetDesc(texture, &texture_desc);
    rtv_desc.Format = texture_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    hr = ID3D11Device_CreateRenderTargetView(d3d11_device, (ID3D11Resource *)texture,
            &rtv_desc, &rtv);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ID3D11Device_GetImmediateContext(d3d11_device, &context);
        clear_color[0] = r;
        clear_color[1] = g;
        clear_color[2] = b;
        clear_color[3] = 1.0f;
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear_color);
        ID3D11DeviceContext_Release(context);
        ID3D11RenderTargetView_Release(rtv);
    }
    ID3D11Texture2D_Release(texture);
    hr = IDCompositionSurface_EndDraw(surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
}

static void test_surface_suspend_scroll_virtual(void)
{
    IDCompositionVirtualSurface *virtual_surface = NULL;
    IDCompositionDevice2 *device2;
    IDCompositionDevice *device;
    IDCompositionSurface *surface, *surface2;
    IDXGIDevice *dxgi_device;
    ID3D11Texture2D *texture;
    POINT offset;
    RECT rect;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDevice,
            (void **)&device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDXGIDevice_Release(dxgi_device);
        return;
    }
    hr = IDCompositionDevice_QueryInterface(device, &IID_IDCompositionDevice2, (void **)&device2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDCompositionDevice_Release(device);
        IDXGIDevice_Release(dxgi_device);
        return;
    }

    /* the first device interface makes surfaces too; a surface is not a swap chain (a caller asks to tell them apart) */
    hr = IDCompositionDevice_CreateSurface(device, 32, 32, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        IUnknown *unk = (IUnknown *)0xdeadbeef;

        hr = IDCompositionSurface_QueryInterface(surface, &IID_IDXGISwapChain1, (void **)&unk);
        ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);
        ok(!unk, "Got unexpected pointer %p.\n", unk);
        IDCompositionSurface_Release(surface);
    }

    hr = IDCompositionDevice2_CreateSurface(device2, 32, 32, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice2_CreateSurface(device2, 32, 32, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (surface && surface2)
    {
        hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_ID3D11Texture2D,
                (void **)&texture, &offset);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            ID3D11Texture2D_Release(texture);
            hr = IDCompositionSurface_SuspendDraw(surface);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            hr = IDCompositionSurface_BeginDraw(surface2, NULL, &IID_ID3D11Texture2D,
                    (void **)&texture, &offset);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            if (SUCCEEDED(hr))
            {
                ID3D11Texture2D_Release(texture);
                hr = IDCompositionSurface_ResumeDraw(surface);
                ok(hr == DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED, "Got unexpected hr %#lx.\n", hr);
                hr = IDCompositionSurface_EndDraw(surface2);
                ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            }
            hr = IDCompositionSurface_ResumeDraw(surface);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            hr = IDCompositionSurface_EndDraw(surface);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            hr = IDCompositionSurface_ResumeDraw(surface);
            ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED, "Got unexpected hr %#lx.\n", hr);
        }
    }

    hr = IDCompositionDevice2_CreateVirtualSurface(device2, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &virtual_surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        SetRect(&rect, 4, 4, 20, 20);
        hr = IDCompositionVirtualSurface_BeginDraw(virtual_surface, &rect, &IID_ID3D11Texture2D,
                (void **)&texture, &offset);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            ID3D11Texture2D_Release(texture);
            hr = IDCompositionVirtualSurface_EndDraw(virtual_surface);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        }
        hr = IDCompositionVirtualSurface_Scroll(virtual_surface, NULL, NULL, 1, 1);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionVirtualSurface_Trim(virtual_surface, &rect, 1);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionVirtualSurface_Resize(virtual_surface, 48, 48);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionVirtualSurface_Release(virtual_surface);
    }

    if (surface2) IDCompositionSurface_Release(surface2);
    if (surface) IDCompositionSurface_Release(surface);
    IDCompositionDevice2_Release(device2);
    IDCompositionDevice_Release(device);
    IDXGIDevice_Release(dxgi_device);
}

static void test_surface_composition(void)
{
    static const unsigned int window_size = 200, surface_size = 100;
    IDCompositionSurfaceFactory *surface_factory;
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice *dcomp_device;
    IDCompositionSurface *surface;
    ID3D11Device *d3d11_device;
    IDCompositionTarget *target;
    IDCompositionVisual *visual;
    D3D_FEATURE_LEVEL feature_level;
    ULONG refcount;
    HRESULT hr;
    HWND hwnd;

    /* Visible 200x200 topmost window at the top-left of the primary monitor, so the composited surface
     * is not covered by other windows when the screen is read. */
    hwnd = CreateWindowExW(WS_EX_TOPMOST, L"static", L"dcomp_surface_composition",
            WS_OVERLAPPED | WS_VISIBLE, 0, 0, window_size, window_size, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");
    if (!hwnd)
        return;
    MoveWindow(hwnd, 0, 0, window_size, window_size, TRUE);

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    ok(hr == S_OK, "Failed to create D3D11 device, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(desktop_device, hwnd, TRUE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_QueryInterface(desktop_device, &IID_IDCompositionDevice,
            (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateSurfaceFactory(desktop_device, (IUnknown *)d3d11_device,
            &surface_factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionSurfaceFactory_CreateSurface(surface_factory, surface_size, surface_size,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Render the surface green and commit: a surface-only scene is presented
     * through the target's auxiliary producer. */
    render_surface_color(d3d11_device, surface, 0.0f, 1.0f, 0.0f);

    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target, visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_Commit(desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_WaitForCommitCompletion(desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0, 0xff, 0x00));

    /* Render the surface yellow. */
    render_surface_color(d3d11_device, surface, 1.0f, 1.0f, 0.0f);

    /* Remains green before calling Commit(). */
    expect_rendered_color(hwnd, 50, 50, RGB(0, 0xff, 0x00));

    hr = IDCompositionDesktopDevice_Commit(desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_WaitForCommitCompletion(desktop_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0x00));

    IDCompositionVisual_SetContent(visual, NULL);
    IDCompositionSurface_Release(surface);
    IDCompositionSurfaceFactory_Release(surface_factory);
    IDCompositionTarget_Release(target);
    IDCompositionVisual_Release(visual);
    IDCompositionDevice_Release(dcomp_device);
    IDCompositionDesktopDevice_Release(desktop_device);
    refcount = ID3D11Device_Release(d3d11_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    DestroyWindow(hwnd);
}

typedef HRESULT (STDMETHODCALLTYPE *open_shared_t)(void *iface, HANDLE handle, REFIID iid, void **out);

static HRESULT open_shared(IDCompositionDesktopDevice *device, HANDLE handle, REFIID iid, void **out)
{
    /* slot 29 of the vtable of IDCompositionDesktopDevice, see test_shared_visual() */
    return (*(open_shared_t **)device)[29](device, handle, iid, out);
}

/* A target opened from a shared visual handle is a portal: what another device commits as its root shows inside the visual of the
 * same handle (the compositor of the Windows App SDK shows its scene in the window of the UI thread this way). */
static void test_shared_visual_portal(void)
{
    static const unsigned int window_size = 200, surface_size = 100;
    typedef HRESULT (WINAPI *create_handle_t)(HANDLE *);
    typedef HRESULT (WINAPI *create_surface_handle_t)(DWORD, SECURITY_ATTRIBUTES *, HANDLE *);
    IDCompositionDesktopDevice *viewer, *portal_device;
    create_surface_handle_t create_surface_handle;
    IDCompositionVisual2 *shared, *portal_root;
    IDCompositionSurfaceFactory *factory;
    IDCompositionTarget *window_target, *portal;
    HANDLE handle, duplicate, other;
    create_handle_t create_handle;
    D3D_FEATURE_LEVEL feature_level;
    IDCompositionSurface *surface;
    ID3D11Device *d3d11_device;
    unsigned int order, i;
    ULONG refcount;
    void *out;
    HRESULT hr;
    HWND hwnd;

    create_handle = (void *)GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionCreateSharedVisualHandle");
    create_surface_handle = (void *)GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionCreateSurfaceHandle");
    if (!create_handle || !pDCompositionCreateDevice3)
    {
        win_skip("DCompositionCreateSharedVisualHandle() or DCompositionCreateDevice3() is unavailable.\n");
        return;
    }

    hr = pDCompositionCreateDevice3(NULL, &IID_IDCompositionDesktopDevice, (void **)&viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) return;

    /* only a handle of DCompositionCreateSharedVisualHandle opens: another object, or a closed handle, is E_HANDLE (Windows 11) */
    hr = create_handle(&handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    for (i = 0; i < 4; ++i)
    {
        winetest_push_context("handle %u", i);
        other = NULL;
        if (i == 0) other = CreateEventW(NULL, TRUE, FALSE, NULL);
        else if (i == 1) other = GetCurrentProcess();
        else if (i == 2)
        {
            hr = create_handle(&other);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            CloseHandle(other);
        }
        /* a handle of DCompositionCreateSurfaceHandle (a stub in Wine) */
        else if (!create_surface_handle || strcmp(winetest_platform, "windows")
                || FAILED(create_surface_handle(COMPOSITIONOBJECT_ALL_ACCESS, NULL, &other)))
        {
            skip("No composition surface handle.\n");
            winetest_pop_context();
            continue;
        }
        out = (void *)0xdeadbeef;
        hr = open_shared(viewer, other, &IID_IDCompositionVisual, &out);
        ok(hr == E_HANDLE, "Got unexpected hr %#lx.\n", hr);
        ok(!out, "Got %p.\n", out);
        out = (void *)0xdeadbeef;
        hr = open_shared(viewer, other, &IID_IDCompositionTarget, &out);
        ok(hr == E_HANDLE, "Got unexpected hr %#lx.\n", hr);
        ok(!out, "Got %p.\n", out);
        if (i == 0 || i == 3) CloseHandle(other);
        winetest_pop_context();
    }
    CloseHandle(handle);
    refcount = IDCompositionDesktopDevice_Release(viewer);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    hwnd = CreateWindowExW(WS_EX_TOPMOST, L"static", L"dcomp_shared_visual_portal",
            WS_OVERLAPPED | WS_VISIBLE, 0, 0, window_size, window_size, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");
    if (!hwnd) return;
    MoveWindow(hwnd, 0, 0, window_size, window_size, TRUE);

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
            D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    ok(hr == S_OK, "Failed to create D3D11 device, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return;
    }

    /* order 0 releases the viewer first, order 1 the device of the portal first */
    for (order = 0; order < 2; ++order)
    {
        winetest_push_context("order %u", order);

        hr = create_handle(&handle);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ok(DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS),
                "DuplicateHandle failed, error %lu.\n", GetLastError());

        /* the viewer: the shared visual is the root of the window's target */
        hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&viewer);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = open_shared(viewer, handle, &IID_IDCompositionVisual2, (void **)&shared);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_CreateTargetForHwnd(viewer, hwnd, TRUE, &window_target);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)shared);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_Commit(viewer);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        /* the portal: another device opens a duplicate of the handle as a target, and its root is a green surface */
        hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = open_shared(portal_device, duplicate, &IID_IDCompositionTarget, (void **)&portal);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_CreateVisual(portal_device, &portal_root);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_CreateSurfaceFactory(portal_device, (IUnknown *)d3d11_device, &factory);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionSurfaceFactory_CreateSurface(factory, surface_size, surface_size,
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        render_surface_color(d3d11_device, surface, 0.0f, 1.0f, 0.0f);
        hr = IDCompositionVisual2_SetContent(portal_root, (IUnknown *)surface);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionTarget_SetRoot(portal, (IDCompositionVisual *)portal_root);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_Commit(portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_WaitForCommitCompletion(portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        expect_rendered_color(hwnd, 50, 50, RGB(0, 0xff, 0));

        /* a commit of the portal's device alone shows its new content in the viewer's window */
        render_surface_color(d3d11_device, surface, 1.0f, 1.0f, 0.0f);
        hr = IDCompositionDesktopDevice_Commit(portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_WaitForCommitCompletion(portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        expect_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));

        IDCompositionSurface_Release(surface);
        IDCompositionSurfaceFactory_Release(factory);
        IDCompositionVisual2_Release(portal_root);
        CloseHandle(handle);
        CloseHandle(duplicate);

        if (!order)
        {
            /* the viewer goes first; the portal's device still commits */
            IDCompositionTarget_Release(window_target);
            IDCompositionVisual2_Release(shared);
            refcount = IDCompositionDesktopDevice_Release(viewer);
            ok(!refcount, "Viewer has %lu references left.\n", refcount);
            hr = IDCompositionDesktopDevice_Commit(portal_device);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            /* the committed tree holds the device's objects until a commit without it (Windows 11) */
            hr = IDCompositionTarget_SetRoot(portal, NULL);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            hr = IDCompositionDesktopDevice_Commit(portal_device);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            IDCompositionTarget_Release(portal);
            refcount = IDCompositionDesktopDevice_Release(portal_device);
            ok(!refcount, "Portal device has %lu references left.\n", refcount);
        }
        else
        {
            /* the portal goes first; the viewer still commits */
            hr = IDCompositionTarget_SetRoot(portal, NULL);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            hr = IDCompositionDesktopDevice_Commit(portal_device);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            IDCompositionTarget_Release(portal);
            refcount = IDCompositionDesktopDevice_Release(portal_device);
            ok(!refcount, "Portal device has %lu references left.\n", refcount);
            hr = IDCompositionDesktopDevice_Commit(viewer);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            IDCompositionTarget_Release(window_target);
            IDCompositionVisual2_Release(shared);
            refcount = IDCompositionDesktopDevice_Release(viewer);
            ok(!refcount, "Viewer has %lu references left.\n", refcount);
        }
        winetest_pop_context();
    }

    refcount = ID3D11Device_Release(d3d11_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    DestroyWindow(hwnd);
}

#define expect_not_rendered_color(a, b, c, d) _expect_not_rendered_color(__LINE__, a, b, c, d)
static void _expect_not_rendered_color(int line, HWND hwnd, int x, int y, COLORREF old_color)
{
    COLORREF color = CLR_INVALID;
    DWORD start = GetTickCount();
    POINT point = {x, y};
    HDC hdc;

    ClientToScreen(hwnd, &point);
    hdc = GetDC(NULL);
    while (GetTickCount() - start < 1000)
    {
        if ((color = GetPixel(hdc, point.x, point.y)) != old_color) break;
        Sleep(20);
    }
    ReleaseDC(NULL, hdc);
    ok_(__FILE__, line)(color != old_color, "Still color %#06lx at (%d, %d).\n", color, x, y);
}

static IDCompositionSurface *create_color_surface(IDCompositionDesktopDevice *device, ID3D11Device *d3d11_device,
        unsigned int size, float r, float g, float b)
{
    IDCompositionSurfaceFactory *factory;
    IDCompositionSurface *surface = NULL;
    HRESULT hr;

    hr = IDCompositionDesktopDevice_CreateSurfaceFactory(device, (IUnknown *)d3d11_device, &factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr)) return NULL;
    hr = IDCompositionSurfaceFactory_CreateSurface(factory, size, size, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionSurfaceFactory_Release(factory);
    if (surface) render_surface_color(d3d11_device, surface, r, g, b);
    return surface;
}

/* one thread's loop: commits its device, switching the portal's root and its content if given */
struct portal_thread
{
    IDCompositionDesktopDevice *device;
    IDCompositionTarget *target;
    IDCompositionVisual2 *roots[2];
    IDCompositionSurface *surface;
    unsigned int iterations;
    LONG failures;
};

static DWORD WINAPI portal_thread_proc(void *arg)
{
    struct portal_thread *t = arg;
    unsigned int i;

    for (i = 0; i < t->iterations; ++i)
    {
        if (t->target) IDCompositionTarget_SetRoot(t->target, (IDCompositionVisual *)t->roots[i & 1]);
        if (t->surface) IDCompositionVisual2_SetContent(t->roots[0], (i & 2) ? NULL : (IUnknown *)t->surface);
        if (FAILED(IDCompositionDesktopDevice_Commit(t->device))) InterlockedIncrement(&t->failures);
    }
    return 0;
}

static BOOL run_portal_threads(struct portal_thread *threads, unsigned int count)
{
    HANDLE handles[4];
    unsigned int i;
    DWORD ret;

    for (i = 0; i < count; ++i) handles[i] = CreateThread(NULL, 0, portal_thread_proc, &threads[i], 0, NULL);
    ret = WaitForMultipleObjects(count, handles, TRUE, 120000);
    for (i = 0; i < count; ++i) CloseHandle(handles[i]);
    return ret < WAIT_OBJECT_0 + count;
}

static SIZE_T get_private_bytes(void)
{
    BOOL (WINAPI *get_info)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
    PROCESS_MEMORY_COUNTERS counters = {sizeof(counters)};

    get_info = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo");
    if (!get_info || !get_info(GetCurrentProcess(), &counters, sizeof(counters))) return 0;
    return counters.PagefileUsage;   /* the private bytes */
}

static DWORD get_handle_count(void)
{
    BOOL (WINAPI *get_count)(HANDLE, DWORD *);
    DWORD count = 0;

    get_count = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessHandleCount");
    if (get_count) get_count(GetCurrentProcess(), &count);
    return count;
}

/* The portal cases of the signed-off design (docs/uwp/PORTAL_SNAPSHOT_DESIGN.md in WineExplorer): one target per object, removal,
 * swap chains, two devices committing at once, a cycle and nesting of portals, and the registry of shared handles. */
static void test_shared_visual_portal_cases(void)
{
    static const unsigned int window_size = 200, surface_size = 100;
    typedef HRESULT (WINAPI *create_handle_t)(HANDLE *);
    IDCompositionDesktopDevice *viewer, *portal_device, *device_a, *device_b, *device_c;
    IDCompositionVisual2 *shared, *shared2, *root, *root2, *child, *v_c_a, *v_a_b, *root_d, *root_e, *v_a_e, *v_b_d;
    IDCompositionTarget *window_target, *portal, *portal_a, *portal_b, *portal_d, *portal_e;
    IDCompositionSurface *surface, *surface2;
    struct portal_thread threads[2];
    D3D_FEATURE_LEVEL feature_level;
    create_handle_t create_handle;
    HANDLE handle, duplicate, h_a, h_b, h_d, h_e, other;
    ID3D11Device *d3d11_device;
    BOOL is_wine = !strcmp(winetest_platform, "wine");
    SIZE_T before, after;
    DWORD count_before, count_after;
    unsigned int i;
    ULONG refcount;
    void *out;
    HRESULT hr;
    HWND hwnd;

    create_handle = (void *)GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionCreateSharedVisualHandle");
    if (!create_handle || !pDCompositionCreateDevice3)
    {
        win_skip("DCompositionCreateSharedVisualHandle() or DCompositionCreateDevice3() is unavailable.\n");
        return;
    }
    hwnd = CreateWindowExW(WS_EX_TOPMOST, L"static", L"dcomp_shared_visual_portal_cases",
            WS_OVERLAPPED | WS_VISIBLE, 0, 0, window_size, window_size, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");
    if (!hwnd) return;
    MoveWindow(hwnd, 0, 0, window_size, window_size, TRUE);
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
            D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    ok(hr == S_OK, "Failed to create D3D11 device, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return;
    }

    /* A duplicate handle is the same object, also after the original is closed: while the visual lives, another open is
     * E_ACCESSDENIED, and once it is released the duplicate opens a visual again. An object opens as a target once (Windows 11:
     * E_ACCESSDENIED for a second target, also after the first was released, on any device). */
    winetest_push_context("one target");
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = create_handle(&handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS);
    hr = open_shared(viewer, handle, &IID_IDCompositionVisual2, (void **)&shared);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    CloseHandle(handle);
    out = (void *)0xdeadbeef;
    hr = open_shared(viewer, duplicate, &IID_IDCompositionVisual2, &out);
    ok(hr == E_ACCESSDENIED, "Got unexpected hr %#lx.\n", hr);
    ok(!out, "Got %p.\n", out);
    if (SUCCEEDED(hr)) IUnknown_Release((IUnknown *)out);
    IDCompositionVisual2_Release(shared);
    hr = open_shared(viewer, duplicate, &IID_IDCompositionVisual2, (void **)&shared);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(viewer, duplicate, &IID_IDCompositionTarget, (void **)&portal);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    out = (void *)0xdeadbeef;
    hr = open_shared(viewer, duplicate, &IID_IDCompositionTarget, &out);
    ok(hr == E_ACCESSDENIED, "Got unexpected hr %#lx.\n", hr);
    ok(!out, "Got %p.\n", out);
    IDCompositionTarget_Release(portal);
    out = (void *)0xdeadbeef;
    hr = open_shared(portal_device, duplicate, &IID_IDCompositionTarget, &out);
    ok(hr == E_ACCESSDENIED, "Got unexpected hr %#lx.\n", hr);
    ok(!out, "Got %p.\n", out);
    if (shared) IDCompositionVisual2_Release(shared);
    CloseHandle(duplicate);
    refcount = IDCompositionDesktopDevice_Release(portal_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDCompositionDesktopDevice_Release(viewer);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    winetest_pop_context();

    /* Removal: a root of NULL, and the release of a portal target with a root, take the portal out of the viewer's window. */
    winetest_push_context("removal");
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = create_handle(&handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(viewer, handle, &IID_IDCompositionVisual2, (void **)&shared);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(viewer, hwnd, TRUE, &window_target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)shared);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(portal_device, handle, &IID_IDCompositionTarget, (void **)&portal);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(portal_device, &root);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    surface = create_color_surface(portal_device, d3d11_device, surface_size, 1.0f, 1.0f, 0.0f);
    IDCompositionVisual2_SetContent(root, (IUnknown *)surface);
    IDCompositionTarget_SetRoot(portal, (IDCompositionVisual *)root);
    hr = IDCompositionDesktopDevice_Commit(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));
    IDCompositionTarget_SetRoot(portal, NULL);
    hr = IDCompositionDesktopDevice_Commit(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_not_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));
    IDCompositionTarget_SetRoot(portal, (IDCompositionVisual *)root);
    hr = IDCompositionDesktopDevice_Commit(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));
    /* A released portal target keeps showing what it committed last (Windows 11), also when the viewer commits, takes the
     * shared visual out of its window and puts it back. */
    IDCompositionTarget_Release(portal);
    hr = IDCompositionDesktopDevice_Commit(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));
    IDCompositionTarget_SetRoot(window_target, NULL);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_not_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));
    IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)shared);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));
    winetest_pop_context();

    /* A swap chain in a portal's tree is not shown (a FIXME in Wine); the commits succeed and the surface beside it shows. */
    winetest_push_context("swap chain");
    {
        DXGI_SWAP_CHAIN_DESC1 desc = {64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_PREMULTIPLIED, 0};
        IDXGISwapChain1 *swapchain = NULL;
        IDXGIFactory2 *factory = NULL;
        IDXGIAdapter *adapter;
        IDXGIDevice *dxgi_device;

        ID3D11Device_QueryInterface(d3d11_device, &IID_IDXGIDevice, (void **)&dxgi_device);
        IDXGIDevice_GetAdapter(dxgi_device, &adapter);
        IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
        hr = IDXGIFactory2_CreateSwapChainForComposition(factory, (IUnknown *)d3d11_device, &desc, NULL, &swapchain);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDXGIFactory2_Release(factory);
        IDXGIAdapter_Release(adapter);
        IDXGIDevice_Release(dxgi_device);

        hr = create_handle(&h_a);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = open_shared(viewer, h_a, &IID_IDCompositionVisual2, (void **)&shared2);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)shared2);
        hr = IDCompositionDesktopDevice_Commit(viewer);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = open_shared(portal_device, h_a, &IID_IDCompositionTarget, (void **)&portal);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_CreateVisual(portal_device, &child);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionVisual2_SetOffsetX(child, 120.0f);
        if (swapchain) IDCompositionVisual2_SetContent(child, (IUnknown *)swapchain);
        render_surface_color(d3d11_device, surface, 0.0f, 1.0f, 0.0f);
        IDCompositionVisual2_AddVisual(root, (IDCompositionVisual *)child, FALSE, NULL);
        IDCompositionTarget_SetRoot(portal, (IDCompositionVisual *)root);
        hr = IDCompositionDesktopDevice_Commit(portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDesktopDevice_Commit(viewer);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        expect_rendered_color(hwnd, 50, 50, RGB(0, 0xff, 0));
        IDCompositionVisual2_RemoveAllVisuals(root);
        IDCompositionTarget_SetRoot(portal, NULL);
        hr = IDCompositionDesktopDevice_Commit(portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionTarget_Release(portal);
        IDCompositionVisual2_SetContent(child, NULL);
        IDCompositionVisual2_Release(child);
        if (swapchain) IDXGISwapChain1_Release(swapchain);
        IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)shared);
        IDCompositionVisual2_Release(shared2);
        CloseHandle(h_a);
    }
    winetest_pop_context();

    /* Two threads: the viewer commits while the portal's device switches its root and the root's content (P1 of the review). */
    winetest_push_context("concurrent");
    hr = create_handle(&h_b);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(viewer, h_b, &IID_IDCompositionVisual2, (void **)&shared2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)shared2);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(portal_device, h_b, &IID_IDCompositionTarget, (void **)&portal);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(portal_device, &root2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    surface2 = create_color_surface(portal_device, d3d11_device, surface_size, 0.0f, 0.0f, 1.0f);
    IDCompositionVisual2_SetContent(root2, (IUnknown *)surface2);
    memset(threads, 0, sizeof(threads));
    threads[0].device = viewer;
    threads[0].iterations = 500;
    threads[1].device = portal_device;
    threads[1].target = portal;
    threads[1].roots[0] = root;
    threads[1].roots[1] = root2;
    threads[1].surface = surface;
    threads[1].iterations = 500;
    ok(run_portal_threads(threads, 2), "The threads did not finish.\n");
    ok(!threads[0].failures, "The viewer's commit failed %ld times.\n", threads[0].failures);
    ok(!threads[1].failures, "The portal's commit failed %ld times.\n", threads[1].failures);
    IDCompositionTarget_SetRoot(portal, NULL);
    hr = IDCompositionDesktopDevice_Commit(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_Release(portal);
    IDCompositionTarget_SetRoot(window_target, NULL);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_Release(window_target);
    IDCompositionVisual2_Release(shared2);
    IDCompositionVisual2_Release(shared);
    IDCompositionVisual2_SetContent(root, NULL);
    IDCompositionVisual2_SetContent(root2, NULL);
    IDCompositionVisual2_Release(root2);
    IDCompositionVisual2_Release(root);
    IDCompositionSurface_Release(surface2);
    IDCompositionSurface_Release(surface);
    CloseHandle(h_b);
    CloseHandle(handle);
    /* Windows 11 may still hold the objects of the last frames here (it releases them later) */
    IDCompositionDesktopDevice_WaitForCommitCompletion(portal_device);
    IDCompositionDesktopDevice_WaitForCommitCompletion(viewer);
    refcount = IDCompositionDesktopDevice_Release(portal_device);
    ok(!refcount || broken(refcount), "Portal device has %lu references left.\n", refcount);
    refcount = IDCompositionDesktopDevice_Release(viewer);
    ok(!refcount || broken(refcount), "Viewer has %lu references left.\n", refcount);
    winetest_pop_context();

    /* Nesting: device C's window shows the portal of A, and A's portal tree holds a visual of B's portal: B's green surface shows in
     * C's window through A (Wine flattens a nested portal into the outer snapshot). Then a cycle of two other objects: A's portal
     * tree shows B's portal and B's shows A's (each object has one visual at a time). A and B committing at once both return, and
     * memory does not grow. */
    winetest_push_context("nesting");
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&device_a);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&device_b);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&device_c);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    create_handle(&h_a);
    create_handle(&h_b);
    hr = open_shared(device_c, h_a, &IID_IDCompositionVisual2, (void **)&v_c_a);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device_c, hwnd, TRUE, &window_target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)v_c_a);
    hr = IDCompositionDesktopDevice_Commit(device_c);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = open_shared(device_a, h_a, &IID_IDCompositionTarget, (void **)&portal_a);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(device_a, h_b, &IID_IDCompositionVisual2, (void **)&v_a_b);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device_a, &root);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionVisual2_AddVisual(root, (IDCompositionVisual *)v_a_b, FALSE, NULL);
    IDCompositionTarget_SetRoot(portal_a, (IDCompositionVisual *)root);

    hr = open_shared(device_b, h_b, &IID_IDCompositionTarget, (void **)&portal_b);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device_b, &root2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    surface2 = create_color_surface(device_b, d3d11_device, surface_size, 0.0f, 1.0f, 0.0f);
    IDCompositionVisual2_SetContent(root2, (IUnknown *)surface2);
    IDCompositionTarget_SetRoot(portal_b, (IDCompositionVisual *)root2);

    hr = IDCompositionDesktopDevice_Commit(device_b);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(device_a);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0, 0xff, 0));
    /* a commit of the inner portal's device alone shows (Notepad's compositor commits without the UI thread) */
    render_surface_color(d3d11_device, surface2, 0.0f, 0.0f, 1.0f);
    hr = IDCompositionDesktopDevice_Commit(device_b);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0, 0, 0xff));
    render_surface_color(d3d11_device, surface2, 0.0f, 1.0f, 0.0f);

    /* the cycle: D on device A shows E, E on device B shows D */
    create_handle(&h_d);
    create_handle(&h_e);
    hr = open_shared(device_a, h_d, &IID_IDCompositionTarget, (void **)&portal_d);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(device_a, h_e, &IID_IDCompositionVisual2, (void **)&v_a_e);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device_a, &root_d);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionVisual2_AddVisual(root_d, (IDCompositionVisual *)v_a_e, FALSE, NULL);
    IDCompositionTarget_SetRoot(portal_d, (IDCompositionVisual *)root_d);
    hr = open_shared(device_b, h_e, &IID_IDCompositionTarget, (void **)&portal_e);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(device_b, h_d, &IID_IDCompositionVisual2, (void **)&v_b_d);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device_b, &root_e);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionVisual2_SetContent(root_e, (IUnknown *)surface2);
    IDCompositionVisual2_SetOffsetX(v_b_d, 10.0f);
    IDCompositionVisual2_AddVisual(root_e, (IDCompositionVisual *)v_b_d, FALSE, NULL);
    IDCompositionTarget_SetRoot(portal_e, (IDCompositionVisual *)root_e);

    /* warm up, then measure */
    memset(threads, 0, sizeof(threads));
    threads[0].device = device_a;
    threads[0].iterations = 50;
    threads[1].device = device_b;
    threads[1].iterations = 50;
    ok(run_portal_threads(threads, 2), "The threads did not finish.\n");
    before = get_private_bytes();
    threads[0].iterations = threads[1].iterations = 400;
    ok(run_portal_threads(threads, 2), "The threads did not finish.\n");
    after = get_private_bytes();
    ok(!threads[0].failures && !threads[1].failures, "Commits failed %ld and %ld times.\n",
            threads[0].failures, threads[1].failures);
    if (is_wine && before && after)
        ok(after < before + 8 * 1024 * 1024, "Private bytes grew from %Iu to %Iu.\n", before, after);

    IDCompositionTarget_SetRoot(portal_a, NULL);
    IDCompositionTarget_SetRoot(portal_b, NULL);
    IDCompositionTarget_SetRoot(portal_d, NULL);
    IDCompositionTarget_SetRoot(portal_e, NULL);
    IDCompositionDesktopDevice_Commit(device_a);
    IDCompositionDesktopDevice_Commit(device_b);
    IDCompositionTarget_SetRoot(window_target, NULL);
    IDCompositionDesktopDevice_Commit(device_c);
    IDCompositionTarget_Release(window_target);
    IDCompositionTarget_Release(portal_a);
    IDCompositionTarget_Release(portal_b);
    IDCompositionTarget_Release(portal_d);
    IDCompositionTarget_Release(portal_e);
    IDCompositionVisual2_RemoveAllVisuals(root);
    IDCompositionVisual2_RemoveAllVisuals(root_d);
    IDCompositionVisual2_RemoveAllVisuals(root_e);
    IDCompositionVisual2_SetContent(root2, NULL);
    IDCompositionVisual2_SetContent(root_e, NULL);
    IDCompositionVisual2_Release(root);
    IDCompositionVisual2_Release(root2);
    IDCompositionVisual2_Release(root_d);
    IDCompositionVisual2_Release(root_e);
    IDCompositionVisual2_Release(v_a_b);
    IDCompositionVisual2_Release(v_a_e);
    IDCompositionVisual2_Release(v_b_d);
    IDCompositionVisual2_Release(v_c_a);
    IDCompositionSurface_Release(surface2);
    CloseHandle(h_a);
    CloseHandle(h_b);
    CloseHandle(h_d);
    CloseHandle(h_e);
    /* (see above: Windows 11 may still hold objects of the last frames) */
    refcount = IDCompositionDesktopDevice_Release(device_c);
    ok(!refcount || broken(refcount), "Device C has %lu references left.\n", refcount);
    refcount = IDCompositionDesktopDevice_Release(device_b);
    ok(!refcount || broken(refcount), "Device B has %lu references left.\n", refcount);
    refcount = IDCompositionDesktopDevice_Release(device_a);
    ok(!refcount || broken(refcount), "Device A has %lu references left.\n", refcount);
    winetest_pop_context();

    /* The registry of shared handles: a named event of the caller's is not a shared handle, and a share leaves the registry
     * once its handles are all closed (Wine: the process's handle count stays flat). */
    winetest_push_context("registry");
    hr = pDCompositionCreateDevice3(NULL, &IID_IDCompositionDesktopDevice, (void **)&viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    other = CreateEventW(NULL, TRUE, FALSE, L"wine_dcomp_test_named_event");
    out = (void *)0xdeadbeef;
    hr = open_shared(viewer, other, &IID_IDCompositionVisual, &out);
    ok(hr == E_HANDLE, "Got unexpected hr %#lx.\n", hr);
    ok(!out, "Got %p.\n", out);
    CloseHandle(other);
    if (is_wine)
    {
        create_handle(&handle);
        CloseHandle(handle);
        count_before = get_handle_count();
        for (i = 0; i < 32; ++i)
        {
            create_handle(&handle);
            CloseHandle(handle);
        }
        count_after = get_handle_count();
        ok(count_after <= count_before + 1, "Handle count grew from %lu to %lu.\n", count_before, count_after);
    }
    refcount = IDCompositionDesktopDevice_Release(viewer);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    winetest_pop_context();

    refcount = ID3D11Device_Release(d3d11_device);
    ok(!refcount || broken(refcount), "Device has %lu references left.\n", refcount);
    DestroyWindow(hwnd);
}

/* Based on Wine-Staging patch 0014 by Zhiyi Zhang */
static void test_target_SetRoot(void)
{
    IDCompositionTarget *target, *target2;
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    HWND hwnd, hwnd2, hwnd3;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)swapchain);
    ok(hr == S_OK || broken(hr == DXGI_ERROR_UNSUPPORTED),
            "Got unexpected hr %#lx.\n", hr);
    hwnd2 = create_window();
    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd2, TRUE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hwnd3 = create_window();
    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd3, TRUE, &target2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target, visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* SetRoot with a visual already bound to a target */
    hr = IDCompositionTarget_SetRoot(target2, visual);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target2, visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    DestroyWindow(hwnd3);
    DestroyWindow(hwnd2);
    IDCompositionTarget_Release(target2);
    IDCompositionTarget_Release(target);
    IDCompositionVisual_Release(visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0050 by Zhiyi Zhang */
static void test_visual_SetContent(void)
{
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Obviously IDCompositionDevice doesn't have IDXGISwapChain1 */
    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)dcomp_device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)swapchain);
    ok(hr == S_OK || broken(hr == DXGI_ERROR_UNSUPPORTED),
            "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetContent(visual, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionVisual_Release(visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0039 by Zhiyi Zhang */
static void test_visual_SetBitmapInterpolationMode(void)
{
    static const struct
    {
        unsigned int mode;
        HRESULT hr;
    }
    tests[] =
    {
        {DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, S_OK},
        {DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR, S_OK},
        {DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT, S_OK},
        {DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR + 1, E_INVALIDARG},
    };
    IDCompositionDesktopDevice *dcomp_device;
    IDCompositionVisual2 *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    unsigned int i;
    HRESULT hr;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        hr = IDCompositionVisual2_SetBitmapInterpolationMode(visual, tests[i].mode);
        ok(hr == tests[i].hr, "Got unexpected hr %#lx for mode %u.\n", hr, tests[i].mode);
    }

    IDCompositionVisual2_Release(visual);
    refcount = IDCompositionDesktopDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0039 by Zhiyi Zhang */
static void test_visual_SetBorderMode(void)
{
    static const struct
    {
        unsigned int mode;
        HRESULT hr;
    }
    tests[] =
    {
        {DCOMPOSITION_BORDER_MODE_SOFT, S_OK},
        {DCOMPOSITION_BORDER_MODE_HARD, S_OK},
        {DCOMPOSITION_BORDER_MODE_INHERIT, S_OK},
        {DCOMPOSITION_BORDER_MODE_HARD + 1, E_INVALIDARG},
    };
    IDCompositionDesktopDevice *dcomp_device;
    IDCompositionVisual2 *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    unsigned int i;
    HRESULT hr;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        hr = IDCompositionVisual2_SetBorderMode(visual, tests[i].mode);
        ok(hr == tests[i].hr, "Got unexpected hr %#lx for mode %u.\n", hr, tests[i].mode);
    }

    IDCompositionVisual2_Release(visual);
    refcount = IDCompositionDesktopDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0039 by Zhiyi Zhang */
static void test_visual_SetBackFaceVisibility(void)
{
    static const struct
    {
        unsigned int visibility;
        HRESULT hr;
    }
    tests[] =
    {
        {DCOMPOSITION_BACKFACE_VISIBILITY_VISIBLE, S_OK},
        {DCOMPOSITION_BACKFACE_VISIBILITY_HIDDEN, S_OK},
        {DCOMPOSITION_BACKFACE_VISIBILITY_INHERIT, S_OK},
        {DCOMPOSITION_BACKFACE_VISIBILITY_HIDDEN + 1, E_INVALIDARG},
    };
    IDCompositionDesktopDevice *dcomp_device;
    IDCompositionVisual2 *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    unsigned int i;
    HRESULT hr;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        hr = IDCompositionVisual2_SetBackFaceVisibility(visual, tests[i].visibility);
        ok(hr == tests[i].hr, "Got unexpected hr %#lx for visibility %u.\n", hr, tests[i].visibility);
    }

    IDCompositionVisual2_Release(visual);
    refcount = IDCompositionDesktopDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0045 by Zhiyi Zhang */
static void test_visual_SetOffsetX(void)
{
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetOffsetX(visual, 0);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetOffsetX(visual, 2000000.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetOffsetX(visual, -2000000.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionVisual_Release(visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0044 by Zhiyi Zhang */
static void test_visual_SetOffsetXAnimation(void)
{
    IDCompositionAnimation *animation;
    IDCompositionDevice *dcomp_device;
    IDCompositionVisual *visual;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;
    HWND hwnd;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateAnimation(dcomp_device, &animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    /* NULL animation pointer */
    hr = IDCompositionVisual_SetOffsetXAnimation(visual, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* An animation which didn't end */
    hr = IDCompositionVisual_SetOffsetXAnimation(visual, animation);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Normal animation */
    hr = IDCompositionAnimation_End(animation, 1, 1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetOffsetXAnimation(visual, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionAnimation_Release(animation);
done:
    IDCompositionVisual_Release(visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_animation(void)
{
    IDCompositionAnimation *animation;
    IDCompositionDevice *dcomp_device;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateAnimation(dcomp_device, &animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    /* AddRepeat before any segment */
    hr = IDCompositionAnimation_AddRepeat(animation, 0, 1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* AddCubic: valid */
    hr = IDCompositionAnimation_AddCubic(animation, 0, 0, 1, 0, 0);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* AddCubic: offset earlier than the last end */
    hr = IDCompositionAnimation_AddCubic(animation, -1, 0, 1, 0, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* AddCubic: NaN offset */
    hr = IDCompositionAnimation_AddCubic(animation, NAN, 0, 1, 0, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* AddCubic: NaN coefficient */
    hr = IDCompositionAnimation_AddCubic(animation, 0, NAN, 1, 0, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* AddRepeat: offset not after the previous segment */
    hr = IDCompositionAnimation_AddRepeat(animation, 0, 1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* AddRepeat: valid */
    hr = IDCompositionAnimation_AddRepeat(animation, 1, 1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* AddRepeat: negative duration */
    hr = IDCompositionAnimation_AddRepeat(animation, 1, -1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* AddRepeat: duration longer than the offset */
    hr = IDCompositionAnimation_AddRepeat(animation, 2, 3);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* AddSinusoidal: valid */
    hr = IDCompositionAnimation_AddSinusoidal(animation, 1, 0, 1, 1, 0);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* AddSinusoidal: offset earlier than the last end */
    hr = IDCompositionAnimation_AddSinusoidal(animation, 0, 0, 1, 1, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* End: valid */
    hr = IDCompositionAnimation_End(animation, 2, 1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* AddCubic after End */
    hr = IDCompositionAnimation_AddCubic(animation, 3, 0, 1, 0, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Reset, then End before any segment */
    hr = IDCompositionAnimation_Reset(animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionAnimation_End(animation, 1, 1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionAnimation_Release(animation);
done:
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* Based on Wine-Staging patch 0039 by Zhiyi Zhang */
static void test_visual_AddVisual(void)
{
    IDCompositionVisual *parent_visual, *parent_visual2, *child_visual, *ref_visual;
    IDCompositionDevice *dcomp_device;
    IDXGISwapChain *swapchain;
    IDXGIDevice *dxgi_device;
    IDCompositionTarget *target;
    ULONG refcount;
    HRESULT hr;
    HWND hwnd, hwnd2;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hwnd = create_window();
    swapchain = create_swapchain(dxgi_device, hwnd);
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &parent_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &parent_visual2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &child_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &ref_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Adding NULL visual */
    hr = IDCompositionVisual_AddVisual(parent_visual, NULL, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Adding a visual with a reference visual not added to any parent */
    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, FALSE, ref_visual);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Adding a visual with a reference visual added to a different parent */
    hr = IDCompositionVisual_AddVisual(parent_visual2, ref_visual, FALSE, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, FALSE, ref_visual);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_RemoveAllVisuals(parent_visual2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Adding a root visual */
    hwnd2 = create_window();
    hr = IDCompositionDevice_CreateTargetForHwnd(dcomp_device, hwnd2, TRUE, &target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(target, child_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionTarget_SetRoot(target, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionTarget_Release(target);
    DestroyWindow(hwnd2);

    /* Adding a visual below a reference visual */
    hr = IDCompositionVisual_AddVisual(parent_visual, ref_visual, FALSE, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_ref(parent_visual, 1);
    expect_ref(child_visual, 1);
    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, FALSE, ref_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_ref(parent_visual, 1);
    expect_ref(child_visual, 1);

    /* Adding a visual that's already a child */
    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Adding a visual above a reference visual */
    hr = IDCompositionVisual_RemoveVisual(parent_visual, child_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, TRUE, ref_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Adding a visual above all child visuals */
    hr = IDCompositionVisual_RemoveAllVisuals(parent_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, TRUE, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Adding a visual below all child visuals */
    hr = IDCompositionVisual_RemoveAllVisuals(parent_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent_visual, child_visual, FALSE, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDCompositionVisual_RemoveAllVisuals(parent_visual2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual_RemoveAllVisuals(parent_visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionVisual_Release(ref_visual);
    IDCompositionVisual_Release(child_visual);
    IDCompositionVisual_Release(parent_visual2);
    IDCompositionVisual_Release(parent_visual);
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    IDXGISwapChain_Release(swapchain);
    DestroyWindow(hwnd);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_clip_animation(void)
{
    IDCompositionAnimation *animation;
    IDCompositionDevice *dcomp_device;
    IDCompositionRectangleClip *clip;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateRectangleClip(dcomp_device, &clip);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateAnimation(dcomp_device, &animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    /* NULL animation pointer */
    hr = IDCompositionRectangleClip_SetLeftAnimation(clip, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopLeftRadiusXAnimation(clip, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* An animation which didn't end */
    hr = IDCompositionRectangleClip_SetLeftAnimation(clip, animation);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopLeftRadiusXAnimation(clip, animation);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Normal (ended) animation accepted on every property */
    hr = IDCompositionAnimation_End(animation, 1, 1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetLeftAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetRightAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetBottomAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopLeftRadiusXAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopLeftRadiusYAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopRightRadiusXAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopRightRadiusYAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetBottomLeftRadiusXAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetBottomLeftRadiusYAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetBottomRightRadiusXAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetBottomRightRadiusYAnimation(clip, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Plain setters: only finiteness is validated (no range, negative radii ok) */
    hr = IDCompositionRectangleClip_SetLeft(clip, NAN);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetLeft(clip, 2000000.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetLeft(clip, -2000000.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionRectangleClip_SetTopLeftRadiusX(clip, -1.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionAnimation_Release(animation);
    IDCompositionRectangleClip_Release(clip);
done:
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_effect_group_opacity(void)
{
    IDCompositionAnimation *animation;
    IDCompositionDevice *dcomp_device;
    IDCompositionEffectGroup *group;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateEffectGroup(dcomp_device, &group);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateAnimation(dcomp_device, &animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    /* NULL animation pointer */
    hr = IDCompositionEffectGroup_SetOpacityAnimation(group, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* An animation which didn't end */
    hr = IDCompositionEffectGroup_SetOpacityAnimation(group, animation);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Normal (ended) animation */
    hr = IDCompositionAnimation_End(animation, 1, 1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionEffectGroup_SetOpacityAnimation(group, animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Plain opacity: only finiteness is validated (values outside [0,1] accepted) */
    hr = IDCompositionEffectGroup_SetOpacity(group, NAN);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionEffectGroup_SetOpacity(group, 2.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionEffectGroup_SetOpacity(group, -1.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionAnimation_Release(animation);
    IDCompositionEffectGroup_Release(group);
done:
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_transforms(void)
{
    IDCompositionAnimation *animation;
    IDCompositionDevice *dcomp_device;
    IDCompositionTranslateTransform *translate;
    IDCompositionScaleTransform *scale;
    IDCompositionRotateTransform *rotate;
    IDCompositionSkewTransform *skew;
    IDCompositionMatrixTransform *matrix;
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateAnimation(dcomp_device, &animation);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    /* NULL out pointer */
    hr = IDCompositionDevice_CreateTranslateTransform(dcomp_device, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    /* Translate transform */
    hr = IDCompositionDevice_CreateTranslateTransform(dcomp_device, &translate);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        void *p1, *p2;
        expect_ref(translate, 1);
        check_interface(translate, &IID_IUnknown, TRUE);
        check_interface(translate, &IID_IDCompositionEffect, TRUE);
        check_interface(translate, &IID_IDCompositionTransform3D, TRUE);
        check_interface(translate, &IID_IDCompositionTransform, TRUE);
        check_interface(translate, &IID_IDCompositionTranslateTransform, TRUE);
        check_interface(translate, &IID_IDCompositionScaleTransform, FALSE);
        check_interface(translate, &IID_IDCompositionVisual, FALSE);
        /* All transform interfaces resolve to the same pointer */
        hr = IUnknown_QueryInterface((IUnknown *)translate, &IID_IDCompositionTransform, &p1);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IUnknown_QueryInterface((IUnknown *)translate, &IID_IDCompositionTranslateTransform, &p2);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ok(p1 == p2, "Expected the same pointer, got %p and %p.\n", p1, p2);
        IUnknown_Release((IUnknown *)p1);
        IUnknown_Release((IUnknown *)p2);

        /* Plain setters: only finiteness is validated */
        hr = IDCompositionTranslateTransform_SetOffsetX(translate, NAN);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionTranslateTransform_SetOffsetX(translate, 2000000.0f);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionTranslateTransform_SetOffsetY(translate, -2000000.0f);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        /* Animation setters */
        hr = IDCompositionTranslateTransform_SetOffsetXAnimation(translate, NULL);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionTranslateTransform_SetOffsetXAnimation(translate, animation);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionAnimation_End(animation, 1, 1);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionTranslateTransform_SetOffsetXAnimation(translate, animation);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        IDCompositionTranslateTransform_Release(translate);
    }

    /* Scale transform */
    hr = IDCompositionDevice_CreateScaleTransform(dcomp_device, &scale);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        expect_ref(scale, 1);
        check_interface(scale, &IID_IDCompositionScaleTransform, TRUE);
        check_interface(scale, &IID_IDCompositionTranslateTransform, FALSE);
        hr = IDCompositionScaleTransform_SetScaleX(scale, NAN);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionScaleTransform_SetScaleX(scale, 0.0f);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionScaleTransform_SetCenterX(scale, -1.0f);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionScaleTransform_Release(scale);
    }

    /* Rotate transform */
    hr = IDCompositionDevice_CreateRotateTransform(dcomp_device, &rotate);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        expect_ref(rotate, 1);
        check_interface(rotate, &IID_IDCompositionRotateTransform, TRUE);
        hr = IDCompositionRotateTransform_SetAngle(rotate, NAN);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionRotateTransform_SetAngle(rotate, 360.0f);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionRotateTransform_Release(rotate);
    }

    /* Skew transform */
    hr = IDCompositionDevice_CreateSkewTransform(dcomp_device, &skew);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        expect_ref(skew, 1);
        check_interface(skew, &IID_IDCompositionSkewTransform, TRUE);
        hr = IDCompositionSkewTransform_SetAngleX(skew, NAN);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionSkewTransform_SetAngleX(skew, 45.0f);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionSkewTransform_Release(skew);
    }

    /* Matrix transform */
    hr = IDCompositionDevice_CreateMatrixTransform(dcomp_device, &matrix);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        D2D1_MATRIX_3X2_F m;
        expect_ref(matrix, 1);
        check_interface(matrix, &IID_IDCompositionMatrixTransform, TRUE);
        /* SetMatrix: no validation (NaN accepted) */
        m._11 = NAN; m._12 = 0.0f; m._21 = 0.0f; m._22 = 1.0f; m._31 = 0.0f; m._32 = 0.0f;
        hr = IDCompositionMatrixTransform_SetMatrix(matrix, &m);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        /* SetMatrixElement: row in [0,2], col in [0,1], value finite */
        hr = IDCompositionMatrixTransform_SetMatrixElement(matrix, 0, 0, NAN);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionMatrixTransform_SetMatrixElement(matrix, 2, 1, 1.0f);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionMatrixTransform_SetMatrixElement(matrix, 3, 0, 1.0f);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionMatrixTransform_SetMatrixElement(matrix, 0, 2, 1.0f);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        /* SetMatrixElementAnimation: same bounds plus animation validation */
        hr = IDCompositionMatrixTransform_SetMatrixElementAnimation(matrix, 0, 0, NULL);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionMatrixTransform_SetMatrixElementAnimation(matrix, 3, 0, animation);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionMatrixTransform_SetMatrixElementAnimation(matrix, 0, 0, animation);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDCompositionMatrixTransform_Release(matrix);
    }

    IDCompositionAnimation_Release(animation);
done:
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_visual_SetTransform_scale(void)
{
    IDCompositionDesktopDevice *dcomp_device;
    IDCompositionVisual2 *visual;
    D2D_MATRIX_3X2_F matrix = {{{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}}};
    IDXGIDevice *dxgi_device;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Chromium scales a 1x1 solid-colour surface up to the layer size. */
    matrix._11 = 1920.0f;
    matrix._22 = 1080.0f;
    matrix._31 = 4000000.0f;
    hr = IDCompositionVisual2_SetTransform(visual, &matrix);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDCompositionVisual2_Release(visual);
    refcount = IDCompositionDesktopDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_visual_SetTransformObject(void)
{
    IDCompositionDevice *dcomp_device, *dcomp_device2;
    IDCompositionVisual *visual;
    IDCompositionTranslateTransform *translate, *foreign;
    IDXGIDevice *dxgi_device, *dxgi_device2;
    ULONG refcount;
    HRESULT hr;

    if (!(dxgi_device = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice, (void **)&dcomp_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(dcomp_device, &visual);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateTranslateTransform(dcomp_device, &translate);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    /* NULL transform */
    hr = IDCompositionVisual_SetTransformObject(visual, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* Same-device transform: accepted, and no reference is taken */
    hr = IDCompositionVisual_SetTransformObject(visual, (IDCompositionTransform *)translate);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_ref(translate, 1);

    /* Clearing with NULL keeps the reference count unchanged */
    hr = IDCompositionVisual_SetTransformObject(visual, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_ref(translate, 1);

    /* Foreign-device transform is rejected */
    if ((dxgi_device2 = create_device(D3D10_CREATE_DEVICE_BGRA_SUPPORT)))
    {
        hr = pDCompositionCreateDevice(dxgi_device2, &IID_IDCompositionDevice, (void **)&dcomp_device2);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDCompositionDevice_CreateTranslateTransform(dcomp_device2, &foreign);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDCompositionVisual_SetTransformObject(visual, (IDCompositionTransform *)foreign);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
            IDCompositionTranslateTransform_Release(foreign);
        }
        refcount = IDCompositionDevice_Release(dcomp_device2);
        ok(!refcount, "Device has %lu references left.\n", refcount);
        refcount = IDXGIDevice_Release(dxgi_device2);
        ok(!refcount, "Device has %lu references left.\n", refcount);
    }

    IDCompositionTranslateTransform_Release(translate);
    IDCompositionVisual_Release(visual);
done:
    refcount = IDCompositionDevice_Release(dcomp_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    refcount = IDXGIDevice_Release(dxgi_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

/* The viewer's thread uses the viewer's D3D11 device (immediate context: draws into its own surface, commits its device) while
 * another thread commits the portal's device. A Commit of the portal redraws the viewer's window on the thread that committed,
 * so it must not touch the D3D11 device the application uses on its own thread. Failures show as a removed device, a failed
 * Commit, a wrong final color or a crash. */
struct viewer_thread
{
    IDCompositionDesktopDevice *device;
    ID3D11Device *d3d11_device;
    IDCompositionSurface *surface;
    unsigned int iterations;
    LONG failures;
};

static DWORD WINAPI viewer_thread_proc(void *arg)
{
    struct viewer_thread *t = arg;
    unsigned int i;

    for (i = 0; i < t->iterations; ++i)
    {
        render_surface_color(t->d3d11_device, t->surface, (i & 1) ? 1.0f : 0.0f, 0.0f, (i & 1) ? 0.0f : 1.0f);
        if (FAILED(IDCompositionDesktopDevice_Commit(t->device))) InterlockedIncrement(&t->failures);
    }
    return 0;
}

static void test_shared_visual_portal_viewer_thread(void)
{
    static const unsigned int window_size = 200, surface_size = 100, iterations = 300;
    typedef HRESULT (WINAPI *create_handle_t)(HANDLE *);
    IDCompositionVisual2 *shared, *portal_root, *own_root;
    IDCompositionDesktopDevice *viewer, *portal_device;
    IDCompositionTarget *window_target, *portal;
    IDCompositionSurface *own_surface, *portal_surface;
    create_handle_t create_handle;
    D3D_FEATURE_LEVEL feature_level;
    struct viewer_thread vt = {0};
    ID3D11Device *d3d11_device;
    HANDLE handle, thread;
    unsigned int i;
    ULONG refcount;
    HRESULT hr;
    HWND hwnd;

    create_handle = (void *)GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionCreateSharedVisualHandle");
    if (!create_handle || !pDCompositionCreateDevice3)
    {
        win_skip("DCompositionCreateSharedVisualHandle() or DCompositionCreateDevice3() is unavailable.\n");
        return;
    }
    hwnd = CreateWindowExW(WS_EX_TOPMOST, L"static", L"dcomp_portal_viewer_thread",
            WS_OVERLAPPED | WS_VISIBLE, 0, 0, window_size, window_size, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "Failed to create window.\n");
    if (!hwnd) return;
    MoveWindow(hwnd, 0, 0, window_size, window_size, TRUE);
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
            D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                D3D11_SDK_VERSION, &d3d11_device, &feature_level, NULL);
    ok(hr == S_OK, "Failed to create D3D11 device, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return;
    }

    /* the viewer: its window shows the shared visual, with a surface of its own as the visual's sibling content */
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = create_handle(&handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(viewer, handle, &IID_IDCompositionVisual2, (void **)&shared);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(viewer, &own_root);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    own_surface = create_color_surface(viewer, d3d11_device, surface_size, 0.0f, 0.0f, 1.0f);
    ok(own_surface != NULL, "Failed to create the viewer's surface.\n");
    hr = IDCompositionVisual2_SetContent(own_root, (IUnknown *)own_surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(own_root, (IDCompositionVisual *)shared, TRUE, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetOffsetX(shared, 100.0f);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(viewer, hwnd, TRUE, &window_target);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(window_target, (IDCompositionVisual *)own_root);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    /* the portal: another device, a surface that changes at each Commit */
    hr = pDCompositionCreateDevice3((IUnknown *)d3d11_device, &IID_IDCompositionDesktopDevice, (void **)&portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = open_shared(portal_device, handle, &IID_IDCompositionTarget, (void **)&portal);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(portal_device, &portal_root);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    portal_surface = create_color_surface(portal_device, d3d11_device, surface_size, 0.0f, 1.0f, 0.0f);
    ok(portal_surface != NULL, "Failed to create the portal's surface.\n");
    hr = IDCompositionVisual2_SetContent(portal_root, (IUnknown *)portal_surface);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(portal, (IDCompositionVisual *)portal_root);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_WaitForCommitCompletion(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 150, 50, RGB(0, 0xff, 0));

    /* the viewer's thread draws on the D3D11 device and commits; this thread commits the portal, with new pixels each time */
    vt.device = viewer;
    vt.d3d11_device = d3d11_device;
    vt.surface = own_surface;
    vt.iterations = iterations;
    thread = CreateThread(NULL, 0, viewer_thread_proc, &vt, 0, NULL);
    ok(thread != NULL, "Failed to create thread.\n");
    for (i = 0; i < iterations; ++i)
    {
        /* nothing is drawn here: the D3D11 device belongs to the viewer's thread. Every Commit publishes a snapshot and redraws
         * the viewers anyway (PORTAL_SNAPSHOT_DESIGN.md, section 1) */
        hr = IDCompositionDesktopDevice_Commit(portal_device);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        if (FAILED(hr)) break;
    }
    ok(WaitForSingleObject(thread, 120000) == WAIT_OBJECT_0, "The viewer's thread did not end.\n");
    CloseHandle(thread);
    ok(!vt.failures, "%ld commits of the viewer failed.\n", vt.failures);
    hr = ID3D11Device_GetDeviceRemovedReason(d3d11_device);
    ok(hr == S_OK, "The D3D11 device was removed, hr %#lx.\n", hr);

    /* both devices still work: a last Commit shows the viewer's own surface and the portal's */
    render_surface_color(d3d11_device, own_surface, 1.0f, 1.0f, 0.0f);
    hr = IDCompositionDesktopDevice_Commit(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_WaitForCommitCompletion(viewer);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    expect_rendered_color(hwnd, 50, 50, RGB(0xff, 0xff, 0));
    expect_rendered_color(hwnd, 150, 50, RGB(0, 0xff, 0));

    hr = IDCompositionTarget_SetRoot(portal, NULL);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(portal_device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDCompositionSurface_Release(portal_surface);
    IDCompositionVisual2_Release(portal_root);
    IDCompositionTarget_Release(portal);
    refcount = IDCompositionDesktopDevice_Release(portal_device);
    ok(!refcount, "Portal device has %lu references left.\n", refcount);
    IDCompositionTarget_Release(window_target);
    IDCompositionSurface_Release(own_surface);
    IDCompositionVisual2_Release(own_root);
    IDCompositionVisual2_Release(shared);
    CloseHandle(handle);
    refcount = IDCompositionDesktopDevice_Release(viewer);
    ok(!refcount, "Viewer has %lu references left.\n", refcount);
    refcount = ID3D11Device_Release(d3d11_device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
    DestroyWindow(hwnd);
}

START_TEST(dcomp)
{
    HMODULE module;

    module = LoadLibraryW(L"dcomp.dll");
    if (!module)
    {
        win_skip("dcomp.dll not found.\n");
        return;
    }

    pDCompositionCreateDevice = (void *)GetProcAddress(module, "DCompositionCreateDevice");
    pDCompositionCreateDevice2 = (void *)GetProcAddress(module, "DCompositionCreateDevice2");
    pDCompositionCreateDevice3 = (void *)GetProcAddress(module, "DCompositionCreateDevice3");

    if (!pDCompositionCreateDevice)
    {
        win_skip("DCompositionCreateDevice() is unavailable.\n");
        FreeLibrary(module);
        return;
    }

    test_DCompositionCreateDevice();
    test_DCompositionCreateDevice2();
    test_DCompositionCreateDevice3();
    test_shared_visual();
    test_device_CreateTargetForHwnd();
    test_device_CreateVisual();
    test_device_CreateSurfaceFactory();
    test_surface_factory_CreateSurface();
    test_surface_interface();
    test_surface_begin_end_Draw();
    test_surface_suspend_scroll_virtual();
    test_surface_composition();
    test_shared_visual_portal();
    test_shared_visual_portal_cases();
    test_shared_visual_portal_viewer_thread();
    test_target_SetRoot();
    test_visual_SetContent();
    test_visual_SetBitmapInterpolationMode();
    test_visual_SetBorderMode();
    test_visual_SetBackFaceVisibility();
    test_visual_SetOffsetX();
    test_visual_SetOffsetXAnimation();
    test_animation();
    test_clip_animation();
    test_effect_group_opacity();
    test_transforms();
    test_visual_SetTransformObject();
    test_visual_SetTransform_scale();
    test_visual_AddVisual();

    FreeLibrary(module);
}
