/*
 * agg/found.c - a join answered from the flag of a connected index (docs/COL-CONNECT.md, "Found")
 *
 * A semi, anti or inner join of the scanned table to the parent of a connection that returns no column of the parent asks one thing of each row: is there a parent
 * row with its key. The flag the child's segments hold ("<fk>#found") says it as of the stamp of the segment, so the parent is not read: the scan keeps the rows
 * whose flag is set, less the rows of the keys that changed in the parent since the oldest stamp (the journal of the parent index names them), plus the rows of
 * those keys that the parent holds now. That holds for every row only if the rows are all in stamped segments (no write buffer) and the journal vouches for the
 * stamp. A flag NULL is "not found then", which a parent row inserted since can change: so unless every segment is clean (no row had a key the parent lacked,
 * SIFT_CONN_STAMP_CLEAN: a foreign key keeps it so) no parent row may have been inserted since. Else the join reads the parent as ever.
 */
/* check-memory: allow-allocs every array here is bounded by FOUND_MAX_KEYS */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "core/compat.h"
#include "executor/tuptable.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg_int.h"
#include "agg/pred_int.h"
#include "col/am.h"
#include "col/conn.h"

/* the keys that changed in the parent since a stamp are bounded: more, and the join reads the parent (probing each one costs a descent of a btree) */
#define FOUND_MAX_KEYS	16384
/* a probe of the btree costs about 7,000 instructions, a join that loads the parent a few hundred for each of its rows: past this many keys that were deleted or changed the join is cheaper */
#define FOUND_MAX_PROBES	2048

bool		sift_agg_connect_found = true;

/* does the scanned index take columns from a parent (EXPLAIN shows where the parent was read from) */
bool
sift_agg_connected(const AggExec *ex)
{
	return ex->index != NULL && sift_col_is_index(ex->index) && sift_conn_specs(ex->index) != NULL;
}

/* is the join one the flag answers: a semi, anti or inner join by one integer column of the scanned table, with no column of the parent read, no condition of the parent's own */
static bool
found_possible(AggExec *ex, int ci)
{
	const SiftAggSpec *s = ex->spec;
	const SiftAggCombo *cb = &s->combo[ci];
	const SiftAggJoin *j = &s->join[cb->join];
	const SiftSrcLeaf *lf;

	if (!sift_agg_connect_found || s->topn || s->nparts > 0 || cb->level != 0 || j->relid == InvalidOid || j->child >= 0 || j->params != NIL)
		return false;
	if ((j->kind != SJ_INNER && j->kind != SJ_SEMI && j->kind != SJ_ANTI) || j->tkey[0] != NULL || j->nidx > 0 || j->fslot >= 0 || j->first.ok)
		return false;
	if (!semi_lut_possible(s, ci, ex->nbase))
		return false;
	lf = &ex->lay->leaf[cb->leaf[0]];
	return lf->kind == SCL_SCALAR && lf->parent < 0 && lf->path == NIL && (lf->vtype == SCV_I32 || lf->vtype == SCV_I64);
}

/* the btree of the parent that is unique on the key alone */
static Oid
parent_unique(Relation parent, AttrNumber key)
{
	List	   *oids = RelationGetIndexList(parent);
	Oid			found = InvalidOid;

	foreach_oid(ioid, oids)
	{
		Relation	idx;

		if (OidIsValid(found))
			break;
		idx = index_open(ioid, AccessShareLock);
		if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index->indisunique && idx->rd_index->indisvalid && idx->rd_index->indnkeyatts == 1 &&
			idx->rd_index->indkey.values[0] == key && RelationGetIndexPredicate(idx) == NIL)
			found = ioid;
		index_close(idx, AccessShareLock);
	}
	list_free(oids);
	return found;
}

static Datum
int_key_datum(Oid typid, int64 k)
{
	return typid == INT2OID ? Int16GetDatum((int16) k) : typid == INT4OID ? Int32GetDatum((int32) k) : Int64GetDatum(k);
}

