/*
 * col/f2/store/ops_scope.c - SiftScopeOps of format major 2: what the summary of a scope of an index or a sift_table keeps (col/f2/scope/scope.h) for the scope planner (agg/scope/plan.c), the
 * executor of the plans that read it (col/f2/agg/scope/run.c), the members of a key, and the router that keeps a scan to the granules of one scope key (f2_scope_route.h).
 */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "utils/rel.h"

#include "col/col.h"
#include "col/f2/scope/route.h"
#include "col/f2/store/ops_int.h"
#include "col/f2/agg/scope/int.h"

StaticAssertDecl(SIFT_SCOPE_MEAS_CAP == SIFT_SCOPE_LAYOUT_MEAS, "the layout of a scope holds the measures of the summary");

/* A table that declares scopes, or an index that does, tracks its changes and is not stale: its summary is current */
static bool
scope_readable(Relation rel)
{
	return sift_f2_scoped(rel) && (rel->rd_rel->relkind != RELKIND_INDEX || (sift_col_rel(rel)->track && !sift_col_rel(rel)->stale));
}

static bool
scope_layout(Relation rel, int ord, SiftScopeLayout *out)
{
	SiftF2ScopeLayout l;

	if (!sift_f2_scope_layout(rel, ord, &l))
		return false;
	*out = (SiftScopeLayout) {.member_col = l.member_col,.time_col = l.time_col,.nmeas = l.nmeas};
	memcpy(out->meas_col, l.meas_col, sizeof(l.meas_col));
	memcpy(out->meas_scale, l.meas_scale, sizeof(l.meas_scale));
	return true;
}

/* An integer to an integer, a date, timestamp or timestamptz to its own type, a string to a string, a uuid to a uuid: the codes of the key map, or the word itself */
static bool
scope_key_codes(Oid key_type, Oid typid)
{
	return sift_keymap_same_kind(key_type, typid) || (sift_scope_word_type(key_type) && typid == key_type);
}

/* The members of a key and the granules of the key that its summary names (NULL when it cannot route) */
static ArrayType *
scope_key_members(Relation heap, Relation rel, int ord, Snapshot snap, Oid key_type, Datum key, Oid member_type, MemoryContext cx, uint32 *n,
				  const struct SiftScopeRouter **router)
{
	SiftKeyMap *km = sift_keymap_type(key_type) ? sift_f2_scope_keymap(rel, ord) : NULL;
	const int64 code = sift_scope_key_code(key_type, km, key);

	*router = sift_f2_scope_build_router(rel, ord, snap, &code, 1, km, cx);
	return sift_f2s_key_members(heap, rel, ord, snap, km, code, member_type, n);
}

static void
scope_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	sift_v6_scope_upper_paths(&sift_f2_scope_ops, root, input_rel, output_rel, extra);
}

static void
scope_topk_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	sift_v6_scope_topk_paths(&sift_f2_scope_ops, root, input_rel, output_rel);
}

static bool
scope_one_key(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *pairs, IndexOptInfo *ix, AttrNumber fact_att, AttrNumber pairs_att, int *ord, Expr **key)
{
	return sift_v6_scope_one_key(&sift_f2_scope_ops, root, fact, pairs, ix, fact_att, pairs_att, ord, key);
}

static const struct SiftScopeRouter *
scope_build_router(Relation index, int ord, Snapshot snap, const int64 *keys, int nkeys, MemoryContext cx)
{
	return sift_f2_scope_build_router(index, ord, snap, keys, (uint32) nkeys, NULL, cx);
}

static void
scope_router_counts(const struct SiftScopeRouter *r, int64 *kept, int64 *total)
{
	*kept = r->mask.kept;
	*total = r->mask.n;
}

const SiftScopeOps sift_f2_scope_ops = {
	.readable = scope_readable,
	.nscopes = sift_f2_nscopes,
	.layout = scope_layout,
	.excl_ok = sift_f2_scope_excl_ok,
	.ring_col = sift_f2_scope_ring_eligible,
	.key_codes = scope_key_codes,
	.exec = &sift_agg_v6_scope_plan_methods,
	.key_members = scope_key_members,
	.upper_paths = scope_upper_paths,
	.topk_paths = scope_topk_paths,
	.one_key = scope_one_key,
	.build_router = scope_build_router,
	.router_counts = scope_router_counts
};
