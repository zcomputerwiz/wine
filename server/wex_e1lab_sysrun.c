/*
 * WineExplorer E1 lab: the S6 endorsement, the lab generations and the broker image (never in a shipped build)
 *
 * Copyright 2026 WineExplorer contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Moved out of sysrun.c by R5 of docs/SESSION0_SPLIT_REVIEW.md, with no change of behaviour. sysrun.c keeps
 * sysrun_e1s6_endorse (it refuses without the lab switches) and calls e1lab_sysrun_main_image when a process maps
 * its first executable image; the generation functions of sysrun.h are defined here. wex_e1lab.h lists the hooks.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <poll.h>

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
#include "session0.h"
#include "wex_e1lab.h"

#ifdef WEX_E1S6_TRANSPORT_TEST

#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)

#ifdef WEX_E1_TEST_HOOKS
#define E1S6_AUDIT(...) fprintf( stderr, __VA_ARGS__ )
#else
#define E1S6_AUDIT(...) do { } while (0)
#endif
#define E1S6_REQUIRE(c, site) do { if (!(c)) { \
    E1S6_AUDIT( "E1S6_DENY server site=%s errno=%d\n", site, errno ); goto done; \
} } while (0)

static int e1s6_get_fd( const unsigned char *p, const char *role )
{
    obj_handle_t handle = p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
    struct object *obj = get_handle_obj( current->process, handle, 0, NULL );
    struct fd *fd;
    int ret = -1;
    if (!obj)
    {
        E1S6_AUDIT( "E1S6_DENY server site=endorse-%s-handle status=%08x\n", role, get_error() );
        return -1;
    }
    if ((fd = get_obj_fd( obj )))
    {
        ret = fcntl( get_unix_fd( fd ), F_DUPFD_CLOEXEC, 0 );
        if (ret < 0)
            E1S6_AUDIT( "E1S6_DENY server site=endorse-%s-dup-fd errno=%d status=%08x\n",
                        role, errno, get_error() );
        release_object( fd );
    }
    else E1S6_AUDIT( "E1S6_DENY server site=endorse-%s-fd-object status=%08x\n", role, get_error() );
    release_object( obj );
    return ret;
}

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

/* The machine root's generation: a new value whenever the root, or one of the directories the chain's paths go
 * through, is another object or has another owner or mode (replaced, moved aside, opened up). The root
 * directory's own size and times are not part of it: with session 0 the machine hives are saved into it, a new
 * file renamed over the old one, every few seconds while they change, and each save changed the directory's
 * times and so ended every elevation request in flight (found by the R9 lab cases, 2026-11-13). */
