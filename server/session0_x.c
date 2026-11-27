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

/* M0 of docs/SESSION0_CROSS_SERVER.md (see its "Appendix: X0 wire protocol"). The words used here:
 *
 * - a connection (struct x_conn) is one session link, as session0.c keeps it; it ends with the socket;
 * - a peer (struct x_peer) is the server at the other end at one epoch (pid, start time, nonce); it outlives a lost
 *   connection for WEX_X_LOST_SECONDS while its process lives, and gets the next connection of the same epoch;
 * - the owner of an object keeps a reference (struct x_ref) per OPEN, bound to the peer that opened it; the holder
 *   keeps a struct x_held and counts its own handles;
 * - an account (struct x_account) counts what is in flight for one uid: session 0 counts each user over all of the
 *   user's servers; a user's server counts session 0.
 *
 * Nothing here blocks: frames the socket does not take are queued and sent on POLLOUT. Requests of a peer whose
 * account has more than WEX_X_MAX_QUEUED bytes queued wait (counted in flight) until the queue is under half; the
 * link is always read, so two servers that each own objects of the other never wait on each other. */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "object.h"
#include "process.h"
#include "request.h"
#include "security.h"
#include "thread.h"
#include "session0.h"
#include "session0_x.h"

#define X_HASH_SIZE 4096
#define X_MAX_KINDS 8

enum { CAP_REFS, CAP_SUBS, CAP_QUEUED, CAP_INFLIGHT };

struct x_account
{
    struct list         entry;
    uid_t               uid;
    unsigned int        refs;      /* references issued to this account's peers */
    unsigned int        subs;      /* their subscriptions */
    size_t              queued;    /* bytes queued for its connections */
    unsigned int        logged;    /* a bit per cap: logged at half, not yet under a quarter again */
};

struct x_qframe
{
    struct list               entry;
    size_t                    size;
    char                      data[1];
};

/* a request of this side waiting for its reply; it belongs to the peer, so it outlives a lost link and is sent again
 * (with the same call number) when the same epoch links again */
struct x_call
{
    struct list         entry;
    unsigned int        call;
    unsigned int        type;
    x_done_fn           done;
    void               *ctx;
    struct x_qframe    *frame;       /* the request as sent */
};

/* The owner's copy of a reply that must not be served twice (a TRY's or an OPEN's success): a request sent again
 * after a relink gets it back instead. Kept until the holder's low-water mark (the lowest call it still waits for,
 * in the status field of each request) passes it. A TRY's success still kept when the peer's epoch ends is given
 * back: as far as the owner knows it never arrived. */
#define X_MAX_CACHED 1024
struct x_cached
{
    struct list               entry;
    unsigned int              call;
    unsigned int              type;
    unsigned long long        ref;
    struct wex_x_granted      granted;     /* OPEN's body */
    struct object            *give_back;   /* TRY: the object taken */
    const struct x_kind_ops  *ops;
};

struct x_conn
{
    struct list         entry;       /* in conns */
    struct fd          *fd;          /* session0.c's, no reference */
    uid_t               uid;
    pid_t               pid;         /* the peer server's Unix pid (SO_PEERCRED) */
    unsigned int        session;
    unsigned int        number;      /* session 0: this connection's number */
    struct x_peer      *peer;        /* after the peer's HELLO */
    struct x_account   *account;
    struct list         queue;       /* struct x_qframe */
    size_t              queued;
    struct list         deferred;    /* the lab's held PING answers (struct x_qframe) */
    struct list         stalled;     /* the peer's requests waiting while the account's queue is over the cap */
    unsigned int        inflight_in; /* the peer's requests not answered yet (deferred and stalled) */
    int                 paused;      /* the lab: not read */
    int                 stall_logged;
};

struct x_peer
{
    struct list           entry;     /* in peers */
    struct wex_x_epoch    epoch;
    uid_t                 uid;
    unsigned int          session;
    struct x_conn        *conn;      /* NULL while the link is lost */
    struct x_account     *account;
    struct list           refs;      /* struct x_ref issued to this peer */
    unsigned int          ref_count;
    unsigned int          sub_count;
    struct timeout_user  *lost_timer;
    timeout_t             lost_since;
    struct list           calls;     /* struct x_call: this side's requests to the peer, oldest first */
    unsigned int          call_count;
    unsigned int          next_call;
    struct list           cached;    /* struct x_cached: replies the peer may ask for again */
    unsigned int          cached_count;
    unsigned int          stray;     /* replies to no call of ours, and other frames dropped */
};

struct x_ref
{
    struct list               peer_entry;
    struct list               hash_entry;
    struct list               armed_entry;   /* in armed_refs while armed */
    unsigned long long        id;
    struct x_peer            *peer;
    struct object            *obj;
    const struct x_kind_ops  *ops;
    unsigned int              access;
    struct token             *token;          /* the caller's, from link_bound_token */
    int                       subscribed;
    int                       armed;
};

static struct wex_x_epoch own_epoch;
static struct list conns = LIST_INIT( conns );
static struct list peers = LIST_INIT( peers );
static struct list accounts = LIST_INIT( accounts );
static struct list armed_refs = LIST_INIT( armed_refs );
static struct list ref_hash[X_HASH_SIZE];
static const struct x_kind_ops *kinds[X_MAX_KINDS];
static unsigned int conn_numbers;
static int defer_pings;
int x_lab_forge_hello;
unsigned int x_lost_seconds = WEX_X_LOST_SECONDS;  /* the lab may shorten it */
struct list x_all_held = LIST_INIT( x_all_held );

static void x_serve( struct x_conn *conn, const struct wex_x_header *hdr, const char *body, size_t size );
static void x_unstall( struct x_account *account );

/* ---- randomness, epochs ---- */

static void x_random( void *buf, size_t size )
{
    static unsigned long long fallback;
    size_t done = 0;
    int fd;

    if ((fd = open( "/dev/urandom", O_RDONLY | O_CLOEXEC )) != -1)
    {
        ssize_t n;
        while (done < size && (n = read( fd, (char *)buf + done, size - done )) > 0) done += n;
        close( fd );
    }
    if (done < size)  /* never the same value twice in this server, at least */
    {
        unsigned long long v = ++fallback ^ ((unsigned long long)getpid() << 32) ^ (unsigned long long)monotonic_time;
        memcpy( buf, &v, size < sizeof(v) ? size : sizeof(v) );
    }
}

/* a process's state letter and start time from /proc/<pid>/stat; 0 when it does not exist, -1 when it cannot be read
 * for another reason */
