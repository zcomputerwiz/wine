/*
 * The input-site classes of inputhost.dll
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

/* Windows.UI.Internal.Input.InputSite and ...ActivationConfigurationInputObject: the classes through which the input
 * component of the Windows App SDK (the application's own copy, where the system has none) attaches its input windows to
 * the system's input delivery. What is here was measured on Windows 10 (19045) and Windows 11 (26300) at the interface
 * boundary: calls with our own objects and their results, no more.
 *
 *   - Windows 10 serves the site factory as {8ca0442d-...} and refuses {73f80911-...}; Windows 11 the reverse. The
 *     application's own copy of the component asks for the former, so that is the one served here.
 *   - Its one method takes the caller's host object and returns a site. The host is asked for the interface
 *     {92179cc0-...}, and slot 9 of that interface for a handle: the composition input sink of the caller's input window
 *     (NtCreateCompositionInputSink). Windows does not check the handle (an event, or NULL, gives a site as well); a host
 *     that refuses the interface or fails the call ends the process there, here the creation fails.
 *   - The configuration object is created from a site and has two 32-bit settings that start as 0 and read back what
 *     was written.
 *   - A site lists four more interfaces on Windows ({e6414e4b-...}, {2cc67fbf-...}, {46cae093-...} and IClosable), the
 *     configuration object more still. Their methods are not known; only IClosable is answered.
 * The site does not deliver input yet.
 * 32-bit: the calls were measured with 64-bit callers; the argument counts are the same ones here.
 */

#include <stdarg.h>
#include <stddef.h>

#define COBJMACROS
#include "initguid.h"
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "objbase.h"

#include "activation.h"

#define WIDL_using_Windows_Foundation
#include "windows.foundation.h"
#define WIDL_using_Windows_UI_Internal_Input
#include "classes.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(inputhost);

/* asked of the host; slot 9 gives the composition input sink of the caller's input window */
DEFINE_GUID( IID_input_site_host, 0x92179cc0, 0xae1f, 0x48bc, 0xa3, 0x54, 0x8f, 0x7a, 0x88, 0x00, 0xf2, 0xfb );
#define HOST_SLOT_GET_SINK 9

static HRESULT host_get_sink( IUnknown *host, HANDLE *sink )
{
    void **vtbl = *(void ***)host;
    return ((HRESULT (STDMETHODCALLTYPE *)( IUnknown *, HANDLE * ))vtbl[HOST_SLOT_GET_SINK])( host, sink );
}

static HRESULT copy_iids( const IID *const *list, ULONG count, ULONG *iid_count, IID **iids )
{
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    if (!(*iids = CoTaskMemAlloc( count * sizeof(**iids) ))) return E_OUTOFMEMORY;
    for (i = 0; i < count; i++) (*iids)[i] = *list[i];
    *iid_count = count;
    return S_OK;
}

