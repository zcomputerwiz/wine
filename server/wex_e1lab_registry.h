/*
 * WineExplorer E1 lab: the prefix id and the S13/S14 managed hives of registry.c (never in a shipped build)
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
 * Moved out of registry.c by R5 of docs/SESSION0_SPLIT_REVIEW.md, with no change. This file holds code, not
 * declarations (like Wine's code-bearing headers): registry.c includes it once, at one point, with the lab switches
 * only, because the lab hives work on the
 * registry's own objects (struct key, its values, the save branches and the static helpers that build, load and
 * write keys), which a separate file could reach only if registry.c exported them. registry.c keeps the hooks
 * listed in docs/STOCK_CHANGES.md: this include, the call in load_e1s_profile, the reaping after a periodic save
 * and the process start check.
 */

#ifndef WEX_E1LAB_REGISTRY_INCLUDED
#error "wex_e1lab_registry.h is included by registry.c, not compiled on its own"
#endif

/* E1-S6/R8: this prefix's id, made once per server (the R8 binding reports it, wex_e1lab_pipe.c) */
static unsigned char e1s_prefix_id[16];

int registry_get_prefix_id( unsigned char prefix_id[16] )
{
    static const unsigned char zero[16] = {0};
    if (!memcmp( e1s_prefix_id, zero, 16 ))
    {
        int fd = open( "/dev/urandom", O_RDONLY | O_CLOEXEC );
        if (fd < 0) return 0;
        while (!memcmp( e1s_prefix_id, zero, 16 ))
        {
            if (read( fd, e1s_prefix_id, 16 ) != 16)
            {
                close( fd );
                return 0;
            }
        }
        close( fd );
    }
    memcpy( prefix_id, e1s_prefix_id, 16 );
    return 1;
}

#if defined(WEX_E1S13_HKU_TEST) && defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
#include <sys/random.h>

static unsigned int e1s_hive_count;
static struct e1s_sid_generation *e1s_generations;
static uint64_t e1s_server_generation;

static int e1s_deny( unsigned int status, const char *site )
{
    fprintf( stderr, "E1S14 deny=%s status=%08x\n", site, status );
    set_error( status );
    return 0;
}

static int e1s_random( void *data, size_t size )
{
    ssize_t n;
    do n = getrandom( data, size, 0 ); while (n < 0 && errno == EINTR);
    return n == (ssize_t)size;
}

static int e1s_acquire_lease( struct e1s_hive *m, struct token *token, obj_handle_t context )
{
    struct wex_profile_lease_reserved *lease = &m->storage.reserved;
    struct e1s_sid_generation *g;
    uint64_t request_id, server_gen;
    unsigned int count = 0;
    if (!token_e1s_profile_request( context, token, &request_id, &server_gen ))
        return e1s_deny( STATUS_ACCESS_DENIED, "lease-request" );
    if (!e1s_server_generation)
    {
        /* reuse the ID if an R8 BINDING (registry_get_prefix_id) already made it */
        if (!registry_get_prefix_id( (unsigned char[16]){0} ))
        { e1s_server_generation = 0; return e1s_deny( STATUS_UNSUCCESSFUL, "lease-rng" ); }
        e1s_server_generation = server_gen;
    }
    if (server_gen != e1s_server_generation) return e1s_deny( STATUS_ACCESS_DENIED, "lease-server-generation" );
    for (g = e1s_generations; g; g = g->next)
    {
        count++;
        if (!memcmp( g->sid, m->account.sid, sizeof(g->sid) )) break;
    }
    if (!g)
    {
        if (count >= 4096) return e1s_deny( STATUS_INSUFFICIENT_RESOURCES, "generation-limit" );
        if (!(g = calloc( 1, sizeof(*g) ))) return e1s_deny( STATUS_NO_MEMORY, "generation-alloc" );
        memcpy( g->sid, m->account.sid, sizeof(g->sid) );
        g->generation = 1; g->next = e1s_generations; e1s_generations = g;
    }
    if (g->generation == UINT64_MAX) return e1s_deny( STATUS_INTEGER_OVERFLOW, "generation-exhausted" );
    m->generation = g;
    /* Generation is never reused, including setup failures. The 128-bit id is
     * (server generation, global lease allocation sequence), so leases for
     * different SIDs cannot collide. */
    {
        static uint64_t next_lease;
        if (next_lease == UINT64_MAX) return e1s_deny( STATUS_INTEGER_OVERFLOW, "lease-exhausted" );
        wex_token_put64( lease->lease_id, e1s_server_generation );
        wex_token_put64( lease->lease_id + 8, ++next_lease );
    }
    lease->request_id = request_id;
    lease->server_gen = e1s_server_generation;
    memcpy( lease->prefix_id, e1s_prefix_id, 16 );
    memcpy( lease->account_key, m->account.incarnation, 16 );
    memcpy( lease->user_sid, token_get_user_sid( token ), sizeof(lease->user_sid) );
    lease->map_gen = m->account.generation;
    lease->profile_generation = m->storage.profile_generation;
    lease->state = 1;
    return 1;
}

