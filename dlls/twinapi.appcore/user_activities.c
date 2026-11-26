/* WinRT Windows.ApplicationModel.UserActivities.UserActivityRequestManager (WineExplorer UWP compatibility)
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
 * UserActivityRequestManager.GetForCurrentView: one manager per view thread, the same object on every call, and
 * ERROR_NOT_FOUND off a view's thread, as on Windows 11 (XAML probe). Nothing asks applications for user activities
 * (there is no Timeline), so UserActivityRequested is accepted and never raised.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct request_manager
{
    IUserActivityRequestManager IUserActivityRequestManager_iface;
    LONG ref;
    struct list entry;
    DWORD thread_id;
    LONG token;
};

static CRITICAL_SECTION managers_cs;
static CRITICAL_SECTION_DEBUG managers_cs_debug =
{
    0, 0, &managers_cs,
    { &managers_cs_debug.ProcessLocksList, &managers_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": managers_cs") }
};
static CRITICAL_SECTION managers_cs = { &managers_cs_debug, -1, 0, 0, 0, 0 };
static struct list managers = LIST_INIT( managers );

static inline struct request_manager *impl_from_IUserActivityRequestManager( IUserActivityRequestManager *iface )
{
    return CONTAINING_RECORD( iface, struct request_manager, IUserActivityRequestManager_iface );
}

static HRESULT WINAPI manager_QueryInterface( IUserActivityRequestManager *iface, REFIID iid, void **out )
{
    struct request_manager *impl = impl_from_IUserActivityRequestManager( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IUserActivityRequestManager ))
    {
        IUserActivityRequestManager_AddRef( (*out = &impl->IUserActivityRequestManager_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI manager_AddRef( IUserActivityRequestManager *iface )
{
    return InterlockedIncrement( &impl_from_IUserActivityRequestManager( iface )->ref );
}

/* the managers live as long as the process: the list keeps a reference */
static ULONG WINAPI manager_Release( IUserActivityRequestManager *iface )
{
    return InterlockedDecrement( &impl_from_IUserActivityRequestManager( iface )->ref );
}

