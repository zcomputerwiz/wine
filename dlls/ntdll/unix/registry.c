/*
 * Registry functions
 *
 * Copyright 1999 Juergen Schmied
 * Copyright 2000 Alexandre Julliard
 * Copyright 2005 Ivan Leo Puoti, Laurent Pinchart
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
 *
 */

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <string.h>

#include "ntstatus.h"
#include "winternl.h"
#include "unix_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(reg);

/* maximum length of a value name in bytes (without terminating null) */
#define MAX_VALUE_LENGTH (16383 * sizeof(WCHAR))


NTSTATUS open_hkcu_key( const char *path, HANDLE *key )
{
    NTSTATUS status;
    char buffer[256];
    WCHAR bufferW[256];
    DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
    DWORD i, len = sizeof(sid_data);
    SID *sid;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;

    status = NtQueryInformationToken( GetCurrentThreadEffectiveToken(), TokenUser, sid_data, len, &len );
    if (status) return status;

    sid = ((TOKEN_USER *)sid_data)->User.Sid;
    len = snprintf( buffer, sizeof(buffer), "\\Registry\\User\\S-%u-%u", sid->Revision,
                   MAKELONG( MAKEWORD( sid->IdentifierAuthority.Value[5], sid->IdentifierAuthority.Value[4] ),
                             MAKEWORD( sid->IdentifierAuthority.Value[3], sid->IdentifierAuthority.Value[2] )));
    for (i = 0; i < sid->SubAuthorityCount; i++)
        len += snprintf( buffer + len, sizeof(buffer) - len, "-%u", sid->SubAuthority[i] );
    len += snprintf( buffer + len, sizeof(buffer) - len, "\\%s", path );

    ascii_to_unicode( bufferW, buffer, len + 1 );
    init_unicode_string( &name, bufferW );
    InitializeObjectAttributes( &attr, &name, OBJ_CASE_INSENSITIVE, 0, NULL );
    return NtCreateKey( key, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL );
}



/* WineExplorer system tier: session 0 serves every program on one connection kind, so a request states the
 * program's registry view (the 32-bit view of a WoW64 program) unless the caller chose one */
static ACCESS_MASK regsrv_view( ACCESS_MASK access )
{
    if (access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY)) return access;
    return access | (is_wow64() ? KEY_WOW64_32KEY : KEY_WOW64_64KEY);
}

/* WineExplorer system tier: where a create or an open goes. A machine key goes to session 0, and the
 * absolute path is kept with its handle there; a per-session key below a session 0 key is opened in this
 * process's server by its absolute path. */
struct regsrv_target
{
    UNICODE_STRING    path;     /* absolute path of a machine key */
    BOOL              owned;    /* path.Buffer to be freed */
    BOOL              remote;   /* session 0 */
    BOOL              use_abs;  /* open here by the absolute path */
    OBJECT_ATTRIBUTES abs;
};

static NTSTATUS regsrv_target( const OBJECT_ATTRIBUTES *attr, struct regsrv_target *t )
{
    NTSTATUS status;

    memset( t, 0, sizeof(*t) );
    if (regsrv_is_handle( attr->RootDirectory ))
    {
        if ((status = regsrv_full_path( attr->RootDirectory, attr->ObjectName, &t->path ))) return status;
        t->owned = TRUE;
        if (!(t->remote = regsrv_route_path( t->path.Buffer, t->path.Length )))
        {
            t->abs = *attr;
            t->abs.RootDirectory = 0;
            t->abs.ObjectName = &t->path;
            t->use_abs = TRUE;
        }
    }
    else if (!attr->RootDirectory && attr->ObjectName &&
             regsrv_route_path( attr->ObjectName->Buffer, attr->ObjectName->Length ))
    {
        t->path = *attr->ObjectName;
        t->remote = TRUE;
    }
    return STATUS_SUCCESS;
}

static void regsrv_target_free( struct regsrv_target *t )
{
    if (t->owned) free( t->path.Buffer );
}

static NTSTATUS open_key( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr, ULONG options,
                          BOOL virtualize );

/* WineExplorer system tier T2b: UAC registry virtualization (the virtual store), for programs whose machine
 * registry is held by session 0 (docs/SYSTEM_TIER_WORK_ORDERS.md). As on Windows since Vista: for a 32-bit
 * program with a limited token (not elevated, not SYSTEM or a service account), keys under HKLM\Software
 * other than Classes, Microsoft\Windows and Microsoft\Windows NT are virtualized: a write that session 0
 * refuses goes to the user's own HKCU\Software\Classes\VirtualStore\Machine\Software\Wow6432Node\<rest>, and
 * reads see the store first, then the real key. Such a key is a virtual handle here (REGVIRT_TAG + index * 4)
 * that holds a handle of the real key (or none) and of the store key (or none until the first write). Not done:
 * a manifest's requestedExecutionLevel (not known to ntdll, so treated as not stated), and NtQueryKey counts
 * are the real key's. */
#define REGVIRT_TAG  0x60000000
#define REGVIRT_MAX  0x00100000

struct regvirt_key
{
    HANDLE         real;        /* the real key in session 0, or 0 */
    HANDLE         store;       /* the store key, or 0 until it exists */
    UNICODE_STRING real_path;   /* absolute, as opened */
    UNICODE_STRING store_path;  /* absolute */
    BOOL           used;
};

static struct regvirt_key *regvirt_keys;
static unsigned int regvirt_count, regvirt_size;
static pthread_mutex_t regvirt_mutex = PTHREAD_MUTEX_INITIALIZER;
static int regvirt_state;  /* 0 not decided, 1 this process is virtualized, -1 not */

static struct regvirt_key *regvirt_get( HANDLE handle )
{
    ULONG_PTR value = (ULONG_PTR)handle;
    unsigned int index;

    if (value < REGVIRT_TAG || value >= REGVIRT_TAG + REGVIRT_MAX * 4 || (value & 3)) return NULL;
    index = (value - REGVIRT_TAG) / 4;
    if (index >= regvirt_count || !regvirt_keys[index].used) return NULL;
    return &regvirt_keys[index];
}

BOOL regvirt_is_handle( HANDLE handle )
{
    ULONG_PTR value = (ULONG_PTR)handle;
    return regvirt_count && value >= REGVIRT_TAG && value < REGVIRT_TAG + REGVIRT_MAX * 4 && !(value & 3);
}

/* the handle that stands for a virtual key in calls that are not virtualized (object queries, security) */
HANDLE regvirt_underlying( HANDLE handle )
{
    struct regvirt_key *key;
    HANDLE ret = handle;

    if (!regvirt_is_handle( handle )) return handle;
    pthread_mutex_lock( &regvirt_mutex );
    if ((key = regvirt_get( handle ))) ret = key->real ? key->real : key->store;
    pthread_mutex_unlock( &regvirt_mutex );
    return ret;
}

/* 32-bit, limited, not SYSTEM or a service account */
static BOOL regvirt_qualifies(void)
{
    static const SID_IDENTIFIER_AUTHORITY nt_authority = { SECURITY_NT_AUTHORITY };
    DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
    TOKEN_ELEVATION_TYPE type;
    DWORD len;
    SID *sid;

    if (regvirt_state) return regvirt_state > 0;
    regvirt_state = -1;
    if (!is_wow64()) return FALSE;
    if (NtQueryInformationToken( (HANDLE)~(ULONG_PTR)3, TokenElevationType, &type, sizeof(type), &len ) ||
        type == TokenElevationTypeFull) return FALSE;
    len = sizeof(sid_data);
    if (NtQueryInformationToken( (HANDLE)~(ULONG_PTR)3, TokenUser, sid_data, len, &len )) return FALSE;
    sid = ((TOKEN_USER *)sid_data)->User.Sid;
    if (!memcmp( &sid->IdentifierAuthority, &nt_authority, sizeof(nt_authority) ) && sid->SubAuthorityCount == 1 &&
        sid->SubAuthority[0] >= SECURITY_LOCAL_SYSTEM_RID && sid->SubAuthority[0] <= SECURITY_NETWORK_SERVICE_RID)
        return FALSE;  /* S-1-5-18, 19, 20 */
    regvirt_state = 1;
    return TRUE;
}

