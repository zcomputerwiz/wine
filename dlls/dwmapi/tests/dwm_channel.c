/*
 * WineExplorer Desktop Environment - dwm_channel.c
 *
 * Conformance tests for the per-desktop DWM broker pipe (docs/DWM_ARCHITECTURE.md section 2.0): the transport, the
 * server check and the broker's per-message checks, talking to the desktop's real dwm.exe. They need a desktop with
 * a broker (DwmService=1 on a shell desktop) and skip without one; on Windows the private export is missing.
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
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "dwmapi.h"
#include "wine/dwm_protocol.h"
#include "wine/wex_pipe.h"
#include "wine/test.h"

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

static HRESULT (WINAPI *pWexDwmpChannelEcho)(const void *in, ULONG in_len, void *out, ULONG *out_len);

/* a client end of this desktop's broker pipe, opened by hand: the test plays a client that does not use dwmapi */
static HANDLE open_broker(void)
{
    WCHAR name[MAX_PATH * 2 + 64];

    if (!dwm_pipe_name( GetThreadDesktop( GetCurrentThreadId() ), name, ARRAY_SIZE(name) ))
        return INVALID_HANDLE_VALUE;
    return wex_pipe_connect( name, 2000 );
}

/* one raw request; returns the broker's status, or 0xffffffff when the transport failed */
static NTSTATUS raw_call( HANDLE pipe, uint32_t opcode, uint32_t window, uint32_t version, const void *in,
                          uint32_t in_len, void *out, uint32_t out_max, uint32_t *out_len, uint64_t *generation )
{
    unsigned char req_buf[sizeof(struct dwm_request) + DWM_MAX_MESSAGE_SIZE + 64];
    unsigned char rep_buf[sizeof(struct dwm_reply) + DWM_MAX_MESSAGE_SIZE];
    struct dwm_request req = { opcode, in_len, window, version };
    struct dwm_reply rep;
    HANDLE event = CreateEventW( NULL, TRUE, FALSE, NULL );
    DWORD size, err;

    memcpy( req_buf, &req, sizeof(req) );
    if (in_len) memcpy( req_buf + sizeof(req), in, in_len );
    err = wex_pipe_transact( pipe, req_buf, sizeof(req) + in_len, rep_buf, sizeof(rep_buf), &size, event, 5000 );
    CloseHandle( event );
    if (err || size < sizeof(rep)) return 0xffffffff;
    memcpy( &rep, rep_buf, sizeof(rep) );
    ok( rep.length == size - sizeof(rep), "reply length %u, message %lu\n", rep.length, size );
    if (generation) *generation = rep.generation;
    if (out_len) *out_len = rep.length;
    if (out && rep.length <= out_max) memcpy( out, rep_buf + sizeof(rep), rep.length );
    return rep.status;
}

static void test_echo_sizes(void)
{
    static const ULONG sizes[] = { 0, 1, 100, DWM_MAX_MESSAGE_SIZE };
    unsigned char *payload = malloc( DWM_MAX_MESSAGE_SIZE + 1 ), *reply = malloc( DWM_MAX_MESSAGE_SIZE );
    ULONG reply_len;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < DWM_MAX_MESSAGE_SIZE + 1; i++) payload[i] = (unsigned char)i;
    for (i = 0; i < ARRAY_SIZE(sizes); i++)
    {
        reply_len = DWM_MAX_MESSAGE_SIZE;
        memset( reply, 0, DWM_MAX_MESSAGE_SIZE );
        hr = pWexDwmpChannelEcho( sizes[i] ? payload : NULL, sizes[i], reply, &reply_len );
        ok( hr == S_OK, "size %lu: hr %#lx\n", sizes[i], hr );
        ok( reply_len == sizes[i], "size %lu: reply %lu\n", sizes[i], reply_len );
        ok( !memcmp( reply, payload, sizes[i] ), "size %lu: payload differs\n", sizes[i] );
    }
    reply_len = DWM_MAX_MESSAGE_SIZE;
    hr = pWexDwmpChannelEcho( payload, DWM_MAX_MESSAGE_SIZE + 1, reply, &reply_len );
    ok( hr == HRESULT_FROM_NT( STATUS_PORT_MESSAGE_TOO_LONG ), "max+1: hr %#lx\n", hr );
    free( payload );
    free( reply );
}

static DWORD WINAPI echo_thread( void *arg )
{
    unsigned char payload[100], reply[100];
    unsigned int i, failures = 0;
    ULONG len;

    memset( payload, (int)(INT_PTR)arg, sizeof(payload) );
    for (i = 0; i < 100; i++)
    {
        len = sizeof(reply);
        if (pWexDwmpChannelEcho( payload, sizeof(payload), reply, &len ) || len != sizeof(payload) ||
            memcmp( reply, payload, sizeof(payload) )) failures++;
    }
    return failures;
}

