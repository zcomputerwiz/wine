/*
 * CoreDispatcher and the helpers shared by the CoreWindow objects of windows.ui
 * (WineExplorer UWP compatibility, U1.1)
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

/* ---- event handler lists ------------------------------------------------------------------------ */

struct ui_handler
{
    struct list entry;
    INT64       token;
    IUnknown   *handler;
};

static LONG64 next_token;

HRESULT ui_handler_add( struct list *list, CRITICAL_SECTION *cs, IUnknown *handler, EventRegistrationToken *token )
{
    struct ui_handler *entry;

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

HRESULT ui_handler_remove( struct list *list, CRITICAL_SECTION *cs, EventRegistrationToken token )
{
    struct ui_handler *entry;
    IUnknown *handler = NULL;

    EnterCriticalSection( cs );
    LIST_FOR_EACH_ENTRY( entry, list, struct ui_handler, entry )
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

/* The handlers registered now, each with a reference, in registration order. */
IUnknown **ui_handler_snapshot( struct list *list, CRITICAL_SECTION *cs, unsigned int *count )
{
    struct ui_handler *entry;
    IUnknown **handlers;
    unsigned int n = 0;

    *count = 0;
    EnterCriticalSection( cs );
    n = list_count( list );
    if (n && (handlers = calloc( n, sizeof(*handlers) )))
    {
        LIST_FOR_EACH_ENTRY( entry, list, struct ui_handler, entry )
        {
            IUnknown_AddRef( entry->handler );
            handlers[(*count)++] = entry->handler;
        }
    }
    else handlers = NULL;
    LeaveCriticalSection( cs );
    return handlers;
}

void ui_handler_snapshot_free( IUnknown **handlers, unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; i++) IUnknown_Release( handlers[i] );
    free( handlers );
}

void ui_handler_clear( struct list *list )
{
    struct ui_handler *entry, *next;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, list, struct ui_handler, entry )
    {
        list_remove( &entry->entry );
        IUnknown_Release( entry->handler );
        free( entry );
    }
}

/* ---- IAsyncAction of RunAsync and RunIdleAsync -------------------------------------------------- */

/* the enumerators of AsyncStatus (asyncinfo.idl) have no prefix */
#define AsyncStatus_Started   Started
#define AsyncStatus_Completed Completed
#define AsyncStatus_Canceled  Canceled
#define AsyncStatus_Error     Error
#define ASYNC_CLOSED 4

struct async_action
{
    IAsyncAction IAsyncAction_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    IAsyncActionCompletedHandler *handler;
    BOOL handler_set;
    int status;   /* AsyncStatus, or ASYNC_CLOSED */
    HRESULT hr;
    UINT32 id;
};

static LONG next_async_id;

static inline struct async_action *impl_from_IAsyncAction( IAsyncAction *iface )
{
    return CONTAINING_RECORD( iface, struct async_action, IAsyncAction_iface );
}

static inline struct async_action *impl_from_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_action, IAsyncInfo_iface );
}

static HRESULT WINAPI async_QueryInterface( IAsyncAction *iface, REFIID iid, void **out )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncAction ))
        *out = &impl->IAsyncAction_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo ))
        *out = &impl->IAsyncInfo_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI async_AddRef( IAsyncAction *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncAction( iface )->ref );
}

