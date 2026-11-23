/*
 * WineExplorer E1 lab: the R8 binding of a pipe's server end (never in a shipped build)
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
 * get_pipe_caller_binding's E1R8 operations (docs/E1_S18E_R8_SPEC.md), moved out of named_pipe.c by R5 of
 * docs/SESSION0_SPLIT_REVIEW.md with no change of behaviour. The E1R8 frames (the tuple the broker sends, the
 * ENDORSED message to the verifier and the verifier's RECEIPT) go through the one C codec, e1r8_codec.h, which
 * wexelevate and consent use too. named_pipe.c keeps the binding pointer of a server end and calls
 * e1lab_pipe_e1r8_op; wex_e1lab.h lists the hooks.
 */

#include "config.h"

#if defined(WEX_E1S6_TRANSPORT_TEST)

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "thread.h"
#include "request.h"
#include "security.h"
#include "process.h"
#include "sysrun.h"
#include "user.h"
#include "accounts.h"
#include "session0.h"
#include "e1s_token.h"
#include "wex_e1lab.h"
#include "e1r8_codec.h"

struct pipe_binding
{
    unsigned char prefix_id[16];
    unsigned char host_process_id[16];
    unsigned char opener_id[16];
    unsigned char writer_id[16];
    unsigned char desktop_route[16];
    unsigned long long server_gen;
    unsigned long long root_gen;
    unsigned long long broker_gen;
    unsigned long long map_gen;
    unsigned long long policy_gen;
    unsigned int caller_session;
    unsigned long long host_uid;
    struct process *opener_process;
    struct process *writer_process;
    struct desktop *desktop;
    struct token *opener_token;
    struct luid opener_token_id;
    struct luid opener_modified_id;
    int endorsed;
    struct process *dialog_process;
    struct token *dialog_token;
    struct luid dialog_token_id;
    struct luid dialog_modified_id;
    int dialog_pin_pid;
    unsigned char dialog_id[16];
    struct e1r8_tuple sealed;   /* the tuple endorsed at op 2, with dialog_id */
    int receipt_used;   /* R10: one receipt installation attempt per binding */
};

static const unsigned char zero16[16];

void e1lab_pipe_binding_free( struct pipe_binding *b )
{
    if (!b) return;
    if (b->desktop) release_object( b->desktop );
    if (b->opener_process) release_object( b->opener_process );
    if (b->writer_process) release_object( b->writer_process );
    if (b->opener_token) release_object( b->opener_token );
    if (b->dialog_process) release_object( b->dialog_process );
    if (b->dialog_token) release_object( b->dialog_token );
    free( b );
}

/* R8 desktop rule (3e decision 4 as corrected by Claude, 2026-11-10): like the consent prompt on
 * Windows, D runs on the session's interactive (visible) window station whatever window station the
 * caller uses: on its secure desktop for desktop_mode 1, or on the caller's own desktop for mode 2
 * (which therefore needs a caller on that window station). The session is checked separately. */
static int dialog_desktop_ok( struct process *dialog, const struct pipe_binding *b, unsigned int mode )
{
    struct desktop *d = NULL;
    struct winstation *visible = get_visible_winstation();
    int ok;

    if (dialog->desktop && !(d = get_desktop_obj( dialog, dialog->desktop, 0 ))) clear_error();
    ok = d && visible && d->winstation == visible &&
         ((mode == 1 && d->secure) || (mode == 2 && d == b->desktop));
#ifdef WEX_R8_SERVER_TEST
    {
        char dn[64] = "", cn[64] = "";
        data_size_t len = 0, i;
        const WCHAR *name;
        if (d && (name = get_object_name( &d->winstation->obj, &len )))
            for (i = 0; i < len / sizeof(WCHAR) && i < sizeof(dn) - 1; i++) dn[i] = name[i] < 128 ? name[i] : '?';
        if (b->desktop && (name = get_object_name( &b->desktop->winstation->obj, &len )))
            for (i = 0; i < len / sizeof(WCHAR) && i < sizeof(cn) - 1; i++) cn[i] = name[i] < 128 ? name[i] : '?';
        fprintf( stderr, "wex-r8: desktop ok=%d mode=%u d_secure=%d d_ws=%s visible=%d caller_ws=%s caller_visible=%d\n",
                 ok, mode, d ? d->secure : -1, dn, d && d->winstation == visible, cn,
                 b->desktop && b->desktop->winstation == visible );
    }
#endif
    if (d) release_object( d );
    return ok;
}

