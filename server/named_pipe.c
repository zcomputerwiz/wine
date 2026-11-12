/*
 * Server-side pipe management
 *
 * Copyright (C) 1998 Alexandre Julliard
 * Copyright (C) 2001 Mike McCormack
 * Copyright 2016 Jacek Caban for CodeWeavers
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

#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"

#include "file.h"
#include "handle.h"
#include "thread.h"
#include "request.h"
#include "security.h"
#include "process.h"
#include "sysrun.h"
#include "user.h"
#include "accounts.h"
#include "session0.h"
#include "unicode.h"

struct named_pipe;

#ifdef WEX_E1S6_TRANSPORT_TEST
#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>
#include <time.h>
#include "e1s_token.h"
struct pipe_binding
{
    unsigned char prefix_id[16];
    unsigned char host_process_id[16];
    unsigned char opener_id[16];
    unsigned char writer_id[16];
    unsigned char desktop_route[16];
    unsigned long long server_gen;
    unsigned long long root_gen;
    unsigned long long broker_gen;
    unsigned long long map_gen;
    unsigned long long policy_gen;
    unsigned int caller_session;
    unsigned long long host_uid;
    struct process *opener_process;
    struct process *writer_process;
    struct desktop *desktop;
    struct token *opener_token;
    struct luid opener_token_id;
    struct luid opener_modified_id;
    int endorsed;
    struct process *dialog_process;
    struct token *dialog_token;
    struct luid dialog_token_id;
    struct luid dialog_modified_id;
    int dialog_pin_pid;
    unsigned char dialog_id[16];
    unsigned char sealed_tuple[220];
    int receipt_used;   /* R10: one receipt installation attempt per binding */
};

static inline unsigned long long unpack_u64_le( const unsigned char *p )
{
    unsigned long long v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= ((unsigned long long)p[i]) << (8 * i);
    return v;
}

static inline unsigned int unpack_u32_le( const unsigned char *p )
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < 4; i++) v |= ((unsigned int)p[i]) << (8 * i);
    return v;
}

/* R8 desktop rule (3e decision 4 as corrected by Claude, 2026-11-10): like the consent prompt on
 * Windows, D runs on the session's interactive (visible) window station whatever window station the
 * caller uses: on its secure desktop for desktop_mode 1, or on the caller's own desktop for mode 2
 * (which therefore needs a caller on that window station). The session is checked separately. */
static int dialog_desktop_ok( struct process *dialog, const struct pipe_binding *b, unsigned int mode )
{
    struct desktop *d = NULL;
    struct winstation *visible = get_visible_winstation();
    int ok;

    if (dialog->desktop && !(d = get_desktop_obj( dialog, dialog->desktop, 0 ))) clear_error();
    ok = d && visible && d->winstation == visible &&
         ((mode == 1 && d->secure) || (mode == 2 && d == b->desktop));
#ifdef WEX_R8_SERVER_TEST
    {
        char dn[64] = "", cn[64] = "";
        data_size_t len = 0, i;
        const WCHAR *name;
        if (d && (name = get_object_name( &d->winstation->obj, &len )))
            for (i = 0; i < len / sizeof(WCHAR) && i < sizeof(dn) - 1; i++) dn[i] = name[i] < 128 ? name[i] : '?';
        if (b->desktop && (name = get_object_name( &b->desktop->winstation->obj, &len )))
            for (i = 0; i < len / sizeof(WCHAR) && i < sizeof(cn) - 1; i++) cn[i] = name[i] < 128 ? name[i] : '?';
        fprintf( stderr, "wex-r8: desktop ok=%d mode=%u d_secure=%d d_ws=%s visible=%d caller_ws=%s caller_visible=%d\n",
                 ok, mode, d ? d->secure : -1, dn, d && d->winstation == visible, cn,
                 b->desktop && b->desktop->winstation == visible );
    }
#endif
    if (d) release_object( d );
    return ok;
}
#endif

/* WineExplorer private: immutable identity at client message submission. */
struct pipe_writer
{
    struct process *process;
    struct token   *primary;
    struct luid     token_id;
    struct luid     modified_id;
    int             impersonated;
#ifdef WEX_E1S6_TRANSPORT_TEST
    struct desktop *desktop;
#endif
};

struct pipe_message
{
    struct list          entry;      /* entry in message queue */
    data_size_t          read_pos;   /* already read bytes */
    struct iosb         *iosb;       /* message iosb */
    struct async        *async;      /* async of pending write */
    struct pipe_writer   writer;
};

struct pipe_end
{
    struct object        obj;        /* object header */
    struct fd           *fd;         /* pipe file descriptor */
    unsigned int         flags;      /* pipe flags */
    unsigned int         state;      /* pipe state */
    struct named_pipe   *pipe;
    struct pipe_end     *connection; /* the other end of the pipe */
    process_id_t         client_pid; /* process that created the client */
    process_id_t         server_pid; /* process that created the server */
    struct token        *client_token; /* token of the thread that opened the client end (elevation enforced) */
    struct process      *client_process; /* retained opener, not a later PID lookup */
    struct token        *client_primary; /* primary token at open */
    struct luid          client_token_id;
    struct luid          client_modified_id;
    int                  client_impersonated; /* opener had an explicit thread token */
    struct pipe_writer   consumed_writer; /* retained before freeing the consumed message */
    unsigned int         binding_reads; /* E1 supports one request read per connection */
    int                  consumed_valid;
    data_size_t          buffer_size;/* size of buffered data that doesn't block caller */
    struct list          message_queue;
    struct async_queue   read_q;     /* read queue */
    struct async_queue   write_q;    /* write queue */
#ifdef WEX_E1S6_TRANSPORT_TEST
    struct pipe_binding *binding;
#endif
};

struct pipe_server
{
    struct pipe_end      pipe_end;   /* common header for both pipe ends */
    struct list          entry;      /* entry in named pipe listeners list */
    unsigned int         options;    /* pipe options */
    struct async_queue   listen_q;   /* listen queue */
};

struct named_pipe
{
    struct object       obj;         /* object header */
    int                 message_mode;
    unsigned int        sharing;
    unsigned int        maxinstances;
    unsigned int        outsize;
    unsigned int        insize;
    unsigned int        instances;
    timeout_t           timeout;
    struct list         listeners;   /* list of servers listening on this pipe */
    struct async_queue  waiters;     /* list of clients waiting to connect */
    WCHAR              *s0_name;     /* WineExplorer T5.2: a pipe of session 0 (opening it bridges there) */
    data_size_t         s0_len;
};

struct named_pipe_device
{
    struct object       obj;         /* object header */
    struct namespace   *pipes;       /* named pipe namespace */
};

struct named_pipe_device_file
{
    struct object             obj;         /* object header */
    struct fd                *fd;          /* pseudo-fd for ioctls */
    struct named_pipe_device *device;      /* named pipe device */
};

struct named_pipe_init_data
{
    unsigned int   maxinstances;
    unsigned int   outsize;
    unsigned int   insize;
    unsigned int   sharing;
    timeout_t      timeout;
    unsigned int   flags;
};

static void named_pipe_dump( struct object *obj, int verbose );
static bool named_pipe_init( struct object *obj, const void *init_data );
static WCHAR *named_pipe_get_full_name( struct object *obj, data_size_t max, data_size_t *ret_len );
static int named_pipe_link_name( struct object *obj, struct object_name *name, struct object *parent );
static struct object *named_pipe_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options );
static void named_pipe_destroy( struct object *obj );

static struct type_descr named_pipe_type =
{
    { NULL, 0 },                /* name */
    STANDARD_RIGHTS_ALL,        /* valid_access */
    {                           /* mapping */
        STANDARD_RIGHTS_READ,
        STANDARD_RIGHTS_WRITE | FILE_CREATE_PIPE_INSTANCE,
        STANDARD_RIGHTS_EXECUTE,
        STANDARD_RIGHTS_ALL
    }
};

static const struct object_ops named_pipe_ops =
{
    .size          = sizeof(struct named_pipe),
    .type          = &named_pipe_type,
    .dump          = named_pipe_dump,
    .init          = named_pipe_init,
    .get_full_name = named_pipe_get_full_name,
    .link_name     = named_pipe_link_name,
    .open_file     = named_pipe_open_file,
    .destroy       = named_pipe_destroy,
};

/* common server and client pipe end functions */
static void pipe_end_destroy( struct object *obj );
static enum server_fd_type pipe_end_get_fd_type( struct fd *fd );
static struct fd *pipe_end_get_fd( struct object *obj );
static struct security_descriptor *pipe_end_get_sd( struct object *obj );
static int pipe_end_set_sd( struct object *obj, const struct security_descriptor *sd,
                            unsigned int set_info );
static WCHAR *pipe_end_get_full_name( struct object *obj, data_size_t max, data_size_t *len );
static void pipe_end_read( struct fd *fd, struct async *async, file_pos_t pos );
static void pipe_end_write( struct fd *fd, struct async *async_data, file_pos_t pos );
static void pipe_end_flush( struct fd *fd, struct async *async );
static void pipe_end_get_volume_info( struct fd *fd, struct async *async, unsigned int info_class );
static void pipe_end_reselect_async( struct fd *fd, struct async_queue *queue );
static void pipe_end_get_file_info( struct fd *fd, obj_handle_t handle, unsigned int info_class );

/* server end functions */
static void pipe_server_dump( struct object *obj, int verbose );
static struct object *pipe_server_lookup_name( struct object *obj, struct unicode_str *name,
                                               unsigned int attr, struct object *root );
static struct object *pipe_server_open_file( struct object *obj, unsigned int access,
                                             unsigned int sharing, unsigned int options );
static void pipe_server_destroy( struct object *obj);
static void pipe_server_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );

static const struct object_ops pipe_server_ops =
{
    .size          = sizeof(struct pipe_server),
    .type          = &file_type,
    .dump          = pipe_server_dump,
    .get_fd        = pipe_end_get_fd,
    .get_sync      = default_fd_get_sync,
    .get_sd        = pipe_end_get_sd,
    .set_sd        = pipe_end_set_sd,
    .get_full_name = pipe_end_get_full_name,
    .lookup_name   = pipe_server_lookup_name,
    .open_file     = pipe_server_open_file,
    .close_handle  = async_close_obj_handle,
    .destroy       = pipe_server_destroy,
};

static const struct fd_ops pipe_server_fd_ops =
{
    .get_fd_type     = pipe_end_get_fd_type,
    .read            = pipe_end_read,
    .write           = pipe_end_write,
    .flush           = pipe_end_flush,
    .get_file_info   = pipe_end_get_file_info,
    .get_volume_info = pipe_end_get_volume_info,
    .ioctl           = pipe_server_ioctl,
    .reselect_async  = pipe_end_reselect_async,
};

/* client end functions */
static void pipe_client_dump( struct object *obj, int verbose );
static void pipe_client_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );

static const struct object_ops pipe_client_ops =
{
    .size          = sizeof(struct pipe_end),
    .type          = &file_type,
    .dump          = pipe_client_dump,
    .get_fd        = pipe_end_get_fd,
    .get_sync      = default_fd_get_sync,
    .get_sd        = pipe_end_get_sd,
    .set_sd        = pipe_end_set_sd,
    .get_full_name = pipe_end_get_full_name,
    .close_handle  = async_close_obj_handle,
    .destroy       = pipe_end_destroy,
};

static const struct fd_ops pipe_client_fd_ops =
{
    .get_fd_type     = pipe_end_get_fd_type,
    .read            = pipe_end_read,
    .write           = pipe_end_write,
    .flush           = pipe_end_flush,
    .get_file_info   = pipe_end_get_file_info,
    .get_volume_info = pipe_end_get_volume_info,
    .ioctl           = pipe_client_ioctl,
    .reselect_async  = pipe_end_reselect_async,
};

/* WineExplorer system tier T5.2: a pipe end bridged to another server (below) */
static const struct object_ops pipe_bridge_ops;
static void pipe_bridge_flush( struct pipe_end *end );
static void pipe_bridge_update_events( struct pipe_end *end );
static void pipe_bridge_closed( struct pipe_end *end );
static struct object *pipe_bridge_open_remote( struct named_pipe *pipe, unsigned int options );
static struct object *pipe_bridge_lookup( struct unicode_str *name );

static void named_pipe_device_dump( struct object *obj, int verbose );
static bool named_pipe_device_init( struct object *obj, const void *init_data );
static WCHAR *named_pipe_device_get_full_name( struct object *obj, data_size_t max, data_size_t *len );
static struct object *named_pipe_device_lookup_name( struct object *obj,
    struct unicode_str *name, unsigned int attr, struct object *root );
static struct object *named_pipe_device_open_file( struct object *obj, unsigned int access,
                                                   unsigned int sharing, unsigned int options );
static void named_pipe_device_destroy( struct object *obj );

static const struct object_ops named_pipe_device_ops =
{
    .size          = sizeof(struct named_pipe_device),
    .type          = &device_type,
    .dump          = named_pipe_device_dump,
    .init          = named_pipe_device_init,
    .get_full_name = named_pipe_device_get_full_name,
    .lookup_name   = named_pipe_device_lookup_name,
    .open_file     = named_pipe_device_open_file,
    .destroy       = named_pipe_device_destroy,
};

static void named_pipe_device_file_dump( struct object *obj, int verbose );
static struct fd *named_pipe_device_file_get_fd( struct object *obj );
static WCHAR *named_pipe_device_file_get_full_name( struct object *obj, data_size_t max, data_size_t *len );
static void named_pipe_device_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );
static enum server_fd_type named_pipe_device_file_get_fd_type( struct fd *fd );
static void named_pipe_device_file_destroy( struct object *obj );

static const struct object_ops named_pipe_device_file_ops =
{
    .size          = sizeof(struct named_pipe_device_file),
    .type          = &file_type,
    .dump          = named_pipe_device_file_dump,
    .get_fd        = named_pipe_device_file_get_fd,
    .get_sync      = default_fd_get_sync,
    .get_full_name = named_pipe_device_file_get_full_name,
    .destroy       = named_pipe_device_file_destroy,
};

static const struct fd_ops named_pipe_device_fd_ops =
{
    .get_fd_type   = named_pipe_device_file_get_fd_type,
    .get_file_info = default_fd_get_file_info,
    .ioctl         = named_pipe_device_ioctl,
    .queue_async   = default_fd_queue_async,
};

static void named_pipe_dir_dump( struct object *obj, int verbose );
static struct fd *named_pipe_dir_get_fd( struct object *obj );
static WCHAR *named_pipe_dir_get_full_name( struct object *obj, data_size_t max, data_size_t *ret_len );
static void named_pipe_dir_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );
static struct object *named_pipe_dir_lookup_name( struct object *obj, struct unicode_str *name,
                                                  unsigned int attr, struct object *root );
static struct object *named_pipe_dir_open_file( struct object *obj, unsigned int access,
                                                unsigned int sharing, unsigned int options );
