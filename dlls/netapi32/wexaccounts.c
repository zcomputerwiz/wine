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

/* The client of docs/LOCAL_USERS_DESIGN.md: the program talks to \\.\pipe\WexSession0\accounts, the service wexaccounts of
 * session 0 forwards the request to the account helper of the machine, which does all the policy (who may do what, the
 * last administrator, a user that is logged in). This file only translates: the NetUser* and NetLocalGroup* calls into
 * the frames of the service and what comes back into the structures and the error codes of the API.
 *
 * Phase 1 has two local groups: Administrators (the accounts of the administrator type) and Users (the others). Other
 * groups and domain accounts as members come later. */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#include "winnt.h"
#include "lm.h"
#include "lmerr.h"
#include "wine/debug.h"
#include "wexaccounts.h"

WINE_DEFAULT_DEBUG_CHANNEL(netapi32);

/* the wire: a copy of what the client needs of src/include/wexaccounts_proto.h */
#define WEXA_MAGIC       0x41584557u
#define WEXA_VERSION     1
#define WEXA_MAX_FRAME   65536
#define WEXA_MAX_STR     255

enum
{
    WEXA_LIST_USERS = 1, WEXA_GET_USER, WEXA_ADMIN_MEMBERS, WEXA_CREATE_USER, WEXA_DELETE_USER, WEXA_RENAME_USER,
    WEXA_SET_FULLNAME, WEXA_SET_LOCKED, WEXA_SET_TYPE, WEXA_SET_PASSWORD, WEXA_START_SYNC
};

enum
{
    WEXA_OK = 0, WEXA_DENIED, WEXA_NOT_FOUND, WEXA_EXISTS, WEXA_BAD_ARGUMENT, WEXA_LOGGED_IN, WEXA_BUSY,
    WEXA_UNAVAILABLE, WEXA_LAST_ADMIN, WEXA_BAD_REQUEST, WEXA_SYNC_PENDING
};

#pragma pack(push,1)
struct wexa_request
{
    UINT32 magic;
    UINT16 version;
    UINT16 op;
    UINT32 request_id;
    UINT32 flags;
    UINT32 verdict;
    UINT32 caller_uid;
    UINT32 body_len;
};

struct wexa_reply
{
    UINT32 magic;
    UINT16 version;
    UINT16 status;
    UINT32 request_id;
    UINT32 body_len;
};
#pragma pack(pop)

#ifndef FILTER_NORMAL_ACCOUNT
#define FILTER_NORMAL_ACCOUNT  0x0002
#endif
#define PARM_ERROR_NONE        0
#define USER_NAME_PARMNUM      1
#define USER_PASSWORD_PARMNUM  3
#define USER_PRIV_PARMNUM      5

#define TYPE_STANDARD  0
#define TYPE_ADMIN     1

static const WCHAR session0_svcctl[] = L"\\\\.\\pipe\\WexSession0\\svcctl";
static const WCHAR default_pipe[] = L"\\\\.\\pipe\\WexSession0\\accounts";

static const WCHAR group_admins[] = L"Administrators";
static const WCHAR group_users[] = L"Users";
static const WCHAR comment_admins[] = L"Administrators have complete and unrestricted access to the computer";
static const WCHAR comment_users[] = L"Users are prevented from making accidental or intentional system-wide changes";

static INIT_ONCE active_once = INIT_ONCE_STATIC_INIT;
static BOOL active;

