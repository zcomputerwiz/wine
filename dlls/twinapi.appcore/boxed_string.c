/* A string in an object: IReference<HSTRING> and IPropertyValue of type String
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

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/* IReference<HSTRING> (the IID is derived from the parameterized type; widl does not make this one) */
static const GUID IID_IReference_HSTRING_local =
    {0xfd416dfb, 0x2a07, 0x52eb, {0xaa, 0xe3, 0xdf, 0xce, 0x14, 0x11, 0x6c, 0x05}};

struct string_reference;
struct string_referenceVtbl
{
    HRESULT (WINAPI *QueryInterface)( struct string_reference *iface, REFIID iid, void **out );
    ULONG (WINAPI *AddRef)( struct string_reference *iface );
    ULONG (WINAPI *Release)( struct string_reference *iface );
    HRESULT (WINAPI *GetIids)( struct string_reference *iface, ULONG *count, IID **iids );
    HRESULT (WINAPI *GetRuntimeClassName)( struct string_reference *iface, HSTRING *name );
    HRESULT (WINAPI *GetTrustLevel)( struct string_reference *iface, TrustLevel *level );
    HRESULT (WINAPI *get_Value)( struct string_reference *iface, HSTRING *value );
};
struct string_reference
{
    const struct string_referenceVtbl *lpVtbl;
};

struct boxed
{
    IPropertyValue IPropertyValue_iface;
    struct string_reference reference;
    LONG ref;
    HSTRING value;
};

static inline struct boxed *impl_from_IPropertyValue( IPropertyValue *iface )
{
    return CONTAINING_RECORD( iface, struct boxed, IPropertyValue_iface );
}

static inline struct boxed *impl_from_reference( struct string_reference *iface )
{
    return CONTAINING_RECORD( iface, struct boxed, reference );
}

static HRESULT WINAPI pv_QueryInterface( IPropertyValue *iface, REFIID iid, void **out )
{
    struct boxed *impl = impl_from_IPropertyValue( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IPropertyValue ))
        *out = &impl->IPropertyValue_iface;
    else if (IsEqualGUID( iid, &IID_IReference_HSTRING_local ))
        *out = &impl->reference;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)&impl->IPropertyValue_iface );
    return S_OK;
}

static ULONG WINAPI pv_AddRef( IPropertyValue *iface )
{
    return InterlockedIncrement( &impl_from_IPropertyValue( iface )->ref );
}

