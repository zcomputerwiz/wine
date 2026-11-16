/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 *
 * Disabled E1-S4 provider. Also compiled into the root-only provisioning tool,
 * so the writer, server and standalone lab probe use exactly the same parser.
 */
#ifndef WEX_ACCOUNTS_STANDALONE
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/xattr.h>
#endif

#include "accounts.h"
#ifndef WEX_ACCOUNTS_STANDALONE
#include "sysrun.h"
#endif

#define MAP_LIMIT (1024 * 1024)
/* Every serialized record exceeds 40 bytes, so this bounds all records that
 * can fit in the map without imposing a smaller account-count ceiling. */
#define ACCOUNT_LIMIT (MAP_LIMIT / 40)
#define U32_MAX_VALUE UINT64_C(4294967295)

/* sid[]: the three subauthorities between 21 and the RID: the machine's, 0 0 0 for the legacy record, or the
 * domain's for a domain record (docs/DOMAIN_LOGON.md) */
struct account_record
{
    char name[64], key[33];
    uint32_t uid, rid, sid[3];
    int active, legacy, domain;
};

/* version 2 (docs/DOMAIN_LOGON.md) adds the joined domain to the header (domain[], domain_name) and a kind to
 * every record; version 1 maps are read and written as before */
struct account_map
{
    uint64_t generation, next_rid;
    uint32_t machine[3], domain[3];
    char domain_name[16];
    unsigned int version, count;
    struct account_record records[ACCOUNT_LIMIT];
};

struct account_policy
{
    /* builtin_admin: the native account that receives the reserved RID 500
     * (the built-in Administrator) when the provisioning tool first maps it. */
    char admin_group[64], system_group[64], builtin_admin[64];
    gid_t system_gid;
};

/* Reserved RID of the built-in Administrator account (well-known machine-SID-500). */
#define BUILTIN_ADMIN_RID 500

struct account_native
{
    char name[64];
    uid_t uid;
    gid_t gid;
    int supported;
};

/* Overrides exist only in the root tool or an explicitly compiled lab binary.
 * The wineserver production object has no environment/path override. */
#if defined(WEX_ACCOUNTS_TOOL) || defined(WEX_ACCOUNTS_TEST)
static const char *account_directory = "/etc/wineexplorer";
#else
#define account_directory "/etc/wineexplorer"
#endif
#ifdef WEX_ACCOUNTS_TEST
/* test_winbind_passwd: users NSS knows but /etc/passwd does not (Winbind's), for the domain cases */
static const char *test_passwd, *test_group, *test_winbind_passwd;
static uid_t test_server_uid;
#endif

int accounts_valid_name( const char *s )
{
    size_t i, len = strlen( s );
    if (!len || len > 63 || *s == '-' || !strcmp( s, "." ) || !strcmp( s, ".." )) return 0;
    for (i = 0; i < len; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.' ||
              (s[i] == '$' && i == len - 1))) return 0;
    return 1;
}

/* a domain's flat (NetBIOS) name: 1 to 15 of the characters of an account name */
static int valid_domain_name( const char *s )
{
    return strlen( s ) <= 15 && accounts_valid_name( s ) && !strchr( s, '$' );
}

/* DOMAIN\user, as Winbind names a domain user with "winbind use default domain = no"; the domain part goes to
 * *domain_len. The whole name fits an account name's 63 characters. */
static int valid_domain_account( const char *s, size_t *domain_len )
{
    const char *sep = strchr( s, '\\' );
    char flat[16];
    size_t len;

    if (!sep || strlen( s ) > 63 || (len = sep - s) == 0 || len > 15 || strchr( sep + 1, '\\' )) return 0;
    memcpy( flat, s, len );
    flat[len] = 0;
    if (!valid_domain_name( flat ) || !accounts_valid_name( sep + 1 )) return 0;
    if (domain_len) *domain_len = len;
    return 1;
}

/* a name that can be in the map: a local account's, or a domain account's */
static int valid_any_account( const char *s )
{
    return accounts_valid_name( s ) || valid_domain_account( s, NULL );
}

