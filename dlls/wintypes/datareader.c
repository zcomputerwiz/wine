/*
 * Windows.Storage.Streams.DataReader
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

/* A reader over a buffer (DataReader.FromBuffer). A reader over a stream can be created, but loading from the stream
 * (LoadAsync) is not implemented. */

#include "private.h"
#include "winnls.h"
#include "robuffer.h"

WINE_DEFAULT_DEBUG_CHANNEL(wintypes);

struct data_reader
{
    IDataReader IDataReader_iface;
    IClosable IClosable_iface;
    LONG ref;

    IInputStream *stream;
    IBuffer *buffer;
    const BYTE *data;
    UINT32 length;
    UINT32 position;
    UnicodeEncoding encoding;
    ByteOrder order;
    InputStreamOptions options;
};

static inline struct data_reader *impl_from_IDataReader( IDataReader *iface )
{
    return CONTAINING_RECORD( iface, struct data_reader, IDataReader_iface );
}

static HRESULT WINAPI data_reader_QueryInterface( IDataReader *iface, REFIID iid, void **out )
{
    struct data_reader *impl = impl_from_IDataReader( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDataReader ))
        *out = &impl->IDataReader_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }

    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI data_reader_AddRef( IDataReader *iface )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI data_reader_Release( IDataReader *iface )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->buffer) IBuffer_Release( impl->buffer );
        if (impl->stream) IInputStream_Release( impl->stream );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI data_reader_GetIids( IDataReader *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_GetRuntimeClassName( IDataReader *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_Streams_DataReader,
                                wcslen( RuntimeClass_Windows_Storage_Streams_DataReader ), class_name );
}

