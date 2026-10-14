/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * Private shell window controls, offered only on the privileged socket.
 */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include <math.h>
#include "waylanddrv.h"
#include "wexwayland.h"
#include "wine/debug.h"
WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static const WCHAR handle_prop[] = u"__wine_wayland_window_handle";

static void wex_handle(void *user_data, struct wex_window_v1 *window, const char *handle)
{
    HWND hwnd = user_data;
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    struct wayland_surface *surface;
    WCHAR name[33];
    RTL_ATOM atom;
    unsigned int i;
    if (!data) return;
    surface = data->wayland_surface;
    if (!surface || surface->wex_window != window || surface->wex_handle_atom) goto done;
    /* The private v2 server generates a 128-bit hexadecimal token. Avoid
     * publishing truncated names, and never treat tokens as pointers. */
    if (strlen(handle) != 32) goto done;
    for (i = 0; i < 32; ++i)
    {
        if (!((handle[i] >= '0' && handle[i] <= '9') ||
              (handle[i] >= 'a' && handle[i] <= 'f'))) goto done;
        name[i] = handle[i];
    }
    name[32] = 0;
    if (NtAddAtom(name, 32 * sizeof(WCHAR), &atom)) goto done;
    if (!NtUserSetProp(hwnd, handle_prop, (HANDLE)(ULONG_PTR)atom))
    {
        NtDeleteAtom(atom);
        goto done;
    }
    surface->wex_handle_atom = atom;
    /* A late handle participates in the current global Win32 order, rather
     * than the order in which separate Wayland connections happened to map. */
    wayland_wex_update(surface, HWND_TOP, 0);
done:
    wayland_win_data_release(data);
}

static const struct wex_window_v1_listener wex_listener = { wex_handle };

void wayland_wex_destroy(struct wayland_surface *surface)
{
    if (surface->wex_handle_atom)
    {
        if ((ULONG_PTR)NtUserGetProp(surface->hwnd, handle_prop) == surface->wex_handle_atom)
            NtUserRemoveProp(surface->hwnd, handle_prop);
        NtDeleteAtom(surface->wex_handle_atom);
        surface->wex_handle_atom = 0;
    }
    wex_window_v1_destroy(surface->wex_window);
    surface->wex_window = NULL;
}

static unsigned int window_layer(HWND hwnd)
{
    unsigned int role = (ULONG_PTR)NtUserGetProp(hwnd, WEX_WAYLAND_ROLE_PROP);
    return role == WEX_WAYLAND_DESKTOP || role == WEX_WAYLAND_SHELL ? role :
           NtUserGetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST ?
           WEX_WINDOW_V1_LAYER_TOPMOST : WEX_WINDOW_V1_LAYER_NORMAL;
}

static BOOL stack_with_sibling(struct wayland_surface *surface, HWND hwnd, BOOL above)
{
    struct wayland_win_data *data;
    struct wayland_surface *sibling;
    BOOL placed = FALSE;
    if (window_layer(hwnd) != surface->wex_layer) return FALSE;
    if (wex_window_v1_get_version(surface->wex_window) >= 2)
    {
        union { ATOM_BASIC_INFORMATION info; char buffer[sizeof(ATOM_BASIC_INFORMATION) + 66]; } atom;
        RTL_ATOM id = (ULONG_PTR)NtUserGetProp(hwnd, handle_prop);
        char handle[33];
        unsigned int i;
        if (id && !NtQueryInformationAtom(id, AtomBasicInformation, &atom, sizeof(atom), NULL)
            && atom.info.NameLength == 32 * sizeof(WCHAR))
        {
            for (i = 0; i < 32; ++i) handle[i] = atom.info.Name[i];
            handle[32] = 0;
            if (above) wex_window_v1_place_above_handle(surface->wex_window, handle);
            else wex_window_v1_place_below_handle(surface->wex_window, handle);
            return TRUE;
        }
    }
    /* v1 compositors accept objects only from this process's connection. */
    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    sibling = data->wayland_surface;
    if (sibling && sibling->wex_window && sibling->wex_layer == surface->wex_layer)
    {
        if (above) wex_window_v1_place_above(surface->wex_window, sibling->wex_window);
        else wex_window_v1_place_below(surface->wex_window, sibling->wex_window);
        placed = TRUE;
    }
    wayland_win_data_release(data);
    return placed;
}

