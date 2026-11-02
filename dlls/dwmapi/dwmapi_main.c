/*
 * Dwmapi
 *
 * Copyright 2007 Andras Kovacs
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

#include <stdarg.h>
#include <stdio.h>

#include "winternl.h"
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winuser.h"
#include "dwmapi.h"
#include "wine/debug.h"
#include "wine/dwm_protocol.h"
#include "dwm_client.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwmapi);

#ifndef DWMWA_MICA_EFFECT
#define DWMWA_MICA_EFFECT 1029
#endif

#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

#define THUMBNAIL_TABLE_SIZE 64
#define ATTRIBUTE_TABLE_SIZE 256

typedef struct
{
    HTHUMBNAIL handle;
    HWND dest;
    HWND src;
    DWM_THUMBNAIL_PROPERTIES props;
} THUMBNAIL_ENTRY;

typedef struct
{
    HWND hwnd;
    DWORD attribute;
    DWORD value;
} ATTRIBUTE_ENTRY;

static THUMBNAIL_ENTRY thumbnail_table[THUMBNAIL_TABLE_SIZE];
/* A future DWM service will own this state; until then it is kept in a process-local table. */
static ATTRIBUTE_ENTRY attribute_table[ATTRIBUTE_TABLE_SIZE];
static DWORD thumbnail_next_handle;

static CRITICAL_SECTION thumbnail_cs;
static CRITICAL_SECTION_DEBUG thumbnail_cs_debug = {
    0, 0, &thumbnail_cs,
    { &thumbnail_cs_debug.ProcessLocksList,
      &thumbnail_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)"dwmapi thumbnail_cs" }
};
static CRITICAL_SECTION thumbnail_cs = { &thumbnail_cs_debug, -1, 0, 0, 0, 0 };

static BOOL get_stored_attribute(HWND hwnd, DWORD attribute, DWORD *value)
{
    DWORD stored;
    int i;
    BOOL found = FALSE;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < ATTRIBUTE_TABLE_SIZE; i++)
    {
        if (attribute_table[i].hwnd == hwnd && attribute_table[i].attribute == attribute)
        {
            stored = attribute_table[i].value;
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (found)
        *value = stored;
    return found;
}

/* The broker, when there is one, holds the canonical value, so a setter in another process is seen. The local
 * table is a mirror: it answers when there is no broker, and it refills a broker that lost its state. */
static BOOL get_attribute_value(HWND hwnd, DWORD attribute, DWORD *value)
{
    BOOL found = FALSE;
    DWORD brokered;

    if (!dwm_client_get_attr(hwnd, attribute, &found, &brokered))
    {
        if (found)
        {
            *value = brokered;
            return TRUE;
        }
        if (get_stored_attribute(hwnd, attribute, value))
        {
            dwm_client_set_attr(hwnd, attribute, *value);
            return TRUE;
        }
        return FALSE;
    }
    return get_stored_attribute(hwnd, attribute, value);
}


/**********************************************************************
 *           DwmIsCompositionEnabled         (DWMAPI.@)
 */
HRESULT WINAPI DwmIsCompositionEnabled(BOOL *enabled)
{
    RTL_OSVERSIONINFOEXW version;

    TRACE("%p\n", enabled);

    if (!enabled)
        return E_INVALIDARG;

    *enabled = FALSE;
    version.dwOSVersionInfoSize = sizeof(version);
    if (!RtlGetVersion(&version))
        *enabled = (version.dwMajorVersion > 6 || (version.dwMajorVersion == 6 && version.dwMinorVersion >= 3));

    return S_OK;
}

/**********************************************************************
 *           DwmEnableComposition         (DWMAPI.102)
 */
HRESULT WINAPI DwmEnableComposition(UINT uCompositionAction)
{
    FIXME("(%d) stub\n", uCompositionAction);

    return S_OK;
}

/**********************************************************************
 *           DwmExtendFrameIntoClientArea    (DWMAPI.@)
 */
HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND hwnd, const MARGINS* margins)
{
    FIXME("(%p, %p) stub\n", hwnd, margins);

    return S_OK;
}

/**********************************************************************
 *           DwmGetColorizationColor      (DWMAPI.@)
 */
