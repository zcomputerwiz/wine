/*
 *
 * Copyright 2008 Alistair Leslie-Hughes
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "slpublic.h"
#include "slerror.h"

WINE_DEFAULT_DEBUG_CHANNEL(slc);

HRESULT WINAPI SLGetLicensingStatusInformation(HSLC handle, const SLID *app, const SLID *product,
                                               LPCWSTR name, UINT *count, SL_LICENSING_STATUS **status)
{
    FIXME("(%p %p %p %s %p %p) stub\n", handle, app, product, debugstr_w(name), count, status );

    return SL_E_RIGHT_NOT_CONSUMED;
}

HRESULT WINAPI SLOpen(HSLC *handle)
{
    FIXME("(%p) stub\n", handle );

    if (!handle)
        return E_INVALIDARG;

    *handle = (HSLC)0xdeadbeef;

    return S_OK;
}

HRESULT WINAPI SLClose(HSLC handle)
{
    FIXME("(%p) stub\n", handle );

    return S_OK;
}

/* Nothing fires licensing events on Wine, so a registration is only remembered: Windows refuses a second
 * unregistration of the same (event, application, handle). */
struct event_registration
{
    struct list entry;
    const WCHAR *event_id;
    SLID app;
    HANDLE event;
};

static struct list event_registrations = LIST_INIT( event_registrations );
static CRITICAL_SECTION event_cs;
static CRITICAL_SECTION_DEBUG event_cs_debug =
{
    0, 0, &event_cs,
    { &event_cs_debug.ProcessLocksList, &event_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": event_cs") }
};
static CRITICAL_SECTION event_cs = { &event_cs_debug, -1, 0, 0, 0, 0 };

static HRESULT check_event_args( PCWSTR event_id, const SLID *app, HANDLE event, const WCHAR **known_id )
{
    static const WCHAR *const known_ids[] =
    {
        SL_EVENT_LICENSING_STATE_CHANGED,
        SL_EVENT_POLICY_CHANGED,
        SL_EVENT_USER_NOTIFICATION,
    };
    unsigned int i;

    if (!event_id || !app || !event) return E_INVALIDARG;
    for (i = 0; i < ARRAY_SIZE(known_ids); i++)
    {
        if (wcscmp( event_id, known_ids[i] )) continue;
        *known_id = known_ids[i];
        return S_OK;
    }
    return SL_E_INVALID_EVENT_ID;
}

static struct event_registration *find_registration( const WCHAR *event_id, const SLID *app, HANDLE event )
{
    struct event_registration *reg;

    LIST_FOR_EACH_ENTRY( reg, &event_registrations, struct event_registration, entry )
        if (reg->event_id == event_id && IsEqualGUID( &reg->app, app ) && reg->event == event) return reg;
    return NULL;
}

HRESULT WINAPI SLRegisterEvent(HSLC handle, PCWSTR event_id, const SLID *app, HANDLE event)
{
    struct event_registration *reg;
    const WCHAR *id;
    DWORD flags;
    HRESULT hr;

    TRACE("(%p %s %s %p)\n", handle, debugstr_w(event_id), debugstr_guid(app), event);

    if (FAILED(hr = check_event_args( event_id, app, event, &id ))) return hr;
    if (!GetHandleInformation( event, &flags )) return HRESULT_FROM_WIN32( GetLastError() );

    EnterCriticalSection( &event_cs );
    if (!find_registration( id, app, event ))
    {
        if (!(reg = malloc( sizeof(*reg) ))) hr = E_OUTOFMEMORY;
        else
        {
            reg->event_id = id;
            reg->app = *app;
            reg->event = event;
            list_add_tail( &event_registrations, &reg->entry );
        }
    }
    LeaveCriticalSection( &event_cs );
    return hr;
}