/* A loaded managed root never falls through to the local SID default. */
static int e1s_secure_keys( struct key *key, struct security_descriptor *sd, unsigned int depth, unsigned int *count )
{
    int i;
    size_t size = sizeof(*sd) + sd->owner_len + sd->group_len + sd->sacl_len + sd->dacl_len;
    if (depth > 64 || ++*count > 16384 || key->flags & (KEY_SYMLINK | KEY_PREDEF))
    { set_error( STATUS_NOT_REGISTRY_FILE ); return 0; }
    free( key->obj.sd );
    if (!(key->obj.sd = memdup( sd, size ))) return 0;
    key->e1s_sd = sd;
    if (list_empty( &key->e1s_entry )) list_add_tail( &e1s_keys, &key->e1s_entry );
    key->flags |= KEY_VOLATILE | KEY_E1S_PERSIST;
    for (i = 0; i <= key->last_subkey; i++)
        if (!e1s_secure_keys( key->subkeys[i], sd, depth + 1, count )) return 0;
    return 1;
}

static struct key *e1s_create_new_root( const char *ascii )
{
    struct unicode_str name;
    WCHAR *buffer = ascii_to_unicode_str( ascii, &name );
    struct key *key;
    struct object_params params = { .ops = &key_ops, .root = &root_key->obj, .name = name,
                                    .attr = OBJ_CASE_INSENSITIVE | OBJ_OPENLINK };
    if (!buffer) return NULL;
    key = open_named_object( &params );
    if (key)
    { release_object( key ); free( buffer ); set_error( STATUS_OBJECT_NAME_COLLISION ); return NULL; }
    if (get_error() != STATUS_OBJECT_NAME_NOT_FOUND) { free( buffer ); return NULL; }
    clear_error();
    key = create_key_recursive( root_key, name, current_time );
    free( buffer );
    return key;
}

static int e1s_read_hive( struct e1s_hive *m )
{
    struct stat before, after;
    FILE *stream;
    char *text;
    size_t used = 0;
    ssize_t n;
    int ret = 0;
    if (fstat( m->storage.hive_fd, &before ) || before.st_size < 24 || before.st_size > 1024 * 1024)
    { set_error( STATUS_NOT_REGISTRY_FILE ); return 0; }
    if (!(text = mem_alloc( before.st_size + 1 ))) return 0;
    while (used < before.st_size)
    {
        n = pread( m->storage.hive_fd, text + used, before.st_size - used, used );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += n;
    }
    text[used] = 0;
    if (memchr( text, 0, used ) || text[used - 1] != '\n' || fstat( m->storage.hive_fd, &after ) ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) goto done;
    if (!(stream = fmemopen( text, used, "r" ))) goto done;
    e1s_strict_loading = 1;
    load_keys( m->user, NULL, stream, 0 ); /* fixed relative base, no autodetection */
    e1s_strict_loading = 0;
    /* A saved hive lists parent keys that earlier sections already created: that is not a failure. */
    if (get_error() == STATUS_OBJECT_NAME_EXISTS) clear_error();
    ret = !get_error();
    if (ret) m->storage.hive_identity = after;
    fclose( stream );
done:
    free( text );
    if (!ret && !get_error()) set_error( STATUS_NOT_REGISTRY_FILE );
    return ret;
}

static void e1s_set_str_val( struct key *key, const char *name, const char *val )
{
    struct unicode_str uname, uval;
    WCHAR *wname = ascii_to_unicode_str( name, &uname );
    WCHAR *wval = ascii_to_unicode_str( val, &uval );
    if (wname && wval)
    {
        WCHAR *buf = mem_alloc( uval.len + sizeof(WCHAR) );
        if (buf)
        {
            memcpy( buf, uval.str, uval.len );
            buf[uval.len / sizeof(WCHAR)] = 0;
            set_value( key, uname, REG_SZ, buf, uval.len + sizeof(WCHAR) );
            free( buf );
        }
    }
    free( wname );
    free( wval );
}

