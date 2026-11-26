/*
 * Tests for Windows.ApplicationModel.UserActivities.UserActivityChannel and Windows.UI.StartScreen.JumpList
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
#include <stddef.h>

#include "windef.h"
#include "winbase.h"

#define COBJMACROS
#include "winstring.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_ApplicationModel_UserActivities
#include "windows.applicationmodel.useractivities.h"
#define WIDL_using_Windows_UI_StartScreen
#include "windows.ui.startscreen.h"

#include "wine/test.h"

static void *get_factory( const WCHAR *class_name, REFIID iid )
{
    HSTRING str;
    void *factory = NULL;
    HRESULT hr;

    WindowsCreateString( class_name, wcslen( class_name ), &str );
    hr = RoGetActivationFactory( str, iid, &factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "%s: got hr %#lx.\n", debugstr_w( class_name ), hr );
    return factory;
}

static HSTRING hstr( const WCHAR *str )
{
    HSTRING value;

    WindowsCreateString( str, wcslen( str ), &value );
    return value;
}

static BOOL hstr_equals( HSTRING value, const WCHAR *expected )
{
    return !wcscmp( WindowsGetStringRawBuffer( value, NULL ), expected );
}

static void test_UserActivityChannel(void)
{
    IUserActivityChannelStatics2 *statics2;
    IUserActivityChannelStatics *statics;
    IAsyncOperation_UserActivity *operation;
    IUserActivityVisualElements *visual;
    IUserActivityChannel *channel, *other;
    IUserActivitySession *session;
    IUserActivity *activity, *again;
    IUserActivity3 *activity3;
    UserActivityState state;
    IAsyncAction *action;
    boolean roamable;
    HSTRING id, text;
    ULONG count;
    IID *iids;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.ApplicationModel.UserActivities.UserActivityChannel",
                                 &IID_IUserActivityChannelStatics )))
    {
        win_skip( "UserActivityChannel runtimeclass not registered, skipping tests.\n" );
        return;
    }

    hr = IUserActivityChannelStatics_GetDefault( statics, &channel );
    ok( hr == S_OK && channel, "got hr %#lx.\n", hr );
    hr = IUserActivityChannel_GetIids( channel, &count, &iids );
    ok( hr == S_OK && count == 1 && IsEqualGUID( &iids[0], &IID_IUserActivityChannel ), "got hr %#lx, %lu iids.\n", hr, count );
    CoTaskMemFree( iids );

    hr = IUserActivityChannelStatics_QueryInterface( statics, &IID_IUserActivityChannelStatics2, (void **)&statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IUserActivityChannelStatics2_DisableAutoSessionCreation( statics2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    other = (void *)0xdeadbeef;
    hr = IUserActivityChannelStatics2_TryGetForWebAccount( statics2, NULL, &other );
    ok( hr == S_OK && !other, "got hr %#lx, channel %p.\n", hr, other );
    IUserActivityChannelStatics2_Release( statics2 );

    id = hstr( L"wine-test-activity" );
    hr = IUserActivityChannel_DeleteAllActivitiesAsync( channel, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );

    hr = IUserActivityChannel_GetOrCreateUserActivityAsync( channel, NULL, &operation );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    hr = IUserActivityChannel_GetOrCreateUserActivityAsync( channel, id, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncOperation_UserActivity_GetResults( operation, &activity );
    ok( hr == S_OK && activity, "got hr %#lx.\n", hr );
    IAsyncOperation_UserActivity_Release( operation );

    hr = IUserActivity_get_State( activity, &state );
    ok( hr == S_OK && state == UserActivityState_New, "got hr %#lx, state %d.\n", hr, state );
    hr = IUserActivity_get_ActivityId( activity, &text );
    ok( hr == S_OK && hstr_equals( text, L"wine-test-activity" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( text );

    hr = IUserActivity_QueryInterface( activity, &IID_IUserActivity3, (void **)&activity3 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IUserActivity3_put_IsRoamable( activity3, TRUE );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    roamable = FALSE;
    hr = IUserActivity3_get_IsRoamable( activity3, &roamable );
    ok( hr == S_OK && roamable, "got hr %#lx, roamable %d.\n", hr, roamable );
    IUserActivity3_Release( activity3 );

    hr = IUserActivity_get_VisualElements( activity, &visual );
    ok( hr == S_OK && visual, "got hr %#lx.\n", hr );
    text = hstr( L"Calculation" );
    hr = IUserActivityVisualElements_put_DisplayText( visual, text );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    hr = IUserActivityVisualElements_get_DisplayText( visual, &text );
    ok( hr == S_OK && hstr_equals( text, L"Calculation" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    IUserActivityVisualElements_Release( visual );

    hr = IUserActivity_SaveAsync( activity, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncAction_GetResults( action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );
    /* saving leaves the activity New and the channel gives a new one; a session publishes it */
    hr = IUserActivity_get_State( activity, &state );
    ok( hr == S_OK && state == UserActivityState_New, "got hr %#lx, state %d.\n", hr, state );
    hr = IUserActivityChannel_GetOrCreateUserActivityAsync( channel, id, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncOperation_UserActivity_GetResults( operation, &again );
    ok( hr == S_OK && again != activity, "got hr %#lx, %p.\n", hr, again );
    IUserActivity_Release( again );
    IAsyncOperation_UserActivity_Release( operation );

    hr = IUserActivity_CreateSession( activity, &session );
    ok( hr == S_OK && session, "got hr %#lx.\n", hr );
    hr = IUserActivity_get_State( activity, &state );
    ok( hr == S_OK && state == UserActivityState_Published, "got hr %#lx, state %d.\n", hr, state );
    hr = IUserActivitySession_get_ActivityId( session, &text );
    ok( hr == S_OK && hstr_equals( text, L"wine-test-activity" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    IUserActivitySession_Release( session );

    /* a published activity is returned again */
    hr = IUserActivityChannel_GetOrCreateUserActivityAsync( channel, id, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncOperation_UserActivity_GetResults( operation, &again );
    ok( hr == S_OK && again == activity, "got hr %#lx, %p, expected %p.\n", hr, again, activity );
    IUserActivity_Release( again );
    IAsyncOperation_UserActivity_Release( operation );

    /* and a deleted one is not */
    hr = IUserActivityChannel_DeleteActivityAsync( channel, id, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );
    hr = IUserActivityChannel_GetOrCreateUserActivityAsync( channel, id, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncOperation_UserActivity_GetResults( operation, &again );
    ok( hr == S_OK && again != activity, "got hr %#lx, %p.\n", hr, again );
    hr = IUserActivity_get_State( again, &state );
    ok( hr == S_OK && state == UserActivityState_New, "got hr %#lx, state %d.\n", hr, state );
    IUserActivity_Release( again );
    IAsyncOperation_UserActivity_Release( operation );

    WindowsDeleteString( id );
    IUserActivity_Release( activity );
    IUserActivityChannel_Release( channel );
    IUserActivityChannelStatics_Release( statics );
}

static void test_JumpList(void)
{
    IVector_JumpListItem *items, *items2;
    IAsyncOperation_JumpList *operation;
    IJumpListItemStatics *item_statics;
    IJumpListItem *item, *separator, *loaded;
    JumpListSystemGroupKind group;
    IJumpListStatics *statics;
    IAsyncAction *action;
    IJumpList *list;
    JumpListItemKind kind;
    boolean supported;
    HSTRING text, name;
    UINT32 size;
    HRESULT hr;

    if (!(statics = get_factory( L"Windows.UI.StartScreen.JumpList", &IID_IJumpListStatics )))
    {
        win_skip( "JumpList runtimeclass not registered, skipping tests.\n" );
        return;
    }
    item_statics = get_factory( L"Windows.UI.StartScreen.JumpListItem", &IID_IJumpListItemStatics );
    ok( !!item_statics, "no JumpListItem factory.\n" );

    hr = IJumpListStatics_IsSupported( statics, &supported );
    ok( hr == S_OK && supported, "got hr %#lx, supported %d.\n", hr, supported );

    text = hstr( L"--new" );
    name = hstr( L"New window" );
    hr = IJumpListItemStatics_CreateWithArguments( item_statics, text, name, &item );
    ok( hr == S_OK && item, "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    WindowsDeleteString( name );
    hr = IJumpListItem_get_Kind( item, &kind );
    ok( hr == S_OK && kind == JumpListItemKind_Arguments, "got hr %#lx, kind %d.\n", hr, kind );
    hr = IJumpListItem_get_DisplayName( item, &text );
    ok( hr == S_OK && hstr_equals( text, L"New window" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    hr = IJumpListItem_get_Arguments( item, &text );
    ok( hr == S_OK && hstr_equals( text, L"--new" ), "got hr %#lx.\n", hr );
    WindowsDeleteString( text );
    text = hstr( L"Tasks" );
    hr = IJumpListItem_put_GroupName( item, text );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( text );

    hr = IJumpListItemStatics_CreateSeparator( item_statics, &separator );
    ok( hr == S_OK && separator, "got hr %#lx.\n", hr );
    hr = IJumpListItem_get_Kind( separator, &kind );
    ok( hr == S_OK && kind == JumpListItemKind_Separator, "got hr %#lx, kind %d.\n", hr, kind );

    hr = IJumpListStatics_LoadCurrentAsync( statics, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncOperation_JumpList_GetResults( operation, &list );
    ok( hr == S_OK && list, "got hr %#lx.\n", hr );
    IAsyncOperation_JumpList_Release( operation );

    hr = IJumpList_get_Items( list, &items );
    ok( hr == S_OK && items, "got hr %#lx.\n", hr );
    hr = IVector_JumpListItem_get_Size( items, &size );
    ok( hr == S_OK && !size, "got hr %#lx, size %u.\n", hr, size );
    hr = IVector_JumpListItem_Append( items, item );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVector_JumpListItem_Append( items, separator );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVector_JumpListItem_get_Size( items, &size );
    ok( hr == S_OK && size == 2, "got hr %#lx, size %u.\n", hr, size );
    hr = IVector_JumpListItem_GetAt( items, 2, &loaded );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    IVector_JumpListItem_Release( items );

    hr = IJumpList_get_SystemGroupKind( list, &group );
    ok( hr == S_OK && group == JumpListSystemGroupKind_Recent, "got hr %#lx, group %d.\n", hr, group );
    hr = IJumpList_put_SystemGroupKind( list, JumpListSystemGroupKind_Frequent );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJumpList_SaveAsync( list, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncAction_GetResults( action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );
    IJumpList_Release( list );

    /* a list loaded again has the saved items, as new objects */
    hr = IJumpListStatics_LoadCurrentAsync( statics, &operation );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncOperation_JumpList_GetResults( operation, &list );
    ok( hr == S_OK && list, "got hr %#lx.\n", hr );
    IAsyncOperation_JumpList_Release( operation );
    hr = IJumpList_get_SystemGroupKind( list, &group );
    ok( hr == S_OK && group == JumpListSystemGroupKind_Frequent, "got hr %#lx, group %d.\n", hr, group );
    hr = IJumpList_get_Items( list, &items2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVector_JumpListItem_get_Size( items2, &size );
    ok( hr == S_OK && size == 2, "got hr %#lx, size %u.\n", hr, size );
    {
        hr = IVector_JumpListItem_GetAt( items2, 0, &loaded );
        ok( hr == S_OK && loaded != item, "got hr %#lx.\n", hr );
        hr = IJumpListItem_get_GroupName( loaded, &text );
        ok( hr == S_OK && hstr_equals( text, L"Tasks" ), "got hr %#lx.\n", hr );
        WindowsDeleteString( text );
        IJumpListItem_Release( loaded );
        hr = IVector_JumpListItem_GetAt( items2, 1, &loaded );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = IJumpListItem_get_Kind( loaded, &kind );
        ok( hr == S_OK && kind == JumpListItemKind_Separator, "got hr %#lx, kind %d.\n", hr, kind );
        IJumpListItem_Release( loaded );
    }
    hr = IVector_JumpListItem_Clear( items2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IVector_JumpListItem_Release( items2 );
    hr = IJumpList_SaveAsync( list, &action );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAsyncAction_Release( action );
    IJumpList_Release( list );

    IJumpListItem_Release( separator );
    IJumpListItem_Release( item );
    IJumpListItemStatics_Release( item_statics );
    IJumpListStatics_Release( statics );
}

/* IUserActivityRequestManagerInterop {dd69f876-...}, the second interface of the factory (useractivityinterop.h): GetForWindow is the
 * method after the IInspectable ones. Microsoft.UI.Xaml calls it with its window. */
static void test_UserActivityRequestManagerInterop(void)
{
    static const GUID IID_interop = {0xdd69f876, 0x9699, 0x4715, {0x90, 0x95, 0xe3, 0x7e, 0xa3, 0x0d, 0xfa, 0x1b}};
    typedef HRESULT (WINAPI *get_for_window_t)( void *iface, HWND window, REFIID riid, void **value );
    static const WCHAR *class_name = L"Windows.ApplicationModel.UserActivities.UserActivityRequestManager";
    IUserActivityRequestManager *manager, *manager2;
    IActivationFactory *factory;
    get_for_window_t get_for_window;
    void *interop;
    HSTRING str;
    HWND hwnd;
    HRESULT hr;

    WindowsCreateString( class_name, wcslen( class_name ), &str );
    hr = RoGetActivationFactory( str, &IID_interop, &interop );
    WindowsDeleteString( str );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s is not available.\n", debugstr_w( class_name ) );
        return;
    }
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    hr = IUnknown_QueryInterface( (IUnknown *)interop, &IID_IActivationFactory, (void **)&factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) IActivationFactory_Release( factory );

    get_for_window = (*(get_for_window_t **)interop)[6];
    hwnd = CreateWindowW( L"static", L"interop", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL );
    ok( hwnd != NULL, "CreateWindow failed.\n" );

    manager = NULL;
    hr = get_for_window( interop, hwnd, &IID_IUserActivityRequestManager, (void **)&manager );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( manager != NULL, "got no manager.\n" );
    manager2 = NULL;
    hr = get_for_window( interop, hwnd, &IID_IUserActivityRequestManager, (void **)&manager2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( manager2 == manager, "got %p and %p.\n", manager, manager2 );
    if (manager2) IUserActivityRequestManager_Release( manager2 );
    if (manager) IUserActivityRequestManager_Release( manager );

    manager = (void *)0xdeadbeef;
    hr = get_for_window( interop, (HWND)0x12345678, &IID_IUserActivityRequestManager, (void **)&manager );
    ok( FAILED(hr), "got hr %#lx.\n", hr );
    ok( !manager, "got %p.\n", manager );

    DestroyWindow( hwnd );
    IUnknown_Release( (IUnknown *)interop );
}

START_TEST(activities)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_UserActivityChannel();
    test_JumpList();
    test_UserActivityRequestManagerInterop();

    RoUninitialize();
}
