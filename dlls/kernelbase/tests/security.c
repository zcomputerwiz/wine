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

static HRESULT (WINAPI *pDeriveAppContainerSidFromAppContainerName)(const WCHAR *, PSID *);

static HRESULT (WINAPI *pAppContainerRegisterSid)(PSID, LPCWSTR, LPCWSTR);
static HRESULT (WINAPI *pAppContainerUnregisterSid)(PSID);
static HRESULT (WINAPI *pAppContainerLookupMoniker)(PSID, LPWSTR *);
static void (WINAPI *pAppContainerFreeMemory)(void *);

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
 * independent lookup allocation + matching free, per-SID isolation, corrupt and
 * abandoned-value handling, cross-process persistence, deterministic concurrent
 * same-SID publication, and lock serialization (a blocked reader and a failed
 * lock acquisition that must not touch the registry).
 *
 * Identities are NOT fixed S-1-5-21 test SIDs: each run derives fresh package
 * SIDs from unique per-run app container names via the accepted
 * DeriveAppContainerSidFromAppContainerName API, so a parent and its re-exec'd
 * helper processes reconstruct identical SIDs from the name on the command line
 * without ever unregistering a foreign or hardcoded identity.
 ******************************************************************************/

static const WCHAR AC_TEST_ROOT[] = L"Software\\Wine\\AppContainer\\Mappings";

/* A unique-per-run app container name. The tag guarantees distinctness even if
 * GetTickCount() collides between two calls in the same process. */
static void ac_gen_name( WCHAR *buf, int cch, const WCHAR *tag )
{
    swprintf( buf, cch, L"%s-%u-%u", tag, (unsigned)GetTickCount(), (unsigned)GetCurrentProcessId() );
}

/* Derive the deterministic package SID for `name`, or NULL on failure. */
static PSID ac_derive_sid( const WCHAR *name )
{
    PSID sid = NULL;
    if (!pDeriveAppContainerSidFromAppContainerName) return NULL;
    if (pDeriveAppContainerSidFromAppContainerName( name, &sid ) != S_OK) return NULL;
    return sid;
}

/* Build "<AC_TEST_ROOT>\<SID-string>" for the package SID derived from `name`,
 * on the process heap, or NULL. */
static WCHAR *ac_test_path_for_name( const WCHAR *name )
{
    PSID sid = ac_derive_sid( name );
    UNICODE_STRING us;
    WCHAR *path = NULL, *sidstr;
    DWORD root_len, slen;

    if (!sid) return NULL;
    if (RtlConvertSidToUnicodeString( &us, sid, TRUE )) { RtlFreeSid( sid ); return NULL; }
    sidstr = us.Buffer;
    root_len = (DWORD)wcslen( AC_TEST_ROOT );
    slen = (DWORD)wcslen( sidstr );
    path = HeapAlloc( GetProcessHeap(), 0, (root_len + 1 + slen + 1) * sizeof(WCHAR) );
    if (path)
    {
        memcpy( path, AC_TEST_ROOT, root_len * sizeof(WCHAR) );
        path[root_len] = L'\\';
        memcpy( path + root_len + 1, sidstr, (slen + 1) * sizeof(WCHAR) );
    }
    RtlFreeUnicodeString( &us );
    RtlFreeSid( sid );
    return path;
}

/* Ensure every level of AC_TEST_ROOT exists under the HKCU key `hkcu`. */
static NTSTATUS ac_test_ensure_root( HANDLE hkcu )
{
    DWORD len = (DWORD)wcslen( AC_TEST_ROOT );
    HANDLE handle = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    DWORD i;

    for (i = 0; i <= len && !status; i++)
    {
        if (i < len && AC_TEST_ROOT[i] != L'\\') continue;
        if (handle) NtClose( handle );
        {
            OBJECT_ATTRIBUTES attr; UNICODE_STRING us; ULONG disp;

            RtlInitUnicodeString( &us, AC_TEST_ROOT );
            us.Length = i * sizeof(WCHAR);
            InitializeObjectAttributes( &attr, &us, OBJ_CASE_INSENSITIVE, hkcu, NULL );
            status = NtCreateKey( &handle, KEY_READ | KEY_WRITE, &attr, 0, NULL, REG_OPTION_NON_VOLATILE, &disp );
        }
    }
    if (handle) NtClose( handle );
    return status;
}