static BOOL WINAPI detect_active( INIT_ONCE *once, void *param, void **context )
{
    WCHAR tmp[8];
    HANDLE pipe;
    DWORD err;

    active = FALSE;
    if (GetEnvironmentVariableW( L"WEX_NETAPI_STOCK", tmp, ARRAY_SIZE(tmp) )) return TRUE;   /* tests of the stock code */
    if (GetEnvironmentVariableW( L"WEX_ACCOUNTS_PIPE", tmp, 1 )) { active = TRUE; return TRUE; }

    /* like the service manager (services' forward_available): a machine has session 0 when its pipe name is found there,
     * and then the accounts are session 0's, reachable or not (U5): an unreachable service is an error, never the
     * list of this process */
    pipe = CreateFileW( session0_svcctl, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
    if (pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle( pipe );
        active = TRUE;
        return TRUE;
    }
    err = GetLastError();
    active = err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND;
    return TRUE;
}

BOOL wexa_active(void)
{
    InitOnceExecuteOnce( &active_once, detect_active, NULL, NULL );
    return active;
}

/* strings */

static BOOL to_utf8( const WCHAR *str, char *out, int *len )
{
    int n;

    if (!str) return FALSE;
    if (!*str) { out[0] = 0; *len = 0; return TRUE; }
    n = WideCharToMultiByte( CP_UTF8, 0, str, -1, out, WEXA_MAX_STR + 1, NULL, NULL );
    if (!n) return FALSE;
    *len = n - 1;
    return TRUE;
}

static void from_utf8( const char *str, int len, WCHAR *out, int size )
{
    int n = len ? MultiByteToWideChar( CP_UTF8, 0, str, len, out, size - 1 ) : 0;

    out[n] = 0;
}

struct frame
{
    BYTE data[WEXA_MAX_FRAME];
    DWORD len;
};

static BOOL put_str( struct frame *f, const char *s, int len )
{
    WORD n = len;

    if (f->len + 2 + len > sizeof(f->data)) return FALSE;
    memcpy( f->data + f->len, &n, 2 );
    memcpy( f->data + f->len + 2, s, len );
    f->len += 2 + len;
    return TRUE;
}

static BOOL put_wstr( struct frame *f, const WCHAR *s )
{
    char buf[WEXA_MAX_STR + 1];
    int len;

    return to_utf8( s, buf, &len ) && put_str( f, buf, len );
}

static void put_u32( struct frame *f, DWORD v )
{
    memcpy( f->data + f->len, &v, 4 );
    f->len += 4;
}

struct reader
{
    const BYTE *p;
    DWORD len, off;
    BOOL bad;
};

static void reader_str( struct reader *r, WCHAR *out, int size )
{
    WORD n;

    out[0] = 0;
    if (r->bad || r->off + 2 > r->len) { r->bad = TRUE; return; }
    memcpy( &n, r->p + r->off, 2 );
    r->off += 2;
    if (r->off + n > r->len) { r->bad = TRUE; return; }
    from_utf8( (const char *)r->p + r->off, n, out, size );
    r->off += n;
}

static DWORD reader_u32( struct reader *r )
{
    DWORD v = 0;

    if (r->bad || r->off + 4 > r->len) { r->bad = TRUE; return 0; }
    memcpy( &v, r->p + r->off, 4 );
    r->off += 4;
    return v;
}

/* the call itself: one frame to the service, one back */

static DWORD wexa_call( WORD op, const struct frame *body, BYTE *reply, DWORD *reply_len, DWORD *status )
{
    static LONG next_id;
    struct wexa_request req = { WEXA_MAGIC, WEXA_VERSION, op, InterlockedIncrement( &next_id ), 0, 0, 0, 0 };
    struct wexa_reply rep;
    WCHAR pipe_name[MAX_PATH];
    BYTE *in = NULL;
    DWORD mode = PIPE_READMODE_MESSAGE, wrote, got = 0, err = 0, i, blen = body ? body->len : 0;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    BYTE *frame;

    *status = WEXA_UNAVAILABLE;
    *reply_len = 0;
    if (!GetEnvironmentVariableW( L"WEX_ACCOUNTS_PIPE", pipe_name, ARRAY_SIZE(pipe_name) ))
        lstrcpyW( pipe_name, default_pipe );

    if (blen > WEXA_MAX_FRAME - sizeof(req)) return ERROR_INVALID_PARAMETER;
    if (!(frame = HeapAlloc( GetProcessHeap(), 0, sizeof(req) + blen ))) return ERROR_NOT_ENOUGH_MEMORY;
    req.body_len = blen;
    memcpy( frame, &req, sizeof(req) );
    if (blen) memcpy( frame + sizeof(req), body->data, blen );

    for (i = 0; i < 10; i++)
    {
        pipe = CreateFileW( pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW( pipe_name, 1000 );
        else Sleep( 300 );
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        TRACE( "the accounts service is not reachable (%lu)\n", GetLastError() );
        HeapFree( GetProcessHeap(), 0, frame );
        return RPC_S_SERVER_UNAVAILABLE;
    }

    SetNamedPipeHandleState( pipe, &mode, NULL, NULL );
    in = HeapAlloc( GetProcessHeap(), 0, sizeof(rep) + WEXA_MAX_FRAME );
    if (!in) err = ERROR_NOT_ENOUGH_MEMORY;
    else if (!WriteFile( pipe, frame, sizeof(req) + blen, &wrote, NULL ))
        err = RPC_S_SERVER_UNAVAILABLE;
    else if (!ReadFile( pipe, in, sizeof(rep) + WEXA_MAX_FRAME, &got, NULL ) || got < sizeof(rep))
        err = RPC_S_SERVER_UNAVAILABLE;
    CloseHandle( pipe );
    HeapFree( GetProcessHeap(), 0, frame );

    if (!err)
    {
        memcpy( &rep, in, sizeof(rep) );
        if (rep.magic != WEXA_MAGIC || rep.version != WEXA_VERSION || rep.body_len != got - sizeof(rep)
            || rep.body_len > WEXA_MAX_FRAME)
            err = ERROR_INVALID_DATA;
        else
        {
            *status = rep.status;
            memcpy( reply, in + sizeof(rep), rep.body_len );
            *reply_len = rep.body_len;
        }
    }
    HeapFree( GetProcessHeap(), 0, in );
    return err;
}

/* what the service says -> what the API says. notfound: the code for a name that is not there */
static NET_API_STATUS map_status( DWORD err, DWORD status, NET_API_STATUS notfound, NET_API_STATUS badarg )
{
    if (err) return err;
    switch (status)
    {
    case WEXA_OK:
    case WEXA_SYNC_PENDING: return NERR_Success;   /* the change is made; the map follows */
    case WEXA_DENIED:       return ERROR_ACCESS_DENIED;
    case WEXA_NOT_FOUND:    return notfound;
    case WEXA_EXISTS:       return NERR_UserExists;
    case WEXA_BAD_ARGUMENT: return badarg;
    case WEXA_LOGGED_IN:    return ERROR_SHARING_VIOLATION;   /* no closer code: the account is in use */
    case WEXA_BUSY:         return ERROR_BUSY;
    case WEXA_UNAVAILABLE:  return RPC_S_SERVER_UNAVAILABLE;
    case WEXA_LAST_ADMIN:   return NERR_LastAdmin;
    default:                return ERROR_INVALID_DATA;
    }
}

static NET_API_STATUS simple_call( WORD op, const struct frame *body, NET_API_STATUS notfound, NET_API_STATUS badarg )
{
    BYTE *reply = HeapAlloc( GetProcessHeap(), 0, WEXA_MAX_FRAME );
    DWORD len, status, err;
    NET_API_STATUS ret;

    if (!reply) return ERROR_NOT_ENOUGH_MEMORY;
    err = wexa_call( op, body, reply, &len, &status );
    ret = map_status( err, status, notfound, badarg );
    HeapFree( GetProcessHeap(), 0, reply );
    return ret;
}

/* the users */

struct user
{
    WCHAR name[WEXA_MAX_STR + 1];
    WCHAR full[WEXA_MAX_STR + 1];
    DWORD uid, type, locked;
};

struct user_list
{
    struct user *u;
    DWORD count, total;
};

static void free_list( struct user_list *l )
{
    HeapFree( GetProcessHeap(), 0, l->u );
    memset( l, 0, sizeof(*l) );
}

static void read_user( struct reader *r, struct user *u )
{
    reader_str( r, u->name, ARRAY_SIZE(u->name) );
    reader_str( r, u->full, ARRAY_SIZE(u->full) );
    u->uid = reader_u32( r );
    u->type = reader_u32( r );
    u->locked = reader_u32( r );
}

static NET_API_STATUS get_user( const WCHAR *name, struct user *u )
{
    struct frame *f;
    BYTE *reply;
    DWORD len, status, err;
    struct reader r;
    NET_API_STATUS ret;

    if (!name || !*name) return NERR_UserNotFound;
    if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
    f->len = 0;
    if (!put_wstr( f, name )) { HeapFree( GetProcessHeap(), 0, f ); return NERR_UserNotFound; }
    if (!(reply = HeapAlloc( GetProcessHeap(), 0, WEXA_MAX_FRAME ))) { HeapFree( GetProcessHeap(), 0, f ); return ERROR_NOT_ENOUGH_MEMORY; }
    err = wexa_call( WEXA_GET_USER, f, reply, &len, &status );
    ret = map_status( err, status, NERR_UserNotFound, NERR_UserNotFound );
    if (!ret)
    {
        r.p = reply; r.len = len; r.off = 0; r.bad = FALSE;
        read_user( &r, u );
        if (r.bad) ret = ERROR_INVALID_DATA;
    }
    HeapFree( GetProcessHeap(), 0, reply );
    HeapFree( GetProcessHeap(), 0, f );
    return ret;
}

/* all the pages of a list: the users (records) or the administrators (names, the other fields empty) */
static NET_API_STATUS fetch_list( WORD op, struct user_list *l )
{
    BYTE *reply = HeapAlloc( GetProcessHeap(), 0, WEXA_MAX_FRAME );
    struct frame *f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) );
    DWORD start = 0;
    NET_API_STATUS ret = NERR_Success;

    memset( l, 0, sizeof(*l) );
    if (!reply || !f) ret = ERROR_NOT_ENOUGH_MEMORY;
    while (!ret)
    {
        DWORD len, status, err, total, first, count, i;
        struct reader r;

        f->len = 0;
        if (start) put_u32( f, start );
        err = wexa_call( op, f, reply, &len, &status );
        if ((ret = map_status( err, status, NERR_UserNotFound, ERROR_INVALID_PARAMETER ))) break;
        r.p = reply; r.len = len; r.off = 0; r.bad = FALSE;
        total = reader_u32( &r );
        first = reader_u32( &r );
        count = reader_u32( &r );
        if (r.bad || first != start || count > total) { ret = ERROR_INVALID_DATA; break; }
        if (!l->u && total)
        {
            if (total > 100000 || !(l->u = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, total * sizeof(*l->u) )))
            {
                ret = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
        }
        l->total = total;
        for (i = 0; i < count && start + i < total; i++)
        {
            struct user *u = &l->u[start + i];

            if (op == WEXA_LIST_USERS) read_user( &r, u );
            else
            {
                reader_str( &r, u->name, ARRAY_SIZE(u->name) );
                u->type = TYPE_ADMIN;
            }
            if (r.bad) { ret = ERROR_INVALID_DATA; break; }
        }
        if (ret) break;
        start += count;
        l->count = start;
        if (!count || start >= total) break;
    }
    HeapFree( GetProcessHeap(), 0, reply );
    HeapFree( GetProcessHeap(), 0, f );
    if (ret) free_list( l );
    return ret;
}

