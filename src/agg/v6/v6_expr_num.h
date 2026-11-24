/*
 * agg/v6/v6_expr_num.h - the exact numeric arguments of the scan of SiftAggV6 (SVD_NUM_EXPR of SiftV6Derive): the product of up to three affine factors of numeric columns,
 *   a * (1 - b) * (1 + c)      l_extendedprice * (1 - l_discount) * (1 + l_tax)
 * where a factor is a column, or a column plus or minus a constant, and every column is a scaled integer (value * 10^scale, up to 18 digits) of the index. The product of scales is the sum of the
 * scales, as numeric_mul gives it, and a sum of scales aligns to the larger one, as numeric_add does: the result is the integer PostgreSQL's numeric holds, at the scale of its display.
 *
 * A batch is computed in int64 with the overflow of every operation checked, and again in 128 bits only when one of its rows leaves int64 (the vector is then SVEC_NUM128); a value past 128 bits is an
 * error the statement can avoid with sift.enable_agg = off. A factor of one derivation that is the first factors of another (the discounted price of the charge) is computed once: the longer one multiplies
 * the vector of the shorter. A NULL column makes the row NULL; a NaN is refused by the scan before the derivation runs.
 *
 * A factor is stored in the fields of SiftV6Derive that the plan carries: cond[i] its column, cval[i] its constant k (at the scale s of the factor) and four bytes of else_bytes: negated (the factor is
 * k - x), p (x is multiplied by 10^p to the scale of the factor), s, and the scale of the column; aux is the scale of the result.
 */
#ifndef SIFT_AGG_V6_EXPR_NUM_H
#define SIFT_AGG_V6_EXPR_NUM_H

#include "postgres.h"

#include "agg/v6/v6_expr_str.h"
#include "agg/v6/v6_plan_rewrite.h"

#define SIFT_VN_MAX_FACTORS	3

/* e is the product of factors over numeric columns of the index (not one plain column): its derivation; the scale of its result is d->aux */
extern bool sift_vn_derive_of(const SiftV6Resolver *rs, const Expr *e, SiftV6Derive *d);

/* The scales of the columns of a derivation the plan carried as integers (they are not in the integers): after sift_v6_derive_unpack */
extern void sift_vn_derive_restore(SiftV6Derive *d);

/* The derivation a is the first factors of the derivation b: b is a's vector times the others */
extern bool sift_vn_prefix_of(const SiftV6Derive *a, const SiftV6Derive *b);

/* The vector of d for the rows of bs: cond[i] is the vector of the column of d->cond[i], prefix the vector of the first nprefix factors (the derivation sift_vn_prefix_of names; NULL: none) */
extern void sift_vn_derive(const SiftV6Derive *d, SiftVec *const *cond, const SiftVec *prefix, int nprefix, const SiftBatchSel *bs, SiftVxOut *out);

/*
 * CASE WHEN cond THEN val [ELSE 0 | NULL] END as the argument of sum() or count() is the aggregate with FILTER (WHERE cond) over val: sum(CASE WHEN c THEN x ELSE NULL END) is sum(x) FILTER (WHERE c), the
 * same for count; with ELSE 0, sum(..) is sum(x) FILTER (WHERE c) except that it is 0, not NULL, for a group that has rows and none of them passes c (and NULL for no rows at all): the rewrite is
 * CASE WHEN count(*) = 0 THEN NULL ELSE coalesce(sum(x) FILTER (WHERE c), 0) END. The same tree gives the same copy, so that the plan can make it again (sift_v6_collapse_offset_sums).
 */
extern Node *sift_vn_case_to_filter(Node *expr);

/* sum(x) FILTER (WHERE c) or count(x) FILTER (WHERE c) as sum(CASE WHEN c THEN x END): the aggregate of the argument that a scan computes as an expression when it decides no filter; NULL for any other aggregate */
extern Aggref *sift_vn_filter_to_case(const Aggref *a);

/* The expression of the statement as the planner and the plan of SiftAggV6 read it: CASE as FILTER, then the sums of an offset collapsed */
extern Node *sift_vn_rewrite_aggs(Node *expr);

#endif							/* SIFT_AGG_V6_EXPR_NUM_H */
