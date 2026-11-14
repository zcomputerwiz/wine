/* WinRT Windows.UI.Xaml: classes served by an installed XAML component
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
 * The XAML framework itself is not part of this library. When a XAML component is installed (WineExplorer's setup
 * installs one, built on the Uno Platform), the value ComponentPath of HKLM\Software\Wine\Xaml names its directory:
 * a self-contained .NET application XamlShim (XamlShim.exe, XamlShim.dll, hostfxr.dll). The first class this library
 * does not implement starts the .NET runtime in the process through hostfxr; every such class then goes to the
 * component's XamlShim.Activation.GetActivationFactory(const WCHAR *name, IActivationFactory **factory). Without a
 * component, nothing changes. The setup registers the component's classes (XamlShim.exe --classes) to this library.
 */

#include "private.h"
#include "winreg.h"

WINE_DEFAULT_DEBUG_CHANNEL(xaml);

typedef void *hostfxr_handle;

struct hostfxr_initialize_parameters
{
    size_t size;
    const WCHAR *host_path;     /* the component's apphost: hostfxr takes the application root from it */
    const WCHAR *dotnet_root;
};

typedef int (__cdecl *hostfxr_init_fn)( int argc, const WCHAR **argv, const struct hostfxr_initialize_parameters *params,
                                        hostfxr_handle *handle );
typedef int (__cdecl *hostfxr_get_delegate_fn)( hostfxr_handle handle, int type, void **delegate );
typedef int (WINAPI *get_function_pointer_fn)( const WCHAR *type, const WCHAR *method, const WCHAR *delegate_type,
                                               void *load_context, void *reserved, void **delegate );
typedef HRESULT (WINAPI *get_factory_fn)( const WCHAR *name, IActivationFactory **factory );

#define HDT_GET_FUNCTION_POINTER 6
#define UNMANAGEDCALLERSONLY_METHOD ((const WCHAR *)-1)

static INIT_ONCE start_once = INIT_ONCE_STATIC_INIT;
static get_factory_fn get_factory;

static BOOL component_file( const WCHAR *dir, const WCHAR *name, WCHAR *path, DWORD size )
{
    if (wcslen( dir ) + 1 + wcslen( name ) >= size) return FALSE;
    wcscpy( path, dir );
    wcscat( path, L"\\" );
    wcscat( path, name );
    return TRUE;
}

static BOOL CALLBACK start_component( INIT_ONCE *once, void *param, void **context )
{
    WCHAR dir[MAX_PATH], fxr[MAX_PATH], app[MAX_PATH], host[MAX_PATH];
    struct hostfxr_initialize_parameters params;
    get_function_pointer_fn get_function_pointer = NULL;
    hostfxr_get_delegate_fn get_delegate;
    hostfxr_handle handle = NULL;
    DWORD size = sizeof(dir), start = GetTickCount();
    hostfxr_init_fn init;
    const WCHAR *argv[1];
    HMODULE module;
    int rc;

    if (RegGetValueW( HKEY_LOCAL_MACHINE, L"Software\\Wine\\Xaml", L"ComponentPath", RRF_RT_REG_SZ, NULL, dir, &size ))
    {
        TRACE( "no XAML component installed.\n" );
        return TRUE;
    }
    if (!component_file( dir, L"hostfxr.dll", fxr, MAX_PATH ) || !component_file( dir, L"XamlShim.dll", app, MAX_PATH ) ||
        !component_file( dir, L"XamlShim.exe", host, MAX_PATH ))
        return TRUE;
    if (!(module = LoadLibraryW( fxr )))
    {
        ERR( "Cannot load %s, error %lu.\n", debugstr_w(fxr), GetLastError() );
        return TRUE;
    }
    init = (hostfxr_init_fn)GetProcAddress( module, "hostfxr_initialize_for_dotnet_command_line" );
    get_delegate = (hostfxr_get_delegate_fn)GetProcAddress( module, "hostfxr_get_runtime_delegate" );
    if (!init || !get_delegate)
    {
        ERR( "%s lacks the hosting exports.\n", debugstr_w(fxr) );
        return TRUE;
    }
    params.size = sizeof(params);
    params.host_path = host;
    params.dotnet_root = dir;
    argv[0] = app;
    if ((rc = init( 1, argv, &params, &handle )) < 0 || !handle)
    {
        ERR( "hostfxr initialization for %s failed, %#x.\n", debugstr_w(app), rc );
        return TRUE;
    }
    if ((rc = get_delegate( handle, HDT_GET_FUNCTION_POINTER, (void **)&get_function_pointer )) < 0 || !get_function_pointer)
    {
        ERR( "hostfxr_get_runtime_delegate failed, %#x.\n", rc );
        return TRUE;
    }
    rc = get_function_pointer( L"XamlShim.Activation, XamlShim", L"GetActivationFactory", UNMANAGEDCALLERSONLY_METHOD,
                               NULL, NULL, (void **)&get_factory );
    if (rc < 0) get_factory = NULL;
    TRACE( "XAML component %s started in %lu ms, %#x.\n", debugstr_w(dir), GetTickCount() - start, rc );
    return TRUE;
}

HRESULT component_get_activation_factory( HSTRING classid, IActivationFactory **factory )
{
    HRESULT hr;

    *factory = NULL;
    InitOnceExecuteOnce( &start_once, start_component, NULL, NULL );
    if (!get_factory) return CLASS_E_CLASSNOTAVAILABLE;
    hr = get_factory( WindowsGetStringRawBuffer( classid, NULL ), factory );
    TRACE( "class %s from the XAML component: %#lx.\n", debugstr_hstring(classid), hr );
    return hr;
}
