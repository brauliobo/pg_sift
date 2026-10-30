/*
 * col/amscan/amscan.c - the index access method scan of a sift_col index
 * begin, rescan, end, the bitmap and the tuple scans
 * and the batches a tuple scan returns
 */
#include "postgres.h"
#include <math.h>
#include "access/table.h"
#include "utils/datum.h"
#include "pgstat.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/uuid.h"
#include "col/am.h"
#include "col/amscan/amscan_int.h"

/*
 * One scan of the index, as a bitmap (amgetbitmap) or tuple by tuple (amgettuple): the source, the predicates of the keys and, for amgettuple, the rows of
 * the unit being returned. Everything but the heap relation lives in cxt, which a rescan resets.
 */
#define COL_LAZY_FIRST	16

#define COL_LAZY_MAX	512

typedef struct ColScanOpaque
{
	MemoryContext cxt;
	MemoryContext batch;		/* the values of the unit being returned */
	Relation	heap;
	SiftColSource *src;
	SiftColPred *preds;
	int			npreds;
	bool		started;
	bool		unsat;			/* a key no row can satisfy */
	bool		recheck;
	int		   *first;			/* first[attno]: the leaf of attribute attno */
	int			natts;
	SiftColDef *defs;
	uint32		n,				/* the rows of the unit */
				pos,
				cap;
	ItemPointerData *tids;
	ItemPointerData *dtids;		/* the rows a point directory of heap addresses lists, returned before the units' (no unit read for them) */
	uint32		ndtids;
	ItemPointerData *ltids;		/* the addresses of the lookup handed over as the scan asks (SIFT_COLO_POINT_LAZY), nlazy of them in the last chunk; NULL once all are */
	uint32		nlazy;
	Datum	   *vals;			/* n * natts, only when the executor wants index tuples */
	bool	   *nulls;
	IndexTuple	itup;
	bool		project;		/* an index-only scan forms its tuples from the columns of want (the attribute numbers of the index the plan reads) only (sift_col_ioss_want) */
	Bitmapset  *want;
	IndexFetchTableData *fetch;	/* the heap, for a string the index could not keep (an overflow value) */
	TupleTableSlot *slot;
} ColScanOpaque;

/*
 * An index-only scan of a sift_col index forms its index tuples from the columns the plan node reads, the others NULL (a column it never reads is not decoded,
 * converted or formed: for `select id` over 7 columns that was most of the scan's instructions). The node reads what its target list, its quals and its recheck
 * quals reference (INDEX_VAR); the index conditions are the access method's. The scan descriptor is made by the node's first call, so after ExecutorStart
 * (sift_col_ioss_want) the node's function is replaced by one that, on that first call, tells ambeginscan which columns this scan is for, and puts the node's own
 * function back. A scan that begins without the entry (a plain index scan, a node the hook did not see) reads every column, as before.
 */
typedef struct IossWant
{
	PlanState  *ps;
	Bitmapset  *want;
	bool		project;
	bool		taken;
} IossWant;

static List *ioss_pending;		/* IossWant of the nodes that have not run yet, in TopMemoryContext; emptied at the end of the transaction */

static IossWant *ioss_cur;		/* the entry of the node whose first call is running */

static ExecProcNodeMtd ioss_real;	/* ExecIndexOnlyScan, the same for every node */

static bool ioss_xact_registered;

static void
ioss_free_pending(void)
{
	foreach_ptr(IossWant, w, ioss_pending)
	{
		bms_free(w->want);
		pfree(w);
	}
	list_free(ioss_pending);
	ioss_pending = NIL;
}

static void
ioss_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT)
	{
		ioss_cur = NULL;
		ioss_free_pending();
	}
}

