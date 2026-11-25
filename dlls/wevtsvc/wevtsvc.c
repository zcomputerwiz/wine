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
 * callers (it impersonates long enough to read the token, never to act), decides access with the logs' descriptors
 * ("Access rules"), and never opens a path a client sends (backups are written by the client from streamed records).
 *
 * One thread serves everything through an I/O completion port: overlapped connects, reads and writes, so that
 * waiting connections (subscriptions above all) hold no thread. Requests are read only while the bytes the service
 * holds for requests and replies stay under a budget. */

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
#include "wine/wex_caller.h"

WINE_DEFAULT_DEBUG_CHANNEL(eventlog);

static SERVICE_STATUS_HANDLE svc_handle;
static HANDLE port;               /* the completion port of every pipe */
static volatile LONG stopping;

static BOOL in_session0;          /* session 0 of the system tier (else a plain prefix's own service) */
static WCHAR computer_name[MAX_COMPUTERNAME_LENGTH + 1];

/* log handle rights (the Event Log's access bits) */
#define RIGHT_READ  1
#define RIGHT_WRITE 2
#define RIGHT_CLEAR 4

/* Windows' built-in descriptors of Application and System (read on Windows 10 with wevtutil), used when a log has no
 * CustomSD; custom logs without one take Application's. Security keeps its own rule (SeSecurityPrivilege). */
static const WCHAR application_sd[] =
    L"O:BAG:SYD:(A;;0x2;;;S-1-15-2-1)(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x7;;;SO)(A;;0x3;;;IU)(A;;0x3;;;SU)"
    L"(A;;0x3;;;S-1-5-3)(A;;0x3;;;S-1-5-33)(A;;0x1;;;S-1-5-32-573)";
static const WCHAR system_sd[] =
    L"O:BAG:SYD:(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x3;;;BO)(A;;0x5;;;SO)(A;;0x1;;;IU)(A;;0x3;;;SU)"
    L"(A;;0x1;;;S-1-5-3)(A;;0x2;;;S-1-5-33)(A;;0x1;;;S-1-5-32-573)";

#define BUDGET (16 * 1024 * 1024)   /* bytes of requests and replies held at once, service-wide (E4) */
static size_t budget_used;

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
#define SMALL_READ       4096   /* the first part of every message; a larger one is read whole after a peek */

struct log_handle
{
    struct evt_log *log;
    DWORD           rights;
    BOOL            source;      /* RegisterEventSource: reports */
    WCHAR           source_name[256];
    DWORD           position;    /* the next record of a sequential read, 0: not started */
};

enum io_kind { IO_CONNECT, IO_READ, IO_WRITE };

struct io
{
    OVERLAPPED         ov;
    enum io_kind       kind;
    struct connection *conn;
};

struct listener
{
    const WCHAR *name;
    BOOL         local;      /* the service's own server: session 0's processes, or a plain prefix's */
    BOOL         first;      /* the first instance is made with FILE_FLAG_FIRST_PIPE_INSTANCE */
};

struct connection
{
    struct list        entry;            /* in connections, or in waiting (no budget to read) */
    HANDLE             pipe;
    const struct listener *listener;
    BOOL               connected, identified, closing;
    unsigned int       pending;          /* I/O in flight; freed at 0 once closing */
    struct io          io_connect, io_read, io_write;
    BYTE               small[SMALL_READ];
    BYTE              *write_buf;
    DWORD              write_size;
    BOOL               reading;
    struct log_handle *notify;           /* a NOTIFY waiting for a record */
    HANDLE             token;            /* the caller's, at identification level at least */
    struct account    *account;
    BOOL               service_account;  /* SYSTEM, LocalService or NetworkService */
    unsigned int       caller_class;     /* WEX_CALLER_*, from the server (H3) */
    BOOL               security_priv;    /* SeSecurityPrivilege enabled */
    DWORD              client_pid;       /* tracing only, never for decisions */
    struct log_handle  handles[MAX_CONN_HANDLES];
};

