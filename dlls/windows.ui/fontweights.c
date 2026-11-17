/* WinRT Windows.UI.Text.FontWeights Implementation
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

struct statics
{
    IActivationFactory IActivationFactory_iface;
    IFontWeightsStatics IFontWeightsStatics_iface;
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
    else if (IsEqualGUID( iid, &IID_IFontWeightsStatics ))
        *out = &impl->IFontWeightsStatics_iface;
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
    list[0] = IID_IFontWeightsStatics;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.Text.FontWeights", 27, name );
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

DEFINE_IINSPECTABLE( statics, IFontWeightsStatics, struct statics, IActivationFactory_iface );

#define WEIGHT( name, number )                                                                       \
    static HRESULT WINAPI statics_get_##name( IFontWeightsStatics *iface, FontWeight *value )        \
    {                                                                                                \
        TRACE( "iface %p, value %p.\n", iface, value );                                              \
        value->Weight = number;                                                                      \
        return S_OK;                                                                                 \
    }
WEIGHT( Black, 900 )
WEIGHT( Bold, 700 )
WEIGHT( ExtraBlack, 950 )
WEIGHT( ExtraBold, 800 )
WEIGHT( ExtraLight, 200 )
WEIGHT( Light, 300 )
WEIGHT( Medium, 500 )
WEIGHT( Normal, 400 )
WEIGHT( SemiBold, 600 )
WEIGHT( SemiLight, 350 )
WEIGHT( Thin, 100 )
#undef WEIGHT

static const struct IFontWeightsStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IFontWeightsStatics methods */
    statics_get_Black,
    statics_get_Bold,
    statics_get_ExtraBlack,
    statics_get_ExtraBold,
    statics_get_ExtraLight,
    statics_get_Light,
    statics_get_Medium,
    statics_get_Normal,
    statics_get_SemiBold,
    statics_get_SemiLight,
    statics_get_Thin,
};

static struct statics statics_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *fontweights_factory = &statics_impl.IActivationFactory_iface;
