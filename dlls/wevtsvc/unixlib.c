/*
 * WineExplorer: wevtsvc's Unix side, the socket to the journal bridge
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/wex_journal.h"
#include "unixlib.h"

#define IO_DEADLINE_MS 5000   /* connect and each send: a bridge that stops reading cannot hold the service */

static long long now_ms(void)
{
    struct timespec t;

    clock_gettime( CLOCK_MONOTONIC, &t );
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* a connected socket to path, or -errno. The same shape as wexjoin's connect_to (src/dlls/wexjoin/unixlib.c), with
 * close-on-exec, a non-blocking socket and a deadline on the connect (a third user is when a shared header pays) */
static int connect_to( const char *path )
{
    struct sockaddr_un addr;
    int fd, e;

    if ((fd = socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 )) < 0) return -EIO;
    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, path );
    if (!connect( fd, (struct sockaddr *)&addr, sizeof(addr) )) return fd;
    e = errno;
    if (e == EAGAIN || e == EINPROGRESS)
    {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        socklen_t len = sizeof(e);

        if (poll( &pfd, 1, IO_DEADLINE_MS ) == 1 && !getsockopt( fd, SOL_SOCKET, SO_ERROR, &e, &len ) && !e)
            return fd;
        if (!e) e = ETIMEDOUT;
    }
    close( fd );
    return -e;
}

/* the bridge's socket: a fixed path, never one from a caller */
static NTSTATUS journal_connect( void *args )
{
    struct journal_connect_params *params = args;
    int fd = connect_to( WEXJ_SOCKET );

    params->fd = -1;
    if (fd < 0) return STATUS_PIPE_NOT_AVAILABLE;
    params->fd = fd;
    return STATUS_SUCCESS;
}

/* the whole buffer within the deadline; MSG_NOSIGNAL: a bridge gone mid-frame must not signal the service */
static NTSTATUS journal_send( void *args )
{
    struct journal_io_params *params = args;
    const char *p = (const char *)(ULONG_PTR)params->buffer;
    unsigned int left = params->size;
    long long deadline = now_ms() + IO_DEADLINE_MS;
    ssize_t n;

    params->done = 0;
    while (left)
    {
        struct pollfd pfd = { params->fd, POLLOUT, 0 };
        long long wait = deadline - now_ms();

        if (wait <= 0) return STATUS_TIMEOUT;
        if (poll( &pfd, 1, (int)wait ) < 0 && errno != EINTR) return STATUS_PIPE_DISCONNECTED;
        if (!(pfd.revents & (POLLOUT | POLLERR | POLLHUP))) continue;
        if ((n = send( params->fd, p, left, MSG_NOSIGNAL | MSG_DONTWAIT )) == -1)
        {
            if (errno == EINTR || errno == EAGAIN) continue;
            return STATUS_PIPE_DISCONNECTED;
        }
        p += n;
        left -= n;
        params->done += n;
    }
    return STATUS_SUCCESS;
}

/* what has arrived within the timeout: STATUS_TIMEOUT for nothing, STATUS_PIPE_DISCONNECTED at the end */
static NTSTATUS journal_recv( void *args )
{
    struct journal_io_params *params = args;
    struct pollfd pfd = { params->fd, POLLIN, 0 };
    ssize_t n;
    int r;

    params->done = 0;
    while ((r = poll( &pfd, 1, params->timeout )) == -1 && errno == EINTR);
    if (r == 0) return STATUS_TIMEOUT;
    if (r == -1) return STATUS_PIPE_DISCONNECTED;
    while ((n = recv( params->fd, (char *)(ULONG_PTR)params->buffer, params->size, MSG_DONTWAIT )) == -1 &&
           errno == EINTR);
    if (n < 0 && errno == EAGAIN) return STATUS_TIMEOUT;
    if (n <= 0) return STATUS_PIPE_DISCONNECTED;
    params->done = n;
    return STATUS_SUCCESS;
}

static NTSTATUS journal_close( void *args )
{
    struct journal_close_params *params = args;

    if (params->fd != -1) close( params->fd );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    journal_connect,
    journal_send,
    journal_recv,
    journal_close,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_journal_funcs_count );

#ifdef _WIN64
/* the parameters have the same layout for a 32-bit caller */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    journal_connect,
    journal_send,
    journal_recv,
    journal_close,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_journal_funcs_count );
#endif
