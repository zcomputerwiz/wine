/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 */
#ifndef WEX_ACCOUNTS_H
#define WEX_ACCOUNTS_H

#include <stdint.h>
#include <sys/types.h>

#define WEX_ACCOUNT_MAP_V1 (UINT64_C(1) << 6)
#define WEX_ACCOUNT_NAME_MAX 63

/* Private provider statuses, not public authentication errors or NTSTATUS. */
enum wex_account_status
{
    WEX_ACCOUNT_OK,
    WEX_ACCOUNT_UNKNOWN,
    WEX_ACCOUNT_TOMBSTONED,
    WEX_ACCOUNT_NSS_MISMATCH,
    WEX_ACCOUNT_SID_COLLISION,
    WEX_ACCOUNT_MAP_UNREADABLE,
    WEX_ACCOUNT_MAP_UNPROTECTED,
    WEX_ACCOUNT_MAP_INVALID,
    WEX_ACCOUNT_ADMIN_GROUP_MISSING,
    WEX_ACCOUNT_NSS_ERROR,
    WEX_ACCOUNT_UNSUPPORTED,
    WEX_ACCOUNT_LEGACY_CONFLICT,
    WEX_ACCOUNT_CAPABILITY_MISSING
};

struct wex_account
{
    char name[WEX_ACCOUNT_NAME_MAX + 1];
    uid_t uid;
    gid_t primary_gid;
    unsigned char incarnation[16];
    /* Binary SID S-1-5-21-a-b-c-rid: revision=1, authority=5, count=5.
     * These are host-order subauthorities; no native structure is a wire record. */
    uint32_t sid[5];
    uint64_t generation;
    int administrator;
};

/* name != NULL selects a canonical name; otherwise uid selects the account.
 * No cache. Zeroes result on every failure. Call only on an authenticated,
 * negotiated internal link; passing a mask is not endpoint authentication.
 * No production caller or capability advertisement is installed by S4. */
enum wex_account_status lookup_account( const char *name, uid_t uid, uint64_t negotiated_caps,
                                        struct wex_account *result );

/* System tier T4-0: the mapped SID (subauthorities after S-1-5) of the local account with this uid */
int accounts_own_sid( uid_t uid, unsigned int subauth[5] );

#ifdef WEX_E1S6_TRANSPORT_TEST
int accounts_get_map_generation( unsigned long long *gen );
#endif

#endif
