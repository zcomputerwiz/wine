/* WinRT Windows.UI.Core.SystemNavigationManager Implementation
 *
 * Copyright (C) 2026 WineExplorer contributors
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

/* The back navigation of the view: the application chooses whether the title bar shows a back button and is told
 * when it is pressed. The window never raises BackRequested; the handlers and the visibility are kept. One manager
 * serves the process (a process has one view). */
struct manager
{
    ISystemNavigationManager ISystemNavigationManager_iface;
    ISystemNavigationManager2 ISystemNavigationManager2_iface;
    LONG ref;
    AppViewBackButtonVisibility visibility;
    struct list handlers;
    CRITICAL_SECTION cs;
};

static const struct ISystemNavigationManagerVtbl manager_vtbl;
static const struct ISystemNavigationManager2Vtbl manager2_vtbl;
static CRITICAL_SECTION_DEBUG manager_cs_debug;
static struct manager manager =
{
    { &manager_vtbl },
    { &manager2_vtbl },
    1,
    AppViewBackButtonVisibility_Disabled,
    LIST_INIT( manager.handlers ),
    { &manager_cs_debug, -1, 0, 0, 0, 0 },
};
static CRITICAL_SECTION_DEBUG manager_cs_debug =
{
    0, 0, &manager.cs, { &manager_cs_debug.ProcessLocksList, &manager_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": manager.cs") }
};

static inline struct manager *impl_from_ISystemNavigationManager( ISystemNavigationManager *iface )
{
    return CONTAINING_RECORD( iface, struct manager, ISystemNavigationManager_iface );
}

static HRESULT WINAPI manager_QueryInterface( ISystemNavigationManager *iface, REFIID iid, void **out )
{
    struct manager *impl = impl_from_ISystemNavigationManager( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ISystemNavigationManager ))
        *out = &impl->ISystemNavigationManager_iface;
    else if (IsEqualGUID( iid, &IID_ISystemNavigationManager2 ))
        *out = &impl->ISystemNavigationManager2_iface;
    if (!*out)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI manager_AddRef( ISystemNavigationManager *iface )
{
    struct manager *impl = impl_from_ISystemNavigationManager( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI manager_Release( ISystemNavigationManager *iface )
{
    struct manager *impl = impl_from_ISystemNavigationManager( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) InterlockedIncrement( &impl->ref );   /* the manager of the process stays */
    return ref ? ref : 1;
}

static HRESULT WINAPI manager_GetIids( ISystemNavigationManager *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_ISystemNavigationManager;
    list[1] = IID_ISystemNavigationManager2;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI manager_GetRuntimeClassName( ISystemNavigationManager *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.SystemNavigationManager", 39, name );
}

static HRESULT WINAPI manager_GetTrustLevel( ISystemNavigationManager *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI manager_add_BackRequested( ISystemNavigationManager *iface,
                                                 IEventHandler_BackRequestedEventArgs *handler,
                                                 EventRegistrationToken *token )
{
    struct manager *impl = impl_from_ISystemNavigationManager( iface );

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return ui_handler_add( &impl->handlers, &impl->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI manager_remove_BackRequested( ISystemNavigationManager *iface, EventRegistrationToken token )
{
    struct manager *impl = impl_from_ISystemNavigationManager( iface );

    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return ui_handler_remove( &impl->handlers, &impl->cs, token );
}

static const struct ISystemNavigationManagerVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    /* IInspectable methods */
    manager_GetIids,
    manager_GetRuntimeClassName,
    manager_GetTrustLevel,
    /* ISystemNavigationManager methods */
    manager_add_BackRequested,
    manager_remove_BackRequested,
};

DEFINE_IINSPECTABLE( manager2, ISystemNavigationManager2, struct manager, ISystemNavigationManager_iface );

static HRESULT WINAPI manager2_get_AppViewBackButtonVisibility( ISystemNavigationManager2 *iface,
                                                                AppViewBackButtonVisibility *value )
{
    struct manager *impl = impl_from_ISystemNavigationManager2( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->visibility;
    return S_OK;
}

static HRESULT WINAPI manager2_put_AppViewBackButtonVisibility( ISystemNavigationManager2 *iface,
                                                                AppViewBackButtonVisibility value )
{
    struct manager *impl = impl_from_ISystemNavigationManager2( iface );

    TRACE( "iface %p, value %d.\n", iface, value );
    if (value < AppViewBackButtonVisibility_Visible || value > AppViewBackButtonVisibility_Disabled)
        return E_INVALIDARG;
    impl->visibility = value;
    return S_OK;
}

static const struct ISystemNavigationManager2Vtbl manager2_vtbl =
{
    manager2_QueryInterface,
    manager2_AddRef,
    manager2_Release,
    /* IInspectable methods */
    manager2_GetIids,
    manager2_GetRuntimeClassName,
    manager2_GetTrustLevel,
    /* ISystemNavigationManager2 methods */
    manager2_get_AppViewBackButtonVisibility,
    manager2_put_AppViewBackButtonVisibility,
};

struct statics
{
    IActivationFactory IActivationFactory_iface;
    ISystemNavigationManagerStatics ISystemNavigationManagerStatics_iface;
    LONG ref;
};

static inline struct statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ISystemNavigationManagerStatics ))
        *out = &impl->ISystemNavigationManagerStatics_iface;
    if (!*out)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_ISystemNavigationManagerStatics;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Core.SystemNavigationManager", 39, name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p: not activatable.\n", iface, instance );
    *instance = NULL;
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

DEFINE_IINSPECTABLE( statics, ISystemNavigationManagerStatics, struct statics, IActivationFactory_iface );

static HRESULT WINAPI statics_GetForCurrentView( ISystemNavigationManagerStatics *iface, ISystemNavigationManager **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );

    *result = &manager.ISystemNavigationManager_iface;
    ISystemNavigationManager_AddRef( *result );
    return S_OK;
}

static const struct ISystemNavigationManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* ISystemNavigationManagerStatics methods */
    statics_GetForCurrentView,
};

static struct statics statics_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *systemnavigation_factory = &statics_impl.IActivationFactory_iface;