#ifdef __linux__
/* E1-S18e R10: the consumed record for token issue comes from the verifier, never from broker bytes.
 * After the verifier has consumed the evidence it sends one RECEIPT packet (E1R8 type 30) on the control
 * socket the server retained at endorsement, before it answers the broker. The broker's E1R8 op 4 only
 * names its S10 context; the server reads that one packet without blocking, requires the kernel
 * credentials of the endorsed root verifier, compares every field with the tuple it sealed at
 * ENDORSE_DIALOG and with the current generations, builds the kind 4 input itself and installs it with
 * the retained opener objects. Any failure burns the context. */
struct receipt_cookie
{
    int verifier_pin;
    unsigned long long root_gen, map_gen, policy_gen;
};

static int receipt_validate( void *cookie, const unsigned char *input )
{
    struct receipt_cookie *c = cookie;
    struct pollfd pin = { c->verifier_pin, POLLIN, 0 };
    unsigned long long gen;

    if (poll( &pin, 1, 0 )) return 0;  /* the verifier that vouched has gone */
    if (!accounts_get_map_generation( &gen ) || gen != c->map_gen) return 0;
    if (!sysrun_get_root_generation( &gen ) || gen != c->root_gen) return 0;
    if (!sysrun_get_policy_generation( &gen ) || gen != c->policy_gen) return 0;
    return 1;
}

static void receipt_release( void *cookie )
{
    struct receipt_cookie *c = cookie;
    close( c->verifier_pin );
    free( c );
}

static int receipt_nonzero( const unsigned char *p, unsigned int n )
{
    unsigned char bits = 0;
    while (n--) bits |= *p++;
    return bits != 0;
}

