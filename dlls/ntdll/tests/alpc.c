/*
 * Advanced Local Procedure Call tests
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

#include <stdarg.h>
#include <windef.h>
#include <winternl.h>
#include <ntstatus.h>
#include "wine/test.h"

#define DECL_FUNCPTR(f) static typeof(f) *p##f;

DECL_FUNCPTR(AlpcGetHeaderSize)
DECL_FUNCPTR(AlpcGetMessageAttribute)
DECL_FUNCPTR(AlpcInitializeMessageAttribute)
DECL_FUNCPTR(NtAlpcAcceptConnectPort)
DECL_FUNCPTR(NtAlpcConnectPort)
DECL_FUNCPTR(NtAlpcCreatePort)
DECL_FUNCPTR(NtAlpcCreateSecurityContext)
DECL_FUNCPTR(NtAlpcImpersonateClientOfPort)
DECL_FUNCPTR(NtAlpcSendWaitReceivePort)

#undef DECL_FUNCPTR

static void init_functions(void)
{
    HMODULE ntdll;

    ntdll = GetModuleHandleA("ntdll.dll");
    ok(ntdll != NULL, "GetModuleHandleA failed.\n");

#define LOAD_FUNCPTR(f) p##f = (void *)GetProcAddress(ntdll, #f);

    LOAD_FUNCPTR(AlpcGetHeaderSize)
    LOAD_FUNCPTR(AlpcGetMessageAttribute)
    LOAD_FUNCPTR(AlpcInitializeMessageAttribute)
    LOAD_FUNCPTR(NtAlpcAcceptConnectPort)
    LOAD_FUNCPTR(NtAlpcConnectPort)
    LOAD_FUNCPTR(NtAlpcCreatePort)
    LOAD_FUNCPTR(NtAlpcCreateSecurityContext)
    LOAD_FUNCPTR(NtAlpcImpersonateClientOfPort)
    LOAD_FUNCPTR(NtAlpcSendWaitReceivePort)

#undef LOAD_FUNCPTR
}

static void init_port_attr(ALPC_PORT_ATTRIBUTES *attr, ULONG flags, SIZE_T max_msg_length)
{
    attr->Flags = flags;
    attr->SecurityQos.Length = sizeof(attr->SecurityQos);
    attr->SecurityQos.ImpersonationLevel = SecurityIdentification;
    attr->SecurityQos.ContextTrackingMode = SECURITY_STATIC_TRACKING;
    attr->SecurityQos.EffectiveOnly = FALSE;
    attr->MaxMessageLength = max_msg_length;
    attr->MemoryBandwidth = 512;
    attr->MaxPoolUsage = 0xffffffff;
    attr->MaxSectionSize = 0xffffffff;
    attr->MaxViewSize = 0xffffffff;
    attr->MaxTotalSectionSize = 0xffffffff;
    attr->DupObjectTypes = (flags & ALPC_PORTFLG_ALLOW_DUP_OBJECT) ? 0xffffffff : 0;
}

static void test_AlpcGetHeaderSize(void)
{
    unsigned int i, j;
    SIZE_T size;

    static const struct
    {
        ULONG attribute;
        SIZE_T size;
    }
    tests[] =
    {
        {0, 0},
        {1u << 1, 0},
        {1u << 2, 0},
        {1u << 3, 0},
        {1u << 4, 0},
        {1u << 5, 0},
        {1u << 6, 0},
        {1u << 7, 0},
        {1u << 8, 0},
        {1u << 9, 0},
        {1u << 10, 0},
        {1u << 11, 0},
        {1u << 12, 0},
        {1u << 13, 0},
        {1u << 14, 0},
        {1u << 15, 0},
        {1u << 16, 0},
        {1u << 17, 0},
        {1u << 18, 0},
        {1u << 19, 0},
        {1u << 20, 0},
        {1u << 21, 0},
        {1u << 22, 0},
        {1u << 23, 0},
        {1u << 24, 0},
        {1u << 25, sizeof(ALPC_WORK_ON_BEHALF_ATTR)},   /* ALPC_MESSAGE_WORK_ON_BEHALF_ATTRIBUTE */
        {1u << 26, sizeof(ALPC_DIRECT_ATTR)},           /* ALPC_MESSAGE_DIRECT_ATTRIBUTE */
        {1u << 27, sizeof(ALPC_TOKEN_ATTR)},            /* ALPC_MESSAGE_TOKEN_ATTRIBUTE */
        {1u << 28, sizeof(ALPC_HANDLE_ATTR)},           /* ALPC_MESSAGE_HANDLE_ATTRIBUTE */
        {1u << 29, sizeof(ALPC_CONTEXT_ATTR)},          /* ALPC_MESSAGE_CONTEXT_ATTRIBUTE */
        {1u << 30, sizeof(ALPC_VIEW_ATTR)},             /* ALPC_MESSAGE_VIEW_ATTRIBUTE */
        {1u << 31, sizeof(ALPC_SECURITY_ATTR)},         /* ALPC_MESSAGE_SECURITY_ATTRIBUTE */
        {0xffffffff, sizeof(ALPC_WORK_ON_BEHALF_ATTR) +
                     sizeof(ALPC_DIRECT_ATTR) +
                     sizeof(ALPC_TOKEN_ATTR) +
                     sizeof(ALPC_HANDLE_ATTR) +
                     sizeof(ALPC_CONTEXT_ATTR) +
                     sizeof(ALPC_VIEW_ATTR) +
                     sizeof(ALPC_SECURITY_ATTR)},       /* All attributes */
    };

    if (!pAlpcGetHeaderSize)
    {
        win_skip("AlpcGetHeaderSize is unavailable.\n");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        winetest_push_context("i %d", i);

        /* Single attribute */
        size = pAlpcGetHeaderSize(tests[i].attribute);
        ok(size == sizeof(ALPC_MESSAGE_ATTRIBUTES) + tests[i].size, "Got unexpected size %Ix.\n", size);

        /* Two attributes */
        for (j = 0; j < ARRAY_SIZE(tests); j++)
        {
            if (tests[i].attribute & tests[j].attribute)
                continue;

            winetest_push_context("j %d", j);

            size = pAlpcGetHeaderSize(tests[i].attribute | tests[j].attribute);
            ok(size == sizeof(ALPC_MESSAGE_ATTRIBUTES) + tests[i].size + tests[j].size,
               "Got unexpected size %Ix.\n", size);

            winetest_pop_context();
        }
        winetest_pop_context();
    }
}

