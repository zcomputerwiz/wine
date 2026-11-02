/*
 * WineExplorer Desktop Environment - dwm_protocol.h
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

#ifndef __WINE_DWM_PROTOCOL_H
#define __WINE_DWM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "windef.h"

#define DWM_PROTOCOL_VERSION     1u
#define DWM_MAX_MESSAGE_SIZE     4096u
#define DWM_MAX_QUEUE_DEPTH      64u
#define DWM_MAX_PROCESS_CHANNELS 8u

enum dwm_opcode
{
    DWM_OP_ECHO     = 1,
    DWM_OP_SET_ATTR = 2,   /* window_token = HWND; payload struct dwm_attr_set; reply struct dwm_attr_reply */
    DWM_OP_GET_ATTR = 3,   /* window_token = HWND; payload struct dwm_attr_get; reply struct dwm_attr_reply */
    DWM_OP_LAST     = DWM_OP_GET_ATTR,
};

/* scalar (DWORD) window attributes held by the broker */
struct dwm_attr_set   { uint32_t attribute; uint32_t value; };
struct dwm_attr_get   { uint32_t attribute; };
struct dwm_attr_reply { uint32_t found; uint32_t value; };

C_ASSERT( sizeof(struct dwm_attr_set) == 8 );
C_ASSERT( sizeof(struct dwm_attr_get) == 4 );
C_ASSERT( sizeof(struct dwm_attr_reply) == 8 );

/*
 * Fixed-width control-channel message envelope (Section 2.1 of DWM_ARCHITECTURE.md).
 * Uses only fixed-width integer types (uint32_t / uint64_t), never long, size_t, or
 * pointers, so 32-bit (WoW64) and 64-bit processes share the exact wire layout.
 */
struct dwm_msg_header
{
    uint32_t opcode;
    uint32_t version;
    uint32_t length;
    uint32_t request_id;
    uint64_t connection_epoch;
    uint64_t window_token;
    uint32_t revision;
    uint32_t capabilities;
};

C_ASSERT( sizeof(struct dwm_msg_header) == 40 );
C_ASSERT( offsetof(struct dwm_msg_header, opcode) == 0 );
C_ASSERT( offsetof(struct dwm_msg_header, version) == 4 );
C_ASSERT( offsetof(struct dwm_msg_header, length) == 8 );
C_ASSERT( offsetof(struct dwm_msg_header, request_id) == 12 );
C_ASSERT( offsetof(struct dwm_msg_header, connection_epoch) == 16 );
C_ASSERT( offsetof(struct dwm_msg_header, window_token) == 24 );
C_ASSERT( offsetof(struct dwm_msg_header, revision) == 32 );
C_ASSERT( offsetof(struct dwm_msg_header, capabilities) == 36 );

/*
 * Operations and fixed-width 32/64-bit invariant parameter block for the private
 * ProcessWineDwmChannel process information class in NtSetInformationProcess.
 */
#ifndef __WINESRC__
#define ProcessWineDwmChannel 1004
#endif

enum dwm_channel_op
{
    DWM_CHANNEL_OP_CONNECT         = 1,
    DWM_CHANNEL_OP_GET_CAPABILITY  = 2,
    DWM_CHANNEL_OP_BROKER_REGISTER = 3,
    DWM_CHANNEL_OP_SEND            = 4,
    DWM_CHANNEL_OP_RECV            = 5,
    DWM_CHANNEL_OP_REPLY           = 6,
};

struct dwm_channel_call
{
    uint32_t op;
    uint32_t handle;
    uint32_t version;
    uint32_t status;
    uint64_t epoch;
    uint64_t capability;
    uint64_t in_ptr;
    uint32_t in_len;
    uint32_t out_len;
    uint64_t out_ptr;
    uint32_t request_id;
    uint32_t sender_pid;
    uint32_t sender_tid;
    uint32_t desktop_id;
};

C_ASSERT( sizeof(struct dwm_channel_call) == 72 );
C_ASSERT( offsetof(struct dwm_channel_call, op) == 0 );
C_ASSERT( offsetof(struct dwm_channel_call, handle) == 4 );
C_ASSERT( offsetof(struct dwm_channel_call, version) == 8 );
C_ASSERT( offsetof(struct dwm_channel_call, status) == 12 );
C_ASSERT( offsetof(struct dwm_channel_call, epoch) == 16 );
C_ASSERT( offsetof(struct dwm_channel_call, capability) == 24 );
C_ASSERT( offsetof(struct dwm_channel_call, in_ptr) == 32 );
C_ASSERT( offsetof(struct dwm_channel_call, in_len) == 40 );
C_ASSERT( offsetof(struct dwm_channel_call, out_len) == 44 );
C_ASSERT( offsetof(struct dwm_channel_call, out_ptr) == 48 );
C_ASSERT( offsetof(struct dwm_channel_call, request_id) == 56 );
C_ASSERT( offsetof(struct dwm_channel_call, sender_pid) == 60 );
C_ASSERT( offsetof(struct dwm_channel_call, sender_tid) == 64 );
C_ASSERT( offsetof(struct dwm_channel_call, desktop_id) == 68 );

#endif /* __WINE_DWM_PROTOCOL_H */
