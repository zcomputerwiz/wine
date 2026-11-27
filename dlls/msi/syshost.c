/*
 * WineExplorer: deferred custom actions without impersonation run as SYSTEM in session 0
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

/* Windows Installer runs a deferred custom action that has msidbCustomActionTypeNoImpersonate as LocalSystem when the
 * install is elevated and per machine. Under WineExplorer the elevated msiexec runs in the user's server, so such an
 * action runs in a host in session 0 instead (WineExplorer's docs/MSI_NOIMPERSONATE_SYSTEM.md, option B):
 *
 *  - the client (the elevated msiexec running the install) opens the session 0 service WexMsiServer over the pipe
 *    bridge; the service checks the caller and starts the host (msiexec -WexSysHost, SYSTEM in session 0), which takes
 *    over that one connection: the host has no other client;
 *  - the client sends each action's binary; the host writes it into a folder of its own (SYSTEM and Administrators)
 *    and loads or starts it from there: the client never names a path the host runs;
 *  - a DLL action calls msi back (MsiGetProperty, MsiProcessMessage) over ncalrpc as in the ordinary custom action
 *    server; the host serves a local endpoint for it and relays its RPC channels over the connection, and the client
 *    passes on only what a deferred action may do: the msi interface, the calls a deferred action makes, and only
 *    the restricted handle it gave that action (msi_handle_is_restricted), whose properties are CustomActionData,
 *    ProductCode and UserSID (s_remote_GetProperty).
 *
 * Without session 0 (a plain prefix, development mode) the service is not there and actions run as the user, as
 * before. */

#include <stdarg.h>
#include <stdio.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#include "sddl.h"
#include "msidefs.h"
#include "winuser.h"
#include "objbase.h"
#include "oleauto.h"

#include "msipriv.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "wine/wex_eventlog.h"
#include "wine/wex_msisys.h"

WINE_DEFAULT_DEBUG_CHANNEL(msi);

/* RPC PDUs (DCE/RPC connection-oriented, as rpcrt4 sends them over ncalrpc) */
#define PDU_HDR_SIZE        16
#define PDU_REQUEST         0
#define PDU_FAULT           3
#define PDU_BIND            11
#define PDU_ALTER_CONTEXT   14
#define PDU_FIRST_FRAG      0x01
#define PDU_LAST_FRAG       0x02
#define PDU_OBJECT_UUID     0x80
#define PDU_MAX_FRAG        0x10000

/* IWineMsiRemote (winemsi.idl): the calls a deferred action makes, by operation number */
static const GUID msi_remote_iid = {0x56d58b64, 0x8780, 0x4c22, {0xa8, 0xbc, 0x8b, 0x0b, 0x29, 0xe4, 0xa9, 0xf8}};
#define OP_GETPROPERTY      13
#define OP_PROCESSMESSAGE   15
#define OP_GETMODE          21
#define OP_GETLANGUAGE      27
#define OP_FORMATRECORD     29
#define OP_GETACTIONINFO    33
#define OP_CLOSEHANDLE      34

static BOOL read_full( HANDLE h, void *buffer, DWORD size )
{
    BYTE *p = buffer;
    DWORD got;

    while (size)
    {
        if (!ReadFile( h, p, size, &got, NULL ) && GetLastError() != ERROR_MORE_DATA) return FALSE;
        if (!got) return FALSE;
        p += got;
        size -= got;
    }
    return TRUE;
}

static BOOL write_full( HANDLE h, const void *buffer, DWORD size )
{
    const BYTE *p = buffer;
    DWORD done;

    while (size)
    {
        if (!WriteFile( h, p, size, &done, NULL ) || !done) return FALSE;
        p += done;
        size -= done;
    }
    return TRUE;
}

/* one PDU from a message-mode pipe: the header, then the rest of the fragment */
static BYTE *read_pdu( HANDLE h, DWORD *size )
{
    BYTE hdr[PDU_HDR_SIZE], *pdu;
    USHORT frag;

    if (!read_full( h, hdr, sizeof(hdr) )) return NULL;
    memcpy( &frag, hdr + 8, sizeof(frag) );
    if (frag < PDU_HDR_SIZE) return NULL;
    if (!(pdu = malloc( frag ))) return NULL;
    memcpy( pdu, hdr, sizeof(hdr) );
    if (frag > PDU_HDR_SIZE && !read_full( h, pdu + PDU_HDR_SIZE, frag - PDU_HDR_SIZE ))
    {
        free( pdu );
        return NULL;
    }
    *size = frag;
    return pdu;
}

static BOOL send_frame( HANDLE pipe, CRITICAL_SECTION *cs, UINT32 type, UINT32 channel, const void *data1,
                        UINT32 size1, const void *data2, UINT32 size2 )
{
    struct wex_msisys_frame frame = { type, channel, size1 + size2 };
    BOOL ret;

    EnterCriticalSection( cs );
    ret = write_full( pipe, &frame, sizeof(frame) ) && (!size1 || write_full( pipe, data1, size1 )) &&
          (!size2 || write_full( pipe, data2, size2 ));
    LeaveCriticalSection( cs );
    return ret;
}

static BYTE *read_frame( HANDLE pipe, struct wex_msisys_frame *frame )
{
    BYTE *data;

    if (!read_full( pipe, frame, sizeof(*frame) )) return NULL;
    if (frame->size > WEX_MSISYS_MAX_FRAME) return NULL;
    if (!(data = malloc( frame->size ? frame->size : 1 ))) return NULL;
    if (frame->size && !read_full( pipe, data, frame->size ))
    {
        free( data );
        return NULL;
    }
    return data;
}

/******************************************************************************
 * the client: the elevated msiexec running the install
 */

struct relay_channel
{
    struct list entry;
    struct msi_syshost *host;
    UINT32 id;
    HANDLE local;           /* our connection to this process's own msi endpoint */
    HANDLE thread;          /* the pump; cancelling its read ends the channel */
    volatile BOOL closing;  /* for a pump that was not in its read when cancelled */
    UINT32 pass_call;       /* a request whose first fragment was allowed: its further fragments pass */
    BOOL   passing;
    UINT32 drop_call;       /* a request that was refused: its further fragments are dropped */
    BOOL   dropping;
};

struct run_wait
{
    struct list entry;
    GUID guid;
    HANDLE event;
    UINT status;
    UINT code;
};

struct msi_syshost
{
    HANDLE pipe;
    DWORD pid;
    HANDLE reader;
    CRITICAL_SECTION write_cs;
    CRITICAL_SECTION run_cs;  /* a RUN frame and its binary's chunks go together */
    CRITICAL_SECTION cs;    /* channels, waits, dead */
    struct list channels;
    struct list waits;
    BOOL dead;
};

