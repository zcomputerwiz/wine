/*
 * WineExplorer E1 lab hooks of the server (never in a shipped build)
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
 * The E1 lab code (docs/E1_S6_TRANSPORT.md, the R8 and R9 elevation chains, the S10 token and S13/S14 profile
 * tests) lives in the wex_e1lab_*.c files, compiled only with the lab switches (scripts/lab/r8-lab-build.sh). The
 * stock files call it through the hooks declared here (R5 of docs/SESSION0_SPLIT_REVIEW.md); each hook is listed
 * in docs/STOCK_CHANGES.md. Without the switches this header declares nothing but the audit macro.
 */

#ifndef __WINE_SERVER_WEX_E1LAB_H
#define __WINE_SERVER_WEX_E1LAB_H

/* the lab's audit line for a refused check; nothing in a normal build */
#if defined(WEX_E1S6_TRANSPORT_TEST) && defined(WEX_E1_TEST_HOOKS)
#include <stdio.h>
#define E1S6_BINDING_AUDIT(site) fprintf( stderr, "E1S6_DENY pipe site=%s\n", site )
#else
#define E1S6_BINDING_AUDIT(site) do {} while (0)
#endif

#ifdef WEX_E1S6_TRANSPORT_TEST

struct process;
struct token;
struct desktop;

/* named_pipe.c: the R8 binding of a server end (wex_e1lab_pipe.c) */
struct pipe_binding;
extern void e1lab_pipe_binding_free( struct pipe_binding *binding );
/* get_pipe_caller_binding with an E1R8 request: the broker's ops 1 to 4 on the binding of its server end.
 * Returns 0 with the error set when refused. */
extern int e1lab_pipe_e1r8_op( struct pipe_binding **binding, struct process *opener, struct token *opener_primary,
                               struct process *writer_process, struct desktop *writer_desktop,
                               const void *data, data_size_t size );
/* get_pipe_caller_binding in session 0: R9's broker B0 endorses its S6 channel; sets the error itself */
extern void e1lab_pipe_session0_endorse( const void *data, data_size_t size );

/* sysrun.c (wex_e1lab_sysrun.c): the first executable image of a process, for the lab's broker images; the body of
 * sysrun_e1s6_endorse (1 when the endorsement was sent) */
struct fd;
extern void e1lab_sysrun_main_image( struct process *process, struct fd *fd, int system_role );
#ifdef __linux__
extern int e1lab_sysrun_e1s6_endorse( const void *data, unsigned int size );
#endif

#endif /* WEX_E1S6_TRANSPORT_TEST */

#ifdef WEX_X0_LAB_TEST
/* X0 (docs/SESSION0_CROSS_SERVER.md, its appendix): the lab's control socket and object kind (wex_x0lab.c), set up
 * at the end of session0_init in session 0 and in each linked user's server; session0.c: the lab drops a user's
 * server's session link */
extern void x0lab_init(void);
extern void session0_lab_drop_link(void);
extern void session0_lab_hold_relink( int held );
#endif /* WEX_X0_LAB_TEST */

#endif /* __WINE_SERVER_WEX_E1LAB_H */
