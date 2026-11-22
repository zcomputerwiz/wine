/* Windows.UI.ApplicationSettings.AccountsSettingsPane (WineExplorer UWP)
 *
 * The object model of the accounts pane and its commands, and the interop interface for desktop windows. No pane is drawn:
 * showing it raises AccountCommandsRequested (the handlers fill in the commands, deferrals are honoured) and the returned
 * action is complete. An application that lists the accounts of its providers in its own window works; one that expects
 * the system's pane to appear does not.
 *
 * Reused: the type-pruned vectors of dlls/windows.applicationmodel/vector.c (copied into this module as vector.c; they came
 * from Wine's windows.media.speech) for the command lists; the pattern of the completed asynchronous objects and of the
 * activation factories with several interfaces from windows.applicationmodel/appservice.c.
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
#include "asyncinfo.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

/* the parts every object here has: the reference count, the Inspectable methods of one interface */
#define DEFINE_INSPECTABLE_BASE( pfx, itype, stype, member, class_name, iid )                                       \
static inline stype *pfx##_from( itype *iface )                                                                     \
{                                                                                                                   \
    return CONTAINING_RECORD( iface, stype, member );                                                               \
}                                                                                                                   \
static ULONG WINAPI pfx##_AddRef( itype *iface )                                                                    \
{                                                                                                                   \
    return InterlockedIncrement( &pfx##_from( iface )->ref );                                                       \
}                                                                                                                   \
static ULONG WINAPI pfx##_Release( itype *iface )                                                                   \
{                                                                                                                   \
    stype *impl = pfx##_from( iface );                                                                              \
    ULONG ref = InterlockedDecrement( &impl->ref );                                                                 \
    if (!ref) pfx##_destroy( impl );                                                                                \
    return ref;                                                                                                     \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetIids( itype *iface, ULONG *count, IID **iids )                                       \
{                                                                                                                   \
    IID *list;                                                                                                      \
    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;                                            \
    *list = iid;                                                                                                    \
    *count = 1;                                                                                                     \
    *iids = list;                                                                                                   \
    return S_OK;                                                                                                    \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetRuntimeClassName( itype *iface, HSTRING *name )                                      \
{                                                                                                                   \
    return WindowsCreateString( class_name, ARRAY_SIZE(class_name) - 1, name );                                     \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetTrustLevel( itype *iface, TrustLevel *level )                                        \
{                                                                                                                   \
    *level = BaseTrust;                                                                                             \
    return S_OK;                                                                                                    \
}

#define INSPECTABLE_ENTRIES( pfx ) pfx##_QueryInterface, pfx##_AddRef, pfx##_Release, pfx##_GetIids, \
                                   pfx##_GetRuntimeClassName, pfx##_GetTrustLevel

static BOOL is_basic_iid( REFIID iid )
{
    return IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject );
}

static void release_object( void *object )
{
    if (object) IUnknown_Release( (IUnknown *)object );
}

static void *add_ref_object( void *object )
{
    if (object) IUnknown_AddRef( (IUnknown *)object );
    return object;
}

/*
 * A completed IAsyncAction
 */

struct action
{
    IAsyncAction IAsyncAction_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncActionCompletedHandler *handler;
};

static inline struct action *impl_from_IAsyncAction( IAsyncAction *iface )
{
    return CONTAINING_RECORD( iface, struct action, IAsyncAction_iface );
}

static inline struct action *impl_from_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct action, IAsyncInfo_iface );
}

static HRESULT WINAPI action_QueryInterface( IAsyncAction *iface, REFIID iid, void **out )
{
    struct action *impl = impl_from_IAsyncAction( iface );

    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IAsyncAction )) *out = &impl->IAsyncAction_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo )) *out = &impl->IAsyncInfo_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI action_AddRef( IAsyncAction *iface )
{
    return InterlockedIncrement( &impl_from_IAsyncAction( iface )->ref );
}

static ULONG WINAPI action_Release( IAsyncAction *iface )
{
    struct action *impl = impl_from_IAsyncAction( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        release_object( impl->handler );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI action_GetIids( IAsyncAction *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 2 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IAsyncAction;
    list[1] = IID_IAsyncInfo;
    *count = 2;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI action_GetRuntimeClassName( IAsyncAction *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Foundation.IAsyncAction", ARRAY_SIZE(L"Windows.Foundation.IAsyncAction") - 1, name );
}

static HRESULT WINAPI action_GetTrustLevel( IAsyncAction *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI action_put_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler *handler )
{
    struct action *impl = impl_from_IAsyncAction( iface );

    TRACE( "iface %p, handler %p.\n", iface, handler );
    if (impl->handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    if (!handler) return S_OK;
    impl->handler = add_ref_object( handler );
    IAsyncActionCompletedHandler_Invoke( handler, iface, Completed );
    return S_OK;
}

static HRESULT WINAPI action_get_Completed( IAsyncAction *iface, IAsyncActionCompletedHandler **handler )
{
    *handler = add_ref_object( impl_from_IAsyncAction( iface )->handler );
    return S_OK;
}

static HRESULT WINAPI action_GetResults( IAsyncAction *iface )
{
    return S_OK;
}

static const struct IAsyncActionVtbl action_vtbl =
{
    INSPECTABLE_ENTRIES( action ),
    action_put_Completed,
    action_get_Completed,
    action_GetResults,
};

static HRESULT WINAPI action_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    return action_QueryInterface( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, iid, out );
}

static ULONG WINAPI action_info_AddRef( IAsyncInfo *iface )
{
    return action_AddRef( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface );
}

static ULONG WINAPI action_info_Release( IAsyncInfo *iface )
{
    return action_Release( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface );
}

static HRESULT WINAPI action_info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids )
{
    return action_GetIids( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, count, iids );
}

static HRESULT WINAPI action_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name )
{
    return action_GetRuntimeClassName( &impl_from_IAsyncInfo( iface )->IAsyncAction_iface, name );
}

static HRESULT WINAPI action_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI action_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI action_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI action_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *code )
{
    *code = S_OK;
    return S_OK;
}

static HRESULT WINAPI action_info_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI action_info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const struct IAsyncInfoVtbl action_info_vtbl =
{
    action_info_QueryInterface,
    action_info_AddRef,
    action_info_Release,
    action_info_GetIids,
    action_info_GetRuntimeClassName,
    action_info_GetTrustLevel,
    action_info_get_Id,
    action_info_get_Status,
    action_info_get_ErrorCode,
    action_info_Cancel,
    action_info_Close,
};

