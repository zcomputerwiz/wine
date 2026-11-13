/*
 * Package context of a token (WineExplorer UWP compatibility)
 *
 * Copyright (C) 2026 WineExplorer contributors
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

#include "config.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "object.h"
#include "file.h"
#include "request.h"
#include "thread.h"
#include "uwp_context.h"

#define CATALOG_DIR      "appx-catalog"     /* the records, in the prefix directory */
#define DRIVE_C_DIR      "dosdevices/c:"    /* the packages, anywhere on drive C: */
#define RECORD_SUFFIX    ".wexappx"
#define FULL_NAME_MAX    127   /* PACKAGE_FULL_NAME_MAX_LENGTH */
#define APP_ID_MAX       64
#define REL_PATH_MAX     512
#define ENTRY_POINT_MAX  256

static const char record_magic[] = "WEXAPPX 1";

struct uwp_context
{
    unsigned int             refcount;
    dev_t                    exe_dev;   /* the executable of the application */
    ino_t                    exe_ino;
    struct uwp_context_sizes sizes;
    data_size_t              total;     /* size of the strings that follow */
    /* WCHAR data[] */
};

struct field
{
    const WCHAR *str;
    size_t       len;   /* in characters */
};

static int is_name_char( WCHAR ch )
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           ch == '.' || ch == '-';
}

static WCHAR to_lower( WCHAR ch )
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 'a' - 'A' : ch;
}

/* compare a counted string with an ASCII string, ignoring ASCII case */
static int equal_ascii( const WCHAR *str, size_t len, const char *ascii, size_t ascii_len )
{
    size_t i;

    if (len != ascii_len) return 0;
    for (i = 0; i < len; i++) if (to_lower( str[i] ) != to_lower( (unsigned char)ascii[i] )) return 0;
    return 1;
}

static int equal_field( const struct field *a, const struct field *b )
{
    size_t i;

    if (a->len != b->len) return 0;
    for (i = 0; i < a->len; i++) if (to_lower( a->str[i] ) != to_lower( b->str[i] )) return 0;
    return 1;
}

/* is this field exactly the given keyword? */
static int is_keyword( const struct field *field, const char *keyword )
{
    size_t i, len = strlen( keyword );

    if (field->len != len) return 0;
    for (i = 0; i < len; i++) if (field->str[i] != (unsigned char)keyword[i]) return 0;
    return 1;
}

/* a package full name is Name_Version_Architecture_ResourceId_PublisherId; return the length of the name
 * and the offset of the publisher id, or 0 when it is not one */
static int split_full_name( const char *name, size_t len, size_t *name_len, size_t *publisher )
{
    size_t i, count = 0, first = 0, last = 0;

    if (!len || len > FULL_NAME_MAX) return 0;
    for (i = 0; i < len; i++)
    {
        if (name[i] == '_')
        {
            if (!count++) first = i;
            last = i;
        }
        else if (!is_name_char( (unsigned char)name[i] )) return 0;
    }
    if (count != 4 || !first || last + 1 == len || name[0] == '.') return 0;
    *name_len = first;
    *publisher = last + 1;
    return 1;
}

/* a relative path inside the package: components separated by single backslashes, none of them "." or "..",
 * no drive, stream or device syntax */
static int valid_relative_path( const struct field *path )
{
    size_t i, start = 0;

    if (!path->len || path->len > REL_PATH_MAX) return 0;
    for (i = 0; i <= path->len; i++)
    {
        WCHAR ch = i < path->len ? path->str[i] : '\\';

        if (ch == '\\')
        {
            size_t n = i - start;

            if (!n) return 0;
            if (path->str[start] == '.' && (n == 1 || (n == 2 && path->str[start + 1] == '.'))) return 0;
            if (path->str[i - 1] == '.' || path->str[i - 1] == ' ') return 0;
            start = i + 1;
        }
        else if (ch < 0x20 || ch > 0x7e || ch == '/' || ch == ':' || ch == '"' || ch == '<' || ch == '>' ||
                 ch == '|' || ch == '?' || ch == '*')
            return 0;
    }
    return 1;
}

