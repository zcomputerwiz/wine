/* WinRT small settings classes: EducationSettings, PlatformDiagnosticsAndUsageDataSettings, AssignedAccessSettings,
 * DiagnosticsSettings, ApplicationDataManager and PenDevice
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
 * What a desktop application on Windows 11 sees (measured, unpackaged): not an education environment; no assigned access
 * (not enabled, not a kiosk, the default settings have no user); an empty or null user is E_INVALIDARG; the pen of a
 * pointer is nothing (S_OK, no object). Wine has no telemetry: the diagnostics level is Basic (the lowest a consumer system
 * has) and the diagnostics are not used to tailor experiences. ApplicationDataManager gives the data of the package of
 * the process; other packages are not found (Windows gives the data of any installed package of the user).
 */

#include "private.h"

#include "appmodel.h"
#include "roapi.h"
#include "windows.storage.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

/* ---- EducationSettings ---- */

static HRESULT WINAPI education_get_IsEducationEnvironment( IEducationSettingsStatics *iface, boolean *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = FALSE;
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( education, IEducationSettingsStatics )

static const IEducationSettingsStaticsVtbl education_vtbl =
{
    education_QueryInterface,
    education_AddRef,
    education_Release,
    /* IInspectable methods */
    education_GetIids,
    education_GetRuntimeClassName,
    education_GetTrustLevel,
    /* IEducationSettingsStatics methods */
    education_get_IsEducationEnvironment,
};

static struct static_factory education_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.System.Profile.EducationSettings", NULL, 1,
    {{ &IID_IEducationSettingsStatics, { &education_vtbl, &education_static_factory } }},
};

IActivationFactory *education_settings_factory = &education_static_factory.IActivationFactory_iface;

/* ---- PlatformDiagnosticsAndUsageDataSettings ---- */

static CRITICAL_SECTION level_cs;
static CRITICAL_SECTION_DEBUG level_cs_debug =
{
    0, 0, &level_cs,
    { &level_cs_debug.ProcessLocksList, &level_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": level_cs") }
};
static CRITICAL_SECTION level_cs = { &level_cs_debug, -1, 0, 0, 0, 0 };
static struct list level_handlers = LIST_INIT( level_handlers );

static HRESULT WINAPI platform_get_CollectionLevel( IPlatformDiagnosticsAndUsageDataSettingsStatics *iface,
                                                    PlatformDataCollectionLevel *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = PlatformDataCollectionLevel_Basic;
    return S_OK;
}

