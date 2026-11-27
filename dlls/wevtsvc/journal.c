/*
 * WineExplorer: the EventLog service's link to the systemd journal bridge (docs/EVENTLOG_JOURNAL.md, J1)
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

/* In session 0 only. A worker thread keeps the connection to the bridge's socket (through the Unix side: Wine's
 * Winsock has no AF_UNIX), reads its frames as untrusted input and posts each event to the service's completion port;
 * the service's own thread stores it (the store has one thread). Records the service stores that did not come from
 * the journal go the other way, queued here and sent by the worker. The cursor of the last journal entry stored is
 * kept in that record's data, so it is read back from the logs at start (at-least-once, no separate file). */

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winnls.h"
#include "winreg.h"
#include "sddl.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/wex_journal.h"

#include "wevtsvc_private.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(eventlog);

#define MAX_QUEUED        1024              /* records waiting for the bridge; more are dropped (and counted) */
#define MAX_QUEUED_BYTES  (1024 * 1024)     /* and their bytes (a flood while the bridge is down) */

/* an event or a gap for the service's thread */
struct journal_item
{
    BOOL         gap;
    WCHAR        log[16];
    WCHAR        source[WEXJ_MAX_SOURCE + 1];
    WORD         type;
    DWORD        id;
    DWORD        num_strings;
    DWORD        strings_size;   /* bytes of the strings, each NUL-terminated */
    WCHAR       *strings;
    char         cursor[WEXJ_MAX_CURSOR];
};

struct out_frame
{
    struct list  entry;
    DWORD        size;
    char         data[1];
};

static HANDLE port;
static ULONG_PTR port_key;
static HANDLE thread, stop_event;
static CRITICAL_SECTION cs;
static CRITICAL_SECTION_DEBUG cs_debug = { 0, 0, &cs, { &cs_debug.ProcessLocksList, &cs_debug.ProcessLocksList },
                                           0, 0, { (DWORD_PTR)(__FILE__ ": cs") } };
static CRITICAL_SECTION cs = { &cs_debug, -1, 0, 0, 0, 0 };
static struct list queue = LIST_INIT( queue );  /* struct out_frame, under cs */
static DWORD queued, queued_bytes, dropped;
static char last_cursor[WEXJ_MAX_CURSOR];       /* under cs */
static BOOL running;

/* ---- strings ---- */

/* control characters and ANSI escape sequences become spaces (journalctl shows escapes; R6) */
static void sanitize( WCHAR *s )
{
    WCHAR *p = s;

    while (*p)
    {
        if (*p == 0x1b)
        {
            WCHAR *q = p + 1;
            if (*q == '[')
            {
                q++;
                while (*q && !(*q >= 0x40 && *q <= 0x7e)) q++;
                if (*q) q++;
            }
            else if (*q) q++;
            while (p < q) *p++ = ' ';
            continue;
        }
        if (*p < 0x20 || *p == 0x7f) *p = ' ';
        p++;
    }
}

/* a UTF-8 string of the frame (inside [p, end), terminated there) as UTF-16, cut to max characters; NULL when it is
 * not terminated inside the frame */
static WCHAR *take_string( const char **p, const char *end, DWORD max )
{
    const char *s = *p, *nul = memchr( s, 0, end - s );
    WCHAR *w;
    int len;

    if (!nul) return NULL;
    *p = nul + 1;
    len = MultiByteToWideChar( CP_UTF8, 0, s, nul - s, NULL, 0 );
    if (!(w = malloc( (len + 4) * sizeof(WCHAR) ))) return NULL;
    MultiByteToWideChar( CP_UTF8, 0, s, nul - s, w, len );
    w[len] = 0;
    if ((DWORD)len > max)
    {
        w[max - 3] = w[max - 2] = w[max - 1] = '.';
        w[max] = 0;
    }
    sanitize( w );
    return w;
}

/* what the bridge may write (the review): a bridge that went wrong can write only its own rows' sources and ids, never
 * another program's source or the service's own events; the Linux log takes any valid source with the rows' ids */
