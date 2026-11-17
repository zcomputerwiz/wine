/* Windows.ApplicationModel.DataTransfer.StandardDataFormats
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

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct statics
{
    IActivationFactory IActivationFactory_iface;
    IStandardDataFormatsStatics IStandardDataFormatsStatics_iface;
    IStandardDataFormatsStatics2 IStandardDataFormatsStatics2_iface;
    IStandardDataFormatsStatics3 IStandardDataFormatsStatics3_iface;
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
    else if (IsEqualGUID( iid, &IID_IStandardDataFormatsStatics ))
        *out = &impl->IStandardDataFormatsStatics_iface;
    else if (IsEqualGUID( iid, &IID_IStandardDataFormatsStatics2 ))
        *out = &impl->IStandardDataFormatsStatics2_iface;
    else if (IsEqualGUID( iid, &IID_IStandardDataFormatsStatics3 ))
        *out = &impl->IStandardDataFormatsStatics3_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
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

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 3 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IStandardDataFormatsStatics;
    list[1] = IID_IStandardDataFormatsStatics2;
    list[2] = IID_IStandardDataFormatsStatics3;
    *count = 3;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.StandardDataFormats", name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
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

#define FORMAT_GETTER( pfx, itype, name, string ) \
    static HRESULT WINAPI pfx##_get_##name( itype *iface, HSTRING *value ) \
    { \
        TRACE( "iface %p, value %p.\n", iface, value ); \
        return create_name( string, value ); \
    }

DEFINE_IINSPECTABLE( statics1, IStandardDataFormatsStatics, struct statics, IActivationFactory_iface )

FORMAT_GETTER( statics1, IStandardDataFormatsStatics, Text, DATA_FORMAT_TEXT )
FORMAT_GETTER( statics1, IStandardDataFormatsStatics, Uri, DATA_FORMAT_URI )
FORMAT_GETTER( statics1, IStandardDataFormatsStatics, Html, DATA_FORMAT_HTML )
FORMAT_GETTER( statics1, IStandardDataFormatsStatics, Rtf, DATA_FORMAT_RTF )
FORMAT_GETTER( statics1, IStandardDataFormatsStatics, Bitmap, DATA_FORMAT_BITMAP )
FORMAT_GETTER( statics1, IStandardDataFormatsStatics, StorageItems, DATA_FORMAT_STORAGE )

static const IStandardDataFormatsStaticsVtbl statics1_vtbl =
{
    statics1_QueryInterface,
    statics1_AddRef,
    statics1_Release,
    /* IInspectable methods */
    statics1_GetIids,
    statics1_GetRuntimeClassName,
    statics1_GetTrustLevel,
    /* IStandardDataFormatsStatics methods */
    statics1_get_Text,
    statics1_get_Uri,
    statics1_get_Html,
    statics1_get_Rtf,
    statics1_get_Bitmap,
    statics1_get_StorageItems,
};

DEFINE_IINSPECTABLE( statics2, IStandardDataFormatsStatics2, struct statics, IActivationFactory_iface )

FORMAT_GETTER( statics2, IStandardDataFormatsStatics2, WebLink, L"WebLink" )
FORMAT_GETTER( statics2, IStandardDataFormatsStatics2, ApplicationLink, L"ApplicationLink" )

static const IStandardDataFormatsStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IStandardDataFormatsStatics2 methods */
    statics2_get_WebLink,
    statics2_get_ApplicationLink,
};

DEFINE_IINSPECTABLE( statics3, IStandardDataFormatsStatics3, struct statics, IActivationFactory_iface )

FORMAT_GETTER( statics3, IStandardDataFormatsStatics3, UserActivityJsonArray, L"UserActivityJsonArray" )

static const IStandardDataFormatsStatics3Vtbl statics3_vtbl =
{
    statics3_QueryInterface,
    statics3_AddRef,
    statics3_Release,
    /* IInspectable methods */
    statics3_GetIids,
    statics3_GetRuntimeClassName,
    statics3_GetTrustLevel,
    /* IStandardDataFormatsStatics3 methods */
    statics3_get_UserActivityJsonArray,
};

static struct statics standard_data_formats =
{
    {&factory_vtbl},
    {&statics1_vtbl},
    {&statics2_vtbl},
    {&statics3_vtbl},
    1
};

IActivationFactory *standard_data_formats_factory = &standard_data_formats.IActivationFactory_iface;
