/*
 * Unit test suite for userenv functions
 *
 * Copyright 2008 Google (Lei Zhang)
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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"

#include "userenv.h"
#include "sddl.h"
#include "objbase.h"

#include "wine/test.h"

#define expect(EXPECTED,GOT) ok((GOT)==(EXPECTED), "Expected %d, got %d\n", (EXPECTED), (GOT))
#define expect_env(EXPECTED,GOT,VAR) ok((GOT)==(EXPECTED), "Expected %d, got %d for %s (%d)\n", (EXPECTED), (GOT), (VAR), j)
#define expect_gle(EXPECTED) ok(GetLastError() == (EXPECTED), "Expected %d, got %ld\n", (EXPECTED), GetLastError())

static BOOL (WINAPI *pIsWow64Process)(HANDLE,PBOOL);

struct profile_item
{
    const char * name;
};

/* Helper function for retrieving environment variables */
static BOOL get_env(const WCHAR * env, const char * var, char ** result)
{
    const WCHAR * p = env;
    int envlen, varlen, buflen;
    char buf[256];

    if (!env || !var || !result) return FALSE;

    varlen = strlen(var);
    do
    {
        if (!WideCharToMultiByte( CP_ACP, 0, p, -1, buf, sizeof(buf), NULL, NULL )) buf[sizeof(buf)-1] = 0;
        envlen = strlen(buf);
        if (CompareStringA(GetThreadLocale(), NORM_IGNORECASE|LOCALE_USE_CP_ACP, buf, min(envlen, varlen), var, varlen) == CSTR_EQUAL)
        {
            if (buf[varlen] == '=')
            {
                buflen = strlen(buf);
                *result = HeapAlloc(GetProcessHeap(), 0, buflen + 1);
                if (!*result) return FALSE;
                memcpy(*result, buf, buflen + 1);
                return TRUE;
            }
        }
        while (*p) p++;
        p++;
    } while (*p);
    return FALSE;
}

