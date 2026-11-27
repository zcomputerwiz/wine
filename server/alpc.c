/*
 * Server-side advanced local procedure call management
 *
 * Copyright 2026 Zhiyi Zhang for CodeWeavers
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
 *
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>

#include "windef.h"
#include "winternl.h"
#include "ntstatus.h"

#include "file.h"
#include "handle.h"
#include "thread.h"
#include "process.h"
#include "security.h"
#include "request.h"

/* message types, as seen by the receiver */
#define ALPC_TYPE_REQUEST            1
#define ALPC_TYPE_REPLY              2
#define ALPC_TYPE_DATAGRAM           3
#define ALPC_TYPE_PORT_CLOSED        5
#define ALPC_TYPE_CONNECTION_REQUEST 10
#define ALPC_TYPE_CONNECTION_REPLY   11
/* set by the system on the requests a server receives (measured on Windows) */
#define ALPC_TYPE_RECEIVED_FLAG      0x2000
#define ALPC_MAX_DATA                0x10000
#define ALPC_PORT_CONNECT            0x0001  /* the right to connect (the bit of ALPC_PORT_QUERY_STATE) */

static const WCHAR alpc_port_name[] = {'A','L','P','C',' ','P','o','r','t'};

struct type_descr alpc_port_type =
{
    { alpc_port_name, sizeof(alpc_port_name) }, /* name */
    ALPC_PORT_ALL_ACCESS,                       /* valid_access */
    {                                           /* mapping */
         STANDARD_RIGHTS_READ | ALPC_PORT_QUERY_STATE,
         DELETE | ALPC_PORT_QUERY_STATE,
         0,
         ALPC_PORT_ALL_ACCESS
    },
};

enum alpc_port_enum_type
{
    CONNECTION_PORT,
    COMMUNICATION_PORT
};

enum alpc_port_status
{
    UNINITIALIZED,
    CONNECTED,
    REFUSED,
    DISCONNECTED
};

struct alpc_port;

struct alpc_message
{
    struct list       entry;
    unsigned int      type;
    unsigned int      id;
    unsigned int      callback_id;
    unsigned int      pid, tid;        /* sender */
    struct token     *token;           /* token of the sending thread when the message was sent (referenced), or NULL */
    int               sec_level;       /* impersonation level the sender gave with its security attribute */
    unsigned int      status;          /* carried by connection replies */
    unsigned int      sync_tid;        /* request: sent synchronously by this thread; reply: only this thread receives it */
    struct alpc_port *from;            /* server communication port the message came through (not referenced) */
    data_size_t       len;
    unsigned char     data[1];
};

/* a message received by a server that is waiting for its reply */
struct alpc_outstanding
{
    struct list       entry;
    unsigned int      id;
    unsigned int      callback_id;
    int               is_connect;
    struct token     *token;           /* token of the sender when the request was sent (referenced), or NULL */
    int               sec_level;       /* impersonation level the sender gave with its security attribute */
    unsigned int      sync_tid;        /* the thread that sent the request synchronously, or 0 */
    struct alpc_port *from;            /* server communication port (not referenced) */
};

struct alpc_port
{
    struct object            obj;                   /* object header */
    enum alpc_port_enum_type type;                  /* communication port or connection port */
    enum alpc_port_status    status;                /* port status */
    struct thread           *thread;                /* thread owning the port */
    unsigned int             flags;                 /* flags in port attributes */
    mem_size_t               max_msg_len;           /* max message length in port attributes */
    int                      is_client;             /* communication port of the connecting side */
    struct alpc_port        *conn;                  /* server communication port: its connection port (referenced) */
    struct alpc_port        *peer;                  /* client: the server side (referenced); server: the client (not referenced) */
    struct list              queue;                 /* messages waiting to be received */
    struct list              outstanding;           /* connection port: requests waiting for a reply */
    mem_size_t               context;               /* port context */
    unsigned int             sequence;              /* messages received so far */
};

struct alpc_port_init_data
{
    enum alpc_port_enum_type type;
    unsigned int             flags;
    mem_size_t               max_msg_len;
};

static unsigned int next_message_id = 0x100;
static unsigned int next_callback_id = 0x1000;

