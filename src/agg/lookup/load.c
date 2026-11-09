/*
 * agg/lookup/load.c - the load of a joined table into a lookup table
 * The probe of the table, the load of a table or a child, the maps shared between scans,
 * and map_load.
 */
#include "postgres.h"
#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "access/amapi.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "portability/instr_time.h"
#include "storage/predicate.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/lookup/int.h"
#include "agg/init_int.h"

#define LOOKUP_RESERVE_MAX	(1 << 22)	/* rows a map is sized for before the load: a table the statistics put above is grown as it is read */

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
 * The keys the scan itself restricts the join's key leaf to: a top-level `leaf IN (constants)` of the row, which every tuple that reaches the join satisfies, so a
 * table read for these keys alone gives each tuple the rows it would find in the whole table (a NULL element matches nothing). n keys of the integer type *typ in *vals,
 * -1: no such list, or its type is not the table key's (an int4 key against an int8 list is read whole).
 */
static int
map_in_keys(AggExec *ex, int ci, int *att, Oid *typ, Datum **vals)
{
	const SiftAggSpec *s = ex->spec;
	const SiftAggCombo *cb = &s->combo[ci];
	const SiftAggJoin *j = &s->join[cb->join];

	if (j->kind == SJ_RIGHT || j->kind == SJ_FULL || j->params != NIL || j->nkey != 1 || j->keynull[0] || j->tkey[0] != NULL || j->keypos[0] < 0 || cb->level != 0 ||
		j->first.qual != NIL || j->first.nord > 0 || (j->keytype[0] != INT4OID && j->keytype[0] != INT8OID))
		return -1;
	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		MemoryContext old;
		ExprState  *e;
		Datum		d;
		bool		isnull;
		ArrayType  *arr;
		int16		elen;
		bool		ebyval;
		char		ealign;
		Datum	   *elems;
		bool	   *nulls;
		int			ne,
					n = 0;

		if (p->kind != SAP_IN || p->op != 0 || p->level != 0 || p->late || p->expr == NULL || p->leaf != cb->leaf[j->keypos[0]] || p->argtype != j->keytype[0])
			continue;
		old = MemoryContextSwitchTo(ex->cxt);
		e = operand_state(ex, p->expr);
		d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
		MemoryContextSwitchTo(old);
		if (isnull)
			return -1;
		arr = DatumGetArrayTypeP(d);
		get_typlenbyvalalign(p->argtype, &elen, &ebyval, &ealign);
		deconstruct_array(arr, p->argtype, elen, ebyval, ealign, &elems, &nulls, &ne);
		*vals = palloc((ne + 1) * sizeof(Datum));
		for (int k = 0; k < ne; k++)
			if (!nulls[k])
				(*vals)[n++] = elems[k];
		*att = j->keyatt[0];
		*typ = j->keytype[0];
		return n;
	}
	return -1;
}

/*
 * The rows of a table that its keys are few, read through a btree on the column: one probe a key, where the table is read page by page. The keys are those of a dependent
 * lookup (the ids of a request joined to the table of their rows) or the scan's own IN list of the join's key. It pays while the keys are few against the pages
 * (sift_agg_probe_pays). Returns the index opened, with the scan key of the array of keys; NULL: read the table whole.
 */
static bool map_index_covers(Relation idx, const SiftAggJoin *j);

/*
 * The entries a key has in the btree, where the statistics of the table say nothing (never analyzed): the index is asked for a few of the keys of the list, evenly spread,
 * and the entries it holds for them counted (the entries only, no heap row; a key with more than MAP_PILOT_CAP is counted as that many).
 */
#define MAP_PILOT_KEYS	4
#define MAP_PILOT_CAP	256

