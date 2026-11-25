/*
 * Event Log Service (WineExplorer): the log files
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

/* docs/EVENTLOG_DESIGN.md, "Storage" (E5). One file per log, our own format: a 32-byte header, then records, each an
 * EVENTLOGRECORD followed by a trailer (a CRC32 of the record, then its record number). A load accepts the run of
 * records from the start whose CRCs are valid and whose numbers rise by one, and cuts the rest (a torn record, stale
 * records after the end). Records are appended; when the oldest records go (overwrite as needed, or older than the
 * retention) or the log is cleared, the file is written again in place, never replaced by a rename, so that it keeps
 * its owner and mode. The service flushes at most once a second. */

#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winreg.h"
#include "sddl.h"
#include "wine/debug.h"

#include "wevtsvc_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(eventlog);

#define EVT_FILE_MAGIC   "WXEVLOG1"
#define EVT_FILE_VERSION 1
#define EVT_MAX_FILE     (256 * 1024 * 1024)  /* a larger file is read up to this size */
#define EVT_DEFAULT_MAX  0x80000              /* MaxSize when the key has none: 512 KiB, as XP */
#define EVT_MIN_MAX      0x10000

struct evt_file_header
{
    char      magic[8];
    DWORD     version;
    DWORD     next;        /* the number of the next record (kept across a clear) */
    ULONGLONG created;     /* FILETIMEs: the log's creation, its last write (or clear), its last read */
    ULONGLONG written;
    ULONGLONG read;
    ULONGLONG reserved;
};  /* 48 bytes */

static ULONGLONG now_filetime(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime( &ft );
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

struct evt_trailer
{
    DWORD crc;
    DWORD number;
};

static WCHAR store_dir[MAX_PATH];
static struct list logs = LIST_INIT( logs );

static const WCHAR eventlog_key[] = L"System\\CurrentControlSet\\Services\\EventLog\\";

void store_init( const WCHAR *dir )
{
    lstrcpynW( store_dir, dir, ARRAY_SIZE(store_dir) );
}

/* a log name we accept as a file name: a registry key name without path characters */
static BOOL valid_log_name( const WCHAR *name )
{
    const WCHAR *p;

    if (!name[0] || wcslen( name ) > 200 || !wcscmp( name, L"." ) || !wcscmp( name, L".." )) return FALSE;
    for (p = name; *p; p++) if (*p < 0x20 || wcschr( L"\\/:*?\"<>|", *p )) return FALSE;
    return TRUE;
}

static void file_name( const WCHAR *log, WCHAR *path )
{
    const WCHAR *base = log;

    if (!wcsicmp( log, L"Application" )) base = L"AppEvent";
    else if (!wcsicmp( log, L"System" )) base = L"SysEvent";
    else if (!wcsicmp( log, L"Security" )) base = L"SecEvent";
    swprintf( path, MAX_PATH, L"%s\\%s.evt", store_dir, base );
}

static HKEY open_log_key( const WCHAR *name )
{
    WCHAR path[300];
    HKEY key;

    swprintf( path, ARRAY_SIZE(path), L"%s%s", eventlog_key, name );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key )) return NULL;
    return key;
}

/* MaxSize, Retention and CustomSD, from session 0's registry (never from a client) */
void store_refresh_settings( struct evt_log *log )
{
    DWORD value, size, type;
    HKEY key;

    log->max_size = EVT_DEFAULT_MAX;
    log->retention = 0;
    free( log->custom_sd );
    log->custom_sd = NULL;
    if (!(key = open_log_key( log->name ))) return;
    size = sizeof(value);
    if (!RegQueryValueExW( key, L"MaxSize", NULL, &type, (BYTE *)&value, &size ) && type == REG_DWORD)
        log->max_size = value < EVT_MIN_MAX ? EVT_MIN_MAX : value;
    size = sizeof(value);
    if (!RegQueryValueExW( key, L"Retention", NULL, &type, (BYTE *)&value, &size ) && type == REG_DWORD)
        log->retention = value;
    if (!RegQueryValueExW( key, L"CustomSD", NULL, &type, NULL, &size ) && (type == REG_SZ || type == REG_EXPAND_SZ) &&
        size && size < 65536 && (log->custom_sd = calloc( 1, size + sizeof(WCHAR) )))
    {
        if (RegQueryValueExW( key, L"CustomSD", NULL, &type, (BYTE *)log->custom_sd, &size ))
        {
            free( log->custom_sd );
            log->custom_sd = NULL;
        }
    }
    RegCloseKey( key );
}

