/*
 * col/dict/dict_int.h - what the files of the string dictionaries share
 * the statics one file of the dictionary calls from another
 */
#ifndef SIFT_COL_DICT_INT_H
#define SIFT_COL_DICT_INT_H

#include "col/enc_int.h"

/*
 * col/dict/dict.c - L1 of the column engine: the dictionaries of strings (code = first-seen order).
 */

/* ---------------------------------------------------------------------------
 * 2. Dictionaries
 * ------------------------------------------------------------------------- */

#define DICT_MINCAP		16

extern void	cdict_sort(SiftCDict *d);

#endif