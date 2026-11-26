/*
 * Tests for Windows.UI.ApplicationSettings.AccountsSettingsPane (WineExplorer UWP)
 *
 * This file owns the interface identifiers of the test module (it includes initguid.h; the other files do not).
 *
 * Not run on Windows: the expectations are those of the documented API, and what the implementation does without a pane
 * (showing it asks for the commands and completes; no window is drawn).
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

#define COBJMACROS
#include "initguid.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_System
#include "windows.system.h"
#define WIDL_using_Windows_UI
#include "windows.ui.h"
#define WIDL_using_Windows_UI_ViewManagement
#include "windows.ui.viewmanagement.h"
#include "uiviewsettingsinterop.h"
#include "weakreference.h"
#define WIDL_using_Windows_UI_Popups
#include "windows.ui.popups.h"
#define WIDL_using_Windows_UI_ApplicationSettings
#include "windows.ui.applicationsettings.h"
#include "accountssettingspaneinterop.h"

#include "wine/test.h"

#define check_interface( obj, iid, exp ) check_interface_( __LINE__, obj, iid, exp )
static void check_interface_( unsigned int line, void *obj, const IID *iid, BOOL supported )
{
    IUnknown *iface = obj;
    IUnknown *unk;
    HRESULT hr, expected_hr;

    expected_hr = supported ? S_OK : E_NOINTERFACE;

    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_( __FILE__, line )( hr == expected_hr, "Got hr %#lx, expected %#lx.\n", hr, expected_hr );
    if (SUCCEEDED(hr)) IUnknown_Release( unk );
}

static const WCHAR *settings_command_class = L"Windows.UI.ApplicationSettings.SettingsCommand";

struct handler
{
    ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs
        ITypedEventHandler_iface;
    LONG ref;
    LONG invoked;
    BOOL defer;
    HANDLE deferral_done;
    IAccountsSettingsPane *last_sender;
};

static struct handler *impl_from_handler( ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct handler, ITypedEventHandler_iface );
}

static HRESULT WINAPI handler_QueryInterface(
        ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs ))
    {
        IUnknown_AddRef( iface );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI handler_AddRef(
        ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs *iface )
{
    return InterlockedIncrement( &impl_from_handler( iface )->ref );
}

static ULONG WINAPI handler_Release(
        ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs *iface )
{
    return InterlockedDecrement( &impl_from_handler( iface )->ref );
}

static DWORD CALLBACK complete_deferral( void *param )
{
    IAccountsSettingsPaneEventDeferral *deferral = param;
    HRESULT hr;

    Sleep( 200 );
    hr = IAccountsSettingsPaneEventDeferral_Complete( deferral );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAccountsSettingsPaneEventDeferral_Release( deferral );
    return 0;
}

static HRESULT WINAPI handler_Invoke(
        ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgs *iface,
        IAccountsSettingsPane *sender, IAccountsSettingsPaneCommandsRequestedEventArgs *args )
{
    struct handler *impl = impl_from_handler( iface );
    IAccountsSettingsPaneCommandsRequestedEventArgs2 *args2;
    IVector_WebAccountProviderCommand *provider_commands;
    IVector_WebAccountCommand *account_commands;
    IVector_CredentialCommand *credential_commands;
    IVector_SettingsCommand *commands;
    IAccountsSettingsPaneEventDeferral *deferral;
    IActivationFactory *factory;
    ISettingsCommandFactory *command_factory;
    IUICommand *command;
    HSTRING str, id;
    UINT32 size;
    IUser *user;
    HRESULT hr;

    InterlockedIncrement( &impl->invoked );
    impl->last_sender = sender;

    hr = IAccountsSettingsPaneCommandsRequestedEventArgs_get_WebAccountProviderCommands( args, &provider_commands );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    size = 0xdeadbeef;
    hr = IVector_WebAccountProviderCommand_get_Size( provider_commands, &size );
    ok( hr == S_OK && !size, "got hr %#lx, size %u.\n", hr, size );
    IVector_WebAccountProviderCommand_Release( provider_commands );

    hr = IAccountsSettingsPaneCommandsRequestedEventArgs_get_WebAccountCommands( args, &account_commands );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IVector_WebAccountCommand_Release( account_commands );
    hr = IAccountsSettingsPaneCommandsRequestedEventArgs_get_CredentialCommands( args, &credential_commands );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IVector_CredentialCommand_Release( credential_commands );

    /* the handler fills in the commands the application offers */
    hr = WindowsCreateString( settings_command_class, wcslen( settings_command_class ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_ISettingsCommandFactory, (void **)&command_factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsCreateString( L"Privacy", 7, &str );
    id = NULL;
    hr = ISettingsCommandFactory_CreateSettingsCommand( command_factory, NULL, str, NULL, &command );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    ISettingsCommandFactory_Release( command_factory );
    hr = IAccountsSettingsPaneCommandsRequestedEventArgs_get_Commands( args, &commands );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVector_SettingsCommand_Append( commands, command );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    size = 0;
    hr = IVector_SettingsCommand_get_Size( commands, &size );
    ok( hr == S_OK && size == 1, "got hr %#lx, size %u.\n", hr, size );
    IVector_SettingsCommand_Release( commands );
    IUICommand_Release( command );
    (void)factory;
    (void)id;

    WindowsCreateString( L"Choose an account", 17, &str );
    hr = IAccountsSettingsPaneCommandsRequestedEventArgs_put_HeaderText( args, str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = IAccountsSettingsPaneCommandsRequestedEventArgs_get_HeaderText( args, &str );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Choose an account" ), "got hr %#lx, text %s.\n",
        hr, debugstr_hstring( str ) );
    WindowsDeleteString( str );

    hr = IAccountsSettingsPaneCommandsRequestedEventArgs_QueryInterface( args, &IID_IAccountsSettingsPaneCommandsRequestedEventArgs2,
                                                                         (void **)&args2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    user = (void *)0xdeadbeef;
    hr = IAccountsSettingsPaneCommandsRequestedEventArgs2_get_User( args2, &user );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (user && user != (void *)0xdeadbeef) IUser_Release( user );
    IAccountsSettingsPaneCommandsRequestedEventArgs2_Release( args2 );

    if (impl->defer)
    {
        HANDLE thread;

        hr = IAccountsSettingsPaneCommandsRequestedEventArgs_GetDeferral( args, &deferral );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        thread = CreateThread( NULL, 0, complete_deferral, deferral, 0, NULL );
        CloseHandle( thread );
    }
    return S_OK;
}

static const ITypedEventHandler_AccountsSettingsPane_AccountsSettingsPaneCommandsRequestedEventArgsVtbl handler_vtbl =
{
    handler_QueryInterface,
    handler_AddRef,
    handler_Release,
    handler_Invoke,
};

static void check_completed_action( IAsyncAction *action )
{
    IAsyncInfo *info;
    AsyncStatus status;
    HRESULT hr;

    hr = IAsyncAction_QueryInterface( action, &IID_IAsyncInfo, (void **)&info );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncInfo_get_Status( info, &status );
    ok( hr == S_OK && status == Completed, "got hr %#lx, status %d.\n", hr, status );
    IAsyncInfo_Release( info );
    hr = IAsyncAction_GetResults( action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
}

static void test_AccountsSettingsPane(void)
{
    static const WCHAR *class_name = L"Windows.UI.ApplicationSettings.AccountsSettingsPane";
    IAccountsSettingsPaneStatics *statics;
    IAccountsSettingsPaneStatics2 *statics2;
    IAccountsSettingsPaneStatics3 *statics3;
    IAccountsSettingsPaneInterop *interop;
    IAccountsSettingsPane *pane, *pane2, *window_pane;
    IActivationFactory *factory;
    IAsyncAction *action;
    struct handler handler = { { &handler_vtbl }, 1 };
    EventRegistrationToken token;
    HWND window;
    HSTRING str;
    HRESULT hr;

    WindowsCreateString( class_name, wcslen( class_name ), &str );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr != S_OK)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( class_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown, TRUE );
    check_interface( factory, &IID_IInspectable, TRUE );
    check_interface( factory, &IID_IActivationFactory, TRUE );
    check_interface( factory, &IID_IAccountsSettingsPaneStatics, TRUE );
    check_interface( factory, &IID_IAccountsSettingsPaneStatics2, TRUE );
    check_interface( factory, &IID_IAccountsSettingsPaneStatics3, TRUE );
    check_interface( factory, &IID_IAccountsSettingsPaneInterop, TRUE );

    hr = IActivationFactory_QueryInterface( factory, &IID_IAccountsSettingsPaneStatics, (void **)&statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IActivationFactory_QueryInterface( factory, &IID_IAccountsSettingsPaneStatics2, (void **)&statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IActivationFactory_QueryInterface( factory, &IID_IAccountsSettingsPaneStatics3, (void **)&statics3 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IActivationFactory_QueryInterface( factory, &IID_IAccountsSettingsPaneInterop, (void **)&interop );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    /* the pane of the current view: the same one each time, with the handler list of its own */
    hr = IAccountsSettingsPaneStatics_GetForCurrentView( statics, &pane );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAccountsSettingsPaneStatics_GetForCurrentView( statics, &pane2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( pane == pane2, "got different panes %p, %p.\n", pane, pane2 );
    IAccountsSettingsPane_Release( pane2 );
    check_interface( pane, &IID_IInspectable, TRUE );
    check_interface( pane, &IID_IAccountsSettingsPane, TRUE );

    /* showing the pane asks for the commands, once for each handler, and completes without a window */
    hr = IAccountsSettingsPane_add_AccountCommandsRequested( pane, &handler.ITypedEventHandler_iface, &token );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( token.value, "got no token.\n" );
    hr = IAccountsSettingsPaneStatics_Show( statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( handler.invoked == 1, "handler invoked %ld times.\n", handler.invoked );
    ok( handler.last_sender == pane, "got sender %p.\n", handler.last_sender );

    hr = IAccountsSettingsPaneStatics2_ShowAddAccountAsync( statics2, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_completed_action( action );
    IAsyncAction_Release( action );
    ok( handler.invoked == 2, "handler invoked %ld times.\n", handler.invoked );

    hr = IAccountsSettingsPaneStatics2_ShowManageAccountsAsync( statics2, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_completed_action( action );
    IAsyncAction_Release( action );
    ok( handler.invoked == 3, "handler invoked %ld times.\n", handler.invoked );

    hr = IAccountsSettingsPaneStatics3_ShowAddAccountForUserAsync( statics3, NULL, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );
    ok( handler.invoked == 4, "handler invoked %ld times.\n", handler.invoked );

    /* a deferral holds the pane back until it is complete */
    handler.defer = TRUE;
    hr = IAccountsSettingsPaneStatics2_ShowAddAccountAsync( statics2, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );
    ok( handler.invoked == 5, "handler invoked %ld times.\n", handler.invoked );
    handler.defer = FALSE;

    hr = IAccountsSettingsPane_remove_AccountCommandsRequested( pane, token );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAccountsSettingsPaneStatics_Show( statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( handler.invoked == 5, "handler invoked %ld times.\n", handler.invoked );
    IAccountsSettingsPane_Release( pane );

    /* a window has a pane of its own */
    window = CreateWindowW( L"static", L"accounts", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL );
    ok( window != NULL, "CreateWindowW failed, error %lu.\n", GetLastError() );
    hr = IAccountsSettingsPaneInterop_GetForWindow( interop, window, &IID_IAccountsSettingsPane, (void **)&window_pane );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAccountsSettingsPaneInterop_GetForWindow( interop, window, &IID_IAccountsSettingsPane, (void **)&pane2 );
    ok( hr == S_OK && pane2 == window_pane, "got hr %#lx, panes %p, %p.\n", hr, window_pane, pane2 );
    IAccountsSettingsPane_Release( pane2 );
    hr = IAccountsSettingsPaneStatics_GetForCurrentView( statics, &pane );
    ok( hr == S_OK && pane != window_pane, "got hr %#lx, panes %p, %p.\n", hr, pane, window_pane );
    IAccountsSettingsPane_Release( pane );
    hr = IAccountsSettingsPaneInterop_GetForWindow( interop, window, &IID_IInspectable, (void **)&pane2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAccountsSettingsPane_Release( pane2 );

    hr = IAccountsSettingsPane_add_AccountCommandsRequested( window_pane, &handler.ITypedEventHandler_iface, &token );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAccountsSettingsPaneInterop_ShowAddAccountForWindowAsync( interop, window, &IID_IAsyncAction, (void **)&action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_completed_action( action );
    IAsyncAction_Release( action );
    ok( handler.invoked == 6, "handler invoked %ld times.\n", handler.invoked );
    ok( handler.last_sender == window_pane, "got sender %p.\n", handler.last_sender );
    hr = IAccountsSettingsPaneInterop_ShowManageAccountsForWindowAsync( interop, window, &IID_IAsyncAction, (void **)&action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );
    ok( handler.invoked == 7, "handler invoked %ld times.\n", handler.invoked );
    IAccountsSettingsPane_remove_AccountCommandsRequested( window_pane, token );
    IAccountsSettingsPane_Release( window_pane );

    hr = IAccountsSettingsPaneInterop_GetForWindow( interop, (HWND)0xdead, &IID_IAccountsSettingsPane, (void **)&pane2 );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    DestroyWindow( window );

    IAccountsSettingsPaneInterop_Release( interop );
    IAccountsSettingsPaneStatics3_Release( statics3 );
    IAccountsSettingsPaneStatics2_Release( statics2 );
    IAccountsSettingsPaneStatics_Release( statics );
    IActivationFactory_Release( factory );
}

static void test_SettingsCommand(void)
{
    ISettingsCommandStatics *statics;
    ISettingsCommandFactory *factory;
    IUICommand *command;
    HSTRING str, label;
    IInspectable *id;
    HRESULT hr;

    WindowsCreateString( settings_command_class, wcslen( settings_command_class ), &str );
    hr = RoGetActivationFactory( str, &IID_ISettingsCommandStatics, (void **)&statics );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr != S_OK)
    {
        WindowsDeleteString( str );
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( settings_command_class ) );
        return;
    }
    hr = RoGetActivationFactory( str, &IID_ISettingsCommandFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = ISettingsCommandStatics_get_AccountsCommand( statics, &command );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_interface( command, &IID_IInspectable, TRUE );
    check_interface( command, &IID_IUICommand, TRUE );
    hr = IUICommand_get_Label( command, &label );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( label );
    IUICommand_Release( command );

    WindowsCreateString( L"About", 5, &label );
    hr = ISettingsCommandFactory_CreateSettingsCommand( factory, NULL, label, NULL, &command );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( label );
    hr = IUICommand_get_Label( command, &label );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( label, NULL ), L"About" ), "got hr %#lx, label %s.\n", hr,
        debugstr_hstring( label ) );
    WindowsDeleteString( label );
    WindowsCreateString( L"Renamed", 7, &label );
    hr = IUICommand_put_Label( command, label );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( label );
    hr = IUICommand_get_Label( command, &label );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( label, NULL ), L"Renamed" ), "got hr %#lx, label %s.\n", hr,
        debugstr_hstring( label ) );
    WindowsDeleteString( label );
    id = (void *)0xdeadbeef;
    hr = IUICommand_get_Id( command, &id );
    ok( hr == S_OK && !id, "got hr %#lx, id %p.\n", hr, id );
    IUICommand_Release( command );

    ISettingsCommandFactory_Release( factory );
    ISettingsCommandStatics_Release( statics );
}

START_TEST(accountssettings)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_AccountsSettingsPane();
    test_SettingsCommand();

    RoUninitialize();
}