static TupleTableSlot *
ioss_first(PlanState *ps)
{
	IossWant   *mine = NULL;
	IossWant   *outer = ioss_cur;
	TupleTableSlot *slot;

	foreach_ptr(IossWant, w, ioss_pending)
		if (w->ps == ps)
			mine = w;
	if (ps->ExecProcNode == ioss_first)
		ps->ExecProcNode = ioss_real;
	ps->ExecProcNodeReal = ioss_real;
	if (mine == NULL)
		return ioss_real(ps);
	ioss_pending = list_delete_ptr(ioss_pending, mine);
	ioss_cur = mine;
	PG_TRY();
	{
		slot = ioss_real(ps);
	}
	PG_FINALLY();
	{
		ioss_cur = outer;
		bms_free(mine->want);
		pfree(mine);
	}
	PG_END_TRY();
	return slot;
}

static bool
ioss_walk(PlanState *ps, void *context)
{
	if (IsA(ps, IndexOnlyScanState))
	{
		IndexOnlyScanState *is = (IndexOnlyScanState *) ps;
		IndexOnlyScan *plan = (IndexOnlyScan *) ps->plan;

		if (is->ioss_ScanDesc == NULL && is->ioss_RelationDesc->rd_rel->relam == sift_col_am_oid() && (ioss_real == NULL || ioss_real == ps->ExecProcNodeReal))
		{
			Bitmapset  *attrs = NULL;
			IossWant   *w;
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
			int			x = -1;

			if (!ioss_xact_registered)
			{
				RegisterXactCallback(ioss_xact_callback, NULL);
				ioss_xact_registered = true;
			}
			w = palloc0(sizeof(IossWant));
			w->ps = ps;
			w->project = true;
			pull_varattnos((Node *) plan->scan.plan.targetlist, INDEX_VAR, &attrs);
			pull_varattnos((Node *) plan->scan.plan.qual, INDEX_VAR, &attrs);
			pull_varattnos((Node *) plan->recheckqual, INDEX_VAR, &attrs);
			while ((x = bms_next_member(attrs, x)) >= 0)
			{
				AttrNumber	a = x + FirstLowInvalidHeapAttributeNumber;

				/* a whole-row reference or a system column of the index tuple: every column */
				w->project &= a > 0;
				w->want = bms_add_member(w->want, a);
			}
			ioss_pending = lappend(ioss_pending, w);
			MemoryContextSwitchTo(old);
			bms_free(attrs);
			ioss_real = ps->ExecProcNodeReal;
			ps->ExecProcNodeReal = ioss_first;
		}
	}
	return planstate_tree_walker(ps, ioss_walk, context);
}

void
sift_col_ioss_want(QueryDesc *queryDesc)
{
	(void) ioss_walk(queryDesc->planstate, NULL);
}

IndexScanDesc
sift_col_ambeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	ColScanOpaque *so = palloc0(sizeof(ColScanOpaque));

	so->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col scan", ALLOCSET_DEFAULT_SIZES);
	so->batch = AllocSetContextCreate(so->cxt, "sift_col scan batch", ALLOCSET_DEFAULT_SIZES);
	scan->opaque = so;
	scan->xs_itupdesc = RelationGetDescr(index);
	if (ioss_cur != NULL && !ioss_cur->taken && ((IndexOnlyScanState *) ioss_cur->ps)->ioss_RelationDesc == index)
	{
		ioss_cur->taken = true;
		so->project = ioss_cur->project;
		so->want = bms_copy(ioss_cur->want);
	}
	return scan;
}

static void
col_fetch_end(ColScanOpaque *so)
{
	if (so->slot)
		ExecDropSingleTupleTableSlot(so->slot);
	if (so->fetch)
		table_index_fetch_end(so->fetch);
	so->slot = NULL;
	so->fetch = NULL;
}

static void
col_scan_end(ColScanOpaque *so)
{
	col_fetch_end(so);
	if (so->src)
		sift_col_close(so->src);
	MemoryContextReset(so->cxt);
	so->batch = AllocSetContextCreate(so->cxt, "sift_col scan batch", ALLOCSET_DEFAULT_SIZES);
	so->src = NULL;
	so->started = so->unsat = so->recheck = false;
	so->n = so->pos = so->cap = 0;
	so->tids = so->dtids = NULL;
	so->ndtids = 0;
	so->ltids = NULL;
	so->nlazy = 0;
	so->vals = NULL;
	so->nulls = NULL;
	so->itup = NULL;
}

