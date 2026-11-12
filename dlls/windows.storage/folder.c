/* WinRT Windows.Storage.StorageFolder implementation
 *
 * Copyright (C) 2026
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

#include <assert.h>
#include "private.h"
#include "objbase.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

struct storage_folder
{
    IStorageFolder IStorageFolder_iface;
    IStorageItem IStorageItem_iface;
    LONG ref;

    HANDLE handle;
    HSTRING path;
    HSTRING name;
};

static inline struct storage_folder *impl_from_IStorageFolder( IStorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder, IStorageFolder_iface );
}

static HRESULT WINAPI storage_folder_QueryInterface( IStorageFolder *iface, REFIID iid, void **out )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out)
        return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStorageFolder ))
    {
        *out = iface;
    }
    else if (IsEqualGUID( iid, &IID_IStorageItem ))
    {
        *out = &impl->IStorageItem_iface;
    }
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    IStorageFolder_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI storage_folder_AddRef( IStorageFolder *iface )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI storage_folder_Release( IStorageFolder *iface )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->handle != INVALID_HANDLE_VALUE) CloseHandle( impl->handle );
        if (impl->path) WindowsDeleteString( impl->path );
        if (impl->name) WindowsDeleteString( impl->name );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI storage_folder_GetIids( IStorageFolder *iface, ULONG *iid_count, IID **iids )
{
    if (!iid_count || !iids)
        return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_GetRuntimeClassName( IStorageFolder *iface, HSTRING *class_name )
{
    if (!class_name)
        return E_POINTER;
    *class_name = NULL;
    return WindowsCreateString( L"Windows.Storage.StorageFolder",
            ARRAY_SIZE( L"Windows.Storage.StorageFolder" ) - 1, class_name );
}

static HRESULT WINAPI storage_folder_GetTrustLevel( IStorageFolder *iface, TrustLevel *trust_level )
{
    if (!trust_level)
        return E_POINTER;
    *trust_level = BaseTrust;
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

/* IStorageFolder mutation/enumeration methods are not supported. */

