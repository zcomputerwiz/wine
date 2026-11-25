/*
 * WineExplorer system tier: the cross-server protocol on the session link (X0)
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

/* docs/SESSION0_CROSS_SERVER.md, M0 and "Appendix: X0 wire protocol". session0.c owns the session link's socket and
 * hands this file the X frames, the socket's writability and its end; this file keeps the epochs, the peers, the
 * owner's references, the holder's calls and references, the subscriptions and the caps. */

#ifndef __WINE_SERVER_SESSION0_X_H
#define __WINE_SERVER_SESSION0_X_H

#include <sys/types.h>
#include "wine/list.h"
#include "wine/wex_session0.h"

struct fd;
struct object;
struct token;
struct thread;
struct x_conn;
struct x_peer;

/* this server's epoch (pid, start time, nonce); before any link */
extern void x_init(void);

/* session0.c: a session link (its fd, no reference taken; the peer's account, Unix pid and session) now carries X
 * frames; this side's HELLO goes out at once */
extern struct x_conn *x_conn_create( struct fd *fd, uid_t uid, pid_t pid, unsigned int session );
/* one message of the link that starts with WEX_X_MAGIC (bad: cut, or with descriptors) */
extern void x_conn_frame( struct x_conn *conn, const void *buf, size_t size, int bad );
/* the socket can take more (POLLOUT) */
extern void x_conn_writable( struct x_conn *conn );
/* the link ended; conn is freed */
extern void x_conn_closed( struct x_conn *conn );
/* the poll events the connection wants (POLLIN unless held back, POLLOUT while frames are queued) */
extern int x_conn_events( const struct x_conn *conn );

/* An object kind that can be opened over the link (X1: global names, X3: processes; the lab: a counter) */
struct x_kind_ops
{
    unsigned int kind;  /* WEX_X_KIND_* */
    /* open name (bytes) for the caller's token: the object (a reference for the caller) and the rights granted */
    unsigned int (*open)( const void *name, unsigned int size, unsigned int access, struct token *token,
                          struct object **obj, unsigned int *granted );
    int (*ready)( struct object *obj );
    /* STATUS_SUCCESS (taken) or STATUS_TIMEOUT */
    unsigned int (*try_acquire)( struct object *obj );
    /* a success that could not be sent: give it back by the object's own rules */
    void (*give_back)( struct object *obj );
};
extern void x_register_kind( const struct x_kind_ops *ops );
/* the object may have become ready: armed subscriptions on it get their notice */
extern void x_object_changed( struct object *obj );

/* The caller's identity for a request: the thread's effective token (its impersonation token, else its process's)
 * described; NULL with *status STATUS_BAD_IMPERSONATION_LEVEL for an identification or anonymous impersonation */
extern void *x_describe_caller( struct thread *thread, data_size_t *size, unsigned int *status );
extern void *x_describe_token( struct token *token, data_size_t *size, unsigned int *status );

/* ---- the holder's side ---- */

/* a reference this server holds into a peer: counted by its handles here; dead once the peer's epoch ended */
struct x_held
{
    struct list         entry;      /* in the list of all held references */
    struct x_peer      *peer;       /* NULL: the owner's epoch ended */
    unsigned long long  id;
    unsigned int        granted;
    unsigned int        handles;
    unsigned int        notices;    /* NOTICEs received */
    int                 hint;       /* a notice came and no try since */
};

/* the completion of a call: status, and the reply's header and body (NULL when the call failed here) */
typedef void (*x_done_fn)( void *ctx, unsigned int status, const struct wex_x_header *reply, const void *body,
                           size_t body_size );

/* Send a request to peer and call done with its reply. STATUS_PENDING when sent (done comes later, exactly once);
 * otherwise the status, and done is not called. raw (the lab): no local caps. */
extern unsigned int x_call( struct x_peer *peer, unsigned int type, unsigned long long ref,
                            const void *body, size_t body_size, const void *desc, size_t desc_size,
                            const void *extra, size_t extra_size, int raw, x_done_fn done, void *ctx );
extern struct x_held *x_held_add( struct x_peer *peer, unsigned long long id, unsigned int granted );
extern struct x_held *x_held_find( unsigned long long id );
extern void x_held_free( struct x_held *held );
/* a user's server: its peer (session 0 at its current epoch); session 0: the peer of a session, or NULL */
extern struct x_peer *x_peer_of_session( unsigned int session );

/* ---- for the lab (wex_x0lab.c) ---- */

/* one line per peer, as the owner sees it, into buf; the length */
extern size_t x_peers_report( char *buf, size_t size );
extern void x_own_epoch( struct wex_x_epoch *epoch );
/* the owner holds PING answers (as requests in flight) while on; off sends them */
extern void x_set_defer( int on );
/* a holder stops (or starts again) reading its link to peer */
extern void x_peer_pause( struct x_peer *peer, int paused );
extern unsigned int x_peer_calls( struct x_peer *peer );
/* the next HELLO this server sends names another pid (the lab's test of the peer's check) */
extern int x_lab_forge_hello;
/* how long a peer whose link is lost is kept (WEX_X_LOST_SECONDS; the lab shortens it) */
extern unsigned int x_lost_seconds;
/* a frame of only a header, as given (the lab: replies, notices and HELLOs that must not be answered) */
extern int x_send_raw_header( struct x_peer *peer, unsigned int type, unsigned int call, unsigned long long ref );
extern struct list x_all_held;

#endif /* __WINE_SERVER_SESSION0_X_H */
