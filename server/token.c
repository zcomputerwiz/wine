/*
 * Tokens
 *
 * Copyright (C) 1998 Alexandre Julliard
 * Copyright (C) 2003 Mike McCormack
 * Copyright (C) 2005 Robert Shearman
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

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "thread.h"
#include "process.h"
#include "request.h"
#include "security.h"
#include "sysrun.h"
#include "accounts.h"
#include "session0.h"
#include "uwp_context.h"
#include "e1s_token.h"
#include "wine/wex_token.h"
#include "wine/wex_profile.h"
#define WEX_CALLER_CONSTANTS_ONLY
#include "wine/wex_caller.h"

#define MAX_SUBAUTH_COUNT 1

const struct luid SeIncreaseQuotaPrivilege        = {  5, 0 };
const struct luid SeAssignPrimaryTokenPrivilege  = {  3, 0 };
const struct luid SeTcbPrivilege                  = {  7, 0 };
const struct luid SeSecurityPrivilege             = {  8, 0 };
const struct luid SeTakeOwnershipPrivilege        = {  9, 0 };
const struct luid SeLoadDriverPrivilege           = { 10, 0 };
const struct luid SeSystemProfilePrivilege        = { 11, 0 };
const struct luid SeSystemtimePrivilege           = { 12, 0 };
const struct luid SeProfileSingleProcessPrivilege = { 13, 0 };
const struct luid SeIncreaseBasePriorityPrivilege = { 14, 0 };
const struct luid SeCreatePagefilePrivilege       = { 15, 0 };
const struct luid SeBackupPrivilege               = { 17, 0 };
const struct luid SeRestorePrivilege              = { 18, 0 };
const struct luid SeShutdownPrivilege             = { 19, 0 };
const struct luid SeDebugPrivilege                = { 20, 0 };
const struct luid SeSystemEnvironmentPrivilege    = { 22, 0 };
const struct luid SeChangeNotifyPrivilege         = { 23, 0 };
const struct luid SeRemoteShutdownPrivilege       = { 24, 0 };
const struct luid SeUndockPrivilege               = { 25, 0 };
const struct luid SeManageVolumePrivilege         = { 28, 0 };
const struct luid SeImpersonatePrivilege          = { 29, 0 };
const struct luid SeCreateGlobalPrivilege         = { 30, 0 };
static const struct luid SeIncreaseWorkingSetPrivilege = { 33, 0 };
static const struct luid SeTimeZonePrivilege       = { 34, 0 };
static const struct luid SeCreateSymbolicLinkPrivilege = { 35, 0 };
static const struct luid SeDelegateSessionUserImpersonatePrivilege = { 36, 0 };
static const struct luid SeCreateTokenPrivilege   = {  2, 0 };
static const struct luid SeLockMemoryPrivilege    = {  4, 0 };
static const struct luid SeCreatePermanentPrivilege = { 16, 0 };
static const struct luid SeAuditPrivilege         = { 21, 0 };

struct sid_attrs
{
    const struct sid *sid;
    unsigned int      attrs;
};

const struct sid world_sid = { SID_REVISION, 1, SECURITY_WORLD_SID_AUTHORITY, { SECURITY_WORLD_RID } };
const struct sid local_system_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_LOCAL_SYSTEM_RID } };
const struct sid local_user_sid = { SID_REVISION, 5, SECURITY_NT_AUTHORITY, { SECURITY_NT_NON_UNIQUE, 0, 0, 0, 1000 } };
const struct sid builtin_admins_sid = { SID_REVISION, 2, SECURITY_NT_AUTHORITY, { SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS } };
const struct sid builtin_users_sid = { SID_REVISION, 2, SECURITY_NT_AUTHORITY, { SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS } };
const struct sid domain_users_sid = { SID_REVISION, 5, SECURITY_NT_AUTHORITY, { SECURITY_NT_NON_UNIQUE, 0, 0, 0, DOMAIN_GROUP_RID_USERS } };

static const struct sid local_sid = { SID_REVISION, 1, SECURITY_LOCAL_SID_AUTHORITY, { SECURITY_LOCAL_RID } };
static const struct sid console_logon_sid = { SID_REVISION, 1, SECURITY_LOCAL_SID_AUTHORITY, { 1 } };
static const struct sid this_organization_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_THIS_ORGANIZATION_RID } };
static const struct sid interactive_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_INTERACTIVE_RID } };
static const struct sid anonymous_logon_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_ANONYMOUS_LOGON_RID } };
static const struct sid authenticated_user_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_AUTHENTICATED_USER_RID } };
static const struct sid high_label_sid = { SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, { SECURITY_MANDATORY_HIGH_RID } };
static const struct sid system_label_sid = { SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, { SECURITY_MANDATORY_SYSTEM_RID } };
static const struct sid service_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_SERVICE_RID } };
static const struct sid local_service_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_LOCAL_SERVICE_RID } };
static const struct sid network_service_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_NETWORK_SERVICE_RID } };
static const struct sid all_app_packages_sid = { SID_REVISION, 2, { 0, 0, 0, 0, 0, 15 }, { 2, 1 } };

static struct luid prev_luid_value = { 1000, 0 };

static const WCHAR token_name[] = {'T','o','k','e','n'};

struct type_descr token_type =
{
    { token_name, sizeof(token_name) },   /* name */
    TOKEN_ALL_ACCESS | SYNCHRONIZE,       /* valid_access */
    {                                     /* mapping */
        STANDARD_RIGHTS_READ | TOKEN_QUERY_SOURCE | TOKEN_QUERY | TOKEN_DUPLICATE,
        STANDARD_RIGHTS_WRITE | TOKEN_ADJUST_SESSIONID | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_GROUPS
        | TOKEN_ADJUST_PRIVILEGES,
        STANDARD_RIGHTS_EXECUTE | TOKEN_IMPERSONATE | TOKEN_ASSIGN_PRIMARY,
        TOKEN_ALL_ACCESS
    },
};

struct token
{
    struct object  obj;             /* object header */
    struct luid    token_id;        /* system-unique id of token */
    struct luid    modified_id;     /* new id allocated every time token is modified */
    struct luid    lineage_id;      /* token_id of the first token of the chain of duplicates and filtered copies */
    struct luid    auth_id;         /* authentication (logon) id, shared by all tokens of the same logon */
    struct wex_account e1s_account; /* S10 provenance; zero generation means unmanaged */
    struct list    privileges;      /* privileges available to the token */
    struct list    groups;          /* groups that the user of this token belongs to (sid_and_attributes) */
    struct sid    *user;            /* SID of user this token represents */
    struct sid    *owner;           /* SID of owner */
    struct sid    *primary_group;   /* SID of user's primary group */
    unsigned int   primary;         /* is this a primary or impersonation token? */
    unsigned int   session_id;      /* token session id */
    struct acl    *default_dacl;    /* the default DACL to assign to objects created by this user */
    int            impersonation_level; /* impersonation level this token is capable of if non-primary token */
    int            elevation;       /* elevation type */
    struct list    kernel_object;   /* list of kernel object pointers */
    unsigned int   integrity_rid;   /* integrity level RID */
    struct list    restricted_sids; /* list of restricting SIDs */
    int            is_restricted;   /* token has restricting SIDs */
    struct sid    *app_container;   /* AppContainer package SID */
    struct list    capabilities;    /* capability SIDs */
    struct uwp_context *package;    /* package context (UWP compatibility) */
    int            package_pending; /* no process image has been checked against the context yet */
    int            windows_sd;      /* R7: Windows' descriptor was given (token_protect_system, token_protect_elevated) */
    int            link_bound;      /* WineExplorer H3: made by link_bound_token from another server's description */
    unsigned int   link_session;    /* ... for that link's session */
    int            session_system;  /* B132: a session's system role, mapped to its account (token_map_service_user):
                                     * a flag, never a SID, so no ACE matches it; WEX_CALLER_SESSION_SYSTEM says it */
};

struct privilege
{
    struct list entry;
    struct luid luid;
    unsigned    enabled  : 1; /* is the privilege currently enabled? */
    unsigned    def      : 1; /* is the privilege enabled by default? */
};

struct group
{
    struct list    entry;
    unsigned int   attrs;
    struct sid     sid;
};

static void token_dump( struct object *obj, int verbose );
static void token_destroy( struct object *obj );
static int token_set_sd( struct object *obj, const struct security_descriptor *sd,
                         unsigned int set_info );
static struct list *token_get_kernel_obj_list( struct object *obj );
static struct security_descriptor *create_security_label_sd( struct token *token, const struct sid *label_sid );
static inline void allocate_luid( struct luid *luid );

static const struct object_ops token_ops =
{
    .size    = sizeof(struct token),
    .type    = &token_type,
    .dump    = token_dump,
    .set_sd  = token_set_sd,
    .get_kernel_obj_list = token_get_kernel_obj_list,
    .destroy = token_destroy,
};

static void token_dump( struct object *obj, int verbose )
{
    struct token *token = (struct token *)obj;
    assert( obj->ops == &token_ops );
    fprintf( stderr, "Token id=%d.%u primary=%u impersonation level=%d\n", token->token_id.high_part,
             token->token_id.low_part, token->primary, token->impersonation_level );
}

static int token_set_sd( struct object *obj, const struct security_descriptor *sd,
                         unsigned int set_info )
{
    int ret = default_set_sd( obj, sd, set_info & ~LABEL_SECURITY_INFORMATION );

    if (ret) allocate_luid( &((struct token *)obj)->modified_id );
    return ret;
}

static struct list *token_get_kernel_obj_list( struct object *obj )
{
    struct token *token = (struct token *)obj;
    return &token->kernel_object;
}

void security_set_thread_token( struct thread *thread, obj_handle_t handle )
{
    if (!handle)
    {
        if (thread->token)
            release_object( thread->token );
        thread->token = NULL;
    }
    else
    {
        struct token *token = (struct token *)get_handle_obj( current->process,
                                                              handle,
                                                              TOKEN_IMPERSONATE,
                                                              &token_ops );
        if (token && thread->process->token)
        {
            /* WineExplorer B99, from Windows' SeTokenCanImpersonate: a token of a higher integrity than the thread's
             * process, also with SeImpersonatePrivilege, and without that privilege an elevated token while the
             * process is not elevated, goes to the thread as an identification-level copy. Measured: on Windows 11 a
             * Low integrity process may open and duplicate a Medium token of its user, but SetThreadToken and
             * ImpersonateLoggedOnUser with it give its thread an identification token; on Windows 10 the same holds
             * for a Low copy of an elevated administrator's token (SeImpersonatePrivilege enabled) and its High
             * parent. The rule's other part (another user's token) is left to the pipe servers
             * (token_impersonation_level): Windows lets a process impersonate a token it got from LogonUser. */
            struct token *process_token = thread->process->token;
            int had = token->primary ? SecurityImpersonation : token->impersonation_level;

            if (had > SecurityIdentification &&
                (token->integrity_rid > process_token->integrity_rid ||
                 (!token_has_privilege( process_token, SeImpersonatePrivilege ) &&
                  token->elevation == TokenElevationTypeFull && process_token->elevation != TokenElevationTypeFull)))
            {
                struct token *copy = token_duplicate( token, FALSE, SecurityIdentification, NULL, NULL, 0, NULL, 0, FALSE );

                release_object( token );
                if (!copy) return;
                token = copy;
            }
        }
        if (token)
        {
            if (thread->token)
                release_object( thread->token );
            thread->token = token;
        }
    }
}

/* The SID of the server's user. Wine gives every user S-1-5-21-0-0-0-1000. With the system tier's session 0,
 * which holds every user's registry, each user needs a SID of their own: a server whose programs use session 0
 * takes its user's SID from the E1 accounts map (docs/SYSTEM_TIER_WORK_ORDERS.md, T4-0); the account bound to the
 * legacy SID keeps S-1-5-21-0-0-0-1000. */
static struct sid mapped_user_sid;
static const struct sid *server_user_sid = &local_user_sid;

/* WineExplorer domain logon WO 2 (docs/DOMAIN_LOGON.md): when the server's user is a domain account of the map,
 * its Domain Users (the domain SID and RID 513, in place of the fixed S-1-5-21-0-0-0-513) and the domain groups
 * the root step recorded at logon; none of it comes from the user's processes. */
#define DOMAIN_GROUPS_MAX 1024
static int server_user_domain;
static struct sid server_domain_users_sid;
static struct sid *server_domain_groups;
static unsigned int server_domain_group_count;

static void make_domain_sid( struct sid *sid, const unsigned int subauth[5], unsigned int rid )
{
    unsigned int i;

    sid->revision = SID_REVISION;
    sid->sub_count = 5;
    memcpy( sid->id_auth, local_user_sid.id_auth, sizeof(sid->id_auth) );
    for (i = 0; i < 4; i++) sid->sub_auth[i] = subauth[i];
    sid->sub_auth[4] = rid;
}

/* the server's user is a domain account in the domain group rid (Domain Admins: 512) */
int security_server_user_in_domain_group( unsigned int rid )
{
    unsigned int i;

    if (!server_user_domain) return 0;
    for (i = 0; i < server_domain_group_count; i++)
        if (server_domain_groups[i].sub_auth[4] == rid) return 1;
    return 0;
}

void security_init_server_user(void)
{
    unsigned int subauth[5], i, count;
    uint32_t *rids;

    if (session0_server)
    {
        /* session 0 is the system: its own HKEY_CURRENT_USER is SYSTEM's, never a user's hive */
        server_user_sid = &local_system_sid;
        return;
    }
    if (!session0_client) return;
    if ((rids = malloc( DOMAIN_GROUPS_MAX * sizeof(*rids) )) &&
        accounts_own_domain( getuid(), subauth, rids, DOMAIN_GROUPS_MAX, &count ))
    {
        server_user_domain = 1;
        make_domain_sid( &server_domain_users_sid, subauth, DOMAIN_GROUP_RID_USERS );
        if (count && (server_domain_groups = malloc( count * sizeof(*server_domain_groups) )))
        {
            for (i = 0; i < count; i++) make_domain_sid( &server_domain_groups[i], subauth, rids[i] );
            server_domain_group_count = count;
        }
    }
    free( rids );
    if (!accounts_own_sid( getuid(), subauth )) return;
    mapped_user_sid.revision = SID_REVISION;
    mapped_user_sid.sub_count = 5;
    memcpy( mapped_user_sid.id_auth, local_user_sid.id_auth, sizeof(mapped_user_sid.id_auth) );
    for (i = 0; i < 5; i++) mapped_user_sid.sub_auth[i] = subauth[i];
    server_user_sid = &mapped_user_sid;
}

const struct sid *security_server_user_sid(void)
{
    return server_user_sid;
}

const struct sid *security_unix_uid_to_sid( uid_t uid )
{
    /* very simple mapping: either the current user or not the current user */
    if (uid == getuid())
        return server_user_sid;
    else
        return &anonymous_logon_sid;
}

static int acl_is_valid( const struct acl *acl, data_size_t size )
{
    ULONG i;
    const struct ace *ace;

    if (size < sizeof(*acl)) return FALSE;

    size = min(size, MAX_ACL_LEN);
    size -= sizeof(*acl);

    for (i = 0, ace = ace_first( acl ); i < acl->count; i++, ace = ace_next( ace ))
    {
        if (size < sizeof(*ace) || size < ace->size) return FALSE;
        size -= ace->size;
        switch (ace->type)
        {
        case ACCESS_DENIED_ACE_TYPE:
        case ACCESS_ALLOWED_ACE_TYPE:
        case SYSTEM_AUDIT_ACE_TYPE:
        case SYSTEM_ALARM_ACE_TYPE:
        case SYSTEM_MANDATORY_LABEL_ACE_TYPE:
            break;
        default:
            return FALSE;
        }
        if (!sid_valid_size( (const struct sid *)(ace + 1), ace->size - sizeof(*ace) )) return FALSE;
    }
    return TRUE;
}

static unsigned int get_sid_count( const struct sid *sid, data_size_t size )
{
    unsigned int count;

    for (count = 0; sid_valid_size( sid, size ); count++)
    {
        size -= sid_len( sid );
        sid = (const struct sid *)((char *)sid + sid_len( sid ));
    }
    return count;
}

/* checks whether all members of a security descriptor fit inside the size
 * of memory specified */
