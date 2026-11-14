/* WinRT streams of a file (StorageFile.OpenAsync, OpenReadAsync, OpenTransactedWriteAsync)
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

/* What Windows 11 did with the probe of WineExplorer (tests/uwp/u1_1 in that repository):
 *   - a stream has its own position; GetInputStreamAt, GetOutputStreamAt and CloneStream give streams with their
 *     own position on the same file;
 *   - setting the size leaves the position alone, and does nothing (with success) on a stream that cannot write;
 *   - a read past the end gives an empty buffer; a count over the capacity of the buffer is E_INVALIDARG;
 *   - writing to a stream opened for reading is E_ACCESSDENIED;
 *   - a closed stream answers RO_E_CLOSED; closing twice is fine;
 *   - a transacted write works on a copy of the file, which replaces the file at the commit. */

#include <stdlib.h>
#include "private.h"
#include "objbase.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

/* the open file, shared by a stream and the streams made from it */
struct stream_file
{
    LONG ref;
    HANDLE handle;
    BOOL writable;
    CRITICAL_SECTION cs; /* the file pointer of the handle */
};

struct file_stream
{
    IRandomAccessStream IRandomAccessStream_iface;
    IRandomAccessStreamWithContentType IRandomAccessStreamWithContentType_iface;
    IContentTypeProvider IContentTypeProvider_iface;
    IInputStream IInputStream_iface;
    IOutputStream IOutputStream_iface;
    IClosable IClosable_iface;
    LONG ref;

    SRWLOCK lock;
    struct stream_file *file; /* NULL when the stream is closed */
    UINT64 position;
    WCHAR *content_type;
};

static HRESULT last_error(void)
{
    return HRESULT_FROM_WIN32( GetLastError() );
}

static void stream_file_release( struct stream_file *file )
{
    if (InterlockedDecrement( &file->ref )) return;
    CloseHandle( file->handle );
    DeleteCriticalSection( &file->cs );
    free( file );
}

/* the file of a stream with a reference, or NULL when the stream is closed */
static struct stream_file *stream_get_file( struct file_stream *impl )
{
    struct stream_file *file;

    AcquireSRWLockShared( &impl->lock );
    if ((file = impl->file)) InterlockedIncrement( &file->ref );
    ReleaseSRWLockShared( &impl->lock );
    return file;
}

static HRESULT file_stream_alloc( struct stream_file *file, UINT64 position, const WCHAR *content_type,
                                  struct file_stream **out );

static inline struct file_stream *impl_from_IRandomAccessStream( IRandomAccessStream *iface )
{
    return CONTAINING_RECORD( iface, struct file_stream, IRandomAccessStream_iface );
}

