/*
 * Windows.Storage.ApplicationData (WineExplorer UWP U1.4)
 *
 * Copyright 2024 Mohamad Al-Jaf (skeleton)
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
#include <stdio.h>
#include <wchar.h>

#include "private.h"
#include "winreg.h"
#include "appmodel.h"
#include "roapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(data);

/*
 * The data of a package, as observed on Windows 11 (tests/uwp/u1_1/README.md):
 *   %LOCALAPPDATA%\Packages\<family>\LocalState, TempState, RoamingState, LocalCache  (created on first use)
 *   LocalSettings and RoamingSettings: containers of typed values; here in the registry of the user,
 *   HKCU\Software\WineExplorer\Packages\<family>\<Local|Roaming>\<container>, one string per value ("i4:-7")
 *   ClearAsync(Local) removes the local files and settings; ClearAllAsync everything.
 * An unpackaged process has no application data.
 */

#define MAX_SETTINGS_PATH 1024

/* ---- an already completed IAsyncAction ------------------------------------------------------------- */

struct completed_action
{
    IAsyncAction IAsyncAction_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    HRESULT result;
    IAsyncActionCompletedHandler *handler;
};

static inline struct completed_action *impl_from_IAsyncAction( IAsyncAction *iface )
{
    return CONTAINING_RECORD( iface, struct completed_action, IAsyncAction_iface );
}

static inline struct completed_action *impl_from_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct completed_action, IAsyncInfo_iface );
}

static HRESULT WINAPI action_QueryInterface( IAsyncAction *iface, REFIID iid, void **out )
{
    struct completed_action *impl = impl_from_IAsyncAction( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncAction ))
        *out = &impl->IAsyncAction_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo ))
        *out = &impl->IAsyncInfo_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI action_AddRef( IAsyncAction *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncAction( iface )->ref );
}

