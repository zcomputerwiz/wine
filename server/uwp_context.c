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

#include "config.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#include "winternl.h"

#include "object.h"
#include "thread.h"
#include "uwp_context.h"

struct uwp_context
{
    unsigned int refcount;
    WCHAR *data;  /* points to the trailing strings in this allocation */
    struct unicode_str package_full_name;
    struct unicode_str family_name;
    struct unicode_str app_id;
    struct unicode_str installed_root;
};

/*
 * Validate one counted field. Returns 0 on success, -1 on invalid input.
 * The field must be non-NULL, nonempty, a whole number of WCHARs, within the
 * per-field limit, and free of embedded NUL code units.
 */
static int validate_field( const WCHAR *str, data_size_t len )
{
    data_size_t i, count;

    if (!str || !len)
        return -1;
    if (len & 1)  /* odd byte length is not a whole number of WCHARs */
        return -1;
    if (len > UWP_CONTEXT_MAX_FIELD_BYTES)
        return -1;

    count = len / sizeof(WCHAR);
    for (i = 0; i < count; i++)
        if (!str[i])  /* embedded NUL code unit */
            return -1;

    return 0;
}

/* Copy one counted field into dst at byte offset *offset; return its start. */
static const WCHAR *copy_field( WCHAR *dst, data_size_t *offset, const WCHAR *src, data_size_t len )
{
    WCHAR *p = dst + *offset / sizeof(WCHAR);

    memcpy( p, src, len );
    *offset += len;
    return p;
}

struct uwp_context *uwp_context_create( const WCHAR *package_full_name, data_size_t package_full_name_len,
                                        const WCHAR *family_name, data_size_t family_name_len,
                                        const WCHAR *app_id, data_size_t app_id_len,
                                        const WCHAR *installed_root, data_size_t installed_root_len )
{
    struct uwp_context *ctx;
    const WCHAR *fields[4];
    data_size_t lens[4], total = 0, offset, i;

    fields[0] = package_full_name; lens[0] = package_full_name_len;
    fields[1] = family_name;       lens[1] = family_name_len;
    fields[2] = app_id;            lens[2] = app_id_len;
    fields[3] = installed_root;    lens[3] = installed_root_len;

    for (i = 0; i < 4; i++)
    {
        if (validate_field( fields[i], lens[i] ))
        {
            set_error( STATUS_INVALID_PARAMETER );
            return NULL;
        }
        /* Bound the running total; also guards against integer overflow. */
        if (lens[i] > UWP_CONTEXT_MAX_TOTAL_BYTES - total)
        {
            set_error( STATUS_INVALID_PARAMETER );
            return NULL;
        }
        total += lens[i];
    }

    ctx = mem_alloc( sizeof(*ctx) + total );
    if (!ctx)
        return NULL;  /* mem_alloc set STATUS_NO_MEMORY */
    ctx->data = (WCHAR *)(ctx + 1);

    offset = 0;
    ctx->package_full_name.str = copy_field( ctx->data, &offset, package_full_name, package_full_name_len );
    ctx->package_full_name.len = package_full_name_len;
    ctx->family_name.str = copy_field( ctx->data, &offset, family_name, family_name_len );
    ctx->family_name.len = family_name_len;
    ctx->app_id.str = copy_field( ctx->data, &offset, app_id, app_id_len );
    ctx->app_id.len = app_id_len;
    ctx->installed_root.str = copy_field( ctx->data, &offset, installed_root, installed_root_len );
    ctx->installed_root.len = installed_root_len;

    ctx->refcount = 1;
    return ctx;
}

struct uwp_context *uwp_context_retain( struct uwp_context *ctx )
{
    if (!ctx)
        return NULL;
    if (ctx->refcount >= INT_MAX)
    {
        set_error( STATUS_INTEGER_OVERFLOW );
        return NULL;
    }
    ctx->refcount++;
    return ctx;
}

void uwp_context_release( struct uwp_context *ctx )
{
    if (!ctx)
        return;
    assert( ctx->refcount );
    if (!--ctx->refcount)
    {
        free( ctx );
    }
}

const struct unicode_str *uwp_context_get_package_full_name( const struct uwp_context *ctx )
{
    return ctx ? &ctx->package_full_name : NULL;
}

const struct unicode_str *uwp_context_get_family_name( const struct uwp_context *ctx )
{
    return ctx ? &ctx->family_name : NULL;
}

const struct unicode_str *uwp_context_get_app_id( const struct uwp_context *ctx )
{
    return ctx ? &ctx->app_id : NULL;
}

const struct unicode_str *uwp_context_get_installed_root( const struct uwp_context *ctx )
{
    return ctx ? &ctx->installed_root : NULL;
}
