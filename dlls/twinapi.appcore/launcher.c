/* WinRT Windows.System.Launcher Implementation
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

#define WIDL_using_Windows_Storage
#include "private.h"

#include "shellapi.h"
#include "windows.storage.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/* The launcher hands a URI or a file to the shell, as the Windows shell does for the default handler; the operations
 * it returns are done when made, their result says whether the shell took the request. */
struct launcher
{
    IActivationFactory IActivationFactory_iface;
    ILauncherStatics ILauncherStatics_iface;
    LONG ref;
};

static inline struct launcher *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct launcher, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct launcher *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ILauncherStatics ))
        *out = &impl->ILauncherStatics_iface;
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
    struct launcher *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct launcher *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_ILauncherStatics;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return create_name( L"Windows.System.Launcher", name );
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

static inline struct launcher *impl_from_ILauncherStatics( ILauncherStatics *iface )
{
    return CONTAINING_RECORD( iface, struct launcher, ILauncherStatics_iface );
}

static HRESULT WINAPI statics_QueryInterface( ILauncherStatics *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI statics_AddRef( ILauncherStatics *iface )
{
    return factory_AddRef( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface );
}

static ULONG WINAPI statics_Release( ILauncherStatics *iface )
{
    return factory_Release( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface );
}

static HRESULT WINAPI statics_GetIids( ILauncherStatics *iface, ULONG *count, IID **iids )
{
    return factory_GetIids( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, count, iids );
}

static HRESULT WINAPI statics_GetRuntimeClassName( ILauncherStatics *iface, HSTRING *name )
{
    return factory_GetRuntimeClassName( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, name );
}

static HRESULT WINAPI statics_GetTrustLevel( ILauncherStatics *iface, TrustLevel *level )
{
    return factory_GetTrustLevel( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, level );
}

static HRESULT shell_open( const WCHAR *target, IAsyncOperation_boolean **operation )
{
    boolean launched = FALSE;

    TRACE( "target %s.\n", debugstr_w( target ) );
    if (target && *target) launched = (INT_PTR)ShellExecuteW( NULL, L"open", target, NULL, NULL, SW_SHOWNORMAL ) > 32;
    return async_bool_completed( launched, operation );
}

static HRESULT launch_uri( IUriRuntimeClass *uri, IAsyncOperation_boolean **operation )
{
    HSTRING string;
    HRESULT hr;

    *operation = NULL;
    if (!uri) return E_INVALIDARG;
    if (FAILED(hr = IUriRuntimeClass_get_AbsoluteUri( uri, &string ))) return hr;
    hr = shell_open( WindowsGetStringRawBuffer( string, NULL ), operation );
    WindowsDeleteString( string );
    return hr;
}

static HRESULT launch_file( IStorageFile *file, IAsyncOperation_boolean **operation )
{
    IStorageItem *item;
    HSTRING path;
    HRESULT hr;

    *operation = NULL;
    if (!file) return E_INVALIDARG;
    if (FAILED(hr = IStorageFile_QueryInterface( file, &IID_IStorageItem, (void **)&item ))) return hr;
    hr = IStorageItem_get_Path( item, &path );
    IStorageItem_Release( item );
    if (FAILED(hr)) return hr;
    hr = shell_open( WindowsGetStringRawBuffer( path, NULL ), operation );
    WindowsDeleteString( path );
    return hr;
}

static HRESULT WINAPI statics_LaunchFileAsync( ILauncherStatics *iface, IStorageFile *file,
                                               IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );
    return launch_file( file, operation );
}

static HRESULT WINAPI statics_LaunchFileWithOptionsAsync( ILauncherStatics *iface, IStorageFile *file,
                                                          IInspectable *options, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, file %p, options %p, operation %p.\n", iface, file, options, operation );
    return launch_file( file, operation );
}

static HRESULT WINAPI statics_LaunchUriAsync( ILauncherStatics *iface, IUriRuntimeClass *uri,
                                              IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, uri %p, operation %p.\n", iface, uri, operation );
    return launch_uri( uri, operation );
}

static HRESULT WINAPI statics_LaunchUriWithOptionsAsync( ILauncherStatics *iface, IUriRuntimeClass *uri,
                                                         IInspectable *options, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, uri %p, options %p, operation %p.\n", iface, uri, options, operation );
    return launch_uri( uri, operation );
}

static const struct ILauncherStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* ILauncherStatics methods */
    statics_LaunchFileAsync,
    statics_LaunchFileWithOptionsAsync,
    statics_LaunchUriAsync,
    statics_LaunchUriWithOptionsAsync,
};

static struct launcher launcher_impl =
{
    {&factory_vtbl},
    {&statics_vtbl},
    1,
};

IActivationFactory *launcher_factory = &launcher_impl.IActivationFactory_iface;
