/* Installing a package for IPackageManager: AddPackageAsync, StagePackageAsync and their variants
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
#include "shlwapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

/*
 * The staging of a package is done by wexappx (the package installer of WineExplorer): it checks the signature and the block map,
 * extracts the package and records it in the catalog of the prefix. Wine has no separate "staged but not registered" state, so a
 * staged package is installed for the user; the difference with Windows is that FindPackages sees it from then on. Only packages
 * a file Uri names are handled (a download is not done); the dependencies the caller lists are not installed, wexappx resolves
 * them from the packages it already has. DevelopmentMode and the DeveloperMode and AllowUnsigned options of StagePackageOptions
 * become wexappx's --development, which installs a package whose signature is not trusted.
 */

struct deployment_result
{
    IDeploymentResult IDeploymentResult_iface;
    LONG ref;
    HSTRING error_text;
    HRESULT extended_error;
    GUID activity_id;
};

static inline struct deployment_result *impl_from_IDeploymentResult( IDeploymentResult *iface )
{
    return CONTAINING_RECORD( iface, struct deployment_result, IDeploymentResult_iface );
}

static HRESULT WINAPI result_QueryInterface( IDeploymentResult *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDeploymentResult ))
    {
        *out = iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI result_AddRef( IDeploymentResult *iface )
{
    return InterlockedIncrement( &impl_from_IDeploymentResult( iface )->ref );
}

static ULONG WINAPI result_Release( IDeploymentResult *iface )
{
    struct deployment_result *impl = impl_from_IDeploymentResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->error_text );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI result_GetIids( IDeploymentResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI result_GetRuntimeClassName( IDeploymentResult *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Management_Deployment_DeploymentResult,
                                wcslen( RuntimeClass_Windows_Management_Deployment_DeploymentResult ), class_name );
}

static HRESULT WINAPI result_GetTrustLevel( IDeploymentResult *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI result_get_ErrorText( IDeploymentResult *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IDeploymentResult( iface )->error_text, value );
}

static HRESULT WINAPI result_get_ActivityId( IDeploymentResult *iface, GUID *value )
{
    *value = impl_from_IDeploymentResult( iface )->activity_id;
    return S_OK;
}

static HRESULT WINAPI result_get_ExtendedErrorCode( IDeploymentResult *iface, HRESULT *value )
{
    *value = impl_from_IDeploymentResult( iface )->extended_error;
    return S_OK;
}

static const IDeploymentResultVtbl result_vtbl =
{
    result_QueryInterface,
    result_AddRef,
    result_Release,
    /* IInspectable methods */
    result_GetIids,
    result_GetRuntimeClassName,
    result_GetTrustLevel,
    /* IDeploymentResult methods */
    result_get_ErrorText,
    result_get_ActivityId,
    result_get_ExtendedErrorCode,
};

static HRESULT result_create( const WCHAR *text, HRESULT extended_error, IDeploymentResult **out )
{
    struct deployment_result *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDeploymentResult_iface.lpVtbl = &result_vtbl;
    impl->ref = 1;
    impl->extended_error = extended_error;
    CoCreateGuid( &impl->activity_id );
    if (FAILED(hr = WindowsCreateString( text, wcslen( text ), &impl->error_text )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IDeploymentResult_iface;
    return S_OK;
}

/* the job: what the thread pool thread needs, kept alive by the operation as its parameter */
struct install_job
{
    IUnknown IUnknown_iface;
    LONG ref;
    IUriRuntimeClass *uri;
    BOOL development;
    BOOL all_users;          /* provisioning: wexappx install --all-users, on the file the family name was staged from */
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
};

static inline struct install_job *impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct install_job, IUnknown_iface );
}

static HRESULT WINAPI job_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI job_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &impl_from_IUnknown( iface )->ref );
}

static ULONG WINAPI job_Release( IUnknown *iface )
{
    struct install_job *job = impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &job->ref );

    if (!ref)
    {
        if (job->uri) IUriRuntimeClass_Release( job->uri );
        free( job );
    }
    return ref;
}

static const IUnknownVtbl job_vtbl = { job_QueryInterface, job_AddRef, job_Release };

