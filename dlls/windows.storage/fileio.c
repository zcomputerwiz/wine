/* WinRT Windows.Storage.FileIO and Windows.Storage.PathIO implementation
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
 *   - text is written as UTF-8 without a byte order mark; UTF-16 text is written with one;
 *   - a write replaces the content, whatever the encoding of the old content was;
 *   - a byte order mark in a file that is read decides the encoding, and is not part of the text;
 *   - lines are written with CR LF after each one;
 *   - the file must exist (PathIO does not create it). */

#include <stdlib.h>
#include "private.h"
#include "objbase.h"
#include "winnls.h"
#include "roapi.h"

WINE_DEFAULT_DEBUG_CHANNEL(storage);

#define IO_APPEND 1

static HRESULT last_error(void)
{
    return HRESULT_FROM_WIN32( GetLastError() );
}

static HRESULT call_path( struct storage_call *call, WCHAR **path )
{
    if (call->object) return storage_item_get_path( call->object, path );
    return storage_resolve_path( call->text, path );
}

static HRESULT read_file( const WCHAR *path, BYTE **data, DWORD *size )
{
    LARGE_INTEGER length;
    DWORD done = 0, got;
    HANDLE file;
    HRESULT hr = S_OK;

    *data = NULL;
    *size = 0;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return last_error();
    if (!GetFileSizeEx( file, &length )) hr = last_error();
    else if (length.QuadPart > 0x7fffffff) hr = E_OUTOFMEMORY;
    else if (!(*data = malloc( length.QuadPart + 1 ))) hr = E_OUTOFMEMORY;
    while (SUCCEEDED(hr) && done < length.QuadPart)
    {
        if (!ReadFile( file, *data + done, length.QuadPart - done, &got, NULL )) hr = last_error();
        else if (!got) break;
        else done += got;
    }
    CloseHandle( file );
    if (FAILED(hr))
    {
        free( *data );
        *data = NULL;
        return hr;
    }
    *size = done;
    return S_OK;
}

static HRESULT write_file( HANDLE file, const BYTE *data, DWORD size )
{
    DWORD done = 0, written;

    while (done < size)
    {
        if (!WriteFile( file, data + done, size - done, &written, NULL )) return last_error();
        done += written;
    }
    return S_OK;
}

/* open an existing file to replace its content or to add to it; for adding, mark gets its first bytes */
static HRESULT open_for_write( const WCHAR *path, BOOL append, HANDLE *file, BYTE mark[3], DWORD *mark_size )
{
    LARGE_INTEGER zero = {0};
    HRESULT hr;

    *mark_size = 0;
    *file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (*file == INVALID_HANDLE_VALUE) return last_error();
    if (append)
    {
        if (!ReadFile( *file, mark, 3, mark_size, NULL )) *mark_size = 0;
        if (SetFilePointerEx( *file, zero, NULL, FILE_END )) return S_OK;
    }
    else if (SetFilePointerEx( *file, zero, NULL, FILE_BEGIN ) && SetEndOfFile( *file )) return S_OK;

    hr = last_error();
    CloseHandle( *file );
    *file = INVALID_HANDLE_VALUE;
    return hr;
}

static UnicodeEncoding detect_mark( const BYTE *data, DWORD size, UnicodeEncoding fallback, DWORD *skip )
{
    *skip = 0;
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) { *skip = 3; return UnicodeEncoding_Utf8; }
    if (size >= 2 && data[0] == 0xff && data[1] == 0xfe) { *skip = 2; return UnicodeEncoding_Utf16LE; }
    if (size >= 2 && data[0] == 0xfe && data[1] == 0xff) { *skip = 2; return UnicodeEncoding_Utf16BE; }
    return fallback;
}