/* Pre-create an EMPTY mapping leaf for `name` (a key with no values), simulating
 * a writer that created the key and died before publishing either value. */
static BOOL ac_precreate_empty_leaf( const WCHAR *name )
{
    WCHAR *path = ac_test_path_for_name( name );
    HANDLE hkcu = NULL, key = NULL;
    OBJECT_ATTRIBUTES attr; UNICODE_STRING us;
    BOOL applied = FALSE;

    if (!path) return FALSE;
    if (!RtlOpenCurrentUser( KEY_READ | KEY_WRITE | KEY_CREATE_SUB_KEY, &hkcu ) &&
        !ac_test_ensure_root( hkcu ))
    {
        RtlInitUnicodeString( &us, path );
        InitializeObjectAttributes( &attr, &us, OBJ_CASE_INSENSITIVE, hkcu, NULL );
        applied = (NtCreateKey( &key, KEY_READ | KEY_WRITE, &attr, 0, NULL, REG_OPTION_NON_VOLATILE, NULL ) == STATUS_SUCCESS);
        if (key) NtClose( key );
    }
    if (hkcu) NtClose( hkcu );
    HeapFree( GetProcessHeap(), 0, path );
    return applied;
}

/* Overwrite our OWN leaf's Moniker value with an arbitrary type/bytes to create a
 * corrupt stored value. Returns TRUE only if the corruption was actually applied. */
static BOOL ac_corrupt_moniker( const WCHAR *name, DWORD type, const void *bytes, DWORD len )
{
    WCHAR *path = ac_test_path_for_name( name );
    HANDLE hkcu = NULL, key = NULL;
    OBJECT_ATTRIBUTES attr; UNICODE_STRING us, vus;
    BOOL applied = FALSE;

    if (!path) return FALSE;
    if (!RtlOpenCurrentUser( KEY_READ | KEY_SET_VALUE, &hkcu ))
    {
        RtlInitUnicodeString( &us, path );
        InitializeObjectAttributes( &attr, &us, OBJ_CASE_INSENSITIVE, hkcu, NULL );
        if (!NtOpenKeyEx( &key, KEY_READ | KEY_SET_VALUE, &attr, 0 ))
        {
            RtlInitUnicodeString( &vus, L"Moniker" );
            applied = (NtSetValueKey( key, &vus, 0, type, bytes, len ) == STATUS_SUCCESS);
            NtClose( key );
        }
        NtClose( hkcu );
    }
    HeapFree( GetProcessHeap(), 0, path );
    return applied;
}

static void test_AppContainerRegistration( void )
{
    WCHAR name_a[64], name_b[64];
    PSID sid_a, sid_b;
    LPWSTR moniker;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerUnregisterSid ||
        !pAppContainerLookupMoniker || !pAppContainerFreeMemory)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }

    ac_gen_name( name_a, ARRAY_SIZE(name_a), L"REG-A" );
    ac_gen_name( name_b, ARRAY_SIZE(name_b), L"REG-B" );
    sid_a = ac_derive_sid( name_a );
    sid_b = ac_derive_sid( name_b );
    if (!sid_a || !sid_b)
    {
        ok( FALSE, "could not derive test package SIDs.\n" );
        goto done;
    }

    /* Start from a clean slate for the two identities we own this run. */
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

    /* --- two lookups yield INDEPENDENT allocations: distinct pointers, each freed
     *      independently without corrupting the other, both with exact content --- */
    {
        LPWSTR m1 = NULL, m2 = NULL;

        hr = pAppContainerLookupMoniker( sid_a, &m1 );
        ok( hr == S_OK && m1, "independent lookup 1 failed: %#lx.\n", (unsigned long)hr );
        hr = pAppContainerLookupMoniker( sid_a, &m2 );
        ok( hr == S_OK && m2, "independent lookup 2 failed: %#lx.\n", (unsigned long)hr );
        if (m1 && m2)
        {
            ok( m1 != m2, "expected two distinct allocations, both %p.\n", (void *)m1 );
            ok( !lstrcmpW( m1, L"TestMonikerA-\x767d" ) && !lstrcmpW( m2, L"TestMonikerA-\x767d" ),
                "independent allocations differ in content: %s vs %s.\n",
                wine_dbgstr_w(m1), wine_dbgstr_w(m2) );
            pAppContainerFreeMemory( m1 );
            pAppContainerFreeMemory( m2 );
        }
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

/* Corrupt-value handling: overwrite our OWN leaf's Moniker value with a wrong data
 * type, an unterminated REG_SZ, and an odd-length REG_SZ; each must be rejected as
 * ERROR_INVALID_DATA with NULL output rather than returned as garbage. */
static void test_AppContainerCorruptValue( void )
{
    WCHAR name[64];
    PSID sid;
    LPWSTR moniker;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }

    ac_gen_name( name, ARRAY_SIZE(name), L"CT" );
    sid = ac_derive_sid( name );
    if (!sid)
    {
        ok( FALSE, "could not derive corrupt-value SID.\n" );
        return;
    }

    pAppContainerUnregisterSid( sid );
    ok( pAppContainerRegisterSid( sid, L"Good", L"D" ) == S_OK, "setup register failed.\n" );

    /* (a) wrong data type: REG_DWORD instead of REG_SZ */
    {
        DWORD dword = 0x12345678;
        if (ac_corrupt_moniker( name, REG_DWORD, &dword, sizeof(dword) ))
        {
            moniker = NULL;
            hr = pAppContainerLookupMoniker( sid, &moniker );
            ok( hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && moniker == NULL,
                "REG_DWORD lookup returned %#lx / %p (expected INVALID_DATA, NULL).\n",
                (unsigned long)hr, (void *)moniker );
            if (moniker) pAppContainerFreeMemory( moniker );
        }
        else ok( FALSE, "could not store REG_DWORD corrupt value.\n" );
    }

    /* (b) unterminated REG_SZ: exactly two WCHARs ('A','B'), no trailing NUL. Use an
     * explicit byte array -- a string literal would append a terminating NUL WCHAR. */
    {
        static const BYTE unterm[] = { 'A', 0, 'B', 0 };   /* 4 bytes: two WCHARs, no NUL */
        if (ac_corrupt_moniker( name, REG_SZ, unterm, sizeof(unterm) ))
        {
            moniker = NULL;
            hr = pAppContainerLookupMoniker( sid, &moniker );
            ok( hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && moniker == NULL,
                "unterminated lookup returned %#lx / %p (expected INVALID_DATA, NULL).\n",
                (unsigned long)hr, (void *)moniker );
            if (moniker) pAppContainerFreeMemory( moniker );
        }
        else ok( FALSE, "could not store unterminated corrupt value.\n" );
    }

    /* (c) odd-length REG_SZ: 3 bytes is not a whole number of WCHARs */
    {
        static const BYTE odd[] = { 'A', 0, 'B' };   /* 3 bytes */
        if (ac_corrupt_moniker( name, REG_SZ, odd, sizeof(odd) ))
        {
            moniker = NULL;
            hr = pAppContainerLookupMoniker( sid, &moniker );
            ok( hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && moniker == NULL,
                "odd-length lookup returned %#lx / %p (expected INVALID_DATA, NULL).\n",
                (unsigned long)hr, (void *)moniker );
            if (moniker) pAppContainerFreeMemory( moniker );
        }
        else ok( FALSE, "could not store odd-length corrupt value.\n" );
    }

    /* Unregister removes the whole leaf regardless of the value type we left. */
    ok( pAppContainerUnregisterSid( sid ) == S_OK, "cleanup unregister failed.\n" );

    RtlFreeSid( sid );
}

/* Abandoned-writer policy: a prior writer that created the leaf key but died before
 * publishing either value leaves an EMPTY subkey. Registering must repair it (publish
 * both values) rather than report a spurious duplicate or invent an existing mapping. */
static void test_AppContainerAbandonedWriter( void )
{
    WCHAR name[64];
    PSID sid;
    LPWSTR moniker;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }

    ac_gen_name( name, ARRAY_SIZE(name), L"REP" );
    sid = ac_derive_sid( name );
    if (!sid)
    {
        ok( FALSE, "could not derive repair SID.\n" );
        return;
    }

    pAppContainerUnregisterSid( sid );
    if (!ac_precreate_empty_leaf( name ))
    {
        ok( FALSE, "could not pre-create abandoned leaf.\n" );
        goto done;
    }

    hr = pAppContainerRegisterSid( sid, L"RepMoniker", L"RepDisp" );
    ok( hr == S_OK, "repair of abandoned leaf returned %#lx (expected S_OK).\n", (unsigned long)hr );

    moniker = NULL;
    hr = pAppContainerLookupMoniker( sid, &moniker );
    ok( hr == S_OK && moniker && !lstrcmpW( moniker, L"RepMoniker" ),
        "post-repair lookup %#lx / %s (expected RepMoniker).\n",
        (unsigned long)hr, wine_dbgstr_w(moniker ? moniker : L"(null)") );
    if (moniker) pAppContainerFreeMemory( moniker );

done:
    pAppContainerUnregisterSid( sid );
    RtlFreeSid( sid );
}


/* ---- re-exec'd helper processes (launched with a "/acchild ..." command line) ---- */

static int ac_parse_word( const WCHAR **pp, WCHAR *out, int cch )
{
    const WCHAR *s = *pp;
    int n = 0;

    while (*s == L' ') s++;
    while (*s && *s != L' ' && n < cch - 1) out[n++] = *s++;
    out[n] = 0;
    *pp = s;
    return n;
}

static DWORD ac_parse_int( const WCHAR **pp )
{
    const WCHAR *s = *pp;
    DWORD v = 0;

    while (*s == L' ') s++;
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (DWORD)(*s - L'0'); s++; }
    *pp = s;
    return v;
}

static int child_lookup( const WCHAR *name, const WCHAR *expected )
{
    PSID sid = ac_derive_sid( name );
    LPWSTR moniker = NULL;
    BOOL good = FALSE;
    HRESULT hr;

    if (sid)
    {
        hr = pAppContainerLookupMoniker( sid, &moniker );
        good = (hr == S_OK && moniker && !lstrcmpW( moniker, expected ));
        printf( "  [acchild] lookup: hr=%#lx moniker=%s\n", (unsigned long)hr,
                wine_dbgstr_w(moniker ? moniker : L"(null)") );
        if (moniker) pAppContainerFreeMemory( moniker );
        RtlFreeSid( sid );
    }
    return good ? 0 : 1;
}

static int child_conc( int idx, const WCHAR *name, DWORD ppid )
{
    WCHAR ename[96];
    HANDLE ready, go;
    PSID sid = ac_derive_sid( name );
    HRESULT hr;
    int code = 2;

    if (!sid) return 2;

    swprintf( ename, ARRAY_SIZE(ename), L"WineACTestReady%d_%u", idx, ppid );
    ready = OpenEventW( EVENT_MODIFY_STATE, FALSE, ename );
    swprintf( ename, ARRAY_SIZE(ename), L"WineACTestGo_%u", ppid );
    go = OpenEventW( SYNCHRONIZE, FALSE, ename );
    if (!ready || !go) { RtlFreeSid( sid ); return 2; }

    SetEvent( ready );                   /* signal "armed" to the parent */
    WaitForSingleObject( go, INFINITE ); /* park until the barrier releases us together */

    hr = pAppContainerRegisterSid( sid, L"ConcMoniker", L"ConcDisp" );
    if (hr == S_OK) code = 0;
    else if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) code = 1;
    printf( "  [acchild] conc[%d]: hr=%#lx\n", idx, (unsigned long)hr );

    RtlFreeSid( sid );
    CloseHandle( ready );
    CloseHandle( go );
    return code;
}

/* Child for the blocked-reader test: signals a named "entered" event the instant it is
 * about to call the lookup, so the parent can deterministically observe us actually
 * reaching the blocking read (no arbitrary startup-time sleep), then performs the
 * lookup -- which must block on the lock the parent holds until it releases it. */
static int child_blockread( const WCHAR *name, const WCHAR *expected, DWORD ppid )
{
    WCHAR ename[96];
    HANDLE entered;
    PSID sid = ac_derive_sid( name );
    LPWSTR moniker = NULL;
    BOOL good = FALSE;
    HRESULT hr;

    if (!sid) return 2;

    swprintf( ename, ARRAY_SIZE(ename), L"WineACBlockReadEntered_%u", ppid );
    entered = OpenEventW( EVENT_MODIFY_STATE, FALSE, ename );
    if (entered) { SetEvent( entered ); CloseHandle( entered ); }

    hr = pAppContainerLookupMoniker( sid, &moniker );   /* blocks on the parent-held lock */
    good = (hr == S_OK && moniker && !lstrcmpW( moniker, expected ));
    printf( "  [acchild] blockread: hr=%#lx moniker=%s\n", (unsigned long)hr,
            wine_dbgstr_w(moniker ? moniker : L"(null)") );
    if (moniker) pAppContainerFreeMemory( moniker );
    RtlFreeSid( sid );
    return good ? 0 : 1;
}

static int ac_child_main( void )
{
    const WCHAR *cmdline = GetCommandLineW();
    const WCHAR *p = wcsstr( cmdline, L"/acchild" );
    WCHAR mode[16], name[96], expected[96];

    if (!p) return 1;
    p += 8; /* skip "/acchild" */

    if (ac_parse_word( &p, mode, ARRAY_SIZE(mode) ) == 0) return 1;

    if (!lstrcmpW( mode, L"lookup" ))
    {
        ac_parse_word( &p, name, ARRAY_SIZE(name) );       /* container name */
        ac_parse_word( &p, expected, ARRAY_SIZE(expected) );/* expected moniker */
        return child_lookup( name, expected );
    }
    if (!lstrcmpW( mode, L"conc" ))
    {
        int idx = (int)ac_parse_int( &p );
        ac_parse_word( &p, name, ARRAY_SIZE(name) );       /* container name */
        DWORD ppid = ac_parse_int( &p );
        return child_conc( idx, name, ppid );
    }
    if (!lstrcmpW( mode, L"blockread" ))
    {
        ac_parse_word( &p, name, ARRAY_SIZE(name) );        /* container name */
        ac_parse_word( &p, expected, ARRAY_SIZE(expected) );/* expected moniker */
        DWORD ppid = ac_parse_int( &p );
        return child_blockread( name, expected, ppid );
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

/* Bounded, idempotent cleanup of one child: wait briefly, terminate if still
 * running, reap, then close both handles and NULL them so a later call is a no-op.
 * Every Win32 call's result is evaluated; a child that will not die is surfaced as an
 * error (it would otherwise be an orphan) rather than silently ignored. Never
 * double-closes and never leaks the handles. */
static void ac_reap( PROCESS_INFORMATION *pi )
{
    DWORD wst;

    if (!pi || !pi->hProcess) return;

    wst = WaitForSingleObject( pi->hProcess, 3000 );
    if (wst == WAIT_FAILED)
        fprintf(stderr, "ac_reap: initial wait failed (%lu)\n", (unsigned long)GetLastError() );
    else if (wst == WAIT_TIMEOUT)
    {
        if (!TerminateProcess( pi->hProcess, 1 ))
            fprintf(stderr, "ac_reap: TerminateProcess failed\n" );
        wst = WaitForSingleObject( pi->hProcess, 5000 );   /* let termination complete */
        if (wst != WAIT_OBJECT_0 && wst != WAIT_ABANDONED)
            fprintf(stderr, "ac_reap: child did not exit after terminate (wait %lu)\n", (unsigned long)wst );
    }

    CloseHandle( pi->hThread );
    CloseHandle( pi->hProcess );
    pi->hThread = NULL;
    pi->hProcess = NULL;
}

/* A separate process must be able to look up a mapping this process registered:
 * persistence comes from the registry store, not an in-process cache. */
static void test_AppContainerPersistence( void )
{
    WCHAR name[64], args[160];
    PSID sid;
    PROCESS_INFORMATION pi;
    DWORD code = 0xdeadbeef;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }

    ac_gen_name( name, ARRAY_SIZE(name), L"PERSIST" );
    sid = ac_derive_sid( name );
    if (!sid)
    {
        ok( FALSE, "could not derive persistence SID.\n" );
        return;
    }

    pAppContainerUnregisterSid( sid );
    hr = pAppContainerRegisterSid( sid, L"PersistMoniker", L"PersistDisp" );
    ok( hr == S_OK, "got %#lx for persistence register.\n", (unsigned long)hr );

    swprintf( args, ARRAY_SIZE(args), L"security /acchild lookup %s PersistMoniker", name );
    ZeroMemory( &pi, sizeof(pi) );
    if (!launch_child( args, &pi ))
        ok( FALSE, "could not launch persistence child.\n" );   /* required case: count as failure */
    else
    {
        DWORD wst = WaitForSingleObject( pi.hProcess, 30000 );
        if (wst == WAIT_OBJECT_0)
        {
            if (!GetExitCodeProcess( pi.hProcess, &code )) code = 0xdeadbeef;
            ok( code == 0, "separate-process lookup failed, exit %lu.\n", code );
        }
        else
            ok( FALSE, "persistence child did not finish in time (wait %lu).\n", wst );
        ac_reap( &pi );
    }

    pAppContainerUnregisterSid( sid ); /* cleanup owned mapping */
    RtlFreeSid( sid );
}

/* Two real children race to register the SAME package SID, released together by a
 * parent barrier (no sleep-based readiness). Exactly one must succeed (S_OK) and the
 * other report already-exists; afterward exactly one mapping with the expected
 * moniker exists. */
static void test_AppContainerConcurrent( void )
{
    WCHAR name[64], ename[96], args[200];
    PSID sid;
    PROCESS_INFORMATION pi[2];
    HANDLE ready[2] = { NULL, NULL }, go = NULL, procs[2];
    DWORD code[2] = { 0xdeadbeef, 0xdeadbeef };
    LPWSTR moniker;
    HRESULT hr;
    int i;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }

    ac_gen_name( name, ARRAY_SIZE(name), L"CONC" );
    sid = ac_derive_sid( name );
    if (!sid)
    {
        ok( FALSE, "could not derive concurrency SID.\n" );
        goto done;
    }

    ZeroMemory( pi, sizeof(pi) );
    pAppContainerUnregisterSid( sid );

    swprintf( ename, ARRAY_SIZE(ename), L"WineACTestReady0_%u", GetCurrentProcessId() );
    ready[0] = CreateEventW( NULL, TRUE, FALSE, ename );
    swprintf( ename, ARRAY_SIZE(ename), L"WineACTestReady1_%u", GetCurrentProcessId() );
    ready[1] = CreateEventW( NULL, TRUE, FALSE, ename );
    swprintf( ename, ARRAY_SIZE(ename), L"WineACTestGo_%u", GetCurrentProcessId() );
    go = CreateEventW( NULL, TRUE, FALSE, ename );
    if (!ready[0] || !ready[1] || !go)
    {
        ok( FALSE, "could not create barrier events.\n" );
        goto done;
    }

    for (i = 0; i < 2; i++)
    {
        swprintf( args, ARRAY_SIZE(args), L"security /acchild conc %d %s %u", i, name, GetCurrentProcessId() );
        if (!launch_child( args, &pi[i] ))
        {
            ok( FALSE, "could not launch concurrent child %d.\n", i );   /* required case: failure */
            goto done_events;   /* done_events reaps every launched child idempotently */
        }
    }

    if (WaitForMultipleObjects( 2, ready, TRUE, 30000 ) != WAIT_OBJECT_0)
    {
        ok( FALSE, "children did not arm within timeout.\n" );   /* failure, not skip */
        goto done_events;
    }

    SetEvent( go ); /* release both writers together */

    procs[0] = pi[0].hProcess;
    procs[1] = pi[1].hProcess;
    if (WaitForMultipleObjects( 2, procs, TRUE, 60000 ) != WAIT_OBJECT_0)
        ok( FALSE, "concurrent children did not both finish in time.\n" );

    if (!GetExitCodeProcess( pi[0].hProcess, &code[0] )) code[0] = 0xdeadbeef;
    if (!GetExitCodeProcess( pi[1].hProcess, &code[1] )) code[1] = 0xdeadbeef;

    ok( (code[0] == 0 && code[1] == 1) || (code[0] == 1 && code[1] == 0),
        "concurrent same-SID writers gave %lu / %lu, expected {S_OK, ALREADY_EXISTS}.\n", code[0], code[1] );

    moniker = NULL;
    hr = pAppContainerLookupMoniker( sid, &moniker );
    ok( hr == S_OK && moniker && !lstrcmpW( moniker, L"ConcMoniker" ),
        "post-race lookup %#lx / %s (expected exactly one published mapping).\n",
        (unsigned long)hr, wine_dbgstr_w(moniker ? moniker : L"(null)") );
    if (moniker) pAppContainerFreeMemory( moniker );

done_events:
    for (i = 0; i < 2; i++) ac_reap( &pi[i] );   /* idempotent cleanup, no double-close */
    if (ready[0]) CloseHandle( ready[0] );
    if (ready[1]) CloseHandle( ready[1] );
    if (go) CloseHandle( go );

done:
    if (sid) { pAppContainerUnregisterSid( sid ); RtlFreeSid( sid ); }
}

