/*
 * WineExplorer Desktop Environment - dwm_client.c
 *
 * Client-side DWM broker control channel helper and test hook.
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
#include "dwmapi.h"
#include "wine/dwm_protocol.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwmapi);

/* Brief failure cache window (ms) to avoid reconnect loops when no broker is registered. */
#define DWM_FAIL_CACHE_MS 50ULL

static CRITICAL_SECTION dwm_client_cs;
static CRITICAL_SECTION_DEBUG dwm_client_cs_debug =
{
    0, 0, &dwm_client_cs,
    { &dwm_client_cs_debug.ProcessLocksList, &dwm_client_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": dwm_client_cs") }
};
static CRITICAL_SECTION dwm_client_cs = { &dwm_client_cs_debug, -1, 0, 0, 0, 0 };

static HANDLE cached_handle;
static HDESK cached_desktop;
static uint64_t cached_epoch;

static HDESK fail_desktop;
static ULONGLONG fail_tick;
static NTSTATUS fail_status;

static void close_cached_channel(void)
{
    if (cached_handle)
    {
        NtClose( cached_handle );
        cached_handle = NULL;
    }
    cached_desktop = NULL;
    cached_epoch = 0;
}

static void record_failure( HDESK desktop, NTSTATUS status )
{
    fail_desktop = desktop;
    fail_tick = GetTickCount64();
    fail_status = status;
}

/***********************************************************************
 *           dwm_client_connect
 *
 * Open a waitable client channel bound to the calling thread's current desktop.
 * Never called from DllMain.
 */