BOOL msi_syshost_wanted( MSIPACKAGE *package, UINT type )
{
    TOKEN_ELEVATION_TYPE elevation;
    DWORD size;

    if ((type & (msidbCustomActionTypeInScript | msidbCustomActionTypeNoImpersonate)) !=
            (msidbCustomActionTypeInScript | msidbCustomActionTypeNoImpersonate))
        return FALSE;
    if (package->syshost_unavailable) return FALSE;
    /* Windows: SYSTEM only for an elevated per-machine install; a per-user install runs the action as the user. A
     * full token or an unfiltered administrator's (default type: UAC off, the built-in Administrator) asks; a filtered
     * one never does. The service decides whether the caller is an elevated administrator. */
    if (package->Context != MSIINSTALLCONTEXT_MACHINE) return FALSE;
    if (!GetTokenInformation( GetCurrentThreadEffectiveToken(), TokenElevationType, &elevation, sizeof(elevation),
                              &size ) || elevation == TokenElevationTypeLimited)
        return FALSE;
    return TRUE;
}

static void fail_waits( struct msi_syshost *host )
{
    struct run_wait *wait;

    EnterCriticalSection( &host->cs );
    host->dead = TRUE;
    LIST_FOR_EACH_ENTRY( wait, &host->waits, struct run_wait, entry )
    {
        wait->status = ERROR_BROKEN_PIPE;
        SetEvent( wait->event );
    }
    LeaveCriticalSection( &host->cs );
}

static struct relay_channel *find_channel( struct msi_syshost *host, UINT32 id )
{
    struct relay_channel *channel;

    LIST_FOR_EACH_ENTRY( channel, &host->channels, struct relay_channel, entry )
        if (channel->id == id) return channel;
    return NULL;
}

/* our own msi endpoint's answers go back to the host's channel */
static DWORD WINAPI relay_pump( void *arg )
{
    struct relay_channel *channel = arg;
    struct msi_syshost *host = channel->host;
    BYTE *pdu;
    DWORD size;

    while (!channel->closing && (pdu = read_pdu( channel->local, &size )))
    {
        BOOL ok = send_frame( host->pipe, &host->write_cs, WEX_MSISYS_DATA, channel->id, pdu, size, NULL, 0 );
        free( pdu );
        if (!ok) break;
    }
    send_frame( host->pipe, &host->write_cs, WEX_MSISYS_CLOSE, channel->id, NULL, 0, NULL, 0 );

    /* once off the list nobody else touches the channel's handles */
    EnterCriticalSection( &host->cs );
    list_remove( &channel->entry );
    LeaveCriticalSection( &host->cs );
    CloseHandle( channel->local );
    CloseHandle( channel->thread );
    free( channel );
    return 0;
}