static HRESULT WINAPI manager_GetIids( IUserActivityRequestManager *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_GetRuntimeClassName( IUserActivityRequestManager *iface, HSTRING *class_name )
{
    const WCHAR *name = RuntimeClass_Windows_ApplicationModel_UserActivities_UserActivityRequestManager;
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI manager_GetTrustLevel( IUserActivityRequestManager *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI manager_add_UserActivityRequested( IUserActivityRequestManager *iface,
        ITypedEventHandler_UserActivityRequestManager_UserActivityRequestedEventArgs *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p: never raised.\n", iface, handler, token );
    if (!handler || !token) return E_INVALIDARG;
    token->value = InterlockedIncrement( &impl_from_IUserActivityRequestManager( iface )->token );
    return S_OK;
}

static HRESULT WINAPI manager_remove_UserActivityRequested( IUserActivityRequestManager *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static const IUserActivityRequestManagerVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    /* IInspectable methods */
    manager_GetIids,
    manager_GetRuntimeClassName,
    manager_GetTrustLevel,
    /* IUserActivityRequestManager methods */
    manager_add_UserActivityRequested,
    manager_remove_UserActivityRequested,
};

/*
 * IUserActivityRequestManagerInterop {dd69f876-9699-4715-9095-e37ea30dfa1b} (useractivityinterop.h of the Windows SDK): the second
 * interface of the factory, HRESULT GetForWindow(HWND window, REFIID riid, void **value) after the IInspectable methods. Microsoft.UI.Xaml
 * (Notepad of the Windows App Runtime) asks for it with its window and the IID of IUserActivityRequestManager, and gets the manager of
 * the thread of the window. Observed on Windows 11 with a debugger trace (tests/uwp/alpc/wintrace.c, factory-iid-probe.c).
 */
typedef struct IUserActivityRequestManagerInterop IUserActivityRequestManagerInterop;
struct IUserActivityRequestManagerInteropVtbl
{
    HRESULT (WINAPI *QueryInterface)( IUserActivityRequestManagerInterop *iface, REFIID iid, void **out );
    ULONG (WINAPI *AddRef)( IUserActivityRequestManagerInterop *iface );
    ULONG (WINAPI *Release)( IUserActivityRequestManagerInterop *iface );
    HRESULT (WINAPI *GetIids)( IUserActivityRequestManagerInterop *iface, ULONG *iid_count, IID **iids );
    HRESULT (WINAPI *GetRuntimeClassName)( IUserActivityRequestManagerInterop *iface, HSTRING *class_name );
    HRESULT (WINAPI *GetTrustLevel)( IUserActivityRequestManagerInterop *iface, TrustLevel *trust_level );
    HRESULT (WINAPI *GetForWindow)( IUserActivityRequestManagerInterop *iface, HWND window, REFIID riid, void **value );
};
struct IUserActivityRequestManagerInterop
{
    const struct IUserActivityRequestManagerInteropVtbl *lpVtbl;
};
static const GUID IID_IUserActivityRequestManagerInterop =
    {0xdd69f876, 0x9699, 0x4715, {0x90, 0x95, 0xe3, 0x7e, 0xa3, 0x0d, 0xfa, 0x1b}};

struct factory
{
    IActivationFactory IActivationFactory_iface;
    IUserActivityRequestManagerStatics IUserActivityRequestManagerStatics_iface;
    IUserActivityRequestManagerInterop IUserActivityRequestManagerInterop_iface;
    LONG ref;
};

static inline struct factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IUserActivityRequestManagerStatics ))
        *out = &impl->IUserActivityRequestManagerStatics_iface;
    else if (IsEqualGUID( iid, &IID_IUserActivityRequestManagerInterop ))
        *out = &impl->IUserActivityRequestManagerInterop_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
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
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
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

DEFINE_IINSPECTABLE( statics, IUserActivityRequestManagerStatics, struct factory, IActivationFactory_iface )

/* the manager of a thread: made on the first call and kept (the same object on every call) */
static HRESULT manager_for_thread( DWORD thread_id, IUserActivityRequestManager **result )
{
    struct request_manager *manager, *found = NULL;

    EnterCriticalSection( &managers_cs );
    LIST_FOR_EACH_ENTRY( manager, &managers, struct request_manager, entry )
        if (manager->thread_id == thread_id) { found = manager; break; }
    if (!found && (found = calloc( 1, sizeof(*found) )))
    {
        found->IUserActivityRequestManager_iface.lpVtbl = &manager_vtbl;
        found->ref = 1;   /* the list's */
        found->thread_id = thread_id;
        list_add_tail( &managers, &found->entry );
    }
    if (found) IUserActivityRequestManager_AddRef( (*result = &found->IUserActivityRequestManager_iface) );
    LeaveCriticalSection( &managers_cs );
    return found ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI statics_GetForCurrentView( IUserActivityRequestManagerStatics *iface, IUserActivityRequestManager **result )
{
    ICoreApplicationView *core = core_view_for_current_thread();

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    *result = NULL;
    if (!core) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    ICoreApplicationView_Release( core );

    return manager_for_thread( GetCurrentThreadId(), result );
}

static const IUserActivityRequestManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IUserActivityRequestManagerStatics methods */
    statics_GetForCurrentView,
};

DEFINE_IINSPECTABLE( interop, IUserActivityRequestManagerInterop, struct factory, IActivationFactory_iface )

static HRESULT WINAPI interop_GetForWindow( IUserActivityRequestManagerInterop *iface, HWND window, REFIID riid, void **value )
{
    IUserActivityRequestManager *manager;
    DWORD thread_id, process_id;
    HRESULT hr;

    TRACE( "iface %p, window %p, riid %s, value %p.\n", iface, window, debugstr_guid( riid ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!window || !riid) return E_INVALIDARG;
    if (!(thread_id = GetWindowThreadProcessId( window, &process_id ))) return HRESULT_FROM_WIN32( ERROR_INVALID_WINDOW_HANDLE );
    if (process_id != GetCurrentProcessId()) return E_INVALIDARG;   /* a window of another process has no manager here */

    if (FAILED(hr = manager_for_thread( thread_id, &manager ))) return hr;
    hr = IUserActivityRequestManager_QueryInterface( manager, riid, value );
    IUserActivityRequestManager_Release( manager );
    return hr;
}

static const struct IUserActivityRequestManagerInteropVtbl interop_vtbl =
{
    interop_QueryInterface,
    interop_AddRef,
    interop_Release,
    /* IInspectable methods */
    interop_GetIids,
    interop_GetRuntimeClassName,
    interop_GetTrustLevel,
    /* IUserActivityRequestManagerInterop methods */
    interop_GetForWindow,
};

static struct factory factory =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&interop_vtbl},
    1,
};

IActivationFactory *user_activity_request_manager_factory = &factory.IActivationFactory_iface;