static int x_process_stat( pid_t pid, char *state, unsigned long long *start )
{
    char path[64], buf[1024], *p;
    int fd, field;
    ssize_t n;

    snprintf( path, sizeof(path), "/proc/%d/stat", (int)pid );
    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return errno == ENOENT || errno == ESRCH ? 0 : -1;
    n = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (n <= 0) return 0;
    buf[n] = 0;
    if (!(p = strrchr( buf, ')' )) || p[1] != ' ') return -1;
    *state = p[2];
    for (field = 2; field < 22 && p; field++) p = strchr( p + 1, ' ' );
    if (!p) return -1;
    *start = strtoull( p + 1, NULL, 10 );
    return 1;
}

/* Is the server of an epoch still running? Ended: no such process, a zombie, or another start time (a reused pid).
 * When /proc cannot be read for another reason it counts as alive, as id_block_owner_alive does. */
static int x_epoch_alive( const struct wex_x_epoch *epoch )
{
    unsigned long long start;
    char state;

    switch (x_process_stat( epoch->pid, &state, &start ))
    {
    case 0:  return 0;
    case 1:  return state != 'Z' && state != 'X' && start == epoch->start;
    default: return 1;
    }
}

void x_init(void)
{
    unsigned int i;
    char state;

    for (i = 0; i < X_HASH_SIZE; i++) list_init( &ref_hash[i] );
    own_epoch.pid = getpid();
    if (x_process_stat( getpid(), &state, &own_epoch.start ) != 1) own_epoch.start = 0;
    do x_random( &own_epoch.nonce, sizeof(own_epoch.nonce) ); while (!own_epoch.nonce);
}

void x_own_epoch( struct wex_x_epoch *epoch )
{
    *epoch = own_epoch;
}

/* ---- accounts and caps ---- */

static struct x_account *x_account_get( uid_t uid )
{
    struct x_account *account;

    LIST_FOR_EACH_ENTRY( account, &accounts, struct x_account, entry )
        if (account->uid == uid) return account;
    if (!(account = calloc( 1, sizeof(*account) ))) return NULL;
    account->uid = uid;
    list_add_tail( &accounts, &account->entry );
    return account;
}

/* log an account once when a count reaches half of its cap, and again after it fell under a quarter */
static void x_account_note( struct x_account *account, int cap, size_t value )
{
    static const char *names[] = { "references", "subscriptions", "queued bytes", "requests in flight" };
    static const size_t limits[] = { WEX_X_MAX_REFS, WEX_X_MAX_SUBS, WEX_X_MAX_QUEUED, WEX_X_MAX_INFLIGHT };

    if (value >= limits[cap] / 2)
    {
        if (account->logged & (1 << cap)) return;
        account->logged |= 1 << cap;
        fprintf( stderr, "x0: uid %u at half of the %s cap (%lu of %lu)\n", (unsigned int)account->uid, names[cap],
                 (unsigned long)value, (unsigned long)limits[cap] );
    }
    else if (value < limits[cap] / 4) account->logged &= ~(1 << cap);
}

/* ---- sending ---- */

int x_conn_events( const struct x_conn *conn )
{
    return (conn->paused ? 0 : POLLIN) | (list_empty( &conn->queue ) ? 0 : POLLOUT);
}

static void x_update_events( struct x_conn *conn )
{
    set_fd_events( conn->fd, x_conn_events( conn ));
}

/* build a frame: the header, then up to three parts; NULL when it does not fit (raw: a little over the limit, for
 * the lab's test of the limit) */
static struct x_qframe *x_build( unsigned int type, unsigned int call, unsigned int status, unsigned long long ref,
                                 const void *p1, size_t s1, const void *p2, size_t s2, const void *p3, size_t s3,
                                 int raw )
{
    size_t size = sizeof(struct wex_x_header) + s1 + s2 + s3;
    struct wex_x_header hdr;
    struct x_qframe *q;
    char *p;

    if (size > WEX_SESSION0_MAX_MSG + (raw ? 4096 : 0)) return NULL;
    if (!(q = malloc( offsetof( struct x_qframe, data[size] )))) return NULL;
    q->size = size;
    hdr.magic = WEX_X_MAGIC;
    hdr.size = size;
    hdr.type = type;
    hdr.call = call;
    hdr.status = status;
    hdr.desc_size = (type == WEX_X_OPEN || type == WEX_X_TOKEN) ? s2 : 0;  /* X4: the token's caller too */
    hdr.ref = ref;
    p = q->data;
    memcpy( p, &hdr, sizeof(hdr) );
    p += sizeof(hdr);
    if (s1) memcpy( p, p1, s1 );
    p += s1;
    if (s2) memcpy( p, p2, s2 );
    p += s2;
    if (s3) memcpy( p, p3, s3 );
    return q;
}

/* send a frame on conn, or queue it; takes q. 1 when sent or queued, 0 when it is lost (no link, the link failed):
 * a TRY's success in it is given back */
static int x_transmit( struct x_conn *conn, struct x_qframe *q )
{
    if (!conn)
    {
        free( q );
        return 0;
    }
    if (list_empty( &conn->queue ))
    {
        ssize_t n = send( get_unix_fd( conn->fd ), q->data, q->size, MSG_NOSIGNAL | MSG_DONTWAIT );

        if (n == (ssize_t)q->size)
        {
            free( q );
            return 1;
        }
        if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS))
        {
            free( q );  /* the link is ending: its end comes through poll */
            return 0;
        }
    }
    list_add_tail( &conn->queue, &q->entry );
    conn->queued += q->size;
    conn->account->queued += q->size;
    x_account_note( conn->account, CAP_QUEUED, conn->account->queued );
    /* a peer that keeps sending past every cap while it reads nothing loses its link (a loss: its references wait
     * for the relink) */
    if (conn->queued > 2 * (size_t)WEX_X_MAX_QUEUED)
    {
        fprintf( stderr, "x0: uid %u (server pid %d) reads nothing past twice the queued cap: link cut\n",
                 (unsigned int)conn->uid, (int)conn->pid );
        shutdown( get_unix_fd( conn->fd ), SHUT_RDWR );
    }
    x_update_events( conn );
    return 1;
}

void x_conn_writable( struct x_conn *conn )
{
    while (!list_empty( &conn->queue ))
    {
        struct x_qframe *q = LIST_ENTRY( list_head( &conn->queue ), struct x_qframe, entry );
        ssize_t n = send( get_unix_fd( conn->fd ), q->data, q->size, MSG_NOSIGNAL | MSG_DONTWAIT );

        if (n != (ssize_t)q->size) break;  /* full again, or the link is ending (its end comes through poll) */
        list_remove( &q->entry );
        conn->queued -= q->size;
        conn->account->queued -= q->size;
        free( q );
    }
    x_account_note( conn->account, CAP_QUEUED, conn->account->queued );
    x_update_events( conn );
    if (conn->account->queued <= WEX_X_MAX_QUEUED / 2) x_unstall( conn->account );
}

