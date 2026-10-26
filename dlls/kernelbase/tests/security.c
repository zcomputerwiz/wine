/*
 * Copyright (C) 2023 Paul Gofman for CodeWeavers
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winerror.h>
#include <winternl.h>

#include "wine/test.h"

static BOOL (WINAPI *pDeriveCapabilitySidsFromName)(const WCHAR *, PSID **, DWORD *, PSID **, DWORD *);

static NTSTATUS (WINAPI *pRtlDeriveCapabilitySidsFromName)(UNICODE_STRING *, PSID, PSID);

static HRESULT (WINAPI *pAppContainerRegisterSid)(PSID, LPCWSTR, LPCWSTR);
static HRESULT (WINAPI *pAppContainerUnregisterSid)(PSID);
static HRESULT (WINAPI *pAppContainerLookupMoniker)(PSID, LPWSTR *);
static void (WINAPI *pAppContainerFreeMemory)(void *);

/* Deterministic test identities: a given token always yields the same SID, so a
 * parent and a re-exec'd helper process can reconstruct identical SIDs without
 * any IPC beyond the command line. Every token below is owned by this test and
 * cleaned up before it finishes. */
#define TOKEN_A       0xa11ceu
#define TOKEN_B       0xb22deu
#define TOKEN_PERSIST 0x1111u
#define TOKEN_CONC    0x2222u
#define TOKEN_CORRUPT 0x3333u

static PSID create_test_sid( DWORD token )
{
    static const SID_IDENTIFIER_AUTHORITY nt_auth = { { 0, 0, 0, 0, 0, 5 } }; /* NT authority */
    PSID sid;

    if (RtlAllocateAndInitializeSid( &nt_auth, 3, 21, token, 0x7f00, 0, 0, 0, 0, 0, &sid ) != STATUS_SUCCESS)
        return NULL;
    return sid;
}

static void test_DeriveCapabilitySidsFromName(void)
{
    BYTE auth_count_sid, auth_count_group_sid;
    PSID *check_group_sid, *check_sid;
    DWORD sid_count, group_sid_count;
    PSID *group_sids, *sids;
    UNICODE_STRING name_us;
    NTSTATUS status;
    DWORD size;
    BOOL bret;

    if (!pDeriveCapabilitySidsFromName)
    {
        win_skip ("DeriveCapabilitySidsFromName is not available.\n");
        return;
    }

    if (0)
    {
        /* Crashes on Windows. */
        pDeriveCapabilitySidsFromName(L"test", NULL, &group_sid_count, NULL, &sid_count);
    }

    sid_count = group_sid_count = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    bret = pDeriveCapabilitySidsFromName(L"test", &group_sids, &group_sid_count, &sids, &sid_count);
    ok(bret && GetLastError() == 0xdeadbeef, "got bret %d, err %lu.\n", bret, GetLastError());
    ok(group_sid_count == 1, "got %lu.\n", group_sid_count);
    ok(sid_count == 1, "got %lu.\n", sid_count);

    auth_count_sid = *RtlSubAuthorityCountSid(sids[0]);
    auth_count_group_sid = *RtlSubAuthorityCountSid(group_sids[0]);

    size = RtlLengthRequiredSid(auth_count_sid);
    check_sid = malloc( size );
    size = RtlLengthRequiredSid(auth_count_group_sid);
    check_group_sid = malloc( size );

    RtlInitUnicodeString(&name_us, L"test");
    status = pRtlDeriveCapabilitySidsFromName(&name_us, check_group_sid, check_sid);
    ok(!status, "failed, status %#lx.\n", status);
    ok(!memcmp(sids[0], check_sid, RtlLengthRequiredSid(auth_count_sid)), "mismatch.\n");
    ok(!memcmp(group_sids[0], check_group_sid, RtlLengthRequiredSid(auth_count_group_sid)), "mismatch.\n");

    free(check_sid);
    free(check_group_sid);

    LocalFree(group_sids[0]);
    LocalFree(group_sids);
    LocalFree(sids[0]);
    LocalFree(sids);
}


