/*
 * Registry key handles that this process's own server does not hold (WineExplorer system tier)
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

/*
 * A registry key handle of a process has one of three back ends (docs/SYSTEM_TIER_DESIGN.md,
 * docs/SESSION0_SPLIT_REVIEW.md R3):
 * - local: a handle of the process's own server, as in stock Wine. It is not in the table below.
 * - session 0 (T1): a key of the machine trees (\Registry\Machine and \Registry\User\.Default, except the per-session
 *   keys of wine/wex_session0.h) and, with T4a, of the process user's own hive, when the process's server uses
 *   session 0 (wineserver --session0). Session 0 is reached over a channel of the process's own: a socket that the
 *   server got from session 0 with a description of the process's token. All threads share it under a mutex. The
 *   entry keeps the key's absolute path, so that it survives a new channel: when the channel is lost, the next call
 *   gets a new one and opens each key again by its path before using it.
 * - virtual store (T2b): UAC registry virtualization. For a 32-bit program with a limited token (not elevated, not
 *   SYSTEM or a service account), keys under HKLM\Software other than Classes, Microsoft\Windows and
 *   Microsoft\Windows NT are virtualized, as on Windows since Vista: a write that session 0 refuses goes to the
 *   user's own HKCU\Software\Classes\VirtualStore\Machine\Software\Wow6432Node\<rest>, and reads see the store
 *   first, then the real key. The entry holds a handle of the real key (a session 0 key, or none) and of the store
 *   key (or none until the first write). Not done: a manifest's requestedExecutionLevel (not known to ntdll, so
 *   treated as not stated).
 * The session 0 and virtual store keys are entries of one table, handed out as WEXKEY_TAG + index * 4, above every
 * handle a server hands out (at most 0x00ffffff entries, so below 0x04000000). Close, duplication and the calls that
 * only follow the handle are handled here once; registry.c, server.c, file.c and security.c keep the hooks.
 *
 * Duplication into another process (DuplicateHandle with another target): the target gets a copy of the entry, sent
 * in a system APC (APC_WEX_REGKEY) with a section that holds a record of it (kind, access, absolute path). The
 * target opens a session 0 key again with its own token on its own channel when it first uses it, as after a new
 * channel, and a virtual key's store key by its own user's store path. So the target's token decides, as an access
 * check of the new access would, and not the grant of the source. Duplication from another process goes to that
 * process (APC_DUP_HANDLE), which then sends the entry.
 *
 * Not done:
 * - Inheritance into a child process. The child's handle must have the parent's value, and must exist before the
 *   child runs any code; but the child's loader opens keys of its own (and so takes entries) before an APC from the
 *   parent could run in it, and the startup information that the server hands the child (create_process,
 *   get_startup_info) has no room for the table. It would take a new part of that startup information, read by the
 *   child's ntdll before the loader starts, with the entries placed at their parent's values. An inheritable key of
 *   session 0 is not inherited; the child gets no handle at that value.
 * - NtSaveKey, NtLoadKey and NtUnloadKey on a session 0 key: they go to the process's own server, which does not
 *   know the handle.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/server.h"
#include "wine/wex_session0.h"
#include "wine/debug.h"
#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(reg);

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#define WEXKEY_TAG  0x40000000
#define WEXKEY_MAX  0x00400000  /* table entries */

enum wexkey_kind
{
    WEXKEY_FREE,
    WEXKEY_SESSION0,
    WEXKEY_VIRTUAL
};

struct wexkey
{
    enum wexkey_kind kind;
    ACCESS_MASK      access;      /* session 0: as sent, with the view; virtual: as asked */
    ULONG            attributes;  /* session 0: OBJ_OPENLINK and OBJ_CASE_INSENSITIVE, for a reopen */
    WCHAR           *path;        /* absolute path as opened (a virtual key's: its real key's) */
    USHORT           len;         /* in bytes */
    obj_handle_t     remote;      /* session 0: the handle there, valid for generation */
    unsigned int     generation;  /* session 0: the channel's; S0_NOT_OPEN until it is opened there */
    HANDLE           real;        /* virtual: the real key (a session 0 key), or 0 */
    HANDLE           store;       /* virtual: the store key, or 0 until it exists */
    UNICODE_STRING   store_path;  /* virtual: absolute */
};

#define S0_NOT_OPEN 0  /* never a channel's generation */

/* one mutex for the table and the channel to session 0: a call on the channel uses its entry */
static struct wexkey *keys;
static unsigned int keys_count, keys_size;
static pthread_mutex_t wexkey_mutex = PTHREAD_MUTEX_INITIALIZER;

/* the channel to session 0 */
static unsigned int s0_generation = 1;
static int s0_state;  /* 0 not asked yet, 1 session 0 in use, -1 not used */
static int s0_sock = -1, s0_request_fd = -1, s0_reply_fd = -1, s0_wait_fd = -1;

static const char * const s0_session_machine_keys[] = { WEX_SESSION_MACHINE_KEYS };
static const char * const s0_session_default_keys[] = { WEX_SESSION_DEFAULT_USER_KEYS };

static void wexkey_lock( sigset_t *old )
{
    pthread_sigmask( SIG_BLOCK, &server_block_set, old );
    pthread_mutex_lock( &wexkey_mutex );
}

static void wexkey_unlock( sigset_t *old )
{
    pthread_mutex_unlock( &wexkey_mutex );
    pthread_sigmask( SIG_SETMASK, old, NULL );
}

/* the handle is in the table's range (in this process or in another) */
BOOL wexkey_is_handle( HANDLE handle )
{
    ULONG_PTR value = (ULONG_PTR)handle;

    return value >= WEXKEY_TAG && value < WEXKEY_TAG + WEXKEY_MAX * 4 && !(value & 3);
}

/* the entry of a handle, of the given kind (WEXKEY_FREE: any); with the mutex held */
static struct wexkey *get_key( HANDLE handle, enum wexkey_kind kind )
{
    unsigned int index;

    if (!wexkey_is_handle( handle )) return NULL;
    index = ((ULONG_PTR)handle - WEXKEY_TAG) / 4;
    if (index >= keys_count || keys[index].kind == WEXKEY_FREE) return NULL;
    if (kind != WEXKEY_FREE && keys[index].kind != kind) return NULL;
    return &keys[index];
}

/* a new, cleared entry; with the mutex held. Entry pointers are not valid after the mutex is released. */
static struct wexkey *alloc_key( HANDLE *handle )
{
    unsigned int i;

    for (i = 0; i < keys_count; i++) if (keys[i].kind == WEXKEY_FREE) break;
    if (i == keys_count)
    {
        if (i == WEXKEY_MAX) return NULL;
        if (i == keys_size)
        {
            unsigned int size = keys_size ? keys_size * 2 : 64;
            struct wexkey *new_keys = realloc( keys, size * sizeof(*new_keys) );

            if (!new_keys) return NULL;
            keys = new_keys;
            keys_size = size;
        }
        keys_count++;
    }
    memset( &keys[i], 0, sizeof(keys[i]) );
    *handle = (HANDLE)(ULONG_PTR)(WEXKEY_TAG + i * 4);
    return &keys[i];
}

/* a copy of a counted string (at least one character's room, so that an empty path still has a buffer) */
static WCHAR *copy_path( const WCHAR *path, USHORT len )
{
    WCHAR *copy = malloc( max( len, sizeof(WCHAR) ));

    if (copy) memcpy( copy, path, len );
    return copy;
}


