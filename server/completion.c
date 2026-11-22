/*
 * Server-side IO completion ports implementation
 *
 * Copyright (C) 2007 Andrey Turkin
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

/* FIXME: "max concurrent active threads" parameter is not used */

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"

static const WCHAR completion_name[] = {'I','o','C','o','m','p','l','e','t','i','o','n'};

struct type_descr completion_type =
{
    { completion_name, sizeof(completion_name) },   /* name */
    IO_COMPLETION_ALL_ACCESS,                       /* valid_access */
    {                                               /* mapping */
        STANDARD_RIGHTS_READ | IO_COMPLETION_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | IO_COMPLETION_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        IO_COMPLETION_ALL_ACCESS
    },
};

struct comp_msg
{
    struct   list queue_entry;
    apc_param_t   ckey;
    apc_param_t   cvalue;
    apc_param_t   information;
    unsigned int  status;
    struct completion_packet *packet; /* the completion packet this msg is from or NULL */
};

struct completion_wait
{
    struct object      obj;
    obj_handle_t       handle;
    struct completion *completion;
    struct thread     *thread;
    struct comp_msg   *msg;
    struct list        wait_queue_entry;
};

struct completion
{
    struct object       obj;
    struct object      *sync;
    struct list         queue;
    struct list         wait_queue;
    unsigned int        depth;
    unsigned int        concurrent;
};

struct completion_packet
{
    struct object      obj;                       /* object header */
    struct list        entry;                     /* list entry in the target object wait completion packet queue */
    struct object     *target;                    /* target object this packet waits for */
    struct completion *completion;                /* completion object */
    apc_param_t        ckey;                      /* key context */
    apc_param_t        cvalue;                    /* apc context */
    apc_param_t        information;               /* IO_STATUS_BLOCK information */
    unsigned int       status;                    /* completion status */
    unsigned int       in_target_packet_queue: 1; /* whether the packet is in the wait queue of the target */
    unsigned int       in_completion_queue: 1;    /* whether the packet is in the completion queue */
    unsigned int       pad: 30;                   /* padding */
};

static void remove_completion_packet_msg( struct comp_msg *msg );

static void completion_wait_dump( struct object*, int );
static int completion_wait_signaled( struct object *obj, struct wait_queue_entry *entry );
static void completion_wait_satisfied( struct object *obj, struct wait_queue_entry *entry );
static void completion_wait_destroy( struct object * );

static const struct object_ops completion_wait_ops =
{
    .size         = sizeof(struct completion_wait),
    .type         = &no_type,
    .dump         = completion_wait_dump,
    .add_queue    = add_queue,
    .remove_queue = remove_queue,
    .signaled     = completion_wait_signaled,
    .satisfied    = completion_wait_satisfied,
    .destroy      = completion_wait_destroy,
};

static void completion_wait_destroy( struct object *obj )
{
    struct completion_wait *wait = (struct completion_wait *)obj;

    free( wait->msg );
}

static void completion_wait_dump( struct object *obj, int verbose )
{
    struct completion_wait *wait = (struct completion_wait *)obj;

    assert( obj->ops == &completion_wait_ops );
    fprintf( stderr, "Completion wait completion=%p\n", wait->completion );
}

static int completion_wait_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct completion_wait *wait = (struct completion_wait *)obj;

    assert( obj->ops == &completion_wait_ops );
    if (!wait->completion) return 1;
    return wait->completion->depth;
}

static void completion_wait_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
    struct completion_wait *wait = (struct completion_wait *)obj;
    struct list *msg_entry;
    struct comp_msg *msg;

    assert( obj->ops == &completion_wait_ops );
    if (!wait->completion)
    {
        make_wait_abandoned( entry );
        return;
    }
    msg_entry = list_head( &wait->completion->queue );
    assert( msg_entry );
    msg = LIST_ENTRY( msg_entry, struct comp_msg, queue_entry );
    --wait->completion->depth;
    list_remove( &msg->queue_entry );
    if (wait->msg) free( wait->msg );
    wait->msg = msg;
}

struct completion_init_data
{
    unsigned int concurrent;
};

static void completion_dump( struct object*, int );
static bool completion_init( struct object *obj, const void *init_data );
static struct object *completion_get_sync( struct object * );
static int completion_close_handle( struct object *obj, struct process *process, obj_handle_t handle );
static void completion_destroy( struct object * );

static const struct object_ops completion_ops =
{
    .size         = sizeof(struct completion),
    .type         = &completion_type,
    .dump         = completion_dump,
    .init         = completion_init,
    .get_sync     = completion_get_sync,
    .close_handle = completion_close_handle,
    .destroy      = completion_destroy,
};