static void x_reply( struct x_conn *conn, const struct wex_x_header *req, unsigned int status, unsigned long long ref,
                     const void *body, size_t size )
{
    struct x_qframe *q = x_build( req->type | WEX_X_REPLY, req->call, status, ref, body, size, NULL, 0, NULL, 0, 0 );

    if (q) x_transmit( conn, q );
}

/* ---- the owner's cache of replies that must not be served twice ---- */

static void x_cached_free( struct x_peer *peer, struct x_cached *c, int delivered )
{
    list_remove( &c->entry );
    peer->cached_count--;
    if (c->give_back)
    {
        if (!delivered) c->ops->give_back( c->give_back );
        release_object( c->give_back );
    }
    free( c );
}

/* the holder waits for no call below low: what it got is delivered */
static void x_cache_ack( struct x_peer *peer, unsigned int low )
{
    struct x_cached *c, *next;

    LIST_FOR_EACH_ENTRY_SAFE( c, next, &peer->cached, struct x_cached, entry )
        if ((int)(c->call - low) < 0) x_cached_free( peer, c, 1 );
}

static void x_cache_add( struct x_peer *peer, const struct wex_x_header *req, unsigned long long ref,
                         const struct wex_x_granted *granted, struct object *give_back, const struct x_kind_ops *ops )
{
    struct x_cached *c;

    /* a holder that never moves its low-water mark: the oldest counts as delivered */
    if (peer->cached_count >= X_MAX_CACHED)
        x_cached_free( peer, LIST_ENTRY( list_head( &peer->cached ), struct x_cached, entry ), 1 );
    if (!(c = calloc( 1, sizeof(*c) ))) return;  /* served once anyway; only a replay would miss it */
    c->call = req->call;
    c->type = req->type;
    c->ref = ref;
    if (granted) c->granted = *granted;
    if (give_back)
    {
        c->give_back = grab_object( give_back );
        c->ops = ops;
    }
    list_add_tail( &peer->cached, &c->entry );
    peer->cached_count++;
}

/* a request sent again after a relink: its cached reply, if it had one; 1 when replayed */
static int x_cache_replay( struct x_conn *conn, const struct wex_x_header *req )
{
    struct x_cached *c;

    LIST_FOR_EACH_ENTRY( c, &conn->peer->cached, struct x_cached, entry )
    {
        if (c->call != req->call || c->type != req->type) continue;
        x_reply( conn, req, STATUS_SUCCESS, c->ref, req->type == WEX_X_OPEN ? &c->granted : NULL,
                 req->type == WEX_X_OPEN ? sizeof(c->granted) : 0 );
        return 1;
    }
    return 0;
}

/* ---- the owner's references ---- */

static struct list *x_bucket( unsigned long long id )
{
    return &ref_hash[(id ^ (id >> 32)) % X_HASH_SIZE];
}

static struct x_ref *x_ref_find_any( unsigned long long id )
{
    struct x_ref *ref;

    LIST_FOR_EACH_ENTRY( ref, x_bucket( id ), struct x_ref, hash_entry )
        if (ref->id == id) return ref;
    return NULL;
}

/* the peer's own reference id; an id of another peer, a released, forged or earlier id: NULL alike */
static struct x_ref *x_ref_lookup( struct x_peer *peer, unsigned long long id )
{
    struct x_ref *ref = id ? x_ref_find_any( id ) : NULL;
    return ref && ref->peer == peer ? ref : NULL;
}

static void x_disarm( struct x_ref *ref )
{
    if (!ref->armed) return;
    ref->armed = 0;
    list_remove( &ref->armed_entry );
}

static void x_send_notice( struct x_ref *ref )
{
    struct x_qframe *q;

    if (!ref->peer->conn || !(q = x_build( WEX_X_NOTICE, 0, 0, ref->id, NULL, 0, NULL, 0, NULL, 0, 0 ))) return;
    x_transmit( ref->peer->conn, q );
}

/* arm a subscription: a ready object gets its notice now (and the subscription is disarmed), else at the next change
 * that finds it ready. Without a link the subscription stays armed until the peer links again. */
static void x_arm( struct x_ref *ref )
{
    if (!ref->subscribed) return;
    if (ref->peer->conn && ref->ops->ready( ref->obj ))
    {
        x_disarm( ref );
        x_send_notice( ref );
        return;
    }
    if (ref->armed) return;
    ref->armed = 1;
    list_add_tail( &armed_refs, &ref->armed_entry );
}

void x_object_changed( struct object *obj )
{
    struct x_ref *ref, *next;

    LIST_FOR_EACH_ENTRY_SAFE( ref, next, &armed_refs, struct x_ref, armed_entry )
    {
        if (ref->obj != obj || !ref->peer->conn || !ref->ops->ready( obj )) continue;
        x_disarm( ref );
        x_send_notice( ref );
    }
}

static void x_unsubscribe( struct x_ref *ref )
{
    if (!ref->subscribed) return;
    x_disarm( ref );
    ref->subscribed = 0;
    ref->peer->sub_count--;
    ref->peer->account->subs--;
    x_account_note( ref->peer->account, CAP_SUBS, ref->peer->account->subs );
}

static void x_ref_free( struct x_ref *ref )
{
    x_unsubscribe( ref );
    list_remove( &ref->peer_entry );
    list_remove( &ref->hash_entry );
    ref->peer->ref_count--;
    ref->peer->account->refs--;
    x_account_note( ref->peer->account, CAP_REFS, ref->peer->account->refs );
    release_object( ref->obj );
    if (ref->token) release_object( ref->token );
    free( ref );
}

void x_register_kind( const struct x_kind_ops *ops )
{
    unsigned int i;

    for (i = 0; i < X_MAX_KINDS; i++)
    {
        if (kinds[i] == ops) return;
        if (!kinds[i])
        {
            kinds[i] = ops;
            return;
        }
    }
}

static const struct x_kind_ops *x_find_kind( unsigned int kind )
{
    unsigned int i;

    for (i = 0; i < X_MAX_KINDS && kinds[i]; i++) if (kinds[i]->kind == kind) return kinds[i];
    return NULL;
}

/* ---- the caller's identity ---- */

