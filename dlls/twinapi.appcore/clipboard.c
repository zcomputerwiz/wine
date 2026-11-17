/* Windows.ApplicationModel.DataTransfer.Clipboard, on the Win32 clipboard (text and HTML)
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
#include "winuser.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

enum clipboard_event { EVENT_CONTENT_CHANGED, EVENT_HISTORY_CHANGED, EVENT_ROAMING_CHANGED, EVENT_HISTORY_ENABLED_CHANGED,
                       EVENT_COUNT };

static CRITICAL_SECTION clipboard_cs;
static CRITICAL_SECTION_DEBUG clipboard_cs_debug =
{
    0, 0, &clipboard_cs,
    { &clipboard_cs_debug.ProcessLocksList, &clipboard_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": clipboard_cs") }
};
static CRITICAL_SECTION clipboard_cs = { &clipboard_cs_debug, -1, 0, 0, 0, 0 };

/* the handlers are kept; the clipboard is not watched, so they are never called */
static struct list event_lists[EVENT_COUNT] =
{
    LIST_INIT( event_lists[0] ), LIST_INIT( event_lists[1] ), LIST_INIT( event_lists[2] ), LIST_INIT( event_lists[3] ),
};

/* only one thread at a time opens the clipboard */
static CRITICAL_SECTION access_cs;
static CRITICAL_SECTION_DEBUG access_cs_debug =
{
    0, 0, &access_cs,
    { &access_cs_debug.ProcessLocksList, &access_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": access_cs") }
};
static CRITICAL_SECTION access_cs = { &access_cs_debug, -1, 0, 0, 0, 0 };

/* the clipboard may be held by another program for a moment */
static BOOL open_clipboard(void)
{
    int i;

    for (i = 0; i < 20; i++)
    {
        if (OpenClipboard( NULL )) return TRUE;
        Sleep( 10 );
    }
    return FALSE;
}

static UINT html_format(void)
{
    static UINT format;

    if (!format) format = RegisterClipboardFormatW( DATA_FORMAT_HTML );
    return format;
}

static HRESULT view_get_string( IDataPackageView *view, const WCHAR *format, HSTRING *value )
{
    IAsyncOperation_HSTRING *operation;
    HSTRING id;
    boolean found;
    HRESULT hr;

    *value = NULL;
    if (FAILED( hr = WindowsCreateString( format, wcslen( format ), &id ) )) return hr;
    if (SUCCEEDED( hr = IDataPackageView_Contains( view, id, &found ) ) && !found) hr = S_FALSE;
    if (hr == S_OK && SUCCEEDED( hr = IDataPackageView_GetCustomTextAsync( view, id, &operation ) ))
    {
        hr = IAsyncOperation_HSTRING_GetResults( operation, value );
        IAsyncOperation_HSTRING_Release( operation );
    }
    WindowsDeleteString( id );
    return hr;
}

