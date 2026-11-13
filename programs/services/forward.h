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

/* Included after svcctl.h (rpc.c) or svcctl_s0.h (forward.c), which define the types. The handles are session
 * 0's (client context handles). */

#ifndef __WINE_PROGRAMS_SERVICES_FORWARD_H
#define __WINE_PROGRAMS_SERVICES_FORWARD_H

extern BOOL forward_available(void);
extern DWORD forward_open_manager( DWORD access, SC_RPC_HANDLE *remote );
extern DWORD forward_close( SC_RPC_HANDLE *remote );
extern DWORD forward_OpenServiceW( SC_RPC_HANDLE manager, LPCWSTR name, DWORD access, SC_RPC_HANDLE *service );
extern DWORD forward_CreateServiceW( SC_RPC_HANDLE manager, LPCWSTR name, LPCWSTR display, DWORD access,
                                     DWORD type, DWORD start, DWORD error, LPCWSTR path, LPCWSTR group, DWORD *tag,
                                     const BYTE *depends, DWORD depends_size, LPCWSTR start_name,
                                     const BYTE *password, DWORD password_size, BOOL wow64,
                                     SC_RPC_HANDLE *service );
extern DWORD forward_GetServiceDisplayNameW( SC_RPC_HANDLE manager, LPCWSTR name, WCHAR *buffer, DWORD *size );
extern DWORD forward_GetServiceKeyNameW( SC_RPC_HANDLE manager, LPCWSTR display, WCHAR *buffer, DWORD *size );
extern DWORD forward_EnumServicesStatusW( SC_RPC_HANDLE manager, DWORD type, DWORD state, BYTE *buffer,
                                          DWORD size, DWORD *needed, DWORD *returned );
extern DWORD forward_EnumServicesStatusExW( SC_RPC_HANDLE manager, DWORD type, DWORD state, BYTE *buffer,
                                            DWORD size, DWORD *needed, DWORD *returned, LPCWSTR group );
extern DWORD forward_DeleteService( SC_RPC_HANDLE service );
extern DWORD forward_QueryServiceConfigW( SC_RPC_HANDLE service, QUERY_SERVICE_CONFIGW *config, DWORD size,
                                          DWORD *needed );
extern DWORD forward_ChangeServiceConfigW( SC_RPC_HANDLE service, DWORD type, DWORD start, DWORD error,
                                           LPCWSTR path, LPCWSTR group, DWORD *tag, const BYTE *depends,
                                           DWORD depends_size, LPCWSTR start_name, const BYTE *password,
                                           DWORD password_size, LPCWSTR display );
extern DWORD forward_ChangeServiceConfig2W( SC_RPC_HANDLE service, SC_RPC_CONFIG_INFOW config );
extern DWORD forward_QueryServiceConfig2W( SC_RPC_HANDLE service, DWORD level, BYTE *buffer, DWORD size,
                                           DWORD *needed );
extern DWORD forward_QueryServiceConfigEx( SC_RPC_HANDLE service, DWORD level, SC_RPC_CONFIG_INFOW *info );
extern DWORD forward_QueryServiceStatusEx( SC_RPC_HANDLE service, SC_STATUS_TYPE level, BYTE *buffer, DWORD size,
                                           DWORD *needed );
extern DWORD forward_QueryServiceStatus( SC_RPC_HANDLE service, SERVICE_STATUS *status );
extern DWORD forward_StartServiceW( SC_RPC_HANDLE service, DWORD argc, LPCWSTR *argv );
extern DWORD forward_ControlService( SC_RPC_HANDLE service, DWORD control, SERVICE_STATUS *status );
extern DWORD forward_ControlServiceExW( SC_RPC_HANDLE service, DWORD control, DWORD level,
                                        SC_RPC_SERVICE_CONTROL_IN_PARAMSW *in,
                                        SC_RPC_SERVICE_CONTROL_OUT_PARAMSW *out );
extern DWORD forward_QueryServiceObjectSecurity( SC_RPC_HANDLE service, SECURITY_INFORMATION info,
                                                 BYTE *descriptor, DWORD size, DWORD *needed );
extern DWORD forward_SetServiceObjectSecurity( SC_RPC_HANDLE service, SECURITY_INFORMATION info,
                                               BYTE *descriptor, DWORD size );
extern DWORD forward_EnumDependentServicesW( SC_RPC_HANDLE service, DWORD state, BYTE *buffer, DWORD size,
                                             DWORD *needed, DWORD *returned );

#endif