static void open_channel( struct msi_syshost *host, UINT32 id )
{
    struct relay_channel *channel;
    WCHAR name[48];
    DWORD mode = PIPE_READMODE_MESSAGE;
    HANDLE thread;

    swprintf( name, ARRAY_SIZE(name), L"\\\\.\\pipe\\lrpc\\msi%x", GetCurrentProcessId() );
    if (!(channel = calloc( 1, sizeof(*channel) ))) goto refuse;
    channel->host = host;
    channel->id = id;
    channel->local = CreateFileW( name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
    if (channel->local == INVALID_HANDLE_VALUE)
    {
        ERR( "cannot open our own msi endpoint: %lu\n", GetLastError() );
        free( channel );
        goto refuse;
    }
    SetNamedPipeHandleState( channel->local, &mode, NULL, NULL );

    if (!(thread = CreateThread( NULL, 0, relay_pump, channel, CREATE_SUSPENDED, NULL )))
    {
        CloseHandle( channel->local );
        free( channel );
        goto refuse;
    }
    channel->thread = thread;
    EnterCriticalSection( &host->cs );
    list_add_tail( &host->channels, &channel->entry );
    LeaveCriticalSection( &host->cs );
    ResumeThread( thread );
    return;

refuse:
    send_frame( host->pipe, &host->write_cs, WEX_MSISYS_CLOSE, id, NULL, 0, NULL, 0 );
}

enum relay_verdict { RELAY_PASS, RELAY_FAULT, RELAY_DROP, RELAY_CLOSE };

/* bind and alter context: only the msi interface */
static enum relay_verdict check_bind( const BYTE *pdu, DWORD size )
{
    DWORD pos = PDU_HDR_SIZE + 8;  /* max_xmit_frag, max_recv_frag, assoc_group_id */
    BYTE count, transfers;
    unsigned int i;
    GUID iid;

    if (size < pos + 4) return RELAY_CLOSE;
    count = pdu[pos];
    pos += 4;
    if (!count) return RELAY_CLOSE;
    for (i = 0; i < count; i++)
    {
        /* p_cont_id, n_transfer_syn, reserved, abstract syntax (uuid, version), transfer syntaxes (uuid, version) */
        if (size < pos + 4 + 20) return RELAY_CLOSE;
        transfers = pdu[pos + 2];
        memcpy( &iid, pdu + pos + 4, sizeof(iid) );
        if (!IsEqualGUID( &iid, &msi_remote_iid ))
        {
            WARN( "SYSTEM custom action binds %s: refused\n", debugstr_guid(&iid) );
            return RELAY_CLOSE;
        }
        pos += 4 + 20 + transfers * 20;
    }
    return pos <= size ? RELAY_PASS : RELAY_CLOSE;
}

/* a request from the host: what a deferred action may call, on its own restricted handle */
static enum relay_verdict check_request( struct relay_channel *channel, const BYTE *pdu, DWORD size )
{
    BYTE flags = pdu[3];
    UINT32 call_id, handle;
    USHORT opnum;
    DWORD stub;
    GUID guid;

    memcpy( &call_id, pdu + 12, sizeof(call_id) );
    if (!(flags & PDU_FIRST_FRAG))
    {
        if (channel->dropping && call_id == channel->drop_call)
        {
            if (flags & PDU_LAST_FRAG) channel->dropping = FALSE;
            return RELAY_DROP;
        }
        if (!channel->passing || call_id != channel->pass_call) return RELAY_CLOSE;
        if (flags & PDU_LAST_FRAG) channel->passing = FALSE;
        return RELAY_PASS;
    }

    if (size < PDU_HDR_SIZE + 8) return RELAY_CLOSE;
    memcpy( &opnum, pdu + PDU_HDR_SIZE + 6, sizeof(opnum) );
    stub = PDU_HDR_SIZE + 8 + ((flags & PDU_OBJECT_UUID) ? 16 : 0);

    switch (opnum)
    {
    case OP_GETACTIONINFO:
        if (size < stub + sizeof(guid)) goto fault;
        memcpy( &guid, pdu + stub, sizeof(guid) );
        if (!custom_action_runs_in_syshost( &guid )) goto fault;
        break;
    case OP_GETPROPERTY:
    case OP_PROCESSMESSAGE:
    case OP_GETMODE:
    case OP_GETLANGUAGE:
    case OP_FORMATRECORD:
    case OP_CLOSEHANDLE:
        if (size < stub + sizeof(handle)) goto fault;
        memcpy( &handle, pdu + stub, sizeof(handle) );
        if (!msi_handle_is_restricted( handle )) goto fault;
        break;
    default:
        TRACE( "operation %u of a SYSTEM custom action refused\n", opnum );
        goto fault;
    }
    if (!(flags & PDU_LAST_FRAG))
    {
        channel->passing = TRUE;
        channel->pass_call = call_id;
    }
    return RELAY_PASS;

fault:
    if (!(flags & PDU_LAST_FRAG))
    {
        channel->dropping = TRUE;
        channel->drop_call = call_id;
    }
    return RELAY_FAULT;
}

static enum relay_verdict check_pdu( struct relay_channel *channel, const BYTE *pdu, DWORD size )
{
    USHORT frag, auth;

    if (size < PDU_HDR_SIZE || pdu[0] != 5 || !(pdu[4] & 0x10)) return RELAY_CLOSE;  /* version 5, little endian */
    memcpy( &frag, pdu + 8, sizeof(frag) );
    memcpy( &auth, pdu + 10, sizeof(auth) );
    if (frag != size || auth) return RELAY_CLOSE;
    switch (pdu[2])
    {
    case PDU_BIND:
    case PDU_ALTER_CONTEXT:
        return check_bind( pdu, size );
    case PDU_REQUEST:
        return check_request( channel, pdu, size );
    default:
        return RELAY_CLOSE;
    }
}

/* the refusal of a request: a fault with ERROR_ACCESS_DENIED, which msi's remote call returns as its error */
static void send_fault( struct msi_syshost *host, UINT32 id, const BYTE *request )
{
    BYTE fault[32] = { 5, 0, PDU_FAULT, PDU_FIRST_FRAG | PDU_LAST_FRAG, 0x10 };
    USHORT frag = sizeof(fault);
    UINT32 status = ERROR_ACCESS_DENIED;

    memcpy( fault + 8, &frag, sizeof(frag) );
    memcpy( fault + 12, request + 12, 4 );                  /* call id */
    memcpy( fault + 20, request + PDU_HDR_SIZE + 4, 2 );    /* presentation context */
    memcpy( fault + 24, &status, sizeof(status) );
    send_frame( host->pipe, &host->write_cs, WEX_MSISYS_DATA, id, fault, sizeof(fault), NULL, 0 );
}

static void relay_data( struct msi_syshost *host, UINT32 id, const BYTE *pdu, DWORD size )
{
    struct relay_channel *channel;
    enum relay_verdict verdict = RELAY_DROP;
    DWORD written;

    /* under the lock: the pump frees the channel only after taking it off the list */
    EnterCriticalSection( &host->cs );
    if ((channel = find_channel( host, id )))
    {
        verdict = check_pdu( channel, pdu, size );
        if (verdict == RELAY_PASS && !WriteFile( channel->local, pdu, size, &written, NULL ))
            verdict = RELAY_CLOSE;
        if (verdict == RELAY_CLOSE)
        {
            WARN( "closing a SYSTEM custom action's RPC channel %u\n", id );
            channel->closing = TRUE;
            CancelSynchronousIo( channel->thread );  /* the pump's read fails: it sends CLOSE and frees it */
        }
    }
    LeaveCriticalSection( &host->cs );
    if (verdict == RELAY_FAULT) send_fault( host, id, pdu );
}

static void close_channel( struct msi_syshost *host, UINT32 id )
{
    struct relay_channel *channel;

    EnterCriticalSection( &host->cs );
    if ((channel = find_channel( host, id )))
    {
        channel->closing = TRUE;
        CancelSynchronousIo( channel->thread );
    }
    LeaveCriticalSection( &host->cs );
}

static DWORD WINAPI client_reader( void *arg )
{
    struct msi_syshost *host = arg;
    struct wex_msisys_frame frame;
    struct run_wait *wait;
    BYTE *data;

    while ((data = read_frame( host->pipe, &frame )))
    {
        switch (frame.type)
        {
        case WEX_MSISYS_DONE:
            if (frame.size == sizeof(struct wex_msisys_done))
            {
                const struct wex_msisys_done *done = (const struct wex_msisys_done *)data;

                EnterCriticalSection( &host->cs );
                LIST_FOR_EACH_ENTRY( wait, &host->waits, struct run_wait, entry )
                {
                    if (!IsEqualGUID( &wait->guid, &done->guid )) continue;
                    wait->status = done->status;
                    wait->code = done->code;
                    SetEvent( wait->event );
                    break;
                }
                LeaveCriticalSection( &host->cs );
            }
            break;
        case WEX_MSISYS_OPEN:
            open_channel( host, frame.channel );
            break;
        case WEX_MSISYS_DATA:
            relay_data( host, frame.channel, data, frame.size );
            break;
        case WEX_MSISYS_CLOSE:
            close_channel( host, frame.channel );
            break;
        default:
            WARN( "unexpected frame %u from the SYSTEM host\n", frame.type );
            break;
        }
        free( data );
    }
    TRACE( "the SYSTEM host %04lx is gone\n", host->pid );
    fail_waits( host );
    return 0;
}

/* whether this prefix has a session 0: its service manager's pipe is reachable through the bridge (a plain prefix
 * or development mode has no session 0, and the bridge answers "not found" for any name) */
static BOOL session0_present(void)
{
    HANDLE pipe = CreateFileW( L"\\\\.\\pipe\\WexSession0\\svcctl", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL );

    if (pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle( pipe );
        return TRUE;
    }
    /* fail closed: only "not found" means no session 0 (busy, access denied and the rest mean it is there) */
    return GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND;
}

/* the install's log: why a SYSTEM action fails */
static void log_failure( MSIPACKAGE *package, const WCHAR *what, DWORD err )
{
    WCHAR text[256];
    MSIRECORD *rec;

    swprintf( text, ARRAY_SIZE(text), L"WexMsiServer (SYSTEM custom actions): %s (%lu); the action fails", what, err );
    ERR( "%s\n", debugstr_w(text) );
    if (!(rec = MSI_CreateRecord( 0 ))) return;
    MSI_RecordSetStringW( rec, 0, text );
    MSI_ProcessMessage( package, INSTALLMESSAGE_INFO, rec );
    msiobj_release( &rec->hdr );
}

UINT msi_syshost_connect( MSIPACKAGE *package, DWORD arch )
{
    struct wex_msisys_request request = { WEX_MSISYS_MAGIC, WEX_MSISYS_VERSION };
    struct wex_msisys_frame frame;
    struct msi_syshost *host;
    unsigned int index = (arch == SCS_64BIT_BINARY), tries;
    UINT32 status;
    DWORD err;
    BYTE *data;
    HANDLE pipe;

    if (package->syshost[index] && !package->syshost[index]->dead) return ERROR_SUCCESS;
    if (package->syshost[index]) return ERROR_BROKEN_PIPE;  /* the host died: the install fails, no fallback */
    if (package->syshost_unavailable) return ERROR_NOT_SUPPORTED;

    /* Only a prefix without session 0 runs the actions as the user. With session 0 a missing pipe (the service
     * stopped or crashed) fails the action, logged, and so does every other failure: the install rolls back rather
     * than changing the action's identity silently (the reviewer's and the orchestrator's conditions). The service
     * is not started from here: a session's service manager starts a session-0-only service locally, in the
     * session's own server, not in session 0 (measured in c7_msi), and that copy is of no use */
    for (tries = 0;; tries++)
    {
        /* identification only: the service checks who asks, it never acts as the user */
        pipe = CreateFileW( WEX_MSISYS_PIPE, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL );
        if (pipe != INVALID_HANDLE_VALUE) break;
        err = GetLastError();
        if ((err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) && !session0_present())
        {
            WARN( "no session 0: actions without impersonation run as the installing user\n" );
            package->syshost_unavailable = 1;
            return ERROR_NOT_SUPPORTED;
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        {
            log_failure( package, L"the session 0 service wexmsiserver is not running", err );
            return err;
        }
        if (err != ERROR_PIPE_BUSY || tries >= 5)
        {
            log_failure( package, L"the service cannot be reached", err );
            return err;
        }
        WaitNamedPipeW( WEX_MSISYS_PIPE, 5000 );
    }

    request.arch = arch == SCS_64BIT_BINARY ? 64 : 32;
    request.flags = WEX_MSISYS_PER_MACHINE;  /* msi_syshost_wanted: package->Context is MSIINSTALLCONTEXT_MACHINE */
    if (package->ProductCode) lstrcpynW( request.product, package->ProductCode, ARRAY_SIZE(request.product) );
    if (package->PackagePath) lstrcpynW( request.package, package->PackagePath, ARRAY_SIZE(request.package) );
    if (!write_full( pipe, &request, sizeof(request) ) || !read_full( pipe, &status, sizeof(status) ))
    {
        ERR( "the session 0 service did not answer: %lu\n", GetLastError() );
        CloseHandle( pipe );
        return ERROR_BROKEN_PIPE;
    }
    if (status)
    {
        /* msi_syshost_wanted asks only for an elevated per-machine install: a refusal means something is wrong */
        ERR( "the session 0 service refused a SYSTEM host: %u\n", status );
        CloseHandle( pipe );
        return status;
    }
    if (!(data = read_frame( pipe, &frame )) || frame.type != WEX_MSISYS_READY ||
            frame.size != sizeof(struct wex_msisys_ready))
    {
        ERR( "the SYSTEM host did not start\n" );
        free( data );
        CloseHandle( pipe );
        return ERROR_BROKEN_PIPE;
    }

    if (!(host = calloc( 1, sizeof(*host) )))
    {
        free( data );
        CloseHandle( pipe );
        return ERROR_OUTOFMEMORY;
    }
    host->pipe = pipe;
    host->pid = ((struct wex_msisys_ready *)data)->pid;
    free( data );
    InitializeCriticalSection( &host->write_cs );
    InitializeCriticalSection( &host->run_cs );
    InitializeCriticalSection( &host->cs );
    list_init( &host->channels );
    list_init( &host->waits );
    if (!(host->reader = CreateThread( NULL, 0, client_reader, host, 0, NULL )))
    {
        DeleteCriticalSection( &host->write_cs ); DeleteCriticalSection( &host->run_cs );
        DeleteCriticalSection( &host->cs );
        CloseHandle( pipe );
        free( host );
        return ERROR_OUTOFMEMORY;
    }
    TRACE( "SYSTEM custom action host %04lx (%u bit) in session 0\n", host->pid, request.arch );
    package->syshost[index] = host;
    return ERROR_SUCCESS;
}

/* the binary to send: open, and its size (WEX_MSISYS_MAX_BINARY at most) */
static HANDLE open_binary( const WCHAR *path, DWORD *size )
{
    LARGE_INTEGER len;
    HANDLE file;

    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return file;
    if (!GetFileSizeEx( file, &len ) || !len.QuadPart || len.QuadPart > WEX_MSISYS_MAX_BINARY)
    {
        SetLastError( len.QuadPart ? ERROR_FILE_TOO_LARGE : ERROR_INVALID_DATA );
        CloseHandle( file );
        return INVALID_HANDLE_VALUE;
    }
    *size = len.LowPart;
    return file;
}

/* the binary in CHUNK frames after its RUN frame (the caller holds run_cs: no other RUN in between); a large one
 * (Edge's installer is about 290 MB) is never held whole on either side */
static UINT send_binary( struct msi_syshost *host, HANDLE file, DWORD size )
{
    BYTE *buffer;
    DWORD done = 0, got;
    UINT ret = ERROR_SUCCESS;

    if (!(buffer = malloc( WEX_MSISYS_MAX_CHUNK ))) return ERROR_OUTOFMEMORY;
    while (done < size)
    {
        if (!ReadFile( file, buffer, min( size - done, WEX_MSISYS_MAX_CHUNK ), &got, NULL ) || !got)
        {
            ret = got ? GetLastError() : ERROR_HANDLE_EOF;
            break;
        }
        if (!send_frame( host->pipe, &host->write_cs, WEX_MSISYS_CHUNK, 0, buffer, got, NULL, 0 ))
        {
            ret = ERROR_BROKEN_PIPE;
            break;
        }
        done += got;
    }
    free( buffer );
    return ret;
}

UINT msi_syshost_run( MSIPACKAGE *package, DWORD arch, UINT kind, const GUID *guid, const WCHAR *action,
                      const WCHAR *binary, const WCHAR *entry, BOOL async, UINT *code )
{
    struct wex_msisys_run run = { *guid, kind, async ? WEX_MSISYS_RUN_ASYNC : 0 };
    struct msi_syshost *host;
    struct run_wait wait = { .guid = *guid };
    BYTE *payload;
    char *entry_a = NULL;
    HANDLE file;
    DWORD size;
    UINT r;

    if (!action) action = L"";
    run.action_size = (lstrlenW( action ) + 1) * sizeof(WCHAR);

    if ((r = msi_syshost_connect( package, arch ))) return r;
    host = package->syshost[arch == SCS_64BIT_BINARY];

    if ((file = open_binary( binary, &size )) == INVALID_HANDLE_VALUE)
    {
        UINT err = GetLastError();

        ERR( "cannot read the binary %s: %u\n", debugstr_w(binary), err );
        return err ? err : ERROR_FUNCTION_FAILED;
    }
    run.binary_size = size;
    if (kind == WEX_MSISYS_KIND_DLL)
    {
        entry_a = strdupWtoA( entry );
        run.entry_size = entry_a ? strlen( entry_a ) + 1 : 0;
    }
    else run.entry_size = (lstrlenW( entry ) + 1) * sizeof(WCHAR);
    if (sizeof(run) + run.entry_size + run.action_size > WEX_MSISYS_MAX_FRAME ||
            !(payload = malloc( sizeof(run) + run.entry_size + run.action_size )))
    {
        CloseHandle( file );
        free( entry_a );
        return ERROR_OUTOFMEMORY;
    }
    memcpy( payload, &run, sizeof(run) );
    memcpy( payload + sizeof(run), kind == WEX_MSISYS_KIND_DLL ? (void *)entry_a : (void *)entry, run.entry_size );
    memcpy( payload + sizeof(run) + run.entry_size, action, run.action_size );
    free( entry_a );

    wait.event = CreateEventW( NULL, TRUE, FALSE, NULL );
    EnterCriticalSection( &host->cs );
    if (host->dead) wait.status = ERROR_BROKEN_PIPE;
    else list_add_tail( &host->waits, &wait.entry );
    LeaveCriticalSection( &host->cs );
    if (wait.status)
    {
        CloseHandle( wait.event );
        CloseHandle( file );
        free( payload );
        return wait.status;
    }

    /* the RUN frame and its binary's chunks, with no other RUN in between */
    EnterCriticalSection( &host->run_cs );
    r = send_frame( host->pipe, &host->write_cs, WEX_MSISYS_RUN, 0, payload,
                    sizeof(run) + run.entry_size + run.action_size, NULL, 0 ) ? send_binary( host, file, size )
                                                                              : ERROR_BROKEN_PIPE;
    LeaveCriticalSection( &host->run_cs );
    CloseHandle( file );
    free( payload );
    if (r)
    {
        /* a binary cut short leaves the host waiting for the rest: the connection cannot go on */
        ERR( "cannot send the binary %s: %u\n", debugstr_w(binary), r );
        EnterCriticalSection( &host->cs );
        list_remove( &wait.entry );
        host->dead = TRUE;
        LeaveCriticalSection( &host->cs );
        CloseHandle( wait.event );
        return r;
    }
    WaitForSingleObject( wait.event, INFINITE );
    EnterCriticalSection( &host->cs );
    list_remove( &wait.entry );
    LeaveCriticalSection( &host->cs );
    CloseHandle( wait.event );
    *code = wait.code;
    return wait.status;
}

void msi_syshost_stop( MSIPACKAGE *package )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(package->syshost); i++)
    {
        struct msi_syshost *host = package->syshost[i];

        if (!host) continue;
        send_frame( host->pipe, &host->write_cs, WEX_MSISYS_END, 0, NULL, 0, NULL, 0 );
        if (WaitForSingleObject( host->reader, 60000 ))
        {
            WARN( "the SYSTEM host %04lx does not end\n", host->pid );
            CancelSynchronousIo( host->reader );
            WaitForSingleObject( host->reader, 5000 );
        }
        CloseHandle( host->reader );
        /* the relay's pumps wait on our own endpoint: end them, and wait until they have let go of the host */
        for (;;)
        {
            struct relay_channel *channel;
            BOOL empty;

            EnterCriticalSection( &host->cs );
            empty = list_empty( &host->channels );
            LIST_FOR_EACH_ENTRY( channel, &host->channels, struct relay_channel, entry )
            {
                channel->closing = TRUE;
                CancelSynchronousIo( channel->thread );
            }
            LeaveCriticalSection( &host->cs );
            if (empty) break;
            Sleep( 10 );
        }
        CloseHandle( host->pipe );
        DeleteCriticalSection( &host->write_cs ); DeleteCriticalSection( &host->run_cs );
        DeleteCriticalSection( &host->cs );
        free( host );
        package->syshost[i] = NULL;
    }
}