static void free_records( struct evt_log *log )
{
    struct evt_record *rec, *next;

    LIST_FOR_EACH_ENTRY_SAFE( rec, next, &log->records, struct evt_record, entry )
    {
        list_remove( &rec->entry );
        free( rec );
    }
    log->count = 0;
    log->bytes = 0;
}

/* a record as it is stored: a plausible EVENTLOGRECORD of exactly size bytes */
static BOOL record_valid( const BYTE *data, DWORD size )
{
    const EVENTLOGRECORD *rec = (const EVENTLOGRECORD *)data;

    return size >= sizeof(*rec) + sizeof(DWORD) && size <= WEX_EVTLOG_MAX_RECORD && !(size % 4) &&
           rec->Length == size && rec->Reserved == 0x654c664c &&
           *(const DWORD *)(data + size - sizeof(DWORD)) == size;
}

/* the valid rising run from the start of the file's records; the rest is cut (E5) */
static void load_records( struct evt_log *log, const BYTE *buf, DWORD size )
{
    DWORD pos = sizeof(struct evt_file_header), expect = 0, cut = 0;

    while (size - pos >= sizeof(EVENTLOGRECORD))
    {
        const EVENTLOGRECORD *rec = (const EVENTLOGRECORD *)(buf + pos);
        struct evt_trailer trailer;
        struct evt_record *stored;
        DWORD len = rec->Length;

        if (len > size - pos || size - pos - len < sizeof(trailer) || !record_valid( buf + pos, len )) break;
        memcpy( &trailer, buf + pos + len, sizeof(trailer) );
        if (trailer.crc != RtlComputeCrc32( 0, buf + pos, len ) || trailer.number != rec->RecordNumber ||
            (expect && trailer.number != expect) || !trailer.number)
            break;
        if (!(stored = malloc( offsetof( struct evt_record, data[len] )))) break;
        stored->number = trailer.number;
        stored->size = len;
        memcpy( stored->data, buf + pos, len );
        list_add_tail( &log->records, &stored->entry );
        log->count++;
        log->bytes += len + sizeof(trailer);
        expect = trailer.number + 1;
        pos += len + sizeof(trailer);
    }
    if (pos < size) cut = size - pos;
    if (expect && expect > log->next) log->next = expect;
    if (cut) WARN( "%s: %lu bytes after record %lu cut (torn or stale)\n", debugstr_w(log->name), cut, expect - 1 );
}

/* the header at the start of the file */
static DWORD write_header( struct evt_log *log )
{
    struct evt_file_header hdr;
    LARGE_INTEGER zero = {{ 0 }};
    DWORD written;

    memset( &hdr, 0, sizeof(hdr) );
    memcpy( hdr.magic, EVT_FILE_MAGIC, sizeof(hdr.magic) );
    hdr.version = EVT_FILE_VERSION;
    hdr.next = log->next;
    hdr.created = log->created;
    hdr.written = log->written;
    hdr.read = log->read;
    if (!SetFilePointerEx( log->file, zero, NULL, FILE_BEGIN ) ||
        !WriteFile( log->file, &hdr, sizeof(hdr), &written, NULL ) || written != sizeof(hdr))
        return GetLastError();
    return ERROR_SUCCESS;
}

/* write the whole file again, in place */
static DWORD rewrite_file( struct evt_log *log )
{
    struct evt_record *rec;
    DWORD written, status;

    if ((status = write_header( log ))) return status;
    LIST_FOR_EACH_ENTRY( rec, &log->records, struct evt_record, entry )
    {
        struct evt_trailer trailer = { RtlComputeCrc32( 0, rec->data, rec->size ), rec->number };

        if (!WriteFile( log->file, rec->data, rec->size, &written, NULL ) || written != rec->size ||
            !WriteFile( log->file, &trailer, sizeof(trailer), &written, NULL ) || written != sizeof(trailer))
            return GetLastError();
    }
    if (!SetEndOfFile( log->file )) return GetLastError();
    FlushFileBuffers( log->file );
    log->dirty = FALSE;
    return ERROR_SUCCESS;
}

