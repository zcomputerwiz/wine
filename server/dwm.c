/*
 * WineExplorer Desktop Environment - dwm.c
 *
 * Per-desktop DWM broker control channel (Phase P1, Slice 1 of docs/DWM_ARCHITECTURE.md).
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"
#include "wine/dwm_protocol.h"

#include "object.h"
#include "file.h"
#include "handle.h"
#include "process.h"
#include "thread.h"
#include "request.h"
#include "user.h"
#include "security.h"

enum dwm_msg_state
{
    DWM_MSG_QUEUED   = 1, /* queued on endpoint->broker_queue awaiting broker dwm_recv */
    DWM_MSG_INFLIGHT = 2, /* delivered to broker via dwm_recv, awaiting broker dwm_reply */
    DWM_MSG_REPLIED  = 3, /* answered by broker via dwm_reply, awaiting client dwm_recv */
    DWM_MSG_ABORTED  = 4, /* aborted by broker disconnect or desktop revoke */
};

struct dwm_channel;

struct dwm_message
{
    struct list         broker_entry;   /* entry in endpoint->broker_queue or endpoint->inflight_msgs */
    struct list         client_entry;   /* entry in client->messages */
    struct dwm_channel *client;         /* originating client channel (NULL if client closed) */
    unsigned int        request_id;     /* server-assigned request id */
    process_id_t        sender_pid;     /* server-stamped sender process id */
    thread_id_t         sender_tid;     /* server-stamped sender thread id */
    unsigned int        desktop_id;     /* server-stamped desktop cookie */
    unsigned int        reply_status;   /* NTSTATUS set by broker dwm_reply */
    enum dwm_msg_state  state;          /* message lifecycle state */
    data_size_t         size;           /* byte length of data (sizeof(struct dwm_msg_header) + payload) */
    unsigned char      *data;           /* message envelope + payload buffer */
};

struct dwm_endpoint
{
    struct list         entry;           /* entry in all_endpoints */
    unsigned int        refcount;        /* reference count (desktop + active channels) */
    struct desktop     *desktop;         /* owning desktop (cleared on dwm_revoke_desktop) */
    struct winstation  *winstation;      /* owning window station pointer (for identity comparison) */
    unsigned int        desktop_id;      /* server-assigned non-zero desktop cookie */
    struct dwm_channel *broker;          /* registered broker channel, or NULL */
    struct list         clients;         /* connected client channels (struct dwm_channel.desktop_entry) */
    struct list         broker_queue;    /* pending messages awaiting broker dwm_recv */
    struct list         inflight_msgs;   /* messages popped by broker awaiting dwm_reply */
    unsigned int        broker_queued;   /* count of messages in broker_queue */
    unsigned int        next_request_id; /* monotonic server-assigned request id counter */
    unsigned __int64    pending_cap;     /* one-use capability token (0 if none) */
    process_id_t        cap_owner_pid;   /* pid of desktop->top_window owner that minted pending_cap */
    int                 revoked;         /* non-zero once desktop is destroyed */
};

struct dwm_channel
{
    struct object        obj;            /* server waitable object header */
    struct object       *sync;           /* manual-reset internal sync object */
    struct dwm_endpoint *endpoint;       /* refcounted desktop endpoint */
    struct desktop      *desktop;        /* desktop pointer bound at connect/register time */
    struct winstation   *winstation;     /* window station pointer bound at connect/register time */
    struct list          desktop_entry;  /* entry in endpoint->clients (for client channels) */
    struct list          proc_entry;     /* entry in all_channels list */
    struct list          messages;       /* client's active/completed messages (struct dwm_message.client_entry) */
    process_id_t         owner_pid;      /* process id that created this channel */
    unsigned __int64     connect_epoch;  /* desktop epoch at creation time */
    unsigned int         queued_count;   /* number of outstanding messages on this client channel */
    int                  is_broker;      /* 1 for broker channel, 0 for client channel */
    int                  disconnected;   /* 1 if broker unregistered/exited after channel creation */
    int                  revoked;        /* 1 if desktop was destroyed */
    int                  closed;         /* 1 once handle is closed or owning process died */
};

static struct list all_endpoints = LIST_INIT( all_endpoints );
static struct list all_channels = LIST_INIT( all_channels );
static unsigned int next_desktop_cookie = 1;
static unsigned __int64 next_cap_seq = 1;

static const WCHAR dwm_channel_type_name[] = {'D','w','m','C','h','a','n','n','e','l'};

