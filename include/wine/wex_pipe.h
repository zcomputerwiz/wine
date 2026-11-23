/*
 * WineExplorer Desktop Environment - wex_pipe.h
 *
 * The transport of the project's per-user brokers (DWM first; the token broker and wexelevate later): a local
 * message-mode named pipe, a client connect and an overlapped transaction, both with timeouts, and the client's
 * check that the pipe's server is the expected system program with an ordinary token. Caller policy (who may ask
 * what) and pipe names stay with each broker. Static inline: the users live in different trees and modules.
 * Users import advapi32 and kernelbase (kernel32).
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

#ifndef __WINE_WEX_PIPE_H
#define __WINE_WEX_PIPE_H

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "sddl.h"
#include "aclapi.h"

/* A server instance: message mode, local clients only, the descriptor from sddl. first: this must be the pipe's first
 * instance (FILE_FLAG_FIRST_PIPE_INSTANCE), so a broker that finds the name taken does not serve. */
static inline HANDLE wex_pipe_create( const WCHAR *name, const WCHAR *sddl, BOOL first, DWORD max_message )
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    HANDLE pipe;

    if (sddl && !ConvertStringSecurityDescriptorToSecurityDescriptorW( sddl, SDDL_REVISION_1,
                                                                       &sa.lpSecurityDescriptor, NULL ))
        return INVALID_HANDLE_VALUE;
    pipe = CreateNamedPipeW( name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             PIPE_UNLIMITED_INSTANCES, max_message, max_message, 0, &sa );
    LocalFree( sa.lpSecurityDescriptor );
    return pipe;
}

/* One read or write on an overlapped pipe handle, bounded by timeout_ms (INFINITE for none). event is a manual-reset
 * event of the caller's. Returns a Win32 error; on a timeout the I/O is cancelled and ERROR_TIMEOUT returned. */
static inline DWORD wex_pipe_io( HANDLE pipe, BOOL write, void *buffer, DWORD size, DWORD *done, HANDLE event,
                                 DWORD timeout_ms )
{
    OVERLAPPED ov = { 0 };
    DWORD err = ERROR_SUCCESS;
    BOOL ret;

    *done = 0;
    ov.hEvent = event;
    ResetEvent( event );
    ret = write ? WriteFile( pipe, buffer, size, NULL, &ov ) : ReadFile( pipe, buffer, size, NULL, &ov );
    if (!ret && (err = GetLastError()) == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject( event, timeout_ms ) != WAIT_OBJECT_0)
        {
            CancelIoEx( pipe, &ov );
            GetOverlappedResult( pipe, &ov, done, TRUE );
            return ERROR_TIMEOUT;
        }
        err = ERROR_SUCCESS;
    }
    if (!err && !GetOverlappedResult( pipe, &ov, done, FALSE )) err = GetLastError();
    return err;
}

/* Send a request and read its reply (one message each way). *reply_size: the reply's size. */
static inline DWORD wex_pipe_transact( HANDLE pipe, const void *request, DWORD request_size, void *reply,
                                       DWORD reply_max, DWORD *reply_size, HANDLE event, DWORD timeout_ms )
{
    DWORD err, done;

    *reply_size = 0;
    if ((err = wex_pipe_io( pipe, TRUE, (void *)request, request_size, &done, event, timeout_ms ))) return err;
    if (done != request_size) return ERROR_WRITE_FAULT;
    return wex_pipe_io( pipe, FALSE, reply, reply_max, reply_size, event, timeout_ms );
}

/* Open a client end: overlapped, message mode, not inheritable (the connection dies with the process). Waits up to
 * timeout_ms for a free instance. Returns INVALID_HANDLE_VALUE with the last error set. */
static inline HANDLE wex_pipe_connect( const WCHAR *name, DWORD timeout_ms )
{
    DWORD mode = PIPE_READMODE_MESSAGE, start = GetTickCount();
    HANDLE pipe;

    for (;;)
    {
        DWORD elapsed;

        pipe = CreateFileW( name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL );
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
        elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms || !WaitNamedPipeW( name, timeout_ms - elapsed ))
        {
            SetLastError( ERROR_PIPE_BUSY );
            return INVALID_HANDLE_VALUE;
        }
    }
    if (!SetNamedPipeHandleState( pipe, &mode, NULL, NULL ))
    {
        DWORD err = GetLastError();
        CloseHandle( pipe );
        SetLastError( err );
        return INVALID_HANDLE_VALUE;
    }
    return pipe;
}

