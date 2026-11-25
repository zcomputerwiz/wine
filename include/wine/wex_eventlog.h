/*
 * WineExplorer event log: the pipe protocol between clients and the EventLog service
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

/* docs/EVENTLOG_DESIGN.md (WineExplorer). Our own protocol: Windows programs reach the log only through advapi32, so
 * no Microsoft protocol is needed. A message-mode pipe; each request is one message and gets one reply message; one
 * request in flight per connection. Statuses are Win32 error codes. Strings are UTF-16, terminated, even-sized.
 *
 * The service listens on \\.\pipe\WexSession0\eventlog (its own server's processes: session 0's, or a plain prefix's)
 * and, in session 0, on \\.\pipe\eventlog, which a session's open of \\.\pipe\WexSession0\eventlog reaches through
 * the pipe bridge. */

#ifndef __WINE_WINE_WEX_EVENTLOG_H
#define __WINE_WINE_WEX_EVENTLOG_H

#define WEX_EVTLOG_PIPE         L"\\\\.\\pipe\\WexSession0\\eventlog"
#define WEX_EVTLOG_PIPE_BRIDGED L"\\\\.\\pipe\\eventlog"   /* session 0 only: the bridge's end */

#define WEX_EVTLOG_MAGIC        0x4c565857   /* "WXVL" */
#define WEX_EVTLOG_VERSION      1

/* limits (E4) */
#define WEX_EVTLOG_MAX_RECORD   0x3ffff      /* a record, as Windows */
#define WEX_EVTLOG_MAX_READ     0x7ffff      /* a read or stream reply's records */
#define WEX_EVTLOG_MAX_MSG      (WEX_EVTLOG_MAX_READ + 1 + 64)
#define WEX_EVTLOG_MAX_STRING   32767        /* characters, without the terminator */
#define WEX_EVTLOG_MAX_STRINGS  256          /* insertion strings */
#define WEX_EVTLOG_MAX_SID      68           /* bytes: a SID of 15 sub-authorities */
#define WEX_EVTLOG_MAX_CONNECTIONS   256     /* per account */
#define WEX_EVTLOG_MAX_HANDLES       1024    /* per account */
#define WEX_EVTLOG_MAX_SUBSCRIPTIONS 256     /* per account */

/* operations */
#define WEX_EVTLOG_OPEN         1   /* body: wex_evtlog_open, name; reply: handle */
#define WEX_EVTLOG_CLOSE        2
#define WEX_EVTLOG_REPORT       3   /* body: wex_evtlog_report, SID, strings, data */
#define WEX_EVTLOG_READ         4   /* arg[0]: EVENTLOG_* read flags, arg[1]: record number; body: the buffer size;
                                     * reply: records, or ERROR_INSUFFICIENT_BUFFER with arg[0] the size needed */
#define WEX_EVTLOG_COUNT        5   /* reply arg[0] */
#define WEX_EVTLOG_OLDEST       6   /* reply arg[0]: the oldest record number */
#define WEX_EVTLOG_INFO         7   /* reply: wex_evtlog_information; arg[0]: the log is full */
#define WEX_EVTLOG_CLEAR        8   /* arg[0]: 1 after a backup, arg[1]: the last record number the backup has; with
                                     * newer records ERROR_RETRY: the client streams the rest and clears again (N6) */
#define WEX_EVTLOG_STREAM       9   /* arg[0]: the first record number (0: the oldest); reply: records, arg[0]: the
                                     * next number, ERROR_HANDLE_EOF after the last (E3: the client writes backups) */
#define WEX_EVTLOG_NOTIFY       10  /* completes when a record is written to the handle's log (a connection of its own) */

/* OPEN flags */
#define WEX_EVTLOG_OPEN_LOG     0   /* name: a log (OpenEventLog) */
#define WEX_EVTLOG_OPEN_SOURCE  1   /* name: an event source (RegisterEventSource) */

#include <pshpack4.h>
struct wex_evtlog_header
{
    unsigned int magic;
    unsigned int version;
    unsigned int op;
    unsigned int size;      /* the whole message, header included */
    unsigned int status;    /* replies: a Win32 error code */
    unsigned int handle;    /* the log handle the request is about */
    unsigned int arg[2];
};

struct wex_evtlog_open
{
    unsigned int flags;     /* WEX_EVTLOG_OPEN_* */
    unsigned int name_size; /* bytes, terminator included */
};

struct wex_evtlog_report
{
    unsigned short type;        /* EVENTLOG_*_TYPE */
    unsigned short category;
    unsigned int   event_id;
    unsigned int   num_strings;
    unsigned int   strings_size;/* bytes of the terminated strings, one after the other */
    unsigned int   data_size;
    unsigned int   sid_size;    /* 0: none */
};
/* INFO's reply, and what advapi32's GetEventLogInformation returns at the Wine-private level below (section 9:
 * Event Viewer's log properties). Times are FILETIMEs (UTC) from the log file's header. */
#define WEX_EVTLOG_INFORMATION_LEVEL 0x57455801   /* outside Windows' documented levels */

struct wex_evtlog_information
{
    unsigned int       full;          /* as EVENTLOG_FULL_INFORMATION */
    unsigned int       count;         /* records */
    unsigned int       oldest;        /* the oldest record number, 0 when empty */
    unsigned int       next;          /* the number the next record gets */
    unsigned int       max_size;      /* MaxSize in force */
    unsigned int       retention;     /* Retention in force: 0 overwrite as needed, seconds, 0xffffffff never */
    unsigned long long file_size;
    unsigned long long created;
    unsigned long long written;       /* the last record written, or the clear */
    unsigned long long read;          /* the last read or stream */
};
#include <poppack.h>

#endif /* __WINE_WINE_WEX_EVENTLOG_H */