/******************************************************************************
 * AppContainer registration conformance tests.
 *
 * These exercise the genuine HKCU-backed kernelbase mapping implemented in
 * security.c: new/duplicate/missing lifecycle, exact Unicode round-trip,
 * independent lookup allocation + matching free, per-SID isolation, cross-
 * process persistence, and deterministic concurrent same-SID publication.
 ******************************************************************************/

static void test_AppContainerRegistration( void )
{
    PSID sid_a = create_test_sid( TOKEN_A );
    PSID sid_b = create_test_sid( TOKEN_B );
    LPWSTR moniker;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerUnregisterSid ||
        !pAppContainerLookupMoniker || !pAppContainerFreeMemory)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }
    if (!sid_a || !sid_b)
    {
        win_skip( "could not create test SIDs.\n" );
        goto done;
    }

    /* Start from a clean slate for the two identities we own. */
    pAppContainerUnregisterSid( sid_a );
    pAppContainerUnregisterSid( sid_b );

    /* --- invalid / NULL inputs are rejected safely, never with bad memory --- */
    hr = pAppContainerRegisterSid( NULL, L"m", L"d" );
    ok( hr == E_INVALIDARG, "got %#lx for NULL-SID register.\n", (unsigned long)hr );
    hr = pAppContainerUnregisterSid( NULL );
    ok( hr == E_INVALIDARG, "got %#lx for NULL-SID unregister.\n", (unsigned long)hr );
    moniker = (LPWSTR)0xdeadbeef;
    hr = pAppContainerLookupMoniker( NULL, &moniker );
    ok( hr == E_INVALIDARG && moniker == NULL, "got %#lx / %p for NULL-SID lookup.\n",
        (unsigned long)hr, (void *)moniker );
    hr = pAppContainerLookupMoniker( sid_a, NULL );
    ok( hr == E_POINTER, "got %#lx for NULL output pointer.\n", (unsigned long)hr );

    /* --- missing lookup / unregister before registration -> FILE_NOT_FOUND --- */
    moniker = (LPWSTR)0xdeadbeef;
    hr = pAppContainerLookupMoniker( sid_a, &moniker );
    ok( hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got %#lx for missing lookup.\n", (unsigned long)hr );
    ok( moniker == NULL, "expected output set to NULL, got %p.\n", (void *)moniker );
    hr = pAppContainerUnregisterSid( sid_a );
    ok( hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got %#lx for missing unregister.\n", (unsigned long)hr );

    /* --- fresh registration S_OK + exact Unicode round-trip --- */
    hr = pAppContainerRegisterSid( sid_a, L"TestMonikerA-\x767d", L"Display A" );
    ok( hr == S_OK, "got %#lx for fresh register A.\n", (unsigned long)hr );

    moniker = NULL;
    hr = pAppContainerLookupMoniker( sid_a, &moniker );
    ok( hr == S_OK, "got %#lx for lookup A.\n", (unsigned long)hr );
    ok( moniker != NULL, "expected non-NULL moniker A.\n" );
    if (moniker)
    {
        ok( !lstrcmpW( moniker, L"TestMonikerA-\x767d" ), "got %s for moniker A.\n", wine_dbgstr_w(moniker) );
        pAppContainerFreeMemory( moniker );
        moniker = NULL;
    }

    /* --- duplicate registration -> ALREADY_EXISTS, original mapping intact --- */
    hr = pAppContainerRegisterSid( sid_a, L"DIFFERENT", L"nope" );
    ok( hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "got %#lx for duplicate register A.\n", (unsigned long)hr );

    moniker = NULL;
    hr = pAppContainerLookupMoniker( sid_a, &moniker );
    ok( hr == S_OK && moniker && !lstrcmpW( moniker, L"TestMonikerA-\x767d" ),
        "original clobbered: %#lx / %s.\n", (unsigned long)hr, wine_dbgstr_w(moniker ? moniker : L"(null)") );
    if (moniker) { pAppContainerFreeMemory( moniker ); moniker = NULL; }

    /* --- second identity registers independently (A/B isolation) --- */
    hr = pAppContainerRegisterSid( sid_b, L"TestMonikerB", L"Display B" );
    ok( hr == S_OK, "got %#lx for fresh register B.\n", (unsigned long)hr );

    moniker = NULL;
    hr = pAppContainerLookupMoniker( sid_b, &moniker );
    ok( hr == S_OK && moniker && !lstrcmpW( moniker, L"TestMonikerB" ), "got %#lx / %s for lookup B.\n",
        (unsigned long)hr, wine_dbgstr_w(moniker ? moniker : L"(null)") );
    if (moniker) { pAppContainerFreeMemory( moniker ); moniker = NULL; }

    /* --- unregistering A leaves B intact; second unregister -> FILE_NOT_FOUND --- */
    hr = pAppContainerUnregisterSid( sid_a );
    ok( hr == S_OK, "got %#lx for unregister A.\n", (unsigned long)hr );

    moniker = (LPWSTR)0xdeadbeef;
    hr = pAppContainerLookupMoniker( sid_a, &moniker );
    ok( hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got %#lx for lookup A after unregister.\n", (unsigned long)hr );
    ok( moniker == NULL, "expected NULL after unregister, got %p.\n", (void *)moniker );

    hr = pAppContainerUnregisterSid( sid_a );
    ok( hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got %#lx for second unregister A.\n", (unsigned long)hr );

    moniker = NULL;
    hr = pAppContainerLookupMoniker( sid_b, &moniker );
    ok( hr == S_OK && moniker && !lstrcmpW( moniker, L"TestMonikerB" ), "B lost: %#lx / %s.\n",
        (unsigned long)hr, wine_dbgstr_w(moniker ? moniker : L"(null)") );
    if (moniker) { pAppContainerFreeMemory( moniker ); moniker = NULL; }

    /* --- cleanup the mapping we own --- */
    hr = pAppContainerUnregisterSid( sid_b );
    ok( hr == S_OK, "got %#lx for unregister B.\n", (unsigned long)hr );

done:
    if (sid_a) RtlFreeSid( sid_a );
    if (sid_b) RtlFreeSid( sid_b );
}

/* Stored-type validation: overwrite our OWN test key's Moniker value with a
 * non-REG_SZ type and confirm the lookup fails cleanly rather than returning
 * garbage. Only touches keys this test created for its own SIDs. */
static void test_AppContainerStoredType( void )
{
    static const WCHAR root[] = L"Software\\Wine\\AppContainer\\Mappings";
    PSID sid = create_test_sid( TOKEN_CORRUPT );
    UNICODE_STRING us;
    WCHAR *path = NULL, *moniker = NULL;
    HANDLE key = NULL, hkcu = NULL;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING us_path, value_us;
    DWORD data = 0x12345678;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }
    if (!sid)
    {
        win_skip( "could not create test SID.\n" );
        return;
    }

    pAppContainerUnregisterSid( sid );
    ok( pAppContainerRegisterSid( sid, L"Good", L"D" ) == S_OK, "setup register failed.\n" );

    if (!RtlConvertSidToUnicodeString( &us, sid, TRUE ))
    {
        DWORD root_len = (DWORD)wcslen( root ), sid_len = us.Length / sizeof(WCHAR);
        path = malloc( (root_len + 1 + sid_len + 1) * sizeof(WCHAR) );
        if (path)
        {
            memcpy( path, root, root_len * sizeof(WCHAR) );
            path[root_len] = L'\\';
            memcpy( path + root_len + 1, us.Buffer, (sid_len + 1) * sizeof(WCHAR) );

            RtlInitUnicodeString( &us_path, path );
            if (!RtlOpenCurrentUser( KEY_READ | KEY_WRITE, &hkcu ))
            {
                InitializeObjectAttributes( &attr, &us_path, OBJ_CASE_INSENSITIVE, hkcu, NULL );
                if (!NtOpenKeyEx( &key, KEY_READ | KEY_SET_VALUE, &attr, 0 ))
                {
                    RtlInitUnicodeString( &value_us, L"Moniker" );
                    NtSetValueKey( key, &value_us, 0, REG_DWORD, &data, sizeof(data) );
                    NtClose( key );

                    moniker = NULL;
                    hr = pAppContainerLookupMoniker( sid, &moniker );
                    ok( hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && moniker == NULL,
                        "corrupt-type lookup returned %#lx / %p (expected INVALID_DATA, NULL).\n",
                        (unsigned long)hr, (void *)moniker );
                    if (moniker) pAppContainerFreeMemory( moniker );
                }
                else
                    win_skip( "could not open own test key to corrupt it.\n" );
                NtClose( hkcu );
            }
        }
    }

    /* Unregister removes the whole leaf regardless of the value type we left. */
    ok( pAppContainerUnregisterSid( sid ) == S_OK, "cleanup unregister failed.\n" );

    if (path) free( path );
    RtlFreeUnicodeString( &us );
    RtlFreeSid( sid );
}


/* ---- re-exec'd helper processes (launched with a "/acchild ..." command line) ---- */

static DWORD next_int_w( const WCHAR **pp )
{
    const WCHAR *s = *pp;
    DWORD v = 0;

    while (*s == L' ') s++;
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (DWORD)(*s - L'0'); s++; }
    *pp = s;
    return v;
}

static int child_lookup( DWORD token )
{
    static const WCHAR expected[] = L"PersistMoniker";
    PSID sid = create_test_sid( token );
    LPWSTR moniker = NULL;
    BOOL ok_result = FALSE;
    HRESULT hr;

    if (sid)
    {
        hr = pAppContainerLookupMoniker( sid, &moniker );
        ok_result = (hr == S_OK && moniker && !lstrcmpW( moniker, expected ));
        printf( "  [acchild] lookup: hr=%#lx moniker=%s\n", (unsigned long)hr,
                wine_dbgstr_w(moniker ? moniker : L"(null)") );
        if (moniker) pAppContainerFreeMemory( moniker );
        RtlFreeSid( sid );
    }
    return ok_result ? 0 : 1;
}

static int child_conc( int idx, DWORD token, DWORD ppid )
{
    WCHAR name[64];
    HANDLE ready, go;
    PSID sid = create_test_sid( token );
    HRESULT hr;
    int code = 2;

    if (!sid) return 2;

    swprintf( name, ARRAY_SIZE(name), L"WineACTestReady%d_%u", idx, ppid );
    ready = OpenEventW( EVENT_MODIFY_STATE, FALSE, name );
    swprintf( name, ARRAY_SIZE(name), L"WineACTestGo_%u", ppid );
    go = OpenEventW( SYNCHRONIZE, FALSE, name );
    if (!ready || !go) { RtlFreeSid( sid ); return 2; }

    SetEvent( ready );                 /* signal "armed" to the parent */
    WaitForSingleObject( go, INFINITE );/* park until the barrier releases us together */

    hr = pAppContainerRegisterSid( sid, L"ConcMoniker", L"ConcDisp" );
    if (hr == S_OK) code = 0;
    else if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) code = 1;
    printf( "  [acchild] conc[%d]: hr=%#lx\n", idx, (unsigned long)hr );

    RtlFreeSid( sid );
    CloseHandle( ready );
    CloseHandle( go );
    return code;
}

