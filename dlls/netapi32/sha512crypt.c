/*
 * WineExplorer: crypt(3) SHA-512 hashes ("$6$salt$hash") made in the client
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
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

/* The password of a new or changed local account never leaves the process as text (docs/LOCAL_USERS_DESIGN.md, U2):
 * the client sends the account helper this hash, and the helper only checks its form. The scheme is the published
 * SHA-512 crypt of the crypt(3) family, 5000 rounds; its text description is public and the code below is written
 * from that description. The one-way function is BCrypt's SHA-512, applied to a whole message at a time. */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "bcrypt.h"
#include "wexaccounts.h"

#define SHA512_LEN   64
#define SALT_LEN     16
#define ROUNDS       5000
#define MAX_KEY      1024    /* UTF-8 bytes of a password: 256 characters at most, 3 bytes each, with room */

static const char itoa64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static BOOL sha512( const BYTE *data, ULONG len, BYTE out[SHA512_LEN] )
{
    return BCryptHash( BCRYPT_SHA512_ALG_HANDLE, NULL, 0, (BYTE *)data, len, out, SHA512_LEN ) == 0;
}

static char *put64( char *p, BYTE b2, BYTE b1, BYTE b0, int n )
{
    unsigned int w = (b2 << 16) | (b1 << 8) | b0;

    while (n-- > 0)
    {
        *p++ = itoa64[w & 0x3f];
        w >>= 6;
    }
    return p;
}

/* A repeated copy of a 64 byte digest, cut to len bytes */
static void repeat_digest( BYTE *out, const BYTE digest[SHA512_LEN], size_t len )
{
    size_t i;

    for (i = 0; i < len; i++) out[i] = digest[i % SHA512_LEN];
}

BOOL wexa_sha512_crypt( const char *key, const char *salt_in, char out[WEXA_HASH_MAX] )
{
    BYTE *buf, A[SHA512_LEN], B[SHA512_LEN], C[SHA512_LEN], DP[SHA512_LEN], DS[SHA512_LEN], P[MAX_KEY], S[SALT_LEN];
    char salt[SALT_LEN + 1];
    size_t key_len = strlen( key ), salt_len = 0, n, i, pos;
    char *p;

    while (salt_in[salt_len] && salt_in[salt_len] != '$' && salt_len < SALT_LEN)
    {
        salt[salt_len] = salt_in[salt_len];
        salt_len++;
    }
    salt[salt_len] = 0;
    if (key_len > MAX_KEY) return FALSE;
    /* the longest message is H(the key, key_len times) for P; the others are shorter */
    n = key_len * key_len;
    if (n < 16384) n = 16384;
    if (!(buf = malloc( n + 4096 ))) return FALSE;

    /* B = H(key salt key) */
    pos = 0;
    memcpy( buf + pos, key, key_len ); pos += key_len;
    memcpy( buf + pos, salt, salt_len ); pos += salt_len;
    memcpy( buf + pos, key, key_len ); pos += key_len;
    if (!sha512( buf, pos, B )) goto fail;

    /* A = H(key salt B as long as the key, then B or the key for each bit of the key's length) */
    pos = 0;
    memcpy( buf + pos, key, key_len ); pos += key_len;
    memcpy( buf + pos, salt, salt_len ); pos += salt_len;
    repeat_digest( buf + pos, B, key_len ); pos += key_len;
    for (n = key_len; n > 0; n >>= 1)
    {
        if (n & 1) { memcpy( buf + pos, B, SHA512_LEN ); pos += SHA512_LEN; }
        else { memcpy( buf + pos, key, key_len ); pos += key_len; }
    }
    if (!sha512( buf, pos, A )) goto fail;

    /* P: H(the key, key_len times) repeated to the key's length */
    pos = 0;
    for (i = 0; i < key_len; i++) { memcpy( buf + pos, key, key_len ); pos += key_len; }
    if (!sha512( buf, pos, DP )) goto fail;
    repeat_digest( P, DP, key_len );

    /* S: H(the salt, 16 + A[0] times) repeated to the salt's length */
    pos = 0;
    for (i = 0; i < (size_t)16 + A[0]; i++) { memcpy( buf + pos, salt, salt_len ); pos += salt_len; }
    if (!sha512( buf, pos, DS )) goto fail;
    repeat_digest( S, DS, salt_len );

    memcpy( C, A, SHA512_LEN );
    for (i = 0; i < ROUNDS; i++)
    {
        pos = 0;
        if (i & 1) { memcpy( buf + pos, P, key_len ); pos += key_len; }
        else { memcpy( buf + pos, C, SHA512_LEN ); pos += SHA512_LEN; }
        if (i % 3) { memcpy( buf + pos, S, salt_len ); pos += salt_len; }
        if (i % 7) { memcpy( buf + pos, P, key_len ); pos += key_len; }
        if (i & 1) { memcpy( buf + pos, C, SHA512_LEN ); pos += SHA512_LEN; }
        else { memcpy( buf + pos, P, key_len ); pos += key_len; }
        if (!sha512( buf, pos, C )) goto fail;
    }

    p = out;
    memcpy( p, "$6$", 3 ); p += 3;
    memcpy( p, salt, salt_len ); p += salt_len;
    *p++ = '$';
    p = put64( p, C[0], C[21], C[42], 4 );
    p = put64( p, C[22], C[43], C[1], 4 );
    p = put64( p, C[44], C[2], C[23], 4 );
    p = put64( p, C[3], C[24], C[45], 4 );
    p = put64( p, C[25], C[46], C[4], 4 );
    p = put64( p, C[47], C[5], C[26], 4 );
    p = put64( p, C[6], C[27], C[48], 4 );
    p = put64( p, C[28], C[49], C[7], 4 );
    p = put64( p, C[50], C[8], C[29], 4 );
    p = put64( p, C[9], C[30], C[51], 4 );
    p = put64( p, C[31], C[52], C[10], 4 );
    p = put64( p, C[53], C[11], C[32], 4 );
    p = put64( p, C[12], C[33], C[54], 4 );
    p = put64( p, C[34], C[55], C[13], 4 );
    p = put64( p, C[56], C[14], C[35], 4 );
    p = put64( p, C[15], C[36], C[57], 4 );
    p = put64( p, C[37], C[58], C[16], 4 );
    p = put64( p, C[59], C[17], C[38], 4 );
    p = put64( p, C[18], C[39], C[60], 4 );
    p = put64( p, C[40], C[61], C[19], 4 );
    p = put64( p, C[62], C[20], C[41], 4 );
    p = put64( p, 0, 0, C[63], 2 );
    *p = 0;
    free( buf );
    return TRUE;

fail:
    free( buf );
    return FALSE;
}

/* A new hash of a password with a random 16 character salt */
BOOL wexa_hash_password( const char *password, char out[WEXA_HASH_MAX] )
{
    char salt[SALT_LEN + 1];
    BYTE rnd[SALT_LEN];
    int i;

    if (BCryptGenRandom( NULL, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG )) return FALSE;
    for (i = 0; i < SALT_LEN; i++) salt[i] = itoa64[rnd[i] & 0x3f];
    salt[SALT_LEN] = 0;
    return wexa_sha512_crypt( password, salt, out );
}
