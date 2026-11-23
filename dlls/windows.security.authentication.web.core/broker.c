/* WinRT Windows.Security.Authentication.Web.Core: the client of the token broker
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

/* With HKCU\Software\Wine\WebAuthentication "Broker" (REG_DWORD 1), token requests go to the user's token broker
 * (tokenbroker.exe, protocol in wine/tokenbroker.h), started here on demand. Off by default until the broker signs in
 * (WineExplorer docs/WAM_BROKER_PLAN.md, pattern 8). When the broker cannot be reached, or its answer is not valid,
 * the request is answered as without it, so programs with a browser sign-in of their own go on as before. */

#include "private.h"
#include "winreg.h"
#include "sddl.h"
#include "wine/tokenbroker.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(webcore);

#define BROKER_START_WAIT_MS 5000
/* how long an answer is waited for: a silent request may refresh a token over the network; an interactive one lasts
 * as long as the sign-in window, which the broker closes after ten minutes */
#define BROKER_SILENT_TIMEOUT_MS      (60 * 1000)
#define BROKER_INTERACTIVE_TIMEOUT_MS (11 * 60 * 1000)

BOOL token_broker_enabled( void )
{
    DWORD value = 0, size = sizeof(value);

    return !RegGetValueW( HKEY_CURRENT_USER, L"Software\\Wine\\WebAuthentication", L"Broker", RRF_RT_REG_DWORD, NULL,
                          &value, &size ) && value == 1;
}

/* the user of a process token, allocated */
static SID *token_user( HANDLE token )
{
    char buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *user = (TOKEN_USER *)buffer;
    DWORD len;
    SID *sid;

    if (!GetTokenInformation( token, TokenUser, buffer, sizeof(buffer), &len )) return NULL;
    len = GetLengthSid( user->User.Sid );
    if (!(sid = malloc( len ))) return NULL;
    CopySid( len, sid, user->User.Sid );
    return sid;
}

static SID *process_user( HANDLE process )
{
    HANDLE token;
    SID *sid;

    if (!OpenProcessToken( process, TOKEN_QUERY, &token )) return NULL;
    sid = token_user( token );
    CloseHandle( token );
    return sid;
}

/* is the pipe's server the machine's token broker, outside any AppContainer? The pipe exists only once a broker runs,
 * so a sandboxed process of the same user (a Store app) could create it first and answer for the broker: the user's
 * SID alone does not tell them apart. The image must be tokenbroker.exe in the system directory (system32 for the
 * 64-bit broker, syswow64 for the 32-bit one), which a user cannot write where the machine tree belongs to wexsys
 * (in a stock prefix it is the user's own folder, and the image check adds nothing). Its token must be a normal
 * user's: a sandbox that starts the real image suspended gives it the sandbox's token (below medium integrity or
 * restricted). Wine's sandboxes are the same Unix user: this is defence in depth, not a sandbox boundary. */
static BOOL token_is_normal( HANDLE token )
{
    BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    TOKEN_MANDATORY_LABEL *label = (TOKEN_MANDATORY_LABEL *)buffer;
    DWORD restricted = 1, len;
    SID *sid;

    if (!GetTokenInformation( token, TokenIsRestricted, &restricted, sizeof(restricted), &len ) || restricted)
        return FALSE;
    if (!GetTokenInformation( token, TokenIntegrityLevel, buffer, sizeof(buffer), &len )) return FALSE;
    sid = label->Label.Sid;
    if (!IsValidSid( sid ) || !*GetSidSubAuthorityCount( sid )) return FALSE;
    return *GetSidSubAuthority( sid, *GetSidSubAuthorityCount( sid ) - 1 ) >= SECURITY_MANDATORY_MEDIUM_RID;
}

