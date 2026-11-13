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
    ICoreWindowInterop ICoreWindowInterop_iface;
    LONG ref;

    HWND hwnd;                  /* the native window, created on first need, on the thread of the window */
    SIZE size;                  /* its client size, for SizeChanged */
    BOOL pointer_inside;        /* the mouse is over the window (PointerEntered raised) */
    struct ui_pointer_state last_pointer;
    UINT32 frame_id;

    struct list entry;          /* the windows of the process */
    DWORD thread_id;
    ICoreDispatcher *dispatcher;

    CRITICAL_SECTION cs;
    BOOL attached;              /* GetForCurrentThread finds it */
    BOOL visible, active, closed, input_enabled, shown;
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

enum args_kind { ARGS_CLOSED, ARGS_ACTIVATED, ARGS_VISIBILITY, ARGS_INPUT_ENABLED, ARGS_KEY, ARGS_CHARACTER, ARGS_SIZE };

struct event_args
{
    ICoreWindowEventArgs ICoreWindowEventArgs_iface;
    IWindowActivatedEventArgs IWindowActivatedEventArgs_iface;
    IVisibilityChangedEventArgs IVisibilityChangedEventArgs_iface;
    IInputEnabledEventArgs IInputEnabledEventArgs_iface;
    IKeyEventArgs IKeyEventArgs_iface;
    ICharacterReceivedEventArgs ICharacterReceivedEventArgs_iface;
    IWindowSizeChangedEventArgs IWindowSizeChangedEventArgs_iface;
    LONG ref;
    enum args_kind kind;
    boolean handled;
    CoreWindowActivationState state;
    boolean visible;        /* VisibilityChanged: visible; InputEnabled: enabled */
    VirtualKey virtual_key;
    UINT32 key_code;
    CorePhysicalKeyStatus key_status;
    Size size;
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
    else if (args->kind == ARGS_INPUT_ENABLED && IsEqualGUID( iid, &IID_IInputEnabledEventArgs ))
        *out = &args->IInputEnabledEventArgs_iface;
    else if (args->kind == ARGS_KEY && IsEqualGUID( iid, &IID_IKeyEventArgs ))
        *out = &args->IKeyEventArgs_iface;
    else if (args->kind == ARGS_CHARACTER && IsEqualGUID( iid, &IID_ICharacterReceivedEventArgs ))
        *out = &args->ICharacterReceivedEventArgs_iface;
    else if (args->kind == ARGS_SIZE && IsEqualGUID( iid, &IID_IWindowSizeChangedEventArgs ))
        *out = &args->IWindowSizeChangedEventArgs_iface;
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
    const WCHAR *class_name;

