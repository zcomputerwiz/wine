/*
 * Pointer events of a CoreWindow (WineExplorer UWP compatibility, U1.1b)
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

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

/*
 * The arguments of PointerEntered, PointerExited, PointerMoved, PointerPressed, PointerReleased and
 * PointerWheelChanged for a mouse, as observed on Windows 11 (tests/uwp/u1_1/README.md): pointer id 1,
 * PointerDeviceType Mouse, positions in the client area of the window in DIPs.
 */

struct pointer_event
{
    IPointerEventArgs IPointerEventArgs_iface;
    ICoreWindowEventArgs ICoreWindowEventArgs_iface;
    IPointerPoint IPointerPoint_iface;
    IPointerPointProperties IPointerPointProperties_iface;
    IPointerDevice IPointerDevice_iface;
    LONG ref;
    BOOL handled;
    struct ui_pointer_state state;
};

static inline struct pointer_event *impl_from_IPointerEventArgs( IPointerEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct pointer_event, IPointerEventArgs_iface );
}

static inline struct pointer_event *impl_from_ICoreWindowEventArgs( ICoreWindowEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct pointer_event, ICoreWindowEventArgs_iface );
}

static inline struct pointer_event *impl_from_IPointerPoint( IPointerPoint *iface )
{
    return CONTAINING_RECORD( iface, struct pointer_event, IPointerPoint_iface );
}

static inline struct pointer_event *impl_from_IPointerPointProperties( IPointerPointProperties *iface )
{
    return CONTAINING_RECORD( iface, struct pointer_event, IPointerPointProperties_iface );
}

static inline struct pointer_event *impl_from_IPointerDevice( IPointerDevice *iface )
{
    return CONTAINING_RECORD( iface, struct pointer_event, IPointerDevice_iface );
}

/* one object holds the arguments, the point, its properties and its device; they share the reference count */
static HRESULT pointer_query( struct pointer_event *impl, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IPointerEventArgs ))
        *out = &impl->IPointerEventArgs_iface;
    else if (IsEqualGUID( iid, &IID_ICoreWindowEventArgs ))
        *out = &impl->ICoreWindowEventArgs_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG pointer_addref( struct pointer_event *impl )
{
    return InterlockedIncrement( &impl->ref );
}