/* the RID of an account, from the account map; the unix uid when there is none */
static DWORD user_rid( const struct user *u )
{
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    WCHAR domain[256];
    DWORD sid_len = sizeof(sid_buf), dom_len = ARRAY_SIZE(domain);
    SID_NAME_USE use;
    SID *sid = (SID *)sid_buf;

    if (LookupAccountNameW( NULL, u->name, sid, &sid_len, domain, &dom_len, &use ) && sid->SubAuthorityCount)
        return sid->SubAuthority[sid->SubAuthorityCount - 1];
    return u->uid;
}

#define EMPTY_STR  L""

static DWORD user_flags( const struct user *u )
{
    return UF_SCRIPT | UF_NORMAL_ACCOUNT | (u->locked ? UF_ACCOUNTDISABLE : 0);
}

static DWORD level_struct_size( DWORD level )
{
    switch (level)
    {
    case 0:  return sizeof(USER_INFO_0);
    case 1:  return sizeof(USER_INFO_1);
    case 10: return sizeof(USER_INFO_10);
    case 20: return sizeof(USER_INFO_20);
    default: return 0;
    }
}

static DWORD entry_size( DWORD level, const struct user *u )
{
    return level_struct_size( level ) + (lstrlenW( u->name ) + 1 + lstrlenW( u->full ) + 1) * sizeof(WCHAR);
}

