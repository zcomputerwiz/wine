/*
 * Client side of the wexsys system role: the setgid launcher and the checks of the server
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

/*
 * Active only when the root-owned /etc/wineexplorer/sysrun.conf names a machine root and the setgid launcher
 * wex-sysrun is installed. Then the server (and wineboot --init) start through the launcher, and a client whose
 * own effective group is the system group only talks to a server that passes the check below. Otherwise this
 * file does nothing and Wine starts as stock.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"

#define SYSRUN_CONF     "/etc/wineexplorer/sysrun.conf"
#define SYSRUN_LAUNCHER "/usr/local/libexec/wineexplorer/wex-sysrun"

extern char **environ;

/* the group and the machine root of the configuration, which must be a regular root-owned file that only root
 * can write (checked on the open descriptor) */
static BOOL read_config( char *group, size_t size )
{
    char buf[4096], *line, *next, root[8] = "";
    struct stat st;
    ssize_t n;
    int fd;

    group[0] = 0;
    if ((fd = open( SYSRUN_CONF, O_RDONLY | O_NOFOLLOW | O_CLOEXEC )) == -1) return FALSE;
    if (fstat( fd, &st ) || !S_ISREG( st.st_mode ) || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))
        n = 0;
    else
        n = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (n <= 0) return FALSE;
    buf[n] = 0;
    for (line = buf; line; line = next)
    {
        if ((next = strchr( line, '\n' ))) *next++ = 0;
        if (!strncmp( line, "group=", 6 ))
        {
            if (strlen( line + 6 ) >= size) return FALSE;
            strcpy( group, line + 6 );
        }
        else if (!strncmp( line, "machine_root=", 13 ))
        {
            if (line[13] != '/') return FALSE;
            strcpy( root, "set" );
        }
    }
    return group[0] && root[0];
}

/* the group id of the system group, or -1 if there is no valid configuration */
static gid_t get_system_gid(void)
{
    static gid_t gid = (gid_t)-2;
    char group[64];
    struct group *gr;

    if (gid == (gid_t)-2)
    {
        gid = (gid_t)-1;
        if (read_config( group, sizeof(group) ) && (gr = getgrnam( group ))) gid = gr->gr_gid;
    }
    return gid;
}

/* does this process run with the system group as its effective group? */
BOOL sysrun_client_is_system(void)
{
    gid_t gid = get_system_gid();

    return gid != (gid_t)-1 && getegid() == gid && getgid() != gid;
}

/* Can the server be started through the launcher? The configuration is valid, the launcher is installed
 * (root-owned, setgid) and the prefix is the one the launcher serves: the user's own ~/.wine. */
BOOL sysrun_launcher_usable(void)
{
    static int usable = -1;
    struct passwd *pwd;
    struct stat st;
    char *prefix;

    if (usable != -1) return usable;
    usable = 0;
    if (get_system_gid() == (gid_t)-1) return FALSE;
    if (lstat( SYSRUN_LAUNCHER, &st ) || !S_ISREG( st.st_mode ) || st.st_uid != 0 || !(st.st_mode & S_ISGID))
        return FALSE;  /* not installed: stock behavior, nothing to report */
    if (!(pwd = getpwuid( getuid() )) || pwd->pw_dir[0] != '/' || asprintf( &prefix, "%s/.wine", pwd->pw_dir ) < 0)
        return FALSE;
    if (strcmp( config_dir, prefix ))
        fprintf( stderr, "wine: WARNING: the prefix %s is not %s, the system role is not available for it\n",
                 config_dir, prefix );
    else
        usable = 1;
    free( prefix );
    return usable;
}

/* run the launcher in a mode, without arguments of the caller and with its environment (the launcher builds its own) */
int sysrun_spawn( const char *mode, pid_t *pid )
{
    char *argv[3] = { (char *)SYSRUN_LAUNCHER, (char *)mode, NULL };

    return posix_spawn( pid, SYSRUN_LAUNCHER, NULL, NULL, argv, environ );
}

void sysrun_warn_refused( const char *mode )
{
    fprintf( stderr, "wine: WARNING: the launcher %s refused to run %s, starting without the system role\n",
             SYSRUN_LAUNCHER, mode );
}

/* Is the process at the other end of the socket the system role server of this user? It has no write access
 * to the machine root unless it is raised, so the check is on the saved group: only a process that the launcher
 * started has the system group as its saved group and the user's own ids otherwise. The caller must run with
 * the system group, so a server that a user started without the launcher is never trusted. */
BOOL sysrun_server_ok( int fd )
{
    unsigned int ruid, euid, suid, fuid, rgid, egid, sgid, fgid;
    char path[64], buf[8192], *p;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    gid_t gid = get_system_gid();
    BOOL got_uid = FALSE, got_gid = FALSE;
    ssize_t n;
    int sfd;

    if (gid == (gid_t)-1 || getsockopt( fd, SOL_SOCKET, SO_PEERCRED, &cred, &len ) || cred.pid <= 0) return FALSE;
    if (cred.uid != getuid()) return FALSE;
    snprintf( path, sizeof(path), "/proc/%d/status", (int)cred.pid );
    if ((sfd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return FALSE;
    n = read( sfd, buf, sizeof(buf) - 1 );
    close( sfd );
    if (n <= 0) return FALSE;
    buf[n] = 0;
    for (p = buf; p; p = strchr( p, '\n' ))
    {
        if (*p == '\n') p++;
        if (!strncmp( p, "Uid:", 4 ))
            got_uid = sscanf( p + 4, "%u %u %u %u", &ruid, &euid, &suid, &fuid ) == 4;
        else if (!strncmp( p, "Gid:", 4 ))
            got_gid = sscanf( p + 4, "%u %u %u %u", &rgid, &egid, &sgid, &fgid ) == 4;
    }
    if (!got_uid || !got_gid) return FALSE;
    return ruid == getuid() && euid == ruid && suid == ruid && fuid == ruid &&
           rgid == getgid() && sgid == gid && (egid == rgid || egid == gid);
}
