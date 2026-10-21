/*
 * agg/lookup.c - the load of a joined table into a lookup table, at scan start
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

static void
map_add(AggExec *ex, MapTab *m, TupleTableSlot *slot, const SiftAggJoin *j)
{
	size_t		kb = (size_t) m->nrow * m->nkey,
				cb = (size_t) m->nrow * m->ncol;
	Datum		kd[SIFT_AGG_MAX_JKEYS];
	bool		knull[SIFT_AGG_MAX_JKEYS];
	bool		anynull = false;

	for (int k = 0; k < m->nkey; k++)
	{
		kd[k] = slot_getattr(slot, j->keyatt[k], &knull[k]);
		anynull |= knull[k];
	}
	if (anynull && !m->keep)
		return;					/* NULL equals nothing */
	if (m->nrow == m->cap)
	{
		m->cap = Max(m->cap * 2, 256);
		m->kptr = m->kptr ? repalloc(m->kptr, (size_t) m->cap * m->nkey * sizeof(char *)) :
			MemoryContextAlloc(m->cxt, (size_t) m->cap * m->nkey * sizeof(char *));
		m->klen = m->klen ? repalloc(m->klen, (size_t) m->cap * m->nkey * sizeof(uint32)) :
			MemoryContextAlloc(m->cxt, (size_t) m->cap * m->nkey * sizeof(uint32));
		m->kint = m->kint ? repalloc(m->kint, (size_t) m->cap * m->nkey * sizeof(int64)) :
			MemoryContextAlloc(m->cxt, (size_t) m->cap * m->nkey * sizeof(int64));
		m->col = m->col ? repalloc(m->col, (size_t) m->cap * Max(m->ncol, 1) * sizeof(Datum)) :
			MemoryContextAlloc(m->cxt, (size_t) m->cap * Max(m->ncol, 1) * sizeof(Datum));
		m->cnull = m->cnull ? repalloc(m->cnull, (size_t) m->cap * Max(m->ncol, 1)) :
			MemoryContextAlloc(m->cxt, (size_t) m->cap * Max(m->ncol, 1));
		if (m->keep)
			m->nokey = m->nokey ? repalloc(m->nokey, m->cap) : MemoryContextAlloc(m->cxt, m->cap);
	}
	if (m->keep)
		m->nokey[m->nrow] = anynull;
	for (int k = 0; k < m->nkey; k++)
	{
		if (knull[k])
		{
			m->kptr[kb + k] = NULL;
			m->klen[kb + k] = 0;
			m->kint[kb + k] = 0;
		}
		else if (m->ktext[k])
		{
			struct varlena *v = PG_DETOAST_DATUM_COPY(kd[k]);

			m->kptr[kb + k] = VARDATA_ANY(v);
			m->klen[kb + k] = VARSIZE_ANY_EXHDR(v);
		}
		else
			m->kint[kb + k] = sift_aggspec_operand_int(j->keytype[k], kd[k]);
	}
	for (int c = 0; c < m->ncol; c++)
	{
		int16		len;
		bool		byval;
		Datum		d = slot_getattr(slot, j->colatt[c], &m->cnull[cb + c]);

		get_typlenbyval(j->coltype[c], &len, &byval);
		m->col[cb + c] = m->cnull[cb + c] ? (Datum) 0 : byval ? d : len == -1 ? PointerGetDatum(PG_DETOAST_DATUM_COPY(d)) :
			datumCopy(d, byval, len);
	}
	m->nrow++;
}

/* a row the scan of the table returned: into the table when it holds the ON clause's own conditions (`own`, when they read no leaf: a row that fails them matches no tuple) */
void
map_row(AggExec *ex, MapTab *m, TupleTableSlot *slot, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own)
{
	ex->lookup_read++;
	if (own)
	{
		bool		isnull;
		Datum		d;

		for (int c = 0; c < j->ncol; c++)
		{
			bool		cnull;
			Datum		v = slot_getattr(slot, j->colatt[c], &cnull);

			bind_param(ex, cb->nleaves + c, v, cnull);
		}
		d = ExecEvalExprSwitchContext(own, ex->lutcx, &isnull);
		if ((ex->lookup_read & 1023) == 0)
			ResetExprContext(ex->lutcx);
		if (isnull || !DatumGetBool(d))
			return;
	}
	map_add(ex, m, slot, j);
}

