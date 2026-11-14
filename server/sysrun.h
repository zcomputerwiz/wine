/*
 * Server side of the wexsys system role: effective group handling
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

#ifndef __WINE_SERVER_SYSRUN_H
#define __WINE_SERVER_SYSRUN_H

#include <sys/types.h>
#include <sys/socket.h>

/* Linux pidfd socket options (6.5) and control message, for C libraries older than the kernel */
#ifndef SO_PASSPIDFD
#define SO_PASSPIDFD 76
#endif
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif
#ifndef SCM_PIDFD
#define SCM_PIDFD 0x04
#endif

struct process;

/* Set only for a server that the setgid wex-sysrun launcher started (saved gid = the group of the root-owned
 * /etc/wineexplorer/sysrun.conf) and whose configuration names a machine root. Otherwise every function below
 * does nothing and the server behaves as stock Wine. */
extern int sysrun_active;

extern void sysrun_init(void);
extern const char *sysrun_machine_root(void);

/* One "key=value" line of /etc/wineexplorer/sysrun.conf, the only reader of that file in the server. The file
 * must be a regular file owned by root that only root can write (checked on the open descriptor). */
enum sysrun_conf
{
    SYSRUN_CONF_ABSENT,   /* no such file, an unsafe file, or no line for the key */
    SYSRUN_CONF_VALUE,    /* exactly one line with a value that fits */
    SYSRUN_CONF_INVALID   /* the key repeated, an empty value, or one longer than size - 1 */
};
extern enum sysrun_conf sysrun_conf_get( const char *key, char *value, size_t size );
extern gid_t sysrun_group(void);

/* raise the effective group to the system group; returns 1 if it was raised, to be passed to sysrun_drop() */
extern int sysrun_raise(void);
extern void sysrun_drop( int raised );

/* does this client run with the system group (SO_PEERCRED at connect time)? */
extern int sysrun_process_privileged( const struct process *process );
extern int sysrun_raise_for_current(void);

/* The system role (C3). Only while sysrun is active and elevation is enforced; otherwise these do nothing. */
struct fd;
struct thread;

/* a new process: protect it with the descriptor of its token (system and service accounts), and decide
 * whether the connection with the system group is a root process that waits for its image check */
extern int sysrun_process_created( struct process *process, int is_root );
/* a new thread of a protected process gets the descriptor of its process */
extern int sysrun_thread_created( struct thread *thread );
/* the main image of a process was mapped: verify it, and give a verified system group root process the SYSTEM token */
extern void sysrun_main_image( struct process *process, struct fd *fd );
/* does the process have the system role (a SYSTEM token and the descriptor of a system process)? */
extern int sysrun_process_is_system( struct process *process );
/* is the process the elevation broker: the system role, verified from the mapped machine-root
 * system32/wexelevate.exe (production image name only), with its SYSTEM primary token? */
extern int sysrun_process_is_broker( struct process *process );
/* S6 lab sideband, compiled out unless WEX_E1S6_TRANSPORT_TEST is defined. */
extern int sysrun_e1s6_endorse( const void *data, unsigned int size );
#ifdef WEX_E1S6_TRANSPORT_TEST
extern int sysrun_get_s6_generations( struct process *process, unsigned long long *server_gen, unsigned long long *broker_gen );
extern int sysrun_get_root_generation( unsigned long long *root_gen );
extern int sysrun_get_policy_generation( unsigned long long *policy_gen );
extern int registry_get_prefix_id( unsigned char prefix_id[16] );
#endif
/* are the desktop rules of the secure desktop (reserved names, cross-desktop access) in force? */
extern int sysrun_desktops_enforced(void);
/* access check of a request that names another process or thread by id instead of by handle */
extern int sysrun_check_process_access( struct process *target, unsigned int access );

/* file operations on a Unix path, run with the system group if the requesting client has it */
extern int sysrun_mkdir( const char *path, mode_t mode );
extern int sysrun_open( const char *path, int flags, mode_t mode );
extern int sysrun_rename( const char *old, const char *new );
extern int sysrun_link( const char *old, const char *new );
extern int sysrun_unlink( const char *path );

#endif  /* __WINE_SERVER_SYSRUN_H */