static int install_verifier_receipt( struct pipe_binding *b, obj_handle_t context )
{
    static const unsigned char evidence_head[8] = { 3, 0, 1, 0, 220, 0, 0, 0 };
    unsigned char pkt[512], input[WEX_TOKEN_INPUT_SIZE], t[E1R8_SEALED_SIZE];
    const unsigned char *ev;
    char canonical[64], ancillary[256];
    struct iovec io = { pkt, sizeof(pkt) };
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    struct ucred peer, cred;
    struct pollfd live = { -1, POLLIN, 0 };
    socklen_t len = sizeof(peer);
    struct e1r8_receipt receipt;
    struct e1s_token_consumed r;
    struct receipt_cookie *cookie = NULL;
    struct timespec ts;
    struct luid id, mod;
    unsigned long long now, completed, expiry, gen, server_gen, broker_gen;
    unsigned int i, creds = 0;
    int fd = current->process->e1s6_control_fd, pin = -1, one = 1, ok = 0, err;
    const char *site;
    ssize_t n;

    /* the sealed tuple's bytes, which the evidence record and the kind 4 input carry */
    if (e1r8_encode_sealed_tuple( t, sizeof(t), &b->sealed ))
    {
        site = "receipt-frame";
        goto done;
    }

    site = "receipt-opener";
    if (b->opener_process->is_terminating || !b->opener_process->running_threads ||
        b->opener_process->token != b->opener_token) goto done;
    token_get_ids( b->opener_token, &id, &mod );
    if (memcmp( &id, &b->opener_token_id, sizeof(id) ) || memcmp( &mod, &b->opener_modified_id, sizeof(mod) )) goto done;

    site = "receipt-generations";
    if (!sysrun_get_s6_generations( current->process, &server_gen, &broker_gen ) ||
        server_gen != b->server_gen || broker_gen != b->broker_gen) goto done;
    if (!accounts_get_map_generation( &gen ) || gen != b->map_gen) goto done;
    if (!sysrun_get_root_generation( &gen ) || gen != b->root_gen) goto done;
    if (!sysrun_get_policy_generation( &gen ) || gen != b->policy_gen) goto done;

    /* the endorsed verifier: root peer of the retained control socket, pinned and alive */
    site = "receipt-verifier";
    if (fd < 0 || getsockopt( fd, SOL_SOCKET, SO_PEERCRED, &peer, &len ) || len != sizeof(peer) ||
        peer.uid != 0 || peer.pid <= 0) goto done;
    len = sizeof(pin);
    if (getsockopt( fd, SOL_SOCKET, SO_PEERPIDFD, &pin, &len ) || len != sizeof(pin) || pin < 0)
    { pin = -1; goto done; }
    live.fd = pin;
    if (poll( &live, 1, 0 )) goto done;
    if (setsockopt( fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one) )) goto done;

    /* exactly one packet, already queued: the verifier sends it before it answers the broker */
    site = "receipt-receive";
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = ancillary; msg.msg_controllen = sizeof(ancillary);
    n = recvmsg( fd, &msg, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC );
    if (n < 0) goto done;
    for (cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS &&
            cmsg->cmsg_len == CMSG_LEN( sizeof(cred) ))
        {
            memcpy( &cred, CMSG_DATA( cmsg ), sizeof(cred) );
            if (cred.uid != 0 || cred.pid != peer.pid) creds = 2;
            creds++;
        }
        else
        {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            {
                int *fds = (int *)CMSG_DATA( cmsg );
                unsigned int count = (cmsg->cmsg_len - CMSG_LEN( 0 )) / sizeof(int);
                for (i = 0; i < count; i++) close( fds[i] );
            }
            creds = 2;
        }
    }
    site = "receipt-sender";
    if (creds != 1 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) goto done;

    /* the frame: a receipt's length, this attempt's request, attempt, nonce and channel as sealed at ENDORSE_DIALOG
     * and a nonzero evidence reference; the record, then the name, are checked below in this order */
    site = "receipt-frame";
    if (n < 317 || n > 379 || e1r8_get_header( pkt, n, E1R8_TYPE_RECEIPT, &b->sealed, NULL ) ||
        !receipt_nonzero( pkt + 76, 16 )) goto done;
    ev = pkt + 92;

    /* the exact 220-byte evidence record against the sealed tuple and the binding's generations */
    site = "receipt-evidence";
    if (clock_gettime( CLOCK_MONOTONIC, &ts ) || ts.tv_sec <= 0) goto done;
    now = (unsigned long long)ts.tv_sec * 1000000000 + ts.tv_nsec;
    completed = e1r8_get_u64( ev + 148 );
    expiry = e1r8_get_u64( ev + 156 );
    if (memcmp( ev, evidence_head, 8 ) || e1r8_get_u64( ev + 8 ) != 0x3ff ||
        e1r8_get_u32( ev + 212 ) != 1 || e1r8_get_u32( ev + 216 ) != 1) goto done;
    site = "receipt-evidence-request";
    if (memcmp( ev + 16, t, 8 ) || memcmp( ev + 24, t + 92, 32 ) || memcmp( ev + 56, t + 60, 32 ) ||
        memcmp( ev + 88, t + 12, 32 ) || memcmp( ev + 120, t + 8, 4 )) goto done;
    site = "receipt-evidence-account";
    if (!receipt_nonzero( ev + 124, 16 ) || !e1r8_get_u64( ev + 140 ) ||
        e1r8_get_u64( ev + 140 ) >= 0xffffffffull) goto done;
    site = "receipt-evidence-time";
    if (!completed || completed > now || now >= expiry || expiry > b->sealed.deadline) goto done;
    /* The record's root and verifier generations are the verifier's own instance values: the verifier
     * generation is the one sealed in the tuple; its root generation has no server counterpart (the
     * server's root generation is a path generation and stays in the kind 4 input from the tuple). */
    site = "receipt-evidence-generations";
    if (e1r8_get_u64( ev + 172 ) != b->server_gen || e1r8_get_u64( ev + 180 ) != b->broker_gen ||
        e1r8_get_u64( ev + 188 ) != b->sealed.verifier_gen || e1r8_get_u64( ev + 196 ) != b->map_gen ||
        e1r8_get_u64( ev + 204 ) != b->policy_gen) goto done;

    /* the whole frame through the codec, which also checks the canonical account name (its account rule) */
    site = "receipt-name";
    if ((err = e1r8_decode_receipt( pkt, n, &b->sealed, &receipt ))) goto done;
    memcpy( canonical, receipt.account, receipt.account_len );
    canonical[receipt.account_len] = 0;

    /* kind 4 from the sealed tuple and the receipt; nothing here is a broker byte */
    memset( input, 0, sizeof(input) );
    wex_token_put32( input, 0x00010004 );
    wex_token_put32( input + 4, WEX_TOKEN_REQUEST_SIZE );
    wex_token_put64( input + 8, WEX_TOKEN_CAPS );
    memcpy( input + 16, t, 8 );             /* request id */
    memcpy( input + 24, t + 92, 32 );       /* request digest */
    memcpy( input + 56, t + 60, 32 );       /* binding digest */
    memcpy( input + 88, t + 12, 32 );       /* nonce */
    memcpy( input + 120, t + 8, 4 );        /* attempt */
    memcpy( input + 124, ev + 124, 16 );    /* account key the verifier authenticated */
    memcpy( input + 140, t + 132, 24 );     /* root, server, broker generations */
    memcpy( input + 164, t + 164, 16 );     /* map, policy generations */
    wex_token_put32( input + 180, b->caller_session );
    memcpy( input + 188, receipt.evidence_ref, 16 );  /* evidence reference */

    site = "receipt-install";
    if (!(cookie = malloc( sizeof(*cookie) ))) goto done;
    cookie->verifier_pin = pin;
    cookie->root_gen = b->root_gen; cookie->map_gen = b->map_gen; cookie->policy_gen = b->policy_gen;
    memset( &r, 0, sizeof(r) );
    memcpy( r.input, input, sizeof(r.input) );
    r.canonical_name = canonical;
    r.caller = b->opener_process;
    r.primary = b->opener_token;
    r.expiry = expiry;
    r.validate = receipt_validate;
    r.release = receipt_release;
    r.cookie = cookie;
    if ((ok = token_install_e1s_consumed( context, &r ))) { pin = -1; cookie = NULL; }
