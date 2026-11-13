/*
 * Implementation of VERSION.DLL
 *
 * Copyright 1996,1997 Marcus Meissner
 * Copyright 1997 David Cuthbert
 * Copyright 1999 Ulrich Weigand
 * Copyright 2005 Paul Vriens
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
 *
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winver.h"
#include "winuser.h"
#include "winnls.h"
#include "winternl.h"
#include "winerror.h"
#include "errhandlingapi.h"
#include "appmodel.h"
#include "symcrypt.h"

#include "kernelbase.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ver);

typedef struct
{
    WORD offset;
    WORD length;
    WORD flags;
    WORD id;
    WORD handle;
    WORD usage;
} NE_NAMEINFO;

typedef struct
{
    WORD  type_id;
    WORD  count;
    DWORD resloader;
} NE_TYPEINFO;

struct version_info
{
    DWORD major;
    DWORD minor;
    DWORD build;
};

/***********************************************************************
 * Version Info Structure
 */

typedef struct
{
    WORD  wLength;
    WORD  wValueLength;
    CHAR  szKey[1];
#if 0   /* variable length structure */
    /* DWORD aligned */
    BYTE  Value[];
    /* DWORD aligned */
    VS_VERSION_INFO_STRUCT16 Children[];
#endif
} VS_VERSION_INFO_STRUCT16;

typedef struct
{
    WORD  wLength;
    WORD  wValueLength;
    WORD  wType; /* 1:Text, 0:Binary */
    WCHAR szKey[1];
#if 0   /* variable length structure */
    /* DWORD aligned */
    BYTE  Value[];
    /* DWORD aligned */
    VS_VERSION_INFO_STRUCT32 Children[];
#endif
} VS_VERSION_INFO_STRUCT32;

#define VersionInfoIs16( ver ) \
    ( ((const VS_VERSION_INFO_STRUCT16 *)ver)->szKey[0] >= ' ' )

#define DWORD_ALIGN( base, ptr ) \
    ( (LPBYTE)(base) + ((((LPBYTE)(ptr) - (LPBYTE)(base)) + 3) & ~3) )

#define VersionInfo16_Value( ver )  \
    DWORD_ALIGN( (ver), (ver)->szKey + strlen((ver)->szKey) + 1 )
#define VersionInfo32_Value( ver )  \
    DWORD_ALIGN( (ver), (ver)->szKey + lstrlenW((ver)->szKey) + 1 )

#define VersionInfo16_Children( ver )  \
    (const VS_VERSION_INFO_STRUCT16 *)( VersionInfo16_Value( ver ) + \
                           ( ( (ver)->wValueLength + 3 ) & ~3 ) )
#define VersionInfo32_Children( ver )  \
    (const VS_VERSION_INFO_STRUCT32 *)( VersionInfo32_Value( ver ) + \
                           ( ( (ver)->wValueLength * \
                               ((ver)->wType? 2 : 1) + 3 ) & ~3 ) )

#define VersionInfo16_Next( ver ) \
    (VS_VERSION_INFO_STRUCT16 *)( (LPBYTE)ver + (((ver)->wLength + 3) & ~3) )
#define VersionInfo32_Next( ver ) \
    (VS_VERSION_INFO_STRUCT32 *)( (LPBYTE)ver + (((ver)->wLength + 3) & ~3) )


/***********************************************************************
 * Win8 info, reported if the app doesn't provide compat GUID in the manifest and
 * doesn't have higher OS version in PE header.
 */
static const struct version_info windows8_version_info = { 6, 2, 9200 };

/***********************************************************************
 * Win8.1 info, reported if the app doesn't provide compat GUID in the manifest and
 * OS version in PE header is 8.1 or higher but below 10.
 */
static const struct version_info windows8_1_version_info = { 6, 3, 9600 };


/***********************************************************************
 * Windows versions that need compatibility GUID specified in manifest
 * in order to be reported by the APIs.
 */
static const struct
{
    struct version_info info;
    GUID guid;
} version_data[] =
{
    /* Windows 8.1 */
    {
        { 6, 3, 9600 },
        {0x1f676c76,0x80e1,0x4239,{0x95,0xbb,0x83,0xd0,0xf6,0xd0,0xda,0x78}}
    },
    /* Windows 10 */
    {
        { 10, 0, 19045 },
        {0x8e0f7a12,0xbfb3,0x4fe8,{0xb9,0xa5,0x48,0xfd,0x50,0xa1,0x5a,0x9a}}
    }
};


/******************************************************************************
 *  init_current_version
 *
 * Initialize the current_version variable.
 *
 * For compatibility, Windows 8.1 and later report Win8 version unless the app
 * has a manifest or higher OS version in the PE optional header
 * that confirms its compatibility with newer versions of Windows.
 *
 */
static RTL_OSVERSIONINFOEXW current_version;

static BOOL CALLBACK init_current_version(PINIT_ONCE init_once, PVOID parameter, PVOID *context)
{
    struct acci
    {
        DWORD ElementCount;
        COMPATIBILITY_CONTEXT_ELEMENT Elements[1];
    } *acci;
    BOOL have_os_compat_elements = FALSE;
    const struct version_info *ver;
    IMAGE_NT_HEADERS *nt;
    SIZE_T req;
    int idx;

    current_version.dwOSVersionInfoSize = sizeof(current_version);
    if (!set_ntstatus( RtlGetVersion(&current_version) )) return FALSE;

    for (idx = ARRAY_SIZE(version_data); idx--;)
        if ( current_version.dwMajorVersion >  version_data[idx].info.major ||
            (current_version.dwMajorVersion == version_data[idx].info.major &&
             current_version.dwMinorVersion >= version_data[idx].info.minor))
            break;

    if (idx < 0) return TRUE;
    ver = &windows8_version_info;

    if (RtlQueryInformationActivationContext(0, NtCurrentTeb()->Peb->ActivationContextData, NULL,
            CompatibilityInformationInActivationContext, NULL, 0, &req) != STATUS_BUFFER_TOO_SMALL
        || !req)
        goto done;

    if (!(acci = HeapAlloc(GetProcessHeap(), 0, req)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (RtlQueryInformationActivationContext(0, NtCurrentTeb()->Peb->ActivationContextData, NULL,
            CompatibilityInformationInActivationContext, acci, req, &req) == STATUS_SUCCESS)
    {
        do
        {
            DWORD i;

            for (i = 0; i < acci->ElementCount; i++)
            {
                if (acci->Elements[i].Type != ACTCTX_COMPATIBILITY_ELEMENT_TYPE_OS)
                    continue;

                have_os_compat_elements = TRUE;

                if (IsEqualGUID(&acci->Elements[i].Id, &version_data[idx].guid))
                {
                    ver = &version_data[idx].info;

                    if (ver->major == current_version.dwMajorVersion &&
                        ver->minor == current_version.dwMinorVersion)
                        ver = NULL;

                    idx = 0;  /* break from outer loop */
                    break;
                }
            }
        } while (idx--);
    }
    HeapFree(GetProcessHeap(), 0, acci);

done:
    if (!have_os_compat_elements && current_version.dwMajorVersion >= 10
            && (nt = RtlImageNtHeader(NtCurrentTeb()->Peb->ImageBaseAddress))
            && (nt->OptionalHeader.MajorOperatingSystemVersion > 6
            || (nt->OptionalHeader.MajorOperatingSystemVersion == 6
            && nt->OptionalHeader.MinorOperatingSystemVersion >= 3)))
    {
        if (current_version.dwMajorVersion > 10)
            FIXME("Unsupported current_version.dwMajorVersion %lu.\n", current_version.dwMajorVersion);

        ver = nt->OptionalHeader.MajorOperatingSystemVersion >= 10 ? NULL : &windows8_1_version_info;
    }

    if (ver)
    {
        current_version.dwMajorVersion = ver->major;
        current_version.dwMinorVersion = ver->minor;
        current_version.dwBuildNumber  = ver->build;
    }
    return TRUE;
}


/**********************************************************************
 *  find_entry_by_id
 *
 * Find an entry by id in a resource directory
 * Copied from loader/pe_resource.c
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_by_id( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                         WORD id, const void *root,
                                                         DWORD root_size )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;
    int min, max, pos;

    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    min = dir->NumberOfNamedEntries;
    max = min + dir->NumberOfIdEntries - 1;

    if (max >= (root_size - ((INT_PTR)dir - (INT_PTR)root) - sizeof(*dir)) / sizeof(*entry))
        return NULL;

    while (min <= max)
    {
        pos = (min + max) / 2;
        if (entry[pos].Id == id)
        {
            DWORD offset = entry[pos].OffsetToDirectory;
            if (offset > root_size - sizeof(*dir)) return NULL;
            return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + offset);
        }
        if (entry[pos].Id > id) max = pos - 1;
        else min = pos + 1;
    }
    return NULL;
}


/**********************************************************************
 *  find_entry_default
 *
 * Find a default entry in a resource directory
 * Copied from loader/pe_resource.c
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_default( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                           const void *root )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;

    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + entry->OffsetToDirectory);
}


/**********************************************************************
 *  push_language
 *
 * push a language onto the list of languages to try
 */
static inline int push_language( WORD *list, int pos, WORD lang )
{
    int i;
    for (i = 0; i < pos; i++) if (list[i] == lang) return pos;
    list[pos++] = lang;
    return pos;
}


/**********************************************************************
 *  find_entry_language
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_language( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                            const void *root, DWORD root_size,
                                                            DWORD flags )
{
    const IMAGE_RESOURCE_DIRECTORY *ret;
    WORD list[9];
    int i, pos = 0;

    if (flags & FILE_VER_GET_LOCALISED)
    {
        /* cf. LdrFindResource_U */
        pos = push_language( list, pos, MAKELANGID( LANG_NEUTRAL, SUBLANG_NEUTRAL ) );
        pos = push_language( list, pos, LANGIDFROMLCID( NtCurrentTeb()->CurrentLocale ) );
        pos = push_language( list, pos, GetUserDefaultLangID() );
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetUserDefaultLangID()), SUBLANG_NEUTRAL ));
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetUserDefaultLangID()), SUBLANG_DEFAULT ));
        pos = push_language( list, pos, GetSystemDefaultLangID() );
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetSystemDefaultLangID()), SUBLANG_NEUTRAL ));
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetSystemDefaultLangID()), SUBLANG_DEFAULT ));
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }
    else
    {
        /* FIXME: resolve LN file here */
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }

    for (i = 0; i < pos; i++) if ((ret = find_entry_by_id( dir, list[i], root, root_size ))) return ret;
    return find_entry_default( dir, root );
}


static DWORD read_data( HANDLE handle, DWORD offset, void *data, DWORD len )
{
    DWORD res;

    SetFilePointer( handle, offset, NULL, FILE_BEGIN );
    if (!ReadFile( handle, data, len, &res, NULL )) res = 0;
    return res;
}

/***********************************************************************
 *           find_ne_resource         [internal]
 */
static BOOL find_ne_resource( HANDLE handle, DWORD *resLen, DWORD *resOff )
{
    const WORD typeid = VS_FILE_INFO | 0x8000;
    const WORD resid = VS_VERSION_INFO | 0x8000;
    IMAGE_OS2_HEADER nehd;
    NE_TYPEINFO *typeInfo;
    NE_NAMEINFO *nameInfo;
    DWORD nehdoffset = *resOff;
    LPBYTE resTab;
    DWORD resTabSize;
    int count;

    /* Read in NE header */
    if (read_data( handle, nehdoffset, &nehd, sizeof(nehd) ) != sizeof(nehd)) return FALSE;

    resTabSize = nehd.ne_restab - nehd.ne_rsrctab;
    if ( !resTabSize )
    {
        TRACE("No resources in NE dll\n" );
        return FALSE;
    }

    /* Read in resource table */
    resTab = HeapAlloc( GetProcessHeap(), 0, resTabSize );
    if ( !resTab ) return FALSE;

    if (read_data( handle, nehd.ne_rsrctab + nehdoffset, resTab, resTabSize ) != resTabSize)
    {
        HeapFree( GetProcessHeap(), 0, resTab );
        return FALSE;
    }

    /* Find resource */
    typeInfo = (NE_TYPEINFO *)(resTab + 2);
    while (typeInfo->type_id)
    {
        if (typeInfo->type_id == typeid) goto found_type;
        typeInfo = (NE_TYPEINFO *)((char *)(typeInfo + 1) +
                                   typeInfo->count * sizeof(NE_NAMEINFO));
    }
    TRACE("No typeid entry found\n" );
    HeapFree( GetProcessHeap(), 0, resTab );
    return FALSE;

 found_type:
    nameInfo = (NE_NAMEINFO *)(typeInfo + 1);

    for (count = typeInfo->count; count > 0; count--, nameInfo++)
        if (nameInfo->id == resid) goto found_name;

    TRACE("No resid entry found\n" );
    HeapFree( GetProcessHeap(), 0, resTab );
    return FALSE;

 found_name:
    /* Return resource data */
    *resLen = nameInfo->length << *(WORD *)resTab;
    *resOff = nameInfo->offset << *(WORD *)resTab;

    HeapFree( GetProcessHeap(), 0, resTab );
    return TRUE;
}

