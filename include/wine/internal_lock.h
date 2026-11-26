/*
 * WineExplorer: named locks that only Wine's own processes of one session use
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_WINE_INTERNAL_LOCK_H
#define __WINE_WINE_INTERNAL_LOCK_H

/* Wine's internal locks (an init-once mutex, a cache lock) live in \Sessions\<session>\WineObjects, which the server
 * makes for every session next to BaseNamedObjects (server/directory.c, create_session), not in the session's
 * \BaseNamedObjects: programs cannot reach them by a Win32 name, and for session 0, whose BaseNamedObjects is the
 * root \BaseNamedObjects that WineExplorer shares with the sessions (SESSION0_CROSS_SERVER.md, M1), they stay out of
 * the shared directory. The session is the PEB's, so a lock keeps the scope its unprefixed name had.
 *
 * Every user goes through these definitions: the format for code that builds the name itself (the Unix side), the
 * helpers for PE code. The helpers behave like CreateMutexW and CreateSemaphoreW: NULL and the last error on failure,
 * the last error ERROR_ALREADY_EXISTS when the object existed. */

#define WINE_INTERNAL_LOCK_DIR_FMT  "\\Sessions\\%u\\WineObjects\\"
#define WINE_INTERNAL_LOCK_DIR_FMTW L"\\Sessions\\%u\\WineObjects\\"

#ifndef WINE_UNIX_LIB

#include <stdio.h>

/* Include after windef.h, winbase.h and winternl.h. */

static inline BOOL wine_internal_lock_path( const WCHAR *name, WCHAR *buffer, size_t size )
{
    int len = _snwprintf( buffer, size, WINE_INTERNAL_LOCK_DIR_FMTW L"%s", NtCurrentTeb()->Peb->SessionId, name );

    if (len < 0 || (size_t)len >= size)
    {
        SetLastError( ERROR_FILENAME_EXCED_RANGE );
        return FALSE;
    }
    return TRUE;
}

static inline HANDLE wine_internal_lock_result( NTSTATUS status, HANDLE handle )
{
    if (status < 0)
    {
        SetLastError( RtlNtStatusToDosError( status ) );
        return NULL;
    }
    SetLastError( status == 0x40000000 /* STATUS_OBJECT_NAME_EXISTS */ ? ERROR_ALREADY_EXISTS : 0 );
    return handle;
}

static inline HANDLE wine_create_internal_mutex( BOOL owned, const WCHAR *name )
{
    WCHAR buffer[MAX_PATH + 64];
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING str;
    HANDLE handle = NULL;

    if (!wine_internal_lock_path( name, buffer, ARRAY_SIZE(buffer) )) return NULL;
    RtlInitUnicodeString( &str, buffer );
    InitializeObjectAttributes( &attr, &str, OBJ_OPENIF, NULL, NULL );
    return wine_internal_lock_result( NtCreateMutant( &handle, MUTEX_ALL_ACCESS, &attr, owned ), handle );
}

static inline HANDLE wine_create_internal_semaphore( LONG initial, LONG max, const WCHAR *name )
{
    WCHAR buffer[MAX_PATH + 64];
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING str;
    HANDLE handle = NULL;

    if (!wine_internal_lock_path( name, buffer, ARRAY_SIZE(buffer) )) return NULL;
    RtlInitUnicodeString( &str, buffer );
    InitializeObjectAttributes( &attr, &str, OBJ_OPENIF, NULL, NULL );
    return wine_internal_lock_result( NtCreateSemaphore( &handle, SEMAPHORE_ALL_ACCESS, &attr, initial, max ), handle );
}

#endif /* WINE_UNIX_LIB */

#endif /* __WINE_WINE_INTERNAL_LOCK_H */
