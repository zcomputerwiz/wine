/*
 * Windows.ApplicationModel.Resources and Windows.ApplicationModel.Resources.Core (package resources)
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

/* The behaviour follows what Windows 11 did with the probe of WineExplorer (tests/uwp/u1_1, application
 * ViewResources): names relative to the map of a loader, a leading slash for an absolute name, missing names give an
 * empty string (ResourceLoader) or a null candidate (ResourceMap.GetValue), a missing map is 0x80073B1F, the
 * view-independent context is one object shared by all its users, file candidates are full paths. The resources
 * come from the resources.pri of the package (or, without a package, of the folder of the program). */

#include <stdlib.h>
#include "private.h"
#include "objbase.h"
#include "roapi.h"
#include "winnls.h"
#include "winreg.h"
#include "appmodel.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(model);

#define ERROR_MRM_MAP_NOT_FOUND_HR ((HRESULT)0x80073b1f)

/* ---- the resource file of the process --------------------------------------------------------------------- */

static struct pri *process_pri;
static HRESULT process_pri_hr;
static INIT_ONCE process_pri_once = INIT_ONCE_STATIC_INIT;

static BOOL WINAPI load_process_pri( INIT_ONCE *once, void *param, void **context )
{
    WCHAR root[MAX_PATH], *slash;
    UINT32 length = ARRAY_SIZE(root);

    if (GetCurrentPackagePath( &length, root ))
    {
        /* no package: the resources.pri beside the program */
        if (!GetModuleFileNameW( NULL, root, ARRAY_SIZE(root) )) root[0] = 0;
        if ((slash = wcsrchr( root, '\\' ))) *slash = 0;
    }
    process_pri_hr = pri_load( root, &process_pri );
    return TRUE;
}

static HRESULT get_pri( struct pri **pri )
{
    InitOnceExecuteOnce( &process_pri_once, load_process_pri, NULL, NULL );
    *pri = process_pri;
    return process_pri_hr;
}

static BOOL map_exists( const struct pri *pri, const WCHAR *prefix )
{
    size_t length = wcslen( prefix );
    UINT32 i;

    for (i = 0; i < pri->item_count; i++)
        if (!wcsnicmp( pri->items[i].name, prefix, length )) return TRUE;
    return FALSE;
}

static HRESULT create_uri( const WCHAR *text, IUriRuntimeClass **uri )
{
    static const WCHAR *class_name = L"Windows.Foundation.Uri";
    IUriRuntimeClassFactory *factory;
    HSTRING_HEADER header;
    HSTRING string, value;
    HRESULT hr;

    *uri = NULL;
    if (FAILED(hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &header, &string ))) return hr;
    if (FAILED(hr = RoGetActivationFactory( string, &IID_IUriRuntimeClassFactory, (void **)&factory ))) return hr;
    if (SUCCEEDED(hr = WindowsCreateString( text, wcslen( text ), &value )))
    {
        hr = IUriRuntimeClassFactory_CreateUri( factory, value, uri );
        WindowsDeleteString( value );
    }
    IUriRuntimeClassFactory_Release( factory );
    return hr;
}

/* "ms-resource://name/" followed by a path */
static HRESULT create_resource_uri( const struct pri *pri, const WCHAR *path, IUriRuntimeClass **uri )
{
    size_t length = wcslen( pri->name ) + wcslen( path ) + 16;
    WCHAR *text;
    HRESULT hr;

    if (!(text = malloc( length * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    swprintf( text, length, L"ms-resource://%s/%s", pri->name, path );
    hr = create_uri( text, uri );
    free( text );
    return hr;
}

/* The full name of a resource: base is the prefix of the map ("" or "Resources/"). A leading slash makes the name
 * absolute; an ms-resource URI is made absolute by its path, or relative without one ("ms-resource:Greeting"). */
static WCHAR *resource_full_name( const WCHAR *base, const WCHAR *name )
{
    size_t length;
    WCHAR *full;

    if (!wcsnicmp( name, L"ms-resource:", 12 ))
    {
        name += 12;
        if (name[0] == '/' && name[1] == '/')
        {
            name += 2;
            while (*name && *name != '/') name++;
        }
    }
    if (*name == '/')
    {
        while (*name == '/') name++;
        base = L"";
    }
    length = wcslen( base ) + wcslen( name ) + 1;
    if (!(full = malloc( length * sizeof(WCHAR) ))) return NULL;
    wcscpy( full, base );
    wcscat( full, name );
    return full;
}

/* ---- the qualifier values of a context: IObservableMap<HSTRING, HSTRING> ---------------------------------- */

struct qualifier_entry
{
    HSTRING key;
    HSTRING value;
};

struct qualifier_map
{
    IObservableMap_HSTRING_HSTRING IObservableMap_HSTRING_HSTRING_iface;
    IMap_HSTRING_HSTRING IMap_HSTRING_HSTRING_iface;
    IIterable_IKeyValuePair_HSTRING_HSTRING IIterable_IKeyValuePair_HSTRING_HSTRING_iface;
    LONG ref;

    SRWLOCK lock;
    struct qualifier_entry *entries;
    UINT32 count;
    UINT32 capacity;
};

static inline struct qualifier_map *impl_from_IObservableMap_HSTRING_HSTRING( IObservableMap_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct qualifier_map, IObservableMap_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI qualifier_map_QueryInterface( IObservableMap_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    struct qualifier_map *impl = impl_from_IObservableMap_HSTRING_HSTRING( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IObservableMap_HSTRING_HSTRING ))
        *out = &impl->IObservableMap_HSTRING_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IMap_HSTRING_HSTRING ))
        *out = &impl->IMap_HSTRING_HSTRING_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_HSTRING ))
        *out = &impl->IIterable_IKeyValuePair_HSTRING_HSTRING_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI qualifier_map_AddRef( IObservableMap_HSTRING_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IObservableMap_HSTRING_HSTRING( iface )->ref );
}

