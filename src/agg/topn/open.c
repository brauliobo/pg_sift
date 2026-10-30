/*
 * agg/topn/open.c - the top-N reader: its keys, spec and start
 * Which leaves can be a key or an output, the spec the planner hands over, the bounds, and
 * the heap, the sort and the fetch a reader opens with, and its close.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "access/tableam.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "utils/varlena.h"

#include "agg/topn/int.h"

bool		sift_topn_zone_keys = true;
#ifdef SIFT_TESTING
bool		sift_topn_heads = true;
#endif
bool		sift_topn_runs = true;
bool		sift_topn_gorder_on = true;

/* Does a heap entry keep the value of the leaf as an integer (leaf_datum gives its Datum): the output of a row is then read from its unit */
bool
sift_topn_out_ok(const SiftSrcLeaf *lf)
{
	switch (lf->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return sift_topn_key_ok(lf) && (lf->vtype == SCV_I32 || lf->vtype == SCV_I64);
		default:
			return false;
	}
}

bool
sift_topn_key_ok(const SiftSrcLeaf *lf)
{
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->lossy)
		return false;
	switch (lf->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return lf->vtype == SCV_I32 || lf->vtype == SCV_I64;
		case NUMERICOID:
			/* a wide numeric is read through the dictionary of its datums (agg/wide.c), a NaN among them, which numeric_cmp sorts last */
			return lf->vtype == SCV_I64 || (lf->wide && lf->vtype == SCV_CODE);
		case INTERVALOID:
			/* read through the dictionary of its datums (agg/wide.c); interval_cmp compares the total, so 1 mon and 30 days tie */
			return lf->wide && lf->vtype == SCV_CODE;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case CASHOID:
		case TIMEOID:
			return lf->vtype == SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return lf->vtype == SCV_F64;
		case TEXTOID:
		case VARCHAROID:
		case UUIDOID:
		case BYTEAOID:
		case INETOID:
			return lf->vtype == SCV_CODE;
		default:
			/* an enum is the vector of the OIDs of its values: the scan ranks them in the order of the type */
			return type_is_enum(lf->typid) && lf->vtype == SCV_I32 && !lf->wide;
	}
}

Node *
sift_topn_spec(int nkey, const int *leaf, const bool *desc, const bool *nulls_first, const Oid *sortop, const Oid *cmp, const Oid *collid, const SiftTopXform *xf, List *exprs, int nout, const int *attno, const int *oleaf, int flags, Node *count, Node *offset)
{
	List	   *l = list_make1_int(nkey);

	for (int j = 0; j < nkey; j++)
	{
		l = lappend_int(lappend_int(l, leaf[j]), (desc[j] ? 1 : 0) | (nulls_first[j] ? 2 : 0));
		l = lappend_int(lappend_int(lappend_int(l, (int) sortop[j]), (int) cmp[j]), (int) collid[j]);
		l = lappend_int(lappend_int(lappend_int(lappend_int(l, xf[j].kind), (int) (uint32) xf[j].c), (int) (uint32) ((uint64) xf[j].c >> 32)), (int) xf[j].coll);
	}
	l = lappend_int(l, nout);
	for (int c = 0; c < nout; c++)
		l = lappend_int(lappend_int(l, attno[c]), oleaf[c]);
	l = lappend_int(l, flags);
	return (Node *) list_make4(l, count, offset, exprs);
}

/* The rows the scan holds for LIMIT and OFFSET, as EXPLAIN shows them: their sum when they are constants; ", sorted" when the plan sorts every row that passes; ", stored dictionary" when the first key is read as the codes of the stored dictionary */
char *
sift_topn_limit_text(Node *spec)
{
	Node	   *count = SPEC_COUNT(spec),
			   *offset = SPEC_OFFSET(spec);
	int			flags = llast_int(SPEC_KEYS(spec));
	const char *how = psprintf("%s%s", (flags & TOPN_SORT) ? ", sorted" : "", (flags & TOPN_RAWKEY) ? ", stored dictionary" : "");
	int64		n;

	if (IsA(count, Const) && (offset == NULL || IsA(offset, Const)) &&
		!pg_add_s64_overflow(DatumGetInt64(((Const *) count)->constvalue), offset ? DatumGetInt64(((Const *) offset)->constvalue) : 0, &n))
		return psprintf(INT64_FORMAT "%s", n, how);
	return psprintf("by parameter%s", how);
}

