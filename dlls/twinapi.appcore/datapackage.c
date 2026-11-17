/* Windows.ApplicationModel.DataTransfer.DataPackage and DataPackageView
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

/* ---- the formats of a package, shared by the package, its views and the clipboard snapshots ---- */

enum format_kind
{
    FORMAT_STRING,    /* text, HTML and RTF */
    FORMAT_OBJECT,    /* an object given to SetData, or a Uri */
    FORMAT_PROVIDER,  /* a delayed renderer (kept, not called) */
};

struct format
{
    HSTRING id;
    enum format_kind kind;
    HSTRING string;
    IUnknown *object;
};

struct data_store
{
    LONG ref;
    CRITICAL_SECTION cs;
    struct format *formats;
    UINT32 count, capacity;
    IDataPackagePropertySet *props;
    DataPackageOperation requested;
    /* the package that owns the store, while it lives: the sender of OperationCompleted */
    IDataPackage *owner;
    struct list operation_completed, destroyed, share_completed, share_canceled;
};

HRESULT data_store_create( struct data_store **out )
{
    struct data_store *store;
    HRESULT hr;

    if (!(store = calloc( 1, sizeof(*store) ))) return E_OUTOFMEMORY;
    store->ref = 1;
    store->requested = DataPackageOperation_Copy;
    list_init( &store->operation_completed );
    list_init( &store->destroyed );
    list_init( &store->share_completed );
    list_init( &store->share_canceled );
    InitializeCriticalSectionEx( &store->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    store->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": data_store.cs");
    if (FAILED( hr = data_props_create( &store->props )))
    {
        DeleteCriticalSection( &store->cs );
        free( store );
        return hr;
    }
    *out = store;
    return S_OK;
}

void data_store_addref( struct data_store *store )
{
    InterlockedIncrement( &store->ref );
}

static void format_clear( struct format *format )
{
    WindowsDeleteString( format->id );
    WindowsDeleteString( format->string );
    if (format->object) IUnknown_Release( format->object );
}

void data_store_release( struct data_store *store )
{
    UINT32 i;

    if (InterlockedDecrement( &store->ref )) return;
    for (i = 0; i < store->count; i++) format_clear( &store->formats[i] );
    free( store->formats );
    handler_list_clear( &store->operation_completed );
    handler_list_clear( &store->destroyed );
    handler_list_clear( &store->share_completed );
    handler_list_clear( &store->share_canceled );
    IDataPackagePropertySet_Release( store->props );
    DeleteCriticalSection( &store->cs );
    free( store );
}

static BOOL format_matches( const struct format *format, HSTRING id )
{
    return CompareStringOrdinal( WindowsGetStringRawBuffer( format->id, NULL ), -1,
                                 WindowsGetStringRawBuffer( id, NULL ), -1, TRUE ) == CSTR_EQUAL;
}

/* called with the lock held */
static struct format *store_find( struct data_store *store, HSTRING id )
{
    UINT32 i;

    for (i = 0; i < store->count; i++)
        if (format_matches( &store->formats[i], id )) return &store->formats[i];
    return NULL;
}

/* adds or replaces a format; the string and the object are taken as given (copied here) */
static HRESULT store_set( struct data_store *store, HSTRING id, enum format_kind kind, HSTRING string, IUnknown *object )
{
    struct format new = {0}, *format, *formats;
    HRESULT hr;

    if (!WindowsGetStringLen( id )) return E_INVALIDARG;
    if (FAILED( hr = WindowsDuplicateString( id, &new.id ) )) return hr;
    if (FAILED( hr = WindowsDuplicateString( string, &new.string ) ))
    {
        WindowsDeleteString( new.id );
        return hr;
    }
    new.kind = kind;
    if ((new.object = object)) IUnknown_AddRef( object );

    EnterCriticalSection( &store->cs );
    if ((format = store_find( store, id )))
    {
        format_clear( format );
        *format = new;
    }
    else if (store->count < store->capacity ||
             (formats = realloc( store->formats, max( 4, store->capacity * 2 ) * sizeof(*formats) )))
    {
        if (store->count == store->capacity)
        {
            store->formats = formats;
            store->capacity = max( 4, store->capacity * 2 );
        }
        store->formats[store->count++] = new;
    }
    else
    {
        format_clear( &new );
        hr = E_OUTOFMEMORY;
    }
    LeaveCriticalSection( &store->cs );
    return hr;
}

