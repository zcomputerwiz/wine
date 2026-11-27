/*
 * WineExplorer system tier (session 0): shared definitions
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

/* Session 0 is a wineserver started with --session0 that holds the machine registry for every session
 * (docs/SYSTEM_TIER_DESIGN.md, docs/SYSTEM_TIER_WORK_ORDERS.md). A program gets a registry channel from its own
 * server (request get_registry_channel); that server connects to session 0's socket and hands it one end of a
 * socket pair together with a description of the program's token. Session 0 serves the other end as a
 * registry-only client with that token. */

#ifndef __WINE_WINE_WEX_SESSION0_H
#define __WINE_WINE_WEX_SESSION0_H

/* the link message from a user's server to session 0: header, then the token description; SCM_RIGHTS carries
 * the channel. Session 0 answers with a reply. One message per connection. */
#define WEX_SESSION0_MAGIC    0x30535857  /* "WXS0" */
#define WEX_SESSION0_VERSION  9
#define WEX_SESSION0_MAX_MSG  65536

struct wex_session0_request
{
    unsigned int magic;
    unsigned int version;
    unsigned int desc_size;   /* size of the token description that follows */
    unsigned int type;        /* WEX_SESSION0_* below */
    union                     /* the last field, by type (one word on the wire whatever its name) */
    {
        unsigned int machine;     /* CHANNEL: the program's machine (IMAGE_FILE_MACHINE_*): its registry view */
        unsigned int saved;       /* HIVE_SAVED: 1 written, 0 not */
        unsigned int name_size;   /* PIPE, PIPE_WAIT: the size in bytes of the pipe name after the description */
        unsigned int session_id;  /* SESSION_ID: the id the server asks for (0: any) */
        unsigned int block;       /* BLOCK: the block of ids the server asks for again (0: a new one) */
        unsigned int unused;      /* USER_HIVE, HIVE_UNLOAD: 0 */
    };
};

/* Message types. CHANNEL: the program's registry channel in SCM_RIGHTS; one message, then the link closes.
 * USER_HIVE (T4a): a user's server hands session 0 a copy of its user.reg (SCM_RIGHTS: a memory file sealed
 * against writes and size changes; none for a new hive) to hold under \Registry\User\<SID of the described
 * token>; the link stays open: session 0 sends HIVE_SAVE messages (the whole hive in a memory file, SCM_RIGHTS)
 * for that server to write, the server answers each with HIVE_SAVED (machine: 1 written, 0 not; a hive that was
 * not written is sent again), and the server sends HIVE_UNLOAD at its exit, answered by a last HIVE_SAVE when
 * there are changes and a reply. */
#define WEX_SESSION0_CHANNEL      0
#define WEX_SESSION0_USER_HIVE    1
#define WEX_SESSION0_HIVE_SAVE    2
#define WEX_SESSION0_HIVE_UNLOAD  3
/* SESSION_ID (T6.1): a user's server asks for its session id at start; session 0 gives each logon its own. The
 * link then stays open as the session link (T6.2): session 0 sends orders on it (wex_session0_order) and the
 * server answers each (wex_session0_order_reply). The server accepts orders only on the link it opened itself
 * to the configured socket, after it checked with SO_PEERCRED that session 0's account and the system group
 * serve that socket; it checks every link to session 0 the same way. */
#define WEX_SESSION0_SESSION_ID   4
#define WEX_SESSION0_HIVE_SAVED   5
/* PIPE (T5.2): a program of the session opens a pipe of session 0 (\pipe\WexSession0\<name> in the session). The
 * token description is followed by the pipe's name (machine: its size in bytes, UTF-16) and, since version 7, the
 * opener's process id (4 bytes; 0: none, as PIPE_WAIT has none); SCM_RIGHTS: one end of
 * a stream socket, on which session 0 joins the pipe (each message: a 4-byte length, then the data). One
 * message, then the link closes. */
#define WEX_SESSION0_PIPE         6
/* PIPE_WAIT: a program of the session waits for a pipe of session 0 (FSCTL_PIPE_WAIT); the same message as PIPE,
 * without a socket. The status says whether session 0 has the pipe; no instance is taken. */
#define WEX_SESSION0_PIPE_WAIT    7
/* BLOCK (R6, docs/SESSION0_PROCESS_IDS.md): a linked server asks for a block of process and thread ids, a new one
 * (block 0) or one it held before session 0 restarted; the reply's value is the block (WEX_ID_BLOCK_SIZE indices
 * from block * WEX_ID_BLOCK_SIZE; ids are indices * 4). Session 0 gives a block to one live server at a time, so
 * the ids of different servers never meet. No token description, no descriptor. */
