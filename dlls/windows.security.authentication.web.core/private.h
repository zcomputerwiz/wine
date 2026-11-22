/* WinRT Windows.Security.Authentication.Web.Core Implementation
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

#ifndef __WINE_WEBCORE_PRIVATE_H
#define __WINE_WEBCORE_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "activation.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_System
#include "windows.system.h"
#define WIDL_using_Windows_Storage_Streams
#include "windows.storage.streams.h"
#define WIDL_using_Windows_Security_Credentials
#include "windows.security.credentials.h"
#define WIDL_using_Windows_Security_Authentication_Web_Core
#include "windows.security.authentication.web.core.h"

#include "webauthenticationcoremanagerinterop.h"

#include "async_private.h"

/*
 *
 * Factories
 *
 */

extern IActivationFactory *manager_factory;
extern IActivationFactory *request_factory;
extern IActivationFactory *response_factory;
extern IActivationFactory *providererror_factory;

/*
 *
 * async.c
 *
 */

HRESULT async_operation_inspectable_create_completed( const GUID *iid, IInspectable *result, IAsyncOperation_IInspectable **out );
HRESULT async_operation_inspectable_create( const GUID *iid, IUnknown *invoker, IUnknown *param,
                                            async_operation_callback callback, IAsyncOperation_IInspectable **out );

/*
 *
 * map.c
 *
 */

HRESULT map_hstring_create( IMap_HSTRING_HSTRING **out );
void map_hstring_dump( IMap_HSTRING_HSTRING *iface, const char *prefix );
BOOL diagnostic_provider_enabled( void );
HRESULT map_hstring_pairs( IMap_HSTRING_HSTRING *iface, WCHAR **text, UINT32 *len );
void diagnostic_interaction_error( IInspectable *request, UINT32 *code, const WCHAR **message );
HRESULT diagnostic_provider_create( HSTRING id, HSTRING authority, IWebAccountProvider **out );
HRESULT provider_create( HSTRING id, HSTRING authority, const WCHAR *display_name, IWebAccountProvider **out );
void diagnostic_dump_request( const char *method, IInspectable *request );

/*
 *
 * broker.c
 *
 */

BOOL token_broker_enabled( void );
HRESULT token_broker_request( IInspectable *request, IInspectable *account, BOOL silent, HWND window,
                              IWebTokenRequestResult **result );
HRESULT token_broker_find_accounts( IWebAccountProvider *provider, HSTRING client_id, IFindAllAccountsResult **result );
HRESULT token_broker_find_account( IWebAccountProvider *provider, HSTRING id, IWebAccount **account );

/*
 *
 * account.c
 *
 */

HRESULT web_account_create( IWebAccountProvider *provider, const WCHAR *id, UINT32 id_len, const WCHAR *user_name,
                            UINT32 user_name_len, WebAccountState state, IWebAccount **out );
void web_account_set_properties( IWebAccount *account, IMap_HSTRING_HSTRING *properties );
HSTRING web_account_id( IInspectable *account );

/*
 *
 * response.c
 *
 */

HRESULT webprovidererror_create( UINT32 error_code, HSTRING error_message, IWebProviderError **out );
HRESULT webtokenrequestresult_create( WebTokenRequestStatus status, IVectorView_WebTokenResponse *response_data,
                                      IWebProviderError *response_error, IWebTokenRequestResult **out );
HRESULT findallaccountsresult_create( FindAllWebAccountsStatus status, IVectorView_WebAccount *accounts,
                                      IWebProviderError *provider_error, IFindAllAccountsResult **out );
HRESULT vector_view_webtokenresponse_create_empty( IVectorView_WebTokenResponse **out );
HRESULT vector_view_webaccount_create_empty( IVectorView_WebAccount **out );
HRESULT vector_view_webtokenresponse_create( IWebTokenResponse **items, UINT32 count, IVectorView_WebTokenResponse **out );
HRESULT vector_view_webaccount_create( IWebAccount **items, UINT32 count, IVectorView_WebAccount **out );
HRESULT webtokenresponse_create( HSTRING token, IWebAccount *web_account, IMap_HSTRING_HSTRING *properties,
                                 IWebTokenResponse **out );

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )
#define DEFINE_IINSPECTABLE_OUTER( pfx, iface_type, impl_type, outer_iface )                       \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, impl->outer_iface )

#endif
