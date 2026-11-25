/*
 * WineExplorer: the local accounts of the machine, through the accounts service of session 0
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

#ifndef __WINE_NETAPI32_WEXACCOUNTS_H
#define __WINE_NETAPI32_WEXACCOUNTS_H

#include "lm.h"

/* On a machine that has a session 0 the NetUser* and NetLocalGroup* functions on the local computer are answered by the
 * accounts service there (docs/LOCAL_USERS_DESIGN.md); where there is none, Wine's own behaviour stays. The wire is the
 * one of src/include/wexaccounts_proto.h of the WineExplorer tree (a copy of what the client needs is in wexaccounts.c). */

#define WEXA_HASH_MAX  128   /* "$6$" + 16 + "$" + 86 + NUL, with room */

/* the client's hash of a password (U2): the text of a SHA-512 crypt hash; salt_in's characters up to a '$' (16 at most) */
BOOL wexa_sha512_crypt( const char *key, const char *salt_in, char out[WEXA_HASH_MAX] );
BOOL wexa_hash_password( const char *password, char out[WEXA_HASH_MAX] );

/* TRUE when this process asks the accounts service (a session 0 exists, and WEX_NETAPI_STOCK is not set) */
BOOL wexa_active(void);

NET_API_STATUS wexa_user_enum( DWORD level, DWORD filter, BYTE **bufptr, DWORD prefmaxlen, DWORD *entriesread,
                               DWORD *totalentries, DWORD *resume_handle );
NET_API_STATUS wexa_user_getinfo( const WCHAR *username, DWORD level, BYTE **bufptr );
NET_API_STATUS wexa_user_add( DWORD level, BYTE *buf, DWORD *parm_err );
NET_API_STATUS wexa_user_del( const WCHAR *username );
NET_API_STATUS wexa_user_setinfo( const WCHAR *username, DWORD level, BYTE *buf, DWORD *parm_err );
NET_API_STATUS wexa_user_getlocalgroups( const WCHAR *username, DWORD level, DWORD flags, BYTE **bufptr,
                                         DWORD prefmaxlen, DWORD *entriesread, DWORD *totalentries );
NET_API_STATUS wexa_group_enum( DWORD level, BYTE **bufptr, DWORD prefmaxlen, DWORD *entriesread, DWORD *totalentries,
                                DWORD_PTR *resume_handle );
NET_API_STATUS wexa_group_getinfo( const WCHAR *groupname, DWORD level, BYTE **bufptr );
NET_API_STATUS wexa_group_getmembers( const WCHAR *groupname, DWORD level, BYTE **bufptr, DWORD prefmaxlen,
                                      DWORD *entriesread, DWORD *totalentries, DWORD_PTR *resume_handle );
/* AddMember(s) (add) and DelMember(s): level 0 is a SID (one entry), level 3 "DOMAIN\name" */
NET_API_STATUS wexa_group_members( const WCHAR *groupname, DWORD level, BYTE *buf, DWORD count, BOOL add );

#endif