void
sift_col_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	col_scan_end(scan->opaque);
	if (keys && scan->numberOfKeys > 0)
		memcpy(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
}

void
sift_col_amendscan(IndexScanDesc scan)
{
	ColScanOpaque *so = scan->opaque;

	col_fetch_end(so);
	if (so->src)
		sift_col_close(so->src);
	if (so->heap)
		table_close(so->heap, NoLock);
	MemoryContextDelete(so->cxt);
}

/* Opens the source and makes the predicates of the keys; the executor checked the privileges and the row security of the table, and the planner only offers operators it knows leak nothing */
static void
col_scan_begin(IndexScanDesc scan, bool lazy)
{
	ColScanOpaque *so = scan->opaque;
	MemoryContext old = MemoryContextSwitchTo(so->cxt);
	/* check-memory: allow the scan copies what it keeps before it opens the heap */
	SiftColRel *r = sift_col_rel(scan->indexRelation);
	ColKeys		keys;
	const SiftKey *tids;
	bool		sat;
	int			wl = -1;

	so->first = palloc((r->natts + 1) * sizeof(int));
	memcpy(so->first, r->first, (r->natts + 1) * sizeof(int));
	so->natts = r->natts;
	so->defs = palloc(r->meta.ncols * sizeof(SiftColDef));
	memcpy(so->defs, r->meta.cols, r->meta.ncols * sizeof(SiftColDef));
	so->preds = palloc(Max(scan->numberOfKeys, 1) * sizeof(SiftColPred));
	keys = (ColKeys) {.defs = so->defs, .first = so->first, .idesc = RelationGetDescr(scan->indexRelation)};
	if (so->heap == NULL)
		so->heap = table_open(scan->indexRelation->rd_index->indrelid, AccessShareLock);
	sat = scan_preds(scan, &keys, so->preds);
	/*
	 * an equality or IN on a plain leaf finds the rows of the write buffers in their records, which costs a few instructions a row where their batches cost thousands:
	 * the one with the fewest values (of an IN list of the scope and an equality on the same key, the equality), since the heap checks the rows it finds against all
	 */
	for (int i = 0; sat && !scan->xs_want_itup && i < scan->numberOfKeys; i++)
		if (sift_col_wbuf_lookup_ok(&so->defs[so->preds[i].col], &so->preds[i]) && (wl < 0 || so->preds[i].nvals < so->preds[wl].nvals))
			wl = i;
	so->src = sift_col_open(so->heap, scan->indexRelation, scan->xs_snapshot, so->cxt,
							SIFT_COLO_NOCHECKS | SIFT_COLO_POINTS | (wl >= 0 ? SIFT_COLO_NOWBUF : 0) | (scan->xs_want_itup ? 0 : SIFT_COLO_POINT_TIDS | (lazy ? SIFT_COLO_POINT_LAZY : 0)));
	if (sat)
	{
		ItemPointerData *wt = NULL;
		uint32		nw = 0;

		/* before the begin: a lookup that finds too many rows has the source load the buffers (sift_col_wbuf_lookup) */
		if (wl >= 0)
			wt = sift_col_wbuf_lookup(so->src, &so->preds[wl], so->cxt, &nw);
		so->npreds = scan->numberOfKeys;
		sift_col_begin(so->src, so->preds, so->npreds, 0, PG_UINT32_MAX);
		/* the addresses a directory of heap addresses lists are the fingerprints': the heap checks every condition again */
		if ((tids = sift_col_point_tids(so->src, &so->ndtids)) != NULL)
		{
			so->dtids = palloc(so->ndtids * sizeof(ItemPointerData));
			for (uint32 i = 0; i < so->ndtids; i++)
				sift_key_to_tid(tids[i], &so->dtids[i]);
			keys.recheck = true;
		}
		if (wt != NULL)
		{
			/* the rows of the buffer are the record's value of one condition: the heap checks them all */
			so->dtids = so->dtids ? repalloc(so->dtids, (so->ndtids + nw) * sizeof(ItemPointerData)) : palloc(nw * sizeof(ItemPointerData));
			memcpy(so->dtids + so->ndtids, wt, nw * sizeof(ItemPointerData));
			so->ndtids += nw;
			keys.recheck = true;
		}
		if (sift_col_point_lazy(so->src))
		{
			so->ltids = palloc(COL_LAZY_MAX * sizeof(ItemPointerData));
			keys.recheck = true;
		}
	}
	else
		so->unsat = true;
	so->recheck = keys.recheck;
	so->started = true;
	MemoryContextSwitchTo(old);
	/* pg_stat_*_indexes.idx_scan and the Index Searches of EXPLAIN (the access method counts them) */
	pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
	if (scan->instrument)
		scan->instrument->nsearches++;
#endif
}

