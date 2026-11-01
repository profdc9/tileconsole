/* SPDX-License-Identifier: MIT */
/*
 * Editor name:      tde, the Thomson-Davis Editor (public domain).
 * Filename:         console.c for the tilegame port
 *
 * The console of the editor on a tilegfx text screen and plat_getkey: the same functions as the Unix, DOS and Windows consoles of the editor
 * (tdefunc.h), written for a screen that is an array of 80 x 25 cells (character in the low byte, DOS attribute in the high byte) and a keyboard
 * that reports which key was pressed with which modifiers.
 */
#include "tdestr.h"
#include "common.h"
#include "define.h"
#include "tdefunc.h"
#include "tde_os.h"

#include "keymap.h"
#include "platform.h"

#define COLS 80
#define ROWS 25

int  stdoutput;                 /* the redirected stdout (the Unix editor's; not used here) */
int  xterm;                     /* running in an xterm? Never: the keyboard reports every key with its modifiers */

static tg_text_t *scr;
static void (*present)(void);
static void (*idle)(void);
static int last_caps, last_num = 1;
static int flush_enabled = 1;

void tde_console_attach(tg_text_t *screen, void (*present_fn)(void))
{
   scr = screen;
   present = present_fn;
}

void tde_console_set_idle(void (*idle_fn)(void))
{
   idle = idle_fn;
}

void refresh( void )
{
   if (present != NULL)
      present( );
}

static uint16_t *cell( int row, int col )
{
   static uint16_t scrap;                       /* somewhere to put what is off the screen or has no screen */
   if (scr == NULL || row < 0 || row >= ROWS || col < 0 || col >= COLS)
      return( &scrap );
   return( &scr->cells[row * COLS + col] );
}

/*
 * Name:    console_init
 * Purpose: initialise the console (video and keyboard)
 * Passed:  cfg:  pointer to hold video config data
 */
void console_init( struct vcfg *cfg )
{
   video_config( cfg );
}

void console_suspend( void )
{
}

void console_resume( int pause )
{
   if (pause)
      getkey( );
   refresh( );
}

void console_exit( void )
{
   if (scr != NULL)
      tg_text_show_cursor( scr, false );
   refresh( );
}

void video_config( struct vcfg *cfg )
{
   cfg->color = TRUE;
   cfg->rows  = ROWS;
   cfg->cols  = COLS;
   g_display.adapter = VGA;
   g_display.shadow_width = 2;
   /* scan lines of the 16 high cells: an underline, the lower half and the whole cell (the DOS values are for 14 lines) */
   g_display.cursor[SMALL_CURSOR]  = 0x0e0f;
   g_display.cursor[MEDIUM_CURSOR] = 0x080f;
   g_display.cursor[LARGE_CURSOR]  = 0x000f;
   g_display.curses_cursor = 0x0e0f;
}

/* ---- the keyboard ---- */

/*
 * The editor's keys are the PC scan codes from 256 up (define.h), with the shift, ctrl and alt bits. A HID usage code gives the scan code of the key
 * that is in the same place on a PC keyboard (usage 0 means: no key).
 */
static const short usage_key[0x66] = {
   [0x04] = _A, _B, _C, _D, _E, _F, _G, _H, _I, _J, _K, _L, _M, _N, _O, _P, _Q, _R, _S, _T, _U, _V, _W, _X, _Y, _Z,
   [0x1E] = _1, _2, _3, _4, _5, _6, _7, _8, _9, _0,
   [0x28] = _ENTER, _ESC, _BACKSPACE, _TAB, _SPACEBAR, _MINUS, _EQUALS, _LBRACKET, _RBRACKET, _BACKSLASH,
   [0x32] = _BACKSLASH, _SEMICOLON, _APOSTROPHE, _BACKQUOTE, _COMMA, _PERIOD, _SLASH,
   [0x39] = _CAPSLOCK, _F1, _F2, _F3, _F4, _F5, _F6, _F7, _F8, _F9, _F10, _F11, _F12, _PRTSC,
   [0x49] = _INS, _HOME, _PGUP, _DEL, _END, _PGDN, _RIGHT, _LEFT, _DOWN, _UP,
   [0x54] = _GREY_SLASH, _GREY_STAR, _GREY_MINUS, _GREY_PLUS, _GREY_ENTER,
   [0x59] = _END, _DOWN, _PGDN, _LEFT, _CENTER, _RIGHT, _HOME, _UP, _PGUP, _INS, _DEL,
   [0x64] = _LEFT_BACKSLASH,
};

/*
 * The editor's key for a key press: its key number (0 for a key it has no use for), the shift, ctrl and alt bits (and _FUNCTION for the keys of the cursor
 * block), and the character the key types without Ctrl and Alt (0 if it types none), for translate_key.
 */
int  tde_event_key( const plat_key_event_t *ev, int *shift_state, int *ch )
{
int  key;

   key = (ev->usage < sizeof usage_key / sizeof *usage_key) ? usage_key[ev->usage] : 0;
   if (key == 0)
      return( 0 );

   *shift_state = 0;
   if (ev->modifiers & PLAT_MOD_SHIFT)
      *shift_state |= _SHIFT;
   if (ev->modifiers & PLAT_MOD_CTRL)
      *shift_state |= _CTRL;
   if (ev->modifiers & PLAT_MOD_ALT)
      *shift_state |= _ALT;

   /*
    * The character the key types without Ctrl and Alt (and with Shift and the locks): printable ones only; the editor makes keys of the rest.
    * The keypad numbers type with num lock on, and are the cursor keys without; the keys of the cursor block are extended keys.
    */
   *ch = keymap_translate( ev->usage, ev->modifiers & PLAT_MOD_SHIFT, ev->caps_lock, ev->num_lock );
   if (*ch < ' ' || *ch > '~')
      *ch = 0;
   if (key >= _HOME && key <= _DEL && !(ev->usage >= 0x54 && ev->usage <= 0x63))
      *shift_state |= _FUNCTION;
   return( key );
}

