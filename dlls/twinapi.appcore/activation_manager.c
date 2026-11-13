/*
 * ApplicationActivationManager: package activation by application user model id
 * (WineExplorer UWP compatibility)
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

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define COBJMACROS
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winternl.h"
#include "objbase.h"
#include "shobjidl.h"
#include "appmodel.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/*
 * The application is started by the process that asks for it, with a duplicate of that process's own
 * token: the manager never launches anything with the token of a server process. A client that asks
 * for a local server (the documented way to create this class) reaches a surrogate, whose object is
 * marshalled by handing the client an in-process manager of its own (IMarshal below), so that every
 * call still runs in the caller.
 *
 * The identity of the started process does not come from here either. The server reads the record of
 * the application in the package catalog of the prefix, attaches the package context to the duplicate
 * token, and lets the new process keep it only if the image it maps is the executable of that record.
 */

struct manager
{
    IApplicationActivationManager IApplicationActivationManager_iface;
    IMarshal IMarshal_iface;
    LONG refcount;
};

static inline struct manager *impl_from_IApplicationActivationManager( IApplicationActivationManager *iface )
{
    return CONTAINING_RECORD( iface, struct manager, IApplicationActivationManager_iface );
}

static inline struct manager *impl_from_IMarshal( IMarshal *iface )
{
    return CONTAINING_RECORD( iface, struct manager, IMarshal_iface );
}

/* the compatibility gate: exactly the DWORD 1; absent, another type or another value leave it off */
static BOOL uwp_compatibility_enabled(void)
{
    DWORD value = 0, type = REG_NONE, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, L"Software\\WineExplorer\\Shell", L"UwpCompatibility",
                      RRF_RT_REG_DWORD, &type, &value, &size ))
        return FALSE;
    return type == REG_DWORD && size == sizeof(value) && value == 1;
}

/*
 * The environment of the started process: that of the caller, with the folders of the packages the application
 * depends on (its direct dependencies, the framework packages) in front of PATH, so that the libraries of a framework
 * are found by name like those of the application. NULL (the caller's environment) when there are none.
 */
static WCHAR *build_environment( const WCHAR *full_name )
{
    PACKAGE_INFO_REFERENCE reference;
    UINT32 length = 0, count = 0, i;
    BYTE *buffer = NULL;
    WCHAR *env = NULL, *old_env, *new_env = NULL, *p, *dirs = NULL, *old_path = NULL;
    SIZE_T dirs_len = 0, env_len = 0;
    PACKAGE_INFO *infos;

    if (OpenPackageInfoByFullName( full_name, 0, &reference )) return NULL;
    if (GetPackageInfo( reference, PACKAGE_FILTER_DIRECT, &length, NULL, &count ) != ERROR_INSUFFICIENT_BUFFER || !count)
        goto done;
    if (!(buffer = malloc( length )) || GetPackageInfo( reference, PACKAGE_FILTER_DIRECT, &length, buffer, &count ))
        goto done;
    infos = (PACKAGE_INFO *)buffer;
    for (i = 0; i < count; i++) dirs_len += wcslen( infos[i].path ) + 1;
    if (!(dirs = malloc( (dirs_len + 1) * sizeof(WCHAR) ))) goto done;
    dirs[0] = 0;
    for (i = 0; i < count; i++)
    {
        wcscat( dirs, infos[i].path );
        wcscat( dirs, L";" );
    }

    if (!(old_env = GetEnvironmentStringsW())) goto done;
    for (p = old_env; *p; p += wcslen( p ) + 1)
    {
        if (!wcsnicmp( p, L"PATH=", 5 )) old_path = p + 5;
        env_len += wcslen( p ) + 1;
    }
    if ((new_env = malloc( (env_len + dirs_len + 16) * sizeof(WCHAR) )))
    {
        WCHAR *out = new_env;
        BOOL have_path = FALSE;

        for (p = old_env; *p; p += wcslen( p ) + 1)
        {
            if (!wcsnicmp( p, L"PATH=", 5 ))
            {
                have_path = TRUE;
                wcscpy( out, L"PATH=" );
                wcscat( out, dirs );
                wcscat( out, old_path );
            }
            else wcscpy( out, p );
            out += wcslen( out ) + 1;
        }
        if (!have_path)
        {
            wcscpy( out, L"PATH=" );
            wcscat( out, dirs );
            out += wcslen( out ) + 1;
        }
        *out = 0;
        env = new_env;
        new_env = NULL;
    }
    FreeEnvironmentStringsW( old_env );

done:
    free( new_env );
    free( dirs );
    free( buffer );
    ClosePackageInfo( reference );
    return env;
}

