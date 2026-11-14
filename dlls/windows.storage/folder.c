/* WinRT Windows.Storage.StorageFolder and Windows.Storage.StorageFile implementation
 *
 * Copyright (C) 2026
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

/* A storage item is a path. The behaviour follows what Windows 11 did with the probe of WineExplorer
 * (tests/uwp/u1_1 in that repository): names that are relative paths, "name (2).ext" for unique names, the
 * error codes. File streams (OpenAsync), thumbnails and the extra properties are not implemented. */

#include <assert.h>
#include <stdlib.h>
#include "private.h"
#include "objbase.h"
#include "winreg.h"
#include "winnls.h"
#include "appmodel.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

#define HRESULT_NO_PACKAGE ((HRESULT)0x80073d54) /* the process has no package identity */

enum item_kind { ITEM_ANY, ITEM_FILE, ITEM_FOLDER };

struct storage_item
{
    IStorageItem IStorageItem_iface; /* also the IUnknown of the object */
    IStorageFile IStorageFile_iface;
    IStorageFolder IStorageFolder_iface;
    IStorageFolder2 IStorageFolder2_iface;
    IStorageItemProperties IStorageItemProperties_iface;
    IStorageFilePropertiesWithAvailability IStorageFilePropertiesWithAvailability_iface;
    LONG ref;

    BOOL folder;
    SRWLOCK lock;
    WCHAR *path; /* changes when the item is renamed or moved */
};

static struct storage_item *item_create( const WCHAR *path, BOOL folder );

/* ---- paths ------------------------------------------------------------------------------------------ */

static HRESULT hresult_from_error( DWORD error )
{
    if (error == ERROR_FILE_EXISTS) error = ERROR_ALREADY_EXISTS;
    return HRESULT_FROM_WIN32( error );
}

static HRESULT last_error(void)
{
    return hresult_from_error( GetLastError() );
}

static WCHAR *path_join( const WCHAR *folder, const WCHAR *name )
{
    size_t folder_len = wcslen( folder ), name_len = wcslen( name );
    WCHAR *path;

    while (folder_len && (folder[folder_len - 1] == '\\' || folder[folder_len - 1] == '/')) folder_len--;
    if (!(path = malloc( (folder_len + name_len + 2) * sizeof(WCHAR) ))) return NULL;
    memcpy( path, folder, folder_len * sizeof(WCHAR) );
    path[folder_len] = '\\';
    memcpy( path + folder_len + 1, name, (name_len + 1) * sizeof(WCHAR) );
    for (name_len = folder_len; path[name_len]; name_len++) if (path[name_len] == '/') path[name_len] = '\\';
    return path;
}

/* the last component of a path (the whole path for a drive root) */
static const WCHAR *path_name( const WCHAR *path )
{
    const WCHAR *end = path + wcslen( path ), *name;

    while (end > path && (end[-1] == '\\' || end[-1] == '/')) end--;
    name = end;
    while (name > path && name[-1] != '\\' && name[-1] != '/') name--;
    return name == end ? path : name;
}

static WCHAR *path_parent( const WCHAR *path )
{
    const WCHAR *name = path_name( path );
    size_t len = name - path;
    WCHAR *parent;

    while (len > 3 && (path[len - 1] == '\\' || path[len - 1] == '/')) len--;
    if (!(parent = malloc( (len + 1) * sizeof(WCHAR) ))) return NULL;
    memcpy( parent, path, len * sizeof(WCHAR) );
    parent[len] = 0;
    return parent;
}

static const WCHAR *path_extension( const WCHAR *path )
{
    const WCHAR *name = path_name( path ), *dot = wcsrchr( name, '.' );
    return dot ? dot : name + wcslen( name );
}

/* A name given to a folder may be a relative path (Windows creates the folders in between), but it may not leave the
 * folder. */
static BOOL valid_relative_name( const WCHAR *name, BOOL allow_separators )
{
    const WCHAR *p, *component = name;

    if (!name || !name[0]) return FALSE;
    if (name[0] == '\\' || name[0] == '/') return FALSE;
    for (p = name; ; p++)
    {
        if (*p == '\\' || *p == '/' || !*p)
        {
            size_t len = p - component;
            if (*p && !allow_separators) return FALSE;
            if (!len) return FALSE;
            if (len == 2 && component[0] == '.' && component[1] == '.') return FALSE;
            if (len == 1 && component[0] == '.') return FALSE;
            if (!*p) break;
            component = p + 1;
        }
        else if (*p < ' ' || wcschr( L":*?\"<>|", *p )) return FALSE;
    }
    return TRUE;
}

/* An absolute drive path (letter, colon, backslash) or a UNC path with a server and a share. */
static HRESULT validate_absolute_path( const WCHAR *path, UINT32 length )
{
    UINT32 i;

    if (!length) return E_INVALIDARG;
    for (i = 0; i < length; i++) if (!path[i]) return E_INVALIDARG;

    if ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
    {
        if (length < 3 || path[1] != ':' || path[2] != '\\') return E_INVALIDARG;
    }
    else if (length >= 2 && path[0] == '\\' && path[1] == '\\')
    {
        /* no device namespaces (\\.\, \\?\), no third backslash, a server and a share */
        if (length < 3) return E_INVALIDARG;
        if (path[2] == '.' || path[2] == '?' || path[2] == '\\') return E_INVALIDARG;
        for (i = 2; i < length; i++) if (path[i] == '\\') break;
        if (i >= length || i + 1 >= length || path[i + 1] == '\\') return E_INVALIDARG;
    }
    else return E_INVALIDARG;

    if (length > MAX_PATH) return E_INVALIDARG;
    return S_OK;
}

