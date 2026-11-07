/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 * S13 descriptor application. No normal-build activation or generic xattr trust.
 */
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "file.h"
#include "thread.h"
#include "security.h"
#include "e1s_profile.h"
#include "e1s_token.h"

#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <sys/syscall.h>
#include <linux/stat.h>

struct secured_inode
{
    struct secured_inode *next;
    int fd;
    struct stat st;
    struct security_descriptor *sd;
    size_t size;
};
struct e1s_file_security { struct secured_inode *first; unsigned int count; };
static struct secured_inode *secured;
static unsigned int secured_count;
static const char sd_xattr[] = "user.WEX_E1S13_SD";
static struct
{
    struct security_descriptor sd;
    struct acl acl;
} denied_sd = { { .control = SE_DACL_PRESENT | SE_DACL_PROTECTED, .dacl_len = sizeof(struct acl) },
                { .revision = ACL_REVISION, .size = sizeof(struct acl) } };

static int private_metadata( int fd )
{
    const char *names[] = { "user.WINEREPARSE", "system.posix_acl_access", "system.posix_acl_default" };
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(names); i++)
        if (fgetxattr( fd, names[i], NULL, 0 ) >= 0 ||
            (errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP)) return 0;
    return 1;
}

static int same_mount( int left, int right )
{
    struct statx a, b;
    return !syscall( SYS_statx, left, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &a ) &&
           !syscall( SYS_statx, right, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &b ) &&
           (a.stx_mask & STATX_MNT_ID) && (b.stx_mask & STATX_MNT_ID) && a.stx_mnt_id == b.stx_mnt_id;
}

/* Explicit Windows owner/group, protected DACL and High mandatory label.
 * Registry matches existing per-user trustee set; files follow S12 intent
 * (Alice and SYSTEM only). Unix owner/modes are never changed. */
struct security_descriptor *e1s_profile_alloc_sd( const struct sid *user, int file )
{
    struct sid high = { SID_REVISION, 1, SECURITY_MANDATORY_LABEL_AUTHORITY, { SECURITY_MANDATORY_HIGH_RID } };
    const struct sid *trustees[] = { user, &local_system_sid, &builtin_admins_sid };
    struct security_descriptor *sd;
    struct acl *acl;
    struct ace *ace;
    unsigned int i, count = file ? 2 : 3, flags = CONTAINER_INHERIT_ACE | (file ? OBJECT_INHERIT_ACE : 0);
    size_t ulen = sid_len( user ), sacl = sizeof(*acl) + sizeof(*ace) + sid_len( &high );
    size_t dacl = sizeof(*acl), size;
    for (i = 0; i < count; i++) dacl += sizeof(*ace) + sid_len( trustees[i] );
    size = sizeof(*sd) + 2 * ulen + sacl + dacl;
    if (!(sd = mem_alloc( size ))) return NULL;
    memset( sd, 0, size );
    sd->control = SE_DACL_PRESENT | SE_DACL_PROTECTED | SE_SACL_PRESENT | SE_SACL_PROTECTED;
    sd->owner_len = sd->group_len = ulen;
    sd->sacl_len = sacl; sd->dacl_len = dacl;
    copy_sid( copy_sid( (struct sid *)(sd + 1), user ), user );
    acl = (struct acl *)((char *)(sd + 1) + 2 * ulen);
    acl->revision = ACL_REVISION; acl->size = sacl; acl->count = 1;
    set_ace( ace_first( acl ), &high, SYSTEM_MANDATORY_LABEL_ACE_TYPE, flags, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP );
    acl = (struct acl *)((char *)acl + sacl);
    acl->revision = ACL_REVISION; acl->size = dacl; acl->count = count;
    for (i = 0, ace = ace_first( acl ); i < count; i++, ace = ace_next( ace ))
        set_ace( ace, trustees[i], ACCESS_ALLOWED_ACE_TYPE, flags, file ? FILE_ALL_ACCESS : KEY_ALL_ACCESS );
    return sd;
}

