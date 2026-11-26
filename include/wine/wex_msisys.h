/*
 * WineExplorer: msi's SYSTEM custom action host in session 0, the protocol
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

#ifndef __WINE_WINE_WEX_MSISYS_H
#define __WINE_WINE_WEX_MSISYS_H

/* A deferred custom action without impersonation of an elevated per-machine install runs as SYSTEM in session 0
 * (WineExplorer's docs/MSI_NOIMPERSONATE_SYSTEM.md, option B). The elevated msiexec opens the session 0 service
 * WexMsiServer over the pipe bridge and sends one request; the service checks the caller on that connection, answers
 * with a status and, when it is 0, starts the host (msiexec -WexSysHost <handle>, SYSTEM in session 0) and hands it
 * the connection. From then on both sides exchange frames: a header and its payload. */

#define WEX_MSISYS_PIPE_LOCAL   L"\\\\.\\pipe\\wexmsiserver"
#define WEX_MSISYS_PIPE         L"\\\\.\\pipe\\WexSession0\\wexmsiserver"

#define WEX_MSISYS_MAGIC        0x5359534d  /* "MSYS" */
#define WEX_MSISYS_VERSION      1

#define WEX_MSISYS_PER_MACHINE  0x0001      /* the install's context is per machine (the client's statement) */

struct wex_msisys_request
{
    UINT32 magic;
    UINT32 version;
    UINT32 arch;            /* 32 or 64: the host's bitness */
    UINT32 flags;           /* WEX_MSISYS_* */
    WCHAR  product[40];     /* the ProductCode, for the events */
    WCHAR  package[260];    /* the package's path, for the events */
};

/* what the service tells the host about the install it serves, for the host's event after each action: environment
 * variables of the host process (the service sets them; the client never does) */
#define WEX_MSISYS_ENV_USER     L"WEXMSISYS_USER"      /* the caller's SID */
#define WEX_MSISYS_ENV_SESSION  L"WEXMSISYS_SESSION"   /* the caller's session */
#define WEX_MSISYS_ENV_PRODUCT  L"WEXMSISYS_PRODUCT"
#define WEX_MSISYS_ENV_PACKAGE  L"WEXMSISYS_PACKAGE"

/* the events, source WexMsiServer in the System log */
#define WEX_MSISYS_EVENT_SOURCE  L"WexMsiServer"
#define WEX_MSISYS_EVENT_HOST    1  /* a host started: package, product, user, session, host pid */
#define WEX_MSISYS_EVENT_REFUSED 2  /* a request refused: package, product, user, session, reason */
#define WEX_MSISYS_EVENT_ACTION  3  /* an action ran as SYSTEM: action, status, code, package, product, user, session */

/* the service's answer: one UINT32, 0 or a Win32 error (ERROR_ACCESS_DENIED: the caller is not an elevated
 * administrator, or the install is per user) */

/* frames */
enum wex_msisys_type
{
    WEX_MSISYS_READY = 1,   /* host -> client: struct wex_msisys_ready */
    WEX_MSISYS_RUN,         /* client -> host: struct wex_msisys_run, then the entry or arguments, then the binary */
    WEX_MSISYS_DONE,        /* host -> client: struct wex_msisys_done */
    WEX_MSISYS_OPEN,        /* host -> client: the action opened RPC channel `channel` (no payload) */
    WEX_MSISYS_DATA,        /* both ways: one RPC PDU of channel `channel` */
    WEX_MSISYS_CLOSE,       /* both ways: channel `channel` is closed (no payload) */
    WEX_MSISYS_END,         /* client -> host: no more actions; the host removes its folder and exits */
};

#define WEX_MSISYS_MAX_FRAME    (512u << 20)  /* a binary of up to 512 MB */

struct wex_msisys_frame
{
    UINT32 type;
    UINT32 channel;
    UINT32 size;            /* of the payload */
};

struct wex_msisys_ready
{
    UINT32 pid;             /* the host's process id, for the log */
};

#define WEX_MSISYS_KIND_DLL     1   /* entry: the exported function's name (ANSI, with its 0) */
#define WEX_MSISYS_KIND_EXE     2   /* entry: the arguments (WCHAR, with their 0) */

#define WEX_MSISYS_RUN_ASYNC    0x0001  /* EXE: do not wait for the process (msidbCustomActionTypeAsync) */

struct wex_msisys_run
{
    GUID   guid;            /* the action's (DLL: what the host asks remote_GetActionInfo for) */
    UINT32 kind;            /* WEX_MSISYS_KIND_* */
    UINT32 flags;           /* WEX_MSISYS_RUN_* */
    UINT32 entry_size;      /* bytes */
    UINT32 action_size;     /* bytes: the action's name (WCHAR, with its 0), for the event; after the entry */
    UINT32 binary_size;     /* bytes; after the action's name */
};

struct wex_msisys_done
{
    GUID   guid;
    UINT32 status;          /* 0, or the Win32 error that kept the action from running */
    UINT32 code;            /* DLL: the entry point's return; EXE: the process's exit code */
};

#endif /* __WINE_WINE_WEX_MSISYS_H */