/******************************************************************************
 * the host: msiexec -WexSysHost <handle>, SYSTEM in session 0
 */

struct host_channel
{
    struct list entry;
    UINT32 id;
    HANDLE pipe;
};

static struct
{
    HANDLE pipe;                    /* the client's connection */
    CRITICAL_SECTION write_cs;
    CRITICAL_SECTION cs;            /* channels */
    struct list channels;
    UINT32 next_channel;
    WCHAR endpoint[48];             /* our ncalrpc endpoint, relayed to the client */
    WCHAR endpoint_pipe[80];
    WCHAR folder[MAX_PATH];         /* the binaries we write */
    HANDLE lock;                    /* host.lock in it, held open: a restarted service skips a folder in use */
    LONG files;
    SECURITY_ATTRIBUTES endpoint_sa;
} syshost;

static void random_hex( WCHAR *buffer, size_t size )
{
    GUID guid;

    CoCreateGuid( &guid );
    swprintf( buffer, size, L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", (unsigned int)guid.Data1, guid.Data2, guid.Data3,
              guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
              guid.Data4[6], guid.Data4[7] );
}

/* the folder for the binaries: SYSTEM and Administrators, as Windows' C:\Windows\Installer\MSI*.tmp, made by us
 * before the first binary. In the machine root the server maps both (SYSTEM too) to the system group's bits (B41,
 * sd_system_group_mode): mode 2770, session 0's account and the system group, nobody else; a user's limited processes
 * have no access, elevated ones and the sessions' system roles have the group, as for every machine-root file */
