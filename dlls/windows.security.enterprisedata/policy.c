/*
 * Windows.Security.EnterpriseData.ProtectionPolicyManager
 *
 * A system without enterprise data protection policies: no identity is managed, nothing is protected, every access is
 * allowed. The results are those of Windows 11 on a machine that has no policy (measured with the public statics).
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

WINE_DEFAULT_DEBUG_CHANNEL(enterprisedata);

struct policy_factory
{
    IActivationFactory IActivationFactory_iface;
    IProtectionPolicyManagerStatics IProtectionPolicyManagerStatics_iface;
    IProtectionPolicyManagerStatics2 IProtectionPolicyManagerStatics2_iface;
    IProtectionPolicyManagerStatics3 IProtectionPolicyManagerStatics3_iface;
    IProtectionPolicyManagerStatics4 IProtectionPolicyManagerStatics4_iface;
    BOOL partial_trust; /* the private flavour of the class: the first two statics only, partial trust */
    LONG ref;
};

static LONG event_token_counter;

static inline struct policy_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct policy_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct policy_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IProtectionPolicyManagerStatics ))
        *out = &impl->IProtectionPolicyManagerStatics_iface;
    else if (IsEqualGUID( iid, &IID_IProtectionPolicyManagerStatics2 ))
        *out = &impl->IProtectionPolicyManagerStatics2_iface;
    else if (!impl->partial_trust && IsEqualGUID( iid, &IID_IProtectionPolicyManagerStatics3 ))
        *out = &impl->IProtectionPolicyManagerStatics3_iface;
    else if (!impl->partial_trust && IsEqualGUID( iid, &IID_IProtectionPolicyManagerStatics4 ))
        *out = &impl->IProtectionPolicyManagerStatics4_iface;

    if (*out)
    {
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct policy_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct policy_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    struct policy_factory *impl = impl_from_IActivationFactory( iface );
    ULONG count = impl->partial_trust ? 3 : 5;
    IID *list;

    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    if (!(list = CoTaskMemAlloc( count * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IActivationFactory;
    list[1] = IID_IProtectionPolicyManagerStatics;
    list[2] = IID_IProtectionPolicyManagerStatics2;
    if (!impl->partial_trust)
    {
        list[3] = IID_IProtectionPolicyManagerStatics3;
        list[4] = IID_IProtectionPolicyManagerStatics4;
    }
    *iid_count = count;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    struct policy_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );

    *trust_level = impl->partial_trust ? PartialTrust : BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
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

/* one set of IUnknown and IInspectable methods per statics interface, all forwarding to the factory */
#define DEFINE_STATICS_IINSPECTABLE( pfx, type )                                                                           \
    static inline struct policy_factory *impl_from_##type( type *iface )                                                   \
    {                                                                                                                      \
        return CONTAINING_RECORD( iface, struct policy_factory, type##_iface );                                            \
    }                                                                                                                      \
    static HRESULT WINAPI pfx##_QueryInterface( type *iface, REFIID iid, void **out )                                      \
    {                                                                                                                      \
        return factory_QueryInterface( &impl_from_##type( iface )->IActivationFactory_iface, iid, out );                   \
    }                                                                                                                      \
    static ULONG WINAPI pfx##_AddRef( type *iface )                                                                        \
    {                                                                                                                      \
        return factory_AddRef( &impl_from_##type( iface )->IActivationFactory_iface );                                     \
    }                                                                                                                      \
    static ULONG WINAPI pfx##_Release( type *iface )                                                                       \
    {                                                                                                                      \
        return factory_Release( &impl_from_##type( iface )->IActivationFactory_iface );                                    \
    }                                                                                                                      \
    static HRESULT WINAPI pfx##_GetIids( type *iface, ULONG *iid_count, IID **iids )                                       \
    {                                                                                                                      \
        return factory_GetIids( &impl_from_##type( iface )->IActivationFactory_iface, iid_count, iids );                   \
    }                                                                                                                      \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( type *iface, HSTRING *class_name )                                    \
    {                                                                                                                      \
        return factory_GetRuntimeClassName( &impl_from_##type( iface )->IActivationFactory_iface, class_name );            \
    }                                                                                                                      \
    static HRESULT WINAPI pfx##_GetTrustLevel( type *iface, TrustLevel *trust_level )                                      \
    {                                                                                                                      \
        return factory_GetTrustLevel( &impl_from_##type( iface )->IActivationFactory_iface, trust_level );                 \
    }

static BOOL is_empty( HSTRING str )
{
    return WindowsIsStringEmpty( str );
}

static HRESULT add_event( EventRegistrationToken *token )
{
    token->value = InterlockedIncrement( &event_token_counter );
    return S_OK;
}

/* IProtectionPolicyManagerStatics */

DEFINE_STATICS_IINSPECTABLE( statics, IProtectionPolicyManagerStatics )

static HRESULT WINAPI statics_IsIdentityManaged( IProtectionPolicyManagerStatics *iface, HSTRING identity, boolean *result )
{
    TRACE( "iface %p, identity %s, result %p.\n", iface, debugstr_hstring( identity ), result );

    if (impl_from_IProtectionPolicyManagerStatics( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    if (is_empty( identity )) return E_INVALIDARG;
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics_TryApplyProcessUIPolicy( IProtectionPolicyManagerStatics *iface, HSTRING identity, boolean *result )
{
    TRACE( "iface %p, identity %s, result %p.\n", iface, debugstr_hstring( identity ), result );

    if (impl_from_IProtectionPolicyManagerStatics( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    if (is_empty( identity )) return E_INVALIDARG;
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics_ClearProcessUIPolicy( IProtectionPolicyManagerStatics *iface )
{
    TRACE( "iface %p.\n", iface );

    if (impl_from_IProtectionPolicyManagerStatics( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    return S_OK;
}

static HRESULT WINAPI statics_CreateCurrentThreadNetworkContext( IProtectionPolicyManagerStatics *iface, HSTRING identity, IInspectable **context )
{
    FIXME( "iface %p, identity %s, context %p stub!\n", iface, debugstr_hstring( identity ), context );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_GetPrimaryManagedIdentityForNetworkEndpointAsync( IProtectionPolicyManagerStatics *iface, IInspectable *endpoint_host,
                                                                                IInspectable **operation )
{
    FIXME( "iface %p, endpoint_host %p, operation %p stub!\n", iface, endpoint_host, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_RevokeContent( IProtectionPolicyManagerStatics *iface, HSTRING identity )
{
    TRACE( "iface %p, identity %s.\n", iface, debugstr_hstring( identity ) );

    if (is_empty( identity )) return E_INVALIDARG;
    return S_OK;
}

static HRESULT WINAPI statics_GetForCurrentView( IProtectionPolicyManagerStatics *iface, IProtectionPolicyManager **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );

    *result = NULL;
    return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

static HRESULT WINAPI statics_add_ProtectedAccessSuspending( IProtectionPolicyManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                             EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return add_event( token );
}

static HRESULT WINAPI statics_remove_ProtectedAccessSuspending( IProtectionPolicyManagerStatics *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI statics_add_ProtectedAccessResumed( IProtectionPolicyManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                          EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return add_event( token );
}

static HRESULT WINAPI statics_remove_ProtectedAccessResumed( IProtectionPolicyManagerStatics *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI statics_add_ProtectedContentRevoked( IProtectionPolicyManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                           EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return add_event( token );
}

static HRESULT WINAPI statics_remove_ProtectedContentRevoked( IProtectionPolicyManagerStatics *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI statics_CheckAccess( IProtectionPolicyManagerStatics *iface, HSTRING source_identity, HSTRING target_identity,
                                           ProtectionPolicyEvaluationResult *result )
{
    TRACE( "iface %p, source_identity %s, target_identity %s, result %p.\n", iface, debugstr_hstring( source_identity ),
           debugstr_hstring( target_identity ), result );

    if (impl_from_IProtectionPolicyManagerStatics( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    *result = ProtectionPolicyEvaluationResult_Allowed;
    return S_OK;
}

static HRESULT WINAPI statics_RequestAccessAsync( IProtectionPolicyManagerStatics *iface, HSTRING source_identity, HSTRING target_identity,
                                                  IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    TRACE( "iface %p, source_identity %s, target_identity %s, operation %p.\n", iface, debugstr_hstring( source_identity ),
           debugstr_hstring( target_identity ), operation );

    /* Windows without a policy reports ERROR_NOT_FOUND here */
    *operation = NULL;
    return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
}

static const struct IProtectionPolicyManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IProtectionPolicyManagerStatics methods */
    statics_IsIdentityManaged,
    statics_TryApplyProcessUIPolicy,
    statics_ClearProcessUIPolicy,
    statics_CreateCurrentThreadNetworkContext,
    statics_GetPrimaryManagedIdentityForNetworkEndpointAsync,
    statics_RevokeContent,
    statics_GetForCurrentView,
    statics_add_ProtectedAccessSuspending,
    statics_remove_ProtectedAccessSuspending,
    statics_add_ProtectedAccessResumed,
    statics_remove_ProtectedAccessResumed,
    statics_add_ProtectedContentRevoked,
    statics_remove_ProtectedContentRevoked,
    statics_CheckAccess,
    statics_RequestAccessAsync,
};

/* IProtectionPolicyManagerStatics2 */

DEFINE_STATICS_IINSPECTABLE( statics2, IProtectionPolicyManagerStatics2 )

static HRESULT WINAPI statics2_HasContentBeenRevokedSince( IProtectionPolicyManagerStatics2 *iface, HSTRING identity, DateTime since, boolean *result )
{
    TRACE( "iface %p, identity %s, since %#I64x, result %p.\n", iface, debugstr_hstring( identity ), since.UniversalTime, result );

    /* measured: TRUE, although nothing was ever revoked */
    *result = TRUE;
    return S_OK;
}

static HRESULT WINAPI statics2_CheckAccessForApp( IProtectionPolicyManagerStatics2 *iface, HSTRING source_identity, HSTRING app_package_family_name,
                                                  ProtectionPolicyEvaluationResult *result )
{
    TRACE( "iface %p, source_identity %s, app_package_family_name %s, result %p.\n", iface, debugstr_hstring( source_identity ),
           debugstr_hstring( app_package_family_name ), result );

    if (is_empty( app_package_family_name )) return E_INVALIDARG;
    *result = ProtectionPolicyEvaluationResult_Allowed;
    return S_OK;
}

static HRESULT WINAPI statics2_RequestAccessForAppAsync( IProtectionPolicyManagerStatics2 *iface, HSTRING source_identity,
                                                         HSTRING app_package_family_name, IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, source_identity %s, app_package_family_name %s, operation %p stub!\n", iface, debugstr_hstring( source_identity ),
           debugstr_hstring( app_package_family_name ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_GetEnforcementLevel( IProtectionPolicyManagerStatics2 *iface, HSTRING identity, EnforcementLevel *result )
{
    TRACE( "iface %p, identity %s, result %p.\n", iface, debugstr_hstring( identity ), result );

    if (impl_from_IProtectionPolicyManagerStatics2( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    *result = EnforcementLevel_NoProtection;
    return S_OK;
}

static HRESULT WINAPI statics2_IsUserDecryptionAllowed( IProtectionPolicyManagerStatics2 *iface, HSTRING identity, boolean *result )
{
    TRACE( "iface %p, identity %s, result %p.\n", iface, debugstr_hstring( identity ), result );

    if (impl_from_IProtectionPolicyManagerStatics2( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    *result = TRUE;
    return S_OK;
}

static HRESULT WINAPI statics2_IsProtectionUnderLockRequired( IProtectionPolicyManagerStatics2 *iface, HSTRING identity, boolean *result )
{
    TRACE( "iface %p, identity %s, result %p.\n", iface, debugstr_hstring( identity ), result );

    if (impl_from_IProtectionPolicyManagerStatics2( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics2_add_PolicyChanged( IProtectionPolicyManagerStatics2 *iface, IEventHandler_IInspectable *handler,
                                                  EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    return add_event( token );
}

static HRESULT WINAPI statics2_remove_PolicyChanged( IProtectionPolicyManagerStatics2 *iface, EventRegistrationToken token )
{
    TRACE( "iface %p, token %#I64x.\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI statics2_get_IsProtectionEnabled( IProtectionPolicyManagerStatics2 *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (impl_from_IProtectionPolicyManagerStatics2( iface )->partial_trust) return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    *value = FALSE;
    return S_OK;
}

static const struct IProtectionPolicyManagerStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IProtectionPolicyManagerStatics2 methods */
    statics2_HasContentBeenRevokedSince,
    statics2_CheckAccessForApp,
    statics2_RequestAccessForAppAsync,
    statics2_GetEnforcementLevel,
    statics2_IsUserDecryptionAllowed,
    statics2_IsProtectionUnderLockRequired,
    statics2_add_PolicyChanged,
    statics2_remove_PolicyChanged,
    statics2_get_IsProtectionEnabled,
};

/* IProtectionPolicyManagerStatics3: the requests that carry audit information; none of them is used without a policy */

DEFINE_STATICS_IINSPECTABLE( statics3, IProtectionPolicyManagerStatics3 )

static HRESULT WINAPI statics3_RequestAccessWithAuditingInfoAsync( IProtectionPolicyManagerStatics3 *iface, HSTRING source_identity,
                                                                   HSTRING target_identity, IInspectable *audit_info,
                                                                   IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, source_identity %s, target_identity %s, audit_info %p, operation %p stub!\n", iface,
           debugstr_hstring( source_identity ), debugstr_hstring( target_identity ), audit_info, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics3_RequestAccessWithMessageAsync( IProtectionPolicyManagerStatics3 *iface, HSTRING source_identity,
                                                              HSTRING target_identity, IInspectable *audit_info, HSTRING message_from_app,
                                                              IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, source_identity %s, target_identity %s, audit_info %p, message_from_app %s, operation %p stub!\n", iface,
           debugstr_hstring( source_identity ), debugstr_hstring( target_identity ), audit_info, debugstr_hstring( message_from_app ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics3_RequestAccessForAppWithAuditingInfoAsync( IProtectionPolicyManagerStatics3 *iface, HSTRING source_identity,
                                                                         HSTRING app_package_family_name, IInspectable *audit_info,
                                                                         IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, source_identity %s, app_package_family_name %s, audit_info %p, operation %p stub!\n", iface,
           debugstr_hstring( source_identity ), debugstr_hstring( app_package_family_name ), audit_info, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics3_RequestAccessForAppWithMessageAsync( IProtectionPolicyManagerStatics3 *iface, HSTRING source_identity,
                                                                    HSTRING app_package_family_name, IInspectable *audit_info,
                                                                    HSTRING message_from_app, IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, source_identity %s, app_package_family_name %s, audit_info %p, message_from_app %s, operation %p stub!\n", iface,
           debugstr_hstring( source_identity ), debugstr_hstring( app_package_family_name ), audit_info, debugstr_hstring( message_from_app ),
           operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics3_LogAuditEvent( IProtectionPolicyManagerStatics3 *iface, HSTRING source_identity, HSTRING target_identity,
                                              IInspectable *audit_info )
{
    TRACE( "iface %p, source_identity %s, target_identity %s, audit_info %p.\n", iface, debugstr_hstring( source_identity ),
           debugstr_hstring( target_identity ), audit_info );
    return S_OK;
}

static const struct IProtectionPolicyManagerStatics3Vtbl statics3_vtbl =
{
    statics3_QueryInterface,
    statics3_AddRef,
    statics3_Release,
    /* IInspectable methods */
    statics3_GetIids,
    statics3_GetRuntimeClassName,
    statics3_GetTrustLevel,
    /* IProtectionPolicyManagerStatics3 methods */
    statics3_RequestAccessWithAuditingInfoAsync,
    statics3_RequestAccessWithMessageAsync,
    statics3_RequestAccessForAppWithAuditingInfoAsync,
    statics3_RequestAccessForAppWithMessageAsync,
    statics3_LogAuditEvent,
};

/* IProtectionPolicyManagerStatics4 */

DEFINE_STATICS_IINSPECTABLE( statics4, IProtectionPolicyManagerStatics4 )

static HRESULT WINAPI statics4_IsRoamableProtectionEnabled( IProtectionPolicyManagerStatics4 *iface, HSTRING identity, boolean *result )
{
    TRACE( "iface %p, identity %s, result %p.\n", iface, debugstr_hstring( identity ), result );

    if (is_empty( identity )) return E_INVALIDARG;
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics4_RequestAccessWithBehaviorAsync( IProtectionPolicyManagerStatics4 *iface, HSTRING source_identity,
                                                               HSTRING target_identity, IInspectable *audit_info, HSTRING message_from_app,
                                                               ProtectionPolicyRequestAccessBehavior behavior,
                                                               IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, source_identity %s, target_identity %s, audit_info %p, message_from_app %s, behavior %d, operation %p stub!\n", iface,
           debugstr_hstring( source_identity ), debugstr_hstring( target_identity ), audit_info, debugstr_hstring( message_from_app ),
           behavior, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_RequestAccessForAppWithBehaviorAsync( IProtectionPolicyManagerStatics4 *iface, HSTRING source_identity,
                                                                     HSTRING app_package_family_name, IInspectable *audit_info,
                                                                     HSTRING message_from_app, ProtectionPolicyRequestAccessBehavior behavior,
                                                                     IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, source_identity %s, app_package_family_name %s, audit_info %p, message_from_app %s, behavior %d, operation %p stub!\n",
           iface, debugstr_hstring( source_identity ), debugstr_hstring( app_package_family_name ), audit_info,
           debugstr_hstring( message_from_app ), behavior, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_RequestAccessToFilesForAppAsync( IProtectionPolicyManagerStatics4 *iface, IInspectable *source_item_list,
                                                                HSTRING app_package_family_name, IInspectable *audit_info, IInspectable **operation )
{
    FIXME( "iface %p, source_item_list %p, app_package_family_name %s, audit_info %p, operation %p stub!\n", iface, source_item_list,
           debugstr_hstring( app_package_family_name ), audit_info, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_RequestAccessToFilesForAppWithMessageAndBehaviorAsync( IProtectionPolicyManagerStatics4 *iface,
                                                                                      IInspectable *source_item_list, HSTRING app_package_family_name,
                                                                                      IInspectable *audit_info, HSTRING message_from_app,
                                                                                      ProtectionPolicyRequestAccessBehavior behavior,
                                                                                      IInspectable **operation )
{
    FIXME( "iface %p, source_item_list %p, app_package_family_name %s, audit_info %p, message_from_app %s, behavior %d, operation %p stub!\n",
           iface, source_item_list, debugstr_hstring( app_package_family_name ), audit_info, debugstr_hstring( message_from_app ), behavior,
           operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_RequestAccessToFilesForProcessAsync( IProtectionPolicyManagerStatics4 *iface, IInspectable *source_item_list,
                                                                    UINT32 process_id, IInspectable *audit_info, IInspectable **operation )
{
    FIXME( "iface %p, source_item_list %p, process_id %u, audit_info %p, operation %p stub!\n", iface, source_item_list, process_id,
           audit_info, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_RequestAccessToFilesForProcessWithMessageAndBehaviorAsync( IProtectionPolicyManagerStatics4 *iface,
                                                                                          IInspectable *source_item_list, UINT32 process_id,
                                                                                          IInspectable *audit_info, HSTRING message_from_app,
                                                                                          ProtectionPolicyRequestAccessBehavior behavior,
                                                                                          IInspectable **operation )
{
    FIXME( "iface %p, source_item_list %p, process_id %u, audit_info %p, message_from_app %s, behavior %d, operation %p stub!\n", iface,
           source_item_list, process_id, audit_info, debugstr_hstring( message_from_app ), behavior, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_IsFileProtectionRequiredAsync( IProtectionPolicyManagerStatics4 *iface, IInspectable *target, HSTRING identity,
                                                              IInspectable **operation )
{
    FIXME( "iface %p, target %p, identity %s, operation %p stub!\n", iface, target, debugstr_hstring( identity ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_IsFileProtectionRequiredForNewFileAsync( IProtectionPolicyManagerStatics4 *iface, IInspectable *parent_folder,
                                                                        HSTRING identity, HSTRING desired_name, IInspectable **operation )
{
    FIXME( "iface %p, parent_folder %p, identity %s, desired_name %s, operation %p stub!\n", iface, parent_folder,
           debugstr_hstring( identity ), debugstr_hstring( desired_name ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_get_PrimaryManagedIdentity( IProtectionPolicyManagerStatics4 *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    return WindowsCreateString( NULL, 0, value );
}

static HRESULT WINAPI statics4_GetPrimaryManagedIdentityForIdentity( IProtectionPolicyManagerStatics4 *iface, HSTRING identity, HSTRING *result )
{
    TRACE( "iface %p, identity %s, result %p.\n", iface, debugstr_hstring( identity ), result );

    return WindowsCreateString( NULL, 0, result );
}

static const struct IProtectionPolicyManagerStatics4Vtbl statics4_vtbl =
{
    statics4_QueryInterface,
    statics4_AddRef,
    statics4_Release,
    /* IInspectable methods */
    statics4_GetIids,
    statics4_GetRuntimeClassName,
    statics4_GetTrustLevel,
    /* IProtectionPolicyManagerStatics4 methods */
    statics4_IsRoamableProtectionEnabled,
    statics4_RequestAccessWithBehaviorAsync,
    statics4_RequestAccessForAppWithBehaviorAsync,
    statics4_RequestAccessToFilesForAppAsync,
    statics4_RequestAccessToFilesForAppWithMessageAndBehaviorAsync,
    statics4_RequestAccessToFilesForProcessAsync,
    statics4_RequestAccessToFilesForProcessWithMessageAndBehaviorAsync,
    statics4_IsFileProtectionRequiredAsync,
    statics4_IsFileProtectionRequiredForNewFileAsync,
    statics4_get_PrimaryManagedIdentity,
    statics4_GetPrimaryManagedIdentityForIdentity,
};

static struct policy_factory policy_manager_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    {&statics3_vtbl},
    {&statics4_vtbl},
    FALSE,
    1,
};

/* the factory that Notepad asks for by a second name; it serves the first two statics and is partially trusted: on Windows
 * it answers ERROR_NOT_SUPPORTED to the queries of the policy state and works for the rest (the change event, RevokeContent,
 * CheckAccessForApp and HasContentBeenRevokedSince) */
static struct policy_factory policy_manager_private_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    {&statics3_vtbl},
    {&statics4_vtbl},
    TRUE,
    1,
};

IActivationFactory *policy_manager_factory = &policy_manager_impl.IActivationFactory_iface;
IActivationFactory *policy_manager_private_factory = &policy_manager_private_impl.IActivationFactory_iface;