static void e1s_populate_volatile_env( struct key *user, const struct wex_profile_storage *storage,
                                       const struct wex_account *account, struct security_descriptor *sd )
{
    struct unicode_str path;
    WCHAR *buffer = ascii_to_unicode_str( "Volatile Environment", &path );
    char buf[512];
    char computer_name[64] = "WINEHOST";
    struct key *vol_env;

    if (!buffer) return;
    vol_env = create_key_recursive( user, path, current_time );
    free( buffer );
    if (!vol_env) return;

    vol_env->flags |= KEY_VOLATILE;
    vol_env->flags &= ~KEY_E1S_PERSIST;

    if (sd)
    {
        size_t size = sizeof(*sd) + sd->owner_len + sd->group_len + sd->sacl_len + sd->dacl_len;
        free( vol_env->obj.sd );
        vol_env->obj.sd = memdup( sd, size );
        vol_env->e1s_sd = sd;
    }

    {
        static const WCHAR comp_path[] = {
            'S','y','s','t','e','m','\\',
            'C','o','n','t','r','o','l','S','e','t','0','0','1','\\',
            'C','o','n','t','r','o','l','\\',
            'C','o','m','p','u','t','e','r','N','a','m','e','\\',
            'A','c','t','i','v','e','C','o','m','p','u','t','e','r','N','a','m','e'
        };
        static const WCHAR comp_val[] = { 'C','o','m','p','u','t','e','r','N','a','m','e' };
        struct unicode_str cval_name = { comp_val, sizeof(comp_val) };
        struct key *ckey = find_subkey_by_path( root_key, comp_path, sizeof(comp_path) );
        int idx;
        struct key_value *kv;
        if (ckey && (kv = find_value( ckey, cval_name, &idx )) && kv->type == REG_SZ && kv->len > 0)
        {
            const WCHAR *w = kv->data;
            size_t wlen = kv->len / sizeof(WCHAR);
            size_t j;
            if (wlen > 0 && w[wlen - 1] == 0) wlen--;
            if (wlen > 0 && wlen < sizeof(computer_name))
            {
                for (j = 0; j < wlen; j++)
                    computer_name[j] = (w[j] < 128) ? (char)w[j] : '_';
                computer_name[wlen] = '\0';
            }
        }
    }

    /* 1. USERNAME */
    e1s_set_str_val( vol_env, "USERNAME", account->name );

    /* 2. USERPROFILE */
    e1s_set_str_val( vol_env, "USERPROFILE", storage->path.windows );

    /* 3. HOMEDRIVE */
    if (storage->path.windows[0] && storage->path.windows[1] == ':')
    {
        char drive[3] = { storage->path.windows[0], ':', '\0' };
        e1s_set_str_val( vol_env, "HOMEDRIVE", drive );
        /* 4. HOMEPATH */
        e1s_set_str_val( vol_env, "HOMEPATH", storage->path.windows + 2 );
    }
    else
    {
        e1s_set_str_val( vol_env, "HOMEDRIVE", "C:" );
        e1s_set_str_val( vol_env, "HOMEPATH", storage->path.windows );
    }

    /* 5. USERDOMAIN */
    e1s_set_str_val( vol_env, "USERDOMAIN", computer_name );

    /* 6. APPDATA */
    snprintf( buf, sizeof(buf), "%s\\AppData\\Roaming", storage->path.windows );
    e1s_set_str_val( vol_env, "APPDATA", buf );

    /* 7. LOCALAPPDATA */
    snprintf( buf, sizeof(buf), "%s\\AppData\\Local", storage->path.windows );
    e1s_set_str_val( vol_env, "LOCALAPPDATA", buf );

    /* 8. TEMP */
    snprintf( buf, sizeof(buf), "%s\\AppData\\Local\\Temp", storage->path.windows );
    e1s_set_str_val( vol_env, "TEMP", buf );

    /* 9. TMP */
    e1s_set_str_val( vol_env, "TMP", buf );

    /* Standard session variables */
    e1s_set_str_val( vol_env, "CLIENTNAME", "Console" );
    e1s_set_str_val( vol_env, "SESSIONNAME", "Console" );
    e1s_set_str_val( vol_env, "HOMESHARE", "" );

    release_object( vol_env );
}

/* A further logon of the account whose hive is already loaded and committed
 * (E1-S18 review R14). The loaded bundle is kept; this request gets its own
 * lease, to be committed once like the first. Only the exact same account,
 * storage generation and user SID may join, after the storage and the token
 * context have been validated again. The claim has already burned the LOAD
 * attempt of this context. */
static struct e1s_hive *e1s_join_hive( struct e1s_hive *m, struct token *token, obj_handle_t context,
                                       const struct wex_account *account, uint64_t storage_generation,
                                       unsigned int access, obj_handle_t *result )
{
    struct wex_profile_lease_reserved saved = m->storage.reserved;
    unsigned int status;

    if (memcmp( m->account.incarnation, account->incarnation, 16 ) ||
        m->account.generation != account->generation || m->account.uid != account->uid ||
        strcmp( m->account.name, account->name ) ||
        !storage_generation || storage_generation != m->storage.profile_generation ||
        memcmp( saved.user_sid, token_get_user_sid( token ), sizeof(saved.user_sid) ))
    { e1s_deny( STATUS_OBJECT_NAME_COLLISION, "join-identity" ); return NULL; }
    if (wex_profile_verify_existing( config_dir_fd, account, &m->storage ) ||
        !token_validate_e1s_profile( context, token, account ))
    { e1s_deny( STATUS_ACCESS_DENIED, "join-verify" ); return NULL; }
    if (!e1s_acquire_lease( m, token, context )) { m->storage.reserved = saved; return NULL; }
    if (!(*result = e1s_alloc_key_handle( m->user, access, 0 )) ||
        !token_bind_e1s_profile( context, token, m->storage.reserved.lease_id ))
    {
        status = get_error();
        if (*result) { close_handle( current->process, *result ); *result = 0; }
        m->storage.reserved = saved;
        set_error( status ? status : STATUS_INVALID_HANDLE );
        return NULL;
    }
    m->joined = 1;
    m->idle_sweeps = 0;
    fprintf( stderr, "E1S14 join lease state=1\n" );
    clear_error();
    return m;
}