static struct type_descr dwm_channel_type =
{
    { dwm_channel_type_name, sizeof(dwm_channel_type_name) },
    STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x3,
    {
        STANDARD_RIGHTS_READ | 0x1,
        STANDARD_RIGHTS_WRITE | 0x2,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x3
    },
};

static void dwm_channel_dump( struct object *obj, int verbose );
static struct object *dwm_channel_get_sync( struct object *obj );
static int dwm_channel_close_handle( struct object *obj, struct process *process, obj_handle_t handle );
static void dwm_channel_destroy( struct object *obj );

static const struct object_ops dwm_channel_ops =
{
    .size           = sizeof(struct dwm_channel),
    .type           = &dwm_channel_type,
    .dump           = dwm_channel_dump,
    .get_sync       = dwm_channel_get_sync,
    .close_handle   = dwm_channel_close_handle,
    .destroy        = dwm_channel_destroy,
};

static struct dwm_endpoint *grab_endpoint( struct dwm_endpoint *endpoint )
{
    assert( endpoint->refcount > 0 );
    endpoint->refcount++;
    return endpoint;
}

static void release_endpoint( struct dwm_endpoint *endpoint )
{
    assert( endpoint->refcount > 0 );
    if (!--endpoint->refcount)
    {
        assert( list_empty( &endpoint->clients ) );
        assert( list_empty( &endpoint->broker_queue ) );
        assert( list_empty( &endpoint->inflight_msgs ) );
        list_remove( &endpoint->entry );
        free( endpoint );
    }
}

static struct dwm_endpoint *get_desktop_endpoint( struct desktop *desktop )
{
    struct dwm_endpoint *endpoint = desktop->dwm_endpoint;

    if (endpoint) return endpoint;

    if (!(endpoint = mem_alloc( sizeof(*endpoint) ))) return NULL;
    list_add_tail( &all_endpoints, &endpoint->entry );
    endpoint->refcount        = 1;
    endpoint->desktop         = desktop;
    endpoint->winstation      = desktop->winstation;
    endpoint->desktop_id      = next_desktop_cookie++;
    if (!endpoint->desktop_id) endpoint->desktop_id = next_desktop_cookie++;
    endpoint->broker          = NULL;
    list_init( &endpoint->clients );
    list_init( &endpoint->broker_queue );
    list_init( &endpoint->inflight_msgs );
    endpoint->broker_queued   = 0;
    endpoint->next_request_id = 1;
    endpoint->pending_cap     = 0;
    endpoint->cap_owner_pid   = 0;
    endpoint->revoked         = 0;

    if (!desktop->dwm_epoch) desktop->dwm_epoch = 1;
    desktop->dwm_endpoint = endpoint;
    return endpoint;
}

static void free_dwm_message( struct dwm_message *msg )
{
    free( msg->data );
    free( msg );
}

static int client_has_ready_reply( const struct dwm_channel *client )
{
    const struct dwm_message *msg;

    LIST_FOR_EACH_ENTRY( msg, &client->messages, const struct dwm_message, client_entry )
    {
        if (msg->state == DWM_MSG_REPLIED || msg->state == DWM_MSG_ABORTED)
            return 1;
    }
    return 0;
}

static void update_client_sync( struct dwm_channel *client )
{
    if (!client->sync) return;
    if (client->revoked || client->disconnected || client->closed || client_has_ready_reply( client ))
        signal_sync( client->sync );
    else
        reset_sync( client->sync );
}

static void update_broker_sync( struct dwm_channel *broker )
{
    struct dwm_endpoint *endpoint = broker->endpoint;

    if (!broker->sync) return;
    if (broker->revoked || broker->disconnected || broker->closed ||
        (endpoint && (endpoint->revoked || !list_empty( &endpoint->broker_queue ))))
        signal_sync( broker->sync );
    else
        reset_sync( broker->sync );
}

