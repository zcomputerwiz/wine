/* WinRT Windows.Foundation.Diagnostics logging implementation
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
 * LoggingChannel, LoggingActivity, LoggingFields, LoggingOptions and LoggingChannelOptions. Windows writes the events
 * of a channel to ETW when a tracing session listens to it; no session listens on Wine, so a channel is never enabled
 * and its events go nowhere. The objects keep what they are given (names, ids, options), as Windows 11 reports them.
 */

#include "private.h"
#include "bcrypt.h"

#include "initguid.h"
#define WIDL_using_Windows_Foundation_Diagnostics
#include "windows.foundation.diagnostics.h"

WINE_DEFAULT_DEBUG_CHANNEL(wintypes);

/* the id of a channel created from a name only (Windows 8.1 compatibility mode): the LoggingChannel provider's */
static const GUID compatibility_provider = {0x4bd2826e, 0x54a1, 0x4ba9, {0xbf, 0x63, 0x92, 0xb7, 0x3e, 0xa1, 0xac, 0x4a}};

/* the id of a provider from its name, as ETW providers derive it: SHA-1 of the namespace and the upper-case name in
 * UTF-16 big-endian, the first 16 bytes with version 5 */
static void provider_id_from_name( const WCHAR *name, GUID *id )
{
    static const BYTE namespace[16] = {0x48, 0x2c, 0x2d, 0xb2, 0xc3, 0x90, 0x47, 0xc8,
                                       0x87, 0xf8, 0x1a, 0x15, 0xbf, 0xc1, 0x30, 0xfb};
    BCRYPT_HASH_HANDLE hash;
    BYTE digest[20], *data;
    UINT len = wcslen( name ), i;

    memset( id, 0, sizeof(*id) );
    if (!(data = malloc( sizeof(namespace) + len * 2 ))) return;
    memcpy( data, namespace, sizeof(namespace) );
    for (i = 0; i < len; i++)
    {
        WCHAR c = towupper( name[i] );
        data[sizeof(namespace) + 2 * i] = c >> 8;
        data[sizeof(namespace) + 2 * i + 1] = c & 0xff;
    }
    if (!BCryptCreateHash( BCRYPT_SHA1_ALG_HANDLE, &hash, NULL, 0, NULL, 0, 0 ))
    {
        BCryptHashData( hash, data, sizeof(namespace) + len * 2, 0 );
        BCryptFinishHash( hash, digest, sizeof(digest), 0 );
        BCryptDestroyHash( hash );
        digest[7] = (digest[7] & 0x0f) | 0x50;
        memcpy( id, digest, sizeof(*id) );
    }
    free( data );
}