static BOOL make_folder(void)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    WCHAR windir[MAX_PATH], name[40], lock[MAX_PATH];
    BOOL ret = FALSE;
    int tries;

    if (!GetWindowsDirectoryW( windir, ARRAY_SIZE(windir) )) return FALSE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW( L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                                                                SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL ))
        return FALSE;
    swprintf( syshost.folder, ARRAY_SIZE(syshost.folder), L"%s\\Installer", windir );
    CreateDirectoryW( syshost.folder, NULL );
    for (tries = 0; tries < 8 && !ret; tries++)
    {
        random_hex( name, ARRAY_SIZE(name) );
        swprintf( syshost.folder, ARRAY_SIZE(syshost.folder), L"%s\\Installer\\WexSys%s", windir, name );
        ret = CreateDirectoryW( syshost.folder, &sa );  /* a fresh one, never one that is there already */
    }
    LocalFree( sa.lpSecurityDescriptor );
    if (!ret)
    {
        ERR( "cannot make a folder for the binaries: %lu\n", GetLastError() );
        return FALSE;
    }
    swprintf( lock, ARRAY_SIZE(lock), L"%s\\host.lock", syshost.folder );
    syshost.lock = CreateFileW( lock, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, NULL );
    if (syshost.lock == INVALID_HANDLE_VALUE) WARN( "no lock file in the folder: %lu\n", GetLastError() );
    return TRUE;
}