/* The next unit with a row that passes the predicates, and those rows */
static bool
col_scan_unit(ColScanOpaque *so, SiftColUnit *unit, SiftColSel *sel)
{
	bool		used;

	if (so->unsat)
		return false;
	while (sift_col_next(so->src, unit))
	{
		CHECK_FOR_INTERRUPTS();
		sift_col_sel_all(unit, sel);
		/* the rows of a point directory are the fingerprint's: the heap checks the condition again */
		if (!sift_col_filter_points(unit, so->preds, so->npreds, sel, &used) || sift_col_sel_count(sel) == 0)
			continue;
		so->recheck |= used;
		return true;
	}
	return false;
}

int64
sift_col_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	ColScanOpaque *so = scan->opaque;
	MemoryContext old = MemoryContextSwitchTo(so->cxt);
	SiftColUnit unit;
	SiftColSel	sel;
	int64		ntids = 0;

	col_scan_begin(scan, false);
	if (so->ndtids > 0)
	{
		tbm_add_tuples(tbm, so->dtids, so->ndtids, true);
		ntids += so->ndtids;
	}
	while (col_scan_unit(so, &unit, &sel))
	{
		if (sel.count > so->cap)
		{
			so->cap = Max(sel.count, 2 * so->cap);
			so->tids = so->tids ? repalloc(so->tids, so->cap * sizeof(ItemPointerData)) : palloc(so->cap * sizeof(ItemPointerData));
		}
		sift_col_tids(&unit, &sel, so->tids);
		/* the stored values are the heap's (HOT keeps the indexed columns), and a row the snapshot cannot see is dropped by the heap scan: no recheck but for a key the leaf cannot test */
		tbm_add_tuples(tbm, so->tids, sel.count, so->recheck);
		ntids += sel.count;
	}
	MemoryContextSwitchTo(old);
	col_scan_end(so);
	return ntids;
}

/* ---- tuple by tuple: an index scan and an index-only scan, in the order of the units (no order to give) ---- */

/*
 * Can the leaf of attribute attno give its value back: the integer-like and float leaves, and the text, varchar and uuid columns the option
 * return_columns names. The executor asks for every returnable column of the index whether the query needs it or not, so a string column is
 * read, one string at a time out of the dictionary of the segment, for every row returned: the option leaves the choice to the index.
 */
bool
sift_col_returns_str(TupleDesc desc, int a, int nleaves, bool json)
{
	Oid			t = TupleDescAttr(desc, a - 1)->atttypid;

	return !json && nleaves == 1 && (t == TEXTOID || t == VARCHAROID || t == UUIDOID);
}

