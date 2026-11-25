/*
 * Server-side registry management
 *
 * Copyright (C) 1999 Alexandre Julliard
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

/* To do:
 * - symbolic links
 */

#include "config.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_SYS_PERSONALITY_H
#include <sys/personality.h>
#endif

#include "ntstatus.h"
#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"
#include "process.h"
#include "unicode.h"
#include "security.h"
#include "sysrun.h"
#include "wine/wex_sysrun_conf.h"
#include "accounts.h"
#include "session0.h"
#include "e1s_token.h"
#include "e1s_profile.h"
#include "wine/wex_profile.h"
#include "wine/wex_session0.h"

#include "winternl.h"

struct notify
{
    struct list       entry;    /* entry in list of notifications */
    struct event    **events;   /* events to set when changing this key */
    unsigned int      event_count; /* number of events */
    int               subtree;  /* true if subtree notification */
    unsigned int      filter;   /* which events to notify on */
    obj_handle_t      hkey;     /* hkey associated with this notification */
    struct process   *process;  /* process in which the hkey is valid */
    unsigned int     *cookies;  /* system tier: a registry-only client's notifications, sent to its relay */
    unsigned int      cookie_count;
};

static const WCHAR key_name[] = {'K','e','y'};

struct type_descr key_type =
{
    { key_name, sizeof(key_name) },   /* name */
    KEY_ALL_ACCESS | SYNCHRONIZE,     /* valid_access */
    {                                 /* mapping */
        STANDARD_RIGHTS_READ | KEY_NOTIFY | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
        STANDARD_RIGHTS_WRITE | KEY_CREATE_SUB_KEY | KEY_SET_VALUE,
        STANDARD_RIGHTS_EXECUTE | KEY_CREATE_LINK | KEY_NOTIFY | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE,
        KEY_ALL_ACCESS
    },
};

/* a registry key */
struct key
{
    struct object     obj;         /* object header */
    WCHAR            *class;       /* key class */
    data_size_t       classlen;    /* length of class name */
    int               last_subkey; /* last in use subkey */
    int               nb_subkeys;  /* count of allocated subkeys */
    struct key      **subkeys;     /* subkeys array */
    struct key       *wow6432node; /* Wow6432Node subkey */
    int               last_value;  /* last in use value */
    int               nb_values;   /* count of allocated values in array */
    struct key_value *values;      /* values array */
    unsigned int      flags;       /* flags */
    timeout_t         modif;       /* last modification time */
    struct list       notify_list; /* list of notifications */
    struct security_descriptor *e1s_sd; /* borrowed managed default, SID-parametric */
    struct e1s_hive   *e1s_view;    /* private dynamic HKCR union cache */
    struct list       e1s_entry;   /* all managed keys, even deleted/detached ones */
    struct wex_account *e1s_proxy_account; /* cache proxy owns identity, no hive reference */
    uint64_t          e1s_proxy_generation;
    int               e1s_proxy_classes;
};

/* key flags */
#define KEY_VOLATILE 0x0001  /* key is volatile (not saved to disk) */
#define KEY_DELETED  0x0002  /* key has been deleted */
#define KEY_DIRTY    0x0004  /* key has been modified */
#define KEY_SYMLINK  0x0008  /* key is a symbolic link */
#define KEY_WOWSHARE 0x0010  /* key is a Wow64 shared key (used for Software\Classes) */
#define KEY_PREDEF   0x0020  /* key is marked as predefined */
#define KEY_E1S_ROOT 0x0040  /* S14 alone can unload/rename this root */
#define KEY_E1S_PERSIST 0x0080 /* managed persistence, separate from synthetic volatility */

#define OBJ_KEY_WOW64 0x100000 /* magic flag added to attributes for WoW64 redirection */

/* a key value */
struct key_value
{
    WCHAR            *name;    /* value name */
    unsigned short    namelen; /* length of value name */
    unsigned int      type;    /* value type */
    data_size_t       len;     /* value data length in bytes */
    void             *data;    /* pointer to value data */
};

#define MIN_SUBKEYS  8   /* min. number of allocated subkeys per key */
#define MIN_VALUES   8   /* min. number of allocated values per key */

#define MAX_NAME_LEN  256    /* max. length of a key name */
#define MAX_VALUE_LEN 16383  /* max. length of a value name */

/* the root of the registry tree */
static struct key *root_key;
static struct list e1s_keys = LIST_INIT(e1s_keys);
struct e1s_sid_generation
{
    struct e1s_sid_generation *next;
    unsigned int sid[5];
    uint64_t generation;
};
struct e1s_hive
{
    struct e1s_hive *next;
    struct wex_account account;
    struct wex_profile_storage storage;
    struct security_descriptor *sd;
    struct key *user, *classes, *view, *machine_classes, *alias;
    struct e1s_sid_generation *generation;
    enum { E1S_LOADING, E1S_LOADED, E1S_UNLOADING, E1S_POISONED } phase;
    unsigned int idle_sweeps;
    int refreshing;
    int joined;             /* a later logon took a lease on the already loaded hive */
};
static struct e1s_hive *e1s_hives;
static int e1s_strict_loading;
static int e1s_refresh_view( struct key * );
static struct key *e1s_view_user( struct key *, int create );
static struct e1s_hive *e1s_find_hive( const struct wex_account * );
static int e1s_save_hive( struct e1s_hive * );
static int e1s_key_ready( const struct key * );

static const timeout_t ticks_1601_to_1970 = (timeout_t)86400 * (369 * 365 + 89) * TICKS_PER_SEC;
static const timeout_t save_period = 30 * -TICKS_PER_SEC;  /* delay between periodic saves */
static struct timeout_user *save_timeout_user;  /* saving timer */
static enum prefix_type { PREFIX_UNKNOWN, PREFIX_32BIT, PREFIX_64BIT } prefix_type;

static const WCHAR wow6432node[] = {'W','o','w','6','4','3','2','N','o','d','e'};
static const WCHAR symlink_value[] = {'S','y','m','b','o','l','i','c','L','i','n','k','V','a','l','u','e'};
static const struct unicode_str symlink_str = { symlink_value, sizeof(symlink_value) };

static void set_periodic_save_timer(void);
static struct key_value *find_value( const struct key *key, struct unicode_str name, int *index );

static inline struct key *get_parent( const struct key *key );
static struct key *find_subkey( const struct key *key, struct unicode_str name, int *index );
static void delete_value( struct key *key, struct unicode_str name );
static int delete_key( struct key *key, int recurse );
static struct key *create_key_recursive( struct key *root, struct unicode_str name, timeout_t modif );
static void make_dirty( struct key *key );
static void touch_key_ex( struct key *key, unsigned int change, bool mark_dirty );

/* How a branch is stored (WineExplorer R4 of docs/SESSION0_SPLIT_REVIEW.md). Session 0 is the only writer of the
 * machine hives: a server that is not session 0 never writes a machine hive (the launcher without session 0 and
 * merge-on-save are retired), so there is no merging of a shared file any more. */
enum hive_storage
{
    HIVE_FILE,          /* the server's own file, saved by itself (a stock prefix; session 0's machine hives) */
    HIVE_MACHINE_COPY,  /* a machine hive that session 0 holds: the server's copy is never saved */
    HIVE_AGENT,         /* T4a, session 0: a user's hive, sent to the user's server to write */
    HIVE_MANAGED,       /* S14 (lab): a managed profile, saved through its descriptor */
};

/* information about where to save a registry branch */
struct save_branch_info
{
    struct key     *key;
    const char     *filename;
    enum hive_storage kind;
    bool            loaded;   /* read from its file at startup: clean until it changes */
    bool            warned;   /* HIVE_MACHINE_COPY outside the launcher: said once that changes are not saved */
    struct e1s_hive *managed; /* HIVE_MANAGED: the profile */
    void           *agent;    /* HIVE_AGENT: the user's server that saves this hive */
    struct security_descriptor *owner_sd; /* T4a: default descriptor of the hive's keys, for its user */
    struct security_descriptor *policy_sd; /* the same for the hive's policy trees: the user reads */
    struct key     *classes_alias;        /* T4a: \Registry\User\<SID>_Classes */
    unsigned int    save_pending;         /* T4a: saves sent to the agent that it has not yet answered */
};

#define MAX_SAVE_BRANCH_INFO (11 + 64) /* three startup branches, eight managed profiles, 64 users' hives (session 0) */
static int save_branch_count;
static struct save_branch_info save_branch_info[MAX_SAVE_BRANCH_INFO];

static bool registry_loaded = false;
static int load_nofollow;  /* loading a hive that its user wrote: links are not followed */

static struct save_branch_info *find_branch_info( const struct key *key )
{
    const struct key *k;
    int i;
    for (k = key; k; k = get_parent( k ))
    {
        for (i = 0; i < save_branch_count; i++)
        {
            if (save_branch_info[i].key == k)
                return &save_branch_info[i];
        }
    }
    return NULL;
}

static WCHAR *build_relative_key_path( const struct key *key, const struct key *base, data_size_t *ret_len )
{
    const struct key *k;
    data_size_t total_len = 0;
    WCHAR *path, *p;

    if (key == base)
    {
        *ret_len = 0;
        return NULL;
    }

    for (k = key; k && k != base; k = get_parent( k ))
    {
        if (total_len) total_len += sizeof(WCHAR);
        total_len += k->obj.name->len;
    }
    if (!k || !total_len)
    {
        *ret_len = 0;
        return NULL;
    }

    if (!(path = mem_alloc( total_len )))
    {
        *ret_len = 0;
        return NULL;
    }

    p = (WCHAR *)((char *)path + total_len);
    for (k = key; k && k != base; k = get_parent( k ))
    {
        p = (WCHAR *)((char *)p - k->obj.name->len);
        memcpy( p, k->obj.name->name, k->obj.name->len );
        if (p > path)
        {
            p = (WCHAR *)((char *)p - sizeof(WCHAR));
            *p = '\\';
        }
    }

    *ret_len = total_len;
    return path;
}

/* pathlen in bytes; the prefix matches up to a backslash or the end */
static bool match_ascii_prefix_ci( const WCHAR *path, data_size_t pathlen, const char *prefix )
{
    return wex_session_key_match( path, pathlen / sizeof(WCHAR), prefix ) != 0;
}

static struct key *find_subkey_by_path( struct key *root, const WCHAR *path, data_size_t pathlen )
{
    struct key *cur = root;
    while (pathlen && cur)
    {
        struct unicode_str elem;
        int index;
        elem.str = path;
        elem.len = get_path_element( path, pathlen );
        cur = find_subkey( cur, elem, &index );
        if (!cur) return NULL;
        if (elem.len < pathlen)
        {
            elem.len += sizeof(WCHAR);
            path += elem.len / sizeof(WCHAR);
            pathlen -= elem.len;
        }
        else break;
    }
    return cur;
}

/* The elevation helper (HKLM/Software/Wine/Elevation/Helper) is unset when the key or the value is absent,
 * or the value is an empty string. Any other state, including a wrong type or an odd length, counts as
 * configured: elevation enforcement fails closed. Same rule as is_elevation_helper_unset() in ntdll. */
int elevation_helper_unset(void)
{
    static const WCHAR key_path[] = {'M','a','c','h','i','n','e','\\','S','o','f','t','w','a','r','e','\\',
                                     'W','i','n','e','\\','E','l','e','v','a','t','i','o','n'};
    static const WCHAR value_name[] = {'H','e','l','p','e','r'};
    struct unicode_str name = { value_name, sizeof(value_name) };
    struct key_value *value;
    struct key *key;
    int index;

    if (!root_key || !(key = find_subkey_by_path( root_key, key_path, sizeof(key_path) ))) return TRUE;
    if (!(value = find_value( key, name, &index ))) return TRUE;
    if (value->type != REG_SZ && value->type != REG_EXPAND_SZ) return FALSE;
    if (value->len % sizeof(WCHAR)) return FALSE;
    return !value->len || !*(const WCHAR *)value->data;
}

/* Same administrator-group policy as accounts.c: an unsafe or invalid file
 * denies everyone; only a missing key in a valid file defaults to sudo. */
static const char *admin_group_name(void)
{
    struct wex_sysrun_conf conf;
    static char name[64];

    wex_sysrun_conf_load( "/etc/wineexplorer/sysrun.conf", &conf );
    return wex_sysrun_conf_admin_group( &conf, name, sizeof(name) ) ? name : NULL;
}

/* Whether the user of this server is in the administrator group of the group database. Decided once, when the
 * first process (the logon) gets its token, so a membership change applies at the next logon. A missing user or
 * group counts as no. */
int user_is_administrator(void)
{
    static int is_admin = -1;
    struct passwd *pw;
    struct group *gr;
    const char *group;
    gid_t *gids;
    int i, count = 32;

    if (is_admin != -1) return is_admin;
    is_admin = 0;
    /* a domain account in the domain's Domain Admins (RID 512, recorded by the root step at logon) is an
     * administrator as a member of admin_group is: the limited token at logon, elevation through the broker
     * (docs/DOMAIN_LOGON.md, WO 2, condition 3) */
    if (!(group = admin_group_name())) return 0;
    if (security_server_user_in_domain_group( 512 )) return is_admin = 1;
    if (!(pw = getpwuid( getuid() )) || !(gr = getgrnam( group ))) return 0;
    for (;;)
    {
        int n = count;
        if (!(gids = malloc( count * sizeof(*gids) ))) return 0;
        if (getgrouplist( pw->pw_name, pw->pw_gid, gids, &n ) != -1) { count = n; break; }
        free( gids );
        if (n <= count) return 0;
        count = n;
    }
    for (i = 0; i < count; i++) if (gids[i] == gr->gr_gid) is_admin = 1;
    free( gids );
    return is_admin;
}

unsigned int supported_machines_count = 0;
unsigned short supported_machines[8];
unsigned short native_machine = 0;

/* information about a file being loaded */
struct file_load_info
{
    const char *filename; /* input file name */
    FILE       *file;     /* input file */
    char       *buffer;   /* line buffer */
    int         len;      /* buffer length */
    int         line;     /* current input line */
    WCHAR      *tmp;      /* temp buffer to use while parsing input */
    size_t      tmplen;   /* length of temp buffer */
};


struct key_init_data
{
    unsigned int options;
    data_size_t  classlen;
    const WCHAR *class;
    timeout_t    modif;
};

static void key_dump( struct object *obj, int verbose );
static bool key_init( struct object *obj, const void *init_data );
static unsigned int key_map_access( struct object *obj, unsigned int access );
static struct security_descriptor *key_get_sd( struct object *obj );
static WCHAR *key_get_full_name( struct object *obj, data_size_t max, data_size_t *len );
static struct object *key_lookup_name( struct object *obj, struct unicode_str *name,
                                       unsigned int attr, struct object *root );
static int key_link_name( struct object *obj, struct object_name *name, struct object *parent );
static void key_unlink_name( struct object *obj, struct object_name *name );
static int key_close_handle( struct object *obj, struct process *process, obj_handle_t handle );
static void key_destroy( struct object *obj );

static const struct object_ops key_ops =
{
    .size          = sizeof(struct key),
    .type          = &key_type,
    .dump          = key_dump,
    .init          = key_init,
    .map_access    = key_map_access,
    .get_sd        = key_get_sd,
    .get_full_name = key_get_full_name,
    .lookup_name   = key_lookup_name,
    .link_name     = key_link_name,
    .unlink_name   = key_unlink_name,
    .close_handle  = key_close_handle,
    .destroy       = key_destroy,
};


static inline int is_wow6432node( const WCHAR *name, unsigned int len )
{
    len = get_path_element( name, len );
    return (len == sizeof(wow6432node) && !memicmp_strW( name, wow6432node, sizeof( wow6432node )));
}

static inline struct key *get_parent( const struct key *key )
{
    struct object *parent = key->obj.name->parent;

    if (!parent || parent->ops != &key_ops) return NULL;
    return (struct key *)parent;
}

/*
 * The registry text file format v2 used by this code is similar to the one
 * used by REGEDIT import/export functionality, with the following differences:
 * - strings and key names can contain \x escapes for Unicode
 * - key names use escapes too in order to support Unicode
 * - the modification time optionally follows the key name
 * - REG_EXPAND_SZ and REG_MULTI_SZ are saved as strings instead of hex
 */

/* dump the full path of a key */
static void dump_path( const struct key *key, const struct key *base, FILE *f )
{
    if (get_parent( key ) && get_parent( key ) != base)
    {
        dump_path( get_parent( key ), base, f );
        fprintf( f, "\\\\" );
    }
    dump_strW( key->obj.name->name, key->obj.name->len, f, "[]" );
}

/* dump a value to a text file */
static void dump_value( const struct key_value *value, FILE *f )
{
    unsigned int i, dw;
    int count;

    if (value->namelen)
    {
        fputc( '\"', f );
        count = 1 + dump_strW( value->name, value->namelen, f, "\"\"" );
        count += fprintf( f, "\"=" );
    }
    else count = fprintf( f, "@=" );

    switch(value->type)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ:
        /* only output properly terminated strings in string format */
        if (value->len < sizeof(WCHAR)) break;
        if (value->len % sizeof(WCHAR)) break;
        if (((WCHAR *)value->data)[value->len / sizeof(WCHAR) - 1]) break;
        if (value->type != REG_SZ) fprintf( f, "str(%x):", value->type );
        fputc( '\"', f );
        dump_strW( (WCHAR *)value->data, value->len - sizeof(WCHAR), f, "\"\"" );
        fprintf( f, "\"\n" );
        return;

    case REG_DWORD:
        if (value->len != sizeof(dw)) break;
        memcpy( &dw, value->data, sizeof(dw) );
        fprintf( f, "dword:%08x\n", dw );
        return;
    }

    if (value->type == REG_BINARY) count += fprintf( f, "hex:" );
    else count += fprintf( f, "hex(%x):", value->type );
    for (i = 0; i < value->len; i++)
    {
        count += fprintf( f, "%02x", *((unsigned char *)value->data + i) );
        if (i < value->len-1)
        {
            fputc( ',', f );
            if (++count > 76)
            {
                fprintf( f, "\\\n  " );
                count = 2;
            }
        }
    }
    fputc( '\n', f );
}