/* An ordinary user's token: not an AppContainer, not restricted, integrity medium or above. */
static inline BOOL wex_token_is_ordinary( HANDLE token )
{
    BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    TOKEN_MANDATORY_LABEL *label = (TOKEN_MANDATORY_LABEL *)buffer;
    DWORD container = 1, restricted = 1, len;
    PSID sid;

    if (!GetTokenInformation( token, TokenIsAppContainer, &container, sizeof(container), &len ) || container)
        return FALSE;
    if (!GetTokenInformation( token, TokenIsRestricted, &restricted, sizeof(restricted), &len ) || restricted)
        return FALSE;
    if (!GetTokenInformation( token, TokenIntegrityLevel, buffer, sizeof(buffer), &len )) return FALSE;
    sid = label->Label.Sid;
    if (!IsValidSid( sid ) || !*GetSidSubAuthorityCount( sid )) return FALSE;
    return *GetSidSubAuthority( sid, *GetSidSubAuthorityCount( sid ) - 1 ) >= SECURITY_MANDATORY_MEDIUM_RID;
}

/* Is the pipe's server the system program image (for example L"dwm.exe": system32 or syswow64 of the machine's
 * Windows directory) running with an ordinary token? Once per connection, never per message. A sandboxed process can
 * create the name first, or start the real image suspended with its own token: the image alone does not prove the
 * server. Wine's sandboxes are the same Unix user, so this is defence in depth. Any failed query is a refusal.
 * Windows needs only PROCESS_QUERY_LIMITED_INFORMATION to open a process's token (measured on Windows 11; the broker
 * grants it to AppContainers, wex_pipe_grant_appcontainers). Wine's server asks for PROCESS_QUERY_INFORMATION while
 * elevation is enforced, so that is tried when the token cannot be opened. */
static inline BOOL wex_pipe_server_is( HANDLE pipe, const WCHAR *image_name )
{
    static const WCHAR *const dirs[] = { L"\\system32\\", L"\\syswow64\\" };
    WCHAR image[MAX_PATH], expected[MAX_PATH];
    DWORD size = ARRAY_SIZE(image);
    ULONG pid = 0;
    HANDLE process, token = NULL;
    UINT windir, i;
    BOOL ordinary;

    if (!GetNamedPipeServerProcessId( pipe, &pid ) || !pid) return FALSE;
    if (!(process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid ))) return FALSE;
    if (!OpenProcessToken( process, TOKEN_QUERY, &token ))
    {
        HANDLE full = OpenProcess( PROCESS_QUERY_INFORMATION, FALSE, pid );
        if (full && !OpenProcessToken( full, TOKEN_QUERY, &token )) token = NULL;
        if (full) CloseHandle( full );
    }
    ordinary = token && wex_token_is_ordinary( token );
    if (token) CloseHandle( token );
    if (!ordinary || !QueryFullProcessImageNameW( process, 0, image, &size ))
    {
        CloseHandle( process );
        return FALSE;
    }
    CloseHandle( process );
    if (!(windir = GetWindowsDirectoryW( expected, MAX_PATH )) || windir >= MAX_PATH) return FALSE;
    for (i = 0; i < ARRAY_SIZE(dirs); i++)
    {
        if (windir + wcslen( dirs[i] ) + wcslen( image_name ) >= MAX_PATH) continue;
        wcscpy( expected + windir, dirs[i] );
        wcscat( expected, image_name );
        if (!wcsicmp( image, expected )) return TRUE;
    }
    return FALSE;
}

/* For a broker: let every AppContainer (ALL APPLICATION PACKAGES, S-1-15-2-1) query this process with the limited
 * right and read its token, the minimum for an AppContainer client to run wex_pipe_server_is (measured on Windows 11:
 * without these grants an AppContainer cannot open a medium process at all). */
static inline void wex_pipe_grant_appcontainers(void)
{
    EXPLICIT_ACCESSW ea = { 0 };
    PSECURITY_DESCRIPTOR sd;
    PACL dacl, new_dacl;
    PSID all_packages;
    HANDLE objects[2] = { GetCurrentProcess(), NULL };
    DWORD rights[2] = { PROCESS_QUERY_LIMITED_INFORMATION, TOKEN_QUERY };
    unsigned int i;

    if (!ConvertStringSidToSidW( L"S-1-15-2-1", &all_packages )) return;
    if (!OpenProcessToken( GetCurrentProcess(), READ_CONTROL | WRITE_DAC, &objects[1] )) objects[1] = NULL;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = all_packages;
    for (i = 0; i < ARRAY_SIZE(objects); i++)
    {
        if (!objects[i]) continue;
        sd = NULL;
        new_dacl = NULL;
        ea.grfAccessPermissions = rights[i];
        /* an object without a DACL already lets everyone in: a DACL made of this one ACE would lock its owner out */
        if (!GetSecurityInfo( objects[i], SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd ) &&
            dacl && !SetEntriesInAclW( 1, &ea, dacl, &new_dacl ))
            SetSecurityInfo( objects[i], SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, new_dacl, NULL );
        LocalFree( new_dacl );
        LocalFree( sd );
    }
    if (objects[1]) CloseHandle( objects[1] );
    LocalFree( all_packages );
}

#endif /* __WINE_WEX_PIPE_H */