static void alpc_port_dump( struct object *obj, int verbose );
static bool alpc_port_init( struct object *obj, const void *init_data );
static int alpc_port_signaled( struct object *obj, struct wait_queue_entry *entry );
static void alpc_port_satisfied( struct object *obj, struct wait_queue_entry *entry );
static void alpc_port_destroy( struct object *obj );

static const struct object_ops alpc_port_ops =
{
    .size         = sizeof(struct alpc_port),
    .type         = &alpc_port_type,
    .dump         = alpc_port_dump,
    .init         = alpc_port_init,
    .add_queue    = add_queue,
    .remove_queue = remove_queue,
    .signaled     = alpc_port_signaled,
    .satisfied    = alpc_port_satisfied,
    .destroy      = alpc_port_destroy
};

static void alpc_port_dump( struct object *obj, int verbose )
{
    struct alpc_port *port = (struct alpc_port *)obj;
    assert( obj->ops == &alpc_port_ops );
    fprintf( stderr, "ALPC Port type=%d status=%d\n", port->type, port->status );
}

static void init_port( struct alpc_port *port, enum alpc_port_enum_type type, unsigned int flags, mem_size_t max_msg_len )
{
    port->type        = type;
    port->flags       = flags;
    port->max_msg_len = max_msg_len;
    port->status      = UNINITIALIZED;
    port->thread      = (struct thread *)grab_object( current );
    port->is_client   = 0;
    port->conn        = NULL;
    port->peer        = NULL;
    port->context     = 0;
    port->sequence    = 0;
    list_init( &port->queue );
    list_init( &port->outstanding );
}

static bool alpc_port_init( struct object *obj, const void *init_data )
{
    const struct alpc_port_init_data *data = init_data;

    init_port( (struct alpc_port *)obj, data->type, data->flags, data->max_msg_len );
    return true;
}

/* The next message the port gives to thread (NULL: to a waiter that is no thread, such as a wait completion packet).
 * The reply to a synchronous request goes to the thread that sent the request only: on Windows it does not make the
 * port signaled for anyone else (CoreMessagingXP's wait packet on its port would otherwise fire for a reply its own
 * synchronous call takes, and its receive then waits for a message that never comes). */
static struct alpc_message *find_message_for( struct alpc_port *port, struct thread *thread )
{
    struct alpc_port *queue_port = port;
    struct alpc_message *msg;

    if (port->type == COMMUNICATION_PORT && !port->is_client) queue_port = port->conn;
    if (!queue_port) return NULL;

    LIST_FOR_EACH_ENTRY( msg, &queue_port->queue, struct alpc_message, entry )
    {
        if (port->type == COMMUNICATION_PORT && !port->is_client && msg->from != port) continue;
        if (msg->sync_tid && (msg->type & 0xff) == ALPC_TYPE_REPLY && (!thread || thread->id != msg->sync_tid)) continue;
        return msg;
    }
    return NULL;
}

static int alpc_port_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct alpc_port *port = (struct alpc_port *)obj;

    assert( obj->ops == &alpc_port_ops );
    return find_message_for( port, entry ? get_wait_queue_thread( entry ) : NULL ) != NULL;
}

static void alpc_port_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
}

static void free_message( struct alpc_message *msg )
{
    if (msg->token) release_object( msg->token );
    free( msg );
}

static void free_outstanding( struct alpc_outstanding *out )
{
    if (out->token) release_object( out->token );
    free( out );
}

/* The token of the sending thread, taken when the message is sent: what is impersonated later is exactly this one.
 * Only a message sent with a security attribute carries a token (measured on Windows 11: without the attribute the
 * impersonation succeeds and the thread gets nothing); the attribute also gives the level the sender allows. */
static void capture_sender_token( struct alpc_message *msg, int sec_level )
{
    struct token *token;

    msg->token = NULL;
    msg->sec_level = -1;
    if (sec_level < 0 || !current) return;
    if ((token = thread_get_impersonation_token( current )))
    {
        msg->token = (struct token *)grab_object( token );
        msg->sec_level = sec_level;
    }
}