HRESULT WINAPI DwmGetColorizationColor(DWORD *colorization, BOOL *opaque_blend)
{
    DWORD type, value, cbData;

    TRACE("(%p, %p)\n", colorization, opaque_blend);

    if (!colorization || !opaque_blend)
        return E_INVALIDARG;

    *colorization = 0xC40078D7;
    *opaque_blend = TRUE;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                    L"ColorizationColor", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        *colorization = value;

    cbData = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                    L"ColorizationOpaqueBlend", RRF_RT_REG_DWORD, &type, &value, &cbData) == ERROR_SUCCESS)
        *opaque_blend = (value != 0);

    return S_OK;
}

/**********************************************************************
 *        DwmInvalidateIconicBitmaps      (DWMAPI.@)
 */
HRESULT WINAPI DwmInvalidateIconicBitmaps(HWND hwnd)
{
    TRACE("(%p)\n", hwnd);

    if (!IsWindow(hwnd))
        return E_HANDLE;

    return S_OK;
}

/**********************************************************************
 *           DwmSetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD attributenum, LPCVOID attribute, DWORD size)
{
    DWORD value;
    int i;

    TRACE("(%p, %lx, %p, %lx)\n", hwnd, attributenum, attribute, size);

    if (!IsWindow(hwnd))
        return E_HANDLE;
    if (!attribute)
        return E_INVALIDARG;
    if (size != 4)
        return E_INVALIDARG;
    if (attributenum == 0 || (attributenum >= DWMWA_LAST && attributenum != DWMWA_MICA_EFFECT))
        return E_INVALIDARG;

    value = *(const DWORD *)attribute;

    dwm_client_set_attr(hwnd, attributenum, value);  /* best effort: the local table is the mirror */

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < ATTRIBUTE_TABLE_SIZE; i++)
    {
        if (attribute_table[i].hwnd == hwnd && attribute_table[i].attribute == attributenum)
        {
            attribute_table[i].value = value;
            LeaveCriticalSection(&thumbnail_cs);
            return S_OK;
        }
    }
    /* no existing entry; take a free slot, reclaiming dead windows first */
    for (i = 0; i < ATTRIBUTE_TABLE_SIZE; i++)
    {
        if (!attribute_table[i].hwnd || !IsWindow(attribute_table[i].hwnd))
        {
            attribute_table[i].hwnd = hwnd;
            attribute_table[i].attribute = attributenum;
            attribute_table[i].value = value;
            LeaveCriticalSection(&thumbnail_cs);
            return S_OK;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    return E_OUTOFMEMORY;
}

/**********************************************************************
 *           DwmGetGraphicsStreamClient         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamClient(UINT uIndex, UUID *pClientUuid)
{
    FIXME("(%d, %p) stub\n", uIndex, pClientUuid);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmGetTransportAttributes         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetTransportAttributes(BOOL *pfIsRemoting, BOOL *pfIsConnected, DWORD *pDwGeneration)
{
    FIXME("(%p, %p, %p) stub\n", pfIsRemoting, pfIsConnected, pDwGeneration);

    return DWM_E_COMPOSITIONDISABLED;
}

/**********************************************************************
 *           DwmUnregisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmUnregisterThumbnail(HTHUMBNAIL thumbnail)
{
    THUMBNAIL_ENTRY entry;
    BOOL found = FALSE;
    int i;

    TRACE("(%p)\n", thumbnail);

    if (!thumbnail)
        return E_INVALIDARG;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (thumbnail_table[i].handle == thumbnail)
        {
            entry = thumbnail_table[i];
            memset(&thumbnail_table[i], 0, sizeof(thumbnail_table[i]));
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (!found)
        return E_INVALIDARG;

    /* tell the broker to drop the thumbnail (best effort; fails quietly without one) */
    dwm_client_thumb_del(entry.dest, (UINT)(ULONG_PTR)entry.handle);
    return S_OK;
}

/**********************************************************************
 *           DwmEnableMMCSS         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableMMCSS(BOOL enableMMCSS)
{
    FIXME("(%d) stub\n", enableMMCSS);

    return S_OK;
}

/**********************************************************************
 *           DwmGetGraphicsStreamTransformHint         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamTransformHint(UINT uIndex, MilMatrix3x2D *pTransform)
{
    TRACE("(%d, %p)\n", uIndex, pTransform);

    return DWM_E_COMPOSITIONDISABLED;
}

/**********************************************************************
 *           DwmEnableBlurBehindWindow         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableBlurBehindWindow(HWND hWnd, const DWM_BLURBEHIND *pBlurBuf)
{
    TRACE("%p %p\n", hWnd, pBlurBuf);

    if (!IsWindow(hWnd))
        return E_HANDLE;
    if (!pBlurBuf)
        return E_INVALIDARG;

    return S_OK;
}

/**********************************************************************
 *           DwmDefWindowProc         (DWMAPI.@)
 */
