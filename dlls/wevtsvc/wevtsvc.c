/*
 * Event Log Service
 *
 * Copyright 2020 Dmitry Timoshkov
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

/* WineExplorer (docs/EVENTLOG_DESIGN.md, L1): the service keeps the machine's event logs and is their only writer.
 * Clients speak our own protocol (include/wine/wex_eventlog.h) over a message-mode pipe. With the system tier the
 * service runs in session 0; a session's programs reach it through the pipe bridge, which hands the service their
 * token (made with link_bound_token: the account and session are the link's). The service only identifies its
 * callers (it impersonates long enough to read the token, never to act), decides access from the table of
 * "Access rules", and never opens a path a client sends (backups are written by the client from streamed records). */

#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#include "windef.h"
#include "winbase.h"
#include "winsvc.h"
#include "winreg.h"
#include "sddl.h"
#include "wine/debug.h"

#include "wevtsvc_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(eventlog);

static SERVICE_STATUS_HANDLE svc_handle;
static HANDLE done_event;
static volatile LONG stopping;

CRITICAL_SECTION evt_lock;
static CRITICAL_SECTION_DEBUG evt_lock_debug =
{
    0, 0, &evt_lock,
    { &evt_lock_debug.ProcessLocksList, &evt_lock_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": evt_lock") }
};
CRITICAL_SECTION evt_lock = { &evt_lock_debug, -1, 0, 0, 0, 0 };
CONDITION_VARIABLE evt_written = CONDITION_VARIABLE_INIT;

static BOOL in_session0;          /* session 0 of the system tier (else a plain prefix's own service) */
static WCHAR computer_name[MAX_COMPUTERNAME_LENGTH + 1];

/* log handle rights */
#define RIGHT_READ  1
#define RIGHT_WRITE 2
#define RIGHT_CLEAR 4

/* what an account holds (E4's caps) */
struct account
{
    struct list  entry;
    BYTE         sid[SECURITY_MAX_SID_SIZE];
    unsigned int connections;
    unsigned int handles;
    unsigned int subscriptions;
};
static struct list accounts = LIST_INIT( accounts );

#define MAX_CONN_HANDLES 64

struct log_handle
{
    struct evt_log *log;
    DWORD           rights;
    BOOL            source;      /* RegisterEventSource: reports */
    WCHAR           source_name[256];
    DWORD           position;    /* the next record of a sequential read, 0: not started */
};

struct connection
{
    HANDLE            pipe;
    BOOL              local;           /* the service's own server: session 0's processes, or a plain prefix's */
    HANDLE            token;           /* the caller's, at identification level at least */
    struct account   *account;
    BOOL              admin;           /* an elevated administrator */
    BOOL              security_priv;   /* SeSecurityPrivilege enabled */
    DWORD             client_pid;      /* tracing only, never for decisions */
    struct log_handle handles[MAX_CONN_HANDLES];
};

/* ---- accounts ---- */

static struct account *account_get( PSID sid )
{
    struct account *account;

    LIST_FOR_EACH_ENTRY( account, &accounts, struct account, entry )
        if (EqualSid( (PSID)account->sid, sid )) return account;
    if (!(account = calloc( 1, sizeof(*account) ))) return NULL;
    CopySid( sizeof(account->sid), (PSID)account->sid, sid );
    list_add_tail( &accounts, &account->entry );
    return account;
}

/* ---- records ---- */