static void test_create_env(void)
{
    BOOL r, is_wow64 = FALSE;
    HANDLE htok;
    WCHAR * env[4];
    char * st, systemroot[100], programdata[100], allusersprofile[100];
    int i, j;

    static const struct profile_item common_vars[] = {
        { "ComSpec" },
        { "COMPUTERNAME" },
        { "NUMBER_OF_PROCESSORS" },
        { "OS" },
        { "PROCESSOR_ARCHITECTURE" },
        { "PROCESSOR_IDENTIFIER" },
        { "PROCESSOR_LEVEL" },
        { "PROCESSOR_REVISION" },
        { "SystemDrive" },
        { "SystemRoot" },
        { "windir" }
    };
    static const struct profile_item common_post_nt4_vars[] = {
        { "ALLUSERSPROFILE" },
        { "ProgramData" },
        { "TEMP" },
        { "TMP" },
        { "CommonProgramFiles" },
        { "ProgramFiles" },
        { "PATH" },
        { "USERPROFILE" }
    };
    static const struct profile_item common_win64_vars[] = {
        { "ProgramFiles(x86)" },
        { "CommonProgramFiles(x86)" },
        { "ProgramW6432" },
        { "CommonProgramW6432" }
    };

    r = SetEnvironmentVariableA("WINE_XYZZY", "ZZYZX");
    expect(TRUE, r);

    r = GetEnvironmentVariableA("SystemRoot", systemroot, sizeof(systemroot));
    ok(r != 0, "GetEnvironmentVariable failed (%ld)\n", GetLastError());

    r = SetEnvironmentVariableA("SystemRoot", "overwrite");
    expect(TRUE, r);

    r = GetEnvironmentVariableA("ProgramData", programdata, sizeof(programdata));
    ok(r != 0, "GetEnvironmentVariable failed (%ld)\n", GetLastError());

    r = SetEnvironmentVariableA("ProgramData", "overwrite");
    expect(TRUE, r);

    if (0)
    {
        /* Crashes on NT4 */
        r = CreateEnvironmentBlock(NULL, NULL, FALSE);
        expect(FALSE, r);
    }

    r = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY|TOKEN_DUPLICATE, &htok);
    expect(TRUE, r);

    if (0)
    {
        /* Crashes on NT4 */
        r = CreateEnvironmentBlock(NULL, htok, FALSE);
        expect(FALSE, r);
    }

    r = CreateEnvironmentBlock((LPVOID) &env[0], NULL, FALSE);
    expect(TRUE, r);

    r = CreateEnvironmentBlock((LPVOID) &env[1], htok, FALSE);
    expect(TRUE, r);

    r = CreateEnvironmentBlock((LPVOID) &env[2], NULL, TRUE);
    expect(TRUE, r);

    r = CreateEnvironmentBlock((LPVOID) &env[3], htok, TRUE);
    expect(TRUE, r);

    r = SetEnvironmentVariableA("SystemRoot", systemroot);
    expect(TRUE, r);

    r = SetEnvironmentVariableA("ProgramData", programdata);
    expect(TRUE, r);

    for(i=0; i<4; i++)
    {
        r = get_env(env[i], "SystemRoot", &st);
        ok(!strcmp(st, "SystemRoot=overwrite"), "%s\n", st);
        expect(TRUE, r);
        HeapFree(GetProcessHeap(), 0, st);

        r = get_env(env[i], "ProgramData", &st);
        ok(strcmp(st, "ProgramData=overwrite"), "%s\n", st);
        expect(TRUE, r);
        HeapFree(GetProcessHeap(), 0, st);
    }

    /* Test for common environment variables (NT4 and higher) */
    for (i = 0; i < ARRAY_SIZE(common_vars); i++)
    {
        for (j = 0; j < 4; j++)
        {
            r = get_env(env[j], common_vars[i].name, &st);
            expect_env(TRUE, r, common_vars[i].name);
            if (r) HeapFree(GetProcessHeap(), 0, st);
        }
    }

    /* Test for common environment variables (post NT4) */
    if (!GetEnvironmentVariableA("ALLUSERSPROFILE", allusersprofile, sizeof(allusersprofile)))
    {
        win_skip("Some environment variables are not present on NT4\n");
    }
    else
    {
        for (i = 0; i < ARRAY_SIZE(common_post_nt4_vars); i++)
        {
            for (j = 0; j < 4; j++)
            {
                r = get_env(env[j], common_post_nt4_vars[i].name, &st);
                expect_env(TRUE, r, common_post_nt4_vars[i].name);
                if (!strcmp(common_post_nt4_vars[i].name, "ALLUSERSPROFILE") ||
                        !strcmp(common_post_nt4_vars[i].name, "ProgramData"))
                    ok(!strcmp(st + strlen(common_post_nt4_vars[i].name) + 1, allusersprofile), "%s\n", st);
                if (r) HeapFree(GetProcessHeap(), 0, st);
            }
        }
    }

    if(pIsWow64Process)
        pIsWow64Process(GetCurrentProcess(), &is_wow64);
    if (sizeof(void*)==8 || is_wow64)
    {
        for (i = 0; i < ARRAY_SIZE(common_win64_vars); i++)
        {
            for (j=0; j<4; j++)
            {
                r = get_env(env[j], common_win64_vars[i].name, &st);
                ok(r || broken(!r)/* Vista,2k3,XP */, "Expected 1, got 0 for %s\n", common_win64_vars[i].name);
                if (r) HeapFree(GetProcessHeap(), 0, st);
            }
        }
    }

    r = get_env(env[0], "WINE_XYZZY", &st);
    expect(FALSE, r);

    r = get_env(env[1], "WINE_XYZZY", &st);
    expect(FALSE, r);

    r = get_env(env[2], "WINE_XYZZY", &st);
    expect(TRUE, r);
    if (r) HeapFree(GetProcessHeap(), 0, st);

    r = get_env(env[3], "WINE_XYZZY", &st);
    expect(TRUE, r);
    if (r) HeapFree(GetProcessHeap(), 0, st);

    for (i = 0; i < ARRAY_SIZE(env); i++)
    {
        r = DestroyEnvironmentBlock(env[i]);
        expect(TRUE, r);
    }
}

