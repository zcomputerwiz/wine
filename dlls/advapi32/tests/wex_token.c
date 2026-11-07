/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * WineExplorer private token protocol: ordinary clients never get a context.
 */
#include <windows.h>
#include <string.h>
#include <winternl.h>
#include "wine/test.h"
#include "wine/wex_token.h"

typedef NTSTATUS (WINAPI *create_fn)(ULONG,HANDLE*,HANDLE,ULONGLONG*,const void*,ULONG,HANDLE*,void*,ULONG*);

START_TEST(wex_token)
{
    create_fn create = (create_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateWexAccountToken");
    HANDLE context = NULL, token = (HANDLE)0xdeadbeef;
    ULONGLONG caps = WEX_TOKEN_CAPS;
    ULONG size = 0, i;
    NTSTATUS status;
    BYTE input[WEX_TOKEN_INPUT_SIZE] = {0}, output[WEX_TOKEN_RESPONSE_SIZE];

    if (!create) { win_skip("WineExplorer private token export absent\n"); return; }
    status = create(WEX_TOKEN_NEGOTIATE, &context, NULL, &caps, NULL, 0, &token, NULL, &size);
    ok(status == (NTSTATUS)0xc0000022 || status == (NTSTATUS)0xc00000bb,
       "ordinary process negotiated: %#lx\n", status);
    ok(!context && !token && !caps && !size, "failed negotiation exposed output\n");
    if (context) CloseHandle(context);
    if (token) CloseHandle(token);

    /* A mask in serialized bytes is not a negotiated connection. The bogus
     * handle goes through the server, while no installed record can exist. */
    context = (HANDLE)0x1234;
    caps = WEX_TOKEN_CAPS;
    wex_token_put32(input, 0x00010004);
    wex_token_put32(input + 4, WEX_TOKEN_REQUEST_SIZE);
    wex_token_put64(input + 8, caps);
    memset(output, 0xcc, sizeof(output));
    size = sizeof(output); token = (HANDLE)0xdeadbeef;
    status = create(WEX_TOKEN_ISSUE, &context, NULL, &caps, input, sizeof(input), &token, output, &size);
    ok(status != 0 && !token, "unnegotiated token: %#lx %p\n", status, token);
    ok(size == WEX_TOKEN_FAILURE_SIZE && wex_token_u32(output) == 0x00010005 &&
       wex_token_u32(output + 4) == WEX_TOKEN_FAILURE_SIZE && wex_token_u32(output + 32) == (ULONG)status,
       "invalid failure record\n");
    for (i = 8; i < sizeof(output); i++)
        if (i < 32 || i >= 36) ok(!output[i], "failure byte %lu leaked: %x\n", i, output[i]);
    if (token) CloseHandle(token);
}