static HRESULT WINAPI data_reader_GetTrustLevel( IDataReader *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

/* the next count bytes, or NULL when the reader does not have them */
static const BYTE *data_reader_take( struct data_reader *impl, UINT32 count )
{
    const BYTE *bytes;

    if (count > impl->length - impl->position) return NULL;
    bytes = impl->data + impl->position;
    impl->position += count;
    return bytes;
}

/* a number of size bytes in the byte order of the reader, as the bytes of this machine (little endian) */
static HRESULT data_reader_number( struct data_reader *impl, void *value, UINT32 size )
{
    const BYTE *bytes = data_reader_take( impl, size );
    BYTE *out = value;
    UINT32 i;

    if (!value) return E_POINTER;
    if (!bytes) return E_BOUNDS;
    for (i = 0; i < size; i++) out[i] = impl->order == ByteOrder_BigEndian ? bytes[size - 1 - i] : bytes[i];
    return S_OK;
}

static HRESULT WINAPI data_reader_get_UnconsumedBufferLength( IDataReader *iface, UINT32 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    *value = impl->length - impl->position;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_UnicodeEncoding( IDataReader *iface, UnicodeEncoding *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDataReader( iface )->encoding;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_UnicodeEncoding( IDataReader *iface, UnicodeEncoding value )
{
    if (value != UnicodeEncoding_Utf8 && value != UnicodeEncoding_Utf16LE && value != UnicodeEncoding_Utf16BE)
        return E_INVALIDARG;
    impl_from_IDataReader( iface )->encoding = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_ByteOrder( IDataReader *iface, ByteOrder *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDataReader( iface )->order;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_ByteOrder( IDataReader *iface, ByteOrder value )
{
    if (value != ByteOrder_LittleEndian && value != ByteOrder_BigEndian) return E_INVALIDARG;
    impl_from_IDataReader( iface )->order = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_InputStreamOptions( IDataReader *iface, InputStreamOptions *value )
{
    if (!value) return E_POINTER;
    *value = impl_from_IDataReader( iface )->options;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_InputStreamOptions( IDataReader *iface, InputStreamOptions value )
{
    impl_from_IDataReader( iface )->options = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadByte( IDataReader *iface, BYTE *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadBytes( IDataReader *iface, UINT32 value_size, BYTE *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    const BYTE *bytes;

    TRACE( "iface %p, value_size %u, value %p.\n", iface, value_size, value );

    if (value_size && !value) return E_POINTER;
    if (!(bytes = data_reader_take( impl, value_size ))) return E_BOUNDS;
    if (value_size) memcpy( value, bytes, value_size );
    return S_OK;
}

static HRESULT create_buffer( const BYTE *data, UINT32 size, IBuffer **buffer )
{
    IBufferByteAccess *access;
    IBufferFactory *factory;
    BYTE *bytes;
    HRESULT hr;

    *buffer = NULL;
    if (FAILED(hr = IActivationFactory_QueryInterface( buffer_activation_factory, &IID_IBufferFactory, (void **)&factory )))
        return hr;
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

static HRESULT WINAPI data_reader_ReadBuffer( IDataReader *iface, UINT32 length, IBuffer **buffer )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    const BYTE *bytes;

    TRACE( "iface %p, length %u, buffer %p.\n", iface, length, buffer );

    if (!buffer) return E_POINTER;
    *buffer = NULL;
    if (!(bytes = data_reader_take( impl, length ))) return E_BOUNDS;
    return create_buffer( bytes, length, buffer );
}

static HRESULT WINAPI data_reader_ReadBoolean( IDataReader *iface, boolean *value )
{
    BYTE byte = 0;
    HRESULT hr;

    if (!value) return E_POINTER;
    hr = data_reader_number( impl_from_IDataReader( iface ), &byte, sizeof(byte) );
    *value = !!byte;
    return hr;
}

static HRESULT WINAPI data_reader_ReadGuid( IDataReader *iface, GUID *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    const BYTE *bytes;
    HRESULT hr;

    if (!value) return E_POINTER;
    if (impl->length - impl->position < sizeof(GUID)) return E_BOUNDS;
    if (FAILED(hr = data_reader_number( impl, &value->Data1, sizeof(value->Data1) ))) return hr;
    if (FAILED(hr = data_reader_number( impl, &value->Data2, sizeof(value->Data2) ))) return hr;
    if (FAILED(hr = data_reader_number( impl, &value->Data3, sizeof(value->Data3) ))) return hr;
    if (!(bytes = data_reader_take( impl, sizeof(value->Data4) ))) return E_BOUNDS;
    memcpy( value->Data4, bytes, sizeof(value->Data4) );
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadInt16( IDataReader *iface, INT16 *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadInt32( IDataReader *iface, INT32 *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadInt64( IDataReader *iface, INT64 *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadUInt16( IDataReader *iface, UINT16 *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadUInt32( IDataReader *iface, UINT32 *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadUInt64( IDataReader *iface, UINT64 *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadSingle( IDataReader *iface, FLOAT *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

static HRESULT WINAPI data_reader_ReadDouble( IDataReader *iface, DOUBLE *value )
{
    return data_reader_number( impl_from_IDataReader( iface ), value, sizeof(*value) );
}

/* code_unit_count is in bytes for UTF-8 and in 16-bit units for UTF-16 */
static HRESULT WINAPI data_reader_ReadString( IDataReader *iface, UINT32 code_unit_count, HSTRING *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    const BYTE *bytes;
    WCHAR *text;
    UINT32 i;
    HRESULT hr;

    TRACE( "iface %p, code_unit_count %u, value %p.\n", iface, code_unit_count, value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!code_unit_count) return S_OK;

    if (impl->encoding == UnicodeEncoding_Utf8)
    {
        int count;

        if (code_unit_count > impl->length - impl->position) return E_BOUNDS;
        bytes = impl->data + impl->position;
        if (!(count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, code_unit_count, NULL, 0 )))
            return HRESULT_FROM_WIN32( ERROR_NO_UNICODE_TRANSLATION );
        if (!(text = malloc( count * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
        MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, code_unit_count, text, count );
        impl->position += code_unit_count;
        hr = WindowsCreateString( text, count, value );
        free( text );
        return hr;
    }

    if (code_unit_count > (impl->length - impl->position) / sizeof(WCHAR)) return E_BOUNDS;
    if (!(text = malloc( code_unit_count * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    bytes = data_reader_take( impl, code_unit_count * sizeof(WCHAR) );
    for (i = 0; i < code_unit_count; i++)
    {
        if (impl->encoding == UnicodeEncoding_Utf16LE) text[i] = bytes[2 * i] | (bytes[2 * i + 1] << 8);
        else text[i] = bytes[2 * i + 1] | (bytes[2 * i] << 8);
    }
    hr = WindowsCreateString( text, code_unit_count, value );
    free( text );
    return hr;
}

static HRESULT WINAPI data_reader_ReadDateTime( IDataReader *iface, DateTime *value )
{
    if (!value) return E_POINTER;
    return data_reader_number( impl_from_IDataReader( iface ), &value->UniversalTime, sizeof(value->UniversalTime) );
}

static HRESULT WINAPI data_reader_ReadTimeSpan( IDataReader *iface, TimeSpan *value )
{
    if (!value) return E_POINTER;
    return data_reader_number( impl_from_IDataReader( iface ), &value->Duration, sizeof(value->Duration) );
}

static HRESULT WINAPI data_reader_LoadAsync( IDataReader *iface, UINT32 count, IAsyncOperation_UINT32 **operation )
{
    FIXME( "iface %p, count %u, operation %p stub!\n", iface, count, operation );
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_DetachBuffer( IDataReader *iface, IBuffer **buffer )
{
    struct data_reader *impl = impl_from_IDataReader( iface );

    TRACE( "iface %p, buffer %p.\n", iface, buffer );

    if (!buffer) return E_POINTER;
    if ((*buffer = impl->buffer)) IBuffer_AddRef( *buffer );
    return S_OK;
}

static HRESULT WINAPI data_reader_DetachStream( IDataReader *iface, IInputStream **stream )
{
    struct data_reader *impl = impl_from_IDataReader( iface );

    TRACE( "iface %p, stream %p.\n", iface, stream );

    if (!stream) return E_POINTER;
    *stream = impl->stream;
    impl->stream = NULL;
    return S_OK;
}

static const struct IDataReaderVtbl data_reader_vtbl =
{
    data_reader_QueryInterface,
    data_reader_AddRef,
    data_reader_Release,
    /* IInspectable methods */
    data_reader_GetIids,
    data_reader_GetRuntimeClassName,
    data_reader_GetTrustLevel,
    /* IDataReader methods */
    data_reader_get_UnconsumedBufferLength,
    data_reader_get_UnicodeEncoding,
    data_reader_put_UnicodeEncoding,
    data_reader_get_ByteOrder,
    data_reader_put_ByteOrder,
    data_reader_get_InputStreamOptions,
    data_reader_put_InputStreamOptions,
    data_reader_ReadByte,
    data_reader_ReadBytes,
    data_reader_ReadBuffer,
    data_reader_ReadBoolean,
    data_reader_ReadGuid,
    data_reader_ReadInt16,
    data_reader_ReadInt32,
    data_reader_ReadInt64,
    data_reader_ReadUInt16,
    data_reader_ReadUInt32,
    data_reader_ReadUInt64,
    data_reader_ReadSingle,
    data_reader_ReadDouble,
    data_reader_ReadString,
    data_reader_ReadDateTime,
    data_reader_ReadTimeSpan,
    data_reader_LoadAsync,
    data_reader_DetachBuffer,
    data_reader_DetachStream,
};

DEFINE_IINSPECTABLE( data_reader_closable, IClosable, struct data_reader, IDataReader_iface )

static HRESULT WINAPI data_reader_closable_Close( IClosable *iface )
{
    struct data_reader *impl = impl_from_IClosable( iface );
    IClosable *closable;

    TRACE( "iface %p.\n", iface );

    if (impl->stream && SUCCEEDED(IInputStream_QueryInterface( impl->stream, &IID_IClosable, (void **)&closable )))
    {
        IClosable_Close( closable );
        IClosable_Release( closable );
    }
    return S_OK;
}

static const struct IClosableVtbl data_reader_closable_vtbl =
{
    data_reader_closable_QueryInterface,
    data_reader_closable_AddRef,
    data_reader_closable_Release,
    /* IInspectable methods */
    data_reader_closable_GetIids,
    data_reader_closable_GetRuntimeClassName,
    data_reader_closable_GetTrustLevel,
    /* IClosable methods */
    data_reader_closable_Close,
};

static HRESULT data_reader_create( IBuffer *buffer, IInputStream *stream, IDataReader **out )
{
    struct data_reader *impl;
    IBufferByteAccess *access;
    BYTE *bytes = NULL;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDataReader_iface.lpVtbl = &data_reader_vtbl;
    impl->IClosable_iface.lpVtbl = &data_reader_closable_vtbl;
    impl->ref = 1;
    impl->encoding = UnicodeEncoding_Utf8;
    impl->order = ByteOrder_BigEndian;

    if (buffer)
    {
        if (FAILED(hr = IBuffer_get_Length( buffer, &impl->length )) ||
            FAILED(hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&access )))
        {
            free( impl );
            return hr;
        }
        hr = IBufferByteAccess_Buffer( access, &bytes );
        IBufferByteAccess_Release( access );
        if (FAILED(hr))
        {
            free( impl );
            return hr;
        }
        impl->data = bytes;
        IBuffer_AddRef( (impl->buffer = buffer) );
    }
    if (stream) IInputStream_AddRef( (impl->stream = stream) );

    *out = &impl->IDataReader_iface;
    return S_OK;
}

struct data_reader_statics
{
    IActivationFactory IActivationFactory_iface;
    IDataReaderFactory IDataReaderFactory_iface;
    IDataReaderStatics IDataReaderStatics_iface;
    LONG ref;
};

static inline struct data_reader_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct data_reader_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct data_reader_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IDataReaderFactory ))
        *out = &impl->IDataReaderFactory_iface;
    else if (IsEqualGUID( iid, &IID_IDataReaderStatics ))
        *out = &impl->IDataReaderStatics_iface;
    else
    {
        WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
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
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* the class has no default constructor */
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

DEFINE_IINSPECTABLE( data_reader_factory, IDataReaderFactory, struct data_reader_statics, IActivationFactory_iface )

static HRESULT WINAPI data_reader_factory_CreateDataReader( IDataReaderFactory *iface, IInputStream *input_stream,
                                                            IDataReader **data_reader )
{
    TRACE( "iface %p, input_stream %p, data_reader %p.\n", iface, input_stream, data_reader );

    if (!data_reader) return E_POINTER;
    return data_reader_create( NULL, input_stream, data_reader );
}

static const struct IDataReaderFactoryVtbl data_reader_factory_vtbl =
{
    data_reader_factory_QueryInterface,
    data_reader_factory_AddRef,
    data_reader_factory_Release,
    /* IInspectable methods */
    data_reader_factory_GetIids,
    data_reader_factory_GetRuntimeClassName,
    data_reader_factory_GetTrustLevel,
    /* IDataReaderFactory methods */
    data_reader_factory_CreateDataReader,
};

DEFINE_IINSPECTABLE( data_reader_statics, IDataReaderStatics, struct data_reader_statics, IActivationFactory_iface )

static HRESULT WINAPI data_reader_statics_FromBuffer( IDataReaderStatics *iface, IBuffer *buffer, IDataReader **data_reader )
{
    TRACE( "iface %p, buffer %p, data_reader %p.\n", iface, buffer, data_reader );

    if (!data_reader) return E_POINTER;
    *data_reader = NULL;
    if (!buffer) return E_INVALIDARG;
    return data_reader_create( buffer, NULL, data_reader );
}

static const struct IDataReaderStaticsVtbl data_reader_statics_vtbl =
{
    data_reader_statics_QueryInterface,
    data_reader_statics_AddRef,
    data_reader_statics_Release,
    /* IInspectable methods */
    data_reader_statics_GetIids,
    data_reader_statics_GetRuntimeClassName,
    data_reader_statics_GetTrustLevel,
    /* IDataReaderStatics methods */
    data_reader_statics_FromBuffer,
};

static struct data_reader_statics data_reader_statics =
{
    {&factory_vtbl},
    {&data_reader_factory_vtbl},
    {&data_reader_statics_vtbl},
    1,
};

IActivationFactory *data_reader_activation_factory = &data_reader_statics.IActivationFactory_iface;
