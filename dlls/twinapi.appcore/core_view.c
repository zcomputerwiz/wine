/*
 * CoreApplicationView and launch activation (WineExplorer UWP compatibility, U1.1)
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

/* ---- event handler list --------------------------------------------------------------------------- */

struct handler
{
    struct list entry;
    INT64 token;
    IUnknown *handler;
};

static LONG64 next_token;

HRESULT handler_list_add( struct list *list, CRITICAL_SECTION *cs, IUnknown *handler, EventRegistrationToken *token )
{
    struct handler *entry;

    if (!token) return E_POINTER;
    token->value = 0;
    if (!handler) return E_INVALIDARG;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    entry->handler = handler;
    IUnknown_AddRef( handler );
    entry->token = InterlockedIncrement64( &next_token );
    EnterCriticalSection( cs );
    list_add_tail( list, &entry->entry );
    LeaveCriticalSection( cs );
    token->value = entry->token;
    return S_OK;
}

HRESULT handler_list_remove( struct list *list, CRITICAL_SECTION *cs, EventRegistrationToken token )
{
    struct handler *entry;
    IUnknown *handler = NULL;

    EnterCriticalSection( cs );
    LIST_FOR_EACH_ENTRY( entry, list, struct handler, entry )
    {
        if (entry->token != token.value) continue;
        list_remove( &entry->entry );
        handler = entry->handler;
        free( entry );
        break;
    }
    LeaveCriticalSection( cs );
    if (handler) IUnknown_Release( handler );
    return S_OK;
}

IUnknown **handler_list_snapshot( struct list *list, CRITICAL_SECTION *cs, unsigned int *count )
{
    struct handler *entry;
    IUnknown **handlers = NULL;
    unsigned int n;

    *count = 0;
    EnterCriticalSection( cs );
    if ((n = list_count( list )) && (handlers = calloc( n, sizeof(*handlers) )))
    {
        LIST_FOR_EACH_ENTRY( entry, list, struct handler, entry )
        {
            IUnknown_AddRef( entry->handler );
            handlers[(*count)++] = entry->handler;
        }
    }
    LeaveCriticalSection( cs );
    return handlers;
}

void handler_list_snapshot_free( IUnknown **handlers, unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; i++) IUnknown_Release( handlers[i] );
    free( handlers );
}

void handler_list_clear( struct list *list )
{
    struct handler *entry, *next;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, list, struct handler, entry )
    {
        list_remove( &entry->entry );
        IUnknown_Release( entry->handler );
        free( entry );
    }
}

/* ---- a dispatcher callback made of a function ----------------------------------------------------- */

struct callback_handler
{
    IDispatchedHandler IDispatchedHandler_iface;
    LONG ref;
    void (*callback)( void *context );
    void *context;
};

static inline struct callback_handler *impl_from_IDispatchedHandler( IDispatchedHandler *iface )
{
    return CONTAINING_RECORD( iface, struct callback_handler, IDispatchedHandler_iface );
}

