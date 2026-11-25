/*
 * WineExplorer X0 lab: a control socket and an object kind for the tests of the cross-server protocol
 * (never in a shipped build)
 *
 * Copyright 2026 WineExplorer contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * docs/SESSION0_CROSS_SERVER.md, "Appendix: X0 wire protocol and how its tests reach it". Compiled only with
 * -DWEX_X0_LAB_TEST (scripts/lab/r8-lab-build.sh). Every lab server (session 0 and each linked user's server) listens
 * on wex-x0-lab in its server directory, mode 0600, and serves root only: one text command per connection, one text
 * answer. The lab object kind (WEX_X_KIND_LAB) is a named counter: ready while above 0; a try takes one.
 *
 * Commands (positional words, then key=value options; peer=S picks the peer of session S, for session 0):
 *   epoch | peers | held
 *   set NAME COUNT | get NAME                                       (the owner's counters)
 *   open NAME [n=N] [access=HEX] [level=anon|ident|imp] [session=S] [nowait]
 *   query|sub|cancel|try|release ID [raw] [nowait]                   (ID in hex; raw: past the holder's table)
 *   sub all | release all [nowait]
 *   ping [n=N] [size=BYTES] [raw] [nowait]                           (size: the whole frame)
 *   batch N                                                          (a nowait command's results so far)
 *   defer on|off | pause | resume | droplink | notices ID
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
#include "wex_e1lab.h"

#ifdef WEX_X0_LAB_TEST

#define LAB_SOCKET  "wex-x0-lab"
#define LAB_REPLY   65536
#define LAB_WINDOW  64

/* ---- the lab object kind: a named counter ---- */

struct lab_counter
{
    struct object  obj;
    struct list    entry;
    char           name[64];
    unsigned int   count;
};

static struct list counters = LIST_INIT( counters );

static void counter_dump( struct object *obj, int verbose )
{
    struct lab_counter *counter = (struct lab_counter *)obj;
    fprintf( stderr, "X0 lab counter %s=%u\n", counter->name, counter->count );
}

static const struct object_ops counter_ops =
{
    .size = sizeof(struct lab_counter),
    .type = &no_type,
    .dump = counter_dump,
};

/* the counter of a name, made at 0 when new; a reference for the caller */
static struct lab_counter *counter_get( const char *name, size_t len )
{
    struct lab_counter *counter;

    if (!len || len >= sizeof(counter->name)) return NULL;
    LIST_FOR_EACH_ENTRY( counter, &counters, struct lab_counter, entry )
        if (strlen( counter->name ) == len && !memcmp( counter->name, name, len ))
            return (struct lab_counter *)grab_object( counter );
    if (!(counter = alloc_object( &counter_ops ))) return NULL;
    memcpy( counter->name, name, len );
    counter->name[len] = 0;
    counter->count = 0;
    list_add_tail( &counters, &counter->entry );  /* the table keeps the first reference */
    return (struct lab_counter *)grab_object( counter );
}

static unsigned int lab_open( const void *name, unsigned int size, unsigned int access, struct token *token,
                              struct object **obj, unsigned int *granted )
{
    struct lab_counter *counter = counter_get( name, size );

    if (!counter) return STATUS_OBJECT_NAME_INVALID;
    *obj = &counter->obj;
    *granted = access;  /* the lab grants what is asked; operations check the rights in the reference */
    return STATUS_SUCCESS;
}

static int lab_ready( struct object *obj )
{
    return ((struct lab_counter *)obj)->count > 0;
}

static unsigned int lab_try( struct object *obj )
{
    struct lab_counter *counter = (struct lab_counter *)obj;

    if (!counter->count) return STATUS_TIMEOUT;
    counter->count--;
    return STATUS_SUCCESS;
}

static void lab_give_back( struct object *obj )
{
    ((struct lab_counter *)obj)->count++;
    fprintf( stderr, "x0lab: a try's success could not be sent: %s given back\n", ((struct lab_counter *)obj)->name );
    x_object_changed( obj );
}