done:
    if (!ok)
    {
        E1S6_BINDING_AUDIT( site );
        token_install_e1s_consumed( context, NULL );  /* a failed receipt burns the context */
        set_error( STATUS_ACCESS_DENIED );
    }
    else fprintf( stderr, "wex-r8: receipt installed\n" );
    free( cookie );
    if (pin >= 0) close( pin );
    return ok;
}
#endif /* __linux__ */

#define DENY(site) do { E1S6_BINDING_AUDIT( site ); set_error( STATUS_ACCESS_DENIED ); return 0; } while (0)
#define DENY_D(site) do { release_object( D ); DENY( site ); } while (0)

/* Fill an id from /dev/urandom until it is nonzero and differs from the given ones. */
static int random_id( int rng, unsigned char id[16], const unsigned char *const *others, unsigned int count )
{
    unsigned int i;
    for (;;)
    {
        int clash = !memcmp( id, zero16, 16 );
        for (i = 0; i < count && !clash; i++) clash = !memcmp( id, others[i], 16 );
        if (!clash) return 1;
        if (read( rng, id, 16 ) != 16) return 0;
    }
}

/* op 1: bind this connection's opener, writer and desktop; reply with the binding's ids and generations */
static int e1r8_bind( struct pipe_binding **binding, struct process *opener, struct token *opener_primary,
                      struct process *writer_process, struct desktop *writer_desktop, data_size_t size )
{
    struct pipe_binding *b;
    unsigned char reply_buf[132], *ptr;
    int rng, i;

    if (size != 8) DENY( "e1r8-size" );
    if (!sysrun_process_is_broker( current->process )) DENY( "caller-not-broker" );
    if (!process_pin_matches( opener )) DENY( "opener-pin-missing" );
    if (!writer_desktop) DENY( "writer-desktop-missing" );
    if (*binding) DENY( "binding-exists" );

    if (!(b = mem_alloc( sizeof(*b) )))
    {
        E1S6_BINDING_AUDIT( "binding-alloc" );
        set_error( STATUS_NO_MEMORY );
        return 0;
    }
    memset( b, 0, sizeof(*b) );

