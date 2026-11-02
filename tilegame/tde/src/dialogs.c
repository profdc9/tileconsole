/*
 * New editor name:  TDE, the Thomson-Davis Editor.
 * Author:           Jason Hood
 * Date:             November 15, 2003
 *
 * This file contains the dialog definitions for various functions.
 *
 * Dialogs in TDE are very simple - they only contains labels, edit fields and
 * checkboxes. The first line of the definition gives the size of the box
 * (including the frame) and the edit field with the initial focus. A label is
 * indicated by an ERROR value in the .n field; a checkbox has FALSE or TRUE;
 * anything else is the width of an edit field. Every edit field and checkbox
 * must be stored consecutively. The last item must have an .x value of 0 to
 * end the list.
 *
 * A callback function can be used to change the state of the dialog and to
 * verify the contents of edit fields. The callback is entered with the id of
 * the edit field or check box, along with the current text of the edit field.
 * A negative id is used for an edit field that has become empty or not. The
 * callback is called with an id of zero upon completion of the dialog. The
 * return value is OK for success or the id of the edit field to re-edit
 * (ERROR to use the current).
 *
 * jmh 050817: Every edit field should have a label, with the labels stored
 *             immediately before the edits. The appropriate label is then
 *             highlighted when an edit field is selected.
 */


#include "tdestr.h"
#include "common.h"


/*
 * DefineSearch
 */

/*
 * ReplaceString
 */

/*
 * BorderBlockEx
 */

/*
 * DefineDiff
 */

/*
 * DefineGrep
 */

/*
 * Execute
 */

/*
 * FileAttributes
 */

/*
 * NumberBlock
 */

/*
 * SetMargins
 */

/*
 * SetTabs
 */


