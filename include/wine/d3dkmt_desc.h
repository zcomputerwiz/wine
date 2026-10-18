/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#ifndef __WINE_D3DKMT_DESC_H
#define __WINE_D3DKMT_DESC_H

#include <dxgi.h>
#include <d3d11.h>
#include <ddk/d3dkmthk.h>

/* {64b8bb26-4b20-4a99-b1d5-c9c0fb53c9f1} */
DEFINE_GUID(IID_WineDXGISharedData,
    0x64b8bb26, 0x4b20, 0x4a99, 0xb1, 0xd5, 0xc9, 0xc0, 0xfb, 0x53, 0xc9, 0xf1);

struct wine_dxgi_shared_data
{
    D3DKMT_HANDLE kmt_resource;
    D3DKMT_HANDLE kmt_keyed_mutex;
    HANDLE nt_handle;
    HANDLE shared_mapping;
    void *shared_memory;
    DWORD shared_memory_size;
    DWORD shared_pitch;
};

struct d3dkmt_dxgi_desc
{
    UINT                        size;
    UINT                        version;
    UINT                        width;
    UINT                        height;
    DXGI_FORMAT                 format;
    UINT                        unknown_0;
    UINT                        unknown_1;
    UINT                        keyed_mutex;
    D3DKMT_HANDLE               mutex_handle;
    D3DKMT_HANDLE               sync_handle;
    UINT                        nt_shared;
    UINT                        unknown_2;
    UINT                        unknown_3;
    UINT                        unknown_4;
};

struct d3dkmt_d3d11_desc
{
    struct d3dkmt_dxgi_desc     dxgi;
    D3D11_RESOURCE_DIMENSION    dimension;
    union
    {
        D3D10_BUFFER_DESC       d3d10_buf;
        D3D10_TEXTURE1D_DESC    d3d10_1d;
        D3D10_TEXTURE2D_DESC    d3d10_2d;
        D3D10_TEXTURE3D_DESC    d3d10_3d;
        D3D11_BUFFER_DESC       d3d11_buf;
        D3D11_TEXTURE1D_DESC    d3d11_1d;
        D3D11_TEXTURE2D_DESC    d3d11_2d;
        D3D11_TEXTURE3D_DESC    d3d11_3d;
    };
};

#endif /* __WINE_D3DKMT_DESC_H */
