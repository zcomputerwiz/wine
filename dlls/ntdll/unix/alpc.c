/*
 * Advanced Local Procedure Call
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
 */

#if 0
#pragma makedep unix
#endif

#include "ntstatus.h"
#include "wine/debug.h"
#include "wine/server.h"
#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(alpc);

#define ALPC_MSGFLG_REPLY 0x10000

struct alpc_result
{
    unsigned int type, id, callback_id, pid, tid, status, data_len, sequence, sent_id;
    int has_msg, has_context;
    ULONG_PTR port_context;
};

/* one request to the server: send a message (data after the header) and/or take one from the queue */
static NTSTATUS server_send_receive( HANDLE port, ULONG flags, const ALPC_PORT_MESSAGE *send_msg, int sec_level,
                                     void *recv_data, ULONG max_recv, int has_recv, struct alpc_result *r )
{
    NTSTATUS status;

    memset( r, 0, sizeof(*r) );
    SERVER_START_REQ( alpc_send_receive )
    {
        req->handle   = wine_server_obj_handle( port );
        req->flags    = flags;
        req->has_send = send_msg != NULL;
        req->has_recv = has_recv;
        req->max_recv = max_recv;
        req->sec_level = sec_level;
        if (send_msg)
        {
            req->msg_id = send_msg->MessageId;
            if (send_msg->DataLength) wine_server_add_data( req, send_msg + 1, send_msg->DataLength );
        }
        if (has_recv && max_recv) wine_server_set_reply( req, recv_data, max_recv );
        status = wine_server_call( req );
        r->has_msg      = reply->has_msg;
        r->type         = reply->type;
        r->id           = reply->msg_id;
        r->callback_id  = reply->callback_id;
        r->pid          = reply->pid;
        r->tid          = reply->tid;
        r->status       = reply->status;
        r->data_len     = reply->data_len;
        r->sequence     = reply->sequence;
        r->has_context  = reply->has_context;
        r->port_context = reply->port_context;
        r->sent_id      = reply->sent_id;
    }
    SERVER_END_REQ;
    return status;
}

static void fill_header( ALPC_PORT_MESSAGE *msg, const struct alpc_result *r )
{
    memset( msg, 0, sizeof(*msg) );
    msg->DataLength = r->data_len;
    msg->TotalLength = sizeof(*msg) + r->data_len;
    msg->Type = r->type;
    msg->ClientId.UniqueProcess = ULongToHandle( r->pid );
    msg->ClientId.UniqueThread = ULongToHandle( r->tid );
    msg->MessageId = r->id;
    msg->CallbackId = r->callback_id;
}

/* the attributes are stored in the order of their bits, the highest first */
static void *get_attribute( ALPC_MESSAGE_ATTRIBUTES *attrs, ULONG flag )
{
    static const struct { ULONG flag; SIZE_T size; } sizes[] =
    {
        { ALPC_MESSAGE_SECURITY_ATTRIBUTE, sizeof(ALPC_SECURITY_ATTR) },
        { ALPC_MESSAGE_VIEW_ATTRIBUTE, sizeof(ALPC_VIEW_ATTR) },
        { ALPC_MESSAGE_CONTEXT_ATTRIBUTE, sizeof(ALPC_CONTEXT_ATTR) },
        { ALPC_MESSAGE_HANDLE_ATTRIBUTE, sizeof(ALPC_HANDLE_ATTR) },
        { ALPC_MESSAGE_TOKEN_ATTRIBUTE, sizeof(ALPC_TOKEN_ATTR) },
        { ALPC_MESSAGE_DIRECT_ATTRIBUTE, sizeof(ALPC_DIRECT_ATTR) },
        { ALPC_MESSAGE_WORK_ON_BEHALF_ATTRIBUTE, sizeof(ALPC_WORK_ON_BEHALF_ATTR) },
    };
    ULONG above = attrs->AllocatedAttributes & ~(flag | (flag - 1));
    SIZE_T offset = sizeof(*attrs);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(sizes); i++) if (above & sizes[i].flag) offset += sizes[i].size;
    return (char *)attrs + offset;
}