static void test_AlpcGetMessageAttribute(void)
{
    unsigned char buffer[1024];
    ALPC_MESSAGE_ATTRIBUTES *attr = (ALPC_MESSAGE_ATTRIBUTES *)buffer;
    unsigned int i, j;
    void *ptr;

    static const ULONG attributes[] =
    {
        0,
        ALPC_MESSAGE_WORK_ON_BEHALF_ATTRIBUTE,
        ALPC_MESSAGE_DIRECT_ATTRIBUTE,
        ALPC_MESSAGE_TOKEN_ATTRIBUTE,
        ALPC_MESSAGE_HANDLE_ATTRIBUTE,
        ALPC_MESSAGE_CONTEXT_ATTRIBUTE,
        ALPC_MESSAGE_VIEW_ATTRIBUTE,
        ALPC_MESSAGE_SECURITY_ATTRIBUTE,
    };

    if (!pAlpcGetMessageAttribute)
    {
        win_skip("AlpcGetMessageAttribute is unavailable.\n");
        return;
    }

    /* Invalid flag */
    attr->AllocatedAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL;
    attr->ValidAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL;
    ptr = pAlpcGetMessageAttribute(attr, 0x1);
    ok(ptr == NULL, "Got unexpected ptr.\n");

    for (i = 0; i < ARRAY_SIZE(attributes); i++)
    {
        winetest_push_context("%#lx", attributes[i]);

        /* The message has no allocated attributes */
        attr->AllocatedAttributes = 0;
        attr->ValidAttributes = attributes[i];
        ptr = pAlpcGetMessageAttribute(attr, attributes[i]);
        ok(ptr == NULL, "Got unexpected ptr.\n");

        /* The message has no valid attributes */
        attr->AllocatedAttributes = attributes[i];
        attr->ValidAttributes = 0;
        ptr = pAlpcGetMessageAttribute(attr, attributes[i]);
        if (attributes[i])
            ok(ptr == ((unsigned char *)attr + sizeof(*attr)), "Got unexpected ptr.\n");
        else
            ok(ptr == NULL, "Got unexpected ptr.\n");

        /* Normal calls */
        attr->AllocatedAttributes = attributes[i];
        attr->ValidAttributes = attributes[i];
        ptr = pAlpcGetMessageAttribute(attr, attributes[i]);
        if (attributes[i])
            ok(ptr == ((unsigned char *)attr + sizeof(*attr)), "Got unexpected ptr.\n");
        else
            ok(ptr == NULL, "Got unexpected ptr.\n");

        /* The message has full attributes */
        attr->AllocatedAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL;
        attr->ValidAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL;
        ptr = pAlpcGetMessageAttribute(attr, attributes[i]);
        if (attributes[i])
            ok(ptr == ((unsigned char *)attr + pAlpcGetHeaderSize(attr->AllocatedAttributes & ~(attributes[i] | (attributes[i] - 1)))),
               "Got unexpected ptr.\n");
        else
            ok(ptr == NULL, "Got unexpected ptr.\n");

        /* The message has some valid attributes */
        for (j = 0; j < ARRAY_SIZE(attributes); j++)
        {
            attr->AllocatedAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL;
            attr->ValidAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL & ~attributes[j];
            ptr = pAlpcGetMessageAttribute(attr, attributes[i]);
            if (attributes[i])
                ok(ptr == ((unsigned char *)attr + pAlpcGetHeaderSize(attr->AllocatedAttributes & ~(attributes[i] | (attributes[i] - 1)))),
                   "Got unexpected ptr.\n");
            else
                ok(ptr == NULL, "Got unexpected ptr.\n");
        }

        /* Two attributes are passed to AlpcGetMessageAttribute() */
        for (j = 0; j < ARRAY_SIZE(attributes); j++)
        {
            if (!attributes[i] || !attributes[j] || attributes[i] == attributes[j])
                continue;

            attr->AllocatedAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL;
            attr->ValidAttributes = ALPC_MESSAGE_ATTRIBUTE_ALL;
            ptr = pAlpcGetMessageAttribute(attr, attributes[i] | attributes[j]);
            ok(ptr == NULL, "Got unexpected ptr.\n");
        }

        winetest_pop_context();
    }
}