static const struct x_kind_ops lab_kind =
{
    WEX_X_KIND_LAB, lab_open, lab_ready, lab_try, lab_give_back
};

/* ---- the control socket ---- */

struct lab_client
{
    struct object  obj;
    struct fd     *fd;
    int            started;
};

static void client_dump( struct object *obj, int verbose )
{
    fprintf( stderr, "X0 lab client\n" );
}

static void client_destroy( struct object *obj )
{
    struct lab_client *client = (struct lab_client *)obj;
    if (client->fd) release_object( client->fd );
}

static const struct object_ops client_ops =
{
    .size    = sizeof(struct lab_client),
    .type    = &no_type,
    .dump    = client_dump,
    .destroy = client_destroy,
};

static void client_event( struct fd *fd, int event );
static void listen_event( struct fd *fd, int event );
static const struct fd_ops client_fd_ops = { .poll_event = client_event };
static const struct fd_ops listen_fd_ops = { .poll_event = listen_event };

/* answer a command and end the client's connection (its reference) */
static void lab_reply( struct lab_client *client, const char *text )
{
    send( get_unix_fd( client->fd ), text, strlen( text ), MSG_NOSIGNAL | MSG_DONTWAIT );
    release_object( client );
}

/* ---- batches of calls ---- */

enum lab_op { OP_OPEN, OP_PING, OP_IDS };

struct lab_batch
{
    struct list           entry;
    unsigned int          number;
    enum lab_op           op;
    unsigned int          type;        /* OP_IDS: the request */
    struct x_peer        *peer;
    unsigned int          total, issued, done, ok;
    struct { unsigned int status, count; } st[8];
    unsigned long long    ids[8];      /* OPEN: the first ids */
    unsigned int          id_count;
    unsigned long long   *list;        /* OP_IDS: the references */
    int                   raw;
    unsigned int          access;
    char                  name[64];
    void                 *desc;
    data_size_t           desc_size;
    size_t                ping_size;
    struct lab_client    *client;      /* answered when the batch is done; NULL: nowait */
};

struct lab_call
{
    struct lab_batch     *batch;
    unsigned long long    id;
};

static struct list batches = LIST_INIT( batches );
static unsigned int batch_numbers;

static void batch_status( struct lab_batch *batch, unsigned int status )
{
    unsigned int i;

    if (!status) batch->ok++;
    for (i = 0; i < ARRAY_SIZE(batch->st); i++)
    {
        if (batch->st[i].count && batch->st[i].status != status) continue;
        batch->st[i].status = status;
        batch->st[i].count++;
        return;
    }
}

static size_t batch_report( const struct lab_batch *batch, char *buf, size_t size )
{
    size_t len;
    unsigned int i;

    len = snprintf( buf, size, "batch=%u total=%u done=%u ok=%u st=", batch->number, batch->total, batch->done,
                    batch->ok );
    for (i = 0; i < ARRAY_SIZE(batch->st) && batch->st[i].count && len < size; i++)
        len += snprintf( buf + len, size - len, "%s%x:%u", i ? "," : "", batch->st[i].status, batch->st[i].count );
    if (batch->id_count && len < size)
    {
        len += snprintf( buf + len, size - len, " id=%llx ids=", batch->ids[0] );
        for (i = 0; i < batch->id_count && len < size; i++)
            len += snprintf( buf + len, size - len, "%s%llx", i ? "," : "", batch->ids[i] );
    }
    if (len < size) len += snprintf( buf + len, size - len, "\n" );
    return len;
}

static void batch_pump( struct lab_batch *batch );