/* characters of path matched by an ASCII prefix that ends there or at a backslash, without regard to case */
static unsigned int regvirt_match( const WCHAR *path, unsigned int count, const char *prefix )
{
    unsigned int i, len = strlen( prefix );

    if (count < len) return 0;
    for (i = 0; i < len; i++)
    {
        WCHAR ch = path[i];
        unsigned char p = prefix[i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (p >= 'A' && p <= 'Z') p += 'a' - 'A';
        if (ch != p) return 0;
    }
    return (count == len || path[len] == '\\') ? len : 0;
}

/* the part of an absolute HKLM\Software path below Software (and below Wow6432Node) that is virtualized;
 * FALSE outside the scope */
static BOOL regvirt_scope( const UNICODE_STRING *path, const WCHAR **rest, unsigned int *rest_count )
{
    static const char * const excluded[] = { "Classes", "Microsoft\\Windows", "Microsoft\\Windows NT" };
    unsigned int count = path->Length / sizeof(WCHAR), n, i;
    const WCHAR *p = path->Buffer;

    if (!(n = regvirt_match( p, count, "\\Registry\\Machine\\Software" ))) return FALSE;
    if (n == count) return FALSE;  /* Software itself */
    p += n + 1;
    count -= n + 1;
    if ((n = regvirt_match( p, count, "Wow6432Node" )))
    {
        if (n == count) return FALSE;
        p += n + 1;
        count -= n + 1;
    }
    for (i = 0; i < ARRAY_SIZE(excluded); i++) if (regvirt_match( p, count, excluded[i] )) return FALSE;
    *rest = p;
    *rest_count = count;
    return TRUE;
}

/* \Registry\User\<SID>\Software\Classes\VirtualStore\Machine\Software\Wow6432Node\<rest> */
static BOOL regvirt_store_path( const WCHAR *rest, unsigned int rest_count, UNICODE_STRING *store )
{
    DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
    char buffer[256];
    DWORD i, len = sizeof(sid_data);
    unsigned int n;
    SID *sid;

    if (NtQueryInformationToken( (HANDLE)~(ULONG_PTR)3, TokenUser, sid_data, len, &len )) return FALSE;
    sid = ((TOKEN_USER *)sid_data)->User.Sid;
    n = snprintf( buffer, sizeof(buffer), "\\Registry\\User\\S-%u-%u", sid->Revision,
                  MAKELONG( MAKEWORD( sid->IdentifierAuthority.Value[5], sid->IdentifierAuthority.Value[4] ),
                            MAKEWORD( sid->IdentifierAuthority.Value[3], sid->IdentifierAuthority.Value[2] )));
    for (i = 0; i < sid->SubAuthorityCount; i++) n += snprintf( buffer + n, sizeof(buffer) - n, "-%u", sid->SubAuthority[i] );
    n += snprintf( buffer + n, sizeof(buffer) - n, "\\Software\\Classes\\VirtualStore\\Machine\\Software\\Wow6432Node\\" );
    if (n >= sizeof(buffer) || (n + rest_count) * sizeof(WCHAR) > 0xfffe) return FALSE;
    if (!(store->Buffer = malloc( (n + rest_count) * sizeof(WCHAR) ))) return FALSE;
    ascii_to_unicode( store->Buffer, buffer, n );
    memcpy( store->Buffer + n, rest, rest_count * sizeof(WCHAR) );
    store->Length = store->MaximumLength = (n + rest_count) * sizeof(WCHAR);
    return TRUE;
}

static NTSTATUS regvirt_open_path( HANDLE *key, const UNICODE_STRING *path, ACCESS_MASK access, BOOL create )
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING parent;
    NTSTATUS status;
    HANDLE handle;

    InitializeObjectAttributes( &attr, (UNICODE_STRING *)path, OBJ_CASE_INSENSITIVE, 0, NULL );
    /* the store is under HKCU\Software\Classes: never redirected for a 32-bit program */
    if (!create) return NtOpenKeyEx( key, access | KEY_WOW64_64KEY, &attr, 0 );
    status = NtCreateKey( key, access | KEY_WOW64_64KEY, &attr, 0, NULL, 0, NULL );
    if (status != STATUS_OBJECT_NAME_NOT_FOUND) return status;

    /* a key is created only below an existing one: make the missing parents first (VirtualStore\Machine\...) */
    parent = *path;
    while (parent.Length && parent.Buffer[parent.Length / sizeof(WCHAR) - 1] != '\\') parent.Length -= sizeof(WCHAR);
    if (parent.Length <= sizeof(WCHAR)) return status;
    parent.Length -= sizeof(WCHAR);
    parent.MaximumLength = parent.Length;
    if ((status = regvirt_open_path( &handle, &parent, KEY_CREATE_SUB_KEY, TRUE ))) return status;
    NtClose( handle );
    return NtCreateKey( key, access | KEY_WOW64_64KEY, &attr, 0, NULL, 0, NULL );
}

static HANDLE regvirt_add( HANDLE real, HANDLE store, const UNICODE_STRING *real_path, UNICODE_STRING *store_path )
{
    struct regvirt_key *key = NULL;
    unsigned int i;
    HANDLE ret = 0;
    WCHAR *copy;

    if (!(copy = malloc( max( real_path->Length, sizeof(WCHAR) )))) return 0;
    memcpy( copy, real_path->Buffer, real_path->Length );
    pthread_mutex_lock( &regvirt_mutex );
    for (i = 0; i < regvirt_count; i++) if (!regvirt_keys[i].used) break;
    if (i == regvirt_count && i < REGVIRT_MAX)
    {
        if (i == regvirt_size)
        {
            unsigned int size = regvirt_size ? regvirt_size * 2 : 32;
            struct regvirt_key *keys = realloc( regvirt_keys, size * sizeof(*keys) );
            if (keys) { regvirt_keys = keys; regvirt_size = size; }
        }
        if (i < regvirt_size) regvirt_count++;
    }
    if (i < regvirt_count)
    {
        key = &regvirt_keys[i];
        key->real = real;
        key->store = store;
        key->real_path.Buffer = copy;
        key->real_path.Length = key->real_path.MaximumLength = real_path->Length;
        key->store_path = *store_path;
        store_path->Buffer = NULL;  /* the key owns it now */
        key->used = TRUE;
        ret = (HANDLE)(ULONG_PTR)(REGVIRT_TAG + i * 4);
    }
    pthread_mutex_unlock( &regvirt_mutex );
    if (!key) free( copy );
    return ret;
}

#define REGVIRT_WRITE (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_CREATE_LINK | DELETE | WRITE_DAC | WRITE_OWNER | \
                       GENERIC_WRITE | GENERIC_ALL)

/* after a create or open of an absolute path that session 0 answered with status (key: its handle on success):
 * virtualize it when the process and the path qualify; returns the final status */