static void test_AlpcInitializeMessageAttribute(void)
{
    unsigned char buffer[1024];
    ALPC_MESSAGE_ATTRIBUTES *attr = (ALPC_MESSAGE_ATTRIBUTES *)buffer;
    SIZE_T required_size, size;
    unsigned int i, j, k;
    NTSTATUS status;

    static const ULONG attributes[] =
    {
        0,
        ALPC_MESSAGE_WORK_ON_BEHALF_ATTRIBUTE,
        ALPC_MESSAGE_DIRECT_ATTRIBUTE,
        ALPC_MESSAGE_TOKEN_ATTRIBUTE,
        ALPC_MESSAGE_HANDLE_ATTRIBUTE,
        ALPC_MESSAGE_CONTEXT_ATTRIBUTE,
        ALPC_MESSAGE_VIEW_ATTRIBUTE,
        ALPC_MESSAGE_SECURITY_ATTRIBUTE,
        0xffffffff,
    };

    if (!pAlpcInitializeMessageAttribute)
    {
        win_skip("AlpcInitializeMessageAttribute is unavailable.\n");
        return;
    }

    /* Check parameters */
    for (i = 0; i < ARRAY_SIZE(attributes); i++)
    {
        winetest_push_context("i %d", i);

        size = pAlpcGetHeaderSize(attributes[i]);

        /* Null message pointer */
        required_size = 0;
        status = pAlpcInitializeMessageAttribute(attributes[i], NULL, size, &required_size);
        ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
        ok(required_size == size, "Expected %Ix, got %Ix.\n", size, required_size);

        /* Buffer too small */
        attr->AllocatedAttributes = 0xdeadbeef;
        attr->ValidAttributes = 0xdeadbeef;
        required_size = 0;
        status = pAlpcInitializeMessageAttribute(attributes[i], attr, size - 1, &required_size);
        ok(status == STATUS_BUFFER_TOO_SMALL, "Got unexpected status %#lx.\n", status);
        ok(attr->AllocatedAttributes == 0xdeadbeef, "Got unexpected %#lx.\n", attr->AllocatedAttributes);
        ok(attr->ValidAttributes == 0xdeadbeef, "Got unexpected %#lx.\n", attr->ValidAttributes);
        ok(required_size == size, "Expected %Ix, got %Ix.\n", size, required_size);

        /* Buffer too large */
        attr->AllocatedAttributes = 0xdeadbeef;
        attr->ValidAttributes = 0xdeadbeef;
        required_size = 0;
        status = pAlpcInitializeMessageAttribute(attributes[i], attr, size + 1, &required_size);
        ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
        ok(attr->AllocatedAttributes == attributes[i], "Got unexpected %#lx.\n", attr->AllocatedAttributes);
        ok(attr->ValidAttributes == 0, "Got unexpected %#lx.\n", attr->ValidAttributes);
        ok(required_size == size, "Expected %Ix, got %Ix.\n", size, required_size);

        /* Correct buffer size */
        memset(buffer, 0xa1, sizeof(buffer));
        required_size = 0;
        status = pAlpcInitializeMessageAttribute(attributes[i], attr, size, &required_size);
        ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
        ok(attr->AllocatedAttributes == attributes[i], "Got unexpected %#lx.\n", attr->AllocatedAttributes);
        ok(attr->ValidAttributes == 0, "Got unexpected %#lx.\n", attr->ValidAttributes);
        ok(required_size == size, "Expected %Ix, got %Ix.\n", size, required_size);
        /* Test that AlpcInitializeMessageAttribute() only sets AllocatedAttributes and ValidAttributes */
        for (k = sizeof(*attr); k < sizeof(buffer); k++)
        {
            if (buffer[k] != 0xa1)
            {
                ok(0, "Marker got overwritten at %d.\n", k);
                break;
            }
        }

        /* Two attributes */
        for (j = 0; j < ARRAY_SIZE(attributes); j++)
        {
            if (attributes[i] & attributes[j])
                continue;

            winetest_push_context("j %d", j);

            memset(buffer, 0xb2, sizeof(buffer));
            size = pAlpcGetHeaderSize(attributes[i] | attributes[j]);
            required_size = 0;
            status = pAlpcInitializeMessageAttribute(attributes[i] | attributes[j], attr, size, &required_size);
            ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
            ok(attr->AllocatedAttributes == (attributes[i] | attributes[j]),
               "Got unexpected %#lx.\n", attr->AllocatedAttributes);
            ok(attr->ValidAttributes == 0, "Got unexpected %#lx.\n", attr->ValidAttributes);
            ok(required_size == size, "Expected %Ix, got %Ix.\n", size, required_size);
            /* Test that AlpcInitializeMessageAttribute() only sets AllocatedAttributes and ValidAttributes */
            for (k = sizeof(*attr); k < sizeof(buffer); k++)
            {
                if (buffer[k] != 0xb2)
                {
                    ok(0, "Marker got overwritten at %d.\n", k);
                    break;
                }
            }

            winetest_pop_context();
        }
        winetest_pop_context();
    }
}