static HRESULT WINAPI file_stream_QueryInterface( IRandomAccessStream *iface, REFIID iid, void **out )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IRandomAccessStream ))
        *out = &impl->IRandomAccessStream_iface;
    else if (IsEqualGUID( iid, &IID_IRandomAccessStreamWithContentType ))
        *out = &impl->IRandomAccessStreamWithContentType_iface;
    else if (IsEqualGUID( iid, &IID_IContentTypeProvider ))
        *out = &impl->IContentTypeProvider_iface;
    else if (IsEqualGUID( iid, &IID_IInputStream ))
        *out = &impl->IInputStream_iface;
    else if (IsEqualGUID( iid, &IID_IOutputStream ))
        *out = &impl->IOutputStream_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    IRandomAccessStream_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI file_stream_AddRef( IRandomAccessStream *iface )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI file_stream_Release( IRandomAccessStream *iface )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->file) stream_file_release( impl->file );
        free( impl->content_type );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI file_stream_GetIids( IRandomAccessStream *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI file_stream_GetRuntimeClassName( IRandomAccessStream *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Storage.Streams.FileRandomAccessStream";
    return WindowsCreateString( name, ARRAY_SIZE(name) - 1, class_name );
}

static HRESULT WINAPI file_stream_GetTrustLevel( IRandomAccessStream *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI file_stream_get_Size( IRandomAccessStream *iface, UINT64 *value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    struct stream_file *file;
    LARGE_INTEGER size;
    HRESULT hr = S_OK;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = 0;
    if (!(file = stream_get_file( impl ))) return RO_E_CLOSED;
    if (GetFileSizeEx( file->handle, &size )) *value = size.QuadPart;
    else hr = last_error();
    stream_file_release( file );
    return hr;
}

static HRESULT WINAPI file_stream_put_Size( IRandomAccessStream *iface, UINT64 value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    struct stream_file *file;
    LARGE_INTEGER size;
    HRESULT hr = S_OK;

    TRACE( "iface %p, value %I64u.\n", iface, value );

    if (!(file = stream_get_file( impl ))) return RO_E_CLOSED;
    if (file->writable)
    {
        size.QuadPart = value;
        EnterCriticalSection( &file->cs );
        if (!SetFilePointerEx( file->handle, size, NULL, FILE_BEGIN ) || !SetEndOfFile( file->handle )) hr = last_error();
        LeaveCriticalSection( &file->cs );
    }
    stream_file_release( file );
    return hr;
}

static HRESULT file_stream_at( struct file_stream *impl, UINT64 position, struct file_stream **out )
{
    struct stream_file *file;
    HRESULT hr;

    *out = NULL;
    if (!(file = stream_get_file( impl ))) return RO_E_CLOSED;
    if (FAILED(hr = file_stream_alloc( file, position, impl->content_type, out ))) stream_file_release( file );
    return hr;
}

static HRESULT WINAPI file_stream_GetInputStreamAt( IRandomAccessStream *iface, UINT64 position, IInputStream **stream )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface ), *other;
    HRESULT hr;

    TRACE( "iface %p, position %I64u, stream %p.\n", iface, position, stream );

    if (!stream) return E_POINTER;
    *stream = NULL;
    if (SUCCEEDED(hr = file_stream_at( impl, position, &other ))) *stream = &other->IInputStream_iface;
    return hr;
}

static HRESULT WINAPI file_stream_GetOutputStreamAt( IRandomAccessStream *iface, UINT64 position, IOutputStream **stream )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface ), *other;
    HRESULT hr;

    TRACE( "iface %p, position %I64u, stream %p.\n", iface, position, stream );

    if (!stream) return E_POINTER;
    *stream = NULL;
    if (SUCCEEDED(hr = file_stream_at( impl, position, &other ))) *stream = &other->IOutputStream_iface;
    return hr;
}

static HRESULT WINAPI file_stream_get_Position( IRandomAccessStream *iface, UINT64 *value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    AcquireSRWLockShared( &impl->lock );
    *value = impl->position;
    hr = impl->file ? S_OK : RO_E_CLOSED;
    ReleaseSRWLockShared( &impl->lock );
    return hr;
}

static HRESULT WINAPI file_stream_Seek( IRandomAccessStream *iface, UINT64 position )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    HRESULT hr = S_OK;

    TRACE( "iface %p, position %I64u.\n", iface, position );

    AcquireSRWLockExclusive( &impl->lock );
    if (impl->file) impl->position = position;
    else hr = RO_E_CLOSED;
    ReleaseSRWLockExclusive( &impl->lock );
    return hr;
}

static HRESULT WINAPI file_stream_CloneStream( IRandomAccessStream *iface, IRandomAccessStream **stream )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface ), *other;
    HRESULT hr;

    TRACE( "iface %p, stream %p.\n", iface, stream );

    if (!stream) return E_POINTER;
    *stream = NULL;
    if (SUCCEEDED(hr = file_stream_at( impl, 0, &other ))) *stream = &other->IRandomAccessStream_iface;
    return hr;
}