/* create the folders of a path, up to the one that contains the last component */
static DWORD create_parents( const WCHAR *path, size_t fixed )
{
    WCHAR *copy = wcsdup( path );
    size_t i;
    DWORD error = 0;

    if (!copy) return ERROR_OUTOFMEMORY;
    for (i = fixed + 1; copy[i] && !error; i++)
    {
        if (copy[i] != '\\') continue;
        copy[i] = 0;
        if (!CreateDirectoryW( copy, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) error = GetLastError();
        copy[i] = '\\';
    }
    free( copy );
    return error;
}

/* "name (N).ext" in the folder, for the first N from 2 that is free */
static WCHAR *unique_path( const WCHAR *path, BOOL folder )
{
    const WCHAR *ext = folder ? path + wcslen( path ) : path_extension( path );
    size_t stem = ext - path, size = wcslen( path ) + 16;
    WCHAR *candidate;
    unsigned int n;

    if (!(candidate = malloc( size * sizeof(WCHAR) ))) return NULL;
    for (n = 2; n < 100000; n++)
    {
        swprintf( candidate, size, L"%.*s (%u)%s", (int)stem, path, n, ext );
        if (GetFileAttributesW( candidate ) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    free( candidate );
    return NULL;
}

static DWORD delete_tree( const WCHAR *path )
{
    WIN32_FIND_DATAW data;
    WCHAR *pattern, *child;
    DWORD error = 0;
    HANDLE find;

    if (!(pattern = path_join( path, L"*" ))) return ERROR_OUTOFMEMORY;
    find = FindFirstFileW( pattern, &data );
    free( pattern );
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
            if (!(child = path_join( path, data.cFileName ))) { error = ERROR_OUTOFMEMORY; break; }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                error = delete_tree( child );
            else
            {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW( child, FILE_ATTRIBUTE_NORMAL );
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!RemoveDirectoryW( child )) error = GetLastError(); }
                else if (!DeleteFileW( child )) error = GetLastError();
            }
            free( child );
        } while (!error && FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (!error && !RemoveDirectoryW( path )) error = GetLastError();
    return error;
}

/* ---- application URIs --------------------------------------------------------------------------------- */

static HRESULT package_folder( const WCHAR *leaf, WCHAR **out )
{
    WCHAR family[256], local[MAX_PATH], path[MAX_PATH * 2];
    UINT32 length = ARRAY_SIZE(family);

    if (GetCurrentPackageFamilyName( &length, family )) return HRESULT_NO_PACKAGE;
    if (!leaf)
    {
        length = ARRAY_SIZE(path);
        if (GetCurrentPackagePath( &length, path )) return HRESULT_NO_PACKAGE;
    }
    else
    {
        if (!GetEnvironmentVariableW( L"LOCALAPPDATA", local, ARRAY_SIZE(local) )) return E_FAIL;
        swprintf( path, ARRAY_SIZE(path), L"%s\\Packages\\%s\\%s", local, family, leaf );
    }
    return (*out = wcsdup( path )) ? S_OK : E_OUTOFMEMORY;
}

static int hex_digit( WCHAR c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ms-appdata:///local/..., ms-appdata:///roaming/..., ms-appdata:///temp/... and ms-appx:///... */
static HRESULT resolve_application_uri( const WCHAR *uri, WCHAR **out )
{
    static const struct { const WCHAR *segment; const WCHAR *leaf; } folders[] =
    {
        { L"local", L"LocalState" }, { L"roaming", L"RoamingState" }, { L"temp", L"TempState" },
    };
    BOOL appdata = !wcsnicmp( uri, L"ms-appdata:", 11 );
    const WCHAR *p = uri + (appdata ? 11 : 8), *leaf = NULL;
    WCHAR *rest, *base, *dst;
    char utf8[MAX_PATH * 3];
    size_t i, count = 0;
    HRESULT hr;

    *out = NULL;
    if (p[0] == '/' && p[1] == '/')
    {
        p += 2;
        while (*p && *p != '/') p++; /* the package name, or nothing */
    }
    if (*p != '/') return E_INVALIDARG;
    p++;

    /* percent escapes are UTF-8 bytes */
    for (i = 0; p[i] && p[i] != '?' && p[i] != '#'; i++)
    {
        int high, low;
        if (count >= sizeof(utf8) - 4) return E_INVALIDARG;
        if (p[i] == '%' && (high = hex_digit( p[i + 1] )) >= 0 && (low = hex_digit( p[i + 2] )) >= 0)
        {
            utf8[count++] = high * 16 + low;
            i += 2;
        }
        else count += WideCharToMultiByte( CP_UTF8, 0, p + i, 1, utf8 + count, 4, NULL, NULL );
    }
    if (!(rest = malloc( (count + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    rest[MultiByteToWideChar( CP_UTF8, 0, utf8, count, rest, count )] = 0;
    for (dst = rest; *dst; dst++) if (*dst == '/') *dst = '\\';

    dst = rest;
    if (appdata)
    {
        for (i = 0; i < ARRAY_SIZE(folders); i++)
        {
            size_t len = wcslen( folders[i].segment );
            if (wcsnicmp( rest, folders[i].segment, len ) || (rest[len] && rest[len] != '\\')) continue;
            leaf = folders[i].leaf;
            dst = rest + len + (rest[len] ? 1 : 0);
            break;
        }
        if (!leaf) { free( rest ); return E_INVALIDARG; }
    }
    if (*dst && !valid_relative_name( dst, TRUE )) { free( rest ); return E_INVALIDARG; }

    if (SUCCEEDED(hr = package_folder( leaf, &base )))
    {
        *out = *dst ? path_join( base, dst ) : wcsdup( base );
        free( base );
        if (!*out) hr = E_OUTOFMEMORY;
    }
    free( rest );
    return hr;
}

/* an absolute path or an application URI, as the path functions of Windows.Storage take them */
HRESULT storage_resolve_path( const WCHAR *path, WCHAR **out )
{
    HRESULT hr;

    *out = NULL;
    if (!wcsnicmp( path, L"ms-appdata:", 11 ) || !wcsnicmp( path, L"ms-appx:", 8 )) return resolve_application_uri( path, out );
    if (FAILED(hr = validate_absolute_path( path, wcslen( path ) ))) return hr;
    return (*out = wcsdup( path )) ? S_OK : E_OUTOFMEMORY;
}

/* ---- calls -------------------------------------------------------------------------------------------- */

static inline struct storage_call *storage_call_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct storage_call, IUnknown_iface );
}

static HRESULT WINAPI storage_call_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualIID( iid, &IID_IUnknown ))
    {
        IUnknown_AddRef( iface );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI storage_call_AddRef( IUnknown *iface )
{
    return InterlockedIncrement( &storage_call_from_IUnknown( iface )->ref );
}

static ULONG WINAPI storage_call_Release( IUnknown *iface )
{
    struct storage_call *call = storage_call_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &call->ref );

    if (!ref)
    {
        if (call->object) IInspectable_Release( call->object );
        if (call->other) IInspectable_Release( call->other );
        free( call->text );
        free( call->contents );
        free( call->bytes );
        free( call );
    }
    return ref;
}

static const IUnknownVtbl storage_call_vtbl =
{
    storage_call_QueryInterface,
    storage_call_AddRef,
    storage_call_Release,
};

struct storage_call *storage_call_create( HRESULT (*run)( struct storage_call *call, PROPVARIANT *result ),
                                          void *object, void *other, HSTRING text, UINT32 option )
{
    struct storage_call *call;
    const WCHAR *buffer;
    UINT32 length;

    if (!(call = calloc( 1, sizeof(*call) ))) return NULL;
    call->IUnknown_iface.lpVtbl = &storage_call_vtbl;
    call->ref = 1;
    call->run = run;
    call->option = option;

    buffer = WindowsGetStringRawBuffer( text, &length );
    if (!(call->text = malloc( (length + 1) * sizeof(WCHAR) )))
    {
        free( call );
        return NULL;
    }
    memcpy( call->text, buffer, length * sizeof(WCHAR) );
    call->text[length] = 0;

    if ((call->object = object)) IInspectable_AddRef( call->object );
    if ((call->other = other)) IInspectable_AddRef( call->other );
    return call;
}

HRESULT storage_call_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct storage_call *call = storage_call_from_IUnknown( param );
    return call->run( call, result );
}

#define START_CALL( create, call, operation )                                                      \
    do                                                                                             \
    {                                                                                              \
        struct storage_call *call_ = (call);                                                       \
        HRESULT hr_;                                                                               \
        if (!call_) return E_OUTOFMEMORY;                                                          \
        hr_ = create( NULL, &call_->IUnknown_iface, storage_call_async, operation );               \
        IUnknown_Release( &call_->IUnknown_iface );                                                \
        return hr_;                                                                                \
    } while (0)

static inline struct storage_item *item_from_call( struct storage_call *call )
{
    return CONTAINING_RECORD( (IStorageItem *)call->object, struct storage_item, IStorageItem_iface );
}

static void set_result( PROPVARIANT *result, void *iface )
{
    result->vt = VT_UNKNOWN;
    result->punkVal = iface;
}

/* the path of any storage item, also one that is not ours */
HRESULT storage_item_get_path( void *object, WCHAR **path )
{
    IStorageItem *item;
    HSTRING string;
    HRESULT hr;

    *path = NULL;
    if (!object) return E_INVALIDARG;
    if (FAILED(hr = IInspectable_QueryInterface( (IInspectable *)object, &IID_IStorageItem, (void **)&item ))) return hr;
    hr = IStorageItem_get_Path( item, &string );
    IStorageItem_Release( item );
    if (FAILED(hr)) return hr;
    if (!WindowsGetStringLen( string )) hr = E_INVALIDARG;
    else if (!(*path = wcsdup( WindowsGetStringRawBuffer( string, NULL ) ))) hr = E_OUTOFMEMORY;
    WindowsDeleteString( string );
    return hr;
}

static WCHAR *item_path( struct storage_item *impl )
{
    WCHAR *path;

    AcquireSRWLockShared( &impl->lock );
    path = wcsdup( impl->path );
    ReleaseSRWLockShared( &impl->lock );
    return path;
}

static void item_set_path( struct storage_item *impl, WCHAR *path )
{
    WCHAR *old;

    AcquireSRWLockExclusive( &impl->lock );
    old = impl->path;
    impl->path = path;
    ReleaseSRWLockExclusive( &impl->lock );
    free( old );
}

static HRESULT item_open( const WCHAR *path, enum item_kind kind, struct storage_item **out )
{
    DWORD attributes = GetFileAttributesW( path );
    BOOL folder;

    *out = NULL;
    if (attributes == INVALID_FILE_ATTRIBUTES) return last_error();
    folder = !!(attributes & FILE_ATTRIBUTE_DIRECTORY);
    if ((kind == ITEM_FILE && folder) || (kind == ITEM_FOLDER && !folder)) return E_INVALIDARG;
    return (*out = item_create( path, folder )) ? S_OK : E_OUTOFMEMORY;
}

/* ---- BasicProperties ---------------------------------------------------------------------------------- */

struct basic_properties
{
    IBasicProperties IBasicProperties_iface;
    LONG ref;
    UINT64 size;
    DateTime modified;
};

static inline struct basic_properties *impl_from_IBasicProperties( IBasicProperties *iface )
{
    return CONTAINING_RECORD( iface, struct basic_properties, IBasicProperties_iface );
}

static HRESULT WINAPI basic_properties_QueryInterface( IBasicProperties *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IBasicProperties ))
    {
        IBasicProperties_AddRef( (*out = iface) );
        return S_OK;
    }
    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI basic_properties_AddRef( IBasicProperties *iface )
{
    return InterlockedIncrement( &impl_from_IBasicProperties( iface )->ref );
}

static ULONG WINAPI basic_properties_Release( IBasicProperties *iface )
{
    struct basic_properties *impl = impl_from_IBasicProperties( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI basic_properties_GetIids( IBasicProperties *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI basic_properties_GetRuntimeClassName( IBasicProperties *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_FileProperties_BasicProperties,
                                wcslen( RuntimeClass_Windows_Storage_FileProperties_BasicProperties ), class_name );
}

static HRESULT WINAPI basic_properties_GetTrustLevel( IBasicProperties *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI basic_properties_get_Size( IBasicProperties *iface, UINT64 *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IBasicProperties( iface )->size;
    return S_OK;
}

static HRESULT WINAPI basic_properties_get_DateModified( IBasicProperties *iface, DateTime *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IBasicProperties( iface )->modified;
    return S_OK;
}

static HRESULT WINAPI basic_properties_get_ItemDate( IBasicProperties *iface, DateTime *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IBasicProperties( iface )->modified;
    return S_OK;
}

static const struct IBasicPropertiesVtbl basic_properties_vtbl =
{
    basic_properties_QueryInterface,
    basic_properties_AddRef,
    basic_properties_Release,
    /* IInspectable methods */
    basic_properties_GetIids,
    basic_properties_GetRuntimeClassName,
    basic_properties_GetTrustLevel,
    /* IBasicProperties methods */
    basic_properties_get_Size,
    basic_properties_get_DateModified,
    basic_properties_get_ItemDate,
};

/* ---- the operations of an item ------------------------------------------------------------------------ */

static HRESULT item_rename_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *impl = item_from_call( call );
    WCHAR *path, *parent, *target = NULL, *unique;
    HRESULT hr = S_OK;
    DWORD flags = 0;

    if (!valid_relative_name( call->text, FALSE )) return E_INVALIDARG;
    if (!(path = item_path( impl ))) return E_OUTOFMEMORY;
    if ((parent = path_parent( path ))) target = path_join( parent, call->text );
    free( parent );
    if (!target) { free( path ); return E_OUTOFMEMORY; }

    if (wcsicmp( path, target ) && GetFileAttributesW( target ) != INVALID_FILE_ATTRIBUTES)
    {
        switch (call->option)
        {
        case NameCollisionOption_GenerateUniqueName:
            if (!(unique = unique_path( target, impl->folder ))) hr = E_OUTOFMEMORY;
            free( target );
            target = unique;
            break;
        case NameCollisionOption_ReplaceExisting:
            flags = MOVEFILE_REPLACE_EXISTING;
            break;
        default:
            hr = HRESULT_FROM_WIN32( ERROR_ALREADY_EXISTS );
            break;
        }
    }
    if (SUCCEEDED(hr) && !MoveFileExW( path, target, flags )) hr = last_error();
    free( path );
    if (SUCCEEDED(hr)) item_set_path( impl, target );
    else free( target );
    return hr;
}

static HRESULT item_delete_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *impl = item_from_call( call );
    WCHAR *path = item_path( impl );
    HRESULT hr = S_OK;

    if (!path) return E_OUTOFMEMORY;
    if (impl->folder)
    {
        if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) hr = last_error();
        else hr = hresult_from_error( delete_tree( path ) );
    }
    else if (!DeleteFileW( path )) hr = last_error();
    free( path );
    return hr;
}

static HRESULT item_properties_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *impl = item_from_call( call );
    WIN32_FILE_ATTRIBUTE_DATA data;
    struct basic_properties *properties;
    WCHAR *path = item_path( impl );
    BOOL ok;

    if (!path) return E_OUTOFMEMORY;
    ok = GetFileAttributesExW( path, GetFileExInfoStandard, &data );
    free( path );
    if (!ok) return last_error();

    if (!(properties = calloc( 1, sizeof(*properties) ))) return E_OUTOFMEMORY;
    properties->IBasicProperties_iface.lpVtbl = &basic_properties_vtbl;
    properties->ref = 1;
    if (!impl->folder) properties->size = ((UINT64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    properties->modified.UniversalTime = ((INT64)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    set_result( result, &properties->IBasicProperties_iface );
    return S_OK;
}

/* ---- the operations of a folder ----------------------------------------------------------------------- */

static HRESULT folder_child_path( struct storage_call *call, BOOL create, WCHAR **out )
{
    struct storage_item *impl = item_from_call( call );
    WCHAR *path;
    DWORD error = 0;

    *out = NULL;
    if (!valid_relative_name( call->text, TRUE )) return E_INVALIDARG;
    if (!(path = item_path( impl ))) return E_OUTOFMEMORY;
    *out = path_join( path, call->text );
    if (*out && create && wcspbrk( call->text, L"\\/" )) error = create_parents( *out, wcslen( path ) );
    free( path );
    if (!*out) return E_OUTOFMEMORY;
    if (error)
    {
        free( *out );
        *out = NULL;
        return hresult_from_error( error );
    }
    return S_OK;
}

static HRESULT folder_create_file_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *file;
    WCHAR *path, *unique;
    DWORD disposition, error = 0;
    HANDLE handle;
    HRESULT hr;

    if (FAILED(hr = folder_child_path( call, TRUE, &path ))) return hr;
    switch (call->option)
    {
    case CreationCollisionOption_ReplaceExisting: disposition = CREATE_ALWAYS; break;
    case CreationCollisionOption_OpenIfExists: disposition = OPEN_ALWAYS; break;
    default: disposition = CREATE_NEW; break;
    }
    for (;;)
    {
        handle = CreateFileW( path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              disposition, FILE_ATTRIBUTE_NORMAL, NULL );
        if (handle != INVALID_HANDLE_VALUE) break;
        error = GetLastError();
        if (call->option != CreationCollisionOption_GenerateUniqueName) break;
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS && error != ERROR_ACCESS_DENIED) break;
        if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) break;
        if (!(unique = unique_path( path, FALSE ))) { error = ERROR_OUTOFMEMORY; break; }
        free( path );
        path = unique;
    }
    if (handle == INVALID_HANDLE_VALUE)
    {
        /* a folder of that name is in the way */
        if (error == ERROR_ACCESS_DENIED && GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES) error = ERROR_ALREADY_EXISTS;
        free( path );
        return hresult_from_error( error );
    }
    CloseHandle( handle );

    file = item_create( path, FALSE );
    free( path );
    if (!file) return E_OUTOFMEMORY;
    set_result( result, &file->IStorageFile_iface );
    return S_OK;
}