void wayland_wex_create(struct wayland_surface *surface)
{
    if (!process_wayland.wex_manager) return;
    surface->wex_window = wex_window_manager_v1_get_window(process_wayland.wex_manager, surface->xdg_toplevel);
    wex_window_v1_add_listener(surface->wex_window, &wex_listener, surface->hwnd);
    surface->wex_layer = ~0u;
    surface->wex_edge = ~0u;
    surface->wex_activation_pending = surface->hwnd == NtUserGetForegroundWindow();
    wayland_wex_update(surface, HWND_TOP, SWP_NOZORDER);
}

void wayland_wex_update(struct wayland_surface *surface, HWND insert_after, UINT flags)
{
    HWND hwnd;
    unsigned int layer, edge = 0, size = 0;
    if (!surface->wex_window) return;
    layer = window_layer(surface->hwnd);
    if (layer != surface->wex_layer)
    {
        wex_window_v1_set_layer(surface->wex_window, layer);
        surface->wex_layer = layer;
    }
    if (layer == WEX_WINDOW_V1_LAYER_SHELL)
    {
        edge = (ULONG_PTR)NtUserGetProp(surface->hwnd, WEX_WAYLAND_EDGE_PROP);
        size = (ULONG_PTR)NtUserGetProp(surface->hwnd, WEX_WAYLAND_SIZE_PROP);
        size = round(size / surface->window.scale);
        if (edge > WEX_WINDOW_V1_EDGE_RIGHT) edge = WEX_WINDOW_V1_EDGE_NONE;
    }
    if (edge != surface->wex_edge || size != surface->wex_size)
    {
        wex_window_v1_set_exclusive_edge(surface->wex_window, edge, size);
        surface->wex_edge = edge;
        surface->wex_size = size;
    }
    if (!(flags & SWP_NOZORDER))
    {
        if (insert_after == HWND_BOTTOM) wex_window_v1_place_bottom(surface->wex_window);
        else
        {
            hwnd = NtUserGetWindowRelative(surface->hwnd, GW_HWNDPREV);
            for (; hwnd; hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDPREV))
                if (stack_with_sibling(surface, hwnd, FALSE)) break;
            if (!hwnd)
            {
                hwnd = NtUserGetWindowRelative(surface->hwnd, GW_HWNDNEXT);
                for (; hwnd; hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDNEXT))
                    if (stack_with_sibling(surface, hwnd, TRUE)) break;
            }
        }
    }
    if (surface->wex_minimized && !surface->window.minimized)
    {
        if (surface->hwnd == NtUserGetForegroundWindow()) surface->wex_activation_pending = TRUE;
        else if (wex_window_v1_get_version(surface->wex_window) >= 2)
            wex_window_v1_restore(surface->wex_window);
    }
    surface->wex_minimized = surface->window.minimized;
    /* Restore can reuse its existing buffer without triggering a new paint.
     * Once the updated Win32 state is non-minimized, flush a pending activation
     * against that committed content instead of waiting for another buffer. */
    wayland_wex_flush_activation(surface);
    wl_display_flush(process_wayland.wl_display);
}

void wayland_wex_flush_activation(struct wayland_surface *surface)
{
    if (!surface->wex_window || !surface->wex_activation_pending || !surface->content_width ||
        surface->window.minimized) return;
    surface->wex_activation_pending = FALSE;
    /* A Win32 activation can precede the first buffer commit. The compositor
     * can focus only mapped views, so send the request after that commit. */
    if (surface->hwnd == NtUserGetForegroundWindow())
        wex_window_v1_activate(surface->wex_window);
}

void WAYLAND_ActivateWindow(HWND hwnd, HWND previous)
{
    struct wayland_win_data *data;
    TRACE("activate hwnd=%p previous=%p\n", hwnd, previous);
    /* activate() raises the compositor view. Keep Wine's own hit-test order
     * consistent with that raise, including cross-thread foreground changes. */
    if (process_wayland.wex_manager)
        NtUserSetWindowPos(hwnd, window_layer(hwnd) == WEX_WINDOW_V1_LAYER_DESKTOP ? HWND_BOTTOM : HWND_TOP,
                           0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    data = wayland_win_data_get(hwnd);
    if (!data) return;
    if (data->wayland_surface && data->wayland_surface->wex_window)
    {
        ++data->wayland_surface->wex_activation_epoch;
        data->wayland_surface->wex_activation_pending = TRUE;
        wayland_wex_flush_activation(data->wayland_surface);
        wl_display_flush(process_wayland.wl_display);
    }
    wayland_win_data_release(data);
}
