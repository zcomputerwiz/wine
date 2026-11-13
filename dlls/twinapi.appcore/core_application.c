/* WinRT Windows.ApplicationModel.Core.CoreApplication implementation
 *
 * Copyright 2025 Zhiyi Zhang for CodeWeavers
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
#include "appmodel.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/*
 * CoreApplication.Run for a packaged process (WineExplorer UWP compatibility, U1.1). It runs the view of the
 * IFrameworkViewSource on the calling thread, which becomes the thread of the view, of its CoreWindow and of
 * the dispatcher of that window:
 *
 *   CreateView, Initialize(view), SetWindow(window), Load(entry point), Run, Uninitialize
 *
 * The launch activation of the view is queued first and raised when the thread processes events. A process
 * without a package context gets an error: only a packaged process is an application.
 * Not implemented: presentation and input (the window has no native window), suspend and resume, more than one
 * view, the entry point string, the application properties.
 */

struct core_application
{
    IActivationFactory IActivationFactory_iface;
    ICoreApplication ICoreApplication_iface;
    ICoreApplicationExit ICoreApplicationExit_iface;
    ICoreImmersiveApplication ICoreImmersiveApplication_iface;
    LONG ref;

    BOOL running;
    struct list suspending_handlers, resuming_handlers, exiting_handlers;
};

static CRITICAL_SECTION app_cs;
static CRITICAL_SECTION_DEBUG app_cs_debug =
{
    0, 0, &app_cs,
    { &app_cs_debug.ProcessLocksList, &app_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": app_cs") }
};
static CRITICAL_SECTION app_cs = { &app_cs_debug, -1, 0, 0, 0, 0 };

static inline struct core_application *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct core_application, IActivationFactory_iface );
}

