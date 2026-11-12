/*
 * WineExplorer system tier (session 0)
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

#ifndef __WINE_SERVER_SESSION0_H
#define __WINE_SERVER_SESSION0_H

#include <sys/types.h>
#include "request.h"

/* create_process flag: a registry-only client of session 0, with the given token */
#define WEX_CREATE_REGISTRY_CLIENT 0x80000000

/* change notifications pending for one program: its relay in its own server, and session 0 for its client */
#define SESSION0_MAX_COOKIES 4096
/* the largest user hive session 0 holds (a larger one stays with the user's server) */
#define SESSION0_MAX_USER_HIVE (64 * 1024 * 1024)

extern int session0_server;   /* this server is session 0 */
extern int session0_client;   /* this server's programs get their machine registry from session 0 */

/* after sysrun_init and the options, before the registry is loaded */
extern void session0_init( int server_mode );
/* the account session 0 runs as (session0_user= in the configuration), whose saved machine hives a user's
 * server accepts; (uid_t)-1 when there is none */
extern uid_t session0_hive_owner(void);
/* the requests a registry-only client may make */
extern int session0_request_allowed( enum request req );
/* session 0: report a change to a registry-only client's relay */
extern void session0_relay_notify( struct process *process, unsigned int cookie );
/* a user's server: drop a process's relay, setting its pending events */
extern void session0_relay_release( struct process *process );

/* T4a, session 0 (registry.c): hold a user's hive for its server (agent), drop it; send it to the agent
 * (session0.c) */
struct sid;
extern unsigned int session0_load_user_hive( const struct sid *sid, int fd, void *agent );
extern void session0_unload_user_hive( void *agent, int save );
extern int session0_send_hive( void *agent, int fd );
/* the agent wrote (ok) or failed to write the hive it was sent last; a failure makes it dirty again */
extern void session0_user_hive_saved( void *agent, int ok );
/* T4a, a user's server: its user hive is held by session 0 (its own copy is not saved); flush it at exit */
extern int session0_user_hive_remote(void);
extern void session0_client_exit(void);

/* T5.2, a user's server (session0.c): a stream socket bridged to the named pipe of session 0 for the token, or
 * -1 with the error set */
struct token;
extern int session0_open_pipe( struct token *token, const WCHAR *name, data_size_t len );
/* T5.2, session 0 (named_pipe.c): connect a session's opener to a pipe here over sock (owned from now on) */
extern unsigned int pipe_bridge_accept( struct token *token, const WCHAR *name, data_size_t len, int sock,
                                        uid_t uid, int session );

#endif /* __WINE_SERVER_SESSION0_H */
