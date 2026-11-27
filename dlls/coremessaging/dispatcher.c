/* WinRT Windows.System.DispatcherQueue Implementation
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
#include "winuser.h"
#include "objbase.h"

#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(messaging);

/* A queue belongs to one thread and runs its work from that thread's message loop, through a message-only window.
 * Work can be queued from any thread; the window, the timers and the shutdown live on the queue's thread. */

#define WM_DQ_PROCESS   (WM_USER + 0)
#define WM_DQ_SHUTDOWN  (WM_USER + 1) /* lparam: the queue, with a reference for the message */
#define WM_DQ_TIMER     (WM_USER + 2) /* lparam: the timer, with a reference for the message */

enum queue_priority
{
    QUEUE_PRIORITY_HIGH,
    QUEUE_PRIORITY_NORMAL,
    QUEUE_PRIORITY_LOW,
    QUEUE_PRIORITY_COUNT
};

struct work_item
{
    struct list entry;
    IDispatcherQueueHandler *handler;
};

struct event_handler
{
    struct list entry;
    IUnknown *handler;
    EventRegistrationToken token;
};

struct shutdown_waiter
{
    IUnknown IUnknown_iface;
    LONG ref;
    HANDLE drained;
};

struct dispatcher_queue
{
    IDispatcherQueue IDispatcherQueue_iface;
    IDispatcherQueue2 IDispatcherQueue2_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    struct list items[QUEUE_PRIORITY_COUNT];
    struct list starting_handlers;
    struct list completed_handlers;
    struct list timers;          /* timers with a Win32 timer, queue thread only */
    DWORD thread_id;
    HWND hwnd;
    BOOL dedicated;
    BOOL posted;                 /* a WM_DQ_PROCESS is pending */
    BOOL shutdown_posted;        /* a shutdown has been requested */
    BOOL closed;                 /* no new work is accepted */
    BOOL finished;               /* the shutdown is done, the window is gone */
    IAsyncAction *shutdown_op;
    struct shutdown_waiter *waiter;
};

struct dispatcher_queue_timer
{
    IDispatcherQueueTimer IDispatcherQueueTimer_iface;
    LONG ref;

    struct dispatcher_queue *queue;
    struct list entry;           /* in queue->timers */
    BOOL listed;
    TimeSpan interval;
    boolean repeating;
    boolean running;
    struct list tick_handlers;
};

static LONG64 next_token;
static DWORD queue_tls = TLS_OUT_OF_INDEXES;
static INIT_ONCE queue_init_once = INIT_ONCE_STATIC_INIT;
static const WCHAR queue_class_name[] = L"Wine_SystemDispatcherQueue";

static LRESULT CALLBACK queue_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );

static BOOL WINAPI queue_init( INIT_ONCE *once, void *param, void **context )
{
    WNDCLASSW class = {0};

    if ((queue_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES) return FALSE;

    class.lpfnWndProc = queue_wndproc;
    class.hInstance = GetModuleHandleW( L"coremessaging.dll" );
    class.lpszClassName = queue_class_name;
    if (!RegisterClassW( &class ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return FALSE;
    return TRUE;
}

static struct dispatcher_queue *queue_get_current(void)
{
    if (!InitOnceExecuteOnce( &queue_init_once, queue_init, NULL, NULL )) return NULL;
    return TlsGetValue( queue_tls );
}

static void event_handlers_add( struct dispatcher_queue *queue, struct list *list, IUnknown *handler, EventRegistrationToken *token )
{
    struct event_handler *entry;

    if (!(entry = calloc( 1, sizeof(*entry) ))) return;
    entry->handler = handler;
    IUnknown_AddRef( handler );
    entry->token.value = InterlockedIncrement64( &next_token );
    *token = entry->token;

    EnterCriticalSection( &queue->cs );
    list_add_tail( list, &entry->entry );
    LeaveCriticalSection( &queue->cs );
}

static HRESULT event_handlers_remove( struct dispatcher_queue *queue, struct list *list, EventRegistrationToken token )
{
    struct event_handler *entry, *found = NULL;

    EnterCriticalSection( &queue->cs );
    LIST_FOR_EACH_ENTRY( entry, list, struct event_handler, entry )
    {
        if (entry->token.value != token.value) continue;
        list_remove( &entry->entry );
        found = entry;
        break;
    }
    LeaveCriticalSection( &queue->cs );

    if (found)
    {
        IUnknown_Release( found->handler );
        free( found );
    }
    return S_OK;
}

static void event_handlers_clear( struct list *list )
{
    struct event_handler *entry, *next;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, list, struct event_handler, entry )
    {
        list_remove( &entry->entry );
        IUnknown_Release( entry->handler );
        free( entry );
    }
}

/* takes the handlers of a list, with a reference each, so that they can be called without the lock */
static IUnknown **event_handlers_snapshot( struct dispatcher_queue *queue, struct list *list, UINT *count )
{
    struct event_handler *entry;
    IUnknown **handlers;
    UINT i = 0;

    EnterCriticalSection( &queue->cs );
    *count = list_count( list );
    if ((handlers = calloc( *count + 1, sizeof(*handlers) )))
    {
        LIST_FOR_EACH_ENTRY( entry, list, struct event_handler, entry )
        {
            handlers[i++] = entry->handler;
            IUnknown_AddRef( entry->handler );
        }
    }
    else *count = 0;
    LeaveCriticalSection( &queue->cs );

    return handlers;
}

static inline struct shutdown_waiter *impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct shutdown_waiter, IUnknown_iface );
}