static HRESULT WINAPI storage_folder_CreateFileAsyncOverloadDefaultOptions( IStorageFolder *iface, HSTRING name,
        IAsyncOperation_StorageFile **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_CreateFileAsync( IStorageFolder *iface, HSTRING name,
        CreationCollisionOption options, IAsyncOperation_StorageFile **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, options %u, operation %p stub!\n", iface, debugstr_hstring( name ), options, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_CreateFolderAsyncOverloadDefaultOptions( IStorageFolder *iface, HSTRING name,
        IAsyncOperation_StorageFolder **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_CreateFolderAsync( IStorageFolder *iface, HSTRING name,
        CreationCollisionOption options, IAsyncOperation_StorageFolder **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, options %u, operation %p stub!\n", iface, debugstr_hstring( name ), options, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_GetFileAsync( IStorageFolder *iface, HSTRING name,
        IAsyncOperation_StorageFile **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_GetFolderAsync( IStorageFolder *iface, HSTRING name,
        IAsyncOperation_StorageFolder **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_GetItemAsync( IStorageFolder *iface, HSTRING name,
        IAsyncOperation_IStorageItem **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount( IStorageFolder *iface,
        IAsyncOperation_IVectorView_StorageFile **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount( IStorageFolder *iface,
        IAsyncOperation_IVectorView_StorageFolder **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_folder_GetItemsAsyncOverloadDefaultStartAndCount( IStorageFolder *iface,
        IAsyncOperation_IVectorView_IStorageItem **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

/* IStorageItem mutation methods are not supported. */

static HRESULT WINAPI storage_item_RenameAsyncOverloadDefaultOptions( IStorageItem *iface, HSTRING name,
        IAsyncAction **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, operation %p stub!\n", iface, debugstr_hstring( name ), operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_RenameAsync( IStorageItem *iface, HSTRING name, NameCollisionOption option,
        IAsyncAction **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, name %s, option %u, operation %p stub!\n", iface, debugstr_hstring( name ), option, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_DeleteAsyncOverloadDefaultOptions( IStorageItem *iface, IAsyncAction **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_DeleteAsync( IStorageItem *iface, StorageDeleteOption option,
        IAsyncAction **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, option %u, operation %p stub!\n", iface, option, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_GetBasicPropertiesAsync( IStorageItem *iface,
        IAsyncOperation_BasicProperties **operation )
{
    if (!operation)
        return E_POINTER;
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    *operation = NULL;
    return E_NOTIMPL;
}

/* IStorageItem read-only metadata. */

static HRESULT WINAPI storage_item_get_Name( IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = CONTAINING_RECORD( iface, struct storage_folder, IStorageItem_iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value)
        return E_POINTER;
    *value = NULL;
    if (!impl->name)
        return E_UNEXPECTED;

    return WindowsDuplicateString( impl->name, value );
}

static HRESULT WINAPI storage_item_get_Path( IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = CONTAINING_RECORD( iface, struct storage_folder, IStorageItem_iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value)
        return E_POINTER;
    *value = NULL;
    if (!impl->path)
        return E_UNEXPECTED;

    return WindowsDuplicateString( impl->path, value );
}

static HRESULT WINAPI storage_item_get_Attributes( IStorageItem *iface, FileAttributes *value )
{
    struct storage_folder *impl = CONTAINING_RECORD( iface, struct storage_folder, IStorageItem_iface );
    BY_HANDLE_FILE_INFORMATION info;
    FileAttributes attrs = FileAttributes_Normal;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value)
        return E_POINTER;
    *value = FileAttributes_Normal;

    if (!GetFileInformationByHandle( impl->handle, &info ))
        return HRESULT_FROM_WIN32( GetLastError() );

    if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) attrs |= FileAttributes_ReadOnly;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) attrs |= FileAttributes_Directory;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) attrs |= FileAttributes_Archive;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) attrs |= FileAttributes_Temporary;

    *value = attrs;
    return S_OK;
}

static HRESULT WINAPI storage_item_get_DateCreated( IStorageItem *iface, DateTime *value )
{
    struct storage_folder *impl = CONTAINING_RECORD( iface, struct storage_folder, IStorageItem_iface );
    BY_HANDLE_FILE_INFORMATION info;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value)
        return E_POINTER;
    value->UniversalTime = 0;

    if (!GetFileInformationByHandle( impl->handle, &info ))
        return HRESULT_FROM_WIN32( GetLastError() );

    value->UniversalTime = ((INT64)info.ftCreationTime.dwHighDateTime << 32) | info.ftCreationTime.dwLowDateTime;
    return S_OK;
}

static HRESULT WINAPI storage_item_IsOfType( IStorageItem *iface, StorageItemTypes type, boolean *value )
{
    TRACE( "iface %p, type %u, value %p.\n", iface, type, value );

    if (!value)
        return E_POINTER;
    *value = (type == StorageItemTypes_Folder);
    return S_OK;
}

DEFINE_IINSPECTABLE( storage_item, IStorageItem, struct storage_folder, IStorageFolder_iface )

static const struct IStorageFolderVtbl storage_folder_vtbl =
{
    /* IUnknown methods */
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

static const struct IStorageItemVtbl storage_item_vtbl =
{
    /* IUnknown methods */
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

static struct storage_folder *storage_folder_create( HANDLE handle, const WCHAR *path, UINT32 path_length )
{
    struct storage_folder *folder;
    const WCHAR *name_start;
    UINT32 name_length;
    UINT32 end;

    if (!(folder = calloc( 1, sizeof(*folder) ))) return NULL;
    folder->IStorageFolder_iface.lpVtbl = &storage_folder_vtbl;
    folder->IStorageItem_iface.lpVtbl = &storage_item_vtbl;
    folder->ref = 1;
    folder->handle = handle;

    if (FAILED(WindowsCreateString( path, path_length, &folder->path )))
    {
        free( folder );
        return NULL;
    }

    /* The name is the last path component, ignoring trailing separators. */
    end = path_length;
    while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) end--;
    name_start = path + end;
    while (name_start > path && path[name_start - path - 1] != L'\\' && path[name_start - path - 1] != L'/')
        name_start--;
    name_length = end - (name_start - path);
    if (!name_length) name_length = end; /* drive root such as "C:\" */

    if (FAILED(WindowsCreateString( name_start, name_length, &folder->name )))
    {
        WindowsDeleteString( folder->path );
        free( folder );
        return NULL;
    }

    return folder;
}

struct get_folder_async_params
{
    IUnknown IUnknown_iface;
    LONG ref;

    HSTRING path;
};

static void get_folder_async_params_destroy( struct get_folder_async_params *params )
{
    WindowsDeleteString( params->path );
}

DEFINE_ASYNC_PARAMS( get_folder_async_params )

static HRESULT get_folder_from_path_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct get_folder_async_params *params = get_folder_async_params_from_IUnknown( param );
    struct storage_folder *folder;
    UINT32 length, i;
    const WCHAR *path;
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;

    assert( !called_async );

    path = WindowsGetStringRawBuffer( params->path, &length );

    /* An empty path (including a NULL HSTRING) is an invalid argument (observed: 0x80070057). */
    if (length == 0)
        return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );

    /* Reject an embedded NUL code unit in the counted string. */
    for (i = 0; i < length; i++)
        if (path[i] == 0)
            return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );

    /* Reject relative paths; only an absolute drive path (letter, colon, backslash)
     * or a valid UNC server/share path is supported. Drive letters are case
     * insensitive. */
    if ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
    {
        if (length < 3 || path[1] != L':' || path[2] != L'\\')
            return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );
    }
    else if (length >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        /* UNC path: \\server\share. Reject device namespaces (\\.\, \\?\), a
         * third backslash (\\\), and require a nonempty ordinary server and
         * share (the first share code unit must not be a separator). */
        if (length < 3)
            return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );
        if (path[2] == L'.' || path[2] == L'?' || path[2] == L'\\')
            return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );
        for (i = 2; i < length; i++)
            if (path[i] == L'\\')
                break;
        /* i is the server/share separator, or length if there is no share. */
        if (i >= length || i + 1 >= length || path[i + 1] == L'\\')
            return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );
    }
    else
    {
        return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );
    }

    /* Reject overlong paths. */
    if (length > MAX_PATH)
        return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );

    handle = CreateFileW( path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL );
    if (handle == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32( GetLastError() );

    /* Verify the exact opened handle is a directory (prevents a directory-to-file race). */
    if (!GetFileInformationByHandle( handle, &info ))
    {
        DWORD err = GetLastError();
        CloseHandle( handle );
        return HRESULT_FROM_WIN32( err );
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        CloseHandle( handle );
        return HRESULT_FROM_WIN32( ERROR_INVALID_PARAMETER );
    }

    if (!(folder = storage_folder_create( handle, path, length )))
    {
        CloseHandle( handle );
        return E_OUTOFMEMORY;
    }

    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)&folder->IStorageFolder_iface;
    return S_OK;
}

