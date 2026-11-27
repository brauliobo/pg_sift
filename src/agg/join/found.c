/*
 * agg/join/found.c - a dimension of a star join answered from the found flag of a connected index (agg/join/found.h).
 */
/* check-memory: allow-allocs every array here is bounded by FOUND_MAX_KEYS */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "parser/parsetree.h"

#include "agg/dim_cache.h"
#include "agg/join/found.h"
#include "col/am/conn_route.h"

/* the keys that changed in the parent since a stamp are bounded: more, and the dimension is read (each costs a search of the sorted keys a row) */
#define FOUND_MAX_KEYS		16384
/* a probe of the btree of the parent costs about 7,000 instructions, a read of the parent a few hundred a row: past this many keys that were deleted or changed the read is cheaper */
#define FOUND_MAX_PROBES	2048

struct SiftV6Found
{
	int			conn;
	SiftV6JoinMode mode;
	SiftConnKeys changed;		/* the keys the journal names: a flag set for one of them says nothing */
	SiftConnKeys present;		/* those of them the parent holds now */
};

bool
sift_v6_found_serves(const SiftV6Spec *spec, const SiftV6FoundFact *fact)
{
	return fact->index != NULL && fact->ops->conn != NULL && !spec->rows && !spec->ext->unnest && spec->ext->resid == NIL;
}

static bool
int_key(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID;
}

/* the join asks only whether a parent row has the key: one integer key of the fact index, no condition of the dimension */
static bool
found_shape(const SiftV6Ext *x, int d)
{
	const SiftV6XDim *xd = &x->dim[d];

	return (xd->mode == SV6J_INNER_EQUI || xd->mode == SV6J_SEMI_EQUI || xd->mode == SV6J_ANTI_EQUI) && xd->nkeys == 1 && xd->fsrc[0] == SV6SRC_ATTR &&
		xd->kx[0] == 0 && xd->kres[0] == 0 && xd->kvarno[0] == 0 && x->on[d] == NIL && int_key(xd->ftype[0]) && int_key(xd->ktype[0]);
}

/* the columns the join reads of the dimension are its key, which a matched row has the value of the fact's key in */
static bool
reads_key_only(const SiftV6XDim *xd, PlanState *ps, AttrNumber key)
{
	AttrNumber	att;

	for (int c = 0; c < xd->npay; c++)
		if (sift_dimcache_whole_table(ps, xd->pcol[c], &att) == NULL || att != key || xd->ptype[c] != xd->ktype[0])
			return false;
	return true;
}

/* The changed keys the parent holds now: a key only updates that kept it ended is held for every snapshot, the others are probed in its unique btree under the snapshot */
static void
present_keys(SiftV6Found *f, Relation parent, AttrNumber key, Snapshot snap, MemoryContext cx, SiftV6FoundStats *stats)
{
	int64	   *probe = MemoryContextAlloc(cx, Max(f->changed.n, 1) * sizeof(int64));
	bool	   *one = MemoryContextAlloc(cx, Max(f->changed.n, 1) * sizeof(bool));
	uint32		nprobe = 0;
	SiftConnProbe *p;

	f->present.keys = MemoryContextAlloc(cx, Max(f->changed.n, 1) * sizeof(int64));
	for (uint32 i = 0; i < f->changed.n; i++)
		if (!f->changed.kept[i])
			probe[nprobe++] = f->changed.keys[i];
	if (nprobe > 0)
	{
		p = sift_conn_probe_begin(cx, parent, key, snap);
		sift_conn_probe_batch(p, probe, NULL, nprobe, one);
	}
	for (uint32 i = 0, j = 0; i < f->changed.n; i++)
		if (f->changed.kept[i] || one[j++])
			f->present.keys[f->present.n++] = f->changed.keys[i];
	stats->probes += nprobe;
}

/* why the flags of connection c of the fact do not tell the rows of the parent under the snapshot (NULL: they do, and f has the keys outside the proof) */
static const char *
prove(const SiftV6FoundFact *fact, Relation parent, Relation pidx, SiftV6Found *f)
{
	SiftConnStamp stamp;
	bool		clean;
	const char *why = fact->ops->conn->proof(fact->index, f->conn, &stamp, &clean);
	SiftConnWhy verdict;
	uint32		nprobe = 0;

	if (why != NULL)
		return why;
	if ((verdict = sift_jrnl_vouches(parent, pidx, &stamp, fact->snap)) != SIFT_CONN_OK)
		return sift_conn_why_name(verdict);
	if (!clean && sift_jrnl_inserted_rows(parent, pidx, &stamp) > 0)
		return "parent rows inserted since the stamp";
	if ((verdict = sift_jrnl_changed(parent, pidx, &stamp, fact->snap, FOUND_MAX_KEYS, &f->changed)) != SIFT_CONN_OK)
		return sift_conn_why_name(verdict);
	for (uint32 i = 0; i < f->changed.n; i++)
		nprobe += !f->changed.kept[i];
	return nprobe > FOUND_MAX_PROBES ? "too many keys deleted or changed" : NULL;
}

