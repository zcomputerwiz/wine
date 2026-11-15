/* WinRT Windows.System.User of the running application (WineExplorer UWP compatibility)
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
 * The user a launch activation is for (IActivatedEventArgsWithUser.User): the user of the process token, a local,
 * locally authenticated user. NonRoamableId has the shape Windows gives it (44 characters of base64, the same for the
 * same user, different for another): the base64 of the SHA-256 of the user's SID. The user's properties and picture
 * (GetPropertyAsync, GetPropertiesAsync, GetPictureAsync) are not implemented.
 */

#include "private.h"
#include "sddl.h"
#include "bcrypt.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct user
{
    IUser IUser_iface;
    LONG ref;
    HSTRING id;
};

static inline struct user *impl_from_IUser( IUser *iface )
{
    return CONTAINING_RECORD( iface, struct user, IUser_iface );
}

static HRESULT WINAPI user_QueryInterface( IUser *iface, REFIID iid, void **out )
{
    struct user *impl = impl_from_IUser( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IUser ))
    {
        IUser_AddRef( (*out = &impl->IUser_iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI user_AddRef( IUser *iface )
{
    return InterlockedIncrement( &impl_from_IUser( iface )->ref );
}

static ULONG WINAPI user_Release( IUser *iface )
{
    struct user *impl = impl_from_IUser( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI user_GetIids( IUser *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI user_GetRuntimeClassName( IUser *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.System.User", 19, class_name );
}

static HRESULT WINAPI user_GetTrustLevel( IUser *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI user_get_NonRoamableId( IUser *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IUser( iface )->id, value );
}

static HRESULT WINAPI user_get_AuthenticationStatus( IUser *iface, UserAuthenticationStatus *value )
{
    *value = UserAuthenticationStatus_LocallyAuthenticated;
    return S_OK;
}

static HRESULT WINAPI user_get_Type( IUser *iface, UserType *value )
{
    *value = UserType_LocalUser;
    return S_OK;
}

static HRESULT WINAPI user_GetPropertyAsync( IUser *iface, HSTRING value, IAsyncOperation_IInspectable **operation )
{
    FIXME( "iface %p, value %s, operation %p stub!\n", iface, debugstr_hstring( value ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI user_GetPropertiesAsync( IUser *iface, IVectorView_HSTRING *values,
                                               IAsyncOperation_IPropertySet **operation )
{
    FIXME( "iface %p, values %p, operation %p stub!\n", iface, values, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI user_GetPictureAsync( IUser *iface, UserPictureSize size,
                                            IAsyncOperation_IRandomAccessStreamReference **operation )
{
    FIXME( "iface %p, size %d, operation %p stub!\n", iface, size, operation );
    return E_NOTIMPL;
}

static const IUserVtbl user_vtbl =
{
    user_QueryInterface,
    user_AddRef,
    user_Release,
    /* IInspectable methods */
    user_GetIids,
    user_GetRuntimeClassName,
    user_GetTrustLevel,
    /* IUser methods */
    user_get_NonRoamableId,
    user_get_AuthenticationStatus,
    user_get_Type,
    user_GetPropertyAsync,
    user_GetPropertiesAsync,
    user_GetPictureAsync,
};

/* the base64 of the SHA-256 of the process user's SID string: 44 characters */
static HRESULT user_id( HSTRING *out )
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    BYTE token_user[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)], digest[32];
    WCHAR *sid = NULL, id[45];
    HANDLE token;
    DWORD size;
    unsigned int i, j;
    NTSTATUS status;

    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token )) return HRESULT_FROM_WIN32( GetLastError() );
    if (!GetTokenInformation( token, TokenUser, token_user, sizeof(token_user), &size ) ||
        !ConvertSidToStringSidW( ((TOKEN_USER *)token_user)->User.Sid, &sid ))
    {
        CloseHandle( token );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    CloseHandle( token );
    status = BCryptHash( BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (BYTE *)sid, wcslen( sid ) * sizeof(WCHAR), digest, sizeof(digest) );
    LocalFree( sid );
    if (status) return HRESULT_FROM_NT( status );

    for (i = j = 0; i < sizeof(digest); i += 3)
    {
        UINT32 bits = digest[i] << 16 | (i + 1 < sizeof(digest) ? digest[i + 1] << 8 : 0) |
                      (i + 2 < sizeof(digest) ? digest[i + 2] : 0);
        id[j++] = alphabet[(bits >> 18) & 63];
        id[j++] = alphabet[(bits >> 12) & 63];
        id[j++] = i + 1 < sizeof(digest) ? alphabet[(bits >> 6) & 63] : '=';
        id[j++] = i + 2 < sizeof(digest) ? alphabet[bits & 63] : '=';
    }
    id[j] = 0;
    return WindowsCreateString( id, j, out );
}

HRESULT user_create( IUser **out )
{
    struct user *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUser_iface.lpVtbl = &user_vtbl;
    impl->ref = 1;
    if (FAILED(hr = user_id( &impl->id )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IUser_iface;
    return S_OK;
}