/* runs wexappx install on the file; the output is the error text */
static HRESULT run_wexappx( const WCHAR *path, BOOL development, BOOL all_users, DWORD *exit_code, WCHAR **text )
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE read = NULL, write = NULL;
    WCHAR *cmd;
    char buffer[2048];
    DWORD len = 0, got;
    size_t cmd_len = wcslen( path ) + 64;
    HRESULT hr = S_OK;

    *text = NULL;
    if (!(cmd = malloc( cmd_len * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    swprintf( cmd, cmd_len, L"wexappx.exe install%ls%ls \"%ls\"", development ? L" --development" : L"",
              all_users ? L" --all-users" : L"", path );

    if (!CreatePipe( &read, &write, &sa, 0 ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        free( cmd );
        return hr;
    }
    SetHandleInformation( read, HANDLE_FLAG_INHERIT, 0 );
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write;
    si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );

    TRACE( "running %s.\n", debugstr_w( cmd ) );
    if (!CreateProcessW( NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        CloseHandle( read );
        CloseHandle( write );
        free( cmd );
        return hr;
    }
    CloseHandle( write );
    while (len < sizeof(buffer) - 1 && ReadFile( read, buffer + len, sizeof(buffer) - 1 - len, &got, NULL ) && got) len += got;
    while (ReadFile( read, buffer, sizeof(buffer), &got, NULL ) && got) {}   /* drain: wexappx must not block on a full pipe */
    WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, exit_code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    CloseHandle( read );
    free( cmd );

    buffer[len] = 0;
    if ((*text = malloc( (len + 1) * sizeof(WCHAR) )))
        MultiByteToWideChar( CP_UTF8, 0, buffer, len + 1, *text, len + 1 );
    return hr;
}

/* The package file a family was installed from is remembered (HKCU\Software\WineExplorer\Appx\SourcePackages, by family name),
 * because provisioning names only the family: wexappx installs for every user from a package file, never from the files of
 * a user's own installation (the broker freezes the file the consent describes). */
static const WCHAR sources_key[] = L"Software\\WineExplorer\\Appx\\SourcePackages";

/* "Installed NAME_VERSION_ARCH_RESOURCE_PUBLISHERID" is the first line of wexappx's output: the family is NAME_PUBLISHERID */
static void remember_source( const WCHAR *text, const WCHAR *path )
{
    static const WCHAR prefix[] = L"Installed ";
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    const WCHAR *name, *first, *last, *end;
    HKEY key;

    if (!text || wcsncmp( text, prefix, wcslen( prefix ) )) return;
    name = text + wcslen( prefix );
    end = name + wcscspn( name, L"\r\n" );
    first = wcschr( name, '_' );
    for (last = end; last > name && *last != '_'; last--) ;
    if (!first || first >= end || last <= first || last >= end) return;
    if (swprintf( family, ARRAY_SIZE(family), L"%.*ls%.*ls", (int)(first - name), name, (int)(end - last), last ) < 0) return;
    if (RegCreateKeyExW( HKEY_CURRENT_USER, sources_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, family, 0, REG_SZ, (const BYTE *)path, (wcslen( path ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

static BOOL find_source( const WCHAR *family, WCHAR *path, DWORD len )
{
    DWORD size = len * sizeof(WCHAR), type;
    HKEY key;
    BOOL found;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, sources_key, 0, KEY_QUERY_VALUE, &key )) return FALSE;
    found = !RegQueryValueExW( key, family, NULL, &type, (BYTE *)path, &size ) && type == REG_SZ;
    RegCloseKey( key );
    return found && GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

static HRESULT install_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct install_job *job = impl_from_IUnknown( param );
    WCHAR path[MAX_PATH], *text = NULL;
    IDeploymentResult *deployment;
    HSTRING abs_uri = NULL;
    DWORD size = ARRAY_SIZE(path), exit_code = 0;
    HRESULT hr;

    if (!called_async) return STATUS_PENDING;

    if (job->family[0])
    {
        if (!find_source( job->family, path, ARRAY_SIZE(path) ))
        {
            FIXME( "no package file is known for family %s.\n", debugstr_w( job->family ) );
            hr = HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
            if (SUCCEEDED(result_create( L"the package file this family was installed from is not known (or is gone)", hr,
                                         &deployment )))
            {
                result->vt = VT_UNKNOWN;
                result->punkVal = (IUnknown *)deployment;
            }
            return hr;
        }
    }
    else
    {
        if (FAILED(hr = IUriRuntimeClass_get_AbsoluteUri( job->uri, &abs_uri ))) return hr;
        hr = PathCreateFromUrlW( WindowsGetStringRawBuffer( abs_uri, NULL ), path, &size, 0 );
        WindowsDeleteString( abs_uri );
        if (FAILED(hr))
        {
            FIXME( "only a file Uri can be installed.\n" );
            return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        }
    }

    if (FAILED(hr = run_wexappx( path, job->development, job->all_users, &exit_code, &text ))) return hr;
    TRACE( "wexappx exit code %lu, output %s.\n", exit_code, debugstr_w( text ) );

    if (!exit_code && !job->all_users) remember_source( text, path );
    /* provisioning a package that is already provisioned is not an error */
    if (exit_code && job->all_users && text && wcsstr( text, L"is already installed" )) exit_code = 0;
    hr = exit_code ? HRESULT_FROM_WIN32( ERROR_INSTALL_FAILURE ) : S_OK;
    if (SUCCEEDED(result_create( text ? text : L"", hr, &deployment )))
    {
        result->vt = VT_UNKNOWN;
        result->punkVal = (IUnknown *)deployment;
    }
    free( text );
    return hr;
}

HRESULT start_install( IUriRuntimeClass *uri, DeploymentOptions options, BOOL development, async_operation **out )
{
    struct install_job *job;
    HRESULT hr;

    *out = NULL;
    if (!uri) return E_INVALIDARG;
    if (!(job = calloc( 1, sizeof(*job) ))) return E_OUTOFMEMORY;
    job->IUnknown_iface.lpVtbl = &job_vtbl;
    job->ref = 1;
    IUriRuntimeClass_AddRef( (job->uri = uri) );
    job->development = development || (options & DeploymentOptions_DevelopmentMode);

    hr = async_deployment_create( NULL, &job->IUnknown_iface, install_callback, out );
    IUnknown_Release( &job->IUnknown_iface );
    return hr;
}

/* ProvisionPackageForAllUsersAsync: the package of that family, which was installed from a file, is installed for every user
 * (wexappx install --all-users, which asks an administrator's consent through the elevation broker) */
HRESULT start_provision( HSTRING family_name, async_operation **out )
{
    struct install_job *job;
    const WCHAR *family;
    UINT32 len;
    HRESULT hr;

    *out = NULL;
    family = WindowsGetStringRawBuffer( family_name, &len );
    if (!family || !len || len > PACKAGE_FAMILY_NAME_MAX_LENGTH) return E_INVALIDARG;
    if (!(job = calloc( 1, sizeof(*job) ))) return E_OUTOFMEMORY;
    job->IUnknown_iface.lpVtbl = &job_vtbl;
    job->ref = 1;
    job->all_users = TRUE;
    memcpy( job->family, family, len * sizeof(WCHAR) );

    hr = async_deployment_create( NULL, &job->IUnknown_iface, install_callback, out );
    IUnknown_Release( &job->IUnknown_iface );
    return hr;
}
