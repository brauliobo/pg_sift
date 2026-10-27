/*
 * col/amscan.c - the bitmap scan of a sift_col index (docs/COL-SCAN.md): the keys of the operator classes sift.col_*_ops become
 * predicates of the column source, the TIDs of the rows that pass go to the TID bitmap, and the heap scan above applies MVCC.
 * No amgettuple: an index scan has no order to give (units are in TID order), ordered reads stay with the top-N of SiftAgg.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/itup.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "utils/datum.h"
#include "mb/pg_wchar.h"
#include "pgstat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/uuid.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/gorder.h"
#include "col/point.h"
#include "core/cmp.h"

/*
 * The cost of a scan, in microseconds of an ordinary run (cost units are about that on the default settings: a cached b-tree lookup of 8 units takes 6-8 us and a
 * bitmap heap scan of 1,700 units about 1.7 ms; the figures below are measured, docs/COL-SCAN.md, and scale with cpu_operator_cost like the rest of the planner's costs).
 */
#define COL_SCAN_FIXED		24.0	/* opening the index, the snapshot and the directory of the segment: the startup */
#define COL_SCAN_FIXED_TID	12.0	/* ... for a lookup the directory of heap addresses answers: the source is opened and the first addresses read in 67,000 instructions (16 us measured, 4 of them the probe) */
#define COL_SCAN_GORD_CHUNK	10.0	/* a chunk of the sorted keys of a segment (1,024 entries): its directory entry and its two blobs, for the bounds and each chunk between them */
#define COL_SCAN_GORD_SORT	0.03	/* an address of a range sorted by its position in the heap */
#define COL_SCAN_WALK		0.03	/* the zones and summaries of one unit tested against the constants */
#define COL_SCAN_WORD		0.002	/* ... when the point directory answered, so only the bits of the kept units are walked */
#define COL_SCAN_PROBE		4.0		/* one key looked up in a point directory: its slots are a page or two of a table the lookups of other keys do not share */
#define COL_SCAN_UNIT		22.0	/* reading and decoding the head of a unit that is kept, besides its pages */
#define COL_SCAN_UNIT_PAGES	3		/* the pages of that head, or of the whole unit where the index is narrower */
#define COL_SCAN_ROW		0.009	/* the filter of one value of a column of a kept unit */
#define COL_SCAN_EMIT_IOS	0.15	/* ... to an index-only scan, which forms a tuple of the columns the plan reads and no heap row: 1,200 instructions a row (272M for 160,000 rows of 800,000, a sequential scan 570M) */
#define COL_SCAN_EMIT		0.5		/* handing one row over: its position in the unit, its TID and its visibility (a tuple scan 0.9, a bitmap 0.3 measured) */

bool		sift_enable_col_scan = true;
double		sift_plan_scale = 1.0;

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

/* What the keys of one scan need to know besides the keys */
typedef struct ColKeys
{
	const SiftColDef *defs;		/* the leaves of the index */
	const int  *first;			/* first[attno]: the leaf of attribute attno */
	TupleDesc	idesc;
	bool		recheck;		/* a key replaced by IS NOT NULL because the leaf cannot test its constant exactly: the heap scan applies the clause */
} ColKeys;

/* The constant of a key as a predicate value of the leaf: the key's own type is an integer, date, timestamp, boolean, float, text, char(n) or uuid (numeric: numeric_preds) */
static void
key_value(Oid argtype, int32 typmod, Datum d, SiftColValue *v)
{
	*v = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
	switch (argtype)
	{
		case INT2OID:
			v->v.i = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			v->v.i = DatumGetInt32(d);
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			v->v.i = DatumGetInt64(d);
			break;
		case BOOLOID:
			v->v.i = DatumGetBool(d);
			break;
		case FLOAT4OID:
			v->v.f = DatumGetFloat4(d);
			break;
		case FLOAT8OID:
			v->v.f = DatumGetFloat8(d);
			break;
		case TEXTOID:
			{
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

				v->v.s = VARDATA_ANY(t);
				v->len = VARSIZE_ANY_EXHDR(t);
			}
			break;
		case BPCHAROID:
			{
				/* the leaf holds the value padded with blanks to its length in characters; trailing blanks of the constant are not part of its value */
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
				const char *s = VARDATA_ANY(t);
				int			len = VARSIZE_ANY_EXHDR(t),
							pad;

				while (len > 0 && s[len - 1] == ' ')
					len--;
				pad = typmod - VARHDRSZ - pg_mbstrlen_with_len(s, len);
				if (pad > 0)
				{
					char	   *buf = palloc(len + pad);

					memcpy(buf, s, len);
					memset(buf + len, ' ', pad);
					s = buf;
					len += pad;
				}
				v->v.s = s;
				v->len = len;
			}
			break;
		case UUIDOID:
			v->v.s = (const char *) DatumGetUUIDP(d);
			v->len = UUID_LEN;
			break;
		default:
			SIFT_INVARIANT(false, "sift_col: no scan on a key of type %u", argtype);
	}
}