static double
map_pilot_rows(AggExec *ex, Relation rel, Relation idx, Oid typ, const Datum *vals, int nv)
{
	int			n = Min(nv, MAP_PILOT_KEYS);
	double		entries = 0;
	RegProcedure eq = get_opcode(get_opfamily_member(idx->rd_opfamily[0], typ, typ, BTEqualStrategyNumber));
	IndexScanDesc iscan = sift_index_beginscan(rel, idx, ex->snap, 1);

	for (int i = 0; i < n; i++)
	{
		ScanKeyData sk;
		int			seen = 0;

		ScanKeyEntryInitialize(&sk, 0, 1, BTEqualStrategyNumber, typ, InvalidOid, eq, vals[(int64) i * nv / n]);
		index_rescan(iscan, &sk, 1, NULL, 0);
		while (seen < MAP_PILOT_CAP && index_getnext_tid(iscan, ForwardScanDirection) != NULL)
			seen++;
		entries += seen;
	}
	index_endscan(iscan);
	return n > 0 ? entries / n : 0;
}


static Relation
map_probe(AggExec *ex, int ci, Relation rel, ScanKeyData *key, int *nkeys, const Datum *val, bool own)
{
	const MapTab *dm = NULL;
	const SiftAggJoin *j = &ex->spec->join[ex->spec->combo[ci].join];
	Relation	best = NULL;
	int			nbest = 1;
	Oid			typ = InvalidOid;
	double		nkey,
				rpk,
				tuples = Max(sift_agg_table_rows(RelationGetRelid(rel)), 1.0),
				pages = RelationGetNumberOfBlocks(rel),
				bprice = HUGE_VAL,
				allvis = rel->rd_rel->relallvisible / Max(pages, 1.0);
	bool		heap = rel->rd_rel->relam == HEAP_TABLE_AM_OID,
				bcov = false;
	int			att = 0,
				cd = map_keyed_dependent(ex, ci, &att, &typ),
				nv = 0;
	Datum	   *vals = NULL;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			nnum = 0;
	bool		cols;

	if (cd >= 0)
		dm = ex->cs[cd].map;
	/* the equalities of the rest of the ON clause are tested by the table's own filter on each row read */
	else if ((j->nidx > 0 && !own) || (nv = map_in_keys(ex, ci, &att, &typ, &vals)) < 0)
		return NULL;
	/* the index whose entries cost least for a key (join_keys_pay) */
	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		ScanKeyData ik[SIFT_AGG_MAX_JIDX];
		int			ta[SIFT_AGG_MAX_JIDX],
					n = 1;
		bool		cov;
		double		r,
					price;

		if (!(idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index->indisvalid && heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			  idx->rd_index->indkey.values[0] == att && idx->rd_opcintype[0] == typ))
		{
			index_close(idx, AccessShareLock);
			continue;
		}
		/* the table's `column = value` conditions on the columns that follow the key in the index are keys of the scan too: its entries are those that can match, no heap row is read for the others */
		for (; dm == NULL && n < idx->rd_index->indnkeyatts && n < SIFT_AGG_MAX_JIDX; n++)
		{
			int			k = sift_agg_trail_cond(j, idx->rd_index->indkey.values[n], idx->rd_opfamily[n], idx->rd_indcollation[n]);
			int			strategy;
			Oid			lt,
						rt;

			if (k < 0)
				break;
			get_op_opfamily_properties(j->idxop[k], idx->rd_opfamily[n], false, &strategy, &lt, &rt);
			ScanKeyEntryInitialize(&ik[n], 0, n + 1, strategy, rt, j->idxcoll[k], get_opcode(j->idxop[k]), val[k]);
			ta[n - 1] = idx->rd_index->indkey.values[n];
		}
		cov = map_index_covers(idx, j);
		r = sift_agg_key_rows(RelationGetRelid(rel), att, ta, n - 1, tuples, idx->rd_index->indisunique && idx->rd_index->indnkeyatts == 1);
		if (r <= 0 && dm == NULL && nv > 0 && n == 1)
			r = map_pilot_rows(ex, rel, idx, typ, vals, nv);
		price = r * sift_agg_entry_ops(cov ? allvis : 0, heap);
		if (r > 0 && price < bprice)
		{
			if (best)
				index_close(best, AccessShareLock);
			best = idx;
			nbest = n;
			bprice = price;
			rpk = r;
			bcov = cov;
			memcpy(&key[1], &ik[1], (n - 1) * sizeof(ScanKeyData));
		}
		else
			index_close(idx, AccessShareLock);
	}
	if (best == NULL)
		return NULL;
	nkey = Min(dm ? dm->kd[0].n : nv, tuples);
	cols = map_load_cols_possible(rel, j, &nnum);
	if (!sift_agg_probe_pays(nkey, nkey * rpk, pages, bcov ? allvis : 0, heap, sift_agg_whole_ops(tuples, pages, cols, nnum, !cols && lookup_keys_only(ex->spec, ex->lay, ci, ex->nbase))))
	{
		index_close(best, AccessShareLock);
		return NULL;
	}
	if (dm)
	{
		vals = palloc((dm->kd[0].n + 1) * sizeof(Datum));
		for (uint32 i = 0; i < dm->kd[0].n; i++)
		{
			int64		v = dm->kd[0].ival[i];

			if (typ == INT4OID && (v < PG_INT32_MIN || v > PG_INT32_MAX))
				continue;		/* no int4 value equals it */
			vals[nv++] = typ == INT4OID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
		}
	}
	get_typlenbyvalalign(typ, &typlen, &typbyval, &typalign);
	ScanKeyEntryInitialize(key, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, typ, InvalidOid,
						   get_opcode(get_opfamily_member(best->rd_opfamily[0], typ, typ, BTEqualStrategyNumber)),
						   PointerGetDatum(construct_array(vals, nv, typ, typlen, typbyval, typalign)));
	pfree(vals);
	*nkeys = nbest;
	return best;
}