static int valid_class_id( const struct field *id )
{
    size_t i;

    if (!id->len || id->len > 255) return 0;
    for (i = 0; i < id->len; i++) if (!is_name_char( id->str[i] ) && id->str[i] != '_') return 0;
    return 1;
}

static int valid_app_id( const struct field *id )
{
    size_t i;

    if (!id->len || id->len > APP_ID_MAX) return 0;
    for (i = 0; i < id->len; i++) if (!is_name_char( id->str[i] ) || id->str[i] == '-') return 0;
    return 1;
}

/* split the next line of a record into its tab separated fields; returns the number of fields, 0 at the
 * end and -1 on a malformed line */
static int next_line( const WCHAR **pos, const WCHAR *end, struct field fields[4] )
{
    const WCHAR *p = *pos, *start = p;
    int count = 0;

    if (p == end) return 0;
    for (;; p++)
    {
        if (p == end) return -1;  /* the last line has no line feed */
        if (*p == '\t' || *p == '\n')
        {
            if (count == 4) return -1;
            fields[count].str = start;
            fields[count].len = p - start;
            count++;
            start = p + 1;
            if (*p == '\n') break;
        }
        else if (*p < 0x20 || *p > 0x7e) return -1;
    }
    *pos = p + 1;
    return count;
}

/* open a regular file below a directory of the prefix; a symbolic link in the last component is refused */
static int open_prefix_file( const char *dir, const char *name, struct stat *st )
{
    char path[sizeof(DRIVE_C_DIR) + FULL_NAME_MAX + 2 * REL_PATH_MAX + 16];
    int fd;

    if ((unsigned int)snprintf( path, sizeof(path), "%s/%s", dir, name ) >= sizeof(path)) return -1;
    if ((fd = openat( config_dir_fd, path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC )) == -1) return -1;
    if (fstat( fd, st ) || !S_ISREG( st->st_mode ))
    {
        close( fd );
        return -1;
    }
    return fd;
}

/* read the record of a package; returns its text and length in characters */
static WCHAR *read_record( const char *full_name, size_t *ret_len )
{
    char name[FULL_NAME_MAX + sizeof(RECORD_SUFFIX)];
    struct stat st;
    WCHAR *text;
    size_t size, done = 0;
    int fd;

    snprintf( name, sizeof(name), "%s%s", full_name, RECORD_SUFFIX );
    if ((fd = open_prefix_file( CATALOG_DIR, name, &st )) == -1)
    {
        set_error( STATUS_NOT_FOUND );
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > UWP_RECORD_MAX_BYTES || (st.st_size & 1))
    {
        close( fd );
        set_error( STATUS_FILE_CORRUPT_ERROR );
        return NULL;
    }
    size = st.st_size;
    if (!(text = mem_alloc( size )))
    {
        close( fd );
        return NULL;
    }
    while (done < size)
    {
        ssize_t n = read( fd, (char *)text + done, size - done );
        if (n <= 0) break;
        done += n;
    }
    close( fd );
    if (done != size)
    {
        free( text );
        set_error( STATUS_FILE_CORRUPT_ERROR );
        return NULL;
    }
    *ret_len = size / sizeof(WCHAR);
    return text;
}

/* find the one published package of a family; returns the length of its full name, 0 with the error set */
static size_t find_package( const struct field *family, char full_name[FULL_NAME_MAX + 1] )
{
    const size_t suffix_len = sizeof(RECORD_SUFFIX) - 1;
    struct dirent *de;
    size_t found = 0;
    DIR *dir = NULL;
    int fd;

    if ((fd = openat( config_dir_fd, CATALOG_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC )) == -1 ||
        !(dir = fdopendir( fd )))
    {
        if (fd != -1) close( fd );
        set_error( STATUS_NOT_FOUND );
        return 0;
    }
    while ((de = readdir( dir )))
    {
        size_t len = strlen( de->d_name ), name_len, publisher, i;

        if (len <= suffix_len || strcmp( de->d_name + len - suffix_len, RECORD_SUFFIX )) continue;
        len -= suffix_len;
        if (!split_full_name( de->d_name, len, &name_len, &publisher )) continue;
        /* the family name is Name_PublisherId */
        if (family->len != name_len + 1 + len - publisher) continue;
        for (i = 0; i < name_len; i++)
            if (to_lower( family->str[i] ) != to_lower( (unsigned char)de->d_name[i] )) break;
        if (i < name_len || family->str[name_len] != '_') continue;
        if (!equal_ascii( family->str + name_len + 1, len - publisher, de->d_name + publisher, len - publisher ))
            continue;
        if (found)  /* two packages of the family: the main package is not known */
        {
            closedir( dir );
            set_error( STATUS_OBJECT_NAME_COLLISION );
            return 0;
        }
        memcpy( full_name, de->d_name, len );
        full_name[len] = 0;
        found = len;
    }
    closedir( dir );
    if (!found) set_error( STATUS_NOT_FOUND );
    return found;
}