static void completion_destroy( struct object *obj)
{
    struct completion *completion = (struct completion *) obj;
    struct comp_msg *tmp, *next;

    LIST_FOR_EACH_ENTRY_SAFE( tmp, next, &completion->queue, struct comp_msg, queue_entry )
    {
        free( tmp );
    }

    if (completion->sync) release_object( completion->sync );
}

static void completion_dump( struct object *obj, int verbose )
{
    struct completion *completion = (struct completion *) obj;

    assert( obj->ops == &completion_ops );
    fprintf( stderr, "Completion depth=%u\n", completion->depth );
}

static bool completion_init( struct object *obj, const void *init_data )
{
    struct completion *completion = (struct completion *)obj;
    const struct completion_init_data *data = init_data;

    completion->depth = 0;
    completion->concurrent = data->concurrent;
    list_init( &completion->queue );
    list_init( &completion->wait_queue );
    return !!(completion->sync = create_internal_sync( 1, 0 ));
}

static struct object *completion_get_sync( struct object *obj )
{
    struct completion *completion = (struct completion *)obj;
    assert( obj->ops == &completion_ops );
    return grab_object( completion->sync );
}

static int completion_close_handle( struct object *obj, struct process *process, obj_handle_t handle )
{
    struct completion *completion = (struct completion *)obj;
    struct completion_wait *wait, *wait_next;

    if (completion->obj.handle_count != 1) return 1;

    LIST_FOR_EACH_ENTRY_SAFE( wait, wait_next, &completion->wait_queue, struct completion_wait, wait_queue_entry )
    {
        assert( wait->completion );
        wait->completion = NULL;
        list_remove( &wait->wait_queue_entry );
        if (!wait->msg)
        {
            wake_up( &wait->obj, 0 );
            cleanup_thread_completion( wait->thread );
        }
    }
    signal_sync( completion->sync );
    return 1;
}

void cleanup_thread_completion( struct thread *thread )
{
    if (!thread->completion_wait) return;

    if (thread->completion_wait->handle)
    {
        close_handle( thread->process, thread->completion_wait->handle );
        thread->completion_wait->handle = 0;
    }
    if (thread->completion_wait->completion) list_remove( &thread->completion_wait->wait_queue_entry );
    release_object( &thread->completion_wait->obj );
    thread->completion_wait = NULL;
}

static struct completion_wait *create_completion_wait( struct thread *thread )
{
    struct completion_wait *wait;

    if (!(wait = alloc_object( &completion_wait_ops ))) return NULL;
    wait->completion = NULL;
    wait->thread = thread;
    wait->msg = NULL;
    if (!(wait->handle = alloc_handle( current->process, wait, SYNCHRONIZE, 0 )))
    {
        release_object( &wait->obj );
        return NULL;
    }
    return wait;
}

struct completion *get_completion_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    return (struct completion *) get_handle_obj( process, handle, access, &completion_ops );
}

void add_completion( struct completion *completion, apc_param_t ckey, apc_param_t cvalue,
                     unsigned int status, apc_param_t information, struct completion_packet *packet )
{
    struct comp_msg *msg = mem_alloc( sizeof( *msg ) );
    struct completion_wait *wait;

    if (!msg)
        return;

    msg->packet = packet;
    msg->ckey = ckey;
    msg->cvalue = cvalue;
    msg->status = status;
    msg->information = information;

    list_add_tail( &completion->queue, &msg->queue_entry );
    completion->depth++;
    LIST_FOR_EACH_ENTRY( wait, &completion->wait_queue, struct completion_wait, wait_queue_entry )
    {
        wake_up( &wait->obj, 1 );
        if (list_empty( &completion->queue )) return;
    }
    if (!list_empty( &completion->queue )) signal_sync( completion->sync );
}

static const WCHAR completion_packet_name[] = {'W','a','i','t','C','o','m','p','l','e','t','i','o','n','P','a','c','k','e','t'};

struct type_descr completion_packet_type =
{
    { completion_packet_name, sizeof(completion_packet_name) }, /* name */
    WAIT_COMPLETION_PACKET_ALL_ACCESS | SYNCHRONIZE,            /* valid_access */
    {                                                           /* mapping */
         WAIT_COMPLETION_PACKET_GENERIC_READ,
         WAIT_COMPLETION_PACKET_GENERIC_WRITE,
         WAIT_COMPLETION_PACKET_GENERIC_EXECUTE,
         WAIT_COMPLETION_PACKET_ALL_ACCESS
    },
};

static void completion_packet_dump( struct object *, int );
static void completion_packet_destroy( struct object * );

static const struct object_ops completion_packet_ops =
{
    .size         = sizeof(struct completion_packet),
    .type         = &completion_packet_type,
    .dump         = completion_packet_dump,
    .destroy      = completion_packet_destroy,
};

