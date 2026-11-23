/*
 * WineExplorer Desktop Environment - dwm_test.h
 *
 * Shared by the DWM broker tests (dwm_attr, dwm_channel, dwm_thumb).
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

#ifndef __WINE_DWMAPI_TESTS_DWM_TEST_H
#define __WINE_DWMAPI_TESTS_DWM_TEST_H

#include "winreg.h"

/* Does a broker answer on this thread's desktop? With DwmService=1 the desktop manager starts dwm.exe while it
 * starts the test, so the broker gets up to 5 s to create its pipe; without the setting there is none to wait for.
 * echo is dwmapi's WexDwmpChannelEcho. */
static inline BOOL dwm_broker_answers( HRESULT (WINAPI *echo)(const void *, ULONG, void *, ULONG *) )
{
    DWORD value = 0, size = sizeof(value), start = GetTickCount();
    BOOL service;
    ULONG len;

    service = !RegGetValueW( HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"DwmService", RRF_RT_REG_DWORD,
                             NULL, &value, &size ) && value;
    for (;;)
    {
        len = 0;
        if (echo( NULL, 0, NULL, &len ) == S_OK) return TRUE;
        if (!service || GetTickCount() - start >= 5000) return FALSE;
        Sleep( 100 );
    }
}

#endif
