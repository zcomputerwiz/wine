/* WinRT Windows.ApplicationModel.LimitedAccessFeatures Implementation
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

WINE_DEFAULT_DEBUG_CHANNEL(model);

/* A limited access feature is one the vendor unlocks for an application with a token. No token is ever accepted here:
 * the result says the feature is unavailable, which is what an application that does not own the feature gets. */
struct request_result
{
    ILimitedAccessFeatureRequestResult ILimitedAccessFeatureRequestResult_iface;
    LONG ref;
    HSTRING feature_id;
    LimitedAccessFeatureStatus status;
};

static inline struct request_result *impl_from_ILimitedAccessFeatureRequestResult( ILimitedAccessFeatureRequestResult *iface )
{
    return CONTAINING_RECORD( iface, struct request_result, ILimitedAccessFeatureRequestResult_iface );
}

static HRESULT WINAPI result_QueryInterface( ILimitedAccessFeatureRequestResult *iface, REFIID iid, void **out )
{
    struct request_result *impl = impl_from_ILimitedAccessFeatureRequestResult( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILimitedAccessFeatureRequestResult ))
    {
        *out = &impl->ILimitedAccessFeatureRequestResult_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI result_AddRef( ILimitedAccessFeatureRequestResult *iface )
{
    struct request_result *impl = impl_from_ILimitedAccessFeatureRequestResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI result_Release( ILimitedAccessFeatureRequestResult *iface )
{
    struct request_result *impl = impl_from_ILimitedAccessFeatureRequestResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        WindowsDeleteString( impl->feature_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI result_GetIids( ILimitedAccessFeatureRequestResult *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_ILimitedAccessFeatureRequestResult;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI result_GetRuntimeClassName( ILimitedAccessFeatureRequestResult *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.LimitedAccessFeatureRequestResult", 58, name );
}

static HRESULT WINAPI result_GetTrustLevel( ILimitedAccessFeatureRequestResult *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI result_get_FeatureId( ILimitedAccessFeatureRequestResult *iface, HSTRING *value )
{
    struct request_result *impl = impl_from_ILimitedAccessFeatureRequestResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl->feature_id, value );
}

static HRESULT WINAPI result_get_Status( ILimitedAccessFeatureRequestResult *iface, LimitedAccessFeatureStatus *value )
{
    struct request_result *impl = impl_from_ILimitedAccessFeatureRequestResult( iface );

    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI result_get_EstimatedRemovalDate( ILimitedAccessFeatureRequestResult *iface, IReference_DateTime **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = NULL;
    return S_OK;
}

static const struct ILimitedAccessFeatureRequestResultVtbl result_vtbl =
{
    result_QueryInterface,
    result_AddRef,
    result_Release,
    /* IInspectable methods */
    result_GetIids,
    result_GetRuntimeClassName,
    result_GetTrustLevel,
    /* ILimitedAccessFeatureRequestResult methods */
    result_get_FeatureId,
    result_get_Status,
    result_get_EstimatedRemovalDate,
};

struct statics
{
    IActivationFactory IActivationFactory_iface;
    ILimitedAccessFeaturesStatics ILimitedAccessFeaturesStatics_iface;
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
    else if (IsEqualGUID( iid, &IID_ILimitedAccessFeaturesStatics ))
        *out = &impl->ILimitedAccessFeaturesStatics_iface;
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
    *list = IID_ILimitedAccessFeaturesStatics;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.ApplicationModel.LimitedAccessFeatures", 46, name );
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

DEFINE_IINSPECTABLE( statics, ILimitedAccessFeaturesStatics, struct statics, IActivationFactory_iface );

static HRESULT WINAPI statics_TryUnlockFeature( ILimitedAccessFeaturesStatics *iface, HSTRING feature_id, HSTRING token,
                                                HSTRING attestation, ILimitedAccessFeatureRequestResult **result )
{
    struct request_result *impl;
    HRESULT hr;

    TRACE( "iface %p, feature_id %s, token %s, attestation %s, result %p.\n", iface, debugstr_hstring( feature_id ),
           debugstr_hstring( token ), debugstr_hstring( attestation ), result );

    *result = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ILimitedAccessFeatureRequestResult_iface.lpVtbl = &result_vtbl;
    impl->ref = 1;
    impl->status = LimitedAccessFeatureStatus_Unavailable;
    if (FAILED(hr = WindowsDuplicateString( feature_id, &impl->feature_id )))
    {
        free( impl );
        return hr;
    }
    *result = &impl->ILimitedAccessFeatureRequestResult_iface;
    return S_OK;
}

static const struct ILimitedAccessFeaturesStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* ILimitedAccessFeaturesStatics methods */
    statics_TryUnlockFeature,
};

static struct statics statics_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *limited_access_features_factory = &statics_impl.IActivationFactory_iface;