static struct list connections = LIST_INIT( connections );
static struct list waiting = LIST_INIT( waiting );  /* connections that will read when the budget allows */

static void start_read( struct connection *conn );
static void conn_close( struct connection *conn );
static BOOL listen_next( const struct listener *listener );

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

static void *budget_alloc( size_t size )
{
    void *p = malloc( size ? size : 1 );
    if (p) budget_used += size;
    return p;
}

static void budget_free( void *p, size_t size )
{
    if (!p) return;
    free( p );
    budget_used -= size;
    /* connections that waited for room read again */
    while (budget_used < BUDGET && !list_empty( &waiting ))
    {
        struct connection *conn = LIST_ENTRY( list_head( &waiting ), struct connection, entry );
        list_remove( &conn->entry );
        list_add_tail( &connections, &conn->entry );
        start_read( conn );
    }
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

static void send_reply( struct connection *conn, const struct wex_evtlog_header *hdr, const BYTE *body, DWORD size );

/* a record was written to log (or it was cleared): every NOTIFY waiting on it completes */
void notify_log( struct evt_log *log )
{
    struct connection *conn, *next;

    LIST_FOR_EACH_ENTRY_SAFE( conn, next, &connections, struct connection, entry )
    {
        struct wex_evtlog_header hdr;

        if (!conn->notify || conn->notify->log != log || conn->closing) continue;
        memset( &hdr, 0, sizeof(hdr) );
        hdr.op = WEX_EVTLOG_NOTIFY;
        hdr.handle = (conn->notify - conn->handles) + 1;
        conn->notify = NULL;
        conn->account->subscriptions--;
        send_reply( conn, &hdr, NULL, 0 );
    }
}

/* the service's own record in the System log (source "EventLog") */
void report_system_event( WORD type, DWORD id, const WCHAR *string )
{
    struct evt_log *log = store_get_log( L"System", TRUE );
    DWORD size, status, strings_size = string ? (wcslen( string ) + 1) * sizeof(WCHAR) : 0;
    BYTE *rec;

    if (!log) return;
    if (!(rec = build_record( L"EventLog", type, 0, id, NULL, 0, string, strings_size, string ? 1 : 0, NULL, 0,
                              &size, &status )))
        return;
    if (!store_append( log, rec, size )) notify_log( log );
    free( rec );
}

/* ---- access (docs/EVENTLOG_DESIGN.md, "Access rules") ---- */

/* the rights the caller of conn has on log */
static DWORD log_rights( struct connection *conn, struct evt_log *log )
{
    static GENERIC_MAPPING mapping = { RIGHT_READ, RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE | RIGHT_CLEAR,
                                       RIGHT_READ | RIGHT_WRITE | RIGHT_CLEAR };
    const WCHAR *sddl;
    PSECURITY_DESCRIPTOR sd;
    PRIVILEGE_SET privs;
    DWORD privs_size, granted, desired, rights = 0;
    BOOL ok, system = !wcsicmp( log->name, L"System" );

    /* Security: the system (session 0's own, system-local) and a caller with SeSecurityPrivilege enabled read and clear
     * it; nobody writes it until auditing exists */
    if (log->is_security)
        return conn->security_priv || conn->caller_class == WEX_CALLER_SYSTEM_LOCAL ? RIGHT_READ | RIGHT_CLEAR : 0;
    sddl = log->custom_sd ? log->custom_sd : system ? system_sd : application_sd;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW( sddl, SDDL_REVISION_1, &sd, NULL ))
        return 0;  /* a CustomSD that does not parse gives nothing */
    for (desired = RIGHT_READ; desired <= RIGHT_CLEAR; desired <<= 1)
    {
        privs_size = sizeof(privs);
        if (AccessCheck( sd, conn->token, desired, &mapping, &privs, &privs_size, &granted, &ok ) && ok)
            rights |= desired;
    }
    LocalFree( sd );
    /* E1, L1's form: a service account that the server does not call this server's own system (a session's system role,
     * over the bridge) counts as its user: read and write, read only on System, never clear (H3: get_caller_class) */
    if (conn->caller_class != WEX_CALLER_SYSTEM_LOCAL && conn->service_account)
        rights &= system ? RIGHT_READ : RIGHT_READ | RIGHT_WRITE;
    return rights;
}

