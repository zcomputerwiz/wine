/* WinRT Windows.ApplicationModel.UserActivities: UserActivityChannel, UserActivity and their parts
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

/*
 * An application describes what the user is doing as a UserActivity (an id, the visual elements a timeline would
 * show, the URIs that bring the activity back) and saves it to its channel. There is no timeline and no
 * synchronisation in WineExplorer, so the channel keeps the saved activities of the process: GetOrCreate returns
 * a published activity again (an activity is published when a session is made, as measured on Windows 11), Delete
 * removes one. The JSON forms of an activity (ToJson, TryParseFromJson, ToJsonArray) and
 * the session history belong to the timeline and are not implemented: they report E_NOTIMPL or nothing found.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

static HRESULT set_string( HSTRING *field, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( *field );
    *field = copy;
    return S_OK;
}

static void set_uri( IUriRuntimeClass **field, IUriRuntimeClass *value )
{
    if (value) IUriRuntimeClass_AddRef( value );
    if (*field) IUriRuntimeClass_Release( *field );
    *field = value;
}

static HRESULT get_uri( IUriRuntimeClass *field, IUriRuntimeClass **value )
{
    if ((*value = field)) IUriRuntimeClass_AddRef( field );
    return S_OK;
}

static BOOL is_basic_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject );
}

static HRESULT single_iid( const GUID *iid, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = *iid;
    *count = 1;
    *iids = list;
    return S_OK;
}

/* ---- UserActivityContentInfo ---- */

struct content_info
{
    IUserActivityContentInfo IUserActivityContentInfo_iface;
    LONG ref;
    HSTRING json;
};

static inline struct content_info *impl_from_IUserActivityContentInfo( IUserActivityContentInfo *iface )
{
    return CONTAINING_RECORD( iface, struct content_info, IUserActivityContentInfo_iface );
}

