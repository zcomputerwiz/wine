/*
 * Copyright 2023 Rémi Bernon for CodeWeavers
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
#include <stddef.h>

#include "windef.h"
#include "winbase.h"

#define COBJMACROS
#include "winstring.h"
#include "appnotify.h"
#include "activation.h"
#include "shlobj.h"
#include "wine/list.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_UI_Core
#define WIDL_using_Windows_ApplicationModel_Activation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Security_ExchangeActiveSyncProvisioning
#include "windows.security.exchangeactivesyncprovisioning.h"
#define WIDL_using_Windows_System_Profile
#include "windows.system.profile.h"
#define WIDL_using_Windows_System_UserProfile
#include "windows.system.userprofile.h"
#define WIDL_using_Windows_UI_ViewManagement
#include "windows.ui.viewmanagement.h"
#define WIDL_using_Windows_ApplicationModel_Core
#include "windows.applicationmodel.core.h"
#define WIDL_using_Windows_ApplicationModel_DataTransfer
#include "windows.applicationmodel.datatransfer.h"

#include "wine/debug.h"

extern HRESULT activation_manager_get_class_object( REFCLSID clsid, REFIID iid, void **out );

/* event handler lists and views (core_view.c) */
extern HRESULT handler_list_add( struct list *list, CRITICAL_SECTION *cs, IUnknown *handler, EventRegistrationToken *token );
extern HRESULT handler_list_remove( struct list *list, CRITICAL_SECTION *cs, EventRegistrationToken token );
extern IUnknown **handler_list_snapshot( struct list *list, CRITICAL_SECTION *cs, unsigned int *count );
extern void handler_list_snapshot_free( IUnknown **handlers, unsigned int count );
extern void handler_list_clear( struct list *list );
extern HRESULT dispatch_callback( ICoreDispatcher *dispatcher, void (*callback)( void *context ), void *context );
extern HRESULT core_view_create( ICoreWindow *window, boolean is_main, ICoreApplicationView **out );
extern ICoreApplicationView *core_view_for_current_thread(void);
extern ICoreApplicationView *core_view_main(void);
extern void core_view_detach( ICoreApplicationView *view );
extern void core_view_raise_launch( ICoreApplicationView *view, const WCHAR *arguments, const WCHAR *tile_id );
extern void core_view_for_each( void (*callback)( void *context ), void *context );

extern IActivationFactory *application_view_factory;
extern IActivationFactory *client_device_information_factory;
extern IActivationFactory *analytics_info_factory;
extern IActivationFactory *advertising_manager_factory;
extern IActivationFactory *core_application_factory;
extern IActivationFactory *data_transfer_manager_statics_factory;

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )
