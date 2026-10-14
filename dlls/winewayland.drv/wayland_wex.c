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

static BOOL hosting_enabled(struct wayland_surface *surface)
{
    return wex_window_v1_get_version(surface->wex_window) >= 7 &&
           NtUserGetProp(surface->hwnd, WEX_WAYLAND_HOST_PROP) &&
           (ULONG_PTR)NtUserGetProp(surface->hwnd, WEX_WAYLAND_ASSOCIATION_PROP) ==
               WEX_WAYLAND_ASSOCIATION_PREPARED && !surface->wex_association_pending;
}

static void wex_group(void *user_data, struct wex_window_v1 *window, uint32_t status)
{
    HWND hwnd = user_data;
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    struct wayland_surface *surface;
    if (!data) return;
    surface = data->wayland_surface;
    if (surface && surface->wex_window == window)
    {
        surface->wex_group = status;
        if (hosting_enabled(surface) && status)
            NtUserSetProp(hwnd, WEX_WAYLAND_GROUP_PROP, (HANDLE)(ULONG_PTR)status);
        else NtUserRemoveProp(hwnd, WEX_WAYLAND_GROUP_PROP);
        if (status != WEX_WINDOW_V1_GROUP_STATUS_ACTIVE)
        {
            surface->wex_focus = WEX_WINDOW_V1_FOCUS_TARGET_NONE;
            NtUserRemoveProp(hwnd, WEX_WAYLAND_FOCUS_PROP);
            NtUserPostMessage(hwnd, WM_WAYLAND_CLEAR_FOREGROUND, surface->wex_activation_epoch, 0);
        }
    }
    wayland_win_data_release(data);
}

static void wex_configured(void *user_data, struct wex_window_v1 *window, uint32_t serial, uint32_t status)
{
    struct wayland_win_data *data = wayland_win_data_get(user_data);
    struct wayland_surface *surface;
    if (!data) return;
    surface = data->wayland_surface;
    if (surface && surface->wex_window == window && serial == surface->wex_group_serial)
    {
        surface->wex_group_pending = FALSE;
        wayland_wex_update_hosting(surface);
        if (status == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE && hosting_enabled(surface))
        {
            surface->wex_activation_pending = surface->hwnd == NtUserGetForegroundWindow();
            wayland_wex_flush_activation(surface);
        }
        wl_display_flush(process_wayland.wl_display);
    }
    wayland_win_data_release(data);
}

static void wex_focus(void *user_data, struct wex_window_v1 *window, uint32_t epoch, uint32_t target)
{
    HWND hwnd = user_data;
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    struct wayland_surface *surface;
    if (!data) return;
    surface = data->wayland_surface;
    if (surface && surface->wex_window == window && epoch == surface->wex_activation_epoch)
    {
        surface->wex_focus = target;
        surface->wex_focus_epoch = epoch;
        if (hosting_enabled(surface) && surface->wex_group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE)
        {
            if (target) NtUserSetProp(hwnd, WEX_WAYLAND_FOCUS_PROP, (HANDLE)(ULONG_PTR)target);
            else NtUserRemoveProp(hwnd, WEX_WAYLAND_FOCUS_PROP);
            NtUserPostMessage(hwnd, target == WEX_WINDOW_V1_FOCUS_TARGET_NATIVE ?
                              WM_WAYLAND_FOREIGN_FOREGROUND : WM_WAYLAND_CLEAR_FOREGROUND, epoch, 0);
        }
    }
    wayland_win_data_release(data);
}

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

static void wex_association(void *user_data, struct wex_window_v1 *window, uint32_t status)
{
    HWND hwnd = user_data;
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    struct wayland_surface *surface;
    if (!data) return;
    surface = data->wayland_surface;
    if (surface && surface->wex_window == window)
    {
        /* Only one association request is outstanding. If the bridge changed
         * its atoms in the meantime, suppress the old response and send the
         * new pair after completing this one. */
        surface->wex_association_pending = FALSE;
        surface->wex_host_attempted = FALSE;
        if ((ULONG_PTR)NtUserGetProp(hwnd, WEX_WAYLAND_NATIVE_PROP) == surface->wex_native_atom &&
            (ULONG_PTR)NtUserGetProp(hwnd, WEX_WAYLAND_CONTROLLER_PROP) == surface->wex_controller_atom)
        {
            if (status) NtUserSetProp(hwnd, WEX_WAYLAND_ASSOCIATION_PROP, (HANDLE)(ULONG_PTR)status);
            else NtUserRemoveProp(hwnd, WEX_WAYLAND_ASSOCIATION_PROP);
        }
        wayland_wex_update(surface, HWND_TOP, SWP_NOZORDER);
    }
    wayland_win_data_release(data);
}
static const struct wex_window_v1_listener wex_listener = {
    .handle = wex_handle, .foreign_association = wex_association,
    .foreign_group = wex_group, .foreign_focus = wex_focus,
    .configured_foreign = wex_configured,
};

