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

/* The license id is the licenseId="{...}" attribute near the start of the license document. */
static BOOL parse_license_id( const BYTE *blob, UINT size, SLID *id )
{
    static const char tag[] = "licenseId=\"{";
    const char *p = (const char *)blob, *end = p + min( size, 4096 );
    unsigned int i, v = 0;

    for (; p + sizeof(tag) + 36 <= end; p++) if (!memcmp( p, tag, sizeof(tag) - 1 )) break;
    if (p + sizeof(tag) + 36 > end) return FALSE;
    p += sizeof(tag) - 1;

    /* 8-4-4-4-12 hexadecimal digits */
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
    return p[36] == '}';
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

    if (!get_license_dir( path, ARRAY_SIZE(path) - 48 )) return HRESULT_FROM_WIN32( ERROR_PATH_NOT_FOUND );
    swprintf( path + wcslen( path ), 48, L"\\{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}.xrm-ms",
              license_id.Data1, license_id.Data2, license_id.Data3, license_id.Data4[0], license_id.Data4[1],
              license_id.Data4[2], license_id.Data4[3], license_id.Data4[4], license_id.Data4[5],
              license_id.Data4[6], license_id.Data4[7] );

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