static struct e1s_hive *e1s_load_hive( struct token *token, obj_handle_t context,
                                     const unsigned char *record, data_size_t size,
                                     unsigned int access, obj_handle_t *result )
{
    struct wex_account account;
    struct e1s_hive *m = NULL, *other;
    struct e1s_file_security *security = NULL;
    struct unicode_str path;
    struct stat prefix;
    struct key *alias = NULL;
    char name[128];
    WCHAR *buffer = NULL;
    unsigned int count = 0, status;
    enum wex_profile_status storage_status;
    set_error( STATUS_ACCESS_DENIED );
    if (!token_claim_e1s_profile( context, token, record, size, &account )) return NULL;
    for (other = e1s_hives; other; other = other->next)
        if (!memcmp( other->account.sid, account.sid, sizeof(account.sid) ))
        {
            if (other->phase == E1S_LOADED && other->storage.reserved.state == 2)
                return e1s_join_hive( other, token, context, &account, wex_token_u64( record + 32 ), access, result );
            e1s_deny( other->phase == E1S_UNLOADING ? STATUS_DELETE_PENDING :
                      other->phase == E1S_LOADING ? STATUS_DEVICE_NOT_READY :
                      other->phase == E1S_POISONED ? STATUS_REGISTRY_CORRUPT : STATUS_OBJECT_NAME_COLLISION,
                      "load-conflict" );
            return NULL;
        }
    if (equal_sid( token_get_user_sid( token ), security_server_user_sid() ) || e1s_hive_count >= 8)
    { set_error( STATUS_ACCESS_DENIED ); return NULL; }
    if (!(m = calloc( 1, sizeof(*m) ))) { set_error( STATUS_NO_MEMORY ); return NULL; }
    m->account = account; m->storage.profile_fd = m->storage.hive_fd = m->storage.sid_lock_fd = -1;
    m->phase = E1S_LOADING;
    m->next = e1s_hives; e1s_hives = m; e1s_hive_count++;
    if (fstat( config_dir_fd, &prefix )) goto failed;
    storage_status = wex_profile_open_existing( config_dir_fd, prefix.st_dev, prefix.st_ino,
        getuid(), getgid(), &account, wex_token_u64( record + 32 ), &m->storage );
    if (storage_status)
    {
        set_error( storage_status == WEX_PROFILE_ABSENT ? STATUS_OBJECT_PATH_NOT_FOUND :
                   storage_status == WEX_PROFILE_BUSY ? STATUS_SHARING_VIOLATION :
                   storage_status == WEX_PROFILE_RECOVERY ? STATUS_REGISTRY_CORRUPT : STATUS_ACCESS_DENIED );
        fprintf( stderr, "E1S14 deny=storage-open storage=%u status=%08x\n", storage_status, get_error() );
        goto failed;
    }
    if (!e1s_acquire_lease( m, token, context )) goto failed;
    clear_error();
    if (!(m->sd = e1s_profile_alloc_sd( token_get_user_sid( token ), 0 ))) goto failed;
    snprintf( name, sizeof(name), "User\\%s", m->storage.path.sid );
    if (!(m->user = e1s_create_new_root( name ))) goto failed;
    snprintf( name, sizeof(name), "User\\%s_Classes", m->storage.path.sid );
    if (!(alias = e1s_create_new_root( name ))) goto failed;
    if (!(security = apply_e1s_profile_security( &account, &m->storage )))
    { set_error( STATUS_ACCESS_DENIED ); goto failed; }
    if (!e1s_read_hive( m )) goto failed;
    buffer = ascii_to_unicode_str( "Software\\Classes", &path );
    if (!buffer || !(m->classes = create_key_recursive( m->user, path, current_time ))) goto failed;
    free( buffer ); buffer = NULL;
    /* Per-user classes use the same shared Wow6432Node rule as machine
     * classes. Ordinary Bob retains init_registry's existing behavior. */
    buffer = ascii_to_unicode_str( "Wow6432Node", &path );
    if (buffer)
    {
        struct key *wow = create_key_recursive( m->classes, path, current_time );
        if (!wow) goto failed;
        wow->flags |= KEY_WOWSHARE; release_object( wow );
    }
    free( buffer ); buffer = NULL;
    if (!e1s_secure_keys( m->user, m->sd, 0, &count ) || !e1s_secure_keys( alias, m->sd, 0, &count )) goto failed;
    /* HKU SID_Classes is an alias of the actual per-user Software/Classes.
     * HKCR uses a separate unnamed live union, never imports machine data into
     * Alice's hive. The whole operation is synchronous before publication. */
    snprintf( name, sizeof(name), "\\Registry\\User\\%s\\Software\\Classes", m->storage.path.sid );
    buffer = ascii_to_unicode_str( name, &path );
    if (!buffer) goto failed;
    /* A persisted hive already holds Software\Classes and Wow6432Node: NAME_EXISTS from the creates above is not an error. */
    if (get_error() == STATUS_OBJECT_NAME_EXISTS) clear_error();
    set_value( alias, symlink_str, REG_LINK, path.str, path.len );
    free( buffer ); buffer = NULL;
    if (get_error()) goto failed;
    alias->flags |= KEY_SYMLINK;
    snprintf( name, sizeof(name), "User\\%s_E1S13View", m->storage.path.sid );
    if (!(m->view = e1s_create_new_root( name ))) goto failed;
    if (!e1s_secure_keys( m->view, m->sd, 0, &count )) goto failed;
    key_unlink_name( &m->view->obj, m->view->obj.name );
    m->view->e1s_view = m;
    buffer = ascii_to_unicode_str( "Machine\\Software\\Classes", &path );
    if (buffer)
    {
        struct object_params params = { .ops = &key_ops, .root = &root_key->obj, .name = path,
                                        .attr = OBJ_CASE_INSENSITIVE };
        m->machine_classes = open_named_object( &params );
    }
    free( buffer ); buffer = NULL;
    if (!m->machine_classes || !e1s_refresh_view( m->view )) goto failed;
    if (wex_profile_verify_existing( config_dir_fd, &account, &m->storage ) || !e1s_profile_security_verify( security ) ||
        !token_validate_e1s_profile( context, token, &account ))
    { set_error( STATUS_ACCESS_DENIED ); goto failed; }
    if (save_branch_count < 3 || save_branch_count >= MAX_SAVE_BRANCH_INFO)
    { e1s_deny( STATUS_INSUFFICIENT_RESOURCES, "save-registration" ); goto failed; }
    if (!(*result = e1s_alloc_key_handle( m->user, access, 0 ))) goto failed;
    if (!token_bind_e1s_profile( context, token, m->storage.reserved.lease_id ))
    {
        close_handle( current->process, *result ); *result = 0;
        set_error( STATUS_INVALID_HANDLE ); goto failed;
    }
    m->user->flags |= KEY_E1S_ROOT;
    m->classes->flags |= KEY_E1S_ROOT;
    alias->flags |= KEY_E1S_ROOT;
    e1s_profile_security_commit( security );
    m->storage.acl_pending = 0;
    m->alias = alias;
    e1s_populate_volatile_env( m->user, &m->storage, &m->account, m->sd );
    {
        struct save_branch_info *info = &save_branch_info[save_branch_count++];
        memset( info, 0, sizeof(*info) ); list_init( &info->tombstones );
        info->key = (struct key *)grab_object( m->user );
        info->managed = m;
        info->filename = "<managed-profile>"; /* diagnostics only, never opened */
    }
    m->phase = E1S_LOADED;
    clear_error();
    return m;
failed:
    status = get_error();
    if (!status) status = STATUS_INSUFFICIENT_RESOURCES;
    free( buffer );
    if (m->view) { m->view->e1s_view = NULL; release_object( m->view ); }
    if (m->classes) release_object( m->classes );
    if (alias) { delete_key( alias, 1 ); release_object( alias ); }
    if (m->user) { delete_key( m->user, 1 ); release_object( m->user ); }
    if (m->machine_classes) release_object( m->machine_classes );
    e1s_profile_security_abort( security );
    wex_profile_storage_close( &m->storage );
    if (m->generation) m->generation->generation++;
    m->storage.reserved.state = 3;
    e1s_hives = m->next; e1s_hive_count--; /* synchronous reservation is list head */
    free( m->sd ); free( m );
    set_error( status );
    return NULL;
}

