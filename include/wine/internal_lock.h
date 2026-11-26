/*
 * WineExplorer: named locks that only Wine's own processes of one server use
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

#include <string.h>

/* Wine's internal locks (an init-once mutex, a cache lock) live in \KernelObjects, not in the session's
 * \BaseNamedObjects: programs cannot reach them by a Win32 name, and a server that shares \BaseNamedObjects with
 * other servers (WineExplorer's session 0) does not share them. The scope is the server, as it was the session's
 * namespace before. Like CreateMutexW and CreateSemaphoreW: NULL and the last error on failure, the last error
 * ERROR_ALREADY_EXISTS when the object existed. Include after windef.h, winbase.h and winternl.h. */

#define WINE_INTERNAL_LOCK_DIR L"\\KernelObjects\\"

static inline BOOL wine_internal_lock_name( const WCHAR *name, WCHAR *buffer, size_t size, UNICODE_STRING *str )
{
    size_t dir_len = ARRAY_SIZE( WINE_INTERNAL_LOCK_DIR ) - 1, len = wcslen( name );

    if (dir_len + len >= size)
    {
        SetLastError( ERROR_FILENAME_EXCED_RANGE );
        return FALSE;
    }
    memcpy( buffer, WINE_INTERNAL_LOCK_DIR, dir_len * sizeof(WCHAR) );
    memcpy( buffer + dir_len, name, (len + 1) * sizeof(WCHAR) );
    RtlInitUnicodeString( str, buffer );
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

    if (!wine_internal_lock_name( name, buffer, ARRAY_SIZE(buffer), &str )) return NULL;
    InitializeObjectAttributes( &attr, &str, OBJ_OPENIF, NULL, NULL );
    return wine_internal_lock_result( NtCreateMutant( &handle, MUTEX_ALL_ACCESS, &attr, owned ), handle );
}

static inline HANDLE wine_create_internal_semaphore( LONG initial, LONG max, const WCHAR *name )
{
    WCHAR buffer[MAX_PATH + 64];
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING str;
    HANDLE handle = NULL;

    if (!wine_internal_lock_name( name, buffer, ARRAY_SIZE(buffer), &str )) return NULL;
    InitializeObjectAttributes( &attr, &str, OBJ_OPENIF, NULL, NULL );
    return wine_internal_lock_result( NtCreateSemaphore( &handle, SEMAPHORE_ALL_ACCESS, &attr, initial, max ), handle );
}

#endif /* __WINE_WINE_INTERNAL_LOCK_H */