long getkey( void )
{
plat_key_event_t ev;
int  key, ch, shift_state;
long new_key;

   for (;;) {
      g_status.control_break = FALSE;
      while (!plat_getkey( &ev )) {
         if (plat_quit_requested( ))
            exit( 0 );
         if (idle != NULL)
            idle( );
         else
            refresh( );
      }
      last_caps = ev.caps_lock;
      last_num  = ev.num_lock;
      key = tde_event_key( &ev, &shift_state, &ch );
      if (key == 0)
         continue;
      new_key = translate_key( key, shift_state, (text_t)ch );
      if (new_key != ERROR)
         return( new_key );
   }
}

int  waitkey( int enh_keyboard )
{
   (void)enh_keyboard;
   return( 0 );
}

int  capslock_active( void )
{
   return( last_caps );
}

int  numlock_active( void )
{
   return( last_num );
}

void tde_console_set_flush( int enabled )
{
   flush_enabled = enabled;
}

void flush_keyboard( void )
{
plat_key_event_t ev;

   if (!flush_enabled)
      return;
   while (plat_getkey( &ev ))
      ;
}

void page( unsigned char payg )
{
   (void)payg;
}

/* ---- the screen ---- */

void xygoto( int col, int line )
{
   if (scr == NULL)
      return;
   if (col < 0 || line < 0) {
      tg_text_show_cursor( scr, false );
   } else {
      tg_text_set_cursor( scr, col, line );
      set_cursor_size( mode.insert ? g_display.insert_cursor : g_display.overw_cursor );
   }
}

void display_line( text_ptr text, unsigned char *attr, int len, int line, int col )
{
uint16_t *p = cell( line, col );

   if (col + len > COLS)
      len = COLS - col;
   while (len-- > 0)
      *p++ = (uint16_t)(*text++ | (*attr++ << 8));
}

void c_output( int c, int col, int line, int attr )
{
   *cell( line, col ) = (uint16_t)((c & 255) | (attr << 8));
}

/* A negative count repeats down the column. */
void c_repeat( int c, int cnt, int col, int line, int attr )
{
uint16_t v = (uint16_t)((c & 255) | (attr << 8));

   if (cnt < 0) {
      while (cnt++ < 0)
         *cell( line++, col ) = v;
   } else {
      while (cnt-- > 0)
         *cell( line, col++ ) = v;
   }
}

void s_output( const char *s, int line, int col, int attr )
{
int  max = g_display.ncols;

   if (g_display.output_space && col != 0)
      c_output( ' ', col - 1, line, attr );

   while (*s  &&  col < max)
      c_output( (text_t)*s++, col++, line, attr );

   if (g_display.output_space && col != max)
      c_output( ' ', col, line, attr );
}

void hlight_line( int x, int y, int lgth, int attr )
{
   if (x + lgth > g_display.ncols)
      lgth = g_display.ncols - x;
   while (lgth-- > 0) {
      uint16_t *p = cell( y, x++ );
      *p = (uint16_t)((*p & 255) | (attr << 8));
   }
}

void cls( void )
{
   for (int i = 0; scr != NULL && i < ROWS * COLS; i++)
      scr->cells[i] = (uint16_t)(' ' | (Color( Text ) << 8));
}

/*
 * csize: the scan lines of the cursor as the PC's BIOS has them, the top in the high byte and the bottom in the low one.
 */
void set_cursor_size( int csize )
{
   if (scr == NULL)
      return;
   g_display.curses_cursor = csize;
   tg_text_set_cursor_shape( scr, (csize >> 8) & 15, csize & 15 );
   tg_text_show_cursor( scr, true );
}

void set_overscan_color( int color )
{
   (void)color;       /* the picture round the text is black */
}

void save_screen_line( int col, int line, Char *screen_buffer )
{
   for (int i = col; i < COLS; i++)
      *screen_buffer++ = *cell( line, i );
}

void restore_screen_line( int col, int line, Char *screen_buffer )
{
   for (int i = col; i < COLS; i++)
      *cell( line, i ) = *screen_buffer++;
   refresh( );
}

void save_area( Char *buffer, int wid, int len, int row, int col )
{
   adjust_area( &wid, &len, &row, &col, NULL );
   for (int r = 0; r < len; r++)
      for (int c = 0; c < wid; c++)
         *buffer++ = *cell( row + r, col + c );
}

void restore_area( Char *buffer, int wid, int len, int row, int col )
{
   adjust_area( &wid, &len, &row, &col, NULL );
   for (int r = 0; r < len; r++)
      for (int c = 0; c < wid; c++)
         *cell( row + r, col + c ) = *buffer++;
   refresh( );
}

/* The shadow of a dialog: the characters under it in dark grey on black. */
void shadow_area( int wid, int len, int row, int col )
{
int  w;
int  i;
int  j;
int  alen;

   if (g_display.shadow) {
      alen = len;
      adjust_area( &wid, &len, &row, &col, &w );

      if (w > 0) {
         for (i = len - 1; i > 0; i--)
            for (j = 0; j < w; j++)
               hlight_line( col + wid - w + j, row + i, 1, 8 );
      }
      if (alen < len) {
         col += g_display.shadow_width;
         wid -= g_display.shadow_width + w;
         row += len - 1;
         hlight_line( col, row, wid, 8 );
      }
      refresh( );
   }
}
