/*
 * agg/lookup/load.c - the load of a joined table into a lookup table
 * The probe of the table, the load of a table or a child, the maps shared between scans,
 * and map_load.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "portability/instr_time.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/lookup/int.h"

/*
 * The arms of one statement (UNION ALL, an inlined CTE read several times) that join the same table with the same columns and own conditions read
 * the same rows in the same snapshot: the first arm loads them into a context of the statement and the others use that table, which no scan changes.
 * A table is shared within one EState only; the entry goes with the context (a parallel worker has no EState and loads its own).
 */
typedef struct SharedMap
{
	EState	   *estate;
	char	   *key;
	MapTab	   *map;
	MemoryContextCallback cb;
} SharedMap;

static List *shared_maps = NIL;

/*
 * The rows of a table that its dependent lookup keys, read through a btree on the column: one probe a key, where the table is read page by page. It pays while the keys
 * are few against the pages (sift_agg_probe_pays). Returns the index opened, with the scan key of the array of keys; NULL: read the table whole.
 */
static Relation
map_probe(AggExec *ex, int ci, Relation rel, ScanKeyData *key)
{
	const MapTab *dm;
	Relation	best = NULL;
	Oid			typ = InvalidOid;
	double		rpk;
	bool		unique = false;
	int			att = 0,
				cd = map_keyed_dependent(ex, ci, &att, &typ);
	Datum	   *vals;
	int			n = 0;
	int16		typlen;
	bool		typbyval;
	char		typalign;

	if (cd < 0)
		return NULL;
	dm = ex->cs[cd].map;
	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);

		if (best == NULL && idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index->indisvalid && heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			idx->rd_index->indkey.values[0] == att && idx->rd_opcintype[0] == typ)
		{
			best = idx;
			unique = idx->rd_index->indisunique && idx->rd_index->indnkeyatts == 1;
		}
		else
			index_close(idx, AccessShareLock);
	}
	if (best == NULL)
		return NULL;
	rpk = sift_agg_rows_per_key(RelationGetRelid(rel), att, rel->rd_rel->reltuples);
	if ((rpk <= 0 && !unique) || !sift_agg_probe_pays(dm->kd[0].n, dm->kd[0].n * Max(rpk, 1.0), RelationGetNumberOfBlocks(rel)))
	{
		index_close(best, AccessShareLock);
		return NULL;
	}
	vals = palloc((dm->kd[0].n + 1) * sizeof(Datum));
	for (uint32 i = 0; i < dm->kd[0].n; i++)
	{
		int64		v = dm->kd[0].ival[i];

		if (typ == INT4OID && (v < PG_INT32_MIN || v > PG_INT32_MAX))
			continue;			/* no int4 value equals it */
		vals[n++] = typ == INT4OID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
	}
	get_typlenbyvalalign(typ, &typlen, &typbyval, &typalign);
	ScanKeyEntryInitialize(key, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, typ, InvalidOid,
						   get_opcode(get_opfamily_member(best->rd_opfamily[0], typ, typ, BTEqualStrategyNumber)),
						   PointerGetDatum(construct_array(vals, n, typ, typlen, typbyval, typalign)));
	pfree(vals);
	return best;
}

