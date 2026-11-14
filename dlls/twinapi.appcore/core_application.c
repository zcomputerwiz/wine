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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "private.h"
#include "winternl.h"
#include "appmodel.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/*
 * CoreApplication.Run for a packaged process (WineExplorer UWP compatibility, U1.1), as observed on Windows 11
 * (tests/uwp/u1_1): the caller has to be in a multithreaded apartment (RPC_E_WRONG_THREAD otherwise). The view
 * runs on a thread of its own, which becomes the thread of its CoreWindow and of the dispatcher of that window:
 *
 *   CreateView, Initialize(view), SetWindow(window), Load(entry point of the manifest), launch activation,
 *   Run, Uninitialize
 *
 * The calling thread waits. When the view is finished (it returned from Run, or CoreApplication.Exit closed
 * its window) the calling thread raises Exiting, if Exit has not, and the process ends: Run does not return.
 * A view that fails in one of its steps makes Run return that error instead. A process without a package
 * context gets an error: only a packaged process is an application.
 * Not implemented: presentation and input (the window has no native window), suspend and resume, more than one
 * view, the application properties.
 */

struct run_state
{
    IFrameworkViewSource *source;
    DWORD main_thread;               /* the thread that called Run */
    HANDLE exit_request;             /* Exit asks the main thread to raise Exiting */
    HANDLE exiting_done;             /* the main thread has raised it */
    HRESULT hr;                      /* the first error of the view */
    BOOL failed;
    BOOL exiting_raised;
};

struct core_application
{
    IActivationFactory IActivationFactory_iface;
    ICoreApplication ICoreApplication_iface;
    ICoreApplicationExit ICoreApplicationExit_iface;
    ICoreImmersiveApplication ICoreImmersiveApplication_iface;
    ICoreApplication2 ICoreApplication2_iface;
    LONG ref;

