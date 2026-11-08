/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2026 WineExplorer contributors
 */
#ifndef WEX_E1S_PROFILE_H
#define WEX_E1S_PROFILE_H
#include "wex_profile_storage.h"
struct process;
struct token;
struct e1s_file_security;
extern struct e1s_file_security *apply_e1s_profile_security( const struct wex_account *,
                                                           const struct wex_profile_storage * );
extern void e1s_profile_security_commit( struct e1s_file_security * );
extern int e1s_profile_security_verify( const struct e1s_file_security * );
extern void e1s_profile_security_abort( struct e1s_file_security * );
extern struct security_descriptor *e1s_profile_file_sd( int fd, int *managed );
extern int e1s_profile_check_fd( int fd, unsigned int access );
struct e1s_profile_parent { int fd; char *leaf; };
extern int e1s_profile_check_parent( const char *, unsigned int, int, mode_t *, struct e1s_profile_parent * );
extern int e1s_profile_inherit_fd( const struct e1s_profile_parent *, int );
extern void e1s_profile_close_parent( struct e1s_profile_parent * );
extern struct security_descriptor *e1s_profile_alloc_sd( const struct sid *, int file );
extern int e1s_profile_secure_hive( int, const struct wex_account * );
extern int e1s_profile_process_started( struct token * );
#endif
