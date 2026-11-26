/* WinRT Windows.Management.Policies.NamedPolicy implementation
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

/*
 * The policies of the policy configuration service provider that applications read, unmanaged (no device management
 * on Wine), with the values Windows 11 gives them then: a user policy read for a user has its default value; read
 * without a user, it has no value (E_UNEXPECTED). Policies not in the table are unknown (E_UNEXPECTED).
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct policy
{
    const WCHAR *area;
    const WCHAR *name;
    NamedPolicyKind kind;
    BOOL user_scope;
    INT32 default_value;
};

static const struct policy policies[] =
{
    {L"Education", L"AllowGraphingCalculator", NamedPolicyKind_Int32, TRUE, 1},
};

struct policy_data
{
    INamedPolicyData INamedPolicyData_iface;
    LONG ref;
    const struct policy *policy;
    IUser *user;
};

static inline struct policy_data *impl_from_INamedPolicyData( INamedPolicyData *iface )
{
    return CONTAINING_RECORD( iface, struct policy_data, INamedPolicyData_iface );
}

static HRESULT WINAPI data_QueryInterface( INamedPolicyData *iface, REFIID iid, void **out )
{
    struct policy_data *impl = impl_from_INamedPolicyData( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_INamedPolicyData ))
    {
        INamedPolicyData_AddRef( (*out = &impl->INamedPolicyData_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_AddRef( INamedPolicyData *iface )
{
    return InterlockedIncrement( &impl_from_INamedPolicyData( iface )->ref );
}

static ULONG WINAPI data_Release( INamedPolicyData *iface )
{
    struct policy_data *impl = impl_from_INamedPolicyData( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        if (impl->user) IUser_Release( impl->user );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI data_GetIids( INamedPolicyData *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_GetRuntimeClassName( INamedPolicyData *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Management.Policies.NamedPolicyData";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI data_GetTrustLevel( INamedPolicyData *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI data_get_Area( INamedPolicyData *iface, HSTRING *value )
{
    const WCHAR *area = impl_from_INamedPolicyData( iface )->policy->area;
    return WindowsCreateString( area, wcslen( area ), value );
}

static HRESULT WINAPI data_get_Name( INamedPolicyData *iface, HSTRING *value )
{
    const WCHAR *name = impl_from_INamedPolicyData( iface )->policy->name;
    return WindowsCreateString( name, wcslen( name ), value );
}

static HRESULT WINAPI data_get_Kind( INamedPolicyData *iface, NamedPolicyKind *value )
{
    *value = impl_from_INamedPolicyData( iface )->policy->kind;
    return S_OK;
}

static HRESULT policy_value( struct policy_data *impl, INT32 *value );

/* never managed; a policy without a value says so and fails, as on Windows */
static HRESULT WINAPI data_get_IsManaged( INamedPolicyData *iface, boolean *value )
{
    INT32 unused;

    *value = FALSE;
    return policy_value( impl_from_INamedPolicyData( iface ), &unused );
}

static HRESULT WINAPI data_get_IsUserPolicy( INamedPolicyData *iface, boolean *value )
{
    *value = impl_from_INamedPolicyData( iface )->user != NULL;
    return S_OK;
}

static HRESULT WINAPI data_get_User( INamedPolicyData *iface, IUser **value )
{
    struct policy_data *impl = impl_from_INamedPolicyData( iface );
    if ((*value = impl->user)) IUser_AddRef( impl->user );
    return S_OK;
}

/* the value: a user policy has one only when read for a user */
static HRESULT policy_value( struct policy_data *impl, INT32 *value )
{
    if (impl->policy->user_scope && !impl->user) return E_UNEXPECTED;
    *value = impl->policy->default_value;
    return S_OK;
}

static HRESULT WINAPI data_GetBoolean( INamedPolicyData *iface, boolean *result )
{
    INT32 value;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );
    if (SUCCEEDED(hr = policy_value( impl_from_INamedPolicyData( iface ), &value ))) *result = !!value;
    return hr;
}