/* find the named child of a given key and return its index */
static struct key *find_subkey( const struct key *key, struct unicode_str name, int *index )
{
    int i, min, max, res;
    data_size_t len;

    min = 0;
    max = key->last_subkey;
    while (min <= max)
    {
        i = (min + max) / 2;
        len = min( key->subkeys[i]->obj.name->len, name.len );
        res = memicmp_strW( key->subkeys[i]->obj.name->name, name.str, len );
        if (!res) res = key->subkeys[i]->obj.name->len - name.len;
        if (!res)
        {
            *index = i;
            return key->subkeys[i];
        }
        if (res > 0) max = i - 1;
        else min = i + 1;
    }
    *index = min;  /* this is where we should insert it */
    return NULL;
}

/* try to grow the array of subkeys; return 1 if OK, 0 on error */
static int grow_subkeys( struct key *key )
{
    struct key **new_subkeys;
    int nb_subkeys;

    if (key->nb_subkeys)
    {
        nb_subkeys = key->nb_subkeys + (key->nb_subkeys / 2);  /* grow by 50% */
        if (!(new_subkeys = realloc( key->subkeys, nb_subkeys * sizeof(*new_subkeys) )))
        {
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
    }
    else
    {
        nb_subkeys = MIN_SUBKEYS;
        if (!(new_subkeys = mem_alloc( nb_subkeys * sizeof(*new_subkeys) ))) return 0;
    }
    key->subkeys    = new_subkeys;
    key->nb_subkeys = nb_subkeys;
    return 1;
}

/* save a registry and all its subkeys to a text file */
static void save_subkeys( const struct key *key, const struct key *base, FILE *f )
{
    int i;

    if (key->flags & KEY_VOLATILE) return;
    /* save key if it has either some values or no subkeys, or needs special options */
    /* keys with no values but subkeys are saved implicitly by saving the subkeys */
    if ((key->last_value >= 0) || (key->last_subkey == -1) || key->class || (key->flags & KEY_SYMLINK) ||
        (session0_server && key->obj.sd))
    {
        fprintf( f, "\n[" );
        if (key != base) dump_path( key, base, f );
        fprintf( f, "] %u\n", (unsigned int)((key->modif - ticks_1601_to_1970) / TICKS_PER_SEC) );
        fprintf( f, "#time=%x%08x\n", (unsigned int)(key->modif >> 32), (unsigned int)key->modif );
        if (key->class)
        {
            fprintf( f, "#class=\"" );
            dump_strW( key->class, key->classlen, f, "\"\"" );
            fprintf( f, "\"\n" );
        }
        /* the system tier's stored key permission */
        if (session0_server && key->obj.sd)
        {
            const struct security_descriptor *sd = key->obj.sd;
            const unsigned char *bytes = (const unsigned char *)sd;
            data_size_t size = sizeof(*sd) + sd->owner_len + sd->group_len + sd->sacl_len + sd->dacl_len;
            data_size_t n;
            fprintf( f, "#sd=" );
            for (n = 0; n < size; n++) fprintf( f, "%02x", (unsigned int)bytes[n] );
            fprintf( f, "\n" );
        }
        if (key->flags & KEY_SYMLINK) fputs( "#link\n", f );
        for (i = 0; i <= key->last_value; i++) dump_value( &key->values[i], f );
    }
    for (i = 0; i <= key->last_subkey; i++) save_subkeys( key->subkeys[i], base, f );
}

static void dump_operation( const struct key *key, const struct key_value *value, const char *op )
{
    fprintf( stderr, "%s key ", op );
    if (key) dump_path( key, NULL, stderr );
    else fprintf( stderr, "ERROR" );
    if (value)
    {
        fprintf( stderr, " value ");
        dump_value( value, stderr );
    }
    else fprintf( stderr, "\n" );
}

static void key_dump( struct object *obj, int verbose )
{
    struct key *key = (struct key *)obj;
    assert( obj->ops == &key_ops );
    fprintf( stderr, "Key flags=%x ", key->flags );
    dump_path( key, NULL, stderr );
    fprintf( stderr, "\n" );
}

static bool key_init( struct object *obj, const void *init_data )
{
    struct key *key = (struct key *)obj;
    struct key *parent = get_parent( key );
    const struct key_init_data *data = init_data;

    key->class       = NULL;
    key->flags       = 0;
    key->last_subkey = -1;
    key->nb_subkeys  = 0;
    key->subkeys     = NULL;
    key->wow6432node = NULL;
    key->nb_values   = 0;
    key->last_value  = -1;
    key->values      = NULL;
    key->modif       = data->modif;
    key->e1s_sd      = parent ? parent->e1s_sd : NULL;
    key->e1s_view    = parent ? parent->e1s_view : NULL;
    list_init( &key->e1s_entry );
    key->e1s_proxy_account = NULL;
    key->e1s_proxy_generation = 0;
    key->e1s_proxy_classes = 0;
    if (key->e1s_sd)
    {
        if (!(data->options & REG_OPTION_VOLATILE)) key->flags |= KEY_E1S_PERSIST;
    }
    list_init( &key->notify_list );

    if ((key->classlen = (data->classlen / sizeof(WCHAR)) * sizeof(WCHAR)) &&
        !(key->class = memdup( data->class, key->classlen ))) return false;

    if (data->options & REG_OPTION_CREATE_LINK) key->flags |= KEY_SYMLINK;
    if (data->options & REG_OPTION_VOLATILE) key->flags |= KEY_VOLATILE;
    else if (parent && parent->flags & KEY_VOLATILE)
    {
        if (key->e1s_sd) key->flags |= KEY_VOLATILE;
        else
        {
            set_error( STATUS_CHILD_MUST_BE_VOLATILE );
            return false;
        }
    }
    else if (!registry_loaded)
    {
        key->flags |= KEY_DIRTY;
    }
    else make_dirty( key );

    if (key->e1s_sd) list_add_tail( &e1s_keys, &key->e1s_entry );
    return true;
}

/* notify waiter and maybe delete the notification */
static void do_notification( struct key *key, struct notify *notify, int del )
{
    unsigned int i;

    for (i = 0; i < notify->event_count; ++i)
    {
        set_event( notify->events[i] );
        release_object( notify->events[i] );
    }
    free( notify->events );
    notify->events = NULL;
    notify->event_count = 0;
    for (i = 0; i < notify->cookie_count; ++i) session0_relay_notify( notify->process, notify->cookies[i] );
    if (notify->cookie_count) notify->process->wex_relay_cookies -= notify->cookie_count;
    free( notify->cookies );
    notify->cookies = NULL;
    notify->cookie_count = 0;

    if (del)
    {
        list_remove( &notify->entry );
        free( notify );
    }
}

static inline struct notify *find_notify( struct key *key, struct process *process, obj_handle_t hkey )
{
    struct notify *notify;

    LIST_FOR_EACH_ENTRY( notify, &key->notify_list, struct notify, entry )
    {
        if (notify->process == process && notify->hkey == hkey) return notify;
    }
    return NULL;
}

static unsigned int key_map_access( struct object *obj, unsigned int access )
{
    access = default_map_access( obj, access );
    /* filter the WOW64 masks, as they aren't real access bits */
    return access & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
}

struct default_ace { const struct sid *sid; unsigned char flags; unsigned int mask; };

static struct security_descriptor *key_alloc_default_sd( const struct default_ace *aces, unsigned int count )
{
    struct security_descriptor *sd;
    struct acl *dacl;
    struct ace *ace;
    struct sid *sid;
    size_t admins_sid_len = sid_len( &builtin_admins_sid );
    size_t dacl_len = sizeof(*dacl);
    unsigned int i;

    for (i = 0; i < count; i++) dacl_len += sizeof(*ace) + sid_len( aces[i].sid );

    sd = mem_alloc( sizeof(*sd) + 2 * admins_sid_len + dacl_len );
    sd->control   = SE_DACL_PRESENT;
    sd->owner_len = admins_sid_len;
    sd->group_len = admins_sid_len;
    sd->sacl_len  = 0;
    sd->dacl_len  = dacl_len;
    sid = (struct sid *)(sd + 1);
    sid = copy_sid( sid, &builtin_admins_sid );
    copy_sid( sid, &builtin_admins_sid );

    dacl = (struct acl *)((char *)(sd + 1) + 2 * admins_sid_len);
    dacl->revision = ACL_REVISION;
    dacl->pad1     = 0;
    dacl->size     = dacl_len;
    dacl->count    = count;
    dacl->pad2     = 0;
    for (i = 0, ace = ace_first( dacl ); i < count; i++, ace = ace_next( ace ))
        set_ace( ace, aces[i].sid, ACCESS_ALLOWED_ACE_TYPE, aces[i].flags, aces[i].mask );
    return sd;
}

static WCHAR *format_user_registry_path( const struct sid *sid, struct unicode_str *path );

/* is the key in the \Registry\User tree (HKCU, HKU\.Default)? */
static int key_in_user_tree( struct key *key )
{
    static const WCHAR user[] = {'U','s','e','r'};
    static const WCHAR classes[] = {'_','C','l','a','s','s','e','s'};
    static WCHAR *own_name;  /* "S-1-5-21-...", the name of the token user's key below User */
    static data_size_t own_len;
    const WCHAR *name;
    data_size_t len;
    struct key *parent, *sub = NULL;

    while ((parent = get_parent( key )) && parent != root_key) { sub = key; key = parent; }
    if (!parent || !(name = get_object_name( &key->obj, &len ))) return FALSE;
    if (len != sizeof(user) || memicmp_strW( name, user, sizeof(user) )) return FALSE;
    /* With a system role server only the token user's own subtree and its _Classes belong to the user: HKU\S-1-5-18,
     * S-1-5-19, S-1-5-20 and .DEFAULT get the machine default descriptor. */
    if (!sysrun_active || !elevation_enforced() || !sub) return TRUE;
    if (!own_name)
    {
        struct unicode_str path;
        WCHAR *full = format_user_registry_path( security_server_user_sid(), &path );
        own_len = path.len - 5 * sizeof(WCHAR);  /* skip "User\\" */
        own_name = memdup( full + 5, own_len );
        free( full );
    }
    if (!(name = get_object_name( &sub->obj, &len ))) return FALSE;
    if (len == own_len && !memicmp_strW( name, own_name, own_len )) return TRUE;
    return len == own_len + sizeof(classes) && !memicmp_strW( name, own_name, own_len ) &&
           !memicmp_strW( name + own_len / sizeof(WCHAR), classes, sizeof(classes) );
}

/* is the key \Registry\User (HKEY_USERS) itself? */
static int key_is_user_root( struct key *key )
{
    static const WCHAR user[] = {'U','s','e','r'};
    const WCHAR *name;
    data_size_t len;

    if (get_parent( key ) != root_key || !(name = get_object_name( &key->obj, &len ))) return FALSE;
    return len == sizeof(user) && !memicmp_strW( name, user, sizeof(user) );
}

/* The service accounts' own profiles below User (S-1-5-19 LocalService, S-1-5-20 NetworkService, and their _Classes):
 * Windows gives each account full control of its own HKCU. Returns the account's SID, NULL for other keys. */
/* RESTRICTED: a restricted token (a sandbox such as Chromium's GPU and utility processes) must also pass a second
 * check against its restricting SIDs; on Windows a user's hive grants this SID read access, so such a process can
 * read its user's settings (win32u needs HKCU\Software\Wine for the display configuration) */
static const struct sid restricted_code_sid = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_RESTRICTED_CODE_RID } };

static const struct sid *key_service_profile_sid( struct key *key )
{
    static const struct sid local_service = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_LOCAL_SERVICE_RID } };
    static const struct sid network_service = { SID_REVISION, 1, SECURITY_NT_AUTHORITY, { SECURITY_NETWORK_SERVICE_RID } };
    static const WCHAR user[] = {'U','s','e','r'};
    static const WCHAR ls[] = {'S','-','1','-','5','-','1','9'};
    static const WCHAR ns[] = {'S','-','1','-','5','-','2','0'};
    static const WCHAR classes[] = {'_','C','l','a','s','s','e','s'};
    const WCHAR *name;
    data_size_t len;
    struct key *parent, *sub = NULL;

    while ((parent = get_parent( key )) && parent != root_key) { sub = key; key = parent; }
    if (!parent || !sub || !(name = get_object_name( &key->obj, &len ))) return NULL;
    if (len != sizeof(user) || memicmp_strW( name, user, sizeof(user) )) return NULL;
    if (!(name = get_object_name( &sub->obj, &len ))) return NULL;
    if (len == sizeof(ls) + sizeof(classes) && !memicmp_strW( name + sizeof(ls) / sizeof(WCHAR), classes, sizeof(classes) ))
        len = sizeof(ls);
    if (len != sizeof(ls)) return NULL;
    if (!memicmp_strW( name, ls, sizeof(ls) )) return &local_service;
    if (!memicmp_strW( name, ns, sizeof(ns) )) return &network_service;
    return NULL;
}

/* Keys under HKLM that win32u's prepare_devices rewrites on every GUI process start (the display device
 * cache). Windows has SYSTEM components do this; here every process does, so while elevation is enforced these
 * keys and their subkeys default to Users full control. Deviation from Windows, see STOCK_CHANGES.md. */
static const char * const display_cache_paths[] =
{
    "System\\ControlSet001\\Enum\\DISPLAY",
    "System\\ControlSet001\\Enum\\PCI",
    "System\\ControlSet001\\Control\\Class\\{4D36E968-E325-11CE-BFC1-08002BE10318}",
    "System\\ControlSet001\\Control\\Class\\{4D36E96E-E325-11CE-BFC1-08002BE10318}",
    "System\\ControlSet001\\Control\\Video",
    "System\\ControlSet001\\Control\\DeviceClasses",
    "System\\ControlSet001\\Hardware Profiles\\Current",
    "HARDWARE\\DEVICEMAP",
    "Software\\Microsoft\\DirectX",
};

/* Written by services.exe and wineboot --init. With the system role active they run as SYSTEM and need no grant;
 * without it (a user-writable prefix, no launcher) they run limited, so these two stay Users-writable. */
static const char * const boot_state_paths[] =
{
    "System\\ControlSet001\\Control\\ServiceCurrent",
    "System\\ControlSet001\\Control\\ComputerName\\ActiveComputerName",
};

/* is path a key above an entry of display_cache_paths? */
static bool is_display_cache_ancestor( const WCHAR *path, data_size_t len, const char *entry )
{
    size_t i, wlen = len / sizeof(WCHAR);

    if (strlen( entry ) <= wlen || entry[wlen] != '\\') return false;
    for (i = 0; i < wlen; i++)
        if ((path[i] < 128 ? tolower( path[i] ) : path[i]) != tolower( (unsigned char)entry[i] )) return false;
    return true;
}

/* Is the key, or with new_name the subkey of it that is about to be created, part of the display cache?
 * A new subkey also counts when it is above a cache key, because the cache path has to be created. */
static int key_in_display_cache( struct key *key, const struct unicode_str *new_name )
{
    static const WCHAR machine[] = {'M','a','c','h','i','n','e'};
    struct key *top = key, *parent;
    const WCHAR *name;
    data_size_t len, total;
    WCHAR *path, *full;
    unsigned int i;
    int ret = FALSE;

    while ((parent = get_parent( top )) && parent != root_key) top = parent;
    if (!parent || (top == key && !new_name) || !(name = get_object_name( &top->obj, &len ))) return FALSE;
    if (len != sizeof(machine) || memicmp_strW( name, machine, sizeof(machine) )) return FALSE;

    path = build_relative_key_path( key, top, &len );  /* NULL for the Machine key itself */
    total = len + (new_name ? new_name->len + (len ? sizeof(WCHAR) : 0) : 0);
    if (!(full = mem_alloc( total + sizeof(WCHAR) ))) goto done;
    if (len) memcpy( full, path, len );
    if (new_name)
    {
        if (len) full[len / sizeof(WCHAR)] = '\\';
        memcpy( (char *)full + total - new_name->len, new_name->str, new_name->len );
    }
    for (i = 0; i < ARRAY_SIZE(display_cache_paths) && !ret; i++)
        ret = match_ascii_prefix_ci( full, total, display_cache_paths[i] ) ||
              (new_name && is_display_cache_ancestor( full, total, display_cache_paths[i] ));
    for (i = 0; i < ARRAY_SIZE(boot_state_paths) && !ret && !sysrun_active; i++)
        ret = match_ascii_prefix_ci( full, total, boot_state_paths[i] ) ||
              (new_name && is_display_cache_ancestor( full, total, boot_state_paths[i] ));
    free( full );
done:
    free( path );
    return ret;
}

/* Group Policy (docs/GROUP_POLICY.md): the policy trees of a user's hive held by session 0. Only the system and
 * administrators write them, as on Windows; the path decides, because a new key inherits no descriptor here. */
static const char * const policy_tree_paths[] =
{
    "Software\\Policies",
    "Software\\Microsoft\\Windows\\CurrentVersion\\Policies",
};

/* is the key the root of a policy tree of the hive, or below one (below: 1), or above one (below: 0)? */
static int key_at_policy_tree( const struct key *key, const struct save_branch_info *info, int below )
{
    data_size_t len;
    unsigned int i;
    int ret = 0;
    WCHAR *path = build_relative_key_path( key, info->key, &len );

    if (!path) return 0;
    for (i = 0; i < ARRAY_SIZE(policy_tree_paths) && !ret; i++)
    {
        if (below) ret = match_ascii_prefix_ci( path, len, policy_tree_paths[i] );
        else ret = is_display_cache_ancestor( path, len, policy_tree_paths[i] );
    }
    free( path );
    return ret;
}