static void named_pipe_dir_destroy( struct object *obj );

static const struct object_ops named_pipe_dir_ops =
{
    .size          = sizeof(struct named_pipe_device_file),
    .type          = &file_type,
    .dump          = named_pipe_dir_dump,
    .get_fd        = named_pipe_dir_get_fd,
    .get_sync      = default_fd_get_sync,
    .get_full_name = named_pipe_dir_get_full_name,
    .lookup_name   = named_pipe_dir_lookup_name,
    .open_file     = named_pipe_dir_open_file,
    .destroy       = named_pipe_dir_destroy,
};

static const struct fd_ops named_pipe_dir_fd_ops =
{
    .get_file_info = default_fd_get_file_info,
    .ioctl         = named_pipe_dir_ioctl,
    .queue_async   = default_fd_queue_async,
};

static void named_pipe_dump( struct object *obj, int verbose )
{
    fputs( "Named pipe\n", stderr );
}

static bool named_pipe_init( struct object *obj, const void *init_data )
{
    struct named_pipe *pipe = (struct named_pipe *)obj;
    const struct named_pipe_init_data *data = init_data;

    pipe->instances    = 0;
    pipe->insize       = data->insize;
    pipe->outsize      = data->outsize;
    pipe->maxinstances = data->maxinstances;
    pipe->timeout      = data->timeout;
    pipe->message_mode = (data->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE) != 0;
    pipe->sharing      = data->sharing;
    pipe->s0_name      = NULL;
    pipe->s0_len       = 0;
    init_async_queue( &pipe->waiters );
    list_init( &pipe->listeners );
    return true;
}

static WCHAR *named_pipe_get_full_name( struct object *obj, data_size_t max, data_size_t *ret_len )
{
    WCHAR *ret;

    if (!(ret = default_get_full_name( obj, max, ret_len )))
        set_error( STATUS_OBJECT_PATH_INVALID );
    else if (*ret_len > max)
        set_error( STATUS_BUFFER_OVERFLOW );
    return ret;
}

static void pipe_server_dump( struct object *obj, int verbose )
{
    struct pipe_server *server = (struct pipe_server *) obj;
    assert( obj->ops == &pipe_server_ops );
    fprintf( stderr, "Named pipe server pipe=%p state=%d\n", server->pipe_end.pipe,
             server->pipe_end.state );
}

static void pipe_client_dump( struct object *obj, int verbose )
{
    struct pipe_end *client = (struct pipe_end *) obj;
    assert( obj->ops == &pipe_client_ops );
    fprintf( stderr, "Named pipe client server=%p\n", client->connection );
}

static void named_pipe_destroy( struct object *obj)
{
    struct named_pipe *pipe = (struct named_pipe *) obj;

    assert( list_empty( &pipe->listeners ) );
    assert( !pipe->instances );
    free_async_queue( &pipe->waiters );
    free( pipe->s0_name );
}

static struct fd *pipe_end_get_fd( struct object *obj )
{
    struct pipe_end *pipe_end = (struct pipe_end *) obj;
    return (struct fd *) grab_object( pipe_end->fd );
}

#ifdef WEX_E1S6_TRANSPORT_TEST
static void clear_pipe_binding( struct pipe_end *pipe_end )
{
    if (pipe_end->binding)
    {
        if (pipe_end->binding->desktop) release_object( pipe_end->binding->desktop );
        if (pipe_end->binding->opener_process) release_object( pipe_end->binding->opener_process );
        if (pipe_end->binding->writer_process) release_object( pipe_end->binding->writer_process );
        if (pipe_end->binding->opener_token) release_object( pipe_end->binding->opener_token );
        if (pipe_end->binding->dialog_process) release_object( pipe_end->binding->dialog_process );
        if (pipe_end->binding->dialog_token) release_object( pipe_end->binding->dialog_token );
        free( pipe_end->binding );
        pipe_end->binding = NULL;
    }
}
#endif

static void clear_pipe_writer( struct pipe_writer *writer )
{
#ifdef WEX_E1S6_TRANSPORT_TEST
    if (writer->desktop) release_object( writer->desktop );
#endif
    if (writer->primary) release_object( writer->primary );
    if (writer->process) release_object( writer->process );
    memset( writer, 0, sizeof(*writer) );
}

static struct pipe_message *queue_message( struct pipe_end *pipe_end, struct iosb *iosb )
{
    struct pipe_message *message;

    if (!(message = mem_alloc( sizeof(*message) ))) return NULL;
    message->iosb = (struct iosb *)grab_object( iosb );
    message->async = NULL;
    message->read_pos = 0;
    memset( &message->writer, 0, sizeof(message->writer) );
    /* Both write and transceive submit here, while current is still the writer.
     * Only client-to-server messages on connections with opener retention need this. */
    if (pipe_end->connection && pipe_end->connection->client_process)
    {
        message->writer.process = (struct process *)grab_object( current->process );
        message->writer.impersonated = (current->token != NULL);
        if (current->process->token)
        {
            message->writer.primary = (struct token *)grab_object( current->process->token );
            token_get_ids( message->writer.primary, &message->writer.token_id, &message->writer.modified_id );
        }
#ifdef WEX_E1S6_TRANSPORT_TEST
        message->writer.desktop = get_thread_desktop_quiet( current );
#endif
    }
    list_add_tail( &pipe_end->message_queue, &message->entry );
    return message;
}

static void wake_message( struct pipe_message *message, data_size_t result )
{
    struct async *async = message->async;

    message->async = NULL;
    if (!async) return;

    async_request_complete( async, STATUS_SUCCESS, result, 0, NULL );
    release_object( async );
}

static void free_message( struct pipe_message *message )
{
    list_remove( &message->entry );
    if (message->iosb) release_object( message->iosb );
    clear_pipe_writer( &message->writer );
    free( message );
}

static void pipe_end_disconnect( struct pipe_end *pipe_end, unsigned int status )
{
    struct pipe_end *connection = pipe_end->connection;
    struct pipe_message *message, *next;
    struct async *async;

    pipe_end->connection = NULL;
#ifdef WEX_E1S6_TRANSPORT_TEST
    clear_pipe_binding( pipe_end );
#endif
    clear_pipe_writer( &pipe_end->consumed_writer );
    pipe_end->consumed_valid = 0;
    pipe_end->binding_reads = 0;
    if (pipe_end->client_primary) release_object( pipe_end->client_primary );
    if (pipe_end->client_process) release_object( pipe_end->client_process );
    pipe_end->client_primary = NULL;
    pipe_end->client_process = NULL;

    pipe_end->state = status == STATUS_PIPE_DISCONNECTED
        ? FILE_PIPE_DISCONNECTED_STATE : FILE_PIPE_CLOSING_STATE;
    if (pipe_end->fd) fd_async_wake_up( pipe_end->fd, ASYNC_TYPE_WAIT, status );
    async_wake_up( &pipe_end->read_q, status );
    LIST_FOR_EACH_ENTRY_SAFE( message, next, &pipe_end->message_queue, struct pipe_message, entry )
    {
        async = message->async;
        clear_pipe_writer( &message->writer );
        if (async || status == STATUS_PIPE_DISCONNECTED) free_message( message );
        if (!async) continue;
        async_terminate( async, status );
        release_object( async );
    }
    if (pipe_end->fd && status == STATUS_PIPE_DISCONNECTED) set_fd_signaled( pipe_end->fd, 0 );

    if (connection)
    {
        connection->connection = NULL;
        pipe_end_disconnect( connection, status );
    }
    /* last: a bridge drops its own reference here, which can free it */
    if (pipe_end->obj.ops == &pipe_bridge_ops) pipe_bridge_closed( pipe_end );
}

static void pipe_end_destroy( struct object *obj )
{
    struct pipe_end *pipe_end = (struct pipe_end *)obj;
    struct pipe_message *message;

    pipe_end_disconnect( pipe_end, STATUS_PIPE_BROKEN );
#ifdef WEX_E1S6_TRANSPORT_TEST
    clear_pipe_binding( pipe_end );
#endif

    while (!list_empty( &pipe_end->message_queue ))
    {
        message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
        assert( !message->async );
        free_message( message );
    }

    free_async_queue( &pipe_end->read_q );
    free_async_queue( &pipe_end->write_q );
    if (pipe_end->fd) release_object( pipe_end->fd );
    if (pipe_end->pipe) release_object( pipe_end->pipe );
    if (pipe_end->client_token) release_object( pipe_end->client_token );
}

static struct object *pipe_server_lookup_name( struct object *obj, struct unicode_str *name,
                                               unsigned int attr, struct object *root )
{
    if (name && name->len)
        set_error( STATUS_OBJECT_NAME_INVALID );

    return NULL;
}

static struct object *pipe_server_open_file( struct object *obj, unsigned int access,
                                             unsigned int sharing, unsigned int options )
{
    struct pipe_server *server = (struct pipe_server *)obj;

    return named_pipe_open_file(  &server->pipe_end.pipe->obj, access, sharing, options );
}

static void pipe_server_destroy( struct object *obj )
{
    struct pipe_server *server = (struct pipe_server *)obj;
    struct named_pipe *pipe = server->pipe_end.pipe;

    assert( obj->ops == &pipe_server_ops );

    assert( pipe->instances );
    if (!--pipe->instances) unlink_named_object( &pipe->obj );
    if (server->pipe_end.state == FILE_PIPE_LISTENING_STATE)
        list_remove( &server->entry );

    free_async_queue( &server->listen_q );
    pipe_end_destroy( obj );
}

static void named_pipe_device_dump( struct object *obj, int verbose )
{
    fputs( "Named pipe device\n", stderr );
}

static bool named_pipe_device_init( struct object *obj, const void *init_data )
{
    struct named_pipe_device *device = (struct named_pipe_device *)obj;

    return !!(device->pipes = create_namespace( 7 ));
}

static WCHAR *named_pipe_device_get_full_name( struct object *obj, data_size_t max, data_size_t *len )
{
    WCHAR *ret = default_get_full_name( obj, max, len );
    if (*len > max) set_error( STATUS_BUFFER_OVERFLOW );
    return ret;
}

static struct object *named_pipe_device_lookup_name( struct object *obj, struct unicode_str *name,
                                                     unsigned int attr, struct object *root )
{
    struct named_pipe_device *device = (struct named_pipe_device*)obj;
    struct object *found;

    assert( obj->ops == &named_pipe_device_ops );
    assert( device->pipes );

    if (!name) return NULL;  /* open the device itself */
    if (!name->len && name->str)
    {
        /* open the root directory */
        struct named_pipe_device_file *dir = alloc_object( &named_pipe_dir_ops );

        if (!dir) return NULL;

        dir->fd = NULL;  /* defer alloc_pseudo_fd() until after we have options */
        dir->device = (struct named_pipe_device *)grab_object( obj );

        return &dir->obj;
    }

    if ((found = find_object( device->pipes, *name, attr | OBJ_CASE_INSENSITIVE )))
        name->len = 0;
    else if (session0_client && (found = pipe_bridge_lookup( name )))
        name->len = 0;

    return found;
}

static struct object *named_pipe_device_open_file( struct object *obj, unsigned int access,
                                                   unsigned int sharing, unsigned int options )
{
    struct named_pipe_device_file *file;

    if (!(file = alloc_object( &named_pipe_device_file_ops ))) return NULL;
    file->device = (struct named_pipe_device *)grab_object( obj );
    if (!(file->fd = alloc_pseudo_fd( &named_pipe_device_fd_ops, obj, options )))
    {
        release_object( file );
        return NULL;
    }
    allow_fd_caching( file->fd );
    return &file->obj;
}

static void named_pipe_device_destroy( struct object *obj )
{
    struct named_pipe_device *device = (struct named_pipe_device*)obj;
    assert( obj->ops == &named_pipe_device_ops );
    free( device->pipes );
}

struct object *create_named_pipe_device( struct object *root, struct unicode_str name,
                                         unsigned int attr, const struct security_descriptor *sd )
{
    struct object_params params = { .ops = &named_pipe_device_ops, .root = root,
                                    .name = name, .attr = attr, .sd = sd };

    return create_named_object( &params );
}

static void named_pipe_device_file_dump( struct object *obj, int verbose )
{
    struct named_pipe_device_file *file = (struct named_pipe_device_file *)obj;

    fprintf( stderr, "File on named pipe device %p\n", file->device );
}

static struct fd *named_pipe_device_file_get_fd( struct object *obj )
{
    struct named_pipe_device_file *file = (struct named_pipe_device_file *)obj;
    return (struct fd *)grab_object( file->fd );
}

static WCHAR *named_pipe_device_file_get_full_name( struct object *obj, data_size_t max, data_size_t *len )
{
    struct named_pipe_device_file *file = (struct named_pipe_device_file *)obj;
    return named_pipe_device_get_full_name( &file->device->obj, max, len );
}

static enum server_fd_type named_pipe_device_file_get_fd_type( struct fd *fd )
{
    return FD_TYPE_DEVICE;
}

static void named_pipe_device_file_destroy( struct object *obj )
{
    struct named_pipe_device_file *file = (struct named_pipe_device_file*)obj;
    assert( obj->ops == &named_pipe_device_file_ops );
    if (file->fd) release_object( file->fd );
    release_object( file->device );
}

static void named_pipe_dir_dump( struct object *obj, int verbose )
{
    struct named_pipe_device_file *dir = (struct named_pipe_device_file *)obj;

    fprintf( stderr, "Root directory of named pipe device %p\n", dir->device );
}

static struct fd *named_pipe_dir_get_fd( struct object *obj )
{
    struct named_pipe_device_file *dir = (struct named_pipe_device_file *)obj;
    return (struct fd *)grab_object( dir->fd );
}

static WCHAR *named_pipe_dir_get_full_name( struct object *obj, data_size_t max, data_size_t *ret_len )
{
    struct named_pipe_device_file *dir = (struct named_pipe_device_file *)obj;
    data_size_t len;
    WCHAR *device_name, *ret;

    device_name = named_pipe_device_get_full_name( &dir->device->obj, max, &len );
    if (!device_name) return NULL;

    len += sizeof(WCHAR);
    ret = realloc(device_name, len);
    if (!ret)
    {
        free(device_name);
        return NULL;
    }
    ret[len / sizeof(WCHAR) - 1] = '\\';

    if (len > max) set_error( STATUS_BUFFER_OVERFLOW );
    *ret_len = len;
    return ret;
}

static struct object *named_pipe_dir_lookup_name( struct object *obj, struct unicode_str *name,
                                                  unsigned int attr, struct object *root )
{
    struct named_pipe_device_file *dir = (struct named_pipe_device_file *)obj;
    if (!name || !name->len) return NULL;  /* open the directory itself */
    return named_pipe_device_lookup_name( &dir->device->obj, name, attr, root );
}

