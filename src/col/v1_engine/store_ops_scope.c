/*
 * col/v1_engine/store_ops_scope.c - SiftScopeOps of Format 1: the scopes a sift_col index of format major 1 declares (col/scope/scope.h) for the scope planner (agg/scope/plan.c). Format 1 keeps
 * no cubes of a scope (exec NULL), so a join of the facts with the pairs of a key is the scan of the facts of the members the pairs give the key under the snapshot (agg/scope/scan.c), the
 * members read by the pairs reader of the scope (col/v1/scan/scope.c). Its scans are not routed: the granules of the facts hold the rows of every key (one_key and build_router NULL).
 */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/scope/scope.h"
#include "col/scope/scope.h"
#include "col/v1/conn/conn.h"
#include "col/v1/store_ops.h"

static bool
scope_readable(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX && sift_scope_specs(rel) != NULL;
}

static int
scope_count(Relation rel)
{
	return sift_scope_specs(rel)->n;
}

/* The index column of the member key of the facts, an integer the pairs reader codes as itself: 0 when there is none */
static int
member_col(Relation index, int ord)
{
	const AttrNumber fk = get_attnum(index->rd_index->indrelid, sift_scope_specs(index)->s[ord]->fk);

	for (int i = 0; i < index->rd_index->indnatts; i++)
		if (index->rd_index->indkey.values[i] == fk)
			return sift_conn_integer_type(TupleDescAttr(RelationGetDescr(index), i)->atttypid) ? i + 1 : 0;
	return 0;
}

static bool
scope_layout(Relation rel, int ord, SiftScopeLayout *out)
{
	*out = (SiftScopeLayout) {.member_col = member_col(rel, ord)};
	return out->member_col > 0;
}

static bool
scope_key_codes(Oid key_type, Oid typid)
{
	return sift_conn_integer_type(key_type) && sift_conn_integer_type(typid);
}

/* The members the pairs give the key under snap, as an array of member_type */
static ArrayType *
scope_key_members(Relation heap, Relation rel, int ord, Snapshot snap, Oid key_type, Datum key, Oid member_type, MemoryContext cx, uint32 *n,
				  const struct SiftScopeRouter **router)
{
	const int64 code = sift_scope_key_code(key_type, NULL, key);
	int64	   *members = sift_scope_members(heap, rel, sift_scope_specs(rel)->s[ord]->key_name, NULL, snap, &code, 1, n);
	Datum	   *vals = palloc(Max(*n, 1) * sizeof(Datum));
	int16		len;
	bool		byval;
	char		align;

	for (uint32 i = 0; i < *n; i++)
		vals[i] = sift_scope_key_datum(member_type, NULL, members[i]);
	*router = NULL;
	get_typlenbyvalalign(member_type, &len, &byval, &align);
	return construct_array(vals, (int) *n, member_type, len, byval, align);
}

static void
scope_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	sift_v6_scope_upper_paths(&sift_v1_scope_ops, root, input_rel, output_rel, extra);
}

const SiftScopeOps sift_v1_scope_ops = {
	.readable = scope_readable,
	.nscopes = scope_count,
	.layout = scope_layout,
	.key_codes = scope_key_codes,
	.key_members = scope_key_members,
	.upper_paths = scope_upper_paths
};
