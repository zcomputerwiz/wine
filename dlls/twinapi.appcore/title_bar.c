/* WinRT title bars of a view (WineExplorer UWP compatibility)
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

/*
 * CoreApplicationView.TitleBar (CoreApplicationViewTitleBar) and ApplicationView.TitleBar (ApplicationViewTitleBar).
 *
 * The CoreWindow keeps its standard frame: a view is never extended into the title bar. ExtendViewIntoTitleBar is
 * kept as set; the metrics are those Windows 11 gives before the window is shown (height 0, no insets, not visible),
 * and LayoutMetricsChanged and IsVisibleChanged are accepted and never raised, as on Windows for a view that is not
 * extended. An application drawing its own title bar from Height (the Calculator) then gives it no height.
 * ApplicationViewTitleBar keeps the colours set (null until set); the frame does not use them.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/* ---- CoreApplicationViewTitleBar ---- */

struct core_title_bar
{
    ICoreApplicationViewTitleBar ICoreApplicationViewTitleBar_iface;
    LONG ref;
    boolean extend;
    LONG token;
};

static inline struct core_title_bar *impl_from_ICoreApplicationViewTitleBar( ICoreApplicationViewTitleBar *iface )
{
    return CONTAINING_RECORD( iface, struct core_title_bar, ICoreApplicationViewTitleBar_iface );
}