static HRESULT WINAPI data_GetBinary( INamedPolicyData *iface, IBuffer **result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_GetInt32( INamedPolicyData *iface, INT32 *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return policy_value( impl_from_INamedPolicyData( iface ), result );
}

static HRESULT WINAPI data_GetInt64( INamedPolicyData *iface, INT64 *result )
{
    INT32 value;
    HRESULT hr;

    TRACE( "iface %p, result %p.\n", iface, result );
    if (SUCCEEDED(hr = policy_value( impl_from_INamedPolicyData( iface ), &value ))) *result = value;
    return hr;
}

static HRESULT WINAPI data_GetString( INamedPolicyData *iface, HSTRING *result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    return E_NOTIMPL;
}

/* the policies do not change on Wine: the handler is kept nowhere and never called */
static HRESULT WINAPI data_add_Changed( INamedPolicyData *iface, ITypedEventHandler_NamedPolicyData_IInspectable *handler,
                                       EventRegistrationToken *cookie )
{
    TRACE( "iface %p, handler %p, cookie %p.\n", iface, handler, cookie );
    if (!handler) return E_INVALIDARG;
    cookie->value = 1;
    return S_OK;
}

static HRESULT WINAPI data_remove_Changed( INamedPolicyData *iface, EventRegistrationToken cookie )
{
    TRACE( "iface %p, cookie %#I64x.\n", iface, cookie.value );
    return S_OK;
}

static const INamedPolicyDataVtbl data_vtbl =
{
    data_QueryInterface,
    data_AddRef,
    data_Release,
    /* IInspectable methods */
    data_GetIids,
    data_GetRuntimeClassName,
    data_GetTrustLevel,
    /* INamedPolicyData methods */
    data_get_Area,
    data_get_Name,
    data_get_Kind,
    data_get_IsManaged,
    data_get_IsUserPolicy,
    data_get_User,
    data_GetBoolean,
    data_GetBinary,
    data_GetInt32,
    data_GetInt64,
    data_GetString,
    data_add_Changed,
    data_remove_Changed,
};

static HRESULT policy_data_create( HSTRING area, HSTRING name, IUser *user, INamedPolicyData **result )
{
    const WCHAR *area_str = WindowsGetStringRawBuffer( area, NULL ), *name_str = WindowsGetStringRawBuffer( name, NULL );
    const struct policy *policy = NULL;
    struct policy_data *impl;
    unsigned int i;

    if (!result) return E_POINTER;
    *result = NULL;
    for (i = 0; i < ARRAY_SIZE(policies); i++)
        if (!wcsicmp( policies[i].area, area_str ) && !wcsicmp( policies[i].name, name_str )) policy = &policies[i];
    if (!policy)
    {
        FIXME( "unknown policy %s/%s.\n", debugstr_w( area_str ), debugstr_w( name_str ) );
        return E_UNEXPECTED;
    }
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->INamedPolicyData_iface.lpVtbl = &data_vtbl;
    impl->ref = 1;
    impl->policy = policy;
    /* a user policy read for a user belongs to that user */
    if (policy->user_scope && (impl->user = user)) IUser_AddRef( user );
    *result = &impl->INamedPolicyData_iface;
    return S_OK;
}

/* ---- the statics ---- */

struct named_policy
{
    IActivationFactory IActivationFactory_iface;
    INamedPolicyStatics INamedPolicyStatics_iface;
    LONG ref;
};

static inline struct named_policy *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct named_policy, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct named_policy *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_INamedPolicyStatics ))
        *out = &impl->INamedPolicyStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
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
    TRACE( "iface %p, instance %p.\n", iface, instance );
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

DEFINE_IINSPECTABLE( statics, INamedPolicyStatics, struct named_policy, IActivationFactory_iface )

static HRESULT WINAPI statics_GetPolicyFromPath( INamedPolicyStatics *iface, HSTRING area, HSTRING name,
                                                 INamedPolicyData **result )
{
    TRACE( "iface %p, area %s, name %s, result %p.\n", iface, debugstr_hstring( area ), debugstr_hstring( name ), result );
    return policy_data_create( area, name, NULL, result );
}

static HRESULT WINAPI statics_GetPolicyFromPathForUser( INamedPolicyStatics *iface, IUser *user, HSTRING area,
                                                        HSTRING name, INamedPolicyData **result )
{
    TRACE( "iface %p, user %p, area %s, name %s, result %p.\n", iface, user, debugstr_hstring( area ),
           debugstr_hstring( name ), result );
    if (!user) return E_INVALIDARG;
    return policy_data_create( area, name, user, result );
}

static const INamedPolicyStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* INamedPolicyStatics methods */
    statics_GetPolicyFromPath,
    statics_GetPolicyFromPathForUser,
};

static struct named_policy named_policy =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *named_policy_factory = &named_policy.IActivationFactory_iface;
