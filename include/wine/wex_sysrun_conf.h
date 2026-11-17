/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 *
 * Protected sysrun.conf reader shared by the Unix clients, server and tools.
 */
#ifndef __WINE_WEX_SYSRUN_CONF_H
#define __WINE_WEX_SYSRUN_CONF_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/xattr.h>
#endif

enum wex_sysrun_conf_status
{
    WEX_SYSRUN_CONF_ABSENT,
    WEX_SYSRUN_CONF_VALUE,
    WEX_SYSRUN_CONF_INVALID
};

struct wex_sysrun_conf
{
    char text[4096];
    int valid;
    int absent;
};

static inline int wex_sysrun_conf_no_acl( int fd )
{
#ifdef __linux__
    ssize_t n = fgetxattr( fd, "system.posix_acl_access", NULL, 0 );
    return n < 0 && (errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
#else
    return 0;
#endif
}

/* No symlink component or writable ancestor, including sticky directories.
 * Keep descriptors throughout traversal; never resolve a checked path again. */
static inline int wex_sysrun_conf_directory( const char *path )
{
    char copy[PATH_MAX], *p, *next;
    struct stat st;
    int fd, child;

    if (!path || path[0] != '/' || strlen( path ) >= sizeof(copy)) { errno = EINVAL; return -1; }
    strcpy( copy, path + 1 );
    if ((fd = open( "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1) return -1;
    for (p = copy;; p = next)
    {
        if (fstat( fd, &st ) || !S_ISDIR( st.st_mode ) || st.st_uid ||
            (st.st_mode & 0022) || !wex_sysrun_conf_no_acl( fd )) { errno = EACCES; break; }
        if (!*p) return fd;
        if ((next = strchr( p, '/' ))) *next++ = 0;
        else next = p + strlen( p );
        if (!*p || !strcmp( p, "." ) || !strcmp( p, ".." )) { errno = EINVAL; break; }
        if ((child = openat( fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC )) == -1) break;
        close( fd );
        fd = child;
    }
    child = errno;
    close( fd );
    errno = child;
    return -1;
}

/* dir must come from protected traversal (wex_sysrun_conf_directory or the
 * accounts provider's protected_directory). A missing file is ABSENT for
 * optional consumers; administrator policy additionally requires valid. */
static inline int wex_sysrun_conf_load_at( int dir, struct wex_sysrun_conf *conf )
{
    struct stat before, after;
    char *line, *end, *eq, *p, *old;
    size_t used = 0, len;
    ssize_t n;
    int fd;

    conf->valid = 0;
    conf->absent = 0;
    conf->text[0] = 0;
    if ((fd = openat( dir, "sysrun.conf", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC )) == -1)
    {
        conf->absent = errno == ENOENT;
        return 0;
    }
    if (fstat( fd, &before ) || !S_ISREG( before.st_mode ) || before.st_uid ||
        (before.st_mode & 0022) || before.st_nlink != 1 || !wex_sysrun_conf_no_acl( fd ) ||
        before.st_size <= 0 || before.st_size >= (off_t)sizeof(conf->text)) goto done;
    for (;;)
    {
        n = read( fd, conf->text + used, sizeof(conf->text) - used );
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) break;
        used += n;
        if (used == sizeof(conf->text)) goto done;
    }
    if (fstat( fd, &after ) || used != (size_t)before.st_size || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime ||
        before.st_mode != after.st_mode || before.st_uid != after.st_uid ||
        before.st_gid != after.st_gid || after.st_nlink != 1 || !wex_sysrun_conf_no_acl( fd )) goto done;
    if (!used || memchr( conf->text, 0, used ) || conf->text[used - 1] != '\n') goto done;
    conf->text[used] = 0;
    for (line = conf->text; *line; line = end + 1)
    {
        end = strchr( line, '\n' );
        for (p = line; p < end; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) goto done;
        if (line == end || *line == '#') continue;
        if (!(eq = memchr( line, '=', end - line )) || eq == line || eq + 1 == end) goto done;
        for (p = line; p < eq; p++) if (!((*p >= 'a' && *p <= 'z') || *p == '_' ||
                                        (p > line && *p >= '0' && *p <= '9'))) goto done;
        len = eq - line;
        for (old = conf->text; old < line; old = strchr( old, '\n' ) + 1)
            if (*old != '#' && !strncmp( old, line, len ) && old[len] == '=') goto done;
    }
    conf->valid = 1;
done:
    close( fd );
    return conf->valid;
}

static inline int wex_sysrun_conf_parent( const char *path )
{
    char directory[PATH_MAX], *slash;

    if (!path || strlen( path ) >= sizeof(directory)) { errno = EINVAL; return -1; }
    strcpy( directory, path );
    if (!(slash = strrchr( directory, '/' )) || strcmp( slash + 1, "sysrun.conf" ))
    {
        errno = EINVAL;
        return -1;
    }
    if (slash == directory) slash[1] = 0;
    else *slash = 0;
    return wex_sysrun_conf_directory( directory );
}

static inline int wex_sysrun_conf_load( const char *path, struct wex_sysrun_conf *conf )
{
    int dir, ret;

    conf->valid = 0;
    conf->absent = 0;
    if ((dir = wex_sysrun_conf_parent( path )) == -1)
    {
        conf->absent = errno == ENOENT;
        return 0;
    }
    ret = wex_sysrun_conf_load_at( dir, conf );
    close( dir );
    return ret;
}

static inline enum wex_sysrun_conf_status wex_sysrun_conf_get( const struct wex_sysrun_conf *conf,
                                                              const char *key, char *value, size_t size )
{
    const char *line, *end;
    size_t keylen = strlen( key ), len;

    if (size) value[0] = 0;
    if (!conf->valid) return conf->absent ? WEX_SYSRUN_CONF_ABSENT : WEX_SYSRUN_CONF_INVALID;
    for (line = conf->text; *line; line = end + 1)
    {
        end = strchr( line, '\n' );
        if ((size_t)(end - line) <= keylen || strncmp( line, key, keylen ) || line[keylen] != '=') continue;
        len = end - line - keylen - 1;
        if (len >= size) return WEX_SYSRUN_CONF_INVALID;
        memcpy( value, line + keylen + 1, len );
        value[len] = 0;
        return WEX_SYSRUN_CONF_VALUE;
    }
    return WEX_SYSRUN_CONF_ABSENT;
}

/* The administrator policy: default only when the key is absent from a valid
 * protected file. Keep the account-name grammar in one place for all readers. */
static inline int wex_sysrun_conf_valid_name( const char *name )
{
    size_t i, len = strlen( name );

    if (!len || len > 63 || *name == '-' || !strcmp( name, "." ) || !strcmp( name, ".." )) return 0;
    for (i = 0; i < len; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_' || name[i] == '-' || name[i] == '.' ||
              (name[i] == '$' && i == len - 1))) return 0;
    return 1;
}

static inline int wex_sysrun_conf_admin_group( const struct wex_sysrun_conf *conf, char *group, size_t size )
{
    enum wex_sysrun_conf_status status;

    if (size) group[0] = 0;
    if (!conf->valid) return 0;
    status = wex_sysrun_conf_get( conf, "admin_group", group, size );
    if (status == WEX_SYSRUN_CONF_INVALID) return 0;
    if (status == WEX_SYSRUN_CONF_ABSENT)
    {
        if (size < sizeof("sudo")) return 0;
        strcpy( group, "sudo" );
    }
    return wex_sysrun_conf_valid_name( group );
}

#endif /* __WINE_WEX_SYSRUN_CONF_H */