static void test_concurrent(void)
{
    HANDLE threads[4];
    DWORD failures;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(threads); i++) threads[i] = CreateThread( NULL, 0, echo_thread, (void *)(INT_PTR)(i + 1), 0, NULL );
    WaitForMultipleObjects( ARRAY_SIZE(threads), threads, TRUE, 30000 );
    for (i = 0; i < ARRAY_SIZE(threads); i++)
    {
        GetExitCodeThread( threads[i], &failures );
        ok( !failures, "thread %u: %lu failed echoes\n", i, failures );
        CloseHandle( threads[i] );
    }
}

/* the server is dwm.exe with an ordinary token; a pipe this test serves itself is not */
static void test_server_check(void)
{
    WCHAR name[64];
    HANDLE pipe, server, client;

    pipe = open_broker();
    ok( pipe != INVALID_HANDLE_VALUE, "no broker pipe: %lu\n", GetLastError() );
    if (pipe == INVALID_HANDLE_VALUE) return;
    ok( wex_pipe_server_is( pipe, L"dwm.exe" ), "the broker's server check failed\n" );
    ok( !wex_pipe_server_is( pipe, L"explorer.exe" ), "the broker passed as explorer.exe\n" );
    CloseHandle( pipe );

    swprintf( name, ARRAY_SIZE(name), L"\\\\.\\pipe\\WineExplorer\\DwmTest\\%lu", GetCurrentProcessId() );
    server = wex_pipe_create( name, NULL, TRUE, 256 );
    ok( server != INVALID_HANDLE_VALUE, "create failed %lu\n", GetLastError() );
    client = wex_pipe_connect( name, 1000 );
    ok( client != INVALID_HANDLE_VALUE, "connect failed %lu\n", GetLastError() );
    ok( !wex_pipe_server_is( client, L"dwm.exe" ), "an impostor passed the server check\n" );
    CloseHandle( client );
    CloseHandle( server );
}

struct other_desktop_window { HDESK desktop; HWND hwnd; HANDLE done, quit; };

static DWORD WINAPI other_desktop_thread( void *arg )
{
    struct other_desktop_window *w = arg;
    MSG msg;

    if (SetThreadDesktop( w->desktop ))
        w->hwnd = CreateWindowExW( 0, L"static", L"dwm_pipe_other", WS_POPUP, 0, 0, 10, 10, 0, 0, 0, NULL );
    SetEvent( w->done );
    while (MsgWaitForMultipleObjects( 1, &w->quit, FALSE, INFINITE, QS_ALLINPUT ) != WAIT_OBJECT_0)
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE )) DispatchMessageW( &msg );
    if (w->hwnd) DestroyWindow( w->hwnd );
    return 0;
}