int sd_is_valid( const struct security_descriptor *sd, data_size_t size )
{
    size_t offset = sizeof(struct security_descriptor);
    const struct sid *group;
    const struct sid *owner;
    const struct acl *sacl;
    const struct acl *dacl;
    int dummy;

    if (size < offset)
        return FALSE;

    if (sd->owner_len >= offsetof(struct sid, sub_auth[255]) || offset + sd->owner_len > size) return FALSE;
    owner = sd_get_owner( sd );
    offset += sd->owner_len;
    if (owner && !sid_valid_size( owner, sd->owner_len )) return FALSE;

    if (sd->group_len >= offsetof(struct sid, sub_auth[255]) || offset + sd->group_len > size) return FALSE;
    group = sd_get_group( sd );
    offset += sd->group_len;
    if (group && !sid_valid_size( group, sd->group_len )) return FALSE;

    if ((sd->sacl_len >= MAX_ACL_LEN) || (offset + sd->sacl_len > size))
        return FALSE;
    sacl = sd_get_sacl( sd, &dummy );
    if (sacl && !acl_is_valid( sacl, sd->sacl_len ))
        return FALSE;
    offset += sd->sacl_len;

    if ((sd->dacl_len >= MAX_ACL_LEN) || (offset + sd->dacl_len > size))
        return FALSE;
    dacl = sd_get_dacl( sd, &dummy );
    if (dacl && !acl_is_valid( dacl, sd->dacl_len ))
        return FALSE;

    return TRUE;
}

/* extract security labels from SACL */
struct acl *extract_security_labels( const struct acl *sacl )
{
    size_t size = sizeof(*sacl);
    const struct ace *ace;
    struct ace *label_ace;
    unsigned int i, count = 0;
    struct acl *label_acl;