    if (!registry_get_prefix_id( b->prefix_id ))
    {
        free( b );
        DENY( "prefix-id" );
    }

    if ((rng = open( "/dev/urandom", O_RDONLY | O_CLOEXEC )) < 0)
    {
        free( b );
        DENY( "rng-open" );
    }
    {
        const unsigned char *const others[] = { b->prefix_id, b->host_process_id, b->opener_id, b->writer_id };
        if (!random_id( rng, b->host_process_id, others, 1 ) || !random_id( rng, b->opener_id, others, 2 ) ||
            !random_id( rng, b->writer_id, others, 3 ) || !random_id( rng, b->desktop_route, others, 4 ))
        {
            close( rng );
            free( b );
            set_error( STATUS_ACCESS_DENIED );
            return 0;
        }
    }
    close( rng );

    if (!sysrun_get_s6_generations( current->process, &b->server_gen, &b->broker_gen ))
    {
        free( b );
        DENY( "s6-generations" );
    }
    if (!accounts_get_map_generation( &b->map_gen ))
    {
        free( b );
        DENY( "map-generation" );
    }
    if (!sysrun_get_root_generation( &b->root_gen ) || !sysrun_get_policy_generation( &b->policy_gen ))
    {
        free( b );
        DENY( "path-generations" );
    }

    b->caller_session = opener->session_id;
    b->host_uid = opener->wex_pin_uid;
    b->opener_process = (struct process *)grab_object( opener );
    b->writer_process = (struct process *)grab_object( writer_process );
    b->desktop = (struct desktop *)grab_object( writer_desktop );
    b->opener_token = (struct token *)grab_object( opener_primary );
    token_get_ids( b->opener_token, &b->opener_token_id, &b->opener_modified_id );
    *binding = b;

    ptr = reply_buf;
    memcpy( ptr, b->prefix_id, 16 ); ptr += 16;
    memcpy( ptr, b->host_process_id, 16 ); ptr += 16;
    memcpy( ptr, b->opener_id, 16 ); ptr += 16;
    memcpy( ptr, b->writer_id, 16 ); ptr += 16;
    memcpy( ptr, b->desktop_route, 16 ); ptr += 16;

    for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->server_gen >> (8 * i));
    for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->root_gen >> (8 * i));
    for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->broker_gen >> (8 * i));
    for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->map_gen >> (8 * i));
    for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->policy_gen >> (8 * i));

    for (i = 0; i < 4; i++) *ptr++ = (unsigned char)(b->caller_session >> (8 * i));
    for (i = 0; i < 8; i++) *ptr++ = (unsigned char)(b->host_uid >> (8 * i));
    assert( ptr - reply_buf == 132 );
    set_reply_data( reply_buf, sizeof(reply_buf) );
    return 1;
}

/* Send ENDORSED to the endorsed verifier on the retained control socket, with D's pin and the prefix directory. */
static int send_endorsed( const struct e1r8_tuple *t, struct process *D )
{
    unsigned char endorsed_buf[244];
    struct ucred sender = { getpid(), geteuid(), getegid() };
    char ancillary[CMSG_SPACE(sizeof(sender)) + CMSG_SPACE(2 * sizeof(int))];
    struct iovec io = { endorsed_buf, sizeof(endorsed_buf) };
    struct msghdr msg = { 0 };
    struct cmsghdr *cmsg;
    int d_pidfd, raised, sent_ok, rights[2];

    if (e1r8_encode_endorsed( endorsed_buf, sizeof(endorsed_buf), t, (unsigned int)D->wex_pin_pid, D->wex_pin_uid ))
        return 0;

    /* a duplicate of D's kernel pin, never a fresh lookup by pid (pid reuse) */
    d_pidfd = fcntl( D->wex_pin_pidfd, F_DUPFD_CLOEXEC, 0 );
    if (d_pidfd < 0) return -1;

    memset( ancillary, 0, sizeof(ancillary) );
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ancillary;
    msg.msg_controllen = sizeof(ancillary);

    cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN( sizeof(sender) );

    cmsg = CMSG_NXTHDR( &msg, cmsg );
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    /* second right: this prefix's directory, so the verifier can provision the
     * administrator's profile in it without trusting a path (the kernel duplicates it) */
    rights[0] = d_pidfd;
    rights[1] = config_dir_fd;
    cmsg->cmsg_len = CMSG_LEN( sizeof(rights) );
    memcpy( CMSG_DATA(cmsg), rights, sizeof(rights) );

    sent_ok = 0;
    raised = sysrun_raise();
    if (raised)
    {
        sender.pid = getpid();
        sender.uid = geteuid();
        sender.gid = getegid();
        cmsg = CMSG_FIRSTHDR( &msg );
        memcpy( CMSG_DATA(cmsg), &sender, sizeof(sender) );
        sent_ok = (sendmsg( current->process->e1s6_control_fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL ) == (ssize_t)sizeof(endorsed_buf));
    }
    sysrun_drop( raised );
    close( d_pidfd );
    return sent_ok;
}

