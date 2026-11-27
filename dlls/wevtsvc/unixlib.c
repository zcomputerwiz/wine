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
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/wex_journal.h"
#include "unixlib.h"

/* the bridge's socket: a fixed path, never one from a caller */
static NTSTATUS journal_connect( void *args )
{
    struct journal_connect_params *params = args;
    struct sockaddr_un addr;
    int fd;

    params->fd = -1;
    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, WEXJ_SOCKET );
    if ((fd = socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 )) == -1) return STATUS_UNSUCCESSFUL;
    if (connect( fd, (struct sockaddr *)&addr, sizeof(addr) ) == -1)
    {
        close( fd );
        return STATUS_PIPE_NOT_AVAILABLE;
    }
    params->fd = fd;
    return STATUS_SUCCESS;
}

static NTSTATUS journal_send( void *args )
{
    struct journal_io_params *params = args;
    const char *p = (const char *)(ULONG_PTR)params->buffer;
    unsigned int left = params->size;
    ssize_t n;

    params->done = 0;
    while (left)
    {
        if ((n = send( params->fd, p, left, MSG_NOSIGNAL )) == -1)
        {
            if (errno == EINTR) continue;
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
    while ((n = recv( params->fd, (char *)(ULONG_PTR)params->buffer, params->size, 0 )) == -1 && errno == EINTR);
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