static BOOL server_is_broker( HANDLE process, ULONG pid )
{
    static const WCHAR *const dirs[] = { L"\\system32\\tokenbroker.exe", L"\\syswow64\\tokenbroker.exe" };
    WCHAR image[MAX_PATH], expected[MAX_PATH];
    DWORD size = ARRAY_SIZE(image), container = 0, len;
    HANDLE token;
    UINT windir, i;

    if (!OpenProcessToken( process, TOKEN_QUERY, &token )) return FALSE;
    if (!GetTokenInformation( token, TokenIsAppContainer, &container, sizeof(container), &len )) container = 1;
    if (!container && !token_is_normal( token )) container = 1;
    CloseHandle( token );
    if (container)
    {
        WARN( "the pipe's server process %04lx runs in an AppContainer or with a sandbox's token\n", pid );
        return FALSE;
    }
    if (!QueryFullProcessImageNameW( process, 0, image, &size ))
    {
        WARN( "no image name for the pipe's server process %04lx: %lu\n", pid, GetLastError() );
        return FALSE;
    }
    if (!(windir = GetWindowsDirectoryW( expected, MAX_PATH )) || windir >= MAX_PATH) return FALSE;
    for (i = 0; i < ARRAY_SIZE(dirs); i++)
    {
        if (windir + wcslen( dirs[i] ) >= MAX_PATH) continue;
        wcscpy( expected + windir, dirs[i] );
        if (!wcsicmp( image, expected )) return TRUE;
    }
    WARN( "the pipe's server process %04lx is %s, not the token broker\n", pid, debugstr_w( image ) );
    return FALSE;
}

/* is the pipe's server a process of this program's user? (the pipe's name and descriptor say so; this checks it) */
static BOOL server_is_own_user( HANDLE pipe, const SID *own )
{
    HANDLE process;
    ULONG pid;
    SID *sid;
    BOOL ret = FALSE;

    if (!GetNamedPipeServerProcessId( pipe, &pid ))
    {
        WARN( "no server process id for the pipe: %lu\n", GetLastError() );
        return FALSE;
    }
    /* the query right, not only the limited one: with elevation enforced WineExplorer's server gives a process's token
     * only to a handle with PROCESS_QUERY_INFORMATION (server/token.c, open_token) */
    if (!(process = OpenProcess( PROCESS_QUERY_INFORMATION, FALSE, pid )))
    {
        WARN( "cannot open the pipe's server process %04lx: %lu\n", pid, GetLastError() );
        return FALSE;
    }
    if ((sid = process_user( process )))
    {
        ret = EqualSid( sid, (SID *)own ) && server_is_broker( process, pid );
        if (!ret && WARN_ON(webcore))
        {
            WCHAR *server_text = NULL, *own_text = NULL;

            ConvertSidToStringSidW( sid, &server_text );
            ConvertSidToStringSidW( (SID *)own, &own_text );
            WARN( "the pipe's server process %04lx runs as %s, this program as %s\n", pid, debugstr_w( server_text ),
                  debugstr_w( own_text ) );
            LocalFree( server_text );
            LocalFree( own_text );
        }
        free( sid );
    }
    else WARN( "cannot read the user of the pipe's server process %04lx: %lu\n", pid, GetLastError() );
    CloseHandle( process );
    return ret;
}