/* Case-insensitive ASCII prefix test (lstrncmpiW is not available in this build). */
static int ac_starts_with( const WCHAR *s, const WCHAR *prefix )
{
    while (*prefix)
    {
        WCHAR a = *s, b = *prefix;
        if (a >= L'A' && a <= L'Z') a += 32;
        if (b >= L'A' && b <= L'Z') b += 32;
        if (a != b) return 1;
        s++; prefix++;
    }
    return 0;
}

static int ac_child_main( void )
{
    const WCHAR *cmdline = GetCommandLineW();
    const WCHAR *p = wcsstr( cmdline, L"/acchild" );

    if (!p) return 1;
    p += 8; /* skip "/acchild" */
    while (*p == L' ') p++;

    if (!ac_starts_with( p, L"lookup" ))
    {
        p += 6; /* skip "lookup"; next_int_w skips the following space */
        DWORD token = next_int_w( &p );
        return child_lookup( token );
    }
    if (!ac_starts_with( p, L"conc" ))
    {
        p += 4; /* skip "conc" */
        int idx = (int)next_int_w( &p );
        DWORD token = next_int_w( &p );
        DWORD ppid = next_int_w( &p );
        return child_conc( idx, token, ppid );
    }
    return 1;
}


static BOOL launch_child( const WCHAR *args, PROCESS_INFORMATION *pi )
{
    WCHAR path[MAX_PATH], cmdline[MAX_PATH + 256];
    STARTUPINFOW si;

    if (!GetModuleFileNameW( NULL, path, MAX_PATH )) return FALSE;
    swprintf( cmdline, ARRAY_SIZE(cmdline), L"\"%s\" %s", path, args );
    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    return CreateProcessW( path, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, pi );
}

/* A separate process must be able to look up a mapping this process registered:
 * persistence comes from the registry store, not an in-process cache. */
static void test_AppContainerPersistence( void )
{
    PSID sid = create_test_sid( TOKEN_PERSIST );
    PROCESS_INFORMATION pi;
    WCHAR args[64];
    DWORD code = 0xdeadbeef;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }
    if (!sid)
    {
        win_skip( "could not create persistence SID.\n" );
        return;
    }

    pAppContainerUnregisterSid( sid );
    hr = pAppContainerRegisterSid( sid, L"PersistMoniker", L"PersistDisp" );
    ok( hr == S_OK, "got %#lx for persistence register.\n", (unsigned long)hr );

    swprintf( args, ARRAY_SIZE(args), L"security /acchild lookup %u", TOKEN_PERSIST );
    if (launch_child( args, &pi ))
    {
        WaitForSingleObject( pi.hProcess, 30000 );
        GetExitCodeProcess( pi.hProcess, &code );
        ok( code == 0, "separate-process lookup failed, exit %lu.\n", code );
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
    }
    else
        win_skip( "could not launch persistence child.\n" );

    pAppContainerUnregisterSid( sid ); /* cleanup owned mapping */
    RtlFreeSid( sid );
}

/* Two real children race to register the SAME SID, released together by a parent
 * barrier. Exactly one must succeed (S_OK) and the other report already-exists;
 * afterward exactly one mapping with the expected moniker exists. */