static HRESULT WINAPI callback_handler_QueryInterface( IDispatchedHandler *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatchedHandler ))
    {
        *out = iface;
        IDispatchedHandler_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI callback_handler_AddRef( IDispatchedHandler *iface )
{
    return InterlockedIncrement( &impl_from_IDispatchedHandler( iface )->ref );
}

static ULONG WINAPI callback_handler_Release( IDispatchedHandler *iface )
{
    struct callback_handler *impl = impl_from_IDispatchedHandler( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI callback_handler_Invoke( IDispatchedHandler *iface )
{
    struct callback_handler *impl = impl_from_IDispatchedHandler( iface );

    impl->callback( impl->context );
    return S_OK;
}

static const IDispatchedHandlerVtbl callback_handler_vtbl =
{
    callback_handler_QueryInterface,
    callback_handler_AddRef,
    callback_handler_Release,
    callback_handler_Invoke,
};

/* Run a function on the thread of a dispatcher, at normal priority, in order with what is queued. */
HRESULT dispatch_callback( ICoreDispatcher *dispatcher, void (*callback)( void *context ), void *context )
{
    struct callback_handler *handler;
    IAsyncAction *action;
    HRESULT hr;

    if (!(handler = calloc( 1, sizeof(*handler) ))) return E_OUTOFMEMORY;
    handler->IDispatchedHandler_iface.lpVtbl = &callback_handler_vtbl;
    handler->ref = 1;
    handler->callback = callback;
    handler->context = context;
    hr = ICoreDispatcher_RunAsync( dispatcher, CoreDispatcherPriority_Normal, &handler->IDispatchedHandler_iface,
                                   &action );
    IDispatchedHandler_Release( &handler->IDispatchedHandler_iface );
    if (SUCCEEDED(hr)) IAsyncAction_Release( action );
    return hr;
}

/* ---- the arguments of a launch activation --------------------------------------------------------- */

struct launch_args
{
    IActivatedEventArgs IActivatedEventArgs_iface;
    ILaunchActivatedEventArgs ILaunchActivatedEventArgs_iface;
    LONG ref;
    HSTRING arguments;
    HSTRING tile_id;
};

static inline struct launch_args *impl_from_IActivatedEventArgs( IActivatedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct launch_args, IActivatedEventArgs_iface );
}

static inline struct launch_args *impl_from_ILaunchActivatedEventArgs( ILaunchActivatedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct launch_args, ILaunchActivatedEventArgs_iface );
}

static HRESULT launch_args_query( struct launch_args *args, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IActivatedEventArgs ))
        *out = &args->IActivatedEventArgs_iface;
    else if (IsEqualGUID( iid, &IID_ILaunchActivatedEventArgs ))
        *out = &args->ILaunchActivatedEventArgs_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG launch_args_release( struct launch_args *args )
{
    ULONG ref = InterlockedDecrement( &args->ref );

    if (!ref)
    {
        WindowsDeleteString( args->arguments );
        WindowsDeleteString( args->tile_id );
        free( args );
    }
    return ref;
}

static HRESULT WINAPI activated_args_QueryInterface( IActivatedEventArgs *iface, REFIID iid, void **out )
{
    return launch_args_query( impl_from_IActivatedEventArgs( iface ), iid, out );
}

static ULONG WINAPI activated_args_AddRef( IActivatedEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_IActivatedEventArgs( iface )->ref );
}

static ULONG WINAPI activated_args_Release( IActivatedEventArgs *iface )
{
    return launch_args_release( impl_from_IActivatedEventArgs( iface ) );
}