/***********************************************************************
 *           find_pe_resource         [internal]
 */
static BOOL find_pe_resource( HANDLE handle, DWORD *resLen, DWORD *resOff, DWORD flags )
{
    union
    {
        IMAGE_NT_HEADERS32 nt32;
        IMAGE_NT_HEADERS64 nt64;
    } pehd;
    DWORD pehdoffset = *resOff;
    PIMAGE_DATA_DIRECTORY resDataDir;
    PIMAGE_SECTION_HEADER sections;
    LPBYTE resSection;
    DWORD len, section_size, data_size, resDirSize;
    const void *resDir;
    const IMAGE_RESOURCE_DIRECTORY *resPtr;
    const IMAGE_RESOURCE_DATA_ENTRY *resData;
    int i, nSections;
    BOOL ret = FALSE;

    /* Read in PE header */
    len = read_data( handle, pehdoffset, &pehd, sizeof(pehd) );
    if (len < sizeof(pehd.nt32.FileHeader)) return FALSE;
    if (len < sizeof(pehd)) memset( (char *)&pehd + len, 0, sizeof(pehd) - len );

    switch (pehd.nt32.OptionalHeader.Magic)
    {
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        resDataDir = pehd.nt32.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        resDataDir = pehd.nt64.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    default:
        return FALSE;
    }

    if ( !resDataDir->Size )
    {
        TRACE("No resources in PE dll\n" );
        return FALSE;
    }

    /* Read in section table */
    nSections = pehd.nt32.FileHeader.NumberOfSections;
    sections = HeapAlloc( GetProcessHeap(), 0,
                          nSections * sizeof(IMAGE_SECTION_HEADER) );
    if ( !sections ) return FALSE;

    len = FIELD_OFFSET( IMAGE_NT_HEADERS32, OptionalHeader ) + pehd.nt32.FileHeader.SizeOfOptionalHeader;
    if (read_data( handle, pehdoffset + len, sections, nSections * sizeof(IMAGE_SECTION_HEADER) ) !=
        nSections * sizeof(IMAGE_SECTION_HEADER))
    {
        HeapFree( GetProcessHeap(), 0, sections );
        return FALSE;
    }

    /* Find resource section */
    for ( i = 0; i < nSections; i++ )
        if (    resDataDir->VirtualAddress >= sections[i].VirtualAddress
             && resDataDir->VirtualAddress <  sections[i].VirtualAddress +
                                              sections[i].SizeOfRawData )
            break;

    if ( i == nSections )
    {
        HeapFree( GetProcessHeap(), 0, sections );
        TRACE("Couldn't find resource section\n" );
        return FALSE;
    }

    /* Read in resource section */
    data_size = sections[i].SizeOfRawData;
    section_size = max( data_size, sections[i].Misc.VirtualSize );
    resSection = HeapAlloc( GetProcessHeap(), 0, section_size );
    if ( !resSection )
    {
        HeapFree( GetProcessHeap(), 0, sections );
        return FALSE;
    }

    if (read_data( handle, sections[i].PointerToRawData, resSection, data_size ) != data_size) goto done;
    if (data_size < section_size) memset( (char *)resSection + data_size, 0, section_size - data_size );

    /* Find resource */
    resDir = resSection + (resDataDir->VirtualAddress - sections[i].VirtualAddress);
    resDirSize = section_size - (resDataDir->VirtualAddress - sections[i].VirtualAddress);

    resPtr = resDir;
    resPtr = find_entry_by_id( resPtr, VS_FILE_INFO, resDir, resDirSize );
    if ( !resPtr )
    {
        TRACE("No typeid entry found\n" );
        goto done;
    }
    resPtr = find_entry_by_id( resPtr, VS_VERSION_INFO, resDir, resDirSize );
    if ( !resPtr )
    {
        TRACE("No resid entry found\n" );
        goto done;
    }
    resPtr = find_entry_language( resPtr, resDir, resDirSize, flags );
    if ( !resPtr )
    {
        TRACE("No default language entry found\n" );
        goto done;
    }

    /* Find resource data section */
    resData = (const IMAGE_RESOURCE_DATA_ENTRY*)resPtr;
    for ( i = 0; i < nSections; i++ )
        if (    resData->OffsetToData >= sections[i].VirtualAddress
             && resData->OffsetToData <  sections[i].VirtualAddress +
                                         sections[i].SizeOfRawData )
            break;

    if ( i == nSections )
    {
        TRACE("Couldn't find resource data section\n" );
        goto done;
    }

    /* Return resource data */
    *resLen = resData->Size;
    *resOff = resData->OffsetToData - sections[i].VirtualAddress + sections[i].PointerToRawData;
    ret = TRUE;

 done:
    HeapFree( GetProcessHeap(), 0, resSection );
    HeapFree( GetProcessHeap(), 0, sections );
    return ret;
}


/***********************************************************************
 *           find_version_resource         [internal]
 */
static DWORD find_version_resource( HANDLE handle, DWORD *reslen, DWORD *offset, DWORD flags )
{
    IMAGE_DOS_HEADER mzh;
    WORD magic;

    if (read_data( handle, 0, &mzh, sizeof(mzh) ) != sizeof(mzh)) return 0;
    if (mzh.e_magic != IMAGE_DOS_SIGNATURE) return 0;

    if (read_data( handle, mzh.e_lfanew, &magic, sizeof(magic) ) != sizeof(magic)) return 0;
    *offset = mzh.e_lfanew;

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        if (!find_ne_resource( handle, reslen, offset )) magic = 0;
        break;
    case IMAGE_NT_SIGNATURE:
        if (!find_pe_resource( handle, reslen, offset, flags )) magic = 0;
        break;
    }
    WARN( "Can't handle %04x files.\n", magic );
    return magic;
}

/******************************************************************************
 *   This function will print via standard TRACE, debug info regarding
 *   the file info structure vffi.
 */
static void print_vffi_debug(const VS_FIXEDFILEINFO *vffi)
{
    BOOL    versioned_printer = FALSE;

    if((vffi->dwFileType == VFT_DLL) || (vffi->dwFileType == VFT_DRV))
    {
        if(vffi->dwFileSubtype == VFT2_DRV_VERSIONED_PRINTER)
            /* this is documented for newer w2k Drivers and up */
            versioned_printer = TRUE;
        else if( (vffi->dwFileSubtype == VFT2_DRV_PRINTER) &&
                 (vffi->dwFileVersionMS != vffi->dwProductVersionMS) &&
                 (vffi->dwFileVersionMS > 0) &&
                 (vffi->dwFileVersionMS <= 3) )
            /* found this on NT 3.51, NT4.0 and old w2k Drivers */
            versioned_printer = TRUE;
    }

    TRACE("structversion=%u.%u, ",
            HIWORD(vffi->dwStrucVersion),LOWORD(vffi->dwStrucVersion));
    if(versioned_printer)
    {
        WORD mode = LOWORD(vffi->dwFileVersionMS);
        WORD ver_rev = HIWORD(vffi->dwFileVersionLS);
        TRACE("fileversion=%lu.%u.%u.%u (%s.major.minor.release), ",
            (vffi->dwFileVersionMS),
            HIBYTE(ver_rev), LOBYTE(ver_rev), LOWORD(vffi->dwFileVersionLS),
            (mode == 3) ? "Usermode" : ((mode <= 2) ? "Kernelmode" : "?") );
    }
    else
    {
        TRACE("fileversion=%u.%u.%u.%u, ",
            HIWORD(vffi->dwFileVersionMS),LOWORD(vffi->dwFileVersionMS),
            HIWORD(vffi->dwFileVersionLS),LOWORD(vffi->dwFileVersionLS));
    }
    TRACE("productversion=%u.%u.%u.%u\n",
          HIWORD(vffi->dwProductVersionMS),LOWORD(vffi->dwProductVersionMS),
          HIWORD(vffi->dwProductVersionLS),LOWORD(vffi->dwProductVersionLS));

    TRACE("flagmask=0x%lx, flags=0x%lx %s%s%s%s%s%s\n",
          vffi->dwFileFlagsMask, vffi->dwFileFlags,
          (vffi->dwFileFlags & VS_FF_DEBUG) ? "DEBUG," : "",
          (vffi->dwFileFlags & VS_FF_PRERELEASE) ? "PRERELEASE," : "",
          (vffi->dwFileFlags & VS_FF_PATCHED) ? "PATCHED," : "",
          (vffi->dwFileFlags & VS_FF_PRIVATEBUILD) ? "PRIVATEBUILD," : "",
          (vffi->dwFileFlags & VS_FF_INFOINFERRED) ? "INFOINFERRED," : "",
          (vffi->dwFileFlags & VS_FF_SPECIALBUILD) ? "SPECIALBUILD," : "");

    TRACE("(");

    TRACE("OS=0x%x.0x%x ", HIWORD(vffi->dwFileOS), LOWORD(vffi->dwFileOS));

    switch (vffi->dwFileOS&0xFFFF0000)
    {
    case VOS_DOS:TRACE("DOS,");break;
    case VOS_OS216:TRACE("OS/2-16,");break;
    case VOS_OS232:TRACE("OS/2-32,");break;
    case VOS_NT:TRACE("NT,");break;
    case VOS_UNKNOWN:
    default:
        TRACE("UNKNOWN(0x%lx),",vffi->dwFileOS&0xFFFF0000);break;
    }

    switch (LOWORD(vffi->dwFileOS))
    {
    case VOS__BASE:TRACE("BASE");break;
    case VOS__WINDOWS16:TRACE("WIN16");break;
    case VOS__WINDOWS32:TRACE("WIN32");break;
    case VOS__PM16:TRACE("PM16");break;
    case VOS__PM32:TRACE("PM32");break;
    default:
        TRACE("UNKNOWN(0x%x)",LOWORD(vffi->dwFileOS));break;
    }

    TRACE(")\n");

    switch (vffi->dwFileType)
    {
    case VFT_APP:TRACE("filetype=APP");break;
    case VFT_DLL:
        TRACE("filetype=DLL");
        if(vffi->dwFileSubtype != 0)
        {
            if(versioned_printer) /* NT3.x/NT4.0 or old w2k Driver  */
                TRACE(",PRINTER");
            TRACE(" (subtype=0x%lx)", vffi->dwFileSubtype);
        }
        break;
    case VFT_DRV:
        TRACE("filetype=DRV,");
        switch(vffi->dwFileSubtype)
        {
        case VFT2_DRV_PRINTER:TRACE("PRINTER");break;
        case VFT2_DRV_KEYBOARD:TRACE("KEYBOARD");break;
        case VFT2_DRV_LANGUAGE:TRACE("LANGUAGE");break;
        case VFT2_DRV_DISPLAY:TRACE("DISPLAY");break;
        case VFT2_DRV_MOUSE:TRACE("MOUSE");break;
        case VFT2_DRV_NETWORK:TRACE("NETWORK");break;
        case VFT2_DRV_SYSTEM:TRACE("SYSTEM");break;
        case VFT2_DRV_INSTALLABLE:TRACE("INSTALLABLE");break;
        case VFT2_DRV_SOUND:TRACE("SOUND");break;
        case VFT2_DRV_COMM:TRACE("COMM");break;
        case VFT2_DRV_INPUTMETHOD:TRACE("INPUTMETHOD");break;
        case VFT2_DRV_VERSIONED_PRINTER:TRACE("VERSIONED_PRINTER");break;
        case VFT2_UNKNOWN:
        default:
            TRACE("UNKNOWN(0x%lx)",vffi->dwFileSubtype);break;
        }
        break;
    case VFT_FONT:
        TRACE("filetype=FONT,");
        switch (vffi->dwFileSubtype)
        {
        case VFT2_FONT_RASTER:TRACE("RASTER");break;
        case VFT2_FONT_VECTOR:TRACE("VECTOR");break;
        case VFT2_FONT_TRUETYPE:TRACE("TRUETYPE");break;
        default:TRACE("UNKNOWN(0x%lx)",vffi->dwFileSubtype);break;
        }
        break;
    case VFT_VXD:TRACE("filetype=VXD");break;
    case VFT_STATIC_LIB:TRACE("filetype=STATIC_LIB");break;
    case VFT_UNKNOWN:
    default:
        TRACE("filetype=Unknown(0x%lx)",vffi->dwFileType);break;
    }

    TRACE("\n");
    TRACE("filedate=0x%lx.0x%lx\n",vffi->dwFileDateMS,vffi->dwFileDateLS);
}