void wayland_wex_update_hosting(struct wayland_surface *surface)
{
    RECT rect = surface->window.client_rect;
    BOOL enabled;
    ULONG_PTR focus;
    if (!surface->wex_window || wex_window_v1_get_version(surface->wex_window) < 7 ||
        surface->wex_group_pending) return;
    enabled = hosting_enabled(surface);
    OffsetRect(&rect, -surface->window.rect.left, -surface->window.rect.top);
    rect = map_rect_to_surface(surface, rect);
    if (surface->wex_group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE &&
        (!enabled || !EqualRect(&rect, &surface->wex_host_rect)))
    {
        surface->wex_group_pending = TRUE;
        surface->wex_host_attempted = FALSE;
        wex_window_v1_uncommit_foreign(surface->wex_window, ++surface->wex_group_serial);
        return;
    }
    if (!enabled)
    {
        NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_GROUP_PROP);
        NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_FOCUS_PROP);
        return;
    }
    if (!surface->content_width || surface->window.minimized) return;
    if (surface->wex_group != WEX_WINDOW_V1_GROUP_STATUS_ACTIVE &&
        (!surface->wex_host_attempted || !EqualRect(&rect, &surface->wex_host_rect)))
    {
        surface->wex_host_rect = rect;
        surface->wex_host_attempted = TRUE;
        surface->wex_group_pending = TRUE;
        wex_window_v1_configure_foreign(surface->wex_window, ++surface->wex_group_serial,
                                      rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }
    if (surface->wex_group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE &&
        (focus = (ULONG_PTR)NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_FOCUS_REQUEST_PROP)) &&
        surface->hwnd == NtUserGetForegroundWindow() && focus <= WEX_WAYLAND_FOCUS_RESTORE_NATIVE)
        wex_window_v1_set_foreign_focus(surface->wex_window, ++surface->wex_activation_epoch, focus);
}

static BOOL get_atom_handle(RTL_ATOM id, char handle[33])
{
    union { ATOM_BASIC_INFORMATION info; char buffer[sizeof(ATOM_BASIC_INFORMATION) + 66]; } atom;
    unsigned int i;
    if (!id) { handle[0] = 0; return TRUE; }
    if (NtQueryInformationAtom(id, AtomBasicInformation, &atom, sizeof(atom), NULL) ||
        atom.info.NameLength != 32 * sizeof(WCHAR)) return FALSE;
    for (i=0;i<32;i++)
    {
        WCHAR ch = atom.info.Name[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return FALSE;
        handle[i] = ch;
    }
    handle[32] = 0;
    return TRUE;
}

static void update_foreign_association(struct wayland_surface *surface)
{
    RTL_ATOM native, controller;
    char token[33], handle[33];
    if (wex_window_v1_get_version(surface->wex_window) < 4 || surface->wex_association_pending) return;
    native = (ULONG_PTR)NtUserGetProp(surface->hwnd, WEX_WAYLAND_NATIVE_PROP);
    controller = (ULONG_PTR)NtUserGetProp(surface->hwnd, WEX_WAYLAND_CONTROLLER_PROP);
    if (native == surface->wex_native_atom && controller == surface->wex_controller_atom) return;
    if (!get_atom_handle(native, token)) strcpy(token,"?");
    if (!get_atom_handle(controller, handle)) strcpy(handle,"?");
    surface->wex_native_atom = native;
    surface->wex_controller_atom = controller;
    surface->wex_association_pending = TRUE;
    NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_ASSOCIATION_PROP);
    NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_GROUP_PROP);
    NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_FOCUS_PROP);
    wex_window_v1_associate_foreign(surface->wex_window, token, handle);
}

void wayland_wex_destroy(struct wayland_surface *surface)
{
    NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_ASSOCIATION_PROP);
    NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_GROUP_PROP);
    NtUserRemoveProp(surface->hwnd, WEX_WAYLAND_FOCUS_PROP);
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
    if (flags & SWP_FRAMECHANGED) surface->wex_host_attempted = FALSE;
    update_foreign_association(surface);
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
    wayland_wex_update_hosting(surface);
    if (!surface->wex_window || !surface->wex_activation_pending || !surface->content_width ||
        surface->window.minimized) return;
    surface->wex_activation_pending = FALSE;
    /* A Win32 activation can precede the first buffer commit. The compositor
     * can focus only mapped views, so send the request after that commit. */
    if (surface->hwnd == NtUserGetForegroundWindow())
    {
        if (surface->wex_group == WEX_WINDOW_V1_GROUP_STATUS_ACTIVE && hosting_enabled(surface))
            wex_window_v1_set_foreign_focus(surface->wex_window, surface->wex_activation_epoch,
                                          WEX_WINDOW_V1_FOCUS_TARGET_NATIVE);
        else wex_window_v1_activate(surface->wex_window);
    }
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