/*
 * Does the btree hold, and return, every column the load reads (the key and the columns of the join, an INCLUDE column too)? Then the rows on the pages the visibility map
 * calls all-visible come from the entries, with no heap fetch.
 */
static bool
map_index_covers(Relation idx, const SiftAggJoin *j)
{
	int			natts = idx->rd_index->indnatts;
	int			need[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS],
				nneed = 0;

	if (idx->rd_indam->amcanreturn == NULL)
		return false;
	for (int k = 0; k < j->nkey; k++)
	{
		if (j->tkey[k])
			return false;
		need[nneed++] = j->keyatt[k];
	}
	for (int c = 0; c < j->ncol; c++)
	{
		if (j->colexpr[c])
			return false;
		need[nneed++] = j->colatt[c];
	}
	for (int n = 0; n < nneed; n++)
	{
		int			i;

		for (i = 0; i < natts; i++)
			if (idx->rd_index->indkey.values[i] == need[n] && idx->rd_indam->amcanreturn(idx, i + 1))
				break;
		if (i == natts)
			return false;
	}
	return true;
}

/* the entries of the scan into the table, a row fetched from the heap only where its page is not all-visible (what an Index Only Scan does) */
static void
map_scan_entries(AggExec *ex, MapTab *m, Relation rel, Relation idx, IndexScanDesc iscan, TupleTableSlot *slot, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own)
{
	TupleTableSlot *vs = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);
	Buffer		vmbuf = InvalidBuffer;
	int			natts = RelationGetDescr(rel)->natts;
	Datum		ivals[INDEX_MAX_KEYS];
	bool		inulls[INDEX_MAX_KEYS];

	for (ItemPointer tid; (tid = index_getnext_tid(iscan, ForwardScanDirection)) != NULL;)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(tid);

		if (!VM_ALL_VISIBLE(rel, blk, &vmbuf))
		{
			ex->lookup_heap++;
			if (index_fetch_heap(iscan, slot))
				map_row(ex, m, slot, j, cb, own);
			continue;
		}
		PredicateLockPage(rel, blk, ex->snap);
		index_deform_tuple(iscan->xs_itup, iscan->xs_itupdesc, ivals, inulls);
		ExecClearTuple(vs);
		memset(vs->tts_isnull, true, natts * sizeof(bool));
		for (int i = 0; i < idx->rd_index->indnatts; i++)
		{
			int			a = idx->rd_index->indkey.values[i];

			if (a > 0)
			{
				vs->tts_values[a - 1] = ivals[i];
				vs->tts_isnull[a - 1] = inulls[i];
			}
		}
		ExecStoreVirtualTuple(vs);
		map_row(ex, m, vs, j, cb, own);
	}
	if (vmbuf != InvalidBuffer)
		ReleaseBuffer(vmbuf);
	ExecDropSingleTupleTableSlot(vs);
}

