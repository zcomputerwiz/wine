/*
 * WineExplorer system tier: processes of a session's server, opened by session 0 (X3, M3 waits and queries)
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

/* docs/SESSION0_CROSS_SERVER.md, M3. Session 0 opens a process of a session's server (its id is in a block that
 * server leased) over the X0 link:
 * - the owner (the session's server) serves the process kind: it checks the rights against the caller's token
 *   (link_bound_token) and the process's descriptor, and answers WEX_X_INFO from its own data; only session 0's
 *   callers (a user's server's one peer), never another user's server;
 * - session 0 holds a remote process object: OpenProcess starts an open (OPEN, then INFO, then SUBSCRIBE) and ntdll
 *   waits on the returned call; the object is signaled at the death notice (after INFO again for the exit code) or
 *   when the owner's epoch ends (M6); get_process_info and get_process_image_name answer from the owner's
 *   information; every other operation on it is STATUS_NOT_SUPPORTED. */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

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

/* ---- the owner's side: the process kind ---- */

static int process_class_supported( unsigned int info_class )
{
    switch (info_class)
    {
    case ProcessBasicInformation:
    case ProcessTimes:
    case ProcessPriorityClass:
    case ProcessSessionInformation:
    case ProcessWow64Information:
    case ProcessImageFileName:
    case ProcessImageFileNameWin32:
        return 1;
    }
    return 0;
}

static unsigned int xproc_open( const void *name, unsigned int size, unsigned int access, struct token *token,
                                struct object **obj, unsigned int *granted )
{
    struct process *process;
    unsigned int pid, mapped;

    /* M3: session 0's callers only; session 0's own processes are not opened over the link (a user's server
     * acting on them, or on another user's, is not in this design) */
    if (session0_server) return STATUS_ACCESS_DENIED;
    if (size != sizeof(pid)) return STATUS_INVALID_PARAMETER;
    memcpy( &pid, name, sizeof(pid) );
    clear_error();
    if (!(process = get_process_from_id( pid ))) return get_error();
    mapped = map_obj_access( &process->obj, access );
    if (!check_object_access( token, &process->obj, &mapped ))
    {
        unsigned int status = get_error();

        release_object( process );
        clear_error();
        return status ? status : STATUS_ACCESS_DENIED;
    }
    *obj = &process->obj;
    *granted = mapped;
    return STATUS_SUCCESS;
}

static int xproc_ready( struct object *obj )
{
    return !((struct process *)obj)->running_threads;
}

static unsigned int xproc_try_acquire( struct object *obj )
{
    return xproc_ready( obj ) ? STATUS_SUCCESS : STATUS_TIMEOUT;  /* a process is not consumed by a wait */
}

static void xproc_give_back( struct object *obj )
{
}

static unsigned int xproc_info( struct object *obj, unsigned int granted, unsigned int info_class, void **data,
                                size_t *size )
{
    static unsigned char warned[64];
    struct process *process = (struct process *)obj;
    struct wex_x_process_info info;
    unsigned int image_len = process->image ? process->imagelen : 0;
    char *buf;

    if (!process_class_supported( info_class ))
    {
        /* one c2r-split lab run shows which further classes Click-to-Run asks for */
        if (info_class >= sizeof(warned) * 8 || !(warned[info_class / 8] & (1 << (info_class % 8))))
        {
            if (info_class < sizeof(warned) * 8) warned[info_class / 8] |= 1 << (info_class % 8);
            fprintf( stderr, "x3: FIXME: process information class %u for session 0 is not supported\n", info_class );
        }
        return STATUS_NOT_SUPPORTED;
    }
    /* every supported class needs the limited query right, as get_process_info does (Windows: Vista and later) */
    if (!(granted & PROCESS_QUERY_LIMITED_INFORMATION)) return STATUS_ACCESS_DENIED;
    if (image_len > 32768) image_len = 0;
    memset( &info, 0, sizeof(info) );
    info.affinity      = process->affinity;
    info.peb           = process->peb;
    info.start_time    = process->start_time;
    info.end_time      = process->end_time;
    info.pid           = get_process_id( process );
    info.ppid          = process->parent_id;
    info.exit_code     = process->exit_code;
    info.session_id    = process->session_id;
    info.priority      = process->priority;
    info.base_priority = process->base_priority;
    info.disable_boost = process->disable_boost;
    info.machine       = process->machine;
    info.running       = process->running_threads != 0;
    info.image_len     = image_len;
    if (!(buf = malloc( sizeof(info) + image_len ))) return STATUS_NO_MEMORY;
    memcpy( buf, &info, sizeof(info) );
    if (image_len) memcpy( buf + sizeof(info), process->image, image_len );
    *data = buf;
    *size = sizeof(info) + image_len;
    return STATUS_SUCCESS;
}

