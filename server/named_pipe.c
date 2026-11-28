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
#include "wex_e1lab.h"
#define WEX_CALLER_CONSTANTS_ONLY
#include "wine/wex_caller.h"
#define WEX_ACCOUNTS_CONSTANTS_ONLY
#include "wine/wex_accounts.h"

struct named_pipe;

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
    unsigned int         client_lease; /* a bridge: the lease of client_pid's block (R6), 0 for a local client */
    unsigned int         client_session; /* its session (a bridge: the session of the server that opened it), or: */
#define PIPE_SESSION_UNKNOWN (~0u)         /* not known (no client yet, no token, a bridge without a session link) */
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
    char                *acc_msg;    /* H1 (A3): a reserved accounts instance's last client message, read whole */
    data_size_t          acc_len;
    int                  acc_valid;  /* set by such a read, cleared by any other read and by accounts_forward */
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
    int                  reserved;   /* WineExplorer H1: an instance of a reserved name, made by LocalSystem in session 0 */
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

static int creating_pipe;  /* B17: create_named_pipe is looking the name up (an lrpc\ name stays local) */

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
        /* WineExplorer: the file mapping, as on Windows, so that a generic right in a pipe's descriptor (GA for
         * SYSTEM) grants the file rights a client asks for; session 0 checks a bridged client against it */
        FILE_GENERIC_READ,
        FILE_GENERIC_WRITE | FILE_CREATE_PIPE_INSTANCE,
        FILE_GENERIC_EXECUTE,
        FILE_ALL_ACCESS
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
static void pipe_end_set_file_info( struct fd *fd, struct async *async, unsigned int info_class );

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
    .set_file_info   = pipe_end_set_file_info,
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
    .set_file_info   = pipe_end_set_file_info,
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
    .set_file_info = default_fd_set_file_info,
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
    .set_file_info = default_fd_set_file_info,
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
    e1lab_pipe_binding_free( pipe_end->binding );
    pipe_end->binding = NULL;
#endif
    clear_pipe_writer( &pipe_end->consumed_writer );
    pipe_end->consumed_valid = 0;
    pipe_end->binding_reads = 0;
    free( pipe_end->acc_msg );
    pipe_end->acc_msg = NULL;
    pipe_end->acc_len = 0;
    pipe_end->acc_valid = 0;
    if (pipe_end->client_primary) release_object( pipe_end->client_primary );
    if (pipe_end->client_process) release_object( pipe_end->client_process );
    pipe_end->client_primary = NULL;
    pipe_end->client_process = NULL;
    /* nothing of the last client is reported once it is gone (a server end keeps them until the next connect else) */
    pipe_end->client_pid = 0;
    pipe_end->client_lease = 0;
    pipe_end->client_session = PIPE_SESSION_UNKNOWN;

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
    e1lab_pipe_binding_free( pipe_end->binding );
    pipe_end->binding = NULL;
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

