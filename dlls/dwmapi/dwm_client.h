/*
 * WineExplorer Desktop Environment - dwm_client.h
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __WINE_DWMAPI_DWM_CLIENT_H
#define __WINE_DWMAPI_DWM_CLIENT_H

#include "wine/dwm_protocol.h"

extern NTSTATUS dwm_client_set_attr( HWND hwnd, DWORD attribute, DWORD value );
extern NTSTATUS dwm_client_get_attr( HWND hwnd, DWORD attribute, BOOL *found, DWORD *value );
extern NTSTATUS dwm_client_thumb_set( HWND dest, UINT handle, HWND src, const struct dwm_thumb_set *set );
extern NTSTATUS dwm_client_thumb_del( HWND dest, UINT handle );

#endif
