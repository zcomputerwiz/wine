/*
 * Server side of the wexsys system role: effective group handling
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
 * The setgid launcher wex-sysrun starts the server with effective and saved group = the system group (wexsys)
 * and the user's own real group. The server drops the effective group to the real one at once and keeps wexsys
 * only as the saved group, so that Unix files opened on behalf of a client are opened with the user's rights.
 * It raises the effective group again only
 *  - around the load and save of the machine registry hives, and
 *  - for opening, creating, renaming and unlinking Unix paths that a client requests, if that client itself runs
 *    with the system group (SO_PEERCRED at connect time, struct process::peer_gid).
 * Without this a client with limited rights could make the server write the machine root for it.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_SYS_PRCTL_H
# include <sys/prctl.h>
#endif

#include "object.h"
#include "process.h"
#include "request.h"
#include "thread.h"
#include "sysrun.h"

#define SYSRUN_CONF "/etc/wineexplorer/sysrun.conf"

int sysrun_active;
static gid_t system_gid, user_gid;
static char machine_root[PATH_MAX];
static int is_raised;  /* the effective group is the system group now */

/* A non-dumpable process stays so: a credential change may set the flag to fs.suid_dumpable, which an
 * administrator can have at 1 or 2. */
static void set_not_dumpable(void)
{
#if defined(__linux__) && defined(HAVE_SYS_PRCTL_H)
    prctl( PR_SET_DUMPABLE, 0, 0, 0, 0 );
#endif
}

/* read "group" and "machine_root" from the configuration, which must be a regular root-owned file that only
 * root can write (checked on the open descriptor) */
static int read_config( char *group, size_t group_size )
{
    char buf[4096], *line, *next;
    struct stat st;
    ssize_t n;
    int fd;

    group[0] = machine_root[0] = 0;
    if ((fd = open( SYSRUN_CONF, O_RDONLY | O_NOFOLLOW | O_CLOEXEC )) == -1) return 0;
    if (fstat( fd, &st ) || !S_ISREG( st.st_mode ) || st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))
        n = 0;
    else
        n = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (n <= 0) return 0;
    buf[n] = 0;
    for (line = buf; line; line = next)
    {
        if ((next = strchr( line, '\n' ))) *next++ = 0;
        if (!strncmp( line, "group=", 6 ))
        {
            if (strlen( line + 6 ) >= group_size) return 0;
            strcpy( group, line + 6 );
        }
        else if (!strncmp( line, "machine_root=", 13 ))
        {
            if (strlen( line + 13 ) >= sizeof(machine_root)) return 0;
            strcpy( machine_root, line + 13 );
        }
    }
    return group[0] && machine_root[0] == '/';
}

/* called first thing in main() */
void sysrun_init(void)
{
#ifdef __linux__
    char group[64];
    struct group *gr;
    gid_t rgid, egid, sgid;

    /* only the launcher gives a process an effective and saved group that differs from the real one */
    if (getresgid( &rgid, &egid, &sgid ) || rgid == sgid || egid != sgid) return;
    if (!read_config( group, sizeof(group) )) return;
    if (!(gr = getgrnam( group )) || gr->gr_gid != sgid) return;
    system_gid = sgid;
    user_gid = rgid;
    if (setegid( user_gid )) fatal_error( "cannot drop the system group: %s\n", strerror( errno ));
    set_not_dumpable();
    sysrun_active = 1;
#endif
}

const char *sysrun_machine_root(void)
{
    return sysrun_active ? machine_root : NULL;
}

gid_t sysrun_group(void)
{
    return system_gid;
}

int sysrun_raise(void)
{
    int err = errno;

    if (!sysrun_active || is_raised) return 0;
    if (setegid( system_gid )) return 0;  /* the operation then fails with the user's rights */
    set_not_dumpable();
    is_raised = 1;
    errno = err;
    return 1;
}

void sysrun_drop( int raised )
{
    int err = errno;

    if (!raised) return;
    if (setegid( user_gid )) fatal_error( "cannot drop the system group: %s\n", strerror( errno ));
    set_not_dumpable();
    is_raised = 0;
    errno = err;
}

int sysrun_process_privileged( const struct process *process )
{
    return sysrun_active && process && process->peer_uid == getuid() && process->peer_gid == system_gid;
}

int sysrun_raise_for_current(void)
{
    return sysrun_active && current && sysrun_process_privileged( current->process ) ? sysrun_raise() : 0;
}

int sysrun_mkdir( const char *path, mode_t mode )
{
    int raised = sysrun_raise_for_current(), ret = mkdir( path, mode );
    sysrun_drop( raised );
    return ret;
}

int sysrun_open( const char *path, int flags, mode_t mode )
{
    int raised = sysrun_raise_for_current(), ret = open( path, flags, mode );
    sysrun_drop( raised );
    return ret;
}

int sysrun_rename( const char *old, const char *new )
{
    int raised = sysrun_raise_for_current(), ret = rename( old, new );
    sysrun_drop( raised );
    return ret;
}

int sysrun_link( const char *old, const char *new )
{
    int raised = sysrun_raise_for_current(), ret = link( old, new );
    sysrun_drop( raised );
    return ret;
}

int sysrun_unlink( const char *path )
{
    int raised = sysrun_raise_for_current(), ret = unlink( path );
    sysrun_drop( raised );
    return ret;
}
