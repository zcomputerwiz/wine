/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * Private E1-S10 byte protocol. No native structures cross this interface.
 */
#ifndef __WINE_WEX_TOKEN_H
#define __WINE_WEX_TOKEN_H

#define WEX_TOKEN_NEGOTIATE 1
#define WEX_TOKEN_ISSUE 2
/* Synthetic consumed-record installer, only in WEX_E1S10_TOKEN_TEST servers. */
#define WEX_TOKEN_TEST_INSTALL 0x80000001u
#define WEX_TOKEN_CAPS 0xc0u /* ACCOUNT_MAP_V1 | TOKEN_V1, isolated lab link only */
#define WEX_TOKEN_REQUEST_SIZE 188
#define WEX_TOKEN_INPUT_SIZE 204 /* kind 4 followed by connection-local evidence_ref */
#define WEX_TOKEN_RESPONSE_SIZE 136 /* kind 5 with the S4 28-byte SID */
#define WEX_TOKEN_FAILURE_SIZE 108 /* kind 5 with an empty SID blob */
#define WEX_TOKEN_FIXTURE_SIZE 268 /* input plus zero-padded canonical name[64] */

static inline unsigned int wex_token_u32( const unsigned char *p )
{
    return p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

static inline unsigned long long wex_token_u64( const unsigned char *p )
{
    return wex_token_u32( p ) | (unsigned long long)wex_token_u32( p + 4 ) << 32;
}

static inline void wex_token_put32( unsigned char *p, unsigned int n )
{
    unsigned int i;
    for (i = 0; i < 4; i++) p[i] = n >> (8 * i);
}

static inline void wex_token_put64( unsigned char *p, unsigned long long n )
{
    wex_token_put32( p, n );
    wex_token_put32( p + 4, n >> 32 );
}

#endif