/* The value of a LIMIT or OFFSET expression of the plan, evaluated as the Limit node does when it starts */
static int64
eval_bound(Node *e, ExprContext *econtext, PlanState *parent, bool *isnull)
{
	return DatumGetInt64(ExecEvalExprSwitchContext(ExecInitExpr((Expr *) e, parent), econtext, isnull));
}

/*
 * LIMIT + OFFSET, whatever the values the plan was made for: false when no heap can hold them (no limit, past the cap, an overflow). *rows is the sum all the same
 * (at most INT_MAX) when there is one: the walk of the order array holds no heap and ends at it.
 * A negative sum is a rows-wanted of zero: the Limit node above raises its error before it asks for a row. *offset: the OFFSET (0 for NULL).
 */
static bool
bound_rows(Node *spec, ExprContext *econtext, PlanState *parent, int *rows, int64 *offset)
{
	bool		isnull,
				offnull = true;
	int64		count = eval_bound(SPEC_COUNT(spec), econtext, parent, &isnull),
				n;

	*rows = 0;
	*offset = SPEC_OFFSET(spec) ? eval_bound(SPEC_OFFSET(spec), econtext, parent, &offnull) : 0;
	if (offnull)
		*offset = 0;
	if (isnull || pg_add_s64_overflow(count, *offset, &n))
		return false;
	*rows = (int) Min(Max(n, 0), PG_INT32_MAX);
	return n <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS);
}

/* The rows ordered by the keys (in the direction of each, the NULLs where it says), then by the TID: the order of the heap */
Tuplesortstate *
sort_begin(SiftTopN *t)
{
	int			n = t->nkey + 1;
	TupleDesc	desc = CreateTemplateTupleDesc(n);
	AttrNumber *att = palloc(n * sizeof(AttrNumber));
	Oid		   *op = palloc(n * sizeof(Oid)),
			   *coll = palloc0(n * sizeof(Oid));
	bool	   *nf = palloc(n * sizeof(bool));

	for (int j = 0; j < n; j++)
	{
		bool		key = j < t->nkey;
		bool		blob = key && t->key[j].blob;

		TupleDescInitEntry(desc, j + 1, NULL, blob ? t->key[j].typid : INT8OID, -1, 0);
		att[j] = j + 1;
		/* a key of byte strings sorts by the operator and collation of the query: the comparator of the heap plan itself */
		op[j] = blob ? t->key[j].sortop : get_opfamily_member(INTEGER_BTREE_FAM_OID, INT8OID, INT8OID, key && t->key[j].desc ? BTGreaterStrategyNumber : BTLessStrategyNumber);
		coll[j] = blob ? t->key[j].collid : InvalidOid;
		nf[j] = key && t->key[j].nulls_first;
	}
	sift_tupdesc_finalize(desc);
	t->sin = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	t->sout = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	return tuplesort_begin_heap(desc, n, att, op, coll, nf, work_mem, NULL, TUPLESORT_RANDOMACCESS);
}

/* The slot and the index fetch of a heap: of the partition part of a partitioned table, with the attribute numbers it has for the columns of the scan tuple (by the names of the table's) */
void
fetch_begin(SiftTopN *t, int part)
{
	TopFetch   *f = &t->pf[part];
	Relation	heap = t->rel;
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(t));

	f->attno = t->attno;
	if (heap->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		heap = sift_aggpart_heap(t->src, part);
		f->attno = palloc(Max(t->nout, 1) * sizeof(int));
		for (int c = 0; c < t->nout; c++)
			f->attno[c] = get_attnum(RelationGetRelid(heap), get_attname(RelationGetRelid(t->rel), t->attno[c], false));
	}
	f->slot = table_slot_create(heap, NULL);
	f->fetch = sift_table_index_fetch_begin(heap);
	MemoryContextSwitchTo(old);
}

static int
enum_by_order(const void *a, const void *b)
{
	return SIFT_CMP3(((const EnumItem *) a)->order, ((const EnumItem *) b)->order);
}

static int
enum_by_oid(const void *a, const void *b)
{
	return SIFT_CMP3(((const EnumItem *) a)->oid, ((const EnumItem *) b)->oid);
}

