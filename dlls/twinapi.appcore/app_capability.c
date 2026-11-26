/* WinRT Windows.Security.Authorization.AppCapabilityAccess.AppCapability Implementation
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

/*
 * Measured on Windows 11 for an unpackaged desktop application: every capability name is Allowed, for CheckAccess and for
 * RequestAccessAsync (which shows no prompt), a name that is not a capability too; an empty name (Create) and a process
 * id of 0 are E_INVALIDARG; the user is the current one. DisplayMessage is a plain property. The access never changes, so
 * AccessChanged is accepted and never raised. The two RequestAccessForCapabilities methods (maps of results) are not
 * implemented.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct app_capability
{
    struct simple_object base;
    IAppCapability2 IAppCapability2_iface;
    HSTRING name;
    HSTRING display_message;
    IUser *user;
};

static CRITICAL_SECTION changed_cs;
static CRITICAL_SECTION_DEBUG changed_cs_debug =
{
    0, 0, &changed_cs,
    { &changed_cs_debug.ProcessLocksList, &changed_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": changed_cs") }
};
static CRITICAL_SECTION changed_cs = { &changed_cs_debug, -1, 0, 0, 0, 0 };
static struct list changed_handlers = LIST_INIT( changed_handlers );

DEFINE_SIMPLE_IINSPECTABLE( capability_base, IAppCapability )

static inline struct app_capability *impl_from_IAppCapability2( IAppCapability2 *iface )
{
    return CONTAINING_RECORD( iface, struct app_capability, IAppCapability2_iface );
}

static HRESULT WINAPI capability_QueryInterface( IAppCapability *iface, REFIID iid, void **out )
{
    struct app_capability *impl = (struct app_capability *)iface;

    if (IsEqualGUID( iid, &IID_IAppCapability2 ))
    {
        IAppCapability2_AddRef( (*out = &impl->IAppCapability2_iface) );
        return S_OK;
    }
    return capability_base_QueryInterface( iface, iid, out );
}

static HRESULT WINAPI capability_get_CapabilityName( IAppCapability *iface, HSTRING *value )
{
    return WindowsDuplicateString( ((struct app_capability *)iface)->name, value );
}

static HRESULT WINAPI capability_get_User( IAppCapability *iface, IUser **value )
{
    IUser_AddRef( (*value = ((struct app_capability *)iface)->user) );
    return S_OK;
}

static HRESULT WINAPI capability_RequestAccessAsync( IAppCapability *iface, IAsyncOperation_AppCapabilityAccessStatus **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return async_access_status_completed( AppCapabilityAccessStatus_Allowed, operation );
}

static HRESULT WINAPI capability_CheckAccess( IAppCapability *iface, AppCapabilityAccessStatus *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    *result = AppCapabilityAccessStatus_Allowed;
    return S_OK;
}

static HRESULT WINAPI capability_add_AccessChanged( IAppCapability *iface, ITypedEventHandler_AppCapability_AppCapabilityAccessChangedEventArgs *handler,
                                                    EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!handler) return E_INVALIDARG;
    return handler_list_add( &changed_handlers, &changed_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI capability_remove_AccessChanged( IAppCapability *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return handler_list_remove( &changed_handlers, &changed_cs, token );
}

static const IAppCapabilityVtbl capability_vtbl =
{
    capability_QueryInterface,
    capability_base_AddRef,
    capability_base_Release,
    /* IInspectable methods */
    capability_base_GetIids,
    capability_base_GetRuntimeClassName,
    capability_base_GetTrustLevel,
    /* IAppCapability methods */
    capability_get_CapabilityName,
    capability_get_User,
    capability_RequestAccessAsync,
    capability_CheckAccess,
    capability_add_AccessChanged,
    capability_remove_AccessChanged,
};

static HRESULT WINAPI capability2_QueryInterface( IAppCapability2 *iface, REFIID iid, void **out )
{
    return capability_QueryInterface( (IAppCapability *)impl_from_IAppCapability2( iface ), iid, out );
}

static ULONG WINAPI capability2_AddRef( IAppCapability2 *iface )
{
    return simple_object_AddRef( (IInspectable *)impl_from_IAppCapability2( iface ) );
}

static ULONG WINAPI capability2_Release( IAppCapability2 *iface )
{
    return simple_object_Release( (IInspectable *)impl_from_IAppCapability2( iface ) );
}