/* the entries [first, first + count) of a list, laid out in one buffer (strings after the structures) */
static NET_API_STATUS build_users( DWORD level, const struct user *u, DWORD count, BYTE **out )
{
    DWORD i, size = sizeof(WCHAR);   /* one empty string, shared */
    BYTE *buf, *entry;
    WCHAR *str;

    for (i = 0; i < count; i++) size += entry_size( level, &u[i] );
    if (NetApiBufferAllocate( size, (void **)&buf )) return ERROR_NOT_ENOUGH_MEMORY;
    memset( buf, 0, size );
    entry = buf;
    str = (WCHAR *)(buf + count * level_struct_size( level ));
    {
        WCHAR *empty = (WCHAR *)(buf + size - sizeof(WCHAR));

        *empty = 0;
        for (i = 0; i < count; i++)
        {
            const struct user *e = &u[i];
            WCHAR *name = str, *full;

            lstrcpyW( name, e->name );
            str += lstrlenW( name ) + 1;
            full = str;
            lstrcpyW( full, e->full );
            str += lstrlenW( full ) + 1;

            switch (level)
            {
            case 0:
                ((USER_INFO_0 *)entry)->usri0_name = name;
                break;
            case 1:
            {
                USER_INFO_1 *info = (USER_INFO_1 *)entry;

                info->usri1_name = name;
                info->usri1_password = NULL;
                info->usri1_password_age = 0;
                info->usri1_priv = e->type == TYPE_ADMIN ? USER_PRIV_ADMIN : USER_PRIV_USER;
                info->usri1_home_dir = empty;
                info->usri1_comment = empty;
                info->usri1_flags = user_flags( e );
                info->usri1_script_path = empty;
                break;
            }
            case 10:
            {
                USER_INFO_10 *info = (USER_INFO_10 *)entry;

                info->usri10_name = name;
                info->usri10_comment = empty;
                info->usri10_usr_comment = empty;
                info->usri10_full_name = full;
                break;
            }
            case 20:
            {
                USER_INFO_20 *info = (USER_INFO_20 *)entry;

                info->usri20_name = name;
                info->usri20_full_name = full;
                info->usri20_comment = empty;
                info->usri20_flags = user_flags( e );
                info->usri20_user_id = user_rid( e );
                break;
            }
            }
            entry += level_struct_size( level );
        }
    }
    *out = buf;
    return NERR_Success;
}

/* the entries of the list that fit prefmaxlen from the resume index: how many */
static DWORD fit_entries( DWORD level, const struct user *u, DWORD from, DWORD total, DWORD prefmaxlen )
{
    DWORD i, size = sizeof(WCHAR);

    if (prefmaxlen == MAX_PREFERRED_LENGTH) return total - from;
    for (i = from; i < total; i++)
    {
        size += entry_size( level, &u[i] );
        if (size > prefmaxlen) break;
    }
    return i - from;
}

NET_API_STATUS wexa_user_enum( DWORD level, DWORD filter, BYTE **bufptr, DWORD prefmaxlen, DWORD *entriesread,
                               DWORD *totalentries, DWORD *resume_handle )
{
    struct user_list l;
    NET_API_STATUS ret;
    DWORD from = resume_handle ? *resume_handle : 0, n;

    if (!level_struct_size( level )) return ERROR_INVALID_LEVEL;
    *bufptr = NULL;
    *entriesread = *totalentries = 0;
    /* the accounts here are all normal accounts (the other kinds are in domains) */
    if (filter && !(filter & FILTER_NORMAL_ACCOUNT)) return NERR_Success;
    if ((ret = fetch_list( WEXA_LIST_USERS, &l ))) return ret;
    if (from > l.count) from = l.count;
    n = fit_entries( level, l.u, from, l.count, prefmaxlen );
    *totalentries = l.count - from;
    if (n)
    {
        ret = build_users( level, l.u + from, n, bufptr );
        if (!ret) *entriesread = n;
    }
    if (!ret && from + n < l.count)
    {
        if (resume_handle) *resume_handle = from + n;
        ret = ERROR_MORE_DATA;
    }
    else if (!ret && resume_handle) *resume_handle = 0;
    free_list( &l );
    return ret;
}

