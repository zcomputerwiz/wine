/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * Private scalar window properties. No process-local pointers cross this ABI.
 * Set before mapping, or follow an update with SetWindowPos to notify the driver.
 * Edge sizes are Win32 physical pixels; the driver converts to surface units.
 */
#ifndef __WEX_WAYLAND_H
#define __WEX_WAYLAND_H
#define WEX_WAYLAND_ROLE_PROP u"__wine_wayland_role"
#define WEX_WAYLAND_EDGE_PROP u"__wine_wayland_exclusive_edge"
#define WEX_WAYLAND_SIZE_PROP u"__wine_wayland_exclusive_size"
/* Global atom IDs, owned by the bridge. Set before ShowWindow. */
#define WEX_WAYLAND_NATIVE_PROP u"__wine_wayland_native_token"
#define WEX_WAYLAND_CONTROLLER_PROP u"__wine_wayland_native_controller"
/* Asynchronous reservation status; PREPARED does not imply hosted content. */
#define WEX_WAYLAND_ASSOCIATION_PROP u"__wine_wayland_native_association"
#define WEX_WAYLAND_ASSOCIATION_UNBOUND 0
#define WEX_WAYLAND_ASSOCIATION_PREPARED 1
#define WEX_WAYLAND_ASSOCIATION_UNAVAILABLE 2
/* Explicit opt-in: caller must exclude the client area from Win32 hit testing. */
#define WEX_WAYLAND_HOST_PROP u"__wine_wayland_native_host"
/* In-process bridge/uxtheme contract, never sent to the driver or compositor.
 * OUTER is a borrowed HRGN owned by the bridge (absent means rectangular).
 * MASK_UPDATING distinguishes bridge clipping from an application's shape. */
#define WEX_WAYLAND_FRAME_OUTER_PROP u"__wine_wayland_frame_outer_region"
#define WEX_WAYLAND_FRAME_MASK_UPDATING_PROP u"__wine_wayland_frame_mask_updating"
#define WEX_WAYLAND_GROUP_PROP u"__wine_wayland_native_group"
/* Read-only preferred wl_surface scale, in units of 1/120. Native inventory
 * geometry is logical; the bridge maps it into this surface's Win32 pixels.
 * A registered SCALE_EVENT_MESSAGE notifies the owning thread after changes. */
#define WEX_WAYLAND_SCALE_PROP u"__wine_wayland_scale_120"
#define WEX_WAYLAND_SCALE_EVENT_MESSAGE u"WineExplorer.WaylandScaleChanged"
#define WEX_WAYLAND_GROUP_DETACHED 0
#define WEX_WAYLAND_GROUP_ACTIVE 1
#define WEX_WAYLAND_GROUP_NOT_READY 2
#define WEX_WAYLAND_FOCUS_PROP u"__wine_wayland_native_focus"
/* One-shot command, consumed after SetWindowPos. Restore requires foreground. */
#define WEX_WAYLAND_FOCUS_REQUEST_PROP u"__wine_wayland_native_focus_request"
/* Observed compositor pointer capture for a hosted Win32 move/size loop. */
#define WEX_WAYLAND_CAPTURE_PROP u"__wine_wayland_native_pointer_capture"
/* One-shot request for an owned foreground frame outside the move/size loop
 * (work-area maximize/restore). The compositor still checks physical focus. */
#define WEX_WAYLAND_CAPTURE_REQUEST_PROP u"__wine_wayland_native_pointer_capture_request"
/* Captured frame state preceding its resize request: 1 maximize, 2 restore. */
#define WEX_WAYLAND_MAXIMIZE_REQUEST_PROP u"__wine_wayland_native_maximize_request"
/* v10 transaction transport, separate from coalescible native observation.
 * All values fit 32 bits, including signed coordinates represented as LONG.
 * Set request fields, then COMMAND last, followed by SetWindowPos. Coordinates
 * are physical Win32 screen client coordinates; COMMIT uses the current frame
 * client hole after painting, ignoring the request geometry fields.
 * PROTOCOL advertises transport only: a compositor can still reply unavailable.
 * EVENT_MESSAGE runs synchronously on the owning UI thread, with serial in
 * WPARAM and stage in LPARAM. EVENT_* is an immutable snapshot for that call
 * only; do not retain it or read it from an observer thread. No pointer crosses
 * the Unix/PE boundary, including in WoW64. Every transition is delivered.
 */
