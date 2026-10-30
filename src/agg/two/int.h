/*
 * agg/two/int.h - what the files of agg/two/ share
 * The typedefs of the arms and of the two level shape, and the functions the parts call
 * in one another.
 */
#ifndef SIFT_AGG_TWO_DIR_INT_H
#define SIFT_AGG_TWO_DIR_INT_H

#include "agg/plan_int.h"

typedef struct CteSub
{
	Index		rti;			/* 0: the arm reads the table itself */
	List	   *tlist;			/* the target list of the common table expression */
} CteSub;

/* what an arm (a subquery of the outer query) is, in the terms of the table it reads */
typedef struct ArmIn
{
	CteSub		sub;
	List	   *cols;			/* the expressions of its output columns */
	List	   *group;			/* its group expressions */
	List	   *gsgc;			/* their SortGroupClauses */
	List	   *used;			/* the group expressions that are an outer key */
	List	   *kexpr;			/* the group expressions that are a key of the scan (outer or hidden) ... */
	List	   *kidx;			/* ... and their keys */
	List	   *quals;			/* its WHERE, when it reads a common table expression */
	Node	   *id;				/* the group expression that is unique per row */
	SiftAggArm *out;
} ArmIn;

typedef struct HavingSub
{
	AggCtx	   *c;
	ArmIn	   *in;
	List	   *src;			/* inner aggregate of each Param made */
	bool		bad;
} HavingSub;

typedef struct SubShape
{
	PlannerGlobal *glob;
	List	   *params;			/* the output Params of the sub-plan being walked */
	List	   *subs;			/* the sub-select of each sub-plan met */
	bool		bad;
} SubShape;

/* What planning one two level aggregate builds up: the arms, the spec and the scan's target list */
typedef struct TwoLevel
{
	PlannerInfo *root;
	PlannerInfo *base;			/* the root of the scan the arms share */
	List	   *common;			/* the conditions of the base's WHERE that every arm has: the scan's */
	List	   *drop;			/* the others (of the base's: its pointers in the FromScan): arm filters, with those of the other arms */
	Index		srti;			/* the outer query's FROM item: the arms */
	JoinExpr   *join;			/* the arms are joined to a spine; NULL: they are the FROM item */
	Index		spine_rti;
	List	   *spine_cols;		/* the Vars of the spine that the join and the keys read: the columns of its plan */
	SiftAggSpine *spine;
	int			pair_att[SIFT_AGG_MAX_JKEYS];	/* the column of the arms each pair of the join reads */
	List	   *roots;			/* the planned roots of the arms */
	List	   *gates;
	ArmIn		arms[SIFT_AGG_MAX_ARMS];
	int			narms;
	int			ncols;			/* the columns every arm returns */
	AggCtx		ctx;
	SiftAggSpec *s;
	OutWalk		ow;
	List	   *scan_tlist;
	Relids		outer;			/* children the arms join by an inner join where another arm keeps them in an outer one (see from_same) */
} TwoLevel;

extern Node * cte_sub_mutator(Node *n, CteSub *s);
extern List * outer_arms(PlannerInfo *root, Index srti, List **gates);
extern PlannerInfo * arm_base(PlannerInfo *arm, ArmIn *in);
extern bool same_scan(PlannerInfo *a, PlannerInfo *b, Relids *outer);
extern Node * strip_ph(Node *n);
extern bool null_const(Node *e);
extern bool cond_split(Node *q, Node **operand, SiftAggCond *out);
extern bool outer_cond(Node *q, Index srti, SiftAggCond *out);

extern bool arm_gates(AggCtx *c, SiftAggSpec *s, List *gates);
extern int arm_key_of(const ArmIn *arm, Node *x);
extern bool outer_agg(AggCtx *c, SiftAggSpec *s, Aggref *oa, Index srti, ArmIn *arms, int narms, SiftAggAgg *out);
extern bool hidden_keys(AggCtx *c, SiftAggSpec *s, ArmIn *arms, int narms, List **scan_tlist, bool sets);
extern PlannerInfo * two_arms(TwoLevel *t);
extern bool two_arm_terms(TwoLevel *t, int k);

extern bool two_keys(TwoLevel *t);
extern bool two_aggs(TwoLevel *t, RelOptInfo *output_rel, GroupPathExtraData *gextra);
extern bool two_from(TwoLevel *t);
extern bool two_spine(TwoLevel *t);
extern RelOptInfo * arm_input(PlannerInfo *base);

#endif							/* SIFT_AGG_TWO_DIR_INT_H */
