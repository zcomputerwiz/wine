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

#endif /* __WINE_SERVER_SESSION0_H */
