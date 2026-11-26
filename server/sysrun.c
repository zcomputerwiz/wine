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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
#include "wine/wex_sysrun_conf.h"
#include "security.h"
#include "file.h"
#include "handle.h"
#include "session0.h"
#include "wex_e1lab.h"

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

/* setegid, then not dumpable again, with every signal held in between (B106): the credential change sets the flag to
 * fs.suid_dumpable, and with 1 the user, who may signal this server, could time a SIGQUIT into that window and get
 * a core of it. A signal sent meanwhile is delivered once the flag is 0 again. Returns setegid's result. */
static int set_egid( gid_t gid )
{
    sigset_t all, old;
    int ret, err;

    sigfillset( &all );
    sigprocmask( SIG_BLOCK, &all, &old );
    ret = setegid( gid );
    err = errno;
    set_not_dumpable();
    sigprocmask( SIG_SETMASK, &old, NULL );
    errno = err;
    return ret;
}

enum sysrun_conf sysrun_conf_get( const char *key, char *value, size_t size )
{
    struct wex_sysrun_conf conf;

    wex_sysrun_conf_load( SYSRUN_CONF, &conf );
    switch (wex_sysrun_conf_get( &conf, key, value, size ))
    {
    case WEX_SYSRUN_CONF_ABSENT: return SYSRUN_CONF_ABSENT;
    case WEX_SYSRUN_CONF_VALUE: return SYSRUN_CONF_VALUE;
    default: return SYSRUN_CONF_INVALID;
    }
}

/* "group" and "machine_root" of the configuration, each given exactly once */
static int read_config( char *group, size_t group_size )
{
    struct wex_sysrun_conf conf;

    machine_root[0] = 0;
    if (!wex_sysrun_conf_load( SYSRUN_CONF, &conf ) ||
        wex_sysrun_conf_get( &conf, "group", group, group_size ) != WEX_SYSRUN_CONF_VALUE ||
        wex_sysrun_conf_get( &conf, "machine_root", machine_root, sizeof(machine_root) ) != WEX_SYSRUN_CONF_VALUE)
    {
        machine_root[0] = 0;
        return 0;
    }
    return machine_root[0] == '/';
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
    if (set_egid( user_gid )) fatal_error( "cannot drop the system group: %s\n", strerror( errno ));
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
    if (set_egid( system_gid )) return 0;  /* the operation then fails with the user's rights */
    is_raised = 1;
    errno = err;
    return 1;
}

void sysrun_drop( int raised )
{
    int err = errno;

    if (!raised) return;
    if (set_egid( user_gid )) fatal_error( "cannot drop the system group: %s\n", strerror( errno ));
    is_raised = 0;
    errno = err;
}

int sysrun_process_privileged( const struct process *process )
{
    return sysrun_active && process && process->peer_uid == getuid() && process->peer_gid == system_gid;
}

/* The system role's authority (WineExplorer R2): a process started over the system role connection
 * (sysrun_process_privileged) whose primary token is SYSTEM's and holds SeTcbPrivilege. Wine's administrator token
 * holds SeTcbPrivilege too, so the privilege alone proves nothing; SYSTEM's token alone is not enough either, since
 * the connection is what the launcher vouches for. Session 0 has no system role connection: callers there decide
 * for themselves. */
int sysrun_process_is_tcb_system( struct process *process )
{
    return sysrun_process_privileged( process ) && process->token &&
           token_service_account( process->token ) == SERVICE_ACCOUNT_SYSTEM &&
           token_has_privilege( process->token, SeTcbPrivilege );
}

int sysrun_raise_for_current(void)
{
    return sysrun_active && current && sysrun_process_privileged( current->process ) ? sysrun_raise() : 0;
}

/* ---- the system role (C3) ---------------------------------------------------------------------------------- */

/* The live value. A latch at the end of init_registry (B112 hardening, batch 1137) read the role as off for a server
 * whose prefix gets its elevation helper value after the server started (a new prefix: the suite's, setup's), which
 * then never started winedevice.exe with SYSTEM's token; the flip the latch was for is in docs/KNOWN_BUGS.md (B115).
 *
 * With the role off a launcher-started server keeps the creator's system group for a child that has neither a
 * service token nor an elevated one (sysrun_process_created returns first). That mode is only for test images
 * (WEX_ELEVATION=0): every installed machine has the role on since 2026-11-23 (setup-machine-root.sh). */