HRESULT WINAPI SLUnregisterEvent(HSLC handle, PCWSTR event_id, const SLID *app, HANDLE event)
{
    struct event_registration *reg;
    const WCHAR *id;
    HRESULT hr;

    TRACE("(%p %s %s %p)\n", handle, debugstr_w(event_id), debugstr_guid(app), event);

    if (FAILED(hr = check_event_args( event_id, app, event, &id ))) return hr;

    EnterCriticalSection( &event_cs );
    if ((reg = find_registration( id, app, event )))
    {
        list_remove( &reg->entry );
        free( reg );
    }
    else hr = SL_E_EVENT_NOT_REGISTERED;
    LeaveCriticalSection( &event_cs );
    return hr;
}

/* 36 characters: 8-4-4-4-12 hexadecimal digits */
static BOOL parse_guid( const char *p, SLID *id )
{
    unsigned int i, v = 0;

    memset( id, 0, sizeof(*id) );
    for (i = 0; i < 36; i++)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (p[i] != '-') return FALSE;
            continue;
        }
        if (p[i] >= '0' && p[i] <= '9') v = p[i] - '0';
        else if (p[i] >= 'a' && p[i] <= 'f') v = p[i] - 'a' + 10;
        else if (p[i] >= 'A' && p[i] <= 'F') v = p[i] - 'A' + 10;
        else return FALSE;
        if (i < 8) id->Data1 = id->Data1 << 4 | v;
        else if (i < 13) id->Data2 = id->Data2 << 4 | v;
        else if (i < 18) id->Data3 = id->Data3 << 4 | v;
        else
        {
            unsigned int k = i < 23 ? (i - 19) / 2 : 2 + (i - 24) / 2;
            id->Data4[k] = id->Data4[k] << 4 | v;
        }
    }
    return TRUE;
}

/* The license id is the licenseId="{...}" attribute near the start of the license document. */
static BOOL parse_license_id( const BYTE *blob, UINT size, SLID *id )
{
    static const char tag[] = "licenseId=\"{";
    const char *p = (const char *)blob, *end = p + min( size, 4096 );

    for (; p + sizeof(tag) + 36 <= end; p++) if (!memcmp( p, tag, sizeof(tag) - 1 )) break;
    if (p + sizeof(tag) + 36 > end) return FALSE;
    p += sizeof(tag) - 1;
    return parse_guid( p, id ) && p[36] == '}';
}

/* Licenses are kept as files of their own inside the prefix, named after the license id (WineExplorer's
 * software protection client, see docs/STOCK_CHANGES.md). */
static BOOL get_license_dir( WCHAR *dir, DWORD size )
{
    static const WCHAR subdir[] = L"\\SPPWine\\Licenses";
    WCHAR *p;
    DWORD len = GetEnvironmentVariableW( L"ProgramData", dir, size );

    if (!len || len + ARRAY_SIZE(subdir) > size)
    {
        len = GetWindowsDirectoryW( dir, size );
        if (!len || len + ARRAY_SIZE(subdir) > size) return FALSE;
    }
    wcscpy( dir + len, subdir );
    for (p = dir + 3; *p; p++)
    {
        if (*p != '\\') continue;
        *p = 0;
        CreateDirectoryW( dir, NULL );
        *p = '\\';
    }
    CreateDirectoryW( dir, NULL );
    return TRUE;
}

static BOOL get_license_path( const SLID *id, WCHAR *path )
{
    if (!get_license_dir( path, MAX_PATH - 48 )) return FALSE;
    swprintf( path + wcslen( path ), 48, L"\\{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}.xrm-ms",
              id->Data1, id->Data2, id->Data3, id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
              id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7] );
    return TRUE;
}

HRESULT WINAPI SLGetLicenseFileId(HSLC handle, UINT size, const BYTE *blob, SLID *id)
{
    TRACE("(%p %u %p %p)\n", handle, size, blob, id);

    if (!handle || !size || !blob || !id) return E_INVALIDARG;
    return parse_license_id( blob, size, id ) ? S_OK : E_INVALIDARG;
}

HRESULT WINAPI SLUninstallLicense(HSLC handle, const SLID *id)
{
    WCHAR path[MAX_PATH];

    TRACE("(%p %s)\n", handle, debugstr_guid( id ));

    if (!handle || !id) return E_INVALIDARG;
    if (!get_license_path( id, path )) return HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );
    if (!DeleteFileW( path )) return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