BOOL WINAPI DwmDefWindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, LRESULT *plResult)
{
    static int i;

    if (!i++) FIXME("stub\n");

    return FALSE;
}

/**********************************************************************
 *           DwmGetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd, DWORD attribute, PVOID pv_attribute, DWORD size)
{
    HRESULT hr;

    TRACE("(%p %ld %p %ld)\n", hwnd, attribute, pv_attribute, size);

    if (!IsWindow(hwnd))
        return E_HANDLE;

    switch (attribute) {
    case DWMWA_EXTENDED_FRAME_BOUNDS:
    {
        RECT *rect = (RECT *)pv_attribute;
        DPI_AWARENESS_CONTEXT context;

        if (!rect)
            return E_INVALIDARG;
        if (size < sizeof(*rect))
            return E_NOT_SUFFICIENT_BUFFER;
        if (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)
            return E_HANDLE;

        /* DWM frame bounds are always in physical coords */
        context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        if (GetWindowRect(hwnd, rect))
            hr = S_OK;
        else
            hr = HRESULT_FROM_WIN32(GetLastError());

        SetThreadDpiAwarenessContext(context);
        break;
    }
    case DWMWA_NCRENDERING_ENABLED:
    {
        BOOL *value = (BOOL *)pv_attribute;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        *value = TRUE;
        hr = S_OK;
        break;
    }
    case DWMWA_CLOAKED:
    {
        DWORD *value = (DWORD *)pv_attribute;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        *value = 0;
        hr = S_OK;
        break;
    }
    case DWMWA_HAS_ICONIC_BITMAP:
    {
        BOOL *value = (BOOL *)pv_attribute;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        *value = FALSE;
        hr = S_OK;
        break;
    }
    case DWMWA_CAPTION_BUTTON_BOUNDS:
    {
        RECT *rect = (RECT *)pv_attribute;
        RECT window_rect;
        int width;

        if (!rect)
            return E_INVALIDARG;
        if (size < sizeof(*rect))
            return E_INVALIDARG;
        if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION))
        {
            rect->left = rect->top = rect->right = rect->bottom = 0;
            hr = S_OK;
            break;
        }
        if (!GetWindowRect(hwnd, &window_rect))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        width = window_rect.right - window_rect.left;
        rect->top = 0;
        rect->bottom = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
        rect->right = width;
        rect->left = width - 3 * GetSystemMetrics(SM_CXSIZE);
        if (rect->left < 0)
            rect->left = 0;
        hr = S_OK;
        break;
    }
    case DWMWA_NCRENDERING_POLICY:
    case DWMWA_ALLOW_NCPAINT:
    case DWMWA_FORCE_ICONIC_REPRESENTATION:
    case DWMWA_FLIP3D_POLICY:
    case DWMWA_DISALLOW_PEEK:
    case DWMWA_EXCLUDED_FROM_PEEK:
    case DWMWA_FREEZE_REPRESENTATION:
    case DWMWA_PASSIVE_UPDATE_MODE:
        hr = E_INVALIDARG;
        break;
    case DWMWA_USE_IMMERSIVE_DARK_MODE:
    case DWMWA_WINDOW_CORNER_PREFERENCE:
    case DWMWA_BORDER_COLOR:
    case DWMWA_CAPTION_COLOR:
    case DWMWA_TEXT_COLOR:
    case DWMWA_SYSTEMBACKDROP_TYPE:
    case DWMWA_USE_HOSTBACKDROPBRUSH:
    case DWMWA_MICA_EFFECT:
    {
        DWORD *value = (DWORD *)pv_attribute;
        DWORD default_value;

        if (!value)
            return E_INVALIDARG;
        if (size < sizeof(*value))
            return E_INVALIDARG;
        if (attribute == DWMWA_BORDER_COLOR || attribute == DWMWA_CAPTION_COLOR ||
            attribute == DWMWA_TEXT_COLOR)
            default_value = DWMWA_COLOR_DEFAULT;
        else
            default_value = 0;
        if (!get_attribute_value(hwnd, attribute, value))
            *value = default_value;
        hr = S_OK;
        break;
    }
    default:
        FIXME("attribute %ld not implemented.\n", attribute);
        hr = E_INVALIDARG;
        break;
    }

    return hr;
}