HRESULT data_store_set_string( struct data_store *store, const WCHAR *format_id, const WCHAR *value )
{
    HSTRING id = NULL, string = NULL;
    HRESULT hr;

    if (SUCCEEDED( hr = WindowsCreateString( format_id, wcslen( format_id ), &id ) ) &&
        SUCCEEDED( hr = WindowsCreateString( value, wcslen( value ), &string ) ))
        hr = store_set( store, id, FORMAT_STRING, string, NULL );
    WindowsDeleteString( id );
    WindowsDeleteString( string );
    return hr;
}

static HRESULT store_set_const( struct data_store *store, const WCHAR *format_id, HSTRING string, IUnknown *object )
{
    HSTRING id;
    HRESULT hr;

    if (FAILED( hr = WindowsCreateString( format_id, wcslen( format_id ), &id ) )) return hr;
    hr = store_set( store, id, object ? FORMAT_OBJECT : FORMAT_STRING, string, object );
    WindowsDeleteString( id );
    return hr;
}

/* a copy of a format, or NOT_FOUND */
static HRESULT store_get( struct data_store *store, HSTRING id, struct format *copy )
{
    struct format *format;
    HRESULT hr = HRESULT_FROM_WIN32( ERROR_NOT_FOUND );

    memset( copy, 0, sizeof(*copy) );
    EnterCriticalSection( &store->cs );
    if ((format = store_find( store, id )))
    {
        copy->kind = format->kind;
        if (SUCCEEDED( hr = WindowsDuplicateString( format->string, &copy->string ) ) && format->object)
        {
            copy->object = format->object;
            IUnknown_AddRef( copy->object );
        }
    }
    LeaveCriticalSection( &store->cs );
    return hr;
}

static HRESULT store_get_const( struct data_store *store, const WCHAR *format_id, struct format *copy )
{
    HSTRING id;
    HRESULT hr;

    if (FAILED( hr = WindowsCreateString( format_id, wcslen( format_id ), &id ) )) return hr;
    hr = store_get( store, id, copy );
    WindowsDeleteString( id );
    return hr;
}

static void format_copy_clear( struct format *copy )
{
    WindowsDeleteString( copy->string );
    if (copy->object) IUnknown_Release( copy->object );
}

/* ---- the event args of OperationCompleted ---- */

struct operation_args
{
    IOperationCompletedEventArgs IOperationCompletedEventArgs_iface;
    IOperationCompletedEventArgs2 IOperationCompletedEventArgs2_iface;
    LONG ref;
    DataPackageOperation operation;
    HSTRING accepted_format;
};

static inline struct operation_args *impl_from_IOperationCompletedEventArgs( IOperationCompletedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct operation_args, IOperationCompletedEventArgs_iface );
}

static HRESULT WINAPI args_QueryInterface( IOperationCompletedEventArgs *iface, REFIID iid, void **out )
{
    struct operation_args *impl = impl_from_IOperationCompletedEventArgs( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IOperationCompletedEventArgs ))
        *out = &impl->IOperationCompletedEventArgs_iface;
    else if (IsEqualGUID( iid, &IID_IOperationCompletedEventArgs2 ))
        *out = &impl->IOperationCompletedEventArgs2_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI args_AddRef( IOperationCompletedEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_IOperationCompletedEventArgs( iface )->ref );
}