static struct alpc_message *alloc_message( unsigned int type, unsigned int id, unsigned int callback_id,
                                           const void *data, data_size_t len )
{
    struct alpc_message *msg = mem_alloc( sizeof(*msg) + len );

    if (!msg) return NULL;
    msg->type        = type;
    msg->id          = id;
    msg->callback_id = callback_id;
    msg->pid         = current ? current->process->id : 0;
    msg->tid         = current ? current->id : 0;
    msg->status      = 0;
    msg->sync_tid    = 0;
    msg->token       = NULL;
    msg->sec_level   = -1;
    msg->from        = NULL;
    msg->len         = len;
    if (len) memcpy( msg->data, data, len );
    return msg;
}

/* add a message to the queue of the port that receives it, and wake the waiters */
static void queue_message( struct alpc_port *queue_port, struct alpc_message *msg )
{
    list_add_tail( &queue_port->queue, &msg->entry );
    wake_up( &queue_port->obj, 0 );
    if (msg->from) wake_up( &msg->from->obj, 0 );
}

static void purge_messages_from( struct alpc_port *conn, struct alpc_port *from )
{
    struct alpc_message *msg, *next;
    struct alpc_outstanding *out, *out_next;

    LIST_FOR_EACH_ENTRY_SAFE( msg, next, &conn->queue, struct alpc_message, entry )
    {
        if (msg->from != from) continue;
        list_remove( &msg->entry );
        free_message( msg );
    }
    LIST_FOR_EACH_ENTRY_SAFE( out, out_next, &conn->outstanding, struct alpc_outstanding, entry )
    {
        if (out->from != from) continue;
        list_remove( &out->entry );
        free_outstanding( out );
    }
}

/* tell the server that the client side is gone */
static void client_disconnected( struct alpc_port *client )
{
    struct alpc_port *server = client->peer;
    struct alpc_message *msg;
    unsigned long long now;

    if (client->status != CONNECTED) return;
    client->status = DISCONNECTED;
    if (!server || !server->conn) return;
    server->status = DISCONNECTED;
    now = current_time;
    if ((msg = alloc_message( ALPC_TYPE_PORT_CLOSED, 0, 0, &now, sizeof(now) )))
    {
        msg->from = server;
        queue_message( server->conn, msg );
    }
}

static void alpc_port_destroy( struct object *obj )
{
    struct alpc_port *port = (struct alpc_port *)obj;
    struct alpc_message *msg, *next;
    struct alpc_outstanding *out, *out_next;

    assert( obj->ops == &alpc_port_ops );

    if (port->type == COMMUNICATION_PORT)
    {
        if (port->is_client)
        {
            client_disconnected( port );
            if (port->peer)
            {
                port->peer->peer = NULL;
                release_object( port->peer );
            }
        }
        else if (port->conn)
        {
            purge_messages_from( port->conn, port );
            if (port->peer) port->peer->peer = NULL;
            release_object( port->conn );
        }
    }
    LIST_FOR_EACH_ENTRY_SAFE( msg, next, &port->queue, struct alpc_message, entry )
    {
        list_remove( &msg->entry );
        if (msg->type == (ALPC_TYPE_CONNECTION_REQUEST | ALPC_TYPE_RECEIVED_FLAG) && msg->from && msg->from->peer)
        {
            /* a pending connection is refused */
            struct alpc_message *reply = alloc_message( ALPC_TYPE_CONNECTION_REPLY, msg->id, msg->callback_id, NULL, 0 );

            if (reply)
            {
                reply->status = STATUS_PORT_CONNECTION_REFUSED;
                queue_message( msg->from->peer, reply );
            }
        }
        free_message( msg );
    }
    LIST_FOR_EACH_ENTRY_SAFE( out, out_next, &port->outstanding, struct alpc_outstanding, entry )
    {
        list_remove( &out->entry );
        free_outstanding( out );
    }
    release_object( port->thread );
}

static struct alpc_port *get_port( obj_handle_t handle )
{
    return (struct alpc_port *)get_handle_obj( current->process, handle, 0, &alpc_port_ops );
}

