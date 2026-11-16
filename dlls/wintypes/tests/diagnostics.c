/*
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

#define COBJMACROS
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Diagnostics
#include "windows.foundation.h"   /* its IIDs are defined in wintypes.c */
#include "initguid.h"
#include "windows.foundation.diagnostics.h"

#include "wine/test.h"

static HSTRING string( const WCHAR *s )
{
    HSTRING str = NULL;
    WindowsCreateString( s, wcslen( s ), &str );
    return str;
}

static void *factory( const WCHAR *class_name, REFIID iid )
{
    IActivationFactory *factory;
    HSTRING str = string( class_name );
    void *out = NULL;
    HRESULT hr;

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "%s: got hr %#lx.\n", debugstr_w( class_name ), hr );
    if (hr != S_OK) return NULL;
    if (iid == &IID_IActivationFactory) return factory;
    hr = IActivationFactory_QueryInterface( factory, iid, &out );
    ok( hr == S_OK, "%s: got hr %#lx.\n", debugstr_w( class_name ), hr );
    IActivationFactory_Release( factory );
    return out;
}

static void test_LoggingChannelOptions(void)
{
    static const GUID group = {0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
    ILoggingChannelOptionsFactory *options_factory;
    ILoggingChannelOptions *options;
    IActivationFactory *activation;
    IInspectable *inspectable;
    GUID value;
    HRESULT hr;

    if (!(activation = factory( L"Windows.Foundation.Diagnostics.LoggingChannelOptions", &IID_IActivationFactory ))) return;
    hr = IActivationFactory_ActivateInstance( activation, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( inspectable, &IID_ILoggingChannelOptions, (void **)&options );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    memset( &value, 0xcc, sizeof(value) );
    hr = ILoggingChannelOptions_get_Group( options, &value );
    ok( hr == S_OK && IsEqualGUID( &value, &GUID_NULL ), "got hr %#lx, group %s.\n", hr, debugstr_guid( &value ) );
    hr = ILoggingChannelOptions_put_Group( options, group );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannelOptions_get_Group( options, &value );
    ok( hr == S_OK && IsEqualGUID( &value, &group ), "got hr %#lx, group %s.\n", hr, debugstr_guid( &value ) );
    ILoggingChannelOptions_Release( options );
    IInspectable_Release( inspectable );

    hr = IActivationFactory_QueryInterface( activation, &IID_ILoggingChannelOptionsFactory, (void **)&options_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannelOptionsFactory_Create( options_factory, group, &options );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannelOptions_get_Group( options, &value );
    ok( hr == S_OK && IsEqualGUID( &value, &group ), "got hr %#lx, group %s.\n", hr, debugstr_guid( &value ) );
    ILoggingChannelOptions_Release( options );
    ILoggingChannelOptionsFactory_Release( options_factory );
    IActivationFactory_Release( activation );
}

static void test_LoggingOptions(void)
{
    ILoggingOptionsFactory *options_factory;
    IActivationFactory *activation;
    IInspectable *inspectable;
    ILoggingOptions *options;
    LoggingOpcode opcode;
    INT64 keywords;
    INT32 tags;
    INT16 task;
    GUID id;
    HRESULT hr;

    if (!(activation = factory( L"Windows.Foundation.Diagnostics.LoggingOptions", &IID_IActivationFactory ))) return;
    hr = IActivationFactory_ActivateInstance( activation, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( inspectable, &IID_ILoggingOptions, (void **)&options );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    keywords = tags = task = opcode = 0x55;
    memset( &id, 0xcc, sizeof(id) );
    ILoggingOptions_get_Keywords( options, &keywords );
    ILoggingOptions_get_Tags( options, &tags );
    ILoggingOptions_get_Task( options, &task );
    ILoggingOptions_get_Opcode( options, &opcode );
    hr = ILoggingOptions_get_ActivityId( options, &id );
    ok( hr == S_OK && !keywords && !tags && !task && opcode == LoggingOpcode_Info && IsEqualGUID( &id, &GUID_NULL ),
        "got hr %#lx, keywords %#I64x tags %#x task %d opcode %d activity %s.\n", hr, keywords, tags, task, opcode,
        debugstr_guid( &id ) );
    ILoggingOptions_Release( options );
    IInspectable_Release( inspectable );

    hr = IActivationFactory_QueryInterface( activation, &IID_ILoggingOptionsFactory, (void **)&options_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingOptionsFactory_CreateWithKeywords( options_factory, 0x0000400000000000, &options );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingOptions_get_Keywords( options, &keywords );
    ok( hr == S_OK && keywords == 0x0000400000000000, "got hr %#lx, keywords %#I64x.\n", hr, keywords );
    ILoggingOptions_Release( options );
    ILoggingOptionsFactory_Release( options_factory );
    IActivationFactory_Release( activation );
}

static void test_LoggingFields(void)
{
    static const BYTE bytes[] = {1, 2, 3};
    IActivationFactory *activation;
    IInspectable *inspectable;
    ILoggingFields *fields;
    HSTRING name, value;
    HRESULT hr;

    if (!(activation = factory( L"Windows.Foundation.Diagnostics.LoggingFields", &IID_IActivationFactory ))) return;
    hr = IActivationFactory_ActivateInstance( activation, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( inspectable, &IID_ILoggingFields, (void **)&fields );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    name = string( L"name" );
    value = string( L"value" );
    hr = ILoggingFields_AddString( fields, name, value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_AddInt32( fields, name, 7 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_AddBoolean( fields, name, TRUE );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_AddUInt64WithFormat( fields, name, 5, LoggingFieldFormat_Hexadecimal );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_AddUInt8Array( fields, name, ARRAY_SIZE(bytes), (BYTE *)bytes );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_AddString( fields, NULL, value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_EndStruct( fields );
    ok( hr == E_UNEXPECTED, "got hr %#lx.\n", hr );
    hr = ILoggingFields_BeginStruct( fields, name );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_EndStruct( fields );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingFields_Clear( fields );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( name );
    WindowsDeleteString( value );
    ILoggingFields_Release( fields );
    IInspectable_Release( inspectable );
    IActivationFactory_Release( activation );
}

static void test_LoggingChannel(void)
{
    static const GUID group = {0x4f50731a, 0x89cf, 0x4782, {0xb3, 0xe0, 0xdc, 0xe8, 0xc9, 0x04, 0x76, 0xba}};
    static const GUID id = {0x0905ca09, 0x610e, 0x401e, {0xb6, 0x50, 0x2f, 0x21, 0x29, 0x80, 0xb9, 0xe0}};
    static const GUID compatibility_id = {0x4bd2826e, 0x54a1, 0x4ba9, {0xbf, 0x63, 0x92, 0xb7, 0x3e, 0xa1, 0xac, 0x4a}};
    static const GUID name_id = {0x240d2c99, 0x9252, 0x550c, {0x77, 0x6f, 0x1a, 0xb4, 0x4e, 0x79, 0xb1, 0x42}};
    ILoggingChannelOptionsFactory *options_factory;
    ILoggingChannelFactory2 *channel_factory2;
    ILoggingChannelFactory *channel_factory;
    ILoggingChannelOptions *options;
    ILoggingActivity *activity;
    ILoggingChannel2 *channel2;
    ILoggingChannel *channel;
    ILoggingTarget *target;
    IClosable *closable;
    LoggingLevel level;
    HSTRING name, str;
    boolean enabled;
    GUID value;
    HRESULT hr;

    if (!(options_factory = factory( L"Windows.Foundation.Diagnostics.LoggingChannelOptions", &IID_ILoggingChannelOptionsFactory ))) return;
    hr = ILoggingChannelOptionsFactory_Create( options_factory, group, &options );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ILoggingChannelOptionsFactory_Release( options_factory );
    if (!(channel_factory2 = factory( L"Windows.Foundation.Diagnostics.LoggingChannel", &IID_ILoggingChannelFactory2 ))) return;

    name = string( L"Microsoft.Windows.Calculator.Test" );
    hr = ILoggingChannelFactory2_CreateWithOptionsAndId( channel_factory2, name, (void *)options, id, (void *)&channel );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannel_get_Name( channel, &str );
    ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Microsoft.Windows.Calculator.Test" ), "got %s.\n",
        debugstr_hstring( str ) );
    WindowsDeleteString( str );
    /* enabled while a tracing session listens to the channel (Windows' own telemetry may) */
    enabled = 2;
    hr = ILoggingChannel_get_Enabled( channel, &enabled );
    ok( hr == S_OK && enabled <= 1, "got hr %#lx, enabled %d.\n", hr, enabled );
    level = 0x55;
    hr = ILoggingChannel_get_Level( channel, &level );
    ok( hr == S_OK && level == LoggingLevel_Verbose, "got hr %#lx, level %d.\n", hr, level );
    hr = ILoggingChannel_LogMessage( channel, name );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannel_QueryInterface( channel, &IID_ILoggingChannel2, (void **)&channel2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannel2_get_Id( channel2, &value );
    ok( hr == S_OK && IsEqualGUID( &value, &id ), "got hr %#lx, id %s.\n", hr, debugstr_guid( &value ) );
    ILoggingChannel2_Release( channel2 );

    hr = ILoggingChannel_QueryInterface( channel, &IID_ILoggingTarget, (void **)&target );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    enabled = 2;
    hr = ILoggingTarget_IsEnabled( target, &enabled );
    ok( hr == S_OK && enabled <= 1, "got hr %#lx, enabled %d.\n", hr, enabled );
    hr = ILoggingTarget_LogEvent( target, name );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    /* an activity has the start event's name and an id of its own */
    activity = NULL;
    hr = ILoggingTarget_StartActivity( target, name, (void *)&activity );
    ok( hr == S_OK && activity, "got hr %#lx, activity %p.\n", hr, activity );
    if (activity)
    {
        memset( &value, 0, sizeof(value) );
        hr = ILoggingActivity_get_Id( activity, &value );
        ok( hr == S_OK && !IsEqualGUID( &value, &GUID_NULL ), "got hr %#lx, id %s.\n", hr, debugstr_guid( &value ) );
        hr = ILoggingActivity_get_Name( activity, &str );
        ok( hr == S_OK && !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"Microsoft.Windows.Calculator.Test" ), "got %s.\n",
            debugstr_hstring( str ) );
        WindowsDeleteString( str );
        ILoggingActivity_Release( activity );
    }
    ILoggingTarget_Release( target );

    hr = ILoggingChannel_QueryInterface( channel, &IID_IClosable, (void **)&closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IClosable_Close( closable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IClosable_Release( closable );
    ILoggingChannel_Release( channel );

    /* a channel created from its name only: the id comes from the name */
    hr = ILoggingChannelFactory2_QueryInterface( channel_factory2, &IID_ILoggingChannelFactory, (void **)&channel_factory );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannelFactory_Create( channel_factory, name, (void *)&channel );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ILoggingChannel_QueryInterface( channel, &IID_ILoggingChannel2, (void **)&channel2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    /* Windows 8.1 compatibility mode: the LoggingChannel provider's own id */
    hr = ILoggingChannel2_get_Id( channel2, &value );
    ok( hr == S_OK && IsEqualGUID( &value, &compatibility_id ), "got hr %#lx, id %s.\n", hr, debugstr_guid( &value ) );
    ILoggingChannel2_Release( channel2 );
    ILoggingChannel_Release( channel );
    /* with options and no id: the id ETW providers derive from their name */
    hr = ILoggingChannelFactory2_CreateWithOptions( channel_factory2, name, NULL, (void *)&channel );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        hr = ILoggingChannel_QueryInterface( channel, &IID_ILoggingChannel2, (void **)&channel2 );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = ILoggingChannel2_get_Id( channel2, &value );
        ok( hr == S_OK && IsEqualGUID( &value, &name_id ), "got hr %#lx, id %s.\n", hr, debugstr_guid( &value ) );
        ILoggingChannel2_Release( channel2 );
        ILoggingChannel_Release( channel );
    }
    ILoggingChannelFactory_Release( channel_factory );
    ILoggingChannelFactory2_Release( channel_factory2 );
    ILoggingChannelOptions_Release( options );
    WindowsDeleteString( name );
}

static HRESULT WINAPI status_handler_QueryInterface( IEventHandler_TracingStatusChangedEventArgs *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IEventHandler_TracingStatusChangedEventArgs ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI status_handler_AddRef( IEventHandler_TracingStatusChangedEventArgs *iface ) { return 2; }
static ULONG WINAPI status_handler_Release( IEventHandler_TracingStatusChangedEventArgs *iface ) { return 1; }

static HRESULT WINAPI status_handler_Invoke( IEventHandler_TracingStatusChangedEventArgs *iface, IInspectable *sender,
                                             ITracingStatusChangedEventArgs *args )
{
    return S_OK;
}

static const IEventHandler_TracingStatusChangedEventArgsVtbl status_handler_vtbl =
{
    status_handler_QueryInterface,
    status_handler_AddRef,
    status_handler_Release,
    status_handler_Invoke,
};

static IEventHandler_TracingStatusChangedEventArgs status_handler = {&status_handler_vtbl};

static void test_AsyncCausalityTracer(void)
{
    static const GUID platform = {0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
    IAsyncCausalityTracerStatics *statics;
    EventRegistrationToken token;
    HSTRING name;
    HRESULT hr;

    if (!(statics = factory( L"Windows.Foundation.Diagnostics.AsyncCausalityTracer", &IID_IAsyncCausalityTracerStatics ))) return;
    name = string( L"operation" );
    hr = IAsyncCausalityTracerStatics_TraceOperationCreation( statics, CausalityTraceLevel_Required, CausalitySource_Application,
                                                               platform, 1, name, 0 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncCausalityTracerStatics_TraceOperationRelation( statics, CausalityTraceLevel_Important, CausalitySource_Library,
                                                               platform, 1, CausalityRelation_Join );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncCausalityTracerStatics_TraceSynchronousWorkStart( statics, CausalityTraceLevel_Verbose, CausalitySource_System,
                                                                  platform, 1, CausalitySynchronousWork_Execution );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncCausalityTracerStatics_TraceSynchronousWorkCompletion( statics, CausalityTraceLevel_Verbose, CausalitySource_System,
                                                                       CausalitySynchronousWork_Execution );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncCausalityTracerStatics_TraceOperationCompletion( statics, CausalityTraceLevel_Required, CausalitySource_Application,
                                                                 platform, 1, Completed );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( name );

    token.value = 0;
    hr = IAsyncCausalityTracerStatics_add_TracingStatusChanged( statics, &status_handler, &token );
    ok( hr == S_OK && token.value, "got hr %#lx, token %#I64x.\n", hr, token.value );
    hr = IAsyncCausalityTracerStatics_remove_TracingStatusChanged( statics, token );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IAsyncCausalityTracerStatics_add_TracingStatusChanged( statics, NULL, &token );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    IAsyncCausalityTracerStatics_Release( statics );
}

START_TEST(diagnostics)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    test_LoggingChannelOptions();
    test_LoggingOptions();
    test_LoggingFields();
    test_LoggingChannel();
    test_AsyncCausalityTracer();
    RoUninitialize();
}