/***********************************************************************
 * The user's key path
 */

/* "\Registry\User\S-..." for the user of a token, in ASCII; *len is its length */
NTSTATUS wexkey_user_key_path( HANDLE token, char *buffer, size_t size, unsigned int *len )
{
    DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
    DWORD i, ret_len = sizeof(sid_data);
    NTSTATUS status;
    SID *sid;
    int n;

    if ((status = NtQueryInformationToken( token, TokenUser, sid_data, sizeof(sid_data), &ret_len ))) return status;
    sid = ((TOKEN_USER *)sid_data)->User.Sid;
    n = snprintf( buffer, size, "\\Registry\\User\\S-%u-%u", sid->Revision,
                  MAKELONG( MAKEWORD( sid->IdentifierAuthority.Value[5], sid->IdentifierAuthority.Value[4] ),
                            MAKEWORD( sid->IdentifierAuthority.Value[3], sid->IdentifierAuthority.Value[2] )));
    for (i = 0; i < sid->SubAuthorityCount && n >= 0 && (size_t)n < size; i++)
        n += snprintf( buffer + n, size - n, "-%u", sid->SubAuthority[i] );
    if (n < 0 || (size_t)n >= size) return STATUS_BUFFER_TOO_SMALL;
    *len = n;
    return STATUS_SUCCESS;
}


/***********************************************************************
 * Session 0: which paths, and the channel
 */

static BOOL s0_in_list( const WCHAR *rest, unsigned int count, const char * const *list, unsigned int n )
{
    unsigned int i;

    for (i = 0; i < n; i++) if (wex_session_key_match( rest, count, list[i] )) return TRUE;
    return FALSE;
}

/* a path of the machine trees that session 0 holds */
static BOOL s0_machine_path( const WCHAR *path, USHORT len )
{
    unsigned int count = len / sizeof(WCHAR), n;

    if (!path) return FALSE;
    if ((n = wex_session_key_match( path, count, "\\Registry\\Machine" )))
        return n == count || !s0_in_list( path + n + 1, count - n - 1, s0_session_machine_keys,
                                          ARRAY_SIZE(s0_session_machine_keys) );
    if ((n = wex_session_key_match( path, count, "\\Registry\\User\\.Default" )))
        return n == count || !s0_in_list( path + n + 1, count - n - 1, s0_session_default_keys,
                                          ARRAY_SIZE(s0_session_default_keys) );
    return FALSE;
}

/* T4a: when session 0 also holds this process's user hive, the absolute paths of \Registry\User\<SID> and
 * <SID>_Classes for the process token's user go there too (ASCII, set at each new channel) */
static int s0_user_hive;
static char s0_user_prefix[160];

static void s0_set_user_prefix(void)
{
    unsigned int len;

    /* room for "_Classes" */
    if (wexkey_user_key_path( (HANDLE)~(ULONG_PTR)3 /* the process token */, s0_user_prefix,
                              sizeof(s0_user_prefix) - 9, &len ))
        s0_user_prefix[0] = 0;
}

/* a path below \Registry\User\S-: perhaps this process's user hive */
static BOOL s0_user_candidate( const WCHAR *path, USHORT len )
{
    static const char prefix[] = "\\Registry\\User\\S-";
    unsigned int i, count = len / sizeof(WCHAR);

    if (!path || count < sizeof(prefix) - 1) return FALSE;
    for (i = 0; i < sizeof(prefix) - 1; i++)
    {
        WCHAR ch = path[i];
        unsigned char p = prefix[i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (p >= 'A' && p <= 'Z') p += 'a' - 'A';
        if (ch != p) return FALSE;
    }
    return TRUE;
}

/* this process's user hive (or its _Classes), held by session 0 */
static BOOL s0_user_path( const WCHAR *path, USHORT len )
{
    char classes[176];
    unsigned int count = len / sizeof(WCHAR);

    if (!s0_user_hive || !s0_user_prefix[0] || !path) return FALSE;
    if (wex_session_key_match( path, count, s0_user_prefix )) return TRUE;
    snprintf( classes, sizeof(classes), "%s_Classes", s0_user_prefix );
    return wex_session_key_match( path, count, classes ) != 0;
}

static void s0_disconnect(void)
{
    if (s0_request_fd != -1) close( s0_request_fd );
    if (s0_reply_fd != -1) close( s0_reply_fd );
    if (s0_wait_fd != -1) close( s0_wait_fd );
    if (s0_sock != -1) close( s0_sock );
    s0_request_fd = s0_reply_fd = s0_wait_fd = s0_sock = -1;
    if (++s0_generation == S0_NOT_OPEN) s0_generation++;
}

/* send a request and read its reply on the channel; FALSE when the channel is lost */
static BOOL s0_transfer( struct __server_request_info *req, unsigned int *status )
{
    struct iovec vec[__SERVER_MAX_DATA + 1];
    unsigned int i, count = 1;
    size_t left = sizeof(req->u.req);
    char *ptr;
    ssize_t ret;

    vec[0].iov_base = &req->u.req;
    vec[0].iov_len = sizeof(req->u.req);
    if (req->u.req.request_header.request_size)
    {
        for (i = 0; i < req->data_count; i++)
        {
            vec[count].iov_base = (void *)req->data[i].ptr;
            vec[count].iov_len = req->data[i].size;
            left += req->data[i].size;
            count++;
        }
    }
    while (left)
    {
        if ((ret = writev( s0_request_fd, vec, count )) < 0)
        {
            if (errno == EINTR) continue;
            return FALSE;
        }
        left -= ret;
        for (i = 0; i < count && ret; i++)
        {
            size_t part = min( (size_t)ret, vec[i].iov_len );
            vec[i].iov_base = (char *)vec[i].iov_base + part;
            vec[i].iov_len -= part;
            ret -= part;
        }
    }

    for (ptr = (char *)&req->u.reply, left = sizeof(req->u.reply); left; )
    {
        if ((ret = read( s0_reply_fd, ptr, left )) > 0) { ptr += ret; left -= ret; continue; }
        if (ret < 0 && errno == EINTR) continue;
        return FALSE;
    }
    for (ptr = req->reply_data, left = req->u.reply.reply_header.reply_size; left; )
    {
        if ((ret = read( s0_reply_fd, ptr, left )) > 0) { ptr += ret; left -= ret; continue; }
        if (ret < 0 && errno == EINTR) continue;
        return FALSE;
    }
    *status = req->u.reply.reply_header.error;
    return TRUE;
}

static int s0_receive_fd( int sock, obj_handle_t *handle )
{
    char cmsg_buffer[256];
    struct msghdr msghdr;
    struct cmsghdr *cmsg;
    struct iovec vec;
    int ret, fd = -1;

    memset( &msghdr, 0, sizeof(msghdr) );
    vec.iov_base = handle;
    vec.iov_len = sizeof(*handle);
    msghdr.msg_iov = &vec;
    msghdr.msg_iovlen = 1;
    msghdr.msg_control = cmsg_buffer;
    msghdr.msg_controllen = sizeof(cmsg_buffer);
    do ret = recvmsg( sock, &msghdr, MSG_CMSG_CLOEXEC ); while (ret == -1 && errno == EINTR);
    if (ret <= 0) return -1;
    for (cmsg = CMSG_FIRSTHDR( &msghdr ); cmsg; cmsg = CMSG_NXTHDR( &msghdr, cmsg ))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) memcpy( &fd, CMSG_DATA( cmsg ), sizeof(fd) );
    return fd;
}

