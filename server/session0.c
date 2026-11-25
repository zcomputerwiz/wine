/*
 * WineExplorer system tier (session 0)
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

/* Session 0 holds the machine registry for every session (docs/SYSTEM_TIER_DESIGN.md, option D).
 *
 * A server started with --session0 listens on a SEQPACKET socket (session0_socket in the root-owned
 * /etc/wineexplorer/sysrun.conf, or WEX_SESSION0_SOCKET when there is no such line). A user's server connects
 * to it for each program that asks for a registry channel (request get_registry_channel), sends a description
 * of the program's token and one end of a socket pair, and hands the other end to the program. Session 0
 * serves its end as a registry-only client process with that token: it can make registry requests and nothing
 * else, and it has no Unix pid, so session 0 never signals or traces it.
 *
 * Trust: session 0 believes a token description only from a user's server that the launcher started (its
 * uids are the user's, its saved group is the system group and its real group is not), or, in
 * development (WEX_SESSION0_DEV=1 in session 0's own environment), from a server of session 0's own uid.
 * Anything else is refused. */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "object.h"
#include "process.h"
#include "request.h"
#include "security.h"
#include "sysrun.h"
#include "thread.h"
#include "e1s_token.h"
#include "accounts.h"
#include "session0.h"
#include "session0_x.h"
#include "wex_e1lab.h"
#include "wine/wex_session0.h"
#include "wine/wex_sysrun_peer.h"

#define SESSION0_CONF "/etc/wineexplorer/sysrun.conf"

/* the requests about a pipe: the token description, then the pipe's name */
static inline int is_pipe_request( unsigned int type )
{
    return type == WEX_SESSION0_PIPE || type == WEX_SESSION0_PIPE_WAIT;
}

int session0_server;
int session0_client;

static char session0_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int session0_dev;                /* development trust: a peer of session 0's own uid */
static gid_t peer_group;                /* production trust: the launcher's system group */
static uid_t hive_owner = (uid_t)-1;    /* the session 0 account, when it is configured */

/* what a link is (R2): each link starts as a request link with one message to come; a SESSION_ID request that is
 * granted makes it a session link (T6.2, session 0: orders go out on it), a USER_HIVE request that is granted a
 * hive link (T4a: on both sides, the agent of a user's hive) */
enum link_kind
{
    LINK_REQUEST,
    LINK_SESSION,
    LINK_HIVE
};

struct session0_socket
{
    struct object obj;
    struct fd    *fd;
    enum link_kind kind;
    int           trusted;              /* links: the peer may describe tokens */
    int           pending;              /* a link whose message has not come yet */
    struct timeout_user *timeout;       /* when a pending link is dropped */
    unsigned int  session_id;           /* T6.2, session 0: a session link, of this session */
    unsigned int  serial;               /* the serial of the last order sent */
    unsigned int  outstanding;          /* the serial of the order that waits for its answer, or 0 */
    uid_t         peer_uid;
    pid_t         peer_pid;
    struct list   entry;                /* in session_links */
    struct x_conn *x;                   /* X0: a session link carries X frames, both ways (session0_x.c) */
};

/* T6.2, session 0: the session link of each user's server, to send it orders. A user can start a few servers of
 * their own (one per prefix), each with its link; more than that per account are refused. */
#define SESSION0_MAX_SESSIONS_PER_USER 32
static struct list session_links = LIST_INIT( session_links );

/* session 0: the session of the server at the other end of LINK, from that server's session link; -1 without one */
static int peer_session( const struct session0_socket *link )
{
    struct session0_socket *other;
    int session = -1;

    LIST_FOR_EACH_ENTRY( other, &session_links, struct session0_socket, entry )
        if (other->peer_pid == link->peer_pid && other->peer_uid == link->peer_uid) session = other->session_id;
    return session;
}

/* Session ids are never given twice, also across restarts of session 0, while servers of the old session 0 still
 * run: the next id is kept in session0.ids in session 0's directory, reserved in blocks so that the file is
 * written rarely. Every id given out is below the number in the file. */
#define SESSION_ID_BLOCK 64
static unsigned int next_session_id;      /* 0: the file was not read yet */
static unsigned int reserved_session_id;

/* R9: the account each session id was given to. An id is given again only to that account, also after a restart
 * of session 0: the owners are kept in session0.owners ("id uid" lines, appended before the answer; the last
 * SESSION0_MAX_OWNERS are read at start). An id without a record is not given back. */
#define SESSION0_MAX_OWNERS 65536
struct session_id_owner
{
    unsigned int id;
    uid_t        uid;
};
static struct session_id_owner *id_owners;
static unsigned int id_owner_count, id_owner_capacity;
static int id_owners_loaded;

static int add_id_owner( unsigned int id, uid_t uid )
{
    unsigned int i;

    for (i = 0; i < id_owner_count; i++)
        if (id_owners[i].id == id)
        {
            id_owners[i].uid = uid;  /* a later record wins */
            return 1;
        }
    if (id_owner_count == SESSION0_MAX_OWNERS)
    {
        memmove( id_owners, id_owners + 1, (id_owner_count - 1) * sizeof(*id_owners) );  /* the oldest goes */
        id_owner_count--;
    }
    if (id_owner_count == id_owner_capacity)
    {
        unsigned int capacity = id_owner_capacity ? id_owner_capacity * 2 : 64;
        struct session_id_owner *grown = realloc( id_owners, capacity * sizeof(*grown) );

        if (!grown) return 0;
        id_owners = grown;
        id_owner_capacity = capacity;
    }
    id_owners[id_owner_count].id = id;
    id_owners[id_owner_count].uid = uid;
    id_owner_count++;
    return 1;
}

static void load_id_owners(void)
{
    char line[64];
    unsigned int id, uid;
    FILE *file;
    int fd;

    id_owners_loaded = 1;
    if ((fd = openat( config_dir_fd, "session0.owners", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC )) == -1)
        return;
    if (!(file = fdopen( fd, "r" )))
    {
        close( fd );
        return;
    }
    while (fgets( line, sizeof(line), file ))
        if (sscanf( line, "%u %u", &id, &uid ) == 2 && id) add_id_owner( id, uid );
    fclose( file );
}

/* record a new id's owner before it is given out; 0 if it cannot be recorded (the id is then not given) */
static int record_id_owner( unsigned int id, uid_t uid )
{
    char line[64];
    int fd, len, ok;

    if (!id_owners_loaded) load_id_owners();
    len = snprintf( line, sizeof(line), "%u %u\n", id, (unsigned int)uid );
    if ((fd = openat( config_dir_fd, "session0.owners", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
                      0600 )) == -1)
    {
        fprintf( stderr, "session0: cannot record the owner of session %u: %s\n", id, strerror( errno ));
        return 0;
    }
    ok = write( fd, line, len ) == len && !fsync( fd );
    close( fd );
    return ok && add_id_owner( id, uid );
}

/* 1 when uid may have id back (it is recorded as that account's), 0 otherwise */
static int reclaim_session_id( unsigned int id, uid_t uid )
{
    unsigned int i;

    if (!id_owners_loaded) load_id_owners();
    for (i = 0; i < id_owner_count; i++)
        if (id_owners[i].id == id) return id_owners[i].uid == uid;
    return 0;
}

static void reserve_session_ids(void)
{
    static const char name[] = "session0.ids", tmp[] = "session0.ids.tmp";
    char buf[32];
    unsigned int reserved;
    ssize_t n;
    int fd, ok = 0;

    if (!next_session_id)
    {
        unsigned long value = 0;

        if ((fd = openat( config_dir_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC )) != -1)
        {
            if ((n = read( fd, buf, sizeof(buf) - 1 )) > 0)
            {
                buf[n] = 0;
                value = strtoul( buf, NULL, 10 );
            }
            close( fd );
        }
        next_session_id = reserved_session_id = (value && value < 0x80000000) ? value : 1;
    }
    if (next_session_id < reserved_session_id) return;
    reserved = next_session_id + SESSION_ID_BLOCK;
    n = snprintf( buf, sizeof(buf), "%u\n", reserved );
    if ((fd = openat( config_dir_fd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600 )) != -1)
    {
        ok = write( fd, buf, n ) == n && !fsync( fd );
        close( fd );
        ok = ok && !renameat( config_dir_fd, tmp, config_dir_fd, name );
        if (!ok) unlinkat( config_dir_fd, tmp, 0 );
    }
    if (!ok) fprintf( stderr, "session0: cannot keep the next session id in %s: %s\n", name, strerror( errno ));
    reserved_session_id = reserved;  /* without the file, ids are still unique for this run */
}

/* ---- R6 (docs/SESSION0_PROCESS_IDS.md): blocks of process and thread ids ---------------------------------------
 * Session 0 hands each linked server blocks of ids, so the ids of different servers never meet. Block 0 is session
 * 0's own. A block belongs to one server process (account, session, Unix pid and its start time) until that process
 * has ended: a server that links again after session 0 restarted gets its blocks back, and a block whose server is
 * gone is handed out again. The table is kept in session0.blocks next to session0.ids. */
#define ID_BLOCKS_PER_SERVER 64

static int connect_session0( int nonblock );
static ssize_t link_receive( int sock, void *buf, size_t size, int *fd, int *bad );
static int link_request( int sock, unsigned int type, unsigned int last, const void *desc, data_size_t desc_size,
                         const void *extra, data_size_t extra_size, int fd, struct wex_session0_reply *answer );