/* the impersonation level of the security attribute of a message that is sent, -1 if it has none */
static int get_security_level( ALPC_MESSAGE_ATTRIBUTES *attrs )
{
    ALPC_SECURITY_ATTR *sec;

    if (!attrs || !(attrs->ValidAttributes & ALPC_MESSAGE_SECURITY_ATTRIBUTE) ||
        !(attrs->AllocatedAttributes & ALPC_MESSAGE_SECURITY_ATTRIBUTE))
        return -1;
    sec = get_attribute( attrs, ALPC_MESSAGE_SECURITY_ATTRIBUTE );
    return sec->QoS ? sec->QoS->ImpersonationLevel : SecurityImpersonation;
}

static void fill_attributes( ALPC_MESSAGE_ATTRIBUTES *attrs, const struct alpc_result *r )
{
    if (!attrs) return;
    attrs->ValidAttributes = 0;
    if (r->has_msg && (attrs->AllocatedAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE))
    {
        ALPC_CONTEXT_ATTR *ctx = get_attribute( attrs, ALPC_MESSAGE_CONTEXT_ATTRIBUTE );

        /* Windows 11 fills the structure even where the attribute is not marked valid (a connection request, a datagram
         * a server sent to its own connection port), with a null port context: a program that reads it unchecked sees no
         * stale value */
        ctx->PortContext = r->has_context ? (void *)r->port_context : NULL;
        ctx->MessageContext = NULL;
        ctx->Sequence = r->sequence;
        ctx->MessageId = r->id;
        ctx->CallbackId = r->callback_id;
        if (r->has_context) attrs->ValidAttributes |= ALPC_MESSAGE_CONTEXT_ATTRIBUTE;
    }
}

/* a relative timeout is turned into an absolute one, so that repeated waits keep one deadline */
static const LARGE_INTEGER *absolute_timeout( const LARGE_INTEGER *timeout, LARGE_INTEGER *buf )
{
    if (!timeout) return NULL;
    if (timeout->QuadPart >= 0) return timeout;
    NtQuerySystemTime( buf );
    buf->QuadPart -= timeout->QuadPart;
    return buf;
}

/* send (the first time) and receive; wait for a message to arrive while the queue is empty */
static NTSTATUS send_wait_receive( HANDLE port, ULONG flags, const ALPC_PORT_MESSAGE *send_msg, int sec_level,
                                   void *recv_data, ULONG max_recv, int has_recv, const LARGE_INTEGER *timeout,
                                   struct alpc_result *r )
{
    LARGE_INTEGER abs_buf;
    const LARGE_INTEGER *deadline = absolute_timeout( timeout, &abs_buf );
    NTSTATUS status = server_send_receive( port, flags, send_msg, sec_level, recv_data, max_recv, has_recv, r );

    while (has_recv && status == STATUS_PENDING)
    {
        if ((status = NtWaitForSingleObject( port, FALSE, deadline )) != STATUS_WAIT_0) return status;
        status = server_send_receive( port, flags & ~ALPC_MSGFLG_REPLY, NULL, -1, recv_data, max_recv, has_recv, r );
    }
    return status;
}

