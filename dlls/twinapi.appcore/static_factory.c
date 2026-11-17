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

    /* a factory without statics has the activation factory interface only */
    if (!(list = CoTaskMemAlloc( max( impl->count, 1 ) * sizeof(*list) ))) return E_OUTOFMEMORY;
    for (i = 0; i < impl->count; i++) list[i] = *impl->statics[i].iid;
    if (!impl->count) list[0] = IID_IActivationFactory;
    *count = max( impl->count, 1 );
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

/* ---- an object with one interface (and no state to share) ---- */

HRESULT WINAPI simple_object_QueryInterface( IInspectable *iface, REFIID iid, void **out )
{
    struct simple_object *impl = (struct simple_object *)iface;

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, impl->iid ))
    {
        IInspectable_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

ULONG WINAPI simple_object_AddRef( IInspectable *iface )
{
    return InterlockedIncrement( &((struct simple_object *)iface)->ref );
}

ULONG WINAPI simple_object_Release( IInspectable *iface )
{
    struct simple_object *impl = (struct simple_object *)iface;
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->destroy) impl->destroy( impl );
        free( impl );
    }
    return ref;
}

HRESULT WINAPI simple_object_GetIids( IInspectable *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = *((struct simple_object *)iface)->iid;
    *count = 1;
    *iids = list;
    return S_OK;
}

HRESULT WINAPI simple_object_GetRuntimeClassName( IInspectable *iface, HSTRING *name )
{
    const WCHAR *class_name = ((struct simple_object *)iface)->class_name;

    return WindowsCreateString( class_name, wcslen( class_name ), name );
}

HRESULT WINAPI simple_object_GetTrustLevel( IInspectable *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* SIZE is the size of the whole object, which starts with the simple_object; the memory is zeroed */
HRESULT simple_object_create( SIZE_T size, const void *vtbl, const GUID *iid, const WCHAR *class_name,
                              void (*destroy)( struct simple_object * ), struct simple_object **out )
{
    struct simple_object *impl;

    if (!(impl = calloc( 1, size ))) return E_OUTOFMEMORY;
    impl->lpVtbl = vtbl;
    impl->ref = 1;
    impl->iid = iid;
    impl->class_name = class_name;
    impl->destroy = destroy;
    *out = impl;
    return S_OK;
}