struct id_block
{
    uid_t              uid;
    unsigned int       session;
    pid_t              pid;     /* 0: free */
    unsigned long long start;   /* the process's start time (/proc/<pid>/stat), so a reused pid is not the owner */
    unsigned int       serial;  /* this lease, in this session 0 only (not saved): a new owner gets a new one */
};

static struct id_block id_blocks[WEX_ID_BLOCK_COUNT];
static int id_blocks_loaded;
static unsigned int id_block_serial;

/* the start time of a process, in clock ticks since boot; 0 when it does not exist */
static unsigned long long process_start_time( pid_t pid )
{
    char path[64], buf[1024], *p;
    unsigned long long start = 0;
    int fd, field;
    ssize_t n;

    snprintf( path, sizeof(path), "/proc/%d/stat", (int)pid );
    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return 0;
    n = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (n <= 0) return 0;
    buf[n] = 0;
    if (!(p = strrchr( buf, ')' ))) return 0;  /* the name may hold anything but ends at the last parenthesis */
    for (field = 2; field < 22 && p; field++) p = strchr( p + 1, ' ' );
    if (p) start = strtoull( p + 1, NULL, 10 );
    return start;
}

/* Is the block's server still running? Only a process that does not exist, or a process with another start time
 * (a reused pid), has ended: when /proc cannot be read for another reason the owner counts as alive. */
static int id_block_owner_alive( const struct id_block *b )
{
    char path[64];
    unsigned long long start;

    if (!b->pid) return 0;
    if ((start = process_start_time( b->pid ))) return start == b->start;
    snprintf( path, sizeof(path), "/proc/%d", (int)b->pid );
    return access( path, F_OK ) == -1 && errno == ENOENT ? 0 : 1;
}

static void load_id_blocks(void)
{
    char buf[256];
    FILE *f;
    int fd;

    id_blocks_loaded = 1;
    if ((fd = openat( config_dir_fd, "session0.blocks", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC )) == -1)
        return;
    if (!(f = fdopen( fd, "r" )))
    {
        close( fd );
        return;
    }
    while (fgets( buf, sizeof(buf), f ))
    {
        unsigned int block, uid, session;
        int pid;
        unsigned long long start;

        if (sscanf( buf, "%u %u %u %d %llu", &block, &uid, &session, &pid, &start ) != 5) continue;
        if (!block || block >= WEX_ID_BLOCK_COUNT || pid <= 0) continue;
        id_blocks[block].uid = uid;
        id_blocks[block].session = session;
        id_blocks[block].pid = pid;
        id_blocks[block].start = start;
        id_blocks[block].serial = ++id_block_serial;
    }
    fclose( f );
}

static int save_id_blocks(void)
{
    static const char name[] = "session0.blocks", tmp[] = "session0.blocks.tmp";
    unsigned int i;
    FILE *f;
    int fd, ok;

    if ((fd = openat( config_dir_fd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600 )) == -1)
        return 0;
    if (!(f = fdopen( fd, "w" )))
    {
        close( fd );
        return 0;
    }
    for (i = 1; i < WEX_ID_BLOCK_COUNT; i++)
        if (id_blocks[i].pid)
            fprintf( f, "%u %u %u %d %llu\n", i, (unsigned int)id_blocks[i].uid, id_blocks[i].session,
                     (int)id_blocks[i].pid, id_blocks[i].start );
    ok = !fflush( f ) && !fsync( fileno( f ) );
    if (fclose( f )) ok = 0;
    ok = ok && !renameat( config_dir_fd, tmp, config_dir_fd, name );
    if (!ok) unlinkat( config_dir_fd, tmp, 0 );
    return ok;
}

/* a block for the server process pid of uid and session: wanted again, or a new one; 0 when there is none */
static unsigned int grant_id_block( uid_t uid, unsigned int session, pid_t pid, unsigned int wanted )
{
    unsigned long long start = process_start_time( pid );
    unsigned int i, held = 0, block = 0;
    struct id_block *b, saved;

    if (!start || wanted >= WEX_ID_BLOCK_COUNT) return 0;
    if (!id_blocks_loaded) load_id_blocks();
    for (i = 1; i < WEX_ID_BLOCK_COUNT; i++)
        if (id_blocks[i].pid == pid && id_blocks[i].start == start) held++;
    if (wanted)
    {
        b = &id_blocks[wanted];
        if (b->pid == pid && b->start == start && b->uid == uid) return wanted;  /* this server's already */
        if (id_block_owner_alive( b )) return 0;  /* another live server's */
    }
    if (held >= ID_BLOCKS_PER_SERVER) return 0;
    if (wanted) block = wanted;
    else
    {
        for (i = 1; i < WEX_ID_BLOCK_COUNT && !block; i++)
            if (!id_block_owner_alive( &id_blocks[i] )) block = i;
        if (!block) return 0;
    }
    b = &id_blocks[block];
    saved = *b;
    b->uid = uid;
    b->session = session;
    b->pid = pid;
    b->start = start;
    b->serial = ++id_block_serial;
    /* a block is given only once it is on disk: after a restart session 0 knows every block it gave */
    if (!save_id_blocks())
    {
        fprintf( stderr, "session0: cannot keep the id blocks in session0.blocks: %s\n", strerror( errno ));
        *b = saved;
        return 0;
    }
    return block;
}

/* R6 step 2: is a process or thread id in a block that session 0 leased to the server at the other end of link (its
 * pid, account and start time, so not an earlier server whose pid was reused)? The lease's serial, or 0 */
static unsigned int id_of_peer( unsigned int id, const struct session0_socket *link )
{
    unsigned int block = id / 4 / WEX_ID_BLOCK_SIZE;
    const struct id_block *b;

    if (!id || !block || block >= WEX_ID_BLOCK_COUNT) return 0;
    if (!id_blocks_loaded) load_id_blocks();
    b = &id_blocks[block];
    if (b->pid != link->peer_pid || b->uid != link->peer_uid || b->start != process_start_time( link->peer_pid ))
        return 0;
    return b->serial;
}

/* session 0: is the lease (serial) that a bridged opener's id came from still the block's lease? Once the block went
 * to another server the id may name another process, so it is not reported any more */
int session0_id_lease_current( unsigned int id, unsigned int serial )
{
    unsigned int block = id / 4 / WEX_ID_BLOCK_SIZE;

    return serial && block && block < WEX_ID_BLOCK_COUNT && id_blocks[block].serial == serial;
}

/* A new block of ids for this server's own processes, or block again after session 0 restarted; 0 when there is
 * none. Session 0 grants its own blocks; a user's server asks session 0. */
unsigned int session0_new_id_block( unsigned int wanted )
{
    struct wex_session0_reply answer;
    int sock;

    if (session0_server) return grant_id_block( getuid(), 0, getpid(), wanted );
    /* non-blocking, as the pipe and channel requests: a session 0 that does not answer within link_request's 2 s
     * costs this request, never the server's loop. A grant whose answer is lost stays counted for this server in
     * session 0's table until the server ends (at most ID_BLOCKS_PER_SERVER) */
    if (!session0_client || (sock = connect_session0( 1 )) == -1) return 0;
    if (link_request( sock, WEX_SESSION0_BLOCK, wanted, NULL, 0, NULL, 0, -1, &answer ) ||
        answer.status || !answer.value || answer.value >= WEX_ID_BLOCK_COUNT || (wanted && answer.value != wanted))
        answer.value = 0;
    close( sock );
    return answer.value;
}

static void init_link( struct session0_socket *link )
{
    link->kind = LINK_REQUEST;
    link->trusted = 0;
    link->pending = 0;
    link->timeout = NULL;
    link->session_id = 0;
    link->serial = 0;
    link->outstanding = 0;
    link->peer_uid = (uid_t)-1;
    link->peer_pid = 0;
    list_init( &link->entry );
    link->x = NULL;
}

/* Links that wait for their message are few and short-lived: a user's server sends it right after connecting.
 * Peers that are not trusted are dropped at once, so a local user cannot hold session 0's descriptors. */
#define SESSION0_MAX_PENDING 64
#define SESSION0_PENDING_TIMEOUT (-5 * TICKS_PER_SEC)
static int pending_links;

static void session0_socket_dump( struct object *obj, int verbose );
static void session0_socket_destroy( struct object *obj );
static void session0_listen_event( struct fd *fd, int event );
static void session0_link_event( struct fd *fd, int event );

static const struct object_ops session0_socket_ops =
{
    .size    = sizeof(struct session0_socket),
    .type    = &no_type,
    .dump    = session0_socket_dump,
    .destroy = session0_socket_destroy,
};

static const struct fd_ops session0_listen_fd_ops = { .poll_event = session0_listen_event };
static const struct fd_ops session0_link_fd_ops = { .poll_event = session0_link_event };

static void session0_socket_dump( struct object *obj, int verbose )
{
    struct session0_socket *sock = (struct session0_socket *)obj;
    fprintf( stderr, "Session 0 socket fd=%p trusted=%d\n", sock->fd, sock->trusted );
}

static void session0_socket_destroy( struct object *obj )
{
    struct session0_socket *sock = (struct session0_socket *)obj;
    if (sock->timeout) remove_timeout_user( sock->timeout );
    if (sock->pending) pending_links--;
    if (sock->x) x_conn_closed( sock->x );  /* before its fd goes */
    /* session 0: a hive never outlives its agent's link, however the link ended */
    if (sock->kind == LINK_HIVE && session0_server) session0_unload_user_hive( sock, 0 );
    if (sock->kind == LINK_SESSION && session0_server)
    {
        list_remove( &sock->entry );
        fprintf( stderr, "session0: session %u (server pid %d) ended\n", sock->session_id, (int)sock->peer_pid );
    }
    if (sock->fd) release_object( sock->fd );
}