static HRESULT folder_create_folder_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *folder;
    WCHAR *path, *unique;
    DWORD error = 0, attributes;
    HRESULT hr;

    if (FAILED(hr = folder_child_path( call, TRUE, &path ))) return hr;
    while (!CreateDirectoryW( path, NULL ))
    {
        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) break;
        attributes = GetFileAttributesW( path );
        if (call->option == CreationCollisionOption_OpenIfExists && attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            error = 0;
            break;
        }
        if (call->option == CreationCollisionOption_ReplaceExisting)
        {
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) error = delete_tree( path );
            else error = DeleteFileW( path ) ? 0 : GetLastError();
            if (error) break;
            continue;
        }
        if (call->option != CreationCollisionOption_GenerateUniqueName) break;
        if (!(unique = unique_path( path, TRUE ))) { error = ERROR_OUTOFMEMORY; break; }
        free( path );
        path = unique;
        error = 0;
    }
    if (error)
    {
        free( path );
        return hresult_from_error( error );
    }

    folder = item_create( path, TRUE );
    free( path );
    if (!folder) return E_OUTOFMEMORY;
    set_result( result, &folder->IStorageFolder_iface );
    return S_OK;
}

/* option: the kind of item wanted; flags: nonzero when a missing item is not an error (TryGetItemAsync) */
static HRESULT folder_get_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *item;
    WCHAR *path;
    HRESULT hr;

    if (FAILED(hr = folder_child_path( call, FALSE, &path ))) return hr;
    hr = item_open( path, call->option, &item );
    free( path );
    if (FAILED(hr))
    {
        if (!call->flags) return hr;
        set_result( result, NULL );
        return S_OK;
    }
    if (call->option == ITEM_FILE) set_result( result, &item->IStorageFile_iface );
    else if (call->option == ITEM_FOLDER) set_result( result, &item->IStorageFolder_iface );
    else set_result( result, &item->IStorageItem_iface );
    return S_OK;
}