bool
sift_col_amcanreturn(Relation index, int attno)
{
	/* check-memory: allow reads the definitions only, no call that locks */
	SiftColRel *r = sift_col_rel(index);
	const SiftColDef *d = &r->meta.cols[r->first[attno]];

	if (r->nleaves[attno] != 1 || r->name[attno] || (d->path && d->path[0]) || (d->flags & SIFT_COLD_LOSSY))
		return false;
	switch (d->type)
	{
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
		case SIFT_COLT_BOOL:
		case SIFT_COLT_DATE:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
		case SIFT_COLT_FLOAT4:
		case SIFT_COLT_FLOAT8:
			return true;
		case SIFT_COLT_NUMERIC:
			return d->store == SIFT_COLS_INT;
		case SIFT_COLT_TEXT:
		case SIFT_COLT_UUID:
			return r->ret[attno] && d->store == SIFT_COLS_STR;
		default:
			return false;
	}
}

static Datum
str_datum(const SiftColDef *d, const SiftColVec *v, uint32 k)
{
	const char *s = v->vbytes + v->voff[k];
	uint32		len = (uint32) (v->voff[k + 1] - v->voff[k]);

	if (d->type == SIFT_COLT_UUID)
	{
		pg_uuid_t  *u = palloc(UUID_LEN);

		SIFT_INVARIANT(len == UUID_LEN, "sift_col: the uuid of a dictionary holds %u bytes", len);
		memcpy(u, s, UUID_LEN);
		return UUIDPGetDatum(u);
	}
	return PointerGetDatum(cstring_to_text_with_len(s, len));
}

static Datum
vec_datum(const SiftColDef *d, const SiftColVec *v, uint32 k)
{
	switch (d->type)
	{
		case SIFT_COLT_INT2:
			return Int16GetDatum((int16) v->i[k]);
		case SIFT_COLT_INT4:
		case SIFT_COLT_DATE:
			return Int32GetDatum((int32) v->i[k]);
		case SIFT_COLT_INT8:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			return Int64GetDatum(v->i[k]);
		case SIFT_COLT_BOOL:
			return BoolGetDatum(v->i[k] != 0);
		case SIFT_COLT_FLOAT4:
			return Float4GetDatum((float4) v->f[k]);
		case SIFT_COLT_FLOAT8:
			return Float8GetDatum(v->f[k]);
		case SIFT_COLT_NUMERIC:
			return sift_col_key_numeric(d->scale, v->i[k]);
		default:
			SIFT_INVARIANT(false, "sift_col: no value of type %d to return", d->type);
			return (Datum) 0;
	}
}

/*
 * A string the index could not keep (the strings of a leaf in a unit passed SIFT_COL_UNIT_MAX_BYTES, so the value is stored as overflow and read from
 * the heap), read from the heap: the version of the row's chain that the snapshot sees, whose indexed columns are those of the entry. None visible:
 * NULL, and the executor drops the tuple as it would after any index scan.
 */
static Datum
heap_str(IndexScanDesc scan, ColScanOpaque *so, int attno, ItemPointer tid, bool *isnull)
{
	bool		call_again = false,
				all_dead = false;
	Form_pg_attribute att = TupleDescAttr(scan->xs_itupdesc, attno - 1);
	AttrNumber	hattr = scan->indexRelation->rd_index->indkey.values[attno - 1];
	Datum		v;

	if (so->fetch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(so->cxt);

		so->fetch = sift_table_index_fetch_begin(so->heap);
		so->slot = table_slot_create(so->heap, NULL);
		MemoryContextSwitchTo(old);
	}
	if (!table_index_fetch_tuple(so->fetch, tid, scan->xs_snapshot, so->slot, &call_again, &all_dead))
	{
		*isnull = true;
		return (Datum) 0;
	}
	v = slot_getattr(so->slot, hattr, isnull);
	if (!*isnull)
		v = datumCopy(v, att->attbyval, att->attlen);
	ExecClearTuple(so->slot);
	return v;
}