/* op 2 (ENDORSE_DIALOG): check the broker's dialog D and its pre-endorsement tuple, seal the tuple with a new
 * dialog id and send ENDORSED to the verifier */
static int e1r8_endorse_dialog( struct pipe_binding *b, const unsigned char *data, data_size_t size )
{
    struct e1r8_tuple t;
    obj_handle_t d_handle;
    struct process *D;
    unsigned long long cur_server_gen, cur_broker_gen;
    unsigned long long cur_map_gen, cur_root_gen, cur_policy_gen;
    struct luid op_id, op_mod;
    int rng, sent;

    if (size != 12 + E1R8_PRE_ENDORSEMENT_SIZE) DENY( "e1r8-size" );
    /* the decoder's only possible error here, an unknown desktop mode, is refused below (after the dialog's
     * checks, where the server has always refused it); the tuple is filled in either case */
    e1r8_decode_pre_endorsement_tuple( data + 12, E1R8_PRE_ENDORSEMENT_SIZE, &t );
    if (!sysrun_process_is_broker( current->process )) DENY( "caller-not-broker" );
    if (current->token) DENY( "caller-has-token" );
    if (!sysrun_get_s6_generations( current->process, &cur_server_gen, &cur_broker_gen )) DENY( "s6-generations" );
    if (!b) DENY( "binding-missing" );
    /* A further attempt of the same request gets a fresh dialog: only once the previous
     * dialog is gone, before any receipt, with the next attempt number (at most 3) and the
     * same request id and deadline. */
    if (b->endorsed &&
        (b->receipt_used || b->dialog_process->running_threads ||
         t.request_id != b->sealed.request_id ||
         t.attempt != b->sealed.attempt + 1 ||
         t.attempt > 3 ||
         t.deadline != b->sealed.deadline))
        DENY( "already-endorsed" );

    d_handle = (obj_handle_t)e1r8_get_u32( data + 8 );
    D = get_process_from_handle( d_handle, PROCESS_QUERY_LIMITED_INFORMATION );
    if (!D) DENY( "dialog-handle" );
    if (D->wex_creator != current->process) DENY_D( "dialog-creator" );
    if (!D->wex_protected[0]) DENY_D( "dialog-not-protected" );
    if (!D->wex_dialog_image) DENY_D( "dialog-image-not-consent" );
    if (!process_unstarted_suspended( D )) DENY_D( "dialog-not-suspended" );
    if (token_service_account( D->token ) != SERVICE_ACCOUNT_SYSTEM) DENY_D( "dialog-not-system" );
    if (!process_pin_matches( D )) DENY_D( "dialog-pin-mismatch" );
    if (D->session_id != b->caller_session) DENY_D( "dialog-session-mismatch" );
    if (!dialog_desktop_ok( D, b, t.desktop_mode )) DENY_D( "dialog-desktop-mismatch" );

    if (!t.deadline) DENY_D( "tuple-deadline" );
    if (t.root_gen != b->root_gen) DENY_D( "tuple-root-gen" );
    if (t.server_gen != b->server_gen) DENY_D( "tuple-server-gen" );
    if (t.broker_gen != b->broker_gen) DENY_D( "tuple-broker-gen" );
    /* verifier_gen is not checked by the server */
    if (t.map_gen != b->map_gen) DENY_D( "tuple-map-gen" );
    if (t.policy_gen != b->policy_gen) DENY_D( "tuple-policy-gen" );
    if (t.caller_session != b->caller_session) DENY_D( "tuple-caller-session" );
    if (memcmp( t.desktop_route, b->desktop_route, 16 )) DENY_D( "tuple-desktop-route" );
    if (t.desktop_mode != E1R8_DESKTOP_MODE_SECURE && t.desktop_mode != E1R8_DESKTOP_MODE_INSECURE)
        DENY_D( "tuple-desktop-mode" );

    if (cur_server_gen != b->server_gen || cur_broker_gen != b->broker_gen) DENY_D( "current-s6-generations" );
    if (!accounts_get_map_generation( &cur_map_gen ) || cur_map_gen != b->map_gen) DENY_D( "current-map-generation" );
    if (!sysrun_get_root_generation( &cur_root_gen ) || cur_root_gen != b->root_gen ||
        !sysrun_get_policy_generation( &cur_policy_gen ) || cur_policy_gen != b->policy_gen)
        DENY_D( "current-path-generations" );

    if (b->opener_process->is_terminating || b->opener_process->running_threads == 0) DENY_D( "opener-dead" );
    if (b->opener_process->token != b->opener_token) DENY_D( "opener-token-changed" );
    token_get_ids( b->opener_token, &op_id, &op_mod );
    if (memcmp( &op_id, &b->opener_token_id, sizeof(op_id) ) ||
        memcmp( &op_mod, &b->opener_modified_id, sizeof(op_mod) ))
        DENY_D( "opener-token-id-changed" );

    if ((rng = open( "/dev/urandom", O_RDONLY | O_CLOEXEC )) < 0) DENY_D( "rng-open" );
    if (b->endorsed) memset( b->dialog_id, 0, 16 );  /* a further attempt: a new dialog id */
    {
        const unsigned char *const others[] = { b->prefix_id, b->host_process_id, b->opener_id, b->writer_id,
                                                b->desktop_route };
        if (!random_id( rng, b->dialog_id, others, 5 ))
        {
            close( rng );
            DENY_D( "rng-read" );
        }
    }
    close( rng );

    if (current->process->e1s6_control_fd < 0) DENY_D( "endorsed-control-fd" );
    memcpy( t.dialog_id, b->dialog_id, 16 );
    if ((sent = send_endorsed( &t, D )) < 0) DENY_D( "endorsed-pidfd-open" );
    if (!sent)
    {
        close( current->process->e1s6_control_fd );
        current->process->e1s6_control_fd = -1;
        DENY_D( "endorsed-send" );
    }

    if (b->dialog_process) release_object( b->dialog_process );
    if (b->dialog_token) release_object( b->dialog_token );
    b->dialog_process = (struct process *)grab_object( D );
    b->dialog_token = (struct token *)grab_object( D->token );
    token_get_ids( b->dialog_token, &b->dialog_token_id, &b->dialog_modified_id );
    b->dialog_pin_pid = D->wex_pin_pid;
    b->sealed = t;
    b->endorsed = 1;
    release_object( D );

    set_reply_data( b->dialog_id, 16 );
    return 1;
}

