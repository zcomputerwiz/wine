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

/* is the pipe's server a process of this program's user? (the pipe's name and descriptor say so; this checks it) */
static BOOL server_is_own_user( HANDLE pipe, const SID *own )
{
    HANDLE process;
    ULONG pid;
    SID *sid;
    BOOL ret = FALSE;

    if (!GetNamedPipeServerProcessId( pipe, &pid )) return FALSE;
    if (!(process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid ))) return FALSE;
    if ((sid = process_user( process )))
    {
        ret = EqualSid( sid, (SID *)own );
        free( sid );
    }
    CloseHandle( process );
    return ret;
}

static void start_broker( void )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR path[MAX_PATH];
    UINT len;

    if (!(len = GetSystemDirectoryW( path, MAX_PATH )) || len + 17 > MAX_PATH) return;
    wcscat( path, L"\\tokenbroker.exe" );
    if (!CreateProcessW( path, NULL, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi ))
    {
        WARN( "cannot start the token broker: %lu\n", GetLastError() );
        return;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
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
                            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL );
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW( name, 1000 );
        else if (GetLastError() == ERROR_FILE_NOT_FOUND && !started)
        {
            start_broker();
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

/* One exchange with the user's broker. S_OK with its reply (free_reply), or a failure: then the call is
 * answered as without a broker. */
static HRESULT broker_exchange( unsigned int kind, const struct field fields[TOKENBROKER_FIELD_COUNT],
                                WebTokenRequestPromptType prompt, HWND window, struct broker_reply *out )
{
    static LONG next_id;
    struct tokenbroker_request *req;
    struct tokenbroker_reply *reply;
    char *buffer = NULL, *answer = NULL;
    HANDLE token, pipe = INVALID_HANDLE_VALUE;
    DWORD offset, size;
    SID *own = NULL;
    HRESULT hr = E_FAIL;
    unsigned int i;

    memset( out, 0, sizeof(*out) );
    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token )) goto done;
    own = token_user( token );
    CloseHandle( token );
    if (!own) goto done;
    if (!(buffer = malloc( TOKENBROKER_MAX_MESSAGE )) || !(answer = malloc( TOKENBROKER_MAX_MESSAGE ))) goto done;

    req = (struct tokenbroker_request *)buffer;
    req->hdr.magic = TOKENBROKER_MAGIC;
    req->hdr.version = TOKENBROKER_VERSION;
    req->hdr.kind = kind;
    req->hdr.request_id = InterlockedIncrement( &next_id );
    req->prompt_type = prompt;
    req->window = HandleToULong( window );
    offset = sizeof(*req);
    for (i = 0; i < TOKENBROKER_FIELD_COUNT; i++)
    {
        req->field_len[i] = fields[i].len * sizeof(WCHAR);
        if (req->field_len[i] > TOKENBROKER_MAX_MESSAGE - offset) goto done;
        memcpy( buffer + offset, fields[i].text, req->field_len[i] );
        offset += req->field_len[i];
    }
    req->hdr.size = offset;

    if ((pipe = connect_broker( own )) == INVALID_HANDLE_VALUE) goto done;
    if (!TransactNamedPipe( pipe, buffer, offset, answer, TOKENBROKER_MAX_MESSAGE, &size, NULL )) goto done;
    reply = (struct tokenbroker_reply *)answer;
    if (size < sizeof(*reply) || reply->hdr.magic != TOKENBROKER_MAGIC || reply->hdr.version != TOKENBROKER_VERSION ||
        reply->hdr.kind != req->hdr.kind || reply->hdr.request_id != req->hdr.request_id || reply->hdr.size != size)
    {
        WARN( "the token broker's answer is not valid\n" );
        goto done;
    }
    offset = sizeof(*reply);
    for (i = 0; i < TOKENBROKER_REPLY_COUNT; i++)
    {
        if (reply->field_len[i] % sizeof(WCHAR) || reply->field_len[i] > size - offset)
        {
            WARN( "the token broker's answer is not valid\n" );
            goto done;
        }
        out->field[i].text = (const WCHAR *)(answer + offset);
        out->field[i].len = reply->field_len[i] / sizeof(WCHAR);
        offset += reply->field_len[i];
    }
    if (offset != size) goto done;
    out->status = reply->status;
    out->code = reply->error_code;
    out->buffer = answer;
    out->size = size;
    answer = NULL;
    /* lengths only: tokens are never logged */
    TRACE( "request %u kind %u: status %d, error %#x, token length %u\n", req->hdr.request_id, kind, out->status,
           out->code, out->field[TOKENBROKER_REPLY_TOKEN].len );
    hr = S_OK;