static BOOL start_broker( void )
{
    static const WCHAR name[] = L"\\tokenbroker.exe";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR path[MAX_PATH];
    UINT len;

    if (!(len = GetSystemDirectoryW( path, MAX_PATH )) || len + ARRAY_SIZE(name) > MAX_PATH) return FALSE;
    wcscat( path, name );
    if (!CreateProcessW( path, NULL, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi ))
    {
        WARN( "cannot start the token broker: %lu\n", GetLastError() );
        return FALSE;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return TRUE;
}

/* the user's broker, started when it does not run */
static HANDLE connect_broker( const SID *own )
{
    WCHAR name[256], *sid_string;
    DWORD mode = PIPE_READMODE_MESSAGE, start;
    BOOL started = FALSE;
    HANDLE pipe;

    if (!ConvertSidToStringSidW( (SID *)own, &sid_string )) return INVALID_HANDLE_VALUE;
    swprintf( name, ARRAY_SIZE(name), L"%s%s", TOKENBROKER_PIPE_PREFIX, sid_string );
    LocalFree( sid_string );
    start = GetTickCount();
    for (;;)
    {
        pipe = CreateFileW( name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL );
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW( name, 1000 );
        else if (GetLastError() == ERROR_FILE_NOT_FOUND && !started)
        {
            /* no broker to wait for: the call is answered as without one at once */
            if (!start_broker()) return INVALID_HANDLE_VALUE;
            started = TRUE;
        }
        else if (GetLastError() != ERROR_FILE_NOT_FOUND) return INVALID_HANDLE_VALUE;
        else Sleep( 50 );
        if (GetTickCount() - start > BROKER_START_WAIT_MS) return INVALID_HANDLE_VALUE;
    }
    if (!SetNamedPipeHandleState( pipe, &mode, NULL, NULL ) || !server_is_own_user( pipe, own ))
    {
        WARN( "the token broker's pipe is not the user's\n" );
        CloseHandle( pipe );
        return INVALID_HANDLE_VALUE;
    }
    return pipe;
}

/* one string of a request or reply: counted UTF-16 text */
struct field
{
    const WCHAR *text;
    UINT32 len;     /* characters */
};

static struct field hstring_field( HSTRING string )
{
    struct field field;
    field.text = WindowsGetStringRawBuffer( string, &field.len );
    return field;
}

/* the answer of the broker; the fields point into buffer */
struct broker_reply
{
    int status;
    UINT32 code;
    struct field field[TOKENBROKER_REPLY_COUNT];
    char *buffer;
    DWORD size;             /* of buffer, zeroed before it is freed: it held the token */
};

static void free_reply( struct broker_reply *reply )
{
    if (reply->buffer) SecureZeroMemory( reply->buffer, reply->size );
    free( reply->buffer );
    reply->buffer = NULL;
}

/* a request message to the broker, built where the call is made (the program may change its request afterwards) */
struct broker_message
{
    struct tokenbroker_request *req;
    DWORD size;
};

static void free_message( struct broker_message *msg )
{
    if (msg->req) SecureZeroMemory( msg->req, msg->size );
    free( msg->req );
    msg->req = NULL;
}

static HRESULT build_message( unsigned int kind, const struct field fields[TOKENBROKER_FIELD_COUNT],
                              WebTokenRequestPromptType prompt, HWND window, struct broker_message *msg )
{
    static LONG next_id;
    DWORD size = sizeof(*msg->req), offset;
    unsigned int i;

    msg->req = NULL;
    for (i = 0; i < TOKENBROKER_FIELD_COUNT; i++)
    {
        if (fields[i].len > (TOKENBROKER_MAX_MESSAGE - size) / sizeof(WCHAR)) return E_INVALIDARG;
        size += fields[i].len * sizeof(WCHAR);
    }
    if (!(msg->req = calloc( 1, size ))) return E_OUTOFMEMORY;
    msg->size = size;
    msg->req->hdr.magic = TOKENBROKER_MAGIC;
    msg->req->hdr.version = TOKENBROKER_VERSION;
    msg->req->hdr.kind = kind;
    msg->req->hdr.request_id = InterlockedIncrement( &next_id );
    msg->req->hdr.size = size;
    msg->req->prompt_type = prompt;
    msg->req->window = HandleToULong( window );
    for (i = 0, offset = sizeof(*msg->req); i < TOKENBROKER_FIELD_COUNT; i++)
    {
        msg->req->field_len[i] = fields[i].len * sizeof(WCHAR);
        memcpy( (char *)msg->req + offset, fields[i].text, msg->req->field_len[i] );
        offset += msg->req->field_len[i];
    }
    return S_OK;
}

/* the reply's fields, checked against its header */
static BOOL parse_reply( const struct tokenbroker_request *req, char *answer, DWORD size, struct broker_reply *out )
{
    const struct tokenbroker_reply *reply = (const struct tokenbroker_reply *)answer;
    DWORD offset = sizeof(*reply);
    unsigned int i;

    if (size < sizeof(*reply) || reply->hdr.magic != TOKENBROKER_MAGIC || reply->hdr.version != TOKENBROKER_VERSION ||
        reply->hdr.kind != req->hdr.kind || reply->hdr.request_id != req->hdr.request_id || reply->hdr.size != size)
        return FALSE;
    for (i = 0; i < TOKENBROKER_REPLY_COUNT; i++)
    {
        if (reply->field_len[i] % sizeof(WCHAR) || reply->field_len[i] > size - offset) return FALSE;
        out->field[i].text = (const WCHAR *)(answer + offset);
        out->field[i].len = reply->field_len[i] / sizeof(WCHAR);
        offset += reply->field_len[i];
    }
    if (offset != size) return FALSE;
    out->status = reply->status;
    out->code = reply->error_code;
    return TRUE;
}

/* The message to the user's broker and its reply, waited for up to timeout ms. S_OK with the reply (free_reply), or a
 * failure: then the call is answered as without a broker. */
static HRESULT transact( const struct broker_message *msg, DWORD timeout, struct broker_reply *out )
{
    HANDLE token, pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED ov = {0};
    char *answer = NULL;
    SID *own = NULL;
    HRESULT hr = E_FAIL;
    DWORD size = 0;
    BOOL ok;

    memset( out, 0, sizeof(*out) );
    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token )) goto done;
    own = token_user( token );
    CloseHandle( token );
    if (!own || !(answer = malloc( TOKENBROKER_MAX_MESSAGE )) ||
        !(ov.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL )))
        goto done;
    if ((pipe = connect_broker( own )) == INVALID_HANDLE_VALUE) goto done;

    ok = TransactNamedPipe( pipe, msg->req, msg->size, answer, TOKENBROKER_MAX_MESSAGE, &size, &ov );
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject( ov.hEvent, timeout ) != WAIT_OBJECT_0)
        {
            WARN( "the token broker did not answer within %lu ms\n", timeout );
            CancelIoEx( pipe, &ov );
        }
        ok = GetOverlappedResult( pipe, &ov, &size, TRUE );
    }
    if (!ok) goto done;
    if (!parse_reply( msg->req, answer, size, out ))
    {
        WARN( "the token broker's answer is not valid\n" );
        goto done;
    }
    out->buffer = answer;
    out->size = size;
    answer = NULL;
    /* lengths only: tokens are never logged */
    TRACE( "request %u kind %u: status %d, error %#x, token length %u\n", msg->req->hdr.request_id,
           msg->req->hdr.kind, out->status, out->code, out->field[TOKENBROKER_REPLY_TOKEN].len );
    hr = S_OK;