    for (i = 0, ace = ace_first( sacl ); i < sacl->count; i++, ace = ace_next( ace ))
    {
        if (ace->type == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
        {
            size += ace->size;
            count++;
        }
    }

    label_acl = mem_alloc( size );
    if (!label_acl) return NULL;

    label_acl->revision = sacl->revision;
    label_acl->pad1     = 0;
    label_acl->size     = size;
    label_acl->count    = count;
    label_acl->pad2     = 0;

    label_ace = ace_first( label_acl );
    for (i = 0, ace = ace_first( sacl ); i < sacl->count; i++, ace = ace_next( ace ))
        if (ace->type == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
            label_ace = mem_append( label_ace, ace, ace->size );

    return label_acl;
}

/* replace security labels in an existing SACL */
struct acl *replace_security_labels( const struct acl *old_sacl, const struct acl *new_sacl )
{
    const struct ace *ace;
    struct ace *replaced_ace;
    unsigned int i, count = 0;
    unsigned char revision = ACL_REVISION;
    struct acl *replaced_acl;
    data_size_t size = sizeof(*replaced_acl);

    if (old_sacl)
    {
        revision = max( revision, old_sacl->revision );
        for (i = 0, ace = ace_first( old_sacl ); i < old_sacl->count; i++, ace = ace_next( ace ))
        {
            if (ace->type == SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
            size += ace->size;
            count++;
        }
    }

    if (new_sacl)
    {
        revision = max( revision, new_sacl->revision );
        for (i = 0, ace = ace_first( new_sacl ); i < new_sacl->count; i++, ace = ace_next( ace ))
        {
            if (ace->type != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
            size += ace->size;
            count++;
        }
    }
    if (size > MAX_ACL_LEN)
    {
        set_error( STATUS_INVALID_ACL );
        return NULL;
    }

    replaced_acl = mem_alloc( size );
    if (!replaced_acl) return NULL;

    replaced_acl->revision = revision;
    replaced_acl->pad1     = 0;
    replaced_acl->size     = size;
    replaced_acl->count    = count;
    replaced_acl->pad2     = 0;

    replaced_ace = (struct ace *)(replaced_acl + 1);
    if (old_sacl)
    {
        for (i = 0, ace = ace_first( old_sacl ); i < old_sacl->count; i++, ace = ace_next( ace ))
            if (ace->type != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                replaced_ace = mem_append( replaced_ace, ace, ace->size );
    }

    if (new_sacl)
    {
        for (i = 0, ace = ace_first( new_sacl ); i < new_sacl->count; i++, ace = ace_next( ace ))
            if (ace->type == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                replaced_ace = mem_append( replaced_ace, ace, ace->size );
    }

    return replaced_acl;
}

static inline int is_equal_luid( struct luid luid1, struct luid luid2 )
{
    return (luid1.low_part == luid2.low_part && luid1.high_part == luid2.high_part);
}

static inline void allocate_luid( struct luid *luid )
{
    if (!++prev_luid_value.low_part) ++prev_luid_value.high_part;
    *luid = prev_luid_value;
}

/* allocate a new authentication (logon) id from the same allocator as token ids */
static void alloc_logon_luid( struct luid *luid )
{
    allocate_luid( luid );
}

DECL_HANDLER( allocate_locally_unique_id )
{
    allocate_luid( &reply->luid );
}

static inline struct luid_attr luid_and_attr_from_privilege( const struct privilege *in )
{
    struct luid_attr ret = { in->luid };

    ret.attrs = (in->enabled ? SE_PRIVILEGE_ENABLED : 0) |
                (in->def ? SE_PRIVILEGE_ENABLED_BY_DEFAULT : 0);
    return ret;
}

static struct privilege *privilege_add( struct token *token, struct luid luid, int enabled )
{
    struct privilege *privilege = mem_alloc( sizeof(*privilege) );
    if (privilege)
    {
        privilege->luid = luid;
        privilege->def = privilege->enabled = (enabled != 0);
        list_add_tail( &token->privileges, &privilege->entry );
    }
    return privilege;
}

static inline void privilege_remove( struct privilege *privilege )
{
    list_remove( &privilege->entry );
    free( privilege );
}

static void token_destroy( struct object *obj )
{
    struct token* token;
    struct list *cursor, *cursor_next;

    assert( obj->ops == &token_ops );
    token = (struct token *)obj;

    free( token->user );
    free( token->owner );
    free( token->primary_group );
    free( token->app_container );

    LIST_FOR_EACH_SAFE( cursor, cursor_next, &token->privileges )
    {
        struct privilege *privilege = LIST_ENTRY( cursor, struct privilege, entry );
        privilege_remove( privilege );
    }

    LIST_FOR_EACH_SAFE( cursor, cursor_next, &token->groups )
    {
        struct group *group = LIST_ENTRY( cursor, struct group, entry );
        list_remove( &group->entry );
        free( group );
    }

    LIST_FOR_EACH_SAFE( cursor, cursor_next, &token->restricted_sids )
    {
        struct group *group = LIST_ENTRY( cursor, struct group, entry );
        list_remove( &group->entry );
        free( group );
    }

    LIST_FOR_EACH_SAFE( cursor, cursor_next, &token->capabilities )
    {
        struct group *group = LIST_ENTRY( cursor, struct group, entry );
        list_remove( &group->entry );
        free( group );
    }

    free( token->default_dacl );
    if (token->package) uwp_context_release( token->package );
}

/* creates a new token.
 *  groups may be NULL if group_count is 0.
 *  privs may be NULL if priv_count is 0.
 *  default_dacl may be NULL, indicating that all objects created by the user
 *   are unsecured.
 *  modified_id may be NULL, indicating that a new modified_id luid should be
 *   allocated.
 */
static struct token *create_token( unsigned int primary, unsigned int session_id, const struct sid *user,
                                   const struct sid *owner, const struct sid *primary_group,
                                   const struct sid_attrs *groups, unsigned int group_count,
                                   const struct luid_attr *privs, unsigned int priv_count,
                                   const struct acl *default_dacl, const struct luid *modified_id,
                                   int impersonation_level, int elevation )
{
    struct token *token = alloc_object( &token_ops );
    if (token)
    {
        unsigned int i;

        allocate_luid( &token->token_id );
        token->lineage_id = token->token_id;
        if (modified_id)
            token->modified_id = *modified_id;
        else
            allocate_luid( &token->modified_id );
        /* the caller sets the authentication (logon) id; zero is the unassigned stub */
        token->auth_id.low_part = 0;
        token->auth_id.high_part = 0;
        memset( &token->e1s_account, 0, sizeof(token->e1s_account) );
        list_init( &token->privileges );
        list_init( &token->groups );
        list_init( &token->restricted_sids );
        list_init( &token->capabilities );
        list_init( &token->kernel_object );
        token->primary = primary;
        token->session_id = session_id;
        /* primary tokens don't have impersonation levels */
        if (primary)
            token->impersonation_level = -1;
        else
            token->impersonation_level = impersonation_level;
        token->default_dacl = NULL;
        token->elevation = elevation;
        token->integrity_rid = SECURITY_MANDATORY_HIGH_RID;
        token->app_container = NULL;
        token->is_restricted = 0;
        token->package = NULL;
        token->package_pending = 0;
        token->windows_sd = 0;
        token->link_bound = 0;    /* H3: a token of this server's own (link_bound_token sets it) */
        token->link_session = 0;
        token->session_system = 0;

        /* copy user */
        token->user = memdup( user, sid_len( user ));
        if (!token->user)
        {
            release_object( token );
            return NULL;
        }

        /* copy owner */
        token->owner = memdup( owner, sid_len( owner ));
        if (!token->owner)
        {
            release_object( token );
            return NULL;
        }

        /* copy primary group */
        token->primary_group = memdup( primary_group, sid_len( primary_group ));
        if (!token->primary_group)
        {
            release_object( token );
            return NULL;
        }

        /* copy groups */
        for (i = 0; i < group_count; i++)
        {
            size_t size = offsetof( struct group, sid.sub_auth[groups[i].sid->sub_count] );
            struct group *group = mem_alloc( size );

            if (!group)
            {
                release_object( token );
                return NULL;
            }
            group->attrs = groups[i].attrs;
            copy_sid( &group->sid, groups[i].sid );
            list_add_tail( &token->groups, &group->entry );
        }

        /* copy privileges */
        for (i = 0; i < priv_count; i++)
        {
            /* note: we don't check uniqueness: the caller must make sure
             * privs doesn't contain any duplicate luids */
            if (!privilege_add( token, privs[i].luid, privs[i].attrs & SE_PRIVILEGE_ENABLED ))
            {
                release_object( token );
                return NULL;
            }
        }

        if (default_dacl)
        {
            token->default_dacl = memdup( default_dacl, default_dacl->size );
            if (!token->default_dacl)
            {
                release_object( token );
                return NULL;
            }
        }
    }
    return token;
}

static int filter_group( struct group *group, const struct sid *filter, unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        if (equal_sid( &group->sid, filter )) return 1;
        filter = (const struct sid *)((char *)filter + sid_len( filter ));
    }

    return 0;
}

static int filter_privilege( struct privilege *privilege, const struct luid_attr *filter, unsigned int count )
{
    unsigned int i;

    for (i = 0; i < count; i++)
        if (is_equal_luid( privilege->luid, filter[i].luid )) return 1;

    return 0;
}

struct token *token_duplicate( struct token *src_token, unsigned primary,
                               int impersonation_level, const struct security_descriptor *sd,
                               const struct luid_attr *remove_privs, unsigned int remove_priv_count,
                               const struct sid *remove_groups, unsigned int remove_group_count,
                               unsigned int from_public_api )
{
    const struct luid *modified_id =
        primary || (impersonation_level == src_token->impersonation_level) ?
            &src_token->modified_id : NULL;
    struct token *token = NULL;
    struct privilege *privilege;
    struct group *group;

    /* Windows will not turn an identification-level token into a primary token. Stock Wine relies on it
     * (shell32 runas, msi custom actions and the ntdll loader start processes from the linked token), so this
     * only applies while an elevation helper is configured. */
    if (primary && !src_token->primary && src_token->impersonation_level < SecurityImpersonation &&
        elevation_enforced())
    {
        set_error( STATUS_BAD_IMPERSONATION_LEVEL );
        return NULL;
    }

    if (!primary &&
        (impersonation_level < SecurityAnonymous ||
         impersonation_level > SecurityDelegation ||
         (!src_token->primary && (impersonation_level > src_token->impersonation_level))))
    {
        set_error( STATUS_BAD_IMPERSONATION_LEVEL );
        return NULL;
    }

    token = create_token( primary, src_token->session_id, src_token->user, src_token->owner,
                          src_token->primary_group, NULL, 0, NULL, 0, src_token->default_dacl,
                          modified_id, impersonation_level, src_token->elevation );
    if (!token) return token;
    token->lineage_id = src_token->lineage_id;
    token->auth_id = src_token->auth_id;
    token->e1s_account = src_token->e1s_account;
    token->link_bound = src_token->link_bound;  /* a copy of a session's token stays a session's (H3) */
    token->link_session = src_token->link_session;
    token->session_system = src_token->session_system;

    /* copy groups */
    LIST_FOR_EACH_ENTRY( group, &src_token->groups, struct group, entry )
    {
        size_t size = offsetof( struct group, sid.sub_auth[group->sid.sub_count] );
        struct group *newgroup = mem_alloc( size );
        if (!newgroup)
        {
            release_object( token );
            return NULL;
        }
        memcpy( newgroup, group, size );
        if (filter_group( group, remove_groups, remove_group_count ))
        {
            newgroup->attrs &= ~(SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT);
            newgroup->attrs |= SE_GROUP_USE_FOR_DENY_ONLY;
        }
        list_add_tail( &token->groups, &newgroup->entry );
    }

    /* copy privileges */
    LIST_FOR_EACH_ENTRY( privilege, &src_token->privileges, struct privilege, entry )
    {
        if (filter_privilege( privilege, remove_privs, remove_priv_count )) continue;
        if (!privilege_add( token, privilege->luid, privilege->enabled ))
        {
            release_object( token );
            return NULL;
        }
    }

    token->integrity_rid = src_token->integrity_rid;
    if (src_token->app_container)
    {
        token->app_container = memdup( src_token->app_container, sid_len( src_token->app_container ) );
        if (!token->app_container)
        {
            release_object( token );
            return NULL;
        }
    }

    /* copy restricted sids */
    LIST_FOR_EACH_ENTRY( group, &src_token->restricted_sids, struct group, entry )
    {
        size_t size = offsetof( struct group, sid.sub_auth[group->sid.sub_count] );
        struct group *newgroup = mem_alloc( size );
        if (!newgroup)
        {
            release_object( token );
            return NULL;
        }
        memcpy( newgroup, group, size );
        list_add_tail( &token->restricted_sids, &newgroup->entry );
    }
    token->is_restricted = src_token->is_restricted;

    /* the package context follows every duplicate, checked against an image or not */
    if (src_token->package)
    {
        token->package = uwp_context_retain( src_token->package );
        token->package_pending = src_token->package_pending;
    }

    /* copy capabilities */
    LIST_FOR_EACH_ENTRY( group, &src_token->capabilities, struct group, entry )
    {
        size_t size = offsetof( struct group, sid.sub_auth[group->sid.sub_count] );
        struct group *newgroup = mem_alloc( size );
        if (!newgroup)
        {
            release_object( token );
            return NULL;
        }
        memcpy( newgroup, group, size );
        list_add_tail( &token->capabilities, &newgroup->entry );
    }

    if (sd && !default_set_sd( &token->obj, sd, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                              DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION ))
    {
        release_object( token );
        return NULL;
    }

    if (from_public_api)
    {
        /* Public duplication uses creator object defaults, but derives an omitted label from
         * source token integrity state rather than the source object's security descriptor. */
        int has_label = FALSE;

        if (sd)
        {
            const struct acl *sacl;
            int present;

            sacl = sd_get_sacl( sd, &present );
            if (present && sacl)
            {
                const struct ace *ace;
                unsigned int i;
                for (i = 0, ace = ace_first( sacl ); i < sacl->count; i++, ace = ace_next( ace ))
                    if (ace->type == SYSTEM_MANDATORY_LABEL_ACE_TYPE) { has_label = TRUE; break; }
            }
        }

        if (!has_label)
        {
            struct sid label_sid = { SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, { token->integrity_rid } };
            struct security_descriptor *label_sd = create_security_label_sd( token, &label_sid );

            if (!label_sd)
            {
                release_object( token );
                return NULL;
            }
            if (!default_set_sd( &token->obj, label_sd, LABEL_SECURITY_INFORMATION ))
            {
                free( label_sd );
                release_object( token );
                return NULL;
            }
            free( label_sd );
        }
    }
    else if (src_token->obj.sd)
    {
        const struct acl *sacl;
        const struct ace *ace;
        unsigned int i;
        int present;

        sacl = sd_get_sacl( src_token->obj.sd, &present );
        if (present)
        {
            for (i = 0, ace = ace_first( sacl ); i < sacl->count; i++, ace = ace_next( ace ))
            {
                if (ace->type != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
                token_assign_label( token, (const struct sid *)(ace + 1) );
            }
        }
    }

    /* Copying an unchanged security context into a new object preserves its version. */
    if (modified_id) token->modified_id = *modified_id;
    return token;
}

static struct acl *create_default_dacl( const struct sid *user )
{
    struct ace *ace;
    struct acl *default_dacl;
    size_t default_dacl_size = sizeof(*default_dacl) + 2 * sizeof(*ace) +
                               sid_len( &local_system_sid ) + sid_len( user );

    default_dacl = mem_alloc( default_dacl_size );
    if (!default_dacl) return NULL;

    default_dacl->revision = ACL_REVISION;
    default_dacl->pad1     = 0;
    default_dacl->size     = default_dacl_size;
    default_dacl->count    = 2;
    default_dacl->pad2     = 0;

    /* GENERIC_ALL for Local System */
    ace = set_ace( ace_first( default_dacl ), &local_system_sid, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL );
    /* GENERIC_ALL for specified user */
    set_ace( ace_next( ace ), user, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL );

    return default_dacl;
}

struct sid_data
{
    SID_IDENTIFIER_AUTHORITY idauth;
    int count;
    unsigned int subauth[MAX_SUBAUTH_COUNT];
};

static struct security_descriptor *create_security_label_sd( struct token *token, const struct sid *label_sid )
{
    size_t sid_size = sid_len( label_sid ), sacl_size, sd_size;
    struct security_descriptor *sd;
    struct acl *sacl;

    sacl_size = sizeof(*sacl) + sizeof(struct ace) + sid_size;
    sd_size = sizeof(struct security_descriptor) + sacl_size;
    if (!(sd = mem_alloc( sd_size )))
        return NULL;

    sd->control   = SE_SACL_PRESENT;
    sd->owner_len = 0;
    sd->group_len = 0;
    sd->sacl_len  = sacl_size;
    sd->dacl_len  = 0;

    sacl = (struct acl *)(sd + 1);
    sacl->revision = ACL_REVISION;
    sacl->pad1     = 0;
    sacl->size     = sacl_size;
    sacl->count    = 1;
    sacl->pad2     = 0;

    set_ace( ace_first( sacl ), label_sid, SYSTEM_MANDATORY_LABEL_ACE_TYPE, 0,
             SYSTEM_MANDATORY_LABEL_NO_WRITE_UP );
    assert( sd_is_valid( sd, sd_size ) );
    return sd;
}

int token_assign_label( struct token *token, const struct sid *label )
{
    struct security_descriptor *sd;
    int ret = 0;

    if ((sd = create_security_label_sd( token, label )))
    {
        ret = set_sd_defaults_from_token( &token->obj, sd, LABEL_SECURITY_INFORMATION, token );
        if (ret)
        {
            if (label->sub_count > 0) token->integrity_rid = label->sub_auth[label->sub_count - 1];
            allocate_luid( &token->modified_id );
        }
        free( sd );
    }

    return ret;
}

struct token *get_token_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    return (struct token *)get_handle_obj( process, handle, access, &token_ops );
}

struct token *token_create_admin( unsigned primary, int impersonation_level, int elevation, unsigned int session_id )
{
    struct token *token = NULL;
    struct sid alias_admins_sid = { SID_REVISION, 2, SECURITY_NT_AUTHORITY, { SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS }};
    struct sid alias_users_sid = { SID_REVISION, 2, SECURITY_NT_AUTHORITY, { SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS }};
    /* on Windows, this value changes every time the user logs on */
    struct sid logon_sid = { SID_REVISION, 3, SECURITY_NT_AUTHORITY, { SECURITY_LOGON_IDS_RID, 0, 0 /* FIXME: should be randomly generated when tokens are inherited by new processes */ }};
    const struct sid *user_sid = security_unix_uid_to_sid( getuid() );
    struct acl *default_dacl = create_default_dacl( user_sid );
    const struct luid_attr admin_privs[] =
    {
        { SeChangeNotifyPrivilege, SE_PRIVILEGE_ENABLED },
        { SeTcbPrivilege, 0 },
        { SeSecurityPrivilege, 0 },
        { SeBackupPrivilege, 0 },
        { SeRestorePrivilege, 0 },
        { SeSystemtimePrivilege, 0 },
        { SeShutdownPrivilege, 0 },
        { SeRemoteShutdownPrivilege, 0 },
        { SeTakeOwnershipPrivilege, 0 },
        { SeDebugPrivilege, 0 },
        { SeSystemEnvironmentPrivilege, 0 },
        { SeSystemProfilePrivilege, 0 },
        { SeProfileSingleProcessPrivilege, 0 },
        { SeIncreaseBasePriorityPrivilege, 0 },
        { SeLoadDriverPrivilege, SE_PRIVILEGE_ENABLED },
        { SeCreatePagefilePrivilege, 0 },
        { SeIncreaseQuotaPrivilege, 0 },
        { SeUndockPrivilege, 0 },
        { SeManageVolumePrivilege, 0 },
        { SeImpersonatePrivilege, SE_PRIVILEGE_ENABLED },
        { SeCreateGlobalPrivilege, SE_PRIVILEGE_ENABLED },
    };
    /* note: we don't include non-builtin groups here for the user -
     * telling us these is the job of a client-side program */
    const struct sid_attrs admin_groups[] =
    {
        { &world_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY },
        { &local_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY },
        { &interactive_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY },
        { &authenticated_user_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY },
        { &domain_users_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY|SE_GROUP_OWNER },
        { &alias_admins_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY|SE_GROUP_OWNER },
        { &alias_users_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY },
        { &logon_sid, SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY|SE_GROUP_LOGON_ID },
    };

    if (server_user_domain && user_sid == server_user_sid)
    {
        /* a domain account (WO 2): the domain's Domain Users is owner and primary group, and the recorded domain
         * groups are enabled groups of the token; everything else is the list above (Administrators included,
         * which the logon filters to deny-only or removes as for any account, process.c) */
        struct sid_attrs *groups;
        unsigned int i, n = 0;

        if (!(groups = malloc( (ARRAY_SIZE( admin_groups ) + server_domain_group_count) * sizeof(*groups) )))
        {
            free( default_dacl );
            return NULL;
        }
        for (i = 0; i < ARRAY_SIZE( admin_groups ); i++)
        {
            groups[n] = admin_groups[i];
            if (admin_groups[i].sid == &domain_users_sid) groups[n].sid = &server_domain_users_sid;
            n++;
        }
        for (i = 0; i < server_domain_group_count; i++)
        {
            if (equal_sid( &server_domain_groups[i], &server_domain_users_sid )) continue;
            groups[n].sid = &server_domain_groups[i];
            groups[n].attrs = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
            n++;
        }
        token = create_token( primary, session_id, user_sid, &server_domain_users_sid, &server_domain_users_sid,
                              groups, n, admin_privs, ARRAY_SIZE( admin_privs ),
                              default_dacl, NULL, impersonation_level, elevation );
        free( groups );
    }
    else
        token = create_token( primary, session_id, user_sid, &domain_users_sid, &domain_users_sid,
                              admin_groups, ARRAY_SIZE( admin_groups ), admin_privs, ARRAY_SIZE( admin_privs ),
                              default_dacl, NULL, impersonation_level, elevation );

    /* Assign a high security label to the token. The default would be medium
     * but Wine provides admin access to all applications right now so high
     * makes more sense for the time being. */
    if (!token_assign_label( token, &high_label_sid ))
    {
        release_object( token );
        return NULL;
    }

    free( default_dacl );
    return token;
}

/* create the first interactive user token of the server; it gets a new logon (authentication) id */
struct token *token_create_first_interactive( unsigned int session_id )
{
    struct token *token = token_create_admin( TRUE, -1, TokenElevationTypeLimited, session_id );
    if (token)
        alloc_logon_luid( &token->auth_id );
    return token;
}

/* copy the authentication (logon) id from src to dst */
void token_copy_auth_id( struct token *dst, const struct token *src )
{
    dst->auth_id = src->auth_id;
    allocate_luid( &dst->modified_id );
}

/* Tokens of the system role (docs/ELEVATION_SYSTEM_ROLE_WORKORDER.md, C3): LocalSystem, LocalService and
 * NetworkService, all primary, Default elevation, no linked token, System integrity.
 * Privileges and groups follow the native tokens measured in docs/ELEVATION_LOCAL_VALIDATION_WORKORDER.md
 * (Task Scheduler tokens). Left out on purpose: the per-task SIDs and the unknown S-1-5-32-* SIDs. A service's own SID
 * (NT SERVICE\name) is added by token_create_service_sid when services.exe passes it (B114). */
struct token *token_create_service( unsigned int account, unsigned int session_id )
{
    return token_create_service_sid( account, session_id, NULL );
}

/* a service SID, S-1-5-80-x-x-x-x-x (SECURITY_SERVICE_ID_BASE_RID and the SHA-1 of the upper case name) */
static int is_service_id_sid( const struct sid *sid )
{
    static const unsigned char nt_authority[6] = SECURITY_NT_AUTHORITY;

    return sid->revision == SID_REVISION && sid->sub_count == SECURITY_SERVICE_ID_RID_COUNT &&
           !memcmp( sid->id_auth, nt_authority, sizeof(nt_authority) ) &&
           sid->sub_auth[0] == SECURITY_SERVICE_ID_BASE_RID;
}

/* the token of a service account; own_sid, when not NULL, is the service's own SID, added as an enabled group
 * that may own objects, as in Windows' tokens of services whose SID type is unrestricted */
struct token *token_create_service_sid( unsigned int account, unsigned int session_id, const struct sid *own_sid )
{
    const unsigned int on = SE_PRIVILEGE_ENABLED;
    const unsigned int grp = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
    const struct luid_attr system_privs[] =
    {
        { SeLockMemoryPrivilege, on }, { SeTcbPrivilege, on }, { SeSystemProfilePrivilege, on },
        { SeProfileSingleProcessPrivilege, on }, { SeIncreaseBasePriorityPrivilege, on },
        { SeCreatePagefilePrivilege, on }, { SeCreatePermanentPrivilege, on }, { SeDebugPrivilege, on },
        { SeAuditPrivilege, on }, { SeChangeNotifyPrivilege, on }, { SeImpersonatePrivilege, on },
        { SeCreateGlobalPrivilege, on }, { SeIncreaseWorkingSetPrivilege, on }, { SeTimeZonePrivilege, on },
        { SeCreateSymbolicLinkPrivilege, on }, { SeDelegateSessionUserImpersonatePrivilege, on },
        { SeAssignPrimaryTokenPrivilege, 0 }, { SeIncreaseQuotaPrivilege, 0 }, { SeSecurityPrivilege, 0 },
        { SeTakeOwnershipPrivilege, 0 }, { SeLoadDriverPrivilege, 0 }, { SeSystemtimePrivilege, 0 },
        { SeBackupPrivilege, 0 }, { SeRestorePrivilege, 0 }, { SeShutdownPrivilege, 0 },
        { SeSystemEnvironmentPrivilege, 0 }, { SeUndockPrivilege, 0 }, { SeManageVolumePrivilege, 0 },
    };
    /* SeImpersonate stays enabled although the measured Task Scheduler tokens lack it: Microsoft documents it as a
     * default privilege of Local Service, Network Service and Local System services */
    const struct luid_attr local_service_privs[] =
    {
        { SeChangeNotifyPrivilege, on }, { SeCreateGlobalPrivilege, on }, { SeImpersonatePrivilege, on },
        { SeAssignPrimaryTokenPrivilege, 0 }, { SeSystemtimePrivilege, 0 }, { SeShutdownPrivilege, 0 },
        { SeAuditPrivilege, 0 }, { SeUndockPrivilege, 0 }, { SeIncreaseWorkingSetPrivilege, 0 },
        { SeTimeZonePrivilege, 0 },
    };
    const struct luid_attr network_service_privs[] =
    {
        { SeChangeNotifyPrivilege, on }, { SeCreateGlobalPrivilege, on }, { SeImpersonatePrivilege, on },
        { SeAssignPrimaryTokenPrivilege, 0 }, { SeIncreaseQuotaPrivilege, 0 }, { SeShutdownPrivilege, 0 },
        { SeAuditPrivilege, 0 }, { SeUndockPrivilege, 0 }, { SeIncreaseWorkingSetPrivilege, 0 },
        { SeTimeZonePrivilege, 0 },
    };
    /* CONSOLE LOGON may be an artifact of the Task Scheduler measurement: provisional */
    const struct sid_attrs service_groups[] =
    {
        { &world_sid, grp }, { &builtin_users_sid, grp }, { &service_sid, grp }, { &console_logon_sid, grp },
        { &authenticated_user_sid, grp }, { &this_organization_sid, grp }, { &local_sid, grp },
        { &builtin_admins_sid, SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_OWNER },
    };
    const struct sid *user;
    const struct sid *owner;
    const struct sid *primary_group = &builtin_users_sid;
    const struct sid_attrs *groups = service_groups;
    const struct luid_attr *privs = local_service_privs;
    unsigned int group_count = ARRAY_SIZE( service_groups ) - 1;  /* no Administrators */
    unsigned int priv_count = ARRAY_SIZE( local_service_privs );
    struct token *token;
    struct acl *default_dacl;

    switch (account)
    {
    case SERVICE_ACCOUNT_SYSTEM:
        user = &local_system_sid;
        owner = &builtin_admins_sid;
        primary_group = &builtin_admins_sid;
        group_count = ARRAY_SIZE( service_groups );
        privs = system_privs;
        priv_count = ARRAY_SIZE( system_privs );
        break;
    case SERVICE_ACCOUNT_LOCAL_SERVICE:
        user = &local_service_sid;
        owner = user;
        break;
    case SERVICE_ACCOUNT_NETWORK_SERVICE:
        user = &network_service_sid;
        owner = user;
        privs = network_service_privs;
        priv_count = ARRAY_SIZE( network_service_privs );
        break;
    default:
        return NULL;
    }
    if (own_sid && !is_service_id_sid( own_sid )) return NULL;

    /* the default DACL of the account: SYSTEM and Administrators for LocalSystem, SYSTEM and the account for the others */
    if (!(default_dacl = create_default_dacl( account == SERVICE_ACCOUNT_SYSTEM ? &builtin_admins_sid : user )))
        return NULL;
    if (own_sid)
    {
        struct sid_attrs with_sid[ARRAY_SIZE( service_groups ) + 1];

        memcpy( with_sid, groups, group_count * sizeof(*groups) );
        with_sid[group_count].sid = own_sid;
        with_sid[group_count].attrs = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_OWNER;
        token = create_token( 1, session_id, user, owner, primary_group, with_sid, group_count + 1, privs, priv_count,
                              default_dacl, NULL, -1, TokenElevationTypeDefault );
    }
    else
        token = create_token( 1, session_id, user, owner, primary_group, groups, group_count, privs, priv_count,
                              default_dacl, NULL, -1, TokenElevationTypeDefault );
    free( default_dacl );
    if (!token) return NULL;
    /* service (SYSTEM) logons use a fixed authentication id */
    token->auth_id.low_part = 0x3e7;
    token->auth_id.high_part = 0;
    if (!token_assign_label( token, &system_label_sid ))
    {
        release_object( token );
        return NULL;
    }
    return token;
}

/* which system role account the user of a token is (SERVICE_ACCOUNT_*), or -1 */
int token_service_account( const struct token *token )
{
    if (equal_sid( token->user, &local_system_sid )) return SERVICE_ACCOUNT_SYSTEM;
    if (equal_sid( token->user, &local_service_sid )) return SERVICE_ACCOUNT_LOCAL_SERVICE;
    if (equal_sid( token->user, &network_service_sid )) return SERVICE_ACCOUNT_NETWORK_SERVICE;
    return -1;
}

/* The token of an elevated process is only for SYSTEM and Administrators (enabled in the full token only, deny-only
 * in the user's limited token): the user's limited processes must not duplicate, impersonate or assign it. */
int token_protect_elevated( struct token *token )
{
    size_t acl_size = sizeof(struct acl) + 2 * sizeof(struct ace) + sid_len( &local_system_sid ) + sid_len( &builtin_admins_sid );
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
    acl->count = 2;
    ace = set_ace( ace_first( acl ), &local_system_sid, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL );
    set_ace( ace_next( ace ), &builtin_admins_sid, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL );
    ret = set_sd_defaults_from_token( &token->obj, sd, DACL_SECURITY_INFORMATION, token );
    free( sd );
    if (ret) token->windows_sd = 1;
    return ret;
}

/* WineExplorer (R7 of docs/SESSION0_SPLIT_REVIEW.md): the descriptor Windows gives the token of a system process,
 * measured black box on Windows 10 (2026-11-24: OpenProcessToken for each right from an elevated administrator, the
 * descriptors read as SYSTEM). The mandatory label is kept. Users get nothing in either kind.
 * - service: a service's token (services.exe asks for one per service, create_service_token). Owner and group the
 *   account; SYSTEM all; the account all, standing in for the service's own SID (S-1-5-80-...), which Windows grants
 *   all and Wine's tokens do not have; Administrators TOKEN_QUERY only.
 * - otherwise, a system process that is not a service (services.exe, wineboot, session 0's own programs; on Windows
 *   wininit, services, lsass, winlogon). Owner Administrators, group SYSTEM; SYSTEM all; Administrators assign,
 *   duplicate, impersonate, query, query source and read control. */
int token_protect_system( struct token *token, int service )
{
    const unsigned int admin_mask = service ? TOKEN_QUERY : TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE |
                                                            TOKEN_QUERY | TOKEN_QUERY_SOURCE | READ_CONTROL;
    const struct sid *owner = service ? token->user : &builtin_admins_sid;
    const struct sid *group = service ? token->user : &local_system_sid;
    const int account_ace = service && !equal_sid( token->user, &local_system_sid );
    size_t acl_size = sizeof(struct acl) + (account_ace ? 3 : 2) * sizeof(struct ace) + sid_len( &local_system_sid ) +
                      (account_ace ? sid_len( token->user ) : 0) + sid_len( &builtin_admins_sid );
    size_t size = sizeof(struct security_descriptor) + sid_len( owner ) + sid_len( group ) + acl_size;
    struct security_descriptor *sd;
    struct acl *acl;
    struct ace *ace;
    char *ptr;
    int ret;

    if (!(sd = mem_alloc( size ))) return 0;
    memset( sd, 0, size );
    sd->control = SE_DACL_PRESENT;
    sd->owner_len = sid_len( owner );
    sd->group_len = sid_len( group );
    sd->dacl_len = acl_size;
    ptr = (char *)(sd + 1);
    memcpy( ptr, owner, sd->owner_len );
    ptr += sd->owner_len;
    memcpy( ptr, group, sd->group_len );
    ptr += sd->group_len;
    acl = (struct acl *)ptr;
    acl->revision = ACL_REVISION;
    acl->size = acl_size;
    acl->count = account_ace ? 3 : 2;
    ace = set_ace( ace_first( acl ), &local_system_sid, ACCESS_ALLOWED_ACE_TYPE, 0, TOKEN_ALL_ACCESS );
    if (account_ace) ace = set_ace( ace_next( ace ), token->user, ACCESS_ALLOWED_ACE_TYPE, 0, TOKEN_ALL_ACCESS );
    set_ace( ace_next( ace ), &builtin_admins_sid, ACCESS_ALLOWED_ACE_TYPE, 0, admin_mask );
    ret = set_sd_defaults_from_token( &token->obj, sd, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                      DACL_SECURITY_INFORMATION, token );
    free( sd );
    if (ret) token->windows_sd = 1;
    return ret;
}

/* a process's new primary token (dst, a duplicate of src, which copied src's label) gets src's owner, group and DACL
 * when src has Windows' descriptor (token_protect_system, token_protect_elevated): a service's process keeps its
 * service descriptor. Any other token's descriptor (most have the default DACL their label came with) is not copied. */
int token_keep_descriptor( struct token *dst, const struct token *src )
{
    if (!src->windows_sd || !src->obj.sd) return 1;
    if (!set_sd_defaults_from_token( &dst->obj, src->obj.sd, OWNER_SECURITY_INFORMATION |
                                     GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, NULL )) return 0;
    dst->windows_sd = 1;
    return 1;
}

/* a token of a process that was elevated: the full half of a split administrator token */
int token_is_elevated( const struct token *token )
{
    return token->primary && token->elevation == TokenElevationTypeFull;
}

/* create the primary token of a service account; only the system role may (SeTcbPrivilege, enabled) */
DECL_HANDLER(create_service_token)
{
    struct token *caller = current->process->token, *token;

    /* WineExplorer T5.4: session 0 runs services too; only session 0's own account has programs there */
    if (!session0_server && (!sysrun_active || !elevation_enforced()))
    {
        set_error( STATUS_NOT_SUPPORTED );
        return;
    }
    if (!token_has_privilege( caller, SeTcbPrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }
    /* Wine's administrator token also holds SeTcbPrivilege: require the system role itself (in session 0, a
     * process whose token is SYSTEM's: only session 0's own account runs programs there, and its registry-only
     * clients cannot make this request) */
    if (session0_server ? token_service_account( caller ) != SERVICE_ACCOUNT_SYSTEM
                        : !sysrun_process_is_tcb_system( current->process ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    if (req->account > SERVICE_ACCOUNT_NETWORK_SERVICE)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    /* B114: the service's own SID, computed by services.exe (trusted: the caller is the system role) */
    if (get_req_data_size() && (!sid_valid_size( get_req_data(), get_req_data_size() ) ||
                                !is_service_id_sid( get_req_data() )))
    {
        set_error( STATUS_INVALID_SID );
        return;
    }
    if ((token = token_create_service_sid( req->account, caller->session_id,
                                           get_req_data_size() ? get_req_data() : NULL )))
    {
        if (token_protect_system( token, 1 ))
            reply->token = alloc_handle_no_access_check( current->process, token, TOKEN_ALL_ACCESS, 0 );
        release_object( token );
    }
}

static struct privilege *token_find_privilege( struct token *token, struct luid luid, int enabled_only )
{
    struct privilege *privilege;
    LIST_FOR_EACH_ENTRY( privilege, &token->privileges, struct privilege, entry )
    {
        if (is_equal_luid( luid, privilege->luid ))
        {
            if (enabled_only && !privilege->enabled)
                return NULL;
            return privilege;
        }
    }
    return NULL;
}

static unsigned int token_adjust_privileges( struct token *token, const struct luid_attr *privs,
                                             unsigned int count, struct luid_attr *mod_privs,
                                             unsigned int mod_privs_count )
{
    unsigned int i, modified_count = 0;

    /* mark as modified */
    allocate_luid( &token->modified_id );

    for (i = 0; i < count; i++)
    {
        struct privilege *privilege = token_find_privilege( token, privs[i].luid, FALSE );
        if (!privilege)
        {
            set_error( STATUS_NOT_ALL_ASSIGNED );
            continue;
        }

        if (privs[i].attrs & SE_PRIVILEGE_REMOVED) privilege_remove( privilege );
        else
        {
            /* save previous state for caller */
            if (mod_privs_count)
            {
                *mod_privs++ = luid_and_attr_from_privilege( privilege );
                mod_privs_count--;
                modified_count++;
            }
            privilege->enabled = !!(privs[i].attrs & SE_PRIVILEGE_ENABLED);
        }
    }
    return modified_count;
}

static void token_disable_privileges( struct token *token )
{
    struct privilege *privilege;

    /* mark as modified */
    allocate_luid( &token->modified_id );

    LIST_FOR_EACH_ENTRY( privilege, &token->privileges, struct privilege, entry )
        privilege->enabled = FALSE;
}

/* Windows only honours a privilege that is enabled in the primary token. */
int token_has_privilege( struct token *token, struct luid priv )
{
    return token_find_privilege( token, priv, TRUE ) != NULL;
}

/* Can the caller give a process this token? Only enforced while an elevation helper is configured.
 * A token that is not at least an impersonation-level token is refused. A copy of the caller's own
 * token (same lineage: a duplicate or a filtered version) is allowed when it gives no more than the caller has:
 * not a higher integrity, not elevated while the caller is not (B99 route 3: a Low process's token is a copy of its
 * Medium parent's, so the parent's token has its lineage; measured on Windows 11, a Low process's CreateProcessAsUser
 * with a primary copy of its Medium parent's token fails with ERROR_PRIVILEGE_NOT_HELD, one of its own token works).
 * Anything else needs SeAssignPrimaryTokenPrivilege, which Wine's service tokens do not have yet, so SeTcbPrivilege
 * (held by a full administrator token and by the planned elevation broker) stands in for it.
 * shortcut: SeTcbPrivilege stands in for SeAssignPrimaryTokenPrivilege, drop it once the broker has its own token. */
unsigned int token_check_assign_primary( struct token *token )
{
    struct token *caller = current->process->token;

    if (!elevation_enforced()) return STATUS_SUCCESS;
    if (!token->primary && token->impersonation_level < SecurityImpersonation)
        return STATUS_BAD_IMPERSONATION_LEVEL;
    if (is_equal_luid( token->lineage_id, caller->lineage_id ) && token->integrity_rid <= caller->integrity_rid &&
        !(token->elevation == TokenElevationTypeFull && caller->elevation != TokenElevationTypeFull))
        return STATUS_SUCCESS;
    if (token_has_privilege( caller, SeAssignPrimaryTokenPrivilege ) || token_has_privilege( caller, SeTcbPrivilege ))
        return STATUS_SUCCESS;
    return STATUS_PRIVILEGE_NOT_HELD;
}

/* Impersonation level a pipe server gets for a client token (Windows SeTokenCanImpersonate).
 * A client of higher integrity than the server gives identification, also with SeImpersonatePrivilege (B99: measured
 * on Windows 10 for SetThreadToken, which uses the same check). Without SeImpersonatePrivilege in the server's primary
 * token the level drops to identification too, unless the client has the server's user and is not elevated while
 * the server is not.
 * shortcut: the client's requested SQOS level is not sent to the server, so it is the client token's own level
 * (impersonation by default), upgrade when create_file carries SecurityQualityOfService. */
int token_impersonation_level( struct token *client, struct token *server )
{
    int level = client->primary ? SecurityImpersonation : client->impersonation_level;

    if (level > SecurityImpersonation) level = SecurityImpersonation;
    if (client->integrity_rid > server->integrity_rid) return level < SecurityIdentification ? level : SecurityIdentification;
    if (token_has_privilege( server, SeImpersonatePrivilege )) return level;
    if (equal_sid( client->user, server->user ) && client->integrity_rid <= server->integrity_rid &&
        !(client->elevation == TokenElevationTypeFull && server->elevation != TokenElevationTypeFull))
        return level;
    return level < SecurityIdentification ? level : SecurityIdentification;
}

int token_check_privileges( struct token *token, int all_required, const struct luid_attr *reqprivs,
                            unsigned int count, struct luid_attr *usedprivs)
{
    unsigned int i, enabled_count = 0;

    for (i = 0; i < count; i++)
    {
        struct privilege *privilege = token_find_privilege( token, reqprivs[i].luid, TRUE );

        if (usedprivs)
            usedprivs[i] = reqprivs[i];

        if (privilege && privilege->enabled)
        {
            enabled_count++;
            if (usedprivs) usedprivs[i].attrs |= SE_PRIVILEGE_USED_FOR_ACCESS;
        }
    }

    if (all_required)
        return (enabled_count == count);
    else
        return (enabled_count > 0);
}

int token_sid_present( struct token *token, const struct sid *sid, int deny )
{
    struct group *group;

    if (equal_sid( token->user, sid )) return TRUE;

    LIST_FOR_EACH_ENTRY( group, &token->groups, struct group, entry )
    {
        /* Disabled deny-only groups must still participate in deny ACEs, but
         * must never grant access, even if ENABLED is also present. */
        if (group->attrs & SE_GROUP_USE_FOR_DENY_ONLY)
        {
            if (!deny) continue;
        }
        else if (!(group->attrs & SE_GROUP_ENABLED)) continue;
        if (equal_sid( &group->sid, sid )) return TRUE;
    }

    if (token->app_container)
    {
        if (equal_sid( token->app_container, sid )) return TRUE;
        if (equal_sid( &all_app_packages_sid, sid )) return TRUE;
    }

    LIST_FOR_EACH_ENTRY( group, &token->capabilities, struct group, entry )
    {
        if (!(group->attrs & SE_GROUP_ENABLED)) continue;
        if (equal_sid( &group->sid, sid )) return TRUE;
    }

    return FALSE;
}

/* Checks access to a security descriptor. 'sd' must have been validated by
 * caller. It returns STATUS_SUCCESS if call succeeded or an error indicating
 * the reason. 'status' parameter will indicate if access is granted or denied.
 *
 * If both returned value and 'status' are STATUS_SUCCESS then access is granted.
 */
static unsigned int token_access_check( struct token *token,
                                 const struct security_descriptor *sd,
                                 unsigned int desired_access,
                                 struct luid_attr *privs,
                                 unsigned int *priv_count,
                                 const struct generic_map *mapping,
                                 unsigned int *granted_access,
                                 unsigned int *status )
{
    unsigned int current_access = 0;
    unsigned int denied_access = 0;
    ULONG i;
    const struct acl *dacl;
    int dacl_present;
    const struct ace *ace;
    const struct sid *owner;

    /* assume no access rights */
    *granted_access = 0;

    /* fail if desired_access contains generic rights */
    if (desired_access & (GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE|GENERIC_ALL))
    {
        if (priv_count) *priv_count = 0;
        return STATUS_GENERIC_NOT_MAPPED;
    }

    dacl = sd_get_dacl( sd, &dacl_present );
    owner = sd_get_owner( sd );
    if (!owner || !sd_get_group( sd ))
    {
        if (priv_count) *priv_count = 0;
        return STATUS_INVALID_SECURITY_DESCR;
    }

    /* Mandatory integrity check: check if object has a mandatory label in SACL */
    {
        const struct acl *sacl;
        int sacl_present;
        sacl = sd_get_sacl( sd, &sacl_present );
        if (sacl_present && sacl)
        {
            const struct ace *m_ace;
            const struct sid *m_sid;
            unsigned int m_idx;
            for (m_idx = 0, m_ace = ace_first( sacl ); m_idx < sacl->count; m_idx++, m_ace = ace_next( m_ace ))
            {
                if (m_ace->type != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
                m_sid = (const struct sid *)(m_ace + 1);
                if (m_sid->sub_count > 0)
                {
                    unsigned int obj_integrity = m_sid->sub_auth[m_sid->sub_count - 1];
                    if (token->integrity_rid < obj_integrity)
                    {
                        unsigned int policy = m_ace->mask;
                        if (!policy || (policy & 1 /* SYSTEM_MANDATORY_LABEL_NO_WRITE_UP */))
                            denied_access |= (mapping->write & ~(READ_CONTROL | SYNCHRONIZE)) | WRITE_DAC | WRITE_OWNER | DELETE;
                        if (policy & 2 /* SYSTEM_MANDATORY_LABEL_NO_READ_UP */)
                            denied_access |= mapping->read & ~SYNCHRONIZE;
                        if (policy & 4 /* SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP */)
                            denied_access |= mapping->exec & ~(READ_CONTROL | SYNCHRONIZE);
                    }
                }
            }
        }
    }

    if (!(desired_access & MAXIMUM_ALLOWED) && (desired_access & denied_access))
    {
        if (priv_count) *priv_count = 0;
        *granted_access = 0;
        *status = STATUS_ACCESS_DENIED;
        return STATUS_SUCCESS;
    }

    /* 1: Grant desired access if the object is unprotected */
    if (!dacl_present || !dacl)
    {
        if (priv_count) *priv_count = 0;
        if (desired_access & MAXIMUM_ALLOWED)
            *granted_access = mapping->all & ~denied_access;
        else
            *granted_access = desired_access & ~denied_access;
        return *status = *granted_access == desired_access || (desired_access & MAXIMUM_ALLOWED) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    }

    /* 2: Check if caller wants access to system security part. Note: access
     * is only granted if specifically asked for */
    if (desired_access & ACCESS_SYSTEM_SECURITY)
    {
        const struct luid_attr security_priv = { SeSecurityPrivilege, 0 };
        struct luid_attr retpriv = security_priv;
        if (token_check_privileges( token, TRUE, &security_priv, 1, &retpriv ))
        {
            if (priv_count)
            {
                /* assumes that there will only be one privilege to return */
                if (*priv_count >= 1)
                {
                    *priv_count = 1;
                    *privs = retpriv;
                }
                else
                {
                    *priv_count = 1;
                    return STATUS_BUFFER_TOO_SMALL;
                }
            }
            current_access |= ACCESS_SYSTEM_SECURITY;
            if (desired_access == current_access)
            {
                *granted_access = current_access;
                return *status = STATUS_SUCCESS;
            }
        }
        else
        {
            if (priv_count) *priv_count = 0;
            *status = STATUS_PRIVILEGE_NOT_HELD;
            return STATUS_SUCCESS;
        }
    }
    else if (priv_count) *priv_count = 0;

    /* 3: Check whether the token is the owner */
    /* NOTE: SeTakeOwnershipPrivilege is not checked for here - it is instead
     * checked when a "set owner" call is made, overriding the access rights
     * determined here. */
    if (token_sid_present( token, owner, FALSE ))
    {
        current_access |= (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE);
        if (desired_access == current_access)
        {
            *granted_access = current_access;
            return *status = STATUS_SUCCESS;
        }
    }

    /* 4: Grant rights according to the DACL */
    for (i = 0, ace = ace_first( dacl ); i < dacl->count; i++, ace = ace_next( ace ))
    {
        const struct sid *sid = (const struct sid *)(ace + 1);

        if (ace->flags & INHERIT_ONLY_ACE) continue;

        switch (ace->type)
        {
        case ACCESS_DENIED_ACE_TYPE:
            if (token_sid_present( token, sid, TRUE ))
            {
                unsigned int access = map_access( ace->mask, mapping );
                if (desired_access & MAXIMUM_ALLOWED)
                    denied_access |= access;
                else
                {
                    denied_access |= (access & ~current_access);
                    if (desired_access & access) goto done;
                }
            }
            break;
        case ACCESS_ALLOWED_ACE_TYPE:
            if (token_sid_present( token, sid, FALSE ))
            {
                unsigned int access = map_access( ace->mask, mapping );
                if (desired_access & MAXIMUM_ALLOWED)
                    current_access |= access;
                else
                    current_access |= (access & ~denied_access);
            }
            break;
        }

        /* don't bother carrying on checking if we've already got all of
            * rights we need */
        if (desired_access == *granted_access)
            break;
    }

    if (token->is_restricted || !list_empty( &token->restricted_sids ))
    {
        unsigned int restricted_allowed = 0;
        unsigned int restricted_denied = 0;

        for (i = 0, ace = ace_first( dacl ); i < dacl->count; i++, ace = ace_next( ace ))
        {
            const struct sid *sid = (const struct sid *)(ace + 1);
            struct group *rg;
            int match = 0;

            if (ace->flags & INHERIT_ONLY_ACE) continue;

            LIST_FOR_EACH_ENTRY( rg, &token->restricted_sids, struct group, entry )
            {
                if (equal_sid( &rg->sid, sid ))
                {
                    match = 1;
                    break;
                }
            }
            if (!match) continue;

            switch (ace->type)
            {
            case ACCESS_DENIED_ACE_TYPE:
            {
                unsigned int access = map_access( ace->mask, mapping );
                if (desired_access & MAXIMUM_ALLOWED)
                    restricted_denied |= access;
                else
                {
                    restricted_denied |= (access & ~restricted_allowed);
                    if (desired_access & access)
                    {
                        *status = STATUS_ACCESS_DENIED;
                        *granted_access = 0;
                        return STATUS_SUCCESS;
                    }
                }
                break;
            }
            case ACCESS_ALLOWED_ACE_TYPE:
            {
                unsigned int access = map_access( ace->mask, mapping );
                if (desired_access & MAXIMUM_ALLOWED)
                    restricted_allowed |= access;
                else
                    restricted_allowed |= (access & ~restricted_denied);
                break;
            }
            }
        }

        if (desired_access & MAXIMUM_ALLOWED)
            current_access &= (restricted_allowed & ~restricted_denied);
        else
        {
            if ((restricted_allowed & desired_access) != desired_access)
            {
                *status = STATUS_ACCESS_DENIED;
                *granted_access = 0;
                return STATUS_SUCCESS;
            }
            current_access &= restricted_allowed;
        }
    }

done:
    if (desired_access & MAXIMUM_ALLOWED)
        *granted_access = current_access & ~denied_access;
    else
        if ((current_access & desired_access) == desired_access)
            *granted_access = current_access & desired_access;
        else
            *granted_access = 0;

    *status = *granted_access ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    return STATUS_SUCCESS;
}

const struct acl *token_get_default_dacl( struct token *token )
{
    return token->default_dacl;
}

const struct sid *token_get_owner( struct token *token )
{
    return token->owner;
}

const struct sid *token_get_primary_group( struct token *token )
{
    return token->primary_group;
}

unsigned int token_get_session_id( struct token *token )
{
    return token->session_id;
}

/* WineExplorer (B120): the identity of the process that manages a desktop (creates its top window) is kept with the
 * desktop. When that process died, only a process of the same user and session, with an integrity level at least as
 * high and a token that is neither an AppContainer's nor restricted, may take the desktop window over. */
struct sid *token_desktop_manager_user( struct token *token )
{
    return token ? memdup( token->user, sid_len( token->user )) : NULL;
}

unsigned int token_desktop_manager_il( struct token *token )
{
    return token->integrity_rid;
}

int token_may_take_desktop( struct token *token, const struct sid *user, unsigned int session, unsigned int il )
{
    if (!token || !user) return 0;
    if (token->app_container || token->is_restricted) return 0;
    return equal_sid( token->user, user ) && token->session_id == session && token->integrity_rid >= il;
}

void token_get_ids( const struct token *token, struct luid *token_id, struct luid *modified_id )
{
    *token_id = token->token_id;
    *modified_id = token->modified_id;
}

/* TEMPORARY (B132 measurement, the reviewer's item 3; removed before the batch): an access check that refuses a
 * session's system role in session 0 (a mapped, link-bound token), with the object and the access asked */
static void b132diag_refused( struct token *token, struct object *obj, unsigned int desired )
{
    if (!session0_server || !token->link_bound || !token->session_system) return;
    fprintf( stderr, "B132diag: session %u's system role refused access %08x on ", token->link_session, desired );
    obj->ops->dump( obj, 0 );
}

int check_object_access(struct token *token, struct object *obj, unsigned int *access)
{
    struct generic_map mapping;
    struct security_descriptor *sd;
    unsigned int status, desired = *access;
    int res;

    if (!token)
        token = current->token ? current->token : current->process->token;

    /* Identification-level impersonation is for reading the client's identity only. Windows fails every
     * object open with it, a registry key with access denied. */
    if (!token->primary && token->impersonation_level < SecurityImpersonation && elevation_enforced())
    {
        set_error( object_is_registry_key( obj ) ? STATUS_ACCESS_DENIED : STATUS_BAD_IMPERSONATION_LEVEL );
        return FALSE;
    }

    mapping.all = map_obj_access( obj, GENERIC_ALL );

    /* as on Windows, an enabled SeDebugPrivilege opens any process or thread, whatever its descriptor says; the
     * descriptors of the system role's processes would otherwise shut out an administrator */
    if (sysrun_active && (obj->ops->type == &process_type || obj->ops->type == &thread_type) &&
        token_has_privilege( token, SeDebugPrivilege ))
    {
        if (*access & MAXIMUM_ALLOWED) *access = mapping.all;
        return TRUE;
    }

    /* a key without a descriptor is checked against its default one while elevation is enforced */
    sd = obj->sd;
    if (!sd && elevation_enforced() && object_is_registry_key( obj )) sd = obj->ops->get_sd( obj );
    if (!sd)
    {
        if (*access & MAXIMUM_ALLOWED) *access = mapping.all;
        return TRUE;
    }

    mapping.read  = map_obj_access( obj, GENERIC_READ );
    mapping.write = map_obj_access( obj, GENERIC_WRITE );
    mapping.exec  = map_obj_access( obj, GENERIC_EXECUTE );

    /* Mandatory integrity check: check if object has a mandatory label */
    {
        const struct acl *sacl;
        int sacl_present;
        sacl = sd_get_sacl( sd, &sacl_present );
        if (sacl_present && sacl)
        {
            const struct ace *ace;
            const struct sid *sid;
            unsigned int i;
            for (i = 0, ace = ace_first( sacl ); i < sacl->count; i++, ace = ace_next( ace ))
            {
                if (ace->type != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
                sid = (const struct sid *)(ace + 1);
                if (sid->sub_count > 0)
                {
                    unsigned int obj_integrity = sid->sub_auth[sid->sub_count - 1];
                    if (token->integrity_rid < obj_integrity)
                    {
                        unsigned int write_mask = (mapping.write & ~(READ_CONTROL | SYNCHRONIZE)) | WRITE_DAC | WRITE_OWNER | DELETE;
                        if (*access & write_mask)
                        {
                            b132diag_refused( token, obj, desired );
                            set_error( STATUS_ACCESS_DENIED );
                            return FALSE;
                        }
                    }
                }
            }
        }
    }

    res = token_access_check( token, sd, *access, NULL, NULL,
                              &mapping, access, &status ) == STATUS_SUCCESS &&
          status == STATUS_SUCCESS;

    if (!res)
    {
        b132diag_refused( token, obj, desired );
        set_error( STATUS_ACCESS_DENIED );
    }
    return res;
}


/* create a security token */
DECL_HANDLER(create_token)
{
    struct token *token;
    struct object_params params;
    struct sid *user;
    struct sid *owner;
    struct sid *primary_group;
    struct sid_attrs *groups;
    struct luid_attr *privs;
    struct acl *dacl = NULL;
    unsigned int i;
    data_size_t data_size, groups_size;
    struct acl *default_dacl = NULL;
    unsigned int *attrs;
    struct sid *sid;

    /* native NtCreateToken needs SeCreateTokenPrivilege, which no Wine token has; without this check a limited
     * process could make itself an administrator token and impersonate it, past every access check */
    if (elevation_enforced() && !token_has_privilege( current->process->token, SeCreateTokenPrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if (!get_req_object_attributes( &params )) return;
    if (params.root) release_object( params.root );  /* unused */

    user = (struct sid *)get_req_data_after_objattr( &params, &data_size );

    if (!user || !sid_valid_size( user, data_size ))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    data_size -= sid_len( user );

    owner = (struct sid *)((char *)user + sid_len( user ));
    if (!sid_valid_size( owner, data_size ))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    data_size -= sid_len( owner );

    primary_group = (struct sid *)((char *)owner + sid_len( owner ));
    if (!sid_valid_size( primary_group, data_size ))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    data_size -= sid_len( primary_group );

    groups_size = req->group_count * sizeof( attrs[0] );

    if (data_size < groups_size)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    groups = mem_alloc( req->group_count * sizeof( groups[0] ) );
    if (!groups) return;

    attrs = (unsigned int *)((char *)primary_group + sid_len( primary_group ));
    sid = (struct sid *)&attrs[req->group_count];

    for (i = 0; i < req->group_count; i++)
    {
        groups[i].attrs = attrs[i];
        groups[i].sid = sid;

        if (!sid_valid_size( sid, data_size - groups_size ))
        {
            free( groups );
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }

        groups_size += sid_len( sid );
        sid = (struct sid *)((char *)sid + sid_len( sid ));
    }

    data_size -= groups_size;

    if (data_size < req->priv_count * sizeof( privs[0] ))
    {
        free( groups );
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    privs = (struct luid_attr *)((char *)attrs + groups_size);
    data_size -= req->priv_count * sizeof( privs[0] );

    if (data_size)
    {
        dacl = (struct acl *)((char *)privs + req->priv_count * sizeof(privs[0]));
        if (!acl_is_valid( dacl, data_size ))
        {
            free( groups );
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    }
    else
        dacl = default_dacl = create_default_dacl( user );

    token = create_token( req->primary, default_session_id, user, owner, primary_group, groups,
                          req->group_count, privs, req->priv_count, dacl, NULL,
                          req->impersonation_level, TokenElevationTypeFull );
    if (token)
    {
        /* honor the authentication id supplied by the privileged caller */
        token->auth_id = req->auth_id;
        reply->token = alloc_handle( current->process, token, req->access, params.attr );
        release_object( token );
    }
    free( default_dacl );
    free( groups );
}


/* does an open of a token ask only to read it? (MAXIMUM_ALLOWED and the generic rights that include more do not) */
static int token_access_is_query( unsigned int access )
{
    if (access & MAXIMUM_ALLOWED) return 0;
    access = map_access( access, &token_type.mapping );
    return !(access & ~(TOKEN_QUERY | TOKEN_QUERY_SOURCE | READ_CONTROL));
}

/* open a security token */
DECL_HANDLER(open_token)
{
    /* Windows needs only the limited query right on the process or thread (OpenProcessToken from Low integrity works
     * there), and the token's own descriptor decides the rest. With elevation enforced, so does this server for a
     * token that has Windows' descriptor (R7): the system role's (token_protect_system) and elevated primary tokens
     * (token_protect_elevated), marked windows_sd. Any other token, also one with a DACL (most have the default DACL
     * that came with their label, which grants their user everything, and an elevated client's impersonation copy
     * has one too), keeps the stricter rule: the limited right opens it only for reading; duplicating, impersonating,
     * assigning or adjusting needs the full query right. */
    unsigned int query = sysrun_active && elevation_enforced();
    unsigned int read_only = token_access_is_query( req->access );

    if (req->flags & OPEN_TOKEN_THREAD)
    {
        struct thread *thread = get_thread_from_handle( req->handle, query ? THREAD_QUERY_LIMITED_INFORMATION : 0 );

        if (thread && query && !read_only && thread->token && !thread->token->windows_sd)
        {
            release_object( thread );
            thread = get_thread_from_handle( req->handle, THREAD_QUERY_INFORMATION );
        }
        if (thread)
        {
            if (thread->token)
            {
                if (!thread->token->primary && thread->token->impersonation_level <= SecurityAnonymous)
                    set_error( STATUS_CANT_OPEN_ANONYMOUS );
                else
                {
                    /* OpenAsSelf: the access check uses the process token, which matters when the caller is
                     * impersonating at identification level (a pipe server reading its client's identity) */
                    struct token *target = thread->token, *impersonated = current->token;
                    if (req->flags & OPEN_TOKEN_AS_SELF) current->token = NULL;
                    reply->token = alloc_handle( current->process, target, req->access, req->attributes );
                    current->token = impersonated;
                }
            }
            else
                set_error( STATUS_NO_TOKEN );
            release_object( thread );
        }
    }
    else
    {
        struct process *process = get_process_from_handle( req->handle, query ? PROCESS_QUERY_LIMITED_INFORMATION : 0 );

        if (process && query && !read_only && process->token && !process->token->windows_sd)
        {
            release_object( process );
            process = get_process_from_handle( req->handle, PROCESS_QUERY_INFORMATION );
        }
        if (process)
        {
            if (process->token)
                reply->token = alloc_handle( current->process, process->token, req->access,
                                             req->attributes );
            else
                set_error( STATUS_NO_TOKEN );
            release_object( process );
        }
    }
}

/* adjust the privileges held by a token */
DECL_HANDLER(adjust_token_privileges)
{
    struct token *token;
    unsigned int access = TOKEN_ADJUST_PRIVILEGES;

    if (req->get_modified_state) access |= TOKEN_QUERY;

    if ((token = (struct token *)get_handle_obj( current->process, req->handle,
                                                 access, &token_ops )))
    {
        const struct luid_attr *privs = get_req_data();
        struct luid_attr *modified_privs = NULL;
        unsigned int priv_count = get_req_data_size() / sizeof(*privs);
        unsigned int modified_priv_count = 0;

        if (req->get_modified_state && !req->disable_all)
        {
            unsigned int i;
            /* count modified privs */
            for (i = 0; i < priv_count; i++)
            {
                struct privilege *privilege = token_find_privilege( token, privs[i].luid, FALSE );
                if (privilege && req->get_modified_state)
                    modified_priv_count++;
            }
            reply->len = modified_priv_count;
            modified_priv_count = min( modified_priv_count, get_reply_max_size() / sizeof(*modified_privs) );
            if (modified_priv_count)
                modified_privs = set_reply_data_size( modified_priv_count * sizeof(*modified_privs) );
        }
        reply->len = modified_priv_count * sizeof(*modified_privs);

        if (req->disable_all)
            token_disable_privileges( token );
        else
            token_adjust_privileges( token, privs, priv_count, modified_privs, modified_priv_count );

        release_object( token );
    }
}

/* retrieves the list of privileges that may be held be the token */
DECL_HANDLER(get_token_privileges)
{
    struct token *token;

    if ((token = (struct token *)get_handle_obj( current->process, req->handle,
                                                 TOKEN_QUERY,
                                                 &token_ops )))
    {
        int priv_count = 0;
        struct luid_attr *privs;
        struct privilege *privilege;

        LIST_FOR_EACH_ENTRY( privilege, &token->privileges, struct privilege, entry )
            priv_count++;

        reply->len = priv_count * sizeof(*privs);
        if (reply->len <= get_reply_max_size())
        {
            privs = set_reply_data_size( priv_count * sizeof(*privs) );
            if (privs)
                LIST_FOR_EACH_ENTRY( privilege, &token->privileges, struct privilege, entry )
                    *privs++ = luid_and_attr_from_privilege( privilege );
        }
        else
            set_error(STATUS_BUFFER_TOO_SMALL);

        release_object( token );
    }
}

/* creates a duplicate of the token */
DECL_HANDLER(duplicate_token)
{
    struct token *src_token;
    struct object_params params;

    if (!get_req_object_attributes( &params )) return;

    if ((src_token = (struct token *)get_handle_obj( current->process, req->handle,
                                                     TOKEN_DUPLICATE,
                                                     &token_ops )))
    {
        struct token *token = token_duplicate( src_token, req->primary, req->impersonation_level, params.sd, NULL, 0, NULL, 0, TRUE );
        if (token)
        {
            unsigned int access = req->access ? req->access : get_handle_access( current->process, req->handle );
            reply->new_handle = alloc_handle_no_access_check( current->process, token, access, params.attr );
            release_object( token );
        }
        release_object( src_token );
    }
    if (params.root) release_object( params.root );
}

/* the groups that a UAC filtered token keeps for deny only: Administrators, the privileged domain groups
 * (S-1-5-21-a-b-c-RID) and the privileged builtin aliases */
static int sid_is_uac_filtered( const struct sid *sid )
{
    static const unsigned int domain_rids[] = { 512, 516, 517, 518, 519, 520, 521, 526, 527 };
    static const unsigned int alias_rids[] = { 544, 548, 549, 550, 551, 556, 569 };
    unsigned int i;

    if (memcmp( sid->id_auth, local_user_sid.id_auth, sizeof(sid->id_auth) )) return 0;
    if (sid->sub_count == 5 && sid->sub_auth[0] == SECURITY_NT_NON_UNIQUE)
    {
        for (i = 0; i < ARRAY_SIZE(domain_rids); i++) if (sid->sub_auth[4] == domain_rids[i]) return 1;
    }
    else if (sid->sub_count == 2 && sid->sub_auth[0] == SECURITY_BUILTIN_DOMAIN_RID)
    {
        for (i = 0; i < ARRAY_SIZE(alias_rids); i++) if (sid->sub_auth[1] == alias_rids[i]) return 1;
    }
    return 0;
}

/* native filter_token: DISABLE_MAX_PRIVILEGE keeps only SeChangeNotify; LUA_TOKEN: Administrators and the other
 * UAC filtered groups deny-only, Limited, Medium label */
static void token_apply_filter( struct token *token, unsigned int flags )
{
    if (flags & (DISABLE_MAX_PRIVILEGE | LUA_TOKEN)) allocate_luid( &token->modified_id );
    if (flags & DISABLE_MAX_PRIVILEGE)
    {
        struct list *cursor, *cursor_next;
        LIST_FOR_EACH_SAFE( cursor, cursor_next, &token->privileges )
        {
            struct privilege *privilege = LIST_ENTRY( cursor, struct privilege, entry );
            if (!is_equal_luid( privilege->luid, SeChangeNotifyPrivilege ))
                privilege_remove( privilege );
        }
    }

    if (flags & LUA_TOKEN)
    {
        struct group *group;
        token->elevation = TokenElevationTypeLimited;
        LIST_FOR_EACH_ENTRY( group, &token->groups, struct group, entry )
        {
            if (sid_is_uac_filtered( &group->sid ))
            {
                group->attrs &= ~(SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT);
                group->attrs |= SE_GROUP_USE_FOR_DENY_ONLY;
            }
        }
        if (token->integrity_rid > SECURITY_MANDATORY_MEDIUM_RID)
        {
            struct sid med_label = { SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, { SECURITY_MANDATORY_MEDIUM_RID } };
            token_assign_label( token, &med_label );
        }
    }
}

/* native LSA filtered (UAC) token: only SeChangeNotify (enabled), SeShutdown, SeUndock,
 * SeIncreaseWorkingSet and SeTimeZone (disabled) */
void token_make_filtered( struct token *token )
{
    struct list *cursor, *cursor_next;

    token_apply_filter( token, LUA_TOKEN );
    LIST_FOR_EACH_SAFE( cursor, cursor_next, &token->privileges )
    {
        struct privilege *privilege = LIST_ENTRY( cursor, struct privilege, entry );
        if (is_equal_luid( privilege->luid, SeChangeNotifyPrivilege )) continue;
        if (is_equal_luid( privilege->luid, SeShutdownPrivilege ) ||
            is_equal_luid( privilege->luid, SeUndockPrivilege ))
            privilege->def = privilege->enabled = FALSE;
        else
            privilege_remove( privilege );
    }
    privilege_add( token, SeIncreaseWorkingSetPrivilege, FALSE );
    privilege_add( token, SeTimeZonePrivilege, FALSE );
    allocate_luid( &token->modified_id );
}

/* an administrator's token that is not split, as in Safe Mode where User Account Control does not run: all its
 * groups and privileges stay, and it is the full (elevated) one, with the filtered half as its linked token, as the
 * full token the broker hands out (docs/SAFE_MODE_ELEVATED_DESKTOP.md) */
void token_make_full( struct token *token )
{
    token->elevation = TokenElevationTypeFull;
    token_assign_label( token, &high_label_sid );
    allocate_luid( &token->modified_id );
}

/* set when this server gave its user a standard token: tokens derived from it (a filtered copy is Limited
 * again) must not get a linked full token or an administrator grant */
int standard_user_session;

/* a standard user (not in the administrator group): the filtered token without Administrators and without a
 * split, so TokenElevationType is Default as for a non-split token on Windows; the other UAC filtered groups stay
 * for deny only */
void token_make_standard( struct token *token )
{
    struct group *group, *next;

    standard_user_session = 1;

    LIST_FOR_EACH_ENTRY_SAFE( group, next, &token->groups, struct group, entry )
    {
        if (equal_sid( &group->sid, &builtin_admins_sid ))
        {
            list_remove( &group->entry );
            free( group );
        }
        else if (sid_is_uac_filtered( &group->sid ))
        {
            group->attrs &= ~(SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT);
            group->attrs |= SE_GROUP_USE_FOR_DENY_ONLY;
        }
    }
    token->elevation = TokenElevationTypeDefault;
    allocate_luid( &token->modified_id );
}

/* creates a restricted version of a token */
DECL_HANDLER(filter_token)
{
    struct token *src_token;

    if ((src_token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_DUPLICATE, &token_ops )))
    {
        const struct luid_attr *filter_privileges = get_req_data();
        unsigned int priv_count, group_count, restrict_count;
        const struct sid *filter_groups;
        const struct sid *restrict_groups;
        data_size_t total_size = get_req_data_size();
        data_size_t priv_size = min( req->privileges_size, total_size );
        data_size_t disable_size = min( req->disable_sids_size, total_size - priv_size );
        data_size_t restrict_size = total_size - priv_size - disable_size;
        struct token *token;

        priv_count = priv_size / sizeof(struct luid_attr);
        filter_groups = (const struct sid *)((char *)filter_privileges + priv_size);
        group_count = get_sid_count( filter_groups, disable_size );
        restrict_groups = (const struct sid *)((char *)filter_groups + disable_size);
        restrict_count = get_sid_count( restrict_groups, restrict_size );

        token = token_duplicate( src_token, src_token->primary, src_token->impersonation_level, NULL,
                                 filter_privileges, priv_count, filter_groups, group_count, FALSE );
        if (token)
        {
            token_apply_filter( token, req->flags );

            token->is_restricted = (src_token->is_restricted || restrict_count > 0);

            if (src_token->is_restricted || !list_empty( &src_token->restricted_sids ))
            {
                if (restrict_count)
                {
                    struct list *cursor, *cursor_next;
                    LIST_FOR_EACH_SAFE( cursor, cursor_next, &token->restricted_sids )
                    {
                        struct group *rg = LIST_ENTRY( cursor, struct group, entry );
                        const struct sid *s = restrict_groups;
                        unsigned int j, matched = 0;
                        for (j = 0; j < restrict_count; j++)
                        {
                            if (equal_sid( &rg->sid, s )) { matched = 1; break; }
                            s = (const struct sid *)((const char *)s + sid_len( s ));
                        }
                        if (!matched)
                        {
                            list_remove( &rg->entry );
                            free( rg );
                        }
                    }
                    if (list_empty( &token->restricted_sids ))
                    {
                        static const struct sid null_sid = { SID_REVISION, 1, SECURITY_NULL_SID_AUTHORITY, { SECURITY_NULL_RID } };
                        size_t size = max( sizeof(struct group), offsetof( struct group, sid.sub_auth[1] ) );
                        struct group *res_group = mem_alloc( size );
                        if (!res_group)
                        {
                            set_error( STATUS_NO_MEMORY );
                            release_object( token );
                            release_object( src_token );
                            return;
                        }
                        res_group->attrs = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
                        copy_sid( &res_group->sid, &null_sid );
                        list_add_tail( &token->restricted_sids, &res_group->entry );
                    }
                }
            }
            else if (restrict_count)
            {
                const struct sid *sid = restrict_groups;
                unsigned int i;
                for (i = 0; i < restrict_count; i++)
                {
                    struct group *existing;
                    int found = 0;
                    LIST_FOR_EACH_ENTRY( existing, &token->restricted_sids, struct group, entry )
                    {
                        if (equal_sid( &existing->sid, sid )) { found = 1; break; }
                    }
                    if (!found)
                    {
                        size_t size = offsetof( struct group, sid.sub_auth[sid->sub_count] );
                        struct group *res_group = mem_alloc( size );
                        if (!res_group)
                        {
                            set_error( STATUS_NO_MEMORY );
                            release_object( token );
                            release_object( src_token );
                            return;
                        }
                        res_group->attrs = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
                        copy_sid( &res_group->sid, sid );
                        list_add_tail( &token->restricted_sids, &res_group->entry );
                    }
                    sid = (const struct sid *)((char *)sid + sid_len( sid ));
                }
            }

            {
                unsigned int access = get_handle_access( current->process, req->handle );
                reply->new_handle = alloc_handle_no_access_check( current->process, token, access, 0 );
                release_object( token );
            }
        }
        release_object( src_token );
    }
}

/* creates a LowBox (AppContainer) token */
DECL_HANDLER(create_lowbox_token)
{
    struct token *src_token, *token;

    if ((src_token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_DUPLICATE, &token_ops )))
    {
        const struct sid *package_sid = get_req_data();
        data_size_t package_sid_size = min( req->package_sid_size, get_req_data_size() );
        const struct sid *capabilities_data = (const struct sid *)((char *)package_sid + package_sid_size);
        data_size_t capabilities_size = get_req_data_size() - package_sid_size;
        struct sid low_label_sid = { SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, { SECURITY_MANDATORY_LOW_RID } };

        if (!package_sid_size || !sid_valid_size( package_sid, package_sid_size ))
        {
            set_error( STATUS_INVALID_PARAMETER );
            release_object( src_token );
            return;
        }

        token = token_duplicate( src_token, src_token->primary, src_token->impersonation_level, NULL,
                                 NULL, 0, NULL, 0, FALSE );
        if (token)
        {
            unsigned int access;

            token->app_container = memdup( package_sid, sid_len( package_sid ) );
            token_assign_label( token, &low_label_sid );

            /* Add capabilities */
            while (capabilities_size > 0)
            {
                size_t size;
                struct group *cap;

                if (!sid_valid_size( capabilities_data, capabilities_size )) break;
                size = offsetof( struct group, sid.sub_auth[capabilities_data->sub_count] );
                cap = mem_alloc( size );
                if (cap)
                {
                    cap->attrs = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
                    copy_sid( &cap->sid, capabilities_data );
                    list_add_tail( &token->capabilities, &cap->entry );
                }
                capabilities_size -= sid_len( capabilities_data );
                capabilities_data = (const struct sid *)((char *)capabilities_data + sid_len( capabilities_data ));
            }

            access = req->access ? req->access : get_handle_access( current->process, req->handle );
            reply->token_handle = alloc_handle_no_access_check( current->process, token, access, 0 );
            release_object( token );
        }
        release_object( src_token );
    }
}

/* checks the specified privileges are held by the token */
DECL_HANDLER(check_token_privileges)
{
    struct token *token;

    if ((token = (struct token *)get_handle_obj( current->process, req->handle,
                                                 TOKEN_QUERY,
                                                 &token_ops )))
    {
        unsigned int count = get_req_data_size() / sizeof(struct luid_attr);

        if (!token->primary && token->impersonation_level <= SecurityAnonymous)
            set_error( STATUS_BAD_IMPERSONATION_LEVEL );
        else if (get_reply_max_size() >= count * sizeof(struct luid_attr))
        {
            struct luid_attr *usedprivs = set_reply_data_size( count * sizeof(*usedprivs) );
            reply->has_privileges = token_check_privileges( token, req->all_required, get_req_data(), count, usedprivs );
        }
        else
            set_error( STATUS_BUFFER_OVERFLOW );
        release_object( token );
    }
}

/* checks that a user represented by a token is allowed to access an object
 * represented by a security descriptor */
DECL_HANDLER(access_check)
{
    data_size_t sd_size = get_req_data_size();
    const struct security_descriptor *sd = get_req_data();
    struct token *token;

    if (!sd_is_valid( sd, sd_size ))
    {
        set_error( STATUS_ACCESS_VIOLATION );
        return;
    }

    if ((token = (struct token *)get_handle_obj( current->process, req->handle,
                                                 TOKEN_QUERY,
                                                 &token_ops )))
    {
        unsigned int status;
        struct luid_attr priv;
        unsigned int priv_count = 1;

        memset(&priv, 0, sizeof(priv));

        /* only impersonation tokens may be used with this function */
        if (token->primary)
        {
            set_error( STATUS_NO_IMPERSONATION_TOKEN );
            release_object( token );
            return;
        }
        /* anonymous impersonation tokens can't be used */
        if (token->impersonation_level <= SecurityAnonymous)
        {
            set_error( STATUS_BAD_IMPERSONATION_LEVEL );
            release_object( token );
            return;
        }

        status = token_access_check( token, sd, req->desired_access, &priv, &priv_count, &req->mapping,
                                     &reply->access_granted, &reply->access_status );

        reply->privileges_len = priv_count*sizeof(struct luid_attr);

        if ((priv_count > 0) && (reply->privileges_len <= get_reply_max_size()))
        {
            struct luid_attr *privs = set_reply_data_size( priv_count * sizeof(*privs) );
            memcpy( privs, &priv, sizeof(priv) );
        }

        set_error( status );
        release_object( token );
    }
}

/* retrieves an SID from the token */
DECL_HANDLER(get_token_sid)
{
    struct token *token;

    reply->sid_len = 0;

    if ((token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_QUERY, &token_ops )))
    {
        const struct sid *sid = NULL;

        switch (req->which_sid)
        {
        case TokenUser:
            assert(token->user);
            sid = token->user;
            break;
        case TokenPrimaryGroup:
            sid = token->primary_group;
            break;
        case TokenOwner:
            sid = token->owner;
            break;
        case TokenAppContainerSid:
            sid = token->app_container;
            break;
        default:
            set_error( STATUS_INVALID_PARAMETER );
            break;
        }

        if (sid)
        {
            reply->sid_len = sid_len( sid );
            if (reply->sid_len <= get_reply_max_size()) set_reply_data( sid, reply->sid_len );
            else set_error( STATUS_BUFFER_TOO_SMALL );
        }
        release_object( token );
    }
}

/* retrieves the groups that the user represented by the token belongs to */
DECL_HANDLER(get_token_groups)
{
    struct token *token;

    if ((token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_QUERY, &token_ops )))
    {
        unsigned int group_count = 0;
        const struct group *group;
        const struct list *group_list = (req->which_groups == 1) ? &token->restricted_sids : &token->groups;

        LIST_FOR_EACH_ENTRY( group, group_list, const struct group, entry )
        {
            if (req->attr_mask && !(group->attrs & req->attr_mask)) continue;
            group_count++;
            reply->sid_len += sid_len( &group->sid );
        }
        reply->attr_len = sizeof(unsigned int) * group_count;

        if (reply->attr_len + reply->sid_len <= get_reply_max_size())
        {
            unsigned int *attr_ptr = set_reply_data_size( reply->attr_len + reply->sid_len );
            struct sid *sid = (struct sid *)(attr_ptr + group_count);

            if (attr_ptr)
            {
                LIST_FOR_EACH_ENTRY( group, group_list, const struct group, entry )
                {
                    if (req->attr_mask && !(group->attrs & req->attr_mask)) continue;
                    sid = copy_sid( sid, &group->sid );
                    *attr_ptr++ = group->attrs;
                }
            }
        }
        else set_error( STATUS_BUFFER_TOO_SMALL );

        release_object( token );
    }
}

DECL_HANDLER(get_token_info)
{
    struct token *token;

    if ((token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_QUERY, &token_ops )))
    {
        reply->token_id = token->token_id;
        reply->modified_id = token->modified_id;
        reply->auth_id = token->auth_id;
        reply->session_id = token->session_id;
        reply->primary = token->primary;
        reply->impersonation_level = token->impersonation_level;
        reply->elevation_type = token->elevation;
        /* Tokens with TokenElevationTypeDefault are considered elevated for the
           purposes of GetTokenInformation(TokenElevation) if they belong to the
           admins group. */
        if (token->elevation == TokenElevationTypeDefault)
            reply->is_elevated = token_sid_present( token, &builtin_admins_sid, FALSE );
        else
            reply->is_elevated = token->elevation == TokenElevationTypeFull;
        reply->group_count = list_count( &token->groups );
        reply->privilege_count = list_count( &token->privileges );
        reply->integrity_rid = token->integrity_rid;
        reply->is_app_container = (token->app_container != NULL);
        reply->is_restricted = token->is_restricted || !list_empty( &token->restricted_sids );
        release_object( token );
    }
}

DECL_HANDLER(set_token_integrity_level)
{
    struct token *token;

    if ((token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_ADJUST_DEFAULT, &token_ops )))
    {
        struct sid label_sid = { SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, { req->integrity_rid } };

        /* raising the integrity level needs SeTcbPrivilege, lowering does not */
        if (elevation_enforced() && req->integrity_rid > token->integrity_rid &&
            !token_has_privilege( current->process->token, SeTcbPrivilege ))
        {
            set_error( STATUS_PRIVILEGE_NOT_HELD );
            release_object( token );
            return;
        }
        token_assign_label( token, &label_sid );
        release_object( token );
    }
}

DECL_HANDLER(get_token_default_dacl)
{
    struct token *token;

    if (!(token = (struct token *)get_handle_obj( current->process, req->handle,
                                                  TOKEN_QUERY, &token_ops )))
        return;

    if (token->default_dacl)
    {
        reply->acl_len = token->default_dacl->size;
        if (reply->acl_len <= get_reply_max_size())
        {
            struct acl *acl_reply = set_reply_data_size( reply->acl_len );
            if (acl_reply) memcpy( acl_reply, token->default_dacl, reply->acl_len );
        }
        else set_error( STATUS_BUFFER_TOO_SMALL );
    }
    release_object( token );
}

DECL_HANDLER(set_token_default_dacl)
{
    struct token *token;
    struct acl *new_dacl = NULL;
    const struct acl *acl = get_req_data();
    unsigned int acl_size = get_req_data_size();

    if (acl_size && !acl_is_valid( acl, acl_size ))
    {
        set_error( STATUS_INVALID_ACL );
        return;
    }

    if ((token = (struct token *)get_handle_obj( current->process, req->handle,
                                                 TOKEN_ADJUST_DEFAULT,
                                                 &token_ops )))
    {
        /* Allocate before replacing the old DACL: failure must not silently mutate it. */
        if (acl_size && !(new_dacl = memdup( acl, acl_size )))
        {
            release_object( token );
            return;
        }
        free( token->default_dacl );
        token->default_dacl = new_dacl;
        allocate_luid( &token->modified_id );

        release_object( token );
    }
}

/* E1-S10 is deliberately unavailable in normal builds. S18 must supply a
 * trusted consumed-record producer after S8 redemption, current authority and
 * retained-caller validation. The only producer here is a synthetic lab fixture;
 * a kind-4 record, reference or capability mask alone never proves authentication.
 * SPDX-License-Identifier for this new implementation: GPL-2.0-or-later.
 */
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
/* WineExplorer E1 lab: the S10 token context and its operations (R5: wex_e1lab_token.h) */
#define WEX_E1LAB_TOKEN_INCLUDED
#include "wex_e1lab_token.h"
#endif

const struct wex_account *token_e1s_account( struct token *token )
{
    return token->e1s_account.generation ? &token->e1s_account : NULL;
}

const struct sid *token_get_user_sid( struct token *token )
{
    return token->user;
}

int token_e1s_profile_high( struct token *token )
{
    return token->integrity_rid >= SECURITY_MANDATORY_HIGH_RID;
}

int token_e1s_broker(void)
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    return e1s_broker();
#else
    return 0;
#endif
}

/* GPL-2.0-or-later S14b: bind the one-shot LOAD context to the server's lease.
 * Commit does not redeem evidence again or extend its deadline. S18 still owns
 * the final authority/deadline/cancellation checks before guarded resume. */
int token_bind_e1s_profile( obj_handle_t context, struct token *token, const unsigned char *lease_id )
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    return e1lab_bind_e1s_profile( context, token, lease_id );
#else
    return 0;
#endif
}

int token_validate_e1s_commit( obj_handle_t context, struct token *token, const unsigned char *lease_id,
                             uint64_t request_id, uint64_t server_gen )
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    return e1lab_validate_e1s_commit( context, token, lease_id, request_id, server_gen );
#else
    set_error( STATUS_NOT_SUPPORTED ); return 0;
#endif
}

/* create_process duplicates its primary internally, preserving lineage,
 * AuthenticationId and ModifiedId. SID or logon identity alone is insufficient;
 * a fresh S10 issuance of the same account has a different lineage and logon. */
int token_e1s_commit_identity( struct token *issued, struct token *primary )
{
    return issued && primary && primary->primary && issued->primary &&
           is_equal_luid( issued->lineage_id, primary->lineage_id ) &&
           is_equal_luid( issued->auth_id, primary->auth_id ) &&
           is_equal_luid( issued->modified_id, primary->modified_id ) &&
           issued->session_id == primary->session_id &&
           primary->elevation == TokenElevationTypeFull && primary->integrity_rid == SECURITY_MANDATORY_HIGH_RID &&
           !primary->is_restricted && !primary->app_container &&
           list_empty( &primary->restricted_sids ) && list_empty( &primary->capabilities ) &&
           primary->e1s_account.generation == issued->e1s_account.generation &&
           !memcmp( primary->e1s_account.incarnation, issued->e1s_account.incarnation, 16 ) &&
           equal_sid( primary->user, issued->user );
}

int token_validate_e1s_profile( obj_handle_t context, struct token *token, const struct wex_account *account )
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    return e1lab_validate_e1s_profile( context, token, account );
#else
    return 0;
#endif
}

int token_claim_e1s_profile( obj_handle_t context, struct token *token, const void *record,
                            data_size_t size, struct wex_account *account )
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    return e1lab_claim_e1s_profile( context, token, record, size, account );
#else
    return 0;
#endif
}

/* Broker storage query before LOAD. Same identity record as LOAD except the
 * storage generation must be 0. Never burns profile_attempted, never grants
 * or extends anything; repeatable only until LOAD has been attempted. */
int token_query_e1s_profile( obj_handle_t context, struct token *token, const void *record,
                             data_size_t size, struct wex_account *account )
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    return e1lab_query_e1s_profile( context, token, record, size, account );
#else
    (void)context; (void)token; (void)record; (void)size; (void)account;
    return 0;
#endif
}

/* S14 obtains the request id from the retained consumed context, never from
 * the storage reference, token's numeric handle or an unload caller. */
int token_e1s_profile_request( obj_handle_t context, struct token *token, uint64_t *request_id, uint64_t *server_gen )
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    return e1lab_e1s_profile_request( context, token, request_id, server_gen );
#else
    return 0;
#endif
}

int token_install_e1s_consumed( obj_handle_t context, const struct e1s_token_consumed *record )
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    return e1lab_install_e1s_consumed( context, record );
#else
    set_error( STATUS_NOT_SUPPORTED );
    return 0;
#endif
}