/* the link's message came (or it was dropped): it no longer counts as pending */
static void link_settled( struct session0_socket *link )
{
    if (link->timeout) remove_timeout_user( link->timeout );
    link->timeout = NULL;
    if (link->pending) pending_links--;
    link->pending = 0;
}

static void link_timeout( void *private )
{
    struct session0_socket *link = private;

    link->timeout = NULL;
    link_settled( link );
    release_object( link );  /* the reference the link held for its message */
}

/* T6.2, session 0: send an order to a session's server on its session link; 1 when sent */
static int session0_send_order( struct session0_socket *link, unsigned int order, unsigned int arg )
{
    struct wex_session0_order msg;

    msg.magic = WEX_SESSION0_MAGIC;
    msg.order = order;
    msg.serial = ++link->serial;
    msg.arg = arg;
    if (send( get_unix_fd( link->fd ), &msg, sizeof(msg), MSG_NOSIGNAL | MSG_DONTWAIT ) != sizeof(msg)) return 0;
    link->outstanding = msg.serial;  /* one order at a time; its answer clears it */
    return 1;
}

/* one "key=value" line of the root-owned configuration, given exactly once (sysrun_conf_get) */
static int read_conf( const char *key, char *value, size_t size )
{
    return sysrun_conf_get( key, value, size ) == SYSRUN_CONF_VALUE;
}

/* the socket path: the configuration, else (development) the environment */
static int socket_from_conf;  /* session 0 is named by the machine's configuration, not by the environment */

static int get_socket_path( int conf_only )
{
    const char *env;

    switch (sysrun_conf_get( "session0_socket", session0_path, sizeof(session0_path) ))
    {
    case SYSRUN_CONF_VALUE:
        socket_from_conf = session0_path[0] == '/';
        return socket_from_conf;
    case SYSRUN_CONF_INVALID:
        return 0;  /* a broken configuration never falls back to the environment */
    case SYSRUN_CONF_ABSENT:
        break;
    }
    if (conf_only || !(env = getenv( "WEX_SESSION0_SOCKET" )) || env[0] != '/' ||
        strlen( env ) >= sizeof(session0_path)) return 0;
    strcpy( session0_path, env );
    return 1;
}

/* Under systemd (wex-session0.service, Type=notify): tell it that the socket listens, so units ordered after session 0
 * start when it can serve them (they used to poll for the socket). The sd_notify protocol without libsystemd: one
 * datagram "READY=1" to $NOTIFY_SOCKET (a path, or "@" for the abstract namespace). Nothing to do without it. The
 * variable is dropped afterwards, so that no process started from here reports in session 0's name. */
static void notify_service_ready(void)
{
    const char *path = getenv( "NOTIFY_SOCKET" );
    struct sockaddr_un addr;
    size_t len;
    int fd;

    if (!path || (path[0] != '/' && path[0] != '@') || (len = strlen( path )) < 2 || len >= sizeof(addr.sun_path))
        return;
    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    memcpy( addr.sun_path, path, len );
    if (path[0] == '@') addr.sun_path[0] = 0;
    if ((fd = socket( AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0 )) != -1)
    {
        if (sendto( fd, "READY=1", 7, MSG_NOSIGNAL, (struct sockaddr *)&addr,
                    offsetof( struct sockaddr_un, sun_path ) + len ) != 7)
            fprintf( stderr, "wineserver: --session0: cannot tell systemd it is ready: %s\n", strerror( errno ));
        close( fd );
    }
    unsetenv( "NOTIFY_SOCKET" );
}

static void init_listener(void)
{
    struct sockaddr_un addr;
    struct session0_socket *listener;
    struct stat st;
    int fd;

    if (!get_socket_path( 0 )) fatal_error( "--session0: no session0_socket in %s\n", SESSION0_CONF );
    session0_dev = getenv( "WEX_SESSION0_DEV" ) && !strcmp( getenv( "WEX_SESSION0_DEV" ), "1" );
    if (!session0_dev)
    {
        char group[64];
        struct group *gr;

        if (!read_conf( "group", group, sizeof(group) ) || !(gr = getgrnam( group )))
            fatal_error( "--session0: no system group in %s\n", SESSION0_CONF );
        peer_group = gr->gr_gid;
    }

    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, session0_path );
    if (!lstat( session0_path, &st ))
    {
        if (!S_ISSOCK( st.st_mode )) fatal_error( "--session0: %s exists and is not a socket\n", session0_path );
        unlink( session0_path );
    }
    if ((fd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 )) == -1 ||
        bind( fd, (struct sockaddr *)&addr, sizeof(addr) ) == -1 ||
        chmod( session0_path, 0666 ) == -1 || listen( fd, 64 ) == -1)
        fatal_error( "--session0: cannot listen on %s: %s\n", session0_path, strerror( errno ));

    if (!(listener = alloc_object( &session0_socket_ops ))) fatal_error( "--session0: no memory\n" );
    init_link( listener );
    if (!(listener->fd = create_anonymous_fd( &session0_listen_fd_ops, fd, &listener->obj, 0 )))
        fatal_error( "--session0: no listener object\n" );
    set_fd_events( listener->fd, POLLIN );  /* the listener keeps its reference for the life of the server */
    master_socket_timeout = TIMEOUT_INFINITE;  /* session 0 runs until it is stopped */
    notify_service_ready();
}

/* T6.2, a user's server: who must serve session 0's socket. With the configuration, session 0's account and
 * (when configured) the system group; in development (no session0_user, a server the launcher did not start),
 * the server's own uid. */
static uid_t session0_peer_uid = (uid_t)-1;
static gid_t session0_peer_gid = (gid_t)-1;
static pid_t session0_peer_pid;  /* session 0's Unix pid, from the last link's SO_PEERCRED */

/* a link to session 0, after checking that the configured account serves the socket; -1 if not */
static int connect_session0( int nonblock )
{
    static int refused_logged;
    struct sockaddr_un addr;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int sock;

    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, session0_path );
    if ((sock = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0 )) == -1)
        return -1;
    if (connect( sock, (struct sockaddr *)&addr, sizeof(addr) ) == -1)
    {
        close( sock );
        return -1;
    }
    if (getsockopt( sock, SOL_SOCKET, SO_PEERCRED, &cred, &len ) || cred.uid != session0_peer_uid ||
        (session0_peer_gid != (gid_t)-1 && cred.gid != session0_peer_gid))
    {
        if (!refused_logged++)
            fprintf( stderr, "wineserver: %s is not served by session 0's account; not used\n", session0_path );
        close( sock );
        errno = EACCES;
        return -1;
    }
    session0_peer_pid = cred.pid;  /* X0: the epoch session 0 claims in its HELLO must be this process's */
    return sock;
}

/* T6.2, a user's server: the session link, on which session 0 sends orders */
static struct session0_socket *session_link;
static struct timeout_user *relink_timeout;
static unsigned int relink_delay;  /* seconds, doubled after each failed try up to a minute */

static int request_session_id( unsigned int wanted );
static void schedule_relink( unsigned int delay );

static void relink( void *private )
{
    relink_timeout = NULL;
    if (session_link) return;
    if (request_session_id( default_session_id ))
    {
        unsigned int i, block;

        fprintf( stderr, "wineserver: linked to session 0 again as session %u\n", default_session_id );
        relink_delay = 0;
        /* R6: the blocks of ids this server holds are its own again in session 0's table */
        for (i = 0; (block = ptid_block( i )); i++)
            if (session0_new_id_block( block ) != block)
                fprintf( stderr, "wineserver: session 0 did not give back the id block %u\n", block );
        return;
    }
    schedule_relink( relink_delay < 30 ? relink_delay * 2 : 60 );
}

static void schedule_relink( unsigned int delay )
{
    if (relink_timeout) return;
    relink_delay = delay;
    relink_timeout = add_timeout_user( -(timeout_t)delay * TICKS_PER_SEC, relink, NULL );
}

/* While the link is down, session 0's socket directory is watched: when session 0 (restarted) makes its socket again,
 * this server links within a second instead of at its next try, which backs off up to a minute (the machine registry is
 * missing until then). The directory stays when session 0 stops (RuntimeDirectoryPreserve). The socket is bound
 * before it listens, so the try is a second after the event, and a failed one falls back to the backoff. Linux only;
 * elsewhere the backoff alone. */
static struct fd *relink_watch;

static void relink_watch_event( struct fd *fd, int event )
{
#ifdef HAVE_SYS_INOTIFY_H
    union { struct inotify_event ev; char buf[sizeof(struct inotify_event) + NAME_MAX + 1]; } u;
    const char *name = strrchr( session0_path, '/' ) + 1;
    int created = 0;
    ssize_t n, i;

    while ((n = read( get_unix_fd( fd ), u.buf, sizeof(u.buf) )) > 0)
    {
        for (i = 0; i + (ssize_t)sizeof(struct inotify_event) <= n;
             i += sizeof(struct inotify_event) + ((struct inotify_event *)(u.buf + i))->len)
        {
            const struct inotify_event *ev = (const struct inotify_event *)(u.buf + i);
            if (ev->len && !strcmp( ev->name, name )) created = 1;
        }
    }
    if (!created || session_link) return;
    if (relink_timeout)
    {
        remove_timeout_user( relink_timeout );
        relink_timeout = NULL;
    }
    schedule_relink( 1 );
#endif
}

static const struct fd_ops relink_watch_fd_ops = { .poll_event = relink_watch_event };