struct listed_name
{
    WCHAR *name;
    BOOL folder;
};

static int __cdecl compare_listed_names( const void *a, const void *b )
{
    const struct listed_name *first = a, *second = b;

    if (first->folder != second->folder) return first->folder ? -1 : 1;
    return CompareStringOrdinal( first->name, -1, second->name, -1, TRUE ) - CSTR_EQUAL;
}

static const struct vector_iids file_view_iids =
{
    .iterable = &IID_IIterable_StorageFile,
    .iterator = &IID_IIterator_StorageFile,
    .vector = &IID_IVectorView_StorageFile, /* only the view is handed out */
    .view = &IID_IVectorView_StorageFile,
};

static const struct vector_iids folder_view_iids =
{
    .iterable = &IID_IIterable_StorageFolder,
    .iterator = &IID_IIterator_StorageFolder,
    .vector = &IID_IVectorView_StorageFolder,
    .view = &IID_IVectorView_StorageFolder,
};

static const struct vector_iids item_view_iids =
{
    .iterable = &IID_IIterable_IStorageItem,
    .iterator = &IID_IIterator_IStorageItem,
    .vector = &IID_IVectorView_IStorageItem,
    .view = &IID_IVectorView_IStorageItem,
};

/* option: the kind of items to list. Folders come first, each kind is sorted by name. */
static HRESULT folder_list_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *impl = item_from_call( call ), *item;
    struct listed_name *names = NULL, *tmp;
    size_t count = 0, capacity = 0, i;
    IVectorView_IInspectable *view;
    IVector_IInspectable *vector;
    WIN32_FIND_DATAW data;
    WCHAR *path, *pattern, *child;
    HRESULT hr = S_OK;
    HANDLE find;

    if (!(path = item_path( impl ))) return E_OUTOFMEMORY;
    if (!(pattern = path_join( path, L"*" ))) { free( path ); return E_OUTOFMEMORY; }
    find = FindFirstFileW( pattern, &data );
    free( pattern );
    if (find == INVALID_HANDLE_VALUE)
    {
        hr = last_error();
        free( path );
        return hr;
    }
    do
    {
        BOOL folder = !!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

        if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
        if ((call->option == ITEM_FILE && folder) || (call->option == ITEM_FOLDER && !folder)) continue;
        if (count == capacity)
        {
            capacity = capacity ? capacity * 2 : 32;
            if (!(tmp = realloc( names, capacity * sizeof(*names) ))) { hr = E_OUTOFMEMORY; break; }
            names = tmp;
        }
        if (!(names[count].name = wcsdup( data.cFileName ))) { hr = E_OUTOFMEMORY; break; }
        names[count++].folder = folder;
    } while (FindNextFileW( find, &data ));
    FindClose( find );

    if (SUCCEEDED(hr))
    {
        qsort( names, count, sizeof(*names), compare_listed_names );
        hr = vector_inspectable_create( call->option == ITEM_FILE ? &file_view_iids :
                                        call->option == ITEM_FOLDER ? &folder_view_iids : &item_view_iids, &vector );
    }
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < count && SUCCEEDED(hr); i++)
        {
            IInspectable *element;

            if (!(child = path_join( path, names[i].name ))) { hr = E_OUTOFMEMORY; break; }
            item = item_create( child, names[i].folder );
            free( child );
            if (!item) { hr = E_OUTOFMEMORY; break; }
            if (call->option == ITEM_FILE) element = (IInspectable *)&item->IStorageFile_iface;
            else if (call->option == ITEM_FOLDER) element = (IInspectable *)&item->IStorageFolder_iface;
            else element = (IInspectable *)&item->IStorageItem_iface;
            hr = IVector_IInspectable_Append( vector, element );
            IInspectable_Release( element );
        }
        if (SUCCEEDED(hr) && SUCCEEDED(hr = IVector_IInspectable_GetView( vector, &view ))) set_result( result, view );
        IVector_IInspectable_Release( vector );
    }

    for (i = 0; i < count; i++) free( names[i].name );
    free( names );
    free( path );
    return hr;
}

/* ---- the operations of a file ------------------------------------------------------------------------- */

/* option: NameCollisionOption; flags: nonzero to move, zero to copy. other: the folder, text: the new name or "". */
static HRESULT file_transfer_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *impl = item_from_call( call ), *copy;
    WCHAR *source, *folder = NULL, *target = NULL, *unique;
    const WCHAR *name;
    HRESULT hr;
    BOOL ok;

    if (!(source = item_path( impl ))) return E_OUTOFMEMORY;
    name = call->text[0] ? call->text : path_name( source );
    if (!valid_relative_name( name, FALSE )) hr = E_INVALIDARG;
    else if (SUCCEEDED(hr = storage_item_get_path( call->other, &folder )) && !(target = path_join( folder, name )))
        hr = E_OUTOFMEMORY;
    free( folder );

    if (SUCCEEDED(hr) && call->option == NameCollisionOption_GenerateUniqueName &&
        GetFileAttributesW( target ) != INVALID_FILE_ATTRIBUTES)
    {
        if (!(unique = unique_path( target, FALSE ))) hr = E_OUTOFMEMORY;
        free( target );
        target = unique;
    }
    if (SUCCEEDED(hr))
    {
        BOOL replace = call->option == NameCollisionOption_ReplaceExisting;

        if (call->flags) ok = MoveFileExW( source, target, MOVEFILE_COPY_ALLOWED | (replace ? MOVEFILE_REPLACE_EXISTING : 0) );
        else ok = CopyFileW( source, target, !replace );
        if (!ok) hr = last_error();
    }
    free( source );
    if (FAILED(hr))
    {
        free( target );
        return hr;
    }

    if (call->flags)
    {
        item_set_path( impl, target );
        return S_OK;
    }
    copy = item_create( target, FALSE );
    free( target );
    if (!copy) return E_OUTOFMEMORY;
    set_result( result, &copy->IStorageFile_iface );
    return S_OK;
}

/* flags: nonzero to move, zero to copy. other: the file that is replaced. */
static HRESULT file_replace_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *impl = item_from_call( call );
    WCHAR *source, *target;
    HRESULT hr;
    BOOL ok;

    if (FAILED(hr = storage_item_get_path( call->other, &target ))) return hr;
    if (!(source = item_path( impl ))) { free( target ); return E_OUTOFMEMORY; }
    if (call->flags) ok = MoveFileExW( source, target, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING );
    else ok = CopyFileW( source, target, FALSE );
    if (!ok) hr = last_error();
    free( source );
    if (SUCCEEDED(hr) && call->flags) item_set_path( impl, target );
    else free( target );
    return hr;
}

/* ---- the object --------------------------------------------------------------------------------------- */