void *x_describe_token( struct token *token, data_size_t *size, unsigned int *status )
{
    void *desc;

    /* a thread impersonating at identification or anonymous level cannot act: never described as if it could
     * (token_create_described builds a primary token) */
    if (!token_can_act( token ))
    {
        *status = STATUS_BAD_IMPERSONATION_LEVEL;
        return NULL;
    }
    *size = token_describe( token, NULL, 0 );
    if (*size > WEX_SESSION0_MAX_MSG - sizeof(struct wex_x_header) - sizeof(struct wex_x_open))
    {
        *status = STATUS_BUFFER_OVERFLOW;
        return NULL;
    }
    if (!(desc = malloc( *size )))
    {
        *status = STATUS_NO_MEMORY;
        return NULL;
    }
    token_describe( token, desc, *size );
    *status = STATUS_SUCCESS;
    return desc;
}

void *x_describe_caller( struct thread *thread, data_size_t *size, unsigned int *status )
{
    return x_describe_token( thread->token ? thread->token : thread->process->token, size, status );
}

/* ---- peers ---- */

/* the peer's epoch ended: this side's calls to it fail (never while only its link is lost) */
static void x_fail_calls( struct x_peer *peer )
{
    struct x_call *call;

    while (!list_empty( &peer->calls ))
    {
        call = LIST_ENTRY( list_head( &peer->calls ), struct x_call, entry );
        list_remove( &call->entry );
        peer->call_count--;
        call->done( call->ctx, STATUS_CONNECTION_DISCONNECTED, NULL, NULL, 0 );
        free( call->frame );
        free( call );
    }
}

/* the lowest call this side still waits for from peer (its low-water mark, in each request's status field) */
static unsigned int x_low_water( struct x_peer *peer )
{
    if (list_empty( &peer->calls )) return peer->next_call + 1;
    return LIST_ENTRY( list_head( &peer->calls ), struct x_call, entry )->call;
}

/* send a call's request on the peer's connection (again, after a relink): a copy, with the current low-water mark */
static int x_call_send( struct x_peer *peer, struct x_call *call )
{
    struct x_qframe *q = malloc( offsetof( struct x_qframe, data[call->frame->size] ));
    unsigned int low = x_low_water( peer );

    if (!q) return 0;
    q->size = call->frame->size;
    memcpy( q->data, call->frame->data, q->size );
    memcpy( q->data + offsetof( struct wex_x_header, status ), &low, sizeof(low) );
    return x_transmit( peer->conn, q );
}

/* the peer's epoch has ended: the owner drops what it issued to it; what this server holds into it is dead */
static void x_peer_end( struct x_peer *peer, const char *why )
{
    unsigned int dropped = peer->ref_count, dead = 0;
    struct x_held *held, *next;

    while (!list_empty( &peer->refs ))
        x_ref_free( LIST_ENTRY( list_head( &peer->refs ), struct x_ref, peer_entry ));
    /* (a notify callback may free its held reference) */
    LIST_FOR_EACH_ENTRY_SAFE( held, next, &x_all_held, struct x_held, entry )
        if (held->peer == peer)
        {
            held->peer = NULL;
            dead++;
            if (held->notify) held->notify( held, held->ctx );
        }
    if (peer->conn)
    {
        peer->conn->peer = NULL;
        peer->conn = NULL;
    }
    x_fail_calls( peer );
    /* a TRY's success the holder never acknowledged: as far as the owner knows, it never arrived */
    while (!list_empty( &peer->cached ))
        x_cached_free( peer, LIST_ENTRY( list_head( &peer->cached ), struct x_cached, entry ), 0 );
    if (peer->lost_timer) remove_timeout_user( peer->lost_timer );
    list_remove( &peer->entry );
    fprintf( stderr, "x0: peer pid %u (uid %u, session %u) ended (%s): %u references dropped, %u held ones dead\n",
             peer->epoch.pid, (unsigned int)peer->uid, peer->session, why, dropped, dead );
    free( peer );
}

static void x_lost_tick( void *private )
{
    struct x_peer *peer = private;

    peer->lost_timer = NULL;
    if (peer->conn) return;
    if (!x_epoch_alive( &peer->epoch ))
    {
        x_peer_end( peer, "its server is gone" );
        return;
    }
    if (monotonic_time - peer->lost_since >= (timeout_t)x_lost_seconds * TICKS_PER_SEC)
    {
        x_peer_end( peer, "its link did not come back" );
        return;
    }
    peer->lost_timer = add_timeout_user( -TICKS_PER_SEC, x_lost_tick, peer );
}

static void x_hello( struct x_conn *conn, const char *body, size_t size )
{
    struct wex_x_hello hello;
    struct x_peer *peer, *next, *found = NULL;
    struct x_ref *ref;

    if (conn->peer || size != sizeof(hello)) return;
    memcpy( &hello, body, sizeof(hello) );
    /* the epoch must be the connected process's (SO_PEERCRED) at its start time; the nonce tells a restart */
    if (!hello.epoch.nonce || hello.epoch.pid != (unsigned int)conn->pid || !x_epoch_alive( &hello.epoch ))
    {
        fprintf( stderr, "x0: HELLO from server pid %d (uid %u) with an epoch not its own: ignored\n",
                 (int)conn->pid, (unsigned int)conn->uid );
        return;
    }
    LIST_FOR_EACH_ENTRY_SAFE( peer, next, &peers, struct x_peer, entry )
    {
        if (peer->uid == conn->uid && !memcmp( &peer->epoch, &hello.epoch, sizeof(hello.epoch) ))
        {
            /* the same epoch, but it no longer keeps us (it dropped its peer, e.g. after WEX_X_LOST_SECONDS): our
             * references into it are gone there, so ours are dead too and our calls fail; a fresh peer follows */
            if (hello.peer_nonce != own_epoch.nonce)
            {
                x_peer_end( peer, "it started fresh" );
                continue;
            }
            found = peer;
            continue;
        }
        /* an earlier epoch of the same server process, or (a user's server, whose one peer is session 0) an earlier
         * session 0: ended */
        if ((peer->uid == conn->uid && peer->epoch.pid == hello.epoch.pid) || !session0_server)
            x_peer_end( peer, "a new epoch linked" );
    }
    if (found)
    {
        struct x_call *call;


        if (found->conn && found->conn != conn) found->conn->peer = NULL;  /* a reply still coming there is dropped */
        if (found->lost_timer) remove_timeout_user( found->lost_timer );
        found->lost_timer = NULL;
        found->conn = conn;
        conn->peer = found;
        fprintf( stderr, "x0: peer pid %u (uid %u, session %u) linked again: %u references kept, %u calls sent again\n",
                 found->epoch.pid, (unsigned int)found->uid, found->session, found->ref_count, found->call_count );
        /* Whatever was inside the lost socket is gone (SEQPACKET drops what the closing end had not read):
         * - this side's calls are sent again with their call numbers: RELEASE, CANCEL, QUERY, SUBSCRIBE and PING are
         *   idempotent, and the peer replays a TRY's or an OPEN's success from its cache instead of serving it twice;
         * - every subscription is armed again, for notices that were lost. */
        LIST_FOR_EACH_ENTRY( call, &found->calls, struct x_call, entry ) x_call_send( found, call );
        LIST_FOR_EACH_ENTRY( ref, &found->refs, struct x_ref, peer_entry ) x_arm( ref );
        return;
    }
    if (!(peer = calloc( 1, sizeof(*peer) ))) return;
    peer->epoch = hello.epoch;
    peer->uid = conn->uid;
    peer->session = session0_server ? conn->session : 0;
    peer->conn = conn;
    peer->account = conn->account;
    list_init( &peer->refs );
    list_init( &peer->calls );
    list_init( &peer->cached );
    list_add_tail( &peers, &peer->entry );
    conn->peer = peer;
}