static ULONG WINAPI qualifier_map_Release( IObservableMap_HSTRING_HSTRING *iface )
{
    struct qualifier_map *impl = impl_from_IObservableMap_HSTRING_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++)
        {
            WindowsDeleteString( impl->entries[i].key );
            WindowsDeleteString( impl->entries[i].value );
        }
        free( impl->entries );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI qualifier_map_GetIids( IObservableMap_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI qualifier_map_GetRuntimeClassName( IObservableMap_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.ApplicationModel.Resources.Core.ResourceQualifierObservableMap";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI qualifier_map_GetTrustLevel( IObservableMap_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI qualifier_map_add_MapChanged( IObservableMap_HSTRING_HSTRING *iface,
        IMapChangedEventHandler_HSTRING_HSTRING *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p semi-stub!\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 1;
    return S_OK;
}

static HRESULT WINAPI qualifier_map_remove_MapChanged( IObservableMap_HSTRING_HSTRING *iface, EventRegistrationToken token )
{
    FIXME( "iface %p, token %#I64x semi-stub!\n", iface, token.value );
    return S_OK;
}

static const struct IObservableMap_HSTRING_HSTRINGVtbl qualifier_map_vtbl =
{
    qualifier_map_QueryInterface,
    qualifier_map_AddRef,
    qualifier_map_Release,
    /* IInspectable methods */
    qualifier_map_GetIids,
    qualifier_map_GetRuntimeClassName,
    qualifier_map_GetTrustLevel,
    /* IObservableMap<HSTRING, HSTRING> methods */
    qualifier_map_add_MapChanged,
    qualifier_map_remove_MapChanged,
};

/* the entry of a key (the names of qualifiers do not depend on case), or -1 */
static int qualifier_map_find( struct qualifier_map *impl, const WCHAR *key, UINT32 length )
{
    UINT32 i, n;
    const WCHAR *k;

    for (i = 0; i < impl->count; i++)
    {
        k = WindowsGetStringRawBuffer( impl->entries[i].key, &n );
        if (n == length && CompareStringOrdinal( k, n, key, length, TRUE ) == CSTR_EQUAL) return i;
    }
    return -1;
}

/* a copy of a value, NULL when the key is not there */
static WCHAR *qualifier_map_get( struct qualifier_map *impl, const WCHAR *key )
{
    WCHAR *value = NULL;
    int i;

    AcquireSRWLockShared( &impl->lock );
    if ((i = qualifier_map_find( impl, key, wcslen( key ) )) >= 0)
        value = wcsdup( WindowsGetStringRawBuffer( impl->entries[i].value, NULL ) );
    ReleaseSRWLockShared( &impl->lock );
    return value;
}

static HRESULT qualifier_map_set( struct qualifier_map *impl, HSTRING key, HSTRING value, boolean *replaced )
{
    const WCHAR *k;
    HSTRING copy;
    UINT32 n;
    HRESULT hr;
    int i;

    k = WindowsGetStringRawBuffer( key, &n );
    if (!n) return E_INVALIDARG;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    AcquireSRWLockExclusive( &impl->lock );
    if ((i = qualifier_map_find( impl, k, n )) >= 0)
    {
        WindowsDeleteString( impl->entries[i].value );
        impl->entries[i].value = copy;
        copy = NULL;
        if (replaced) *replaced = TRUE;
    }
    else
    {
        if (impl->count == impl->capacity)
        {
            struct qualifier_entry *entries;
            UINT32 capacity = impl->capacity ? impl->capacity * 2 : 16;
            if (!(entries = realloc( impl->entries, capacity * sizeof(*entries) ))) hr = E_OUTOFMEMORY;
            else impl->entries = entries, impl->capacity = capacity;
        }
        if (SUCCEEDED(hr) && SUCCEEDED(hr = WindowsDuplicateString( key, &impl->entries[impl->count].key )))
        {
            impl->entries[impl->count++].value = copy;
            copy = NULL;
        }
        if (replaced) *replaced = FALSE;
    }
    ReleaseSRWLockExclusive( &impl->lock );
    WindowsDeleteString( copy );
    return hr;
}

static HRESULT qualifier_map_set_text( struct qualifier_map *impl, const WCHAR *key, const WCHAR *value )
{
    HSTRING k, v;
    HRESULT hr;

    if (FAILED(hr = WindowsCreateString( key, wcslen( key ), &k ))) return hr;
    if (SUCCEEDED(hr = WindowsCreateString( value, wcslen( value ), &v )))
    {
        hr = qualifier_map_set( impl, k, v, NULL );
        WindowsDeleteString( v );
    }
    WindowsDeleteString( k );
    return hr;
}

static void qualifier_map_clear( struct qualifier_map *impl )
{
    UINT32 i;

    AcquireSRWLockExclusive( &impl->lock );
    for (i = 0; i < impl->count; i++)
    {
        WindowsDeleteString( impl->entries[i].key );
        WindowsDeleteString( impl->entries[i].value );
    }
    impl->count = 0;
    ReleaseSRWLockExclusive( &impl->lock );
}

DEFINE_IINSPECTABLE( qualifier_imap, IMap_HSTRING_HSTRING, struct qualifier_map, IObservableMap_HSTRING_HSTRING_iface )

static HRESULT WINAPI qualifier_imap_Lookup( IMap_HSTRING_HSTRING *iface, HSTRING key, HSTRING *value )
{
    struct qualifier_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    const WCHAR *k;
    HRESULT hr = E_BOUNDS;
    UINT32 n;
    int i;

    TRACE( "iface %p, key %s, value %p.\n", iface, debugstr_hstring( key ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    k = WindowsGetStringRawBuffer( key, &n );
    AcquireSRWLockShared( &impl->lock );
    if ((i = qualifier_map_find( impl, k, n )) >= 0) hr = WindowsDuplicateString( impl->entries[i].value, value );
    ReleaseSRWLockShared( &impl->lock );
    return hr;
}

static HRESULT WINAPI qualifier_imap_get_Size( IMap_HSTRING_HSTRING *iface, UINT32 *size )
{
    struct qualifier_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );

    if (!size) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    *size = impl->count;
    ReleaseSRWLockShared( &impl->lock );
    return S_OK;
}

static HRESULT WINAPI qualifier_imap_HasKey( IMap_HSTRING_HSTRING *iface, HSTRING key, boolean *exists )
{
    struct qualifier_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    const WCHAR *k;
    UINT32 n;

    if (!exists) return E_POINTER;
    k = WindowsGetStringRawBuffer( key, &n );
    AcquireSRWLockShared( &impl->lock );
    *exists = qualifier_map_find( impl, k, n ) >= 0;
    ReleaseSRWLockShared( &impl->lock );
    return S_OK;
}

static HRESULT WINAPI qualifier_imap_GetView( IMap_HSTRING_HSTRING *iface, IMapView_HSTRING_HSTRING **view )
{
    FIXME( "iface %p, view %p stub!\n", iface, view );
    if (!view) return E_POINTER;
    *view = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI qualifier_imap_Insert( IMap_HSTRING_HSTRING *iface, HSTRING key, HSTRING value, boolean *replaced )
{
    struct qualifier_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );

    TRACE( "iface %p, key %s, value %s, replaced %p.\n", iface, debugstr_hstring( key ), debugstr_hstring( value ), replaced );
    return qualifier_map_set( impl, key, value, replaced );
}

static HRESULT WINAPI qualifier_imap_Remove( IMap_HSTRING_HSTRING *iface, HSTRING key )
{
    struct qualifier_map *impl = impl_from_IMap_HSTRING_HSTRING( iface );
    HRESULT hr = E_BOUNDS;
    const WCHAR *k;
    UINT32 n;
    int i;

    k = WindowsGetStringRawBuffer( key, &n );
    AcquireSRWLockExclusive( &impl->lock );
    if ((i = qualifier_map_find( impl, k, n )) >= 0)
    {
        WindowsDeleteString( impl->entries[i].key );
        WindowsDeleteString( impl->entries[i].value );
        memmove( impl->entries + i, impl->entries + i + 1, (impl->count - i - 1) * sizeof(*impl->entries) );
        impl->count--;
        hr = S_OK;
    }
    ReleaseSRWLockExclusive( &impl->lock );
    return hr;
}

static HRESULT WINAPI qualifier_imap_Clear( IMap_HSTRING_HSTRING *iface )
{
    qualifier_map_clear( impl_from_IMap_HSTRING_HSTRING( iface ) );
    return S_OK;
}

static const struct IMap_HSTRING_HSTRINGVtbl qualifier_imap_vtbl =
{
    qualifier_imap_QueryInterface,
    qualifier_imap_AddRef,
    qualifier_imap_Release,
    /* IInspectable methods */
    qualifier_imap_GetIids,
    qualifier_imap_GetRuntimeClassName,
    qualifier_imap_GetTrustLevel,
    /* IMap<HSTRING, HSTRING> methods */
    qualifier_imap_Lookup,
    qualifier_imap_get_Size,
    qualifier_imap_HasKey,
    qualifier_imap_GetView,
    qualifier_imap_Insert,
    qualifier_imap_Remove,
    qualifier_imap_Clear,
};

/* a pair of the map (a copy) */
struct qualifier_pair
{
    IKeyValuePair_HSTRING_HSTRING IKeyValuePair_HSTRING_HSTRING_iface;
    LONG ref;
    HSTRING key;
    HSTRING value;
};

static inline struct qualifier_pair *impl_from_IKeyValuePair_HSTRING_HSTRING( IKeyValuePair_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct qualifier_pair, IKeyValuePair_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI qualifier_pair_QueryInterface( IKeyValuePair_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IKeyValuePair_HSTRING_HSTRING ))
    {
        *out = iface;
        IKeyValuePair_HSTRING_HSTRING_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI qualifier_pair_AddRef( IKeyValuePair_HSTRING_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IKeyValuePair_HSTRING_HSTRING( iface )->ref );
}

static ULONG WINAPI qualifier_pair_Release( IKeyValuePair_HSTRING_HSTRING *iface )
{
    struct qualifier_pair *impl = impl_from_IKeyValuePair_HSTRING_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->key );
        WindowsDeleteString( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI qualifier_pair_GetIids( IKeyValuePair_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI qualifier_pair_GetRuntimeClassName( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI qualifier_pair_GetTrustLevel( IKeyValuePair_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI qualifier_pair_get_Key( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *key )
{
    if (!key) return E_POINTER;
    return WindowsDuplicateString( impl_from_IKeyValuePair_HSTRING_HSTRING( iface )->key, key );
}

static HRESULT WINAPI qualifier_pair_get_Value( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IKeyValuePair_HSTRING_HSTRING( iface )->value, value );
}

static const struct IKeyValuePair_HSTRING_HSTRINGVtbl qualifier_pair_vtbl =
{
    qualifier_pair_QueryInterface,
    qualifier_pair_AddRef,
    qualifier_pair_Release,
    /* IInspectable methods */
    qualifier_pair_GetIids,
    qualifier_pair_GetRuntimeClassName,
    qualifier_pair_GetTrustLevel,
    /* IKeyValuePair<HSTRING, HSTRING> methods */
    qualifier_pair_get_Key,
    qualifier_pair_get_Value,
};

/* an iterator over a copy of the pairs taken when it was made */
struct qualifier_iterator
{
    IIterator_IKeyValuePair_HSTRING_HSTRING IIterator_IKeyValuePair_HSTRING_HSTRING_iface;
    LONG ref;
    IKeyValuePair_HSTRING_HSTRING **pairs;
    UINT32 count;
    UINT32 index;
};

static inline struct qualifier_iterator *impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( IIterator_IKeyValuePair_HSTRING_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct qualifier_iterator, IIterator_IKeyValuePair_HSTRING_HSTRING_iface );
}

static HRESULT WINAPI qualifier_iterator_QueryInterface( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_IKeyValuePair_HSTRING_HSTRING ))
    {
        *out = iface;
        IIterator_IKeyValuePair_HSTRING_HSTRING_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI qualifier_iterator_AddRef( IIterator_IKeyValuePair_HSTRING_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface )->ref );
}

static ULONG WINAPI qualifier_iterator_Release( IIterator_IKeyValuePair_HSTRING_HSTRING *iface )
{
    struct qualifier_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IKeyValuePair_HSTRING_HSTRING_Release( impl->pairs[i] );
        free( impl->pairs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI qualifier_iterator_GetIids( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI qualifier_iterator_GetRuntimeClassName( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *class_name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI qualifier_iterator_GetTrustLevel( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI qualifier_iterator_get_Current( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, IKeyValuePair_HSTRING_HSTRING **value )
{
    struct qualifier_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );

    if (!value) return E_POINTER;
    *value = NULL;
    if (impl->index >= impl->count) return E_BOUNDS;
    IKeyValuePair_HSTRING_HSTRING_AddRef( (*value = impl->pairs[impl->index]) );
    return S_OK;
}

static HRESULT WINAPI qualifier_iterator_get_HasCurrent( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, boolean *value )
{
    struct qualifier_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );

    if (!value) return E_POINTER;
    *value = impl->index < impl->count;
    return S_OK;
}

static HRESULT WINAPI qualifier_iterator_MoveNext( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, boolean *value )
{
    struct qualifier_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );

    if (!value) return E_POINTER;
    if (impl->index < impl->count) impl->index++;
    *value = impl->index < impl->count;
    return S_OK;
}

static HRESULT WINAPI qualifier_iterator_GetMany( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, UINT32 size,
                                                  IKeyValuePair_HSTRING_HSTRING **items, UINT32 *count )
{
    struct qualifier_iterator *impl = impl_from_IIterator_IKeyValuePair_HSTRING_HSTRING( iface );
    UINT32 i;

    if (!count || (size && !items)) return E_POINTER;
    for (i = 0; i < size && impl->index < impl->count; i++, impl->index++)
        IKeyValuePair_HSTRING_HSTRING_AddRef( (items[i] = impl->pairs[impl->index]) );
    *count = i;
    return S_OK;
}

static const struct IIterator_IKeyValuePair_HSTRING_HSTRINGVtbl qualifier_iterator_vtbl =
{
    qualifier_iterator_QueryInterface,
    qualifier_iterator_AddRef,
    qualifier_iterator_Release,
    /* IInspectable methods */
    qualifier_iterator_GetIids,
    qualifier_iterator_GetRuntimeClassName,
    qualifier_iterator_GetTrustLevel,
    /* IIterator<IKeyValuePair<HSTRING, HSTRING>*> methods */
    qualifier_iterator_get_Current,
    qualifier_iterator_get_HasCurrent,
    qualifier_iterator_MoveNext,
    qualifier_iterator_GetMany,
};

DEFINE_IINSPECTABLE( qualifier_iterable, IIterable_IKeyValuePair_HSTRING_HSTRING, struct qualifier_map, IObservableMap_HSTRING_HSTRING_iface )

static HRESULT WINAPI qualifier_iterable_First( IIterable_IKeyValuePair_HSTRING_HSTRING *iface,
                                                IIterator_IKeyValuePair_HSTRING_HSTRING **value )
{
    struct qualifier_map *impl = impl_from_IIterable_IKeyValuePair_HSTRING_HSTRING( iface );
    struct qualifier_iterator *iterator;
    HRESULT hr = S_OK;
    UINT32 i;

    if (!value) return E_POINTER;
    *value = NULL;
    if (!(iterator = calloc( 1, sizeof(*iterator) ))) return E_OUTOFMEMORY;
    iterator->IIterator_IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &qualifier_iterator_vtbl;
    iterator->ref = 1;

    AcquireSRWLockShared( &impl->lock );
    if (impl->count && !(iterator->pairs = calloc( impl->count, sizeof(*iterator->pairs) ))) hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED(hr) && i < impl->count; i++)
    {
        struct qualifier_pair *pair;

        if (!(pair = calloc( 1, sizeof(*pair) ))) { hr = E_OUTOFMEMORY; break; }
        pair->IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &qualifier_pair_vtbl;
        pair->ref = 1;
        WindowsDuplicateString( impl->entries[i].key, &pair->key );
        WindowsDuplicateString( impl->entries[i].value, &pair->value );
        iterator->pairs[iterator->count++] = &pair->IKeyValuePair_HSTRING_HSTRING_iface;
    }
    ReleaseSRWLockShared( &impl->lock );

    if (FAILED(hr))
    {
        IIterator_IKeyValuePair_HSTRING_HSTRING_Release( &iterator->IIterator_IKeyValuePair_HSTRING_HSTRING_iface );
        return hr;
    }
    *value = &iterator->IIterator_IKeyValuePair_HSTRING_HSTRING_iface;
    return S_OK;
}

static const struct IIterable_IKeyValuePair_HSTRING_HSTRINGVtbl qualifier_iterable_vtbl =
{
    qualifier_iterable_QueryInterface,
    qualifier_iterable_AddRef,
    qualifier_iterable_Release,
    /* IInspectable methods */
    qualifier_iterable_GetIids,
    qualifier_iterable_GetRuntimeClassName,
    qualifier_iterable_GetTrustLevel,
    /* IIterable<IKeyValuePair<HSTRING, HSTRING>*> methods */
    qualifier_iterable_First,
};

/* the values a context starts with, from the settings of the user */
static void qualifier_map_set_defaults( struct qualifier_map *impl )
{
    WCHAR languages[512] = {0}, region[16] = {0}, *p;
    ULONG count = 0, size = ARRAY_SIZE(languages);
    DWORD light = 1, light_size = sizeof(light), layout = 0;
    unsigned int i;

    if (!GetUserPreferredUILanguages( MUI_LANGUAGE_NAME, &count, languages, &size )) wcscpy( languages, L"en-US" );
    /* a list of names, each ended by a zero: one string with semicolons */
    for (p = languages; *p || p[1];) { p += wcslen( p ); if (p[1]) *p = ';'; }
    if (!GetUserDefaultGeoName( region, ARRAY_SIZE(region) )) wcscpy( region, L"US" );
    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                  RRF_RT_REG_DWORD, NULL, &light, &light_size );
    GetLocaleInfoEx( LOCALE_NAME_USER_DEFAULT, LOCALE_IREADINGLAYOUT | LOCALE_RETURN_NUMBER, (WCHAR *)&layout, sizeof(layout) / sizeof(WCHAR) );

    for (i = 0; i < PRI_QUALIFIER_TYPES; i++)
    {
        const WCHAR *value = L"";

        switch (i)
        {
        case 0: value = languages; break;
        case 1: value = L"standard"; break;
        case 3: value = region; break;
        case 5: value = layout == 1 ? L"RTL" : L"LTR"; break;
        case 6: value = light ? L"light" : L"dark"; break;
        case 10: value = L"Desktop"; break;
        }
        qualifier_map_set_text( impl, pri_qualifier_names[i], value );
    }
}

static HRESULT qualifier_map_create( struct qualifier_map **out )
{
    struct qualifier_map *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IObservableMap_HSTRING_HSTRING_iface.lpVtbl = &qualifier_map_vtbl;
    impl->IMap_HSTRING_HSTRING_iface.lpVtbl = &qualifier_imap_vtbl;
    impl->IIterable_IKeyValuePair_HSTRING_HSTRING_iface.lpVtbl = &qualifier_iterable_vtbl;
    impl->ref = 1;
    InitializeSRWLock( &impl->lock );
    *out = impl;
    return S_OK;
}

/* ---- ResourceContext ---------------------------------------------------------------------------------------- */

struct resource_context
{
    IResourceContext IResourceContext_iface;
    LONG ref;
    struct qualifier_map *values;
};

static inline struct resource_context *impl_from_IResourceContext( IResourceContext *iface )
{
    return CONTAINING_RECORD( iface, struct resource_context, IResourceContext_iface );
}

static HRESULT resource_context_create( BOOL defaults, IResourceContext **out );

/* the values of a context by qualifier type, for pri_order (free with context_values_free) */
static void context_values( IResourceContext *iface, WCHAR **values )
{
    struct resource_context *impl = impl_from_IResourceContext( iface );
    unsigned int i;

    for (i = 0; i < PRI_QUALIFIER_TYPES; i++) values[i] = qualifier_map_get( impl->values, pri_qualifier_names[i] );
}

static void context_values_free( WCHAR **values )
{
    unsigned int i;
    for (i = 0; i < PRI_QUALIFIER_TYPES; i++) free( values[i] );
}

static HRESULT WINAPI resource_context_QueryInterface( IResourceContext *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IResourceContext ))
    {
        *out = iface;
        IResourceContext_AddRef( iface );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI resource_context_AddRef( IResourceContext *iface )
{
    return InterlockedIncrement( &impl_from_IResourceContext( iface )->ref );
}

static ULONG WINAPI resource_context_Release( IResourceContext *iface )
{
    struct resource_context *impl = impl_from_IResourceContext( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IObservableMap_HSTRING_HSTRING_Release( &impl->values->IObservableMap_HSTRING_HSTRING_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI resource_context_GetIids( IResourceContext *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_context_GetRuntimeClassName( IResourceContext *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceContext,
                                wcslen( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceContext ), class_name );
}

static HRESULT WINAPI resource_context_GetTrustLevel( IResourceContext *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI resource_context_get_QualifierValues( IResourceContext *iface, IObservableMap_HSTRING_HSTRING **value )
{
    struct resource_context *impl = impl_from_IResourceContext( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IObservableMap_HSTRING_HSTRING_AddRef( (*value = &impl->values->IObservableMap_HSTRING_HSTRING_iface) );
    return S_OK;
}

static HRESULT WINAPI resource_context_Reset( IResourceContext *iface )
{
    struct resource_context *impl = impl_from_IResourceContext( iface );

    TRACE( "iface %p.\n", iface );

    qualifier_map_clear( impl->values );
    qualifier_map_set_defaults( impl->values );
    return S_OK;
}

static HRESULT WINAPI resource_context_ResetQualifierValues( IResourceContext *iface, IIterable_HSTRING *names )
{
    FIXME( "iface %p, names %p: resets every value\n", iface, names );
    return resource_context_Reset( iface );
}

static HRESULT WINAPI resource_context_OverrideToMatch( IResourceContext *iface, IIterable_ResourceQualifier *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_context_Clone( IResourceContext *iface, IResourceContext **clone )
{
    struct resource_context *impl = impl_from_IResourceContext( iface ), *copy;
    HRESULT hr;
    UINT32 i;

    TRACE( "iface %p, clone %p.\n", iface, clone );

    if (!clone) return E_POINTER;
    if (FAILED(hr = resource_context_create( FALSE, clone ))) return hr;
    copy = impl_from_IResourceContext( *clone );
    AcquireSRWLockShared( &impl->values->lock );
    for (i = 0; i < impl->values->count && SUCCEEDED(hr); i++)
        hr = qualifier_map_set( copy->values, impl->values->entries[i].key, impl->values->entries[i].value, NULL );
    ReleaseSRWLockShared( &impl->values->lock );
    return hr;
}

static HRESULT WINAPI resource_context_get_Languages( IResourceContext *iface, IVectorView_HSTRING **value )
{
    struct resource_context *impl = impl_from_IResourceContext( iface );
    WCHAR *languages, *p, *end;
    IVector_HSTRING *vector;
    HSTRING string;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = vector_hstring_create( &vector ))) return hr;
    if ((languages = qualifier_map_get( impl->values, L"Language" )))
    {
        for (p = languages; *p && SUCCEEDED(hr); p = *end ? end + 1 : end)
        {
            for (end = p; *end && *end != ';' && *end != ','; end++);
            if (end == p) continue;
            if (SUCCEEDED(hr = WindowsCreateString( p, end - p, &string )))
            {
                hr = IVector_HSTRING_Append( vector, string );
                WindowsDeleteString( string );
            }
        }
        free( languages );
    }
    if (SUCCEEDED(hr)) hr = IVector_HSTRING_GetView( vector, value );
    IVector_HSTRING_Release( vector );
    return hr;
}

static HRESULT WINAPI resource_context_put_Languages( IResourceContext *iface, IVectorView_HSTRING *languages )
{
    struct resource_context *impl = impl_from_IResourceContext( iface );
    WCHAR text[1024] = {0};
    UINT32 count = 0, i, n;
    HSTRING string;
    HRESULT hr;

    TRACE( "iface %p, languages %p.\n", iface, languages );

    if (!languages) return E_POINTER;
    if (FAILED(hr = IVectorView_HSTRING_get_Size( languages, &count ))) return hr;
    for (i = 0; i < count; i++)
    {
        const WCHAR *buffer;

        if (FAILED(hr = IVectorView_HSTRING_GetAt( languages, i, &string ))) return hr;
        buffer = WindowsGetStringRawBuffer( string, &n );
        if (wcslen( text ) + n + 2 < ARRAY_SIZE(text))
        {
            if (i) wcscat( text, L";" );
            wcsncat( text, buffer, n );
        }
        WindowsDeleteString( string );
    }
    return qualifier_map_set_text( impl->values, L"Language", text );
}

static const struct IResourceContextVtbl resource_context_vtbl =
{
    resource_context_QueryInterface,
    resource_context_AddRef,
    resource_context_Release,
    /* IInspectable methods */
    resource_context_GetIids,
    resource_context_GetRuntimeClassName,
    resource_context_GetTrustLevel,
    /* IResourceContext methods */
    resource_context_get_QualifierValues,
    resource_context_Reset,
    resource_context_ResetQualifierValues,
    resource_context_OverrideToMatch,
    resource_context_Clone,
    resource_context_get_Languages,
    resource_context_put_Languages,
};

static HRESULT resource_context_create( BOOL defaults, IResourceContext **out )
{
    struct resource_context *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IResourceContext_iface.lpVtbl = &resource_context_vtbl;
    impl->ref = 1;
    if (FAILED(hr = qualifier_map_create( &impl->values )))
    {
        free( impl );
        return hr;
    }
    if (defaults) qualifier_map_set_defaults( impl->values );
    *out = &impl->IResourceContext_iface;
    return S_OK;
}

/* the context of GetForViewIndependentUse (one for the process) and the default context of the manager */
static IResourceContext *shared_contexts[2];
static INIT_ONCE shared_contexts_once = INIT_ONCE_STATIC_INIT;

static BOOL WINAPI create_shared_contexts( INIT_ONCE *once, void *param, void **context )
{
    resource_context_create( TRUE, &shared_contexts[0] );
    resource_context_create( TRUE, &shared_contexts[1] );
    return TRUE;
}

static HRESULT get_shared_context( unsigned int which, IResourceContext **out )
{
    InitOnceExecuteOnce( &shared_contexts_once, create_shared_contexts, NULL, NULL );
    if (!(*out = shared_contexts[which])) return E_OUTOFMEMORY;
    IResourceContext_AddRef( *out );
    return S_OK;
}
#define VIEW_INDEPENDENT_CONTEXT 0
#define MANAGER_CONTEXT 1

/* ---- ResourceQualifier -------------------------------------------------------------------------------------- */

struct resource_qualifier
{
    IResourceQualifier IResourceQualifier_iface;
    LONG ref;
    const struct pri_qualifier *qualifier;
    BOOL match;
};

static inline struct resource_qualifier *impl_from_IResourceQualifier( IResourceQualifier *iface )
{
    return CONTAINING_RECORD( iface, struct resource_qualifier, IResourceQualifier_iface );
}

static HRESULT WINAPI resource_qualifier_QueryInterface( IResourceQualifier *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IResourceQualifier ))
    {
        *out = iface;
        IResourceQualifier_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI resource_qualifier_AddRef( IResourceQualifier *iface )
{
    return InterlockedIncrement( &impl_from_IResourceQualifier( iface )->ref );
}

static ULONG WINAPI resource_qualifier_Release( IResourceQualifier *iface )
{
    struct resource_qualifier *impl = impl_from_IResourceQualifier( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI resource_qualifier_GetIids( IResourceQualifier *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_qualifier_GetRuntimeClassName( IResourceQualifier *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceQualifier,
                                wcslen( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceQualifier ), class_name );
}

static HRESULT WINAPI resource_qualifier_GetTrustLevel( IResourceQualifier *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI resource_qualifier_get_QualifierName( IResourceQualifier *iface, HSTRING *value )
{
    const WCHAR *name = pri_qualifier_names[impl_from_IResourceQualifier( iface )->qualifier->type];
    if (!value) return E_POINTER;
    return WindowsCreateString( name, wcslen( name ), value );
}

static HRESULT WINAPI resource_qualifier_get_QualifierValue( IResourceQualifier *iface, HSTRING *value )
{
    const WCHAR *text = impl_from_IResourceQualifier( iface )->qualifier->value;
    if (!value) return E_POINTER;
    return WindowsCreateString( text, wcslen( text ), value );
}

static HRESULT WINAPI resource_qualifier_get_IsDefault( IResourceQualifier *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IResourceQualifier( iface )->qualifier->fallback >= 1000;
    return S_OK;
}

static HRESULT WINAPI resource_qualifier_get_IsMatch( IResourceQualifier *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IResourceQualifier( iface )->match;
    return S_OK;
}

static HRESULT WINAPI resource_qualifier_get_Score( IResourceQualifier *iface, DOUBLE *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IResourceQualifier( iface )->match ? 1.0 : 0.0;
    return S_OK;
}

static const struct IResourceQualifierVtbl resource_qualifier_vtbl =
{
    resource_qualifier_QueryInterface,
    resource_qualifier_AddRef,
    resource_qualifier_Release,
    /* IInspectable methods */
    resource_qualifier_GetIids,
    resource_qualifier_GetRuntimeClassName,
    resource_qualifier_GetTrustLevel,
    /* IResourceQualifier methods */
    resource_qualifier_get_QualifierName,
    resource_qualifier_get_QualifierValue,
    resource_qualifier_get_IsDefault,
    resource_qualifier_get_IsMatch,
    resource_qualifier_get_Score,
};

/* ---- ResourceCandidate -------------------------------------------------------------------------------------- */

struct resource_candidate
{
    IResourceCandidate IResourceCandidate_iface;
    IResourceCandidate2 IResourceCandidate2_iface;
    IResourceCandidate3 IResourceCandidate3_iface;
    LONG ref;
    const struct pri *pri;
    const struct pri_candidate *candidate;
    BOOL match;
};

static inline struct resource_candidate *impl_from_IResourceCandidate( IResourceCandidate *iface )
{
    return CONTAINING_RECORD( iface, struct resource_candidate, IResourceCandidate_iface );
}

static HRESULT WINAPI resource_candidate_QueryInterface( IResourceCandidate *iface, REFIID iid, void **out )
{
    struct resource_candidate *impl = impl_from_IResourceCandidate( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IResourceCandidate ))
        *out = &impl->IResourceCandidate_iface;
    else if (IsEqualGUID( iid, &IID_IResourceCandidate2 ))
        *out = &impl->IResourceCandidate2_iface;
    else if (IsEqualGUID( iid, &IID_IResourceCandidate3 ))
        *out = &impl->IResourceCandidate3_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI resource_candidate_AddRef( IResourceCandidate *iface )
{
    return InterlockedIncrement( &impl_from_IResourceCandidate( iface )->ref );
}

static ULONG WINAPI resource_candidate_Release( IResourceCandidate *iface )
{
    struct resource_candidate *impl = impl_from_IResourceCandidate( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI resource_candidate_GetIids( IResourceCandidate *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_candidate_GetRuntimeClassName( IResourceCandidate *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceCandidate,
                                wcslen( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceCandidate ), class_name );
}

static HRESULT WINAPI resource_candidate_GetTrustLevel( IResourceCandidate *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static const struct vector_iids qualifier_vector_iids =
{
    .iterable = &IID_IIterable_ResourceQualifier,
    .iterator = &IID_IIterator_ResourceQualifier,
    .vector = &IID_IVectorView_ResourceQualifier,
    .view = &IID_IVectorView_ResourceQualifier,
};

static HRESULT WINAPI resource_candidate_get_Qualifiers( IResourceCandidate *iface, IVectorView_ResourceQualifier **value )
{
    struct resource_candidate *impl = impl_from_IResourceCandidate( iface );
    IVector_IInspectable *vector;
    HRESULT hr;
    UINT32 i;

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = vector_inspectable_create( &qualifier_vector_iids, &vector ))) return hr;
    for (i = 0; i < impl->candidate->qualifier_count && SUCCEEDED(hr); i++)
    {
        struct resource_qualifier *qualifier;

        if (!(qualifier = calloc( 1, sizeof(*qualifier) ))) { hr = E_OUTOFMEMORY; break; }
        qualifier->IResourceQualifier_iface.lpVtbl = &resource_qualifier_vtbl;
        qualifier->ref = 1;
        qualifier->qualifier = &impl->pri->qualifiers[impl->candidate->qualifiers[i]];
        qualifier->match = impl->match;
        hr = IVector_IInspectable_Append( vector, (IInspectable *)&qualifier->IResourceQualifier_iface );
        IResourceQualifier_Release( &qualifier->IResourceQualifier_iface );
    }
    if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( vector );
    return hr;
}

static HRESULT WINAPI resource_candidate_get_IsMatch( IResourceCandidate *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IResourceCandidate( iface )->match;
    return S_OK;
}

static HRESULT WINAPI resource_candidate_get_IsMatchAsDefault( IResourceCandidate *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI resource_candidate_get_IsDefault( IResourceCandidate *iface, boolean *value )
{
    struct resource_candidate *impl = impl_from_IResourceCandidate( iface );
    UINT32 i;

    if (!value) return E_POINTER;
    *value = TRUE;
    for (i = 0; i < impl->candidate->qualifier_count; i++)
        if (impl->pri->qualifiers[impl->candidate->qualifiers[i]].fallback < 1000) *value = FALSE;
    return S_OK;
}

/* the text of a string, or the full path of a file */
static HRESULT candidate_text( struct resource_candidate *impl, HSTRING *value )
{
    WCHAR *path;
    size_t length;
    HRESULT hr;

    if (impl->candidate->kind != PRI_KIND_FILE)
        return WindowsCreateString( impl->candidate->value, wcslen( impl->candidate->value ), value );
    length = wcslen( impl->pri->root ) + wcslen( impl->candidate->value ) + 2;
    if (!(path = malloc( length * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    swprintf( path, length, L"%s\\%s", impl->pri->root, impl->candidate->value );
    hr = WindowsCreateString( path, wcslen( path ), value );
    free( path );
    return hr;
}

static HRESULT WINAPI resource_candidate_get_ValueAsString( IResourceCandidate *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return candidate_text( impl_from_IResourceCandidate( iface ), value );
}

/* the first method of IStorageFileStatics (5984c710-daf2-43c8-8bb4-a4d3eacfd03f), not in the headers of Wine */
static const GUID IID_storage_file_statics = {0x5984c710, 0xdaf2, 0x43c8, {0x8b, 0xb4, 0xa4, 0xd3, 0xea, 0xcf, 0xd0, 0x3f}};
typedef struct storage_file_statics { const struct storage_file_staticsVtbl *lpVtbl; } storage_file_statics;
struct storage_file_staticsVtbl
{
    IInspectableVtbl base;
    HRESULT (STDMETHODCALLTYPE *GetFileFromPathAsync)( storage_file_statics *iface, HSTRING path, IAsyncOperation_StorageFile **operation );
};

static HRESULT WINAPI resource_candidate_GetValueAsFileAsync( IResourceCandidate *iface, IAsyncOperation_StorageFile **operation )
{
    static const WCHAR *class_name = L"Windows.Storage.StorageFile";
    struct resource_candidate *impl = impl_from_IResourceCandidate( iface );
    storage_file_statics *statics;
    HSTRING_HEADER header;
    HSTRING string, path;
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (impl->candidate->kind != PRI_KIND_FILE) return E_ILLEGAL_METHOD_CALL;
    if (FAILED(hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &header, &string ))) return hr;
    if (FAILED(hr = RoGetActivationFactory( string, &IID_storage_file_statics, (void **)&statics ))) return hr;
    if (SUCCEEDED(hr = candidate_text( impl, &path )))
    {
        hr = statics->lpVtbl->GetFileFromPathAsync( statics, path, operation );
        WindowsDeleteString( path );
    }
    statics->lpVtbl->base.Release( (IInspectable *)statics );
    return hr;
}

static HRESULT WINAPI resource_candidate_GetQualifierValue( IResourceCandidate *iface, HSTRING name, HSTRING *value )
{
    struct resource_candidate *impl = impl_from_IResourceCandidate( iface );
    const WCHAR *wanted = WindowsGetStringRawBuffer( name, NULL );
    UINT32 i;

    if (!value) return E_POINTER;
    *value = NULL;
    for (i = 0; i < impl->candidate->qualifier_count; i++)
    {
        const struct pri_qualifier *q = &impl->pri->qualifiers[impl->candidate->qualifiers[i]];
        if (!wcsicmp( pri_qualifier_names[q->type], wanted )) return WindowsCreateString( q->value, wcslen( q->value ), value );
    }
    return S_OK;
}

static const struct IResourceCandidateVtbl resource_candidate_vtbl =
{
    resource_candidate_QueryInterface,
    resource_candidate_AddRef,
    resource_candidate_Release,
    /* IInspectable methods */
    resource_candidate_GetIids,
    resource_candidate_GetRuntimeClassName,
    resource_candidate_GetTrustLevel,
    /* IResourceCandidate methods */
    resource_candidate_get_Qualifiers,
    resource_candidate_get_IsMatch,
    resource_candidate_get_IsMatchAsDefault,
    resource_candidate_get_IsDefault,
    resource_candidate_get_ValueAsString,
    resource_candidate_GetValueAsFileAsync,
    resource_candidate_GetQualifierValue,
};

DEFINE_IINSPECTABLE( resource_candidate2, IResourceCandidate2, struct resource_candidate, IResourceCandidate_iface )

static HRESULT WINAPI resource_candidate2_GetValueAsStreamAsync( IResourceCandidate2 *iface, IAsyncOperation_IRandomAccessStream **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct IResourceCandidate2Vtbl resource_candidate2_vtbl =
{
    resource_candidate2_QueryInterface,
    resource_candidate2_AddRef,
    resource_candidate2_Release,
    /* IInspectable methods */
    resource_candidate2_GetIids,
    resource_candidate2_GetRuntimeClassName,
    resource_candidate2_GetTrustLevel,
    /* IResourceCandidate2 methods */
    resource_candidate2_GetValueAsStreamAsync,
};

DEFINE_IINSPECTABLE( resource_candidate3, IResourceCandidate3, struct resource_candidate, IResourceCandidate_iface )

static HRESULT WINAPI resource_candidate3_get_Kind( IResourceCandidate3 *iface, ResourceCandidateKind *value )
{
    struct resource_candidate *impl = impl_from_IResourceCandidate3( iface );

    if (!value) return E_POINTER;
    *value = impl->candidate->kind == PRI_KIND_FILE ? ResourceCandidateKind_File :
             impl->candidate->kind == PRI_KIND_EMBEDDED ? ResourceCandidateKind_EmbeddedData : ResourceCandidateKind_String;
    return S_OK;
}

static const struct IResourceCandidate3Vtbl resource_candidate3_vtbl =
{
    resource_candidate3_QueryInterface,
    resource_candidate3_AddRef,
    resource_candidate3_Release,
    /* IInspectable methods */
    resource_candidate3_GetIids,
    resource_candidate3_GetRuntimeClassName,
    resource_candidate3_GetTrustLevel,
    /* IResourceCandidate3 methods */
    resource_candidate3_get_Kind,
};

static HRESULT resource_candidate_create( const struct pri *pri, UINT32 index, BOOL match, IResourceCandidate **out )
{
    struct resource_candidate *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IResourceCandidate_iface.lpVtbl = &resource_candidate_vtbl;
    impl->IResourceCandidate2_iface.lpVtbl = &resource_candidate2_vtbl;
    impl->IResourceCandidate3_iface.lpVtbl = &resource_candidate3_vtbl;
    impl->ref = 1;
    impl->pri = pri;
    impl->candidate = &pri->candidates[index];
    impl->match = match;
    *out = &impl->IResourceCandidate_iface;
    return S_OK;
}

/* the best candidate of an item for a context (the default context of the manager without one) */
static HRESULT resolve_item( const struct pri *pri, const struct pri_item *item, IResourceContext *context, IResourceCandidate **out )
{
    WCHAR *values[PRI_QUALIFIER_TYPES];
    UINT32 *order;
    HRESULT hr;

    *out = NULL;
    if (!item->count) return S_OK;
    if (!(order = malloc( item->count * sizeof(*order) ))) return E_OUTOFMEMORY;
    if (context) IResourceContext_AddRef( context );
    else if (FAILED(hr = get_shared_context( MANAGER_CONTEXT, &context ))) { free( order ); return hr; }
    context_values( context, values );
    pri_order( pri, item, (const WCHAR *const *)values, order );
    hr = resource_candidate_create( pri, order[0], pri_candidate_matches( pri, &pri->candidates[order[0]], (const WCHAR *const *)values ), out );
    context_values_free( values );
    IResourceContext_Release( context );
    free( order );
    return hr;
}

static const struct vector_iids candidate_vector_iids =
{
    .iterable = &IID_IIterable_ResourceCandidate,
    .iterator = &IID_IIterator_ResourceCandidate,
    .vector = &IID_IVectorView_ResourceCandidate,
    .view = &IID_IVectorView_ResourceCandidate,
};

/* every candidate of an item, the best for the context first */
static HRESULT candidate_list( const struct pri *pri, const struct pri_item *item, IResourceContext *context, BOOL resolved,
                               IVectorView_ResourceCandidate **out )
{
    WCHAR *values[PRI_QUALIFIER_TYPES];
    IVector_IInspectable *vector;
    IResourceCandidate *candidate;
    UINT32 *order, i;
    HRESULT hr;

    *out = NULL;
    if (!(order = malloc( (item->count ? item->count : 1) * sizeof(*order) ))) return E_OUTOFMEMORY;
    if (context) IResourceContext_AddRef( context );
    else if (FAILED(hr = get_shared_context( MANAGER_CONTEXT, &context ))) { free( order ); return hr; }
    context_values( context, values );
    pri_order( pri, item, (const WCHAR *const *)values, order );

    if (SUCCEEDED(hr = vector_inspectable_create( &candidate_vector_iids, &vector )))
    {
        for (i = 0; i < item->count && SUCCEEDED(hr); i++)
        {
            BOOL match = resolved && pri_candidate_matches( pri, &pri->candidates[order[i]], (const WCHAR *const *)values );
            if (FAILED(hr = resource_candidate_create( pri, order[i], match, &candidate ))) break;
            hr = IVector_IInspectable_Append( vector, (IInspectable *)candidate );
            IResourceCandidate_Release( candidate );
        }
        if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)out );
        IVector_IInspectable_Release( vector );
    }
    context_values_free( values );
    IResourceContext_Release( context );
    free( order );
    return hr;
}

/* ---- NamedResource ------------------------------------------------------------------------------------------ */

struct named_resource
{
    INamedResource INamedResource_iface;
    LONG ref;
    const struct pri *pri;
    const struct pri_item *item;
};

static inline struct named_resource *impl_from_INamedResource( INamedResource *iface )
{
    return CONTAINING_RECORD( iface, struct named_resource, INamedResource_iface );
}

static HRESULT WINAPI named_resource_QueryInterface( INamedResource *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_INamedResource ))
    {
        *out = iface;
        INamedResource_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI named_resource_AddRef( INamedResource *iface )
{
    return InterlockedIncrement( &impl_from_INamedResource( iface )->ref );
}

static ULONG WINAPI named_resource_Release( INamedResource *iface )
{
    struct named_resource *impl = impl_from_INamedResource( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI named_resource_GetIids( INamedResource *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI named_resource_GetRuntimeClassName( INamedResource *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Resources_Core_NamedResource,
                                wcslen( RuntimeClass_Windows_ApplicationModel_Resources_Core_NamedResource ), class_name );
}

static HRESULT WINAPI named_resource_GetTrustLevel( INamedResource *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI named_resource_get_Uri( INamedResource *iface, IUriRuntimeClass **value )
{
    struct named_resource *impl = impl_from_INamedResource( iface );
    if (!value) return E_POINTER;
    return create_resource_uri( impl->pri, impl->item->name, value );
}

static HRESULT WINAPI named_resource_get_Candidates( INamedResource *iface, IVectorView_ResourceCandidate **value )
{
    struct named_resource *impl = impl_from_INamedResource( iface );
    if (!value) return E_POINTER;
    return candidate_list( impl->pri, impl->item, NULL, FALSE, value );
}

static HRESULT WINAPI named_resource_Resolve( INamedResource *iface, IResourceCandidate **result )
{
    struct named_resource *impl = impl_from_INamedResource( iface );
    if (!result) return E_POINTER;
    return resolve_item( impl->pri, impl->item, NULL, result );
}

static HRESULT WINAPI named_resource_ResolveForContext( INamedResource *iface, IResourceContext *context, IResourceCandidate **result )
{
    struct named_resource *impl = impl_from_INamedResource( iface );
    if (!result) return E_POINTER;
    if (!context) return E_INVALIDARG;
    return resolve_item( impl->pri, impl->item, context, result );
}

static HRESULT WINAPI named_resource_ResolveAll( INamedResource *iface, IVectorView_ResourceCandidate **result )
{
    struct named_resource *impl = impl_from_INamedResource( iface );
    if (!result) return E_POINTER;
    return candidate_list( impl->pri, impl->item, NULL, TRUE, result );
}

static HRESULT WINAPI named_resource_ResolveAllForContext( INamedResource *iface, IResourceContext *context,
                                                          IVectorView_ResourceCandidate **result )
{
    struct named_resource *impl = impl_from_INamedResource( iface );
    if (!result) return E_POINTER;
    if (!context) return E_INVALIDARG;
    return candidate_list( impl->pri, impl->item, context, TRUE, result );
}

static const struct INamedResourceVtbl named_resource_vtbl =
{
    named_resource_QueryInterface,
    named_resource_AddRef,
    named_resource_Release,
    /* IInspectable methods */
    named_resource_GetIids,
    named_resource_GetRuntimeClassName,
    named_resource_GetTrustLevel,
    /* INamedResource methods */
    named_resource_get_Uri,
    named_resource_get_Candidates,
    named_resource_Resolve,
    named_resource_ResolveForContext,
    named_resource_ResolveAll,
    named_resource_ResolveAllForContext,
};

static HRESULT named_resource_create( const struct pri *pri, const struct pri_item *item, INamedResource **out )
{
    struct named_resource *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->INamedResource_iface.lpVtbl = &named_resource_vtbl;
    impl->ref = 1;
    impl->pri = pri;
    impl->item = item;
    *out = &impl->INamedResource_iface;
    return S_OK;
}

/* ---- ResourceMap -------------------------------------------------------------------------------------------- */

/* the interface the copied map code uses for the inner map; never handed out */
static const GUID IID_named_resource_map_inner = {0x6f0ba1d7, 0x3b8e, 0x4c42, {0x9d, 0x55, 0x2a, 0x6e, 0x10, 0x8f, 0x4a, 0x31}};

static const struct map_iids named_resource_map_iids =
{
    .map = &IID_named_resource_map_inner,
    .view = &IID_IMapView_HSTRING_NamedResource,
    .iterable = &IID_IIterable_IKeyValuePair_HSTRING_NamedResource,
    .iterator = &IID_IIterator_IKeyValuePair_HSTRING_NamedResource,
    .pair = &IID_IKeyValuePair_HSTRING_NamedResource,
};

struct resource_map
{
    IResourceMap IResourceMap_iface;
    LONG ref;
    const struct pri *pri;
    WCHAR *prefix;                       /* "" or "Resources/" */
    IMapView_HSTRING_IInspectable *view; /* the named resources under the prefix, by the rest of their name */
};

static inline struct resource_map *impl_from_IResourceMap( IResourceMap *iface )
{
    return CONTAINING_RECORD( iface, struct resource_map, IResourceMap_iface );
}

static HRESULT WINAPI resource_map_QueryInterface( IResourceMap *iface, REFIID iid, void **out )
{
    struct resource_map *impl = impl_from_IResourceMap( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IResourceMap ))
    {
        *out = iface;
        IResourceMap_AddRef( iface );
        return S_OK;
    }
    /* the collection interfaces are those of the map view */
    if (IsEqualGUID( iid, &IID_IMapView_HSTRING_NamedResource ) || IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_NamedResource ))
        return IMapView_HSTRING_IInspectable_QueryInterface( impl->view, iid, out );
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI resource_map_AddRef( IResourceMap *iface )
{
    return InterlockedIncrement( &impl_from_IResourceMap( iface )->ref );
}

static ULONG WINAPI resource_map_Release( IResourceMap *iface )
{
    struct resource_map *impl = impl_from_IResourceMap( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IMapView_HSTRING_IInspectable_Release( impl->view );
        free( impl->prefix );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI resource_map_GetIids( IResourceMap *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_map_GetRuntimeClassName( IResourceMap *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceMap,
                                wcslen( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceMap ), class_name );
}

static HRESULT WINAPI resource_map_GetTrustLevel( IResourceMap *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI resource_map_get_Uri( IResourceMap *iface, IUriRuntimeClass **value )
{
    struct resource_map *impl = impl_from_IResourceMap( iface );
    if (!value) return E_POINTER;
    return create_resource_uri( impl->pri, impl->prefix, value );
}

static HRESULT map_get_value( struct resource_map *impl, HSTRING resource, IResourceContext *context, IResourceCandidate **value )
{
    const struct pri_item *item;
    WCHAR *name;

    if (!value) return E_POINTER;
    *value = NULL;
    if (!WindowsGetStringLen( resource )) return E_INVALIDARG;
    if (!(name = resource_full_name( impl->prefix, WindowsGetStringRawBuffer( resource, NULL ) ))) return E_OUTOFMEMORY;
    item = pri_find( impl->pri, name, wcslen( name ) );
    free( name );
    if (!item) return S_OK; /* Windows gives a null candidate */
    return resolve_item( impl->pri, item, context, value );
}

static HRESULT WINAPI resource_map_GetValue( IResourceMap *iface, HSTRING resource, IResourceCandidate **value )
{
    TRACE( "iface %p, resource %s, value %p.\n", iface, debugstr_hstring( resource ), value );
    return map_get_value( impl_from_IResourceMap( iface ), resource, NULL, value );
}

static HRESULT WINAPI resource_map_GetValueForContext( IResourceMap *iface, HSTRING resource, IResourceContext *context,
                                                       IResourceCandidate **value )
{
    TRACE( "iface %p, resource %s, context %p, value %p.\n", iface, debugstr_hstring( resource ), context, value );
    if (!context) return E_INVALIDARG;
    return map_get_value( impl_from_IResourceMap( iface ), resource, context, value );
}

static HRESULT resource_map_create( const struct pri *pri, const WCHAR *prefix, IResourceMap **out );

static HRESULT WINAPI resource_map_GetSubtree( IResourceMap *iface, HSTRING reference, IResourceMap **map )
{
    struct resource_map *impl = impl_from_IResourceMap( iface );
    WCHAR *name, *prefix;
    HRESULT hr = S_OK;

    TRACE( "iface %p, reference %s, map %p.\n", iface, debugstr_hstring( reference ), map );

    if (!map) return E_POINTER;
    *map = NULL;
    if (!(name = resource_full_name( impl->prefix, WindowsGetStringRawBuffer( reference, NULL ) ))) return E_OUTOFMEMORY;
    if ((prefix = malloc( (wcslen( name ) + 2) * sizeof(WCHAR) )))
    {
        wcscpy( prefix, name );
        if (*prefix && prefix[wcslen( prefix ) - 1] != '/') wcscat( prefix, L"/" );
        if (map_exists( impl->pri, prefix )) hr = resource_map_create( impl->pri, prefix, map );
        free( prefix );
    }
    else hr = E_OUTOFMEMORY;
    free( name );
    return hr;
}

static const struct IResourceMapVtbl resource_map_vtbl =
{
    resource_map_QueryInterface,
    resource_map_AddRef,
    resource_map_Release,
    /* IInspectable methods */
    resource_map_GetIids,
    resource_map_GetRuntimeClassName,
    resource_map_GetTrustLevel,
    /* IResourceMap methods */
    resource_map_get_Uri,
    resource_map_GetValue,
    resource_map_GetValueForContext,
    resource_map_GetSubtree,
};

static HRESULT resource_map_create( const struct pri *pri, const WCHAR *prefix, IResourceMap **out )
{
    size_t length = wcslen( prefix );
    IMap_HSTRING_IInspectable *map;
    struct resource_map *impl;
    IInspectable *inner;
    HRESULT hr;
    UINT32 i;

    *out = NULL;
    if (FAILED(hr = single_threaded_map_create( &named_resource_map_iids, NULL, &inner ))) return hr;
    hr = IInspectable_QueryInterface( inner, &IID_named_resource_map_inner, (void **)&map );
    IInspectable_Release( inner );
    if (FAILED(hr)) return hr;

    for (i = 0; i < pri->item_count && SUCCEEDED(hr); i++)
    {
        const WCHAR *rest = pri->items[i].name + length;
        INamedResource *named;
        boolean replaced;
        HSTRING key;

        if (wcsnicmp( pri->items[i].name, prefix, length )) continue;
        if (FAILED(hr = named_resource_create( pri, &pri->items[i], &named ))) break;
        if (SUCCEEDED(hr = WindowsCreateString( rest, wcslen( rest ), &key )))
        {
            hr = IMap_HSTRING_IInspectable_Insert( map, key, (IInspectable *)named, &replaced );
            WindowsDeleteString( key );
        }
        INamedResource_Release( named );
    }

    if (SUCCEEDED(hr) && !(impl = calloc( 1, sizeof(*impl) ))) hr = E_OUTOFMEMORY;
    if (FAILED(hr))
    {
        IMap_HSTRING_IInspectable_Release( map );
        return hr;
    }
    impl->IResourceMap_iface.lpVtbl = &resource_map_vtbl;
    impl->ref = 1;
    impl->pri = pri;
    impl->prefix = wcsdup( prefix );
    hr = IMap_HSTRING_IInspectable_GetView( map, &impl->view );
    IMap_HSTRING_IInspectable_Release( map );
    if (FAILED(hr) || !impl->prefix)
    {
        if (impl->view) IMapView_HSTRING_IInspectable_Release( impl->view );
        free( impl->prefix );
        free( impl );
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    *out = &impl->IResourceMap_iface;
    return S_OK;
}

/* ---- ResourceManager ---------------------------------------------------------------------------------------- */

static const GUID IID_resource_map_map_inner = {0x2a3c7b1e, 0x52d4, 0x4f0e, {0x8b, 0x61, 0x93, 0x0c, 0x5e, 0x77, 0x14, 0xa8}};

static const struct map_iids resource_map_map_iids =
{
    .map = &IID_resource_map_map_inner,
    .view = &IID_IMapView_HSTRING_ResourceMap,
    .iterable = &IID_IIterable_IKeyValuePair_HSTRING_ResourceMap,
    .iterator = &IID_IIterator_IKeyValuePair_HSTRING_ResourceMap,
    .pair = &IID_IKeyValuePair_HSTRING_ResourceMap,
};

struct resource_manager
{
    IResourceManager IResourceManager_iface;
    IResourceManager2 IResourceManager2_iface;
    LONG ref;
};

static inline struct resource_manager *impl_from_IResourceManager( IResourceManager *iface )
{
    return CONTAINING_RECORD( iface, struct resource_manager, IResourceManager_iface );
}

static HRESULT WINAPI resource_manager_QueryInterface( IResourceManager *iface, REFIID iid, void **out )
{
    struct resource_manager *impl = impl_from_IResourceManager( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IResourceManager ))
        *out = &impl->IResourceManager_iface;
    else if (IsEqualGUID( iid, &IID_IResourceManager2 ))
        *out = &impl->IResourceManager2_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

/* one manager for the process, never freed */
static ULONG WINAPI resource_manager_AddRef( IResourceManager *iface )
{
    return 2;
}

static ULONG WINAPI resource_manager_Release( IResourceManager *iface )
{
    return 1;
}

static HRESULT WINAPI resource_manager_GetIids( IResourceManager *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_manager_GetRuntimeClassName( IResourceManager *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceManager,
                                wcslen( RuntimeClass_Windows_ApplicationModel_Resources_Core_ResourceManager ), class_name );
}

static HRESULT WINAPI resource_manager_GetTrustLevel( IResourceManager *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI resource_manager_get_MainResourceMap( IResourceManager *iface, IResourceMap **value )
{
    struct pri *pri;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = get_pri( &pri ))) return hr;
    return resource_map_create( pri, L"", value );
}

static HRESULT WINAPI resource_manager_get_AllResourceMaps( IResourceManager *iface, IMapView_HSTRING_ResourceMap **maps )
{
    IMap_HSTRING_IInspectable *map;
    IResourceMap *main_map;
    IInspectable *inner;
    boolean replaced;
    struct pri *pri;
    HSTRING key;
    HRESULT hr;

    TRACE( "iface %p, maps %p.\n", iface, maps );

    if (!maps) return E_POINTER;
    *maps = NULL;
    if (FAILED(hr = get_pri( &pri ))) return hr;
    if (FAILED(hr = single_threaded_map_create( &resource_map_map_iids, NULL, &inner ))) return hr;
    hr = IInspectable_QueryInterface( inner, &IID_resource_map_map_inner, (void **)&map );
    IInspectable_Release( inner );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = resource_map_create( pri, L"", &main_map )))
    {
        if (SUCCEEDED(hr = WindowsCreateString( pri->name, wcslen( pri->name ), &key )))
        {
            hr = IMap_HSTRING_IInspectable_Insert( map, key, (IInspectable *)main_map, &replaced );
            WindowsDeleteString( key );
        }
        IResourceMap_Release( main_map );
    }
    if (SUCCEEDED(hr)) hr = IMap_HSTRING_IInspectable_GetView( map, (IMapView_HSTRING_IInspectable **)maps );
    IMap_HSTRING_IInspectable_Release( map );
    return hr;
}

static HRESULT WINAPI resource_manager_get_DefaultContext( IResourceManager *iface, IResourceContext **value )
{
    if (!value) return E_POINTER;
    return get_shared_context( MANAGER_CONTEXT, value );
}

static HRESULT WINAPI resource_manager_LoadPriFiles( IResourceManager *iface, IIterable_IStorageFile *files )
{
    FIXME( "iface %p, files %p stub!\n", iface, files );
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_manager_UnloadPriFiles( IResourceManager *iface, IIterable_IStorageFile *files )
{
    FIXME( "iface %p, files %p stub!\n", iface, files );
    return E_NOTIMPL;
}

static const struct IResourceManagerVtbl resource_manager_vtbl =
{
    resource_manager_QueryInterface,
    resource_manager_AddRef,
    resource_manager_Release,
    /* IInspectable methods */
    resource_manager_GetIids,
    resource_manager_GetRuntimeClassName,
    resource_manager_GetTrustLevel,
    /* IResourceManager methods */
    resource_manager_get_MainResourceMap,
    resource_manager_get_AllResourceMaps,
    resource_manager_get_DefaultContext,
    resource_manager_LoadPriFiles,
    resource_manager_UnloadPriFiles,
};

DEFINE_IINSPECTABLE( resource_manager2, IResourceManager2, struct resource_manager, IResourceManager_iface )

static HRESULT WINAPI resource_manager2_GetAllNamedResourcesForPackage( IResourceManager2 *iface, HSTRING name,
        ResourceLayoutInfo info, IVectorView_NamedResource **table )
{
    FIXME( "iface %p, name %s, table %p stub!\n", iface, debugstr_hstring( name ), table );
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_manager2_GetAllSubtreesForPackage( IResourceManager2 *iface, HSTRING name,
        ResourceLayoutInfo info, IVectorView_ResourceMap **table )
{
    FIXME( "iface %p, name %s, table %p stub!\n", iface, debugstr_hstring( name ), table );
    return E_NOTIMPL;
}

static const struct IResourceManager2Vtbl resource_manager2_vtbl =
{
    resource_manager2_QueryInterface,
    resource_manager2_AddRef,
    resource_manager2_Release,
    /* IInspectable methods */
    resource_manager2_GetIids,
    resource_manager2_GetRuntimeClassName,
    resource_manager2_GetTrustLevel,
    /* IResourceManager2 methods */
    resource_manager2_GetAllNamedResourcesForPackage,
    resource_manager2_GetAllSubtreesForPackage,
};

static struct resource_manager the_resource_manager = { {&resource_manager_vtbl}, {&resource_manager2_vtbl}, 1 };

/* ---- ResourceLoader ----------------------------------------------------------------------------------------- */

struct resource_loader
{
    IResourceLoader IResourceLoader_iface;
    IResourceLoader2 IResourceLoader2_iface;
    LONG ref;
    const struct pri *pri;
    WCHAR *prefix; /* "Resources/" */
};

static inline struct resource_loader *impl_from_IResourceLoader( IResourceLoader *iface )
{
    return CONTAINING_RECORD( iface, struct resource_loader, IResourceLoader_iface );
}

static HRESULT WINAPI resource_loader_QueryInterface( IResourceLoader *iface, REFIID iid, void **out )
{
    struct resource_loader *impl = impl_from_IResourceLoader( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IResourceLoader ))
        *out = &impl->IResourceLoader_iface;
    else if (IsEqualGUID( iid, &IID_IResourceLoader2 ))
        *out = &impl->IResourceLoader2_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI resource_loader_AddRef( IResourceLoader *iface )
{
    return InterlockedIncrement( &impl_from_IResourceLoader( iface )->ref );
}

static ULONG WINAPI resource_loader_Release( IResourceLoader *iface )
{
    struct resource_loader *impl = impl_from_IResourceLoader( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        free( impl->prefix );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI resource_loader_GetIids( IResourceLoader *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI resource_loader_GetRuntimeClassName( IResourceLoader *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Resources_ResourceLoader,
                                wcslen( RuntimeClass_Windows_ApplicationModel_Resources_ResourceLoader ), class_name );
}

static HRESULT WINAPI resource_loader_GetTrustLevel( IResourceLoader *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* the string of a resource for the view-independent context; a missing resource gives an empty string */
static HRESULT loader_string( struct resource_loader *impl, const WCHAR *resource, HSTRING *value )
{
    const struct pri_item *item;
    IResourceCandidate *candidate = NULL;
    IResourceContext *context;
    WCHAR *name;
    HRESULT hr;

    *value = NULL;
    if (!*resource) return E_INVALIDARG;
    if (!(name = resource_full_name( impl->prefix, resource ))) return E_OUTOFMEMORY;
    item = pri_find( impl->pri, name, wcslen( name ) );
    free( name );
    if (!item) return S_OK;
    if (FAILED(hr = get_shared_context( VIEW_INDEPENDENT_CONTEXT, &context ))) return hr;
    hr = resolve_item( impl->pri, item, context, &candidate );
    IResourceContext_Release( context );
    if (SUCCEEDED(hr) && candidate)
    {
        hr = IResourceCandidate_get_ValueAsString( candidate, value );
        IResourceCandidate_Release( candidate );
    }
    return hr;
}

static HRESULT WINAPI resource_loader_GetString( IResourceLoader *iface, HSTRING resource, HSTRING *value )
{
    TRACE( "iface %p, resource %s, value %p.\n", iface, debugstr_hstring( resource ), value );

    if (!value) return E_POINTER;
    return loader_string( impl_from_IResourceLoader( iface ), WindowsGetStringRawBuffer( resource, NULL ), value );
}

static const struct IResourceLoaderVtbl resource_loader_vtbl =
{
    resource_loader_QueryInterface,
    resource_loader_AddRef,
    resource_loader_Release,
    /* IInspectable methods */
    resource_loader_GetIids,
    resource_loader_GetRuntimeClassName,
    resource_loader_GetTrustLevel,
    /* IResourceLoader methods */
    resource_loader_GetString,
};

DEFINE_IINSPECTABLE( resource_loader2, IResourceLoader2, struct resource_loader, IResourceLoader_iface )

static HRESULT loader_string_for_uri( struct resource_loader *impl, IUriRuntimeClass *uri, HSTRING *value )
{
    HSTRING text;
    HRESULT hr;

    *value = NULL;
    if (!uri) return E_INVALIDARG;
    if (FAILED(hr = IUriRuntimeClass_get_RawUri( uri, &text ))) return hr;
    if (wcsnicmp( WindowsGetStringRawBuffer( text, NULL ), L"ms-resource:", 12 )) hr = E_INVALIDARG;
    else hr = loader_string( impl, WindowsGetStringRawBuffer( text, NULL ), value );
    WindowsDeleteString( text );
    return hr;
}

static HRESULT WINAPI resource_loader2_GetStringForUri( IResourceLoader2 *iface, IUriRuntimeClass *uri, HSTRING *value )
{
    TRACE( "iface %p, uri %p, value %p.\n", iface, uri, value );

    if (!value) return E_POINTER;
    return loader_string_for_uri( impl_from_IResourceLoader2( iface ), uri, value );
}

static const struct IResourceLoader2Vtbl resource_loader2_vtbl =
{
    resource_loader2_QueryInterface,
    resource_loader2_AddRef,
    resource_loader2_Release,
    /* IInspectable methods */
    resource_loader2_GetIids,
    resource_loader2_GetRuntimeClassName,
    resource_loader2_GetTrustLevel,
    /* IResourceLoader2 methods */
    resource_loader2_GetStringForUri,
};

/* a loader for a map of the main map ("Resources" without a name); a map that is not there is an error */
static HRESULT resource_loader_create( const WCHAR *map, IResourceLoader **out )
{
    struct resource_loader *impl;
    struct pri *pri;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = get_pri( &pri ))) return hr;
    if (!map || !*map) map = L"Resources";
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IResourceLoader_iface.lpVtbl = &resource_loader_vtbl;
    impl->IResourceLoader2_iface.lpVtbl = &resource_loader2_vtbl;
    impl->ref = 1;
    impl->pri = pri;
    if (!(impl->prefix = malloc( (wcslen( map ) + 2) * sizeof(WCHAR) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    wcscpy( impl->prefix, map );
    wcscat( impl->prefix, L"/" );
    if (!map_exists( pri, impl->prefix ))
    {
        IResourceLoader_Release( &impl->IResourceLoader_iface );
        return ERROR_MRM_MAP_NOT_FOUND_HR;
    }
    *out = &impl->IResourceLoader_iface;
    return S_OK;
}

/* ---- the factories ------------------------------------------------------------------------------------------ */

struct loader_statics
{
    IActivationFactory IActivationFactory_iface;
    IResourceLoaderFactory IResourceLoaderFactory_iface;
    IResourceLoaderStatics IResourceLoaderStatics_iface;
    IResourceLoaderStatics2 IResourceLoaderStatics2_iface;
    IResourceLoaderStatics3 IResourceLoaderStatics3_iface;
    IResourceLoaderStatics4 IResourceLoaderStatics4_iface;
    LONG ref;
};

static inline struct loader_statics *loader_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct loader_statics, IActivationFactory_iface );
}

static HRESULT WINAPI loader_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct loader_statics *impl = loader_statics_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IResourceLoaderFactory )) *out = &impl->IResourceLoaderFactory_iface;
    else if (IsEqualGUID( iid, &IID_IResourceLoaderStatics )) *out = &impl->IResourceLoaderStatics_iface;
    else if (IsEqualGUID( iid, &IID_IResourceLoaderStatics2 )) *out = &impl->IResourceLoaderStatics2_iface;
    else if (IsEqualGUID( iid, &IID_IResourceLoaderStatics3 )) *out = &impl->IResourceLoaderStatics3_iface;
    else if (IsEqualGUID( iid, &IID_IResourceLoaderStatics4 )) *out = &impl->IResourceLoaderStatics4_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI loader_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &loader_statics_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI loader_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &loader_statics_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI loader_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI loader_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI loader_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI loader_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!instance) return E_POINTER;
    return resource_loader_create( NULL, (IResourceLoader **)instance );
}

static const struct IActivationFactoryVtbl loader_factory_vtbl =
{
    loader_factory_QueryInterface,
    loader_factory_AddRef,
    loader_factory_Release,
    /* IInspectable methods */
    loader_factory_GetIids,
    loader_factory_GetRuntimeClassName,
    loader_factory_GetTrustLevel,
    /* IActivationFactory methods */
    loader_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( loader_by_name, IResourceLoaderFactory, struct loader_statics, IActivationFactory_iface )

static HRESULT WINAPI loader_by_name_CreateResourceLoaderByName( IResourceLoaderFactory *iface, HSTRING name, IResourceLoader **loader )
{
    TRACE( "iface %p, name %s, loader %p.\n", iface, debugstr_hstring( name ), loader );

    if (!loader) return E_POINTER;
    return resource_loader_create( WindowsGetStringRawBuffer( name, NULL ), loader );
}

static const struct IResourceLoaderFactoryVtbl loader_by_name_vtbl =
{
    loader_by_name_QueryInterface,
    loader_by_name_AddRef,
    loader_by_name_Release,
    /* IInspectable methods */
    loader_by_name_GetIids,
    loader_by_name_GetRuntimeClassName,
    loader_by_name_GetTrustLevel,
    /* IResourceLoaderFactory methods */
    loader_by_name_CreateResourceLoaderByName,
};

DEFINE_IINSPECTABLE( loader_statics1, IResourceLoaderStatics, struct loader_statics, IActivationFactory_iface )

static HRESULT WINAPI loader_statics1_GetStringForReference( IResourceLoaderStatics *iface, IUriRuntimeClass *uri, HSTRING *value )
{
    IResourceLoader *loader;
    HRESULT hr;

    TRACE( "iface %p, uri %p, value %p.\n", iface, uri, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (FAILED(hr = resource_loader_create( NULL, &loader ))) return hr;
    hr = loader_string_for_uri( impl_from_IResourceLoader( loader ), uri, value );
    IResourceLoader_Release( loader );
    return hr;
}

static const struct IResourceLoaderStaticsVtbl loader_statics1_vtbl =
{
    loader_statics1_QueryInterface,
    loader_statics1_AddRef,
    loader_statics1_Release,
    /* IInspectable methods */
    loader_statics1_GetIids,
    loader_statics1_GetRuntimeClassName,
    loader_statics1_GetTrustLevel,
    /* IResourceLoaderStatics methods */
    loader_statics1_GetStringForReference,
};

DEFINE_IINSPECTABLE( loader_statics2, IResourceLoaderStatics2, struct loader_statics, IActivationFactory_iface )

static HRESULT WINAPI loader_statics2_GetForCurrentView( IResourceLoaderStatics2 *iface, IResourceLoader **loader )
{
    if (!loader) return E_POINTER;
    return resource_loader_create( NULL, loader );
}

static HRESULT WINAPI loader_statics2_GetForCurrentViewWithName( IResourceLoaderStatics2 *iface, HSTRING name, IResourceLoader **loader )
{
    if (!loader) return E_POINTER;
    return resource_loader_create( WindowsGetStringRawBuffer( name, NULL ), loader );
}

static HRESULT WINAPI loader_statics2_GetForViewIndependentUse( IResourceLoaderStatics2 *iface, IResourceLoader **loader )
{
    if (!loader) return E_POINTER;
    return resource_loader_create( NULL, loader );
}

static HRESULT WINAPI loader_statics2_GetForViewIndependentUseWithName( IResourceLoaderStatics2 *iface, HSTRING name,
                                                                        IResourceLoader **loader )
{
    TRACE( "iface %p, name %s, loader %p.\n", iface, debugstr_hstring( name ), loader );

    if (!loader) return E_POINTER;
    return resource_loader_create( WindowsGetStringRawBuffer( name, NULL ), loader );
}

static const struct IResourceLoaderStatics2Vtbl loader_statics2_vtbl =
{
    loader_statics2_QueryInterface,
    loader_statics2_AddRef,
    loader_statics2_Release,
    /* IInspectable methods */
    loader_statics2_GetIids,
    loader_statics2_GetRuntimeClassName,
    loader_statics2_GetTrustLevel,
    /* IResourceLoaderStatics2 methods */
    loader_statics2_GetForCurrentView,
    loader_statics2_GetForCurrentViewWithName,
    loader_statics2_GetForViewIndependentUse,
    loader_statics2_GetForViewIndependentUseWithName,
};

DEFINE_IINSPECTABLE( loader_statics3, IResourceLoaderStatics3, struct loader_statics, IActivationFactory_iface )

static HRESULT WINAPI loader_statics3_GetForUIContext( IResourceLoaderStatics3 *iface, IUIContext *context, IResourceLoader **loader )
{
    if (!loader) return E_POINTER;
    return resource_loader_create( NULL, loader );
}

static const struct IResourceLoaderStatics3Vtbl loader_statics3_vtbl =
{
    loader_statics3_QueryInterface,
    loader_statics3_AddRef,
    loader_statics3_Release,
    /* IInspectable methods */
    loader_statics3_GetIids,
    loader_statics3_GetRuntimeClassName,
    loader_statics3_GetTrustLevel,
    /* IResourceLoaderStatics3 methods */
    loader_statics3_GetForUIContext,
};

DEFINE_IINSPECTABLE( loader_statics4, IResourceLoaderStatics4, struct loader_statics, IActivationFactory_iface )

static HRESULT WINAPI loader_statics4_GetDefaultPriPath( IResourceLoaderStatics4 *iface, HSTRING name, HSTRING *value )
{
    FIXME( "iface %p, name %s, value %p stub!\n", iface, debugstr_hstring( name ), value );
    return E_NOTIMPL;
}

static const struct IResourceLoaderStatics4Vtbl loader_statics4_vtbl =
{
    loader_statics4_QueryInterface,
    loader_statics4_AddRef,
    loader_statics4_Release,
    /* IInspectable methods */
    loader_statics4_GetIids,
    loader_statics4_GetRuntimeClassName,
    loader_statics4_GetTrustLevel,
    /* IResourceLoaderStatics4 methods */
    loader_statics4_GetDefaultPriPath,
};

static struct loader_statics loader_statics =
{
    {&loader_factory_vtbl}, {&loader_by_name_vtbl}, {&loader_statics1_vtbl}, {&loader_statics2_vtbl},
    {&loader_statics3_vtbl}, {&loader_statics4_vtbl}, 1,
};

IActivationFactory *resource_loader_factory = &loader_statics.IActivationFactory_iface;

/* ResourceManager */

struct manager_statics
{
    IActivationFactory IActivationFactory_iface;
    IResourceManagerStatics IResourceManagerStatics_iface;
    LONG ref;
};

static inline struct manager_statics *manager_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct manager_statics, IActivationFactory_iface );
}

static HRESULT WINAPI manager_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct manager_statics *impl = manager_statics_from_IActivationFactory( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IResourceManagerStatics ))
        *out = &impl->IResourceManagerStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI manager_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &manager_statics_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI manager_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &manager_statics_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI manager_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI manager_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    if (!instance) return E_POINTER;
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl manager_factory_vtbl =
{
    manager_factory_QueryInterface,
    manager_factory_AddRef,
    manager_factory_Release,
    /* IInspectable methods */
    manager_factory_GetIids,
    manager_factory_GetRuntimeClassName,
    manager_factory_GetTrustLevel,
    /* IActivationFactory methods */
    manager_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( manager_statics1, IResourceManagerStatics, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI manager_statics1_get_Current( IResourceManagerStatics *iface, IResourceManager **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = &the_resource_manager.IResourceManager_iface;
    return S_OK;
}

static HRESULT WINAPI manager_statics1_IsResourceReference( IResourceManagerStatics *iface, HSTRING reference, boolean *value )
{
    if (!value) return E_POINTER;
    *value = !wcsnicmp( WindowsGetStringRawBuffer( reference, NULL ), L"ms-resource:", 12 );
    return S_OK;
}

static const struct IResourceManagerStaticsVtbl manager_statics1_vtbl =
{
    manager_statics1_QueryInterface,
    manager_statics1_AddRef,
    manager_statics1_Release,
    /* IInspectable methods */
    manager_statics1_GetIids,
    manager_statics1_GetRuntimeClassName,
    manager_statics1_GetTrustLevel,
    /* IResourceManagerStatics methods */
    manager_statics1_get_Current,
    manager_statics1_IsResourceReference,
};

static struct manager_statics manager_statics = { {&manager_factory_vtbl}, {&manager_statics1_vtbl}, 1 };

IActivationFactory *resource_manager_factory = &manager_statics.IActivationFactory_iface;

/* ResourceContext */

struct context_statics
{
    IActivationFactory IActivationFactory_iface;
    IResourceContextStatics IResourceContextStatics_iface;
    IResourceContextStatics2 IResourceContextStatics2_iface;
    IResourceContextStatics3 IResourceContextStatics3_iface;
    IResourceContextStatics4 IResourceContextStatics4_iface;
    LONG ref;
};

static inline struct context_statics *context_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct context_statics, IActivationFactory_iface );
}

static HRESULT WINAPI context_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct context_statics *impl = context_statics_from_IActivationFactory( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IResourceContextStatics )) *out = &impl->IResourceContextStatics_iface;
    else if (IsEqualGUID( iid, &IID_IResourceContextStatics2 )) *out = &impl->IResourceContextStatics2_iface;
    else if (IsEqualGUID( iid, &IID_IResourceContextStatics3 )) *out = &impl->IResourceContextStatics3_iface;
    else if (IsEqualGUID( iid, &IID_IResourceContextStatics4 )) *out = &impl->IResourceContextStatics4_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI context_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &context_statics_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI context_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &context_statics_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI context_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI context_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI context_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI context_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!instance) return E_POINTER;
    return resource_context_create( TRUE, (IResourceContext **)instance );
}

static const struct IActivationFactoryVtbl context_factory_vtbl =
{
    context_factory_QueryInterface,
    context_factory_AddRef,
    context_factory_Release,
    /* IInspectable methods */
    context_factory_GetIids,
    context_factory_GetRuntimeClassName,
    context_factory_GetTrustLevel,
    /* IActivationFactory methods */
    context_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( context_statics1, IResourceContextStatics, struct context_statics, IActivationFactory_iface )

static HRESULT WINAPI context_statics1_CreateMatchingContext( IResourceContextStatics *iface, IIterable_ResourceQualifier *result,
                                                             IResourceContext **value )
{
    FIXME( "iface %p, result %p, value %p stub!\n", iface, result, value );
    return E_NOTIMPL;
}

static const struct IResourceContextStaticsVtbl context_statics1_vtbl =
{
    context_statics1_QueryInterface,
    context_statics1_AddRef,
    context_statics1_Release,
    /* IInspectable methods */
    context_statics1_GetIids,
    context_statics1_GetRuntimeClassName,
    context_statics1_GetTrustLevel,
    /* IResourceContextStatics methods */
    context_statics1_CreateMatchingContext,
};

DEFINE_IINSPECTABLE( context_statics2, IResourceContextStatics2, struct context_statics, IActivationFactory_iface )

static HRESULT WINAPI context_statics2_GetForCurrentView( IResourceContextStatics2 *iface, IResourceContext **value )
{
    if (!value) return E_POINTER;
    return get_shared_context( VIEW_INDEPENDENT_CONTEXT, value );
}

/* a global value goes into both shared contexts */
static HRESULT set_global_value( HSTRING key, HSTRING value )
{
    IResourceContext *context;
    HRESULT hr = S_OK;
    unsigned int i;

    for (i = 0; i < 2 && SUCCEEDED(hr); i++)
    {
        if (FAILED(hr = get_shared_context( i, &context ))) break;
        hr = qualifier_map_set( impl_from_IResourceContext( context )->values, key, value, NULL );
        IResourceContext_Release( context );
    }
    return hr;
}

static HRESULT WINAPI context_statics2_SetGlobalQualifierValue( IResourceContextStatics2 *iface, HSTRING key, HSTRING value )
{
    TRACE( "iface %p, key %s, value %s.\n", iface, debugstr_hstring( key ), debugstr_hstring( value ) );
    return set_global_value( key, value );
}

static HRESULT WINAPI context_statics2_ResetGlobalQualifierValues( IResourceContextStatics2 *iface )
{
    IResourceContext *context;
    unsigned int i;

    for (i = 0; i < 2; i++)
    {
        if (FAILED(get_shared_context( i, &context ))) continue;
        IResourceContext_Reset( context );
        IResourceContext_Release( context );
    }
    return S_OK;
}

static HRESULT WINAPI context_statics2_ResetGlobalQualifierValuesForSpecifiedQualifiers( IResourceContextStatics2 *iface,
                                                                                       IIterable_HSTRING *names )
{
    FIXME( "iface %p, names %p: resets every value\n", iface, names );
    return context_statics2_ResetGlobalQualifierValues( iface );
}

static HRESULT WINAPI context_statics2_GetForViewIndependentUse( IResourceContextStatics2 *iface, IResourceContext **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return get_shared_context( VIEW_INDEPENDENT_CONTEXT, value );
}

static const struct IResourceContextStatics2Vtbl context_statics2_vtbl =
{
    context_statics2_QueryInterface,
    context_statics2_AddRef,
    context_statics2_Release,
    /* IInspectable methods */
    context_statics2_GetIids,
    context_statics2_GetRuntimeClassName,
    context_statics2_GetTrustLevel,
    /* IResourceContextStatics2 methods */
    context_statics2_GetForCurrentView,
    context_statics2_SetGlobalQualifierValue,
    context_statics2_ResetGlobalQualifierValues,
    context_statics2_ResetGlobalQualifierValuesForSpecifiedQualifiers,
    context_statics2_GetForViewIndependentUse,
};

DEFINE_IINSPECTABLE( context_statics3, IResourceContextStatics3, struct context_statics, IActivationFactory_iface )

static HRESULT WINAPI context_statics3_SetGlobalQualifierValueWithPersistence( IResourceContextStatics3 *iface, HSTRING key,
                                                                              HSTRING value, ResourceQualifierPersistence persistence )
{
    if (persistence != ResourceQualifierPersistence_None) FIXME( "persistence %u not supported\n", persistence );
    return set_global_value( key, value );
}

static const struct IResourceContextStatics3Vtbl context_statics3_vtbl =
{
    context_statics3_QueryInterface,
    context_statics3_AddRef,
    context_statics3_Release,
    /* IInspectable methods */
    context_statics3_GetIids,
    context_statics3_GetRuntimeClassName,
    context_statics3_GetTrustLevel,
    /* IResourceContextStatics3 methods */
    context_statics3_SetGlobalQualifierValueWithPersistence,
};

DEFINE_IINSPECTABLE( context_statics4, IResourceContextStatics4, struct context_statics, IActivationFactory_iface )

static HRESULT WINAPI context_statics4_GetForUIContext( IResourceContextStatics4 *iface, IUIContext *context, IResourceContext **value )
{
    if (!value) return E_POINTER;
    return get_shared_context( VIEW_INDEPENDENT_CONTEXT, value );
}

static const struct IResourceContextStatics4Vtbl context_statics4_vtbl =
{
    context_statics4_QueryInterface,
    context_statics4_AddRef,
    context_statics4_Release,
    /* IInspectable methods */
    context_statics4_GetIids,
    context_statics4_GetRuntimeClassName,
    context_statics4_GetTrustLevel,
    /* IResourceContextStatics4 methods */
    context_statics4_GetForUIContext,
};

static struct context_statics context_statics =
{
    {&context_factory_vtbl}, {&context_statics1_vtbl}, {&context_statics2_vtbl}, {&context_statics3_vtbl},
    {&context_statics4_vtbl}, 1,
};

IActivationFactory *resource_context_factory = &context_statics.IActivationFactory_iface;