static const struct x_kind_ops process_kind =
{
    .kind        = WEX_X_KIND_PROCESS,
    .open        = xproc_open,
    .ready       = xproc_ready,
    .try_acquire = xproc_try_acquire,
    .give_back   = xproc_give_back,
    .info        = xproc_info,
};

/* a process of this server ended: session 0's subscriptions to it get their death notice */
void x_process_ended( struct process *process )
{
    x_object_changed( &process->obj );
}

/* ---- session 0's side: the remote process ---- */

/* the exit status a remote process reports when its owner's epoch ended before its death notice came (M6) */
#define X_OWNER_GONE_EXIT STATUS_CONNECTION_DISCONNECTED

struct x_rproc
{
    struct object              obj;
    struct object             *sync;      /* signaled once it has ended, or its owner did */
    struct x_held             *held;      /* NULL before the open's reply */
    struct x_peer             *peer;      /* the owner at the open (the held reference keeps the current one) */
    struct wex_x_process_info  info;      /* the owner's, at the open and at the death notice */
    WCHAR                     *image;
    unsigned int               granted;
    int                        ended;
    int                        refreshing;  /* an INFO after the death notice is in flight */
};

/* an open in progress: the caller waits on it, then takes the handle with x_process_result */
struct x_pcall
{
    struct object              obj;
    struct object             *sync;      /* signaled when the open has ended */
    struct process            *caller;    /* only this process takes the result */
    struct x_rproc            *rproc;
    unsigned int               status;    /* STATUS_PENDING while it runs */
    unsigned int               access;
    unsigned int               attributes;
};

static const WCHAR rproc_type_name[] = {'P','r','o','c','e','s','s'};

/* its own type (not process_type, so SeDebugPrivilege's rule for processes does not widen a handle beyond what the
 * owner granted), with the process type's name and mapping */
static struct type_descr rproc_type =
{
    { rproc_type_name, sizeof(rproc_type_name) },
    PROCESS_ALL_ACCESS,
    {
        STANDARD_RIGHTS_READ | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        STANDARD_RIGHTS_WRITE | PROCESS_SUSPEND_RESUME | PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA
        | PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
        | PROCESS_CREATE_THREAD,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
        PROCESS_ALL_ACCESS
    },
};

static void rproc_dump( struct object *obj, int verbose );
static struct object *rproc_get_sync( struct object *obj );
static unsigned int rproc_map_access( struct object *obj, unsigned int access );
static void rproc_destroy( struct object *obj );

static const struct object_ops rproc_ops =
{
    .size       = sizeof(struct x_rproc),
    .type       = &rproc_type,
    .dump       = rproc_dump,
    .get_sync   = rproc_get_sync,
    .map_access = rproc_map_access,
    .destroy    = rproc_destroy,
};

static void pcall_dump( struct object *obj, int verbose );
static struct object *pcall_get_sync( struct object *obj );
static void pcall_destroy( struct object *obj );

