/*
 * tilegame: where the editor's blocks come from (see tdestate.h).  This header has no macros, so a program that
 * links the editor can include it beside its own use of malloc.
 */
#ifndef TDEALLOC_H
#define TDEALLOC_H

#include <stddef.h>

/* Not set: the C library's malloc.  Set it before tde_state_create and leave it until tde_state_destroy (a block is freed by
 * the allocator that made it).  The state itself always comes from the C library. */
typedef struct {
   void *(*get)( size_t size );
   void *(*resize)( void *p, size_t size );
   void  (*put)( void *p );
   long  (*avail)( void );              /* the bytes left, for the status line (or NULL) */
} tde_allocator_t;

void tde_state_set_allocator( const tde_allocator_t *a );   /* NULL: back to the C library's */
long tde_state_avail( void );                               /* the allocator's avail( ), or -1 when none is set */

#endif
