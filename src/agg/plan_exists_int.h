/*
 * agg/plan_exists_int.h - shared between the files of the existential predicates (plan_exists.c, plan_path.c, plan_contains.c).
 */
#ifndef SIFT_AGG_PLAN_EXISTS_INT_H
#define SIFT_AGG_PLAN_EXISTS_INT_H

#include "utils/jsonb.h"
#include "utils/numeric.h"

#include "agg/plan_int.h"
#include "col/shred_pg.h"

typedef struct Lit Lit;
typedef struct LitSet
{
	const Lit  *lit;
	int			n;
	bool		outside;
} LitSet;

typedef SiftAggPred *(*ElemBuild) (AggCtx *c, int grp, void *arg);

/* a value the element leaf is compared with: a JSON string, number or boolean */
struct Lit
{
	char		kind;			/* 's', 'n', 'b', 'z' (JSON null) */
	const char *s;
	int			len;
	Numeric		num;
	bool		b;
};

/* agg/plan_exists.c */
extern SiftAggPred *any_elem_outside(AggCtx *c, int grp, const Lit *lit, int n);
extern SiftAggPred *any_elem_in(AggCtx *c, int grp, const Lit *lit, int n);
extern SiftAggPred *any_elem(AggCtx *c, int grp, ElemBuild build, void *arg);
extern SiftAggPred *any_elem_seq(AggCtx *c, int grp, ElemBuild build, const Lit *lit, int n);
extern SiftAggPred *lit_test(AggCtx *c, int grp, const char *name, int op, const Lit *lit, bool neg);
extern SiftAggPred *leaf_lit_cmp(AggCtx *c, int leaf, int op, const Lit *lit);
extern bool lit_is_plain_text(const Lit *lit);

/* agg/plan_path.c */
extern bool jsonb_lit(const JsonbValue *v, Lit *lit);
extern SiftAggPred *compile_path_exists(AggCtx *c, Node *n);

/* agg/plan_contains.c */
extern SiftAggPred *compile_contains(AggCtx *c, Node *target, Node *rhs);
extern SiftAggPred *compile_array_test(AggCtx *c, Node *n, Node **target);
extern SiftAggPred *compile_has_keys(AggCtx *c, Oid fid, Node *target, Node *keys_arg);

#endif
