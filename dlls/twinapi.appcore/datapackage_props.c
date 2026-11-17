/* Windows.ApplicationModel.DataTransfer.DataPackagePropertySet and DataPackagePropertySetView
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

struct map_entry
{
    HSTRING key;
    IInspectable *value;
};

/* the property set; the view of it reads the same object */
struct props
{
    IDataPackagePropertySet IDataPackagePropertySet_iface;
    IDataPackagePropertySet2 IDataPackagePropertySet2_iface;
    IDataPackagePropertySet3 IDataPackagePropertySet3_iface;
    IDataPackagePropertySet4 IDataPackagePropertySet4_iface;
    IMap_HSTRING_IInspectable IMap_HSTRING_IInspectable_iface;
    IIterable_IKeyValuePair_HSTRING_IInspectable IIterable_iface;
    LONG ref;
    CRITICAL_SECTION cs;

    HSTRING title, description, application_name, package_family_name, enterprise_id, user_activity_json;
    IUnknown *thumbnail, *logo, *listing_uri, *web_link, *application_link;
    Color logo_background;
    IVector_HSTRING *file_types;
    struct map_entry *entries;
    UINT32 count, capacity;
};

static inline struct props *impl_from_IDataPackagePropertySet( IDataPackagePropertySet *iface )
{
    return CONTAINING_RECORD( iface, struct props, IDataPackagePropertySet_iface );
}

static HRESULT props_get_string( struct props *props, HSTRING *field, HSTRING *value )
{
    HRESULT hr;

    EnterCriticalSection( &props->cs );
    hr = WindowsDuplicateString( *field, value );
    LeaveCriticalSection( &props->cs );
    return hr;
}

static HRESULT props_put_string( struct props *props, HSTRING *field, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;

    if (FAILED( hr = WindowsDuplicateString( value, &copy ) )) return hr;
    EnterCriticalSection( &props->cs );
    WindowsDeleteString( *field );
    *field = copy;
    LeaveCriticalSection( &props->cs );
    return S_OK;
}

static HRESULT props_get_object( struct props *props, IUnknown **field, void **value )
{
    EnterCriticalSection( &props->cs );
    if ((*value = *field)) IUnknown_AddRef( *field );
    LeaveCriticalSection( &props->cs );
    return S_OK;
}

static HRESULT props_put_object( struct props *props, IUnknown **field, IUnknown *value )
{
    IUnknown *old;

    if (value) IUnknown_AddRef( value );
    EnterCriticalSection( &props->cs );
    old = *field;
    *field = value;
    LeaveCriticalSection( &props->cs );
    if (old) IUnknown_Release( old );
    return S_OK;
}

static HRESULT props_get_file_types( struct props *props, void **value )
{
    HRESULT hr = S_OK;

    EnterCriticalSection( &props->cs );
    if (!props->file_types) hr = hstring_vector_create( TRUE, NULL, 0, (void **)&props->file_types );
    if (SUCCEEDED( hr ))
    {
        IVector_HSTRING_AddRef( props->file_types );
        *value = props->file_types;
    }
    LeaveCriticalSection( &props->cs );
    return hr;
}

/* the map: Lookup, Size, HasKey for both the property set and its view */

static struct map_entry *props_find( struct props *props, HSTRING key )
{
    UINT32 i;
    INT32 order;

    for (i = 0; i < props->count; i++)
    {
        WindowsCompareStringOrdinal( props->entries[i].key, key, &order );
        if (!order) return &props->entries[i];
    }
    return NULL;
}

static HRESULT props_lookup( struct props *props, HSTRING key, IInspectable **value )
{
    struct map_entry *entry;
    HRESULT hr = E_BOUNDS;

    *value = NULL;
    EnterCriticalSection( &props->cs );
    if ((entry = props_find( props, key )))
    {
        if ((*value = entry->value)) IInspectable_AddRef( *value );
        hr = S_OK;
    }
    LeaveCriticalSection( &props->cs );
    return hr;
}

