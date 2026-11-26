/*
 * WineExplorer E1 lab: the S10 token context of token.c (never in a shipped build)
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
 * Moved out of token.c by R5 of docs/SESSION0_SPLIT_REVIEW.md, with no change. This file holds code, not
 * declarations (like Wine's code-bearing headers): token.c includes it once, at one point, with the S10 and S6 lab
 * switches only, because the context issues and
 * checks tokens through token.c's own structure and static helpers, which a separate file could reach only if
 * token.c exported them. token.c keeps the functions of e1s_token.h and create_e1s_account_token; with the
 * switches each calls its e1lab_* body here, without them it refuses as before (docs/STOCK_CHANGES.md).
 */

#ifndef WEX_E1LAB_TOKEN_INCLUDED
#error "wex_e1lab_token.h is included by token.c, not compiled on its own"
#endif


struct e1s_token_context
{
    struct object obj;
    struct process *owner; /* weak: owner's handle table owns this object */
    thread_id_t thread;
    struct token *broker_primary;
    struct token *issued;
    int profile_attempted;
    unsigned char profile_lease_id[16]; /* server-only binding, never client bytes */
    struct luid profile_modified;
    struct luid broker_id, broker_modified;
    unsigned long long broker_gen, caps, expiry;
    unsigned int state; /* 0 negotiated, 1 installed, 2 permanently consumed */
    struct process *caller;
    struct token *primary;
    struct luid caller_id, caller_modified;
    unsigned char input[WEX_TOKEN_INPUT_SIZE];
    char name[WEX_ACCOUNT_NAME_MAX + 1];
    int (*validate)( void *, const unsigned char * );
    void (*release)( void * );
    void *cookie;
};

static void e1s_context_dump( struct object *obj, int verbose )
{
    fprintf( stderr, "E1S token context (private)\n" );
}

static void e1s_context_destroy( struct object *obj )
{
    struct e1s_token_context *c = (struct e1s_token_context *)obj;
    if (c->release) c->release( c->cookie );
    if (c->broker_primary) release_object( c->broker_primary );
    if (c->issued) release_object( c->issued );
    if (c->caller) release_object( c->caller );
    if (c->primary) release_object( c->primary );
}

static const struct object_ops e1s_context_ops =
{
    .size = sizeof(struct e1s_token_context),
    .type = &no_type,
    .dump = e1s_context_dump,
    .destroy = e1s_context_destroy,
};

static int e1s_nonzero( const unsigned char *p, unsigned int n )
{
    unsigned char bits = 0;
    while (n--) bits |= *p++;
    return bits != 0;
}