static BOOL s0_send_fd( int sock, int fd )
{
    struct send_fd data = { 0, fd };  /* thread id 0: the channel's only thread */
    char cmsg_buffer[CMSG_SPACE( sizeof(int) )];
    struct msghdr msghdr;
    struct cmsghdr *cmsg;
    struct iovec vec;
    int ret;

    memset( &msghdr, 0, sizeof(msghdr) );
    vec.iov_base = &data;
    vec.iov_len = sizeof(data);
    msghdr.msg_iov = &vec;
    msghdr.msg_iovlen = 1;
    msghdr.msg_control = cmsg_buffer;
    msghdr.msg_controllen = sizeof(cmsg_buffer);
    cmsg = CMSG_FIRSTHDR( &msghdr );
    cmsg->cmsg_len = CMSG_LEN( sizeof(fd) );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy( CMSG_DATA( cmsg ), &fd, sizeof(fd) );
    do ret = sendmsg( sock, &msghdr, MSG_NOSIGNAL ); while (ret == -1 && errno == EINTR);
    return ret == sizeof(data);
}

/* ask this process's server for a channel and make the client handshake on it; with the mutex held */
static BOOL s0_connect(void)
{
    obj_handle_t cookie = 0, handle = 0, version = 0;
    int user_hive = 0;
    int sock = -1, request = -1, reply_pipe[2] = { -1, -1 }, wait_pipe[2] = { -1, -1 };
    unsigned int status;
    WORD machines[8];
    sigset_t sigset;

    server_enter_uninterrupted_section( &fd_cache_mutex, &sigset );
    SERVER_START_REQ( get_registry_channel )
    {
        status = wine_server_call( req );
        cookie = reply->cookie;
        user_hive = reply->user_hive;
    }
    SERVER_END_REQ;
    if (!status) sock = wine_server_receive_fd( &handle );
    server_leave_uninterrupted_section( &fd_cache_mutex, &sigset );

    if (status == STATUS_NOT_SUPPORTED || status == STATUS_NOT_IMPLEMENTED)
    {
        s0_state = -1;  /* this server keeps the machine registry itself */
        return FALSE;
    }
    s0_state = 1;  /* from now on the machine trees are session 0's, reachable or not */
    if ((s0_user_hive = !status && user_hive)) s0_set_user_prefix();
    if (status || sock == -1 || handle != cookie) goto error;

    if ((request = s0_receive_fd( sock, &version )) == -1 || version != SERVER_PROTOCOL_VERSION) goto error;
    if (server_pipe( reply_pipe ) == -1 || server_pipe( wait_pipe ) == -1) goto error;
    if (!s0_send_fd( sock, reply_pipe[1] ) || !s0_send_fd( sock, wait_pipe[1] )) goto error;
    s0_sock = sock;
    s0_request_fd = request;
    s0_reply_fd = reply_pipe[0];
    s0_wait_fd = wait_pipe[0];
    sock = request = reply_pipe[0] = wait_pipe[0] = -1;

    SERVER_START_REQ( init_first_thread )
    {
        req->unix_pid    = getpid();
        req->unix_tid    = get_unix_tid();
        req->reply_fd    = reply_pipe[1];
        req->wait_fd     = wait_pipe[1];
        req->debug_level = 0;
        req->page_size   = get_host_page_size();
        wine_server_set_reply( req, machines, sizeof(machines) );
        if (!s0_transfer( &__req, &status )) status = STATUS_PIPE_DISCONNECTED;
    }
    SERVER_END_REQ;
    close( reply_pipe[1] );
    close( wait_pipe[1] );
    if (status)
    {
        s0_disconnect();
        return FALSE;
    }

    /* session 0 reports key changes on the wait pipe: its read end goes to this process's server, which
     * sets the events of the notifications (relay_registry_notification) */
    wine_server_send_fd( s0_wait_fd );
    SERVER_START_REQ( set_registry_relay )
    {
        req->fd = s0_wait_fd;
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    if (status) WARN( "no relay for change notifications: %08x\n", status );
    close( s0_wait_fd );
    s0_wait_fd = -1;
    return TRUE;

error:
    if (sock != -1) close( sock );
    if (request != -1) close( request );
    if (reply_pipe[0] != -1) { close( reply_pipe[0] ); close( reply_pipe[1] ); }
    if (wait_pipe[0] != -1) { close( wait_pipe[0] ); close( wait_pipe[1] ); }
    return FALSE;
}

/* the absolute path is one that session 0 holds, and this process's server uses session 0 */
static BOOL s0_route_path( const WCHAR *path, USHORT len )
{
    BOOL machine = s0_machine_path( path, len );
    sigset_t sigset;

    if ((!machine && !s0_user_candidate( path, len )) || s0_state < 0) return FALSE;
    if (!s0_state)
    {
        wexkey_lock( &sigset );
        if (!s0_state) s0_connect();
        wexkey_unlock( &sigset );
    }
    return s0_state > 0 && (machine || s0_user_path( path, len ));
}

/* open a session 0 key again by its path on this channel; with the mutex held and a channel */
static BOOL s0_reopen( struct wexkey *key, unsigned int *status )
{
    BOOL ok;

    SERVER_START_REQ( open_key )
    {
        req->parent     = 0;
        req->access     = key->access;
        req->attributes = key->attributes;
        wine_server_add_data( req, key->path, key->len );
        if ((ok = s0_transfer( &__req, status )) && !*status)
        {
            key->remote = reply->hkey;
            key->generation = s0_generation;
        }
    }
    SERVER_END_REQ;
    return ok;
}

/* make a request on the channel. A session 0 key's current handle there is written to *field first (the key is
 * opened again if it belongs to an earlier channel or was never opened in this process); a lost channel is
 * replaced once. */
unsigned int wexkey_session0_call( void *req_ptr, HANDLE handle, obj_handle_t *field, unsigned int *generation )
{
    struct __server_request_info * const req = req_ptr;
    struct wexkey *key = NULL;
    unsigned int status = STATUS_PIPE_DISCONNECTED, attempt;
    sigset_t sigset;

    wexkey_lock( &sigset );
    for (attempt = 0; attempt < 2; attempt++)
    {
        if (handle && !(key = get_key( handle, WEXKEY_SESSION0 )))
        {
            status = STATUS_INVALID_HANDLE;
            break;
        }
        if (s0_request_fd == -1 && !s0_connect())
        {
            status = STATUS_PIPE_DISCONNECTED;
            break;
        }
        if (key)
        {
            if (key->generation != s0_generation)
            {
                if (!s0_reopen( key, &status ))
                {
                    s0_disconnect();
                    continue;
                }
                if (status) break;
            }
            *field = key->remote;
        }
        if (generation) *generation = s0_generation;
        if (s0_transfer( req, &status )) break;
        s0_disconnect();
        status = STATUS_PIPE_DISCONNECTED;
    }
    wexkey_unlock( &sigset );
    return status;
}

/* a new key handle for a handle of session 0 opened on the given channel generation (S0_NOT_OPEN: to be opened by
 * its path at its first use) */
HANDLE wexkey_add_session0( obj_handle_t remote, unsigned int generation, const UNICODE_STRING *path,
                            ACCESS_MASK access, ULONG attributes )
{
    struct wexkey *key;
    sigset_t sigset;
    HANDLE ret = 0;
    WCHAR *copy;

    if (!(copy = copy_path( path->Buffer, path->Length ))) return 0;
    wexkey_lock( &sigset );
    if ((key = alloc_key( &ret )))
    {
        key->kind = WEXKEY_SESSION0;
        key->remote = remote;
        key->generation = generation;
        key->access = access;
        key->attributes = attributes & (OBJ_OPENLINK | OBJ_CASE_INSENSITIVE);
        key->path = copy;
        key->len = path->Length;
    }
    wexkey_unlock( &sigset );
    if (!key) free( copy );
    return ret;
}

/* close a session 0 key's handle there; with the mutex held */
static void s0_free( struct wexkey *key )
{
    unsigned int status;

    if (key->generation == s0_generation && s0_request_fd != -1)
    {
        SERVER_START_REQ( close_handle )
        {
            req->handle = key->remote;
            if (!s0_transfer( &__req, &status )) s0_disconnect();
        }
        SERVER_END_REQ;
    }
}

/* WineExplorer system tier: session 0 serves every program on one connection kind, so a request states the
 * program's registry view (the 32-bit view of a WoW64 program) unless the caller chose one */
ACCESS_MASK wexkey_view( ACCESS_MASK access )
{
    if (access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)) return access;
    return access | (is_wow64() ? KEY_WOW64_32KEY : KEY_WOW64_64KEY);
}