done:
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle( pipe );
    if (ov.hEvent) CloseHandle( ov.hEvent );
    if (answer) SecureZeroMemory( answer, TOKENBROKER_MAX_MESSAGE );
    free( answer );
    free( own );
    if (FAILED(hr)) WARN( "the token broker was not reached; answering without it\n" );
    return hr;
}

/* one exchange that needs no user: an account lookup */
static HRESULT broker_exchange( unsigned int kind, const struct field fields[TOKENBROKER_FIELD_COUNT],
                                struct broker_reply *out )
{
    struct broker_message msg;
    HRESULT hr;

    memset( out, 0, sizeof(*out) );
    if (FAILED(hr = build_message( kind, fields, WebTokenRequestPromptType_Default, NULL, &msg ))) return hr;
    hr = transact( &msg, BROKER_SILENT_TIMEOUT_MS, out );
    free_message( &msg );
    return hr;
}

/* the next NUL-terminated string of a pairs or list field, or FALSE at its end */
static BOOL next_string( struct field *rest, struct field *out )
{
    const WCHAR *end = rest->text, *limit = rest->text + rest->len;

    while (end < limit && *end) end++;
    if (end == limit) return FALSE;
    out->text = rest->text;
    out->len = end - rest->text;
    rest->text = end + 1;
    rest->len -= out->len + 1;
    return TRUE;
}