static BOOL bridge_may_write( DWORD log, const WCHAR *source, DWORD id )
{
    switch (log)
    {
    case WEXJ_LOG_SYSTEM:
        if (!wcscmp( source, L"Service Control Manager" )) return id == 7000 || id == 7031 || id == 7034;
        if (!wcscmp( source, L"Disk" )) return id == 11;
        if (!wcscmp( source, L"Linux Kernel" )) return id >= 1 && id <= 6;
        return FALSE;
    case WEXJ_LOG_APPLICATION:
        return !wcscmp( source, L"Linux Kernel" ) && id == 4;
    case WEXJ_LOG_LINUX:
        return id >= 1 && id <= 6;
    }
    return FALSE;
}

static BOOL valid_source( const WCHAR *s )
{
    const WCHAR *p;

    if (!*s || wcslen( s ) > WEXJ_MAX_SOURCE) return FALSE;
    for (p = s; *p; p++) if (*p == '\\' || *p == '/' || *p < 0x20) return FALSE;
    return TRUE;
}

/* ---- the worker: frames from the bridge ---- */

static BOOL post_event( const char *body, DWORD size )
{
    const char *p = body + sizeof(struct wexj_event), *end = body + size;
    struct wexj_event ev;
    struct journal_item *item = NULL;
    WCHAR *source, *s, *strings[WEXJ_MAX_STRINGS];
    const WCHAR *log;
    DWORD i, n, total = 0, len;
    const char *cursor;

    if (size < sizeof(ev)) return FALSE;
    memcpy( &ev, body, sizeof(ev) );
    if (ev.num_strings > WEXJ_MAX_STRINGS) return FALSE;
    switch (ev.event_type)
    {
    case EVENTLOG_ERROR_TYPE: case EVENTLOG_WARNING_TYPE: case EVENTLOG_INFORMATION_TYPE: break;
    default: return FALSE;  /* audit types come with J2 */
    }
    switch (ev.log)
    {
    case WEXJ_LOG_SYSTEM:      log = L"System"; break;
    case WEXJ_LOG_APPLICATION: log = L"Application"; break;
    case WEXJ_LOG_LINUX:       log = L"Linux"; break;
    default: return FALSE;     /* Security: J2 */
    }
    if (!(source = take_string( &p, end, WEXJ_MAX_SOURCE ))) return FALSE;
    cursor = p;
    if (!memchr( p, 0, end - p ) || strlen( p ) >= WEXJ_MAX_CURSOR)
    {
        free( source );
        return FALSE;
    }
    p += strlen( p ) + 1;
    for (n = 0; n < ev.num_strings; n++)
    {
        if (!(strings[n] = take_string( &p, end, WEXJ_MAX_STRING ))) break;
        total += (wcslen( strings[n] ) + 1) * sizeof(WCHAR);
    }
    if (n == ev.num_strings && p == end && valid_source( source ) && !bridge_may_write( ev.log, source, ev.event_id ))
    {
        WARN( "the bridge may not write %s %lu: dropped\n", debugstr_w(source), (unsigned long)ev.event_id );
        for (i = 0; i < n; i++) free( strings[i] );
        free( source );
        return TRUE;  /* refused, not malformed: the connection stays */
    }
    if (n == ev.num_strings && p == end && valid_source( source ) && (item = calloc( 1, sizeof(*item) )))
        item->strings = malloc( total + sizeof(WCHAR) );
    if (!item || !item->strings)
    {
        free( item );
        for (i = 0; i < n; i++) free( strings[i] );
        free( source );
        return FALSE;
    }
    for (i = 0, s = item->strings; i < n; i++)
    {
        len = wcslen( strings[i] ) + 1;
        memcpy( s, strings[i], len * sizeof(WCHAR) );
        s += len;
        free( strings[i] );
    }
    lstrcpyW( item->log, log );
    lstrcpyW( item->source, source );
    free( source );
    item->type = ev.event_type;
    item->id = ev.event_id;
    item->num_strings = n;
    item->strings_size = total;
    strcpy( item->cursor, cursor );
    PostQueuedCompletionStatus( port, 0, port_key, (OVERLAPPED *)item );
    return TRUE;
}

/* one frame of the bridge; FALSE closes the connection */
static BOOL handle_frame( const struct wexj_header *hdr, const char *body, DWORD size )
{
    struct journal_item *item;

    switch (hdr->type)
    {
    case WEXJ_EVENT:
        return post_event( body, size );
    case WEXJ_GAP:
        if (size || !(item = calloc( 1, sizeof(*item) ))) return FALSE;
        item->gap = TRUE;
        PostQueuedCompletionStatus( port, 0, port_key, (OVERLAPPED *)item );
        return TRUE;
    }
    return FALSE;
}