static void batch_done( void *ctx, unsigned int status, const struct wex_x_header *reply, const void *body,
                        size_t size )
{
    struct lab_call *call = ctx;
    struct lab_batch *batch = call->batch;
    struct x_held *held;

    if (!status && reply)
    {
        if (batch->op == OP_OPEN)
        {
            struct wex_x_granted granted = { 0, 0 };

            if (size >= sizeof(granted)) memcpy( &granted, body, sizeof(granted) );
            x_held_add( batch->peer, reply->ref, granted.access );
            if (batch->id_count < ARRAY_SIZE(batch->ids)) batch->ids[batch->id_count++] = reply->ref;
        }
        else if (batch->op == OP_IDS && batch->type == WEX_X_RELEASE && (held = x_held_find( call->id )) &&
                 held->peer == batch->peer)
            x_held_free( held );
        else if (batch->op == OP_IDS && batch->type == WEX_X_QUERY && size >= sizeof(struct wex_x_granted) &&
                 batch->id_count < ARRAY_SIZE(batch->ids))
        {
            const struct wex_x_granted *granted = body;
            batch->ids[batch->id_count++] = ((unsigned long long)granted->ready << 32) | granted->access;
        }
    }
    free( call );
    batch->done++;
    batch_status( batch, status );
    batch_pump( batch );
}

static void batch_pump( struct lab_batch *batch )
{
    char text[1024];

    while (batch->issued < batch->total && (!batch->client || batch->issued - batch->done < LAB_WINDOW))
    {
        struct lab_call *call = malloc( sizeof(*call) );
        unsigned int status = STATUS_NO_MEMORY;

        batch->issued++;
        if (call)
        {
            call->batch = batch;
            call->id = 0;
            switch (batch->op)
            {
            case OP_OPEN:
            {
                struct wex_x_open open = { WEX_X_KIND_LAB, batch->access, strlen( batch->name ), 0 };
                status = x_call( batch->peer, WEX_X_OPEN, 0, &open, sizeof(open), batch->desc, batch->desc_size,
                                 batch->name, open.name_size, batch->raw, batch_done, call );
                break;
            }
            case OP_PING:
            {
                size_t body = batch->ping_size > sizeof(struct wex_x_header) ?
                              batch->ping_size - sizeof(struct wex_x_header) : 0;
                char *data = calloc( 1, body + 1 );

                status = data ? x_call( batch->peer, WEX_X_PING, 0, data, body, NULL, 0, NULL, 0, batch->raw,
                                        batch_done, call ) : STATUS_NO_MEMORY;
                free( data );
                break;
            }
            case OP_IDS:
            {
                struct x_held *held = NULL;
                struct x_peer *peer = batch->peer;

                call->id = batch->list[batch->issued - 1];
                if (!batch->raw)
                {
                    if (!(held = x_held_find( call->id )))
                    {
                        status = STATUS_INVALID_HANDLE;  /* not held here */
                        break;
                    }
                    if (!(peer = held->peer))
                    {
                        status = STATUS_CONNECTION_DISCONNECTED;  /* the owner's epoch ended */
                        break;
                    }
                    if (batch->type == WEX_X_TRY) held->hint = 0;
                }
                status = x_call( peer, batch->type, call->id, NULL, 0, NULL, 0, NULL, 0, batch->raw, batch_done, call );
                break;
            }
            }
        }
        if (status == STATUS_PENDING) continue;
        free( call );
        batch->done++;
        batch_status( batch, status );
    }
    if (batch->client && batch->done == batch->total)
    {
        struct lab_client *client = batch->client;

        batch->client = NULL;
        batch_report( batch, text, sizeof(text) );
        lab_reply( client, text );
    }
}

static struct lab_batch *batch_new( struct lab_client *client, enum lab_op op, struct x_peer *peer, unsigned int n,
                                    int nowait )
{
    struct lab_batch *batch = calloc( 1, sizeof(*batch) );

    if (!batch) return NULL;
    batch->number = ++batch_numbers;
    batch->op = op;
    batch->peer = peer;
    batch->total = n;
    batch->client = nowait ? NULL : client;
    list_add_tail( &batches, &batch->entry );
    return batch;
}