/* Disconnect a broker or client channel, waking any waiters and cleaning up queues. */
static void dwm_channel_disconnect( struct dwm_channel *channel )
{
    struct dwm_endpoint *endpoint = channel->endpoint;

    if (channel->closed) return;
    channel->closed = 1;
    list_remove( &channel->proc_entry );
    list_init( &channel->proc_entry );

    if (!endpoint)
    {
        if (channel->sync) signal_sync( channel->sync );
        return;
    }

    if (channel->is_broker)
    {
        struct dwm_message *msg, *next_msg;
        struct dwm_channel *client;

        if (endpoint->broker == channel)
        {
            endpoint->broker = NULL;
            if (endpoint->desktop) endpoint->desktop->dwm_epoch++;
        }

        /* Abort all messages waiting in broker_queue */
        LIST_FOR_EACH_ENTRY_SAFE( msg, next_msg, &endpoint->broker_queue, struct dwm_message, broker_entry )
        {
            list_remove( &msg->broker_entry );
            list_init( &msg->broker_entry );
            assert( endpoint->broker_queued > 0 );
            endpoint->broker_queued--;

            if (msg->client && !msg->client->closed)
            {
                msg->state = DWM_MSG_ABORTED;
                msg->reply_status = STATUS_PORT_DISCONNECTED;
                update_client_sync( msg->client );
            }
            else
            {
                if (msg->client)
                {
                    list_remove( &msg->client_entry );
                    assert( msg->client->queued_count > 0 );
                    msg->client->queued_count--;
                }
                free_dwm_message( msg );
            }
        }

        /* Abort all in-flight messages awaiting dwm_reply */
        LIST_FOR_EACH_ENTRY_SAFE( msg, next_msg, &endpoint->inflight_msgs, struct dwm_message, broker_entry )
        {
            list_remove( &msg->broker_entry );
            list_init( &msg->broker_entry );

            if (msg->client && !msg->client->closed)
            {
                msg->state = DWM_MSG_ABORTED;
                msg->reply_status = STATUS_PORT_DISCONNECTED;
                update_client_sync( msg->client );
            }
            else
            {
                if (msg->client)
                {
                    list_remove( &msg->client_entry );
                    assert( msg->client->queued_count > 0 );
                    msg->client->queued_count--;
                }
                free_dwm_message( msg );
            }
        }

        /* Mark all currently connected client channels on this desktop as disconnected and wake them */
        LIST_FOR_EACH_ENTRY( client, &endpoint->clients, struct dwm_channel, desktop_entry )
        {
            client->disconnected = 1;
            update_client_sync( client );
        }

        if (channel->sync) signal_sync( channel->sync );
    }
    else
    {
        struct dwm_message *msg, *next_msg;

        if (!list_empty( &channel->desktop_entry ))
        {
            list_remove( &channel->desktop_entry );
            list_init( &channel->desktop_entry );
        }

        LIST_FOR_EACH_ENTRY_SAFE( msg, next_msg, &channel->messages, struct dwm_message, client_entry )
        {
            list_remove( &msg->client_entry );
            list_init( &msg->client_entry );
            assert( channel->queued_count > 0 );
            channel->queued_count--;
            msg->client = NULL;

            if (msg->state == DWM_MSG_QUEUED)
            {
                list_remove( &msg->broker_entry );
                assert( endpoint->broker_queued > 0 );
                endpoint->broker_queued--;
                free_dwm_message( msg );
                if (endpoint->broker) update_broker_sync( endpoint->broker );
            }
            else if (msg->state == DWM_MSG_INFLIGHT)
            {
                /* Broker already popped this message; detach from client and free completely. */
                list_remove( &msg->broker_entry );
                free_dwm_message( msg );
            }
            else
            {
                free_dwm_message( msg );
            }
        }

        if (channel->sync) signal_sync( channel->sync );
    }
}

/* Revoke a desktop's DWM endpoint when the desktop object is destroyed. */
void dwm_revoke_desktop( struct desktop *desktop )
{
    struct dwm_endpoint *endpoint = desktop->dwm_endpoint;
    struct dwm_channel *client, *next_client;

    if (!endpoint) return;

    desktop->dwm_endpoint = NULL;
    desktop->dwm_epoch++;
    endpoint->revoked = 1;
    endpoint->pending_cap = 0;
    endpoint->cap_owner_pid = 0;
    endpoint->desktop = NULL;

    if (endpoint->broker)
    {
        struct dwm_channel *broker = endpoint->broker;
        broker->revoked = 1;
        dwm_channel_disconnect( broker );
    }

    LIST_FOR_EACH_ENTRY_SAFE( client, next_client, &endpoint->clients, struct dwm_channel, desktop_entry )
    {
        client->revoked = 1;
        client->disconnected = 1;
        list_remove( &client->desktop_entry );
        list_init( &client->desktop_entry );
        update_client_sync( client );
    }

    release_endpoint( endpoint );
}

/* Clean up all DWM channels and pending capabilities owned by an exiting process. */
void dwm_cleanup_process( struct process *process )
{
    process_id_t pid = get_process_id( process );
    struct dwm_endpoint *endpoint;
    struct dwm_channel *channel, *next;

    LIST_FOR_EACH_ENTRY( endpoint, &all_endpoints, struct dwm_endpoint, entry )
    {
        if (endpoint->cap_owner_pid == pid)
        {
            endpoint->pending_cap = 0;
            endpoint->cap_owner_pid = 0;
        }
    }

    LIST_FOR_EACH_ENTRY_SAFE( channel, next, &all_channels, struct dwm_channel, proc_entry )
    {
        if (channel->owner_pid != pid) continue;
        dwm_channel_disconnect( channel );
    }
}