static void watch_session0_directory(void)
{
#ifdef HAVE_SYS_INOTIFY_H
    char dir[sizeof(session0_path)], *slash;
    int unix_fd;

    if (relink_watch) return;
    strcpy( dir, session0_path );
    if (!(slash = strrchr( dir, '/' )) || slash == dir) return;
    *slash = 0;
    if ((unix_fd = inotify_init1( IN_NONBLOCK | IN_CLOEXEC )) == -1) return;
    if (inotify_add_watch( unix_fd, dir, IN_CREATE | IN_MOVED_TO ) == -1)
    {
        close( unix_fd );
        return;
    }
    /* kept for the life of the server: cheap, and the link may drop again */
    if ((relink_watch = create_anonymous_fd( &relink_watch_fd_ops, unix_fd, NULL, 0 )))
        set_fd_events( relink_watch, POLLIN );
#endif
}

static void session_link_event( struct fd *fd, int event );
static const struct fd_ops session_link_fd_ops = { .poll_event = session_link_event };

/* carry out one order of session 0; its status, and the value of the answer */
static unsigned int session_order( const struct wex_session0_order *order, unsigned int *value )
{
    switch (order->order)
    {
    case WEX_SESSION0_ORDER_PING:
        *value = default_session_id;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static void session_link_event( struct fd *fd, int event )
{
    static char buf[WEX_SESSION0_MAX_MSG];  /* an order, or an X frame (X0) */
    const struct wex_session0_order *order = (const struct wex_session0_order *)buf;
    struct wex_session0_order_reply answer;
    struct session0_socket *link = session_link;
    int desc_fd, bad;
    ssize_t n;

    if ((event & POLLOUT) && link && link->x) x_conn_writable( link->x );
    if (event & POLLIN)
    {
        n = link_receive( get_unix_fd( fd ), buf, sizeof(buf), &desc_fd, &bad );
        if (desc_fd != -1)
        {
            close( desc_fd );  /* nothing on this link carries a descriptor */
            bad = 1;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (n == sizeof(*order) && !bad && order->magic == WEX_SESSION0_MAGIC)
        {
            answer.magic = WEX_SESSION0_MAGIC;
            answer.serial = order->serial;
            answer.value = 0;
            answer.status = session_order( order, &answer.value );
            send( get_unix_fd( fd ), &answer, sizeof(answer), MSG_NOSIGNAL | MSG_DONTWAIT );
            return;
        }
        if (n >= (ssize_t)sizeof(unsigned int) && link && link->x && *(const unsigned int *)buf == WEX_X_MAGIC)
        {
            x_conn_frame( link->x, buf, n, bad );
            return;
        }
        if (n > 0) return;  /* neither: ignored */
    }
    else if (!(event & (POLLHUP | POLLERR))) return;
    /* session 0 is gone (restarted): link again, under this server's own session id */
    set_fd_events( fd, -1 );
    if ((link = session_link))
    {
        session_link = NULL;
        release_object( link );
    }
    fprintf( stderr, "wineserver: session 0 closed the session link; linking again\n" );
    watch_session0_directory();
    schedule_relink( 2 );
}

/* keep a link on which session 0 has answered SESSION_ID as the session link; 1 when kept */
static int open_session_link( int sock )
{
    struct session0_socket *link;

    if (fcntl( sock, F_SETFL, O_NONBLOCK ) == -1 || !(link = alloc_object( &session0_socket_ops ))) return 0;
    init_link( link );
    link->trusted = 1;
    link->kind = LINK_SESSION;
    if (!(link->fd = create_anonymous_fd( &session_link_fd_ops, sock, &link->obj, 0 )))
    {
        release_object( link );
        return 1;  /* the socket was closed with the fd object */
    }
    session_link = link;
    /* X0: the link carries X frames from now on; this server's HELLO goes out first */
    link->x = x_conn_create( link->fd, session0_peer_uid, session0_peer_pid, default_session_id );
    set_fd_events( link->fd, link->x ? x_conn_events( link->x ) : POLLIN );
    return 1;
}

/* T6.1: the session id of this server's logon from session 0 (wanted 0), or this server's own id again after
 * session 0 restarted; T6.2: the link stays as the session link. 1 when linked */
static int request_session_id( unsigned int wanted )
{
    struct wex_session0_request hdr = { WEX_SESSION0_MAGIC, WEX_SESSION0_VERSION, 0, WEX_SESSION0_SESSION_ID, wanted };
    struct wex_session0_reply answer;
    struct pollfd pfd;
    int sock;

    if ((sock = connect_session0( 0 )) == -1) return 0;
    pfd.fd = sock;
    pfd.events = POLLIN;
    /* a new server waits for session 0 as long as a logon may take (B26: session 0 is one thread, and writing a
     * user's policy or hive can keep it busy for seconds); linking again runs in the server's own loop, so it
     * waits briefly and tries again later */
    if (send( sock, &hdr, sizeof(hdr), MSG_NOSIGNAL ) == sizeof(hdr) &&
        poll( &pfd, 1, wanted ? 2000 : 30000 ) == 1 &&
        recv( sock, &answer, sizeof(answer), 0 ) == sizeof(answer) && answer.magic == WEX_SESSION0_MAGIC &&
        !answer.status && answer.value && (!wanted || answer.value == wanted))
    {
        default_session_id = answer.value;
        if (open_session_link( sock )) return session_link != NULL;
    }
    close( sock );
    return 0;
}

void session0_init( int server_mode )
{
    x_init();  /* X0: this server's epoch, before any link */
    if (server_mode)
    {
        session0_server = 1;
        default_session_id = 0;  /* T5.3: session 0's own programs (its services) are in session 0, as on Windows */
        init_listener();
        if (!ptid_add_block( 0 )) fatal_error( "session 0: no table for its block of ids\n" );  /* R6: block 0 */
#ifdef WEX_X0_LAB_TEST
        x0lab_init();
#endif
        return;
    }
    /* a launcher-started server uses the configuration only: the user controls the environment */
    if (get_socket_path( sysrun_active )) session0_client = 1;
    /* The machine's session 0 serves launcher-started servers only (peer_trusted). A server that the launcher did
     * not start (the setup's skeleton prefix, a prefix of the user's own, a program run with another WINEPREFIX)
     * is therefore a server of its own, with the files of its prefix, as it was before session 0 existed: as a
     * client it would be refused, and its programs' machine registry would fail. Development mode (the socket
     * named by the environment, no configuration) is unchanged. */
    if (session0_client && !sysrun_active && socket_from_conf) session0_client = 0;
    if (session0_client)
    {
        char name[64];
        struct passwd *pw;
        struct group *gr;

        if (read_conf( "session0_user", name, sizeof(name) ) && (pw = getpwnam( name )) && pw->pw_uid)
            hive_owner = session0_peer_uid = pw->pw_uid;
        if (read_conf( "group", name, sizeof(name) ) && (gr = getgrnam( name ))) session0_peer_gid = gr->gr_gid;
        if (session0_peer_uid == (uid_t)-1 && !sysrun_active)
            session0_peer_uid = getuid();  /* development: session 0 of the server's own account */
        else if (session0_peer_uid == (uid_t)-1 || (sysrun_active && session0_peer_gid == (gid_t)-1))
        {
            fprintf( stderr, "wineserver: %s names no session 0 account or system group; session 0 not used\n",
                     SESSION0_CONF );
            session0_client = 0;
        }
    }
    /* The launcher without session 0 is retired (WineExplorer R4, docs/SESSION0_SPLIT_REVIEW.md, decision 2): only
     * session 0 writes the machine hives, so a launcher-started server never runs as a machine registry of its own.
     * A configuration without session 0 is not an installed system's; such a server does not start. */
    if (sysrun_active && !session0_client)
        fatal_error( "the launcher's configuration %s names no session 0; a launcher-started server needs it\n",
                     SESSION0_CONF );
    /* a launcher-started server is a logon session only with an id from session 0: without one it does not start
     * (an id it made up could be another session's); a development server keeps 1 */
    if (session0_client)
    {
        unsigned int block;

        if (!request_session_id( 0 ))
        {
            if (sysrun_active) fatal_error( "session 0 at %s did not register this session\n", session0_path );
        }
        /* R6: the ids of this server's processes and threads come from blocks that session 0 leases to it, so they
         * never name another server's. Session 0 may be busy for seconds at a logon (B26): a few tries. A development
         * server without one keeps the stock numbering, and says so */
        else
        {
            unsigned int i;

            for (i = 0, block = 0; i < 15 && !block; i++) block = session0_new_id_block( 0 );
            if (!block || !ptid_add_block( block ))
            {
                if (sysrun_active) fatal_error( "session 0 at %s gave this session no block of ids\n", session0_path );
                fprintf( stderr, "wineserver: no block of ids from session 0: this server's ids may be another's\n" );
            }
        }
#ifdef WEX_X0_LAB_TEST
        x0lab_init();
#endif
    }
}

#ifdef WEX_X0_LAB_TEST
/* the lab (wex_x0lab.c): a user's server loses its session link as on a broken connection; the relink brings it
 * back with the same epoch */
void session0_lab_drop_link(void)
{
    if (session_link) shutdown( get_unix_fd( session_link->fd ), SHUT_RDWR );
}
#endif

uid_t session0_hive_owner(void)
{
    return hive_owner;
}

/* the peer is trusted (see the top of this file): a launcher-started wineserver of another user, whose real group
 * is not the system group (include/wine/wex_sysrun_peer.h, with the pidfd pin) */
static int peer_trusted( int client, const struct ucred *cred )
{
    struct wex_sysrun_peer_rule rule;

    if (session0_dev) return cred->uid == getuid();
    if (cred->uid == getuid()) return 0;
    rule.uid = cred->uid;
    rule.real_gid = (gid_t)-1;
    rule.sys_gid = peer_group;
    rule.server_name = 1;
    return wex_sysrun_peer_ok( client, &rule );
}

static void session0_listen_event( struct fd *fd, int event )
{
    struct wex_session0_reply reply = { WEX_SESSION0_MAGIC, STATUS_ACCESS_DENIED, 0 };
    struct session0_socket *link;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int client;

    if (!(event & POLLIN)) return;
    if ((client = accept4( get_unix_fd( fd ), NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC )) == -1) return;
    if (getsockopt( client, SOL_SOCKET, SO_PEERCRED, &cred, &len ) || !peer_trusted( client, &cred ) ||
        pending_links >= SESSION0_MAX_PENDING)
    {
        if (pending_links >= SESSION0_MAX_PENDING) reply.status = STATUS_INSUFFICIENT_RESOURCES;
        send( client, &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT );
        close( client );
        return;
    }
    if (!(link = alloc_object( &session0_socket_ops )))
    {
        close( client );
        return;
    }
    init_link( link );
    link->trusted = 1;
    link->pending = 1;
    link->peer_uid = cred.uid;
    link->peer_pid = cred.pid;
    pending_links++;
    if (!(link->fd = create_anonymous_fd( &session0_link_fd_ops, client, &link->obj, 0 )))
    {
        release_object( link );
        return;
    }
    link->timeout = add_timeout_user( SESSION0_PENDING_TIMEOUT, link_timeout, link );
    set_fd_events( link->fd, POLLIN );  /* the link holds its own reference until the message is handled */
}

/* A token that a session's server describes stands for that server's own user (the account map's SID of the
 * peer's uid) or for its session's system role (SYSTEM, LocalService, NetworkService). Another user's account is
 * refused: a session's server cannot act as another user in session 0 (T5 review). What the description says
 * beyond the user (groups, privileges, elevation) rests on the launcher-started server, as everything since T1. */
static int described_token_bound( struct token *token, uid_t uid )
{
    static const unsigned char nt_authority[6] = { 0, 0, 0, 0, 0, 5 };
    const struct sid *user = token_get_user_sid( token );
    unsigned int subauth[5], i;

    if (session0_dev) return 1;
    if (token_service_account( token ) != -1) return 1;
    if (!accounts_own_sid( uid, subauth )) return 0;
    if (user->sub_count != 5 || memcmp( user->id_auth, nt_authority, sizeof(nt_authority) )) return 0;
    for (i = 0; i < 5; i++) if (user->sub_auth[i] != subauth[i]) return 0;
    return 1;
}

/* the described token, if it is bound to the peer; else NULL with *status set */
static struct token *create_bound_token( const void *desc, data_size_t size, uid_t uid, unsigned int *status )
{
    struct token *token = token_create_described( desc, size );

    *status = STATUS_INVALID_PARAMETER;
    if (!token) return NULL;
    if (described_token_bound( token, uid )) return token;
    release_object( token );
    *status = STATUS_ACCESS_DENIED;
    return NULL;
}

/* X0 (docs/SESSION0_CROSS_SERVER.md, condition 1): the only way an X request turns a token description into a
 * token. Session 0 binds a user's server to that server's own user or its session's system role
 * (described_token_bound); a user's server believes session 0, its one peer (checked by connect_session0), for any
 * account. Either way the description's session must be the link's: the session of the user's server for session
 * 0, session 0 for a user's server (M3 serves callers in session 0 only, condition 4). NULL with *status set. */
struct token *link_bound_token( uid_t uid, unsigned int session, const void *desc, data_size_t size,
                                unsigned int *status )
{
    struct token *token = token_create_described( desc, size );

    *status = STATUS_INVALID_PARAMETER;
    if (!token) return NULL;
    if ((session0_server && !described_token_bound( token, uid )) || token_get_session_id( token ) != session)
    {
        release_object( token );
        *status = STATUS_ACCESS_DENIED;
        return NULL;
    }
    *status = STATUS_SUCCESS;
    return token;
}

/* serve a channel as a registry-only client with the described token */
static unsigned int start_registry_client( int channel, const void *desc, data_size_t size, unsigned int machine,
                                           uid_t uid )
{
    struct process *process;
    struct thread *thread;
    struct token *token;
    unsigned int status;

    if (!(token = create_bound_token( desc, size, uid, &status )))
    {
        close( channel );
        return status;
    }
    clear_error();
    process = create_process( channel, NULL, WEX_CREATE_REGISTRY_CLIENT, NULL, NULL, NULL, 0, token );
    release_object( token );
    if (!process) return get_error() ? get_error() : STATUS_NO_MEMORY;
    /* the program's machine decides its registry view: a 32-bit program sees the 32-bit HKLM\Software */
    if (machine <= 0xffff && is_machine_supported( machine )) process->machine = machine;
    if ((thread = create_thread( -1, process, NULL ))) add_process_thread( process, thread );
    status = thread ? STATUS_SUCCESS : (get_error() ? get_error() : STATUS_NO_MEMORY);
    release_object( process );
    return status;
}

/* ---- the exchange on a link (R2 of docs/SESSION0_SPLIT_REVIEW.md): one message, at most one descriptor ---- */

/* Send the vectors as one message, with fd (SCM_RIGHTS) unless it is -1; 1 when all of it went out. The
 * descriptor stays the caller's. */
static int link_send( int sock, struct iovec *vec, int count, int fd, int flags )
{
    char cmsg_buffer[CMSG_SPACE( sizeof(int) )];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    ssize_t total = 0;
    int i;

    memset( &msg, 0, sizeof(msg) );
    msg.msg_iov = vec;
    msg.msg_iovlen = count;
    for (i = 0; i < count; i++) total += vec[i].iov_len;
    if (fd != -1)
    {
        memset( cmsg_buffer, 0, sizeof(cmsg_buffer) );
        msg.msg_control = cmsg_buffer;
        msg.msg_controllen = sizeof(cmsg_buffer);
        cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_len = CMSG_LEN( sizeof(int) );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        memcpy( CMSG_DATA( cmsg ), &fd, sizeof(int) );
    }
    return sendmsg( sock, &msg, flags | MSG_NOSIGNAL ) == total;
}

/* Receive one message of at most size bytes. *fd: the first descriptor that came with it (the caller's to close,
 * also for an empty or rejected message), or -1; any other descriptor is closed here. *bad: the message or its
 * descriptors were cut, or more than one descriptor came. Returns what recvmsg returned. */
static ssize_t link_receive( int sock, void *buf, size_t size, int *fd, int *bad )
{
    char cmsg_buffer[CMSG_SPACE( 2 * sizeof(int) )];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec vec;
    ssize_t n;

    *fd = -1;
    *bad = 0;
    memset( &msg, 0, sizeof(msg) );
    vec.iov_base = buf;
    vec.iov_len = size;
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);
    if ((n = recvmsg( sock, &msg, MSG_CMSG_CLOEXEC )) < 0) return n;
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) *bad = 1;
    for (cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
    {
        int *fds = (int *)CMSG_DATA( cmsg ), count, i;

        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        count = (cmsg->cmsg_len - CMSG_LEN( 0 )) / sizeof(int);
        for (i = 0; i < count; i++)
        {
            if (*fd == -1) *fd = fds[i];
            else
            {
                close( fds[i] );
                *bad = 1;
            }
        }
    }
    return n;
}

/* A request of a user's server on its connected link to session 0: the header (type and its last field), the
 * description of a token, extra bytes and at most one descriptor; then session 0's answer, which comes at once
 * (2 seconds). 0 when answered, -1 when the request did not go out, -2 when no answer came. */
static int link_request( int sock, unsigned int type, unsigned int last, const void *desc, data_size_t desc_size,
                         const void *extra, data_size_t extra_size, int fd, struct wex_session0_reply *answer )
{
    struct wex_session0_request hdr;
    struct iovec vec[3];
    struct pollfd pfd;

    memset( &hdr, 0, sizeof(hdr) );
    hdr.magic = WEX_SESSION0_MAGIC;
    hdr.version = WEX_SESSION0_VERSION;
    hdr.desc_size = desc_size;
    hdr.type = type;
    hdr.machine = last;
    vec[0].iov_base = &hdr;
    vec[0].iov_len = sizeof(hdr);
    vec[1].iov_base = (void *)desc;
    vec[1].iov_len = desc_size;
    vec[2].iov_base = (void *)extra;
    vec[2].iov_len = extra_size;
    if (!link_send( sock, vec, extra_size ? 3 : 2, fd, 0 )) return -1;
    pfd.fd = sock;
    pfd.events = POLLIN;
    if (poll( &pfd, 1, 2000 ) != 1 || recv( sock, answer, sizeof(*answer), 0 ) != sizeof(*answer) ||
        answer->magic != WEX_SESSION0_MAGIC) return -2;
    return 0;
}

static void session0_link_event( struct fd *fd, int event )
{
    struct session0_socket *link = get_fd_user( fd );
    struct wex_session0_request *hdr;
    struct wex_session0_reply reply = { WEX_SESSION0_MAGIC, STATUS_ACCESS_DENIED, 0 };
    void *buf;
    int channel, bad;
    ssize_t n;

    /* X0: a session link also waits for room to send its queued X frames */
    if (link->kind == LINK_SESSION && link->x)
    {
        if (event & POLLOUT) x_conn_writable( link->x );
        if (!(event & (POLLIN | POLLHUP | POLLERR))) return;
        if (!(event & POLLIN))  /* the end, while not read (the lab's pause) */
        {
            release_object( link );
            return;
        }
    }
    if (!(event & POLLIN) || !(buf = malloc( WEX_SESSION0_MAX_MSG )))
    {
        release_object( link );
        return;
    }
    /* also for an empty message: descriptors can come with one, and are closed below like any other */
    n = link_receive( get_unix_fd( fd ), buf, WEX_SESSION0_MAX_MSG, &channel, &bad );
    link_settled( link );

    hdr = buf;
    if (link->kind == LINK_SESSION)
    {
        /* a session link: answers to orders, or the session's server is gone (end of file) */
        struct wex_session0_order_reply *answer = buf;

        if (channel != -1)
        {
            close( channel );
            bad = 1;
        }
        /* X0: an X frame */
        if (n >= (ssize_t)sizeof(unsigned int) && link->x && *(const unsigned int *)buf == WEX_X_MAGIC)
        {
            x_conn_frame( link->x, buf, n, bad );
            free( buf );
            return;
        }
        /* exactly one answer per order sent; anything else is ignored */
        if (n == sizeof(*answer) && answer->magic == WEX_SESSION0_MAGIC && link->outstanding &&
            answer->serial == link->outstanding)
        {
            link->outstanding = 0;
            if (!answer->status)  /* the only order so far is PING */
                fprintf( stderr, "session0: session %u (server pid %d, uid %u) answers on its link: session id %u\n",
                         link->session_id, (int)link->peer_pid, (unsigned int)link->peer_uid, answer->value );
        }
        free( buf );
        if (n > 0) return;  /* anything else is ignored */
        release_object( link );
        return;
    }
    if (link->kind == LINK_HIVE)
    {
        /* a hive link: HIVE_SAVED after each save, HIVE_UNLOAD at the server's exit, or the server is gone (end
         * of file) */
        int valid = n >= (ssize_t)sizeof(*hdr) && hdr->magic == WEX_SESSION0_MAGIC;
        int unload = valid && hdr->type == WEX_SESSION0_HIVE_UNLOAD;
        int saved = valid && hdr->type == WEX_SESSION0_HIVE_SAVED ? (hdr->saved == 1 ? 1 : 0) : -1;

        if (channel != -1) close( channel );
        free( buf );
        if (saved != -1) session0_user_hive_saved( link, saved );
        if (n > 0 && !unload) return;  /* nothing else is expected: ignore it */
        session0_unload_user_hive( link, unload );
        reply.status = STATUS_SUCCESS;
        if (unload) send( get_unix_fd( fd ), &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT );
        release_object( link );
        return;
    }
    if (n < (ssize_t)sizeof(*hdr) || bad ||
        hdr->magic != WEX_SESSION0_MAGIC || hdr->version != WEX_SESSION0_VERSION ||
        (!is_pipe_request( hdr->type ) && hdr->desc_size != n - sizeof(*hdr)) ||
        (is_pipe_request( hdr->type ) && (hdr->desc_size > n - sizeof(*hdr) ||
                                          hdr->name_size + (hdr->type == WEX_SESSION0_PIPE ? sizeof(unsigned int) : 0) !=
                                          n - sizeof(*hdr) - hdr->desc_size)) ||
        (hdr->type != WEX_SESSION0_CHANNEL && hdr->type != WEX_SESSION0_USER_HIVE &&
         hdr->type != WEX_SESSION0_SESSION_ID && hdr->type != WEX_SESSION0_BLOCK && !is_pipe_request( hdr->type )) ||
        ((hdr->type == WEX_SESSION0_CHANNEL || hdr->type == WEX_SESSION0_PIPE) && channel == -1) ||
        ((hdr->type == WEX_SESSION0_PIPE_WAIT || hdr->type == WEX_SESSION0_BLOCK) && channel != -1) ||
        (hdr->type == WEX_SESSION0_BLOCK && hdr->desc_size))
        reply.status = STATUS_INVALID_PARAMETER;
    else if (!link->trusted)
        reply.status = STATUS_ACCESS_DENIED;
    else if (hdr->type == WEX_SESSION0_SESSION_ID)
    {
        /* a new server asks with 0; a server that lost its link (session 0 restarted) asks for its own id again,
         * which it gets when the id was given out before (below the reserved number) and no link holds it */
        unsigned int wanted = hdr->session_id;
        struct session0_socket *other;
        unsigned int count = 0, in_use = 0;

        reserve_session_ids();
        LIST_FOR_EACH_ENTRY( other, &session_links, struct session0_socket, entry )
        {
            if (other->peer_uid == link->peer_uid) count++;
            if (wanted && other->session_id == wanted) in_use = 1;
        }
        if (count >= SESSION0_MAX_SESSIONS_PER_USER) reply.status = STATUS_INSUFFICIENT_RESOURCES;
        else if (wanted && (wanted >= next_session_id || in_use || !reclaim_session_id( wanted, link->peer_uid )))
            reply.status = STATUS_OBJECT_NAME_COLLISION;  /* R9: an id goes back only to its own account */
        else if (!wanted && !record_id_owner( next_session_id, link->peer_uid ))
            reply.status = STATUS_INSUFFICIENT_RESOURCES;
        else
        {
            reply.status = STATUS_SUCCESS;
            reply.value = wanted ? wanted : next_session_id++;
            /* T6.2: the link stays as the session link; a first order checks that the server takes orders */
            if (channel != -1) close( channel );
            free( buf );
            link->session_id = reply.value;
            link->kind = LINK_SESSION;
            list_add_tail( &session_links, &link->entry );
            if (wanted)
                fprintf( stderr, "session0: session %u (server pid %d, uid %u) linked again\n", wanted,
                         (int)link->peer_pid, (unsigned int)link->peer_uid );
            send( get_unix_fd( fd ), &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT );
            session0_send_order( link, WEX_SESSION0_ORDER_PING, 0 );
            /* X0: the session link carries X frames from now on; session 0's HELLO */
            if ((link->x = x_conn_create( link->fd, link->peer_uid, link->peer_pid, link->session_id )))
                set_fd_events( link->fd, x_conn_events( link->x ));
            return;
        }
    }
    else if (hdr->type == WEX_SESSION0_BLOCK)
    {
        /* R6: a block of ids for the linked server at the other end (its session link names its session) */
        int session = peer_session( link );

        if (session <= 0) reply.status = STATUS_ACCESS_DENIED;
        else if (!(reply.value = grant_id_block( link->peer_uid, session, link->peer_pid, hdr->block )))
            reply.status = STATUS_INSUFFICIENT_RESOURCES;
        else reply.status = STATUS_SUCCESS;
    }
    else if (hdr->type == WEX_SESSION0_PIPE_WAIT)
    {
        /* whether the pipe exists here, for a wait in a session (the token is not needed: a wait checks no access) */
        reply.status = pipe_bridge_query( (const WCHAR *)((const char *)(hdr + 1) + hdr->desc_size), hdr->name_size,
                                          link->peer_uid, peer_session( link ) );
    }
    else if (hdr->type == WEX_SESSION0_PIPE)
    {
        /* T5.2: join the channel (a stream socket) to the named pipe, as a client with the described token */
        struct token *token = create_bound_token( hdr + 1, hdr->desc_size, link->peer_uid, &reply.status );
        WCHAR *name = malloc( hdr->name_size ? hdr->name_size : 1 );

        if (!token || !name)
        {
            if (token) reply.status = STATUS_NO_MEMORY;
            else fprintf( stderr, "session0: pipe open refused for uid %u: the described token (%x)\n",
                          (unsigned int)link->peer_uid, reply.status );
            close( channel );
        }
        else
        {
            int session = peer_session( link );  /* R9: the session of the bridge's peer server */
            unsigned int client_pid, lease;

            memcpy( name, (const char *)(hdr + 1) + hdr->desc_size, hdr->name_size );
            /* R6 step 2: the opener's process id, after the name; kept only when it is in a block of the peer
             * server's (an id no other server has), else 0 as before; the lease goes with it */
            memcpy( &client_pid, (const char *)(hdr + 1) + hdr->desc_size + hdr->name_size, sizeof(client_pid) );
            if (!(lease = id_of_peer( client_pid, link ))) client_pid = 0;
            fcntl( channel, F_SETFL, O_NONBLOCK );
            reply.status = pipe_bridge_accept( token, name, hdr->name_size, channel, link->peer_uid, session,
                                               client_pid, lease );
        }
        channel = -1;  /* the bridge owns it now, or it was closed */
        free( name );
        if (token) release_object( token );
    }
    else if (hdr->type == WEX_SESSION0_USER_HIVE)
    {
        /* T4a: hold the user's hive (channel is the user.reg descriptor, or none) for this link's server */
        struct token *token = create_bound_token( hdr + 1, hdr->desc_size, link->peer_uid, &reply.status );

        if (token)  /* else reply.status says why */
        {
            reply.status = session0_load_user_hive( token_get_user_sid( token ), channel, link );
            channel = -1;
            release_object( token );
        }
        if (channel != -1) close( channel );
        free( buf );
        send( get_unix_fd( fd ), &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT );
        if (reply.status) release_object( link );
        else link->kind = LINK_HIVE;  /* the link stays: it is the hive's agent */
        return;
    }
    else
    {
        fcntl( channel, F_SETFL, O_NONBLOCK );
        reply.status = start_registry_client( channel, hdr + 1, hdr->desc_size, hdr->machine, link->peer_uid );
        channel = -1;  /* the client owns it now, or it was closed */
    }
    if (channel != -1) close( channel );
    free( buf );
    send( get_unix_fd( fd ), &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT );
    release_object( link );  /* one message per link */
}

/* T4a, session 0: send a user's hive (a memory file) to the server that holds the link */
int session0_send_hive( void *agent, int fd )
{
    struct session0_socket *link = agent;
    struct wex_session0_hive_save save = { WEX_SESSION0_MAGIC, WEX_SESSION0_HIVE_SAVE };
    struct iovec vec = { &save, sizeof(save) };

    return link_send( get_unix_fd( link->fd ), &vec, 1, fd, MSG_DONTWAIT );
}

int session0_request_allowed( enum request req )
{
    switch (req)
    {
    case REQ_init_first_thread:
    case REQ_create_key:
    case REQ_open_key:
    case REQ_delete_key:
    case REQ_flush_key:
    case REQ_enum_key:
    case REQ_set_key_value:
    case REQ_get_key_value:
    case REQ_enum_key_value:
    case REQ_delete_key_value:
    case REQ_rename_key:
    case REQ_set_registry_notification:  /* relay form only: a registry-only client has no events */
    case REQ_close_handle:
    case REQ_dup_handle:
    case REQ_get_security_object:
    case REQ_set_security_object:
    case REQ_get_object_info:
    case REQ_get_object_name:
    case REQ_get_object_type:
        return 1;
    default:
        return 0;
    }
}

/* Change notification on a session 0 key. The program registers an event with its own server
 * (relay_registry_notification) and passes the returned cookie to session 0 with set_registry_notification.
 * Session 0 writes the cookie to the channel's wait pipe when the key changes; the read end of that pipe is the
 * program's relay in its own server (set_registry_relay), which sets the event. When the channel is lost, the
 * relay sets every pending event: a spurious change report, never a lost one. */

#define RELAY_MAX_PENDING SESSION0_MAX_COOKIES

struct registry_relay
{
    struct object   obj;
    struct fd      *fd;
    struct process *process;      /* no reference: the process owns the relay */
    struct list     pending;
    unsigned int    pending_count;
    unsigned int    next_cookie;
};

struct relay_entry
{
    struct list   entry;
    unsigned int  cookie;
    struct event *event;
};

static void relay_dump( struct object *obj, int verbose );
static void relay_destroy( struct object *obj );
static void relay_poll_event( struct fd *fd, int event );

static const struct object_ops relay_ops =
{
    .size    = sizeof(struct registry_relay),
    .type    = &no_type,
    .dump    = relay_dump,
    .destroy = relay_destroy,
};

static const struct fd_ops relay_fd_ops = { .poll_event = relay_poll_event };

static void relay_dump( struct object *obj, int verbose )
{
    struct registry_relay *relay = (struct registry_relay *)obj;
    fprintf( stderr, "Registry relay pending=%u\n", relay->pending_count );
}

/* set the event of one cookie, or of all */
static void relay_fire( struct registry_relay *relay, unsigned int cookie, int all )
{
    struct relay_entry *entry, *next;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &relay->pending, struct relay_entry, entry )
    {
        if (!all && entry->cookie != cookie) continue;
        set_event( entry->event );
        release_object( entry->event );
        list_remove( &entry->entry );
        free( entry );
        relay->pending_count--;
    }
}

static void relay_destroy( struct object *obj )
{
    struct registry_relay *relay = (struct registry_relay *)obj;

    relay_fire( relay, 0, 1 );
    if (relay->fd) release_object( relay->fd );
}

static void relay_poll_event( struct fd *fd, int event )
{
    struct registry_relay *relay = get_fd_user( fd );
    unsigned int cookies[64], i;
    ssize_t n = -1;

    if (event & POLLIN)
    {
        while ((n = read( get_unix_fd( fd ), cookies, sizeof(cookies) )) > 0)
            for (i = 0; i < n / sizeof(cookies[0]); i++) relay_fire( relay, cookies[i], 0 );
    }
    if (!n || (event & (POLLHUP | POLLERR)))  /* the channel is gone */
    {
        relay_fire( relay, 0, 1 );
        set_fd_events( fd, -1 );
    }
}

void session0_relay_release( struct process *process )
{
    struct registry_relay *relay = (struct registry_relay *)process->wex_relay;

    if (!relay) return;
    process->wex_relay = NULL;
    relay->process = NULL;
    relay_fire( relay, 0, 1 );
    release_object( relay );
}

void session0_relay_notify( struct process *process, unsigned int cookie )
{
    struct thread *thread;
    int fd;

    if (!process || !process->wex_registry_client || list_empty( &process->thread_list )) return;
    thread = LIST_ENTRY( list_head( &process->thread_list ), struct thread, proc_entry );
    if (!thread->wait_fd || (fd = get_unix_fd( thread->wait_fd )) == -1) return;
    fcntl( fd, F_SETFL, O_NONBLOCK );  /* never wait for a program's server */
    if (write( fd, &cookie, sizeof(cookie) ) != sizeof(cookie)) return;
}

DECL_HANDLER(set_registry_relay)
{
    struct registry_relay *relay;
    int fd = thread_get_inflight_fd( current, req->fd );

    if (fd == -1)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }
    if (!session0_client || current->process->wex_registry_client)
    {
        close( fd );
        set_error( STATUS_NOT_SUPPORTED );
        return;
    }
    if (!(relay = alloc_object( &relay_ops )))
    {
        close( fd );
        return;
    }
    relay->fd = NULL;
    relay->process = current->process;
    list_init( &relay->pending );
    relay->pending_count = 0;
    relay->next_cookie = 1;
    fcntl( fd, F_SETFL, O_NONBLOCK );
    if (!(relay->fd = create_anonymous_fd( &relay_fd_ops, fd, &relay->obj, 0 )))
    {
        release_object( relay );
        return;
    }
    session0_relay_release( current->process );  /* a relay of an earlier channel */
    current->process->wex_relay = &relay->obj;
    set_fd_events( relay->fd, POLLIN );
}

