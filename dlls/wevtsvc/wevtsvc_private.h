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
    BOOL                 dirty;         /* written since the last flush (records or header) */
    ULONGLONG            created, written, read;  /* FILETIMEs, kept in the file's header */
    BOOL                 is_security;
};

/* store.c (the service's one thread) */
extern struct evt_log *store_get_log( const WCHAR *name, BOOL create );
extern void store_refresh_settings( struct evt_log *log );
extern void store_refresh_all(void);
extern DWORD store_append( struct evt_log *log, const BYTE *record, DWORD size );
extern DWORD store_clear( struct evt_log *log );
extern DWORD store_clear_through( struct evt_log *log, DWORD number );
extern struct evt_record *store_find( struct evt_log *log, DWORD number );
extern void store_flush_all(void);
extern void store_init( const WCHAR *dir );
extern DWORD store_oldest( const struct evt_log *log );
extern void store_touch_read( struct evt_log *log );
extern void store_information( struct evt_log *log, struct wex_evtlog_information *info );

/* wevtsvc.c */
extern void report_system_event( WORD type, DWORD id, const WCHAR *string );
extern void notify_log( struct evt_log *log );
extern BYTE *evt_build_record( const WCHAR *source, WORD type, WORD category, DWORD id, const BYTE *sid,
                               DWORD sid_size, const WCHAR *strings, DWORD strings_size, DWORD num_strings,
                               const BYTE *data, DWORD data_size, DWORD *size, DWORD *status );

/* journal.c (WineExplorer, docs/EVENTLOG_JOURNAL.md): the link to the journal bridge, session 0 only */
#define JOURNAL_KEY 1  /* the completion key of its posted events (connections use their address) */
extern void journal_start( HANDLE port, ULONG_PTR key );
extern void journal_stop(void);
extern void journal_dispatch( void *item );
extern void journal_record( const WCHAR *log, const WCHAR *source, WORD type, DWORD id, const BYTE *sid,
                            DWORD sid_size, const WCHAR *strings, DWORD num_strings, DWORD number );

#endif /* __WEVTSVC_PRIVATE_H */