static HRESULT WINAPI activated_args_GetIids( IActivatedEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI activated_args_GetRuntimeClassName( IActivatedEventArgs *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs", 59, name );
}

static HRESULT WINAPI activated_args_GetTrustLevel( IActivatedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI activated_args_get_Kind( IActivatedEventArgs *iface, ActivationKind *value )
{
    *value = ActivationKind_Launch;
    return S_OK;
}

static HRESULT WINAPI activated_args_get_PreviousExecutionState( IActivatedEventArgs *iface,
                                                                 ApplicationExecutionState *value )
{
    *value = ApplicationExecutionState_NotRunning;
    return S_OK;
}

static HRESULT WINAPI activated_args_get_SplashScreen( IActivatedEventArgs *iface, ISplashScreen **value )
{
    FIXME( "iface %p, value %p: no splash screen.\n", iface, value );
    *value = NULL;
    return S_OK;
}

static const IActivatedEventArgsVtbl activated_args_vtbl =
{
    activated_args_QueryInterface,
    activated_args_AddRef,
    activated_args_Release,
    activated_args_GetIids,
    activated_args_GetRuntimeClassName,
    activated_args_GetTrustLevel,
    activated_args_get_Kind,
    activated_args_get_PreviousExecutionState,
    activated_args_get_SplashScreen,
};

static HRESULT WINAPI launch_args_QueryInterface( ILaunchActivatedEventArgs *iface, REFIID iid, void **out )
{
    return launch_args_query( impl_from_ILaunchActivatedEventArgs( iface ), iid, out );
}

static ULONG WINAPI launch_args_AddRef( ILaunchActivatedEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_ILaunchActivatedEventArgs( iface )->ref );
}

static ULONG WINAPI launch_args_Release( ILaunchActivatedEventArgs *iface )
{
    return launch_args_release( impl_from_ILaunchActivatedEventArgs( iface ) );
}

static HRESULT WINAPI launch_args_GetIids( ILaunchActivatedEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI launch_args_GetRuntimeClassName( ILaunchActivatedEventArgs *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs", 59, name );
}

static HRESULT WINAPI launch_args_GetTrustLevel( ILaunchActivatedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI launch_args_get_Arguments( ILaunchActivatedEventArgs *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_ILaunchActivatedEventArgs( iface )->arguments, value );
}

static HRESULT WINAPI launch_args_get_TileId( ILaunchActivatedEventArgs *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_ILaunchActivatedEventArgs( iface )->tile_id, value );
}

static const ILaunchActivatedEventArgsVtbl launch_args_vtbl =
{
    launch_args_QueryInterface,
    launch_args_AddRef,
    launch_args_Release,
    launch_args_GetIids,
    launch_args_GetRuntimeClassName,
    launch_args_GetTrustLevel,
    launch_args_get_Arguments,
    launch_args_get_TileId,
};

static HRESULT launch_args_create( const WCHAR *arguments, const WCHAR *tile_id, IActivatedEventArgs **out )
{
    struct launch_args *args;
    HRESULT hr;

    if (!(args = calloc( 1, sizeof(*args) ))) return E_OUTOFMEMORY;
    args->IActivatedEventArgs_iface.lpVtbl = &activated_args_vtbl;
    args->ILaunchActivatedEventArgs_iface.lpVtbl = &launch_args_vtbl;
    args->ref = 1;
    if (FAILED(hr = WindowsCreateString( arguments, wcslen( arguments ), &args->arguments )) ||
        FAILED(hr = WindowsCreateString( tile_id, wcslen( tile_id ), &args->tile_id )))
    {
        launch_args_release( args );
        return hr;
    }
    *out = &args->IActivatedEventArgs_iface;
    return S_OK;
}

/* ---- CoreApplicationView -------------------------------------------------------------------------- */

struct core_view
{
    ICoreApplicationView ICoreApplicationView_iface;
    ICoreApplicationView2 ICoreApplicationView2_iface;
    LONG ref;

    struct list entry;       /* the views of the process */
    DWORD thread_id;
    boolean is_main;
    ICoreWindow *window;
    ICoreDispatcher *dispatcher;

    CRITICAL_SECTION cs;
    struct list activated_handlers;
};

static CRITICAL_SECTION views_cs;
static CRITICAL_SECTION_DEBUG views_cs_debug =
{
    0, 0, &views_cs,
    { &views_cs_debug.ProcessLocksList, &views_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": views_cs") }
};
static CRITICAL_SECTION views_cs = { &views_cs_debug, -1, 0, 0, 0, 0 };
static struct list views = LIST_INIT( views );

static inline struct core_view *impl_from_ICoreApplicationView( ICoreApplicationView *iface )
{
    return CONTAINING_RECORD( iface, struct core_view, ICoreApplicationView_iface );
}

static inline struct core_view *impl_from_ICoreApplicationView2( ICoreApplicationView2 *iface )
{
    return CONTAINING_RECORD( iface, struct core_view, ICoreApplicationView2_iface );
}

static HRESULT view_query( struct core_view *view, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_ICoreApplicationView ))
        *out = &view->ICoreApplicationView_iface;
    else if (IsEqualGUID( iid, &IID_ICoreApplicationView2 ))
        *out = &view->ICoreApplicationView2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG view_release( struct core_view *view )
{
    ULONG ref = InterlockedDecrement( &view->ref );

    if (!ref)
    {
        EnterCriticalSection( &views_cs );
        if (view->entry.next) list_remove( &view->entry );
        LeaveCriticalSection( &views_cs );
        handler_list_clear( &view->activated_handlers );
        ICoreDispatcher_Release( view->dispatcher );
        ICoreWindow_Release( view->window );
        DeleteCriticalSection( &view->cs );
        free( view );
    }
    return ref;
}

static HRESULT WINAPI view_QueryInterface( ICoreApplicationView *iface, REFIID iid, void **out )
{
    return view_query( impl_from_ICoreApplicationView( iface ), iid, out );
}

static ULONG WINAPI view_AddRef( ICoreApplicationView *iface )
{
    return InterlockedIncrement( &impl_from_ICoreApplicationView( iface )->ref );
}

static ULONG WINAPI view_Release( ICoreApplicationView *iface )
{
    return view_release( impl_from_ICoreApplicationView( iface ) );
}