/* the rows of the joined table, visible to the scan's snapshot, hashed by key; from an index when its own conditions name a value of a btree, from the cache of an earlier query when the table did not change */
MapTab *
map_load_table(AggExec *ex, int ci, MemoryContext cxt)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ExprState  *own = sift_agg_lookup_pushdown && j->fslot >= 0 && !sift_aggjoin_preserves(j->kind) && !slot_reads_leaf(cb, j->fslot) ?
		ex->cs[ci].slot[j->fslot] : NULL;
	MapTab	   *m;
	Relation	rel = table_open(j->relid, AccessShareLock);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	ScanKeyData keys[SIFT_AGG_MAX_JIDX];
	Datum		val[SIFT_AGG_MAX_JIDX];
	int			nkeys = 0;
	bool		none = false;
	Relation	idx;
	KeptTry		kept = {.cx = cxt};
	SiftAggJoin lj = *j;
	ExprState  *firstq = NULL;
	ExprContext *firstcx = NULL;
	FirstOrd	ord = {0};
	instr_time	t0;

	INSTR_TIME_SET_CURRENT(t0);
	for (int k = 0; sift_agg_lookup_pushdown && k < j->nidx && !none; k++)
	{
		bool		isnull;
		Oid			t = exprType(j->idxval[k]);
		Datum		d = ExecEvalExprSwitchContext(ExecInitExpr((Expr *) j->idxval[k], EXPR_PARENT(ex)), ex->lutcx, &isnull);

		none = isnull;			/* the equality is never true of a NULL: no row can match */
		val[k] = isnull ? (Datum) 0 : datumCopy(d, get_typbyval(t), get_typlen(t));
	}
	ResetExprContext(ex->lutcx);
	idx = sift_agg_lookup_pushdown && j->nidx > 0 && !none ? map_index(rel, j, val, keys, &nkeys) : NULL;
	if (idx == NULL && !none && sift_agg_lookup_pushdown && j->nidx == 0 && (idx = map_probe(ex, ci, rel, keys)) != NULL)
		nkeys = 1;
	if (idx == NULL && !none && (m = map_kept_find(ex, ci, rel, cxt, own != NULL, &kept)) != NULL)
	{
		MemoryContextSwitchTo(old);
		ExecDropSingleTupleTableSlot(slot);
		table_close(rel, NoLock);
		map_loaded(ex, 0, t0);
		return m;
	}
	if (ex->first_peek)
	{
		/* the table is asked for when it costs nothing: not in a cache, it is read when the child has run enough (map_first_row) */
		if (idx)
			index_close(idx, AccessShareLock);
		MemoryContextSwitchTo(old);
		ExecDropSingleTupleTableSlot(slot);
		table_close(rel, NoLock);
		return NULL;
	}
	/* a table kept for other arms too is loaded with the columns of them all */
	if (kept.cols.n > 0)
	{
		lj.ncol = kept.cols.n;
		for (int c = 0; c < lj.ncol; c++)
		{
			lj.colatt[c] = kept.cols.att[c];
			lj.coltype[c] = kept.cols.type[c];
		}
	}
	if (j->first.qual != NIL)
	{
		firstq = ExecInitQual(j->first.qual, NULL);
		firstcx = CreateStandaloneExprContext();
	}
	MemoryContextSwitchTo(kept.cx);
	m = map_new(&lj, own != NULL, kept.cx);
	m->lazy = kept.cols.n == 0 && lookup_keys_only(ex->spec, ex->lay, ci, ex->nbase);
	/* the key expressions live for the load, not in the context a cache keeps */
	MemoryContextSwitchTo(old);
	for (int k = 0; k < j->nkey; k++)
		if (j->tkey[k])
		{
			m->kcx = m->kcx ? m->kcx : CreateStandaloneExprContext();
			m->kx[k] = ExecInitExpr((Expr *) j->tkey[k], NULL);
		}
	MemoryContextSwitchTo(kept.cx);
	if (idx)
	{
		IndexScanDesc iscan = sift_index_beginscan(rel, idx, ex->snap, nkeys);

		index_rescan(iscan, keys, nkeys, NULL, 0);
		while (index_getnext_slot(iscan, ForwardScanDirection, slot))
			map_row(ex, m, slot, &lj, cb, own);
		index_endscan(iscan);
		index_close(idx, AccessShareLock);
		ex->lookup_index++;
	}
	else if (!none && !map_load_cols(ex, m, &lj, cb, own, rel))
	{
		/* the heap tests `column = value` on the page, before it makes a tuple of a row; another access method ignores the keys, the filter holds them too */
		bool		heap = rel->rd_tableam == GetHeapamTableAmRoutine();
		TableScanDesc scan;

		for (int k = 0; heap && sift_agg_lookup_pushdown && k < j->nidx; k++)
			if (!j->idxany[k])
				ScanKeyEntryInitialize(&keys[nkeys++], 0, j->idxatt[k], BTEqualStrategyNumber, InvalidOid, j->idxcoll[k], get_opcode(j->idxop[k]), val[k]);
		scan = sift_table_beginscan(rel, ex->snap, nkeys, keys);

		/* a first-row lookup of a child that tests more than its key: the rows it would not return are not loaded */
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool		pass = true;

			if (firstq)
			{
				firstcx->ecxt_scantuple = slot;
				pass = ExecQual(firstq, firstcx);
				ResetExprContext(firstcx);
			}
			if (pass)
			{
				uint32		n = m->nrow;

				map_row(ex, m, slot, &lj, cb, own);
				if (j->first.nord > 0 && m->nrow > n)
					first_order_row(m, &j->first, &ord, slot);
			}
		}
		table_endscan(scan);
	}
	if (ord.cx)
	{
		map_keep_first(m, &j->first, &ord);
		MemoryContextDelete(ord.cx);
	}
	if (j->first.hybrid)
		map_keep_agreeing(m);
	map_index_rows(m);
	if (m->kcx)
	{
		FreeExprContext(m->kcx, true);
		m->kcx = NULL;
		memset(m->kx, 0, sizeof(m->kx));
	}
	if (firstcx)
		FreeExprContext(firstcx, true);
	MemoryContextSwitchTo(old);
	map_kept_keep(rel, &kept, m, cxt);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, NoLock);
	map_loaded(ex, m->nrow, t0);
	return kept.cols.n > 0 ? map_view(m, &kept.cols, j, cxt) : m;
}

