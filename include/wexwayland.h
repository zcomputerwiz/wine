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
#define WEX_WAYLAND_DESKTOP 2
#define WEX_WAYLAND_SHELL 3
#define WEX_WAYLAND_EDGE_NONE 0
#define WEX_WAYLAND_EDGE_TOP 1
#define WEX_WAYLAND_EDGE_BOTTOM 2
#define WEX_WAYLAND_EDGE_LEFT 3
#define WEX_WAYLAND_EDGE_RIGHT 4
#endif