static HRESULT WINAPI activation_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct core_application *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ICoreApplication ))
        *out = &impl->ICoreApplication_iface;
    else if (IsEqualGUID( iid, &IID_ICoreApplicationExit ))
        *out = &impl->ICoreApplicationExit_iface;
    else if (IsEqualGUID( iid, &IID_ICoreImmersiveApplication ))
        *out = &impl->ICoreImmersiveApplication_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI activation_factory_AddRef( IActivationFactory *iface )
{
    struct core_application *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI activation_factory_Release( IActivationFactory *iface )
{
    struct core_application *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI activation_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
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

DEFINE_IINSPECTABLE( core_application, ICoreApplication, struct core_application, IActivationFactory_iface )
DEFINE_IINSPECTABLE( core_application_exit, ICoreApplicationExit, struct core_application, IActivationFactory_iface )
DEFINE_IINSPECTABLE( core_immersive, ICoreImmersiveApplication, struct core_application, IActivationFactory_iface )

/* the command line after the program name, as ActivateApplication was given it */
static const WCHAR *launch_arguments(void)
{
    const WCHAR *p = GetCommandLineW();

    if (*p == '"')
    {
        for (p++; *p && *p != '"'; p++) {}
        if (*p) p++;
    }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* the application id of this process, "" if it has none */
static BOOL get_application_id( WCHAR *id, UINT32 size )
{
    WCHAR aumid[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1 + 65];
    UINT32 len = ARRAY_SIZE(aumid);
    WCHAR *bang;

    if (GetCurrentApplicationUserModelId( &len, aumid ) || !(bang = wcschr( aumid, '!' ))) return FALSE;
    if (wcslen( bang + 1 ) >= size) return FALSE;
    wcscpy( id, bang + 1 );
    return TRUE;
}

static BOOL is_packaged(void)
{
    UINT32 len = 0;

    return GetCurrentPackageFullName( &len, NULL ) == ERROR_INSUFFICIENT_BUFFER;
}

static HRESULT WINAPI core_application_get_Id( ICoreApplication *iface, HSTRING *value )
{
    WCHAR id[65];

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!get_application_id( id, ARRAY_SIZE(id) )) return HRESULT_FROM_WIN32( APPMODEL_ERROR_NO_APPLICATION );
    return WindowsCreateString( id, wcslen( id ), value );
}

/* Suspending, Resuming and Exiting are accepted; suspend and resume are not implemented, so they are not raised. */
static HRESULT WINAPI core_application_add_Suspending( ICoreApplication *iface,
        IEventHandler_SuspendingEventArgs *handler, EventRegistrationToken *token )
{
    struct core_application *impl = impl_from_ICoreApplication( iface );

    FIXME( "iface %p, handler %p, token %p: never raised.\n", iface, handler, token );
    return handler_list_add( &impl->suspending_handlers, &app_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application_remove_Suspending( ICoreApplication *iface, EventRegistrationToken token )
{
    struct core_application *impl = impl_from_ICoreApplication( iface );
    return handler_list_remove( &impl->suspending_handlers, &app_cs, token );
}

static HRESULT WINAPI core_application_add_Resuming( ICoreApplication *iface, IEventHandler_IInspectable *handler,
                                                     EventRegistrationToken *token )
{
    struct core_application *impl = impl_from_ICoreApplication( iface );

    FIXME( "iface %p, handler %p, token %p: never raised.\n", iface, handler, token );
    return handler_list_add( &impl->resuming_handlers, &app_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application_remove_Resuming( ICoreApplication *iface, EventRegistrationToken token )
{
    struct core_application *impl = impl_from_ICoreApplication( iface );
    return handler_list_remove( &impl->resuming_handlers, &app_cs, token );
}

static HRESULT WINAPI core_application_get_Properties( ICoreApplication *iface, IPropertySet **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI core_application_GetCurrentView( ICoreApplication *iface, ICoreApplicationView **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!(*value = core_view_for_current_thread())) return E_ILLEGAL_METHOD_CALL;
    return S_OK;
}

static HRESULT WINAPI core_application_Run( ICoreApplication *iface, IFrameworkViewSource *source )
{
    struct core_application *impl = impl_from_ICoreApplication( iface );
    HRESULT (WINAPI *create_window)( ICoreWindow **window );
    void (WINAPI *detach_window)( ICoreWindow *window );
    ICoreApplicationView *view = NULL;
    IFrameworkView *framework = NULL;
    ICoreWindow *window = NULL;
    WCHAR id[65];
    HMODULE ui;
    HRESULT hr, hr_run;

    TRACE( "iface %p, source %p.\n", iface, source );

    if (!source) return E_INVALIDARG;
    if (!is_packaged()) return HRESULT_FROM_WIN32( APPMODEL_ERROR_NO_PACKAGE );

    if (!(ui = LoadLibraryW( L"windows.ui.dll" )) ||
        !(create_window = (void *)GetProcAddress( ui, "WineUICreateCoreWindow" )) ||
        !(detach_window = (void *)GetProcAddress( ui, "WineUIDetachCoreWindow" )))
    {
        ERR( "Cannot find the window creation of windows.ui.\n" );
        return E_NOTIMPL;
    }

    EnterCriticalSection( &app_cs );
    if (impl->running) hr = E_ILLEGAL_METHOD_CALL;
    else
    {
        impl->running = TRUE;
        hr = S_OK;
    }
    LeaveCriticalSection( &app_cs );
    if (FAILED(hr)) return hr;

    if (FAILED(hr = IFrameworkViewSource_CreateView( source, &framework ))) goto done;
    if (!framework)
    {
        hr = E_POINTER;
        goto done;
    }
    if (FAILED(hr = create_window( &window ))) goto done;
    if (FAILED(hr = core_view_create( window, TRUE, &view ))) goto done;

    /* the launch activation, raised when the thread processes events */
    if (!get_application_id( id, ARRAY_SIZE(id) )) id[0] = 0;
    if (FAILED(hr = core_view_queue_launch( view, launch_arguments(), id ))) goto done;

    if (FAILED(hr = IFrameworkView_Initialize( framework, view ))) goto uninitialize;
    if (FAILED(hr = IFrameworkView_SetWindow( framework, window ))) goto uninitialize;
    if (FAILED(hr = IFrameworkView_Load( framework, NULL ))) goto uninitialize;
    hr = IFrameworkView_Run( framework );

uninitialize:
    hr_run = hr;
    hr = IFrameworkView_Uninitialize( framework );
    if (SUCCEEDED(hr_run)) hr_run = hr;
    hr = hr_run;

done:
    if (view)
    {
        core_view_detach( view );
        ICoreApplicationView_Release( view );
    }
    if (window)
    {
        detach_window( window );
        ICoreWindow_Release( window );
    }
    if (framework) IFrameworkView_Release( framework );
    EnterCriticalSection( &app_cs );
    impl->running = FALSE;
    LeaveCriticalSection( &app_cs );
    return hr;
}

static HRESULT WINAPI core_application_RunWithActivationFactories( ICoreApplication *iface, IGetActivationFactory *factory )
{
    FIXME( "iface %p, factory %p stub!\n", iface, factory );
    return E_NOTIMPL;
}

static const struct ICoreApplicationVtbl core_application_vtbl =
{
    core_application_QueryInterface,
    core_application_AddRef,
    core_application_Release,
    /* IInspectable methods */
    core_application_GetIids,
    core_application_GetRuntimeClassName,
    core_application_GetTrustLevel,
    /* ICoreApplication methods */
    core_application_get_Id,
    core_application_add_Suspending,
    core_application_remove_Suspending,
    core_application_add_Resuming,
    core_application_remove_Resuming,
    core_application_get_Properties,
    core_application_GetCurrentView,
    core_application_Run,
    core_application_RunWithActivationFactories,
};

static void close_current_window( void *context )
{
    ICoreApplicationView *view = core_view_for_current_thread();
    ICoreWindow *window;

    if (!view) return;
    if (SUCCEEDED(ICoreApplicationView_get_CoreWindow( view, &window )))
    {
        ICoreWindow_Close( window );
        ICoreWindow_Release( window );
    }
    ICoreApplicationView_Release( view );
}

/* Exit raises Exiting, then closes the windows of the views, which ends their Run. */
static HRESULT WINAPI core_application_exit_Exit( ICoreApplicationExit *iface )
{
    struct core_application *impl = impl_from_ICoreApplicationExit( iface );
    IUnknown **handlers;
    unsigned int i, count;

    TRACE( "iface %p.\n", iface );

    handlers = handler_list_snapshot( &impl->exiting_handlers, &app_cs, &count );
    for (i = 0; i < count; i++)
        IEventHandler_IInspectable_Invoke( (IEventHandler_IInspectable *)handlers[i], NULL, NULL );
    handler_list_snapshot_free( handlers, count );

    core_view_for_each( close_current_window, NULL );
    return S_OK;
}

static HRESULT WINAPI core_application_exit_add_Exiting( ICoreApplicationExit *iface,
        IEventHandler_IInspectable *handler, EventRegistrationToken *token )
{
    struct core_application *impl = impl_from_ICoreApplicationExit( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return handler_list_add( &impl->exiting_handlers, &app_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application_exit_remove_Exiting( ICoreApplicationExit *iface, EventRegistrationToken token )
{
    struct core_application *impl = impl_from_ICoreApplicationExit( iface );
    return handler_list_remove( &impl->exiting_handlers, &app_cs, token );
}

static const struct ICoreApplicationExitVtbl core_application_exit_vtbl =
{
    core_application_exit_QueryInterface,
    core_application_exit_AddRef,
    core_application_exit_Release,
    /* IInspectable methods */
    core_application_exit_GetIids,
    core_application_exit_GetRuntimeClassName,
    core_application_exit_GetTrustLevel,
    /* ICoreApplicationExit methods */
    core_application_exit_Exit,
    core_application_exit_add_Exiting,
    core_application_exit_remove_Exiting,
};

static HRESULT WINAPI core_immersive_get_Views( ICoreImmersiveApplication *iface,
                                                IVectorView_CoreApplicationView **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI core_immersive_CreateNewView( ICoreImmersiveApplication *iface, HSTRING runtime_type,
                                                    HSTRING entry_point, ICoreApplicationView **view )
{
    FIXME( "iface %p, runtime_type %p, entry_point %p, view %p stub!\n", iface, runtime_type, entry_point, view );
    *view = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI core_immersive_get_MainView( ICoreImmersiveApplication *iface, ICoreApplicationView **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!(*value = core_view_main())) return E_ILLEGAL_METHOD_CALL;
    return S_OK;
}

static const struct ICoreImmersiveApplicationVtbl core_immersive_vtbl =
{
    core_immersive_QueryInterface,
    core_immersive_AddRef,
    core_immersive_Release,
    /* IInspectable methods */
    core_immersive_GetIids,
    core_immersive_GetRuntimeClassName,
    core_immersive_GetTrustLevel,
    /* ICoreImmersiveApplication methods */
    core_immersive_get_Views,
    core_immersive_CreateNewView,
    core_immersive_get_MainView,
};

static struct core_application core_application =
{
    {&activation_factory_vtbl},
    {&core_application_vtbl},
    {&core_application_exit_vtbl},
    {&core_immersive_vtbl},
    1,
    FALSE,
    { &core_application.suspending_handlers, &core_application.suspending_handlers },
    { &core_application.resuming_handlers, &core_application.resuming_handlers },
    { &core_application.exiting_handlers, &core_application.exiting_handlers },
};

IActivationFactory *core_application_factory = &core_application.IActivationFactory_iface;
