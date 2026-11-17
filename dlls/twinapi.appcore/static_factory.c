/* A WinRT activation factory with statics, for classes that need no state in the factory
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
 * The IUnknown and IInspectable methods of every interface of a factory are the same, so they are written once here
 * and every statics vtable uses them. A static_iface is the object a statics interface pointer designates: its vtable
 * pointer and the factory it belongs to.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

static inline struct static_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct static_factory, IActivationFactory_iface );
}

static struct static_factory *factory_from_iface( void *iface, BOOL *is_factory )
{
    struct static_iface *statics = iface;

    *is_factory = statics->lpVtbl == &static_factory_vtbl;
    if (*is_factory) return impl_from_IActivationFactory( iface );
    return statics->factory;
}

HRESULT WINAPI static_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    BOOL is_factory;
    struct static_factory *impl = factory_from_iface( iface, &is_factory );
    UINT32 i;

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else
        for (i = 0; i < impl->count; i++)
            if (IsEqualGUID( iid, impl->statics[i].iid )) *out = &impl->statics[i].iface;
    if (!*out)
    {
        FIXME( "%s: %s not implemented, returning E_NOINTERFACE.\n", debugstr_w( impl->class_name ), debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

ULONG WINAPI static_factory_AddRef( IActivationFactory *iface )
{
    BOOL is_factory;
    return InterlockedIncrement( &factory_from_iface( iface, &is_factory )->ref );
}

ULONG WINAPI static_factory_Release( IActivationFactory *iface )
{
    BOOL is_factory;
    return InterlockedDecrement( &factory_from_iface( iface, &is_factory )->ref );
}

HRESULT WINAPI static_factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    BOOL is_factory;
    struct static_factory *impl = factory_from_iface( iface, &is_factory );
    IID *list;
    UINT32 i;

    if (!impl->count || !(list = CoTaskMemAlloc( impl->count * sizeof(*list) ))) return impl->count ? E_OUTOFMEMORY : E_NOTIMPL;
    for (i = 0; i < impl->count; i++) list[i] = *impl->statics[i].iid;
    *count = impl->count;
    *iids = list;
    return S_OK;
}

HRESULT WINAPI static_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    BOOL is_factory;
    struct static_factory *impl = factory_from_iface( iface, &is_factory );

    return WindowsCreateString( impl->class_name, wcslen( impl->class_name ), name );
}

HRESULT WINAPI static_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI static_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct static_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, instance %p.\n", iface, instance );
    *instance = NULL;
    return impl->activate ? impl->activate( instance ) : E_NOTIMPL;
}

const struct IActivationFactoryVtbl static_factory_vtbl =
{
    static_factory_QueryInterface,
    static_factory_AddRef,
    static_factory_Release,
    /* IInspectable methods */
    static_factory_GetIids,
    static_factory_GetRuntimeClassName,
    static_factory_GetTrustLevel,
    /* IActivationFactory methods */
    static_factory_ActivateInstance,
};