NET_API_STATUS wexa_user_getinfo( const WCHAR *username, DWORD level, BYTE **bufptr )
{
    struct user u;
    NET_API_STATUS ret;

    if (!level_struct_size( level )) return level == 2 || level == 3 || level == 4 || level == 11 || level == 23 ? NERR_InternalError : ERROR_INVALID_LEVEL;
    if ((ret = get_user( username, &u ))) return ret;
    return build_users( level, &u, 1, bufptr );
}

static NET_API_STATUS hash_of( const WCHAR *password, char *hash )
{
    char utf8[3 * (PWLEN + 1) + 1];

    if (!password || lstrlenW( password ) > PWLEN) return NERR_PasswordTooShort;
    if (!*password) utf8[0] = 0;
    else if (!WideCharToMultiByte( CP_UTF8, 0, password, -1, utf8, sizeof(utf8), NULL, NULL )) return NERR_PasswordTooShort;
    if (!wexa_hash_password( utf8, hash )) return ERROR_NOT_ENOUGH_MEMORY;
    SecureZeroMemory( utf8, sizeof(utf8) );
    return NERR_Success;
}

NET_API_STATUS wexa_user_add( DWORD level, BYTE *buf, DWORD *parm_err )
{
    USER_INFO_2 *info = (USER_INFO_2 *)buf;   /* levels 3 and 4 begin the same way up to the full name */
    const WCHAR *full = EMPTY_STR;
    struct frame *f;
    char hash[WEXA_HASH_MAX];
    NET_API_STATUS ret;
    DWORD flags;

    if (level < 1 || level > 4) return ERROR_INVALID_LEVEL;
    if (!buf) return ERROR_INVALID_PARAMETER;
    if (level >= 2 && info->usri2_full_name) full = info->usri2_full_name;
    flags = info->usri2_flags;
    if (parm_err) *parm_err = PARM_ERROR_NONE;

    if (!info->usri2_name || !*info->usri2_name || lstrlenW( info->usri2_name ) > UNLEN)
    {
        if (parm_err) *parm_err = USER_NAME_PARMNUM;
        return NERR_BadUsername;
    }
    if (info->usri2_priv != USER_PRIV_USER)
    {
        if (parm_err) *parm_err = USER_PRIV_PARMNUM;
        return ERROR_INVALID_PARAMETER;
    }
    if ((ret = hash_of( info->usri2_password ? info->usri2_password : EMPTY_STR, hash )))
    {
        if (parm_err) *parm_err = USER_PASSWORD_PARMNUM;
        return ret;
    }

    if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
    f->len = 0;
    if (!put_wstr( f, info->usri2_name ) || !put_wstr( f, full ))
        ret = NERR_BadUsername;
    else
    {
        put_u32( f, TYPE_STANDARD );
        if (!put_str( f, hash, strlen( hash ) )) ret = ERROR_INVALID_PARAMETER;
        else ret = simple_call( WEXA_CREATE_USER, f, NERR_UserNotFound, NERR_BadUsername );
    }
    SecureZeroMemory( f, sizeof(*f) );
    HeapFree( GetProcessHeap(), 0, f );
    if (!ret && (flags & UF_ACCOUNTDISABLE))
    {
        if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
        f->len = 0;
        put_wstr( f, info->usri2_name );
        put_u32( f, 1 );
        ret = simple_call( WEXA_SET_LOCKED, f, NERR_UserNotFound, ERROR_INVALID_PARAMETER );
        HeapFree( GetProcessHeap(), 0, f );
    }
    return ret;
}

NET_API_STATUS wexa_user_del( const WCHAR *username )
{
    struct frame *f;
    NET_API_STATUS ret;

    if (!username || !*username) return NERR_UserNotFound;
    if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
    f->len = 0;
    if (!put_wstr( f, username )) ret = NERR_UserNotFound;
    else ret = simple_call( WEXA_DELETE_USER, f, NERR_UserNotFound, NERR_UserNotFound );
    HeapFree( GetProcessHeap(), 0, f );
    return ret;
}

static NET_API_STATUS set_two( WORD op, const WCHAR *name, const WCHAR *text )
{
    struct frame *f;
    NET_API_STATUS ret;

    if (!name || !*name || !text) return NERR_UserNotFound;
    if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
    f->len = 0;
    if (!put_wstr( f, name ) || !put_wstr( f, text )) ret = ERROR_INVALID_PARAMETER;
    else ret = simple_call( op, f, NERR_UserNotFound, ERROR_INVALID_PARAMETER );
    HeapFree( GetProcessHeap(), 0, f );
    return ret;
}

static NET_API_STATUS set_password( const WCHAR *name, const WCHAR *password )
{
    struct frame *f;
    char hash[WEXA_HASH_MAX];
    NET_API_STATUS ret;

    if (!name || !*name) return NERR_UserNotFound;
    if ((ret = hash_of( password, hash ))) return ret;
    if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
    f->len = 0;
    if (!put_wstr( f, name ) || !put_str( f, hash, strlen( hash ) )) ret = ERROR_INVALID_PARAMETER;
    else ret = simple_call( WEXA_SET_PASSWORD, f, NERR_UserNotFound, ERROR_INVALID_PARAMETER );
    SecureZeroMemory( f, sizeof(*f) );
    HeapFree( GetProcessHeap(), 0, f );
    return ret;
}