static int system_role_enabled(void)
{
    return sysrun_active && elevation_enforced();
}

/* Safe Mode (docs/SAFE_MODE.md): /run/wineexplorer/bootmode, a regular file that root owns and others cannot write,
 * in a root-owned directory that others cannot write, says safe, safenet or safecmd. Read once: the mode does not
 * change during a start. The same source of truth as the launcher's and the broker's; SafeBoot\Option is not used
 * (an elevated program can write it). */
int sysrun_safe_boot(void)
{
    static int safe = -1;
    struct stat st;
    char buf[16];
    ssize_t n;
    int fd;

    if (safe != -1) return safe;
    safe = 0;
    if (lstat( "/run/wineexplorer", &st ) || !S_ISDIR( st.st_mode ) || st.st_uid || (st.st_mode & 022)) return 0;
    if ((fd = open( "/run/wineexplorer/bootmode", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC )) == -1) return 0;
    if (!fstat( fd, &st ) && S_ISREG( st.st_mode ) && !st.st_uid && !(st.st_mode & 022) &&
        (n = read( fd, buf, sizeof(buf) - 1 )) > 0)
    {
        buf[n] = 0;
        buf[strcspn( buf, "\r\n" )] = 0;
        safe = !strcmp( buf, "safe" ) || !strcmp( buf, "safenet" ) || !strcmp( buf, "safecmd" );
    }
    close( fd );
    return safe;
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

/* The machine root's images that the server trusts, and for what. Each is recognised by the device and inode of
 * the mapped file (machine_image_roles), never by a path string. */
#define IMAGE_SYSTEM      0x1  /* may be the main image of a system group root process (it gets SYSTEM's token) */
#define IMAGE_BROKER      0x2  /* the installed elevation broker (production names only) */
#define IMAGE_DIALOG      0x4  /* E1-S18e R8 step 3c: the credentials dialog */
#define IMAGE_SAFE_SHELL  0x8  /* an administrator's elevated desktop in Safe Mode */

static const struct
{
    const char  *path;   /* below the machine root */
    unsigned int roles;
} machine_images[] =
{
    { "/drive_c/windows/system32/services.exe",   IMAGE_SYSTEM },
    { "/drive_c/windows/system32/wexelevate.exe", IMAGE_SYSTEM | IMAGE_BROKER },
    { "/drive_c/windows/system32/wineboot.exe",   IMAGE_SYSTEM },
    { "/drive_c/windows/system32/winedevice.exe", IMAGE_SYSTEM },
    { "/drive_c/windows/syswow64/wineboot.exe",   IMAGE_SYSTEM },
    { "/drive_c/windows/syswow64/winedevice.exe", IMAGE_SYSTEM },
    { "/drive_c/windows/syswow64/wexelevate.exe", IMAGE_BROKER },
    { "/drive_c/windows/system32/consent.exe",    IMAGE_DIALOG },
    { "/drive_c/windows/syswow64/consent.exe",    IMAGE_DIALOG },
    { "/drive_c/windows/explorer.exe",            IMAGE_SAFE_SHELL },
};

static void kill_unverified( void *private )
{
    struct process *process = private;

    kill_process( process, 1 );
    release_object( process );
}

/* the roles, out of those asked, of the machine-root image that st is (device and inode); 0 when none */
static unsigned int machine_image_roles( const struct stat *st, unsigned int asked )
{
    char path[PATH_MAX];
    struct stat ref;
    unsigned int i, roles = 0;

    for (i = 0; i < ARRAY_SIZE(machine_images); i++)
    {
        if (!(machine_images[i].roles & asked)) continue;
        if ((unsigned int)snprintf( path, sizeof(path), "%s%s", machine_root, machine_images[i].path ) >= sizeof(path))
            continue;
        if (!stat( path, &ref ) && ref.st_dev == st->st_dev && ref.st_ino == st->st_ino)
            roles |= machine_images[i].roles & asked;
    }
    return roles;
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
    struct stat st;
    struct token *token;
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
            unsigned int roles = machine_image_roles( &st, IMAGE_BROKER | (process->wex_creator ? IMAGE_DIALOG : 0) );

            broker = !!(roles & IMAGE_BROKER);
            /* R8 3c: a protected dialog whose first executable image is the machine-root consent.exe;
             * mapping it later, as a second executable, never counts */
            process->wex_dialog_image = !!(roles & IMAGE_DIALOG);
#ifdef WEX_R8_SERVER_TEST
            if (process->wex_creator)
                fprintf( stderr, "wex-r8: dialog %04x image consent=%d\n", process->id, process->wex_dialog_image );
#endif
        }
    }