/* the broker's per-message checks: the window must be on its desktop, and only its owner changes it */
static void test_window_checks(void)
{
    struct dwm_attr_set set = { DWMWA_CAPTION_COLOR, 0x00332211 };
    struct dwm_attr_get get = { DWMWA_CAPTION_COLOR };
    struct dwm_attr_reply rep;
    struct other_desktop_window other = { 0 };
    uint64_t generation1 = 0, generation2 = 0;
    unsigned char big[64];
    uint32_t len;
    HANDLE pipe, thread;
    NTSTATUS status;
    HWND hwnd, foreign;

    pipe = open_broker();
    if (pipe == INVALID_HANDLE_VALUE) return;
    hwnd = CreateWindowExW( 0, L"static", L"dwm_pipe_own", WS_POPUP, 0, 0, 10, 10, 0, 0, 0, NULL );

    status = raw_call( pipe, DWM_OP_SET_ATTR, HandleToUlong( hwnd ), DWM_PROTOCOL_VERSION, &set, sizeof(set),
                       &rep, sizeof(rep), &len, &generation1 );
    ok( !status, "own window: status %#lx\n", status );
    ok( generation1, "no generation\n" );
    status = raw_call( pipe, DWM_OP_GET_ATTR, HandleToUlong( hwnd ), DWM_PROTOCOL_VERSION, &get, sizeof(get),
                       &rep, sizeof(rep), &len, &generation2 );
    ok( !status && len == sizeof(rep) && rep.found && rep.value == set.value, "get: status %#lx found %u value %#x\n",
        status, rep.found, rep.value );
    ok( generation2 == generation1, "generation changed: %I64x, %I64x\n", generation1, generation2 );

    /* another process's window on this desktop: the shell's taskbar, which the broker lets this client read */
    if ((foreign = FindWindowW( L"Shell_TrayWnd", NULL )))
    {
        status = raw_call( pipe, DWM_OP_SET_ATTR, HandleToUlong( foreign ), DWM_PROTOCOL_VERSION, &set,
                           sizeof(set), &rep, sizeof(rep), &len, NULL );
        ok( status == STATUS_ACCESS_DENIED, "foreign window: status %#lx\n", status );
        status = raw_call( pipe, DWM_OP_GET_ATTR, HandleToUlong( foreign ), DWM_PROTOCOL_VERSION, &get,
                           sizeof(get), &rep, sizeof(rep), &len, NULL );
        ok( !status, "foreign window, read: status %#lx\n", status );
    }
    else skip( "no taskbar window\n" );

    /* this process's window on another desktop */
    other.desktop = CreateDesktopW( L"dwm_pipe_other", NULL, NULL, 0, GENERIC_ALL, NULL );
    other.done = CreateEventW( NULL, TRUE, FALSE, NULL );
    other.quit = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (other.desktop)
    {
        thread = CreateThread( NULL, 0, other_desktop_thread, &other, 0, NULL );
        WaitForSingleObject( other.done, 5000 );
        if (other.hwnd)
        {
            status = raw_call( pipe, DWM_OP_SET_ATTR, HandleToUlong( other.hwnd ), DWM_PROTOCOL_VERSION, &set,
                               sizeof(set), &rep, sizeof(rep), &len, NULL );
            ok( status == STATUS_ACCESS_DENIED, "other desktop's window: status %#lx\n", status );
            status = raw_call( pipe, DWM_OP_GET_ATTR, HandleToUlong( other.hwnd ), DWM_PROTOCOL_VERSION, &get,
                               sizeof(get), &rep, sizeof(rep), &len, NULL );
            ok( status == STATUS_ACCESS_DENIED, "other desktop's window, read: status %#lx\n", status );
        }
        else skip( "no window on another desktop\n" );
        SetEvent( other.quit );
        WaitForSingleObject( thread, 5000 );
        CloseHandle( thread );
        CloseDesktop( other.desktop );
    }
    CloseHandle( other.done );
    CloseHandle( other.quit );

    /* malformed requests */
    status = raw_call( pipe, DWM_OP_SET_ATTR, HandleToUlong( hwnd ), DWM_PROTOCOL_VERSION, &set, sizeof(set) - 1,
                       &rep, sizeof(rep), &len, NULL );
    ok( status == STATUS_INVALID_PARAMETER, "short payload: status %#lx\n", status );
    status = raw_call( pipe, DWM_OP_SET_ATTR, HandleToUlong( hwnd ), DWM_PROTOCOL_VERSION + 1, &set, sizeof(set),
                       &rep, sizeof(rep), &len, NULL );
    ok( status == STATUS_REVISION_MISMATCH, "wrong version: status %#lx\n", status );
    status = raw_call( pipe, 0x7777, HandleToUlong( hwnd ), DWM_PROTOCOL_VERSION, NULL, 0, NULL, 0, &len, NULL );
    ok( status == STATUS_NOT_IMPLEMENTED, "unknown opcode: status %#lx\n", status );
    memset( big, 0, sizeof(big) );
    status = raw_call( pipe, DWM_OP_ECHO, 0, DWM_PROTOCOL_VERSION, big, sizeof(big), big, sizeof(big), &len, NULL );
    ok( !status && len == sizeof(big), "echo after errors: status %#lx len %u\n", status, len );

    CloseHandle( pipe );
    DestroyWindow( hwnd );
}

/* a connection's thumbnails go away with it */
static void test_thumbnail_connection(void)
{
    struct dwm_thumb_set set = { 7, 0, DWM_THUMB_VISIBLE, 255, { 0, 0, 10, 10 }, { 0 } };
    struct dwm_thumb_list_req list = { 0 };
    unsigned char buf[DWM_MAX_MESSAGE_SIZE];
    struct dwm_thumb_list_reply *lrep = (struct dwm_thumb_list_reply *)buf;
    struct dwm_thumb *thumbs = (struct dwm_thumb *)(buf + sizeof(*lrep));
    HWND dest, src;
    HANDLE pipe, watcher;
    uint32_t len, i;
    NTSTATUS status;
    BOOL found;

    if ((pipe = open_broker()) == INVALID_HANDLE_VALUE) return;
    if ((watcher = open_broker()) == INVALID_HANDLE_VALUE)
    {
        CloseHandle( pipe );
        return;
    }
    dest = CreateWindowExW( 0, L"static", L"dwm_pipe_dest", WS_POPUP, 0, 0, 10, 10, 0, 0, 0, NULL );
    src  = CreateWindowExW( 0, L"static", L"dwm_pipe_src", WS_POPUP, 0, 0, 10, 10, 0, 0, 0, NULL );
    set.src = HandleToUlong( src );
    status = raw_call( pipe, DWM_OP_THUMB_SET, HandleToUlong( dest ), DWM_PROTOCOL_VERSION, &set, sizeof(set),
                       NULL, 0, &len, NULL );
    ok( !status, "thumb set: status %#lx\n", status );

    status = raw_call( watcher, DWM_OP_THUMB_LIST, 0, DWM_PROTOCOL_VERSION, &list, sizeof(list), buf, sizeof(buf),
                       &len, NULL );
    for (i = 0, found = FALSE; !status && i < lrep->count; i++) found |= thumbs[i].dest == HandleToUlong( dest );
    ok( !status && found, "thumbnail not listed: status %#lx\n", status );

    CloseHandle( pipe );
    Sleep( 200 );   /* the broker's client thread sees the pipe close */
    status = raw_call( watcher, DWM_OP_THUMB_LIST, 0, DWM_PROTOCOL_VERSION, &list, sizeof(list), buf, sizeof(buf),
                       &len, NULL );
    for (i = 0, found = FALSE; !status && i < lrep->count; i++) found |= thumbs[i].dest == HandleToUlong( dest );
    ok( !status && !found, "thumbnail kept after its connection closed: status %#lx\n", status );

    CloseHandle( watcher );
    DestroyWindow( dest );
    DestroyWindow( src );
}