/* A key the leaf cannot test exactly (a NaN, a numeric out of the range of the leaf): IS NOT NULL keeps every row it could match and the heap scan decides */
static void
key_any(SiftColPred *p, ColKeys *k)
{
	p->op = SIFT_COLOP_NOTNULL;
	p->nvals = 0;
	p->vals = NULL;
	k->recheck = true;
}

/* The strategies of the operator classes: btree's numbers (1 <, 2 <=, 3 =, 4 >=, 5 >) and, for text, 7 LIKE and 8 starts_with (^@) */
#define COL_STRAT_LIKE			7
#define COL_STRAT_STARTS_WITH	8

static SiftColOp
strategy_op(StrategyNumber s)
{
	switch (s)
	{
		case BTLessStrategyNumber:
			return SIFT_COLOP_LT;
		case BTLessEqualStrategyNumber:
			return SIFT_COLOP_LE;
		case BTEqualStrategyNumber:
			return SIFT_COLOP_EQ;
		case BTGreaterEqualStrategyNumber:
			return SIFT_COLOP_GE;
		case BTGreaterStrategyNumber:
			return SIFT_COLOP_GT;
	}
	SIFT_INVARIANT(false, "sift_col: strategy %d is not an operator of the scan", s);
	return 0;
}

/*
 * LIKE and ^@ on a text leaf: the literal prefix of the pattern (up to its first % or _, a backslash escapes) is a byte prefix test of the leaf, which is exact for
 * 'prefix%', '=' for a pattern with no wildcard and a superset for any other pattern (the heap scan applies the pattern then). A pattern with no prefix, one that ends in
 * a backslash (an error to LIKE) and, in an encoding where a wildcard byte can be part of a character, any pattern: IS NOT NULL and a recheck.
 */
static void
like_pred(ColKeys *k, SiftColPred *p, SiftColValue *v, bool starts_with)
{
	const char *pat = v->v.s;
	int			len = v->len,
				n = 0,
				i = 0;
	char	   *buf = palloc(len + 1);

	if (!starts_with && GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() > 1)
	{
		key_any(p, k);
		return;
	}
	while (i < len)
	{
		if (!starts_with && pat[i] == '\\')
		{
			if (i + 1 >= len)
			{
				key_any(p, k);
				return;
			}
			buf[n++] = pat[i + 1];
			i += 2;
			continue;
		}
		if (!starts_with && (pat[i] == '%' || pat[i] == '_'))
			break;
		buf[n++] = pat[i++];
	}
	if (n == 0)
	{
		key_any(p, k);
		return;
	}
	v->v.s = buf;
	v->len = n;
	p->op = !starts_with && i == len ? SIFT_COLOP_EQ : SIFT_COLOP_PREFIX;
	p->nvals = 1;
	p->vals = v;
	if (!starts_with && i < len && !(pat[i] == '%' && i == len - 1))
		k->recheck = true;
}

static bool
key_is_float(Oid t)
{
	return t == FLOAT4OID || t == FLOAT8OID;
}

/*
 * A numeric(p <= 18, s) leaf holds the number times 10^s: the constants are scaled as sift_col_numeric_key says, so that the keys that satisfy
 * the predicate are exactly those of the key range. false: no value can satisfy the key. A constant the leaf cannot hold (NaN, or too large to scale) and an
 * array with an operator other than = fall back to IS NOT NULL.
 */
