/*
 * Suspend, resume and the background state of an application (WineExplorer UWP U1.1c)
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

/* Windows 11 with the probe of WineExplorer (tests/uwp/u1_1, application ViewSuspend):
 *   - at launch, LeavingBackground comes after Run and before the window becomes visible;
 *   - a minimized window: WindowDeactivated, Visible=0 and EnteredBackground, then Suspending on the thread of the view,
 *     with a deadline about 5 s away; the application may complete the deferral from any thread;
 *   - restored: Resuming on the thread of the view, LeavingBackground, then Visible=1;
 *   - suspended without a change of visibility (by the process lifetime manager): Suspending and Resuming only.
 * Wine has no process lifetime manager: the visibility of the main window drives these events, and a suspended
 * process keeps running. */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

#define SUSPEND_DEADLINE_MS 5000

enum lifecycle_state { STATE_BACKGROUND, STATE_FOREGROUND, STATE_SUSPENDED };

static CRITICAL_SECTION lifecycle_cs;
static CRITICAL_SECTION_DEBUG lifecycle_cs_debug =
{
    0, 0, &lifecycle_cs,
    { &lifecycle_cs_debug.ProcessLocksList, &lifecycle_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": lifecycle_cs") }
};
static CRITICAL_SECTION lifecycle_cs = { &lifecycle_cs_debug, -1, 0, 0, 0, 0 };

static struct list handlers[LIFECYCLE_EVENT_COUNT] =
{
    LIST_INIT( handlers[0] ), LIST_INIT( handlers[1] ), LIST_INIT( handlers[2] ), LIST_INIT( handlers[3] ),
    LIST_INIT( handlers[4] ),
};
static enum lifecycle_state state = STATE_BACKGROUND;
static BOOL exiting;

HRESULT lifecycle_add( enum lifecycle_event event, IUnknown *handler, EventRegistrationToken *token )
{
    if (!handler || !token) return E_INVALIDARG;
    return handler_list_add( &handlers[event], &lifecycle_cs, handler, token );
}

HRESULT lifecycle_remove( enum lifecycle_event event, EventRegistrationToken token )
{
    return handler_list_remove( &handlers[event], &lifecycle_cs, token );
}

/* ---- the deferral of a suspension and of the background events ------------------------------------------- */

struct deferral
{
    ISuspendingDeferral ISuspendingDeferral_iface;
    IDeferral IDeferral_iface;
    IClosable IClosable_iface;
    LONG ref;
    LONG completed;
};

static inline struct deferral *impl_from_ISuspendingDeferral( ISuspendingDeferral *iface )
{
    return CONTAINING_RECORD( iface, struct deferral, ISuspendingDeferral_iface );
}

static HRESULT WINAPI deferral_QueryInterface( ISuspendingDeferral *iface, REFIID iid, void **out )
{
    struct deferral *impl = impl_from_ISuspendingDeferral( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ISuspendingDeferral ))
        *out = &impl->ISuspendingDeferral_iface;
    else if (IsEqualGUID( iid, &IID_IDeferral ))
        *out = &impl->IDeferral_iface;
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

static ULONG WINAPI deferral_AddRef( ISuspendingDeferral *iface )
{
    return InterlockedIncrement( &impl_from_ISuspendingDeferral( iface )->ref );
}

