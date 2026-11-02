/*
 * WineExplorer Desktop Environment - dwm_channel.c
 *
 * Conformance tests for the per-desktop DWM broker control channel
 * (Phase P1, Slice 1 of docs/DWM_ARCHITECTURE.md).
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
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "wine/dwm_protocol.h"
#include "wine/test.h"

/*
 * The DWM control channel is a Wine-only private mechanism reached through the
 * private ProcessWineDwmChannel process information class (NtSetInformationProcess)
 * and the private WexDwmpChannelEcho dwmapi export. On real Windows neither exists,
 * so the whole test skips cleanly there (missing export, or the information class
 * failing with STATUS_INVALID_INFO_CLASS).
 *
 * Not testable from user mode: the server stamps sender_pid/sender_tid/desktop_id/
 * request_id/connection_epoch/capabilities onto every message and never trusts the
 * caller-supplied values for those fields. The wire carries no identity fields a
 * client could set, so identity cannot be spoofed; there is no user-mode API to
 * observe the stamped values on the receiving side, so this property cannot be
 * exercised directly.
 */

static HRESULT (WINAPI *pWexDwmpChannelEcho)(const void *in, ULONG in_len, void *out, ULONG *out_len);

static HANDLE broker_handle, broker_thread, broker_stop_event;
static uint64_t broker_epoch;
static HDESK switch_desktop, test_desktop;

static void use_test_desktop(void)
{
    if (test_desktop) SetThreadDesktop( test_desktop );
}
static uint64_t switch_epoch;
static NTSTATUS switch_status;

static NTSTATUS dwm_channel_call( struct dwm_channel_call *call )
{
    return NtSetInformationProcess( NtCurrentProcess(), ProcessWineDwmChannel, call, sizeof(*call) );
}

static void build_echo_message( unsigned char *buf, uint32_t len, const void *payload )
{
    struct dwm_msg_header *hdr = (struct dwm_msg_header *)buf;
    memset( buf, 0, sizeof(*hdr) + len );
    hdr->opcode  = DWM_OP_ECHO;
    hdr->version = DWM_PROTOCOL_VERSION;
    hdr->length  = len;
    if (len && payload) memcpy( buf + sizeof(*hdr), payload, len );
}

/* Broker loop thread: receive requests and reply with the same payload. */
static DWORD WINAPI broker_thread_fn( LPVOID arg )
{
    unsigned char buf[sizeof(struct dwm_msg_header) + DWM_MAX_MESSAGE_SIZE];
    struct dwm_channel_call call;
    NTSTATUS status;

    use_test_desktop();

    for (;;)
    {
        if (WaitForSingleObject( broker_stop_event, 0 ) == WAIT_OBJECT_0) break;
        memset( &call, 0, sizeof(call) );
        call.op      = DWM_CHANNEL_OP_RECV;
        call.handle  = HandleToUlong( broker_handle );
        call.out_ptr = (uintptr_t)buf;
        call.out_len = sizeof(buf);
        status = dwm_channel_call( &call );
        if (status == STATUS_PENDING)
        {
            HANDLE objs[2] = { broker_handle, broker_stop_event };
            if (WaitForMultipleObjects( 2, objs, FALSE, 100 ) == WAIT_OBJECT_0 + 1) break;
            continue;
        }
        if (status != STATUS_SUCCESS) break;  /* broker disconnected or error */
        {
            uint32_t id = call.request_id, size = call.out_len;
            memset( &call, 0, sizeof(call) );
            call.op         = DWM_CHANNEL_OP_REPLY;
            call.handle     = HandleToUlong( broker_handle );
            call.request_id = id;
            call.status     = STATUS_SUCCESS;
            call.in_ptr     = (uintptr_t)buf;
            call.in_len     = size;
            dwm_channel_call( &call );
        }
    }
    return 0;
}

static void start_broker_thread(void)
{
    DWORD tid;
    broker_thread = CreateThread( NULL, 0, broker_thread_fn, NULL, 0, &tid );
}