NTSTATUS dwm_client_connect( HANDLE *ret_handle, uint64_t *ret_epoch )
{
    struct dwm_channel_call call;
    NTSTATUS status;

    if (!ret_handle) return STATUS_INVALID_PARAMETER;
    *ret_handle = NULL;
    if (ret_epoch) *ret_epoch = 0;

    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_CONNECT;

    status = NtSetInformationProcess( NtCurrentProcess(), ProcessWineDwmChannel, &call, sizeof(call) );
    if (status) return status;

    if (call.version != DWM_PROTOCOL_VERSION)
    {
        NtClose( UlongToHandle( call.handle ) );
        return STATUS_REVISION_MISMATCH;
    }

    *ret_handle = UlongToHandle( call.handle );
    if (ret_epoch) *ret_epoch = call.epoch;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           dwm_client_call
 *
 * Perform a bounded synchronous request/reply call to the current desktop's
 * DWM broker with brief failure caching when no broker is registered.
 */
NTSTATUS dwm_client_call( uint32_t opcode, const void *in, uint32_t in_len,
                          void *out, uint32_t *out_len, uint32_t timeout_ms )
{
    unsigned char req_buf[sizeof(struct dwm_msg_header) + DWM_MAX_MESSAGE_SIZE];
    unsigned char rep_buf[sizeof(struct dwm_msg_header) + DWM_MAX_MESSAGE_SIZE];
    struct dwm_msg_header hdr, rep_hdr;
    struct dwm_channel_call call;
    HDESK cur_desktop;
    BOOL retried = FALSE;
    NTSTATUS status;

    if (in_len > DWM_MAX_MESSAGE_SIZE) return STATUS_PORT_MESSAGE_TOO_LONG;
    if (in_len && !in) return STATUS_INVALID_PARAMETER;
    if (out_len && *out_len && !out) return STATUS_INVALID_PARAMETER;

    EnterCriticalSection( &dwm_client_cs );

    cur_desktop = GetThreadDesktop( GetCurrentThreadId() );

    /* If the calling thread switched desktops, drop the cached channel for the old desktop. */
    if (cached_handle && cached_desktop != cur_desktop)
        close_cached_channel();

    /* Brief failure cache so missing-broker calls do not spin in reconnect loops. */
    if (!cached_handle && fail_status && fail_desktop == cur_desktop)
    {
        if (GetTickCount64() - fail_tick < DWM_FAIL_CACHE_MS)
        {
            status = fail_status;
            LeaveCriticalSection( &dwm_client_cs );
            return status;
        }
        fail_status = STATUS_SUCCESS;
    }

retry_connect:
    if (!cached_handle)
    {
        status = dwm_client_connect( &cached_handle, &cached_epoch );
        if (status)
        {
            record_failure( cur_desktop, status );
            LeaveCriticalSection( &dwm_client_cs );
            return status;
        }
        cached_desktop = cur_desktop;
        retried = TRUE;
    }

    memset( &hdr, 0, sizeof(hdr) );
    hdr.opcode           = opcode;
    hdr.version          = DWM_PROTOCOL_VERSION;
    hdr.length           = in_len;
    hdr.request_id       = 0;
    hdr.connection_epoch = cached_epoch;
    hdr.window_token     = 0;
    hdr.revision         = 0;
    hdr.capabilities     = 0;

    memcpy( req_buf, &hdr, sizeof(hdr) );
    if (in_len) memcpy( req_buf + sizeof(hdr), in, in_len );

    memset( &call, 0, sizeof(call) );
    call.op     = DWM_CHANNEL_OP_SEND;
    call.handle = HandleToUlong( cached_handle );
    call.in_ptr = (uintptr_t)req_buf;
    call.in_len = sizeof(hdr) + in_len;

    status = NtSetInformationProcess( NtCurrentProcess(), ProcessWineDwmChannel, &call, sizeof(call) );
    if (status == STATUS_PORT_DISCONNECTED || status == STATUS_ACCESS_DENIED || status == STATUS_INVALID_HANDLE)
    {
        close_cached_channel();
        if (!retried && status == STATUS_PORT_DISCONNECTED)
        {
            retried = TRUE;
            goto retry_connect;
        }
        record_failure( cur_desktop, status );
        LeaveCriticalSection( &dwm_client_cs );
        return status;
    }
    if (status)
    {
        LeaveCriticalSection( &dwm_client_cs );
        return status;
    }

    cached_epoch = call.epoch;

    for (;;)
    {
        memset( &call, 0, sizeof(call) );
        call.op      = DWM_CHANNEL_OP_RECV;
        call.handle  = HandleToUlong( cached_handle );
        call.out_ptr = (uintptr_t)rep_buf;
        call.out_len = sizeof(rep_buf);

        status = NtSetInformationProcess( NtCurrentProcess(), ProcessWineDwmChannel, &call, sizeof(call) );
        if (status == STATUS_PENDING)
        {
            LARGE_INTEGER timeout, *ptimeout = NULL;
            NTSTATUS wait_status;

            if (timeout_ms != INFINITE)
            {
                timeout.QuadPart = -(LONGLONG)timeout_ms * 10000;
                ptimeout = &timeout;
            }

            wait_status = NtWaitForSingleObject( cached_handle, FALSE, ptimeout );
            if (wait_status != STATUS_WAIT_0)
            {
                close_cached_channel();
                status = (wait_status == STATUS_TIMEOUT) ? STATUS_IO_TIMEOUT : wait_status;
                LeaveCriticalSection( &dwm_client_cs );
                return status;
            }
            continue;
        }
        break;
    }

    if (status)
    {
        if (status == STATUS_PORT_DISCONNECTED)
        {
            close_cached_channel();
            record_failure( cur_desktop, status );
        }
        LeaveCriticalSection( &dwm_client_cs );
        return status;
    }

    fail_status = STATUS_SUCCESS;

    if (call.status)
    {
        status = (NTSTATUS)call.status;
        LeaveCriticalSection( &dwm_client_cs );
        return status;
    }

    if (call.out_len < sizeof(rep_hdr))
    {
        LeaveCriticalSection( &dwm_client_cs );
        return STATUS_INVALID_NETWORK_RESPONSE;
    }

    memcpy( &rep_hdr, rep_buf, sizeof(rep_hdr) );
    if (rep_hdr.version != DWM_PROTOCOL_VERSION ||
        rep_hdr.length != call.out_len - sizeof(rep_hdr))
    {
        LeaveCriticalSection( &dwm_client_cs );
        return STATUS_INVALID_NETWORK_RESPONSE;
    }

    if (out_len)
    {
        uint32_t avail = *out_len;
        *out_len = rep_hdr.length;
        if (avail < rep_hdr.length)
        {
            LeaveCriticalSection( &dwm_client_cs );
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (rep_hdr.length && out)
            memcpy( out, rep_buf + sizeof(rep_hdr), rep_hdr.length );
    }

    LeaveCriticalSection( &dwm_client_cs );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           WexDwmpChannelEcho   (DWMAPI.@)
 *
 * Private test hook performing an ECHO round-trip over the desktop DWM channel.
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
