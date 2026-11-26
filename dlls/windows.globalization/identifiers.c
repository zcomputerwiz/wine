/* WinRT Windows.Globalization.CalendarIdentifiers and ClockIdentifiers implementation
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

WINE_DEFAULT_DEBUG_CHANNEL(locale);

static HRESULT identifier( const WCHAR *name, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsCreateString( name, wcslen( name ), value );
}

/* both classes are static: their factories have no default constructor */
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
    return E_NOTIMPL;
}

/* ---- CalendarIdentifiers ---- */

struct calendar_identifiers
{
    IActivationFactory IActivationFactory_iface;
    ICalendarIdentifiersStatics ICalendarIdentifiersStatics_iface;
    ICalendarIdentifiersStatics2 ICalendarIdentifiersStatics2_iface;
    ICalendarIdentifiersStatics3 ICalendarIdentifiersStatics3_iface;
    LONG ref;
};

static inline struct calendar_identifiers *calendar_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct calendar_identifiers, IActivationFactory_iface );
}

static HRESULT WINAPI calendar_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct calendar_identifiers *impl = calendar_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ICalendarIdentifiersStatics ))
        *out = &impl->ICalendarIdentifiersStatics_iface;
    else if (IsEqualGUID( iid, &IID_ICalendarIdentifiersStatics2 ))
        *out = &impl->ICalendarIdentifiersStatics2_iface;
    else if (IsEqualGUID( iid, &IID_ICalendarIdentifiersStatics3 ))
        *out = &impl->ICalendarIdentifiersStatics3_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI calendar_factory_AddRef( IActivationFactory *iface )
{
    struct calendar_identifiers *impl = calendar_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI calendar_factory_Release( IActivationFactory *iface )
{
    struct calendar_identifiers *impl = calendar_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static const IActivationFactoryVtbl calendar_factory_vtbl =
{
    calendar_factory_QueryInterface,
    calendar_factory_AddRef,
    calendar_factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

#define CALENDAR_IDENTIFIER( pfx, iface_type, prop, name )                                   \
    static HRESULT WINAPI pfx##_get_##prop( iface_type *iface, HSTRING *value )               \
    {                                                                                         \
        TRACE( "iface %p, value %p.\n", iface, value );                                       \
        return identifier( name, value );                                                     \
    }

DEFINE_IINSPECTABLE( calendar_statics, ICalendarIdentifiersStatics, struct calendar_identifiers, IActivationFactory_iface )

CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Gregorian, L"GregorianCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Hebrew, L"HebrewCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Hijri, L"HijriCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Japanese, L"JapaneseCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Julian, L"JulianCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Korean, L"KoreanCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Taiwan, L"TaiwanCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, Thai, L"ThaiCalendar" )
CALENDAR_IDENTIFIER( calendar_statics, ICalendarIdentifiersStatics, UmAlQura, L"UmAlQuraCalendar" )

static const ICalendarIdentifiersStaticsVtbl calendar_statics_vtbl =
{
    calendar_statics_QueryInterface,
    calendar_statics_AddRef,
    calendar_statics_Release,
    /* IInspectable methods */
    calendar_statics_GetIids,
    calendar_statics_GetRuntimeClassName,
    calendar_statics_GetTrustLevel,
    /* ICalendarIdentifiersStatics methods */
    calendar_statics_get_Gregorian,
    calendar_statics_get_Hebrew,
    calendar_statics_get_Hijri,
    calendar_statics_get_Japanese,
    calendar_statics_get_Julian,
    calendar_statics_get_Korean,
    calendar_statics_get_Taiwan,
    calendar_statics_get_Thai,
    calendar_statics_get_UmAlQura,
};

DEFINE_IINSPECTABLE( calendar_statics2, ICalendarIdentifiersStatics2, struct calendar_identifiers, IActivationFactory_iface )

CALENDAR_IDENTIFIER( calendar_statics2, ICalendarIdentifiersStatics2, Persian, L"PersianCalendar" )

static const ICalendarIdentifiersStatics2Vtbl calendar_statics2_vtbl =
{
    calendar_statics2_QueryInterface,
    calendar_statics2_AddRef,
    calendar_statics2_Release,
    /* IInspectable methods */
    calendar_statics2_GetIids,
    calendar_statics2_GetRuntimeClassName,
    calendar_statics2_GetTrustLevel,
    /* ICalendarIdentifiersStatics2 methods */
    calendar_statics2_get_Persian,
};

DEFINE_IINSPECTABLE( calendar_statics3, ICalendarIdentifiersStatics3, struct calendar_identifiers, IActivationFactory_iface )

CALENDAR_IDENTIFIER( calendar_statics3, ICalendarIdentifiersStatics3, ChineseLunar, L"ChineseLunarCalendar" )
CALENDAR_IDENTIFIER( calendar_statics3, ICalendarIdentifiersStatics3, JapaneseLunar, L"JapaneseLunarCalendar" )
CALENDAR_IDENTIFIER( calendar_statics3, ICalendarIdentifiersStatics3, KoreanLunar, L"KoreanLunarCalendar" )
CALENDAR_IDENTIFIER( calendar_statics3, ICalendarIdentifiersStatics3, TaiwanLunar, L"TaiwanLunarCalendar" )
CALENDAR_IDENTIFIER( calendar_statics3, ICalendarIdentifiersStatics3, VietnameseLunar, L"VietnameseLunarCalendar" )

static const ICalendarIdentifiersStatics3Vtbl calendar_statics3_vtbl =
{
    calendar_statics3_QueryInterface,
    calendar_statics3_AddRef,
    calendar_statics3_Release,
    /* IInspectable methods */
    calendar_statics3_GetIids,
    calendar_statics3_GetRuntimeClassName,
    calendar_statics3_GetTrustLevel,
    /* ICalendarIdentifiersStatics3 methods */
    calendar_statics3_get_ChineseLunar,
    calendar_statics3_get_JapaneseLunar,
    calendar_statics3_get_KoreanLunar,
    calendar_statics3_get_TaiwanLunar,
    calendar_statics3_get_VietnameseLunar,
};

static struct calendar_identifiers calendar_identifiers =
{
    {&calendar_factory_vtbl},
    {&calendar_statics_vtbl},
    {&calendar_statics2_vtbl},
    {&calendar_statics3_vtbl},
    1,
};

IActivationFactory *calendar_identifiers_factory = &calendar_identifiers.IActivationFactory_iface;

/* ---- ClockIdentifiers ---- */

struct clock_identifiers
{
    IActivationFactory IActivationFactory_iface;
    IClockIdentifiersStatics IClockIdentifiersStatics_iface;
    LONG ref;
};

static inline struct clock_identifiers *clock_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct clock_identifiers, IActivationFactory_iface );
}

static HRESULT WINAPI clock_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct clock_identifiers *impl = clock_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IClockIdentifiersStatics ))
        *out = &impl->IClockIdentifiersStatics_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI clock_factory_AddRef( IActivationFactory *iface )
{
    struct clock_identifiers *impl = clock_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI clock_factory_Release( IActivationFactory *iface )
{
    struct clock_identifiers *impl = clock_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static const IActivationFactoryVtbl clock_factory_vtbl =
{
    clock_factory_QueryInterface,
    clock_factory_AddRef,
    clock_factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( clock_statics, IClockIdentifiersStatics, struct clock_identifiers, IActivationFactory_iface )

CALENDAR_IDENTIFIER( clock_statics, IClockIdentifiersStatics, TwelveHour, L"12HourClock" )
CALENDAR_IDENTIFIER( clock_statics, IClockIdentifiersStatics, TwentyFourHour, L"24HourClock" )

static const IClockIdentifiersStaticsVtbl clock_statics_vtbl =
{
    clock_statics_QueryInterface,
    clock_statics_AddRef,
    clock_statics_Release,
    /* IInspectable methods */
    clock_statics_GetIids,
    clock_statics_GetRuntimeClassName,
    clock_statics_GetTrustLevel,
    /* IClockIdentifiersStatics methods */
    clock_statics_get_TwelveHour,
    clock_statics_get_TwentyFourHour,
};

static struct clock_identifiers clock_identifiers =
{
    {&clock_factory_vtbl},
    {&clock_statics_vtbl},
    1,
};

IActivationFactory *clock_identifiers_factory = &clock_identifiers.IActivationFactory_iface;