static unsigned long long e1s_now(void)
{
    struct timespec ts;
    if (clock_gettime( CLOCK_MONOTONIC, &ts ) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000 || (unsigned long long)ts.tv_sec > (~0ULL - 1000000000) / 1000000000)
        return 0;
    return (unsigned long long)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* S6's actual image/process endorsement is mandatory in addition to H2's gate.
 * Do not infer broker role from SYSTEM, an inherited handle or a claimed PID. */
static int e1s_broker(void)
{
    struct process *p = current->process;
    struct luid id, modified;
    if (!elevation_enforced() || !sysrun_active || current->token ||
        !sysrun_process_is_system( p ) || !sysrun_process_is_tcb_system( p ) || !p->e1s6_broker_image ||
        !p->e1s6_broker_generation || p->token != p->e1s6_primary) return 0;
    token_get_ids( p->token, &id, &modified );
    return is_equal_luid( id, p->e1s6_token_id ) && is_equal_luid( modified, p->e1s6_modified_id );
}

static int e1s_context_current( const struct e1s_token_context *c )
{
    struct process *p = current->process;
    return c->owner == p && c->thread == current->id && c->broker_primary == p->token &&
           c->broker_gen == p->e1s6_broker_generation &&
           is_equal_luid( c->broker_id, p->e1s6_token_id ) &&
           is_equal_luid( c->broker_modified, p->e1s6_modified_id );
}

static int e1s_request_valid( const unsigned char *in, unsigned long long caps )
{
    unsigned int i;
    if (wex_token_u32( in ) != 0x00010004 || wex_token_u32( in + 4 ) != WEX_TOKEN_REQUEST_SIZE ||
        wex_token_u64( in + 8 ) != caps || !wex_token_u64( in + 16 ) ||
        !e1s_nonzero( in + 24, 32 ) || !e1s_nonzero( in + 56, 32 ) ||
        !e1s_nonzero( in + 88, 32 ) || !wex_token_u32( in + 120 ) ||
        !e1s_nonzero( in + 124, 16 ) || wex_token_u32( in + 184 ) ||
        !e1s_nonzero( in + 188, 16 )) return 0;
    for (i = 140; i < 180; i += 8) if (!wex_token_u64( in + i )) return 0;
    return 1;
}

static int e1s_lookup( const struct e1s_token_context *c, struct wex_account *account )
{
    /* Canonical name is retained by the trusted producer, never part of ISSUE.
     * Re-resolve through S4 on every issuance; never accept a SID from the broker. */
    return lookup_account( c->name, 0, c->caps, account ) == WEX_ACCOUNT_OK && account->administrator &&
           account->generation == wex_token_u64( c->input + 164 ) &&
           !memcmp( account->incarnation, c->input + 124, 16 );
}

static int e1s_caller_current( const struct e1s_token_context *c )
{
    struct token *t = c->primary;
    return c->caller && c->caller->running_threads && c->caller->token == t &&
           c->caller->session_id == t->session_id && t->primary && !t->app_container && !t->is_restricted &&
           list_empty( &t->restricted_sids ) && list_empty( &t->capabilities ) && t->elevation == TokenElevationTypeDefault &&
           t->integrity_rid == SECURITY_MANDATORY_MEDIUM_RID &&
           !token_sid_present( t, &builtin_admins_sid, TRUE ) && token_service_account( t ) == -1 &&
           (t->auth_id.low_part || t->auth_id.high_part) &&
           is_equal_luid( t->token_id, c->caller_id ) && is_equal_luid( t->modified_id, c->caller_modified ) &&
           t->session_id == wex_token_u32( c->input + 180 );
}

/* Build from explicit S4 identity, not token_create_admin(getuid), a linked
 * token, a caller duplicate, or a SYSTEM token with its user SID replaced.
 * S4 v1 has primary GID + fresh mapped-admin membership, no arbitrary Unix
 * supplementary SID list. Keep that precise snapshot; add only fixed Windows
 * baseline groups. Unix group SIDs follow S-1-22-2-gid. No host groups change. */
static struct token *e1s_create_account_token( const struct wex_account *a, struct token *caller )
{
    struct sid user = { SID_REVISION, 5, SECURITY_NT_AUTHORITY, { 0 } };
    struct sid primary = { SID_REVISION, 2, {0,0,0,0,0,22}, { 2, 0 } };
    struct sid logon = { SID_REVISION, 3, SECURITY_NT_AUTHORITY, { SECURITY_LOGON_IDS_RID, 0, 0 } };
    const unsigned int grp = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
    const struct sid_attrs groups[] =
    {
        { &world_sid, grp }, { &local_sid, grp }, { &interactive_sid, grp },
        { &authenticated_user_sid, grp }, { &builtin_users_sid, grp },
        { &builtin_admins_sid, grp | SE_GROUP_OWNER },
        { &primary, grp },
        { &logon, grp | SE_GROUP_LOGON_ID },
    };
    const struct luid_attr privs[] =
    {
        { SeChangeNotifyPrivilege, SE_PRIVILEGE_ENABLED }, { SeTcbPrivilege, 0 },
        { SeSecurityPrivilege, 0 }, { SeBackupPrivilege, 0 }, { SeRestorePrivilege, 0 },
        { SeSystemtimePrivilege, 0 }, { SeShutdownPrivilege, 0 }, { SeRemoteShutdownPrivilege, 0 },
        { SeTakeOwnershipPrivilege, 0 }, { SeDebugPrivilege, 0 }, { SeSystemEnvironmentPrivilege, 0 },
        { SeSystemProfilePrivilege, 0 }, { SeProfileSingleProcessPrivilege, 0 },
        { SeIncreaseBasePriorityPrivilege, 0 }, { SeLoadDriverPrivilege, SE_PRIVILEGE_ENABLED },
        { SeCreatePagefilePrivilege, 0 }, { SeIncreaseQuotaPrivilege, 0 }, { SeUndockPrivilege, 0 },
        { SeManageVolumePrivilege, 0 }, { SeImpersonatePrivilege, SE_PRIVILEGE_ENABLED },
        { SeCreateGlobalPrivilege, SE_PRIVILEGE_ENABLED },
    };
    struct token *t;
    struct acl *dacl;
    struct luid auth;

    /* No wrapped/zero logon IDs, even at allocator exhaustion. Reserve room
     * for create_token's ids and label/descriptor security-version changes. */
    if ((unsigned int)prev_luid_value.high_part == ~0u && prev_luid_value.low_part >= ~0u - 8)
    {
        set_error( STATUS_INSUFFICIENT_RESOURCES );
        return NULL;
    }
    alloc_logon_luid( &auth );
    if ((!auth.low_part && !auth.high_part) || is_equal_luid( auth, caller->auth_id ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return NULL;
    }
    memcpy( user.sub_auth, a->sid, sizeof(a->sid) );
    primary.sub_auth[1] = a->primary_gid;
    logon.sub_auth[1] = auth.high_part;
    logon.sub_auth[2] = auth.low_part;
    if (!(dacl = create_default_dacl( &user ))) return NULL;
    t = create_token( TRUE, caller->session_id, &user, &user,
                      &primary, groups, ARRAY_SIZE(groups), privs, ARRAY_SIZE(privs),
                      dacl, NULL, -1, TokenElevationTypeFull );
    free( dacl );
    if (!t) return NULL;
    t->auth_id = auth;
    if (!token_assign_label( t, &high_label_sid ) || !token_protect_elevated( t ))
    {
        release_object( t );
        return NULL;
    }
    return t;
}

static int e1s_install_consumed( struct e1s_token_context *c, const struct e1s_token_consumed *r )
{
    unsigned long long now;
    size_t n;
    if (c->state) return 0;
    c->state = 2;
    if (!r || !r->canonical_name || !r->caller || !r->primary || !r->validate || !r->release ||
        !e1s_request_valid( r->input, c->caps ) || wex_token_u64( r->input + 156 ) != c->broker_gen ||
        !(now = e1s_now()) || now >= r->expiry || r->caller == c->owner ||
        r->caller->token != r->primary) return 0;
    for (n = 0; n <= WEX_ACCOUNT_NAME_MAX && r->canonical_name[n]; n++)
        if ((unsigned char)r->canonical_name[n] > 127) return 0;
    if (!n || n > WEX_ACCOUNT_NAME_MAX || !r->validate( r->cookie, r->input )) return 0;
    memcpy( c->input, r->input, sizeof(c->input) );
    memcpy( c->name, r->canonical_name, n + 1 );
    c->caller = (struct process *)grab_object( r->caller );
    c->primary = (struct token *)grab_object( r->primary );
    token_get_ids( c->primary, &c->caller_id, &c->caller_modified );
    if (!e1s_caller_current( c ))
    {
        release_object( c->caller );
        release_object( c->primary );
        c->caller = NULL;
        c->primary = NULL;
        return 0;
    }
    c->expiry = r->expiry;
    c->validate = r->validate; c->release = r->release; c->cookie = r->cookie;
    c->state = 1;
    return 1;
}

#ifdef WEX_E1_TEST_HOOKS
/* Sole current producer: synthetic lab data. No PAM or S8 redemption is implied.
 * S18 calls the internal handoff above only after actual evidence redemption.
 * These fixture callbacks cannot be selected through a production operation. */
static int e1s_fixture_validate( void *cookie, const unsigned char *input ) { return 1; }
static void e1s_fixture_release( void *cookie ) {}

static int e1s_install_fixture( struct e1s_token_context *c, obj_handle_t caller,
                               const unsigned char *in, data_size_t size )
{
    struct e1s_token_consumed r;
    struct process *process;
    char name[64];
    unsigned int n;
    unsigned long long now;
    int ret;
    if (c->state) return 0;
    if (size != WEX_TOKEN_FIXTURE_SIZE || !e1s_request_valid( in, c->caps ) ||
        wex_token_u64( in + 156 ) != c->broker_gen) goto failed;
    for (n = 0; n < 64 && in[204+n]; n++)
        if (in[204+n] > 127) goto failed;
    if (!n || n == 64 || e1s_nonzero( in + 204 + n, 64 - n )) goto failed;
    memcpy( name, in + 204, 64 );
    if (!(process = get_process_from_handle( caller, PROCESS_QUERY_LIMITED_INFORMATION ))) goto failed;
    /* Deliberately synthetic identity, including unknown/tombstoned/stale
     * records for negative issuance probes. ISSUE always calls real S4. */
    memset( &r, 0, sizeof(r) );
    memcpy( r.input, in, sizeof(r.input) ); r.canonical_name = name;
    r.caller = process; r.primary = process->token;
    now = e1s_now();
    r.expiry = now && now <= ~0ULL - 5000000000ULL ? now + 5000000000ULL : 0;
    r.validate = e1s_fixture_validate; r.release = e1s_fixture_release;
    ret = e1s_install_consumed( c, &r );
    release_object( process );
    return ret;
failed:
    c->state = 2;
    return 0;
}
#endif

static void e1s_issue( struct e1s_token_context *c, const unsigned char *in, data_size_t size,
                       obj_handle_t *handle, unsigned char *out )
{
    struct wex_account account, fresh;
    struct token *t;
    unsigned long long now;
    unsigned int i, installed = c->state == 1;

    c->state = 2; /* burn before parsing, account lookup, allocation or comparison */
    set_error( STATUS_ACCESS_DENIED );
    if (!installed || size != WEX_TOKEN_INPUT_SIZE || !e1s_request_valid( in, c->caps ) ||
        memcmp( in, c->input, WEX_TOKEN_INPUT_SIZE ) || !(now = e1s_now()) || now >= c->expiry ||
        !e1s_caller_current( c ) || !c->validate( c->cookie, c->input ) || !e1s_lookup( c, &account )) return;
    clear_error();
    if (!(t = e1s_create_account_token( &account, c->primary ))) return;
    /* Slow NSS/allocation must not extend the deadline or permit a changed caller. */
    if (!e1s_caller_current( c ) || !c->validate( c->cookie, c->input ) || !e1s_lookup( c, &fresh ) ||
        fresh.uid != account.uid || fresh.primary_gid != account.primary_gid ||
        strcmp( fresh.name, account.name ) || memcmp( fresh.sid, account.sid, sizeof(account.sid) ) ||
        !(now = e1s_now()) || now >= c->expiry)
    {
        set_error( STATUS_ACCESS_DENIED );
        release_object( t );
        return;
    }
    clear_error();
    *handle = alloc_handle_no_access_check( current->process, t,
                                            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY, 0 );
    if (*handle)
    {
        t->e1s_account = account;
        c->issued = (struct token *)grab_object( t );
        memset( out, 0, WEX_TOKEN_RESPONSE_SIZE );
        wex_token_put32( out, 0x00010005 ); wex_token_put32( out + 4, WEX_TOKEN_RESPONSE_SIZE );
        wex_token_put64( out + 8, c->caps );
        memcpy( out + 16, in + 16, 8 ); memcpy( out + 24, in + 148, 8 );
        /* token_ref is scoped by this context/server; not a handle or bearer grant. */
        wex_token_put32( out + 40, t->token_id.low_part ); wex_token_put32( out + 44, t->token_id.high_part );
        wex_token_put32( out + 48, t->auth_id.low_part ); wex_token_put32( out + 52, t->auth_id.high_part );
        memcpy( out + 56, account.incarnation, 16 );
        wex_token_put32( out + 72, 28 ); out[76] = SID_REVISION; out[77] = 5; out[83] = 5;
        for (i = 0; i < 5; i++) wex_token_put32( out + 84 + 4*i, account.sid[i] );
        wex_token_put32( out + 104, t->auth_id.low_part ); wex_token_put32( out + 108, t->auth_id.high_part );
        wex_token_put32( out + 112, t->token_id.low_part ); wex_token_put32( out + 116, t->token_id.high_part );
        wex_token_put32( out + 120, t->session_id ); wex_token_put32( out + 124, TokenPrimary );
        wex_token_put32( out + 128, TokenElevationTypeFull );
        wex_token_put32( out + 132, SECURITY_MANDATORY_HIGH_RID );
    }
    release_object( t );
}

/* The lab bodies of the S13/S14 profile functions of e1s_token.h (token.c keeps the functions) */
#if defined(WEX_E1S13_HKU_TEST)

static int e1lab_bind_e1s_profile( obj_handle_t context, struct token *token, const unsigned char *lease_id )
{
    struct e1s_token_context *c;
    int ret = 0;
    if (!e1s_broker() || !(c = (struct e1s_token_context *)get_handle_obj(
            current->process, context, 0, &e1s_context_ops ))) return 0;
    if (e1s_context_current( c ) && c->state == 2 && c->profile_attempted && c->issued == token &&
        !e1s_nonzero( c->profile_lease_id, 16 ))
    {
        memcpy( c->profile_lease_id, lease_id, 16 );
        c->profile_modified = token->modified_id;
        ret = 1;
    }
    release_object( c );
    return ret;
}

static int e1lab_validate_e1s_commit( obj_handle_t context, struct token *token, const unsigned char *lease_id,
                                      uint64_t request_id, uint64_t server_gen )
{
    struct e1s_token_context *c;
    unsigned int status = STATUS_SUCCESS;
    if (!(c = (struct e1s_token_context *)get_handle_obj( current->process, context, 0, &e1s_context_ops )))
        status = STATUS_INVALID_HANDLE;
    else
    {
        if (!e1s_broker() || !e1s_context_current( c )) status = STATUS_INVALID_OWNER;
        else if (c->state != 2 || !c->profile_attempted || c->issued != token ||
                 !is_equal_luid( c->profile_modified, token->modified_id )) status = STATUS_BAD_TOKEN_TYPE;
        else if (wex_token_u64( c->input + 148 ) != server_gen) status = STATUS_REVISION_MISMATCH;
        else if (wex_token_u64( c->input + 16 ) != request_id ||
                 memcmp( c->profile_lease_id, lease_id, 16 )) status = STATUS_INVALID_PARAMETER_MIX;
        release_object( c );
    }
    if (status)
        fprintf( stderr, "E1S14 deny=%s status=%08x\n",
                 status == STATUS_INVALID_HANDLE ? "commit-context-handle" :
                 status == STATUS_INVALID_OWNER ? "commit-context-owner" :
                 status == STATUS_BAD_TOKEN_TYPE ? "commit-context-token" :
                 status == STATUS_REVISION_MISMATCH ? "commit-context-generation" : "commit-context-lease", status );
    set_error( status );
    return !status;
}

static int e1lab_validate_e1s_profile( obj_handle_t context, struct token *token, const struct wex_account *account )
{
    struct e1s_token_context *c;
    struct wex_account fresh;
    unsigned long long now;
    int ret = 0;
    if (!e1s_broker()) return 0;
    c = (struct e1s_token_context *)get_handle_obj( current->process, context, 0, &e1s_context_ops );
    if (!c) return 0;
    if (e1s_context_current( c ) && c->issued == token && c->state == 2 &&
        (now = e1s_now()) && now < c->expiry && e1s_caller_current( c ) &&
        c->validate( c->cookie, c->input ) && e1s_lookup( c, &fresh ) &&
        !strcmp( fresh.name, account->name ) && fresh.uid == account->uid &&
        fresh.primary_gid == account->primary_gid && fresh.generation == account->generation &&
        !memcmp( fresh.sid, account->sid, sizeof(fresh.sid) ) &&
        !memcmp( fresh.incarnation, account->incarnation, 16 )) ret = 1;
    release_object( c );
    return ret;
}

static int e1lab_claim_e1s_profile( obj_handle_t context, struct token *token, const void *record,
                                    data_size_t size, struct wex_account *account )
{
    struct e1s_token_context *c;
    const unsigned char *in = record;
    int ret = 0;
    if (!e1s_broker()) return 0;
    c = (struct e1s_token_context *)get_handle_obj( current->process, context, 0, &e1s_context_ops );
    if (!c) return 0;
    if (e1s_context_current( c ) && !c->profile_attempted)
    {
        c->profile_attempted = 1; /* malformed references burn too */
        if (size == WEX_HKU_RECORD_SIZE && wex_token_u32( in ) == 1 &&
            wex_token_u32( in + 4 ) == WEX_HKU_RECORD_SIZE && !wex_token_u64( in + 40 ) &&
            wex_token_u64( in + 32 ) && token && token->e1s_account.generation &&
            !memcmp( in + 8, token->e1s_account.incarnation, 16 ) &&
            wex_token_u64( in + 24 ) == token->e1s_account.generation &&
            token_validate_e1s_profile( context, token, &token->e1s_account ))
        { *account = token->e1s_account; ret = 1; }
    }
    release_object( c );
    return ret;
}

static int e1lab_query_e1s_profile( obj_handle_t context, struct token *token, const void *record,
                                    data_size_t size, struct wex_account *account )
{
    struct e1s_token_context *c;
    const unsigned char *in = record;
    int ret = 0;
    if (!e1s_broker() || !token) return 0;
    c = (struct e1s_token_context *)get_handle_obj( current->process, context, 0, &e1s_context_ops );
    if (!c) return 0;
    if (e1s_context_current( c ) && !c->profile_attempted &&
        size == WEX_HKU_RECORD_SIZE && wex_token_u32( in ) == 1 &&
        wex_token_u32( in + 4 ) == WEX_HKU_RECORD_SIZE && !wex_token_u64( in + 32 ) && !wex_token_u64( in + 40 ) &&
        token->e1s_account.generation && !memcmp( in + 8, token->e1s_account.incarnation, 16 ) &&
        wex_token_u64( in + 24 ) == token->e1s_account.generation)
    {
        release_object( c );
        if (!token_validate_e1s_profile( context, token, &token->e1s_account )) return 0;
        *account = token->e1s_account;
        return 1;
    }
    release_object( c );
    return ret;
}

static int e1lab_e1s_profile_request( obj_handle_t context, struct token *token, uint64_t *request_id,
                                      uint64_t *server_gen )
{
    struct e1s_token_context *c;
    int ret = 0;
    if (!token_validate_e1s_profile( context, token, token_e1s_account( token ) )) return 0;
    if (!(c = (struct e1s_token_context *)get_handle_obj( current->process, context, 0, &e1s_context_ops ))) return 0;
    *request_id = wex_token_u64( c->input + 16 );
    *server_gen = wex_token_u64( c->input + 148 );
    ret = *request_id && *server_gen;
    release_object( c );
    return ret;
}
#endif /* WEX_E1S13_HKU_TEST */

static int e1lab_install_e1s_consumed( obj_handle_t context, const struct e1s_token_consumed *record )
{
    struct e1s_token_context *c;
    int ret = 0;
    if (!e1s_broker()) { set_error( STATUS_ACCESS_DENIED ); return 0; }
    if (!(c = (struct e1s_token_context *)get_handle_obj( current->process, context, 0, &e1s_context_ops ))) return 0;
    if (e1s_context_current( c )) ret = e1s_install_consumed( c, record );
    if (ret) clear_error();
    else set_error( STATUS_ACCESS_DENIED );
    release_object( c );
    return ret;
}

/* the lab body of create_e1s_account_token */
static void e1lab_create_e1s_account_token( const struct create_e1s_account_token_request *req,
                                            struct create_e1s_account_token_reply *reply )
{
    struct e1s_token_context *c;
    unsigned char *out;
    unsigned int status;

    if (!e1s_broker()) { set_error( STATUS_ACCESS_DENIED ); return; }
    if (req->operation == WEX_TOKEN_NEGOTIATE)
    {
        if (req->context || req->caller || get_req_data_size() || get_reply_max_size() ||
            req->caps != WEX_TOKEN_CAPS) { set_error( STATUS_NOT_SUPPORTED ); return; }
        if (!(c = alloc_object( &e1s_context_ops ))) return;
        memset( (char *)c + sizeof(c->obj), 0, sizeof(*c) - sizeof(c->obj) );
        c->owner = current->process; c->thread = current->id;
        c->broker_primary = (struct token *)grab_object( c->owner->token );
        c->broker_gen = c->owner->e1s6_broker_generation;
        c->broker_id = c->owner->e1s6_token_id; c->broker_modified = c->owner->e1s6_modified_id;
        c->caps = req->caps;
        reply->context = alloc_handle_no_access_check( c->owner, c, 0, 0 );
        if (reply->context) reply->caps = c->caps;
        release_object( c );
        return;
    }
    if (!(c = (struct e1s_token_context *)get_handle_obj( current->process, req->context, 0, &e1s_context_ops ))) return;
    set_error( STATUS_ACCESS_DENIED );
    if (!e1s_context_current( c )) goto done;
#ifdef WEX_E1_TEST_HOOKS
    if (req->operation == WEX_TOKEN_TEST_INSTALL)
    {
        if (req->caps == c->caps && !get_reply_max_size() &&
            e1s_install_fixture( c, req->caller, get_req_data(), get_req_data_size() )) clear_error();
        goto done;
    }
#endif
    if (req->operation != WEX_TOKEN_ISSUE) { c->state = 2; goto done; }
    /* Malformed sizing/capability also consumes the installed record. Allocate
     * reply storage before the token handle so no later allocation can leak it. */
    if (req->caller || req->caps != c->caps || get_reply_max_size() != WEX_TOKEN_RESPONSE_SIZE)
    { c->state = 2; goto done; }
    clear_error();
    if (!(out = set_reply_data_size( WEX_TOKEN_RESPONSE_SIZE ))) { c->state = 2; goto done; }
    memset( out, 0, WEX_TOKEN_RESPONSE_SIZE );
    e1s_issue( c, get_req_data(), get_req_data_size(), &reply->token, out );
    status = get_error();
    if (!reply->token && !status) set_error( status = STATUS_INSUFFICIENT_RESOURCES );
    if (status)
    {
        if (reply->token) close_handle( current->process, reply->token );
        reply->token = 0;
        memset( out, 0, WEX_TOKEN_RESPONSE_SIZE );
        wex_token_put32( out, 0x00010005 ); wex_token_put32( out + 4, WEX_TOKEN_FAILURE_SIZE );
        wex_token_put32( out + 32, status );
        current->reply_size = WEX_TOKEN_FAILURE_SIZE;
    }
done:
    release_object( c );
}
