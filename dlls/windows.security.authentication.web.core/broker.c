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
        pipe = CreateFileW( name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
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

/* a request's string, appended to the message (empty when the request has none) */
static BOOL append_field( char *buffer, DWORD *offset, unsigned int *len, HSTRING string )
{
    UINT32 count;
    const WCHAR *text = WindowsGetStringRawBuffer( string, &count );

    *len = count * sizeof(WCHAR);
    if (*len > TOKENBROKER_MAX_MESSAGE - *offset) return FALSE;
    memcpy( buffer + *offset, text, *len );
    *offset += *len;
    return TRUE;
}

/* the request's fields in the protocol's order (enum tokenbroker_field) */
static void request_fields( IWebTokenRequest *request, HSTRING fields[TOKENBROKER_FIELD_COUNT],
                            WebTokenRequestPromptType *prompt )
{
    IWebAccountProvider *provider = NULL;
    IWebAccountProvider2 *provider2;
    IWebTokenRequest3 *request3;

    IWebTokenRequest_get_ClientId( request, &fields[TOKENBROKER_FIELD_CLIENT_ID] );
    IWebTokenRequest_get_Scope( request, &fields[TOKENBROKER_FIELD_SCOPE] );
    IWebTokenRequest_get_PromptType( request, prompt );
    if (SUCCEEDED(IWebTokenRequest_get_WebAccountProvider( request, &provider )) && provider)
    {
        IWebAccountProvider_get_Id( provider, &fields[TOKENBROKER_FIELD_PROVIDER_ID] );
        if (SUCCEEDED(IWebAccountProvider_QueryInterface( provider, &IID_IWebAccountProvider2, (void **)&provider2 )))
        {
            IWebAccountProvider2_get_Authority( provider2, &fields[TOKENBROKER_FIELD_AUTHORITY] );
            IWebAccountProvider2_Release( provider2 );
        }
        IWebAccountProvider_Release( provider );
    }
    if (SUCCEEDED(IWebTokenRequest_QueryInterface( request, &IID_IWebTokenRequest3, (void **)&request3 )))
    {
        IWebTokenRequest3_get_CorrelationId( request3, &fields[TOKENBROKER_FIELD_CORRELATION_ID] );
        IWebTokenRequest3_Release( request3 );
    }
}

/* Ask the broker. S_OK with its status, error code and message (the caller frees the message), or a failure: then
 * the request is answered as without a broker. */
HRESULT token_broker_request( IInspectable *request, BOOL silent, HWND window, WebTokenRequestStatus *status,
                              UINT32 *code, HSTRING *message )
{
    static LONG next_id;
    HSTRING fields[TOKENBROKER_FIELD_COUNT] = {0};
    WebTokenRequestPromptType prompt = 0;
    struct tokenbroker_request *req;
    struct tokenbroker_reply *reply;
    IWebTokenRequest *token_request;
    char *buffer = NULL, *answer = NULL;
    HANDLE token, pipe = INVALID_HANDLE_VALUE;
    DWORD offset, size;
    SID *own = NULL;
    HRESULT hr = E_FAIL;
    unsigned int i;

    if (!request || FAILED(IInspectable_QueryInterface( request, &IID_IWebTokenRequest, (void **)&token_request )))
        return E_INVALIDARG;
    request_fields( token_request, fields, &prompt );
    IWebTokenRequest_Release( token_request );

    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token )) goto done;
    own = token_user( token );
    CloseHandle( token );
    if (!own) goto done;
    if (!(buffer = malloc( TOKENBROKER_MAX_MESSAGE )) || !(answer = malloc( TOKENBROKER_MAX_MESSAGE ))) goto done;

    req = (struct tokenbroker_request *)buffer;
    req->hdr.magic = TOKENBROKER_MAGIC;
    req->hdr.version = TOKENBROKER_VERSION;
    req->hdr.kind = silent ? TOKENBROKER_GET_TOKEN_SILENT : TOKENBROKER_REQUEST_TOKEN;
    req->hdr.request_id = InterlockedIncrement( &next_id );
    req->prompt_type = prompt;
    req->window = HandleToULong( window );
    offset = sizeof(*req);
    for (i = 0; i < TOKENBROKER_FIELD_COUNT; i++)
        if (!append_field( buffer, &offset, &req->field_len[i], fields[i] )) goto done;
    req->hdr.size = offset;

    if ((pipe = connect_broker( own )) == INVALID_HANDLE_VALUE) goto done;
    if (!TransactNamedPipe( pipe, buffer, offset, answer, TOKENBROKER_MAX_MESSAGE, &size, NULL )) goto done;
    reply = (struct tokenbroker_reply *)answer;
    if (size < sizeof(*reply) || reply->hdr.magic != TOKENBROKER_MAGIC || reply->hdr.version != TOKENBROKER_VERSION ||
        reply->hdr.kind != req->hdr.kind || reply->hdr.request_id != req->hdr.request_id || reply->hdr.size != size ||
        reply->message_len % sizeof(WCHAR) || reply->message_len != size - sizeof(*reply))
    {
        WARN( "the token broker's answer is not valid\n" );
        goto done;
    }
    if (FAILED(hr = WindowsCreateString( (const WCHAR *)(reply + 1), reply->message_len / sizeof(WCHAR), message )))
        goto done;
    *status = reply->status;
    *code = reply->error_code;
    TRACE( "request %u: status %d, error %#x\n", req->hdr.request_id, reply->status, reply->error_code );
    hr = S_OK;

done:
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle( pipe );
    for (i = 0; i < TOKENBROKER_FIELD_COUNT; i++) WindowsDeleteString( fields[i] );
    free( buffer );
    free( answer );
    free( own );
    if (FAILED(hr)) WARN( "the token broker was not reached; answering without it\n" );
    return hr;
}