static void test_NtAlpcCreatePort(void)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\test_NtAlpcCreatePort_port");
    OBJECT_NAME_INFORMATION *name_info;
    OBJECT_TYPE_INFORMATION *type_info;
    ALPC_PORT_ATTRIBUTES port_attr;
    OBJECT_BASIC_INFORMATION info;
    unsigned char buffer[1024];
    HANDLE handle, handle2;
    OBJECT_ATTRIBUTES attr;
    DWORD size, flags;
    NTSTATUS status;
    ULONG flag;
    BOOL ret;

    if (!pNtAlpcCreatePort)
    {
        win_skip("NtAlpcCreatePort is unavailable.\n");
        return;
    }

    InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
    init_port_attr(&port_attr, 0, 1024);

    /* Check parameters */
    status = pNtAlpcCreatePort(NULL, &attr, &port_attr);
    ok(status == STATUS_ACCESS_VIOLATION, "Got unexpected status %#lx.\n", status);

    status = pNtAlpcCreatePort(&handle, NULL, &port_attr);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    ret = GetHandleInformation(handle, &flags);
    ok(ret, "GetHandleInformation failed, error %ld.\n", GetLastError());
    CloseHandle(handle);

    status = pNtAlpcCreatePort(&handle, &attr, NULL);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    ret = GetHandleInformation(handle, &flags);
    ok(ret, "GetHandleInformation failed, error %ld.\n", GetLastError());
    CloseHandle(handle);

    status = pNtAlpcCreatePort(&handle, NULL, NULL);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    ret = GetHandleInformation(handle, &flags);
    ok(ret, "GetHandleInformation failed, error %ld.\n", GetLastError());
    CloseHandle(handle);

    /* Normal calls */
    status = pNtAlpcCreatePort(&handle, &attr, &port_attr);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);

    /* Check handle */
    ret = GetHandleInformation(handle, &flags);
    ok(ret, "GetHandleInformation failed, error %ld.\n", GetLastError());

    /* Check object attributes and granted access */
    status = NtQueryObject(handle, ObjectBasicInformation, &info, sizeof(info), NULL);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    ok(info.Attributes == 0, "Got attributes %#lx\n", info.Attributes);
    ok(info.GrantedAccess == (STANDARD_RIGHTS_ALL | 0x1), "Got access %#lx\n", info.GrantedAccess);

    /* Check object name */
    status = NtQueryObject(handle, ObjectNameInformation, buffer, sizeof(buffer), &size);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    name_info = (OBJECT_NAME_INFORMATION *)buffer;
    ok(!wcsicmp(name_info->Name.Buffer, name.Buffer), "Got unexpected name %s.\n",
       debugstr_w(name_info->Name.Buffer));

    /* Check object type */
    status = NtQueryObject(handle, ObjectTypeInformation, buffer, sizeof(buffer), &size);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    type_info = (OBJECT_TYPE_INFORMATION *)buffer;
    ok(!wcscmp(type_info->TypeName.Buffer, L"ALPC Port"), "Got unexpected type %s.\n",
       debugstr_w(type_info->TypeName.Buffer));

    /* Duplicate name */
    status = pNtAlpcCreatePort(&handle2, &attr, &port_attr);
    ok(status == STATUS_OBJECT_NAME_COLLISION, "Got unexpected status %#lx.\n", status);

    CloseHandle(handle);

    /* Test handle validity after deletion */
    ret = GetHandleInformation(handle, &flags);
    ok(!ret, "GetHandleInformation succeeded.\n");

    /* Test Flags */
    for (flag = 0x1; flag != 0; flag = flag << 1)
    {
        winetest_push_context("%#lx", flag);

        port_attr.Flags = flag;
        status = pNtAlpcCreatePort(&handle, &attr, &port_attr);
        if (flag == 0x100000)
        {
            ok(status == STATUS_INVALID_PARAMETER, "Got unexpected status %#lx.\n", status);
            winetest_pop_context();
            continue;
        }
        else
        {
            ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
        }
        ret = GetHandleInformation(handle, &flags);
        ok(ret, "GetHandleInformation failed, error %ld.\n", GetLastError());
        status = NtQueryObject(handle, ObjectBasicInformation, &info, sizeof(info), NULL);
        ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
        ok(info.Attributes == 0, "Got attributes %#lx\n", info.Attributes);
        ok(info.GrantedAccess == (STANDARD_RIGHTS_ALL | 0x1), "Got access %#lx\n", info.GrantedAccess);

        CloseHandle(handle);
        winetest_pop_context();
    }
}

#define TEST_MSG_SIZE 0x200

struct test_msg
{
    ALPC_PORT_MESSAGE header;
    char data[TEST_MSG_SIZE - sizeof(ALPC_PORT_MESSAGE)];
};

static void init_test_msg(struct test_msg *msg, const char *data)
{
    memset(msg, 0, sizeof(*msg));
    msg->header.DataLength = strlen(data);
    msg->header.TotalLength = sizeof(ALPC_PORT_MESSAGE) + strlen(data);
    memcpy(msg->data, data, strlen(data));
}

static HANDLE test_conn_port;
static LONG test_closed_count;

