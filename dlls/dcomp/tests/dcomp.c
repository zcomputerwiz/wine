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
#include <d3d10_1.h>
#include <dxgi.h>
#include "dcomp.h"
#include "wine/test.h"

static HRESULT (WINAPI *pDCompositionCreateDevice)(IDXGIDevice *dxgi_device, REFIID iid, void **device);
static HRESULT (WINAPI *pDCompositionCreateDevice2)(IUnknown *rendering_device, REFIID iid, void **device);
static HRESULT (WINAPI *pDCompositionCreateDevice3)(IUnknown *rendering_device, REFIID iid, void **device);

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
    test_device_CreateTargetForHwnd();
    test_device_CreateVisual();
    test_target_SetRoot();
    test_visual_SetContent();
    test_visual_SetBitmapInterpolationMode();
    test_visual_SetBorderMode();
    test_visual_SetBackFaceVisibility();
    test_visual_SetOffsetX();
    test_visual_AddVisual();

    FreeLibrary(module);
}