/* build an EVENTLOGRECORD; NULL with *status set when it is over the record maximum */
static BYTE *build_record( const WCHAR *source, WORD type, WORD category, DWORD id, const BYTE *sid, DWORD sid_size,
                           const WCHAR *strings, DWORD strings_size, DWORD num_strings, const BYTE *data,
                           DWORD data_size, DWORD *size, DWORD *status )
{
    DWORD source_size = (wcslen( source ) + 1) * sizeof(WCHAR);
    DWORD computer_size = (wcslen( computer_name ) + 1) * sizeof(WCHAR);
    DWORD pos, sid_offset, strings_offset, data_offset, total;
    EVENTLOGRECORD *rec;
    BYTE *buf;

    pos = sizeof(EVENTLOGRECORD) + source_size + computer_size;
    pos = (pos + 3) & ~3;
    sid_offset = pos;
    pos += sid_size;
    strings_offset = pos;
    pos += strings_size;
    data_offset = pos;
    pos += data_size;
    pos = (pos + 3) & ~3;
    total = pos + sizeof(DWORD);
    if (total > WEX_EVTLOG_MAX_RECORD)
    {
        *status = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    if (!(buf = calloc( 1, total )))
    {
        *status = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    rec = (EVENTLOGRECORD *)buf;
    rec->Length = total;
    rec->Reserved = 0x654c664c;  /* "LfLe" */
    rec->TimeGenerated = rec->TimeWritten = (DWORD)time( NULL );
    rec->EventID = id;
    rec->EventType = type;
    rec->NumStrings = num_strings;
    rec->EventCategory = category;
    rec->StringOffset = strings_offset;
    rec->UserSidLength = sid_size;
    rec->UserSidOffset = sid_size ? sid_offset : 0;
    rec->DataLength = data_size;
    rec->DataOffset = data_offset;
    memcpy( buf + sizeof(EVENTLOGRECORD), source, source_size );
    memcpy( buf + sizeof(EVENTLOGRECORD) + source_size, computer_name, computer_size );
    if (sid_size) memcpy( buf + sid_offset, sid, sid_size );
    if (strings_size) memcpy( buf + strings_offset, strings, strings_size );
    if (data_size) memcpy( buf + data_offset, data, data_size );
    *(DWORD *)(buf + total - sizeof(DWORD)) = total;
    *size = total;
    *status = ERROR_SUCCESS;
    return buf;
}

/* the service's own record in the System log (source "EventLog"); under the lock */
void report_system_event( WORD type, DWORD id, const WCHAR *string )
{
    struct evt_log *log = store_get_log( L"System", TRUE );
    DWORD size, status, strings_size = string ? (wcslen( string ) + 1) * sizeof(WCHAR) : 0;
    BYTE *rec;

    if (!log) return;
    if (!(rec = build_record( L"EventLog", type, 0, id, NULL, 0, string, strings_size, string ? 1 : 0, NULL, 0,
                              &size, &status )))
        return;
    if (!store_append( log, rec, size )) WakeAllConditionVariable( &evt_written );
    free( rec );
}

/* ---- access (docs/EVENTLOG_DESIGN.md, "Access rules") ---- */

static BOOL is_builtin( const struct evt_log *log, const WCHAR *name )
{
    return !wcsicmp( log->name, name );
}

/* the rights the caller of conn has on log */
static DWORD log_rights( struct connection *conn, struct evt_log *log )
{
    static GENERIC_MAPPING mapping = { RIGHT_READ, RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE | RIGHT_CLEAR,
                                       RIGHT_READ | RIGHT_WRITE | RIGHT_CLEAR };
    DWORD rights = 0;

    if (log->is_security)
    {
        /* SeSecurityPrivilege reads and clears; nobody writes until auditing exists */
        return conn->security_priv ? RIGHT_READ | RIGHT_CLEAR : 0;
    }
    if (log->custom_sd && !is_builtin( log, L"System" ) && !is_builtin( log, L"Application" ))
    {
        PSECURITY_DESCRIPTOR sd;
        PRIVILEGE_SET privs;
        DWORD privs_size = sizeof(privs), granted, desired;
        BOOL ok;

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW( log->custom_sd, SDDL_REVISION_1, &sd, NULL ))
            return 0;  /* a CustomSD that does not parse gives nothing */
        for (desired = RIGHT_READ; desired <= RIGHT_CLEAR; desired <<= 1)
        {
            privs_size = sizeof(privs);
            if (AccessCheck( sd, conn->token, desired, &mapping, &privs, &privs_size, &granted, &ok ) && ok)
                rights |= desired;
        }
        LocalFree( sd );
        return rights;
    }
    rights = RIGHT_READ;
    if (conn->admin) rights |= RIGHT_CLEAR;
    if (!is_builtin( log, L"System" )) rights |= RIGHT_WRITE;
    /* E1, L1's form: System is written by session 0's own processes (the service's own server), not by a session's
     * caller whatever its token (a session's system role counts as the session's user); in a plain prefix every
     * process is the prefix's own */
    else if (conn->local) rights |= RIGHT_WRITE;
    return rights;
}

/* identify the caller: its token at identification level, held for the connection; FALSE to refuse it */
static BOOL identify( struct connection *conn )
{
    static const LUID_AND_ATTRIBUTES none = { { 0, 0 }, 0 };
    BYTE user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)user_buf;
    PRIVILEGE_SET privs = { 1, PRIVILEGE_SET_ALL_NECESSARY, { none } };
    BYTE admins_buf[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(admins_buf);
    TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
    BOOL member = FALSE, held = FALSE;
    ULONG session = 0;

    if (!ImpersonateNamedPipeClient( conn->pipe )) return FALSE;
    if (!OpenThreadToken( GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &conn->token ))
    {
        RevertToSelf();
        return FALSE;
    }
    RevertToSelf();  /* identification only: the service never acts as its caller */
    if (!GetTokenInformation( conn->token, TokenUser, user, sizeof(user_buf), &size ) ||
        IsWellKnownSid( user->User.Sid, WinAnonymousSid ))
        return FALSE;  /* anonymous callers are refused (E2) */
    size = sizeof(admins_buf);
    if (CreateWellKnownSid( WinBuiltinAdministratorsSid, NULL, admins_buf, &size ))
        CheckTokenMembership( conn->token, admins_buf, &member );
    GetTokenInformation( conn->token, TokenElevationType, &elevation, sizeof(elevation), &size );
    conn->admin = member && elevation != TokenElevationTypeLimited;
    LookupPrivilegeValueW( NULL, L"SeSecurityPrivilege", &privs.Privilege[0].Luid );
    conn->security_priv = PrivilegeCheck( conn->token, &privs, &held ) && held;
    /* a local connection in session 0 must be a session 0 process (a session reaches only the bridged name) */
    if (conn->local && in_session0 && GetNamedPipeClientSessionId( conn->pipe, &session ) && session)
        return FALSE;
    GetNamedPipeClientProcessId( conn->pipe, &conn->client_pid );
    if (!(conn->account = account_get( user->User.Sid ))) return FALSE;
    if (conn->account->connections >= WEX_EVTLOG_MAX_CONNECTIONS)
    {
        WARN( "connection refused: the account holds %u\n", conn->account->connections );
        return FALSE;
    }
    conn->account->connections++;
    return TRUE;
}

/* ---- requests ---- */

struct reply
{
    struct wex_evtlog_header hdr;
    BYTE *body;
    DWORD body_size;
};

/* a terminated UTF-16 string of exactly size bytes, at most max characters */
static BOOL valid_string( const BYTE *p, DWORD size, DWORD max )
{
    const WCHAR *s = (const WCHAR *)p;
    DWORD len = size / sizeof(WCHAR);

    return size >= sizeof(WCHAR) && !(size % sizeof(WCHAR)) && len - 1 <= max && !s[len - 1] &&
           wcslen( s ) == len - 1;
}

/* the log an event source belongs to: the EventLog key's log that has it as a subkey; Application otherwise */
static void source_log( const WCHAR *source, WCHAR *log, DWORD count )
{
    WCHAR name[256], path[600];
    DWORD i, len;
    HKEY key, sub;

    lstrcpynW( log, L"Application", count );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\EventLog", 0, KEY_READ, &key ))
        return;
    for (i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( key, i, name, &len, NULL, NULL, NULL, NULL ); i++)
    {
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", name, source );
        if (RegOpenKeyExW( key, path, 0, KEY_READ, &sub )) continue;
        RegCloseKey( sub );
        lstrcpynW( log, name, count );
        break;
    }
    RegCloseKey( key );
}

static struct log_handle *get_handle( struct connection *conn, unsigned int handle )
{
    if (!handle || handle > MAX_CONN_HANDLES || !conn->handles[handle - 1].log) return NULL;
    return &conn->handles[handle - 1];
}

static DWORD do_open( struct connection *conn, const BYTE *body, DWORD size, struct reply *reply )
{
    struct wex_evtlog_open open;
    WCHAR log_name[256];
    const WCHAR *name;
    struct evt_log *log;
    struct log_handle *h = NULL;
    DWORD i, rights;

    if (size < sizeof(open)) return ERROR_INVALID_PARAMETER;
    memcpy( &open, body, sizeof(open) );
    if (open.flags > WEX_EVTLOG_OPEN_SOURCE || open.name_size != size - sizeof(open) ||
        !valid_string( body + sizeof(open), open.name_size, WEX_EVTLOG_MAX_STRING ))
        return ERROR_INVALID_PARAMETER;
    name = (const WCHAR *)(body + sizeof(open));
    if (open.flags == WEX_EVTLOG_OPEN_SOURCE) source_log( name, log_name, ARRAY_SIZE(log_name) );
    else lstrcpynW( log_name, name, ARRAY_SIZE(log_name) );
    /* an unknown log opens Application, as Windows does */
    if (!(log = store_get_log( log_name, TRUE )) && !(log = store_get_log( L"Application", TRUE )))
        return ERROR_FILE_NOT_FOUND;
    store_refresh_settings( log );
    rights = log_rights( conn, log );
    if (open.flags == WEX_EVTLOG_OPEN_SOURCE ? !(rights & RIGHT_WRITE) : !(rights & RIGHT_READ))
        return ERROR_ACCESS_DENIED;
    if (conn->account->handles >= WEX_EVTLOG_MAX_HANDLES) return ERROR_NOT_ENOUGH_QUOTA;
    for (i = 0; i < MAX_CONN_HANDLES; i++) if (!conn->handles[i].log) { h = &conn->handles[i]; break; }
    if (!h) return ERROR_NOT_ENOUGH_QUOTA;
    memset( h, 0, sizeof(*h) );
    h->log = log;
    h->rights = rights;
    h->source = open.flags == WEX_EVTLOG_OPEN_SOURCE;
    lstrcpynW( h->source_name, name, ARRAY_SIZE(h->source_name) );
    conn->account->handles++;
    reply->hdr.handle = i + 1;
    return ERROR_SUCCESS;
}

static void close_handle( struct connection *conn, struct log_handle *h )
{
    h->log = NULL;
    conn->account->handles--;
}

static DWORD do_report( struct connection *conn, struct log_handle *h, const BYTE *body, DWORD size )
{
    struct wex_evtlog_report rep;
    const BYTE *sid, *strings, *data;
    DWORD i, pos, rec_size, status;
    BYTE *rec;

    if (!h->source || !(h->rights & RIGHT_WRITE)) return h->source ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE;
    if (size < sizeof(rep)) return ERROR_INVALID_PARAMETER;
    memcpy( &rep, body, sizeof(rep) );
    if (rep.sid_size > WEX_EVTLOG_MAX_SID || rep.num_strings > WEX_EVTLOG_MAX_STRINGS ||
        rep.strings_size > WEX_EVTLOG_MAX_RECORD || rep.data_size > WEX_EVTLOG_MAX_RECORD ||
        (ULONGLONG)sizeof(rep) + rep.sid_size + rep.strings_size + rep.data_size != size)
        return ERROR_INVALID_PARAMETER;
    switch (rep.type)
    {
    case EVENTLOG_SUCCESS: case EVENTLOG_ERROR_TYPE: case EVENTLOG_WARNING_TYPE: case EVENTLOG_INFORMATION_TYPE:
    case EVENTLOG_AUDIT_SUCCESS: case EVENTLOG_AUDIT_FAILURE:
        break;
    default:
        return ERROR_INVALID_PARAMETER;
    }
    sid = body + sizeof(rep);
    strings = sid + rep.sid_size;
    data = strings + rep.strings_size;
    /* the SID the caller passes is stored (Windows stores what the caller gives), when it is a well-formed SID */
    if (rep.sid_size && (!IsValidSid( (PSID)sid ) || GetLengthSid( (PSID)sid ) != rep.sid_size))
        return ERROR_INVALID_PARAMETER;
    for (i = 0, pos = 0; i < rep.num_strings; i++)
    {
        const WCHAR *s = (const WCHAR *)(strings + pos);
        DWORD left = (rep.strings_size - pos) / sizeof(WCHAR), len;

        for (len = 0; len < left && s[len]; len++) if (len >= WEX_EVTLOG_MAX_STRING) return ERROR_INVALID_PARAMETER;
        if (len >= left) return ERROR_INVALID_PARAMETER;
        pos += (len + 1) * sizeof(WCHAR);
    }
    if (pos != rep.strings_size) return ERROR_INVALID_PARAMETER;
    if (!(rec = build_record( h->source_name, rep.type, rep.category, rep.event_id, sid, rep.sid_size,
                              (const WCHAR *)strings, rep.strings_size, rep.num_strings, data, rep.data_size,
                              &rec_size, &status )))
        return status;
    status = store_append( h->log, rec, rec_size );
    free( rec );
    if (!status) WakeAllConditionVariable( &evt_written );
    TRACE( "pid %04lx: event %lu into %s: %lu\n", conn->client_pid, rep.event_id, debugstr_w(h->log->name), status );
    return status;
}

/* the records a read returns, into reply */
static DWORD do_read( struct connection *conn, struct log_handle *h, const struct wex_evtlog_header *req,
                      const BYTE *body, DWORD size, struct reply *reply )
{
    DWORD flags = req->arg[0], bufsize, need = 0, oldest = store_oldest( h->log ), newest, number, used = 0;
    BOOL forwards, seek;
    struct evt_record *rec;

    if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
    if (size != sizeof(DWORD)) return ERROR_INVALID_PARAMETER;
    memcpy( &bufsize, body, sizeof(bufsize) );
    if (bufsize > WEX_EVTLOG_MAX_READ) bufsize = WEX_EVTLOG_MAX_READ;  /* never more, whatever the request says */
    seek = flags & EVENTLOG_SEEK_READ;
    forwards = flags & EVENTLOG_FORWARDS_READ;
    if (!!(flags & EVENTLOG_SEQUENTIAL_READ) == !!seek || !!forwards == !!(flags & EVENTLOG_BACKWARDS_READ) ||
        (flags & ~(EVENTLOG_SEQUENTIAL_READ | EVENTLOG_SEEK_READ | EVENTLOG_FORWARDS_READ | EVENTLOG_BACKWARDS_READ)))
        return ERROR_INVALID_PARAMETER;
    if (!oldest) return ERROR_HANDLE_EOF;
    newest = h->log->next - 1;
    if (seek)
    {
        if (req->arg[1] < oldest || req->arg[1] > newest) return ERROR_INVALID_PARAMETER;
        number = req->arg[1];
    }
    else if (!h->position) number = forwards ? oldest : newest;
    else number = h->position;
    if (number < oldest) number = oldest;  /* the ring dropped what the read had not reached */
    if (!(reply->body = malloc( bufsize ? bufsize : 1 ))) return ERROR_NOT_ENOUGH_MEMORY;
    while (number >= oldest && number <= newest && (rec = store_find( h->log, number )))
    {
        if (used + rec->size > bufsize)
        {
            if (!used) need = rec->size;
            break;
        }
        memcpy( reply->body + used, rec->data, rec->size );
        used += rec->size;
        number = forwards ? number + 1 : number - 1;
    }
    if (need)
    {
        reply->hdr.arg[0] = need;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (!used) return ERROR_HANDLE_EOF;
    h->position = number ? number : (DWORD)-1;  /* backwards past the first record: nothing more */
    reply->body_size = used;
    return ERROR_SUCCESS;
}

/* records from a number on, for a backup the client writes (E3) */
static DWORD do_stream( struct log_handle *h, const struct wex_evtlog_header *req, struct reply *reply )
{
    DWORD number = req->arg[0] ? req->arg[0] : store_oldest( h->log ), used = 0;
    struct evt_record *rec;

    if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
    if (number < store_oldest( h->log )) number = store_oldest( h->log );
    if (!number || number >= h->log->next) return ERROR_HANDLE_EOF;
    if (!(reply->body = malloc( WEX_EVTLOG_MAX_READ ))) return ERROR_NOT_ENOUGH_MEMORY;
    LIST_FOR_EACH_ENTRY( rec, &h->log->records, struct evt_record, entry )
    {
        if (rec->number < number) continue;
        if (used + rec->size > WEX_EVTLOG_MAX_READ) break;
        memcpy( reply->body + used, rec->data, rec->size );
        used += rec->size;
        number = rec->number + 1;
    }
    reply->body_size = used;
    reply->hdr.arg[0] = number;
    return used ? ERROR_SUCCESS : ERROR_HANDLE_EOF;
}

/* wait (with the lock) until a record is written to the handle's log; the client keeps one such request on a
 * connection of its own */
static DWORD do_notify( struct connection *conn, struct log_handle *h )
{
    unsigned int generation = h->log->generation;
    DWORD avail;

    if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
    if (conn->account->subscriptions >= WEX_EVTLOG_MAX_SUBSCRIPTIONS) return ERROR_NOT_ENOUGH_QUOTA;
    conn->account->subscriptions++;
    while (h->log->generation == generation && !stopping)
    {
        SleepConditionVariableCS( &evt_written, &evt_lock, 1000 );
        /* the client went away: the subscription ends */
        if (!PeekNamedPipe( conn->pipe, NULL, 0, NULL, &avail, NULL )) break;
    }
    conn->account->subscriptions--;
    return h->log->generation != generation ? ERROR_SUCCESS : ERROR_OPERATION_ABORTED;
}

static DWORD dispatch( struct connection *conn, const struct wex_evtlog_header *req, const BYTE *body, DWORD size,
                       struct reply *reply )
{
    struct log_handle *h = NULL;

    if (req->op != WEX_EVTLOG_OPEN && !(h = get_handle( conn, req->handle ))) return ERROR_INVALID_HANDLE;
    switch (req->op)
    {
    case WEX_EVTLOG_OPEN:   return do_open( conn, body, size, reply );
    case WEX_EVTLOG_CLOSE:  close_handle( conn, h ); return ERROR_SUCCESS;
    case WEX_EVTLOG_REPORT: return do_report( conn, h, body, size );
    case WEX_EVTLOG_READ:   return do_read( conn, h, req, body, size, reply );
    case WEX_EVTLOG_COUNT:
        if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
        reply->hdr.arg[0] = h->log->count;
        return ERROR_SUCCESS;
    case WEX_EVTLOG_OLDEST:
        if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
        reply->hdr.arg[0] = store_oldest( h->log );
        return ERROR_SUCCESS;
    case WEX_EVTLOG_INFO:
        if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
        reply->hdr.arg[0] = h->log->full_reported;
        return ERROR_SUCCESS;
    case WEX_EVTLOG_CLEAR:
        if (!(h->rights & RIGHT_CLEAR)) return ERROR_ACCESS_DENIED;
        TRACE( "pid %04lx clears %s (backup made: %u)\n", conn->client_pid, debugstr_w(h->log->name), req->arg[0] );
        return store_clear( h->log );
    case WEX_EVTLOG_STREAM: return do_stream( h, req, reply );
    case WEX_EVTLOG_NOTIFY: return do_notify( conn, h );
    default:                return ERROR_INVALID_FUNCTION;  /* an unknown operation is refused */
    }
}

/* one message of at most WEX_EVTLOG_MAX_MSG bytes into *buf (grown as needed); its size, or 0 to close */
static DWORD read_message( HANDLE pipe, BYTE **buf, DWORD *cap )
{
    DWORD got = 0, left = 0, more;

    if (ReadFile( pipe, *buf, *cap, &got, NULL )) return got;
    if (GetLastError() != ERROR_MORE_DATA) return 0;
    if (!PeekNamedPipe( pipe, NULL, 0, NULL, NULL, &left ) || (ULONGLONG)got + left > WEX_EVTLOG_MAX_MSG)
        return 0;  /* over the limit: a malformed frame closes the connection */
    if (got + left > *cap)
    {
        BYTE *grown = realloc( *buf, got + left );
        if (!grown) return 0;
        *buf = grown;
        *cap = got + left;
    }
    if (!ReadFile( pipe, *buf + got, left, &more, NULL ) || more != left) return 0;
    return got + left;
}

static DWORD WINAPI connection_thread( void *arg )
{
    struct connection *conn = arg;
    DWORD cap = 65536, size, written, i;
    BYTE *buf = malloc( cap );
    BOOL identified;

    EnterCriticalSection( &evt_lock );
    identified = buf && identify( conn );
    LeaveCriticalSection( &evt_lock );
    while (identified && !stopping && (size = read_message( conn->pipe, &buf, &cap )))
    {
        struct wex_evtlog_header req;
        struct reply reply;
        BOOL ok;

        if (size < sizeof(req)) break;
        memcpy( &req, buf, sizeof(req) );
        if (req.magic != WEX_EVTLOG_MAGIC || req.size != size) break;  /* malformed: the connection closes */
        memset( &reply, 0, sizeof(reply) );
        reply.hdr.magic = WEX_EVTLOG_MAGIC;
        reply.hdr.version = WEX_EVTLOG_VERSION;
        reply.hdr.op = req.op;
        reply.hdr.handle = req.handle;
        if (req.version != WEX_EVTLOG_VERSION) reply.hdr.status = ERROR_REVISION_MISMATCH;
        else
        {
            EnterCriticalSection( &evt_lock );
            reply.hdr.status = dispatch( conn, &req, buf + sizeof(req), size - sizeof(req), &reply );
            LeaveCriticalSection( &evt_lock );
        }
        reply.hdr.size = sizeof(reply.hdr) + reply.body_size;
        if (reply.body_size)
        {
            BYTE *out = malloc( reply.hdr.size );

            ok = out != NULL;
            if (ok)
            {
                memcpy( out, &reply.hdr, sizeof(reply.hdr) );
                memcpy( out + sizeof(reply.hdr), reply.body, reply.body_size );
                ok = WriteFile( conn->pipe, out, reply.hdr.size, &written, NULL );
                free( out );
            }
        }
        else ok = WriteFile( conn->pipe, &reply.hdr, sizeof(reply.hdr), &written, NULL );
        free( reply.body );
        if (!ok) break;
    }
    EnterCriticalSection( &evt_lock );
    if (identified)
    {
        for (i = 0; i < MAX_CONN_HANDLES; i++) if (conn->handles[i].log) close_handle( conn, &conn->handles[i] );
        conn->account->connections--;
    }
    LeaveCriticalSection( &evt_lock );
    if (conn->token) CloseHandle( conn->token );
    DisconnectNamedPipe( conn->pipe );
    CloseHandle( conn->pipe );
    free( buf );
    free( conn );
    return 0;
}

struct listener
{
    const WCHAR *name;
    BOOL         local;
};

static DWORD WINAPI listener_thread( void *arg )
{
    const struct listener *listener = arg;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };

    /* every account may connect; the service decides what each may do. Anonymous callers are refused at connect */
    ConvertStringSecurityDescriptorToSecurityDescriptorW( L"D:(A;;GRGW;;;WD)(A;;GA;;;SY)", SDDL_REVISION_1,
                                                          &sa.lpSecurityDescriptor, NULL );
    while (!stopping)
    {
        struct connection *conn;
        HANDLE pipe, thread;

        pipe = CreateNamedPipeW( listener->name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                 PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa );
        if (pipe == INVALID_HANDLE_VALUE)
        {
            ERR( "cannot create %s: %lu\n", debugstr_w(listener->name), GetLastError() );
            Sleep( 1000 );
            continue;
        }
        if (!ConnectNamedPipe( pipe, NULL ) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle( pipe );
            continue;
        }
        if (stopping || !(conn = calloc( 1, sizeof(*conn) )))
        {
            CloseHandle( pipe );
            continue;
        }
        conn->pipe = pipe;
        conn->local = listener->local;
        if (!(thread = CreateThread( NULL, 0, connection_thread, conn, 0, NULL )))
        {
            CloseHandle( pipe );
            free( conn );
            continue;
        }
        CloseHandle( thread );
    }
    LocalFree( sa.lpSecurityDescriptor );
    return 0;
}

/* at most one flush a second (E5) */
static DWORD WINAPI flush_thread( void *arg )
{
    while (WaitForSingleObject( done_event, 1000 ) == WAIT_TIMEOUT)
    {
        EnterCriticalSection( &evt_lock );
        store_flush_all();
        LeaveCriticalSection( &evt_lock );
    }
    return 0;
}

static void eventlog_update_status(DWORD state)
{
    SERVICE_STATUS status;

    status.dwServiceType = SERVICE_WIN32;
    status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = 0;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint = 0;
    status.dwWaitHint = 0;
    status.dwCurrentState = state;

    SetServiceStatus(svc_handle, &status);
}

static void WINAPI eventlog_handler(DWORD control)
{
    TRACE("%#lx\n", control);

    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        eventlog_update_status(SERVICE_STOP_PENDING);
        InterlockedExchange( &stopping, 1 );
        SetEvent(done_event);
        break;

    default:
        eventlog_update_status(SERVICE_RUNNING);
        break;
    }
}