static void test_get_profiles_dir(void)
{
    static const char ProfileListA[] = "Software\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
    static const char ProfilesDirectory[] = "ProfilesDirectory";
    BOOL r;
    DWORD cch, profiles_len;
    LONG l;
    HKEY key;
    char *profiles_dir, *buf, small_buf[1];

    l = RegOpenKeyExA(HKEY_LOCAL_MACHINE, ProfileListA, 0, KEY_READ, &key);
    ok(!l, "RegOpenKeyExA failed: %ld\n", GetLastError());

    l = RegQueryValueExA(key, ProfilesDirectory, NULL, NULL, NULL, &cch);
    if (l)
    {
        win_skip("No ProfilesDirectory value (NT4), skipping tests\n");
        return;
    }
    buf = HeapAlloc(GetProcessHeap(), 0, cch);
    RegQueryValueExA(key, ProfilesDirectory, NULL, NULL, (BYTE *)buf, &cch);
    RegCloseKey(key);
    profiles_len = ExpandEnvironmentStringsA(buf, NULL, 0);
    profiles_dir = HeapAlloc(GetProcessHeap(), 0, profiles_len);
    ExpandEnvironmentStringsA(buf, profiles_dir, profiles_len);
    HeapFree(GetProcessHeap(), 0, buf);

    SetLastError(0xdeadbeef);
    r = GetProfilesDirectoryA(NULL, NULL);
    expect(FALSE, r);
    expect_gle(ERROR_INVALID_PARAMETER);
    SetLastError(0xdeadbeef);
    r = GetProfilesDirectoryA(NULL, &cch);
    expect(FALSE, r);
    expect_gle(ERROR_INVALID_PARAMETER);
    SetLastError(0xdeadbeef);
    cch = 1;
    r = GetProfilesDirectoryA(small_buf, &cch);
    expect(FALSE, r);
    expect_gle(ERROR_INSUFFICIENT_BUFFER);
    /* MSDN claims the returned character count includes the NULL terminator
     * when the buffer is too small, but that's not in fact what gets returned.
     */
    ok(cch == profiles_len - 1, "expected %ld, got %ld\n", profiles_len - 1, cch);
    /* Allocate one more character than the return value to prevent a buffer
     * overrun.
     */
    buf = HeapAlloc(GetProcessHeap(), 0, cch + 1);
    r = GetProfilesDirectoryA(buf, &cch);
    /* Rather than a BOOL, the return value is also the number of characters
     * stored in the buffer.
     */
    ok(profiles_len - 1 == r, "expected %ld, got %d\n", profiles_len - 1, r);
    ok(!strcmp(buf, profiles_dir), "expected %s, got %s\n", profiles_dir, buf);
    ok(strlen(buf) + 1 == cch, "String length is %Iu, but cch is %lu\n", strlen(buf), cch);
    ok(strlen(buf) + 1 == r, "String length is %Iu, but returned count is %u\n", strlen(buf), r);

    HeapFree(GetProcessHeap(), 0, buf);
    HeapFree(GetProcessHeap(), 0, profiles_dir);

    SetLastError(0xdeadbeef);
    r = GetProfilesDirectoryW(NULL, NULL);
    expect(FALSE, r);
    expect_gle(ERROR_INVALID_PARAMETER);

    cch = 0;
    SetLastError(0xdeadbeef);
    r = GetProfilesDirectoryW(NULL, &cch);
    expect(FALSE, r);
    expect_gle(ERROR_INSUFFICIENT_BUFFER);
    ok(cch, "expected cch > 0\n");

    SetLastError(0xdeadbeef);
    r = GetProfilesDirectoryW(NULL, &cch);
    expect(FALSE, r);
    expect_gle(ERROR_INSUFFICIENT_BUFFER);
}

static void test_get_user_profile_dir(void)
{
    BOOL ret;
    DWORD error, len, len2;
    HANDLE token;
    char *dirA;
    WCHAR *dirW;

    if (!GetEnvironmentVariableA( "ALLUSERSPROFILE", NULL, 0 ))
    {
        win_skip("Skipping tests on NT4\n");
        return;
    }

    ret = OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token );
    ok(ret, "expected success %lu\n", GetLastError());

    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryA( NULL, NULL, NULL );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", error);

    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryA( token, NULL, NULL );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", error);

    dirA = HeapAlloc( GetProcessHeap(), 0, 32 );
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryA( token, dirA, NULL );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", error);
    HeapFree( GetProcessHeap(), 0, dirA );

    len = 0;
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryA( token, NULL, &len );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", error);
    ok(!len, "expected 0, got %lu\n", len);

    len = SHRT_MAX;
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryA( token, NULL, &len );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", error);
    ok(len == SHRT_MAX, "expected SHRT_MAX, got %lu\n", len);

    len = 0;
    dirA = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, 32 );
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryA( token, dirA, &len );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    HeapFree( GetProcessHeap(), 0, dirA );
    if (error == ERROR_INSUFFICIENT_BUFFER)
    {
        ok(len, "expected len > 0\n");

        dirA = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, len );
        SetLastError( 0xdeadbeef );
        ret = GetUserProfileDirectoryA( token, dirA, &len );
        ok(ret, "expected success %lu\n", GetLastError());
        ok(len, "expected len > 0\n");
        ok(lstrlenA( dirA ) == len - 1, "length mismatch %d != %ld - 1\n", lstrlenA( dirA ), len );
        trace("%s\n", dirA);
        HeapFree( GetProcessHeap(), 0, dirA );
    }
    else
        ok(broken(error == ERROR_INVALID_PARAMETER) /* win10 1809+ */,
           "unexpected error %lu\n", error);

    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryW( NULL, NULL, NULL );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    todo_wine ok(error == ERROR_INVALID_HANDLE, "expected ERROR_INVALID_HANDLE, got %lu\n", error);

    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryW( token, NULL, NULL );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", error);

    dirW = HeapAlloc( GetProcessHeap(), 0, 32 );
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryW( token, dirW, NULL );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INVALID_PARAMETER, "expected ERROR_INVALID_PARAMETER, got %lu\n", error);
    HeapFree( GetProcessHeap(), 0, dirW );

    len = 0;
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryW( token, NULL, &len );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INSUFFICIENT_BUFFER, "expected ERROR_INSUFFICIENT_BUFFER, got %lu\n", error);
    ok(len, "expected len > 0\n");

    len = SHRT_MAX;
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryW( token, NULL, &len );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    ok(error == ERROR_INSUFFICIENT_BUFFER, "expected ERROR_INSUFFICIENT_BUFFER, got %lu\n", error);
    ok(len != SHRT_MAX, "expected not SHRT_MAX, got %lu\n", len);

    dirW = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, len * sizeof(WCHAR) );
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryW( token, dirW, &len );
    ok(ret, "expected success %lu\n", GetLastError());
    ok(len, "expected len > 0\n");
    ok(lstrlenW( dirW ) == len - 1, "length mismatch %d != %ld - 1\n", lstrlenW( dirW ), len );
    HeapFree( GetProcessHeap(), 0, dirW );

    len2 = 0;
    dirW = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, 32 * sizeof(WCHAR) );
    SetLastError( 0xdeadbeef );
    ret = GetUserProfileDirectoryW( token, dirW, &len2 );
    error = GetLastError();
    ok(!ret, "expected failure\n");
    HeapFree( GetProcessHeap(), 0, dirW );
    if (error == ERROR_INSUFFICIENT_BUFFER)
        ok(len2 == len, "expected %ld, got %ld\n", len, len2);
    else
        ok(broken(error == ERROR_INVALID_PARAMETER) /* win10 1809+ */,
           "unexpected error %lu\n", error);

    CloseHandle( token );
}

