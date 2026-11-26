/*
 * WineExplorer: accounts_forward, the accounts service's way to the root helper
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

/* include/wine/wex_accounts.h says what this is. named_pipe.c checks the connection (H1) and calls accounts_call_start
 * with the verdict and the caller's uid; this file builds the helper's request, sends it as one datagram on a
 * connection of its own (the helper serves one connection at a time and drops idle ones), and waits for the reply in
 * the server's loop: a call object the client waits on, signaled with the reply, a closed connection, or after
 * WEX_ACCOUNTS_TIMEOUT seconds. */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "object.h"
#include "process.h"
#include "request.h"
#include "thread.h"
#include "session0.h"
#define WEX_ACCOUNTS_CONSTANTS_ONLY
#include "wine/wex_accounts.h"

struct accounts_call
{
    struct object         obj;
    struct object        *sync;          /* signaled when the call has ended */
    struct fd            *fd;            /* the connection to the helper, until the reply */
    struct timeout_user  *timeout;
    unsigned int          request_id;
    unsigned int          status;        /* STATUS_SUCCESS, STATUS_PIPE_DISCONNECTED or STATUS_IO_TIMEOUT */
    unsigned short        helper_status;
    char                 *reply;
    data_size_t           reply_len;
};

static void call_dump( struct object *obj, int verbose );
static struct object *call_get_sync( struct object *obj );
static void call_destroy( struct object *obj );
static void call_poll_event( struct fd *fd, int event );

static const struct object_ops accounts_call_ops =
{
    .size     = sizeof(struct accounts_call),
    .type     = &no_type,
    .dump     = call_dump,
    .get_sync = call_get_sync,
    .destroy  = call_destroy,
};

static const struct fd_ops accounts_call_fd_ops = { .poll_event = call_poll_event };

static unsigned int next_request_id;

static void call_dump( struct object *obj, int verbose )
{
    struct accounts_call *call = (struct accounts_call *)obj;
    fprintf( stderr, "Accounts call request %u status %x\n", call->request_id, call->status );
}

static struct object *call_get_sync( struct object *obj )
{
    return grab_object( ((struct accounts_call *)obj)->sync );
}

/* the call has ended: no more connection, no more timeout, the waiter wakes */
static void call_end( struct accounts_call *call, unsigned int status )
{
    if (call->fd)
    {
        set_fd_events( call->fd, -1 );
        release_object( call->fd );
        call->fd = NULL;
    }
    if (call->timeout) remove_timeout_user( call->timeout );
    call->timeout = NULL;
    call->status = status;
    signal_sync( call->sync );
}

static void call_destroy( struct object *obj )
{
    struct accounts_call *call = (struct accounts_call *)obj;

    if (call->fd) release_object( call->fd );
    if (call->timeout) remove_timeout_user( call->timeout );
    if (call->sync) release_object( call->sync );
    free( call->reply );
}

static void call_timeout( void *private )
{
    struct accounts_call *call = private;

    call->timeout = NULL;
    call_end( call, STATUS_IO_TIMEOUT );
}

static void call_poll_event( struct fd *fd, int event )
{
    struct accounts_call *call = get_fd_user( fd );
    struct wex_accounts_reply hdr;
    char *buf;
    ssize_t n;

    if (!(event & POLLIN))
    {
        call_end( call, STATUS_PIPE_DISCONNECTED );
        return;
    }
    if (!(buf = malloc( WEX_ACCOUNTS_MAX_FRAME + 1 )))
    {
        call_end( call, STATUS_NO_MEMORY );
        return;
    }
    n = recv( get_unix_fd( fd ), buf, WEX_ACCOUNTS_MAX_FRAME + 1, MSG_DONTWAIT );
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
    {
        free( buf );
        return;
    }
    /* one reply of the helper's: its magic, version, our request id, a body of the size it says; anything else is a
     * helper that cannot be talked to */
    if (n < (ssize_t)sizeof(hdr) || n > WEX_ACCOUNTS_MAX_FRAME)
    {
        free( buf );
        call_end( call, STATUS_PIPE_DISCONNECTED );
        return;
    }
    memcpy( &hdr, buf, sizeof(hdr) );
    if (hdr.magic != WEX_ACCOUNTS_MAGIC || hdr.version != WEX_ACCOUNTS_VERSION || hdr.request_id != call->request_id ||
        hdr.body_len != n - sizeof(hdr))
    {
        free( buf );
        call_end( call, STATUS_PIPE_DISCONNECTED );
        return;
    }
    call->helper_status = hdr.status;
    call->reply_len = hdr.body_len;
    memmove( buf, buf + sizeof(hdr), hdr.body_len );
    call->reply = buf;
    call_end( call, STATUS_SUCCESS );
}

/* a local account's uid from /etc/passwd itself: never NSS, which on a domain member can wait for a directory server
 * while the whole server waits (the reviewer's A1); the helper's account is a local system account. -1 if none. */