/* open a key of session 0 by its absolute path (not virtualized) */
static NTSTATUS s0_open( HANDLE *key, const UNICODE_STRING *path, ACCESS_MASK access )
{
    unsigned int status, generation = 0;
    ACCESS_MASK view = wexkey_view( access );

    *key = 0;
    SERVER_START_REQ( open_key )
    {
        req->parent     = 0;
        req->access     = view;
        req->attributes = OBJ_CASE_INSENSITIVE;
        wine_server_add_data( req, path->Buffer, path->Length );
        status = wexkey_session0_call( req, 0, NULL, &generation );
        if (!status && !(*key = wexkey_add_session0( reply->hkey, generation, path, view, OBJ_CASE_INSENSITIVE )))
            status = STATUS_NO_MEMORY;
    }
    SERVER_END_REQ;
    return status;
}

/* the absolute path of a name relative to a key of the table; the caller frees path->Buffer */
static NTSTATUS full_path( HANDLE parent, enum wexkey_kind kind, const UNICODE_STRING *name, UNICODE_STRING *path )
{
    struct wexkey *key;
    unsigned int len;
    NTSTATUS status = STATUS_SUCCESS;
    sigset_t sigset;

    wexkey_lock( &sigset );
    if (!(key = get_key( parent, kind ))) status = STATUS_INVALID_HANDLE;
    else
    {
        len = key->len + (name && name->Length ? sizeof(WCHAR) + name->Length : 0);
        if (len > 0xfffe || !(path->Buffer = malloc( max( len, sizeof(WCHAR) )))) status = STATUS_NO_MEMORY;
        else
        {
            memcpy( path->Buffer, key->path, key->len );
            if (name && name->Length)
            {
                path->Buffer[key->len / sizeof(WCHAR)] = '\\';
                memcpy( path->Buffer + key->len / sizeof(WCHAR) + 1, name->Buffer, name->Length );
            }
            path->Length = path->MaximumLength = len;
        }
    }
    wexkey_unlock( &sigset );
    return status;
}

/* where a create or an open goes. A machine key goes to session 0, and the absolute path is kept with its handle
 * there; a per-session key below a session 0 key is opened in this process's server by its absolute path. */
NTSTATUS wexkey_target_init( const OBJECT_ATTRIBUTES *attr, struct wexkey_target *t )
{
    NTSTATUS status;

    memset( t, 0, sizeof(*t) );
    if (wexkey_is_handle( attr->RootDirectory ))
    {
        if ((status = full_path( attr->RootDirectory, WEXKEY_SESSION0, attr->ObjectName, &t->path ))) return status;
        t->owned = TRUE;
        if (!(t->remote = s0_route_path( t->path.Buffer, t->path.Length )))
        {
            t->abs = *attr;
            t->abs.RootDirectory = 0;
            t->abs.ObjectName = &t->path;
            t->use_abs = TRUE;
        }
    }
    else if (!attr->RootDirectory && attr->ObjectName &&
             s0_route_path( attr->ObjectName->Buffer, attr->ObjectName->Length ))
    {
        t->path = *attr->ObjectName;
        t->remote = TRUE;
    }
    return STATUS_SUCCESS;
}

void wexkey_target_free( struct wexkey_target *t )
{
    if (t->owned) free( t->path.Buffer );
}


/***********************************************************************
 * The virtual store (T2b)
 */

static int virt_state;  /* 0 not decided, 1 this process is virtualized, -1 not */

/* 32-bit, limited, not SYSTEM or a service account */
static BOOL virt_qualifies(void)
{
    static const SID_IDENTIFIER_AUTHORITY nt_authority = { SECURITY_NT_AUTHORITY };
    DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
    TOKEN_ELEVATION_TYPE type;
    DWORD len;
    SID *sid;

    if (virt_state) return virt_state > 0;
    virt_state = -1;
    if (!is_wow64()) return FALSE;
    if (NtQueryInformationToken( (HANDLE)~(ULONG_PTR)3, TokenElevationType, &type, sizeof(type), &len ) ||
        type == TokenElevationTypeFull) return FALSE;
    len = sizeof(sid_data);
    if (NtQueryInformationToken( (HANDLE)~(ULONG_PTR)3, TokenUser, sid_data, len, &len )) return FALSE;
    sid = ((TOKEN_USER *)sid_data)->User.Sid;
    if (!memcmp( &sid->IdentifierAuthority, &nt_authority, sizeof(nt_authority) ) && sid->SubAuthorityCount == 1 &&
        sid->SubAuthority[0] >= SECURITY_LOCAL_SYSTEM_RID && sid->SubAuthority[0] <= SECURITY_NETWORK_SERVICE_RID)
        return FALSE;  /* S-1-5-18, 19, 20 */
    virt_state = 1;
    return TRUE;
}

/* the part of an absolute HKLM\Software path below Software (and below Wow6432Node) that is virtualized;
 * FALSE outside the scope */
static BOOL virt_scope( const UNICODE_STRING *path, const WCHAR **rest, unsigned int *rest_count )
{
    static const char * const excluded[] = { "Classes", "Microsoft\\Windows", "Microsoft\\Windows NT" };
    unsigned int count = path->Length / sizeof(WCHAR), n, i;
    const WCHAR *p = path->Buffer;

    if (!(n = wex_session_key_match( p, count, "\\Registry\\Machine\\Software" ))) return FALSE;
    if (n == count) return FALSE;  /* Software itself */
    p += n + 1;
    count -= n + 1;
    if ((n = wex_session_key_match( p, count, "Wow6432Node" )))
    {
        if (n == count) return FALSE;
        p += n + 1;
        count -= n + 1;
    }
    for (i = 0; i < ARRAY_SIZE(excluded); i++) if (wex_session_key_match( p, count, excluded[i] )) return FALSE;
    *rest = p;
    *rest_count = count;
    return TRUE;
}

