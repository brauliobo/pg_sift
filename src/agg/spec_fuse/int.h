/*
 * agg/spec_fuse/int.h - what the files of agg/spec_fuse/ share
 * The arm and the pass, and the functions the joins, the conjuncts and the build of a pass
 * call in one another.
 */
#ifndef SIFT_AGG_SPEC_FUSE_DIR_INT_H
#define SIFT_AGG_SPEC_FUSE_DIR_INT_H

#include "agg/plan_int.h"

typedef struct FuseArm
{
	const SiftAggSpec *s;
	int		   *join;			/* per join of the arm: its join in the pass */
	int		   *combo;			/* per combo */
	int		  **col;			/* per join: the column of each of its columns in the pass */
	int		  **slot;			/* per combo: the slot of each of its slots in the pass */
	int		   *fslot;			/* per join: the slot of the rest of its ON clause in the pass, -1 none */
	int		   *fcombo;			/* per join: the combo of that slot in the pass */
	SiftAggSpec a;				/* the arm over the joins and combos of the pass */
} FuseArm;

typedef struct Fuse
{
	int			n;
	FuseArm	   *arm;
	SiftAggSpec u;				/* the joins and combos of the pass, the leaves of all the arms */
	int			npost;			/* the conjuncts over the joined rows of the arms, each once, and the arms (bit i) that apply it; 0: all of them */
	SiftAggPred **post;
	uint32	   *postsets;
	int			npreds;			/* predicates of the pass: those of the arms (ids below npreds_total of the arm with most) and its own conjuncts */
	int			ncommon;		/* the conjuncts over the rows that every arm has: the pass applies them */
	SiftAggPred **common;
	SiftAggPred **resid;		/* per arm: its other conjuncts as one node, NULL none: they are applied to its set (spec.setpred) */
	int			nagg;			/* the aggregates of the pass: those of the arms, each once */
	SiftAggAgg *agg;
	uint32	   *aggsets;		/* per aggregate: the arms that read it; NULL when every arm has the same aggregates */
	int		  **aggmap;			/* per arm: the aggregate of the pass that is each of its aggregates */
	List	   *owners;			/* the slots that give the ids of an aggregate's values (SlotOwn): their aggregate is the pass's when it is known */
} Fuse;

typedef struct SlotOwn
{
	int			combo;
	int			slot;
	int			arm;
	int			agg;			/* the aggregate of the arm */
	List	   *key;			/* that aggregate, apart from the slot it reads */
} SlotOwn;
extern bool fuse_join(Fuse *f, FuseArm *arm, int j);
extern SiftAggPred * pred_noid(const SiftAggPred *p);
extern List * agg_pack(const SiftAggAgg *a);
extern bool fuse_combo(Fuse *f, FuseArm *arm, int ci);
extern void fuse_fslot(FuseArm *arm, int j);
extern bool fuse_fslots(Fuse *f);

extern SiftAggPred * pred_number(const SiftAggPred *p, int *next);
extern bool fuse_posts(Fuse *f);
extern bool fuse_aggs(Fuse *f);
extern void fuse_adapt(Fuse *f, FuseArm *arm);
extern void fuse_unpost(Fuse *f);
extern SiftAggPred * junction(SiftAggPredKind kind, int n, SiftAggPred **child);
extern bool fuse_split(Fuse *f);

#endif							/* SIFT_AGG_SPEC_FUSE_DIR_INT_H */