/***********************************************************************
 *           GetFileVersionInfoSizeW         (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeW( LPCWSTR filename, LPDWORD handle )
{
    return GetFileVersionInfoSizeExW( FILE_VER_GET_LOCALISED, filename, handle );
}

/***********************************************************************
 *           GetFileVersionInfoSizeA         (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeA( LPCSTR filename, LPDWORD handle )
{
    return GetFileVersionInfoSizeExA( FILE_VER_GET_LOCALISED, filename, handle );
}

/******************************************************************************
 *           GetFileVersionInfoSizeExW       (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeExW( DWORD flags, LPCWSTR filename, LPDWORD ret_handle )
{
    DWORD len, offset, magic = 1;
    HMODULE hModule;

    TRACE("(0x%lx,%s,%p)\n", flags, debugstr_w(filename), ret_handle );

    if (ret_handle) *ret_handle = 0;

    if (!filename)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!*filename)
    {
        SetLastError(ERROR_BAD_PATHNAME);
        return 0;
    }
    if (flags & ~FILE_VER_GET_LOCALISED)
        FIXME("flags 0x%lx ignored\n", flags & ~FILE_VER_GET_LOCALISED);

    if ((hModule = LoadLibraryExW( filename, 0, LOAD_LIBRARY_AS_IMAGE_RESOURCE )))
    {
        HRSRC hRsrc = NULL;
        if (!(flags & FILE_VER_GET_LOCALISED))
        {
            LANGID english = MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT );
            hRsrc = FindResourceExW( hModule, (LPWSTR)VS_FILE_INFO,
                                     MAKEINTRESOURCEW(VS_VERSION_INFO), english );
        }
        if (!hRsrc)
            hRsrc = FindResourceW( hModule, MAKEINTRESOURCEW(VS_VERSION_INFO),
                                   (LPWSTR)VS_FILE_INFO );
        if (hRsrc)
        {
            magic = IMAGE_NT_SIGNATURE;
            len = SizeofResource( hModule, hRsrc );
        }
        FreeLibrary( hModule );
    }
    else
    {
        HANDLE handle = CreateFileW( filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, 0 );
        if (handle == INVALID_HANDLE_VALUE) return 0;
        magic = find_version_resource( handle, &len, &offset, flags );
        CloseHandle( handle );
    }

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        /* We have a 16bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is more than the actual needed space:
         *
         * (info->wLength - sizeof(VS_FIXEDFILEINFO)) * 4
         *
         * This extra buffer is used for ANSI to Unicode conversions in W-Calls.
         * info->wLength should be the same as len. Currently it isn't but that
         * doesn't seem to be a problem (len is bigger than info->wLength).
         */
        SetLastError(0);
        return (len - sizeof(VS_FIXEDFILEINFO)) * 4;

    case IMAGE_NT_SIGNATURE:
        /* We have a 32bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is 2 times the actual needed space + 4 bytes "FE2X"
         * This extra buffer is used for Unicode to ANSI conversions in A-Calls
         */
        SetLastError(0);
        return (len * 2) + 4;

    default:
        if (GetVersion() & 0x80000000) /* Windows 95/98 */
            SetLastError(ERROR_FILE_NOT_FOUND);
        else
            SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
}

/******************************************************************************
 *           GetFileVersionInfoSizeExA       (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeExA( DWORD flags, LPCSTR filename, LPDWORD handle )
{
    UNICODE_STRING filenameW;
    DWORD retval;

    TRACE("(0x%lx,%s,%p)\n", flags, debugstr_a(filename), handle );

    if(filename)
        RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else
        filenameW.Buffer = NULL;

    retval = GetFileVersionInfoSizeExW(flags, filenameW.Buffer, handle);

    RtlFreeUnicodeString(&filenameW);

    return retval;
}

/***********************************************************************
 *           GetFileVersionInfoExW           (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoExW( DWORD flags, LPCWSTR filename, DWORD ignored, DWORD datasize, LPVOID data )
{
    static const char signature[4] = "FE2X";
    DWORD len, offset, magic = 1;
    HMODULE hModule;
    VS_VERSION_INFO_STRUCT32* vvis = data;

    TRACE("(0x%lx,%s,%ld,size=%ld,data=%p)\n",
          flags, debugstr_w(filename), ignored, datasize, data );

    if (!data)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if (flags & ~FILE_VER_GET_LOCALISED)
        FIXME("flags 0x%lx ignored\n", flags & ~FILE_VER_GET_LOCALISED);

    if ((hModule = LoadLibraryExW( filename, 0, LOAD_LIBRARY_AS_IMAGE_RESOURCE )))
    {
        HRSRC hRsrc = NULL;
        if (!(flags & FILE_VER_GET_LOCALISED))
        {
            LANGID english = MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT );
            hRsrc = FindResourceExW( hModule, (LPWSTR)VS_FILE_INFO,
                                     MAKEINTRESOURCEW(VS_VERSION_INFO), english );
        }
        if (!hRsrc)
            hRsrc = FindResourceW( hModule, MAKEINTRESOURCEW(VS_VERSION_INFO),
                                   (LPWSTR)VS_FILE_INFO );
        if (hRsrc)
        {
            HGLOBAL hMem = LoadResource( hModule, hRsrc );
            magic = IMAGE_NT_SIGNATURE;
            len = min( SizeofResource(hModule, hRsrc), datasize );
            memcpy( data, LockResource( hMem ), len );
            FreeResource( hMem );
        }
        FreeLibrary( hModule );
    }
    else
    {
        HANDLE handle = CreateFileW( filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, 0 );
        if (handle == INVALID_HANDLE_VALUE) return 0;
        if ((magic = find_version_resource( handle, &len, &offset, flags )))
            len = read_data( handle, offset, data, min( len, datasize ));
        CloseHandle( handle );
    }

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        /* We have a 16bit resource. */
        if (TRACE_ON(ver))
            print_vffi_debug( (VS_FIXEDFILEINFO *)VersionInfo16_Value( (VS_VERSION_INFO_STRUCT16 *)data ));
        SetLastError(0);
        return TRUE;

    case IMAGE_NT_SIGNATURE:
        /* We have a 32bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is 2 times the actual needed space + 4 bytes "FE2X"
         * This extra buffer is used for Unicode to ANSI conversions in A-Calls
         */
        len = vvis->wLength + sizeof(signature);
        if (datasize >= len) memcpy( (char*)data + vvis->wLength, signature, sizeof(signature) );
        if (TRACE_ON(ver))
            print_vffi_debug( (VS_FIXEDFILEINFO *)VersionInfo32_Value( vvis ));
        SetLastError(0);
        return TRUE;

    default:
        SetLastError( ERROR_RESOURCE_DATA_NOT_FOUND );
        return FALSE;
    }
}

/***********************************************************************
 *           GetFileVersionInfoExA           (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoExA( DWORD flags, LPCSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    UNICODE_STRING filenameW;
    BOOL retval;

    TRACE("(0x%lx,%s,%ld,size=%ld,data=%p)\n",
          flags, debugstr_a(filename), handle, datasize, data );

    if(filename)
        RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else
        filenameW.Buffer = NULL;

    retval = GetFileVersionInfoExW(flags, filenameW.Buffer, handle, datasize, data);

    RtlFreeUnicodeString(&filenameW);

    return retval;
}

/***********************************************************************
 *           GetFileVersionInfoW             (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoW( LPCWSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    return GetFileVersionInfoExW(FILE_VER_GET_LOCALISED, filename, handle, datasize, data);
}

/***********************************************************************
 *           GetFileVersionInfoA             (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoA( LPCSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    return GetFileVersionInfoExA(FILE_VER_GET_LOCALISED, filename, handle, datasize, data);
}

/***********************************************************************
 *           VersionInfo16_FindChild             [internal]
 */
static const VS_VERSION_INFO_STRUCT16 *VersionInfo16_FindChild( const VS_VERSION_INFO_STRUCT16 *info,
                                                                LPCSTR key, UINT len )
{
    const VS_VERSION_INFO_STRUCT16 *child = VersionInfo16_Children( info );

    while ((char *)child < (char *)info + info->wLength )
    {
        if (!strnicmp( child->szKey, key, len ) && !child->szKey[len])
            return child;

        if (!(child->wLength)) return NULL;
        child = VersionInfo16_Next( child );
    }

    return NULL;
}

/***********************************************************************
 *           VersionInfo32_FindChild             [internal]
 */
static const VS_VERSION_INFO_STRUCT32 *VersionInfo32_FindChild( const VS_VERSION_INFO_STRUCT32 *info,
                                                                LPCWSTR key, UINT len )
{
    const VS_VERSION_INFO_STRUCT32 *child = VersionInfo32_Children( info );

    while ((char *)child < (char *)info + info->wLength )
    {
        if (!wcsnicmp( child->szKey, key, len ) && !child->szKey[len])
            return child;

        if (!(child->wLength)) return NULL;
        child = VersionInfo32_Next( child );
    }

    return NULL;
}

/***********************************************************************
 *           VersionInfo16_QueryValue              [internal]
 *
 *    Gets a value from a 16-bit NE resource
 */
static BOOL VersionInfo16_QueryValue( const VS_VERSION_INFO_STRUCT16 *info, LPCSTR lpSubBlock,
                               LPVOID *lplpBuffer, UINT *puLen )
{
    while ( *lpSubBlock )
    {
        /* Find next path component */
        LPCSTR lpNextSlash;
        for ( lpNextSlash = lpSubBlock; *lpNextSlash; lpNextSlash++ )
            if ( *lpNextSlash == '\\' )
                break;

        /* Skip empty components */
        if ( lpNextSlash == lpSubBlock )
        {
            lpSubBlock++;
            continue;
        }

        /* We have a non-empty component: search info for key */
        info = VersionInfo16_FindChild( info, lpSubBlock, lpNextSlash-lpSubBlock );
        if ( !info )
        {
            if (puLen) *puLen = 0 ;
            SetLastError( ERROR_RESOURCE_TYPE_NOT_FOUND );
            return FALSE;
        }

        /* Skip path component */
        lpSubBlock = lpNextSlash;
    }

    /* Return value */
    *lplpBuffer = VersionInfo16_Value( info );
    if (puLen)
        *puLen = info->wValueLength;

    return TRUE;
}

/***********************************************************************
 *           VersionInfo32_QueryValue              [internal]
 *
 *    Gets a value from a 32-bit PE resource
 */
static BOOL VersionInfo32_QueryValue( const VS_VERSION_INFO_STRUCT32 *info, LPCWSTR lpSubBlock,
                                      LPVOID *lplpBuffer, UINT *puLen, BOOL *pbText )
{
    PVOID ptr;
    TRACE("lpSubBlock : (%s)\n", debugstr_w(lpSubBlock));

    while ( *lpSubBlock )
    {
        /* Find next path component */
        LPCWSTR lpNextSlash;
        for ( lpNextSlash = lpSubBlock; *lpNextSlash; lpNextSlash++ )
            if ( *lpNextSlash == '\\' )
                break;

        /* Skip empty components */
        if ( lpNextSlash == lpSubBlock )
        {
            lpSubBlock++;
            continue;
        }

        /* We have a non-empty component: search info for key */
        info = VersionInfo32_FindChild( info, lpSubBlock, lpNextSlash-lpSubBlock );
        if ( !info )
        {
            if (puLen) *puLen = 0 ;
            SetLastError( ERROR_RESOURCE_TYPE_NOT_FOUND );
            return FALSE;
        }

        /* Skip path component */
        lpSubBlock = lpNextSlash;
    }

    /* Return value */
    ptr = VersionInfo32_Value(info);
    if ((PBYTE)ptr >= ((PBYTE)info + info->wLength))  /* empty value */
        ptr = (WCHAR*)info->szKey + wcslen(info->szKey);

    *lplpBuffer = ptr;
    if (puLen)
        *puLen = info->wValueLength;
    if (pbText)
        *pbText = info->wType;

    return TRUE;
}

