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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

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
#include "session0.h"
#include "wine/wex_session0.h"

#define SESSION0_CONF "/etc/wineexplorer/sysrun.conf"

int session0_server;
int session0_client;

static char session0_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int session0_dev;                /* development trust: a peer of session 0's own uid */
static gid_t peer_group;                /* production trust: the launcher's system group */
static uid_t hive_owner = (uid_t)-1;    /* the session 0 account, when it is configured */

struct session0_socket
{
    struct object obj;
    struct fd    *fd;
    int           trusted;              /* links: the peer may describe tokens */
    int           hive;                 /* T4a: the link is the agent of a user's hive */
};

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
    if (sock->fd) release_object( sock->fd );
}

/* one "key=value" line of the root-owned configuration; 0 if the file or the key is missing */
static int read_conf( const char *key, char *value, size_t size )
{
    char buf[4096], *line, *next;
    size_t keylen = strlen( key );
    struct stat st;
    ssize_t n = 0;
    int fd;

    if ((fd = open( SESSION0_CONF, O_RDONLY | O_NOFOLLOW | O_CLOEXEC )) == -1) return 0;
    if (!fstat( fd, &st ) && S_ISREG( st.st_mode ) && !st.st_uid && !(st.st_mode & (S_IWGRP | S_IWOTH)))
        n = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (n <= 0) return 0;
    buf[n] = 0;
    for (line = buf; line; line = next)
    {
        if ((next = strchr( line, '\n' ))) *next++ = 0;
        if (strncmp( line, key, keylen ) || line[keylen] != '=') continue;
        if (strlen( line + keylen + 1 ) >= size) return 0;
        strcpy( value, line + keylen + 1 );
        return value[0] != 0;
    }
    return 0;
}

/* the socket path: the configuration, else (development) the environment */
static int get_socket_path( int conf_only )
{
    const char *env;

    if (read_conf( "session0_socket", session0_path, sizeof(session0_path) )) return session0_path[0] == '/';
    if (conf_only || !(env = getenv( "WEX_SESSION0_SOCKET" )) || env[0] != '/' ||
        strlen( env ) >= sizeof(session0_path)) return 0;
    strcpy( session0_path, env );
    return 1;
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
    listener->trusted = 0;
    listener->hive = 0;
    if (!(listener->fd = create_anonymous_fd( &session0_listen_fd_ops, fd, &listener->obj, 0 )))
        fatal_error( "--session0: no listener object\n" );
    set_fd_events( listener->fd, POLLIN );  /* the listener keeps its reference for the life of the server */
    master_socket_timeout = TIMEOUT_INFINITE;  /* session 0 runs until it is stopped */
}

void session0_init( int server_mode )
{
    if (server_mode)
    {
        session0_server = 1;
        init_listener();
        return;
    }
    /* a launcher-started server uses the configuration only: the user controls the environment */
    if (get_socket_path( sysrun_active )) session0_client = 1;
    if (session0_client)
    {
        char user[64];
        struct passwd *pw;

        if (read_conf( "session0_user", user, sizeof(user) ) && (pw = getpwnam( user )) && pw->pw_uid)
            hive_owner = pw->pw_uid;
    }
}

uid_t session0_hive_owner(void)
{
    return hive_owner;
}

/* A launcher-started server of the peer's uid. Only the setgid launcher gives a process of a user who is not in
 * the system group that group as its saved group, and it starts only the configured wineserver and the machine
 * root's wineboot; such a process is not dumpable, so the user cannot take it over. Its executable cannot be
 * checked from here: /proc/<pid>/exe of a non-dumpable process of another uid is not readable. */
static int peer_is_launched_server( pid_t pid, uid_t uid )
{
    char path[64], buf[2048], *line;
    unsigned int r, e, s, f;
    int fd, uid_ok = 0, gid_ok = 0;
    ssize_t n;

    snprintf( path, sizeof(path), "/proc/%d/status", (int)pid );
    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return 0;
    n = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (n <= 0) return 0;
    buf[n] = 0;
    if ((line = strstr( buf, "\nUid:" )) && sscanf( line + 5, "%u %u %u %u", &r, &e, &s, &f ) == 4)
        uid_ok = r == uid && e == uid && s == uid && f == uid;
    if ((line = strstr( buf, "\nGid:" )) && sscanf( line + 5, "%u %u %u %u", &r, &e, &s, &f ) == 4)
        gid_ok = s == peer_group && r != peer_group && (e == r || e == peer_group);
    return uid_ok && gid_ok && !strncmp( buf, "Name:\twineserver\n", 17 );
}