static HRESULT WINAPI core_title_bar_QueryInterface( ICoreApplicationViewTitleBar *iface, REFIID iid, void **out )
{
    struct core_title_bar *impl = impl_from_ICoreApplicationViewTitleBar( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ICoreApplicationViewTitleBar ))
    {
        ICoreApplicationViewTitleBar_AddRef( (*out = &impl->ICoreApplicationViewTitleBar_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI core_title_bar_AddRef( ICoreApplicationViewTitleBar *iface )
{
    return InterlockedIncrement( &impl_from_ICoreApplicationViewTitleBar( iface )->ref );
}

static ULONG WINAPI core_title_bar_Release( ICoreApplicationViewTitleBar *iface )
{
    struct core_title_bar *impl = impl_from_ICoreApplicationViewTitleBar( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI core_title_bar_GetIids( ICoreApplicationViewTitleBar *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI core_title_bar_GetRuntimeClassName( ICoreApplicationViewTitleBar *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.ApplicationModel.Core.CoreApplicationViewTitleBar";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI core_title_bar_GetTrustLevel( ICoreApplicationViewTitleBar *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI core_title_bar_put_ExtendViewIntoTitleBar( ICoreApplicationViewTitleBar *iface, boolean value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    impl_from_ICoreApplicationViewTitleBar( iface )->extend = !!value;
    return S_OK;
}

static HRESULT WINAPI core_title_bar_get_ExtendViewIntoTitleBar( ICoreApplicationViewTitleBar *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl_from_ICoreApplicationViewTitleBar( iface )->extend;
    return S_OK;
}

static HRESULT WINAPI core_title_bar_get_SystemOverlayLeftInset( ICoreApplicationViewTitleBar *iface, DOUBLE *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI core_title_bar_get_SystemOverlayRightInset( ICoreApplicationViewTitleBar *iface, DOUBLE *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI core_title_bar_get_Height( ICoreApplicationViewTitleBar *iface, DOUBLE *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI core_title_bar_add_LayoutMetricsChanged( ICoreApplicationViewTitleBar *iface,
        ITypedEventHandler_CoreApplicationViewTitleBar_IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p: never raised.\n", iface, handler, token );
    if (!handler || !token) return E_INVALIDARG;
    token->value = InterlockedIncrement( &impl_from_ICoreApplicationViewTitleBar( iface )->token );
    return S_OK;
}

static HRESULT WINAPI core_title_bar_remove_LayoutMetricsChanged( ICoreApplicationViewTitleBar *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI core_title_bar_get_IsVisible( ICoreApplicationViewTitleBar *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI core_title_bar_add_IsVisibleChanged( ICoreApplicationViewTitleBar *iface,
        ITypedEventHandler_CoreApplicationViewTitleBar_IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p: never raised.\n", iface, handler, token );
    if (!handler || !token) return E_INVALIDARG;
    token->value = InterlockedIncrement( &impl_from_ICoreApplicationViewTitleBar( iface )->token );
    return S_OK;
}

static HRESULT WINAPI core_title_bar_remove_IsVisibleChanged( ICoreApplicationViewTitleBar *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const ICoreApplicationViewTitleBarVtbl core_title_bar_vtbl =
{
    core_title_bar_QueryInterface,
    core_title_bar_AddRef,
    core_title_bar_Release,
    /* IInspectable methods */
    core_title_bar_GetIids,
    core_title_bar_GetRuntimeClassName,
    core_title_bar_GetTrustLevel,
    /* ICoreApplicationViewTitleBar methods */
    core_title_bar_put_ExtendViewIntoTitleBar,
    core_title_bar_get_ExtendViewIntoTitleBar,
    core_title_bar_get_SystemOverlayLeftInset,
    core_title_bar_get_SystemOverlayRightInset,
    core_title_bar_get_Height,
    core_title_bar_add_LayoutMetricsChanged,
    core_title_bar_remove_LayoutMetricsChanged,
    core_title_bar_get_IsVisible,
    core_title_bar_add_IsVisibleChanged,
    core_title_bar_remove_IsVisibleChanged,
};

HRESULT core_title_bar_create( ICoreApplicationViewTitleBar **out )
{
    struct core_title_bar *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICoreApplicationViewTitleBar_iface.lpVtbl = &core_title_bar_vtbl;
    impl->ref = 1;
    *out = &impl->ICoreApplicationViewTitleBar_iface;
    return S_OK;
}

/* ---- ApplicationViewTitleBar ---- */

enum title_bar_color
{
    FOREGROUND, BACKGROUND, BUTTON_FOREGROUND, BUTTON_BACKGROUND, BUTTON_HOVER_FOREGROUND, BUTTON_HOVER_BACKGROUND,
    BUTTON_PRESSED_FOREGROUND, BUTTON_PRESSED_BACKGROUND, INACTIVE_FOREGROUND, INACTIVE_BACKGROUND,
    BUTTON_INACTIVE_FOREGROUND, BUTTON_INACTIVE_BACKGROUND, TITLE_BAR_COLOR_COUNT
};

struct view_title_bar
{
    IApplicationViewTitleBar IApplicationViewTitleBar_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    IReference_Color *colors[TITLE_BAR_COLOR_COUNT];
};

static inline struct view_title_bar *impl_from_IApplicationViewTitleBar( IApplicationViewTitleBar *iface )
{
    return CONTAINING_RECORD( iface, struct view_title_bar, IApplicationViewTitleBar_iface );
}

static HRESULT WINAPI view_title_bar_QueryInterface( IApplicationViewTitleBar *iface, REFIID iid, void **out )
{
    struct view_title_bar *impl = impl_from_IApplicationViewTitleBar( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IApplicationViewTitleBar ))
    {
        IApplicationViewTitleBar_AddRef( (*out = &impl->IApplicationViewTitleBar_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI view_title_bar_AddRef( IApplicationViewTitleBar *iface )
{
    return InterlockedIncrement( &impl_from_IApplicationViewTitleBar( iface )->ref );
}

static ULONG WINAPI view_title_bar_Release( IApplicationViewTitleBar *iface )
{
    struct view_title_bar *impl = impl_from_IApplicationViewTitleBar( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    unsigned int i;

    if (!ref)
    {
        for (i = 0; i < TITLE_BAR_COLOR_COUNT; i++)
            if (impl->colors[i]) IReference_Color_Release( impl->colors[i] );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI view_title_bar_GetIids( IApplicationViewTitleBar *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI view_title_bar_GetRuntimeClassName( IApplicationViewTitleBar *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.UI.ViewManagement.ApplicationViewTitleBar";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI view_title_bar_GetTrustLevel( IApplicationViewTitleBar *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT put_color( IApplicationViewTitleBar *iface, enum title_bar_color which, IReference_Color *value )
{
    struct view_title_bar *impl = impl_from_IApplicationViewTitleBar( iface );
    IReference_Color *old;

    TRACE( "iface %p, color %u, value %p.\n", iface, which, value );
    if (value) IReference_Color_AddRef( value );
    EnterCriticalSection( &impl->cs );
    old = impl->colors[which];
    impl->colors[which] = value;
    LeaveCriticalSection( &impl->cs );
    if (old) IReference_Color_Release( old );
    return S_OK;
}

static HRESULT get_color( IApplicationViewTitleBar *iface, enum title_bar_color which, IReference_Color **value )
{
    struct view_title_bar *impl = impl_from_IApplicationViewTitleBar( iface );

    TRACE( "iface %p, color %u, value %p.\n", iface, which, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    if ((*value = impl->colors[which])) IReference_Color_AddRef( *value );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

#define TITLE_BAR_COLOR( name, which )                                                                       \
    static HRESULT WINAPI view_title_bar_put_##name( IApplicationViewTitleBar *iface, IReference_Color *value ) \
    {                                                                                                       \
        return put_color( iface, which, value );                                                            \
    }                                                                                                       \
    static HRESULT WINAPI view_title_bar_get_##name( IApplicationViewTitleBar *iface, IReference_Color **value ) \
    {                                                                                                       \
        return get_color( iface, which, value );                                                            \
    }

TITLE_BAR_COLOR( ForegroundColor, FOREGROUND )
TITLE_BAR_COLOR( BackgroundColor, BACKGROUND )
TITLE_BAR_COLOR( ButtonForegroundColor, BUTTON_FOREGROUND )
TITLE_BAR_COLOR( ButtonBackgroundColor, BUTTON_BACKGROUND )
TITLE_BAR_COLOR( ButtonHoverForegroundColor, BUTTON_HOVER_FOREGROUND )
TITLE_BAR_COLOR( ButtonHoverBackgroundColor, BUTTON_HOVER_BACKGROUND )
TITLE_BAR_COLOR( ButtonPressedForegroundColor, BUTTON_PRESSED_FOREGROUND )
TITLE_BAR_COLOR( ButtonPressedBackgroundColor, BUTTON_PRESSED_BACKGROUND )
TITLE_BAR_COLOR( InactiveForegroundColor, INACTIVE_FOREGROUND )
TITLE_BAR_COLOR( InactiveBackgroundColor, INACTIVE_BACKGROUND )
TITLE_BAR_COLOR( ButtonInactiveForegroundColor, BUTTON_INACTIVE_FOREGROUND )
TITLE_BAR_COLOR( ButtonInactiveBackgroundColor, BUTTON_INACTIVE_BACKGROUND )

static const IApplicationViewTitleBarVtbl view_title_bar_vtbl =
{
    view_title_bar_QueryInterface,
    view_title_bar_AddRef,
    view_title_bar_Release,
    /* IInspectable methods */
    view_title_bar_GetIids,
    view_title_bar_GetRuntimeClassName,
    view_title_bar_GetTrustLevel,
    /* IApplicationViewTitleBar methods */
    view_title_bar_put_ForegroundColor,
    view_title_bar_get_ForegroundColor,
    view_title_bar_put_BackgroundColor,
    view_title_bar_get_BackgroundColor,
    view_title_bar_put_ButtonForegroundColor,
    view_title_bar_get_ButtonForegroundColor,
    view_title_bar_put_ButtonBackgroundColor,
    view_title_bar_get_ButtonBackgroundColor,
    view_title_bar_put_ButtonHoverForegroundColor,
    view_title_bar_get_ButtonHoverForegroundColor,
    view_title_bar_put_ButtonHoverBackgroundColor,
    view_title_bar_get_ButtonHoverBackgroundColor,
    view_title_bar_put_ButtonPressedForegroundColor,
    view_title_bar_get_ButtonPressedForegroundColor,
    view_title_bar_put_ButtonPressedBackgroundColor,
    view_title_bar_get_ButtonPressedBackgroundColor,
    view_title_bar_put_InactiveForegroundColor,
    view_title_bar_get_InactiveForegroundColor,
    view_title_bar_put_InactiveBackgroundColor,
    view_title_bar_get_InactiveBackgroundColor,
    view_title_bar_put_ButtonInactiveForegroundColor,
    view_title_bar_get_ButtonInactiveForegroundColor,
    view_title_bar_put_ButtonInactiveBackgroundColor,
    view_title_bar_get_ButtonInactiveBackgroundColor,
};

HRESULT view_title_bar_create( IApplicationViewTitleBar **out )
{
    struct view_title_bar *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IApplicationViewTitleBar_iface.lpVtbl = &view_title_bar_vtbl;
    impl->ref = 1;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": view_title_bar.cs");
    *out = &impl->IApplicationViewTitleBar_iface;
    return S_OK;
}