/* start a batch: nowait answers with its number once everything is sent */
static void batch_start( struct lab_client *client, struct lab_batch *batch, int nowait )
{
    char text[1024];

    batch_pump( batch );
    if (!nowait) return;
    batch_report( batch, text, sizeof(text) );
    lab_reply( client, text );
}

/* ---- commands ---- */

struct lab_args
{
    char        *word[4];
    unsigned int words;
    const char  *opt_n, *opt_access, *opt_level, *opt_session, *opt_peer, *opt_size;
    int          raw, nowait;
};

static void parse_args( char *line, struct lab_args *args )
{
    char *tok, *save = NULL;

    memset( args, 0, sizeof(*args) );
    for (tok = strtok_r( line, " \t\r\n", &save ); tok; tok = strtok_r( NULL, " \t\r\n", &save ))
    {
        if (!strcmp( tok, "raw" )) args->raw = 1;
        else if (!strcmp( tok, "nowait" )) args->nowait = 1;
        else if (!strncmp( tok, "n=", 2 )) args->opt_n = tok + 2;
        else if (!strncmp( tok, "access=", 7 )) args->opt_access = tok + 7;
        else if (!strncmp( tok, "level=", 6 )) args->opt_level = tok + 6;
        else if (!strncmp( tok, "session=", 8 )) args->opt_session = tok + 8;
        else if (!strncmp( tok, "peer=", 5 )) args->opt_peer = tok + 5;
        else if (!strncmp( tok, "size=", 5 )) args->opt_size = tok + 5;
        else if (args->words < ARRAY_SIZE(args->word)) args->word[args->words++] = tok;
    }
}

/* the lab's caller token: the first interactive token of this server's session, or SYSTEM in session 0;
 * level: an impersonation copy at that level */
static struct token *lab_token( const char *level )
{
    struct token *base, *token;
    int lvl;

    base = session0_server ? token_create_service( SERVICE_ACCOUNT_SYSTEM, 0 )
                           : token_create_first_interactive( default_session_id );
    if (!base || !level) return base;
    if (!strcmp( level, "anon" )) lvl = SecurityAnonymous;
    else if (!strcmp( level, "ident" )) lvl = SecurityIdentification;
    else if (!strcmp( level, "delegate" )) lvl = SecurityDelegation;
    else lvl = SecurityImpersonation;
    token = token_duplicate( base, FALSE, lvl, NULL, NULL, 0, NULL, 0, FALSE );
    release_object( base );
    return token;
}