static NTSTATUS regvirt_after( NTSTATUS status, HANDLE *key, const UNICODE_STRING *path, ACCESS_MASK access,
                               BOOL create, ULONG *dispos )
{
    UNICODE_STRING store = { 0 };
    const WCHAR *rest;
    unsigned int rest_count;
    HANDLE real = 0, store_key = 0, handle;
    ACCESS_MASK view = access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY);
    BOOL existed = FALSE;

    if (status != STATUS_SUCCESS && status != STATUS_ACCESS_DENIED && status != STATUS_OBJECT_NAME_NOT_FOUND)
        return status;
    if (status == STATUS_ACCESS_DENIED && !(access & REGVIRT_WRITE)) return status;
    if (!regvirt_scope( path, &rest, &rest_count ) || !regvirt_qualifies()) return status;
    if (!regvirt_store_path( rest, rest_count, &store )) return status;

    if (status == STATUS_SUCCESS)
    {
        /* the real key: virtual only when the store has the key */
        if (regvirt_open_path( &store_key, &store, KEY_ALL_ACCESS, FALSE )) goto done;
        real = *key;
        existed = TRUE;
    }
    else
    {
        OBJECT_ATTRIBUTES attr;

        InitializeObjectAttributes( &attr, (UNICODE_STRING *)path, OBJ_CASE_INSENSITIVE, 0, NULL );
        if (!open_key( &real, KEY_READ | view, &attr, 0, FALSE )) existed = TRUE;  /* not virtualized */
        else real = 0;
        if (regvirt_open_path( &store_key, &store, KEY_ALL_ACCESS, FALSE )) store_key = 0;
        else existed = TRUE;
        if (!store_key && (create || (real && (access & REGVIRT_WRITE))) &&
            regvirt_open_path( &store_key, &store, KEY_ALL_ACCESS, TRUE ))
            store_key = 0;
        if (!real && !store_key)
        {
            status = create ? STATUS_ACCESS_DENIED : STATUS_OBJECT_NAME_NOT_FOUND;
            goto done;
        }
    }
    if (!(handle = regvirt_add( real, store_key, path, &store )))
    {
        if (store_key) NtClose( store_key );
        if (real && real != *key) NtClose( real );
        goto done;
    }
    TRACE( "virtualized %s -> %p\n", debugstr_us(path), handle );
    *key = handle;
    if (dispos) *dispos = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    status = STATUS_SUCCESS;
done:
    free( store.Buffer );
    return status;
}