/*
 * The keys of changed that a row of the parent visible to the scan holds. A key only updates ended is held for every snapshot (the update wrote a new version of the
 * key, and no one deleted it or changed it: the journal says so); the others are probed in the unique btree under the scan's snapshot. False: there is no btree to probe.
 */
static bool
parent_present(AggExec *ex, Relation parent, AttrNumber key, const SiftConnKeys *changed, int64 *out, uint32 *nout)
{
	Oid			uoid = InvalidOid;
	Relation	uidx;
	Oid			typid = TupleDescAttr(RelationGetDescr(parent), key - 1)->atttypid;
	TupleTableSlot *slot;
	IndexScanDesc scan;
	ScanKeyData skey;
	bool		probe = false;

	*nout = 0;
	for (uint32 i = 0; i < changed->n; i++)
	{
		probe |= !changed->kept[i];
		if (changed->kept[i])
			out[(*nout)++] = changed->keys[i];
	}
	if (!probe)
		return true;
	if (!OidIsValid(uoid = parent_unique(parent, key)))
		return false;
	uidx = index_open(uoid, AccessShareLock);
	slot = table_slot_create(parent, NULL);
	scan = sift_index_beginscan(parent, uidx, ex->snap, 1);
	for (uint32 i = 0; i < changed->n; i++)
	{
		Oid			intype = uidx->rd_opcintype[0];

		if (changed->kept[i])
			continue;
		ScanKeyEntryInitialize(&skey, 0, 1, BTEqualStrategyNumber, InvalidOid, uidx->rd_indcollation[0],
							   get_opcode(get_opfamily_member(uidx->rd_opfamily[0], intype, intype, BTEqualStrategyNumber)), int_key_datum(typid, changed->keys[i]));
		index_rescan(scan, &skey, 1, NULL, 0);
		if (index_getnext_slot(scan, ForwardScanDirection, slot))
			out[(*nout)++] = changed->keys[i];
		ex->lookup_heap++;
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	index_close(uidx, NoLock);
	return true;
}

SiftAggPred *
pred_node(AggExec *ex, SiftAggPredKind kind, int *total, int leaf, int op, int nchild, const char *desc)
{
	SiftAggPred *p = palloc0(sizeof(SiftAggPred));

	p->kind = kind;
	p->id = (*total)++;
	p->leaf = leaf;
	p->leaf2 = -1;
	p->op = op;
	p->nchild = nchild;
	p->child = nchild ? palloc0(nchild * sizeof(SiftAggPred *)) : NULL;
	p->desc = pstrdup(desc);
	ex->ps = repalloc(ex->ps, *total * sizeof(PredState));
	memset(&ex->ps[p->id], 0, sizeof(PredState));
	return p;
}

/* leaf IN (keys), or NOT IN: the keys of the leaf's type only */
SiftAggPred *
pred_in(AggExec *ex, int *total, int leaf, bool not_in, const int64 *keys, uint32 n, const char *desc)
{
	SiftAggPred *p = pred_node(ex, SAP_IN, total, leaf, not_in, 0, desc);
	PredState  *st = &ex->ps[p->id];
	int64	   *vals = palloc(Max(n, 1) * sizeof(int64));
	int			nv = 0;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	bool		i32 = ex->lay->leaf[leaf].vtype == SCV_I32;

	for (uint32 i = 0; i < n; i++)
	{
		if (i32 && (keys[i] < PG_INT32_MIN || keys[i] > PG_INT32_MAX))
			continue;
		vals[nv++] = keys[i];
		lo = Min(lo, keys[i]);
		hi = Max(hi, keys[i]);
	}
	in_build(st, vals, nv, lo, hi);
	st->ready = true;
	p->argtype = ex->lay->leaf[leaf].typid;
	pfree(vals);
	return p;
}

/*
 * The rows with a parent row: flag set and key unchanged, or key among the changed keys the parent holds now. The keys that are not in the leaf's range are in no row.
 * changed: the keys the journal names; present: those of them the parent holds.
 */
static SiftAggPred *
pred_exists(AggExec *ex, int *total, int found, int fk, const SiftConnKeys *changed, const int64 *present, uint32 npresent)
{
	SiftAggPred *root,
			   *both;

	if (changed->n == 0)
		return pred_node(ex, SAP_NULL, total, found, 1, 0, "connection flag is set");
	root = npresent > 0 ? pred_node(ex, SAP_OR, total, -1, 0, 2, "connection flag, key unchanged or present") : NULL;
	both = pred_node(ex, SAP_AND, total, -1, 0, 2, "connection flag and key unchanged");
	both->child[0] = pred_node(ex, SAP_NULL, total, found, 1, 0, "connection flag is set");
	both->child[1] = pred_in(ex, total, fk, true, changed->keys, changed->n, "key not changed in the parent");
	if (root == NULL)
		return both;
	root->child[0] = both;
	root->child[1] = pred_in(ex, total, fk, false, present, npresent, "key changed in the parent and present");
	return root;
}

/*
 * Answers the join of combo ci from the flag when the rows allow it: *add gets the conjunct that keeps the rows the join keeps, the combo gets an empty table and
 * passes every tuple. False: the join is the ordinary one (the reason, when the index is connected to the parent, is the one EXPLAIN says).
 */
bool
join_found(AggExec *ex, int ci, int *total, List **add)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	SiftConnStamp stamp;
	SiftConnKeys changed = {0};
	Relation	parent,
				pidx;
	Oid			pidxoid;
	int			found,
				fk;
	bool		clean;
	const char *why;
	int64	   *present;
	uint32		npresent = 0,
				nprobe = 0;
	SiftAggPred *p;
	ComboSt    *cs = &ex->cs[ci];

	if (!found_possible(ex, ci) || ex->index == NULL || sift_conn_find(ex->heap, ex->index, ex->lay->leaf[cb->leaf[0]].attno, j->relid, j->keyatt[0], &found, &pidxoid) < 0)
		return false;
	fk = cb->leaf[0];
	parent = table_open(j->relid, AccessShareLock);
	pidx = index_open(pidxoid, AccessShareLock);
	why = sift_colsrc_conn_proof(ex->src, found, &stamp.lineage, &stamp.xmin, &clean);
	stamp.reserved = 0;
	if (why == NULL)
	{
		SiftConnWhy verdict = sift_conn_vouches(parent, pidx, &stamp, ex->snap);

		if (verdict != SIFT_CONN_OK)
			why = sift_conn_why_name(verdict);
		else if (!clean && sift_conn_inserted_rows(parent, pidx, &stamp) > 0)
			why = "parent rows inserted since the stamp";
		else if ((verdict = sift_conn_changed(parent, pidx, &stamp, ex->snap, FOUND_MAX_KEYS, &changed)) != SIFT_CONN_OK)
			why = sift_conn_why_name(verdict);
	}
	present = palloc(Max(changed.n, 1) * sizeof(int64));
	for (uint32 i = 0; why == NULL && i < changed.n; i++)
		if (!changed.kept[i] && ++nprobe > FOUND_MAX_PROBES)
			why = "too many keys deleted or changed";
	if (why == NULL && !parent_present(ex, parent, j->keyatt[0], &changed, present, &npresent))
		why = "no unique index on the parent key";
	index_close(pidx, NoLock);
	table_close(parent, NoLock);
	if (why != NULL)
	{
		ex->found_why = ex->found_why ? ex->found_why : why;
		sift_conn_keys_free(&changed);
		return false;
	}
	if (j->kind == SJ_ANTI)
	{
		p = pred_node(ex, SAP_NOT_TRUE, total, -1, 0, 1, "no parent row");
		p->child[0] = pred_exists(ex, total, found, fk, &changed, present, npresent);
	}
	else
		p = pred_exists(ex, total, found, fk, &changed, present, npresent);
	*add = lappend(*add, p);
	cs->map = palloc0(sizeof(MapTab));
	cs->map->proof = true;
	cs->spass = true;
	ex->nfound++;
	ex->found_keys += changed.n;
	sift_conn_keys_free(&changed);
	return true;
}
