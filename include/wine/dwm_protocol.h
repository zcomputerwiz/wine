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
    DWM_OP_POLL     = 4,   /* payload struct dwm_poll_req; reply struct dwm_poll_reply + count struct dwm_change */
    DWM_OP_THUMB_SET  = 5, /* window_token = destination HWND; payload struct dwm_thumb_set; no reply payload */
    DWM_OP_THUMB_DEL  = 6, /* window_token = destination HWND; payload struct dwm_thumb_del; no reply payload */
    DWM_OP_THUMB_LIST = 7, /* payload struct dwm_thumb_list_req; reply struct dwm_thumb_list_reply + count struct dwm_thumb */
    DWM_OP_LAST     = DWM_OP_THUMB_LIST,
};

/* live thumbnails (P8). A thumbnail is keyed by the registering connection's epoch and its HTHUMBNAIL value;
 * rectangles are left, top, right, bottom. dest is relative to the destination window (window rectangle, as
 * on Windows), source to the source window (or its client area with DWM_THUMB_CLIENT_ONLY). */
#define DWM_THUMB_VISIBLE     0x1u
#define DWM_THUMB_CLIENT_ONLY 0x2u
#define DWM_THUMB_HAS_SOURCE  0x4u  /* source is set; otherwise the whole source window */
#define DWM_THUMB_MAX         80u   /* per desktop (one list reply fits a message); registrations beyond fail with STATUS_QUOTA_EXCEEDED */

struct dwm_thumb_set        { uint32_t handle; uint32_t src; uint32_t flags; uint32_t opacity; int32_t dest[4]; int32_t source[4]; };
struct dwm_thumb_del        { uint32_t handle; uint32_t pad; };
struct dwm_thumb_list_req   { uint64_t since_revision; };
struct dwm_thumb_list_reply { uint64_t revision; uint32_t count; uint32_t pad; };  /* count 0 and same revision: unchanged */
struct dwm_thumb            { uint32_t dest; uint32_t src; uint32_t flags; uint32_t opacity; int32_t dest_rect[4]; int32_t source[4]; };

C_ASSERT( sizeof(struct dwm_thumb_set) == 48 );
C_ASSERT( sizeof(struct dwm_thumb_del) == 8 );
C_ASSERT( sizeof(struct dwm_thumb_list_req) == 8 );
C_ASSERT( sizeof(struct dwm_thumb_list_reply) == 16 );
C_ASSERT( sizeof(struct dwm_thumb) == 48 );

/* scalar (DWORD) window attributes held by the broker */
struct dwm_attr_set   { uint32_t attribute; uint32_t value; };
struct dwm_attr_get   { uint32_t attribute; };
struct dwm_attr_reply { uint32_t found; uint32_t value; };

C_ASSERT( sizeof(struct dwm_attr_set) == 8 );
C_ASSERT( sizeof(struct dwm_attr_get) == 4 );
C_ASSERT( sizeof(struct dwm_attr_reply) == 8 );

/* change log and table snapshot for DWM_OP_POLL */
#define DWM_POLL_MAX_CHANGES 100u
#define DWM_POLL_SNAPSHOT    1u  /* entries are table pages, not log entries */
#define DWM_POLL_MORE        2u  /* more pages follow: call again with the returned cursor */

struct dwm_poll_req   { uint64_t since_revision; uint32_t cursor; uint32_t pad; };
struct dwm_change     { uint32_t hwnd; uint32_t attribute; uint32_t value; uint32_t pad; };
struct dwm_poll_reply { uint64_t revision; uint32_t count; uint32_t flags; };

C_ASSERT( sizeof(struct dwm_poll_req) == 16 );
C_ASSERT( sizeof(struct dwm_change) == 16 );
C_ASSERT( sizeof(struct dwm_poll_reply) == 16 );

/*
 * Frame clock page (Section 2.3 of DWM_ARCHITECTURE.md, P6 slice 1). The compositor (the only writer)
 * publishes one record per composed frame; dwmapi reads it to pace DwmFlush and to fill
 * DwmGetCompositionTimingInfo. seq is a sequence lock: the writer makes it odd, writes the fields, then
 * makes it even (with release barriers); readers retry while seq is odd or changed. QPC values are Wine
 * QPC units (100 ns, CLOCK_MONOTONIC, the same as ntdll's monotonic_counter).
 */
struct dwm_clock_page
{
    volatile LONG seq;
    UINT32 flags;
    UINT64 frame;
    INT64 qpc_composed;
    INT64 qpc_period;
};

C_ASSERT( sizeof(struct dwm_clock_page) == 32 );
C_ASSERT( offsetof(struct dwm_clock_page, seq) == 0 );
C_ASSERT( offsetof(struct dwm_clock_page, flags) == 4 );
C_ASSERT( offsetof(struct dwm_clock_page, frame) == 8 );
C_ASSERT( offsetof(struct dwm_clock_page, qpc_composed) == 16 );
C_ASSERT( offsetof(struct dwm_clock_page, qpc_period) == 24 );

#define DWM_CLOCK_COMPOSED 0x1   /* frame and qpc_composed come from the compositor: composed, not displayed */
/* the desktop name is appended to both; the mapping and the event are per desktop */
#define DWM_CLOCK_NAME       L"Local\\__wex_dwm_clock_"
#define DWM_CLOCK_EVENT_NAME L"Local\\__wex_dwm_frame_"

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

/* a full DWM_OP_POLL reply (header + reply + a full page of changes) must fit in one message */
C_ASSERT( sizeof(struct dwm_msg_header) + sizeof(struct dwm_poll_reply) +
          DWM_POLL_MAX_CHANGES * sizeof(struct dwm_change) <= DWM_MAX_MESSAGE_SIZE );
C_ASSERT( sizeof(struct dwm_msg_header) + sizeof(struct dwm_thumb_list_reply) +
          DWM_THUMB_MAX * sizeof(struct dwm_thumb) <= DWM_MAX_MESSAGE_SIZE );

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