static ULONG WINAPI async_Release( IAsyncAction *iface )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->handler) IAsyncActionCompletedHandler_Release( impl->handler );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_GetIids( IAsyncAction *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_GetRuntimeClassName( IAsyncAction *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncAction", 31, name );
}

static HRESULT WINAPI async_GetTrustLevel( IAsyncAction *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_put_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler *handler )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, handler %p.\n", iface, handler );

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_CLOSED) hr = E_ILLEGAL_METHOD_CALL;
    else if (impl->handler_set) hr = E_ILLEGAL_DELEGATE_ASSIGNMENT;
    else
    {
        impl->handler_set = TRUE;
        if ((impl->handler = handler))
        {
            IAsyncActionCompletedHandler_AddRef( handler );
            if (impl->status != AsyncStatus_Started)
            {
                int status = impl->status;

                impl->handler = NULL;   /* invoked once, here */
                LeaveCriticalSection( &impl->cs );
                IAsyncActionCompletedHandler_Invoke( handler, iface, status );
                IAsyncActionCompletedHandler_Release( handler );
                return S_OK;
            }
        }
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI async_get_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler **handler )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_CLOSED) hr = E_ILLEGAL_METHOD_CALL;
    else if ((*handler = impl->handler)) IAsyncActionCompletedHandler_AddRef( *handler );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI async_GetResults( IAsyncAction *iface )
{
    struct async_action *impl = impl_from_IAsyncAction( iface );
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    switch (impl->status)
    {
    case AsyncStatus_Completed: hr = S_OK; break;
    case AsyncStatus_Canceled: hr = HRESULT_FROM_WIN32( ERROR_CANCELLED ); break;
    case AsyncStatus_Error: hr = impl->hr; break;
    default: hr = E_ILLEGAL_METHOD_CALL; break;
    }
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static const IAsyncActionVtbl async_vtbl =
{
    async_QueryInterface,
    async_AddRef,
    async_Release,
    async_GetIids,
    async_GetRuntimeClassName,
    async_GetTrustLevel,
    async_put_Completed,
    async_get_Completed,
    async_GetResults,
};

static HRESULT WINAPI async_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    return async_QueryInterface( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, iid, out );
}

static ULONG WINAPI async_info_AddRef( IAsyncInfo *iface )
{
    return async_AddRef( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface );
}

static ULONG WINAPI async_info_Release( IAsyncInfo *iface )
{
    return async_Release( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface );
}

static HRESULT WINAPI async_info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids )
{
    return async_GetIids( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, count, iids );
}

static HRESULT WINAPI async_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name )
{
    return async_GetRuntimeClassName( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, name );
}

static HRESULT WINAPI async_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    return async_GetTrustLevel( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, level );
}

static HRESULT WINAPI async_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    struct async_action *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_CLOSED) hr = E_ILLEGAL_METHOD_CALL;
    else *id = impl->id;
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI async_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_action *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_CLOSED) hr = E_ILLEGAL_METHOD_CALL;
    else *status = impl->status;
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI async_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *code )
{
    struct async_action *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_CLOSED) *code = hr = E_ILLEGAL_METHOD_CALL;
    else *code = impl->status == AsyncStatus_Error ? impl->hr : S_OK;
    LeaveCriticalSection( &impl->cs );
    return hr;
}

/* settle an action that has not finished: Completed, Canceled or Error; the Completed handler runs once */
static void async_finish( struct async_action *impl, AsyncStatus status, HRESULT hr )
{
    IAsyncActionCompletedHandler *handler = NULL;

    EnterCriticalSection( &impl->cs );
    if (impl->status == AsyncStatus_Started)
    {
        impl->status = status;
        impl->hr = hr;
        handler = impl->handler;
        impl->handler = NULL;
    }
    LeaveCriticalSection( &impl->cs );
    if (handler)
    {
        IAsyncActionCompletedHandler_Invoke( handler, &impl->IAsyncAction_iface, status );
        IAsyncActionCompletedHandler_Release( handler );
    }
}

static HRESULT WINAPI async_info_Cancel( IAsyncInfo *iface )
{
    struct async_action *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_CLOSED) hr = E_ILLEGAL_METHOD_CALL;
    LeaveCriticalSection( &impl->cs );
    /* an action that has not started is dropped by the dispatcher; one that is running finishes */
    if (!hr) async_finish( impl, AsyncStatus_Canceled, S_OK );
    return hr;
}

static HRESULT WINAPI async_info_Close( IAsyncInfo *iface )
{
    struct async_action *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == AsyncStatus_Started) hr = E_ILLEGAL_STATE_CHANGE;
    else impl->status = ASYNC_CLOSED;
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static const IAsyncInfoVtbl async_info_vtbl =
{
    async_info_QueryInterface,
    async_info_AddRef,
    async_info_Release,
    async_info_GetIids,
    async_info_GetRuntimeClassName,
    async_info_GetTrustLevel,
    async_info_get_Id,
    async_info_get_Status,
    async_info_get_ErrorCode,
    async_info_Cancel,
    async_info_Close,
};