static HRESULT item_query_interface( struct storage_item *impl, REFIID iid, void **out )
{
    TRACE( "impl %p, iid %s, out %p.\n", impl, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IStorageItem ))
        *out = &impl->IStorageItem_iface;
    else if (IsEqualGUID( iid, &IID_IStorageItemProperties ))
        *out = &impl->IStorageItemProperties_iface;
    else if (impl->folder && IsEqualGUID( iid, &IID_IStorageFolder ))
        *out = &impl->IStorageFolder_iface;
    else if (impl->folder && IsEqualGUID( iid, &IID_IStorageFolder2 ))
        *out = &impl->IStorageFolder2_iface;
    else if (!impl->folder && IsEqualGUID( iid, &IID_IStorageFile ))
        *out = &impl->IStorageFile_iface;
    else if (!impl->folder && IsEqualGUID( iid, &IID_IStorageFilePropertiesWithAvailability ))
        *out = &impl->IStorageFilePropertiesWithAvailability_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    InterlockedIncrement( &impl->ref );
    return S_OK;
}

static ULONG item_add_ref( struct storage_item *impl )
{
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "impl %p, ref %lu.\n", impl, ref );
    return ref;
}

static ULONG item_release( struct storage_item *impl )
{
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "impl %p, ref %lu.\n", impl, ref );

    if (!ref)
    {
        free( impl->path );
        free( impl );
    }
    return ref;
}

static HRESULT item_get_iids( struct storage_item *impl, ULONG *iid_count, IID **iids )
{
    const IID *folder_iids[] = { &IID_IStorageFolder, &IID_IStorageItem, &IID_IStorageItemProperties, &IID_IStorageFolder2 };
    const IID *file_iids[] = { &IID_IStorageFile, &IID_IStorageItem, &IID_IStorageItemProperties,
                               &IID_IStorageFilePropertiesWithAvailability };
    const IID **list = impl->folder ? folder_iids : file_iids;
    ULONG i, count = impl->folder ? ARRAY_SIZE(folder_iids) : ARRAY_SIZE(file_iids);

    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    if (!(*iids = CoTaskMemAlloc( count * sizeof(IID) ))) return E_OUTOFMEMORY;
    for (i = 0; i < count; i++) (*iids)[i] = *list[i];
    *iid_count = count;
    return S_OK;
}

static HRESULT item_get_runtime_class_name( struct storage_item *impl, HSTRING *class_name )
{
    const WCHAR *name = impl->folder ? L"Windows.Storage.StorageFolder" : L"Windows.Storage.StorageFile";

    if (!class_name) return E_POINTER;
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT item_get_trust_level( struct storage_item *impl, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

#define DEFINE_ITEM_IFACE( pfx, iface_type )                                                       \
    static inline struct storage_item *impl_from_##iface_type( iface_type *iface )                 \
    {                                                                                              \
        return CONTAINING_RECORD( iface, struct storage_item, iface_type##_iface );                \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        return item_query_interface( impl_from_##iface_type( iface ), iid, out );                  \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        return item_add_ref( impl_from_##iface_type( iface ) );                                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        return item_release( impl_from_##iface_type( iface ) );                                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        return item_get_iids( impl_from_##iface_type( iface ), iid_count, iids );                  \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        return item_get_runtime_class_name( impl_from_##iface_type( iface ), class_name );         \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        return item_get_trust_level( impl_from_##iface_type( iface ), trust_level );               \
    }

DEFINE_ITEM_IFACE( storage_item, IStorageItem )
DEFINE_ITEM_IFACE( storage_file, IStorageFile )
DEFINE_ITEM_IFACE( storage_folder, IStorageFolder )
DEFINE_ITEM_IFACE( storage_folder2, IStorageFolder2 )
DEFINE_ITEM_IFACE( item_properties, IStorageItemProperties )
DEFINE_ITEM_IFACE( file_availability, IStorageFilePropertiesWithAvailability )

#define ITEM_OBJECT( impl ) (&(impl)->IStorageItem_iface)

/* IStorageItem */

static HRESULT WINAPI storage_item_RenameAsync( IStorageItem *iface, HSTRING name, NameCollisionOption option,
                                                IAsyncAction **operation )
{
    TRACE( "iface %p, name %s, option %u, operation %p.\n", iface, debugstr_hstring( name ), option, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_action_create, storage_call_create( item_rename_run, iface, NULL, name, option ), operation );
}

static HRESULT WINAPI storage_item_RenameAsyncOverloadDefaultOptions( IStorageItem *iface, HSTRING name,
                                                                      IAsyncAction **operation )
{
    return storage_item_RenameAsync( iface, name, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_item_DeleteAsync( IStorageItem *iface, StorageDeleteOption option, IAsyncAction **operation )
{
    TRACE( "iface %p, option %u, operation %p.\n", iface, option, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_action_create, storage_call_create( item_delete_run, iface, NULL, NULL, option ), operation );
}

static HRESULT WINAPI storage_item_DeleteAsyncOverloadDefaultOptions( IStorageItem *iface, IAsyncAction **operation )
{
    return storage_item_DeleteAsync( iface, StorageDeleteOption_Default, operation );
}

static HRESULT WINAPI storage_item_GetBasicPropertiesAsync( IStorageItem *iface, IAsyncOperation_BasicProperties **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_basic_properties_create, storage_call_create( item_properties_run, iface, NULL, NULL, 0 ),
                operation );
}

static HRESULT WINAPI storage_item_get_Name( IStorageItem *iface, HSTRING *value )
{
    struct storage_item *impl = impl_from_IStorageItem( iface );
    const WCHAR *name;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    name = path_name( impl->path );
    hr = WindowsCreateString( name, wcslen( name ), value );
    ReleaseSRWLockShared( &impl->lock );
    return hr;
}

static HRESULT WINAPI storage_item_get_Path( IStorageItem *iface, HSTRING *value )
{
    struct storage_item *impl = impl_from_IStorageItem( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    hr = WindowsCreateString( impl->path, wcslen( impl->path ), value );
    ReleaseSRWLockShared( &impl->lock );
    return hr;
}

static HRESULT item_attribute_data( struct storage_item *impl, WIN32_FILE_ATTRIBUTE_DATA *data )
{
    WCHAR *path = item_path( impl );
    BOOL ok;

    if (!path) return E_OUTOFMEMORY;
    ok = GetFileAttributesExW( path, GetFileExInfoStandard, data );
    free( path );
    return ok ? S_OK : last_error();
}

static HRESULT WINAPI storage_item_get_Attributes( IStorageItem *iface, FileAttributes *value )
{
    struct storage_item *impl = impl_from_IStorageItem( iface );
    WIN32_FILE_ATTRIBUTE_DATA data;
    FileAttributes attrs = FileAttributes_Normal;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = FileAttributes_Normal;
    if (FAILED(hr = item_attribute_data( impl, &data ))) return hr;

    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) attrs |= FileAttributes_ReadOnly;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) attrs |= FileAttributes_Directory;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) attrs |= FileAttributes_Archive;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) attrs |= FileAttributes_Temporary;
    *value = attrs;
    return S_OK;
}

static HRESULT WINAPI storage_item_get_DateCreated( IStorageItem *iface, DateTime *value )
{
    struct storage_item *impl = impl_from_IStorageItem( iface );
    WIN32_FILE_ATTRIBUTE_DATA data;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    value->UniversalTime = 0;
    if (FAILED(hr = item_attribute_data( impl, &data ))) return hr;
    value->UniversalTime = ((INT64)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI storage_item_IsOfType( IStorageItem *iface, StorageItemTypes type, boolean *value )
{
    struct storage_item *impl = impl_from_IStorageItem( iface );

    TRACE( "iface %p, type %u, value %p.\n", iface, type, value );

    if (!value) return E_POINTER;
    *value = (type == (impl->folder ? StorageItemTypes_Folder : StorageItemTypes_File));
    return S_OK;
}

static const struct IStorageItemVtbl storage_item_vtbl =
{
    storage_item_QueryInterface,
    storage_item_AddRef,
    storage_item_Release,
    /* IInspectable methods */
    storage_item_GetIids,
    storage_item_GetRuntimeClassName,
    storage_item_GetTrustLevel,
    /* IStorageItem methods */
    storage_item_RenameAsyncOverloadDefaultOptions,
    storage_item_RenameAsync,
    storage_item_DeleteAsyncOverloadDefaultOptions,
    storage_item_DeleteAsync,
    storage_item_GetBasicPropertiesAsync,
    storage_item_get_Name,
    storage_item_get_Path,
    storage_item_get_Attributes,
    storage_item_get_DateCreated,
    storage_item_IsOfType,
};

/* IStorageFolder */

static HRESULT WINAPI storage_folder_CreateFileAsync( IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
                                                      IAsyncOperation_StorageFile **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, name %s, options %u, operation %p.\n", iface, debugstr_hstring( name ), options, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_storage_file_create,
                storage_call_create( folder_create_file_run, ITEM_OBJECT( impl ), NULL, name, options ), operation );
}

static HRESULT WINAPI storage_folder_CreateFileAsyncOverloadDefaultOptions( IStorageFolder *iface, HSTRING name,
                                                                            IAsyncOperation_StorageFile **operation )
{
    return storage_folder_CreateFileAsync( iface, name, CreationCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_folder_CreateFolderAsync( IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
                                                        IAsyncOperation_StorageFolder **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, name %s, options %u, operation %p.\n", iface, debugstr_hstring( name ), options, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_storage_folder_create,
                storage_call_create( folder_create_folder_run, ITEM_OBJECT( impl ), NULL, name, options ), operation );
}

static HRESULT WINAPI storage_folder_CreateFolderAsyncOverloadDefaultOptions( IStorageFolder *iface, HSTRING name,
                                                                              IAsyncOperation_StorageFolder **operation )
{
    return storage_folder_CreateFolderAsync( iface, name, CreationCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_folder_GetFileAsync( IStorageFolder *iface, HSTRING name, IAsyncOperation_StorageFile **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_storage_file_create,
                storage_call_create( folder_get_run, ITEM_OBJECT( impl ), NULL, name, ITEM_FILE ), operation );
}

static HRESULT WINAPI storage_folder_GetFolderAsync( IStorageFolder *iface, HSTRING name,
                                                     IAsyncOperation_StorageFolder **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_storage_folder_create,
                storage_call_create( folder_get_run, ITEM_OBJECT( impl ), NULL, name, ITEM_FOLDER ), operation );
}

static HRESULT WINAPI storage_folder_GetItemAsync( IStorageFolder *iface, HSTRING name,
                                                   IAsyncOperation_IStorageItem **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_storage_item_create,
                storage_call_create( folder_get_run, ITEM_OBJECT( impl ), NULL, name, ITEM_ANY ), operation );
}

static HRESULT WINAPI storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount( IStorageFolder *iface,
        IAsyncOperation_IVectorView_StorageFile **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_file_view_create,
                storage_call_create( folder_list_run, ITEM_OBJECT( impl ), NULL, NULL, ITEM_FILE ), operation );
}

static HRESULT WINAPI storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount( IStorageFolder *iface,
        IAsyncOperation_IVectorView_StorageFolder **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_folder_view_create,
                storage_call_create( folder_list_run, ITEM_OBJECT( impl ), NULL, NULL, ITEM_FOLDER ), operation );
}

