/*
 * WineExplorer: the link between the systemd journal bridge and the EventLog service (docs/EVENTLOG_JOURNAL.md)
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

/* Shared by wevtsvc (Wine) and wex-journal-bridge (native, src/wex-journal-bridge): plain C types only.
 *
 * Transport: AF_UNIX, SOCK_STREAM, at WEXJ_SOCKET (the bridge's; it admits session 0's account only, by SO_PEERCRED
 * uid). A stream of frames: a header, then size - sizeof(header) bytes of body. Strings are UTF-8, each terminated by
 * a NUL inside the body; a receiver checks every count and terminator against the frame and drops a malformed frame's
 * connection. Both sides treat the other's frames as untrusted.
 *
 *   HELLO  (service to bridge, first): the cursor of the last journal entry the service stored, or an empty string.
 *   EVENT  (bridge to service): struct wexj_event, then the source, the entry's cursor and num_strings strings.
 *   GAP    (bridge to service): the stored cursor is no longer in the journal; the bridge goes on from the oldest
 *          entry it has (the service writes one Warning).
 *   RECORD (service to bridge): a record the service stored that did not come from the journal: struct wexj_record,
 *          then the log, the source, the user SID as a string (or empty) and num_strings strings. */

#ifndef __WINE_WEX_JOURNAL_H
#define __WINE_WEX_JOURNAL_H

#define WEXJ_SOCKET        "/run/wineexplorer-journal/events"
#define WEXJ_MAGIC         0x4a584557  /* "WEXJ" */
#define WEXJ_VERSION       1
#define WEXJ_MAX_FRAME     65536
#define WEXJ_MAX_STRING    1024        /* characters of one insertion string; longer ones are cut */
#define WEXJ_MAX_STRINGS   16
#define WEXJ_MAX_SOURCE    64
#define WEXJ_MAX_CURSOR    512

#define WEXJ_HELLO         1
#define WEXJ_EVENT         2
#define WEXJ_GAP           3
#define WEXJ_RECORD        4

#define WEXJ_LOG_SYSTEM      1
#define WEXJ_LOG_APPLICATION 2
#define WEXJ_LOG_LINUX       3   /* the administrators-only custom log */
#define WEXJ_LOG_SECURITY    4   /* J2 */

/* the record data of an event from the journal: this magic, then the entry's cursor (UTF-8, NUL-terminated) */
#define WEXJ_CURSOR_MAGIC  0x434a5857  /* "WXJC" */

struct wexj_header
{
    unsigned int   magic;
    unsigned short version;
    unsigned short type;
    unsigned int   size;       /* the whole frame, header included, at most WEXJ_MAX_FRAME */
    unsigned int   reserved;
};

struct wexj_event
{
    unsigned int   event_id;
    unsigned short event_type; /* EVENTLOG_ERROR_TYPE (1), WARNING (2), INFORMATION (4), AUDIT_SUCCESS (8), FAILURE (16) */
    unsigned short num_strings;
    unsigned int   log;        /* WEXJ_LOG_* */
    unsigned int   reserved;
};

struct wexj_record
{
    unsigned int   event_id;
    unsigned short event_type;
    unsigned short num_strings;
    unsigned int   record_number;
    unsigned int   reserved;
};

#endif /* __WINE_WEX_JOURNAL_H */
