/*
 * Inert UWP package-context storage building block
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

/* Maximum size, in bytes, of a single counted UTF-16 field. */
#define UWP_CONTEXT_MAX_FIELD_BYTES 8192
/* Maximum total size, in bytes, of all fields in one context. */
#define UWP_CONTEXT_MAX_TOTAL_BYTES 32768

/*
 * An immutable container for the four counted UTF-16 strings that identify a
 * UWP package context: the package full name, the package family name, the
 * application ID and the installed package root.
 *
 * This is a pure memory/lifetime building block. It performs no token or
 * process association, reads no environment or PEB, does no filesystem access,
 * launches nothing and makes no package-activation or authenticity claim. The
 * stored strings preserve case and exact UTF-16 code units; no normalization
 * is applied. It is not wired to any server entry point.
 */
struct uwp_context;

/*
 * Create an immutable package context from four counted UTF-16 strings.
 *
 * Each field must be non-NULL, nonempty, an even number of bytes (a whole
 * number of WCHARs), at most UWP_CONTEXT_MAX_FIELD_BYTES, and free of embedded
 * NUL code units. The sum of the four field sizes must not exceed
 * UWP_CONTEXT_MAX_TOTAL_BYTES; the running total is checked so that the
 * addition cannot overflow.
 *
 * The input is deep-copied into a single immutable allocation; the caller
 * must supply valid, aligned buffers containing the declared byte lengths,
 * keeps ownership of its own buffers and may free them immediately. Returns a
 * context with a reference count of one, or NULL on invalid input or out of
 * memory (fail closed).
 */
struct uwp_context *uwp_context_create( const WCHAR *package_full_name, data_size_t package_full_name_len,
                                        const WCHAR *family_name, data_size_t family_name_len,
                                        const WCHAR *app_id, data_size_t app_id_len,
                                        const WCHAR *installed_root, data_size_t installed_root_len );

/* Increment the reference count and return the context; fail on overflow. */
struct uwp_context *uwp_context_retain( struct uwp_context *ctx );

/* Decrement the reference count; frees the context when it reaches zero. */
void uwp_context_release( struct uwp_context *ctx );

/*
 * Read-only accessors. Each returns a counted string whose buffer lives in the
 * context's immutable allocation and is valid until the context is released.
 * Return NULL if ctx is NULL.
 */
const struct unicode_str *uwp_context_get_package_full_name( const struct uwp_context *ctx );
const struct unicode_str *uwp_context_get_family_name( const struct uwp_context *ctx );
const struct unicode_str *uwp_context_get_app_id( const struct uwp_context *ctx );
const struct unicode_str *uwp_context_get_installed_root( const struct uwp_context *ctx );

#endif  /* __WINE_SERVER_UWP_CONTEXT_H */