static HRESULT completed_action_create( IAsyncAction **out )
{
    struct action *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAsyncAction_iface.lpVtbl = &action_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &action_info_vtbl;
    impl->ref = 1;
    *out = &impl->IAsyncAction_iface;
    return S_OK;
}

/*
 * The commands
 */

struct provider_command
{
    IWebAccountProviderCommand IWebAccountProviderCommand_iface;
    LONG ref;
    IWebAccountProvider *provider;
    IWebAccountProviderCommandInvokedHandler *invoked;
};

static void provider_command_destroy( struct provider_command *impl );
DEFINE_INSPECTABLE_BASE( provider_command, IWebAccountProviderCommand, struct provider_command, IWebAccountProviderCommand_iface,
                         L"Windows.UI.ApplicationSettings.WebAccountProviderCommand", IID_IWebAccountProviderCommand )

static HRESULT WINAPI provider_command_QueryInterface( IWebAccountProviderCommand *iface, REFIID iid, void **out )
{
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IWebAccountProviderCommand ))
    {
        *out = add_ref_object( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static void provider_command_destroy( struct provider_command *impl )
{
    release_object( impl->provider );
    release_object( impl->invoked );
    free( impl );
}

static HRESULT WINAPI provider_command_get_WebAccountProvider( IWebAccountProviderCommand *iface, IWebAccountProvider **value )
{
    *value = add_ref_object( provider_command_from( iface )->provider );
    return S_OK;
}

static HRESULT WINAPI provider_command_get_Invoked( IWebAccountProviderCommand *iface,
                                                    IWebAccountProviderCommandInvokedHandler **value )
{
    *value = add_ref_object( provider_command_from( iface )->invoked );
    return S_OK;
}

static const struct IWebAccountProviderCommandVtbl provider_command_vtbl =
{
    INSPECTABLE_ENTRIES( provider_command ),
    provider_command_get_WebAccountProvider,
    provider_command_get_Invoked,
};

struct account_command
{
    IWebAccountCommand IWebAccountCommand_iface;
    LONG ref;
    IWebAccount *account;
    IWebAccountCommandInvokedHandler *invoked;
    SupportedWebAccountActions actions;
};

static void account_command_destroy( struct account_command *impl );
DEFINE_INSPECTABLE_BASE( account_command, IWebAccountCommand, struct account_command, IWebAccountCommand_iface,
                         L"Windows.UI.ApplicationSettings.WebAccountCommand", IID_IWebAccountCommand )

static HRESULT WINAPI account_command_QueryInterface( IWebAccountCommand *iface, REFIID iid, void **out )
{
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IWebAccountCommand ))
    {
        *out = add_ref_object( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static void account_command_destroy( struct account_command *impl )
{
    release_object( impl->account );
    release_object( impl->invoked );
    free( impl );
}

static HRESULT WINAPI account_command_get_WebAccount( IWebAccountCommand *iface, IWebAccount **value )
{
    *value = add_ref_object( account_command_from( iface )->account );
    return S_OK;
}

static HRESULT WINAPI account_command_get_Invoked( IWebAccountCommand *iface, IWebAccountCommandInvokedHandler **value )
{
    *value = add_ref_object( account_command_from( iface )->invoked );
    return S_OK;
}

static HRESULT WINAPI account_command_get_Actions( IWebAccountCommand *iface, SupportedWebAccountActions *value )
{
    *value = account_command_from( iface )->actions;
    return S_OK;
}

static const struct IWebAccountCommandVtbl account_command_vtbl =
{
    INSPECTABLE_ENTRIES( account_command ),
    account_command_get_WebAccount,
    account_command_get_Invoked,
    account_command_get_Actions,
};

struct credential_command
{
    ICredentialCommand ICredentialCommand_iface;
    LONG ref;
    IPasswordCredential *credential;
    ICredentialCommandCredentialDeletedHandler *deleted;
};

static void credential_command_destroy( struct credential_command *impl );
DEFINE_INSPECTABLE_BASE( credential_command, ICredentialCommand, struct credential_command, ICredentialCommand_iface,
                         L"Windows.UI.ApplicationSettings.CredentialCommand", IID_ICredentialCommand )

static HRESULT WINAPI credential_command_QueryInterface( ICredentialCommand *iface, REFIID iid, void **out )
{
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_ICredentialCommand ))
    {
        *out = add_ref_object( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static void credential_command_destroy( struct credential_command *impl )
{
    release_object( impl->credential );
    release_object( impl->deleted );
    free( impl );
}

static HRESULT WINAPI credential_command_get_PasswordCredential( ICredentialCommand *iface, IPasswordCredential **value )
{
    *value = add_ref_object( credential_command_from( iface )->credential );
    return S_OK;
}

static HRESULT WINAPI credential_command_get_CredentialDeleted( ICredentialCommand *iface,
                                                                ICredentialCommandCredentialDeletedHandler **value )
{
    *value = add_ref_object( credential_command_from( iface )->deleted );
    return S_OK;
}

static const struct ICredentialCommandVtbl credential_command_vtbl =
{
    INSPECTABLE_ENTRIES( credential_command ),
    credential_command_get_PasswordCredential,
    credential_command_get_CredentialDeleted,
};

struct settings_command
{
    IUICommand IUICommand_iface;
    LONG ref;
    HSTRING label;
    IUICommandInvokedHandler *invoked;
    IInspectable *id;
};

static void settings_command_destroy( struct settings_command *impl );
DEFINE_INSPECTABLE_BASE( settings_command, IUICommand, struct settings_command, IUICommand_iface,
                         L"Windows.UI.ApplicationSettings.SettingsCommand", IID_IUICommand )

static HRESULT WINAPI settings_command_QueryInterface( IUICommand *iface, REFIID iid, void **out )
{
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IUICommand ))
    {
        *out = add_ref_object( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static void settings_command_destroy( struct settings_command *impl )
{
    WindowsDeleteString( impl->label );
    release_object( impl->invoked );
    release_object( impl->id );
    free( impl );
}

static HRESULT WINAPI settings_command_get_Label( IUICommand *iface, HSTRING *value )
{
    return WindowsDuplicateString( settings_command_from( iface )->label, value );
}

static HRESULT WINAPI settings_command_put_Label( IUICommand *iface, HSTRING value )
{
    struct settings_command *impl = settings_command_from( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->label );
    impl->label = copy;
    return S_OK;
}

static HRESULT WINAPI settings_command_get_Invoked( IUICommand *iface, IUICommandInvokedHandler **value )
{
    *value = add_ref_object( settings_command_from( iface )->invoked );
    return S_OK;
}

static HRESULT WINAPI settings_command_put_Invoked( IUICommand *iface, IUICommandInvokedHandler *value )
{
    struct settings_command *impl = settings_command_from( iface );
    IUICommandInvokedHandler *old = impl->invoked;

    impl->invoked = add_ref_object( value );
    release_object( old );
    return S_OK;
}

static HRESULT WINAPI settings_command_get_Id( IUICommand *iface, IInspectable **value )
{
    *value = add_ref_object( settings_command_from( iface )->id );
    return S_OK;
}

static HRESULT WINAPI settings_command_put_Id( IUICommand *iface, IInspectable *value )
{
    struct settings_command *impl = settings_command_from( iface );
    IInspectable *old = impl->id;

    impl->id = add_ref_object( value );
    release_object( old );
    return S_OK;
}

static const struct IUICommandVtbl settings_command_vtbl =
{
    INSPECTABLE_ENTRIES( settings_command ),
    settings_command_get_Label,
    settings_command_put_Label,
    settings_command_get_Invoked,
    settings_command_put_Invoked,
    settings_command_get_Id,
    settings_command_put_Id,
};

static HRESULT settings_command_create( IInspectable *id, HSTRING label, IUICommandInvokedHandler *handler, IUICommand **out )
{
    struct settings_command *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IUICommand_iface.lpVtbl = &settings_command_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( label, &impl->label )))
    {
        free( impl );
        return hr;
    }
    impl->invoked = add_ref_object( handler );
    impl->id = add_ref_object( id );
    *out = &impl->IUICommand_iface;
    return S_OK;
}

/*
 * The deferral and the arguments of AccountCommandsRequested
 */

struct deferral
{
    IAccountsSettingsPaneEventDeferral IAccountsSettingsPaneEventDeferral_iface;
    LONG ref;
    struct args *args;
};

struct args
{
    IAccountsSettingsPaneCommandsRequestedEventArgs IAccountsSettingsPaneCommandsRequestedEventArgs_iface;
    IAccountsSettingsPaneCommandsRequestedEventArgs2 IAccountsSettingsPaneCommandsRequestedEventArgs2_iface;
    LONG ref;
    LONG deferrals;
    HANDLE idle;
    IVector_IInspectable *provider_commands;
    IVector_IInspectable *account_commands;
    IVector_IInspectable *credential_commands;
    IVector_IInspectable *commands;
    HSTRING header;
};

static void deferral_destroy( struct deferral *impl );
DEFINE_INSPECTABLE_BASE( deferral, IAccountsSettingsPaneEventDeferral, struct deferral, IAccountsSettingsPaneEventDeferral_iface,
                         L"Windows.UI.ApplicationSettings.AccountsSettingsPaneEventDeferral", IID_IAccountsSettingsPaneEventDeferral )

static HRESULT WINAPI deferral_QueryInterface( IAccountsSettingsPaneEventDeferral *iface, REFIID iid, void **out )
{
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IAccountsSettingsPaneEventDeferral ))
    {
        *out = add_ref_object( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static void args_release_deferral( struct args *args );

static void deferral_destroy( struct deferral *impl )
{
    IAccountsSettingsPaneCommandsRequestedEventArgs_Release( &impl->args->IAccountsSettingsPaneCommandsRequestedEventArgs_iface );
    free( impl );
}

static HRESULT WINAPI deferral_Complete( IAccountsSettingsPaneEventDeferral *iface )
{
    struct deferral *impl = deferral_from( iface );

    TRACE( "iface %p.\n", iface );
    args_release_deferral( impl->args );
    return S_OK;
}

static const struct IAccountsSettingsPaneEventDeferralVtbl deferral_vtbl =
{
    INSPECTABLE_ENTRIES( deferral ),
    deferral_Complete,
};

static void args_release_deferral( struct args *args )
{
    if (!InterlockedDecrement( &args->deferrals )) SetEvent( args->idle );
}

static void args_destroy( struct args *impl );
DEFINE_INSPECTABLE_BASE( args, IAccountsSettingsPaneCommandsRequestedEventArgs, struct args,
                         IAccountsSettingsPaneCommandsRequestedEventArgs_iface,
                         L"Windows.UI.ApplicationSettings.AccountsSettingsPaneCommandsRequestedEventArgs",
                         IID_IAccountsSettingsPaneCommandsRequestedEventArgs )

static HRESULT WINAPI args_QueryInterface( IAccountsSettingsPaneCommandsRequestedEventArgs *iface, REFIID iid, void **out )
{
    struct args *impl = args_from( iface );

    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IAccountsSettingsPaneCommandsRequestedEventArgs ))
        *out = &impl->IAccountsSettingsPaneCommandsRequestedEventArgs_iface;
    else if (IsEqualGUID( iid, &IID_IAccountsSettingsPaneCommandsRequestedEventArgs2 ))
        *out = &impl->IAccountsSettingsPaneCommandsRequestedEventArgs2_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static void args_destroy( struct args *impl )
{
    release_object( impl->provider_commands );
    release_object( impl->account_commands );
    release_object( impl->credential_commands );
    release_object( impl->commands );
    WindowsDeleteString( impl->header );
    CloseHandle( impl->idle );
    free( impl );
}

static HRESULT WINAPI args_get_WebAccountProviderCommands( IAccountsSettingsPaneCommandsRequestedEventArgs *iface,
                                                           IVector_WebAccountProviderCommand **value )
{
    *value = add_ref_object( args_from( iface )->provider_commands );
    return S_OK;
}

static HRESULT WINAPI args_get_WebAccountCommands( IAccountsSettingsPaneCommandsRequestedEventArgs *iface,
                                                   IVector_WebAccountCommand **value )
{
    *value = add_ref_object( args_from( iface )->account_commands );
    return S_OK;
}

static HRESULT WINAPI args_get_CredentialCommands( IAccountsSettingsPaneCommandsRequestedEventArgs *iface,
                                                   IVector_CredentialCommand **value )
{
    *value = add_ref_object( args_from( iface )->credential_commands );
    return S_OK;
}

static HRESULT WINAPI args_get_Commands( IAccountsSettingsPaneCommandsRequestedEventArgs *iface, IVector_SettingsCommand **value )
{
    *value = add_ref_object( args_from( iface )->commands );
    return S_OK;
}

static HRESULT WINAPI args_get_HeaderText( IAccountsSettingsPaneCommandsRequestedEventArgs *iface, HSTRING *value )
{
    return WindowsDuplicateString( args_from( iface )->header, value );
}

static HRESULT WINAPI args_put_HeaderText( IAccountsSettingsPaneCommandsRequestedEventArgs *iface, HSTRING value )
{
    struct args *impl = args_from( iface );
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( impl->header );
    impl->header = copy;
    return S_OK;
}

static HRESULT WINAPI args_GetDeferral( IAccountsSettingsPaneCommandsRequestedEventArgs *iface,
                                        IAccountsSettingsPaneEventDeferral **deferral )
{
    struct args *impl = args_from( iface );
    struct deferral *object;

    TRACE( "iface %p, deferral %p.\n", iface, deferral );

    *deferral = NULL;
    if (!(object = calloc( 1, sizeof(*object) ))) return E_OUTOFMEMORY;
    object->IAccountsSettingsPaneEventDeferral_iface.lpVtbl = &deferral_vtbl;
    object->ref = 1;
    object->args = impl;
    IAccountsSettingsPaneCommandsRequestedEventArgs_AddRef( iface );
    if (InterlockedIncrement( &impl->deferrals ) == 1) ResetEvent( impl->idle );
    *deferral = &object->IAccountsSettingsPaneEventDeferral_iface;
    return S_OK;
}

static const struct IAccountsSettingsPaneCommandsRequestedEventArgsVtbl args_vtbl =
{
    INSPECTABLE_ENTRIES( args ),
    args_get_WebAccountProviderCommands,
    args_get_WebAccountCommands,
    args_get_CredentialCommands,
    args_get_Commands,
    args_get_HeaderText,
    args_put_HeaderText,
    args_GetDeferral,
};

static inline struct args *args_from_2( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface )
{
    return CONTAINING_RECORD( iface, struct args, IAccountsSettingsPaneCommandsRequestedEventArgs2_iface );
}

static HRESULT WINAPI args2_QueryInterface( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface, REFIID iid, void **out )
{
    return args_QueryInterface( &args_from_2( iface )->IAccountsSettingsPaneCommandsRequestedEventArgs_iface, iid, out );
}

static ULONG WINAPI args2_AddRef( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface )
{
    return args_AddRef( &args_from_2( iface )->IAccountsSettingsPaneCommandsRequestedEventArgs_iface );
}

static ULONG WINAPI args2_Release( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface )
{
    return args_Release( &args_from_2( iface )->IAccountsSettingsPaneCommandsRequestedEventArgs_iface );
}

static HRESULT WINAPI args2_GetIids( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface, ULONG *count, IID **iids )
{
    return args_GetIids( &args_from_2( iface )->IAccountsSettingsPaneCommandsRequestedEventArgs_iface, count, iids );
}

static HRESULT WINAPI args2_GetRuntimeClassName( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface, HSTRING *name )
{
    return args_GetRuntimeClassName( &args_from_2( iface )->IAccountsSettingsPaneCommandsRequestedEventArgs_iface, name );
}

static HRESULT WINAPI args2_GetTrustLevel( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI args2_get_User( IAccountsSettingsPaneCommandsRequestedEventArgs2 *iface, IUser **value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = NULL;   /* the pane of the process is not tied to a user object */
    return S_OK;
}

static const struct IAccountsSettingsPaneCommandsRequestedEventArgs2Vtbl args2_vtbl =
{
    INSPECTABLE_ENTRIES( args2 ),
    args2_get_User,
};

static const struct vector_iids provider_command_iids =
{
    &IID_IIterable_WebAccountProviderCommand, &IID_IIterator_WebAccountProviderCommand,
    &IID_IVector_WebAccountProviderCommand, &IID_IVectorView_WebAccountProviderCommand,
};
static const struct vector_iids account_command_iids =
{
    &IID_IIterable_WebAccountCommand, &IID_IIterator_WebAccountCommand,
    &IID_IVector_WebAccountCommand, &IID_IVectorView_WebAccountCommand,
};
static const struct vector_iids credential_command_iids =
{
    &IID_IIterable_CredentialCommand, &IID_IIterator_CredentialCommand,
    &IID_IVector_CredentialCommand, &IID_IVectorView_CredentialCommand,
};
static const struct vector_iids settings_command_iids =
{
    &IID_IIterable_SettingsCommand, &IID_IIterator_SettingsCommand,
    &IID_IVector_SettingsCommand, &IID_IVectorView_SettingsCommand,
};

static HRESULT args_create( struct args **out )
{
    struct args *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IAccountsSettingsPaneCommandsRequestedEventArgs_iface.lpVtbl = &args_vtbl;
    impl->IAccountsSettingsPaneCommandsRequestedEventArgs2_iface.lpVtbl = &args2_vtbl;
    impl->ref = 1;
    impl->idle = CreateEventW( NULL, TRUE, TRUE, NULL );
    if (FAILED(hr = vector_inspectable_create( &provider_command_iids, &impl->provider_commands )) ||
        FAILED(hr = vector_inspectable_create( &account_command_iids, &impl->account_commands )) ||
        FAILED(hr = vector_inspectable_create( &credential_command_iids, &impl->credential_commands )) ||
        FAILED(hr = vector_inspectable_create( &settings_command_iids, &impl->commands )))
    {
        args_destroy( impl );
        return hr;
    }
    *out = impl;
    return S_OK;
}

/*
 * The pane: one for each thread (the current view) or window, kept for the life of the process
 */

struct pane
{
    IAccountsSettingsPane IAccountsSettingsPane_iface;
    struct list entry;
    LONG ref;
    ULONG_PTR key;
    BOOL window;
    LONG next_token;
    struct handler
    {
        struct list entry;
        LONGLONG token;
        ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs *handler;
    } *handlers;
    UINT32 count;
};

static CRITICAL_SECTION panes_cs;
static CRITICAL_SECTION_DEBUG panes_cs_debug =
{
    0, 0, &panes_cs, { &panes_cs_debug.ProcessLocksList, &panes_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": panes_cs") }
};
static CRITICAL_SECTION panes_cs = { &panes_cs_debug, -1, 0, 0, 0, 0 };
static struct list panes = LIST_INIT( panes );

static void pane_destroy( struct pane *impl )
{
    /* a pane stays in the list; this is only reached by an object that never got there */
    free( impl );
}

DEFINE_INSPECTABLE_BASE( pane, IAccountsSettingsPane, struct pane, IAccountsSettingsPane_iface,
                         L"Windows.UI.ApplicationSettings.AccountsSettingsPane", IID_IAccountsSettingsPane )

static HRESULT WINAPI pane_QueryInterface( IAccountsSettingsPane *iface, REFIID iid, void **out )
{
    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IAccountsSettingsPane ))
    {
        *out = add_ref_object( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI pane_add_AccountCommandsRequested( IAccountsSettingsPane *iface,
        ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs *handler, EventRegistrationToken *cookie )
{
    struct pane *impl = pane_from( iface );
    struct handler *list;

    TRACE( "iface %p, handler %p, cookie %p.\n", iface, handler, cookie );

    if (!handler) return E_POINTER;
    EnterCriticalSection( &panes_cs );
    if (!(list = realloc( impl->handlers, (impl->count + 1) * sizeof(*list) )))
    {
        LeaveCriticalSection( &panes_cs );
        return E_OUTOFMEMORY;
    }
    impl->handlers = list;
    list[impl->count].token = InterlockedIncrement( &impl->next_token );
    list[impl->count].handler = add_ref_object( handler );
    cookie->value = list[impl->count].token;
    impl->count++;
    LeaveCriticalSection( &panes_cs );
    return S_OK;
}

static HRESULT WINAPI pane_remove_AccountCommandsRequested( IAccountsSettingsPane *iface, EventRegistrationToken cookie )
{
    struct pane *impl = pane_from( iface );
    UINT32 i;

    TRACE( "iface %p, cookie %I64d.\n", iface, cookie.value );

    EnterCriticalSection( &panes_cs );
    for (i = 0; i < impl->count; i++)
    {
        if (impl->handlers[i].token != cookie.value) continue;
        release_object( impl->handlers[i].handler );
        memmove( impl->handlers + i, impl->handlers + i + 1, (impl->count - i - 1) * sizeof(*impl->handlers) );
        impl->count--;
        break;
    }
    LeaveCriticalSection( &panes_cs );
    return S_OK;
}

static const struct IAccountsSettingsPaneVtbl pane_vtbl =
{
    INSPECTABLE_ENTRIES( pane ),
    pane_add_AccountCommandsRequested,
    pane_remove_AccountCommandsRequested,
};

/* the pane of a thread or a window; the caller owns a reference */
static HRESULT pane_get( BOOL window, ULONG_PTR key, struct pane **out )
{
    struct pane *impl;

    EnterCriticalSection( &panes_cs );
    LIST_FOR_EACH_ENTRY( impl, &panes, struct pane, entry )
    {
        if (impl->window == window && impl->key == key)
        {
            InterlockedIncrement( &impl->ref );
            *out = impl;
            LeaveCriticalSection( &panes_cs );
            return S_OK;
        }
    }
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        LeaveCriticalSection( &panes_cs );
        return E_OUTOFMEMORY;
    }
    impl->IAccountsSettingsPane_iface.lpVtbl = &pane_vtbl;
    impl->ref = 2;   /* the list and the caller */
    impl->key = key;
    impl->window = window;
    list_add_tail( &panes, &impl->entry );
    *out = impl;
    LeaveCriticalSection( &panes_cs );
    return S_OK;
}

/* what showing the pane does without a pane: ask the handlers for the commands */
static HRESULT pane_raise( struct pane *impl )
{
    ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs **handlers;
    struct args *args;
    UINT32 i, count;
    HRESULT hr;

    EnterCriticalSection( &panes_cs );
    count = impl->count;
    handlers = count ? malloc( count * sizeof(*handlers) ) : NULL;
    for (i = 0; handlers && i < count; i++) handlers[i] = add_ref_object( impl->handlers[i].handler );
    LeaveCriticalSection( &panes_cs );
    if (count && !handlers) return E_OUTOFMEMORY;

    if (SUCCEEDED(hr = args_create( &args )))
    {
        for (i = 0; i < count; i++)
        {
            ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs_Invoke(
                handlers[i], &impl->IAccountsSettingsPane_iface,
                &args->IAccountsSettingsPaneCommandsRequestedEventArgs_iface );
        }
        /* a handler that took a deferral completes it from where it likes; the commands are final when all are complete */
        if (args->deferrals) WaitForSingleObject( args->idle, 10000 );
        IAccountsSettingsPaneCommandsRequestedEventArgs_Release( &args->IAccountsSettingsPaneCommandsRequestedEventArgs_iface );
    }
    for (i = 0; i < count; i++) release_object( handlers[i] );
    free( handlers );
    return hr;
}

static HRESULT show_for( BOOL window, ULONG_PTR key, IAsyncAction **action )
{
    struct pane *pane;
    HRESULT hr;

    if (FAILED(hr = pane_get( window, key, &pane ))) return hr;
    hr = pane_raise( pane );
    IAccountsSettingsPane_Release( &pane->IAccountsSettingsPane_iface );
    TRACE( "no pane is drawn: the commands were asked for, hr %#lx.\n", hr );
    if (FAILED(hr) || !action) return hr;
    return completed_action_create( action );
}

/*
 * The activation factories
 */

struct statics
{
    IActivationFactory IActivationFactory_iface;
    IAccountsSettingsPaneStatics IAccountsSettingsPaneStatics_iface;
    IAccountsSettingsPaneStatics2 IAccountsSettingsPaneStatics2_iface;
    IAccountsSettingsPaneStatics3 IAccountsSettingsPaneStatics3_iface;
    IAccountsSettingsPaneInterop IAccountsSettingsPaneInterop_iface;
};

static struct statics statics_impl;

static inline struct statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct statics, IActivationFactory_iface );
}

static HRESULT WINAPI statics_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IAccountsSettingsPaneStatics )) *out = &impl->IAccountsSettingsPaneStatics_iface;
    else if (IsEqualGUID( iid, &IID_IAccountsSettingsPaneStatics2 )) *out = &impl->IAccountsSettingsPaneStatics2_iface;
    else if (IsEqualGUID( iid, &IID_IAccountsSettingsPaneStatics3 )) *out = &impl->IAccountsSettingsPaneStatics3_iface;
    else if (IsEqualGUID( iid, &IID_IAccountsSettingsPaneInterop )) *out = &impl->IAccountsSettingsPaneInterop_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI statics_factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI statics_factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI statics_factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( 3 * sizeof(*list) ))) return E_OUTOFMEMORY;
    list[0] = IID_IAccountsSettingsPaneStatics;
    list[1] = IID_IAccountsSettingsPaneStatics2;
    list[2] = IID_IAccountsSettingsPaneStatics3;
    *count = 3;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI statics_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.UI.ApplicationSettings.AccountsSettingsPane",
                                ARRAY_SIZE(L"Windows.UI.ApplicationSettings.AccountsSettingsPane") - 1, name );
}