/* Lock serialization across processes, deterministically: the parent holds the real
 * registration lock while a child attempts a lookup. The child's only path to exit
 * goes through the same lock, so it provably cannot finish until the parent releases;
 * a bounded wait that times out is the proof it is waiting for publication, and after
 * release it must observe the complete value (not a half-published or missing one). */
static void test_AppContainerBlockedReader( void )
{
    static const WCHAR lock_name[] = L"\\BaseNamedObjects\\WineAppContainerRegistrationLock";
    WCHAR name[64], args[200], ename[96];
    PSID sid;
    PROCESS_INFORMATION pi;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING us;
    HANDLE lock = NULL, entered = NULL;
    NTSTATUS status;
    DWORD code = 0xdeadbeef, wst;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }

    ac_gen_name( name, ARRAY_SIZE(name), L"BLOCK" );
    sid = ac_derive_sid( name );
    if (!sid)
    {
        ok( FALSE, "could not derive blocked-reader SID.\n" );
        return;
    }

    /* Publish a complete mapping first, then hold the lock ourselves. */
    pAppContainerUnregisterSid( sid );
    hr = pAppContainerRegisterSid( sid, L"PubMoniker", L"PubDisp" );
    ok( hr == S_OK, "setup register for blocked-reader returned %#lx.\n", (unsigned long)hr );

    RtlInitUnicodeString( &us, lock_name );
    InitializeObjectAttributes( &attr, &us, OBJ_CASE_INSENSITIVE | OBJ_OPENIF, NULL, NULL );
    status = NtCreateMutant( &lock, SYNCHRONIZE, &attr, FALSE );
    if (status && status != STATUS_OBJECT_NAME_EXISTS)
    {
        ok( FALSE, "could not create lock: %#lx.\n", (unsigned long)status );
        goto done;
    }
    if (NtWaitForSingleObject( lock, FALSE, NULL ) != STATUS_WAIT_0)
    {
        ok( FALSE, "could not take the registration lock.\n" );
        NtClose( lock );
        lock = NULL;
        goto done;
    }

    /* Readiness handshake: the child signals this event the instant it is about to call
     * the lookup. Waiting on it (instead of an arbitrary sleep) proves the child actually
     * reached the blocking read, so a subsequent still-running check is a genuine proof
     * that the held lock -- not slow startup -- is what keeps it from finishing. */
    swprintf( ename, ARRAY_SIZE(ename), L"WineACBlockReadEntered_%u", GetCurrentProcessId() );
    entered = CreateEventW( NULL, TRUE, FALSE, ename );
    if (!entered)
    {
        ok( FALSE, "could not create blocked-reader handshake event.\n" );
        goto done;
    }

    swprintf( args, ARRAY_SIZE(args), L"security /acchild blockread %s PubMoniker %u", name, GetCurrentProcessId() );
    ZeroMemory( &pi, sizeof(pi) );
    if (!launch_child( args, &pi ))
    {
        ok( FALSE, "could not launch blocked-reader child.\n" );   /* required case: failure */
    }
    else
    {
        wst = WaitForSingleObject( entered, 30000 );
        if (wst != WAIT_OBJECT_0)
            ok( FALSE, "child did not reach the blocking read in time (wait %lu).\n", wst );   /* failure, not skip */

        /* We hold the lock and the child has reached its lookup, so it cannot have exited:
         * a bounded wait that times out is the proof reader locking is required. */
        wst = WaitForSingleObject( pi.hProcess, 2000 );
        ok( wst == WAIT_TIMEOUT, "reader should be blocked on the held lock, got %lu.\n", wst );

        NtReleaseMutant( lock, NULL );   /* release -> child proceeds to read */
        NtClose( lock );
        lock = NULL;

        wst = WaitForSingleObject( pi.hProcess, 30000 );
        if (wst == WAIT_OBJECT_0)
        {
            if (!GetExitCodeProcess( pi.hProcess, &code )) code = 0xdeadbeef;
            ok( code == 0, "blocked reader saw an incomplete/missing mapping, exit %lu.\n", code );
        }
        else
            ok( FALSE, "blocked reader did not finish after lock release (wait %lu).\n", wst );
        ac_reap( &pi );
    }