static bool
numeric_preds(const SiftColDef *def, ScanKey key, SiftColPred *p, ColKeys *k)
{
	StrategyNumber st = key->sk_strategy;

	if (key->sk_flags & SK_SEARCHARRAY)
	{
		ArrayType  *arr = DatumGetArrayTypeP(key->sk_argument);
		int16		len;
		bool		byval;
		char		align;
		Datum	   *el;
		bool	   *nl;
		int			n,
					m = 0;
		SiftColValue *vals;

		if (st != BTEqualStrategyNumber)
		{
			key_any(p, k);
			return true;
		}
		get_typlenbyvalalign(NUMERICOID, &len, &byval, &align);
		deconstruct_array(arr, NUMERICOID, len, byval, align, &el, &nl, &n);
		vals = palloc(Max(n, 1) * sizeof(SiftColValue));
		for (int j = 0; j < n; j++)
		{
			int64		lo;

			if (nl[j])
				continue;
			if (!sift_col_numeric_key_fits(def->scale, el[j]))
			{
				key_any(p, k);
				return true;
			}
			lo = sift_col_numeric_key(def->scale, el[j], SIFT_COL_ROUND_DOWN);
			if (lo != sift_col_numeric_key(def->scale, el[j], SIFT_COL_ROUND_UP))
				continue;
			vals[m] = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
			vals[m++].v.i = lo;
		}
		if (m == 0)
			return false;
		p->op = SIFT_COLOP_IN;
		p->nvals = m;
		p->vals = vals;
		return true;
	}
	if (!sift_col_numeric_key_fits(def->scale, key->sk_argument))
	{
		key_any(p, k);
		return true;
	}
	{
		SiftColValue *v = palloc(sizeof(SiftColValue));
		int64		down = sift_col_numeric_key(def->scale, key->sk_argument, SIFT_COL_ROUND_DOWN),
					up = sift_col_numeric_key(def->scale, key->sk_argument, SIFT_COL_ROUND_UP);

		*v = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
		/* x > c is key > floor(c), x >= c is key >= ceil(c), x < c is key < ceil(c), x <= c is key <= floor(c); x = c needs a constant of at most s digits */
		if (st == BTEqualStrategyNumber && down != up)
			return false;
		v->v.i = (st == BTGreaterStrategyNumber || st == BTLessEqualStrategyNumber || st == BTEqualStrategyNumber) ? down : up;
		p->op = strategy_op(st);
		p->nvals = 1;
		p->vals = v;
	}
	return true;
}

