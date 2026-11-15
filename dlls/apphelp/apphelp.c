/*
 * Copyright 2011 André Hentschel
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
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winreg.h"
#include <appcompatapi.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(apphelp);

/* FIXME: don't know where to place that enum */
typedef enum _PATH_TYPE {
    DOS_PATH,
    NT_PATH
} PATH_TYPE;

/* FIXME: don't know where to place that typedef */
typedef HANDLE PDB;
typedef HANDLE HSDB;
typedef DWORD TAGID;

/* FIXME: don't know where to place that define */
#define TAGID_NULL 0x0

/* SdbGetPermLayerKeys flags */
#define GPLK_USER    1
#define GPLK_MACHINE 2

BOOL WINAPI ApphelpCheckInstallShieldPackage( void* ptr, LPCWSTR path )
{
    FIXME("stub: %p %s\n", ptr, debugstr_w(path));
    return TRUE;
}

BOOL WINAPI ApphelpCheckMsiPackage( void* ptr, LPCWSTR path )
{
    FIXME("stub: %p %s\n", ptr, debugstr_w(path));
    return TRUE;
}

PDB WINAPI SdbCreateDatabase( LPCWSTR path, PATH_TYPE type )
{
    FIXME("stub: %s %u\n", debugstr_w(path), type);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

BOOL WINAPI ApphelpCheckShellObject( REFCLSID clsid, BOOL shim, ULONGLONG *flags )
{
    TRACE("(%s, %d, %p)\n", debugstr_guid(clsid), shim, flags);
    if (flags) *flags = 0;
    return TRUE;
}

BOOL WINAPI ShimFlushCache( HWND hwnd, HINSTANCE instance, LPCSTR cmdline, int cmd )
{
    FIXME("stub: %p %p %s %u\n", hwnd, instance, debugstr_a(cmdline), cmd);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return TRUE;
}

HSDB WINAPI SdbInitDatabase(DWORD flags, LPCWSTR path)
{
    FIXME("stub: %08lx %s\n", flags, debugstr_w(path));
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

PDB WINAPI SdbOpenDatabase(LPCWSTR path, PATH_TYPE type)
{
    FIXME("stub: %s %08x\n", debugstr_w(path), type);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

TAGID WINAPI SdbGetFirstChild(PDB pdb, TAGID parent)
{
    FIXME("stub: %p %ld\n", pdb, parent);
    return TAGID_NULL;
}

void WINAPI SdbCloseDatabase(PDB pdb)
{
    FIXME("stub: %p\n", pdb);
}

void WINAPI SdbGetAppPatchDir(HSDB hsdb, WCHAR *path, DWORD size)
{
    FIXME("stub: %p %p %ld\n", hsdb, path, size);
    if (size && path) *path = 0;
}

BOOL WINAPI SetPermLayerState( PCWSTR path, PCWSTR layers, DWORD flags, BOOL machine, BOOL state)
{
    FIXME("stub: %s %s %ld %d %d\n", debugstr_w(path), debugstr_w(layers), flags, machine, state);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return TRUE;
}

BOOL WINAPI SdbSetPermLayerKeys( PCWSTR path, PCWSTR layers, BOOL machine )
{
    static const WCHAR layers_key[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    HKEY base = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    HKEY key;
    LSTATUS status;

    if (!path)
        return FALSE;

    status = RegCreateKeyExW( base, layers_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL );
    if (status != ERROR_SUCCESS)
        return FALSE;

    if (!layers || !*layers)
    {
        status = RegDeleteValueW( key, path );
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }
    else
    {
        status = RegSetValueExW( key, path, 0, REG_SZ, (const BYTE *)layers,
                                 (DWORD)((lstrlenW( layers ) + 1) * sizeof(WCHAR)) );
    }

    RegCloseKey( key );
    return status == ERROR_SUCCESS;
}

/* Read the REG_SZ value named `path` from the Layers key under `base` into
 * `buffer`. Returns the data length in bytes (excluding the null terminator),
 * or 0 if the value does not exist or does not fit in the buffer. */
static DWORD read_layer_value( HKEY base, const WCHAR *subkey, PCWSTR path, PWSTR buffer, DWORD buffer_size )
{
    DWORD required, stored;
    LSTATUS status;

    status = RegGetValueW( base, subkey, path, RRF_RT_REG_SZ, NULL, NULL, &required );
    if (status != ERROR_SUCCESS) return 0;
    if (required < sizeof(WCHAR) || required > buffer_size) return 0;
    stored = buffer_size;
    status = RegGetValueW( base, subkey, path, RRF_RT_REG_SZ, NULL, buffer, &stored );
    if (status != ERROR_SUCCESS) return 0;
    return stored - sizeof(WCHAR);
}

BOOL WINAPI SdbGetPermLayerKeys( PCWSTR path, PWSTR layers, PDWORD size, DWORD flags )
{
    static const WCHAR layers_key[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    WCHAR machine_buf[512], user_buf[512];
    PWSTR out;
    DWORD machine_len, user_len, total;

    if (!path || !layers || !size)
        return FALSE;
    if (!(flags & (GPLK_USER | GPLK_MACHINE)))
        return FALSE;

    machine_len = (flags & GPLK_MACHINE)
        ? read_layer_value( HKEY_LOCAL_MACHINE, layers_key, path, machine_buf, sizeof(machine_buf) )
        : 0;
    user_len = (flags & GPLK_USER)
        ? read_layer_value( HKEY_CURRENT_USER, layers_key, path, user_buf, sizeof(user_buf) )
        : 0;

    if (!machine_len && !user_len)
        return FALSE;

    total = machine_len + user_len + (DWORD)sizeof(WCHAR);
    if (machine_len && user_len)
        total += (DWORD)sizeof(WCHAR);

    if (total > *size)
    {
        *size = total;
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        return FALSE;
    }

    out = layers;
    if (machine_len)
    {
        memcpy( out, machine_buf, machine_len );
        out += machine_len / sizeof(WCHAR);
    }
    if (machine_len && user_len)
        *out++ = L' ';
    if (user_len)
    {
        memcpy( out, user_buf, user_len );
        out += user_len / sizeof(WCHAR);
    }
    *out = 0;
    *size = total;
    return TRUE;
}