static HRESULT WINAPI shutdown_waiter_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI shutdown_waiter_AddRef( IUnknown *iface )
{
    struct shutdown_waiter *impl = impl_from_IUnknown( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI shutdown_waiter_Release( IUnknown *iface )
{
    struct shutdown_waiter *impl = impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        CloseHandle( impl->drained );
        free( impl );
    }
    return ref;
}

static const IUnknownVtbl shutdown_waiter_vtbl =
{
    shutdown_waiter_QueryInterface,
    shutdown_waiter_AddRef,
    shutdown_waiter_Release,
};

static HRESULT shutdown_wait_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct shutdown_waiter *waiter = impl_from_IUnknown( invoker );

    if (!called_async) return STATUS_PENDING;
    WaitForSingleObject( waiter->drained, INFINITE );
    return S_OK;
}

struct shutdown_starting_args
{
    IDispatcherQueueShutdownStartingEventArgs IDispatcherQueueShutdownStartingEventArgs_iface;
    LONG ref;
};

static inline struct shutdown_starting_args *impl_from_IDispatcherQueueShutdownStartingEventArgs( IDispatcherQueueShutdownStartingEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct shutdown_starting_args, IDispatcherQueueShutdownStartingEventArgs_iface );
}

static HRESULT WINAPI shutdown_starting_args_QueryInterface( IDispatcherQueueShutdownStartingEventArgs *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatcherQueueShutdownStartingEventArgs ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI shutdown_starting_args_AddRef( IDispatcherQueueShutdownStartingEventArgs *iface )
{
    struct shutdown_starting_args *impl = impl_from_IDispatcherQueueShutdownStartingEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI shutdown_starting_args_Release( IDispatcherQueueShutdownStartingEventArgs *iface )
{
    struct shutdown_starting_args *impl = impl_from_IDispatcherQueueShutdownStartingEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI shutdown_starting_args_GetIids( IDispatcherQueueShutdownStartingEventArgs *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI shutdown_starting_args_GetRuntimeClassName( IDispatcherQueueShutdownStartingEventArgs *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.System.DispatcherQueueShutdownStartingEventArgs";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI shutdown_starting_args_GetTrustLevel( IDispatcherQueueShutdownStartingEventArgs *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI shutdown_starting_args_GetDeferral( IDispatcherQueueShutdownStartingEventArgs *iface, IDeferral **result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    if (!result) return E_POINTER;
    *result = NULL;
    return E_NOTIMPL;
}

static const struct IDispatcherQueueShutdownStartingEventArgsVtbl shutdown_starting_args_vtbl =
{
    shutdown_starting_args_QueryInterface,
    shutdown_starting_args_AddRef,
    shutdown_starting_args_Release,
    /* IInspectable methods */
    shutdown_starting_args_GetIids,
    shutdown_starting_args_GetRuntimeClassName,
    shutdown_starting_args_GetTrustLevel,
    /* IDispatcherQueueShutdownStartingEventArgs methods */
    shutdown_starting_args_GetDeferral,
};

static struct work_item *queue_pop( struct dispatcher_queue *queue )
{
    struct work_item *item = NULL;
    struct list *head;
    UINT i;

    EnterCriticalSection( &queue->cs );
    for (i = 0; i < QUEUE_PRIORITY_COUNT; i++)
    {
        if (!(head = list_head( &queue->items[i] ))) continue;
        item = LIST_ENTRY( head, struct work_item, entry );
        list_remove( &item->entry );
        break;
    }
    if (!item) queue->posted = FALSE;
    LeaveCriticalSection( &queue->cs );

    return item;
}

static void queue_process( struct dispatcher_queue *queue )
{
    struct work_item *item;
    HRESULT hr;

    while ((item = queue_pop( queue )))
    {
        hr = IDispatcherQueueHandler_Invoke( item->handler );
        if (FAILED(hr)) WARN( "handler %p returned %#lx.\n", item->handler, hr );
        IDispatcherQueueHandler_Release( item->handler );
        free( item );
    }
}

static void timer_apply( struct dispatcher_queue_timer *timer );

static void queue_run_shutdown( struct dispatcher_queue *queue )
{
    IDispatcherQueue *iface = &queue->IDispatcherQueue_iface;
    struct event_handler *handler, *next_handler;
    struct dispatcher_queue_timer *timer, *next;
    struct shutdown_starting_args *args;
    struct list completed;
    IUnknown **handlers;
    UINT i, count;

    if (queue->finished) return;
    TRACE( "queue %p.\n", queue );

    if ((handlers = event_handlers_snapshot( queue, &queue->starting_handlers, &count )))
    {
        if (count && (args = calloc( 1, sizeof(*args) )))
        {
            args->IDispatcherQueueShutdownStartingEventArgs_iface.lpVtbl = &shutdown_starting_args_vtbl;
            args->ref = 1;
            for (i = 0; i < count; i++)
                ITypedEventHandler_DispatcherQueue_DispatcherQueueShutdownStartingEventArgs_Invoke(
                        (ITypedEventHandler_DispatcherQueue_DispatcherQueueShutdownStartingEventArgs *)handlers[i], iface,
                        &args->IDispatcherQueueShutdownStartingEventArgs_iface );
            IDispatcherQueueShutdownStartingEventArgs_Release( &args->IDispatcherQueueShutdownStartingEventArgs_iface );
        }
        for (i = 0; i < count; i++) IUnknown_Release( handlers[i] );
        free( handlers );
    }

    /* the work queued before the shutdown still runs */
    EnterCriticalSection( &queue->cs );
    queue->closed = TRUE;
    LeaveCriticalSection( &queue->cs );
    queue_process( queue );

    LIST_FOR_EACH_ENTRY_SAFE( timer, next, &queue->timers, struct dispatcher_queue_timer, entry )
    {
        KillTimer( queue->hwnd, (UINT_PTR)timer );
        list_remove( &timer->entry );
        timer->listed = FALSE;
        IDispatcherQueueTimer_Release( &timer->IDispatcherQueueTimer_iface );
    }

    /* the shutdown operation completes before ShutdownCompleted is raised */
    if (queue->waiter)
    {
        IAsyncInfo *info;
        AsyncStatus status = Started;

        SetEvent( queue->waiter->drained );
        if (queue->shutdown_op && SUCCEEDED(IAsyncAction_QueryInterface( queue->shutdown_op, &IID_IAsyncInfo, (void **)&info )))
        {
            for (i = 0; i < 5000; i++)
            {
                if (FAILED(IAsyncInfo_get_Status( info, &status )) || status != Started) break;
                Sleep( 1 );
            }
            IAsyncInfo_Release( info );
        }
    }

    /* ShutdownCompleted is raised once: the handlers leave the list and are called and released under the lock,
     * so that a remove_ShutdownCompleted from another thread returns only once the queue let go of them */
    EnterCriticalSection( &queue->cs );
    list_init( &completed );
    list_move_tail( &completed, &queue->completed_handlers );
    LIST_FOR_EACH_ENTRY_SAFE( handler, next_handler, &completed, struct event_handler, entry )
    {
        list_remove( &handler->entry );
        ITypedEventHandler_DispatcherQueue_IInspectable_Invoke( (ITypedEventHandler_DispatcherQueue_IInspectable *)handler->handler, iface, NULL );
        IUnknown_Release( handler->handler );
        free( handler );
    }
    event_handlers_clear( &queue->starting_handlers );
    event_handlers_clear( &queue->completed_handlers );
    queue->finished = TRUE;
    LeaveCriticalSection( &queue->cs );

    if (queue_get_current() == queue) TlsSetValue( queue_tls, NULL );
    SetWindowLongPtrW( queue->hwnd, GWLP_USERDATA, 0 );
    DestroyWindow( queue->hwnd );
    queue->hwnd = NULL;
    if (queue->dedicated) PostQuitMessage( 0 );
}

static void timer_fire( struct dispatcher_queue *queue, UINT_PTR id )
{
    struct dispatcher_queue_timer *timer, *found = NULL;
    IUnknown **handlers;
    UINT i, count;

    LIST_FOR_EACH_ENTRY( timer, &queue->timers, struct dispatcher_queue_timer, entry )
    {
        if ((UINT_PTR)timer != id) continue;
        found = timer;
        break;
    }
    if (!found)
    {
        KillTimer( queue->hwnd, id );
        return;
    }

    IDispatcherQueueTimer_AddRef( &found->IDispatcherQueueTimer_iface );
    EnterCriticalSection( &queue->cs );
    if (!found->repeating) found->running = FALSE;
    LeaveCriticalSection( &queue->cs );
    if (!found->repeating) timer_apply( found );

    if ((handlers = event_handlers_snapshot( queue, &found->tick_handlers, &count )))
    {
        for (i = 0; i < count; i++)
        {
            ITypedEventHandler_DispatcherQueueTimer_IInspectable_Invoke( (ITypedEventHandler_DispatcherQueueTimer_IInspectable *)handlers[i],
                                                                         &found->IDispatcherQueueTimer_iface, NULL );
            IUnknown_Release( handlers[i] );
        }
        free( handlers );
    }
    IDispatcherQueueTimer_Release( &found->IDispatcherQueueTimer_iface );
}

static LRESULT CALLBACK queue_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct dispatcher_queue *queue = (struct dispatcher_queue *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_DQ_PROCESS:
        if (queue) queue_process( queue );
        return 0;
    case WM_DQ_SHUTDOWN:
    {
        struct dispatcher_queue *target = (struct dispatcher_queue *)lparam;
        queue_run_shutdown( target );
        IDispatcherQueue_Release( &target->IDispatcherQueue_iface );
        return 0;
    }
    case WM_DQ_TIMER:
    {
        struct dispatcher_queue_timer *timer = (struct dispatcher_queue_timer *)lparam;
        timer_apply( timer );
        IDispatcherQueueTimer_Release( &timer->IDispatcherQueueTimer_iface );
        return 0;
    }
    case WM_TIMER:
        if (queue) timer_fire( queue, wparam );
        return 0;
    }

    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static inline struct dispatcher_queue *impl_from_IDispatcherQueue( IDispatcherQueue *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher_queue, IDispatcherQueue_iface );
}

static HRESULT WINAPI dispatcher_queue_QueryInterface( IDispatcherQueue *iface, REFIID iid, void **out )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatcherQueue ))
    {
        IInspectable_AddRef( (*out = &impl->IDispatcherQueue_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IDispatcherQueue2 ))
    {
        IInspectable_AddRef( (*out = &impl->IDispatcherQueue2_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dispatcher_queue_AddRef( IDispatcherQueue *iface )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI dispatcher_queue_Release( IDispatcherQueue *iface )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    struct work_item *item, *next;
    UINT i;

    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->hwnd) SetWindowLongPtrW( impl->hwnd, GWLP_USERDATA, 0 );
        for (i = 0; i < QUEUE_PRIORITY_COUNT; i++)
        {
            LIST_FOR_EACH_ENTRY_SAFE( item, next, &impl->items[i], struct work_item, entry )
            {
                list_remove( &item->entry );
                IDispatcherQueueHandler_Release( item->handler );
                free( item );
            }
        }
        event_handlers_clear( &impl->starting_handlers );
        event_handlers_clear( &impl->completed_handlers );
        if (impl->shutdown_op) IAsyncAction_Release( impl->shutdown_op );
        if (impl->waiter) IUnknown_Release( &impl->waiter->IUnknown_iface );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI dispatcher_queue_GetIids( IDispatcherQueue *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_queue_GetRuntimeClassName( IDispatcherQueue *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_System_DispatcherQueue,
                                ARRAY_SIZE(RuntimeClass_Windows_System_DispatcherQueue) - 1, class_name );
}

static HRESULT WINAPI dispatcher_queue_GetTrustLevel( IDispatcherQueue *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT timer_create( struct dispatcher_queue *queue, IDispatcherQueueTimer **out );

static HRESULT WINAPI dispatcher_queue_CreateTimer( IDispatcherQueue *iface, IDispatcherQueueTimer **result )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    return timer_create( impl, result );
}

static HRESULT queue_enqueue( struct dispatcher_queue *queue, enum queue_priority priority,
                              IDispatcherQueueHandler *handler, boolean *result )
{
    struct work_item *item;
    BOOL post = FALSE;

    if (!handler || !result) return E_POINTER;
    if (!(item = calloc( 1, sizeof(*item) ))) return E_OUTOFMEMORY;
    IDispatcherQueueHandler_AddRef( (item->handler = handler) );

    EnterCriticalSection( &queue->cs );
    if (queue->closed || queue->finished)
    {
        LeaveCriticalSection( &queue->cs );
        IDispatcherQueueHandler_Release( handler );
        free( item );
        *result = FALSE;
        return S_OK;
    }
    list_add_tail( &queue->items[priority], &item->entry );
    if (!queue->posted) post = queue->posted = TRUE;
    if (post && !PostMessageW( queue->hwnd, WM_DQ_PROCESS, 0, 0 ))
    {
        ERR( "failed to post to queue %p, error %lu.\n", queue, GetLastError() );
        queue->posted = FALSE;
    }
    LeaveCriticalSection( &queue->cs );

    *result = TRUE;
    return S_OK;
}

static HRESULT WINAPI dispatcher_queue_TryEnqueue( IDispatcherQueue *iface, IDispatcherQueueHandler *callback, boolean *result )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, callback %p, result %p.\n", iface, callback, result );

    return queue_enqueue( impl, QUEUE_PRIORITY_NORMAL, callback, result );
}

static HRESULT WINAPI dispatcher_queue_TryEnqueueWithPriority( IDispatcherQueue *iface, DispatcherQueuePriority priority,
                                                               IDispatcherQueueHandler *callback, boolean *result )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );
    enum queue_priority index = QUEUE_PRIORITY_NORMAL;

    TRACE( "iface %p, priority %d, callback %p, result %p.\n", iface, priority, callback, result );

    if (priority > DispatcherQueuePriority_Normal) index = QUEUE_PRIORITY_HIGH;
    else if (priority < DispatcherQueuePriority_Normal) index = QUEUE_PRIORITY_LOW;
    return queue_enqueue( impl, index, callback, result );
}

static HRESULT WINAPI dispatcher_queue_add_ShutdownStarting( IDispatcherQueue *iface,
        ITypedEventHandler_DispatcherQueue_DispatcherQueueShutdownStartingEventArgs *handler, EventRegistrationToken *token )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;
    event_handlers_add( impl, &impl->starting_handlers, (IUnknown *)handler, token );
    return S_OK;
}