/* The predicates of the keys; false when one cannot be satisfied (an operator is strict: a NULL operand matches nothing) */
static bool
scan_preds(IndexScanDesc scan, ColKeys *k, SiftColPred *preds)
{
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		int			leaf = k->first[key->sk_attno];
		Form_pg_attribute att = TupleDescAttr(k->idesc, key->sk_attno - 1);
		Oid			argtype = OidIsValid(key->sk_subtype) ? key->sk_subtype : att->atttypid;
		SiftColPred *p = &preds[i];

		*p = (SiftColPred) {.col = leaf};
		/* a NaN is stored as a NULL with the overflow flag, and a row with an overflow flag is a candidate of every test of the leaf: the heap scan decides */
		if (k->defs[leaf].type == SIFT_COLT_NUMERIC)
			k->recheck = true;
		if (key->sk_flags & SK_SEARCHNULL)
			p->op = SIFT_COLOP_ISNULL;
		else if (key->sk_flags & SK_SEARCHNOTNULL)
			p->op = SIFT_COLOP_NOTNULL;
		else if (key->sk_flags & SK_ISNULL)
			return false;
		else if (k->defs[leaf].type == SIFT_COLT_NUMERIC)
		{
			if (!numeric_preds(&k->defs[leaf], key, p, k))
				return false;
		}
		else if ((key->sk_flags & SK_SEARCHARRAY) && key->sk_strategy > BTGreaterStrategyNumber)
			key_any(p, k);
		else if (key->sk_flags & SK_SEARCHARRAY)
		{
			ArrayType  *arr = DatumGetArrayTypeP(key->sk_argument);
			Oid			elemtype = ARR_ELEMTYPE(arr);
			int16		len;
			bool		byval;
			char		align;
			Datum	   *el;
			bool	   *nl;
			int			n,
						m = 0;
			bool		nan = false;
			SiftColValue *vals;

			get_typlenbyvalalign(elemtype, &len, &byval, &align);
			deconstruct_array(arr, elemtype, len, byval, align, &el, &nl, &n);
			vals = palloc(Max(n, 1) * sizeof(SiftColValue));
			for (int j = 0; j < n; j++)
				if (!nl[j])
				{
					key_value(elemtype, att->atttypmod, el[j], &vals[m]);
					nan |= key_is_float(elemtype) && isnan(vals[m].v.f);
					m++;
				}
			if (m == 0)
				return false;
			if (nan)
			{
				key_any(p, k);
				continue;
			}
			/* x < ANY (a) is x < max(a) and x > ANY (a) is x > min(a); only the integer-like, float and boolean keys have order operators */
			if (key->sk_strategy != BTEqualStrategyNumber)
			{
				int			best = 0;
				bool		up = key->sk_strategy == BTLessStrategyNumber || key->sk_strategy == BTLessEqualStrategyNumber;

				for (int j = 1; j < m; j++)
				{
					bool		more = key_is_float(elemtype) ? vals[j].v.f > vals[best].v.f : vals[j].v.i > vals[best].v.i;
					bool		less = key_is_float(elemtype) ? vals[j].v.f < vals[best].v.f : vals[j].v.i < vals[best].v.i;

					if (up ? more : less)
						best = j;
				}
				vals[0] = vals[best];
				m = 1;
				p->op = strategy_op(key->sk_strategy);
			}
			else
				p->op = SIFT_COLOP_IN;
			p->nvals = m;
			p->vals = vals;
		}
		else
		{
			SiftColValue *v = palloc(sizeof(SiftColValue));

			key_value(argtype, att->atttypmod, key->sk_argument, v);
			if (key_is_float(argtype) && isnan(v->v.f))
			{
				key_any(p, k);
				continue;
			}
			if (key->sk_strategy == COL_STRAT_LIKE || key->sk_strategy == COL_STRAT_STARTS_WITH)
			{
				like_pred(k, p, v, key->sk_strategy == COL_STRAT_STARTS_WITH);
				continue;
			}
			p->op = strategy_op(key->sk_strategy);
			p->nvals = 1;
			p->vals = v;
		}
	}
	return true;
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

	so->first = palloc((r->natts + 1) * sizeof(int));
	memcpy(so->first, r->first, (r->natts + 1) * sizeof(int));
	so->natts = r->natts;
	so->defs = palloc(r->meta.ncols * sizeof(SiftColDef));
	memcpy(so->defs, r->meta.cols, r->meta.ncols * sizeof(SiftColDef));
	so->preds = palloc(Max(scan->numberOfKeys, 1) * sizeof(SiftColPred));
	keys = (ColKeys) {.defs = so->defs, .first = so->first, .idesc = RelationGetDescr(scan->indexRelation)};
	if (so->heap == NULL)
		so->heap = table_open(scan->indexRelation->rd_index->indrelid, AccessShareLock);
	so->src = sift_col_open(so->heap, scan->indexRelation, scan->xs_snapshot, so->cxt, SIFT_COLO_NOCHECKS | SIFT_COLO_POINTS | (scan->xs_want_itup ? 0 : SIFT_COLO_POINT_TIDS | (lazy ? SIFT_COLO_POINT_LAZY : 0)));
	if (scan_preds(scan, &keys, so->preds))
	{
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

	if (r->nleaves[attno] != 1 || r->name[attno] || index->rd_opcintype[attno - 1] == ANYELEMENTOID || (d->path && d->path[0]) || (d->flags & SIFT_COLD_LOSSY))
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

/* ---- the cost ---- */

/* The range of key values a clause keeps when its operand is a constant of an integer-like type (false: it is not, or the clause is not a comparison) */
static bool
clause_range(PlannerInfo *root, Oid opfamily, Node *clause, int64 *lo, int64 *hi, int64 **set, int *nset)
{
	Oid			opno;
	Node	   *right;
	Const	   *c;
	int			strat;

	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	*set = NULL;
	*nset = 0;
	if (IsA(clause, OpExpr))
	{
		opno = ((OpExpr *) clause)->opno;
		right = (Node *) lsecond(((OpExpr *) clause)->args);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		opno = ((ScalarArrayOpExpr *) clause)->opno;
		right = (Node *) lsecond(((ScalarArrayOpExpr *) clause)->args);
	}
	else
		return false;
	strat = get_op_opfamily_strategy(opno, opfamily);
	right = estimate_expression_value(root, right);
	if (!IsA(right, Const) || ((Const *) right)->constisnull)
		return false;
	c = (Const *) right;
	if (IsA(clause, OpExpr))
	{
		int64		v;

		switch (c->consttype)
		{
			case INT2OID:
				v = DatumGetInt16(c->constvalue);
				break;
			case INT4OID:
			case DATEOID:
				v = DatumGetInt32(c->constvalue);
				break;
			case INT8OID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				v = DatumGetInt64(c->constvalue);
				break;
			default:
				return false;
		}
		if (strat == BTLessStrategyNumber || strat == BTLessEqualStrategyNumber || strat == BTEqualStrategyNumber)
			*hi = strat == BTLessStrategyNumber && v > PG_INT64_MIN ? v - 1 : v;
		if (strat == BTGreaterStrategyNumber || strat == BTGreaterEqualStrategyNumber || strat == BTEqualStrategyNumber)
			*lo = strat == BTGreaterStrategyNumber && v < PG_INT64_MAX ? v + 1 : v;
		return true;
	}
	else
	{
		ArrayType  *arr = DatumGetArrayTypeP(c->constvalue);
		Oid			et = ARR_ELEMTYPE(arr);
		int16		len;
		bool		byval;
		char		align;
		Datum	   *el;
		bool	   *nl;
		int			n,
					m = 0;
		int64	   *vals;

		if (strat != BTEqualStrategyNumber || (et != INT2OID && et != INT4OID && et != INT8OID && et != DATEOID && et != TIMESTAMPOID && et != TIMESTAMPTZOID))
			return false;
		get_typlenbyvalalign(et, &len, &byval, &align);
		deconstruct_array(arr, et, len, byval, align, &el, &nl, &n);
		vals = palloc(Max(n, 1) * sizeof(int64));
		for (int j = 0; j < n; j++)
			if (!nl[j])
				vals[m++] = et == INT2OID ? DatumGetInt16(el[j]) : et == INT4OID || et == DATEOID ? DatumGetInt32(el[j]) : DatumGetInt64(el[j]);
		qsort(vals, m, sizeof(int64), sift_cmp_i64);
		*set = vals;
		*nset = m;
		return true;
	}
}

/* does the zone [zlo, zhi] hold a key of the sorted set (or lie in [lo, hi] when there is no set)? */
static bool
range_meets(int64 lo, int64 hi, const int64 *set, int nset, int64 zlo, int64 zhi)
{
	int			a = 0,
				b = nset;

	if (zlo > zhi || zhi < lo || zlo > hi)
		return false;
	if (set == NULL)
		return true;
	while (a < b)
	{
		int			m = a + (b - a) / 2;

		if (set[m] < zlo)
			a = m + 1;
		else
			b = m;
	}
	return a < nset && set[a] <= zhi;
}

/*
 * The share of the units the constants of the index quals leave, by the real zones of a sample of the units; 1 when the zones cannot tell (text, a parameter,
 * a column the sample does not describe). The quals are ANDed: a unit stays when every one of them may hold in it.
 */
static double
zones_kept(PlannerInfo *root, IndexPath *path, Relation irel)
{
	const SiftColZones *z = sift_col_zones(irel);
	IndexOptInfo *index = path->indexinfo;
	bool	   *alive;
	bool		tested = false;
	int			left = 0;

	if (z == NULL || z->n == 0)
		return 1.0;
	alive = palloc(z->n * sizeof(bool));
	memset(alive, true, z->n * sizeof(bool));
	foreach_node(IndexClause, ic, path->indexclauses)
	{
		/* the estimates below read the catalog, which can reload the index's relcache entry: ask for its cached state where it is used */
		int			leaf = sift_col_rel(irel)->first[ic->indexcol + 1];

		foreach_node(RestrictInfo, ri, ic->indexquals)
		{
			int64		lo,
						hi;
			int64	   *set;
			int			nset;

			if (leaf >= z->ncols || z->lo[leaf] == NULL || !clause_range(root, index->opfamily[ic->indexcol], (Node *) ri->clause, &lo, &hi, &set, &nset))
				continue;
			tested = true;
			for (uint32 k = 0; k < z->n; k++)
				alive[k] = alive[k] && range_meets(lo, hi, set, nset, z->lo[leaf][k], z->hi[leaf][k]);
		}
	}
	if (!tested)
		return 1.0;
	for (uint32 k = 0; k < z->n; k++)
		left += alive[k];
	return (double) left / z->n;
}

/*
 * The share of the units the small-set summaries of the columns an equality or an IN list tests leave, as SiftAgg counts it
 * (plan_cost.c set_kept): a test on the share s of a leaf's distinct values (the clause's selectivity) keeps a unit that holds n keys of it with the
 * probability 1 - exp(-s * n); a unit without a summary stays, and a summary that keeps nearly every unit is not counted.
 */
#define COL_SCAN_SET_USEFUL	0.85

static double
sets_kept(PlannerInfo *root, IndexPath *path, Relation irel)
{
	const SiftColLeafStats *ls = sift_col_leaf_stats(irel);
	IndexOptInfo *index = path->indexinfo;
	double		kept = 1.0;

	foreach_node(IndexClause, ic, path->indexclauses)
	{
		const SiftColLeafStats *l = &ls[sift_col_rel(irel)->first[ic->indexcol + 1]];

		if (l->set_units <= 0 || l->set_sampled <= 0)
			continue;
		foreach_node(RestrictInfo, ri, ic->indexquals)
		{
			Oid			opno = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->opno :
				IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->opno : InvalidOid;
			double		cover = l->set_units / l->set_sampled,
						held;

			if (!OidIsValid(opno) || get_op_opfamily_strategy(opno, index->opfamily[ic->indexcol]) != BTEqualStrategyNumber)
				continue;
			held = 1.0 - exp(-clause_selectivity(root, (Node *) ri->clause, index->rel->relid, JOIN_INNER, NULL) * l->set_keys / l->set_units);
			if (held < COL_SCAN_SET_USEFUL)
				kept = Min(kept, 1.0 - cover + cover * held);
		}
	}
	return kept;
}


/* A LIKE or ^@ whose pattern starts with a literal prefix (like_pred): the dictionary names the keys with it, and the point directory can look them up */
static bool
prefix_clause(const OpExpr *op, int strategy)
{
	const Const *c = list_length(op->args) == 2 && IsA(lsecond(op->args), Const) ? (const Const *) lsecond(op->args) : NULL;
	const char *s;
	int			len;

	if ((strategy != COL_STRAT_LIKE && strategy != COL_STRAT_STARTS_WITH) || c == NULL || c->constisnull || c->consttype != TEXTOID)
		return false;
	s = VARDATA_ANY(DatumGetTextPP(c->constvalue));
	len = VARSIZE_ANY_EXHDR(DatumGetTextPP(c->constvalue));
	if (strategy == COL_STRAT_STARTS_WITH)
		return len > 0;
	return len > 0 && s[0] != '%' && s[0] != '_' && s[0] != '\\';
}
/*
 * The share of the units that hold the rows a point directory (point_columns) lists for the equalities and short IN lists of the quals, and whether it answers
 * at all: the keys are the clause's own (at most SIFT_CPOINT_MAX_KEYS, and past SIFT_CPOINT_MIN_KEYS a sixteenth of the rows), the rows of the keys the selectivity of the clause (misses and the keys with many rows
 * included), and a lookup finds the units of a random layout, 1 - exp(-rows / units) of them; the segments without a directory stay whole. *tid is the
 * share of those units whose directory lists heap addresses (point_tid_columns): the scan returns their rows without reading the unit (an index-only scan reads it, for the values).
 */
static double
point_kept(PlannerInfo *root, IndexPath *path, Relation irel, double units, int *nprobes, double *tid, double *extra)
{
	const SiftColLeafStats *ls = sift_col_leaf_stats(irel);
	IndexOptInfo *index = path->indexinfo;
	double		kept = 1.0;

	*nprobes = 0;
	*tid = 0.0;
	*extra = 0.0;
	foreach_node(IndexClause, ic, path->indexclauses)
	{
		const SiftColLeafStats *l = &ls[sift_col_rel(irel)->first[ic->indexcol + 1]];

		if (l->point_units <= 0 || l->nunits <= 0)
			continue;
		foreach_node(RestrictInfo, ri, ic->indexquals)
		{
			Oid			opno = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->opno :
				IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->opno : InvalidOid;
			int			strategy = OidIsValid(opno) ? get_op_opfamily_strategy(opno, index->opfamily[ic->indexcol]) : 0;
			double		nkeys = 1.0,
						cover = Min(l->point_units / l->nunits, 1.0),
						rows,
						share;

			if (strategy != BTEqualStrategyNumber && !(IsA(ri->clause, OpExpr) && prefix_clause((OpExpr *) ri->clause, strategy)))
				continue;
			if (IsA(ri->clause, ScalarArrayOpExpr))
				nkeys = estimate_array_length(root, lsecond(((ScalarArrayOpExpr *) ri->clause)->args));
			rows = clause_selectivity(root, (Node *) ri->clause, index->rel->relid, JOIN_INNER, NULL) * index->rel->tuples;
			/* a prefix names the keys of the dictionary that start with it: the directory probes one key each (the rows the directory lists per key say how many) */
			if (strategy != BTEqualStrategyNumber)
				nkeys = Max(1.0, ceil(rows * (l->point_rows > 0 ? l->point_keys / l->point_rows : 1.0)));
			if (nkeys > SIFT_CPOINT_MAX_KEYS || (nkeys > SIFT_CPOINT_MIN_KEYS && nkeys > index->rel->tuples / 16))
				continue;
			share = 1.0 - exp(-rows / units);
			/* equal counts: a key that is in every unit still has its rows listed, and the scan filters those, not the rows of the units */
			if (1.0 - cover + cover * share <= kept)
			{
				kept = 1.0 - cover + cover * share;
				*nprobes = (int) ceil(nkeys);
				*tid = path->path.pathtype == T_IndexOnlyScan ? 0.0 : l->point_tid_units / l->point_units;
			}
		}
	}
	/* a range on a leaf with a global order is a seek in the sorted keys: the addresses of the rows between the bounds, in as many units as a lookup of that many rows finds */
	for (int col = 0; col < index->nkeycolumns; col++)
	{
		const SiftColLeafStats *l = &ls[sift_col_rel(irel)->first[col + 1]];
		List	   *ranges = NIL;
		bool		other = false;

		foreach_node(IndexClause, ic, path->indexclauses)
		{
			if (ic->indexcol != col)
				continue;
			foreach_node(RestrictInfo, ri, ic->indexquals)
			{
				int			strategy = IsA(ri->clause, OpExpr) ? get_op_opfamily_strategy(((OpExpr *) ri->clause)->opno, index->opfamily[col]) : 0;

				if (strategy == BTLessStrategyNumber || strategy == BTLessEqualStrategyNumber || strategy == BTGreaterStrategyNumber || strategy == BTGreaterEqualStrategyNumber)
					ranges = lappend(ranges, ri);
				else
					other = true;
			}
		}
		if (ranges != NIL && !other && l->nunits > 0 && l->gord_rows > l->gord_grp_rows)
		{
			double		rows = clauselist_selectivity(root, ranges, index->rel->relid, JOIN_INNER, NULL) * index->rel->tuples,
						cover = Min((l->gord_rows - l->gord_grp_rows) / Max(index->rel->tuples, 1.0), 1.0),
						share = 1.0 - exp(-rows / units);

			if (rows <= SIFT_CORDER_MAX_ROWS && (*nprobes == 0 ? 1.0 - cover + cover * share <= kept : 1.0 - cover + cover * share < kept))
			{
				kept = 1.0 - cover + cover * share;
				*nprobes = 1;
				*tid = path->path.pathtype == T_IndexOnlyScan ? 0.0 : cover;
				*extra = (2.0 + ceil(rows / 1024.0)) * COL_SCAN_GORD_CHUNK + rows * COL_SCAN_GORD_SORT;
			}
		}
		list_free(ranges);
	}
	return kept;
}

/*
 * The cost of the scan in the planner's units (about microseconds, see COL_SCAN_FIXED): the startup is the opening of the index and the pruning of the units (the
 * zones and summaries of each, or the keys looked up in the point directory and the bits of the kept units); the rest is reading the kept units, filtering their
 * values (only the rows the directory lists, when it is what keeps them) and handing the rows over. The start is separate so that a LIMIT stops the scan early: the units are read one at a time.
 */
void
sift_col_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost,
						Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Relation	irel;
	double		kept,
				pkept,
				tid,
				extra,
				units,
				kept_units,
				idx_rows,
				scale = cpu_operator_cost / 0.0025;
	int			ncols = list_length(path->indexclauses),
				nprobes;

	genericcostestimate(root, path, loop_count, &costs);
	irel = index_open(index->indexoid, AccessShareLock);
	units = Max(1.0, ceil(index->rel->pages / 256.0));
	pkept = point_kept(root, path, irel, units, &nprobes, &tid, &extra);
	kept = Min(Min(zones_kept(root, path, irel), sets_kept(root, path, irel)), pkept);
	index_close(irel, AccessShareLock);
	kept_units = kept * units;
	/* an IN list or ANY (array) is a scan for each element, and genericcostestimate counts the tuples of one */
	idx_rows = costs.numIndexTuples * Max(costs.num_sa_scans, 1);
	costs.indexStartupCost = scale * (COL_SCAN_FIXED - (nprobes > 0 ? tid : 0.0) * (COL_SCAN_FIXED - COL_SCAN_FIXED_TID) + (nprobes > 0 ? nprobes * COL_SCAN_PROBE + extra + kept_units * COL_SCAN_WORD : units * COL_SCAN_WALK));
	costs.indexTotalCost = costs.indexStartupCost
		+ kept_units * (nprobes > 0 ? 1.0 - tid : 1.0) * (scale * COL_SCAN_UNIT + Min(COL_SCAN_UNIT_PAGES, Max(1.0, index->pages / units)) * seq_page_cost)
		+ (nprobes > 0 ? idx_rows * (1.0 - tid) : kept * index->rel->tuples) * ncols * scale * COL_SCAN_ROW
		+ index->pages * cpu_tuple_cost
		+ idx_rows * (scale * (path->path.pathtype == T_IndexOnlyScan ? COL_SCAN_EMIT_IOS : COL_SCAN_EMIT) + cpu_index_tuple_cost);
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	/* the addresses of a lookup come in heap order (a list ascends, the segments follow each other), so the tuple scan reads the heap as a bitmap scan does */
	*indexCorrelation = nprobes > 0 ? tid : 0.0;
	*indexPages = Max(1.0, kept * index->pages);
}