static ULONG WINAPI args_Release( IOperationCompletedEventArgs *iface )
{
    struct operation_args *impl = impl_from_IOperationCompletedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->accepted_format );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI args_GetIids( IOperationCompletedEventArgs *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IOperationCompletedEventArgs;
    list[1] = IID_IOperationCompletedEventArgs2;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI args_GetRuntimeClassName( IOperationCompletedEventArgs *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.OperationCompletedEventArgs", name );
}

static HRESULT WINAPI args_GetTrustLevel( IOperationCompletedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI args_get_Operation( IOperationCompletedEventArgs *iface, DataPackageOperation *value )
{
    *value = impl_from_IOperationCompletedEventArgs( iface )->operation;
    return S_OK;
}

static const IOperationCompletedEventArgsVtbl args_vtbl =
{
    args_QueryInterface,
    args_AddRef,
    args_Release,
    /* IInspectable methods */
    args_GetIids,
    args_GetRuntimeClassName,
    args_GetTrustLevel,
    /* IOperationCompletedEventArgs methods */
    args_get_Operation,
};

DEFINE_IINSPECTABLE( args2, IOperationCompletedEventArgs2, struct operation_args, IOperationCompletedEventArgs_iface )

static HRESULT WINAPI args2_get_AcceptedFormatId( IOperationCompletedEventArgs2 *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IOperationCompletedEventArgs2( iface )->accepted_format, value );
}

static const IOperationCompletedEventArgs2Vtbl args2_vtbl =
{
    args2_QueryInterface,
    args2_AddRef,
    args2_Release,
    /* IInspectable methods */
    args2_GetIids,
    args2_GetRuntimeClassName,
    args2_GetTrustLevel,
    /* IOperationCompletedEventArgs2 methods */
    args2_get_AcceptedFormatId,
};

static void store_raise_operation_completed( struct data_store *store, DataPackageOperation operation, HSTRING format )
{
    struct operation_args *args;
    IDataPackage *owner;
    IUnknown **handlers;
    unsigned int i, count;

    EnterCriticalSection( &store->cs );
    if ((owner = store->owner)) IDataPackage_AddRef( owner );
    LeaveCriticalSection( &store->cs );
    if (!owner) return;

    if ((args = calloc( 1, sizeof(*args) )))
    {
        args->IOperationCompletedEventArgs_iface.lpVtbl = &args_vtbl;
        args->IOperationCompletedEventArgs2_iface.lpVtbl = &args2_vtbl;
        args->ref = 1;
        args->operation = operation;
        WindowsDuplicateString( format, &args->accepted_format );

        handlers = handler_list_snapshot( &store->operation_completed, &store->cs, &count );
        for (i = 0; i < count; i++)
            ITypedEventHandler_DataPackage_OperationCompletedEventArgs_Invoke(
                (ITypedEventHandler_DataPackage_OperationCompletedEventArgs *)handlers[i], owner,
                &args->IOperationCompletedEventArgs_iface );
        handler_list_snapshot_free( handlers, count );
        IOperationCompletedEventArgs_Release( &args->IOperationCompletedEventArgs_iface );
    }
    IDataPackage_Release( owner );
}

/* ---- the package ---- */

struct data_package
{
    IDataPackage IDataPackage_iface;
    IDataPackage2 IDataPackage2_iface;
    IDataPackage3 IDataPackage3_iface;
    IDataPackage4 IDataPackage4_iface;
    LONG ref;
    struct data_store *store;
};

static inline struct data_package *impl_from_IDataPackage( IDataPackage *iface )
{
    return CONTAINING_RECORD( iface, struct data_package, IDataPackage_iface );
}

static HRESULT WINAPI package_QueryInterface( IDataPackage *iface, REFIID iid, void **out )
{
    struct data_package *impl = impl_from_IDataPackage( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDataPackage ))
        *out = &impl->IDataPackage_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackage2 ))
        *out = &impl->IDataPackage2_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackage3 ))
        *out = &impl->IDataPackage3_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackage4 ))
        *out = &impl->IDataPackage4_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI package_AddRef( IDataPackage *iface )
{
    return InterlockedIncrement( &impl_from_IDataPackage( iface )->ref );
}

