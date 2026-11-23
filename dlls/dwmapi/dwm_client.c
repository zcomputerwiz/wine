/*
 * WineExplorer Desktop Environment - dwm_client.c
 *
 * The client end of the DWM broker's pipe (docs/DWM_ARCHITECTURE.md section 2.0) and the private exports over it.
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winuser.h"
#include "winreg.h"
#include "dwmapi.h"
#include "dwm_client.h"
#include "wine/wex_pipe.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwmapi);

/* Brief failure cache window (ms) to avoid reconnect loops when no broker serves the desktop. */
#define DWM_FAIL_CACHE_MS 50ULL
/* desktops this process talks to at once; a process with windows on more desktops reconnects */
#define DWM_CLIENT_LINKS  4

static CRITICAL_SECTION dwm_client_cs;
static CRITICAL_SECTION_DEBUG dwm_client_cs_debug =
{
    0, 0, &dwm_client_cs,
    { &dwm_client_cs_debug.ProcessLocksList, &dwm_client_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": dwm_client_cs") }
};
static CRITICAL_SECTION dwm_client_cs = { &dwm_client_cs_debug, -1, 0, 0, 0, 0 };

/* A connection to one desktop's broker, made when a call needs it. generation is the last broker generation seen
 * there: a new one means a broker that does not hold this process's attributes yet. */
struct broker_link
{
    HDESK desktop;
    HANDLE pipe;
    uint64_t generation;
    ULONGLONG fail_tick;
    NTSTATUS fail_status;
};

static struct broker_link links[DWM_CLIENT_LINKS];
static unsigned int next_evicted;
static HANDLE io_event;
static BOOL broker_changed;

/* The DwmService gate (HKCU\Software\WineExplorer\Shell), read once per process: without it no desktop starts a
 * broker, so the attribute and thumbnail calls do not ask for one. */
static BOOL broker_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0)
    {
        DWORD value = 0, size = sizeof(value);
        enabled = !RegGetValueW( HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"DwmService",
                                 RRF_RT_REG_DWORD, NULL, &value, &size ) && value;
    }
    return enabled;
}

static void close_link( struct broker_link *link )
{
    if (link->pipe) CloseHandle( link->pipe );
    link->pipe = NULL;
}

/* The broker to ask about a window: the desktop of the thread that owns it when that thread is this process's
 * (routing by window, not by the calling thread: a process may have windows on several desktops); otherwise the
 * calling thread's desktop. */
static HDESK call_desktop( HWND hwnd )
{
    DWORD pid = 0, tid = hwnd ? GetWindowThreadProcessId( hwnd, &pid ) : 0;
    HDESK desktop = NULL;

    if (tid && pid == GetCurrentProcessId()) desktop = GetThreadDesktop( tid );
    return desktop ? desktop : GetThreadDesktop( GetCurrentThreadId() );
}

static struct broker_link *get_link( HDESK desktop )
{
    struct broker_link *link;
    unsigned int i;

    for (i = 0; i < DWM_CLIENT_LINKS; i++) if (links[i].desktop == desktop) return &links[i];
    for (i = 0; i < DWM_CLIENT_LINKS; i++) if (!links[i].desktop) break;
    if (i == DWM_CLIENT_LINKS)
    {
        i = next_evicted++ % DWM_CLIENT_LINKS;
        close_link( &links[i] );
    }
    link = &links[i];
    memset( link, 0, sizeof(*link) );
    link->desktop = desktop;
    return link;
}