/* Cache keys are detached proxy objects: copied identity and generation, an
 * owned SD, and no reference to the hive or its HKCR view. A real open using a
 * proxy returns an ordinary handle, which participates in the busy check. */
static obj_handle_t e1s_cache_root( struct e1s_hive *m, unsigned int access, int classes )
{
    struct key *proxy;
    struct security_descriptor *sd = key_get_sd( &(classes ? m->view : m->user)->obj );
    obj_handle_t result = 0;
    unsigned int count = 0;
    char name[160];
    snprintf( name, sizeof(name), "User\\%s_E1S14Proxy", m->storage.path.sid );
    if (!(proxy = e1s_create_new_root( name ))) return 0;
    if (sd && e1s_secure_keys( proxy, sd, 0, &count ) &&
        (proxy->e1s_proxy_account = memdup( &m->account, sizeof(m->account) )))
    {
        list_remove( &proxy->e1s_entry ); list_init( &proxy->e1s_entry );
        proxy->e1s_sd = proxy->obj.sd;
        proxy->e1s_proxy_generation = m->generation->generation;
        proxy->e1s_proxy_classes = classes;
        result = e1s_alloc_key_handle( proxy, access, 0 );
    }
    key_unlink_name( &proxy->obj, proxy->obj.name );
    release_object( proxy );
    return result;
}

/* The synthetic KEY_VOLATILE bit isolates this tree from Bob's startup save
 * branches. KEY_E1S_PERSIST distinguishes actual persistent managed keys from
 * keys explicitly created with REG_OPTION_VOLATILE. */
static int e1s_write_keys( const struct key *key, const struct key *root, FILE *f,
                           unsigned int depth, unsigned int *count )
{
    int i;
    if (!(key->flags & KEY_E1S_PERSIST)) return 1;
    if (depth > 64 || ++*count > 16384 || key->flags & (KEY_SYMLINK | KEY_PREDEF | KEY_DELETED)) return 0;
    /* S13's supported hive format reestablishes the canonical descriptor on
     * load. Never claim durability for a changed per-key SD that format cannot
     * restore. Keep the hive/lease on this unsupported state. */
    if (key->obj.sd)
    {
        const struct security_descriptor *sd = key->e1s_sd;
        size_t size = sizeof(*sd) + sd->owner_len + sd->group_len + sd->sacl_len + sd->dacl_len;
        if (key->obj.sd->owner_len != sd->owner_len || key->obj.sd->group_len != sd->group_len ||
            key->obj.sd->sacl_len != sd->sacl_len || key->obj.sd->dacl_len != sd->dacl_len ||
            memcmp( key->obj.sd, sd, size )) return 0;
    }
    fprintf( f, "\n[" );
    if (key != root) dump_path( key, root, f );
    fprintf( f, "] %u\n", (unsigned int)((key->modif - ticks_1601_to_1970) / TICKS_PER_SEC) );
    if (key->class)
    {
        fputs( "#class=\"", f ); dump_strW( key->class, key->classlen, f, "\"\"" ); fputs( "\"\n", f );
    }
    if (ferror( f ) || ftello( f ) < 0 || ftello( f ) > 1024 * 1024) return 0;
    for (i = 0; i <= key->last_value; i++)
    {
        if (key->values[i].len > 1024 * 1024) return 0;
        dump_value( &key->values[i], f );
        if (ftello( f ) > 1024 * 1024) return 0;
    }
    for (i = 0; i <= key->last_subkey; i++)
        if (!e1s_write_keys( key->subkeys[i], root, f, depth + 1, count )) return 0;
    return !ferror( f );
}

static int e1s_write_hive( int fd, void *arg )
{
    struct e1s_hive *m = arg;
    FILE *f;
    unsigned int count = 0;
    int copy = dup( fd ), ret;
    if (copy < 0) return 0;
    if (!(f = fdopen( copy, "w" ))) { close( copy ); return 0; }
    fputs( "WINE REGISTRY Version 2\n", f );
    ret = e1s_write_keys( m->user, m->user, f, 0, &count );
    if (fclose( f )) ret = 0;
    return ret && e1s_profile_secure_hive( fd, &m->account );
}

