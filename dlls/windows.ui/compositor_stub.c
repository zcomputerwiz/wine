/* Logging stubs of Windows.UI.Composition.Compositor and WindowsUdk.Foundation.Metadata.ApiInformation
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
#include "windows.ui.composition.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

/*
 * An experiment, not an implementation: dcompi.dll of the Windows App Runtime (the lifted compositor behind WinUI 3) asks the system
 * for the factory of WindowsUdk.Foundation.Metadata.ApiInformation (queries it for {5e03ab98-c7e0-4449-8518-568d62e9b396} and calls
 * its first method with a BOOLEAN out parameter) and then activates Windows.UI.Composition.Compositor and queries the instance for
 * two interfaces (docs/uwp/WINAPPSDK.md). Until it is known what it calls, every object here answers every interface with one
 * object whose methods all succeed and log what they are given and who called.
 */
struct stub_object
{
    const void *const *vtbl;
    const char *name;
};

static HRESULT WINAPI stub_QueryInterface( void *iface, REFIID iid, void **out )
{
    struct stub_object *obj = iface;

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ))
        FIXME( "%s %p: QueryInterface(%s): itself (caller %p)\n", obj->name, iface, debugstr_guid( iid ), __builtin_return_address(0) );
    else
        FIXME( "%s %p: QueryInterface(%s): itself, as every interface (caller %p)\n", obj->name, iface, debugstr_guid( iid ),
               __builtin_return_address(0) );
    *out = iface;
    return S_OK;
}

static ULONG WINAPI stub_AddRef( void *iface )
{
    return 2;
}

static ULONG WINAPI stub_Release( void *iface )
{
    return 1;
}

