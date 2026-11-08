/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * Run on the NORMAL server in both architectures. Every operation denies.
 */
#include <windows.h>
#include <winternl.h>
#include <string.h>
#include "wine/test.h"
#include "wine/wex_profile.h"

typedef NTSTATUS (WINAPI *load_fn)(ULONG,HANDLE,HANDLE,ACCESS_MASK,const void*,ULONG,HANDLE*,ULONG*);
typedef NTSTATUS (WINAPI *load_ex_fn)(ULONG,HANDLE,HANDLE,ACCESS_MASK,const void*,ULONG,HANDLE*,ULONG*,ULONGLONG*);

START_TEST(wex_hku)
{
    load_fn load = (load_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtLoadWexProfile");
    load_ex_fn load_ex = (load_ex_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtLoadWexProfileEx");
    HANDLE token = NULL, key;
    BYTE record[WEX_HKU_RECORD_SIZE] = {0};
    ULONG op, managed;
    NTSTATUS status;
    ULONGLONG generation;
    if (!load) { win_skip("WineExplorer private profile export absent\n"); return; }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    wex_token_put32(record, 1); wex_token_put32(record + 4, sizeof(record));
    memset(record + 8, 0x11, 16); wex_token_put64(record + 24, 1); wex_token_put64(record + 32, 1);
    ok(load_ex != NULL, "S14 generation export missing\n");
    for (op = WEX_HKU_LOAD; op <= WEX_HKU_STORAGE_QUERY; op++)
    {
        key = (HANDLE)0xdeadbeef; managed = ~0u;
        status = load(op, op == WEX_HKU_LOAD ? (HANDLE)0x1234 : NULL, token,
                      op == WEX_HKU_QUERY ? 0 : KEY_READ,
                      op == WEX_HKU_LOAD ? record : NULL, op == WEX_HKU_LOAD ? sizeof(record) : 0,
                      &key, &managed);
        ok(status == (NTSTATUS)0xc00000bb, "normal operation %lu: %#lx\n", op, status);
        ok(!key && !managed, "operation %lu exposed output %p/%lu\n", op, key, managed);
        if (key) CloseHandle(key);
        if (load_ex)
        {
            key = (HANDLE)0xdeadbeef; managed = ~0u; generation = ~(ULONGLONG)0;
            status = load_ex(op,NULL,token,0,NULL,0,&key,&managed,&generation);
            ok(status == (NTSTATUS)0xc00000bb, "normal extended operation %lu: %#lx\n", op, status);
            ok(!key && !managed && !generation, "extended operation %lu leaked outputs\n", op);
            if (key) CloseHandle(key);
        }
    }
    /* S14b production commit has a valid-sized local process-handle payload.
     * A normal server still denies before interpreting context or token. */
    wex_token_put32(record, (ULONG)(ULONG_PTR)GetCurrentProcess());
    key = (HANDLE)0xdeadbeef; managed = ~0u; generation = ~(ULONGLONG)0;
    status = load_ex ? load_ex(WEX_HKU_COMMIT_PROCESS, (HANDLE)0x1234, token, 0,
                              record, WEX_HKU_COMMIT_RECORD_SIZE, &key, &managed, &generation) : 0;
    if (load_ex)
    {
        ok(status == (NTSTATUS)0xc00000bb, "normal commit: %#lx\n", status);
        ok(!key && !managed && !generation, "normal commit exposed outputs\n");
        if (key) CloseHandle(key);
    }
    key = (HANDLE)0xdeadbeef; managed = ~0u;
    status = load(WEX_HKU_LOAD, NULL, token, KEY_READ, record, sizeof(record) - 1, &key, &managed);
    ok(status != 0 && !key && !managed, "short record accepted\n");
    key = (HANDLE)0xdeadbeef; managed = ~0u;
    status = load(WEX_HKU_LOAD, NULL, token, KEY_READ, record, sizeof(record) + 1, &key, &managed);
    ok(status == (NTSTATUS)0xc000000d && !key && !managed, "oversize API accepted\n");

    /* S15 normal server checks: ordinary process behavior is preserved */
    {
        typedef BOOL (WINAPI *get_prof_fn)(HANDLE, LPWSTR, LPDWORD);
        typedef BOOL (WINAPI *create_env_fn)(LPVOID*, HANDLE, BOOL);
        typedef BOOL (WINAPI *destroy_env_fn)(LPVOID);
        typedef BOOL (WINAPI *expand_env_fn)(HANDLE, LPCWSTR, LPWSTR, DWORD);

        HMODULE hUserenv = LoadLibraryA("userenv.dll");
        if (hUserenv)
        {
            get_prof_fn pGetUserProfileDirectoryW = (get_prof_fn)GetProcAddress(hUserenv, "GetUserProfileDirectoryW");
            create_env_fn pCreateEnvironmentBlock = (create_env_fn)GetProcAddress(hUserenv, "CreateEnvironmentBlock");
            destroy_env_fn pDestroyEnvironmentBlock = (destroy_env_fn)GetProcAddress(hUserenv, "DestroyEnvironmentBlock");
            expand_env_fn pExpandEnvironmentStringsForUserW = (expand_env_fn)GetProcAddress(hUserenv, "ExpandEnvironmentStringsForUserW");

            if (pGetUserProfileDirectoryW)
            {
                WCHAR dir[MAX_PATH];
                DWORD size = ARRAY_SIZE(dir);
                BOOL ret = pGetUserProfileDirectoryW(token, dir, &size);
                ok(ret, "GetUserProfileDirectoryW failed on normal token: %lu\n", GetLastError());
                ok(wcsstr(dir, L".e1s") == NULL, "normal token saw managed .e1s directory: %s\n", wine_dbgstr_w(dir));
            }

            if (pCreateEnvironmentBlock && pDestroyEnvironmentBlock)
            {
                LPVOID env = NULL;
                BOOL ret = pCreateEnvironmentBlock(&env, token, FALSE);
                ok(ret, "CreateEnvironmentBlock failed on normal token: %lu\n", GetLastError());
                if (ret && env)
                    pDestroyEnvironmentBlock(env);
            }

            if (pExpandEnvironmentStringsForUserW)
            {
                WCHAR exp[MAX_PATH];
                BOOL ret = pExpandEnvironmentStringsForUserW(token, L"%USERPROFILE%", exp, ARRAY_SIZE(exp));
                ok(ret, "ExpandEnvironmentStringsForUserW failed on normal token: %lu\n", GetLastError());
                ok(wcsstr(exp, L".e1s") == NULL, "normal token expanded .e1s directory: %s\n", wine_dbgstr_w(exp));
            }

            FreeLibrary(hUserenv);
        }
    }

    CloseHandle(token);
}