static void lab_command( struct lab_client *client, char *line )
{
    static char text[LAB_REPLY];
    struct lab_args args;
    struct x_peer *peer;
    struct lab_batch *batch;
    struct lab_counter *counter;
    struct x_held *held;
    unsigned int n;
    const char *verb;

    parse_args( line, &args );
    verb = args.words ? args.word[0] : "";
    peer = x_peer_of_session( args.opt_peer ? strtoul( args.opt_peer, NULL, 0 ) : 0 );
    n = args.opt_n ? strtoul( args.opt_n, NULL, 0 ) : 1;

    if (!strcmp( verb, "epoch" ))
    {
        struct wex_x_epoch epoch;
        x_own_epoch( &epoch );
        snprintf( text, sizeof(text), "pid=%u start=%llu nonce=%llx\n", epoch.pid, epoch.start, epoch.nonce );
    }
    else if (!strcmp( verb, "peers" ))
    {
        size_t len = x_peers_report( text, sizeof(text) - 8 );
        if (!len) strcpy( text, "none\n" );
    }
    else if (!strcmp( verb, "held" ))
    {
        unsigned int live = 0, dead = 0;
        LIST_FOR_EACH_ENTRY( held, &x_all_held, struct x_held, entry ) held->peer ? live++ : dead++;
        snprintf( text, sizeof(text), "live=%u dead=%u calls=%u\n", live, dead, x_peer_calls( peer ));
    }
    else if (!strcmp( verb, "set" ) && args.words == 3)
    {
        if (!(counter = counter_get( args.word[1], strlen( args.word[1] )))) strcpy( text, "status=c0000033\n" );
        else
        {
            counter->count = strtoul( args.word[2], NULL, 0 );
            x_object_changed( &counter->obj );
            release_object( counter );
            strcpy( text, "status=0\n" );
        }
    }
    else if (!strcmp( verb, "get" ) && args.words == 2)
    {
        if (!(counter = counter_get( args.word[1], strlen( args.word[1] )))) strcpy( text, "status=c0000033\n" );
        else
        {
            snprintf( text, sizeof(text), "count=%u\n", counter->count );
            release_object( counter );
        }
    }
    else if (!strcmp( verb, "open" ) && args.words == 2 && strlen( args.word[1] ) < 64)
    {
        struct token *token = lab_token( args.opt_level );
        unsigned int status = STATUS_NO_MEMORY;
        data_size_t size = 0;
        void *desc = token ? x_describe_token( token, &size, &status ) : NULL;

        if (token) release_object( token );
        if (!desc)
        {
            snprintf( text, sizeof(text), "batch=0 total=%u done=%u ok=0 st=%x:%u\n", n, n, status, n );
            lab_reply( client, text );
            return;
        }
        /* a description naming another session than the link's (the receiver must refuse it) */
        if (args.opt_session)
        {
            unsigned int session = strtoul( args.opt_session, NULL, 0 );
            memcpy( (char *)desc + 2 * sizeof(unsigned int), &session, sizeof(session) );
        }
        if (!(batch = batch_new( client, OP_OPEN, peer, n, args.nowait )))
        {
            free( desc );
            lab_reply( client, "status=c0000017\n" );
            return;
        }
        batch->desc = desc;
        batch->desc_size = size;
        batch->raw = args.raw;
        batch->access = args.opt_access ? strtoul( args.opt_access, NULL, 16 ) : SYNCHRONIZE | 0x1f0003;
        strcpy( batch->name, args.word[1] );
        batch_start( client, batch, args.nowait );
        return;
    }
    else if (!strcmp( verb, "ping" ))
    {
        if (!(batch = batch_new( client, OP_PING, peer, n, args.nowait )))
        {
            lab_reply( client, "status=c0000017\n" );
            return;
        }
        batch->raw = args.raw;
        batch->ping_size = args.opt_size ? strtoul( args.opt_size, NULL, 0 ) : 64;
        batch_start( client, batch, args.nowait );
        return;
    }
    else if ((!strcmp( verb, "query" ) || !strcmp( verb, "sub" ) || !strcmp( verb, "cancel" ) ||
              !strcmp( verb, "try" ) || !strcmp( verb, "release" )) && args.words == 2)
    {
        unsigned int type = !strcmp( verb, "query" ) ? WEX_X_QUERY : !strcmp( verb, "sub" ) ? WEX_X_SUBSCRIBE :
                            !strcmp( verb, "cancel" ) ? WEX_X_CANCEL : !strcmp( verb, "try" ) ? WEX_X_TRY :
                            WEX_X_RELEASE;
        unsigned int count = 0;

        if (!strcmp( args.word[1], "all" ))
        {
            LIST_FOR_EACH_ENTRY( held, &x_all_held, struct x_held, entry ) if (held->peer == peer) count++;
        }
        else count = n;
        if (!(batch = batch_new( client, OP_IDS, peer, count, args.nowait )) ||
            !(batch->list = calloc( count + 1, sizeof(*batch->list) )))
        {
            lab_reply( client, "status=c0000017\n" );
            return;
        }
        batch->type = type;
        batch->raw = args.raw;
        if (!strcmp( args.word[1], "all" ))
        {
            unsigned int i = 0;
            LIST_FOR_EACH_ENTRY( held, &x_all_held, struct x_held, entry )
                if (held->peer == peer && i < count) batch->list[i++] = held->id;
        }
        else
        {
            unsigned int i;
            unsigned long long id = strtoull( args.word[1], NULL, 16 );
            for (i = 0; i < count; i++) batch->list[i] = id;  /* n=2: the same request twice */
        }
        batch_start( client, batch, args.nowait );
        return;
    }
    else if (!strcmp( verb, "batch" ) && args.words == 2)
    {
        unsigned int number = strtoul( args.word[1], NULL, 0 );
        strcpy( text, "status=c0000034\n" );
        LIST_FOR_EACH_ENTRY( batch, &batches, struct lab_batch, entry )
            if (batch->number == number) batch_report( batch, text, sizeof(text) );
    }
    else if (!strcmp( verb, "defer" ) && args.words == 2)
    {
        x_set_defer( !strcmp( args.word[1], "on" ));
        strcpy( text, "status=0\n" );
    }
    else if (!strcmp( verb, "pause" ) || !strcmp( verb, "resume" ))
    {
        if (!peer) strcpy( text, "status=c000020c\n" );
        else
        {
            x_peer_pause( peer, !strcmp( verb, "pause" ));
            strcpy( text, "status=0\n" );
        }
    }
    else if (!strcmp( verb, "droplink" ) && !session0_server)
    {
        session0_lab_drop_link();
        strcpy( text, "status=0\n" );
    }
    else if (!strcmp( verb, "notices" ) && args.words == 2)
    {
        if (!(held = x_held_find( strtoull( args.word[1], NULL, 16 )))) strcpy( text, "status=c0000008\n" );
        else snprintf( text, sizeof(text), "notices=%u hint=%d live=%d\n", held->notices, held->hint, !!held->peer );
    }
    else strcpy( text, "status=c000000d unknown command\n" );
    lab_reply( client, text );
}