static uid_t local_account_uid( const char *name )
{
    char line[1024], *uid_field, *end;
    size_t len = strlen( name );
    uid_t uid = (uid_t)-1;
    unsigned long val;
    int at_start = 1, start;
    FILE *f;

    if (!(f = fopen( "/etc/passwd", "re" ))) return uid;
    while (fgets( line, sizeof(line), f ))
    {
        /* name:password:uid:... (the rest of a line longer than the buffer is skipped) */
        start = at_start;
        at_start = strchr( line, '\n' ) != NULL;
        if (!start || strncmp( line, name, len ) || line[len] != ':') continue;
        if (!(uid_field = strchr( line + len + 1, ':' ))) break;
        uid_field++;
        val = strtoul( uid_field, &end, 10 );
        if (end != uid_field && *end == ':') uid = (uid_t)val;
        break;
    }
    fclose( f );
    return uid;
}

/* the uid of accountsd.conf's helper_user (root-owned configuration, read at each request: a name, looked up in
 * /etc/passwd, or a number); -1 when there is none, or when it is root */
static uid_t helper_uid(void)
{
    char line[256], *p, *end;
    uid_t uid = (uid_t)-1;
    unsigned long val;
    FILE *f;

    if (!(f = fopen( WEX_ACCOUNTS_CONFIG, "re" ))) return uid;
    while (fgets( line, sizeof(line), f ))
    {
        if (strncmp( line, "helper_user=", 12 )) continue;
        if ((p = strpbrk( line + 12, "\r\n" ))) *p = 0;
        if (!line[12]) continue;
        val = strtoul( line + 12, &end, 10 );
        uid = *end ? local_account_uid( line + 12 ) : (uid_t)val;
    }
    fclose( f );
    return uid ? uid : (uid_t)-1;
}

/* a connection to the helper, after checking that its account serves the socket; -1 if not */
static int connect_helper(void)
{
    struct sockaddr_un addr;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    uid_t uid = helper_uid();
    int sock;

    if (uid == (uid_t)-1) return -1;
    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, WEX_ACCOUNTS_SOCKET );
    if ((sock = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 )) == -1) return -1;
    if (connect( sock, (struct sockaddr *)&addr, sizeof(addr) ) == -1 ||
        getsockopt( sock, SOL_SOCKET, SO_PEERCRED, &cred, &len ) || cred.uid != uid)
    {
        close( sock );
        return -1;
    }
    return sock;
}

/* Start a call: the request (header built here, nothing from the client or the service in it but op and body) goes
 * out at once; the returned object is signaled when the call has ended. NULL with the error set. */
struct object *accounts_call_start( unsigned int op, unsigned int verdict, unsigned int caller_uid,
                                    const void *body, data_size_t size )
{
    struct wex_accounts_request hdr;
    struct accounts_call *call;
    char *frame;
    int sock;

    if (op > 0xffff || size > WEX_ACCOUNTS_MAX_FRAME - sizeof(hdr))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    if (!(call = alloc_object( &accounts_call_ops ))) return NULL;
    call->fd = NULL;
    call->timeout = NULL;
    call->reply = NULL;
    call->reply_len = 0;
    call->helper_status = 0;
    call->status = STATUS_PENDING;
    if (!++next_request_id) next_request_id = 1;
    call->request_id = next_request_id;
    if (!(call->sync = create_internal_sync( 1, 0 )))
    {
        release_object( call );
        return NULL;
    }
    hdr.magic = WEX_ACCOUNTS_MAGIC;
    hdr.version = WEX_ACCOUNTS_VERSION;
    hdr.op = op;
    hdr.request_id = call->request_id;
    hdr.flags = 0;
    hdr.verdict = verdict;
    hdr.caller_uid = caller_uid;
    hdr.body_len = size;
    if ((sock = connect_helper()) == -1)
    {
        call_end( call, STATUS_PIPE_DISCONNECTED );  /* no helper (or not its account): the service says 1722 */
        return &call->obj;
    }
    if (!(frame = malloc( sizeof(hdr) + size )))
    {
        close( sock );
        release_object( call );
        set_error( STATUS_NO_MEMORY );
        return NULL;
    }
    memcpy( frame, &hdr, sizeof(hdr) );
    memcpy( frame + sizeof(hdr), body, size );
    if (send( sock, frame, sizeof(hdr) + size, MSG_NOSIGNAL | MSG_DONTWAIT ) != (ssize_t)(sizeof(hdr) + size))
    {
        free( frame );
        close( sock );
        call_end( call, STATUS_PIPE_DISCONNECTED );
        return &call->obj;
    }
    free( frame );
    if (!(call->fd = create_anonymous_fd( &accounts_call_fd_ops, sock, &call->obj, 0 )))
    {
        call_end( call, STATUS_PIPE_DISCONNECTED );
        return &call->obj;
    }
    set_fd_events( call->fd, POLLIN );
    call->timeout = add_timeout_user( -(timeout_t)WEX_ACCOUNTS_TIMEOUT * TICKS_PER_SEC, call_timeout, call );
    return &call->obj;
}

/* the result of a call that has ended (STATUS_PENDING while it has not) */
DECL_HANDLER(accounts_result)
{
    struct accounts_call *call = (struct accounts_call *)get_handle_obj( current->process, req->call, 0,
                                                                          &accounts_call_ops );

    if (!call) return;
    if (call->status == STATUS_PENDING) set_error( STATUS_PENDING );
    else if (call->status) set_error( call->status );
    else
    {
        reply->helper_status = call->helper_status;
        reply->total = call->reply_len;
        if (call->reply_len > get_reply_max_size()) set_error( STATUS_BUFFER_OVERFLOW );
        else set_reply_data( call->reply, call->reply_len );
    }
    release_object( call );
}
