/*
 * uxtheme Double-buffered Drawing API
 *
 * Copyright (C) 2008 Reece H. Dunn
 * Copyright 2017 Nikolay Sivov for CodeWeavers
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

#include <stdlib.h>
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"
#include "vfwmsgs.h"
#include "uxtheme.h"
#include "uxthemedll.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uxtheme);

struct paintbuffer
{
    HDC targetdc;
    HDC memorydc;
    HBITMAP bitmap;
    RECT rect;
    void *bits;
    BP_BUFFERFORMAT format;
    BP_PAINTPARAMS params;
    BOOL has_params;
};

static void free_paintbuffer(struct paintbuffer *buffer)
{
    DeleteObject(buffer->bitmap);
    DeleteDC(buffer->memorydc);
    free(buffer);
}

static struct paintbuffer *get_buffer_obj(HPAINTBUFFER handle)
{
    return handle;
}

/***********************************************************************
 *      BufferedPaintInit                                  (UXTHEME.@)
 */
HRESULT WINAPI BufferedPaintInit(VOID)
{
    FIXME("Stub ()\n");
    return S_OK;
}

/***********************************************************************
 *      BufferedPaintUnInit                                (UXTHEME.@)
 */
HRESULT WINAPI BufferedPaintUnInit(VOID)
{
    FIXME("Stub ()\n");
    return S_OK;
}

/***********************************************************************
 *      BeginBufferedPaint                                 (UXTHEME.@)
 */
