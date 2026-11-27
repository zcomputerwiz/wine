/*
 * WineExplorer: who a service's caller is (the class the server decides, for services in session 0)
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

/* docs/EVENTLOG_DESIGN.md, the caller class (the reviewer's H3). Only the server knows whether a token came over
 * another server's link and for which session, so the decision is made once there (token_caller_class in
 * server/token.c) and asked for with the request get_caller_class; this header has no logic of its own. A service
 * asks for the class of a pipe connection's client (the server end of its pipe) or of a token. */

#ifndef __WINE_WINE_WEX_CALLER_H
#define __WINE_WINE_WEX_CALLER_H

#define WEX_CALLER_NONE           0  /* no token, or anonymous */
#define WEX_CALLER_USER           1  /* an authenticated caller (a session's system role and service groups too) */
#define WEX_CALLER_ELEVATED_ADMIN 2  /* a real user's elevated administrator token */
#define WEX_CALLER_SYSTEM_LOCAL   3  /* SYSTEM, LocalService or NetworkService of session 0 (or a plain prefix) itself */
#define WEX_CALLER_SESSION_SYSTEM 4  /* B132: a session's system role over a link, mapped by session 0 to the account
                                        whose uid runs it (that account's rights; the class names what it is) */

#ifndef WEX_CALLER_CONSTANTS_ONLY

#include "wine/server.h"

/* the class of the client of a pipe connection (pipe: the server end), or WEX_CALLER_NONE on failure */
static inline unsigned int wex_caller_class_of_pipe( HANDLE pipe )
{
    unsigned int class = WEX_CALLER_NONE;

    SERVER_START_REQ( get_caller_class )
    {
        req->handle = wine_server_obj_handle( pipe );
        req->is_pipe = 1;
        if (!wine_server_call( req )) class = reply->caller_class;
    }
    SERVER_END_REQ;
    return class;
}

/* the class of a token (TOKEN_QUERY), or WEX_CALLER_NONE on failure */
static inline unsigned int wex_caller_class_of_token( HANDLE token )
{
    unsigned int class = WEX_CALLER_NONE;

    SERVER_START_REQ( get_caller_class )
    {
        req->handle = wine_server_obj_handle( token );
        req->is_pipe = 0;
        if (!wine_server_call( req )) class = reply->caller_class;
    }
    SERVER_END_REQ;
    return class;
}

#endif /* WEX_CALLER_CONSTANTS_ONLY */

#endif /* __WINE_WINE_WEX_CALLER_H */
