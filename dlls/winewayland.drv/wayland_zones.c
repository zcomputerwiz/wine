/*
 * SPDX-License-Identifier: LGPL-2.1-or-later OR GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * Optional shared Wayland coordinate spaces.
 */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include "ntstatus.h"
#include "waylanddrv.h"
#include "wine/debug.h"
WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static char *zone_handle;
static BOOL zone_done;

static NTSTATUS open_current_user(HANDLE *key)
{
    HANDLE token;
    char info[256], path[256];
    WCHAR path_w[256];
    SID *sid;
    ULONG size;
    unsigned int i, len;
    unsigned long long authority = 0;
    NTSTATUS status;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;

    if ((status = NtOpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))) return status;
    status = NtQueryInformationToken(token, TokenUser, info, sizeof(info), &size);
    NtClose(token);
    if (status) return status;
    sid = ((TOKEN_USER *)info)->User.Sid;
    for (i = 0; i < 6; ++i) authority = (authority << 8) | sid->IdentifierAuthority.Value[i];
    len = snprintf(path, sizeof(path), "\\Registry\\User\\S-%u-%llu", sid->Revision, authority);
    for (i = 0; i < sid->SubAuthorityCount; ++i)
        len += snprintf(path + len, sizeof(path) - len, "-%u", sid->SubAuthority[i]);
    for (i = 0; i <= len; ++i) path_w[i] = path[i];
    RtlInitUnicodeString(&name, path_w);
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtOpenKey(key, KEY_ALL_ACCESS, &attr);
}

static void zone_size(void *data, struct xx_zone_v1 *zone, int32_t width, int32_t height)
{
    process_wayland.zone_width = width;
    process_wayland.zone_height = height;
    /* Re-place live items after changes to the compositor's coordinate space. */
    wayland_zones_reposition();
}

static void zone_handle_event(void *data, struct xx_zone_v1 *zone, const char *handle)
{
    free(zone_handle);
    zone_handle = strdup(handle);
    TRACE("shared zone handle %s\n", debugstr_a(handle));
}

static void zone_done_event(void *data, struct xx_zone_v1 *zone)
{
    zone_done = TRUE;
}

static void zone_item_rejected(void *data, struct xx_zone_v1 *zone, struct xx_zone_item_v1 *item)
{
    WARN("Compositor rejected zone item %p\n", item);
}

static void zone_item_entered(void *data, struct xx_zone_v1 *zone, struct xx_zone_item_v1 *item) {}

static void zone_item_left(void *data, struct xx_zone_v1 *zone, struct xx_zone_item_v1 *item)
{
    /* Item user data is an HWND, never a freed surface pointer. */
    HWND hwnd = wl_proxy_get_user_data((struct wl_proxy *)item);
    struct wayland_win_data *win = wayland_win_data_get(hwnd);
    if (!win) return;
    if (win->wayland_surface && win->wayland_surface->zone_item == item)
    {
        xx_zone_v1_add_item(zone, item);
        wayland_zone_place(win->wayland_surface);
        wl_surface_commit(win->wayland_surface->wl_surface);
        wl_display_flush(process_wayland.wl_display);
    }
    wayland_win_data_release(win);
}

static const struct xx_zone_v1_listener zone_listener =
{
    zone_size, zone_handle_event, zone_done_event,
    zone_item_rejected, zone_item_entered, zone_item_left
};