/* A session's caller (over the bridge) is checked without the SERVICE group (S-1-5-6) and per-service SIDs
 * (S-1-5-80-*): a session's token is believed for its user and groups under T1, but a service group in it is no more
 * trustworthy than a session's SYSTEM token (E1). They become deny-only; real services write from session 0. */
static BOOL restrict_service_groups( struct connection *conn )
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    SID_AND_ATTRIBUTES disable[64];
    TOKEN_GROUPS *groups;
    DWORD size = 0, i, count = 0;
    HANDLE restricted;
    BOOL ok;

    GetTokenInformation( conn->token, TokenGroups, NULL, 0, &size );
    if (!size || !(groups = malloc( size ))) return FALSE;
    if (!GetTokenInformation( conn->token, TokenGroups, groups, size, &size ))
    {
        free( groups );
        return FALSE;
    }
    for (i = 0; i < groups->GroupCount && count < ARRAY_SIZE(disable); i++)
    {
        PSID sid = groups->Groups[i].Sid;
        SID_IDENTIFIER_AUTHORITY *auth = GetSidIdentifierAuthority( sid );
        DWORD rid0 = *GetSidSubAuthorityCount( sid ) ? *GetSidSubAuthority( sid, 0 ) : 0;

        if (memcmp( auth, &nt, sizeof(nt) )) continue;
        if (rid0 != SECURITY_SERVICE_RID && rid0 != 80 /* SECURITY_SERVICE_ID_BASE_RID */) continue;
        disable[count].Sid = sid;
        disable[count].Attributes = 0;
        count++;
    }
    if (!count)
    {
        free( groups );
        return TRUE;
    }
    ok = CreateRestrictedToken( conn->token, 0, count, disable, 0, NULL, 0, NULL, &restricted );
    free( groups );
    if (!ok) return FALSE;  /* refused rather than believed */
    CloseHandle( conn->token );
    conn->token = restricted;
    return TRUE;
}

/* identify the caller: its token at identification level, held for the connection; FALSE to refuse it */
static BOOL identify( struct connection *conn )
{
    static const LUID_AND_ATTRIBUTES none = { { 0, 0 }, 0 };
    BYTE user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)user_buf;
    PRIVILEGE_SET privs = { 1, PRIVILEGE_SET_ALL_NECESSARY, { none } };
    DWORD size = sizeof(user_buf);
    BOOL held = FALSE;
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
    conn->service_account = IsWellKnownSid( user->User.Sid, WinLocalSystemSid ) ||
                            IsWellKnownSid( user->User.Sid, WinLocalServiceSid ) ||
                            IsWellKnownSid( user->User.Sid, WinNetworkServiceSid );
    if (!conn->listener->local && !restrict_service_groups( conn )) return FALSE;
    conn->caller_class = wex_caller_class_of_pipe( conn->pipe );  /* the server decides (H3) */
    LookupPrivilegeValueW( NULL, L"SeSecurityPrivilege", &privs.Privilege[0].Luid );
    conn->security_priv = PrivilegeCheck( conn->token, &privs, &held ) && held;
    /* a local connection in session 0 must be a session 0 process (a session reaches only the bridged name; a local
     * process that connects to the bridged name is merely classed as a session's caller, a harmless downgrade) */
    if (conn->listener->local && in_session0 && GetNamedPipeClientSessionId( conn->pipe, &session ) && session)
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
    BYTE *body;      /* from budget_alloc */
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
    rights = log_rights( conn, log );
    TRACE( "pid %04lx opens %s of %s: class %u, service account %d, local %d, rights %#lx\n", conn->client_pid,
           debugstr_w(name), debugstr_w(log->name), conn->caller_class, conn->service_account,
           conn->listener->local, rights );
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
    if (conn->notify == h)
    {
        conn->notify = NULL;
        conn->account->subscriptions--;
    }
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
    if (!status) notify_log( h->log );
    TRACE( "pid %04lx: event %lu into %s: %lu\n", conn->client_pid, rep.event_id, debugstr_w(h->log->name), status );
    return status;
}

