/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 */
#ifndef __WINE_WEX_PROFILE_H
#define __WINE_WEX_PROFILE_H
#include "wex_token.h"
/* Private S13, not S1 kind 6. LE: version:u32=1, size:u32=48,
 * S4 account_key[16], map_generation:u64, S12 storage_generation:u64,
 * reserved:u64=0. No SID, name, path, prefix or transferable lease. */
#define WEX_HKU_RECORD_SIZE 48
#define WEX_HKU_LOAD 1
#define WEX_HKU_QUERY 2
#define WEX_HKU_OPEN 3
#define WEX_HKU_UNLOAD_S14 4
#define WEX_HKU_CURRENT 5
#define WEX_HKU_CLASSES 6
#define WEX_HKU_CACHE_CURRENT 7
#define WEX_HKU_CACHE_CLASSES 8
#define WEX_HKU_TEST_CRASH 9 /* separate WEX_E1S14_CRASH_TEST, lab broker only */
#define WEX_HKU_TEST_TRANSFER 10 /* retired lab opcode; always denies, never reuse */
#define WEX_HKU_COMMIT_PROCESS 11
/* COMMIT: context is the retained S10 context used by LOAD, token is the exact
 * issued token, access=0, record is a LE u32 process handle in this broker's
 * handle table. No client-supplied identity or lease record is authority.
 * Success: no key, managed=1, generation=0. Request 325 layout is unchanged. */
#define WEX_HKU_COMMIT_RECORD_SIZE 4
#endif