static void test_appcontainer_sid(void)
{
    static const struct
    {
        const WCHAR *name;
        const char *sid;
    } cases[] =
    {
        { L"test", "S-1-15-2-1980125950-1037672881-421768107-1949737198-2922275827-507320043-1582245000" },
        { L"TEST", "S-1-15-2-1980125950-1037672881-421768107-1949737198-2922275827-507320043-1582245000" },
        { L"WineExplorer.MixedCase_01", "S-1-15-2-3520518378-1338609999-3243366982-3993260403-3393427587-956535139-1620658767" },
        { L"wineexplorer.mixedcase_01", "S-1-15-2-3520518378-1338609999-3243366982-3993260403-3393427587-956535139-1620658767" },
        { L"W\x00e4ne\x03a3", "S-1-15-2-1925959860-2464893839-2425465101-3504107519-2374159773-1197180585-2285453026" },
        { L"a/b:*? ", "S-1-15-2-4173701422-852867342-4044399721-1874327424-3122796747-664920408-875619919" }
    };
    SID_IDENTIFIER_AUTHORITY authority = { SECURITY_APP_PACKAGE_AUTHORITY };
    HRESULT (WINAPI *derive)(PCWSTR, PSID *);
    WCHAR name[66], path[MAX_PATH];
    PSID sid, again, other;
    unsigned int i, length;
    HRESULT hr;
    char *text;
    BOOL ret;

    derive = (void *)GetProcAddress( GetModuleHandleW(L"userenv.dll"), "DeriveAppContainerSidFromAppContainerName" );
    if (!derive)
    {
        win_skip("DeriveAppContainerSidFromAppContainerName is not available\n");
        return;
    }
    GetModuleFileNameW( GetModuleHandleW(L"userenv.dll"), path, ARRAY_SIZE(path) );
    trace("Testing userenv from %s\n", wine_dbgstr_w(path));
    sid = (PSID)0xdeadbeef;
    hr = derive( NULL, &sid );
    ok(hr == E_INVALIDARG && sid == (PSID)0xdeadbeef, "NULL name: %#lx, %p\n", hr, sid);
    hr = derive( L"", &sid );
    ok(hr == E_INVALIDARG && sid == (PSID)0xdeadbeef, "Empty name: %#lx, %p\n", hr, sid);
    hr = derive( L"test", NULL );
    ok(hr == E_INVALIDARG, "NULL output: %#lx\n", hr);
    for (i = 0; i < ARRAY_SIZE(cases); ++i)
    {
        winetest_push_context("case %u", i);
        sid = NULL;
        hr = derive( cases[i].name, &sid );
        ok(hr == S_OK && sid != NULL, "derive returned %#lx, %p\n", hr, sid);
        if (hr == S_OK && sid)
        {
            ok(IsValidSid(sid), "Invalid SID\n");
            ok(*GetSidSubAuthorityCount(sid) == 8, "Wrong subauthority count\n");
            ok(!memcmp(GetSidIdentifierAuthority(sid), &authority, sizeof(authority)), "Wrong authority\n");
            ok(*GetSidSubAuthority(sid, 0) == SECURITY_APP_PACKAGE_BASE_RID, "Wrong base RID\n");
            text = NULL;
            ret = ConvertSidToStringSidA( sid, &text );
            ok(ret, "ConvertSidToStringSidA failed: %lu\n", GetLastError());
            if (ret) ok(!strcmp(text, cases[i].sid), "SID %s != %s\n", text, cases[i].sid);
            LocalFree( text );
            again = NULL;
            hr = derive( cases[i].name, &again );
            ok(hr == S_OK && again != NULL, "Repeat failed: %#lx\n", hr);
            if (again) { ok(EqualSid(sid, again), "Repeat differs\n"); FreeSid(again); }
            ok(!FreeSid(sid), "FreeSid failed\n");
        }
        winetest_pop_context();
    }
    sid = other = NULL;
    hr = derive( L"test", &sid );
    ok(hr == S_OK, "test failed %#lx\n", hr);
    hr = derive( L"other", &other );
    ok(hr == S_OK, "other failed %#lx\n", hr);
    if (sid && other) ok(!EqualSid(sid, other), "Different names returned same SID\n");
    FreeSid(sid); FreeSid(other);
    for (length = 1; length <= 65; ++length)
    {
        if (length != 1 && length != 63 && length != 64 && length != 65) continue;
        for (i = 0; i < length; ++i) name[i] = 'A';
        name[length] = 0;
        sid = (PSID)0xdeadbeef;
        hr = derive( name, &sid );
        if (length == 65)
            ok(hr == E_INVALIDARG && sid == (PSID)0xdeadbeef, "Length65: %#lx, %p\n", hr, sid);
        else
        {
            ok(hr == S_OK && sid != (PSID)0xdeadbeef && sid, "Length%u: %#lx, %p\n", length, hr, sid);
            if (hr == S_OK && sid) FreeSid(sid);
        }
    }
}

