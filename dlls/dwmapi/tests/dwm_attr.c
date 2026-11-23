/*
 * WineExplorer Desktop Environment - dwm_attr.c
 *
 * Conformance tests for cross-process window attribute state held by the DWM broker (Phase P3).
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

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "tlhelp32.h"
#include "dwmapi.h"
#include "wine/test.h"

#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

static HRESULT (WINAPI *pWexDwmpChannelEcho)(const void *in, ULONG in_len, void *out, ULONG *out_len);

static DWORD run_child( const WCHAR *exe, HWND hwnd, DWORD expected, DWORD attr )
{
    WCHAR cmd[MAX_PATH + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;
    BOOL ret;

    memset( &si, 0, sizeof(si) );
    si.cb = sizeof(si);

    if (attr == DWMWA_CAPTION_COLOR)
        wsprintfW( cmd, L"\"%s\" dwm_attr child %lx %lx", exe, HandleToUlong( hwnd ), expected );
    else
        wsprintfW( cmd, L"\"%s\" dwm_attr child %lx %lx %lx", exe, HandleToUlong( hwnd ), expected, attr );

    ret = CreateProcessW( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi );
    ok( ret, "CreateProcessW failed, error %lu\n", GetLastError() );
    if (ret)
    {
        /* a reader that hangs fails its phase and is ended, never left behind */
        if (WaitForSingleObject( pi.hProcess, 10000 ))
        {
            ok( 0, "reader of attribute %#lx timed out\n", attr );
            TerminateProcess( pi.hProcess, 1 );
            WaitForSingleObject( pi.hProcess, 5000 );
        }
        else GetExitCodeProcess( pi.hProcess, &exit_code );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
    }
    return exit_code;
}

static DWORD parent_of( HANDLE snapshot, DWORD pid )
{
    PROCESSENTRY32W entry = { sizeof(entry) };

    if (!Process32FirstW( snapshot, &entry )) return 0;
    do if (entry.th32ProcessID == pid) return entry.th32ParentProcessID;
    while (Process32NextW( snapshot, &entry ));
    return 0;
}

/* Ends the broker of this test's desktop: the dwm.exe started by the desktop manager that started this test
 * (explorer /desktop=NAME cmd /c test). FALSE when the test was started some other way. */
static BOOL kill_own_broker(void)
{
    PROCESSENTRY32W entry = { sizeof(entry) };
    HANDLE snapshot, process;
    DWORD manager;
    BOOL killed = FALSE;

    snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
    if (snapshot == INVALID_HANDLE_VALUE) return FALSE;
    manager = parent_of( snapshot, parent_of( snapshot, GetCurrentProcessId() ) );
    if (manager && Process32FirstW( snapshot, &entry ))
    {
        do
        {
            if (entry.th32ParentProcessID != manager || wcsicmp( entry.szExeFile, L"dwm.exe" )) continue;
            if (!(process = OpenProcess( PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID ))) continue;
            killed = TerminateProcess( process, 1 );
            WaitForSingleObject( process, 5000 );
            CloseHandle( process );
        } while (Process32NextW( snapshot, &entry ));
    }
    CloseHandle( snapshot );
    return killed;
}

/* The desktop manager starts a new broker, which holds nothing; the setter's first call that reaches it hands it
 * the process's attributes (synchronously, in that call), so another process reads them again. One bounded phase
 * waits for the new broker, its echo doing the replay; then one cross-process read. */
static void test_broker_restart( const WCHAR *exe, HWND hwnd )
{
    DWORD exit_code, start;
    ULONG len = 0;
    HRESULT hr = E_FAIL;

    if (!kill_own_broker())
    {
        skip( "not started by a desktop manager with a broker\n" );
        return;
    }
    /* the desktop manager restarts it after 250 ms */
    for (start = GetTickCount(); GetTickCount() - start < 5000; Sleep( 100 ))
        if ((hr = pWexDwmpChannelEcho( NULL, 0, NULL, &len )) == S_OK) break;
    ok( hr == S_OK, "the broker did not come back within 5 s: %#lx\n", hr );
    if (hr != S_OK) return;
    exit_code = run_child( exe, hwnd, 3, DWMWA_WINDOW_CORNER_PREFERENCE );
    ok( exit_code == 0, "the restarted broker did not get the corner preference back\n" );
}