DECL_HANDLER(create_e1s_account_token)
{
#if defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    e1lab_create_e1s_account_token( req, reply );
#else
    /* No production capability negotiation, consumed-record producer or fallback. */
    set_error( STATUS_NOT_SUPPORTED );
#endif
}

DECL_HANDLER(create_linked_token)
{
    struct token *token, *linked;
    int elevation, primary;

    /* a standard user has no linked token (native: ERROR_NO_SUCH_LOGON_SESSION) */
    if (elevation_enforced() && standard_user_session)
    {
        set_error( STATUS_NO_SUCH_LOGON_SESSION );
        return;
    }

    if ((token = (struct token *)get_handle_obj( current->process, req->handle,
                                                 TOKEN_QUERY, &token_ops )))
    {
        switch (token->elevation)
        {
        case TokenElevationTypeFull:
            elevation = TokenElevationTypeLimited;
            break;
        case TokenElevationTypeLimited:
            elevation = TokenElevationTypeFull;
            break;
        default:
            release_object( token );
            return;
        }
        /* Everyone gets an identification-level token. Windows hands a SeTcbPrivilege caller a usable primary
         * token (docs/ELEVATION_BOUNDARY_PLAN.md, N6): here only the system role does (the elevation broker),
         * because Wine's administrator token holds SeTcbPrivilege too. */
        primary = elevation_enforced() && sysrun_process_is_tcb_system( current->process );
        if ((linked = token_create_admin( primary, primary ? -1 : SecurityIdentification, elevation, token->session_id )))
        {
            unsigned int access = MAXIMUM_ALLOWED;
            /* the linked token belongs to the same logon as its source */
            linked->auth_id = token->auth_id;
            /* a Medium caller cannot get the write rights of a High token (mandatory label) */
            if (check_object_access( NULL, &linked->obj, &access ))
                reply->linked = alloc_handle_no_access_check( current->process, linked, access & TOKEN_ALL_ACCESS, 0 );
            release_object( linked );
        }
        release_object( token );
    }
}