static HRESULT properties_from_pairs( struct field pairs, IMap_HSTRING_HSTRING **out )
{
    struct field key, value;
    HSTRING hkey, hvalue;
    boolean replaced;
    HRESULT hr;

    if (FAILED(hr = map_hstring_create( out ))) return hr;
    while (next_string( &pairs, &key ) && next_string( &pairs, &value ))
    {
        if (FAILED(hr = WindowsCreateString( key.text, key.len, &hkey ))) break;
        if (SUCCEEDED(hr = WindowsCreateString( value.text, value.len, &hvalue )))
        {
            hr = IMap_HSTRING_HSTRING_Insert( *out, hkey, hvalue, &replaced );
            WindowsDeleteString( hvalue );
        }
        WindowsDeleteString( hkey );
        if (FAILED(hr)) break;
    }
    if (FAILED(hr))
    {
        IMap_HSTRING_HSTRING_Release( *out );
        *out = NULL;
    }
    return hr;
}

/* the provider's id and authority as request fields (the HSTRINGs are the caller's to free) */
static void provider_fields( IWebAccountProvider *provider, HSTRING *id, HSTRING *authority )
{
    IWebAccountProvider2 *provider2;

    *id = *authority = NULL;
    if (!provider) return;
    IWebAccountProvider_get_Id( provider, id );
    if (SUCCEEDED(IWebAccountProvider_QueryInterface( provider, &IID_IWebAccountProvider2, (void **)&provider2 )))
    {
        IWebAccountProvider2_get_Authority( provider2, authority );
        IWebAccountProvider2_Release( provider2 );
    }
}

/* a Microsoft account (the "consumers" authority) is reported as WebAccountState_None, an organization's as Connected */
static WebAccountState account_state( IWebAccountProvider *provider )
{
    return provider_is_consumers( provider ) ? WebAccountState_None : WebAccountState_Connected;
}

/* the broker's answer to a token request as a WebTokenRequestResult */
static HRESULT token_result( const struct broker_reply *reply, IWebAccountProvider *provider, IWebTokenRequestResult **out )
{
    IVectorView_WebTokenResponse *responses = NULL;
    IMap_HSTRING_HSTRING *properties = NULL;
    IWebProviderError *error = NULL;
    IWebTokenResponse *response = NULL;
    IWebAccount *account = NULL;
    HSTRING message = NULL, token = NULL;
    HRESULT hr;

    if (reply->status < WebTokenRequestStatus_Success || reply->status > WebTokenRequestStatus_ProviderError)
    {
        WARN( "the token broker's status %d is not valid\n", reply->status );
        return E_FAIL;
    }
    if (reply->status == WebTokenRequestStatus_Success)
    {
        if (reply->field[TOKENBROKER_REPLY_ACCOUNT_ID].len)
        {
            IMap_HSTRING_HSTRING *account_properties;

            if (FAILED(hr = web_account_create( provider, reply->field[TOKENBROKER_REPLY_ACCOUNT_ID].text,
                                                reply->field[TOKENBROKER_REPLY_ACCOUNT_ID].len,
                                                reply->field[TOKENBROKER_REPLY_USER_NAME].text,
                                                reply->field[TOKENBROKER_REPLY_USER_NAME].len,
                                                account_state( provider ), &account )))
                goto done;
            if (SUCCEEDED(properties_from_pairs( reply->field[TOKENBROKER_REPLY_ACCOUNT_PROPERTIES],
                                                 &account_properties )))
            {
                web_account_set_properties( account, account_properties );
                IMap_HSTRING_HSTRING_Release( account_properties );
            }
        }
        if (FAILED(hr = properties_from_pairs( reply->field[TOKENBROKER_REPLY_PROPERTIES], &properties ))) goto done;
        if (FAILED(hr = WindowsCreateString( reply->field[TOKENBROKER_REPLY_TOKEN].text,
                                             reply->field[TOKENBROKER_REPLY_TOKEN].len, &token )))
            goto done;
        if (FAILED(hr = webtokenresponse_create( token, account, properties, &response ))) goto done;
        hr = vector_view_webtokenresponse_create( &response, 1, &responses );
    }
    else
    {
        /* programs read the error of a failed request without a check for NULL */
        if (FAILED(hr = WindowsCreateString( reply->field[TOKENBROKER_REPLY_MESSAGE].text,
                                             reply->field[TOKENBROKER_REPLY_MESSAGE].len, &message )))
            goto done;
        if (FAILED(hr = webprovidererror_create( reply->code, message, &error ))) goto done;
        hr = vector_view_webtokenresponse_create( NULL, 0, &responses );
    }
    if (SUCCEEDED(hr)) hr = webtokenrequestresult_create( reply->status, responses, error, out );

done:
    if (responses) IVectorView_WebTokenResponse_Release( responses );
    if (response) IWebTokenResponse_Release( response );
    if (properties) IMap_HSTRING_HSTRING_Release( properties );
    if (account) IWebAccount_Release( account );
    if (error) IWebProviderError_Release( error );
    WindowsDeleteString( message );
    WindowsDeleteString( token );
    return hr;
}