#define WEX_WAYLAND_RESIZE_PROTOCOL_PROP u"__wine_wayland_resize_protocol"
/* Owned controller opt-in; not an advertisement of native eligibility. */
#define WEX_WAYLAND_RESIZE_ENABLE_PROP u"__wine_wayland_resize_enable"
#define WEX_WAYLAND_RESIZE_COMMAND_PROP u"__wine_wayland_resize_command"
#define WEX_WAYLAND_RESIZE_SERIAL_PROP u"__wine_wayland_resize_serial"
#define WEX_WAYLAND_RESIZE_EDGES_PROP u"__wine_wayland_resize_edges"
#define WEX_WAYLAND_RESIZE_X_PROP u"__wine_wayland_resize_x"
#define WEX_WAYLAND_RESIZE_Y_PROP u"__wine_wayland_resize_y"
#define WEX_WAYLAND_RESIZE_WIDTH_PROP u"__wine_wayland_resize_width"
#define WEX_WAYLAND_RESIZE_HEIGHT_PROP u"__wine_wayland_resize_height"
#define WEX_WAYLAND_RESIZE_EVENT_MESSAGE u"__wine_wayland_resize_event"
#define WEX_WAYLAND_RESIZE_EVENT_CONFIGURE_PROP u"__wine_wayland_resize_event_configure"
#define WEX_WAYLAND_RESIZE_EVENT_X_PROP u"__wine_wayland_resize_event_x"
#define WEX_WAYLAND_RESIZE_EVENT_Y_PROP u"__wine_wayland_resize_event_y"
#define WEX_WAYLAND_RESIZE_EVENT_WIDTH_PROP u"__wine_wayland_resize_event_width"
#define WEX_WAYLAND_RESIZE_EVENT_HEIGHT_PROP u"__wine_wayland_resize_event_height"
#define WEX_WAYLAND_RESIZE_REQUEST 1
#define WEX_WAYLAND_RESIZE_COMMIT 2
#define WEX_WAYLAND_RESIZE_CANCEL 3
/* Edges are protocol bit flags (top=1,bottom=2,left=4,right=8), not EDGE_*. */
#define WEX_WAYLAND_RESIZE_ACCEPTED 0
#define WEX_WAYLAND_RESIZE_ACKNOWLEDGED 1
#define WEX_WAYLAND_RESIZE_NATIVE_READY 2
#define WEX_WAYLAND_RESIZE_COMPLETE 3
#define WEX_WAYLAND_RESIZE_ROLLING_BACK 4
#define WEX_WAYLAND_RESIZE_CANCELLED 5
#define WEX_WAYLAND_RESIZE_SUPERSEDED 6
#define WEX_WAYLAND_RESIZE_UNAVAILABLE 7
#define WEX_WAYLAND_RESIZE_STALE 8
#define WEX_WAYLAND_RESIZE_FAILED 9
#define WEX_WAYLAND_FOCUS_NONE 0
#define WEX_WAYLAND_FOCUS_NATIVE 1
#define WEX_WAYLAND_FOCUS_FRAME 2
#define WEX_WAYLAND_FOCUS_RESTORE_NATIVE 3
#define WEX_WAYLAND_DESKTOP 2
#define WEX_WAYLAND_SHELL 3
#define WEX_WAYLAND_EDGE_NONE 0
#define WEX_WAYLAND_EDGE_TOP 1
#define WEX_WAYLAND_EDGE_BOTTOM 2
#define WEX_WAYLAND_EDGE_LEFT 3
#define WEX_WAYLAND_EDGE_RIGHT 4
#endif