static ULONG WINAPI pv_Release( IPropertyValue *iface )
{
    struct boxed *impl = impl_from_IPropertyValue( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI pv_GetIids( IPropertyValue *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IPropertyValue;
    list[1] = IID_IReference_HSTRING_local;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI pv_GetRuntimeClassName( IPropertyValue *iface, HSTRING *name )
{
    return create_name( L"Windows.Foundation.IReference`1<String>", name );
}

static HRESULT WINAPI pv_GetTrustLevel( IPropertyValue *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI pv_get_Type( IPropertyValue *iface, PropertyType *value )
{
    *value = PropertyType_String;
    return S_OK;
}

static HRESULT WINAPI pv_get_IsNumericScalar( IPropertyValue *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI pv_GetString( IPropertyValue *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IPropertyValue( iface )->value, value );
}

/* every other type is a mismatch */
#define PV_SCALAR( name, type ) \
    static HRESULT WINAPI pv_##name( IPropertyValue *iface, type *value ) { return TYPE_E_TYPEMISMATCH; }
#define PV_ARRAY( name, type ) \
    static HRESULT WINAPI pv_##name( IPropertyValue *iface, UINT32 *size, type **value ) { return TYPE_E_TYPEMISMATCH; }

PV_SCALAR( GetUInt8, BYTE )
PV_SCALAR( GetInt16, INT16 )
PV_SCALAR( GetUInt16, UINT16 )
PV_SCALAR( GetInt32, INT32 )
PV_SCALAR( GetUInt32, UINT32 )
PV_SCALAR( GetInt64, INT64 )
PV_SCALAR( GetUInt64, UINT64 )
PV_SCALAR( GetSingle, FLOAT )
PV_SCALAR( GetDouble, DOUBLE )
PV_SCALAR( GetChar16, WCHAR )
PV_SCALAR( GetBoolean, boolean )
PV_SCALAR( GetGuid, GUID )
PV_SCALAR( GetDateTime, DateTime )
PV_SCALAR( GetTimeSpan, TimeSpan )
PV_SCALAR( GetPoint, Point )
PV_SCALAR( GetSize, Size )
PV_SCALAR( GetRect, Rect )
PV_ARRAY( GetUInt8Array, BYTE )
PV_ARRAY( GetInt16Array, INT16 )
PV_ARRAY( GetUInt16Array, UINT16 )
PV_ARRAY( GetInt32Array, INT32 )
PV_ARRAY( GetUInt32Array, UINT32 )
PV_ARRAY( GetInt64Array, INT64 )
PV_ARRAY( GetUInt64Array, UINT64 )
PV_ARRAY( GetSingleArray, FLOAT )
PV_ARRAY( GetDoubleArray, DOUBLE )
PV_ARRAY( GetChar16Array, WCHAR )
PV_ARRAY( GetBooleanArray, boolean )
PV_ARRAY( GetStringArray, HSTRING )
PV_ARRAY( GetInspectableArray, IInspectable * )
PV_ARRAY( GetGuidArray, GUID )
PV_ARRAY( GetDateTimeArray, DateTime )
PV_ARRAY( GetTimeSpanArray, TimeSpan )
PV_ARRAY( GetPointArray, Point )
PV_ARRAY( GetSizeArray, Size )
PV_ARRAY( GetRectArray, Rect )

static const IPropertyValueVtbl pv_vtbl =
{
    pv_QueryInterface,
    pv_AddRef,
    pv_Release,
    /* IInspectable methods */
    pv_GetIids,
    pv_GetRuntimeClassName,
    pv_GetTrustLevel,
    /* IPropertyValue methods */
    pv_get_Type,
    pv_get_IsNumericScalar,
    pv_GetUInt8,
    pv_GetInt16,
    pv_GetUInt16,
    pv_GetInt32,
    pv_GetUInt32,
    pv_GetInt64,
    pv_GetUInt64,
    pv_GetSingle,
    pv_GetDouble,
    pv_GetChar16,
    pv_GetBoolean,
    pv_GetString,
    pv_GetGuid,
    pv_GetDateTime,
    pv_GetTimeSpan,
    pv_GetPoint,
    pv_GetSize,
    pv_GetRect,
    pv_GetUInt8Array,
    pv_GetInt16Array,
    pv_GetUInt16Array,
    pv_GetInt32Array,
    pv_GetUInt32Array,
    pv_GetInt64Array,
    pv_GetUInt64Array,
    pv_GetSingleArray,
    pv_GetDoubleArray,
    pv_GetChar16Array,
    pv_GetBooleanArray,
    pv_GetStringArray,
    pv_GetInspectableArray,
    pv_GetGuidArray,
    pv_GetDateTimeArray,
    pv_GetTimeSpanArray,
    pv_GetPointArray,
    pv_GetSizeArray,
    pv_GetRectArray,
};

static HRESULT WINAPI ref_QueryInterface( struct string_reference *iface, REFIID iid, void **out )
{
    return pv_QueryInterface( &impl_from_reference( iface )->IPropertyValue_iface, iid, out );
}

static ULONG WINAPI ref_AddRef( struct string_reference *iface )
{
    return pv_AddRef( &impl_from_reference( iface )->IPropertyValue_iface );
}

static ULONG WINAPI ref_Release( struct string_reference *iface )
{
    return pv_Release( &impl_from_reference( iface )->IPropertyValue_iface );
}

static HRESULT WINAPI ref_GetIids( struct string_reference *iface, ULONG *count, IID **iids )
{
    return pv_GetIids( &impl_from_reference( iface )->IPropertyValue_iface, count, iids );
}

static HRESULT WINAPI ref_GetRuntimeClassName( struct string_reference *iface, HSTRING *name )
{
    return pv_GetRuntimeClassName( &impl_from_reference( iface )->IPropertyValue_iface, name );
}

static HRESULT WINAPI ref_GetTrustLevel( struct string_reference *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ref_get_Value( struct string_reference *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_reference( iface )->value, value );
}

static const struct string_referenceVtbl ref_vtbl =
{
    ref_QueryInterface,
    ref_AddRef,
    ref_Release,
    ref_GetIids,
    ref_GetRuntimeClassName,
    ref_GetTrustLevel,
    ref_get_Value,
};

HRESULT boxed_string_create( HSTRING value, IInspectable **out )
{
    struct boxed *impl;
    HRESULT hr;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (FAILED( hr = WindowsDuplicateString( value, &impl->value )))
    {
        free( impl );
        return hr;
    }
    impl->IPropertyValue_iface.lpVtbl = &pv_vtbl;
    impl->reference.lpVtbl = &ref_vtbl;
    impl->ref = 1;
    *out = (IInspectable *)&impl->IPropertyValue_iface;
    return S_OK;
}

/* the string of an object that holds one (a boxed string made by an app is an IPropertyValue as well) */
HRESULT inspectable_get_string( IInspectable *object, HSTRING *value )
{
    IPropertyValue *pv;
    HRESULT hr;

    if (!object) return E_POINTER;
    if (FAILED( hr = IInspectable_QueryInterface( object, &IID_IPropertyValue, (void **)&pv ))) return hr;
    hr = IPropertyValue_GetString( pv, value );
    IPropertyValue_Release( pv );
    return hr;
}