static ULONG pointer_release( struct pointer_event *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT pointer_trust_level( TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* ---- PointerDevice ----------------------------------------------------------------------------------- */

static HRESULT WINAPI device_QueryInterface( IPointerDevice *iface, REFIID iid, void **out )
{
    struct pointer_event *impl = impl_from_IPointerDevice( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IPointerDevice ))
    {
        *out = iface;
        pointer_addref( impl );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI device_AddRef( IPointerDevice *iface )
{
    return pointer_addref( impl_from_IPointerDevice( iface ) );
}

static ULONG WINAPI device_Release( IPointerDevice *iface )
{
    return pointer_release( impl_from_IPointerDevice( iface ) );
}

static HRESULT WINAPI device_GetIids( IPointerDevice *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI device_GetRuntimeClassName( IPointerDevice *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Devices.Input.PointerDevice", wcslen( L"Windows.Devices.Input.PointerDevice" ), name );
}

static HRESULT WINAPI device_GetTrustLevel( IPointerDevice *iface, TrustLevel *level )
{
    return pointer_trust_level( level );
}

static HRESULT WINAPI device_get_PointerDeviceType( IPointerDevice *iface, PointerDeviceType *value )
{
    *value = PointerDeviceType_Mouse;
    return S_OK;
}

static HRESULT WINAPI device_get_IsIntegrated( IPointerDevice *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI device_get_MaxContacts( IPointerDevice *iface, UINT32 *value )
{
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI device_get_PhysicalDeviceRect( IPointerDevice *iface, Rect *value )
{
    memset( value, 0, sizeof(*value) );
    return S_OK;
}

static HRESULT WINAPI device_get_ScreenRect( IPointerDevice *iface, Rect *value )
{
    value->X = 0;
    value->Y = 0;
    value->Width = GetSystemMetrics( SM_CXSCREEN );
    value->Height = GetSystemMetrics( SM_CYSCREEN );
    return S_OK;
}

static HRESULT WINAPI device_get_SupportedUsages( IPointerDevice *iface, IVectorView_PointerDeviceUsage **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static const IPointerDeviceVtbl device_vtbl =
{
    device_QueryInterface,
    device_AddRef,
    device_Release,
    device_GetIids,
    device_GetRuntimeClassName,
    device_GetTrustLevel,
    device_get_PointerDeviceType,
    device_get_IsIntegrated,
    device_get_MaxContacts,
    device_get_PhysicalDeviceRect,
    device_get_ScreenRect,
    device_get_SupportedUsages,
};

/* ---- PointerPointProperties -------------------------------------------------------------------------- */

static HRESULT WINAPI props_QueryInterface( IPointerPointProperties *iface, REFIID iid, void **out )
{
    struct pointer_event *impl = impl_from_IPointerPointProperties( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IPointerPointProperties ))
    {
        *out = iface;
        pointer_addref( impl );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI props_AddRef( IPointerPointProperties *iface )
{
    return pointer_addref( impl_from_IPointerPointProperties( iface ) );
}

static ULONG WINAPI props_Release( IPointerPointProperties *iface )
{
    return pointer_release( impl_from_IPointerPointProperties( iface ) );
}

static HRESULT WINAPI props_GetIids( IPointerPointProperties *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI props_GetRuntimeClassName( IPointerPointProperties *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Input.PointerPointProperties", wcslen( L"Windows.UI.Input.PointerPointProperties" ), name );
}

static HRESULT WINAPI props_GetTrustLevel( IPointerPointProperties *iface, TrustLevel *level )
{
    return pointer_trust_level( level );
}

static HRESULT WINAPI props_get_Pressure( IPointerPointProperties *iface, FLOAT *value )
{
    struct pointer_event *impl = impl_from_IPointerPointProperties( iface );

    *value = (impl->state.left || impl->state.right || impl->state.middle) ? 0.5f : 0.0f;
    return S_OK;
}

#define PROPS_BOOL( name, expr )                                                                       \
    static HRESULT WINAPI props_get_##name( IPointerPointProperties *iface, boolean *value )           \
    {                                                                                                  \
        struct pointer_event *impl = impl_from_IPointerPointProperties( iface );                       \
        *value = (expr);                                                                               \
        return S_OK;                                                                                   \
    }

#define PROPS_FLOAT( name )                                                                            \
    static HRESULT WINAPI props_get_##name( IPointerPointProperties *iface, FLOAT *value )             \
    {                                                                                                  \
        *value = 0.0f;                                                                                 \
        return S_OK;                                                                                   \
    }

PROPS_BOOL( IsInverted, FALSE )
PROPS_BOOL( IsEraser, FALSE )
PROPS_FLOAT( Orientation )
PROPS_FLOAT( XTilt )
PROPS_FLOAT( YTilt )
PROPS_FLOAT( Twist )

static HRESULT WINAPI props_get_ContactRect( IPointerPointProperties *iface, Rect *value )
{
    struct pointer_event *impl = impl_from_IPointerPointProperties( iface );

    value->X = impl->state.x;
    value->Y = impl->state.y;
    value->Width = value->Height = 0;
    return S_OK;
}

PROPS_BOOL( TouchConfidence, TRUE )
PROPS_BOOL( IsLeftButtonPressed, impl->state.left )
PROPS_BOOL( IsRightButtonPressed, impl->state.right )
PROPS_BOOL( IsMiddleButtonPressed, impl->state.middle )

static HRESULT WINAPI props_get_MouseWheelDelta( IPointerPointProperties *iface, INT32 *value )
{
    *value = impl_from_IPointerPointProperties( iface )->state.wheel_delta;
    return S_OK;
}

PROPS_BOOL( IsHorizontalMouseWheel, impl->state.horizontal_wheel )
PROPS_BOOL( IsPrimary, TRUE )
PROPS_BOOL( IsInRange, TRUE )
PROPS_BOOL( IsCanceled, FALSE )
PROPS_BOOL( IsBarrelButtonPressed, FALSE )
PROPS_BOOL( IsXButton1Pressed, impl->state.x1 )
PROPS_BOOL( IsXButton2Pressed, impl->state.x2 )

static HRESULT WINAPI props_get_PointerUpdateKind( IPointerPointProperties *iface, PointerUpdateKind *value )
{
    *value = impl_from_IPointerPointProperties( iface )->state.update_kind;
    return S_OK;
}

static HRESULT WINAPI props_HasUsage( IPointerPointProperties *iface, UINT32 usage_page, UINT32 usage_id, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI props_GetUsageValue( IPointerPointProperties *iface, UINT32 usage_page, UINT32 usage_id, INT32 *value )
{
    *value = 0;
    return S_OK;
}

static const IPointerPointPropertiesVtbl props_vtbl =
{
    props_QueryInterface,
    props_AddRef,
    props_Release,
    props_GetIids,
    props_GetRuntimeClassName,
    props_GetTrustLevel,
    props_get_Pressure,
    props_get_IsInverted,
    props_get_IsEraser,
    props_get_Orientation,
    props_get_XTilt,
    props_get_YTilt,
    props_get_Twist,
    props_get_ContactRect,
    props_get_ContactRect,
    props_get_TouchConfidence,
    props_get_IsLeftButtonPressed,
    props_get_IsRightButtonPressed,
    props_get_IsMiddleButtonPressed,
    props_get_MouseWheelDelta,
    props_get_IsHorizontalMouseWheel,
    props_get_IsPrimary,
    props_get_IsInRange,
    props_get_IsCanceled,
    props_get_IsBarrelButtonPressed,
    props_get_IsXButton1Pressed,
    props_get_IsXButton2Pressed,
    props_get_PointerUpdateKind,
    props_HasUsage,
    props_GetUsageValue,
};

/* ---- PointerPoint ------------------------------------------------------------------------------------ */

static HRESULT WINAPI point_QueryInterface( IPointerPoint *iface, REFIID iid, void **out )
{
    struct pointer_event *impl = impl_from_IPointerPoint( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IPointerPoint ))
    {
        *out = iface;
        pointer_addref( impl );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI point_AddRef( IPointerPoint *iface )
{
    return pointer_addref( impl_from_IPointerPoint( iface ) );
}

static ULONG WINAPI point_Release( IPointerPoint *iface )
{
    return pointer_release( impl_from_IPointerPoint( iface ) );
}

static HRESULT WINAPI point_GetIids( IPointerPoint *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI point_GetRuntimeClassName( IPointerPoint *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Input.PointerPoint", wcslen( L"Windows.UI.Input.PointerPoint" ), name );
}

static HRESULT WINAPI point_GetTrustLevel( IPointerPoint *iface, TrustLevel *level )
{
    return pointer_trust_level( level );
}

static HRESULT WINAPI point_get_PointerDevice( IPointerPoint *iface, IPointerDevice **value )
{
    struct pointer_event *impl = impl_from_IPointerPoint( iface );

    IPointerDevice_AddRef( *value = &impl->IPointerDevice_iface );
    return S_OK;
}

static HRESULT WINAPI point_get_Position( IPointerPoint *iface, Point *value )
{
    struct pointer_event *impl = impl_from_IPointerPoint( iface );

    value->X = impl->state.x;
    value->Y = impl->state.y;
    return S_OK;
}

static HRESULT WINAPI point_get_pointer_id( IPointerPoint *iface, UINT32 *value )
{
    *value = impl_from_IPointerPoint( iface )->state.pointer_id;
    return S_OK;
}

static HRESULT WINAPI point_get_FrameId( IPointerPoint *iface, UINT32 *value )
{
    *value = impl_from_IPointerPoint( iface )->state.frame_id;
    return S_OK;
}

static HRESULT WINAPI point_get_Timestamp( IPointerPoint *iface, UINT64 *value )
{
    *value = impl_from_IPointerPoint( iface )->state.timestamp;
    return S_OK;
}

static HRESULT WINAPI point_get_IsInContact( IPointerPoint *iface, boolean *value )
{
    struct pointer_event *impl = impl_from_IPointerPoint( iface );

    *value = impl->state.left || impl->state.right || impl->state.middle || impl->state.x1 || impl->state.x2;
    return S_OK;
}

static HRESULT WINAPI point_get_Properties( IPointerPoint *iface, IPointerPointProperties **value )
{
    struct pointer_event *impl = impl_from_IPointerPoint( iface );

    IPointerPointProperties_AddRef( *value = &impl->IPointerPointProperties_iface );
    return S_OK;
}

static const IPointerPointVtbl point_vtbl =
{
    point_QueryInterface,
    point_AddRef,
    point_Release,
    point_GetIids,
    point_GetRuntimeClassName,
    point_GetTrustLevel,
    point_get_PointerDevice,
    point_get_Position,
    point_get_Position,
    point_get_pointer_id,
    point_get_FrameId,
    point_get_Timestamp,
    point_get_IsInContact,
    point_get_Properties,
};

/* ---- PointerEventArgs -------------------------------------------------------------------------------- */

static HRESULT WINAPI args_QueryInterface( IPointerEventArgs *iface, REFIID iid, void **out )
{
    return pointer_query( impl_from_IPointerEventArgs( iface ), iid, out );
}

static ULONG WINAPI args_AddRef( IPointerEventArgs *iface )
{
    return pointer_addref( impl_from_IPointerEventArgs( iface ) );
}

static ULONG WINAPI args_Release( IPointerEventArgs *iface )
{
    return pointer_release( impl_from_IPointerEventArgs( iface ) );
}

static HRESULT WINAPI args_GetIids( IPointerEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI args_GetRuntimeClassName( IPointerEventArgs *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.PointerEventArgs", wcslen( L"Windows.UI.Core.PointerEventArgs" ), name );
}

static HRESULT WINAPI args_GetTrustLevel( IPointerEventArgs *iface, TrustLevel *level )
{
    return pointer_trust_level( level );
}

static HRESULT WINAPI args_get_CurrentPoint( IPointerEventArgs *iface, IPointerPoint **value )
{
    struct pointer_event *impl = impl_from_IPointerEventArgs( iface );

    IPointerPoint_AddRef( *value = &impl->IPointerPoint_iface );
    return S_OK;
}

static HRESULT WINAPI args_get_KeyModifiers( IPointerEventArgs *iface, VirtualKeyModifiers *value )
{
    *value = impl_from_IPointerEventArgs( iface )->state.modifiers;
    return S_OK;
}

static HRESULT WINAPI args_GetIntermediatePoints( IPointerEventArgs *iface, IVector_PointerPoint **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static const IPointerEventArgsVtbl args_vtbl =
{
    args_QueryInterface,
    args_AddRef,
    args_Release,
    args_GetIids,
    args_GetRuntimeClassName,
    args_GetTrustLevel,
    args_get_CurrentPoint,
    args_get_KeyModifiers,
    args_GetIntermediatePoints,
};

static HRESULT WINAPI base_QueryInterface( ICoreWindowEventArgs *iface, REFIID iid, void **out )
{
    return pointer_query( impl_from_ICoreWindowEventArgs( iface ), iid, out );
}

static ULONG WINAPI base_AddRef( ICoreWindowEventArgs *iface )
{
    return pointer_addref( impl_from_ICoreWindowEventArgs( iface ) );
}

static ULONG WINAPI base_Release( ICoreWindowEventArgs *iface )
{
    return pointer_release( impl_from_ICoreWindowEventArgs( iface ) );
}

static HRESULT WINAPI base_GetIids( ICoreWindowEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI base_GetRuntimeClassName( ICoreWindowEventArgs *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.PointerEventArgs", wcslen( L"Windows.UI.Core.PointerEventArgs" ), name );
}

static HRESULT WINAPI base_GetTrustLevel( ICoreWindowEventArgs *iface, TrustLevel *level )
{
    return pointer_trust_level( level );
}

static HRESULT WINAPI base_get_Handled( ICoreWindowEventArgs *iface, boolean *value )
{
    *value = impl_from_ICoreWindowEventArgs( iface )->handled;
    return S_OK;
}

static HRESULT WINAPI base_put_Handled( ICoreWindowEventArgs *iface, boolean value )
{
    impl_from_ICoreWindowEventArgs( iface )->handled = value;
    return S_OK;
}

static const ICoreWindowEventArgsVtbl base_vtbl =
{
    base_QueryInterface,
    base_AddRef,
    base_Release,
    base_GetIids,
    base_GetRuntimeClassName,
    base_GetTrustLevel,
    base_get_Handled,
    base_put_Handled,
};

/* the arguments of one pointer event; the caller releases them */
IPointerEventArgs *ui_pointer_args_create( const struct ui_pointer_state *state )
{
    struct pointer_event *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IPointerEventArgs_iface.lpVtbl = &args_vtbl;
    impl->ICoreWindowEventArgs_iface.lpVtbl = &base_vtbl;
    impl->IPointerPoint_iface.lpVtbl = &point_vtbl;
    impl->IPointerPointProperties_iface.lpVtbl = &props_vtbl;
    impl->IPointerDevice_iface.lpVtbl = &device_vtbl;
    impl->ref = 1;
    impl->state = *state;
    return &impl->IPointerEventArgs_iface;
}

BOOL ui_pointer_args_handled( IPointerEventArgs *args )
{
    return impl_from_IPointerEventArgs( args )->handled;
}