/* the log of a name (Application, System, Security, or a log registered under the EventLog key); create: load it */
struct evt_log *store_get_log( const WCHAR *name, BOOL create )
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    struct evt_log *log;
    LARGE_INTEGER size;
    BYTE *buf = NULL;
    DWORD read = 0;
    HKEY key;

    LIST_FOR_EACH_ENTRY( log, &logs, struct evt_log, entry )
        if (!wcsicmp( log->name, name )) return log;
    if (!create || !valid_log_name( name )) return NULL;
    if (wcsicmp( name, L"Application" ) && wcsicmp( name, L"System" ) && wcsicmp( name, L"Security" ))
    {
        if (!(key = open_log_key( name ))) return NULL;
        RegCloseKey( key );
    }
    if (!(log = calloc( 1, sizeof(*log) ))) return NULL;
    lstrcpynW( log->name, name, ARRAY_SIZE(log->name) );
    log->is_security = !wcsicmp( name, L"Security" );
    list_init( &log->records );
    log->next = 1;
    file_name( log->name, log->path );
    /* the service's account only: no session reads the file directly */
    ConvertStringSecurityDescriptorToSecurityDescriptorW( L"D:P(A;;FA;;;SY)", SDDL_REVISION_1,
                                                          &sa.lpSecurityDescriptor, NULL );
    log->file = CreateFileW( log->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL );
    LocalFree( sa.lpSecurityDescriptor );
    if (log->file == INVALID_HANDLE_VALUE)
    {
        ERR( "cannot open %s: %lu\n", debugstr_w(log->path), GetLastError() );
        free( log );
        return NULL;
    }
    if (GetFileSizeEx( log->file, &size ) && size.QuadPart >= (LONGLONG)sizeof(struct evt_file_header))
    {
        DWORD len = size.QuadPart > EVT_MAX_FILE ? EVT_MAX_FILE : (DWORD)size.QuadPart;
        struct evt_file_header hdr;

        if ((buf = malloc( len )) && ReadFile( log->file, buf, len, &read, NULL ) && read == len)
        {
            memcpy( &hdr, buf, sizeof(hdr) );
            if (!memcmp( hdr.magic, EVT_FILE_MAGIC, sizeof(hdr.magic) ) && hdr.version == EVT_FILE_VERSION)
            {
                if (hdr.next) log->next = hdr.next;
                log->created = hdr.created;
                log->written = hdr.written;
                log->read = hdr.read;
                load_records( log, buf, len );
            }
            else WARN( "%s is not one of our log files: started empty\n", debugstr_w(log->path) );
        }
        free( buf );
    }
    if (!log->created) log->created = log->written = now_filetime();
    store_refresh_settings( log );
    /* what was cut on load (or a new file) is gone from the file too */
    rewrite_file( log );
    list_add_tail( &logs, &log->entry );
    TRACE( "%s: %lu records, next %lu\n", debugstr_w(log->name), log->count, log->next );
    return log;
}

DWORD store_oldest( const struct evt_log *log )
{
    if (list_empty( &log->records )) return 0;
    return LIST_ENTRY( list_head( &log->records ), struct evt_record, entry )->number;
}

struct evt_record *store_find( struct evt_log *log, DWORD number )
{
    struct evt_record *rec;

    LIST_FOR_EACH_ENTRY( rec, &log->records, struct evt_record, entry ) if (rec->number == number) return rec;
    return NULL;
}

/* room for need more bytes under the log's policy, dropping the oldest records the policy allows; whether some went */
static DWORD make_room( struct evt_log *log, DWORD need, BOOL *dropped )
{
    DWORD limit = log->max_size - sizeof(struct evt_file_header), now = (DWORD)time( NULL );

    *dropped = FALSE;
    if (need > limit) return ERROR_LOG_FILE_FULL;
    while (log->bytes + need > limit)
    {
        struct evt_record *oldest = LIST_ENTRY( list_head( &log->records ), struct evt_record, entry );

        if (log->retention == EVT_RETENTION_NEVER) return ERROR_LOG_FILE_FULL;
        if (log->retention && now - ((EVENTLOGRECORD *)oldest->data)->TimeWritten < log->retention)
            return ERROR_LOG_FILE_FULL;
        list_remove( &oldest->entry );
        log->count--;
        log->bytes -= oldest->size + sizeof(struct evt_trailer);
        free( oldest );
        *dropped = TRUE;
    }
    return ERROR_SUCCESS;
}