static const struct object_ops pcall_ops =
{
    .size     = sizeof(struct x_pcall),
    .type     = &no_type,
    .dump     = pcall_dump,
    .get_sync = pcall_get_sync,
    .destroy  = pcall_destroy,
};

static void rproc_dump( struct object *obj, int verbose )
{
    struct x_rproc *rproc = (struct x_rproc *)obj;
    fprintf( stderr, "Remote process pid=%04x ended=%d\n", rproc->info.pid, rproc->ended );
}

static struct object *rproc_get_sync( struct object *obj )
{
    struct x_rproc *rproc = (struct x_rproc *)obj;
    return grab_object( rproc->sync );
}

static unsigned int rproc_map_access( struct object *obj, unsigned int access )
{
    access = default_map_access( obj, access );
    if (access & PROCESS_QUERY_INFORMATION) access |= PROCESS_QUERY_LIMITED_INFORMATION;
    if (access & PROCESS_SET_INFORMATION) access |= PROCESS_SET_LIMITED_INFORMATION;
    return access;
}

static void x_ignore_reply( void *ctx, unsigned int status, const struct wex_x_header *reply, const void *body,
                            size_t body_size )
{
}

static void rproc_destroy( struct object *obj )
{
    struct x_rproc *rproc = (struct x_rproc *)obj;

    if (rproc->held)
    {
        /* the last handle here: the owner drops its entry (idempotent, so one that crosses a link loss is fine) */
        if (rproc->held->peer)
            x_call( rproc->held->peer, WEX_X_RELEASE, rproc->held->id, NULL, 0, NULL, 0, NULL, 0, 0,
                    x_ignore_reply, NULL );
        x_held_free( rproc->held );
    }
    if (rproc->sync) release_object( rproc->sync );
    free( rproc->image );
}

static void pcall_dump( struct object *obj, int verbose )
{
    struct x_pcall *call = (struct x_pcall *)obj;
    fprintf( stderr, "Remote process open status=%08x\n", call->status );
}

static struct object *pcall_get_sync( struct object *obj )
{
    struct x_pcall *call = (struct x_pcall *)obj;
    return grab_object( call->sync );
}

static void pcall_destroy( struct object *obj )
{
    struct x_pcall *call = (struct x_pcall *)obj;

    if (call->rproc) release_object( call->rproc );
    if (call->caller) release_object( call->caller );
    if (call->sync) release_object( call->sync );
}

static void rproc_set_ended( struct x_rproc *rproc )
{
    if (rproc->ended) return;
    rproc->ended = 1;
    signal_sync( rproc->sync );
}

/* the owner's information in an INFO reply; 0 when it is malformed */
static int rproc_take_info( struct x_rproc *rproc, const void *body, size_t size )
{
    struct wex_x_process_info info;
    WCHAR *image = NULL;

    if (size < sizeof(info)) return 0;
    memcpy( &info, body, sizeof(info) );
    if (info.image_len != size - sizeof(info) || info.image_len % sizeof(WCHAR)) return 0;
    if (info.image_len && !(image = memdup( (const char *)body + sizeof(info), info.image_len ))) return 0;
    free( rproc->image );
    rproc->image = image;
    rproc->info = info;
    return 1;
}

/* the INFO after the death notice: the exit code and end time, then the process is signaled */
static void rproc_refresh_done( void *ctx, unsigned int status, const struct wex_x_header *reply, const void *body,
                                size_t body_size )
{
    struct x_rproc *rproc = ctx;

    rproc->refreshing = 0;
    if (status || !rproc_take_info( rproc, body, body_size ) || rproc->info.running)
    {
        /* the owner cannot say (gone, or the reference was released): it has ended as far as session 0 knows */
        if (!rproc->info.end_time) rproc->info.end_time = current_time;
        if (rproc->info.running) rproc->info.exit_code = X_OWNER_GONE_EXIT;
        rproc->info.running = 0;
    }
    rproc_set_ended( rproc );
    release_object( rproc );
}