static HRESULT WINAPI inspectable_GetIids( IInspectable *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

/* ---- LoggingChannelOptions ---- */

struct channel_options
{
    ILoggingChannelOptions ILoggingChannelOptions_iface;
    LONG ref;
    GUID group;
};

static inline struct channel_options *impl_from_ILoggingChannelOptions( ILoggingChannelOptions *iface )
{
    return CONTAINING_RECORD( iface, struct channel_options, ILoggingChannelOptions_iface );
}

static HRESULT WINAPI channel_options_QueryInterface( ILoggingChannelOptions *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILoggingChannelOptions ))
    {
        ILoggingChannelOptions_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI channel_options_AddRef( ILoggingChannelOptions *iface )
{
    return InterlockedIncrement( &impl_from_ILoggingChannelOptions( iface )->ref );
}

static ULONG WINAPI channel_options_Release( ILoggingChannelOptions *iface )
{
    struct channel_options *impl = impl_from_ILoggingChannelOptions( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI channel_options_GetIids( ILoggingChannelOptions *iface, ULONG *count, IID **iids )
{
    return inspectable_GetIids( (IInspectable *)iface, count, iids );
}

static HRESULT WINAPI channel_options_GetRuntimeClassName( ILoggingChannelOptions *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Foundation_Diagnostics_LoggingChannelOptions,
                                wcslen( RuntimeClass_Windows_Foundation_Diagnostics_LoggingChannelOptions ), name );
}

static HRESULT WINAPI channel_options_GetTrustLevel( ILoggingChannelOptions *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI channel_options_get_Group( ILoggingChannelOptions *iface, GUID *value )
{
    *value = impl_from_ILoggingChannelOptions( iface )->group;
    return S_OK;
}

static HRESULT WINAPI channel_options_put_Group( ILoggingChannelOptions *iface, GUID value )
{
    impl_from_ILoggingChannelOptions( iface )->group = value;
    return S_OK;
}

static const ILoggingChannelOptionsVtbl channel_options_vtbl =
{
    channel_options_QueryInterface,
    channel_options_AddRef,
    channel_options_Release,
    /* IInspectable methods */
    channel_options_GetIids,
    channel_options_GetRuntimeClassName,
    channel_options_GetTrustLevel,
    /* ILoggingChannelOptions methods */
    channel_options_get_Group,
    channel_options_put_Group,
};

static HRESULT channel_options_create( const GUID *group, ILoggingChannelOptions **out )
{
    struct channel_options *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ILoggingChannelOptions_iface.lpVtbl = &channel_options_vtbl;
    impl->ref = 1;
    if (group) impl->group = *group;
    *out = &impl->ILoggingChannelOptions_iface;
    return S_OK;
}

/* ---- LoggingOptions ---- */

struct logging_options
{
    ILoggingOptions ILoggingOptions_iface;
    LONG ref;
    INT64 keywords;
    INT32 tags;
    INT16 task;
    LoggingOpcode opcode;
    GUID activity_id, related_activity_id;
};

static inline struct logging_options *impl_from_ILoggingOptions( ILoggingOptions *iface )
{
    return CONTAINING_RECORD( iface, struct logging_options, ILoggingOptions_iface );
}

static HRESULT WINAPI logging_options_QueryInterface( ILoggingOptions *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILoggingOptions ))
    {
        ILoggingOptions_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI logging_options_AddRef( ILoggingOptions *iface )
{
    return InterlockedIncrement( &impl_from_ILoggingOptions( iface )->ref );
}

static ULONG WINAPI logging_options_Release( ILoggingOptions *iface )
{
    struct logging_options *impl = impl_from_ILoggingOptions( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI logging_options_GetIids( ILoggingOptions *iface, ULONG *count, IID **iids )
{
    return inspectable_GetIids( (IInspectable *)iface, count, iids );
}

static HRESULT WINAPI logging_options_GetRuntimeClassName( ILoggingOptions *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Foundation_Diagnostics_LoggingOptions,
                                wcslen( RuntimeClass_Windows_Foundation_Diagnostics_LoggingOptions ), name );
}

static HRESULT WINAPI logging_options_GetTrustLevel( ILoggingOptions *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

#define OPTION( name, type, field ) \
    static HRESULT WINAPI logging_options_get_##name( ILoggingOptions *iface, type *value ) \
    { *value = impl_from_ILoggingOptions( iface )->field; return S_OK; } \
    static HRESULT WINAPI logging_options_put_##name( ILoggingOptions *iface, type value ) \
    { impl_from_ILoggingOptions( iface )->field = value; return S_OK; }

OPTION( Keywords, INT64, keywords )
OPTION( Tags, INT32, tags )
OPTION( Task, INT16, task )
OPTION( Opcode, LoggingOpcode, opcode )
OPTION( ActivityId, GUID, activity_id )
OPTION( RelatedActivityId, GUID, related_activity_id )

static const ILoggingOptionsVtbl logging_options_vtbl =
{
    logging_options_QueryInterface,
    logging_options_AddRef,
    logging_options_Release,
    /* IInspectable methods */
    logging_options_GetIids,
    logging_options_GetRuntimeClassName,
    logging_options_GetTrustLevel,
    /* ILoggingOptions methods */
    logging_options_get_Keywords,
    logging_options_put_Keywords,
    logging_options_get_Tags,
    logging_options_put_Tags,
    logging_options_get_Task,
    logging_options_put_Task,
    logging_options_get_Opcode,
    logging_options_put_Opcode,
    logging_options_get_ActivityId,
    logging_options_put_ActivityId,
    logging_options_get_RelatedActivityId,
    logging_options_put_RelatedActivityId,
};

static HRESULT logging_options_create( INT64 keywords, ILoggingOptions **out )
{
    struct logging_options *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ILoggingOptions_iface.lpVtbl = &logging_options_vtbl;
    impl->ref = 1;
    impl->keywords = keywords;
    *out = &impl->ILoggingOptions_iface;
    return S_OK;
}

/* ---- LoggingFields: the fields are not kept (no session takes them); only the structure nesting is checked ---- */

struct fields
{
    ILoggingFields ILoggingFields_iface;
    LONG ref;
    LONG depth;
};

static inline struct fields *impl_from_ILoggingFields( ILoggingFields *iface )
{
    return CONTAINING_RECORD( iface, struct fields, ILoggingFields_iface );
}

static HRESULT WINAPI fields_QueryInterface( ILoggingFields *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILoggingFields ))
    {
        ILoggingFields_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fields_AddRef( ILoggingFields *iface )
{
    return InterlockedIncrement( &impl_from_ILoggingFields( iface )->ref );
}

static ULONG WINAPI fields_Release( ILoggingFields *iface )
{
    struct fields *impl = impl_from_ILoggingFields( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI fields_GetIids( ILoggingFields *iface, ULONG *count, IID **iids )
{
    return inspectable_GetIids( (IInspectable *)iface, count, iids );
}

static HRESULT WINAPI fields_GetRuntimeClassName( ILoggingFields *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Foundation_Diagnostics_LoggingFields,
                                wcslen( RuntimeClass_Windows_Foundation_Diagnostics_LoggingFields ), name );
}

static HRESULT WINAPI fields_GetTrustLevel( ILoggingFields *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT fields_accept( ILoggingFields *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static HRESULT WINAPI fields_Clear( ILoggingFields *iface )
{
    TRACE( "iface %p.\n", iface );
    impl_from_ILoggingFields( iface )->depth = 0;
    return S_OK;
}

static HRESULT WINAPI fields_BeginStruct( ILoggingFields *iface, HSTRING name )
{
    TRACE( "iface %p, name %s.\n", iface, debugstr_hstring( name ) );
    InterlockedIncrement( &impl_from_ILoggingFields( iface )->depth );
    return S_OK;
}

static HRESULT WINAPI fields_BeginStructWithTags( ILoggingFields *iface, HSTRING name, INT32 tags )
{
    return fields_BeginStruct( iface, name );
}

/* as on Windows, a structure must have been begun */
static HRESULT WINAPI fields_EndStruct( ILoggingFields *iface )
{
    struct fields *impl = impl_from_ILoggingFields( iface );

    TRACE( "iface %p.\n", iface );
    if (impl->depth <= 0) return E_UNEXPECTED;
    impl->depth--;
    return S_OK;
}

/* the fields of every type and format: accepted (no session takes them) */

static HRESULT WINAPI fields_AddEmpty( ILoggingFields *iface, HSTRING name )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddEmptyWithFormat( ILoggingFields *iface, HSTRING name, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddEmptyWithFormatAndTags( ILoggingFields *iface, HSTRING name, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt8( ILoggingFields *iface, HSTRING name, BYTE value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt8WithFormat( ILoggingFields *iface, HSTRING name, BYTE value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt8WithFormatAndTags( ILoggingFields *iface, HSTRING name, BYTE value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt8Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, BYTE *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt8ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, BYTE *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt8ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, BYTE *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt16( ILoggingFields *iface, HSTRING name, INT16 value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt16WithFormat( ILoggingFields *iface, HSTRING name, INT16 value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt16WithFormatAndTags( ILoggingFields *iface, HSTRING name, INT16 value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt16Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT16 *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt16ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT16 *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt16ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT16 *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt16( ILoggingFields *iface, HSTRING name, UINT16 value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt16WithFormat( ILoggingFields *iface, HSTRING name, UINT16 value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt16WithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT16 value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt16Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT16 *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt16ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT16 *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt16ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT16 *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt32( ILoggingFields *iface, HSTRING name, INT32 value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt32WithFormat( ILoggingFields *iface, HSTRING name, INT32 value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt32WithFormatAndTags( ILoggingFields *iface, HSTRING name, INT32 value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt32Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT32 *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt32ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT32 *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt32ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT32 *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt32( ILoggingFields *iface, HSTRING name, UINT32 value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt32WithFormat( ILoggingFields *iface, HSTRING name, UINT32 value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt32WithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt32Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT32 *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt32ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT32 *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt32ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT32 *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt64( ILoggingFields *iface, HSTRING name, INT64 value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt64WithFormat( ILoggingFields *iface, HSTRING name, INT64 value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt64WithFormatAndTags( ILoggingFields *iface, HSTRING name, INT64 value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt64Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT64 *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt64ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT64 *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddInt64ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, INT64 *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt64( ILoggingFields *iface, HSTRING name, UINT64 value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt64WithFormat( ILoggingFields *iface, HSTRING name, UINT64 value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt64WithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT64 value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt64Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT64 *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt64ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT64 *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddUInt64ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, UINT64 *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSingle( ILoggingFields *iface, HSTRING name, FLOAT value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSingleWithFormat( ILoggingFields *iface, HSTRING name, FLOAT value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSingleWithFormatAndTags( ILoggingFields *iface, HSTRING name, FLOAT value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSingleArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, FLOAT *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSingleArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, FLOAT *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSingleArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, FLOAT *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDouble( ILoggingFields *iface, HSTRING name, DOUBLE value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDoubleWithFormat( ILoggingFields *iface, HSTRING name, DOUBLE value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDoubleWithFormatAndTags( ILoggingFields *iface, HSTRING name, DOUBLE value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDoubleArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, DOUBLE *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDoubleArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, DOUBLE *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDoubleArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, DOUBLE *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddChar16( ILoggingFields *iface, HSTRING name, WCHAR value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddChar16WithFormat( ILoggingFields *iface, HSTRING name, WCHAR value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddChar16WithFormatAndTags( ILoggingFields *iface, HSTRING name, WCHAR value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddChar16Array( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, WCHAR *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddChar16ArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, WCHAR *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddChar16ArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, WCHAR *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddBoolean( ILoggingFields *iface, HSTRING name, boolean value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddBooleanWithFormat( ILoggingFields *iface, HSTRING name, boolean value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddBooleanWithFormatAndTags( ILoggingFields *iface, HSTRING name, boolean value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddBooleanArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, boolean *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddBooleanArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, boolean *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddBooleanArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, boolean *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddString( ILoggingFields *iface, HSTRING name, HSTRING value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddStringWithFormat( ILoggingFields *iface, HSTRING name, HSTRING value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddStringWithFormatAndTags( ILoggingFields *iface, HSTRING name, HSTRING value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddStringArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, HSTRING *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddStringArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, HSTRING *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddStringArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, HSTRING *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddGuid( ILoggingFields *iface, HSTRING name, GUID value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddGuidWithFormat( ILoggingFields *iface, HSTRING name, GUID value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddGuidWithFormatAndTags( ILoggingFields *iface, HSTRING name, GUID value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddGuidArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, GUID *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddGuidArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, GUID *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddGuidArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, GUID *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDateTime( ILoggingFields *iface, HSTRING name, DateTime value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDateTimeWithFormat( ILoggingFields *iface, HSTRING name, DateTime value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDateTimeWithFormatAndTags( ILoggingFields *iface, HSTRING name, DateTime value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDateTimeArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, DateTime *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDateTimeArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, DateTime *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddDateTimeArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, DateTime *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddTimeSpan( ILoggingFields *iface, HSTRING name, TimeSpan value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddTimeSpanWithFormat( ILoggingFields *iface, HSTRING name, TimeSpan value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddTimeSpanWithFormatAndTags( ILoggingFields *iface, HSTRING name, TimeSpan value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddTimeSpanArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, TimeSpan *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddTimeSpanArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, TimeSpan *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddTimeSpanArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, TimeSpan *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddPoint( ILoggingFields *iface, HSTRING name, Point value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddPointWithFormat( ILoggingFields *iface, HSTRING name, Point value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddPointWithFormatAndTags( ILoggingFields *iface, HSTRING name, Point value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddPointArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Point *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddPointArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Point *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddPointArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Point *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSize( ILoggingFields *iface, HSTRING name, Size value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSizeWithFormat( ILoggingFields *iface, HSTRING name, Size value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSizeWithFormatAndTags( ILoggingFields *iface, HSTRING name, Size value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSizeArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Size *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSizeArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Size *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddSizeArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Size *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddRect( ILoggingFields *iface, HSTRING name, Rect value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddRectWithFormat( ILoggingFields *iface, HSTRING name, Rect value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddRectWithFormatAndTags( ILoggingFields *iface, HSTRING name, Rect value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddRectArray( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Rect *value )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddRectArrayWithFormat( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Rect *value, LoggingFieldFormat format )
{
    return fields_accept( iface );
}

static HRESULT WINAPI fields_AddRectArrayWithFormatAndTags( ILoggingFields *iface, HSTRING name, UINT32 __valueSize, Rect *value, LoggingFieldFormat format, INT32 tags )
{
    return fields_accept( iface );
}

static const ILoggingFieldsVtbl fields_vtbl =
{
    fields_QueryInterface,
    fields_AddRef,
    fields_Release,
    /* IInspectable methods */
    fields_GetIids,
    fields_GetRuntimeClassName,
    fields_GetTrustLevel,
    /* ILoggingFields methods */
    fields_Clear,
    fields_BeginStruct,
    fields_BeginStructWithTags,
    fields_EndStruct,
    fields_AddEmpty,
    fields_AddEmptyWithFormat,
    fields_AddEmptyWithFormatAndTags,
    fields_AddUInt8,
    fields_AddUInt8WithFormat,
    fields_AddUInt8WithFormatAndTags,
    fields_AddUInt8Array,
    fields_AddUInt8ArrayWithFormat,
    fields_AddUInt8ArrayWithFormatAndTags,
    fields_AddInt16,
    fields_AddInt16WithFormat,
    fields_AddInt16WithFormatAndTags,
    fields_AddInt16Array,
    fields_AddInt16ArrayWithFormat,
    fields_AddInt16ArrayWithFormatAndTags,
    fields_AddUInt16,
    fields_AddUInt16WithFormat,
    fields_AddUInt16WithFormatAndTags,
    fields_AddUInt16Array,
    fields_AddUInt16ArrayWithFormat,
    fields_AddUInt16ArrayWithFormatAndTags,
    fields_AddInt32,
    fields_AddInt32WithFormat,
    fields_AddInt32WithFormatAndTags,
    fields_AddInt32Array,
    fields_AddInt32ArrayWithFormat,
    fields_AddInt32ArrayWithFormatAndTags,
    fields_AddUInt32,
    fields_AddUInt32WithFormat,
    fields_AddUInt32WithFormatAndTags,
    fields_AddUInt32Array,
    fields_AddUInt32ArrayWithFormat,
    fields_AddUInt32ArrayWithFormatAndTags,
    fields_AddInt64,
    fields_AddInt64WithFormat,
    fields_AddInt64WithFormatAndTags,
    fields_AddInt64Array,
    fields_AddInt64ArrayWithFormat,
    fields_AddInt64ArrayWithFormatAndTags,
    fields_AddUInt64,
    fields_AddUInt64WithFormat,
    fields_AddUInt64WithFormatAndTags,
    fields_AddUInt64Array,
    fields_AddUInt64ArrayWithFormat,
    fields_AddUInt64ArrayWithFormatAndTags,
    fields_AddSingle,
    fields_AddSingleWithFormat,
    fields_AddSingleWithFormatAndTags,
    fields_AddSingleArray,
    fields_AddSingleArrayWithFormat,
    fields_AddSingleArrayWithFormatAndTags,
    fields_AddDouble,
    fields_AddDoubleWithFormat,
    fields_AddDoubleWithFormatAndTags,
    fields_AddDoubleArray,
    fields_AddDoubleArrayWithFormat,
    fields_AddDoubleArrayWithFormatAndTags,
    fields_AddChar16,
    fields_AddChar16WithFormat,
    fields_AddChar16WithFormatAndTags,
    fields_AddChar16Array,
    fields_AddChar16ArrayWithFormat,
    fields_AddChar16ArrayWithFormatAndTags,
    fields_AddBoolean,
    fields_AddBooleanWithFormat,
    fields_AddBooleanWithFormatAndTags,
    fields_AddBooleanArray,
    fields_AddBooleanArrayWithFormat,
    fields_AddBooleanArrayWithFormatAndTags,
    fields_AddString,
    fields_AddStringWithFormat,
    fields_AddStringWithFormatAndTags,
    fields_AddStringArray,
    fields_AddStringArrayWithFormat,
    fields_AddStringArrayWithFormatAndTags,
    fields_AddGuid,
    fields_AddGuidWithFormat,
    fields_AddGuidWithFormatAndTags,
    fields_AddGuidArray,
    fields_AddGuidArrayWithFormat,
    fields_AddGuidArrayWithFormatAndTags,
    fields_AddDateTime,
    fields_AddDateTimeWithFormat,
    fields_AddDateTimeWithFormatAndTags,
    fields_AddDateTimeArray,
    fields_AddDateTimeArrayWithFormat,
    fields_AddDateTimeArrayWithFormatAndTags,
    fields_AddTimeSpan,
    fields_AddTimeSpanWithFormat,
    fields_AddTimeSpanWithFormatAndTags,
    fields_AddTimeSpanArray,
    fields_AddTimeSpanArrayWithFormat,
    fields_AddTimeSpanArrayWithFormatAndTags,
    fields_AddPoint,
    fields_AddPointWithFormat,
    fields_AddPointWithFormatAndTags,
    fields_AddPointArray,
    fields_AddPointArrayWithFormat,
    fields_AddPointArrayWithFormatAndTags,
    fields_AddSize,
    fields_AddSizeWithFormat,
    fields_AddSizeWithFormatAndTags,
    fields_AddSizeArray,
    fields_AddSizeArrayWithFormat,
    fields_AddSizeArrayWithFormatAndTags,
    fields_AddRect,
    fields_AddRectWithFormat,
    fields_AddRectWithFormatAndTags,
    fields_AddRectArray,
    fields_AddRectArrayWithFormat,
    fields_AddRectArrayWithFormatAndTags,
};

static HRESULT fields_create( IInspectable **out )
{
    struct fields *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ILoggingFields_iface.lpVtbl = &fields_vtbl;
    impl->ref = 1;
    *out = (IInspectable *)&impl->ILoggingFields_iface;
    return S_OK;
}

/* ---- LoggingChannel and LoggingActivity ---- */

struct channel
{
    ILoggingChannel ILoggingChannel_iface;
    ILoggingChannel2 ILoggingChannel2_iface;
    ILoggingTarget ILoggingTarget_iface;
    IClosable IClosable_iface;
    LONG ref;
    HSTRING name;
    GUID id;
};

struct activity
{
    ILoggingActivity ILoggingActivity_iface;
    ILoggingActivity2 ILoggingActivity2_iface;
    ILoggingTarget ILoggingTarget_iface;
    IClosable IClosable_iface;
    LONG ref;
    HSTRING name;
    GUID id;
    ILoggingChannel *channel;
};

static HRESULT activity_create( HSTRING name, ILoggingChannel *channel, ILoggingActivity **out );

static inline struct channel *impl_from_ILoggingChannel( ILoggingChannel *iface )
{
    return CONTAINING_RECORD( iface, struct channel, ILoggingChannel_iface );
}

static HRESULT WINAPI channel_QueryInterface( ILoggingChannel *iface, REFIID iid, void **out )
{
    struct channel *impl = impl_from_ILoggingChannel( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILoggingChannel ))
        *out = &impl->ILoggingChannel_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingChannel2 ))
        *out = &impl->ILoggingChannel2_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingTarget ))
        *out = &impl->ILoggingTarget_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI channel_AddRef( ILoggingChannel *iface )
{
    return InterlockedIncrement( &impl_from_ILoggingChannel( iface )->ref );
}

static ULONG WINAPI channel_Release( ILoggingChannel *iface )
{
    struct channel *impl = impl_from_ILoggingChannel( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI channel_GetIids( ILoggingChannel *iface, ULONG *count, IID **iids )
{
    return inspectable_GetIids( (IInspectable *)iface, count, iids );
}

static HRESULT WINAPI channel_GetRuntimeClassName( ILoggingChannel *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Foundation_Diagnostics_LoggingChannel,
                                wcslen( RuntimeClass_Windows_Foundation_Diagnostics_LoggingChannel ), name );
}

static HRESULT WINAPI channel_GetTrustLevel( ILoggingChannel *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI channel_get_Name( ILoggingChannel *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_ILoggingChannel( iface )->name, value );
}

/* no tracing session listens on Wine */
static HRESULT WINAPI channel_get_Enabled( ILoggingChannel *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI channel_get_Level( ILoggingChannel *iface, LoggingLevel *value )
{
    *value = LoggingLevel_Verbose;
    return S_OK;
}

static HRESULT WINAPI channel_LogMessage( ILoggingChannel *iface, HSTRING message )
{
    TRACE( "iface %p, message %s.\n", iface, debugstr_hstring( message ) );
    return S_OK;
}

static HRESULT WINAPI channel_LogMessageWithLevel( ILoggingChannel *iface, HSTRING message, LoggingLevel level )
{
    return channel_LogMessage( iface, message );
}

static HRESULT WINAPI channel_LogValuePair( ILoggingChannel *iface, HSTRING name, INT32 value )
{
    TRACE( "iface %p, name %s, value %d.\n", iface, debugstr_hstring( name ), value );
    return S_OK;
}

static HRESULT WINAPI channel_LogValuePairWithLevel( ILoggingChannel *iface, HSTRING name, INT32 value, LoggingLevel level )
{
    return channel_LogValuePair( iface, name, value );
}

/* never raised: the channel is never enabled */
static HRESULT WINAPI channel_add_LoggingEnabled( ILoggingChannel *iface, ITypedEventHandler_ILoggingChannel_IInspectable *handler,
                                                  EventRegistrationToken *token )
{
    static LONG next;

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!handler) return E_INVALIDARG;
    token->value = InterlockedIncrement( &next );
    return S_OK;
}

static HRESULT WINAPI channel_remove_LoggingEnabled( ILoggingChannel *iface, EventRegistrationToken token )
{
    return S_OK;
}

static const ILoggingChannelVtbl channel_vtbl =
{
    channel_QueryInterface,
    channel_AddRef,
    channel_Release,
    /* IInspectable methods */
    channel_GetIids,
    channel_GetRuntimeClassName,
    channel_GetTrustLevel,
    /* ILoggingChannel methods */
    channel_get_Name,
    channel_get_Enabled,
    channel_get_Level,
    channel_LogMessage,
    channel_LogMessageWithLevel,
    channel_LogValuePair,
    channel_LogValuePairWithLevel,
    channel_add_LoggingEnabled,
    channel_remove_LoggingEnabled,
};

DEFINE_IINSPECTABLE( channel2, ILoggingChannel2, struct channel, ILoggingChannel_iface )

static HRESULT WINAPI channel2_get_Id( ILoggingChannel2 *iface, GUID *value )
{
    *value = impl_from_ILoggingChannel2( iface )->id;
    return S_OK;
}

static const ILoggingChannel2Vtbl channel2_vtbl =
{
    channel2_QueryInterface,
    channel2_AddRef,
    channel2_Release,
    /* IInspectable methods */
    channel2_GetIids,
    channel2_GetRuntimeClassName,
    channel2_GetTrustLevel,
    /* ILoggingChannel2 methods */
    channel2_get_Id,
};

/* the ILoggingTarget of a channel or of an activity (which writes through its channel): never enabled */
#define LOGGING_TARGET( prefix, type, base_iface )                                                                    \
    DEFINE_IINSPECTABLE_( prefix, ILoggingTarget, type, prefix##_impl, ILoggingTarget_iface, &impl->base_iface )      \
    static HRESULT WINAPI prefix##_IsEnabled( ILoggingTarget *iface, boolean *result )                                \
    { *result = FALSE; return S_OK; }                                                                                 \
    static HRESULT WINAPI prefix##_IsEnabledWithLevel( ILoggingTarget *iface, LoggingLevel level, boolean *result )   \
    { *result = FALSE; return S_OK; }                                                                                 \
    static HRESULT WINAPI prefix##_IsEnabledWithLevelAndKeywords( ILoggingTarget *iface, LoggingLevel level,          \
                                                                  INT64 keywords, boolean *result )                   \
    { *result = FALSE; return S_OK; }                                                                                 \
    static HRESULT WINAPI prefix##_LogEvent( ILoggingTarget *iface, HSTRING name )                                    \
    { TRACE( "iface %p, name %s.\n", iface, debugstr_hstring( name ) ); return S_OK; }                               \
    static HRESULT WINAPI prefix##_LogEventWithFields( ILoggingTarget *iface, HSTRING name, ILoggingFields *fields )  \
    { return prefix##_LogEvent( iface, name ); }                                                                      \
    static HRESULT WINAPI prefix##_LogEventWithFieldsAndLevel( ILoggingTarget *iface, HSTRING name,                   \
                                                               ILoggingFields *fields, LoggingLevel level )           \
    { return prefix##_LogEvent( iface, name ); }                                                                      \
    static HRESULT WINAPI prefix##_LogEventWithFieldsAndOptions( ILoggingTarget *iface, HSTRING name,                 \
            ILoggingFields *fields, LoggingLevel level, ILoggingOptions *options )                                     \
    { return prefix##_LogEvent( iface, name ); }                                                                      \
    static HRESULT WINAPI prefix##_StartActivity( ILoggingTarget *iface, HSTRING name, ILoggingActivity **result )    \
    { return activity_create( name, prefix##_channel( iface ), result ); }                                            \
    static HRESULT WINAPI prefix##_StartActivityWithFields( ILoggingTarget *iface, HSTRING name,                      \
                                                            ILoggingFields *fields, ILoggingActivity **result )       \
    { return prefix##_StartActivity( iface, name, result ); }                                                         \
    static HRESULT WINAPI prefix##_StartActivityWithFieldsAndLevel( ILoggingTarget *iface, HSTRING name,              \
            ILoggingFields *fields, LoggingLevel level, ILoggingActivity **result )                                    \
    { return prefix##_StartActivity( iface, name, result ); }                                                         \
    static HRESULT WINAPI prefix##_StartActivityWithFieldsAndOptions( ILoggingTarget *iface, HSTRING name,            \
            ILoggingFields *fields, LoggingLevel level, ILoggingOptions *options, ILoggingActivity **result )           \
    { return prefix##_StartActivity( iface, name, result ); }                                                         \
    static const ILoggingTargetVtbl prefix##_vtbl =                                                                   \
    {                                                                                                                 \
        prefix##_QueryInterface, prefix##_AddRef, prefix##_Release,                                                   \
        prefix##_GetIids, prefix##_GetRuntimeClassName, prefix##_GetTrustLevel,                                       \
        prefix##_IsEnabled, prefix##_IsEnabledWithLevel, prefix##_IsEnabledWithLevelAndKeywords,                      \
        prefix##_LogEvent, prefix##_LogEventWithFields, prefix##_LogEventWithFieldsAndLevel,                          \
        prefix##_LogEventWithFieldsAndOptions, prefix##_StartActivity, prefix##_StartActivityWithFields,              \
        prefix##_StartActivityWithFieldsAndLevel, prefix##_StartActivityWithFieldsAndOptions,                         \
    };

static ILoggingChannel *channel_target_channel( ILoggingTarget *iface );
LOGGING_TARGET( channel_target, struct channel, ILoggingChannel_iface )
static ILoggingChannel *channel_target_channel( ILoggingTarget *iface )
{
    return &channel_target_impl( iface )->ILoggingChannel_iface;
}

DEFINE_IINSPECTABLE_( channel_closable, IClosable, struct channel, channel_closable_impl, IClosable_iface, &impl->ILoggingChannel_iface )

static HRESULT WINAPI channel_closable_Close( IClosable *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static const IClosableVtbl channel_closable_vtbl =
{
    channel_closable_QueryInterface,
    channel_closable_AddRef,
    channel_closable_Release,
    /* IInspectable methods */
    channel_closable_GetIids,
    channel_closable_GetRuntimeClassName,
    channel_closable_GetTrustLevel,
    /* IClosable methods */
    channel_closable_Close,
};

static HRESULT channel_create( HSTRING name, const GUID *id, ILoggingChannel **out )
{
    struct channel *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ILoggingChannel_iface.lpVtbl = &channel_vtbl;
    impl->ILoggingChannel2_iface.lpVtbl = &channel2_vtbl;
    impl->ILoggingTarget_iface.lpVtbl = &channel_target_vtbl;
    impl->IClosable_iface.lpVtbl = &channel_closable_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( name, &impl->name )))
    {
        free( impl );
        return hr;
    }
    if (id) impl->id = *id;
    else provider_id_from_name( WindowsGetStringRawBuffer( name, NULL ), &impl->id );
    *out = &impl->ILoggingChannel_iface;
    return S_OK;
}

static inline struct activity *impl_from_ILoggingActivity( ILoggingActivity *iface )
{
    return CONTAINING_RECORD( iface, struct activity, ILoggingActivity_iface );
}

static HRESULT WINAPI activity_QueryInterface( ILoggingActivity *iface, REFIID iid, void **out )
{
    struct activity *impl = impl_from_ILoggingActivity( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILoggingActivity ))
        *out = &impl->ILoggingActivity_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingActivity2 ))
        *out = &impl->ILoggingActivity2_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingTarget ))
        *out = &impl->ILoggingTarget_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &impl->IClosable_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI activity_AddRef( ILoggingActivity *iface )
{
    return InterlockedIncrement( &impl_from_ILoggingActivity( iface )->ref );
}

static ULONG WINAPI activity_Release( ILoggingActivity *iface )
{
    struct activity *impl = impl_from_ILoggingActivity( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->name );
        if (impl->channel) ILoggingChannel_Release( impl->channel );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI activity_GetIids( ILoggingActivity *iface, ULONG *count, IID **iids )
{
    return inspectable_GetIids( (IInspectable *)iface, count, iids );
}

static HRESULT WINAPI activity_GetRuntimeClassName( ILoggingActivity *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_Foundation_Diagnostics_LoggingActivity,
                                wcslen( RuntimeClass_Windows_Foundation_Diagnostics_LoggingActivity ), name );
}

static HRESULT WINAPI activity_GetTrustLevel( ILoggingActivity *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI activity_get_Name( ILoggingActivity *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_ILoggingActivity( iface )->name, value );
}

static HRESULT WINAPI activity_get_Id( ILoggingActivity *iface, GUID *value )
{
    *value = impl_from_ILoggingActivity( iface )->id;
    return S_OK;
}

static const ILoggingActivityVtbl activity_vtbl =
{
    activity_QueryInterface,
    activity_AddRef,
    activity_Release,
    /* IInspectable methods */
    activity_GetIids,
    activity_GetRuntimeClassName,
    activity_GetTrustLevel,
    /* ILoggingActivity methods */
    activity_get_Name,
    activity_get_Id,
};

DEFINE_IINSPECTABLE( activity2, ILoggingActivity2, struct activity, ILoggingActivity_iface )

static HRESULT WINAPI activity2_get_Channel( ILoggingActivity2 *iface, ILoggingChannel **value )
{
    struct activity *impl = impl_from_ILoggingActivity2( iface );
    if ((*value = impl->channel)) ILoggingChannel_AddRef( impl->channel );
    return S_OK;
}

static HRESULT WINAPI activity2_StopActivity( ILoggingActivity2 *iface, HSTRING name )
{
    TRACE( "iface %p, name %s.\n", iface, debugstr_hstring( name ) );
    return S_OK;
}

static HRESULT WINAPI activity2_StopActivityWithFields( ILoggingActivity2 *iface, HSTRING name, ILoggingFields *fields )
{
    return activity2_StopActivity( iface, name );
}

static HRESULT WINAPI activity2_StopActivityWithFieldsAndOptions( ILoggingActivity2 *iface, HSTRING name, ILoggingFields *fields,
                                                                   ILoggingOptions *options )
{
    return activity2_StopActivity( iface, name );
}

static const ILoggingActivity2Vtbl activity2_vtbl =
{
    activity2_QueryInterface,
    activity2_AddRef,
    activity2_Release,
    /* IInspectable methods */
    activity2_GetIids,
    activity2_GetRuntimeClassName,
    activity2_GetTrustLevel,
    /* ILoggingActivity2 methods */
    activity2_get_Channel,
    activity2_StopActivity,
    activity2_StopActivityWithFields,
    activity2_StopActivityWithFieldsAndOptions,
};

static ILoggingChannel *activity_target_channel( ILoggingTarget *iface );
LOGGING_TARGET( activity_target, struct activity, ILoggingActivity_iface )
static ILoggingChannel *activity_target_channel( ILoggingTarget *iface )
{
    return activity_target_impl( iface )->channel;
}

DEFINE_IINSPECTABLE_( activity_closable, IClosable, struct activity, activity_closable_impl, IClosable_iface, &impl->ILoggingActivity_iface )

static HRESULT WINAPI activity_closable_Close( IClosable *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static const IClosableVtbl activity_closable_vtbl =
{
    activity_closable_QueryInterface,
    activity_closable_AddRef,
    activity_closable_Release,
    /* IInspectable methods */
    activity_closable_GetIids,
    activity_closable_GetRuntimeClassName,
    activity_closable_GetTrustLevel,
    /* IClosable methods */
    activity_closable_Close,
};

static HRESULT activity_create( HSTRING name, ILoggingChannel *channel, ILoggingActivity **out )
{
    struct activity *impl;
    HRESULT hr;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ILoggingActivity_iface.lpVtbl = &activity_vtbl;
    impl->ILoggingActivity2_iface.lpVtbl = &activity2_vtbl;
    impl->ILoggingTarget_iface.lpVtbl = &activity_target_vtbl;
    impl->IClosable_iface.lpVtbl = &activity_closable_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( name, &impl->name )))
    {
        free( impl );
        return hr;
    }
    CoCreateGuid( &impl->id );
    if ((impl->channel = channel)) ILoggingChannel_AddRef( channel );
    *out = &impl->ILoggingActivity_iface;
    return S_OK;
}

/* ---- AsyncCausalityTracer: the causality of asynchronous operations, traced to ETW on Windows; no session listens on
 * Wine, so every trace is accepted and goes nowhere ---- */

struct causality_tracer
{
    IActivationFactory IActivationFactory_iface;
    IAsyncCausalityTracerStatics IAsyncCausalityTracerStatics_iface;
    LONG ref;
};

static inline struct causality_tracer *causality_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct causality_tracer, IActivationFactory_iface );
}

static HRESULT WINAPI causality_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct causality_tracer *impl = causality_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncCausalityTracerStatics ))
        *out = &impl->IAsyncCausalityTracerStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI causality_factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &causality_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI causality_factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &causality_from_IActivationFactory( iface )->ref );
}

static HRESULT WINAPI causality_factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    return inspectable_GetIids( (IInspectable *)iface, count, iids );
}

static HRESULT WINAPI causality_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    FIXME( "iface %p, name %p stub!\n", iface, name );
    return E_NOTIMPL;
}

static HRESULT WINAPI causality_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    FIXME( "iface %p, level %p stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI causality_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl causality_factory_vtbl =
{
    causality_factory_QueryInterface,
    causality_factory_AddRef,
    causality_factory_Release,
    /* IInspectable methods */
    causality_factory_GetIids,
    causality_factory_GetRuntimeClassName,
    causality_factory_GetTrustLevel,
    /* IActivationFactory methods */
    causality_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE_( causality_statics, IAsyncCausalityTracerStatics, struct causality_tracer, causality_statics_impl,
                      IAsyncCausalityTracerStatics_iface, &impl->IActivationFactory_iface )

static HRESULT WINAPI causality_statics_TraceOperationCreation( IAsyncCausalityTracerStatics *iface, CausalityTraceLevel level,
        CausalitySource source, GUID platform, UINT64 operation, HSTRING name, UINT64 related )
{
    TRACE( "iface %p, level %d, source %d, operation %#I64x, name %s.\n", iface, level, source, operation, debugstr_hstring( name ) );
    return S_OK;
}

static HRESULT WINAPI causality_statics_TraceOperationCompletion( IAsyncCausalityTracerStatics *iface, CausalityTraceLevel level,
        CausalitySource source, GUID platform, UINT64 operation, AsyncStatus status )
{
    TRACE( "iface %p, level %d, source %d, operation %#I64x, status %d.\n", iface, level, source, operation, status );
    return S_OK;
}

static HRESULT WINAPI causality_statics_TraceOperationRelation( IAsyncCausalityTracerStatics *iface, CausalityTraceLevel level,
        CausalitySource source, GUID platform, UINT64 operation, CausalityRelation relation )
{
    TRACE( "iface %p, level %d, source %d, operation %#I64x, relation %d.\n", iface, level, source, operation, relation );
    return S_OK;
}

static HRESULT WINAPI causality_statics_TraceSynchronousWorkStart( IAsyncCausalityTracerStatics *iface, CausalityTraceLevel level,
        CausalitySource source, GUID platform, UINT64 operation, CausalitySynchronousWork work )
{
    TRACE( "iface %p, level %d, source %d, operation %#I64x, work %d.\n", iface, level, source, operation, work );
    return S_OK;
}

static HRESULT WINAPI causality_statics_TraceSynchronousWorkCompletion( IAsyncCausalityTracerStatics *iface, CausalityTraceLevel level,
        CausalitySource source, CausalitySynchronousWork work )
{
    TRACE( "iface %p, level %d, source %d, work %d.\n", iface, level, source, work );
    return S_OK;
}

/* never raised: no session starts or stops tracing on Wine */
static HRESULT WINAPI causality_statics_add_TracingStatusChanged( IAsyncCausalityTracerStatics *iface,
        IEventHandler_TracingStatusChangedEventArgs *handler, EventRegistrationToken *cookie )
{
    static LONG next;

    TRACE( "iface %p, handler %p, cookie %p.\n", iface, handler, cookie );
    if (!handler) return E_INVALIDARG;
    cookie->value = InterlockedIncrement( &next );
    return S_OK;
}

static HRESULT WINAPI causality_statics_remove_TracingStatusChanged( IAsyncCausalityTracerStatics *iface, EventRegistrationToken cookie )
{
    TRACE( "iface %p, cookie %#I64x.\n", iface, cookie.value );
    return S_OK;
}

static const IAsyncCausalityTracerStaticsVtbl causality_statics_vtbl =
{
    causality_statics_QueryInterface,
    causality_statics_AddRef,
    causality_statics_Release,
    /* IInspectable methods */
    causality_statics_GetIids,
    causality_statics_GetRuntimeClassName,
    causality_statics_GetTrustLevel,
    /* IAsyncCausalityTracerStatics methods */
    causality_statics_TraceOperationCreation,
    causality_statics_TraceOperationCompletion,
    causality_statics_TraceOperationRelation,
    causality_statics_TraceSynchronousWorkStart,
    causality_statics_TraceSynchronousWorkCompletion,
    causality_statics_add_TracingStatusChanged,
    causality_statics_remove_TracingStatusChanged,
};

static struct causality_tracer causality_tracer = {{&causality_factory_vtbl}, {&causality_statics_vtbl}, 1};

/* ---- the factories ---- */

enum diag_class { DIAG_CHANNEL, DIAG_ACTIVITY, DIAG_FIELDS, DIAG_OPTIONS, DIAG_CHANNEL_OPTIONS };

struct diag_factory
{
    IActivationFactory IActivationFactory_iface;
    ILoggingChannelFactory ILoggingChannelFactory_iface;
    ILoggingChannelFactory2 ILoggingChannelFactory2_iface;
    ILoggingChannelOptionsFactory ILoggingChannelOptionsFactory_iface;
    ILoggingOptionsFactory ILoggingOptionsFactory_iface;
    ILoggingActivityFactory ILoggingActivityFactory_iface;
    enum diag_class kind;
    LONG ref;
};

static inline struct diag_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct diag_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct diag_factory *impl = impl_from_IActivationFactory( iface );
    enum diag_class kind = impl->kind;

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingChannelFactory ) && kind == DIAG_CHANNEL)
        *out = &impl->ILoggingChannelFactory_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingChannelFactory2 ) && kind == DIAG_CHANNEL)
        *out = &impl->ILoggingChannelFactory2_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingChannelOptionsFactory ) &&
             kind == DIAG_CHANNEL_OPTIONS)
        *out = &impl->ILoggingChannelOptionsFactory_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingOptionsFactory ) && kind == DIAG_OPTIONS)
        *out = &impl->ILoggingOptionsFactory_iface;
    else if (IsEqualGUID( iid, &IID_ILoggingActivityFactory ) && kind == DIAG_ACTIVITY)
        *out = &impl->ILoggingActivityFactory_iface;
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

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    return inspectable_GetIids( (IInspectable *)iface, count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    FIXME( "iface %p, name %p stub!\n", iface, name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    FIXME( "iface %p, level %p stub!\n", iface, level );
    return E_NOTIMPL;
}

/* the default constructors: LoggingFields, LoggingOptions and LoggingChannelOptions have one */
static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    enum diag_class kind = impl_from_IActivationFactory( iface )->kind;

    TRACE( "iface %p, instance %p.\n", iface, instance );
    if (kind == DIAG_FIELDS) return fields_create( instance );
    if (kind == DIAG_OPTIONS)
        return logging_options_create( 0, (ILoggingOptions **)instance );
    if (kind == DIAG_CHANNEL_OPTIONS)
        return channel_options_create( NULL, (ILoggingChannelOptions **)instance );
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
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

DEFINE_IINSPECTABLE( channel_factory, ILoggingChannelFactory, struct diag_factory, IActivationFactory_iface )

/* Windows 8.1 compatibility mode: the channel has the LoggingChannel provider's id */
static HRESULT WINAPI channel_factory_Create( ILoggingChannelFactory *iface, HSTRING name, ILoggingChannel **result )
{
    TRACE( "iface %p, name %s, result %p.\n", iface, debugstr_hstring( name ), result );
    return channel_create( name, &compatibility_provider, result );
}

static const ILoggingChannelFactoryVtbl channel_factory_vtbl =
{
    channel_factory_QueryInterface,
    channel_factory_AddRef,
    channel_factory_Release,
    /* IInspectable methods */
    channel_factory_GetIids,
    channel_factory_GetRuntimeClassName,
    channel_factory_GetTrustLevel,
    /* ILoggingChannelFactory methods */
    channel_factory_Create,
};

DEFINE_IINSPECTABLE( channel_factory2, ILoggingChannelFactory2, struct diag_factory, IActivationFactory_iface )

/* the id from the name */
static HRESULT WINAPI channel_factory2_CreateWithOptions( ILoggingChannelFactory2 *iface, HSTRING name,
                                                          ILoggingChannelOptions *options, ILoggingChannel **result )
{
    TRACE( "iface %p, name %s, options %p, result %p.\n", iface, debugstr_hstring( name ), options, result );
    return channel_create( name, NULL, result );
}

static HRESULT WINAPI channel_factory2_CreateWithOptionsAndId( ILoggingChannelFactory2 *iface, HSTRING name,
                                                               ILoggingChannelOptions *options, GUID id, ILoggingChannel **result )
{
    TRACE( "iface %p, name %s, options %p, id %s, result %p.\n", iface, debugstr_hstring( name ), options,
           debugstr_guid( &id ), result );
    return channel_create( name, &id, result );
}

static const ILoggingChannelFactory2Vtbl channel_factory2_vtbl =
{
    channel_factory2_QueryInterface,
    channel_factory2_AddRef,
    channel_factory2_Release,
    /* IInspectable methods */
    channel_factory2_GetIids,
    channel_factory2_GetRuntimeClassName,
    channel_factory2_GetTrustLevel,
    /* ILoggingChannelFactory2 methods */
    channel_factory2_CreateWithOptions,
    channel_factory2_CreateWithOptionsAndId,
};

DEFINE_IINSPECTABLE( channel_options_factory, ILoggingChannelOptionsFactory, struct diag_factory, IActivationFactory_iface )

static HRESULT WINAPI channel_options_factory_Create( ILoggingChannelOptionsFactory *iface, GUID group,
                                                      ILoggingChannelOptions **result )
{
    TRACE( "iface %p, group %s, result %p.\n", iface, debugstr_guid( &group ), result );
    return channel_options_create( &group, result );
}

static const ILoggingChannelOptionsFactoryVtbl channel_options_factory_vtbl =
{
    channel_options_factory_QueryInterface,
    channel_options_factory_AddRef,
    channel_options_factory_Release,
    /* IInspectable methods */
    channel_options_factory_GetIids,
    channel_options_factory_GetRuntimeClassName,
    channel_options_factory_GetTrustLevel,
    /* ILoggingChannelOptionsFactory methods */
    channel_options_factory_Create,
};

DEFINE_IINSPECTABLE( logging_options_factory, ILoggingOptionsFactory, struct diag_factory, IActivationFactory_iface )

static HRESULT WINAPI logging_options_factory_CreateWithKeywords( ILoggingOptionsFactory *iface, INT64 keywords,
                                                                  ILoggingOptions **result )
{
    TRACE( "iface %p, keywords %#I64x, result %p.\n", iface, keywords, result );
    return logging_options_create( keywords, result );
}

static const ILoggingOptionsFactoryVtbl logging_options_factory_vtbl =
{
    logging_options_factory_QueryInterface,
    logging_options_factory_AddRef,
    logging_options_factory_Release,
    /* IInspectable methods */
    logging_options_factory_GetIids,
    logging_options_factory_GetRuntimeClassName,
    logging_options_factory_GetTrustLevel,
    /* ILoggingOptionsFactory methods */
    logging_options_factory_CreateWithKeywords,
};

DEFINE_IINSPECTABLE( activity_factory, ILoggingActivityFactory, struct diag_factory, IActivationFactory_iface )

static HRESULT WINAPI activity_factory_CreateLoggingActivity( ILoggingActivityFactory *iface, HSTRING name,
                                                              ILoggingChannel *channel, ILoggingActivity **result )
{
    TRACE( "iface %p, name %s, channel %p, result %p.\n", iface, debugstr_hstring( name ), channel, result );
    return activity_create( name, channel, result );
}

static HRESULT WINAPI activity_factory_CreateLoggingActivityWithLevel( ILoggingActivityFactory *iface, HSTRING name,
                                                                       ILoggingChannel *channel, LoggingLevel level,
                                                                       ILoggingActivity **result )
{
    return activity_factory_CreateLoggingActivity( iface, name, channel, result );
}

static const ILoggingActivityFactoryVtbl activity_factory_vtbl =
{
    activity_factory_QueryInterface,
    activity_factory_AddRef,
    activity_factory_Release,
    /* IInspectable methods */
    activity_factory_GetIids,
    activity_factory_GetRuntimeClassName,
    activity_factory_GetTrustLevel,
    /* ILoggingActivityFactory methods */
    activity_factory_CreateLoggingActivity,
    activity_factory_CreateLoggingActivityWithLevel,
};

#define DIAG_FACTORY( var, class ) \
    static struct diag_factory var = \
    { \
        {&factory_vtbl}, {&channel_factory_vtbl}, {&channel_factory2_vtbl}, {&channel_options_factory_vtbl}, \
        {&logging_options_factory_vtbl}, {&activity_factory_vtbl}, class, 1, \
    };

DIAG_FACTORY( channel_statics, DIAG_CHANNEL )
DIAG_FACTORY( activity_statics, DIAG_ACTIVITY )
DIAG_FACTORY( fields_statics, DIAG_FIELDS )
DIAG_FACTORY( options_statics, DIAG_OPTIONS )
DIAG_FACTORY( channel_options_statics, DIAG_CHANNEL_OPTIONS )

/* the factory of a Windows.Foundation.Diagnostics logging class, or null */
IActivationFactory *diagnostics_factory( const WCHAR *name )
{
    if (!wcscmp( name, RuntimeClass_Windows_Foundation_Diagnostics_AsyncCausalityTracer ))
        return &causality_tracer.IActivationFactory_iface;
    if (!wcscmp( name, RuntimeClass_Windows_Foundation_Diagnostics_LoggingChannel )) return &channel_statics.IActivationFactory_iface;
    if (!wcscmp( name, RuntimeClass_Windows_Foundation_Diagnostics_LoggingActivity )) return &activity_statics.IActivationFactory_iface;
    if (!wcscmp( name, RuntimeClass_Windows_Foundation_Diagnostics_LoggingFields )) return &fields_statics.IActivationFactory_iface;
    if (!wcscmp( name, RuntimeClass_Windows_Foundation_Diagnostics_LoggingOptions )) return &options_statics.IActivationFactory_iface;
    if (!wcscmp( name, RuntimeClass_Windows_Foundation_Diagnostics_LoggingChannelOptions ))
        return &channel_options_statics.IActivationFactory_iface;
    return NULL;
}