static DWORD WINAPI test_alpc_server(void *arg)
{
    ALPC_PORT_ATTRIBUTES port_attr;
    HANDLE comm_port = NULL;
    BOOL quit = FALSE;
    unsigned int count;

    for (count = 0; !quit && count < 20; ++count)
    {
        char attr_buffer[256];
        ALPC_MESSAGE_ATTRIBUTES *attrs = (ALPC_MESSAGE_ATTRIBUTES *)attr_buffer;
        struct test_msg msg, reply;
        LARGE_INTEGER timeout;
        NTSTATUS status;
        SIZE_T size, req;

        memset(&msg, 0xcc, sizeof(msg));
        pAlpcInitializeMessageAttribute(ALPC_MESSAGE_CONTEXT_ATTRIBUTE, attrs, sizeof(attr_buffer), &req);
        size = sizeof(msg);
        timeout.QuadPart = -100000000;
        status = pNtAlpcSendWaitReceivePort(test_conn_port, 0, NULL, NULL, &msg.header, &size, attrs, &timeout);
        ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
        if (status) break;
        ok(size == sizeof(msg), "Got unexpected size %Iu.\n", size);

        switch (msg.header.Type & 0xff)
        {
            case 10: /* connection request */
                ok(msg.header.Type == (10 | 0x2000), "Got unexpected type %#x.\n", msg.header.Type);
                ok(msg.header.DataLength == 5 && !memcmp(msg.data, "hello", 5), "Got unexpected connection data.\n");
                ok(msg.header.TotalLength == sizeof(ALPC_PORT_MESSAGE) + 5, "Got unexpected total length %u.\n",
                        msg.header.TotalLength);
                ok(!attrs->ValidAttributes, "Got unexpected attributes %#lx.\n", attrs->ValidAttributes);

                init_port_attr(&port_attr, 0, 0x1000);
                init_test_msg(&reply, "ACCEPTED");
                reply.header.MessageId = msg.header.MessageId;
                status = pNtAlpcAcceptConnectPort(&comm_port, test_conn_port, 0, NULL, &port_attr, (void *)0x1234,
                        &reply.header, NULL, TRUE);
                ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
                break;

            case 1: /* request or datagram */
                ok(msg.header.Type == (1 | 0x2000), "Got unexpected type %#x.\n", msg.header.Type);
                ok(attrs->ValidAttributes == ALPC_MESSAGE_CONTEXT_ATTRIBUTE, "Got unexpected attributes %#lx.\n",
                        attrs->ValidAttributes);
                if (attrs->ValidAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE)
                {
                    ALPC_CONTEXT_ATTR *context = pAlpcGetMessageAttribute(attrs, ALPC_MESSAGE_CONTEXT_ATTRIBUTE);

                    ok(context->PortContext == (void *)0x1234, "Got unexpected port context %p.\n",
                            context->PortContext);
                    ok(!context->MessageContext, "Got unexpected message context %p.\n", context->MessageContext);
                    /* the message id of the context attribute is always 0 on 32-bit */
                    ok(context->MessageId == (sizeof(void *) == 8 ? msg.header.MessageId : 0),
                            "Got unexpected message id %lu.\n", context->MessageId);
                }
                if (msg.header.DataLength == 5 && !memcmp(msg.data, "quit.", 5))
                {
                    quit = TRUE;
                }
                else if (msg.header.DataLength == 5 && !memcmp(msg.data, "reque", 5))
                {
                    init_test_msg(&reply, "REPLY");
                    reply.header.ClientId = msg.header.ClientId;
                    reply.header.MessageId = msg.header.MessageId;
                    status = pNtAlpcSendWaitReceivePort(test_conn_port, 0x10000, &reply.header, NULL, NULL, NULL,
                            NULL, NULL);
                    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
                }
                break;

            case 5: /* port closed */
                ok(msg.header.Type == 5, "Got unexpected type %#x.\n", msg.header.Type);
                ok(msg.header.DataLength == 8, "Got unexpected data length %u.\n", msg.header.DataLength);
                InterlockedIncrement(&test_closed_count);
                break;

            default:
                ok(0, "Got unexpected message type %#x.\n", msg.header.Type);
                break;
        }
    }
    if (comm_port) CloseHandle(comm_port);
    return 0;
}

static void test_alpc_messages(void)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\test_alpc_messages_port");
    char attr_buffer[256];
    ALPC_MESSAGE_ATTRIBUTES *attrs = (ALPC_MESSAGE_ATTRIBUTES *)attr_buffer;
    ALPC_PORT_ATTRIBUTES port_attr;
    struct test_msg msg, reply;
    OBJECT_ATTRIBUTES attr;
    LARGE_INTEGER timeout;
    HANDLE client, thread;
    NTSTATUS status;
    SIZE_T size, req;

    if (!pNtAlpcConnectPort || !pNtAlpcAcceptConnectPort || !pNtAlpcSendWaitReceivePort)
    {
        win_skip("ALPC messaging is unavailable.\n");
        return;
    }

    InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
    init_port_attr(&port_attr, 0, 0x1000);
    status = pNtAlpcCreatePort(&test_conn_port, &attr, &port_attr);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    test_closed_count = 0;
    thread = CreateThread(NULL, 0, test_alpc_server, NULL, 0, NULL);

    /* the buffer for the answer to the connection is too small: the connection is closed again */
    init_test_msg(&msg, "hello");
    size = sizeof(ALPC_PORT_MESSAGE) + 5;
    client = (HANDLE)0xdeadbeef;
    timeout.QuadPart = -100000000;
    status = pNtAlpcConnectPort(&client, &name, NULL, &port_attr, ALPC_SYNC_CONNECTION, NULL, &msg.header, &size,
            NULL, NULL, &timeout);
    ok(status == STATUS_BUFFER_TOO_SMALL, "Got unexpected status %#lx.\n", status);
    ok(size == sizeof(ALPC_PORT_MESSAGE) + 8, "Got unexpected size %Iu.\n", size);
    ok(client == (HANDLE)0xdeadbeef, "Got unexpected handle %p.\n", client);

    init_test_msg(&msg, "hello");
    size = sizeof(msg);
    timeout.QuadPart = -100000000;
    status = pNtAlpcConnectPort(&client, &name, NULL, &port_attr, ALPC_SYNC_CONNECTION, NULL, &msg.header, &size,
            NULL, NULL, &timeout);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    ok(client != NULL, "Got unexpected handle %p.\n", client);
    ok(size == sizeof(ALPC_PORT_MESSAGE) + 8, "Got unexpected size %Iu.\n", size);
    ok(msg.header.Type == 0xb, "Got unexpected type %#x.\n", msg.header.Type);
    ok(msg.header.DataLength == 8 && !memcmp(msg.data, "ACCEPTED", 8), "Got unexpected connection answer.\n");

    /* a datagram */
    init_test_msg(&msg, "dgram");
    status = pNtAlpcSendWaitReceivePort(client, 0, &msg.header, NULL, NULL, NULL, NULL, NULL);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);

    /* a request, and its reply */
    init_test_msg(&msg, "reque");
    memset(&reply, 0xcc, sizeof(reply));
    pAlpcInitializeMessageAttribute(ALPC_MESSAGE_CONTEXT_ATTRIBUTE, attrs, sizeof(attr_buffer), &req);
    size = sizeof(reply);
    timeout.QuadPart = -100000000;
    status = pNtAlpcSendWaitReceivePort(client, ALPC_MSGFLG_SYNC_REQUEST, &msg.header, NULL, &reply.header, &size,
            attrs, &timeout);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    ok(reply.header.Type == 2, "Got unexpected type %#x.\n", reply.header.Type);
    ok(reply.header.DataLength == 5 && !memcmp(reply.data, "REPLY", 5), "Got unexpected reply.\n");
    ok(attrs->ValidAttributes == ALPC_MESSAGE_CONTEXT_ATTRIBUTE, "Got unexpected attributes %#lx.\n",
            attrs->ValidAttributes);

    /* nothing is waiting: the receive times out */
    memset(&reply, 0xcc, sizeof(reply));
    size = sizeof(reply);
    timeout.QuadPart = -1000000;
    status = pNtAlpcSendWaitReceivePort(client, 0, NULL, NULL, &reply.header, &size, NULL, &timeout);
    ok(status == STATUS_TIMEOUT, "Got unexpected status %#lx.\n", status);

    init_test_msg(&msg, "quit.");
    status = pNtAlpcSendWaitReceivePort(client, 0, &msg.header, NULL, NULL, NULL, NULL, NULL);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    CloseHandle(client);
    ok(test_closed_count == 1, "Got unexpected count %ld.\n", test_closed_count);
    CloseHandle(test_conn_port);
}