static unsigned int count_process_channels( struct process *process )
{
    process_id_t pid = get_process_id( process );
    const struct dwm_channel *channel;
    unsigned int count = 0;

    LIST_FOR_EACH_ENTRY( channel, &all_channels, const struct dwm_channel, proc_entry )
    {
        if (channel->owner_pid == pid && !channel->closed) count++;
    }
    return count;
}

static unsigned __int64 generate_capability_token(void)
{
    unsigned __int64 rnd = 0, seq = next_cap_seq++;
    int fd;

    if ((fd = open( "/dev/urandom", O_RDONLY )) != -1)
    {
        ssize_t r = read( fd, &rnd, sizeof(rnd) );
        (void)r;
        close( fd );
    }

    /* Mix monotonic sequence, current_time, and urandom entropy via SplitMix64 */
    rnd ^= seq + (unsigned __int64)current_time * 0x9e3779b97f4a7c15ULL;
    rnd = (rnd ^ (rnd >> 30)) * 0xbf58476d1ce4e5b9ULL;
    rnd = (rnd ^ (rnd >> 27)) * 0x94d049bb133111ebULL;
    rnd = rnd ^ (rnd >> 31);
    if (!rnd) rnd = seq ? seq : 1;
    return rnd;
}

/*
 * Security Check 1 (Window Station & Desktop Rights):
 * Resolve the calling thread's current desktop while enforcing:
 *   - The caller process is attached to a valid window station with WINSTA_READATTRIBUTES.
 *   - The caller thread is attached to a valid desktop with DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS.
 *   - The thread's desktop belongs to the caller process's window station (refuses other-winstation use).
 *   - cross_desktop_denied( current, desktop ) passes under the security policy.
 */
static struct desktop *dwm_get_caller_desktop(void)
{
    struct winstation *winstation;
    struct desktop *desktop;

    if (!(winstation = get_process_winstation( current->process, WINSTA_READATTRIBUTES )))
        return NULL;

    if (!(desktop = get_thread_desktop( current, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS )))
    {
        release_object( winstation );
        return NULL;
    }

    if (desktop->winstation != winstation || cross_desktop_denied( current, desktop ))
    {
        set_error( STATUS_ACCESS_DENIED );
        release_object( desktop );
        release_object( winstation );
        return NULL;
    }

    release_object( winstation );
    return desktop;
}

/*
 * Security Check 2 (Cross-Desktop / Cross-Window-Station Channel Refusal):
 * Verify that the calling thread's current desktop and window station match the
 * desktop to which the DWM channel was bound at creation time, using both
 * cross_desktop_denied( current, channel->desktop ) and direct pointer equality.
 */
static struct desktop *dwm_check_channel_desktop( struct dwm_channel *channel )
{
    struct desktop *desktop;

    if (!channel->desktop || channel->revoked || !channel->endpoint || channel->endpoint->revoked)
    {
        set_error( STATUS_PORT_DISCONNECTED );
        return NULL;
    }

    if (cross_desktop_denied( current, channel->desktop ))
        return NULL;

    if (!(desktop = dwm_get_caller_desktop()))
        return NULL;

    if (desktop != channel->desktop || desktop->winstation != channel->winstation)
    {
        set_error( STATUS_ACCESS_DENIED );
        release_object( desktop );
        return NULL;
    }

    return desktop;
}

/*
 * Security Check 3 (Window-Desktop Ownership):
 * If a message envelope references a non-zero window_token, verify the window
 * exists and belongs to the channel's desktop via window_desktop_denied( win )
 * and get_window_desktop( win ) == desktop.
 */
static int dwm_check_window_token( struct desktop *desktop, unsigned __int64 window_token )
{
    struct window *win;

    if (!window_token) return 1;
    if (window_token > 0xffffffffULL)
    {
        set_error( STATUS_INVALID_HANDLE );
        return 0;
    }

    if (!(win = get_user_object( (user_handle_t)window_token, NTUSER_OBJ_WINDOW )))
    {
        set_error( STATUS_INVALID_HANDLE );
        return 0;
    }

    if (window_desktop_denied( win ) || get_window_desktop( win ) != desktop)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }

    return 1;
}

static struct dwm_channel *create_dwm_channel( struct desktop *desktop, struct dwm_endpoint *endpoint, int is_broker )
{
    struct dwm_channel *channel;