static HRESULT props_insert( struct props *props, HSTRING key, IInspectable *value, boolean *replaced )
{
    struct map_entry *entry, *entries;
    HRESULT hr = S_OK;
    HSTRING copy;

    EnterCriticalSection( &props->cs );
    if ((entry = props_find( props, key )))
    {
        IInspectable *old = entry->value;
        if ((entry->value = value)) IInspectable_AddRef( value );
        if (old) IInspectable_Release( old );
        *replaced = TRUE;
    }
    else
    {
        *replaced = FALSE;
        if (props->count == props->capacity)
        {
            UINT32 capacity = props->capacity ? props->capacity * 2 : 4;
            if ((entries = realloc( props->entries, capacity * sizeof(*entries) )))
            {
                props->entries = entries;
                props->capacity = capacity;
            }
            else hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED( hr ) && SUCCEEDED( hr = WindowsDuplicateString( key, &copy ) ))
        {
            entry = &props->entries[props->count++];
            entry->key = copy;
            if ((entry->value = value)) IInspectable_AddRef( value );
        }
    }
    LeaveCriticalSection( &props->cs );
    return hr;
}

static HRESULT props_remove( struct props *props, HSTRING key )
{
    struct map_entry *entry;
    HRESULT hr = E_BOUNDS;

    EnterCriticalSection( &props->cs );
    if ((entry = props_find( props, key )))
    {
        WindowsDeleteString( entry->key );
        if (entry->value) IInspectable_Release( entry->value );
        *entry = props->entries[--props->count];
        hr = S_OK;
    }
    LeaveCriticalSection( &props->cs );
    return hr;
}

static void props_clear_entries( struct props *props )
{
    UINT32 i;

    for (i = 0; i < props->count; i++)
    {
        WindowsDeleteString( props->entries[i].key );
        if (props->entries[i].value) IInspectable_Release( props->entries[i].value );
    }
    props->count = 0;
}

static HRESULT props_has_key( struct props *props, HSTRING key, boolean *found )
{
    EnterCriticalSection( &props->cs );
    *found = !!props_find( props, key );
    LeaveCriticalSection( &props->cs );
    return S_OK;
}

/* ---- the property set ---- */

static HRESULT WINAPI set_QueryInterface( IDataPackagePropertySet *iface, REFIID iid, void **out )
{
    struct props *impl = impl_from_IDataPackagePropertySet( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDataPackagePropertySet ))
        *out = &impl->IDataPackagePropertySet_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackagePropertySet2 ))
        *out = &impl->IDataPackagePropertySet2_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackagePropertySet3 ))
        *out = &impl->IDataPackagePropertySet3_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackagePropertySet4 ))
        *out = &impl->IDataPackagePropertySet4_iface;
    else if (IsEqualGUID( iid, &IID_IMap_HSTRING_IInspectable ))
        *out = &impl->IMap_HSTRING_IInspectable_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable ))
        *out = &impl->IIterable_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI set_AddRef( IDataPackagePropertySet *iface )
{
    return InterlockedIncrement( &impl_from_IDataPackagePropertySet( iface )->ref );
}