static struct object *named_pipe_dir_open_file( struct object *obj, unsigned int access,
                                                unsigned int sharing, unsigned int options )
{
    struct named_pipe_device_file *dir = (struct named_pipe_device_file *)obj;

    if (dir->fd)
    {
        /* Trying to open by (already opened) file object */
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return NULL;
    }

    /* Turn this "proto-object" into an actual file object */

    if (!(dir->fd = alloc_pseudo_fd( &named_pipe_dir_fd_ops, obj, options )))
        return NULL;

    allow_fd_caching( dir->fd );
    return grab_object( obj );
}

static void named_pipe_dir_destroy( struct object *obj )
{
    struct named_pipe_device_file *file = (struct named_pipe_device_file *)obj;
    assert( obj->ops == &named_pipe_dir_ops );
    if (file->fd) release_object( file->fd );
    release_object( file->device );
}

static void pipe_end_flush( struct fd *fd, struct async *async )
{
    struct pipe_end *pipe_end = get_fd_user( fd );

    if (!pipe_end->pipe)
    {
        set_error( STATUS_PIPE_DISCONNECTED );
        return;
    }

    if (pipe_end->connection && !list_empty( &pipe_end->connection->message_queue ))
    {
        fd_queue_async( pipe_end->fd, async, ASYNC_TYPE_WAIT );
        set_error( STATUS_PENDING );
    }
}

static data_size_t pipe_end_get_avail( struct pipe_end *pipe_end )
{
    struct pipe_message *message;
    data_size_t avail = 0;

    LIST_FOR_EACH_ENTRY( message, &pipe_end->message_queue, struct pipe_message, entry )
        avail += message->iosb->in_size - message->read_pos;

    return avail;
}

