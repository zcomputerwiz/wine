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
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
# include <poll.h>
#endif
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
#include "handle.h"

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

/* sysrun_account of an elevated process (the broker's); the service accounts are SERVICE_ACCOUNT_* */
#define SYSRUN_ACCOUNT_ELEVATED 3

/* Give a process or thread of the system role the descriptor of a Windows service process: SYSTEM (and the service
 * account) full control; Administrators query and read the descriptor, so full access needs SeDebugPrivilege;
 * Users and Authenticated Users only the limited query and SYNCHRONIZE, so that task managers can list it.
 * The owner and group come from the token. */
static int protect_object( struct object *obj, struct token *token, int account, int is_thread )
{
    /* an elevated process (B8): the full token's Administrators have full control of it, as they have of any
     * elevated process, but the user's limited token (Administrators is deny-only in it) has the limited query only. The
     * owner is the token's owner, Administrators, never the user: an owner always gets WRITE_DAC. */
    const unsigned int admin_mask = account == SYSRUN_ACCOUNT_ELEVATED ? GENERIC_ALL :
                                    is_thread ? THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL
                                              : PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL;
    const unsigned int user_mask = (is_thread ? THREAD_QUERY_LIMITED_INFORMATION : PROCESS_QUERY_LIMITED_INFORMATION) | SYNCHRONIZE;
    const struct sid *acct = account == SERVICE_ACCOUNT_LOCAL_SERVICE ? &local_service_account_sid :
                             account == SERVICE_ACCOUNT_NETWORK_SERVICE ? &network_service_account_sid : NULL;
    size_t acl_size = sizeof(struct acl) + 4 * sizeof(struct ace) + sid_len( &local_system_sid ) + sid_len( &builtin_admins_sid ) +
                      sid_len( &builtin_users_sid ) + sid_len( &authenticated_users_sid ) + (acct ? sid_len( acct ) + sizeof(struct ace) : 0);
    size_t owner_len = account == SYSRUN_ACCOUNT_ELEVATED ? sid_len( &builtin_admins_sid ) : 0;
    size_t size = sizeof(struct security_descriptor) + owner_len + acl_size;
    struct security_descriptor *sd;
    struct acl *acl;
    struct ace *ace;
    int ret;

    if (!(sd = mem_alloc( size ))) return 0;
    memset( sd, 0, size );
    sd->control = SE_DACL_PRESENT;
    sd->dacl_len = acl_size;
    if (owner_len)  /* the token's owner would be the user */
    {
        sd->owner_len = owner_len;
        memcpy( sd + 1, &builtin_admins_sid, owner_len );
    }
    acl = (struct acl *)((char *)(sd + 1) + owner_len);
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
    if ((account = token_service_account( process->token )) < 0)
    {
        /* The connection credentials are those of the process that made the socket pair, so a process that was
         * started by one with the system group looks as if it had it too. Only an elevated process (its Unix process keeps
         * the group, ntdll sysrun_must_drop_group) keeps the raised file access; any other loses it. */
        if (!token_is_elevated( process->token ))
        {
            process->peer_gid = -1;
            return 1;
        }
        process->sysrun_protected = 1;
        process->sysrun_account = SYSRUN_ACCOUNT_ELEVATED;
        return token_protect_elevated( process->token ) &&
               protect_object( &process->obj, process->token, SYSRUN_ACCOUNT_ELEVATED, 0 );
    }
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
    "/drive_c/windows/system32/wexelevate.exe",
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

/* is st one of the machine-root images in names (both ABIs), by device and inode? */
static int is_machine_image( const struct stat *st, const char *const *names, unsigned int count )
{
    char path[PATH_MAX];
    struct stat ref;
    unsigned int i;

    for (i = 0; i < count; i++)
        if ((unsigned int)snprintf( path, sizeof(path), "%s%s", machine_root, names[i] ) < sizeof(path) &&
            !stat( path, &ref ) && ref.st_dev == st->st_dev && ref.st_ino == st->st_ino) return 1;
    return 0;
}

/* the installed elevation broker images (production names only) */
static int is_broker_image( const struct stat *st )
{
    static const char *const brokers[] =
    {
        "/drive_c/windows/system32/wexelevate.exe",
        "/drive_c/windows/syswow64/wexelevate.exe",
    };
    return is_machine_image( st, brokers, ARRAY_SIZE(brokers) );
}

/* E1-S18e R8 step 3c: the installed credentials dialog images */
static int is_dialog_image( const struct stat *st )
{
    static const char *const dialogs[] =
    {
        "/drive_c/windows/system32/consent.exe",
        "/drive_c/windows/syswow64/consent.exe",
    };
    return is_machine_image( st, dialogs, ARRAY_SIZE(dialogs) );
}

/* the broker identity holds only with the primary token it had when its image was recognised */
static void pin_broker( struct process *process )
{
    struct luid modified;

    token_get_ids( process->token, &process->wex_broker_token_id, &modified );
    process->wex_broker_image = 1;
}

void sysrun_main_image( struct process *process, struct fd *fd )
{
    char path[PATH_MAX];
    struct stat st, ref;
    struct token *token;
    unsigned int i;
    int unix_fd, broker = 0;

    /* The elevation broker is recognised by the actual mapped file (device and inode of the
     * machine-root wexelevate.exe), whether services.exe started it or not; never by an image
     * string, the SYSTEM SID alone or a parent claim. Decided once, on the first executable
     * image: a process that maps the broker later, as a second executable, never becomes it. */
    if (!process->wex_image_seen)
    {
        process->wex_image_seen = 1;
        if (system_role_enabled() && fd && (unix_fd = get_unix_fd( fd )) != -1 &&
            !fstat( unix_fd, &st ) && S_ISREG( st.st_mode ))
        {
            broker = is_broker_image( &st );
            /* R8 3c: a protected dialog whose first executable image is the machine-root consent.exe;
             * mapping it later, as a second executable, never counts */
            process->wex_dialog_image = process->wex_creator && is_dialog_image( &st );
#ifdef WEX_R8_SERVER_TEST
            if (process->wex_creator)
                fprintf( stderr, "wex-r8: dialog %04x image consent=%d\n", process->id, process->wex_dialog_image );
#endif
        }
    }

#ifdef WEX_E1S6_TRANSPORT_TEST
    /* The role follows the actual mapped file, never a client image string,
     * SYSTEM SID alone, a parent claim or inherited socket credentials. */
    if (!process->e1s6_image_seen && system_role_enabled() && fd && (unix_fd = get_unix_fd( fd )) != -1 &&
        !fstat( unix_fd, &st ) && S_ISREG( st.st_mode ))
    {
        static const char *const brokers[] = { "wexelevate.exe", "wexelevate_test.exe" };
        for (i = 0; i < ARRAY_SIZE(brokers); i++)
            if ((unsigned int)snprintf( path, sizeof(path), "%s/drive_c/windows/system32/%s", machine_root, brokers[i] ) < sizeof(path) &&
                !stat( path, &ref ) && ref.st_dev == st.st_dev && ref.st_ino == st.st_ino)
                process->e1s6_broker_image = 1;
    }
    process->e1s6_image_seen = 1;
#endif
    if (!process->sysrun_pending)
    {
        if (broker) pin_broker( process );
        return;
    }
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
            if (broker) pin_broker( process );
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

int sysrun_process_is_broker( struct process *process )
{
    struct luid id, modified;

#ifdef WEX_E1S6_TRANSPORT_TEST
    /* lab only: the test broker image; it has no pinned primary */
    if (process->e1s6_broker_image && sysrun_process_is_system( process ) &&
        token_service_account( process->token ) == SERVICE_ACCOUNT_SYSTEM) return 1;
#endif
    if (!process->wex_broker_image || !sysrun_process_is_system( process ) ||
        token_service_account( process->token ) != SERVICE_ACCOUNT_SYSTEM) return 0;
    token_get_ids( process->token, &id, &modified );
    return !memcmp( &id, &process->wex_broker_token_id, sizeof(id) );
}

int sysrun_desktops_enforced(void)
{
    return system_role_enabled();
}

int sysrun_process_is_system( struct process *process )
{
    return system_role_enabled() && process->sysrun_protected && !process->sysrun_pending &&
           process->sysrun_account == SERVICE_ACCOUNT_SYSTEM;
}

/* Direct kernel-authenticated endorsement to the verifier's private FD. The
 * caller has already passed the S3 consumed-opener/writer comparisons. This
 * operation neither issues tokens nor accepts authentication results. */
#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
#define SO_PEERPIDFD_LAB 77
#define E1S6_REQUIRE(c, site) do { if (!(c)) { \
    fprintf( stderr, "E1S6_DENY server site=%s errno=%d\n", site, errno ); goto done; \
} } while (0)