static void completion_packet_dump( struct object *obj, int verbose )
{
    struct completion_packet *packet = (struct completion_packet *)obj;

    assert( obj->ops == &completion_packet_ops );
    fprintf( stderr, "WaitCompletionPacket target=%p completion=%p ckey=%llx cvalue=%llx "
             "information=%llx status=%#x in_target_packet_queue=%d in_completion_queue=%d\n",
             packet->target, packet->completion, (long long unsigned int)packet->ckey,
             (long long unsigned int)packet->cvalue, (long long unsigned int)packet->information,
             packet->status, packet->in_target_packet_queue, packet->in_completion_queue );
}

static struct completion_packet *get_completion_packet_obj( struct process *process,
                                                            obj_handle_t handle,
                                                            unsigned int access )
{
    return (struct completion_packet *)get_handle_obj( process, handle, access, &completion_packet_ops );
}

/* try to wake up completion packets in the object when it's signaled */
void wake_up_completion_packets( struct object *obj )
{
    struct completion_packet *packet;

    if (list_empty( &obj->completion_packet_queue ))
        return;

    if (!is_obj_signaled( obj ))
        return;

    LIST_FOR_EACH_ENTRY( packet, &obj->completion_packet_queue, struct completion_packet, entry )
    {
        assert( packet->in_target_packet_queue );
        assert( packet->target );
        assert( !packet->in_completion_queue );
        assert( packet->completion );

        list_remove( &packet->entry );
        release_object( packet->target );
        packet->in_target_packet_queue = 0;
        packet->target = NULL;
        packet->in_completion_queue = 1;
        add_completion( packet->completion, packet->ckey, packet->cvalue, packet->status,
                        packet->information, packet );
    }
}

static void cancel_completion_packet( struct completion_packet *packet )
{
    struct comp_msg *comp_msg;

    if (packet->in_target_packet_queue)
    {
        assert( packet->target );
        list_remove( &packet->entry );
        packet->in_target_packet_queue = 0;
    }

    if (packet->in_completion_queue)
    {
        assert( packet->completion );
        LIST_FOR_EACH_ENTRY( comp_msg, &packet->completion->queue, struct comp_msg, queue_entry )
        {
            if (comp_msg->packet == packet)
            {
                list_remove( &comp_msg->queue_entry );
                free( comp_msg );
                packet->completion->depth--;
                break;
            }
        }

        packet->in_completion_queue = 0;
    }

    if (packet->target)
    {
        release_object( packet->target );
        packet->target = NULL;
    }

    if (packet->completion)
    {
        release_object( packet->completion );
        packet->completion = NULL;
    }
}

static void completion_packet_destroy( struct object *obj )
{
    struct completion_packet *packet = (struct completion_packet *)obj;

    cancel_completion_packet( packet );
}

static void remove_completion_packet_msg( struct comp_msg *msg )
{
    if (!msg->packet)
        return;

    assert( msg->packet->in_completion_queue );
    assert( msg->packet->completion );
    release_object( msg->packet->completion );
    msg->packet->completion = NULL;
    msg->packet->in_completion_queue = 0;
    msg->packet = NULL;
}

/* create a completion */
DECL_HANDLER(create_completion)
{
    struct completion_init_data data = { .concurrent = req->concurrent };
    struct object_params params = { .ops = &completion_ops, .access = req->access, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;
    reply->handle = create_named_obj_handle( current->process, &params );
    if (params.root) release_object( params.root );
}

/* open a completion */
DECL_HANDLER(open_completion)
{
    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &completion_ops, get_req_unicode_str(), req->attributes );
}


/* add completion to completion port */
DECL_HANDLER(add_completion)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, IO_COMPLETION_MODIFY_STATE );
    struct reserve *reserve = NULL;

    if (!completion) return;

    if (req->reserve_handle && !(reserve = get_completion_reserve_obj( current->process, req->reserve_handle, 0 )))
    {
        release_object( completion );
        return;
    }

    add_completion( completion, req->ckey, req->cvalue, req->status, req->information, NULL );

    if (reserve) release_object( reserve );
    release_object( completion );
}