static struct async_action *async_create(void)
{
    struct async_action *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IAsyncAction_iface.lpVtbl = &async_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_info_vtbl;
    impl->ref = 1;
    impl->status = AsyncStatus_Started;
    impl->id = InterlockedIncrement( &next_async_id );
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": async_action.cs");
    return impl;
}

/* ---- CoreDispatcher ----------------------------------------------------------------------------- */

struct item
{
    struct list entry;
    int priority;                 /* CoreDispatcherPriority */
    IDispatchedHandler *handler;
    IIdleDispatchedHandler *idle_handler;
    void (*callback)( void *context );   /* an event of the runtime itself */
    void *context;
    struct async_action *action;
};

struct dispatcher
{
    ICoreDispatcher ICoreDispatcher_iface;
    ICoreAcceleratorKeys ICoreAcceleratorKeys_iface;
    LONG ref;

    DWORD thread_id;
    CRITICAL_SECTION cs;
    HANDLE wake;
    BOOL quit;
    struct list items;
    struct list accelerator_handlers;
};

static inline struct dispatcher *impl_from_ICoreDispatcher( ICoreDispatcher *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher, ICoreDispatcher_iface );
}

static inline struct dispatcher *impl_from_ICoreAcceleratorKeys( ICoreAcceleratorKeys *iface )
{
    return CONTAINING_RECORD( iface, struct dispatcher, ICoreAcceleratorKeys_iface );
}

static HRESULT WINAPI dispatcher_QueryInterface( ICoreDispatcher *iface, REFIID iid, void **out )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_ICoreDispatcher ))
        *out = &impl->ICoreDispatcher_iface;
    else if (IsEqualGUID( iid, &IID_ICoreAcceleratorKeys ))
        *out = &impl->ICoreAcceleratorKeys_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI dispatcher_AddRef( ICoreDispatcher *iface )
{
    return InterlockedIncrement( &impl_from_ICoreDispatcher( iface )->ref );
}

static void item_free( struct item *item )
{
    if (item->handler) IDispatchedHandler_Release( item->handler );
    if (item->idle_handler) IIdleDispatchedHandler_Release( item->idle_handler );
    if (item->action) IAsyncAction_Release( &item->action->IAsyncAction_iface );
    free( item );
}