static HRESULT WINAPI dispatcher_queue_remove_ShutdownStarting( IDispatcherQueue *iface, EventRegistrationToken token )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );

    return event_handlers_remove( impl, &impl->starting_handlers, token );
}

static HRESULT WINAPI dispatcher_queue_add_ShutdownCompleted( IDispatcherQueue *iface,
        ITypedEventHandler_DispatcherQueue_IInspectable *handler, EventRegistrationToken *token )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;
    event_handlers_add( impl, &impl->completed_handlers, (IUnknown *)handler, token );
    return S_OK;
}

static HRESULT WINAPI dispatcher_queue_remove_ShutdownCompleted( IDispatcherQueue *iface, EventRegistrationToken token )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue( iface );

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );

    return event_handlers_remove( impl, &impl->completed_handlers, token );
}

static const struct IDispatcherQueueVtbl dispatcher_queue_vtbl =
{
    dispatcher_queue_QueryInterface,
    dispatcher_queue_AddRef,
    dispatcher_queue_Release,
    /* IInspectable methods */
    dispatcher_queue_GetIids,
    dispatcher_queue_GetRuntimeClassName,
    dispatcher_queue_GetTrustLevel,
    /* IDispatcherQueue methods */
    dispatcher_queue_CreateTimer,
    dispatcher_queue_TryEnqueue,
    dispatcher_queue_TryEnqueueWithPriority,
    dispatcher_queue_add_ShutdownStarting,
    dispatcher_queue_remove_ShutdownStarting,
    dispatcher_queue_add_ShutdownCompleted,
    dispatcher_queue_remove_ShutdownCompleted,
};