static void session0_listen_event( struct fd *fd, int event )
{
    struct session0_socket *link;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int client;

    if (!(event & POLLIN)) return;
    if ((client = accept4( get_unix_fd( fd ), NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC )) == -1) return;
    if (getsockopt( client, SOL_SOCKET, SO_PEERCRED, &cred, &len ) || !(link = alloc_object( &session0_socket_ops )))
    {
        close( client );
        return;
    }
    link->hive = 0;
    if (session0_dev) link->trusted = cred.uid == getuid();
    else link->trusted = cred.uid != getuid() && peer_is_launched_server( cred.pid, cred.uid );
    if (!(link->fd = create_anonymous_fd( &session0_link_fd_ops, client, &link->obj, 0 )))
    {
        release_object( link );
        return;
    }
    set_fd_events( link->fd, POLLIN );  /* the link holds its own reference until the message is handled */
}

/* serve a channel as a registry-only client with the described token */
static unsigned int start_registry_client( int channel, const void *desc, data_size_t size )
{
    struct process *process;
    struct thread *thread;
    struct token *token;
    unsigned int status;

    if (!(token = token_create_described( desc, size )))
    {
        close( channel );
        return STATUS_INVALID_PARAMETER;
    }
    clear_error();
    process = create_process( channel, NULL, WEX_CREATE_REGISTRY_CLIENT, NULL, NULL, NULL, 0, token );
    release_object( token );
    if (!process) return get_error() ? get_error() : STATUS_NO_MEMORY;
    if ((thread = create_thread( -1, process, NULL ))) add_process_thread( process, thread );
    status = thread ? STATUS_SUCCESS : (get_error() ? get_error() : STATUS_NO_MEMORY);
    release_object( process );
    return status;
}