/* the rows of a child plan (a sub-select, VALUES list or function), run once; the rows are the plan's, in the query's snapshot */
static MapTab *
map_load_child(AggExec *ex, int ci)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ExprState  *own = sift_agg_lookup_pushdown && j->fslot >= 0 && !sift_aggjoin_preserves(j->kind) && !slot_reads_leaf(cb, j->fslot) ?
		ex->cs[ci].slot[j->fslot] : NULL;
	PlanState  *child = j->subplan > 0 ? list_nth(ex->css.ss.ps.state->es_subplanstates, j->subplan - 1) : list_nth(ex->css.custom_ps, j->child);
	MemoryContext caller = MemoryContextSwitchTo(ex->cxt);
	MapTab	   *m = map_new(j, own != NULL, ex->cxt);
	instr_time	t0;

	INSTR_TIME_SET_CURRENT(t0);
	/* the top-N that starts again (topn_widen) reads the rows of a plan that has run to its end */
	if (ex->widen > 0)
	{
		MemoryContextSwitchTo(caller);
		ExecReScan(child);
		MemoryContextSwitchTo(ex->cxt);
	}
	for (;;)
	{
		TupleTableSlot *slot;

		MemoryContextSwitchTo(caller);
		slot = ExecProcNode(child);
		MemoryContextSwitchTo(ex->cxt);
		if (TupIsNull(slot))
			break;
		map_row(ex, m, slot, j, cb, own);
	}
	map_index_rows(m);
	MemoryContextSwitchTo(caller);
	map_loaded(ex, m->nrow, t0);
	return m;
}

static void
shared_map_forget(void *arg)
{
	shared_maps = list_delete_ptr(shared_maps, arg);
}

/*
 * Is the table kept without the dictionary of its keys? The join is answered from a byte table of the keys of the table (semi_lut_build), which asks for the key of
 * each row and its columns: the ids of the keys and the chains of the rows (a hash insert of each key that misses the cache) are made by the first use that needs
 * them (map_ensure), which is a use of the table as a lookup. The planner prices the load by the same test.
 */
bool
lookup_keys_only(const SiftAggSpec *s, const SiftSrcLayout *lay, int ci, int nbase)
{
	const SiftAggCombo *cb = &s->combo[ci];
	const SiftAggJoin *j = &s->join[cb->join];

	return semi_lut_possible(s, ci, nbase) && lay->leaf[cb->leaf[0]].vtype == SCV_I32 && j->params == NIL && j->child < 0 && j->first.qual == NIL && !j->first.hybrid &&
		j->first.nord == 0;
}