/*
 * The btree of the joined table that starts with columns the table's `column = value` conditions give (the planner found them, join_index_quals),
 * opened, with the keys of its scan: the rows that can match are those it returns. NULL when no index fits; *none when a value is NULL (no row).
 */
static Relation
map_index(Relation rel, const SiftAggJoin *j, const Datum *val, ScanKeyData *keys, int *nkeys)
{
	Relation	best = NULL;
	int			nbest = 0;

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		ScanKeyData ik[SIFT_AGG_MAX_JIDX];
		int			n = 0;

		if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index->indisvalid && heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL))
			for (; n < idx->rd_index->indnkeyatts && n < SIFT_AGG_MAX_JIDX; n++)
			{
				int			k;
				int			strategy;
				Oid			lt,
							rt;

				for (k = 0; k < j->nidx; k++)
					if (j->idxatt[k] == idx->rd_index->indkey.values[n] && op_in_opfamily(j->idxop[k], idx->rd_opfamily[n]) &&
						idx->rd_indcollation[n] == j->idxcoll[k])
						break;
				if (k == j->nidx)
					break;
				get_op_opfamily_properties(j->idxop[k], idx->rd_opfamily[n], false, &strategy, &lt, &rt);
				if (strategy != BTEqualStrategyNumber)
					break;
				ScanKeyEntryInitialize(&ik[n], 0, n + 1, strategy, rt, j->idxcoll[k], get_opcode(j->idxop[k]), val[k]);
			}
		if (n > nbest)
		{
			if (best)
				index_close(best, AccessShareLock);
			best = idx;
			nbest = n;
			memcpy(keys, ik, n * sizeof(ScanKeyData));
		}
		else
			index_close(idx, AccessShareLock);
	}
	*nkeys = nbest;
	return best;
}

/* an empty table of the rows of a join; filtered: only rows that hold the ON clause's own conditions are loaded (map_row) */
static MapTab *
map_new(const SiftAggJoin *j, bool filtered, MemoryContext cxt)
{
	MapTab	   *m = palloc0(sizeof(MapTab));

	m->cxt = cxt;
	m->nkey = j->nkey;
	m->ncol = j->ncol;
	m->filtered = filtered;
	m->keep = sift_aggjoin_preserves(j->kind);
	for (int k = 0; k < j->nkey; k++)
		m->ktext[k] = j->keytype[k] == TEXTOID || j->keytype[k] == VARCHAROID;
	return m;
}

/* the hash chains of the rows: the rows of a key are the chain of its slot (a row of the same key is another link: a join repeats a tuple per match) */
static void
map_index_rows(MapTab *m)
{
	m->nslot = 64;
	while (m->nslot < 2 * m->nrow)
		m->nslot *= 2;
	m->head = palloc0(m->nslot * sizeof(uint32));
	m->next = palloc(Max(m->nrow, 1) * sizeof(uint32));
	m->hv = palloc(Max(m->nrow, 1) * sizeof(uint32));
	m->hit = m->keep ? palloc0(Max(m->nrow, 1)) : NULL;
	for (uint32 r = m->nrow; r-- > 0;)
	{
		uint32		h;

		if (m->keep && m->nokey[r])
			continue;
		h = map_hash(m, m->kptr + (size_t) r * m->nkey, m->klen + (size_t) r * m->nkey, m->kint + (size_t) r * m->nkey);
		m->hv[r] = h;
		m->next[r] = m->head[h & (m->nslot - 1)];
		m->head[h & (m->nslot - 1)] = r + 1;
	}
}

/* EXPLAIN ANALYZE: the rows loaded and the time since t0 */
void
map_loaded(AggExec *ex, int nrows, instr_time t0)
{
	instr_time	t1;

	ex->lookup_loaded += nrows;
	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_SUBTRACT(t1, t0);
	ex->lookup_us += INSTR_TIME_GET_MICROSEC(t1);
}

