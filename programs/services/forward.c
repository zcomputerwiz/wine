/*
 * Services: calls for session 0's service manager (WineExplorer system tier T5.4)
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
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

/* A session's services.exe keeps the services that every session has of its own (drivers, PlugPlay, RpcSs, ...)
 * and sends the calls for every other service to session 0's services.exe, through \pipe\WexSession0\svcctl,
 * which the session's server bridges to session 0 (T5.2). Each connection is made while impersonating the
 * caller, so session 0 sees the caller's own token, and is the caller's alone: every binding has network options
 * of its own, and rpcrt4 shares a connection only within one association (same options). The client stubs are
 * generated with the prefix s0_ (svcctl_s0.idl), next to the server's. */

#define WIN32_LEAN_AND_MEAN
#include <stdarg.h>
#include <windows.h>
#include <winsvc.h>
#include <rpc.h>
#include "wine/debug.h"
#include "wine/exception.h"
#include "svcctl_s0.h"
#include "forward.h"

WINE_DEFAULT_DEBUG_CHANNEL(service);

static const WCHAR s0_pipe[] = L"\\\\.\\pipe\\WexSession0\\svcctl";
static LONG next_binding;

static handle_t s0_bind(void)
{
    WCHAR options[32];
    RPC_WSTR binding_str;
    handle_t handle = NULL;

    /* options of its own: this binding's association, and so its connection, is not shared with another caller */
    swprintf( options, ARRAY_SIZE(options), L"WexCaller=%ld", InterlockedIncrement( &next_binding ));
    if (RpcStringBindingComposeW( NULL, (RPC_WSTR)L"ncacn_np", NULL, (RPC_WSTR)L"\\pipe\\WexSession0\\svcctl",
                                  (RPC_WSTR)options, &binding_str ) != RPC_S_OK)
        return NULL;
    if (RpcBindingFromStringBindingW( binding_str, &handle ) != RPC_S_OK) handle = NULL;
    RpcStringFreeW( &binding_str );
    return handle;
}

handle_t __RPC_USER MACHINE_HANDLEW_bind( MACHINE_HANDLEW name )
{
    return s0_bind();
}

void __RPC_USER MACHINE_HANDLEW_unbind( MACHINE_HANDLEW name, handle_t h )
{
    RpcBindingFree( &h );
}

handle_t __RPC_USER SVCCTL_HANDLEW_bind( SVCCTL_HANDLEW name )
{
    return s0_bind();
}

void __RPC_USER SVCCTL_HANDLEW_unbind( SVCCTL_HANDLEW name, handle_t h )
{
    RpcBindingFree( &h );
}

handle_t __RPC_USER MACHINE_HANDLEA_bind( MACHINE_HANDLEA name )
{
    return NULL;  /* the ANSI calls are not forwarded */
}

void __RPC_USER MACHINE_HANDLEA_unbind( MACHINE_HANDLEA name, handle_t h )
{
}

static LONG WINAPI rpc_filter( EXCEPTION_POINTERS *eptr )
{
    return I_RpcExceptionFilter( eptr->ExceptionRecord->ExceptionCode );
}

static DWORD map_exception_code( DWORD code )
{
    switch (code)
    {
    case RPC_X_NULL_REF_POINTER:
        return ERROR_INVALID_ADDRESS;
    case RPC_X_ENUM_VALUE_OUT_OF_RANGE:
    case RPC_X_BYTE_COUNT_TOO_SMALL:
        return ERROR_INVALID_PARAMETER;
    case RPC_S_INVALID_BINDING:
    case RPC_X_SS_IN_NULL_CONTEXT:
        return ERROR_INVALID_HANDLE;
    default:
        return code;
    }
}

#define FORWARD( call ) \
    do { \
        DWORD err; \
        __TRY { err = call; } \
        __EXCEPT( rpc_filter ) { err = map_exception_code( GetExceptionCode() ); } \
        __ENDTRY \
        return err; \
    } while (0)

