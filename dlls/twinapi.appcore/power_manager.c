/* WinRT Windows.System.Power.PowerManager Implementation
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
 * The values are those of GetSystemPowerStatus when asked. The changed events are accepted and are not raised: Wine has no
 * power notifications to raise them from.
 */

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

enum power_event { EVENT_ENERGY_SAVER, EVENT_BATTERY, EVENT_SUPPLY, EVENT_CHARGE, EVENT_DISCHARGE, EVENT_COUNT };

static CRITICAL_SECTION events_cs;
static CRITICAL_SECTION_DEBUG events_cs_debug =
{
    0, 0, &events_cs,
    { &events_cs_debug.ProcessLocksList, &events_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": events_cs") }
};
static CRITICAL_SECTION events_cs = { &events_cs_debug, -1, 0, 0, 0, 0 };
static struct list events[EVENT_COUNT] =
{
    LIST_INIT( events[0] ), LIST_INIT( events[1] ), LIST_INIT( events[2] ), LIST_INIT( events[3] ), LIST_INIT( events[4] ),
};

static BOOL get_status( SYSTEM_POWER_STATUS *status )
{
    memset( status, 0, sizeof(*status) );
    if (GetSystemPowerStatus( status )) return TRUE;
    status->ACLineStatus = 255;
    status->BatteryFlag = 255;
    status->BatteryLifePercent = 255;
    status->BatteryLifeTime = ~0u;
    return FALSE;
}

static BOOL has_battery( const SYSTEM_POWER_STATUS *status )
{
    return status->BatteryFlag != 255 && !(status->BatteryFlag & 128);
}

static HRESULT add_event( enum power_event event, IEventHandler_IInspectable *handler, EventRegistrationToken *token )
{
    if (!handler) return E_INVALIDARG;
    return handler_list_add( &events[event], &events_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI statics_get_EnergySaverStatus( IPowerManagerStatics *iface, EnergySaverStatus *value )
{
    SYSTEM_POWER_STATUS status;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_status( &status );
    if (!has_battery( &status )) *value = EnergySaverStatus_Disabled;
    else *value = (status.SystemStatusFlag & 1) ? EnergySaverStatus_On : EnergySaverStatus_Off;
    return S_OK;
}

static HRESULT WINAPI statics_add_EnergySaverStatusChanged( IPowerManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                            EventRegistrationToken *token )
{
    return add_event( EVENT_ENERGY_SAVER, handler, token );
}

static HRESULT WINAPI statics_remove_EnergySaverStatusChanged( IPowerManagerStatics *iface, EventRegistrationToken token )
{
    return handler_list_remove( &events[EVENT_ENERGY_SAVER], &events_cs, token );
}

static HRESULT WINAPI statics_get_BatteryStatus( IPowerManagerStatics *iface, BatteryStatus *value )
{
    SYSTEM_POWER_STATUS status;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_status( &status );
    if (!has_battery( &status )) *value = BatteryStatus_NotPresent;
    else if (status.BatteryFlag & 8) *value = BatteryStatus_Charging;
    else if (status.ACLineStatus == 1) *value = BatteryStatus_Idle;
    else *value = BatteryStatus_Discharging;
    return S_OK;
}

static HRESULT WINAPI statics_add_BatteryStatusChanged( IPowerManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                        EventRegistrationToken *token )
{
    return add_event( EVENT_BATTERY, handler, token );
}

static HRESULT WINAPI statics_remove_BatteryStatusChanged( IPowerManagerStatics *iface, EventRegistrationToken token )
{
    return handler_list_remove( &events[EVENT_BATTERY], &events_cs, token );
}

static HRESULT WINAPI statics_get_PowerSupplyStatus( IPowerManagerStatics *iface, PowerSupplyStatus *value )
{
    SYSTEM_POWER_STATUS status;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_status( &status );
    *value = status.ACLineStatus == 1 ? PowerSupplyStatus_Adequate : PowerSupplyStatus_NotPresent;
    return S_OK;
}

static HRESULT WINAPI statics_add_PowerSupplyStatusChanged( IPowerManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                            EventRegistrationToken *token )
{
    return add_event( EVENT_SUPPLY, handler, token );
}

static HRESULT WINAPI statics_remove_PowerSupplyStatusChanged( IPowerManagerStatics *iface, EventRegistrationToken token )
{
    return handler_list_remove( &events[EVENT_SUPPLY], &events_cs, token );
}

static HRESULT WINAPI statics_get_RemainingChargePercent( IPowerManagerStatics *iface, INT32 *value )
{
    SYSTEM_POWER_STATUS status;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_status( &status );
    *value = status.BatteryLifePercent <= 100 ? status.BatteryLifePercent : 100;
    return S_OK;
}

static HRESULT WINAPI statics_add_RemainingChargePercentChanged( IPowerManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                                 EventRegistrationToken *token )
{
    return add_event( EVENT_CHARGE, handler, token );
}

static HRESULT WINAPI statics_remove_RemainingChargePercentChanged( IPowerManagerStatics *iface, EventRegistrationToken token )
{
    return handler_list_remove( &events[EVENT_CHARGE], &events_cs, token );
}

/* a time span is in 100 ns units; unknown is the maximum, as Windows gives when it cannot tell */
static HRESULT WINAPI statics_get_RemainingDischargeTime( IPowerManagerStatics *iface, TimeSpan *value )
{
    SYSTEM_POWER_STATUS status;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_status( &status );
    value->Duration = status.BatteryLifeTime != ~0u ? (INT64)status.BatteryLifeTime * 10000000 : (INT64)0x7fffffffffffffffLL;
    return S_OK;
}

static HRESULT WINAPI statics_add_RemainingDischargeTimeChanged( IPowerManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                                 EventRegistrationToken *token )
{
    return add_event( EVENT_DISCHARGE, handler, token );
}

static HRESULT WINAPI statics_remove_RemainingDischargeTimeChanged( IPowerManagerStatics *iface, EventRegistrationToken token )
{
    return handler_list_remove( &events[EVENT_DISCHARGE], &events_cs, token );
}

DEFINE_STATICS_IINSPECTABLE( statics, IPowerManagerStatics )

static const IPowerManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IPowerManagerStatics methods */
    statics_get_EnergySaverStatus,
    statics_add_EnergySaverStatusChanged,
    statics_remove_EnergySaverStatusChanged,
    statics_get_BatteryStatus,
    statics_add_BatteryStatusChanged,
    statics_remove_BatteryStatusChanged,
    statics_get_PowerSupplyStatus,
    statics_add_PowerSupplyStatusChanged,
    statics_remove_PowerSupplyStatusChanged,
    statics_get_RemainingChargePercent,
    statics_add_RemainingChargePercentChanged,
    statics_remove_RemainingChargePercentChanged,
    statics_get_RemainingDischargeTime,
    statics_add_RemainingDischargeTimeChanged,
    statics_remove_RemainingDischargeTimeChanged,
};

static struct static_factory power_manager_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.System.Power.PowerManager", NULL, 1,
    {{ &IID_IPowerManagerStatics, { &statics_vtbl, &power_manager_static_factory } }},
};

IActivationFactory *power_manager_factory = &power_manager_static_factory.IActivationFactory_iface;
