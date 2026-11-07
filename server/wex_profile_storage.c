/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * Descriptor-anchored E1-S12 storage. No production enablement.
 */
#ifndef WEX_PROFILE_STANDALONE
#include "config.h"
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "wex_profile_storage.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
static int hive_unchanged( const struct wex_profile_storage * );
#endif

static int valid_name( const char *name )
{
    static const char * const reserved[] = {
        "con", "prn", "aux", "nul", "clock$", "conin$", "conout$",
        "default", "public", "allusers", "root", "wexsetup", "systemprofile"
    };
    size_t i, n = strnlen( name, WEX_ACCOUNT_NAME_MAX + 1 );
    if (!n || n > WEX_ACCOUNT_NAME_MAX) return 0;
    for (i = 0; i < n; i++)
    {
        unsigned char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (i && ((c >= '0' && c <= '9') || c == '-')))) return 0;
    }
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (!strcasecmp( name, reserved[i] )) return 0;
    if (n == 4 && (!strncasecmp( name, "com", 3 ) || !strncasecmp( name, "lpt", 3 )) &&
        name[3] >= '0' && name[3] <= '9') return 0;
    return 1;
}

static void make_paths( struct wex_profile_layout *p )
{
    size_t i;
    snprintf( p->relative, sizeof(p->relative), "drive_c/users/.e1s/%s/%s", p->sid, p->leaf );
    snprintf( p->hive, sizeof(p->hive), "drive_c/users/.e1s/%s/%s/user.reg", p->sid, p->leaf );
    snprintf( p->windows, sizeof(p->windows), "C:/users/.e1s/%s/%s", p->sid, p->leaf );
    for (i = 0; p->windows[i]; i++) if (p->windows[i] == '/') p->windows[i] = '\\';
}

enum wex_profile_status wex_profile_layout( const struct wex_account *a, struct wex_profile_layout *out )
{
    struct wex_profile_layout p = {0};
    unsigned int i, nonzero = 0;
    if (!out) return WEX_PROFILE_COMPONENT;
    memset( out, 0, sizeof(*out) );
    if (!a || !valid_name( a->name ) || !a->generation || !a->administrator || !a->uid)
        return WEX_PROFILE_COMPONENT;
    for (i = 0; i < 16; i++) nonzero |= a->incarnation[i];
    if (!nonzero || a->sid[0] != 21 || !a->sid[4]) return WEX_PROFILE_COMPONENT;
    if (!a->sid[1] || !a->sid[2] || !a->sid[3] || a->sid[4] < 1001) return WEX_PROFILE_LEGACY;
    snprintf( p.sid, sizeof(p.sid), "S-1-5-21-%"PRIu32"-%"PRIu32"-%"PRIu32"-%"PRIu32,
              a->sid[1], a->sid[2], a->sid[3], a->sid[4] );
    for (i = 0; i < 16; i++) snprintf( p.incarnation + i * 2, 3, "%02x", a->incarnation[i] );
    snprintf( p.leaf, sizeof(p.leaf), "%s-%s", p.incarnation, a->name );
    for (i = 33; p.leaf[i]; i++) p.leaf[i] = tolower( (unsigned char)p.leaf[i] );
    make_paths( &p );
    *out = p;
    return WEX_PROFILE_OK;
}

#if defined(WEX_E1S12_PROFILE_TEST) || defined(WEX_E1S13_HKU_TEST)
/* This entire implementation is absent from the default object. S13/S14 must
 * supply authenticated server integration before replacing this lab gate. */
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/magic.h>
#include <linux/stat.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/xattr.h>

#define SEED_LIMIT (1024 * 1024)
#define TREE_LIMIT 16384
#define DEPTH_LIMIT 32
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

#ifdef WEX_E1S12_PROFILE_TEST
static int test_stop_step; /* set only by the separate lab CLI */
static void checkpoint( int step )
{
    if (test_stop_step == step)
    {
        printf( "checkpoint=%d\n", step );
        fflush( stdout );
        raise( SIGSTOP );
    }
}

#endif

struct chain
{
    int prefix, dos, drive, users, base, sid, lock;
    uid_t uid;
    gid_t gid;
    dev_t dev;
    ino_t ino;
    uint64_t mount_id;
    const char *sid_name;
};

static int mount_identity( int fd, uint64_t *id )
{
    struct statx st;
    if (syscall( SYS_statx, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &st ) ||
        !(st.stx_mask & STATX_MNT_ID)) return 0;
    *id = st.stx_mnt_id;
    return 1;
}

static int supported_filesystem( int fd )
{
    struct statfs st;
    if (fstatfs( fd, &st )) return 0;
    switch ((unsigned long)st.f_type)
    {
    case EXT4_SUPER_MAGIC: case XFS_SUPER_MAGIC: case BTRFS_SUPER_MAGIC:
    case TMPFS_MAGIC: case OVERLAYFS_SUPER_MAGIC: return 1;
    default: return 0;
    }
}

/* Wine's descriptor-backed reparse data (server/fd.c:XATTR_REPARSE) can
 * redirect Windows opens even on a plain Unix directory. Reject any value. */
static int no_reparse( int fd )
{
    ssize_t n = fgetxattr( fd, "user.WINEREPARSE", NULL, 0 );
    return n < 0 && (errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP);
}

/* Reject both access and default POSIX ACLs, including read-only named ACEs.
 * Windows security xattrs are S13's separate responsibility. */
static int no_posix_acl( int fd )
{
    const char *names[] = { "system.posix_acl_access", "system.posix_acl_default" };
    size_t i;
    for (i = 0; i < 2; i++)
    {
        ssize_t n = fgetxattr( fd, names[i], NULL, 0 );
        if (n >= 0 || (errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP)) return 0;
    }
    return 1;
}

static enum wex_profile_status safe_fd( int fd, const struct chain *c, int directory, int private )
{
    struct stat st;
    uint64_t mount;
    if (fstat( fd, &st )) return WEX_PROFILE_IO;
    if (st.st_uid != c->uid || st.st_gid != c->gid) return WEX_PROFILE_OWNER;
    if (st.st_dev != c->dev || (directory ? !S_ISDIR( st.st_mode ) : !S_ISREG( st.st_mode )))
        return WEX_PROFILE_SUBSTITUTION;
    if (!mount_identity( fd, &mount ) || mount != c->mount_id || !no_reparse( fd ))
        return WEX_PROFILE_SUBSTITUTION;
    if ((!directory && st.st_nlink != 1) || !no_posix_acl( fd )) return WEX_PROFILE_MODE;
    if ((st.st_mode & 07000) || (private ? (st.st_mode & 0777) != (directory ? 0700 : 0600) : (st.st_mode & 0022)))
        return WEX_PROFILE_MODE;
    return WEX_PROFILE_OK;
}

