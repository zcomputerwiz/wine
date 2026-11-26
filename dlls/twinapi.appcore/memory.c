/* WinRT Windows.System.MemoryManager implementation
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

/*
 * The memory of the application as Windows 11 reports it to a process that is not managed by a resource policy: its
 * usage is its commit charge, its limit the system's commit limit (nothing sets another), a usage level only for a
 * packaged process (another one gets STATUS_NOT_FOUND), and the limit events are never raised.
 */

#include "private.h"
#include "psapi.h"
#include "appmodel.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinapi);

static void process_counters( PROCESS_MEMORY_COUNTERS *counters )
{
    memset( counters, 0, sizeof(*counters) );
    counters->cb = sizeof(*counters);
    GetProcessMemoryInfo( GetCurrentProcess(), counters, sizeof(*counters) );
}

static UINT64 commit_limit(void)
{
    MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
    return GlobalMemoryStatusEx( &status ) ? status.ullTotalPageFile : 0;
}

/* ---- AppMemoryReport and ProcessMemoryReport: the counters when they were made ---- */

struct memory_report
{
    IAppMemoryReport IAppMemoryReport_iface;
    IAppMemoryReport2 IAppMemoryReport2_iface;
    IProcessMemoryReport IProcessMemoryReport_iface;
    LONG ref;
    PROCESS_MEMORY_COUNTERS counters;
    UINT64 limit;
};

static inline struct memory_report *impl_from_IAppMemoryReport( IAppMemoryReport *iface )
{
    return CONTAINING_RECORD( iface, struct memory_report, IAppMemoryReport_iface );
}

static HRESULT WINAPI app_report_QueryInterface( IAppMemoryReport *iface, REFIID iid, void **out )
{
    struct memory_report *impl = impl_from_IAppMemoryReport( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAppMemoryReport ))
        *out = &impl->IAppMemoryReport_iface;
    else if (IsEqualGUID( iid, &IID_IAppMemoryReport2 ))
        *out = &impl->IAppMemoryReport2_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI app_report_AddRef( IAppMemoryReport *iface )
{
    return InterlockedIncrement( &impl_from_IAppMemoryReport( iface )->ref );
}

