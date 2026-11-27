/*
 * Experimental features, off by default
 *
 * Copyright 2026 WineExplorer contributors
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

#ifndef __WINE_WINE_EXPERIMENTAL_H
#define __WINE_WINE_EXPERIMENTAL_H

#include <windef.h>
#include <winbase.h>

/* WINE_EXPERIMENTAL is a list of feature names separated by commas or spaces, compared case-insensitively.
 * "all" turns on every feature, "none" (or an unset variable) none. The variable is read on every call, so a
 * process may change it before the feature is first used. */
static inline BOOL __wine_experimental_enabled( const WCHAR *name )
{
    WCHAR buffer[256];
    const WCHAR *p = buffer, *start;
    DWORD len;

    len = GetEnvironmentVariableW( L"WINE_EXPERIMENTAL", buffer, ARRAY_SIZE(buffer) );
    if (!len || len >= ARRAY_SIZE(buffer)) return FALSE;

    while (*p)
    {
        while (*p == ',' || *p == ' ') p++;
        start = p;
        while (*p && *p != ',' && *p != ' ') p++;
        if (p == start) continue;
        if (CompareStringOrdinal( start, p - start, L"none", -1, TRUE ) == CSTR_EQUAL) return FALSE;
        if (CompareStringOrdinal( start, p - start, L"all", -1, TRUE ) == CSTR_EQUAL) return TRUE;
        if (CompareStringOrdinal( start, p - start, name, -1, TRUE ) == CSTR_EQUAL) return TRUE;
    }
    return FALSE;
}

#endif /* __WINE_WINE_EXPERIMENTAL_H */