static HRESULT WINAPI platform_add_CollectionLevelChanged( IPlatformDiagnosticsAndUsageDataSettingsStatics *iface,
                                                           IEventHandler_IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!handler) return E_INVALIDARG;
    return handler_list_add( &level_handlers, &level_cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI platform_remove_CollectionLevelChanged( IPlatformDiagnosticsAndUsageDataSettingsStatics *iface,
                                                              EventRegistrationToken token )
{
    return handler_list_remove( &level_handlers, &level_cs, token );
}

static HRESULT WINAPI platform_CanCollectDiagnostics( IPlatformDiagnosticsAndUsageDataSettingsStatics *iface,
                                                      PlatformDataCollectionLevel level, boolean *result )
{
    TRACE( "iface %p, level %d, result %p.\n", iface, level, result );
    *result = level <= PlatformDataCollectionLevel_Basic;
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( platform, IPlatformDiagnosticsAndUsageDataSettingsStatics )

static const IPlatformDiagnosticsAndUsageDataSettingsStaticsVtbl platform_vtbl =
{
    platform_QueryInterface,
    platform_AddRef,
    platform_Release,
    /* IInspectable methods */
    platform_GetIids,
    platform_GetRuntimeClassName,
    platform_GetTrustLevel,
    /* IPlatformDiagnosticsAndUsageDataSettingsStatics methods */
    platform_get_CollectionLevel,
    platform_add_CollectionLevelChanged,
    platform_remove_CollectionLevelChanged,
    platform_CanCollectDiagnostics,
};

static struct static_factory platform_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.System.Profile.PlatformDiagnosticsAndUsageDataSettings", NULL, 1,
    {{ &IID_IPlatformDiagnosticsAndUsageDataSettingsStatics, { &platform_vtbl, &platform_static_factory } }},
};

IActivationFactory *platform_diagnostics_factory = &platform_static_factory.IActivationFactory_iface;

/* ---- AssignedAccessSettings ---- */

struct user_settings
{
    struct simple_object base;
    IUser *user;
};

static void user_settings_destroy( struct simple_object *object )
{
    struct user_settings *impl = (struct user_settings *)object;

    if (impl->user) IUser_Release( impl->user );
}

static HRESULT user_settings_create( const void *vtbl, const GUID *iid, const WCHAR *class_name, IUser *user, void **out )
{
    struct simple_object *object;
    struct user_settings *impl;
    HRESULT hr;

    if (FAILED(hr = simple_object_create( sizeof(*impl), vtbl, iid, class_name, user_settings_destroy, &object ))) return hr;
    impl = (struct user_settings *)object;
    if (user) IUser_AddRef( (impl->user = user) );
    *out = object;
    return S_OK;
}

DEFINE_SIMPLE_IINSPECTABLE( assigned, IAssignedAccessSettings )

static HRESULT WINAPI assigned_get_IsEnabled( IAssignedAccessSettings *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI assigned_get_IsSingleAppKioskMode( IAssignedAccessSettings *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI assigned_get_User( IAssignedAccessSettings *iface, IUser **value )
{
    if ((*value = ((struct user_settings *)iface)->user)) IUser_AddRef( *value );
    return S_OK;
}

static const IAssignedAccessSettingsVtbl assigned_vtbl =
{
    assigned_QueryInterface,
    assigned_AddRef,
    assigned_Release,
    /* IInspectable methods */
    assigned_GetIids,
    assigned_GetRuntimeClassName,
    assigned_GetTrustLevel,
    /* IAssignedAccessSettings methods */
    assigned_get_IsEnabled,
    assigned_get_IsSingleAppKioskMode,
    assigned_get_User,
};

static HRESULT WINAPI assigned_statics_GetDefault( IAssignedAccessSettingsStatics *iface, IAssignedAccessSettings **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return user_settings_create( &assigned_vtbl, &IID_IAssignedAccessSettings, L"Windows.System.UserProfile.AssignedAccessSettings",
                                 NULL, (void **)result );
}

static HRESULT WINAPI assigned_statics_GetForUser( IAssignedAccessSettingsStatics *iface, IUser *user,
                                                   IAssignedAccessSettings **result )
{
    TRACE( "iface %p, user %p, result %p.\n", iface, user, result );
    *result = NULL;
    if (!user) return E_INVALIDARG;
    return user_settings_create( &assigned_vtbl, &IID_IAssignedAccessSettings, L"Windows.System.UserProfile.AssignedAccessSettings",
                                 user, (void **)result );
}

DEFINE_STATICS_IINSPECTABLE( assigned_statics, IAssignedAccessSettingsStatics )

static const IAssignedAccessSettingsStaticsVtbl assigned_statics_vtbl =
{
    assigned_statics_QueryInterface,
    assigned_statics_AddRef,
    assigned_statics_Release,
    /* IInspectable methods */
    assigned_statics_GetIids,
    assigned_statics_GetRuntimeClassName,
    assigned_statics_GetTrustLevel,
    /* IAssignedAccessSettingsStatics methods */
    assigned_statics_GetDefault,
    assigned_statics_GetForUser,
};

static struct static_factory assigned_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.System.UserProfile.AssignedAccessSettings", NULL, 1,
    {{ &IID_IAssignedAccessSettingsStatics, { &assigned_statics_vtbl, &assigned_static_factory } }},
};

IActivationFactory *assigned_access_settings_factory = &assigned_static_factory.IActivationFactory_iface;

/* ---- DiagnosticsSettings ---- */

DEFINE_SIMPLE_IINSPECTABLE( diagnostics, IDiagnosticsSettings )

static HRESULT WINAPI diagnostics_get_CanUseDiagnosticsToTailorExperiences( IDiagnosticsSettings *iface, boolean *value )
{
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI diagnostics_get_User( IDiagnosticsSettings *iface, IUser **value )
{
    if ((*value = ((struct user_settings *)iface)->user)) IUser_AddRef( *value );
    return S_OK;
}

static const IDiagnosticsSettingsVtbl diagnostics_vtbl =
{
    diagnostics_QueryInterface,
    diagnostics_AddRef,
    diagnostics_Release,
    /* IInspectable methods */
    diagnostics_GetIids,
    diagnostics_GetRuntimeClassName,
    diagnostics_GetTrustLevel,
    /* IDiagnosticsSettings methods */
    diagnostics_get_CanUseDiagnosticsToTailorExperiences,
    diagnostics_get_User,
};

static HRESULT WINAPI diagnostics_statics_GetDefault( IDiagnosticsSettingsStatics *iface, IDiagnosticsSettings **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return user_settings_create( &diagnostics_vtbl, &IID_IDiagnosticsSettings, L"Windows.System.UserProfile.DiagnosticsSettings",
                                 NULL, (void **)value );
}

static HRESULT WINAPI diagnostics_statics_GetForUser( IDiagnosticsSettingsStatics *iface, IUser *user, IDiagnosticsSettings **value )
{
    TRACE( "iface %p, user %p, value %p.\n", iface, user, value );
    *value = NULL;
    if (!user) return E_INVALIDARG;
    return user_settings_create( &diagnostics_vtbl, &IID_IDiagnosticsSettings, L"Windows.System.UserProfile.DiagnosticsSettings",
                                 user, (void **)value );
}

DEFINE_STATICS_IINSPECTABLE( diagnostics_statics, IDiagnosticsSettingsStatics )

static const IDiagnosticsSettingsStaticsVtbl diagnostics_statics_vtbl =
{
    diagnostics_statics_QueryInterface,
    diagnostics_statics_AddRef,
    diagnostics_statics_Release,
    /* IInspectable methods */
    diagnostics_statics_GetIids,
    diagnostics_statics_GetRuntimeClassName,
    diagnostics_statics_GetTrustLevel,
    /* IDiagnosticsSettingsStatics methods */
    diagnostics_statics_GetDefault,
    diagnostics_statics_GetForUser,
};

static struct static_factory diagnostics_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.System.UserProfile.DiagnosticsSettings", NULL, 1,
    {{ &IID_IDiagnosticsSettingsStatics, { &diagnostics_statics_vtbl, &diagnostics_static_factory } }},
};

IActivationFactory *diagnostics_settings_factory = &diagnostics_static_factory.IActivationFactory_iface;

/* ---- ApplicationDataManager ---- */

static HRESULT WINAPI manager_CreateForPackageFamily( IApplicationDataManagerStatics *iface, HSTRING family, IApplicationData **data )
{
    WCHAR current[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    IApplicationDataStatics *statics;
    UINT32 length = ARRAY_SIZE(current);
    HSTRING class_name;
    HRESULT hr;

    TRACE( "iface %p, family %s, data %p.\n", iface, debugstr_hstring( family ), data );

    *data = NULL;
    if (!WindowsGetStringLen( family )) return E_INVALIDARG;
    if (GetCurrentPackageFamilyName( &length, current ) != ERROR_SUCCESS ||
        wcsicmp( current, WindowsGetStringRawBuffer( family, NULL ) ))
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );

    if (FAILED(hr = WindowsCreateString( RuntimeClass_Windows_Storage_ApplicationData,
                                         ARRAY_SIZE(RuntimeClass_Windows_Storage_ApplicationData) - 1, &class_name ))) return hr;
    hr = RoGetActivationFactory( class_name, &IID_IApplicationDataStatics, (void **)&statics );
    WindowsDeleteString( class_name );
    if (FAILED(hr)) return hr;
    hr = IApplicationDataStatics_get_Current( statics, data );
    IApplicationDataStatics_Release( statics );
    return hr;
}

DEFINE_STATICS_IINSPECTABLE( manager, IApplicationDataManagerStatics )

static const IApplicationDataManagerStaticsVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    /* IInspectable methods */
    manager_GetIids,
    manager_GetRuntimeClassName,
    manager_GetTrustLevel,
    /* IApplicationDataManagerStatics methods */
    manager_CreateForPackageFamily,
};

static struct static_factory manager_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.Management.Core.ApplicationDataManager", NULL, 1,
    {{ &IID_IApplicationDataManagerStatics, { &manager_vtbl, &manager_static_factory } }},
};

IActivationFactory *application_data_manager_factory = &manager_static_factory.IActivationFactory_iface;

/* ---- PenDevice: there is no pen ---- */

static HRESULT WINAPI pen_GetFromPointerId( IPenDeviceStatics *iface, UINT32 pointer_id, IPenDevice **result )
{
    TRACE( "iface %p, pointer_id %lu, result %p.\n", iface, (unsigned long)pointer_id, result );
    *result = NULL;
    return S_OK;
}

DEFINE_STATICS_IINSPECTABLE( pen, IPenDeviceStatics )

static const IPenDeviceStaticsVtbl pen_vtbl =
{
    pen_QueryInterface,
    pen_AddRef,
    pen_Release,
    /* IInspectable methods */
    pen_GetIids,
    pen_GetRuntimeClassName,
    pen_GetTrustLevel,
    /* IPenDeviceStatics methods */
    pen_GetFromPointerId,
};

static struct static_factory pen_static_factory =
{
    {&static_factory_vtbl}, 1, L"Windows.Devices.Input.PenDevice", NULL, 1,
    {{ &IID_IPenDeviceStatics, { &pen_vtbl, &pen_static_factory } }},
};

IActivationFactory *pen_device_factory = &pen_static_factory.IActivationFactory_iface;