static void stop_broker_thread(void)
{
    if (!broker_thread) return;
    SetEvent( broker_stop_event );
    WaitForSingleObject( broker_thread, INFINITE );
    CloseHandle( broker_thread );
    broker_thread = NULL;
    ResetEvent( broker_stop_event );
}

/* (1) No broker registered: echo fails with the server's no-broker status. */
static void test_no_broker(void)
{
    unsigned char payload[16], reply[16];
    ULONG reply_len = sizeof(reply);
    HRESULT hr;

    memset( payload, 0xa5, sizeof(payload) );
    hr = pWexDwmpChannelEcho( payload, sizeof(payload), reply, &reply_len );
    ok( hr == HRESULT_FROM_NT( STATUS_PORT_DISCONNECTED ),
        "expected no-broker echo to fail with STATUS_PORT_DISCONNECTED, got hr %#lx\n", hr );
}

/* (2) Obtain the broker capability and register as broker. */
static void test_broker(void)
{
    struct dwm_channel_call call;
    uint64_t capability;
    NTSTATUS status;

    GetDesktopWindow();  /* ensure the desktop window (top-window owner) exists */

    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_GET_CAPABILITY;
    status = dwm_channel_call( &call );
    if (status == STATUS_ACCESS_DENIED)
    {
        /* Test process does not own the desktop top window (managed desktop). */
        trace( "test process does not own the desktop top window\n" );
        memset( &call, 0, sizeof(call) );
        call.op         = DWM_CHANNEL_OP_BROKER_REGISTER;
        call.capability = 0xdeadbeefcafebabeULL;
        status = dwm_channel_call( &call );
        ok( status == STATUS_ACCESS_DENIED,
            "expected bogus broker register to be refused, got %08lx\n", status );
        skip( "cannot obtain broker capability (not the desktop top-window owner); "
              "skipping broker-dependent tests\n" );
        return;
    }
    if (status != STATUS_SUCCESS)
    {
        skip( "failed to mint broker capability (status %08lx); skipping broker tests\n", status );
        return;
    }
    capability = call.capability;
    ok( capability != 0, "expected a non-zero capability token\n" );

    memset( &call, 0, sizeof(call) );
    call.op         = DWM_CHANNEL_OP_BROKER_REGISTER;
    call.capability = capability;
    status = dwm_channel_call( &call );
    ok( status == STATUS_SUCCESS, "expected broker register to succeed, got %08lx\n", status );
    if (status != STATUS_SUCCESS)
    {
        skip( "failed to register broker (status %08lx); skipping broker tests\n", status );
        return;
    }
    broker_handle = UlongToHandle( call.handle );
    broker_epoch  = call.epoch;
    ok( broker_handle != NULL, "expected a broker channel handle\n" );
    start_broker_thread();
}

/* (3) Echo round trips for various payload sizes. */
static void test_echo_sizes(void)
{
    static const uint32_t sizes[] = { 0, 1, 100, DWM_MAX_MESSAGE_SIZE };
    unsigned char payload[DWM_MAX_MESSAGE_SIZE + 1], reply[DWM_MAX_MESSAGE_SIZE + 1];
    ULONG reply_len;
    HRESULT hr;
    unsigned int i, j;

    Sleep( 100 );  /* clear the client helper's brief failure cache from test (1) */

    for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++)
    {
        uint32_t size = sizes[i];
        for (j = 0; j < size; j++) payload[j] = (unsigned char)(j * 7 + i);
        reply_len = sizeof(reply);
        hr = pWexDwmpChannelEcho( size ? payload : NULL, size, reply, &reply_len );
        ok( hr == S_OK, "echo size %u failed: hr %#lx\n", size, hr );
        ok( reply_len == size, "echo size %u: expected reply_len %u, got %lu\n", size, size, reply_len );
        if (size) ok( !memcmp( payload, reply, size ), "echo size %u: payload mismatch\n", size );
    }

    for (i = 0; i <= DWM_MAX_MESSAGE_SIZE; i++) payload[i] = (unsigned char)i;
    reply_len = sizeof(reply);
    hr = pWexDwmpChannelEcho( payload, DWM_MAX_MESSAGE_SIZE + 1, reply, &reply_len );
    ok( hr == HRESULT_FROM_NT( STATUS_PORT_MESSAGE_TOO_LONG ),
        "expected max+1 echo to fail with STATUS_PORT_MESSAGE_TOO_LONG, got hr %#lx\n", hr );
}

