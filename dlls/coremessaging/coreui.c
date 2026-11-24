/* CoreUICreate of CoreMessaging.dll
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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "unknwn.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(messaging);

/*
 * CoreUICreate(void **object), as seen on Windows 11 (tests/uwp/alpc/coreui-probe.c): it returns S_OK with the same process-wide
 * object each time, and E_POINTER for a null argument. The Windows App Runtime (FrameworkUdk, CoreWindowIntegration) asks the
 * object for IUnknown, which gives another pointer than the object itself, and keeps it. What the object offers beyond IUnknown is
 * not known; it is the connection of the process to the window system's CoreUI session, which Wine does not have. The object here
 * has IUnknown only and lives as long as the process.
 */

/* the interface {4b2edab9-129b-4733-be15-356853d55ace} of the object: the real one has at least 24 methods; the Windows App
 * Runtime calls the one in slot 8 with a pointer and the address of an interface pointer and asks the result for IUnknown */
struct coreui_session
{
    const void *const *vtbl;
};

struct coreui
{
    IUnknown outer;
    IUnknown inner;
    struct coreui_session session;
};

static const GUID IID_ICoreUISession = {0x4b2edab9, 0x129b, 0x4733, {0xbe, 0x15, 0x35, 0x68, 0x53, 0xd5, 0x5a, 0xce}};

static HRESULT WINAPI object_QueryInterface( IUnknown *iface, REFIID iid, void **out );

static ULONG WINAPI object_AddRef( IUnknown *iface )
{
    return 2;
}

static ULONG WINAPI object_Release( IUnknown *iface )
{
    return 1;
}

static const IUnknownVtbl coreui_vtbl = { object_QueryInterface, object_AddRef, object_Release };

static struct coreui coreui;

/*
 * What slot 8 gives (with a null argument, on Windows 11): an object with IUnknown (another pointer than the object) and
 * {fcd9d50d-0902-4a2d-ac94-b95c3316f6a0} (the object itself), an interface of 4 methods after IUnknown.
 */
static const GUID IID_ICoreUIItem = {0xfcd9d50d, 0x0902, 0x4a2d, {0xac, 0x94, 0xb9, 0x5c, 0x33, 0x16, 0xf6, 0xa0}};

struct coreui_item
{
    const void *const *vtbl;  /* the interface */
    IUnknown inner;
};

static HRESULT WINAPI item_QueryInterface( void *iface, REFIID iid, void **out );

static ULONG WINAPI item_AddRef( void *iface )
{
    return 2;
}

static ULONG WINAPI item_Release( void *iface )
{
    return 1;
}

#define ITEM_STUB(n) \
    static HRESULT WINAPI item_slot##n( void *iface, void *a, void *b, void *c, void *d ) \
    { \
        FIXME( "iface %p, item slot " #n ": %p, %p, %p, %p stub!\n", iface, a, b, c, d ); \
        return E_NOTIMPL; \
    }
ITEM_STUB(3) ITEM_STUB(4) ITEM_STUB(5) ITEM_STUB(6)

static const void *const item_vtbl[] =
{
    item_QueryInterface, item_AddRef, item_Release, item_slot3, item_slot4, item_slot5, item_slot6,
};

/*
 * The interfaces behind the item are not known yet. The Windows App Runtime registers listeners and handlers (back button, window
 * position, unhandled errors, input panes ...) with the CoreUI session through them; until each is known an unknown interface is
 * answered with one object that has 32 methods that all succeed and log what they are given.
 */
struct generic
{
    const void *const *vtbl;
};

static HRESULT WINAPI generic_QueryInterface( void *iface, REFIID iid, void **out );

static ULONG WINAPI generic_AddRef( void *iface )
{
    return 2;
}

static ULONG WINAPI generic_Release( void *iface )
{
    return 1;
}

#define GENERIC_STUB(n) \
    static HRESULT WINAPI generic_slot##n( void *iface, void *a, void *b, void *c, void *d ) \
    { \
        FIXME( "iface %p, generic slot " #n ": %p, %p, %p, %p semi-stub (caller %p)\n", iface, a, b, c, d, \
               __builtin_return_address(0) ); \
        return S_OK; \
    }
GENERIC_STUB(3) GENERIC_STUB(4) GENERIC_STUB(5) GENERIC_STUB(6) GENERIC_STUB(7) GENERIC_STUB(8) GENERIC_STUB(9)
GENERIC_STUB(10) GENERIC_STUB(11) GENERIC_STUB(12) GENERIC_STUB(13) GENERIC_STUB(14) GENERIC_STUB(15) GENERIC_STUB(16)
GENERIC_STUB(17) GENERIC_STUB(18) GENERIC_STUB(19) GENERIC_STUB(20) GENERIC_STUB(21) GENERIC_STUB(22) GENERIC_STUB(23)
GENERIC_STUB(24) GENERIC_STUB(25) GENERIC_STUB(26) GENERIC_STUB(27) GENERIC_STUB(28) GENERIC_STUB(29) GENERIC_STUB(30)
GENERIC_STUB(31)

static const void *const generic_vtbl[] =
{
    generic_QueryInterface, generic_AddRef, generic_Release,
    generic_slot3, generic_slot4, generic_slot5, generic_slot6, generic_slot7, generic_slot8, generic_slot9, generic_slot10,
    generic_slot11, generic_slot12, generic_slot13, generic_slot14, generic_slot15, generic_slot16, generic_slot17,
    generic_slot18, generic_slot19, generic_slot20, generic_slot21, generic_slot22, generic_slot23, generic_slot24,
    generic_slot25, generic_slot26, generic_slot27, generic_slot28, generic_slot29, generic_slot30, generic_slot31,
};

static struct generic generic_object = { generic_vtbl };

static HRESULT WINAPI generic_QueryInterface( void *iface, REFIID iid, void **out )
{
    FIXME( "generic %p, iid %s (caller %p)\n", iface, debugstr_guid( iid ), __builtin_return_address(0) );
    *out = &generic_object;
    return S_OK;
}

static HRESULT WINAPI item_inner_QueryInterface( IUnknown *iface, REFIID iid, void **out );
static const IUnknownVtbl item_inner_vtbl = { item_inner_QueryInterface, object_AddRef, object_Release };

static struct coreui_item coreui_item = { item_vtbl, { &item_inner_vtbl } };

static HRESULT WINAPI item_QueryInterface( void *iface, REFIID iid, void **out )
{
    return item_inner_QueryInterface( &coreui_item.inner, iid, out );
}

static HRESULT WINAPI item_inner_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ))
        *out = &coreui_item.inner;
    else if (IsEqualGUID( iid, &IID_ICoreUIItem ))
        *out = &coreui_item;
    else
        return generic_QueryInterface( &generic_object, iid, out );
    return S_OK;
}