DECL_HANDLER(relay_registry_notification)
{
    struct registry_relay *relay = (struct registry_relay *)current->process->wex_relay;
    struct relay_entry *entry;
    struct event *event;

    if (!relay)
    {
        set_error( STATUS_NOT_SUPPORTED );
        return;
    }
    if (relay->pending_count >= RELAY_MAX_PENDING)
    {
        set_error( STATUS_INSUFFICIENT_RESOURCES );
        return;
    }
    if (!(event = get_event_obj( current->process, req->event, SYNCHRONIZE ))) return;
    if (!(entry = mem_alloc( sizeof(*entry) )))
    {
        release_object( event );
        return;
    }
    entry->cookie = relay->next_cookie++;
    if (!relay->next_cookie) relay->next_cookie = 1;
    entry->event = event;
    reset_event( event );
    list_add_tail( &relay->pending, &entry->entry );
    relay->pending_count++;
    reply->cookie = entry->cookie;
}

/* T4a, a user's server: the link that holds this user's hive in session 0. Session 0 sends the hive back to be
 * written to user.reg, since its account cannot write in the user's home. */
static struct session0_socket *hive_link;
static int hive_kept_here;  /* user.reg could not be given to session 0 */

static void hive_link_event( struct fd *fd, int event );
static const struct fd_ops hive_link_fd_ops = { .poll_event = hive_link_event };

