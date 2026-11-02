/*
 * WineExplorer Desktop Environment - DWM broker supervision for the desktop manager
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

/*
 * Starts and supervises the DWM broker of the desktop this explorer manages
 * (docs/DWM_ARCHITECTURE.md section 3). Off unless HKCU\Software\WineExplorer\Shell\DwmService is non-zero.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "winternl.h"
#include "wine/dwm_protocol.h"
#include "wine/debug.h"
#include "explorer_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(explorer);

#define DWM_MAX_CRASHES  3      /* per minute, then give up */
#define DWM_READY_MS     3000   /* a broker that lives this long counts as started */

static HANDLE dwm_stop_event, dwm_thread;
static HDESK dwm_desktop;

static BOOL dwm_service_enabled(void)
{
    DWORD value = 0, size = sizeof(value);

    return !RegGetValueW( HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"DwmService",
                          RRF_RT_REG_DWORD, NULL, &value, &size ) && value;
}

static HANDLE launch_broker(void)
{
    struct dwm_channel_call call = { .op = DWM_CHANNEL_OP_GET_CAPABILITY };
    WCHAR cmdline[128], station[MAX_PATH], desktop[MAX_PATH], full[2 * MAX_PATH + 2];
    STARTUPINFOW si = { sizeof(si) };
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    PROCESS_INFORMATION pi;
    HANDLE read_pipe, write_pipe;
    BOOL wow64 = FALSE, ok;
    PVOID redirect = NULL;

    if (NtSetInformationProcess( GetCurrentProcess(), ProcessWineDwmChannel, &call, sizeof(call) )) return NULL;

    if (!GetUserObjectInformationW( GetProcessWindowStation(), UOI_NAME, station, sizeof(station), NULL ) ||
        !GetUserObjectInformationW( dwm_desktop, UOI_NAME, desktop, sizeof(desktop), NULL )) return NULL;
    swprintf( full, ARRAY_SIZE(full), L"%s\\%s", station, desktop );
    si.lpDesktop = full;

    /* the capability goes through the child's stdin, not the command line other processes can read */
    if (!CreatePipe( &read_pipe, &write_pipe, &sa, 0 )) return NULL;
    SetHandleInformation( write_pipe, HANDLE_FLAG_INHERIT, 0 );
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = read_pipe;
    si.hStdOutput = GetStdHandle( STD_OUTPUT_HANDLE );
    si.hStdError  = GetStdHandle( STD_ERROR_HANDLE );

    swprintf( cmdline, ARRAY_SIZE(cmdline), L"\"C:\\windows\\system32\\dwm.exe\" /parent=%lu", GetCurrentProcessId() );
    /* a 32-bit explorer must still reach the native broker */
    if (IsWow64Process( GetCurrentProcess(), &wow64 ) && wow64) Wow64DisableWow64FsRedirection( &redirect );
    ok = CreateProcessW( NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi );
    if (wow64) Wow64RevertWow64FsRedirection( redirect );
    CloseHandle( read_pipe );
    if (ok)
    {
        char text[17];
        DWORD written;

        sprintf( text, "%016llx", (unsigned long long)call.capability );
        WriteFile( write_pipe, text, 16, &written, NULL );
        CloseHandle( pi.hThread );
    }
    CloseHandle( write_pipe );
    return ok ? pi.hProcess : NULL;
}

static DWORD WINAPI dwm_supervisor( void *arg )
{
    static const DWORD backoff[] = { 250, 1000, 4000 };
    ULONGLONG crash_times[DWM_MAX_CRASHES] = { 0 };
    unsigned int crashes = 0;

    if (!SetThreadDesktop( dwm_desktop )) return 1;
    for (;;)
    {
        HANDLE process = launch_broker();
        ULONGLONG start = GetTickCount64(), now;
        DWORD wait;

        if (process)
        {
            if (WaitForSingleObject( dwm_stop_event, 0 ) == WAIT_OBJECT_0)  /* stopped while launching */
            {
                TerminateProcess( process, 0 );
                CloseHandle( process );
                return 0;
            }
            HANDLE objs[2] = { process, dwm_stop_event };
            wait = WaitForMultipleObjects( 2, objs, FALSE, INFINITE );
            if (wait == WAIT_OBJECT_0 + 1)
            {
                TerminateProcess( process, 0 );
                CloseHandle( process );
                return 0;
            }
            CloseHandle( process );
        }
        now = GetTickCount64();
        if (now - start >= DWM_READY_MS) crashes = 0;
        crash_times[crashes % DWM_MAX_CRASHES] = now;
        if (++crashes >= DWM_MAX_CRASHES && now - crash_times[crashes % DWM_MAX_CRASHES] < 60000)
        {
            ERR( "dwm.exe keeps failing, giving up\n" );
            return 1;
        }
        if (WaitForSingleObject( dwm_stop_event, backoff[min( crashes, ARRAY_SIZE(backoff) ) - 1] ) == WAIT_OBJECT_0)
            return 0;
    }
}

void start_dwm_broker(void)
{
    if (dwm_thread || !dwm_service_enabled()) return;
    if (!(dwm_desktop = GetThreadDesktop( GetCurrentThreadId() ))) return;
    if (!(dwm_stop_event = CreateEventW( NULL, TRUE, FALSE, NULL ))) return;
    dwm_thread = CreateThread( NULL, 0, dwm_supervisor, NULL, 0, NULL );
}

void stop_dwm_broker(void)
{
    if (!dwm_thread) return;
    SetEvent( dwm_stop_event );
    WaitForSingleObject( dwm_thread, 5000 );
    CloseHandle( dwm_thread );
    CloseHandle( dwm_stop_event );
    dwm_thread = dwm_stop_event = NULL;
}