/* the rows of the joined table, visible to the scan's snapshot, hashed by key; from an index when its own conditions name a value of a btree */
static MapTab *
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
	m = map_new(j, own != NULL, cxt);
	if (idx)
	{
		IndexScanDesc iscan = sift_index_beginscan(rel, idx, ex->snap, nkeys);

		index_rescan(iscan, keys, nkeys, NULL, 0);
		while (index_getnext_slot(iscan, ForwardScanDirection, slot))
			map_row(ex, m, slot, j, cb, own);
		index_endscan(iscan);
		index_close(idx, AccessShareLock);
		ex->lookup_index++;
	}
	else if (!none)
	{
		/* the heap tests `column = value` on the page, before it makes a tuple of a row; another access method ignores the keys, the filter holds them too */
		bool		heap = rel->rd_tableam == GetHeapamTableAmRoutine();
		TableScanDesc scan;

		for (int k = 0; heap && sift_agg_lookup_pushdown && k < j->nidx; k++)
			ScanKeyEntryInitialize(&keys[nkeys++], 0, j->idxatt[k], BTEqualStrategyNumber, InvalidOid, j->idxcoll[k], get_opcode(j->idxop[k]), val[k]);
		scan = sift_table_beginscan(rel, ex->snap, nkeys, keys);

		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
			map_row(ex, m, slot, j, cb, own);
		table_endscan(scan);
	}
	map_index_rows(m);
	MemoryContextSwitchTo(old);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, NoLock);
	map_loaded(ex, m->nrow, t0);
	return m;
}

/* the rows of a child plan (a sub-select, VALUES list or function), run once; the rows are the plan's, in the query's snapshot */
static MapTab *
map_load_child(AggExec *ex, int ci)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ExprState  *own = sift_agg_lookup_pushdown && j->fslot >= 0 && !sift_aggjoin_preserves(j->kind) && !slot_reads_leaf(cb, j->fslot) ?
		ex->cs[ci].slot[j->fslot] : NULL;
	PlanState  *child = list_nth(ex->css.custom_ps, j->child);
	MemoryContext caller = MemoryContextSwitchTo(ex->cxt);
	MapTab	   *m = map_new(j, own != NULL, ex->cxt);
	instr_time	t0;

	INSTR_TIME_SET_CURRENT(t0);
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

static void
shared_map_forget(void *arg)
{
	shared_maps = list_delete_ptr(shared_maps, arg);
}

/* what decides the rows of a table: the relation, its key and output columns and the conditions that narrow the load */
static char *
shared_map_key(AggExec *ex, int ci)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	bool		own = sift_agg_lookup_pushdown && j->fslot >= 0 && !slot_reads_leaf(cb, j->fslot);
	StringInfoData k;

	initStringInfo(&k);
	/* the condition reads the table's columns as Params nleaves + i: the same text over another number of leaves is another condition */
	appendStringInfo(&k, "%u %d %d %d %d|", j->relid, own, sift_agg_lookup_pushdown, cb->nleaves, j->nkey);
	for (int i = 0; i < j->nkey; i++)
		appendStringInfo(&k, "%d/%u ", j->keyatt[i], j->keytype[i]);
	appendStringInfo(&k, "|%d|", j->ncol);
	for (int i = 0; i < j->ncol; i++)
		appendStringInfo(&k, "%d/%u ", j->colatt[i], j->coltype[i]);
	if (own)
		appendStringInfo(&k, "|%s", nodeToString(cb->slot[j->fslot].expr));
	for (int i = 0; sift_agg_lookup_pushdown && i < j->nidx; i++)
		appendStringInfo(&k, "|%d/%u/%u/%s", j->idxatt[i], j->idxop[i], j->idxcoll[i], nodeToString(j->idxval[i]));
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
	key = shared_map_key(ex, ci);
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

	if (j->params != NIL)
		return map_new(j, false, ex->cxt);
	if (j->child >= 0)
		return map_load_child(ex, ci);
	return sift_agg_share_lookups && !sift_aggjoin_preserves(j->kind) ? map_load_shared(ex, ci) : map_load_table(ex, ci, ex->cxt);
}

/*
 * A semi or anti join on the integer leaf of the rows (`col IN (SELECT k FROM t)`, EXISTS, NOT EXISTS) that gives no column and keeps no
 * other condition is a test of the leaf against the keys of the table: a byte table over their range, built once, answers it for every
 * tuple, where the generic path makes a cache entry per distinct value of the leaf (a hash of the table's rows each).
 */
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