/* a table the run reads: its pages and the share the visibility map holds all-visible, for the Lookup line of EXPLAIN */
void
map_note_table(AggExec *ex, Relation rel)
{
	BlockNumber pages = RelationGetNumberOfBlocks(rel);

	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		return;
	ex->lookup_pages += pages;
	ex->lookup_allvis += Min((BlockNumber) rel->rd_rel->relallvisible, pages);
}

/*
 * Is the join of combo ci one whose table can be kept as classes of its keys (map_load_dense)? An inner or left join by one int4 leaf, nothing else in its
 * ON clause, and expressions that read the columns alone (not the key itself): then the output rows of a key are those of its class. No other lookup may be keyed by this
 * one's rows (they are no longer one for each key); an expression of its columns in a combo of its own is.
 */
bool
sift_agg_dense_join(const SiftAggSpec *s, int ci, const SiftSrcLayout *lay)
{
	const SiftAggCombo *cb = &s->combo[ci];
	const SiftAggJoin *j = &s->join[cb->join];

	if (!sift_agg_lookup_columns || (j->kind != SJ_INNER && j->kind != SJ_LEFT) || j->nkey != 1 || j->keytype[0] != INT4OID || j->keynull[0] ||
		j->keypos[0] < 0 || j->keyexpr[0] != NULL || j->tkey[0] != NULL || j->fslot >= 0 || j->nidx > 0 || j->params != NIL || j->child >= 0 ||
		j->first.qual != NIL || j->first.hybrid || j->first.nord > 0 || cb->nleaves != 1 || cb->leaf[0] < 0 || cb->level != 0)
		return false;
	for (int sl = 0; sl < cb->nslots; sl++)
		if (slot_reads_leaf(cb, sl))
			return false;
	/* the key as a column would be that of the first key of the class */
	for (int c = 0; c < j->ncol; c++)
		if (j->colexpr[c] == NULL && j->colatt[c] == j->keyatt[0])
			return false;
	for (int cd = 0; cd < s->ncombo; cd++)
		for (int p = 0; s->combo[cd].join >= 0 && p < s->combo[cd].nleaves; p++)
			if (s->combo[cd].leaf[p] < 0 && s->combo[cd].refcombo[p] == ci)
				return false;
	return lay->leaf[cb->leaf[0]].vtype == SCV_I32;
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
	map_note_table(ex, rel);
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
	if (idx == NULL && !none && sift_agg_lookup_pushdown)
		idx = map_probe(ex, ci, rel, keys, &nkeys, val, own != NULL);
	if (idx == NULL && !none && own == NULL && (m = lookdim_load(ex, ci, rel, cxt)) != NULL)
	{
		MemoryContextSwitchTo(old);
		ExecDropSingleTupleTableSlot(slot);
		table_close(rel, NoLock);
		map_loaded(ex, 0, t0);
		return m;
	}
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
			lj.colatt[c] = lj.colexpr[c] ? 0 : kept.cols.att[c];
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
	for (int c = 0; c < lj.ncol; c++)
		if (lj.colexpr[c])
		{
			m->kcx = m->kcx ? m->kcx : CreateStandaloneExprContext();
			m->cx[c] = ExecInitExpr((Expr *) lj.colexpr[c], NULL);
		}
	MemoryContextSwitchTo(kept.cx);
	/* a table read whole (no condition of its own, no index) fills the map to about its rows */
	if (idx == NULL && own == NULL && rel->rd_rel->reltuples > 0)
		map_reserve(m, (uint32) Min(rel->rd_rel->reltuples, LOOKUP_RESERVE_MAX));
	if (idx)
	{
		IndexScanDesc iscan = sift_index_beginscan(rel, idx, ex->snap, nkeys);

		/* the entries are enough where the index holds the columns; the key array makes no difference to the entries returned */
		bool		entries = sift_agg_lookup_pushdown && map_index_covers(idx, &lj);

		iscan->xs_want_itup = entries;
		index_rescan(iscan, keys, nkeys, NULL, 0);
		if (entries)
			map_scan_entries(ex, m, rel, idx, iscan, slot, &lj, cb, own);
		else
		{
			while (index_getnext_slot(iscan, ForwardScanDirection, slot))
				map_row(ex, m, slot, &lj, cb, own);
			ex->lookup_hscan += m->nrow;
		}
		index_endscan(iscan);
		index_close(idx, AccessShareLock);
		ex->lookup_index++;
	}
	else if (!none && !(own == NULL && !m->lazy && sift_agg_dense_join(ex->spec, ci, ex->lay) && map_load_dense(ex, m, &lj, cb, rel)) &&
			 !map_load_cols(ex, m, &lj, cb, own, rel))
	{
		/* the heap tests `column = value` on the page, before it makes a tuple of a row; another access method ignores the keys, the filter holds them too */
		bool		heap = rel->rd_tableam == GetHeapamTableAmRoutine();

		TableScanDesc scan;

		if (ex->lookup_why == NULL && heap && own == NULL)
		{
			char	   *why = map_cols_why(rel, j);

			ex->lookup_why = MemoryContextStrdup(ex->cxt, why ? why : "");
			if (why)
				elog(DEBUG1, "sift: lookup table \"%s\" is read from the heap: %s", RelationGetRelationName(rel), why);
		}

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
		ex->lookup_hscan += m->nrow;
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
		memset(m->cx, 0, sizeof(m->cx));
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

	return semi_lut_possible(s, ci, nbase) && semi_lut_leaf(lay, cb->leaf[0]) && j->params == NIL && j->child < 0 && j->first.qual == NIL && !j->first.hybrid &&
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
	/* the columns that are expressions are named by their place: the key says which they are */
	for (int i = 0; i < j->ncol; i++)
		if (j->colexpr[i])
			appendStringInfo(&k, "|expr %s", nodeToString(j->colexpr[i]));
	if (own)
		appendStringInfo(&k, "|%s", nodeToString(cb->slot[j->fslot].expr));
	if (lookup_keys_only(ex->spec, ex->lay, ci, ex->nbase))
		appendStringInfoString(&k, "|keys");
	/* the rows of a dense table are the classes of its keys: only an arm that takes it that way can read them */
	if (sift_agg_dense_join(ex->spec, ci, ex->lay))
		appendStringInfoString(&k, "|dense");
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
	Datum	   *keys;

	if (ex->pmaps && ex->pmaps[ci])
		return map_share_attach(ex->pmaps[ci], j, ex->cxt);
	if (j->params != NIL)
		return map_new(j, false, ex->cxt);
	if (j->child >= 0)
		return map_load_child(ex, ci);
	/* a table read for the keys of this scan alone is no other arm's table: it is not shared */
	return sift_agg_share_lookups && !sift_aggjoin_preserves(j->kind) && !(sift_agg_lookup_pushdown && (map_keyed_dependent(ex, ci, &att, &typ) >= 0 || map_in_keys(ex, ci, &att, &typ, &keys) >= 0)) ?
		map_load_shared(ex, ci) : map_load_table(ex, ci, ex->cxt);
}

/* does an expression read one of the first n parameters (the leaves of a combo)? */
SIFT_WALKER(reads_leaf_param, int *, nleaves,
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid < *nleaves)
		return true;)

/* does the expression of a slot of the combo read one of its leaves? */
bool
slot_reads_leaf(const SiftAggCombo *cb, int sl)
{
	int			nleaves = cb->nleaves;

	return reads_leaf_param((Node *) cb->slot[sl].expr, &nleaves);
}