/* the records a read returns, into reply: as many as fit in the client's buffer (at most WEX_EVTLOG_MAX_READ); the
 * reply is sized to them, never to what the request asks */
static DWORD do_read( struct log_handle *h, const struct wex_evtlog_header *req, const BYTE *body, DWORD size,
                      struct reply *reply )
{
    DWORD flags = req->arg[0], bufsize, oldest = store_oldest( h->log ), newest, start, number, used = 0;
    BOOL forwards, seek;
    struct evt_record *rec;

    if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
    if (size != sizeof(DWORD)) return ERROR_INVALID_PARAMETER;
    memcpy( &bufsize, body, sizeof(bufsize) );
    if (bufsize > WEX_EVTLOG_MAX_READ) bufsize = WEX_EVTLOG_MAX_READ;
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
    if (number < oldest && number) number = oldest;  /* the ring dropped what the read had not reached */
    start = number;
    /* first the size of what fits, then the reply of exactly that size */
    while (number >= oldest && number <= newest && (rec = store_find( h->log, number )))
    {
        if (used + rec->size > bufsize)
        {
            if (!used)
            {
                reply->hdr.arg[0] = rec->size;
                return ERROR_INSUFFICIENT_BUFFER;
            }
            break;
        }
        used += rec->size;
        number = forwards ? number + 1 : number - 1;
    }
    if (!used) return ERROR_HANDLE_EOF;
    if (!(reply->body = budget_alloc( used ))) return ERROR_NOT_ENOUGH_MEMORY;
    reply->body_size = used;
    for (used = 0, number = start; used < reply->body_size; number = forwards ? number + 1 : number - 1)
    {
        rec = store_find( h->log, number );
        memcpy( reply->body + used, rec->data, rec->size );
        used += rec->size;
    }
    h->position = number ? number : (DWORD)-1;  /* backwards past the first record: nothing more */
    store_touch_read( h->log );
    return ERROR_SUCCESS;
}

/* records from a number on, for a backup the client writes (E3); reply arg[0]: the next number */
static DWORD do_stream( struct log_handle *h, const struct wex_evtlog_header *req, struct reply *reply )
{
    DWORD number = req->arg[0] ? req->arg[0] : store_oldest( h->log ), used = 0, next;
    struct evt_record *rec;

    if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
    if (number < store_oldest( h->log )) number = store_oldest( h->log );
    if (!number || number >= h->log->next) return ERROR_HANDLE_EOF;
    next = number;
    LIST_FOR_EACH_ENTRY( rec, &h->log->records, struct evt_record, entry )
    {
        if (rec->number < number) continue;
        if (used + rec->size > WEX_EVTLOG_MAX_READ) break;
        used += rec->size;
        next = rec->number + 1;
    }
    if (!used) return ERROR_HANDLE_EOF;
    if (!(reply->body = budget_alloc( used ))) return ERROR_NOT_ENOUGH_MEMORY;
    reply->body_size = used;
    used = 0;
    LIST_FOR_EACH_ENTRY( rec, &h->log->records, struct evt_record, entry )
    {
        if (rec->number < number || rec->number >= next) continue;
        memcpy( reply->body + used, rec->data, rec->size );
        used += rec->size;
    }
    reply->hdr.arg[0] = next;
    store_touch_read( h->log );
    return ERROR_SUCCESS;
}

