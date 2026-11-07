/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 */
#ifndef WEX_PROFILE_H
#define WEX_PROFILE_H

#include "accounts.h"

enum wex_profile_status
{
    WEX_PROFILE_OK, WEX_PROFILE_DISABLED, WEX_PROFILE_COMPONENT,
    WEX_PROFILE_MAPPING, WEX_PROFILE_SUBSTITUTION, WEX_PROFILE_OWNER,
    WEX_PROFILE_MODE, WEX_PROFILE_LEGACY, WEX_PROFILE_IDENTITY,
    WEX_PROFILE_BUSY, WEX_PROFILE_SEED, WEX_PROFILE_IO,
    WEX_PROFILE_INCOMPLETE, WEX_PROFILE_LIMIT, WEX_PROFILE_FILESYSTEM, WEX_PROFILE_ABSENT
};

struct wex_profile_layout
{
    char sid[64], incarnation[33], leaf[97];
    char relative[256], windows[256], hive[256];
};

/* Reserved S1 kind-6 fields, native memory only, NOT wire serialization.
 * All remain zero in S12. S14 alone may allocate leases and set state 1..3.
 * S13 must apply/query ACL intent before exposing any Windows handle/path.
 */
struct wex_profile_lease_reserved
{
    unsigned char lease_id[16];
    uint64_t request_id, server_gen;
    unsigned char prefix_id[16], account_key[16], user_sid[28];
    uint64_t map_gen, profile_generation;
    uint32_t state;
};

struct wex_profile_storage
{
    struct wex_profile_layout path;
    int profile_fd, hive_fd; /* owned by successful recipient; close both */
    uint64_t profile_generation;
    int reused, acl_pending; /* acl_pending is ALWAYS 1, never launch readiness */
    char acl_intent[320];
    struct wex_profile_lease_reserved reserved;
};

/* Canonical trusted S4 snapshot only; no typed username or prefix metadata
 * constitutes account authority. Layout is a candidate until prepare returns
 * the stored label (an explicit S4 rename retains SID/incarnation/storage).
 * Every failure clears output; failed storage fds are -1.
 */
enum wex_profile_status wex_profile_layout( const struct wex_account *, struct wex_profile_layout * );

/* Internal disabled helper API; no Wine request/export, service or installer.
 * prefix_fd is the authority's pinned Bob prefix, with expected device/inode.
 * seed_fd must be opened from the root-protected OFFLINE skel user.reg by
 * the trusted caller, never Bob's user.reg or a machine/shared live hive.
 * The library validates the descriptors; the caller owns provenance, S4/S10
 * and generation checks. Run permanently as Bob, without supplementary groups
 * or elevated host capabilities; never perform prefix mutation as root.
 */
enum wex_profile_status wex_profile_prepare( int prefix_fd, dev_t, ino_t,
    uid_t bob, gid_t bob_group, int seed_fd, const struct wex_account *,
    struct wex_profile_storage * );

/* S13 no-create reopen. Storage generation is a consistency reference, not a lease. */
enum wex_profile_status wex_profile_open_existing( int prefix_fd, dev_t, ino_t,
    uid_t, gid_t, const struct wex_account *, uint64_t, struct wex_profile_storage * );
enum wex_profile_status wex_profile_verify_existing( int prefix_fd, const struct wex_account *,
                                                     const struct wex_profile_storage * );
#endif