/* get completion from completion port */
DECL_HANDLER(remove_completion)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, IO_COMPLETION_MODIFY_STATE );
    struct list *entry;
    struct comp_msg *msg;

    if (!completion) return;

    entry = list_head( &completion->queue );
    if (req->alertable && !list_empty( &current->user_apc )
        && !(entry && current->completion_wait && current->completion_wait->completion == completion))
    {
        set_error( STATUS_USER_APC );
        release_object( completion );
        return;
    }
    if (current->completion_wait)
    {
        list_remove( &current->completion_wait->wait_queue_entry );
    }
    else if (!(current->completion_wait = create_completion_wait( current )))
    {
        release_object( completion );
        return;
    }
    current->completion_wait->completion = completion;
    list_add_head( &completion->wait_queue, &current->completion_wait->wait_queue_entry );
    if (!entry)
    {
        reply->wait_handle = current->completion_wait->handle;
        set_error( STATUS_PENDING );
    }
    else
    {
        list_remove( entry );
        completion->depth--;
        msg = LIST_ENTRY( entry, struct comp_msg, queue_entry );
        reply->ckey = msg->ckey;
        reply->cvalue = msg->cvalue;
        reply->status = msg->status;
        reply->information = msg->information;
        remove_completion_packet_msg( msg );
        free( msg );
        reply->wait_handle = 0;
        if (list_empty( &completion->queue )) reset_sync( completion->sync );
    }

    release_object( completion );
}

/* get completion after successful waiting for it */
DECL_HANDLER(get_thread_completion)
{
    struct comp_msg *msg;

    if (!current->completion_wait || !(msg = current->completion_wait->msg))
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    reply->ckey = msg->ckey;
    reply->cvalue = msg->cvalue;
    reply->status = msg->status;
    reply->information = msg->information;
    free( msg );
    current->completion_wait->msg = NULL;
    if (!current->completion_wait->completion) cleanup_thread_completion( current );
}

/* get queue depth for completion port */
DECL_HANDLER(query_completion)
{
    struct completion* completion = get_completion_obj( current->process, req->handle, IO_COMPLETION_QUERY_STATE );

    if (!completion) return;

    reply->depth = completion->depth;

    release_object( completion );
}

/* create a wait completion packet */
DECL_HANDLER(create_completion_packet)
{
    struct object_params params = { .ops = &completion_packet_ops, .access = req->access };

    if (!get_req_object_attributes( &params )) return;
    reply->handle = create_named_obj_handle( current->process, &params );
    if (params.root) release_object( params.root );
}

/* associate a wait completion packet */
DECL_HANDLER(associate_completion_packet)
{
    struct completion_packet *packet;
    struct completion *completion;
    struct object *target, *sync;

    packet = get_completion_packet_obj( current->process, req->packet, WAIT_COMPLETION_PACKET_QUERY_STATE );
    if (!packet)
        return;

    if (packet->in_target_packet_queue || packet->in_completion_queue)
    {
        release_object( packet );
        set_error( STATUS_INVALID_PARAMETER_1 );
        return;
    }

    target = get_handle_obj( current->process, req->target, SYNCHRONIZE, NULL );
    if (!target)
    {
        release_object( packet );
        return;
    }

    /* mutexs, keyed events and completions are not allowed for associating with wait completion packets */
    if (target->ops->type == &mutex_type
        || target->ops->type == &keyed_event_type
        || target->ops->type == &completion_type)
    {
        release_object( target );
        release_object( packet );
        set_error( STATUS_INVALID_PARAMETER_3 );
        return;
    }

    completion = get_completion_obj( current->process, req->completion, IO_COMPLETION_MODIFY_STATE );
    if (!completion)
    {
        release_object( target );
        release_object( packet );
        return;
    }

    assert( !packet->completion );
    assert( !packet->in_completion_queue );
    assert( !packet->target );
    assert( !packet->in_target_packet_queue );

    packet->completion = (struct completion *)grab_object( completion );
    packet->ckey = req->ckey;
    packet->cvalue = req->cvalue;
    packet->information = req->information;
    packet->status = req->status;

    if (is_obj_signaled( target ))
    {
        packet->in_completion_queue = 1;
        add_completion( packet->completion, packet->ckey, packet->cvalue, packet->status,
                        packet->information, packet );
        reply->already_signaled = 1;
    }
    else
    {
        packet->target = grab_object( target );
        sync = target->ops->get_sync( target );
        list_add_tail( &sync->completion_packet_queue, &packet->entry );
        packet->in_target_packet_queue = 1;
        release_object( sync );
        reply->already_signaled = 0;
    }
    release_object( completion );
    release_object( target );
    release_object( packet );
}

/* cancel a wait completion packet */
DECL_HANDLER(cancel_completion_packet)
{
    struct completion_packet *packet;

    packet = get_completion_packet_obj( current->process, req->packet, WAIT_COMPLETION_PACKET_QUERY_STATE );
    if (!packet)
        return;

    if (!packet->in_target_packet_queue && !packet->in_completion_queue)
    {
        set_error( STATUS_CANCELLED );
        release_object( packet );
        return;
    }

    if (packet->in_completion_queue && !req->remove_signaled)
    {
        set_error( STATUS_PENDING );
        release_object( packet );
        return;
    }

    cancel_completion_packet( packet );
    release_object( packet );
}
