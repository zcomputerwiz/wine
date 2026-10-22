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
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/file.h>
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
    uint64_t          mod_seq;     /* modification sequence in session */
};

/* key flags */
#define KEY_VOLATILE 0x0001  /* key is volatile (not saved to disk) */
#define KEY_DELETED  0x0002  /* key has been deleted */
#define KEY_DIRTY    0x0004  /* key has been modified */
#define KEY_SYMLINK  0x0008  /* key is a symbolic link */
#define KEY_WOWSHARE 0x0010  /* key is a Wow64 shared key (used for Software\Classes) */
#define KEY_PREDEF   0x0020  /* key is marked as predefined */

#define OBJ_KEY_WOW64 0x100000 /* magic flag added to attributes for WoW64 redirection */

/* a key value */
struct key_value
{
    WCHAR            *name;    /* value name */
    unsigned short    namelen; /* length of value name */
    unsigned int      type;    /* value type */
    data_size_t       len;     /* value data length in bytes */
    void             *data;    /* pointer to value data */
    uint64_t          mod_seq; /* modification sequence in session */
};

#define MIN_SUBKEYS  8   /* min. number of allocated subkeys per key */
#define MIN_VALUES   8   /* min. number of allocated values per key */

#define MAX_NAME_LEN  256    /* max. length of a key name */
#define MAX_VALUE_LEN 16383  /* max. length of a value name */

/* the root of the registry tree */
static struct key *root_key;

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
static void make_dirty( struct key *key );
static void touch_key_ex( struct key *key, unsigned int change, bool mark_dirty );

struct reg_tombstone
{
    struct list   entry;
    WCHAR        *key_path;
    data_size_t   key_pathlen;
    WCHAR        *val_name;
    data_size_t   val_namelen;
    bool          is_value;
};

/* information about where to save a registry branch */
struct save_branch_info
{
    struct key     *key;
    const char     *filename;
    dev_t           dev;
    ino_t           ino;
    struct timespec mtime;
    bool            has_stat;
    bool            has_ephemeral_mods;
    bool            is_shared_machine;
    struct list     tombstones;
};

#define MAX_SAVE_BRANCH_INFO 3
static int save_branch_count;
static struct save_branch_info save_branch_info[MAX_SAVE_BRANCH_INFO];

static bool registry_loaded = false;
static bool in_merge = false;
static uint64_t next_mod_seq = 0;

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