/**********************************************************************
 *           DwmRegisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmRegisterThumbnail(HWND dest, HWND src, PHTHUMBNAIL thumbnail_id)
{
    int i, j;
    HTHUMBNAIL handle;

    TRACE("(%p %p %p)\n", dest, src, thumbnail_id);

    if (!thumbnail_id)
        return E_INVALIDARG;
    if (!dest)
        return E_INVALIDARG;
    if (!src)
        return E_INVALIDARG;
    if (!IsWindow(dest) || !IsWindow(src))
        return E_HANDLE;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (!thumbnail_table[i].handle)
            break;
    }
    if (i == THUMBNAIL_TABLE_SIZE)
    {
        /* table full; reclaim a slot whose dest or src is no longer a window */
        for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
        {
            if (!IsWindow(thumbnail_table[i].dest) || !IsWindow(thumbnail_table[i].src))
            {
                memset(&thumbnail_table[i], 0, sizeof(thumbnail_table[i]));
                break;
            }
        }
        if (i == THUMBNAIL_TABLE_SIZE)
        {
            LeaveCriticalSection(&thumbnail_cs);
            return E_OUTOFMEMORY;
        }
    }

    /* allocate a handle from the counter, skipping 0 and any value currently in the table */
    do
    {
        if (++thumbnail_next_handle == 0)
            thumbnail_next_handle = 1;
        handle = (HTHUMBNAIL)(ULONG_PTR)thumbnail_next_handle;
        for (j = 0; j < THUMBNAIL_TABLE_SIZE; j++)
        {
            if (thumbnail_table[j].handle == handle)
                break;
        }
    } while (j != THUMBNAIL_TABLE_SIZE);

    thumbnail_table[i].handle = handle;
    thumbnail_table[i].dest = dest;
    thumbnail_table[i].src = src;
    memset(&thumbnail_table[i].props, 0, sizeof(thumbnail_table[i].props));
    thumbnail_table[i].props.opacity = 255;  /* fully opaque until DWM_TNP_OPACITY says otherwise */
    LeaveCriticalSection(&thumbnail_cs);

    *thumbnail_id = handle;
    return S_OK;
}

static int get_display_frequency(void)
{
    DEVMODEW mode;
    BOOL ret;

    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    ret = EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &mode, 0);
    if (ret && (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency > 1)
    {
        return mode.dmDisplayFrequency;
    }
    else
    {
        WARN("Failed to query display frequency, returning a fallback value.\n");
        return 60;
    }
}

/* Frame clock (docs/DWM_ARCHITECTURE.md section 2.3, P6 slice 1): the compositor publishes one record per
 * composed frame into a per-desktop shared page and pulses an event. We open both read-only, once per
 * process, and read a consistent snapshot with the sequence lock. Never wait while holding any lock. */
static HANDLE clock_mapping, clock_event;
static struct dwm_clock_page *clock_page;
static BOOL clock_opened;
static INT64 clock_last_failed;   /* QPC time of the last failed open */

static BOOL clock_open(void)
{
    WCHAR desk_name[MAX_PATH], name[MAX_PATH + 64], event_name[MAX_PATH + 64];
    HDESK desk;
    LARGE_INTEGER li;
    INT64 now;

    if (clock_opened) return clock_page != NULL;

    /* retry the open at most once per second when it failed */
    NtQueryPerformanceCounter(&li, NULL);
    now = li.QuadPart;
    if (clock_last_failed && now - clock_last_failed < 10000000) return FALSE;

    if (!(desk = GetThreadDesktop(GetCurrentThreadId()))) return FALSE;
    if (!GetUserObjectInformationW(desk, UOI_NAME, desk_name, ARRAY_SIZE(desk_name), NULL)) return FALSE;
    lstrcpynW(name, DWM_CLOCK_NAME, ARRAY_SIZE(name));
    lstrcatW(name, desk_name);
    lstrcpynW(event_name, DWM_CLOCK_EVENT_NAME, ARRAY_SIZE(event_name));
    lstrcatW(event_name, desk_name);

    clock_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!clock_mapping)
    {
        clock_last_failed = now;
        return FALSE;
    }
    clock_event = OpenEventW(SYNCHRONIZE, FALSE, event_name);
    clock_page = MapViewOfFile(clock_mapping, FILE_MAP_READ, 0, 0, sizeof(*clock_page));
    clock_opened = TRUE;
    return clock_page != NULL;
}