static HRESULT WINAPI stub_GetIids( void *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI stub_GetRuntimeClassName( void *iface, HSTRING *name )
{
    struct stub_object *obj = iface;
    FIXME( "%s %p: GetRuntimeClassName (caller %p)\n", obj->name, iface, __builtin_return_address(0) );
    return E_NOTIMPL;
}

static HRESULT WINAPI stub_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT slot_log( void *iface, int slot, void *a, void *b, void *c, void *d, void *caller )
{
    struct stub_object *obj = iface;
    FIXME( "%s %p: slot %d(%p, %p, %p, %p) semi-stub (caller %p)\n", obj->name, iface, slot, a, b, c, d, caller );
    return S_OK;
}

#define SLOT(n) static HRESULT WINAPI slot##n( void *iface, void *a, void *b, void *c, void *d ) { return slot_log( iface, n, a, b, c, d, __builtin_return_address(0) ); }
SLOT(6)
SLOT(7)
SLOT(8)
SLOT(9)
SLOT(10)
SLOT(11)
SLOT(12)
SLOT(13)
SLOT(14)
SLOT(15)
SLOT(16)
SLOT(17)
SLOT(18)
SLOT(19)
SLOT(20)
SLOT(21)
SLOT(22)
SLOT(23)
SLOT(24)
SLOT(25)
SLOT(26)
SLOT(27)
SLOT(28)
SLOT(29)
SLOT(30)
SLOT(31)
SLOT(32)
SLOT(33)
SLOT(34)
SLOT(35)
SLOT(36)
SLOT(37)
SLOT(38)
SLOT(39)
SLOT(40)
SLOT(41)
SLOT(42)
SLOT(43)
SLOT(44)
SLOT(45)
SLOT(46)
SLOT(47)
SLOT(48)
SLOT(49)
SLOT(50)
SLOT(51)
SLOT(52)
SLOT(53)
SLOT(54)
SLOT(55)
SLOT(56)
SLOT(57)
SLOT(58)
SLOT(59)
SLOT(60)
SLOT(61)
SLOT(62)
SLOT(63)
SLOT(64)
SLOT(65)
SLOT(66)
SLOT(67)
SLOT(68)
SLOT(69)
SLOT(70)
SLOT(71)
SLOT(72)
SLOT(73)
SLOT(74)
SLOT(75)
SLOT(76)
SLOT(77)
SLOT(78)
SLOT(79)
SLOT(80)
SLOT(81)
SLOT(82)
SLOT(83)
SLOT(84)
SLOT(85)
SLOT(86)
SLOT(87)
SLOT(88)
SLOT(89)
SLOT(90)
SLOT(91)
SLOT(92)
SLOT(93)
SLOT(94)
SLOT(95)
SLOT(96)
SLOT(97)
SLOT(98)
SLOT(99)
SLOT(100)
SLOT(101)
SLOT(102)
SLOT(103)
SLOT(104)
SLOT(105)
SLOT(106)
SLOT(107)
SLOT(108)
SLOT(109)
SLOT(110)
SLOT(111)
SLOT(112)
SLOT(113)
SLOT(114)
SLOT(115)
SLOT(116)
SLOT(117)
SLOT(118)
SLOT(119)
SLOT(120)
SLOT(121)
SLOT(122)
SLOT(123)
SLOT(124)
SLOT(125)
SLOT(126)
SLOT(127)

static const void *const stub_vtbl[] =
{
    stub_QueryInterface, stub_AddRef, stub_Release, stub_GetIids, stub_GetRuntimeClassName, stub_GetTrustLevel,
    slot6,
    slot7,
    slot8,
    slot9,
    slot10,
    slot11,
    slot12,
    slot13,
    slot14,
    slot15,
    slot16,
    slot17,
    slot18,
    slot19,
    slot20,
    slot21,
    slot22,
    slot23,
    slot24,
    slot25,
    slot26,
    slot27,
    slot28,
    slot29,
    slot30,
    slot31,
    slot32,
    slot33,
    slot34,
    slot35,
    slot36,
    slot37,
    slot38,
    slot39,
    slot40,
    slot41,
    slot42,
    slot43,
    slot44,
    slot45,
    slot46,
    slot47,
    slot48,
    slot49,
    slot50,
    slot51,
    slot52,
    slot53,
    slot54,
    slot55,
    slot56,
    slot57,
    slot58,
    slot59,
    slot60,
    slot61,
    slot62,
    slot63,
    slot64,
    slot65,
    slot66,
    slot67,
    slot68,
    slot69,
    slot70,
    slot71,
    slot72,
    slot73,
    slot74,
    slot75,
    slot76,
    slot77,
    slot78,
    slot79,
    slot80,
    slot81,
    slot82,
    slot83,
    slot84,
    slot85,
    slot86,
    slot87,
    slot88,
    slot89,
    slot90,
    slot91,
    slot92,
    slot93,
    slot94,
    slot95,
    slot96,
    slot97,
    slot98,
    slot99,
    slot100,
    slot101,
    slot102,
    slot103,
    slot104,
    slot105,
    slot106,
    slot107,
    slot108,
    slot109,
    slot110,
    slot111,
    slot112,
    slot113,
    slot114,
    slot115,
    slot116,
    slot117,
    slot118,
    slot119,
    slot120,
    slot121,
    slot122,
    slot123,
    slot124,
    slot125,
    slot126,
    slot127
};

static struct stub_object compositor_object = { stub_vtbl, "Windows.UI.Composition.Compositor instance" };
static struct stub_object apiinfo_statics = { stub_vtbl, "WindowsUdk ApiInformation statics" };

/* the factories: ActivateInstance of the compositor answers its object, any other interface asked of a factory is answered by the
 * object of the class (the statics of ApiInformation) */
struct stub_factory
{
    IActivationFactory IActivationFactory_iface;
    const char *name;
    struct stub_object *object;
};

static inline struct stub_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct stub_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct stub_factory *impl = impl_from_IActivationFactory( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = iface;
        return S_OK;
    }
    FIXME( "factory of %s: QueryInterface(%s): the object of the class (caller %p)\n", impl->name, debugstr_guid( iid ),
           __builtin_return_address(0) );
    *out = impl->object;
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct stub_factory *impl = impl_from_IActivationFactory( iface );

    FIXME( "factory of %s: ActivateInstance (caller %p)\n", impl->name, __builtin_return_address(0) );
    if (impl->object != &compositor_object) return E_NOTIMPL;
    *instance = (IInspectable *)&compositor_object;
    return S_OK;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_GetIids, factory_GetRuntimeClassName, factory_GetTrustLevel,
    factory_ActivateInstance,
};

static struct stub_factory compositor_factory = { { &factory_vtbl }, "Windows.UI.Composition.Compositor", &compositor_object };
static struct stub_factory apiinfo_factory = { { &factory_vtbl }, "WindowsUdk.Foundation.Metadata.ApiInformation", &apiinfo_statics };

IActivationFactory *compositor_stub_factory = &compositor_factory.IActivationFactory_iface;
IActivationFactory *apiinfo_stub_factory = &apiinfo_factory.IActivationFactory_iface;
