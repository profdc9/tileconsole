/*
 * tilegame: the state of the editor.
 *
 * The variables TDE keeps at file scope are members of one structure and are reached through the
 * pointer 'tde'.  Those shared by several files are members of tde_state_t; the ones private to a file
 * are in a structure of their own (in that file), found through tde->mod[].  Each name is a macro for the
 * member, so the editor's code reads as it always did.
 *
 *   tde_state_create( )   makes a new state (all variables at their first values) and selects it
 *   tde_state_destroy( )  frees every block the editor still holds, and the state, and selects none
 *
 * Memory: TDE's malloc, calloc, realloc and free are the tracking versions below, so that destroying the
 * state returns everything the editor allocated, even what its own code never freed.
 */
#ifndef TDESTATE_H
#define TDESTATE_H

#include <setjmp.h>
#include <stddef.h>

#include "tdestate_gen.h"

typedef struct tde_alloc tde_alloc;

typedef struct tde_state {
   TDE_SHARED_MEMBERS
   void      *mod[TDE_MOD_COUNT];      /* the files' own parts */
   tde_alloc *blocks;                  /* every block allocated and not yet freed */
   size_t     blocks_live;
} tde_state_t;

extern tde_state_t *tde;

#include "tdestate_macros.h"

tde_state_t *tde_state_create( void );
void         tde_state_destroy( void );
size_t       tde_state_live_blocks( void );  /* blocks still allocated (for tests) */

#include "tdealloc.h"

void *tde_malloc( size_t );
void *tde_calloc( size_t, size_t );
void *tde_realloc( void *, size_t );
void  tde_free( void * );


#ifndef TDE_STATE_IMPL
#include <stdlib.h>
#include <string.h>
#define malloc(n)      tde_malloc( n )
#define calloc(n, m)   tde_calloc( n, m )
#define realloc(p, n)  tde_realloc( p, n )
#define free(p)        tde_free( p )
#endif

#endif