/* WineExplorer system tier (docs/SYSTEM_TIER_DESIGN.md): a token described by a user's server to session 0,
 * which builds a primary token from it for that program's registry channel. Both ends are the same server
 * build, so the description uses native layout: a header, then the user, owner and primary group SIDs, the
 * AppContainer SID when there is one, the groups, the privileges, the restricting SIDs and the capabilities
 * (each a 32-bit attribute word and a SID, privileges as luid_attr), then the default DACL. Every item is a
 * multiple of 4 bytes long. */
#define WEX_TOKEN_DESC_MAGIC 0x31544458  /* "XDT1" */

struct wex_token_desc
{
    unsigned int magic;
    unsigned int size;
    unsigned int session_id;
    int          elevation;
    unsigned int integrity_rid;
    unsigned int is_restricted;
    unsigned int group_count;
    unsigned int priv_count;
    unsigned int restricted_count;
    unsigned int capability_count;
    unsigned int has_app_container;
    unsigned int dacl_size;
    struct luid  auth_id;
};

static data_size_t desc_put( char *buf, data_size_t pos, data_size_t size, const void *data, data_size_t len )
{
    if (buf && pos + len <= size) memcpy( buf + pos, data, len );
    return pos + len;
}

static data_size_t desc_put_groups( char *buf, data_size_t pos, data_size_t size, struct list *list,
                                    unsigned int *count )
{
    struct group *group;

    *count = 0;
    LIST_FOR_EACH_ENTRY( group, list, struct group, entry )
    {
        pos = desc_put( buf, pos, size, &group->attrs, sizeof(group->attrs) );
        pos = desc_put( buf, pos, size, &group->sid, sid_len( &group->sid ));
        (*count)++;
    }
    return pos;
}