/* \Registry\User\<SID>\Software\Classes\VirtualStore\Machine\Software\Wow6432Node\<rest> */
static BOOL virt_store_path( const WCHAR *rest, unsigned int rest_count, UNICODE_STRING *store )
{
    static const char tail[] = "\\Software\\Classes\\VirtualStore\\Machine\\Software\\Wow6432Node\\";
    char buffer[256];
    unsigned int n;

    if (wexkey_user_key_path( (HANDLE)~(ULONG_PTR)3, buffer, sizeof(buffer) - (sizeof(tail) - 1), &n )) return FALSE;
    memcpy( buffer + n, tail, sizeof(tail) - 1 );
    n += sizeof(tail) - 1;
    if ((n + rest_count) * sizeof(WCHAR) > 0xfffe) return FALSE;
    if (!(store->Buffer = malloc( (n + rest_count) * sizeof(WCHAR) ))) return FALSE;
    ascii_to_unicode( store->Buffer, buffer, n );
    memcpy( store->Buffer + n, rest, rest_count * sizeof(WCHAR) );
    store->Length = store->MaximumLength = (n + rest_count) * sizeof(WCHAR);
    return TRUE;
}

static NTSTATUS virt_open_path( HANDLE *key, const UNICODE_STRING *path, ACCESS_MASK access, BOOL create )
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING parent;
    NTSTATUS status;
    HANDLE handle;

    InitializeObjectAttributes( &attr, (UNICODE_STRING *)path, OBJ_CASE_INSENSITIVE, 0, NULL );
    /* the store is under HKCU\Software\Classes: never redirected for a 32-bit program */
    if (!create) return NtOpenKeyEx( key, access | KEY_WOW64_64KEY, &attr, 0 );
    status = NtCreateKey( key, access | KEY_WOW64_64KEY, &attr, 0, NULL, 0, NULL );
    if (status != STATUS_OBJECT_NAME_NOT_FOUND) return status;

    /* a key is created only below an existing one: make the missing parents first (VirtualStore\Machine\...) */
    parent = *path;
    while (parent.Length && parent.Buffer[parent.Length / sizeof(WCHAR) - 1] != '\\') parent.Length -= sizeof(WCHAR);
    if (parent.Length <= sizeof(WCHAR)) return status;
    parent.Length -= sizeof(WCHAR);
    parent.MaximumLength = parent.Length;
    if ((status = virt_open_path( &handle, &parent, KEY_CREATE_SUB_KEY, TRUE ))) return status;
    NtClose( handle );
    return NtCreateKey( key, access | KEY_WOW64_64KEY, &attr, 0, NULL, 0, NULL );
}

/* a new virtual key; takes the handles and store_path->Buffer (also when it fails: then the caller closes them) */
static HANDLE virt_add( HANDLE real, HANDLE store, const UNICODE_STRING *real_path, UNICODE_STRING *store_path,
                        ACCESS_MASK access )
{
    struct wexkey *key;
    sigset_t sigset;
    HANDLE ret = 0;
    WCHAR *copy;

    if (!(copy = copy_path( real_path->Buffer, real_path->Length ))) return 0;
    wexkey_lock( &sigset );
    if ((key = alloc_key( &ret )))
    {
        key->kind = WEXKEY_VIRTUAL;
        key->access = access;
        key->real = real;
        key->store = store;
        key->path = copy;
        key->len = real_path->Length;
        key->store_path = *store_path;
        store_path->Buffer = NULL;  /* the key owns it now */
    }
    wexkey_unlock( &sigset );
    if (!key) free( copy );
    return ret;
}

#define VIRT_WRITE (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_CREATE_LINK | DELETE | WRITE_DAC | WRITE_OWNER | \
                    GENERIC_WRITE | GENERIC_ALL)

/* after a create or open of an absolute path that session 0 answered with status (key: its handle on success):
 * virtualize it when the process and the path qualify; returns the final status */
NTSTATUS wexkey_virtualize( NTSTATUS status, HANDLE *key, const UNICODE_STRING *path, ACCESS_MASK access,
                            BOOL create, ULONG *dispos )
{
    UNICODE_STRING store = { 0 };
    const WCHAR *rest;
    unsigned int rest_count;
    HANDLE real = 0, store_key = 0, handle;
    ACCESS_MASK view = access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY);
    BOOL existed = FALSE;

    if (status != STATUS_SUCCESS && status != STATUS_ACCESS_DENIED && status != STATUS_OBJECT_NAME_NOT_FOUND)
        return status;
    if (status == STATUS_ACCESS_DENIED && !(access & VIRT_WRITE)) return status;
    if (!virt_scope( path, &rest, &rest_count ) || !virt_qualifies()) return status;
    if (!virt_store_path( rest, rest_count, &store )) return status;

    if (status == STATUS_SUCCESS)
    {
        /* the real key: virtual only when the store has the key */
        if (virt_open_path( &store_key, &store, KEY_ALL_ACCESS, FALSE )) goto done;
        real = *key;
        existed = TRUE;
    }
    else
    {
        if (!s0_open( &real, path, KEY_READ | view )) existed = TRUE;  /* not virtualized */
        else real = 0;
        if (virt_open_path( &store_key, &store, KEY_ALL_ACCESS, FALSE )) store_key = 0;
        else existed = TRUE;
        if (!store_key && (create || (real && (access & VIRT_WRITE))) &&
            virt_open_path( &store_key, &store, KEY_ALL_ACCESS, TRUE ))
            store_key = 0;
        if (!real && !store_key)
        {
            status = create ? STATUS_ACCESS_DENIED : STATUS_OBJECT_NAME_NOT_FOUND;
            goto done;
        }
    }
    if (!(handle = virt_add( real, store_key, path, &store, access )))
    {
        if (store_key) NtClose( store_key );
        if (real && real != *key) NtClose( real );
        goto done;
    }
    TRACE( "virtualized %s -> %p\n", debugstr_us(path), handle );
    *key = handle;
    if (dispos) *dispos = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    status = STATUS_SUCCESS;
done:
    free( store.Buffer );
    return status;
}

/* the handle is a virtual key of this process */
BOOL wexkey_is_virtual( HANDLE handle )
{
    sigset_t sigset;
    BOOL ret;

    if (!wexkey_is_handle( handle )) return FALSE;
    wexkey_lock( &sigset );
    ret = get_key( handle, WEXKEY_VIRTUAL ) != NULL;
    wexkey_unlock( &sigset );
    return ret;
}

/* the handle that stands for a virtual key in calls that are not virtualized (object queries, security,
 * notifications); any other handle is returned as it is */
HANDLE wexkey_underlying( HANDLE handle )
{
    struct wexkey *key;
    HANDLE ret = handle;
    sigset_t sigset;

    if (!wexkey_is_handle( handle )) return handle;
    wexkey_lock( &sigset );
    if ((key = get_key( handle, WEXKEY_VIRTUAL ))) ret = key->real ? key->real : key->store;
    wexkey_unlock( &sigset );
    return ret;
}