void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    static struct listener local = { WEX_EVTLOG_PIPE, TRUE }, bridged = { WEX_EVTLOG_PIPE_BRIDGED, FALSE };
    WCHAR dir[MAX_PATH];
    DWORD session = 1, len = ARRAY_SIZE(computer_name);
    HANDLE thread;

    TRACE("starting Event Log Service\n");

    svc_handle = RegisterServiceCtrlHandlerW(L"EventLog", eventlog_handler);
    if (!svc_handle)
    {
        ERR("RegisterServiceCtrlHandler error %lu\n", GetLastError());
        return;
    }

    done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    GetComputerNameW( computer_name, &len );
    ProcessIdToSessionId( GetCurrentProcessId(), &session );
    in_session0 = !session;
    GetSystemDirectoryW( dir, ARRAY_SIZE(dir) - 8 );
    lstrcatW( dir, L"\\config" );
    CreateDirectoryW( dir, NULL );
    store_init( dir );
    EnterCriticalSection( &evt_lock );
    store_get_log( L"Application", TRUE );
    store_get_log( L"System", TRUE );
    store_get_log( L"Security", TRUE );  /* created now, empty: written once auditing exists */
    LeaveCriticalSection( &evt_lock );

    if ((thread = CreateThread( NULL, 0, listener_thread, &local, 0, NULL ))) CloseHandle( thread );
    if (in_session0 && (thread = CreateThread( NULL, 0, listener_thread, &bridged, 0, NULL ))) CloseHandle( thread );
    if ((thread = CreateThread( NULL, 0, flush_thread, NULL, 0, NULL ))) CloseHandle( thread );

    eventlog_update_status(SERVICE_RUNNING);
    WaitForSingleObject(done_event, INFINITE);

    EnterCriticalSection( &evt_lock );
    store_flush_all();
    LeaveCriticalSection( &evt_lock );
    WakeAllConditionVariable( &evt_written );
    eventlog_update_status(SERVICE_STOPPED);

    TRACE("exiting Event Log Service\n");
}