/* "Volatile Environment" belongs to one logon: wineboot makes it, as a volatile key, for the user who logs on.
 * A hive file that has it (a copy of a template prefix, or a file written before the key was volatile) would
 * keep it for ever, with another account's name and paths: a key that is loaded is an ordinary key, and making
 * it again as volatile does not change that. So it is dropped when a user's hive is loaded. */
static void drop_saved_volatile_environment( struct key *hive )
{
    static const WCHAR nameW[] = {'V','o','l','a','t','i','l','e',' ','E','n','v','i','r','o','n','m','e','n','t'};
    struct unicode_str name = { nameW, sizeof(nameW) };
    struct key *key;
    int index;

    if ((key = find_subkey( hive, name, &index ))) delete_key( key, 1 );
    clear_error();
}

/* may the caller write the hive's policy trees? Asked of the first tree's root, which always exists. */
static int policy_write_allowed( struct key *hive )
{
    WCHAR path[64];
    unsigned int access = KEY_SET_VALUE;
    size_t j, len = strlen( policy_tree_paths[0] );
    struct key *key;
    int ret;

    for (j = 0; j < len; j++) path[j] = (unsigned char)policy_tree_paths[0][j];
    if (!(key = find_subkey_by_path( hive, path, len * sizeof(WCHAR) ))) return 0;
    ret = check_object_access( NULL, &key->obj, &access );
    clear_error();
    return ret;
}

/* The hive file is the user's own, so what it says in the policy trees is not policy: a hive that session 0
 * loads gets the trees empty, and the Group Policy service (wexpolicy) writes the user's policy into them. The
 * roots always exist; a create below them then meets a parent with the policy descriptor. */
static void reset_policy_trees( struct key *hive )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(policy_tree_paths); i++)
    {
        WCHAR path[64];
        struct unicode_str name;
        struct key *key;
        size_t j, len = strlen( policy_tree_paths[i] );

        for (j = 0; j < len; j++) path[j] = (unsigned char)policy_tree_paths[i][j];
        /* a key above the tree that the file made a link would lead the readers of the policy elsewhere: such a
         * key goes, with all below it, and is made again as a plain key */
        for (key = hive, name.str = path, name.len = len * sizeof(WCHAR); key && name.len; )
        {
            struct unicode_str part = name;
            int index;

            part.len = get_path_element( name.str, name.len );
            if (!(key = find_subkey( key, part, &index ))) break;
            if (key->flags & KEY_SYMLINK)
            {
                delete_key( key, 1 );
                break;
            }
            if (part.len < name.len) part.len += sizeof(WCHAR);
            name.str += part.len / sizeof(WCHAR);
            name.len -= part.len;
        }
        if ((key = find_subkey_by_path( hive, path, len * sizeof(WCHAR) ))) delete_key( key, 1 );
        name.str = path;
        name.len = len * sizeof(WCHAR);
        if ((key = create_key_recursive( hive, name, current_time ))) release_object( key );
        clear_error();
    }
}

/* Default descriptors for keys that have none. A limited token has Administrators deny-only, so the
 * descriptors have to name the user (HKCU) and Users (everywhere else) instead of relying on Administrators:
 * the user and SYSTEM get full control of the user tree; elsewhere Users can read the key itself and
 * Administrators and SYSTEM have full control. */
static struct security_descriptor *key_get_sd( struct object *obj )
{
    static struct security_descriptor *key_default_sd, *key_user_sd, *key_display_sd, *key_ls_sd, *key_ns_sd, *key_users_sd;
    const struct sid *service_sid;

    if (obj->sd) return obj->sd;
    if (((struct key *)obj)->e1s_sd) return ((struct key *)obj)->e1s_sd;
    if (session0_server)  /* system tier T4a: a user's hive held by session 0 belongs to that user */
    {
        struct save_branch_info *info = find_branch_info( (struct key *)obj );
        if (info && info->owner_sd)
            return info->policy_sd && key_at_policy_tree( (struct key *)obj, info, 1 ) ? info->policy_sd : info->owner_sd;
    }