static NET_API_STATUS set_locked( const WCHAR *name, BOOL locked )
{
    struct frame *f;
    NET_API_STATUS ret;

    if (!name || !*name) return NERR_UserNotFound;
    if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
    f->len = 0;
    if (!put_wstr( f, name )) ret = NERR_UserNotFound;
    else
    {
        put_u32( f, locked ? 1 : 0 );
        ret = simple_call( WEXA_SET_LOCKED, f, NERR_UserNotFound, ERROR_INVALID_PARAMETER );
    }
    HeapFree( GetProcessHeap(), 0, f );
    return ret;
}

NET_API_STATUS wexa_user_setinfo( const WCHAR *username, DWORD level, BYTE *buf, DWORD *parm_err )
{
    if (parm_err) *parm_err = PARM_ERROR_NONE;
    if (!buf) return ERROR_INVALID_PARAMETER;

    switch (level)
    {
    case 0:     /* the name */
        return set_two( WEXA_RENAME_USER, username, ((USER_INFO_0 *)buf)->usri0_name );
    case 1:     /* the password, if there is one, and the flags */
    {
        USER_INFO_1 *info = (USER_INFO_1 *)buf;
        NET_API_STATUS ret = NERR_Success;

        if (info->usri1_password && *info->usri1_password) ret = set_password( username, info->usri1_password );
        if (!ret) ret = set_locked( username, (info->usri1_flags & UF_ACCOUNTDISABLE) != 0 );
        return ret;
    }
    case 1003:  /* the password */
        return set_password( username, ((USER_INFO_1003 *)buf)->usri1003_password );
    case 1008:  /* the flags: the account is disabled or not (the other flags have no place in a unix account) */
        return set_locked( username, (((USER_INFO_1008 *)buf)->usri1008_flags & UF_ACCOUNTDISABLE) != 0 );
    case 1011:  /* the full name */
        return set_two( WEXA_SET_FULLNAME, username, ((USER_INFO_1011 *)buf)->usri1011_full_name );
    case 2: case 3: case 4: case 1005: case 1006: case 1007: case 1009: case 1010: case 1012: case 1013: case 1014:
    case 1017: case 1018: case 1020: case 1024: case 1025: case 1051: case 1052: case 1053:
        FIXME( "level %lu is not supported on this machine\n", level );
        return ERROR_NOT_SUPPORTED;
    default:
        return ERROR_INVALID_LEVEL;
    }
}

/* the groups */

static BOOL is_group( const WCHAR *name, const WCHAR *group )
{
    return name && !wcsicmp( name, group );
}

static const WCHAR *group_comment( const WCHAR *group )
{
    return is_group( group, group_admins ) ? comment_admins : comment_users;
}

NET_API_STATUS wexa_user_getlocalgroups( const WCHAR *username, DWORD level, DWORD flags, BYTE **bufptr,
                                         DWORD prefmaxlen, DWORD *entriesread, DWORD *totalentries )
{
    struct user u;
    const WCHAR *group;
    LOCALGROUP_USERS_INFO_0 *info;
    DWORD size;
    NET_API_STATUS ret;

    *bufptr = NULL;
    *entriesread = *totalentries = 0;
    if (level != 0) return ERROR_INVALID_LEVEL;
    if ((ret = get_user( username, &u ))) return ret;
    group = u.type == TYPE_ADMIN ? group_admins : group_users;
    size = sizeof(*info) + (lstrlenW( group ) + 1) * sizeof(WCHAR);
    *totalentries = 1;
    if (prefmaxlen != MAX_PREFERRED_LENGTH && prefmaxlen < size) return ERROR_MORE_DATA;
    if (NetApiBufferAllocate( size, (void **)&info )) return ERROR_NOT_ENOUGH_MEMORY;
    info->lgrui0_name = (WCHAR *)(info + 1);
    lstrcpyW( info->lgrui0_name, group );
    *bufptr = (BYTE *)info;
    *entriesread = 1;
    return NERR_Success;
}