static ULONG WINAPI set_Release( IDataPackagePropertySet *iface )
{
    struct props *impl = impl_from_IDataPackagePropertySet( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->title );
        WindowsDeleteString( impl->description );
        WindowsDeleteString( impl->application_name );
        WindowsDeleteString( impl->package_family_name );
        WindowsDeleteString( impl->enterprise_id );
        WindowsDeleteString( impl->user_activity_json );
        if (impl->thumbnail) IUnknown_Release( impl->thumbnail );
        if (impl->logo) IUnknown_Release( impl->logo );
        if (impl->listing_uri) IUnknown_Release( impl->listing_uri );
        if (impl->web_link) IUnknown_Release( impl->web_link );
        if (impl->application_link) IUnknown_Release( impl->application_link );
        if (impl->file_types) IVector_HSTRING_Release( impl->file_types );
        props_clear_entries( impl );
        free( impl->entries );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI set_GetIids( IDataPackagePropertySet *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 6 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IDataPackagePropertySet;
    list[1] = IID_IDataPackagePropertySet2;
    list[2] = IID_IDataPackagePropertySet3;
    list[3] = IID_IDataPackagePropertySet4;
    list[4] = IID_IMap_HSTRING_IInspectable;
    list[5] = IID_IIterable_IKeyValuePair_HSTRING_IInspectable;
    *count = 6;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI set_GetRuntimeClassName( IDataPackagePropertySet *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.DataPackagePropertySet", name );
}

static HRESULT WINAPI set_GetTrustLevel( IDataPackagePropertySet *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

#define STR_GET( pfx, itype, props_expr, name, field ) \
    static HRESULT WINAPI pfx##_get_##name( itype *iface, HSTRING *value ) \
    { \
        struct props *props = props_expr( iface ); \
        return props_get_string( props, &props->field, value ); \
    }
#define STR_PUT( pfx, itype, props_expr, name, field ) \
    static HRESULT WINAPI pfx##_put_##name( itype *iface, HSTRING value ) \
    { \
        struct props *props = props_expr( iface ); \
        return props_put_string( props, &props->field, value ); \
    }
#define OBJ_GET( pfx, itype, props_expr, name, field, otype ) \
    static HRESULT WINAPI pfx##_get_##name( itype *iface, otype **value ) \
    { \
        struct props *props = props_expr( iface ); \
        return props_get_object( props, &props->field, (void **)value ); \
    }
#define OBJ_PUT( pfx, itype, props_expr, name, field, otype ) \
    static HRESULT WINAPI pfx##_put_##name( itype *iface, otype *value ) \
    { \
        struct props *props = props_expr( iface ); \
        return props_put_object( props, &props->field, (IUnknown *)value ); \
    }

STR_GET( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, Title, title )
STR_PUT( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, Title, title )
STR_GET( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, Description, description )
STR_PUT( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, Description, description )
OBJ_GET( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, Thumbnail, thumbnail, IRandomAccessStreamReference )
OBJ_PUT( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, Thumbnail, thumbnail, IRandomAccessStreamReference )
STR_GET( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, ApplicationName, application_name )
STR_PUT( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, ApplicationName, application_name )
OBJ_GET( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, ApplicationListingUri, listing_uri, IUriRuntimeClass )
OBJ_PUT( set, IDataPackagePropertySet, impl_from_IDataPackagePropertySet, ApplicationListingUri, listing_uri, IUriRuntimeClass )

static HRESULT WINAPI set_get_FileTypes( IDataPackagePropertySet *iface, IVector_HSTRING **value )
{
    return props_get_file_types( impl_from_IDataPackagePropertySet( iface ), (void **)value );
}

static const IDataPackagePropertySetVtbl set_vtbl =
{
    set_QueryInterface,
    set_AddRef,
    set_Release,
    /* IInspectable methods */
    set_GetIids,
    set_GetRuntimeClassName,
    set_GetTrustLevel,
    /* IDataPackagePropertySet methods */
    set_get_Title,
    set_put_Title,
    set_get_Description,
    set_put_Description,
    set_get_Thumbnail,
    set_put_Thumbnail,
    set_get_FileTypes,
    set_get_ApplicationName,
    set_put_ApplicationName,
    set_get_ApplicationListingUri,
    set_put_ApplicationListingUri,
};

DEFINE_IINSPECTABLE( set2, IDataPackagePropertySet2, struct props, IDataPackagePropertySet_iface )

#define set2_props( iface ) impl_from_IDataPackagePropertySet2( iface )

OBJ_GET( set2, IDataPackagePropertySet2, set2_props, ContentSourceWebLink, web_link, IUriRuntimeClass )
OBJ_PUT( set2, IDataPackagePropertySet2, set2_props, ContentSourceWebLink, web_link, IUriRuntimeClass )
OBJ_GET( set2, IDataPackagePropertySet2, set2_props, ContentSourceApplicationLink, application_link, IUriRuntimeClass )
OBJ_PUT( set2, IDataPackagePropertySet2, set2_props, ContentSourceApplicationLink, application_link, IUriRuntimeClass )
STR_GET( set2, IDataPackagePropertySet2, set2_props, PackageFamilyName, package_family_name )
STR_PUT( set2, IDataPackagePropertySet2, set2_props, PackageFamilyName, package_family_name )
OBJ_GET( set2, IDataPackagePropertySet2, set2_props, Square30x30Logo, logo, IRandomAccessStreamReference )
OBJ_PUT( set2, IDataPackagePropertySet2, set2_props, Square30x30Logo, logo, IRandomAccessStreamReference )

static HRESULT WINAPI set2_get_LogoBackgroundColor( IDataPackagePropertySet2 *iface, Color *value )
{
    struct props *props = set2_props( iface );

    EnterCriticalSection( &props->cs );
    *value = props->logo_background;
    LeaveCriticalSection( &props->cs );
    return S_OK;
}

static HRESULT WINAPI set2_put_LogoBackgroundColor( IDataPackagePropertySet2 *iface, Color value )
{
    struct props *props = set2_props( iface );

    EnterCriticalSection( &props->cs );
    props->logo_background = value;
    LeaveCriticalSection( &props->cs );
    return S_OK;
}

static const IDataPackagePropertySet2Vtbl set2_vtbl =
{
    set2_QueryInterface,
    set2_AddRef,
    set2_Release,
    /* IInspectable methods */
    set2_GetIids,
    set2_GetRuntimeClassName,
    set2_GetTrustLevel,
    /* IDataPackagePropertySet2 methods */
    set2_get_ContentSourceWebLink,
    set2_put_ContentSourceWebLink,
    set2_get_ContentSourceApplicationLink,
    set2_put_ContentSourceApplicationLink,
    set2_get_PackageFamilyName,
    set2_put_PackageFamilyName,
    set2_get_Square30x30Logo,
    set2_put_Square30x30Logo,
    set2_get_LogoBackgroundColor,
    set2_put_LogoBackgroundColor,
};

DEFINE_IINSPECTABLE( set3, IDataPackagePropertySet3, struct props, IDataPackagePropertySet_iface )

#define set3_props( iface ) impl_from_IDataPackagePropertySet3( iface )

STR_GET( set3, IDataPackagePropertySet3, set3_props, EnterpriseId, enterprise_id )
STR_PUT( set3, IDataPackagePropertySet3, set3_props, EnterpriseId, enterprise_id )

static const IDataPackagePropertySet3Vtbl set3_vtbl =
{
    set3_QueryInterface,
    set3_AddRef,
    set3_Release,
    /* IInspectable methods */
    set3_GetIids,
    set3_GetRuntimeClassName,
    set3_GetTrustLevel,
    /* IDataPackagePropertySet3 methods */
    set3_get_EnterpriseId,
    set3_put_EnterpriseId,
};

DEFINE_IINSPECTABLE( set4, IDataPackagePropertySet4, struct props, IDataPackagePropertySet_iface )

#define set4_props( iface ) impl_from_IDataPackagePropertySet4( iface )

STR_GET( set4, IDataPackagePropertySet4, set4_props, ContentSourceUserActivityJson, user_activity_json )
STR_PUT( set4, IDataPackagePropertySet4, set4_props, ContentSourceUserActivityJson, user_activity_json )

static const IDataPackagePropertySet4Vtbl set4_vtbl =
{
    set4_QueryInterface,
    set4_AddRef,
    set4_Release,
    /* IInspectable methods */
    set4_GetIids,
    set4_GetRuntimeClassName,
    set4_GetTrustLevel,
    /* IDataPackagePropertySet4 methods */
    set4_get_ContentSourceUserActivityJson,
    set4_put_ContentSourceUserActivityJson,
};

DEFINE_IINSPECTABLE( map, IMap_HSTRING_IInspectable, struct props, IDataPackagePropertySet_iface )

static HRESULT WINAPI map_Lookup( IMap_HSTRING_IInspectable *iface, HSTRING key, IInspectable **value )
{
    return props_lookup( impl_from_IMap_HSTRING_IInspectable( iface ), key, value );
}

static HRESULT WINAPI map_get_Size( IMap_HSTRING_IInspectable *iface, UINT32 *value )
{
    *value = impl_from_IMap_HSTRING_IInspectable( iface )->count;
    return S_OK;
}

static HRESULT WINAPI map_HasKey( IMap_HSTRING_IInspectable *iface, HSTRING key, boolean *found )
{
    return props_has_key( impl_from_IMap_HSTRING_IInspectable( iface ), key, found );
}

static HRESULT WINAPI map_GetView( IMap_HSTRING_IInspectable *iface, IMapView_HSTRING_IInspectable **view )
{
    FIXME( "iface %p, view %p stub!\n", iface, view );
    return E_NOTIMPL;
}

static HRESULT WINAPI map_Insert( IMap_HSTRING_IInspectable *iface, HSTRING key, IInspectable *value, boolean *replaced )
{
    return props_insert( impl_from_IMap_HSTRING_IInspectable( iface ), key, value, replaced );
}

static HRESULT WINAPI map_Remove( IMap_HSTRING_IInspectable *iface, HSTRING key )
{
    return props_remove( impl_from_IMap_HSTRING_IInspectable( iface ), key );
}

static HRESULT WINAPI map_Clear( IMap_HSTRING_IInspectable *iface )
{
    struct props *props = impl_from_IMap_HSTRING_IInspectable( iface );

    EnterCriticalSection( &props->cs );
    props_clear_entries( props );
    LeaveCriticalSection( &props->cs );
    return S_OK;
}

static const IMap_HSTRING_IInspectableVtbl map_vtbl =
{
    map_QueryInterface,
    map_AddRef,
    map_Release,
    /* IInspectable methods */
    map_GetIids,
    map_GetRuntimeClassName,
    map_GetTrustLevel,
    /* IMap<HSTRING, IInspectable *> methods */
    map_Lookup,
    map_get_Size,
    map_HasKey,
    map_GetView,
    map_Insert,
    map_Remove,
    map_Clear,
};

DEFINE_IINSPECTABLE_( iterable, IIterable_IKeyValuePair_HSTRING_IInspectable, struct props, impl_from_props_iterable,
                      IIterable_iface, &impl->IDataPackagePropertySet_iface )

static HRESULT WINAPI iterable_First( IIterable_IKeyValuePair_HSTRING_IInspectable *iface,
                                      IIterator_IKeyValuePair_HSTRING_IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const IIterable_IKeyValuePair_HSTRING_IInspectableVtbl iterable_vtbl =
{
    iterable_QueryInterface,
    iterable_AddRef,
    iterable_Release,
    /* IInspectable methods */
    iterable_GetIids,
    iterable_GetRuntimeClassName,
    iterable_GetTrustLevel,
    /* IIterable<IKeyValuePair<HSTRING, IInspectable *> *> methods */
    iterable_First,
};

HRESULT data_props_create( IDataPackagePropertySet **out )
{
    struct props *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDataPackagePropertySet_iface.lpVtbl = &set_vtbl;
    impl->IDataPackagePropertySet2_iface.lpVtbl = &set2_vtbl;
    impl->IDataPackagePropertySet3_iface.lpVtbl = &set3_vtbl;
    impl->IDataPackagePropertySet4_iface.lpVtbl = &set4_vtbl;
    impl->IMap_HSTRING_IInspectable_iface.lpVtbl = &map_vtbl;
    impl->IIterable_iface.lpVtbl = &iterable_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": props.cs");
    *out = &impl->IDataPackagePropertySet_iface;
    return S_OK;
}

/* ---- the view ---- */

struct props_view
{
    IDataPackagePropertySetView IDataPackagePropertySetView_iface;
    IDataPackagePropertySetView2 IDataPackagePropertySetView2_iface;
    IDataPackagePropertySetView3 IDataPackagePropertySetView3_iface;
    IDataPackagePropertySetView4 IDataPackagePropertySetView4_iface;
    IDataPackagePropertySetView5 IDataPackagePropertySetView5_iface;
    IMapView_HSTRING_IInspectable IMapView_HSTRING_IInspectable_iface;
    IIterable_IKeyValuePair_HSTRING_IInspectable IIterable_iface;
    LONG ref;
    struct props *props;
};

static inline struct props_view *impl_from_IDataPackagePropertySetView( IDataPackagePropertySetView *iface )
{
    return CONTAINING_RECORD( iface, struct props_view, IDataPackagePropertySetView_iface );
}

static HRESULT WINAPI view_QueryInterface( IDataPackagePropertySetView *iface, REFIID iid, void **out )
{
    struct props_view *impl = impl_from_IDataPackagePropertySetView( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDataPackagePropertySetView ))
        *out = &impl->IDataPackagePropertySetView_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackagePropertySetView2 ))
        *out = &impl->IDataPackagePropertySetView2_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackagePropertySetView3 ))
        *out = &impl->IDataPackagePropertySetView3_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackagePropertySetView4 ))
        *out = &impl->IDataPackagePropertySetView4_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackagePropertySetView5 ))
        *out = &impl->IDataPackagePropertySetView5_iface;
    else if (IsEqualGUID( iid, &IID_IMapView_HSTRING_IInspectable ))
        *out = &impl->IMapView_HSTRING_IInspectable_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_IInspectable ))
        *out = &impl->IIterable_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI view_AddRef( IDataPackagePropertySetView *iface )
{
    return InterlockedIncrement( &impl_from_IDataPackagePropertySetView( iface )->ref );
}