    switch (args->kind)
    {
    case ARGS_ACTIVATED: class_name = L"Windows.UI.Core.WindowActivatedEventArgs"; break;
    case ARGS_VISIBILITY: class_name = L"Windows.UI.Core.VisibilityChangedEventArgs"; break;
    case ARGS_INPUT_ENABLED: class_name = L"Windows.UI.Core.InputEnabledEventArgs"; break;
    case ARGS_KEY: class_name = L"Windows.UI.Core.KeyEventArgs"; break;
    case ARGS_CHARACTER: class_name = L"Windows.UI.Core.CharacterReceivedEventArgs"; break;
    case ARGS_SIZE: class_name = L"Windows.UI.Core.WindowSizeChangedEventArgs"; break;
    default: class_name = L"Windows.UI.Core.CoreWindowEventArgs"; break;
    }
    return WindowsCreateString( class_name, wcslen( class_name ), name );
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

#define ARGS_COMMON( name, type, member )                                                              \
    static inline struct event_args *args_from_##name( type *iface )                                  \
    {                                                                                                 \
        return CONTAINING_RECORD( iface, struct event_args, member );                                 \
    }                                                                                                 \
    static HRESULT WINAPI name##_args_QueryInterface( type *iface, REFIID iid, void **out )            \
    {                                                                                                 \
        return args_query( args_from_##name( iface ), iid, out );                                     \
    }                                                                                                 \
    static ULONG WINAPI name##_args_AddRef( type *iface )                                              \
    {                                                                                                 \
        return args_addref( args_from_##name( iface ) );                                              \
    }                                                                                                 \
    static ULONG WINAPI name##_args_Release( type *iface )                                             \
    {                                                                                                 \
        return args_release( args_from_##name( iface ) );                                             \
    }                                                                                                 \
    static HRESULT WINAPI name##_args_GetIids( type *iface, ULONG *count, IID **iids )                 \
    {                                                                                                 \
        return E_NOTIMPL;                                                                             \
    }                                                                                                 \
    static HRESULT WINAPI name##_args_GetRuntimeClassName( type *iface, HSTRING *class_name )          \
    {                                                                                                 \
        return args_class_name( args_from_##name( iface ), class_name );                              \
    }                                                                                                 \
    static HRESULT WINAPI name##_args_GetTrustLevel( type *iface, TrustLevel *level )                  \
    {                                                                                                 \
        *level = BaseTrust;                                                                           \
        return S_OK;                                                                                  \
    }

ARGS_COMMON( input_enabled, IInputEnabledEventArgs, IInputEnabledEventArgs_iface )
ARGS_COMMON( key, IKeyEventArgs, IKeyEventArgs_iface )
ARGS_COMMON( character, ICharacterReceivedEventArgs, ICharacterReceivedEventArgs_iface )
ARGS_COMMON( size, IWindowSizeChangedEventArgs, IWindowSizeChangedEventArgs_iface )

static HRESULT WINAPI input_enabled_args_get_InputEnabled( IInputEnabledEventArgs *iface, boolean *value )
{
    *value = args_from_input_enabled( iface )->visible;
    return S_OK;
}

static const IInputEnabledEventArgsVtbl input_enabled_args_vtbl =
{
    input_enabled_args_QueryInterface,
    input_enabled_args_AddRef,
    input_enabled_args_Release,
    input_enabled_args_GetIids,
    input_enabled_args_GetRuntimeClassName,
    input_enabled_args_GetTrustLevel,
    input_enabled_args_get_InputEnabled,
};

static HRESULT WINAPI key_args_get_VirtualKey( IKeyEventArgs *iface, VirtualKey *value )
{
    *value = args_from_key( iface )->virtual_key;
    return S_OK;
}

static HRESULT WINAPI key_args_get_KeyStatus( IKeyEventArgs *iface, CorePhysicalKeyStatus *value )
{
    *value = args_from_key( iface )->key_status;
    return S_OK;
}

static const IKeyEventArgsVtbl key_args_vtbl =
{
    key_args_QueryInterface,
    key_args_AddRef,
    key_args_Release,
    key_args_GetIids,
    key_args_GetRuntimeClassName,
    key_args_GetTrustLevel,
    key_args_get_VirtualKey,
    key_args_get_KeyStatus,
};

static HRESULT WINAPI character_args_get_KeyCode( ICharacterReceivedEventArgs *iface, UINT32 *value )
{
    *value = args_from_character( iface )->key_code;
    return S_OK;
}

static HRESULT WINAPI character_args_get_KeyStatus( ICharacterReceivedEventArgs *iface, CorePhysicalKeyStatus *value )
{
    *value = args_from_character( iface )->key_status;
    return S_OK;
}

static const ICharacterReceivedEventArgsVtbl character_args_vtbl =
{
    character_args_QueryInterface,
    character_args_AddRef,
    character_args_Release,
    character_args_GetIids,
    character_args_GetRuntimeClassName,
    character_args_GetTrustLevel,
    character_args_get_KeyCode,
    character_args_get_KeyStatus,
};

static HRESULT WINAPI size_args_get_Size( IWindowSizeChangedEventArgs *iface, Size *value )
{
    *value = args_from_size( iface )->size;
    return S_OK;
}

static const IWindowSizeChangedEventArgsVtbl size_args_vtbl =
{
    size_args_QueryInterface,
    size_args_AddRef,
    size_args_Release,
    size_args_GetIids,
    size_args_GetRuntimeClassName,
    size_args_GetTrustLevel,
    size_args_get_Size,
};

static struct event_args *args_create( enum args_kind kind )
{
    struct event_args *args;

    if (!(args = calloc( 1, sizeof(*args) ))) return NULL;
    args->IInputEnabledEventArgs_iface.lpVtbl = &input_enabled_args_vtbl;
    args->IKeyEventArgs_iface.lpVtbl = &key_args_vtbl;
    args->ICharacterReceivedEventArgs_iface.lpVtbl = &character_args_vtbl;
    args->IWindowSizeChangedEventArgs_iface.lpVtbl = &size_args_vtbl;
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
        case EVENT_INPUT_ENABLED:
            ITypedEventHandler_CoreWindow_InputEnabledEventArgs_Invoke(
                (ITypedEventHandler_CoreWindow_InputEnabledEventArgs *)handlers[i], &window->ICoreWindow_iface,
                &args->IInputEnabledEventArgs_iface );
            break;
        case EVENT_KEY_DOWN:
        case EVENT_KEY_UP:
            ITypedEventHandler_CoreWindow_KeyEventArgs_Invoke(
                (ITypedEventHandler_CoreWindow_KeyEventArgs *)handlers[i], &window->ICoreWindow_iface,
                &args->IKeyEventArgs_iface );
            break;
        case EVENT_CHARACTER_RECEIVED:
            ITypedEventHandler_CoreWindow_CharacterReceivedEventArgs_Invoke(
                (ITypedEventHandler_CoreWindow_CharacterReceivedEventArgs *)handlers[i], &window->ICoreWindow_iface,
                &args->ICharacterReceivedEventArgs_iface );
            break;
        case EVENT_SIZE_CHANGED:
            ITypedEventHandler_CoreWindow_WindowSizeChangedEventArgs_Invoke(
                (ITypedEventHandler_CoreWindow_WindowSizeChangedEventArgs *)handlers[i], &window->ICoreWindow_iface,
                &args->IWindowSizeChangedEventArgs_iface );
            break;
        default:
            break;
        }
    }
    ui_handler_snapshot_free( handlers, count );
}

enum window_op { OP_VISIBILITY, OP_ACTIVATED, OP_CLOSED, OP_INPUT_ENABLED };

struct window_event_item
{
    struct core_window *window;
    enum window_op op;
    boolean visible;
    CoreWindowActivationState state;
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
            args->state = item->state;
            window_raise( window, EVENT_ACTIVATED, args );
        }
        break;
    case OP_INPUT_ENABLED:
        if ((args = args_create( ARGS_INPUT_ENABLED )))
        {
            args->visible = item->visible;
            window_raise( window, EVENT_INPUT_ENABLED, args );
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

static void window_post_state( struct core_window *window, enum window_op op, boolean visible,
                               CoreWindowActivationState state )
{
    struct window_event_item *item;

    if (!(item = calloc( 1, sizeof(*item) ))) return;
    item->window = window;
    item->op = op;
    item->visible = visible;
    item->state = state;
    ICoreWindow_AddRef( &window->ICoreWindow_iface );
    if (FAILED(ui_dispatcher_post( window->dispatcher, CoreDispatcherPriority_Normal, window_event_run, item )))
    {
        ICoreWindow_Release( &window->ICoreWindow_iface );
        free( item );
    }
}

static void window_post( struct core_window *window, enum window_op op, boolean visible )
{
    window_post_state( window, op, visible, CoreWindowActivationState_CodeActivated );
}

/* ---- the native window ---------------------------------------------------------------------------- */

#define CORE_WINDOW_CLASS L"Windows.UI.Core.CoreWindow"
#define DEFAULT_CLIENT_WIDTH 1200
#define DEFAULT_CLIENT_HEIGHT 900

static HRESULT window_close_internal( struct core_window *impl );
static BOOL CALLBACK register_window_class( INIT_ONCE *once, void *param, void **context );
static LRESULT CALLBACK core_window_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp );

static struct core_window *window_from_hwnd( HWND hwnd )
{
    return (struct core_window *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
}

/* the client rectangle in screen coordinates; with no native window yet, the size the window will have */
static void window_get_client_rect( struct core_window *window, RECT *rect )
{
    if (window->hwnd)
    {
        POINT origin = { 0, 0 };

        GetClientRect( window->hwnd, rect );
        ClientToScreen( window->hwnd, &origin );
        OffsetRect( rect, origin.x, origin.y );
        return;
    }
    rect->left = rect->top = 0;
    rect->right = min( DEFAULT_CLIENT_WIDTH, GetSystemMetrics( SM_CXSCREEN ) );
    rect->bottom = min( DEFAULT_CLIENT_HEIGHT, GetSystemMetrics( SM_CYSCREEN ) );
}

/* The native window is created on the thread of the window, when the application first needs it (Activate or
 * ICoreWindowInterop::WindowHandle), hidden. */
static BOOL window_ensure_native( struct core_window *window )
{
    static INIT_ONCE class_once = INIT_ONCE_STATIC_INIT;
    WCHAR title[MAX_PATH], *name;
    RECT rect;
    DWORD style = WS_OVERLAPPEDWINDOW;

    if (window->hwnd) return TRUE;
    if (window->thread_id != GetCurrentThreadId()) return FALSE;
    InitOnceExecuteOnce( &class_once, register_window_class, NULL, NULL );

    GetModuleFileNameW( NULL, title, ARRAY_SIZE(title) );
    name = wcsrchr( title, '\\' );
    name = name ? name + 1 : title;

    window_get_client_rect( window, &rect );
    AdjustWindowRectEx( &rect, style, FALSE, 0 );
    window->hwnd = CreateWindowExW( 0, CORE_WINDOW_CLASS, name, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                    rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, NULL, window );
    if (!window->hwnd)
    {
        ERR( "failed to create the native window, error %lu\n", GetLastError() );
        return FALSE;
    }
    window_get_client_rect( window, &rect );
    window->size.cx = rect.right - rect.left;
    window->size.cy = rect.bottom - rect.top;
    return TRUE;
}

static BOOL CALLBACK register_window_class( INIT_ONCE *once, void *param, void **context )
{
    WNDCLASSEXW cls = { sizeof(cls) };

    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = core_window_proc;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = CORE_WINDOW_CLASS;
    RegisterClassExW( &cls );
    return TRUE;
}

static void window_raise_key( struct core_window *window, enum window_event event, enum args_kind kind, WPARAM wp,
                              LPARAM lp, BOOL released, BOOL *handled )
{
    struct event_args *args;

    if (!(args = args_create( kind ))) return;
    if (kind == ARGS_KEY) args->virtual_key = wp;
    else args->key_code = wp;
    args->key_status.RepeatCount = LOWORD( lp );
    args->key_status.ScanCode = (lp >> 16) & 0xff;
    args->key_status.IsExtendedKey = !!(lp & (1 << 24));
    args->key_status.IsMenuKeyDown = !!(lp & (1 << 29));
    args->key_status.WasKeyDown = !!(lp & (1 << 30));
    args->key_status.IsKeyReleased = released;
    window_raise( window, event, args );
    *handled = args->handled;
    args_release( args );
}

static VirtualKeyModifiers current_modifiers(void)
{
    VirtualKeyModifiers modifiers = VirtualKeyModifiers_None;

    if (GetKeyState( VK_CONTROL ) & 0x8000) modifiers |= VirtualKeyModifiers_Control;
    if (GetKeyState( VK_MENU ) & 0x8000) modifiers |= VirtualKeyModifiers_Menu;
    if (GetKeyState( VK_SHIFT ) & 0x8000) modifiers |= VirtualKeyModifiers_Shift;
    if ((GetKeyState( VK_LWIN ) | GetKeyState( VK_RWIN )) & 0x8000) modifiers |= VirtualKeyModifiers_Windows;
    return modifiers;
}

/* a pointer event of the mouse; x and y are client coordinates; the buttons are the MK_ flags of the message */
static BOOL window_raise_pointer( struct core_window *window, enum window_event event, int x, int y, WPARAM keys,
                                  PointerUpdateKind kind, int wheel_delta, BOOL horizontal )
{
    IPointerEventArgs *args;
    struct ui_pointer_state state = {0};
    unsigned int i, count;
    IUnknown **handlers;
    BOOL handled;

    state.x = x;
    state.y = y;
    state.pointer_id = 1;
    state.frame_id = ++window->frame_id;
    state.timestamp = GetTickCount64() * 1000;
    state.left = !!(keys & MK_LBUTTON);
    state.right = !!(keys & MK_RBUTTON);
    state.middle = !!(keys & MK_MBUTTON);
    state.x1 = !!(keys & MK_XBUTTON1);
    state.x2 = !!(keys & MK_XBUTTON2);
    state.wheel_delta = wheel_delta;
    state.horizontal_wheel = horizontal;
    state.update_kind = kind;
    state.modifiers = current_modifiers();
    window->last_pointer = state;

    if (!(args = ui_pointer_args_create( &state ))) return FALSE;
    handlers = ui_handler_snapshot( &window->handlers[event], &window->cs, &count );
    for (i = 0; i < count; i++)
        ITypedEventHandler_CoreWindow_PointerEventArgs_Invoke( (ITypedEventHandler_CoreWindow_PointerEventArgs *)handlers[i],
                                                                &window->ICoreWindow_iface, args );
    ui_handler_snapshot_free( handlers, count );
    handled = ui_pointer_args_handled( args );
    IPointerEventArgs_Release( args );
    return handled;
}

static void window_set_active( struct core_window *window, BOOL active, CoreWindowActivationState state )
{
    BOOL changed;

    EnterCriticalSection( &window->cs );
    changed = window->active != active;
    window->active = active;
    LeaveCriticalSection( &window->cs );
    if (changed) window_post_state( window, OP_ACTIVATED, 0, state );
}

static void window_set_visible( struct core_window *window, BOOL visible )
{
    BOOL changed;

    EnterCriticalSection( &window->cs );
    changed = window->visible != visible;
    window->visible = visible;
    LeaveCriticalSection( &window->cs );
    if (changed) window_post( window, OP_VISIBILITY, visible );
}

static LRESULT CALLBACK core_window_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct core_window *window = msg == WM_NCCREATE ? NULL : window_from_hwnd( hwnd );
    BOOL handled = FALSE;

    if (msg == WM_NCCREATE)
    {
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams );
        return DefWindowProcW( hwnd, msg, wp, lp );
    }
    if (!window || window->closed) return DefWindowProcW( hwnd, msg, wp, lp );

    switch (msg)
    {
    case WM_ACTIVATE:
        if (LOWORD( wp ) == WA_INACTIVE) window_set_active( window, FALSE, CoreWindowActivationState_Deactivated );
        else window_set_active( window, TRUE, LOWORD( wp ) == WA_CLICKACTIVE ?
                                CoreWindowActivationState_PointerActivated : CoreWindowActivationState_CodeActivated );
        break;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) window_set_visible( window, FALSE );
        else
        {
            if (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) window_set_visible( window, IsWindowVisible( hwnd ) );
            if ((int)LOWORD( lp ) != window->size.cx || (int)HIWORD( lp ) != window->size.cy)
            {
                struct event_args *args;

                window->size.cx = LOWORD( lp );
                window->size.cy = HIWORD( lp );
                if ((args = args_create( ARGS_SIZE )))
                {
                    args->size.Width = window->size.cx;
                    args->size.Height = window->size.cy;
                    window_raise( window, EVENT_SIZE_CHANGED, args );
                    args_release( args );
                }
            }
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        window_raise_key( window, EVENT_KEY_DOWN, ARGS_KEY, wp, lp, FALSE, &handled );
        if (handled && msg == WM_KEYDOWN) return 0;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        window_raise_key( window, EVENT_KEY_UP, ARGS_KEY, wp, lp, TRUE, &handled );
        if (handled && msg == WM_KEYUP) return 0;
        break;
    case WM_CHAR:
        window_raise_key( window, EVENT_CHARACTER_RECEIVED, ARGS_CHARACTER, wp, lp, FALSE, &handled );
        return 0;

    case WM_MOUSEMOVE:
        if (!window->pointer_inside)
        {
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };

            window->pointer_inside = TRUE;
            TrackMouseEvent( &track );
            window_raise_pointer( window, EVENT_POINTER_ENTERED, (short)LOWORD( lp ), (short)HIWORD( lp ), wp,
                                  PointerUpdateKind_Other, 0, FALSE );
        }
        window_raise_pointer( window, EVENT_POINTER_MOVED, (short)LOWORD( lp ), (short)HIWORD( lp ), wp,
                              PointerUpdateKind_Other, 0, FALSE );
        return 0;
    case WM_MOUSELEAVE:
        window->pointer_inside = FALSE;
        window_raise_pointer( window, EVENT_POINTER_EXITED, window->last_pointer.x, window->last_pointer.y,
                              0, PointerUpdateKind_Other, 0, FALSE );
        return 0;

#define POINTER_BUTTON( down_msg, dblclk_msg, up_msg, down_kind, up_kind )                                  \
    case down_msg:                                                                                           \
    case dblclk_msg:                                                                                         \
        window_raise_pointer( window, EVENT_POINTER_PRESSED, (short)LOWORD( lp ), (short)HIWORD( lp ), wp,   \
                              down_kind, 0, FALSE );                                                         \
        return 0;                                                                                            \
    case up_msg:                                                                                             \
        window_raise_pointer( window, EVENT_POINTER_RELEASED, (short)LOWORD( lp ), (short)HIWORD( lp ), wp,  \
                              up_kind, 0, FALSE );                                                           \
        return 0;
    POINTER_BUTTON( WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONUP, PointerUpdateKind_LeftButtonPressed,
                    PointerUpdateKind_LeftButtonReleased )
    POINTER_BUTTON( WM_RBUTTONDOWN, WM_RBUTTONDBLCLK, WM_RBUTTONUP, PointerUpdateKind_RightButtonPressed,
                    PointerUpdateKind_RightButtonReleased )
    POINTER_BUTTON( WM_MBUTTONDOWN, WM_MBUTTONDBLCLK, WM_MBUTTONUP, PointerUpdateKind_MiddleButtonPressed,
                    PointerUpdateKind_MiddleButtonReleased )
#undef POINTER_BUTTON

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };   /* screen coordinates */

        ScreenToClient( hwnd, &pt );
        window_raise_pointer( window, EVENT_POINTER_WHEEL_CHANGED, pt.x, pt.y, LOWORD( wp ), PointerUpdateKind_Other,
                              GET_WHEEL_DELTA_WPARAM( wp ), msg == WM_MOUSEHWHEEL );
        return 0;
    }