/* sift.plan_scale: the table the index covers has that many times the pages and rows, with the statistics of the columns as they are (an absolute n_distinct stays, a fraction grows) */
static void
col_scale_rel(RelOptInfo *rel)
{
	if (sift_plan_scale <= 1.0)
		return;
	rel->pages = (BlockNumber) Min(rel->pages * sift_plan_scale, (double) (MaxBlockNumber - 1));
	rel->tuples *= sift_plan_scale;
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		ii->pages = (BlockNumber) Min(ii->pages * sift_plan_scale, (double) (MaxBlockNumber - 1));
		ii->tuples *= sift_plan_scale;
	}
}

/* ---- off means off ---- */

/*
 * With sift.enable_col_scan off the planner must build no bitmap path on a sift_col index: a path with a bad cost still beats one that
 * enable_seqscan = off disabled (PostgreSQL counts the disabled nodes before the cost). The index stays in the list, SiftAgg reads it there.
 * Up to PostgreSQL 18 get_relation_info_hook clears amhasgetbitmap of the index; 19 has no such hook, so the bitmap paths that use one are
 * disabled in the pathlist of the relation, one node more than a sequential scan that enable_seqscan turned off. sift.plan_scale is applied by
 * get_relation_info_hook up to 18 and by build_simple_rel_hook, which replaces it, in 19.
 */