static void test_AppContainerConcurrent( void )
{
    PSID sid = create_test_sid( TOKEN_CONC );
    PROCESS_INFORMATION pi[2];
    HANDLE ready[2], go, procs[2];
    DWORD code[2] = { 0xdeadbeef, 0xdeadbeef };
    WCHAR name[64], args[160];
    LPWSTR moniker;
    HRESULT hr;
    int i;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }
    if (!sid)
    {
        win_skip( "could not create concurrency SID.\n" );
        return;
    }

    ZeroMemory( pi, sizeof(pi) );
    pAppContainerUnregisterSid( sid );

    swprintf( name, ARRAY_SIZE(name), L"WineACTestReady0_%u", GetCurrentProcessId() );
    ready[0] = CreateEventW( NULL, TRUE, FALSE, name );
    swprintf( name, ARRAY_SIZE(name), L"WineACTestReady1_%u", GetCurrentProcessId() );
    ready[1] = CreateEventW( NULL, TRUE, FALSE, name );
    swprintf( name, ARRAY_SIZE(name), L"WineACTestGo_%u", GetCurrentProcessId() );
    go = CreateEventW( NULL, TRUE, FALSE, name );
    if (!ready[0] || !ready[1] || !go)
    {
        win_skip( "could not create barrier events.\n" );
        goto cleanup;
    }

    for (i = 0; i < 2; i++)
    {
        swprintf( args, ARRAY_SIZE(args), L"security /acchild conc %d %u %u", i, TOKEN_CONC, GetCurrentProcessId() );
        if (!launch_child( args, &pi[i] ))
        {
            win_skip( "could not launch concurrent child %d.\n", i );
            for (i = 0; i < 2 && pi[i].hProcess; i++)
            {
                CloseHandle( pi[i].hThread );
                CloseHandle( pi[i].hProcess );
            }
            goto cleanup;
        }
    }

    if (WaitForMultipleObjects( 2, ready, TRUE, 30000 ) != WAIT_OBJECT_0)
    {
        win_skip( "children did not arm within timeout.\n" );
        for (i = 0; i < 2; i++) TerminateProcess( pi[i].hProcess, 1 );
        goto cleanup;
    }

    Sleep( 50 ); /* let both children park on the go event before release */
    SetEvent( go ); /* release both writers together */

    procs[0] = pi[0].hProcess;
    procs[1] = pi[1].hProcess;
    WaitForMultipleObjects( 2, procs, TRUE, 60000 );
    GetExitCodeProcess( pi[0].hProcess, &code[0] );
    GetExitCodeProcess( pi[1].hProcess, &code[1] );

    ok( (code[0] == 0 && code[1] == 1) || (code[0] == 1 && code[1] == 0),
        "concurrent same-SID writers gave %lu / %lu, expected {S_OK, ALREADY_EXISTS}.\n", code[0], code[1] );

    moniker = NULL;
    hr = pAppContainerLookupMoniker( sid, &moniker );
    ok( hr == S_OK && moniker && !lstrcmpW( moniker, L"ConcMoniker" ),
        "post-race lookup %#lx / %s (expected exactly one published mapping).\n",
        (unsigned long)hr, wine_dbgstr_w(moniker ? moniker : L"(null)") );
    if (moniker) pAppContainerFreeMemory( moniker );