START_TEST(dwm_attr)
{
    char **argv;
    int argc;
    HMODULE dwmapi;
    ULONG out_len = 0;
    HRESULT hr;
    WCHAR exe[MAX_PATH];
    HWND hwnd, hwnd2;
    DWORD val, exit_code, len;

    argc = winetest_get_mainargs( &argv );
    if (argc >= 3 && !strcmp( argv[2], "child" ))
    {
        HWND child_hwnd;
        DWORD expected, attr, v = 0;
        HRESULT child_hr;

        if (argc < 5) ExitProcess( 1 );
        child_hwnd = (HWND)UlongToHandle( strtoul( argv[3], NULL, 16 ) );
        expected = strtoul( argv[4], NULL, 16 );
        attr = (argc > 5) ? strtoul( argv[5], NULL, 16 ) : DWMWA_CAPTION_COLOR;
        if (attr == 0x33) attr = DWMWA_WINDOW_CORNER_PREFERENCE;

        /* the broker answers reads only for a process with a window on its desktop */
        CreateWindowExW( 0, L"static", L"dwm_attr_child", WS_POPUP, 0, 0, 10, 10, 0, 0, 0, NULL );
        child_hr = DwmGetWindowAttribute( child_hwnd, attr, &v, 4 );
        ExitProcess( (child_hr == S_OK && v == expected) ? 0 : 1 );
    }

    dwmapi = GetModuleHandleW( L"dwmapi.dll" );
    if (!dwmapi) dwmapi = LoadLibraryW( L"dwmapi.dll" );
    pWexDwmpChannelEcho = (void *)GetProcAddress( dwmapi, "WexDwmpChannelEcho" );
    if (!pWexDwmpChannelEcho)
    {
        win_skip( "WexDwmpChannelEcho not available (not a Wine build).\n" );
        return;
    }

    hr = pWexDwmpChannelEcho( NULL, 0, NULL, &out_len );
    if (hr != S_OK)
    {
        skip( "needs a desktop with a DWM broker\n" );
        return;
    }

    len = GetModuleFileNameW( NULL, exe, ARRAY_SIZE(exe) );
    ok( len > 0, "GetModuleFileNameW failed, error %lu\n", GetLastError() );

    hwnd = CreateWindowExW( 0, L"static", L"dwm_attr_test", WS_POPUP | WS_VISIBLE,
                            0, 0, 100, 100, 0, 0, 0, NULL );
    ok( !!hwnd, "CreateWindowExW failed, error %lu\n", GetLastError() );

    val = 0x00112233;
    hr = DwmSetWindowAttribute( hwnd, DWMWA_CAPTION_COLOR, &val, sizeof(val) );
    ok( hr == S_OK, "DwmSetWindowAttribute DWMWA_CAPTION_COLOR failed: %#lx\n", hr );

    val = 3;
    hr = DwmSetWindowAttribute( hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &val, sizeof(val) );
    ok( hr == S_OK, "DwmSetWindowAttribute DWMWA_WINDOW_CORNER_PREFERENCE failed: %#lx\n", hr );

    exit_code = run_child( exe, hwnd, 0x00112233, DWMWA_CAPTION_COLOR );
    ok( exit_code == 0, "child failed to read caption color, exit_code %lu\n", exit_code );

    exit_code = run_child( exe, hwnd, 3, DWMWA_WINDOW_CORNER_PREFERENCE );
    ok( exit_code == 0, "child failed to read corner preference, exit_code %lu\n", exit_code );

    val = DWMWA_COLOR_DEFAULT;
    hr = DwmSetWindowAttribute( hwnd, DWMWA_CAPTION_COLOR, &val, sizeof(val) );
    ok( hr == S_OK, "DwmSetWindowAttribute DWMWA_COLOR_DEFAULT failed: %#lx\n", hr );

    exit_code = run_child( exe, hwnd, DWMWA_COLOR_DEFAULT, DWMWA_CAPTION_COLOR );
    ok( exit_code == 0, "child failed to read reset caption color, exit_code %lu\n", exit_code );

    test_broker_restart( exe, hwnd );

    DestroyWindow( hwnd );

    val = 0;
    hr = DwmGetWindowAttribute( hwnd, DWMWA_CAPTION_COLOR, &val, sizeof(val) );
    ok( hr == E_HANDLE, "expected E_HANDLE on destroyed window, got %#lx\n", hr );

    hwnd2 = CreateWindowExW( 0, L"static", L"dwm_attr_test2", WS_POPUP | WS_VISIBLE,
                             0, 0, 100, 100, 0, 0, 0, NULL );
    ok( !!hwnd2, "CreateWindowExW failed, error %lu\n", GetLastError() );
    ok( hwnd2 != hwnd, "expected new HWND, got reused handle\n" );

    exit_code = run_child( exe, hwnd2, DWMWA_COLOR_DEFAULT, DWMWA_CAPTION_COLOR );
    ok( exit_code == 0, "child failed to read default caption color on new window, exit_code %lu\n", exit_code );

    DestroyWindow( hwnd2 );
}
