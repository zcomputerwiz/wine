/*
 * Win32_ComputerSystem methods: Rename, JoinDomainOrWorkgroup, UnjoinDomainOrWorkgroup
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
 * What Windows PowerShell 5.1's Add-Computer, Remove-Computer and Rename-Computer (and netdom's WMI path) call. Each
 * method answers the status of the netapi32 function behind it, as Windows' Workstation service does.
 */

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wbemcli.h"
#include "lm.h"

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

/* Wine's lm.h declares only NetGetJoinInformation; these are netapi32's */
NET_API_STATUS WINAPI NetJoinDomain( const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *, DWORD );
NET_API_STATUS WINAPI NetUnjoinDomain( const WCHAR *, const WCHAR *, const WCHAR *, DWORD );
NET_API_STATUS WINAPI NetRenameMachineInDomain( const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *, DWORD );

#define NETSETUP_JOIN_DOMAIN 0x00000001
#define NETSETUP_ACCT_CREATE 0x00000002
#define NETSETUP_ACCT_DELETE 0x00000004

/* a string in parameter, NULL when it is absent or null */
static WCHAR *get_string_param( IWbemClassObject *in, const WCHAR *name )
{
    VARIANT v;
    WCHAR *ret = NULL;

    if (!in) return NULL;
    VariantInit( &v );
    if (SUCCEEDED(IWbemClassObject_Get( in, name, 0, &v, NULL, NULL )) && V_VT( &v ) == VT_BSTR && V_BSTR( &v ))
        ret = wcsdup( V_BSTR( &v ));
    VariantClear( &v );
    return ret;
}

static UINT32 get_uint_param( IWbemClassObject *in, const WCHAR *name, UINT32 def )
{
    VARIANT v;
    UINT32 ret = def;

    if (!in) return def;
    VariantInit( &v );
    if (SUCCEEDED(IWbemClassObject_Get( in, name, 0, &v, NULL, NULL )) && (V_VT( &v ) == VT_UI4 || V_VT( &v ) == VT_I4)) ret = V_UI4( &v );
    VariantClear( &v );
    return ret;
}

static HRESULT return_value( const WCHAR *method, UINT32 status, IWbemClassObject **out )
{
    IWbemClassObject *sig, *params;
    VARIANT v;
    HRESULT hr;

    if (!out) return S_OK;
    *out = NULL;
    if ((hr = create_signature( WBEMPROX_NAMESPACE_CIMV2, L"Win32_ComputerSystem", method, PARAM_OUT, &sig ))) return hr;
    hr = IWbemClassObject_SpawnInstance( sig, 0, &params );
    IWbemClassObject_Release( sig );
    if (hr) return hr;
    V_VT( &v ) = VT_UI4;
    V_UI4( &v ) = status;
    hr = IWbemClassObject_Put( params, L"ReturnValue", 0, &v, CIM_UINT32 );
    if (hr == S_OK) *out = params;
    else IWbemClassObject_Release( params );
    return hr;
}

static void free_credentials( WCHAR *a, WCHAR *b, WCHAR *c )
{
    if (b) { SecureZeroMemory( b, lstrlenW( b ) * sizeof(WCHAR) ); }
    free( a );
    free( b );
    free( c );
}

/* Rename( Name, Password, UserName ) */
HRESULT compsys_rename( IWbemClassObject *obj, IWbemContext *context, IWbemClassObject *in, IWbemClassObject **out )
{
    WCHAR *name = get_string_param( in, L"Name" ), *password = get_string_param( in, L"Password" );
    WCHAR *user = get_string_param( in, L"UserName" ), *member = NULL;
    NETSETUP_JOIN_STATUS type = NetSetupUnknownStatus;
    UINT32 status;

    TRACE( "%p, %p, %p, %p\n", obj, context, in, out );

    if (!name) status = ERROR_INVALID_PARAMETER;
    else if (NetGetJoinInformation( NULL, &member, &type ) == NERR_Success && type == NetSetupDomainName)
        status = NetRenameMachineInDomain( NULL, name, user, password, NETSETUP_ACCT_CREATE );
    else
        status = SetComputerNameExW( ComputerNamePhysicalDnsHostname, name ) ? 0 : GetLastError();
    if (member) NetApiBufferFree( member );
    free_credentials( name, password, user );
    return return_value( L"Rename", status, out );
}

/* JoinDomainOrWorkgroup( Name, Password, UserName, AccountOU, FJoinOptions ) */
HRESULT compsys_join_domain_or_workgroup( IWbemClassObject *obj, IWbemContext *context, IWbemClassObject *in,
                                          IWbemClassObject **out )
{
    WCHAR *name = get_string_param( in, L"Name" ), *password = get_string_param( in, L"Password" );
    WCHAR *user = get_string_param( in, L"UserName" ), *ou = get_string_param( in, L"AccountOU" );
    UINT32 options = get_uint_param( in, L"FJoinOptions", 0 ), status;

    TRACE( "%p, %p, %p, %p\n", obj, context, in, out );

    if (!name) status = ERROR_INVALID_PARAMETER;
    else status = NetJoinDomain( NULL, name, ou, user, password, options );
    free( ou );
    free_credentials( name, password, user );
    return return_value( L"JoinDomainOrWorkgroup", status, out );
}

/* UnjoinDomainOrWorkgroup( Password, UserName, FUnjoinOptions ) */
HRESULT compsys_unjoin_domain_or_workgroup( IWbemClassObject *obj, IWbemContext *context, IWbemClassObject *in,
                                            IWbemClassObject **out )
{
    WCHAR *password = get_string_param( in, L"Password" ), *user = get_string_param( in, L"UserName" );
    UINT32 options = get_uint_param( in, L"FUnjoinOptions", 0 ), status;

    TRACE( "%p, %p, %p, %p\n", obj, context, in, out );

    status = NetUnjoinDomain( NULL, user, password, options );
    free_credentials( NULL, password, user );
    return return_value( L"UnjoinDomainOrWorkgroup", status, out );
}