static enum wex_profile_status same_edge( int parent, const char *name, int child )
{
    struct stat named, opened;
    if (fstat( child, &opened ) || fstatat( parent, name, &named, AT_SYMLINK_NOFOLLOW ))
        return WEX_PROFILE_SUBSTITUTION;
    return named.st_dev == opened.st_dev && named.st_ino == opened.st_ino &&
           (named.st_mode & S_IFMT) == (opened.st_mode & S_IFMT) ? WEX_PROFILE_OK : WEX_PROFILE_SUBSTITUTION;
}

/* Lock order: prefix provision flock -> stable SID flock -> hive inode flock.
 * Saves hold the SID flock throughout, including replacement of user.reg.
 * Provisioners take the same SID lock; a different incarnation cannot race a
 * loaded SID. Never unlink the lock file, even after a failed load. */
static enum wex_profile_status sid_lock( const struct chain *c, int create, int *result )
{
    int fd = openat( c->sid, ".hive.lock", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
                     (create ? O_CREAT : 0), 0600 );
    enum wex_profile_status s;
    *result = -1;
    if (fd < 0) return !create && errno == ENOENT ? WEX_PROFILE_OK : WEX_PROFILE_SUBSTITUTION;
    s = safe_fd( fd, c, 0, 1 );
    if (!s) s = same_edge( c->sid, ".hive.lock", fd );
    if (!s && flock( fd, LOCK_EX | LOCK_NB )) s = errno == EWOULDBLOCK ? WEX_PROFILE_BUSY : WEX_PROFILE_IO;
    if (!s && create && fsync( c->sid )) s = WEX_PROFILE_IO;
    if (s) close( fd );
    else *result = fd;
    return s;
}

/* Any interrupted save is retained for operator recovery, including unknown
 * marker contents or a lone temporary file. Never select or delete either. */
static enum wex_profile_status save_recovery( int fd )
{
    static const char * const names[] = { ".e1s14-state", ".e1s14-save" };
    struct stat st;
    unsigned int i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (!fstatat( fd, names[i], &st, AT_SYMLINK_NOFOLLOW )) return WEX_PROFILE_RECOVERY;
        if (errno != ENOENT) return WEX_PROFILE_IO;
    }
    return WEX_PROFILE_OK;
}

/* A wrong-owner 0700/0600 object may be unopenable as Bob. Classify it from
 * the no-follow directory entry first, then still validate the opened object.
 * These observations never authorize following or repairing the entry. */
static enum wex_profile_status named_metadata( int parent, const char *name, const struct chain *c,
                                              int directory, int private )
{
    struct stat st;
    if (fstatat( parent, name, &st, AT_SYMLINK_NOFOLLOW ))
        return errno == ENOENT ? WEX_PROFILE_MAPPING : WEX_PROFILE_IO;
    if (directory ? !S_ISDIR( st.st_mode ) : !S_ISREG( st.st_mode )) return WEX_PROFILE_SUBSTITUTION;
    if (st.st_uid != c->uid || st.st_gid != c->gid) return WEX_PROFILE_OWNER;
    if ((st.st_mode & 07000) || (private ? (st.st_mode & 0777) != (directory ? 0700 : 0600) : (st.st_mode & 0022)))
        return WEX_PROFILE_MODE;
    return WEX_PROFILE_OK;
}

/* Windows name matching is case insensitive even on a case-sensitive host.
 * Do not create/select a canonical component alongside a conflicting alias. */
