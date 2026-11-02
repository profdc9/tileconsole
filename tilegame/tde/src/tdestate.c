/*
 * tilegame: creating and destroying the state of the editor, and the allocation tracking (see tdestate.h).
 *
 * This file is not public domain: it is part of tilegame (MIT).
 */
#define TDE_STATE_IMPL
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tdestr.h"
#include "tdefunc.h"

tde_state_t *tde;

/* a block as the editor allocated it: this header, and then what it asked for */
struct tde_alloc {
   tde_alloc *prev, *next;
   size_t     size;
   size_t     pad;                 /* (keeps what follows aligned to 16 bytes) */
};

static void unlink_block( tde_alloc *b )
{
   if (b->prev != NULL)
      b->prev->next = b->next;
   else
      tde->blocks = b->next;
   if (b->next != NULL)
      b->next->prev = b->prev;
   tde->blocks_live--;
}

static void link_block( tde_alloc *b )
{
   b->prev = NULL;
   b->next = tde->blocks;
   if (b->next != NULL)
      b->next->prev = b;
   tde->blocks = b;
   tde->blocks_live++;
}

void *tde_malloc( size_t size )
{
   tde_alloc *b;

   if (tde == NULL)
      return( NULL );
   b = malloc( sizeof(*b) + size );
   if (b == NULL)
      return( NULL );
   b->size = size;
   link_block( b );
   return( b + 1 );
}

void *tde_calloc( size_t n, size_t size )
{
   void *p;

   if (size != 0  &&  n > SIZE_MAX / size)
      return( NULL );
   p = tde_malloc( n * size );
   if (p != NULL)
      memset( p, 0, n * size );
   return( p );
}

void *tde_realloc( void *p, size_t size )
{
   tde_alloc *b, *n;

   if (p == NULL)
      return( tde_malloc( size ) );
   b = (tde_alloc *)p - 1;
   unlink_block( b );
   n = realloc( b, sizeof(*b) + size );
   if (n == NULL) {
      link_block( b );
      return( NULL );
   }
   n->size = size;
   link_block( n );
   return( n + 1 );
}

void tde_free( void *p )
{
   tde_alloc *b;

   if (p == NULL)
      return;
   b = (tde_alloc *)p - 1;
   unlink_block( b );
   free( b );
}

size_t tde_state_live_blocks( void )
{
   return( tde != NULL ? tde->blocks_live : 0 );
}

/* (help.c is data only, and does not see the editor's headers) */
void help_state_init( void )
{
   { static const int tmpl_help_dim[2][2] = { { 25, 80 }, { 25, 80 } }; memcpy( &help_dim, &tmpl_help_dim, sizeof help_dim ); }
}

tde_state_t *tde_state_create( void )
{
   tde_state_t *s = calloc( 1, sizeof(*s) );

   if (s == NULL)
      return( NULL );
   tde = s;
   TDE_STATE_INITS
   return( s );
}

void tde_state_destroy( void )
{
   tde_alloc *b, *next;
   tde_state_t *s = tde;

   if (s == NULL)
      return;
   for (b = s->blocks; b != NULL; b = next) {
      next = b->next;
      free( b );
   }
   tde = NULL;
   free( s );
}
