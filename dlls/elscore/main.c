/*
 * Extended Linguistic Services (elscore.dll)
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

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#define ELSCOREAPI
#include "elscore.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(elscore);

/* No linguistic service (language detection, script detection, transliteration) is available: the list of services is empty,
 * which is a valid answer, and a caller that needs a service finds none. Programs such as Windows' Notepad import the
 * library and ask once for the services. */

/***********************************************************************
 *      MappingGetServices (ELSCORE.@)
 */
HRESULT WINAPI MappingGetServices( MAPPING_ENUM_OPTIONS *options, MAPPING_SERVICE_INFO **services, DWORD *count )
{
    TRACE( "options %p, services %p, count %p.\n", options, services, count );

    if (!services || !count) return E_INVALIDARG;
    if (options && options->Size < sizeof(*options)) return E_INVALIDARG;

    *services = NULL;
    *count = 0;
    return S_OK;
}

/***********************************************************************
 *      MappingFreeServices (ELSCORE.@)
 */
HRESULT WINAPI MappingFreeServices( MAPPING_SERVICE_INFO *services )
{
    TRACE( "services %p.\n", services );
    return S_OK;
}

/***********************************************************************
 *      MappingRecognizeText (ELSCORE.@)
 */
HRESULT WINAPI MappingRecognizeText( MAPPING_SERVICE_INFO *service, const WCHAR *text, DWORD length, DWORD index,
                                     MAPPING_OPTIONS *options, MAPPING_PROPERTY_BAG *bag )
{
    FIXME( "service %p, text %s, length %lu, index %lu, options %p, bag %p: no service is available.\n",
           service, debugstr_wn( text, length ), length, index, options, bag );
    return E_NOTIMPL;
}

/***********************************************************************
 *      MappingDoAction (ELSCORE.@)
 */
HRESULT WINAPI MappingDoAction( MAPPING_PROPERTY_BAG *bag, DWORD index, const WCHAR *action )
{
    FIXME( "bag %p, index %lu, action %s: no service is available.\n", bag, index, debugstr_w( action ) );
    return E_NOTIMPL;
}

/***********************************************************************
 *      MappingFreePropertyBag (ELSCORE.@)
 */
HRESULT WINAPI MappingFreePropertyBag( MAPPING_PROPERTY_BAG *bag )
{
    TRACE( "bag %p.\n", bag );
    return S_OK;
}