DEFINE_IINSPECTABLE( dispatcher_queue2, IDispatcherQueue2, struct dispatcher_queue, IDispatcherQueue_iface )

static HRESULT WINAPI dispatcher_queue2_get_HasThreadAccess( IDispatcherQueue2 *iface, boolean *value )
{
    struct dispatcher_queue *impl = impl_from_IDispatcherQueue2( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->thread_id == GetCurrentThreadId();
    return S_OK;
}

static const struct IDispatcherQueue2Vtbl dispatcher_queue2_vtbl =
{
    dispatcher_queue2_QueryInterface,
    dispatcher_queue2_AddRef,
    dispatcher_queue2_Release,
    /* IInspectable methods */
    dispatcher_queue2_GetIids,
    dispatcher_queue2_GetRuntimeClassName,
    dispatcher_queue2_GetTrustLevel,
    /* IDispatcherQueue2 methods */
    dispatcher_queue2_get_HasThreadAccess,
};

/* creates a queue for the calling thread */
static HRESULT queue_create_current( BOOL dedicated, struct dispatcher_queue **out )
{
    struct dispatcher_queue *impl;
    UINT i;

    if (!InitOnceExecuteOnce( &queue_init_once, queue_init, NULL, NULL )) return E_OUTOFMEMORY;
    if (TlsGetValue( queue_tls ))
    {
        WARN( "thread %04lx already has a queue.\n", GetCurrentThreadId() );
        return RPC_E_WRONG_THREAD;
    }
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;

    impl->IDispatcherQueue_iface.lpVtbl = &dispatcher_queue_vtbl;
    impl->IDispatcherQueue2_iface.lpVtbl = &dispatcher_queue2_vtbl;
    impl->ref = 1;
    for (i = 0; i < QUEUE_PRIORITY_COUNT; i++) list_init( &impl->items[i] );
    list_init( &impl->starting_handlers );
    list_init( &impl->completed_handlers );
    list_init( &impl->timers );
    impl->thread_id = GetCurrentThreadId();
    impl->dedicated = dedicated;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": dispatcher_queue.cs");

    if (!(impl->hwnd = CreateWindowExW( 0, queue_class_name, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                        GetModuleHandleW( L"coremessaging.dll" ), NULL )))
    {
        ERR( "failed to create the queue window, error %lu.\n", GetLastError() );
        impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        free( impl );
        return E_FAIL;
    }
    SetWindowLongPtrW( impl->hwnd, GWLP_USERDATA, (LONG_PTR)impl );
    TlsSetValue( queue_tls, impl );

    TRACE( "created queue %p, thread %04lx, window %p.\n", impl, impl->thread_id, impl->hwnd );
    *out = impl;
    return S_OK;
}