static WINE_TOKEN_PACKAGE_INFORMATION *get_token_package( HANDLE token, NTSTATUS *status )
{
    WINE_TOKEN_PACKAGE_INFORMATION *info = NULL;
    ULONG size = 0;

    *status = NtQueryInformationToken( token, TokenWinePackage, NULL, 0, &size );
    while (*status == STATUS_BUFFER_TOO_SMALL)
    {
        free( info );
        if (!(info = malloc( size )))
        {
            *status = STATUS_NO_MEMORY;
            return NULL;
        }
        *status = NtQueryInformationToken( token, TokenWinePackage, info, size, &size );
    }
    if (*status)
    {
        free( info );
        return NULL;
    }
    return info;
}

static HRESULT WINAPI manager_QueryInterface( IApplicationActivationManager *iface, REFIID iid, void **out )
{
    struct manager *manager = impl_from_IApplicationActivationManager( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IApplicationActivationManager ))
        *out = &manager->IApplicationActivationManager_iface;
    else if (IsEqualGUID( iid, &IID_IMarshal ))
        *out = &manager->IMarshal_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI manager_AddRef( IApplicationActivationManager *iface )
{
    struct manager *manager = impl_from_IApplicationActivationManager( iface );
    return InterlockedIncrement( &manager->refcount );
}

static ULONG WINAPI manager_Release( IApplicationActivationManager *iface )
{
    struct manager *manager = impl_from_IApplicationActivationManager( iface );
    ULONG ref = InterlockedDecrement( &manager->refcount );

    if (!ref) free( manager );
    return ref;
}