cleanup:
    for (i = 0; i < 2 && pi[i].hProcess; i++)
    {
        CloseHandle( pi[i].hThread );
        CloseHandle( pi[i].hProcess );
    }
    if (ready[0]) CloseHandle( ready[0] );
    if (ready[1]) CloseHandle( ready[1] );
    if (go) CloseHandle( go );

    pAppContainerUnregisterSid( sid ); /* cleanup owned mapping */
    RtlFreeSid( sid );
}


START_TEST(security)
{
    HMODULE hmod;

    hmod = LoadLibraryA("kernelbase.dll");
    pDeriveCapabilitySidsFromName = (void *)GetProcAddress(hmod, "DeriveCapabilitySidsFromName");
    pAppContainerRegisterSid      = (void *)GetProcAddress(hmod, "AppContainerRegisterSid");
    pAppContainerUnregisterSid    = (void *)GetProcAddress(hmod, "AppContainerUnregisterSid");
    pAppContainerLookupMoniker    = (void *)GetProcAddress(hmod, "AppContainerLookupMoniker");
    pAppContainerFreeMemory       = (void *)GetProcAddress(hmod, "AppContainerFreeMemory");

    hmod = LoadLibraryA("ntdll.dll");
    pRtlDeriveCapabilitySidsFromName = (void *)GetProcAddress(hmod, "RtlDeriveCapabilitySidsFromName");

    if (wcsstr( GetCommandLineW(), L"/acchild" ))
    {
        int code = ac_child_main();   /* re-exec'd helper process */
        fflush( stdout );
        ExitProcess( code );
    }

    test_DeriveCapabilitySidsFromName();
    test_AppContainerRegistration();
    test_AppContainerStoredType();
    test_AppContainerPersistence();
    test_AppContainerConcurrent();
}