/* Create an ALPC port */
DECL_HANDLER(alpc_create_port)
{
    struct alpc_port *alpc_port;
    struct alpc_port_init_data data = { .type = CONNECTION_PORT, .flags = req->flags,
                                        .max_msg_len = req->max_msg_len };
    struct object_params params = { .ops = &alpc_port_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;

    if ((alpc_port = create_named_object( &params )))
    {
        int created = get_error() != STATUS_OBJECT_NAME_EXISTS;

        /* a new port without a descriptor gets the defaults of the token of its creator, as on Windows */
        if (created && !alpc_port->obj.sd && !params.sd)
        {
            static const struct security_descriptor empty_sd;

            set_sd_defaults_from_token( &alpc_port->obj, &empty_sd,
                                        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        current->token ? current->token : current->process->token );
        }
        /* the creator gets its handle whatever the descriptor says; opening an existing port is checked */
        if (created)
            reply->handle = alloc_handle_no_access_check( current->process, alpc_port, ALPC_PORT_ALL_ACCESS, params.attr );
        else
            reply->handle = alloc_handle( current->process, alpc_port, ALPC_PORT_ALL_ACCESS, params.attr );
        release_object( alpc_port );
    }

    if (params.root) release_object( params.root );
}

/* Connect to a named connection port */
DECL_HANDLER(alpc_connect)
{
    struct object_params params = { .ops = &alpc_port_ops };
    struct alpc_port *conn, *client, *server;
    struct alpc_message *msg;
    const void *data;
    data_size_t len;
    unsigned int id;

    if (!get_req_object_attributes( &params )) return;
    data = get_req_data_after_objattr( &params, &len );
    if (params.root) release_object( params.root );
    if (len > ALPC_MAX_DATA)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!(conn = open_named_object( &params ))) return;
    if (conn->obj.ops != &alpc_port_ops || conn->type != CONNECTION_PORT)
    {
        release_object( conn );
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return;
    }
    {
        unsigned int access = ALPC_PORT_CONNECT;

        if (!check_object_access( NULL, &conn->obj, &access ))  /* sets the error */
        {
            release_object( conn );
            return;
        }
    }

    if (!(client = alloc_object( &alpc_port_ops )))
    {
        release_object( conn );
        return;
    }
    init_port( client, COMMUNICATION_PORT, req->port_flags, conn->max_msg_len );
    client->is_client = 1;
    if (!(server = alloc_object( &alpc_port_ops )))
    {
        release_object( client );
        release_object( conn );
        return;
    }
    init_port( server, COMMUNICATION_PORT, req->port_flags, conn->max_msg_len );
    server->conn = (struct alpc_port *)grab_object( conn );
    server->peer = client;
    client->peer = server;  /* the reference of the allocation */

    id = ++next_message_id;
    if (!(msg = alloc_message( ALPC_TYPE_CONNECTION_REQUEST | ALPC_TYPE_RECEIVED_FLAG, id, ++next_callback_id, data, len )))
    {
        release_object( client );  /* releases server */
        release_object( conn );
        return;
    }
    msg->from = server;
    capture_sender_token( msg, req->sec_level );

    if ((reply->handle = alloc_handle( current->process, client, ALPC_PORT_ALL_ACCESS, 0 )))
    {
        reply->msg_id = id;
        queue_message( conn, msg );
    }
    else
    {
        free_message( msg );
    }
    release_object( client );
    release_object( conn );
}