/* op 3 (RECHECK): the endorsed dialog and the opener are unchanged */
static int e1r8_recheck( struct pipe_binding *b, const unsigned char *data, data_size_t size )
{
    unsigned long long cur_server_gen, cur_broker_gen;
    unsigned long long cur_map_gen, cur_root_gen, cur_policy_gen;
    struct luid id, mod;

    if (size != 24) DENY( "e1r8-size" );
    if (!sysrun_process_is_broker( current->process )) DENY( "recheck-caller-not-broker" );
    if (current->token) DENY( "recheck-caller-has-token" );
    if (!sysrun_get_s6_generations( current->process, &cur_server_gen, &cur_broker_gen ))
        DENY( "recheck-s6-generations" );
    if (!b || !b->endorsed) DENY( "recheck-not-endorsed" );
    if (memcmp( data + 8, b->dialog_id, 16 )) DENY( "recheck-dialog-id-mismatch" );

    if (b->dialog_process->is_terminating || b->dialog_process->running_threads == 0) DENY( "recheck-dialog-dead" );
    if (b->dialog_process->token != b->dialog_token) DENY( "recheck-dialog-token-changed" );
    token_get_ids( b->dialog_token, &id, &mod );
    if (memcmp( &id, &b->dialog_token_id, sizeof(id) ) ||
        memcmp( &mod, &b->dialog_modified_id, sizeof(mod) ))
        DENY( "recheck-dialog-token-id-changed" );
    if (b->dialog_process->session_id != b->caller_session) DENY( "recheck-dialog-session-mismatch" );
    if (!dialog_desktop_ok( b->dialog_process, b, b->sealed.desktop_mode )) DENY( "recheck-dialog-desktop-mismatch" );
    if (!process_pin_matches( b->dialog_process ) || b->dialog_process->wex_pin_pid != b->dialog_pin_pid)
        DENY( "recheck-dialog-pin-mismatch" );

    if (b->opener_process->is_terminating || b->opener_process->running_threads == 0) DENY( "recheck-opener-dead" );
    if (b->opener_process->token != b->opener_token) DENY( "recheck-opener-token-changed" );
    token_get_ids( b->opener_token, &id, &mod );
    if (memcmp( &id, &b->opener_token_id, sizeof(id) ) ||
        memcmp( &mod, &b->opener_modified_id, sizeof(mod) ))
        DENY( "recheck-opener-token-id-changed" );

    if (cur_server_gen != b->server_gen || cur_broker_gen != b->broker_gen) DENY( "recheck-current-s6-generations" );
    if (!accounts_get_map_generation( &cur_map_gen ) || cur_map_gen != b->map_gen)
        DENY( "recheck-current-map-generation" );
    if (!sysrun_get_root_generation( &cur_root_gen ) || cur_root_gen != b->root_gen ||
        !sysrun_get_policy_generation( &cur_policy_gen ) || cur_policy_gen != b->policy_gen)
        DENY( "recheck-current-path-generations" );
    return 1;
}