/***********************************************************************
 *           VerQueryValueA              (kernelbase.@)
 */
BOOL WINAPI VerQueryValueA( LPCVOID pBlock, LPCSTR lpSubBlock,
                               LPVOID *lplpBuffer, PUINT puLen )
{
    static const char rootA[] = "\\";
    const VS_VERSION_INFO_STRUCT16 *info = pBlock;

    TRACE("(%p,%s,%p,%p)\n",
                pBlock, debugstr_a(lpSubBlock), lplpBuffer, puLen );

     if (!pBlock)
        return FALSE;

    if (lpSubBlock == NULL || lpSubBlock[0] == '\0')
        lpSubBlock = rootA;

    if ( !VersionInfoIs16( info ) )
    {
        BOOL ret, isText;
        INT len;
        LPWSTR lpSubBlockW;
        UINT value_len;

        len  = MultiByteToWideChar(CP_ACP, 0, lpSubBlock, -1, NULL, 0);
        lpSubBlockW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));

        if (!lpSubBlockW)
            return FALSE;

        MultiByteToWideChar(CP_ACP, 0, lpSubBlock, -1, lpSubBlockW, len);

        ret = VersionInfo32_QueryValue(pBlock, lpSubBlockW, lplpBuffer, &value_len, &isText);
        if (puLen) *puLen = value_len;

        HeapFree(GetProcessHeap(), 0, lpSubBlockW);

        if (ret && isText)
        {
            /* Set lpBuffer so it points to the 'empty' area where we store
             * the converted strings
             */
            LPSTR lpBufferA = (LPSTR)pBlock + info->wLength + 4;
            DWORD pos = (LPCSTR)*lplpBuffer - (LPCSTR)pBlock;
            len = WideCharToMultiByte(CP_ACP, 0, *lplpBuffer, value_len,
                                      lpBufferA + pos, info->wLength - pos, NULL, NULL);
            *lplpBuffer = lpBufferA + pos;
            if (puLen) *puLen = len;
        }
        return ret;
    }

    return VersionInfo16_QueryValue(info, lpSubBlock, lplpBuffer, puLen);
}

/***********************************************************************
 *           VerQueryValueW              (kernelbase.@)
 */
BOOL WINAPI VerQueryValueW( LPCVOID pBlock, LPCWSTR lpSubBlock,
                               LPVOID *lplpBuffer, PUINT puLen )
{
    const VS_VERSION_INFO_STRUCT32 *info = pBlock;

    TRACE("(%p,%s,%p,%p)\n",
                pBlock, debugstr_w(lpSubBlock), lplpBuffer, puLen );

    if (!pBlock)
        return FALSE;

    if (!lpSubBlock || !lpSubBlock[0])
        lpSubBlock = L"\\";

    if ( VersionInfoIs16( info ) )
    {
        BOOL ret;
        int len;
        LPSTR lpSubBlockA;

        len = WideCharToMultiByte(CP_ACP, 0, lpSubBlock, -1, NULL, 0, NULL, NULL);
        lpSubBlockA = HeapAlloc(GetProcessHeap(), 0, len * sizeof(char));

        if (!lpSubBlockA)
            return FALSE;

        WideCharToMultiByte(CP_ACP, 0, lpSubBlock, -1, lpSubBlockA, len, NULL, NULL);

        ret = VersionInfo16_QueryValue(pBlock, lpSubBlockA, lplpBuffer, puLen);

        HeapFree(GetProcessHeap(), 0, lpSubBlockA);

        if (ret && wcscmp( lpSubBlock, L"\\" ) && wcsicmp( lpSubBlock, L"\\VarFileInfo\\Translation" ))
        {
            /* Set lpBuffer so it points to the 'empty' area where we store
             * the converted strings
             */
            LPWSTR lpBufferW = (LPWSTR)((LPSTR)pBlock + info->wLength);
            DWORD pos = (LPCSTR)*lplpBuffer - (LPCSTR)pBlock;
            DWORD max = (info->wLength - sizeof(VS_FIXEDFILEINFO)) * 4 - info->wLength;

            len = MultiByteToWideChar(CP_ACP, 0, *lplpBuffer, -1,
                                      lpBufferW + pos, max/sizeof(WCHAR) - pos );
            *lplpBuffer = lpBufferW + pos;
            if (puLen) *puLen = len;
        }
        return ret;
    }

    return VersionInfo32_QueryValue(info, lpSubBlock, lplpBuffer, puLen, NULL);
}


/******************************************************************************
 *   file_existsA
 */
static BOOL file_existsA( char const * path, char const * file, BOOL excl )
{
    DWORD sharing = excl ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE;
    char filename[MAX_PATH];
    int len;
    HANDLE handle;

    if (path)
    {
        strcpy( filename, path );
        len = strlen(filename);
        if (len && filename[len - 1] != '\\') strcat( filename, "\\" );
        strcat( filename, file );
    }
    else if (!SearchPathA( NULL, file, NULL, MAX_PATH, filename, NULL )) return FALSE;

    handle = CreateFileA( filename, 0, sharing, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle( handle );
    return TRUE;
}

/******************************************************************************
 *   file_existsW
 */
static BOOL file_existsW( const WCHAR *path, const WCHAR *file, BOOL excl )
{
    DWORD sharing = excl ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE;
    WCHAR filename[MAX_PATH];
    int len;
    HANDLE handle;

    if (path)
    {
        lstrcpyW( filename, path );
        len = lstrlenW(filename);
        if (len && filename[len - 1] != '\\') lstrcatW( filename, L"\\" );
        lstrcatW( filename, file );
    }
    else if (!SearchPathW( NULL, file, NULL, MAX_PATH, filename, NULL )) return FALSE;

    handle = CreateFileW( filename, 0, sharing, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle( handle );
    return TRUE;
}

/*****************************************************************************
 *   VerFindFileA (kernelbase.@)
 *
 *   Determines where to install a file based on whether it locates another
 *   version of the file in the system.  The values VerFindFile returns are
 *   used in a subsequent call to the VerInstallFile function.
 */
DWORD WINAPI VerFindFileA( DWORD flags, LPCSTR filename, LPCSTR win_dir, LPCSTR app_dir,
                           LPSTR cur_dir, PUINT curdir_len, LPSTR dest, PUINT dest_len )
{
    DWORD  retval = 0;
    const char *curDir;
    const char *destDir;
    char winDir[MAX_PATH], systemDir[MAX_PATH];

    TRACE("flags = %lx filename=%s windir=%s appdir=%s curdirlen=%p(%u) destdirlen=%p(%u)\n",
          flags, debugstr_a(filename), debugstr_a(win_dir), debugstr_a(app_dir),
          curdir_len, curdir_len ? *curdir_len : 0, dest_len, dest_len ? *dest_len : 0 );

    /* Figure out where the file should go; shared files default to the
       system directory */

    GetSystemDirectoryA(systemDir, sizeof(systemDir));
    curDir = "";

    if(flags & VFFF_ISSHAREDFILE)
    {
        destDir = systemDir;
        /* Were we given a filename?  If so, try to find the file. */
        if(filename)
        {
            if(file_existsA(destDir, filename, FALSE)) curDir = destDir;
            else if(app_dir && file_existsA(app_dir, filename, FALSE))
                curDir = app_dir;

            if(!file_existsA(systemDir, filename, FALSE))
                retval |= VFF_CURNEDEST;
        }
    }
    else /* not a shared file */
    {
        destDir = app_dir ? app_dir : "";
        if(filename)
        {
            GetWindowsDirectoryA( winDir, MAX_PATH );
            if(file_existsA(destDir, filename, FALSE)) curDir = destDir;
            else if(file_existsA(winDir, filename, FALSE))
                curDir = winDir;
            else if(file_existsA(systemDir, filename, FALSE))
                curDir = systemDir;

            if (app_dir && app_dir[0])
            {
                if(!file_existsA(app_dir, filename, FALSE))
                    retval |= VFF_CURNEDEST;
            }
            else if(file_existsA(NULL, filename, FALSE))
                retval |= VFF_CURNEDEST;
        }
    }

    /* Check to see if the file exists and is in use by another application */
    if (filename && file_existsA(curDir, filename, FALSE))
    {
        if (filename && !file_existsA(curDir, filename, TRUE))
           retval |= VFF_FILEINUSE;
    }

    if (dest_len && dest)
    {
        UINT len = strlen(destDir) + 1;
        if (*dest_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynA(dest, destDir, *dest_len);
        *dest_len = len;
    }
    if (curdir_len && cur_dir)
    {
        UINT len = strlen(curDir) + 1;
        if (*curdir_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynA(cur_dir, curDir, *curdir_len);
        *curdir_len = len;
    }

    TRACE("ret = %lu (%s%s%s) curdir=%s destdir=%s\n", retval,
          (retval & VFF_CURNEDEST) ? "VFF_CURNEDEST " : "",
          (retval & VFF_FILEINUSE) ? "VFF_FILEINUSE " : "",
          (retval & VFF_BUFFTOOSMALL) ? "VFF_BUFFTOOSMALL " : "",
          debugstr_a(cur_dir), debugstr_a(dest));

    return retval;
}

/*****************************************************************************
 * VerFindFileW (kernelbase.@)
 */
DWORD WINAPI VerFindFileW( DWORD flags, LPCWSTR filename, LPCWSTR win_dir, LPCWSTR app_dir,
                           LPWSTR cur_dir, PUINT curdir_len, LPWSTR dest, PUINT dest_len )
{
    DWORD retval = 0;
    const WCHAR *curDir;
    const WCHAR *destDir;

    TRACE("flags = %lx filename=%s windir=%s appdir=%s curdirlen=%p(%u) destdirlen=%p(%u)\n",
          flags, debugstr_w(filename), debugstr_w(win_dir), debugstr_w(app_dir),
          curdir_len, curdir_len ? *curdir_len : 0, dest_len, dest_len ? *dest_len : 0 );

    /* Figure out where the file should go; shared files default to the
       system directory */

    curDir = L"";

    if(flags & VFFF_ISSHAREDFILE)
    {
        destDir = system_dir;
        /* Were we given a filename?  If so, try to find the file. */
        if(filename)
        {
            if(file_existsW(destDir, filename, FALSE)) curDir = destDir;
            else if(app_dir && file_existsW(app_dir, filename, FALSE))
            {
                curDir = app_dir;
                retval |= VFF_CURNEDEST;
            }
        }
    }
    else /* not a shared file */
    {
        destDir = app_dir ? app_dir : L"";
        if(filename)
        {
            if(file_existsW(destDir, filename, FALSE)) curDir = destDir;
            else if(file_existsW(windows_dir, filename, FALSE))
            {
                curDir = windows_dir;
                retval |= VFF_CURNEDEST;
            }
            else if (file_existsW(system_dir, filename, FALSE))
            {
                curDir = system_dir;
                retval |= VFF_CURNEDEST;
            }
        }
    }

    if (filename && !file_existsW(curDir, filename, TRUE))
        retval |= VFF_FILEINUSE;

    if (dest_len && dest)
    {
        UINT len = lstrlenW(destDir) + 1;
        if (*dest_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynW(dest, destDir, *dest_len);
        *dest_len = len;
    }
    if (curdir_len && cur_dir)
    {
        UINT len = lstrlenW(curDir) + 1;
        if (*curdir_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynW(cur_dir, curDir, *curdir_len);
        *curdir_len = len;
    }

    TRACE("ret = %lu (%s%s%s) curdir=%s destdir=%s\n", retval,
          (retval & VFF_CURNEDEST) ? "VFF_CURNEDEST " : "",
          (retval & VFF_FILEINUSE) ? "VFF_FILEINUSE " : "",
          (retval & VFF_BUFFTOOSMALL) ? "VFF_BUFFTOOSMALL " : "",
          debugstr_w(cur_dir), debugstr_w(dest));
    return retval;
}


/***********************************************************************
 *         GetProductInfo   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetProductInfo( DWORD os_major, DWORD os_minor,
                                              DWORD sp_major, DWORD sp_minor, DWORD *type )
{
    return RtlGetProductInfo( os_major, os_minor, sp_major, sp_minor, type );
}


/***********************************************************************
 *         GetVersion   (kernelbase.@)
 */
DWORD WINAPI GetVersion(void)
{
    OSVERSIONINFOEXW info;
    DWORD result;

    info.dwOSVersionInfoSize = sizeof(info);
    if (!GetVersionExW( (OSVERSIONINFOW *)&info )) return 0;

    result = MAKELONG( MAKEWORD( info.dwMajorVersion, info.dwMinorVersion ),
                       (info.dwPlatformId ^ 2) << 14 );

    if (info.dwPlatformId == VER_PLATFORM_WIN32_NT)
        result |= LOWORD(info.dwBuildNumber) << 16;
    return result;
}


/***********************************************************************
 *         GetVersionExA   (kernelbase.@)
 */
BOOL WINAPI GetVersionExA( OSVERSIONINFOA *info )
{
    OSVERSIONINFOEXW infoW;

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOA) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXA))
    {
        WARN( "wrong OSVERSIONINFO size from app (got: %ld)\n", info->dwOSVersionInfoSize );
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        return FALSE;
    }

    infoW.dwOSVersionInfoSize = sizeof(infoW);
    if (!GetVersionExW( (OSVERSIONINFOW *)&infoW )) return FALSE;

    info->dwMajorVersion = infoW.dwMajorVersion;
    info->dwMinorVersion = infoW.dwMinorVersion;
    info->dwBuildNumber  = infoW.dwBuildNumber;
    info->dwPlatformId   = infoW.dwPlatformId;
    WideCharToMultiByte( CP_ACP, 0, infoW.szCSDVersion, -1,
                         info->szCSDVersion, sizeof(info->szCSDVersion), NULL, NULL );

    if (info->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA))
    {
        OSVERSIONINFOEXA *vex = (OSVERSIONINFOEXA *)info;
        vex->wServicePackMajor = infoW.wServicePackMajor;
        vex->wServicePackMinor = infoW.wServicePackMinor;
        vex->wSuiteMask        = infoW.wSuiteMask;
        vex->wProductType      = infoW.wProductType;
    }
    return TRUE;
}


/***********************************************************************
 *         GetVersionExW   (kernelbase.@)
 */
BOOL WINAPI GetVersionExW( OSVERSIONINFOW *info )
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOW) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW))
    {
        WARN( "wrong OSVERSIONINFO size from app (got: %ld)\n", info->dwOSVersionInfoSize );
        return FALSE;
    }

    if (!InitOnceExecuteOnce(&init_once, init_current_version, NULL, NULL)) return FALSE;

    info->dwMajorVersion = current_version.dwMajorVersion;
    info->dwMinorVersion = current_version.dwMinorVersion;
    info->dwBuildNumber  = current_version.dwBuildNumber;
    info->dwPlatformId   = current_version.dwPlatformId;
    wcscpy( info->szCSDVersion, current_version.szCSDVersion );

    if (info->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW))
    {
        OSVERSIONINFOEXW *vex = (OSVERSIONINFOEXW *)info;
        vex->wServicePackMajor = current_version.wServicePackMajor;
        vex->wServicePackMinor = current_version.wServicePackMinor;
        vex->wSuiteMask        = current_version.wSuiteMask;
        vex->wProductType      = current_version.wProductType;
    }
    return TRUE;
}