static int e1s_save_hive( struct e1s_hive *m )
{
    enum wex_profile_status s;
    if (m->phase != E1S_LOADED && m->phase != E1S_UNLOADING)
        return e1s_deny( STATUS_REGISTRY_CORRUPT, "save-state" );
    s = wex_profile_save( config_dir_fd, &m->account, &m->storage, e1s_write_hive, m );
    if (!s) return 1;
    if (s == WEX_PROFILE_BUSY)
    {
        m->phase = E1S_LOADED; /* prefix-lock conflict occurred before a save transaction */
        return e1s_deny( STATUS_SHARING_VIOLATION, "save-prefix-lock" );
    }
    m->phase = E1S_POISONED; /* keep keys, lease, locks and all recovery data */
    return e1s_deny( s == WEX_PROFILE_RECOVERY ? STATUS_REGISTRY_CORRUPT :
                     s == WEX_PROFILE_IDENTITY || s == WEX_PROFILE_SUBSTITUTION ? STATUS_FILE_INVALID :
                     STATUS_REGISTRY_IO_FAILED, "durable-save" );
}

static int e1s_unload_hive( struct e1s_hive *m )
{
    struct e1s_hive **link;
    struct key *key, *next;
    struct sid sid = { SID_REVISION, 5, SECURITY_NT_AUTHORITY, {0} };
    unsigned int i;
    if (m->phase == E1S_LOADING) return e1s_deny( STATUS_DEVICE_NOT_READY, "unload-loading" );
    if (m->phase == E1S_UNLOADING) return e1s_deny( STATUS_DELETE_PENDING, "unload-unloading" );
    if (m->phase != E1S_LOADED) return e1s_deny( STATUS_REGISTRY_CORRUPT, "unload-recovery" );
    /* A later logon whose setup failed gives only its own lease back: the hive
     * returns to the committed state of the earlier users and stays loaded
     * while they live. */
    if (m->joined && m->storage.reserved.state == 1) m->storage.reserved.state = 2;
    memcpy( sid.sub_auth, m->account.sid, sizeof(m->account.sid) );
    if (process_e1s_profile_busy( &sid ))
    {
        return e1s_deny( STATUS_RESOURCE_IN_USE, "unload-live-user" );
    }
    LIST_FOR_EACH_ENTRY( key, &e1s_keys, struct key, e1s_entry )
        if (key->e1s_sd == m->sd && (key->obj.handle_count || !list_empty( &key->notify_list ) ||
                                    !list_empty( &key->obj.wait_queue )))
            return e1s_deny( STATUS_CANNOT_DELETE, "unload-open-key" );
    /* Check save registration before any I/O or destructive step. */
    for (i = 3; i < save_branch_count; i++) if (save_branch_info[i].managed == m) break;
    if (i == save_branch_count || save_branch_info[i].key != m->user || !list_empty( &save_branch_info[i].tombstones ))
        return e1s_deny( STATUS_REGISTRY_CORRUPT, "unload-save-registration" );
    m->phase = E1S_UNLOADING;
    if (!e1s_save_hive( m )) return 0;
    /* Synchronous server dispatch is the load/unload lock. No client request,
     * token assignment or new-process publication interleaves this commit. */
    m->generation->generation++;
    LIST_FOR_EACH_ENTRY_SAFE( key, next, &e1s_keys, struct key, e1s_entry )
        if (key->e1s_sd == m->sd)
        {
            key->e1s_sd = NULL; key->e1s_view = NULL; key->flags |= KEY_DELETED;
            list_remove( &key->e1s_entry ); list_init( &key->e1s_entry );
        }
    release_object( save_branch_info[i].key );
    if (i != --save_branch_count) save_branch_info[i] = save_branch_info[save_branch_count];
    list_init( &save_branch_info[i].tombstones ); /* managed lists are always empty */
    memset( &save_branch_info[save_branch_count], 0, sizeof(save_branch_info[0]) );
    unlink_named_object( &m->alias->obj );
    unlink_named_object( &m->user->obj );
    release_object( m->alias ); release_object( m->classes ); release_object( m->user );
    release_object( m->view ); release_object( m->machine_classes );
    wex_profile_storage_close( &m->storage );
    m->storage.reserved.state = 3;
    for (link = &e1s_hives; *link != m; link = &(*link)->next) ;
    *link = m->next; e1s_hive_count--;
    free( m->sd ); free( m );
    clear_error();
    return 1;
}

/* All checks precede the infallible state/accounting writes. Request dispatch
 * serializes this with thread resume, token assignment, death and unload. */