struct imp_results
{
    NTSTATUS recv, imp_connect, imp_forged, imp_request, imp_changed_id;
};

static struct imp_results imp_results;
static HANDLE imp_port;

static void revert_to_self(void)
{
    HANDLE token = NULL;

    NtSetInformationThread(GetCurrentThread(), ThreadImpersonationToken, &token, sizeof(token));
}

static DWORD WINAPI imp_server(void *arg)
{
    struct test_msg msg, copy, reply;
    LARGE_INTEGER timeout;
    HANDLE comm = NULL;
    ALPC_PORT_ATTRIBUTES port_attr;
    SIZE_T size;

    memset(&imp_results, 0, sizeof(imp_results));
    size = sizeof(msg);
    timeout.QuadPart = -50000000;
    imp_results.recv = pNtAlpcSendWaitReceivePort(imp_port, 0, NULL, NULL, &msg.header, &size, NULL, &timeout);
    if (imp_results.recv) return 0;

    imp_results.imp_connect = pNtAlpcImpersonateClientOfPort(imp_port, &msg.header, NULL);
    if (!imp_results.imp_connect) revert_to_self();

    init_port_attr(&port_attr, 0, 0x1000);
    init_test_msg(&reply, "OK");
    reply.header.MessageId = msg.header.MessageId;
    pNtAlpcAcceptConnectPort(&comm, imp_port, 0, NULL, &port_attr, NULL, &reply.header, NULL, TRUE);

    size = sizeof(msg);
    timeout.QuadPart = -50000000;
    imp_results.recv = pNtAlpcSendWaitReceivePort(imp_port, 0, NULL, NULL, &msg.header, &size, NULL, &timeout);
    if (imp_results.recv) return 0;

    copy = msg;
    copy.header.MessageId += 1000;
    imp_results.imp_forged = pNtAlpcImpersonateClientOfPort(imp_port, &copy.header, NULL);
    if (!imp_results.imp_forged) revert_to_self();

    imp_results.imp_request = pNtAlpcImpersonateClientOfPort(imp_port, &msg.header, NULL);
    if (!imp_results.imp_request) revert_to_self();

    /* the client id of the header is informational: the message id is what counts */
    copy = msg;
    copy.header.ClientId.UniqueProcess = (HANDLE)((ULONG_PTR)copy.header.ClientId.UniqueProcess + 4);
    copy.header.ClientId.UniqueThread = (HANDLE)((ULONG_PTR)copy.header.ClientId.UniqueThread + 4);
    imp_results.imp_changed_id = pNtAlpcImpersonateClientOfPort(imp_port, &copy.header, NULL);
    if (!imp_results.imp_changed_id) revert_to_self();

    if (comm) CloseHandle(comm);
    return 0;
}

/* one connection to a port with the given flags; the server thread tries to impersonate */
static void run_impersonation(const WCHAR *port_name, ULONG port_flags)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    ALPC_PORT_ATTRIBUTES port_attr;
    struct test_msg msg;
    LARGE_INTEGER timeout;
    HANDLE client = NULL, thread;
    NTSTATUS status;
    SIZE_T size;

    RtlInitUnicodeString(&name, port_name);
    InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
    init_port_attr(&port_attr, port_flags, 0x1000);
    status = pNtAlpcCreatePort(&imp_port, &attr, &port_attr);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    thread = CreateThread(NULL, 0, imp_server, NULL, 0, NULL);

    init_test_msg(&msg, "hi");
    size = sizeof(msg);
    init_port_attr(&port_attr, 0, 0x1000);
    timeout.QuadPart = -50000000;
    status = pNtAlpcConnectPort(&client, &name, NULL, &port_attr, ALPC_SYNC_CONNECTION, NULL, &msg.header, &size,
            NULL, NULL, &timeout);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    init_test_msg(&msg, "req");
    status = pNtAlpcSendWaitReceivePort(client, 0, &msg.header, NULL, NULL, NULL, NULL, NULL);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    if (client) CloseHandle(client);
    CloseHandle(imp_port);
}