/* the store key of a virtual key, created at the first write */
NTSTATUS wexkey_virtual_store( HANDLE handle, HANDLE *store )
{
    struct wexkey *key;
    UNICODE_STRING path = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE created;
    sigset_t sigset;

    wexkey_lock( &sigset );
    if (!(key = get_key( handle, WEXKEY_VIRTUAL ))) status = STATUS_INVALID_HANDLE;
    else if (key->store) *store = key->store;
    else if (!(path.Buffer = copy_path( key->store_path.Buffer, key->store_path.Length ))) status = STATUS_NO_MEMORY;
    else
    {
        path.Length = path.MaximumLength = key->store_path.Length;
        status = STATUS_PENDING;
    }
    wexkey_unlock( &sigset );
    if (status != STATUS_PENDING) return status;

    status = virt_open_path( &created, &path, KEY_ALL_ACCESS, TRUE );
    free( path.Buffer );
    if (status) return status;
    wexkey_lock( &sigset );
    if ((key = get_key( handle, WEXKEY_VIRTUAL )))
    {
        if (!key->store)  /* else another thread made it meanwhile */
        {
            key->store = created;
            created = 0;
        }
        *store = key->store;
    }
    wexkey_unlock( &sigset );
    if (created) NtClose( created );
    return key ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

static BOOL virt_handles( HANDLE handle, HANDLE *real, HANDLE *store )
{
    struct wexkey *key;
    sigset_t sigset;

    wexkey_lock( &sigset );
    if ((key = get_key( handle, WEXKEY_VIRTUAL )))
    {
        *real = key->real;
        *store = key->store;
    }
    wexkey_unlock( &sigset );
    return key != NULL;
}

/* a create or open relative to a virtual key: the same, by the absolute path of the real key */
NTSTATUS wexkey_virtual_relative( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                  const UNICODE_STRING *class, ULONG options, ULONG *dispos, BOOL create )
{
    OBJECT_ATTRIBUTES abs = *attr;
    UNICODE_STRING path;
    NTSTATUS status;

    if ((status = full_path( attr->RootDirectory, WEXKEY_VIRTUAL, attr->ObjectName, &path ))) return status;
    abs.RootDirectory = 0;
    abs.ObjectName = &path;
    status = create ? NtCreateKey( key, access, &abs, 0, class, options, dispos ) : NtOpenKeyEx( key, access, &abs, options );
    free( path.Buffer );
    return status;
}

/* value of a virtual key: the store first */
NTSTATUS wexkey_virtual_query_value( HANDLE handle, const UNICODE_STRING *name, KEY_VALUE_INFORMATION_CLASS info_class,
                                     void *info, DWORD length, DWORD *result_len )
{
    HANDLE real, store;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;

    if (!virt_handles( handle, &real, &store )) return STATUS_INVALID_HANDLE;
    if (store) status = NtQueryValueKey( store, name, info_class, info, length, result_len );
    if (status == STATUS_OBJECT_NAME_NOT_FOUND && real)
        status = NtQueryValueKey( real, name, info_class, info, length, result_len );
    return status;
}

/* is the name a value (or subkey) of the store key? */
static BOOL virt_in_store( HANDLE store, const WCHAR *name, ULONG name_len, BOOL subkey )
{
    UNICODE_STRING str = { name_len, name_len, (WCHAR *)name };
    KEY_VALUE_BASIC_INFORMATION info;
    OBJECT_ATTRIBUTES attr;
    HANDLE key;
    DWORD len;
    NTSTATUS status;

    if (!store) return FALSE;
    if (!subkey)
    {
        status = NtQueryValueKey( store, &str, KeyValueBasicInformation, &info, sizeof(info), &len );
        return status != STATUS_OBJECT_NAME_NOT_FOUND;
    }
    InitializeObjectAttributes( &attr, &str, OBJ_CASE_INSENSITIVE, store, NULL );
    if (NtOpenKeyEx( &key, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &attr, 0 )) return FALSE;
    NtClose( key );
    return TRUE;
}

/* the number of values (or subkeys) of the store key */
static ULONG virt_store_count( HANDLE store, BOOL subkey )
{
    char buffer[sizeof(KEY_FULL_INFORMATION) + 512];
    KEY_FULL_INFORMATION *full = (KEY_FULL_INFORMATION *)buffer;
    DWORD len;

    if (!store || NtQueryKey( store, KeyFullInformation, full, sizeof(buffer), &len )) return 0;
    return subkey ? full->SubKeys : full->Values;
}

/* name of the index-th value (or subkey) of a key, for skipping duplicates */
static NTSTATUS virt_name( HANDLE key, ULONG index, BOOL subkey, WCHAR *name, ULONG *name_len )
{
    char buffer[sizeof(KEY_BASIC_INFORMATION) + 512 * sizeof(WCHAR)];
    DWORD len;
    NTSTATUS status;

    if (subkey)
    {
        KEY_BASIC_INFORMATION *info = (KEY_BASIC_INFORMATION *)buffer;
        if ((status = NtEnumerateKey( key, index, KeyBasicInformation, info, sizeof(buffer), &len ))) return status;
        *name_len = min( info->NameLength, 512 * sizeof(WCHAR) );
        memcpy( name, info->Name, *name_len );
    }
    else
    {
        KEY_VALUE_BASIC_INFORMATION *info = (KEY_VALUE_BASIC_INFORMATION *)buffer;
        if ((status = NtEnumerateValueKey( key, index, KeyValueBasicInformation, info, sizeof(buffer), &len )))
            return status;
        *name_len = min( info->NameLength, 512 * sizeof(WCHAR) );
        memcpy( name, info->Name, *name_len );
    }
    return STATUS_SUCCESS;
}

/* the number of values (or subkeys) of a virtual key's merged view */
static ULONG virt_merged_count( HANDLE handle, BOOL subkey )
{
    WCHAR name[512];
    HANDLE real, store;
    ULONG count, k, name_len;

    if (!virt_handles( handle, &real, &store )) return 0;
    count = virt_store_count( store, subkey );
    for (k = 0; real && !virt_name( real, k, subkey, name, &name_len ); k++)
        if (!virt_in_store( store, name, name_len, subkey )) count++;
    return count;
}

/* merged enumeration of a virtual key: the store's entries first, then the real key's that the store has not */
NTSTATUS wexkey_virtual_enumerate( HANDLE handle, ULONG index, BOOL subkey, int info_class, void *info,
                                   DWORD length, DWORD *result_len )
{
    WCHAR name[512];
    HANDLE real, store;
    ULONG count, k, name_len;
    NTSTATUS status;

    if (!virt_handles( handle, &real, &store )) return STATUS_INVALID_HANDLE;
    count = virt_store_count( store, subkey );
    if (index < count)
        return subkey ? NtEnumerateKey( store, index, info_class, info, length, result_len )
                      : NtEnumerateValueKey( store, index, info_class, info, length, result_len );
    if (!real) return STATUS_NO_MORE_ENTRIES;
    index -= count;
    for (k = 0; ; k++)
    {
        if ((status = virt_name( real, k, subkey, name, &name_len ))) return status;
        if (virt_in_store( store, name, name_len, subkey )) continue;
        if (!index--) break;
    }
    return subkey ? NtEnumerateKey( real, k, info_class, info, length, result_len )
                  : NtEnumerateValueKey( real, k, info_class, info, length, result_len );
}

/* a virtual key's counts in NtQueryKey's answer are those of the merged view */
void wexkey_virtual_query_key( HANDLE handle, KEY_INFORMATION_CLASS info_class, void *info )
{
    ULONG values, subkeys;

    if (info_class != KeyFullInformation && info_class != KeyCachedInformation) return;
    values = virt_merged_count( handle, FALSE );
    subkeys = virt_merged_count( handle, TRUE );
    if (info_class == KeyFullInformation)
    {
        KEY_FULL_INFORMATION *full = info;
        full->Values = values;
        full->SubKeys = subkeys;
        full->MaxNameLen = max( full->MaxNameLen, 255 * sizeof(WCHAR) );
        full->MaxValueNameLen = max( full->MaxValueNameLen, 255 * sizeof(WCHAR) );
        full->MaxValueDataLen = max( full->MaxValueDataLen, 4096 );
    }
    else
    {
        KEY_CACHED_INFORMATION *cached = info;
        cached->Values = values;
        cached->SubKeys = subkeys;
        cached->MaxNameLen = max( cached->MaxNameLen, 255 * sizeof(WCHAR) );
        cached->MaxValueNameLen = max( cached->MaxValueNameLen, 255 * sizeof(WCHAR) );
        cached->MaxValueDataLen = max( cached->MaxValueDataLen, 4096 );
    }
}


/***********************************************************************
 * Calls on any key of the table, close and duplication
 */

/* a request on a key handle: to session 0 for a session 0 key, to this process's server for any other (a virtual
 * key goes to its real key, or its store key when it has none). *field is the request's handle field. */
unsigned int wexkey_server_call( void *req_ptr, HANDLE handle, obj_handle_t *field )
{
    if (wexkey_is_handle( handle ))
    {
        HANDLE target = wexkey_underlying( handle );

        if (wexkey_is_handle( target )) return wexkey_session0_call( req_ptr, target, field, NULL );
        *field = wine_server_obj_handle( target );  /* a virtual key's store key, in this process's server */
    }
    return wine_server_call( req_ptr );
}

NTSTATUS wexkey_close( HANDLE handle )
{
    struct wexkey *key;
    HANDLE real = 0, store = 0;
    WCHAR *path = NULL, *store_path = NULL;
    sigset_t sigset;

    wexkey_lock( &sigset );
    if ((key = get_key( handle, WEXKEY_FREE )))
    {
        if (key->kind == WEXKEY_SESSION0) s0_free( key );
        real = key->real;
        store = key->store;
        path = key->path;
        store_path = key->store_path.Buffer;
        memset( key, 0, sizeof(*key) );  /* WEXKEY_FREE */
    }
    wexkey_unlock( &sigset );
    if (!key) return STATUS_INVALID_HANDLE;
    if (real) NtClose( real );
    if (store) NtClose( store );
    free( path );
    free( store_path );
    return STATUS_SUCCESS;
}

/* duplicate a session 0 key within this process */
static NTSTATUS s0_duplicate( HANDLE source, HANDLE *dest, ACCESS_MASK access, ULONG options )
{
    struct wexkey *key;
    unsigned int status, generation = 0;
    obj_handle_t remote = 0;
    ULONG attributes;
    UNICODE_STRING copy;
    sigset_t sigset;

    wexkey_lock( &sigset );
    if (!(key = get_key( source, WEXKEY_SESSION0 )))
    {
        wexkey_unlock( &sigset );
        return STATUS_INVALID_HANDLE;
    }
    /* the view stays the source's: a reopen by the path needs it */
    if (options & DUPLICATE_SAME_ACCESS) access = key->access;
    else access = (access & ~(KEY_WOW64_32KEY | KEY_WOW64_64KEY)) | (key->access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY));
    attributes = key->attributes;
    copy.Length = copy.MaximumLength = key->len;
    copy.Buffer = copy_path( key->path, key->len );
    wexkey_unlock( &sigset );
    if (!copy.Buffer) return STATUS_NO_MEMORY;

    SERVER_START_REQ( dup_handle )
    {
        req->src_process = wine_server_obj_handle( NtCurrentProcess() );
        req->dst_process = wine_server_obj_handle( NtCurrentProcess() );
        req->access      = access;
        req->attributes  = 0;
        req->options     = options & ~DUPLICATE_CLOSE_SOURCE;
        status = wexkey_session0_call( req, source, &req->src_handle, &generation );
        remote = reply->handle;
    }
    SERVER_END_REQ;
    if (!status && !(*dest = wexkey_add_session0( remote, generation, &copy, access, attributes )))
        status = STATUS_NO_MEMORY;
    free( copy.Buffer );
    return status;
}