static HRESULT WINAPI manager_ActivateApplication( IApplicationActivationManager *iface, const WCHAR *aumid,
                                                   const WCHAR *arguments, ACTIVATEOPTIONS options, DWORD *pid )
{
    WINE_TOKEN_PACKAGE_INFORMATION *info = NULL;
    HANDLE process_token = NULL, token = NULL;
    WCHAR *path = NULL, *root = NULL, *cmdline = NULL, *environment = NULL;
    const WCHAR *root_str, *exe_str;
    size_t root_len, exe_len, args_len;
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    NTSTATUS status;
    HRESULT hr;

    TRACE( "iface %p, aumid %s, arguments %s, options %#x, pid %p.\n", iface, debugstr_w( aumid ),
           debugstr_w( arguments ), options, pid );

    if (!aumid || !pid) return E_INVALIDARG;
    *pid = 0;

    if (!uwp_compatibility_enabled())
    {
        WARN( "UWP compatibility is not enabled.\n" );
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    }
    if (options) FIXME( "options %#x are ignored.\n", options );

    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process_token ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!DuplicateTokenEx( process_token, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                           TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, NULL, SecurityImpersonation,
                           TokenPrimary, &token ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }

    /* the server finds the application in its catalog; nothing but its name is passed */
    status = NtSetInformationToken( token, TokenWinePackage, (void *)aumid, wcslen( aumid ) * sizeof(WCHAR) );
    if (status == STATUS_NOT_FOUND || status == STATUS_INVALID_PARAMETER)
    {
        WARN( "No application %s in the package catalog, status %#lx.\n", debugstr_w( aumid ), status );
        hr = E_APPLICATION_NOT_REGISTERED;
        goto done;
    }
    if (status)
    {
        WARN( "Failed to get the package context of %s, status %#lx.\n", debugstr_w( aumid ), status );
        hr = HRESULT_FROM_NT( status );
        goto done;
    }

    if (!(info = get_token_package( token, &status )))
    {
        hr = HRESULT_FROM_NT( status );
        goto done;
    }
    root_str = info->data + (info->full_name_len + info->family_name_len + info->app_id_len) / sizeof(WCHAR);
    root_len = info->root_len / sizeof(WCHAR);
    exe_str = root_str + root_len;
    exe_len = info->executable_len / sizeof(WCHAR);
    args_len = arguments ? wcslen( arguments ) : 0;

    hr = E_OUTOFMEMORY;
    if (!(root = malloc( (root_len + 1) * sizeof(WCHAR) ))) goto done;
    memcpy( root, root_str, root_len * sizeof(WCHAR) );
    root[root_len] = 0;

    if (!(path = malloc( (root_len + 1 + exe_len + 1) * sizeof(WCHAR) ))) goto done;
    memcpy( path, root_str, root_len * sizeof(WCHAR) );
    path[root_len] = '\\';
    memcpy( path + root_len + 1, exe_str, exe_len * sizeof(WCHAR) );
    path[root_len + 1 + exe_len] = 0;

    /* "path" arguments */
    if (!(cmdline = malloc( (root_len + exe_len + args_len + 5) * sizeof(WCHAR) ))) goto done;
    cmdline[0] = '"';
    wcscpy( cmdline + 1, path );
    wcscat( cmdline, L"\"" );
    if (args_len)
    {
        wcscat( cmdline, L" " );
        wcscat( cmdline, arguments );
    }

    memset( &si, 0, sizeof(si) );
    si.cb = sizeof(si);
    /* The server refuses the process unless the image it maps is the executable of the catalog record, so a
     * failure here leaves no process behind. */
    {
        WCHAR *full_name = malloc( info->full_name_len + sizeof(WCHAR) );

        if (full_name)
        {
            memcpy( full_name, info->data, info->full_name_len );
            full_name[info->full_name_len / sizeof(WCHAR)] = 0;
            environment = build_environment( full_name );
            free( full_name );
        }
    }
    if (!CreateProcessAsUserW( token, path, cmdline, NULL, NULL, FALSE, environment ? CREATE_UNICODE_ENVIRONMENT : 0,
                               environment, root, &si, &pi ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        WARN( "Failed to start %s, error %lu.\n", debugstr_w( path ), GetLastError() );
        goto done;
    }
    *pid = pi.dwProcessId;
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    hr = S_OK;

done:
    free( environment );
    free( cmdline );
    free( path );
    free( root );
    free( info );
    if (token) CloseHandle( token );
    CloseHandle( process_token );
    return hr;
}

static HRESULT WINAPI manager_ActivateForFile( IApplicationActivationManager *iface, const WCHAR *aumid,
                                               IShellItemArray *items, const WCHAR *verb, DWORD *pid )
{
    FIXME( "iface %p, aumid %s, items %p, verb %s, pid %p stub!\n", iface, debugstr_w( aumid ), items,
           debugstr_w( verb ), pid );
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_ActivateForProtocol( IApplicationActivationManager *iface, const WCHAR *aumid,
                                                   IShellItemArray *items, DWORD *pid )
{
    FIXME( "iface %p, aumid %s, items %p, pid %p stub!\n", iface, debugstr_w( aumid ), items, pid );
    return E_NOTIMPL;
}

static const IApplicationActivationManagerVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    manager_ActivateApplication,
    manager_ActivateForFile,
    manager_ActivateForProtocol,
};

/* Marshalling: the client gets an in-process manager of its own, there is no state to carry. */

static HRESULT WINAPI marshal_QueryInterface( IMarshal *iface, REFIID iid, void **out )
{
    struct manager *manager = impl_from_IMarshal( iface );
    return IApplicationActivationManager_QueryInterface( &manager->IApplicationActivationManager_iface, iid, out );
}

static ULONG WINAPI marshal_AddRef( IMarshal *iface )
{
    struct manager *manager = impl_from_IMarshal( iface );
    return IApplicationActivationManager_AddRef( &manager->IApplicationActivationManager_iface );
}

static ULONG WINAPI marshal_Release( IMarshal *iface )
{
    struct manager *manager = impl_from_IMarshal( iface );
    return IApplicationActivationManager_Release( &manager->IApplicationActivationManager_iface );
}

static HRESULT WINAPI marshal_GetUnmarshalClass( IMarshal *iface, REFIID iid, void *object, DWORD context,
                                                 void *reserved, DWORD flags, CLSID *clsid )
{
    *clsid = CLSID_ApplicationActivationManager;
    return S_OK;
}

static HRESULT WINAPI marshal_GetMarshalSizeMax( IMarshal *iface, REFIID iid, void *object, DWORD context,
                                                 void *reserved, DWORD flags, DWORD *size )
{
    *size = 0;
    return S_OK;
}

static HRESULT WINAPI marshal_MarshalInterface( IMarshal *iface, IStream *stream, REFIID iid, void *object,
                                                DWORD context, void *reserved, DWORD flags )
{
    return S_OK;
}

static HRESULT WINAPI marshal_UnmarshalInterface( IMarshal *iface, IStream *stream, REFIID iid, void **out )
{
    struct manager *manager = impl_from_IMarshal( iface );
    return IApplicationActivationManager_QueryInterface( &manager->IApplicationActivationManager_iface, iid, out );
}

static HRESULT WINAPI marshal_ReleaseMarshalData( IMarshal *iface, IStream *stream )
{
    return S_OK;
}

static HRESULT WINAPI marshal_DisconnectObject( IMarshal *iface, DWORD reserved )
{
    return S_OK;
}

static const IMarshalVtbl marshal_vtbl =
{
    marshal_QueryInterface,
    marshal_AddRef,
    marshal_Release,
    marshal_GetUnmarshalClass,
    marshal_GetMarshalSizeMax,
    marshal_MarshalInterface,
    marshal_UnmarshalInterface,
    marshal_ReleaseMarshalData,
    marshal_DisconnectObject,
};

static HRESULT WINAPI factory_QueryInterface( IClassFactory *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IClassFactory ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IClassFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IClassFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID iid, void **out )
{
    struct manager *manager;
    HRESULT hr;

    TRACE( "iface %p, outer %p, iid %s, out %p.\n", iface, outer, debugstr_guid( iid ), out );

    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(manager = calloc( 1, sizeof(*manager) ))) return E_OUTOFMEMORY;
    manager->IApplicationActivationManager_iface.lpVtbl = &manager_vtbl;
    manager->IMarshal_iface.lpVtbl = &marshal_vtbl;
    manager->refcount = 1;

    hr = IApplicationActivationManager_QueryInterface( &manager->IApplicationActivationManager_iface, iid, out );
    IApplicationActivationManager_Release( &manager->IApplicationActivationManager_iface );
    return hr;
}

static HRESULT WINAPI factory_LockServer( IClassFactory *iface, BOOL lock )
{
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_CreateInstance,
    factory_LockServer,
};

static IClassFactory activation_manager_factory = { &factory_vtbl };

HRESULT activation_manager_get_class_object( REFCLSID clsid, REFIID iid, void **out )
{
    if (!IsEqualGUID( clsid, &CLSID_ApplicationActivationManager )) return CLASS_E_CLASSNOTAVAILABLE;
    return IClassFactory_QueryInterface( &activation_manager_factory, iid, out );
}