/* what decides the rows of a table: the relation, its key and (cols) output columns and the conditions that narrow the load */
char *
shared_map_key(AggExec *ex, int ci, bool cols)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	bool		own = sift_agg_lookup_pushdown && j->fslot >= 0 && !slot_reads_leaf(cb, j->fslot);
	StringInfoData k;

	initStringInfo(&k);
	/* an own condition reads the table's columns as Params nleaves + i: the same text over another number of leaves is another condition */
	appendStringInfo(&k, "%u %d %d %d %d|", j->relid, own, sift_agg_lookup_pushdown, own ? cb->nleaves : 0, j->nkey);
	for (int i = 0; i < j->nkey; i++)
		appendStringInfo(&k, "%d/%u/%d/%s ", j->keyatt[i], j->keytype[i], j->keynull[i], j->tkey[i] ? nodeToString(j->tkey[i]) : "");
	if (cols || own)
	{
		appendStringInfo(&k, "|%d|", j->ncol);
		for (int i = 0; i < j->ncol; i++)
			appendStringInfo(&k, "%d/%u ", j->colatt[i], j->coltype[i]);
	}
	if (own)
		appendStringInfo(&k, "|%s", nodeToString(cb->slot[j->fslot].expr));
	if (lookup_keys_only(ex->spec, ex->lay, ci, ex->nbase))
		appendStringInfoString(&k, "|keys");
	if (j->first.qual != NIL)
		appendStringInfo(&k, "|first %s", nodeToString(j->first.qual));
	if (j->first.hybrid)
		appendStringInfoString(&k, "|agree");
	for (int i = 0; i < j->first.nord; i++)
		appendStringInfo(&k, "|ord %d/%d/%d", j->first.ordatt[i], j->first.orddesc[i], j->first.ordnullsfirst[i]);
	for (int i = 0; sift_agg_lookup_pushdown && i < j->nidx; i++)
		appendStringInfo(&k, "|%d/%u/%u/%d/%s", j->idxatt[i], j->idxop[i], j->idxcoll[i], j->idxany[i], nodeToString(j->idxval[i]));
	return k.data;
}

static MapTab *
map_load_shared(AggExec *ex, int ci)
{
	EState	   *estate = ex->css.ss.ps.state;
	char	   *key;
	SharedMap  *sm;
	MemoryContext mcxt;
	MemoryContext old;

	if (estate == NULL)
		return map_load_table(ex, ci, ex->cxt);
	key = shared_map_key(ex, ci, true);
	foreach_ptr(SharedMap, e, shared_maps)
		if (e->estate == estate && strcmp(e->key, key) == 0)
		{
			ex->nshared++;
			return e->map;
		}
	mcxt = AllocSetContextCreate(estate->es_query_cxt, "sift agg shared lookup", ALLOCSET_DEFAULT_SIZES);
	sm = MemoryContextAlloc(mcxt, sizeof(SharedMap));
	sm->estate = estate;
	sm->key = MemoryContextStrdup(mcxt, key);
	sm->map = map_load_table(ex, ci, mcxt);
	sm->cb.func = shared_map_forget;
	sm->cb.arg = sm;
	MemoryContextRegisterResetCallback(mcxt, &sm->cb);
	old = MemoryContextSwitchTo(TopMemoryContext);
	shared_maps = lappend(shared_maps, sm);
	MemoryContextSwitchTo(old);
	return sm->map;
}

/* the rows of the joined table or plan, hashed by key */
MapTab *
map_load(AggExec *ex, int ci)
{
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];
	int			att;
	Oid			typ;

	if (ex->pmaps && ex->pmaps[ci])
		return map_share_attach(ex->pmaps[ci], j, ex->cxt);
	if (j->params != NIL)
		return map_new(j, false, ex->cxt);
	if (j->child >= 0)
		return map_load_child(ex, ci);
	return sift_agg_share_lookups && !sift_aggjoin_preserves(j->kind) && !(sift_agg_lookup_pushdown && map_keyed_dependent(ex, ci, &att, &typ) >= 0) ? map_load_shared(ex, ci) : map_load_table(ex, ci, ex->cxt);
}

/* does an expression read one of the first n parameters (the leaves of a combo)? */
static bool
reads_leaf_param(Node *n, int *nleaves)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid < *nleaves)
		return true;
	return expression_tree_walker(n, reads_leaf_param, nleaves);
}

/* does the expression of a slot of the combo read one of its leaves? */
bool
slot_reads_leaf(const SiftAggCombo *cb, int sl)
{
	int			nleaves = cb->nleaves;

	return reads_leaf_param((Node *) cb->slot[sl].expr, &nleaves);
}
