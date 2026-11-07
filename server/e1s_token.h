/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 */
#ifndef __WINE_SERVER_E1S_TOKEN_H
#define __WINE_SERVER_E1S_TOKEN_H

#include "wine/wex_token.h"

/* Server-internal S18 handoff, never a Wine client structure or IPC operation.
 * The producer has redeemed S8 on its owning authenticated connection, matched
 * every evidence field to the frozen S5 request and current authorities, and
 * serialized cancellation with consumption. Retain the original S3 objects,
 * not objects reopened from a PID. No serialized 'authenticated' flag exists.
 * input = exact kind 4 + original S8 evidence_ref; expiry = original absolute
 * host CLOCK_MONOTONIC ns deadline, never a newly computed interval.
 * validate rechecks current root/server/broker/map/policy and endpoint ownership,
 * caller/writer/primary versions, request/image, deadline, route and cancellation.
 * It is trusted server code, not a callback nominated by a Wine client.
 * It must be bounded and nonblocking on the server request thread.
 * On success the context owns cookie and invokes release once at destruction.
 * On failure the producer retains cookie ownership. Closing native authority
 * must make validate fail before releasing pins; no numeric grant survives it.
 */
struct e1s_token_consumed
{
    unsigned char input[WEX_TOKEN_INPUT_SIZE];
    const char *canonical_name;
    struct process *caller;
    struct token *primary;
    unsigned long long expiry;
    int (*validate)( void *cookie, const unsigned char *input );
    void (*release)( void *cookie );
    void *cookie;
};

/* One call attempt per negotiation. Returns NTSTATUS via get_error(). Normal
 * builds refuse; no S18 producer or production negotiation is installed here. */
extern int token_install_e1s_consumed( obj_handle_t context, const struct e1s_token_consumed *record );

#endif