/* N6: a clear after a backup names the last record the backup has; newer records make the client stream the tail */
static DWORD do_clear( struct connection *conn, struct log_handle *h, const struct wex_evtlog_header *req )
{
    DWORD status;

    if (!(h->rights & RIGHT_CLEAR)) return ERROR_ACCESS_DENIED;
    TRACE( "pid %04lx clears %s (mode %u, backup to %u)\n", conn->client_pid, debugstr_w(h->log->name), req->arg[0],
           req->arg[1] );
    /* mode 2, after the client's bounded retries against a steady writer: clear through the backup's last record
     * and keep the newer ones, so that nothing is lost and the clear always ends */
    if (req->arg[0] == 2) status = store_clear_through( h->log, req->arg[1] );
    else if (req->arg[0] && h->log->next - 1 > req->arg[1]) return ERROR_RETRY;
    else status = store_clear( h->log );
    if (!status) notify_log( h->log );
    return status;
}

static DWORD do_info( struct log_handle *h, struct reply *reply )
{
    struct wex_evtlog_information info;

    if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
    store_information( h->log, &info );
    if (!(reply->body = budget_alloc( sizeof(info) ))) return ERROR_NOT_ENOUGH_MEMORY;
    memcpy( reply->body, &info, sizeof(info) );
    reply->body_size = sizeof(info);
    reply->hdr.arg[0] = info.full;
    return ERROR_SUCCESS;
}

/* a request; WEX_EVTLOG_NOTIFY parks the connection (no reply now) and returns ERROR_IO_PENDING */
static DWORD dispatch( struct connection *conn, const struct wex_evtlog_header *req, const BYTE *body, DWORD size,
                       struct reply *reply )
{
    struct log_handle *h = NULL;

    if (req->op == WEX_EVTLOG_CALLER)
    {
        reply->hdr.arg[0] = conn->caller_class;  /* who the server says this caller is (tests of H3) */
        return ERROR_SUCCESS;
    }
    if (req->op != WEX_EVTLOG_OPEN && !(h = get_handle( conn, req->handle ))) return ERROR_INVALID_HANDLE;
    switch (req->op)
    {
    case WEX_EVTLOG_OPEN:   return do_open( conn, body, size, reply );
    case WEX_EVTLOG_CLOSE:  close_handle( conn, h ); return ERROR_SUCCESS;
    case WEX_EVTLOG_REPORT: return do_report( conn, h, body, size );
    case WEX_EVTLOG_READ:   return do_read( h, req, body, size, reply );
    case WEX_EVTLOG_COUNT:
        if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
        reply->hdr.arg[0] = h->log->count;
        return ERROR_SUCCESS;
    case WEX_EVTLOG_OLDEST:
        if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
        reply->hdr.arg[0] = store_oldest( h->log );
        return ERROR_SUCCESS;
    case WEX_EVTLOG_INFO:   return do_info( h, reply );
    case WEX_EVTLOG_CLEAR:  return do_clear( conn, h, req );
    case WEX_EVTLOG_STREAM: return do_stream( h, req, reply );
    case WEX_EVTLOG_NOTIFY:
        if (!(h->rights & RIGHT_READ)) return ERROR_ACCESS_DENIED;
        if (conn->account->subscriptions >= WEX_EVTLOG_MAX_SUBSCRIPTIONS) return ERROR_NOT_ENOUGH_QUOTA;
        conn->account->subscriptions++;
        conn->notify = h;
        return ERROR_IO_PENDING;
    default:                return ERROR_INVALID_FUNCTION;  /* an unknown operation is refused */
    }
}

/* ---- the connections' I/O ---- */

static void conn_free_if_done( struct connection *conn )
{
    DWORD i;

    if (!conn->closing || conn->pending) return;
    list_remove( &conn->entry );
    if (conn->identified)
    {
        for (i = 0; i < MAX_CONN_HANDLES; i++) if (conn->handles[i].log) close_handle( conn, &conn->handles[i] );
        conn->account->connections--;
    }
    if (conn->token) CloseHandle( conn->token );
    budget_free( conn->write_buf, conn->write_size );
    CloseHandle( conn->pipe );
    free( conn );
}