/* a NOTICE (the process ended: its readiness) or the owner's epoch ended (held->peer is NULL) */
static void rproc_notify( struct x_held *held, void *ctx )
{
    struct x_rproc *rproc = ctx;

    if (rproc->ended || rproc->refreshing) return;
    if (held->peer && !(rproc->granted & PROCESS_QUERY_LIMITED_INFORMATION))
    {
        rproc->info.running = 0;  /* a wait-only handle: no exit code to ask for */
        rproc_set_ended( rproc );
        return;
    }
    if (!held->peer)
    {
        /* M6: the owner's server is gone (its epoch ended), so the process is too */
        if (rproc->info.running)
        {
            rproc->info.exit_code = X_OWNER_GONE_EXIT;
            rproc->info.end_time = current_time;
            rproc->info.running = 0;
        }
        rproc_set_ended( rproc );
        return;
    }
    {
        struct wex_x_info_req req = { ProcessBasicInformation, 0 };

        grab_object( rproc );
        rproc->refreshing = 1;
        if (x_call( held->peer, WEX_X_INFO, held->id, &req, sizeof(req), NULL, 0, NULL, 0, 0,
                    rproc_refresh_done, rproc ) != STATUS_PENDING)
            rproc_refresh_done( rproc, STATUS_CONNECTION_DISCONNECTED, NULL, NULL, 0 );
    }
}

static void pcall_end( struct x_pcall *call, unsigned int status )
{
    call->status = status;
    signal_sync( call->sync );
}

/* the open has its information (or needs none): the death notice is armed, and the caller can take the handle */
static void pcall_opened( struct x_pcall *call )
{
    struct x_rproc *rproc = call->rproc;

    if (!rproc->info.running) rproc_set_ended( rproc );
    else if (rproc->held->peer && (rproc->granted & SYNCHRONIZE))
        /* armed now, and at once if it ended meanwhile (level-triggered) */
        x_call( rproc->held->peer, WEX_X_SUBSCRIBE, rproc->held->id, NULL, 0, NULL, 0, NULL, 0, 0,
                x_ignore_reply, NULL );
    pcall_end( call, STATUS_SUCCESS );
}

static void pcall_info_done( void *ctx, unsigned int status, const struct wex_x_header *reply, const void *body,
                             size_t body_size )
{
    struct x_pcall *call = ctx;

    if (!status && !rproc_take_info( call->rproc, body, body_size )) status = STATUS_INVALID_PARAMETER;
    if (!status) pcall_opened( call );
    else pcall_end( call, status );
    release_object( call );
}

static void pcall_open_done( void *ctx, unsigned int status, const struct wex_x_header *reply, const void *body,
                             size_t body_size )
{
    struct x_pcall *call = ctx;
    struct x_rproc *rproc = call->rproc;
    struct wex_x_granted granted;
    struct wex_x_info_req req = { ProcessBasicInformation, 0 };

    if (!status && (body_size != sizeof(granted) || !reply->ref)) status = STATUS_INVALID_PARAMETER;
    if (status)
    {
        pcall_end( call, status );
        release_object( call );
        return;
    }
    memcpy( &granted, body, sizeof(granted) );
    if (!(rproc->held = x_held_add( rproc->peer, reply->ref, granted.access )))
    {
        x_call( rproc->peer, WEX_X_RELEASE, reply->ref, NULL, 0, NULL, 0, NULL, 0, 0, x_ignore_reply, NULL );
        pcall_end( call, STATUS_NO_MEMORY );
        release_object( call );
        return;
    }
    rproc->held->notify = rproc_notify;
    rproc->held->ctx = rproc;
    /* masks are information: the owner keeps them; here they bound the handles session 0 makes (rproc's sd) */
    rproc->granted = granted.access;
    if (granted.ready) rproc->info.running = 0;  /* it had ended already */
    /* without the limited query right the handle can only wait (and nothing is asked of the owner) */
    if (!(granted.access & PROCESS_QUERY_LIMITED_INFORMATION))
    {
        pcall_opened( call );
        release_object( call );
        return;
    }
    if ((status = x_call( rproc->peer, WEX_X_INFO, reply->ref, &req, sizeof(req), NULL, 0, NULL, 0, 0,
                          pcall_info_done, call )) != STATUS_PENDING)
    {
        pcall_end( call, status );
        release_object( call );
    }
}