static HRESULT WINAPI statics_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI statics_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl statics_factory_vtbl =
{
    statics_factory_QueryInterface,
    statics_factory_AddRef,
    statics_factory_Release,
    statics_factory_GetIids,
    statics_factory_GetRuntimeClassName,
    statics_factory_GetTrustLevel,
    statics_factory_ActivateInstance,
};

/* the statics interfaces share the factory's identity: each forwards to the factory object */
#define DEFINE_STATICS_FORWARD( pfx, itype, member, iid )                                                           \
static inline struct statics *pfx##_from( itype *iface )                                                            \
{                                                                                                                   \
    return CONTAINING_RECORD( iface, struct statics, member );                                                      \
}                                                                                                                   \
static HRESULT WINAPI pfx##_QueryInterface( itype *iface, REFIID iid, void **out )                                  \
{                                                                                                                   \
    return statics_factory_QueryInterface( &pfx##_from( iface )->IActivationFactory_iface, iid, out );              \
}                                                                                                                   \
static ULONG WINAPI pfx##_AddRef( itype *iface )                                                                    \
{                                                                                                                   \
    return 2;                                                                                                       \
}                                                                                                                   \
static ULONG WINAPI pfx##_Release( itype *iface )                                                                   \
{                                                                                                                   \
    return 1;                                                                                                       \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetIids( itype *iface, ULONG *count, IID **iids )                                       \
{                                                                                                                   \
    return statics_factory_GetIids( &pfx##_from( iface )->IActivationFactory_iface, count, iids );                  \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetRuntimeClassName( itype *iface, HSTRING *name )                                      \
{                                                                                                                   \
    return statics_factory_GetRuntimeClassName( &pfx##_from( iface )->IActivationFactory_iface, name );             \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetTrustLevel( itype *iface, TrustLevel *level )                                        \
{                                                                                                                   \
    *level = BaseTrust;                                                                                             \
    return S_OK;                                                                                                    \
}

DEFINE_STATICS_FORWARD( statics1, IAccountsSettingsPaneStatics, IAccountsSettingsPaneStatics_iface, IID_IAccountsSettingsPaneStatics )

static HRESULT WINAPI statics1_GetForCurrentView( IAccountsSettingsPaneStatics *iface, IAccountsSettingsPane **current )
{
    struct pane *pane;
    HRESULT hr;

    TRACE( "iface %p, current %p.\n", iface, current );

    if (FAILED(hr = pane_get( FALSE, GetCurrentThreadId(), &pane ))) return hr;
    *current = &pane->IAccountsSettingsPane_iface;
    return S_OK;
}

static HRESULT WINAPI statics1_Show( IAccountsSettingsPaneStatics *iface )
{
    TRACE( "iface %p.\n", iface );
    return show_for( FALSE, GetCurrentThreadId(), NULL );
}

static const struct IAccountsSettingsPaneStaticsVtbl statics1_vtbl =
{
    INSPECTABLE_ENTRIES( statics1 ),
    statics1_GetForCurrentView,
    statics1_Show,
};

DEFINE_STATICS_FORWARD( statics2, IAccountsSettingsPaneStatics2, IAccountsSettingsPaneStatics2_iface, IID_IAccountsSettingsPaneStatics2 )

static HRESULT WINAPI statics2_ShowManageAccountsAsync( IAccountsSettingsPaneStatics2 *iface, IAsyncAction **action )
{
    TRACE( "iface %p, action %p.\n", iface, action );
    return show_for( FALSE, GetCurrentThreadId(), action );
}

static HRESULT WINAPI statics2_ShowAddAccountAsync( IAccountsSettingsPaneStatics2 *iface, IAsyncAction **action )
{
    TRACE( "iface %p, action %p.\n", iface, action );
    return show_for( FALSE, GetCurrentThreadId(), action );
}

static const struct IAccountsSettingsPaneStatics2Vtbl statics2_vtbl =
{
    INSPECTABLE_ENTRIES( statics2 ),
    statics2_ShowManageAccountsAsync,
    statics2_ShowAddAccountAsync,
};

DEFINE_STATICS_FORWARD( statics3, IAccountsSettingsPaneStatics3, IAccountsSettingsPaneStatics3_iface, IID_IAccountsSettingsPaneStatics3 )

static HRESULT WINAPI statics3_ShowManageAccountsForUserAsync( IAccountsSettingsPaneStatics3 *iface, IUser *user,
                                                               IAsyncAction **action )
{
    TRACE( "iface %p, user %p, action %p.\n", iface, user, action );
    return show_for( FALSE, GetCurrentThreadId(), action );
}

static HRESULT WINAPI statics3_ShowAddAccountForUserAsync( IAccountsSettingsPaneStatics3 *iface, IUser *user,
                                                           IAsyncAction **action )
{
    TRACE( "iface %p, user %p, action %p.\n", iface, user, action );
    return show_for( FALSE, GetCurrentThreadId(), action );
}

static const struct IAccountsSettingsPaneStatics3Vtbl statics3_vtbl =
{
    INSPECTABLE_ENTRIES( statics3 ),
    statics3_ShowManageAccountsForUserAsync,
    statics3_ShowAddAccountForUserAsync,
};

DEFINE_STATICS_FORWARD( interop, IAccountsSettingsPaneInterop, IAccountsSettingsPaneInterop_iface, IID_IAccountsSettingsPaneInterop )

static HRESULT WINAPI interop_GetForWindow( IAccountsSettingsPaneInterop *iface, HWND window, REFIID iid, void **pane )
{
    struct pane *object;
    HRESULT hr;

    TRACE( "iface %p, window %p, iid %s, pane %p.\n", iface, window, debugstr_guid( iid ), pane );

    if (!pane) return E_POINTER;
    *pane = NULL;
    if (!IsWindow( window )) return E_INVALIDARG;
    if (FAILED(hr = pane_get( TRUE, (ULONG_PTR)window, &object ))) return hr;
    hr = IAccountsSettingsPane_QueryInterface( &object->IAccountsSettingsPane_iface, iid, pane );
    IAccountsSettingsPane_Release( &object->IAccountsSettingsPane_iface );
    return hr;
}

static HRESULT interop_show( HWND window, REFIID iid, void **out )
{
    IAsyncAction *action;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsWindow( window )) return E_INVALIDARG;
    if (FAILED(hr = show_for( TRUE, (ULONG_PTR)window, &action ))) return hr;
    hr = IAsyncAction_QueryInterface( action, iid, out );
    IAsyncAction_Release( action );
    return hr;
}

static HRESULT WINAPI interop_ShowManageAccountsForWindowAsync( IAccountsSettingsPaneInterop *iface, HWND window, REFIID iid,
                                                                void **action )
{
    TRACE( "iface %p, window %p, iid %s, action %p.\n", iface, window, debugstr_guid( iid ), action );
    return interop_show( window, iid, action );
}

static HRESULT WINAPI interop_ShowAddAccountForWindowAsync( IAccountsSettingsPaneInterop *iface, HWND window, REFIID iid,
                                                            void **action )
{
    TRACE( "iface %p, window %p, iid %s, action %p.\n", iface, window, debugstr_guid( iid ), action );
    return interop_show( window, iid, action );
}

static const struct IAccountsSettingsPaneInteropVtbl interop_vtbl =
{
    INSPECTABLE_ENTRIES( interop ),
    interop_GetForWindow,
    interop_ShowManageAccountsForWindowAsync,
    interop_ShowAddAccountForWindowAsync,
};

static struct statics statics_impl =
{
    {&statics_factory_vtbl},
    {&statics1_vtbl},
    {&statics2_vtbl},
    {&statics3_vtbl},
    {&interop_vtbl},
};

IActivationFactory *accountssettingspane_factory = &statics_impl.IActivationFactory_iface;

/* the factories of the commands: a factory interface next to IActivationFactory, which does not create anything */

struct command_factory
{
    IActivationFactory IActivationFactory_iface;
    IWebAccountProviderCommandFactory IWebAccountProviderCommandFactory_iface;
    IWebAccountCommandFactory IWebAccountCommandFactory_iface;
    ICredentialCommandFactory ICredentialCommandFactory_iface;
    ISettingsCommandFactory ISettingsCommandFactory_iface;
    ISettingsCommandStatics ISettingsCommandStatics_iface;
};

static struct command_factory provider_command_factory_impl, account_command_factory_impl, credential_command_factory_impl,
                              settings_command_factory_impl;

static inline struct command_factory *command_factory_from( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct command_factory, IActivationFactory_iface );
}

static HRESULT WINAPI command_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct command_factory *impl = command_factory_from( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (is_basic_iid( iid ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = &impl->IActivationFactory_iface;
    else if (impl == &provider_command_factory_impl && IsEqualGUID( iid, &IID_IWebAccountProviderCommandFactory ))
        *out = &impl->IWebAccountProviderCommandFactory_iface;
    else if (impl == &account_command_factory_impl && IsEqualGUID( iid, &IID_IWebAccountCommandFactory ))
        *out = &impl->IWebAccountCommandFactory_iface;
    else if (impl == &credential_command_factory_impl && IsEqualGUID( iid, &IID_ICredentialCommandFactory ))
        *out = &impl->ICredentialCommandFactory_iface;
    else if (impl == &settings_command_factory_impl && IsEqualGUID( iid, &IID_ISettingsCommandFactory ))
        *out = &impl->ISettingsCommandFactory_iface;
    else if (impl == &settings_command_factory_impl && IsEqualGUID( iid, &IID_ISettingsCommandStatics ))
        *out = &impl->ISettingsCommandStatics_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI command_factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI command_factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI command_factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI command_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    struct command_factory *impl = command_factory_from( iface );
    const WCHAR *text = L"Windows.UI.ApplicationSettings.SettingsCommand";

    if (impl == &provider_command_factory_impl) text = L"Windows.UI.ApplicationSettings.WebAccountProviderCommand";
    else if (impl == &account_command_factory_impl) text = L"Windows.UI.ApplicationSettings.WebAccountCommand";
    else if (impl == &credential_command_factory_impl) text = L"Windows.UI.ApplicationSettings.CredentialCommand";
    return WindowsCreateString( text, wcslen( text ), name );
}

static HRESULT WINAPI command_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI command_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl command_factory_vtbl =
{
    command_factory_QueryInterface,
    command_factory_AddRef,
    command_factory_Release,
    command_factory_GetIids,
    command_factory_GetRuntimeClassName,
    command_factory_GetTrustLevel,
    command_factory_ActivateInstance,
};

#define DEFINE_COMMAND_FORWARD( pfx, itype, member )                                                                \
static inline struct command_factory *pfx##_from( itype *iface )                                                    \
{                                                                                                                   \
    return CONTAINING_RECORD( iface, struct command_factory, member );                                              \
}                                                                                                                   \
static HRESULT WINAPI pfx##_QueryInterface( itype *iface, REFIID iid, void **out )                                  \
{                                                                                                                   \
    return command_factory_QueryInterface( &pfx##_from( iface )->IActivationFactory_iface, iid, out );              \
}                                                                                                                   \
static ULONG WINAPI pfx##_AddRef( itype *iface )                                                                    \
{                                                                                                                   \
    return 2;                                                                                                       \
}                                                                                                                   \
static ULONG WINAPI pfx##_Release( itype *iface )                                                                   \
{                                                                                                                   \
    return 1;                                                                                                       \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetIids( itype *iface, ULONG *count, IID **iids )                                       \
{                                                                                                                   \
    return command_factory_GetIids( &pfx##_from( iface )->IActivationFactory_iface, count, iids );                  \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetRuntimeClassName( itype *iface, HSTRING *name )                                      \
{                                                                                                                   \
    return command_factory_GetRuntimeClassName( &pfx##_from( iface )->IActivationFactory_iface, name );             \
}                                                                                                                   \
static HRESULT WINAPI pfx##_GetTrustLevel( itype *iface, TrustLevel *level )                                        \
{                                                                                                                   \
    *level = BaseTrust;                                                                                             \
    return S_OK;                                                                                                    \
}

DEFINE_COMMAND_FORWARD( provider_factory, IWebAccountProviderCommandFactory, IWebAccountProviderCommandFactory_iface )

static HRESULT WINAPI provider_factory_CreateWebAccountProviderCommand( IWebAccountProviderCommandFactory *iface,
        IWebAccountProvider *provider, IWebAccountProviderCommandInvokedHandler *invoked, IWebAccountProviderCommand **instance )
{
    struct provider_command *impl;

    TRACE( "iface %p, provider %p, invoked %p, instance %p.\n", iface, provider, invoked, instance );

    *instance = NULL;
    if (!provider) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebAccountProviderCommand_iface.lpVtbl = &provider_command_vtbl;
    impl->ref = 1;
    impl->provider = add_ref_object( provider );
    impl->invoked = add_ref_object( invoked );
    *instance = &impl->IWebAccountProviderCommand_iface;
    return S_OK;
}

static const struct IWebAccountProviderCommandFactoryVtbl provider_factory_vtbl =
{
    INSPECTABLE_ENTRIES( provider_factory ),
    provider_factory_CreateWebAccountProviderCommand,
};

DEFINE_COMMAND_FORWARD( account_factory, IWebAccountCommandFactory, IWebAccountCommandFactory_iface )

static HRESULT WINAPI account_factory_CreateWebAccountCommand( IWebAccountCommandFactory *iface, IWebAccount *account,
        IWebAccountCommandInvokedHandler *invoked, SupportedWebAccountActions actions, IWebAccountCommand **instance )
{
    struct account_command *impl;

    TRACE( "iface %p, account %p, invoked %p, actions %#x, instance %p.\n", iface, account, invoked, actions, instance );

    *instance = NULL;
    if (!account) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IWebAccountCommand_iface.lpVtbl = &account_command_vtbl;
    impl->ref = 1;
    impl->account = add_ref_object( account );
    impl->invoked = add_ref_object( invoked );
    impl->actions = actions;
    *instance = &impl->IWebAccountCommand_iface;
    return S_OK;
}

static const struct IWebAccountCommandFactoryVtbl account_factory_vtbl =
{
    INSPECTABLE_ENTRIES( account_factory ),
    account_factory_CreateWebAccountCommand,
};

DEFINE_COMMAND_FORWARD( credential_factory, ICredentialCommandFactory, ICredentialCommandFactory_iface )

static HRESULT credential_command_create( IPasswordCredential *credential, ICredentialCommandCredentialDeletedHandler *deleted,
                                          ICredentialCommand **instance )
{
    struct credential_command *impl;

    *instance = NULL;
    if (!credential) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICredentialCommand_iface.lpVtbl = &credential_command_vtbl;
    impl->ref = 1;
    impl->credential = add_ref_object( credential );
    impl->deleted = add_ref_object( deleted );
    *instance = &impl->ICredentialCommand_iface;
    return S_OK;
}

static HRESULT WINAPI credential_factory_CreateCredentialCommand( ICredentialCommandFactory *iface,
        IPasswordCredential *credential, ICredentialCommand **instance )
{
    TRACE( "iface %p, credential %p, instance %p.\n", iface, credential, instance );
    return credential_command_create( credential, NULL, instance );
}

static HRESULT WINAPI credential_factory_CreateCredentialCommandWithHandler( ICredentialCommandFactory *iface,
        IPasswordCredential *credential, ICredentialCommandCredentialDeletedHandler *deleted, ICredentialCommand **instance )
{
    TRACE( "iface %p, credential %p, deleted %p, instance %p.\n", iface, credential, deleted, instance );
    return credential_command_create( credential, deleted, instance );
}

static const struct ICredentialCommandFactoryVtbl credential_factory_vtbl =
{
    INSPECTABLE_ENTRIES( credential_factory ),
    credential_factory_CreateCredentialCommand,
    credential_factory_CreateCredentialCommandWithHandler,
};

DEFINE_COMMAND_FORWARD( settings_factory, ISettingsCommandFactory, ISettingsCommandFactory_iface )

static HRESULT WINAPI settings_factory_CreateSettingsCommand( ISettingsCommandFactory *iface, IInspectable *id, HSTRING label,
                                                              IUICommandInvokedHandler *handler, IUICommand **instance )
{
    IUICommand *command;
    HRESULT hr;

    TRACE( "iface %p, id %p, label %s, handler %p, instance %p.\n", iface, id, debugstr_hstring( label ), handler, instance );

    *instance = NULL;
    if (FAILED(hr = settings_command_create( id, label, handler, &command ))) return hr;
    *instance = command;
    return S_OK;
}

static const struct ISettingsCommandFactoryVtbl settings_factory_vtbl =
{
    INSPECTABLE_ENTRIES( settings_factory ),
    settings_factory_CreateSettingsCommand,
};

DEFINE_COMMAND_FORWARD( settings_statics, ISettingsCommandStatics, ISettingsCommandStatics_iface )

static HRESULT WINAPI settings_statics_get_AccountsCommand( ISettingsCommandStatics *iface, IUICommand **value )
{
    HSTRING label;
    IUICommand *command;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    *value = NULL;
    if (FAILED(hr = WindowsCreateString( L"Accounts", 8, &label ))) return hr;
    hr = settings_command_create( NULL, label, NULL, &command );
    WindowsDeleteString( label );
    if (FAILED(hr)) return hr;
    *value = command;
    return S_OK;
}

static const struct ISettingsCommandStaticsVtbl settings_statics_vtbl =
{
    INSPECTABLE_ENTRIES( settings_statics ),
    settings_statics_get_AccountsCommand,
};

static struct command_factory provider_command_factory_impl =
{
    {&command_factory_vtbl}, {&provider_factory_vtbl}, {NULL}, {NULL}, {NULL}, {NULL},
};
static struct command_factory account_command_factory_impl =
{
    {&command_factory_vtbl}, {NULL}, {&account_factory_vtbl}, {NULL}, {NULL}, {NULL},
};
static struct command_factory credential_command_factory_impl =
{
    {&command_factory_vtbl}, {NULL}, {NULL}, {&credential_factory_vtbl}, {NULL}, {NULL},
};
static struct command_factory settings_command_factory_impl =
{
    {&command_factory_vtbl}, {NULL}, {NULL}, {NULL}, {&settings_factory_vtbl}, {&settings_statics_vtbl},
};

IActivationFactory *webaccountprovidercommand_factory = &provider_command_factory_impl.IActivationFactory_iface;
IActivationFactory *webaccountcommand_factory = &account_command_factory_impl.IActivationFactory_iface;
IActivationFactory *credentialcommand_factory = &credential_command_factory_impl.IActivationFactory_iface;
IActivationFactory *settingscommand_factory = &settings_command_factory_impl.IActivationFactory_iface;