    case WM_CLOSE:
        /* the system closes the window of the view and the application ends: Closed, then the dispatcher quits */
        window_close_internal( window );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

/* ---- ICoreWindowInterop --------------------------------------------------------------------------- */

static inline struct core_window *impl_from_ICoreWindowInterop( ICoreWindowInterop *iface )
{
    return CONTAINING_RECORD( iface, struct core_window, ICoreWindowInterop_iface );
}

static HRESULT WINAPI interop_QueryInterface( ICoreWindowInterop *iface, REFIID iid, void **out )
{
    return ICoreWindow_QueryInterface( &impl_from_ICoreWindowInterop( iface )->ICoreWindow_iface, iid, out );
}

static ULONG WINAPI interop_AddRef( ICoreWindowInterop *iface )
{
    return ICoreWindow_AddRef( &impl_from_ICoreWindowInterop( iface )->ICoreWindow_iface );
}

static ULONG WINAPI interop_Release( ICoreWindowInterop *iface )
{
    return ICoreWindow_Release( &impl_from_ICoreWindowInterop( iface )->ICoreWindow_iface );
}

static HRESULT WINAPI interop_get_WindowHandle( ICoreWindowInterop *iface, HWND *hwnd )
{
    struct core_window *impl = impl_from_ICoreWindowInterop( iface );

    TRACE( "iface %p, hwnd %p.\n", iface, hwnd );
    if (!hwnd) return E_POINTER;
    *hwnd = NULL;
    if (impl->closed) return E_ILLEGAL_METHOD_CALL;
    if (!window_ensure_native( impl )) return RPC_E_WRONG_THREAD;
    *hwnd = impl->hwnd;
    return S_OK;
}

static HRESULT WINAPI interop_put_MessageHandled( ICoreWindowInterop *iface, boolean value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    return S_OK;
}

static const ICoreWindowInteropVtbl interop_vtbl =
{
    interop_QueryInterface,
    interop_AddRef,
    interop_Release,
    interop_get_WindowHandle,
    interop_put_MessageHandled,
};

/* ---- ICoreWindow --------------------------------------------------------------------------------- */

static HRESULT WINAPI window_QueryInterface( ICoreWindow *iface, REFIID iid, void **out )
{
    struct core_window *impl = impl_from_ICoreWindow( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_ICoreWindowInterop ))
    {
        *out = &impl->ICoreWindowInterop_iface;
        ICoreWindow_AddRef( iface );
        return S_OK;
    }
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
    RECT rect;

    TRACE( "iface %p, value %p.\n", iface, value );
    /* Observed on Windows 11: the client area of the window, 1200x900 for a window of the default size (before
     * the native window exists, the size it will have) */
    window_get_client_rect( impl_from_ICoreWindow( iface ), &rect );
    value->X = rect.left;
    value->Y = rect.top;
    value->Width = rect.right - rect.left;
    value->Height = rect.bottom - rect.top;
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
    BOOL first;

    TRACE( "iface %p.\n", iface );

    if (impl->thread_id != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (impl->closed) return S_OK;
    if (!window_ensure_native( impl )) return E_FAIL;

    /* Observed on Windows 11 with an unlocked desktop: Activate shows the window; VisibilityChanged(true),
     * Activated (CodeActivated) and InputEnabled(true) follow, in that order, once events are processed; no
     * SizeChanged. */
    EnterCriticalSection( &impl->cs );
    first = !impl->shown;
    impl->shown = TRUE;
    LeaveCriticalSection( &impl->cs );

    window_set_visible( impl, TRUE );
    ShowWindow( impl->hwnd, IsIconic( impl->hwnd ) ? SW_RESTORE : SW_SHOW );
    SetForegroundWindow( impl->hwnd );
    window_set_active( impl, TRUE, CoreWindowActivationState_CodeActivated );
    if (first) window_post( impl, OP_INPUT_ENABLED, impl->input_enabled );
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
    if (impl->hwnd)
    {
        HWND hwnd = impl->hwnd;

        impl->hwnd = NULL;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, 0 );
        DestroyWindow( hwnd );
    }
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
    window->ICoreWindowInterop_iface.lpVtbl = &interop_vtbl;
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