    if (!(channel = alloc_object( &dwm_channel_ops ))) return NULL;

    channel->sync          = NULL;
    channel->endpoint      = grab_endpoint( endpoint );
    channel->desktop       = desktop;
    channel->winstation    = desktop->winstation;
    list_init( &channel->desktop_entry );
    list_init( &channel->proc_entry );
    list_init( &channel->messages );
    channel->owner_pid     = get_process_id( current->process );
    channel->connect_epoch = desktop->dwm_epoch;
    channel->queued_count  = 0;
    channel->is_broker     = is_broker;
    channel->disconnected  = 0;
    channel->revoked       = 0;
    channel->closed        = 0;

    if (!(channel->sync = create_internal_sync( 1, 0 )))
    {
        release_object( channel );
        return NULL;
    }

    list_add_tail( &all_channels, &channel->proc_entry );
    if (!is_broker) list_add_tail( &endpoint->clients, &channel->desktop_entry );
    return channel;
}

static void dwm_channel_dump( struct object *obj, int verbose )
{
    struct dwm_channel *channel = (struct dwm_channel *)obj;
    assert( obj->ops == &dwm_channel_ops );
    fprintf( stderr, "DwmChannel role=%s pid=%04x epoch=%llx queued=%u\n",
             channel->is_broker ? "broker" : "client",
             channel->owner_pid, (unsigned long long)channel->connect_epoch,
             channel->queued_count );
}

static struct object *dwm_channel_get_sync( struct object *obj )
{
    struct dwm_channel *channel = (struct dwm_channel *)obj;
    assert( obj->ops == &dwm_channel_ops );
    return grab_object( channel->sync );
}

static int dwm_channel_close_handle( struct object *obj, struct process *process, obj_handle_t handle )
{
    struct dwm_channel *channel = (struct dwm_channel *)obj;
    assert( obj->ops == &dwm_channel_ops );

    if (obj->handle_count <= 1)
        dwm_channel_disconnect( channel );
    return 1;
}

static void dwm_channel_destroy( struct object *obj )
{
    struct dwm_channel *channel = (struct dwm_channel *)obj;
    assert( obj->ops == &dwm_channel_ops );

    dwm_channel_disconnect( channel );
    if (channel->sync) release_object( channel->sync );
    if (channel->endpoint) release_endpoint( channel->endpoint );
}

/* Connect a client channel to the current thread's desktop DWM broker endpoint */
DECL_HANDLER(dwm_connect)
{
    struct desktop *desktop;
    struct dwm_endpoint *endpoint;
    struct dwm_channel *channel;

    if (!(desktop = dwm_get_caller_desktop())) return;

    if (count_process_channels( current->process ) >= DWM_MAX_PROCESS_CHANNELS)
    {
        set_error( STATUS_QUOTA_EXCEEDED );
        release_object( desktop );
        return;
    }

    if (!(endpoint = get_desktop_endpoint( desktop )))
    {
        release_object( desktop );
        return;
    }

    if (endpoint->revoked)
    {
        set_error( STATUS_PORT_DISCONNECTED );
        release_object( desktop );
        return;
    }

    if ((channel = create_dwm_channel( desktop, endpoint, 0 )))
    {
        reply->handle  = alloc_handle_no_access_check( current->process, channel,
                                                       SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE, 0 );
        reply->version = DWM_PROTOCOL_VERSION;
        reply->epoch   = desktop->dwm_epoch;
        release_object( channel );
    }

    release_object( desktop );
}

/* Mint a one-use DWM broker capability token for the desktop's top-window owner */
DECL_HANDLER(dwm_get_broker_capability)
{
    struct desktop *desktop;
    struct process *owner;
    struct dwm_endpoint *endpoint;

    if (!(desktop = dwm_get_caller_desktop())) return;

    /*
     * Only the process owning desktop->top_window (explorer.exe for a managed
     * desktop) may mint a broker capability token for this desktop.
     */
    owner = get_top_window_owner( desktop );
    if (!owner || owner != current->process)
    {
        set_error( STATUS_ACCESS_DENIED );
        release_object( desktop );
        return;
    }

    if (!(endpoint = get_desktop_endpoint( desktop )))
    {
        release_object( desktop );
        return;
    }

    if (endpoint->revoked)
    {
        set_error( STATUS_PORT_DISCONNECTED );
        release_object( desktop );
        return;
    }

    endpoint->pending_cap   = generate_capability_token();
    endpoint->cap_owner_pid = get_process_id( owner );

    reply->capability = endpoint->pending_cap;
    reply->epoch      = desktop->dwm_epoch;
    release_object( desktop );
}