struct dedicated_start
{
    DISPATCHERQUEUE_THREAD_APARTMENTTYPE apartment;
    HANDLE ready;
    struct dispatcher_queue *queue;
    HRESULT hr;
};

static DWORD WINAPI dedicated_thread_proc( void *arg )
{
    struct dedicated_start *start = arg;
    struct dispatcher_queue *queue;
    HRESULT init_hr = E_FAIL;
    MSG msg;

    SetThreadDescription( GetCurrentThread(), L"wine_dispatcherqueue" );

    if (start->apartment == DQTAT_COM_STA) init_hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    else if (start->apartment == DQTAT_COM_ASTA) init_hr = RoInitialize( RO_INIT_SINGLETHREADED );

    if (SUCCEEDED(start->hr = queue_create_current( TRUE, &queue )))
    {
        /* one reference for this thread, one for the creator */
        IDispatcherQueue_AddRef( &queue->IDispatcherQueue_iface );
        start->queue = queue;
    }
    SetEvent( start->ready );

    if (SUCCEEDED(start->hr))
    {
        while (GetMessageW( &msg, NULL, 0, 0 ) > 0)
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        IDispatcherQueue_Release( &queue->IDispatcherQueue_iface );
    }

    if (SUCCEEDED(init_hr))
    {
        if (start->apartment == DQTAT_COM_ASTA) RoUninitialize();
        else CoUninitialize();
    }
    return 0;
}