static BOOL validate_package_string_buffer( const UINT32 *length, const WCHAR *buffer )
{
    return length && (buffer || !*length);
}

/* The process handle has to allow PROCESS_QUERY_LIMITED_INFORMATION, with or without a package. */
static LONG validate_package_process_handle( HANDLE process )
{
    PROCESS_BASIC_INFORMATION info;
    NTSTATUS status;

    status = NtQueryInformationProcess( process, ProcessBasicInformation, &info, sizeof(info), NULL );
    if (status) return RtlNtStatusToDosError( status );
    return ERROR_SUCCESS;
}

enum package_string
{
    PACKAGE_STRING_FULL_NAME,
    PACKAGE_STRING_FAMILY_NAME,
    PACKAGE_STRING_APP_ID,
    PACKAGE_STRING_PATH,
    PACKAGE_STRING_AUMID,
};

/* The package context of a token. The server keeps it with the token; nothing in the process (environment,
 * PEB, image path) is consulted. Returns NULL with the error: APPMODEL_ERROR_NO_PACKAGE without a context. */
static WINE_TOKEN_PACKAGE_INFORMATION *get_token_package( HANDLE token, LONG *err )
{
    WINE_TOKEN_PACKAGE_INFORMATION *info = NULL;
    ULONG size = 0;
    NTSTATUS status;

    status = NtQueryInformationToken( token, TokenWinePackage, NULL, 0, &size );
    while (status == STATUS_BUFFER_TOO_SMALL)
    {
        HeapFree( GetProcessHeap(), 0, info );
        if (!(info = HeapAlloc( GetProcessHeap(), 0, size )))
        {
            *err = ERROR_OUTOFMEMORY;
            return NULL;
        }
        status = NtQueryInformationToken( token, TokenWinePackage, info, size, &size );
    }
    if (status)
    {
        HeapFree( GetProcessHeap(), 0, info );
        *err = status == STATUS_NOT_FOUND ? APPMODEL_ERROR_NO_PACKAGE : RtlNtStatusToDosError( status );
        return NULL;
    }
    return info;
}

/* Lengths count characters and include the terminator, in and out. */
static LONG copy_package_string( const WINE_TOKEN_PACKAGE_INFORMATION *info, enum package_string which,
                                 UINT32 *length, WCHAR *buffer )
{
    const WCHAR *family = info->data + info->full_name_len / sizeof(WCHAR);
    const WCHAR *app_id = family + info->family_name_len / sizeof(WCHAR);
    const WCHAR *str;
    UINT32 len, extra = 0;

    switch (which)
    {
    case PACKAGE_STRING_FULL_NAME:
        str = info->data;
        len = info->full_name_len / sizeof(WCHAR);
        break;
    case PACKAGE_STRING_FAMILY_NAME:
        str = family;
        len = info->family_name_len / sizeof(WCHAR);
        break;
    case PACKAGE_STRING_APP_ID:
        str = app_id;
        len = info->app_id_len / sizeof(WCHAR);
        break;
    case PACKAGE_STRING_PATH:
        str = app_id + info->app_id_len / sizeof(WCHAR);
        len = info->root_len / sizeof(WCHAR);
        break;
    case PACKAGE_STRING_AUMID:   /* FamilyName!ApplicationId */
        str = family;
        len = info->family_name_len / sizeof(WCHAR);
        extra = 1 + info->app_id_len / sizeof(WCHAR);
        break;
    default:
        return ERROR_INVALID_PARAMETER;
    }

    if (*length < len + extra + 1)
    {
        *length = len + extra + 1;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    memcpy( buffer, str, len * sizeof(WCHAR) );
    if (extra)
    {
        buffer[len] = '!';
        memcpy( buffer + len + 1, app_id, (extra - 1) * sizeof(WCHAR) );
    }
    buffer[len + extra] = 0;
    *length = len + extra + 1;
    return ERROR_SUCCESS;
}

static LONG get_token_package_string( HANDLE token, enum package_string which, UINT32 *length, WCHAR *buffer )
{
    WINE_TOKEN_PACKAGE_INFORMATION *info;
    LONG err = ERROR_SUCCESS;

    if (!(info = get_token_package( token, &err ))) return err;
    err = copy_package_string( info, which, length, buffer );
    HeapFree( GetProcessHeap(), 0, info );
    return err;
}

/* The identity of a process is the package context of its primary token. */
static LONG get_process_package_string( HANDLE process, enum package_string which, UINT32 *length, WCHAR *buffer )
{
    HANDLE token;
    NTSTATUS status;
    LONG err;

    if ((status = NtOpenProcessToken( process, TOKEN_QUERY, &token ))) return RtlNtStatusToDosError( status );
    err = get_token_package_string( token, which, length, buffer );
    NtClose( token );
    return err;
}

/***********************************************************************
 *         GetCurrentApplicationUserModelId   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentApplicationUserModelId( UINT32 *length, WCHAR *id )
{
    LONG err;

    TRACE( "(%p %p)\n", length, id );

    if (!validate_package_string_buffer( length, id ))
        return ERROR_INVALID_PARAMETER;

    err = get_process_package_string( GetCurrentProcess(), PACKAGE_STRING_AUMID, length, id );
    return err == APPMODEL_ERROR_NO_PACKAGE ? APPMODEL_ERROR_NO_APPLICATION : err;
}

/***********************************************************************
 *         GetCurrentPackageFamilyName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageFamilyName( UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p)\n", length, name );

    if (!validate_package_string_buffer( length, name ))
        return ERROR_INVALID_PARAMETER;

    return get_process_package_string( GetCurrentProcess(), PACKAGE_STRING_FAMILY_NAME, length, name );
}


/***********************************************************************
 *         GetCurrentPackageFullName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageFullName( UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p)\n", length, name );

    if (!validate_package_string_buffer( length, name ))
        return ERROR_INVALID_PARAMETER;

    return get_process_package_string( GetCurrentProcess(), PACKAGE_STRING_FULL_NAME, length, name );
}


/***********************************************************************
 *         GetCurrentPackageId   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageId( UINT32 *len, BYTE *buffer )
{
    WCHAR full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    UINT32 name_len = ARRAY_SIZE(full_name);
    LONG err;

    TRACE( "(%p %p)\n", len, buffer );

    if (!len || (!buffer && *len))
        return ERROR_INVALID_PARAMETER;

    if ((err = get_process_package_string( GetCurrentProcess(), PACKAGE_STRING_FULL_NAME, &name_len, full_name )))
        return err;
    return PackageIdFromFullName( full_name, 0 /* PACKAGE_INFORMATION_BASIC */, len, buffer );
}

/***********************************************************************
 *         GetCurrentPackagePath   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackagePath( UINT32 *length, WCHAR *path )
{
    TRACE( "(%p %p)\n", length, path );

    if (!validate_package_string_buffer( length, path ))
        return ERROR_INVALID_PARAMETER;

    return get_process_package_string( GetCurrentProcess(), PACKAGE_STRING_PATH, length, path );
}


/***********************************************************************
 *         GetPackageFullName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFullName( HANDLE process, UINT32 *length, WCHAR *name )
{
    LONG err;

    TRACE( "(%p %p %p)\n", process, length, name );

    if (!process || !validate_package_string_buffer( length, name ))
        return ERROR_INVALID_PARAMETER;

    if ((err = validate_package_process_handle( process )))
        return err;

    return get_process_package_string( process, PACKAGE_STRING_FULL_NAME, length, name );
}


/***********************************************************************
 *         GetPackageFamilyName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFamilyName( HANDLE process, UINT32 *length, WCHAR *name )
{
    LONG err;

    TRACE( "(%p %p %p)\n", process, length, name );

    if (!process || !validate_package_string_buffer( length, name ))
        return ERROR_INVALID_PARAMETER;

    if ((err = validate_package_process_handle( process )))
        return err;

    return get_process_package_string( process, PACKAGE_STRING_FAMILY_NAME, length, name );
}


/***********************************************************************
 *         GetPackageFullNameFromToken   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFullNameFromToken( HANDLE token, UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p %p)\n", token, length, name );

    if (!token || !validate_package_string_buffer( length, name ))
        return ERROR_INVALID_PARAMETER;

    return get_token_package_string( token, PACKAGE_STRING_FULL_NAME, length, name );
}


/***********************************************************************
 *         GetPackageFamilyNameFromToken   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFamilyNameFromToken( HANDLE token, UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p %p)\n", token, length, name );

    if (!token || !validate_package_string_buffer( length, name ))
        return ERROR_INVALID_PARAMETER;

    return get_token_package_string( token, PACKAGE_STRING_FAMILY_NAME, length, name );
}

static const struct
{
    UINT32 code;
    const WCHAR *name;
}
arch_names[] =
{
    {PROCESSOR_ARCHITECTURE_INTEL,         L"x86"},
    {PROCESSOR_ARCHITECTURE_ARM,           L"arm"},
    {PROCESSOR_ARCHITECTURE_AMD64,         L"x64"},
    {PROCESSOR_ARCHITECTURE_NEUTRAL,       L"neutral"},
    {PROCESSOR_ARCHITECTURE_ARM64,         L"arm64"},
    {PROCESSOR_ARCHITECTURE_UNKNOWN,       L"unknown"},
};

static UINT32 processor_arch_from_string(const WCHAR *str, unsigned int len)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(arch_names); ++i)
        if (lstrlenW(arch_names[i].name) == len && !wcsnicmp(str, arch_names[i].name, len))
            return arch_names[i].code;
    return ~0u;
}

static const WCHAR *processor_arch_from_code(UINT32 code)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(arch_names); ++i)
        if (arch_names[i].code == code)
            return arch_names[i].name;

    return NULL;
}

/***********************************************************************
 *         PackageIdFromFullName   (kernelbase.@)
 */