/* a table every row of which the join reads */
static bool
whole_rel(const RelOptInfo *rel)
{
	return rel->reloptkind == RELOPT_BASEREL && rel->rtekind == RTE_RELATION && rel->baserestrictinfo == NIL;
}

/* does index fact_index connect its attribute fatt to the column key of the table of rel */
static bool
index_conn(PlannerInfo *root, const SiftStoreOps *ops, Oid fact_index, int fatt, const RelOptInfo *rel, AttrNumber key)
{
	Relation	index = index_open(fact_index, AccessShareLock);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	Oid			pidx;
	const int	conn = ops->conn->conn(heap, index, fatt, planner_rt_fetch(rel->relid, root)->relid, key, &pidx);

	table_close(heap, NoLock);
	index_close(index, NoLock);
	return conn >= 0;
}

bool
sift_v6_found_planned(PlannerInfo *root, const SiftStoreOps *ops, Oid fact_index, const SiftV6Ext *x, int d, const Path *child)
{
	const SiftV6XDim *xd = &x->dim[d];

	if (ops == NULL || ops->conn == NULL || !found_shape(x, d) || !whole_rel(child->parent))
		return false;
	for (int c = 0; c < xd->npay; c++)
		if (xd->pcol[c] != xd->kcol[0])
			return false;
	return index_conn(root, ops, fact_index, xd->fatt[0], child->parent, xd->kcol[0]);
}

SiftV6Found *
sift_v6_found_open(const SiftV6FoundFact *fact, const SiftV6Ext *x, int d, PlanState *ps, MemoryContext cx, SiftV6FoundStats *stats)
{
	AttrNumber	key;
	Relation	parent,
				pidx;
	Oid			pidxoid;
	int			conn;
	SiftV6Found *f;
	const char *why;

	if (!found_shape(x, d) || (parent = sift_dimcache_whole_table(ps, x->dim[d].kcol[0], &key)) == NULL || !reads_key_only(&x->dim[d], ps, key) ||
		(conn = fact->ops->conn->conn(fact->heap, fact->index, x->dim[d].fatt[0], RelationGetRelid(parent), key, &pidxoid)) < 0)
		return NULL;
	f = MemoryContextAllocZero(cx, sizeof(SiftV6Found));
	f->conn = conn;
	f->mode = x->dim[d].mode;
	pidx = index_open(pidxoid, AccessShareLock);
	why = prove(fact, parent, pidx, f);
	index_close(pidx, NoLock);
	if (why != NULL)
	{
		stats->why = stats->why != NULL ? stats->why : why;
		sift_conn_keys_free(&f->changed);
		pfree(f);
		return NULL;
	}
	present_keys(f, parent, key, fact->snap, cx, stats);
	stats->proofs++;
	stats->keys += f->changed.n;
	return f;
}

SiftV6Col
sift_v6_found_col(const SiftV6Found *f)
{
	return (SiftV6Col) {.attno = SIFT_V6_FOUND_ATTNO(f->conn),.typid = BOOLOID,.src = SV6SRC_ATTR};
}

static inline bool
valid_at(const SiftVec *v, uint32 r)
{
	return v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1);
}

void
sift_v6_found_apply(const SiftV6Found *f, const SiftVec *flag, const SiftVec *key, SiftBatchSel *bs)
{
	const bool	anti = f->mode == SV6J_ANTI_EQUI;

	SIFT_INVARIANT(flag->dense && key->dense && flag->type == SVEC_I64 && key->type == SVEC_I64, "the found flag and the key of a join are dense integer columns");
	for (uint32 r = 0; r < bs->n_raw; r++)
	{
		const uint64 bit = UINT64CONST(1) << (r & 63);
		int64		k;
		bool		match;

		if (!(bs->mask[r >> 6] & bit))
			continue;
		k = ((const int64 *) key->data)[r];
		match = valid_at(key, r) &&
			((valid_at(flag, r) && ((const int64 *) flag->data)[r] != 0 && !sift_conn_has(&f->changed, k)) || sift_conn_has(&f->present, k));
		if (match == anti)
			bs->mask[r >> 6] &= ~bit;
	}
}

void
sift_v6_found_explain(const SiftV6FoundStats *s, ExplainState *es)
{
	if (s->proofs > 0)
	{
		ExplainPropertyInteger("Found Proofs", NULL, s->proofs, es);
		ExplainPropertyInteger("Found Keys Outside The Proof", NULL, (int64) s->keys, es);
		ExplainPropertyInteger("Parent Heap Fetches", NULL, (int64) s->probes, es);
	}
	if (s->why != NULL)
		ExplainPropertyText("Found Fallback", s->why, es);
}