static HRESULT WINAPI info_QueryInterface( IUserActivityContentInfo *iface, REFIID iid, void **out )
{
    struct content_info *impl = impl_from_IUserActivityContentInfo( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IUserActivityContentInfo ))
    {
        IInspectable_AddRef( (*out = &impl->IUserActivityContentInfo_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI info_AddRef( IUserActivityContentInfo *iface )
{
    return InterlockedIncrement( &impl_from_IUserActivityContentInfo( iface )->ref );
}

static ULONG WINAPI info_Release( IUserActivityContentInfo *iface )
{
    struct content_info *impl = impl_from_IUserActivityContentInfo( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->json );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI info_GetIids( IUserActivityContentInfo *iface, ULONG *count, IID **iids )
{
    return single_iid( &IID_IUserActivityContentInfo, count, iids );
}

static HRESULT WINAPI info_GetRuntimeClassName( IUserActivityContentInfo *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.UserActivities.UserActivityContentInfo", name );
}

static HRESULT WINAPI info_GetTrustLevel( IUserActivityContentInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI info_ToJson( IUserActivityContentInfo *iface, HSTRING *result )
{
    return WindowsDuplicateString( impl_from_IUserActivityContentInfo( iface )->json, result );
}

static const IUserActivityContentInfoVtbl info_vtbl =
{
    info_QueryInterface,
    info_AddRef,
    info_Release,
    /* IInspectable methods */
    info_GetIids,
    info_GetRuntimeClassName,
    info_GetTrustLevel,
    /* IUserActivityContentInfo methods */
    info_ToJson,
};

static HRESULT WINAPI info_statics_FromJson( IUserActivityContentInfoStatics *iface, HSTRING value,
                                             IUserActivityContentInfo **result )
{
    struct content_info *impl;
    HRESULT hr;

    TRACE( "iface %p, value %s, result %p.\n", iface, debugstr_hstring( value ), result );

    *result = NULL;
    if (!WindowsGetStringLen( value )) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUserActivityContentInfo_iface.lpVtbl = &info_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( value, &impl->json )))
    {
        free( impl );
        return hr;
    }
    *result = &impl->IUserActivityContentInfo_iface;
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( info_statics, IUserActivityContentInfoStatics )

static const IUserActivityContentInfoStaticsVtbl info_statics_vtbl =
{
    info_statics_QueryInterface,
    info_statics_AddRef,
    info_statics_Release,
    /* IInspectable methods */
    info_statics_GetIids,
    info_statics_GetRuntimeClassName,
    info_statics_GetTrustLevel,
    /* IUserActivityContentInfoStatics methods */
    info_statics_FromJson,
};

static struct static_factory info_factory =
{
    {&static_factory_vtbl},
    1,
    L"Windows.ApplicationModel.UserActivities.UserActivityContentInfo",
    NULL,
    1,
    {{ &IID_IUserActivityContentInfoStatics, { &info_statics_vtbl, &info_factory } }},
};

IActivationFactory *user_activity_content_info_factory = &info_factory.IActivationFactory_iface;

/* ---- UserActivityAttribution ---- */

struct attribution
{
    IUserActivityAttribution IUserActivityAttribution_iface;
    LONG ref;
    IUriRuntimeClass *icon_uri;
    HSTRING alternate_text;
    boolean add_image_query;
};

static inline struct attribution *impl_from_IUserActivityAttribution( IUserActivityAttribution *iface )
{
    return CONTAINING_RECORD( iface, struct attribution, IUserActivityAttribution_iface );
}

static HRESULT WINAPI attr_QueryInterface( IUserActivityAttribution *iface, REFIID iid, void **out )
{
    struct attribution *impl = impl_from_IUserActivityAttribution( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IUserActivityAttribution ))
    {
        IInspectable_AddRef( (*out = &impl->IUserActivityAttribution_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI attr_AddRef( IUserActivityAttribution *iface )
{
    return InterlockedIncrement( &impl_from_IUserActivityAttribution( iface )->ref );
}

static ULONG WINAPI attr_Release( IUserActivityAttribution *iface )
{
    struct attribution *impl = impl_from_IUserActivityAttribution( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->icon_uri) IUriRuntimeClass_Release( impl->icon_uri );
        WindowsDeleteString( impl->alternate_text );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI attr_GetIids( IUserActivityAttribution *iface, ULONG *count, IID **iids )
{
    return single_iid( &IID_IUserActivityAttribution, count, iids );
}

static HRESULT WINAPI attr_GetRuntimeClassName( IUserActivityAttribution *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.UserActivities.UserActivityAttribution", name );
}

static HRESULT WINAPI attr_GetTrustLevel( IUserActivityAttribution *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI attr_get_IconUri( IUserActivityAttribution *iface, IUriRuntimeClass **value )
{
    return get_uri( impl_from_IUserActivityAttribution( iface )->icon_uri, value );
}

static HRESULT WINAPI attr_put_IconUri( IUserActivityAttribution *iface, IUriRuntimeClass *value )
{
    set_uri( &impl_from_IUserActivityAttribution( iface )->icon_uri, value );
    return S_OK;
}

static HRESULT WINAPI attr_get_AlternateText( IUserActivityAttribution *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUserActivityAttribution( iface )->alternate_text, value );
}

static HRESULT WINAPI attr_put_AlternateText( IUserActivityAttribution *iface, HSTRING value )
{
    return set_string( &impl_from_IUserActivityAttribution( iface )->alternate_text, value );
}

static HRESULT WINAPI attr_get_AddImageQuery( IUserActivityAttribution *iface, boolean *value )
{
    *value = impl_from_IUserActivityAttribution( iface )->add_image_query;
    return S_OK;
}

static HRESULT WINAPI attr_put_AddImageQuery( IUserActivityAttribution *iface, boolean value )
{
    impl_from_IUserActivityAttribution( iface )->add_image_query = value;
    return S_OK;
}

static const IUserActivityAttributionVtbl attr_vtbl =
{
    attr_QueryInterface,
    attr_AddRef,
    attr_Release,
    /* IInspectable methods */
    attr_GetIids,
    attr_GetRuntimeClassName,
    attr_GetTrustLevel,
    /* IUserActivityAttribution methods */
    attr_get_IconUri,
    attr_put_IconUri,
    attr_get_AlternateText,
    attr_put_AlternateText,
    attr_get_AddImageQuery,
    attr_put_AddImageQuery,
};

static HRESULT attribution_create( IUriRuntimeClass *icon_uri, IUserActivityAttribution **out )
{
    struct attribution *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUserActivityAttribution_iface.lpVtbl = &attr_vtbl;
    impl->ref = 1;
    set_uri( &impl->icon_uri, icon_uri );
    *out = &impl->IUserActivityAttribution_iface;
    return S_OK;
}

static HRESULT attribution_activate( IInspectable **instance )
{
    return attribution_create( NULL, (IUserActivityAttribution **)instance );
}

static HRESULT WINAPI attr_statics_CreateWithUri( IUserActivityAttributionFactory *iface, IUriRuntimeClass *icon_uri,
                                                  IUserActivityAttribution **value )
{
    TRACE( "iface %p, icon_uri %p, value %p.\n", iface, icon_uri, value );
    return attribution_create( icon_uri, value );
}

DEFINE_STATICS_IINSPECTABLE( attr_statics, IUserActivityAttributionFactory )

static const IUserActivityAttributionFactoryVtbl attr_statics_vtbl =
{
    attr_statics_QueryInterface,
    attr_statics_AddRef,
    attr_statics_Release,
    /* IInspectable methods */
    attr_statics_GetIids,
    attr_statics_GetRuntimeClassName,
    attr_statics_GetTrustLevel,
    /* IUserActivityAttributionFactory methods */
    attr_statics_CreateWithUri,
};

static struct static_factory attribution_factory =
{
    {&static_factory_vtbl},
    1,
    L"Windows.ApplicationModel.UserActivities.UserActivityAttribution",
    attribution_activate,
    1,
    {{ &IID_IUserActivityAttributionFactory, { &attr_statics_vtbl, &attribution_factory } }},
};

IActivationFactory *user_activity_attribution_factory = &attribution_factory.IActivationFactory_iface;

/* ---- UserActivityVisualElements ---- */

struct visual_elements
{
    IUserActivityVisualElements IUserActivityVisualElements_iface;
    IUserActivityVisualElements2 IUserActivityVisualElements2_iface;
    LONG ref;
    HSTRING display_text;
    HSTRING description;
    HSTRING attribution_text;
    Color color;
    IUserActivityAttribution *attribution;
    IAdaptiveCard *content;
};

static inline struct visual_elements *impl_from_IUserActivityVisualElements( IUserActivityVisualElements *iface )
{
    return CONTAINING_RECORD( iface, struct visual_elements, IUserActivityVisualElements_iface );
}

static HRESULT WINAPI visual_QueryInterface( IUserActivityVisualElements *iface, REFIID iid, void **out )
{
    struct visual_elements *impl = impl_from_IUserActivityVisualElements( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IUserActivityVisualElements ))
        *out = &impl->IUserActivityVisualElements_iface;
    else if (IsEqualGUID( iid, &IID_IUserActivityVisualElements2 ))
        *out = &impl->IUserActivityVisualElements2_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI visual_AddRef( IUserActivityVisualElements *iface )
{
    return InterlockedIncrement( &impl_from_IUserActivityVisualElements( iface )->ref );
}

static ULONG WINAPI visual_Release( IUserActivityVisualElements *iface )
{
    struct visual_elements *impl = impl_from_IUserActivityVisualElements( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->display_text );
        WindowsDeleteString( impl->description );
        WindowsDeleteString( impl->attribution_text );
        if (impl->attribution) IUserActivityAttribution_Release( impl->attribution );
        if (impl->content) IAdaptiveCard_Release( impl->content );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI visual_GetIids( IUserActivityVisualElements *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IUserActivityVisualElements;
    list[1] = IID_IUserActivityVisualElements2;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI visual_GetRuntimeClassName( IUserActivityVisualElements *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.UserActivities.UserActivityVisualElements", name );
}

static HRESULT WINAPI visual_GetTrustLevel( IUserActivityVisualElements *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI visual_get_DisplayText( IUserActivityVisualElements *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUserActivityVisualElements( iface )->display_text, value );
}

static HRESULT WINAPI visual_put_DisplayText( IUserActivityVisualElements *iface, HSTRING value )
{
    return set_string( &impl_from_IUserActivityVisualElements( iface )->display_text, value );
}

static HRESULT WINAPI visual_get_Description( IUserActivityVisualElements *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUserActivityVisualElements( iface )->description, value );
}

static HRESULT WINAPI visual_put_Description( IUserActivityVisualElements *iface, HSTRING value )
{
    return set_string( &impl_from_IUserActivityVisualElements( iface )->description, value );
}

static HRESULT WINAPI visual_get_BackgroundColor( IUserActivityVisualElements *iface, Color *value )
{
    *value = impl_from_IUserActivityVisualElements( iface )->color;
    return S_OK;
}

static HRESULT WINAPI visual_put_BackgroundColor( IUserActivityVisualElements *iface, Color value )
{
    impl_from_IUserActivityVisualElements( iface )->color = value;
    return S_OK;
}

static HRESULT WINAPI visual_get_Attribution( IUserActivityVisualElements *iface, IUserActivityAttribution **value )
{
    struct visual_elements *impl = impl_from_IUserActivityVisualElements( iface );

    if ((*value = impl->attribution)) IUserActivityAttribution_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI visual_put_Attribution( IUserActivityVisualElements *iface, IUserActivityAttribution *value )
{
    struct visual_elements *impl = impl_from_IUserActivityVisualElements( iface );

    if (value) IUserActivityAttribution_AddRef( value );
    if (impl->attribution) IUserActivityAttribution_Release( impl->attribution );
    impl->attribution = value;
    return S_OK;
}

static HRESULT WINAPI visual_put_Content( IUserActivityVisualElements *iface, IAdaptiveCard *value )
{
    struct visual_elements *impl = impl_from_IUserActivityVisualElements( iface );

    if (value) IAdaptiveCard_AddRef( value );
    if (impl->content) IAdaptiveCard_Release( impl->content );
    impl->content = value;
    return S_OK;
}

static HRESULT WINAPI visual_get_Content( IUserActivityVisualElements *iface, IAdaptiveCard **value )
{
    struct visual_elements *impl = impl_from_IUserActivityVisualElements( iface );

    if ((*value = impl->content)) IAdaptiveCard_AddRef( *value );
    return S_OK;
}

static const IUserActivityVisualElementsVtbl visual_vtbl =
{
    visual_QueryInterface,
    visual_AddRef,
    visual_Release,
    /* IInspectable methods */
    visual_GetIids,
    visual_GetRuntimeClassName,
    visual_GetTrustLevel,
    /* IUserActivityVisualElements methods */
    visual_get_DisplayText,
    visual_put_DisplayText,
    visual_get_Description,
    visual_put_Description,
    visual_get_BackgroundColor,
    visual_put_BackgroundColor,
    visual_get_Attribution,
    visual_put_Attribution,
    visual_put_Content,
    visual_get_Content,
};

DEFINE_IINSPECTABLE( visual2, IUserActivityVisualElements2, struct visual_elements, IUserActivityVisualElements_iface )

static HRESULT WINAPI visual2_get_AttributionDisplayText( IUserActivityVisualElements2 *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUserActivityVisualElements2( iface )->attribution_text, value );
}

static HRESULT WINAPI visual2_put_AttributionDisplayText( IUserActivityVisualElements2 *iface, HSTRING value )
{
    return set_string( &impl_from_IUserActivityVisualElements2( iface )->attribution_text, value );
}

static const IUserActivityVisualElements2Vtbl visual2_vtbl =
{
    visual2_QueryInterface,
    visual2_AddRef,
    visual2_Release,
    /* IInspectable methods */
    visual2_GetIids,
    visual2_GetRuntimeClassName,
    visual2_GetTrustLevel,
    /* IUserActivityVisualElements2 methods */
    visual2_get_AttributionDisplayText,
    visual2_put_AttributionDisplayText,
};

static HRESULT visual_elements_create( IUserActivityVisualElements **out )
{
    struct visual_elements *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUserActivityVisualElements_iface.lpVtbl = &visual_vtbl;
    impl->IUserActivityVisualElements2_iface.lpVtbl = &visual2_vtbl;
    impl->ref = 1;
    *out = &impl->IUserActivityVisualElements_iface;
    return S_OK;
}

/* ---- UserActivitySession ---- */

struct session
{
    IUserActivitySession IUserActivitySession_iface;
    IClosable IClosable_iface;
    LONG ref;
    HSTRING activity_id;
};

static inline struct session *impl_from_IUserActivitySession( IUserActivitySession *iface )
{
    return CONTAINING_RECORD( iface, struct session, IUserActivitySession_iface );
}

static HRESULT WINAPI session_QueryInterface( IUserActivitySession *iface, REFIID iid, void **out )
{
    struct session *impl = impl_from_IUserActivitySession( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IUserActivitySession ))
        *out = &impl->IUserActivitySession_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI session_AddRef( IUserActivitySession *iface )
{
    return InterlockedIncrement( &impl_from_IUserActivitySession( iface )->ref );
}

static ULONG WINAPI session_Release( IUserActivitySession *iface )
{
    struct session *impl = impl_from_IUserActivitySession( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->activity_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI session_GetIids( IUserActivitySession *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IUserActivitySession;
    list[1] = IID_IClosable;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI session_GetRuntimeClassName( IUserActivitySession *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.UserActivities.UserActivitySession", name );
}

static HRESULT WINAPI session_GetTrustLevel( IUserActivitySession *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI session_get_ActivityId( IUserActivitySession *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUserActivitySession( iface )->activity_id, value );
}

static const IUserActivitySessionVtbl session_vtbl =
{
    session_QueryInterface,
    session_AddRef,
    session_Release,
    /* IInspectable methods */
    session_GetIids,
    session_GetRuntimeClassName,
    session_GetTrustLevel,
    /* IUserActivitySession methods */
    session_get_ActivityId,
};

DEFINE_IINSPECTABLE( session_closable, IClosable, struct session, IUserActivitySession_iface )

static HRESULT WINAPI session_closable_Close( IClosable *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static const IClosableVtbl session_closable_vtbl =
{
    session_closable_QueryInterface,
    session_closable_AddRef,
    session_closable_Release,
    /* IInspectable methods */
    session_closable_GetIids,
    session_closable_GetRuntimeClassName,
    session_closable_GetTrustLevel,
    /* IClosable methods */
    session_closable_Close,
};

static HRESULT session_create( HSTRING activity_id, IUserActivitySession **out )
{
    struct session *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUserActivitySession_iface.lpVtbl = &session_vtbl;
    impl->IClosable_iface.lpVtbl = &session_closable_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( activity_id, &impl->activity_id )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IUserActivitySession_iface;
    return S_OK;
}

/* ---- UserActivity ---- */

struct user_activity
{
    IUserActivity IUserActivity_iface;
    IUserActivity2 IUserActivity2_iface;
    IUserActivity3 IUserActivity3_iface;
    LONG ref;
    HSTRING activity_id;
    UserActivityState state;
    IUserActivityVisualElements *visual_elements;
    IUriRuntimeClass *content_uri;
    IUriRuntimeClass *fallback_uri;
    IUriRuntimeClass *activation_uri;
    HSTRING content_type;
    IUserActivityContentInfo *content_info;
    boolean roamable;
};

static inline struct user_activity *impl_from_IUserActivity( IUserActivity *iface )
{
    return CONTAINING_RECORD( iface, struct user_activity, IUserActivity_iface );
}

static HRESULT WINAPI activity_QueryInterface( IUserActivity *iface, REFIID iid, void **out )
{
    struct user_activity *impl = impl_from_IUserActivity( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IUserActivity ))
        *out = &impl->IUserActivity_iface;
    else if (IsEqualGUID( iid, &IID_IUserActivity2 ))
        *out = &impl->IUserActivity2_iface;
    else if (IsEqualGUID( iid, &IID_IUserActivity3 ))
        *out = &impl->IUserActivity3_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI activity_AddRef( IUserActivity *iface )
{
    return InterlockedIncrement( &impl_from_IUserActivity( iface )->ref );
}

static ULONG WINAPI activity_Release( IUserActivity *iface )
{
    struct user_activity *impl = impl_from_IUserActivity( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->activity_id );
        WindowsDeleteString( impl->content_type );
        if (impl->visual_elements) IUserActivityVisualElements_Release( impl->visual_elements );
        if (impl->content_uri) IUriRuntimeClass_Release( impl->content_uri );
        if (impl->fallback_uri) IUriRuntimeClass_Release( impl->fallback_uri );
        if (impl->activation_uri) IUriRuntimeClass_Release( impl->activation_uri );
        if (impl->content_info) IUserActivityContentInfo_Release( impl->content_info );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI activity_GetIids( IUserActivity *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 3 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IUserActivity;
    list[1] = IID_IUserActivity2;
    list[2] = IID_IUserActivity3;
    *count = 3;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI activity_GetRuntimeClassName( IUserActivity *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.UserActivities.UserActivity", name );
}

static HRESULT WINAPI activity_GetTrustLevel( IUserActivity *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI activity_get_State( IUserActivity *iface, UserActivityState *value )
{
    *value = impl_from_IUserActivity( iface )->state;
    return S_OK;
}

static HRESULT WINAPI activity_get_ActivityId( IUserActivity *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUserActivity( iface )->activity_id, value );
}

static HRESULT WINAPI activity_get_VisualElements( IUserActivity *iface, IUserActivityVisualElements **value )
{
    struct user_activity *impl = impl_from_IUserActivity( iface );

    IUserActivityVisualElements_AddRef( (*value = impl->visual_elements) );
    return S_OK;
}

static HRESULT WINAPI activity_get_ContentUri( IUserActivity *iface, IUriRuntimeClass **value )
{
    return get_uri( impl_from_IUserActivity( iface )->content_uri, value );
}

static HRESULT WINAPI activity_put_ContentUri( IUserActivity *iface, IUriRuntimeClass *value )
{
    set_uri( &impl_from_IUserActivity( iface )->content_uri, value );
    return S_OK;
}

static HRESULT WINAPI activity_get_ContentType( IUserActivity *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUserActivity( iface )->content_type, value );
}

static HRESULT WINAPI activity_put_ContentType( IUserActivity *iface, HSTRING value )
{
    return set_string( &impl_from_IUserActivity( iface )->content_type, value );
}

static HRESULT WINAPI activity_get_FallbackUri( IUserActivity *iface, IUriRuntimeClass **value )
{
    return get_uri( impl_from_IUserActivity( iface )->fallback_uri, value );
}

static HRESULT WINAPI activity_put_FallbackUri( IUserActivity *iface, IUriRuntimeClass *value )
{
    set_uri( &impl_from_IUserActivity( iface )->fallback_uri, value );
    return S_OK;
}

static HRESULT WINAPI activity_get_ActivationUri( IUserActivity *iface, IUriRuntimeClass **value )
{
    return get_uri( impl_from_IUserActivity( iface )->activation_uri, value );
}

static HRESULT WINAPI activity_put_ActivationUri( IUserActivity *iface, IUriRuntimeClass *value )
{
    set_uri( &impl_from_IUserActivity( iface )->activation_uri, value );
    return S_OK;
}

static HRESULT WINAPI activity_get_ContentInfo( IUserActivity *iface, IUserActivityContentInfo **value )
{
    struct user_activity *impl = impl_from_IUserActivity( iface );

    if ((*value = impl->content_info)) IUserActivityContentInfo_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI activity_put_ContentInfo( IUserActivity *iface, IUserActivityContentInfo *value )
{
    struct user_activity *impl = impl_from_IUserActivity( iface );

    if (value) IUserActivityContentInfo_AddRef( value );
    if (impl->content_info) IUserActivityContentInfo_Release( impl->content_info );
    impl->content_info = value;
    return S_OK;
}

/* ---- the activities saved by the process ---- */

static CRITICAL_SECTION saved_cs;
static CRITICAL_SECTION_DEBUG saved_cs_debug =
{
    0, 0, &saved_cs,
    { &saved_cs_debug.ProcessLocksList, &saved_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": saved_cs") }
};
static CRITICAL_SECTION saved_cs = { &saved_cs_debug, -1, 0, 0, 0, 0 };
static IUserActivity **saved;
static UINT32 saved_count;

static BOOL id_equals( HSTRING a, HSTRING b )
{
    INT32 order = 1;

    WindowsCompareStringOrdinal( a, b, &order );
    return !order;
}

/* the index of the saved activity with this id, or saved_count; the lock is held */
static UINT32 saved_find( HSTRING id )
{
    UINT32 i;

    for (i = 0; i < saved_count; i++)
    {
        HSTRING other;
        BOOL same;

        if (FAILED(IUserActivity_get_ActivityId( saved[i], &other ))) continue;
        same = id_equals( other, id );
        WindowsDeleteString( other );
        if (same) break;
    }
    return i;
}

static void saved_remove_at( UINT32 index )
{
    IUserActivity_Release( saved[index] );
    memmove( saved + index, saved + index + 1, (saved_count - index - 1) * sizeof(*saved) );
    saved_count--;
}

static HRESULT saved_store( IUserActivity *activity )
{
    HSTRING id;
    UINT32 index;
    HRESULT hr;

    if (FAILED(hr = IUserActivity_get_ActivityId( activity, &id ))) return hr;
    EnterCriticalSection( &saved_cs );
    index = saved_find( id );
    if (index < saved_count)
    {
        IUserActivity_AddRef( activity );
        IUserActivity_Release( saved[index] );
        saved[index] = activity;
        hr = S_OK;
    }
    else
    {
        IUserActivity **list = realloc( saved, (saved_count + 1) * sizeof(*saved) );

        if (!list) hr = E_OUTOFMEMORY;
        else
        {
            saved = list;
            IUserActivity_AddRef( activity );
            saved[saved_count++] = activity;
            hr = S_OK;
        }
    }
    LeaveCriticalSection( &saved_cs );
    WindowsDeleteString( id );
    return hr;
}

static HRESULT WINAPI activity_SaveAsync( IUserActivity *iface, IAsyncAction **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    /* measured on Windows 11: the state stays New after SaveAsync, and the channel gives a new activity (State New) for
     * the id again; only a session publishes the activity (see CreateSession) */
    return async_action_completed( S_OK, operation );
}

/* Windows 11 failed CreateSession with E_ILLEGAL_METHOD_CALL for a saved activity on the machine it was measured on
 * (the activity history may have been off); here a session publishes the activity, which the channel then returns. */
static HRESULT WINAPI activity_CreateSession( IUserActivity *iface, IUserActivitySession **result )
{
    struct user_activity *impl = impl_from_IUserActivity( iface );
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (FAILED(hr = session_create( impl->activity_id, result ))) return hr;
    if (FAILED(hr = saved_store( iface )))
    {
        IUserActivitySession_Release( *result );
        *result = NULL;
        return hr;
    }
    impl->state = UserActivityState_Published;
    return S_OK;
}

static const IUserActivityVtbl activity_vtbl =
{
    activity_QueryInterface,
    activity_AddRef,
    activity_Release,
    /* IInspectable methods */
    activity_GetIids,
    activity_GetRuntimeClassName,
    activity_GetTrustLevel,
    /* IUserActivity methods */
    activity_get_State,
    activity_get_ActivityId,
    activity_get_VisualElements,
    activity_get_ContentUri,
    activity_put_ContentUri,
    activity_get_ContentType,
    activity_put_ContentType,
    activity_get_FallbackUri,
    activity_put_FallbackUri,
    activity_get_ActivationUri,
    activity_put_ActivationUri,
    activity_get_ContentInfo,
    activity_put_ContentInfo,
    activity_SaveAsync,
    activity_CreateSession,
};

DEFINE_IINSPECTABLE( activity2, IUserActivity2, struct user_activity, IUserActivity_iface )

static HRESULT WINAPI activity2_ToJson( IUserActivity2 *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p: the JSON form is not implemented.\n", iface, result );
    *result = NULL;
    return E_NOTIMPL;
}

static const IUserActivity2Vtbl activity2_vtbl =
{
    activity2_QueryInterface,
    activity2_AddRef,
    activity2_Release,
    /* IInspectable methods */
    activity2_GetIids,
    activity2_GetRuntimeClassName,
    activity2_GetTrustLevel,
    /* IUserActivity2 methods */
    activity2_ToJson,
};

DEFINE_IINSPECTABLE( activity3, IUserActivity3, struct user_activity, IUserActivity_iface )

static HRESULT WINAPI activity3_get_IsRoamable( IUserActivity3 *iface, boolean *value )
{
    *value = impl_from_IUserActivity3( iface )->roamable;
    return S_OK;
}

static HRESULT WINAPI activity3_put_IsRoamable( IUserActivity3 *iface, boolean value )
{
    impl_from_IUserActivity3( iface )->roamable = value;
    return S_OK;
}

static const IUserActivity3Vtbl activity3_vtbl =
{
    activity3_QueryInterface,
    activity3_AddRef,
    activity3_Release,
    /* IInspectable methods */
    activity3_GetIids,
    activity3_GetRuntimeClassName,
    activity3_GetTrustLevel,
    /* IUserActivity3 methods */
    activity3_get_IsRoamable,
    activity3_put_IsRoamable,
};

static HRESULT activity_create( HSTRING activity_id, IUserActivity **out )
{
    struct user_activity *impl;
    HRESULT hr;

    *out = NULL;
    if (!WindowsGetStringLen( activity_id )) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUserActivity_iface.lpVtbl = &activity_vtbl;
    impl->IUserActivity2_iface.lpVtbl = &activity2_vtbl;
    impl->IUserActivity3_iface.lpVtbl = &activity3_vtbl;
    impl->ref = 1;
    impl->state = UserActivityState_New;
    if (FAILED(hr = WindowsDuplicateString( activity_id, &impl->activity_id )) ||
        FAILED(hr = visual_elements_create( &impl->visual_elements )))
    {
        IUserActivity_Release( &impl->IUserActivity_iface );
        return hr;
    }
    *out = &impl->IUserActivity_iface;
    return S_OK;
}

static HRESULT WINAPI activity_statics_CreateWithActivityId( IUserActivityFactory *iface, HSTRING activity_id,
                                                             IUserActivity **value )
{
    TRACE( "iface %p, activity_id %s, value %p.\n", iface, debugstr_hstring( activity_id ), value );
    return activity_create( activity_id, value );
}

DEFINE_STATICS_IINSPECTABLE( activity_factory_iface, IUserActivityFactory )

static const IUserActivityFactoryVtbl activity_factory_iface_vtbl =
{
    activity_factory_iface_QueryInterface,
    activity_factory_iface_AddRef,
    activity_factory_iface_Release,
    /* IInspectable methods */
    activity_factory_iface_GetIids,
    activity_factory_iface_GetRuntimeClassName,
    activity_factory_iface_GetTrustLevel,
    /* IUserActivityFactory methods */
    activity_statics_CreateWithActivityId,
};

static HRESULT WINAPI activity_statics_TryParseFromJson( IUserActivityStatics *iface, HSTRING json, IUserActivity **result )
{
    FIXME( "iface %p, json %s, result %p: the JSON form is not implemented.\n", iface, debugstr_hstring( json ), result );
    *result = NULL;
    return S_OK;
}

static HRESULT WINAPI activity_statics_TryParseFromJsonArray( IUserActivityStatics *iface, HSTRING json,
                                                              IVector_UserActivity **result )
{
    FIXME( "iface %p, json %s, result %p: the JSON form is not implemented.\n", iface, debugstr_hstring( json ), result );
    *result = NULL;
    return S_OK;
}

static HRESULT WINAPI activity_statics_ToJsonArray( IUserActivityStatics *iface, IIterable_UserActivity *activities,
                                                    HSTRING *result )
{
    FIXME( "iface %p, activities %p, result %p: the JSON form is not implemented.\n", iface, activities, result );
    *result = NULL;
    return E_NOTIMPL;
}

DEFINE_STATICS_IINSPECTABLE( activity_statics, IUserActivityStatics )

static const IUserActivityStaticsVtbl activity_statics_vtbl =
{
    activity_statics_QueryInterface,
    activity_statics_AddRef,
    activity_statics_Release,
    /* IInspectable methods */
    activity_statics_GetIids,
    activity_statics_GetRuntimeClassName,
    activity_statics_GetTrustLevel,
    /* IUserActivityStatics methods */
    activity_statics_TryParseFromJson,
    activity_statics_TryParseFromJsonArray,
    activity_statics_ToJsonArray,
};

static struct static_factory activity_factory =
{
    {&static_factory_vtbl},
    1,
    L"Windows.ApplicationModel.UserActivities.UserActivity",
    NULL,
    2,
    {
        { &IID_IUserActivityFactory, { &activity_factory_iface_vtbl, &activity_factory } },
        { &IID_IUserActivityStatics, { &activity_statics_vtbl, &activity_factory } },
    },
};

IActivationFactory *user_activity_factory = &activity_factory.IActivationFactory_iface;

/* ---- UserActivityChannel ---- */

struct channel
{
    IUserActivityChannel IUserActivityChannel_iface;
    LONG ref;
};

static inline struct channel *impl_from_IUserActivityChannel( IUserActivityChannel *iface )
{
    return CONTAINING_RECORD( iface, struct channel, IUserActivityChannel_iface );
}

static HRESULT WINAPI channel_QueryInterface( IUserActivityChannel *iface, REFIID iid, void **out )
{
    struct channel *impl = impl_from_IUserActivityChannel( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IUserActivityChannel ))
    {
        IInspectable_AddRef( (*out = &impl->IUserActivityChannel_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI channel_AddRef( IUserActivityChannel *iface )
{
    return InterlockedIncrement( &impl_from_IUserActivityChannel( iface )->ref );
}

static ULONG WINAPI channel_Release( IUserActivityChannel *iface )
{
    return InterlockedDecrement( &impl_from_IUserActivityChannel( iface )->ref );
}

static HRESULT WINAPI channel_GetIids( IUserActivityChannel *iface, ULONG *count, IID **iids )
{
    return single_iid( &IID_IUserActivityChannel, count, iids );
}

static HRESULT WINAPI channel_GetRuntimeClassName( IUserActivityChannel *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.UserActivities.UserActivityChannel", name );
}

static HRESULT WINAPI channel_GetTrustLevel( IUserActivityChannel *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI channel_GetOrCreateUserActivityAsync( IUserActivityChannel *iface, HSTRING activity_id,
                                                            IAsyncOperation_UserActivity **operation )
{
    IUserActivity *activity = NULL;
    UINT32 index;
    HRESULT hr;

    TRACE( "iface %p, activity_id %s, operation %p.\n", iface, debugstr_hstring( activity_id ), operation );

    if (!WindowsGetStringLen( activity_id )) return E_INVALIDARG;
    EnterCriticalSection( &saved_cs );
    if ((index = saved_find( activity_id )) < saved_count) IUserActivity_AddRef( (activity = saved[index]) );
    LeaveCriticalSection( &saved_cs );

    if (!activity && FAILED(hr = activity_create( activity_id, &activity ))) return hr;
    hr = async_user_activity_completed( activity, S_OK, operation );
    IUserActivity_Release( activity );
    return hr;
}

static HRESULT WINAPI channel_DeleteActivityAsync( IUserActivityChannel *iface, HSTRING activity_id, IAsyncAction **operation )
{
    UINT32 index;

    TRACE( "iface %p, activity_id %s, operation %p.\n", iface, debugstr_hstring( activity_id ), operation );

    EnterCriticalSection( &saved_cs );
    if ((index = saved_find( activity_id )) < saved_count) saved_remove_at( index );
    LeaveCriticalSection( &saved_cs );
    return async_action_completed( S_OK, operation );
}

static HRESULT WINAPI channel_DeleteAllActivitiesAsync( IUserActivityChannel *iface, IAsyncAction **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    EnterCriticalSection( &saved_cs );
    while (saved_count) saved_remove_at( saved_count - 1 );
    LeaveCriticalSection( &saved_cs );
    return async_action_completed( S_OK, operation );
}

static const IUserActivityChannelVtbl channel_vtbl =
{
    channel_QueryInterface,
    channel_AddRef,
    channel_Release,
    /* IInspectable methods */
    channel_GetIids,
    channel_GetRuntimeClassName,
    channel_GetTrustLevel,
    /* IUserActivityChannel methods */
    channel_GetOrCreateUserActivityAsync,
    channel_DeleteActivityAsync,
    channel_DeleteAllActivitiesAsync,
};

/* one channel serves every user: there is one user and no account to separate activities by */
static struct channel default_channel = { {&channel_vtbl}, 1 };

static HRESULT WINAPI channel_statics_GetDefault( IUserActivityChannelStatics *iface, IUserActivityChannel **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    IUserActivityChannel_AddRef( (*result = &default_channel.IUserActivityChannel_iface) );
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( channel_statics, IUserActivityChannelStatics )

static const IUserActivityChannelStaticsVtbl channel_statics_vtbl =
{
    channel_statics_QueryInterface,
    channel_statics_AddRef,
    channel_statics_Release,
    /* IInspectable methods */
    channel_statics_GetIids,
    channel_statics_GetRuntimeClassName,
    channel_statics_GetTrustLevel,
    /* IUserActivityChannelStatics methods */
    channel_statics_GetDefault,
};

static HRESULT WINAPI channel_statics2_DisableAutoSessionCreation( IUserActivityChannelStatics2 *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

/* no web account has a channel here */
static HRESULT WINAPI channel_statics2_TryGetForWebAccount( IUserActivityChannelStatics2 *iface, IWebAccount *account,
                                                            IUserActivityChannel **result )
{
    TRACE( "iface %p, account %p, result %p.\n", iface, account, result );
    *result = NULL;
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( channel_statics2, IUserActivityChannelStatics2 )

static const IUserActivityChannelStatics2Vtbl channel_statics2_vtbl =
{
    channel_statics2_QueryInterface,
    channel_statics2_AddRef,
    channel_statics2_Release,
    /* IInspectable methods */
    channel_statics2_GetIids,
    channel_statics2_GetRuntimeClassName,
    channel_statics2_GetTrustLevel,
    /* IUserActivityChannelStatics2 methods */
    channel_statics2_DisableAutoSessionCreation,
    channel_statics2_TryGetForWebAccount,
};

static HRESULT WINAPI channel_statics3_GetForUser( IUserActivityChannelStatics3 *iface, IUser *user,
                                                   IUserActivityChannel **result )
{
    TRACE( "iface %p, user %p, result %p.\n", iface, user, result );
    if (!user) return E_INVALIDARG;
    IUserActivityChannel_AddRef( (*result = &default_channel.IUserActivityChannel_iface) );
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( channel_statics3, IUserActivityChannelStatics3 )

static const IUserActivityChannelStatics3Vtbl channel_statics3_vtbl =
{
    channel_statics3_QueryInterface,
    channel_statics3_AddRef,
    channel_statics3_Release,
    /* IInspectable methods */
    channel_statics3_GetIids,
    channel_statics3_GetRuntimeClassName,
    channel_statics3_GetTrustLevel,
    /* IUserActivityChannelStatics3 methods */
    channel_statics3_GetForUser,
};

static struct static_factory channel_factory =
{
    {&static_factory_vtbl},
    1,
    L"Windows.ApplicationModel.UserActivities.UserActivityChannel",
    NULL,
    3,
    {
        { &IID_IUserActivityChannelStatics, { &channel_statics_vtbl, &channel_factory } },
        { &IID_IUserActivityChannelStatics2, { &channel_statics2_vtbl, &channel_factory } },
        { &IID_IUserActivityChannelStatics3, { &channel_statics3_vtbl, &channel_factory } },
    },
};

IActivationFactory *user_activity_channel_factory = &channel_factory.IActivationFactory_iface;