static set_rel_pathlist_hook_type prev_pathlist_hook;

/*
 * A query the index covers gets an index-only path, and the bitmap path PostgreSQL builds beside it carries the price of that one: a read of every kept unit (the
 * lookup of a key in the point directory returns the addresses and reads none, when the directory lists them). A lookup by one clause of the directory therefore
 * reads its units through the index-only path, 20 to 50 times slower than the bitmap path on the addresses. The bitmap path is added priced as what it does.
 */
static void
add_address_paths(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *added = NIL;

	foreach_ptr(Path, p, rel->pathlist)
	{
		IndexPath  *ip = (IndexPath *) p;
		IndexPath  *bp;

		if (!IsA(p, IndexPath) || p->pathtype != T_IndexOnlyScan || ip->indexinfo->relam != sift_col_am_oid() || ip->indexclauses == NIL ||
			ip->indexorderbys != NIL)
			continue;
		bp = create_index_path(root, ip->indexinfo, ip->indexclauses, ip->indexorderbys, ip->indexorderbycols, ip->path.pathkeys, ip->indexscandir, false,
							   PATH_REQ_OUTER(&ip->path), 1.0, false);
		if (bp->path.total_cost >= ip->path.total_cost)
			continue;
		added = lappend(added, bp);
		added = lappend(added, create_bitmap_heap_path(root, rel, (Path *) bp, bms_union(rel->lateral_relids, PATH_REQ_OUTER(&bp->path)), 1.0, 0));
	}
	foreach_ptr(Path, p, added)
		add_path(rel, p);
}