struct x_peer *x_peer_of_session( unsigned int session )
{
    struct x_peer *peer, *found = NULL;

    LIST_FOR_EACH_ENTRY( peer, &peers, struct x_peer, entry )
    {
        if (!session0_server) return peer;
        if (peer->session == session && (!found || peer->conn)) found = peer;
    }
    return found;
}

unsigned int x_peer_session( const struct x_peer *peer )
{
    return peer->session;
}

uid_t x_peer_uid( const struct x_peer *peer )
{
    return peer->uid;
}

struct x_peer *x_peer_of_server( unsigned int pid, unsigned long long start )
{
    struct x_peer *peer;

    LIST_FOR_EACH_ENTRY( peer, &peers, struct x_peer, entry )
        if (peer->epoch.pid == pid && peer->epoch.start == start) return peer;
    return NULL;
}

/* ---- connections ---- */

struct x_conn *x_conn_create( struct fd *fd, uid_t uid, pid_t pid, unsigned int session )
{
    struct wex_x_hello hello;
    struct x_conn *conn;
    struct x_qframe *q;

    if (!(conn = calloc( 1, sizeof(*conn) ))) return NULL;
    if (!(conn->account = x_account_get( uid )))
    {
        free( conn );
        return NULL;
    }
    conn->fd = fd;
    conn->uid = uid;
    conn->pid = pid;
    conn->session = session;
    conn->number = ++conn_numbers;
    list_init( &conn->queue );
    list_init( &conn->deferred );
    list_init( &conn->stalled );
    list_add_tail( &conns, &conn->entry );

    memset( &hello, 0, sizeof(hello) );
    hello.epoch = own_epoch;
    /* the epoch this side still keeps for the server at the other end (its process: SO_PEERCRED), if any */
    {
        struct x_peer *peer;
        LIST_FOR_EACH_ENTRY( peer, &peers, struct x_peer, entry )
            if (peer->uid == uid && peer->epoch.pid == (unsigned int)pid) hello.peer_nonce = peer->epoch.nonce;
    }
    if (x_lab_forge_hello)  /* the lab: one HELLO that names another pid, which the peer must refuse */
    {
        x_lab_forge_hello = 0;
        hello.epoch.pid ^= 1;
    }
    hello.connection = session0_server ? conn->number : 0;
    hello.session = session0_server ? session : default_session_id;
    if ((q = x_build( WEX_X_HELLO, 0, 0, 0, &hello, sizeof(hello), NULL, 0, NULL, 0, 0 ))) x_transmit( conn, q );
    return conn;
}

static void x_drop_frames( struct list *list )
{
    while (!list_empty( list ))
    {
        struct x_qframe *q = LIST_ENTRY( list_head( list ), struct x_qframe, entry );
        list_remove( &q->entry );
        free( q );
    }
}

void x_conn_closed( struct x_conn *conn )
{
    struct x_peer *peer = conn->peer;

    /* the calls stay with the peer while its epoch lives (sent again on the relink); replies that never went out are
     * dropped: a TRY's or an OPEN's success stays in the owner's cache for the relink, or is given back when the
     * peer's epoch ends */
    list_remove( &conn->entry );
    conn->peer = NULL;
    if (peer) peer->conn = NULL;
    conn->account->queued -= conn->queued;
    x_drop_frames( &conn->queue );
    x_drop_frames( &conn->deferred );
    x_drop_frames( &conn->stalled );
    if (peer)
    {
        if (!x_epoch_alive( &peer->epoch )) x_peer_end( peer, "its server is gone" );
        else
        {
            /* a lost link, not an ended server: the references wait for the relink, for a bounded time */
            fprintf( stderr, "x0: link to peer pid %u (uid %u, session %u) lost: %u references wait for it\n",
                     peer->epoch.pid, (unsigned int)peer->uid, peer->session, peer->ref_count );
            peer->lost_since = monotonic_time;
            if (!peer->lost_timer) peer->lost_timer = add_timeout_user( -TICKS_PER_SEC, x_lost_tick, peer );
        }
    }
    free( conn );
    x_unstall( NULL );
}

/* requests that waited while their account's queue was over the cap are served once it is under half */
static void x_unstall( struct x_account *account )
{
    struct x_conn *conn;

    LIST_FOR_EACH_ENTRY( conn, &conns, struct x_conn, entry )
    {
        if (account && conn->account != account) continue;
        while (!list_empty( &conn->stalled ) && conn->account->queued <= WEX_X_MAX_QUEUED / 2 && conn->peer)
        {
            struct x_qframe *q = LIST_ENTRY( list_head( &conn->stalled ), struct x_qframe, entry );
            const struct wex_x_header *hdr = (const struct wex_x_header *)q->data;

            list_remove( &q->entry );
            conn->inflight_in--;
            x_serve( conn, hdr, q->data + sizeof(*hdr), q->size - sizeof(*hdr) );
            free( q );
        }
        if (list_empty( &conn->stalled )) conn->stall_logged = 0;
    }
}

static void x_reply_received( struct x_conn *conn, const struct wex_x_header *hdr, const char *body, size_t size )
{
    struct x_peer *peer = conn->peer;
    struct x_call *call;

    if (!peer) return;  /* before HELLO, or a connection the peer has left: dropped */
    LIST_FOR_EACH_ENTRY( call, &peer->calls, struct x_call, entry )
    {
        if (call->call != hdr->call || call->type != (hdr->type & ~WEX_X_REPLY)) continue;
        list_remove( &call->entry );
        peer->call_count--;
        call->done( call->ctx, hdr->status, hdr, body, size );
        free( call->frame );
        free( call );
        return;
    }
    peer->stray++;  /* a reply to no call of ours: never answered */
}