int e1lab_pipe_e1r8_op( struct pipe_binding **binding, struct process *opener, struct token *opener_primary,
                        struct process *writer_process, struct desktop *writer_desktop,
                        const void *data, data_size_t size )
{
    const unsigned char *req = data;
    unsigned short op;

    if (size < 8) DENY( "e1r8-size" );
    if (e1r8_get_u16( req + 4 ) != 1) DENY( "e1r8-version" );
    op = e1r8_get_u16( req + 6 );

    switch (op)
    {
    case 1:
        return e1r8_bind( binding, opener, opener_primary, writer_process, writer_desktop, size );
    case 2:
        return e1r8_endorse_dialog( *binding, req, size );
    case 3:
        return e1r8_recheck( *binding, req, size );
#ifdef __linux__
    case 4:
        /* R10: install the verifier's receipt into the broker's S10 context (handle at 8) */
        if (size != 12) DENY( "e1r8-size" );
        if (!sysrun_process_is_broker( current->process ) || current->token) DENY( "receipt-caller-not-broker" );
        if (!*binding || !(*binding)->endorsed || (*binding)->receipt_used) DENY( "receipt-state" );
        (*binding)->receipt_used = 1;
        return install_verifier_receipt( *binding, (obj_handle_t)e1r8_get_u32( req + 8 ) );
#endif
    default:
        DENY( "e1r8-op" );
    }
}

void e1lab_pipe_session0_endorse( const void *data, data_size_t size )
{
    /* E1 R9: session 0's broker B0 registers its S6 channel with the verifier here. It has no caller in this
     * server to bind (its requests come over the bridge; the verifier binds each attempt to the agent's channel),
     * so none of the binding applies: only the endorsement, whose own checks (the broker image mapped by one
     * of session 0's programs, a LocalSystem primary token with SeTcbPrivilege, no thread token) decide. */
    if (size < 4 || !memcmp( data, "E1R8", 4 ))
    {
        E1S6_BINDING_AUDIT( "session0-op" );
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    sysrun_e1s6_endorse( data, size );  /* sets the error itself */
}

#endif /* WEX_E1S6_TRANSPORT_TEST */