static void session0_link_event( struct fd *fd, int event )
{
    struct session0_socket *link = get_fd_user( fd );
    struct wex_session0_request *hdr;
    struct wex_session0_reply reply = { WEX_SESSION0_MAGIC, STATUS_ACCESS_DENIED };
    char cmsg_buffer[CMSG_SPACE( 2 * sizeof(int) )];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec vec;
    void *buf;
    int channel = -1, extra = -1;
    ssize_t n;

    if (!(event & POLLIN) || !(buf = malloc( WEX_SESSION0_MAX_MSG )))
    {
        release_object( link );
        return;
    }
    memset( &msg, 0, sizeof(msg) );
    vec.iov_base = buf;
    vec.iov_len = WEX_SESSION0_MAX_MSG;
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);
    n = recvmsg( get_unix_fd( fd ), &msg, MSG_CMSG_CLOEXEC );
    for (cmsg = n > 0 ? CMSG_FIRSTHDR( &msg ) : NULL; cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
    {
        int *fds = (int *)CMSG_DATA( cmsg ), count, i;

        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        count = (cmsg->cmsg_len - CMSG_LEN( 0 )) / sizeof(int);
        for (i = 0; i < count; i++)
        {
            if (channel == -1) channel = fds[i];
            else { if (extra != -1) close( extra ); extra = fds[i]; }
        }
    }
    if (extra != -1) close( extra );

    hdr = buf;
    if (link->hive)
    {
        /* a hive link: HIVE_UNLOAD at the server's exit, or the server is gone (end of file) */
        int unload = n >= (ssize_t)sizeof(*hdr) && hdr->magic == WEX_SESSION0_MAGIC &&
                     hdr->type == WEX_SESSION0_HIVE_UNLOAD;
        if (channel != -1) close( channel );
        free( buf );
        if (n > 0 && !unload) return;  /* nothing else is expected: ignore it */
        session0_unload_user_hive( link, unload );
        reply.status = STATUS_SUCCESS;
        if (unload) send( get_unix_fd( fd ), &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT );
        release_object( link );
        return;
    }
    if (n < (ssize_t)sizeof(*hdr) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || extra != -1 ||
        hdr->magic != WEX_SESSION0_MAGIC || hdr->version != WEX_SESSION0_VERSION ||
        hdr->desc_size != n - sizeof(*hdr) ||
        (hdr->type != WEX_SESSION0_CHANNEL && hdr->type != WEX_SESSION0_USER_HIVE) ||
        (hdr->type == WEX_SESSION0_CHANNEL && channel == -1))
        reply.status = STATUS_INVALID_PARAMETER;
    else if (!link->trusted)
        reply.status = STATUS_ACCESS_DENIED;
    else if (hdr->type == WEX_SESSION0_USER_HIVE)
    {
        /* T4a: hold the user's hive (channel is the user.reg descriptor, or none) for this link's server */
        struct token *token = token_create_described( hdr + 1, hdr->desc_size );

        if (!token) reply.status = STATUS_INVALID_PARAMETER;
        else
        {
            reply.status = session0_load_user_hive( token_get_user_sid( token ), channel, link );
            channel = -1;
            release_object( token );
        }
        if (channel != -1) close( channel );
        free( buf );
        send( get_unix_fd( fd ), &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT );
        if (reply.status) release_object( link );
        else link->hive = 1;  /* the link stays: it is the hive's agent */
        return;
    }
    else
    {
        fcntl( channel, F_SETFL, O_NONBLOCK );
        reply.status = start_registry_client( channel, hdr + 1, hdr->desc_size );
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
    char cmsg_buffer[CMSG_SPACE( sizeof(int) )];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec vec;

    memset( &msg, 0, sizeof(msg) );
    vec.iov_base = &save;
    vec.iov_len = sizeof(save);
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);
    cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_len = CMSG_LEN( sizeof(int) );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy( CMSG_DATA( cmsg ), &fd, sizeof(int) );
    return sendmsg( get_unix_fd( link->fd ), &msg, MSG_NOSIGNAL | MSG_DONTWAIT ) == sizeof(save);
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

#define RELAY_MAX_PENDING 4096

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

static void hive_link_event( struct fd *fd, int event );
static const struct fd_ops hive_link_fd_ops = { .poll_event = hive_link_event };

int session0_user_hive_remote(void)
{
    return hive_link != NULL;
}

/* write a hive that session 0 sent (a memory file) to user.reg, through a temporary file and a rename */
static void write_user_hive( int src )
{
    static const char tmp[] = "user.reg.wex-session0";
    char buf[65536];
    ssize_t n = 0;
    int dst, ok = 1;

    if (lseek( src, 0, SEEK_SET )) return;
    if ((dst = openat( config_dir_fd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600 )) == -1)
        return;
    while (ok && (n = read( src, buf, sizeof(buf) )) > 0)
        if (write( dst, buf, n ) != n) ok = 0;
    if (n < 0 || fsync( dst )) ok = 0;
    close( dst );
    if (ok && !renameat( config_dir_fd, tmp, config_dir_fd, "user.reg" )) return;
    unlinkat( config_dir_fd, tmp, 0 );
}

/* one message on the hive link: a save, which is written (0), or the reply to HIVE_UNLOAD (1); -1 when gone */
static int hive_link_receive( int sock )
{
    struct wex_session0_reply reply;  /* a save has the same size and comes with a descriptor */
    char cmsg_buffer[CMSG_SPACE( sizeof(int) )];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec vec;
    int fd = -1;
    ssize_t n;

    memset( &msg, 0, sizeof(msg) );
    vec.iov_base = &reply;
    vec.iov_len = sizeof(reply);
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);
    if ((n = recvmsg( sock, &msg, MSG_CMSG_CLOEXEC )) <= 0) return (n < 0 && errno == EAGAIN) ? 0 : -1;
    for (cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) memcpy( &fd, CMSG_DATA( cmsg ), sizeof(fd) );
    if (n != sizeof(reply) || reply.magic != WEX_SESSION0_MAGIC)
    {
        if (fd != -1) close( fd );
        return 0;
    }
    if (fd != -1)
    {
        if (reply.status == WEX_SESSION0_HIVE_SAVE) write_user_hive( fd );
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

/* hold this user's hive in session 0, for a program of the server's own user; 1 when held */
static int ensure_user_hive( struct token *token )
{
    struct wex_session0_request hdr;
    struct wex_session0_reply answer;
    struct sockaddr_un addr;
    char cmsg_buffer[CMSG_SPACE( sizeof(int) )];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec vec[2];
    struct pollfd pfd;
    struct session0_socket *link;
    data_size_t size;
    void *desc = NULL;
    int sock = -1, hive = -1, ok = 0;

    if (hive_link) return 1;
    if (!equal_sid( token_get_user_sid( token ), security_server_user_sid() )) return 0;  /* SYSTEM, services */
    size = token_describe( token, NULL, 0 );
    if (size > WEX_SESSION0_MAX_MSG - sizeof(hdr) || !(desc = malloc( size ))) return 0;
    token_describe( token, desc, size );
    hive = openat( config_dir_fd, "user.reg", O_RDONLY | O_NOFOLLOW | O_CLOEXEC );  /* none: a new hive */

    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, session0_path );
    if ((sock = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 )) == -1 ||
        connect( sock, (struct sockaddr *)&addr, sizeof(addr) ) == -1) goto done;
    hdr.magic = WEX_SESSION0_MAGIC;
    hdr.version = WEX_SESSION0_VERSION;
    hdr.desc_size = size;
    hdr.type = WEX_SESSION0_USER_HIVE;
    vec[0].iov_base = &hdr;
    vec[0].iov_len = sizeof(hdr);
    vec[1].iov_base = desc;
    vec[1].iov_len = size;
    memset( &msg, 0, sizeof(msg) );
    msg.msg_iov = vec;
    msg.msg_iovlen = 2;
    if (hive != -1)
    {
        msg.msg_control = cmsg_buffer;
        msg.msg_controllen = sizeof(cmsg_buffer);
        cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_len = CMSG_LEN( sizeof(int) );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        memcpy( CMSG_DATA( cmsg ), &hive, sizeof(int) );
    }
    if (sendmsg( sock, &msg, MSG_NOSIGNAL ) != (ssize_t)(sizeof(hdr) + size)) goto done;
    pfd.fd = sock;
    pfd.events = POLLIN;
    if (poll( &pfd, 1, 2000 ) != 1 || recv( sock, &answer, sizeof(answer), 0 ) != sizeof(answer) ||
        answer.magic != WEX_SESSION0_MAGIC || answer.status) goto done;
    if (!(link = alloc_object( &session0_socket_ops ))) goto done;
    link->trusted = 1;
    link->hive = 1;
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

/* a registry channel to session 0 for the calling program, with its primary token */
DECL_HANDLER(get_registry_channel)
{
    struct wex_session0_request hdr;
    struct wex_session0_reply answer;
    struct sockaddr_un addr;
    char cmsg_buffer[CMSG_SPACE( sizeof(int) )];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    struct iovec vec[2];
    struct pollfd pfd;
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
    if (size > WEX_SESSION0_MAX_MSG - sizeof(hdr) || !(desc = mem_alloc( size ))) return;
    token_describe( current->process->token, desc, size );

    if (socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv ) == -1)
    {
        free( desc );
        file_set_error();
        return;
    }
    memset( &addr, 0, sizeof(addr) );
    addr.sun_family = AF_UNIX;
    strcpy( addr.sun_path, session0_path );
    if ((link = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 )) == -1 ||
        connect( link, (struct sockaddr *)&addr, sizeof(addr) ) == -1)
    {
        set_error( STATUS_PIPE_NOT_AVAILABLE );
        goto done;
    }
    hdr.magic = WEX_SESSION0_MAGIC;
    hdr.version = WEX_SESSION0_VERSION;
    hdr.desc_size = size;
    hdr.type = WEX_SESSION0_CHANNEL;
    vec[0].iov_base = &hdr;
    vec[0].iov_len = sizeof(hdr);
    vec[1].iov_base = desc;
    vec[1].iov_len = size;
    memset( &msg, 0, sizeof(msg) );
    msg.msg_iov = vec;
    msg.msg_iovlen = 2;
    msg.msg_control = cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);
    cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_len = CMSG_LEN( sizeof(int) );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy( CMSG_DATA( cmsg ), &sv[0], sizeof(int) );
    if (sendmsg( link, &msg, MSG_NOSIGNAL ) != (ssize_t)(sizeof(hdr) + size))
    {
        set_error( STATUS_PIPE_DISCONNECTED );
        goto done;
    }
    /* session 0 answers at once; a session 0 that does not is treated as absent */
    pfd.fd = link;
    pfd.events = POLLIN;
    if (poll( &pfd, 1, 2000 ) != 1 || recv( link, &answer, sizeof(answer), 0 ) != sizeof(answer) ||
        answer.magic != WEX_SESSION0_MAGIC)
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