NET_API_STATUS wexa_group_enum( DWORD level, BYTE **bufptr, DWORD prefmaxlen, DWORD *entriesread, DWORD *totalentries,
                                DWORD_PTR *resume_handle )
{
    static const WCHAR *names[] = { group_admins, group_users };
    DWORD i, from = resume_handle ? *resume_handle : 0, size = 0, fit = 0;
    BYTE *buf, *entry;
    WCHAR *str;

    if (level != 0 && level != 1) return ERROR_INVALID_LEVEL;
    *bufptr = NULL;
    *entriesread = *totalentries = 0;
    if (from > 2) from = 2;
    *totalentries = 2 - from;
    for (i = from; i < 2; i++)
    {
        DWORD one = (level ? sizeof(LOCALGROUP_INFO_1) : sizeof(LOCALGROUP_INFO_0)) + (lstrlenW( names[i] ) + 1) * sizeof(WCHAR)
                    + (level ? (lstrlenW( group_comment( names[i] ) ) + 1) * sizeof(WCHAR) : 0);

        if (prefmaxlen != MAX_PREFERRED_LENGTH && size + one > prefmaxlen) break;
        size += one;
        fit++;
    }
    if (fit && NetApiBufferAllocate( size, (void **)&buf )) return ERROR_NOT_ENOUGH_MEMORY;
    if (fit)
    {
        entry = buf;
        str = (WCHAR *)(buf + fit * (level ? sizeof(LOCALGROUP_INFO_1) : sizeof(LOCALGROUP_INFO_0)));
        for (i = 0; i < fit; i++)
        {
            const WCHAR *name = names[from + i];

            if (level)
            {
                LOCALGROUP_INFO_1 *info = (LOCALGROUP_INFO_1 *)entry;

                info->lgrpi1_name = str;
                lstrcpyW( str, name );
                str += lstrlenW( name ) + 1;
                info->lgrpi1_comment = str;
                lstrcpyW( str, group_comment( name ) );
                str += lstrlenW( group_comment( name ) ) + 1;
                entry += sizeof(*info);
            }
            else
            {
                LOCALGROUP_INFO_0 *info = (LOCALGROUP_INFO_0 *)entry;

                info->lgrpi0_name = str;
                lstrcpyW( str, name );
                str += lstrlenW( name ) + 1;
                entry += sizeof(*info);
            }
        }
        *bufptr = buf;
        *entriesread = fit;
    }
    if (from + fit < 2)
    {
        if (resume_handle) *resume_handle = from + fit;
        return ERROR_MORE_DATA;
    }
    if (resume_handle) *resume_handle = 0;
    return NERR_Success;
}

NET_API_STATUS wexa_group_getinfo( const WCHAR *groupname, DWORD level, BYTE **bufptr )
{
    const WCHAR *name, *comment;
    DWORD size;
    WCHAR *str;
    BYTE *buf;

    if (level != 0 && level != 1) return ERROR_INVALID_LEVEL;
    if (is_group( groupname, group_admins )) name = group_admins;
    else if (is_group( groupname, group_users )) name = group_users;
    else return NERR_GroupNotFound;
    comment = group_comment( name );
    size = (level ? sizeof(LOCALGROUP_INFO_1) : sizeof(LOCALGROUP_INFO_0)) + (lstrlenW( name ) + 1) * sizeof(WCHAR)
           + (level ? (lstrlenW( comment ) + 1) * sizeof(WCHAR) : 0);
    if (NetApiBufferAllocate( size, (void **)&buf )) return ERROR_NOT_ENOUGH_MEMORY;
    if (level)
    {
        LOCALGROUP_INFO_1 *info = (LOCALGROUP_INFO_1 *)buf;

        str = (WCHAR *)(info + 1);
        info->lgrpi1_name = str;
        lstrcpyW( str, name );
        info->lgrpi1_comment = str + lstrlenW( name ) + 1;
        lstrcpyW( info->lgrpi1_comment, comment );
    }
    else
    {
        LOCALGROUP_INFO_0 *info = (LOCALGROUP_INFO_0 *)buf;

        info->lgrpi0_name = (WCHAR *)(info + 1);
        lstrcpyW( info->lgrpi0_name, name );
    }
    *bufptr = buf;
    return NERR_Success;
}

/* the members of a group (names): Administrators from the helper's list, Users the standard accounts */
static NET_API_STATUS group_names( const WCHAR *groupname, struct user_list *l )
{
    struct user_list all;
    NET_API_STATUS ret;
    DWORD i, n = 0;

    if (is_group( groupname, group_admins )) return fetch_list( WEXA_ADMIN_MEMBERS, l );
    if (!is_group( groupname, group_users )) return NERR_GroupNotFound;
    if ((ret = fetch_list( WEXA_LIST_USERS, &all ))) return ret;
    for (i = 0; i < all.count; i++)
        if (all.u[i].type != TYPE_ADMIN) all.u[n++] = all.u[i];
    all.count = n;
    *l = all;
    return NERR_Success;
}

NET_API_STATUS wexa_group_getmembers( const WCHAR *groupname, DWORD level, BYTE **bufptr, DWORD prefmaxlen,
                                      DWORD *entriesread, DWORD *totalentries, DWORD_PTR *resume_handle )
{
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD computer_len = ARRAY_SIZE(computer), i, from = resume_handle ? *resume_handle : 0, size = 0, fit = 0;
    struct user_list l;
    NET_API_STATUS ret;
    LOCALGROUP_MEMBERS_INFO_3 *info;
    BYTE *buf;
    WCHAR *str;

    *bufptr = NULL;
    *entriesread = *totalentries = 0;
    if (level != 3)
    {
        if (level > 3) return ERROR_INVALID_LEVEL;
        FIXME( "level %lu is not supported on this machine\n", level );
        return ERROR_NOT_SUPPORTED;
    }
    if (!GetComputerNameW( computer, &computer_len )) return GetLastError();
    if ((ret = group_names( groupname, &l ))) return ret;
    if (from > l.count) from = l.count;
    *totalentries = l.count - from;

    for (i = from; i < l.count; i++)
    {
        DWORD one = sizeof(*info) + (computer_len + 1 + lstrlenW( l.u[i].name ) + 1) * sizeof(WCHAR);

        if (prefmaxlen != MAX_PREFERRED_LENGTH && size + one > prefmaxlen) break;
        size += one;
        fit++;
    }
    if (fit)
    {
        if (NetApiBufferAllocate( size, (void **)&buf )) { free_list( &l ); return ERROR_NOT_ENOUGH_MEMORY; }
        info = (LOCALGROUP_MEMBERS_INFO_3 *)buf;
        str = (WCHAR *)(info + fit);
        for (i = 0; i < fit; i++)
        {
            info[i].lgrmi3_domainandname = str;
            lstrcpyW( str, computer );
            str += computer_len;
            *str++ = '\\';
            lstrcpyW( str, l.u[from + i].name );
            str += lstrlenW( l.u[from + i].name ) + 1;
        }
        *bufptr = buf;
        *entriesread = fit;
    }
    ret = NERR_Success;
    if (from + fit < l.count)
    {
        if (resume_handle) *resume_handle = from + fit;
        ret = ERROR_MORE_DATA;
    }
    else if (resume_handle) *resume_handle = 0;
    free_list( &l );
    return ret;
}