/* Register the current process as the DWM broker for the current thread's desktop */
DECL_HANDLER(dwm_broker_register)
{
    struct desktop *desktop;
    struct process *owner;
    struct dwm_endpoint *endpoint;
    struct dwm_channel *channel;

    if (req->flags)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!(desktop = dwm_get_caller_desktop())) return;

    if (!(endpoint = get_desktop_endpoint( desktop )))
    {
        release_object( desktop );
        return;
    }

    if (endpoint->revoked)
    {
        set_error( STATUS_PORT_DISCONNECTED );
        release_object( desktop );
        return;
    }

    /* Verify the desktop still has its top_window owner and the capability token matches */
    owner = get_top_window_owner( desktop );
    if (!owner || !req->capability || !endpoint->pending_cap ||
        req->capability != endpoint->pending_cap ||
        get_process_id( owner ) != endpoint->cap_owner_pid)
    {
        set_error( STATUS_ACCESS_DENIED );
        release_object( desktop );
        return;
    }

    if (endpoint->broker && !endpoint->broker->closed && !endpoint->broker->revoked)
    {
        set_error( STATUS_PORT_ALREADY_SET );
        release_object( desktop );
        return;
    }

    if (count_process_channels( current->process ) >= DWM_MAX_PROCESS_CHANNELS)
    {
        set_error( STATUS_QUOTA_EXCEEDED );
        release_object( desktop );
        return;
    }

    if ((channel = create_dwm_channel( desktop, endpoint, 1 )))
    {
        obj_handle_t handle = alloc_handle_no_access_check( current->process, channel,
                                                            SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE, 0 );
        if (handle)
        {
            /* Consume the one-use capability token and advance the desktop epoch */
            endpoint->pending_cap   = 0;
            endpoint->cap_owner_pid = 0;
            desktop->dwm_epoch++;
            channel->connect_epoch  = desktop->dwm_epoch;
            endpoint->broker        = channel;

            reply->handle  = handle;
            reply->version = DWM_PROTOCOL_VERSION;
            reply->epoch   = desktop->dwm_epoch;
        }
        release_object( channel );
    }

    release_object( desktop );
}

/* Send a bounded DWM control message from a client channel to its desktop broker */
DECL_HANDLER(dwm_send)
{
    struct dwm_channel *channel;
    struct desktop *desktop;
    struct dwm_endpoint *endpoint;
    struct dwm_msg_header hdr;
    struct dwm_message *msg;
    data_size_t raw_size, payload_size;
    unsigned int request_id;

    if (!(channel = (struct dwm_channel *)get_handle_obj( current->process, req->handle, 0, &dwm_channel_ops )))
        return;

    if (channel->is_broker)
    {
        set_error( STATUS_INVALID_DEVICE_REQUEST );
        release_object( channel );
        return;
    }

    if (!(desktop = dwm_check_channel_desktop( channel )))
    {
        release_object( channel );
        return;
    }

    endpoint = channel->endpoint;

    /*
     * Fail with STATUS_PORT_DISCONNECTED if the channel was invalidated by a prior
     * broker exit/unregistration or if no broker is currently registered on the desktop.
     */
    if (channel->closed || channel->disconnected || channel->revoked ||
        !endpoint || endpoint->revoked ||
        !endpoint->broker || endpoint->broker->closed || endpoint->broker->revoked)
    {
        set_error( STATUS_PORT_DISCONNECTED );
        release_object( desktop );
        release_object( channel );
        return;
    }

    raw_size = get_req_data_size();
    if (raw_size < sizeof(struct dwm_msg_header))
    {
        set_error( STATUS_INVALID_PARAMETER );
        release_object( desktop );
        release_object( channel );
        return;
    }

    payload_size = raw_size - sizeof(struct dwm_msg_header);
    if (payload_size > DWM_MAX_MESSAGE_SIZE)
    {
        set_error( STATUS_PORT_MESSAGE_TOO_LONG );
        release_object( desktop );
        release_object( channel );
        return;
    }

    memcpy( &hdr, get_req_data(), sizeof(hdr) );
    if (hdr.version != DWM_PROTOCOL_VERSION || hdr.length != payload_size || hdr.opcode != DWM_OP_ECHO)
    {
        set_error( STATUS_INVALID_PARAMETER );
        release_object( desktop );
        release_object( channel );
        return;
    }

    if (!dwm_check_window_token( desktop, hdr.window_token ))
    {
        release_object( desktop );
        release_object( channel );
        return;
    }

    if (channel->queued_count >= DWM_MAX_QUEUE_DEPTH || endpoint->broker_queued >= DWM_MAX_QUEUE_DEPTH)
    {
        set_error( STATUS_QUOTA_EXCEEDED );
        release_object( desktop );
        release_object( channel );
        return;
    }

    if (!(msg = mem_alloc( sizeof(*msg) )))
    {
        release_object( desktop );
        release_object( channel );
        return;
    }

    if (!(msg->data = mem_alloc( raw_size )))
    {
        free( msg );
        release_object( desktop );
        release_object( channel );
        return;
    }

    request_id = endpoint->next_request_id++;
    if (!request_id) request_id = endpoint->next_request_id++;

    /*
     * Stamp server-authoritative request_id, connection_epoch, and zero capabilities
     * into the message header, and record sender pid/tid/desktop_id from current.
     * Caller-supplied values for these fields are never trusted.
     */
    hdr.request_id       = request_id;
    hdr.connection_epoch = desktop->dwm_epoch;
    hdr.capabilities     = 0;

    memcpy( msg->data, &hdr, sizeof(hdr) );
    if (payload_size)
        memcpy( msg->data + sizeof(hdr), (const unsigned char *)get_req_data() + sizeof(hdr), payload_size );

    msg->client       = channel;
    msg->request_id   = request_id;
    msg->sender_pid   = get_process_id( current->process );
    msg->sender_tid   = get_thread_id( current );
    msg->desktop_id   = endpoint->desktop_id;
    msg->reply_status = STATUS_PENDING;
    msg->state        = DWM_MSG_QUEUED;
    msg->size         = raw_size;

    list_add_tail( &channel->messages, &msg->client_entry );
    list_add_tail( &endpoint->broker_queue, &msg->broker_entry );
    channel->queued_count++;
    endpoint->broker_queued++;

    update_broker_sync( endpoint->broker );

    reply->request_id = request_id;
    reply->epoch      = desktop->dwm_epoch;

    release_object( desktop );
    release_object( channel );
}