static BOOL send_frame( int fd, const void *data, DWORD size )
{
    struct journal_io_params params = { fd, size, (ULONG_PTR)data, 0, 0 };
    return !WINE_UNIX_CALL( unix_journal_send, &params );
}

static BOOL send_hello( int fd )
{
    char frame[sizeof(struct wexj_header) + WEXJ_MAX_CURSOR];
    struct wexj_header *hdr = (struct wexj_header *)frame;
    DWORD len;

    EnterCriticalSection( &cs );
    len = strlen( last_cursor ) + 1;
    memcpy( frame + sizeof(*hdr), last_cursor, len );
    LeaveCriticalSection( &cs );
    hdr->magic = WEXJ_MAGIC;
    hdr->version = WEXJ_VERSION;
    hdr->type = WEXJ_HELLO;
    hdr->size = sizeof(*hdr) + len;
    hdr->reserved = 0;
    return send_frame( fd, frame, hdr->size );
}

/* the records queued for the bridge go out; FALSE when the connection failed */
static BOOL drain_queue( int fd )
{
    struct out_frame *f;

    for (;;)
    {
        EnterCriticalSection( &cs );
        if (list_empty( &queue ))
        {
            LeaveCriticalSection( &cs );
            return TRUE;
        }
        f = LIST_ENTRY( list_head( &queue ), struct out_frame, entry );
        list_remove( &f->entry );
        queued--;
        queued_bytes -= f->size;
        LeaveCriticalSection( &cs );
        if (!send_frame( fd, f->data, f->size ))
        {
            free( f );
            return FALSE;
        }
        free( f );
    }
}

static DWORD WINAPI journal_thread( void *arg )
{
    static char buf[2 * WEXJ_MAX_FRAME];

    while (WaitForSingleObject( stop_event, 0 ) == WAIT_TIMEOUT)
    {
        struct journal_connect_params conn = { -1 };
        struct journal_close_params close_params;
        DWORD have = 0;

        if (WINE_UNIX_CALL( unix_journal_connect, &conn ))
        {
            /* no bridge (not installed, or not started yet): try again in a while */
            WaitForSingleObject( stop_event, 10000 );
            continue;
        }
        TRACE( "connected to the journal bridge\n" );
        if (send_hello( conn.fd ))
        {
            for (;;)
            {
                struct journal_io_params io = { conn.fd, sizeof(buf) - have, (ULONG_PTR)(buf + have), 250, 0 };
                NTSTATUS status;

                if (WaitForSingleObject( stop_event, 0 ) != WAIT_TIMEOUT || !drain_queue( conn.fd )) break;
                status = WINE_UNIX_CALL( unix_journal_recv, &io );
                if (status == STATUS_TIMEOUT) continue;
                if (status) break;
                have += io.done;
                while (have >= sizeof(struct wexj_header))
                {
                    struct wexj_header hdr;

                    memcpy( &hdr, buf, sizeof(hdr) );
                    if (hdr.magic != WEXJ_MAGIC || hdr.version != WEXJ_VERSION || hdr.size < sizeof(hdr) ||
                        hdr.size > WEXJ_MAX_FRAME)
                        goto bad;
                    if (have < hdr.size) break;
                    if (!handle_frame( &hdr, buf + sizeof(hdr), hdr.size - sizeof(hdr) )) goto bad;
                    memmove( buf, buf + hdr.size, have - hdr.size );
                    have -= hdr.size;
                }
                continue;
            bad:
                WARN( "a malformed frame from the journal bridge: connection closed\n" );
                break;
            }
        }
        close_params.fd = conn.fd;
        WINE_UNIX_CALL( unix_journal_close, &close_params );
        WaitForSingleObject( stop_event, 2000 );
    }
    return 0;
}

/* ---- the service's thread ---- */

static const WCHAR msg_file[] = L"%SystemRoot%\\system32\\wevtsvc.dll";

/* A source of the Linux log (the listed units' configured names) that logs for the first time gets its key, with our
 * message module; at most 64 of them, beyond that the event is refused. The other logs' sources are wine.inf's. */
