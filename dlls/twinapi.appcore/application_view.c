/* WinRT Windows.UI.ViewManagement.ApplicationView implementation
 *
 * Copyright 2025 Zhiyi Zhang for CodeWeavers
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
#include "initguid.h"
#include "windows.ui.core.corewindow.interop.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct factory
{
    IActivationFactory IActivationFactory_iface;
    IApplicationViewStatics IApplicationViewStatics_iface;
    IApplicationViewStatics2 IApplicationViewStatics2_iface;
    IApplicationViewStatics3 IApplicationViewStatics3_iface;
    IApplicationViewInteropStatics IApplicationViewInteropStatics_iface;
    LONG ref;
};

static inline struct factory *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct factory, IActivationFactory_iface);
}

static HRESULT WINAPI activation_factory_QueryInterface(IActivationFactory *iface, REFIID iid,
                                                        void **out)
{
    struct factory *impl = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
        || IsEqualGUID(iid, &IID_IInspectable)
        || IsEqualGUID(iid, &IID_IAgileObject)
        || IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IActivationFactory_AddRef(&impl->IActivationFactory_iface);
        *out = &impl->IActivationFactory_iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IApplicationViewStatics))
    {
        IApplicationViewStatics_AddRef(&impl->IApplicationViewStatics_iface);
        *out = &impl->IApplicationViewStatics_iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IApplicationViewStatics2))
    {
        IApplicationViewStatics2_AddRef(&impl->IApplicationViewStatics2_iface);
        *out = &impl->IApplicationViewStatics2_iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IApplicationViewStatics3))
    {
        IApplicationViewStatics3_AddRef(&impl->IApplicationViewStatics3_iface);
        *out = &impl->IApplicationViewStatics3_iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IApplicationViewInteropStatics))
    {
        IApplicationViewInteropStatics_AddRef(&impl->IApplicationViewInteropStatics_iface);
        *out = &impl->IApplicationViewInteropStatics_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI activation_factory_AddRef(IActivationFactory *iface)
{
    struct factory *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG WINAPI activation_factory_Release(IActivationFactory *iface)
{
    struct factory *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static HRESULT WINAPI activation_factory_GetIids(IActivationFactory *iface, ULONG *iid_count,
                                                 IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetRuntimeClassName(IActivationFactory *iface,
                                                             HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetTrustLevel(IActivationFactory *iface,
                                                       TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_ActivateInstance(IActivationFactory *iface,
                                                          IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl activation_factory_vtbl =
{
    activation_factory_QueryInterface,
    activation_factory_AddRef,
    activation_factory_Release,
    /* IInspectable methods */
    activation_factory_GetIids,
    activation_factory_GetRuntimeClassName,
    activation_factory_GetTrustLevel,
    /* IActivationFactory methods */
    activation_factory_ActivateInstance,
};

/* ---- the ApplicationView of a CoreApplicationView (WineExplorer UWP compatibility) ----------------------------
 *
 * One per view thread, made by GetForCurrentView. As on Windows 11 (tests/uwp/xaml/cx-probe, LaunchProbe.cpp): the
 * same object for every call on the view's thread, an empty title, not full screen, SetPreferredMinSize accepted and
 * TryResizeView succeeding before the window is shown. TryResizeView sizes the CoreWindow's client area. */

struct application_view
{
    IApplicationView IApplicationView_iface;
    IApplicationView2 IApplicationView2_iface;
    IApplicationView3 IApplicationView3_iface;
    IApplicationView4 IApplicationView4_iface;
    LONG ref;
    ApplicationViewMode view_mode;
    struct list entry;
    DWORD thread_id;
    INT32 id;
    HSTRING title;
    boolean screen_capture;
    Size min_size;
    IApplicationViewTitleBar *title_bar;    /* made on first use, kept with the view */
};

static CRITICAL_SECTION application_views_cs;
static CRITICAL_SECTION_DEBUG application_views_cs_debug =
{
    0, 0, &application_views_cs,
    { &application_views_cs_debug.ProcessLocksList, &application_views_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": application_views_cs") }
};
static CRITICAL_SECTION application_views_cs = { &application_views_cs_debug, -1, 0, 0, 0, 0 };
static struct list application_views = LIST_INIT( application_views );
static INT32 next_view_id = 1;

