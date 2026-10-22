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

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "object.h"
#include "process.h"
#include "request.h"
#include "thread.h"
#include "sysrun.h"
#include "security.h"
#include "file.h"

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

/* ---- the system role (C3) ---------------------------------------------------------------------------------- */

static int system_role_enabled(void)
{
    return sysrun_active && elevation_enforced();
}

static const struct sid authenticated_users_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_AUTHENTICATED_USER_RID } };
static const struct sid local_service_account_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_LOCAL_SERVICE_RID } };
static const struct sid network_service_account_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_NETWORK_SERVICE_RID } };

/* Give a process or thread of the system role the descriptor of a Windows service process: SYSTEM (and the service
 * account) full control; Administrators query and read the descriptor, so full access needs SeDebugPrivilege;
 * Users and Authenticated Users only the limited query and SYNCHRONIZE, so that task managers can list it.
 * The owner and group come from the token. */
static int protect_object( struct object *obj, struct token *token, int account, int is_thread )
{
    const unsigned int admin_mask = is_thread ? THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL
                                              : PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL;
    const unsigned int user_mask = (is_thread ? THREAD_QUERY_LIMITED_INFORMATION : PROCESS_QUERY_LIMITED_INFORMATION) | SYNCHRONIZE;
    const struct sid *acct = account == SERVICE_ACCOUNT_LOCAL_SERVICE ? &local_service_account_sid :
                             account == SERVICE_ACCOUNT_NETWORK_SERVICE ? &network_service_account_sid : NULL;
    size_t acl_size = sizeof(struct acl) + 4 * sizeof(struct ace) + sid_len( &local_system_sid ) + sid_len( &builtin_admins_sid ) +
                      sid_len( &builtin_users_sid ) + sid_len( &authenticated_users_sid ) + (acct ? sid_len( acct ) + sizeof(struct ace) : 0);
    size_t size = sizeof(struct security_descriptor) + acl_size;
    struct security_descriptor *sd;
    struct acl *acl;
    struct ace *ace;
    int ret;

    if (!(sd = mem_alloc( size ))) return 0;
    memset( sd, 0, size );
    sd->control = SE_DACL_PRESENT;
    sd->dacl_len = acl_size;
    acl = (struct acl *)(sd + 1);
    acl->revision = ACL_REVISION;
    acl->size = acl_size;
    acl->count = acct ? 5 : 4;
    ace = set_ace( ace_first( acl ), &local_system_sid, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL );
    if (acct) ace = set_ace( ace_next( ace ), acct, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL );
    ace = set_ace( ace_next( ace ), &builtin_admins_sid, ACCESS_ALLOWED_ACE_TYPE, 0, admin_mask );
    ace = set_ace( ace_next( ace ), &builtin_users_sid, ACCESS_ALLOWED_ACE_TYPE, 0, user_mask );
    set_ace( ace_next( ace ), &authenticated_users_sid, ACCESS_ALLOWED_ACE_TYPE, 0, user_mask );
    ret = set_sd_defaults_from_token( obj, sd, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                      token );
    free( sd );
    return ret;
}

int sysrun_process_created( struct process *process, int is_root )
{
    struct token *token;
    int account, ret = 1;

    if (!system_role_enabled()) return 1;
    if (is_root)
    {
        /* the connection has the system group: the launcher started it. It gets the SYSTEM token only when its main
         * image is a system program of the machine root, but its descriptor protects it from the start */
        if (!sysrun_process_privileged( process )) return 1;
        if (!(token = token_create_service( SERVICE_ACCOUNT_SYSTEM, token_get_session_id( process->token ) ))) return 0;
        ret = protect_object( &process->obj, token, SERVICE_ACCOUNT_SYSTEM, 0 );
        process->sysrun_account = SERVICE_ACCOUNT_SYSTEM;
        process->sysrun_pending = 1;
        process->sysrun_protected = 1;
        release_object( token );
        return ret;
    }
    if ((account = token_service_account( process->token )) < 0) return 1;
    process->sysrun_protected = 1;
    process->sysrun_account = account;
    /* The connection credentials of a new process are those of the process that made the socket pair, so
     * a service started by services.exe would look like it has the system group. Its Unix process drops the group
     * (ntdll, sysrun.c) and so does the server's view of it. */
    if (account != SERVICE_ACCOUNT_SYSTEM) process->peer_gid = -1;
    return protect_object( &process->obj, process->token, account, 0 );
}

int sysrun_thread_created( struct thread *thread )
{
    struct process *process = thread->process;

    if (!process->sysrun_protected || thread->obj.sd) return 1;
    return protect_object( &thread->obj, process->token, process->sysrun_account, 1 );
}

/* the system programs of the machine root that may be the main image of a system group root process */
static const char *const system_images[] =
{
    "/drive_c/windows/system32/services.exe",
    "/drive_c/windows/system32/wineboot.exe",
    "/drive_c/windows/system32/winedevice.exe",
    "/drive_c/windows/syswow64/wineboot.exe",
    "/drive_c/windows/syswow64/winedevice.exe",
};

static void kill_unverified( void *private )
{
    struct process *process = private;

    kill_process( process, 1 );
    release_object( process );
}

void sysrun_main_image( struct process *process, struct fd *fd )
{
    char path[PATH_MAX];
    struct stat st, ref;
    struct token *token;
    unsigned int i;
    int unix_fd;

    if (!process->sysrun_pending) return;
    process->sysrun_pending = 0;
    /* device and inode of the mapped file, never the NT path: that goes through the user's prefix links */
    if (fd && (unix_fd = get_unix_fd( fd )) != -1 && !fstat( unix_fd, &st ) && S_ISREG( st.st_mode ))
    {
        for (i = 0; i < ARRAY_SIZE(system_images); i++)
        {
            if (snprintf( path, sizeof(path), "%s%s", machine_root, system_images[i] ) >= sizeof(path)) continue;
            if (stat( path, &ref ) || ref.st_dev != st.st_dev || ref.st_ino != st.st_ino) continue;
            if (!(token = token_create_service( SERVICE_ACCOUNT_SYSTEM, process->session_id ))) break;
            release_object( process->token );
            process->token = token;
            return;
        }
    }
    fprintf( stderr, "wineserver: process %04x has the system group but its image is not a system program of the machine root\n",
             process->id );
    process->peer_gid = -1;  /* no more raised file access from now on */
    /* killed from the main loop: this runs in a request of the process itself */
    grab_object( process );
    add_timeout_user( -TICKS_PER_SEC / 100, kill_unverified, process );
}

int sysrun_check_process_access( struct process *target, unsigned int access )
{
    if (!target->sysrun_protected || target == current->process) return 1;
    return check_object_access( NULL, &target->obj, &access );
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