static int e1s_profile_commit_process( struct e1s_hive *m, struct token *issued,
                                      obj_handle_t context, obj_handle_t handle )
{
    struct process *child;
    const struct wex_account *account;
    unsigned int status;
    if (m->phase != E1S_LOADED)
        return e1s_deny( m->phase == E1S_LOADING ? STATUS_DEVICE_NOT_READY :
                         m->phase == E1S_UNLOADING ? STATUS_DELETE_PENDING : STATUS_REGISTRY_CORRUPT, "commit-phase" );
    if (!token_validate_e1s_commit( context, issued, m->storage.reserved.lease_id,
                                  m->storage.reserved.request_id, m->storage.reserved.server_gen )) return 0;
    if (m->storage.reserved.server_gen != e1s_server_generation)
        return e1s_deny( STATUS_REVISION_MISMATCH, "commit-server" );
    if (m->storage.reserved.state != 1) return e1s_deny( STATUS_ALREADY_COMMITTED, "commit-state" );
    if (!handle || handle == (obj_handle_t)~0u ||
        !(child = get_process_from_handle( handle, PROCESS_QUERY_LIMITED_INFORMATION )))
        return e1s_deny( STATUS_INVALID_CID, "commit-process-handle" );
    if ((status = process_e1s_profile_committable( child )))
        e1s_deny( status, status == STATUS_INVALID_CID ? "commit-foreign-server" :
                         status == STATUS_PROCESS_IS_TERMINATING ? "commit-dead" :
                         status == STATUS_RESOURCE_IN_USE ? "commit-process-bound" : "commit-not-suspended" );
    else if (!(account = child->token ? token_e1s_account( child->token ) : NULL) ||
             memcmp( account->incarnation, m->storage.reserved.account_key, 16 ) ||
             memcmp( account->sid, m->account.sid, sizeof(account->sid) ) ||
             !equal_sid( token_get_user_sid( child->token ), token_get_user_sid( issued ) ))
        e1s_deny( STATUS_NO_SUCH_USER, "commit-account" );
    else if (child->e1s_profile_issued != issued || !token_e1s_commit_identity( issued, child->token ))
        e1s_deny( STATUS_BAD_TOKEN_TYPE, "commit-issued-token" );
    else
    {
        /* Already in e1s_profile_processes since create_process, before its
         * first thread. Bind exactly that entry; do not insert it twice. */
        memcpy( child->e1s_profile_lease_id, m->storage.reserved.lease_id, 16 );
        memcpy( child->e1s_profile_sid, m->account.sid, sizeof(child->e1s_profile_sid) );
        m->storage.reserved.state = 2;
        m->idle_sweeps = 0;
        clear_error();
    }
    release_object( child );
    return !get_error();
}

/* Committed leases belong to server process tracking, not broker lifetime.
 * Retry last-user draining on the existing 30-second registry timer; real key
 * handles keep a hive loaded. Setup-only leases still require explicit rollback
 * release (S18), so a loaded profile cannot disappear during child setup. */
static void e1s_reap_hives(void)
{
    struct e1s_hive *m, *next;
    for (m = e1s_hives; m; m = next)
    {
        struct sid sid = { SID_REVISION, 5, SECURITY_NT_AUTHORITY, {0} };
        next = m->next;
        memcpy( sid.sub_auth, m->account.sid, sizeof(m->account.sid) );
        if (m->phase != E1S_LOADED || m->storage.reserved.state != 2) continue;
        if (process_e1s_profile_busy( &sid )) m->idle_sweeps = 0;
        else if (m->idle_sweeps++) e1s_unload_hive( m );
    }
}