LONG WINAPI PackageIdFromFullName(const WCHAR *full_name, UINT32 flags, UINT32 *buffer_length, BYTE *buffer)
{
    const WCHAR *name, *version_str, *arch_str, *resource_id, *publisher_id, *s;
    PACKAGE_ID *id = (PACKAGE_ID *)buffer;
    UINT32 size, buffer_size, len;

    TRACE("full_name %s, flags %#x, buffer_length %p, buffer %p.\n",
            debugstr_w(full_name), flags, buffer_length, buffer);

    if (flags)
        FIXME("Flags %#x are not supported.\n", flags);

    if (!full_name || !buffer_length)
        return ERROR_INVALID_PARAMETER;

    if (!buffer && *buffer_length)
        return ERROR_INVALID_PARAMETER;

    name = full_name;
    if (!(version_str = wcschr(name, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++version_str;

    if (!(arch_str = wcschr(version_str, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++arch_str;

    if (!(resource_id = wcschr(arch_str, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++resource_id;

    if (!(publisher_id = wcschr(resource_id, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++publisher_id;

    /* Publisher id length should be 13. */
    size = sizeof(*id) + sizeof(WCHAR) * ((version_str - name) + (publisher_id - resource_id) + 13 + 1);
    buffer_size = *buffer_length;
    *buffer_length = size;
    if (buffer_size < size)
        return ERROR_INSUFFICIENT_BUFFER;

    memset(id, 0, sizeof(*id));
    if ((id->processorArchitecture = processor_arch_from_string(arch_str, resource_id - arch_str - 1)) == ~0u)
    {
        FIXME("Unrecognized arch %s.\n", debugstr_w(arch_str));
        return ERROR_INVALID_PARAMETER;
    }
    buffer += sizeof(*id);

    id->version.Major = wcstol(version_str, NULL, 10);
    if (!(s = wcschr(version_str, L'.')))
        return ERROR_INVALID_PARAMETER;
    ++s;
    id->version.Minor = wcstol(s, NULL, 10);
    if (!(s = wcschr(s, L'.')))
        return ERROR_INVALID_PARAMETER;
    ++s;
    id->version.Build = wcstol(s, NULL, 10);
    if (!(s = wcschr(s, L'.')))
        return ERROR_INVALID_PARAMETER;
    ++s;
    id->version.Revision = wcstol(s, NULL, 10);

    id->name = (WCHAR *)buffer;
    len = version_str - name - 1;
    memcpy(id->name, name, sizeof(*id->name) * len);
    id->name[len] = 0;
    buffer += sizeof(*id->name) * (len + 1);

    id->resourceId = (WCHAR *)buffer;
    len = publisher_id - resource_id - 1;
    memcpy(id->resourceId, resource_id, sizeof(*id->resourceId) * len);
    id->resourceId[len] = 0;
    buffer += sizeof(*id->resourceId) * (len + 1);

    id->publisherId = (WCHAR *)buffer;
    len = lstrlenW(publisher_id);
    if (len != 13)
        return ERROR_INVALID_PARAMETER;
    memcpy(id->publisherId, publisher_id, sizeof(*id->publisherId) * len);
    id->publisherId[len] = 0;

    return ERROR_SUCCESS;
}

/* wrappers for symcrypt */
SYMCRYPT_CPU_FEATURES SYMCRYPT_CALL SymCryptCpuFeaturesNeverPresent(void) { return 0; }

DECLSPEC_NORETURN void SYMCRYPT_CALL SymCryptFatal( UINT32 fatalCode )
{
    EXCEPTION_RECORD rec;

    ERR( "SymCrypt fatal error 0x%08x\n", fatalCode );

    memset( &rec, 0, sizeof(rec) );
    rec.ExceptionCode = STATUS_FAIL_FAST_EXCEPTION;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    rec.NumberParameters = 2;
    rec.ExceptionInformation[0] = FAST_FAIL_CRYPTO_LIBRARY;
    rec.ExceptionInformation[1] = fatalCode;

    RaiseFailFastException( &rec, NULL, FAIL_FAST_GENERATE_EXCEPTION_ADDRESS );

    /* Fail-closed fallback: ensure process terminates if RaiseFailFastException returns */
    TerminateProcess( GetCurrentProcess(), STATUS_FAIL_FAST_EXCEPTION );
    RtlExitUserProcess( STATUS_FAIL_FAST_EXCEPTION );
    for (;;) { }
}

void SYMCRYPT_CALL SymCryptInjectError( PBYTE pbBuf, SIZE_T cbBuf ) { }

#if SYMCRYPT_CPU_X86 | SYMCRYPT_CPU_AMD64
/* In user mode, XMM registers are preserved across context switches and interrupts
 * by the OS user-mode thread context per the x86/x64 ABI (unlike kernel mode where
 * KeSaveExtendedProcessorState is required). SymCrypt's official user-mode environment
 * (env_windowsUserModeWin8_1.c) documents that SaveXmm/RestoreXmm are no-ops. */
SYMCRYPT_ERROR SYMCRYPT_CALL SymCryptSaveXmm( PSYMCRYPT_EXTENDED_SAVE_DATA pSaveArea ) { return SYMCRYPT_NO_ERROR; }
void SYMCRYPT_CALL SymCryptRestoreXmm( PSYMCRYPT_EXTENDED_SAVE_DATA pSaveArea ) { }
#endif

static void publisher_id_from_hash(const BYTE hash[8], WCHAR out[PACKAGE_PUBLISHERID_MAX_LENGTH + 1])
{
    static const char charset[] = "0123456789abcdefghjkmnpqrstvwxyz";
    out[0]  = charset[(hash[0] >> 3) & 0x1f];
    out[1]  = charset[((hash[0] << 2) | (hash[1] >> 6)) & 0x1f];
    out[2]  = charset[(hash[1] >> 1) & 0x1f];
    out[3]  = charset[((hash[1] << 4) | (hash[2] >> 4)) & 0x1f];
    out[4]  = charset[((hash[2] << 1) | (hash[3] >> 7)) & 0x1f];
    out[5]  = charset[(hash[3] >> 2) & 0x1f];
    out[6]  = charset[((hash[3] << 3) | (hash[4] >> 5)) & 0x1f];
    out[7]  = charset[hash[4] & 0x1f];
    out[8]  = charset[(hash[5] >> 3) & 0x1f];
    out[9]  = charset[((hash[5] << 2) | (hash[6] >> 6)) & 0x1f];
    out[10] = charset[(hash[6] >> 1) & 0x1f];
    out[11] = charset[((hash[6] << 4) | (hash[7] >> 4)) & 0x1f];
    out[12] = charset[(hash[7] << 1) & 0x1f];
    out[13] = 0;
}

/***********************************************************************
 *         PackageFullNameFromId   (kernelbase.@)
 */
LONG WINAPI PackageFullNameFromId(const PACKAGE_ID *id, UINT32 *length, WCHAR *buffer)
{
    WCHAR full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    WCHAR version[PACKAGE_VERSION_MAX_LENGTH + 1];
    const WCHAR *arch;
    UINT32 capacity, req_len;
    size_t len;

    TRACE("id %p, length %p, buffer %p\n", id, length, buffer);

    if (!id || !length)
        return ERROR_INVALID_PARAMETER;

    len = id->name ? wcslen(id->name) : 0;
    if (len < PACKAGE_NAME_MIN_LENGTH || len > PACKAGE_NAME_MAX_LENGTH)
        return ERROR_INVALID_PARAMETER;

    *full_name = 0;
    wcscpy(full_name, id->name);
    wcscat(full_name, L"_");

    swprintf(version, ARRAYSIZE(version), L"%u.%u.%u.%u", id->version.Major, id->version.Minor,
            id->version.Build, id->version.Revision);
    wcscat(full_name, version);
    wcscat(full_name, L"_");

    arch = processor_arch_from_code(id->processorArchitecture);
    if (!arch)
    {
        WARN("Unrecognized architecture id %u.\n", id->processorArchitecture);
        return ERROR_INVALID_PARAMETER;
    }

    wcscat(full_name, arch);
    wcscat(full_name, L"_");

    if (id->resourceId)
    {
        len = wcslen(id->resourceId);

        if (len > PACKAGE_RESOURCEID_MAX_LENGTH)
            return ERROR_INVALID_PARAMETER;

        wcscat(full_name, id->resourceId);
    }
    wcscat(full_name, L"_");

    if (id->publisherId)
    {
        len = wcslen(id->publisherId);

        if (len != PACKAGE_PUBLISHERID_MAX_LENGTH)
            return ERROR_INVALID_PARAMETER;

        wcscat(full_name, id->publisherId);
    }
    else
    {
        BYTE hash[SYMCRYPT_SHA256_RESULT_SIZE];
        WCHAR pub_id[PACKAGE_PUBLISHERID_MAX_LENGTH + 1];

        if (!id->publisher)
            return ERROR_INVALID_PARAMETER;

        SymCryptSha256((const BYTE *)id->publisher, wcslen(id->publisher) * sizeof(WCHAR), hash);
        publisher_id_from_hash(hash, pub_id);
        wcscat(full_name, pub_id);
    }

    capacity = *length;
    req_len = wcslen(full_name) + 1;
    *length = req_len;

    if (!buffer || capacity < req_len)
        return ERROR_INSUFFICIENT_BUFFER;

    wcscpy(buffer, full_name);
    return ERROR_SUCCESS;
}

/* ---- package information from the catalog of the prefix (WineExplorer UWP U1.2) ------------------------------
 *
 * The packages that wexappx installs (or that register-wine.py registers for development) are described by records
 * in the catalog directory of the prefix: appx-catalog\PackageFullName.wexappx, UTF-16 text, "WEXAPPX 1", then lines
 * "root<TAB>path", "publisher<TAB>name", "app<TAB>Id<TAB>exe[<TAB>entry point]", "class<TAB>id<TAB>path" and
 * "dependency<TAB>PackageFullName". The identity of a process comes from its token (server); these functions only
 * answer questions about packages by name, so they read the records as information.
 */


static void *pkg_alloc( SIZE_T size ) { return HeapAlloc( GetProcessHeap(), 0, size ); }
static void *pkg_calloc( SIZE_T count, SIZE_T size ) { return HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, count * size ); }
static void *pkg_realloc( void *ptr, SIZE_T size )
{
    return ptr ? HeapReAlloc( GetProcessHeap(), 0, ptr, size ) : HeapAlloc( GetProcessHeap(), 0, size );
}
static void pkg_free( void *ptr ) { if (ptr) HeapFree( GetProcessHeap(), 0, ptr ); }
#define CATALOG_MAX_APPS 64
#define CATALOG_MAX_DEPENDENCIES 16
#define CATALOG_RECORD_MAX (256 * 1024)
#define PACKAGE_INFO_FLAGS 0x10         /* what Windows 11 reports in PACKAGE_INFO.flags for these packages */

#ifndef PACKAGE_FILTER_HEAD
#define PACKAGE_FILTER_HEAD 0x10
#define PACKAGE_FILTER_DIRECT 0x20
#endif

struct package_record
{
    WCHAR full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    WCHAR root[MAX_PATH];
    WCHAR publisher[PACKAGE_PUBLISHER_MAX_LENGTH + 1];
    unsigned int app_count, dependency_count;
    WCHAR apps[CATALOG_MAX_APPS][PACKAGE_RELATIVE_APPLICATION_ID_MAX_LENGTH + 1];
    WCHAR dependencies[CATALOG_MAX_DEPENDENCIES][PACKAGE_FULL_NAME_MAX_LENGTH + 1];
};

/* "Name_1.2.3.4_arch_resource_publisherid": printable name characters and four underscores only */
static BOOL package_full_name_is_valid( const WCHAR *name )
{
    unsigned int len = 0, underscores = 0;

    if (!name) return FALSE;
    for (; name[len]; len++)
    {
        WCHAR c = name[len];
        if (len >= PACKAGE_FULL_NAME_MAX_LENGTH) return FALSE;
        if (c == '_') underscores++;
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-'))
            return FALSE;
    }
    return len >= PACKAGE_FULL_NAME_MIN_LENGTH && underscores == 4;
}

/* "Name_publisherid": the family name of a package full name */
static BOOL package_family_from_full_name( const WCHAR *full_name, WCHAR *family, unsigned int size )
{
    const WCHAR *first = wcschr( full_name, '_' ), *last = wcsrchr( full_name, '_' );
    unsigned int name_len;

    if (!first || !last || last == first) return FALSE;
    name_len = first - full_name;
    if (name_len + 1 + wcslen( last + 1 ) + 1 > size) return FALSE;
    memcpy( family, full_name, name_len * sizeof(WCHAR) );
    family[name_len] = '_';
    wcscpy( family + name_len + 1, last + 1 );
    return TRUE;
}

static BOOL package_family_is_valid( const WCHAR *family )
{
    const WCHAR *underscore;
    unsigned int i;

    if (!family || !(underscore = wcsrchr( family, '_' )) || underscore == family) return FALSE;
    if (wcslen( underscore + 1 ) != PACKAGE_PUBLISHERID_MAX_LENGTH || wcschr( family, '_' ) != underscore) return FALSE;
    if (underscore - family < PACKAGE_NAME_MIN_LENGTH || underscore - family > PACKAGE_NAME_MAX_LENGTH) return FALSE;
    for (i = 0; i < PACKAGE_PUBLISHERID_MAX_LENGTH; i++)
    {
        WCHAR c = underscore[1 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return FALSE;
    }
    return TRUE;
}

/* the catalog directory (with a trailing backslash), or with a name the path of that record */
static BOOL catalog_path( const WCHAR *record_name, WCHAR *path, unsigned int size )
{
    WCHAR config[MAX_PATH];
    DWORD len = GetEnvironmentVariableW( L"WINECONFIGDIR", config, ARRAY_SIZE(config) );
    int ret;

    if (!len || len >= ARRAY_SIZE(config) || wcsncmp( config, L"\\??\\", 4 )) return FALSE;
    if (record_name) ret = swprintf( path, size, L"\\\\?\\%s\\appx-catalog\\%s.wexappx", config + 4, record_name );
    else ret = swprintf( path, size, L"\\\\?\\%s\\appx-catalog\\", config + 4 );
    return ret > 0;
}

static BOOL record_set( WCHAR *dest, unsigned int size, const WCHAR *src, unsigned int len )
{
    if (len >= size) return FALSE;
    memcpy( dest, src, len * sizeof(WCHAR) );
    dest[len] = 0;
    return TRUE;
}

/* ERROR_SUCCESS, ERROR_NOT_FOUND for an unknown package, ERROR_INVALID_PARAMETER for a malformed name */
static LONG package_record_load( const WCHAR *full_name, struct package_record *rec )
{
    WCHAR path[MAX_PATH], *text, *line, *next, *end;
    HANDLE file;
    DWORD bytes = 0;
    BOOL first = TRUE, have_root = FALSE;

    if (!package_full_name_is_valid( full_name )) return ERROR_INVALID_PARAMETER;
    if (!catalog_path( full_name, path, ARRAY_SIZE(path) )) return ERROR_NOT_FOUND;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return ERROR_NOT_FOUND;
    if (!(text = pkg_alloc( CATALOG_RECORD_MAX + sizeof(WCHAR) )))
    {
        CloseHandle( file );
        return ERROR_OUTOFMEMORY;
    }
    if (!ReadFile( file, text, CATALOG_RECORD_MAX, &bytes, NULL )) bytes = 0;
    CloseHandle( file );
    text[bytes / sizeof(WCHAR)] = 0;

    memset( rec, 0, sizeof(*rec) );
    wcscpy( rec->full_name, full_name );
    for (line = text; *line; line = next)
    {
        WCHAR *fields[4] = { 0 };
        unsigned int count = 0;

        for (end = line; *end && *end != '\n'; end++) ;
        next = *end ? end + 1 : end;
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;

        if (first)
        {
            first = FALSE;
            if (wcscmp( line, L"WEXAPPX 1" )) break;
            continue;
        }
        for (fields[count++] = line; count < ARRAY_SIZE(fields); )
        {
            WCHAR *tab = wcschr( fields[count - 1], '\t' );
            if (!tab) break;
            *tab = 0;
            fields[count++] = tab + 1;
        }
        if (!wcscmp( fields[0], L"root" ) && count >= 2)
            have_root = record_set( rec->root, ARRAY_SIZE(rec->root), fields[1], wcslen( fields[1] ) );
        else if (!wcscmp( fields[0], L"publisher" ) && count >= 2)
            record_set( rec->publisher, ARRAY_SIZE(rec->publisher), fields[1], wcslen( fields[1] ) );
        else if (!wcscmp( fields[0], L"app" ) && count >= 2 && rec->app_count < CATALOG_MAX_APPS)
        {
            if (record_set( rec->apps[rec->app_count], ARRAY_SIZE(rec->apps[0]), fields[1], wcslen( fields[1] ) ))
                rec->app_count++;
        }
        else if (!wcscmp( fields[0], L"dependency" ) && count >= 2 && rec->dependency_count < CATALOG_MAX_DEPENDENCIES)
        {
            if (package_full_name_is_valid( fields[1] ) &&
                record_set( rec->dependencies[rec->dependency_count], ARRAY_SIZE(rec->dependencies[0]), fields[1],
                            wcslen( fields[1] ) ))
                rec->dependency_count++;
        }
    }
    pkg_free( text );
    return have_root ? ERROR_SUCCESS : ERROR_NOT_FOUND;
}

/* the full names in the catalog of a family; the array is allocated, the caller frees it */
static WCHAR (*catalog_family_members( const WCHAR *family, unsigned int *count ))[PACKAGE_FULL_NAME_MAX_LENGTH + 1]
{
    WCHAR (*names)[PACKAGE_FULL_NAME_MAX_LENGTH + 1] = NULL;
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], member_family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    WIN32_FIND_DATAW data;
    HANDLE find;
    unsigned int capacity = 0;

    *count = 0;
    if (!catalog_path( NULL, dir, ARRAY_SIZE(dir) ) || swprintf( pattern, ARRAY_SIZE(pattern), L"%s*.wexappx", dir ) < 0)
        return NULL;
    if ((find = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE) return NULL;
    do
    {
        unsigned int len = wcslen( data.cFileName ) - wcslen( L".wexappx" );
        WCHAR name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];

        if (len > PACKAGE_FULL_NAME_MAX_LENGTH) continue;
        memcpy( name, data.cFileName, len * sizeof(WCHAR) );
        name[len] = 0;
        if (!package_full_name_is_valid( name ) ||
            !package_family_from_full_name( name, member_family, ARRAY_SIZE(member_family) ) ||
            wcsicmp( member_family, family ))
            continue;
        if (*count == capacity)
        {
            void *tmp = pkg_realloc( names, (capacity = capacity ? capacity * 2 : 8) * sizeof(*names) );
            if (!tmp) break;
            names = tmp;
        }
        wcscpy( names[(*count)++], name );
    }
    while (FindNextFileW( find, &data ));
    FindClose( find );
    return names;
}

/* the strings of one PACKAGE_INFO block; with base NULL only the size is computed */
static WCHAR *pool_add( BYTE *base, SIZE_T *used, const WCHAR *str )
{
    SIZE_T bytes = (wcslen( str ) + 1) * sizeof(WCHAR);
    WCHAR *dest = base ? (WCHAR *)(base + *used) : NULL;

    if (dest) memcpy( dest, str, bytes );
    *used += bytes;
    return dest;
}

/* PACKAGE_INFO array for the given records: the structures, then their strings */
static LONG package_infos_build( struct package_record **records, unsigned int count, UINT32 *length, BYTE *buffer,
                                 UINT32 *out_count )
{
    SIZE_T used, total;
    unsigned int i, pass;
    PACKAGE_INFO *infos = (PACKAGE_INFO *)buffer;

    for (pass = 0; pass < 2; pass++)
    {
        BYTE *base = pass ? buffer : NULL;

        used = count * sizeof(PACKAGE_INFO);
        for (i = 0; i < count; i++)
        {
            BYTE id_buffer[sizeof(PACKAGE_ID) + sizeof(WCHAR) * (PACKAGE_FULL_NAME_MAX_LENGTH + 32)];
            UINT32 id_len = sizeof(id_buffer);
            PACKAGE_ID *id = (PACKAGE_ID *)id_buffer;
            WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
            PACKAGE_INFO *info = infos + i;

            if (PackageIdFromFullName( records[i]->full_name, 0, &id_len, id_buffer ) ||
                !package_family_from_full_name( records[i]->full_name, family, ARRAY_SIZE(family) ))
                return ERROR_INVALID_DATA;
            if (pass) memset( info, 0, sizeof(*info) );
            if (pass) info->flags = PACKAGE_INFO_FLAGS;
            if (pass) info->packageId.processorArchitecture = id->processorArchitecture;
            if (pass) info->packageId.version = id->version;
            if (pass) info->path = pool_add( base, &used, records[i]->root ); else pool_add( NULL, &used, records[i]->root );
            if (pass) info->packageFullName = pool_add( base, &used, records[i]->full_name ); else pool_add( NULL, &used, records[i]->full_name );
            if (pass) info->packageFamilyName = pool_add( base, &used, family ); else pool_add( NULL, &used, family );
            if (pass) info->packageId.name = pool_add( base, &used, id->name ); else pool_add( NULL, &used, id->name );
            if (records[i]->publisher[0])
            {
                if (pass) info->packageId.publisher = pool_add( base, &used, records[i]->publisher );
                else pool_add( NULL, &used, records[i]->publisher );
            }
            if (id->resourceId && id->resourceId[0])
            {
                if (pass) info->packageId.resourceId = pool_add( base, &used, id->resourceId );
                else pool_add( NULL, &used, id->resourceId );
            }
            if (pass) info->packageId.publisherId = pool_add( base, &used, id->publisherId ); else pool_add( NULL, &used, id->publisherId );
        }
        if (!pass)
        {
            total = used;
            *out_count = count;
            if (*length < total || !buffer)
            {
                *length = total;
                return ERROR_INSUFFICIENT_BUFFER;
            }
            *length = total;
        }
    }
    return ERROR_SUCCESS;
}

/* the records a PACKAGE_FILTER_* selection of a package covers: the head, then its direct dependencies */
static LONG package_info_select( const struct package_record *head, UINT32 flags, UINT32 *length, BYTE *buffer,
                                 UINT32 *count )
{
    struct package_record **records = NULL;
    unsigned int n = 0, i;
    LONG err = ERROR_SUCCESS;
    BOOL include_head = (flags & PACKAGE_FILTER_HEAD) || !(flags & PACKAGE_FILTER_DIRECT);
    BOOL include_direct = !!(flags & PACKAGE_FILTER_DIRECT);

    if (!length || !count || (!buffer && *length)) return ERROR_INVALID_PARAMETER;
    if (!(records = pkg_calloc( 1 + CATALOG_MAX_DEPENDENCIES, sizeof(*records) ))) return ERROR_OUTOFMEMORY;
    if (include_head && !(records[n] = pkg_alloc( sizeof(**records) ))) { err = ERROR_OUTOFMEMORY; goto done; }
    if (include_head) *records[n++] = *head;
    for (i = 0; include_direct && i < head->dependency_count; i++)
    {
        if (!(records[n] = pkg_alloc( sizeof(**records) ))) { err = ERROR_OUTOFMEMORY; goto done; }
        if (package_record_load( head->dependencies[i], records[n] )) { pkg_free( records[n] ); records[n] = NULL; continue; }
        n++;
    }
    err = n ? package_infos_build( records, n, length, buffer, count ) : ERROR_SUCCESS;
    if (!n) *length = *count = 0;
done:
    for (i = 0; i < 1 + CATALOG_MAX_DEPENDENCIES; i++) pkg_free( records[i] );
    pkg_free( records );
    return err;
}

/***********************************************************************
 *         OpenPackageInfoByFullName   (kernelbase.@)
 */
LONG WINAPI OpenPackageInfoByFullName( const WCHAR *full_name, UINT32 reserved, PACKAGE_INFO_REFERENCE *reference )
{
    struct package_record *rec;
    LONG err;

    TRACE( "(%s %#x %p)\n", debugstr_w( full_name ), reserved, reference );

    if (!full_name || !reference) return ERROR_INVALID_PARAMETER;
    if (!(rec = pkg_alloc( sizeof(*rec) ))) return ERROR_OUTOFMEMORY;
    if ((err = package_record_load( full_name, rec )))
    {
        pkg_free( rec );
        return err;
    }
    *reference = (PACKAGE_INFO_REFERENCE)rec;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         OpenPackageInfoByFullNameForUser   (kernelbase.@)
 */
LONG WINAPI OpenPackageInfoByFullNameForUser( PSID user, const WCHAR *full_name, UINT32 reserved,
                                              PACKAGE_INFO_REFERENCE *reference )
{
    return OpenPackageInfoByFullName( full_name, reserved, reference );
}

/***********************************************************************
 *         ClosePackageInfo   (kernelbase.@)
 */
LONG WINAPI ClosePackageInfo( PACKAGE_INFO_REFERENCE reference )
{
    TRACE( "(%p)\n", reference );

    if (!reference) return ERROR_INVALID_PARAMETER;
    pkg_free( reference );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetPackageInfo   (kernelbase.@)
 */
LONG WINAPI GetPackageInfo( PACKAGE_INFO_REFERENCE reference, UINT32 flags, UINT32 *length, BYTE *buffer,
                            UINT32 *count )
{
    TRACE( "(%p %#x %p %p %p)\n", reference, flags, length, buffer, count );

    if (!reference) return ERROR_INVALID_PARAMETER;
    return package_info_select( (struct package_record *)reference, flags, length, buffer, count );
}

/***********************************************************************
 *         GetPackageApplicationIds   (kernelbase.@)
 */
LONG WINAPI GetPackageApplicationIds( PACKAGE_INFO_REFERENCE reference, UINT32 *length, BYTE *buffer, UINT32 *count )
{
    struct package_record *rec = (struct package_record *)reference;
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    SIZE_T used;
    unsigned int i;

    TRACE( "(%p %p %p %p)\n", reference, length, buffer, count );

    if (!reference || !length || !count || (!buffer && *length)) return ERROR_INVALID_PARAMETER;
    if (!package_family_from_full_name( rec->full_name, family, ARRAY_SIZE(family) )) return ERROR_INVALID_DATA;

    /* an array of pointers, then the "Family!Id" strings */
    used = rec->app_count * sizeof(WCHAR *);
    for (i = 0; i < rec->app_count; i++) used += (wcslen( family ) + 1 + wcslen( rec->apps[i] ) + 1) * sizeof(WCHAR);
    *count = rec->app_count;
    if (*length < used || !buffer)
    {
        *length = used;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    *length = used;
    {
        WCHAR **ids = (WCHAR **)buffer;
        WCHAR *text = (WCHAR *)(buffer + rec->app_count * sizeof(WCHAR *));

        for (i = 0; i < rec->app_count; i++)
        {
            ids[i] = text;
            text += swprintf( text, wcslen( family ) + 1 + wcslen( rec->apps[i] ) + 1, L"%s!%s", family, rec->apps[i] ) + 1;
        }
    }
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetPackagePathByFullName   (kernelbase.@)
 */
LONG WINAPI GetPackagePathByFullName( const WCHAR *name, UINT32 *len, WCHAR *path )
{
    struct package_record *rec;
    UINT32 capacity, needed;
    LONG err;

    TRACE( "(%s %p %p)\n", debugstr_w( name ), len, path );

    if (!len || !name || (!path && *len)) return ERROR_INVALID_PARAMETER;
    if (!(rec = pkg_alloc( sizeof(*rec) ))) return ERROR_OUTOFMEMORY;
    if ((err = package_record_load( name, rec )))
    {
        pkg_free( rec );
        return err;
    }
    capacity = *len;
    needed = wcslen( rec->root ) + 1;
    *len = needed;
    if (capacity < needed || !path) err = ERROR_INSUFFICIENT_BUFFER;
    else wcscpy( path, rec->root );
    pkg_free( rec );
    return err;
}

/***********************************************************************
 *         GetStagedPackagePathByFullName   (kernelbase.@)
 */
LONG WINAPI GetStagedPackagePathByFullName( const WCHAR *name, UINT32 *len, WCHAR *path )
{
    return GetPackagePathByFullName( name, len, path );
}

/* the full names of a family: GetPackagesByPackageFamily and FindPackagesByPackageFamily */
static LONG package_family_names( const WCHAR *family, UINT32 *count, WCHAR **full_names, UINT32 *buffer_len,
                                  WCHAR *buffer, UINT32 *properties )
{
    WCHAR (*members)[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    unsigned int n, i;
    UINT32 chars = 0, capacity_count, capacity_chars;
    LONG err = ERROR_SUCCESS;

    if (!count || !buffer_len || !package_family_is_valid( family )) return ERROR_INVALID_PARAMETER;
    capacity_count = *count;
    capacity_chars = *buffer_len;
    if ((capacity_count && !full_names) || (capacity_chars && !buffer)) return ERROR_INVALID_PARAMETER;

    members = catalog_family_members( family, &n );
    for (i = 0; i < n; i++) chars += wcslen( members[i] ) + 1;
    *count = n;
    *buffer_len = chars;
    if (n && (capacity_count < n || capacity_chars < chars)) err = ERROR_INSUFFICIENT_BUFFER;
    else
    {
        WCHAR *text = buffer;

        for (i = 0; i < n; i++)
        {
            full_names[i] = text;
            wcscpy( text, members[i] );
            text += wcslen( members[i] ) + 1;
            if (properties) properties[i] = PACKAGE_INFO_FLAGS;
        }
    }
    pkg_free( members );
    return err;
}

/***********************************************************************
 *         GetPackagesByPackageFamily   (kernelbase.@)
 */
LONG WINAPI DECLSPEC_HOTPATCH GetPackagesByPackageFamily( const WCHAR *family_name, UINT32 *count, WCHAR **full_names,
                                                          UINT32 *buffer_len, WCHAR *buffer )
{
    TRACE( "(%s %p %p %p %p)\n", debugstr_w( family_name ), count, full_names, buffer_len, buffer );
    return package_family_names( family_name, count, full_names, buffer_len, buffer, NULL );
}

/***********************************************************************
 *         FindPackagesByPackageFamily   (kernelbase.@)
 */
LONG WINAPI FindPackagesByPackageFamily( const WCHAR *family_name, UINT32 flags, UINT32 *count, WCHAR **full_names,
                                         UINT32 *buffer_len, WCHAR *buffer, UINT32 *properties )
{
    TRACE( "(%s %#x %p %p %p %p %p)\n", debugstr_w( family_name ), flags, count, full_names, buffer_len, buffer,
           properties );
    return package_family_names( family_name, count, full_names, buffer_len, buffer, properties );
}

/***********************************************************************
 *         PackageFamilyNameFromId   (kernelbase.@)
 */
LONG WINAPI PackageFamilyNameFromId( const PACKAGE_ID *id, UINT32 *length, WCHAR *buffer )
{
    WCHAR full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1], family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 name_len = ARRAY_SIZE(full_name), capacity, needed;
    LONG err;

    TRACE( "(%p %p %p)\n", id, length, buffer );

    if (!id || !length || (!buffer && *length)) return ERROR_INVALID_PARAMETER;
    if ((err = PackageFullNameFromId( id, &name_len, full_name ))) return err;
    if (!package_family_from_full_name( full_name, family, ARRAY_SIZE(family) )) return ERROR_INVALID_PARAMETER;
    capacity = *length;
    needed = wcslen( family ) + 1;
    *length = needed;
    if (!buffer || capacity < needed) return ERROR_INSUFFICIENT_BUFFER;
    wcscpy( buffer, family );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         PackageNameAndPublisherIdFromFamilyName   (kernelbase.@)
 */
LONG WINAPI PackageNameAndPublisherIdFromFamilyName( const WCHAR *family, UINT32 *name_length, WCHAR *name,
                                                     UINT32 *publisher_id_length, WCHAR *publisher_id )
{
    const WCHAR *underscore;
    UINT32 name_capacity, id_capacity, name_needed, id_needed;
    BOOL small;

    TRACE( "(%s %p %p %p %p)\n", debugstr_w( family ), name_length, name, publisher_id_length, publisher_id );

    if (!name_length || !publisher_id_length || (!name && *name_length) || (!publisher_id && *publisher_id_length) ||
        !package_family_is_valid( family ))
        return ERROR_INVALID_PARAMETER;
    underscore = wcsrchr( family, '_' );
    name_needed = underscore - family + 1;
    id_needed = wcslen( underscore + 1 ) + 1;
    name_capacity = *name_length;
    id_capacity = *publisher_id_length;
    *name_length = name_needed;
    *publisher_id_length = id_needed;
    small = !name || !publisher_id || name_capacity < name_needed || id_capacity < id_needed;
    if (small) return ERROR_INSUFFICIENT_BUFFER;
    memcpy( name, family, (name_needed - 1) * sizeof(WCHAR) );
    name[name_needed - 1] = 0;
    wcscpy( publisher_id, underscore + 1 );
    return ERROR_SUCCESS;
}

/* "Family!Id": the relative application id may not be empty and the family has to be valid */
static BOOL aumid_split( const WCHAR *aumid, WCHAR *family, UINT32 *family_len, const WCHAR **app )
{
    const WCHAR *bang;

    if (!aumid || !(bang = wcschr( aumid, '!' )) || bang == aumid || !bang[1] || (size_t)(bang - aumid) > PACKAGE_FAMILY_NAME_MAX_LENGTH)
        return FALSE;
    memcpy( family, aumid, (bang - aumid) * sizeof(WCHAR) );
    family[bang - aumid] = 0;
    *family_len = bang - aumid;
    *app = bang + 1;
    return package_family_is_valid( family ) && wcslen( *app ) < PACKAGE_RELATIVE_APPLICATION_ID_MAX_LENGTH;
}

/***********************************************************************
 *         ParseApplicationUserModelId   (kernelbase.@)
 */
LONG WINAPI ParseApplicationUserModelId( const WCHAR *aumid, UINT32 *family_length, WCHAR *family, UINT32 *app_length,
                                         WCHAR *app )
{
    WCHAR parsed[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    const WCHAR *app_id;
    UINT32 len, family_capacity, app_capacity;

    TRACE( "(%s %p %p %p %p)\n", debugstr_w( aumid ), family_length, family, app_length, app );

    if (!family_length || !app_length || (!family && *family_length) || (!app && *app_length) ||
        !aumid_split( aumid, parsed, &len, &app_id ))
        return ERROR_INVALID_PARAMETER;
    family_capacity = *family_length;
    app_capacity = *app_length;
    *family_length = len + 1;
    *app_length = wcslen( app_id ) + 1;
    if (!family || !app || family_capacity < *family_length || app_capacity < *app_length) return ERROR_INSUFFICIENT_BUFFER;
    wcscpy( family, parsed );
    wcscpy( app, app_id );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetApplicationUserModelId   (kernelbase.@)
 */
LONG WINAPI GetApplicationUserModelId( HANDLE process, UINT32 *length, WCHAR *aumid )
{
    LONG err;

    TRACE( "(%p %p %p)\n", process, length, aumid );

    if (!process || !length || (!aumid && *length)) return ERROR_INVALID_PARAMETER;
    err = get_process_package_string( process, PACKAGE_STRING_AUMID, length, aumid );
    return err == APPMODEL_ERROR_NO_PACKAGE ? APPMODEL_ERROR_NO_APPLICATION : err;
}

/***********************************************************************
 *         GetCurrentPackageInfo   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageInfo( const UINT32 flags, UINT32 *buffer_size, BYTE *buffer, UINT32 *count )
{
    WCHAR full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    UINT32 name_len = ARRAY_SIZE(full_name), dummy;
    struct package_record *rec;
    LONG err;

    TRACE( "(%#x %p %p %p)\n", flags, buffer_size, buffer, count );

    if (!buffer_size || (!buffer && *buffer_size) || (buffer && !count)) return ERROR_INVALID_PARAMETER;
    if ((err = get_process_package_string( GetCurrentProcess(), PACKAGE_STRING_FULL_NAME, &name_len, full_name )))
        return err;
    if (!(rec = pkg_alloc( sizeof(*rec) ))) return ERROR_OUTOFMEMORY;
    if (!(err = package_record_load( full_name, rec )))
        err = package_info_select( rec, flags, buffer_size, buffer, count ? count : &dummy );
    pkg_free( rec );
    return err;
}