done:
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle( pipe );
    free( buffer );
    if (answer) SecureZeroMemory( answer, TOKENBROKER_MAX_MESSAGE );
    free( answer );
    free( own );
    if (FAILED(hr)) WARN( "the token broker was not reached; answering without it\n" );
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

    if (reply->status == WebTokenRequestStatus_Success)
    {
        if (reply->field[TOKENBROKER_REPLY_ACCOUNT_ID].len &&
            FAILED(hr = web_account_create( provider, reply->field[TOKENBROKER_REPLY_ACCOUNT_ID].text,
                                            reply->field[TOKENBROKER_REPLY_ACCOUNT_ID].len,
                                            reply->field[TOKENBROKER_REPLY_USER_NAME].text,
                                            reply->field[TOKENBROKER_REPLY_USER_NAME].len, WebAccountState_Connected,
                                            &account )))
            goto done;
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
        hr = vector_view_webtokenresponse_create_empty( &responses );
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

/* A token request through the broker: S_OK and its result, or a failure, then the request is answered as without a
 * broker. account is the WebAccount the request names, or NULL. */
HRESULT token_broker_request( IInspectable *request, IInspectable *account, BOOL silent, HWND window,
                              IWebTokenRequestResult **result )
{
    HSTRING client = NULL, scope = NULL, provider_id = NULL, authority = NULL, correlation = NULL, account_id = NULL;
    struct field fields[TOKENBROKER_FIELD_COUNT] = {{0}};
    IMap_HSTRING_HSTRING *properties = NULL;
    WebTokenRequestPromptType prompt = 0;
    IWebAccountProvider *provider = NULL;
    IWebTokenRequest *token_request;
    IWebTokenRequest3 *request3;
    struct broker_reply reply;
    WCHAR *pairs = NULL;
    UINT32 pairs_len = 0;
    HRESULT hr;

    *result = NULL;
    if (!request || FAILED(IInspectable_QueryInterface( request, &IID_IWebTokenRequest, (void **)&token_request )))
        return E_INVALIDARG;
    IWebTokenRequest_get_ClientId( token_request, &client );
    IWebTokenRequest_get_Scope( token_request, &scope );
    IWebTokenRequest_get_PromptType( token_request, &prompt );
    if (FAILED(IWebTokenRequest_get_WebAccountProvider( token_request, &provider ))) provider = NULL;
    provider_fields( provider, &provider_id, &authority );
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

    if (SUCCEEDED(hr = broker_exchange( silent ? TOKENBROKER_GET_TOKEN_SILENT : TOKENBROKER_REQUEST_TOKEN, fields,
                                        prompt, window, &reply )))
    {
        hr = token_result( &reply, provider, result );
        free_reply( &reply );
    }

    if (provider) IWebAccountProvider_Release( provider );
    WindowsDeleteString( client );
    WindowsDeleteString( scope );
    WindowsDeleteString( provider_id );
    WindowsDeleteString( authority );
    WindowsDeleteString( correlation );
    WindowsDeleteString( account_id );
    free( pairs );
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
    HRESULT hr;

    *result = NULL;
    provider_fields( provider, &provider_id, &authority );
    fields[TOKENBROKER_FIELD_CLIENT_ID] = hstring_field( client_id );
    fields[TOKENBROKER_FIELD_PROVIDER_ID] = hstring_field( provider_id );
    fields[TOKENBROKER_FIELD_AUTHORITY] = hstring_field( authority );
    hr = broker_exchange( TOKENBROKER_FIND_ACCOUNTS, fields, 0, NULL, &reply );
    WindowsDeleteString( provider_id );
    WindowsDeleteString( authority );
    if (FAILED(hr)) return hr;

    rest = reply.field[TOKENBROKER_REPLY_ACCOUNTS];
    max = rest.len / 2 + 1;
    if (!(accounts = calloc( max, sizeof(*accounts) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    while (count < max && next_string( &rest, &id ) && next_string( &rest, &name ))
    {
        if (FAILED(hr = web_account_create( provider, id.text, id.len, name.text, name.len, WebAccountState_Connected,
                                            &accounts[count] )))
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
    hr = broker_exchange( TOKENBROKER_FIND_ACCOUNT, fields, 0, NULL, &reply );
    WindowsDeleteString( provider_id );
    WindowsDeleteString( authority );
    if (FAILED(hr)) return hr;
    if (!reply.status && reply.field[TOKENBROKER_REPLY_ACCOUNT_ID].len)
        hr = web_account_create( provider, reply.field[TOKENBROKER_REPLY_ACCOUNT_ID].text,
                                 reply.field[TOKENBROKER_REPLY_ACCOUNT_ID].len,
                                 reply.field[TOKENBROKER_REPLY_USER_NAME].text,
                                 reply.field[TOKENBROKER_REPLY_USER_NAME].len, WebAccountState_Connected, account );
    free_reply( &reply );
    return hr;
}
