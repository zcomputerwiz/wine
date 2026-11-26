/* WinRT Windows.System.Display.DisplayRequest Implementation
 *
 * Copyright (C) 2026 WineExplorer contributors
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
 * An application that shows something the user watches (a video, a presentation) asks that the display stays on. Each
 * request counts; the display may sleep again when every request is released. Releasing a request that was not made is
 * an error, as on Windows (E_ILLEGAL_METHOD_CALL). A request that is still active when the object goes away is
 * released with it.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

struct display_request
{
    struct simple_object base;
    LONG active;
};

/* the requests of the process; the first one keeps the display on, the last release lets it sleep */
static LONG process_requests;

static void requests_add( LONG delta )
{
    LONG count = InterlockedExchangeAdd( &process_requests, delta ) + delta;

    if (delta > 0 && count == delta) SetThreadExecutionState( ES_CONTINUOUS | ES_DISPLAY_REQUIRED );
    else if (delta < 0 && count <= 0) SetThreadExecutionState( ES_CONTINUOUS );
}

DEFINE_SIMPLE_IINSPECTABLE( request, IDisplayRequest )

static HRESULT WINAPI request_RequestActive( IDisplayRequest *iface )
{
    struct display_request *impl = (struct display_request *)iface;

    TRACE( "iface %p.\n", iface );
    InterlockedIncrement( &impl->active );
    requests_add( 1 );
    return S_OK;
}

static HRESULT WINAPI request_RequestRelease( IDisplayRequest *iface )
{
    struct display_request *impl = (struct display_request *)iface;
    LONG active;

    TRACE( "iface %p.\n", iface );
    do
    {
        if ((active = impl->active) <= 0) return E_ILLEGAL_METHOD_CALL;
    } while (InterlockedCompareExchange( &impl->active, active - 1, active ) != active);
    requests_add( -1 );
    return S_OK;
}

static const IDisplayRequestVtbl request_vtbl =
{
    request_QueryInterface,
    request_AddRef,
    request_Release,
    /* IInspectable methods */
    request_GetIids,
    request_GetRuntimeClassName,
    request_GetTrustLevel,
    /* IDisplayRequest methods */
    request_RequestActive,
    request_RequestRelease,
};

static void request_destroy( struct simple_object *object )
{
    LONG active = ((struct display_request *)object)->active;

    if (active > 0) requests_add( -active );
}

static HRESULT request_activate( IInspectable **instance )
{
    return simple_object_create( sizeof(struct display_request), &request_vtbl, &IID_IDisplayRequest,
                                 L"Windows.System.Display.DisplayRequest", request_destroy,
                                 (struct simple_object **)instance );
}

static struct static_factory request_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.System.Display.DisplayRequest", request_activate, 0,
};

IActivationFactory *display_request_factory = &request_factory.IActivationFactory_iface;
