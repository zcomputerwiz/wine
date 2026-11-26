/*
 * Windows user profile private API (profapi.dll)
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

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winreg.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(profapi);

/***********************************************************************
 *      profapi.114 (PROFAPI.114)
 *
 * Undocumented. The Windows App Runtime (Microsoft.Internal.FrameworkUdk.dll) calls it right after
 * PackageFamilyNameFromFullName with the family name of a package, a zero, the name of a key ("ResourcesConfig", whose
 * value "UseSystemMetadataPath" it reads next), an access mask (KEY_READ) and, as the logged call showed, a fifth argument:
 * it opens a key of the registry settings of a package and returns a Win32 error code. The arguments were found by logging
 * what the caller passes (the signature is a deduction, not a documentation). No package settings exist here, so the key is
 * never found: the caller takes its default.
 */
LONG WINAPI profapi_114( const WCHAR *family, void *reserved, const WCHAR *subkey, DWORD access, HKEY *key )
{
    FIXME( "undocumented profapi ordinal 114: family %s, reserved %p, subkey %s, access %#lx, key %p: not found.\n",
           debugstr_w( family ), reserved, debugstr_w( subkey ), access, key );
    if (key) *key = NULL;
    return ERROR_FILE_NOT_FOUND;
}