/* read a consistent snapshot with the sequence lock: retry while seq is odd or changed */
static BOOL clock_read(struct dwm_clock_page *out)
{
    LONG seq1, seq2;
    int tries;

    if (!clock_open()) return FALSE;
    /* bounded: a writer that died in the middle of an update must not hang the reader */
    for (tries = 0; tries < 1000; tries++)
    {
        seq1 = __atomic_load_n(&clock_page->seq, __ATOMIC_ACQUIRE);
        if (seq1 & 1) continue;
        out->flags = clock_page->flags;
        out->frame = clock_page->frame;
        out->qpc_composed = clock_page->qpc_composed;
        out->qpc_period = clock_page->qpc_period;
        seq2 = __atomic_load_n(&clock_page->seq, __ATOMIC_ACQUIRE);
        if (seq1 == seq2) return TRUE;
    }
    return FALSE;
}

/**********************************************************************
 *           DwmGetCompositionTimingInfo         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetCompositionTimingInfo(HWND hwnd, DWM_TIMING_INFO *info)
{
    struct dwm_clock_page snapshot;
    LARGE_INTEGER performance_frequency, qpc;
    static int i;
    int display_frequency;
    QPC_TIME qpc_refresh_period;
    ULONGLONG numerator, denominator;

    if (!info)
        return E_INVALIDARG;

    if (info->cbSize != sizeof(DWM_TIMING_INFO))
        return MILERR_MISMATCHED_SIZE;

    if(!i++) FIXME("(%p %p)\n", hwnd, info);

    memset(info, 0, info->cbSize);
    info->cbSize = sizeof(DWM_TIMING_INFO);

    display_frequency = get_display_frequency();

    QueryPerformanceFrequency(&performance_frequency);
    qpc_refresh_period = performance_frequency.QuadPart / display_frequency;
    if (!qpc_refresh_period)
        qpc_refresh_period = 1;
    info->qpcRefreshPeriod = qpc_refresh_period;

    /* like Windows 10 and later: the rates are the counter frequency over the refresh period in counter ticks */
    numerator = (ULONGLONG)performance_frequency.QuadPart;
    denominator = qpc_refresh_period;
    while ((numerator > 0xFFFFFFFFULL) || (denominator > 0xFFFFFFFFULL))
    {
        numerator >>= 1;
        denominator >>= 1;
    }
    info->rateRefresh.uiNumerator = (UINT32)numerator;
    info->rateRefresh.uiDenominator = (UINT32)denominator;
    info->rateCompose = info->rateRefresh;

    QueryPerformanceCounter(&qpc);
    info->qpcVBlank = (qpc.QuadPart / info->qpcRefreshPeriod) * info->qpcRefreshPeriod;

    /* when the compositor publishes a frame clock, use its composed frame and refresh period */
    if (clock_read(&snapshot) && (snapshot.flags & DWM_CLOCK_COMPOSED))
    {
        info->cFrame = snapshot.frame;
        info->cRefresh = snapshot.frame;
        info->qpcCompose = snapshot.qpc_composed;
        info->qpcRefreshPeriod = snapshot.qpc_period;
        numerator = (ULONGLONG)performance_frequency.QuadPart;
        denominator = (ULONGLONG)snapshot.qpc_period;
        while ((numerator > 0xFFFFFFFFULL) || (denominator > 0xFFFFFFFFULL))
        {
            numerator >>= 1;
            denominator >>= 1;
        }
        info->rateRefresh.uiNumerator = (UINT32)numerator;
        info->rateRefresh.uiDenominator = (UINT32)denominator;
        info->rateCompose = info->rateRefresh;
    }

    return S_OK;
}

/**********************************************************************
 *                  DwmFlush              (DWMAPI.@)
 */