/* session 0 is there: its service manager's pipe opens */
BOOL forward_available(void)
{
    HANDLE pipe = CreateFileW( s0_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );

    if (pipe == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle( pipe );
    return TRUE;
}

/* the connection for a manager handle is made here, as the RPC caller */
DWORD forward_open_manager( DWORD access, SC_RPC_HANDLE *remote )
{
    DWORD err;

    *remote = NULL;
    if (RpcImpersonateClient( NULL ) != RPC_S_OK) return ERROR_ACCESS_DENIED;
    __TRY
    {
        err = s0_svcctl_OpenSCManagerW( NULL, NULL, access, remote );
    }
    __EXCEPT( rpc_filter )
    {
        err = map_exception_code( GetExceptionCode() );
    }
    __ENDTRY
    RpcRevertToSelf();
    if (err) WINE_TRACE( "session 0's service manager: %lu\n", err );
    return err;
}

DWORD forward_close( SC_RPC_HANDLE *remote )
{
    if (!*remote) return ERROR_SUCCESS;
    FORWARD( s0_svcctl_CloseServiceHandle( remote ) );
}

DWORD forward_OpenServiceW( SC_RPC_HANDLE manager, LPCWSTR name, DWORD access, SC_RPC_HANDLE *service )
{
    FORWARD( s0_svcctl_OpenServiceW( manager, name, access, service ) );
}

DWORD forward_CreateServiceW( SC_RPC_HANDLE manager, LPCWSTR name, LPCWSTR display, DWORD access, DWORD type,
                              DWORD start, DWORD error, LPCWSTR path, LPCWSTR group, DWORD *tag,
                              const BYTE *depends, DWORD depends_size, LPCWSTR start_name, const BYTE *password,
                              DWORD password_size, BOOL wow64, SC_RPC_HANDLE *service )
{
    if (wow64)
        FORWARD( s0_svcctl_CreateServiceWOW64W( manager, name, display, access, type, start, error, path, group,
                                                tag, depends, depends_size, start_name, password, password_size,
                                                service ) );
    FORWARD( s0_svcctl_CreateServiceW( manager, name, display, access, type, start, error, path, group, tag,
                                       depends, depends_size, start_name, password, password_size, service ) );
}

DWORD forward_GetServiceDisplayNameW( SC_RPC_HANDLE manager, LPCWSTR name, WCHAR *buffer, DWORD *size )
{
    FORWARD( s0_svcctl_GetServiceDisplayNameW( manager, name, buffer, size ) );
}

DWORD forward_GetServiceKeyNameW( SC_RPC_HANDLE manager, LPCWSTR display, WCHAR *buffer, DWORD *size )
{
    FORWARD( s0_svcctl_GetServiceKeyNameW( manager, display, buffer, size ) );
}

DWORD forward_EnumServicesStatusW( SC_RPC_HANDLE manager, DWORD type, DWORD state, BYTE *buffer, DWORD size,
                                   DWORD *needed, DWORD *returned )
{
    FORWARD( s0_svcctl_EnumServicesStatusW( manager, type, state, buffer, size, needed, returned, NULL ) );
}

DWORD forward_EnumServicesStatusExW( SC_RPC_HANDLE manager, DWORD type, DWORD state, BYTE *buffer, DWORD size,
                                     DWORD *needed, DWORD *returned, LPCWSTR group )
{
    FORWARD( s0_svcctl_EnumServicesStatusExW( manager, SC_ENUM_PROCESS_INFO, type, state, buffer, size, needed,
                                              returned, NULL, group ) );
}

DWORD forward_DeleteService( SC_RPC_HANDLE service )
{
    FORWARD( s0_svcctl_DeleteService( service ) );
}

DWORD forward_QueryServiceConfigW( SC_RPC_HANDLE service, QUERY_SERVICE_CONFIGW *config, DWORD size, DWORD *needed )
{
    FORWARD( s0_svcctl_QueryServiceConfigW( service, config, size, needed ) );
}

DWORD forward_ChangeServiceConfigW( SC_RPC_HANDLE service, DWORD type, DWORD start, DWORD error, LPCWSTR path,
                                    LPCWSTR group, DWORD *tag, const BYTE *depends, DWORD depends_size,
                                    LPCWSTR start_name, const BYTE *password, DWORD password_size,
                                    LPCWSTR display )
{
    FORWARD( s0_svcctl_ChangeServiceConfigW( service, type, start, error, path, group, tag, depends, depends_size,
                                             start_name, password, password_size, display ) );
}

DWORD forward_ChangeServiceConfig2W( SC_RPC_HANDLE service, SC_RPC_CONFIG_INFOW config )
{
    FORWARD( s0_svcctl_ChangeServiceConfig2W( service, config ) );
}

DWORD forward_QueryServiceConfig2W( SC_RPC_HANDLE service, DWORD level, BYTE *buffer, DWORD size, DWORD *needed )
{
    FORWARD( s0_svcctl_QueryServiceConfig2W( service, level, buffer, size, needed ) );
}

DWORD forward_QueryServiceConfigEx( SC_RPC_HANDLE service, DWORD level, SC_RPC_CONFIG_INFOW *info )
{
    FORWARD( s0_svcctl_QueryServiceConfigEx( service, level, info ) );
}

DWORD forward_QueryServiceStatusEx( SC_RPC_HANDLE service, SC_STATUS_TYPE level, BYTE *buffer, DWORD size,
                                    DWORD *needed )
{
    FORWARD( s0_svcctl_QueryServiceStatusEx( service, level, buffer, size, needed ) );
}

DWORD forward_QueryServiceStatus( SC_RPC_HANDLE service, SERVICE_STATUS *status )
{
    FORWARD( s0_svcctl_QueryServiceStatus( service, status ) );
}

DWORD forward_StartServiceW( SC_RPC_HANDLE service, DWORD argc, LPCWSTR *argv )
{
    FORWARD( s0_svcctl_StartServiceW( service, argc, argv ) );
}

DWORD forward_ControlService( SC_RPC_HANDLE service, DWORD control, SERVICE_STATUS *status )
{
    FORWARD( s0_svcctl_ControlService( service, control, status ) );
}

DWORD forward_ControlServiceExW( SC_RPC_HANDLE service, DWORD control, DWORD level,
                                 SC_RPC_SERVICE_CONTROL_IN_PARAMSW *in, SC_RPC_SERVICE_CONTROL_OUT_PARAMSW *out )
{
    FORWARD( s0_svcctl_ControlServiceExW( service, control, level, in, out ) );
}

DWORD forward_QueryServiceObjectSecurity( SC_RPC_HANDLE service, SECURITY_INFORMATION info, BYTE *descriptor,
                                          DWORD size, DWORD *needed )
{
    FORWARD( s0_svcctl_QueryServiceObjectSecurity( service, info, descriptor, size, needed ) );
}

DWORD forward_SetServiceObjectSecurity( SC_RPC_HANDLE service, SECURITY_INFORMATION info, BYTE *descriptor,
                                        DWORD size )
{
    FORWARD( s0_svcctl_SetServiceObjectSecurity( service, info, descriptor, size ) );
}

DWORD forward_EnumDependentServicesW( SC_RPC_HANDLE service, DWORD state, BYTE *buffer, DWORD size, DWORD *needed,
                                      DWORD *returned )
{
    FORWARD( s0_svcctl_EnumDependentServicesW( service, state, buffer, size, needed, returned ) );
}