static int e1s6_get_fd( const unsigned char *p, const char *role )
{
    obj_handle_t handle = p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
    struct object *obj = get_handle_obj( current->process, handle, 0, NULL );
    struct fd *fd;
    int ret = -1;
    if (!obj)
    {
        fprintf( stderr, "E1S6_DENY server site=endorse-%s-handle status=%08x\n", role, get_error() );
        return -1;
    }
    if ((fd = get_obj_fd( obj )))
    {
        ret = fcntl( get_unix_fd( fd ), F_DUPFD_CLOEXEC, 0 );
        if (ret < 0)
            fprintf( stderr, "E1S6_DENY server site=endorse-%s-dup-fd errno=%d status=%08x\n",
                     role, errno, get_error() );
        release_object( fd );
    }
    else fprintf( stderr, "E1S6_DENY server site=endorse-%s-fd-object status=%08x\n", role, get_error() );
    release_object( obj );
    return ret;
}
#endif

#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
static unsigned long long s6_server_generation;

struct stat_gen_tracker
{
    int initialized;
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtim;
    struct timespec ctim;
    unsigned long long tag;
};

static int get_path_generation( const char *path, struct stat_gen_tracker *tracker, unsigned long long *gen )
{
    struct stat st;
    int fd;
    unsigned long long new_tag = 0;

    if (!path || !path[0] || !tracker || !gen) return 0;
    if (stat( path, &st ) != 0) return 0;

    if (!tracker->initialized ||
        tracker->dev != st.st_dev ||
        tracker->ino != st.st_ino ||
        tracker->size != st.st_size ||
        tracker->mtim.tv_sec != st.st_mtim.tv_sec ||
        tracker->mtim.tv_nsec != st.st_mtim.tv_nsec ||
        tracker->ctim.tv_sec != st.st_ctim.tv_sec ||
        tracker->ctim.tv_nsec != st.st_ctim.tv_nsec)
    {
        fd = open( "/dev/urandom", O_RDONLY | O_CLOEXEC );
        if (fd < 0) return 0;
        while (!new_tag)
        {
            if (read( fd, &new_tag, sizeof(new_tag) ) != sizeof(new_tag))
            {
                close( fd );
                return 0;
            }
        }
        close( fd );
        tracker->dev = st.st_dev;
        tracker->ino = st.st_ino;
        tracker->size = st.st_size;
        tracker->mtim = st.st_mtim;
        tracker->ctim = st.st_ctim;
        tracker->tag = new_tag;
        tracker->initialized = 1;
    }
    *gen = tracker->tag;
    return 1;
}

