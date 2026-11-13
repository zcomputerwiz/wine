/*
 * CoreWindow of a view (WineExplorer UWP compatibility, U1.1)
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
 * The window of a view that CoreApplication.Run creates on the calling thread. It has no native window yet:
 * it keeps its state, queues its events to the dispatcher of its thread (so they are raised while that thread
 * processes events) and answers the CoreWindow interface. Presentation and input are not implemented.
 */

enum window_event
{
    EVENT_ACTIVATED,
    EVENT_AUTOMATION_PROVIDER_REQUESTED,
    EVENT_CHARACTER_RECEIVED,
    EVENT_CLOSED,
    EVENT_INPUT_ENABLED,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_POINTER_CAPTURE_LOST,
    EVENT_POINTER_ENTERED,
    EVENT_POINTER_EXITED,
    EVENT_POINTER_MOVED,
    EVENT_POINTER_PRESSED,
    EVENT_POINTER_RELEASED,
    EVENT_TOUCH_HIT_TESTING,
    EVENT_POINTER_WHEEL_CHANGED,
    EVENT_SIZE_CHANGED,
    EVENT_VISIBILITY_CHANGED,
    EVENT_COUNT
};

struct core_window
{
    ICoreWindow ICoreWindow_iface;
    LONG ref;

    struct list entry;          /* the windows of the process */
    DWORD thread_id;
    ICoreDispatcher *dispatcher;

    CRITICAL_SECTION cs;
    BOOL attached;              /* GetForCurrentThread finds it */
    BOOL visible, active, closed, input_enabled;
    CoreWindowFlowDirection flow_direction;
    struct list handlers[EVENT_COUNT];
};

static CRITICAL_SECTION windows_cs;
static CRITICAL_SECTION_DEBUG windows_cs_debug =
{
    0, 0, &windows_cs,
    { &windows_cs_debug.ProcessLocksList, &windows_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": windows_cs") }
};
static CRITICAL_SECTION windows_cs = { &windows_cs_debug, -1, 0, 0, 0, 0 };
static struct list windows = LIST_INIT( windows );

static inline struct core_window *impl_from_ICoreWindow( ICoreWindow *iface )
{
    return CONTAINING_RECORD( iface, struct core_window, ICoreWindow_iface );
}

/* ---- the event arguments ------------------------------------------------------------------------ */

enum args_kind { ARGS_CLOSED, ARGS_ACTIVATED, ARGS_VISIBILITY };

struct event_args
{
    ICoreWindowEventArgs ICoreWindowEventArgs_iface;
    IWindowActivatedEventArgs IWindowActivatedEventArgs_iface;
    IVisibilityChangedEventArgs IVisibilityChangedEventArgs_iface;
    LONG ref;
    enum args_kind kind;
    boolean handled;
    CoreWindowActivationState state;
    boolean visible;
};

static inline struct event_args *args_from_base( ICoreWindowEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct event_args, ICoreWindowEventArgs_iface );
}

static inline struct event_args *args_from_activated( IWindowActivatedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct event_args, IWindowActivatedEventArgs_iface );
}

static inline struct event_args *args_from_visibility( IVisibilityChangedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct event_args, IVisibilityChangedEventArgs_iface );
}

static HRESULT args_query( struct event_args *args, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_ICoreWindowEventArgs ))
        *out = &args->ICoreWindowEventArgs_iface;
    else if (args->kind == ARGS_ACTIVATED && IsEqualGUID( iid, &IID_IWindowActivatedEventArgs ))
        *out = &args->IWindowActivatedEventArgs_iface;
    else if (args->kind == ARGS_VISIBILITY && IsEqualGUID( iid, &IID_IVisibilityChangedEventArgs ))
        *out = &args->IVisibilityChangedEventArgs_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG args_addref( struct event_args *args )
{
    return InterlockedIncrement( &args->ref );
}

static ULONG args_release( struct event_args *args )
{
    ULONG ref = InterlockedDecrement( &args->ref );

    if (!ref) free( args );
    return ref;
}

static HRESULT args_class_name( struct event_args *args, HSTRING *name )
{
    switch (args->kind)
    {
    case ARGS_ACTIVATED: return WindowsCreateString( L"Windows.UI.Core.WindowActivatedEventArgs", 39, name );
    case ARGS_VISIBILITY: return WindowsCreateString( L"Windows.UI.Core.VisibilityChangedEventArgs", 41, name );
    default: return WindowsCreateString( L"Windows.UI.Core.CoreWindowEventArgs", 35, name );
    }
}