static BOOL register_source( const WCHAR *log, const WCHAR *source )
{
    static WCHAR known[64][WEXJ_MAX_SOURCE + 1];
    static unsigned int known_count;
    WCHAR path[MAX_PATH];
    DWORD types = 7, i;
    HKEY key;

    if (wcscmp( log, L"Linux" )) return TRUE;
    for (i = 0; i < known_count; i++) if (!wcsicmp( known[i], source )) return TRUE;
    if (known_count >= ARRAY_SIZE(known))
    {
        WARN( "more than %u Linux log sources: %s refused\n", (unsigned int)ARRAY_SIZE(known), debugstr_w(source) );
        return FALSE;
    }
    swprintf( path, ARRAY_SIZE(path), L"System\\CurrentControlSet\\Services\\EventLog\\Linux\\%s", source );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key ))
    {
        if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return FALSE;
        RegSetValueExW( key, L"EventMessageFile", 0, REG_EXPAND_SZ, (const BYTE *)msg_file, sizeof(msg_file) );
        RegSetValueExW( key, L"TypesSupported", 0, REG_DWORD, (const BYTE *)&types, sizeof(types) );
    }
    RegCloseKey( key );
    lstrcpyW( known[known_count++], source );
    return TRUE;
}

static void store_event( const WCHAR *log_name, const WCHAR *source, WORD type, DWORD id, const WCHAR *strings,
                         DWORD strings_size, DWORD num_strings, const char *cursor )
{
    struct evt_log *log = store_get_log( log_name, TRUE );
    DWORD cursor_len = strlen( cursor ) + 1, data_size = sizeof(DWORD) + cursor_len, size, status, magic;
    BYTE *data, *rec;

    if (!log || !register_source( log_name, source ) || !(data = malloc( data_size ))) return;
    magic = WEXJ_CURSOR_MAGIC;
    memcpy( data, &magic, sizeof(magic) );
    memcpy( data + sizeof(magic), cursor, cursor_len );
    if ((rec = evt_build_record( source, type, 0, id, NULL, 0, strings, strings_size, num_strings, data, data_size,
                                 &size, &status )))
    {
        if (!(status = store_append( log, rec, size )))
        {
            notify_log( log );
            if (*cursor)
            {
                EnterCriticalSection( &cs );
                strcpy( last_cursor, cursor );
                LeaveCriticalSection( &cs );
            }
        }
        free( rec );
    }
    free( data );
}

/* a posted item: an event from the journal, or the gap */
void journal_dispatch( void *ptr )
{
    struct journal_item *item = ptr;

    if (item->gap)
    {
        /* our wording in the message module: entries were lost between the stored cursor and the oldest one kept */
        store_event( L"System", L"Linux Kernel", EVENTLOG_WARNING_TYPE, 6, NULL, 0, 0, "" );
        free( item );
        return;
    }
    store_event( item->log, item->source, item->type, item->id, item->strings, item->strings_size,
                 item->num_strings, item->cursor );
    free( item->strings );
    free( item );
}

/* the cursor in a journal-sourced record's data, or NULL */
static const char *record_cursor( const struct evt_record *rec )
{
    const EVENTLOGRECORD *r = (const EVENTLOGRECORD *)rec->data;
    const char *data;
    DWORD magic;

    if (r->DataLength < sizeof(DWORD) + 1 || (ULONGLONG)r->DataOffset + r->DataLength > rec->size) return NULL;
    data = (const char *)rec->data + r->DataOffset;
    memcpy( &magic, data, sizeof(magic) );
    if (magic != WEXJ_CURSOR_MAGIC || data[r->DataLength - 1] || r->DataLength - sizeof(DWORD) > WEXJ_MAX_CURSOR)
        return NULL;
    return data + sizeof(DWORD);
}

/* the newest journal-sourced record of the logs the bridge writes: its cursor is where the bridge goes on */
static void find_last_cursor(void)
{
    static const WCHAR *names[] = { L"System", L"Application", L"Linux" };
    DWORD best_time = 0, i;

    for (i = 0; i < ARRAY_SIZE(names); i++)
    {
        struct evt_log *log = store_get_log( names[i], TRUE );
        struct evt_record *rec;
        const char *cursor;

        if (!log) continue;
        LIST_FOR_EACH_ENTRY_REV( rec, &log->records, struct evt_record, entry )
        {
            if (!(cursor = record_cursor( rec )) || !*cursor) continue;
            if (((const EVENTLOGRECORD *)rec->data)->TimeWritten >= best_time)
            {
                best_time = ((const EVENTLOGRECORD *)rec->data)->TimeWritten;
                strcpy( last_cursor, cursor );
            }
            break;
        }
    }
}