int sysrun_get_root_generation( unsigned long long *root_gen )
{
    static struct stat_gen_tracker root_tracker;
    const char *root = sysrun_machine_root();
    if (!root) return 0;
    return get_path_generation( root, &root_tracker, root_gen );
}

int sysrun_get_policy_generation( unsigned long long *policy_gen )
{
    static struct stat_gen_tracker policy_tracker;
    return get_path_generation( SYSRUN_CONF, &policy_tracker, policy_gen );
}

int sysrun_get_s6_generations( struct process *process, unsigned long long *server_gen, unsigned long long *broker_gen )
{
    if (!process || !server_gen || !broker_gen) return 0;
    if (!s6_server_generation || !process->e1s6_broker_generation) return 0;
    if (process->e1s6_primary != process->token) return 0;
    *server_gen = s6_server_generation;
    *broker_gen = process->e1s6_broker_generation;
    return 1;
}
#endif

int sysrun_e1s6_endorse( const void *data, unsigned int size )
{
#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    const unsigned char *in = data;
    unsigned char out[96];
    struct ucred peer;
    socklen_t len = sizeof(peer);
    struct process *p = current->process;
    struct token *new_primary = NULL;
    struct luid new_id, new_modified;
    struct pollfd verifier_pins[2] = { { -1, POLLIN, 0 }, { -1, POLLIN, 0 } };
    struct ucred verifier[2];
    unsigned long long fresh, server_generation = s6_server_generation, expected = 0;
    int fd = -1, pin = -1, data_fd = -1, random = -1, raised, ret = 0, type;
    unsigned int i;

    /* Handles belong to this broker's handle table. The client closes its
     * imported handles on every exit; duplicate native fds are local here. */
    E1S6_REQUIRE( size == sizeof(out), "endorse-size" );
    fd = e1s6_get_fd( in, "control" );
    E1S6_REQUIRE( fd >= 0, "endorse-control-fd" );
    pin = e1s6_get_fd( in + 4, "pin" );
    E1S6_REQUIRE( pin >= 0, "endorse-pin-fd" );
    data_fd = e1s6_get_fd( in + 16, "data" );
    E1S6_REQUIRE( data_fd >= 0, "endorse-data-fd" );
    E1S6_REQUIRE( !current->token, "endorse-thread-token" );
    E1S6_REQUIRE( p->e1s6_broker_image, "endorse-broker-image" );
    E1S6_REQUIRE( sysrun_process_is_system( p ), "endorse-system-role" );
    E1S6_REQUIRE( sysrun_process_privileged( p ), "endorse-sysrun-connection" );
    E1S6_REQUIRE( token_service_account( p->token ) == SERVICE_ACCOUNT_SYSTEM, "endorse-system-primary" );
    E1S6_REQUIRE( token_has_privilege( p->token, SeTcbPrivilege ), "endorse-tcb" );
    E1S6_REQUIRE( !getsockopt( pin, SOL_SOCKET, SO_PEERCRED, &peer, &len ) && len == sizeof(peer), "endorse-peercred" );
    E1S6_REQUIRE( peer.uid == getuid(), "endorse-peer-uid" );
    E1S6_REQUIRE( peer.pid > 0 && peer.pid == p->unix_pid, "endorse-peer-pid" );
    for (i = 0; i < 2; i++)
    {
        int socket_fd = i ? data_fd : fd;
        len = sizeof(type);
        E1S6_REQUIRE( !getsockopt( socket_fd, SOL_SOCKET, SO_TYPE, &type, &len ) &&
                     len == sizeof(type) && type == SOCK_SEQPACKET, "endorse-socket-type" );
        len = sizeof(verifier[i]);
        E1S6_REQUIRE( !getsockopt( socket_fd, SOL_SOCKET, SO_PEERCRED, &verifier[i], &len ) &&
                     len == sizeof(verifier[i]), "endorse-verifier-peercred" );
        E1S6_REQUIRE( verifier[i].uid == 0 && verifier[i].pid > 0, "endorse-verifier-root" );
        len = sizeof(verifier_pins[i].fd);
        E1S6_REQUIRE( !getsockopt( socket_fd, SOL_SOCKET, SO_PEERPIDFD_LAB, &verifier_pins[i].fd, &len ) &&
                     len == sizeof(verifier_pins[i].fd) && verifier_pins[i].fd >= 0, "endorse-verifier-peerpidfd" );
    }
    E1S6_REQUIRE( verifier[0].pid == verifier[1].pid, "endorse-verifier-same-peer" );
    /* Both kernel-issued pins must still be live after acquisition: matching
     * socket credential PIDs cannot then describe different PID lifetimes. */
    E1S6_REQUIRE( !poll( verifier_pins, 2, 0 ), "endorse-verifier-pins-live" );
    /* The verifier supplies root/verifier generations and connection nonce.
     * The server supplies its own instance and broker-registration generations.
     * No random value is accepted as a portable grant. */
    E1S6_REQUIRE( (random = open( "/dev/urandom", O_RDONLY | O_CLOEXEC )) >= 0, "endorse-rng-open" );
    if (!server_generation)
    {
        E1S6_REQUIRE( read( random, &server_generation, sizeof(server_generation) ) == (ssize_t)sizeof(server_generation) &&
                     server_generation, "endorse-server-generation" );
    }
    for (i = 0; i < 8; i++) expected |= (unsigned long long)in[8+i] << (8*i);
    fresh = expected;
    if (!expected)
    {
        E1S6_REQUIRE( read( random, &fresh, sizeof(fresh) ) == (ssize_t)sizeof(fresh) && fresh, "endorse-broker-generation" );
        E1S6_REQUIRE( fresh != p->e1s6_broker_generation, "endorse-generation-collision" );
        new_primary = (struct token *)grab_object( p->token );
        token_get_ids( p->token, &new_id, &new_modified );
    }
    else
    {
        struct luid id, modified;
        token_get_ids( p->token, &id, &modified );
        E1S6_REQUIRE( expected == p->e1s6_broker_generation, "endorse-stale-generation" );
        E1S6_REQUIRE( p->token == p->e1s6_primary, "endorse-primary-object" );
        E1S6_REQUIRE( !memcmp( &id, &p->e1s6_token_id, sizeof(id) ), "endorse-token-id" );
        E1S6_REQUIRE( !memcmp( &modified, &p->e1s6_modified_id, sizeof(modified) ), "endorse-modified-id" );
    }
    memcpy( out, in, sizeof(out) );
    /* Local sideband encoding is explicitly LE, separate from S1 kind 7. */
    for (i = 0; i < 8; i++) { out[i] = server_generation >> (8 * i); out[8+i] = fresh >> (8 * i); }
    for (i = 0; i < 4; i++) { out[16+i] = peer.pid >> (8 * i); out[20+i] = p->session_id >> (8 * i); }
    raised = sysrun_raise();
    if (!raised) fprintf( stderr, "E1S6_DENY server site=endorse-raise-group errno=%d\n", errno );
    if (raised)
    {
        /* Automatic SCM_CREDENTIALS uses real IDs on Linux. Explicitly send
         * the effective system gid; the kernel checks real/effective/saved
         * membership, so an ordinary same-UID peer cannot forge wexsys. */
        struct ucred sender = { getpid(), geteuid(), getegid() };
        char ancillary[CMSG_SPACE(sizeof(sender))];
        struct iovec io = { out, sizeof(out) };
        struct msghdr msg = { 0 };
        struct cmsghdr *cmsg;
        msg.msg_iov = &io; msg.msg_iovlen = 1;
        msg.msg_control = ancillary; msg.msg_controllen = sizeof(ancillary);
        cmsg = CMSG_FIRSTHDR( &msg ); cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_CREDENTIALS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(sender)); memcpy( CMSG_DATA(cmsg), &sender, sizeof(sender) );
        ret = sendmsg( fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL ) == (ssize_t)sizeof(out);
        if (!ret) fprintf( stderr, "E1S6_DENY server site=endorse-send errno=%d\n", errno );
    }
    sysrun_drop( raised );
    if (ret)
    {
        /* Publish only after the complete seqpacket was delivered. No fallible
         * operation follows this transfer of the staged token reference. */
        s6_server_generation = server_generation;
        if (p->e1s6_control_fd >= 0) close( p->e1s6_control_fd );
        p->e1s6_control_fd = fcntl( fd, F_DUPFD_CLOEXEC, 0 );
        if (new_primary)
        {
            if (p->e1s6_primary) release_object( p->e1s6_primary );
            p->e1s6_broker_generation = fresh;
            p->e1s6_primary = new_primary;
            p->e1s6_token_id = new_id;
            p->e1s6_modified_id = new_modified;
            new_primary = NULL;
        }
    }
done:
    if (new_primary) release_object( new_primary );
    for (i = 0; i < 2; i++) if (verifier_pins[i].fd >= 0) close( verifier_pins[i].fd );
    if (data_fd >= 0) close( data_fd );
    if (fd >= 0) close( fd );
    if (pin >= 0) close( pin );
    if (random >= 0) close( random );
    if (ret) return 1;
#endif
    set_error( STATUS_ACCESS_DENIED );
    return 0;
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