int session0_user_hive_remote(void)
{
    return hive_link != NULL;
}

/* write a hive that session 0 sent (a memory file) to user.reg, through a temporary file and a rename; 1 when
 * written */
static int write_user_hive( int src )
{
    static const char tmp[] = "user.reg.wex-session0";
    char buf[65536];
    ssize_t n = 0;
    int dst, ok = 1;

    if (lseek( src, 0, SEEK_SET )) return 0;
    if ((dst = openat( config_dir_fd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600 )) == -1)
    {
        fprintf( stderr, "wineserver: cannot write the user hive from session 0: %s\n", strerror( errno ));
        return 0;
    }
    while (ok && (n = read( src, buf, sizeof(buf) )) > 0)
        if (write( dst, buf, n ) != n) ok = 0;
    if (n < 0 || fsync( dst )) ok = 0;
    close( dst );
    if (ok && !renameat( config_dir_fd, tmp, config_dir_fd, "user.reg" )) return 1;
    fprintf( stderr, "wineserver: cannot write the user hive from session 0: %s\n", strerror( errno ));
    unlinkat( config_dir_fd, tmp, 0 );
    return 0;
}

/* one message on the hive link: a save, which is written (0), or the reply to HIVE_UNLOAD (1); -1 when gone */
static int hive_link_receive( int sock )
{
    struct wex_session0_reply reply;  /* a save has the same size and comes with a descriptor */
    int fd, bad;
    ssize_t n;

    if ((n = link_receive( sock, &reply, sizeof(reply), &fd, &bad )) < 0) return errno == EAGAIN ? 0 : -1;
    /* also for an empty message (the end): a descriptor that came with it is closed */
    if (!n)
    {
        if (fd != -1) close( fd );
        return -1;
    }
    if (n != sizeof(reply) || bad || reply.magic != WEX_SESSION0_MAGIC)
    {
        if (fd != -1) close( fd );
        return 0;
    }
    if (fd != -1)
    {
        if (reply.status == WEX_SESSION0_HIVE_SAVE)
        {
            /* the answer: session 0 sends the hive again when it was not written */
            struct wex_session0_request saved = { WEX_SESSION0_MAGIC, WEX_SESSION0_VERSION, 0,
                                                  WEX_SESSION0_HIVE_SAVED, write_user_hive( fd ) };
            send( sock, &saved, sizeof(saved), MSG_NOSIGNAL | MSG_DONTWAIT );
        }
        close( fd );
        return 0;
    }
    return 1;
}