/* The values of an enum in the order of the type (pg_enum.enumsortorder, which ALTER TYPE ... ADD VALUE BEFORE/AFTER changes without touching a row), read when the scan opens */
static void
enum_ranks(TopKey *k, Oid typid)
{
	CatCList   *list = SearchSysCacheList1(ENUMTYPOIDNAME, ObjectIdGetDatum(typid));
	EnumItem   *it = palloc(Max(list->n_members, 1) * sizeof(EnumItem));

	for (int i = 0; i < list->n_members; i++)
	{
		Form_pg_enum e = (Form_pg_enum) GETSTRUCT(&list->members[i]->tuple);

		it[i] = (EnumItem) {.oid = e->oid, .order = e->enumsortorder};
	}
	qsort(it, list->n_members, sizeof(EnumItem), enum_by_order);
	for (int i = 0; i < list->n_members; i++)
		it[i].rank = i;
	qsort(it, list->n_members, sizeof(EnumItem), enum_by_oid);
	k->nenum = list->n_members;
	k->eoid = palloc(Max(k->nenum, 1) * sizeof(Oid));
	k->erank = palloc(Max(k->nenum, 1) * sizeof(uint32));
	for (uint32 i = 0; i < k->nenum; i++)
	{
		k->eoid[i] = it[i].oid;
		k->erank[i] = it[i].rank;
	}
	pfree(it);
	ReleaseCatCacheList(list);
}

/* the heap attribute of a leaf that is a plain column of the table, 0 when it is not */
static int
leaf_var_attno(const SiftSrcLayout *lay, int leaf)
{
	const SiftSrcLeaf *lf = &lay->leaf[leaf];
	Node	   *e;

	if (lf->path != NIL || lf->parent >= 0 || lf->wide || lf->attno < 1 || lf->attno > list_length(lay->attexprs))
		return 0;
	e = list_nth(lay->attexprs, lf->attno - 1);
	return IsA(e, Var) && ((Var *) e)->varattno > 0 ? ((Var *) e)->varattno : 0;
}

int
sift_topn_text_attno(const SiftSrcLayout *lay, int leaf)
{
	return lay->leaf[leaf].typid == TEXTOID || lay->leaf[leaf].typid == VARCHAROID ? leaf_var_attno(lay, leaf) : 0;
}

int
sift_topn_leaf_attno(const SiftSrcLayout *lay, int leaf)
{
	switch (lay->leaf[leaf].typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return leaf_var_attno(lay, leaf);
		default:
			return 0;
	}
}

/* the integers compare as the numbers they are, whatever the width; a date or a time as its own count */
bool
sift_topn_same_scale(Oid a, Oid b)
{
	return a == b || ((a == INT2OID || a == INT4OID || a == INT8OID) && (b == INT2OID || b == INT4OID || b == INT8OID));
}