static bool match_ascii_prefix_ci( const WCHAR *path, data_size_t pathlen, const char *prefix )
{
    size_t plen = strlen( prefix );
    size_t wlen = pathlen / sizeof(WCHAR);
    size_t i;

    if (wlen < plen) return false;
    for (i = 0; i < plen; i++)
    {
        WCHAR c1 = path[i];
        WCHAR c2 = (unsigned char)prefix[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
        if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
        if (c1 != c2) return false;
    }
    return (wlen == plen || path[plen] == '\\');
}

static bool ascii_equals_unicode( const char *ascii, const WCHAR *uni, data_size_t unilen )
{
    size_t len = strlen( ascii );
    size_t i;

    if (unilen != len * sizeof(WCHAR)) return false;
    for (i = 0; i < len; i++)
    {
        WCHAR c1 = uni[i];
        WCHAR c2 = (unsigned char)ascii[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
        if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
        if (c1 != c2) return false;
    }
    return true;
}

static const char * const hklm_ephemeral_prefixes[] =
{
    "System\\ControlSet001\\Enum",
    "System\\CurrentControlSet\\Enum",
    "System\\ControlSet001\\Control\\Class",
    "System\\CurrentControlSet\\Control\\Class",
    "System\\ControlSet001\\Control\\Video",
    "System\\CurrentControlSet\\Control\\Video",
    "System\\ControlSet001\\Control\\DeviceClasses",
    "System\\CurrentControlSet\\Control\\DeviceClasses",
    "System\\ControlSet001\\Control\\Print",
    "System\\CurrentControlSet\\Control\\Print",
    "System\\ControlSet001\\Control\\ComputerName",
    "System\\CurrentControlSet\\Control\\ComputerName",
    "System\\ControlSet001\\Services\\Tcpip\\Parameters",
    "System\\CurrentControlSet\\Services\\Tcpip\\Parameters",
    "Software\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
    "Software\\Wine\\Drives",
    "Software\\Wine\\Ports",
    "Software\\Wow6432Node\\Wine\\Ports",
};

static const char * const hku_default_ephemeral_prefixes[] =
{
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
};

static bool is_per_session_key( const struct key *key )
{
    struct save_branch_info *branch;
    WCHAR *rel_path;
    data_size_t rel_len;
    size_t i;
    bool match = false;

    if (!registry_loaded || in_merge) return false;
    if (geteuid() == 0) return false;

    branch = find_branch_info( key );
    if (!branch || !branch->key) return false;
    if (!branch->is_shared_machine) return false;
    if (save_branch_count >= 3 && branch == &save_branch_info[2]) return false;

    rel_path = build_relative_key_path( key, branch->key, &rel_len );
    if (!rel_path) return false;

    if (branch == &save_branch_info[0])
    {
        for (i = 0; i < ARRAY_SIZE(hklm_ephemeral_prefixes); i++)
        {
            if (match_ascii_prefix_ci( rel_path, rel_len, hklm_ephemeral_prefixes[i] ))
            {
                match = true;
                break;
            }
        }
    }
    else if (branch == &save_branch_info[1])
    {
        for (i = 0; i < ARRAY_SIZE(hku_default_ephemeral_prefixes); i++)
        {
            if (match_ascii_prefix_ci( rel_path, rel_len, hku_default_ephemeral_prefixes[i] ))
            {
                match = true;
                break;
            }
        }
    }

    free( rel_path );
    return match;
}

static void mark_branch_ephemeral( const struct key *key )
{
    struct save_branch_info *branch = find_branch_info( key );
    if (branch) branch->has_ephemeral_mods = true;
}

static bool is_per_session_value( const struct key *key, struct unicode_str val_name )
{
    struct save_branch_info *branch;
    WCHAR *rel_path;
    data_size_t rel_len;
    bool match = false;

    if (!registry_loaded || in_merge) return false;
    if (geteuid() == 0) return false;

    branch = find_branch_info( key );
    if (!branch || branch != &save_branch_info[0]) return false;
    if (!branch->is_shared_machine) return false;

    rel_path = build_relative_key_path( key, branch->key, &rel_len );
    if (!rel_path) return false;

    if (match_ascii_prefix_ci( rel_path, rel_len, "System\\ControlSet001\\Control\\Session Manager\\Environment" ) ||
        match_ascii_prefix_ci( rel_path, rel_len, "System\\CurrentControlSet\\Control\\Session Manager\\Environment" ))
    {
        static const char * const env_cpu_vals[] = {
            "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER",
            "PROCESSOR_LEVEL", "PROCESSOR_REVISION", "NUMBER_OF_PROCESSORS"
        };
        size_t i;
        for (i = 0; i < ARRAY_SIZE(env_cpu_vals); i++)
        {
            if (ascii_equals_unicode( env_cpu_vals[i], val_name.str, val_name.len ))
            {
                match = true;
                break;
            }
        }
    }

    free( rel_path );
    return match;
}

static void record_tombstone( struct key *key, const WCHAR *val_name, data_size_t val_namelen, bool is_value )
{
    struct save_branch_info *branch;
    struct reg_tombstone *ts;
    WCHAR *path;
    data_size_t pathlen;

    if (!registry_loaded || in_merge) return;
    if (key->flags & KEY_VOLATILE) return;
    if (is_per_session_key( key )) return;

    branch = find_branch_info( key );
    if (!branch || !branch->is_shared_machine) return;

    path = build_relative_key_path( key, branch->key, &pathlen );
    if (!path && key != branch->key) return;

    ts = mem_alloc( sizeof(*ts) );
    if (!ts)
    {
        free( path );
        return;
    }
    ts->key_path = path;
    ts->key_pathlen = pathlen;
    ts->is_value = is_value;
    if (is_value && val_namelen)
    {
        ts->val_name = memdup( val_name, val_namelen );
        ts->val_namelen = ts->val_name ? val_namelen : 0;
    }
    else
    {
        ts->val_name = NULL;
        ts->val_namelen = 0;
    }
    list_add_tail( &branch->tombstones, &ts->entry );
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

static void replay_tombstones( struct save_branch_info *info, struct key *tmp_root )
{
    struct reg_tombstone *ts;

    LIST_FOR_EACH_ENTRY( ts, &info->tombstones, struct reg_tombstone, entry )
    {
        struct key *k;
        if (!ts->key_pathlen)
            k = tmp_root;
        else
            k = find_subkey_by_path( tmp_root, ts->key_path, ts->key_pathlen );

        if (!k) continue;

        if (ts->is_value)
        {
            struct unicode_str vname = { ts->val_name, ts->val_namelen };
            delete_value( k, vname );
            clear_error();
        }
        else if (k != tmp_root)
        {
            delete_key( k, 1 );
            clear_error();
        }
    }
}

static void mark_subtree_mod_seq( struct key *key )
{
    int i;
    if (key->flags & KEY_VOLATILE) return;
    key->mod_seq = ++next_mod_seq;
    for (i = 0; i <= key->last_value; i++)
        key->values[i].mod_seq = ++next_mod_seq;
    for (i = 0; i <= key->last_subkey; i++)
        mark_subtree_mod_seq( key->subkeys[i] );
}

static void clear_branch_mod_seq( struct key *key )
{
    int i;
    key->mod_seq = 0;
    for (i = 0; i <= key->last_value; i++)
        key->values[i].mod_seq = 0;
    for (i = 0; i <= key->last_subkey; i++)
        clear_branch_mod_seq( key->subkeys[i] );
}

static void free_tombstones( struct save_branch_info *info )
{
    struct reg_tombstone *ts, *next;
    LIST_FOR_EACH_ENTRY_SAFE( ts, next, &info->tombstones, struct reg_tombstone, entry )
    {
        list_remove( &ts->entry );
        free( ts->key_path );
        free( ts->val_name );
        free( ts );
    }
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
    if ((key->last_value >= 0) || (key->last_subkey == -1) || key->class || (key->flags & KEY_SYMLINK))
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
    key->mod_seq     = 0;
    list_init( &key->notify_list );

    if ((key->classlen = (data->classlen / sizeof(WCHAR)) * sizeof(WCHAR)) &&
        !(key->class = memdup( data->class, key->classlen ))) return false;

    if (data->options & REG_OPTION_CREATE_LINK) key->flags |= KEY_SYMLINK;
    if (data->options & REG_OPTION_VOLATILE) key->flags |= KEY_VOLATILE;
    else if (parent && parent->flags & KEY_VOLATILE)
    {
        set_error( STATUS_CHILD_MUST_BE_VOLATILE );
        return false;
    }
    else if (!registry_loaded || in_merge)
    {
        key->flags |= KEY_DIRTY;
    }
    else
    {
        if (is_per_session_key( key ))
        {
            mark_branch_ephemeral( key );
        }
        else
        {
            key->mod_seq = ++next_mod_seq;
            make_dirty( key );
        }
    }

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

/* is the key in the \Registry\User tree (HKCU, HKU\.Default)? */
static int key_in_user_tree( struct key *key )
{
    static const WCHAR user[] = {'U','s','e','r'};
    const WCHAR *name;
    data_size_t len;
    struct key *parent;

    while ((parent = get_parent( key )) && parent != root_key) key = parent;
    if (!parent || !(name = get_object_name( &key->obj, &len ))) return FALSE;
    return len == sizeof(user) && !memicmp_strW( name, user, sizeof(user) );
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
    /* written by services.exe and wineboot --init, which run limited until the system role exists;
     * shortcut: remove these two when they get the system role (step 2b work order, item 5) */
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
    free( full );
done:
    free( path );
    return ret;
}

/* Default descriptors for keys that have none. A limited token has Administrators deny-only, so the
 * descriptors have to name the user (HKCU) and Users (everywhere else) instead of relying on Administrators:
 * the user and SYSTEM get full control of the user tree; elsewhere Users can read the key itself and
 * Administrators and SYSTEM have full control. */
static struct security_descriptor *key_get_sd( struct object *obj )
{
    static struct security_descriptor *key_default_sd, *key_user_sd, *key_display_sd;

    if (obj->sd) return obj->sd;

    if (key_in_user_tree( (struct key *)obj ))
    {
        if (!key_user_sd)
        {
            const struct default_ace aces[] =
            {
                { &local_user_sid,    CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &local_system_sid,  CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
                { &builtin_admins_sid, CONTAINER_INHERIT_ACE, KEY_ALL_ACCESS },
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
        const struct default_ace aces[] =
        {
            { &builtin_users_sid,  0, KEY_READ },
            { &local_system_sid,   0, KEY_ALL_ACCESS },
            { &builtin_admins_sid, 0, KEY_ALL_ACCESS },
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
        if (is_per_session_key( key ))
        {
            mark_branch_ephemeral( key );
            return;
        }
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
    if (in_merge) return;
    key->modif = current_time;
    if (mark_dirty && !(key->flags & KEY_VOLATILE) && !is_per_session_key( key ))
        make_dirty( key );
    else if (!(key->flags & KEY_VOLATILE) && is_per_session_key( key ))
        mark_branch_ephemeral( key );

    /* do notifications */
    check_notify( key, change, 1 );
    for (key = get_parent( key ); key; key = get_parent( key )) check_notify( key, change, 0 );
}

static void touch_key( struct key *key, unsigned int change )
{
    touch_key_ex( key, change, true );
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
    struct object_params params = { .ops = &key_ops, .name = name, .attr = OBJ_OPENIF | (in_merge ? OBJ_OPENLINK : 0), .init_data = &data };
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
    bool ephemeral;

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

    ephemeral = is_per_session_key( key );
    if (registry_loaded && !in_merge && !(key->flags & KEY_VOLATILE))
    {
        if (!ephemeral)
            record_tombstone( key, NULL, 0, false );
        else
            mark_branch_ephemeral( key );
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

    if (registry_loaded && !in_merge && !(key->flags & KEY_VOLATILE) && !ephemeral)
        mark_subtree_mod_seq( key );

    if (debug_level > 1) dump_operation( key, NULL, "Rename" );
    touch_key_ex( key, REG_NOTIFY_CHANGE_NAME, !(key->flags & KEY_VOLATILE) && !ephemeral );
}

/* delete a key and its values */
static int delete_key( struct key *key, int recurse )
{
    struct key *parent;
    bool ephemeral;

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

    ephemeral = is_per_session_key( key );
    if (registry_loaded && !in_merge && !(key->flags & KEY_VOLATILE))
    {
        if (!ephemeral)
        {
            record_tombstone( key, NULL, 0, false );
            parent->mod_seq = ++next_mod_seq;
        }
        else
        {
            mark_branch_ephemeral( key );
        }
    }

    if (debug_level > 1) dump_operation( key, NULL, "Delete" );
    key->flags |= KEY_DELETED;
    unlink_named_object( &key->obj );
    touch_key_ex( parent, REG_NOTIFY_CHANGE_NAME, !(key->flags & KEY_VOLATILE) && !ephemeral );
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
    value->mod_seq = (registry_loaded && !in_merge && !is_per_session_key( key )) ? ++next_mod_seq : 0;
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
    if (registry_loaded && !in_merge)
    {
        if (!(key->flags & KEY_VOLATILE) && !is_per_session_key( key ) && !is_per_session_value( key, name ))
        {
            value->mod_seq = ++next_mod_seq;
            key->mod_seq = ++next_mod_seq;
        }
        else
        {
            value->mod_seq = 0;
            mark_branch_ephemeral( key );
        }
    }
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
    if (registry_loaded && !in_merge && !(key->flags & KEY_VOLATILE))
    {
        if (!is_per_session_key( key ) && !is_per_session_value( key, name ))
        {
            record_tombstone( key, value->name, value->namelen, true );
            key->mod_seq = ++next_mod_seq;
        }
        else
        {
            mark_branch_ephemeral( key );
        }
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

    if (key && key->flags & KEY_DELETED)
    {
        set_error( STATUS_KEY_DELETED );
        release_object( key );
        key = NULL;
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
    if (subkey)
    {
        update_key_time( subkey, modif );
        release_object( subkey );
    }
    free( info.buffer );
    free( info.tmp );
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

/* load one of the initial registry files */
static int load_init_registry_from_file( const char *filename, struct key *key )
{
    struct stat st;
    FILE *f;
    struct save_branch_info *info;

    assert( save_branch_count < MAX_SAVE_BRANCH_INFO );
    info = &save_branch_info[save_branch_count];
    memset( info, 0, sizeof(*info) );
    list_init( &info->tombstones );

    if ((f = fopen( filename, "r" )))
    {
        if (!fstat( fileno( f ), &st ))
        {
            info->dev      = st.st_dev;
            info->ino      = st.st_ino;
            info->mtime    = st.st_mtim;
            info->has_stat = true;
        }
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
    info->is_shared_machine = is_shared_machine_file( filename );
    save_branch_count++;
    make_object_permanent( &key->obj );
    return (f != NULL);
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

    if (!load_init_registry_from_file( "system.reg", hklm ))
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

    load_init_registry_from_file( "userdef.reg", key );
    release_object( key );

    /* load user.reg into HKEY_CURRENT_USER */

    /* FIXME: match default user in token.c. should get from process token instead */
    current_user_path = format_user_registry_path( &local_user_sid, &current_user_str );
    if (!current_user_path ||
        !(hkcu = create_key_recursive( root_key, current_user_str, current_time )))
        fatal_error( "could not create HKEY_CURRENT_USER registry key\n" );
    free( current_user_path );
    load_init_registry_from_file( "user.reg", hkcu );

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
        if (save_branch_info[i].has_stat)
            make_clean( save_branch_info[i].key );
    }
    registry_loaded = true;

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
}

static void replay_dirty_keys( const struct key *src, const struct key *src_base, struct key *dst_base )
{
    int i;

    if (src->flags & KEY_VOLATILE) return;
    if (is_per_session_key( src )) return;

    if (src != src_base)
    {
        bool has_dirty_val = false;
        for (i = 0; i <= src->last_value; i++)
        {
            if (src->values[i].mod_seq > 0)
            {
                has_dirty_val = true;
                break;
            }
        }

        if (src->mod_seq > 0 || has_dirty_val)
        {
            data_size_t rel_len;
            WCHAR *rel_path = build_relative_key_path( src, src_base, &rel_len );
            if (rel_path)
            {
                struct unicode_str path_str;
                struct key *dst;
                path_str.str = rel_path;
                path_str.len = rel_len;
                dst = create_key_recursive( dst_base, path_str, src->modif );
                free( rel_path );
                if (dst)
                {
                    if (src->mod_seq > 0)
                    {
                        dst->modif = src->modif;
                        if (src->classlen)
                        {
                            free( dst->class );
                            dst->class = memdup( src->class, src->classlen );
                            dst->classlen = dst->class ? src->classlen : 0;
                        }
                        if (src->flags & KEY_SYMLINK) dst->flags |= KEY_SYMLINK;
                    }
                    for (i = 0; i <= src->last_value; i++)
                    {
                        if (src->values[i].mod_seq > 0)
                        {
                            struct unicode_str vname;
                            vname.str = src->values[i].name;
                            vname.len = src->values[i].namelen;
                            set_value( dst, vname, src->values[i].type, src->values[i].data, src->values[i].len );
                        }
                    }
                    release_object( dst );
                }
            }
        }
    }
    else
    {
        for (i = 0; i <= src->last_value; i++)
        {
            if (src->values[i].mod_seq > 0)
            {
                struct unicode_str vname;
                vname.str = src->values[i].name;
                vname.len = src->values[i].namelen;
                set_value( dst_base, vname, src->values[i].type, src->values[i].data, src->values[i].len );
            }
        }
    }

    for (i = 0; i <= src->last_subkey; i++)
        replay_dirty_keys( src->subkeys[i], src_base, dst_base );
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

/* save a registry branch to a file */
static int save_branch( struct save_branch_info *info )
{
    struct key *key = info->key;
    const char *filename = info->filename;
    struct stat st, lst;
    char target_path[PATH_MAX];
    char target_dir[PATH_MAX];
    char lock_path[PATH_MAX];
    char tmp[PATH_MAX];
    int fd, lock_fd = -1, count = 0, ret = 0;
    FILE *f;
    bool is_symlink = false;
    bool file_exists = false;
    bool disk_changed = false;
    struct key *save_root = key;
    struct key *tmp_root = NULL;
    const char *slash;

    if (!(key->flags & KEY_DIRTY))
    {
        if (debug_level > 1) dump_operation( key, NULL, "Not saving clean" );
        return 1;
    }
    tmp[0] = 0;

    /* Check if accessed via symbolic link */
    if (!lstat( filename, &lst ) && S_ISLNK( lst.st_mode ))
    {
        is_symlink = true;
        if (!realpath( filename, target_path ))
        {
            ssize_t len = readlink( filename, target_path, sizeof(target_path) - 1 );
            if (len < 0) return 0;
            target_path[len] = 0;
        }
    }
    else
    {
        snprintf( target_path, sizeof(target_path), "%s", filename );
    }

    /* Extract target directory */
    slash = strrchr( target_path, '/' );
    if (slash)
    {
        size_t dirlen = slash - target_path;
        if (dirlen == 0) dirlen = 1;
        if (dirlen >= sizeof(target_dir)) return 0;
        memcpy( target_dir, target_path, dirlen );
        target_dir[dirlen] = 0;
    }
    else
    {
        strcpy( target_dir, "." );
    }

    /* Check write permission: if target or directory is not writable (EACCES/EROFS),
     * clear dirty state and return 1 without modifying file or replacing link (Requirements 2 & 3). */
    if (access( target_path, F_OK ) == 0)
    {
        if (access( target_path, W_OK ) == -1)
        {
            if (errno == EACCES || errno == EROFS)
            {
                make_clean( key );
                clear_branch_mod_seq( key );
                free_tombstones( info );
                info->has_ephemeral_mods = false;
                return 1;
            }
        }
    }
    if (access( target_dir, W_OK ) == -1)
    {
        if (errno == EACCES || errno == EROFS)
        {
            make_clean( key );
            clear_branch_mod_seq( key );
            free_tombstones( info );
            info->has_ephemeral_mods = false;
            return 1;
        }
    }

    /* Acquire flock(LOCK_EX) on lock file in target directory */
    snprintf( lock_path, sizeof(lock_path), "%s/.reg.lock", target_dir );
    lock_fd = open( lock_path, O_CREAT | O_RDWR, 0664 );
    if (lock_fd == -1 && (errno == EACCES || errno == EROFS))
    {
        lock_fd = open( lock_path, O_RDONLY );
    }
    if (lock_fd == -1)
    {
        if (errno == EACCES || errno == EROFS)
        {
            make_clean( key );
            clear_branch_mod_seq( key );
            free_tombstones( info );
            info->has_ephemeral_mods = false;
            return 1;
        }
        return 0;
    }
    fchmod( lock_fd, 0664 );
    flock( lock_fd, LOCK_EX );

    /* Check file stat on disk */
    if (stat( target_path, &st ) == 0)
    {
        file_exists = true;
        if (info->has_stat)
        {
            if (st.st_dev != info->dev || st.st_ino != info->ino ||
                st.st_mtim.tv_sec != info->mtime.tv_sec ||
                st.st_mtim.tv_nsec != info->mtime.tv_nsec)
            {
                disk_changed = true;
            }
        }
        else
        {
            disk_changed = true;
        }
    }

    /* For shared hives (symlinks into machine root or when ephemeral mods exist or external change occurred),
     * load current on-disk hive into tmp_root, replay session tombstones, and replay session dirty tree. */
    if (file_exists && info->is_shared_machine && (disk_changed || info->has_ephemeral_mods || is_symlink))
    {
        FILE *f_in = fopen( target_path, "r" );
        if (f_in)
        {
            static const WCHAR merge_tmp_name[] = {'_','_','m','e','r','g','e','_','t','m','p','_','_'};
            struct unicode_str tmp_str;
            tmp_str.str = merge_tmp_name;
            tmp_str.len = sizeof(merge_tmp_name);
            in_merge = true;
            tmp_root = create_key_recursive( root_key, tmp_str, current_time );
            if (tmp_root)
            {
                load_keys( tmp_root, target_path, f_in, 0 );
                clear_error();
                replay_tombstones( info, tmp_root );
                replay_dirty_keys( key, key, tmp_root );
                clear_error();
                save_root = tmp_root;
            }
            in_merge = false;
            fclose( f_in );
        }
    }

    /* Create temporary file in target directory */
    for (;;)
    {
        snprintf( tmp, sizeof(tmp), "%s/reg%lx%04x.tmp", target_dir, (long)getpid(), count++ );
        if ((fd = open( tmp, O_CREAT | O_EXCL | O_WRONLY, 0664 )) != -1) break;
        if (errno == EACCES || errno == EROFS)
        {
            if (tmp_root)
            {
                in_merge = true;
                delete_key( tmp_root, 1 );
                release_object( tmp_root );
                in_merge = false;
                clear_error();
            }
            flock( lock_fd, LOCK_UN );
            close( lock_fd );
            make_clean( key );
            clear_branch_mod_seq( key );
            free_tombstones( info );
            info->has_ephemeral_mods = false;
            return 1;
        }
        if (errno != EEXIST) goto done;
        close( fd );
    }

    fchmod( fd, is_symlink ? 0664 : (file_exists ? (st.st_mode & 0777) : 0664) );

    if (!(f = fdopen( fd, "w" )))
    {
        if (tmp[0]) unlink( tmp );
        close( fd );
        goto done;
    }

    if (debug_level > 1)
    {
        fprintf( stderr, "%s: ", target_path );
        dump_operation( key, NULL, "saving" );
    }

    save_all_subkeys_from( key, save_root, f );
    ret = (fflush( f ) == 0 && fsync( fileno( f ) ) == 0);
    if (fclose( f ) != 0) ret = 0;

    if (tmp[0])
    {
        if (ret)
        {
            chmod( tmp, is_symlink ? 0664 : (file_exists ? (st.st_mode & 0777) : 0664) );
            ret = !rename( tmp, target_path );
            if (ret && stat( target_path, &st ) == 0)
            {
                info->dev      = st.st_dev;
                info->ino      = st.st_ino;
                info->mtime    = st.st_mtim;
                info->has_stat = true;
            }
        }
        if (!ret) unlink( tmp );
    }

done:
    if (tmp_root)
    {
        in_merge = true;
        delete_key( tmp_root, 1 );
        release_object( tmp_root );
        in_merge = false;
        clear_error();
    }

    if (lock_fd != -1)
    {
        flock( lock_fd, LOCK_UN );
        close( lock_fd );
    }

    if (ret)
    {
        make_clean( key );
        clear_branch_mod_seq( key );
        free_tombstones( info );
        info->has_ephemeral_mods = false;
    }
    return ret;
}

/* periodic saving of the registry */
static void periodic_save( void *arg )
{
    int i;

    if (fchdir( config_dir_fd ) == -1) return;
    save_timeout_user = NULL;
    for (i = 0; i < save_branch_count; i++)
        save_branch( &save_branch_info[i] );
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));
    set_periodic_save_timer();
}

/* start the periodic save timer */
static void set_periodic_save_timer(void)
{
    if (save_timeout_user) remove_timeout_user( save_timeout_user );
    save_timeout_user = add_timeout_user( save_period, periodic_save, NULL );
}

/* save the modified registry branches to disk */
void flush_registry(void)
{
    int i;

    if (fchdir( config_dir_fd ) == -1) return;
    for (i = 0; i < save_branch_count; i++)
    {
        if (!save_branch( &save_branch_info[i] ))
        {
            fprintf( stderr, "wineserver: could not save registry branch to %s",
                     save_branch_info[i].filename );
            perror( " " );
        }
    }
    if (fchdir( server_dir_fd ) == -1) fatal_error( "chdir to server dir: %s\n", strerror( errno ));
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
        ret = key_in_display_cache( (struct key *)parent, &left );
        if (ret) clear_error();
    }
    release_object( parent );
    return ret;
}

/* create a registry key */
DECL_HANDLER(create_key)
{
    struct key *key, *parent = NULL;
    unsigned int access = req->access;
    struct key_init_data data = { .options = req->options, .modif = current_time };
    struct object_params params = { .ops = &key_ops, .init_data = &data };

    if (!get_req_object_attributes( &params )) return;
    if (params.root) release_object( params.root );
    data.class = get_req_data_after_objattr( &params, &data.classlen );

    if (!is_wow64_process( current->process )) access = (access & ~KEY_WOW64_32KEY) | KEY_WOW64_64KEY;
    if (!(access & KEY_WOW64_64KEY)) params.attr |= OBJ_KEY_WOW64;
    if (req->options & REG_OPTION_CREATE_LINK) params.attr = (params.attr & ~OBJ_OPENIF) | OBJ_OPENLINK;

    if (params.objattr->rootdir)
    {
        if (!(parent = get_hkey_obj( params.objattr->rootdir, 0 ))) return;
        params.root = get_wow6432_parent( parent, access, params.name );
    }

    if (elevation_enforced() && !check_create_subkey( &params )) goto done;

    if ((key = params.name.len ? create_named_object( &params ) : open_named_object( &params )))
    {
        if (!params.name.len || get_error() == STATUS_OBJECT_NAME_EXISTS)
        {
            if (key->flags & KEY_PREDEF) set_error( STATUS_PREDEFINED_HANDLE );
            if (!(access & KEY_WOW64_64KEY)) key = grab_wow6432node( key );
            reply->hkey = alloc_handle( current->process, key, access, params.attr );
        }
        else
        {
            reply->hkey = alloc_handle_no_access_check( current->process, key,
                                                        access, params.attr );
            if (parent)
                touch_key_ex( get_parent( key ), REG_NOTIFY_CHANGE_NAME,
                              !(key->flags & KEY_VOLATILE) && !is_per_session_key( key ) );
            if (debug_level > 1) dump_operation( key, NULL, "Create" );
        }
        release_object( key );
    }

done:
    if (parent) release_object( parent );
}

/* open a registry key */
DECL_HANDLER(open_key)
{
    struct key *key, *parent = NULL;
    unsigned int access = req->access;
    struct object_params params = { .ops = &key_ops, .name = get_req_unicode_str(),
                                    .attr = req->attributes };

    if (!is_wow64_process( current->process )) access = (access & ~KEY_WOW64_32KEY) | KEY_WOW64_64KEY;
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
        reply->hkey = alloc_handle( current->process, key, access, req->attributes );
        if (debug_level > 1) dump_operation( key, NULL, "Open" );
        release_object( key );
    }
    if (parent) release_object( parent );
}

/* delete a registry key */
DECL_HANDLER(delete_key)
{
    struct key *key = (struct key *)get_handle_obj( current->process, req->hkey, DELETE, &key_ops );

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
        /* we don't need to do anything here with the current implementation */
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
        load_registry( key, req->file );
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
        if (key->obj.handle_count)
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

    key = get_hkey_obj( req->hkey, KEY_WRITE );
    if (key)
    {
        rename_key( key, name );
        release_object( key );
    }
}

int object_is_registry_key( struct object *obj )
{
    return obj->ops == &key_ops;
}
