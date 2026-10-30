/*
 * agg/from/int.h - what the files of agg/from/ share
 * The typedefs of the partition variables and candidates and of the unconditional read, and
 * the functions one part calls in another.
 */
#ifndef SIFT_AGG_FROM_INT_H
#define SIFT_AGG_FROM_INT_H

#include "agg/plan_int.h"

/* a column index of a partition: its layout in the terms of the partitioned table's columns, its statistics and pre-aggregates */
typedef struct PartCand
{
	Oid			indexoid;
	SiftSrcLayout *lay;
	const SiftPartStats *stats;
	const SiftSrcPreagg *pa;
} PartCand;

typedef struct PartVars
{
	PlannerInfo *root;
	Index		rti;			/* the partition */
	Index		top;			/* the partitioned table */
	bool		ok;
} PartVars;

typedef struct Unconditional
{
	AggCtx	   *c;
	int			leaf;			/* the scalar leaf whose read (leaf_read) is looked for; -1: the cast of a member's jsonb value to c->cast_want */
	bool		as_text;		/* leaf_read: `(doc ->> 'k')::T`, else the cast of the jsonb value `(doc -> 'k')::T` */
	int			nread;			/* count_leaf_reads: the reads it found */
} Unconditional;

extern AttrNumber part_attno(PartVars *pv, AttrNumber att);
extern Node * part_vars_mutator(Node *n, PartVars *pv);

extern void stats_scaled(Relation rel, SiftSrcStats *st);
extern void index_facts(Relation rel, int nleaves, SiftSrcStats *st, SiftSrcPreagg *pa);
extern const PartCand * part_facts(Oid relid, bool table);

extern bool part_source(AggCtx *c);

extern bool recordset_recheck_ok(AggCtx *c, int grp);
extern Bitmapset * recordset_groups(AggCtx *c);
extern Bitmapset * recordset_columns(AggCtx *c, int grp);
extern Oid member_recheck(AggCtx *c, const SiftAggSpec *s, int j);

extern bool unconditional(Node *n, Unconditional *u);
extern bool count_leaf_reads(Node *n, Unconditional *u);
extern bool pred_reads_leaf(AggCtx *c, const SiftAggPred *p, int leaf);
extern bool pred_reads_any(const SiftAggPred *p, const Bitmapset *leaves);
extern bool pred_reads_set(AggCtx *c, const SiftAggPred *p, const Bitmapset *leaves);
extern bool list_read_by_tuple_tests(const SiftAggSpec *s, int grp);
extern SiftSrcRecheck order_refused(AggCtx *c, int m);
extern bool others_first(AggCtx *c, const Unconditional *u);
extern bool filtered_reads(Node *n, Unconditional *u);
extern uint64 gate_mask(AggCtx *c, const SiftAggSpec *s, int m);



#endif							/* SIFT_AGG_FROM_INT_H */
