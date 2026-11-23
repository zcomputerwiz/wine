/* WinRT IStaticLifetime of Windows.ApplicationModel.Core.CoreApplication
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

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/*
 * C++/WinRT keeps the factories of its classes that have static lifetime in a process-wide map that
 * CoreApplication hands out (winrt::get_static_lifetime_map): GetCollection of IStaticLifetime returns an
 * IMap<HSTRING, IInspectable> (the interface is not in the public metadata). Both are given here by hand: the
 * collection is one map of the process that lives as long as the process.
 */

const GUID IID_IStaticLifetime_twinapi = {0x17b0e613, 0x942a, 0x422d, {0x90, 0x4c, 0xf9, 0x0d, 0xc7, 0x1a, 0x7d, 0xae}};
static const GUID IID_IStaticLifetimeCollection = {0x1b0d3570, 0x0877, 0x5ec2, {0x8a, 0x2c, 0x3b, 0x95, 0x39, 0x50, 0x6a, 0xca}};

struct lifetime_vtbl
{
    HRESULT (WINAPI *QueryInterface)( void *iface, REFIID iid, void **out );
    ULONG (WINAPI *AddRef)( void *iface );
    ULONG (WINAPI *Release)( void *iface );
    HRESULT (WINAPI *GetIids)( void *iface, ULONG *count, IID **iids );
    HRESULT (WINAPI *GetRuntimeClassName)( void *iface, HSTRING *name );
    HRESULT (WINAPI *GetTrustLevel)( void *iface, TrustLevel *level );
    void *slots[8];
};

struct lifetime_object
{
    const struct lifetime_vtbl *vtbl;
};

struct map_entry
{
    HSTRING key;
    IInspectable *value;
};

static CRITICAL_SECTION map_cs;
static CRITICAL_SECTION_DEBUG map_cs_debug =
{
    0, 0, &map_cs,
    { &map_cs_debug.ProcessLocksList, &map_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": map_cs") }
};
static CRITICAL_SECTION map_cs = { &map_cs_debug, -1, 0, 0, 0, 0 };

static struct map_entry *map_entries;
static UINT32 map_count, map_capacity;

static HRESULT WINAPI object_GetIids( void *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI object_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( NULL, 0, name );
}

static HRESULT WINAPI object_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static ULONG WINAPI object_AddRef( void *iface )
{
    return 2;  /* process-wide, never freed */
}

static ULONG WINAPI object_Release( void *iface )
{
    return 1;
}

static int find_entry( HSTRING key )
{
    UINT32 i;
    INT32 result;

    for (i = 0; i < map_count; i++)
        if (SUCCEEDED( WindowsCompareStringOrdinal( map_entries[i].key, key, &result ) ) && !result) return i;
    return -1;
}

static HRESULT WINAPI map_Lookup( void *iface, HSTRING key, IInspectable **value )
{
    int i;

    TRACE( "key %s, value %p.\n", debugstr_hstring( key ), value );

    *value = NULL;
    EnterCriticalSection( &map_cs );
    if ((i = find_entry( key )) >= 0)
    {
        *value = map_entries[i].value;
        IInspectable_AddRef( *value );
    }
    LeaveCriticalSection( &map_cs );
    return i >= 0 ? S_OK : E_BOUNDS;
}

static HRESULT WINAPI map_get_Size( void *iface, UINT32 *size )
{
    *size = map_count;
    return S_OK;
}

static HRESULT WINAPI map_HasKey( void *iface, HSTRING key, boolean *found )
{
    EnterCriticalSection( &map_cs );
    *found = find_entry( key ) >= 0;
    LeaveCriticalSection( &map_cs );
    return S_OK;
}

static HRESULT WINAPI map_GetView( void *iface, void **view )
{
    FIXME( "view %p stub!\n", view );
    *view = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI map_Insert( void *iface, HSTRING key, IInspectable *value, boolean *replaced )
{
    HRESULT hr = S_OK;
    int i;

    TRACE( "key %s, value %p.\n", debugstr_hstring( key ), value );

    EnterCriticalSection( &map_cs );
    if ((i = find_entry( key )) >= 0)
    {
        IInspectable *old = map_entries[i].value;

        IInspectable_AddRef( value );
        map_entries[i].value = value;
        IInspectable_Release( old );
        *replaced = TRUE;
    }
    else
    {
        if (map_count == map_capacity)
        {
            UINT32 capacity = map_capacity ? map_capacity * 2 : 16;
            struct map_entry *entries = realloc( map_entries, capacity * sizeof(*entries) );

            if (!entries) hr = E_OUTOFMEMORY;
            else
            {
                map_entries = entries;
                map_capacity = capacity;
            }
        }
        if (SUCCEEDED( hr ) && SUCCEEDED( hr = WindowsDuplicateString( key, &map_entries[map_count].key ) ))
        {
            IInspectable_AddRef( value );
            map_entries[map_count].value = value;
            map_count++;
        }
        *replaced = FALSE;
    }
    LeaveCriticalSection( &map_cs );
    return hr;
}

static HRESULT WINAPI map_Remove( void *iface, HSTRING key )
{
    HRESULT hr = E_BOUNDS;
    int i;

    EnterCriticalSection( &map_cs );
    if ((i = find_entry( key )) >= 0)
    {
        WindowsDeleteString( map_entries[i].key );
        IInspectable_Release( map_entries[i].value );
        memmove( &map_entries[i], &map_entries[i + 1], (map_count - i - 1) * sizeof(*map_entries) );
        map_count--;
        hr = S_OK;
    }
    LeaveCriticalSection( &map_cs );
    return hr;
}

static HRESULT WINAPI map_Clear( void *iface )
{
    EnterCriticalSection( &map_cs );
    while (map_count)
    {
        map_count--;
        WindowsDeleteString( map_entries[map_count].key );
        IInspectable_Release( map_entries[map_count].value );
    }
    LeaveCriticalSection( &map_cs );
    return S_OK;
}

static HRESULT WINAPI lifetime_QueryInterface( void *iface, REFIID iid, void **out );

static const struct lifetime_vtbl map_vtbl =
{
    lifetime_QueryInterface, object_AddRef, object_Release, object_GetIids, object_GetRuntimeClassName,
    object_GetTrustLevel,
    { map_Lookup, map_get_Size, map_HasKey, map_GetView, map_Insert, map_Remove, map_Clear },
};

static struct lifetime_object map_object = { &map_vtbl };

static HRESULT WINAPI lifetime_GetCollection( void *iface, void **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    *value = &map_object;
    return S_OK;
}

static HRESULT WINAPI lifetime_unused( void *iface )
{
    return E_NOTIMPL;
}

static const struct lifetime_vtbl lifetime_vtbl =
{
    lifetime_QueryInterface, object_AddRef, object_Release, object_GetIids, object_GetRuntimeClassName,
    object_GetTrustLevel,
    { lifetime_unused, lifetime_GetCollection },
};

static struct lifetime_object lifetime_object = { &lifetime_vtbl };

static HRESULT WINAPI lifetime_QueryInterface( void *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ))
        *out = iface;
    else if (iface == &lifetime_object && IsEqualGUID( iid, &IID_IStaticLifetime_twinapi ))
        *out = &lifetime_object;
    else if (iface == &map_object && IsEqualGUID( iid, &IID_IStaticLifetimeCollection ))
        *out = &map_object;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

/* the IStaticLifetime interface of the process, for the factory of CoreApplication */
void *get_static_lifetime(void)
{
    return &lifetime_object;
}
