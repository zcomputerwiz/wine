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

#ifndef __WEVTSVC_UNIXLIB_H
#define __WEVTSVC_UNIXLIB_H

#include "wine/unixlib.h"

/* Wine's Winsock has no AF_UNIX: the connection to the bridge's socket (a fixed path, wine/wex_journal.h) is made
 * here. Every parameter is a fixed-size integer, so the 32-bit side passes the same layout. */

struct journal_connect_params
{
    int          fd;        /* out */
};

struct journal_io_params
{
    int          fd;
    unsigned int size;      /* send: bytes to send; recv: buffer size */
    UINT64       buffer;
    unsigned int timeout;   /* recv: milliseconds */
    unsigned int done;      /* out: bytes sent or received */
};

struct journal_close_params
{
    int          fd;
};

enum journal_funcs
{
    unix_journal_connect,
    unix_journal_send,
    unix_journal_recv,
    unix_journal_close,
    unix_journal_funcs_count,
};

#endif /* __WEVTSVC_UNIXLIB_H */