/* the IInspectable part of a second interface of an object */
#define DEFINE_FORWARDED_IINSPECTABLE( pfx, iface_type, impl_type, iface_mem, base_iface )                                \
    static inline impl_type *pfx##_impl( iface_type *iface )                                                                \
    {                                                                                                                       \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                                            \
    }                                                                                                                       \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )                                \
    {                                                                                                                       \
        return IInspectable_QueryInterface( (IInspectable *)&pfx##_impl( iface )->base_iface, iid, out );      \
    }                                                                                                                       \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                                                  \
    {                                                                                                                       \
        return IInspectable_AddRef( (IInspectable *)&pfx##_impl( iface )->base_iface );                        \
    }                                                                                                                       \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                                                 \
    {                                                                                                                       \
        return IInspectable_Release( (IInspectable *)&pfx##_impl( iface )->base_iface );                       \
    }                                                                                                                       \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )                                 \
    {                                                                                                                       \
        return IInspectable_GetIids( (IInspectable *)&pfx##_impl( iface )->base_iface, iid_count, iids );      \
    }                                                                                                                       \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )                              \
    {                                                                                                                       \
        return IInspectable_GetRuntimeClassName( (IInspectable *)&pfx##_impl( iface )->base_iface, class_name ); \
    }                                                                                                                       \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )                                \
    {                                                                                                                       \
        return IInspectable_GetTrustLevel( (IInspectable *)&pfx##_impl( iface )->base_iface, trust_level );    \
    }

/*
 * The site
 */

struct input_site
{
    IInputSite IInputSite_iface;
    IClosable IClosable_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    IUnknown *host;   /* the host's {92179cc0} interface; NULL once closed */
    HANDLE sink;      /* the caller's handle, not owned */
};

static inline struct input_site *impl_from_IInputSite( IInputSite *iface )
{
    return CONTAINING_RECORD( iface, struct input_site, IInputSite_iface );
}

static void input_site_drop_host( struct input_site *impl )
{
    IUnknown *host;

    EnterCriticalSection( &impl->cs );
    host = impl->host;
    impl->host = NULL;
    impl->sink = NULL;
    LeaveCriticalSection( &impl->cs );
    if (host) IUnknown_Release( host );
}

static HRESULT WINAPI input_site_QueryInterface( IInputSite *iface, REFIID iid, void **out )
{
    struct input_site *impl = impl_from_IInputSite( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IInputSite ))
    {
        *out = &impl->IInputSite_iface;
        IInputSite_AddRef( &impl->IInputSite_iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        *out = &impl->IClosable_iface;
        IInputSite_AddRef( &impl->IInputSite_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI input_site_AddRef( IInputSite *iface )
{
    struct input_site *impl = impl_from_IInputSite( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI input_site_Release( IInputSite *iface )
{
    struct input_site *impl = impl_from_IInputSite( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );

    if (!ref)
    {
        input_site_drop_host( impl );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI input_site_GetIids( IInputSite *iface, ULONG *iid_count, IID **iids )
{
    static const IID *const list[] = { &IID_IInputSite, &IID_IClosable };

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    return copy_iids( list, ARRAY_SIZE(list), iid_count, iids );
}

static HRESULT WINAPI input_site_GetRuntimeClassName( IInputSite *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_UI_Internal_Input_InputSite,
                                wcslen( RuntimeClass_Windows_UI_Internal_Input_InputSite ), class_name );
}

static HRESULT WINAPI input_site_GetTrustLevel( IInputSite *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static const struct IInputSiteVtbl input_site_vtbl =
{
    input_site_QueryInterface,
    input_site_AddRef,
    input_site_Release,
    /* IInspectable methods */
    input_site_GetIids,
    input_site_GetRuntimeClassName,
    input_site_GetTrustLevel,
};

DEFINE_FORWARDED_IINSPECTABLE( input_site_closable, IClosable, struct input_site, IClosable_iface, IInputSite_iface )

static HRESULT WINAPI input_site_closable_Close( IClosable *iface )
{
    struct input_site *impl = input_site_closable_impl( iface );

    TRACE( "iface %p.\n", iface );

    input_site_drop_host( impl );
    return S_OK;
}

static const struct IClosableVtbl input_site_closable_vtbl =
{
    input_site_closable_QueryInterface,
    input_site_closable_AddRef,
    input_site_closable_Release,
    /* IInspectable methods */
    input_site_closable_GetIids,
    input_site_closable_GetRuntimeClassName,
    input_site_closable_GetTrustLevel,
    /* IClosable methods */
    input_site_closable_Close,
};

/*
 * The configuration object
 */

struct configuration
{
    IActivationConfigurationInputObject IActivationConfigurationInputObject_iface;
    IClosable IClosable_iface;
    LONG ref;
    IInputSite *site;
    LONG setting1;
    LONG setting2;
};

static inline struct configuration *impl_from_IActivationConfigurationInputObject( IActivationConfigurationInputObject *iface )
{
    return CONTAINING_RECORD( iface, struct configuration, IActivationConfigurationInputObject_iface );
}

static HRESULT WINAPI configuration_QueryInterface( IActivationConfigurationInputObject *iface, REFIID iid, void **out )
{
    struct configuration *impl = impl_from_IActivationConfigurationInputObject( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationConfigurationInputObject ))
    {
        *out = &impl->IActivationConfigurationInputObject_iface;
        IActivationConfigurationInputObject_AddRef( &impl->IActivationConfigurationInputObject_iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        *out = &impl->IClosable_iface;
        IActivationConfigurationInputObject_AddRef( &impl->IActivationConfigurationInputObject_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI configuration_AddRef( IActivationConfigurationInputObject *iface )
{
    struct configuration *impl = impl_from_IActivationConfigurationInputObject( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI configuration_Release( IActivationConfigurationInputObject *iface )
{
    struct configuration *impl = impl_from_IActivationConfigurationInputObject( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );

    if (!ref)
    {
        IInputSite *site = InterlockedExchangePointer( (void **)&impl->site, NULL );
        if (site) IInputSite_Release( site );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI configuration_GetIids( IActivationConfigurationInputObject *iface, ULONG *iid_count, IID **iids )
{
    static const IID *const list[] = { &IID_IActivationConfigurationInputObject, &IID_IClosable };

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    return copy_iids( list, ARRAY_SIZE(list), iid_count, iids );
}

static HRESULT WINAPI configuration_GetRuntimeClassName( IActivationConfigurationInputObject *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    if (!class_name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_UI_Internal_Input_ActivationConfigurationInputObject,
                                wcslen( RuntimeClass_Windows_UI_Internal_Input_ActivationConfigurationInputObject ), class_name );
}

static HRESULT WINAPI configuration_GetTrustLevel( IActivationConfigurationInputObject *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI configuration_get_Setting1( IActivationConfigurationInputObject *iface, INT32 *value )
{
    struct configuration *impl = impl_from_IActivationConfigurationInputObject( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = ReadNoFence( &impl->setting1 );
    return S_OK;
}

static HRESULT WINAPI configuration_put_Setting1( IActivationConfigurationInputObject *iface, INT32 value )
{
    struct configuration *impl = impl_from_IActivationConfigurationInputObject( iface );

    FIXME( "iface %p, value %d: stored only.\n", iface, value );

    InterlockedExchange( &impl->setting1, value );
    return S_OK;
}

static HRESULT WINAPI configuration_get_Setting2( IActivationConfigurationInputObject *iface, INT32 *value )
{
    struct configuration *impl = impl_from_IActivationConfigurationInputObject( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = ReadNoFence( &impl->setting2 );
    return S_OK;
}

static HRESULT WINAPI configuration_put_Setting2( IActivationConfigurationInputObject *iface, INT32 value )
{
    struct configuration *impl = impl_from_IActivationConfigurationInputObject( iface );

    FIXME( "iface %p, value %d: stored only.\n", iface, value );

    InterlockedExchange( &impl->setting2, value );
    return S_OK;
}

static const struct IActivationConfigurationInputObjectVtbl configuration_vtbl =
{
    configuration_QueryInterface,
    configuration_AddRef,
    configuration_Release,
    /* IInspectable methods */
    configuration_GetIids,
    configuration_GetRuntimeClassName,
    configuration_GetTrustLevel,
    /* IActivationConfigurationInputObject methods */
    configuration_get_Setting1,
    configuration_put_Setting1,
    configuration_get_Setting2,
    configuration_put_Setting2,
};

DEFINE_FORWARDED_IINSPECTABLE( configuration_closable, IClosable, struct configuration, IClosable_iface,
                               IActivationConfigurationInputObject_iface )

static HRESULT WINAPI configuration_closable_Close( IClosable *iface )
{
    struct configuration *impl = configuration_closable_impl( iface );
    IInputSite *site;

    TRACE( "iface %p.\n", iface );

    if ((site = InterlockedExchangePointer( (void **)&impl->site, NULL ))) IInputSite_Release( site );
    return S_OK;
}

static const struct IClosableVtbl configuration_closable_vtbl =
{
    configuration_closable_QueryInterface,
    configuration_closable_AddRef,
    configuration_closable_Release,
    /* IInspectable methods */
    configuration_closable_GetIids,
    configuration_closable_GetRuntimeClassName,
    configuration_closable_GetTrustLevel,
    /* IClosable methods */
    configuration_closable_Close,
};

/*
 * The factories. Both answer IActivationFactory and one creation interface; a class cannot be activated without arguments.
 */

struct factory
{
    IActivationFactory IActivationFactory_iface;
    IInspectable statics_iface;   /* IInputSiteStatics or IActivationConfigurationInputObjectStatics */
    const IID *statics_iid;
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

    if (!out) return E_POINTER;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IActivationFactory_AddRef( &impl->IActivationFactory_iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, impl->statics_iid ))
    {
        *out = &impl->statics_iface;
        IActivationFactory_AddRef( &impl->IActivationFactory_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing ref to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing ref to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    struct factory *impl = impl_from_IActivationFactory( iface );
    const IID *list[] = { &IID_IActivationFactory, impl->statics_iid };

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    return copy_iids( list, ARRAY_SIZE(list), iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );

    /* a factory has no class name on Windows */
    if (class_name) *class_name = NULL;
    return E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (instance) *instance = NULL;
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

static inline struct factory *impl_from_statics( void *iface )
{
    return CONTAINING_RECORD( (IInspectable *)iface, struct factory, statics_iface );
}

static HRESULT WINAPI statics_QueryInterface( IInspectable *iface, REFIID iid, void **out )
{
    return IActivationFactory_QueryInterface( &impl_from_statics( iface )->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI statics_AddRef( IInspectable *iface )
{
    return IActivationFactory_AddRef( &impl_from_statics( iface )->IActivationFactory_iface );
}

static ULONG WINAPI statics_Release( IInspectable *iface )
{
    return IActivationFactory_Release( &impl_from_statics( iface )->IActivationFactory_iface );
}

static HRESULT WINAPI statics_GetIids( IInspectable *iface, ULONG *iid_count, IID **iids )
{
    return IActivationFactory_GetIids( &impl_from_statics( iface )->IActivationFactory_iface, iid_count, iids );
}

static HRESULT WINAPI statics_GetRuntimeClassName( IInspectable *iface, HSTRING *class_name )
{
    return IActivationFactory_GetRuntimeClassName( &impl_from_statics( iface )->IActivationFactory_iface, class_name );
}

static HRESULT WINAPI statics_GetTrustLevel( IInspectable *iface, TrustLevel *trust_level )
{
    return IActivationFactory_GetTrustLevel( &impl_from_statics( iface )->IActivationFactory_iface, trust_level );
}

static HRESULT WINAPI input_site_statics_CreateForHost( IInputSiteStatics *iface, IInspectable *host, IInputSite **site )
{
    struct input_site *impl;
    IUnknown *host_iface;
    HANDLE sink = NULL;
    HRESULT hr;

    TRACE( "iface %p, host %p, site %p.\n", iface, host, site );

    if (!site) return E_POINTER;
    *site = NULL;
    if (!host) return E_INVALIDARG;

    /* Windows ends the process when the host refuses or its call fails */
    if (FAILED(hr = IInspectable_QueryInterface( host, &IID_input_site_host, (void **)&host_iface )))
    {
        WARN( "The host does not have the interface that gives its input sink, hr %#lx.\n", hr );
        return hr;
    }
    if (FAILED(hr = host_get_sink( host_iface, &sink )))
    {
        WARN( "The host did not give its input sink, hr %#lx.\n", hr );
        IUnknown_Release( host_iface );
        return hr;
    }
    /* Windows then hands such a host a handle of its own (slot 10) and asks again; an application's host has a sink */
    if (!sink) FIXME( "The host has no input sink.\n" );

    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        IUnknown_Release( host_iface );
        return E_OUTOFMEMORY;
    }
    impl->IInputSite_iface.lpVtbl = &input_site_vtbl;
    impl->IClosable_iface.lpVtbl = &input_site_closable_vtbl;
    impl->ref = 1;
    impl->host = host_iface;
    impl->sink = sink;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": input_site.cs");

    FIXME( "site %p for the input sink %p: input is not delivered through it yet.\n", impl, sink );

    *site = &impl->IInputSite_iface;
    return S_OK;
}

static const struct IInputSiteStaticsVtbl input_site_statics_vtbl =
{
    (void *)statics_QueryInterface,
    (void *)statics_AddRef,
    (void *)statics_Release,
    /* IInspectable methods */
    (void *)statics_GetIids,
    (void *)statics_GetRuntimeClassName,
    (void *)statics_GetTrustLevel,
    /* IInputSiteStatics methods */
    input_site_statics_CreateForHost,
};

static HRESULT WINAPI configuration_statics_CreateForInputSite( IActivationConfigurationInputObjectStatics *iface, IInputSite *site,
                                                                IActivationConfigurationInputObject **object )
{
    struct configuration *impl;
    IInputSite *checked;
    HRESULT hr;

    TRACE( "iface %p, site %p, object %p.\n", iface, site, object );

    if (!object) return E_POINTER;
    *object = NULL;
    if (!site) return E_INVALIDARG;
    if (FAILED(hr = IInputSite_QueryInterface( site, &IID_IInputSite, (void **)&checked ))) return hr;

    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        IInputSite_Release( checked );
        return E_OUTOFMEMORY;
    }
    impl->IActivationConfigurationInputObject_iface.lpVtbl = &configuration_vtbl;
    impl->IClosable_iface.lpVtbl = &configuration_closable_vtbl;
    impl->ref = 1;
    impl->site = checked;

    *object = &impl->IActivationConfigurationInputObject_iface;
    return S_OK;
}

static const struct IActivationConfigurationInputObjectStaticsVtbl configuration_statics_vtbl =
{
    (void *)statics_QueryInterface,
    (void *)statics_AddRef,
    (void *)statics_Release,
    /* IInspectable methods */
    (void *)statics_GetIids,
    (void *)statics_GetRuntimeClassName,
    (void *)statics_GetTrustLevel,
    /* IActivationConfigurationInputObjectStatics methods */
    configuration_statics_CreateForInputSite,
};

static struct factory input_site_factory =
{
    {&factory_vtbl},
    {(const IInspectableVtbl *)&input_site_statics_vtbl},
    &IID_IInputSiteStatics,
    1,
};

static struct factory configuration_factory =
{
    {&factory_vtbl},
    {(const IInspectableVtbl *)&configuration_statics_vtbl},
    &IID_IActivationConfigurationInputObjectStatics,
    1,
};

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;

    if (!wcscmp( name, RuntimeClass_Windows_UI_Internal_Input_InputSite ))
        IActivationFactory_QueryInterface( &input_site_factory.IActivationFactory_iface, &IID_IActivationFactory, (void **)factory );
    else if (!wcscmp( name, RuntimeClass_Windows_UI_Internal_Input_ActivationConfigurationInputObject ))
        IActivationFactory_QueryInterface( &configuration_factory.IActivationFactory_iface, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}