/* (4) Concurrent echoes from multiple threads. */
#define CONCURRENT_THREADS 4
#define CONCURRENT_ECHOES  100

struct echo_thread_arg { DWORD thread_id; int failures; };

static DWORD WINAPI echo_thread_fn( LPVOID arg )
{
    struct echo_thread_arg *targ = arg;
    unsigned char payload[3], reply[3];
    ULONG reply_len;
    HRESULT hr;
    int i;

    use_test_desktop();

    for (i = 0; i < CONCURRENT_ECHOES; i++)
    {
        payload[0] = (unsigned char)targ->thread_id;
        payload[1] = (unsigned char)i;
        payload[2] = (unsigned char)(i * 31 + targ->thread_id);
        reply_len = sizeof(reply);
        hr = pWexDwmpChannelEcho( payload, sizeof(payload), reply, &reply_len );
        if (hr != S_OK || reply_len != sizeof(payload) || memcmp( payload, reply, sizeof(payload) ))
            targ->failures++;
    }
    return 0;
}

static void test_concurrent(void)
{
    HANDLE threads[CONCURRENT_THREADS];
    struct echo_thread_arg args[CONCURRENT_THREADS];
    DWORD total = 0;
    int i;

    for (i = 0; i < CONCURRENT_THREADS; i++)
    {
        args[i].thread_id = i;
        args[i].failures  = 0;
        threads[i] = CreateThread( NULL, 0, echo_thread_fn, &args[i], 0, NULL );
        ok( threads[i] != NULL, "failed to create echo thread %d\n", i );
    }
    WaitForMultipleObjects( CONCURRENT_THREADS, threads, TRUE, INFINITE );
    for (i = 0; i < CONCURRENT_THREADS; i++) { total += args[i].failures; CloseHandle( threads[i] ); }
    ok( total == 0, "expected 0 concurrent echo failures, got %lu\n", total );
}

/* (5) Epoch increases after broker unregister + re-register; stale channel fails. */
static void test_epoch(void)
{
    struct dwm_channel_call call;
    unsigned char msg[sizeof(struct dwm_msg_header) + 1];
    uint64_t stale_epoch, capability;
    HANDLE stale_handle;
    NTSTATUS status;

    stop_broker_thread();

    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_CONNECT;
    status = dwm_channel_call( &call );
    ok( status == STATUS_SUCCESS, "expected connect to succeed, got %08lx\n", status );
    if (status != STATUS_SUCCESS) { start_broker_thread(); return; }
    stale_handle = UlongToHandle( call.handle );
    stale_epoch  = call.epoch;

    NtClose( broker_handle );  /* unregister the broker; advances the desktop epoch */
    broker_handle = NULL;

    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_GET_CAPABILITY;
    status = dwm_channel_call( &call );
    ok( status == STATUS_SUCCESS, "expected capability mint to succeed, got %08lx\n", status );
    if (status != STATUS_SUCCESS) return;
    capability = call.capability;

    memset( &call, 0, sizeof(call) );
    call.op         = DWM_CHANNEL_OP_BROKER_REGISTER;
    call.capability = capability;
    status = dwm_channel_call( &call );
    ok( status == STATUS_SUCCESS, "expected re-register to succeed, got %08lx\n", status );
    if (status != STATUS_SUCCESS) return;
    broker_handle = UlongToHandle( call.handle );
    broker_epoch  = call.epoch;

    ok( broker_epoch > stale_epoch, "expected epoch to increase (stale %llu, new %llu)\n",
        (unsigned long long)stale_epoch, (unsigned long long)broker_epoch );

    build_echo_message( msg, 1, "\x42" );
    memset( &call, 0, sizeof(call) );
    call.op     = DWM_CHANNEL_OP_SEND;
    call.handle = HandleToUlong( stale_handle );
    call.in_ptr = (uintptr_t)msg;
    call.in_len = sizeof(msg);
    status = dwm_channel_call( &call );
    ok( status == STATUS_PORT_DISCONNECTED,
        "expected stale channel send to fail with STATUS_PORT_DISCONNECTED, got %08lx\n", status );
    NtClose( stale_handle );

    start_broker_thread();
}