HRESULT dispatcher_queue_create( DISPATCHERQUEUE_THREAD_TYPE type, DISPATCHERQUEUE_THREAD_APARTMENTTYPE apartment,
                                 IDispatcherQueue **out )
{
    struct dedicated_start *start;
    struct dispatcher_queue *queue;
    HANDLE thread;
    HRESULT hr;

    *out = NULL;

    if (type == DQTYPE_THREAD_CURRENT)
    {
        if (FAILED(hr = queue_create_current( FALSE, &queue ))) return hr;
        *out = &queue->IDispatcherQueue_iface;
        return S_OK;
    }

    if (!(start = calloc( 1, sizeof(*start) ))) return E_OUTOFMEMORY;
    start->apartment = apartment;
    start->ready = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (!(thread = CreateThread( NULL, 0, dedicated_thread_proc, start, 0, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        CloseHandle( start->ready );
        free( start );
        return hr;
    }
    WaitForSingleObject( start->ready, INFINITE );
    CloseHandle( thread );
    CloseHandle( start->ready );

    if (SUCCEEDED(hr = start->hr)) *out = &start->queue->IDispatcherQueue_iface;
    free( start );
    return hr;
}

static HRESULT queue_post_shutdown( struct dispatcher_queue *queue )
{
    IDispatcherQueue_AddRef( &queue->IDispatcherQueue_iface );
    if (!PostMessageW( queue->hwnd, WM_DQ_SHUTDOWN, 0, (LPARAM)queue ))
    {
        ERR( "failed to post the shutdown of queue %p, error %lu.\n", queue, GetLastError() );
        IDispatcherQueue_Release( &queue->IDispatcherQueue_iface );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    return S_OK;
}

HRESULT dispatcher_queue_shutdown_async( IDispatcherQueue *iface, IAsyncAction **operation )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );
    struct shutdown_waiter *waiter;
    HRESULT hr = S_OK;

    if (!operation) return E_POINTER;
    *operation = NULL;

    EnterCriticalSection( &queue->cs );
    if (queue->shutdown_op)
    {
        IAsyncAction_AddRef( (*operation = queue->shutdown_op) );
        LeaveCriticalSection( &queue->cs );
        return S_OK;
    }

    if (!(waiter = calloc( 1, sizeof(*waiter) )))
    {
        LeaveCriticalSection( &queue->cs );
        return E_OUTOFMEMORY;
    }
    waiter->IUnknown_iface.lpVtbl = &shutdown_waiter_vtbl;
    waiter->ref = 1;
    waiter->drained = CreateEventW( NULL, TRUE, FALSE, NULL );
    queue->waiter = waiter;
    if (queue->finished) SetEvent( waiter->drained );

    if (FAILED(hr = async_action_create( &waiter->IUnknown_iface, shutdown_wait_async, &queue->shutdown_op )))
    {
        LeaveCriticalSection( &queue->cs );
        return hr;
    }
    IAsyncAction_AddRef( (*operation = queue->shutdown_op) );

    if (!queue->finished && !queue->shutdown_posted)
    {
        queue->shutdown_posted = TRUE;
        hr = queue_post_shutdown( queue );
    }
    LeaveCriticalSection( &queue->cs );

    return S_OK;
}

/* the controller is gone: a queue that was not shut down is shut down now */
void dispatcher_queue_controller_released( IDispatcherQueue *iface )
{
    struct dispatcher_queue *queue = impl_from_IDispatcherQueue( iface );
    BOOL post = FALSE;

    EnterCriticalSection( &queue->cs );
    if (!queue->finished && !queue->shutdown_posted) post = queue->shutdown_posted = TRUE;
    LeaveCriticalSection( &queue->cs );
    if (!post) return;

    if (queue->thread_id == GetCurrentThreadId()) queue_run_shutdown( queue );
    else queue_post_shutdown( queue );
}

static inline struct dispatcher_queue_timer *impl_from_IDispatcherQueueTimer( IDispatcherQueueTimer *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher_queue_timer, IDispatcherQueueTimer_iface );
}