static HRESULT WINAPI capability2_GetIids( IAppCapability2 *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IAppCapability;
    list[1] = IID_IAppCapability2;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI capability2_GetRuntimeClassName( IAppCapability2 *iface, HSTRING *name )
{
    return create_name( L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability", name );
}

static HRESULT WINAPI capability2_GetTrustLevel( IAppCapability2 *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI capability2_get_DisplayMessage( IAppCapability2 *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IAppCapability2( iface )->display_message, value );
}

static HRESULT WINAPI capability2_put_DisplayMessage( IAppCapability2 *iface, HSTRING value )
{
    struct app_capability *impl = impl_from_IAppCapability2( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->display_message );
    impl->display_message = copy;
    return S_OK;
}

static const IAppCapability2Vtbl capability2_vtbl =
{
    capability2_QueryInterface,
    capability2_AddRef,
    capability2_Release,
    /* IInspectable methods */
    capability2_GetIids,
    capability2_GetRuntimeClassName,
    capability2_GetTrustLevel,
    /* IAppCapability2 methods */
    capability2_get_DisplayMessage,
    capability2_put_DisplayMessage,
};

static void capability_destroy( struct simple_object *object )
{
    struct app_capability *impl = (struct app_capability *)object;

    WindowsDeleteString( impl->name );
    WindowsDeleteString( impl->display_message );
    if (impl->user) IUser_Release( impl->user );
}

static HRESULT capability_create( IUser *user, HSTRING name, IAppCapability **out )
{
    struct simple_object *object;
    struct app_capability *impl;
    HRESULT hr;

    *out = NULL;
    if (!WindowsGetStringLen( name )) return E_INVALIDARG;
    if (FAILED(hr = simple_object_create( sizeof(*impl), &capability_vtbl, &IID_IAppCapability,
                                          L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability",
                                          capability_destroy, &object ))) return hr;
    impl = (struct app_capability *)object;
    impl->IAppCapability2_iface.lpVtbl = &capability2_vtbl;
    if (FAILED(hr = WindowsDuplicateString( name, &impl->name ))) goto failed;
    if (user) IUser_AddRef( (impl->user = user) );
    else if (FAILED(hr = user_create( &impl->user ))) goto failed;
    *out = (IAppCapability *)object;
    return S_OK;

failed:
    IAppCapability_Release( (IAppCapability *)object );
    return hr;
}

static HRESULT WINAPI statics_RequestAccessForCapabilitiesAsync( IAppCapabilityStatics *iface, IIterable_HSTRING *names,
                                                                 IAsyncOperation_IMapView_HSTRING_AppCapabilityAccessStatus **operation )
{
    FIXME( "iface %p, names %p, operation %p: not implemented.\n", iface, names, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_RequestAccessForCapabilitiesForUserAsync( IAppCapabilityStatics *iface, IUser *user,
                                                                        IIterable_HSTRING *names,
                                                                        IAsyncOperation_IMapView_HSTRING_AppCapabilityAccessStatus **operation )
{
    FIXME( "iface %p, user %p, names %p, operation %p: not implemented.\n", iface, user, names, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_Create( IAppCapabilityStatics *iface, HSTRING name, IAppCapability **result )
{
    TRACE( "iface %p, name %s, result %p.\n", iface, debugstr_hstring( name ), result );
    return capability_create( NULL, name, result );
}

static HRESULT WINAPI statics_CreateWithProcessIdForUser( IAppCapabilityStatics *iface, IUser *user, HSTRING name, UINT32 pid,
                                                          IAppCapability **result )
{
    TRACE( "iface %p, user %p, name %s, pid %lu, result %p.\n", iface, user, debugstr_hstring( name ), (unsigned long)pid, result );
    *result = NULL;
    if (!pid) return E_INVALIDARG;
    return capability_create( user, name, result );
}

DEFINE_STATICS_IINSPECTABLE( statics, IAppCapabilityStatics )

static const IAppCapabilityStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IAppCapabilityStatics methods */
    statics_RequestAccessForCapabilitiesAsync,
    statics_RequestAccessForCapabilitiesForUserAsync,
    statics_Create,
    statics_CreateWithProcessIdForUser,
};

static struct static_factory app_capability_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.Security.Authorization.AppCapabilityAccess.AppCapability", NULL, 1,
    {{ &IID_IAppCapabilityStatics, { &statics_vtbl, &app_capability_static_factory } }},
};

IActivationFactory *app_capability_factory = &app_capability_static_factory.IActivationFactory_iface;