/* the lab body of load_e1s_profile */
static void e1lab_load_e1s_profile( const struct load_e1s_profile_request *req, struct load_e1s_profile_reply *reply )
{
    struct token *token, *effective = current->token ? current->token : current->process->token;
    const struct wex_account *account;
    struct e1s_hive *m;
    struct key *key;
    struct wex_account ignored;
    if (req->operation == WEX_HKU_COMMIT_PROCESS)
    {
        /* Role first, even if the supplied token/context is absent or foreign. */
        if (!token_e1s_broker()) { e1s_deny( STATUS_ACCESS_DENIED, "commit-broker" ); return; }
        if (!req->token || req->access || get_req_data_size() != WEX_HKU_COMMIT_RECORD_SIZE || get_reply_max_size())
        { e1s_deny( STATUS_INVALID_PARAMETER, "commit-shape" ); return; }
        if (!(token = get_token_obj( current->process, req->token, TOKEN_QUERY )))
        { e1s_deny( STATUS_NO_TOKEN, "commit-token-handle" ); return; }
        account = token_e1s_account( token );
        if (!(m = e1s_find_hive( account ))) e1s_deny( STATUS_OBJECT_PATH_NOT_FOUND, "commit-unloaded" );
        else if (e1s_profile_commit_process( m, token, req->context, wex_token_u32( get_req_data() ) ))
            reply->managed = 1;
        release_object( token ); return;
    }
    if (req->operation == WEX_HKU_STORAGE_QUERY)
    {
        struct stat prefix;
        uint64_t gen = 0;
        enum wex_profile_status s;
        if (!token_e1s_broker()) { e1s_deny( STATUS_ACCESS_DENIED, "query-broker" ); return; }
        if (!req->token || !req->context || req->access || get_req_data_size() != WEX_HKU_RECORD_SIZE ||
            get_reply_max_size())
        { e1s_deny( STATUS_INVALID_PARAMETER, "query-shape" ); return; }
        if (!(token = get_token_obj( current->process, req->token, TOKEN_QUERY )))
        { e1s_deny( STATUS_NO_TOKEN, "query-token-handle" ); return; }
        if (!token_query_e1s_profile( req->context, token, get_req_data(), get_req_data_size(), &ignored ))
            e1s_deny( STATUS_ACCESS_DENIED, "query-context" );
        else if (fstat( config_dir_fd, &prefix ))
            e1s_deny( STATUS_ACCESS_DENIED, "query-prefix" );
        else if ((s = wex_profile_query_generation( config_dir_fd, prefix.st_dev, prefix.st_ino,
                                                    getuid(), getgid(), &ignored, &gen )))
        {
            set_error( s == WEX_PROFILE_ABSENT ? STATUS_OBJECT_PATH_NOT_FOUND :
                       s == WEX_PROFILE_BUSY ? STATUS_SHARING_VIOLATION :
                       s == WEX_PROFILE_RECOVERY ? STATUS_REGISTRY_CORRUPT : STATUS_ACCESS_DENIED );
            fprintf( stderr, "E1S14 deny=storage-query storage=%u status=%08x\n", s, get_error() );
        }
        else
        {
            reply->managed = 1;
            reply->generation = gen;
        }
        release_object( token ); return;
    }
    if (req->token || req->operation == WEX_HKU_LOAD) /* only read-side operations may default to the effective token */
    {
        if (!(token = get_token_obj( current->process, req->token, TOKEN_QUERY )))
        {
            if (req->operation == WEX_HKU_LOAD && token_e1s_broker())
                token_claim_e1s_profile( req->context, NULL, get_req_data(), get_req_data_size(), &ignored );
            return;
        }
    }
    else
    {
        token = (struct token *)grab_object( effective );
    }
    account = token_e1s_account( token );
    if (req->operation == WEX_HKU_QUERY && !req->context && !req->access && !get_req_data_size() && !get_reply_max_size())
    {
        reply->managed = !!account;
        if ((m = e1s_find_hive( account )) && m->phase == E1S_LOADED)
            reply->generation = m->generation->generation;
        release_object( token ); return;
    }
    set_error( STATUS_ACCESS_DENIED );
    if (req->operation == WEX_HKU_LOAD)
    {
        if (!token_e1s_broker()) goto done;
        if (get_reply_max_size())
        { token_claim_e1s_profile( req->context, NULL, get_req_data(), get_req_data_size(), &ignored ); goto done; }
        m = e1s_load_hive( token, req->context, get_req_data(), get_req_data_size(), req->access, &reply->hkey );
        if (!m) goto done;
        reply->managed = 1;
        reply->generation = m->generation->generation;
        /* Reloading a persisted hive reopens existing keys, which leaves the NAME_EXISTS warning set. */
        if (get_error() == STATUS_OBJECT_NAME_EXISTS) clear_error();
        goto done;
    }
    else
    {
        if (!account || get_reply_max_size()) goto done;
        if ((req->context && req->operation != WEX_HKU_UNLOAD_S14 && req->operation != WEX_HKU_TEST_TRANSFER) || get_req_data_size()) goto done;
        if (req->operation == WEX_HKU_OPEN || req->operation == WEX_HKU_UNLOAD_S14 ||
            req->operation == WEX_HKU_TEST_CRASH || req->operation == WEX_HKU_TEST_TRANSFER)
        { if (!token_e1s_broker()) goto done; }
        else if (req->operation != WEX_HKU_CURRENT && req->operation != WEX_HKU_CLASSES &&
                 req->operation != WEX_HKU_CACHE_CURRENT && req->operation != WEX_HKU_CACHE_CLASSES) goto done;
        else if (!token_e1s_broker() && (token_e1s_account( effective ) == NULL ||
                 memcmp( token_e1s_account( effective )->incarnation, account->incarnation, 16 ) ||
                 !equal_sid( token_get_user_sid( effective ), token_get_user_sid( token ) ))) goto done;
        if (!(m = e1s_find_hive( account ))) { set_error( STATUS_OBJECT_PATH_NOT_FOUND ); goto done; }
        if (req->operation == WEX_HKU_UNLOAD_S14)
        {
            if (req->access) goto done;
            if (m->phase != E1S_LOADED)
            {
                e1s_unload_hive( m ); /* state denial precedes any lease/cache dereference */
                goto done;
            }
            if (req->context)
            {
                struct key *profile = (struct key *)get_handle_obj( current->process, req->context, 0, &key_ops );
                int match = profile && profile->e1s_proxy_account && !profile->e1s_proxy_classes &&
                    profile->e1s_proxy_generation == m->generation->generation &&
                    e1s_find_hive( profile->e1s_proxy_account ) == m;
                if (profile) release_object( profile );
                if (!match) { e1s_deny( STATUS_INVALID_HANDLE, "unload-profile-handle" ); goto done; }
            }
            if (e1s_unload_hive( m )) reply->managed = 1;
            goto done;
        }
        if (req->operation == WEX_HKU_TEST_CRASH)
        {
#ifdef WEX_E1S14_CRASH_TEST
            if (m->phase == E1S_LOADED && req->access >= 1 && req->access <= 3)
            { m->storage.test_crash_step = req->access; reply->managed = 1; clear_error(); }
#else
            set_error( STATUS_NOT_SUPPORTED );
#endif
            goto done;
        }
        if (req->operation == WEX_HKU_TEST_TRANSFER) { set_error( STATUS_NOT_SUPPORTED ); goto done; }
    }
    if (m->phase != E1S_LOADED)
    {
        e1s_deny( m->phase == E1S_LOADING ? STATUS_DEVICE_NOT_READY :
                  m->phase == E1S_UNLOADING ? STATUS_DELETE_PENDING : STATUS_REGISTRY_CORRUPT, "open-state" );
        goto done;
    }
    key = req->operation == WEX_HKU_CLASSES ? m->view : m->user;
    clear_error();
    if (req->operation == WEX_HKU_OPEN || req->operation == WEX_HKU_CACHE_CURRENT || req->operation == WEX_HKU_CACHE_CLASSES)
        reply->hkey = e1s_cache_root( m, req->access, req->operation == WEX_HKU_CACHE_CLASSES );
    else reply->hkey = e1s_alloc_key_handle( key, req->access, 0 );
    if (reply->hkey) { reply->managed = 1; reply->generation = m->generation->generation; }
done:
    release_object( token );
}
#endif