/* the text of the bytes of a file, in memory of CoTaskMemAlloc, with a terminating zero */
static HRESULT decode_text( const BYTE *data, DWORD size, UnicodeEncoding encoding, WCHAR **text, UINT32 *length )
{
    DWORD skip, i;

    encoding = detect_mark( data, size, encoding, &skip );
    data += skip;
    size -= skip;
    *text = NULL;
    *length = 0;

    if (encoding == UnicodeEncoding_Utf8)
    {
        int count = 0;

        if (size && !(count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, size, NULL, 0 )))
            return HRESULT_FROM_WIN32( ERROR_NO_UNICODE_TRANSLATION );
        if (!(*text = CoTaskMemAlloc( (count + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
        if (size) MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)data, size, *text, count );
        *length = count;
    }
    else
    {
        if (size & 1) return HRESULT_FROM_WIN32( ERROR_NO_UNICODE_TRANSLATION );
        if (!(*text = CoTaskMemAlloc( size + sizeof(WCHAR) ))) return E_OUTOFMEMORY;
        *length = size / sizeof(WCHAR);
        for (i = 0; i < *length; i++)
        {
            if (encoding == UnicodeEncoding_Utf16LE) (*text)[i] = data[2 * i] | (data[2 * i + 1] << 8);
            else (*text)[i] = data[2 * i + 1] | (data[2 * i] << 8);
        }
    }
    (*text)[*length] = 0;
    return S_OK;
}

static HRESULT encode_text( const WCHAR *text, UINT32 length, UnicodeEncoding encoding, BOOL mark, BYTE **data, DWORD *size )
{
    UINT32 i;
    BYTE *p;

    *data = NULL;
    *size = 0;
    if (encoding == UnicodeEncoding_Utf8)
    {
        int count = length ? WideCharToMultiByte( CP_UTF8, 0, text, length, NULL, 0, NULL, NULL ) : 0;

        if (length && !count) return last_error();
        if (!(*data = malloc( count + 1 ))) return E_OUTOFMEMORY;
        if (length) WideCharToMultiByte( CP_UTF8, 0, text, length, (char *)*data, count, NULL, NULL );
        *size = count;
        return S_OK;
    }
    if (encoding != UnicodeEncoding_Utf16LE && encoding != UnicodeEncoding_Utf16BE) return E_INVALIDARG;

    if (!(*data = malloc( (length + 1) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    p = *data;
    if (mark)
    {
        *p++ = encoding == UnicodeEncoding_Utf16LE ? 0xff : 0xfe;
        *p++ = encoding == UnicodeEncoding_Utf16LE ? 0xfe : 0xff;
    }
    for (i = 0; i < length; i++)
    {
        if (encoding == UnicodeEncoding_Utf16LE) { *p++ = text[i] & 0xff; *p++ = text[i] >> 8; }
        else { *p++ = text[i] >> 8; *p++ = text[i] & 0xff; }
    }
    *size = p - *data;
    return S_OK;
}

static HRESULT write_text( const WCHAR *path, const WCHAR *text, UINT32 length, UnicodeEncoding encoding, BOOL append )
{
    DWORD size, mark_size, skip;
    BOOL mark = TRUE;
    BYTE existing[3], *data;
    HANDLE file;
    HRESULT hr;

    if (encoding != UnicodeEncoding_Utf8 && encoding != UnicodeEncoding_Utf16LE && encoding != UnicodeEncoding_Utf16BE)
        return E_INVALIDARG;
    if (FAILED(hr = open_for_write( path, append, &file, existing, &mark_size ))) return hr;
    if (append && mark_size)
    {
        /* text is added in the encoding the file has */
        encoding = detect_mark( existing, mark_size, encoding, &skip );
        mark = FALSE;
    }
    if (SUCCEEDED(hr = encode_text( text, length, encoding, mark, &data, &size )))
    {
        hr = write_file( file, data, size );
        free( data );
    }
    CloseHandle( file );
    return hr;
}

/* ---- the operations ----------------------------------------------------------------------------------- */

static HRESULT read_text_run( struct storage_call *call, PROPVARIANT *result )
{
    WCHAR *path, *text;
    UINT32 length;
    DWORD size;
    BYTE *data;
    HRESULT hr;

    if (FAILED(hr = call_path( call, &path ))) return hr;
    hr = read_file( path, &data, &size );
    free( path );
    if (FAILED(hr)) return hr;
    hr = decode_text( data, size, call->option, &text, &length );
    free( data );
    if (FAILED(hr)) return hr;
    result->vt = VT_LPWSTR;
    result->pwszVal = text;
    return S_OK;
}

static HRESULT read_lines_run( struct storage_call *call, PROPVARIANT *result )
{
    WCHAR *path, *text, *line, *p;
    IVector_HSTRING *vector;
    UINT32 length;
    HSTRING string;
    DWORD size;
    BYTE *data;
    HRESULT hr;

    if (FAILED(hr = call_path( call, &path ))) return hr;
    hr = read_file( path, &data, &size );
    free( path );
    if (FAILED(hr)) return hr;
    hr = decode_text( data, size, call->option, &text, &length );
    free( data );
    if (FAILED(hr)) return hr;

    if (SUCCEEDED(hr = vector_hstring_create( &vector )))
    {
        /* a line ends with CR LF, LF or CR; text after the last end is a line too */
        for (line = p = text; SUCCEEDED(hr) && p <= text + length; p++)
        {
            if (p < text + length && *p != '\r' && *p != '\n') continue;
            if (p == text + length && p == line) break;
            if (SUCCEEDED(hr = WindowsCreateString( line, p - line, &string )))
            {
                hr = IVector_HSTRING_Append( vector, string );
                WindowsDeleteString( string );
            }
            if (p < text + length && p[0] == '\r' && p + 1 < text + length && p[1] == '\n') p++;
            line = p + 1;
        }
        if (SUCCEEDED(hr))
        {
            result->vt = VT_UNKNOWN;
            result->punkVal = (IUnknown *)vector;
        }
        else IVector_HSTRING_Release( vector );
    }
    CoTaskMemFree( text );
    return hr;
}

static HRESULT write_text_run( struct storage_call *call, PROPVARIANT *result )
{
    WCHAR *path;
    HRESULT hr;

    if (FAILED(hr = call_path( call, &path ))) return hr;
    hr = write_text( path, call->contents, call->contents_length, call->option, call->flags & IO_APPEND );
    free( path );
    return hr;
}

/* other: the IIterable<HSTRING> of the lines */
static HRESULT write_lines_run( struct storage_call *call, PROPVARIANT *result )
{
    IIterable_HSTRING *iterable;
    IIterator_HSTRING *iterator;
    WCHAR *path, *text = NULL, *tmp;
    size_t length = 0, capacity = 0;
    boolean more = FALSE;
    HRESULT hr;

    if (FAILED(hr = IInspectable_QueryInterface( call->other, &IID_IIterable_HSTRING, (void **)&iterable ))) return hr;
    hr = IIterable_HSTRING_First( iterable, &iterator );
    IIterable_HSTRING_Release( iterable );
    if (FAILED(hr)) return hr;

    for (hr = IIterator_HSTRING_get_HasCurrent( iterator, &more ); SUCCEEDED(hr) && more;
         hr = IIterator_HSTRING_MoveNext( iterator, &more ))
    {
        const WCHAR *buffer;
        HSTRING string;
        UINT32 count;

        if (FAILED(hr = IIterator_HSTRING_get_Current( iterator, &string ))) break;
        buffer = WindowsGetStringRawBuffer( string, &count );
        if (length + count + 2 > capacity)
        {
            capacity = max( capacity * 2, length + count + 256 );
            if (!(tmp = realloc( text, capacity * sizeof(WCHAR) ))) hr = E_OUTOFMEMORY;
            else text = tmp;
        }
        if (SUCCEEDED(hr))
        {
            memcpy( text + length, buffer, count * sizeof(WCHAR) );
            length += count;
            text[length++] = '\r';
            text[length++] = '\n';
        }
        WindowsDeleteString( string );
        if (FAILED(hr)) break;
    }
    IIterator_HSTRING_Release( iterator );

    if (SUCCEEDED(hr) && SUCCEEDED(hr = call_path( call, &path )))
    {
        hr = write_text( path, text, length, call->option, call->flags & IO_APPEND );
        free( path );
    }
    free( text );
    return hr;
}

static HRESULT create_buffer( const BYTE *data, UINT32 size, IBuffer **buffer )
{
    static const WCHAR *class_name = RuntimeClass_Windows_Storage_Streams_Buffer;
    IBufferByteAccess *access;
    IBufferFactory *factory;
    HSTRING_HEADER header;
    HSTRING string;
    BYTE *bytes;
    HRESULT hr;

    *buffer = NULL;
    if (FAILED(hr = WindowsCreateStringReference( class_name, wcslen( class_name ), &header, &string ))) return hr;
    if (FAILED(hr = RoGetActivationFactory( string, &IID_IBufferFactory, (void **)&factory ))) return hr;
    hr = IBufferFactory_Create( factory, size, buffer );
    IBufferFactory_Release( factory );
    if (FAILED(hr)) return hr;

    if (SUCCEEDED(hr = IBuffer_QueryInterface( *buffer, &IID_IBufferByteAccess, (void **)&access )))
    {
        if (SUCCEEDED(hr = IBufferByteAccess_Buffer( access, &bytes )))
        {
            if (size) memcpy( bytes, data, size );
            hr = IBuffer_put_Length( *buffer, size );
        }
        IBufferByteAccess_Release( access );
    }
    if (FAILED(hr))
    {
        IBuffer_Release( *buffer );
        *buffer = NULL;
    }
    return hr;
}

static HRESULT read_buffer_run( struct storage_call *call, PROPVARIANT *result )
{
    IBuffer *buffer;
    WCHAR *path;
    DWORD size;
    BYTE *data;
    HRESULT hr;

    if (FAILED(hr = call_path( call, &path ))) return hr;
    hr = read_file( path, &data, &size );
    free( path );
    if (FAILED(hr)) return hr;
    hr = create_buffer( data, size, &buffer );
    free( data );
    if (FAILED(hr)) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)buffer;
    return S_OK;
}

/* other: an IBuffer, or NULL when the bytes of the call are written */
static HRESULT write_bytes_run( struct storage_call *call, PROPVARIANT *result )
{
    IBufferByteAccess *access = NULL;
    const BYTE *data = call->bytes;
    UINT32 size = call->size;
    DWORD mark_size;
    BYTE mark[3], *bytes;
    HANDLE file;
    WCHAR *path;
    HRESULT hr;

    if (call->other)
    {
        if (FAILED(hr = IBuffer_get_Length( (IBuffer *)call->other, &size ))) return hr;
        if (FAILED(hr = IInspectable_QueryInterface( call->other, &IID_IBufferByteAccess, (void **)&access ))) return hr;
        if (FAILED(hr = IBufferByteAccess_Buffer( access, &bytes )))
        {
            IBufferByteAccess_Release( access );
            return hr;
        }
        data = bytes;
    }
    if (SUCCEEDED(hr = call_path( call, &path )))
    {
        if (SUCCEEDED(hr = open_for_write( path, FALSE, &file, mark, &mark_size )))
        {
            hr = write_file( file, data, size );
            CloseHandle( file );
        }
        free( path );
    }
    if (access) IBufferByteAccess_Release( access );
    return hr;
}

/* ---- starting them ------------------------------------------------------------------------------------ */

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

static HRESULT io_read_text( IStorageFile *file, HSTRING path, UnicodeEncoding encoding, IAsyncOperation_HSTRING **operation )
{
    if (!operation) return E_POINTER;
    START_CALL( async_operation_hstring_create, storage_call_create( read_text_run, file, NULL, path, encoding ), operation );
}

static HRESULT io_read_lines( IStorageFile *file, HSTRING path, UnicodeEncoding encoding,
                              IAsyncOperation_IVector_HSTRING **operation )
{
    if (!operation) return E_POINTER;
    START_CALL( async_operation_hstring_vector_create, storage_call_create( read_lines_run, file, NULL, path, encoding ),
                operation );
}

static HRESULT io_write_text( IStorageFile *file, HSTRING path, HSTRING contents, UnicodeEncoding encoding, UINT32 flags,
                              IAsyncAction **operation )
{
    struct storage_call *call;
    const WCHAR *buffer;
    UINT32 length;

    if (!operation) return E_POINTER;
    if ((call = storage_call_create( write_text_run, file, NULL, path, encoding )))
    {
        buffer = WindowsGetStringRawBuffer( contents, &length );
        call->flags = flags;
        if (!(call->contents = malloc( (length + 1) * sizeof(WCHAR) )))
        {
            IUnknown_Release( &call->IUnknown_iface );
            return E_OUTOFMEMORY;
        }
        memcpy( call->contents, buffer, length * sizeof(WCHAR) );
        call->contents[length] = 0;
        call->contents_length = length;
    }
    START_CALL( async_action_create, call, operation );
}

static HRESULT io_write_lines( IStorageFile *file, HSTRING path, IIterable_HSTRING *lines, UnicodeEncoding encoding,
                               UINT32 flags, IAsyncAction **operation )
{
    struct storage_call *call;

    if (!operation) return E_POINTER;
    if (!lines) return E_INVALIDARG;
    if ((call = storage_call_create( write_lines_run, file, lines, path, encoding ))) call->flags = flags;
    START_CALL( async_action_create, call, operation );
}

static HRESULT io_read_buffer( IStorageFile *file, HSTRING path, IAsyncOperation_IBuffer **operation )
{
    if (!operation) return E_POINTER;
    START_CALL( async_operation_buffer_create, storage_call_create( read_buffer_run, file, NULL, path, 0 ), operation );
}

static HRESULT io_write_buffer( IStorageFile *file, HSTRING path, IBuffer *buffer, IAsyncAction **operation )
{
    if (!operation) return E_POINTER;
    if (!buffer) return E_INVALIDARG;
    START_CALL( async_action_create, storage_call_create( write_bytes_run, file, buffer, path, 0 ), operation );
}

static HRESULT io_write_bytes( IStorageFile *file, HSTRING path, UINT32 size, BYTE *bytes, IAsyncAction **operation )
{
    struct storage_call *call;

    if (!operation) return E_POINTER;
    if (size && !bytes) return E_INVALIDARG;
    if ((call = storage_call_create( write_bytes_run, file, NULL, path, 0 )))
    {
        if (!(call->bytes = malloc( size + 1 )))
        {
            IUnknown_Release( &call->IUnknown_iface );
            return E_OUTOFMEMORY;
        }
        if (size) memcpy( call->bytes, bytes, size );
        call->size = size;
    }
    START_CALL( async_action_create, call, operation );
}

/* ---- the two classes ---------------------------------------------------------------------------------- */

struct io_statics
{
    IActivationFactory IActivationFactory_iface;
    IFileIOStatics IFileIOStatics_iface;
    IPathIOStatics IPathIOStatics_iface;
    BOOL by_path;
    LONG ref;
};

static inline struct io_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct io_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct io_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (!impl->by_path && IsEqualGUID( iid, &IID_IFileIOStatics ))
        *out = &impl->IFileIOStatics_iface;
    else if (impl->by_path && IsEqualGUID( iid, &IID_IPathIOStatics ))
        *out = &impl->IPathIOStatics_iface;
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
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
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

/* The two interfaces have the same methods: one takes the file, the other its path. */
#define DEFINE_IO_STATICS( pfx, iface_type, target_type, FILE, PATH, CHECK )                                        \
static HRESULT WINAPI pfx##_ReadTextAsync( iface_type *iface, target_type target, IAsyncOperation_HSTRING **operation ) \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_read_text( FILE, PATH, UnicodeEncoding_Utf8, operation );                                             \
}                                                                                                                   \
static HRESULT WINAPI pfx##_ReadTextWithEncodingAsync( iface_type *iface, target_type target, UnicodeEncoding encoding, \
                                                       IAsyncOperation_HSTRING **operation )                        \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_read_text( FILE, PATH, encoding, operation );                                                         \
}                                                                                                                   \
static HRESULT WINAPI pfx##_WriteTextAsync( iface_type *iface, target_type target, HSTRING contents,                \
                                            IAsyncAction **operation )                                              \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_text( FILE, PATH, contents, UnicodeEncoding_Utf8, 0, operation );                               \
}                                                                                                                   \
static HRESULT WINAPI pfx##_WriteTextWithEncodingAsync( iface_type *iface, target_type target, HSTRING contents,    \
                                                        UnicodeEncoding encoding, IAsyncAction **operation )        \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_text( FILE, PATH, contents, encoding, 0, operation );                                           \
}                                                                                                                   \
static HRESULT WINAPI pfx##_AppendTextAsync( iface_type *iface, target_type target, HSTRING contents,               \
                                             IAsyncAction **operation )                                             \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_text( FILE, PATH, contents, UnicodeEncoding_Utf8, IO_APPEND, operation );                       \
}                                                                                                                   \
static HRESULT WINAPI pfx##_AppendTextWithEncodingAsync( iface_type *iface, target_type target, HSTRING contents,   \
                                                         UnicodeEncoding encoding, IAsyncAction **operation )       \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_text( FILE, PATH, contents, encoding, IO_APPEND, operation );                                   \
}                                                                                                                   \
static HRESULT WINAPI pfx##_ReadLinesAsync( iface_type *iface, target_type target,                                  \
                                            IAsyncOperation_IVector_HSTRING **operation )                           \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_read_lines( FILE, PATH, UnicodeEncoding_Utf8, operation );                                            \
}                                                                                                                   \
static HRESULT WINAPI pfx##_ReadLinesWithEncodingAsync( iface_type *iface, target_type target, UnicodeEncoding encoding, \
                                                        IAsyncOperation_IVector_HSTRING **operation )               \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_read_lines( FILE, PATH, encoding, operation );                                                        \
}                                                                                                                   \
static HRESULT WINAPI pfx##_WriteLinesAsync( iface_type *iface, target_type target, IIterable_HSTRING *lines,       \
                                             IAsyncAction **operation )                                             \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_lines( FILE, PATH, lines, UnicodeEncoding_Utf8, 0, operation );                                 \
}                                                                                                                   \
static HRESULT WINAPI pfx##_WriteLinesWithEncodingAsync( iface_type *iface, target_type target, IIterable_HSTRING *lines, \
                                                         UnicodeEncoding encoding, IAsyncAction **operation )       \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_lines( FILE, PATH, lines, encoding, 0, operation );                                             \
}                                                                                                                   \
static HRESULT WINAPI pfx##_AppendLinesAsync( iface_type *iface, target_type target, IIterable_HSTRING *lines,      \
                                              IAsyncAction **operation )                                            \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_lines( FILE, PATH, lines, UnicodeEncoding_Utf8, IO_APPEND, operation );                         \
}                                                                                                                   \
static HRESULT WINAPI pfx##_AppendLinesWithEncodingAsync( iface_type *iface, target_type target, IIterable_HSTRING *lines, \
                                                          UnicodeEncoding encoding, IAsyncAction **operation )      \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_lines( FILE, PATH, lines, encoding, IO_APPEND, operation );                                     \
}                                                                                                                   \
static HRESULT WINAPI pfx##_ReadBufferAsync( iface_type *iface, target_type target, IAsyncOperation_IBuffer **operation ) \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_read_buffer( FILE, PATH, operation );                                                                 \
}                                                                                                                   \
static HRESULT WINAPI pfx##_WriteBufferAsync( iface_type *iface, target_type target, IBuffer *buffer,               \
                                              IAsyncAction **operation )                                            \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_buffer( FILE, PATH, buffer, operation );                                                        \
}                                                                                                                   \
static HRESULT WINAPI pfx##_WriteBytesAsync( iface_type *iface, target_type target, UINT32 size, BYTE *bytes,       \
                                             IAsyncAction **operation )                                             \
{                                                                                                                   \
    CHECK;                                                                                                          \
    return io_write_bytes( FILE, PATH, size, bytes, operation );                                                    \
}                                                                                                                   \
static const struct iface_type##Vtbl pfx##_vtbl =                                                                   \
{                                                                                                                   \
    pfx##_QueryInterface,                                                                                           \
    pfx##_AddRef,                                                                                                   \
    pfx##_Release,                                                                                                  \
    /* IInspectable methods */                                                                                      \
    pfx##_GetIids,                                                                                                  \
    pfx##_GetRuntimeClassName,                                                                                      \
    pfx##_GetTrustLevel,                                                                                            \
    /* the methods of the class */                                                                                  \
    pfx##_ReadTextAsync,                                                                                            \
    pfx##_ReadTextWithEncodingAsync,                                                                                \
    pfx##_WriteTextAsync,                                                                                           \
    pfx##_WriteTextWithEncodingAsync,                                                                               \
    pfx##_AppendTextAsync,                                                                                          \
    pfx##_AppendTextWithEncodingAsync,                                                                              \
    pfx##_ReadLinesAsync,                                                                                           \
    pfx##_ReadLinesWithEncodingAsync,                                                                               \
    pfx##_WriteLinesAsync,                                                                                          \
    pfx##_WriteLinesWithEncodingAsync,                                                                              \
    pfx##_AppendLinesAsync,                                                                                         \
    pfx##_AppendLinesWithEncodingAsync,                                                                             \
    pfx##_ReadBufferAsync,                                                                                          \
    pfx##_WriteBufferAsync,                                                                                         \
    pfx##_WriteBytesAsync,                                                                                          \
};

DEFINE_IINSPECTABLE( file_io, IFileIOStatics, struct io_statics, IActivationFactory_iface )
DEFINE_IINSPECTABLE( path_io, IPathIOStatics, struct io_statics, IActivationFactory_iface )

DEFINE_IO_STATICS( file_io, IFileIOStatics, IStorageFile *, target, NULL, if (!target) return E_INVALIDARG )
DEFINE_IO_STATICS( path_io, IPathIOStatics, HSTRING, NULL, target, (void)0 )

static struct io_statics file_io_statics =
{
    {&factory_vtbl},
    {&file_io_vtbl},
    {&path_io_vtbl},
    FALSE,
    0,
};

static struct io_statics path_io_statics =
{
    {&factory_vtbl},
    {&file_io_vtbl},
    {&path_io_vtbl},
    TRUE,
    0,
};

IActivationFactory *file_io_factory = &file_io_statics.IActivationFactory_iface;
IActivationFactory *path_io_factory = &path_io_statics.IActivationFactory_iface;
