/*
 * WineExplorer: the accounts service's way to the helper (accounts_forward)
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

/* The Local Users and Groups service (wexaccounts, LocalSystem in session 0, \\.\pipe\accounts, reached from sessions
 * as \\.\pipe\WexSession0\accounts) only relays: the server decides who the caller is and speaks to the root helper
 * itself (the reviewer's H1). accounts_forward takes the server end of a client's connection; the server refuses it
 * unless the pipe is the reserved accounts pipe (made by LocalSystem in session 0), the connection came over the bridge
 * with a link-bound token, and then builds the helper's request header (verdict from token_caller_class, the link's
 * uid), sends one datagram to the helper's socket (checked: its uid is accountsd.conf's helper_user) and waits at most
 * 15 s, without blocking the server. The helper's frame is GPO's (src/include/wexaccounts_proto.h); the constants the
 * server needs are repeated here, and must stay equal to that file's. */

#ifndef __WINE_WINE_WEX_ACCOUNTS_H
#define __WINE_WINE_WEX_ACCOUNTS_H

#define WEX_ACCOUNTS_PIPE       "accounts"                         /* the reserved name, in session 0 */
#define WEX_ACCOUNTS_SOCKET     "/run/wineexplorer/accounts.sock"
#define WEX_ACCOUNTS_CONFIG     "/etc/wineexplorer/accountsd.conf" /* helper_user= */
#define WEX_ACCOUNTS_MAGIC      0x41584557u                        /* "WEXA" */
#define WEX_ACCOUNTS_VERSION    1
#define WEX_ACCOUNTS_MAX_FRAME  65536
#define WEX_ACCOUNTS_MAX_REPLY  (WEX_ACCOUNTS_MAX_FRAME - 16)       /* a reply body at most (the frame less its header) */
#define WEX_ACCOUNTS_TIMEOUT    15                                  /* seconds */
#define WEX_ACCOUNTS_VERDICT_NONE     0
#define WEX_ACCOUNTS_VERDICT_USER     1
#define WEX_ACCOUNTS_VERDICT_ELEVATED 2

/* the service whose token may create the reserved pipe (B114), and its SID, NT SERVICE\wexaccounts:
 * S-1-5-80-2861554723-205673086-2129874643-4023588997-3715158216, the SHA-1 of "WEXACCOUNTS" in UTF-16LE as five
 * little-endian sub-authorities (RtlCreateServiceSid); the service needs SERVICE_SID_TYPE_UNRESTRICTED */
#define WEX_ACCOUNTS_SERVICE    "wexaccounts"
#define WEX_ACCOUNTS_SERVICE_RIDS 0xaa8fdc23, 0x0c42527e, 0x7ef34ed3, 0xefd31885, 0xdd70d0c8

struct wex_accounts_request   /* struct wexa_request */
{
    unsigned int   magic;
    unsigned short version;
    unsigned short op;
    unsigned int   request_id;
    unsigned int   flags;
    unsigned int   verdict;
    unsigned int   caller_uid;
    unsigned int   body_len;
};

struct wex_accounts_reply     /* struct wexa_reply */
{
    unsigned int   magic;
    unsigned short version;
    unsigned short status;
    unsigned int   request_id;
    unsigned int   body_len;
};

#ifndef WEX_ACCOUNTS_CONSTANTS_ONLY

#include "wine/server.h"

/* Forward one request of the client connected to pipe (the service's server end) to the helper: op, and as the body
 * len bytes at offset of the message the service last read from that client, whole, in message mode (the server keeps
 * it; each message is forwarded once, the reviewer's A3). The helper's reply body goes into out, which must hold
 * WEX_ACCOUNTS_MAX_REPLY bytes, so that no reply is ever lost (A4); *out_len: in, out's size; out, the bytes. *status is
 * the helper's status.
 * STATUS_INVALID_PARAMETER: out too small, or offset and len outside the message;
 * STATUS_INVALID_DEVICE_STATE: no whole message read since the last forward;
 * STATUS_ACCESS_DENIED: not the reserved pipe, or not a bridged client;
 * STATUS_PIPE_DISCONNECTED: no helper, or it went away; STATUS_IO_TIMEOUT: no reply within 15 s. */
static inline NTSTATUS wex_accounts_forward( HANDLE pipe, ULONG offset, ULONG len, ULONG op, void *out,
                                             ULONG *out_len, USHORT *status )
{
    LARGE_INTEGER timeout;
    HANDLE call = 0;
    NTSTATUS ret;

    if (*out_len < WEX_ACCOUNTS_MAX_REPLY) return STATUS_INVALID_PARAMETER;
    SERVER_START_REQ( accounts_forward )
    {
        req->pipe = wine_server_obj_handle( pipe );
        req->op = op;
        req->offset = offset;
        req->length = len;
        if (!(ret = wine_server_call( req ))) call = wine_server_ptr_handle( reply->call );
    }
    SERVER_END_REQ;
    if (ret) return ret;
    timeout.QuadPart = -(LONGLONG)(WEX_ACCOUNTS_TIMEOUT + 5) * 10000000;  /* the server ends the call at 15 s */
    NtWaitForSingleObject( call, FALSE, &timeout );
    SERVER_START_REQ( accounts_result )
    {
        req->call = wine_server_obj_handle( call );
        wine_server_set_reply( req, out, *out_len );  /* (inside the block, reply is the server's reply) */
        ret = wine_server_call( req );
        *status = reply->helper_status;
        *out_len = wine_server_reply_size( reply );
    }
    SERVER_END_REQ;
    NtClose( call );
    return ret;
}

#endif /* WEX_ACCOUNTS_CONSTANTS_ONLY */

#endif /* __WINE_WINE_WEX_ACCOUNTS_H */
