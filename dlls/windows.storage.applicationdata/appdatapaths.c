/* WinRT Windows.Storage.AppDataPaths implementation
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

#include "initguid.h"
#include "shlobj.h"
#include "appmodel.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(data);

/*
 * The paths of the folders of the user that an application may use: they do not depend on the package. For the current user
 * (GetDefault, and GetForUser with any user: only the current user exists).
 */

struct app_data_paths
{
    IAppDataPaths IAppDataPaths_iface;
    LONG ref;
};

static inline struct app_data_paths *impl_from_IAppDataPaths( IAppDataPaths *iface )
{
    return CONTAINING_RECORD( iface, struct app_data_paths, IAppDataPaths_iface );
}

static HRESULT WINAPI paths_QueryInterface( IAppDataPaths *iface, REFIID iid, void **out )
{
    struct app_data_paths *impl = impl_from_IAppDataPaths( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAppDataPaths ))
    {
        *out = &impl->IAppDataPaths_iface;
        IInspectable_AddRef( (IInspectable *)*out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI paths_AddRef( IAppDataPaths *iface )
{
    struct app_data_paths *impl = impl_from_IAppDataPaths( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI paths_Release( IAppDataPaths *iface )
{
    struct app_data_paths *impl = impl_from_IAppDataPaths( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI paths_GetIids( IAppDataPaths *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI paths_GetRuntimeClassName( IAppDataPaths *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Storage.AppDataPaths", ARRAY_SIZE(L"Windows.Storage.AppDataPaths") - 1, name );
}

static HRESULT WINAPI paths_GetTrustLevel( IAppDataPaths *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT known_folder( REFKNOWNFOLDERID id, HSTRING *value )
{
    WCHAR *path = NULL;
    UINT32 length = 0;
    HRESULT hr;
    LONG err;

    /* the paths need a package identity: a process without one gets APPMODEL_ERROR_NO_PACKAGE, even before the argument is
     * looked at (Windows 11) */
    if ((err = GetCurrentPackageFullName( &length, NULL )) == APPMODEL_ERROR_NO_PACKAGE) return HRESULT_FROM_WIN32( err );
    if (!value) return E_POINTER;
    if (FAILED(hr = SHGetKnownFolderPath( id, KF_FLAG_DEFAULT, NULL, &path ))) return hr;
    hr = WindowsCreateString( path, lstrlenW( path ), value );
    CoTaskMemFree( path );
    return hr;
}

#define PATH_GETTER( name, id ) \
    static HRESULT WINAPI paths_get_##name( IAppDataPaths *iface, HSTRING *value ) \
    { \
        TRACE( "iface %p, value %p.\n", iface, value ); \
        return known_folder( &id, value ); \
    }
PATH_GETTER( Cookies, FOLDERID_Cookies )
PATH_GETTER( Desktop, FOLDERID_Desktop )
PATH_GETTER( Documents, FOLDERID_Documents )
PATH_GETTER( Favorites, FOLDERID_Favorites )
PATH_GETTER( History, FOLDERID_History )
PATH_GETTER( InternetCache, FOLDERID_InternetCache )
PATH_GETTER( LocalAppData, FOLDERID_LocalAppData )
PATH_GETTER( ProgramData, FOLDERID_ProgramData )
PATH_GETTER( RoamingAppData, FOLDERID_RoamingAppData )

static const IAppDataPathsVtbl paths_vtbl =
{
    paths_QueryInterface,
    paths_AddRef,
    paths_Release,
    paths_GetIids,
    paths_GetRuntimeClassName,
    paths_GetTrustLevel,
    paths_get_Cookies,
    paths_get_Desktop,
    paths_get_Documents,
    paths_get_Favorites,
    paths_get_History,
    paths_get_InternetCache,
    paths_get_LocalAppData,
    paths_get_ProgramData,
    paths_get_RoamingAppData,
};

static HRESULT paths_create( IAppDataPaths **out )
{
    struct app_data_paths *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAppDataPaths_iface.lpVtbl = &paths_vtbl;
    impl->ref = 1;
    *out = &impl->IAppDataPaths_iface;
    return S_OK;
}

struct paths_statics
{
    IActivationFactory IActivationFactory_iface;
    IAppDataPathsStatics IAppDataPathsStatics_iface;
};

static inline struct paths_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct paths_statics, IActivationFactory_iface );
}

static inline struct paths_statics *impl_from_IAppDataPathsStatics( IAppDataPathsStatics *iface )
{
    return CONTAINING_RECORD( iface, struct paths_statics, IAppDataPathsStatics_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct paths_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IAppDataPathsStatics ))
        *out = &impl->IAppDataPathsStatics_iface;
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
    return 2;  /* a static object */
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Storage.AppDataPaths", ARRAY_SIZE(L"Windows.Storage.AppDataPaths") - 1, name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* AppDataPaths has no constructor */
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

static HRESULT WINAPI statics_QueryInterface( IAppDataPathsStatics *iface, REFIID iid, void **out )
{
    struct paths_statics *impl = impl_from_IAppDataPathsStatics( iface );
    return IActivationFactory_QueryInterface( &impl->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI statics_AddRef( IAppDataPathsStatics *iface )
{
    return 2;
}

static ULONG WINAPI statics_Release( IAppDataPathsStatics *iface )
{
    return 1;
}

static HRESULT WINAPI statics_GetIids( IAppDataPathsStatics *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_GetRuntimeClassName( IAppDataPathsStatics *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Storage.AppDataPaths", ARRAY_SIZE(L"Windows.Storage.AppDataPaths") - 1, name );
}

static HRESULT WINAPI statics_GetTrustLevel( IAppDataPathsStatics *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI statics_GetForUser( IAppDataPathsStatics *iface, IUser *user, IAppDataPaths **result )
{
    FIXME( "iface %p, user %p, result %p: only the current user, semi-stub\n", iface, user, result );
    return paths_create( result );
}

static HRESULT WINAPI statics_GetDefault( IAppDataPathsStatics *iface, IAppDataPaths **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return paths_create( result );
}

static const IAppDataPathsStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    statics_GetForUser,
    statics_GetDefault,
};

static struct paths_statics paths_statics =
{
    { &factory_vtbl },
    { &statics_vtbl },
};

IActivationFactory *app_data_paths_factory = &paths_statics.IActivationFactory_iface;