static void x_notice( struct x_conn *conn, const struct wex_x_header *hdr )
{
    struct x_held *held;

    if (!conn->peer) return;
    LIST_FOR_EACH_ENTRY( held, &x_all_held, struct x_held, entry )
    {
        if (held->id != hdr->ref || held->peer != conn->peer) continue;
        held->notices++;
        held->hint = 1;
        if (held->notify) held->notify( held, held->ctx );
        return;
    }
}

void x_conn_frame( struct x_conn *conn, const void *buf, size_t size, int bad )
{
    struct wex_x_header hdr;
    const char *body = (const char *)buf + sizeof(hdr);

    if (size < sizeof(hdr)) return;
    memcpy( &hdr, buf, sizeof(hdr) );
    if (hdr.magic != WEX_X_MAGIC) return;
    if (bad || hdr.size != size || size > WEX_SESSION0_MAX_MSG)
    {
        fprintf( stderr, "x0: a cut or malformed frame (type %x, %lu bytes, %u said) from server pid %d\n", hdr.type,
                 (unsigned long)size, hdr.size, (int)conn->pid );
        /* a reply, a NOTICE or a HELLO is never answered (two servers would bounce it for ever); nor anything before
         * the HELLO. Frames with descriptors arrive here as bad (session0.c): none carries one before X5. */
        if (conn->peer && !(hdr.type & WEX_X_REPLY) && hdr.type != WEX_X_HELLO && hdr.type != WEX_X_NOTICE)
            x_reply( conn, &hdr, STATUS_INVALID_PARAMETER, 0, NULL, 0 );
        else if (conn->peer) conn->peer->stray++;
        return;
    }
    size -= sizeof(hdr);
    if (hdr.type & WEX_X_REPLY)
    {
        x_reply_received( conn, &hdr, body, size );
        return;
    }
    switch (hdr.type)
    {
    case WEX_X_HELLO:
        x_hello( conn, body, size );
        return;
    case WEX_X_NOTICE:
        x_notice( conn, &hdr );
        return;
    }
    if (!conn->peer) return;  /* before its HELLO: dropped */
    if (conn->inflight_in >= WEX_X_MAX_INFLIGHT)
    {
        x_reply( conn, &hdr, STATUS_INSUFFICIENT_RESOURCES, 0, NULL, 0 );
        return;
    }
    /* over the queued cap: the request waits, counted in flight (the link is still read) */
    if (conn->account->queued > WEX_X_MAX_QUEUED || !list_empty( &conn->stalled ))
    {
        struct x_qframe *q;

        if (!(q = malloc( offsetof( struct x_qframe, data[size + sizeof(hdr)] ))))
        {
            x_reply( conn, &hdr, STATUS_NO_MEMORY, 0, NULL, 0 );
            return;
        }
        q->size = size + sizeof(hdr);
        memcpy( q->data, buf, q->size );
        list_add_tail( &conn->stalled, &q->entry );
        conn->inflight_in++;
        x_account_note( conn->account, CAP_INFLIGHT, conn->inflight_in );
        if (!conn->stall_logged++)
            fprintf( stderr, "x0: uid %u over the queued cap: requests of server pid %d wait\n",
                     (unsigned int)conn->uid, (int)conn->pid );
        return;
    }
    x_serve( conn, &hdr, body, size );
}

/* ---- the owner's requests ---- */

static void x_serve_open( struct x_conn *conn, const struct wex_x_header *hdr, const char *body, size_t size )
{
    struct x_peer *peer = conn->peer;
    struct wex_x_granted granted = { 0, 0 };
    const struct x_kind_ops *ops;
    struct wex_x_open open;
    struct object *obj = NULL;
    struct token *token;
    struct x_ref *ref;
    unsigned int status;

    if (size < sizeof(open)) goto invalid;
    memcpy( &open, body, sizeof(open) );
    if (hdr->desc_size > size - sizeof(open) || open.name_size != size - sizeof(open) - hdr->desc_size) goto invalid;
    if (!(ops = x_find_kind( open.kind )))
    {
        x_reply( conn, hdr, STATUS_NOT_SUPPORTED, 0, NULL, 0 );
        return;
    }
    /* condition 1: the only way a description in an X request becomes a token */
    if (!(token = link_bound_token( peer->uid, peer->session, body + sizeof(open), hdr->desc_size, &status )))
    {
        fprintf( stderr, "x0: OPEN refused for uid %u, session %u: the described token (%x)\n",
                 (unsigned int)peer->uid, peer->session, status );
        x_reply( conn, hdr, status, 0, NULL, 0 );
        return;
    }
    if (peer->account->refs >= WEX_X_MAX_REFS)
    {
        release_object( token );
        x_reply( conn, hdr, STATUS_INSUFFICIENT_RESOURCES, 0, NULL, 0 );
        return;
    }
    status = ops->open( body + sizeof(open) + hdr->desc_size, open.name_size, open.access, token, &obj,
                        &granted.access );
    if (!status && !(ref = calloc( 1, sizeof(*ref) )))
    {
        release_object( obj );
        status = STATUS_NO_MEMORY;
    }
    if (status)
    {
        release_object( token );
        x_reply( conn, hdr, status, 0, NULL, 0 );
        return;
    }
    do x_random( &ref->id, sizeof(ref->id) ); while (!ref->id || x_ref_find_any( ref->id ));
    ref->peer = peer;
    ref->obj = obj;
    ref->ops = ops;
    ref->access = granted.access;
    ref->token = token;
    list_add_tail( &peer->refs, &ref->peer_entry );
    list_add_tail( x_bucket( ref->id ), &ref->hash_entry );
    peer->ref_count++;
    peer->account->refs++;
    x_account_note( peer->account, CAP_REFS, peer->account->refs );
    granted.ready = ops->ready( obj );
    x_cache_add( peer, hdr, ref->id, &granted, NULL, NULL );  /* sent again after a relink: the same reference */
    x_reply( conn, hdr, STATUS_SUCCESS, ref->id, &granted, sizeof(granted) );
    return;

invalid:
    x_reply( conn, hdr, STATUS_INVALID_PARAMETER, 0, NULL, 0 );
}