HPAINTBUFFER WINAPI BeginBufferedPaint(HDC targetdc, const RECT *rect,
        BP_BUFFERFORMAT format, BP_PAINTPARAMS *params, HDC *retdc)
{
    char bmibuf[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *bmi = (BITMAPINFO *)bmibuf;
    struct paintbuffer *buffer;

    TRACE("(%p %s %d %p %p)\n", targetdc, wine_dbgstr_rect(rect), format,
          params, retdc);

    if (retdc)
        *retdc = NULL;

    if (!targetdc || IsRectEmpty(rect))
        return NULL;

    buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        return NULL;

    buffer->targetdc = targetdc;
    buffer->rect = *rect;
    buffer->format = format;
    buffer->memorydc = CreateCompatibleDC(targetdc);
    if (params)
    {
        buffer->params = *params;
        buffer->has_params = TRUE;
    }

    switch (format)
    {
    case BPBF_COMPATIBLEBITMAP:
        buffer->bitmap = CreateCompatibleBitmap(targetdc, rect->right - rect->left, rect->bottom - rect->top);
        buffer->bits = NULL;
        break;
    case BPBF_DIB:
    case BPBF_TOPDOWNDIB:
    case BPBF_TOPDOWNMONODIB:
        /* create DIB section */
        memset(bmi, 0, sizeof(bmibuf));
        bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
        bmi->bmiHeader.biHeight = format == BPBF_DIB ? rect->bottom - rect->top :
                -(rect->bottom - rect->top);
        bmi->bmiHeader.biWidth = rect->right - rect->left;
        bmi->bmiHeader.biBitCount = format == BPBF_TOPDOWNMONODIB ? 1 : 32;
        bmi->bmiHeader.biPlanes = 1;
        bmi->bmiHeader.biCompression = BI_RGB;
        buffer->bitmap = CreateDIBSection(buffer->memorydc, bmi, DIB_RGB_COLORS, &buffer->bits, NULL, 0);
        break;
    default:
        WARN("Unknown buffer format %d\n", format);
        buffer->bitmap = NULL;
        free_paintbuffer(buffer);
        return NULL;
    }

    if (!buffer->bitmap)
    {
        WARN("Failed to create buffer bitmap\n");
        free_paintbuffer(buffer);
        return NULL;
    }

    SetWindowOrgEx(buffer->memorydc, rect->left, rect->top, NULL);
    if (!params || !(params->dwFlags & BPPF_NOCLIP))
    {
        HRGN hrgn = CreateRectRgn(0, 0, 0, 0);
        if (hrgn)
        {
            if (GetClipRgn(targetdc, hrgn) > 0 || GetRandomRgn(targetdc, hrgn, SYSRGN) > 0)
            {
                POINT org;
                GetWindowOrgEx(targetdc, &org);
                OffsetRgn(hrgn, rect->left - org.x, rect->top - org.y);
                ExtSelectClipRgn(buffer->memorydc, hrgn, RGN_AND);
            }
            DeleteObject(hrgn);
        }
    }
    IntersectClipRect(buffer->memorydc, rect->left, rect->top, rect->right, rect->bottom);
    if (params && params->prcExclude && !IsRectEmpty(params->prcExclude))
    {
        ExcludeClipRect(buffer->memorydc, params->prcExclude->left, params->prcExclude->top,
                        params->prcExclude->right, params->prcExclude->bottom);
    }
    DeleteObject(SelectObject(buffer->memorydc, buffer->bitmap));

    if (params && (params->dwFlags & BPPF_ERASE))
    {
        int w = rect->right - rect->left;
        int h = rect->bottom - rect->top;
        if (buffer->bits)
        {
            int bpp = (format == BPBF_TOPDOWNMONODIB) ? 1 : 32;
            int stride = ((w * bpp + 31) & ~31) / 8;
            memset(buffer->bits, 0, stride * h);
        }
        else
        {
            PatBlt(buffer->memorydc, rect->left, rect->top, w, h, BLACKNESS);
        }
    }

    *retdc = buffer->memorydc;

    return (HPAINTBUFFER)buffer;
}

/***********************************************************************
 *      EndBufferedPaint                                   (UXTHEME.@)
 */
HRESULT WINAPI EndBufferedPaint(HPAINTBUFFER bufferhandle, BOOL update)
{
    struct paintbuffer *buffer = get_buffer_obj(bufferhandle);

    TRACE("(%p %d)\n", bufferhandle, update);

    if (!buffer)
        return E_INVALIDARG;

    if (update)
    {
        int w = buffer->rect.right - buffer->rect.left;
        int h = buffer->rect.bottom - buffer->rect.top;
        HWND hwnd = WindowFromDC(buffer->targetdc);
        BOOL is_glass = hwnd && (GetPropW(hwnd, L"__wine_dwm_alpha") != NULL);
        HBITMAP target_bmp = GetCurrentObject(buffer->targetdc, OBJ_BITMAP);
        DIBSECTION ds;
        BOOL is_32bit_target = (GetObjectW(target_bmp, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBitsPixel == 32);
        BOOL use_alpha = FALSE;

        if (buffer->has_params && buffer->params.pBlendFunction)
            use_alpha = TRUE;
        else if (is_glass)
            use_alpha = TRUE;

        if (use_alpha)
        {
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            if (buffer->has_params && buffer->params.pBlendFunction)
                bf = *buffer->params.pBlendFunction;

            if (!GdiAlphaBlend(buffer->targetdc, buffer->rect.left, buffer->rect.top, w, h,
                               buffer->memorydc, buffer->rect.left, buffer->rect.top, w, h, bf))
            {
                WARN("GdiAlphaBlend() failed\n");
            }
        }
        else if (is_32bit_target && buffer->bits)
        {
            BitBlt(buffer->targetdc, buffer->rect.left, buffer->rect.top, w, h,
                   buffer->memorydc, buffer->rect.left, buffer->rect.top, SRCCOPY);
        }
        else
        {
            if (!BitBlt(buffer->targetdc, buffer->rect.left, buffer->rect.top, w, h,
                        buffer->memorydc, buffer->rect.left, buffer->rect.top, SRCCOPY))
            {
                WARN("BitBlt() failed\n");
            }
        }
    }

    free_paintbuffer(buffer);
    return S_OK;
}

/***********************************************************************
 *      BufferedPaintClear                                 (UXTHEME.@)
 */
HRESULT WINAPI BufferedPaintClear(HPAINTBUFFER hBufferedPaint, const RECT *prc)
{
    struct paintbuffer *buffer = get_buffer_obj(hBufferedPaint);
    int w, h, x0, x1, y0, y1, y;
    RGBQUAD *pixels;

    TRACE("(%p %p)\n", hBufferedPaint, prc);

    if (!buffer)
        return E_INVALIDARG;

    if (!buffer->bits)
        return S_OK;

    w = buffer->rect.right - buffer->rect.left;
    h = buffer->rect.bottom - buffer->rect.top;
    if (w <= 0 || h <= 0)
        return S_OK;

    pixels = (RGBQUAD *)buffer->bits;

    if (!prc)
    {
        memset(buffer->bits, 0, w * h * sizeof(RGBQUAD));
        return S_OK;
    }

    if (prc->left >= buffer->rect.left && prc->top >= buffer->rect.top)
    {
        x0 = max(0, prc->left - buffer->rect.left);
        x1 = min(w, prc->right - buffer->rect.left);
        y0 = max(0, prc->top - buffer->rect.top);
        y1 = min(h, prc->bottom - buffer->rect.top);
    }
    else
    {
        x0 = max(0, prc->left);
        x1 = min(w, prc->right);
        y0 = max(0, prc->top);
        y1 = min(h, prc->bottom);
    }

    if (x1 <= x0 || y1 <= y0)
        return S_OK;

    for (y = y0; y < y1; y++)
    {
        int row = (buffer->format == BPBF_DIB) ? (h - 1 - y) : y;
        memset(&pixels[row * w + x0], 0, (x1 - x0) * sizeof(RGBQUAD));
    }

    return S_OK;
}

/***********************************************************************
 *      BufferedPaintSetAlpha                              (UXTHEME.@)
 */
HRESULT WINAPI BufferedPaintSetAlpha(HPAINTBUFFER hBufferedPaint, const RECT *prc, BYTE alpha)
{
    struct paintbuffer *buffer = get_buffer_obj(hBufferedPaint);
    int w, h, x0, x1, y0, y1, x, y;
    RGBQUAD *pixels;

    TRACE("(%p %p %u)\n", hBufferedPaint, prc, alpha);

    if (!buffer)
        return E_INVALIDARG;

    if (!buffer->bits)
        return E_FAIL;

    w = buffer->rect.right - buffer->rect.left;
    h = buffer->rect.bottom - buffer->rect.top;
    if (w <= 0 || h <= 0)
        return S_OK;

    pixels = (RGBQUAD *)buffer->bits;

    if (!prc)
    {
        for (x = 0; x < w * h; x++)
            pixels[x].rgbReserved = alpha;
        return S_OK;
    }

    if (prc->left >= buffer->rect.left && prc->top >= buffer->rect.top)
    {
        x0 = max(0, prc->left - buffer->rect.left);
        x1 = min(w, prc->right - buffer->rect.left);
        y0 = max(0, prc->top - buffer->rect.top);
        y1 = min(h, prc->bottom - buffer->rect.top);
    }
    else
    {
        x0 = max(0, prc->left);
        x1 = min(w, prc->right);
        y0 = max(0, prc->top);
        y1 = min(h, prc->bottom);
    }

    if (x1 <= x0 || y1 <= y0)
        return S_OK;

    for (y = y0; y < y1; y++)
    {
        int row = (buffer->format == BPBF_DIB) ? (h - 1 - y) : y;
        for (x = x0; x < x1; x++)
            pixels[row * w + x].rgbReserved = alpha;
    }

    return S_OK;
}

/***********************************************************************
 *      GetBufferedPaintBits                               (UXTHEME.@)
 */
HRESULT WINAPI GetBufferedPaintBits(HPAINTBUFFER bufferhandle, RGBQUAD **bits, int *width)
{
    struct paintbuffer *buffer = get_buffer_obj(bufferhandle);

    TRACE("(%p %p %p)\n", buffer, bits, width);

    if (!bits || !width)
        return E_POINTER;

    if (!buffer || !buffer->bits)
        return E_FAIL;

    *bits = buffer->bits;
    *width = buffer->rect.right - buffer->rect.left;

    return S_OK;
}

/***********************************************************************
 *      GetBufferedPaintDC                                 (UXTHEME.@)
 */
HDC WINAPI GetBufferedPaintDC(HPAINTBUFFER bufferhandle)
{
    struct paintbuffer *buffer = get_buffer_obj(bufferhandle);

    TRACE("(%p)\n", buffer);

    return buffer ? buffer->memorydc : NULL;
}

/***********************************************************************
 *      GetBufferedPaintTargetDC                           (UXTHEME.@)
 */
HDC WINAPI GetBufferedPaintTargetDC(HPAINTBUFFER bufferhandle)
{
    struct paintbuffer *buffer = get_buffer_obj(bufferhandle);

    TRACE("(%p)\n", buffer);

    return buffer ? buffer->targetdc : NULL;
}

/***********************************************************************
 *      GetBufferedPaintTargetRect                         (UXTHEME.@)
 */
HRESULT WINAPI GetBufferedPaintTargetRect(HPAINTBUFFER bufferhandle, RECT *rect)
{
    struct paintbuffer *buffer = get_buffer_obj(bufferhandle);

    TRACE("(%p %p)\n", buffer, rect);

    if (!rect)
        return E_POINTER;

    if (!buffer)
        return E_FAIL;

    *rect = buffer->rect;
    return S_OK;
}

/***********************************************************************
 *      BeginBufferedAnimation                             (UXTHEME.@)
 */
HANIMATIONBUFFER WINAPI BeginBufferedAnimation(HWND hwnd, HDC hdcTarget, const RECT *rcTarget,
                                               BP_BUFFERFORMAT dwFormat, BP_PAINTPARAMS *pPaintParams,
                                               BP_ANIMATIONPARAMS *pAnimationParams, HDC *phdcFrom,
                                               HDC *phdcTo)
{
    FIXME("Stub (%p %p %p %u %p %p %p %p)\n", hwnd, hdcTarget, rcTarget, dwFormat,
          pPaintParams, pAnimationParams, phdcFrom, phdcTo);

    return NULL;
}

/***********************************************************************
 *      BufferedPaintRenderAnimation                       (UXTHEME.@)
 */
BOOL WINAPI BufferedPaintRenderAnimation(HWND hwnd, HDC hdcTarget)
{
    FIXME("Stub (%p %p)\n", hwnd, hdcTarget);

    return FALSE;
}

/***********************************************************************
 *      BufferedPaintStopAllAnimations                     (UXTHEME.@)
 */
HRESULT WINAPI BufferedPaintStopAllAnimations(HWND hwnd)
{
    FIXME("Stub (%p)\n", hwnd);

    return E_NOTIMPL;
}

/***********************************************************************
 *      EndBufferedAnimation                               (UXTHEME.@)
 */
HRESULT WINAPI EndBufferedAnimation(HANIMATIONBUFFER hbpAnimation, BOOL fUpdateTarget)
{
    FIXME("Stub (%p %u)\n", hbpAnimation, fUpdateTarget);

    return E_NOTIMPL;
}