/* describe a token; returns the size needed, the description is written only when it fits */
data_size_t token_describe( struct token *token, void *data, data_size_t size )
{
    struct wex_token_desc hdr;
    struct privilege *priv;
    char *buf = data;
    data_size_t pos = sizeof(hdr);

    memset( &hdr, 0, sizeof(hdr) );
    hdr.magic = WEX_TOKEN_DESC_MAGIC;
    hdr.session_id = token->session_id;
    hdr.elevation = token->elevation;
    hdr.integrity_rid = token->integrity_rid;
    hdr.is_restricted = token->is_restricted;
    hdr.has_app_container = token->app_container != NULL;
    hdr.dacl_size = token->default_dacl ? token->default_dacl->size : 0;
    hdr.auth_id = token->auth_id;

    pos = desc_put( buf, pos, size, token->user, sid_len( token->user ));
    pos = desc_put( buf, pos, size, token->owner, sid_len( token->owner ));
    pos = desc_put( buf, pos, size, token->primary_group, sid_len( token->primary_group ));
    if (token->app_container) pos = desc_put( buf, pos, size, token->app_container, sid_len( token->app_container ));
    pos = desc_put_groups( buf, pos, size, &token->groups, &hdr.group_count );
    LIST_FOR_EACH_ENTRY( priv, &token->privileges, struct privilege, entry )
    {
        struct luid_attr la;
        la.luid = priv->luid;
        la.attrs = (priv->enabled ? SE_PRIVILEGE_ENABLED : 0) | (priv->def ? SE_PRIVILEGE_ENABLED_BY_DEFAULT : 0);
        pos = desc_put( buf, pos, size, &la, sizeof(la) );
        hdr.priv_count++;
    }
    pos = desc_put_groups( buf, pos, size, &token->restricted_sids, &hdr.restricted_count );
    pos = desc_put_groups( buf, pos, size, &token->capabilities, &hdr.capability_count );
    if (token->default_dacl) pos = desc_put( buf, pos, size, token->default_dacl, token->default_dacl->size );
    hdr.size = pos;
    desc_put( buf, 0, size, &hdr, sizeof(hdr) );
    return pos;
}