static void append_ascii( WCHAR **pos, const char *str, size_t len )
{
    size_t i;

    for (i = 0; i < len; i++) *(*pos)++ = (unsigned char)str[i];
}

static void append_field( WCHAR **pos, const struct field *field )
{
    memcpy( *pos, field->str, field->len * sizeof(WCHAR) );
    *pos += field->len;
}

struct uwp_context *uwp_context_from_aumid( const WCHAR *aumid, data_size_t len )
{
    char full_name[FULL_NAME_MAX + 1], exe_path[2 * REL_PATH_MAX + 2];
    struct field family, app_id, fields[4], app = { NULL, 0 }, exe = { NULL, 0 }, entry = { NULL, 0 }, root, root_rel;
    const WCHAR *pos, *end;
    struct uwp_context *ctx = NULL;
    size_t i, count = len / sizeof(WCHAR), full_len, name_len, publisher, text_len, classes = 0;
    WCHAR *text, *out;
    struct stat st;
    int fd, ret;

    /* FamilyName!ApplicationId */
    if (!count || (len & 1) || count > FULL_NAME_MAX + 1 + APP_ID_MAX) goto invalid;
    for (i = 0; i < count; i++) if (aumid[i] == '!') break;
    if (i == count) goto invalid;
    family.str = aumid;
    family.len = i;
    app_id.str = aumid + i + 1;
    app_id.len = count - i - 1;
    if (!family.len || !valid_app_id( &app_id )) goto invalid;
    for (i = 0; i < family.len; i++) if (!is_name_char( family.str[i] ) && family.str[i] != '_') goto invalid;

    if (!(full_len = find_package( &family, full_name ))) return NULL;
    if (!split_full_name( full_name, full_len, &name_len, &publisher )) goto invalid;
    if (!(text = read_record( full_name, &text_len ))) return NULL;

    /* A record that cannot be read completely is an error: it never counts as "no package". */
    pos = text;
    end = text + text_len;
    if (next_line( &pos, end, fields ) != 1 ||
        !equal_ascii( fields[0].str, fields[0].len, record_magic, sizeof(record_magic) - 1 ))
        goto corrupt;

    /* the package directory: a path on drive C: whose last component is the package full name */
    if (next_line( &pos, end, fields ) != 2 || !is_keyword( &fields[0], "root" )) goto corrupt;
    root = fields[1];
    if (root.len < 3 + full_len || to_lower( root.str[0] ) != 'c' || root.str[1] != ':' || root.str[2] != '\\')
        goto corrupt;
    root_rel.str = root.str + 3;
    root_rel.len = root.len - 3;
    if (!valid_relative_path( &root_rel ) ||
        (root_rel.len > full_len && root_rel.str[root_rel.len - full_len - 1] != '\\') ||
        !equal_ascii( root_rel.str + root_rel.len - full_len, full_len, full_name, full_len ))
        goto corrupt;

    while ((ret = next_line( &pos, end, fields )))
    {
        if (ret < 3 || !valid_relative_path( &fields[2] )) goto corrupt;
        if (is_keyword( &fields[0], "app" ))
        {
            if (!valid_app_id( &fields[1] )) goto corrupt;
            if (ret == 4 && (!fields[3].len || fields[3].len > ENTRY_POINT_MAX)) goto corrupt;
            if (!equal_field( &fields[1], &app_id )) continue;
            if (app.str) goto corrupt;  /* declared twice */
            app = fields[1];
            exe = fields[2];
            if (ret == 4) entry = fields[3];
        }
        else if (is_keyword( &fields[0], "class" ))
        {
            if (ret != 3 || !valid_class_id( &fields[1] )) goto corrupt;
            classes += (fields[1].len + fields[2].len + 2) * sizeof(WCHAR);
        }
        else goto corrupt;
    }
    if (!app.str)
    {
        free( text );
        set_error( STATUS_NOT_FOUND );
        return NULL;
    }