/* a token request for the broker, read from the program's request where the call is made */
struct token_broker_call
{
    struct broker_message msg;
    IWebAccountProvider *provider;
    BOOL silent;
};

/* The token request as the broker's message: S_OK, or a failure, then the request is answered as without a broker.
 * account is the WebAccount the request names, or NULL. */
HRESULT token_broker_prepare( IInspectable *request, IInspectable *account, BOOL silent, HWND window,
                              struct token_broker_call **out )
{
    HSTRING client = NULL, scope = NULL, provider_id = NULL, authority = NULL, correlation = NULL, account_id = NULL;
    struct field fields[TOKENBROKER_FIELD_COUNT] = {{0}};
    IMap_HSTRING_HSTRING *properties = NULL;
    WebTokenRequestPromptType prompt = 0;
    IWebTokenRequest *token_request;
    IWebTokenRequest3 *request3;
    struct token_broker_call *call;
    WCHAR *pairs = NULL;
    UINT32 pairs_len = 0;
    HRESULT hr;

    *out = NULL;
    if (!request || FAILED(IInspectable_QueryInterface( request, &IID_IWebTokenRequest, (void **)&token_request )))
        return E_INVALIDARG;
    if (!(call = calloc( 1, sizeof(*call) )))
    {
        IWebTokenRequest_Release( token_request );
        return E_OUTOFMEMORY;
    }
    call->silent = silent;
    IWebTokenRequest_get_ClientId( token_request, &client );
    IWebTokenRequest_get_Scope( token_request, &scope );
    IWebTokenRequest_get_PromptType( token_request, &prompt );
    if (FAILED(IWebTokenRequest_get_WebAccountProvider( token_request, &call->provider ))) call->provider = NULL;
    provider_fields( call->provider, &provider_id, &authority );
    if (SUCCEEDED(IWebTokenRequest_QueryInterface( token_request, &IID_IWebTokenRequest3, (void **)&request3 )))
    {
        IWebTokenRequest3_get_CorrelationId( request3, &correlation );
        IWebTokenRequest3_Release( request3 );
    }
    if (SUCCEEDED(IWebTokenRequest_get_Properties( token_request, &properties )) && properties)
    {
        map_hstring_pairs( properties, &pairs, &pairs_len );
        IMap_HSTRING_HSTRING_Release( properties );
    }
    IWebTokenRequest_Release( token_request );
    account_id = web_account_id( account );