    BOOL running;
    struct run_state *run;
    struct list exiting_handlers;
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
    else if (IsEqualGUID( iid, &IID_ICoreApplication2 ))
        *out = &impl->ICoreApplication2_iface;
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

/* Suspending and Resuming are raised by lifecycle.c when the main window is hidden and shown again */
static HRESULT WINAPI core_application_add_Suspending( ICoreApplication *iface,
        IEventHandler_SuspendingEventArgs *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return lifecycle_add( LIFECYCLE_SUSPENDING, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application_remove_Suspending( ICoreApplication *iface, EventRegistrationToken token )
{
    return lifecycle_remove( LIFECYCLE_SUSPENDING, token );
}

static HRESULT WINAPI core_application_add_Resuming( ICoreApplication *iface, IEventHandler_IInspectable *handler,
                                                     EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return lifecycle_add( LIFECYCLE_RESUMING, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application_remove_Resuming( ICoreApplication *iface, EventRegistrationToken token )
{
    return lifecycle_remove( LIFECYCLE_RESUMING, token );
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

/* the EntryPoint of the application in the manifest, as a new HSTRING (NULL for none) */
static HSTRING get_entry_point(void)
{
    WINE_TOKEN_PACKAGE_INFORMATION *info = NULL;
    HSTRING ret = NULL;
    HANDLE token;
    ULONG size = 0;
    NTSTATUS status;

    if (NtOpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token )) return NULL;
    status = NtQueryInformationToken( token, TokenWinePackage, NULL, 0, &size );
    while (status == STATUS_BUFFER_TOO_SMALL)
    {
        free( info );
        if (!(info = malloc( size ))) break;
        status = NtQueryInformationToken( token, TokenWinePackage, info, size, &size );
    }
    NtClose( token );
    if (!status && info && info->entry_point_len)
    {
        const WCHAR *entry = info->data + (info->full_name_len + info->family_name_len + info->app_id_len +
                                           info->root_len + info->executable_len) / sizeof(WCHAR);
        WindowsCreateString( entry, info->entry_point_len / sizeof(WCHAR), &ret );
    }
    free( info );
    return ret;
}

/* the handlers of Exiting, raised on the thread that called Run */
static void raise_exiting( struct core_application *impl )
{
    IUnknown **handlers;
    unsigned int i, count;

    lifecycle_stop();
    handlers = handler_list_snapshot( &impl->exiting_handlers, &app_cs, &count );
    for (i = 0; i < count; i++)
        IEventHandler_IInspectable_Invoke( (IEventHandler_IInspectable *)handlers[i], NULL, NULL );
    handler_list_snapshot_free( handlers, count );
}

static void remember_error( struct run_state *state, HRESULT hr )
{
    if (FAILED(hr) && !state->failed)
    {
        state->failed = TRUE;
        state->hr = hr;
    }
}

/* the thread of the view */
static DWORD WINAPI view_thread( void *param )
{
    struct run_state *state = param;
    HRESULT (WINAPI *create_window)( ICoreWindow **window );
    void (WINAPI *detach_window)( ICoreWindow *window );
    ICoreApplicationView *view = NULL;
    IFrameworkView *framework = NULL;
    ICoreWindow *window = NULL;
    WCHAR id[65];
    HSTRING entry;
    HMODULE ui;
    BOOL com;
    HRESULT hr;

    com = SUCCEEDED(CoInitializeEx( NULL, COINIT_APARTMENTTHREADED ));
    if (!(ui = LoadLibraryW( L"windows.ui.dll" )) ||
        !(create_window = (void *)GetProcAddress( ui, "WineUICreateCoreWindow" )) ||
        !(detach_window = (void *)GetProcAddress( ui, "WineUIDetachCoreWindow" )))
    {
        ERR( "Cannot find the window creation of windows.ui.\n" );
        remember_error( state, E_NOTIMPL );
        goto done;
    }

    if (FAILED(hr = IFrameworkViewSource_CreateView( state->source, &framework )) || !framework)
    {
        remember_error( state, FAILED(hr) ? hr : E_POINTER );
        goto done;
    }
    if (FAILED(hr = create_window( &window )) || FAILED(hr = core_view_create( window, TRUE, &view )))
    {
        remember_error( state, hr );
        goto release;
    }
    lifecycle_watch_window( window ); /* before the application gets the window: its handlers come after ours */

    hr = IFrameworkView_Initialize( framework, view );
    if (SUCCEEDED(hr)) hr = IFrameworkView_SetWindow( framework, window );
    if (SUCCEEDED(hr))
    {
        entry = get_entry_point();
        hr = IFrameworkView_Load( framework, entry );
        WindowsDeleteString( entry );
    }
    if (SUCCEEDED(hr))
    {
        /* raised here, after Load and before Run */
        if (!get_application_id( id, ARRAY_SIZE(id) )) id[0] = 0;
        core_view_raise_launch( view, launch_arguments(), id );
        hr = IFrameworkView_Run( framework );
    }
    remember_error( state, hr );
    remember_error( state, IFrameworkView_Uninitialize( framework ) );

release:
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
done:
    if (com) CoUninitialize();
    return 0;
}

static HRESULT WINAPI core_application_Run( ICoreApplication *iface, IFrameworkViewSource *source )
{
    struct core_application *impl = impl_from_ICoreApplication( iface );
    struct run_state state = { source, GetCurrentThreadId() };
    APTTYPEQUALIFIER qualifier;
    APTTYPE apartment;
    HANDLE thread, handles[2];
    HRESULT hr;

    TRACE( "iface %p, source %p.\n", iface, source );

    if (!source) return E_INVALIDARG;
    /* Observed on Windows 11: a single-threaded apartment is refused, nothing is called. */
    if (FAILED(hr = CoGetApartmentType( &apartment, &qualifier ))) return hr;
    if (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA) return RPC_E_WRONG_THREAD;
    if (!is_packaged()) return HRESULT_FROM_WIN32( APPMODEL_ERROR_NO_PACKAGE );

    EnterCriticalSection( &app_cs );
    if (impl->running) hr = E_ILLEGAL_METHOD_CALL;
    else
    {
        impl->running = TRUE;
        impl->run = &state;
        hr = S_OK;
    }
    LeaveCriticalSection( &app_cs );
    if (FAILED(hr)) return hr;

    state.exit_request = CreateEventW( NULL, FALSE, FALSE, NULL );
    state.exiting_done = CreateEventW( NULL, TRUE, FALSE, NULL );
    thread = state.exit_request && state.exiting_done ? CreateThread( NULL, 0, view_thread, &state, 0, NULL ) : NULL;
    if (!thread)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    /* the calling thread raises Exiting, for Exit or when the view is finished */
    handles[0] = thread;
    handles[1] = state.exit_request;
    while (WaitForMultipleObjects( 2, handles, FALSE, INFINITE ) == WAIT_OBJECT_0 + 1)
    {
        if (!state.exiting_raised) raise_exiting( impl );
        state.exiting_raised = TRUE;
        SetEvent( state.exiting_done );
    }
    CloseHandle( thread );

    if (!state.failed)
    {
        if (!state.exiting_raised) raise_exiting( impl );
        /* the application is finished: the process ends, Run does not return */
        ExitProcess( 0 );
    }
    hr = state.hr;

done:
    EnterCriticalSection( &app_cs );
    impl->running = FALSE;
    impl->run = NULL;
    LeaveCriticalSection( &app_cs );
    if (state.exit_request) CloseHandle( state.exit_request );
    if (state.exiting_done) CloseHandle( state.exiting_done );
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
    HRESULT (WINAPI *exit_window)( ICoreWindow *window );
    ICoreApplicationView *view = core_view_for_current_thread();
    ICoreWindow *window;
    HMODULE ui;

    if (!view) return;
    if ((ui = GetModuleHandleW( L"windows.ui.dll" )) && (exit_window = (void *)GetProcAddress( ui, "WineUIExitCoreWindow" )) &&
        SUCCEEDED(ICoreApplicationView_get_CoreWindow( view, &window )))
    {
        exit_window( window );
        ICoreWindow_Release( window );
    }
    ICoreApplicationView_Release( view );
}

/* Observed on Windows 11: Exit raises Exiting on the thread that called Run, then the window of the view is closed
 * (Closed is raised on the view thread, its event processing ends), the view returns from Run and is uninitialized,
 * and the process ends. */
static HRESULT WINAPI core_application_exit_Exit( ICoreApplicationExit *iface )
{
    struct core_application *impl = impl_from_ICoreApplicationExit( iface );
    struct run_state *run;
    HANDLE exiting_done = NULL;

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &app_cs );
    if ((run = impl->run) && !run->exiting_raised && GetCurrentThreadId() != run->main_thread)
    {
        exiting_done = run->exiting_done;
        SetEvent( run->exit_request );
    }
    LeaveCriticalSection( &app_cs );

    if (exiting_done) WaitForSingleObject( exiting_done, 10000 );
    else if (!run) raise_exiting( impl );   /* no view: nothing to close */

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

DEFINE_IINSPECTABLE( core_application2, ICoreApplication2, struct core_application, IActivationFactory_iface )

static HRESULT WINAPI core_application2_add_BackgroundActivated( ICoreApplication2 *iface,
        IEventHandler_BackgroundActivatedEventArgs *handler, EventRegistrationToken *token )
{
    FIXME( "iface %p, handler %p, token %p: never raised.\n", iface, handler, token );
    return lifecycle_add( LIFECYCLE_BACKGROUND_ACTIVATED, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application2_remove_BackgroundActivated( ICoreApplication2 *iface, EventRegistrationToken token )
{
    return lifecycle_remove( LIFECYCLE_BACKGROUND_ACTIVATED, token );
}

static HRESULT WINAPI core_application2_add_LeavingBackground( ICoreApplication2 *iface,
        IEventHandler_LeavingBackgroundEventArgs *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return lifecycle_add( LIFECYCLE_LEAVING_BACKGROUND, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application2_remove_LeavingBackground( ICoreApplication2 *iface, EventRegistrationToken token )
{
    return lifecycle_remove( LIFECYCLE_LEAVING_BACKGROUND, token );
}

static HRESULT WINAPI core_application2_add_EnteredBackground( ICoreApplication2 *iface,
        IEventHandler_EnteredBackgroundEventArgs *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return lifecycle_add( LIFECYCLE_ENTERED_BACKGROUND, (IUnknown *)handler, token );
}

static HRESULT WINAPI core_application2_remove_EnteredBackground( ICoreApplication2 *iface, EventRegistrationToken token )
{
    return lifecycle_remove( LIFECYCLE_ENTERED_BACKGROUND, token );
}

static HRESULT WINAPI core_application2_EnablePrelaunch( ICoreApplication2 *iface, boolean value )
{
    TRACE( "iface %p, value %d: there is no prelaunch\n", iface, value );
    return S_OK;
}

static const struct ICoreApplication2Vtbl core_application2_vtbl =
{
    core_application2_QueryInterface,
    core_application2_AddRef,
    core_application2_Release,
    /* IInspectable methods */
    core_application2_GetIids,
    core_application2_GetRuntimeClassName,
    core_application2_GetTrustLevel,
    /* ICoreApplication2 methods */
    core_application2_add_BackgroundActivated,
    core_application2_remove_BackgroundActivated,
    core_application2_add_LeavingBackground,
    core_application2_remove_LeavingBackground,
    core_application2_add_EnteredBackground,
    core_application2_remove_EnteredBackground,
    core_application2_EnablePrelaunch,
};

static struct core_application core_application =
{
    {&activation_factory_vtbl},
    {&core_application_vtbl},
    {&core_application_exit_vtbl},
    {&core_immersive_vtbl},
    {&core_application2_vtbl},
    1,
    FALSE,
    NULL,
    { &core_application.exiting_handlers, &core_application.exiting_handlers },
};

IActivationFactory *core_application_factory = &core_application.IActivationFactory_iface;
