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

#include "accounts.h"
#ifndef WEX_ACCOUNTS_STANDALONE
#include "sysrun.h"
#endif

#define ACCOUNT_LIMIT 4096
#define MAP_LIMIT (1024 * 1024)
#define U32_MAX_VALUE UINT64_C(4294967295)

struct account_record
{
    char name[64], key[33];
    uint32_t uid, rid;
    int active, legacy;
};

struct account_map
{
    uint64_t generation, next_rid;
    uint32_t machine[3];
    unsigned int count;
    struct account_record records[ACCOUNT_LIMIT];
};

struct account_policy
{
    char admin_group[64], system_group[64];
    gid_t system_gid;
};

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
static const char *test_passwd, *test_group;
static uid_t test_server_uid;
#endif

static int valid_name( const char *s )
{
    size_t i, len = strlen( s );
    if (!len || len > 63) return 0;
    for (i = 0; i < len; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.' ||
              (s[i] == '$' && i == len - 1))) return 0;
    return 1;
}

static int parse_number( const char *s, uint64_t max, uint64_t *value )
{
    uint64_t n = 0;
    if (!*s || (s[0] == '0' && s[1])) return 0;
    for (; *s; s++)
    {
        unsigned int digit = *s - '0';
        if (digit > 9 || n > (max - digit) / 10) return 0;
        n = n * 10 + digit;
    }
    *value = n;
    return 1;
}

/* Walk from / with openat: no symlink component, writable ancestor or path
 * re-resolution after checking. Even sticky /tmp is deliberately rejected. */