static void client_event( struct fd *fd, int event )
{
    struct lab_client *client = get_fd_user( fd );
    char line[4096];
    ssize_t n;

    if (client->started) return;
    if (!(event & POLLIN))
    {
        set_fd_events( fd, -1 );
        release_object( client );
        return;
    }
    n = recv( get_unix_fd( fd ), line, sizeof(line) - 1, MSG_DONTWAIT );
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    client->started = 1;
    set_fd_events( fd, -1 );  /* one command per connection */
    if (n <= 0)
    {
        release_object( client );
        return;
    }
    line[n] = 0;
    lab_command( client, line );
}

static void listen_event( struct fd *fd, int event )
{
    struct lab_client *client;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int sock;

    if (!(event & POLLIN)) return;
    if ((sock = accept4( get_unix_fd( fd ), NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC )) == -1) return;
    if (getsockopt( sock, SOL_SOCKET, SO_PEERCRED, &cred, &len ) || cred.uid != 0 ||
        !(client = alloc_object( &client_ops )))
    {
        close( sock );  /* root only */
        return;
    }
    client->fd = NULL;
    client->started = 0;
    if (!(client->fd = create_anonymous_fd( &client_fd_ops, sock, &client->obj, 0 )))
    {
        release_object( client );
        return;
    }
    set_fd_events( client->fd, POLLIN );
}

void x0lab_init(void)
{
    struct sockaddr_un addr;
    struct fd *listener;
    int sock;

    x_register_kind( &lab_kind );
    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, LAB_SOCKET );  /* in the server directory, the working directory */
    unlink( LAB_SOCKET );
    if ((sock = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 )) == -1 ||
        bind( sock, (struct sockaddr *)&addr, sizeof(addr) ) == -1 || chmod( LAB_SOCKET, 0600 ) == -1 ||
        listen( sock, 16 ) == -1)
    {
        fprintf( stderr, "x0lab: no control socket: %s\n", strerror( errno ));
        if (sock != -1) close( sock );
        return;
    }
    if (!(listener = create_anonymous_fd( &listen_fd_ops, sock, NULL, 0 ))) return;
    set_fd_events( listener, POLLIN );  /* kept for the life of the server */
    fprintf( stderr, "x0lab: control socket ready\n" );
}

#endif /* WEX_X0_LAB_TEST */