#define WEX_SESSION0_BLOCK        8
#define WEX_ID_BLOCK_SIZE         4096
#define WEX_ID_BLOCK_COUNT        4096  /* indices below 2^24: ids below 2^26, as on Windows */

/* orders */
#define WEX_SESSION0_ORDER_PING   1   /* answered with the server's session id */

struct wex_session0_order
{
    unsigned int magic;
    unsigned int order;       /* WEX_SESSION0_ORDER_* */
    unsigned int serial;      /* repeated in the answer */
    unsigned int arg;
};

struct wex_session0_order_reply
{
    unsigned int magic;
    unsigned int serial;
    unsigned int status;      /* NTSTATUS; STATUS_NOT_SUPPORTED for an order the server does not know */
    unsigned int value;
};

/* X frames (version 9, docs/SESSION0_CROSS_SERVER.md, M0 and its X0 appendix): the cross-server protocol on the
 * session link, in both directions, beside the orders. A frame is a header, a body by type and, for OPEN, the
 * caller's token description and the name; size counts all of it and is at most WEX_SESSION0_MAX_MSG. A request
 * carries the sender's call number, its reply the same number and the type with WEX_X_REPLY. Replies come in any
 * order. */
#define WEX_X_MAGIC           0x58305857  /* "WX0X" */

#define WEX_X_HELLO           1   /* each side once: struct wex_x_hello; no reply */
#define WEX_X_OPEN            2   /* holder to owner: struct wex_x_open, description, name; reply: ref, wex_x_granted */
#define WEX_X_QUERY           3   /* reply: wex_x_granted */
#define WEX_X_RELEASE         4   /* idempotent: always success */
#define WEX_X_SUBSCRIBE       5   /* arms the subscription of ref */
#define WEX_X_CANCEL          6   /* idempotent: ends the subscription */
#define WEX_X_TRY             7   /* success (acquired) or STATUS_TIMEOUT; re-arms the subscription; never cancelled */
#define WEX_X_NOTICE          8   /* owner to holder: ref is ready (one per arming); no reply */
#define WEX_X_PING            9   /* the body comes back */
#define WEX_X_INFO            10  /* holder to owner: struct wex_x_info_req; reply: the kind's information (X3:
                                   * struct wex_x_process_info, then the image name) */
#define WEX_X_REPLY           0x80000000

/* OPEN kinds (X1: global names); the lab build has its counter */
#define WEX_X_KIND_PROCESS    2           /* X3 (M3): the name is a process id (unsigned int); session 0 callers only */
#define WEX_X_KIND_LAB        0x7f000001

/* caps on what is in flight (per account of the holder; requests per link) */
#define WEX_X_MAX_REFS        16384
#define WEX_X_MAX_SUBS        4096
#define WEX_X_MAX_INFLIGHT    256
#define WEX_X_MAX_QUEUED      (4 * 1024 * 1024)
/* how long an owner keeps the references of a peer whose link is lost while its process lives, in seconds */
#define WEX_X_LOST_SECONDS    120

struct wex_x_header
{
    unsigned int       magic;      /* WEX_X_MAGIC */
    unsigned int       size;       /* the whole frame, header included */
    unsigned int       type;       /* WEX_X_*, with WEX_X_REPLY in a reply */
    unsigned int       call;       /* the sender's call number, repeated in the reply; 0 in HELLO and NOTICE */
    unsigned int       status;     /* replies: NTSTATUS; requests: 0 */
    unsigned int       desc_size;  /* OPEN: the size of the token description after the body */
    unsigned long long ref;        /* the reference the frame is about, or 0 */
};

/* a server's identity: it changes when the server restarts, never when a link does */
struct wex_x_epoch
{
    unsigned long long nonce;      /* random, from the server's start */
    unsigned long long start;      /* the process's start time (/proc/<pid>/stat, clock ticks since boot) */
    unsigned int       pid;
    unsigned int       reserved;
};

struct wex_x_hello
{
    struct wex_x_epoch epoch;
    unsigned int       connection; /* session 0: its number for this connection; a user's server: 0 */
    unsigned int       session;    /* session 0: the link's session; a user's server: its own */
    unsigned long long peer_nonce; /* the nonce of the epoch the sender still keeps for the connecting server, or 0:
                                    * when it is not the receiver's own, the sender started fresh (it dropped the
                                    * peer), and the receiver ends its side of that peer too */
};

struct wex_x_open
{
    unsigned int       kind;       /* WEX_X_KIND_* */
    unsigned int       access;     /* desired access */
    unsigned int       name_size;  /* bytes of the name after the description */
    unsigned int       reserved;
};