static void hive_link_event( struct fd *fd, int event )
{
    if ((event & POLLIN) && hive_link_receive( get_unix_fd( fd ) ) >= 0 && !(event & (POLLHUP | POLLERR))) return;
    if (event & (POLLHUP | POLLERR) || (event & POLLIN))
    {
        /* session 0 is gone: it has dropped the hive; the next channel request loads it again */
        set_fd_events( fd, -1 );
        if (hive_link)
        {
            struct session0_socket *link = hive_link;
            hive_link = NULL;
            release_object( link );
        }
    }
}

/* a copy of user.reg for session 0 in a memory file sealed against changes: session 0 reads it without
 * blocking, and the user cannot change it while it is read. -1 with *ok set: there is no user.reg (a new hive);
 * -1 with *ok clear: user.reg cannot be sent (not a regular file, too large, unreadable) and stays here */
static int user_hive_snapshot( int *ok )
{
    char buf[65536];
    struct stat st;
    off_t copied = 0;
    ssize_t n;
    int src, dst = -1;

    *ok = 0;
    if ((src = openat( config_dir_fd, "user.reg", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC )) == -1)
    {
        *ok = errno == ENOENT;
        return -1;
    }
    if (fstat( src, &st ) || !S_ISREG( st.st_mode ) || st.st_size > SESSION0_MAX_USER_HIVE) goto failed;
    if (!st.st_size)
    {
        *ok = 1;  /* an empty file: a new hive */
        close( src );
        return -1;
    }
    if ((dst = memfd_create( "wex-user-hive", MFD_CLOEXEC | MFD_ALLOW_SEALING )) == -1) goto failed;
    while ((n = read( src, buf, sizeof(buf) )) > 0)
    {
        if (copied + n > SESSION0_MAX_USER_HIVE || write( dst, buf, n ) != n) goto failed;
        copied += n;
    }
    if (n < 0 || !copied ||
        fcntl( dst, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL )) goto failed;
    close( src );
    *ok = 1;
    return dst;

failed:
    fprintf( stderr, "wineserver: user.reg is not given to session 0: not a regular file, too large or unreadable\n" );
    if (dst != -1) close( dst );
    close( src );
    return -1;
}