static ULONG WINAPI app_report_Release( IAppMemoryReport *iface )
{
    struct memory_report *impl = impl_from_IAppMemoryReport( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI app_report_GetIids( IAppMemoryReport *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI app_report_GetRuntimeClassName( IAppMemoryReport *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_System_AppMemoryReport, wcslen( RuntimeClass_Windows_System_AppMemoryReport ), name );
}

static HRESULT WINAPI app_report_GetTrustLevel( IAppMemoryReport *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI app_report_get_PrivateCommitUsage( IAppMemoryReport *iface, UINT64 *value )
{
    *value = impl_from_IAppMemoryReport( iface )->counters.PagefileUsage;
    return S_OK;
}

static HRESULT WINAPI app_report_get_PeakPrivateCommitUsage( IAppMemoryReport *iface, UINT64 *value )
{
    struct memory_report *impl = impl_from_IAppMemoryReport( iface );
    *value = max( impl->counters.PeakPagefileUsage, impl->counters.PagefileUsage );
    return S_OK;
}

static HRESULT WINAPI app_report_get_TotalCommitUsage( IAppMemoryReport *iface, UINT64 *value )
{
    *value = impl_from_IAppMemoryReport( iface )->counters.PagefileUsage;
    return S_OK;
}

static HRESULT WINAPI app_report_get_TotalCommitLimit( IAppMemoryReport *iface, UINT64 *value )
{
    *value = impl_from_IAppMemoryReport( iface )->limit;
    return S_OK;
}

static const IAppMemoryReportVtbl app_report_vtbl =
{
    app_report_QueryInterface,
    app_report_AddRef,
    app_report_Release,
    /* IInspectable methods */
    app_report_GetIids,
    app_report_GetRuntimeClassName,
    app_report_GetTrustLevel,
    /* IAppMemoryReport methods */
    app_report_get_PrivateCommitUsage,
    app_report_get_PeakPrivateCommitUsage,
    app_report_get_TotalCommitUsage,
    app_report_get_TotalCommitLimit,
};

DEFINE_IINSPECTABLE( app_report2, IAppMemoryReport2, struct memory_report, IAppMemoryReport_iface )

/* no expected limit: none is set for the application */
static HRESULT WINAPI app_report2_get_ExpectedTotalCommitLimit( IAppMemoryReport2 *iface, UINT64 *value )
{
    *value = 0;
    return S_OK;
}

static const IAppMemoryReport2Vtbl app_report2_vtbl =
{
    app_report2_QueryInterface,
    app_report2_AddRef,
    app_report2_Release,
    /* IInspectable methods */
    app_report2_GetIids,
    app_report2_GetRuntimeClassName,
    app_report2_GetTrustLevel,
    /* IAppMemoryReport2 methods */
    app_report2_get_ExpectedTotalCommitLimit,
};

static inline struct memory_report *impl_from_IProcessMemoryReport( IProcessMemoryReport *iface )
{
    return CONTAINING_RECORD( iface, struct memory_report, IProcessMemoryReport_iface );
}

static HRESULT WINAPI process_report_QueryInterface( IProcessMemoryReport *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IProcessMemoryReport ))
    {
        IProcessMemoryReport_AddRef( (*out = iface) );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI process_report_AddRef( IProcessMemoryReport *iface )
{
    return InterlockedIncrement( &impl_from_IProcessMemoryReport( iface )->ref );
}

static ULONG WINAPI process_report_Release( IProcessMemoryReport *iface )
{
    struct memory_report *impl = impl_from_IProcessMemoryReport( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI process_report_GetIids( IProcessMemoryReport *iface, ULONG *count, IID **iids )
{
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI process_report_GetRuntimeClassName( IProcessMemoryReport *iface, HSTRING *name )
{
    return WindowsCreateString( RuntimeClass_Windows_System_ProcessMemoryReport,
                                wcslen( RuntimeClass_Windows_System_ProcessMemoryReport ), name );
}

static HRESULT WINAPI process_report_GetTrustLevel( IProcessMemoryReport *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

/* the private part of the working set is not counted apart: the whole working set */
static HRESULT WINAPI process_report_get_PrivateWorkingSetUsage( IProcessMemoryReport *iface, UINT64 *value )
{
    *value = impl_from_IProcessMemoryReport( iface )->counters.WorkingSetSize;
    return S_OK;
}

static HRESULT WINAPI process_report_get_TotalWorkingSetUsage( IProcessMemoryReport *iface, UINT64 *value )
{
    *value = impl_from_IProcessMemoryReport( iface )->counters.WorkingSetSize;
    return S_OK;
}

static const IProcessMemoryReportVtbl process_report_vtbl =
{
    process_report_QueryInterface,
    process_report_AddRef,
    process_report_Release,
    /* IInspectable methods */
    process_report_GetIids,
    process_report_GetRuntimeClassName,
    process_report_GetTrustLevel,
    /* IProcessMemoryReport methods */
    process_report_get_PrivateWorkingSetUsage,
    process_report_get_TotalWorkingSetUsage,
};

static struct memory_report *memory_report_create(void)
{
    struct memory_report *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return NULL;
    impl->IAppMemoryReport_iface.lpVtbl = &app_report_vtbl;
    impl->IAppMemoryReport2_iface.lpVtbl = &app_report2_vtbl;
    impl->IProcessMemoryReport_iface.lpVtbl = &process_report_vtbl;
    impl->ref = 1;
    process_counters( &impl->counters );
    impl->limit = commit_limit();
    return impl;
}

/* ---- the statics ---- */

struct memory_manager
{
    IActivationFactory IActivationFactory_iface;
    IMemoryManagerStatics IMemoryManagerStatics_iface;
    IMemoryManagerStatics2 IMemoryManagerStatics2_iface;
    IMemoryManagerStatics3 IMemoryManagerStatics3_iface;
    IMemoryManagerStatics4 IMemoryManagerStatics4_iface;
    LONG ref;
};

static inline struct memory_manager *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct memory_manager, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct memory_manager *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IMemoryManagerStatics ))
        *out = &impl->IMemoryManagerStatics_iface;
    else if (IsEqualGUID( iid, &IID_IMemoryManagerStatics2 ))
        *out = &impl->IMemoryManagerStatics2_iface;
    else if (IsEqualGUID( iid, &IID_IMemoryManagerStatics3 ))
        *out = &impl->IMemoryManagerStatics3_iface;
    else if (IsEqualGUID( iid, &IID_IMemoryManagerStatics4 ))
        *out = &impl->IMemoryManagerStatics4_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
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
    FIXME( "iface %p, count %p, iids %p stub!\n", iface, count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    FIXME( "iface %p, name %p stub!\n", iface, name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    FIXME( "iface %p, level %p stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
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

DEFINE_IINSPECTABLE( statics, IMemoryManagerStatics, struct memory_manager, IActivationFactory_iface )

static HRESULT WINAPI statics_get_AppMemoryUsage( IMemoryManagerStatics *iface, UINT64 *value )
{
    PROCESS_MEMORY_COUNTERS counters;

    TRACE( "iface %p, value %p.\n", iface, value );
    process_counters( &counters );
    *value = counters.PagefileUsage;
    return S_OK;
}

static HRESULT WINAPI statics_get_AppMemoryUsageLimit( IMemoryManagerStatics *iface, UINT64 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = commit_limit();
    return S_OK;
}

/* only a packaged process has a usage level, as on Windows */
static HRESULT WINAPI statics_get_AppMemoryUsageLevel( IMemoryManagerStatics *iface, AppMemoryUsageLevel *value )
{
    PROCESS_MEMORY_COUNTERS counters;
    UINT32 length = 0;
    UINT64 limit;

    TRACE( "iface %p, value %p.\n", iface, value );
    if (GetCurrentPackageFullName( &length, NULL ) == APPMODEL_ERROR_NO_PACKAGE)
        return HRESULT_FROM_NT( 0xc0000225 /* STATUS_NOT_FOUND */ );
    process_counters( &counters );
    limit = commit_limit();
    if (!limit || counters.PagefileUsage <= limit / 10 * 6) *value = AppMemoryUsageLevel_Low;
    else if (counters.PagefileUsage <= limit / 10 * 8) *value = AppMemoryUsageLevel_Medium;
    else if (counters.PagefileUsage <= limit) *value = AppMemoryUsageLevel_High;
    else *value = AppMemoryUsageLevel_OverLimit;
    return S_OK;
}

/* the usage events are never raised: no resource policy watches the application */
static HRESULT WINAPI statics_add_AppMemoryUsageIncreased( IMemoryManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                           EventRegistrationToken *token )
{
    static LONG next;

    TRACE( "iface %p, handler %p, token %p.\n", iface, handler, token );
    if (!handler) return E_INVALIDARG;
    token->value = InterlockedIncrement( &next );
    return S_OK;
}

static HRESULT WINAPI statics_remove_AppMemoryUsageIncreased( IMemoryManagerStatics *iface, EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI statics_add_AppMemoryUsageDecreased( IMemoryManagerStatics *iface, IEventHandler_IInspectable *handler,
                                                           EventRegistrationToken *token )
{
    return statics_add_AppMemoryUsageIncreased( iface, handler, token );
}

static HRESULT WINAPI statics_remove_AppMemoryUsageDecreased( IMemoryManagerStatics *iface, EventRegistrationToken token )
{
    return S_OK;
}

static HRESULT WINAPI statics_add_AppMemoryUsageLimitChanging( IMemoryManagerStatics *iface,
        IEventHandler_AppMemoryUsageLimitChangingEventArgs *handler, EventRegistrationToken *token )
{
    return statics_add_AppMemoryUsageIncreased( iface, (IEventHandler_IInspectable *)handler, token );
}

static HRESULT WINAPI statics_remove_AppMemoryUsageLimitChanging( IMemoryManagerStatics *iface, EventRegistrationToken token )
{
    return S_OK;
}

static const IMemoryManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IMemoryManagerStatics methods */
    statics_get_AppMemoryUsage,
    statics_get_AppMemoryUsageLimit,
    statics_get_AppMemoryUsageLevel,
    statics_add_AppMemoryUsageIncreased,
    statics_remove_AppMemoryUsageIncreased,
    statics_add_AppMemoryUsageDecreased,
    statics_remove_AppMemoryUsageDecreased,
    statics_add_AppMemoryUsageLimitChanging,
    statics_remove_AppMemoryUsageLimitChanging,
};

DEFINE_IINSPECTABLE( statics2, IMemoryManagerStatics2, struct memory_manager, IActivationFactory_iface )

static HRESULT WINAPI statics2_GetAppMemoryReport( IMemoryManagerStatics2 *iface, IAppMemoryReport **report )
{
    struct memory_report *impl;

    TRACE( "iface %p, report %p.\n", iface, report );
    if (!(impl = memory_report_create())) return E_OUTOFMEMORY;
    *report = &impl->IAppMemoryReport_iface;
    return S_OK;
}

static HRESULT WINAPI statics2_GetProcessMemoryReport( IMemoryManagerStatics2 *iface, IProcessMemoryReport **report )
{
    struct memory_report *impl;

    TRACE( "iface %p, report %p.\n", iface, report );
    if (!(impl = memory_report_create())) return E_OUTOFMEMORY;
    *report = &impl->IProcessMemoryReport_iface;
    return S_OK;
}

static const IMemoryManagerStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IMemoryManagerStatics2 methods */
    statics2_GetAppMemoryReport,
    statics2_GetProcessMemoryReport,
};

DEFINE_IINSPECTABLE( statics3, IMemoryManagerStatics3, struct memory_manager, IActivationFactory_iface )

/* the limit is the system's: it cannot be set, as on Windows for a process without a resource policy */
static HRESULT WINAPI statics3_TrySetAppMemoryUsageLimit( IMemoryManagerStatics3 *iface, UINT64 value, boolean *result )
{
    TRACE( "iface %p, value %I64u, result %p.\n", iface, value, result );
    *result = FALSE;
    return S_OK;
}

static const IMemoryManagerStatics3Vtbl statics3_vtbl =
{
    statics3_QueryInterface,
    statics3_AddRef,
    statics3_Release,
    /* IInspectable methods */
    statics3_GetIids,
    statics3_GetRuntimeClassName,
    statics3_GetTrustLevel,
    /* IMemoryManagerStatics3 methods */
    statics3_TrySetAppMemoryUsageLimit,
};

DEFINE_IINSPECTABLE( statics4, IMemoryManagerStatics4, struct memory_manager, IActivationFactory_iface )

static HRESULT WINAPI statics4_get_ExpectedAppMemoryUsageLimit( IMemoryManagerStatics4 *iface, UINT64 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 0;
    return S_OK;
}

static const IMemoryManagerStatics4Vtbl statics4_vtbl =
{
    statics4_QueryInterface,
    statics4_AddRef,
    statics4_Release,
    /* IInspectable methods */
    statics4_GetIids,
    statics4_GetRuntimeClassName,
    statics4_GetTrustLevel,
    /* IMemoryManagerStatics4 methods */
    statics4_get_ExpectedAppMemoryUsageLimit,
};

static struct memory_manager memory_manager =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    {&statics3_vtbl},
    {&statics4_vtbl},
    1,
};

IActivationFactory *memory_manager_factory = &memory_manager.IActivationFactory_iface;