static HRESULT WINAPI storage_folder_GetItemsAsyncOverloadDefaultStartAndCount( IStorageFolder *iface,
        IAsyncOperation_IVectorView_IStorageItem **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_operation_item_view_create,
                storage_call_create( folder_list_run, ITEM_OBJECT( impl ), NULL, NULL, ITEM_ANY ), operation );
}

static const struct IStorageFolderVtbl storage_folder_vtbl =
{
    storage_folder_QueryInterface,
    storage_folder_AddRef,
    storage_folder_Release,
    /* IInspectable methods */
    storage_folder_GetIids,
    storage_folder_GetRuntimeClassName,
    storage_folder_GetTrustLevel,
    /* IStorageFolder methods */
    storage_folder_CreateFileAsyncOverloadDefaultOptions,
    storage_folder_CreateFileAsync,
    storage_folder_CreateFolderAsyncOverloadDefaultOptions,
    storage_folder_CreateFolderAsync,
    storage_folder_GetFileAsync,
    storage_folder_GetFolderAsync,
    storage_folder_GetItemAsync,
    storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount,
    storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount,
    storage_folder_GetItemsAsyncOverloadDefaultStartAndCount,
};

/* IStorageFolder2 */

static HRESULT WINAPI storage_folder2_TryGetItemAsync( IStorageFolder2 *iface, HSTRING name,
                                                       IAsyncOperation_IStorageItem **operation )
{
    struct storage_item *impl = impl_from_IStorageFolder2( iface );
    struct storage_call *call;

    TRACE( "iface %p, name %s, operation %p.\n", iface, debugstr_hstring( name ), operation );

    if (!operation) return E_POINTER;
    if ((call = storage_call_create( folder_get_run, ITEM_OBJECT( impl ), NULL, name, ITEM_ANY ))) call->flags = 1;
    START_CALL( async_operation_storage_item_create, call, operation );
}

static const struct IStorageFolder2Vtbl storage_folder2_vtbl =
{
    storage_folder2_QueryInterface,
    storage_folder2_AddRef,
    storage_folder2_Release,
    /* IInspectable methods */
    storage_folder2_GetIids,
    storage_folder2_GetRuntimeClassName,
    storage_folder2_GetTrustLevel,
    /* IStorageFolder2 methods */
    storage_folder2_TryGetItemAsync,
};

/* IStorageFile */

static HRESULT WINAPI storage_file_get_FileType( IStorageFile *iface, HSTRING *value )
{
    struct storage_item *impl = impl_from_IStorageFile( iface );
    const WCHAR *extension;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    extension = path_extension( impl->path );
    hr = WindowsCreateString( extension, wcslen( extension ), value );
    ReleaseSRWLockShared( &impl->lock );
    return hr;
}

static HRESULT WINAPI storage_file_get_ContentType( IStorageFile *iface, HSTRING *value )
{
    struct storage_item *impl = impl_from_IStorageFile( iface );
    WCHAR extension[64], type[128] = {0};
    DWORD size = sizeof(type) - sizeof(WCHAR);

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    lstrcpynW( extension, path_extension( impl->path ), ARRAY_SIZE(extension) );
    ReleaseSRWLockShared( &impl->lock );

    if (!extension[0] || RegGetValueW( HKEY_CLASSES_ROOT, extension, L"Content Type", RRF_RT_REG_SZ, NULL, type, &size ))
    {
        /* the registry of Wine has few content types: the common ones are here */
        static const struct { const WCHAR *extension; const WCHAR *type; } types[] =
        {
            { L".txt", L"text/plain" }, { L".log", L"text/plain" }, { L".ini", L"text/plain" },
            { L".htm", L"text/html" }, { L".html", L"text/html" }, { L".css", L"text/css" },
            { L".xml", L"text/xml" }, { L".csv", L"text/csv" }, { L".js", L"application/javascript" },
            { L".json", L"application/json" }, { L".pdf", L"application/pdf" },
            { L".zip", L"application/x-zip-compressed" },
            { L".png", L"image/png" }, { L".jpg", L"image/jpeg" }, { L".jpeg", L"image/jpeg" },
            { L".gif", L"image/gif" }, { L".bmp", L"image/bmp" }, { L".svg", L"image/svg+xml" },
            { L".ico", L"image/x-icon" }, { L".tif", L"image/tiff" }, { L".tiff", L"image/tiff" },
            { L".mp3", L"audio/mpeg" }, { L".wav", L"audio/wav" }, { L".wma", L"audio/x-ms-wma" },
            { L".mp4", L"video/mp4" }, { L".avi", L"video/avi" }, { L".wmv", L"video/x-ms-wmv" },
        };
        unsigned int i;

        type[0] = 0;
        for (i = 0; i < ARRAY_SIZE(types); i++)
        {
            if (wcsicmp( extension, types[i].extension )) continue;
            wcscpy( type, types[i].type );
            break;
        }
    }
    return WindowsCreateString( type, wcslen( type ), value );
}

