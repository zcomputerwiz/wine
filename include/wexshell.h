/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 */
#ifndef WEX_SHELL_H
#define WEX_SHELL_H

#include <windef.h>

#define WEX_SHELL_VERSION 1

typedef BOOL (WINAPI *WEX_SHELL_START)(DWORD version, HWND desktop, UINT width, UINT height,
                                     BOOL show_systray, BOOL no_tray_items);
typedef BOOL (WINAPI *WEX_SHELL_COMMAND)(DWORD version, WCHAR *command, int show);
typedef void (WINAPI *WEX_SHELL_STOP)(void);

#endif