static void conn_close( struct connection *conn )
{
    if (conn->closing) return;
    conn->closing = TRUE;
    if (conn->notify)
    {
        conn->notify = NULL;
        conn->account->subscriptions--;  /* N4: a NOTIFY ends with its client */
    }
    CancelIoEx( conn->pipe, NULL );
    DisconnectNamedPipe( conn->pipe );
    /* freed by sweep_closed once no I/O of it is pending, never here: callers still use it */
}

/* the one place where closed connections are freed: after each completion has been handled */
static void sweep_closed(void)
{
    struct connection *conn, *next;

    LIST_FOR_EACH_ENTRY_SAFE( conn, next, &connections, struct connection, entry ) conn_free_if_done( conn );
    LIST_FOR_EACH_ENTRY_SAFE( conn, next, &waiting, struct connection, entry ) conn_free_if_done( conn );
}

static void send_reply( struct connection *conn, const struct wex_evtlog_header *hdr, const BYTE *body, DWORD size )
{
    struct wex_evtlog_header out = *hdr;

    out.magic = WEX_EVTLOG_MAGIC;
    out.version = WEX_EVTLOG_VERSION;
    out.size = sizeof(out) + size;
    if (!(conn->write_buf = budget_alloc( out.size )))
    {
        conn_close( conn );
        return;
    }
    conn->write_size = out.size;
    memcpy( conn->write_buf, &out, sizeof(out) );
    if (size) memcpy( conn->write_buf + sizeof(out), body, size );
    memset( &conn->io_write.ov, 0, sizeof(conn->io_write.ov) );
    conn->pending++;
    if (!WriteFile( conn->pipe, conn->write_buf, conn->write_size, NULL, &conn->io_write.ov ) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        conn->pending--;
        conn_close( conn );
    }
}

static void start_read( struct connection *conn )
{
    if (conn->closing || conn->reading) return;
    if (budget_used >= BUDGET)
    {
        /* no room: this connection reads when replies have gone out */
        list_remove( &conn->entry );
        list_add_tail( &waiting, &conn->entry );
        return;
    }
    memset( &conn->io_read.ov, 0, sizeof(conn->io_read.ov) );
    conn->reading = TRUE;
    conn->pending++;
    if (!ReadFile( conn->pipe, conn->small, sizeof(conn->small), NULL, &conn->io_read.ov ) &&
        GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_MORE_DATA)
    {
        conn->reading = FALSE;
        conn->pending--;
        conn_close( conn );
    }
}

/* the rest of a message larger than the first part: after a peek, read whole into a buffer of exactly its size */
static BYTE *read_rest( struct connection *conn, DWORD got, DWORD *total )
{
    OVERLAPPED ov;
    DWORD left = 0, more = 0;
    BYTE *buf;

    if (!PeekNamedPipe( conn->pipe, NULL, 0, NULL, NULL, &left ) || (ULONGLONG)got + left > WEX_EVTLOG_MAX_MSG)
        return NULL;  /* over the limit: a malformed frame closes the connection */
    if (!(buf = budget_alloc( got + left ))) return NULL;
    memcpy( buf, conn->small, got );
    memset( &ov, 0, sizeof(ov) );
    ov.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL );
    /* the data is in the pipe already: this completes at once */
    if ((!ReadFile( conn->pipe, buf + got, left, NULL, &ov ) && GetLastError() != ERROR_IO_PENDING) ||
        !GetOverlappedResult( conn->pipe, &ov, &more, TRUE ) || more != left)
    {
        CloseHandle( ov.hEvent );
        budget_free( buf, got + left );
        return NULL;
    }
    CloseHandle( ov.hEvent );
    *total = got + left;
    return buf;
}