/* a descriptor that allows exactly the granted rights to everyone: a handle made later (DuplicateHandle) can be
 * narrower, never wider (a new open is the way to more rights) */
static int rproc_set_sd( struct x_rproc *rproc, unsigned int granted )
{
    struct security_descriptor *sd;
    struct acl *dacl;
    size_t sys_len = sid_len( &local_system_sid ), world_len = sid_len( &world_sid );
    size_t dacl_len = sizeof(*dacl) + sizeof(struct ace) + world_len;
    struct sid *sid;

    if (!(sd = mem_alloc( sizeof(*sd) + 2 * sys_len + dacl_len ))) return 0;
    sd->control   = SE_DACL_PRESENT;
    sd->owner_len = sys_len;
    sd->group_len = sys_len;
    sd->sacl_len  = 0;
    sd->dacl_len  = dacl_len;
    sid = (struct sid *)(sd + 1);
    sid = copy_sid( sid, &local_system_sid );
    copy_sid( sid, &local_system_sid );
    dacl = (struct acl *)((char *)(sd + 1) + 2 * sys_len);
    dacl->revision = ACL_REVISION;
    dacl->pad1     = 0;
    dacl->size     = dacl_len;
    dacl->count    = 1;
    dacl->pad2     = 0;
    set_ace( ace_first( dacl ), &world_sid, ACCESS_ALLOWED_ACE_TYPE, 0, granted );
    rproc->obj.sd = sd;
    return 1;
}

/* open_process in session 0: an id in a block another live server leased is that server's process. 0 when it is
 * not (the local path goes on); 1 when handled: STATUS_PENDING with the call in *handle (ntdll waits on it and takes
 * the result with x_process_result), or the error set */
int x_open_remote_process( process_id_t pid, unsigned int access, unsigned int attributes, obj_handle_t *handle )
{
    struct wex_x_open open;
    struct x_pcall *call;
    struct x_rproc *rproc;
    struct x_peer *peer;
    unsigned long long start;
    unsigned int server_pid, status;
    data_size_t desc_size;
    void *desc;

    if (!session0_server || !session0_id_owner( pid, &server_pid, &start )) return 0;
    if (!(peer = x_peer_of_server( server_pid, start )))
    {
        set_error( STATUS_INVALID_CID );  /* no link to its server: as before X3, it is not found */
        return 1;
    }
    if (!(desc = x_describe_caller( current, &desc_size, &status )))
    {
        set_error( status );
        return 1;
    }
    if (!(rproc = alloc_object( &rproc_ops )))
    {
        free( desc );
        return 1;
    }
    rproc->sync = NULL;
    rproc->held = NULL;
    rproc->peer = peer;
    memset( &rproc->info, 0, sizeof(rproc->info) );
    rproc->info.pid = pid;
    rproc->info.running = 1;
    rproc->image = NULL;
    rproc->granted = 0;
    rproc->ended = 0;
    rproc->refreshing = 0;
    if (!(call = alloc_object( &pcall_ops )))
    {
        release_object( rproc );
        free( desc );
        return 1;
    }
    call->rproc = rproc;
    call->caller = (struct process *)grab_object( current->process );
    call->status = STATUS_PENDING;
    call->access = access;
    call->attributes = attributes;
    if (!(rproc->sync = create_internal_sync( 1, 0 )) || !(call->sync = create_internal_sync( 1, 0 )))
    {
        release_object( call );
        free( desc );
        return 1;
    }
    open.kind = WEX_X_KIND_PROCESS;
    open.access = access;
    open.name_size = sizeof(pid);
    open.reserved = 0;
    grab_object( call );  /* for the call in flight */
    status = x_call( peer, WEX_X_OPEN, 0, &open, sizeof(open), desc, desc_size, &pid, sizeof(pid), 0,
                     pcall_open_done, call );
    free( desc );
    if (status != STATUS_PENDING)
    {
        release_object( call );  /* the call's reference */
        release_object( call );
        set_error( status );
        return 1;
    }
    if ((*handle = alloc_handle_no_access_check( current->process, call, SYNCHRONIZE, 0 )))
        set_error( STATUS_PENDING );
    release_object( call );
    return 1;
}