static int parse_number( const char *s, uint64_t max, uint64_t *value )
{
    uint64_t n = 0;
    if (!*s || (s[0] == '0' && s[1])) return 0;
    for (; *s; s++)
    {
        unsigned int digit = *s - '0';
        if (digit > 9 || digit > max || n > (max - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    *value = n;
    return 1;
}

/* Reject extended access ACLs altogether, including read-only ones.  An
 * unsupported xattr facility has no POSIX ACLs; other errors fail closed. */
static int no_access_acl( int fd )
{
#ifdef __linux__
    ssize_t n = fgetxattr( fd, "system.posix_acl_access", NULL, 0 );
    return n < 0 && (errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
#else
    return 0;
#endif
}

/* Root provisioning diagnostics only. The server keeps status-only replies;
 * no map contents or account records are emitted. */
#ifdef WEX_ACCOUNTS_TOOL
static void protection_error( const char *name, const char *reason, int fd, gid_t expected_gid )
{
    struct stat st;
    int saved_errno = errno;
    fprintf( stderr, "wex-accounts: protection: %s: %s", name ? name : "(null)", reason );
    if (fd >= 0 && !fstat( fd, &st ))
        fprintf( stderr, " uid=%lu gid=%lu mode=%04lo links=%lu", (unsigned long)st.st_uid,
                 (unsigned long)st.st_gid, (unsigned long)(st.st_mode & 07777), (unsigned long)st.st_nlink );
    if (expected_gid != (gid_t)-1) fprintf( stderr, " expected_gid=%lu", (unsigned long)expected_gid );
    fputc( '\n', stderr );
    errno = saved_errno;
}
#else
#define protection_error(name, reason, fd, gid) ((void)(name))
#endif

/* Walk from / with openat: no symlink component, writable ancestor or path
 * re-resolution after checking. Even sticky /tmp is deliberately rejected. */
static int protected_directory( const char *path, enum wex_account_status *status )
{
    char copy[PATH_MAX], *p, *next;
    const char *component = "/";
    struct stat st;
    int fd, child;

    *status = WEX_ACCOUNT_MAP_UNPROTECTED;
    if (!path || path[0] != '/' || strlen( path ) >= sizeof(copy))
    { protection_error( path, "invalid directory path", -1, (gid_t)-1 ); return -1; }
    strcpy( copy, path + 1 );
    while (*copy && copy[strlen(copy) - 1] == '/') copy[strlen(copy) - 1] = 0;
    if ((fd = open( "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1)
    { protection_error( "/", strerror(errno), -1, (gid_t)-1 ); return -1; }
    p = copy;
    for (;;)
    {
        if (fstat( fd, &st ))
        { protection_error( component, "directory stat failed", fd, (gid_t)-1 ); break; }
        if (!S_ISDIR( st.st_mode ) || st.st_uid || (st.st_mode & 0022))
        { protection_error( component, "unsafe directory type/owner/mode", fd, (gid_t)-1 ); break; }
        if (!no_access_acl( fd ))
        { protection_error( component, "extended access ACL or ACL query failure", fd, (gid_t)-1 ); break; }
        if (!*p) { *status = WEX_ACCOUNT_OK; return fd; }
        if ((next = strchr( p, '/' ))) *next++ = 0;
        if (!*p || !strcmp( p, "." ) || !strcmp( p, ".." ) || (next && !*next))
        { protection_error( path, "invalid directory component", fd, (gid_t)-1 ); break; }
        child = openat( fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
        if (child == -1)
        { protection_error( p, strerror(errno), -1, (gid_t)-1 ); break; }
        close( fd );
        fd = child;
        component = p;
        if (!next) p += strlen( p );
        else p = next;
    }
    close( fd );
    return -1;
}

static enum wex_account_status read_protected( int dir, const char *name, gid_t gid,
                                              char **text, size_t *length )
{
    struct stat before, after;
    enum wex_account_status status = WEX_ACCOUNT_MAP_UNREADABLE;
    size_t used = 0;
    ssize_t n;
    int fd;
    char *buf = NULL;

    *text = NULL;
    if ((fd = openat( dir, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC )) == -1)
    {
        int error = errno;
        protection_error( name, strerror(error), -1, gid );
        return error == ELOOP ? WEX_ACCOUNT_MAP_UNPROTECTED : status;
    }
    if (fstat( fd, &before ))
    { protection_error( name, "file stat failed", fd, gid ); goto done; }
    status = WEX_ACCOUNT_MAP_UNPROTECTED;
    if (!S_ISREG( before.st_mode ))
    { protection_error( name, "not a regular file", fd, gid ); goto done; }
    if (before.st_uid)
    { protection_error( name, "owner is not root", fd, gid ); goto done; }
    if (before.st_mode & 0022)
    { protection_error( name, "group/other writable", fd, gid ); goto done; }
    if (before.st_nlink != 1)
    { protection_error( name, "link count is not one", fd, gid ); goto done; }
    if (!no_access_acl( fd ))
    { protection_error( name, "extended access ACL or ACL query failure", fd, gid ); goto done; }
    if (gid != (gid_t)-1 && before.st_gid != gid)
    { protection_error( name, "group mismatch", fd, gid ); goto done; }
    status = WEX_ACCOUNT_MAP_INVALID;
    if (before.st_size <= 0 || before.st_size > MAP_LIMIT) goto done;
    status = WEX_ACCOUNT_MAP_UNREADABLE;
    if (!(buf = malloc( (size_t)before.st_size + 1 ))) goto done;
    while (used < (size_t)before.st_size)
    {
        n = read( fd, buf + used, (size_t)before.st_size - used );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += n;
    }
    if (fstat( fd, &after ) || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime) goto done;
    status = WEX_ACCOUNT_MAP_INVALID;
    if (memchr( buf, 0, used ) || buf[used - 1] != '\n') goto done;
    buf[used] = 0;
    *text = buf;
    *length = used;
    buf = NULL;
    status = WEX_ACCOUNT_OK;
done:
    free( buf );
    close( fd );
    return status;
}

struct native_group_result { struct group gr; char *buffer; };

static int native_group( const char *name, struct native_group_result *out )
{
    size_t size = 1024;
    int err;
    struct group *gr;
    memset( out, 0, sizeof(*out) );
    for (;;)
    {
        if (!(out->buffer = malloc( size ))) return 0;
#ifdef WEX_ACCOUNTS_TEST
        if (test_group)
        {
            FILE *f = fopen( test_group, "r" );
            if (!f) { free( out->buffer ); out->buffer = NULL; return 0; }
            do { err = fgetgrent_r( f, &out->gr, out->buffer, size, &gr ); }
            while (!err && gr && strcmp( gr->gr_name, name ));
            fclose( f );
        }
        else
#endif
            err = getgrnam_r( name, &out->gr, out->buffer, size, &gr );
        if (!err && gr) return 1;
        free( out->buffer );
        out->buffer = NULL;
        if (err != ERANGE || size >= 1024 * 1024) return 0;
        size *= 2;
    }
}

static enum wex_account_status read_policy( int dir, struct account_policy *policy )
{
    char *buf, *line, *next;
    size_t len;
    int seen_admin = 0, seen_group = 0, seen_builtin = 0;
    struct native_group_result gr;
    enum wex_account_status status;

    strcpy( policy->admin_group, "sudo" );
    strcpy( policy->system_group, "wexsys" );
    policy->builtin_admin[0] = 0;
    if ((status = read_protected( dir, "sysrun.conf", (gid_t)-1, &buf, &len ))) return status;
    status = WEX_ACCOUNT_MAP_INVALID;
    for (line = buf; *line; line = next)
    {
        next = strchr( line, '\n' );
        *next++ = 0;
        if (!strncmp( line, "admin_group=", 12 ))
        {
            if (seen_admin++ || !accounts_valid_name( line + 12 )) goto done;
            strcpy( policy->admin_group, line + 12 );
        }
        else if (!strncmp( line, "group=", 6 ))
        {
            if (seen_group++ || !accounts_valid_name( line + 6 )) goto done;
            strcpy( policy->system_group, line + 6 );
        }
        else if (!strncmp( line, "builtin_administrator=", 22 ))
        {
            if (seen_builtin++ || !accounts_valid_name( line + 22 )) goto done;
            strcpy( policy->builtin_admin, line + 22 );
        }
    }
    status = WEX_ACCOUNT_ADMIN_GROUP_MISSING;
    if (!native_group( policy->admin_group, &gr )) goto done;
    free( gr.buffer );
    status = WEX_ACCOUNT_MAP_INVALID;
    if (!native_group( policy->system_group, &gr )) goto done;
    policy->system_gid = gr.gr.gr_gid;
    free( gr.buffer );
    if (policy->system_gid == (gid_t)-1) goto done;
    status = WEX_ACCOUNT_OK;
done:
    free( buf );
    return status;
}

/* Split exact single-space fields. No ignored lines, trailing fields, signs,
 * whitespace variants or alternate numeric representations in the map. */
static int fields( char *line, char **out, unsigned int count )
{
    unsigned int i;
    for (i = 0; i < count; i++)
    {
        out[i] = line;
        if (!*line) return 0;
        line = strchr( line, ' ' );
        if (i == count - 1) return !line;
        if (!line) return 0;
        *line++ = 0;
    }
    return 0;
}

/* Version 1:
 *   WEXACCOUNTS 1 <generation> <next_rid> <machine_a> <machine_b> <machine_c>
 *   <name> <uid> <key> <active|tombstone> 21 <a> <b> <c> <rid>
 * Version 2 (docs/DOMAIN_LOGON.md) adds the joined domain and a kind to every record:
 *   WEXACCOUNTS 2 <generation> <next_rid> <machine_a> <machine_b> <machine_c> <domain_a> <domain_b> <domain_c> <DOMAIN>
 *   <name> <uid> <key> <active|tombstone> <local|domain> 21 <a> <b> <c> <rid>
 * A local record follows the rules of version 1. A domain record is named DOMAIN\user, has a UID of 1000 or more
 * and the domain RID as it is (500 or more); while active its SID and name are of the header's domain, as a
 * tombstone they may be of a domain the machine left. Anything else makes the whole map invalid, as in
 * version 1: a damaged map denies every lookup rather than drop or re-pick identities. */
static enum wex_account_status parse_map( char *buf, struct account_map *map )
{
    char *line = buf, *next, *v[11];
    unsigned int i, j, f;
    uint64_t n;
    int legacy = 0;

    memset( map, 0, sizeof(*map) );
    next = strchr( line, '\n' );
    *next++ = 0;
    if (!strncmp( line, "WEXACCOUNTS 1 ", 14 )) map->version = 1;
    else if (!strncmp( line, "WEXACCOUNTS 2 ", 14 )) map->version = 2;
    else return WEX_ACCOUNT_MAP_INVALID;
    if (!fields( line, v, map->version == 1 ? 7 : 11 ) ||
        !parse_number( v[2], UINT64_MAX, &map->generation ) || !map->generation ||
        !parse_number( v[3], U32_MAX_VALUE + 1, &map->next_rid ) || map->next_rid < 1001)
        return WEX_ACCOUNT_MAP_INVALID;
    for (i = 0; i < 3; i++)
    {
        if (!parse_number( v[4 + i], U32_MAX_VALUE, &n ) || !n) return WEX_ACCOUNT_MAP_INVALID;
        map->machine[i] = n;
    }
    if (map->version == 2)
    {
        for (i = 0; i < 3; i++)
        {
            if (!parse_number( v[7 + i], U32_MAX_VALUE, &n ) || !n) return WEX_ACCOUNT_MAP_INVALID;
            map->domain[i] = n;
        }
        /* the domain is not the machine */
        if (!memcmp( map->domain, map->machine, sizeof(map->domain) )) return WEX_ACCOUNT_MAP_INVALID;
        if (!valid_domain_name( v[10] )) return WEX_ACCOUNT_MAP_INVALID;
        strcpy( map->domain_name, v[10] );
    }
    for (line = next; *line; line = next)
    {
        struct account_record *r;
        if (map->count == ACCOUNT_LIMIT) return WEX_ACCOUNT_MAP_INVALID;
        next = strchr( line, '\n' );
        *next++ = 0;
        if (!fields( line, v, map->version == 1 ? 9 : 10 )) return WEX_ACCOUNT_MAP_INVALID;
        r = &map->records[map->count++];
        /* f: the first SID field ("21") */
        f = 4;
        if (map->version == 2)
        {
            if (!strcmp( v[4], "domain" )) r->domain = 1;
            else if (strcmp( v[4], "local" )) return WEX_ACCOUNT_MAP_INVALID;
            f = 5;
        }
        if (!(r->domain ? valid_domain_account( v[0], NULL ) : accounts_valid_name( v[0] )) ||
            !parse_number( v[1], U32_MAX_VALUE - 1, &n ) || !n)
            return WEX_ACCOUNT_MAP_INVALID;
        strcpy( r->name, v[0] );
        r->uid = n;
        if (strlen( v[2] ) != 32 || strspn( v[2], "0123456789abcdef" ) != 32 ||
            !strcmp( v[2], "00000000000000000000000000000000" )) return WEX_ACCOUNT_MAP_INVALID;
        strcpy( r->key, v[2] );
        if (!strcmp( v[3], "active" )) r->active = 1;
        else if (strcmp( v[3], "tombstone" )) return WEX_ACCOUNT_MAP_INVALID;
        /* The SID is written as five decimal subauthorities after S-1-5,
         * avoiding any platform-dependent native SID layout. */
        if (strcmp( v[f], "21" )) return WEX_ACCOUNT_MAP_INVALID;
        for (i = 0; i < 3; i++)
        {
            if (!parse_number( v[f + 1 + i], U32_MAX_VALUE, &n )) return WEX_ACCOUNT_MAP_INVALID;
            r->sid[i] = n;
        }
        if (!parse_number( v[f + 4], U32_MAX_VALUE, &n )) return WEX_ACCOUNT_MAP_INVALID;
        r->rid = n;
        if (r->domain)
        {
            size_t len;

            valid_domain_account( r->name, &len );
            if (!r->sid[0] || !r->sid[1] || !r->sid[2] || r->uid < 1000 || r->rid < 500 ||
                !memcmp( r->sid, map->machine, sizeof(r->sid) )) return WEX_ACCOUNT_MAP_INVALID;
            if (r->active && (memcmp( r->sid, map->domain, sizeof(r->sid) ) || strlen( map->domain_name ) != len ||
                strncmp( r->name, map->domain_name, len ))) return WEX_ACCOUNT_MAP_INVALID;
        }
        else
        {
            for (i = 0; i < 3; i++)
                if (r->sid[i] != map->machine[i] && r->sid[i] != 0) return WEX_ACCOUNT_MAP_INVALID;
            r->legacy = !r->sid[0] && !r->sid[1] && !r->sid[2];
            if (!r->legacy && memcmp( r->sid, map->machine, sizeof(r->sid) )) return WEX_ACCOUNT_MAP_INVALID;
        }
        for (j = 0; j + 1 < map->count; j++)
        {
            const struct account_record *old = &map->records[j];
            /* every SID once, tombstones included */
            if (!memcmp( old->sid, r->sid, sizeof(r->sid) ) && old->rid == r->rid) return WEX_ACCOUNT_SID_COLLISION;
            if (!strcmp( old->key, r->key ) || (old->active && r->active &&
                (old->uid == r->uid || !strcmp( old->name, r->name )))) return WEX_ACCOUNT_MAP_INVALID;
        }
        if (r->domain) continue;
        if (r->legacy)
        {
            if (legacy++ || r->rid != 1000) return WEX_ACCOUNT_LEGACY_CONFLICT;
        }
        /* Counter RIDs start at 1001; RID 500 is the reserved built-in
         * Administrator. The SID collision check above keeps it unique. */
        else if (r->rid != BUILTIN_ADMIN_RID && (r->rid < 1001 || r->rid >= map->next_rid))
            return WEX_ACCOUNT_MAP_INVALID;
    }
    return WEX_ACCOUNT_OK;
}

static int native_passwd( const char *name, uid_t uid, struct account_native *out )
{
    size_t size = 1024;
    struct passwd pw, *found;
    char *buffer;
    int err;
    for (;;)
    {
        if (!(buffer = malloc( size ))) return 0;
#ifdef WEX_ACCOUNTS_TEST
        if (test_passwd)
        {
            const char *files[2] = { test_passwd, test_winbind_passwd };
            unsigned int k;
            for (k = 0; k < 2 && files[k]; k++)
            {
                FILE *f = fopen( files[k], "r" );
                if (!f) { free( buffer ); return 0; }
                do { err = fgetpwent_r( f, &pw, buffer, size, &found ); }
                while (!err && found && (name ? strcmp( pw.pw_name, name ) : pw.pw_uid != uid));
                fclose( f );
                if (!err && found) break;
            }
        }
        else
#endif
            err = name ? getpwnam_r( name, &pw, buffer, size, &found ) :
                         getpwuid_r( uid, &pw, buffer, size, &found );
        if (!err && found && valid_any_account( pw.pw_name ) && pw.pw_uid &&
            pw.pw_uid != (uid_t)-1 && pw.pw_gid != (gid_t)-1)
        {
            strcpy( out->name, pw.pw_name );
            out->uid = pw.pw_uid;
            out->gid = pw.pw_gid;
            out->supported = pw.pw_shell && pw.pw_shell[0] &&
                strcmp( pw.pw_shell, "/usr/sbin/nologin" ) && strcmp( pw.pw_shell, "/sbin/nologin" ) &&
                strcmp( pw.pw_shell, "/bin/false" ) && strcmp( pw.pw_shell, "/usr/bin/false" );
            free( buffer );
            return 1;
        }
        free( buffer );
        if (err != ERANGE || size >= 1024 * 1024) return 0;
        size *= 2;
    }
}

static int native_match( const struct account_native *u )
{
    struct account_native v;
    return native_passwd( u->name, 0, &v ) && v.uid == u->uid && v.gid == u->gid &&
           !strcmp( v.name, u->name ) && native_passwd( NULL, u->uid, &v ) &&
           v.uid == u->uid && v.gid == u->gid && !strcmp( v.name, u->name );
}

/* Enumerate only local passwd candidates, and require two-way NSS agreement.
 * Duplicate UIDs/names or enumeration/lookup errors deny the entire snapshot. */
#ifdef WEX_ACCOUNTS_TOOL
static enum wex_account_status native_accounts( struct account_native *users, unsigned int *count )
{
    const char *path = "/etc/passwd";
    struct passwd *pw;
    struct account_native u;
    FILE *f;
    unsigned int j;
    enum wex_account_status status = WEX_ACCOUNT_NSS_ERROR;
#ifdef WEX_ACCOUNTS_TEST
    if (test_passwd) path = test_passwd;
#endif
    *count = 0;
    if (!(f = fopen( path, "r" ))) return status;
    for (;;)
    {
        errno = 0;
        pw = fgetpwent( f );
        if (!pw)
        {
            if ((errno && errno != ENOENT) || ferror( f ) || !feof( f )) goto done;
            break;
        }
        if (!pw->pw_uid) continue;
        if (!accounts_valid_name( pw->pw_name ) || *count == ACCOUNT_LIMIT || pw->pw_uid == (uid_t)-1 ||
            pw->pw_gid == (gid_t)-1) goto done;
        strcpy( u.name, pw->pw_name );
        u.uid = pw->pw_uid;
        u.gid = pw->pw_gid;
        u.supported = pw->pw_shell && pw->pw_shell[0] &&
            strcmp( pw->pw_shell, "/usr/sbin/nologin" ) && strcmp( pw->pw_shell, "/sbin/nologin" ) &&
            strcmp( pw->pw_shell, "/bin/false" ) && strcmp( pw->pw_shell, "/usr/bin/false" );
        if (!native_match( &u )) goto done;
        for (j = 0; j < *count; j++)
            if (users[j].uid == u.uid || !strcmp( users[j].name, u.name )) goto done;
        users[(*count)++] = u;
    }
    status = WEX_ACCOUNT_OK;
done:
    fclose( f );
    return status;
}
#endif

static enum wex_account_status administrator_member( const struct account_native *user,
                                                     const char *group, int *member )
{
    struct native_group_result gr;
    gid_t target, *gids;
    int i, count = 32;
    *member = 0;
    if (!native_group( group, &gr )) return WEX_ACCOUNT_ADMIN_GROUP_MISSING;
    target = gr.gr.gr_gid;
    if (target == (gid_t)-1) { free( gr.buffer ); return WEX_ACCOUNT_NSS_ERROR; }
#ifdef WEX_ACCOUNTS_TEST
    if (test_group)
    {
        char **p;
        *member = user->gid == target;
        for (p = gr.gr.gr_mem; *p; p++) if (!strcmp( *p, user->name )) *member = 1;
        free( gr.buffer );
        return WEX_ACCOUNT_OK;
    }
#endif
    free( gr.buffer );
    for (;;)
    {
        int n = count;
        if (count > 65536 || !(gids = malloc( count * sizeof(*gids) ))) return WEX_ACCOUNT_NSS_ERROR;
        if (getgrouplist( user->name, user->gid, gids, &n ) != -1)
        {
            if (n < 0 || n > count) { free( gids ); return WEX_ACCOUNT_NSS_ERROR; }
            for (i = 0; i < n; i++) if (gids[i] == target) *member = 1;
            free( gids );
            return WEX_ACCOUNT_OK;
        }
        free( gids );
        if (n <= count) return WEX_ACCOUNT_NSS_ERROR;
        count = n;
    }
}

static int domain_admins_member( int dir, gid_t gid, const struct account_record *r );

enum wex_account_status lookup_account( const char *name, uid_t uid, uint64_t caps,
                                        struct wex_account *result )
{
    struct account_map *map = NULL;
    struct account_native user;
    struct account_policy policy;
    struct wex_account value;
    const struct account_record *r = NULL;
    const struct account_native *u = &user;
    char *buf = NULL;
    size_t length;
    unsigned int i;
    int dir = -1;
    enum wex_account_status status;
#ifndef WEX_ACCOUNTS_STANDALONE
    int raised = sysrun_raise();
#endif

    memset( result, 0, sizeof(*result) );
    memset( &value, 0, sizeof(value) );
    status = WEX_ACCOUNT_CAPABILITY_MISSING;
    if (!(caps & WEX_ACCOUNT_MAP_V1)) goto done;
    status = WEX_ACCOUNT_UNKNOWN;
    if (name && !valid_any_account( name )) goto done;
    if ((dir = protected_directory( account_directory, &status )) == -1) goto done;
    if ((status = read_policy( dir, &policy ))) goto done;
    if ((status = read_protected( dir, "accounts.map", policy.system_gid, &buf, &length ))) goto done;
    status = WEX_ACCOUNT_MAP_UNREADABLE;
    if (!(map = malloc( sizeof(*map) ))) goto done;
    if ((status = parse_map( buf, map ))) goto done;
    status = WEX_ACCOUNT_UNKNOWN;
    for (i = 0; i < map->count; i++)
        if (name ? !strcmp( name, map->records[i].name ) : uid == map->records[i].uid)
        {
            if (!r || map->records[i].active) r = &map->records[i];
            if (r->active) break;
        }
    if (!r) goto done;
    status = WEX_ACCOUNT_TOMBSTONED;
    if (!r->active) goto done;
    status = WEX_ACCOUNT_NSS_MISMATCH;
    if (!native_passwd( r->name, 0, &user ) || user.uid != r->uid || !native_match( &user )) goto done;
    status = WEX_ACCOUNT_UNSUPPORTED;
    if (!u->supported) goto done;
    status = WEX_ACCOUNT_LEGACY_CONFLICT;
#ifdef WEX_ACCOUNTS_TEST
    if (u->uid == (test_server_uid ? test_server_uid : getuid()) && !r->legacy) goto done;
#else
    if (u->uid == getuid() && !r->legacy) goto done;
#endif
    if ((status = administrator_member( u, policy.admin_group, &value.administrator ))) goto done;
    /* a domain account in Domain Admins is an administrator as a member of admin_group is (WO 2): the same limited
     * token at logon, elevation through the broker and consent */
    if (!value.administrator && domain_admins_member( dir, policy.system_gid, r )) value.administrator = 1;
    strcpy( value.name, r->name );
    value.uid = u->uid;
    value.primary_gid = u->gid;
    for (i = 0; i < 16; i++)
    {
        char byte[3] = { r->key[2*i], r->key[2*i+1], 0 };
        value.incarnation[i] = strtoul( byte, NULL, 16 );
    }
    value.sid[0] = 21;
    for (i = 0; i < 3; i++) value.sid[i+1] = r->sid[i];
    value.sid[4] = r->rid;
    value.generation = map->generation;
    *result = value;
    status = WEX_ACCOUNT_OK;
done:
    free( map );
    free( buf );
    if (dir != -1) close( dir );
#ifndef WEX_ACCOUNTS_STANDALONE
    sysrun_drop( raised );
#endif
    return status;
}

/* The domain groups recorded at a domain account's logon (docs/DOMAIN_LOGON.md, WO 2, condition 4): the file
 * domain-groups beside the map, written only by the root tool (wex-accounts admit, under the map lock), protected
 * as the map is:
 *   WEXGROUPS 1
 *   <incarnation key> <rid> [<rid>...]
 * one line per active domain record, the RIDs of the groups in the map's domain the account is a member of.
 * The groups of the record with key: their number, or -1 when there is no line for it; a missing file is no
 * groups; anything malformed is no groups either (the account is then no member of Domain Admins: fails closed). */
#define GROUPS_MAX 1024
static int read_domain_groups( int dir, gid_t gid, const char *key, uint32_t *rids, unsigned int max )
{
    char *buf = NULL, *line, *next, *field, *end;
    size_t length;
    unsigned int count;
    uint64_t n;
    int found = -1;

    if (read_protected( dir, "domain-groups", gid, &buf, &length )) return -1;
    line = buf;
    next = strchr( line, '\n' );
    *next++ = 0;
    if (strcmp( line, "WEXGROUPS 1" )) goto done;
    for (line = next; *line; line = next)
    {
        next = strchr( line, '\n' );
        *next++ = 0;
        if (strlen( line ) < 33 || line[32] != ' ' || strspn( line, "0123456789abcdef" ) != 32) { found = -1; goto done; }
        if (strncmp( line, key, 32 )) continue;
        if (found != -1) { found = -1; goto done; }   /* a key twice */
        count = 0;
        for (field = line + 33; *field; field = end)
        {
            char number[16];
            size_t len = strcspn( field, " " );
            if (!len || len >= sizeof(number) || count == max) { found = -1; goto done; }
            memcpy( number, field, len );
            number[len] = 0;
            if (!parse_number( number, U32_MAX_VALUE, &n ) || n < 500) { found = -1; goto done; }
            rids[count++] = n;
            end = field + len;
            if (*end == ' ' && !*++end) { found = -1; goto done; }
        }
        found = count;
    }
done:
    free( buf );
    return found;
}

#define DOMAIN_GROUP_RID_ADMINS_VALUE 512

/* a domain account in the domain's Domain Admins (RID 512, as the root step recorded it at logon) */
static int domain_admins_member( int dir, gid_t gid, const struct account_record *r )
{
    uint32_t rids[GROUPS_MAX];
    int count, i;

    if (!r->domain || (count = read_domain_groups( dir, gid, r->key, rids, GROUPS_MAX )) <= 0) return 0;
    for (i = 0; i < count; i++) if (rids[i] == DOMAIN_GROUP_RID_ADMINS_VALUE) return 1;
    return 0;
}

#ifndef WEX_ACCOUNTS_STANDALONE
/* WO 2: for a server whose user is an active domain record: its SID (as accounts_own_sid), the domain SID's
 * three subauthorities and the recorded domain groups. 1 when the user is a domain account. */
int accounts_own_domain( uid_t uid, unsigned int subauth[5], uint32_t *rids, unsigned int max, unsigned int *count )
{
    struct account_map *map = NULL;
    struct account_policy policy;
    enum wex_account_status status;
    char *buf = NULL;
    size_t length;
    unsigned int i;
    int dir, found = 0, n, raised = sysrun_raise();

    *count = 0;
    if ((dir = protected_directory( account_directory, &status )) == -1) goto done;
    if (read_policy( dir, &policy ) || read_protected( dir, "accounts.map", policy.system_gid, &buf, &length ))
        goto done;
    if (!(map = malloc( sizeof(*map) )) || parse_map( buf, map )) goto done;
    for (i = 0; i < map->count; i++)
    {
        const struct account_record *r = &map->records[i];

        if (!r->active || r->uid != uid || !r->domain) continue;
        subauth[0] = 21;
        subauth[1] = r->sid[0];
        subauth[2] = r->sid[1];
        subauth[3] = r->sid[2];
        subauth[4] = r->rid;
        if ((n = read_domain_groups( dir, policy.system_gid, r->key, rids, max )) > 0) *count = n;
        found = 1;
        break;
    }
done:
    free( map );
    free( buf );
    if (dir != -1) close( dir );
    sysrun_drop( raised );
    return found;
}

/* System tier (docs/SYSTEM_TIER_WORK_ORDERS.md, T4-0): the SID subauthorities after S-1-5 of a local account's
 * active record, the legacy record included (21 0 0 0 1000), for a server that takes its user's SID from the map.
 * 1 when found; 0 without a valid protected map or record. Unlike lookup_account, the server's own uid is the
 * question here, so the legacy rule of lookup_account does not apply. */
int accounts_own_sid( uid_t uid, unsigned int subauth[5] )
{
    struct account_map *map = NULL;
    struct account_policy policy;
    enum wex_account_status status;
    char *buf = NULL;
    size_t length;
    unsigned int i;
    int dir, found = 0, raised = sysrun_raise();

    if ((dir = protected_directory( account_directory, &status )) == -1) goto done;
    if (read_policy( dir, &policy ) || read_protected( dir, "accounts.map", policy.system_gid, &buf, &length ))
        goto done;
    if (!(map = malloc( sizeof(*map) )) || parse_map( buf, map )) goto done;
    for (i = 0; i < map->count; i++)
    {
        const struct account_record *r = &map->records[i];

        if (!r->active || r->uid != uid) continue;
        subauth[0] = 21;
        subauth[1] = r->sid[0];
        subauth[2] = r->sid[1];
        subauth[3] = r->sid[2];
        subauth[4] = r->rid;
        found = 1;
        break;
    }
done:
    free( map );
    free( buf );
    if (dir != -1) close( dir );
    sysrun_drop( raised );
    return found;
}
#endif

#ifdef WEX_E1S6_TRANSPORT_TEST
int accounts_get_map_generation( unsigned long long *gen )
{
    int dir = -1;
    char *buf = NULL;
    size_t length;
    struct account_policy policy;
    struct account_map *map = NULL;
    enum wex_account_status status;
#ifndef WEX_ACCOUNTS_STANDALONE
    int raised = sysrun_raise();
#endif

    if (!gen) return 0;
    if ((dir = protected_directory( account_directory, &status )) == -1) goto done;
    if ((status = read_policy( dir, &policy ))) goto done;
    if ((status = read_protected( dir, "accounts.map", policy.system_gid, &buf, &length ))) goto done;
    status = WEX_ACCOUNT_MAP_UNREADABLE;
    if (!(map = malloc( sizeof(*map) ))) goto done;
    if ((status = parse_map( buf, map ))) goto done;
    if (!map->generation)
    {
        status = WEX_ACCOUNT_MAP_INVALID;
        goto done;
    }
    *gen = map->generation;
    status = WEX_ACCOUNT_OK;

done:
    free( map );
    free( buf );
    if (dir != -1) close( dir );
#ifndef WEX_ACCOUNTS_STANDALONE
    sysrun_drop( raised );
#endif
    return status == WEX_ACCOUNT_OK;
}
#endif