/* Receive the next queued DWM request (broker) or completed reply (client) */
DECL_HANDLER(dwm_recv)
{
    struct dwm_channel *channel;
    struct desktop *desktop;
    struct dwm_endpoint *endpoint;

    if (!(channel = (struct dwm_channel *)get_handle_obj( current->process, req->handle, 0, &dwm_channel_ops )))
        return;

    if (!(desktop = dwm_check_channel_desktop( channel )))
    {
        release_object( channel );
        return;
    }

    endpoint = channel->endpoint;

    if (channel->is_broker)
    {
        struct dwm_message *msg;
        struct list *head;

        if (channel->closed || channel->revoked || !endpoint || endpoint->revoked || endpoint->broker != channel)
        {
            set_error( STATUS_PORT_DISCONNECTED );
            release_object( desktop );
            release_object( channel );
            return;
        }

        if (!(head = list_head( &endpoint->broker_queue )))
        {
            update_broker_sync( channel );
            set_error( STATUS_PENDING );
            release_object( desktop );
            release_object( channel );
            return;
        }

        msg = LIST_ENTRY( head, struct dwm_message, broker_entry );
        if (get_reply_max_size() < msg->size)
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            release_object( desktop );
            release_object( channel );
            return;
        }

        list_remove( &msg->broker_entry );
        assert( endpoint->broker_queued > 0 );
        endpoint->broker_queued--;
        msg->state = DWM_MSG_INFLIGHT;
        list_add_tail( &endpoint->inflight_msgs, &msg->broker_entry );

        update_broker_sync( channel );

        reply->request_id = msg->request_id;
        reply->sender_pid = msg->sender_pid;
        reply->sender_tid = msg->sender_tid;
        reply->desktop_id = msg->desktop_id;
        reply->status     = STATUS_SUCCESS;
        set_reply_data( msg->data, msg->size );
    }
    else
    {
        struct dwm_message *msg, *found = NULL;

        LIST_FOR_EACH_ENTRY( msg, &channel->messages, struct dwm_message, client_entry )
        {
            if (msg->state == DWM_MSG_REPLIED || msg->state == DWM_MSG_ABORTED)
            {
                found = msg;
                break;
            }
        }

        if (!found)
        {
            if (channel->closed || channel->disconnected || channel->revoked ||
                !endpoint || endpoint->revoked ||
                !endpoint->broker || endpoint->broker->closed || endpoint->broker->revoked)
            {
                set_error( STATUS_PORT_DISCONNECTED );
            }
            else
            {
                update_client_sync( channel );
                set_error( STATUS_PENDING );
            }
            release_object( desktop );
            release_object( channel );
            return;
        }

        if (found->state == DWM_MSG_ABORTED)
        {
            list_remove( &found->client_entry );
            assert( channel->queued_count > 0 );
            channel->queued_count--;
            free_dwm_message( found );
            update_client_sync( channel );
            set_error( STATUS_PORT_DISCONNECTED );
            release_object( desktop );
            release_object( channel );
            return;
        }

        if (get_reply_max_size() < found->size)
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            release_object( desktop );
            release_object( channel );
            return;
        }

        list_remove( &found->client_entry );
        assert( channel->queued_count > 0 );
        channel->queued_count--;

        reply->request_id = found->request_id;
        reply->sender_pid = found->sender_pid;
        reply->sender_tid = found->sender_tid;
        reply->desktop_id = found->desktop_id;
        reply->status     = found->reply_status;
        set_reply_data( found->data, found->size );

        free_dwm_message( found );
        update_client_sync( channel );
    }

    release_object( desktop );
    release_object( channel );
}

