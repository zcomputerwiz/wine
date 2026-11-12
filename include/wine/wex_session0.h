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
#define WEX_SESSION0_VERSION  5
#define WEX_SESSION0_MAX_MSG  65536

struct wex_session0_request
{
    unsigned int magic;
    unsigned int version;
    unsigned int desc_size;   /* size of the token description that follows */
    unsigned int type;        /* WEX_SESSION0_* below */
    unsigned int machine;     /* CHANNEL: the program's machine (IMAGE_FILE_MACHINE_*): its registry view */
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
 * token description is followed by the pipe's name (machine: its size in bytes, UTF-16); SCM_RIGHTS: one end of
 * a stream socket, on which session 0 joins the pipe (each message: a 4-byte length, then the data). One
 * message, then the link closes. */
#define WEX_SESSION0_PIPE         6

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
 * when its path equals an entry or continues it with a backslash. Compared without regard to case. */
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

#endif /* __WINE_WINE_WEX_SESSION0_H */