static void test_alpc_security(void)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\test_alpc_security_port");
    SID_IDENTIFIER_AUTHORITY world_auth = {SECURITY_WORLD_SID_AUTHORITY};
    SECURITY_DESCRIPTOR sd;
    ALPC_PORT_ATTRIBUTES port_attr;
    OBJECT_ATTRIBUTES attr;
    struct test_msg msg;
    LARGE_INTEGER timeout;
    HANDLE port, client;
    NTSTATUS status;
    SIZE_T size;
    PSID world;
    ACL *acl;

    if (!pNtAlpcConnectPort || !pNtAlpcImpersonateClientOfPort)
    {
        win_skip("ALPC impersonation is unavailable.\n");
        return;
    }

    /* the server has to allow impersonation on its port */
    run_impersonation(L"\\BaseNamedObjects\\test_alpc_imp_denied_port", 0);
    ok(imp_results.imp_connect == STATUS_ACCESS_DENIED, "Got unexpected status %#lx.\n", imp_results.imp_connect);
    ok(imp_results.imp_forged == STATUS_INVALID_MESSAGE, "Got unexpected status %#lx.\n", imp_results.imp_forged);
    ok(imp_results.imp_request == STATUS_ACCESS_DENIED, "Got unexpected status %#lx.\n", imp_results.imp_request);
    ok(imp_results.imp_changed_id == STATUS_ACCESS_DENIED, "Got unexpected status %#lx.\n", imp_results.imp_changed_id);

    run_impersonation(L"\\BaseNamedObjects\\test_alpc_imp_allowed_port", ALPC_PORTFLG_ALLOWIMPERSONATION);
    ok(imp_results.imp_connect == STATUS_SUCCESS, "Got unexpected status %#lx.\n", imp_results.imp_connect);
    ok(imp_results.imp_forged == STATUS_INVALID_MESSAGE, "Got unexpected status %#lx.\n", imp_results.imp_forged);
    ok(imp_results.imp_request == STATUS_SUCCESS, "Got unexpected status %#lx.\n", imp_results.imp_request);
    ok(imp_results.imp_changed_id == STATUS_SUCCESS, "Got unexpected status %#lx.\n", imp_results.imp_changed_id);

    /* a port whose security descriptor denies everybody cannot be connected to */
    RtlAllocateAndInitializeSid(&world_auth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world);
    acl = HeapAlloc(GetProcessHeap(), 0, 256);
    RtlCreateAcl(acl, 256, ACL_REVISION);
    RtlAddAccessDeniedAce(acl, ACL_REVISION, GENERIC_ALL, world);
    RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetDaclSecurityDescriptor(&sd, TRUE, acl, FALSE);
    InitializeObjectAttributes(&attr, &name, 0, NULL, &sd);
    init_port_attr(&port_attr, 0, 0x1000);
    status = pNtAlpcCreatePort(&port, &attr, &port_attr);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);

    init_test_msg(&msg, "hi");
    size = sizeof(msg);
    client = (HANDLE)0xdeadbeef;
    timeout.QuadPart = -10000000;
    status = pNtAlpcConnectPort(&client, &name, NULL, &port_attr, ALPC_SYNC_CONNECTION, NULL, &msg.header, &size,
            NULL, NULL, &timeout);
    ok(status == STATUS_ACCESS_DENIED, "Got unexpected status %#lx.\n", status);
    ok(client == (HANDLE)0xdeadbeef, "Got unexpected handle %p.\n", client);

    CloseHandle(port);
    RtlFreeSid(world);
    HeapFree(GetProcessHeap(), 0, acl);
}

struct tok_results
{
    NTSTATUS recv, imp, open;
    DWORD restricted, level;
};

static struct tok_results tok_results;
static HANDLE tok_port, tok_sent;

static DWORD WINAPI tok_server(void *arg)
{
    struct test_msg msg, reply;
    ALPC_PORT_ATTRIBUTES port_attr;
    LARGE_INTEGER timeout;
    HANDLE comm = NULL, token;
    ULONG len;
    SIZE_T size;

    memset(&tok_results, 0, sizeof(tok_results));
    size = sizeof(msg);
    timeout.QuadPart = -50000000;
    tok_results.recv = pNtAlpcSendWaitReceivePort(tok_port, 0, NULL, NULL, &msg.header, &size, NULL, &timeout);
    if (tok_results.recv) return 0;
    init_port_attr(&port_attr, 0, 0x1000);
    init_test_msg(&reply, "OK");
    reply.header.MessageId = msg.header.MessageId;
    pNtAlpcAcceptConnectPort(&comm, tok_port, 0, NULL, &port_attr, NULL, &reply.header, NULL, TRUE);

    size = sizeof(msg);
    timeout.QuadPart = -50000000;
    tok_results.recv = pNtAlpcSendWaitReceivePort(tok_port, 0, NULL, NULL, &msg.header, &size, NULL, &timeout);
    if (tok_results.recv) return 0;

    /* the client has changed its own token since it sent the message */
    WaitForSingleObject(tok_sent, 10000);

    tok_results.imp = pNtAlpcImpersonateClientOfPort(tok_port, &msg.header, NULL);
    tok_results.open = NtOpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
    if (!tok_results.open)
    {
        NtQueryInformationToken(token, TokenIsRestricted, &tok_results.restricted, sizeof(DWORD), &len);
        NtQueryInformationToken(token, TokenImpersonationLevel, &tok_results.level, sizeof(DWORD), &len);
        CloseHandle(token);
    }
    revert_to_self();
    if (comm) CloseHandle(comm);
    return 0;
}