static void handle_message( struct connection *conn, const BYTE *buf, DWORD size )
{
    struct wex_evtlog_header req;
    struct reply reply;

    if (size < sizeof(req)) goto bad;
    memcpy( &req, buf, sizeof(req) );
    if (req.magic != WEX_EVTLOG_MAGIC || req.size != size) goto bad;  /* malformed: the connection closes */
    memset( &reply, 0, sizeof(reply) );
    reply.hdr.op = req.op;
    reply.hdr.handle = req.handle;
    if (req.version != WEX_EVTLOG_VERSION) reply.hdr.status = ERROR_REVISION_MISMATCH;
    else reply.hdr.status = dispatch( conn, &req, buf + sizeof(req), size - sizeof(req), &reply );
    if (reply.hdr.status == ERROR_IO_PENDING)
    {
        /* a NOTIFY: a read stays posted, so that the client's disconnect ends it (N4); a request meanwhile is a
         * protocol error */
        start_read( conn );
        return;
    }
    send_reply( conn, &reply.hdr, reply.body, reply.body_size );
    budget_free( reply.body, reply.body_size );
    return;

bad:
    conn_close( conn );
}

static void on_read( struct connection *conn, DWORD error, DWORD got )
{
    BYTE *buf = conn->small;
    DWORD total = got;

    conn->reading = FALSE;
    if (conn->closing) return;
    if (error == ERROR_MORE_DATA)
    {
        if (!(buf = read_rest( conn, got, &total )))
        {
            conn_close( conn );
            return;
        }
    }
    else if (error || !got)
    {
        conn_close( conn );  /* the client went away */
        return;
    }
    if (conn->notify)
    {
        conn_close( conn );  /* a request while a NOTIFY is in flight: one request at a time */
        if (buf != conn->small) budget_free( buf, total );
        return;
    }
    handle_message( conn, buf, total );
    if (buf != conn->small) budget_free( buf, total );
}

static void on_write( struct connection *conn, DWORD error )
{
    budget_free( conn->write_buf, conn->write_size );
    conn->write_buf = NULL;
    conn->write_size = 0;
    if (error) conn_close( conn );
    else start_read( conn );
}

static void on_connect( struct connection *conn, DWORD error )
{
    listen_next( conn->listener );  /* the next client gets a new instance */
    if (error && error != ERROR_PIPE_CONNECTED)
    {
        conn_close( conn );
        return;
    }
    conn->connected = TRUE;
    if (!(conn->identified = identify( conn )))
    {
        conn_close( conn );
        return;
    }
    start_read( conn );
}

/* a new listening instance of the listener's pipe */
static BOOL listen_next( const struct listener *listener )
{
    static SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    struct listener *l = (struct listener *)listener;
    struct connection *conn;
    DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;

    if (stopping) return FALSE;
    /* every account may connect; the service decides what each may do. Anonymous callers are refused at connect */
    if (!sa.lpSecurityDescriptor)
        ConvertStringSecurityDescriptorToSecurityDescriptorW( L"D:P(A;;GRGW;;;WD)(A;;GA;;;SY)", SDDL_REVISION_1,
                                                              &sa.lpSecurityDescriptor, NULL );
    if (!(conn = calloc( 1, sizeof(*conn) ))) return FALSE;
    conn->listener = listener;
    /* N2: the first instance proves no one else serves the name (a second listener, or a stale instance) */
    if (l->first) flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    conn->pipe = CreateNamedPipeW( listener->name, flags, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                   PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa );
    if (conn->pipe == INVALID_HANDLE_VALUE)
    {
        ERR( "cannot create %s: %lu\n", debugstr_w(listener->name), GetLastError() );
        free( conn );
        return FALSE;
    }
    l->first = FALSE;
    if (!CreateIoCompletionPort( conn->pipe, port, (ULONG_PTR)conn, 0 ))
    {
        CloseHandle( conn->pipe );
        free( conn );
        return FALSE;
    }
    conn->io_connect.kind = IO_CONNECT;
    conn->io_read.kind = IO_READ;
    conn->io_write.kind = IO_WRITE;
    conn->io_connect.conn = conn->io_read.conn = conn->io_write.conn = conn;
    list_add_tail( &connections, &conn->entry );
    conn->pending++;
    if (!ConnectNamedPipe( conn->pipe, &conn->io_connect.ov ))
    {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
        {
            /* connected before the call: no completion comes */
            conn->pending--;
            on_connect( conn, 0 );
        }
        else if (error != ERROR_IO_PENDING)
        {
            conn->pending--;
            conn_close( conn );
        }
    }
    return TRUE;
}