/* append a record (built by the caller, RecordNumber filled in here) */
DWORD store_append( struct evt_log *log, const BYTE *data, DWORD size )
{
    struct evt_trailer trailer;
    struct evt_record *rec;
    DWORD status, written;
    BOOL dropped;

    if ((status = make_room( log, size + sizeof(trailer), &dropped )))
    {
        if (dropped) rewrite_file( log );
        if (!log->full_reported && wcsicmp( log->name, L"System" ))
        {
            log->full_reported = TRUE;
            report_system_event( EVENTLOG_ERROR_TYPE, 6000, log->name );  /* "The %1 log file is full." */
        }
        log->full_reported = TRUE;
        return status;
    }
    if (!(rec = malloc( offsetof( struct evt_record, data[size] )))) return ERROR_NOT_ENOUGH_MEMORY;
    rec->number = log->next++;
    rec->size = size;
    memcpy( rec->data, data, size );
    ((EVENTLOGRECORD *)rec->data)->RecordNumber = rec->number;
    list_add_tail( &log->records, &rec->entry );
    log->count++;
    log->bytes += size + sizeof(trailer);
    log->written = now_filetime();
    if (dropped) return rewrite_file( log );
    trailer.crc = RtlComputeCrc32( 0, rec->data, size );
    trailer.number = rec->number;
    SetFilePointer( log->file, 0, NULL, FILE_END );
    if (!WriteFile( log->file, rec->data, size, &written, NULL ) || written != size ||
        !WriteFile( log->file, &trailer, sizeof(trailer), &written, NULL ) || written != sizeof(trailer))
        return rewrite_file( log );
    log->dirty = TRUE;
    return ERROR_SUCCESS;
}

DWORD store_clear( struct evt_log *log )
{
    free_records( log );
    log->full_reported = FALSE;
    log->written = now_filetime();
    return rewrite_file( log );
}

/* the records up to number go, newer ones stay (a clear after a backup that a steady writer kept overtaking) */
DWORD store_clear_through( struct evt_log *log, DWORD number )
{
    struct evt_record *rec, *next;

    LIST_FOR_EACH_ENTRY_SAFE( rec, next, &log->records, struct evt_record, entry )
    {
        if (rec->number > number) break;
        list_remove( &rec->entry );
        log->count--;
        log->bytes -= rec->size + sizeof(struct evt_trailer);
        free( rec );
    }
    log->full_reported = FALSE;
    log->written = now_filetime();
    return rewrite_file( log );
}

/* the header's times and the records written since the last flush go to disk (at most once a second) */
void store_flush_all(void)
{
    struct evt_log *log;

    LIST_FOR_EACH_ENTRY( log, &logs, struct evt_log, entry )
    {
        if (!log->dirty) continue;
        write_header( log );
        FlushFileBuffers( log->file );
        log->dirty = FALSE;
    }
}

void store_touch_read( struct evt_log *log )
{
    log->read = now_filetime();
    log->dirty = TRUE;
}

/* section 9: what Event Viewer shows of a log */
void store_information( struct evt_log *log, struct wex_evtlog_information *info )
{
    LARGE_INTEGER size;
    DWORD room;

    memset( info, 0, sizeof(*info) );
    /* full: a record of the smallest size would not fit under the policy (or the log said so already) */
    room = log->max_size - sizeof(struct evt_file_header);
    info->full = log->full_reported ||
                 (log->bytes + sizeof(EVENTLOGRECORD) + 8 > room && log->retention == EVT_RETENTION_NEVER);
    info->count = log->count;
    info->oldest = store_oldest( log );
    info->next = log->next;
    info->max_size = log->max_size;
    info->retention = log->retention;
    if (GetFileSizeEx( log->file, &size )) info->file_size = size.QuadPart;
    info->created = log->created;
    info->written = log->written;
    info->read = log->read;
}

/* section 9: the EventLog key changed (Event Viewer set a MaxSize or Retention): every loaded log reads its settings */
void store_refresh_all(void)
{
    struct evt_log *log;

    LIST_FOR_EACH_ENTRY( log, &logs, struct evt_log, entry ) store_refresh_settings( log );
}