static HRESULT WINAPI base_args_QueryInterface( ICoreWindowEventArgs *iface, REFIID iid, void **out )
{
    return args_query( args_from_base( iface ), iid, out );
}

static ULONG WINAPI base_args_AddRef( ICoreWindowEventArgs *iface )
{
    return args_addref( args_from_base( iface ) );
}

static ULONG WINAPI base_args_Release( ICoreWindowEventArgs *iface )
{
    return args_release( args_from_base( iface ) );
}

static HRESULT WINAPI base_args_GetIids( ICoreWindowEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI base_args_GetRuntimeClassName( ICoreWindowEventArgs *iface, HSTRING *name )
{
    return args_class_name( args_from_base( iface ), name );
}

static HRESULT WINAPI base_args_GetTrustLevel( ICoreWindowEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI base_args_get_Handled( ICoreWindowEventArgs *iface, boolean *value )
{
    *value = args_from_base( iface )->handled;
    return S_OK;
}

static HRESULT WINAPI base_args_put_Handled( ICoreWindowEventArgs *iface, boolean value )
{
    args_from_base( iface )->handled = value;
    return S_OK;
}

static const ICoreWindowEventArgsVtbl base_args_vtbl =
{
    base_args_QueryInterface,
    base_args_AddRef,
    base_args_Release,
    base_args_GetIids,
    base_args_GetRuntimeClassName,
    base_args_GetTrustLevel,
    base_args_get_Handled,
    base_args_put_Handled,
};

static HRESULT WINAPI activated_args_QueryInterface( IWindowActivatedEventArgs *iface, REFIID iid, void **out )
{
    return args_query( args_from_activated( iface ), iid, out );
}

static ULONG WINAPI activated_args_AddRef( IWindowActivatedEventArgs *iface )
{
    return args_addref( args_from_activated( iface ) );
}

static ULONG WINAPI activated_args_Release( IWindowActivatedEventArgs *iface )
{
    return args_release( args_from_activated( iface ) );
}

static HRESULT WINAPI activated_args_GetIids( IWindowActivatedEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI activated_args_GetRuntimeClassName( IWindowActivatedEventArgs *iface, HSTRING *name )
{
    return args_class_name( args_from_activated( iface ), name );
}

static HRESULT WINAPI activated_args_GetTrustLevel( IWindowActivatedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI activated_args_get_WindowActivationState( IWindowActivatedEventArgs *iface,
                                                                CoreWindowActivationState *value )
{
    *value = args_from_activated( iface )->state;
    return S_OK;
}

static const IWindowActivatedEventArgsVtbl activated_args_vtbl =
{
    activated_args_QueryInterface,
    activated_args_AddRef,
    activated_args_Release,
    activated_args_GetIids,
    activated_args_GetRuntimeClassName,
    activated_args_GetTrustLevel,
    activated_args_get_WindowActivationState,
};

static HRESULT WINAPI visibility_args_QueryInterface( IVisibilityChangedEventArgs *iface, REFIID iid, void **out )
{
    return args_query( args_from_visibility( iface ), iid, out );
}

static ULONG WINAPI visibility_args_AddRef( IVisibilityChangedEventArgs *iface )
{
    return args_addref( args_from_visibility( iface ) );
}

static ULONG WINAPI visibility_args_Release( IVisibilityChangedEventArgs *iface )
{
    return args_release( args_from_visibility( iface ) );
}

static HRESULT WINAPI visibility_args_GetIids( IVisibilityChangedEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI visibility_args_GetRuntimeClassName( IVisibilityChangedEventArgs *iface, HSTRING *name )
{
    return args_class_name( args_from_visibility( iface ), name );
}

static HRESULT WINAPI visibility_args_GetTrustLevel( IVisibilityChangedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI visibility_args_get_Visible( IVisibilityChangedEventArgs *iface, boolean *value )
{
    *value = args_from_visibility( iface )->visible;
    return S_OK;
}

static const IVisibilityChangedEventArgsVtbl visibility_args_vtbl =
{
    visibility_args_QueryInterface,
    visibility_args_AddRef,
    visibility_args_Release,
    visibility_args_GetIids,
    visibility_args_GetRuntimeClassName,
    visibility_args_GetTrustLevel,
    visibility_args_get_Visible,
};

static struct event_args *args_create( enum args_kind kind )
{
    struct event_args *args;

    if (!(args = calloc( 1, sizeof(*args) ))) return NULL;
    args->ICoreWindowEventArgs_iface.lpVtbl = &base_args_vtbl;
    args->IWindowActivatedEventArgs_iface.lpVtbl = &activated_args_vtbl;
    args->IVisibilityChangedEventArgs_iface.lpVtbl = &visibility_args_vtbl;
    args->ref = 1;
    args->kind = kind;
    return args;
}

/* ---- raising the events, on the thread of the window --------------------------------------------- */

static void window_raise( struct core_window *window, enum window_event event, struct event_args *args )
{
    unsigned int i, count;
    IUnknown **handlers = ui_handler_snapshot( &window->handlers[event], &window->cs, &count );

    for (i = 0; i < count; i++)
    {
        switch (event)
        {
        case EVENT_ACTIVATED:
            ITypedEventHandler_CoreWindow_WindowActivatedEventArgs_Invoke(
                (ITypedEventHandler_CoreWindow_WindowActivatedEventArgs *)handlers[i], &window->ICoreWindow_iface,
                &args->IWindowActivatedEventArgs_iface );
            break;
        case EVENT_CLOSED:
            ITypedEventHandler_CoreWindow_CoreWindowEventArgs_Invoke(
                (ITypedEventHandler_CoreWindow_CoreWindowEventArgs *)handlers[i], &window->ICoreWindow_iface,
                &args->ICoreWindowEventArgs_iface );
            break;
        case EVENT_VISIBILITY_CHANGED:
            ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_Invoke(
                (ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs *)handlers[i], &window->ICoreWindow_iface,
                &args->IVisibilityChangedEventArgs_iface );
            break;
        default:
            break;
        }
    }
    ui_handler_snapshot_free( handlers, count );
}

enum window_op { OP_VISIBILITY, OP_ACTIVATED, OP_CLOSED };

struct window_event_item
{
    struct core_window *window;
    enum window_op op;
    boolean visible;
};

static void window_event_run( void *context )
{
    struct window_event_item *item = context;
    struct core_window *window = item->window;
    struct event_args *args = NULL;

    switch (item->op)
    {
    case OP_VISIBILITY:
        if ((args = args_create( ARGS_VISIBILITY )))
        {
            args->visible = item->visible;
            window_raise( window, EVENT_VISIBILITY_CHANGED, args );
        }
        break;
    case OP_ACTIVATED:
        if ((args = args_create( ARGS_ACTIVATED )))
        {
            args->state = CoreWindowActivationState_CodeActivated;
            window_raise( window, EVENT_ACTIVATED, args );
        }
        break;
    case OP_CLOSED:
        if ((args = args_create( ARGS_CLOSED ))) window_raise( window, EVENT_CLOSED, args );
        /* the dispatcher stops once what was queued has run */
        ui_dispatcher_quit( window->dispatcher );
        break;
    }
    if (args) args_release( args );
    ICoreWindow_Release( &window->ICoreWindow_iface );
    free( item );
}

static void window_post( struct core_window *window, enum window_op op, boolean visible )
{
    struct window_event_item *item;

    if (!(item = calloc( 1, sizeof(*item) ))) return;
    item->window = window;
    item->op = op;
    item->visible = visible;
    ICoreWindow_AddRef( &window->ICoreWindow_iface );
    if (FAILED(ui_dispatcher_post( window->dispatcher, CoreDispatcherPriority_Normal, window_event_run, item )))
    {
        ICoreWindow_Release( &window->ICoreWindow_iface );
        free( item );
    }
}

/* ---- ICoreWindow --------------------------------------------------------------------------------- */

static HRESULT WINAPI window_QueryInterface( ICoreWindow *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_ICoreWindow ))
    {
        *out = iface;
        ICoreWindow_AddRef( iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI window_AddRef( ICoreWindow *iface )
{
    return InterlockedIncrement( &impl_from_ICoreWindow( iface )->ref );
}

static ULONG WINAPI window_Release( ICoreWindow *iface )
{
    struct core_window *impl = impl_from_ICoreWindow( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        unsigned int i;

        EnterCriticalSection( &windows_cs );
        if (impl->entry.next) list_remove( &impl->entry );
        LeaveCriticalSection( &windows_cs );
        for (i = 0; i < EVENT_COUNT; i++) ui_handler_clear( &impl->handlers[i] );
        ICoreDispatcher_Release( impl->dispatcher );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI window_GetIids( ICoreWindow *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI window_GetRuntimeClassName( ICoreWindow *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.CoreWindow", 26, name );
}

static HRESULT WINAPI window_GetTrustLevel( ICoreWindow *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI window_get_AutomationHostProvider( ICoreWindow *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI window_get_Bounds( ICoreWindow *iface, Rect *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    /* no native window yet: the whole screen */
    value->X = 0;
    value->Y = 0;
    value->Width = GetSystemMetrics( SM_CXSCREEN );
    value->Height = GetSystemMetrics( SM_CYSCREEN );
    return S_OK;
}

static HRESULT WINAPI window_get_CustomProperties( ICoreWindow *iface, IPropertySet **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI window_get_Dispatcher( ICoreWindow *iface, ICoreDispatcher **value )
{
    struct core_window *impl = impl_from_ICoreWindow( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    ICoreDispatcher_AddRef( *value = impl->dispatcher );
    return S_OK;
}

static HRESULT WINAPI window_get_FlowDirection( ICoreWindow *iface, CoreWindowFlowDirection *value )
{
    *value = impl_from_ICoreWindow( iface )->flow_direction;
    return S_OK;
}

static HRESULT WINAPI window_put_FlowDirection( ICoreWindow *iface, CoreWindowFlowDirection value )
{
    if (value < CoreWindowFlowDirection_LeftToRight || value > CoreWindowFlowDirection_RightToLeft) return E_INVALIDARG;
    impl_from_ICoreWindow( iface )->flow_direction = value;
    return S_OK;
}

static HRESULT WINAPI window_get_IsInputEnabled( ICoreWindow *iface, boolean *value )
{
    *value = impl_from_ICoreWindow( iface )->input_enabled;
    return S_OK;
}

static HRESULT WINAPI window_put_IsInputEnabled( ICoreWindow *iface, boolean value )
{
    impl_from_ICoreWindow( iface )->input_enabled = value;
    return S_OK;
}

static HRESULT WINAPI window_get_PointerCursor( ICoreWindow *iface, ICoreCursor **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI window_put_PointerCursor( ICoreWindow *iface, ICoreCursor *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI window_get_PointerPosition( ICoreWindow *iface, Point *value )
{
    POINT pt;

    GetCursorPos( &pt );
    value->X = pt.x;
    value->Y = pt.y;
    return S_OK;
}

static HRESULT WINAPI window_get_Visible( ICoreWindow *iface, boolean *value )
{
    struct core_window *impl = impl_from_ICoreWindow( iface );

    EnterCriticalSection( &impl->cs );
    *value = impl->visible;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI window_Activate( ICoreWindow *iface )
{
    struct core_window *impl = impl_from_ICoreWindow( iface );
    BOOL became_visible = FALSE, became_active = FALSE;

    TRACE( "iface %p.\n", iface );

    if (impl->thread_id != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;

    /* Observed on Windows 11: without a presented frame the window stays invisible and raises neither Activated
     * nor VisibilityChanged, however long the thread processes events. Presentation (U1.1b) will raise them. */
    (void)became_visible;
    (void)became_active;
    return S_OK;
}

/* the window of the view is closed by the system when the application exits */
static HRESULT window_close_internal( struct core_window *impl )
{
    BOOL was_visible = FALSE, already = FALSE;

    if (impl->thread_id != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;

    EnterCriticalSection( &impl->cs );
    if (!(already = impl->closed))
    {
        impl->closed = TRUE;
        was_visible = impl->visible;
        impl->visible = impl->active = FALSE;
    }
    LeaveCriticalSection( &impl->cs );

    if (already) return S_OK;
    if (was_visible) window_post( impl, OP_VISIBILITY, FALSE );
    window_post( impl, OP_CLOSED, 0 );
    return S_OK;
}

/* Observed on Windows 11: an application cannot close the window of its view; Close throws
 * E_ILLEGAL_METHOD_CALL. CoreApplication.Exit ends the application and closes the window. */
static HRESULT WINAPI window_Close( ICoreWindow *iface )
{
    TRACE( "iface %p.\n", iface );
    return E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI window_GetAsyncKeyState( ICoreWindow *iface, VirtualKey key, CoreVirtualKeyStates *state )
{
    SHORT s = GetAsyncKeyState( key );

    *state = (s & 0x8000) ? CoreVirtualKeyStates_Down : CoreVirtualKeyStates_None;
    return S_OK;
}

static HRESULT WINAPI window_GetKeyState( ICoreWindow *iface, VirtualKey key, CoreVirtualKeyStates *state )
{
    SHORT s = GetKeyState( key );

    *state = (s & 0x8000) ? CoreVirtualKeyStates_Down : CoreVirtualKeyStates_None;
    if (s & 1) *state |= CoreVirtualKeyStates_Locked;
    return S_OK;
}

static HRESULT WINAPI window_ReleasePointerCapture( ICoreWindow *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return S_OK;
}

static HRESULT WINAPI window_SetPointerCapture( ICoreWindow *iface )
{
    FIXME( "iface %p stub!\n", iface );
    return S_OK;
}

/* Every event can be subscribed to; only Activated, Closed and VisibilityChanged are raised so far. */
#define WINDOW_EVENT( name, index, handler_type )                                                        \
    static HRESULT WINAPI window_add_##name( ICoreWindow *iface, handler_type *handler,                  \
                                             EventRegistrationToken *cookie )                            \
    {                                                                                                    \
        struct core_window *impl = impl_from_ICoreWindow( iface );                                       \
        TRACE( "iface %p, handler %p, cookie %p.\n", iface, handler, cookie );                           \
        return ui_handler_add( &impl->handlers[index], &impl->cs, (IUnknown *)handler, cookie );         \
    }                                                                                                    \
    static HRESULT WINAPI window_remove_##name( ICoreWindow *iface, EventRegistrationToken cookie )      \
    {                                                                                                    \
        struct core_window *impl = impl_from_ICoreWindow( iface );                                       \
        return ui_handler_remove( &impl->handlers[index], &impl->cs, cookie );                           \
    }

WINDOW_EVENT( Activated, EVENT_ACTIVATED, ITypedEventHandler_CoreWindow_WindowActivatedEventArgs )
WINDOW_EVENT( AutomationProviderRequested, EVENT_AUTOMATION_PROVIDER_REQUESTED,
              ITypedEventHandler_CoreWindow_AutomationProviderRequestedEventArgs )
WINDOW_EVENT( CharacterReceived, EVENT_CHARACTER_RECEIVED, ITypedEventHandler_CoreWindow_CharacterReceivedEventArgs )
WINDOW_EVENT( Closed, EVENT_CLOSED, ITypedEventHandler_CoreWindow_CoreWindowEventArgs )
WINDOW_EVENT( InputEnabled, EVENT_INPUT_ENABLED, ITypedEventHandler_CoreWindow_InputEnabledEventArgs )
WINDOW_EVENT( KeyDown, EVENT_KEY_DOWN, ITypedEventHandler_CoreWindow_KeyEventArgs )
WINDOW_EVENT( KeyUp, EVENT_KEY_UP, ITypedEventHandler_CoreWindow_KeyEventArgs )
WINDOW_EVENT( PointerCaptureLost, EVENT_POINTER_CAPTURE_LOST, ITypedEventHandler_CoreWindow_PointerEventArgs )
WINDOW_EVENT( PointerEntered, EVENT_POINTER_ENTERED, ITypedEventHandler_CoreWindow_PointerEventArgs )
WINDOW_EVENT( PointerExited, EVENT_POINTER_EXITED, ITypedEventHandler_CoreWindow_PointerEventArgs )
WINDOW_EVENT( PointerMoved, EVENT_POINTER_MOVED, ITypedEventHandler_CoreWindow_PointerEventArgs )
WINDOW_EVENT( PointerPressed, EVENT_POINTER_PRESSED, ITypedEventHandler_CoreWindow_PointerEventArgs )
WINDOW_EVENT( PointerReleased, EVENT_POINTER_RELEASED, ITypedEventHandler_CoreWindow_PointerEventArgs )
WINDOW_EVENT( TouchHitTesting, EVENT_TOUCH_HIT_TESTING, ITypedEventHandler_CoreWindow_TouchHitTestingEventArgs )
WINDOW_EVENT( PointerWheelChanged, EVENT_POINTER_WHEEL_CHANGED, ITypedEventHandler_CoreWindow_PointerEventArgs )
WINDOW_EVENT( SizeChanged, EVENT_SIZE_CHANGED, ITypedEventHandler_CoreWindow_WindowSizeChangedEventArgs )
WINDOW_EVENT( VisibilityChanged, EVENT_VISIBILITY_CHANGED, ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs )

static const ICoreWindowVtbl window_vtbl =
{
    window_QueryInterface,
    window_AddRef,
    window_Release,
    window_GetIids,
    window_GetRuntimeClassName,
    window_GetTrustLevel,
    window_get_AutomationHostProvider,
    window_get_Bounds,
    window_get_CustomProperties,
    window_get_Dispatcher,
    window_get_FlowDirection,
    window_put_FlowDirection,
    window_get_IsInputEnabled,
    window_put_IsInputEnabled,
    window_get_PointerCursor,
    window_put_PointerCursor,
    window_get_PointerPosition,
    window_get_Visible,
    window_Activate,
    window_Close,
    window_GetAsyncKeyState,
    window_GetKeyState,
    window_ReleasePointerCapture,
    window_SetPointerCapture,
    window_add_Activated,
    window_remove_Activated,
    window_add_AutomationProviderRequested,
    window_remove_AutomationProviderRequested,
    window_add_CharacterReceived,
    window_remove_CharacterReceived,
    window_add_Closed,
    window_remove_Closed,
    window_add_InputEnabled,
    window_remove_InputEnabled,
    window_add_KeyDown,
    window_remove_KeyDown,
    window_add_KeyUp,
    window_remove_KeyUp,
    window_add_PointerCaptureLost,
    window_remove_PointerCaptureLost,
    window_add_PointerEntered,
    window_remove_PointerEntered,
    window_add_PointerExited,
    window_remove_PointerExited,
    window_add_PointerMoved,
    window_remove_PointerMoved,
    window_add_PointerPressed,
    window_remove_PointerPressed,
    window_add_PointerReleased,
    window_remove_PointerReleased,
    window_add_TouchHitTesting,
    window_remove_TouchHitTesting,
    window_add_PointerWheelChanged,
    window_remove_PointerWheelChanged,
    window_add_SizeChanged,
    window_remove_SizeChanged,
    window_add_VisibilityChanged,
    window_remove_VisibilityChanged,
};

/* ---- the windows of the process ------------------------------------------------------------------ */

/* The window of the calling thread, with a reference; NULL for a thread without one. */
ICoreWindow *ui_window_get_for_current_thread(void)
{
    struct core_window *window;
    ICoreWindow *ret = NULL;

    EnterCriticalSection( &windows_cs );
    LIST_FOR_EACH_ENTRY( window, &windows, struct core_window, entry )
    {
        if (!window->attached || window->thread_id != GetCurrentThreadId()) continue;
        ICoreWindow_AddRef( ret = &window->ICoreWindow_iface );
        break;
    }
    LeaveCriticalSection( &windows_cs );
    return ret;
}

/*
 * Private to twinapi.appcore: CoreApplication.Run creates the window of its view on the calling thread, which
 * becomes the thread of the window and of its dispatcher. A thread has one window.
 */
HRESULT WINAPI WineUICreateCoreWindow( ICoreWindow **out )
{
    struct core_window *window;
    ICoreWindow *existing;
    unsigned int i;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    if ((existing = ui_window_get_for_current_thread()))
    {
        ICoreWindow_Release( existing );
        return E_ILLEGAL_METHOD_CALL;
    }

    if (!(window = calloc( 1, sizeof(*window) ))) return E_OUTOFMEMORY;
    window->ICoreWindow_iface.lpVtbl = &window_vtbl;
    window->ref = 1;
    window->thread_id = GetCurrentThreadId();
    window->input_enabled = TRUE;
    for (i = 0; i < EVENT_COUNT; i++) list_init( &window->handlers[i] );
    InitializeCriticalSectionEx( &window->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    window->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": core_window.cs");
    if (FAILED(hr = ui_dispatcher_create( &window->dispatcher )))
    {
        DeleteCriticalSection( &window->cs );
        free( window );
        return hr;
    }
    window->attached = TRUE;
    EnterCriticalSection( &windows_cs );
    list_add_tail( &windows, &window->entry );
    LeaveCriticalSection( &windows_cs );
    *out = &window->ICoreWindow_iface;
    return S_OK;
}

/* Private to twinapi.appcore: the system closes the window of the view of the calling thread (CoreApplication.Exit). */
HRESULT WINAPI WineUIExitCoreWindow( ICoreWindow *iface )
{
    return window_close_internal( impl_from_ICoreWindow( iface ) );
}

/* The thread no longer has this window: GetForCurrentThread does not find it any more. */
void WINAPI WineUIDetachCoreWindow( ICoreWindow *iface )
{
    struct core_window *window = impl_from_ICoreWindow( iface );

    EnterCriticalSection( &windows_cs );
    window->attached = FALSE;
    LeaveCriticalSection( &windows_cs );
}