static int secure_tree( struct e1s_file_security *batch, int fd, const struct sid *user, unsigned int depth )
{
    struct secured_inode *entry;
    struct stat st, named, opened;
    void *readback;
    DIR *dir;
    struct dirent *child;
    int scan, child_fd, ret = 0;
    if (depth > 32 || ++batch->count > 16384 || secured_count + batch->count > 131072 || fstat( fd, &st ) ||
        (!S_ISDIR( st.st_mode ) && !S_ISREG( st.st_mode )) || st.st_uid != getuid() ||
        st.st_gid != getgid() || (st.st_mode & 07777) != (S_ISDIR( st.st_mode ) ? 0700 : 0600) ||
        (S_ISREG( st.st_mode ) && st.st_nlink != 1) || !private_metadata( fd )) return 0;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return 0;
    entry->fd = fcntl( fd, F_DUPFD_CLOEXEC, 3 );
    entry->sd = e1s_profile_alloc_sd( user, 1 );
    if (entry->fd < 0 || !entry->sd)
    { if (entry->fd >= 0) close( entry->fd ); free( entry->sd ); free( entry ); return 0; }
    entry->st = st;
    entry->size = sizeof(*entry->sd) + entry->sd->owner_len + entry->sd->group_len + entry->sd->sacl_len + entry->sd->dacl_len;
    entry->next = batch->first; batch->first = entry;
    if (!(readback = malloc( entry->size ))) return 0;
    if (!fsetxattr( fd, sd_xattr, entry->sd, entry->size, 0 ) && !fsync( fd ) &&
        fgetxattr( fd, sd_xattr, readback, entry->size ) == (ssize_t)entry->size &&
        !memcmp( readback, entry->sd, entry->size )) ret = 1;
    free( readback );
    if (!ret || !S_ISDIR( st.st_mode )) return ret;
    scan = openat( fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    if (scan < 0) return 0;
    if (!(dir = fdopendir( scan ))) { close( scan ); return 0; }
    for (;;)
    {
        errno = 0;
        if (!(child = readdir( dir ))) { if (errno) ret = 0; break; }
        if (!strcmp( child->d_name, "." ) || !strcmp( child->d_name, ".." )) continue;
        if (fstatat( fd, child->d_name, &named, AT_SYMLINK_NOFOLLOW ) || named.st_dev != st.st_dev ||
            (!S_ISDIR( named.st_mode ) && !S_ISREG( named.st_mode ))) { ret = 0; break; }
        child_fd = openat( fd, child->d_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
                           (S_ISDIR( named.st_mode ) ? O_DIRECTORY : 0) );
        if (child_fd < 0) { ret = 0; break; }
        ret = !fstat( child_fd, &opened ) && opened.st_ino == named.st_ino && opened.st_dev == named.st_dev && same_mount( fd, child_fd ) &&
              secure_tree( batch, child_fd, user, depth + 1 ) &&
              !fstatat( fd, child->d_name, &named, AT_SYMLINK_NOFOLLOW ) &&
              named.st_dev == opened.st_dev && named.st_ino == opened.st_ino;
        close( child_fd );
        if (!ret) break;
    }
    closedir( dir );
    return ret;
}

struct e1s_file_security *apply_e1s_profile_security( const struct wex_account *a,
                                                   const struct wex_profile_storage *storage )
{
    struct e1s_file_security *batch = calloc( 1, sizeof(*batch) );
    struct sid user = { SID_REVISION, 5, SECURITY_NT_AUTHORITY, {0} };
    if (!batch) return NULL;
    memcpy( user.sub_auth, a->sid, sizeof(a->sid) );
    if (!secure_tree( batch, storage->profile_fd, &user, 0 ))
    { e1s_profile_security_abort( batch ); return NULL; }
    return batch;
}

void e1s_profile_security_abort( struct e1s_file_security *batch )
{
    struct secured_inode *e;
    if (!batch) return;
    while ((e = batch->first))
    { batch->first = e->next; close( e->fd ); free( e->sd ); free( e ); }
    free( batch );
}

void e1s_profile_security_commit( struct e1s_file_security *batch )
{
    struct secured_inode *e;
    while ((e = batch->first)) { batch->first = e->next; e->next = secured; secured = e; secured_count++; }
    free( batch );
}

int e1s_profile_security_verify( const struct e1s_file_security *batch )
{
    struct secured_inode *e;
    struct stat st;
    char data[1024];
    for (e = batch->first; e; e = e->next)
        if (fstat( e->fd, &st ) || st.st_dev != e->st.st_dev || st.st_ino != e->st.st_ino ||
            st.st_uid != e->st.st_uid || st.st_gid != e->st.st_gid || st.st_mode != e->st.st_mode ||
            (S_ISREG(st.st_mode) && st.st_nlink != 1) || !private_metadata( e->fd ) ||
            e->size > sizeof(data) || fgetxattr( e->fd, sd_xattr, data, sizeof(data) ) != (ssize_t)e->size ||
            memcmp( data, e->sd, e->size )) return 0;
    return 1;
}

/* Trust only descriptors registered by this server after S4/S10/S12 checks.
 * A copied/forged xattr alone cannot assign Windows identity. Pins prevent
 * inode reuse. Any observed ownership/mode/xattr change returns a denying SD. */
struct security_descriptor *e1s_profile_file_sd( int fd, int *managed )
{
    struct secured_inode *e;
    struct stat st;
    char readback[1024];
    *managed = 0;
    if (fstat( fd, &st )) return NULL;
    for (e = secured; e; e = e->next)
        if (e->st.st_dev == st.st_dev && e->st.st_ino == st.st_ino)
        {
            *managed = 1;
            if (st.st_uid != e->st.st_uid || st.st_gid != e->st.st_gid || st.st_mode != e->st.st_mode ||
                (S_ISREG(st.st_mode) && st.st_nlink != 1) || e->size > sizeof(readback) ||
                fgetxattr( fd, sd_xattr, readback, sizeof(readback) ) != (ssize_t)e->size || !private_metadata( fd ) ||
                memcmp( readback, e->sd, e->size )) return &denied_sd.sd;
            return e->sd;
        }
    return NULL;
}

/* Normal paths do no extra I/O until this lab server has a registered tree.
 * Resolve only to identify an already pinned parent; its inode/SD is authority.
 * Never adopt a native-inserted unregistered directory below a managed root. */
int e1s_profile_check_parent( const char *name, unsigned int access, int create, mode_t *mode,
                              struct e1s_profile_parent *parent )
{
    char *copy, *slash;
    int fd, walk, up, managed, i;
    struct stat before, after;
    parent->fd = -1; parent->leaf = NULL;
    if (!secured) return 1;
    if (!(copy = strdup( name ))) { set_error( STATUS_NO_MEMORY ); return 0; }
    for (i = strlen(copy); i > 1 && copy[i - 1] == '/';) copy[--i] = 0;
    slash = strrchr( copy, '/' );
    parent->leaf = strdup( slash ? slash + 1 : copy );
    if (slash) *slash = 0;
    fd = open( slash ? (*copy ? copy : "/") : ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
    free( copy );
    if (!parent->leaf) { if (fd >= 0) close( fd ); set_error( STATUS_NO_MEMORY ); return 0; }
    if (fd < 0) { e1s_profile_close_parent( parent ); return 1; } /* ordinary open reports its error */
    e1s_profile_file_sd( fd, &managed );
    if (managed)
    {
        parent->fd = fd;
        if (!e1s_profile_check_fd( fd, access | FILE_TRAVERSE )) goto denied;
        if (create) *mode = (*mode & S_IFDIR) ? 0700 : 0600;
        return 1;
    }
    walk = fd;
    for (i = 0; i < 128; i++)
    {
        if (fstat( walk, &before )) break;
        up = openat( walk, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC );
        if (up < 0) break;
        if (fstat( up, &after )) { close( up ); break; }
        e1s_profile_file_sd( up, &managed );
        close( walk ); walk = up;
        if (managed || (before.st_dev == after.st_dev && before.st_ino == after.st_ino)) break;
    }
    close( walk );
    if (managed) goto denied;
    e1s_profile_close_parent( parent );
    return 1;
denied:
    e1s_profile_close_parent( parent );
    set_error( STATUS_ACCESS_DENIED ); return 0;
}

int e1s_profile_inherit_fd( const struct e1s_profile_parent *parent, int fd )
{
    struct stat named, opened;
    struct e1s_file_security *batch;
    struct security_descriptor *sd;
    int managed;
    if (parent->fd < 0) return 1;
    if (fstatat( parent->fd, parent->leaf, &named, AT_SYMLINK_NOFOLLOW ) || fstat( fd, &opened ) ||
        named.st_dev != opened.st_dev || named.st_ino != opened.st_ino ||
        (named.st_mode & S_IFMT) != (opened.st_mode & S_IFMT) || !same_mount( fd, parent->fd )) goto denied;
    e1s_profile_file_sd( fd, &managed );
    if (managed) return 1; /* check_fd below rechecks the stored descriptor */
    sd = e1s_profile_file_sd( parent->fd, &managed );
    if (!managed || sd == &denied_sd.sd || !(batch = calloc( 1, sizeof(*batch) ))) goto denied;
    if (!secure_tree( batch, fd, sd_get_owner( sd ), 0 )) { e1s_profile_security_abort( batch ); goto denied; }
    e1s_profile_security_commit( batch );
    return 1;
denied:
    set_error( STATUS_ACCESS_DENIED ); return 0;
}
#else
struct security_descriptor *e1s_profile_alloc_sd( const struct sid *s, int file ) { return NULL; }
struct e1s_file_security *apply_e1s_profile_security( const struct wex_account *a, const struct wex_profile_storage *s ) { return NULL; }
void e1s_profile_security_abort( struct e1s_file_security *s ) {}
void e1s_profile_security_commit( struct e1s_file_security *s ) {}
int e1s_profile_security_verify( const struct e1s_file_security *s ) { return 0; }
struct security_descriptor *e1s_profile_file_sd( int fd, int *managed ) { *managed = 0; return NULL; }
int e1s_profile_check_parent( const char *name, unsigned int access, int create, mode_t *mode,
                              struct e1s_profile_parent *p ) { p->fd = -1; p->leaf = NULL; return 1; }
int e1s_profile_inherit_fd( const struct e1s_profile_parent *p, int fd ) { return 1; }
#endif

void e1s_profile_close_parent( struct e1s_profile_parent *p )
{
    if (p->fd >= 0) close( p->fd );
    free( p->leaf ); p->fd = -1; p->leaf = NULL;
}

static struct security_descriptor *e1s_check_sd( struct object *obj ) { return obj->sd; }

int e1s_profile_check_fd( int fd, unsigned int access )
{
    static const struct object_ops ops = { .type = &file_type, .get_sd = e1s_check_sd,
                                           .map_access = default_map_access };
    struct object obj = { .ops = &ops };
    int managed;
    obj.sd = e1s_profile_file_sd( fd, &managed );
    if (!managed) return 1;
    if ((access & MAXIMUM_ALLOWED) && !token_e1s_profile_high( current->token ? current->token : current->process->token ))
    { set_error( STATUS_ACCESS_DENIED ); return 0; }
#if defined(WEX_E1S13_HKU_TEST) && defined(__linux__)
    if (obj.sd == &denied_sd.sd) { set_error( STATUS_ACCESS_DENIED ); return 0; }
#endif
    return check_object_access( NULL, &obj, &access );
}

void e1s_profile_unload_s14(void)
{
    /* TODO E1-S14: leases, descendant/key references, checked durable saving
     * and recovery. No deletion, unlock or success until that contract exists. */
    set_error( STATUS_NOT_SUPPORTED );
}