static int protected_directory( const char *path, enum wex_account_status *status )
{
    char copy[PATH_MAX], *p, *next;
    struct stat st;
    int fd, child;

    *status = WEX_ACCOUNT_MAP_UNPROTECTED;
    if (!path || path[0] != '/' || strlen( path ) >= sizeof(copy)) return -1;
    strcpy( copy, path + 1 );
    if ((fd = open( "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1) return -1;
    p = copy;
    for (;;)
    {
        if (fstat( fd, &st ) || !S_ISDIR( st.st_mode ) || st.st_uid || (st.st_mode & 0022)) break;
        if (!*p) { *status = WEX_ACCOUNT_OK; return fd; }
        if ((next = strchr( p, '/' ))) *next++ = 0;
        if (!*p || !strcmp( p, "." ) || !strcmp( p, ".." ) || (next && !*next)) break;
        child = openat( fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
        if (child == -1) break;
        close( fd );
        fd = child;
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
        return errno == ELOOP ? WEX_ACCOUNT_MAP_UNPROTECTED : status;
    if (fstat( fd, &before )) goto done;
    status = WEX_ACCOUNT_MAP_UNPROTECTED;
    if (!S_ISREG( before.st_mode ) || before.st_uid || (before.st_mode & 0022) ||
        before.st_nlink != 1 || (gid != (gid_t)-1 && before.st_gid != gid)) goto done;
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

static struct group *native_group( const char *name )
{
#ifdef WEX_ACCOUNTS_TEST
    if (test_group)
    {
        FILE *f = fopen( test_group, "r" );
        struct group *gr = NULL;
        if (!f) return NULL;
        while ((gr = fgetgrent( f ))) if (!strcmp( gr->gr_name, name )) break;
        fclose( f );
        return gr;
    }
#endif
    return getgrnam( name );
}

static enum wex_account_status read_policy( int dir, struct account_policy *policy )
{
    char *buf, *line, *next;
    size_t len;
    int seen_admin = 0, seen_group = 0;
    struct group *gr;
    enum wex_account_status status;

    strcpy( policy->admin_group, "sudo" );
    strcpy( policy->system_group, "wexsys" );
    if ((status = read_protected( dir, "sysrun.conf", (gid_t)-1, &buf, &len ))) return status;
    status = WEX_ACCOUNT_MAP_INVALID;
    for (line = buf; *line; line = next)
    {
        next = strchr( line, '\n' );
        *next++ = 0;
        if (!strncmp( line, "admin_group=", 12 ))
        {
            if (seen_admin++ || !valid_name( line + 12 )) goto done;
            strcpy( policy->admin_group, line + 12 );
        }
        else if (!strncmp( line, "group=", 6 ))
        {
            if (seen_group++ || !valid_name( line + 6 )) goto done;
            strcpy( policy->system_group, line + 6 );
        }
    }
    status = WEX_ACCOUNT_ADMIN_GROUP_MISSING;
    if (!native_group( policy->admin_group )) goto done;
    status = WEX_ACCOUNT_MAP_INVALID;
    if (!(gr = native_group( policy->system_group )) || gr->gr_gid == (gid_t)-1) goto done;
    policy->system_gid = gr->gr_gid;
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

static enum wex_account_status parse_map( char *buf, struct account_map *map )
{
    char *line = buf, *next, *v[9];
    unsigned int i, j;
    uint64_t n;
    int legacy = 0;

    memset( map, 0, sizeof(*map) );
    next = strchr( line, '\n' );
    *next++ = 0;
    if (!fields( line, v, 7 ) || strcmp( v[0], "WEXACCOUNTS" ) || strcmp( v[1], "1" ) ||
        !parse_number( v[2], UINT64_MAX, &map->generation ) || !map->generation ||
        !parse_number( v[3], U32_MAX_VALUE + 1, &map->next_rid ) || map->next_rid < 1001)
        return WEX_ACCOUNT_MAP_INVALID;
    for (i = 0; i < 3; i++)
    {
        if (!parse_number( v[4 + i], U32_MAX_VALUE, &n ) || !n) return WEX_ACCOUNT_MAP_INVALID;
        map->machine[i] = n;
    }
    for (line = next; *line; line = next)
    {
        struct account_record *r;
        if (map->count == ACCOUNT_LIMIT) return WEX_ACCOUNT_MAP_INVALID;
        next = strchr( line, '\n' );
        *next++ = 0;
        if (!fields( line, v, 9 )) return WEX_ACCOUNT_MAP_INVALID;
        r = &map->records[map->count++];
        if (!valid_name( v[0] ) || !parse_number( v[1], U32_MAX_VALUE - 1, &n ) || !n)
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
        if (strcmp( v[4], "21" )) return WEX_ACCOUNT_MAP_INVALID;
        for (i = 0; i < 3; i++)
        {
            if (!parse_number( v[5 + i], U32_MAX_VALUE, &n )) return WEX_ACCOUNT_MAP_INVALID;
            if (n != map->machine[i] && n != 0) return WEX_ACCOUNT_MAP_INVALID;
        }
        r->legacy = !strcmp( v[5], "0" ) && !strcmp( v[6], "0" ) && !strcmp( v[7], "0" );
        if (!r->legacy)
            for (i = 0; i < 3; i++)
            {
                parse_number( v[5 + i], U32_MAX_VALUE, &n );
                if (n != map->machine[i]) return WEX_ACCOUNT_MAP_INVALID;
            }
        if (!parse_number( v[8], U32_MAX_VALUE, &n )) return WEX_ACCOUNT_MAP_INVALID;
        r->rid = n;
        for (j = 0; j + 1 < map->count; j++)
        {
            const struct account_record *old = &map->records[j];
            if (old->legacy == r->legacy && old->rid == r->rid) return WEX_ACCOUNT_SID_COLLISION;
            if (!strcmp( old->key, r->key ) || (old->active && r->active &&
                (old->uid == r->uid || !strcmp( old->name, r->name )))) return WEX_ACCOUNT_MAP_INVALID;
        }
        if (r->legacy)
        {
            if (legacy++ || r->rid != 1000) return WEX_ACCOUNT_LEGACY_CONFLICT;
        }
        else if (r->rid < 1001 || r->rid >= map->next_rid) return WEX_ACCOUNT_MAP_INVALID;
    }
    return WEX_ACCOUNT_OK;
}

static struct passwd *native_passwd( const char *name, uid_t uid )
{
#ifdef WEX_ACCOUNTS_TEST
    if (test_passwd)
    {
        FILE *f = fopen( test_passwd, "r" );
        struct passwd *pw = NULL;
        if (!f) return NULL;
        while ((pw = fgetpwent( f )))
            if (name ? !strcmp( pw->pw_name, name ) : pw->pw_uid == uid) break;
        fclose( f );
        return pw;
    }
#endif
    return name ? getpwnam( name ) : getpwuid( uid );
}

/* Enumerate only local passwd candidates, and require two-way NSS agreement.
 * Duplicate UIDs/names or enumeration/lookup errors deny the entire snapshot. */
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
        if (!valid_name( pw->pw_name ) || *count == ACCOUNT_LIMIT || pw->pw_uid == (uid_t)-1 ||
            pw->pw_gid == (gid_t)-1) goto done;
        strcpy( u.name, pw->pw_name );
        u.uid = pw->pw_uid;
        u.gid = pw->pw_gid;
        u.supported = pw->pw_shell && pw->pw_shell[0] &&
            strcmp( pw->pw_shell, "/usr/sbin/nologin" ) && strcmp( pw->pw_shell, "/sbin/nologin" ) &&
            strcmp( pw->pw_shell, "/bin/false" ) && strcmp( pw->pw_shell, "/usr/bin/false" );
        if (!(pw = native_passwd( u.name, 0 )) || pw->pw_uid != u.uid || pw->pw_gid != u.gid ||
            strcmp( pw->pw_name, u.name )) goto done;
        if (!(pw = native_passwd( NULL, u.uid )) || pw->pw_gid != u.gid || strcmp( pw->pw_name, u.name )) goto done;
        for (j = 0; j < *count; j++)
            if (users[j].uid == u.uid || !strcmp( users[j].name, u.name )) goto done;
        users[(*count)++] = u;
    }
    status = WEX_ACCOUNT_OK;
done:
    fclose( f );
    return status;
}

static enum wex_account_status administrator_member( const struct account_native *user,
                                                     const char *group, int *member )
{
    struct group *gr = native_group( group );
    gid_t target, *gids;
    int i, count = 32;
    *member = 0;
    if (!gr) return WEX_ACCOUNT_ADMIN_GROUP_MISSING;
    target = gr->gr_gid;
    if (target == (gid_t)-1) return WEX_ACCOUNT_NSS_ERROR;
#ifdef WEX_ACCOUNTS_TEST
    if (test_group)
    {
        char **p;
        *member = user->gid == target;
        for (p = gr->gr_mem; *p; p++) if (!strcmp( *p, user->name )) *member = 1;
        return WEX_ACCOUNT_OK;
    }
#endif
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

enum wex_account_status lookup_account( const char *name, uid_t uid, uint64_t caps,
                                        struct wex_account *result )
{
    struct account_map *map = NULL;
    struct account_native *users = NULL;
    struct account_policy policy;
    struct wex_account value;
    const struct account_record *r = NULL;
    const struct account_native *u = NULL;
    char *buf = NULL;
    size_t length;
    unsigned int i, count;
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
    if (name && !valid_name( name )) goto done;
    if ((dir = protected_directory( account_directory, &status )) == -1) goto done;
    if ((status = read_policy( dir, &policy ))) goto done;
    if ((status = read_protected( dir, "accounts.map", policy.system_gid, &buf, &length ))) goto done;
    status = WEX_ACCOUNT_MAP_UNREADABLE;
    if (!(map = malloc( sizeof(*map) )) || !(users = malloc( ACCOUNT_LIMIT * sizeof(*users) ))) goto done;
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
    if ((status = native_accounts( users, &count ))) goto done;
    for (i = 0; i < count; i++) if (users[i].uid == r->uid && !strcmp( users[i].name, r->name )) u = &users[i];
    status = WEX_ACCOUNT_NSS_MISMATCH;
    if (!u) goto done;
    status = WEX_ACCOUNT_UNSUPPORTED;
    if (!u->supported) goto done;
    status = WEX_ACCOUNT_LEGACY_CONFLICT;
#ifdef WEX_ACCOUNTS_TEST
    if (u->uid == (test_server_uid ? test_server_uid : getuid()) && !r->legacy) goto done;
#else
    if (u->uid == getuid() && !r->legacy) goto done;
#endif
    if ((status = administrator_member( u, policy.admin_group, &value.administrator ))) goto done;
    strcpy( value.name, r->name );
    value.uid = u->uid;
    value.primary_gid = u->gid;
    for (i = 0; i < 16; i++)
    {
        char byte[3] = { r->key[2*i], r->key[2*i+1], 0 };
        value.incarnation[i] = strtoul( byte, NULL, 16 );
    }
    value.sid[0] = 21;
    for (i = 0; i < 3; i++) value.sid[i+1] = r->legacy ? 0 : map->machine[i];
    value.sid[4] = r->rid;
    value.generation = map->generation;
    *result = value;
    status = WEX_ACCOUNT_OK;
done:
    free( users );
    free( map );
    free( buf );
    if (dir != -1) close( dir );
#ifndef WEX_ACCOUNTS_STANDALONE
    sysrun_drop( raised );
#endif
    return status;
}