/* on the queue thread: gives the timer a Win32 timer, or takes it away */
static void timer_apply( struct dispatcher_queue_timer *timer )
{
    struct dispatcher_queue *queue = timer->queue;
    BOOL running;
    UINT ms;

    EnterCriticalSection( &queue->cs );
    running = timer->running && !queue->finished;
    ms = max( 1, timer->interval.Duration / 10000 );
    LeaveCriticalSection( &queue->cs );

    if (running)
    {
        if (!timer->listed)
        {
            IDispatcherQueueTimer_AddRef( &timer->IDispatcherQueueTimer_iface );
            list_add_tail( &queue->timers, &timer->entry );
            timer->listed = TRUE;
        }
        SetTimer( queue->hwnd, (UINT_PTR)timer, ms, NULL );
    }
    else if (timer->listed)
    {
        if (queue->hwnd) KillTimer( queue->hwnd, (UINT_PTR)timer );
        list_remove( &timer->entry );
        timer->listed = FALSE;
        IDispatcherQueueTimer_Release( &timer->IDispatcherQueueTimer_iface );
    }
}

static void timer_update( struct dispatcher_queue_timer *timer )
{
    struct dispatcher_queue *queue = timer->queue;

    if (queue->thread_id == GetCurrentThreadId())
    {
        if (queue->hwnd) timer_apply( timer );
        return;
    }
    IDispatcherQueueTimer_AddRef( &timer->IDispatcherQueueTimer_iface );
    if (!queue->hwnd || !PostMessageW( queue->hwnd, WM_DQ_TIMER, 0, (LPARAM)timer ))
        IDispatcherQueueTimer_Release( &timer->IDispatcherQueueTimer_iface );
}

static HRESULT WINAPI timer_QueryInterface( IDispatcherQueueTimer *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDispatcherQueueTimer ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI timer_AddRef( IDispatcherQueueTimer *iface )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI timer_Release( IDispatcherQueueTimer *iface )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        event_handlers_clear( &impl->tick_handlers );
        IDispatcherQueue_Release( &impl->queue->IDispatcherQueue_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI timer_GetIids( IDispatcherQueueTimer *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI timer_GetRuntimeClassName( IDispatcherQueueTimer *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_System_DispatcherQueueTimer,
                                ARRAY_SIZE(RuntimeClass_Windows_System_DispatcherQueueTimer) - 1, class_name );
}

static HRESULT WINAPI timer_GetTrustLevel( IDispatcherQueueTimer *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI timer_get_Interval( IDispatcherQueueTimer *iface, TimeSpan *value )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->queue->cs );
    *value = impl->interval;
    LeaveCriticalSection( &impl->queue->cs );
    return S_OK;
}

static HRESULT WINAPI timer_put_Interval( IDispatcherQueueTimer *iface, TimeSpan value )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );
    BOOL running;

    TRACE( "iface %p, value %I64d.\n", iface, value.Duration );

    if (value.Duration < 0) return E_INVALIDARG;
    EnterCriticalSection( &impl->queue->cs );
    impl->interval = value;
    running = impl->running;
    LeaveCriticalSection( &impl->queue->cs );
    if (running) timer_update( impl );
    return S_OK;
}

static HRESULT WINAPI timer_get_IsRunning( IDispatcherQueueTimer *iface, boolean *value )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->queue->cs );
    *value = impl->running;
    LeaveCriticalSection( &impl->queue->cs );
    return S_OK;
}