/* the clipboard becomes the text and the HTML of the package */
static HRESULT clipboard_set( IDataPackageView *view )
{
    HSTRING text = NULL, html = NULL;
    HGLOBAL text_mem = NULL, html_mem = NULL;
    HRESULT hr = S_OK;

    if (view)
    {
        if (FAILED( hr = view_get_string( view, DATA_FORMAT_TEXT, &text ) )) goto done;
        if (FAILED( hr = view_get_string( view, DATA_FORMAT_HTML, &html ) )) goto done;
        hr = S_OK;
    }

    if (text)
    {
        UINT32 len;
        const WCHAR *str = WindowsGetStringRawBuffer( text, &len );
        WCHAR *ptr;

        if (!(text_mem = GlobalAlloc( GMEM_MOVEABLE, (len + 1) * sizeof(WCHAR) )) || !(ptr = GlobalLock( text_mem )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        memcpy( ptr, str, len * sizeof(WCHAR) );
        ptr[len] = 0;
        GlobalUnlock( text_mem );
    }
    if (html)
    {
        UINT32 len;
        const WCHAR *str = WindowsGetStringRawBuffer( html, &len );
        int size = WideCharToMultiByte( CP_UTF8, 0, str, len, NULL, 0, NULL, NULL );
        char *ptr;

        if (!(html_mem = GlobalAlloc( GMEM_MOVEABLE, size + 1 )) || !(ptr = GlobalLock( html_mem )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        WideCharToMultiByte( CP_UTF8, 0, str, len, ptr, size, NULL, NULL );
        ptr[size] = 0;
        GlobalUnlock( html_mem );
    }

    EnterCriticalSection( &access_cs );
    if (!open_clipboard()) hr = HRESULT_FROM_WIN32( GetLastError() );
    else
    {
        EmptyClipboard();
        if (text_mem && SetClipboardData( CF_UNICODETEXT, text_mem )) text_mem = NULL;
        if (html_mem && SetClipboardData( html_format(), html_mem )) html_mem = NULL;
        CloseClipboard();
    }
    LeaveCriticalSection( &access_cs );

done:
    if (text_mem) GlobalFree( text_mem );
    if (html_mem) GlobalFree( html_mem );
    WindowsDeleteString( text );
    WindowsDeleteString( html );
    return hr;
}

/* a snapshot of the clipboard, as a view */
static HRESULT clipboard_get( IDataPackageView **view )
{
    struct data_store *store;
    HRESULT hr;
    HANDLE data;

    if (FAILED( hr = data_store_create( &store ) )) return hr;

    EnterCriticalSection( &access_cs );
    if (open_clipboard())
    {
        if ((data = GetClipboardData( CF_UNICODETEXT )))
        {
            const WCHAR *ptr = GlobalLock( data );
            SIZE_T max_len = GlobalSize( data ) / sizeof(WCHAR), len = 0;

            if (ptr)
            {
                while (len < max_len && ptr[len]) len++;
                if (len == max_len) FIXME( "unterminated clipboard text.\n" );
                else data_store_set_string( store, DATA_FORMAT_TEXT, ptr );
                GlobalUnlock( data );
            }
        }
        if ((data = GetClipboardData( html_format() )))
        {
            const char *ptr = GlobalLock( data );
            SIZE_T size = GlobalSize( data ), len = 0;

            if (ptr)
            {
                WCHAR *wide;
                int wlen;

                while (len < size && ptr[len]) len++;
                wlen = MultiByteToWideChar( CP_UTF8, 0, ptr, len, NULL, 0 );
                if ((wide = malloc( (wlen + 1) * sizeof(WCHAR) )))
                {
                    MultiByteToWideChar( CP_UTF8, 0, ptr, len, wide, wlen );
                    wide[wlen] = 0;
                    data_store_set_string( store, DATA_FORMAT_HTML, wide );
                    free( wide );
                }
                GlobalUnlock( data );
            }
        }
        CloseClipboard();
    }
    else WARN( "the clipboard is not available, error %lu.\n", GetLastError() );
    LeaveCriticalSection( &access_cs );

    hr = data_package_view_create( store, view );
    data_store_release( store );
    return hr;
}

static HRESULT clipboard_clear(void)
{
    HRESULT hr = S_OK;

    EnterCriticalSection( &access_cs );
    if (!open_clipboard()) hr = HRESULT_FROM_WIN32( GetLastError() );
    else
    {
        EmptyClipboard();
        CloseClipboard();
    }
    LeaveCriticalSection( &access_cs );
    return hr;
}

struct clipboard_statics
{
    IActivationFactory IActivationFactory_iface;
    IClipboardStatics IClipboardStatics_iface;
    IClipboardStatics2 IClipboardStatics2_iface;
    LONG ref;
};

static inline struct clipboard_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct clipboard_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct clipboard_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IClipboardStatics ))
        *out = &impl->IClipboardStatics_iface;
    else if (IsEqualGUID( iid, &IID_IClipboardStatics2 ))
        *out = &impl->IClipboardStatics2_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
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
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IClipboardStatics;
    list[1] = IID_IClipboardStatics2;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.Clipboard", name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
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

DEFINE_IINSPECTABLE( statics, IClipboardStatics, struct clipboard_statics, IActivationFactory_iface )

static HRESULT WINAPI statics_GetContent( IClipboardStatics *iface, IDataPackageView **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return clipboard_get( result );
}

static HRESULT WINAPI statics_SetContent( IClipboardStatics *iface, IDataPackage *content )
{
    IDataPackageView *view = NULL;
    HRESULT hr;

    TRACE( "iface %p, content %p.\n", iface, content );

    if (content && FAILED( hr = IDataPackage_GetView( content, &view ) )) return hr;
    hr = clipboard_set( view );
    if (view) IDataPackageView_Release( view );
    return hr;
}

static HRESULT WINAPI statics_Flush( IClipboardStatics *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static HRESULT WINAPI statics_Clear( IClipboardStatics *iface )
{
    TRACE( "iface %p.\n", iface );
    return clipboard_clear();
}

static HRESULT WINAPI statics_add_ContentChanged( IClipboardStatics *iface, IEventHandler_IInspectable *handler,
                                                  EventRegistrationToken *token )
{
    return handler_list_add( &event_lists[EVENT_CONTENT_CHANGED], &clipboard_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics_remove_ContentChanged( IClipboardStatics *iface, EventRegistrationToken token )
{
    return handler_list_remove( &event_lists[EVENT_CONTENT_CHANGED], &clipboard_cs, token );
}

static const IClipboardStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IClipboardStatics methods */
    statics_GetContent,
    statics_SetContent,
    statics_Flush,
    statics_Clear,
    statics_add_ContentChanged,
    statics_remove_ContentChanged,
};

DEFINE_IINSPECTABLE( statics2, IClipboardStatics2, struct clipboard_statics, IActivationFactory_iface )

static HRESULT WINAPI statics2_GetHistoryItemsAsync( IClipboardStatics2 *iface,
                                                     IAsyncOperation_ClipboardHistoryItemsResult **operation )
{
    FIXME( "iface %p, operation %p: the history is not available.\n", iface, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics2_ClearHistory( IClipboardStatics2 *iface, boolean *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics2_DeleteItemFromHistory( IClipboardStatics2 *iface, IClipboardHistoryItem *item,
                                                      boolean *result )
{
    TRACE( "iface %p, item %p, result %p.\n", iface, item, result );
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics2_SetHistoryItemAsContent( IClipboardStatics2 *iface, IClipboardHistoryItem *item,
                                                        SetHistoryItemAsContentStatus *result )
{
    TRACE( "iface %p, item %p, result %p.\n", iface, item, result );
    *result = SetHistoryItemAsContentStatus_AccessDenied;
    return S_OK;
}

static HRESULT WINAPI statics2_IsHistoryEnabled( IClipboardStatics2 *iface, boolean *result )
{
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics2_IsRoamingEnabled( IClipboardStatics2 *iface, boolean *result )
{
    *result = FALSE;
    return S_OK;
}

static HRESULT WINAPI statics2_SetContentWithOptions( IClipboardStatics2 *iface, IDataPackage *content,
                                                      IClipboardContentOptions *options, boolean *result )
{
    HRESULT hr;

    TRACE( "iface %p, content %p, options %p, result %p.\n", iface, content, options, result );
    hr = IClipboardStatics_SetContent( &impl_from_IClipboardStatics2( iface )->IClipboardStatics_iface, content );
    *result = SUCCEEDED( hr );
    return hr;
}

static HRESULT WINAPI statics2_add_HistoryChanged( IClipboardStatics2 *iface,
                                                   IEventHandler_ClipboardHistoryChangedEventArgs *handler,
                                                   EventRegistrationToken *token )
{
    return handler_list_add( &event_lists[EVENT_HISTORY_CHANGED], &clipboard_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics2_remove_HistoryChanged( IClipboardStatics2 *iface, EventRegistrationToken token )
{
    return handler_list_remove( &event_lists[EVENT_HISTORY_CHANGED], &clipboard_cs, token );
}

static HRESULT WINAPI statics2_add_RoamingEnabledChanged( IClipboardStatics2 *iface, IEventHandler_IInspectable *handler,
                                                          EventRegistrationToken *token )
{
    return handler_list_add( &event_lists[EVENT_ROAMING_CHANGED], &clipboard_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics2_remove_RoamingEnabledChanged( IClipboardStatics2 *iface, EventRegistrationToken token )
{
    return handler_list_remove( &event_lists[EVENT_ROAMING_CHANGED], &clipboard_cs, token );
}

static HRESULT WINAPI statics2_add_HistoryEnabledChanged( IClipboardStatics2 *iface, IEventHandler_IInspectable *handler,
                                                          EventRegistrationToken *token )
{
    return handler_list_add( &event_lists[EVENT_HISTORY_ENABLED_CHANGED], &clipboard_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics2_remove_HistoryEnabledChanged( IClipboardStatics2 *iface, EventRegistrationToken token )
{
    return handler_list_remove( &event_lists[EVENT_HISTORY_ENABLED_CHANGED], &clipboard_cs, token );
}

static const IClipboardStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IClipboardStatics2 methods */
    statics2_GetHistoryItemsAsync,
    statics2_ClearHistory,
    statics2_DeleteItemFromHistory,
    statics2_SetHistoryItemAsContent,
    statics2_IsHistoryEnabled,
    statics2_IsRoamingEnabled,
    statics2_SetContentWithOptions,
    statics2_add_HistoryChanged,
    statics2_remove_HistoryChanged,
    statics2_add_RoamingEnabledChanged,
    statics2_remove_RoamingEnabledChanged,
    statics2_add_HistoryEnabledChanged,
    statics2_remove_HistoryEnabledChanged,
};

static struct clipboard_statics clipboard_statics = { {&factory_vtbl}, {&statics_vtbl}, {&statics2_vtbl}, 1 };
IActivationFactory *clipboard_factory = &clipboard_statics.IActivationFactory_iface;