static ULONG WINAPI dispatcher_Release( ICoreDispatcher *iface )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        struct item *item, *next;

        LIST_FOR_EACH_ENTRY_SAFE( item, next, &impl->items, struct item, entry )
        {
            list_remove( &item->entry );
            if (item->action) async_finish( item->action, AsyncStatus_Canceled, S_OK );
            item_free( item );
        }
        ui_handler_clear( &impl->accelerator_handlers );
        CloseHandle( impl->wake );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI dispatcher_GetIids( ICoreDispatcher *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI dispatcher_GetRuntimeClassName( ICoreDispatcher *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.CoreDispatcher", 30, name );
}

static HRESULT WINAPI dispatcher_GetTrustLevel( ICoreDispatcher *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI dispatcher_get_HasThreadAccess( ICoreDispatcher *iface, boolean *value )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->thread_id == GetCurrentThreadId();
    return S_OK;
}

/* queue an item; the caller gives its references to the item. Returns FALSE (and frees the item) once the
 * dispatcher has quit: an action queued then is canceled, it never runs. */
static BOOL dispatcher_queue( struct dispatcher *impl, struct item *item )
{
    BOOL quit;

    EnterCriticalSection( &impl->cs );
    if (!(quit = impl->quit)) list_add_tail( &impl->items, &item->entry );
    LeaveCriticalSection( &impl->cs );
    if (quit)
    {
        if (item->action) async_finish( item->action, AsyncStatus_Canceled, S_OK );
        item_free( item );
        return FALSE;
    }
    SetEvent( impl->wake );
    return TRUE;
}

/* the item to run next: the highest priority, the oldest of those; idle items only when nothing else waits.
 * Items of a canceled action are dropped here. Returns NULL when there is none. */
static struct item *dispatcher_next( struct dispatcher *impl, BOOL *only_idle_left )
{
    struct item *item, *next, *best = NULL;

    EnterCriticalSection( &impl->cs );
    LIST_FOR_EACH_ENTRY_SAFE( item, next, &impl->items, struct item, entry )
    {
        if (item->action)
        {
            AsyncStatus status = AsyncStatus_Started;

            IAsyncInfo_get_Status( &item->action->IAsyncInfo_iface, &status );
            if (status != AsyncStatus_Started)
            {
                list_remove( &item->entry );
                item_free( item );
                continue;
            }
        }
        if (!best || item->priority > best->priority) best = item;
    }
    if (best)
    {
        list_remove( &best->entry );
        *only_idle_left = best->priority > CoreDispatcherPriority_Idle ? FALSE : TRUE;
        /* is anything else waiting? then an idle callback is not idle */
        if (!list_empty( &impl->items )) *only_idle_left = FALSE;
    }
    LeaveCriticalSection( &impl->cs );
    return best;
}

/* IIdleDispatchedHandlerArgs of RunIdleAsync */
struct idle_args
{
    IIdleDispatchedHandlerArgs IIdleDispatchedHandlerArgs_iface;
    LONG ref;
    BOOL idle;
};

static HRESULT WINAPI idle_args_QueryInterface( IIdleDispatchedHandlerArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IIdleDispatchedHandlerArgs ))
    {
        *out = iface;
        IIdleDispatchedHandlerArgs_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI idle_args_AddRef( IIdleDispatchedHandlerArgs *iface )
{
    struct idle_args *impl = CONTAINING_RECORD( iface, struct idle_args, IIdleDispatchedHandlerArgs_iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI idle_args_Release( IIdleDispatchedHandlerArgs *iface )
{
    struct idle_args *impl = CONTAINING_RECORD( iface, struct idle_args, IIdleDispatchedHandlerArgs_iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI idle_args_GetIids( IIdleDispatchedHandlerArgs *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI idle_args_GetRuntimeClassName( IIdleDispatchedHandlerArgs *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.IdleDispatchedHandlerArgs", 41, name );
}

static HRESULT WINAPI idle_args_GetTrustLevel( IIdleDispatchedHandlerArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI idle_args_get_IsDispatcherIdle( IIdleDispatchedHandlerArgs *iface, boolean *value )
{
    struct idle_args *impl = CONTAINING_RECORD( iface, struct idle_args, IIdleDispatchedHandlerArgs_iface );
    *value = impl->idle;
    return S_OK;
}

static const IIdleDispatchedHandlerArgsVtbl idle_args_vtbl =
{
    idle_args_QueryInterface,
    idle_args_AddRef,
    idle_args_Release,
    idle_args_GetIids,
    idle_args_GetRuntimeClassName,
    idle_args_GetTrustLevel,
    idle_args_get_IsDispatcherIdle,
};

static void item_run( struct item *item, BOOL idle )
{
    if (item->callback) item->callback( item->context );
    else if (item->idle_handler)
    {
        struct idle_args *args;

        if ((args = calloc( 1, sizeof(*args) )))
        {
            args->IIdleDispatchedHandlerArgs_iface.lpVtbl = &idle_args_vtbl;
            args->ref = 1;
            args->idle = idle;
            IIdleDispatchedHandler_Invoke( item->idle_handler, &args->IIdleDispatchedHandlerArgs_iface );
            IIdleDispatchedHandlerArgs_Release( &args->IIdleDispatchedHandlerArgs_iface );
        }
    }
    else if (item->handler) IDispatchedHandler_Invoke( item->handler );
    if (item->action) async_finish( item->action, AsyncStatus_Completed, S_OK );
}

/* run one item if there is one; returns whether it ran one */
static BOOL dispatcher_run_one( struct dispatcher *impl )
{
    BOOL idle = FALSE;
    struct item *item = dispatcher_next( impl, &idle );

    if (!item) return FALSE;
    item_run( item, idle );
    item_free( item );
    return TRUE;
}

static void pump_messages( struct dispatcher *impl )
{
    MSG msg;

    while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        if (msg.message == WM_QUIT)
        {
            EnterCriticalSection( &impl->cs );
            impl->quit = TRUE;
            LeaveCriticalSection( &impl->cs );
            return;
        }
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
}

static BOOL dispatcher_quit_requested( struct dispatcher *impl )
{
    BOOL quit;

    EnterCriticalSection( &impl->cs );
    quit = impl->quit;
    LeaveCriticalSection( &impl->cs );
    return quit;
}

static HRESULT WINAPI dispatcher_ProcessEvents( ICoreDispatcher *iface, CoreProcessEventsOption options )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );
    BOOL ran;

    TRACE( "iface %p, options %d.\n", iface, options );

    if (impl->thread_id != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;

    switch (options)
    {
    case CoreProcessEventsOption_ProcessOneIfPresent:
        pump_messages( impl );
        dispatcher_run_one( impl );
        return S_OK;

    case CoreProcessEventsOption_ProcessAllIfPresent:
        pump_messages( impl );
        while (dispatcher_run_one( impl )) pump_messages( impl );
        return S_OK;

    case CoreProcessEventsOption_ProcessOneAndAllPending:
    case CoreProcessEventsOption_ProcessUntilQuit:
        for (;;)
        {
            pump_messages( impl );
            ran = FALSE;
            while (dispatcher_run_one( impl )) ran = TRUE;
            if (dispatcher_quit_requested( impl )) break;
            /* one and all pending: done once something ran, otherwise wait for the next event */
            if (ran && options == CoreProcessEventsOption_ProcessOneAndAllPending) break;
            if (!ran) MsgWaitForMultipleObjectsEx( 1, &impl->wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE );
        }
        /* what was queued before the quit still runs */
        while (dispatcher_run_one( impl )) {}
        return S_OK;

    default:
        return E_INVALIDARG;
    }
}

static HRESULT WINAPI dispatcher_RunAsync( ICoreDispatcher *iface, CoreDispatcherPriority priority,
                                           IDispatchedHandler *callback, IAsyncAction **action )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );
    struct item *item;

    TRACE( "iface %p, priority %d, callback %p, action %p.\n", iface, priority, callback, action );

    if (!action) return E_POINTER;
    *action = NULL;
    if (!callback) return E_INVALIDARG;
    if (priority < CoreDispatcherPriority_Idle || priority > CoreDispatcherPriority_High) return E_INVALIDARG;

    if (!(item = calloc( 1, sizeof(*item) ))) return E_OUTOFMEMORY;
    if (!(item->action = async_create()))
    {
        free( item );
        return E_OUTOFMEMORY;
    }
    item->priority = priority;
    item->handler = callback;
    IDispatchedHandler_AddRef( callback );
    IAsyncAction_AddRef( *action = &item->action->IAsyncAction_iface );
    dispatcher_queue( impl, item );
    return S_OK;
}

static HRESULT WINAPI dispatcher_RunIdleAsync( ICoreDispatcher *iface, IIdleDispatchedHandler *callback,
                                               IAsyncAction **action )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );
    struct item *item;

    TRACE( "iface %p, callback %p, action %p.\n", iface, callback, action );

    if (!action) return E_POINTER;
    *action = NULL;
    if (!callback) return E_INVALIDARG;

    if (!(item = calloc( 1, sizeof(*item) ))) return E_OUTOFMEMORY;
    if (!(item->action = async_create()))
    {
        free( item );
        return E_OUTOFMEMORY;
    }
    item->priority = CoreDispatcherPriority_Idle;
    item->idle_handler = callback;
    IIdleDispatchedHandler_AddRef( callback );
    IAsyncAction_AddRef( *action = &item->action->IAsyncAction_iface );
    dispatcher_queue( impl, item );
    return S_OK;
}

static const ICoreDispatcherVtbl dispatcher_vtbl =
{
    dispatcher_QueryInterface,
    dispatcher_AddRef,
    dispatcher_Release,
    dispatcher_GetIids,
    dispatcher_GetRuntimeClassName,
    dispatcher_GetTrustLevel,
    dispatcher_get_HasThreadAccess,
    dispatcher_ProcessEvents,
    dispatcher_RunAsync,
    dispatcher_RunIdleAsync,
};

static HRESULT WINAPI accel_QueryInterface( ICoreAcceleratorKeys *iface, REFIID iid, void **out )
{
    return dispatcher_QueryInterface( &impl_from_ICoreAcceleratorKeys( iface )->ICoreDispatcher_iface, iid, out );
}

static ULONG WINAPI accel_AddRef( ICoreAcceleratorKeys *iface )
{
    return dispatcher_AddRef( &impl_from_ICoreAcceleratorKeys( iface )->ICoreDispatcher_iface );
}

static ULONG WINAPI accel_Release( ICoreAcceleratorKeys *iface )
{
    return dispatcher_Release( &impl_from_ICoreAcceleratorKeys( iface )->ICoreDispatcher_iface );
}

static HRESULT WINAPI accel_GetIids( ICoreAcceleratorKeys *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI accel_GetRuntimeClassName( ICoreAcceleratorKeys *iface, HSTRING *name )
{
    return dispatcher_GetRuntimeClassName( &impl_from_ICoreAcceleratorKeys( iface )->ICoreDispatcher_iface, name );
}

static HRESULT WINAPI accel_GetTrustLevel( ICoreAcceleratorKeys *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* accepted and kept; nothing raises it before input is implemented (U1.1b) */
static HRESULT WINAPI accel_add_AcceleratorKeyActivated( ICoreAcceleratorKeys *iface,
        ITypedEventHandler_CoreDispatcher_AcceleratorKeyEventArgs *handler, EventRegistrationToken *cookie )
{
    struct dispatcher *impl = impl_from_ICoreAcceleratorKeys( iface );

    FIXME( "iface %p, handler %p, cookie %p: the event is never raised.\n", iface, handler, cookie );
    return ui_handler_add( &impl->accelerator_handlers, &impl->cs, (IUnknown *)handler, cookie );
}

static HRESULT WINAPI accel_remove_AcceleratorKeyActivated( ICoreAcceleratorKeys *iface, EventRegistrationToken cookie )
{
    struct dispatcher *impl = impl_from_ICoreAcceleratorKeys( iface );
    return ui_handler_remove( &impl->accelerator_handlers, &impl->cs, cookie );
}

static const ICoreAcceleratorKeysVtbl accel_vtbl =
{
    accel_QueryInterface,
    accel_AddRef,
    accel_Release,
    accel_GetIids,
    accel_GetRuntimeClassName,
    accel_GetTrustLevel,
    accel_add_AcceleratorKeyActivated,
    accel_remove_AcceleratorKeyActivated,
};

/* the dispatcher of the current thread */
HRESULT ui_dispatcher_create( ICoreDispatcher **out )
{
    struct dispatcher *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICoreDispatcher_iface.lpVtbl = &dispatcher_vtbl;
    impl->ICoreAcceleratorKeys_iface.lpVtbl = &accel_vtbl;
    impl->ref = 1;
    impl->thread_id = GetCurrentThreadId();
    if (!(impl->wake = CreateEventW( NULL, FALSE, FALSE, NULL )))
    {
        free( impl );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    list_init( &impl->items );
    list_init( &impl->accelerator_handlers );
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": dispatcher.cs");
    *out = &impl->ICoreDispatcher_iface;
    return S_OK;
}

/* queue an event of the runtime itself; callback owns context and runs on the dispatcher's thread */
HRESULT ui_dispatcher_post( ICoreDispatcher *iface, CoreDispatcherPriority priority,
                            void (*callback)( void *context ), void *context )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );
    struct item *item;

    if (!(item = calloc( 1, sizeof(*item) ))) return E_OUTOFMEMORY;
    item->priority = priority;
    item->callback = callback;
    item->context = context;
    return dispatcher_queue( impl, item ) ? S_OK : RPC_E_DISCONNECTED;
}

/* ProcessEvents(ProcessUntilQuit) returns once what is queued has run */
void ui_dispatcher_quit( ICoreDispatcher *iface )
{
    struct dispatcher *impl = impl_from_ICoreDispatcher( iface );

    EnterCriticalSection( &impl->cs );
    impl->quit = TRUE;
    LeaveCriticalSection( &impl->cs );
    SetEvent( impl->wake );
}