    /* the executable the application has to be started from */
    for (i = 0; i < root_rel.len; i++) exe_path[i] = root_rel.str[i] == '\\' ? '/' : (char)root_rel.str[i];
    exe_path[i] = '/';
    ret = i + 1;
    for (i = 0; i < exe.len; i++) exe_path[ret + i] = exe.str[i] == '\\' ? '/' : (char)exe.str[i];
    exe_path[ret + i] = 0;
    if ((fd = open_prefix_file( DRIVE_C_DIR, exe_path, &st )) == -1)
    {
        free( text );
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        return NULL;
    }
    close( fd );

    /* full name, family name (shorter than the full name), application id, root, executable, entry point, classes */
    if (!(ctx = mem_alloc( sizeof(*ctx) + (full_len * 2 + app.len + root.len + exe.len + entry.len) * sizeof(WCHAR) +
                           classes )))
    {
        free( text );
        return NULL;
    }
    ctx->refcount = 1;
    ctx->exe_dev = st.st_dev;
    ctx->exe_ino = st.st_ino;

    out = (WCHAR *)(ctx + 1);
    append_ascii( &out, full_name, full_len );
    ctx->sizes.full_name = full_len * sizeof(WCHAR);
    append_ascii( &out, full_name, name_len );
    append_ascii( &out, "_", 1 );
    append_ascii( &out, full_name + publisher, full_len - publisher );
    ctx->sizes.family_name = (name_len + 1 + full_len - publisher) * sizeof(WCHAR);
    append_field( &out, &app );
    ctx->sizes.app_id = app.len * sizeof(WCHAR);
    append_field( &out, &root );
    ctx->sizes.root = root.len * sizeof(WCHAR);
    append_field( &out, &exe );
    ctx->sizes.executable = exe.len * sizeof(WCHAR);
    if (entry.str) append_field( &out, &entry );
    ctx->sizes.entry_point = entry.len * sizeof(WCHAR);

    pos = text;
    while (next_line( &pos, end, fields ) > 0)
    {
        if (!is_keyword( &fields[0], "class" )) continue;
        append_field( &out, &fields[1] );
        *out++ = 0;
        append_field( &out, &fields[2] );
        *out++ = 0;
    }
    ctx->sizes.classes = classes;
    ctx->total = (char *)out - (char *)(ctx + 1);
    assert( ctx->total == ctx->sizes.full_name + ctx->sizes.family_name + ctx->sizes.app_id + ctx->sizes.root +
            ctx->sizes.executable + ctx->sizes.entry_point + ctx->sizes.classes );
    free( text );
    return ctx;

corrupt:
    free( text );
    set_error( STATUS_FILE_CORRUPT_ERROR );
    return NULL;

invalid:
    set_error( STATUS_INVALID_PARAMETER );
    return NULL;
}

struct uwp_context *uwp_context_retain( struct uwp_context *ctx )
{
    assert( ctx->refcount < INT_MAX );
    ctx->refcount++;
    return ctx;
}

void uwp_context_release( struct uwp_context *ctx )
{
    assert( ctx->refcount );
    if (!--ctx->refcount) free( ctx );
}

int uwp_context_is_executable( const struct uwp_context *ctx, struct fd *fd )
{
    struct stat st;
    int unix_fd;

    if (!fd || (unix_fd = get_unix_fd( fd )) == -1)
    {
        clear_error();
        return 0;
    }
    return !fstat( unix_fd, &st ) && S_ISREG( st.st_mode ) && st.st_dev == ctx->exe_dev && st.st_ino == ctx->exe_ino;
}

data_size_t uwp_context_get_data( const struct uwp_context *ctx, struct uwp_context_sizes *sizes, const void **data )
{
    *sizes = ctx->sizes;
    *data = ctx + 1;
    return ctx->total;
}