static void x_serve( struct x_conn *conn, const struct wex_x_header *hdr, const char *body, size_t size )
{
    struct wex_x_granted granted;
    struct x_ref *ref = NULL;
    struct x_qframe *q;
    unsigned int status;

    if (!conn->peer) return;
    /* the holder's low-water mark: replies to calls below it arrived */
    x_cache_ack( conn->peer, hdr->status );
    /* a TRY or an OPEN sent again after a relink: its success again, never a second acquisition or reference */
    if ((hdr->type == WEX_X_TRY || hdr->type == WEX_X_OPEN) && x_cache_replay( conn, hdr )) return;
    switch (hdr->type)
    {
    case WEX_X_OPEN:
        x_serve_open( conn, hdr, body, size );
        return;
    case WEX_X_PING:
        if (!(q = x_build( hdr->type | WEX_X_REPLY, hdr->call, 0, hdr->ref, body, size, NULL, 0, NULL, 0, 0 )))
        {
            x_reply( conn, hdr, STATUS_INVALID_PARAMETER, 0, NULL, 0 );
            return;
        }
        if (defer_pings)  /* the lab: held as a request in flight */
        {
            list_add_tail( &conn->deferred, &q->entry );
            conn->inflight_in++;
            x_account_note( conn->account, CAP_INFLIGHT, conn->inflight_in );
            return;
        }
        x_transmit( conn, q );
        return;
    case WEX_X_INFO:  /* X3: the kind's information, from the owner's own data, for the rights in the entry */
    {
        struct wex_x_info_req info;
        void *data = NULL;
        size_t data_size = 0;

        if (size != sizeof(info))
        {
            x_reply( conn, hdr, STATUS_INVALID_PARAMETER, hdr->ref, NULL, 0 );
            return;
        }
        memcpy( &info, body, sizeof(info) );
        if (!(ref = x_ref_lookup( conn->peer, hdr->ref )))
        {
            x_reply( conn, hdr, STATUS_INVALID_HANDLE, hdr->ref, NULL, 0 );
            return;
        }
        status = ref->ops->info ? ref->ops->info( ref->obj, ref->access, info.info_class, &data, &data_size )
                                : STATUS_NOT_SUPPORTED;
        if (!status && data_size > WEX_SESSION0_MAX_MSG - sizeof(*hdr)) status = STATUS_BUFFER_OVERFLOW;
        x_reply( conn, hdr, status, ref->id, status ? NULL : data, status ? 0 : data_size );
        free( data );
        return;
    }
    case WEX_X_READ:   /* X4: the object's memory, for the rights in the entry */
    case WEX_X_TOKEN:  /* X4: the object's token described, checked for the caller's token in the entry */
    {
        struct wex_x_read_req rd;
        struct wex_x_token_req tk;
        struct wex_x_token_reply granted_reply = { 0, 0 };
        void *data = NULL, *body_out = NULL, *joined = NULL;
        size_t data_size = 0, out_size;

        /* a TOKEN request carries the description of OpenProcessToken's caller after its body (the review: the caller
         * decides, as on Windows, not the identity that opened the process) */
        if (hdr->type == WEX_X_READ ? (size != sizeof(rd) || hdr->desc_size)
                                    : (size < sizeof(tk) || hdr->desc_size != size - sizeof(tk) || !hdr->desc_size))
        {
            x_reply( conn, hdr, STATUS_INVALID_PARAMETER, hdr->ref, NULL, 0 );
            return;
        }
        if (!(ref = x_ref_lookup( conn->peer, hdr->ref )))
        {
            x_reply( conn, hdr, STATUS_INVALID_HANDLE, hdr->ref, NULL, 0 );
            return;
        }
        if (hdr->type == WEX_X_READ)
        {
            memcpy( &rd, body, sizeof(rd) );
            if (!rd.size || rd.size > WEX_X_MAX_READ) status = STATUS_INVALID_PARAMETER;
            else status = ref->ops->read ? ref->ops->read( ref->obj, ref->access, rd.addr, rd.size, &data, &data_size )
                                         : STATUS_NOT_SUPPORTED;
            body_out = data;
            out_size = data_size;
        }
        else
        {
            struct token *caller;

            memcpy( &tk, body, sizeof(tk) );
            /* condition 1: the caller's description becomes a token only through link_bound_token */
            if ((caller = link_bound_token( conn->peer->uid, conn->peer->session, body + sizeof(tk), hdr->desc_size,
                                            &status )))
            {
                status = ref->ops->token ? ref->ops->token( ref->obj, ref->access, caller, tk.access,
                                                            &granted_reply.granted, &data, &data_size )
                                         : STATUS_NOT_SUPPORTED;
                release_object( caller );
            }
            out_size = sizeof(granted_reply) + data_size;
            if (!status && !(joined = malloc( out_size ))) status = STATUS_NO_MEMORY;
            if (!status)
            {
                memcpy( joined, &granted_reply, sizeof(granted_reply) );
                memcpy( (char *)joined + sizeof(granted_reply), data, data_size );
            }
            body_out = joined;
        }
        if (!status && out_size > WEX_SESSION0_MAX_MSG - sizeof(*hdr)) status = STATUS_BUFFER_OVERFLOW;
        x_reply( conn, hdr, status, ref->id, status ? NULL : body_out, status ? 0 : out_size );
        free( joined );
        free( data );
        return;
    }
    case WEX_X_QUERY:
    case WEX_X_RELEASE:
    case WEX_X_SUBSCRIBE:
    case WEX_X_CANCEL:
    case WEX_X_TRY:
        break;
    default:
        x_reply( conn, hdr, STATUS_NOT_SUPPORTED, 0, NULL, 0 );
        return;
    }
    if (size)
    {
        x_reply( conn, hdr, STATUS_INVALID_PARAMETER, hdr->ref, NULL, 0 );
        return;
    }
    ref = x_ref_lookup( conn->peer, hdr->ref );
    switch (hdr->type)
    {
    case WEX_X_RELEASE:  /* idempotent: a second release, or one of an id this peer does not hold, does nothing */
        if (ref) x_ref_free( ref );
        x_reply( conn, hdr, STATUS_SUCCESS, hdr->ref, NULL, 0 );
        return;
    case WEX_X_CANCEL:   /* idempotent */
        if (ref) x_unsubscribe( ref );
        x_reply( conn, hdr, STATUS_SUCCESS, hdr->ref, NULL, 0 );
        return;
    }
    if (!ref)
    {
        x_reply( conn, hdr, STATUS_INVALID_HANDLE, hdr->ref, NULL, 0 );
        return;
    }
    switch (hdr->type)
    {
    case WEX_X_QUERY:
        granted.access = ref->access;
        granted.ready = ref->ops->ready( ref->obj );
        x_reply( conn, hdr, STATUS_SUCCESS, ref->id, &granted, sizeof(granted) );
        return;
    case WEX_X_SUBSCRIBE:
        if (!(ref->access & SYNCHRONIZE))
        {
            x_reply( conn, hdr, STATUS_ACCESS_DENIED, ref->id, NULL, 0 );
            return;
        }
        if (!ref->subscribed)
        {
            if (conn->peer->account->subs >= WEX_X_MAX_SUBS)
            {
                x_reply( conn, hdr, STATUS_INSUFFICIENT_RESOURCES, ref->id, NULL, 0 );
                return;
            }
            ref->subscribed = 1;
            conn->peer->sub_count++;
            conn->peer->account->subs++;
            x_account_note( conn->peer->account, CAP_SUBS, conn->peer->account->subs );
        }
        x_reply( conn, hdr, STATUS_SUCCESS, ref->id, NULL, 0 );
        x_arm( ref );
        return;
    case WEX_X_TRY:
        if (!(ref->access & SYNCHRONIZE))
        {
            x_reply( conn, hdr, STATUS_ACCESS_DENIED, ref->id, NULL, 0 );
            return;
        }
        /* condition 2: the holder cannot cancel a try. A success is kept in the cache until the holder's low-water
         * mark passes it: replayed if the try comes again after a relink, given back if the peer's epoch ends first.
         * A failed try re-arms in the same step, as does a successful one for the next wait. */
        status = ref->ops->try_acquire( ref->obj );
        if (!status) x_cache_add( conn->peer, hdr, ref->id, NULL, ref->obj, ref->ops );
        x_reply( conn, hdr, status, ref->id, NULL, 0 );
        x_arm( ref );
        return;
    }
}