static HRESULT WINAPI storage_file_OpenAsync( IStorageFile *iface, FileAccessMode mode,
                                              IAsyncOperation_IRandomAccessStream **operation )
{
    FIXME( "iface %p, mode %u, operation %p stub!\n", iface, mode, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_file_OpenTransactedWriteAsync( IStorageFile *iface,
                                                             IAsyncOperation_StorageStreamTransaction **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_file_CopyOverload( IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                                 NameCollisionOption option, IAsyncOperation_StorageFile **operation )
{
    struct storage_item *impl = impl_from_IStorageFile( iface );

    TRACE( "iface %p, folder %p, name %s, option %u, operation %p.\n", iface, folder, debugstr_hstring( name ), option, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!folder) return E_INVALIDARG;
    START_CALL( async_operation_storage_file_create,
                storage_call_create( file_transfer_run, ITEM_OBJECT( impl ), folder, name, option ), operation );
}

static HRESULT WINAPI storage_file_CopyOverloadDefaultOptions( IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                                               IAsyncOperation_StorageFile **operation )
{
    return storage_file_CopyOverload( iface, folder, name, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_file_CopyOverloadDefaultNameAndOptions( IStorageFile *iface, IStorageFolder *folder,
                                                                      IAsyncOperation_StorageFile **operation )
{
    return storage_file_CopyOverload( iface, folder, NULL, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_file_CopyAndReplaceAsync( IStorageFile *iface, IStorageFile *file, IAsyncAction **operation )
{
    struct storage_item *impl = impl_from_IStorageFile( iface );

    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!file) return E_INVALIDARG;
    START_CALL( async_action_create, storage_call_create( file_replace_run, ITEM_OBJECT( impl ), file, NULL, 0 ), operation );
}

static HRESULT WINAPI storage_file_MoveOverload( IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                                 NameCollisionOption option, IAsyncAction **operation )
{
    struct storage_item *impl = impl_from_IStorageFile( iface );
    struct storage_call *call;

    TRACE( "iface %p, folder %p, name %s, option %u, operation %p.\n", iface, folder, debugstr_hstring( name ), option, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!folder) return E_INVALIDARG;
    if ((call = storage_call_create( file_transfer_run, ITEM_OBJECT( impl ), folder, name, option ))) call->flags = 1;
    START_CALL( async_action_create, call, operation );
}

static HRESULT WINAPI storage_file_MoveOverloadDefaultOptions( IStorageFile *iface, IStorageFolder *folder, HSTRING name,
                                                               IAsyncAction **operation )
{
    return storage_file_MoveOverload( iface, folder, name, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_file_MoveOverloadDefaultNameAndOptions( IStorageFile *iface, IStorageFolder *folder,
                                                                      IAsyncAction **operation )
{
    return storage_file_MoveOverload( iface, folder, NULL, NameCollisionOption_FailIfExists, operation );
}

static HRESULT WINAPI storage_file_MoveAndReplaceAsync( IStorageFile *iface, IStorageFile *file, IAsyncAction **operation )
{
    struct storage_item *impl = impl_from_IStorageFile( iface );
    struct storage_call *call;

    TRACE( "iface %p, file %p, operation %p.\n", iface, file, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!file) return E_INVALIDARG;
    if ((call = storage_call_create( file_replace_run, ITEM_OBJECT( impl ), file, NULL, 0 ))) call->flags = 1;
    START_CALL( async_action_create, call, operation );
}

static const struct IStorageFileVtbl storage_file_vtbl =
{
    storage_file_QueryInterface,
    storage_file_AddRef,
    storage_file_Release,
    /* IInspectable methods */
    storage_file_GetIids,
    storage_file_GetRuntimeClassName,
    storage_file_GetTrustLevel,
    /* IStorageFile methods */
    storage_file_get_FileType,
    storage_file_get_ContentType,
    storage_file_OpenAsync,
    storage_file_OpenTransactedWriteAsync,
    storage_file_CopyOverloadDefaultNameAndOptions,
    storage_file_CopyOverloadDefaultOptions,
    storage_file_CopyOverload,
    storage_file_CopyAndReplaceAsync,
    storage_file_MoveOverloadDefaultNameAndOptions,
    storage_file_MoveOverloadDefaultOptions,
    storage_file_MoveOverload,
    storage_file_MoveAndReplaceAsync,
};

/* IStorageItemProperties */

static HRESULT WINAPI item_properties_GetThumbnailAsyncOverloadDefaultSizeDefaultOptions( IStorageItemProperties *iface,
        INT32 mode, IInspectable **operation )
{
    FIXME( "iface %p, mode %d, operation %p stub!\n", iface, mode, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI item_properties_GetThumbnailAsyncOverloadDefaultOptions( IStorageItemProperties *iface, INT32 mode,
        UINT32 size, IInspectable **operation )
{
    FIXME( "iface %p, mode %d, size %u, operation %p stub!\n", iface, mode, size, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI item_properties_GetThumbnailAsync( IStorageItemProperties *iface, INT32 mode, UINT32 size,
        UINT32 options, IInspectable **operation )
{
    FIXME( "iface %p, mode %d, size %u, options %#x, operation %p stub!\n", iface, mode, size, options, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

/* Windows 11 gave the name with its extension for a file in the data folder of a package */
static HRESULT WINAPI item_properties_get_DisplayName( IStorageItemProperties *iface, HSTRING *value )
{
    struct storage_item *impl = impl_from_IStorageItemProperties( iface );
    return storage_item_get_Name( &impl->IStorageItem_iface, value );
}

static HRESULT WINAPI item_properties_get_DisplayType( IStorageItemProperties *iface, HSTRING *value )
{
    struct storage_item *impl = impl_from_IStorageItemProperties( iface );
    WCHAR type[80];

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (impl->folder) wcscpy( type, L"File folder" );
    else
    {
        const WCHAR *extension;
        size_t i = 0;

        AcquireSRWLockShared( &impl->lock );
        extension = path_extension( impl->path );
        if (extension[0]) for (extension++; extension[i] && i < 60; i++) type[i] = towupper( extension[i] );
        ReleaseSRWLockShared( &impl->lock );
        wcscpy( type + i, i ? L" File" : L"File" );
    }
    return WindowsCreateString( type, wcslen( type ), value );
}

static HRESULT WINAPI item_properties_get_FolderRelativeId( IStorageItemProperties *iface, HSTRING *value )
{
    struct storage_item *impl = impl_from_IStorageItemProperties( iface );
    return storage_item_get_Name( &impl->IStorageItem_iface, value );
}

static HRESULT WINAPI item_properties_get_Properties( IStorageItemProperties *iface, IInspectable **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static const struct IStorageItemPropertiesVtbl item_properties_vtbl =
{
    item_properties_QueryInterface,
    item_properties_AddRef,
    item_properties_Release,
    /* IInspectable methods */
    item_properties_GetIids,
    item_properties_GetRuntimeClassName,
    item_properties_GetTrustLevel,
    /* IStorageItemProperties methods */
    item_properties_GetThumbnailAsyncOverloadDefaultSizeDefaultOptions,
    item_properties_GetThumbnailAsyncOverloadDefaultOptions,
    item_properties_GetThumbnailAsync,
    item_properties_get_DisplayName,
    item_properties_get_DisplayType,
    item_properties_get_FolderRelativeId,
    item_properties_get_Properties,
};

/* IStorageFilePropertiesWithAvailability */

static HRESULT WINAPI file_availability_get_IsAvailable( IStorageFilePropertiesWithAvailability *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static const struct IStorageFilePropertiesWithAvailabilityVtbl file_availability_vtbl =
{
    file_availability_QueryInterface,
    file_availability_AddRef,
    file_availability_Release,
    /* IInspectable methods */
    file_availability_GetIids,
    file_availability_GetRuntimeClassName,
    file_availability_GetTrustLevel,
    /* IStorageFilePropertiesWithAvailability methods */
    file_availability_get_IsAvailable,
};

static struct storage_item *item_create( const WCHAR *path, BOOL folder )
{
    struct storage_item *impl;
    size_t length = wcslen( path );

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IStorageItem_iface.lpVtbl = &storage_item_vtbl;
    impl->IStorageFile_iface.lpVtbl = &storage_file_vtbl;
    impl->IStorageFolder_iface.lpVtbl = &storage_folder_vtbl;
    impl->IStorageFolder2_iface.lpVtbl = &storage_folder2_vtbl;
    impl->IStorageItemProperties_iface.lpVtbl = &item_properties_vtbl;
    impl->IStorageFilePropertiesWithAvailability_iface.lpVtbl = &file_availability_vtbl;
    impl->ref = 1;
    impl->folder = folder;
    InitializeSRWLock( &impl->lock );
    if (!(impl->path = wcsdup( path )))
    {
        free( impl );
        return NULL;
    }
    /* no separator at the end, except for the root of a drive */
    while (length > 3 && (impl->path[length - 1] == '\\' || impl->path[length - 1] == '/')) impl->path[--length] = 0;
    return impl;
}

/* ---- the statics -------------------------------------------------------------------------------------- */

/* text: the path; option: the kind of item wanted */
static HRESULT item_from_path_run( struct storage_call *call, PROPVARIANT *result )
{
    struct storage_item *item;
    WCHAR *path;
    HRESULT hr;

    if (call->option == ITEM_FOLDER)
    {
        /* a folder is only found by an absolute path */
        if (FAILED(hr = validate_absolute_path( call->text, call->size ))) return hr;
        hr = item_open( call->text, ITEM_FOLDER, &item );
    }
    else
    {
        if (wcslen( call->text ) != call->size) return E_INVALIDARG;
        if (FAILED(hr = storage_resolve_path( call->text, &path ))) return hr;
        hr = item_open( path, ITEM_FILE, &item );
        free( path );
    }
    if (FAILED(hr)) return hr;
    if (call->option == ITEM_FOLDER) set_result( result, &item->IStorageFolder_iface );
    else set_result( result, &item->IStorageFile_iface );
    return S_OK;
}

struct item_statics
{
    IActivationFactory IActivationFactory_iface;
    IStorageFolderStatics IStorageFolderStatics_iface;
    IStorageFileStatics IStorageFileStatics_iface;
    BOOL folder;
    LONG ref;
};

static inline struct item_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct item_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct item_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (impl->folder && IsEqualGUID( iid, &IID_IStorageFolderStatics ))
        *out = &impl->IStorageFolderStatics_iface;
    else if (!impl->folder && IsEqualGUID( iid, &IID_IStorageFileStatics ))
        *out = &impl->IStorageFileStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct item_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct item_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    struct item_statics *impl = impl_from_IActivationFactory( iface );
    IID *array;

    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    if (!(array = CoTaskMemAlloc( 2 * sizeof(*array) ))) return E_OUTOFMEMORY;
    array[0] = IID_IActivationFactory;
    array[1] = impl->folder ? IID_IStorageFolderStatics : IID_IStorageFileStatics;
    *iid_count = 2;
    *iids = array;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    if (!class_name) return E_POINTER;
    *class_name = NULL;
    return E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    if (!instance) return E_POINTER;
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
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

DEFINE_IINSPECTABLE( storage_folder_statics, IStorageFolderStatics, struct item_statics, IActivationFactory_iface )

static HRESULT WINAPI storage_folder_statics_GetFolderFromPathAsync( IStorageFolderStatics *iface, HSTRING path,
                                                                     IAsyncOperation_StorageFolder **operation )
{
    struct storage_call *call;

    TRACE( "iface %p, path %s, operation %p.\n", iface, debugstr_hstring( path ), operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    /* a NULL HSTRING is the empty string: the operation fails, the call does not */
    if ((call = storage_call_create( item_from_path_run, NULL, NULL, path, ITEM_FOLDER )))
        call->size = WindowsGetStringLen( path );
    START_CALL( async_operation_storage_folder_create, call, operation );
}

static const struct IStorageFolderStaticsVtbl storage_folder_statics_vtbl =
{
    storage_folder_statics_QueryInterface,
    storage_folder_statics_AddRef,
    storage_folder_statics_Release,
    /* IInspectable methods */
    storage_folder_statics_GetIids,
    storage_folder_statics_GetRuntimeClassName,
    storage_folder_statics_GetTrustLevel,
    /* IStorageFolderStatics methods */
    storage_folder_statics_GetFolderFromPathAsync,
};

DEFINE_IINSPECTABLE( storage_file_statics, IStorageFileStatics, struct item_statics, IActivationFactory_iface )

static HRESULT WINAPI storage_file_statics_GetFileFromPathAsync( IStorageFileStatics *iface, HSTRING path,
                                                                 IAsyncOperation_StorageFile **operation )
{
    struct storage_call *call;

    TRACE( "iface %p, path %s, operation %p.\n", iface, debugstr_hstring( path ), operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if ((call = storage_call_create( item_from_path_run, NULL, NULL, path, ITEM_FILE )))
        call->size = WindowsGetStringLen( path );
    START_CALL( async_operation_storage_file_create, call, operation );
}

static HRESULT WINAPI storage_file_statics_GetFileFromApplicationUriAsync( IStorageFileStatics *iface, IUriRuntimeClass *uri,
                                                                           IAsyncOperation_StorageFile **operation )
{
    struct storage_call *call;
    HSTRING text = NULL;
    HRESULT hr;

    TRACE( "iface %p, uri %p, operation %p.\n", iface, uri, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!uri) return E_INVALIDARG;
    if (FAILED(hr = IUriRuntimeClass_get_RawUri( uri, &text )) && FAILED(hr = IUriRuntimeClass_get_AbsoluteUri( uri, &text )))
        return hr;
    if (wcsnicmp( WindowsGetStringRawBuffer( text, NULL ), L"ms-appdata:", 11 ) &&
        wcsnicmp( WindowsGetStringRawBuffer( text, NULL ), L"ms-appx:", 8 ))
    {
        WindowsDeleteString( text );
        return E_INVALIDARG;
    }
    if ((call = storage_call_create( item_from_path_run, NULL, NULL, text, ITEM_FILE )))
        call->size = WindowsGetStringLen( text );
    WindowsDeleteString( text );
    START_CALL( async_operation_storage_file_create, call, operation );
}

static HRESULT WINAPI storage_file_statics_CreateStreamedFileAsync( IStorageFileStatics *iface, HSTRING name,
        IInspectable *handler, IInspectable *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, name %s, handler %p, thumbnail %p, operation %p stub!\n", iface, debugstr_hstring( name ), handler,
           thumbnail, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_file_statics_ReplaceWithStreamedFileAsync( IStorageFileStatics *iface, IStorageFile *file,
        IInspectable *handler, IInspectable *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, file %p, handler %p, thumbnail %p, operation %p stub!\n", iface, file, handler, thumbnail, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_file_statics_CreateStreamedFileFromUriAsync( IStorageFileStatics *iface, HSTRING name,
        IUriRuntimeClass *uri, IInspectable *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, name %s, uri %p, thumbnail %p, operation %p stub!\n", iface, debugstr_hstring( name ), uri, thumbnail,
           operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_file_statics_ReplaceWithStreamedFileFromUriAsync( IStorageFileStatics *iface, IStorageFile *file,
        IUriRuntimeClass *uri, IInspectable *thumbnail, IAsyncOperation_StorageFile **operation )
{
    FIXME( "iface %p, file %p, uri %p, thumbnail %p, operation %p stub!\n", iface, file, uri, thumbnail, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct IStorageFileStaticsVtbl storage_file_statics_vtbl =
{
    storage_file_statics_QueryInterface,
    storage_file_statics_AddRef,
    storage_file_statics_Release,
    /* IInspectable methods */
    storage_file_statics_GetIids,
    storage_file_statics_GetRuntimeClassName,
    storage_file_statics_GetTrustLevel,
    /* IStorageFileStatics methods */
    storage_file_statics_GetFileFromPathAsync,
    storage_file_statics_GetFileFromApplicationUriAsync,
    storage_file_statics_CreateStreamedFileAsync,
    storage_file_statics_ReplaceWithStreamedFileAsync,
    storage_file_statics_CreateStreamedFileFromUriAsync,
    storage_file_statics_ReplaceWithStreamedFileFromUriAsync,
};

static struct item_statics storage_folder_statics =
{
    {&factory_vtbl},
    {&storage_folder_statics_vtbl},
    {&storage_file_statics_vtbl},
    TRUE,
    0,
};

static struct item_statics storage_file_statics =
{
    {&factory_vtbl},
    {&storage_folder_statics_vtbl},
    {&storage_file_statics_vtbl},
    FALSE,
    0,
};

IActivationFactory *storage_folder_factory = &storage_folder_statics.IActivationFactory_iface;
IActivationFactory *storage_file_factory = &storage_file_statics.IActivationFactory_iface;