/* (6) Queue quota: with a non-draining broker, >64 sends fail with STATUS_QUOTA_EXCEEDED. */
static void test_queue_quota(void)
{
    struct dwm_channel_call call;
    unsigned char msg[sizeof(struct dwm_msg_header) + 1];
    HANDLE quota_handle;
    NTSTATUS status, last = 0;
    int i;

    stop_broker_thread();  /* broker registered but not draining the queue */

    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_CONNECT;
    status = dwm_channel_call( &call );
    ok( status == STATUS_SUCCESS, "expected connect to succeed, got %08lx\n", status );
    if (status != STATUS_SUCCESS) { start_broker_thread(); return; }
    quota_handle = UlongToHandle( call.handle );

    build_echo_message( msg, 1, "\x42" );
    for (i = 0; i <= DWM_MAX_QUEUE_DEPTH; i++)
    {
        memset( &call, 0, sizeof(call) );
        call.op     = DWM_CHANNEL_OP_SEND;
        call.handle = HandleToUlong( quota_handle );
        call.in_ptr = (uintptr_t)msg;
        call.in_len = sizeof(msg);
        status = dwm_channel_call( &call );
        if (i < DWM_MAX_QUEUE_DEPTH)
            ok( status == STATUS_SUCCESS, "send %d failed: %08lx\n", i, status );
        else
            last = status;
    }
    ok( last == STATUS_QUOTA_EXCEEDED,
        "expected send #%d to fail with STATUS_QUOTA_EXCEEDED, got %08lx\n",
        DWM_MAX_QUEUE_DEPTH + 1, last );

    NtClose( quota_handle );  /* aborts the queued messages */
    start_broker_thread();
}

/* (7) A thread that switched desktops connects to the new desktop. */
static DWORD WINAPI desktop_switch_thread_fn( LPVOID arg )
{
    struct dwm_channel_call call;

    if (!SetThreadDesktop( switch_desktop )) { switch_status = STATUS_ACCESS_DENIED; return 1; }
    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_CONNECT;
    switch_status = dwm_channel_call( &call );
    if (switch_status == STATUS_SUCCESS)
    {
        switch_epoch = call.epoch;
        NtClose( UlongToHandle( call.handle ) );
    }
    return 0;
}

static void test_desktop_switch(void)
{
    struct dwm_channel_call call;
    HANDLE thread;
    uint64_t default_epoch;
    NTSTATUS status;

    switch_desktop = CreateDesktopW( L"dwm_test_desktop", NULL, NULL, 0, GENERIC_ALL, NULL );
    if (!switch_desktop)
    {
        win_skip( "CreateDesktopW failed (error %lu); skipping desktop switch test\n", GetLastError() );
        return;
    }

    switch_epoch = 0;
    switch_status = STATUS_UNSUCCESSFUL;
    thread = CreateThread( NULL, 0, desktop_switch_thread_fn, NULL, 0, NULL );
    ok( thread != NULL, "failed to create thread\n" );
    if (thread) { WaitForSingleObject( thread, INFINITE ); CloseHandle( thread ); }
    ok( switch_status == STATUS_SUCCESS, "expected connect on new desktop to succeed, got %08lx\n", switch_status );

    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_CONNECT;
    status = dwm_channel_call( &call );
    ok( status == STATUS_SUCCESS, "expected connect on default desktop to succeed, got %08lx\n", status );
    if (status == STATUS_SUCCESS)
    {
        default_epoch = call.epoch;
        NtClose( UlongToHandle( call.handle ) );
        ok( switch_epoch != default_epoch,
            "expected new desktop epoch (%llu) to differ from default (%llu)\n",
            (unsigned long long)switch_epoch, (unsigned long long)default_epoch );
    }

    CloseDesktop( switch_desktop );
    switch_desktop = NULL;
}