/* Application and System go to the journal; Security, and a log with a CustomSD, only when session 0's own key of that
 * log says so (DWORD "WexToJournal" non-zero): the bridge's account is less privileged than the log's readers, and
 * the decision is session 0's, the bridge's own switch a second gate (the review) */
static BOOL log_to_journal( const WCHAR *log )
{
    WCHAR path[MAX_PATH];
    DWORD value = 0, size = sizeof(value);

    if (!wcsicmp( log, L"Application" ) || !wcsicmp( log, L"System" )) return TRUE;
    swprintf( path, ARRAY_SIZE(path), L"System\\CurrentControlSet\\Services\\EventLog\\%s", log );
    return !RegGetValueW( HKEY_LOCAL_MACHINE, path, L"WexToJournal", RRF_RT_REG_DWORD, NULL, &value, &size ) && value;
}

/* a record the service stored that did not come from the journal goes to the bridge (it writes it to the journal) */
void journal_record( const WCHAR *log, const WCHAR *source, WORD type, DWORD id, const BYTE *sid, DWORD sid_size,
                     const WCHAR *strings, DWORD num_strings, DWORD number )
{
    static char frame[WEXJ_MAX_FRAME];  /* the service's one thread builds here; the queue holds exact copies */
    struct out_frame *f;
    struct wexj_header hdr;
    struct wexj_record rec;
    char *p, *end, *sid_str = NULL, *data = frame;
    const WCHAR *s = strings;
    DWORD i;

    if (!running || !log_to_journal( log )) return;
    p = data + sizeof(hdr) + sizeof(rec);
    end = data + WEXJ_MAX_FRAME;
    p += WideCharToMultiByte( CP_UTF8, 0, log, -1, p, end - p, NULL, NULL );
    p += WideCharToMultiByte( CP_UTF8, 0, source, -1, p, end - p, NULL, NULL );
    if (sid_size && ConvertSidToStringSidA( (PSID)sid, &sid_str ) && strlen( sid_str ) < 256)
    {
        strcpy( p, sid_str );
        p += strlen( sid_str ) + 1;
    }
    else *p++ = 0;
    if (sid_str) LocalFree( sid_str );
    for (i = 0; i < num_strings && i < WEXJ_MAX_STRINGS; i++)
    {
        int len = min( wcslen( s ), WEXJ_MAX_STRING ), n;

        if (end - p < len * 3 + 1) break;
        n = WideCharToMultiByte( CP_UTF8, 0, s, len, p, end - p - 1, NULL, NULL );
        p[n] = 0;
        p += n + 1;
        s += wcslen( s ) + 1;
    }
    hdr.magic = WEXJ_MAGIC;
    hdr.version = WEXJ_VERSION;
    hdr.type = WEXJ_RECORD;
    hdr.size = p - data;
    hdr.reserved = 0;
    rec.event_id = id;
    rec.event_type = type;
    rec.num_strings = i;
    rec.record_number = number;
    rec.reserved = 0;
    memcpy( data, &hdr, sizeof(hdr) );
    memcpy( data + sizeof(hdr), &rec, sizeof(rec) );
    if (!(f = malloc( offsetof( struct out_frame, data[hdr.size] )))) return;
    memcpy( f->data, data, hdr.size );
    f->size = hdr.size;
    EnterCriticalSection( &cs );
    if (queued >= MAX_QUEUED || queued_bytes + f->size > MAX_QUEUED_BYTES)
    {
        if (!dropped++) WARN( "the journal bridge does not keep up: records not sent\n" );
        free( f );
    }
    else
    {
        list_add_tail( &queue, &f->entry );
        queued++;
        queued_bytes += f->size;
    }
    LeaveCriticalSection( &cs );
}

/* session 0's service: the link to the bridge (nothing happens while there is none) */
void journal_start( HANDLE completion_port, ULONG_PTR key )
{
    if (__wine_init_unix_call()) return;
    port = completion_port;
    port_key = key;
    find_last_cursor();
    stop_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    running = TRUE;
    thread = CreateThread( NULL, 0, journal_thread, NULL, 0, NULL );
}

void journal_stop(void)
{
    if (!running) return;
    SetEvent( stop_event );
    WaitForSingleObject( thread, 5000 );
    running = FALSE;
}