/* tilegame: sets up this file's part of the editor state */
void dialogs_state_init( void )
{

   { DIALOG  tmpl_find_dialog[] = {
   { 45, 10, NULL,        ERROR, NULL },
   {  3,  2, "Pattern:",  ERROR, NULL },
   { 12,  2, NULL,        30,    &h_find },
   {  5,  5, "RegX",      FALSE, NULL },
   {  5,  6, "Backward",  FALSE, NULL },
   {  5,  7, "Beginning", FALSE, NULL },
   { 24,  5, "Block",     FALSE, NULL },
   { 24,  6, "All files", FALSE, NULL },
   { 24,  7, "Results",   FALSE, NULL },
   {  0,  0, NULL,        ERROR, NULL }
}; _Static_assert( sizeof tmpl_find_dialog == sizeof find_dialog, "find_dialog" ); memcpy( &find_dialog, &tmpl_find_dialog, sizeof find_dialog ); }
   { DIALOG  tmpl_replace_dialog[] = {
   { 49, 12, NULL,           ERROR, NULL },
   {  7,  2, "Pattern:",     ERROR, NULL },
   {  3,  4, "Replacement:", ERROR, NULL },
   { 16,  2, NULL,           30,    &h_find },
   { 16,  4, NULL,           30,    &h_find },
   {  7,  7, "RegX",         FALSE, NULL },
   {  7,  8, "Backward",     FALSE, NULL },
   {  7,  9, "Beginning",    FALSE, NULL },
   { 26,  7, "Block",        FALSE, NULL },
   { 26,  8, "All files",    FALSE, NULL },
   { 26,  9, "No prompt",    FALSE, NULL },
   {  0,  0, NULL,           ERROR, NULL }
}; _Static_assert( sizeof tmpl_replace_dialog == sizeof replace_dialog, "replace_dialog" ); memcpy( &replace_dialog, &tmpl_replace_dialog, sizeof replace_dialog ); }
   { DIALOG  tmpl_border_dialog[] = {
   { 50, 11, NULL,           ERROR, NULL },
   {  5,  3, "Top-left",     ERROR, NULL },
   { 23,  1, "Top",          ERROR, NULL },
   { 36,  3, "Top-right",    ERROR, NULL },
   {  9,  5, "Left",         ERROR, NULL },
   { 36,  5, "Right",        ERROR, NULL },
   {  2,  7, "Bottom-left",  ERROR, NULL },
   { 22,  9, "Bottom",       ERROR, NULL },
   { 36,  7, "Bottom-right", ERROR, NULL },
   { 15,  3, NULL,           5,     &h_border },
   { 22,  3, NULL,           5,     &h_border },
   { 29,  3, NULL,           5,     &h_border },
   { 15,  5, NULL,           5,     &h_border },
   { 29,  5, NULL,           5,     &h_border },
   { 15,  7, NULL,           5,     &h_border },
   { 22,  7, NULL,           5,     &h_border },
   { 29,  7, NULL,           5,     &h_border },
   {  0,  0, NULL,           ERROR, &h_border }
}; _Static_assert( sizeof tmpl_border_dialog == sizeof border_dialog, "border_dialog" ); memcpy( &border_dialog, &tmpl_border_dialog, sizeof border_dialog ); }
   { DIALOG  tmpl_diff_dialog[] = {
   { 55,  9, NULL,                   ERROR, NULL },
   {  4,  2, "First window:",        ERROR, NULL },
   {  3,  4, "Second window:",       ERROR, NULL },
   { 18,  2, NULL,                   4,     &h_win },
   { 18,  4, NULL,                   4,     &h_win },
   { 25,  2, "Ignore all space",     FALSE, NULL },
   { 25,  3, "Ignore leading space", FALSE, NULL },
   { 25,  4, "Ignore blank lines",   FALSE, NULL },
   { 25,  5, "Ignore end of line",   FALSE, NULL },
   { 25,  6, "Current position",     FALSE, NULL },
   {  0,  0, NULL,                   ERROR, NULL }
}; _Static_assert( sizeof tmpl_diff_dialog == sizeof diff_dialog, "diff_dialog" ); memcpy( &diff_dialog, &tmpl_diff_dialog, sizeof diff_dialog ); }
   { DIALOG  tmpl_grep_dialog[] = {
   { 45, 11, NULL,       ERROR, NULL },
   {  3,  2, "Pattern:", ERROR, NULL },
   {  5,  4, "Files:",   ERROR, NULL },
   { 12,  2, NULL,       30,    &h_find },
   { 12,  4, NULL,       30,    NULL },
   {  7,  7, "RegX",     FALSE, NULL },
   {  7,  8, "Results",  FALSE, NULL },
   { 24,  7, "Load all", FALSE, NULL },
   { 24,  8, "Binary",   FALSE, NULL },
   {  0,  0, NULL,       ERROR, NULL }
}; _Static_assert( sizeof tmpl_grep_dialog == sizeof grep_dialog, "grep_dialog" ); memcpy( &grep_dialog, &tmpl_grep_dialog, sizeof grep_dialog ); }
   { DIALOG  tmpl_exec_dialog[] = {
   { 51, 10, NULL,             ERROR, NULL },
   {  3,  2, "Command:",       ERROR, NULL },
   { 12,  2, NULL,             36,    &h_exec },
   {  3,  5, "Capture output", FALSE, NULL },
   {  3,  6, "No echo",        FALSE, NULL },
   {  3,  7, "No pause",       FALSE, NULL },
   { 27,  5, "Original files", FALSE, NULL },
   { 27,  6, "Reload files",   FALSE, NULL },
   {  0,  0, NULL,             ERROR, NULL }
}; _Static_assert( sizeof tmpl_exec_dialog == sizeof exec_dialog, "exec_dialog" ); memcpy( &exec_dialog, &tmpl_exec_dialog, sizeof exec_dialog ); }
   { DIALOG  tmpl_fattr_dialog[] = {
#if defined( __UNIX__ )
   { 61, 11, NULL,         ERROR, NULL },
   { 10,  2, "User",       ERROR, NULL },
   { 29,  2, "Group",      ERROR, NULL },
   { 48,  2, "Other",      ERROR, NULL },
   {  3,  3, "Readable",   FALSE, NULL },
   {  3,  4, "Writable",   FALSE, NULL },
   {  3,  5, "eXecutable", FALSE, NULL },
   { 22,  3, "Readable",   FALSE, NULL },
   { 22,  4, "Writable",   FALSE, NULL },
   { 22,  5, "eXecutable", FALSE, NULL },
   { 41,  3, "Readable",   FALSE, NULL },
   { 41,  4, "Writable",   FALSE, NULL },
   { 41,  5, "eXecutable", FALSE, NULL },
   { 20,  7, "or enter mode string:", ERROR, NULL },
   { 29,  8, NULL,         4,     NULL },
#else
   { 32, 11, NULL,         ERROR, NULL },
   {  8,  2, "Archive",    FALSE, NULL },
   {  8,  3, "System",     FALSE, NULL },
   {  8,  4, "Hidden",     FALSE, NULL },
   {  8,  5, "Read only",  FALSE, NULL },
   {  3,  7, "or enter attribute string:", ERROR, NULL },
   { 13,  8, NULL,         5,     NULL },
#endif
   {  0,  0, NULL,         ERROR, NULL }
}; _Static_assert( sizeof tmpl_fattr_dialog == sizeof fattr_dialog, "fattr_dialog" ); memcpy( &fattr_dialog, &tmpl_fattr_dialog, sizeof fattr_dialog ); }
   { static const DIALOG tmpl_number_dialog[] = {
   { 31, 13, NULL,               ERROR, NULL },
   {  3,  2, "Starting number:", ERROR, NULL },
   {  9,  4, "Increment:",       ERROR, NULL },
   { 14,  6, "Base:",            ERROR, NULL },
   { 20,  2, NULL,               8,     NULL },
   { 20,  4, NULL,               8,     NULL },
   { 20,  6, NULL,               3,     NULL },
   {  7,  9, "Left align",       FALSE, NULL },
   {  7, 10, "Zero fill",        FALSE, NULL },
   {  0,  0, NULL,               ERROR, NULL }
}; _Static_assert( sizeof tmpl_number_dialog == sizeof number_dialog, "number_dialog" ); memcpy( &number_dialog, &tmpl_number_dialog, sizeof number_dialog ); }
   { static const DIALOG tmpl_margins_dialog[] = {
   { 21, 15, NULL,         ERROR, NULL },
   {  7,  2, "Margins",    ERROR, NULL },
   {  8,  5, "Left:",      ERROR, NULL },
   {  7,  7, "Right:",     ERROR, NULL },
   {  3,  9, "Paragraph:", ERROR, NULL },
   { 14,  5, NULL,         4,     NULL },
   { 14,  7, NULL,         4,     NULL },
   { 14,  9, NULL,         4,     NULL },
   {  4, 12, "Justify",    FALSE, NULL },
   {  0,  0, NULL,         ERROR, NULL }
}; _Static_assert( sizeof tmpl_margins_dialog == sizeof margins_dialog, "margins_dialog" ); memcpy( &margins_dialog, &tmpl_margins_dialog, sizeof margins_dialog ); }
   { static const DIALOG tmpl_tabs_dialog[] = {
   { 19, 10, NULL,         ERROR, NULL },
   {  6,  2, "Tab Size",   ERROR, NULL },
   {  4,  5, "Logical:",   ERROR, NULL },
   {  3,  7, "Physical:",  ERROR, NULL },
   { 13,  5, NULL,         3,     NULL },
   { 13,  7, NULL,         3,     NULL },
   {  0,  0, NULL,         ERROR, NULL }
}; _Static_assert( sizeof tmpl_tabs_dialog == sizeof tabs_dialog, "tabs_dialog" ); memcpy( &tabs_dialog, &tmpl_tabs_dialog, sizeof tabs_dialog ); }
}