    fields[TOKENBROKER_FIELD_CLIENT_ID] = hstring_field( client );
    fields[TOKENBROKER_FIELD_SCOPE] = hstring_field( scope );
    fields[TOKENBROKER_FIELD_PROVIDER_ID] = hstring_field( provider_id );
    fields[TOKENBROKER_FIELD_AUTHORITY] = hstring_field( authority );
    fields[TOKENBROKER_FIELD_CORRELATION_ID] = hstring_field( correlation );
    fields[TOKENBROKER_FIELD_ACCOUNT_ID] = hstring_field( account_id );
    fields[TOKENBROKER_FIELD_PROPERTIES].text = pairs;
    fields[TOKENBROKER_FIELD_PROPERTIES].len = pairs_len;
    hr = build_message( silent ? TOKENBROKER_GET_TOKEN_SILENT : TOKENBROKER_REQUEST_TOKEN, fields, prompt, window,
                        &call->msg );

    WindowsDeleteString( client );
    WindowsDeleteString( scope );
    WindowsDeleteString( provider_id );
    WindowsDeleteString( authority );
    WindowsDeleteString( correlation );
    WindowsDeleteString( account_id );
    free( pairs );
    if (FAILED(hr))
    {
        token_broker_free( call );
        return hr;
    }
    *out = call;
    return S_OK;
}

void token_broker_free( struct token_broker_call *call )
{
    if (!call) return;
    free_message( &call->msg );
    if (call->provider) IWebAccountProvider_Release( call->provider );
    free( call );
}

/* The prepared request through the broker: S_OK and its result, or a failure, then the request is answered as without
 * a broker */
HRESULT token_broker_run( struct token_broker_call *call, IWebTokenRequestResult **result )
{
    struct broker_reply reply;
    HRESULT hr;

    *result = NULL;
    if (FAILED(hr = transact( &call->msg, call->silent ? BROKER_SILENT_TIMEOUT_MS : BROKER_INTERACTIVE_TIMEOUT_MS,
                              &reply )))
        return hr;
    hr = token_result( &reply, call->provider, result );
    free_reply( &reply );
    return hr;
}

/* WebAccount.SignOutAsync through the broker: the account's tokens for client_id (the broker refuses an empty one) */
HRESULT token_broker_sign_out( IWebAccountProvider *provider, HSTRING account_id, HSTRING client_id )
{
    struct field fields[TOKENBROKER_FIELD_COUNT] = {{0}};
    HSTRING provider_id, authority;
    struct broker_reply reply;
    HRESULT hr;

    provider_fields( provider, &provider_id, &authority );
    fields[TOKENBROKER_FIELD_CLIENT_ID] = hstring_field( client_id );
    fields[TOKENBROKER_FIELD_PROVIDER_ID] = hstring_field( provider_id );
    fields[TOKENBROKER_FIELD_AUTHORITY] = hstring_field( authority );
    fields[TOKENBROKER_FIELD_ACCOUNT_ID] = hstring_field( account_id );
    hr = broker_exchange( TOKENBROKER_SIGN_OUT, fields, &reply );
    WindowsDeleteString( provider_id );
    WindowsDeleteString( authority );
    if (FAILED(hr)) return hr;
    if (reply.status != WebTokenRequestStatus_Success)
    {
        WARN( "the token broker refused the sign-out: %#x\n", reply.code );
        hr = reply.code ? reply.code : E_FAIL;
    }
    free_reply( &reply );
    return hr;
}