struct storage_folder_factory
{
    IActivationFactory IActivationFactory_iface;
    IStorageFolderStatics IStorageFolderStatics_iface;
    LONG ref;
};

static inline struct storage_folder_factory *impl_from_storage_folder_factory_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder_factory, IActivationFactory_iface );
}

static HRESULT WINAPI storage_folder_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct storage_folder_factory *impl = impl_from_storage_folder_factory_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out)
        return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IStorageFolderStatics ))
    {
        *out = &impl->IStorageFolderStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI storage_folder_factory_AddRef( IActivationFactory *iface )
{
    struct storage_folder_factory *impl = impl_from_storage_folder_factory_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI storage_folder_factory_Release( IActivationFactory *iface )
{
    struct storage_folder_factory *impl = impl_from_storage_folder_factory_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI storage_folder_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    IID *array;

    if (!iid_count || !iids)
        return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    if (!(array = CoTaskMemAlloc( 2 * sizeof(*array) )))
        return E_OUTOFMEMORY;
    array[0] = IID_IActivationFactory;
    array[1] = IID_IStorageFolderStatics;
    *iid_count = 2;
    *iids = array;
    return S_OK;
}

static HRESULT WINAPI storage_folder_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    if (!class_name)
        return E_POINTER;
    *class_name = NULL;
    return E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI storage_folder_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    if (!trust_level)
        return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI storage_folder_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    if (!instance)
        return E_POINTER;
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl storage_folder_factory_vtbl =
{
    storage_folder_factory_QueryInterface,
    storage_folder_factory_AddRef,
    storage_folder_factory_Release,
    /* IInspectable methods */
    storage_folder_factory_GetIids,
    storage_folder_factory_GetRuntimeClassName,
    storage_folder_factory_GetTrustLevel,
    /* IActivationFactory methods */
    storage_folder_factory_ActivateInstance,
};

static inline struct storage_folder_factory *impl_from_IStorageFolderStatics( IStorageFolderStatics *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder_factory, IStorageFolderStatics_iface );
}

static HRESULT WINAPI storage_folder_statics_QueryInterface( IStorageFolderStatics *iface, REFIID iid, void **out )
{
    struct storage_folder_factory *impl = impl_from_IStorageFolderStatics( iface );

    if (!out)
        return E_POINTER;
    return IActivationFactory_QueryInterface( &impl->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI storage_folder_statics_AddRef( IStorageFolderStatics *iface )
{
    struct storage_folder_factory *impl = impl_from_IStorageFolderStatics( iface );
    return IActivationFactory_AddRef( &impl->IActivationFactory_iface );
}

static ULONG WINAPI storage_folder_statics_Release( IStorageFolderStatics *iface )
{
    struct storage_folder_factory *impl = impl_from_IStorageFolderStatics( iface );
    return IActivationFactory_Release( &impl->IActivationFactory_iface );
}

static HRESULT WINAPI storage_folder_statics_GetIids( IStorageFolderStatics *iface, ULONG *iid_count, IID **iids )
{
    IID *array;

    if (!iid_count || !iids)
        return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    if (!(array = CoTaskMemAlloc( sizeof(*array) )))
        return E_OUTOFMEMORY;
    array[0] = IID_IStorageFolderStatics;
    *iid_count = 1;
    *iids = array;
    return S_OK;
}

static HRESULT WINAPI storage_folder_statics_GetRuntimeClassName( IStorageFolderStatics *iface, HSTRING *class_name )
{
    if (!class_name)
        return E_POINTER;
    *class_name = NULL;
    return E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI storage_folder_statics_GetTrustLevel( IStorageFolderStatics *iface, TrustLevel *trust_level )
{
    if (!trust_level)
        return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI storage_folder_statics_GetFolderFromPathAsync( IStorageFolderStatics *iface, HSTRING path,
        IAsyncOperation_StorageFolder **operation )
{
    struct get_folder_async_params *params;
    HRESULT hr;

    TRACE( "iface %p, path %s, operation %p.\n", iface, debugstr_hstring( path ), operation );

    if (!operation)
        return E_POINTER;

    *operation = NULL;

    if (!(params = get_folder_async_params_alloc()))
        return E_OUTOFMEMORY;

    /* A NULL HSTRING is the canonical empty string, not a missing pointer. */
    if (FAILED(hr = WindowsDuplicateString( path, &params->path )))
    {
        IUnknown_Release( &params->IUnknown_iface );
        return hr;
    }

    hr = async_operation_storage_folder_create( (IUnknown *)iface, &params->IUnknown_iface,
            get_folder_from_path_async, operation );

    IUnknown_Release( &params->IUnknown_iface );

    return hr;
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

static struct storage_folder_factory storage_folder_factory_impl =
{
    {&storage_folder_factory_vtbl},
    {&storage_folder_statics_vtbl},
    0,
};

IActivationFactory *storage_folder_factory = &storage_folder_factory_impl.IActivationFactory_iface;
