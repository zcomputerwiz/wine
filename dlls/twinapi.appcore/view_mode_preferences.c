/* WinRT Windows.UI.ViewManagement.ViewModePreferences Implementation
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

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/* The preferences an application gives TryEnterViewModeAsync: the size it would like (default, or a custom one). */
struct view_mode_preferences
{
    IViewModePreferences IViewModePreferences_iface;
    LONG ref;
    ViewSizePreference preference;
    Size custom_size;
};

static inline struct view_mode_preferences *impl_from_IViewModePreferences( IViewModePreferences *iface )
{
    return CONTAINING_RECORD( iface, struct view_mode_preferences, IViewModePreferences_iface );
}

static HRESULT WINAPI preferences_QueryInterface( IViewModePreferences *iface, REFIID iid, void **out )
{
    struct view_mode_preferences *impl = impl_from_IViewModePreferences( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IViewModePreferences ))
    {
        *out = &impl->IViewModePreferences_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI preferences_AddRef( IViewModePreferences *iface )
{
    struct view_mode_preferences *impl = impl_from_IViewModePreferences( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI preferences_Release( IViewModePreferences *iface )
{
    struct view_mode_preferences *impl = impl_from_IViewModePreferences( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI preferences_GetIids( IViewModePreferences *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IViewModePreferences;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI preferences_GetRuntimeClassName( IViewModePreferences *iface, HSTRING *name )
{
    return create_name( L"Windows.UI.ViewManagement.ViewModePreferences", name );
}

static HRESULT WINAPI preferences_GetTrustLevel( IViewModePreferences *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI preferences_get_ViewSizePreference( IViewModePreferences *iface, ViewSizePreference *value )
{
    struct view_mode_preferences *impl = impl_from_IViewModePreferences( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->preference;
    return S_OK;
}

static HRESULT WINAPI preferences_put_ViewSizePreference( IViewModePreferences *iface, ViewSizePreference value )
{
    struct view_mode_preferences *impl = impl_from_IViewModePreferences( iface );

    TRACE( "iface %p, value %d.\n", iface, value );
    impl->preference = value;
    return S_OK;
}

static HRESULT WINAPI preferences_get_CustomSize( IViewModePreferences *iface, Size *value )
{
    struct view_mode_preferences *impl = impl_from_IViewModePreferences( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->custom_size;
    return S_OK;
}

static HRESULT WINAPI preferences_put_CustomSize( IViewModePreferences *iface, Size value )
{
    struct view_mode_preferences *impl = impl_from_IViewModePreferences( iface );

    TRACE( "iface %p, value %fx%f.\n", iface, value.Width, value.Height );
    impl->custom_size = value;
    return S_OK;
}

static const struct IViewModePreferencesVtbl preferences_vtbl =
{
    preferences_QueryInterface,
    preferences_AddRef,
    preferences_Release,
    /* IInspectable methods */
    preferences_GetIids,
    preferences_GetRuntimeClassName,
    preferences_GetTrustLevel,
    /* IViewModePreferences methods */
    preferences_get_ViewSizePreference,
    preferences_put_ViewSizePreference,
    preferences_get_CustomSize,
    preferences_put_CustomSize,
};

struct statics
{
    IActivationFactory IActivationFactory_iface;
    IViewModePreferencesStatics IViewModePreferencesStatics_iface;
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

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IViewModePreferencesStatics ))
        *out = &impl->IViewModePreferencesStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
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
    *list = IID_IViewModePreferencesStatics;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return create_name( L"Windows.UI.ViewManagement.ViewModePreferences", name );
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

static inline struct statics *impl_from_IViewModePreferencesStatics( IViewModePreferencesStatics *iface )
{
    return CONTAINING_RECORD( iface, struct statics, IViewModePreferencesStatics_iface );
}

static HRESULT WINAPI statics_QueryInterface( IViewModePreferencesStatics *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &impl_from_IViewModePreferencesStatics( iface )->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI statics_AddRef( IViewModePreferencesStatics *iface )
{
    return factory_AddRef( &impl_from_IViewModePreferencesStatics( iface )->IActivationFactory_iface );
}

static ULONG WINAPI statics_Release( IViewModePreferencesStatics *iface )
{
    return factory_Release( &impl_from_IViewModePreferencesStatics( iface )->IActivationFactory_iface );
}

static HRESULT WINAPI statics_GetIids( IViewModePreferencesStatics *iface, ULONG *count, IID **iids )
{
    return factory_GetIids( &impl_from_IViewModePreferencesStatics( iface )->IActivationFactory_iface, count, iids );
}

static HRESULT WINAPI statics_GetRuntimeClassName( IViewModePreferencesStatics *iface, HSTRING *name )
{
    return factory_GetRuntimeClassName( &impl_from_IViewModePreferencesStatics( iface )->IActivationFactory_iface, name );
}

static HRESULT WINAPI statics_GetTrustLevel( IViewModePreferencesStatics *iface, TrustLevel *level )
{
    return factory_GetTrustLevel( &impl_from_IViewModePreferencesStatics( iface )->IActivationFactory_iface, level );
}

static HRESULT WINAPI statics_CreateDefault( IViewModePreferencesStatics *iface, ApplicationViewMode mode,
                                             IViewModePreferences **result )
{
    struct view_mode_preferences *impl;

    TRACE( "iface %p, mode %d, result %p.\n", iface, mode, result );

    *result = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IViewModePreferences_iface.lpVtbl = &preferences_vtbl;
    impl->ref = 1;
    /* the default size of the mode; the always-on-top (compact overlay) window is the one that has a custom size */
    impl->preference = ViewSizePreference_Default;
    *result = &impl->IViewModePreferences_iface;
    return S_OK;
}

static const struct IViewModePreferencesStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IViewModePreferencesStatics methods */
    statics_CreateDefault,
};

static struct statics statics_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *view_mode_preferences_factory = &statics_impl.IActivationFactory_iface;
