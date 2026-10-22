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
#include "executor/tuptable.h"
#include "utils/datum.h"
#include "mb/pg_wchar.h"
#include "pgstat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/optimizer.h"
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

/* the cost of the filter of one value of an integer or dictionary column, in operator costs (measured: docs/COL-SCAN.md) */
#define COL_SCAN_ROW_COST	0.35
/* the zones of a unit and the set up of its scan */
#define COL_SCAN_UNIT_COST	40.0

bool		sift_enable_col_scan = false;

/*
 * One scan of the index, as a bitmap (amgetbitmap) or tuple by tuple (amgettuple): the source, the predicates of the keys and, for amgettuple, the rows of
 * the unit being returned. Everything but the heap relation lives in cxt, which a rescan resets.
 */
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
	Datum	   *vals;			/* n * natts, only when the executor wants index tuples */
	bool	   *nulls;
	IndexTuple	itup;
	IndexFetchTableData *fetch;	/* the heap, for a string the index could not keep (an overflow value) */
	TupleTableSlot *slot;
} ColScanOpaque;

IndexScanDesc
sift_col_ambeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	ColScanOpaque *so = palloc0(sizeof(ColScanOpaque));

	so->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_col scan", ALLOCSET_DEFAULT_SIZES);
	so->batch = AllocSetContextCreate(so->cxt, "sift_col scan batch", ALLOCSET_DEFAULT_SIZES);
	scan->opaque = so;
	scan->xs_itupdesc = RelationGetDescr(index);
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
	so->tids = NULL;
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
col_scan_begin(IndexScanDesc scan)
{
	ColScanOpaque *so = scan->opaque;
	MemoryContext old = MemoryContextSwitchTo(so->cxt);
	SiftColRel *r = sift_col_rel(scan->indexRelation);
	ColKeys		keys;

	so->first = palloc((r->natts + 1) * sizeof(int));
	memcpy(so->first, r->first, (r->natts + 1) * sizeof(int));
	so->natts = r->natts;
	so->defs = palloc(r->meta.ncols * sizeof(SiftColDef));
	memcpy(so->defs, r->meta.cols, r->meta.ncols * sizeof(SiftColDef));
	so->preds = palloc(Max(scan->numberOfKeys, 1) * sizeof(SiftColPred));
	keys = (ColKeys) {.defs = so->defs, .first = so->first, .idesc = RelationGetDescr(scan->indexRelation)};
	if (so->heap == NULL)
		so->heap = table_open(scan->indexRelation->rd_index->indrelid, AccessShareLock);
	so->src = sift_col_open(so->heap, scan->indexRelation, scan->xs_snapshot, so->cxt, SIFT_COLO_NOCHECKS);
	if (scan_preds(scan, &keys, so->preds))
	{
		so->npreds = scan->numberOfKeys;
		sift_col_begin(so->src, so->preds, so->npreds, 0, PG_UINT32_MAX);
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
	if (so->unsat)
		return false;
	while (sift_col_next(so->src, unit))
	{
		CHECK_FOR_INTERRUPTS();
		sift_col_sel_all(unit, sel);
		if (!sift_col_filter(unit, so->preds, so->npreds, sel) || sift_col_sel_count(sel) == 0)
			continue;
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

	col_scan_begin(scan);
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

			if (!sift_col_amcanreturn(scan->indexRelation, a))
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
		col_scan_begin(scan);
	while (so->pos >= so->n)
		if (!col_fill_batch(scan))
			return false;
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
		*set = vals;
		*nset = m;
		return true;
	}
}

static bool
range_meets(int64 lo, int64 hi, const int64 *set, int nset, int64 zlo, int64 zhi)
{
	if (zlo > zhi || zhi < lo || zlo > hi)
		return false;
	if (set == NULL)
		return true;
	for (int i = 0; i < nset; i++)
		if (set[i] >= zlo && set[i] <= zhi)
			return true;
	return false;
}

/*
 * The share of the units the constants of the index quals leave, by the real zones of a sample of the units; 1 when the zones cannot tell (text, a parameter,
 * a column the sample does not describe). The quals are ANDed: a unit stays when every one of them may hold in it.
 */
static double
zones_kept(PlannerInfo *root, IndexPath *path, Relation irel)
{
	const SiftColZones *z = sift_col_zones(irel);
	SiftColRel *r = sift_col_rel(irel);
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
		int			leaf = r->first[ic->indexcol + 1];

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
	SiftColRel *r = sift_col_rel(irel);
	IndexOptInfo *index = path->indexinfo;
	double		kept = 1.0;

	foreach_node(IndexClause, ic, path->indexclauses)
	{
		const SiftColLeafStats *l = &ls[r->first[ic->indexcol + 1]];

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

void
sift_col_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost, Cost *indexTotalCost,
						Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Relation	irel;
	double		kept,
				units,
				cand_rows;
	int			ncols = list_length(path->indexclauses);

	genericcostestimate(root, path, loop_count, &costs);
	irel = index_open(index->indexoid, AccessShareLock);
	kept = Min(zones_kept(root, path, irel), sets_kept(root, path, irel));
	index_close(irel, AccessShareLock);
	units = Max(1.0, ceil(index->rel->pages / 256.0));
	cand_rows = kept * index->rel->tuples;
	costs.indexStartupCost = random_page_cost;
	costs.indexTotalCost = random_page_cost + units * COL_SCAN_UNIT_COST * cpu_operator_cost
		+ kept * index->pages * seq_page_cost
		+ cand_rows * ncols * COL_SCAN_ROW_COST * cpu_operator_cost
		+ costs.numIndexTuples * cpu_index_tuple_cost;
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = 0.0;
	*indexPages = Max(1.0, kept * index->pages);
}

/* ---- off means off ---- */

/*
 * With sift.enable_col_scan off the planner must build no bitmap path on a sift_col index: a path with a bad cost still beats one that
 * enable_seqscan = off disabled (PostgreSQL counts the disabled nodes before the cost). The index stays in the list, SiftAgg reads it there.
 * Up to PostgreSQL 18 get_relation_info_hook clears amhasgetbitmap of the index; 19 has no such hook, so the bitmap paths that use one are
 * disabled in the pathlist of the relation, one node more than a sequential scan that enable_seqscan turned off.
 */
#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
col_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	if (sift_enable_col_scan)
		return;
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->relam == sift_col_am_oid())
			ii->amhasgetbitmap = ii->amhasgettuple = false;
}

void
sift_col_scan_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = col_get_relation_info;
}
#else
static set_rel_pathlist_hook_type prev_pathlist_hook;

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

static void
col_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (sift_enable_col_scan)
		return;
	disable_col_bitmaps(rel->pathlist);
	disable_col_bitmaps(rel->partial_pathlist);
}

void
sift_col_scan_init(void)
{
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = col_set_rel_pathlist;
}
#endif
