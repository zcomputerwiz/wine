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
    for (op = WEX_HKU_LOAD; op <= WEX_HKU_TEST_TRANSFER; op++)
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
    key = (HANDLE)0xdeadbeef; managed = ~0u;
    status = load(WEX_HKU_LOAD, NULL, token, KEY_READ, record, sizeof(record) - 1, &key, &managed);
    ok(status != 0 && !key && !managed, "short record accepted\n");
    key = (HANDLE)0xdeadbeef; managed = ~0u;
    status = load(WEX_HKU_LOAD, NULL, token, KEY_READ, record, sizeof(record) + 1, &key, &managed);
    ok(status == (NTSTATUS)0xc000000d && !key && !managed, "oversize API accepted\n");
    CloseHandle(token);
}
