/*
 * sift_aggstub.h - the sift_colstub index access method and the column source (sift_colsrc.h) over raw heap pages.
 *
 * SIFT_TESTING builds only: it lets the aggregate planner and executor (sift_agg*.c) be developed and tested before the
 * real column index (sift_col) and the table access method (sift_table) exist. sift_aggstub.c is the access method, the
 * definition (layout) of an index and the `paths` reloption, sift_aggstub_src.c the source, sift_aggstub_test.c the
 * sift._test_aggstub_* SQL functions. Deviations from the contract are listed at the top of sift_aggstub.c.
 */
#ifndef SIFT_AGGSTUB_H
#define SIFT_AGGSTUB_H

#ifdef SIFT_TESTING

#include "sift_host.h"

#include "sift.h"
#include "sift_colsrc.h"

#define SIFT_COLSTUB_AM			"sift_colstub"
#define SIFT_COLSTUB_MAX_STR	255		/* bytes of a text value, as the design notes */

/* How a leaf's values are found in the index datum of its attribute */
typedef enum StubRole
{
	STUB_ROLE_VALUE,			/* the attribute itself, or the jsonb value at the leaf's path */
	STUB_ROLE_FIELD,			/* field `name` of the objects of an array */
	STUB_ROLE_KEY,				/* key of the pairs of an object */
	STUB_ROLE_PAIR				/* value of the pairs of an object */
} StubRole;

typedef struct StubLeafX
{
	StubRole	role;
	int			namelen;
	Oid			ioparam;
	FmgrInfo	infn;			/* input function of typid, for a jsonb value that is not text */
} StubLeafX;

/* The definition of an index: the public layout and what the decoder needs besides */
typedef struct StubLayout
{
	SiftSrcLayout l;
	StubLeafX	x[SIFT_COLSRC_MAX_LEAVES];
} StubLayout;

extern void sift_aggstub_init(void);

/* Is the access method the stub's */
extern bool sift_aggstub_owns_index(Oid indexrelam);

/* Registers the stub as a column source provider (sift_colsrc.h) */
extern void sift_aggstub_register(void);

/* The TIDs of the rows of the unit just returned by sift_colsrc_next (test functions) */
extern const SiftKey *sift_aggstub_keys(const SiftSrcSource *src);

/* Validates the definition of the index (types, `paths`) and builds its layout, palloc'd in CurrentMemoryContext */
extern StubLayout *sift_aggstub_layout(Relation index);

/* Decodes every leaf of one row given as the datums of the index attributes: ERROR for a value the index would refuse */
extern void sift_aggstub_check_row(const StubLayout *lay, const Datum *values, const bool *isnull);

#endif							/* SIFT_TESTING */

#endif							/* SIFT_AGGSTUB_H */