static ULONG WINAPI package_Release( IDataPackage *iface )
{
    struct data_package *impl = impl_from_IDataPackage( iface );
    struct data_store *store = impl->store;
    ULONG ref;

    /* the owner is cleared under the lock that takes a reference to it, so it is never taken while this is dying */
    EnterCriticalSection( &store->cs );
    if (!(ref = InterlockedDecrement( &impl->ref ))) store->owner = NULL;
    LeaveCriticalSection( &store->cs );
    if (!ref)
    {
        data_store_release( store );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI package_GetIids( IDataPackage *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 4 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IDataPackage;
    list[1] = IID_IDataPackage2;
    list[2] = IID_IDataPackage3;
    list[3] = IID_IDataPackage4;
    *count = 4;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI package_GetRuntimeClassName( IDataPackage *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.DataPackage", name );
}

static HRESULT WINAPI package_GetTrustLevel( IDataPackage *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI package_GetView( IDataPackage *iface, IDataPackageView **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return data_package_view_create( impl_from_IDataPackage( iface )->store, result );
}

static HRESULT WINAPI package_get_Properties( IDataPackage *iface, IDataPackagePropertySet **value )
{
    struct data_store *store = impl_from_IDataPackage( iface )->store;

    TRACE( "iface %p, value %p.\n", iface, value );
    IDataPackagePropertySet_AddRef( (*value = store->props) );
    return S_OK;
}

static HRESULT WINAPI package_get_RequestedOperation( IDataPackage *iface, DataPackageOperation *value )
{
    struct data_store *store = impl_from_IDataPackage( iface )->store;

    EnterCriticalSection( &store->cs );
    *value = store->requested;
    LeaveCriticalSection( &store->cs );
    return S_OK;
}

static HRESULT WINAPI package_put_RequestedOperation( IDataPackage *iface, DataPackageOperation value )
{
    struct data_store *store = impl_from_IDataPackage( iface )->store;

    TRACE( "iface %p, value %#x.\n", iface, value );
    EnterCriticalSection( &store->cs );
    store->requested = value;
    LeaveCriticalSection( &store->cs );
    return S_OK;
}

static HRESULT WINAPI package_add_OperationCompleted( IDataPackage *iface,
                                                      ITypedEventHandler_DataPackage_OperationCompletedEventArgs *handler,
                                                      EventRegistrationToken *token )
{
    struct data_store *store = impl_from_IDataPackage( iface )->store;

    return handler_list_add( &store->operation_completed, &store->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI package_remove_OperationCompleted( IDataPackage *iface, EventRegistrationToken token )
{
    struct data_store *store = impl_from_IDataPackage( iface )->store;

    return handler_list_remove( &store->operation_completed, &store->cs, token );
}

/* Destroyed is kept but never raised */
static HRESULT WINAPI package_add_Destroyed( IDataPackage *iface, ITypedEventHandler_DataPackage_IInspectable *handler,
                                             EventRegistrationToken *token )
{
    struct data_store *store = impl_from_IDataPackage( iface )->store;

    return handler_list_add( &store->destroyed, &store->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI package_remove_Destroyed( IDataPackage *iface, EventRegistrationToken token )
{
    struct data_store *store = impl_from_IDataPackage( iface )->store;

    return handler_list_remove( &store->destroyed, &store->cs, token );
}

static HRESULT WINAPI package_SetData( IDataPackage *iface, HSTRING format_id, IInspectable *value )
{
    TRACE( "iface %p, format_id %s, value %p.\n", iface, debugstr_hstring( format_id ), value );
    return store_set( impl_from_IDataPackage( iface )->store, format_id, FORMAT_OBJECT, NULL, (IUnknown *)value );
}

static HRESULT WINAPI package_SetDataProvider( IDataPackage *iface, HSTRING format_id, IDataProviderHandler *handler )
{
    FIXME( "iface %p, format_id %s, handler %p: kept, never called.\n", iface, debugstr_hstring( format_id ), handler );
    return store_set( impl_from_IDataPackage( iface )->store, format_id, FORMAT_PROVIDER, NULL, (IUnknown *)handler );
}

static HRESULT WINAPI package_SetText( IDataPackage *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return store_set_const( impl_from_IDataPackage( iface )->store, DATA_FORMAT_TEXT, value, NULL );
}

static HRESULT WINAPI package_SetUri( IDataPackage *iface, IUriRuntimeClass *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_INVALIDARG;
    return store_set_const( impl_from_IDataPackage( iface )->store, DATA_FORMAT_URI, NULL, (IUnknown *)value );
}

static HRESULT WINAPI package_SetHtmlFormat( IDataPackage *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return store_set_const( impl_from_IDataPackage( iface )->store, DATA_FORMAT_HTML, value, NULL );
}

static HRESULT WINAPI package_get_ResourceMap( IDataPackage *iface, IMap_HSTRING_RandomAccessStreamReference **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_SetRtf( IDataPackage *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return store_set_const( impl_from_IDataPackage( iface )->store, DATA_FORMAT_RTF, value, NULL );
}

static HRESULT WINAPI package_SetBitmap( IDataPackage *iface, IRandomAccessStreamReference *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_SetStorageItemsReadOnly( IDataPackage *iface, IIterable_IStorageItem *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI package_SetStorageItems( IDataPackage *iface, IIterable_IStorageItem *value, boolean read_only )
{
    FIXME( "iface %p, value %p, read_only %d stub!\n", iface, value, read_only );
    return E_NOTIMPL;
}

static const IDataPackageVtbl package_vtbl =
{
    package_QueryInterface,
    package_AddRef,
    package_Release,
    /* IInspectable methods */
    package_GetIids,
    package_GetRuntimeClassName,
    package_GetTrustLevel,
    /* IDataPackage methods */
    package_GetView,
    package_get_Properties,
    package_get_RequestedOperation,
    package_put_RequestedOperation,
    package_add_OperationCompleted,
    package_remove_OperationCompleted,
    package_add_Destroyed,
    package_remove_Destroyed,
    package_SetData,
    package_SetDataProvider,
    package_SetText,
    package_SetUri,
    package_SetHtmlFormat,
    package_get_ResourceMap,
    package_SetRtf,
    package_SetBitmap,
    package_SetStorageItemsReadOnly,
    package_SetStorageItems,
};

DEFINE_IINSPECTABLE( package2, IDataPackage2, struct data_package, IDataPackage_iface )

static HRESULT WINAPI package2_SetApplicationLink( IDataPackage2 *iface, IUriRuntimeClass *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_INVALIDARG;
    return store_set_const( impl_from_IDataPackage2( iface )->store, L"ApplicationLink", NULL, (IUnknown *)value );
}

static HRESULT WINAPI package2_SetWebLink( IDataPackage2 *iface, IUriRuntimeClass *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_INVALIDARG;
    return store_set_const( impl_from_IDataPackage2( iface )->store, L"WebLink", NULL, (IUnknown *)value );
}

static const IDataPackage2Vtbl package2_vtbl =
{
    package2_QueryInterface,
    package2_AddRef,
    package2_Release,
    /* IInspectable methods */
    package2_GetIids,
    package2_GetRuntimeClassName,
    package2_GetTrustLevel,
    /* IDataPackage2 methods */
    package2_SetApplicationLink,
    package2_SetWebLink,
};

DEFINE_IINSPECTABLE( package3, IDataPackage3, struct data_package, IDataPackage_iface )

/* ShareCompleted is kept but never raised: there is no share UI */
static HRESULT WINAPI package3_add_ShareCompleted( IDataPackage3 *iface,
                                                   ITypedEventHandler_DataPackage_ShareCompletedEventArgs *handler,
                                                   EventRegistrationToken *token )
{
    struct data_store *store = impl_from_IDataPackage3( iface )->store;

    return handler_list_add( &store->share_completed, &store->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI package3_remove_ShareCompleted( IDataPackage3 *iface, EventRegistrationToken token )
{
    struct data_store *store = impl_from_IDataPackage3( iface )->store;

    return handler_list_remove( &store->share_completed, &store->cs, token );
}

static const IDataPackage3Vtbl package3_vtbl =
{
    package3_QueryInterface,
    package3_AddRef,
    package3_Release,
    /* IInspectable methods */
    package3_GetIids,
    package3_GetRuntimeClassName,
    package3_GetTrustLevel,
    /* IDataPackage3 methods */
    package3_add_ShareCompleted,
    package3_remove_ShareCompleted,
};

DEFINE_IINSPECTABLE( package4, IDataPackage4, struct data_package, IDataPackage_iface )

static HRESULT WINAPI package4_add_ShareCanceled( IDataPackage4 *iface, ITypedEventHandler_DataPackage_IInspectable *handler,
                                                  EventRegistrationToken *token )
{
    struct data_store *store = impl_from_IDataPackage4( iface )->store;

    return handler_list_add( &store->share_canceled, &store->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI package4_remove_ShareCanceled( IDataPackage4 *iface, EventRegistrationToken token )
{
    struct data_store *store = impl_from_IDataPackage4( iface )->store;

    return handler_list_remove( &store->share_canceled, &store->cs, token );
}

static const IDataPackage4Vtbl package4_vtbl =
{
    package4_QueryInterface,
    package4_AddRef,
    package4_Release,
    /* IInspectable methods */
    package4_GetIids,
    package4_GetRuntimeClassName,
    package4_GetTrustLevel,
    /* IDataPackage4 methods */
    package4_add_ShareCanceled,
    package4_remove_ShareCanceled,
};

static HRESULT package_create( IDataPackage **out )
{
    struct data_package *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDataPackage_iface.lpVtbl = &package_vtbl;
    impl->IDataPackage2_iface.lpVtbl = &package2_vtbl;
    impl->IDataPackage3_iface.lpVtbl = &package3_vtbl;
    impl->IDataPackage4_iface.lpVtbl = &package4_vtbl;
    impl->ref = 1;
    if (FAILED( hr = data_store_create( &impl->store )))
    {
        free( impl );
        return hr;
    }
    impl->store->owner = &impl->IDataPackage_iface;
    *out = &impl->IDataPackage_iface;
    return S_OK;
}

/* ---- the view ---- */

struct package_view
{
    IDataPackageView IDataPackageView_iface;
    IDataPackageView2 IDataPackageView2_iface;
    IDataPackageView3 IDataPackageView3_iface;
    IDataPackageView4 IDataPackageView4_iface;
    LONG ref;
    struct data_store *store;
    HSTRING accepted_format;
};

static inline struct package_view *impl_from_IDataPackageView( IDataPackageView *iface )
{
    return CONTAINING_RECORD( iface, struct package_view, IDataPackageView_iface );
}

static HRESULT WINAPI view_QueryInterface( IDataPackageView *iface, REFIID iid, void **out )
{
    struct package_view *impl = impl_from_IDataPackageView( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDataPackageView ))
        *out = &impl->IDataPackageView_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackageView2 ))
        *out = &impl->IDataPackageView2_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackageView3 ))
        *out = &impl->IDataPackageView3_iface;
    else if (IsEqualGUID( iid, &IID_IDataPackageView4 ))
        *out = &impl->IDataPackageView4_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI view_AddRef( IDataPackageView *iface )
{
    return InterlockedIncrement( &impl_from_IDataPackageView( iface )->ref );
}

static ULONG WINAPI view_Release( IDataPackageView *iface )
{
    struct package_view *impl = impl_from_IDataPackageView( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->accepted_format );
        data_store_release( impl->store );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI view_GetIids( IDataPackageView *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 4 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IDataPackageView;
    list[1] = IID_IDataPackageView2;
    list[2] = IID_IDataPackageView3;
    list[3] = IID_IDataPackageView4;
    *count = 4;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI view_GetRuntimeClassName( IDataPackageView *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.DataPackageView", name );
}

static HRESULT WINAPI view_GetTrustLevel( IDataPackageView *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI view_get_Properties( IDataPackageView *iface, IDataPackagePropertySetView **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return data_props_view_create( impl_from_IDataPackageView( iface )->store->props, value );
}

static HRESULT WINAPI view_get_RequestedOperation( IDataPackageView *iface, DataPackageOperation *value )
{
    struct data_store *store = impl_from_IDataPackageView( iface )->store;

    EnterCriticalSection( &store->cs );
    *value = store->requested;
    LeaveCriticalSection( &store->cs );
    return S_OK;
}

static HRESULT WINAPI view_ReportOperationCompleted( IDataPackageView *iface, DataPackageOperation value )
{
    struct package_view *impl = impl_from_IDataPackageView( iface );

    TRACE( "iface %p, value %#x.\n", iface, value );
    store_raise_operation_completed( impl->store, value, impl->accepted_format );
    return S_OK;
}

static HRESULT WINAPI view_get_AvailableFormats( IDataPackageView *iface, IVectorView_HSTRING **format_ids )
{
    struct data_store *store = impl_from_IDataPackageView( iface )->store;
    HSTRING *ids;
    UINT32 i, count;
    HRESULT hr = S_OK;

    TRACE( "iface %p, format_ids %p.\n", iface, format_ids );

    EnterCriticalSection( &store->cs );
    count = store->count;
    if (!(ids = calloc( max( count, 1 ), sizeof(*ids) ))) hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED( hr ) && i < count; i++)
        hr = WindowsDuplicateString( store->formats[i].id, &ids[i] );
    LeaveCriticalSection( &store->cs );

    if (SUCCEEDED( hr )) hr = hstring_vector_create( FALSE, ids, count, (void **)format_ids );
    if (ids) for (i = 0; i < count; i++) WindowsDeleteString( ids[i] );
    free( ids );
    return hr;
}

static HRESULT WINAPI view_Contains( IDataPackageView *iface, HSTRING format_id, boolean *value )
{
    struct data_store *store = impl_from_IDataPackageView( iface )->store;

    TRACE( "iface %p, format_id %s, value %p.\n", iface, debugstr_hstring( format_id ), value );
    EnterCriticalSection( &store->cs );
    *value = !!store_find( store, format_id );
    LeaveCriticalSection( &store->cs );
    return S_OK;
}

static HRESULT view_get_data( struct package_view *impl, HSTRING format_id, IAsyncOperation_IInspectable **operation )
{
    struct format copy;
    IInspectable *object = NULL;
    HRESULT hr;

    if (SUCCEEDED( hr = store_get( impl->store, format_id, &copy ) ))
    {
        if (copy.kind == FORMAT_OBJECT) IInspectable_AddRef( (object = (IInspectable *)copy.object) );
        else if (copy.kind == FORMAT_STRING) hr = boxed_string_create( copy.string, &object );
        else
        {
            FIXME( "format %s has a delayed renderer.\n", debugstr_hstring( format_id ) );
            hr = E_NOTIMPL;
        }
        format_copy_clear( &copy );
    }
    hr = async_inspectable_completed( object, hr, operation );
    if (object) IInspectable_Release( object );
    return hr;
}

static HRESULT WINAPI view_GetDataAsync( IDataPackageView *iface, HSTRING format_id,
                                         IAsyncOperation_IInspectable **operation )
{
    TRACE( "iface %p, format_id %s, operation %p.\n", iface, debugstr_hstring( format_id ), operation );
    return view_get_data( impl_from_IDataPackageView( iface ), format_id, operation );
}

static HRESULT view_get_string( struct package_view *impl, HSTRING format_id, IAsyncOperation_HSTRING **operation )
{
    struct format copy;
    HSTRING string = NULL;
    HRESULT hr;

    if (SUCCEEDED( hr = store_get( impl->store, format_id, &copy ) ))
    {
        if (copy.kind == FORMAT_STRING) hr = WindowsDuplicateString( copy.string, &string );
        else if (copy.kind == FORMAT_OBJECT) hr = inspectable_get_string( (IInspectable *)copy.object, &string );
        else hr = E_NOTIMPL;
        format_copy_clear( &copy );
    }
    if (FAILED( hr )) TRACE( "format %s: %#lx.\n", debugstr_hstring( format_id ), hr );
    hr = async_hstring_completed( string, hr, operation );
    WindowsDeleteString( string );
    return hr;
}

static HRESULT view_get_string_const( struct package_view *impl, const WCHAR *format, IAsyncOperation_HSTRING **operation )
{
    HSTRING id;
    HRESULT hr;

    if (FAILED( hr = WindowsCreateString( format, wcslen( format ), &id ) )) return hr;
    hr = view_get_string( impl, id, operation );
    WindowsDeleteString( id );
    return hr;
}

static HRESULT view_get_uri( struct package_view *impl, const WCHAR *format, IAsyncOperation_Uri **operation )
{
    struct format copy;
    HRESULT hr;

    if (SUCCEEDED( hr = store_get_const( impl->store, format, &copy ) ))
    {
        if (copy.kind != FORMAT_OBJECT) hr = E_NOTIMPL;
        hr = async_uri_completed( (IUriRuntimeClass *)copy.object, hr, operation );
        format_copy_clear( &copy );
        return hr;
    }
    return async_uri_completed( NULL, hr, operation );
}

static HRESULT WINAPI view_GetTextAsync( IDataPackageView *iface, IAsyncOperation_HSTRING **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return view_get_string_const( impl_from_IDataPackageView( iface ), DATA_FORMAT_TEXT, operation );
}

static HRESULT WINAPI view_GetCustomTextAsync( IDataPackageView *iface, HSTRING format_id,
                                               IAsyncOperation_HSTRING **operation )
{
    TRACE( "iface %p, format_id %s, operation %p.\n", iface, debugstr_hstring( format_id ), operation );
    return view_get_string( impl_from_IDataPackageView( iface ), format_id, operation );
}

static HRESULT WINAPI view_GetUriAsync( IDataPackageView *iface, IAsyncOperation_Uri **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return view_get_uri( impl_from_IDataPackageView( iface ), DATA_FORMAT_URI, operation );
}

static HRESULT WINAPI view_GetHtmlFormatAsync( IDataPackageView *iface, IAsyncOperation_HSTRING **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return view_get_string_const( impl_from_IDataPackageView( iface ), DATA_FORMAT_HTML, operation );
}

static HRESULT WINAPI view_GetResourceMapAsync( IDataPackageView *iface,
                                                IAsyncOperation_IMapView_HSTRING_RandomAccessStreamReference **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI view_GetRtfAsync( IDataPackageView *iface, IAsyncOperation_HSTRING **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return view_get_string_const( impl_from_IDataPackageView( iface ), DATA_FORMAT_RTF, operation );
}

static HRESULT WINAPI view_GetBitmapAsync( IDataPackageView *iface, IAsyncOperation_RandomAccessStreamReference **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI view_GetStorageItemsAsync( IDataPackageView *iface,
                                                 IAsyncOperation_IVectorView_IStorageItem **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static const IDataPackageViewVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    /* IInspectable methods */
    view_GetIids,
    view_GetRuntimeClassName,
    view_GetTrustLevel,
    /* IDataPackageView methods */
    view_get_Properties,
    view_get_RequestedOperation,
    view_ReportOperationCompleted,
    view_get_AvailableFormats,
    view_Contains,
    view_GetDataAsync,
    view_GetTextAsync,
    view_GetCustomTextAsync,
    view_GetUriAsync,
    view_GetHtmlFormatAsync,
    view_GetResourceMapAsync,
    view_GetRtfAsync,
    view_GetBitmapAsync,
    view_GetStorageItemsAsync,
};

DEFINE_IINSPECTABLE( view2, IDataPackageView2, struct package_view, IDataPackageView_iface )

static HRESULT WINAPI view2_GetApplicationLinkAsync( IDataPackageView2 *iface, IAsyncOperation_Uri **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return view_get_uri( impl_from_IDataPackageView2( iface ), L"ApplicationLink", operation );
}

static HRESULT WINAPI view2_GetWebLinkAsync( IDataPackageView2 *iface, IAsyncOperation_Uri **operation )
{
    TRACE( "iface %p, operation %p.\n", iface, operation );
    return view_get_uri( impl_from_IDataPackageView2( iface ), L"WebLink", operation );
}

static const IDataPackageView2Vtbl view2_vtbl =
{
    view2_QueryInterface,
    view2_AddRef,
    view2_Release,
    /* IInspectable methods */
    view2_GetIids,
    view2_GetRuntimeClassName,
    view2_GetTrustLevel,
    /* IDataPackageView2 methods */
    view2_GetApplicationLinkAsync,
    view2_GetWebLinkAsync,
};

DEFINE_IINSPECTABLE( view3, IDataPackageView3, struct package_view, IDataPackageView_iface )

static HRESULT WINAPI view3_RequestAccessAsync( IDataPackageView3 *iface,
                                                IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, operation %p stub!\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI view3_RequestAccessWithEnterpriseIdAsync( IDataPackageView3 *iface, HSTRING enterprise_id,
                                                                IAsyncOperation_ProtectionPolicyEvaluationResult **operation )
{
    FIXME( "iface %p, enterprise_id %s, operation %p stub!\n", iface, debugstr_hstring( enterprise_id ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI view3_UnlockAndAssumeEnterpriseIdentity( IDataPackageView3 *iface,
                                                               ProtectionPolicyEvaluationResult *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    *result = ProtectionPolicyEvaluationResult_Allowed;
    return S_OK;
}

static const IDataPackageView3Vtbl view3_vtbl =
{
    view3_QueryInterface,
    view3_AddRef,
    view3_Release,
    /* IInspectable methods */
    view3_GetIids,
    view3_GetRuntimeClassName,
    view3_GetTrustLevel,
    /* IDataPackageView3 methods */
    view3_RequestAccessAsync,
    view3_RequestAccessWithEnterpriseIdAsync,
    view3_UnlockAndAssumeEnterpriseIdentity,
};

DEFINE_IINSPECTABLE( view4, IDataPackageView4, struct package_view, IDataPackageView_iface )

static HRESULT WINAPI view4_SetAcceptedFormatId( IDataPackageView4 *iface, HSTRING format_id )
{
    struct package_view *impl = impl_from_IDataPackageView4( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "iface %p, format_id %s.\n", iface, debugstr_hstring( format_id ) );
    if (FAILED( hr = WindowsDuplicateString( format_id, &copy ) )) return hr;
    EnterCriticalSection( &impl->store->cs );
    WindowsDeleteString( impl->accepted_format );
    impl->accepted_format = copy;
    LeaveCriticalSection( &impl->store->cs );
    return S_OK;
}

static const IDataPackageView4Vtbl view4_vtbl =
{
    view4_QueryInterface,
    view4_AddRef,
    view4_Release,
    /* IInspectable methods */
    view4_GetIids,
    view4_GetRuntimeClassName,
    view4_GetTrustLevel,
    /* IDataPackageView4 methods */
    view4_SetAcceptedFormatId,
};

HRESULT data_package_view_create( struct data_store *store, IDataPackageView **out )
{
    struct package_view *impl;

    if (!out) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDataPackageView_iface.lpVtbl = &view_vtbl;
    impl->IDataPackageView2_iface.lpVtbl = &view2_vtbl;
    impl->IDataPackageView3_iface.lpVtbl = &view3_vtbl;
    impl->IDataPackageView4_iface.lpVtbl = &view4_vtbl;
    impl->ref = 1;
    impl->store = store;
    data_store_addref( store );
    *out = &impl->IDataPackageView_iface;
    return S_OK;
}

/* ---- the activation factory ---- */

struct package_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct package_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct package_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
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
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IActivationFactory;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.DataPackage", name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    IDataPackage *package;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );
    if (!instance) return E_POINTER;
    if (SUCCEEDED( hr = package_create( &package ) )) *instance = (IInspectable *)package;
    return hr;
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

static struct package_factory data_package_statics = { {&factory_vtbl}, 1 };
IActivationFactory *data_package_factory = &data_package_statics.IActivationFactory_iface;