/* duplicate a virtual key within this process: the copy has its own handles of the real and the store key. As with
 * any handle, a close of the source by another thread meanwhile is the program's race. */
static NTSTATUS virt_duplicate( HANDLE source, HANDLE *dest, ACCESS_MASK access, ULONG options )
{
    struct wexkey *key;
    HANDLE real, store, new_real = 0, new_store = 0;
    UNICODE_STRING path = { 0 }, store_path = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    sigset_t sigset;

    wexkey_lock( &sigset );
    if (!(key = get_key( source, WEXKEY_VIRTUAL )))
    {
        wexkey_unlock( &sigset );
        return STATUS_INVALID_HANDLE;
    }
    if (options & DUPLICATE_SAME_ACCESS) access = key->access;
    real = key->real;
    store = key->store;
    path.Length = path.MaximumLength = key->len;
    store_path.Length = store_path.MaximumLength = key->store_path.Length;
    if (!(path.Buffer = copy_path( key->path, key->len )) ||
        !(store_path.Buffer = copy_path( key->store_path.Buffer, key->store_path.Length )))
        status = STATUS_NO_MEMORY;
    wexkey_unlock( &sigset );

    if (!status && real)
        status = NtDuplicateObject( NtCurrentProcess(), real, NtCurrentProcess(), &new_real, 0, 0, DUPLICATE_SAME_ACCESS );
    if (!status && store)
        status = NtDuplicateObject( NtCurrentProcess(), store, NtCurrentProcess(), &new_store, 0, 0, DUPLICATE_SAME_ACCESS );
    if (!status && !(*dest = virt_add( new_real, new_store, &path, &store_path, access ))) status = STATUS_NO_MEMORY;
    if (status)
    {
        if (new_real) NtClose( new_real );
        if (new_store) NtClose( new_store );
    }
    free( path.Buffer );
    free( store_path.Buffer );
    return status;
}

/* the record of a key sent to another process */
struct wexkey_record
{
    unsigned int magic;            /* WEXKEY_RECORD_MAGIC */
    unsigned int kind;             /* enum wexkey_kind */
    unsigned int access;           /* session 0: with the view */
    unsigned int attributes;       /* session 0: for the open */
    unsigned int real_access;      /* virtual: the real key's, with the view (0: no real key) */
    unsigned int real_attributes;  /* virtual */
    unsigned int len;              /* bytes of the path that follows: the real key's for a virtual key */
};

#define WEXKEY_RECORD_MAGIC 0x59454b57  /* "WKEY" */

/* is the process handle this process? */
static BOOL is_current_process( HANDLE process )
{
    return process == NtCurrentProcess() || !NtCompareObjects( process, NtCurrentProcess() );
}