int sysrun_get_root_generation( unsigned long long *root_gen )
{
    static const char * const parts[] = { "", "/drive_c", "/drive_c/windows", "/drive_c/windows/system32",
                                          "/dosdevices" };
    static unsigned long long last_hash, tag;
    static int initialized;
    const char *root = sysrun_machine_root();
    unsigned long long hash = 0xcbf29ce484222325ull, values[5], fresh = 0;
    char path[PATH_MAX];
    struct stat st;
    unsigned int i, j, k;
    int fd;

    if (!root || !root[0] || !root_gen) return 0;
    for (i = 0; i < ARRAY_SIZE(parts); i++)
    {
        if (snprintf( path, sizeof(path), "%s%s", root, parts[i] ) >= (int)sizeof(path)) return 0;
        /* the root as it is named (it may be reached through a link); below it, the entries themselves */
        if (i ? lstat( path, &st ) : stat( path, &st ))
        {
            if (!i) return 0;
            memset( &st, 0, sizeof(st) );  /* a missing part is a state of its own */
        }
        values[0] = st.st_dev; values[1] = st.st_ino; values[2] = st.st_uid; values[3] = st.st_gid;
        values[4] = st.st_mode;
        for (j = 0; j < ARRAY_SIZE(values); j++)
            for (k = 0; k < 8; k++) hash = (hash ^ ((values[j] >> (8 * k)) & 0xff)) * 0x100000001b3ull;
    }
    if (!initialized || hash != last_hash)
    {
        if ((fd = open( "/dev/urandom", O_RDONLY | O_CLOEXEC )) < 0) return 0;
        while (!fresh)
        {
            if (read( fd, &fresh, sizeof(fresh) ) != sizeof(fresh))
            {
                close( fd );
                return 0;
            }
        }
        close( fd );
        last_hash = hash;
        tag = fresh;
        initialized = 1;
    }
    *root_gen = tag;
    return 1;
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

/* Direct kernel-authenticated endorsement to the verifier's private FD. The
 * caller has already passed the S3 consumed-opener/writer comparisons. This
 * operation neither issues tokens nor accepts authentication results. */
int e1lab_sysrun_e1s6_endorse( const void *data, unsigned int size )
{
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
    if (session0_server)
    {
        /* E1 R9: session 0 endorses its broker B0, one of its own LocalSystem programs (checked below); the
         * launcher's system role does not exist here */
        E1S6_REQUIRE( !p->wex_registry_client, "endorse-session0-own-program" );
    }
    else
    {
        E1S6_REQUIRE( sysrun_process_is_system( p ), "endorse-system-role" );
        E1S6_REQUIRE( sysrun_process_privileged( p ), "endorse-sysrun-connection" );
    }
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
        E1S6_REQUIRE( !getsockopt( socket_fd, SOL_SOCKET, SO_PEERPIDFD, &verifier_pins[i].fd, &len ) &&
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
    /* session 0 runs as its own account with the system group as its group: nothing to raise */
    raised = session0_server ? 1 : sysrun_raise();
    if (!raised) E1S6_AUDIT( "E1S6_DENY server site=endorse-raise-group errno=%d\n", errno );
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
        if (!ret) E1S6_AUDIT( "E1S6_DENY server site=endorse-send errno=%d\n", errno );
    }
    if (!session0_server) sysrun_drop( raised );
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
    return ret ? 1 : 0;
}

#endif /* WEX_E1S6_TRANSPORT_TEST && __linux__ */

/* sysrun_main_image, with the lab switch: is the first executable image the broker (or the lab's test broker)? */
void e1lab_sysrun_main_image( struct process *process, struct fd *fd, int system_role )
{
    char path[PATH_MAX];
    struct stat st, ref;
    unsigned int i;
    int unix_fd;

    /* The role follows the actual mapped file, never a client image string,
     * SYSTEM SID alone, a parent claim or inherited socket credentials. */
    if (!process->e1s6_image_seen && system_role && fd && (unix_fd = get_unix_fd( fd )) != -1 &&
        !fstat( unix_fd, &st ) && S_ISREG( st.st_mode ))
    {
        static const char *const brokers[] = { "wexelevate.exe",
#ifdef WEX_E1_TEST_HOOKS
                                                "wexelevate_test.exe"
#endif
                                              };
        for (i = 0; i < ARRAY_SIZE(brokers); i++)
            if ((unsigned int)snprintf( path, sizeof(path), "%s/drive_c/windows/system32/%s", sysrun_machine_root(), brokers[i] ) < sizeof(path) &&
                !stat( path, &ref ) && ref.st_dev == st.st_dev && ref.st_ino == st.st_ino)
                process->e1s6_broker_image = 1;
    }
    /* E1 R9: session 0's broker (B0) is the same image, mapped by one of session 0's own programs; session 0's
     * directory is the machine root */
    if (!process->e1s6_image_seen && session0_server && !process->wex_registry_client && fd &&
        (unix_fd = get_unix_fd( fd )) != -1 && !fstat( unix_fd, &st ) && S_ISREG( st.st_mode ))
    {
        static const char *const brokers[] = { "wexelevate.exe",
#ifdef WEX_E1_TEST_HOOKS
                                                "wexelevate_test.exe"
#endif
                                              };
        for (i = 0; i < ARRAY_SIZE(brokers); i++)
            if ((unsigned int)snprintf( path, sizeof(path), "drive_c/windows/system32/%s", brokers[i] ) < sizeof(path) &&
                !fstatat( config_dir_fd, path, &ref, 0 ) && ref.st_dev == st.st_dev && ref.st_ino == st.st_ino)
                process->e1s6_broker_image = 1;
    }
    process->e1s6_image_seen = 1;
}

#endif /* WEX_E1S6_TRANSPORT_TEST */
