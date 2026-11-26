/* Windows.ApplicationModel.DataTransfer.HtmlFormatHelper
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

struct statics
{
    IActivationFactory IActivationFactory_iface;
    IHtmlFormatHelperStatics IHtmlFormatHelperStatics_iface;
    LONG ref;
};

static inline struct statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IHtmlFormatHelperStatics ))
        *out = &impl->IHtmlFormatHelperStatics_iface;
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

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IHtmlFormatHelperStatics;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return create_name( L"Windows.ApplicationModel.DataTransfer.HtmlFormatHelper", name );
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

DEFINE_IINSPECTABLE( statics, IHtmlFormatHelperStatics, struct statics, IActivationFactory_iface )

/* the value of a header field ("Name:digits") of a CF_HTML text, or -1 */
static int header_value( const char *text, const char *name )
{
    size_t len = strlen( name );
    const char *p = text;

    while (*p)
    {
        if (!strncmp( p, name, len ) && p[len] == ':') return atoi( p + len + 1 );
        if (!(p = strchr( p, '\n' ))) break;
        p++;
    }
    return -1;
}

static HRESULT WINAPI statics_GetStaticFragment( IHtmlFormatHelperStatics *iface, HSTRING html_format, HSTRING *html_fragment )
{
    static const char start_marker[] = "<!--StartFragment-->", end_marker[] = "<!--EndFragment-->";
    UINT32 wlen;
    const WCHAR *wide = WindowsGetStringRawBuffer( html_format, &wlen );
    char *utf8, *start, *end;
    int len, from, to;
    WCHAR *out;
    HRESULT hr;

    TRACE( "iface %p, html_format %s, html_fragment %p.\n", iface, debugstr_hstring( html_format ), html_fragment );

    if (!wlen) return E_INVALIDARG;
    len = WideCharToMultiByte( CP_UTF8, 0, wide, wlen, NULL, 0, NULL, NULL );
    if (!(utf8 = malloc( len + 1 ))) return E_OUTOFMEMORY;
    WideCharToMultiByte( CP_UTF8, 0, wide, wlen, utf8, len, NULL, NULL );
    utf8[len] = 0;

    /* the header offsets are in bytes of the UTF-8 text; the comment markers are the fallback */
    from = header_value( utf8, "StartFragment" );
    to = header_value( utf8, "EndFragment" );
    if (from < 0 || to < from || to > len)
    {
        from = to = -1;
        if ((start = strstr( utf8, start_marker )) && (end = strstr( start, end_marker )))
        {
            from = (start - utf8) + sizeof(start_marker) - 1;
            to = end - utf8;
        }
    }
    if (from < 0)
    {
        free( utf8 );
        return E_INVALIDARG;
    }

    len = MultiByteToWideChar( CP_UTF8, 0, utf8 + from, to - from, NULL, 0 );
    if (!(out = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        free( utf8 );
        return E_OUTOFMEMORY;
    }
    MultiByteToWideChar( CP_UTF8, 0, utf8 + from, to - from, out, len );
    hr = WindowsCreateString( out, len, html_fragment );
    free( out );
    free( utf8 );
    return hr;
}

static HRESULT WINAPI statics_CreateHtmlFormat( IHtmlFormatHelperStatics *iface, HSTRING html_fragment, HSTRING *html_format )
{
    static const char header_fmt[] = "Version:1.0\r\nStartHTML:%010d\r\nEndHTML:%010d\r\n"
                                     "StartFragment:%010d\r\nEndFragment:%010d\r\n";
    static const char prefix[] = "<html>\r\n<body>\r\n<!--StartFragment-->";
    static const char suffix[] = "<!--EndFragment-->\r\n</body>\r\n</html>";
    UINT32 wlen;
    const WCHAR *wide = WindowsGetStringRawBuffer( html_fragment, &wlen );
    char *text, header[128];
    int flen, header_len, start_html, start_fragment, end_fragment, end_html, len;
    WCHAR *out;
    HRESULT hr;

    TRACE( "iface %p, html_fragment %s, html_format %p.\n", iface, debugstr_hstring( html_fragment ), html_format );

    flen = wlen ? WideCharToMultiByte( CP_UTF8, 0, wide, wlen, NULL, 0, NULL, NULL ) : 0;
    header_len = sprintf( header, header_fmt, 0, 0, 0, 0 );
    start_html = header_len;
    start_fragment = start_html + sizeof(prefix) - 1;
    end_fragment = start_fragment + flen;
    end_html = end_fragment + sizeof(suffix) - 1;

    if (!(text = malloc( end_html + 1 ))) return E_OUTOFMEMORY;
    sprintf( text, header_fmt, start_html, end_html, start_fragment, end_fragment );
    text[header_len] = 0;
    memcpy( text + start_html, prefix, sizeof(prefix) - 1 );
    if (flen) WideCharToMultiByte( CP_UTF8, 0, wide, wlen, text + start_fragment, flen, NULL, NULL );
    memcpy( text + end_fragment, suffix, sizeof(suffix) );

    len = MultiByteToWideChar( CP_UTF8, 0, text, end_html, NULL, 0 );
    if (!(out = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        free( text );
        return E_OUTOFMEMORY;
    }
    MultiByteToWideChar( CP_UTF8, 0, text, end_html, out, len );
    hr = WindowsCreateString( out, len, html_format );
    free( out );
    free( text );
    return hr;
}

static const IHtmlFormatHelperStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IHtmlFormatHelperStatics methods */
    statics_GetStaticFragment,
    statics_CreateHtmlFormat,
};

static struct statics html_format_helper = { {&factory_vtbl}, {&statics_vtbl}, 1 };
IActivationFactory *html_format_helper_factory = &html_format_helper.IActivationFactory_iface;