/* Accept or refuse a connection request */
DECL_HANDLER(alpc_accept)
{
    struct alpc_port *conn, *server, *client;
    struct alpc_outstanding *out;
    struct alpc_message *msg;
    unsigned int callback_id = 0;
    int found = 0;

    if (!(conn = get_port( req->handle ))) return;
    if (conn->type != CONNECTION_PORT)
    {
        release_object( conn );
        set_error( STATUS_INVALID_PORT_HANDLE );
        return;
    }
    if (get_req_data_size() > ALPC_MAX_DATA)
    {
        release_object( conn );
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    LIST_FOR_EACH_ENTRY( out, &conn->outstanding, struct alpc_outstanding, entry )
    {
        if (!out->is_connect || out->id != req->msg_id) continue;
        server = out->from;
        callback_id = out->callback_id;
        list_remove( &out->entry );
        free_outstanding( out );
        found = 1;
        break;
    }
    if (!found)
    {
        release_object( conn );
        set_error( STATUS_INVALID_CID );
        return;
    }

    client = server->peer;
    if (!client)
    {
        release_object( conn );
        set_error( STATUS_PORT_DISCONNECTED );
        return;
    }

    if (!(msg = alloc_message( ALPC_TYPE_CONNECTION_REPLY, req->msg_id, callback_id, get_req_data(), get_req_data_size() )))
    {
        release_object( conn );
        return;
    }

    if (req->accept)
    {
        if (!(reply->handle = alloc_handle( current->process, server, ALPC_PORT_ALL_ACCESS, 0 )))
        {
            free_message( msg );
            release_object( conn );
            return;
        }
        server->status  = CONNECTED;
        server->context = req->context;
        client->status  = CONNECTED;
    }
    else
    {
        client->status = REFUSED;
        msg->status = STATUS_PORT_CONNECTION_REFUSED;
    }
    queue_message( client, msg );
    release_object( conn );
}

/* Send a message and/or receive one */
DECL_HANDLER(alpc_send_receive)
{
    struct alpc_port *port;
    struct alpc_message *msg;

    if (!(port = get_port( req->handle ))) return;

    if (req->has_send)
    {
        const void *data = get_req_data();
        data_size_t len = get_req_data_size();

        if (len > ALPC_MAX_DATA)
        {
            set_error( STATUS_INVALID_PARAMETER );
            goto done;
        }

        if ((req->flags & 0x10000) && req->msg_id) /* release the received message: a reply (measured on Windows) */
        {
            struct alpc_port *conn = port->type == CONNECTION_PORT ? port : port->conn;
            struct alpc_outstanding *out;
            struct alpc_port *client = NULL;
            unsigned int callback_id = 0, sync_tid = 0;
            int found = 0;

            if (conn)
            {
                LIST_FOR_EACH_ENTRY( out, &conn->outstanding, struct alpc_outstanding, entry )
                {
                    if (out->is_connect || out->id != req->msg_id) continue;
                    client = out->from->peer;
                    callback_id = out->callback_id;
                    sync_tid = out->sync_tid;
                    list_remove( &out->entry );
                    free_outstanding( out );
                    found = 1;
                    break;
                }
            }
            if (!found)
            {
                set_error( STATUS_INVALID_CID );
                goto done;
            }
            if (client)
            {
                if (!(msg = alloc_message( ALPC_TYPE_REPLY, req->msg_id, callback_id, data, len ))) goto done;
                msg->sync_tid = sync_tid;
                if (sync_tid)
                {
                    /* the thread that sent the request is gone: nobody could ever receive the reply, so it becomes an
                     * ordinary one */
                    struct thread *thread = get_thread_from_id( sync_tid );

                    if (!thread) clear_error();
                    if (!thread || thread->state == TERMINATED) msg->sync_tid = 0;
                    if (thread) release_object( thread );
                }
                queue_message( client, msg );
            }
            reply->sent_id = req->msg_id;
        }
        else
        {
            struct alpc_port *dest, *server;
            unsigned int id;

            if (port->type == CONNECTION_PORT)
            {
                /* a message sent on a connection port is a datagram to the port itself (measured on Windows) */
                if (!(msg = alloc_message( ALPC_TYPE_DATAGRAM, 0, ++next_callback_id, data, len ))) goto done;
                queue_message( port, msg );
                goto recv;
            }
            if (port->status != CONNECTED || !port->peer)
            {
                set_error( STATUS_PORT_DISCONNECTED );
                goto done;
            }
            id = ++next_message_id;
            if (!(msg = alloc_message( ALPC_TYPE_REQUEST | ALPC_TYPE_RECEIVED_FLAG, id, ++next_callback_id, data, len ))) goto done;
            capture_sender_token( msg, req->sec_level );
            if (req->flags & ALPC_MSGFLG_SYNC_REQUEST) msg->sync_tid = current->id;
            if (port->is_client)
            {
                server = port->peer;
                msg->from = server;
                dest = server->conn;
            }
            else
            {
                dest = port->peer;
            }
            queue_message( dest, msg );
            reply->sent_id = id;
        }
    }

recv:
    if (req->has_recv)
    {
        if (!(msg = find_message_for( port, current )))
        {
            set_error( STATUS_PENDING );
            goto done;
        }
        reply->data_len = msg->len;
        if (msg->len > req->max_recv)
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            goto done;
        }
        list_remove( &msg->entry );
        reply->has_msg      = 1;
        reply->type         = msg->type;
        reply->msg_id       = msg->id;
        reply->callback_id  = msg->callback_id;
        reply->pid          = msg->pid;
        reply->tid          = msg->tid;
        reply->status       = msg->status;
        /* no context attribute for a connection request nor for a datagram a server sent to its own connection port (measured on
         * Windows 11, tests/uwp/alpc/alpc-selfdg.c: ValidAttributes 0); a reply to a client has one */
        reply->has_context  = msg->type != (ALPC_TYPE_CONNECTION_REQUEST | ALPC_TYPE_RECEIVED_FLAG) &&
                              (msg->from || port->type != CONNECTION_PORT);
        reply->port_context = msg->from ? msg->from->context : (mem_size_t)req->handle;
        reply->sequence     = ++port->sequence;
        if (msg->from && (msg->type & ALPC_TYPE_RECEIVED_FLAG))
        {
            /* a server that received a request or a connection request owes the answer */
            struct alpc_port *conn = port->type == CONNECTION_PORT ? port : port->conn;
            struct alpc_outstanding *out = mem_alloc( sizeof(*out) );

            if (out)
            {
                out->id          = msg->id;
                out->callback_id = msg->callback_id;
                out->is_connect  = (msg->type & 0xff) == ALPC_TYPE_CONNECTION_REQUEST;
                out->token       = msg->token;  /* the record owns it from now on */
                out->sec_level   = msg->sec_level;
                out->sync_tid    = msg->sync_tid;
                msg->token       = NULL;
                out->from        = msg->from;
                list_add_tail( &conn->outstanding, &out->entry );
            }
        }
        if (msg->len) set_reply_data( msg->data, msg->len );
        free_message( msg );
    }

done:
    release_object( port );
}