/* get_process_info, get_process_image_name and the others: a handle to a remote process. NULL with *remote 0 when
 * the handle is something else (the local path goes on, nothing set); NULL with *remote 1 and the error set (the
 * access check); else the object (a reference) */
struct x_rproc *x_remote_process_from_handle( obj_handle_t handle, unsigned int access, int *remote )
{
    struct object *obj;

    *remote = 0;
    if (!session0_server) return NULL;
    if (!(obj = get_handle_obj( current->process, handle, 0, NULL )))
    {
        clear_error();
        return NULL;
    }
    *remote = obj->ops == &rproc_ops;
    release_object( obj );
    if (!*remote) return NULL;
    return (struct x_rproc *)get_handle_obj( current->process, handle, access, &rproc_ops );
}

/* (the error set before is kept) */
int x_is_remote_process_handle( obj_handle_t handle )
{
    unsigned int error = get_error();
    struct x_rproc *rproc;
    int remote;

    if ((rproc = x_remote_process_from_handle( handle, 0, &remote ))) release_object( rproc );
    set_error( error );
    return remote;
}

void x_remote_process_info( struct x_rproc *rproc, struct get_process_info_reply *reply )
{
    reply->pid           = rproc->info.pid;
    reply->ppid          = rproc->info.ppid;
    reply->exit_code     = rproc->info.exit_code;  /* the owner's: STILL_ACTIVE while it runs */
    reply->priority      = rproc->info.priority;
    reply->base_priority = rproc->info.base_priority;
    reply->disable_boost = rproc->info.disable_boost;
    reply->affinity      = rproc->info.affinity;
    reply->peb           = rproc->info.peb;
    reply->start_time    = rproc->info.start_time;
    reply->end_time      = rproc->info.end_time;
    reply->session_id    = rproc->info.session_id;
    reply->machine       = rproc->info.machine;
    /* the image information (ProcessImageInformation) is not among the classes served */
    if (get_reply_max_size()) set_error( STATUS_NOT_SUPPORTED );
}

/* the image name, as get_process_image_name gives it; NULL with the error set */
const WCHAR *x_remote_process_image( struct x_rproc *rproc, data_size_t *len )
{
    if (!rproc->image)
    {
        set_error( STATUS_INVALID_HANDLE );
        return NULL;
    }
    *len = rproc->info.image_len;
    return rproc->image;
}

void x_process_init(void)
{
    x_register_kind( &process_kind );
}

/* the result of a remote OpenProcess that has ended (STATUS_PENDING while it has not): the handle */
DECL_HANDLER(x_process_result)
{
    struct x_pcall *call = (struct x_pcall *)get_handle_obj( current->process, req->call, 0, &pcall_ops );

    if (!call) return;
    if (call->caller != current->process) set_error( STATUS_ACCESS_DENIED );
    else if (call->status == STATUS_PENDING) set_error( STATUS_PENDING );
    else if (call->status) set_error( call->status );
    else if (!call->rproc->obj.sd && !rproc_set_sd( call->rproc, call->rproc->granted )) set_error( STATUS_NO_MEMORY );
    else reply->handle = alloc_handle_no_access_check( current->process, call->rproc, call->rproc->granted,
                                                       call->attributes );
    release_object( call );
}