HRESULT WINAPI DwmFlush(void)
{
    struct dwm_clock_page snapshot;
    LARGE_INTEGER qpf, qpc, delay;
    LONG64 qpc_refresh_period;
    int display_frequency;
    static BOOL once;

    if (!once++)
        FIXME("stub.\n");
    else
        TRACE("stub.\n");

    /* when the compositor publishes a frame clock, wait for a new composed frame (or two refresh periods) */
    if (clock_read(&snapshot) && (snapshot.flags & DWM_CLOCK_COMPOSED))
    {
        UINT64 frame = snapshot.frame;
        DWORD period_ms = (DWORD)(snapshot.qpc_period / 10000);
        int i;

        if (period_ms < 1) period_ms = 1;
        for (i = 0; i < 2; i++)
        {
            WaitForSingleObject(clock_event, period_ms);
            if (clock_read(&snapshot) && snapshot.frame != frame)
                return S_OK;
        }
        /* nothing changed on screen: the compositor idles */
        return S_OK;
    }

    display_frequency = get_display_frequency();
    NtQueryPerformanceCounter(&qpc, &qpf);
    qpc_refresh_period = qpf.QuadPart / display_frequency;
    if (!qpc_refresh_period)
        qpc_refresh_period = 1;
    delay.QuadPart = (qpc.QuadPart - ((qpc.QuadPart + qpc_refresh_period - 1) / qpc_refresh_period) * qpc_refresh_period)
            * 10000000 / qpf.QuadPart;
    NtDelayExecution(FALSE, &delay);

    return S_OK;
}

/**********************************************************************
 *           DwmAttachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmAttachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmDetachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmDetachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmUpdateThumbnailProperties         (DWMAPI.@)
 */