struct wex_x_granted
{
    unsigned int       access;     /* the rights the owner granted (information: the owner decides) */
    unsigned int       ready;      /* QUERY: the object is ready (signaled) now */
};

struct wex_x_info_req
{
    unsigned int       info_class; /* X3: the ProcessInformationClass asked for (the owner's FIXME names it) */
    unsigned int       reserved;
};

/* X3: what the owner of a process says about it (get_process_info's fields), then image_len bytes of its image's NT
 * name. Taken at the open and again at the death notice; the holder answers its queries from it. */
struct wex_x_process_info
{
    unsigned long long affinity;
    unsigned long long peb;
    long long          start_time;
    long long          end_time;      /* 0 while it runs */
    unsigned int       pid;
    unsigned int       ppid;
    unsigned int       exit_code;
    unsigned int       session_id;
    int                priority;
    int                base_priority;
    int                disable_boost;
    unsigned short     machine;
    unsigned short     running;       /* it has threads left */
    unsigned int       image_len;
    unsigned int       reserved;
};

struct wex_session0_reply
{
    unsigned int magic;
    unsigned int status;      /* NTSTATUS */
    unsigned int value;       /* SESSION_ID: the session id */
};

/* session 0 to a user's server on a hive link */
struct wex_session0_hive_save
{
    unsigned int magic;
    unsigned int type;        /* WEX_SESSION0_HIVE_SAVE */
    unsigned int reserved;    /* the same size as a reply */
};

/* Keys that stay in each session's own server: per-session state that differs between sessions (devices,
 * display, drive and port mappings, computer name state, the service manager's instance counter, Safe Mode's
 * SafeBoot\Option, which each session's wineboot writes from the boot mode, the volatile hardware tree).
 * Paths relative to \Registry\Machine and \Registry\User\.Default; a key matches
 * when its path equals an entry or continues it with a backslash (wex_session_key_match). Compared without regard
 * to case.
 *
 * ntdll opens these keys in the program's own server instead of session 0 (routing). The server's copy of the
 * machine hives keeps them and is never written: only session 0 writes the machine hives (R4 of
 * docs/SESSION0_SPLIT_REVIEW.md retired the merge of a shared hive file, which used this list too). */
#define WEX_SESSION_MACHINE_KEYS \
    "Hardware", \
    "System\\ControlSet001\\Enum", \
    "System\\CurrentControlSet\\Enum", \
    "System\\ControlSet001\\Control\\Class", \
    "System\\CurrentControlSet\\Control\\Class", \
    "System\\ControlSet001\\Control\\Video", \
    "System\\CurrentControlSet\\Control\\Video", \
    "System\\ControlSet001\\Control\\DeviceClasses", \
    "System\\CurrentControlSet\\Control\\DeviceClasses", \
    "System\\ControlSet001\\Control\\Print", \
    "System\\CurrentControlSet\\Control\\Print", \
    "System\\ControlSet001\\Control\\ComputerName", \
    "System\\CurrentControlSet\\Control\\ComputerName", \
    "System\\ControlSet001\\Control\\ServiceCurrent", \
    "System\\CurrentControlSet\\Control\\ServiceCurrent", \
    "System\\ControlSet001\\Control\\SafeBoot\\Option", \
    "System\\CurrentControlSet\\Control\\SafeBoot\\Option", \
    "System\\ControlSet001\\Services\\Tcpip\\Parameters", \
    "System\\CurrentControlSet\\Services\\Tcpip\\Parameters", \
    "Software\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", \
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", \
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", \
    "Software\\Wine\\Drives", \
    "Software\\Wine\\Ports", \
    "Software\\Wow6432Node\\Wine\\Ports"

#define WEX_SESSION_DEFAULT_USER_KEYS \
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", \
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders"

/* the number of characters of path (count characters) that an ASCII prefix matches, when the match ends with the
 * path or at a backslash; 0 when it does not match. Without regard to case (ASCII letters). */
static inline unsigned int wex_session_key_match( const WCHAR *path, unsigned int count, const char *prefix )
{
    unsigned int i;

    for (i = 0; prefix[i]; i++)
    {
        WCHAR ch = path[i];
        unsigned char p = prefix[i];
        if (i >= count) return 0;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (p >= 'A' && p <= 'Z') p += 'a' - 'A';
        if (ch != p) return 0;
    }
    return (count == i || path[i] == '\\') ? i : 0;
}

#endif /* __WINE_WINE_WEX_SESSION0_H */