/* Impersonate the sender of a message. The message is the one the server received on this port, found by the id the
 * server gave it; what the header says about the sender is not used. The thread gets the token the sending thread had when
 * it sent the message (not its token of now: ids are reused and tokens change), if the message came with a security
 * attribute; without it the call succeeds and does nothing, as on Windows. The port must allow impersonation. The level is
 * the lowest of what the sender gave in its security attribute and what the server may have of this client (the rule of the
 * named pipes: identification only, unless the server has SeImpersonatePrivilege or is the same user and not less
 * trusted). Not given: the delegation level (capped to impersonation). */
DECL_HANDLER(alpc_impersonate)
{
    struct alpc_port *port, *conn;
    struct alpc_outstanding *out;
    struct token *token = NULL, *copy;
    int level = -1, found = 0, allowed;

    if (!(port = get_port( req->handle ))) return;
    conn = port->type == CONNECTION_PORT ? port : port->conn;

    if (conn)
    {
        LIST_FOR_EACH_ENTRY( out, &conn->outstanding, struct alpc_outstanding, entry )
        {
            if (out->id != req->msg_id || (port->type != CONNECTION_PORT && out->from != port)) continue;
            token = out->token;
            level = out->sec_level;
            found = 1;
            break;
        }
    }
    /* the message is looked up first, then the port is asked (the order Windows answers in) */
    if (!found)
    {
        release_object( port );
        set_error( STATUS_INVALID_MESSAGE );
        return;
    }
    if (!(conn->flags & ALPC_PORTFLG_ALLOWIMPERSONATION))
    {
        release_object( port );
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    release_object( port );

    if (!token) return;  /* no security attribute: nothing to impersonate */

    if (level > SecurityImpersonation) level = SecurityImpersonation;
    allowed = token_impersonation_level( token, current->process->token );
    if (allowed < level) level = allowed;

    if (!(copy = token_duplicate( token, 0, level, NULL, NULL, 0, NULL, 0, 0 ))) return;
    if (current->token) release_object( current->token );
    current->token = copy;
}

/* Disconnect a communication port */
DECL_HANDLER(alpc_disconnect)
{
    struct alpc_port *port;

    if (!(port = get_port( req->handle ))) return;
    if (port->type != COMMUNICATION_PORT)
    {
        set_error( STATUS_INVALID_PORT_HANDLE );
    }
    else if (port->is_client)
    {
        client_disconnected( port );
    }
    else
    {
        port->status = DISCONNECTED;
        if (port->peer) port->peer->status = DISCONNECTED;
    }
    release_object( port );
}