static ULONG WINAPI deferral_Release( ISuspendingDeferral *iface )
{
    struct deferral *impl = impl_from_ISuspendingDeferral( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI deferral_GetIids( ISuspendingDeferral *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI deferral_GetRuntimeClassName( ISuspendingDeferral *iface, HSTRING *name )
{
    static const WCHAR class_name[] = L"Windows.ApplicationModel.SuspendingDeferral";
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );
}

static HRESULT WINAPI deferral_GetTrustLevel( ISuspendingDeferral *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* the process is not frozen in Wine: completing only records it */
static HRESULT WINAPI deferral_Complete( ISuspendingDeferral *iface )
{
    struct deferral *impl = impl_from_ISuspendingDeferral( iface );

    TRACE( "iface %p.\n", iface );
    InterlockedExchange( &impl->completed, TRUE );
    return S_OK;
}

static const struct ISuspendingDeferralVtbl deferral_vtbl =
{
    deferral_QueryInterface,
    deferral_AddRef,
    deferral_Release,
    /* IInspectable methods */
    deferral_GetIids,
    deferral_GetRuntimeClassName,
    deferral_GetTrustLevel,
    /* ISuspendingDeferral methods */
    deferral_Complete,
};

DEFINE_IINSPECTABLE( foundation_deferral, IDeferral, struct deferral, ISuspendingDeferral_iface )

static HRESULT WINAPI foundation_deferral_Complete( IDeferral *iface )
{
    return deferral_Complete( &impl_from_IDeferral( iface )->ISuspendingDeferral_iface );
}

static const struct IDeferralVtbl foundation_deferral_vtbl =
{
    foundation_deferral_QueryInterface,
    foundation_deferral_AddRef,
    foundation_deferral_Release,
    /* IInspectable methods */
    foundation_deferral_GetIids,
    foundation_deferral_GetRuntimeClassName,
    foundation_deferral_GetTrustLevel,
    /* IDeferral methods */
    foundation_deferral_Complete,
};

DEFINE_IINSPECTABLE( deferral_closable, IClosable, struct deferral, ISuspendingDeferral_iface )

static HRESULT WINAPI deferral_closable_Close( IClosable *iface )
{
    return S_OK;
}

static const struct IClosableVtbl deferral_closable_vtbl =
{
    deferral_closable_QueryInterface,
    deferral_closable_AddRef,
    deferral_closable_Release,
    /* IInspectable methods */
    deferral_closable_GetIids,
    deferral_closable_GetRuntimeClassName,
    deferral_closable_GetTrustLevel,
    /* IClosable methods */
    deferral_closable_Close,
};

static struct deferral *deferral_create(void)
{
    struct deferral *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->ISuspendingDeferral_iface.lpVtbl = &deferral_vtbl;
    impl->IDeferral_iface.lpVtbl = &foundation_deferral_vtbl;
    impl->IClosable_iface.lpVtbl = &deferral_closable_vtbl;
    impl->ref = 1;
    return impl;
}

/* ---- the arguments: SuspendingEventArgs (also its SuspendingOperation), Entered/LeavingBackgroundEventArgs ---- */

struct lifecycle_args
{
    ISuspendingEventArgs ISuspendingEventArgs_iface;
    ISuspendingOperation ISuspendingOperation_iface;
    IEnteredBackgroundEventArgs IEnteredBackgroundEventArgs_iface;
    ILeavingBackgroundEventArgs ILeavingBackgroundEventArgs_iface;
    LONG ref;
    enum lifecycle_event event;
    DateTime deadline;
};

static inline struct lifecycle_args *impl_from_ISuspendingEventArgs( ISuspendingEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct lifecycle_args, ISuspendingEventArgs_iface );
}

static HRESULT WINAPI args_QueryInterface( ISuspendingEventArgs *iface, REFIID iid, void **out )
{
    struct lifecycle_args *impl = impl_from_ISuspendingEventArgs( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ))
        *out = impl->event == LIFECYCLE_ENTERED_BACKGROUND ? (void *)&impl->IEnteredBackgroundEventArgs_iface :
               impl->event == LIFECYCLE_LEAVING_BACKGROUND ? (void *)&impl->ILeavingBackgroundEventArgs_iface :
               (void *)&impl->ISuspendingEventArgs_iface;
    else if (impl->event == LIFECYCLE_SUSPENDING && IsEqualGUID( iid, &IID_ISuspendingEventArgs ))
        *out = &impl->ISuspendingEventArgs_iface;
    else if (impl->event == LIFECYCLE_SUSPENDING && IsEqualGUID( iid, &IID_ISuspendingOperation ))
        *out = &impl->ISuspendingOperation_iface;
    else if (impl->event == LIFECYCLE_ENTERED_BACKGROUND && IsEqualGUID( iid, &IID_IEnteredBackgroundEventArgs ))
        *out = &impl->IEnteredBackgroundEventArgs_iface;
    else if (impl->event == LIFECYCLE_LEAVING_BACKGROUND && IsEqualGUID( iid, &IID_ILeavingBackgroundEventArgs ))
        *out = &impl->ILeavingBackgroundEventArgs_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI args_AddRef( ISuspendingEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_ISuspendingEventArgs( iface )->ref );
}

static ULONG WINAPI args_Release( ISuspendingEventArgs *iface )
{
    struct lifecycle_args *impl = impl_from_ISuspendingEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI args_GetIids( ISuspendingEventArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI args_GetRuntimeClassName( ISuspendingEventArgs *iface, HSTRING *name )
{
    struct lifecycle_args *impl = impl_from_ISuspendingEventArgs( iface );
    const WCHAR *class_name = impl->event == LIFECYCLE_ENTERED_BACKGROUND ? L"Windows.ApplicationModel.EnteredBackgroundEventArgs" :
                              impl->event == LIFECYCLE_LEAVING_BACKGROUND ? L"Windows.ApplicationModel.LeavingBackgroundEventArgs" :
                              L"Windows.ApplicationModel.SuspendingEventArgs";
    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

static HRESULT WINAPI args_GetTrustLevel( ISuspendingEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI args_get_SuspendingOperation( ISuspendingEventArgs *iface, ISuspendingOperation **value )
{
    struct lifecycle_args *impl = impl_from_ISuspendingEventArgs( iface );

    if (!value) return E_POINTER;
    ISuspendingOperation_AddRef( (*value = &impl->ISuspendingOperation_iface) );
    return S_OK;
}

static const struct ISuspendingEventArgsVtbl args_vtbl =
{
    args_QueryInterface,
    args_AddRef,
    args_Release,
    /* IInspectable methods */
    args_GetIids,
    args_GetRuntimeClassName,
    args_GetTrustLevel,
    /* ISuspendingEventArgs methods */
    args_get_SuspendingOperation,
};

DEFINE_IINSPECTABLE( operation, ISuspendingOperation, struct lifecycle_args, ISuspendingEventArgs_iface )

static HRESULT WINAPI operation_GetDeferral( ISuspendingOperation *iface, ISuspendingDeferral **deferral )
{
    struct deferral *impl;

    TRACE( "iface %p, deferral %p.\n", iface, deferral );

    if (!deferral) return E_POINTER;
    if (!(impl = deferral_create())) return E_OUTOFMEMORY;
    *deferral = &impl->ISuspendingDeferral_iface;
    return S_OK;
}

static HRESULT WINAPI operation_get_Deadline( ISuspendingOperation *iface, DateTime *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_ISuspendingOperation( iface )->deadline;
    return S_OK;
}

static const struct ISuspendingOperationVtbl operation_vtbl =
{
    operation_QueryInterface,
    operation_AddRef,
    operation_Release,
    /* IInspectable methods */
    operation_GetIids,
    operation_GetRuntimeClassName,
    operation_GetTrustLevel,
    /* ISuspendingOperation methods */
    operation_GetDeferral,
    operation_get_Deadline,
};

static HRESULT background_deferral( IDeferral **value )
{
    struct deferral *impl;

    if (!value) return E_POINTER;
    if (!(impl = deferral_create())) return E_OUTOFMEMORY;
    *value = &impl->IDeferral_iface;
    return S_OK;
}

DEFINE_IINSPECTABLE( entered, IEnteredBackgroundEventArgs, struct lifecycle_args, ISuspendingEventArgs_iface )

static HRESULT WINAPI entered_GetDeferral( IEnteredBackgroundEventArgs *iface, IDeferral **value )
{
    return background_deferral( value );
}

static const struct IEnteredBackgroundEventArgsVtbl entered_vtbl =
{
    entered_QueryInterface,
    entered_AddRef,
    entered_Release,
    /* IInspectable methods */
    entered_GetIids,
    entered_GetRuntimeClassName,
    entered_GetTrustLevel,
    /* IEnteredBackgroundEventArgs methods */
    entered_GetDeferral,
};

DEFINE_IINSPECTABLE( leaving, ILeavingBackgroundEventArgs, struct lifecycle_args, ISuspendingEventArgs_iface )

static HRESULT WINAPI leaving_GetDeferral( ILeavingBackgroundEventArgs *iface, IDeferral **value )
{
    return background_deferral( value );
}

static const struct ILeavingBackgroundEventArgsVtbl leaving_vtbl =
{
    leaving_QueryInterface,
    leaving_AddRef,
    leaving_Release,
    /* IInspectable methods */
    leaving_GetIids,
    leaving_GetRuntimeClassName,
    leaving_GetTrustLevel,
    /* ILeavingBackgroundEventArgs methods */
    leaving_GetDeferral,
};

static struct lifecycle_args *args_create( enum lifecycle_event event )
{
    struct lifecycle_args *impl;
    FILETIME now;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->ISuspendingEventArgs_iface.lpVtbl = &args_vtbl;
    impl->ISuspendingOperation_iface.lpVtbl = &operation_vtbl;
    impl->IEnteredBackgroundEventArgs_iface.lpVtbl = &entered_vtbl;
    impl->ILeavingBackgroundEventArgs_iface.lpVtbl = &leaving_vtbl;
    impl->ref = 1;
    impl->event = event;
    GetSystemTimeAsFileTime( &now );
    impl->deadline.UniversalTime = (((INT64)now.dwHighDateTime << 32) | now.dwLowDateTime) + SUSPEND_DEADLINE_MS * (INT64)10000;
    return impl;
}

/* ---- raising the events ------------------------------------------------------------------------------------ */

static void raise( enum lifecycle_event event )
{
    struct lifecycle_args *args = NULL;
    IUnknown **list;
    unsigned int i, count;

    TRACE( "event %u.\n", event );

    list = handler_list_snapshot( &handlers[event], &lifecycle_cs, &count );
    if (count && event != LIFECYCLE_RESUMING && !(args = args_create( event )))
    {
        handler_list_snapshot_free( list, count );
        return;
    }
    for (i = 0; i < count; i++)
    {
        switch (event)
        {
        case LIFECYCLE_SUSPENDING:
            IEventHandler_SuspendingEventArgs_Invoke( (IEventHandler_SuspendingEventArgs *)list[i], NULL, &args->ISuspendingEventArgs_iface );
            break;
        case LIFECYCLE_RESUMING:
            IEventHandler_IInspectable_Invoke( (IEventHandler_IInspectable *)list[i], NULL, NULL );
            break;
        case LIFECYCLE_ENTERED_BACKGROUND:
            IEventHandler_EnteredBackgroundEventArgs_Invoke( (IEventHandler_EnteredBackgroundEventArgs *)list[i], NULL,
                                                             &args->IEnteredBackgroundEventArgs_iface );
            break;
        case LIFECYCLE_LEAVING_BACKGROUND:
            IEventHandler_LeavingBackgroundEventArgs_Invoke( (IEventHandler_LeavingBackgroundEventArgs *)list[i], NULL,
                                                             &args->ILeavingBackgroundEventArgs_iface );
            break;
        default:
            break;
        }
    }
    handler_list_snapshot_free( list, count );
    if (args) ISuspendingEventArgs_Release( &args->ISuspendingEventArgs_iface );
}

/* Suspending comes from the dispatcher of the view, after the handlers of the change of visibility */
static void suspend_callback( void *context )
{
    BOOL suspend;

    EnterCriticalSection( &lifecycle_cs );
    if ((suspend = state == STATE_BACKGROUND && !exiting)) state = STATE_SUSPENDED;
    LeaveCriticalSection( &lifecycle_cs );
    if (suspend) raise( LIFECYCLE_SUSPENDING );
}

/* once the application exits, the closing window does not send it to the background (as on Windows) */
void lifecycle_stop(void)
{
    EnterCriticalSection( &lifecycle_cs );
    exiting = TRUE;
    LeaveCriticalSection( &lifecycle_cs );
}

static void visibility_changed( ICoreWindow *window, BOOL visible )
{
    enum lifecycle_state old;
    ICoreDispatcher *dispatcher;

    EnterCriticalSection( &lifecycle_cs );
    if (exiting)
    {
        LeaveCriticalSection( &lifecycle_cs );
        return;
    }
    old = state;
    if (visible) state = STATE_FOREGROUND;
    else if (old == STATE_FOREGROUND) state = STATE_BACKGROUND;
    LeaveCriticalSection( &lifecycle_cs );

    if (visible)
    {
        if (old == STATE_SUSPENDED) raise( LIFECYCLE_RESUMING );
        if (old != STATE_FOREGROUND) raise( LIFECYCLE_LEAVING_BACKGROUND );
    }
    else if (old == STATE_FOREGROUND)
    {
        raise( LIFECYCLE_ENTERED_BACKGROUND );
        if (SUCCEEDED(ICoreWindow_get_Dispatcher( window, &dispatcher )))
        {
            dispatch_callback( dispatcher, suspend_callback, NULL );
            ICoreDispatcher_Release( dispatcher );
        }
    }
}

/* the VisibilityChanged handler on the main window, added before the application gets the window */
struct visibility_handler
{
    ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_iface;
    LONG ref;
};

static inline struct visibility_handler *impl_from_visibility_handler( ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct visibility_handler, ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_iface );
}

static HRESULT WINAPI visibility_handler_QueryInterface( ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs ))
    {
        *out = iface;
        ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI visibility_handler_AddRef( ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_visibility_handler( iface )->ref );
}

static ULONG WINAPI visibility_handler_Release( ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs *iface )
{
    struct visibility_handler *impl = impl_from_visibility_handler( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI visibility_handler_Invoke( ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs *iface,
                                                 ICoreWindow *window, IVisibilityChangedEventArgs *args )
{
    boolean visible = FALSE;

    if (args && SUCCEEDED(IVisibilityChangedEventArgs_get_Visible( args, &visible ))) visibility_changed( window, visible );
    return S_OK;
}

static const struct ITypedEventHandler_CoreWindow_VisibilityChangedEventArgsVtbl visibility_handler_vtbl =
{
    visibility_handler_QueryInterface,
    visibility_handler_AddRef,
    visibility_handler_Release,
    visibility_handler_Invoke,
};

void lifecycle_watch_window( ICoreWindow *window )
{
    struct visibility_handler *impl;
    EventRegistrationToken token;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return;
    impl->ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_iface.lpVtbl = &visibility_handler_vtbl;
    impl->ref = 1;
    if (FAILED(ICoreWindow_add_VisibilityChanged( window, &impl->ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_iface, &token )))
        WARN( "cannot watch the visibility of the window\n" );
    ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_Release( &impl->ITypedEventHandler_CoreWindow_VisibilityChangedEventArgs_iface );
}