static ULONG WINAPI action_Release( IAsyncAction *iface )
{
    struct completed_action *impl = impl_from_IAsyncAction( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->handler) IAsyncActionCompletedHandler_Release( impl->handler );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI action_GetIids( IAsyncAction *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI action_GetRuntimeClassName( IAsyncAction *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI action_GetTrustLevel( IAsyncAction *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI action_put_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler *handler )
{
    struct completed_action *impl = impl_from_IAsyncAction( iface );

    if (impl->handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    if ((impl->handler = handler)) IAsyncActionCompletedHandler_AddRef( handler );
    if (handler) IAsyncActionCompletedHandler_Invoke( handler, iface, SUCCEEDED(impl->result) ? Completed : Error );
    return S_OK;
}

static HRESULT WINAPI action_get_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler **handler )
{
    struct completed_action *impl = impl_from_IAsyncAction( iface );

    if ((*handler = impl->handler)) IAsyncActionCompletedHandler_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI action_GetResults( IAsyncAction *iface, void *unused )
{
    return impl_from_IAsyncAction( iface )->result;
}

static const IAsyncActionVtbl action_vtbl =
{
    action_QueryInterface,
    action_AddRef,
    action_Release,
    action_GetIids,
    action_GetRuntimeClassName,
    action_GetTrustLevel,
    (void *)action_put_Completed,
    (void *)action_get_Completed,
    (void *)action_GetResults,
};

static HRESULT WINAPI info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    return action_QueryInterface( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, iid, out );
}

static ULONG WINAPI info_AddRef( IAsyncInfo *iface )
{
    return action_AddRef( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface );
}

static ULONG WINAPI info_Release( IAsyncInfo *iface )
{
    return action_Release( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface );
}

static HRESULT WINAPI info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI info_get_Id( IAsyncInfo *iface, UINT32 *id ) { *id = 1; return S_OK; }

static HRESULT WINAPI info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    *status = SUCCEEDED(impl_from_IAsyncInfo( iface )->result) ? Completed : Error;
    return S_OK;
}

static HRESULT WINAPI info_get_ErrorCode( IAsyncInfo *iface, HRESULT *code )
{
    *code = impl_from_IAsyncInfo( iface )->result;
    return S_OK;
}

static HRESULT WINAPI info_Cancel( IAsyncInfo *iface ) { return S_OK; }
static HRESULT WINAPI info_Close( IAsyncInfo *iface ) { return S_OK; }

static const IAsyncInfoVtbl info_vtbl =
{
    info_QueryInterface,
    info_AddRef,
    info_Release,
    info_GetIids,
    info_GetRuntimeClassName,
    info_GetTrustLevel,
    info_get_Id,
    info_get_Status,
    info_get_ErrorCode,
    info_Cancel,
    info_Close,
};

static HRESULT completed_action_create( HRESULT result, IAsyncAction **out )
{
    struct completed_action *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncAction_iface.lpVtbl = &action_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &info_vtbl;
    impl->ref = 1;
    impl->result = result;
    *out = &impl->IAsyncAction_iface;
    return S_OK;
}

/* ---- values on disk -------------------------------------------------------------------------------------- */

static IPropertyValueStatics *value_statics;

static IPropertyValueStatics *get_value_statics(void)
{
    IPropertyValueStatics *statics = NULL, *old;
    HSTRING name;

    if (value_statics) return value_statics;
    if (FAILED(WindowsCreateString( L"Windows.Foundation.PropertyValue", 32, &name ))) return NULL;
    RoGetActivationFactory( name, &IID_IPropertyValueStatics, (void **)&statics );
    WindowsDeleteString( name );
    if (!statics) return NULL;
    if ((old = InterlockedCompareExchangePointer( (void **)&value_statics, statics, NULL )))
    {
        IPropertyValueStatics_Release( statics );
        statics = old;
    }
    return statics;
}

struct writer
{
    WCHAR *text;
    size_t length, capacity;
    BOOL failed;
};

static void write_string( struct writer *w, const WCHAR *text )
{
    size_t len = wcslen( text );

    if (w->failed) return;
    if (w->length + len + 1 > w->capacity)
    {
        size_t capacity = max( w->capacity * 2, w->length + len + 256 );
        WCHAR *tmp = realloc( w->text, capacity * sizeof(WCHAR) );

        if (!tmp) { w->failed = TRUE; return; }
        w->text = tmp;
        w->capacity = capacity;
    }
    memcpy( w->text + w->length, text, (len + 1) * sizeof(WCHAR) );
    w->length += len;
}

static void write_format( struct writer *w, const WCHAR *format, ... )
{
    WCHAR buffer[128];
    va_list args;

    va_start( args, format );
    vswprintf( buffer, ARRAY_SIZE(buffer), format, args );
    va_end( args );
    write_string( w, buffer );
}

static void encode_value( struct writer *w, IInspectable *value );

static void encode_entry( void *context, const WCHAR *key, IInspectable *value )
{
    struct writer *w = context;
    struct writer inner = {0};

    encode_value( &inner, value );
    write_format( w, L"%u:", (unsigned int)wcslen( key ) );
    write_string( w, key );
    write_format( w, L"%u:", (unsigned int)(inner.text ? wcslen( inner.text ) : 0) );
    if (inner.text) write_string( w, inner.text );
    if (inner.failed) w->failed = TRUE;
    free( inner.text );
}

/* "s:text", "i4:-7", ... and "o:<count>:" then length-prefixed key and value for a composite value */
static void encode_value( struct writer *w, IInspectable *value )
{
    IPropertyValue *pv;
    IPropertySet *set;
    PropertyType type;
    HSTRING text;
    WCHAR buffer[64];

    if (!value) { w->failed = TRUE; return; }
    if (SUCCEEDED(IInspectable_QueryInterface( value, &IID_IPropertyValue, (void **)&pv )))
    {
        if (FAILED(IPropertyValue_get_Type( pv, &type ))) w->failed = TRUE;
        else switch (type)
        {
        case PropertyType_UInt8:  { BYTE v; IPropertyValue_GetUInt8( pv, &v ); write_format( w, L"u1:%u", v ); break; }
        case PropertyType_Int16:  { INT16 v; IPropertyValue_GetInt16( pv, &v ); write_format( w, L"i2:%d", v ); break; }
        case PropertyType_UInt16: { UINT16 v; IPropertyValue_GetUInt16( pv, &v ); write_format( w, L"u2:%u", v ); break; }
        case PropertyType_Int32:  { INT32 v; IPropertyValue_GetInt32( pv, &v ); write_format( w, L"i4:%d", v ); break; }
        case PropertyType_UInt32: { UINT32 v; IPropertyValue_GetUInt32( pv, &v ); write_format( w, L"u4:%u", v ); break; }
        case PropertyType_Int64:  { INT64 v; IPropertyValue_GetInt64( pv, &v ); write_format( w, L"i8:%I64d", v ); break; }
        case PropertyType_UInt64: { UINT64 v; IPropertyValue_GetUInt64( pv, &v ); write_format( w, L"u8:%I64u", v ); break; }
        case PropertyType_Single: { FLOAT v; IPropertyValue_GetSingle( pv, &v ); swprintf( buffer, ARRAY_SIZE(buffer), L"f4:%.9g", v ); write_string( w, buffer ); break; }
        case PropertyType_Double: { DOUBLE v; IPropertyValue_GetDouble( pv, &v ); swprintf( buffer, ARRAY_SIZE(buffer), L"f8:%.17g", v ); write_string( w, buffer ); break; }
        case PropertyType_Boolean: { boolean v; IPropertyValue_GetBoolean( pv, &v ); write_string( w, v ? L"b:1" : L"b:0" ); break; }
        case PropertyType_Char16: { WCHAR v; IPropertyValue_GetChar16( pv, &v ); write_format( w, L"c:%u", v ); break; }
        case PropertyType_String:
            if (SUCCEEDED(IPropertyValue_GetString( pv, &text )))
            {
                write_string( w, L"s:" );
                write_string( w, WindowsGetStringRawBuffer( text, NULL ) );
                WindowsDeleteString( text );
            }
            else w->failed = TRUE;
            break;
        default:
            FIXME( "property type %d cannot be stored.\n", type );
            w->failed = TRUE;
        }
        IPropertyValue_Release( pv );
        return;
    }
    if (SUCCEEDED(IInspectable_QueryInterface( value, &IID_IPropertySet, (void **)&set )))
    {
        struct writer entries = {0};
        UINT32 count = 0;
        IMap_HSTRING_IInspectable *map;

        if (SUCCEEDED(IPropertySet_QueryInterface( set, &IID_IMap_HSTRING_IInspectable, (void **)&map )))
        {
            IMap_HSTRING_IInspectable_get_Size( map, &count );
            IMap_HSTRING_IInspectable_Release( map );
        }
        property_set_enum( set, encode_entry, &entries );
        write_format( w, L"o:%u:", count );
        if (entries.text) write_string( w, entries.text );
        if (entries.failed) w->failed = TRUE;
        free( entries.text );
        IPropertySet_Release( set );
        return;
    }
    w->failed = TRUE;
}

/* a number up to ':', advancing the cursor; (UINT32)-1 when there is none */
static UINT32 read_count( const WCHAR **cursor )
{
    WCHAR *end;
    unsigned long value = wcstoul( *cursor, &end, 10 );

    if (end == *cursor || *end != ':') return ~0u;
    *cursor = end + 1;
    return value;
}

static HRESULT decode_value( const WCHAR *text, size_t len, IInspectable **value );

static HRESULT decode_entries( const WCHAR *text, size_t len, UINT32 count, IPropertySet *set )
{
    const WCHAR *end = text + len;
    UINT32 i;

    for (i = 0; i < count; i++)
    {
        UINT32 key_len, value_len;
        WCHAR *key;
        HSTRING hkey;
        IInspectable *value;
        HRESULT hr;

        if ((key_len = read_count( &text )) == ~0u || text + key_len > end) return E_FAIL;
        if (!(key = malloc( (key_len + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
        memcpy( key, text, key_len * sizeof(WCHAR) );
        key[key_len] = 0;
        text += key_len;
        if ((value_len = read_count( &text )) == ~0u || text + value_len > end) { free( key ); return E_FAIL; }
        hr = decode_value( text, value_len, &value );
        text += value_len;
        if (SUCCEEDED(hr)) hr = WindowsCreateString( key, key_len, &hkey );
        free( key );
        if (FAILED(hr)) return hr;
        hr = property_set_load( set, hkey, value );
        WindowsDeleteString( hkey );
        IInspectable_Release( value );
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

static HRESULT decode_value( const WCHAR *text, size_t len, IInspectable **value )
{
    IPropertyValueStatics *statics = get_value_statics();
    WCHAR *copy, *colon, *end;
    HRESULT hr = E_FAIL;

    *value = NULL;
    if (!statics) return E_FAIL;
    if (!(copy = malloc( (len + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    memcpy( copy, text, len * sizeof(WCHAR) );
    copy[len] = 0;
    if ((colon = wcschr( copy, ':' )))
    {
        const WCHAR *data = colon + 1;

        *colon = 0;
        if (!wcscmp( copy, L"s" ))
        {
            HSTRING string;

            if (SUCCEEDED(hr = WindowsCreateString( data, wcslen( data ), &string )))
            {
                hr = IPropertyValueStatics_CreateString( statics, string, value );
                WindowsDeleteString( string );
            }
        }
        else if (!wcscmp( copy, L"i4" )) hr = IPropertyValueStatics_CreateInt32( statics, wcstol( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"u4" )) hr = IPropertyValueStatics_CreateUInt32( statics, wcstoul( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"i2" )) hr = IPropertyValueStatics_CreateInt16( statics, wcstol( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"u2" )) hr = IPropertyValueStatics_CreateUInt16( statics, wcstoul( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"u1" )) hr = IPropertyValueStatics_CreateUInt8( statics, wcstoul( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"i8" )) hr = IPropertyValueStatics_CreateInt64( statics, _wcstoi64( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"u8" )) hr = IPropertyValueStatics_CreateUInt64( statics, _wcstoui64( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"f4" )) hr = IPropertyValueStatics_CreateSingle( statics, wcstod( data, NULL ), value );
        else if (!wcscmp( copy, L"f8" )) hr = IPropertyValueStatics_CreateDouble( statics, wcstod( data, NULL ), value );
        else if (!wcscmp( copy, L"b" )) hr = IPropertyValueStatics_CreateBoolean( statics, data[0] == '1', value );
        else if (!wcscmp( copy, L"c" )) hr = IPropertyValueStatics_CreateChar16( statics, wcstoul( data, NULL, 10 ), value );
        else if (!wcscmp( copy, L"o" ))
        {
            UINT32 count = wcstoul( data, &end, 10 );
            IPropertySet *set;

            if (*end == ':' && SUCCEEDED(hr = property_set_create( L"Windows.Storage.ApplicationDataCompositeValue", NULL, NULL, &set )))
            {
                if (SUCCEEDED(hr = decode_entries( end + 1, wcslen( end + 1 ), count, set )))
                    hr = IPropertySet_QueryInterface( set, &IID_IInspectable, (void **)value );
                IPropertySet_Release( set );
            }
        }
    }
    free( copy );
    return hr;
}

/* ---- containers ------------------------------------------------------------------------------------------ */

struct container
{
    IApplicationDataContainer IApplicationDataContainer_iface;
    struct list entry;                  /* all containers, so that a path has one object */
    LONG ref;
    ApplicationDataLocality locality;
    WCHAR name[256];
    WCHAR key[MAX_SETTINGS_PATH];       /* below HKEY_CURRENT_USER */
    IPropertySet *values;
};

static CRITICAL_SECTION containers_cs;
static CRITICAL_SECTION_DEBUG containers_cs_debug =
{
    0, 0, &containers_cs,
    { &containers_cs_debug.ProcessLocksList, &containers_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": containers_cs") }
};
static CRITICAL_SECTION containers_cs = { &containers_cs_debug, -1, 0, 0, 0, 0 };
static struct list containers = LIST_INIT( containers );

extern const IApplicationDataContainerVtbl container_vtbl;

static inline struct container *impl_from_IApplicationDataContainer( IApplicationDataContainer *iface )
{
    return CONTAINING_RECORD( iface, struct container, IApplicationDataContainer_iface );
}

static void settings_write_entry( void *context, const WCHAR *key, IInspectable *value )
{
    struct { HKEY hkey; BOOL failed; } *w = context;
    struct writer text = {0};

    encode_value( &text, value );
    if (text.failed || !text.text) w->failed = TRUE;
    else RegSetValueExW( w->hkey, key, 0, REG_SZ, (const BYTE *)text.text, (wcslen( text.text ) + 1) * sizeof(WCHAR) );
    free( text.text );
}

/* writes all the values of the container: the stored ones are replaced; E_INVALIDARG when one cannot be stored */
static HRESULT container_changed( void *context, IPropertySet *set )
{
    struct container *container = context;
    struct { HKEY hkey; BOOL failed; } w = { 0, FALSE };
    DWORD index, size;
    WCHAR name[512];

    if (RegCreateKeyExW( HKEY_CURRENT_USER, container->key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &w.hkey, NULL )) return E_FAIL;
    for (;;)
    {
        size = ARRAY_SIZE(name);
        if (RegEnumValueW( w.hkey, 0, name, &size, NULL, NULL, NULL, NULL )) break;
        RegDeleteValueW( w.hkey, name );
    }
    (void)index;
    property_set_enum( set, settings_write_entry, &w );
    RegCloseKey( w.hkey );
    return w.failed ? E_INVALIDARG : S_OK;
}

static HRESULT container_load( struct container *container )
{
    HKEY hkey;
    DWORD index;
    HRESULT hr;

    if (FAILED(hr = property_set_create( L"Windows.Storage.ApplicationDataContainerSettings", container_changed, container,
                                         &container->values )))
        return hr;
    if (RegOpenKeyExW( HKEY_CURRENT_USER, container->key, 0, KEY_READ, &hkey )) return S_OK;
    for (index = 0;; index++)
    {
        WCHAR name[512];
        DWORD size = ARRAY_SIZE(name), type, data_size = 0;
        WCHAR *data;
        HSTRING key;
        IInspectable *value;

        if (RegEnumValueW( hkey, index, name, &size, NULL, &type, NULL, &data_size )) break;
        if (type != REG_SZ || !(data = malloc( data_size + sizeof(WCHAR) ))) continue;
        size = ARRAY_SIZE(name);
        data_size += sizeof(WCHAR) - 1;
        if (!RegEnumValueW( hkey, index, name, &size, NULL, &type, (BYTE *)data, &data_size ))
        {
            data[data_size / sizeof(WCHAR)] = 0;
            if (SUCCEEDED(decode_value( data, wcslen( data ), &value )))
            {
                if (SUCCEEDED(WindowsCreateString( name, size, &key )))
                {
                    property_set_load( container->values, key, value );
                    WindowsDeleteString( key );
                }
                IInspectable_Release( value );
            }
        }
        free( data );
    }
    RegCloseKey( hkey );
    return S_OK;
}

/* the one container object of a settings path; a reference is returned */
static HRESULT container_get( ApplicationDataLocality locality, const WCHAR *name, const WCHAR *key, struct container **out )
{
    struct container *container;
    HRESULT hr;

    EnterCriticalSection( &containers_cs );
    LIST_FOR_EACH_ENTRY( container, &containers, struct container, entry )
    {
        if (wcsicmp( container->key, key )) continue;
        InterlockedIncrement( &container->ref );
        *out = container;
        LeaveCriticalSection( &containers_cs );
        return S_OK;
    }
    LeaveCriticalSection( &containers_cs );

    if (!(container = calloc( 1, sizeof(*container) ))) return E_OUTOFMEMORY;
    container->ref = 2;     /* the caller and the list */
    container->locality = locality;
    lstrcpynW( container->name, name, ARRAY_SIZE(container->name) );
    lstrcpynW( container->key, key, ARRAY_SIZE(container->key) );
    if (FAILED(hr = container_load( container ))) { free( container ); return hr; }
    container->IApplicationDataContainer_iface.lpVtbl = &container_vtbl;
    EnterCriticalSection( &containers_cs );
    list_add_tail( &containers, &container->entry );
    LeaveCriticalSection( &containers_cs );
    *out = container;
    return S_OK;
}

static HRESULT WINAPI container_QueryInterface( IApplicationDataContainer *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IApplicationDataContainer ))
    {
        *out = iface;
        IApplicationDataContainer_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI container_AddRef( IApplicationDataContainer *iface )
{
    return InterlockedIncrement( &impl_from_IApplicationDataContainer( iface )->ref );
}

/* the list keeps one reference for the life of the process */
static ULONG WINAPI container_Release( IApplicationDataContainer *iface )
{
    return InterlockedDecrement( &impl_from_IApplicationDataContainer( iface )->ref );
}

static HRESULT WINAPI container_GetIids( IApplicationDataContainer *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }

static HRESULT WINAPI container_GetRuntimeClassName( IApplicationDataContainer *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Storage.ApplicationDataContainer", 40, name );
}

static HRESULT WINAPI container_GetTrustLevel( IApplicationDataContainer *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI container_get_Name( IApplicationDataContainer *iface, HSTRING *value )
{
    struct container *impl = impl_from_IApplicationDataContainer( iface );

    return WindowsCreateString( impl->name, wcslen( impl->name ), value );
}

static HRESULT WINAPI container_get_Locality( IApplicationDataContainer *iface, ApplicationDataLocality *value )
{
    *value = impl_from_IApplicationDataContainer( iface )->locality;
    return S_OK;
}

static HRESULT WINAPI container_get_Values( IApplicationDataContainer *iface, IPropertySet **value )
{
    struct container *impl = impl_from_IApplicationDataContainer( iface );

    IPropertySet_AddRef( *value = impl->values );
    return S_OK;
}

/* IMapView<HSTRING, ApplicationDataContainer *>: a snapshot of the sub-containers */
struct container_view
{
    IMapView_HSTRING_ApplicationDataContainer IMapView_iface;
    IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    LONG ref;
    UINT32 count;
    HSTRING *names;
    struct container **containers;
};

static inline struct container_view *impl_from_IMapView( IMapView_HSTRING_ApplicationDataContainer *iface )
{
    return CONTAINING_RECORD( iface, struct container_view, IMapView_iface );
}

static HRESULT WINAPI view_QueryInterface( IMapView_HSTRING_ApplicationDataContainer *iface, REFIID iid, void **out )
{
    struct container_view *impl = impl_from_IMapView( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IMapView_HSTRING_ApplicationDataContainer ))
        *out = &impl->IMapView_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer ))
        *out = &impl->IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI view_AddRef( IMapView_HSTRING_ApplicationDataContainer *iface )
{
    return InterlockedIncrement( &impl_from_IMapView( iface )->ref );
}

static ULONG WINAPI view_Release( IMapView_HSTRING_ApplicationDataContainer *iface )
{
    struct container_view *impl = impl_from_IMapView( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++)
        {
            WindowsDeleteString( impl->names[i] );
            IApplicationDataContainer_Release( &impl->containers[i]->IApplicationDataContainer_iface );
        }
        free( impl->names );
        free( impl->containers );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI view_GetIids( IMapView_HSTRING_ApplicationDataContainer *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI view_GetRuntimeClassName( IMapView_HSTRING_ApplicationDataContainer *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI view_GetTrustLevel( IMapView_HSTRING_ApplicationDataContainer *iface, TrustLevel *level ) { *level = BaseTrust; return S_OK; }

static HRESULT WINAPI view_Lookup( IMapView_HSTRING_ApplicationDataContainer *iface, HSTRING key,
                                   IApplicationDataContainer **value )
{
    struct container_view *impl = impl_from_IMapView( iface );
    UINT32 i;

    for (i = 0; i < impl->count; i++)
        if (!wcscmp( WindowsGetStringRawBuffer( impl->names[i], NULL ), WindowsGetStringRawBuffer( key, NULL ) ))
        {
            IApplicationDataContainer_AddRef( *value = &impl->containers[i]->IApplicationDataContainer_iface );
            return S_OK;
        }
    *value = NULL;
    return E_BOUNDS;
}

static HRESULT WINAPI view_get_Size( IMapView_HSTRING_ApplicationDataContainer *iface, UINT32 *size )
{
    *size = impl_from_IMapView( iface )->count;
    return S_OK;
}

static HRESULT WINAPI view_HasKey( IMapView_HSTRING_ApplicationDataContainer *iface, HSTRING key, boolean *found )
{
    IApplicationDataContainer *value;

    *found = SUCCEEDED(view_Lookup( iface, key, &value ));
    if (*found) IApplicationDataContainer_Release( value );
    return S_OK;
}

static HRESULT WINAPI view_Split( IMapView_HSTRING_ApplicationDataContainer *iface,
                                  IMapView_HSTRING_ApplicationDataContainer **first,
                                  IMapView_HSTRING_ApplicationDataContainer **second )
{
    *first = *second = NULL;
    return S_OK;
}

static const IMapView_HSTRING_ApplicationDataContainerVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    view_GetIids,
    view_GetRuntimeClassName,
    view_GetTrustLevel,
    view_Lookup,
    view_get_Size,
    view_HasKey,
    view_Split,
};

DEFINE_IINSPECTABLE( view_iterable, IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer, struct container_view, IMapView_iface )

static HRESULT WINAPI view_iterable_First( IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer *iface,
                                           IIterator_IKeyValuePair_HSTRING_ApplicationDataContainer **iterator )
{
    FIXME( "iface %p, iterator %p stub!\n", iface, iterator );
    return E_NOTIMPL;
}

static const IIterable_IKeyValuePair_HSTRING_ApplicationDataContainerVtbl view_iterable_vtbl =
{
    view_iterable_QueryInterface,
    view_iterable_AddRef,
    view_iterable_Release,
    view_iterable_GetIids,
    view_iterable_GetRuntimeClassName,
    view_iterable_GetTrustLevel,
    view_iterable_First,
};

static HRESULT WINAPI container_get_Containers( IApplicationDataContainer *iface,
                                                IMapView_HSTRING_ApplicationDataContainer **value )
{
    struct container *impl = impl_from_IApplicationDataContainer( iface );
    struct container_view *view;
    HKEY hkey;
    DWORD index, count = 0;

    if (!(view = calloc( 1, sizeof(*view) ))) return E_OUTOFMEMORY;
    view->IMapView_iface.lpVtbl = &view_vtbl;
    view->IIterable_IKeyValuePair_HSTRING_ApplicationDataContainer_iface.lpVtbl = &view_iterable_vtbl;
    view->ref = 1;
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, impl->key, 0, KEY_READ, &hkey ))
    {
        RegQueryInfoKeyW( hkey, NULL, NULL, NULL, &count, NULL, NULL, NULL, NULL, NULL, NULL, NULL );
        view->names = calloc( count ? count : 1, sizeof(*view->names) );
        view->containers = calloc( count ? count : 1, sizeof(*view->containers) );
        for (index = 0; index < count && view->names && view->containers; index++)
        {
            WCHAR name[256], key[MAX_SETTINGS_PATH];
            DWORD size = ARRAY_SIZE(name);
            struct container *child;

            if (RegEnumKeyExW( hkey, index, name, &size, NULL, NULL, NULL, NULL )) break;
            swprintf( key, ARRAY_SIZE(key), L"%s\\%s", impl->key, name );
            if (FAILED(container_get( impl->locality, name, key, &child ))) continue;
            WindowsCreateString( name, size, &view->names[view->count] );
            view->containers[view->count++] = child;
        }
        RegCloseKey( hkey );
    }
    *value = &view->IMapView_iface;
    return S_OK;
}

static BOOL valid_container_name( const WCHAR *name )
{
    size_t len = name ? wcslen( name ) : 0;

    return len && len <= 255 && !wcschr( name, '\\' );
}

static HRESULT WINAPI container_CreateContainer( IApplicationDataContainer *iface, HSTRING name,
                                                 ApplicationDataCreateDisposition disposition,
                                                 IApplicationDataContainer **container )
{
    struct container *impl = impl_from_IApplicationDataContainer( iface ), *child;
    const WCHAR *text = WindowsGetStringRawBuffer( name, NULL );
    WCHAR key[MAX_SETTINGS_PATH];
    HKEY hkey;
    HRESULT hr;

    TRACE( "iface %p, name %s, disposition %d, container %p.\n", iface, debugstr_hstring( name ), disposition, container );

    if (!valid_container_name( text )) return E_INVALIDARG;
    swprintf( key, ARRAY_SIZE(key), L"%s\\%s", impl->key, text );
    if (disposition == ApplicationDataCreateDisposition_Existing)
    {
        if (RegOpenKeyExW( HKEY_CURRENT_USER, key, 0, KEY_READ, &hkey )) return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
        RegCloseKey( hkey );
    }
    else if (RegCreateKeyExW( HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL )) return E_FAIL;
    else RegCloseKey( hkey );
    if (FAILED(hr = container_get( impl->locality, text, key, &child ))) return hr;
    *container = &child->IApplicationDataContainer_iface;
    return S_OK;
}

static HRESULT WINAPI container_DeleteContainer( IApplicationDataContainer *iface, HSTRING name )
{
    struct container *impl = impl_from_IApplicationDataContainer( iface ), *child, *next;
    const WCHAR *text = WindowsGetStringRawBuffer( name, NULL );
    WCHAR key[MAX_SETTINGS_PATH];
    size_t len;

    TRACE( "iface %p, name %s.\n", iface, debugstr_hstring( name ) );

    if (!valid_container_name( text )) return E_INVALIDARG;
    swprintf( key, ARRAY_SIZE(key), L"%s\\%s", impl->key, text );
    len = wcslen( key );
    RegDeleteTreeW( HKEY_CURRENT_USER, key );
    /* the objects of the deleted tree stay valid and empty */
    EnterCriticalSection( &containers_cs );
    LIST_FOR_EACH_ENTRY_SAFE( child, next, &containers, struct container, entry )
        if (!wcsnicmp( child->key, key, len ) && (!child->key[len] || child->key[len] == '\\'))
        {
            IMap_HSTRING_IInspectable *map;

            if (SUCCEEDED(IPropertySet_QueryInterface( child->values, &IID_IMap_HSTRING_IInspectable, (void **)&map )))
            {
                RegDeleteTreeW( HKEY_CURRENT_USER, child->key );
                IMap_HSTRING_IInspectable_Clear( map );
                RegDeleteTreeW( HKEY_CURRENT_USER, child->key );
                IMap_HSTRING_IInspectable_Release( map );
            }
        }
    LeaveCriticalSection( &containers_cs );
    return S_OK;
}

const IApplicationDataContainerVtbl container_vtbl =
{
    container_QueryInterface,
    container_AddRef,
    container_Release,
    container_GetIids,
    container_GetRuntimeClassName,
    container_GetTrustLevel,
    container_get_Name,
    container_get_Locality,
    container_get_Values,
    container_get_Containers,
    container_CreateContainer,
    container_DeleteContainer,
};

/* ---- the application data of the process's package --------------------------------------------------------- */

struct application_data
{
    IApplicationData IApplicationData_iface;
    IApplicationData2 IApplicationData2_iface;
    IApplicationData3 IApplicationData3_iface;
    LONG ref;
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    WCHAR base[MAX_PATH];                          /* %LOCALAPPDATA%\Packages\<family> */
    WCHAR settings_key[MAX_SETTINGS_PATH];         /* Software\WineExplorer\Packages\<family> */
    struct container *local, *roaming;
    ITypedEventHandler_ApplicationData_IInspectable *handlers[16];
    CRITICAL_SECTION cs;
};

static struct application_data *current_data;
static CRITICAL_SECTION current_cs;
static CRITICAL_SECTION_DEBUG current_cs_debug =
{
    0, 0, &current_cs,
    { &current_cs_debug.ProcessLocksList, &current_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": current_cs") }
};
static CRITICAL_SECTION current_cs = { &current_cs_debug, -1, 0, 0, 0, 0 };

static inline struct application_data *impl_from_IApplicationData( IApplicationData *iface )
{
    return CONTAINING_RECORD( iface, struct application_data, IApplicationData_iface );
}

static void ensure_directory( const WCHAR *path )
{
    WCHAR copy[MAX_PATH], *p;

    lstrcpynW( copy, path, ARRAY_SIZE(copy) );
    for (p = copy + 3; *p; p++)
    {
        if (*p != '\\') continue;
        *p = 0;
        CreateDirectoryW( copy, NULL );
        *p = '\\';
    }
    CreateDirectoryW( copy, NULL );
}

/* the StorageFolder of a directory, created when it is not there */
static HRESULT folder_for( struct application_data *impl, const WCHAR *leaf, IStorageFolder **folder )
{
    IStorageFolderStatics *statics;
    IAsyncOperation_StorageFolder *operation;
    IAsyncInfo *info;
    WCHAR path[MAX_PATH];
    HSTRING name, hpath;
    AsyncStatus status = Started;
    DWORD start = GetTickCount();
    HRESULT hr;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", impl->base, leaf );
    ensure_directory( path );
    if (FAILED(hr = WindowsCreateString( L"Windows.Storage.StorageFolder", 29, &name ))) return hr;
    hr = RoGetActivationFactory( name, &IID_IStorageFolderStatics, (void **)&statics );
    WindowsDeleteString( name );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = WindowsCreateString( path, wcslen( path ), &hpath )))
    {
        hr = IStorageFolderStatics_GetFolderFromPathAsync( statics, hpath, &operation );
        WindowsDeleteString( hpath );
    }
    IStorageFolderStatics_Release( statics );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = IAsyncOperation_StorageFolder_QueryInterface( operation, &IID_IAsyncInfo, (void **)&info )))
    {
        while (SUCCEEDED(IAsyncInfo_get_Status( info, &status )) && status == Started && GetTickCount() - start < 10000)
            Sleep( 1 );
        IAsyncInfo_Release( info );
        hr = status == Completed ? IAsyncOperation_StorageFolder_GetResults( operation, folder ) : E_FAIL;
    }
    IAsyncOperation_StorageFolder_Release( operation );
    return hr;
}

static HRESULT WINAPI application_data_QueryInterface( IApplicationData *iface, REFIID iid, void **out )
{
    struct application_data *impl = impl_from_IApplicationData( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IApplicationData ))
        *out = &impl->IApplicationData_iface;
    else if (IsEqualGUID( iid, &IID_IApplicationData2 ))
        *out = &impl->IApplicationData2_iface;
    else if (IsEqualGUID( iid, &IID_IApplicationData3 ))
        *out = &impl->IApplicationData3_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

/* the object lives as long as the process: Current is a singleton */
static ULONG WINAPI application_data_AddRef( IApplicationData *iface )
{
    return InterlockedIncrement( &impl_from_IApplicationData( iface )->ref );
}

static ULONG WINAPI application_data_Release( IApplicationData *iface )
{
    return InterlockedDecrement( &impl_from_IApplicationData( iface )->ref );
}

static HRESULT WINAPI application_data_GetIids( IApplicationData *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }

static HRESULT WINAPI application_data_GetRuntimeClassName( IApplicationData *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Storage.ApplicationData", 30, name );
}

static HRESULT WINAPI application_data_GetTrustLevel( IApplicationData *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI application_data_get_Version( IApplicationData *iface, UINT32 *value )
{
    struct application_data *impl = impl_from_IApplicationData( iface );
    DWORD data = 0, size = sizeof(data);

    RegGetValueW( HKEY_CURRENT_USER, impl->settings_key, L"Version", RRF_RT_REG_DWORD, NULL, &data, &size );
    *value = data;
    return S_OK;
}

static HRESULT WINAPI application_data_SetVersionAsync( IApplicationData *iface, UINT32 version,
                                                        IApplicationDataSetVersionHandler *handler,
                                                        IAsyncAction **operation )
{
    struct application_data *impl = impl_from_IApplicationData( iface );
    HKEY hkey;

    FIXME( "iface %p, version %u, handler %p: the handler is not called.\n", iface, version, handler );

    if (!RegCreateKeyExW( HKEY_CURRENT_USER, impl->settings_key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL ))
    {
        RegSetValueExW( hkey, L"Version", 0, REG_DWORD, (const BYTE *)&version, sizeof(version) );
        RegCloseKey( hkey );
    }
    return completed_action_create( S_OK, operation );
}

static void delete_tree( const WCHAR *directory, BOOL remove_directory )
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;

    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", directory );
    if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
            swprintf( path, ARRAY_SIZE(path), L"%s\\%s", directory, data.cFileName );
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree( path, TRUE );
            else
            {
                SetFileAttributesW( path, FILE_ATTRIBUTE_NORMAL );
                DeleteFileW( path );
            }
        }
        while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (remove_directory) RemoveDirectoryW( directory );
}

static void clear_container( struct container *container )
{
    struct container *child, *next;
    size_t len = wcslen( container->key );

    RegDeleteTreeW( HKEY_CURRENT_USER, container->key );
    EnterCriticalSection( &containers_cs );
    LIST_FOR_EACH_ENTRY_SAFE( child, next, &containers, struct container, entry )
        if (!wcsnicmp( child->key, container->key, len ) && (!child->key[len] || child->key[len] == '\\'))
        {
            IMap_HSTRING_IInspectable *map;

            if (SUCCEEDED(IPropertySet_QueryInterface( child->values, &IID_IMap_HSTRING_IInspectable, (void **)&map )))
            {
                IMap_HSTRING_IInspectable_Clear( map );
                IMap_HSTRING_IInspectable_Release( map );
            }
        }
    LeaveCriticalSection( &containers_cs );
    RegDeleteTreeW( HKEY_CURRENT_USER, container->key );
}

static void clear_locality( struct application_data *impl, ApplicationDataLocality locality )
{
    WCHAR path[MAX_PATH];

    switch (locality)
    {
    case ApplicationDataLocality_Local:
        swprintf( path, ARRAY_SIZE(path), L"%s\\LocalState", impl->base );
        delete_tree( path, FALSE );
        clear_container( impl->local );
        break;
    case ApplicationDataLocality_Roaming:
        swprintf( path, ARRAY_SIZE(path), L"%s\\RoamingState", impl->base );
        delete_tree( path, FALSE );
        clear_container( impl->roaming );
        break;
    case ApplicationDataLocality_Temporary:
        swprintf( path, ARRAY_SIZE(path), L"%s\\TempState", impl->base );
        delete_tree( path, FALSE );
        break;
    case ApplicationDataLocality_LocalCache:
        swprintf( path, ARRAY_SIZE(path), L"%s\\LocalCache", impl->base );
        delete_tree( path, FALSE );
        break;
    default: break;
    }
}

static HRESULT WINAPI application_data_ClearAllAsync( IApplicationData *iface, IAsyncAction **operation )
{
    struct application_data *impl = impl_from_IApplicationData( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    clear_locality( impl, ApplicationDataLocality_Local );
    clear_locality( impl, ApplicationDataLocality_Roaming );
    clear_locality( impl, ApplicationDataLocality_Temporary );
    clear_locality( impl, ApplicationDataLocality_LocalCache );
    return completed_action_create( S_OK, operation );
}

static HRESULT WINAPI application_data_ClearAsync( IApplicationData *iface, ApplicationDataLocality locality,
                                                   IAsyncAction **operation )
{
    struct application_data *impl = impl_from_IApplicationData( iface );

    TRACE( "iface %p, locality %d, operation %p.\n", iface, locality, operation );

    if (locality < ApplicationDataLocality_Local || locality > ApplicationDataLocality_LocalCache) return E_INVALIDARG;
    clear_locality( impl, locality );
    return completed_action_create( S_OK, operation );
}

static HRESULT WINAPI application_data_get_LocalSettings( IApplicationData *iface, IApplicationDataContainer **value )
{
    struct application_data *impl = impl_from_IApplicationData( iface );

    IApplicationDataContainer_AddRef( *value = &impl->local->IApplicationDataContainer_iface );
    return S_OK;
}

static HRESULT WINAPI application_data_get_RoamingSettings( IApplicationData *iface, IApplicationDataContainer **value )
{
    struct application_data *impl = impl_from_IApplicationData( iface );

    IApplicationDataContainer_AddRef( *value = &impl->roaming->IApplicationDataContainer_iface );
    return S_OK;
}

static HRESULT WINAPI application_data_get_LocalFolder( IApplicationData *iface, IStorageFolder **value )
{
    return folder_for( impl_from_IApplicationData( iface ), L"LocalState", value );
}

static HRESULT WINAPI application_data_get_RoamingFolder( IApplicationData *iface, IStorageFolder **value )
{
    return folder_for( impl_from_IApplicationData( iface ), L"RoamingState", value );
}

static HRESULT WINAPI application_data_get_TemporaryFolder( IApplicationData *iface, IStorageFolder **value )
{
    return folder_for( impl_from_IApplicationData( iface ), L"TempState", value );
}

static HRESULT WINAPI application_data_add_DataChanged( IApplicationData *iface,
                                                        ITypedEventHandler_ApplicationData_IInspectable *handler,
                                                        EventRegistrationToken *token )
{
    struct application_data *impl = impl_from_IApplicationData( iface );
    UINT32 i;
    HRESULT hr = E_OUTOFMEMORY;

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    for (i = 0; i < ARRAY_SIZE(impl->handlers); i++)
        if (!impl->handlers[i])
        {
            ITypedEventHandler_ApplicationData_IInspectable_AddRef( impl->handlers[i] = handler );
            token->value = i + 1;
            hr = S_OK;
            break;
        }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI application_data_remove_DataChanged( IApplicationData *iface, EventRegistrationToken token )
{
    struct application_data *impl = impl_from_IApplicationData( iface );
    ITypedEventHandler_ApplicationData_IInspectable *handler = NULL;

    EnterCriticalSection( &impl->cs );
    if (token.value >= 1 && token.value <= ARRAY_SIZE(impl->handlers))
    {
        handler = impl->handlers[token.value - 1];
        impl->handlers[token.value - 1] = NULL;
    }
    LeaveCriticalSection( &impl->cs );
    if (handler) ITypedEventHandler_ApplicationData_IInspectable_Release( handler );
    return S_OK;
}

static HRESULT WINAPI application_data_SignalDataChanged( IApplicationData *iface )
{
    struct application_data *impl = impl_from_IApplicationData( iface );
    ITypedEventHandler_ApplicationData_IInspectable *handlers[ARRAY_SIZE(impl->handlers)] = {0};
    UINT32 i;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->cs );
    for (i = 0; i < ARRAY_SIZE(handlers); i++)
        if ((handlers[i] = impl->handlers[i])) ITypedEventHandler_ApplicationData_IInspectable_AddRef( handlers[i] );
    LeaveCriticalSection( &impl->cs );
    for (i = 0; i < ARRAY_SIZE(handlers); i++)
    {
        if (!handlers[i]) continue;
        ITypedEventHandler_ApplicationData_IInspectable_Invoke( handlers[i], iface, NULL );
        ITypedEventHandler_ApplicationData_IInspectable_Release( handlers[i] );
    }
    return S_OK;
}

static HRESULT WINAPI application_data_get_RoamingStorageQuota( IApplicationData *iface, UINT64 *value )
{
    *value = 0;
    return S_OK;
}

static const IApplicationDataVtbl application_data_vtbl =
{
    application_data_QueryInterface,
    application_data_AddRef,
    application_data_Release,
    application_data_GetIids,
    application_data_GetRuntimeClassName,
    application_data_GetTrustLevel,
    application_data_get_Version,
    application_data_SetVersionAsync,
    application_data_ClearAllAsync,
    application_data_ClearAsync,
    application_data_get_LocalSettings,
    application_data_get_RoamingSettings,
    application_data_get_LocalFolder,
    application_data_get_RoamingFolder,
    application_data_get_TemporaryFolder,
    application_data_add_DataChanged,
    application_data_remove_DataChanged,
    application_data_SignalDataChanged,
    application_data_get_RoamingStorageQuota,
};

DEFINE_IINSPECTABLE( application_data2, IApplicationData2, struct application_data, IApplicationData_iface )

static HRESULT WINAPI application_data2_get_LocalCacheFolder( IApplicationData2 *iface, IStorageFolder **value )
{
    struct application_data *impl = CONTAINING_RECORD( iface, struct application_data, IApplicationData2_iface );

    return folder_for( impl, L"LocalCache", value );
}

static const IApplicationData2Vtbl application_data2_vtbl =
{
    application_data2_QueryInterface,
    application_data2_AddRef,
    application_data2_Release,
    application_data2_GetIids,
    application_data2_GetRuntimeClassName,
    application_data2_GetTrustLevel,
    application_data2_get_LocalCacheFolder,
};

DEFINE_IINSPECTABLE( application_data3, IApplicationData3, struct application_data, IApplicationData_iface )

static HRESULT WINAPI application_data3_GetPublisherCacheFolder( IApplicationData3 *iface, HSTRING name, IStorageFolder **value )
{
    FIXME( "iface %p, name %s, value %p stub!\n", iface, debugstr_hstring( name ), value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data3_ClearPublisherCacheFolderAsync( IApplicationData3 *iface, HSTRING name,
                                                                        IAsyncAction **operation )
{
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_data3_get_SharedLocalFolder( IApplicationData3 *iface, IStorageFolder **value )
{
    FIXME( "iface %p, value %p: no shared local folder.\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static const IApplicationData3Vtbl application_data3_vtbl =
{
    application_data3_QueryInterface,
    application_data3_AddRef,
    application_data3_Release,
    application_data3_GetIids,
    application_data3_GetRuntimeClassName,
    application_data3_GetTrustLevel,
    application_data3_GetPublisherCacheFolder,
    application_data3_ClearPublisherCacheFolderAsync,
    application_data3_get_SharedLocalFolder,
};

/* the application data of the package of this process */
static HRESULT application_data_get_current( struct application_data **out )
{
    struct application_data *impl;
    WCHAR local_app_data[MAX_PATH], key[MAX_SETTINGS_PATH];
    UINT32 length = 0;
    LONG err;
    HRESULT hr;

    EnterCriticalSection( &current_cs );
    if ((impl = current_data))
    {
        LeaveCriticalSection( &current_cs );
        *out = impl;
        return S_OK;
    }
    if (!(impl = calloc( 1, sizeof(*impl) ))) { LeaveCriticalSection( &current_cs ); return E_OUTOFMEMORY; }
    length = ARRAY_SIZE(impl->family);
    if ((err = GetCurrentPackageFamilyName( &length, impl->family )))
    {
        free( impl );
        LeaveCriticalSection( &current_cs );
        return HRESULT_FROM_WIN32( err );
    }
    if (!GetEnvironmentVariableW( L"LOCALAPPDATA", local_app_data, ARRAY_SIZE(local_app_data) ))
    {
        free( impl );
        LeaveCriticalSection( &current_cs );
        return E_FAIL;
    }
    swprintf( impl->base, ARRAY_SIZE(impl->base), L"%s\\Packages\\%s", local_app_data, impl->family );
    swprintf( impl->settings_key, ARRAY_SIZE(impl->settings_key), L"Software\\WineExplorer\\Packages\\%s", impl->family );
    impl->IApplicationData_iface.lpVtbl = &application_data_vtbl;
    impl->IApplicationData2_iface.lpVtbl = &application_data2_vtbl;
    impl->IApplicationData3_iface.lpVtbl = &application_data3_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": application_data.cs");
    swprintf( key, ARRAY_SIZE(key), L"%s\\Local", impl->settings_key );
    if (FAILED(hr = container_get( ApplicationDataLocality_Local, L"", key, &impl->local ))) goto failed;
    swprintf( key, ARRAY_SIZE(key), L"%s\\Roaming", impl->settings_key );
    if (FAILED(hr = container_get( ApplicationDataLocality_Roaming, L"", key, &impl->roaming ))) goto failed;
    current_data = impl;
    LeaveCriticalSection( &current_cs );
    *out = impl;
    return S_OK;

failed:
    DeleteCriticalSection( &impl->cs );
    free( impl );
    LeaveCriticalSection( &current_cs );
    return hr;
}

/* ---- the factories ----------------------------------------------------------------------------------- */

struct application_data_statics
{
    IActivationFactory IActivationFactory_iface;
    IApplicationDataStatics IApplicationDataStatics_iface;
    LONG ref;
};

static inline struct application_data_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct application_data_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct application_data_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IApplicationDataStatics ))
    {
        *out = &impl->IApplicationDataStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct application_data_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct application_data_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( application_data_statics, IApplicationDataStatics, struct application_data_statics, IActivationFactory_iface )

static HRESULT WINAPI application_data_statics_get_Current( IApplicationDataStatics *iface, IApplicationData **value )
{
    struct application_data *impl;
    HRESULT hr;

    TRACE( "iface %p, value %p\n", iface, value );

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (FAILED(hr = application_data_get_current( &impl ))) return hr;
    IApplicationData_AddRef( *value = &impl->IApplicationData_iface );
    return S_OK;
}

static const struct IApplicationDataStaticsVtbl application_data_statics_vtbl =
{
    application_data_statics_QueryInterface,
    application_data_statics_AddRef,
    application_data_statics_Release,
    /* IInspectable methods */
    application_data_statics_GetIids,
    application_data_statics_GetRuntimeClassName,
    application_data_statics_GetTrustLevel,
    /* IApplicationDataStatics methods */
    application_data_statics_get_Current,
};

static struct application_data_statics application_data_statics =
{
    {&factory_vtbl},
    {&application_data_statics_vtbl},
    1,
};

IActivationFactory *application_data_factory = &application_data_statics.IActivationFactory_iface;

/* Windows.Storage.ApplicationDataCompositeValue: a property set of its own that settings can hold */
static HRESULT WINAPI composite_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    IPropertySet *set;
    HRESULT hr;

    if (FAILED(hr = property_set_create( L"Windows.Storage.ApplicationDataCompositeValue", NULL, NULL, &set ))) return hr;
    hr = IPropertySet_QueryInterface( set, &IID_IInspectable, (void **)instance );
    IPropertySet_Release( set );
    return hr;
}

static const struct IActivationFactoryVtbl composite_factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    composite_factory_ActivateInstance,
};

static struct application_data_statics composite_statics =
{
    {&composite_factory_vtbl},
    {&application_data_statics_vtbl},
    1,
};

IActivationFactory *composite_value_factory = &composite_statics.IActivationFactory_iface;