/* the store key of a virtual key, created at the first write */
static NTSTATUS regvirt_store( HANDLE handle, HANDLE *store )
{
    struct regvirt_key *key;
    UNICODE_STRING path;
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE created;

    pthread_mutex_lock( &regvirt_mutex );
    if (!(key = regvirt_get( handle ))) status = STATUS_INVALID_HANDLE;
    else if (key->store) *store = key->store;
    else status = STATUS_PENDING;
    if (status == STATUS_PENDING) path = key->store_path;
    pthread_mutex_unlock( &regvirt_mutex );
    if (status != STATUS_PENDING) return status;

    if ((status = regvirt_open_path( &created, &path, KEY_ALL_ACCESS, TRUE ))) return status;
    pthread_mutex_lock( &regvirt_mutex );
    if ((key = regvirt_get( handle )) && !key->store) key->store = created;
    else if (key) { NtClose( created ); created = key->store; }
    pthread_mutex_unlock( &regvirt_mutex );
    *store = created;
    return key ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

static BOOL regvirt_handles( HANDLE handle, HANDLE *real, HANDLE *store )
{
    struct regvirt_key *key;

    pthread_mutex_lock( &regvirt_mutex );
    if ((key = regvirt_get( handle )))
    {
        *real = key->real;
        *store = key->store;
    }
    pthread_mutex_unlock( &regvirt_mutex );
    return key != NULL;
}

NTSTATUS regvirt_close( HANDLE handle )
{
    struct regvirt_key *key;
    HANDLE real = 0, store = 0;
    WCHAR *p1 = NULL, *p2 = NULL;

    pthread_mutex_lock( &regvirt_mutex );
    if ((key = regvirt_get( handle )))
    {
        real = key->real;
        store = key->store;
        p1 = key->real_path.Buffer;
        p2 = key->store_path.Buffer;
        memset( key, 0, sizeof(*key) );
    }
    pthread_mutex_unlock( &regvirt_mutex );
    if (!key) return STATUS_INVALID_HANDLE;
    if (real) NtClose( real );
    if (store) NtClose( store );
    free( p1 );
    free( p2 );
    return STATUS_SUCCESS;
}

/* a create or open relative to a virtual key: the same, by the absolute path of the real key */
static NTSTATUS regvirt_relative( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                  const UNICODE_STRING *class, ULONG options, ULONG *dispos, BOOL create )
{
    struct regvirt_key *parent;
    OBJECT_ATTRIBUTES abs = *attr;
    UNICODE_STRING path;
    NTSTATUS status;
    unsigned int len;

    pthread_mutex_lock( &regvirt_mutex );
    if (!(parent = regvirt_get( attr->RootDirectory )))
    {
        pthread_mutex_unlock( &regvirt_mutex );
        return STATUS_INVALID_HANDLE;
    }
    len = parent->real_path.Length + (attr->ObjectName->Length ? sizeof(WCHAR) + attr->ObjectName->Length : 0);
    if (len > 0xfffe || !(path.Buffer = malloc( len )))
    {
        pthread_mutex_unlock( &regvirt_mutex );
        return STATUS_NO_MEMORY;
    }
    memcpy( path.Buffer, parent->real_path.Buffer, parent->real_path.Length );
    pthread_mutex_unlock( &regvirt_mutex );
    if (attr->ObjectName->Length)
    {
        path.Buffer[parent->real_path.Length / sizeof(WCHAR)] = '\\';
        memcpy( (char *)path.Buffer + parent->real_path.Length + sizeof(WCHAR), attr->ObjectName->Buffer,
                attr->ObjectName->Length );
    }
    path.Length = path.MaximumLength = len;
    abs.RootDirectory = 0;
    abs.ObjectName = &path;
    status = create ? NtCreateKey( key, access, &abs, 0, class, options, dispos ) : NtOpenKeyEx( key, access, &abs, options );
    free( path.Buffer );
    return status;
}

/* value of a virtual key: the store first */
static NTSTATUS regvirt_query_value( HANDLE handle, const UNICODE_STRING *name, KEY_VALUE_INFORMATION_CLASS info_class,
                                     void *info, DWORD length, DWORD *result_len )
{
    HANDLE real, store;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;

    if (!regvirt_handles( handle, &real, &store )) return STATUS_INVALID_HANDLE;
    if (store) status = NtQueryValueKey( store, name, info_class, info, length, result_len );
    if (status == STATUS_OBJECT_NAME_NOT_FOUND && real)
        status = NtQueryValueKey( real, name, info_class, info, length, result_len );
    return status;
}

/* is the name a value (or subkey) of the store key? */
static BOOL regvirt_in_store( HANDLE store, const WCHAR *name, ULONG name_len, BOOL subkey )
{
    UNICODE_STRING str = { name_len, name_len, (WCHAR *)name };
    KEY_VALUE_BASIC_INFORMATION info;
    OBJECT_ATTRIBUTES attr;
    HANDLE key;
    DWORD len;
    NTSTATUS status;

    if (!store) return FALSE;
    if (!subkey)
    {
        status = NtQueryValueKey( store, &str, KeyValueBasicInformation, &info, sizeof(info), &len );
        return status != STATUS_OBJECT_NAME_NOT_FOUND;
    }
    InitializeObjectAttributes( &attr, &str, OBJ_CASE_INSENSITIVE, store, NULL );
    if (NtOpenKeyEx( &key, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &attr, 0 )) return FALSE;
    NtClose( key );
    return TRUE;
}

/* the number of values (or subkeys) of the store key */
static ULONG regvirt_store_count( HANDLE store, BOOL subkey )
{
    char buffer[sizeof(KEY_FULL_INFORMATION) + 512];
    KEY_FULL_INFORMATION *full = (KEY_FULL_INFORMATION *)buffer;
    DWORD len;

    if (!store || NtQueryKey( store, KeyFullInformation, full, sizeof(buffer), &len )) return 0;
    return subkey ? full->SubKeys : full->Values;
}

/* name of the index-th value (or subkey) of a key, for skipping duplicates */
static NTSTATUS regvirt_name( HANDLE key, ULONG index, BOOL subkey, WCHAR *name, ULONG *name_len )
{
    char buffer[sizeof(KEY_BASIC_INFORMATION) + 512 * sizeof(WCHAR)];
    DWORD len;
    NTSTATUS status;

    if (subkey)
    {
        KEY_BASIC_INFORMATION *info = (KEY_BASIC_INFORMATION *)buffer;
        if ((status = NtEnumerateKey( key, index, KeyBasicInformation, info, sizeof(buffer), &len ))) return status;
        *name_len = min( info->NameLength, 512 * sizeof(WCHAR) );
        memcpy( name, info->Name, *name_len );
    }
    else
    {
        KEY_VALUE_BASIC_INFORMATION *info = (KEY_VALUE_BASIC_INFORMATION *)buffer;
        if ((status = NtEnumerateValueKey( key, index, KeyValueBasicInformation, info, sizeof(buffer), &len )))
            return status;
        *name_len = min( info->NameLength, 512 * sizeof(WCHAR) );
        memcpy( name, info->Name, *name_len );
    }
    return STATUS_SUCCESS;
}

/* the number of values (or subkeys) of a virtual key's merged view */
static ULONG regvirt_merged_count( HANDLE handle, BOOL subkey )
{
    WCHAR name[512];
    HANDLE real, store;
    ULONG count, k, name_len;

    if (!regvirt_handles( handle, &real, &store )) return 0;
    count = regvirt_store_count( store, subkey );
    for (k = 0; real && !regvirt_name( real, k, subkey, name, &name_len ); k++)
        if (!regvirt_in_store( store, name, name_len, subkey )) count++;
    return count;
}

/* merged enumeration of a virtual key: the store's entries first, then the real key's that the store has not */
static NTSTATUS regvirt_enumerate( HANDLE handle, ULONG index, BOOL subkey, int info_class, void *info,
                                   DWORD length, DWORD *result_len )
{
    WCHAR name[512];
    HANDLE real, store;
    ULONG count, k, name_len;
    NTSTATUS status;

    if (!regvirt_handles( handle, &real, &store )) return STATUS_INVALID_HANDLE;
    count = regvirt_store_count( store, subkey );
    if (index < count)
        return subkey ? NtEnumerateKey( store, index, info_class, info, length, result_len )
                      : NtEnumerateValueKey( store, index, info_class, info, length, result_len );
    if (!real) return STATUS_NO_MORE_ENTRIES;
    index -= count;
    for (k = 0; ; k++)
    {
        if ((status = regvirt_name( real, k, subkey, name, &name_len ))) return status;
        if (regvirt_in_store( store, name, name_len, subkey )) continue;
        if (!index--) break;
    }
    return subkey ? NtEnumerateKey( real, k, info_class, info, length, result_len )
                  : NtEnumerateValueKey( real, k, info_class, info, length, result_len );
}


/******************************************************************************
 *              NtCreateKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateKey( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                             ULONG index, const UNICODE_STRING *class, ULONG options, ULONG *dispos )
{
    unsigned int ret, generation = 0;
    data_size_t len;
    struct object_attributes *objattr;
    struct regsrv_target target;

    *key = 0;
    if (attr->Length != sizeof(OBJECT_ATTRIBUTES)) return STATUS_INVALID_PARAMETER;
    if (!attr->ObjectName->Length && !attr->RootDirectory) return STATUS_OBJECT_PATH_SYNTAX_BAD;
    if (regvirt_is_handle( attr->RootDirectory ))
        return regvirt_relative( key, access, attr, class, options, dispos, TRUE );
    if ((ret = regsrv_target( attr, &target ))) return ret;
    if ((ret = wine_server_alloc_object_attributes( target.use_abs ? &target.abs : attr, &objattr, &len )))
    {
        regsrv_target_free( &target );
        return ret;
    }
    objattr->attributes |= OBJ_OPENIF | OBJ_CASE_INSENSITIVE;

    TRACE( "(%p,%s,%s,%x,%x,%p)\n", attr->RootDirectory, debugstr_us(attr->ObjectName),
           debugstr_us(class), options, access, key );

    SERVER_START_REQ( create_key )
    {
        req->access     = access;
        req->options    = options;
        wine_server_add_data( req, objattr, len );
        if (class) wine_server_add_data( req, class->Buffer, class->Length );
        if (target.remote)
        {
            req->access = regsrv_view( access );
            ret = regsrv_call( req, attr->RootDirectory, &objattr->rootdir, &generation );
            if ((ret == STATUS_SUCCESS || ret == STATUS_OBJECT_NAME_EXISTS) &&
                !(*key = regsrv_add_key( reply->hkey, generation, &target.path, regsrv_view( access ),
                                         objattr->attributes | ((options & REG_OPTION_OPEN_LINK) ? OBJ_OPENLINK : 0) )))
                ret = STATUS_NO_MEMORY;
        }
        else
        {
            ret = wine_server_call( req );
            *key = wine_server_ptr_handle( reply->hkey );
        }
    }
    SERVER_END_REQ;

    if (ret == STATUS_OBJECT_NAME_EXISTS)
    {
        if (dispos) *dispos = REG_OPENED_EXISTING_KEY;
        ret = STATUS_SUCCESS;
    }
    else if (ret == STATUS_SUCCESS)
    {
        if (dispos) *dispos = REG_CREATED_NEW_KEY;
    }
    if (target.remote) ret = regvirt_after( ret, key, &target.path, access, TRUE, dispos );  /* T2b */

    TRACE( "<- %p\n", *key );
    free( objattr );
    regsrv_target_free( &target );
    return ret;
}


/******************************************************************************
 *              NtCreateKeyTransacted  (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateKeyTransacted( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                       ULONG index, const UNICODE_STRING *class, ULONG options,
                                       HANDLE transacted, ULONG *dispos )
{
    FIXME( "(%p,%s,%s,%x,%x,%p,%p)\n", attr->RootDirectory, debugstr_us(attr->ObjectName),
           debugstr_us(class), options, access, transacted, key );
    return STATUS_NOT_IMPLEMENTED;
}


/******************************************************************************
 *              NtOpenKeyEx  (NTDLL.@)
 */