/* ---- the holder's calls ---- */

unsigned int x_call( struct x_peer *peer, unsigned int type, unsigned long long ref,
                     const void *body, size_t body_size, const void *desc, size_t desc_size,
                     const void *extra, size_t extra_size, int raw, x_done_fn done, void *ctx )
{
    struct x_conn *conn = peer ? peer->conn : NULL;
    struct x_call *call;

    /* a new call while the link is lost fails at once; calls already made wait for the relink */
    if (!conn) return STATUS_CONNECTION_DISCONNECTED;
    if (!raw && peer->call_count >= WEX_X_MAX_INFLIGHT) return STATUS_INSUFFICIENT_RESOURCES;
    if (!raw && conn->account->queued > WEX_X_MAX_QUEUED) return STATUS_INSUFFICIENT_RESOURCES;
    if (!(call = malloc( sizeof(*call) ))) return STATUS_NO_MEMORY;
    if (!++peer->next_call) peer->next_call = 1;
    call->call = peer->next_call;
    call->type = type;
    call->done = done;
    call->ctx = ctx;
    if (!(call->frame = x_build( type, call->call, 0, ref, body, body_size, desc, desc_size, extra, extra_size, raw )))
    {
        free( call );
        return STATUS_BUFFER_OVERFLOW;
    }
    list_add_tail( &peer->calls, &call->entry );
    peer->call_count++;
    if (!x_call_send( peer, call ))
    {
        list_remove( &call->entry );
        peer->call_count--;
        free( call->frame );
        free( call );
        return STATUS_CONNECTION_DISCONNECTED;
    }
    return STATUS_PENDING;
}

struct x_held *x_held_add( struct x_peer *peer, unsigned long long id, unsigned int granted )
{
    struct x_held *held;

    if (!(held = calloc( 1, sizeof(*held) ))) return NULL;
    held->peer = peer;
    held->id = id;
    held->granted = granted;
    held->handles = 1;
    list_add_tail( &x_all_held, &held->entry );
    return held;
}

struct x_held *x_held_find( unsigned long long id )
{
    struct x_held *held;

    LIST_FOR_EACH_ENTRY( held, &x_all_held, struct x_held, entry ) if (held->id == id) return held;
    return NULL;
}

void x_held_free( struct x_held *held )
{
    list_remove( &held->entry );
    free( held );
}

unsigned int x_peer_calls( struct x_peer *peer )
{
    return peer ? peer->call_count : 0;
}

/* ---- the lab's views ---- */

void x_set_defer( int on )
{
    struct x_conn *conn;

    defer_pings = on;
    if (on) return;
    LIST_FOR_EACH_ENTRY( conn, &conns, struct x_conn, entry )
    {
        while (!list_empty( &conn->deferred ))
        {
            struct x_qframe *q = LIST_ENTRY( list_head( &conn->deferred ), struct x_qframe, entry );
            list_remove( &q->entry );
            conn->inflight_in--;
            x_transmit( conn, q );
        }
    }
}

int x_send_raw_header( struct x_peer *peer, unsigned int type, unsigned int call, unsigned long long ref )
{
    struct x_qframe *q;

    if (!peer || !peer->conn || !(q = x_build( type, call, 0, ref, NULL, 0, NULL, 0, NULL, 0, 0 ))) return 0;
    return x_transmit( peer->conn, q );
}

void x_peer_pause( struct x_peer *peer, int paused )
{
    if (!peer->conn) return;  /* the connection's only: a relink reads again */
    peer->conn->paused = paused;
    x_update_events( peer->conn );
}

size_t x_peers_report( char *buf, size_t size )
{
    struct x_account *account;
    struct x_peer *peer;
    size_t len = 0;
    int n;

    LIST_FOR_EACH_ENTRY( peer, &peers, struct x_peer, entry )
    {
        struct x_conn *conn = peer->conn;
        unsigned int stalled = 0;
        struct list *p;

        if (conn) LIST_FOR_EACH( p, &conn->stalled ) stalled++;
        n = snprintf( buf + len, size - len,
                      "peer pid=%u uid=%u session=%u nonce=%llx link=%s conn=%u refs=%u subs=%u calls=%u "
                      "inflight=%u stalled=%u queued=%lu cached=%u stray=%u\n",
                      peer->epoch.pid, (unsigned int)peer->uid, peer->session, peer->epoch.nonce,
                      conn ? "up" : "lost", conn ? conn->number : 0, peer->ref_count, peer->sub_count,
                      peer->call_count, conn ? conn->inflight_in : 0, stalled,
                      conn ? (unsigned long)conn->queued : 0ul, peer->cached_count, peer->stray );
        if (n < 0 || (size_t)n >= size - len) return len;
        len += n;
    }
    LIST_FOR_EACH_ENTRY( account, &accounts, struct x_account, entry )
    {
        n = snprintf( buf + len, size - len, "account uid=%u refs=%u subs=%u queued=%lu\n",
                      (unsigned int)account->uid, account->refs, account->subs, (unsigned long)account->queued );
        if (n < 0 || (size_t)n >= size - len) return len;
        len += n;
    }
    return len;
}