#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
col_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	bool		indexed = false;

	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam == sift_col_am_oid())
		{
			indexed = true;
			if (!sift_enable_col_scan)
				ii->amhasgetbitmap = ii->amhasgettuple = false;
		}
	if (indexed)
		col_scale_rel(rel);
}

static void
col_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (sift_enable_col_scan)
		add_address_paths(root, rel);
}

void
sift_col_scan_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = col_get_relation_info;
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = col_set_rel_pathlist;
}
#else
static bool
bitmap_uses_col(Path *path)
{
	switch (nodeTag(path))
	{
		case T_IndexPath:
			return ((IndexPath *) path)->indexinfo->relam == sift_col_am_oid();
		case T_BitmapAndPath:
			foreach_ptr(Path, p, ((BitmapAndPath *) path)->bitmapquals)
				if (bitmap_uses_col(p))
					return true;
			return false;
		case T_BitmapOrPath:
			foreach_ptr(Path, p, ((BitmapOrPath *) path)->bitmapquals)
				if (bitmap_uses_col(p))
					return true;
			return false;
		default:
			return false;
	}
}

/* disabled like a node of a plan whose enable_ setting is off: it loses to every path that is not, whatever its cost */
static void
disable_col_bitmaps(List *paths)
{
	foreach_ptr(Path, p, paths)
		if (IsA(p, BitmapHeapPath) && bitmap_uses_col(((BitmapHeapPath *) p)->bitmapqual))
			p->disabled_nodes = Max(p->disabled_nodes, 1) + 1;
}

static build_simple_rel_hook_type prev_simple_rel_hook;

static void
col_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (prev_simple_rel_hook)
		prev_simple_rel_hook(root, rel, rte);
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam == sift_col_am_oid())
		{
			col_scale_rel(rel);
			return;
		}
}

static void
col_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (sift_enable_col_scan)
	{
		add_address_paths(root, rel);
		return;
	}
	disable_col_bitmaps(rel->pathlist);
	disable_col_bitmaps(rel->partial_pathlist);
}

void
sift_col_scan_init(void)
{
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = col_set_rel_pathlist;
	prev_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = col_build_simple_rel;
}
#endif