static NTSTATUS open_key( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr, ULONG options,
                          BOOL virtualize )
{
    unsigned int ret, generation = 0;
    ULONG attributes;
    struct regsrv_target target;
    const OBJECT_ATTRIBUTES *use;

    *key = 0;
    if (attr->Length != sizeof(*attr)) return STATUS_INVALID_PARAMETER;
    if (attr->ObjectName->Length & 1) return STATUS_OBJECT_NAME_INVALID;
    if (regvirt_is_handle( attr->RootDirectory ))
        return regvirt_relative( key, access, attr, NULL, options, NULL, FALSE );
    if ((ret = regsrv_target( attr, &target ))) return ret;
    use = target.use_abs ? &target.abs : attr;

    TRACE( "(%p,%s,%x,%p)\n", attr->RootDirectory, debugstr_us(attr->ObjectName), access, key );

    if (options & ~REG_OPTION_OPEN_LINK) FIXME( "options %x not implemented\n", options );

    attributes = attr->Attributes | OBJ_CASE_INSENSITIVE;

    SERVER_START_REQ( open_key )
    {
        req->parent     = wine_server_obj_handle( use->RootDirectory );
        req->access     = access;
        req->attributes = attributes;
        wine_server_add_data( req, use->ObjectName->Buffer, use->ObjectName->Length );
        if (target.remote)
        {
            req->access = regsrv_view( access );
            ret = regsrv_call( req, attr->RootDirectory, &req->parent, &generation );
            if (!ret && !(*key = regsrv_add_key( reply->hkey, generation, &target.path, regsrv_view( access ), attributes )))
                ret = STATUS_NO_MEMORY;
        }
        else
        {
            ret = wine_server_call( req );
            *key = wine_server_ptr_handle( reply->hkey );
        }
    }
    SERVER_END_REQ;
    if (target.remote && virtualize) ret = regvirt_after( ret, key, &target.path, access, FALSE, NULL );  /* T2b */
    regsrv_target_free( &target );
    TRACE("<- %p\n", *key);
    return ret;
}