static void test_appcontainer_profile(void)
{
    static const WCHAR test_name[] = L"WineExplorerTestContainer";
    static const WCHAR test_display[] = L"WineExplorer Test Container";
    static const WCHAR test_desc[] = L"Test container description";
    HRESULT (WINAPI *pCreateAppContainerProfile)(PCWSTR, PCWSTR, PCWSTR, SID_AND_ATTRIBUTES *, DWORD, PSID *);
    HRESULT (WINAPI *pDeleteAppContainerProfile)(PCWSTR);
    HRESULT (WINAPI *pGetAppContainerFolderPath)(PCWSTR, PWSTR *);
    WCHAR key_path[512];
    WCHAR moniker[256];
    WCHAR local_app_data[MAX_PATH];
    WCHAR expected_folder[MAX_PATH];
    PSID sid = NULL, sid2 = NULL;
    PWSTR folder_path = NULL;
    DWORD type, size, len;
    HKEY hkey;
    HRESULT hr;
    LSTATUS status;
    WCHAR *sid_str_w = NULL;
    DWORD attr;

    pCreateAppContainerProfile = (void *)GetProcAddress( GetModuleHandleW(L"userenv.dll"), "CreateAppContainerProfile" );
    pDeleteAppContainerProfile = (void *)GetProcAddress( GetModuleHandleW(L"userenv.dll"), "DeleteAppContainerProfile" );
    pGetAppContainerFolderPath = (void *)GetProcAddress( GetModuleHandleW(L"userenv.dll"), "GetAppContainerFolderPath" );

    if (!pCreateAppContainerProfile || !pDeleteAppContainerProfile)
    {
        win_skip("CreateAppContainerProfile / DeleteAppContainerProfile not available\n");
        return;
    }

    /* Clean up any leftovers from prior runs */
    pDeleteAppContainerProfile( test_name );

    /* Parameter validation */
    sid = (PSID)0xdeadbeef;
    hr = pCreateAppContainerProfile( NULL, test_display, test_desc, NULL, 0, &sid );
    ok(hr == E_INVALIDARG && sid == (PSID)0xdeadbeef, "NULL name: %#lx, %p\n", hr, sid);

    hr = pCreateAppContainerProfile( L"", test_display, test_desc, NULL, 0, &sid );
    ok(hr == E_INVALIDARG && sid == (PSID)0xdeadbeef, "empty name: %#lx, %p\n", hr, sid);

    hr = pCreateAppContainerProfile( test_name, test_display, test_desc, NULL, 0, NULL );
    ok(hr == E_INVALIDARG, "NULL sid out: %#lx\n", hr);

    /* Traversal and separator validation */
    hr = pCreateAppContainerProfile( L"../invalid", test_display, test_desc, NULL, 0, &sid );
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME), "expected ERROR_INVALID_NAME for ../, got %#lx\n", hr);

    hr = pCreateAppContainerProfile( L"invalid/name", test_display, test_desc, NULL, 0, &sid );
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME), "expected ERROR_INVALID_NAME for /, got %#lx\n", hr);

    hr = pCreateAppContainerProfile( L"invalid\\name", test_display, test_desc, NULL, 0, &sid );
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME), "expected ERROR_INVALID_NAME for \\, got %#lx\n", hr);

    hr = pDeleteAppContainerProfile( L"../invalid" );
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME), "expected ERROR_INVALID_NAME on delete ../, got %#lx\n", hr);

    /* Deleting non-existent profile returns S_OK */
    hr = pDeleteAppContainerProfile( L"NonExistentProfile_12345" );
    ok(hr == S_OK, "expected S_OK on delete nonexistent profile, got %#lx\n", hr);

    /* Valid creation */
    sid = NULL;
    hr = pCreateAppContainerProfile( test_name, test_display, test_desc, NULL, 0, &sid );
    ok(hr == S_OK && sid != NULL, "CreateAppContainerProfile failed: %#lx, %p\n", hr, sid);
    if (FAILED(hr) || !sid) return;

    ok(IsValidSid(sid), "Invalid SID returned\n");

    ConvertSidToStringSidW( sid, &sid_str_w );
    ok(sid_str_w != NULL, "ConvertSidToStringSidW failed\n");

    /* Verify registry mapping under HKCU\Software\Classes\Local Settings\... */
    swprintf( key_path, ARRAY_SIZE(key_path),
              L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings\\%s",
              sid_str_w );
    status = RegOpenKeyExW( HKEY_CURRENT_USER, key_path, 0, KEY_READ, &hkey );
    ok(status == ERROR_SUCCESS, "Registry mapping key not found: %ld\n", status);
    if (status == ERROR_SUCCESS)
    {
        size = sizeof(moniker);
        status = RegQueryValueExW( hkey, L"Moniker", NULL, &type, (BYTE *)moniker, &size );
        ok(status == ERROR_SUCCESS, "RegQueryValueExW(Moniker) failed: %ld\n", status);
        ok(!wcsicmp(moniker, test_name), "Moniker mismatch: %s != %s\n", wine_dbgstr_w(moniker), wine_dbgstr_w(test_name));
        RegCloseKey( hkey );
    }

    /* Verify folder creation on disk */
    len = GetEnvironmentVariableW( L"LOCALAPPDATA", local_app_data, ARRAY_SIZE(local_app_data) );
    if (len && len < ARRAY_SIZE(local_app_data))
    {
        swprintf( expected_folder, ARRAY_SIZE(expected_folder), L"%s\\Packages\\%s\\AC", local_app_data, test_name );
        attr = GetFileAttributesW( expected_folder );
        ok(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY),
           "Expected folder does not exist: %s (attr=%lx)\n", wine_dbgstr_w(expected_folder), attr);
    }

    /* Duplicate creation must fail with ERROR_ALREADY_EXISTS */
    sid2 = NULL;
    hr = pCreateAppContainerProfile( test_name, test_display, test_desc, NULL, 0, &sid2 );
    ok(hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "expected ERROR_ALREADY_EXISTS on duplicate, got %#lx\n", hr);
    if (sid2) FreeSid(sid2);

    /* Test GetAppContainerFolderPath */
    if (pGetAppContainerFolderPath)
    {
        folder_path = NULL;
        hr = pGetAppContainerFolderPath( NULL, &folder_path );
        ok(hr == E_INVALIDARG, "NULL sid to GetAppContainerFolderPath: %#lx\n", hr);

        hr = pGetAppContainerFolderPath( sid_str_w, NULL );
        ok(hr == E_INVALIDARG, "NULL path out to GetAppContainerFolderPath: %#lx\n", hr);

        hr = pGetAppContainerFolderPath( L"invalid_sid", &folder_path );
        ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_SID), "invalid SID: %#lx\n", hr);

        hr = pGetAppContainerFolderPath( L"S-1-15-2-999999-999999", &folder_path );
        ok(hr == HRESULT_FROM_WIN32(4250 /* ERROR_NOT_APPCONTAINER */), "nonexistent SID: %#lx\n", hr);

        folder_path = NULL;
        hr = pGetAppContainerFolderPath( sid_str_w, &folder_path );
        ok(hr == S_OK && folder_path != NULL, "GetAppContainerFolderPath failed: %#lx, %p\n", hr, folder_path);
        if (hr == S_OK && folder_path)
        {
            ok(!wcsicmp(folder_path, expected_folder), "Folder path mismatch: %s != %s\n",
               wine_dbgstr_w(folder_path), wine_dbgstr_w(expected_folder));
            CoTaskMemFree( folder_path );
        }

        /* Test GetAppContainerFolderPath with corrupt registry data */
        {
            static const WCHAR mock_sid[] = L"S-1-15-2-12345-67890-99999";
            WCHAR mock_key[512];
            HKEY hmock;
            BYTE corrupt_bytes[] = { 'b', 'a', 'd' }; /* odd length, not WCHAR aligned */
            WCHAR unterminated[] = { 'b', 'a', 'd', '!', 'x' }; /* not NUL-terminated */
            WCHAR traversal_moniker[] = L"..\\traversal";

            swprintf( mock_key, ARRAY_SIZE(mock_key),
                      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings\\%s",
                      mock_sid );

            if (RegCreateKeyExW( HKEY_CURRENT_USER, mock_key, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hmock, NULL ) == ERROR_SUCCESS)
            {
                /* Odd byte count / unaligned */
                RegSetValueExW( hmock, L"Moniker", 0, REG_SZ, corrupt_bytes, sizeof(corrupt_bytes) );
                folder_path = NULL;
                hr = pGetAppContainerFolderPath( mock_sid, &folder_path );
                ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "expected ERROR_INVALID_DATA for odd bytes, got %#lx\n", hr);

                /* Corrupt: REG_BINARY instead of REG_SZ */
                RegSetValueExW( hmock, L"Moniker", 0, REG_BINARY, corrupt_bytes, 2 );
                folder_path = NULL;
                hr = pGetAppContainerFolderPath( mock_sid, &folder_path );
                ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "expected ERROR_INVALID_DATA for REG_BINARY, got %#lx\n", hr);

                /* Corrupt: unterminated string */
                RegSetValueExW( hmock, L"Moniker", 0, REG_SZ, (BYTE *)unterminated, 4 * sizeof(WCHAR) );
                folder_path = NULL;
                hr = pGetAppContainerFolderPath( mock_sid, &folder_path );
                ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "expected ERROR_INVALID_DATA for unterminated, got %#lx\n", hr);

                /* Traversal in stored moniker */
                RegSetValueExW( hmock, L"Moniker", 0, REG_SZ, (BYTE *)traversal_moniker, (wcslen(traversal_moniker) + 1) * sizeof(WCHAR) );
                folder_path = NULL;
                hr = pGetAppContainerFolderPath( mock_sid, &folder_path );
                ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME), "expected ERROR_INVALID_NAME for traversal moniker, got %#lx\n", hr);

                RegCloseKey( hmock );
                RegDeleteKeyW( HKEY_CURRENT_USER, mock_key );
            }
        }
    }
    else
    {
        win_skip("GetAppContainerFolderPath not available\n");
    }

    /* Test DeleteAppContainerProfile */
    hr = pDeleteAppContainerProfile( NULL );
    ok(hr == E_INVALIDARG, "NULL name to DeleteAppContainerProfile: %#lx\n", hr);

    hr = pDeleteAppContainerProfile( test_name );
    ok(hr == S_OK, "DeleteAppContainerProfile failed: %#lx\n", hr);

    /* Verify registry mapping removed */
    status = RegOpenKeyExW( HKEY_CURRENT_USER, key_path, 0, KEY_READ, &hkey );
    ok(status == ERROR_FILE_NOT_FOUND, "Registry mapping should be deleted: %ld\n", status);
    if (status == ERROR_SUCCESS) RegCloseKey( hkey );

    LocalFree( sid_str_w );
    FreeSid( sid );

    /* P1: Deleting a missing profile must preserve unowned directories and sentinel files on disk */
    {
        static const WCHAR unreg_name[] = L"WineExplorerUnregTest_12345";
        WCHAR unreg_pkg[MAX_PATH], unreg_ac[MAX_PATH], unreg_sentinel[MAX_PATH];
        HANDLE hfile;

        pDeleteAppContainerProfile( unreg_name );

        swprintf( unreg_pkg, ARRAY_SIZE(unreg_pkg), L"%s\\Packages\\%s", local_app_data, unreg_name );
        swprintf( unreg_ac, ARRAY_SIZE(unreg_ac), L"%s\\Packages\\%s\\AC", local_app_data, unreg_name );
        swprintf( unreg_sentinel, ARRAY_SIZE(unreg_sentinel), L"%s\\Packages\\%s\\sentinel.txt", local_app_data, unreg_name );

        CreateDirectoryW( unreg_pkg, NULL );
        CreateDirectoryW( unreg_ac, NULL );
        hfile = CreateFileW( unreg_sentinel, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        ok(hfile != INVALID_HANDLE_VALUE, "Failed to create sentinel file: %lu\n", GetLastError());
        if (hfile != INVALID_HANDLE_VALUE) CloseHandle( hfile );

        hr = pDeleteAppContainerProfile( unreg_name );
        ok(hr == S_OK, "DeleteAppContainerProfile missing profile failed: %#lx\n", hr);

        attr = GetFileAttributesW( unreg_pkg );
        ok(attr != INVALID_FILE_ATTRIBUTES || broken(attr == INVALID_FILE_ATTRIBUTES /* win8+ */),
           "Unowned profile directory should be preserved: %s (attr=%lu)\n", wine_dbgstr_w(unreg_pkg), attr);

        attr = GetFileAttributesW( unreg_ac );
        ok(attr != INVALID_FILE_ATTRIBUTES || broken(attr == INVALID_FILE_ATTRIBUTES /* win8+ */),
           "Unowned empty AC directory should be preserved: %s (attr=%lu)\n", wine_dbgstr_w(unreg_ac), attr);

        attr = GetFileAttributesW( unreg_sentinel );
        ok(attr != INVALID_FILE_ATTRIBUTES || broken(attr == INVALID_FILE_ATTRIBUTES /* win8+ */),
           "Unowned sentinel file should be preserved: %s (attr=%lu)\n", wine_dbgstr_w(unreg_sentinel), attr);

        /* Cleanup test files */
        DeleteFileW( unreg_sentinel );
        RemoveDirectoryW( unreg_ac );
        RemoveDirectoryW( unreg_pkg );
    }

    /* P2: CreateAppContainerProfile with file collision at AC directory */
    {
        static const WCHAR collision_name[] = L"WineExplorerFileCollTest_12345";
        WCHAR coll_pkg[MAX_PATH], coll_ac[MAX_PATH], coll_key[512];
        HRESULT (WINAPI *pDerive)(PCWSTR, PSID *);
        PSID sid_coll = NULL, derived_sid = NULL;
        WCHAR *derived_sid_str = NULL;
        HANDLE hfile;

        pDeleteAppContainerProfile( collision_name );

        swprintf( coll_pkg, ARRAY_SIZE(coll_pkg), L"%s\\Packages\\%s", local_app_data, collision_name );
        swprintf( coll_ac, ARRAY_SIZE(coll_ac), L"%s\\Packages\\%s\\AC", local_app_data, collision_name );

        CreateDirectoryW( coll_pkg, NULL );
        hfile = CreateFileW( coll_ac, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        ok(hfile != INVALID_HANDLE_VALUE, "Failed to create collision file: %lu\n", GetLastError());
        if (hfile != INVALID_HANDLE_VALUE) CloseHandle( hfile );

        hr = pCreateAppContainerProfile( collision_name, test_display, test_desc, NULL, 0, &sid_coll );
        ok(FAILED(hr) || broken(hr == S_OK /* win8+ replaces file */),
           "CreateAppContainerProfile should fail on file collision: %#lx\n", hr);
        if (SUCCEEDED(hr) && sid_coll) FreeSid( sid_coll );

        /* File must be preserved, not converted to directory or deleted */
        attr = GetFileAttributesW( coll_ac );
        ok((attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) ||
           broken(attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) /* win8+ */),
           "Collision file should remain a regular file: %s (attr=%lu)\n", wine_dbgstr_w(coll_ac), attr);

        /* Verify registration was rolled back / not present */
        pDerive = (void *)GetProcAddress( GetModuleHandleW(L"userenv.dll"), "DeriveAppContainerSidFromAppContainerName" );
        if (pDerive && pDerive( collision_name, &derived_sid ) == S_OK)
        {
            if (ConvertSidToStringSidW( derived_sid, &derived_sid_str ))
            {
                swprintf( coll_key, ARRAY_SIZE(coll_key),
                          L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings\\%s",
                          derived_sid_str );
                status = RegOpenKeyExW( HKEY_CURRENT_USER, coll_key, 0, KEY_READ, &hkey );
                ok(status == ERROR_FILE_NOT_FOUND || broken(status == ERROR_SUCCESS /* win8+ */),
                   "Registration should be rolled back on failure: %ld\n", status);
                if (status == ERROR_SUCCESS) RegCloseKey( hkey );
                LocalFree( derived_sid_str );
            }
            FreeSid( derived_sid );
        }

        /* Cleanup */
        pDeleteAppContainerProfile( collision_name );
        DeleteFileW( coll_ac );
        RemoveDirectoryW( coll_pkg );
    }
}

START_TEST(userenv)
{
    pIsWow64Process = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");

    test_create_env();
    test_get_profiles_dir();
    test_get_user_profile_dir();
    test_appcontainer_sid();
    test_appcontainer_profile();
}
