/*
 * Event Log Service (WineExplorer): internal definitions
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

#ifndef __WEVTSVC_PRIVATE_H
#define __WEVTSVC_PRIVATE_H

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "wine/list.h"
#include "wine/wex_eventlog.h"

/* one stored record: an EVENTLOGRECORD of size bytes */
struct evt_record
{
    struct list entry;
    DWORD       number;
    DWORD       size;
    BYTE        data[1];
};

#define EVT_RETENTION_NEVER 0xffffffff  /* "do not overwrite" */

/* a log: its records in memory and its file (the service is the only writer) */
struct evt_log
{
    struct list          entry;
    WCHAR                name[256];     /* the log's key under ...\Services\EventLog */
    WCHAR                path[MAX_PATH];
    HANDLE               file;
    struct list          records;       /* oldest first */
    DWORD                count;
    DWORD                bytes;         /* records and trailers in the file */
    DWORD                next;          /* the number of the next record */
    DWORD                max_size;      /* MaxSize */
    DWORD                retention;     /* Retention: 0 overwrite as needed, seconds, or EVT_RETENTION_NEVER */
    WCHAR               *custom_sd;     /* CustomSD (SDDL), or NULL */
    BOOL                 full_reported; /* the "log full" System event was written */
    BOOL                 dirty;         /* written since the last flush */
    unsigned int         generation;    /* counts writes, for notifications */
    BOOL                 is_security;
};

/* store.c; all under the service's lock */
extern struct evt_log *store_get_log( const WCHAR *name, BOOL create );
extern void store_refresh_settings( struct evt_log *log );
extern DWORD store_append( struct evt_log *log, const BYTE *record, DWORD size );
extern DWORD store_clear( struct evt_log *log );
extern struct evt_record *store_find( struct evt_log *log, DWORD number );
extern void store_flush_all(void);
extern void store_init( const WCHAR *dir );
extern DWORD store_oldest( const struct evt_log *log );

/* wevtsvc.c */
extern CRITICAL_SECTION evt_lock;
extern CONDITION_VARIABLE evt_written;
extern void report_system_event( WORD type, DWORD id, const WCHAR *string );

#endif /* __WEVTSVC_PRIVATE_H */