/******************************************************************************
 *              NtOpenKeyEx  (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenKeyEx( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr, ULONG options )
{
    return open_key( key, access, attr, options, TRUE );
}


/******************************************************************************
 *              NtOpenKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenKey( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    return NtOpenKeyEx( key, access, attr, 0 );
}


/******************************************************************************
 *              NtOpenKeyTransactedEx  (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenKeyTransactedEx( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                       ULONG options, HANDLE transaction )
{
    FIXME( "(%p %x %p %x %p) semi-stub\n", key, access, attr, options, transaction );
    return NtOpenKeyEx( key, access, attr, options );
}


/******************************************************************************
 *              NtOpenKeyTransacted  (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenKeyTransacted( HANDLE *key, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                     HANDLE transaction )
{
    return NtOpenKeyTransactedEx( key, access, attr, 0, transaction );
}


/******************************************************************************
 *              NtDeleteKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtDeleteKey( HANDLE key )
{
    unsigned int ret;

    TRACE( "(%p)\n", key );

    if (regvirt_is_handle( key ))  /* T2b: only the store's key can go */
    {
        HANDLE store;
        if ((ret = regvirt_store( key, &store ))) return ret;
        return NtDeleteKey( store );
    }

    SERVER_START_REQ( delete_key )
    {
        req->hkey = wine_server_obj_handle( key );
        ret = (regsrv_is_handle( key ) ? regsrv_call( req, key, &req->hkey, NULL ) : wine_server_call( req ));
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtRenameKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtRenameKey( HANDLE key, UNICODE_STRING *name )
{
    unsigned int ret;

    TRACE( "(%p %s)\n", key, debugstr_us(name) );

    if (!name) return STATUS_ACCESS_VIOLATION;
    if (!name->Buffer || !name->Length) return STATUS_INVALID_PARAMETER;
    if (regvirt_is_handle( key )) return STATUS_ACCESS_DENIED;  /* T2b */

    SERVER_START_REQ( rename_key )
    {
        req->hkey = wine_server_obj_handle( key );
        wine_server_add_data( req, name->Buffer, name->Length );
        ret = regsrv_is_handle( key ) ? regsrv_call( req, key, &req->hkey, NULL ) : wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *     enumerate_key
 *
 * Implementation of NtQueryKey and NtEnumerateKey
 */
static NTSTATUS enumerate_key( HANDLE handle, int index, KEY_INFORMATION_CLASS info_class,
                               void *info, DWORD length, DWORD *result_len )

{
    unsigned int ret;
    void *data_ptr;
    size_t fixed_size;

    switch (info_class)
    {
    case KeyBasicInformation:  data_ptr = ((KEY_BASIC_INFORMATION *)info)->Name; break;
    case KeyFullInformation:   data_ptr = ((KEY_FULL_INFORMATION *)info)->Class; break;
    case KeyNodeInformation:   data_ptr = ((KEY_NODE_INFORMATION *)info)->Name;  break;
    case KeyNameInformation:   data_ptr = ((KEY_NAME_INFORMATION *)info)->Name;  break;
    case KeyCachedInformation: data_ptr = ((KEY_CACHED_INFORMATION *)info)+1;    break;
    default:
        FIXME( "Information class %d not implemented\n", info_class );
        return STATUS_INVALID_PARAMETER;
    }
    fixed_size = (char *)data_ptr - (char *)info;

    SERVER_START_REQ( enum_key )
    {
        req->hkey       = wine_server_obj_handle( handle );
        req->index      = index;
        req->info_class = info_class;
        if (length > fixed_size) wine_server_set_reply( req, data_ptr, length - fixed_size );
        if (!(ret = (regsrv_is_handle( handle ) ? regsrv_call( req, handle, &req->hkey, NULL ) : wine_server_call( req ))))
        {
            switch (info_class)
            {
            case KeyBasicInformation:
            {
                KEY_BASIC_INFORMATION keyinfo;
                keyinfo.LastWriteTime.QuadPart = reply->modif;
                keyinfo.TitleIndex = 0;
                keyinfo.NameLength = reply->namelen;
                memcpy( info, &keyinfo, min( length, fixed_size ) );
            break;
            }

            case KeyFullInformation:
            {
                KEY_FULL_INFORMATION keyinfo;
                keyinfo.LastWriteTime.QuadPart = reply->modif;
                keyinfo.TitleIndex = 0;
                keyinfo.ClassLength = wine_server_reply_size(reply);
                keyinfo.ClassOffset = keyinfo.ClassLength ? fixed_size : -1;
                keyinfo.SubKeys = reply->subkeys;
                keyinfo.MaxNameLen = reply->max_subkey;
                keyinfo.MaxClassLen = reply->max_class;
                keyinfo.Values = reply->values;
                keyinfo.MaxValueNameLen = reply->max_value;
                keyinfo.MaxValueDataLen = reply->max_data;
                memcpy( info, &keyinfo, min( length, fixed_size ) );
                break;
            }

            case KeyNodeInformation:
            {
                KEY_NODE_INFORMATION keyinfo;
                keyinfo.LastWriteTime.QuadPart = reply->modif;
                keyinfo.TitleIndex = 0;
                if (reply->namelen < wine_server_reply_size(reply))
                {
                    keyinfo.ClassLength = wine_server_reply_size(reply) - reply->namelen;
                    keyinfo.ClassOffset = fixed_size + reply->namelen;
                }
                else
                {
                    keyinfo.ClassLength = 0;
                    keyinfo.ClassOffset = -1;
                }
                keyinfo.NameLength = reply->namelen;
                memcpy( info, &keyinfo, min( length, fixed_size ) );
                break;
            }

            case KeyNameInformation:
            {
                KEY_NAME_INFORMATION keyinfo;
                keyinfo.NameLength = reply->namelen;
                memcpy( info, &keyinfo, min( length, fixed_size ) );
                break;
            }

            case KeyCachedInformation:
            {
                KEY_CACHED_INFORMATION keyinfo;
                keyinfo.LastWriteTime.QuadPart = reply->modif;
                keyinfo.TitleIndex = 0;
                keyinfo.SubKeys = reply->subkeys;
                keyinfo.MaxNameLen = reply->max_subkey;
                keyinfo.Values = reply->values;
                keyinfo.MaxValueNameLen = reply->max_value;
                keyinfo.MaxValueDataLen = reply->max_data;
                keyinfo.NameLength = reply->namelen;
                memcpy( info, &keyinfo, min( length, fixed_size ) );
                break;
            }

            default:
                break;
            }
            *result_len = fixed_size + reply->total;
            if (length < fixed_size) ret = STATUS_BUFFER_TOO_SMALL;
            else if (length < *result_len) ret = STATUS_BUFFER_OVERFLOW;
        }
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtEnumerateKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtEnumerateKey( HANDLE handle, ULONG index, KEY_INFORMATION_CLASS info_class,
                                void *info, DWORD length, DWORD *result_len )
{
    /* -1 means query key, so avoid it here */
    if (index == (ULONG)-1) return STATUS_NO_MORE_ENTRIES;
    if (regvirt_is_handle( handle ))  /* T2b: merged */
        return regvirt_enumerate( handle, index, TRUE, info_class, info, length, result_len );
    return enumerate_key( handle, index, info_class, info, length, result_len );
}


/******************************************************************************
 *              NtQueryKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryKey( HANDLE handle, KEY_INFORMATION_CLASS info_class,
                            void *info, DWORD length, DWORD *result_len )
{
    NTSTATUS status = enumerate_key( regvirt_underlying( handle ), -1, info_class, info, length, result_len );

    /* T2b: a virtual key's counts are those of the merged view */
    if (regvirt_is_handle( handle ) && !status &&
        (info_class == KeyFullInformation || info_class == KeyCachedInformation))
    {
        ULONG values = regvirt_merged_count( handle, FALSE ), subkeys = regvirt_merged_count( handle, TRUE );
        if (info_class == KeyFullInformation)
        {
            ((KEY_FULL_INFORMATION *)info)->Values = values;
            ((KEY_FULL_INFORMATION *)info)->SubKeys = subkeys;
            ((KEY_FULL_INFORMATION *)info)->MaxNameLen = max( ((KEY_FULL_INFORMATION *)info)->MaxNameLen, 255 * sizeof(WCHAR) );
            ((KEY_FULL_INFORMATION *)info)->MaxValueNameLen = max( ((KEY_FULL_INFORMATION *)info)->MaxValueNameLen, 255 * sizeof(WCHAR) );
            ((KEY_FULL_INFORMATION *)info)->MaxValueDataLen = max( ((KEY_FULL_INFORMATION *)info)->MaxValueDataLen, 4096 );
        }
        else
        {
            ((KEY_CACHED_INFORMATION *)info)->Values = values;
            ((KEY_CACHED_INFORMATION *)info)->SubKeys = subkeys;
            ((KEY_CACHED_INFORMATION *)info)->MaxNameLen = max( ((KEY_CACHED_INFORMATION *)info)->MaxNameLen, 255 * sizeof(WCHAR) );
            ((KEY_CACHED_INFORMATION *)info)->MaxValueNameLen = max( ((KEY_CACHED_INFORMATION *)info)->MaxValueNameLen, 255 * sizeof(WCHAR) );
            ((KEY_CACHED_INFORMATION *)info)->MaxValueDataLen = max( ((KEY_CACHED_INFORMATION *)info)->MaxValueDataLen, 4096 );
        }
    }
    return status;
}


/******************************************************************************
 *              NtSetInformationKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetInformationKey( HANDLE key, int class, void *info, ULONG length )
{
    FIXME( "(%p,0x%08x,%p,0x%08x) stub\n", key, class, info, length );
    return STATUS_SUCCESS;
}


/* fill the key value info structure for a specific info class */
static void copy_key_value_info( KEY_VALUE_INFORMATION_CLASS info_class, void *info,
                                 DWORD length, int type, int name_len, int data_len )
{
    switch (info_class)
    {
    case KeyValueBasicInformation:
    {
        KEY_VALUE_BASIC_INFORMATION keyinfo;
        keyinfo.TitleIndex = 0;
        keyinfo.Type       = type;
        keyinfo.NameLength = name_len;
        length = min( length, (char *)keyinfo.Name - (char *)&keyinfo );
        memcpy( info, &keyinfo, length );
        break;
    }

    case KeyValueFullInformation:
    {
        KEY_VALUE_FULL_INFORMATION keyinfo;
        keyinfo.TitleIndex = 0;
        keyinfo.Type       = type;
        keyinfo.DataOffset = (char *)keyinfo.Name - (char *)&keyinfo + name_len;
        keyinfo.DataLength = data_len;
        keyinfo.NameLength = name_len;
        length = min( length, (char *)keyinfo.Name - (char *)&keyinfo );
        memcpy( info, &keyinfo, length );
        break;
    }

    case KeyValuePartialInformation:
    {
        KEY_VALUE_PARTIAL_INFORMATION keyinfo;
        keyinfo.TitleIndex = 0;
        keyinfo.Type       = type;
        keyinfo.DataLength = data_len;
        length = min( length, (char *)keyinfo.Data - (char *)&keyinfo );
        memcpy( info, &keyinfo, length );
        break;
    }

    case KeyValuePartialInformationAlign64:
    {
        KEY_VALUE_PARTIAL_INFORMATION_ALIGN64 keyinfo;
        keyinfo.Type       = type;
        keyinfo.DataLength = data_len;
        length = min( length, (char *)keyinfo.Data - (char *)&keyinfo );
        memcpy( info, &keyinfo, length );
        break;
    }

    default:
        break;
    }
}


/******************************************************************************
 *              NtEnumerateValueKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtEnumerateValueKey( HANDLE handle, ULONG index, KEY_VALUE_INFORMATION_CLASS info_class,
                                     void *info, DWORD length, DWORD *result_len )
{
    unsigned int ret;
    void *ptr;
    size_t fixed_size;

    TRACE( "(%p,%u,%d,%p,%d)\n", handle, index, info_class, info, length );

    if (regvirt_is_handle( handle ))  /* T2b: merged */
        return regvirt_enumerate( handle, index, FALSE, info_class, info, length, result_len );

    /* compute the length we want to retrieve */
    switch (info_class)
    {
    case KeyValueBasicInformation:   ptr = ((KEY_VALUE_BASIC_INFORMATION *)info)->Name; break;
    case KeyValueFullInformation:    ptr = ((KEY_VALUE_FULL_INFORMATION *)info)->Name; break;
    case KeyValuePartialInformation: ptr = ((KEY_VALUE_PARTIAL_INFORMATION *)info)->Data; break;
    default:
        FIXME( "Information class %d not implemented\n", info_class );
        return STATUS_INVALID_PARAMETER;
    }
    fixed_size = (char *)ptr - (char *)info;

    SERVER_START_REQ( enum_key_value )
    {
        req->hkey       = wine_server_obj_handle( handle );
        req->index      = index;
        req->info_class = info_class;
        if (length > fixed_size) wine_server_set_reply( req, ptr, length - fixed_size );
        if (!(ret = (regsrv_is_handle( handle ) ? regsrv_call( req, handle, &req->hkey, NULL ) : wine_server_call( req ))))
        {
            copy_key_value_info( info_class, info, length, reply->type, reply->namelen,
                                 wine_server_reply_size(reply) - reply->namelen );
            *result_len = fixed_size + reply->total;
            if (length < *result_len) ret = STATUS_BUFFER_OVERFLOW;
        }
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtQueryValueKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryValueKey( HANDLE handle, const UNICODE_STRING *name,
                                 KEY_VALUE_INFORMATION_CLASS info_class,
                                 void *info, DWORD length, DWORD *result_len )
{
    unsigned int ret;
    UCHAR *data_ptr;
    unsigned int fixed_size, min_size;

    TRACE( "(%p,%s,%d,%p,%d)\n", handle, debugstr_us(name), info_class, info, length );

    if (name->Length > MAX_VALUE_LENGTH) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (regvirt_is_handle( handle ))  /* T2b: the store first */
        return regvirt_query_value( handle, name, info_class, info, length, result_len );

    /* compute the length we want to retrieve */
    switch(info_class)
    {
    case KeyValueBasicInformation:
    {
        KEY_VALUE_BASIC_INFORMATION *basic_info = info;
        min_size = FIELD_OFFSET(KEY_VALUE_BASIC_INFORMATION, Name);
        fixed_size = min_size + name->Length;
        if (min_size < length)
            memcpy(basic_info->Name, name->Buffer, min(length - min_size, name->Length));
        data_ptr = NULL;
        break;
    }

    case KeyValueFullInformation:
    {
        KEY_VALUE_FULL_INFORMATION *full_info = info;
        min_size = FIELD_OFFSET(KEY_VALUE_FULL_INFORMATION, Name);
        fixed_size = min_size + name->Length;
        if (min_size < length)
            memcpy(full_info->Name, name->Buffer, min(length - min_size, name->Length));
        data_ptr = (UCHAR *)full_info->Name + name->Length;
        break;
    }

    case KeyValuePartialInformation:
        min_size = fixed_size = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data);
        data_ptr = ((KEY_VALUE_PARTIAL_INFORMATION *)info)->Data;
        break;

    case KeyValuePartialInformationAlign64:
        min_size = fixed_size = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION_ALIGN64, Data);
        data_ptr = ((KEY_VALUE_PARTIAL_INFORMATION_ALIGN64 *)info)->Data;
        break;

    default:
        FIXME( "Information class %d not implemented\n", info_class );
        return STATUS_INVALID_PARAMETER;
    }

    SERVER_START_REQ( get_key_value )
    {
        req->hkey = wine_server_obj_handle( handle );
        wine_server_add_data( req, name->Buffer, name->Length );
        if (length > fixed_size && data_ptr) wine_server_set_reply( req, data_ptr, length - fixed_size );
        if (!(ret = (regsrv_is_handle( handle ) ? regsrv_call( req, handle, &req->hkey, NULL ) : wine_server_call( req ))))
        {
            copy_key_value_info( info_class, info, length, reply->type,
                                 name->Length, reply->total );
            *result_len = fixed_size + (info_class == KeyValueBasicInformation ? 0 : reply->total);
            if (length < min_size) ret = STATUS_BUFFER_TOO_SMALL;
            else if (length < *result_len) ret = STATUS_BUFFER_OVERFLOW;
        }
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtQueryMultipleValueKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryMultipleValueKey( HANDLE key, KEY_MULTIPLE_VALUE_INFORMATION *info,
                                         ULONG count, void *buffer, ULONG length, ULONG *retlen )
{
    FIXME( "(%p,%p,0x%08x,%p,0x%08x,%p) stub!\n", key, info, count, buffer, length, retlen );
    return STATUS_SUCCESS;
}


/******************************************************************************
 *              NtSetValueKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetValueKey( HANDLE key, const UNICODE_STRING *name, ULONG index,
                               ULONG type, const void *data, ULONG count )
{
    unsigned int ret;

    TRACE( "(%p,%s,%d,%p,%d)\n", key, debugstr_us(name), type, data, count );

    if (name->Length > MAX_VALUE_LENGTH) return STATUS_INVALID_PARAMETER;
    if (regvirt_is_handle( key ))  /* T2b: writes go to the store */
    {
        HANDLE store;
        if ((ret = regvirt_store( key, &store ))) return ret;
        return NtSetValueKey( store, name, index, type, data, count );
    }

    SERVER_START_REQ( set_key_value )
    {
        req->hkey    = wine_server_obj_handle( key );
        req->type    = type;
        req->namelen = name->Length;
        wine_server_add_data( req, name->Buffer, name->Length );
        wine_server_add_data( req, data, count );
        ret = (regsrv_is_handle( key ) ? regsrv_call( req, key, &req->hkey, NULL ) : wine_server_call( req ));
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtDeleteValueKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtDeleteValueKey( HANDLE key, const UNICODE_STRING *name )
{
    unsigned int ret;

    TRACE( "(%p,%s)\n", key, debugstr_us(name) );

    if (name->Length > MAX_VALUE_LENGTH) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (regvirt_is_handle( key ))  /* T2b: only the store's values can go */
    {
        HANDLE store;
        if ((ret = regvirt_store( key, &store ))) return ret;
        return NtDeleteValueKey( store, name );
    }

    SERVER_START_REQ( delete_key_value )
    {
        req->hkey = wine_server_obj_handle( key );
        wine_server_add_data( req, name->Buffer, name->Length );
        ret = (regsrv_is_handle( key ) ? regsrv_call( req, key, &req->hkey, NULL ) : wine_server_call( req ));
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtNotifyChangeMultipleKeys  (NTDLL.@)
 */
NTSTATUS WINAPI NtNotifyChangeMultipleKeys( HANDLE key, ULONG count, OBJECT_ATTRIBUTES *attr,
                                            HANDLE event, PIO_APC_ROUTINE apc, void *apc_context,
                                            IO_STATUS_BLOCK *io, ULONG filter, BOOLEAN subtree,
                                            void *buffer, ULONG length, BOOLEAN async )
{
    unsigned int ret;

    TRACE( "(%p,%u,%p,%p,%p,%p,%p,0x%08x, 0x%08x,%p,0x%08x,0x%08x)\n",
           key, count, attr, event, apc, apc_context, io,
           filter, async, buffer, length, subtree );

    if (count || attr || apc || apc_context || buffer || length)
        FIXME( "Unimplemented optional parameter\n" );
    key = regvirt_underlying( key );  /* T2b: a virtual key is watched through its real key */


    if (!async)
    {
        OBJECT_ATTRIBUTES attr;
        InitializeObjectAttributes( &attr, NULL, 0, NULL, NULL );
        ret = NtCreateEvent( &event, EVENT_ALL_ACCESS, &attr, SynchronizationEvent, FALSE );
        if (ret) return ret;
    }

    if (regsrv_is_handle( key ))
    {
        /* a session 0 key: the event is registered with this process's server, which sets it when session 0
         * reports the change under the cookie (system tier) */
        unsigned int cookie = 0;

        SERVER_START_REQ( relay_registry_notification )
        {
            req->event = wine_server_obj_handle( event );
            ret = wine_server_call( req );
            cookie = reply->cookie;
        }
        SERVER_END_REQ;
        if (!ret)
        {
            SERVER_START_REQ( set_registry_notification )
            {
                req->hkey         = wine_server_obj_handle( key );
                req->event        = 0;
                req->subtree      = subtree;
                req->filter       = filter;
                req->relay_cookie = cookie;
                ret = regsrv_call( req, key, &req->hkey, NULL );
            }
            SERVER_END_REQ;
        }
    }
    else
    {
        SERVER_START_REQ( set_registry_notification )
        {
            req->hkey    = wine_server_obj_handle( key );
            req->event   = wine_server_obj_handle( event );
            req->subtree = subtree;
            req->filter  = filter;
            ret = wine_server_call( req );
        }
        SERVER_END_REQ;
    }

    if (!async)
    {
        if (ret == STATUS_PENDING) ret = NtWaitForSingleObject( event, FALSE, NULL );
        NtClose( event );
    }
    return ret;
}


/******************************************************************************
 *              NtNotifyChangeKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtNotifyChangeKey( HANDLE key, HANDLE event, PIO_APC_ROUTINE apc, void *apc_context,
                                   IO_STATUS_BLOCK *io, ULONG filter, BOOLEAN subtree,
                                   void *buffer, ULONG length, BOOLEAN async )
{
    return NtNotifyChangeMultipleKeys( key, 0, NULL, event, apc, apc_context,
                                       io, filter, subtree, buffer, length, async );
}


/******************************************************************************
 *              NtFlushKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtFlushKey( HANDLE key )
{
    key = regvirt_underlying( key );  /* T2b */
    unsigned int ret;

    TRACE( "key=%p\n", key );

    SERVER_START_REQ( flush_key )
    {
	req->hkey = wine_server_obj_handle( key );
	ret = (regsrv_is_handle( key ) ? regsrv_call( req, key, &req->hkey, NULL ) : wine_server_call( req ));
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtLoadKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtLoadKey( const OBJECT_ATTRIBUTES *attr, OBJECT_ATTRIBUTES *file )
{
    TRACE( "(%p,%p)\n", attr, file );
    return NtLoadKeyEx( attr, file, 0, 0, 0, 0, NULL, NULL );
}

/******************************************************************************
 *              NtLoadKey2  (NTDLL.@)
 */
NTSTATUS WINAPI NtLoadKey2( const OBJECT_ATTRIBUTES *attr, OBJECT_ATTRIBUTES *file, ULONG flags )
{
    return NtLoadKeyEx( attr, file, flags, 0, 0, 0, NULL, NULL );
}

/******************************************************************************
 *              NtLoadKeyEx  (NTDLL.@)
 */
NTSTATUS WINAPI NtLoadKeyEx( const OBJECT_ATTRIBUTES *attr, OBJECT_ATTRIBUTES *file, ULONG flags, HANDLE trustkey,
                             HANDLE event, ACCESS_MASK access, HANDLE *roothandle, IO_STATUS_BLOCK *iostatus )
{
    unsigned int ret;
    HANDLE key;
    data_size_t len;
    struct object_attributes *objattr;
    char *unix_name;
    UNICODE_STRING nt_name;
    OBJECT_ATTRIBUTES new_attr = *file;

    TRACE( "(%p,%p,0x%x,%p,%p,0x%x,%p,%p)\n",
           attr, file, flags, trustkey, event, access, roothandle, iostatus );

    if (flags) FIXME( "flags %x not handled\n", flags );
    if (trustkey) FIXME("trustkey parameter not supported\n");
    if (event) FIXME("event parameter not supported\n");
    if (access) FIXME("access parameter not supported\n");
    if (roothandle) FIXME("roothandle is not filled\n");
    if (iostatus) FIXME("iostatus is not filled\n");

    if (!(ret = get_nt_and_unix_names( &new_attr, &nt_name, &unix_name, FILE_OPEN, FALSE )))
    {
        ret = open_unix_file( &key, unix_name, GENERIC_READ | SYNCHRONIZE,
                              &new_attr, 0, 0, FILE_OPEN, 0, NULL, 0 );
    }
    free( unix_name );
    free( nt_name.Buffer );

    if (ret) return ret;

    if ((ret = wine_server_alloc_object_attributes( attr, &objattr, &len ))) return ret;
    objattr->attributes |= OBJ_OPENIF | OBJ_CASE_INSENSITIVE;

    SERVER_START_REQ( load_registry )
    {
        req->file = wine_server_obj_handle( key );
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        if (ret == STATUS_OBJECT_NAME_EXISTS) ret = STATUS_SUCCESS;
    }
    SERVER_END_REQ;

    NtClose( key );
    free( objattr );
    return ret;
}

/******************************************************************************
 *              NtUnloadKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtUnloadKey( OBJECT_ATTRIBUTES *attr )
{
    unsigned int ret;

    TRACE( "(%p)\n", attr );

    if (!attr || !attr->ObjectName) return STATUS_ACCESS_VIOLATION;
    if (attr->Length != sizeof(*attr)) return STATUS_INVALID_PARAMETER;
    if (attr->ObjectName->Length & 1) return STATUS_OBJECT_NAME_INVALID;

    SERVER_START_REQ( unload_registry )
    {
        req->parent     = wine_server_obj_handle( attr->RootDirectory );
        req->attributes = attr->Attributes;
        wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        ret = wine_server_call(req);
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtSaveKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtSaveKey( HANDLE key, HANDLE file )
{
    unsigned int ret;

    TRACE( "(%p,%p)\n", key, file );

    SERVER_START_REQ( save_registry )
    {
        req->hkey = wine_server_obj_handle( key );
        req->file = wine_server_obj_handle( file );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************************
 *              NtRestoreKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtRestoreKey( HANDLE key, HANDLE file, ULONG flags )
{
    FIXME( "(%p,%p,0x%08x) stub\n", key, file, flags );
    return STATUS_SUCCESS;
}


/******************************************************************************
 *              NtReplaceKey  (NTDLL.@)
 */
NTSTATUS WINAPI NtReplaceKey( OBJECT_ATTRIBUTES *attr, HANDLE key, OBJECT_ATTRIBUTES *replace )
{
    FIXME( "(%s,%p,%s),stub!\n", debugstr_us(attr->ObjectName), key, debugstr_us(replace->ObjectName) );
    return STATUS_SUCCESS;
}

/******************************************************************************
 *              NtQueryLicenseValue  (NTDLL.@)
 *
 * NOTES
 *  On Windows all license properties are stored in a single key, but
 *  unless there is some app which explicitly depends on that, there is
 *  no good reason to reproduce that.
 */
NTSTATUS WINAPI NtQueryLicenseValue( const UNICODE_STRING *name, ULONG *type,
                                     void *data, ULONG length, ULONG *retlen )
{
    static const WCHAR nameW[] = {'\\','R','e','g','i','s','t','r','y','\\',
                                  'M','a','c','h','i','n','e','\\',
                                  'S','o','f','t','w','a','r','e','\\',
                                  'W','i','n','e','\\','L','i','c','e','n','s','e',
                                  'I','n','f','o','r','m','a','t','i','o','n',0};
    UNICODE_STRING keyW = RTL_CONSTANT_STRING( nameW );
    KEY_VALUE_PARTIAL_INFORMATION *info;
    NTSTATUS status = STATUS_OBJECT_NAME_NOT_FOUND;
    DWORD info_length, count;
    OBJECT_ATTRIBUTES attr;
    HANDLE key;

    if (!name || !name->Buffer || !name->Length || !retlen) return STATUS_INVALID_PARAMETER;

    info_length = FIELD_OFFSET( KEY_VALUE_PARTIAL_INFORMATION, Data ) + length;
    if (!(info = malloc( info_length ))) return STATUS_NO_MEMORY;

    InitializeObjectAttributes( &attr, &keyW, 0, 0, NULL );

    /* @@ Wine registry key: HKLM\Software\Wine\LicenseInformation */
    if (!NtOpenKey( &key, KEY_READ, &attr ))
    {
        status = NtQueryValueKey( key, name, KeyValuePartialInformation, info, info_length, &count );
        if (!status || status == STATUS_BUFFER_OVERFLOW)
        {
            if (type) *type = info->Type;
            *retlen = info->DataLength;
            if (status == STATUS_BUFFER_OVERFLOW)
                status = STATUS_BUFFER_TOO_SMALL;
            else
                memcpy( data, info->Data, info->DataLength );
        }
        NtClose( key );
    }

    if (status == STATUS_OBJECT_NAME_NOT_FOUND)
        FIXME( "License key %s not found\n", debugstr_w(name->Buffer) );

    free( info );
    return status;
}