/* (8) Handle leak check: 50 connect/close cycles should not leak handles. */
static void test_handle_leak(void)
{
    struct dwm_channel_call call;
    ULONG before, after;
    int i;

    if (NtQueryInformationProcess( NtCurrentProcess(), ProcessHandleCount, &before, sizeof(before), NULL ))
        return;
    for (i = 0; i < 50; i++)
    {
        memset( &call, 0, sizeof(call) );
        call.op = DWM_CHANNEL_OP_CONNECT;
        if (dwm_channel_call( &call ) == STATUS_SUCCESS)
            NtClose( UlongToHandle( call.handle ) );
    }
    if (NtQueryInformationProcess( NtCurrentProcess(), ProcessHandleCount, &after, sizeof(after), NULL ))
        return;
    ok( after == before, "handle leak: before=%lu after=%lu (diff=%ld)\n",
        before, after, (long)after - (long)before );
}

START_TEST(dwm_channel)
{
    HMODULE module;
    HWINSTA station;
    struct dwm_channel_call call;
    NTSTATUS status;

    module = GetModuleHandleW( L"dwmapi.dll" );
    if (!module) module = LoadLibraryW( L"dwmapi.dll" );
    pWexDwmpChannelEcho = (HRESULT (WINAPI *)(const void *, ULONG, void *, ULONG *))
        (void *)GetProcAddress( module, "WexDwmpChannelEcho" );
    if (!pWexDwmpChannelEcho)
    {
        win_skip( "WexDwmpChannelEcho not available (not a Wine build).\n" );
        return;
    }

    /* Verify the private information class exists; real Windows returns
     * STATUS_INVALID_INFO_CLASS. */
    memset( &call, 0, sizeof(call) );
    call.op = DWM_CHANNEL_OP_CONNECT;
    status = NtSetInformationProcess( NtCurrentProcess(), ProcessWineDwmChannel, &call, sizeof(call) );
    if (status == STATUS_INVALID_INFO_CLASS)
    {
        win_skip( "ProcessWineDwmChannel not available (not a Wine build).\n" );
        return;
    }
    if (status == STATUS_SUCCESS) NtClose( UlongToHandle( call.handle ) );

    /* A process on the service window station creates and owns the top window of its desktops
     * itself (no explorer is started), so this process may mint the broker capability. */
    station = CreateWindowStationW( L"__wineservice_winstation", 0, GENERIC_ALL, NULL );
    if (station) SetProcessWindowStation( station );
    test_desktop = CreateDesktopW( L"dwm_test_owner", NULL, NULL, 0, GENERIC_ALL, NULL );
    if (test_desktop)
    {
        GUID guid = { 0xd3a1c5e0, 0x4b6f, 0x4c1e, { 0x9a, 0x2b, 0x7e, 0x11, 0x5f, 0x30, 0x01, 0x02 }};
        SetThreadDesktop( test_desktop );
        /* like explorer: create the desktop window ourselves, which makes this process the owner */
        CreateWindowExW( 0, (LPCWSTR)MAKEINTATOM(32769), NULL, WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                         0, 0, 0, 0, 0, 0, 0, &guid );
    }

    broker_stop_event = CreateEventW( NULL, TRUE, FALSE, NULL );

    test_no_broker();
    test_broker();
    if (!broker_handle) return;  /* could not register a broker; broker tests skipped */
    test_echo_sizes();
    test_concurrent();
    test_epoch();
    test_queue_quota();
    test_desktop_switch();
    test_handle_leak();

    stop_broker_thread();
    if (broker_handle) { NtClose( broker_handle ); broker_handle = NULL; }
    CloseHandle( broker_stop_event );
}