/* The ids of the license files in the store: a license file is its own license here. */
HRESULT WINAPI SLGetSLIDList(HSLC handle, SLIDTYPE query_type, const SLID *query_id, SLIDTYPE return_type,
                             UINT *count, SLID **ids)
{
    WCHAR path[MAX_PATH], *name;
    WIN32_FIND_DATAW data;
    HANDLE find;
    SLID id, *list = NULL;
    UINT n = 0, capacity = 0;
    char guid[37];
    unsigned int i;

    TRACE("(%p %d %s %d %p %p)\n", handle, query_type, debugstr_guid( query_id ), return_type, count, ids);

    if (!handle || !count || !ids || query_type >= SL_ID_LAST || return_type >= SL_ID_LAST) return E_INVALIDARG;
    *count = 0;
    *ids = NULL;

    if (query_type != SL_ID_ALL_LICENSES && query_type != SL_ID_ALL_LICENSE_FILES)
    {
        FIXME("query type %d for %s is not served from the store\n", query_type, debugstr_guid( query_id ));
        return S_OK;
    }
    if (return_type != SL_ID_LICENSE && return_type != SL_ID_LICENSE_FILE)
    {
        FIXME("return type %d is not served from the store\n", return_type);
        return S_OK;
    }

    if (!get_license_dir( path, MAX_PATH - 48 )) return HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );
    name = path + wcslen( path );
    wcscpy( name, L"\\*.xrm-ms" );
    if ((find = FindFirstFileW( path, &data )) == INVALID_HANDLE_VALUE) return S_OK;
    do
    {
        if (wcslen( data.cFileName ) != 45 || data.cFileName[0] != '{' || data.cFileName[37] != '}') continue;
        for (i = 0; i < 36; i++) guid[i] = data.cFileName[1 + i] < 128 ? data.cFileName[1 + i] : 0;
        guid[36] = 0;
        if (!parse_guid( guid, &id )) continue;
        if (n == capacity)
        {
            SLID *new_list = LocalAlloc( LMEM_FIXED, (capacity + 16) * sizeof(*list) );
            if (!new_list)
            {
                LocalFree( list );
                FindClose( find );
                return E_OUTOFMEMORY;
            }
            if (list) memcpy( new_list, list, n * sizeof(*list) );
            LocalFree( list );
            list = new_list;
            capacity += 16;
        }
        list[n++] = id;
    } while (FindNextFileW( find, &data ));
    FindClose( find );

    *count = n;
    *ids = list;
    return S_OK;
}

/* Product keys: HKLM\SOFTWARE\SPPWine\ProductKeyStore\{pkey id}, a record with the algorithm and the last five
 * characters of the key (the key itself is not kept). The key id is a stable id derived from the normalised key
 * (placeholder: the clean-room spec asks for the Windows construction, SPP clean-room item U2). */
static BOOL normalize_key( const WCHAR *key, char *out )
{
    unsigned int n = 0;

    for (; *key; key++)
    {
        if (*key == '-' || *key == ' ') continue;
        if (n == 25) return FALSE;
        if (*key >= '0' && *key <= '9') out[n++] = *key;
        else if (*key >= 'a' && *key <= 'z') out[n++] = *key - 'a' + 'A';
        else if (*key >= 'A' && *key <= 'Z') out[n++] = *key;
        else return FALSE;
    }
    out[n] = 0;
    return n == 25;
}

static void derive_key_id( const char *key, SLID *id )
{
    UINT64 h[2] = { 0xcbf29ce484222325ull, 0x84222325cbf29ce4ull };
    unsigned int i;

    for (i = 0; key[i]; i++)
    {
        h[0] = (h[0] ^ (BYTE)key[i]) * 0x100000001b3ull;
        h[1] = (h[1] ^ (BYTE)key[i]) * 0x100000001b3ull;
    }
    memcpy( id, h, sizeof(*id) );
}

static void format_key_id( const SLID *id, WCHAR *buffer )
{
    swprintf( buffer, 39, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", id->Data1, id->Data2, id->Data3,
              id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3], id->Data4[4], id->Data4[5], id->Data4[6],
              id->Data4[7] );
}