/* hold this user's hive in session 0, for a program of the server's own user; 1 when held */
static int ensure_user_hive( struct token *token )
{
    struct wex_session0_reply answer;
    struct session0_socket *link;
    data_size_t size;
    void *desc = NULL;
    int sock = -1, hive = -1, ok = 0, readable;

    if (hive_link) return 1;
    if (hive_kept_here) return 0;
    if (!equal_sid( token_get_user_sid( token ), security_server_user_sid() )) return 0;  /* SYSTEM, services */
    size = token_describe( token, NULL, 0 );
    if (size > WEX_SESSION0_MAX_MSG - sizeof(struct wex_session0_request) || !(desc = malloc( size ))) return 0;
    token_describe( token, desc, size );
    hive = user_hive_snapshot( &readable );  /* none: a new hive */
    if (hive == -1 && !readable)
    {
        hive_kept_here = 1;  /* this server keeps the hive for its life */
        goto done;
    }

    if ((sock = connect_session0( 1 )) == -1) goto done;
    if (link_request( sock, WEX_SESSION0_USER_HIVE, 0, desc, size, NULL, 0, hive, &answer ) || answer.status)
        goto done;
    if (!(link = alloc_object( &session0_socket_ops ))) goto done;
    init_link( link );
    link->trusted = 1;
    link->kind = LINK_HIVE;
    if (!(link->fd = create_anonymous_fd( &hive_link_fd_ops, sock, &link->obj, 0 )))
    {
        sock = -1;  /* closed with the fd object */
        release_object( link );
        goto done;
    }
    sock = -1;
    hive_link = link;
    set_fd_events( link->fd, POLLIN );
    ok = 1;

done:
    if (sock != -1) close( sock );
    if (hive != -1) close( hive );
    free( desc );
    return ok;
}

/* at the server's exit: session 0 sends the last changes and drops the hive */
void session0_client_exit(void)
{
    struct wex_session0_request hdr = { WEX_SESSION0_MAGIC, WEX_SESSION0_VERSION, 0, WEX_SESSION0_HIVE_UNLOAD };
    struct session0_socket *link = hive_link;
    struct pollfd pfd;
    int i, sock;

    /* the session link ends with the server: released here, or the leak check at exit prints the object on the
     * terminal of whoever started the server (found on an installed machine, 2026-11-13) */
    if (session_link)
    {
        struct session0_socket *session = session_link;
        session_link = NULL;
        release_object( session );
    }
    if (!link) return;
    hive_link = NULL;
    sock = get_unix_fd( link->fd );
    if (send( sock, &hdr, sizeof(hdr), MSG_NOSIGNAL ) == sizeof(hdr))
    {
        pfd.fd = sock;
        pfd.events = POLLIN;
        for (i = 0; i < 8 && poll( &pfd, 1, 2000 ) == 1; i++) if (hive_link_receive( sock )) break;
    }
    release_object( link );
}

/* T5.2, a user's server: a pipe request to session 0 (TYPE: PIPE with SOCK, one end of a stream socket that
 * session 0 joins to the pipe, or PIPE_WAIT without), for the token; session 0's status */
static unsigned int pipe_request( unsigned int type, struct token *token, const WCHAR *name, data_size_t len,
                                  int sock, unsigned int client_pid )
{
    struct wex_session0_reply answer;
    data_size_t size, extra_size = len;
    unsigned int status;
    char *extra = NULL;
    void *desc;
    int link;

    if (!session0_client) return STATUS_OBJECT_NAME_NOT_FOUND;
    size = token_describe( token, NULL, 0 );
    if (size + len + sizeof(client_pid) > WEX_SESSION0_MAX_MSG - sizeof(struct wex_session0_request))
        return STATUS_NAME_TOO_LONG;
    if (!(desc = mem_alloc( size ))) return STATUS_NO_MEMORY;
    token_describe( token, desc, size );
    /* PIPE: the name, then the opener's process id (R6 step 2) */
    if (type == WEX_SESSION0_PIPE)
    {
        extra_size = len + sizeof(client_pid);
        if (!(extra = mem_alloc( extra_size )))
        {
            free( desc );
            return STATUS_NO_MEMORY;
        }
        memcpy( extra, name, len );
        memcpy( extra + len, &client_pid, sizeof(client_pid) );
    }
    if ((link = connect_session0( 1 )) == -1)
    {
        free( desc );
        free( extra );
        return STATUS_PIPE_NOT_AVAILABLE;
    }
    /* the last field is the name's size (name_size) */
    if (link_request( link, type, len, desc, size, extra ? (const void *)extra : (const void *)name, extra_size, sock,
                      &answer ))
        status = STATUS_PIPE_NOT_AVAILABLE;
    else
        status = answer.status;
    close( link );
    free( desc );
    free( extra );
    return status;
}

/* a user's server: whether session 0 has the pipe, for a wait (FSCTL_PIPE_WAIT) of a program in the session */
unsigned int session0_wait_pipe( struct token *token, const WCHAR *name, data_size_t len )
{
    return pipe_request( WEX_SESSION0_PIPE_WAIT, token, name, len, -1, 0 );
}

/* T5.2, a user's server: ask session 0 to join one end of a stream socket to its named pipe, for the token; the
 * other end, or -1 with the error set */
int session0_open_pipe( struct token *token, const WCHAR *name, data_size_t len )
{
    unsigned int status;
    int sv[2];

    if (socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv ) == -1)
    {
        file_set_error();
        return -1;
    }
    status = pipe_request( WEX_SESSION0_PIPE, token, name, len, sv[0], current ? current->process->id : 0 );
    close( sv[0] );
    if (status)
    {
        set_error( status );
        close( sv[1] );
        return -1;
    }
    fcntl( sv[1], F_SETFL, O_NONBLOCK );
    return sv[1];
}

/* a registry channel to session 0 for the calling program, with its primary token */
DECL_HANDLER(get_registry_channel)
{
    struct wex_session0_reply answer;
    data_size_t size;
    void *desc;
    int sv[2], link;

    if (!session0_client || current->process->wex_registry_client)
    {
        set_error( STATUS_NOT_SUPPORTED );
        return;
    }
    /* T4a: the user's hive goes to session 0 with the first program of the server's user */
    reply->user_hive = ensure_user_hive( current->process->token );
    size = token_describe( current->process->token, NULL, 0 );
    if (size > WEX_SESSION0_MAX_MSG - sizeof(struct wex_session0_request) || !(desc = mem_alloc( size ))) return;
    token_describe( current->process->token, desc, size );

    if (socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv ) == -1)
    {
        free( desc );
        file_set_error();
        return;
    }
    if ((link = connect_session0( 1 )) == -1)
    {
        set_error( STATUS_PIPE_NOT_AVAILABLE );
        goto done;
    }
    /* session 0 answers at once; a session 0 that does not is treated as absent */
    if (link_request( link, WEX_SESSION0_CHANNEL, current->process->machine, desc, size, NULL, 0, sv[0], &answer ))
    {
        set_error( STATUS_PIPE_DISCONNECTED );
        goto done;
    }
    if (answer.status)
    {
        set_error( answer.status );
        goto done;
    }
    reply->cookie = 0x5330;
    if (send_client_fd( current->process, sv[1], reply->cookie ) == -1) set_error( STATUS_PIPE_DISCONNECTED );

done:
    if (link != -1) close( link );
    close( sv[0] );
    close( sv[1] );
    free( desc );
}
