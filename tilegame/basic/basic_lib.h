/* SPDX-License-Identifier: MIT */
/*
 * The small libraries of MY-BASIC's wiki, as functions of the BASIC environment: bit operations (BIT_AND, BIT_OR, BIT_XOR, BIT_NOT, BIT_LSHIFT, BIT_RSHIFT), the
 * miscellaneous ones (ASSERT, IIF, SWAP, MIN, MAX) and string matching (FIND, WILDCARD_FIND). They are the wiki's in name and in what they do; where its code
 * would read outside a string or shift by too much, or print to a console the board does not have, this one does not. See basic/README.md for the functions.
 */
#ifndef BASIC_LIB_H
#define BASIC_LIB_H

struct mb_interpreter_t;

/* Registers the functions (basic_env_init does). */
void basic_lib_register(struct mb_interpreter_t *s);

#endif