static ULONG WINAPI view_Release( IDataPackagePropertySetView *iface )
{
    struct props_view *impl = impl_from_IDataPackagePropertySetView( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IDataPackagePropertySet_Release( &impl->props->IDataPackagePropertySet_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI view_GetIids( IDataPackagePropertySetView *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 7 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IDataPackagePropertySetView;
    list[1] = IID_IDataPackagePropertySetView2;
    list[2] = IID_IDataPackagePropertySetView3;
    list[3] = IID_IDataPackagePropertySetView4;
    list[4] = IID_IDataPackagePropertySetView5;
    list[5] = IID_IMapView_HSTRING_IInspectable;
    list[6] = IID_IIterable_IKeyValuePair_HSTRING_IInspectable;
    *count = 7;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI view_GetRuntimeClassName( IDataPackagePropertySetView *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.DataPackagePropertySetView", name );
}

static HRESULT WINAPI view_GetTrustLevel( IDataPackagePropertySetView *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

#define view_props( iface ) (impl_from_IDataPackagePropertySetView( iface )->props)

STR_GET( view, IDataPackagePropertySetView, view_props, Title, title )
STR_GET( view, IDataPackagePropertySetView, view_props, Description, description )
OBJ_GET( view, IDataPackagePropertySetView, view_props, Thumbnail, thumbnail, IRandomAccessStreamReference )
STR_GET( view, IDataPackagePropertySetView, view_props, ApplicationName, application_name )
OBJ_GET( view, IDataPackagePropertySetView, view_props, ApplicationListingUri, listing_uri, IUriRuntimeClass )

static HRESULT WINAPI view_get_FileTypes( IDataPackagePropertySetView *iface, IVectorView_HSTRING **value )
{
    struct props *props = view_props( iface );
    IVector_HSTRING *types;
    HRESULT hr;

    if (FAILED( hr = props_get_file_types( props, (void **)&types ) )) return hr;
    hr = IVector_HSTRING_GetView( types, value );
    IVector_HSTRING_Release( types );
    return hr;
}

static const IDataPackagePropertySetViewVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    /* IInspectable methods */
    view_GetIids,
    view_GetRuntimeClassName,
    view_GetTrustLevel,
    /* IDataPackagePropertySetView methods */
    view_get_Title,
    view_get_Description,
    view_get_Thumbnail,
    view_get_FileTypes,
    view_get_ApplicationName,
    view_get_ApplicationListingUri,
};

DEFINE_IINSPECTABLE( view2, IDataPackagePropertySetView2, struct props_view, IDataPackagePropertySetView_iface )

#define view2_props( iface ) (impl_from_IDataPackagePropertySetView2( iface )->props)

STR_GET( view2, IDataPackagePropertySetView2, view2_props, PackageFamilyName, package_family_name )
OBJ_GET( view2, IDataPackagePropertySetView2, view2_props, ContentSourceWebLink, web_link, IUriRuntimeClass )
OBJ_GET( view2, IDataPackagePropertySetView2, view2_props, ContentSourceApplicationLink, application_link, IUriRuntimeClass )
OBJ_GET( view2, IDataPackagePropertySetView2, view2_props, Square30x30Logo, logo, IRandomAccessStreamReference )

static HRESULT WINAPI view2_get_LogoBackgroundColor( IDataPackagePropertySetView2 *iface, Color *value )
{
    struct props *props = view2_props( iface );

    EnterCriticalSection( &props->cs );
    *value = props->logo_background;
    LeaveCriticalSection( &props->cs );
    return S_OK;
}

static const IDataPackagePropertySetView2Vtbl view2_vtbl =
{
    view2_QueryInterface,
    view2_AddRef,
    view2_Release,
    /* IInspectable methods */
    view2_GetIids,
    view2_GetRuntimeClassName,
    view2_GetTrustLevel,
    /* IDataPackagePropertySetView2 methods */
    view2_get_PackageFamilyName,
    view2_get_ContentSourceWebLink,
    view2_get_ContentSourceApplicationLink,
    view2_get_Square30x30Logo,
    view2_get_LogoBackgroundColor,
};

DEFINE_IINSPECTABLE( view3, IDataPackagePropertySetView3, struct props_view, IDataPackagePropertySetView_iface )

#define view3_props( iface ) (impl_from_IDataPackagePropertySetView3( iface )->props)

STR_GET( view3, IDataPackagePropertySetView3, view3_props, EnterpriseId, enterprise_id )

static const IDataPackagePropertySetView3Vtbl view3_vtbl =
{
    view3_QueryInterface,
    view3_AddRef,
    view3_Release,
    /* IInspectable methods */
    view3_GetIids,
    view3_GetRuntimeClassName,
    view3_GetTrustLevel,
    /* IDataPackagePropertySetView3 methods */
    view3_get_EnterpriseId,
};

DEFINE_IINSPECTABLE( view4, IDataPackagePropertySetView4, struct props_view, IDataPackagePropertySetView_iface )

#define view4_props( iface ) (impl_from_IDataPackagePropertySetView4( iface )->props)

STR_GET( view4, IDataPackagePropertySetView4, view4_props, ContentSourceUserActivityJson, user_activity_json )

static const IDataPackagePropertySetView4Vtbl view4_vtbl =
{
    view4_QueryInterface,
    view4_AddRef,
    view4_Release,
    /* IInspectable methods */
    view4_GetIids,
    view4_GetRuntimeClassName,
    view4_GetTrustLevel,
    /* IDataPackagePropertySetView4 methods */
    view4_get_ContentSourceUserActivityJson,
};

DEFINE_IINSPECTABLE( view5, IDataPackagePropertySetView5, struct props_view, IDataPackagePropertySetView_iface )

static HRESULT WINAPI view5_get_IsFromRoamingClipboard( IDataPackagePropertySetView5 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static const IDataPackagePropertySetView5Vtbl view5_vtbl =
{
    view5_QueryInterface,
    view5_AddRef,
    view5_Release,
    /* IInspectable methods */
    view5_GetIids,
    view5_GetRuntimeClassName,
    view5_GetTrustLevel,
    /* IDataPackagePropertySetView5 methods */
    view5_get_IsFromRoamingClipboard,
};

DEFINE_IINSPECTABLE( mapview, IMapView_HSTRING_IInspectable, struct props_view, IDataPackagePropertySetView_iface )

static HRESULT WINAPI mapview_Lookup( IMapView_HSTRING_IInspectable *iface, HSTRING key, IInspectable **value )
{
    return props_lookup( impl_from_IMapView_HSTRING_IInspectable( iface )->props, key, value );
}

static HRESULT WINAPI mapview_get_Size( IMapView_HSTRING_IInspectable *iface, UINT32 *value )
{
    *value = impl_from_IMapView_HSTRING_IInspectable( iface )->props->count;
    return S_OK;
}

static HRESULT WINAPI mapview_HasKey( IMapView_HSTRING_IInspectable *iface, HSTRING key, boolean *found )
{
    return props_has_key( impl_from_IMapView_HSTRING_IInspectable( iface )->props, key, found );
}

static HRESULT WINAPI mapview_Split( IMapView_HSTRING_IInspectable *iface, IMapView_HSTRING_IInspectable **first,
                                     IMapView_HSTRING_IInspectable **second )
{
    *first = *second = NULL;
    return S_OK;
}

static const IMapView_HSTRING_IInspectableVtbl mapview_vtbl =
{
    mapview_QueryInterface,
    mapview_AddRef,
    mapview_Release,
    /* IInspectable methods */
    mapview_GetIids,
    mapview_GetRuntimeClassName,
    mapview_GetTrustLevel,
    /* IMapView<HSTRING, IInspectable *> methods */
    mapview_Lookup,
    mapview_get_Size,
    mapview_HasKey,
    mapview_Split,
};

DEFINE_IINSPECTABLE_( view_iterable, IIterable_IKeyValuePair_HSTRING_IInspectable, struct props_view,
                      impl_from_view_iterable, IIterable_iface, &impl->IDataPackagePropertySetView_iface )

static HRESULT WINAPI view_iterable_First( IIterable_IKeyValuePair_HSTRING_IInspectable *iface,
                                           IIterator_IKeyValuePair_HSTRING_IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const IIterable_IKeyValuePair_HSTRING_IInspectableVtbl view_iterable_vtbl =
{
    view_iterable_QueryInterface,
    view_iterable_AddRef,
    view_iterable_Release,
    /* IInspectable methods */
    view_iterable_GetIids,
    view_iterable_GetRuntimeClassName,
    view_iterable_GetTrustLevel,
    /* IIterable<IKeyValuePair<HSTRING, IInspectable *> *> methods */
    view_iterable_First,
};

HRESULT data_props_view_create( IDataPackagePropertySet *props, IDataPackagePropertySetView **out )
{
    struct props_view *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDataPackagePropertySetView_iface.lpVtbl = &view_vtbl;
    impl->IDataPackagePropertySetView2_iface.lpVtbl = &view2_vtbl;
    impl->IDataPackagePropertySetView3_iface.lpVtbl = &view3_vtbl;
    impl->IDataPackagePropertySetView4_iface.lpVtbl = &view4_vtbl;
    impl->IDataPackagePropertySetView5_iface.lpVtbl = &view5_vtbl;
    impl->IMapView_HSTRING_IInspectable_iface.lpVtbl = &mapview_vtbl;
    impl->IIterable_iface.lpVtbl = &view_iterable_vtbl;
    impl->ref = 1;
    impl->props = impl_from_IDataPackagePropertySet( props );
    IDataPackagePropertySet_AddRef( props );
    *out = &impl->IDataPackagePropertySetView_iface;
    return S_OK;
}