/* Can the global order of the first key stand for the scan, the others being read from the heap rows: plain integer and time columns of a heap */
static bool
order_gok(SiftTopN *t, const SiftSrcLayout *lay, Relation heap)
{
	if (t->nkey > TOP_GKEYS || lay->is_table || heap->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return false;
	for (int j = 0; j < t->nkey; j++)
	{
		const TopKey *k = &t->key[j];

		if (k->leaf < 0 || k->xk != TXK_NONE || k->x != NULL || k->nenum || (t->gatt[j] = k->blob ? leaf_var_attno(lay, k->leaf) : sift_topn_leaf_attno(lay, k->leaf)) == 0)
			return false;
		t->gtyp[j] = lay->leaf[k->leaf].typid;
		if (k->blob)
		{
			/* the one key of a text column (text, varchar): its strings are ranked in the arrays under the collation of the query's order */
			if (t->nkey != 1 || k->bkind != BK_TEXT || !OidIsValid(k->collid) || (t->gtyp[0] != TEXTOID && t->gtyp[0] != VARCHAROID))
				return false;
			t->gtext = true;
		}
	}
	return true;
}

SiftTopN *
sift_topn_open(Node *spec, const SiftSrcLayout *lay, SiftSrcSource *src, Relation heap, Snapshot snapshot, ExprContext *econtext, PlanState *parent, MemoryContext cxt, int widen)
{
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftTopN   *t = palloc0(sizeof(SiftTopN));
	List	   *l = SPEC_KEYS(spec);
	int			i = 0;
	bool		bounded;
	int64		offset;
	int			flags;

	t->nkey = list_nth_int(l, i++);
	t->key = palloc(t->nkey * sizeof(TopKey));
	for (int j = 0; j < t->nkey; j++)
	{
		int			leaf = list_nth_int(l, i++),
					fl = list_nth_int(l, i++);
		Oid			sortop = (Oid) list_nth_int(l, i++),
					cmp = (Oid) list_nth_int(l, i++),
					collid = (Oid) list_nth_int(l, i++);
		TopKey	   *k = &t->key[j];
		int			xk = list_nth_int(l, i++);
		uint32		xlo = (uint32) list_nth_int(l, i++),
					xhi = (uint32) list_nth_int(l, i++);
		Oid			xcoll = (Oid) list_nth_int(l, i++);

		*k = (TopKey) {.leaf = leaf, .desc = fl & 1, .nulls_first = (fl & 2) != 0, .vtype = leaf >= 0 ? lay->leaf[leaf].vtype : expr_vtype(xcoll),
			.xk = xk, .xc = (int64) (((uint64) xhi << 32) | xlo), .xcoll = xcoll};
		if (leaf >= 0 && xk == TXK_NONE && type_is_enum(lay->leaf[leaf].typid))
			enum_ranks(k, lay->leaf[leaf].typid);
		if (xk == TXK_EXPR)
		{
			k->x = expr_open(list_nth(SPEC_EXPRS(spec), (int) xlo), lay, xcoll, econtext, cxt);
			t->nexpr++;
		}
		if (xk == TXK_LOWER || xk == TXK_UPPER)
		{
			k->xfn = palloc0(sizeof(FmgrInfo));
			fmgr_info(xk == TXK_LOWER ? F_LOWER_TEXT : F_UPPER_TEXT, k->xfn);
		}
		if (k->vtype == SCV_CODE)
		{
			k->blob = true;
			k->bkind = cmp == F_BTTEXTCMP ? BK_TEXT : cmp == F_UUID_CMP ? BK_UUID : cmp == F_BYTEACMP ? BK_BYTEA : BK_FMGR;
			k->typid = k->bkind == BK_TEXT ? TEXTOID : leaf >= 0 ? lay->leaf[leaf].typid : xcoll;
			k->sortop = sortop;
			k->collid = collid;
			if (k->bkind == BK_FMGR)
			{
				k->cmp = palloc0(sizeof(FmgrInfo));
				fmgr_info(cmp, k->cmp);
				k->typlen = get_typlen(k->typid);
			}
			t->nblob++;
		}
	}
	if (t->nblob > 0 || t->nexpr > 0)
		t->ucxt = AllocSetContextCreate(cxt, "sift top-N unit", ALLOCSET_DEFAULT_SIZES);
	if (t->nblob > 0)
		t->oc = palloc0(ORD_CACHE * sizeof(OrdCode));
	/* the zones of the next keys tell, for a first key that ties, which superblocks can still hold a row before the n-th: they bound the walk in the order of the keys */
	for (t->wkeys = 1; sift_topn_zone_keys && t->wkeys < t->nkey && t->key[t->wkeys].leaf >= 0 && (t->key[t->wkeys].vtype == SCV_I32 || t->key[t->wkeys].vtype == SCV_I64); t->wkeys++)
		;
	t->nout = list_nth_int(l, i++);
	t->attno = palloc(Max(t->nout, 1) * sizeof(int));
	t->oleaf = palloc(Max(t->nout, 1) * sizeof(int));
	for (int c = 0; c < t->nout; c++)
	{
		t->attno[c] = list_nth_int(l, i++);
		t->oleaf[c] = list_nth_int(l, i++);
	}
	bounded = bound_rows(spec, econtext, parent, &t->gwant, &offset);
	t->limit = bounded ? t->gwant : 0;
	/* a heap entry holds SIFT_TOPN_MAX_KEYS keys: more keys sort every row, as no bound does */
	flags = list_nth_int(l, i++);
	if ((flags & TOPN_SORT) || (t->nkey > sift_limit(topn_keys, SIFT_TOPN_MAX_KEYS) && t->limit > 0))
		bounded = false;
	if (bounded && t->limit > 0 && (flags & TOPN_TIES))
	{
		int64		n = (int64) (t->limit + (flags >> TOPN_SLACK_SHIFT)) << (4 * widen);

		t->want = t->limit;
		if (widen < TOPN_WIDEN_MAX && n <= sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS))
			t->limit = (int) n;
		else
			bounded = false;
	}
	if (flags & TOPN_SKIP_OFFSET)
		t->skip = offset;
	t->all = !bounded;
	t->totals = (flags & TOPN_TOTALS) != 0;
	t->nogord = (flags & TOPN_NOGORD) != 0;
	if (flags & TOPN_RAWKEY)
		sift_colsrc_set_raw(src, t->key[0].leaf);
	if (t->all && t->nblob > 0)
		t->sort = sort_begin(t);		/* byte strings do not fit the array of integers: the tuplesort holds them, in memory up to work_mem */
	else if (t->all)
	{
		t->stride = t->nkey + (t->nkey + 63) / 64 + 1;
		t->mem = palloc(1024 * t->stride * sizeof(int64));
		t->capmem = 1024;
	}
	else if (t->limit > 0)
	{
		t->heap = palloc(t->limit * sizeof(TopEnt));
		t->lh = palloc(t->limit * sizeof(TopEnt));
		t->rows = palloc(t->limit * sizeof(uint32));
		t->tids = palloc(t->limit * sizeof(ItemPointerData));
		/* every column of the output is a leaf (the plan says so): the units give them, a heap row is fetched only for the entries they could not */
		if (t->nout > 0 && t->oleaf[0] >= 0 && heap->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			t->nov = t->nout;
			t->otyp = palloc(t->nov * sizeof(Oid));
			t->owide = palloc(t->nov * sizeof(bool));
			t->okey = palloc(t->nov * sizeof(int));
			for (int c = 0; c < t->nov; c++)
			{
				t->otyp[c] = lay->leaf[t->oleaf[c]].typid;
				t->owide[c] = lay->leaf[t->oleaf[c]].vtype == SCV_I64;
				t->okey[c] = -1;
				for (int j = 0; j < t->nkey && t->okey[c] < 0; j++)
				{
					const TopKey *k = &t->key[j];

					if (k->leaf == t->oleaf[c] && k->xk == TXK_NONE && k->x == NULL && !k->blob && !k->nenum && (k->vtype == SCV_I32 || k->vtype == SCV_I64))
						t->okey[c] = j;
				}
				t->nread += t->okey[c] < 0;
			}
			t->ov = palloc((Size) t->limit * t->nov * sizeof(int64));
			t->ovn = palloc((Size) t->limit * t->nov);
			t->uv = palloc((Size) t->limit * t->nov * sizeof(int64));
			t->uvn = palloc((Size) t->limit * t->nov);
		}
	}
	t->gok = order_gok(t, lay, heap);
	t->snap = snapshot;
	t->src = src;
	t->rel = heap;
	t->nfetch = heap->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ? sift_aggpart_count(src) : 1;
	t->pf = palloc0(t->nfetch * sizeof(TopFetch));
	if (heap->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		fetch_begin(t, 0);
	MemoryContextSwitchTo(old);
	return t;
}

void
sift_topn_close(SiftTopN *t)
{
	for (int i = 0; i < t->nfetch; i++)
	{
		if (t->pf[i].slot)
			ExecDropSingleTupleTableSlot(t->pf[i].slot);
		if (t->pf[i].fetch)
			table_index_fetch_end(t->pf[i].fetch);
	}
	if (t->sort)
	{
		ExecDropSingleTupleTableSlot(t->sin);
		ExecDropSingleTupleTableSlot(t->sout);
		tuplesort_end(t->sort);
	}
	if (t->ucxt)
		MemoryContextDelete(t->ucxt);
	if (t->ocxt)
		MemoryContextDelete(t->ocxt);
}

bool
sift_topn_empty(const SiftTopN *t)
{
	return !t->all && t->limit == 0;
}

int
sift_topn_leaf(const SiftTopN *t, int j)
{
	return t->key[j].leaf;
}

int64
sift_topn_skipped(const SiftTopN *t)
{
	return t->skipped;
}

int64
sift_topn_fetched(const SiftTopN *t)
{
	return t->fetched;
}

/* ---- sort expressions (TXK_EXPR) ---- */

/*
 * The scan reads the global order of the key (one entry for every row ever indexed, in the order of the key) in place of the units: the rows of the heap come in
 * the order of the key and the n-th one ends the scan. An entry is for the heap to confirm, as a b-tree's is: the row of the address may be dead, or another one
 * where the first was vacuumed, which holds another key.
 */

/* ---- parallel scan: the participants keep a heap each, the leader merges them ---- */