static const struct sid *desc_get_sid( const char *buf, data_size_t size, data_size_t *pos )
{
    const struct sid *sid = (const struct sid *)(buf + *pos);

    if (*pos > size || size - *pos < offsetof( struct sid, sub_auth )) return NULL;
    if (!sid_valid_size( sid, size - *pos ) || sid->revision != SID_REVISION) return NULL;
    *pos += sid_len( sid );
    return sid;
}

static int desc_get_groups( const char *buf, data_size_t size, data_size_t *pos, unsigned int count,
                            struct list *list )
{
    unsigned int i, attrs;
    const struct sid *sid;

    for (i = 0; i < count; i++)
    {
        struct group *group;

        if (*pos > size || size - *pos < sizeof(attrs)) return 0;
        memcpy( &attrs, buf + *pos, sizeof(attrs) );
        *pos += sizeof(attrs);
        if (!(sid = desc_get_sid( buf, size, pos ))) return 0;
        if (!(group = mem_alloc( offsetof( struct group, sid.sub_auth[sid->sub_count] )))) return 0;
        group->attrs = attrs;
        copy_sid( &group->sid, sid );
        list_add_tail( list, &group->entry );
    }
    return 1;
}

/* WineExplorer H3 (docs/EVENTLOG_DESIGN.md, the caller class): a token made from another server's description over
 * a link (link_bound_token) records it; its copies keep it */
void token_set_link_bound( struct token *token, unsigned int session )
{
    token->link_bound = 1;
    token->link_session = session;
}

int token_is_link_bound( struct token *token )
{
    return token->link_bound;
}

/* ---- WineExplorer: link-bound tokens (docs/SESSION0_CROSS_SERVER.md, "Link-bound tokens"; B132) ----
 * What a user's server describes beyond its user SID is that server's claim. Session 0 checks it against its own
 * accounts record before the token is used (session0.c, link_bound_token): */

static int sid_in_nt_authority( const struct sid *sid )
{
    static const unsigned char nt_authority[6] = { 0, 0, 0, 0, 0, 5 };
    return sid->revision == SID_REVISION && !memcmp( sid->id_auth, nt_authority, sizeof(nt_authority) );
}

/* a session's system role (SYSTEM, LocalService, NetworkService described by a user's server) becomes the account
 * whose uid runs it: the user SID is the account's, the service SID stays only as a deny-only group (so a deny ACE
 * still matches it, an allow ACE no longer does). Nonzero when the user was such an account. */
int token_map_service_user( struct token *token, const unsigned int subauth[5] )
{
    struct sid account = { SID_REVISION, 5, SECURITY_NT_AUTHORITY,
                           { subauth[0], subauth[1], subauth[2], subauth[3], subauth[4] } };
    struct sid *old = token->user, *user;
    struct group *group;
    size_t size;

    if (token_service_account( token ) == -1) return 0;
    if (!(user = memdup( &account, sid_len( &account )))) return 0;
    size = max( offsetof( struct group, sid.sub_auth[old->sub_count] ), sizeof(*group) );
    if (!(group = mem_alloc( size )))
    {
        free( user );
        return 0;
    }
    memset( group, 0, size );
    copy_sid( &group->sid, old );
    group->attrs = SE_GROUP_USE_FOR_DENY_ONLY;
    list_add_tail( &token->groups, &group->entry );
    token->user = user;
    token->session_system = 1;  /* the caller class names it (WEX_CALLER_SESSION_SYSTEM); no SID does */
    free( old );
    /* the user's own limited token has INTERACTIVE (its logon): the system role in session 0 is that token, so it
     * writes the Application log as its user does (Windows' descriptor grants IU), never System */
    if (!token_sid_present( token, &interactive_sid, FALSE ))
    {
        size = max( offsetof( struct group, sid.sub_auth[interactive_sid.sub_count] ), sizeof(*group) );
        if ((group = mem_alloc( size )))
        {
            memset( group, 0, size );
            copy_sid( &group->sid, &interactive_sid );
            group->attrs = SE_GROUP_MANDATORY | SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT;
            list_add_tail( &token->groups, &group->entry );
        }
    }
    return 1;
}