/* the client sends a request with a security attribute of the given level (-1: none) while it impersonates a restricted token */
static void run_token_capture(const WCHAR *port_name, int level, HANDLE restricted_token)
{
    char attr_buffer[128];
    ALPC_MESSAGE_ATTRIBUTES *attrs = (ALPC_MESSAGE_ATTRIBUTES *)attr_buffer;
    SECURITY_QUALITY_OF_SERVICE qos = {sizeof(qos), level < 0 ? SecurityImpersonation : level, SECURITY_STATIC_TRACKING, TRUE};
    ALPC_PORT_ATTRIBUTES port_attr;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name;
    struct test_msg msg;
    LARGE_INTEGER timeout;
    HANDLE client = NULL, thread, none = NULL;
    NTSTATUS status;
    SIZE_T size, req;

    RtlInitUnicodeString(&name, port_name);
    InitializeObjectAttributes(&attr, &name, 0, NULL, NULL);
    init_port_attr(&port_attr, ALPC_PORTFLG_ALLOWIMPERSONATION, 0x1000);
    status = pNtAlpcCreatePort(&tok_port, &attr, &port_attr);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    tok_sent = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, tok_server, NULL, 0, NULL);

    init_test_msg(&msg, "hi");
    size = sizeof(msg);
    init_port_attr(&port_attr, 0, 0x1000);
    timeout.QuadPart = -50000000;
    status = pNtAlpcConnectPort(&client, &name, NULL, &port_attr, ALPC_SYNC_CONNECTION, NULL, &msg.header, &size,
            NULL, NULL, &timeout);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);

    status = NtSetInformationThread(GetCurrentThread(), ThreadImpersonationToken, &restricted_token, sizeof(restricted_token));
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    init_test_msg(&msg, "req");
    if (level >= 0)
    {
        ALPC_SECURITY_ATTR *sec;

        pAlpcInitializeMessageAttribute(ALPC_MESSAGE_SECURITY_ATTRIBUTE, attrs, sizeof(attr_buffer), &req);
        sec = pAlpcGetMessageAttribute(attrs, ALPC_MESSAGE_SECURITY_ATTRIBUTE);
        sec->Flags = 0;
        sec->QoS = &qos;
        sec->ContextHandle = NULL;
        status = pNtAlpcCreateSecurityContext(client, 0, sec);
        ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
        attrs->ValidAttributes = ALPC_MESSAGE_SECURITY_ATTRIBUTE;
        status = pNtAlpcSendWaitReceivePort(client, 0, &msg.header, attrs, NULL, NULL, NULL, NULL);
    }
    else
    {
        status = pNtAlpcSendWaitReceivePort(client, 0, &msg.header, NULL, NULL, NULL, NULL, NULL);
    }
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    NtSetInformationThread(GetCurrentThread(), ThreadImpersonationToken, &none, sizeof(none));
    SetEvent(tok_sent);

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    CloseHandle(tok_sent);
    if (client) CloseHandle(client);
    CloseHandle(tok_port);
}

static void test_alpc_impersonation_token(void)
{
    SID_IDENTIFIER_AUTHORITY world_auth = {SECURITY_WORLD_SID_AUTHORITY};
    SECURITY_QUALITY_OF_SERVICE qos = {sizeof(qos), SecurityImpersonation, SECURITY_STATIC_TRACKING, TRUE};
    HANDLE process_token, filtered, restricted;
    OBJECT_ATTRIBUTES attr;
    TOKEN_GROUPS *groups;
    NTSTATUS status;
    PSID world;

    if (!pNtAlpcConnectPort || !pNtAlpcImpersonateClientOfPort || !pNtAlpcCreateSecurityContext)
    {
        win_skip("ALPC security contexts are unavailable.\n");
        return;
    }

    status = NtOpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process_token);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);
    RtlAllocateAndInitializeSid(&world_auth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world);
    groups = HeapAlloc(GetProcessHeap(), 0, sizeof(*groups));
    groups->GroupCount = 1;
    groups->Groups[0].Sid = world;
    groups->Groups[0].Attributes = 0;
    status = NtFilterToken(process_token, 0, NULL, NULL, groups, &filtered);
    if (status)
    {
        skip("NtFilterToken failed, status %#lx.\n", status);
        goto done;
    }
    InitializeObjectAttributes(&attr, NULL, 0, NULL, NULL);
    attr.SecurityQualityOfService = &qos;
    status = NtDuplicateToken(filtered, TOKEN_ALL_ACCESS, &attr, FALSE, TokenImpersonation, &restricted);
    ok(status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status);

    /* the token that is impersonated is the one the sender had when it sent the message, at the level it gave */
    run_token_capture(L"\\BaseNamedObjects\\test_alpc_token_2_port", 2, restricted);
    ok(tok_results.imp == STATUS_SUCCESS, "Got unexpected status %#lx.\n", tok_results.imp);
    ok(tok_results.open == STATUS_SUCCESS, "Got unexpected status %#lx.\n", tok_results.open);
    ok(tok_results.restricted == 1, "Got unexpected restricted %lu.\n", tok_results.restricted);
    ok(tok_results.level == SecurityImpersonation, "Got unexpected level %lu.\n", tok_results.level);

    run_token_capture(L"\\BaseNamedObjects\\test_alpc_token_1_port", 1, restricted);
    ok(tok_results.imp == STATUS_SUCCESS, "Got unexpected status %#lx.\n", tok_results.imp);
    ok(tok_results.open == STATUS_SUCCESS, "Got unexpected status %#lx.\n", tok_results.open);
    ok(tok_results.restricted == 1, "Got unexpected restricted %lu.\n", tok_results.restricted);
    ok(tok_results.level == SecurityIdentification, "Got unexpected level %lu.\n", tok_results.level);

    /* without a security attribute the call succeeds and gives the thread no token */
    run_token_capture(L"\\BaseNamedObjects\\test_alpc_token_none_port", -1, restricted);
    ok(tok_results.imp == STATUS_SUCCESS, "Got unexpected status %#lx.\n", tok_results.imp);
    ok(tok_results.open == STATUS_NO_TOKEN, "Got unexpected status %#lx.\n", tok_results.open);

    CloseHandle(restricted);
    CloseHandle(filtered);
done:
    HeapFree(GetProcessHeap(), 0, groups);
    RtlFreeSid(world);
    CloseHandle(process_token);
}

START_TEST(alpc)
{
    init_functions();

    test_AlpcGetHeaderSize();
    test_AlpcGetMessageAttribute();
    test_AlpcInitializeMessageAttribute();
    test_NtAlpcCreatePort();
    test_alpc_messages();
    test_alpc_security();
    test_alpc_impersonation_token();
}
