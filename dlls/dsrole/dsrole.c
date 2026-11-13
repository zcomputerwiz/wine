/*
 * Copyright 2001 Mike McCormack
 * Copyright 2002 Andriy Palamarchuk
 * Copyright 2003 Juan Lang
 * Copyright 2005,2006 Paul Vriens
 * Copyright 2006 Robert Reif
 * Copyright 2013 Hans Leidekker for CodeWeavers
 * Copyright 2020 Dmitry Timoshkov
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

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "ntsecapi.h"

#include "dsrole.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dsrole);

/************************************************************
 *  DsRoleFreeMemory (dsrole.@)
 */
void WINAPI DsRoleFreeMemory(void *buffer)
{
    TRACE("(%p)\n", buffer);
    HeapFree(GetProcessHeap(), 0, buffer);
}

/************************************************************
 *  DsRoleGetPrimaryDomainInformation (dsrole.@)
 */
DWORD WINAPI DsRoleGetPrimaryDomainInformation(
    LPCWSTR lpServer, DSROLE_PRIMARY_DOMAIN_INFO_LEVEL InfoLevel,
    PBYTE* Buffer)
{
    DWORD ret;

    FIXME("(%p, %d, %p) stub\n", lpServer, InfoLevel, Buffer);

    /* Check some input parameters */

    if (!Buffer) return ERROR_INVALID_PARAMETER;
    if ((InfoLevel < DsRolePrimaryDomainInfoBasic) || (InfoLevel > DsRoleOperationState)) return ERROR_INVALID_PARAMETER;

    *Buffer = NULL;
    switch (InfoLevel)
    {
        case DsRolePrimaryDomainInfoBasic:
        {
            LSA_OBJECT_ATTRIBUTES ObjectAttributes;
            LSA_HANDLE PolicyHandle;
            PPOLICY_ACCOUNT_DOMAIN_INFO DomainInfo;
            NTSTATUS NtStatus;
            int logon_domain_sz;
            DWORD size;
            PDSROLE_PRIMARY_DOMAIN_INFO_BASIC basic;
            WCHAR flat[64], dns[256];
            DWORD flat_size = sizeof(flat), dns_size = sizeof(dns);
            HKEY key;

            /* WineExplorer: the host's domain join, written to the machine registry by wex-domain-state
             * (docs/GROUP_POLICY.md): a member workstation with the domain's flat and DNS names */
            flat[0] = dns[0] = 0;
            if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\WineExplorer\\Domain", 0,
                               KEY_READ | KEY_WOW64_64KEY, &key))
            {
                if (RegGetValueW(key, NULL, L"FlatName", RRF_RT_REG_SZ, NULL, flat, &flat_size)) flat[0] = 0;
                if (RegGetValueW(key, NULL, L"Name", RRF_RT_REG_SZ, NULL, dns, &dns_size)) dns[0] = 0;
                RegCloseKey(key);
            }
            if (flat[0])
            {
                flat_size = (lstrlenW(flat) + 1) * sizeof(WCHAR);
                dns_size = (lstrlenW(dns) + 1) * sizeof(WCHAR);
                basic = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*basic) + flat_size + 2 * dns_size);
                if (!basic) return ERROR_OUTOFMEMORY;
                basic->MachineRole = DsRole_RoleMemberWorkstation;
                basic->DomainNameFlat = (WCHAR *)(basic + 1);
                lstrcpyW(basic->DomainNameFlat, flat);
                if (dns[0])
                {
                    basic->DomainNameDns = (WCHAR *)((BYTE *)(basic + 1) + flat_size);
                    lstrcpyW(basic->DomainNameDns, dns);
                    basic->DomainForestName = (WCHAR *)((BYTE *)basic->DomainNameDns + dns_size);
                    lstrcpyW(basic->DomainForestName, dns);
                }
                *Buffer = (PBYTE)basic;
                return ERROR_SUCCESS;
            }

            ZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));
            NtStatus = LsaOpenPolicy(NULL, &ObjectAttributes,
             POLICY_VIEW_LOCAL_INFORMATION, &PolicyHandle);
            if (NtStatus != STATUS_SUCCESS)
            {
                TRACE("LsaOpenPolicyFailed with NT status %lx\n",
                    LsaNtStatusToWinError(NtStatus));
                return ERROR_OUTOFMEMORY;
            }
            LsaQueryInformationPolicy(PolicyHandle,
             PolicyAccountDomainInformation, (PVOID*)&DomainInfo);
            logon_domain_sz = lstrlenW(DomainInfo->DomainName.Buffer) + 1;
            LsaClose(PolicyHandle);

            size = sizeof(DSROLE_PRIMARY_DOMAIN_INFO_BASIC) +
             logon_domain_sz * sizeof(WCHAR);
            basic = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
            if (basic)
            {
                basic->MachineRole = DsRole_RoleStandaloneWorkstation;
                basic->DomainNameFlat = (LPWSTR)((LPBYTE)basic +
                 sizeof(DSROLE_PRIMARY_DOMAIN_INFO_BASIC));
                lstrcpyW(basic->DomainNameFlat, DomainInfo->DomainName.Buffer);
                ret = ERROR_SUCCESS;
            }
            else
                ret = ERROR_OUTOFMEMORY;
            *Buffer = (PBYTE)basic;
            LsaFreeMemory(DomainInfo);
        }
        break;
    default:
        ret = ERROR_CALL_NOT_IMPLEMENTED;
    }
    return ret;
}
