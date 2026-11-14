/* WinRT Windows.Storage Implementation
 *
 * Copyright (C) 2025 Mohamad Al-Jaf
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

#ifndef __WINE_WINDOWS_STORAGE_PRIVATE_H
#define __WINE_WINDOWS_STORAGE_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "activation.h"

#include "wine/debug.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Storage
#define WIDL_using_Windows_Storage_Streams
#define WIDL_using_Windows_Storage_FileProperties
#include "windows.storage.h"
#include "windows.storage.streams.h"
#include "windows.storage.fileproperties.h"
#include "robuffer.h"
#include "async_private.h"
#include "storage_private.h"

HRESULT async_operation_buffer_uint32_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperationWithProgress_IBuffer_UINT32 **out );
HRESULT async_operation_uint32_uint32_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperationWithProgress_UINT32_UINT32 **out );
HRESULT async_operation_storage_folder_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_StorageFolder **out );

HRESULT async_operation_storage_file_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_StorageFile **out );
HRESULT async_operation_storage_item_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IStorageItem **out );
HRESULT async_operation_file_view_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IVectorView_StorageFile **out );
HRESULT async_operation_folder_view_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IVectorView_StorageFolder **out );
HRESULT async_operation_item_view_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IVectorView_IStorageItem **out );
HRESULT async_operation_basic_properties_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_BasicProperties **out );
HRESULT async_operation_hstring_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_HSTRING **out );
HRESULT async_operation_buffer_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IBuffer **out );
HRESULT async_operation_hstring_vector_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IVector_HSTRING **out );
HRESULT async_operation_boolean_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_boolean **out );
HRESULT async_operation_random_access_stream_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IRandomAccessStream **out );
HRESULT async_operation_typed_stream_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IRandomAccessStreamWithContentType **out );
HRESULT async_operation_input_stream_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_IInputStream **out );
HRESULT async_operation_transaction_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback,
        IAsyncOperation_StorageStreamTransaction **out );
HRESULT async_action_create( IUnknown *invoker, IUnknown *param, async_operation_callback callback, IAsyncAction **out );

/* filestream.c */
HRESULT file_stream_create( const WCHAR *path, BOOL writable, DWORD sharing, const WCHAR *content_type,
        IRandomAccessStreamWithContentType **out );
HRESULT stream_transaction_create( const WCHAR *path, const WCHAR *content_type, IStorageStreamTransaction **out );

/* vector.c */
struct vector_iids
{
    const GUID *iterable;
    const GUID *iterator;
    const GUID *vector;
    const GUID *view;
};
HRESULT vector_hstring_create( IVector_HSTRING **out );
HRESULT vector_hstring_create_copy( IIterable_HSTRING *iterable, IVector_HSTRING **out );
HRESULT vector_inspectable_create( const struct vector_iids *iids, IVector_IInspectable **out );

/* One call of a storage operation: the arguments are kept in it and run() does the work. The operations finish on
 * the calling thread (the asynchronous object is already completed when the caller gets it). */
struct storage_call
{
    IUnknown IUnknown_iface;
    LONG ref;

    HRESULT (*run)( struct storage_call *call, PROPVARIANT *result );
    IInspectable *object;  /* the item of the call, or NULL */
    IInspectable *other;   /* a second object: folder, file, buffer or collection */
    WCHAR *text;           /* a name or a path */
    WCHAR *contents;       /* text to write */
    UINT32 contents_length;
    BYTE *bytes;
    UINT32 size;
    UINT32 option;
    UINT32 flags;
};
struct storage_call *storage_call_create( HRESULT (*run)( struct storage_call *call, PROPVARIANT *result ),
        void *object, void *other, HSTRING text, UINT32 option );
HRESULT storage_call_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async );

/* folder.c */
HRESULT storage_item_get_path( void *item, WCHAR **path );
HRESULT storage_resolve_path( const WCHAR *path, WCHAR **out );

extern IActivationFactory *random_access_stream_reference_factory;
extern IActivationFactory *memory_stream_activation_factory;
extern IActivationFactory *storage_folder_factory;
extern IActivationFactory *storage_file_factory;
extern IActivationFactory *file_io_factory;
extern IActivationFactory *path_io_factory;

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )
#define DEFINE_IINSPECTABLE_OUTER( pfx, iface_type, impl_type, outer_iface )                       \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, impl->outer_iface )

#define DEFINE_ASYNC_PARAMS( type )                                                                \
    static struct type *type##_from_IUnknown( IUnknown *iface )                                    \
    {                                                                                              \
        return CONTAINING_RECORD( iface, struct type, IUnknown_iface );                            \
    }                                                                                              \
    static HRESULT WINAPI type##_QueryInterface( IUnknown *iface, REFIID iid, void **out )         \
    {                                                                                              \
        if (IsEqualIID( iid, &IID_IUnknown ))                                                      \
        {                                                                                          \
            IUnknown_AddRef( iface );                                                              \
            *out = iface;                                                                          \
            return S_OK;                                                                           \
        }                                                                                          \
        *out = NULL;                                                                               \
        return E_NOINTERFACE;                                                                      \
    }                                                                                              \
    static ULONG WINAPI type##_AddRef( IUnknown *iface )                                           \
    {                                                                                              \
        struct type *object = type##_from_IUnknown( iface );                                       \
        return InterlockedIncrement( &object->ref );                                               \
    }                                                                                              \
    static ULONG WINAPI type##_Release( IUnknown *iface )                                          \
    {                                                                                              \
        struct type *object = type##_from_IUnknown( iface );                                       \
        ULONG ref = InterlockedDecrement( &object->ref );                                          \
        if (!ref) type##_##destroy( object );                                                      \
        return ref;                                                                                \
    }                                                                                              \
    static const IUnknownVtbl type##_vtbl =                                                        \
    {                                                                                              \
        type##_QueryInterface,                                                                     \
        type##_AddRef,                                                                             \
        type##_Release,                                                                            \
    };                                                                                             \
    static struct type *type##_alloc(void)                                                         \
    {                                                                                              \
        struct type *object;                                                                       \
        if (!(object = calloc( 1, sizeof(*object) ))) return NULL;                                 \
        object->IUnknown_iface.lpVtbl = &type##_vtbl;                                              \
        object->ref = 1;                                                                           \
        return object;                                                                             \
    }

#endif