/* FindAllAccountsAsync through the broker: S_OK and its result, or a failure (answered as without a broker) */
HRESULT token_broker_find_accounts( IWebAccountProvider *provider, HSTRING client_id, IFindAllAccountsResult **result )
{
    struct field fields[TOKENBROKER_FIELD_COUNT] = {{0}}, rest, id, name;
    IVectorView_WebAccount *view = NULL;
    IWebProviderError *error = NULL;
    HSTRING provider_id, authority, message = NULL;
    IWebAccount **accounts = NULL;
    UINT32 count = 0, max, i;
    struct broker_reply reply;
    WebAccountState state;
    HRESULT hr;

    *result = NULL;
    provider_fields( provider, &provider_id, &authority );
    fields[TOKENBROKER_FIELD_CLIENT_ID] = hstring_field( client_id );
    fields[TOKENBROKER_FIELD_PROVIDER_ID] = hstring_field( provider_id );
    fields[TOKENBROKER_FIELD_AUTHORITY] = hstring_field( authority );
    hr = broker_exchange( TOKENBROKER_FIND_ACCOUNTS, fields, &reply );
    WindowsDeleteString( provider_id );
    WindowsDeleteString( authority );
    if (FAILED(hr)) return hr;
    if (reply.status < FindAllWebAccountsStatus_Success || reply.status > FindAllWebAccountsStatus_ProviderError)
    {
        WARN( "the token broker's status %d is not valid\n", reply.status );
        free_reply( &reply );
        return E_FAIL;
    }

    state = account_state( provider );
    rest = reply.field[TOKENBROKER_REPLY_ACCOUNTS];
    max = rest.len / 2 + 1;
    if (!(accounts = calloc( max, sizeof(*accounts) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    while (count < max && next_string( &rest, &id ) && next_string( &rest, &name ))
    {
        if (FAILED(hr = web_account_create( provider, id.text, id.len, name.text, name.len, state, &accounts[count] )))
            goto done;
        count++;
    }
    if (FAILED(hr = vector_view_webaccount_create( accounts, count, &view ))) goto done;
    if (reply.status != FindAllWebAccountsStatus_Success)
    {
        if (FAILED(hr = WindowsCreateString( reply.field[TOKENBROKER_REPLY_MESSAGE].text,
                                             reply.field[TOKENBROKER_REPLY_MESSAGE].len, &message )) ||
            FAILED(hr = webprovidererror_create( reply.code, message, &error )))
            goto done;
    }
    hr = findallaccountsresult_create( reply.status, view, error, result );

done:
    for (i = 0; i < count; i++) IWebAccount_Release( accounts[i] );
    free( accounts );
    if (view) IVectorView_WebAccount_Release( view );
    if (error) IWebProviderError_Release( error );
    WindowsDeleteString( message );
    free_reply( &reply );
    return hr;
}

/* FindAccountAsync through the broker: S_OK and the account or NULL, or a failure (answered as without a broker) */
HRESULT token_broker_find_account( IWebAccountProvider *provider, HSTRING id, IWebAccount **account )
{
    struct field fields[TOKENBROKER_FIELD_COUNT] = {{0}};
    HSTRING provider_id, authority;
    struct broker_reply reply;
    HRESULT hr;

    *account = NULL;
    provider_fields( provider, &provider_id, &authority );
    fields[TOKENBROKER_FIELD_PROVIDER_ID] = hstring_field( provider_id );
    fields[TOKENBROKER_FIELD_AUTHORITY] = hstring_field( authority );
    fields[TOKENBROKER_FIELD_ACCOUNT_ID] = hstring_field( id );
    hr = broker_exchange( TOKENBROKER_FIND_ACCOUNT, fields, &reply );
    WindowsDeleteString( provider_id );
    WindowsDeleteString( authority );
    if (FAILED(hr)) return hr;
    if (reply.status == TOKENBROKER_ACCOUNT_FOUND && reply.field[TOKENBROKER_REPLY_ACCOUNT_ID].len)
        hr = web_account_create( provider, reply.field[TOKENBROKER_REPLY_ACCOUNT_ID].text,
                                 reply.field[TOKENBROKER_REPLY_ACCOUNT_ID].len,
                                 reply.field[TOKENBROKER_REPLY_USER_NAME].text,
                                 reply.field[TOKENBROKER_REPLY_USER_NAME].len, account_state( provider ), account );
    free_reply( &reply );
    return hr;
}
