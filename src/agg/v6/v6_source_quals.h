/*
 * agg/v6/v6_source_quals.h - the WHERE quals of an operator that reads the batches of a source (SiftV6Source): the clauses the machine of agg/vexpr.h compiles go to the source, and the
 * clauses over text columns, which the machine does not run, narrow each batch the source gives back (the batch quals of agg/v6/v6_bqual.h). The operator opens the source with the machine
 * program and puts the source over the text quals.
 */
#ifndef SIFT_AGG_V6_SOURCE_QUALS_H
#define SIFT_AGG_V6_SOURCE_QUALS_H

#include "agg/v6/v6_vexpr.h"

typedef struct SiftV6SplitQuals
{
	SiftV6Vexpr machine;		/* the clauses the machine runs, over the columns of the input */
	bool		has_machine;
	List	   *text;			/* the clauses over text columns, as Vars of varno SIFT_V6_BQ_RELID */
} SiftV6SplitQuals;

/* Compile the machine's clauses against in (which gains the columns they read) and keep the text ones; false for a clause that is neither (root: the planner's, to check a text clause the batch quals run; NULL when the node replays what the planner checked) */
extern bool sift_v6_split_quals(MemoryContext cx, PlannerInfo *root, const List *quals, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6SplitQuals *out);

/* The text columns the clauses read join the input, so that the source decodes them for the batches */
extern void sift_v6_split_quals_columns(SiftV6Input *in, const SiftV6SplitQuals *sq);

/* The source over inner (closed with it): the batches of inner, narrowed to the rows the text clauses leave; in is the input inner was opened with, text columns included */
extern SiftV6Source *sift_v6_source_over_quals(MemoryContext cx, SiftV6Source *inner, const SiftV6Input *in, const SiftV6SplitQuals *sq);

#endif							/* SIFT_AGG_V6_SOURCE_QUALS_H */
