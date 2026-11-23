/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 *
 * Is the peer of a connected Unix socket a process that the setgid launcher (tools/wex-sysrun) started? Shared by
 * session 0's listener (server/session0.c), the client of a launcher-started server (dlls/ntdll/unix/sysrun.c) and
 * the launcher itself (WineExplorer R2 of docs/SESSION0_SPLIT_REVIEW.md).
 *
 * Only a program run through a setgid program of the system group has that group as its saved group while its other
 * ids are the user's own; the launcher runs only the configured wineserver and the machine root's loader, and such a
 * process is not dumpable, so the user cannot take it over. Its executable cannot be compared: /proc/<pid>/exe of a
 * non-dumpable process is not readable. The pid from SO_PEERCRED is pinned through a pidfd taken before /proc is
 * read: when that process is still alive afterwards, /proc described it and not a later process given the same pid.
 * Kernels before 6.5 (no SO_PEERPIDFD) get the check without the pin.
 *
 * Users compile with _GNU_SOURCE (struct ucred).
 */
#ifndef __WINE_WEX_SYSRUN_PEER_H
#define __WINE_WEX_SYSRUN_PEER_H

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

/* what the peer must be; a field of (uid_t)-1 or (gid_t)-1 is not checked unless said otherwise */
struct wex_sysrun_peer_rule
{
    uid_t uid;          /* the peer's real, effective, saved and file system uid (always checked) */
    gid_t real_gid;     /* its real gid; (gid_t)-1: any gid but sys_gid */
    gid_t sys_gid;      /* its saved gid (always checked); the effective gid is the real one or this one */
    int server_name;    /* the process name is "wineserver" */
};

/* the ids and name of a process from /proc/<pid>/status; 0 when it cannot be read */
static inline int wex_sysrun_peer_status( pid_t pid, unsigned int uid[4], unsigned int gid[4], int *server_name )
{
    char path[64], buf[8192], *p;
    int fd, got_uid = 0, got_gid = 0;
    ssize_t n;

    snprintf( path, sizeof(path), "/proc/%d/status", (int)pid );
    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return 0;
    n = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (n <= 0) return 0;
    buf[n] = 0;
    *server_name = !strncmp( buf, "Name:\twineserver\n", 17 );
    for (p = buf; p; p = strchr( p, '\n' ))
    {
        if (*p == '\n') p++;
        if (!strncmp( p, "Uid:", 4 ))
            got_uid = sscanf( p + 4, "%u %u %u %u", &uid[0], &uid[1], &uid[2], &uid[3] ) == 4;
        else if (!strncmp( p, "Gid:", 4 ))
            got_gid = sscanf( p + 4, "%u %u %u %u", &gid[0], &gid[1], &gid[2], &gid[3] ) == 4;
    }
    return got_uid && got_gid;
}

/* Does the process with this pid follow the rule? pin: a pidfd of that process, or -1. */
static inline int wex_sysrun_peer_pid_ok( pid_t pid, int pin, const struct wex_sysrun_peer_rule *rule )
{
    unsigned int uid[4], gid[4];
    struct pollfd pfd;
    int server_name, ok, i;

    if (pid <= 0 || rule->sys_gid == (gid_t)-1 || !wex_sysrun_peer_status( pid, uid, gid, &server_name )) return 0;
    ok = !rule->server_name || server_name;
    for (i = 0; i < 4; i++) if (uid[i] != rule->uid) ok = 0;
    /* real, effective, saved: the saved gid is the system group, the real one is not (or is the given one), the
     * effective one is either (the server raises it while it writes the machine root) */
    if (gid[2] != rule->sys_gid) ok = 0;
    if (rule->real_gid == (gid_t)-1 ? gid[0] == rule->sys_gid : gid[0] != rule->real_gid) ok = 0;
    if (gid[1] != gid[0] && gid[1] != rule->sys_gid) ok = 0;
    if (ok && pin != -1)
    {
        pfd.fd = pin;
        pfd.events = POLLIN;  /* a pidfd is readable once its process has exited */
        pfd.revents = 0;
        if (poll( &pfd, 1, 0 ) != 0) ok = 0;
    }
    return ok;
}

/* Is the peer of the connected socket fd a process that follows the rule? The pid and uid come from SO_PEERCRED. */
static inline int wex_sysrun_peer_ok( int fd, const struct wex_sysrun_peer_rule *rule )
{
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int pin = -1, ok;

    if (getsockopt( fd, SOL_SOCKET, SO_PEERCRED, &cred, &len ) || cred.pid <= 0 || cred.uid != rule->uid) return 0;
    len = sizeof(pin);
    if (getsockopt( fd, SOL_SOCKET, SO_PEERPIDFD, &pin, &len )) pin = -1;
    ok = wex_sysrun_peer_pid_ok( cred.pid, pin, rule );
    if (pin != -1) close( pin );
    return ok;
}

#endif /* __WINE_WEX_SYSRUN_PEER_H */
