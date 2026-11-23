/* WinRT Windows.Management.Deployment.StagePackageOptions Implementation
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
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

/* the options of IPackageManager9::StagePackageByUriAsync: a bag of values that the caller sets and the package manager reads;
 * the lists are live vectors the caller appends to */
struct stage_options
{
    IStagePackageOptions IStagePackageOptions_iface;
    LONG ref;

    IVector_IInspectable *dependency_uris;
    IVector_IInspectable *optional_uris;
    IVector_IInspectable *related_uris;
    IVector_HSTRING *optional_family_names;
    IPackageVolume *volume;
    IUriRuntimeClass *external_location;
    StubPackageOption stub_option;
    boolean developer_mode, force_update, install_all_resources, required_content_only, stage_in_place, allow_unsigned;
};

static inline struct stage_options *impl_from_IStagePackageOptions( IStagePackageOptions *iface )
{
    return CONTAINING_RECORD( iface, struct stage_options, IStagePackageOptions_iface );
}

static HRESULT WINAPI options_QueryInterface( IStagePackageOptions *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStagePackageOptions ))
    {
        *out = iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI options_AddRef( IStagePackageOptions *iface )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI options_Release( IStagePackageOptions *iface )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->dependency_uris) IVector_IInspectable_Release( impl->dependency_uris );
        if (impl->optional_uris) IVector_IInspectable_Release( impl->optional_uris );
        if (impl->related_uris) IVector_IInspectable_Release( impl->related_uris );
        if (impl->optional_family_names) IVector_HSTRING_Release( impl->optional_family_names );
        if (impl->volume) IPackageVolume_Release( impl->volume );
        if (impl->external_location) IUriRuntimeClass_Release( impl->external_location );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI options_GetIids( IStagePackageOptions *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI options_GetRuntimeClassName( IStagePackageOptions *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Management_Deployment_StagePackageOptions,
                                wcslen( RuntimeClass_Windows_Management_Deployment_StagePackageOptions ), class_name );
}

static HRESULT WINAPI options_GetTrustLevel( IStagePackageOptions *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* a list the caller can append to: made on first use, the same object every time */
static HRESULT get_uri_list( IVector_IInspectable **list, IVector_Uri **out )
{
    static const struct vector_iids iids =
    {
        &IID_IIterable_Uri, &IID_IIterator_Uri, &IID_IVector_Uri, &IID_IVectorView_Uri,
    };
    HRESULT hr;

    if (!*list && FAILED(hr = vector_inspectable_create( &iids, list ))) return hr;
    return IVector_IInspectable_QueryInterface( *list, &IID_IVector_Uri, (void **)out );
}

static HRESULT WINAPI options_get_DependencyPackageUris( IStagePackageOptions *iface, IVector_Uri **value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_uri_list( &impl->dependency_uris, value );
}

static HRESULT WINAPI options_get_OptionalPackageUris( IStagePackageOptions *iface, IVector_Uri **value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_uri_list( &impl->optional_uris, value );
}

static HRESULT WINAPI options_get_RelatedPackageUris( IStagePackageOptions *iface, IVector_Uri **value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return get_uri_list( &impl->related_uris, value );
}

static HRESULT WINAPI options_get_OptionalPackageFamilyNames( IStagePackageOptions *iface, IVector_HSTRING **value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!impl->optional_family_names && FAILED(hr = vector_hstring_create( &impl->optional_family_names ))) return hr;
    IVector_HSTRING_AddRef( (*value = impl->optional_family_names) );
    return S_OK;
}

static HRESULT WINAPI options_get_TargetVolume( IStagePackageOptions *iface, IPackageVolume **value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if ((*value = impl->volume)) IPackageVolume_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI options_put_TargetVolume( IStagePackageOptions *iface, IPackageVolume *value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (value) IPackageVolume_AddRef( value );
    if (impl->volume) IPackageVolume_Release( impl->volume );
    impl->volume = value;
    return S_OK;
}

static HRESULT WINAPI options_get_ExternalLocationUri( IStagePackageOptions *iface, IUriRuntimeClass **value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if ((*value = impl->external_location)) IUriRuntimeClass_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI options_put_ExternalLocationUri( IStagePackageOptions *iface, IUriRuntimeClass *value )
{
    struct stage_options *impl = impl_from_IStagePackageOptions( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (value) IUriRuntimeClass_AddRef( value );
    if (impl->external_location) IUriRuntimeClass_Release( impl->external_location );
    impl->external_location = value;
    return S_OK;
}

static HRESULT WINAPI options_get_StubPackageOption( IStagePackageOptions *iface, StubPackageOption *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl_from_IStagePackageOptions( iface )->stub_option;
    return S_OK;
}

static HRESULT WINAPI options_put_StubPackageOption( IStagePackageOptions *iface, StubPackageOption value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    impl_from_IStagePackageOptions( iface )->stub_option = value;
    return S_OK;
}

#define BOOL_PROPERTY( name, field ) \
    static HRESULT WINAPI options_get_##name( IStagePackageOptions *iface, boolean *value ) \
    { \
        TRACE( "iface %p, value %p.\n", iface, value ); \
        *value = impl_from_IStagePackageOptions( iface )->field; \
        return S_OK; \
    } \
    static HRESULT WINAPI options_put_##name( IStagePackageOptions *iface, boolean value ) \
    { \
        TRACE( "iface %p, value %d.\n", iface, value ); \
        impl_from_IStagePackageOptions( iface )->field = !!value; \
        return S_OK; \
    }
BOOL_PROPERTY( DeveloperMode, developer_mode )
BOOL_PROPERTY( ForceUpdateFromAnyVersion, force_update )
BOOL_PROPERTY( InstallAllResources, install_all_resources )
BOOL_PROPERTY( RequiredContentGroupOnly, required_content_only )
BOOL_PROPERTY( StageInPlace, stage_in_place )
BOOL_PROPERTY( AllowUnsigned, allow_unsigned )

static const IStagePackageOptionsVtbl options_vtbl =
{
    options_QueryInterface,
    options_AddRef,
    options_Release,
    /* IInspectable methods */
    options_GetIids,
    options_GetRuntimeClassName,
    options_GetTrustLevel,
    /* IStagePackageOptions methods */
    options_get_DependencyPackageUris,
    options_get_TargetVolume,
    options_put_TargetVolume,
    options_get_OptionalPackageFamilyNames,
    options_get_OptionalPackageUris,
    options_get_RelatedPackageUris,
    options_get_ExternalLocationUri,
    options_put_ExternalLocationUri,
    options_get_StubPackageOption,
    options_put_StubPackageOption,
    options_get_DeveloperMode,
    options_put_DeveloperMode,
    options_get_ForceUpdateFromAnyVersion,
    options_put_ForceUpdateFromAnyVersion,
    options_get_InstallAllResources,
    options_put_InstallAllResources,
    options_get_RequiredContentGroupOnly,
    options_put_RequiredContentGroupOnly,
    options_get_StageInPlace,
    options_put_StageInPlace,
    options_get_AllowUnsigned,
    options_put_AllowUnsigned,
};

struct options_factory
{
    IActivationFactory IActivationFactory_iface;
};

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
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
    struct stage_options *impl;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        *instance = NULL;
        return E_OUTOFMEMORY;
    }
    impl->IStagePackageOptions_iface.lpVtbl = &options_vtbl;
    impl->ref = 1;

    *instance = (IInspectable *)&impl->IStagePackageOptions_iface;
    return S_OK;
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

static struct options_factory options_factory = { {&factory_vtbl} };

IActivationFactory *stage_package_options_factory = &options_factory.IActivationFactory_iface;
