/*
 * The protocol of the token broker, the system role behind Windows.Security.Authentication.Web.Core
 * (WineExplorer docs/WAM_BROKER_PLAN.md, steps 1 to 4)
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
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

#ifndef __WINE_WINE_TOKENBROKER_H
#define __WINE_WINE_TOKENBROKER_H

/*
 * One broker per user and session (tokenbroker.exe, started on demand by the client), on the message-mode pipe
 * TOKENBROKER_PIPE_PREFIX followed by the user's SID in string form; the pipe's descriptor admits that user only, and
 * each side checks the other's token user too. Each exchange is one request message and one reply message:
 *
 *   request:  struct tokenbroker_request, then the fields' UTF-16 text (no terminators) in the order of enum
 *             tokenbroker_field, field_len[] giving each one's length in bytes
 *   reply:    struct tokenbroker_reply, then its fields' UTF-16 text in the order of enum tokenbroker_reply_field,
 *             field_len[] giving each one's length in bytes
 *
 * A field that holds pairs or lists (the PROPERTIES fields, ACCOUNTS) separates its strings with a NUL character
 * and ends each with one: "key\0value\0key\0value\0", "id\0user name\0id\0user name\0".
 *
 * hdr.size is the whole message's size in bytes; hdr.request_id is the client's, returned in the reply; a reply of
 * another version, kind or request id is refused. Nothing secret is logged on either side.
 */

#define TOKENBROKER_PIPE_PREFIX     L"\\\\.\\pipe\\WineTokenBroker\\"
#define TOKENBROKER_MAGIC           0x4b425457  /* "WTBK" */
#define TOKENBROKER_VERSION         2
#define TOKENBROKER_MAX_MESSAGE     (256 * 1024)

enum tokenbroker_kind
{
    TOKENBROKER_HELLO = 1,          /* no fields; the reply's status is 0 */
    TOKENBROKER_GET_TOKEN_SILENT,   /* GetTokenSilentlyAsync: never shows anything */
    TOKENBROKER_REQUEST_TOKEN,      /* RequestTokenAsync and the ForWindow forms: may show the sign-in window */
    TOKENBROKER_FIND_ACCOUNTS,      /* FindAllAccountsAsync: the reply's status is a FindAllWebAccountsStatus */
    TOKENBROKER_FIND_ACCOUNT,       /* FindAccountAsync by ACCOUNT_ID: status 0 and the account, or 1 for none */
    TOKENBROKER_SIGN_OUT,           /* WebAccount.SignOutAsync: the account's tokens (for CLIENT_ID if not empty) */
};

enum tokenbroker_field
{
    TOKENBROKER_FIELD_CLIENT_ID,
    TOKENBROKER_FIELD_SCOPE,
    TOKENBROKER_FIELD_PROVIDER_ID,
    TOKENBROKER_FIELD_AUTHORITY,
    TOKENBROKER_FIELD_CORRELATION_ID,
    TOKENBROKER_FIELD_ACCOUNT_ID,   /* the WebAccount the request names, or empty */
    TOKENBROKER_FIELD_PROPERTIES,   /* the request's Properties, as pairs */
    TOKENBROKER_FIELD_COUNT
};

enum tokenbroker_reply_field
{
    TOKENBROKER_REPLY_MESSAGE,      /* the provider error's message */
    TOKENBROKER_REPLY_TOKEN,        /* WebTokenResponse.Token */
    TOKENBROKER_REPLY_ACCOUNT_ID,   /* the response's WebAccount: Id */
    TOKENBROKER_REPLY_USER_NAME,    /* and UserName */
    TOKENBROKER_REPLY_PROPERTIES,   /* WebTokenResponse.Properties, as pairs */
    TOKENBROKER_REPLY_ACCOUNTS,     /* FIND_ACCOUNTS: id and user name of each account */
    TOKENBROKER_REPLY_COUNT
};

struct tokenbroker_header
{
    unsigned int magic;
    unsigned int version;
    unsigned int kind;
    unsigned int request_id;
    unsigned int size;
};

struct tokenbroker_request
{
    struct tokenbroker_header hdr;
    unsigned int prompt_type;               /* WebTokenRequestPromptType */
    unsigned int window;                    /* the calling program's window (an HWND as a number), or 0 */
    unsigned int field_len[TOKENBROKER_FIELD_COUNT];
};

struct tokenbroker_reply
{
    struct tokenbroker_header hdr;
    int          status;                    /* WebTokenRequestStatus, or as the kind says */
    unsigned int error_code;                /* the provider error's code, 0 without one */
    unsigned int field_len[TOKENBROKER_REPLY_COUNT];
};

#endif  /* __WINE_WINE_TOKENBROKER_H */