static inline struct application_view *impl_from_IApplicationView( IApplicationView *iface )
{
    return CONTAINING_RECORD( iface, struct application_view, IApplicationView_iface );
}

static HRESULT WINAPI application_view_QueryInterface( IApplicationView *iface, REFIID iid, void **out )
{
    struct application_view *impl = impl_from_IApplicationView( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IApplicationView ))
        *out = &impl->IApplicationView_iface;
    else if (IsEqualGUID( iid, &IID_IApplicationView2 ))
        *out = &impl->IApplicationView2_iface;
    else if (IsEqualGUID( iid, &IID_IApplicationView3 ))
        *out = &impl->IApplicationView3_iface;
    else if (IsEqualGUID( iid, &IID_IApplicationView4 ))
        *out = &impl->IApplicationView4_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI application_view_AddRef( IApplicationView *iface )
{
    return InterlockedIncrement( &impl_from_IApplicationView( iface )->ref );
}

/* the views live as long as their thread's view does: the list keeps a reference */
static ULONG WINAPI application_view_Release( IApplicationView *iface )
{
    return InterlockedDecrement( &impl_from_IApplicationView( iface )->ref );
}

static HRESULT WINAPI application_view_GetIids( IApplicationView *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI application_view_GetRuntimeClassName( IApplicationView *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.UI.ViewManagement.ApplicationView", 41, class_name );
}

static HRESULT WINAPI application_view_GetTrustLevel( IApplicationView *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI application_view_get_Orientation( IApplicationView *iface, ApplicationViewOrientation *value )
{
    *value = ApplicationViewOrientation_Landscape;
    return S_OK;
}

static HRESULT WINAPI application_view_get_AdjacentToLeftDisplayEdge( IApplicationView *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI application_view_get_AdjacentToRightDisplayEdge( IApplicationView *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI application_view_get_IsFullScreen( IApplicationView *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI application_view_get_IsOnLockScreen( IApplicationView *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI application_view_get_IsScreenCaptureEnabled( IApplicationView *iface, boolean *value )
{
    *value = impl_from_IApplicationView( iface )->screen_capture;
    return S_OK;
}

static HRESULT WINAPI application_view_put_IsScreenCaptureEnabled( IApplicationView *iface, boolean value )
{
    impl_from_IApplicationView( iface )->screen_capture = value;
    return S_OK;
}

static HRESULT WINAPI application_view_put_Title( IApplicationView *iface, HSTRING value )
{
    struct application_view *impl = impl_from_IApplicationView( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->title );
    impl->title = copy;
    return S_OK;
}

static HRESULT WINAPI application_view_get_Title( IApplicationView *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IApplicationView( iface )->title, value );
}

static HRESULT WINAPI application_view_get_Id( IApplicationView *iface, INT32 *value )
{
    *value = impl_from_IApplicationView( iface )->id;
    return S_OK;
}

static HRESULT WINAPI application_view_add_Consolidated( IApplicationView *iface,
        ITypedEventHandler_ApplicationView_ApplicationViewConsolidatedEventArgs *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p semi-stub!\n", iface, handler, token );
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI application_view_remove_Consolidated( IApplicationView *iface, EventRegistrationToken token )
{
    return S_OK;
}

static const IApplicationViewVtbl application_view_vtbl =
{
    application_view_QueryInterface,
    application_view_AddRef,
    application_view_Release,
    /* IInspectable methods */
    application_view_GetIids,
    application_view_GetRuntimeClassName,
    application_view_GetTrustLevel,
    /* IApplicationView methods */
    application_view_get_Orientation,
    application_view_get_AdjacentToLeftDisplayEdge,
    application_view_get_AdjacentToRightDisplayEdge,
    application_view_get_IsFullScreen,
    application_view_get_IsOnLockScreen,
    application_view_get_IsScreenCaptureEnabled,
    application_view_put_IsScreenCaptureEnabled,
    application_view_put_Title,
    application_view_get_Title,
    application_view_get_Id,
    application_view_add_Consolidated,
    application_view_remove_Consolidated,
};

DEFINE_IINSPECTABLE( application_view2, IApplicationView2, struct application_view, IApplicationView_iface )

static HRESULT WINAPI application_view2_get_SuppressSystemOverlays( IApplicationView2 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI application_view2_put_SuppressSystemOverlays( IApplicationView2 *iface, boolean value )
{
    return S_OK;
}

/* the window handle of the calling thread's CoreWindow, or NULL */
static HWND current_core_hwnd(void)
{
    ICoreApplicationView *view = core_view_for_current_thread();
    ICoreWindowInterop *interop;
    ICoreWindow *window = NULL;
    HWND hwnd = NULL;

    if (!view) return NULL;
    if (SUCCEEDED(ICoreApplicationView_get_CoreWindow( view, &window )) && window &&
        SUCCEEDED(ICoreWindow_QueryInterface( window, &IID_ICoreWindowInterop, (void **)&interop )))
    {
        ICoreWindowInterop_get_WindowHandle( interop, &hwnd );
        ICoreWindowInterop_Release( interop );
    }
    if (window) ICoreWindow_Release( window );
    ICoreApplicationView_Release( view );
    return hwnd;
}

static HRESULT WINAPI application_view2_get_VisibleBounds( IApplicationView2 *iface, Rect *value )
{
    HWND hwnd = current_core_hwnd();
    RECT rect = { 0 };

    if (hwnd) GetClientRect( hwnd, &rect );
    value->X = 0;
    value->Y = 0;
    value->Width = rect.right;
    value->Height = rect.bottom;
    return S_OK;
}

static HRESULT WINAPI application_view2_add_VisibleBoundsChanged( IApplicationView2 *iface,
        ITypedEventHandler_ApplicationView_IInspectable *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p semi-stub!\n", iface, handler, token );
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI application_view2_remove_VisibleBoundsChanged( IApplicationView2 *iface, EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI application_view2_SetDesiredBoundsMode( IApplicationView2 *iface, ApplicationViewBoundsMode mode,
                                                              boolean *success )
{
    *success = mode == ApplicationViewBoundsMode_UseVisible;
    return S_OK;
}

static HRESULT WINAPI application_view2_get_DesiredBoundsMode( IApplicationView2 *iface, ApplicationViewBoundsMode *value )
{
    *value = ApplicationViewBoundsMode_UseVisible;
    return S_OK;
}

static const IApplicationView2Vtbl application_view2_vtbl =
{
    application_view2_QueryInterface,
    application_view2_AddRef,
    application_view2_Release,
    /* IInspectable methods */
    application_view2_GetIids,
    application_view2_GetRuntimeClassName,
    application_view2_GetTrustLevel,
    /* IApplicationView2 methods */
    application_view2_get_SuppressSystemOverlays,
    application_view2_put_SuppressSystemOverlays,
    application_view2_get_VisibleBounds,
    application_view2_add_VisibleBoundsChanged,
    application_view2_remove_VisibleBoundsChanged,
    application_view2_SetDesiredBoundsMode,
    application_view2_get_DesiredBoundsMode,
};

DEFINE_IINSPECTABLE( application_view3, IApplicationView3, struct application_view, IApplicationView_iface )

static HRESULT WINAPI application_view3_get_TitleBar( IApplicationView3 *iface, IApplicationViewTitleBar **value )
{
    struct application_view *impl = impl_from_IApplicationView3( iface );
    IApplicationViewTitleBar *bar = NULL;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (!impl->title_bar)
    {
        if (FAILED(hr = view_title_bar_create( &bar ))) return hr;
        if (InterlockedCompareExchangePointer( (void **)&impl->title_bar, bar, NULL ))
            IApplicationViewTitleBar_Release( bar );
    }
    IApplicationViewTitleBar_AddRef( (*value = impl->title_bar) );
    return S_OK;
}

static HRESULT WINAPI application_view3_get_FullScreenSystemOverlayMode( IApplicationView3 *iface, FullScreenSystemOverlayMode *value )
{
    *value = FullScreenSystemOverlayMode_Standard;
    return S_OK;
}

static HRESULT WINAPI application_view3_put_FullScreenSystemOverlayMode( IApplicationView3 *iface, FullScreenSystemOverlayMode value )
{
    return S_OK;
}

static HRESULT WINAPI application_view3_get_IsFullScreenMode( IApplicationView3 *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI application_view3_TryEnterFullScreenMode( IApplicationView3 *iface, boolean *success )
{
    FIXME( "iface %p, success %p stub!\n", iface, success );
    *success = FALSE;
    return S_OK;
}

static HRESULT WINAPI application_view3_ExitFullScreenMode( IApplicationView3 *iface )
{
    return S_OK;
}

static HRESULT WINAPI application_view3_ShowStandardSystemOverlays( IApplicationView3 *iface )
{
    return S_OK;
}

/* sizes the CoreWindow's client area (view pixels at 96 dpi) */
static HRESULT WINAPI application_view3_TryResizeView( IApplicationView3 *iface, Size value, boolean *success )
{
    HWND hwnd = current_core_hwnd();
    RECT rect = { 0, 0, (LONG)(value.Width + 0.5f), (LONG)(value.Height + 0.5f) };

    TRACE( "iface %p, size %.1fx%.1f, success %p.\n", iface, value.Width, value.Height, success );
    *success = FALSE;
    if (!hwnd) return S_OK;
    AdjustWindowRectEx( &rect, GetWindowLongW( hwnd, GWL_STYLE ), FALSE, GetWindowLongW( hwnd, GWL_EXSTYLE ) );
    *success = SetWindowPos( hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
    return S_OK;
}

static HRESULT WINAPI application_view3_SetPreferredMinSize( IApplicationView3 *iface, Size size )
{
    struct application_view *impl = impl_from_IApplicationView3( iface );

    TRACE( "iface %p, size %.1fx%.1f.\n", iface, size.Width, size.Height );
    impl->min_size = size;
    return S_OK;
}

static const IApplicationView3Vtbl application_view3_vtbl =
{
    application_view3_QueryInterface,
    application_view3_AddRef,
    application_view3_Release,
    /* IInspectable methods */
    application_view3_GetIids,
    application_view3_GetRuntimeClassName,
    application_view3_GetTrustLevel,
    /* IApplicationView3 methods */
    application_view3_get_TitleBar,
    application_view3_get_FullScreenSystemOverlayMode,
    application_view3_put_FullScreenSystemOverlayMode,
    application_view3_get_IsFullScreenMode,
    application_view3_TryEnterFullScreenMode,
    application_view3_ExitFullScreenMode,
    application_view3_ShowStandardSystemOverlays,
    application_view3_TryResizeView,
    application_view3_SetPreferredMinSize,
};

DEFINE_IINSPECTABLE( application_view4, IApplicationView4, struct application_view, IApplicationView_iface )

static HRESULT WINAPI application_view4_get_ViewMode( IApplicationView4 *iface, ApplicationViewMode *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl_from_IApplicationView4( iface )->view_mode;
    return S_OK;
}

/* as on Windows 11: the default mode and compact overlay; spanning needs a dual-screen device */
static HRESULT WINAPI application_view4_IsViewModeSupported( IApplicationView4 *iface, ApplicationViewMode mode, boolean *supported )
{
    TRACE( "iface %p, mode %d, supported %p.\n", iface, mode, supported );
    *supported = mode == ApplicationViewMode_Default || mode == ApplicationViewMode_CompactOverlay;
    return S_OK;
}

/* compact overlay: the view's window stays above the others; the default mode: no longer */
static HRESULT WINAPI application_view4_TryEnterViewModeWithPreferencesAsync( IApplicationView4 *iface, ApplicationViewMode mode,
                                                                              IViewModePreferences *preferences,
                                                                              IAsyncOperation_boolean **operation )
{
    struct application_view *impl = impl_from_IApplicationView4( iface );
    HWND hwnd = current_core_hwnd();
    BOOL done = FALSE;

    TRACE( "iface %p, mode %d, preferences %p, operation %p.\n", iface, mode, preferences, operation );

    if (mode == ApplicationViewMode_Default || mode == ApplicationViewMode_CompactOverlay)
    {
        done = !hwnd || SetWindowPos( hwnd, mode == ApplicationViewMode_CompactOverlay ? HWND_TOPMOST : HWND_NOTOPMOST,
                                      0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
        if (done) impl->view_mode = mode;
    }
    return async_bool_completed( done, operation );
}

static HRESULT WINAPI application_view4_TryEnterViewModeAsync( IApplicationView4 *iface, ApplicationViewMode mode,
                                                               IAsyncOperation_boolean **operation )
{
    return application_view4_TryEnterViewModeWithPreferencesAsync( iface, mode, NULL, operation );
}

/* a view is not consolidated (closed in favour of another) on Wine */
static HRESULT WINAPI application_view4_TryConsolidateAsync( IApplicationView4 *iface, IAsyncOperation_boolean **operation )
{
    FIXME( "iface %p, operation %p semi-stub!\n", iface, operation );
    return async_bool_completed( FALSE, operation );
}

static const IApplicationView4Vtbl application_view4_vtbl =
{
    application_view4_QueryInterface,
    application_view4_AddRef,
    application_view4_Release,
    /* IInspectable methods */
    application_view4_GetIids,
    application_view4_GetRuntimeClassName,
    application_view4_GetTrustLevel,
    /* IApplicationView4 methods */
    application_view4_get_ViewMode,
    application_view4_IsViewModeSupported,
    application_view4_TryEnterViewModeAsync,
    application_view4_TryEnterViewModeWithPreferencesAsync,
    application_view4_TryConsolidateAsync,
};

/* the ApplicationView of the calling thread's CoreApplicationView, made on first use */
static HRESULT application_view_for_current_thread( IApplicationView **out )
{
    ICoreApplicationView *core = core_view_for_current_thread();
    struct application_view *view, *found = NULL;

    *out = NULL;
    if (!core) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    ICoreApplicationView_Release( core );

    EnterCriticalSection( &application_views_cs );
    LIST_FOR_EACH_ENTRY( view, &application_views, struct application_view, entry )
        if (view->thread_id == GetCurrentThreadId()) { found = view; break; }
    if (!found && (found = calloc( 1, sizeof(*found) )))
    {
        found->IApplicationView_iface.lpVtbl = &application_view_vtbl;
        found->IApplicationView2_iface.lpVtbl = &application_view2_vtbl;
        found->IApplicationView3_iface.lpVtbl = &application_view3_vtbl;
        found->IApplicationView4_iface.lpVtbl = &application_view4_vtbl;
        found->ref = 1;   /* the list's */
        found->thread_id = GetCurrentThreadId();
        found->id = next_view_id++;
        list_add_tail( &application_views, &found->entry );
    }
    if (found) IApplicationView_AddRef( (*out = &found->IApplicationView_iface) );
    LeaveCriticalSection( &application_views_cs );
    return found ? S_OK : E_OUTOFMEMORY;
}

DEFINE_IINSPECTABLE(statics, IApplicationViewStatics, struct factory, IActivationFactory_iface)

static HRESULT WINAPI statics_Value(IApplicationViewStatics *iface, ApplicationViewState *value)
{
    FIXME("iface %p, value %p stub!\n", iface, value);
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_TryUnsnap(IApplicationViewStatics *iface, boolean *success)
{
    FIXME("iface %p, success %p stub!\n", iface, success);
    return E_NOTIMPL;
}

static const struct IApplicationViewStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IApplicationViewStatics methods */
    statics_Value,
    statics_TryUnsnap,
};

DEFINE_IINSPECTABLE(statics2, IApplicationViewStatics2, struct factory, IActivationFactory_iface)

static HRESULT WINAPI statics2_GetForCurrentView(IApplicationViewStatics2 *iface, IApplicationView **current)
{
    TRACE("iface %p, current %p.\n", iface, current);
    return application_view_for_current_thread(current);   /* ERROR_NOT_FOUND off a view's thread, as before */
}

static HRESULT WINAPI statics2_get_TerminateAppOnFinalViewClose(IApplicationViewStatics2 *iface, boolean *value)
{
    FIXME("iface %p, value %p stub!\n", iface, value);
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_put_TerminateAppOnFinalViewClose(IApplicationViewStatics2 *iface, boolean value)
{
    FIXME("iface %p, value %d stub!\n", iface, value);
    return E_NOTIMPL;
}

static const struct IApplicationViewStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IApplicationViewStatics2 methods */
    statics2_GetForCurrentView,
    statics2_get_TerminateAppOnFinalViewClose,
    statics2_put_TerminateAppOnFinalViewClose
};

DEFINE_IINSPECTABLE(statics3, IApplicationViewStatics3, struct factory, IActivationFactory_iface)

/* the launch preferences: kept as given (Windows 11 starts with Auto and 0x0) */
static ApplicationViewWindowingMode launch_windowing_mode = ApplicationViewWindowingMode_Auto;
static Size launch_view_size;

static HRESULT WINAPI statics3_get_PreferredLaunchWindowingMode(IApplicationViewStatics3 *iface, ApplicationViewWindowingMode *value)
{
    *value = launch_windowing_mode;
    return S_OK;
}

static HRESULT WINAPI statics3_put_PreferredLaunchWindowingMode(IApplicationViewStatics3 *iface, ApplicationViewWindowingMode value)
{
    TRACE("iface %p, value %d.\n", iface, value);
    launch_windowing_mode = value;
    return S_OK;
}

static HRESULT WINAPI statics3_get_PreferredLaunchViewSize(IApplicationViewStatics3 *iface, Size *value)
{
    *value = launch_view_size;
    return S_OK;
}

static HRESULT WINAPI statics3_put_PreferredLaunchViewSize(IApplicationViewStatics3 *iface, Size value)
{
    launch_view_size = value;
    return S_OK;
}

static const struct IApplicationViewStatics3Vtbl statics3_vtbl =
{
    statics3_QueryInterface,
    statics3_AddRef,
    statics3_Release,
    /* IInspectable methods */
    statics3_GetIids,
    statics3_GetRuntimeClassName,
    statics3_GetTrustLevel,
    /* IApplicationViewStatics3 methods */
    statics3_get_PreferredLaunchWindowingMode,
    statics3_put_PreferredLaunchWindowingMode,
    statics3_get_PreferredLaunchViewSize,
    statics3_put_PreferredLaunchViewSize,
};

DEFINE_IINSPECTABLE(interop_statics, IApplicationViewInteropStatics, struct factory, IActivationFactory_iface)

/* the id of the ApplicationView of a CoreWindow's thread: the view GetForCurrentView gives on that thread */
static HRESULT WINAPI interop_statics_GetApplicationViewIdForWindow(IApplicationViewInteropStatics *iface,
                                                                    ICoreWindow *window, INT32 *id)
{
    struct application_view *view, *found = NULL;
    ICoreWindowInterop *interop;
    IApplicationView *current;
    DWORD thread = 0;
    HWND hwnd = 0;
    HRESULT hr;

    TRACE("iface %p, window %p, id %p.\n", iface, window, id);

    if (!window || !id) return E_INVALIDARG;
    if (SUCCEEDED(ICoreWindow_QueryInterface(window, &IID_ICoreWindowInterop, (void **)&interop)))
    {
        ICoreWindowInterop_get_WindowHandle(interop, &hwnd);
        ICoreWindowInterop_Release(interop);
    }
    if (hwnd) thread = GetWindowThreadProcessId(hwnd, NULL);
    if (!thread) return E_INVALIDARG;

    if (thread == GetCurrentThreadId())
    {
        if (FAILED(hr = application_view_for_current_thread(&current))) return hr;
        hr = IApplicationView_get_Id(current, id);
        IApplicationView_Release(current);
        return hr;
    }
    EnterCriticalSection(&application_views_cs);
    LIST_FOR_EACH_ENTRY(view, &application_views, struct application_view, entry)
        if (view->thread_id == thread) { found = view; break; }
    if (found) *id = found->id;
    LeaveCriticalSection(&application_views_cs);
    return found ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static const struct IApplicationViewInteropStaticsVtbl interop_statics_vtbl =
{
    interop_statics_QueryInterface,
    interop_statics_AddRef,
    interop_statics_Release,
    /* IInspectable methods */
    interop_statics_GetIids,
    interop_statics_GetRuntimeClassName,
    interop_statics_GetTrustLevel,
    /* IApplicationViewInteropStatics methods */
    interop_statics_GetApplicationViewIdForWindow,
};

static struct factory factory =
{
    {&activation_factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    {&statics3_vtbl},
    {&interop_statics_vtbl},
    1,
};

IActivationFactory *application_view_factory = &factory.IActivationFactory_iface;