static void pipe_end_set_file_info( struct fd *fd, struct async *async, unsigned int info_class )
{
    struct pipe_end *pipe_end = get_fd_user( fd );

    switch (info_class)
    {
    case FilePipeInformation:
        {
            const FILE_PIPE_INFORMATION *info = get_req_data();

            if (get_req_data_size() < sizeof(*info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
            }
            else if ((info->CompletionMode | info->ReadMode) & ~1)
            {
                set_error( STATUS_INVALID_PARAMETER );
            }
            else if (!pipe_end->pipe)
            {
                set_error( STATUS_PIPE_DISCONNECTED );
            }
            else if (info->ReadMode && !pipe_end->pipe->message_mode)
            {
                set_error( STATUS_INVALID_PARAMETER );
            }
            else
            {
                pipe_end->flags = (info->CompletionMode ? NAMED_PIPE_NONBLOCKING_MODE    : 0) |
                                  (info->ReadMode       ? NAMED_PIPE_MESSAGE_STREAM_READ : 0);
            }
        }
        break;

    default:
        default_fd_set_file_info( fd, async, info_class );
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

/* H1 (the reviewer's A3): a reserved accounts instance keeps the client message its service last read, when that read
 * took one whole message in message mode; any other read forgets it. accounts_forward sends only those bytes. */
static void accounts_record_read( struct pipe_end *pipe_end, struct pipe_message *message, unsigned int status,
                                  data_size_t size )
{
    if (pipe_end->obj.ops != &pipe_server_ops || !((struct pipe_server *)pipe_end)->reserved) return;
    free( pipe_end->acc_msg );
    pipe_end->acc_msg = NULL;
    pipe_end->acc_len = 0;
    pipe_end->acc_valid = 0;
    if (status != STATUS_SUCCESS || !(pipe_end->flags & NAMED_PIPE_MESSAGE_STREAM_READ) || message->read_pos ||
        size != message->iosb->in_size || size > WEX_ACCOUNTS_MAX_FRAME) return;
    if (size && !(pipe_end->acc_msg = memdup( message->iosb->in_data, size ))) return;
    pipe_end->acc_len = size;
    pipe_end->acc_valid = 1;
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
        accounts_record_read( pipe_end, message, status, out_size );
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
            if (!write_pos)
            {
                retain_consumed_writer( pipe_end, message, status, out_size );
                accounts_record_read( pipe_end, message, status, out_size );
            }
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
    process_id_t client_pid = pipe_end->client_pid;
    void *value;

    if (attr_size == sizeof("ClientProcessId") && !memcmp( attr, "ClientProcessId", attr_size ))
    {
        /* a bridged opener's id, once its block went to another server, may name another process: 0 then */
        if (pipe_end->client_lease && !session0_id_lease_current( client_pid, pipe_end->client_lease ))
            client_pid = 0;
        value = &client_pid;
        value_size = sizeof(client_pid);
    }
    else if (attr_size == sizeof("ServerProcessId") && !memcmp( attr, "ServerProcessId", attr_size ))
    {
        value = &pipe_end->server_pid;
        value_size = sizeof(pipe_end->server_pid);
    }
    else if (attr_size == sizeof("ClientSessionId") && !memcmp( attr, "ClientSessionId", attr_size ))
    {
        /* an unknown session fails: 0 would be session 0's, the answer a service trusts most */
        if (pipe_end->client_session == PIPE_SESSION_UNKNOWN)
        {
            set_error( STATUS_NOT_FOUND );
            return;
        }
        value = &pipe_end->client_session;
        value_size = sizeof(pipe_end->client_session);
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
    pipe_end->client_session = PIPE_SESSION_UNKNOWN;
    pipe_end->client_lease = 0;
    /* also on the bridge's user side, which sets neither: FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE reads them */
    pipe_end->client_pid = 0;
    pipe_end->server_pid = 0;
    memset( &pipe_end->consumed_writer, 0, sizeof(pipe_end->consumed_writer) );
    pipe_end->binding_reads = 0;
    pipe_end->consumed_valid = 0;
    pipe_end->acc_msg = NULL;
    pipe_end->acc_len = 0;
    pipe_end->acc_valid = 0;
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
    server->reserved = 0;
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
    client->client_session = current->process->token ? token_get_session_id( current->process->token ) : PIPE_SESSION_UNKNOWN;
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
        server->pipe_end.client_lease = 0;  /* a local client (the instance may have served a bridge before) */
        server->pipe_end.client_session = client->client_session;
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
            if (pipe->s0_name)
            {
                /* T5.2: a pipe of session 0. A missing one is not found, as on Windows, else an RPC client's
                 * connect loop (open fails, wait succeeds) spins for ever; whether an instance is free shows at
                 * the open */
                struct token *token = current->token ? current->token : current->process->token;
                unsigned int status = session0_wait_pipe( token, pipe->s0_name, pipe->s0_len );

                if (status) set_error( status );
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


/* WineExplorer H1: names that only LocalSystem may serve in session 0 (the accounts service's: plain and as the
 * bridged name a session 0 process would use); 1 when the pipe has one */
static const struct sid accounts_service_sid =
    { SID_REVISION, SECURITY_SERVICE_ID_RID_COUNT, SECURITY_NT_AUTHORITY,
      { SECURITY_SERVICE_ID_BASE_RID, WEX_ACCOUNTS_SERVICE_RIDS } };

static int pipe_name_reserved( struct named_pipe *pipe )
{
    static const WCHAR accounts[] = {'a','c','c','o','u','n','t','s'};
    static const WCHAR bridged[] = {'W','e','x','S','e','s','s','i','o','n','0','\\','a','c','c','o','u','n','t','s'};
    data_size_t len;
    const WCHAR *name;

    if (!session0_server || !(name = get_object_name( &pipe->obj, &len ))) return 0;
    return (len == sizeof(accounts) && !memicmp_strW( name, accounts, len )) ||
           (len == sizeof(bridged) && !memicmp_strW( name, bridged, len ));
}

DECL_HANDLER(create_named_pipe)
{
    struct named_pipe *pipe;
    struct pipe_server *server;
    int reserved;
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

    creating_pipe = 1;  /* B17: an lrpc\ name is this server's own when created here */
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
    creating_pipe = 0;

    if (params.root) release_object( params.root );
    if (!pipe) return;
    if (pipe->s0_name)  /* T5.2: session 0's pipes are served in session 0 only */
    {
        release_object( pipe );
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    /* H1: in session 0 only LocalSystem (its primary token, not impersonating) makes instances of a reserved name;
     * they are marked, and accounts_forward serves only marked instances */
    /* the accounts service only (B114): LocalSystem, not impersonating, with NT SERVICE\wexaccounts */
    if ((reserved = pipe_name_reserved( pipe )) &&
        (current->token || !current->process->token ||
         token_service_account( current->process->token ) != SERVICE_ACCOUNT_SYSTEM ||
         !token_sid_present( current->process->token, &accounts_service_sid, FALSE )))
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
        server->reserved = reserved;
        reply->handle = alloc_handle( current->process, server, req->access, params.attr );
        pipe->instances++;
        release_object( server );
    }

    release_object( pipe );
}

/* WineExplorer private: opener and the single complete request message consumed by this server end. */
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
    if (session0_server)  /* E1 R9: session 0's broker B0 endorses its S6 channel (wex_e1lab_pipe.c) */
    {
        e1lab_pipe_session0_endorse( get_req_data(), get_req_data_size() );
        return;
    }
#endif
    /* Keep the same system-role / SeTcbPrivilege gate as create_linked_token. */
    if (!elevation_enforced() || !sysrun_process_is_tcb_system( current->process ))
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
        if (get_req_data_size() >= 4 && !memcmp( get_req_data(), "E1R8", 4 ))
        {
            /* the lab's R8 operations on this server end's binding (wex_e1lab_pipe.c) */
            if (!e1lab_pipe_e1r8_op( &server->binding, client->client_process, client->client_primary,
                                     writer->process, writer->desktop, get_req_data(), get_req_data_size() ))
                goto done;
        }
        else
#endif
        if (!sysrun_e1s6_endorse( get_req_data(), get_req_data_size() )) goto done;
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
    int             draining;      /* its peer closed: what it wrote is still being sent */
    struct timeout_user *drain_timeout;
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

/* stop polling and drop the bridge's own reference (the last thing its caller does) */
static void pipe_bridge_finish( struct pipe_bridge *bridge )
{
    if (bridge->drain_timeout) remove_timeout_user( bridge->drain_timeout );
    bridge->drain_timeout = NULL;
    bridge->alive = 0;
    if (bridge->pipe_end.fd) set_fd_events( bridge->pipe_end.fd, -1 );
    release_object( bridge );
}

static void pipe_bridge_drain_timeout( void *private )
{
    struct pipe_bridge *bridge = private;

    bridge->drain_timeout = NULL;
    pipe_bridge_finish( bridge );
}

/* the connection ended. What the peer wrote before it closed, and was told was written, is sent before the socket
 * closes (T5 review); a socket that takes nothing for 10 s ends it anyway */
static void pipe_bridge_closed( struct pipe_end *end )
{
    struct pipe_bridge *bridge = (struct pipe_bridge *)end;

    if (!bridge->alive || bridge->draining) return;
    pipe_bridge_flush( end );
    if (list_empty( &end->message_queue ) || !end->fd)
    {
        pipe_bridge_finish( bridge );
        return;
    }
    bridge->draining = 1;
    set_fd_events( end->fd, POLLOUT );
    bridge->drain_timeout = add_timeout_user( -10 * TICKS_PER_SEC, pipe_bridge_drain_timeout, bridge );
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

    if (bridge->draining)
    {
        if (event & POLLOUT) pipe_bridge_flush( end );
        if ((event & (POLLERR | POLLHUP)) || list_empty( &end->message_queue )) pipe_bridge_finish( bridge );
        else set_fd_events( fd, POLLOUT );
        return;
    }
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
    bridge->draining = 0;
    bridge->drain_timeout = NULL;
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

static const WCHAR lrpc_prefix[] = {'l','r','p','c','\\'};

static int name_starts( const WCHAR *name, data_size_t len, const WCHAR *start, data_size_t start_len )
{
    return len >= start_len && !memicmp_strW( name, start, start_len );
}

/* a name under lrpc\ (rpcrt4's ncalrpc endpoints) that a service names itself. Not the endpoints every session's
 * own rpcss has (epmapper, irpcss, irot), nor Wine's lsass (lsasspirpc: it reads and writes its callers' memory by
 * process id, so it serves its own server's processes only; bridged, it made secur32's DllMain wait for ever), nor
 * names made from a process id or an OXID (LRPC<pid>.<n>, COM's \pipe\OLE_<oxid>): process ids are per server, so
 * the same name in session 0 is another process. */
static int is_lrpc_pipe_name( const WCHAR *name, data_size_t len )
{
    static const WCHAR pipe_dir[] = {'\\','p','i','p','e','\\'};
    static const WCHAR epmapper[] = {'e','p','m','a','p','p','e','r'};
    static const WCHAR irpcss[] = {'i','r','p','c','s','s'};
    static const WCHAR irot[] = {'i','r','o','t'};
    static const WCHAR lsass[] = {'l','s','a','s','s','p','i','r','p','c'};
    static const WCHAR ole[] = {'O','L','E','_'};
    static const WCHAR lrpc[] = {'L','R','P','C'};

    if (len <= sizeof(lrpc_prefix) || !name_starts( name, len, lrpc_prefix, sizeof(lrpc_prefix) )) return 0;
    name += ARRAY_SIZE(lrpc_prefix);
    len -= sizeof(lrpc_prefix);
    if (name_starts( name, len, pipe_dir, sizeof(pipe_dir) ))
    {
        name += ARRAY_SIZE(pipe_dir);
        len -= sizeof(pipe_dir);
    }
    if (!len) return 0;
    if ((len == sizeof(epmapper) && name_starts( name, len, epmapper, len )) ||
        (len == sizeof(irpcss) && name_starts( name, len, irpcss, len )) ||
        (len == sizeof(irot) && name_starts( name, len, irot, len )) ||
        (len == sizeof(lsass) && name_starts( name, len, lsass, len )))
        return 0;
    return !name_starts( name, len, ole, sizeof(ole) ) && !name_starts( name, len, lrpc, sizeof(lrpc) );
}

/* a user's server: a name under WexSession0\ names a pipe of session 0. So does an ncalrpc endpoint (lrpc\...) that
 * this server does not have: on Windows one namespace serves every session, and a service in session 0 offers its
 * RPC interface to the users' programs that way (B17). The object stands for the pipe in the open. */
static struct object *pipe_bridge_lookup( struct unicode_str *name )
{
    static const WCHAR prefix[] = {'W','e','x','S','e','s','s','i','o','n','0','\\'};
    data_size_t prefix_len = sizeof(prefix);
    struct named_pipe_init_data data = { .maxinstances = 1, .outsize = 65536, .insize = 65536,
                                         .sharing = FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         .flags = NAMED_PIPE_MESSAGE_STREAM_WRITE };
    struct named_pipe *pipe;

    if (is_lrpc_pipe_name( name->str, name->len ))
    {
        if (creating_pipe) return NULL;  /* a server here makes its own endpoint */
        prefix_len = 0;  /* the same name there */
    }
    else if (name->len <= prefix_len || memicmp_strW( name->str, prefix, prefix_len )) return NULL;
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

/* session 0: the pipes that a session may open here (1), those only a session with a session link may (2), and those
 * only a session's system role may, over its session link (3) */
static int pipe_bridge_allowed( const WCHAR *name, data_size_t len )
{
    static const WCHAR svcctl[] = {'s','v','c','c','t','l'};
    static const WCHAR broker[] = {'w','e','x','e','l','e','v','a','t','e','.','r','9'};  /* E1 R9's B0 */
    static const WCHAR eventlog[] = {'e','v','e','n','t','l','o','g'};  /* the EventLog service (L1) */
    static const WCHAR accounts[] = {'a','c','c','o','u','n','t','s'};  /* the accounts service (H1) */

    if (len == sizeof(svcctl) && !memicmp_strW( name, svcctl, len )) return 1;
    if (len == sizeof(eventlog) && !memicmp_strW( name, eventlog, len )) return 2;  /* link_bound_token needs the link */
    if (len == sizeof(accounts) && !memicmp_strW( name, accounts, len )) return 2;
    /* B132: the session's agent only (a session's system role, WEX_CALLER_SESSION_SYSTEM): refused here, before it
     * takes one of B0's instances, so that no other caller can hold them (r9_idle_clients_do_not_hold_broker) */
    if (len == sizeof(broker) && !memicmp_strW( name, broker, len )) return 3;
    /* a service's ncalrpc endpoint: the pipe's descriptor decides, and the service checks its callers itself */
    if (is_lrpc_pipe_name( name, len )) return 1;
    return 0;
}

/* session 0: connect a session's opener (described by token) to a listening instance of the named pipe, over sock
 * (owned from now on); the status for the session's server */
/* session 0: the pipe NAME that a session's program may reach (a name on the list, or an ncalrpc endpoint of a
 * linked session's server); NULL with *status set */
static struct named_pipe *pipe_bridge_find( const WCHAR *name, data_size_t len, uid_t uid, int session,
                                            unsigned int *status )
{
    static const WCHAR device[] = {'\\','D','e','v','i','c','e','\\','N','a','m','e','d','P','i','p','e','\\'};
    struct object_params params = { .ops = &named_pipe_ops, .attr = OBJ_CASE_INSENSITIVE };
    struct named_pipe *pipe;
    WCHAR *path;
    int allowed = 0;

    if (!len || len % sizeof(WCHAR) || !(allowed = pipe_bridge_allowed( name, len )) ||
        (allowed >= 2 && session < 0))
    {
        fprintf( stderr, "session0: pipe open refused for uid %u: %s\n", (unsigned int)uid,
                 allowed >= 2 ? "the server has no session link" : "the name is not on the list" );
        *status = STATUS_ACCESS_DENIED;
        return NULL;
    }
    if (!(path = malloc( sizeof(device) + len )))
    {
        *status = STATUS_NO_MEMORY;
        return NULL;
    }
    memcpy( path, device, sizeof(device) );
    memcpy( (char *)path + sizeof(device), name, len );
    params.name.str = path;
    params.name.len = sizeof(device) + len;
    clear_error();
    pipe = open_named_object( &params );
    free( path );
    if (!pipe) *status = get_error() ? get_error() : STATUS_OBJECT_NAME_NOT_FOUND;
    return pipe;
}

/* session 0: whether a session's program waiting for the pipe NAME finds it here (no instance is taken) */
unsigned int pipe_bridge_query( const WCHAR *name, data_size_t len, uid_t uid, int session )
{
    unsigned int status = STATUS_SUCCESS;
    struct named_pipe *pipe = pipe_bridge_find( name, len, uid, session, &status );

    if (pipe) release_object( pipe );
    return status;
}

unsigned int pipe_bridge_accept( struct token *token, const WCHAR *name, data_size_t len, int sock, uid_t uid,
                                 int session, unsigned int client_pid, unsigned int lease )
{
    unsigned int access = FILE_GENERIC_READ | FILE_GENERIC_WRITE, count = 0, status = STATUS_SUCCESS;
    struct pipe_bridge *bridge, *other;
    struct pipe_server *server;
    struct named_pipe *pipe;

    LIST_FOR_EACH_ENTRY( other, &bridges, struct pipe_bridge, entry )
        if (other->peer_uid == uid) count++;
    if (count >= PIPE_BRIDGE_MAX_PER_USER)
    {
        close( sock );
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!(pipe = pipe_bridge_find( name, len, uid, session, &status )))
    {
        close( sock );
        return status;
    }
    /* a pipe for a session's system role only: what session 0 mapped (WEX_CALLER_SESSION_SYSTEM). Development mode
     * maps nothing and trusts its own uid (T1 runs every session as one account and opens the name from a program as
     * a carrier for the bridge's checks): there the name stays a rule 2 pipe, as before B132 */
    if (pipe_bridge_allowed( name, len ) == 3 && !session0_dev_mode() &&
        token_caller_class( token ) != WEX_CALLER_SESSION_SYSTEM)
    {
        fprintf( stderr, "session0: pipe open refused for uid %u: only a session's system role opens this pipe\n",
                 (unsigned int)uid );
        release_object( pipe );
        close( sock );
        return STATUS_ACCESS_DENIED;
    }
    if (!check_object_access( token, &pipe->obj, &access ) ||
        !(pipe->sharing & FILE_SHARE_READ) || !(pipe->sharing & FILE_SHARE_WRITE))
    {
        fprintf( stderr, "session0: pipe open refused for uid %u: the pipe's descriptor or sharing (%x)\n",
                 (unsigned int)uid, pipe->sharing );
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
    /* the opener is a process of another server. Its id comes from a block session 0 leased to that server (R6), so
       it names no process here and is unique on the machine; 0 when the opener's server gave none that session 0
       could check (also no stale id of an earlier client) */
    bridge->pipe_end.client_pid = client_pid;
    server->pipe_end.client_pid = client_pid;
    bridge->pipe_end.client_lease = client_pid ? lease : 0;
    server->pipe_end.client_lease = bridge->pipe_end.client_lease;
    bridge->pipe_end.client_session = session > 0 ? session : PIPE_SESSION_UNKNOWN;
    server->pipe_end.client_session = bridge->pipe_end.client_session;  /* R9: the session of the opener's server */
    list_remove( &server->entry );
    release_object( pipe );
    pipe_bridge_update_events( &bridge->pipe_end );
    return STATUS_SUCCESS;
}

/* WineExplorer (H3): the class of a service's caller: the client of a pipe connection (the server end's), or a token */
DECL_HANDLER(get_caller_class)
{
    if (req->is_pipe)
    {
        struct pipe_server *server = (struct pipe_server *)get_handle_obj( current->process, req->handle, 0,
                                                                           &pipe_server_ops );
        struct pipe_end *client;

        if (!server) return;
        if (!(client = server->pipe_end.connection)) set_error( STATUS_PIPE_DISCONNECTED );
        /* the token the client opened with (a bridge's: the link-bound description) */
        else reply->caller_class = token_caller_class( client->client_token ? client->client_token : client->client_primary );
        release_object( server );
    }
    else
    {
        struct token *token = get_token_obj( current->process, req->handle, TOKEN_QUERY );

        if (!token) return;
        reply->caller_class = token_caller_class( token );
        release_object( token );
    }
}

/* WineExplorer (H1): the accounts service forwards one request of a client of its reserved pipe to the root helper.
 * Only an instance made under the reservation, connected to a session's program through the bridge with a link-bound
 * description; the verdict and the uid come from that connection, never from the service. The body is the client's
 * own bytes: a part of the message the service last read whole, and each message is forwarded once (A3) */
DECL_HANDLER(accounts_forward)
{
    struct pipe_server *server = (struct pipe_server *)get_handle_obj( current->process, req->pipe, 0,
                                                                       &pipe_server_ops );
    struct pipe_end *client;
    struct object *call;
    unsigned int verdict;

    if (!server) return;
    client = server->pipe_end.connection;
    if (!server->reserved || !client || client->obj.ops != &pipe_bridge_ops || !client->client_token ||
        !token_is_link_bound( client->client_token ) || ((struct pipe_bridge *)client)->peer_uid == (uid_t)-1)
    {
        release_object( server );
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (!server->pipe_end.acc_valid || req->offset > server->pipe_end.acc_len ||
        req->length > server->pipe_end.acc_len - req->offset)
    {
        release_object( server );
        set_error( server->pipe_end.acc_valid ? STATUS_INVALID_PARAMETER : STATUS_INVALID_DEVICE_STATE );
        return;
    }
    server->pipe_end.acc_valid = 0;  /* one forward per message */
    switch (token_caller_class( client->client_token ))
    {
    case WEX_CALLER_ELEVATED_ADMIN: verdict = WEX_ACCOUNTS_VERDICT_ELEVATED; break;
    case WEX_CALLER_USER:
    case WEX_CALLER_SESSION_SYSTEM: verdict = WEX_ACCOUNTS_VERDICT_USER; break;  /* as before that class existed */
    default:                        verdict = WEX_ACCOUNTS_VERDICT_NONE; break;
    }
    if ((call = accounts_call_start( req->op, verdict, ((struct pipe_bridge *)client)->peer_uid,
                                     req->length ? server->pipe_end.acc_msg + req->offset : "", req->length )))
    {
        reply->call = alloc_handle( current->process, call, SYNCHRONIZE, 0 );
        release_object( call );
    }
    release_object( server );
}