/* A Low-integrity client reaches the broker and can check it (Windows 11 measured: the limited query right opens the
 * process and its token). Exit code: 0 checked, 1 no pipe, 2 the server check failed. */
static DWORD run_low_child(void)
{
    WCHAR cmd[MAX_PATH + 64], exe[MAX_PATH];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    TOKEN_MANDATORY_LABEL label = { { 0 } };
    SID_IDENTIFIER_AUTHORITY authority = { SECURITY_MANDATORY_LABEL_AUTHORITY };
    PSID low_sid = NULL;
    HANDLE token, low;
    DWORD code = 0xff;

    GetModuleFileNameW( NULL, exe, ARRAY_SIZE(exe) );
    swprintf( cmd, ARRAY_SIZE(cmd), L"\"%s\" dwm_channel lowchild", exe );
    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                           TOKEN_ASSIGN_PRIMARY, &token )) return code;
    if (DuplicateTokenEx( token, 0, NULL, SecurityImpersonation, TokenPrimary, &low ) &&
        AllocateAndInitializeSid( &authority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low_sid ))
    {
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        label.Label.Sid = low_sid;
        if (SetTokenInformation( low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid( low_sid ) ) &&
            CreateProcessAsUserW( low, NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
        {
            WaitForSingleObject( pi.hProcess, 20000 );
            GetExitCodeProcess( pi.hProcess, &code );
            CloseHandle( pi.hProcess );
            CloseHandle( pi.hThread );
        }
        FreeSid( low_sid );
        CloseHandle( low );
    }
    CloseHandle( token );
    return code;
}

static void test_low_integrity_client(void)
{
    DWORD code = run_low_child();

    if (code == 0xff)
    {
        skip( "no Low integrity child\n" );
        return;
    }
    ok( code == 0, "Low integrity client: exit code %lu (1 no pipe, 2 server check failed)\n", code );
}

static void test_no_handle_leak(void)
{
    ULONG before = 0, after = 0;
    unsigned int i;
    HANDLE pipe;

    NtQueryInformationProcess( GetCurrentProcess(), ProcessHandleCount, &before, sizeof(before), NULL );
    for (i = 0; i < 20; i++)
        if ((pipe = open_broker()) != INVALID_HANDLE_VALUE) CloseHandle( pipe );
    NtQueryInformationProcess( GetCurrentProcess(), ProcessHandleCount, &after, sizeof(after), NULL );
    ok( after == before, "handle leak: before %lu after %lu\n", before, after );
}

START_TEST(dwm_channel)
{
    HMODULE module;
    char **argv;
    ULONG len = 0;
    HANDLE pipe;

    if (winetest_get_mainargs( &argv ) >= 3 && !strcmp( argv[2], "lowchild" ))
    {
        if ((pipe = open_broker()) == INVALID_HANDLE_VALUE) ExitProcess( 1 );
        ExitProcess( wex_pipe_server_is( pipe, L"dwm.exe" ) ? 0 : 2 );
    }

    module = GetModuleHandleW( L"dwmapi.dll" );
    if (!module) module = LoadLibraryW( L"dwmapi.dll" );
    pWexDwmpChannelEcho = (void *)GetProcAddress( module, "WexDwmpChannelEcho" );
    if (!pWexDwmpChannelEcho)
    {
        win_skip( "WexDwmpChannelEcho not available (not a Wine build).\n" );
        return;
    }
    if (pWexDwmpChannelEcho( NULL, 0, NULL, &len ) != S_OK)
    {
        skip( "needs a desktop with a DWM broker\n" );
        return;
    }

    test_echo_sizes();
    test_concurrent();
    test_server_check();
    test_window_checks();
    test_thumbnail_connection();
    test_low_integrity_client();
    test_no_handle_leak();
}