#ifdef WEX_E1S6_TRANSPORT_TEST
    e1lab_sysrun_main_image( process, fd, system_role_enabled() );  /* R5: wex_e1lab_sysrun.c */
#endif
    /* only sysrun_process_created sets sysrun_pending, and only with the system role; checking the role here too
     * keeps a stale bit (B112) from ever giving a process SYSTEM's token */
    if (!process->sysrun_pending || !system_role_enabled())
    {
        process->sysrun_pending = 0;
        if (broker) pin_broker( process );
        return;
    }
    process->sysrun_pending = 0;
    /* device and inode of the mapped file, never the NT path: that goes through the user's prefix links */
    if (fd && (unix_fd = get_unix_fd( fd )) != -1 && !fstat( unix_fd, &st ) && S_ISREG( st.st_mode ))
    {
        /* An administrator's elevated desktop in Safe Mode (the launcher's safe-shell): the machine root's
         * explorer.exe, with the full token this server gives an administrator's programs only in Safe Mode. It keeps
         * that token (not SYSTEM's) and is protected as an elevated process, as one the broker starts
         * (docs/SAFE_MODE_ELEVATED_DESKTOP.md). Without both, it is ended below as any other image. */
        if (sysrun_safe_boot() && token_is_elevated( process->token ) &&
            machine_image_roles( &st, IMAGE_SAFE_SHELL ))
        {
            struct thread *thread;
            int ok;

            process->sysrun_account = SYSRUN_ACCOUNT_ELEVATED;
            ok = token_protect_elevated( process->token ) &&
                 protect_object( &process->obj, process->token, SYSRUN_ACCOUNT_ELEVATED, 0 );
            /* the threads it has were protected as SYSTEM's when it was created: they get the elevated descriptor */
            LIST_FOR_EACH_ENTRY( thread, &process->thread_list, struct thread, proc_entry )
                if (ok) ok = protect_object( &thread->obj, process->token, SYSRUN_ACCOUNT_ELEVATED, 1 );
            if (ok) return;
        }
        if (machine_image_roles( &st, IMAGE_SYSTEM ) &&
            (token = token_create_service( SERVICE_ACCOUNT_SYSTEM, process->session_id )))
        {
            /* R7: the descriptor of a system process that is not a service (its services get theirs from
             * create_service_token); without it the process is ended below */
            if (token_protect_system( token, 0 ))
            {
                release_object( process->token );
                process->token = token;
                if (broker) pin_broker( process );
                return;
            }
            release_object( token );
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

#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1_TEST_HOOKS)
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

/* The S6 lab endorsement (wex_e1lab_sysrun.c); refused without the lab switches. */
int sysrun_e1s6_endorse( const void *data, unsigned int size )
{
#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    if (e1lab_sysrun_e1s6_endorse( data, size )) return 1;
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

/* name lookups of the same paths (B28): a directory only the system group can search hides its files' names from
 * the user's rights, so a file opened there had no name and could not be renamed */
int sysrun_stat( const char *path, struct stat *st )
{
    int raised = sysrun_raise_for_current(), ret = stat( path, st );
    sysrun_drop( raised );
    return ret;
}

int sysrun_lstat( const char *path, struct stat *st )
{
    int raised = sysrun_raise_for_current(), ret = lstat( path, st );
    sysrun_drop( raised );
    return ret;
}

char *sysrun_realpath( const char *path )
{
    int raised = sysrun_raise_for_current();
    char *ret = realpath( path, NULL );
    sysrun_drop( raised );
    return ret;
}