static HRESULT WINAPI session_QueryInterface( void *iface, REFIID iid, void **out )
{
    return object_QueryInterface( &coreui.inner, iid, out );
}

static ULONG WINAPI session_AddRef( void *iface )
{
    return 2;
}

static ULONG WINAPI session_Release( void *iface )
{
    return 1;
}

static HRESULT WINAPI session_slot8( void *iface, void *arg, void **out )
{
    FIXME( "iface %p, arg %p, out %p: semi-stub\n", iface, arg, out );

    if (!out) return E_POINTER;
    *out = &coreui_item;
    return S_OK;
}

#define SESSION_STUB(n) \
    static HRESULT WINAPI session_slot##n( void *iface, void *a, void *b, void *c, void *d ) \
    { \
        FIXME( "iface %p, slot " #n ": %p, %p, %p, %p stub! (caller %p)\n", iface, a, b, c, d, __builtin_return_address(0) ); \
        return E_NOTIMPL; \
    }
/* slot 12 is called with a function of the runtime, a context pointer, a pointer to the object and one more pointer (a handler
 * registration); it is answered with success and nothing is remembered */
static HRESULT WINAPI session_slot12( void *iface, void *a, void *b, void *c, void *d )
{
    FIXME( "iface %p, slot 12: %p, %p, %p, %p semi-stub\n", iface, a, b, c, d );
    return S_OK;
}

/* slot 13 is called the same way as slot 12 (a function of the runtime, a context, a pointer to the object, a pointer) by the
 * newer start-up path the runtime takes when UniversalApiContract is 19 or higher; a failure is fatal there */
static HRESULT WINAPI session_slot13( void *iface, void *a, void *b, void *c, void *d )
{
    FIXME( "iface %p, slot 13: %p, %p, %p, %p semi-stub\n", iface, a, b, c, d );
    return S_OK;
}

/* slot 3 is called (from the wrappers of FrameworkUdk that ask an object for this interface and call one of its methods) with
 * the address of an interface pointer first, and the result is asked for IUnknown: it creates something and gives it back */
static HRESULT WINAPI session_slot3( void *iface, void **out, void *b, void *c )
{
    FIXME( "iface %p, slot 3: %p, %p, %p semi-stub\n", iface, out, b, c );
    if (!out) return E_POINTER;
    *out = &coreui_item;
    return S_OK;
}

SESSION_STUB(4) SESSION_STUB(5) SESSION_STUB(6) SESSION_STUB(7) SESSION_STUB(9) SESSION_STUB(10)
SESSION_STUB(11) SESSION_STUB(14) SESSION_STUB(15) SESSION_STUB(16) SESSION_STUB(17)
SESSION_STUB(18) SESSION_STUB(19) SESSION_STUB(20) SESSION_STUB(21) SESSION_STUB(22) SESSION_STUB(23)

static const void *const session_vtbl[] =
{
    session_QueryInterface, session_AddRef, session_Release,
    session_slot3, session_slot4, session_slot5, session_slot6, session_slot7, session_slot8, session_slot9,
    session_slot10, session_slot11, session_slot12, session_slot13, session_slot14, session_slot15, session_slot16,
    session_slot17, session_slot18, session_slot19, session_slot20, session_slot21, session_slot22, session_slot23,
};

static struct coreui coreui = { { &coreui_vtbl }, { &coreui_vtbl }, { session_vtbl } };

static HRESULT WINAPI object_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ))
        *out = &coreui.inner;
    else if (IsEqualGUID( iid, &IID_ICoreUISession ))
        *out = &coreui.session;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE (caller %p).\n", debugstr_guid( iid ), __builtin_return_address(0) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

HRESULT WINAPI CoreUICreate( void **object )
{
    TRACE( "object %p.\n", object );

    if (!object) return E_POINTER;
    *object = &coreui.outer;
    return S_OK;
}
