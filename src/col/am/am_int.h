/*
 * col/am/am_int.h - what the files of the column access method share
 * the statics one file calls from another
 */
#ifndef SIFT_COL_AM_INT_H
#define SIFT_COL_AM_INT_H

#include "col/build.h"

/* a string reloption: stored as an offset from the options struct, 0 when unset */
#define COL_OPT_STR(fn, field, unset) \
const char * \
fn(Relation index) \
{ \
	const SiftColOptions *o = sift_col_options(index); \
\
	return o->field ? (const char *) o + o->field : (unset); \
}

extern uint64 am_leaf_plan;

extern const char *	am_return_walk(const char *opt, SiftColRel *r, TupleDesc desc);

extern bytea *	am_col_amoptions(Datum reloptions, bool validate);

#endif