/* Open the desktop's pipe and check that dwm.exe serves it. A failed check counts as no broker. */
static NTSTATUS connect_link( struct broker_link *link )
{
    WCHAR name[MAX_PATH * 2 + 64];
    HANDLE pipe;

    if (!dwm_pipe_name( link->desktop, name, ARRAY_SIZE(name) )) return STATUS_PORT_DISCONNECTED;
    if ((pipe = wex_pipe_connect( name, 500 )) == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_PIPE_BUSY ? STATUS_IO_TIMEOUT : STATUS_PORT_DISCONNECTED;
    if (!wex_pipe_server_is( pipe, L"dwm.exe" ))
    {
        WARN( "%s is not served by dwm.exe with an ordinary token\n", debugstr_w( name ) );
        CloseHandle( pipe );
        return STATUS_PORT_DISCONNECTED;
    }
    link->pipe = pipe;
    return STATUS_SUCCESS;
}

static NTSTATUS pipe_status( DWORD err )
{
    switch (err)
    {
    case ERROR_SUCCESS: return STATUS_SUCCESS;
    case ERROR_TIMEOUT: return STATUS_IO_TIMEOUT;
    case ERROR_MORE_DATA: return STATUS_INVALID_NETWORK_RESPONSE;
    default: return STATUS_PORT_DISCONNECTED;   /* broken pipe, no data, not connected */
    }
}

/* one request and its reply; called with dwm_client_cs held */
static NTSTATUS call_locked( struct broker_link *link, uint32_t opcode, HWND hwnd, const void *in, uint32_t in_len,
                             void *out, uint32_t *out_len, uint32_t timeout_ms )
{
    unsigned char req_buf[sizeof(struct dwm_request) + DWM_MAX_MESSAGE_SIZE];
    unsigned char rep_buf[sizeof(struct dwm_reply) + DWM_MAX_MESSAGE_SIZE];
    struct dwm_request req;
    struct dwm_reply rep;
    BOOL retried = FALSE;
    DWORD size;
    NTSTATUS status;

    /* Brief failure cache so missing-broker calls do not spin in reconnect loops. */
    if (!link->pipe && link->fail_status)
    {
        if (GetTickCount64() - link->fail_tick < DWM_FAIL_CACHE_MS) return link->fail_status;
        link->fail_status = STATUS_SUCCESS;
    }
    if (!io_event && !(io_event = CreateEventW( NULL, TRUE, FALSE, NULL ))) return STATUS_NO_MEMORY;

    req.opcode  = opcode;
    req.length  = in_len;
    req.window  = HandleToUlong( hwnd );
    req.version = DWM_PROTOCOL_VERSION;
    memcpy( req_buf, &req, sizeof(req) );
    if (in_len) memcpy( req_buf + sizeof(req), in, in_len );

    for (;;)
    {
        if (!link->pipe && (status = connect_link( link )))
        {
            link->fail_tick = GetTickCount64();
            link->fail_status = status;
            return status;
        }
        status = pipe_status( wex_pipe_transact( link->pipe, req_buf, sizeof(req) + in_len, rep_buf, sizeof(rep_buf),
                                                 &size, io_event, timeout_ms ) );
        if (!status) break;
        close_link( link );
        /* a broker that went away is replaced by the desktop manager: one new connection */
        if (status != STATUS_PORT_DISCONNECTED || retried) return status;
        retried = TRUE;
    }

    if (size < sizeof(rep)) return STATUS_INVALID_NETWORK_RESPONSE;
    memcpy( &rep, rep_buf, sizeof(rep) );
    if (rep.length != size - sizeof(rep)) return STATUS_INVALID_NETWORK_RESPONSE;
    if (rep.generation != link->generation)
    {
        TRACE( "broker generation %I64x on desktop %p\n", (UINT64)rep.generation, link->desktop );
        link->generation = rep.generation;
        broker_changed = TRUE;
    }
    if (rep.status) return rep.status;

    if (out_len)
    {
        uint32_t avail = *out_len;
        *out_len = rep.length;
        if (avail < rep.length) return STATUS_BUFFER_TOO_SMALL;
        if (rep.length && out) memcpy( out, rep_buf + sizeof(rep), rep.length );
    }
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           dwm_client_call_window
 *
 * A bounded synchronous call to the broker of the window's desktop (the calling thread's for no window), with
 * brief failure caching when no broker serves it. The first call that reaches a new broker hands it this process's
 * attributes and thumbnails.
 */
static NTSTATUS dwm_client_call_window( uint32_t opcode, HWND hwnd, const void *in, uint32_t in_len,
                                        void *out, uint32_t *out_len, uint32_t timeout_ms )
{
    HDESK desktop;
    NTSTATUS status;
    BOOL resend;

    if (in_len > DWM_MAX_MESSAGE_SIZE) return STATUS_PORT_MESSAGE_TOO_LONG;
    if (in_len && !in) return STATUS_INVALID_PARAMETER;
    if (out_len && *out_len && !out) return STATUS_INVALID_PARAMETER;
    if (!(desktop = call_desktop( hwnd ))) return STATUS_PORT_DISCONNECTED;

    EnterCriticalSection( &dwm_client_cs );
    status = call_locked( get_link( desktop ), opcode, hwnd, in, in_len, out, out_len, timeout_ms );
    resend = broker_changed;
    broker_changed = FALSE;
    LeaveCriticalSection( &dwm_client_cs );

    /* outside the lock: the tables' lock is taken before this one */
    if (resend) dwm_resend_state();
    return status;
}

static NTSTATUS dwm_client_call( uint32_t opcode, const void *in, uint32_t in_len,
                                 void *out, uint32_t *out_len, uint32_t timeout_ms )
{
    return dwm_client_call_window( opcode, NULL, in, in_len, out, out_len, timeout_ms );
}

/* Attribute state held by the broker. Returns the call status; STATUS_PORT_DISCONNECTED etc. mean no broker. */
NTSTATUS dwm_client_set_attr( HWND hwnd, DWORD attribute, DWORD value )
{
    struct dwm_attr_set req = { attribute, value };
    struct dwm_attr_reply rep;
    uint32_t len = sizeof(rep);
    NTSTATUS status;

    if (!broker_enabled()) return STATUS_PORT_DISCONNECTED;
    status = dwm_client_call_window( DWM_OP_SET_ATTR, hwnd, &req, sizeof(req), &rep, &len, 2000 );
    if (!status && len == sizeof(rep) && !rep.found) status = STATUS_UNSUCCESSFUL;
    return status;
}

/* *found tells whether the broker holds a value for the attribute */
NTSTATUS dwm_client_get_attr( HWND hwnd, DWORD attribute, BOOL *found, DWORD *value )
{
    struct dwm_attr_get req = { attribute };
    struct dwm_attr_reply rep;
    uint32_t len = sizeof(rep);
    NTSTATUS status;

    if (!broker_enabled()) return STATUS_PORT_DISCONNECTED;
    status = dwm_client_call_window( DWM_OP_GET_ATTR, hwnd, &req, sizeof(req), &rep, &len, 2000 );
    if (status) return status;
    if (len != sizeof(rep)) return STATUS_INVALID_NETWORK_RESPONSE;
    *found = rep.found != 0;
    *value = rep.value;
    return STATUS_SUCCESS;
}

/* live thumbnails (P8): register or update a thumbnail on the broker; best effort, fails quietly without one */
NTSTATUS dwm_client_thumb_set( HWND dest, UINT handle, HWND src, const struct dwm_thumb_set *set )
{
    struct dwm_thumb_set req;

    if (!set) return STATUS_INVALID_PARAMETER;
    if (!broker_enabled()) return STATUS_PORT_DISCONNECTED;
    req = *set;
    req.handle = handle;
    req.src    = HandleToUlong( src );
    return dwm_client_call_window( DWM_OP_THUMB_SET, dest, &req, sizeof(req), NULL, NULL, 2000 );
}

NTSTATUS dwm_client_thumb_del( HWND dest, UINT handle )
{
    struct dwm_thumb_del del = { handle, 0 };

    if (!broker_enabled()) return STATUS_PORT_DISCONNECTED;
    return dwm_client_call_window( DWM_OP_THUMB_DEL, dest, &del, sizeof(del), NULL, NULL, 2000 );
}

/***********************************************************************
 *           WexDwmpChannelEcho   (DWMAPI.@)
 *
 * Private test hook: an ECHO round trip to the calling thread's desktop's broker.
 */
HRESULT WINAPI WexDwmpChannelEcho( const void *in, ULONG in_len, void *out, ULONG *out_len )
{
    uint32_t len = out_len ? *out_len : 0;
    NTSTATUS status;

    TRACE( "(%p, %lu, %p, %p)\n", in, in_len, out, out_len );

    if (in_len && !in) return E_INVALIDARG;
    if (out_len && *out_len && !out) return E_INVALIDARG;

    status = dwm_client_call( DWM_OP_ECHO, in, in_len, out, out_len ? &len : NULL, 5000 );
    if (out_len) *out_len = len;
    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

/***********************************************************************
 *           WexDwmpPoll   (DWMAPI.@)
 *
 * Private: the compositor bridge asks the broker for the attribute changes after a revision.
 * out receives struct dwm_poll_reply followed by its struct dwm_change records.
 */
HRESULT WINAPI WexDwmpPoll( UINT64 since_revision, DWORD cursor, void *out, DWORD *out_len )
{
    struct dwm_poll_req req = { since_revision, cursor, 0 };
    uint32_t len;
    NTSTATUS status;

    if (!out || !out_len) return E_INVALIDARG;
    len = *out_len;
    status = dwm_client_call( DWM_OP_POLL, &req, sizeof(req), out, &len, 2000 );
    *out_len = len;
    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

/***********************************************************************
 *           WexDwmpGetAttr   (DWMAPI.@)
 *
 * Private: the shell reads a window's attribute as the broker holds it, whichever process set it
 * (DWMWA_HAS_ICONIC_BITMAP for P9). S_FALSE when the broker holds no value.
 */
HRESULT WINAPI WexDwmpGetAttr( HWND hwnd, DWORD attribute, DWORD *value )
{
    BOOL found = FALSE;
    NTSTATUS status;

    if (!value) return E_INVALIDARG;
    if ((status = dwm_client_get_attr( hwnd, attribute, &found, value ))) return HRESULT_FROM_NT( status );
    return found ? S_OK : S_FALSE;
}

/***********************************************************************
 *           WexDwmpThumbList   (DWMAPI.@)
 *
 * Private: the compositor bridge asks the broker for the live thumbnails after a revision.
 * out receives struct dwm_thumb_list_reply followed by its struct dwm_thumb records.
 */
HRESULT WINAPI WexDwmpThumbList( UINT64 since_revision, void *out, DWORD *out_len )
{
    struct dwm_thumb_list_req req = { since_revision };
    uint32_t len;
    NTSTATUS status;

    if (!out || !out_len) return E_INVALIDARG;
    len = *out_len;
    status = dwm_client_call( DWM_OP_THUMB_LIST, &req, sizeof(req), out, &len, 2000 );
    *out_len = len;
    return status ? HRESULT_FROM_NT( status ) : S_OK;
}