/* Reply to a pending DWM client request from the broker channel */
DECL_HANDLER(dwm_reply)
{
    struct dwm_channel *channel;
    struct desktop *desktop;
    struct dwm_endpoint *endpoint;
    struct dwm_message *msg, *found = NULL;
    struct dwm_msg_header hdr;
    data_size_t raw_size, payload_size;

    if (!(channel = (struct dwm_channel *)get_handle_obj( current->process, req->handle, 0, &dwm_channel_ops )))
        return;

    if (!channel->is_broker)
    {
        set_error( STATUS_INVALID_DEVICE_REQUEST );
        release_object( channel );
        return;
    }

    if (!(desktop = dwm_check_channel_desktop( channel )))
    {
        release_object( channel );
        return;
    }

    endpoint = channel->endpoint;
    if (channel->closed || channel->revoked || !endpoint || endpoint->revoked || endpoint->broker != channel)
    {
        set_error( STATUS_PORT_DISCONNECTED );
        release_object( desktop );
        release_object( channel );
        return;
    }

    raw_size = get_req_data_size();
    if (raw_size < sizeof(struct dwm_msg_header))
    {
        set_error( STATUS_INVALID_PARAMETER );
        release_object( desktop );
        release_object( channel );
        return;
    }

    payload_size = raw_size - sizeof(struct dwm_msg_header);
    if (payload_size > DWM_MAX_MESSAGE_SIZE)
    {
        set_error( STATUS_PORT_MESSAGE_TOO_LONG );
        release_object( desktop );
        release_object( channel );
        return;
    }

    memcpy( &hdr, get_req_data(), sizeof(hdr) );
    if (hdr.version != DWM_PROTOCOL_VERSION || hdr.length != payload_size)
    {
        set_error( STATUS_INVALID_PARAMETER );
        release_object( desktop );
        release_object( channel );
        return;
    }

    LIST_FOR_EACH_ENTRY( msg, &endpoint->inflight_msgs, struct dwm_message, broker_entry )
    {
        if (msg->request_id == req->request_id)
        {
            found = msg;
            break;
        }
    }

    if (!found)
    {
        set_error( STATUS_INVALID_PARAMETER );
        release_object( desktop );
        release_object( channel );
        return;
    }

    if (!found->client || found->client->closed || found->client->revoked)
    {
        list_remove( &found->broker_entry );
        if (found->client)
        {
            list_remove( &found->client_entry );
            assert( found->client->queued_count > 0 );
            found->client->queued_count--;
        }
        free_dwm_message( found );
        release_object( desktop );
        release_object( channel );
        return;
    }

    if (raw_size != found->size)
    {
        unsigned char *new_data = mem_alloc( raw_size );
        if (!new_data)
        {
            release_object( desktop );
            release_object( channel );
            return;
        }
        free( found->data );
        found->data = new_data;
        found->size = raw_size;
    }

    list_remove( &found->broker_entry );
    list_init( &found->broker_entry );

    hdr.request_id       = found->request_id;
    hdr.connection_epoch = desktop->dwm_epoch;
    hdr.capabilities     = 0;

    memcpy( found->data, &hdr, sizeof(hdr) );
    if (payload_size)
        memcpy( found->data + sizeof(hdr), (const unsigned char *)get_req_data() + sizeof(hdr), payload_size );

    found->reply_status = req->status;
    found->state        = DWM_MSG_REPLIED;
    update_client_sync( found->client );

    release_object( desktop );
    release_object( channel );
}