done:
    if (lock) { NtReleaseMutant( lock, NULL ); NtClose( lock ); }
    if (entered) CloseHandle( entered );
    pAppContainerUnregisterSid( sid );
    RtlFreeSid( sid );
}

/* Lock-acquisition failure must not fail open: with the lock name occupied by a
 * DIFFERENT object type (an event) a real NtCreateMutant fails, and the APIs must
 * abort WITHOUT touching the registry -- never masquerading the failure as an
 * existing registration. Run last so it leaves no stray object for other tests. */
static void test_AppContainerLockFailure( void )
{
    static const WCHAR lock_name[] = L"\\BaseNamedObjects\\WineAppContainerRegistrationLock";
    WCHAR name[64];
    PSID sid;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING us;
    HANDLE evh = NULL;
    NTSTATUS status;
    HRESULT hr;

    if (!pAppContainerRegisterSid || !pAppContainerUnregisterSid || !pAppContainerLookupMoniker)
    {
        win_skip( "AppContainer registration APIs are not available.\n" );
        return;
    }

    ac_gen_name( name, ARRAY_SIZE(name), L"LOCKFAIL" );
    sid = ac_derive_sid( name );
    if (!sid)
    {
        ok( FALSE, "could not derive lock-failure SID.\n" );
        return;
    }
    pAppContainerUnregisterSid( sid );

    RtlInitUnicodeString( &us, lock_name );
    InitializeObjectAttributes( &attr, &us, OBJ_CASE_INSENSITIVE, NULL, NULL );
    status = NtCreateEvent( &evh, EVENT_MODIFY_STATE | SYNCHRONIZE, &attr, 0, FALSE );
    if (status)
    {
        ok( FALSE, "could not occupy the lock name: %#lx.\n", (unsigned long)status );
        goto done;
    }

    hr = pAppContainerRegisterSid( sid, L"m", L"d" );
    ok( hr != S_OK, "register should fail when the lock cannot be acquired, got %#lx.\n", (unsigned long)hr );
    ok( hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
        "lock failure must not masquerade as ALREADY_EXISTS, got %#lx.\n", (unsigned long)hr );

    hr = pAppContainerUnregisterSid( sid );
    ok( hr != S_OK, "unregister should fail when the lock cannot be acquired, got %#lx.\n", (unsigned long)hr );

    /* Verify directly (bypassing the locked APIs) that no leaf was created. */
    {
        WCHAR *path = ac_test_path_for_name( name );
        HANDLE hkcu = NULL, key = NULL;
        OBJECT_ATTRIBUTES a2; UNICODE_STRING u2;
        BOOL exists = FALSE;

        if (path && !RtlOpenCurrentUser( KEY_READ, &hkcu ))
        {
            RtlInitUnicodeString( &u2, path );
            InitializeObjectAttributes( &a2, &u2, OBJ_CASE_INSENSITIVE, hkcu, NULL );
            exists = (NtOpenKeyEx( &key, KEY_READ, &a2, 0 ) == STATUS_SUCCESS);
            if (key) NtClose( key );
            NtClose( hkcu );
        }
        HeapFree( GetProcessHeap(), 0, path );
        ok( !exists, "failed register created a registry leaf despite the lock failure.\n" );
    }

done:
    if (evh) NtClose( evh );   /* single close; frees the name for other tests */
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

    hmod = LoadLibraryA("userenv.dll");
    pDeriveAppContainerSidFromAppContainerName = (void *)GetProcAddress(hmod, "DeriveAppContainerSidFromAppContainerName");

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
    test_AppContainerCorruptValue();
    test_AppContainerAbandonedWriter();
    test_AppContainerPersistence();
    test_AppContainerConcurrent();
    test_AppContainerBlockedReader();
    test_AppContainerLockFailure();   /* last: leaves no stray lock object behind */
}