static void pipe_end_get_file_info( struct fd *fd, obj_handle_t handle, unsigned int info_class )
{
    struct pipe_end *pipe_end = get_fd_user( fd );
    struct named_pipe *pipe = pipe_end->pipe;

    switch (info_class)
    {
    case FileNameInformation:
        {
            FILE_NAME_INFORMATION *name_info;
            data_size_t name_len, reply_size;
            const WCHAR *name;

            if (get_reply_max_size() < sizeof(*name_info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }

            /* FIXME: We should be able to return on unlinked pipe */
            if (!pipe || !(name = get_object_name( &pipe->obj, &name_len )))
            {
                set_error( STATUS_PIPE_DISCONNECTED );
                return;
            }

            reply_size = offsetof( FILE_NAME_INFORMATION, FileName[name_len/sizeof(WCHAR) + 1] );
            if (reply_size > get_reply_max_size())
            {
                reply_size = get_reply_max_size();
                set_error( STATUS_BUFFER_OVERFLOW );
            }

            if (!(name_info = set_reply_data_size( reply_size ))) return;
            name_info->FileNameLength = name_len + sizeof(WCHAR);
            name_info->FileName[0] = '\\';
            reply_size -= offsetof( FILE_NAME_INFORMATION, FileName[1] );
            if (reply_size) memcpy( &name_info->FileName[1], name, reply_size );
            break;
        }
    case FilePipeInformation:
    {
            FILE_PIPE_INFORMATION *pipe_info;

            if (!(get_handle_access( current->process, handle) & FILE_READ_ATTRIBUTES))
            {
                set_error( STATUS_ACCESS_DENIED );
                return;
            }

            if (get_reply_max_size() < sizeof(*pipe_info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }

            if (!pipe)
            {
                set_error( STATUS_PIPE_DISCONNECTED );
                return;
            }

            if (!(pipe_info = set_reply_data_size( sizeof(*pipe_info) ))) return;
            pipe_info->ReadMode       = (pipe_end->flags & NAMED_PIPE_MESSAGE_STREAM_READ)
                ? FILE_PIPE_MESSAGE_MODE : FILE_PIPE_BYTE_STREAM_MODE;
            pipe_info->CompletionMode = (pipe_end->flags & NAMED_PIPE_NONBLOCKING_MODE)
                ? FILE_PIPE_COMPLETE_OPERATION : FILE_PIPE_QUEUE_OPERATION;
            break;
        }
    case FilePipeLocalInformation:
        {
            FILE_PIPE_LOCAL_INFORMATION *pipe_info;

            if (!(get_handle_access( current->process, handle) & FILE_READ_ATTRIBUTES))
            {
                set_error( STATUS_ACCESS_DENIED );
                return;
            }

            if (get_reply_max_size() < sizeof(*pipe_info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }

            if (!pipe)
            {
                set_error( STATUS_PIPE_DISCONNECTED );
                return;
            }

            if (!(pipe_info = set_reply_data_size( sizeof(*pipe_info) ))) return;
            pipe_info->NamedPipeType = pipe->message_mode;
            switch (pipe->sharing)
            {
            case FILE_SHARE_READ:
                pipe_info->NamedPipeConfiguration = FILE_PIPE_OUTBOUND;
                break;
            case FILE_SHARE_WRITE:
                pipe_info->NamedPipeConfiguration = FILE_PIPE_INBOUND;
                break;
            case FILE_SHARE_READ | FILE_SHARE_WRITE:
                pipe_info->NamedPipeConfiguration = FILE_PIPE_FULL_DUPLEX;
                break;
            }
            pipe_info->MaximumInstances    = pipe->maxinstances;
            pipe_info->CurrentInstances    = pipe->instances;
            pipe_info->InboundQuota        = pipe->insize;

            pipe_info->ReadDataAvailable   = pipe_end_get_avail( pipe_end );

            pipe_info->OutboundQuota       = pipe->outsize;
            pipe_info->NamedPipeState      = pipe_end->state;
            pipe_info->NamedPipeEnd        = pipe_end->obj.ops == &pipe_server_ops
                ? FILE_PIPE_SERVER_END : FILE_PIPE_CLIENT_END;

            pipe_info->WriteQuotaAvailable = pipe_info->NamedPipeEnd == FILE_PIPE_CLIENT_END
                ? pipe_info->InboundQuota : pipe_info->OutboundQuota;
                /* FIXME: Needs to be reduced by ReadDataAvailable at the other end of the pipe. */
            break;
        }
    case FileStandardInformation:
        {
            FILE_STANDARD_INFORMATION *std_info;

            if (!(get_handle_access( current->process, handle) & FILE_READ_ATTRIBUTES))
            {
                set_error( STATUS_ACCESS_DENIED );
                return;
            }

            if (get_reply_max_size() < sizeof(*std_info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }

            if (!pipe)
            {
                set_error( STATUS_PIPE_DISCONNECTED );
                return;
            }

            if (!(std_info = set_reply_data_size( sizeof(*std_info) ))) return;
            std_info->AllocationSize.QuadPart = pipe->outsize + pipe->insize;
            std_info->EndOfFile.QuadPart      = pipe_end_get_avail( pipe_end );
            std_info->NumberOfLinks           = 1; /* FIXME */
            std_info->DeletePending           = 0; /* FIXME */
            std_info->Directory               = 0;
            break;
        }
    default:
        default_fd_get_file_info( fd, handle, info_class );
    }
}

static struct security_descriptor *pipe_end_get_sd( struct object *obj )
{
    struct pipe_end *pipe_end = (struct pipe_end *) obj;
    if (pipe_end->pipe) return pipe_end->pipe->obj.sd;
    set_error( STATUS_PIPE_DISCONNECTED );
    return NULL;
}

static int pipe_end_set_sd( struct object *obj, const struct security_descriptor *sd,
                            unsigned int set_info )
{
    struct pipe_end *pipe_end = (struct pipe_end *) obj;
    if (pipe_end->pipe) return default_set_sd( &pipe_end->pipe->obj, sd, set_info );
    set_error( STATUS_PIPE_DISCONNECTED );
    return 0;
}

static WCHAR *pipe_end_get_full_name( struct object *obj, data_size_t max, data_size_t *len )
{
    struct pipe_end *pipe_end = (struct pipe_end *) obj;
    return named_pipe_get_full_name( &pipe_end->pipe->obj, max, len );
}

static void pipe_end_get_volume_info( struct fd *fd, struct async *async, unsigned int info_class )
{
    switch (info_class)
    {
    case FileFsDeviceInformation:
        {
            static const FILE_FS_DEVICE_INFORMATION device_info =
            {
                FILE_DEVICE_NAMED_PIPE,
                FILE_DEVICE_ALLOW_APPCONTAINER_TRAVERSAL
            };
            if (get_reply_max_size() >= sizeof(device_info))
                set_reply_data( &device_info, sizeof(device_info) );
            else
                set_error( STATUS_BUFFER_TOO_SMALL );
            break;
        }
    default:
        set_error( STATUS_NOT_IMPLEMENTED );
    }
}

/* A subsequent read supersedes this record even if it is cancelled or fails.
 * Partial/mixed reads cannot be repaired by reading the remaining bytes later. */
static void begin_binding_read( struct pipe_end *pipe_end )
{
    if (!pipe_end->connection || !pipe_end->connection->client_process) return;
    clear_pipe_writer( &pipe_end->consumed_writer );
    pipe_end->consumed_valid = 0;
    if (pipe_end->binding_reads < 2) pipe_end->binding_reads++;
}

static void retain_consumed_writer( struct pipe_end *pipe_end, struct pipe_message *message,
                                    unsigned int status, data_size_t size )
{
    if (pipe_end->binding_reads != 1 || status != STATUS_SUCCESS ||
        !(pipe_end->flags & NAMED_PIPE_MESSAGE_STREAM_READ) || message->read_pos ||
        size != message->iosb->in_size || !message->writer.process || !message->writer.primary) return;

    pipe_end->consumed_writer = message->writer;
    memset( &message->writer, 0, sizeof(message->writer) );
    pipe_end->consumed_valid = 1;
}

static void message_queue_read( struct pipe_end *pipe_end, struct async *async )
{
    struct iosb *iosb = async_get_iosb( async );
    unsigned int status = STATUS_SUCCESS;
    struct pipe_message *message;
    data_size_t out_size;

    if (pipe_end->flags & NAMED_PIPE_MESSAGE_STREAM_READ)
    {
        message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
        out_size = min( iosb->out_size, message->iosb->in_size - message->read_pos );

        if (message->read_pos + out_size < message->iosb->in_size)
            status = STATUS_BUFFER_OVERFLOW;
    }
    else
    {
        data_size_t avail = 0;
        LIST_FOR_EACH_ENTRY( message, &pipe_end->message_queue, struct pipe_message, entry )
        {
            avail += message->iosb->in_size - message->read_pos;
            if (avail >= iosb->out_size) break;
        }
        out_size = min( iosb->out_size, avail );
    }

    message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
    if (!message->read_pos && message->iosb->in_size == iosb->out_size) /* fast path */
    {
        retain_consumed_writer( pipe_end, message, status, out_size );
        async_request_complete( async, status, out_size, out_size, message->iosb->in_data );
        message->iosb->in_data = NULL;
        wake_message( message, message->iosb->in_size );
        free_message( message );
    }
    else
    {
        data_size_t write_pos = 0, writing;
        char *buf = NULL;

        if (out_size && !(buf = malloc( out_size )))
        {
            async_terminate( async, STATUS_NO_MEMORY );
            release_object( iosb );
            return;
        }

        do
        {
            message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
            if (!write_pos) retain_consumed_writer( pipe_end, message, status, out_size );
            writing = min( out_size - write_pos, message->iosb->in_size - message->read_pos );
            if (writing) memcpy( buf + write_pos, (const char *)message->iosb->in_data + message->read_pos, writing );
            write_pos += writing;
            message->read_pos += writing;
            if (message->read_pos == message->iosb->in_size)
            {
                wake_message(message, message->iosb->in_size);
                free_message(message);
            }
        } while (write_pos < out_size);

        async_request_complete( async, status, out_size, out_size, buf );
    }

    release_object( iosb );
}

/* We call async_terminate in our reselect implementation, which causes recursive reselect.
 * We're not interested in such reselect calls, so we ignore them. */
static int ignore_reselect;

static void reselect_write_queue( struct pipe_end *pipe_end );

static void reselect_read_queue( struct pipe_end *pipe_end, int reselect_write )
{
    struct async *async;

    /* a bridge's reader is the other server: what is queued for it is sent there */
    if (pipe_end->obj.ops == &pipe_bridge_ops)
    {
        pipe_bridge_flush( pipe_end );
        return;
    }
    ignore_reselect = 1;
    while (!list_empty( &pipe_end->message_queue ) && (async = find_pending_async( &pipe_end->read_q )))
    {
        message_queue_read( pipe_end, async );
        release_object( async );
        reselect_write = 1;
    }
    ignore_reselect = 0;

    if (pipe_end->connection)
    {
        if (list_empty( &pipe_end->message_queue ))
            fd_async_wake_up( pipe_end->connection->fd, ASYNC_TYPE_WAIT, STATUS_SUCCESS );
        else if (reselect_write)
            reselect_write_queue( pipe_end->connection );
        /* a bridge reads more from the other server once this end has room again */
        if (pipe_end->connection && pipe_end->connection->obj.ops == &pipe_bridge_ops)
            pipe_bridge_update_events( pipe_end->connection );
    }
}

static void reselect_write_queue( struct pipe_end *pipe_end )
{
    struct pipe_message *message, *next;
    struct pipe_end *reader = pipe_end->connection;
    data_size_t avail = 0;

    if (!reader) return;

    ignore_reselect = 1;

    LIST_FOR_EACH_ENTRY_SAFE( message, next, &reader->message_queue, struct pipe_message, entry )
    {
        if (message->async && message->iosb->status != STATUS_PENDING)
        {
            release_object( message->async );
            message->async = NULL;
            free_message( message );
        }
        else
        {
            avail += message->iosb->in_size - message->read_pos;
            if (message->async && (avail <= reader->buffer_size || !message->iosb->in_size))
            {
                wake_message( message, message->iosb->in_size );
            }
            else if (message->async && (pipe_end->flags & NAMED_PIPE_NONBLOCKING_MODE))
            {
                wake_message( message, message->read_pos );
                free_message( message );
            }
        }
    }

    ignore_reselect = 0;
    reselect_read_queue( reader, 0 );
}

static void pipe_end_read( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct pipe_end *pipe_end = get_fd_user( fd );

    switch (pipe_end->state)
    {
    case FILE_PIPE_CONNECTED_STATE:
        if ((pipe_end->flags & NAMED_PIPE_NONBLOCKING_MODE) && list_empty( &pipe_end->message_queue ))
        {
            set_error( STATUS_PIPE_EMPTY );
            return;
        }
        break;
    case FILE_PIPE_DISCONNECTED_STATE:
        set_error( STATUS_PIPE_DISCONNECTED );
        return;
    case FILE_PIPE_LISTENING_STATE:
        set_error( STATUS_PIPE_LISTENING );
        return;
    case FILE_PIPE_CLOSING_STATE:
        if (!list_empty( &pipe_end->message_queue )) break;
        set_error( STATUS_PIPE_BROKEN );
        return;
    }

    begin_binding_read( pipe_end );
    queue_async( &pipe_end->read_q, async );
    reselect_read_queue( pipe_end, 0 );
    set_error( STATUS_PENDING );
}

static void pipe_end_write( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct pipe_end *pipe_end = get_fd_user( fd );
    struct pipe_message *message;
    struct iosb *iosb;

    switch (pipe_end->state)
    {
    case FILE_PIPE_CONNECTED_STATE:
        break;
    case FILE_PIPE_DISCONNECTED_STATE:
        set_error( STATUS_PIPE_DISCONNECTED );
        return;
    case FILE_PIPE_LISTENING_STATE:
        set_error( STATUS_PIPE_LISTENING );
        return;
    case FILE_PIPE_CLOSING_STATE:
        set_error( STATUS_PIPE_CLOSING );
        return;
    }

    if (!pipe_end->pipe->message_mode && !get_req_data_size()) return;

    iosb = async_get_iosb( async );
    message = queue_message( pipe_end->connection, iosb );
    release_object( iosb );
    if (!message) return;

    message->async = (struct async *)grab_object( async );
    queue_async( &pipe_end->write_q, async );
    reselect_read_queue( pipe_end->connection, 1 );
    set_error( STATUS_PENDING );
}

static void pipe_end_reselect_async( struct fd *fd, struct async_queue *queue )
{
    struct pipe_end *pipe_end = get_fd_user( fd );

    if (ignore_reselect) return;

    if (&pipe_end->write_q == queue)
        reselect_write_queue( pipe_end );
    else if (&pipe_end->read_q == queue)
        reselect_read_queue( pipe_end, 0 );
}

static enum server_fd_type pipe_end_get_fd_type( struct fd *fd )
{
    return FD_TYPE_DEVICE;
}

static void pipe_end_peek( struct pipe_end *pipe_end )
{
    unsigned reply_size = get_reply_max_size();
    FILE_PIPE_PEEK_BUFFER *buffer;
    struct pipe_message *message;
    data_size_t avail = 0;
    data_size_t message_length = 0;

    if (reply_size < offsetof( FILE_PIPE_PEEK_BUFFER, Data ))
    {
        set_error( STATUS_INFO_LENGTH_MISMATCH );
        return;
    }
    reply_size -= offsetof( FILE_PIPE_PEEK_BUFFER, Data );

    switch (pipe_end->state)
    {
    case FILE_PIPE_CONNECTED_STATE:
        break;
    case FILE_PIPE_CLOSING_STATE:
        if (!list_empty( &pipe_end->message_queue )) break;
        set_error( STATUS_PIPE_BROKEN );
        return;
    default:
        set_error( pipe_end->pipe ? STATUS_INVALID_PIPE_STATE : STATUS_PIPE_DISCONNECTED );
        return;
    }

    LIST_FOR_EACH_ENTRY( message, &pipe_end->message_queue, struct pipe_message, entry )
        avail += message->iosb->in_size - message->read_pos;
    reply_size = min( reply_size, avail );

    if (avail && pipe_end->pipe->message_mode)
    {
        message = LIST_ENTRY( list_head(&pipe_end->message_queue), struct pipe_message, entry );
        message_length = message->iosb->in_size - message->read_pos;
        reply_size = min( reply_size, message_length );
    }

    if (!(buffer = set_reply_data_size( offsetof( FILE_PIPE_PEEK_BUFFER, Data[reply_size] )))) return;
    buffer->NamedPipeState    = pipe_end->state;
    buffer->ReadDataAvailable = avail;
    buffer->NumberOfMessages  = 0;  /* FIXME */
    buffer->MessageLength     = message_length;

    if (reply_size)
    {
        data_size_t write_pos = 0, writing;
        LIST_FOR_EACH_ENTRY( message, &pipe_end->message_queue, struct pipe_message, entry )
        {
            writing = min( reply_size - write_pos, message->iosb->in_size - message->read_pos );
            memcpy( buffer->Data + write_pos, (const char *)message->iosb->in_data + message->read_pos,
                    writing );
            write_pos += writing;
            if (write_pos == reply_size) break;
        }
    }
    if (message_length > reply_size) set_error( STATUS_BUFFER_OVERFLOW );
}

static void pipe_end_transceive( struct pipe_end *pipe_end, struct async *async )
{
    struct pipe_message *message;
    struct iosb *iosb;

    if (!pipe_end->connection)
    {
        set_error( pipe_end->pipe ? STATUS_INVALID_PIPE_STATE : STATUS_PIPE_DISCONNECTED );
        return;
    }

    if (!(pipe_end->flags & NAMED_PIPE_MESSAGE_STREAM_READ))
    {
        set_error( STATUS_INVALID_READ_MODE );
        return;
    }

    /* not allowed if we already have read data buffered */
    if (!list_empty( &pipe_end->message_queue ))
    {
        set_error( STATUS_PIPE_BUSY );
        return;
    }

    iosb = async_get_iosb( async );
    /* ignore output buffer copy transferred because of METHOD_NEITHER */
    iosb->in_size -= iosb->out_size;
    /* transaction never blocks on write, so just queue a message without async */
    message = queue_message( pipe_end->connection, iosb );
    release_object( iosb );
    if (!message) return;
    reselect_read_queue( pipe_end->connection, 0 );

    begin_binding_read( pipe_end );
    queue_async( &pipe_end->read_q, async );
    reselect_read_queue( pipe_end, 0 );
    set_error( STATUS_PENDING );
}

static void pipe_end_get_connection_attribute( struct pipe_end *pipe_end )
{
    const char *attr = get_req_data();
    data_size_t value_size, attr_size = get_req_data_size();
    void *value;

    if (attr_size == sizeof("ClientProcessId") && !memcmp( attr, "ClientProcessId", attr_size ))
    {
        value = &pipe_end->client_pid;
        value_size = sizeof(pipe_end->client_pid);
    }
    else if (attr_size == sizeof("ServerProcessId") && !memcmp( attr, "ServerProcessId", attr_size ))
    {
        value = &pipe_end->server_pid;
        value_size = sizeof(pipe_end->server_pid);
    }
    else
    {
        set_error( STATUS_ILLEGAL_FUNCTION );
        return;
    }

    if (get_reply_max_size() < value_size)
    {
        set_error( STATUS_INFO_LENGTH_MISMATCH );
        return;
    }

    set_reply_data( value, value_size );
}

static void pipe_end_ioctl( struct pipe_end *pipe_end, ioctl_code_t code, struct async *async )
{
    switch(code)
    {
    case FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE:
        pipe_end_get_connection_attribute( pipe_end );
        break;

    case FSCTL_PIPE_PEEK:
        pipe_end_peek( pipe_end );
        break;

    case FSCTL_PIPE_TRANSCEIVE:
        pipe_end_transceive( pipe_end, async );
        break;

    default:
        default_fd_ioctl( pipe_end->fd, code, async );
    }
}

static void pipe_server_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct pipe_server *server = get_fd_user( fd );

    switch(code)
    {
    case FSCTL_PIPE_LISTEN:
        switch(server->pipe_end.state)
        {
        case FILE_PIPE_LISTENING_STATE:
            break;
        case FILE_PIPE_DISCONNECTED_STATE:
            server->pipe_end.state = FILE_PIPE_LISTENING_STATE;
            list_add_tail( &server->pipe_end.pipe->listeners, &server->entry );
            break;
        case FILE_PIPE_CONNECTED_STATE:
            set_error( STATUS_PIPE_CONNECTED );
            return;
        case FILE_PIPE_CLOSING_STATE:
            set_error( STATUS_PIPE_CLOSING );
            return;
        }

        if (server->pipe_end.flags & NAMED_PIPE_NONBLOCKING_MODE)
        {
            set_error( STATUS_PIPE_LISTENING );
            return;
        }
        queue_async( &server->listen_q, async );
        async_wake_up( &server->pipe_end.pipe->waiters, STATUS_SUCCESS );
        set_error( STATUS_PENDING );
        return;

    case FSCTL_PIPE_DISCONNECT:
        switch(server->pipe_end.state)
        {
        case FILE_PIPE_CONNECTED_STATE:
            /* dump the client connection - all data is lost */
            assert( server->pipe_end.connection );
            release_object( server->pipe_end.connection->pipe );
            server->pipe_end.connection->pipe = NULL;
            break;
        case FILE_PIPE_CLOSING_STATE:
            break;
        case FILE_PIPE_LISTENING_STATE:
            set_error( STATUS_PIPE_LISTENING );
            return;
        case FILE_PIPE_DISCONNECTED_STATE:
            set_error( STATUS_PIPE_DISCONNECTED );
            return;
        }

        pipe_end_disconnect( &server->pipe_end, STATUS_PIPE_DISCONNECTED );
        return;

    case FSCTL_PIPE_IMPERSONATE:
        /* while elevation is enforced, impersonate the client; otherwise the caller's own token. A client of
         * another session (a bridge, T5.2) is always impersonated with its own token */
        if (elevation_enforced() ||
            (server->pipe_end.connection && server->pipe_end.connection->obj.ops == &pipe_bridge_ops))
        {
            struct token *token, *client_token = server->pipe_end.connection ? server->pipe_end.connection->client_token : NULL;

            if (!client_token)
            {
                set_error( STATUS_CANNOT_IMPERSONATE );
                return;
            }
            if (!(token = token_duplicate( client_token, 0,
                                            token_impersonation_level( client_token, current->process->token ),
                                            NULL, NULL, 0, NULL, 0, FALSE )))
                return;
            if (current->token) release_object( current->token );
            current->token = token;
            return;
        }
        if (current->process->token) /* FIXME: use the client token */
        {
            struct token *token;
            if (!(token = token_duplicate( current->process->token, 0, SecurityImpersonation, NULL, NULL, 0, NULL, 0, FALSE )))
                return;
            if (current->token) release_object( current->token );
            current->token = token;
        }
        return;

    default:
        pipe_end_ioctl( &server->pipe_end, code, async );
    }
}

static void pipe_client_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct pipe_end *client = get_fd_user( fd );

    switch(code)
    {
    case FSCTL_PIPE_LISTEN:
        set_error( STATUS_ILLEGAL_FUNCTION );
        return;

    default:
        pipe_end_ioctl( client, code, async );
    }
}

static void init_pipe_end( struct pipe_end *pipe_end, struct named_pipe *pipe,
                           unsigned int pipe_flags, data_size_t buffer_size )
{
    pipe_end->pipe = (struct named_pipe *)grab_object( pipe );
    pipe_end->fd = NULL;
    pipe_end->flags = pipe_flags;
    pipe_end->connection = NULL;
    pipe_end->client_token = NULL;
    pipe_end->client_process = NULL;
    pipe_end->client_primary = NULL;
    memset( &pipe_end->client_token_id, 0, sizeof(pipe_end->client_token_id) );
    memset( &pipe_end->client_modified_id, 0, sizeof(pipe_end->client_modified_id) );
    pipe_end->client_impersonated = 0;
    memset( &pipe_end->consumed_writer, 0, sizeof(pipe_end->consumed_writer) );
    pipe_end->binding_reads = 0;
    pipe_end->consumed_valid = 0;
#ifdef WEX_E1S6_TRANSPORT_TEST
    pipe_end->binding = NULL;
#endif
    pipe_end->buffer_size = buffer_size;
    init_async_queue( &pipe_end->read_q );
    init_async_queue( &pipe_end->write_q );
    list_init( &pipe_end->message_queue );
}

/* E1-S18e R8: is obj one end of a connected single-instance pipe whose server and client ends were
 * both created by the broker (CreatePipe)? Returns its peer (reference taken) and the pipe (no
 * reference). */
int named_pipe_anonymous_end( struct object *obj, struct process *broker, struct object **peer,
                              struct object **pipe )
{
    struct pipe_end *end, *other, *server, *client;

    if (obj->ops != &pipe_server_ops && obj->ops != &pipe_client_ops) return 0;
    end = (struct pipe_end *)obj;
    if (!(other = end->connection) || other->connection != end || !end->pipe || end->pipe != other->pipe) return 0;
    server = obj->ops == &pipe_server_ops ? end : other;
    client = server == end ? other : end;
    if (server->obj.ops != &pipe_server_ops || client->obj.ops != &pipe_client_ops) return 0;
    if (server->server_pid != get_process_id( broker ) || client->client_pid != get_process_id( broker )) return 0;
    if (end->pipe->maxinstances != 1 || end->pipe->instances != 1) return 0;
    *peer = grab_object( &other->obj );
    *pipe = &end->pipe->obj;
    return 1;
}

static struct pipe_server *create_pipe_server( struct named_pipe *pipe, unsigned int options,
                                               unsigned int pipe_flags )
{
    struct pipe_server *server;

    server = alloc_object( &pipe_server_ops );
    if (!server)
        return NULL;

    server->options = options;
    init_pipe_end( &server->pipe_end, pipe, pipe_flags, pipe->insize );
    server->pipe_end.state = FILE_PIPE_LISTENING_STATE;
    server->pipe_end.server_pid = get_process_id( current->process );
    init_async_queue( &server->listen_q );

    list_add_tail( &pipe->listeners, &server->entry );
    if (!(server->pipe_end.fd = alloc_pseudo_fd( &pipe_server_fd_ops, &server->pipe_end.obj, options )))
    {
        release_object( server );
        return NULL;
    }
    allow_fd_caching( server->pipe_end.fd );
    set_fd_signaled( server->pipe_end.fd, 1 );
    async_wake_up( &pipe->waiters, STATUS_SUCCESS );
    return server;
}

static struct pipe_end *create_pipe_client( struct named_pipe *pipe, data_size_t buffer_size, unsigned int options )
{
    struct pipe_end *client;

    client = alloc_object( &pipe_client_ops );
    if (!client)
        return NULL;

    init_pipe_end( client, pipe, 0, buffer_size );
    client->state = FILE_PIPE_CONNECTED_STATE;
    client->client_pid = get_process_id( current->process );
    client->client_process = (struct process *)grab_object( current->process );
    client->client_impersonated = (current->token != NULL);
    if (current->process->token)
    {
        client->client_primary = (struct token *)grab_object( current->process->token );
        token_get_ids( client->client_primary, &client->client_token_id, &client->client_modified_id );
    }
    if (elevation_enforced())
    {
        struct token *token = current->token ? current->token : current->process->token;
        if (token) client->client_token = (struct token *)grab_object( token );
    }

    client->fd = alloc_pseudo_fd( &pipe_client_fd_ops, &client->obj, options );
    if (!client->fd)
    {
        release_object( client );
        return NULL;
    }
    allow_fd_caching( client->fd );
    set_fd_signaled( client->fd, 1 );

    return client;
}

static int named_pipe_link_name( struct object *obj, struct object_name *name, struct object *parent )
{
    if (parent->ops == &named_pipe_dir_ops) parent = &((struct named_pipe_device_file *)parent)->device->obj;
    if (parent->ops != &named_pipe_device_ops)
    {
        set_error( STATUS_OBJECT_NAME_INVALID );
        return 0;
    }
    namespace_add( ((struct named_pipe_device *)parent)->pipes, name );
    name->parent = grab_object( parent );
    return 1;
}

static struct object *named_pipe_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options )
{
    struct named_pipe *pipe = (struct named_pipe *)obj;
    struct pipe_server *server;
    struct pipe_end *client;
    unsigned int pipe_sharing;

    if (pipe->s0_name) return pipe_bridge_open_remote( pipe, options );
    if (list_empty( &pipe->listeners ))
    {
        set_error( STATUS_PIPE_NOT_AVAILABLE );
        return NULL;
    }
    server = LIST_ENTRY( list_head( &pipe->listeners ), struct pipe_server, entry );

    pipe_sharing = pipe->sharing;
    if (((access & GENERIC_READ) && !(pipe_sharing & FILE_SHARE_READ)) ||
        ((access & GENERIC_WRITE) && !(pipe_sharing & FILE_SHARE_WRITE)))
    {
        set_error( STATUS_ACCESS_DENIED );
        return NULL;
    }

    if ((client = create_pipe_client( pipe, pipe->outsize, options )))
    {
        async_wake_up( &server->listen_q, STATUS_SUCCESS );
        server->pipe_end.state = FILE_PIPE_CONNECTED_STATE;
        server->pipe_end.connection = client;
        client->connection = &server->pipe_end;
        server->pipe_end.client_pid = client->client_pid;
        client->server_pid = server->pipe_end.server_pid;
        list_remove( &server->entry );
    }
    return client ? &client->obj : NULL;
}

static void named_pipe_device_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    switch(code)
    {
    case FSCTL_PIPE_WAIT:
    case FSCTL_PIPE_LISTEN:
    case FSCTL_PIPE_IMPERSONATE:
        set_error( STATUS_ILLEGAL_FUNCTION );
        return;

    case FSCTL_PIPE_DISCONNECT:
    case FSCTL_PIPE_TRANSCEIVE:
        set_error( STATUS_PIPE_DISCONNECTED );
        return;

    case FSCTL_PIPE_QUERY_CLIENT_PROCESS:
        set_error( STATUS_INVALID_PARAMETER );
        return;

    default:
        default_fd_ioctl( fd, code, async );
    }
}