NTSTATUS WINAPI NtAlpcAcceptConnectPort( HANDLE *communication_port, HANDLE connection_port,
                                         DWORD flags, OBJECT_ATTRIBUTES *obj_attr,
                                         ALPC_PORT_ATTRIBUTES *port_attr, void *port_context,
                                         ALPC_PORT_MESSAGE *send_msg,
                                         ALPC_MESSAGE_ATTRIBUTES *send_msg_attr, BOOLEAN accept )
{
    NTSTATUS status;

    TRACE( "%p, %p, %#x, %p, %p, %p, %p, %p, %d\n", communication_port, connection_port,
           (unsigned int)flags, obj_attr, port_attr, port_context, send_msg, send_msg_attr, accept );

    if (!send_msg) return STATUS_INVALID_PARAMETER;
    if (accept && !communication_port) return STATUS_ACCESS_VIOLATION;

    SERVER_START_REQ( alpc_accept )
    {
        req->handle  = wine_server_obj_handle( connection_port );
        req->msg_id  = send_msg->MessageId;
        req->accept  = accept;
        req->context = (ULONG_PTR)port_context;
        if (send_msg->DataLength) wine_server_add_data( req, send_msg + 1, send_msg->DataLength );
        if (!(status = wine_server_call( req )) && accept)
            *communication_port = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return status;
}

NTSTATUS WINAPI NtAlpcConnectPort( HANDLE *port_handle, UNICODE_STRING *port_name,
                                   OBJECT_ATTRIBUTES *obj_attr, ALPC_PORT_ATTRIBUTES *port_attr,
                                   DWORD flags, PSID required_server_sid,
                                   ALPC_PORT_MESSAGE *connect_msg, SIZE_T *connect_msg_size,
                                   ALPC_MESSAGE_ATTRIBUTES *send_msg_attr,
                                   ALPC_MESSAGE_ATTRIBUTES *recv_msg_attr, LARGE_INTEGER *timeout )
{
    struct object_attributes *objattr;
    OBJECT_ATTRIBUTES attr;
    struct alpc_result r;
    unsigned char local[0x1000];
    ALPC_PORT_MESSAGE *reply_msg = connect_msg;
    void *reply_data = local;
    ULONG max_reply = sizeof(local);
    HANDLE handle = 0;
    data_size_t len;
    NTSTATUS status;

    TRACE( "%p, %s, %p, %p, %#x, %p, %p, %p, %p, %p, %p\n", port_handle, debugstr_us( port_name ), obj_attr,
           port_attr, (unsigned int)flags, required_server_sid, connect_msg, connect_msg_size, send_msg_attr,
           recv_msg_attr, timeout );

    if (!port_handle) return STATUS_ACCESS_VIOLATION;

    if (obj_attr) attr = *obj_attr;
    else InitializeObjectAttributes( &attr, port_name, 0, 0, NULL );
    if (!attr.ObjectName) attr.ObjectName = port_name;
    if (!attr.ObjectName) return STATUS_INVALID_PARAMETER;
    if ((status = wine_server_alloc_object_attributes( &attr, &objattr, &len ))) return status;

    SERVER_START_REQ( alpc_connect )
    {
        req->flags = flags;
        req->port_flags = port_attr ? port_attr->Flags : 0;
        req->sec_level = get_security_level( send_msg_attr );
        wine_server_add_data( req, objattr, len );
        if (connect_msg && connect_msg->DataLength) wine_server_add_data( req, connect_msg + 1, connect_msg->DataLength );
        if (!(status = wine_server_call( req ))) handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    free( objattr );
    if (status) return status;

    if (connect_msg && connect_msg_size && *connect_msg_size >= sizeof(*connect_msg))
    {
        reply_data = connect_msg + 1;
        max_reply = *connect_msg_size - sizeof(*connect_msg);
    }
    else reply_msg = NULL;

    status = send_wait_receive( handle, 0, NULL, -1, reply_data, max_reply, TRUE, timeout, &r );
    if (status == STATUS_BUFFER_TOO_SMALL && connect_msg_size) *connect_msg_size = sizeof(*connect_msg) + r.data_len;
    if (!status && r.status) status = r.status;
    if (status)
    {
        NtClose( handle );
        return status;
    }

    if (reply_msg)
    {
        fill_header( reply_msg, &r );
        *connect_msg_size = reply_msg->TotalLength;
    }
    fill_attributes( recv_msg_attr, &r );
    *port_handle = handle;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtAlpcCreatePort( HANDLE *port_handle, OBJECT_ATTRIBUTES *attr, ALPC_PORT_ATTRIBUTES *port_attr )
{
    struct object_attributes *objattr = NULL;
    unsigned int status;
    data_size_t len = 0;

    TRACE( "%p, %p, %p.\n", port_handle, attr, port_attr );

    if (!port_handle) return STATUS_ACCESS_VIOLATION;

    *port_handle = NULL;

    if (port_attr)
        TRACE( "port attributes: flags %#x qos (length %#x impersonation_level %d tracking_mode %d "
               "effective only %d) max_msg_length %#lx memory_bandwidth %#lx max_pool_usage %#lx "
               "max_section_size %#lx max_view_size %#lx max_total_section_size %#lx.\n",
               port_attr->Flags, port_attr->SecurityQos.Length, port_attr->SecurityQos.ImpersonationLevel,
               port_attr->SecurityQos.ContextTrackingMode, port_attr->SecurityQos.EffectiveOnly,
               port_attr->MaxMessageLength, port_attr->MemoryBandwidth, port_attr->MaxPoolUsage,
               port_attr->MaxSectionSize, port_attr->MaxViewSize, port_attr->MaxTotalSectionSize );

    if (port_attr && port_attr->Flags & 0x100000) return STATUS_INVALID_PARAMETER;

    if (attr)
    {
        if (attr->ObjectName) TRACE( "name %s.\n", debugstr_us( attr->ObjectName ) );
        if ((status = wine_server_alloc_object_attributes( attr, &objattr, &len ))) return status;
    }

    SERVER_START_REQ( alpc_create_port )
    {
        if (port_attr)
        {
            req->flags = port_attr->Flags;
            req->max_msg_len = port_attr->MaxMessageLength;
        }
        else
        {
            req->flags = 0;
            req->max_msg_len = 65535;
        }
        wine_server_add_data( req, objattr, len );
        if (!(status = wine_server_call( req )))
        {
            *port_handle = wine_server_ptr_handle( reply->handle );
            TRACE( "created %p.\n", *port_handle );
        }
        else
        {
            WARN( "status %#x.\n", status );
        }
    }
    SERVER_END_REQ;
    free( objattr );
    return status;
}

NTSTATUS WINAPI NtAlpcDisconnectPort( HANDLE port_handle, ULONG flags )
{
    NTSTATUS status;

    TRACE( "%p, %#x\n", port_handle, (unsigned int)flags );

    SERVER_START_REQ( alpc_disconnect )
    {
        req->handle = wine_server_obj_handle( port_handle );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    return status;
}

NTSTATUS WINAPI NtAlpcSendWaitReceivePort( HANDLE port_handle, ULONG flags,
                                           ALPC_PORT_MESSAGE *send_msg,
                                           ALPC_MESSAGE_ATTRIBUTES *send_msg_attr,
                                           ALPC_PORT_MESSAGE *recv_msg, SIZE_T *recv_buffer_size,
                                           ALPC_MESSAGE_ATTRIBUTES *recv_msg_attr,
                                           LARGE_INTEGER *timeout )
{
    struct alpc_result r;
    void *recv_data = NULL;
    ULONG max_recv = 0;
    NTSTATUS status;

    TRACE( "%p, %#x, %p, %p, %p, %p, %p, %p\n", port_handle, (unsigned int)flags, send_msg,
           send_msg_attr, recv_msg, recv_buffer_size, recv_msg_attr, timeout );

    if (!send_msg && !recv_msg) return STATUS_INVALID_PARAMETER;
    if (recv_msg)
    {
        if (!recv_buffer_size) return STATUS_ACCESS_VIOLATION;
        if (*recv_buffer_size >= sizeof(*recv_msg))
        {
            recv_data = recv_msg + 1;
            max_recv = *recv_buffer_size - sizeof(*recv_msg);
        }
    }

    status = send_wait_receive( port_handle, flags, send_msg, get_security_level( send_msg_attr ), recv_data, max_recv,
                                recv_msg != NULL, timeout, &r );
    TRACE( "-> %#x send id %u cb %u type %#x len %u, received id %u cb %u type %#x len %u\n", (unsigned int)status,
           send_msg ? (unsigned int)send_msg->MessageId : 0, send_msg ? (unsigned int)send_msg->CallbackId : 0,
           send_msg ? (unsigned int)send_msg->Type : 0, send_msg ? (unsigned int)send_msg->DataLength : 0,
           r.id, r.callback_id, r.type, r.data_len );
    if (recv_msg)
    {
        if (status == STATUS_BUFFER_TOO_SMALL || *recv_buffer_size < sizeof(*recv_msg))
            *recv_buffer_size = sizeof(*recv_msg) + r.data_len;
        else if (!status && r.has_msg) fill_header( recv_msg, &r );
        else if (status == STATUS_TIMEOUT) memset( recv_msg, 0, sizeof(*recv_msg) );
        if (!status) fill_attributes( recv_msg_attr, &r );
    }
    return status;
}

NTSTATUS WINAPI NtAlpcImpersonateClientOfPort( HANDLE port_handle, ALPC_PORT_MESSAGE *msg, void *reserved )
{
    NTSTATUS status;

    TRACE( "%p, %p, %p\n", port_handle, msg, reserved );

    if (!msg) return STATUS_INVALID_PARAMETER;

    SERVER_START_REQ( alpc_impersonate )
    {
        req->handle = wine_server_obj_handle( port_handle );
        req->msg_id = msg->MessageId;
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    return status;
}
