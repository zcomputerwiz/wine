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

void wayland_wex_create(struct wayland_surface *surface)
{
    if (!process_wayland.wex_manager) return;
    surface->wex_window = wex_window_manager_v1_get_window(process_wayland.wex_manager, surface->xdg_toplevel);
    surface->wex_layer = ~0u;
    surface->wex_edge = ~0u;
    surface->wex_activation_pending = surface->hwnd == NtUserGetForegroundWindow();
    wayland_wex_update(surface, HWND_TOP, SWP_NOZORDER);
}

void wayland_wex_update(struct wayland_surface *surface, HWND insert_after, UINT flags)
{
    struct wayland_win_data *sibling_data;
    struct wayland_surface *sibling;
    HWND hwnd;
    unsigned int role, layer, edge = 0, size = 0;
    if (!surface->wex_window) return;
    role = (ULONG_PTR)NtUserGetProp(surface->hwnd, WEX_WAYLAND_ROLE_PROP);
    layer = role == WEX_WAYLAND_DESKTOP || role == WEX_WAYLAND_SHELL ? role :
            NtUserGetWindowLongW(surface->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST ?
            WEX_WINDOW_V1_LAYER_TOPMOST : WEX_WINDOW_V1_LAYER_NORMAL;
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
            /* Only objects on this connection may be passed as siblings.
             * Cross-process neighbours require shared protocol handles. */
            hwnd = NtUserGetWindowRelative(surface->hwnd, GW_HWNDPREV);
            for (; hwnd; hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDPREV))
            {
                if (!(sibling_data = wayland_win_data_get(hwnd))) continue;
                sibling = sibling_data->wayland_surface;
                if (sibling && sibling->wex_window && sibling->wex_layer == layer)
                    wex_window_v1_place_below(surface->wex_window, sibling->wex_window);
                else sibling = NULL;
                wayland_win_data_release(sibling_data);
                if (sibling) break;
            }
            if (!hwnd)
            {
                hwnd = NtUserGetWindowRelative(surface->hwnd, GW_HWNDNEXT);
                for (; hwnd; hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDNEXT))
                {
                    if (!(sibling_data = wayland_win_data_get(hwnd))) continue;
                    sibling = sibling_data->wayland_surface;
                    if (sibling && sibling->wex_window && sibling->wex_layer == layer)
                        wex_window_v1_place_above(surface->wex_window, sibling->wex_window);
                    else sibling = NULL;
                    wayland_win_data_release(sibling_data);
                    if (sibling) break;
                }
            }
        }
    }
    if (surface->wex_minimized && !surface->window.minimized &&
        surface->hwnd == NtUserGetForegroundWindow()) surface->wex_activation_pending = TRUE;
    surface->wex_minimized = surface->window.minimized;
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
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    if (!data) return;
    if (data->wayland_surface && data->wayland_surface->wex_window)
    {
        data->wayland_surface->wex_activation_pending = TRUE;
        wayland_wex_flush_activation(data->wayland_surface);
        wl_display_flush(process_wayland.wl_display);
    }
    wayland_win_data_release(data);
}