static void named_pipe_dir_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct named_pipe_device *device = get_fd_user( fd );

    switch(code)
    {
    case FSCTL_PIPE_WAIT:
        {
            const FILE_PIPE_WAIT_FOR_BUFFER *buffer = get_req_data();
            data_size_t size = get_req_data_size();
            struct named_pipe *pipe;
            timeout_t when;
            struct object_params params = { .ops = &named_pipe_ops, .root = &device->obj };

            if (size < sizeof(*buffer) ||
                size < FIELD_OFFSET(FILE_PIPE_WAIT_FOR_BUFFER, Name[buffer->NameLength/sizeof(WCHAR)]))
            {
                set_error( STATUS_INVALID_PARAMETER );
                return;
            }
            params.name.str = buffer->Name;
            params.name.len = (buffer->NameLength / sizeof(WCHAR)) * sizeof(WCHAR);
            if (!(pipe = open_named_object( &params ))) return;
            if (pipe->s0_name)  /* T5.2: whether session 0 has an instance free shows at the open */
            {
                release_object( pipe );
                return;
            }

            if (list_empty( &pipe->listeners ))
            {
                queue_async( &pipe->waiters, async );
                when = buffer->TimeoutSpecified ? buffer->Timeout.QuadPart : pipe->timeout;
                async_set_timeout( async, when, STATUS_IO_TIMEOUT );
                set_error( STATUS_PENDING );
            }

            release_object( pipe );
            return;
        }

    default:
        named_pipe_device_ioctl( fd, code, async );
    }
}


DECL_HANDLER(create_named_pipe)
{
    struct named_pipe *pipe;
    struct pipe_server *server;
    struct named_pipe_init_data data = { .maxinstances = req->maxinstances, .outsize = req->outsize,
                                         .insize = req->insize, .sharing = req->sharing,
                                         .timeout = req->timeout, .flags = req->flags };
    struct object_params params = { .ops = &named_pipe_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;

    if (!req->sharing || (req->sharing & ~(FILE_SHARE_READ | FILE_SHARE_WRITE)) ||
        (!(req->flags & NAMED_PIPE_MESSAGE_STREAM_WRITE) && (req->flags & NAMED_PIPE_MESSAGE_STREAM_READ)))
    {
        if (params.root) release_object( params.root );
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!req->access)
    {
        if (params.root) release_object( params.root );
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    if (!params.name.len)  /* pipes need a root directory even without a name */
    {
        if (!params.objattr->rootdir)
        {
            set_error( STATUS_OBJECT_PATH_SYNTAX_BAD );
            return;
        }
        if (!(params.root = get_handle_obj( current->process, params.objattr->rootdir, 0, NULL ))) return;
    }

    switch (req->disposition)
    {
    case FILE_OPEN:
        pipe = open_named_object( &params );
        break;
    case FILE_CREATE:
    case FILE_OPEN_IF:
        params.attr |= OBJ_OPENIF;
        pipe = create_named_object( &params );
        reply->created = (pipe && get_error() != STATUS_OBJECT_NAME_EXISTS);
        break;
    default:
        pipe = NULL;
        set_error( STATUS_INVALID_PARAMETER );
        break;
    }

    if (params.root) release_object( params.root );
    if (!pipe) return;
    if (pipe->s0_name)  /* T5.2: session 0's pipes are served in session 0 only */
    {
        release_object( pipe );
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    if (!reply->created)
    {
        if (pipe->maxinstances <= pipe->instances)
        {
            set_error( STATUS_INSTANCE_NOT_AVAILABLE );
            release_object( pipe );
            return;
        }
        if (pipe->sharing != req->sharing || req->disposition == FILE_CREATE)
        {
            set_error( STATUS_ACCESS_DENIED );
            release_object( pipe );
            return;
        }
        clear_error(); /* clear the name collision */
    }

    server = create_pipe_server( pipe, req->options, req->flags );
    if (server)
    {
        reply->handle = alloc_handle( current->process, server, req->access, params.attr );
        pipe->instances++;
        release_object( server );
    }

    release_object( pipe );
}

DECL_HANDLER(set_named_pipe_info)
{
    struct pipe_end *pipe_end;

    pipe_end = (struct pipe_end *)get_handle_obj( current->process, req->handle,
                                                  FILE_WRITE_ATTRIBUTES, &pipe_server_ops );
    if (!pipe_end)
    {
        if (get_error() != STATUS_OBJECT_TYPE_MISMATCH)
            return;

        clear_error();
        pipe_end = (struct pipe_end *)get_handle_obj( current->process, req->handle,
                                                      0, &pipe_client_ops );
        if (!pipe_end) return;
    }

    if (!pipe_end->pipe)
    {
        set_error( STATUS_PIPE_DISCONNECTED );
    }
    else if ((req->flags & ~(NAMED_PIPE_MESSAGE_STREAM_READ | NAMED_PIPE_NONBLOCKING_MODE)) ||
            ((req->flags & NAMED_PIPE_MESSAGE_STREAM_READ) && !pipe_end->pipe->message_mode))
    {
        set_error( STATUS_INVALID_PARAMETER );
    }
    else
    {
        pipe_end->flags = req->flags;
    }

    release_object( pipe_end );
}

/* WineExplorer private: opener and the single complete request message consumed by this server end. */
#ifdef WEX_E1S6_TRANSPORT_TEST
#define E1S6_BINDING_AUDIT(site) fprintf( stderr, "E1S6_DENY pipe site=%s\n", site )
#else
#define E1S6_BINDING_AUDIT(site) do {} while (0)
#endif

#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
/* E1-S18e R10: the consumed record for token issue comes from the verifier, never from broker bytes.
 * After the verifier has consumed the evidence it sends one RECEIPT packet (E1R8 type 30) on the control
 * socket the server retained at endorsement, before it answers the broker. The broker's E1R8 op 4 only
 * names its S10 context; the server reads that one packet without blocking, requires the kernel
 * credentials of the endorsed root verifier, compares every field with the tuple it sealed at
 * ENDORSE_DIALOG and with the current generations, builds the kind 4 input itself and installs it with
 * the retained opener objects. Any failure burns the context. */
struct receipt_cookie
{
    int verifier_pin;
    unsigned long long root_gen, map_gen, policy_gen;
};

static int receipt_validate( void *cookie, const unsigned char *input )
{
    struct receipt_cookie *c = cookie;
    struct pollfd pin = { c->verifier_pin, POLLIN, 0 };
    unsigned long long gen;

    if (poll( &pin, 1, 0 )) return 0;  /* the verifier that vouched has gone */
    if (!accounts_get_map_generation( &gen ) || gen != c->map_gen) return 0;
    if (!sysrun_get_root_generation( &gen ) || gen != c->root_gen) return 0;
    if (!sysrun_get_policy_generation( &gen ) || gen != c->policy_gen) return 0;
    return 1;
}

static void receipt_release( void *cookie )
{
    struct receipt_cookie *c = cookie;
    close( c->verifier_pin );
    free( c );
}

static int receipt_nonzero( const unsigned char *p, unsigned int n )
{
    unsigned char bits = 0;
    while (n--) bits |= *p++;
    return bits != 0;
}

static int install_verifier_receipt( struct pipe_binding *b, obj_handle_t context )
{
    static const unsigned char evidence_head[8] = { 3, 0, 1, 0, 220, 0, 0, 0 };
    unsigned char pkt[512], input[WEX_TOKEN_INPUT_SIZE];
    const unsigned char *t = b->sealed_tuple, *ev = pkt + 92;
    char canonical[64], ancillary[256];
    struct iovec io = { pkt, sizeof(pkt) };
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    struct ucred peer, cred;
    struct pollfd live = { -1, POLLIN, 0 };
    socklen_t len = sizeof(peer);
    struct e1s_token_consumed r;
    struct receipt_cookie *cookie = NULL;
    struct timespec ts;
    struct luid id, mod;
    unsigned long long now, completed, expiry, gen, server_gen, broker_gen;
    unsigned int i, name_len, creds = 0;
    int fd = current->process->e1s6_control_fd, pin = -1, one = 1, ok = 0;
    const char *site;
    ssize_t n;

    site = "receipt-opener";
    if (b->opener_process->is_terminating || !b->opener_process->running_threads ||
        b->opener_process->token != b->opener_token) goto done;
    token_get_ids( b->opener_token, &id, &mod );
    if (memcmp( &id, &b->opener_token_id, sizeof(id) ) || memcmp( &mod, &b->opener_modified_id, sizeof(mod) )) goto done;

    site = "receipt-generations";
    if (!sysrun_get_s6_generations( current->process, &server_gen, &broker_gen ) ||
        server_gen != b->server_gen || broker_gen != b->broker_gen) goto done;
    if (!accounts_get_map_generation( &gen ) || gen != b->map_gen) goto done;
    if (!sysrun_get_root_generation( &gen ) || gen != b->root_gen) goto done;
    if (!sysrun_get_policy_generation( &gen ) || gen != b->policy_gen) goto done;

    /* the endorsed verifier: root peer of the retained control socket, pinned and alive */
    site = "receipt-verifier";
    if (fd < 0 || getsockopt( fd, SOL_SOCKET, SO_PEERCRED, &peer, &len ) || len != sizeof(peer) ||
        peer.uid != 0 || peer.pid <= 0) goto done;
    len = sizeof(pin);
    if (getsockopt( fd, SOL_SOCKET, 77 /* SO_PEERPIDFD */, &pin, &len ) || len != sizeof(pin) || pin < 0)
    { pin = -1; goto done; }
    live.fd = pin;
    if (poll( &live, 1, 0 )) goto done;
    if (setsockopt( fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one) )) goto done;

    /* exactly one packet, already queued: the verifier sends it before it answers the broker */
    site = "receipt-receive";
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = ancillary; msg.msg_controllen = sizeof(ancillary);
    n = recvmsg( fd, &msg, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC );
    if (n < 0) goto done;
    for (cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS &&
            cmsg->cmsg_len == CMSG_LEN( sizeof(cred) ))
        {
            memcpy( &cred, CMSG_DATA( cmsg ), sizeof(cred) );
            if (cred.uid != 0 || cred.pid != peer.pid) creds = 2;
            creds++;
        }
        else
        {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            {
                int *fds = (int *)CMSG_DATA( cmsg );
                unsigned int count = (cmsg->cmsg_len - CMSG_LEN( 0 )) / sizeof(int);
                for (i = 0; i < count; i++) close( fds[i] );
            }
            creds = 2;
        }
    }
    site = "receipt-sender";
    if (creds != 1 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) goto done;

    /* header: this attempt's request, attempt, nonce and channel as sealed at ENDORSE_DIALOG */
    site = "receipt-frame";
    if (n < 317 || n > 379 || memcmp( pkt, "E1R8", 4 ) || pkt[4] != 1 || pkt[5] || pkt[6] != 30 || pkt[7] ||
        unpack_u32_le( pkt + 8 ) != (unsigned int)n || memcmp( pkt + 12, t, 12 ) || unpack_u32_le( pkt + 24 ) ||
        memcmp( pkt + 28, t + 12, 48 ) || !receipt_nonzero( pkt + 76, 16 )) goto done;

    /* the exact 220-byte evidence record against the sealed tuple and the binding's generations */
    site = "receipt-evidence";
    if (clock_gettime( CLOCK_MONOTONIC, &ts ) || ts.tv_sec <= 0) goto done;
    now = (unsigned long long)ts.tv_sec * 1000000000 + ts.tv_nsec;
    completed = unpack_u64_le( ev + 148 );
    expiry = unpack_u64_le( ev + 156 );
    if (memcmp( ev, evidence_head, 8 ) || unpack_u64_le( ev + 8 ) != 0x3ff ||
        unpack_u32_le( ev + 212 ) != 1 || unpack_u32_le( ev + 216 ) != 1) goto done;
    site = "receipt-evidence-request";
    if (memcmp( ev + 16, t, 8 ) || memcmp( ev + 24, t + 92, 32 ) || memcmp( ev + 56, t + 60, 32 ) ||
        memcmp( ev + 88, t + 12, 32 ) || memcmp( ev + 120, t + 8, 4 )) goto done;
    site = "receipt-evidence-account";
    if (!receipt_nonzero( ev + 124, 16 ) || !unpack_u64_le( ev + 140 ) ||
        unpack_u64_le( ev + 140 ) >= 0xffffffffull) goto done;
    site = "receipt-evidence-time";
    if (!completed || completed > now || now >= expiry || expiry > unpack_u64_le( t + 124 )) goto done;
    /* The record's root and verifier generations are the verifier's own instance values: the verifier
     * generation is the one sealed in the tuple; its root generation has no server counterpart (the
     * server's root generation is a path generation and stays in the kind 4 input from the tuple). */
    site = "receipt-evidence-generations";
    if (unpack_u64_le( ev + 172 ) != b->server_gen || unpack_u64_le( ev + 180 ) != b->broker_gen ||
        unpack_u64_le( ev + 188 ) != unpack_u64_le( t + 156 ) || unpack_u64_le( ev + 196 ) != b->map_gen ||
        unpack_u64_le( ev + 204 ) != b->policy_gen) goto done;

    site = "receipt-name";
    name_len = unpack_u32_le( pkt + 312 );
    if (!name_len || name_len > 63 || (unsigned int)n != 316 + name_len) goto done;
    for (i = 0; i < name_len; i++) if (pkt[316 + i] <= 0x20 || pkt[316 + i] >= 0x7f) goto done;
    memcpy( canonical, pkt + 316, name_len );
    canonical[name_len] = 0;

    /* kind 4 from the sealed tuple and the receipt; nothing here is a broker byte */
    memset( input, 0, sizeof(input) );
    wex_token_put32( input, 0x00010004 );
    wex_token_put32( input + 4, WEX_TOKEN_REQUEST_SIZE );
    wex_token_put64( input + 8, WEX_TOKEN_CAPS );
    memcpy( input + 16, t, 8 );             /* request id */
    memcpy( input + 24, t + 92, 32 );       /* request digest */
    memcpy( input + 56, t + 60, 32 );       /* binding digest */
    memcpy( input + 88, t + 12, 32 );       /* nonce */
    memcpy( input + 120, t + 8, 4 );        /* attempt */
    memcpy( input + 124, ev + 124, 16 );    /* account key the verifier authenticated */
    memcpy( input + 140, t + 132, 24 );     /* root, server, broker generations */
    memcpy( input + 164, t + 164, 16 );     /* map, policy generations */
    wex_token_put32( input + 180, b->caller_session );
    memcpy( input + 188, pkt + 76, 16 );    /* evidence reference */

    site = "receipt-install";
    if (!(cookie = malloc( sizeof(*cookie) ))) goto done;
    cookie->verifier_pin = pin;
    cookie->root_gen = b->root_gen; cookie->map_gen = b->map_gen; cookie->policy_gen = b->policy_gen;
    memset( &r, 0, sizeof(r) );
    memcpy( r.input, input, sizeof(r.input) );
    r.canonical_name = canonical;
    r.caller = b->opener_process;
    r.primary = b->opener_token;
    r.expiry = expiry;
    r.validate = receipt_validate;
    r.release = receipt_release;
    r.cookie = cookie;
    if ((ok = token_install_e1s_consumed( context, &r ))) { pin = -1; cookie = NULL; }
done:
    if (!ok)
    {
        E1S6_BINDING_AUDIT( site );
        token_install_e1s_consumed( context, NULL );  /* a failed receipt burns the context */
        set_error( STATUS_ACCESS_DENIED );
    }
    else fprintf( stderr, "wex-r8: receipt installed\n" );
    free( cookie );
    if (pin >= 0) close( pin );
    return ok;
}
#endif

static int check_pipe_writer_binding( struct pipe_end *client, struct pipe_writer *writer )
{
    struct luid opener_id, opener_modified, writer_id, writer_modified;
    token_get_ids( client->client_primary, &opener_id, &opener_modified );
    token_get_ids( writer->primary, &writer_id, &writer_modified );
#define CHECK_WRITER(c, site) do { if (!(c)) { E1S6_BINDING_AUDIT( site ); \
    set_error( STATUS_ACCESS_DENIED ); return 0; } } while (0)
    CHECK_WRITER( !client->client_impersonated, "opener-impersonated" );
    CHECK_WRITER( !writer->impersonated, "writer-impersonated" );
    CHECK_WRITER( writer->process == client->client_process, "writer-process" );
    CHECK_WRITER( writer->primary == client->client_primary, "writer-primary" );
    CHECK_WRITER( client->client_process->token == client->client_primary, "opener-primary-current" );
    CHECK_WRITER( !memcmp( &opener_id, &client->client_token_id, sizeof(opener_id) ), "opener-token-id" );
    CHECK_WRITER( !memcmp( &opener_modified, &client->client_modified_id, sizeof(opener_modified) ), "opener-modified-id" );
    CHECK_WRITER( !memcmp( &writer_id, &writer->token_id, sizeof(writer_id) ), "writer-token-id" );
    CHECK_WRITER( !memcmp( &writer_modified, &writer->modified_id, sizeof(writer_modified) ), "writer-modified-id" );