/* The rows of the next unit that passes the predicates, with the values of the returnable columns when the executor wants index tuples */
static bool
col_fill_batch(IndexScanDesc scan)
{
	ColScanOpaque *so = scan->opaque;
	SiftColUnit unit;
	SiftColSel	sel;
	MemoryContext old;

	if (!col_scan_unit(so, &unit, &sel))
		return false;
	MemoryContextReset(so->batch);
	old = MemoryContextSwitchTo(so->batch);
	so->n = sel.count;
	so->pos = 0;
	so->tids = palloc(so->n * sizeof(ItemPointerData));
	sift_col_tids(&unit, &sel, so->tids);
	if (scan->xs_want_itup)
	{
		so->vals = palloc0(so->n * so->natts * sizeof(Datum));
		so->nulls = palloc(so->n * so->natts * sizeof(bool));
		memset(so->nulls, true, so->n * so->natts * sizeof(bool));
		for (int a = 1; a <= so->natts; a++)
		{
			SiftColVec	vec;
			bool		isstr;

			if (!sift_col_amcanreturn(scan->indexRelation, a) || (so->project && !bms_is_member(a, so->want)))
				continue;
			isstr = so->defs[so->first[a]].store == SIFT_COLS_STR;
			if (isstr)
				sift_col_read_text(&unit, so->first[a], &sel, &vec);
			else
				sift_col_read(&unit, so->first[a], &sel, &vec);
			for (uint32 k = 0; k < so->n; k++)
			{
				bool		isnull = vec.nulls && (vec.nulls[k >> 6] >> (k & 63) & 1),
							ovf = vec.ovf && (vec.ovf[k >> 6] >> (k & 63) & 1);

				so->nulls[k * so->natts + a - 1] = isnull && !ovf;
				if (ovf && isstr)
				{
					bool		hnull;

					so->vals[k * so->natts + a - 1] = heap_str(scan, so, a, &so->tids[k], &hnull);
					so->nulls[k * so->natts + a - 1] = hnull;
				}
				else if (ovf)
				{
					/* the only value a returnable column cannot hold is the NaN of a numeric(p <= 18, s), stored as a NULL with the overflow flag */
					SIFT_INVARIANT(so->defs[so->first[a]].type == SIFT_COLT_NUMERIC,
								   "sift_col: a value of column %d overflowed the index and cannot be returned from it", a);
					so->vals[k * so->natts + a - 1] = DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
				}
				else if (!isnull)
					so->vals[k * so->natts + a - 1] = isstr ? str_datum(&so->defs[so->first[a]], &vec, k) : vec_datum(&so->defs[so->first[a]], &vec, k);
			}
		}
	}
	MemoryContextSwitchTo(old);
	return true;
}

bool
sift_col_amgettuple(IndexScanDesc scan, ScanDirection dir)
{
	ColScanOpaque *so = scan->opaque;

	if (!so->started)
	{
		col_scan_begin(scan, true);
		so->tids = so->dtids;
		so->n = so->ndtids;
	}
	while (so->pos >= so->n)
	{
		if (so->ltids)
		{
			SiftKey		tids[COL_LAZY_MAX];

			/* a scan that stops early reads a few addresses: the chunks double up to the size that keeps the cost of a call small */
			so->n = sift_col_point_next(so->src, tids, Min(COL_LAZY_MAX, Max(COL_LAZY_FIRST, so->nlazy * 2)));
			so->pos = 0;
			if (so->n > 0)
			{
				so->nlazy = so->n;
				for (uint32 i = 0; i < so->n; i++)
					sift_key_to_tid(tids[i], &so->ltids[i]);
				so->tids = so->ltids;
				continue;
			}
			so->ltids = NULL;
		}
		if (!col_fill_batch(scan))
			return false;
	}
	scan->xs_heaptid = so->tids[so->pos];
	scan->xs_recheck = so->recheck;
	if (scan->xs_want_itup)
	{
		if (so->itup)
			pfree(so->itup);
		so->itup = index_form_tuple(scan->xs_itupdesc, so->vals + so->pos * so->natts, so->nulls + so->pos * so->natts);
		so->itup->t_tid = scan->xs_heaptid;
		scan->xs_itup = so->itup;
	}
	so->pos++;
	return true;
}
