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

#ifndef __WINE_SERVER_UWP_CONTEXT_H
#define __WINE_SERVER_UWP_CONTEXT_H

#include "object.h"

struct fd;

/* the largest catalog record, in bytes */
#define UWP_RECORD_MAX_BYTES 65536

/*
 * An immutable package context. The server builds it from a record of the
 * package catalog of its own prefix, never from strings a client supplies: the
 * client names an application (family name and application id) and everything
 * else comes from the record. The context remembers the device and inode of
 * the application's executable, so that the first process started with it can
 * be checked against the file it actually maps.
 *
 * The catalog is the directory "appx-catalog" of the prefix. A package
 * PackageFullName is published by the record file "PackageFullName.wexappx"
 * in it. A record is UTF-16LE text limited to printable ASCII, with lines
 * ending in a line feed and fields separated by a tab:
 *
 *   WEXAPPX 1
 *   root <tab> package directory: a path on drive C: ending in PackageFullName
 *   app <tab> ApplicationId <tab> relative path of the executable [<tab> entry point]
 *   class <tab> ActivatableClassId <tab> relative path of the in-process server
 *
 * Paths use backslashes; the relative ones stay inside the package directory.
 * The package directory is in the user's profile for a per-user package
 * (Program Files belongs to the machine and is not writable by a standard
 * user).
 */
struct uwp_context;

/* the sizes, in bytes, of the strings of a context, in the order they are stored */
struct uwp_context_sizes
{
    data_size_t full_name;    /* package full name */
    data_size_t family_name;  /* package family name */
    data_size_t app_id;       /* application id */
    data_size_t root;         /* DOS path of the package directory */
    data_size_t executable;   /* executable, relative to the package directory */
    data_size_t entry_point;  /* the EntryPoint of the application in the manifest, may be empty */
    data_size_t classes;      /* class table: class, NUL, relative path, NUL, ... */
};

/* build the context of an application ("FamilyName!ApplicationId") from the catalog; NULL with the error set */
extern struct uwp_context *uwp_context_from_aumid( const WCHAR *aumid, data_size_t len );
extern struct uwp_context *uwp_context_retain( struct uwp_context *ctx );
extern void uwp_context_release( struct uwp_context *ctx );
/* is this file the executable of the context's application? */
extern int uwp_context_is_executable( const struct uwp_context *ctx, struct fd *fd );
/* the strings of the context, back to back; returns their total size */
extern data_size_t uwp_context_get_data( const struct uwp_context *ctx, struct uwp_context_sizes *sizes,
                                         const void **data );

#endif  /* __WINE_SERVER_UWP_CONTEXT_H */