static HRESULT WINAPI view_GetIids( ICoreApplicationView *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI view_GetRuntimeClassName( ICoreApplicationView *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.Core.CoreApplicationView", 49, name );
}

static HRESULT WINAPI view_GetTrustLevel( ICoreApplicationView *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI view_get_CoreWindow( ICoreApplicationView *iface, ICoreWindow **value )
{
    struct core_view *view = impl_from_ICoreApplicationView( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    ICoreWindow_AddRef( *value = view->window );
    return S_OK;
}

static HRESULT WINAPI view_add_Activated( ICoreApplicationView *iface,
        ITypedEventHandler_CoreApplicationView_IActivatedEventArgs *handler, EventRegistrationToken *token )
{
    struct core_view *view = impl_from_ICoreApplicationView( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return handler_list_add( &view->activated_handlers, &view->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI view_remove_Activated( ICoreApplicationView *iface, EventRegistrationToken token )
{
    struct core_view *view = impl_from_ICoreApplicationView( iface );

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return handler_list_remove( &view->activated_handlers, &view->cs, token );
}

static HRESULT WINAPI view_get_IsMain( ICoreApplicationView *iface, boolean *value )
{
    *value = impl_from_ICoreApplicationView( iface )->is_main;
    return S_OK;
}

static HRESULT WINAPI view_get_IsHosted( ICoreApplicationView *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static const ICoreApplicationViewVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    view_GetIids,
    view_GetRuntimeClassName,
    view_GetTrustLevel,
    view_get_CoreWindow,
    view_add_Activated,
    view_remove_Activated,
    view_get_IsMain,
    view_get_IsHosted,
};

static HRESULT WINAPI view2_QueryInterface( ICoreApplicationView2 *iface, REFIID iid, void **out )
{
    return view_query( impl_from_ICoreApplicationView2( iface ), iid, out );
}

static ULONG WINAPI view2_AddRef( ICoreApplicationView2 *iface )
{
    return InterlockedIncrement( &impl_from_ICoreApplicationView2( iface )->ref );
}

static ULONG WINAPI view2_Release( ICoreApplicationView2 *iface )
{
    return view_release( impl_from_ICoreApplicationView2( iface ) );
}

static HRESULT WINAPI view2_GetIids( ICoreApplicationView2 *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI view2_GetRuntimeClassName( ICoreApplicationView2 *iface, HSTRING *name )
{
    return view_GetRuntimeClassName( &impl_from_ICoreApplicationView2( iface )->ICoreApplicationView_iface, name );
}

static HRESULT WINAPI view2_GetTrustLevel( ICoreApplicationView2 *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI view2_get_Dispatcher( ICoreApplicationView2 *iface, ICoreDispatcher **value )
{
    struct core_view *view = impl_from_ICoreApplicationView2( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    ICoreDispatcher_AddRef( *value = view->dispatcher );
    return S_OK;
}

static const ICoreApplicationView2Vtbl view2_vtbl =
{
    view2_QueryInterface,
    view2_AddRef,
    view2_Release,
    view2_GetIids,
    view2_GetRuntimeClassName,
    view2_GetTrustLevel,
    view2_get_Dispatcher,
};

/* The view of a window; the window's thread is the view's thread. */
HRESULT core_view_create( ICoreWindow *window, boolean is_main, ICoreApplicationView **out )
{
    struct core_view *view;
    HRESULT hr;

    if (!(view = calloc( 1, sizeof(*view) ))) return E_OUTOFMEMORY;
    view->ICoreApplicationView_iface.lpVtbl = &view_vtbl;
    view->ICoreApplicationView2_iface.lpVtbl = &view2_vtbl;
    view->ref = 1;
    view->thread_id = GetCurrentThreadId();
    view->is_main = is_main;
    ICoreWindow_AddRef( view->window = window );
    if (FAILED(hr = ICoreWindow_get_Dispatcher( window, &view->dispatcher )))
    {
        ICoreWindow_Release( window );
        free( view );
        return hr;
    }
    list_init( &view->activated_handlers );
    InitializeCriticalSectionEx( &view->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    view->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": core_view.cs");

    EnterCriticalSection( &views_cs );
    list_add_tail( &views, &view->entry );
    LeaveCriticalSection( &views_cs );
    *out = &view->ICoreApplicationView_iface;
    return S_OK;
}

/* the view whose thread this is, with a reference, or NULL */
ICoreApplicationView *core_view_for_current_thread(void)
{
    struct core_view *view;
    ICoreApplicationView *ret = NULL;

    EnterCriticalSection( &views_cs );
    LIST_FOR_EACH_ENTRY( view, &views, struct core_view, entry )
    {
        if (view->thread_id != GetCurrentThreadId()) continue;
        ICoreApplicationView_AddRef( ret = &view->ICoreApplicationView_iface );
        break;
    }
    LeaveCriticalSection( &views_cs );
    return ret;
}

/* the main view, with a reference, or NULL */
ICoreApplicationView *core_view_main(void)
{
    struct core_view *view;
    ICoreApplicationView *ret = NULL;

    EnterCriticalSection( &views_cs );
    LIST_FOR_EACH_ENTRY( view, &views, struct core_view, entry )
    {
        if (!view->is_main) continue;
        ICoreApplicationView_AddRef( ret = &view->ICoreApplicationView_iface );
        break;
    }
    LeaveCriticalSection( &views_cs );
    return ret;
}

/* the view and its thread are gone: it is no longer found */
void core_view_detach( ICoreApplicationView *iface )
{
    struct core_view *view = impl_from_ICoreApplicationView( iface );

    EnterCriticalSection( &views_cs );
    if (view->entry.next)
    {
        list_remove( &view->entry );
        view->entry.next = view->entry.prev = NULL;
    }
    LeaveCriticalSection( &views_cs );
}

struct launch_context
{
    struct core_view *view;
    WCHAR *arguments;
    WCHAR *tile_id;
};

static void raise_launch( void *context )
{
    struct launch_context *launch = context;
    struct core_view *view = launch->view;
    IActivatedEventArgs *args;
    IUnknown **handlers;
    unsigned int i, count;

    if (SUCCEEDED(launch_args_create( launch->arguments, launch->tile_id, &args )))
    {
        handlers = handler_list_snapshot( &view->activated_handlers, &view->cs, &count );
        for (i = 0; i < count; i++)
            ITypedEventHandler_CoreApplicationView_IActivatedEventArgs_Invoke(
                (ITypedEventHandler_CoreApplicationView_IActivatedEventArgs *)handlers[i],
                &view->ICoreApplicationView_iface, args );
        handler_list_snapshot_free( handlers, count );
        IActivatedEventArgs_Release( args );
    }
    ICoreApplicationView_Release( &view->ICoreApplicationView_iface );
    free( launch->arguments );
    free( launch->tile_id );
    free( launch );
}

/* The launch activation of the view is raised on its thread, while that thread processes events. */
HRESULT core_view_queue_launch( ICoreApplicationView *iface, const WCHAR *arguments, const WCHAR *tile_id )
{
    struct core_view *view = impl_from_ICoreApplicationView( iface );
    struct launch_context *launch;
    HRESULT hr;

    if (!(launch = calloc( 1, sizeof(*launch) ))) return E_OUTOFMEMORY;
    launch->view = view;
    launch->arguments = wcsdup( arguments );
    launch->tile_id = wcsdup( tile_id );
    ICoreApplicationView_AddRef( iface );
    if (!launch->arguments || !launch->tile_id) hr = E_OUTOFMEMORY;
    else hr = dispatch_callback( view->dispatcher, raise_launch, launch );
    if (FAILED(hr))
    {
        ICoreApplicationView_Release( iface );
        free( launch->arguments );
        free( launch->tile_id );
        free( launch );
    }
    return hr;
}

/* Run a function on the thread of every view. */
void core_view_for_each( void (*callback)( void *context ), void *context )
{
    struct core_view *view;
    ICoreDispatcher *dispatchers[16];
    unsigned int i, n = 0;

    EnterCriticalSection( &views_cs );
    LIST_FOR_EACH_ENTRY( view, &views, struct core_view, entry )
    {
        if (n == ARRAY_SIZE(dispatchers)) break;
        ICoreDispatcher_AddRef( dispatchers[n++] = view->dispatcher );
    }
    LeaveCriticalSection( &views_cs );
    for (i = 0; i < n; i++)
    {
        dispatch_callback( dispatchers[i], callback, context );
        ICoreDispatcher_Release( dispatchers[i] );
    }
}
