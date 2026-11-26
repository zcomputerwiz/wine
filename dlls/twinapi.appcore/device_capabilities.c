/* WinRT Windows.Devices.Input.KeyboardCapabilities, MouseCapabilities and TouchCapabilities Implementation
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

/* The capabilities are read from the system metrics when asked, as the classic window system reports them. */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

#ifndef SM_MOUSEHORIZONTALWHEELPRESENT
#define SM_MOUSEHORIZONTALWHEELPRESENT 91
#endif
#ifndef NID_INTEGRATED_TOUCH
#define NID_INTEGRATED_TOUCH 0x01
#define NID_EXTERNAL_TOUCH   0x02
#endif

/* ---- KeyboardCapabilities ---- */

DEFINE_SIMPLE_IINSPECTABLE( keyboard, IKeyboardCapabilities )

static HRESULT WINAPI keyboard_get_KeyboardPresent( IKeyboardCapabilities *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = GetKeyboardType( 0 ) != 0;
    return S_OK;
}

static const IKeyboardCapabilitiesVtbl keyboard_vtbl =
{
    keyboard_QueryInterface,
    keyboard_AddRef,
    keyboard_Release,
    /* IInspectable methods */
    keyboard_GetIids,
    keyboard_GetRuntimeClassName,
    keyboard_GetTrustLevel,
    /* IKeyboardCapabilities methods */
    keyboard_get_KeyboardPresent,
};

static HRESULT keyboard_activate( IInspectable **instance )
{
    return simple_object_create( sizeof(struct simple_object), &keyboard_vtbl, &IID_IKeyboardCapabilities,
                                 L"Windows.Devices.Input.KeyboardCapabilities", NULL, (struct simple_object **)instance );
}

static struct static_factory keyboard_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.Devices.Input.KeyboardCapabilities", keyboard_activate, 0,
};

IActivationFactory *keyboard_capabilities_factory = &keyboard_factory.IActivationFactory_iface;

/* ---- MouseCapabilities ---- */

DEFINE_SIMPLE_IINSPECTABLE( mouse, IMouseCapabilities )

static HRESULT WINAPI mouse_get_MousePresent( IMouseCapabilities *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = GetSystemMetrics( SM_MOUSEPRESENT ) != 0;
    return S_OK;
}

static HRESULT WINAPI mouse_get_VerticalWheelPresent( IMouseCapabilities *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = GetSystemMetrics( SM_MOUSEWHEELPRESENT ) != 0;
    return S_OK;
}

static HRESULT WINAPI mouse_get_HorizontalWheelPresent( IMouseCapabilities *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = GetSystemMetrics( SM_MOUSEHORIZONTALWHEELPRESENT ) != 0;
    return S_OK;
}

static HRESULT WINAPI mouse_get_SwapButtons( IMouseCapabilities *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = GetSystemMetrics( SM_SWAPBUTTON ) != 0;
    return S_OK;
}

static HRESULT WINAPI mouse_get_NumberOfButtons( IMouseCapabilities *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = GetSystemMetrics( SM_CMOUSEBUTTONS );
    return S_OK;
}

static const IMouseCapabilitiesVtbl mouse_vtbl =
{
    mouse_QueryInterface,
    mouse_AddRef,
    mouse_Release,
    /* IInspectable methods */
    mouse_GetIids,
    mouse_GetRuntimeClassName,
    mouse_GetTrustLevel,
    /* IMouseCapabilities methods */
    mouse_get_MousePresent,
    mouse_get_VerticalWheelPresent,
    mouse_get_HorizontalWheelPresent,
    mouse_get_SwapButtons,
    mouse_get_NumberOfButtons,
};

static HRESULT mouse_activate( IInspectable **instance )
{
    return simple_object_create( sizeof(struct simple_object), &mouse_vtbl, &IID_IMouseCapabilities,
                                 L"Windows.Devices.Input.MouseCapabilities", NULL, (struct simple_object **)instance );
}

static struct static_factory mouse_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.Devices.Input.MouseCapabilities", mouse_activate, 0,
};

IActivationFactory *mouse_capabilities_factory = &mouse_factory.IActivationFactory_iface;

/* ---- TouchCapabilities ---- */

DEFINE_SIMPLE_IINSPECTABLE( touch, ITouchCapabilities )

static HRESULT WINAPI touch_get_TouchPresent( ITouchCapabilities *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = (GetSystemMetrics( SM_DIGITIZER ) & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH)) != 0;
    return S_OK;
}

static HRESULT WINAPI touch_get_Contacts( ITouchCapabilities *iface, UINT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = GetSystemMetrics( SM_MAXIMUMTOUCHES );
    return S_OK;
}

static const ITouchCapabilitiesVtbl touch_vtbl =
{
    touch_QueryInterface,
    touch_AddRef,
    touch_Release,
    /* IInspectable methods */
    touch_GetIids,
    touch_GetRuntimeClassName,
    touch_GetTrustLevel,
    /* ITouchCapabilities methods */
    touch_get_TouchPresent,
    touch_get_Contacts,
};

static HRESULT touch_activate( IInspectable **instance )
{
    return simple_object_create( sizeof(struct simple_object), &touch_vtbl, &IID_ITouchCapabilities,
                                 L"Windows.Devices.Input.TouchCapabilities", NULL, (struct simple_object **)instance );
}

static struct static_factory touch_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.Devices.Input.TouchCapabilities", touch_activate, 0,
};

IActivationFactory *touch_capabilities_factory = &touch_factory.IActivationFactory_iface;