/* a member as the name of a local account: from a SID, or a "COMPUTER\name" (also ".\name" and a bare name) */
static NET_API_STATUS member_name( DWORD level, const BYTE *entry, WCHAR *name, DWORD size )
{
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1], domain[256];
    DWORD computer_len = ARRAY_SIZE(computer), dom_len = ARRAY_SIZE(domain), name_len = size;
    const WCHAR *text, *slash;
    SID_NAME_USE use;

    GetComputerNameW( computer, &computer_len );
    if (level == 0)
    {
        PSID sid = ((const LOCALGROUP_MEMBERS_INFO_0 *)entry)->lgrmi0_sid;

        if (!sid || !LookupAccountSidW( NULL, sid, name, &name_len, domain, &dom_len, &use ) || use != SidTypeUser
            || wcsicmp( domain, computer ))
            return ERROR_NO_SUCH_MEMBER;
        return NERR_Success;
    }
    text = ((const LOCALGROUP_MEMBERS_INFO_3 *)entry)->lgrmi3_domainandname;
    if (!text) return ERROR_NO_SUCH_MEMBER;
    if ((slash = wcschr( text, '\\' )))
    {
        if ((slash - text) != 1 || text[0] != '.')
            if ((DWORD)(slash - text) != computer_len || wcsnicmp( text, computer, computer_len )) return ERROR_NO_SUCH_MEMBER;
        text = slash + 1;
    }
    if (lstrlenW( text ) >= (int)size) return ERROR_NO_SUCH_MEMBER;
    lstrcpyW( name, text );
    return NERR_Success;
}

static NET_API_STATUS change_member( const WCHAR *group, const WCHAR *name, BOOL add )
{
    struct user u;
    NET_API_STATUS ret;
    BOOL admins = is_group( group, group_admins );
    struct frame *f;

    if ((ret = get_user( name, &u ))) return ret == NERR_UserNotFound ? ERROR_NO_SUCH_MEMBER : ret;
    if (admins)
    {
        if (add && u.type == TYPE_ADMIN) return ERROR_MEMBER_IN_ALIAS;
        if (!add && u.type != TYPE_ADMIN) return ERROR_MEMBER_NOT_IN_ALIAS;
    }
    else
    {
        /* Users are the accounts that are not administrators: leaving it means joining Administrators */
        if (add && u.type != TYPE_ADMIN) return ERROR_MEMBER_IN_ALIAS;
        if (!add && u.type == TYPE_ADMIN) return ERROR_MEMBER_NOT_IN_ALIAS;
        return ERROR_NOT_SUPPORTED;
    }
    if (!(f = HeapAlloc( GetProcessHeap(), 0, sizeof(*f) ))) return ERROR_NOT_ENOUGH_MEMORY;
    f->len = 0;
    if (!put_wstr( f, name )) ret = ERROR_NO_SUCH_MEMBER;
    else
    {
        put_u32( f, add ? TYPE_ADMIN : TYPE_STANDARD );
        ret = simple_call( WEXA_SET_TYPE, f, ERROR_NO_SUCH_MEMBER, ERROR_INVALID_PARAMETER );
    }
    HeapFree( GetProcessHeap(), 0, f );
    return ret;
}

NET_API_STATUS wexa_group_members( const WCHAR *groupname, DWORD level, BYTE *buf, DWORD count, BOOL add )
{
    DWORD i, item = level == 0 ? sizeof(LOCALGROUP_MEMBERS_INFO_0) : sizeof(LOCALGROUP_MEMBERS_INFO_3);
    NET_API_STATUS ret;

    if (level != 0 && level != 3) return ERROR_INVALID_LEVEL;
    if (!is_group( groupname, group_admins ) && !is_group( groupname, group_users )) return NERR_GroupNotFound;
    if (!buf && count) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < count; i++)
    {
        WCHAR name[WEXA_MAX_STR + 1];

        if ((ret = member_name( level, buf + i * item, name, ARRAY_SIZE(name) ))) return ret;
        if ((ret = change_member( groupname, name, add ))) return ret;
    }
    return NERR_Success;
}