HRESULT WINAPI DwmUpdateThumbnailProperties(HTHUMBNAIL thumbnail, const DWM_THUMBNAIL_PROPERTIES *props)
{
    DWM_THUMBNAIL_PROPERTIES local_props;
    THUMBNAIL_ENTRY entry;
    BOOL found = FALSE;
    int i;

    TRACE("(%p, %p)\n", thumbnail, props);

    if (!thumbnail)
        return E_INVALIDARG;
    if (!props)
        return E_INVALIDARG;

    /* copy *props into a local first, validate, then lock */
    local_props = *props;
    if (local_props.dwFlags & ~(DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY |
                                DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY))
        return E_INVALIDARG;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (thumbnail_table[i].handle == thumbnail)
        {
            if (!IsWindow(thumbnail_table[i].dest) || !IsWindow(thumbnail_table[i].src))
            {
                LeaveCriticalSection(&thumbnail_cs);
                return E_HANDLE;
            }
            if (local_props.dwFlags & DWM_TNP_RECTDESTINATION)
                thumbnail_table[i].props.rcDestination = local_props.rcDestination;
            if (local_props.dwFlags & DWM_TNP_RECTSOURCE)
                thumbnail_table[i].props.rcSource = local_props.rcSource;
            if (local_props.dwFlags & DWM_TNP_OPACITY)
                thumbnail_table[i].props.opacity = local_props.opacity;
            if (local_props.dwFlags & DWM_TNP_VISIBLE)
                thumbnail_table[i].props.fVisible = local_props.fVisible;
            if (local_props.dwFlags & DWM_TNP_SOURCECLIENTAREAONLY)
                thumbnail_table[i].props.fSourceClientAreaOnly = local_props.fSourceClientAreaOnly;
            /* copy the merged state out before leaving the lock; the broker is called unlocked */
            entry = thumbnail_table[i];
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (!found)
        return E_INVALIDARG;

    /* publish the merged state to the broker (best effort; fails quietly without one) */
    {
        struct dwm_thumb_set set;
        DWORD flags = 0;

        memset(&set, 0, sizeof(set));
        if (entry.props.fVisible) flags |= DWM_THUMB_VISIBLE;
        if (entry.props.fSourceClientAreaOnly) flags |= DWM_THUMB_CLIENT_ONLY;
        if (entry.props.rcSource.left || entry.props.rcSource.top ||
            entry.props.rcSource.right || entry.props.rcSource.bottom)
            flags |= DWM_THUMB_HAS_SOURCE;
        set.flags = flags;
        set.opacity = entry.props.opacity;
        set.dest[0] = entry.props.rcDestination.left;
        set.dest[1] = entry.props.rcDestination.top;
        set.dest[2] = entry.props.rcDestination.right;
        set.dest[3] = entry.props.rcDestination.bottom;
        set.source[0] = entry.props.rcSource.left;
        set.source[1] = entry.props.rcSource.top;
        set.source[2] = entry.props.rcSource.right;
        set.source[3] = entry.props.rcSource.bottom;
        dwm_client_thumb_set(entry.dest, (UINT)(ULONG_PTR)entry.handle, entry.src, &set);
    }
    return S_OK;
}

/**********************************************************************
 *           DwmSetPresentParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetPresentParameters(HWND hwnd, DWM_PRESENT_PARAMETERS *params)
{
    FIXME("(%p %p) stub\n", hwnd, params);
    return S_OK;
};

/**********************************************************************
 *           DwmSetIconicLivePreviewBitmap         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicLivePreviewBitmap(HWND hwnd, HBITMAP hbmp, POINT *pos, DWORD flags)
{
    DWORD value;

    TRACE("(%p %p %p %lx)\n", hwnd, hbmp, pos, flags);

    if (!IsWindow(hwnd))
        return E_HANDLE;
    if (!get_stored_attribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &value) || !value)
        return E_INVALIDARG;
    if (!hbmp)
        return E_INVALIDARG;
    if (GetObjectType(hbmp) != OBJ_BITMAP)
        return E_INVALIDARG;
    if (flags & ~DWM_SIT_DISPLAYFRAME)
        return E_INVALIDARG;

    return S_OK;
}

/**********************************************************************
 *           DwmSetIconicThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicThumbnail(HWND hwnd, HBITMAP hbmp, DWORD flags)
{
    DWORD value;

    TRACE("(%p %p %lx)\n", hwnd, hbmp, flags);

    if (!IsWindow(hwnd))
        return E_HANDLE;
    if (!get_stored_attribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &value) || !value)
        return E_INVALIDARG;
    if (!hbmp)
        return E_INVALIDARG;
    if (GetObjectType(hbmp) != OBJ_BITMAP)
        return E_INVALIDARG;
    if (flags & ~DWM_SIT_DISPLAYFRAME)
        return E_INVALIDARG;

    return S_OK;
}

/**********************************************************************
 *           DwmpGetColorizationParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmpGetColorizationParameters(void *params)
{
    FIXME("(%p) stub\n", params);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmShowContact         (DWMAPI.@)
 */
HRESULT WINAPI DwmShowContact(DWORD pointer_id, enum DWM_SHOWCONTACT showcontact)
{
    FIXME("pointer_id %#lx, showcontact %#x stub\n", pointer_id, showcontact);
    return S_OK;
}

/**********************************************************************
 *           DwmQueryThumbnailSourceSize         (DWMAPI.@)
 */
HRESULT WINAPI DwmQueryThumbnailSourceSize(HTHUMBNAIL thumbnail, SIZE *size)
{
    HWND src = NULL;
    RECT rect;
    int i;
    BOOL found = FALSE;

    TRACE("(%p, %p)\n", thumbnail, size);

    if (!thumbnail)
        return E_INVALIDARG;
    if (!size)
        return E_INVALIDARG;

    EnterCriticalSection(&thumbnail_cs);
    for (i = 0; i < THUMBNAIL_TABLE_SIZE; i++)
    {
        if (thumbnail_table[i].handle == thumbnail)
        {
            found = TRUE;
            if (!IsWindow(thumbnail_table[i].dest) || !IsWindow(thumbnail_table[i].src))
            {
                LeaveCriticalSection(&thumbnail_cs);
                return E_HANDLE;
            }
            src = thumbnail_table[i].src;
            break;
        }
    }
    LeaveCriticalSection(&thumbnail_cs);
    if (!found)
        return E_INVALIDARG;

    /* Windows reports the visible frame of the source, the same rectangle as DWMWA_EXTENDED_FRAME_BOUNDS */
    if (DwmGetWindowAttribute(src, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)) != S_OK)
        memset(&rect, 0, sizeof(rect));
    size->cx = rect.right - rect.left;
    size->cy = rect.bottom - rect.top;
    return S_OK;
}

/**********************************************************************
 *           DwmModifyPreviousDxFrameDuration         (DWMAPI.@)
 */
HRESULT WINAPI DwmModifyPreviousDxFrameDuration(HWND hwnd, INT refreshes, BOOL relative)
{
    TRACE("(%p, %d, %d)\n", hwnd, refreshes, relative);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmTransitionOwnedWindow         (DWMAPI.@)
 */
HRESULT WINAPI DwmTransitionOwnedWindow(HWND hwnd, enum DWMTRANSITION_OWNEDWINDOW_TARGET target)
{
    TRACE("(%p, %d)\n", hwnd, target);

    return E_INVALIDARG;
}