    if (key_is_user_root( (struct key *)obj ))
    {
        /* HKEY_USERS itself: as on Windows every user can read it (the SIDs of the profiles); nobody but the system
         * creates keys in it, and each profile has its own descriptor */
        if (!key_users_sd)
        {
            const struct default_ace aces[] =
            {
                { &builtin_users_sid,         0, KEY_READ },
                { security_server_user_sid(), CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &local_system_sid,          CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &builtin_admins_sid,        CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
            };
            key_users_sd = key_alloc_default_sd( aces, ARRAY_SIZE(aces) );
        }
        return key_users_sd;
    }

    if ((service_sid = key_service_profile_sid( (struct key *)obj )))
    {
        struct security_descriptor **sd = (service_sid->sub_auth[0] == SECURITY_LOCAL_SERVICE_RID) ? &key_ls_sd : &key_ns_sd;

        if (!*sd)
        {
            const struct default_ace aces[] =
            {
                { service_sid,                CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &local_system_sid,          CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &builtin_admins_sid,        CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
            };
            *sd = key_alloc_default_sd( aces, ARRAY_SIZE(aces) );
        }
        return *sd;
    }

    if (key_in_user_tree( (struct key *)obj ))
    {
        if (!key_user_sd)
        {
            const struct default_ace aces[] =
            {
                { security_server_user_sid(), CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &local_system_sid,  CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &builtin_admins_sid, CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &restricted_code_sid, CONTAINER_INHERIT_ACE, KEY_READ },
            };
            key_user_sd = key_alloc_default_sd( aces, ARRAY_SIZE(aces) );
        }
        return key_user_sd;
    }

    if (elevation_enforced() && key_in_display_cache( (struct key *)obj, NULL ))
    {
        if (!key_display_sd)
        {
            const struct default_ace aces[] =
            {
                { &builtin_users_sid,  CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &local_system_sid,   CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &builtin_admins_sid, CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
            };
            key_display_sd = key_alloc_default_sd( aces, ARRAY_SIZE(aces) );
        }
        return key_display_sd;
    }

    if (!key_default_sd)
    {
        /* as HKLM\SOFTWARE on Windows: the creator of a subkey, SYSTEM and Administrators get full control of the keys
         * below too. It matters once a program reads this DACL and sets it on a key with one ACE more (an installer's
         * "add an ACE"): the subkeys of that key then inherit what is here, and SYSTEM and Administrators must keep
         * their access to them (Omaha's ClientStateMedium and PersistedPings, WebView2's setup) */
        static const struct sid creator_owner = { SID_REVISION, 1, SECURITY_CREATOR_SID_AUTHORITY,
                                                  { SECURITY_CREATOR_OWNER_RID } };
        const struct default_ace aces[] =
        {
            { &creator_owner,      INHERIT_ONLY_ACE | CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
            { &builtin_users_sid,  0, KEY_READ },
            { &local_system_sid,   CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
            { &builtin_admins_sid, CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
            /* the old inherit-only ACE stays last: advapi32 security.c looks for the last Users ACE */
            { &builtin_users_sid,  INHERIT_ONLY_ACE | CONTAINER_INHERIT_ACE, GENERIC_READ },
        };
        key_default_sd = key_alloc_default_sd( aces, ARRAY_SIZE(aces) );
    }
    return key_default_sd;
}

static WCHAR *key_get_full_name( struct object *obj, data_size_t max, data_size_t *ret_len )
{
    struct key *key = (struct key *) obj;

    if (key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        return NULL;
    }
    return default_get_full_name( obj, max, ret_len );
}

static struct object *key_lookup_name( struct object *obj, struct unicode_str *name,
                                       unsigned int attr, struct object *root )
{
    struct key *found, *key = (struct key *)obj;
    struct unicode_str tmp;
    data_size_t next;
    int index;

    assert( obj->ops == &key_ops );

    if (!name) return NULL;  /* open the key itself */
    if (!e1s_key_ready( key )) return NULL;
    if (key->e1s_view && !e1s_refresh_view( key )) return NULL;

    if (key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        return NULL;
    }

    if (key->flags & KEY_SYMLINK)
    {
        struct unicode_str name_left;
        struct key_value *value;

        if (!name->len && (attr & OBJ_OPENLINK)) return NULL;
        /* system tier: a user's hive loaded in session 0 never reaches outside itself through its own links */
        if (load_nofollow)
        {
            if (!name->len) return NULL;
            set_error( STATUS_ACCESS_DENIED );
            return NULL;
        }

        if (!(value = find_value( key, symlink_str, &index )) ||
            value->len < sizeof(WCHAR) || *(WCHAR *)value->data != '\\')
        {
            set_error( STATUS_OBJECT_NAME_NOT_FOUND );
            return NULL;
        }
        tmp.str = value->data;
        tmp.len = (value->len / sizeof(WCHAR)) * sizeof(WCHAR);
        if ((obj = lookup_named_object( NULL, tmp, OBJ_CASE_INSENSITIVE, &name_left )))
        {
            if (!name->len) *name = name_left;  /* symlink destination can be created if missing */
            else if (name_left.len)  /* symlink must have been resolved completely */
            {
                release_object( obj );
                obj = NULL;
                set_error( STATUS_OBJECT_NAME_NOT_FOUND );
            }
        }

        key = (struct key *)obj;
        if (key && (key->flags & KEY_WOWSHARE) && (attr & OBJ_KEY_WOW64) && !name->str)
        {
            key = get_parent( key );
            release_object( obj );
            return grab_object( key );
        }

        return obj;
    }

    if (!name->str) return NULL;

    tmp.str = name->str;
    tmp.len = get_path_element( name->str, name->len );

    if (tmp.len > MAX_NAME_LEN * sizeof(WCHAR))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return 0;
    }

    /* skip trailing backslashes */
    for (next = tmp.len; next < name->len; next += sizeof(WCHAR))
        if (name->str[next / sizeof(WCHAR)] != '\\') break;

    if (!(found = find_subkey( key, tmp, &index )))
    {
        if ((key->flags & KEY_WOWSHARE) && (attr & OBJ_KEY_WOW64))
        {
            /* try in the 64-bit parent */
            key = get_parent( key );
            if (!(found = find_subkey( key, tmp, &index ))) return grab_object( key );
        }
    }

    if (!found)
    {
        if (next < name->len)  /* path still has elements */
            set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        else  /* only trailing backslashes */
            name->len = tmp.len;
        return NULL;
    }

    if (next < name->len)  /* move to the next element */
    {
        name->str += next / sizeof(WCHAR);
        name->len -= next;
        if ((attr & OBJ_KEY_WOW64) && found->wow6432node && !is_wow6432node( name->str, name->len ))
            found = found->wow6432node;
    }
    else
    {
        name->str = NULL;
        name->len = 0;
    }
    return grab_object( found );
}

static int key_link_name( struct object *obj, struct object_name *name, struct object *parent )
{
    struct key *key = (struct key *)obj;
    struct key *parent_key = (struct key *)parent;
    struct unicode_str tmp;
    int i, index;

    if (parent->ops != &key_ops)
    {
        /* only the root key can be created inside a normal directory */
        if (!root_key) return directory_link_name( obj, name, parent );
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        return 0;
    }

    if (parent_key->last_subkey + 1 == parent_key->nb_subkeys)
    {
        /* need to grow the array */
        if (!grow_subkeys( parent_key )) return 0;
    }
    tmp.str = name->name;
    tmp.len = name->len;
    find_subkey( parent_key, tmp, &index );

    for (i = ++parent_key->last_subkey; i > index; i--)
        parent_key->subkeys[i] = parent_key->subkeys[i - 1];
    parent_key->subkeys[index] = (struct key *)grab_object( key );
    if (is_wow6432node( name->name, name->len ) &&
        !is_wow6432node( parent_key->obj.name->name, parent_key->obj.name->len ))
        parent_key->wow6432node = key;
    name->parent = parent;
    return 1;
}

static void key_unlink_name( struct object *obj, struct object_name *name )
{
    struct key *key = (struct key *)obj;
    struct key *parent = (struct key *)name->parent;
    int i, nb_subkeys;

    if (!parent) return;

    if (parent->obj.ops != &key_ops)
    {
        unlink_name( name );
        return;
    }

    for (i = 0; i <= parent->last_subkey; i++) if (parent->subkeys[i] == key) break;
    assert( i <= parent->last_subkey );
    for ( ; i < parent->last_subkey; i++) parent->subkeys[i] = parent->subkeys[i + 1];
    parent->last_subkey--;
    name->parent = NULL;
    if (parent->wow6432node == key) parent->wow6432node = NULL;
    release_object( key );

    /* try to shrink the array */
    nb_subkeys = parent->nb_subkeys;
    if (nb_subkeys > MIN_SUBKEYS && parent->last_subkey < nb_subkeys / 2)
    {
        struct key **new_subkeys;
        nb_subkeys -= nb_subkeys / 3;  /* shrink by 33% */
        if (nb_subkeys < MIN_SUBKEYS) nb_subkeys = MIN_SUBKEYS;
        if (!(new_subkeys = realloc( parent->subkeys, nb_subkeys * sizeof(*new_subkeys) ))) return;
        parent->subkeys = new_subkeys;
        parent->nb_subkeys = nb_subkeys;
    }
}

/* close the notification associated with a handle */
static int key_close_handle( struct object *obj, struct process *process, obj_handle_t handle )
{
    struct key * key = (struct key *) obj;
    struct notify *notify = find_notify( key, process, handle );
    if (notify) do_notification( key, notify, 1 );
    return 1;  /* ok to close */
}

static void key_destroy( struct object *obj )
{
    int i;
    struct list *ptr;
    struct key *key = (struct key *)obj;
    assert( obj->ops == &key_ops );

    free( key->class );
    list_remove( &key->e1s_entry );
    free( key->e1s_proxy_account );
    for (i = 0; i <= key->last_value; i++)
    {
        free( key->values[i].name );
        free( key->values[i].data );
    }
    free( key->values );
    for (i = 0; i <= key->last_subkey; i++)
    {
        key->subkeys[i]->obj.name->parent = NULL;
        release_object( key->subkeys[i] );
    }
    free( key->subkeys );
    /* unconditionally notify everything waiting on this key */
    while ((ptr = list_head( &key->notify_list )))
    {
        struct notify *notify = LIST_ENTRY( ptr, struct notify, entry );
        do_notification( key, notify, 1 );
    }
}

/* mark a key and all its parents as dirty (modified) */
static void make_dirty( struct key *key )
{
    while (key)
    {
        if (key->flags & (KEY_DIRTY|KEY_VOLATILE)) return;  /* nothing to do */
        key->flags |= KEY_DIRTY;
        key = get_parent( key );
    }
}

/* mark a key and all its subkeys as clean (not modified) */
static void make_clean( struct key *key )
{
    int i;

    if (key->flags & KEY_VOLATILE) return;
    if (!(key->flags & KEY_DIRTY)) return;
    key->flags &= ~KEY_DIRTY;
    for (i = 0; i <= key->last_subkey; i++) make_clean( key->subkeys[i] );
}

/* go through all the notifications and send them if necessary */
static void check_notify( struct key *key, unsigned int change, int not_subtree )
{
    struct list *ptr, *next;

    LIST_FOR_EACH_SAFE( ptr, next, &key->notify_list )
    {
        struct notify *n = LIST_ENTRY( ptr, struct notify, entry );
        if ( ( not_subtree || n->subtree ) && ( change & n->filter ) )
            do_notification( key, n, 0 );
    }
}

/* update key modification time */
static void touch_key_ex( struct key *key, unsigned int change, bool mark_dirty )
{
    key->modif = current_time;
    if (mark_dirty && !(key->flags & KEY_VOLATILE)) make_dirty( key );

    /* do notifications */
    check_notify( key, change, 1 );
    for (key = get_parent( key ); key; key = get_parent( key )) check_notify( key, change, 0 );
}

static void touch_key( struct key *key, unsigned int change )
{
    touch_key_ex( key, change, true );
}

/* a hive was loaded below this key: as on Windows, the subkey-name watchers of the key and its ancestors are told
 * (nothing is marked for saving, the parent's own data did not change). Windows tells nobody of an unload. */
static void notify_hive_mount( struct key *parent )
{
    if (!parent) return;
    check_notify( parent, REG_NOTIFY_CHANGE_NAME, 1 );
    for (parent = get_parent( parent ); parent; parent = get_parent( parent ))
        check_notify( parent, REG_NOTIFY_CHANGE_NAME, 0 );
}

/* get the wow6432node key if any, grabbing it and releasing the original key */
static struct key *grab_wow6432node( struct key *key )
{
    struct key *ret = key->wow6432node;

    if (!ret) return key;
    if (ret->flags & KEY_WOWSHARE) return key;
    grab_object( ret );
    release_object( key );
    return ret;
}

/* recursively obtain the wow6432node parent key if any */
static struct key *get_wow6432node( struct key *key )
{
    struct key *parent, *ret;
    struct unicode_str name;
    int index;

    if (!key)
        return NULL;

    if (key->wow6432node)
        return key->wow6432node;

    parent = get_parent( key );
    if (parent && key == parent->wow6432node)
        return key;

    if (!(ret = get_wow6432node( parent )))
        return key;

    name.str = key->obj.name->name;
    name.len = key->obj.name->len;
    return find_subkey( ret, name, &index );
}

/* get the real parent key, taking wow6432node into account */
static struct object *get_wow6432_parent( struct key *parent, unsigned int access, struct unicode_str name )
{
    if (parent && !(access & KEY_WOW64_64KEY) && !is_wow6432node( name.str, name.len ))
    {
        struct key *key = get_wow6432node( parent );
        if (key && ((access & KEY_WOW64_32KEY) || (key->flags & KEY_WOWSHARE))) return &key->obj;
    }
    return &parent->obj;
}

/* recursively create a subkey (for internal use only) */
static struct key *create_key_recursive( struct key *root, struct unicode_str name, timeout_t modif )
{
    struct key *key, *parent = (struct key *)grab_object( root );
    struct key_init_data data = { .modif = modif };
    struct object_params params = { .ops = &key_ops, .name = name, .attr = OBJ_OPENIF, .init_data = &data };
    data_size_t len = name.len;

    while (len)
    {
        params.name.len = get_path_element( params.name.str, len );
        params.root = &parent->obj;
        key = create_named_object( &params );
        release_object( parent );
        if (!key) return NULL;
        parent = key;

        /* skip trailing \\ and move to the next element */
        if (params.name.len < len)
        {
            params.name.len += sizeof(WCHAR);
            params.name.str += params.name.len / sizeof(WCHAR);
            len -= params.name.len;
        }
        else break;
    }
    return parent;
}

/* query information about a key or a subkey */
static void enum_key( struct key *key, int index, int info_class, struct enum_key_reply *reply )
{
    int i;
    data_size_t len, namelen, classlen;
    data_size_t max_subkey = 0, max_class = 0;
    data_size_t max_value = 0, max_data = 0;
    WCHAR *fullname = NULL;
    char *data;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if (index != -1)  /* -1 means use the specified key directly */
    {
        if ((index < 0) || (index > key->last_subkey))
        {
            set_error( STATUS_NO_MORE_ENTRIES );
            return;
        }
        key = key->subkeys[index];
    }

    if (key->e1s_view && !e1s_refresh_view( key )) return;

    namelen = key->obj.name->len;
    classlen = key->classlen;

    switch(info_class)
    {
    case KeyNameInformation:
        if (!(fullname = key_get_full_name( &key->obj, ~0u, &namelen ))) return;
        /* fall through */
    case KeyBasicInformation:
        classlen = 0; /* only return the name */
        /* fall through */
    case KeyNodeInformation:
        reply->max_subkey = 0;
        reply->max_class  = 0;
        reply->max_value  = 0;
        reply->max_data   = 0;
        break;
    case KeyFullInformation:
    case KeyCachedInformation:
        for (i = 0; i <= key->last_subkey; i++)
        {
            if (key->subkeys[i]->obj.name->len > max_subkey) max_subkey = key->subkeys[i]->obj.name->len;
            if (key->subkeys[i]->classlen > max_class) max_class = key->subkeys[i]->classlen;
        }
        for (i = 0; i <= key->last_value; i++)
        {
            if (key->values[i].namelen > max_value) max_value = key->values[i].namelen;
            if (key->values[i].len > max_data) max_data = key->values[i].len;
        }
        reply->max_subkey = max_subkey;
        reply->max_class  = max_class;
        reply->max_value  = max_value;
        reply->max_data   = max_data;
        reply->namelen    = namelen;
        if (info_class == KeyCachedInformation)
            classlen = 0; /* don't return any data, only its size */
        namelen = 0;  /* don't return name */
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    reply->subkeys = key->last_subkey + 1;
    reply->values  = key->last_value + 1;
    reply->modif   = key->modif;
    reply->total   = namelen + classlen;

    len = min( reply->total, get_reply_max_size() );
    if (len && (data = set_reply_data_size( len )))
    {
        if (len > namelen)
        {
            reply->namelen = namelen;
            data = mem_append( data, key->obj.name->name, namelen );
            memcpy( data, key->class, len - namelen );
        }
        else if (info_class == KeyNameInformation)
        {
            reply->namelen = namelen;
            memcpy( data, fullname, len );
        }
        else
        {
            reply->namelen = len;
            memcpy( data, key->obj.name->name, len );
        }
    }
    free( fullname );
    if (debug_level > 1) dump_operation( key, NULL, "Enum" );
}

/* rename a key and its values */
static void rename_key( struct key *key, struct unicode_str new_name )
{
    struct object_name *new_name_ptr;
    struct key *parent = get_parent( key );
    data_size_t len;
    int i, index, cur_index;

    if (key->flags & KEY_E1S_ROOT || key->e1s_view)
    { set_error( STATUS_ACCESS_DENIED ); return; }

    /* changing to a path is not allowed */
    len = get_path_element( new_name.str, new_name.len );
    if (!len || len != new_name.len || len > MAX_NAME_LEN * sizeof(WCHAR))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    /* check for existing subkey with the same name */
    if (!parent || find_subkey( parent, new_name, &index ))
    {
        set_error( STATUS_CANNOT_DELETE );
        return;
    }

    if (key == parent->wow6432node || is_wow6432node( new_name.str, new_name.len ))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!(new_name_ptr = mem_alloc( offsetof( struct object_name, name[new_name.len / sizeof(WCHAR)] ))))
        return;

    new_name_ptr->obj = &key->obj;
    new_name_ptr->len = new_name.len;
    new_name_ptr->parent = &parent->obj;
    memcpy( new_name_ptr->name, new_name.str, new_name.len );

    for (cur_index = 0; cur_index <= parent->last_subkey; cur_index++)
        if (parent->subkeys[cur_index] == key) break;

    if (cur_index < index)
    {
        --index;
        for (i = cur_index; i < index; ++i) parent->subkeys[i] = parent->subkeys[i+1];
    }
    else if (cur_index > index)
    {
        for (i = cur_index; i > index; --i) parent->subkeys[i] = parent->subkeys[i-1];
    }
    parent->subkeys[index] = key;

    free( key->obj.name );
    key->obj.name = new_name_ptr;

    if (debug_level > 1) dump_operation( key, NULL, "Rename" );
    touch_key_ex( key, REG_NOTIFY_CHANGE_NAME, !(key->flags & KEY_VOLATILE) );
}

/* delete a key and its values */
static int delete_key( struct key *key, int recurse )
{
    struct key *parent;

    if (key->flags & KEY_E1S_ROOT) { set_error( STATUS_ACCESS_DENIED ); return 0; }

    if (key->flags & KEY_DELETED) return 1;

    if (!(parent = get_parent( key )))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return 0;
    }

    if (recurse)
    {
        while (key->last_subkey >= 0)
            if (!delete_key( key->subkeys[key->last_subkey], 1 )) return 0;
    }
    else if (key->last_subkey >= 0)  /* we can only delete a key that has no subkeys */
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }

    if (debug_level > 1) dump_operation( key, NULL, "Delete" );
    key->flags |= KEY_DELETED;
    unlink_named_object( &key->obj );
    touch_key_ex( parent, REG_NOTIFY_CHANGE_NAME, !(key->flags & KEY_VOLATILE) );
    return 1;
}

/* try to grow the array of values; return 1 if OK, 0 on error */
static int grow_values( struct key *key )
{
    struct key_value *new_val;
    int nb_values;

    if (key->nb_values)
    {
        nb_values = key->nb_values + (key->nb_values / 2);  /* grow by 50% */
        if (!(new_val = realloc( key->values, nb_values * sizeof(*new_val) )))
        {
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
    }
    else
    {
        nb_values = MIN_VALUES;
        if (!(new_val = mem_alloc( nb_values * sizeof(*new_val) ))) return 0;
    }
    key->values = new_val;
    key->nb_values = nb_values;
    return 1;
}

/* find the named value of a given key and return its index in the array */
static struct key_value *find_value( const struct key *key, struct unicode_str name, int *index )
{
    int i, min, max, res;
    data_size_t len;

    min = 0;
    max = key->last_value;
    while (min <= max)
    {
        i = (min + max) / 2;
        len = min( key->values[i].namelen, name.len );
        res = memicmp_strW( key->values[i].name, name.str, len );
        if (!res) res = key->values[i].namelen - name.len;
        if (!res)
        {
            *index = i;
            return &key->values[i];
        }
        if (res > 0) max = i - 1;
        else min = i + 1;
    }
    *index = min;  /* this is where we should insert it */
    return NULL;
}

/* insert a new value; the index must have been returned by find_value */
static struct key_value *insert_value( struct key *key, struct unicode_str name, int index )
{
    struct key_value *value;
    WCHAR *new_name = NULL;
    int i;

    if (name.len > MAX_VALUE_LEN * sizeof(WCHAR))
    {
        set_error( STATUS_NAME_TOO_LONG );
        return NULL;
    }
    if (key->last_value + 1 == key->nb_values)
    {
        if (!grow_values( key )) return NULL;
    }
    if (name.len && !(new_name = memdup( name.str, name.len ))) return NULL;
    for (i = ++key->last_value; i > index; i--) key->values[i] = key->values[i - 1];
    value = &key->values[index];
    value->name    = new_name;
    value->namelen = name.len;
    value->len     = 0;
    value->data    = NULL;
    return value;
}

/* set a key value */
static void set_value( struct key *key, struct unicode_str name,
                       int type, const void *data, data_size_t len )
{
    struct key_value *value;
    void *ptr = NULL;
    int index;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if ((value = find_value( key, name, &index )))
    {
        /* check if the new value is identical to the existing one */
        if (value->type == type && value->len == len &&
            value->data && !memcmp( value->data, data, len ))
        {
            if (debug_level > 1) dump_operation( key, value, "Skip setting" );
            return;
        }
    }

    if (key->flags & KEY_SYMLINK)
    {
        if (type != REG_LINK || name.len != symlink_str.len ||
            memicmp_strW( name.str, symlink_str.str, name.len ))
        {
            set_error( STATUS_ACCESS_DENIED );
            return;
        }
    }

    if (len && !(ptr = memdup( data, len ))) return;

    if (!value)
    {
        if (!(value = insert_value( key, name, index )))
        {
            free( ptr );
            return;
        }
    }
    else free( value->data ); /* already existing, free previous data */

    value->type  = type;
    value->len   = len;
    value->data  = ptr;
    touch_key( key, REG_NOTIFY_CHANGE_LAST_SET );
    if (debug_level > 1) dump_operation( key, value, "Set" );
}

/* get a key value */
static void get_value( struct key *key, struct unicode_str name, int *type, data_size_t *len )
{
    struct key_value *value;
    int index;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if ((value = find_value( key, name, &index )))
    {
        *type = value->type;
        *len  = value->len;
        if (value->data) set_reply_data( value->data, min( value->len, get_reply_max_size() ));
        if (debug_level > 1) dump_operation( key, value, "Get" );
    }
    else
    {
        *type = -1;
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
    }
}

/* enumerate a key value */
static void enum_value( struct key *key, int i, int info_class, struct enum_key_value_reply *reply )
{
    struct key_value *value;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if (i < 0 || i > key->last_value) set_error( STATUS_NO_MORE_ENTRIES );
    else
    {
        void *data;
        data_size_t namelen, maxlen;

        value = &key->values[i];
        reply->type = value->type;
        namelen = value->namelen;

        switch(info_class)
        {
        case KeyValueBasicInformation:
            reply->total = namelen;
            break;
        case KeyValueFullInformation:
            reply->total = namelen + value->len;
            break;
        case KeyValuePartialInformation:
            reply->total = value->len;
            namelen = 0;
            break;
        default:
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }

        maxlen = min( reply->total, get_reply_max_size() );
        if (maxlen && ((data = set_reply_data_size( maxlen ))))
        {
            if (maxlen > namelen)
            {
                reply->namelen = namelen;
                data = mem_append( data, value->name, namelen );
                memcpy( data, value->data, maxlen - namelen );
            }
            else
            {
                reply->namelen = maxlen;
                memcpy( data, value->name, maxlen );
            }
        }
        if (debug_level > 1) dump_operation( key, value, "Enum" );
    }
}

/* delete a value */
static void delete_value( struct key *key, struct unicode_str name )
{
    struct key_value *value;
    int i, index, nb_values;

    if (key->flags & KEY_PREDEF)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if (!(value = find_value( key, name, &index )))
    {
        set_error( STATUS_OBJECT_NAME_NOT_FOUND );
        return;
    }
    if (debug_level > 1) dump_operation( key, value, "Delete" );
    free( value->name );
    free( value->data );
    for (i = index; i < key->last_value; i++) key->values[i] = key->values[i + 1];
    key->last_value--;
    touch_key( key, REG_NOTIFY_CHANGE_LAST_SET );

    /* try to shrink the array */
    nb_values = key->nb_values;
    if (nb_values > MIN_VALUES && key->last_value < nb_values / 2)
    {
        struct key_value *new_val;
        nb_values -= nb_values / 3;  /* shrink by 33% */
        if (nb_values < MIN_VALUES) nb_values = MIN_VALUES;
        if (!(new_val = realloc( key->values, nb_values * sizeof(*new_val) ))) return;
        key->values = new_val;
        key->nb_values = nb_values;
    }
}

/* get the registry key corresponding to an hkey handle */
static struct key *get_hkey_obj( obj_handle_t hkey, unsigned int access )
{
    struct key *key = (struct key *)get_handle_obj( current->process, hkey, access, &key_ops );

    if (key && key->e1s_proxy_account)
    {
        struct e1s_hive *m = e1s_find_hive( key->e1s_proxy_account );
        struct key *resolved = NULL;
        if (m && m->phase == E1S_LOADED && m->generation->generation == key->e1s_proxy_generation)
            resolved = (struct key *)grab_object( key->e1s_proxy_classes ? m->view : m->user );
        else set_error( STATUS_KEY_DELETED );
        release_object( key );
        key = resolved;
    }
    if (key && key->e1s_sd)
    {
        struct e1s_hive *m;
        for (m = e1s_hives; m && m->sd != key->e1s_sd; m = m->next) ;
        if (!m || m->phase != E1S_LOADED)
        { release_object( key ); key = NULL; set_error( STATUS_REGISTRY_CORRUPT ); }
    }
    if (key && key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        release_object( key );
        key = NULL;
    }
    if (key && key->e1s_view)
    {
        struct key *mapped;
        if (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE))
        {
            mapped = e1s_view_user( key, !(access & DELETE) );
            release_object( key ); key = mapped;
        }
        else if (!e1s_refresh_view( key )) { release_object( key ); key = NULL; }
    }
    return key;
}

/* read a line from the input file */
static int read_next_line( struct file_load_info *info )
{
    char *newbuf;
    int newlen, pos = 0;

    info->line++;
    for (;;)
    {
        if (!fgets( info->buffer + pos, info->len - pos, info->file ))
            return (pos != 0);  /* EOF */
        pos = strlen(info->buffer);
        if (info->buffer[pos-1] == '\n')
        {
            /* got a full line */
            info->buffer[--pos] = 0;
            if (pos > 0 && info->buffer[pos-1] == '\r') info->buffer[pos-1] = 0;
            return 1;
        }
        if (pos < info->len - 1) return 1;  /* EOF but something was read */

        /* need to enlarge the buffer */
        newlen = info->len + info->len / 2;
        if (!(newbuf = realloc( info->buffer, newlen )))
        {
            set_error( STATUS_NO_MEMORY );
            return -1;
        }
        info->buffer = newbuf;
        info->len = newlen;
    }
}

/* make sure the temp buffer holds enough space */
static int get_file_tmp_space( struct file_load_info *info, size_t size )
{
    WCHAR *tmp;
    if (info->tmplen >= size) return 1;
    if (!(tmp = realloc( info->tmp, size )))
    {
        set_error( STATUS_NO_MEMORY );
        return 0;
    }
    info->tmp = tmp;
    info->tmplen = size;
    return 1;
}

/* report an error while loading an input file */
static void file_read_error( const char *err, struct file_load_info *info )
{
    if (e1s_strict_loading) { set_error( STATUS_NOT_REGISTRY_FILE ); return; }
    if (info->filename)
        fprintf( stderr, "%s:%d: %s '%s'\n", info->filename, info->line, err, info->buffer );
    else
        fprintf( stderr, "<fd>:%d: %s '%s'\n", info->line, err, info->buffer );
}

/* convert a data type tag to a value type */
static int get_data_type( const char *buffer, int *type, int *parse_type )
{
    struct data_type { const char *tag; int len; int type; int parse_type; };

    static const struct data_type data_types[] =
    {                   /* actual type */  /* type to assume for parsing */
        { "\"",        1,   REG_SZ,              REG_SZ },
        { "str:\"",    5,   REG_SZ,              REG_SZ },
        { "str(2):\"", 8,   REG_EXPAND_SZ,       REG_SZ },
        { "str(7):\"", 8,   REG_MULTI_SZ,        REG_SZ },
        { "hex:",      4,   REG_BINARY,          REG_BINARY },
        { "dword:",    6,   REG_DWORD,           REG_DWORD },
        { "hex(",      4,   -1,                  REG_BINARY },
        { NULL,        0,    0,                  0 }
    };

    const struct data_type *ptr;
    char *end;

    for (ptr = data_types; ptr->tag; ptr++)
    {
        if (strncmp( ptr->tag, buffer, ptr->len )) continue;
        *parse_type = ptr->parse_type;
        if ((*type = ptr->type) != -1) return ptr->len;
        /* "hex(xx):" is special */
        *type = (int)strtoul( buffer + 4, &end, 16 );
        if ((end <= buffer) || strncmp( end, "):", 2 )) return 0;
        return end + 2 - buffer;
    }
    return 0;
}

/* load and create a key from the input file */
static struct key *load_key( struct key *base, const char *buffer, int prefix_len,
                             struct file_load_info *info, timeout_t *modif )
{
    WCHAR *p;
    struct unicode_str name;
    int res;
    unsigned int mod;
    data_size_t len;

    if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return NULL;

    len = info->tmplen;
    if ((res = parse_strW( info->tmp, &len, buffer, ']' )) == -1)
    {
        file_read_error( "Malformed key", info );
        return NULL;
    }
    if (sscanf( buffer + res, " %u", &mod ) == 1)
        *modif = (timeout_t)mod * TICKS_PER_SEC + ticks_1601_to_1970;
    else
        *modif = current_time;

    p = info->tmp;
    while (prefix_len && *p) { if (*p++ == '\\') prefix_len--; }

    if (!*p)
    {
        if (prefix_len > 1)
        {
            file_read_error( "Malformed key", info );
            return NULL;
        }
        /* empty key name, return base key */
        return (struct key *)grab_object( base );
    }
    name.str = p;
    name.len = len - (p - info->tmp + 1) * sizeof(WCHAR);
    if (e1s_strict_loading && *p == '\\')
    { set_error( STATUS_NOT_REGISTRY_FILE ); return NULL; }
    return create_key_recursive( base, name, 0 );
}

/* update the modification time of a key (and its parents) after it has been loaded from a file */
static void update_key_time( struct key *key, timeout_t modif )
{
    while (key && !key->modif)
    {
        key->modif = modif;
        key = get_parent( key );
    }
}

/* load a global option from the input file */
static int load_global_option( const char *buffer, struct file_load_info *info )
{
    const char *p;

    if (!strncmp( buffer, "#arch=", 6 ))
    {
        enum prefix_type type;
        p = buffer + 6;
        if (!strcmp( p, "win32" )) type = PREFIX_32BIT;
        else if (!strcmp( p, "win64" )) type = PREFIX_64BIT;
        else
        {
            file_read_error( "Unknown architecture", info );
            set_error( STATUS_NOT_REGISTRY_FILE );
            return 0;
        }
        if (prefix_type == PREFIX_UNKNOWN) prefix_type = type;
        else if (type != prefix_type)
        {
            file_read_error( "Mismatched architecture", info );
            set_error( STATUS_NOT_REGISTRY_FILE );
            return 0;
        }
    }
    /* ignore unknown options */
    return 1;
}

/* value of a hex digit, or -1 if the character is not a hex digit */
static int hex_digit_value( char c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* load a key option from the input file */
static int load_key_option( struct key *key, const char *buffer, struct file_load_info *info )
{
    const char *p;
    data_size_t len;

    if (!strncmp( buffer, "#time=", 6 ))
    {
        timeout_t modif = 0;
        for (p = buffer + 6; *p; p++)
        {
            if (*p >= '0' && *p <= '9') modif = (modif << 4) | (*p - '0');
            else if (*p >= 'A' && *p <= 'F') modif = (modif << 4) | (*p - 'A' + 10);
            else if (*p >= 'a' && *p <= 'f') modif = (modif << 4) | (*p - 'a' + 10);
            else break;
        }
        update_key_time( key, modif );
    }
    if (!strncmp( buffer, "#class=", 7 ))
    {
        p = buffer + 7;
        if (*p++ != '"') return 0;
        if (!get_file_tmp_space( info, strlen(p) * sizeof(WCHAR) )) return 0;
        len = info->tmplen;
        if (parse_strW( info->tmp, &len, p, '\"' ) == -1) return 0;
        free( key->class );
        if (!(key->class = memdup( info->tmp, len ))) len = 0;
        key->classlen = len;
    }
    if (!strncmp( buffer, "#sd=", 4 ))
    {
        const char *hex = buffer + 4;
        data_size_t hexlen = strlen( hex );
        data_size_t size, n;
        unsigned char *sd;

        /* the system tier's stored key permission */
        if (hexlen & 1) goto bad_sd;  /* odd length */
        size = hexlen / 2;
        if (!size || size > 65536) goto bad_sd;  /* empty or too large */
        for (n = 0; n < hexlen; n++)
            if (!isxdigit( hex[n] )) goto bad_sd;  /* non-hex character */
        if (!(sd = mem_alloc( size ))) return 0;
        for (n = 0; n < size; n++)
            sd[n] = (hex_digit_value( hex[2*n] ) << 4) | hex_digit_value( hex[2*n+1] );
        if (!sd_is_valid( (const struct security_descriptor *)sd, size ))
        {
            free( sd );
            goto bad_sd;
        }
        set_sd_defaults_from_token( &key->obj, (const struct security_descriptor *)sd,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            SACL_SECURITY_INFORMATION, NULL );
        free( sd );
        return 1;

    bad_sd:
        file_read_error( "Malformed security descriptor", info );
        return 0;
    }
    if (!strncmp( buffer, "#link", 5 )) key->flags |= KEY_SYMLINK;
    /* ignore unknown options */
    return 1;
}

/* parse a comma-separated list of hex digits */
static int parse_hex( unsigned char *dest, data_size_t *len, const char *buffer )
{
    const char *p = buffer;
    data_size_t count = 0;
    char *end;

    while (isxdigit(*p))
    {
        unsigned int val = strtoul( p, &end, 16 );
        if (end == p || val > 0xff) return -1;
        if (count++ >= *len) return -1;  /* dest buffer overflow */
        *dest++ = val;
        p = end;
        while (isspace(*p)) p++;
        if (*p == ',') p++;
        while (isspace(*p)) p++;
    }
    *len = count;
    return p - buffer;
}

/* parse a value name and create the corresponding value */
static struct key_value *parse_value_name( struct key *key, const char *buffer, data_size_t *len,
                                           struct file_load_info *info )
{
    struct key_value *value;
    struct unicode_str name;
    int index;

    if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return NULL;
    name.str = info->tmp;
    name.len = info->tmplen;
    if (buffer[0] == '@')
    {
        name.len = 0;
        *len = 1;
    }
    else
    {
        int r = parse_strW( info->tmp, &name.len, buffer + 1, '\"' );
        if (r == -1) goto error;
        *len = r + 1; /* for initial quote */
        name.len -= sizeof(WCHAR);  /* terminating null */
    }
    while (isspace(buffer[*len])) (*len)++;
    if (buffer[*len] != '=') goto error;
    (*len)++;
    while (isspace(buffer[*len])) (*len)++;
    if (!(value = find_value( key, name, &index ))) value = insert_value( key, name, index );
    return value;

 error:
    file_read_error( "Malformed value name", info );
    return NULL;
}

/* load a value from the input file */
static int load_value( struct key *key, const char *buffer, struct file_load_info *info )
{
    DWORD dw;
    void *ptr, *newptr;
    int res, type, parse_type;
    data_size_t maxlen, len;
    struct key_value *value;

    if (!(value = parse_value_name( key, buffer, &len, info ))) return 0;
    if (!(res = get_data_type( buffer + len, &type, &parse_type ))) goto error;
    buffer += len + res;

    switch(parse_type)
    {
    case REG_SZ:
        if (!get_file_tmp_space( info, strlen(buffer) * sizeof(WCHAR) )) return 0;
        len = info->tmplen;
        if (parse_strW( info->tmp, &len, buffer, '\"' ) == -1) goto error;
        ptr = info->tmp;
        break;
    case REG_DWORD:
        dw = strtoul( buffer, NULL, 16 );
        ptr = &dw;
        len = sizeof(dw);
        break;
    case REG_BINARY:  /* hex digits */
        len = 0;
        for (;;)
        {
            maxlen = 1 + strlen(buffer) / 2;  /* at least 2 chars for one hex byte */
            if (!get_file_tmp_space( info, len + maxlen )) return 0;
            if ((res = parse_hex( (unsigned char *)info->tmp + len, &maxlen, buffer )) == -1) goto error;
            len += maxlen;
            buffer += res;
            while (isspace(*buffer)) buffer++;
            if (!*buffer) break;
            if (*buffer != '\\') goto error;
            if (read_next_line( info) != 1) goto error;
            buffer = info->buffer;
            while (isspace(*buffer)) buffer++;
        }
        ptr = info->tmp;
        break;
    default:
        assert(0);
        ptr = NULL;  /* keep compiler quiet */
        break;
    }

    if (!len) newptr = NULL;
    else if (!(newptr = memdup( ptr, len ))) return 0;

    free( value->data );
    value->data = newptr;
    value->len  = len;
    value->type = type;
    return 1;

 error:
    file_read_error( "Malformed value", info );
    free( value->data );
    value->data = NULL;
    value->len  = 0;
    value->type = REG_NONE;
    return 0;
}

/* return the length (in path elements) of name that is part of the key name */
/* for instance if key is USER\foo\bar and name is foo\bar\baz, return 2 */
static int get_prefix_len( struct key *key, const char *name, struct file_load_info *info )
{
    WCHAR *p;
    int res;
    data_size_t len;

    if (!get_file_tmp_space( info, strlen(name) * sizeof(WCHAR) )) return 0;

    len = info->tmplen;
    if (parse_strW( info->tmp, &len, name, ']' ) == -1)
    {
        file_read_error( "Malformed key", info );
        return 0;
    }
    for (p = info->tmp; *p; p++) if (*p == '\\') break;
    len = (p - info->tmp) * sizeof(WCHAR);
    for (res = 1; key != root_key; res++)
    {
        if (len == key->obj.name->len && !memicmp_strW( info->tmp, key->obj.name->name, len )) break;
        key = get_parent( key );
    }
    if (key == root_key) res = 0;  /* no matching name */
    return res;
}

/* load all the keys from the input file */
/* prefix_len is the number of key name prefixes to skip, or -1 for autodetection */
static void load_keys( struct key *key, const char *filename, FILE *f, int prefix_len )
{
    struct key *subkey = NULL;
    struct file_load_info info;
    timeout_t modif = current_time;
    char *p;

    info.filename = filename;
    info.file   = f;
    info.len    = 4;
    info.tmplen = 4;
    info.line   = 0;
    if (!(info.buffer = mem_alloc( info.len ))) return;
    if (!(info.tmp = mem_alloc( info.tmplen )))
    {
        free( info.buffer );
        return;
    }

    if ((read_next_line( &info ) != 1) ||
        strcmp( info.buffer, "WINE REGISTRY Version 2" ))
    {
        set_error( STATUS_NOT_REGISTRY_FILE );
        goto done;
    }

    while (read_next_line( &info ) == 1)
    {
        if (e1s_strict_loading && get_error()) break;
        p = info.buffer;
        while (*p && isspace(*p)) p++;
        switch(*p)
        {
        case '[':   /* new key */
            if (subkey)
            {
                update_key_time( subkey, modif );
                release_object( subkey );
            }
            if (prefix_len == -1) prefix_len = get_prefix_len( key, p + 1, &info );
            if (!(subkey = load_key( key, p + 1, prefix_len, &info, &modif )))
                file_read_error( "Error creating key", &info );
            break;
        case '@':   /* default value */
        case '\"':  /* value */
            if (subkey) load_value( subkey, p, &info );
            else file_read_error( "Value without key", &info );
            break;
        case '#':   /* option */
            /* Only class strings emitted by S14's serializer are accepted.
             * No security/link/global directives can alter managed authority. */
            if (e1s_strict_loading && (!subkey || strncmp( p, "#class=", 7 )))
            { set_error( STATUS_NOT_REGISTRY_FILE ); goto done; }
            if (subkey) load_key_option( subkey, p, &info );
            else if (!load_global_option( p, &info )) goto done;
            break;
        case ';':   /* comment */
        case 0:     /* empty line */
            break;
        default:
            file_read_error( "Unrecognized input", &info );
            break;
        }
    }

 done:
    if (e1s_strict_loading && ferror( f )) set_error( STATUS_NOT_REGISTRY_FILE );
    if (subkey)
    {
        update_key_time( subkey, modif );
        release_object( subkey );
    }
    free( info.buffer );
    free( info.tmp );
}

/* GPL-2.0-or-later S13 additions. Sources remain live; cached union keys
 * contain no authority and writes always select the private user source. */
static struct e1s_hive *e1s_find_hive( const struct wex_account *a )
{
    struct e1s_hive *m;
    if (!a) return NULL;
    for (m = e1s_hives; m; m = m->next)
        if (!memcmp( a->sid, m->account.sid, sizeof(a->sid) ) &&
            !memcmp( a->incarnation, m->account.incarnation, 16 )) return m;
    return NULL;
}

static int e1s_key_ready( const struct key *key )
{
    struct e1s_hive *m;
    if (!key->e1s_sd) return 1;
    if (key->e1s_proxy_account)
    {
        m = e1s_find_hive( key->e1s_proxy_account );
        if (m && m->phase == E1S_LOADED && m->generation->generation == key->e1s_proxy_generation) return 1;
    }
    else
        for (m = e1s_hives; m; m = m->next)
            if (m->sd == key->e1s_sd && (m->phase == E1S_LOADED || m->phase == E1S_LOADING)) return 1;
    set_error( STATUS_REGISTRY_CORRUPT );
    return 0;
}

int e1s_profile_process_started( struct token *token )
{
    struct e1s_hive *m = token ? e1s_find_hive( token_e1s_account( token ) ) : NULL;
#if defined(WEX_E1S13_HKU_TEST) && defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    if (token && token_e1s_account( token ) && (!m || m->phase != E1S_LOADED))
    { set_error( STATUS_OBJECT_PATH_NOT_FOUND ); return 0; }
#endif
    if (m && m->phase == E1S_LOADED) m->idle_sweeps = 0;
    return 1;
}

static struct key *e1s_source_path( struct key *root, struct unicode_str path )
{
    unsigned int len;
    int index;
    while (root && path.len)
    {
        struct unicode_str part = path;
        len = get_path_element( path.str, path.len );
        if (!len) return NULL;
        part.len = len;
        root = find_subkey( root, part, &index );
        if (root && root->flags & (KEY_DELETED | KEY_SYMLINK)) return NULL;
        if (len < path.len) len += sizeof(WCHAR);
        path.str += len / sizeof(WCHAR); path.len -= len;
    }
    return root;
}

static struct key *e1s_view_user( struct key *view, int create )
{
    struct unicode_str path;
    WCHAR *buffer = build_relative_key_path( view, view->e1s_view->view, &path.len );
    struct key *key;
    path.str = buffer;
    if (view != view->e1s_view->view && !buffer)
    { if (!get_error()) set_error( STATUS_KEY_DELETED ); return NULL; }
    if (!create)
    {
        key = e1s_source_path( view->e1s_view->classes, path );
        if (key) grab_object( key );
        else set_error( STATUS_OBJECT_NAME_NOT_FOUND );
    }
    else key = path.len ? create_key_recursive( view->e1s_view->classes, path, current_time ) :
                    (struct key *)grab_object( view->e1s_view->classes );
    if (key && get_error() == STATUS_OBJECT_NAME_EXISTS) clear_error();
    free( buffer );
    return key;
}

static int e1s_refresh_view_inner( struct key *view )
{
    struct e1s_hive *m = view->e1s_view;
    struct unicode_str path, name;
    struct key *sources[2], *src, *child;
    struct key_value *value;
    WCHAR *buffer;
    int pass, i, index, present;
    unsigned int access;
    if (!m) return 1;
    buffer = build_relative_key_path( view, m->view, &path.len );
    if (view != m->view && !buffer)
    { if (!get_error()) set_error( STATUS_KEY_DELETED ); return 0; }
    path.str = buffer;
    sources[0] = e1s_source_path( m->machine_classes, path );
    sources[1] = e1s_source_path( m->classes, path );
    free( buffer );
    if (!sources[0] && !sources[1]) { set_error( STATUS_KEY_DELETED ); return 0; }
    for (pass = 0; pass < 2; pass++)
    {
        if (!(src = sources[pass])) continue;
        access = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS;
        if (!check_object_access( NULL, &src->obj, &access )) return 0;
    }
    for (i = 0; i <= view->last_value; i++)
    { free( view->values[i].name ); free( view->values[i].data ); }
    view->last_value = -1;
    /* Prune disappeared names. Existing handles retain their proxy and fail
     * with KEY_DELETED when neither live source has that relative path. */
    for (i = view->last_subkey; i >= 0; i--)
    {
        child = view->subkeys[i]; name.str = child->obj.name->name; name.len = child->obj.name->len;
        present = (sources[0] && find_subkey( sources[0], name, &index )) ||
                  (sources[1] && find_subkey( sources[1], name, &index ));
        if (!present) { grab_object( child ); delete_key( child, 1 ); release_object( child ); }
    }
    for (pass = 0; pass < 2; pass++)
    {
        if (!(src = sources[pass])) continue;
        if (src->last_subkey > 16383 || src->last_value > 16383)
        { set_error( STATUS_INSUFFICIENT_RESOURCES ); return 0; }
        for (i = 0; i <= src->last_value; i++)
        {
            name.str = src->values[i].name; name.len = src->values[i].namelen;
            if (!(value = find_value( view, name, &index ))) value = insert_value( view, name, index );
            if (!value) return 0;
            free( value->data ); value->data = NULL;
            if (src->values[i].len && !(value->data = memdup( src->values[i].data, src->values[i].len ))) return 0;
            value->len = src->values[i].len; value->type = src->values[i].type;
        }
        for (i = 0; i <= src->last_subkey; i++)
        {
            name.str = src->subkeys[i]->obj.name->name; name.len = src->subkeys[i]->obj.name->len;
            if (!find_subkey( view, name, &index ))
            {
                if (!(child = create_key_recursive( view, name, current_time ))) return 0;
                child->flags |= src->subkeys[i]->flags & KEY_WOWSHARE;
                release_object( child );
            }
        }
    }
    return 1;
}

static int e1s_refresh_view( struct key *view )
{
    struct e1s_hive *m = view->e1s_view;
    int ret;
    if (!m || m->refreshing) return 1;
    m->refreshing = 1;
    ret = e1s_refresh_view_inner( view );
    m->refreshing = 0;
    return ret;
}

static obj_handle_t e1s_alloc_key_handle( struct key *key, unsigned int access, unsigned int attr )
{
    if (!e1s_key_ready( key )) return 0;
    /* Existing MIC checks examine explicit write bits. A managed MAXIMUM_ALLOWED
     * request must not acquire High write rights through that implicit mask. */
    if (key->e1s_sd && (access & MAXIMUM_ALLOWED) &&
        !token_e1s_profile_high( current->token ? current->token : current->process->token ))
    { set_error( STATUS_ACCESS_DENIED ); return 0; }
    return alloc_handle( current->process, key, access, attr );
}

/* load a part of the registry from a file */
static void load_registry( struct key *key, obj_handle_t handle )
{
    struct file *file;
    int fd;

    if (!(file = get_file_obj( current->process, handle, FILE_READ_DATA ))) return;
    fd = dup( get_file_unix_fd( file ) );
    release_object( file );
    if (fd != -1)
    {
        FILE *f = fdopen( fd, "r" );
        if (f)
        {
            load_keys( key, NULL, f, -1 );
            fclose( f );
        }
        else file_set_error();
    }
}

static bool is_shared_machine_file( const char *filename )
{
    struct stat lst;
    char target[PATH_MAX];
    char machine_root[PATH_MAX];
    const char *env_root = getenv( "WINE_MACHINE_ROOT" );
    size_t root_len;

    if (lstat( filename, &lst ) != 0 || !S_ISLNK( lst.st_mode ))
        return false;

    if (!realpath( filename, target ))
        return false;

    if (!env_root || !*env_root) env_root = "/var/lib/wineexplorer/machine";
    if (!realpath( env_root, machine_root ))
    {
        snprintf( machine_root, sizeof(machine_root), "%s", env_root );
    }

    root_len = strlen( machine_root );
    if (strncmp( target, machine_root, root_len ) == 0 &&
        (target[root_len] == '/' || target[root_len] == '\0'))
    {
        return true;
    }

    return false;
}

/* The path of a machine hive of a system role server: the machine root of the root-owned configuration, never
 * the prefix, whose symlinks the user controls. */
static const char *machine_hive_path( const char *name )
{
    char *path;

    if (asprintf( &path, "%s/%s", sysrun_machine_root(), name ) == -1) fatal_error( "out of memory\n" );
    return path;
}

/* Open a machine hive for reading with the system group. The last component must not be a symlink, and the file
 * must be a regular file of the system group that others cannot write and that root owns (a file of the user's
 * uid would stay writable for the user), else the server refuses to start. */
static FILE *fopen_machine_hive( const char *filename )
{
    struct stat st;
    FILE *f;
    int fd, raised = sysrun_raise();

    fd = open( filename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC );
    sysrun_drop( raised );
    if (fd == -1)
    {
        if (errno != ENOENT) fatal_error( "cannot open the machine hive %s: %s\n", filename, strerror( errno ));
        return NULL;
    }
    /* system tier: once session 0 owns the machine hives, it saves them as its own account */
    if (fstat( fd, &st ) || !S_ISREG( st.st_mode ) || (st.st_uid != 0 && st.st_uid != session0_hive_owner()) ||
        st.st_gid != sysrun_group() || (st.st_mode & S_IWOTH))
        fatal_error( "the machine hive %s is not a regular file of root and the system group\n", filename );
    if (!(f = fdopen( fd, "r" ))) fatal_error( "out of memory\n" );
    return f;
}

/* load one of the initial registry files; machine is set for the hives that a system role server takes from the
 * machine root */
static int load_init_registry_from_file( const char *filename, struct key *key, bool machine )
{
    struct stat st;
    FILE *f;
    struct save_branch_info *info;

    assert( save_branch_count < MAX_SAVE_BRANCH_INFO );
    info = &save_branch_info[save_branch_count];
    memset( info, 0, sizeof(*info) );

    if ((f = machine ? fopen_machine_hive( filename ) : fopen( filename, "r" )))
    {
        info->loaded = !fstat( fileno( f ), &st );
        load_keys( key, filename, f, 0 );
        fclose( f );
        if (get_error() == STATUS_NOT_REGISTRY_FILE)
        {
            fprintf( stderr, "%s is not a valid registry file\n", filename );
            return 1;
        }
    }

    info->filename = filename;
    info->key = (struct key *)grab_object( key );
    /* a machine hive of the launcher's machine root, or a prefix's link into it, is session 0's: the copy here is
     * never written (a launcher-started server always has session 0, session0_init) */
    info->kind = machine || is_shared_machine_file( filename ) ? HIVE_MACHINE_COPY : HIVE_FILE;
    save_branch_count++;
    make_object_permanent( &key->obj );
    return (f != NULL);
}

#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
/* WineExplorer E1 lab: the prefix id and the S13/S14 managed hives (R5: wex_e1lab_registry.h) */
#define WEX_E1LAB_REGISTRY_INCLUDED
#include "wex_e1lab_registry.h"
#endif
#if !(defined(WEX_E1S13_HKU_TEST) && defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__))
static int e1s_save_hive( struct e1s_hive *m ) { set_error( STATUS_NOT_SUPPORTED ); return 0; }
#endif

DECL_HANDLER(load_e1s_profile)
{
#if defined(WEX_E1S13_HKU_TEST) && defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    e1lab_load_e1s_profile( req, reply );
#else
    set_error( STATUS_NOT_SUPPORTED );
#endif
}

static WCHAR *format_user_registry_path( const struct sid *sid, struct unicode_str *path )
{
    char buffer[7 + 11 + 11 + 11 * ARRAY_SIZE(sid->sub_auth)];
    unsigned int i;
    int len;

    len = snprintf( buffer, sizeof(buffer), "User\\S-%u-%u", sid->revision,
                    ((unsigned int)sid->id_auth[2] << 24) |
                    ((unsigned int)sid->id_auth[3] << 16) |
                    ((unsigned int)sid->id_auth[4] << 8) |
                    ((unsigned int)sid->id_auth[5]) );
    for (i = 0; i < sid->sub_count; i++) len += snprintf( buffer + len, sizeof(buffer) - len, "-%u", sid->sub_auth[i] );
    return ascii_to_unicode_str( buffer, path );
}

/* The UWP compatibility gate of a user (HKCU\Software\WineExplorer\Shell\UwpCompatibility): exactly the DWORD 1 turns it on;
 * an absent key or value, another type, length or value leave it off. Same rule as the client side of the gate in
 * twinapi.appcore, decided again here so that a process that asks the server directly gets no package context either. */
int uwp_compatibility_enabled( const struct sid *user )
{
    static const WCHAR tail[] = {'\\','S','o','f','t','w','a','r','e','\\','W','i','n','e','E','x','p','l','o','r','e','r',
                                 '\\','S','h','e','l','l'};
    static const WCHAR value_name[] = {'U','w','p','C','o','m','p','a','t','i','b','i','l','i','t','y'};
    struct unicode_str name = { value_name, sizeof(value_name) }, path;
    struct key_value *value;
    struct key *key;
    WCHAR *full, *full_path;
    unsigned int dword;
    int index, ret = 0;

    if (!root_key || !user || !(full = format_user_registry_path( user, &path ))) return 0;
    if ((full_path = malloc( path.len + sizeof(tail) )))
    {
        memcpy( full_path, full, path.len );
        memcpy( (char *)full_path + path.len, tail, sizeof(tail) );
        if ((key = find_subkey_by_path( root_key, full_path, path.len + sizeof(tail) )) &&
            (value = find_value( key, name, &index )) && value->type == REG_DWORD && value->len == sizeof(dword))
        {
            memcpy( &dword, value->data, sizeof(dword) );
            ret = dword == 1;
        }
        free( full_path );
    }
    free( full );
    return ret;
}

#ifdef __aarch64__
static bool supports_aarch32(void)
{
#if defined(HAVE_SYS_PERSONALITY_H)
    int old = personality( PER_LINUX32 );
    if (old == -1) return false;
    personality( old );
    return true;
#endif
    return false;
}
#endif

static void init_supported_machines(void)
{
    unsigned int count = 0;
#ifdef __i386__
    if (prefix_type == PREFIX_32BIT) supported_machines[count++] = IMAGE_FILE_MACHINE_I386;
#elif defined(__x86_64__)
    if (prefix_type == PREFIX_64BIT) supported_machines[count++] = IMAGE_FILE_MACHINE_AMD64;
    supported_machines[count++] = IMAGE_FILE_MACHINE_I386;
#elif defined(__arm__)
    if (prefix_type == PREFIX_32BIT) supported_machines[count++] = IMAGE_FILE_MACHINE_ARMNT;
#elif defined(__aarch64__)
    if (prefix_type == PREFIX_64BIT)
    {
        supported_machines[count++] = IMAGE_FILE_MACHINE_ARM64;
        supported_machines[count++] = IMAGE_FILE_MACHINE_I386;
        if (supports_aarch32()) supported_machines[count++] = IMAGE_FILE_MACHINE_ARMNT;
        supported_machines[count++] = IMAGE_FILE_MACHINE_AMD64;
    }
#else
#error Unsupported machine
#endif
    supported_machines_count = count;
    native_machine = supported_machines[0];
}

/* registry initialisation */
void init_registry(void)
{
    static const WCHAR REGISTRY[] = {'\\','R','E','G','I','S','T','R','Y'};
    static const WCHAR HKLM[] = { 'M','a','c','h','i','n','e' };
    static const WCHAR HKU_default[] = { 'U','s','e','r','\\','.','D','e','f','a','u','l','t' };
    static const WCHAR classes_i386[] = {'S','o','f','t','w','a','r','e','\\',
                                         'C','l','a','s','s','e','s','\\',
                                         'W','o','w','6','4','3','2','N','o','d','e'};
    static const WCHAR classes_arm[] = {'S','o','f','t','w','a','r','e','\\',
                                        'C','l','a','s','s','e','s','\\',
                                        'W','o','w','A','A','3','2','N','o','d','e'};
    static const WCHAR perflib[] = {'S','o','f','t','w','a','r','e','\\',
                                    'M','i','c','r','o','s','o','f','t','\\',
                                    'W','i','n','d','o','w','s',' ','N','T','\\',
                                    'C','u','r','r','e','n','t','V','e','r','s','i','o','n','\\',
                                    'P','e','r','f','l','i','b','\\',
                                    '0','0','9'};
    static const WCHAR controlset[] = {'S','y','s','t','e','m','\\',
                                       'C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t'};
    static const WCHAR controlset001_path[] = {'\\','R','e','g','i','s','t','r','y','\\',
                                               'M','a','c','h','i','n','e','\\',
                                               'S','y','s','t','e','m','\\',
                                               'C','o','n','t','r','o','l','S','e','t','0','0','1'};
    static const struct unicode_str root_name = { REGISTRY, sizeof(REGISTRY) };
    static const struct unicode_str HKLM_name = { HKLM, sizeof(HKLM) };
    static const struct unicode_str HKU_name = { HKU_default, sizeof(HKU_default) };
    static const struct unicode_str perflib_name = { perflib, sizeof(perflib) };
    static const struct unicode_str controlset_name = { controlset, sizeof(controlset) };

    WCHAR *current_user_path;
    struct unicode_str current_user_str;
    struct key *key, *hklm, *hkcu;
    unsigned int i;
    char *p;
    struct key_init_data data = { .modif = current_time };
    struct object_params params = { .ops = &key_ops, .name = root_name,
                                    .attr = OBJ_PERMANENT, .init_data = &data };


    /* switch to the config dir */

    if (fchdir( config_dir_fd ) == -1) fatal_error( "chdir to config dir: %s\n", strerror( errno ));

    /* create the root key */
    root_key = create_named_object( &params );
    assert( root_key );
    release_object( root_key );

    /* load system.reg into Registry\Machine */

    if (!(hklm = create_key_recursive( root_key, HKLM_name, current_time )))
        fatal_error( "could not create Machine registry key\n" );

    if (!load_init_registry_from_file( sysrun_active ? machine_hive_path( "system.reg" ) : "system.reg",
                                       hklm, sysrun_active ))
    {
        if ((p = getenv( "WINEARCH" )) && !strcmp( p, "win32" ))
            prefix_type = PREFIX_32BIT;
        else
            prefix_type = sizeof(void *) > sizeof(int) ? PREFIX_64BIT : PREFIX_32BIT;

        if ((key = create_key_recursive( hklm, controlset_name, current_time )))
        {
            key->flags |= KEY_SYMLINK;
            set_value( key, symlink_str, REG_LINK, controlset001_path, sizeof(controlset001_path) );
            release_object( key );
        }
    }
    else if (prefix_type == PREFIX_UNKNOWN)
        prefix_type = PREFIX_32BIT;

    init_supported_machines();

    /* load userdef.reg into Registry\User\.Default */

    if (!(key = create_key_recursive( root_key, HKU_name, current_time )))
        fatal_error( "could not create User\\.Default registry key\n" );

    load_init_registry_from_file( sysrun_active ? machine_hive_path( "userdef.reg" ) : "userdef.reg",
                                  key, sysrun_active );
    release_object( key );

    /* load user.reg into HKEY_CURRENT_USER */

    /* FIXME: match default user in token.c. should get from process token instead */
    current_user_path = format_user_registry_path( security_server_user_sid(), &current_user_str );
    if (!current_user_path ||
        !(hkcu = create_key_recursive( root_key, current_user_str, current_time )))
        fatal_error( "could not create HKEY_CURRENT_USER registry key\n" );
    free( current_user_path );
    load_init_registry_from_file( "user.reg", hkcu, false );
    drop_saved_volatile_environment( hkcu );

    /* set the shared flag on Software\Classes\Wow6432Node for all platforms */
    for (i = 1; i < supported_machines_count; i++)
    {
        struct unicode_str name;

        switch (supported_machines[i])
        {
        case IMAGE_FILE_MACHINE_I386:  name.str = classes_i386;  name.len = sizeof(classes_i386);  break;
        case IMAGE_FILE_MACHINE_ARMNT: name.str = classes_arm;   name.len = sizeof(classes_arm);   break;
        default: continue;
        }
        if ((key = create_key_recursive( hklm, name, current_time )))
        {
            key->flags |= KEY_WOWSHARE;
            release_object( key );
        }
        /* FIXME: handle HKCU too */
    }

    if ((key = create_key_recursive( hklm, perflib_name, current_time )))
    {
        key->flags |= KEY_PREDEF;
        release_object( key );
    }

    release_object( hklm );
    release_object( hkcu );

    for (i = 0; i < save_branch_count; i++)
    {
        if (save_branch_info[i].loaded)
            make_clean( save_branch_info[i].key );
    }
    registry_loaded = true;

    /* With a system role server the service profiles exist from the start, so that a limited user cannot create
     * them (and their subkeys) through its create access to the User key; they get the machine default descriptor. */
    if (sysrun_active)
    {
        static const char * const service_profiles[] = { "User\\S-1-5-18", "User\\S-1-5-19", "User\\S-1-5-20" };

        for (i = 0; i < ARRAY_SIZE(service_profiles); i++)
        {
            struct unicode_str name;
            WCHAR *path = ascii_to_unicode_str( service_profiles[i], &name );

            if (path && (key = create_key_recursive( root_key, name, current_time ))) release_object( key );
            free( path );
        }
    }

    /* start the periodic save timer */
    set_periodic_save_timer();

    /* create windows directories */

    if (!mkdir( "drive_c/windows", 0777 ))
    {
        mkdir( "drive_c/windows/system32", 0777 );
        for (i = 1; i < supported_machines_count; i++)
        {
            switch (supported_machines[i])
            {
            case IMAGE_FILE_MACHINE_I386:  mkdir( "drive_c/windows/syswow64", 0777 ); break;
            case IMAGE_FILE_MACHINE_ARMNT: mkdir( "drive_c/windows/sysarm32", 0777 ); break;
            }
        }
    }

    /* go back to the server dir */
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));

    /* WineExplorer: the machine hive is loaded, the system role's switch can be read now and for good */
    sysrun_latch_role();
}

/* save a registry branch to a file */
static void save_all_subkeys_from( struct key *header_key, struct key *save_root, FILE *f )
{
    fprintf( f, "WINE REGISTRY Version 2\n" );
    fprintf( f, ";; All keys relative to " );
    dump_path( header_key, NULL, f );
    fprintf( f, "\n" );
    switch (prefix_type)
    {
    case PREFIX_32BIT:
        fprintf( f, "\n#arch=win32\n" );
        break;
    case PREFIX_64BIT:
        fprintf( f, "\n#arch=win64\n" );
        break;
    default:
        break;
    }
    save_subkeys( save_root, save_root, f );
}

static void save_all_subkeys( struct key *key, FILE *f )
{
    save_all_subkeys_from( key, key, f );
}

/* save a registry branch to a file handle */
static void save_registry( struct key *key, obj_handle_t handle )
{
    struct file *file;
    int fd;

    if (key->e1s_sd) { set_error( STATUS_ACCESS_DENIED ); return; }

    if (!(file = get_file_obj( current->process, handle, FILE_WRITE_DATA ))) return;
    fd = dup( get_file_unix_fd( file ) );
    release_object( file );
    if (fd != -1)
    {
        FILE *f = fdopen( fd, "w" );
        if (f)
        {
            save_all_subkeys( key, f );
            if (fclose( f )) file_set_error();
        }
        else
        {
            file_set_error();
            close( fd );
        }
    }
}

/* Save a branch of the server's own (HIVE_FILE) to its file: a hive reached through a symbolic link is written
 * where the link points; a temporary file next to it, with the old file's mode, is renamed over it. A file or
 * directory that this server may not write is left as it is and the changes stay in memory (as before R4c). */
static int do_save_branch( struct save_branch_info *info )
{
    struct key *key = info->key;
    const char *filename = info->filename;
    char target_path[PATH_MAX], target_dir[PATH_MAX], tmp[PATH_MAX];
    struct stat st, lst;
    const char *slash;
    int fd = -1, count = 0, ret = 0, exists;
    mode_t mode = 0664;
    FILE *f;

    if (!(key->flags & KEY_DIRTY))
    {
        if (debug_level > 1) dump_operation( key, NULL, "Not saving clean" );
        return 1;
    }

    if (!lstat( filename, &lst ) && S_ISLNK( lst.st_mode ))
    {
        if (!realpath( filename, target_path ))
        {
            ssize_t len = readlink( filename, target_path, sizeof(target_path) - 1 );
            if (len < 0) return 0;
            target_path[len] = 0;
        }
    }
    else if (snprintf( target_path, sizeof(target_path), "%s", filename ) >= (int)sizeof(target_path)) return 0;

    if ((slash = strrchr( target_path, '/' )))
    {
        size_t dirlen = slash == target_path ? 1 : slash - target_path;
        if (dirlen >= sizeof(target_dir)) return 0;
        memcpy( target_dir, target_path, dirlen );
        target_dir[dirlen] = 0;
    }
    else strcpy( target_dir, "." );

    exists = !stat( target_path, &st );
    if (exists) mode = st.st_mode & 0777;
    if ((exists && access( target_path, W_OK ) && (errno == EACCES || errno == EROFS)) ||
        (access( target_dir, W_OK ) && (errno == EACCES || errno == EROFS)))
    {
        make_clean( key );  /* not ours to write: the changes stay in memory */
        return 1;
    }

    for (;;)
    {
        if (snprintf( tmp, sizeof(tmp), "%s/reg%lx%04x.tmp", target_dir, (long)getpid(), count++ ) >= (int)sizeof(tmp))
            return 0;
        if ((fd = open( tmp, O_CREAT | O_EXCL | O_WRONLY, 0600 )) != -1) break;
        if (errno != EEXIST) return 0;
    }

    if (!(f = fdopen( fd, "w" )))
    {
        unlink( tmp );
        close( fd );
        return 0;
    }

    if (debug_level > 1)
    {
        fprintf( stderr, "%s: ", target_path );
        dump_operation( key, NULL, "saving" );
    }

    save_all_subkeys( key, f );
    ret = !fflush( f ) && !fsync( fileno( f ) ) && !fchmod( fileno( f ), mode );
    if (fclose( f )) ret = 0;
    if (ret) ret = !rename( tmp, target_path );
    if (!ret) unlink( tmp );
    if (ret) make_clean( key );
    return ret;
}

/* System tier T4a (docs/SYSTEM_TIER_WORK_ORDERS.md): session 0 holds a user's hive for the user's own server,
 * which reads user.reg for it at the first program's registry call and writes what session 0 sends back, since
 * session 0's account cannot write in the user's home. */

static struct save_branch_info *session0_find_agent( void *agent )
{
    int i;

    for (i = 0; i < save_branch_count; i++) if (save_branch_info[i].agent == agent) return &save_branch_info[i];
    return NULL;
}

/* send the hive to its server when it changed; 1 when saved or unchanged */
static int session0_save_user_hive( struct save_branch_info *info )
{
    FILE *f;
    int fd, ret = 0;

    if (!(info->key->flags & KEY_DIRTY)) return 1;
    if ((fd = memfd_create( "wex-user-hive", MFD_CLOEXEC )) == -1) return 0;
    if ((f = fdopen( dup( fd ), "w" )))
    {
        save_all_subkeys( info->key, f );
        if (!fflush( f ) && !ferror( f ) && session0_send_hive( info->agent, fd ))
        {
            /* clean from now on; HIVE_SAVED with a failure, or none, makes it dirty again */
            make_clean( info->key );
            info->save_pending++;
            ret = 1;
        }
        fclose( f );
    }
    close( fd );
    return ret;
}

/* the agent's answer to a hive it was sent (answers come in the order of the saves) */
void session0_user_hive_saved( void *agent, int ok )
{
    struct save_branch_info *info = session0_find_agent( agent );

    if (!info || !info->save_pending) return;
    info->save_pending--;
    if (!ok) info->key->flags |= KEY_DIRTY;  /* the whole hive is sent again at the next period */
}

/* a private copy of the user's hive that the user's server sent: a sealed memory file, so that it cannot change
 * or block while session 0 reads it; NULL when it is not one */
static FILE *session0_hive_snapshot( int fd, char **text )
{
    const int seals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    struct stat st;
    size_t used = 0;
    ssize_t n;
    FILE *f;

    *text = NULL;
    if ((fcntl( fd, F_GET_SEALS ) & seals) != seals || fstat( fd, &st ) || !S_ISREG( st.st_mode ) ||
        st.st_size <= 0 || st.st_size > SESSION0_MAX_USER_HIVE) return NULL;
    if (!(*text = malloc( st.st_size + 1 ))) return NULL;
    while (used < st.st_size)
    {
        n = pread( fd, *text + used, st.st_size - used, used );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += n;
    }
    if (used == st.st_size && !memchr( *text, 0, used ) && (f = fmemopen( *text, used, "r" ))) return f;
    free( *text );
    *text = NULL;
    return NULL;
}

/* load a user's hive (fd: the user's user.reg, or -1 for a new one) under \Registry\User\<sid> for agent */
unsigned int session0_load_user_hive( const struct sid *sid, int fd, void *agent )
{
    static const WCHAR classes_link[] = {'\\','R','e','g','i','s','t','r','y','\\','U','s','e','r','\\'};
    struct save_branch_info *info;
    struct unicode_str path, alias_path, target;
    struct key *key = NULL, *alias = NULL;
    WCHAR *name = NULL, *alias_name = NULL, *link = NULL;
    data_size_t len;
    FILE *f = NULL;
    char *text = NULL;
    unsigned int status;

    const struct default_ace aces[] =
    {
        { sid,                 CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
        { &local_system_sid,   CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
        { &builtin_admins_sid, CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
        { &restricted_code_sid, CONTAINER_INHERIT_ACE, KEY_READ },
    };
    const struct default_ace policy_aces[] =
    {
        { sid,                 CONTAINER_INHERIT_ACE, KEY_READ },
        { &local_system_sid,   CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
        { &builtin_admins_sid, CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
        { &restricted_code_sid, CONTAINER_INHERIT_ACE, KEY_READ },
    };

    if (save_branch_count >= MAX_SAVE_BRANCH_INFO)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    if (fd != -1 && !(f = session0_hive_snapshot( fd, &text )))
    {
        status = STATUS_NOT_REGISTRY_FILE;
        goto done;
    }
    if (!(name = format_user_registry_path( sid, &path )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    clear_error();
    if (!(key = create_key_recursive( root_key, path, current_time )))
    {
        status = get_error() ? get_error() : STATUS_NO_MEMORY;
        goto done;
    }
    if (find_branch_info( key ) && find_branch_info( key )->key == key)
    {
        status = STATUS_OBJECT_NAME_COLLISION;  /* held for another server of the same user */
        goto done;
    }
    if (f)
    {
        load_nofollow = 1;
        load_keys( key, "<user hive>", f, 0 );
        load_nofollow = 0;
    }
    clear_error();

    /* \Registry\User\<sid>_Classes is a link to the hive's Software\Classes, as in the E1 profile hives */
    alias_path.len = path.len + 8 * sizeof(WCHAR);
    len = sizeof(classes_link) + (path.len - 5 * sizeof(WCHAR)) + sizeof(L"\\Software\\Classes") - sizeof(WCHAR);
    if (!(alias_name = mem_alloc( alias_path.len )) || !(link = mem_alloc( len )))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    memcpy( alias_name, path.str, path.len );
    memcpy( (char *)alias_name + path.len, L"_Classes", 8 * sizeof(WCHAR) );
    alias_path.str = alias_name;
    memcpy( link, classes_link, sizeof(classes_link) );
    memcpy( (char *)link + sizeof(classes_link), path.str + 5, path.len - 5 * sizeof(WCHAR) );
    memcpy( (char *)link + sizeof(classes_link) + path.len - 5 * sizeof(WCHAR), L"\\Software\\Classes",
            sizeof(L"\\Software\\Classes") - sizeof(WCHAR) );
    target.str = link;
    target.len = len;
    if ((alias = create_key_recursive( root_key, alias_path, current_time )))
    {
        set_value( alias, symlink_str, REG_LINK, target.str, target.len );
        alias->flags |= KEY_SYMLINK | KEY_VOLATILE;
    }
    clear_error();

    info = &save_branch_info[save_branch_count++];
    memset( info, 0, sizeof(*info) );
    info->key = (struct key *)grab_object( key );
    info->filename = "<user hive>";  /* diagnostics only: the agent saves it */
    info->kind = HIVE_AGENT;
    info->agent = agent;
    info->owner_sd = key_alloc_default_sd( aces, ARRAY_SIZE(aces) );
    info->policy_sd = key_alloc_default_sd( policy_aces, ARRAY_SIZE(policy_aces) );
    info->classes_alias = alias ? (struct key *)grab_object( alias ) : NULL;
    reset_policy_trees( key );
    drop_saved_volatile_environment( key );
    make_clean( key );
    notify_hive_mount( get_parent( key ) );  /* gpsvc (wexpolicy) waits for a user's hive under HKEY_USERS */
    status = STATUS_SUCCESS;

done:
    if (f) fclose( f );
    free( text );
    if (fd != -1) close( fd );
    if (key) release_object( key );
    if (alias) release_object( alias );
    free( name );
    free( alias_name );
    free( link );
    return status;
}

/* the agent's server is leaving: save once more when asked, then drop the hive */
void session0_unload_user_hive( void *agent, int save )
{
    struct save_branch_info *info = session0_find_agent( agent );
    int i;

    if (!info) return;
    if (save) session0_save_user_hive( info );
    i = info - save_branch_info;
    if (info->classes_alias)
    {
        unlink_named_object( &info->classes_alias->obj );
        release_object( info->classes_alias );
    }
    info->key->flags |= KEY_DELETED;
    unlink_named_object( &info->key->obj );
    release_object( info->key );
    free( info->owner_sd );
    free( info->policy_sd );
    if (i != --save_branch_count) save_branch_info[i] = save_branch_info[save_branch_count];
    memset( &save_branch_info[save_branch_count], 0, sizeof(save_branch_info[0]) );
}

static int save_branch( struct save_branch_info *info )
{
    switch (info->kind)
    {
    case HIVE_MANAGED:
        return e1s_save_hive( info->managed );
    case HIVE_AGENT:
        return session0_save_user_hive( info );
    case HIVE_MACHINE_COPY:
        /* session 0 writes the machine hives; this copy is for the per-session keys and start-up reads. A server
         * the launcher did not start, whose prefix links its hive into the machine root, is told once. */
        if (!sysrun_active && !session0_client && (info->key->flags & KEY_DIRTY) && !info->warned)
        {
            fprintf( stderr, "wineserver: %s is the machine root's hive, which only session 0 writes: changes made "
                     "by this server are not saved\n", info->filename );
            info->warned = true;
        }
        return 1;
    case HIVE_FILE:
        break;
    }
    /* development mode: session 0 (of the server's own account) holds the prefix's machine hives too */
    if (session0_client && (info == &save_branch_info[0] || info == &save_branch_info[1])) return 1;
    /* T4a: the user's hive once session 0 holds it; session 0 sends it back to be written */
    if (session0_user_hive_remote() && info == &save_branch_info[2]) return 1;
    return do_save_branch( info );
}

/* periodic saving of the registry */
static void periodic_save( void *arg )
{
    int i;

    if (fchdir( config_dir_fd ) == -1) return;
    save_timeout_user = NULL;
    for (i = 0; i < save_branch_count; i++)
        save_branch( &save_branch_info[i] );
#if defined(WEX_E1S13_HKU_TEST) && defined(WEX_E1S10_TOKEN_TEST) && defined(WEX_E1S6_TRANSPORT_TEST) && defined(__linux__)
    e1s_reap_hives();
#endif
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));
    set_periodic_save_timer();
}

/* start the periodic save timer */
static void set_periodic_save_timer(void)
{
    if (save_timeout_user) remove_timeout_user( save_timeout_user );
    /* system tier: session 0 holds every session's machine registry; it saves changes within 5 s, as the
     * lazy flush of Windows does (unchanged hives are not written) */
    save_timeout_user = add_timeout_user( session0_server ? 5 * -TICKS_PER_SEC : save_period, periodic_save, NULL );
}

/* save the modified registry branches to disk */
void flush_registry(void)
{
    int i;

    session0_client_exit();  /* T4a: the last changes of the user's hive held by session 0 */
    if (fchdir( config_dir_fd ) == -1) return;
    for (i = 0; i < save_branch_count; i++)
    {
        if (!save_branch( &save_branch_info[i] ))
        {
            if (save_branch_info[i].managed)
            {
                fprintf( stderr, "wineserver: managed profile save failed status=%08x\n", get_error() );
                continue;
            }
            fprintf( stderr, "wineserver: could not save registry branch to %s",
                     save_branch_info[i].filename );
            perror( " " );
        }
    }
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));
}


/* Windows loads the profile of a service account (S-1-5-18, S-1-5-19, S-1-5-20) before it starts the service, so the
 * account's HKEY_CURRENT_USER exists for it. Here nothing loads it when the machine's server has none yet, so the
 * account may create its own profile key below User, and only that one. */
static int service_profile_create_allowed( struct key *parent, const struct unicode_str *left )
{
    static const WCHAR user[] = {'U','s','e','r'};
    static const WCHAR prefix[] = {'S','-','1','-','5','-'};
    struct token *token = current->token ? current->token : current->process->token;
    const struct sid *sid;
    const WCHAR *name;
    data_size_t len;
    WCHAR expected[8];
    unsigned int rid;

    if (!token || get_parent( parent ) != root_key || !(name = get_object_name( &parent->obj, &len ))) return FALSE;
    if (len != sizeof(user) || memicmp_strW( name, user, sizeof(user) )) return FALSE;
    sid = token_get_user_sid( token );
    if (sid->sub_count != 1 || sid->id_auth[5] != 5) return FALSE;
    rid = sid->sub_auth[0];
    if (rid != 18 && rid != 19 && rid != 20) return FALSE;
    memcpy( expected, prefix, sizeof(prefix) );
    expected[6] = '0' + rid / 10;
    expected[7] = '0' + rid % 10;
    return left->len == sizeof(expected) && !memicmp_strW( left->str, expected, sizeof(expected) );
}

/* a key that does not exist yet needs KEY_CREATE_SUB_KEY on the parent it would be created in */
static int check_create_subkey( const struct object_params *params )
{
    struct unicode_str left;
    struct object *parent;
    unsigned int access = KEY_CREATE_SUB_KEY;
    int ret = TRUE;

    if (!params->name.len) return TRUE;
    /* a lookup failure is reported by the create itself */
    if (!(parent = lookup_named_object( params->root, params->name, params->attr, &left ))) return TRUE;
    if (left.len && parent->ops == &key_ops && !check_object_access( NULL, parent, &access ))
    {
        /* never a link: a profile key made a link would send the account's HKEY_CURRENT_USER, and whatever a
         * privileged program writes there, into another tree */
        ret = key_in_display_cache( (struct key *)parent, &left ) ||
              (!(params->attr & OBJ_OPENLINK) && service_profile_create_allowed( (struct key *)parent, &left ));
        if (ret) clear_error();
    }
    release_object( parent );
    return ret;
}

/* create a registry key */
static const struct sid creator_owner_sid = { SID_REVISION, 1, SECURITY_CREATOR_SID_AUTHORITY, { SECURITY_CREATOR_OWNER_RID } };
static const struct sid creator_group_sid = { SID_REVISION, 1, SECURITY_CREATOR_SID_AUTHORITY, { SECURITY_CREATOR_GROUP_RID } };

/* one ACE of an inherited DACL at out: the parent's ace with these flags and this mask, its SID replaced by sid when
 * given (only a plain allowed or denied ACE has its SID right after the header); returns the next ACE */
static struct ace *inherit_ace( struct ace *out, const struct ace *ace, unsigned char flags, unsigned int mask,
                                const struct sid *sid )
{
    if (sid && (ace->type == ACCESS_ALLOWED_ACE_TYPE || ace->type == ACCESS_DENIED_ACE_TYPE))
        set_ace( out, sid, ace->type, flags, mask );
    else
    {
        memcpy( out, ace, ace->size );
        out->flags = flags;
        out->mask = mask;
    }
    return ace_next( out );
}

/* The DACL of a new key, as Windows makes it (RtlNewSecurityObject): the explicit ACEs given at creation, then,
 * unless that DACL is protected or NULL, the inheritable ACEs of the parent's own descriptor. Only a parent with a
 * descriptor of its own takes part; keys under the built-in default descriptors keep them.
 *
 * Each ACE that applies to the key itself has its generic rights mapped and CREATOR OWNER / CREATOR GROUP replaced by
 * the key's owner / primary group; when such an ACE also passes on (container-inheritable, without no-propagate) and
 * the effective form differs, an inherit-only copy as it was follows for the keys below. That holds for an explicit
 * ACE as for an inherited one: an installer's own "CREATOR OWNER full control, inherit" gives itself access to the
 * key it creates (B46). Inherited ACEs carry INHERITED_ACE. */
static struct ace *new_key_ace( struct ace *out, const struct ace *ace, BOOL inherited, const struct sid *owner,
                                const struct sid *group, unsigned int *count )
{
    const unsigned int inherit = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    const unsigned int generic = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL;
    const unsigned char mark = inherited ? INHERITED_ACE : 0;
    const struct sid *sid = (const struct sid *)(ace + 1), *effective_sid = NULL;
    BOOL passes_on;

    if (inherited && !(ace->flags & CONTAINER_INHERIT_ACE))
    {
        /* object-inherit only: kept for the objects below, effective for none of them here */
        if (ace->flags & NO_PROPAGATE_INHERIT_ACE) return out;
        (*count)++;
        return inherit_ace( out, ace, (ace->flags & ~(CONTAINER_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE)) |
                            INHERIT_ONLY_ACE | INHERITED_ACE, ace->mask, NULL );
    }
    if (!inherited && (ace->flags & INHERIT_ONLY_ACE))
    {
        /* an explicit inherit-only ACE is for the keys below only: as it is */
        (*count)++;
        return inherit_ace( out, ace, ace->flags, ace->mask, NULL );
    }
    if (ace->type == ACCESS_ALLOWED_ACE_TYPE || ace->type == ACCESS_DENIED_ACE_TYPE)
    {
        if (equal_sid( sid, &creator_owner_sid )) effective_sid = owner;
        else if (equal_sid( sid, &creator_group_sid )) effective_sid = group;
    }
    passes_on = (ace->flags & CONTAINER_INHERIT_ACE) && !(ace->flags & NO_PROPAGATE_INHERIT_ACE);
    if (inherited && (ace->flags & NO_PROPAGATE_INHERIT_ACE))
    {
        (*count)++;
        return inherit_ace( out, ace, mark, map_access( ace->mask, &key_type.mapping ), effective_sid );
    }
    if ((ace->mask & generic) || effective_sid)
    {
        /* the effective form for this key, and (when the ACE passes on) the original for the keys below */
        out = inherit_ace( out, ace, inherited || passes_on ? mark : (ace->flags & ~INHERIT_ONLY_ACE) | mark,
                           map_access( ace->mask, &key_type.mapping ), effective_sid );
        (*count)++;
        if (!passes_on) return out;
        (*count)++;
        return inherit_ace( out, ace, (ace->flags & (inherit | NO_PROPAGATE_INHERIT_ACE)) | INHERIT_ONLY_ACE | mark,
                            ace->mask, NULL );
    }
    (*count)++;
    return inherit_ace( out, ace, inherited ? (ace->flags & inherit) | mark : ace->flags, ace->mask, NULL );
}

static void key_inherit_sd( struct key *key, const struct security_descriptor *explicit_sd )
{
    const unsigned int inherit = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    struct token *token = current->token ? current->token : current->process->token;
    const struct acl *parent_dacl = NULL, *explicit_dacl = NULL;
    const struct sid *owner, *group;
    const struct ace *ace;
    struct security_descriptor *sd;
    struct key *parent = get_parent( key );
    struct acl *dacl;
    struct ace *out;
    unsigned int i, count = 0;
    data_size_t size = sizeof(struct acl), extra;
    int present = 0;

    if (explicit_sd)
    {
        explicit_dacl = sd_get_dacl( explicit_sd, &present );
        if (present && !explicit_dacl) return;  /* a NULL DACL: no access check, nothing to add */
        if (!present) explicit_dacl = NULL;
    }
    if (parent && parent->obj.sd && !(explicit_sd && (explicit_sd->control & SE_DACL_PROTECTED)))
    {
        if (!(parent_dacl = sd_get_dacl( parent->obj.sd, &present )) || !present) parent_dacl = NULL;
    }
    if (!explicit_dacl && !parent_dacl) return;
    owner = key->obj.sd && key->obj.sd->owner_len ? sd_get_owner( key->obj.sd ) : token_get_owner( token );
    group = key->obj.sd && key->obj.sd->group_len ? sd_get_group( key->obj.sd ) : token_get_primary_group( token );
    extra = max( sid_len( owner ), sid_len( group ) );

    /* the size, at most: each ACE may become two, with a creator's SID replaced by a longer one */
    if (explicit_dacl)
        for (i = 0, ace = ace_first( explicit_dacl ); i < explicit_dacl->count; i++, ace = ace_next( ace ))
            size += 2 * ace->size + extra;
    if (parent_dacl)
        for (i = 0, ace = ace_first( parent_dacl ); i < parent_dacl->count; i++, ace = ace_next( ace ))
            if (ace->flags & inherit) size += 2 * ace->size + extra;
    if (!(sd = mem_alloc( sizeof(*sd) + size ))) return;
    memset( sd, 0, sizeof(*sd) );
    sd->control = SE_DACL_PRESENT | (parent_dacl ? SE_DACL_AUTO_INHERITED : 0) |
                  (explicit_sd ? explicit_sd->control & SE_DACL_PROTECTED : 0);
    dacl = (struct acl *)(sd + 1);
    dacl->revision = ACL_REVISION;
    dacl->pad1 = 0;
    dacl->pad2 = 0;
    out = ace_first( dacl );
    if (explicit_dacl)
        for (i = 0, ace = ace_first( explicit_dacl ); i < explicit_dacl->count; i++, ace = ace_next( ace ))
            out = new_key_ace( out, ace, FALSE, owner, group, &count );
    if (parent_dacl)
        for (i = 0, ace = ace_first( parent_dacl ); i < parent_dacl->count; i++, ace = ace_next( ace ))
            if (ace->flags & inherit) out = new_key_ace( out, ace, TRUE, owner, group, &count );
    if (!explicit_dacl && !count)
    {
        /* nothing given and nothing inherited: the key keeps the default it has (Windows: the creator's default
         * DACL), never an empty DACL that would deny everyone */
        free( sd );
        return;
    }
    dacl->count = count;
    dacl->size = (char *)out - (char *)dacl;
    sd->dacl_len = dacl->size;
    default_set_sd( &key->obj, sd, DACL_SECURITY_INFORMATION );
    free( sd );
}

DECL_HANDLER(create_key)
{
    struct key *key, *parent = NULL, *mapped = NULL;
    unsigned int access = req->access;
    struct key_init_data data = { .options = req->options, .modif = current_time };
    struct object_params params = { .ops = &key_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;
    if (params.root) release_object( params.root );
    data.class = get_req_data_after_objattr( &params, &data.classlen );

    if (token_e1s_account( current->token ? current->token : current->process->token ) &&
        !e1s_find_hive( token_e1s_account( current->token ? current->token : current->process->token ) ))
    { set_error( STATUS_OBJECT_NAME_NOT_FOUND ); return; }

    /* system tier: a registry-only client of session 0 states its program's view in every request */
    if (!is_wow64_process( current->process ) && !current->process->wex_registry_client)
        access = (access & ~KEY_WOW64_32KEY) | KEY_WOW64_64KEY;
    if (!(access & KEY_WOW64_64KEY)) params.attr |= OBJ_KEY_WOW64;
    if (req->options & REG_OPTION_CREATE_LINK) params.attr = (params.attr & ~OBJ_OPENIF) | OBJ_OPENLINK;

    if (params.objattr->rootdir)
    {
        if (!(parent = get_hkey_obj( params.objattr->rootdir, 0 ))) return;
        params.root = get_wow6432_parent( parent, access, params.name );
        if (parent->e1s_view)
        {
            if (!(mapped = e1s_view_user( parent, 1 ))) goto done;
            params.root = get_wow6432_parent( mapped, access, params.name );
        }
    }

    if (elevation_enforced() && !check_create_subkey( &params )) goto done;

    if ((key = params.name.len ? create_named_object( &params ) : open_named_object( &params )))
    {
        if (mapped)
        {
            struct object_params view_params = { .ops = &key_ops, .root = &parent->obj,
                                                  .name = params.name, .attr = params.attr };
            release_object( key );
            if (!(key = open_named_object( &view_params ))) goto done;
        }
        if (!params.name.len || get_error() == STATUS_OBJECT_NAME_EXISTS)
        {
            if (key->flags & KEY_PREDEF) set_error( STATUS_PREDEFINED_HANDLE );
            if (!(access & KEY_WOW64_64KEY)) key = grab_wow6432node( key );
            reply->hkey = e1s_alloc_key_handle( key, access, params.attr );
        }
        else
        {
            if (!mapped && !key->e1s_sd) key_inherit_sd( key, params.sd );
            reply->hkey = key->e1s_sd ? e1s_alloc_key_handle( key, access, params.attr ) :
                alloc_handle_no_access_check( current->process, key, access, params.attr );
            if (parent)
                touch_key_ex( get_parent( key ), REG_NOTIFY_CHANGE_NAME, !(key->flags & KEY_VOLATILE) );
            if (debug_level > 1) dump_operation( key, NULL, "Create" );
        }
        release_object( key );
    }

done:
    if (mapped) release_object( mapped );
    if (parent) release_object( parent );
}

/* open a registry key */
DECL_HANDLER(open_key)
{
    struct key *key, *parent = NULL;
    unsigned int access = req->access;
    struct object_params params = { .ops = &key_ops, .name = get_req_unicode_str(),
                                    .attr = req->attributes };

    /* system tier: a registry-only client of session 0 states its program's view in every request */
    if (!is_wow64_process( current->process ) && !current->process->wex_registry_client)
        access = (access & ~KEY_WOW64_32KEY) | KEY_WOW64_64KEY;
    if (!(access & KEY_WOW64_64KEY)) params.attr |= OBJ_KEY_WOW64;

    if (req->parent)
    {
        if (!(parent = get_hkey_obj( req->parent, 0 ))) return;
        params.root = get_wow6432_parent( parent, access, params.name );
    }

    if ((key = open_named_object( &params )))
    {
        if (!(access & KEY_WOW64_64KEY)) key = grab_wow6432node( key );
        if (key->flags & KEY_PREDEF) set_error( STATUS_PREDEFINED_HANDLE );
        reply->hkey = e1s_alloc_key_handle( key, access, req->attributes );
        if (debug_level > 1) dump_operation( key, NULL, "Open" );
        release_object( key );
    }
    if (parent) release_object( parent );
}

/* delete a registry key */
DECL_HANDLER(delete_key)
{
    struct key *key = get_hkey_obj( req->hkey, DELETE );

    if (key)
    {
        delete_key( key, 0 );
        release_object( key );
    }
}

/* flush a registry key */
DECL_HANDLER(flush_key)
{
    struct key *key = get_hkey_obj( req->hkey, 0 );
    if (key)
    {
        if (key->e1s_sd)
        {
            struct e1s_hive *m;
            for (m = e1s_hives; m && m->sd != key->e1s_sd; m = m->next) ;
            if (!m) set_error( STATUS_REGISTRY_CORRUPT );
            else e1s_save_hive( m );
        }
        release_object( key );
    }
}

/* enumerate registry subkeys */
DECL_HANDLER(enum_key)
{
    struct key *key;

    if ((key = get_hkey_obj( req->hkey, req->index == -1 ? 0 : KEY_ENUMERATE_SUB_KEYS )))
    {
        enum_key( key, req->index, req->info_class, reply );
        release_object( key );
    }
}

/* set a value of a registry key */
DECL_HANDLER(set_key_value)
{
    struct key *key;
    struct unicode_str name;

    if (req->namelen > get_req_data_size())
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    name.str = get_req_data();
    name.len = (req->namelen / sizeof(WCHAR)) * sizeof(WCHAR);

    if ((key = get_hkey_obj( req->hkey, KEY_SET_VALUE )))
    {
        data_size_t datalen = get_req_data_size() - req->namelen;
        const char *data = (const char *)get_req_data() + req->namelen;

        set_value( key, name, req->type, data, datalen );
        release_object( key );
    }
}

/* retrieve the value of a registry key */
DECL_HANDLER(get_key_value)
{
    struct key *key;
    struct unicode_str name = get_req_unicode_str();

    reply->total = 0;
    if ((key = get_hkey_obj( req->hkey, KEY_QUERY_VALUE )))
    {
        get_value( key, name, &reply->type, &reply->total );
        release_object( key );
    }
}

/* enumerate the value of a registry key */
DECL_HANDLER(enum_key_value)
{
    struct key *key;

    if ((key = get_hkey_obj( req->hkey, KEY_QUERY_VALUE )))
    {
        enum_value( key, req->index, req->info_class, reply );
        release_object( key );
    }
}

/* delete a value of a registry key */
DECL_HANDLER(delete_key_value)
{
    struct key *key;
    struct unicode_str name = get_req_unicode_str();

    if ((key = get_hkey_obj( req->hkey, KEY_SET_VALUE )))
    {
        delete_value( key, name );
        release_object( key );
    }
}

/* load a registry branch from a file */
DECL_HANDLER(load_registry)
{
    struct key *key, *parent = NULL;
    struct key_init_data data = { .modif = current_time };
    struct object_params params = { .ops = &key_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;
    if (params.root) release_object( params.root );

    if (!thread_single_check_privilege( current, SeRestorePrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }
    if (params.objattr->rootdir)
    {
        if (!(parent = get_hkey_obj( params.objattr->rootdir, 0 ))) return;
        params.root = &parent->obj;
    }

    if ((key = create_named_object( &params )))
    {
        if (key->e1s_sd) set_error( STATUS_ACCESS_DENIED );
        else
        {
            load_registry( key, req->file );
            if (!get_error()) notify_hive_mount( get_parent( key ) );
        }
        release_object( key );
    }
    if (parent) release_object( parent );
}

DECL_HANDLER(unload_registry)
{
    struct key *key, *parent = NULL;
    struct object_params params = { .ops = &key_ops, .name = get_req_unicode_str(),
                                    .attr = req->attributes };

    if (!thread_single_check_privilege( current, SeRestorePrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if (req->parent)
    {
        if (!(parent = get_hkey_obj( req->parent, 0 ))) return;
        params.root = &parent->obj;
    }

    if ((key = open_named_object( &params )))
    {
        if (key->e1s_sd) set_error( STATUS_ACCESS_DENIED ); /* only managed broker operation can unload */
        else if (key->obj.handle_count)
            set_error( STATUS_CANNOT_DELETE );
        else if (key->obj.is_permanent)
            set_error( STATUS_ACCESS_DENIED );
        else
            delete_key( key, 1 );     /* FIXME */
        release_object( key );
    }
    if (parent) release_object( parent );
}

/* save a registry branch to a file */
DECL_HANDLER(save_registry)
{
    struct key *key;

    if (!thread_single_check_privilege( current, SeBackupPrivilege ))
    {
        set_error( STATUS_PRIVILEGE_NOT_HELD );
        return;
    }

    if ((key = get_hkey_obj( req->hkey, 0 )))
    {
        save_registry( key, req->file );
        release_object( key );
    }
}

/* add a registry key change notification */
DECL_HANDLER(set_registry_notification)
{
    struct key *key;
    struct event *event;
    struct notify *notify;

    key = get_hkey_obj( req->hkey, KEY_NOTIFY );
    if (key)
    {
        if (key->e1s_view)
        { set_error( STATUS_NOT_SUPPORTED ); release_object( key ); return; }
        if (current->process->wex_registry_client && !req->event && req->relay_cookie)
        {
            /* system tier: the change is reported to the program's own server, which sets its event */
            unsigned int *cookies;

            /* the same bound as the program's relay: a client sending raw requests cannot grow session 0 */
            if (current->process->wex_relay_cookies >= SESSION0_MAX_COOKIES)
            { set_error( STATUS_INSUFFICIENT_RESOURCES ); release_object( key ); return; }
            if (!(notify = find_notify( key, current->process, req->hkey )) && (notify = mem_alloc( sizeof(*notify) )))
            {
                memset( notify, 0, sizeof(*notify) );
                notify->subtree = req->subtree;
                notify->filter  = req->filter;
                notify->hkey    = req->hkey;
                notify->process = current->process;
                list_add_head( &key->notify_list, &notify->entry );
            }
            if (notify && (cookies = realloc( notify->cookies, (notify->cookie_count + 1) * sizeof(*cookies) )))
            {
                notify->cookies = cookies;
                notify->cookies[notify->cookie_count++] = req->relay_cookie;
                current->process->wex_relay_cookies++;
                set_error( STATUS_PENDING );
            }
            else if (notify) set_error( STATUS_NO_MEMORY );
            release_object( key );
            return;
        }
        event = get_event_obj( current->process, req->event, SYNCHRONIZE );
        if (event)
        {
            notify = find_notify( key, current->process, req->hkey );
            if (!notify)
            {
                notify = mem_alloc( sizeof(*notify) );
                if (notify)
                {
                    notify->events  = NULL;
                    notify->event_count = 0;
                    notify->cookies = NULL;
                    notify->cookie_count = 0;
                    notify->subtree = req->subtree;
                    notify->filter  = req->filter;
                    notify->hkey    = req->hkey;
                    notify->process = current->process;
                    list_add_head( &key->notify_list, &notify->entry );
                }
            }
            if (notify)
            {
                struct event **new_array;

                if ((new_array = realloc( notify->events, (notify->event_count + 1) * sizeof(*notify->events) )))
                {
                    notify->events = new_array;
                    notify->events[notify->event_count++] = (struct event *)grab_object( event );
                    reset_event( event );
                    set_error( STATUS_PENDING );
                }
                else set_error( STATUS_NO_MEMORY );
            }
            release_object( event );
        }
        release_object( key );
    }
}

/* rename a registry key */
DECL_HANDLER(rename_key)
{
    struct unicode_str name = get_req_unicode_str();
    struct key *key;

    key = (struct key *)get_handle_obj( current->process, req->hkey, KEY_WRITE, &key_ops );
    if (!key) return;
    if (key->e1s_view) { set_error( STATUS_NOT_SUPPORTED ); release_object( key ); return; }
    release_object( key );
    key = get_hkey_obj( req->hkey, KEY_WRITE );
    if (key)
    {
        /* Group Policy: the keys above a policy tree of a user's hive are not moved away by the user, who could
         * then put a tree of their own in its place. Whoever may write the tree may rename them. */
        struct save_branch_info *info = session0_server ? find_branch_info( key ) : NULL;

        if (info && info->policy_sd && key_at_policy_tree( key, info, 0 ))
        {
            if (!policy_write_allowed( info->key ))
            {
                set_error( STATUS_ACCESS_DENIED );
                release_object( key );
                return;
            }
        }
        rename_key( key, name );
        release_object( key );
    }
}

int object_is_registry_key( struct object *obj )
{
    return obj->ops == &key_ops;
}