static HRESULT WINAPI file_stream_get_CanRead( IRandomAccessStream *iface, BOOLEAN *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI file_stream_get_CanWrite( IRandomAccessStream *iface, BOOLEAN *value )
{
    struct file_stream *impl = impl_from_IRandomAccessStream( iface );
    struct stream_file *file;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = FALSE;
    if (!(file = stream_get_file( impl ))) return RO_E_CLOSED;
    *value = !!file->writable;
    stream_file_release( file );
    return S_OK;
}

static const struct IRandomAccessStreamVtbl file_stream_vtbl =
{
    file_stream_QueryInterface,
    file_stream_AddRef,
    file_stream_Release,
    /* IInspectable methods */
    file_stream_GetIids,
    file_stream_GetRuntimeClassName,
    file_stream_GetTrustLevel,
    /* IRandomAccessStream methods */
    file_stream_get_Size,
    file_stream_put_Size,
    file_stream_GetInputStreamAt,
    file_stream_GetOutputStreamAt,
    file_stream_get_Position,
    file_stream_Seek,
    file_stream_CloneStream,
    file_stream_get_CanRead,
    file_stream_get_CanWrite,
};

DEFINE_IINSPECTABLE( file_stream_typed, IRandomAccessStreamWithContentType, struct file_stream, IRandomAccessStream_iface )

static const struct IRandomAccessStreamWithContentTypeVtbl file_stream_typed_vtbl =
{
    file_stream_typed_QueryInterface,
    file_stream_typed_AddRef,
    file_stream_typed_Release,
    /* IInspectable methods */
    file_stream_typed_GetIids,
    file_stream_typed_GetRuntimeClassName,
    file_stream_typed_GetTrustLevel,
};

DEFINE_IINSPECTABLE( file_stream_content_type, IContentTypeProvider, struct file_stream, IRandomAccessStream_iface )

static HRESULT WINAPI file_stream_content_type_get_ContentType( IContentTypeProvider *iface, HSTRING *value )
{
    struct file_stream *impl = impl_from_IContentTypeProvider( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    return WindowsCreateString( impl->content_type, wcslen( impl->content_type ), value );
}

static const struct IContentTypeProviderVtbl file_stream_content_type_vtbl =
{
    file_stream_content_type_QueryInterface,
    file_stream_content_type_AddRef,
    file_stream_content_type_Release,
    /* IInspectable methods */
    file_stream_content_type_GetIids,
    file_stream_content_type_GetRuntimeClassName,
    file_stream_content_type_GetTrustLevel,
    /* IContentTypeProvider methods */
    file_stream_content_type_get_ContentType,
};

DEFINE_IINSPECTABLE( file_stream_closable, IClosable, struct file_stream, IRandomAccessStream_iface )

static HRESULT WINAPI file_stream_closable_Close( IClosable *iface )
{
    struct file_stream *impl = impl_from_IClosable( iface );
    struct stream_file *file;

    TRACE( "iface %p.\n", iface );

    AcquireSRWLockExclusive( &impl->lock );
    file = impl->file;
    impl->file = NULL;
    ReleaseSRWLockExclusive( &impl->lock );
    if (file) stream_file_release( file );
    return S_OK;
}

static const struct IClosableVtbl file_stream_closable_vtbl =
{
    file_stream_closable_QueryInterface,
    file_stream_closable_AddRef,
    file_stream_closable_Release,
    /* IInspectable methods */
    file_stream_closable_GetIids,
    file_stream_closable_GetRuntimeClassName,
    file_stream_closable_GetTrustLevel,
    /* IClosable methods */
    file_stream_closable_Close,
};

static inline struct file_stream *stream_from_call( struct storage_call *call )
{
    return impl_from_IRandomAccessStream( (IRandomAccessStream *)call->object );
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

/* other: the buffer; option: the count of bytes wanted */
static HRESULT stream_read_run( struct storage_call *call, PROPVARIANT *result )
{
    struct file_stream *impl = stream_from_call( call );
    IBuffer *buffer = (IBuffer *)call->other;
    IBufferByteAccess *access;
    struct stream_file *file;
    UINT32 capacity, done = 0;
    LARGE_INTEGER offset;
    HRESULT hr;
    DWORD got;
    BYTE *data;

    if (FAILED(hr = IBuffer_get_Capacity( buffer, &capacity ))) return hr;
    if (capacity < call->option) return E_INVALIDARG;
    if (FAILED(hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access ))) return hr;
    hr = IBufferByteAccess_Buffer( access, &data );
    IBufferByteAccess_Release( access );
    if (FAILED(hr)) return hr;
    if (!(file = stream_get_file( impl ))) return RO_E_CLOSED;

    AcquireSRWLockExclusive( &impl->lock );
    offset.QuadPart = impl->position;
    EnterCriticalSection( &file->cs );
    if (!SetFilePointerEx( file->handle, offset, NULL, FILE_BEGIN )) hr = last_error();
    while (SUCCEEDED(hr) && done < call->option)
    {
        if (!ReadFile( file->handle, data + done, call->option - done, &got, NULL )) hr = last_error();
        else if (!got) break;
        else done += got;
    }
    LeaveCriticalSection( &file->cs );
    if (SUCCEEDED(hr)) impl->position += done;
    ReleaseSRWLockExclusive( &impl->lock );
    stream_file_release( file );
    if (FAILED(hr)) return hr;

    IBuffer_put_Length( buffer, done );
    result->vt = VT_UNKNOWN;
    IBuffer_AddRef( buffer );
    result->punkVal = (IUnknown *)buffer;
    return S_OK;
}

/* other: the buffer */
static HRESULT stream_write_run( struct storage_call *call, PROPVARIANT *result )
{
    struct file_stream *impl = stream_from_call( call );
    IBuffer *buffer = (IBuffer *)call->other;
    IBufferByteAccess *access;
    struct stream_file *file;
    UINT32 length, done = 0;
    LARGE_INTEGER offset;
    DWORD written;
    HRESULT hr;
    BYTE *data;

    if (FAILED(hr = IBuffer_get_Length( buffer, &length ))) return hr;
    if (FAILED(hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access ))) return hr;
    hr = IBufferByteAccess_Buffer( access, &data );
    IBufferByteAccess_Release( access );
    if (FAILED(hr)) return hr;
    if (!(file = stream_get_file( impl ))) return RO_E_CLOSED;
    if (!file->writable)
    {
        stream_file_release( file );
        return E_ACCESSDENIED;
    }

    AcquireSRWLockExclusive( &impl->lock );
    offset.QuadPart = impl->position;
    EnterCriticalSection( &file->cs );
    if (!SetFilePointerEx( file->handle, offset, NULL, FILE_BEGIN )) hr = last_error();
    while (SUCCEEDED(hr) && done < length)
    {
        if (!WriteFile( file->handle, data + done, length - done, &written, NULL )) hr = last_error();
        else done += written;
    }
    LeaveCriticalSection( &file->cs );
    if (SUCCEEDED(hr)) impl->position += done;
    ReleaseSRWLockExclusive( &impl->lock );
    stream_file_release( file );
    if (FAILED(hr)) return hr;

    result->vt = VT_UI4;
    result->ulVal = done;
    return S_OK;
}

static HRESULT stream_flush_run( struct storage_call *call, PROPVARIANT *result )
{
    struct file_stream *impl = stream_from_call( call );
    struct stream_file *file;

    if (!(file = stream_get_file( impl ))) return RO_E_CLOSED;
    if (file->writable) FlushFileBuffers( file->handle );
    stream_file_release( file );
    result->vt = VT_UI1;
    result->bVal = TRUE;
    return S_OK;
}

DEFINE_IINSPECTABLE( file_stream_input, IInputStream, struct file_stream, IRandomAccessStream_iface )

static HRESULT WINAPI file_stream_input_ReadAsync( IInputStream *iface, IBuffer *buffer, UINT32 count,
        InputStreamOptions options, IAsyncOperationWithProgress_IBuffer_UINT32 **operation )
{
    struct file_stream *impl = impl_from_IInputStream( iface );

    TRACE( "iface %p, buffer %p, count %u, options %d, operation %p.\n", iface, buffer, count, options, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!buffer) return E_POINTER;
    START_CALL( async_operation_buffer_uint32_create,
                storage_call_create( stream_read_run, &impl->IRandomAccessStream_iface, buffer, NULL, count ), operation );
}

static const struct IInputStreamVtbl file_stream_input_vtbl =
{
    file_stream_input_QueryInterface,
    file_stream_input_AddRef,
    file_stream_input_Release,
    /* IInspectable methods */
    file_stream_input_GetIids,
    file_stream_input_GetRuntimeClassName,
    file_stream_input_GetTrustLevel,
    /* IInputStream methods */
    file_stream_input_ReadAsync,
};

DEFINE_IINSPECTABLE( file_stream_output, IOutputStream, struct file_stream, IRandomAccessStream_iface )

static HRESULT WINAPI file_stream_output_WriteAsync( IOutputStream *iface, IBuffer *buffer,
        IAsyncOperationWithProgress_UINT32_UINT32 **operation )
{
    struct file_stream *impl = impl_from_IOutputStream( iface );

    TRACE( "iface %p, buffer %p, operation %p.\n", iface, buffer, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (!buffer) return E_POINTER;
    START_CALL( async_operation_uint32_uint32_create,
                storage_call_create( stream_write_run, &impl->IRandomAccessStream_iface, buffer, NULL, 0 ), operation );
}

static HRESULT WINAPI file_stream_output_FlushAsync( IOutputStream *iface, IAsyncOperation_boolean **operation )
{
    struct file_stream *impl = impl_from_IOutputStream( iface );

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    START_CALL( async_operation_boolean_create,
                storage_call_create( stream_flush_run, &impl->IRandomAccessStream_iface, NULL, NULL, 0 ), operation );
}

static const struct IOutputStreamVtbl file_stream_output_vtbl =
{
    file_stream_output_QueryInterface,
    file_stream_output_AddRef,
    file_stream_output_Release,
    /* IInspectable methods */
    file_stream_output_GetIids,
    file_stream_output_GetRuntimeClassName,
    file_stream_output_GetTrustLevel,
    /* IOutputStream methods */
    file_stream_output_WriteAsync,
    file_stream_output_FlushAsync,
};

/* takes the reference of the caller on file */
static HRESULT file_stream_alloc( struct stream_file *file, UINT64 position, const WCHAR *content_type,
                                  struct file_stream **out )
{
    struct file_stream *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IRandomAccessStream_iface.lpVtbl = &file_stream_vtbl;
    impl->IRandomAccessStreamWithContentType_iface.lpVtbl = &file_stream_typed_vtbl;
    impl->IContentTypeProvider_iface.lpVtbl = &file_stream_content_type_vtbl;
    impl->IInputStream_iface.lpVtbl = &file_stream_input_vtbl;
    impl->IOutputStream_iface.lpVtbl = &file_stream_output_vtbl;
    impl->IClosable_iface.lpVtbl = &file_stream_closable_vtbl;
    impl->ref = 1;
    InitializeSRWLock( &impl->lock );
    if (!(impl->content_type = wcsdup( content_type ? content_type : L"" )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    impl->file = file;
    impl->position = position;
    *out = impl;
    return S_OK;
}

HRESULT file_stream_create( const WCHAR *path, BOOL writable, DWORD sharing, const WCHAR *content_type,
                            IRandomAccessStreamWithContentType **out )
{
    struct file_stream *impl;
    struct stream_file *file;
    HRESULT hr;

    *out = NULL;
    if (!(file = calloc( 1, sizeof(*file) ))) return E_OUTOFMEMORY;
    file->ref = 1;
    file->writable = writable;
    file->handle = CreateFileW( path, GENERIC_READ | (writable ? GENERIC_WRITE : 0), sharing, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL );
    if (file->handle == INVALID_HANDLE_VALUE)
    {
        hr = last_error();
        free( file );
        return hr;
    }
    InitializeCriticalSection( &file->cs );

    if (FAILED(hr = file_stream_alloc( file, 0, content_type, &impl )))
    {
        stream_file_release( file );
        return hr;
    }
    *out = &impl->IRandomAccessStreamWithContentType_iface;
    return S_OK;
}

/* ---- StorageStreamTransaction ------------------------------------------------------------------------- */

struct stream_transaction
{
    IStorageStreamTransaction IStorageStreamTransaction_iface;
    IClosable IClosable_iface;
    LONG ref;

    IRandomAccessStream *stream;
    WCHAR *target;
    WCHAR *copy;
    LONG committed;
    LONG closed;
};

static inline struct stream_transaction *impl_from_IStorageStreamTransaction( IStorageStreamTransaction *iface )
{
    return CONTAINING_RECORD( iface, struct stream_transaction, IStorageStreamTransaction_iface );
}

static void transaction_close( struct stream_transaction *impl )
{
    IClosable *closable;

    if (InterlockedExchange( &impl->closed, TRUE )) return;
    if (SUCCEEDED(IRandomAccessStream_QueryInterface( impl->stream, &IID_IClosable, (void **)&closable )))
    {
        IClosable_Close( closable );
        IClosable_Release( closable );
    }
    if (!impl->committed) DeleteFileW( impl->copy );
}

static HRESULT WINAPI transaction_QueryInterface( IStorageStreamTransaction *iface, REFIID iid, void **out )
{
    struct stream_transaction *impl = impl_from_IStorageStreamTransaction( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IStorageStreamTransaction ))
        *out = &impl->IStorageStreamTransaction_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    IStorageStreamTransaction_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI transaction_AddRef( IStorageStreamTransaction *iface )
{
    return InterlockedIncrement( &impl_from_IStorageStreamTransaction( iface )->ref );
}

static ULONG WINAPI transaction_Release( IStorageStreamTransaction *iface )
{
    struct stream_transaction *impl = impl_from_IStorageStreamTransaction( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        transaction_close( impl );
        IRandomAccessStream_Release( impl->stream );
        free( impl->target );
        free( impl->copy );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI transaction_GetIids( IStorageStreamTransaction *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI transaction_GetRuntimeClassName( IStorageStreamTransaction *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_StorageStreamTransaction,
                                wcslen( RuntimeClass_Windows_Storage_StorageStreamTransaction ), class_name );
}

static HRESULT WINAPI transaction_GetTrustLevel( IStorageStreamTransaction *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI transaction_get_Stream( IStorageStreamTransaction *iface, IRandomAccessStream **value )
{
    struct stream_transaction *impl = impl_from_IStorageStreamTransaction( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    IRandomAccessStream_AddRef( (*value = impl->stream) );
    return S_OK;
}

static HRESULT transaction_commit_run( struct storage_call *call, PROPVARIANT *result )
{
    struct stream_transaction *impl = impl_from_IStorageStreamTransaction( (IStorageStreamTransaction *)call->object );
    IOutputStream *output;

    if (impl->closed) return RO_E_CLOSED;
    if (impl->committed) return S_OK;
    if (SUCCEEDED(IRandomAccessStream_QueryInterface( impl->stream, &IID_IOutputStream, (void **)&output )))
    {
        IAsyncOperation_boolean *flush;
        if (SUCCEEDED(IOutputStream_FlushAsync( output, &flush ))) IAsyncOperation_boolean_Release( flush );
        IOutputStream_Release( output );
    }
    /* the copy is open with FILE_SHARE_DELETE: it can take the place of the file while the stream lives */
    if (!MoveFileExW( impl->copy, impl->target, MOVEFILE_REPLACE_EXISTING )) return last_error();
    InterlockedExchange( &impl->committed, TRUE );
    return S_OK;
}

static HRESULT WINAPI transaction_CommitAsync( IStorageStreamTransaction *iface, IAsyncAction **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (!operation) return E_POINTER;
    START_CALL( async_action_create, storage_call_create( transaction_commit_run, iface, NULL, NULL, 0 ), operation );
}

static const struct IStorageStreamTransactionVtbl transaction_vtbl =
{
    transaction_QueryInterface,
    transaction_AddRef,
    transaction_Release,
    /* IInspectable methods */
    transaction_GetIids,
    transaction_GetRuntimeClassName,
    transaction_GetTrustLevel,
    /* IStorageStreamTransaction methods */
    transaction_get_Stream,
    transaction_CommitAsync,
};

DEFINE_IINSPECTABLE_( transaction_closable, IClosable, struct stream_transaction, transaction_from_IClosable,
                      IClosable_iface, &impl->IStorageStreamTransaction_iface )

static HRESULT WINAPI transaction_closable_Close( IClosable *iface )
{
    TRACE( "iface %p.\n", iface );

    transaction_close( transaction_from_IClosable( iface ) );
    return S_OK;
}

static const struct IClosableVtbl transaction_closable_vtbl =
{
    transaction_closable_QueryInterface,
    transaction_closable_AddRef,
    transaction_closable_Release,
    /* IInspectable methods */
    transaction_closable_GetIids,
    transaction_closable_GetRuntimeClassName,
    transaction_closable_GetTrustLevel,
    /* IClosable methods */
    transaction_closable_Close,
};

HRESULT stream_transaction_create( const WCHAR *path, const WCHAR *content_type, IStorageStreamTransaction **out )
{
    IRandomAccessStreamWithContentType *typed;
    struct stream_transaction *impl;
    size_t length = wcslen( path );
    unsigned int n;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IStorageStreamTransaction_iface.lpVtbl = &transaction_vtbl;
    impl->IClosable_iface.lpVtbl = &transaction_closable_vtbl;
    impl->ref = 1;
    impl->target = wcsdup( path );
    impl->copy = malloc( (length + 24) * sizeof(WCHAR) );
    if (!impl->target || !impl->copy)
    {
        hr = E_OUTOFMEMORY;
        goto failed;
    }

    /* a copy beside the file, so that it can replace the file with a rename */
    for (n = 0, hr = HRESULT_FROM_WIN32( ERROR_FILE_EXISTS ); n < 1000; n++)
    {
        swprintf( impl->copy, length + 24, L"%s.~tmp%x", path, GetTickCount() + n );
        if (CopyFileW( path, impl->copy, TRUE ))
        {
            hr = S_OK;
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS)
        {
            hr = last_error();
            break;
        }
    }
    if (FAILED(hr)) goto failed;

    if (FAILED(hr = file_stream_create( impl->copy, TRUE, FILE_SHARE_READ | FILE_SHARE_DELETE, content_type, &typed )))
    {
        DeleteFileW( impl->copy );
        goto failed;
    }
    hr = IRandomAccessStreamWithContentType_QueryInterface( typed, &IID_IRandomAccessStream, (void **)&impl->stream );
    IRandomAccessStreamWithContentType_Release( typed );
    if (FAILED(hr))
    {
        DeleteFileW( impl->copy );
        goto failed;
    }

    *out = &impl->IStorageStreamTransaction_iface;
    return S_OK;

failed:
    free( impl->target );
    free( impl->copy );
    free( impl );
    return hr;
}
