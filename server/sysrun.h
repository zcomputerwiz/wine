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

struct process;

/* Set only for a server that the setgid wex-sysrun launcher started (saved gid = the group of the root-owned
 * /etc/wineexplorer/sysrun.conf) and whose configuration names a machine root. Otherwise every function below
 * does nothing and the server behaves as stock Wine. */
extern int sysrun_active;

extern void sysrun_init(void);
extern const char *sysrun_machine_root(void);
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