/* duplicate a key of this process into another process: the target makes its own entry from a record */
static NTSTATUS send_key( HANDLE source, HANDLE process, HANDLE *dest, ACCESS_MASK access, ULONG options )
{
    struct wexkey *key, *real;
    struct wexkey_record rec;
    WCHAR *path = NULL;
    LARGE_INTEGER size;
    HANDLE section;
    void *view = NULL;
    SIZE_T view_size = 0;
    union apc_call call;
    union apc_result result;
    NTSTATUS status = STATUS_SUCCESS;
    sigset_t sigset;

    memset( &rec, 0, sizeof(rec) );
    wexkey_lock( &sigset );
    if (!(key = get_key( source, WEXKEY_FREE ))) status = STATUS_INVALID_HANDLE;
    else
    {
        rec.magic = WEXKEY_RECORD_MAGIC;
        rec.kind = key->kind;
        if (options & DUPLICATE_SAME_ACCESS) rec.access = key->access;
        else if (key->kind == WEXKEY_SESSION0)
            rec.access = (access & ~(KEY_WOW64_32KEY | KEY_WOW64_64KEY)) |
                         (key->access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY));
        else rec.access = access;
        rec.attributes = key->attributes;
        if (key->kind == WEXKEY_VIRTUAL && key->real && (real = get_key( key->real, WEXKEY_SESSION0 )))
        {
            rec.real_access = real->access;
            rec.real_attributes = real->attributes;
        }
        rec.len = key->len;
        if (!(path = copy_path( key->path, key->len ))) status = STATUS_NO_MEMORY;
    }
    wexkey_unlock( &sigset );
    if (status) return status;

    size.QuadPart = sizeof(rec) + rec.len;
    status = NtCreateSection( &section, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY, NULL, &size,
                              PAGE_READWRITE, SEC_COMMIT, 0 );
    if (status) goto done;
    status = NtMapViewOfSection( section, NtCurrentProcess(), &view, 0, 0, NULL, &view_size, ViewShare, 0,
                                 PAGE_READWRITE );
    if (status)
    {
        NtClose( section );
        goto done;
    }
    memcpy( view, &rec, sizeof(rec) );
    memcpy( (char *)view + sizeof(rec), path, rec.len );
    NtUnmapViewOfSection( NtCurrentProcess(), view );

    memset( &call, 0, sizeof(call) );
    call.wex_regkey.type    = APC_WEX_REGKEY;
    call.wex_regkey.section = wine_server_obj_handle( section );
    call.wex_regkey.size    = sizeof(rec) + rec.len;
    status = server_queue_process_apc( process, &call, &result );
    NtClose( section );
    if (!status && !(status = result.wex_regkey.status)) *dest = wine_server_ptr_handle( result.wex_regkey.handle );
done:
    free( path );
    return status;
}

/* a key sent by another process (send_key, in the APC_WEX_REGKEY system APC): a new entry of this process for it */
NTSTATUS wexkey_receive( HANDLE section, unsigned int size, HANDLE *handle )
{
    struct wexkey_record rec;
    UNICODE_STRING path = { 0 }, store_path = { 0 };
    const WCHAR *rest;
    unsigned int rest_count;
    HANDLE real = 0, store = 0;
    void *view = NULL;
    SIZE_T view_size = 0;
    NTSTATUS status;

    *handle = 0;
    if (size < sizeof(rec) || size > sizeof(rec) + 0xfffe) return STATUS_INVALID_PARAMETER;
    if ((status = NtMapViewOfSection( section, NtCurrentProcess(), &view, 0, 0, NULL, &view_size, ViewShare, 0,
                                      PAGE_READONLY )))
        return status;
    if (view_size < size) status = STATUS_INVALID_PARAMETER;
    else
    {
        memcpy( &rec, view, sizeof(rec) );
        if (rec.magic != WEXKEY_RECORD_MAGIC || !rec.len || (rec.len & 1) || rec.len != size - sizeof(rec) ||
            (rec.kind != WEXKEY_SESSION0 && rec.kind != WEXKEY_VIRTUAL))
            status = STATUS_INVALID_PARAMETER;
        else if (!(path.Buffer = malloc( rec.len ))) status = STATUS_NO_MEMORY;
        else
        {
            memcpy( path.Buffer, (char *)view + sizeof(rec), rec.len );
            path.Length = path.MaximumLength = rec.len;
        }
    }
    NtUnmapViewOfSection( NtCurrentProcess(), view );
    if (status) return status;

    /* only a key that session 0 holds for this process, opened at its first use with this process's token */
    if (!s0_route_path( path.Buffer, path.Length )) status = STATUS_INVALID_PARAMETER;
    else if (rec.kind == WEXKEY_SESSION0)
    {
        if (!(*handle = wexkey_add_session0( 0, S0_NOT_OPEN, &path, rec.access, rec.attributes )))
            status = STATUS_NO_MEMORY;
    }
    else if (!virt_scope( &path, &rest, &rest_count ) || !virt_store_path( rest, rest_count, &store_path ))
        status = STATUS_INVALID_PARAMETER;
    else
    {
        /* the store key is this process's user's */
        if (virt_open_path( &store, &store_path, KEY_ALL_ACCESS, FALSE )) store = 0;
        if (rec.real_access && !(real = wexkey_add_session0( 0, S0_NOT_OPEN, &path, rec.real_access,
                                                             rec.real_attributes )))
            status = STATUS_NO_MEMORY;
        else if (!real && !store) status = STATUS_OBJECT_NAME_NOT_FOUND;
        else if (!(*handle = virt_add( real, store, &path, &store_path, rec.access ))) status = STATUS_NO_MEMORY;
        if (status)
        {
            if (real) wexkey_close( real );
            if (store) NtClose( store );
        }
    }
    TRACE( "received %s -> %p, status %08x\n", debugstr_us(&path), *handle, (unsigned int)status );
    free( path.Buffer );
    free( store_path.Buffer );
    return status;
}

/* NtDuplicateObject of a handle in the table's range */
NTSTATUS wexkey_duplicate( HANDLE source_process, HANDLE source, HANDLE dest_process, HANDLE *dest,
                           ACCESS_MASK access, ULONG attributes, ULONG options )
{
    sigset_t sigset;
    enum wexkey_kind kind = WEXKEY_FREE;
    struct wexkey *key;
    NTSTATUS status = STATUS_SUCCESS;

    if (dest) *dest = 0;
    if (!is_current_process( source_process ))
    {
        /* a key of another process: that process duplicates it (and sends it when the target is not itself) */
        union apc_call call;
        union apc_result result;

        memset( &call, 0, sizeof(call) );
        call.dup_handle.type        = APC_DUP_HANDLE;
        call.dup_handle.src_handle  = wine_server_obj_handle( source );
        call.dup_handle.dst_process = wine_server_obj_handle( dest_process );
        call.dup_handle.access      = access;
        call.dup_handle.attributes  = attributes;
        call.dup_handle.options     = options;
        if ((status = server_queue_process_apc( source_process, &call, &result ))) return status;
        if (!result.dup_handle.status && dest) *dest = wine_server_ptr_handle( result.dup_handle.handle );
        return result.dup_handle.status;
    }

    wexkey_lock( &sigset );
    if ((key = get_key( source, WEXKEY_FREE ))) kind = key->kind;
    wexkey_unlock( &sigset );
    if (kind == WEXKEY_FREE) return STATUS_INVALID_HANDLE;

    /* inheritance (OBJ_INHERIT in attributes) is not done: see the top of this file */
    if (dest)
    {
        if (!is_current_process( dest_process )) status = send_key( source, dest_process, dest, access, options );
        else if (kind == WEXKEY_SESSION0) status = s0_duplicate( source, dest, access, options );
        else status = virt_duplicate( source, dest, access, options );
    }
    if (options & DUPLICATE_CLOSE_SOURCE) wexkey_close( source );
    return status;
}
