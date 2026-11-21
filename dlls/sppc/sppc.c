/*
 *
 * Copyright 2008 Alistair Leslie-Hughes
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
#include <stdlib.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "slpublic.h"
#include "slerror.h"

WINE_DEFAULT_DEBUG_CHANNEL(slc);

HRESULT WINAPI SLGetLicensingStatusInformation(HSLC handle, const SLID *app, const SLID *product,
                                               LPCWSTR name, UINT *count, SL_LICENSING_STATUS **status)
{
    FIXME("(%p %p %p %s %p %p) stub\n", handle, app, product, debugstr_w(name), count, status );

    return SL_E_RIGHT_NOT_CONSUMED;
}

HRESULT WINAPI SLOpen(HSLC *handle)
{
    FIXME("(%p) stub\n", handle );

    if (!handle)
        return E_INVALIDARG;

    *handle = (HSLC)0xdeadbeef;

    return S_OK;
}

HRESULT WINAPI SLClose(HSLC handle)
{
    FIXME("(%p) stub\n", handle );

    return S_OK;
}

/* Nothing fires licensing events on Wine, so a registration is only remembered: Windows refuses a second
 * unregistration of the same (event, application, handle). */
struct event_registration
{
    struct list entry;
    const WCHAR *event_id;
    SLID app;
    HANDLE event;
};

static struct list event_registrations = LIST_INIT( event_registrations );
static CRITICAL_SECTION event_cs;
static CRITICAL_SECTION_DEBUG event_cs_debug =
{
    0, 0, &event_cs,
    { &event_cs_debug.ProcessLocksList, &event_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": event_cs") }
};
static CRITICAL_SECTION event_cs = { &event_cs_debug, -1, 0, 0, 0, 0 };

static HRESULT check_event_args( PCWSTR event_id, const SLID *app, HANDLE event, const WCHAR **known_id )
{
    static const WCHAR *const known_ids[] =
    {
        SL_EVENT_LICENSING_STATE_CHANGED,
        SL_EVENT_POLICY_CHANGED,
        SL_EVENT_USER_NOTIFICATION,
    };
    unsigned int i;

    if (!event_id || !app || !event) return E_INVALIDARG;
    for (i = 0; i < ARRAY_SIZE(known_ids); i++)
    {
        if (wcscmp( event_id, known_ids[i] )) continue;
        *known_id = known_ids[i];
        return S_OK;
    }
    return SL_E_INVALID_EVENT_ID;
}

static struct event_registration *find_registration( const WCHAR *event_id, const SLID *app, HANDLE event )
{
    struct event_registration *reg;

    LIST_FOR_EACH_ENTRY( reg, &event_registrations, struct event_registration, entry )
        if (reg->event_id == event_id && IsEqualGUID( &reg->app, app ) && reg->event == event) return reg;
    return NULL;
}

HRESULT WINAPI SLRegisterEvent(HSLC handle, PCWSTR event_id, const SLID *app, HANDLE event)
{
    struct event_registration *reg;
    const WCHAR *id;
    DWORD flags;
    HRESULT hr;

    TRACE("(%p %s %s %p)\n", handle, debugstr_w(event_id), debugstr_guid(app), event);

    if (FAILED(hr = check_event_args( event_id, app, event, &id ))) return hr;
    if (!GetHandleInformation( event, &flags )) return HRESULT_FROM_WIN32( GetLastError() );

    EnterCriticalSection( &event_cs );
    if (!find_registration( id, app, event ))
    {
        if (!(reg = malloc( sizeof(*reg) ))) hr = E_OUTOFMEMORY;
        else
        {
            reg->event_id = id;
            reg->app = *app;
            reg->event = event;
            list_add_tail( &event_registrations, &reg->entry );
        }
    }
    LeaveCriticalSection( &event_cs );
    return hr;
}

HRESULT WINAPI SLUnregisterEvent(HSLC handle, PCWSTR event_id, const SLID *app, HANDLE event)
{
    struct event_registration *reg;
    const WCHAR *id;
    HRESULT hr;

    TRACE("(%p %s %s %p)\n", handle, debugstr_w(event_id), debugstr_guid(app), event);

    if (FAILED(hr = check_event_args( event_id, app, event, &id ))) return hr;

    EnterCriticalSection( &event_cs );
    if ((reg = find_registration( id, app, event )))
    {
        list_remove( &reg->entry );
        free( reg );
    }
    else hr = SL_E_EVENT_NOT_REGISTERED;
    LeaveCriticalSection( &event_cs );
    return hr;
}

HRESULT WINAPI SLPersistApplicationPolicies(const SLID *app, const SLID *product, DWORD flags)
{
    FIXME("(%s,%s,%lx) stub\n", wine_dbgstr_guid(app), wine_dbgstr_guid(product), flags);

    if (!app)
        return E_INVALIDARG;

    return S_OK;
}