static void remove_folder(void)
{
    WIN32_FIND_DATAW data;
    WCHAR path[MAX_PATH];
    HANDLE find;

    if (!syshost.folder[0]) return;
    if (syshost.lock && syshost.lock != INVALID_HANDLE_VALUE) CloseHandle( syshost.lock );  /* deleted on close */
    syshost.lock = NULL;
    swprintf( path, ARRAY_SIZE(path), L"%s\\*", syshost.folder );
    if ((find = FindFirstFileW( path, &data )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
            swprintf( path, ARRAY_SIZE(path), L"%s\\%s", syshost.folder, data.cFileName );
            if (!DeleteFileW( path )) WARN( "cannot remove %s: %lu\n", debugstr_w(path), GetLastError() );
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (!RemoveDirectoryW( syshost.folder ))
        WARN( "cannot remove %s: %lu\n", debugstr_w(syshost.folder), GetLastError() );
}

/* a new file for a binary in our folder; its chunks are written as they arrive */
static HANDLE new_binary_file( const WCHAR *ext, WCHAR *path, size_t path_size )
{
    swprintf( path, path_size, L"%s\\%u%s", syshost.folder, (unsigned int)InterlockedIncrement( &syshost.files ), ext );
    return CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
}

static void host_send( UINT32 type, UINT32 channel, const void *data, UINT32 size )
{
    send_frame( syshost.pipe, &syshost.write_cs, type, channel, data, size, NULL, 0 );
}

static struct host_channel *host_find_channel( UINT32 id )
{
    struct host_channel *channel;

    LIST_FOR_EACH_ENTRY( channel, &syshost.channels, struct host_channel, entry )
        if (channel->id == id) return channel;
    return NULL;
}

/* one of the action's RPC connections to our endpoint: its requests go to the client */
static DWORD WINAPI host_pump( void *arg )
{
    struct host_channel *channel = arg;
    BYTE *pdu;
    DWORD size;

    while ((pdu = read_pdu( channel->pipe, &size )))
    {
        host_send( WEX_MSISYS_DATA, channel->id, pdu, size );
        free( pdu );
    }
    host_send( WEX_MSISYS_CLOSE, channel->id, NULL, 0 );
    EnterCriticalSection( &syshost.cs );
    list_remove( &channel->entry );
    LeaveCriticalSection( &syshost.cs );
    DisconnectNamedPipe( channel->pipe );
    CloseHandle( channel->pipe );
    free( channel );
    return 0;
}

static HANDLE new_endpoint_instance( BOOL first )
{
    return CreateNamedPipeW( syshost.endpoint_pipe,
                             PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                             PDU_MAX_FRAG, PDU_MAX_FRAG, 0, &syshost.endpoint_sa );
}

static DWORD WINAPI host_listener( void *arg )
{
    HANDLE pipe = arg;

    for (;;)
    {
        struct host_channel *channel;
        HANDLE thread;

        if (!ConnectNamedPipe( pipe, NULL ) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle( pipe );
        }
        else if ((channel = calloc( 1, sizeof(*channel) )))
        {
            channel->pipe = pipe;
            EnterCriticalSection( &syshost.cs );
            channel->id = ++syshost.next_channel;
            list_add_tail( &syshost.channels, &channel->entry );
            LeaveCriticalSection( &syshost.cs );
            host_send( WEX_MSISYS_OPEN, channel->id, NULL, 0 );
            if ((thread = CreateThread( NULL, 0, host_pump, channel, 0, NULL ))) CloseHandle( thread );
            else
            {
                EnterCriticalSection( &syshost.cs );
                list_remove( &channel->entry );
                LeaveCriticalSection( &syshost.cs );
                host_send( WEX_MSISYS_CLOSE, channel->id, NULL, 0 );
                CloseHandle( pipe );
                free( channel );
            }
        }
        else CloseHandle( pipe );

        if ((pipe = new_endpoint_instance( FALSE )) == INVALID_HANDLE_VALUE)
        {
            ERR( "cannot make another instance of %s: %lu\n", debugstr_w(syshost.endpoint_pipe), GetLastError() );
            return 1;
        }
    }
}

struct host_run
{
    struct wex_msisys_run run;
    BYTE *entry;
    WCHAR *action;
    WCHAR path[MAX_PATH];   /* the binary, as we write it */
    HANDLE file;            /* while its chunks arrive */
    UINT32 received;
    UINT status;            /* nonzero: it cannot run (its chunks are still taken, then the client hears why) */
};

static struct host_run *pending;  /* the RUN whose chunks are arriving (the main loop's only) */

/* the audit (the orchestrator's condition in place of a consent record): an event in the System log from session 0
 * for every action run as SYSTEM, with what the service told us about the caller and the install */
static void host_audit( const WCHAR *action, UINT status, UINT code )
{
    struct wex_evtlog_header hdr = { WEX_EVTLOG_MAGIC, WEX_EVTLOG_VERSION, WEX_EVTLOG_OPEN }, reply;
    struct wex_evtlog_open open = { WEX_EVTLOG_OPEN_SOURCE, sizeof(WEX_MSISYS_EVENT_SOURCE) };
    struct wex_evtlog_report report = { status || code ? EVENTLOG_WARNING_TYPE : EVENTLOG_INFORMATION_TYPE, 0,
                                        WEX_MSISYS_EVENT_ACTION };
    static const WCHAR *names[] = { WEX_MSISYS_ENV_PACKAGE, WEX_MSISYS_ENV_PRODUCT, WEX_MSISYS_ENV_USER,
                                    WEX_MSISYS_ENV_SESSION };
    WCHAR values[4][MAX_PATH], status_str[16], code_str[16];
    const WCHAR *strings[7];
    DWORD mode = PIPE_READMODE_MESSAGE, done, size, pos, len;
    PSID sid = NULL;
    BYTE *msg;
    HANDLE pipe;
    int i;

    for (i = 0; i < 4; i++)
        if (!GetEnvironmentVariableW( names[i], values[i], MAX_PATH )) values[i][0] = 0;
    swprintf( status_str, ARRAY_SIZE(status_str), L"%u", status );
    swprintf( code_str, ARRAY_SIZE(code_str), L"%u", code );
    strings[0] = action;
    strings[1] = status_str;
    strings[2] = code_str;
    for (i = 0; i < 4; i++) strings[3 + i] = values[i];
    TRACE( "%s: status %u, code %u, user %s session %s\n", debugstr_w(action), status, code, debugstr_w(values[2]),
           debugstr_w(values[3]) );

    pipe = CreateFileW( WEX_EVTLOG_PIPE, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL );
    if (pipe == INVALID_HANDLE_VALUE)
    {
        ERR( "no event log for the audit of %s: %lu\n", debugstr_w(action), GetLastError() );
        return;
    }
    SetNamedPipeHandleState( pipe, &mode, NULL, NULL );

    size = sizeof(hdr) + sizeof(open) + sizeof(WEX_MSISYS_EVENT_SOURCE);
    if (!(msg = malloc( size ))) goto done;
    hdr.size = size;
    memcpy( msg, &hdr, sizeof(hdr) );
    memcpy( msg + sizeof(hdr), &open, sizeof(open) );
    memcpy( msg + sizeof(hdr) + sizeof(open), WEX_MSISYS_EVENT_SOURCE, sizeof(WEX_MSISYS_EVENT_SOURCE) );
    if (!WriteFile( pipe, msg, size, &done, NULL ) || !ReadFile( pipe, &reply, sizeof(reply), &done, NULL ) ||
            done < sizeof(reply) || reply.status)
    {
        ERR( "the event log refused the audit source\n" );
        free( msg );
        goto done;
    }
    free( msg );

    if (values[2][0]) ConvertStringSidToSidW( values[2], &sid );
    report.num_strings = ARRAY_SIZE(strings);
    for (i = 0; i < ARRAY_SIZE(strings); i++) report.strings_size += (lstrlenW( strings[i] ) + 1) * sizeof(WCHAR);
    report.sid_size = sid ? GetLengthSid( sid ) : 0;
    size = sizeof(hdr) + sizeof(report) + report.sid_size + report.strings_size;
    if (!(msg = calloc( 1, size ))) goto done;
    hdr.op = WEX_EVTLOG_REPORT;
    hdr.size = size;
    hdr.handle = reply.handle;
    memcpy( msg, &hdr, sizeof(hdr) );
    memcpy( msg + sizeof(hdr), &report, sizeof(report) );
    pos = sizeof(hdr) + sizeof(report);
    if (sid) memcpy( msg + pos, sid, report.sid_size );
    pos += report.sid_size;
    for (i = 0; i < ARRAY_SIZE(strings); i++)
    {
        len = (lstrlenW( strings[i] ) + 1) * sizeof(WCHAR);
        memcpy( msg + pos, strings[i], len );
        pos += len;
    }
    if (!WriteFile( pipe, msg, size, &done, NULL ) || !ReadFile( pipe, &reply, sizeof(reply), &done, NULL ) ||
            done < sizeof(reply) || reply.status)
        ERR( "the audit of %s was not written\n", debugstr_w(action) );
    free( msg );

done:
    if (sid) LocalFree( sid );
    CloseHandle( pipe );
}

static DWORD WINAPI host_run_thread( void *arg )
{
    struct host_run *job = arg;
    struct wex_msisys_done done = { job->run.guid };
    PROCESS_INFORMATION pi;
    STARTUPINFOW si = { sizeof(si) };
    const WCHAR *path = job->path;
    WCHAR *cmd = NULL;
    const WCHAR *args;
    DWORD code;

    if (job->run.kind == WEX_MSISYS_KIND_DLL)
    {
        CoInitializeEx( NULL, COINIT_MULTITHREADED );
        done.code = custom_call_dll_function( syshost.endpoint, &job->run.guid, path );
        CoUninitialize();
        if (!DeleteFileW( path )) WARN( "cannot remove %s: %lu\n", debugstr_w(path), GetLastError() );
        goto end;
    }

    /* an EXE: "path" arguments, run from C:\ as msi does */
    args = (const WCHAR *)job->entry;
    if (!(cmd = malloc( (lstrlenW( path ) + lstrlenW( args ) + 4) * sizeof(WCHAR) )))
    {
        done.status = ERROR_OUTOFMEMORY;
        goto end;
    }
    swprintf( cmd, lstrlenW( path ) + lstrlenW( args ) + 4, L"\"%s\"%s%s", path, args[0] ? L" " : L"", args );
    if (!CreateProcessW( path, cmd, NULL, NULL, FALSE, 0, NULL, L"C:\\", &si, &pi ))
    {
        ERR( "cannot start %s: %lu\n", debugstr_w(path), GetLastError() );
        done.status = GetLastError();
        DeleteFileW( path );
        goto end;
    }
    CloseHandle( pi.hThread );
    if (!(job->run.flags & WEX_MSISYS_RUN_ASYNC))
    {
        WaitForSingleObject( pi.hProcess, INFINITE );
        if (GetExitCodeProcess( pi.hProcess, &code )) done.code = code;
        if (!DeleteFileW( path )) WARN( "cannot remove %s: %lu\n", debugstr_w(path), GetLastError() );
    }
    CloseHandle( pi.hProcess );

end:
    host_audit( job->action, done.status, done.code );
    host_send( WEX_MSISYS_DONE, 0, &done, sizeof(done) );
    free( cmd );
    free( job->entry );
    free( job->action );
    free( job );
    return 0;
}

static void free_job( struct host_run *job )
{
    if (job->file && job->file != INVALID_HANDLE_VALUE)
    {
        CloseHandle( job->file );
        DeleteFileW( job->path );
    }
    free( job->entry );
    free( job->action );
    free( job );
}

/* a job that cannot run, once its chunks are taken: the client hears why, and the audit has it */
static void refuse_job( struct host_run *job )
{
    struct wex_msisys_done done = { job->run.guid, job->status };

    host_audit( job->action, done.status, 0 );
    host_send( WEX_MSISYS_DONE, 0, &done, sizeof(done) );
    free_job( job );
}

static BOOL wide_terminated( const BYTE *data, UINT32 size )
{
    WCHAR last;

    if (size < sizeof(WCHAR) || size % sizeof(WCHAR)) return FALSE;
    memcpy( &last, data + size - sizeof(WCHAR), sizeof(last) );
    return !last;
}

/* the client's request, read as untrusted: sizes that add up, terminated strings, one RUN at a time; FALSE ends the
 * connection. The binary's file is made now, and its chunks follow (host_chunk) */
static BOOL host_start_run( const BYTE *data, UINT32 size )
{
    struct host_run *job;
    const BYTE *p;

    if (pending || size < sizeof(job->run) || !(job = calloc( 1, sizeof(*job) ))) return FALSE;
    memcpy( &job->run, data, sizeof(job->run) );
    p = data + sizeof(job->run);
    if ((UINT64)sizeof(job->run) + job->run.entry_size + job->run.action_size != size ||
            !job->run.binary_size || job->run.binary_size > WEX_MSISYS_MAX_BINARY ||
            (job->run.kind != WEX_MSISYS_KIND_DLL && job->run.kind != WEX_MSISYS_KIND_EXE) ||
            (job->run.flags & ~WEX_MSISYS_RUN_ASYNC) ||
            /* the entry: a function name, or arguments */
            (job->run.kind == WEX_MSISYS_KIND_DLL ? (!job->run.entry_size || p[job->run.entry_size - 1])
                                                  : !wide_terminated( p, job->run.entry_size )) ||
            !wide_terminated( p + job->run.entry_size, job->run.action_size ))
        goto fail;
    if (!(job->entry = malloc( job->run.entry_size )) || !(job->action = malloc( job->run.action_size )))
        goto fail;
    memcpy( job->entry, p, job->run.entry_size );
    memcpy( job->action, p + job->run.entry_size, job->run.action_size );
    job->file = new_binary_file( job->run.kind == WEX_MSISYS_KIND_DLL ? L".dll" : L".exe", job->path,
                                 ARRAY_SIZE(job->path) );
    if (job->file == INVALID_HANDLE_VALUE)
    {
        job->status = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        ERR( "cannot make a file for the binary: %u\n", job->status );
    }
    pending = job;
    return TRUE;

fail:
    ERR( "a malformed request from the client: ending\n" );
    free( job->entry );
    free( job->action );
    free( job );
    return FALSE;
}

/* the pending RUN's next bytes; when they are all there the job starts. FALSE (no RUN, too many bytes) ends the
 * connection */
static BOOL host_chunk( const BYTE *data, UINT32 size )
{
    struct host_run *job = pending;
    HANDLE thread;

    if (!job || !size || size > job->run.binary_size - job->received) return FALSE;
    if (!job->status && !write_full( job->file, data, size ))
    {
        job->status = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        ERR( "cannot write the binary: %u\n", job->status );
    }
    job->received += size;
    if (job->received < job->run.binary_size) return TRUE;

    pending = NULL;
    if (job->status)
    {
        refuse_job( job );
        return TRUE;
    }
    CloseHandle( job->file );
    job->file = NULL;
    if ((thread = CreateThread( NULL, 0, host_run_thread, job, 0, NULL ))) CloseHandle( thread );
    else
    {
        job->status = ERROR_OUTOFMEMORY;
        DeleteFileW( job->path );
        refuse_job( job );
    }
    return TRUE;
}

/* a relayed answer of the client's msi endpoint, read as untrusted before the action's rpcrt4 parses it */
static BOOL valid_pdu( const BYTE *pdu, UINT32 size )
{
    USHORT frag;

    if (size < PDU_HDR_SIZE || size > PDU_MAX_FRAG || pdu[0] != 5 || !(pdu[4] & 0x10)) return FALSE;
    memcpy( &frag, pdu + 8, sizeof(frag) );
    return frag == size;
}

UINT CDECL __wine_msi_syshost( HANDLE pipe )
{
    struct wex_msisys_ready ready = { GetCurrentProcessId() };
    struct wex_msisys_frame frame;
    struct host_channel *channel;
    HANDLE instance, thread;
    WCHAR hex[40];
    BYTE *data;

    syshost.pipe = pipe;
    InitializeCriticalSection( &syshost.write_cs );
    InitializeCriticalSection( &syshost.cs );
    list_init( &syshost.channels );

    /* our endpoint: a random name; only SYSTEM opens it (the bridge checks a session's opener against this DACL),
     * so only the action's own connections come in */
    random_hex( hex, ARRAY_SIZE(hex) );
    swprintf( syshost.endpoint, ARRAY_SIZE(syshost.endpoint), L"wexsys%s", hex );
    swprintf( syshost.endpoint_pipe, ARRAY_SIZE(syshost.endpoint_pipe), L"\\\\.\\pipe\\lrpc\\%s", syshost.endpoint );
    syshost.endpoint_sa.nLength = sizeof(syshost.endpoint_sa);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW( L"D:P(A;;GA;;;SY)", SDDL_REVISION_1,
                                                                &syshost.endpoint_sa.lpSecurityDescriptor, NULL ) ||
            !make_folder() ||
            (instance = new_endpoint_instance( TRUE )) == INVALID_HANDLE_VALUE)
    {
        ERR( "cannot set up the host: %lu\n", GetLastError() );
        remove_folder();
        return 1;
    }
    if (!(thread = CreateThread( NULL, 0, host_listener, instance, 0, NULL )))
    {
        remove_folder();
        return 1;
    }
    CloseHandle( thread );
    host_send( WEX_MSISYS_READY, 0, &ready, sizeof(ready) );

    while ((data = read_frame( pipe, &frame )))
    {
        BOOL end = FALSE;

        switch (frame.type)
        {
        case WEX_MSISYS_RUN:
            if (!host_start_run( data, frame.size )) end = TRUE;
            break;
        case WEX_MSISYS_CHUNK:
            if (!host_chunk( data, frame.size ))
            {
                ERR( "a chunk without its RUN, or too many bytes: ending\n" );
                end = TRUE;
            }
            break;
        case WEX_MSISYS_DATA:
            if (!valid_pdu( data, frame.size ))
            {
                ERR( "a malformed RPC answer from the client: ending\n" );
                end = TRUE;
                break;
            }
            EnterCriticalSection( &syshost.cs );
            if ((channel = host_find_channel( frame.channel )))
            {
                DWORD written;

                if (!WriteFile( channel->pipe, data, frame.size, &written, NULL ))
                    DisconnectNamedPipe( channel->pipe );
            }
            LeaveCriticalSection( &syshost.cs );
            break;
        case WEX_MSISYS_CLOSE:
            EnterCriticalSection( &syshost.cs );
            if ((channel = host_find_channel( frame.channel ))) DisconnectNamedPipe( channel->pipe );
            LeaveCriticalSection( &syshost.cs );
            break;
        case WEX_MSISYS_END:
            end = TRUE;
            break;
        default:
            ERR( "unexpected frame %u from the client: ending\n", frame.type );
            end = TRUE;
            break;
        }
        free( data );
        if (end) break;
    }

    TRACE( "the install is over, ending\n" );
    if (pending) free_job( pending );  /* a binary cut short */
    pending = NULL;
    remove_folder();
    return 0;
}