/* is rid among the verified ones */
static int rid_verified( unsigned int rid, const unsigned int *rids, unsigned int count )
{
    unsigned int i;
    for (i = 0; i < count; i++) if (rids[i] == rid) return 1;
    return 0;
}

/* An enabled group of the token, or its user: may it stay the owner? */
static int owner_allowed( struct token *token, const struct sid *owner )
{
    struct group *group;

    if (equal_sid( owner, token->user )) return 1;
    LIST_FOR_EACH_ENTRY( group, &token->groups, struct group, entry )
        if (equal_sid( &group->sid, owner )) return !(group->attrs & SE_GROUP_USE_FOR_DENY_ONLY) &&
                                                    (group->attrs & SE_GROUP_ENABLED);
    return 0;
}

/* Apply what session 0's record says (flags: LINK_POLICY_ADMIN, the account is an administrator; LINK_POLICY_MAPPED,
 * the user was a session's system role). An administrator's own description (not mapped) is believed, as since T1.
 * Otherwise, as UAC's filtered token: every BUILTIN alias but Users and Guests deny-only (the record models only
 * Administrators, through the admin group); domain groups kept only when verified (the domain record's groups, or
 * the account's own domain's 513, its primary group), never Domain, Schema or Enterprise Admins; a mapped token's
 * SERVICE and per-service groups deny-only; a standard user's privileges; Medium integrity; elevation Limited for an
 * administrator's mapped token, else Default; the owner the user when it is no longer an enabled group. domain:
 * the account's domain SID subauthorities (21, a, b, c), rids its verified domain groups. Nonzero when cut. */
int token_apply_link_policy( struct token *token, unsigned int flags, const unsigned int domain[4],
                             const unsigned int *rids, unsigned int count )
{
    static const unsigned int standard_privileges[] =
    {
        SE_CHANGE_NOTIFY_PRIVILEGE, SE_SHUTDOWN_PRIVILEGE, SE_UNDOCK_PRIVILEGE, SE_INC_WORKING_SET_PRIVILEGE,
        SE_TIME_ZONE_PRIVILEGE,
    };
    struct privilege *privilege, *next;
    struct group *group, *next_group;
    unsigned int i, elevation;
    int cut = 0;

    /* SYSTEM, LocalService and NetworkService as groups of a description that session 0 did not map: no user's token
     * has them, so they are removed, not made deny-only, whatever the description says of them (an administrator's
     * too). A deny-only SYSTEM then means only what token_map_service_user made: a session's system role. */
    if (!(flags & LINK_POLICY_MAPPED))
    {
        LIST_FOR_EACH_ENTRY_SAFE( group, next_group, &token->groups, struct group, entry )
        {
            const struct sid *sid = &group->sid;

            if (!sid_in_nt_authority( sid ) || sid->sub_count != 1 ||
                (sid->sub_auth[0] != SECURITY_LOCAL_SYSTEM_RID && sid->sub_auth[0] != SECURITY_LOCAL_SERVICE_RID &&
                 sid->sub_auth[0] != SECURITY_NETWORK_SERVICE_RID)) continue;
            list_remove( &group->entry );
            free( group );
            cut = 1;
        }
    }
    if ((flags & LINK_POLICY_ADMIN) && !(flags & LINK_POLICY_MAPPED)) return cut;
    LIST_FOR_EACH_ENTRY( group, &token->groups, struct group, entry )
    {
        const struct sid *sid = &group->sid;
        int keep = 1;

        if (group->attrs == SE_GROUP_USE_FOR_DENY_ONLY || !sid_in_nt_authority( sid )) continue;
        if (sid->sub_count == 2 && sid->sub_auth[0] == SECURITY_BUILTIN_DOMAIN_RID)
            keep = sid->sub_auth[1] == DOMAIN_ALIAS_RID_USERS || sid->sub_auth[1] == DOMAIN_ALIAS_RID_GUESTS;
        else if (sid->sub_count == 5 && sid->sub_auth[0] == SECURITY_NT_NON_UNIQUE)
        {
            unsigned int rid = sid->sub_auth[4];
            int same_domain = domain && sid->sub_auth[1] == domain[1] && sid->sub_auth[2] == domain[2] &&
                              sid->sub_auth[3] == domain[3];

            if (rid == DOMAIN_GROUP_RID_ADMINS || rid == DOMAIN_GROUP_RID_SCHEMA_ADMINS ||
                rid == DOMAIN_GROUP_RID_ENTERPRISE_ADMINS)
                keep = 0;
            else keep = same_domain && (rid == DOMAIN_GROUP_RID_USERS || rid_verified( rid, rids, count ));
        }
        else if (sid->sub_count >= 1)
        {
            /* every other NT-authority SID: only the computed logon groups stay (the review: a role SID claimed as a
             * group, SYSTEM above all, would pass its ACEs as the user SID did); everything else deny-only */
            switch (sid->sub_auth[0])
            {
            case SECURITY_NETWORK_RID:            /* 2 */
            case SECURITY_BATCH_RID:              /* 3 */
            case SECURITY_INTERACTIVE_RID:        /* 4 */
            case SECURITY_ANONYMOUS_LOGON_RID:    /* 7 */
            case SECURITY_AUTHENTICATED_USER_RID: /* 11 */
            case SECURITY_TERMINAL_SERVER_RID:    /* 13 */
            case SECURITY_REMOTE_LOGON_RID:       /* 14 */
            case SECURITY_THIS_ORGANIZATION_RID:  /* 15 */
            case SECURITY_LOGON_IDS_RID:          /* 5-x-y, the logon SID */
            case 64:                              /* the authentication package */
            case 113:                             /* local account */
            case 114:                             /* local account and member of Administrators (deny ACEs use it) */
                keep = 1;
                break;
            default:
                keep = 0;
            }
        }
        else keep = 0;
        if (keep) continue;
        group->attrs = SE_GROUP_USE_FOR_DENY_ONLY;
        cut = 1;
    }
    LIST_FOR_EACH_ENTRY_SAFE( privilege, next, &token->privileges, struct privilege, entry )
    {
        for (i = 0; i < ARRAY_SIZE(standard_privileges); i++)
            if (!privilege->luid.high_part && privilege->luid.low_part == standard_privileges[i]) break;
        if (i < ARRAY_SIZE(standard_privileges)) continue;
        privilege_remove( privilege );
        cut = 1;
    }
    if (token->integrity_rid > SECURITY_MANDATORY_MEDIUM_RID)
    {
        token->integrity_rid = SECURITY_MANDATORY_MEDIUM_RID;
        cut = 1;
    }
    elevation = (flags & LINK_POLICY_ADMIN) ? TokenElevationTypeLimited : TokenElevationTypeDefault;
    if (token->elevation != elevation)
    {
        token->elevation = elevation;
        cut = 1;
    }
    if (token->owner && !owner_allowed( token, token->owner ))
    {
        struct sid *owner = memdup( token->user, sid_len( token->user ));

        if (owner)
        {
            free( token->owner );
            token->owner = owner;
            cut = 1;
        }
    }
    /* the primary group as the owner: SYSTEM or Administrators become the user's 513 (its domain's or the machine's
     * Users group) for an S-1-5-21 account, else the user */
    if (token->primary_group && (equal_sid( token->primary_group, &local_system_sid ) ||
                                 equal_sid( token->primary_group, &builtin_admins_sid )))
    {
        struct sid group, *primary;

        memset( &group, 0, sizeof(group) );
        memcpy( &group, token->user, sid_len( token->user ));  /* (allocated at its length) */
        if (group.sub_count == 5 && group.sub_auth[0] == SECURITY_NT_NON_UNIQUE) group.sub_auth[4] = DOMAIN_GROUP_RID_USERS;
        if ((primary = memdup( &group, sid_len( &group ))))
        {
            free( token->primary_group );
            token->primary_group = primary;
            cut = 1;
        }
    }
    return cut;
}

#ifdef WEX_X0_LAB_TEST
/* the X0 lab (wex_x0lab.c): a group added to a lab token before it is described, to show what session 0 makes of a
 * description that claims it (the link-bound token hardening's test of a role SID claimed as a group) */
void token_lab_add_group( struct token *token, const struct sid *sid, unsigned int attrs )
{
    size_t size = max( offsetof( struct group, sid.sub_auth[sid->sub_count] ), sizeof(struct group) );
    struct group *group;

    if (!(group = mem_alloc( size ))) return;
    memset( group, 0, size );
    copy_sid( &group->sid, sid );
    group->attrs = attrs;
    list_add_tail( &token->groups, &group->entry );
}
#endif

static int token_has_service_group( struct token *token )
{
    struct group *group;

    LIST_FOR_EACH_ENTRY( group, &token->groups, struct group, entry )
    {
        if (group->sid.revision != SID_REVISION || memcmp( group->sid.id_auth, service_sid.id_auth, 6 ) ||
            !group->sid.sub_count)
            continue;
        if (group->sid.sub_auth[0] == SECURITY_SERVICE_RID || group->sid.sub_auth[0] == 80) return 1;
    }
    return 0;
}

/* The one decision of who a service's caller is (E1 and U1), for every service of session 0:
 * - system-local: SYSTEM, LocalService or NetworkService of this server, which is session 0 or a plain prefix's own;
 *   never a token from another server's link, nor a system role in a user's server (it counts as the user, E1);
 * - elevated administrator: a real user (not one of those accounts, no SERVICE group or service SID) with
 *   Administrators enabled, High integrity or more and a token that is not the limited one (T1 believes it);
 * - user: anyone else authenticated (the service accounts and service groups of a link-bound token included);
 * - none: no token, or anonymous. */
unsigned int token_caller_class( struct token *token )
{
    static const struct sid anonymous_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_ANONYMOUS_LOGON_RID } };
    int service_account;

    if (!token || equal_sid( token->user, &anonymous_sid )) return WEX_CALLER_NONE;
    /* B132: a session's system role over a link, which session 0 maps to its account: only the server can tell it
     * from the user's own token. Only code that asks for this class sees it (R9's B0) */
    if (token->link_bound && token->session_system) return WEX_CALLER_SESSION_SYSTEM;
    service_account = token_service_account( token ) != -1;
    if (service_account)
        return !token->link_bound && (session0_server || !session0_client) ? WEX_CALLER_SYSTEM_LOCAL : WEX_CALLER_USER;
    if (token_has_service_group( token )) return WEX_CALLER_USER;
    if (token_sid_present( token, &builtin_admins_sid, FALSE ) && token->integrity_rid >= SECURITY_MANDATORY_HIGH_RID &&
        token->elevation != TokenElevationTypeLimited)
        return WEX_CALLER_ELEVATED_ADMIN;
    return WEX_CALLER_USER;
}

/* WineExplorer X0: can the token act (a primary token, or an impersonation token at impersonation or delegation
 * level)? An identification or anonymous impersonation token is never described to another server, since the
 * description is always built into a primary token there (docs/SESSION0_CROSS_SERVER.md, "Trust and constraints") */
int token_can_act( const struct token *token )
{
    return token->primary || token->impersonation_level >= SecurityImpersonation;
}

/* build a primary token from a description; NULL if it is malformed */
struct token *token_create_described( const void *data, data_size_t size )
{
    const char *buf = data;
    struct wex_token_desc hdr;
    const struct sid *user, *owner, *group, *app_container = NULL;
    const struct acl *dacl;
    struct token *token;
    data_size_t pos = sizeof(hdr), privs_pos;
    unsigned int i;

    if (size < sizeof(hdr)) return NULL;
    memcpy( &hdr, buf, sizeof(hdr) );
    if (hdr.magic != WEX_TOKEN_DESC_MAGIC || hdr.size != size || hdr.group_count > 1024 || hdr.priv_count > 256 ||
        hdr.restricted_count > 1024 || hdr.capability_count > 1024) return NULL;
    if (!(user = desc_get_sid( buf, size, &pos )) || !(owner = desc_get_sid( buf, size, &pos )) ||
        !(group = desc_get_sid( buf, size, &pos ))) return NULL;
    if (hdr.has_app_container && !(app_container = desc_get_sid( buf, size, &pos ))) return NULL;

    if (!(token = create_token( TRUE, hdr.session_id, user, owner, group, NULL, 0, NULL, 0, NULL, NULL, 0,
                                hdr.elevation ))) return NULL;
    token->integrity_rid = hdr.integrity_rid;
    token->auth_id = hdr.auth_id;
    if (!desc_get_groups( buf, size, &pos, hdr.group_count, &token->groups )) goto error;

    privs_pos = pos;
    if (pos > size || hdr.priv_count > (size - pos) / sizeof(struct luid_attr)) goto error;
    pos += hdr.priv_count * sizeof(struct luid_attr);
    for (i = 0; i < hdr.priv_count; i++)
    {
        struct luid_attr la;
        struct privilege *priv;

        memcpy( &la, buf + privs_pos + i * sizeof(la), sizeof(la) );
        if (!(priv = privilege_add( token, la.luid, la.attrs & SE_PRIVILEGE_ENABLED ))) goto error;
        priv->def = !!(la.attrs & SE_PRIVILEGE_ENABLED_BY_DEFAULT);
    }
    if (!desc_get_groups( buf, size, &pos, hdr.restricted_count, &token->restricted_sids )) goto error;
    if (!desc_get_groups( buf, size, &pos, hdr.capability_count, &token->capabilities )) goto error;
    token->is_restricted = !!hdr.is_restricted;
    if (app_container && !(token->app_container = memdup( app_container, sid_len( app_container )))) goto error;
    if (hdr.dacl_size)
    {
        if (pos > size || size - pos < hdr.dacl_size) goto error;
        dacl = (const struct acl *)(buf + pos);
        if (!acl_is_valid( dacl, hdr.dacl_size ) || dacl->size != hdr.dacl_size) goto error;
        if (!(token->default_dacl = memdup( dacl, hdr.dacl_size ))) goto error;
        pos += hdr.dacl_size;
    }
    if (pos != size) goto error;
    return token;

error:
    release_object( token );
    return NULL;
}

/* does the token carry a package context that no process image has been checked against? */
int token_package_pending( const struct token *token )
{
    return token->package && token->package_pending;
}

/* The first executable image of a process settles a pending package context, by the file the process
 * actually maps (device and inode), never by an image string. Returns 0 when that file is not the
 * executable of the application. */
int token_package_check_image( struct token *token, struct fd *fd )
{
    if (!token_package_pending( token )) return 1;
    if (!uwp_context_is_executable( token->package, fd )) return 0;
    token->package_pending = 0;
    return 1;
}

/* give a token the package context of an application of the catalog */
DECL_HANDLER(set_token_package)
{
    struct token *token;
    struct uwp_context *ctx;

    if (!(token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_ADJUST_DEFAULT, &token_ops )))
        return;

    /* Only a token that is not yet the identity of anything: a primary token that no process runs with and
     * that has no context. A process gets a context by being started with such a token from the executable
     * of the application (token_package_check_image), never by changing the token it runs with. */
    if (!token->primary || token->package || is_process_token( token )) set_error( STATUS_ACCESS_DENIED );
    else if (!uwp_compatibility_enabled( token_get_user_sid( token ) )) set_error( STATUS_NOT_SUPPORTED );  /* the gate is off */
    else if ((ctx = uwp_context_from_aumid( get_req_data(), get_req_data_size() )))
    {
        token->package = ctx;
        token->package_pending = 1;
    }
    release_object( token );
}

/* get the package context of a token */
DECL_HANDLER(get_token_package)
{
    struct token *token;

    if (!(token = (struct token *)get_handle_obj( current->process, req->handle, TOKEN_QUERY, &token_ops )))
        return;

    if (!token->package) set_error( STATUS_NOT_FOUND );
    else
    {
        struct uwp_context_sizes sizes;
        const void *data;

        reply->total      = uwp_context_get_data( token->package, &sizes, &data );
        reply->full_len   = sizes.full_name;
        reply->family_len = sizes.family_name;
        reply->app_len    = sizes.app_id;
        reply->root_len   = sizes.root;
        reply->exe_len    = sizes.executable;
        reply->entry_len  = sizes.entry_point;
        if (reply->total <= get_reply_max_size()) set_reply_data( data, reply->total );
        else set_error( STATUS_BUFFER_TOO_SMALL );
    }
    release_object( token );
}