HRESULT WINAPI SLInstallProofOfPurchase(HSLC handle, PCWSTR algorithm, PCWSTR key_string, UINT size,
                                        PBYTE data, SLID *key_id)
{
    WCHAR id_string[39], partial[6], name[80];
    char key[26];
    HKEY root, record;
    SLID id;
    LSTATUS ret;
    int i;

    TRACE("(%p %s %p %u %p %p)\n", handle, debugstr_w( algorithm ), key_string, size, data, key_id);

    if (!handle || !algorithm || !*algorithm || !key_string || !key_id) return E_INVALIDARG;
    if (!normalize_key( key_string, key )) return SL_E_INVALID_PRODUCT_KEY;

    derive_key_id( key, &id );
    format_key_id( &id, id_string );
    for (i = 0; i < 5; i++) partial[i] = key[20 + i];
    partial[5] = 0;

    swprintf( name, ARRAY_SIZE(name), L"SOFTWARE\\SPPWine\\ProductKeyStore\\%s", id_string );
    ret = RegCreateKeyExW( HKEY_LOCAL_MACHINE, name, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &record, NULL );
    if (ret) return HRESULT_FROM_WIN32( ret );
    root = record;
    ret = RegSetValueExW( root, L"Algorithm", 0, REG_SZ, (const BYTE *)algorithm,
                          (wcslen( algorithm ) + 1) * sizeof(WCHAR) );
    if (!ret) ret = RegSetValueExW( root, L"PartialKey", 0, REG_SZ, (const BYTE *)partial, sizeof(partial) );
    RegCloseKey( root );
    if (ret) return HRESULT_FROM_WIN32( ret );

    *key_id = id;
    return S_OK;
}

HRESULT WINAPI SLUninstallProofOfPurchase(HSLC handle, const SLID *key_id)
{
    WCHAR id_string[39], name[80];

    TRACE("(%p %s)\n", handle, debugstr_guid( key_id ));

    if (!handle || !key_id) return E_INVALIDARG;
    format_key_id( key_id, id_string );
    swprintf( name, ARRAY_SIZE(name), L"SOFTWARE\\SPPWine\\ProductKeyStore\\%s", id_string );
    RegDeleteKeyExW( HKEY_LOCAL_MACHINE, name, KEY_WOW64_64KEY, 0 ); /* a missing record is not an error */
    return S_OK;
}

HRESULT WINAPI SLGetProductSkuInformation(HSLC handle, const SLID *sku, PCWSTR name, SLDATATYPE *type,
                                          UINT *size, BYTE **value)
{
    FIXME("(%p %s %s %p %p %p) not served from the store\n", handle, debugstr_guid( sku ), debugstr_w( name ),
          type, size, value);

    if (!handle || !sku || !name || !size || !value) return E_INVALIDARG;
    return SL_E_VALUE_NOT_FOUND;
}

HRESULT WINAPI SLInstallLicense(HSLC handle, UINT size, const BYTE *blob, SLID *id)
{
    WCHAR path[MAX_PATH];
    DWORD written;
    HANDLE file;
    SLID license_id;
    BOOL ret;

    TRACE("(%p %u %p %p)\n", handle, size, blob, id);

    if (!handle || !size || !blob || !id) return E_INVALIDARG;
    if (!parse_license_id( blob, size, &license_id )) return E_INVALIDARG;

    if (!get_license_path( &license_id, path )) return HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );

    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    ret = WriteFile( file, blob, size, &written, NULL ) && written == size;
    CloseHandle( file );
    if (!ret)
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        DeleteFileW( path );
        return hr;
    }
    *id = license_id;
    return S_OK;
}

HRESULT WINAPI SLPersistApplicationPolicies(const SLID *app, const SLID *product, DWORD flags)
{
    FIXME("(%s,%s,%lx) stub\n", wine_dbgstr_guid(app), wine_dbgstr_guid(product), flags);

    if (!app)
        return E_INVALIDARG;

    return S_OK;
}