static HRESULT WINAPI timer_get_IsRepeating( IDispatcherQueueTimer *iface, boolean *value )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->queue->cs );
    *value = impl->repeating;
    LeaveCriticalSection( &impl->queue->cs );
    return S_OK;
}

static HRESULT WINAPI timer_put_IsRepeating( IDispatcherQueueTimer *iface, boolean value )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p, value %d.\n", iface, value );

    EnterCriticalSection( &impl->queue->cs );
    impl->repeating = value;
    LeaveCriticalSection( &impl->queue->cs );
    return S_OK;
}

static HRESULT WINAPI timer_Start( IDispatcherQueueTimer *iface )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->queue->cs );
    impl->running = TRUE;
    LeaveCriticalSection( &impl->queue->cs );
    timer_update( impl );
    return S_OK;
}

static HRESULT WINAPI timer_Stop( IDispatcherQueueTimer *iface )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p.\n", iface );

    EnterCriticalSection( &impl->queue->cs );
    impl->running = FALSE;
    LeaveCriticalSection( &impl->queue->cs );
    timer_update( impl );
    return S_OK;
}

static HRESULT WINAPI timer_add_Tick( IDispatcherQueueTimer *iface, ITypedEventHandler_DispatcherQueueTimer_IInspectable *handler,
                                      EventRegistrationToken *token )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;
    event_handlers_add( impl->queue, &impl->tick_handlers, (IUnknown *)handler, token );
    return S_OK;
}

static HRESULT WINAPI timer_remove_Tick( IDispatcherQueueTimer *iface, EventRegistrationToken token )
{
    struct dispatcher_queue_timer *impl = impl_from_IDispatcherQueueTimer( iface );

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );

    return event_handlers_remove( impl->queue, &impl->tick_handlers, token );
}

static const struct IDispatcherQueueTimerVtbl timer_vtbl =
{
    timer_QueryInterface,
    timer_AddRef,
    timer_Release,
    /* IInspectable methods */
    timer_GetIids,
    timer_GetRuntimeClassName,
    timer_GetTrustLevel,
    /* IDispatcherQueueTimer methods */
    timer_get_Interval,
    timer_put_Interval,
    timer_get_IsRunning,
    timer_get_IsRepeating,
    timer_put_IsRepeating,
    timer_Start,
    timer_Stop,
    timer_add_Tick,
    timer_remove_Tick,
};

static HRESULT timer_create( struct dispatcher_queue *queue, IDispatcherQueueTimer **out )
{
    struct dispatcher_queue_timer *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDispatcherQueueTimer_iface.lpVtbl = &timer_vtbl;
    impl->ref = 1;
    impl->repeating = TRUE;
    list_init( &impl->tick_handlers );
    IDispatcherQueue_AddRef( &queue->IDispatcherQueue_iface );
    impl->queue = queue;

    *out = &impl->IDispatcherQueueTimer_iface;
    return S_OK;
}

struct dispatcher_queue_statics
{
    IActivationFactory IActivationFactory_iface;
    IDispatcherQueueStatics IDispatcherQueueStatics_iface;
    LONG ref;
};

static inline struct dispatcher_queue_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher_queue_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct dispatcher_queue_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IDispatcherQueueStatics ))
    {
        IInspectable_AddRef( (*out = &impl->IDispatcherQueueStatics_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct dispatcher_queue_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct dispatcher_queue_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( dispatcher_queue_statics, IDispatcherQueueStatics, struct dispatcher_queue_statics, IActivationFactory_iface )

static HRESULT WINAPI dispatcher_queue_statics_GetForCurrentThread( IDispatcherQueueStatics *iface, IDispatcherQueue **result )
{
    struct dispatcher_queue *queue;

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    if ((queue = queue_get_current()))
        IDispatcherQueue_AddRef( (*result = &queue->IDispatcherQueue_iface) );
    else
        *result = NULL;
    TRACE( "thread %04lx, queue %p.\n", GetCurrentThreadId(), *result );
    return S_OK;
}

static const struct IDispatcherQueueStaticsVtbl dispatcher_queue_statics_vtbl =
{
    dispatcher_queue_statics_QueryInterface,
    dispatcher_queue_statics_AddRef,
    dispatcher_queue_statics_Release,
    /* IInspectable methods */
    dispatcher_queue_statics_GetIids,
    dispatcher_queue_statics_GetRuntimeClassName,
    dispatcher_queue_statics_GetTrustLevel,
    /* IDispatcherQueueStatics methods */
    dispatcher_queue_statics_GetForCurrentThread,
};

static struct dispatcher_queue_statics dispatcher_queue_statics =
{
    {&factory_vtbl},
    {&dispatcher_queue_statics_vtbl},
    1,
};

IActivationFactory *dispatcher_queue_factory = &dispatcher_queue_statics.IActivationFactory_iface;