/* ---- the loop ---- */

static void reload_settings( HKEY key, HANDLE event )
{
    store_refresh_all();
    if (key) RegNotifyChangeKeyValue( key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, event, TRUE );
}

static void serve(void)
{
    HANDLE changed = CreateEventW( NULL, FALSE, FALSE, NULL );
    DWORD last_flush = GetTickCount();
    HKEY key = NULL;

    /* section 9: a change of a log's MaxSize, Retention or CustomSD takes effect without a restart */
    RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\EventLog", 0, KEY_NOTIFY | KEY_READ, &key );
    if (key) RegNotifyChangeKeyValue( key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE );
    while (!stopping)
    {
        OVERLAPPED *ov = NULL;
        ULONG_PTR key_value;
        DWORD bytes = 0, error = 0;

        if (!GetQueuedCompletionStatus( port, &bytes, &key_value, &ov, 1000 )) error = GetLastError();
        if (ov)
        {
            struct io *io = CONTAINING_RECORD( ov, struct io, ov );
            struct connection *conn = io->conn;

            conn->pending--;
            switch (io->kind)
            {
            case IO_CONNECT: on_connect( conn, error ); break;
            case IO_READ:    on_read( conn, error, bytes ); break;
            case IO_WRITE:   on_write( conn, error ); break;
            }
        }
        sweep_closed();
        if (WaitForSingleObject( changed, 0 ) == WAIT_OBJECT_0) reload_settings( key, changed );
        /* at most one flush a second (E5) */
        if (GetTickCount() - last_flush >= 1000)
        {
            store_flush_all();
            last_flush = GetTickCount();
        }
    }
    store_flush_all();
    if (key) RegCloseKey( key );
    CloseHandle( changed );
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
        PostQueuedCompletionStatus( port, 0, 0, NULL );
        break;

    default:
        eventlog_update_status(SERVICE_RUNNING);
        break;
    }
}

void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    static struct listener local = { WEX_EVTLOG_PIPE, TRUE, TRUE }, bridged = { WEX_EVTLOG_PIPE_BRIDGED, FALSE, TRUE };
    WCHAR dir[MAX_PATH];
    DWORD session = 1, len = ARRAY_SIZE(computer_name);

    TRACE("starting Event Log Service\n");

    svc_handle = RegisterServiceCtrlHandlerW(L"EventLog", eventlog_handler);
    if (!svc_handle)
    {
        ERR("RegisterServiceCtrlHandler error %lu\n", GetLastError());
        return;
    }

    port = CreateIoCompletionPort( INVALID_HANDLE_VALUE, NULL, 0, 1 );
    GetComputerNameW( computer_name, &len );
    ProcessIdToSessionId( GetCurrentProcessId(), &session );
    in_session0 = !session;
    GetSystemDirectoryW( dir, ARRAY_SIZE(dir) - 8 );
    lstrcatW( dir, L"\\config" );
    CreateDirectoryW( dir, NULL );
    store_init( dir );
    store_get_log( L"Application", TRUE );
    store_get_log( L"System", TRUE );
    store_get_log( L"Security", TRUE );  /* created now, empty: written once auditing exists */

    listen_next( &local );
    if (in_session0) listen_next( &bridged );
    eventlog_update_status(SERVICE_RUNNING);
    serve();
    eventlog_update_status(SERVICE_STOPPED);

    TRACE("exiting Event Log Service\n");
}