#undef CHECK_WRITER
    return 1;
}

DECL_HANDLER(get_pipe_caller_binding)
{
    struct pipe_end *server, *client;
    struct pipe_writer *writer;
    struct luid token_id, modified_id;
    obj_handle_t process = 0, token = 0, writer_process = 0, writer_token = 0;
#ifdef WEX_E1S6_TRANSPORT_TEST
    const unsigned char *req_data;
    data_size_t req_size;
    struct pipe_binding *b;
    int rng, i;
    unsigned char reply_buf[132];
    unsigned char *ptr;
    static const unsigned char zero16[16] = {0};
#endif

    /* Keep the same system-role / SeTcbPrivilege gate as create_linked_token. */
    if (!elevation_enforced() || token_service_account( current->process->token ) != SERVICE_ACCOUNT_SYSTEM ||
        !sysrun_process_privileged( current->process ) || !token_has_privilege( current->process->token, SeTcbPrivilege ))
    {
        E1S6_BINDING_AUDIT( "authority" );
        set_error( STATUS_ACCESS_DENIED );
        return;
    }

    server = (struct pipe_end *)get_handle_obj( current->process, req->pipe,
                                               FILE_READ_ATTRIBUTES, &pipe_server_ops );
    if (!server)
    {
        E1S6_BINDING_AUDIT( "server-handle" );
        if (get_error() == STATUS_INVALID_HANDLE || get_error() == STATUS_OBJECT_TYPE_MISMATCH)
            set_error( STATUS_INVALID_PARAMETER_1 );
        return;
    }

    if (server->state != FILE_PIPE_CONNECTED_STATE || !(client = server->connection))
    {
        E1S6_BINDING_AUDIT( "connection" );
        set_error( STATUS_PIPE_DISCONNECTED );
        goto done;
    }
    writer = &server->consumed_writer;
    if (!client->client_process || !client->client_primary || !server->consumed_valid ||
        !writer->process || !writer->primary)
    {
        E1S6_BINDING_AUDIT( "consumed-provenance" );
        set_error( STATUS_ACCESS_DENIED );
        goto done;
    }

    if (get_req_data_size())
    {
        if (!check_pipe_writer_binding( client, writer )) goto done;

#ifdef WEX_E1S6_TRANSPORT_TEST
        req_data = get_req_data();
        req_size = get_req_data_size();
        if (req_size >= 4 && !memcmp( req_data, "E1R8", 4 ))
        {
            unsigned short version, op;

            if (req_size < 8)
            {
                E1S6_BINDING_AUDIT( "e1r8-size" );
                set_error( STATUS_ACCESS_DENIED );
                goto done;
            }
            version = req_data[4] | (req_data[5] << 8);
            if (version != 1)
            {
                E1S6_BINDING_AUDIT( "e1r8-version" );
                set_error( STATUS_ACCESS_DENIED );
                goto done;
            }
            op = req_data[6] | (req_data[7] << 8);

            if (op == 1)
            {
                if (req_size != 8)
                {
                    E1S6_BINDING_AUDIT( "e1r8-size" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (!sysrun_process_is_broker( current->process ))
                {
                    E1S6_BINDING_AUDIT( "caller-not-broker" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!process_pin_matches( client->client_process ))
                {
                    E1S6_BINDING_AUDIT( "opener-pin-missing" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!writer->desktop)
                {
                    E1S6_BINDING_AUDIT( "writer-desktop-missing" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (server->binding)
                {
                    E1S6_BINDING_AUDIT( "binding-exists" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                b = mem_alloc( sizeof(*b) );
                if (!b)
                {
                    E1S6_BINDING_AUDIT( "binding-alloc" );
                    set_error( STATUS_NO_MEMORY );
                    goto done;
                }
                memset( b, 0, sizeof(*b) );

                if (!registry_get_prefix_id( b->prefix_id ))
                {
                    free( b );
                    E1S6_BINDING_AUDIT( "prefix-id" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                rng = open( "/dev/urandom", O_RDONLY | O_CLOEXEC );
                if (rng < 0)
                {
                    free( b );
                    E1S6_BINDING_AUDIT( "rng-open" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                while (!memcmp( b->host_process_id, zero16, 16 ) || !memcmp( b->host_process_id, b->prefix_id, 16 ))
                    if (read( rng, b->host_process_id, 16 ) != 16) { close( rng ); free( b ); set_error( STATUS_ACCESS_DENIED ); goto done; }
                while (!memcmp( b->opener_id, zero16, 16 ) || !memcmp( b->opener_id, b->prefix_id, 16 ) || !memcmp( b->opener_id, b->host_process_id, 16 ))
                    if (read( rng, b->opener_id, 16 ) != 16) { close( rng ); free( b ); set_error( STATUS_ACCESS_DENIED ); goto done; }
                while (!memcmp( b->writer_id, zero16, 16 ) || !memcmp( b->writer_id, b->prefix_id, 16 ) || !memcmp( b->writer_id, b->host_process_id, 16 ) || !memcmp( b->writer_id, b->opener_id, 16 ))
                    if (read( rng, b->writer_id, 16 ) != 16) { close( rng ); free( b ); set_error( STATUS_ACCESS_DENIED ); goto done; }
                while (!memcmp( b->desktop_route, zero16, 16 ) || !memcmp( b->desktop_route, b->prefix_id, 16 ) || !memcmp( b->desktop_route, b->host_process_id, 16 ) || !memcmp( b->desktop_route, b->opener_id, 16 ) || !memcmp( b->desktop_route, b->writer_id, 16 ))
                    if (read( rng, b->desktop_route, 16 ) != 16) { close( rng ); free( b ); set_error( STATUS_ACCESS_DENIED ); goto done; }
                close( rng );

                if (!sysrun_get_s6_generations( current->process, &b->server_gen, &b->broker_gen ))
                {
                    free( b );
                    E1S6_BINDING_AUDIT( "s6-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!accounts_get_map_generation( &b->map_gen ))
                {
                    free( b );
                    E1S6_BINDING_AUDIT( "map-generation" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_get_root_generation( &b->root_gen ) || !sysrun_get_policy_generation( &b->policy_gen ))
                {
                    free( b );
                    E1S6_BINDING_AUDIT( "path-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                b->caller_session = client->client_process->session_id;
                b->host_uid = client->client_process->wex_pin_uid;
                b->opener_process = (struct process *)grab_object( client->client_process );
                b->writer_process = (struct process *)grab_object( writer->process );
                b->desktop = (struct desktop *)grab_object( writer->desktop );
                b->opener_token = (struct token *)grab_object( client->client_primary );
                token_get_ids( b->opener_token, &b->opener_token_id, &b->opener_modified_id );
                server->binding = b;

                ptr = reply_buf;
                memcpy( ptr, b->prefix_id, 16 ); ptr += 16;
                memcpy( ptr, b->host_process_id, 16 ); ptr += 16;
                memcpy( ptr, b->opener_id, 16 ); ptr += 16;
                memcpy( ptr, b->writer_id, 16 ); ptr += 16;
                memcpy( ptr, b->desktop_route, 16 ); ptr += 16;

                for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->server_gen >> (8 * i));
                for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->root_gen >> (8 * i));
                for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->broker_gen >> (8 * i));
                for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->map_gen >> (8 * i));
                for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->policy_gen >> (8 * i));

                for (i = 0; i < 4; i++) *ptr++ = (unsigned char)(b->caller_session >> (8 * i));
                for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->host_uid >> (8 * i));
                assert( ptr - reply_buf == 132 );
                set_reply_data( reply_buf, sizeof(reply_buf) );
            }
            else if (op == 2)
            {
                obj_handle_t d_handle;
                const unsigned char *tuple;
                struct process *D;
                unsigned long long cur_server_gen, cur_broker_gen;
                unsigned long long cur_map_gen, cur_root_gen, cur_policy_gen;
                unsigned long long t_deadline, t_root_gen, t_server_gen, t_broker_gen;
                unsigned long long t_map_gen, t_policy_gen;
                unsigned int t_caller_session, t_desktop_mode;
                struct luid op_id, op_mod;

                if (req_size != 216)
                {
                    E1S6_BINDING_AUDIT( "e1r8-size" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_process_is_broker( current->process ))
                {
                    E1S6_BINDING_AUDIT( "caller-not-broker" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (current->token)
                {
                    E1S6_BINDING_AUDIT( "caller-has-token" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_get_s6_generations( current->process, &cur_server_gen, &cur_broker_gen ))
                {
                    E1S6_BINDING_AUDIT( "s6-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!server->binding)
                {
                    E1S6_BINDING_AUDIT( "binding-missing" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                b = server->binding;
                /* A further attempt of the same request gets a fresh dialog: only once the previous
                 * dialog is gone, before any receipt, with the next attempt number (at most 3) and the
                 * same request id and deadline. */
                if (b->endorsed &&
                    (b->receipt_used || b->dialog_process->running_threads ||
                     memcmp( req_data + 12, b->sealed_tuple, 8 ) ||
                     unpack_u32_le( req_data + 12 + 8 ) != unpack_u32_le( b->sealed_tuple + 8 ) + 1 ||
                     unpack_u32_le( req_data + 12 + 8 ) > 3 ||
                     memcmp( req_data + 12 + 124, b->sealed_tuple + 124, 8 )))
                {
                    E1S6_BINDING_AUDIT( "already-endorsed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                d_handle = (obj_handle_t)unpack_u32_le( req_data + 8 );
                D = get_process_from_handle( d_handle, PROCESS_QUERY_LIMITED_INFORMATION );
                if (!D)
                {
                    E1S6_BINDING_AUDIT( "dialog-handle" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (D->wex_creator != current->process)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-creator" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!D->wex_protected[0])
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-not-protected" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!D->wex_dialog_image)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-image-not-consent" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!process_unstarted_suspended( D ))
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-not-suspended" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (token_service_account( D->token ) != SERVICE_ACCOUNT_SYSTEM)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-not-system" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!process_pin_matches( D ))
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-pin-mismatch" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (D->session_id != b->caller_session)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-session-mismatch" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (!dialog_desktop_ok( D, b, unpack_u32_le( req_data + 12 + 200 ) ))
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "dialog-desktop-mismatch" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                tuple = req_data + 12;
                t_deadline = unpack_u64_le( tuple + 124 );
                t_root_gen = unpack_u64_le( tuple + 132 );
                t_server_gen = unpack_u64_le( tuple + 140 );
                t_broker_gen = unpack_u64_le( tuple + 148 );
                /* tuple + 156 is verifier_gen, not checked by server */
                t_map_gen = unpack_u64_le( tuple + 164 );
                t_policy_gen = unpack_u64_le( tuple + 172 );
                t_caller_session = unpack_u32_le( tuple + 180 );
                t_desktop_mode = unpack_u32_le( tuple + 200 );

                if (!t_deadline)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-deadline" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (t_root_gen != b->root_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-root-gen" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (t_server_gen != b->server_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-server-gen" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (t_broker_gen != b->broker_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-broker-gen" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (t_map_gen != b->map_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-map-gen" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (t_policy_gen != b->policy_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-policy-gen" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (t_caller_session != b->caller_session)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-caller-session" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (memcmp( tuple + 184, b->desktop_route, 16 ))
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-desktop-route" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (t_desktop_mode != 1 && t_desktop_mode != 2)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "tuple-desktop-mode" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (cur_server_gen != b->server_gen || cur_broker_gen != b->broker_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "current-s6-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!accounts_get_map_generation( &cur_map_gen ) || cur_map_gen != b->map_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "current-map-generation" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_get_root_generation( &cur_root_gen ) || cur_root_gen != b->root_gen ||
                    !sysrun_get_policy_generation( &cur_policy_gen ) || cur_policy_gen != b->policy_gen)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "current-path-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (b->opener_process->is_terminating || b->opener_process->running_threads == 0)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "opener-dead" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (b->opener_process->token != b->opener_token)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "opener-token-changed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                token_get_ids( b->opener_token, &op_id, &op_mod );
                if (memcmp( &op_id, &b->opener_token_id, sizeof(op_id) ) ||
                    memcmp( &op_mod, &b->opener_modified_id, sizeof(op_mod) ))
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "opener-token-id-changed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                rng = open( "/dev/urandom", O_RDONLY | O_CLOEXEC );
                if (rng < 0)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "rng-open" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (b->endorsed) memset( b->dialog_id, 0, 16 );  /* a further attempt: a new dialog id */
                while (!memcmp( b->dialog_id, zero16, 16 ) ||
                       !memcmp( b->dialog_id, b->prefix_id, 16 ) ||
                       !memcmp( b->dialog_id, b->host_process_id, 16 ) ||
                       !memcmp( b->dialog_id, b->opener_id, 16 ) ||
                       !memcmp( b->dialog_id, b->writer_id, 16 ) ||
                       !memcmp( b->dialog_id, b->desktop_route, 16 ))
                {
                    if (read( rng, b->dialog_id, 16 ) != 16)
                    {
                        close( rng );
                        release_object( D );
                        E1S6_BINDING_AUDIT( "rng-read" );
                        set_error( STATUS_ACCESS_DENIED );
                        goto done;
                    }
                }
                close( rng );

                if (current->process->e1s6_control_fd < 0)
                {
                    release_object( D );
                    E1S6_BINDING_AUDIT( "endorsed-control-fd" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                else
                {
                    unsigned char endorsed_buf[244];
                    struct ucred sender = { getpid(), geteuid(), getegid() };
                    char ancillary[CMSG_SPACE(sizeof(sender)) + CMSG_SPACE(2 * sizeof(int))];
                    struct iovec io = { endorsed_buf, sizeof(endorsed_buf) };
                    struct msghdr msg = { 0 };
                    struct cmsghdr *cmsg;
                    int d_pidfd, raised, sent_ok, rights[2];

                    memcpy( endorsed_buf, "E1R8", 4 );
                    endorsed_buf[4] = 1; endorsed_buf[5] = 0;   /* version: 1 */
                    endorsed_buf[6] = 16; endorsed_buf[7] = 0;  /* type: 16 (ENDORSED) */
                    endorsed_buf[8] = 244; endorsed_buf[9] = 0; /* length: 244 */
                    endorsed_buf[10] = 0; endorsed_buf[11] = 0;
                    memcpy( endorsed_buf + 12, tuple, 8 );       /* request_id */
                    memcpy( endorsed_buf + 20, tuple + 8, 4 );   /* attempt */
                    endorsed_buf[24] = 0; endorsed_buf[25] = 0;  /* reserved: 0 */
                    endorsed_buf[26] = 0; endorsed_buf[27] = 0;
                    memcpy( endorsed_buf + 28, tuple + 12, 32 ); /* nonce */
                    memcpy( endorsed_buf + 60, tuple + 44, 16 ); /* channel_id */
                    /* Tuple tail: tuple[60..203] (144 bytes) + dialog_id (16 bytes) = 160 bytes */
                    memcpy( endorsed_buf + 76, tuple + 60, 144 );
                    memcpy( endorsed_buf + 220, b->dialog_id, 16 );
                    /* D's pin pid and uid (u32 LE each) */
                    for (i = 0; i < 4; i++) endorsed_buf[236 + i] = (unsigned char)((unsigned int)D->wex_pin_pid >> (8 * i));
                    for (i = 0; i < 4; i++) endorsed_buf[240 + i] = (unsigned char)(D->wex_pin_uid >> (8 * i));

                    /* a duplicate of D's kernel pin, never a fresh lookup by pid (pid reuse) */
                    d_pidfd = fcntl( D->wex_pin_pidfd, F_DUPFD_CLOEXEC, 0 );
                    if (d_pidfd < 0)
                    {
                        release_object( D );
                        E1S6_BINDING_AUDIT( "endorsed-pidfd-open" );
                        set_error( STATUS_ACCESS_DENIED );
                        goto done;
                    }

                    memset( ancillary, 0, sizeof(ancillary) );
                    msg.msg_iov = &io;
                    msg.msg_iovlen = 1;
                    msg.msg_control = ancillary;
                    msg.msg_controllen = sizeof(ancillary);

                    cmsg = CMSG_FIRSTHDR( &msg );
                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_CREDENTIALS;
                    cmsg->cmsg_len = CMSG_LEN( sizeof(sender) );

                    cmsg = CMSG_NXTHDR( &msg, cmsg );
                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_RIGHTS;
                    /* second right: this prefix's directory, so the verifier can provision the
                     * administrator's profile in it without trusting a path (the kernel duplicates it) */
                    rights[0] = d_pidfd;
                    rights[1] = config_dir_fd;
                    cmsg->cmsg_len = CMSG_LEN( sizeof(rights) );
                    memcpy( CMSG_DATA(cmsg), rights, sizeof(rights) );

                    sent_ok = 0;
                    raised = sysrun_raise();
                    if (raised)
                    {
                        sender.pid = getpid();
                        sender.uid = geteuid();
                        sender.gid = getegid();
                        cmsg = CMSG_FIRSTHDR( &msg );
                        memcpy( CMSG_DATA(cmsg), &sender, sizeof(sender) );
                        sent_ok = (sendmsg( current->process->e1s6_control_fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL ) == (ssize_t)sizeof(endorsed_buf));
                    }
                    sysrun_drop( raised );
                    close( d_pidfd );

                    if (!sent_ok)
                    {
                        close( current->process->e1s6_control_fd );
                        current->process->e1s6_control_fd = -1;
                        release_object( D );
                        E1S6_BINDING_AUDIT( "endorsed-send" );
                        set_error( STATUS_ACCESS_DENIED );
                        goto done;
                    }
                }

                if (b->dialog_process) release_object( b->dialog_process );
                if (b->dialog_token) release_object( b->dialog_token );
                b->dialog_process = (struct process *)grab_object( D );
                b->dialog_token = (struct token *)grab_object( D->token );
                token_get_ids( b->dialog_token, &b->dialog_token_id, &b->dialog_modified_id );
                b->dialog_pin_pid = D->wex_pin_pid;
                memcpy( b->sealed_tuple, tuple, 204 );
                memcpy( b->sealed_tuple + 204, b->dialog_id, 16 );
                b->endorsed = 1;
                release_object( D );

                set_reply_data( b->dialog_id, 16 );
            }
            else if (op == 3)
            {
                unsigned long long cur_server_gen, cur_broker_gen;
                unsigned long long cur_map_gen, cur_root_gen, cur_policy_gen;
                struct luid id, mod;

                if (req_size != 24)
                {
                    E1S6_BINDING_AUDIT( "e1r8-size" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_process_is_broker( current->process ))
                {
                    E1S6_BINDING_AUDIT( "recheck-caller-not-broker" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (current->token)
                {
                    E1S6_BINDING_AUDIT( "recheck-caller-has-token" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_get_s6_generations( current->process, &cur_server_gen, &cur_broker_gen ))
                {
                    E1S6_BINDING_AUDIT( "recheck-s6-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!server->binding || !server->binding->endorsed)
                {
                    E1S6_BINDING_AUDIT( "recheck-not-endorsed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                b = server->binding;
                if (memcmp( req_data + 8, b->dialog_id, 16 ))
                {
                    E1S6_BINDING_AUDIT( "recheck-dialog-id-mismatch" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (b->dialog_process->is_terminating || b->dialog_process->running_threads == 0)
                {
                    E1S6_BINDING_AUDIT( "recheck-dialog-dead" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (b->dialog_process->token != b->dialog_token)
                {
                    E1S6_BINDING_AUDIT( "recheck-dialog-token-changed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                token_get_ids( b->dialog_token, &id, &mod );
                if (memcmp( &id, &b->dialog_token_id, sizeof(id) ) ||
                    memcmp( &mod, &b->dialog_modified_id, sizeof(mod) ))
                {
                    E1S6_BINDING_AUDIT( "recheck-dialog-token-id-changed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (b->dialog_process->session_id != b->caller_session)
                {
                    E1S6_BINDING_AUDIT( "recheck-dialog-session-mismatch" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!dialog_desktop_ok( b->dialog_process, b, unpack_u32_le( b->sealed_tuple + 200 ) ))
                {
                    E1S6_BINDING_AUDIT( "recheck-dialog-desktop-mismatch" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (!process_pin_matches( b->dialog_process ) || b->dialog_process->wex_pin_pid != b->dialog_pin_pid)
                {
                    E1S6_BINDING_AUDIT( "recheck-dialog-pin-mismatch" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (b->opener_process->is_terminating || b->opener_process->running_threads == 0)
                {
                    E1S6_BINDING_AUDIT( "recheck-opener-dead" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (b->opener_process->token != b->opener_token)
                {
                    E1S6_BINDING_AUDIT( "recheck-opener-token-changed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                token_get_ids( b->opener_token, &id, &mod );
                if (memcmp( &id, &b->opener_token_id, sizeof(id) ) ||
                    memcmp( &mod, &b->opener_modified_id, sizeof(mod) ))
                {
                    E1S6_BINDING_AUDIT( "recheck-opener-token-id-changed" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }

                if (cur_server_gen != b->server_gen || cur_broker_gen != b->broker_gen)
                {
                    E1S6_BINDING_AUDIT( "recheck-current-s6-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!accounts_get_map_generation( &cur_map_gen ) || cur_map_gen != b->map_gen)
                {
                    E1S6_BINDING_AUDIT( "recheck-current-map-generation" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_get_root_generation( &cur_root_gen ) || cur_root_gen != b->root_gen ||
                    !sysrun_get_policy_generation( &cur_policy_gen ) || cur_policy_gen != b->policy_gen)
                {
                    E1S6_BINDING_AUDIT( "recheck-current-path-generations" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
            }
#ifdef __linux__
            else if (op == 4)
            {
                /* R10: install the verifier's receipt into the broker's S10 context (handle at 8) */
                if (req_size != 12)
                {
                    E1S6_BINDING_AUDIT( "e1r8-size" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!sysrun_process_is_broker( current->process ) || current->token)
                {
                    E1S6_BINDING_AUDIT( "receipt-caller-not-broker" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                if (!server->binding || !server->binding->endorsed || server->binding->receipt_used)
                {
                    E1S6_BINDING_AUDIT( "receipt-state" );
                    set_error( STATUS_ACCESS_DENIED );
                    goto done;
                }
                b = server->binding;
                b->receipt_used = 1;
                if (!install_verifier_receipt( b, (obj_handle_t)unpack_u32_le( req_data + 8 ) )) goto done;
            }
#endif
            else
            {
                E1S6_BINDING_AUDIT( "e1r8-op" );
                set_error( STATUS_ACCESS_DENIED );
                goto done;
            }
        }
        else
#endif
        {
            if (!sysrun_e1s6_endorse( get_req_data(), get_req_data_size() )) goto done;
        }
    }

    process = alloc_handle( current->process, client->client_process,
                            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION, 0 );
    if (!process) { E1S6_BINDING_AUDIT( "allocate-opener-process" ); goto done; }
    token = alloc_handle( current->process, client->client_primary, TOKEN_QUERY, 0 );
    if (!token) { E1S6_BINDING_AUDIT( "allocate-opener-token" ); goto done; }
    writer_process = alloc_handle( current->process, writer->process,
                                   SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION, 0 );
    if (!writer_process) { E1S6_BINDING_AUDIT( "allocate-writer-process" ); goto done; }
    writer_token = alloc_handle( current->process, writer->primary, TOKEN_QUERY, 0 );
    if (!writer_token) { E1S6_BINDING_AUDIT( "allocate-writer-token" ); goto done; }

    token_get_ids( client->client_primary, &token_id, &modified_id );
    reply->process = process;
    reply->token = token;
    reply->pid = client->client_pid;
    reply->start_time = client->client_process->start_time;
    reply->token_id = client->client_token_id;
    reply->modified_id = client->client_modified_id;
    reply->impersonated = client->client_impersonated;
    reply->token_current = client->client_primary == client->client_process->token &&
                           token_id.low_part == client->client_token_id.low_part &&
                           token_id.high_part == client->client_token_id.high_part &&
                           modified_id.low_part == client->client_modified_id.low_part &&
                           modified_id.high_part == client->client_modified_id.high_part;
    reply->writer_process = writer_process;
    reply->writer_token = writer_token;
    reply->writer_token_id = writer->token_id;
    reply->writer_modified_id = writer->modified_id;
    reply->version = PIPE_CALLER_BINDING_VERSION;
    reply->writer_flags = PIPE_WRITER_VALID;
    if (writer->impersonated) reply->writer_flags |= PIPE_WRITER_IMPERSONATED;
    token_get_ids( writer->primary, &token_id, &modified_id );
    if (writer->primary == writer->process->token &&
        token_id.low_part == writer->token_id.low_part && token_id.high_part == writer->token_id.high_part &&
        modified_id.low_part == writer->modified_id.low_part && modified_id.high_part == writer->modified_id.high_part)
        reply->writer_flags |= PIPE_WRITER_TOKEN_CURRENT;

done:
    if (get_error())
    {
        if (writer_token) close_handle( current->process, writer_token );
        if (writer_process) close_handle( current->process, writer_process );
        if (token) close_handle( current->process, token );
        if (process) close_handle( current->process, process );
    }
    release_object( server );
}

/* WineExplorer system tier T5.2: pipes of session 0.
 *
 * A program of a session that opens \Device\NamedPipe\WexSession0\<name> gets a client end whose other end is a
 * bridge: its server asks session 0 (session0_open_pipe), with the opener's token, for <name>; session 0 connects
 * a bridge of its own as the client of one of that pipe's listening instances, with the described token as the
 * client's token, so that the service impersonates the real caller. The two bridges are joined by a stream
 * socket; each message is sent as a 4-byte length (little endian) and its data, so message boundaries survive.
 * Only the pipes that session 0 lists may be opened this way. */

#define PIPE_BRIDGE_MAX_MESSAGE  (16 * 1024 * 1024)  /* a longer message ends the connection */
#define PIPE_BRIDGE_MAX_QUEUED   (4 * 1024 * 1024)   /* unread data before the bridge stops reading */
#define PIPE_BRIDGE_MAX_PER_USER 256                 /* session 0: open bridges per account */

struct pipe_bridge
{
    struct pipe_end pipe_end;
    struct list     entry;         /* in bridges */
    int             alive;         /* holds its own reference until the connection ends */
    uid_t           peer_uid;      /* session 0: the account of the session that opened it */
    unsigned char   tx_head[4];    /* the length of the message being sent */
    unsigned int    tx_head_pos;
    unsigned char   rx_head[4];    /* the length of the message being received */
    unsigned int    rx_head_pos;
    unsigned char  *rx_data;
    data_size_t     rx_size;
    data_size_t     rx_pos;
};

static struct list bridges = LIST_INIT( bridges );

static void pipe_bridge_dump( struct object *obj, int verbose )
{
    struct pipe_bridge *bridge = (struct pipe_bridge *)obj;
    fprintf( stderr, "Named pipe bridge connection=%p\n", bridge->pipe_end.connection );
}

static void pipe_bridge_destroy( struct object *obj )
{
    struct pipe_bridge *bridge = (struct pipe_bridge *)obj;

    list_remove( &bridge->entry );
    free( bridge->rx_data );
    pipe_end_destroy( obj );
}

static const struct object_ops pipe_bridge_ops =
{
    .size    = sizeof(struct pipe_bridge),
    .type    = &no_type,
    .dump    = pipe_bridge_dump,
    .destroy = pipe_bridge_destroy,
};

static void pipe_bridge_poll_event( struct fd *fd, int event );

static const struct fd_ops pipe_bridge_fd_ops =
{
    .poll_event = pipe_bridge_poll_event,
};

/* the connection ended: stop polling and drop the bridge's own reference (the last thing its caller does) */
static void pipe_bridge_closed( struct pipe_end *end )
{
    struct pipe_bridge *bridge = (struct pipe_bridge *)end;

    if (!bridge->alive) return;
    pipe_bridge_flush( end );  /* what the peer wrote before it closed, as far as the socket takes it now */
    bridge->alive = 0;
    if (end->fd) set_fd_events( end->fd, -1 );
    release_object( bridge );
}

/* read while the other end has room for more; always watch for the end of the connection */
static void pipe_bridge_update_events( struct pipe_end *end )
{
    struct pipe_bridge *bridge = (struct pipe_bridge *)end;
    int events = 0;

    if (!bridge->alive || !end->fd) return;
    if (end->connection && pipe_end_get_avail( end->connection ) < PIPE_BRIDGE_MAX_QUEUED) events |= POLLIN;
    if (!list_empty( &end->message_queue )) events |= POLLOUT;
    set_fd_events( end->fd, events );
}

/* send what the bridge's peer wrote. Called from requests too, so it never ends the connection itself: an error
 * shows up as POLLERR or POLLHUP in pipe_bridge_poll_event */
static void pipe_bridge_flush( struct pipe_end *end )
{
    struct pipe_bridge *bridge = (struct pipe_bridge *)end;
    struct pipe_message *message;
    int sock, saved_ignore = ignore_reselect;
    data_size_t size;
    ssize_t ret;

    if (!bridge->alive || !end->fd) return;
    sock = get_unix_fd( end->fd );
    /* completing a writer reselects its queue, which would flush again from the middle of this loop */
    ignore_reselect = 1;
    while (!list_empty( &end->message_queue ))
    {
        message = LIST_ENTRY( list_head( &end->message_queue ), struct pipe_message, entry );
        size = message->iosb->in_size;
        if (bridge->tx_head_pos < sizeof(bridge->tx_head))
        {
            if (!bridge->tx_head_pos)
            {
                bridge->tx_head[0] = size;
                bridge->tx_head[1] = size >> 8;
                bridge->tx_head[2] = size >> 16;
                bridge->tx_head[3] = size >> 24;
            }
            ret = send( sock, bridge->tx_head + bridge->tx_head_pos, sizeof(bridge->tx_head) - bridge->tx_head_pos,
                        MSG_NOSIGNAL | MSG_DONTWAIT );
            if (ret <= 0) break;
            bridge->tx_head_pos += ret;
            if (bridge->tx_head_pos < sizeof(bridge->tx_head)) break;
        }
        while (message->read_pos < size)
        {
            ret = send( sock, (const char *)message->iosb->in_data + message->read_pos, size - message->read_pos,
                        MSG_NOSIGNAL | MSG_DONTWAIT );
            if (ret <= 0) break;
            message->read_pos += ret;
        }
        if (message->read_pos < size) break;
        bridge->tx_head_pos = 0;
        wake_message( message, size );
        free_message( message );
    }
    ignore_reselect = saved_ignore;
    pipe_bridge_update_events( end );
}

/* deliver one message from the other server to the bridge's peer */
static void pipe_bridge_deliver( struct pipe_bridge *bridge )
{
    struct pipe_end *peer = bridge->pipe_end.connection;
    struct iosb *iosb;

    if (peer && (iosb = create_data_iosb( bridge->rx_data, bridge->rx_size )))
    {
        bridge->rx_data = NULL;  /* the iosb owns it */
        if (queue_message( peer, iosb )) reselect_read_queue( peer, 0 );
        release_object( iosb );
    }
    free( bridge->rx_data );
    bridge->rx_data = NULL;
    bridge->rx_head_pos = 0;
    bridge->rx_size = bridge->rx_pos = 0;
}

static void pipe_bridge_poll_event( struct fd *fd, int event )
{
    struct pipe_bridge *bridge = get_fd_user( fd );
    struct pipe_end *end = &bridge->pipe_end;
    int sock = get_unix_fd( fd );
    ssize_t ret;

    if (event & POLLOUT) pipe_bridge_flush( end );
    while ((event & POLLIN) && bridge->alive && end->connection &&
           pipe_end_get_avail( end->connection ) < PIPE_BRIDGE_MAX_QUEUED)
    {
        if (bridge->rx_head_pos < sizeof(bridge->rx_head))
        {
            ret = recv( sock, bridge->rx_head + bridge->rx_head_pos, sizeof(bridge->rx_head) - bridge->rx_head_pos,
                        MSG_DONTWAIT );
            if (ret < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (ret <= 0) goto closed;
            bridge->rx_head_pos += ret;
            if (bridge->rx_head_pos < sizeof(bridge->rx_head)) continue;
            bridge->rx_size = bridge->rx_head[0] | (bridge->rx_head[1] << 8) | (bridge->rx_head[2] << 16) |
                              ((data_size_t)bridge->rx_head[3] << 24);
            if (bridge->rx_size > PIPE_BRIDGE_MAX_MESSAGE) goto closed;
            bridge->rx_pos = 0;
            if (!(bridge->rx_data = malloc( bridge->rx_size ? bridge->rx_size : 1 ))) goto closed;
        }
        if (bridge->rx_pos < bridge->rx_size)
        {
            ret = recv( sock, bridge->rx_data + bridge->rx_pos, bridge->rx_size - bridge->rx_pos, MSG_DONTWAIT );
            if (ret < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (ret <= 0) goto closed;
            bridge->rx_pos += ret;
            if (bridge->rx_pos < bridge->rx_size) continue;
        }
        pipe_bridge_deliver( bridge );
    }
    if (!(event & (POLLERR | POLLHUP)) && bridge->alive && end->connection)
    {
        pipe_bridge_update_events( end );
        return;
    }
closed:
    /* the other server closed the connection, or it broke the rules: end it on this side too (this can free the
     * bridge) */
    if (bridge->alive) pipe_end_disconnect( end, STATUS_PIPE_BROKEN );
}

/* a bridge over sock (which it owns from now on, also on failure); it holds its own reference */
static struct pipe_bridge *create_pipe_bridge( struct named_pipe *pipe, int sock, data_size_t buffer_size )
{
    struct pipe_bridge *bridge;

    if (!(bridge = alloc_object( &pipe_bridge_ops )))
    {
        close( sock );
        return NULL;
    }
    init_pipe_end( &bridge->pipe_end, pipe, 0, buffer_size );
    list_add_tail( &bridges, &bridge->entry );
    bridge->pipe_end.state = FILE_PIPE_CONNECTED_STATE;
    bridge->alive = 1;
    bridge->peer_uid = (uid_t)-1;
    bridge->tx_head_pos = bridge->rx_head_pos = 0;
    bridge->rx_data = NULL;
    bridge->rx_size = bridge->rx_pos = 0;
    if (!(bridge->pipe_end.fd = create_anonymous_fd( &pipe_bridge_fd_ops, sock, &bridge->pipe_end.obj, 0 )))
    {
        bridge->alive = 0;
        release_object( bridge );
        return NULL;
    }
    return bridge;
}

/* a user's server: a name under WexSession0\ names a pipe of session 0. The object stands for it in the open */
static struct object *pipe_bridge_lookup( struct unicode_str *name )
{
    static const WCHAR prefix[] = {'W','e','x','S','e','s','s','i','o','n','0','\\'};
    const data_size_t prefix_len = sizeof(prefix);
    struct named_pipe_init_data data = { .maxinstances = 1, .outsize = 65536, .insize = 65536,
                                         .sharing = FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         .flags = NAMED_PIPE_MESSAGE_STREAM_WRITE };
    struct named_pipe *pipe;

    if (name->len <= prefix_len || memicmp_strW( name->str, prefix, prefix_len )) return NULL;
    if (!(pipe = alloc_object( &named_pipe_ops ))) return NULL;
    named_pipe_init( &pipe->obj, &data );
    if (!(pipe->s0_name = memdup( (const char *)name->str + prefix_len, name->len - prefix_len )))
    {
        release_object( pipe );
        return NULL;
    }
    pipe->s0_len = name->len - prefix_len;
    return &pipe->obj;
}

/* a user's server: open a pipe of session 0 for the current thread */
static struct object *pipe_bridge_open_remote( struct named_pipe *pipe, unsigned int options )
{
    struct token *token = current->token ? current->token : current->process->token;
    struct pipe_bridge *bridge;
    struct pipe_end *client;
    int sock;

    if (!token)
    {
        set_error( STATUS_ACCESS_DENIED );
        return NULL;
    }
    if ((sock = session0_open_pipe( token, pipe->s0_name, pipe->s0_len )) == -1) return NULL;
    if (!(bridge = create_pipe_bridge( pipe, sock, pipe->insize ))) return NULL;
    if (!(client = create_pipe_client( pipe, pipe->outsize, options )))
    {
        pipe_end_disconnect( &bridge->pipe_end, STATUS_PIPE_BROKEN );  /* frees the bridge */
        return NULL;
    }
    client->connection = &bridge->pipe_end;
    bridge->pipe_end.connection = client;
    pipe_bridge_update_events( &bridge->pipe_end );
    return &client->obj;
}

/* session 0: the pipes that a session may open here */
static int pipe_bridge_allowed( const WCHAR *name, data_size_t len )
{
    static const WCHAR svcctl[] = {'s','v','c','c','t','l'};

    return len == sizeof(svcctl) && !memicmp_strW( name, svcctl, len );
}

/* session 0: connect a session's opener (described by token) to a listening instance of the named pipe, over sock
 * (owned from now on); the status for the session's server */
unsigned int pipe_bridge_accept( struct token *token, const WCHAR *name, data_size_t len, int sock, uid_t uid )
{
    static const WCHAR device[] = {'\\','D','e','v','i','c','e','\\','N','a','m','e','d','P','i','p','e','\\'};
    struct object_params params = { .ops = &named_pipe_ops, .attr = OBJ_CASE_INSENSITIVE };
    unsigned int access = FILE_GENERIC_READ | FILE_GENERIC_WRITE, count = 0;
    struct pipe_bridge *bridge, *other;
    struct pipe_server *server;
    struct named_pipe *pipe;
    WCHAR *path;

    if (!len || len % sizeof(WCHAR) || !pipe_bridge_allowed( name, len ))
    {
        close( sock );
        return STATUS_ACCESS_DENIED;
    }
    LIST_FOR_EACH_ENTRY( other, &bridges, struct pipe_bridge, entry )
        if (other->peer_uid == uid) count++;
    if (count >= PIPE_BRIDGE_MAX_PER_USER)
    {
        close( sock );
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!(path = malloc( sizeof(device) + len )))
    {
        close( sock );
        return STATUS_NO_MEMORY;
    }
    memcpy( path, device, sizeof(device) );
    memcpy( (char *)path + sizeof(device), name, len );
    params.name.str = path;
    params.name.len = sizeof(device) + len;
    clear_error();
    pipe = open_named_object( &params );
    free( path );
    if (!pipe)
    {
        close( sock );
        return get_error() ? get_error() : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (!check_object_access( token, &pipe->obj, &access ) ||
        !(pipe->sharing & FILE_SHARE_READ) || !(pipe->sharing & FILE_SHARE_WRITE))
    {
        release_object( pipe );
        close( sock );
        return STATUS_ACCESS_DENIED;
    }
    if (list_empty( &pipe->listeners ))
    {
        release_object( pipe );
        close( sock );
        return STATUS_PIPE_NOT_AVAILABLE;
    }
    if (!(bridge = create_pipe_bridge( pipe, sock, pipe->outsize )))
    {
        release_object( pipe );
        return STATUS_NO_MEMORY;
    }
    bridge->peer_uid = uid;
    bridge->pipe_end.client_token = (struct token *)grab_object( token );
    server = LIST_ENTRY( list_head( &pipe->listeners ), struct pipe_server, entry );
    async_wake_up( &server->listen_q, STATUS_SUCCESS );
    server->pipe_end.state = FILE_PIPE_CONNECTED_STATE;
    server->pipe_end.connection = &bridge->pipe_end;
    bridge->pipe_end.connection = &server->pipe_end;
    bridge->pipe_end.server_pid = server->pipe_end.server_pid;
    list_remove( &server->entry );
    release_object( pipe );
    pipe_bridge_update_events( &bridge->pipe_end );
    return STATUS_SUCCESS;
}