static enum wex_profile_status name_conflict( int parent, const char *name, int legacy )
{
    int scan = openat( parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    DIR *dir;
    struct dirent *entry;
    unsigned int count = 0;
    enum wex_profile_status s = WEX_PROFILE_OK;
    if (scan < 0) return WEX_PROFILE_IO;
    if (!(dir = fdopendir( scan ))) { close( scan ); return WEX_PROFILE_IO; }
    for (;;)
    {
        errno = 0;
        entry = readdir( dir );
        if (!entry) { if (errno) s = WEX_PROFILE_IO; break; }
        if (++count > TREE_LIMIT) { s = WEX_PROFILE_LIMIT; break; }
        if (!strcasecmp( entry->d_name, name ) && (legacy || strcmp( entry->d_name, name )))
        { s = WEX_PROFILE_LEGACY; break; }
    }
    closedir( dir );
    return s;
}

static enum wex_profile_status open_dir( int parent, const char *name, const struct chain *c,
                                        int private, int create, int *out )
{
    enum wex_profile_status s;
    int fd;
    if ((s = name_conflict( parent, name, 0 ))) return s;
    if (create && mkdirat( parent, name, 0700 ) && errno != EEXIST) return WEX_PROFILE_IO;
    if ((s = named_metadata( parent, name, c, 1, private ))) return s;
    fd = openat( parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
    if (fd < 0) return errno == ENOENT ? WEX_PROFILE_MAPPING : WEX_PROFILE_SUBSTITUTION;
    s = safe_fd( fd, c, 1, private );
    if (!s) s = same_edge( parent, name, fd );
    if (s) close( fd );
    else *out = fd;
    return s;
}

static enum wex_profile_status mapping( const struct chain *c )
{
    char link[32];
    struct stat before, after;
    ssize_t n;
    if (fstatat( c->dos, "c:", &before, AT_SYMLINK_NOFOLLOW ) || !S_ISLNK( before.st_mode ) ||
        before.st_uid != c->uid || before.st_gid != c->gid) return WEX_PROFILE_MAPPING;
    n = readlinkat( c->dos, "c:", link, sizeof(link) );
    if (n != 10 || memcmp( link, "../drive_c", 10 )) return WEX_PROFILE_MAPPING;
    if (fstatat( c->dos, "c:", &after, AT_SYMLINK_NOFOLLOW ) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino) return WEX_PROFILE_SUBSTITUTION;
    return WEX_PROFILE_OK;
}

static enum wex_profile_status recheck( const struct chain *c )
{
    enum wex_profile_status s;
    struct stat st;
    int dirs[] = { c->prefix, c->dos, c->drive, c->users, c->base, c->sid };
    int parents[] = { c->prefix, c->prefix, c->drive, c->users, c->base };
    int children[] = { c->dos, c->drive, c->users, c->base, c->sid };
    const char *names[] = { "dosdevices", "drive_c", "users", ".e1s", c->sid_name };
    size_t i;
    if (fstat( c->prefix, &st ) || st.st_dev != c->dev || st.st_ino != c->ino)
        return WEX_PROFILE_SUBSTITUTION;
    for (i = 0; i < 6; i++) if ((s = safe_fd( dirs[i], c, 1, i >= 4 ))) return s;
    for (i = 0; i < 5; i++) if ((s = same_edge( parents[i], names[i], children[i] ))) return s;
    if ((s = safe_fd( c->lock, c, 0, 1 )) || (s = same_edge( c->base, ".profile.lock", c->lock ))) return s;
    return mapping( c );
}

#ifdef WEX_E1S12_PROFILE_TEST
static enum wex_profile_status write_file( int dir, const char *name, const void *data, size_t size )
{
    const char *p = data;
    int fd = openat( dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600 );
    enum wex_profile_status s = WEX_PROFILE_IO;
    if (fd < 0) return s;
    while (size)
    {
        ssize_t n = write( fd, p, size );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        p += n;
        size -= n;
    }
    if (!fsync( fd )) s = WEX_PROFILE_OK;
done:
    if (close( fd ) && !s) s = WEX_PROFILE_IO;
    return s;
}

static int random_bytes( void *data, size_t size )
{
    unsigned char *p = data;
    while (size)
    {
        ssize_t n = getrandom( p, size, 0 );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        size -= n;
    }
    return 1;
}

/* Reuse ONLY a bounded, audited projection of the product's offline default
 * template. Never import arbitrary keys, paths, Run entries or setup secrets.
 * No parser/general personalization script runs against any live hive. */
static enum wex_profile_status seed_default( int fd, unsigned int *hide )
{
    static const char key[] = "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\Advanced]";
    struct stat st, after;
    char *buf, *line, *save;
    size_t used = 0;
    int selected = 0, seen = 0;
    enum wex_profile_status s = WEX_PROFILE_SEED;
    *hide = 1; /* wine.inf.in's default if missing from the offline template */
    if (fstat( fd, &st ) || !S_ISREG( st.st_mode ) || st.st_uid || (st.st_mode & 0022) ||
        st.st_nlink != 1 || !no_posix_acl( fd ) || !no_reparse( fd ) || st.st_size < 24 || st.st_size > SEED_LIMIT) return s;
    buf = malloc( (size_t)st.st_size + 1 );
    if (!buf) return WEX_PROFILE_IO;
    while (used < (size_t)st.st_size)
    {
        ssize_t n = pread( fd, buf + used, (size_t)st.st_size - used, used );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += n;
    }
    if (fstat( fd, &after ) || st.st_size != after.st_size ||
        st.st_mtim.tv_sec != after.st_mtim.tv_sec || st.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        st.st_ctim.tv_sec != after.st_ctim.tv_sec || st.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        memchr( buf, 0, used ) || buf[used - 1] != '\n' ||
        used < 24 || memcmp( buf, "WINE REGISTRY Version 2\n", 24 )) goto done;
    buf[used] = 0;
    for (line = strtok_r( buf, "\n", &save ); line; line = strtok_r( NULL, "\n", &save ))
    {
        if (*line == '[')
            selected = !strncmp( line, key, sizeof(key) - 1 ) &&
                       (line[sizeof(key) - 1] == 0 || line[sizeof(key) - 1] == ' ');
        else if (selected && !strncmp( line, "\"HideFileExt\"", 13 ))
        {
            if (seen++ || (strcmp( line, "\"HideFileExt\"=dword:00000000" ) &&
                           strcmp( line, "\"HideFileExt\"=dword:00000001" ))) goto done;
            *hide = line[strlen(line) - 1] == '1';
        }
    }
    s = WEX_PROFILE_OK;
done:
    free( buf );
    return s;
}

#endif

/* Audited subset of shellpath.c CSIDL_Data/_SHRegisterUserShellFolders plus
 * create_extra_folders. Host-home links and machine Fonts are excluded. */
static const struct folder_default { const char *value, *path; } folders[] = {
    { "Desktop", "Desktop" }, { "Personal", "Documents" }, { "Favorites", "Favorites" },
    { "AppData", "AppData/Roaming" }, { "Local AppData", "AppData/Local" },
    { "Start Menu", "AppData/Roaming/Microsoft/Windows/Start Menu" },
    { "Programs", "AppData/Roaming/Microsoft/Windows/Start Menu/Programs" },
    { "Startup", "AppData/Roaming/Microsoft/Windows/Start Menu/Programs/StartUp" },
    { "Recent", "AppData/Roaming/Microsoft/Windows/Recent" },
    { "SendTo", "AppData/Roaming/Microsoft/Windows/SendTo" },
    { "Templates", "AppData/Roaming/Microsoft/Windows/Templates" },
    { "NetHood", "AppData/Roaming/Microsoft/Windows/Network Shortcuts" },
    { "PrintHood", "AppData/Roaming/Microsoft/Windows/Printer Shortcuts" },
    { "AdminTools", "AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Administrative Tools" },
    { "My Music", "Music" }, { "My Pictures", "Pictures" }, { "My Videos", "Videos" },
    { "Cache", "AppData/Local/Microsoft/Windows/INetCache" },
    { "Cookies", "AppData/Local/Microsoft/Windows/INetCookies" },
    { "History", "AppData/Local/Microsoft/Windows/History" },
    { "{374DE290-123F-4565-9164-39C4925E467B}", "Downloads" },
    { "{56784854-C6CB-462B-8169-88E350ACB882}", "Contacts" },
    { "{BFB9D5E0-C6A9-404C-B2B2-AE6DB6AF4968}", "Links" },
    { "{4C5C32FF-BB9D-43B0-BF7D-49B8E0D9A4C4}", "Saved Games" },
    { "{7D1D3A04-DEBB-4115-95CF-2F29DA2920DA}", "Searches" },
    { "{A520A1A4-1780-4FF6-BD18-167343C5AF16}", "AppData/LocalLow" },
    { NULL, "AppData/Local/Temp" }, { NULL, "AppData/Roaming/Microsoft/Windows/Themes" },
    { NULL, "AppData/Roaming/Microsoft/Windows/AccountPictures" }, { NULL, "Pictures/Screenshots" }
};

#ifdef WEX_E1S12_PROFILE_TEST
static enum wex_profile_status mkdir_path( int root, const char *path, const struct chain *c )
{
    char copy[128], *part, *save;
    int fd = fcntl( root, F_DUPFD_CLOEXEC, 3 ), child = -1;
    enum wex_profile_status s = WEX_PROFILE_IO;
    if (fd < 0) return s;
    if (strlen(path) >= sizeof(copy)) { close( fd ); return WEX_PROFILE_LIMIT; }
    strcpy( copy, path );
    for (part = strtok_r( copy, "/", &save ); part; part = strtok_r( NULL, "/", &save ))
    {
        s = open_dir( fd, part, c, 1, 1, &child );
        if (s) break;
        if (fsync( fd )) { close( child ); s = WEX_PROFILE_IO; break; }
        close( fd );
        fd = child;
    }
    if (!s && fsync( fd )) s = WEX_PROFILE_IO;
    close( fd );
    return s;
}

#endif

static enum wex_profile_status check_skeleton( int root, const struct chain *c )
{
    size_t i;
    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
    {
        char copy[128], *part, *save;
        int fd = fcntl( root, F_DUPFD_CLOEXEC, 3 ), child = -1;
        enum wex_profile_status s = WEX_PROFILE_OK;
        if (fd < 0) return WEX_PROFILE_IO;
        strcpy( copy, folders[i].path ); /* trusted table, all <128 bytes */
        for (part = strtok_r( copy, "/", &save ); part; part = strtok_r( NULL, "/", &save ))
        {
            s = open_dir( fd, part, c, 1, 0, &child );
            if (s) break;
            close( fd );
            fd = child;
        }
        close( fd );
        if (s) return s == WEX_PROFILE_MAPPING ? WEX_PROFILE_INCOMPLETE : s;
    }
    return WEX_PROFILE_OK;
}

#ifdef WEX_E1S12_PROFILE_TEST
static void reg_string( FILE *f, const char *s )
{
    for (; *s; s++)
    {
        if (*s == '/' || *s == '\\') fputs( "\\\\", f );
        else { if (*s == '"') fputc( '\\', f ); fputc( *s, f ); }
    }
}

static enum wex_profile_status skeleton( int dir, const struct chain *c,
                                        const struct wex_profile_layout *p, unsigned int hide )
{
    enum wex_profile_status s;
    char *hive = NULL;
    size_t length = 0, i;
    int section;
    FILE *f;
    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
        if ((s = mkdir_path( dir, folders[i].path, c ))) return s;
    f = open_memstream( &hive, &length );
    if (!f) return WEX_PROFILE_IO;
    fputs( "WINE REGISTRY Version 2\n\n[Environment] 0\n\"TEMP\"=\"", f );
    reg_string( f, p->windows ); fputs( "\\\\AppData\\\\Local\\\\Temp\"\n\"TMP\"=\"", f );
    reg_string( f, p->windows ); fputs( "\\\\AppData\\\\Local\\\\Temp\"\n", f );
    fprintf( f, "\n[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\Advanced] 0\n"
                "\"HideFileExt\"=dword:%08x\n\n[Software\\\\Classes] 0\n", hide );
    for (section = 0; section < 2; section++)
    {
        fprintf( f, "\n[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\%s] 0\n",
                 section ? "User Shell Folders" : "Shell Folders" );
        for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
        {
            if (!folders[i].value) continue;
            fprintf( f, "\"%s\"=%s\"", folders[i].value, section ? "str(2):" : "" );
            reg_string( f, p->windows );
            fputs( "\\\\", f ); reg_string( f, folders[i].path ); fputs( "\"\n", f );
        }
    }
    s = ferror( f ) ? WEX_PROFILE_IO : WEX_PROFILE_OK;
    if (fclose( f )) s = WEX_PROFILE_IO;
    if (!s) s = write_file( dir, "user.reg", hive, length );
    free( hive );
    return s;
}

#endif

static void acl_intent( const struct wex_profile_layout *p, char *text, size_t size )
{
    snprintf( text, size, "O:%sG:%sD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%s)S:(ML;OICI;NW;;;HI)",
              p->sid, p->sid, p->sid );
}

static int manifest( char *text, size_t size, const struct chain *c,
                     const struct wex_profile_layout *p, uint64_t generation )
{
    char acl[320];
    acl_intent( p, acl, sizeof(acl) );
    return snprintf( text, size, "WEXPROFILE 1\nsid=%s\nincarnation=%s\nlabel=%s\n"
        "prefix_dev=%ju\nprefix_ino=%ju\ngeneration=%"PRIu64"\nseed=1\nacl=S13_PENDING\nintent=%s\n",
        p->sid, p->incarnation, p->leaf + 33, (uintmax_t)c->dev, (uintmax_t)c->ino, generation, acl );
}

/* Full no-follow ownership/mode/type/link scan, bounded against hostile trees.
 * Existing user data is allowed; symlinks, devices and hardlinks are not.
 * remove=1 is used ONLY for unpublished .txn-<random128> directories under lock.
 */
static enum wex_profile_status walk_tree( int fd, const struct chain *c, unsigned int depth,
                                         unsigned int *count, int remove )
{
    int scan = openat( fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    DIR *dir;
    struct dirent *entry;
    enum wex_profile_status s = WEX_PROFILE_OK;
    if (depth > DEPTH_LIMIT || ++*count > TREE_LIMIT) { if (scan >= 0) close( scan ); return WEX_PROFILE_LIMIT; }
    if (scan < 0) return WEX_PROFILE_IO;
    if (!(dir = fdopendir( scan ))) { close( scan ); return WEX_PROFILE_IO; }
    for (;;)
    {
        struct stat st;
        int child;
        errno = 0;
        entry = readdir( dir );
        if (!entry) { if (errno) s = WEX_PROFILE_IO; break; }
        if (!strcmp( entry->d_name, "." ) || !strcmp( entry->d_name, ".." )) continue;
        if (++*count > TREE_LIMIT) { s = WEX_PROFILE_LIMIT; break; }
        if (fstatat( fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW )) { s = WEX_PROFILE_IO; break; }
        if (!S_ISDIR( st.st_mode ) && !S_ISREG( st.st_mode )) { s = WEX_PROFILE_SUBSTITUTION; break; }
        if ((s = named_metadata( fd, entry->d_name, c, S_ISDIR( st.st_mode ), 1 ))) break;
        child = openat( fd, entry->d_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
                        (S_ISDIR( st.st_mode ) ? O_DIRECTORY : 0) );
        if (child < 0) { s = WEX_PROFILE_SUBSTITUTION; break; }
        s = safe_fd( child, c, S_ISDIR( st.st_mode ), 1 );
        if (!s) s = same_edge( fd, entry->d_name, child );
        if (!s && S_ISDIR( st.st_mode )) s = walk_tree( child, c, depth + 1, count, remove );
        if (!s && remove) s = same_edge( fd, entry->d_name, child );
        if (!s && remove && unlinkat( fd, entry->d_name, S_ISDIR( st.st_mode ) ? AT_REMOVEDIR : 0 )) s = WEX_PROFILE_IO;
        close( child );
        if (s) break;
    }
    closedir( dir );
    return s;
}

static int temp_name( const char *name )
{
    size_t i;
    if (strlen( name ) != 37 || strncmp( name, ".txn-", 5 )) return 0;
    for (i = 5; i < 37; i++) if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return 1;
}

static enum wex_profile_status discard_temp( int parent, const char *name, const struct chain *c )
{
    int fd = -1;
    unsigned int count = 0;
    enum wex_profile_status s = open_dir( parent, name, c, 1, 0, &fd );
    if (s) return s;
    s = walk_tree( fd, c, 0, &count, 0 ); /* preflight before deleting anything */
    count = 0;
    if (!s) s = walk_tree( fd, c, 0, &count, 1 );
    if (!s) s = same_edge( parent, name, fd );
    if (!s && (unlinkat( parent, name, AT_REMOVEDIR ) || fsync( parent ))) s = WEX_PROFILE_IO;
    close( fd );
    return s;
}

/* Select stored label for a genuine S4 rename; never infer identity from name
 * or UID. More than one published label for the key is an identity conflict. */
static enum wex_profile_status find_existing( const struct chain *c, struct wex_profile_layout *p, int *exists,
                                              int cleanup, int saving )
{
    int scan = openat( c->sid, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    DIR *dir;
    struct dirent *entry;
    enum wex_profile_status s = WEX_PROFILE_OK;
    unsigned int count = 0;
    *exists = 0;
    if (scan < 0) return WEX_PROFILE_IO;
    if (!(dir = fdopendir( scan ))) { close( scan ); return WEX_PROFILE_IO; }
    for (;;)
    {
        errno = 0;
        entry = readdir( dir );
        if (!entry) { if (errno) s = WEX_PROFILE_IO; break; }
        if (++count > TREE_LIMIT) { s = WEX_PROFILE_LIMIT; break; }
        /* Recovery is SID-wide, including a retired incarnation. A new key
         * must not hide an interrupted save in an older published profile. */
        if (strlen( entry->d_name ) > 33 && entry->d_name[32] == '-')
        {
            unsigned int i;
            int fd = -1;
            for (i = 0; i < 32; i++)
                if (!((entry->d_name[i] >= '0' && entry->d_name[i] <= '9') ||
                      (entry->d_name[i] >= 'a' && entry->d_name[i] <= 'f'))) break;
            if (i == 32 && !(saving && !strncmp( entry->d_name, p->incarnation, 32 )))
            {
                s = open_dir( c->sid, entry->d_name, c, 1, 0, &fd );
                if (!s) s = save_recovery( fd );
                if (fd >= 0) close( fd );
                if (s) break;
            }
        }
        if (temp_name( entry->d_name ))
        {
            s = cleanup ? discard_temp( c->sid, entry->d_name, c ) : WEX_PROFILE_BUSY;
            if (s) break;
        }
        else if (!strncasecmp( entry->d_name, p->incarnation, 32 ) && entry->d_name[32] == '-')
        {
            if (*exists || strncmp( entry->d_name, p->incarnation, 32 ) ||
                strlen( entry->d_name ) >= sizeof(p->leaf) || !valid_name( entry->d_name + 33 ))
            { s = WEX_PROFILE_IDENTITY; break; }
            strcpy( p->leaf, entry->d_name );
            *exists = 1;
        }
    }
    closedir( dir );
    if (!s) make_paths( p );
    return s;
}

static enum wex_profile_status validate_profile( const struct chain *c, struct wex_profile_storage *out, int recovery )
{
    char text[1024], expected[1024], header[24], *line, *end;
    int meta = -1;
    unsigned int count = 0;
    ssize_t n;
    uint64_t gen;
    enum wex_profile_status s = open_dir( c->sid, out->path.leaf, c, 1, 0, &out->profile_fd );
    if (s) return s;
    if (recovery && (s = save_recovery( out->profile_fd ))) return s;
    s = named_metadata( out->profile_fd, ".complete", c, 0, 1 );
    if (s) return s == WEX_PROFILE_MAPPING ? WEX_PROFILE_LEGACY : s;
    meta = openat( out->profile_fd, ".complete", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC );
    if (meta < 0) return errno == ENOENT ? WEX_PROFILE_LEGACY : WEX_PROFILE_SUBSTITUTION;
    s = safe_fd( meta, c, 0, 1 );
    if (!s) s = same_edge( out->profile_fd, ".complete", meta );
    if (s) { close( meta ); return s; }
    n = pread( meta, text, sizeof(text) - 1, 0 );
    close( meta );
    if (n <= 0 || n >= (ssize_t)sizeof(text) - 1 || memchr( text, 0, n )) return WEX_PROFILE_IDENTITY;
    text[n] = 0;
    line = strstr( text, "\ngeneration=" );
    if (!line || line[12] < '1' || line[12] > '9') return WEX_PROFILE_IDENTITY;
    errno = 0;
    gen = strtoull( line + 12, &end, 10 );
    if (errno || !gen || *end != '\n') return WEX_PROFILE_IDENTITY;
    if (manifest( expected, sizeof(expected), c, &out->path, gen ) != n || strcmp( text, expected ))
        return WEX_PROFILE_IDENTITY;
    s = check_skeleton( out->profile_fd, c );
    if (!s) s = walk_tree( out->profile_fd, c, 0, &count, 0 );
    if (s) return s;
    out->hive_fd = openat( out->profile_fd, "user.reg", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC );
    if (out->hive_fd < 0) return errno == ENOENT ? WEX_PROFILE_INCOMPLETE : WEX_PROFILE_SUBSTITUTION;
    if ((s = safe_fd( out->hive_fd, c, 0, 1 )) || (s = same_edge( out->profile_fd, "user.reg", out->hive_fd ))) return s;
    if (pread( out->hive_fd, header, sizeof(header), 0 ) != (ssize_t)sizeof(header) ||
        memcmp( header, "WINE REGISTRY Version 2\n", sizeof(header) )) return WEX_PROFILE_INCOMPLETE;
    out->profile_generation = gen;
    out->acl_pending = 1;
    acl_intent( &out->path, out->acl_intent, sizeof(out->acl_intent) );
    return same_edge( c->sid, out->path.leaf, out->profile_fd );
}
#endif

/* No-create load-side validation. Never seeds or removes stale transactions. */
static enum wex_profile_status open_existing( int prefix, dev_t dev, ino_t ino, uid_t bob,
    gid_t group, const struct wex_account *account, uint64_t generation, struct wex_profile_storage *out, int lock_hive )
{
    if (!out) return WEX_PROFILE_COMPONENT;
    memset( out, 0, sizeof(*out) );
    out->profile_fd = out->hive_fd = out->sid_lock_fd = -1;
#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    struct chain c = { prefix, -1, -1, -1, -1, -1, -1, bob, group, dev, ino, 0, NULL };
    struct stat st;
    enum wex_profile_status s;
    int exists = 0;
    if (!bob || geteuid() != bob || getegid() != group || !generation)
    { s = WEX_PROFILE_OWNER; goto done; }
    if ((s = wex_profile_layout( account, &out->path ))) goto done;
    c.sid_name = out->path.sid;
    if (!supported_filesystem( prefix ) || !mount_identity( prefix, &c.mount_id ))
    { s = WEX_PROFILE_FILESYSTEM; goto done; }
    if ((s = safe_fd( prefix, &c, 1, 0 ))) goto done;
    if (fstat( prefix, &st ) || st.st_ino != ino) { s = WEX_PROFILE_SUBSTITUTION; goto done; }
    if (!fstatat( prefix, ".e1s", &st, AT_SYMLINK_NOFOLLOW )) { s = WEX_PROFILE_LEGACY; goto done; }
    if (errno != ENOENT) { s = WEX_PROFILE_IO; goto done; }
    if ((s = open_dir( prefix, "dosdevices", &c, 0, 0, &c.dos )) || (s = mapping( &c )) ||
        (s = open_dir( prefix, "drive_c", &c, 0, 0, &c.drive )) ||
        (s = open_dir( c.drive, "users", &c, 0, 0, &c.users )) ||
        (s = name_conflict( c.users, account->name, 1 )) ||
        (s = open_dir( c.users, ".e1s", &c, 1, 0, &c.base ))) goto done;
    if ((s = named_metadata( c.base, ".profile.lock", &c, 0, 1 ))) goto done;
    c.lock = openat( c.base, ".profile.lock", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC );
    if (c.lock < 0) { s = WEX_PROFILE_SUBSTITUTION; goto done; }
    if ((s = safe_fd( c.lock, &c, 0, 1 )) || (s = same_edge( c.base, ".profile.lock", c.lock ))) goto done;
    if (flock( c.lock, LOCK_EX | LOCK_NB )) { s = WEX_PROFILE_BUSY; goto done; }
    if ((s = open_dir( c.base, c.sid_name, &c, 1, 0, &c.sid )) || (s = recheck( &c ))) goto done;
    if (lock_hive > 0)
    {
        if ((s = sid_lock( &c, 1, &out->sid_lock_fd ))) goto done;
    }
    else
    {
        out->sid_lock_fd = openat( c.sid, ".hive.lock", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC );
        if (out->sid_lock_fd < 0) { s = WEX_PROFILE_SUBSTITUTION; goto done; }
        if ((s = safe_fd( out->sid_lock_fd, &c, 0, 1 )) ||
            (s = same_edge( c.sid, ".hive.lock", out->sid_lock_fd ))) goto done;
    }
    if ((s = find_existing( &c, &out->path, &exists, 0, lock_hive < 0 ))) goto done;
    if (!exists) { s = WEX_PROFILE_ABSENT; goto done; }
    if ((s = validate_profile( &c, out, lock_hive >= 0 )) || (s = recheck( &c ))) goto done;
    if (out->profile_generation != generation) { s = WEX_PROFILE_IDENTITY; goto done; }
    /* Stable SID lock survives subsequent atomic replacement of this inode. */
    if (lock_hive > 0 && flock( out->hive_fd, LOCK_EX | LOCK_NB )) { s = WEX_PROFILE_BUSY; goto done; }
    if (fstat( out->hive_fd, &out->hive_identity )) { s = WEX_PROFILE_IO; goto done; }
    out->reused = 1;
done:
    if (c.sid >= 0) close( c.sid );
    if (c.lock >= 0) close( c.lock );
    if (c.base >= 0) close( c.base );
    if (c.users >= 0) close( c.users );
    if (c.drive >= 0) close( c.drive );
    if (c.dos >= 0) close( c.dos );
    if (s)
    {
        if (out->profile_fd >= 0) close( out->profile_fd );
        if (out->hive_fd >= 0) close( out->hive_fd );
        if (out->sid_lock_fd >= 0) close( out->sid_lock_fd );
        memset( out, 0, sizeof(*out) );
        out->profile_fd = out->hive_fd = out->sid_lock_fd = -1;
    }
    return s == WEX_PROFILE_MAPPING || s == WEX_PROFILE_INCOMPLETE ? WEX_PROFILE_ABSENT : s;
#else
    (void)prefix; (void)dev; (void)ino; (void)bob; (void)group; (void)account; (void)generation;
    (void)lock_hive;
    return WEX_PROFILE_DISABLED;
#endif
}

enum wex_profile_status wex_profile_open_existing( int prefix, dev_t dev, ino_t ino, uid_t bob,
    gid_t group, const struct wex_account *account, uint64_t generation, struct wex_profile_storage *out )
{
    return open_existing( prefix, dev, ino, bob, group, account, generation, out, 1 );
}

static enum wex_profile_status verify_existing( int prefix, const struct wex_account *a,
                                                const struct wex_profile_storage *held, int saving )
{
#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    struct stat p, h, reopened;
    struct wex_profile_storage check;
    enum wex_profile_status s;
    if (fstat( prefix, &p )) return WEX_PROFILE_IO;
    s = open_existing( prefix, p.st_dev, p.st_ino, getuid(), getgid(), a, held->profile_generation, &check, saving ? -1 : 0 );
    if (s) return s;
    if (fstat( held->profile_fd, &h ) || fstat( check.profile_fd, &reopened ) ||
        h.st_dev != reopened.st_dev || h.st_ino != reopened.st_ino ||
        fstat( held->hive_fd, &h ) || fstat( check.hive_fd, &reopened ) ||
        h.st_dev != reopened.st_dev || h.st_ino != reopened.st_ino ||
        fstat( held->sid_lock_fd, &h ) || fstat( check.sid_lock_fd, &reopened ) ||
        h.st_dev != reopened.st_dev || h.st_ino != reopened.st_ino) s = WEX_PROFILE_SUBSTITUTION;
    close( check.profile_fd ); close( check.hive_fd ); close( check.sid_lock_fd );
    return s;
#else
    (void)prefix; (void)a; (void)held; (void)saving;
    return WEX_PROFILE_DISABLED;
#endif
}

enum wex_profile_status wex_profile_verify_existing( int prefix, const struct wex_account *a,
                                                     const struct wex_profile_storage *held )
{
    enum wex_profile_status status = verify_existing( prefix, a, held, 0 );
#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    if (!status && !hive_unchanged( held )) status = WEX_PROFILE_IDENTITY;
#endif
    return status;
}

void wex_profile_storage_close( struct wex_profile_storage *s )
{
#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    /* Security pins duplicate the hive's open description. Explicit unlock is
     * necessary; closing just our descriptor would retain its flock. */
    if (s->hive_fd >= 0) flock( s->hive_fd, LOCK_UN );
#endif
    if (s->hive_fd >= 0) close( s->hive_fd );
    if (s->profile_fd >= 0) close( s->profile_fd );
    if (s->sid_lock_fd >= 0) close( s->sid_lock_fd );
    s->hive_fd = s->profile_fd = s->sid_lock_fd = -1;
}

#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
static int hive_unchanged( const struct wex_profile_storage *s )
{
    struct stat st;
    const struct stat *old = &s->hive_identity;
    return !fstat( s->hive_fd, &st ) && !same_edge( s->profile_fd, "user.reg", s->hive_fd ) &&
           st.st_dev == old->st_dev && st.st_ino == old->st_ino && st.st_size == old->st_size &&
           st.st_mode == old->st_mode && st.st_uid == old->st_uid && st.st_gid == old->st_gid && st.st_nlink == 1 &&
           st.st_mtim.tv_sec == old->st_mtim.tv_sec && st.st_mtim.tv_nsec == old->st_mtim.tv_nsec &&
           st.st_ctim.tv_sec == old->st_ctim.tv_sec && st.st_ctim.tv_nsec == old->st_ctim.tv_nsec;
}

static void save_checkpoint( const struct wex_profile_storage *s, unsigned int step )
{
    (void)s; (void)step;
#ifdef WEX_E1S14_CRASH_TEST
    if (s->test_crash_step == step)
    {
        fprintf( stderr, "E1S14 save-crash=%u\n", step ); fflush( stderr );
        _exit( 86 ); /* no signal handler/shutdown save */
    }
#endif
}
#endif

enum wex_profile_status wex_profile_save( int prefix, const struct wex_account *a,
    struct wex_profile_storage *s, int (*writer)(int, void *), void *arg )
{
#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    enum wex_profile_status status;
    struct stat st;
    int marker = -1, temp = -1;
    /* Verification takes S12's prefix lock while our stable SID lock excludes
     * provisioning. It does not reacquire our own flock or follow a path from
     * a Wine client. The same directory and inode are checked again at commit. */
    if (s->sid_lock_fd < 0 || (status = save_recovery( s->profile_fd )))
        return s->sid_lock_fd < 0 ? WEX_PROFILE_IDENTITY : status;
    if ((status = wex_profile_verify_existing( prefix, a, s ))) return status;
    if (!hive_unchanged( s )) return WEX_PROFILE_IDENTITY;
    marker = openat( s->profile_fd, ".e1s14-state", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600 );
    if (marker < 0) return errno == EEXIST ? WEX_PROFILE_RECOVERY : WEX_PROFILE_IO;
    status = WEX_PROFILE_IO;
    if (fsync( marker ) || fsync( s->profile_fd )) goto done;
    save_checkpoint( s, 1 );
    temp = openat( s->profile_fd, ".e1s14-save", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600 );
    if (temp < 0 || flock( temp, LOCK_EX | LOCK_NB ) || !writer( temp, arg ) || fsync( temp ) ||
        fstat( temp, &st ) || !S_ISREG( st.st_mode ) || st.st_nlink != 1 || st.st_size > SEED_LIMIT ||
        st.st_uid != getuid() || st.st_gid != getgid() || (st.st_mode & 07777) != 0600 ||
        !no_reparse( temp ) || !no_posix_acl( temp ) || fsync( s->profile_fd )) goto done;
    save_checkpoint( s, 2 );
    if (verify_existing( prefix, a, s, 1 ) || !hive_unchanged( s ) || same_edge( s->profile_fd, ".e1s14-save", temp ) ||
        same_edge( s->profile_fd, ".e1s14-state", marker )) { status = WEX_PROFILE_IDENTITY; goto done; }
    if (renameat( s->profile_fd, ".e1s14-save", s->profile_fd, "user.reg" ) || fsync( s->profile_fd )) goto done;
    save_checkpoint( s, 3 );
    /* Do not remove the marker until the complete new hive and its directory
     * entry are durable. Any failure retains all remaining recovery material. */
    if (same_edge( s->profile_fd, "user.reg", temp ) ||
        unlinkat( s->profile_fd, ".e1s14-state", 0 ) || fsync( s->profile_fd )) goto done;
    flock( s->hive_fd, LOCK_UN );
    close( s->hive_fd );
    s->hive_fd = temp; temp = -1;
    s->hive_identity = st;
    /* rename can change ctime. Capture the authoritative post-publication stat. */
    if (fstat( s->hive_fd, &s->hive_identity )) goto done;
    status = WEX_PROFILE_OK;
done:
    if (temp >= 0) { flock( temp, LOCK_UN ); close( temp ); }
    close( marker );
    return status;
#else
    (void)prefix; (void)a; (void)s; (void)writer; (void)arg;
    return WEX_PROFILE_DISABLED;
#endif
}

enum wex_profile_status wex_profile_prepare( int prefix, dev_t dev, ino_t ino, uid_t bob,
    gid_t group, int seed, const struct wex_account *account, struct wex_profile_storage *out )
{
    if (!out) return WEX_PROFILE_COMPONENT;
    memset( out, 0, sizeof(*out) );
    out->profile_fd = out->hive_fd = out->sid_lock_fd = -1;
#ifndef WEX_E1S12_PROFILE_TEST
    (void)prefix; (void)dev; (void)ino; (void)bob; (void)group; (void)seed; (void)account;
    return WEX_PROFILE_DISABLED;
#else
    struct chain c = { prefix, -1, -1, -1, -1, -1, -1, bob, group, dev, ino, 0, NULL };
    enum wex_profile_status s;
    int exists = 0, temp = -1, published = 0, held_sid = -1;
    char tempname[38] = {0}, text[1024];
    unsigned char random[16];
    unsigned int hide, i;
    uint64_t generation;
    struct stat st;
    /* Ownership is naturally Bob's: no privileged chown or permission repair
     * of caller-controlled paths. No process/group identity is changed here. */
    if (!bob || geteuid() != bob || getegid() != group) { s = WEX_PROFILE_OWNER; goto done; }
    s = wex_profile_layout( account, &out->path );
    if (s) goto done;
    c.sid_name = out->path.sid;
    if (!supported_filesystem( prefix ) || !mount_identity( prefix, &c.mount_id ))
    { s = WEX_PROFILE_FILESYSTEM; goto done; }
    if ((s = safe_fd( prefix, &c, 1, 0 ))) goto done;
    if (fstat( prefix, &st ) || st.st_ino != ino) { s = WEX_PROFILE_SUBSTITUTION; goto done; }
    if ((s = open_dir( prefix, "dosdevices", &c, 0, 0, &c.dos )) || (s = mapping( &c )) ||
        (s = open_dir( prefix, "drive_c", &c, 0, 0, &c.drive )) ||
        (s = open_dir( c.drive, "users", &c, 0, 0, &c.users ))) goto done;
    /* Refuse legacy name-only and abandoned preliminary split-tree layouts.
     * A symlink is an entry too; never follow it or import/migrate its data. */
    if ((s = name_conflict( c.users, account->name, 1 ))) goto done;
    if (!fstatat( prefix, ".e1s", &st, AT_SYMLINK_NOFOLLOW )) { s = WEX_PROFILE_LEGACY; goto done; }
    if (errno != ENOENT) { s = WEX_PROFILE_IO; goto done; }
    if ((s = open_dir( c.users, ".e1s", &c, 1, 1, &c.base ))) goto done;
    s = named_metadata( c.base, ".profile.lock", &c, 0, 1 );
    if (s && s != WEX_PROFILE_MAPPING) goto done;
    c.lock = openat( c.base, ".profile.lock", O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600 );
    if (c.lock < 0) { s = WEX_PROFILE_SUBSTITUTION; goto done; }
    if ((s = safe_fd( c.lock, &c, 0, 1 )) || (s = same_edge( c.base, ".profile.lock", c.lock ))) goto done;
    if (flock( c.lock, LOCK_EX | LOCK_NB )) { s = errno == EWOULDBLOCK ? WEX_PROFILE_BUSY : WEX_PROFILE_IO; goto done; }
    if ((s = open_dir( c.base, c.sid_name, &c, 1, 1, &c.sid )) || (s = recheck( &c ))) goto done;
    if ((s = sid_lock( &c, 0, &held_sid ))) goto done;
    checkpoint( 1 );
    if ((s = find_existing( &c, &out->path, &exists, 1, 0 ))) goto done;
    if (!exists)
    {
        if ((s = seed_default( seed, &hide ))) goto done;
        if (!random_bytes( random, sizeof(random) ) || !random_bytes( &generation, sizeof(generation) ) || !generation)
        { s = WEX_PROFILE_IO; goto done; }
        strcpy( tempname, ".txn-" );
        for (i = 0; i < 16; i++) snprintf( tempname + 5 + 2 * i, 3, "%02x", random[i] );
        if (mkdirat( c.sid, tempname, 0700 )) { tempname[0] = 0; s = WEX_PROFILE_IO; goto done; }
        if ((s = open_dir( c.sid, tempname, &c, 1, 0, &temp ))) goto done;
        checkpoint( 2 );
        if ((s = skeleton( temp, &c, &out->path, hide ))) goto done;
        checkpoint( 3 );
        i = manifest( text, sizeof(text), &c, &out->path, generation );
        if (i >= sizeof(text)) { s = WEX_PROFILE_LIMIT; goto done; }
        if ((s = write_file( temp, ".complete", text, i ))) goto done;
        if (fsync( temp )) { s = WEX_PROFILE_IO; goto done; }
        checkpoint( 4 );
        if ((s = recheck( &c )) || (s = same_edge( c.sid, tempname, temp ))) goto done;
        {
            unsigned int count = 0;
            if ((s = check_skeleton( temp, &c )) || (s = walk_tree( temp, &c, 0, &count, 0 ))) goto done;
        }
        if (syscall( SYS_renameat2, c.sid, tempname, c.sid, out->path.leaf, RENAME_NOREPLACE ))
        { s = errno == EEXIST || errno == ENOTEMPTY ? WEX_PROFILE_IDENTITY : WEX_PROFILE_IO; goto done; }
        published = 1;
        checkpoint( 5 );
        if (fsync( c.sid ) || fsync( c.base ) || fsync( c.users ) || fsync( c.drive ) || fsync( prefix ))
        { s = WEX_PROFILE_IO; goto done; }
    }
    s = validate_profile( &c, out, 1 );
    if (!s) s = recheck( &c );
    if (!s) out->reused = exists;
done:
    if (held_sid >= 0) close( held_sid );
    if (temp >= 0) close( temp );
    if (tempname[0] && !published)
    {
        enum wex_profile_status cleanup = discard_temp( c.sid, tempname, &c );
        if (cleanup) s = cleanup; /* do not hide unsafe/unremoved unpublished state */
    }
    if (c.sid >= 0) close( c.sid );
    if (c.lock >= 0) close( c.lock ); /* release storage-provision lock, NOT an S14 lease */
    if (c.base >= 0) close( c.base );
    if (c.users >= 0) close( c.users );
    if (c.drive >= 0) close( c.drive );
    if (c.dos >= 0) close( c.dos );
    if (s)
    {
        if (out->profile_fd >= 0) close( out->profile_fd );
        if (out->hive_fd >= 0) close( out->hive_fd );
        memset( out, 0, sizeof(*out) );
        out->profile_fd = out->hive_fd = out->sid_lock_fd = -1;
    }
    return s;
#endif
}