void wayland_zones_init(void)
{
    HANDLE user, key = NULL, mutex = NULL;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name, value;
    WCHAR value_name[64];
    char buffer[4096], *display = getenv("WAYLAND_DISPLAY"), *runtime = getenv("XDG_RUNTIME_DIR");
    KEY_VALUE_PARTIAL_INFORMATION *info = (void *)buffer;
    ULONG size;
    unsigned int hash = 2166136261u;
    unsigned int i;
    const char *p;
    struct wayland_output *output;
    struct wl_output *primary = NULL;

    if (!process_wayland.zone_manager) return;
    /* Serialize creation across Wine processes, including terminal launches.
     * Values are scoped to the socket path, not inherited environment tokens. */
    for (p = runtime ? runtime : ""; *p; ++p) hash = (hash ^ (unsigned char)*p) * 16777619u;
    for (p = display ? display : "wayland-0"; *p; ++p) hash = (hash ^ (unsigned char)*p) * 16777619u;
    snprintf(buffer, sizeof(buffer), "Zone_%08x", hash);
    for (i = 0; (value_name[i] = buffer[i]); ++i) {}
    RtlInitUnicodeString(&value, value_name);
    RtlInitUnicodeString(&name, u"\\BaseNamedObjects\\WineWaylandZones");
    InitializeObjectAttributes(&attr, &name, OBJ_OPENIF | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtCreateMutant(&mutex, MUTANT_ALL_ACCESS, &attr, FALSE) < 0) return;
    if (NtWaitForSingleObject(mutex, FALSE, NULL) < 0) { NtClose(mutex); return; }
    if (!open_current_user(&user))
    {
        RtlInitUnicodeString(&name, u"Software\\Wine\\Wayland");
        InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, user, NULL);
        if (!NtCreateKey(&key, KEY_ALL_ACCESS, &attr, 0, NULL, REG_OPTION_VOLATILE, NULL))
        {
            if (!NtQueryValueKey(key, &value, KeyValuePartialInformation, info, sizeof(buffer), &size) &&
                info->Type == REG_BINARY && info->DataLength &&
                info->DataLength <= sizeof(buffer) - offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) &&
                !info->Data[info->DataLength - 1])
                process_wayland.zone = xx_zone_manager_v1_get_zone_from_handle(
                    process_wayland.zone_manager, (char *)info->Data);
        }
        NtClose(user);
    }
    if (process_wayland.zone)
    {
        xx_zone_v1_add_listener(process_wayland.zone, &zone_listener, NULL);
        wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);
        if (!zone_done || !zone_handle || !*zone_handle || process_wayland.zone_width < 0)
        {
            xx_zone_v1_destroy(process_wayland.zone);
            process_wayland.zone = NULL;
        }
    }
    if (!process_wayland.zone)
    {
        pthread_mutex_lock(&process_wayland.output_mutex);
        wl_list_for_each(output, &process_wayland.output_list, link)
            if (!output->current.logical_x && !output->current.logical_y) { primary = output->wl_output; break; }
        pthread_mutex_unlock(&process_wayland.output_mutex);
        zone_done = FALSE;
        process_wayland.zone = xx_zone_manager_v1_get_zone(process_wayland.zone_manager, primary);
        xx_zone_v1_add_listener(process_wayland.zone, &zone_listener, NULL);
        wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);
    }
    if (key)
    {
        if (zone_done && zone_handle && *zone_handle && process_wayland.zone_width >= 0)
            NtSetValueKey(key, &value, 0, REG_BINARY, zone_handle, strlen(zone_handle) + 1);
        NtClose(key);
    }
    NtReleaseMutant(mutex, NULL);
    NtClose(mutex);
}

static void item_extents(void *data, struct xx_zone_item_v1 *item,
                         int32_t top, int32_t bottom, int32_t left, int32_t right)
{
    struct wayland_win_data *win = wayland_win_data_get(data);
    if (!win) return;
    if (win->wayland_surface && win->wayland_surface->zone_item == item)
    {
        win->wayland_surface->zone_frame.left = left;
        win->wayland_surface->zone_frame.top = top;
    }
    wayland_win_data_release(win);
}

static void item_position(void *data, struct xx_zone_item_v1 *item, int32_t x, int32_t y)
{
    struct wayland_win_data *win = wayland_win_data_get(data);
    RECT rect;
    POINT point;
    if (!win) return;
    if (!win->wayland_surface || win->wayland_surface->zone_item != item)
    { wayland_win_data_release(win); return; }
    TRACE("hwnd %p zone position %d,%d\n", data, x, y);
    /* add_item emits an initial position before the first placement commit. */
    if (win->wayland_surface->zone_initial_position)
    {
        win->wayland_surface->zone_initial_position = FALSE;
        wayland_win_data_release(win);
        return;
    }
    point.x = x + win->wayland_surface->zone_frame.left;
    point.y = y + win->wayland_surface->zone_frame.top;
    point = map_point_from_surface(win->wayland_surface, point);
    rect = win->rects.window;
    /* The Wayland surface may start inside the Win32 window rectangle. */
    point.x -= win->rects.visible.left - rect.left;
    point.y -= win->rects.visible.top - rect.top;
    wayland_win_data_release(win);
    if (point.x != rect.left || point.y != rect.top)
        NtUserSetWindowPos(data, 0, point.x, point.y, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void item_position_failed(void *data, struct xx_zone_item_v1 *item)
{ WARN("Compositor refused position for hwnd %p\n", data); }
static void item_closed(void *data, struct xx_zone_item_v1 *item) {}
static const struct xx_zone_item_v1_listener item_listener =
{ item_extents, item_position, item_position_failed, item_closed };

void wayland_zone_create_item(struct wayland_surface *surface)
{
    if (!process_wayland.zone || process_wayland.zone_width < 0 || process_wayland.zone_height < 0) return;
    surface->zone_item = xx_zone_manager_v1_get_zone_item(process_wayland.zone_manager, surface->xdg_toplevel);
    surface->zone_initial_position = TRUE;
    xx_zone_item_v1_add_listener(surface->zone_item, &item_listener, surface->hwnd);
    xx_zone_v1_add_item(process_wayland.zone, surface->zone_item);
}

void wayland_zone_place(struct wayland_surface *surface)
{
    POINT point;
    if (!surface->zone_item || surface->window.minimized) return;
    point.x = surface->window.rect.left;
    point.y = surface->window.rect.top;
    point = map_point_to_surface(surface, point);
    TRACE("hwnd %p request zone position %d,%d\n", surface->hwnd, point.x, point.y);
    xx_zone_item_v1_set_position(surface->zone_item, point.x - surface->zone_frame.left,
                                point.y - surface->zone_frame.top);
}
