/*
 * agg/lookcol.c - the rows of a lookup table, read from the leaves of one of its sift_col indexes
 *
 * The load of a lookup table reads the heap: a tuple is formed for each row of the table, its conditions are evaluated on it and the rows that
 * pass are hashed. When a sift_col index of the table holds every column the load reads (the keys, the columns the expressions read) as plain
 * leaves, the same rows come from the units of the index: the leaves are decoded a unit at a time, the rows the source says are live and visible
 * (sift_colsrc_live, sift_colsrc_vet, as the scan of the index sees them) get a virtual tuple of the columns, and the rest of the load, the
 * conditions of the table and the hash, is the one of a heap row.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/agg_int.h"
#include "agg/leaf_int.h"
#include "agg/plan_int.h"
#include "core/hash.h"

typedef struct ColNeed
{
	int			att;			/* the column of the table */
	const SiftSrcLeaf *lf;
	SiftSrcVec	vec;
	Datum	   *dval;			/* a text leaf: the value of each entry of the unit's dictionary made so far */
	bool	   *dmade;
	uint32		dn;
	MemoryContext cx;			/* the unit's: reset with it */
} ColNeed;

static bool
leaf_of_type(const SiftSrcLeaf *lf, Oid typid)
{
	if (lf->vtype == SCV_CODE)
		return sift_str_type(typid) && sift_str_type(lf->typid);
	if (lf->typid != typid)
		return false;
	if (typid == NUMERICOID)
		return lf->vtype == SCV_I64;
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return lf->vtype == SCV_I32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return lf->vtype == SCV_I64;
	}
	return false;
}

/* the leaf that holds the whole of column att, as a value of its type: a column of the index that is not an expression, not a path of a jsonb value */
static const SiftSrcLeaf *
column_leaf(const SiftSrcLayout *lay, int att, Oid typid)
{
	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &lay->leaf[i];
		Node	   *e;

		if (lf->parent >= 0 || lf->kind != SCL_SCALAR || lf->path != NIL || lf->lossy || lf->wide || lf->astext)
			continue;
		e = list_nth(lay->attexprs, lf->attno - 1);
		if (e != NULL && IsA(e, Var) && ((Var *) e)->varattno == att)
			return leaf_of_type(lf, typid) ? lf : NULL;
	}
	return NULL;
}

/* a valid sift_col index of the whole table with a leaf for each column of need, those leaves in need[].lf; NULL when no index has them all */
static Relation
covering_index(Relation rel, ColNeed *need, int nneed)
{
	foreach_oid(ixoid, RelationGetIndexList(rel))
	{
		Relation	ix = index_open(ixoid, AccessShareLock);
		bool		ok = sift_colsrc_is_index(ix->rd_rel->relam) && ix->rd_index->indisvalid && ix->rd_index->indisready &&
			heap_attisnull(ix->rd_indextuple, Anum_pg_index_indpred, NULL);

		if (ok)
		{
			SiftSrcLayout *lay = sift_colsrc_layout(ix);

			for (int i = 0; i < nneed && ok; i++)
				ok = (need[i].lf = column_leaf(lay, need[i].att, TupleDescAttr(RelationGetDescr(rel), need[i].att - 1)->atttypid)) != NULL;
		}
		if (ok)
			return ix;
		index_close(ix, AccessShareLock);
	}
	return NULL;
}

static Datum
cell_datum(ColNeed *nd, int64 i, bool *isnull)
{
	const SiftSrcVec *v = &nd->vec;
	const SiftSrcLeaf *lf = nd->lf;

	if (v->isnull != NULL && v->isnull[i])
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;
	if (lf->vtype == SCV_CODE)
	{
		uint32		c = ((const uint32 *) v->data)[i];

		if (!nd->dmade[c])
		{
			MemoryContext old = MemoryContextSwitchTo(nd->cx);

			char		buf[SIFT_SRC_DICT_ENTRY];
			uint32		len;
			const char *str = sift_srcdict_str(v->dict, c, &len, buf);

			nd->dval[c] = PointerGetDatum(cstring_to_text_with_len(str, len));
			nd->dmade[c] = true;
			MemoryContextSwitchTo(old);
		}
		return nd->dval[c];
	}
	{
		int64		iv = lf->vtype == SCV_I32 ? ((const int32 *) v->data)[i] : ((const int64 *) v->data)[i];

		if (lf->typid == NUMERICOID)
			return iv == PG_INT64_MAX ? DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)) :
				NumericGetDatum(int64_div_fast_to_numeric(iv, lf->scale));
		return leaf_datum(lf->typid, iv);
	}
}

/* a conjunct of the table's own conditions the load tests on the decoded leaf before it makes a tuple: `column <op> constant` of an integer, date or timestamp column, or text equality */
typedef struct ColTest
{
	int			need;			/* the column, in need[] */
	int			op;				/* TST_* */
	bool		text;
	int64		iv;
	const char *tv;
	int			tlen;
} ColTest;

#define TST_LT		0
#define TST_LE		1
#define TST_EQ		2
#define TST_GE		3
#define TST_GT		4
#define TST_NE		5
#define MAX_TESTS	8

/* the operator <, <=, =, >=, > or <> of the catalog, as a TST_*; -1 for any other */
static int
builtin_op(Oid opno)
{
	HeapTuple	ht = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	int			op = -1;

	if (HeapTupleIsValid(ht))
	{
		Form_pg_operator o = (Form_pg_operator) GETSTRUCT(ht);
		const char *nm = NameStr(o->oprname);

		if (o->oprnamespace == PG_CATALOG_NAMESPACE)
			op = strcmp(nm, "<") == 0 ? TST_LT : strcmp(nm, "<=") == 0 ? TST_LE : strcmp(nm, "=") == 0 ? TST_EQ :
				strcmp(nm, ">=") == 0 ? TST_GE : strcmp(nm, ">") == 0 ? TST_GT : strcmp(nm, "<>") == 0 ? TST_NE : -1;
		ReleaseSysCache(ht);
	}
	return op;
}

static int64
const_int(Const *c)
{
	switch (c->consttype)
	{
		case INT2OID:
			return DatumGetInt16(c->constvalue);
		case INT4OID:
			return DatumGetInt32(c->constvalue);
		case DATEOID:
			return DatumGetDateADT(c->constvalue);
		default:
			return DatumGetInt64(c->constvalue);
	}
}

/*
 * The conjuncts of the expression that a test on the leaf stands for exactly: a comparison of a column of the table (a Param past the leaves
 * of the combo) with a constant. *ntotal counts the conjuncts, so that the load knows when the tests are the whole condition.
 */
static void
collect_tests(Node *n, const SiftAggJoin *j, int nleaves, const ColNeed *need, int nneed, ColTest *t, int *nt, int *ntotal)
{
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Param	   *pm;
	Const	   *cn;
	int			o,
				c;

	if (n == NULL)
		return;
	if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == AND_EXPR)
	{
		foreach_ptr(Node, a, ((BoolExpr *) n)->args)
			collect_tests(a, j, nleaves, need, nneed, t, nt, ntotal);
		return;
	}
	(*ntotal)++;
	if (!IsA(n, OpExpr) || list_length(((OpExpr *) n)->args) != 2 || *nt >= MAX_TESTS)
		return;
	op = (OpExpr *) n;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (IsA(l, Param) && IsA(r, Const))
	{
		pm = (Param *) l;
		cn = (Const *) r;
		o = builtin_op(op->opno);
	}
	else if (IsA(r, Param) && IsA(l, Const))
	{
		pm = (Param *) r;
		cn = (Const *) l;
		o = builtin_op(op->opno);
		o = o == TST_LT ? TST_GT : o == TST_LE ? TST_GE : o == TST_GT ? TST_LT : o == TST_GE ? TST_LE : o;
	}
	else
		return;
	if (o < 0 || pm->paramkind != PARAM_EXEC || pm->paramid < nleaves || pm->paramid - nleaves >= j->ncol || cn->constisnull)
		return;
	c = pm->paramid - nleaves;
	for (int p = 0; p < nneed; p++)
		if (need[p].att == j->colatt[c])
		{
			Oid			pt = j->coltype[c];
			ColTest    *x = &t[*nt];

			x->need = p;
			x->op = o;
			x->text = sift_str_type(pt);
			if (x->text)
			{
				struct varlena *v;

				if ((cn->consttype != TEXTOID && cn->consttype != VARCHAROID) || (o != TST_EQ && o != TST_NE) || op->inputcollid == InvalidOid ||
					!get_collation_isdeterministic(op->inputcollid))
					return;
				v = PG_DETOAST_DATUM_PACKED(cn->constvalue);
				x->tv = VARDATA_ANY(v);
				x->tlen = VARSIZE_ANY_EXHDR(v);
			}
			else if (sift_int_type(pt) ? !sift_int_type(cn->consttype) : (cn->consttype != pt || (pt != DATEOID && pt != TIMESTAMPOID && pt != TIMESTAMPTZOID)))
				return;
			else
				x->iv = const_int(cn);
			(*nt)++;
			return;
		}
}

static inline bool
cmp_int(int64 a, int op, int64 b)
{
	switch (op)
	{
		case TST_LT:
			return a < b;
		case TST_LE:
			return a <= b;
		case TST_EQ:
			return a == b;
		case TST_GE:
			return a >= b;
		case TST_GT:
			return a > b;
		default:
			return a != b;
	}
}

/* keeps of rows the rows of the unit that pass the test (a NULL never does) */
static void
apply_test(const ColTest *t, const ColNeed *nd, int nrow, uint64 *rows)
{
	const SiftSrcVec *v = &nd->vec;
	int			nw = (nrow + 63) / 64;
	bool		wide = nd->lf->vtype == SCV_I64;
	uint32		code = PG_UINT32_MAX;

	if (t->text)
	{
		const SiftSrcDict *d = sift_srcdict_whole(v->dict);

		for (uint32 c = 0; c < d->n; c++)
			if (d->off[c + 1] - d->off[c] == (uint32) t->tlen && memcmp(d->bytes + d->off[c], t->tv, t->tlen) == 0)
			{
				code = c;
				break;
			}
	}
	for (int w = 0; w < nw; w++)
	{
		uint64		keep = 0;
		int			lim = Min(64, nrow - w * 64);

		if (rows[w] == 0)
			continue;
		for (int b = 0; b < lim; b++)
		{
			int64		i = (int64) w * 64 + b;
			bool		ok;

			if (v->isnull != NULL && v->isnull[i])
				continue;
			if (t->text)
				ok = (((const uint32 *) v->data)[i] == code) == (t->op == TST_EQ);
			else
				ok = cmp_int(wide ? ((const int64 *) v->data)[i] : ((const int32 *) v->data)[i], t->op, t->iv);
			keep |= (uint64) ok << b;
		}
		rows[w] &= keep;
	}
}

/* the index the load of the table reads when the join's columns are all in one (map_load_cols), NULL when the heap is read; need and nneed are the columns */
static Relation
load_index(Relation rel, const SiftAggJoin *j, bool own, ColNeed *need, int *nneed)
{
	*nneed = 0;
	if (!sift_agg_lookup_columns || rel->rd_rel->relkind != RELKIND_RELATION || rel->rd_tableam != GetHeapamTableAmRoutine() || (j->nidx > 0 && !own) || j->first.qual != NIL ||
		j->first.nord > 0)
		return NULL;
	for (int k = 0; k < j->nkey; k++)
		if (j->tkey[k] != NULL)
			return NULL;
	for (int i = 0; i < j->nkey + j->ncol; i++)
	{
		int			att = i < j->nkey ? j->keyatt[i] : j->colatt[i - j->nkey];
		bool		seen = false;

		for (int p = 0; p < *nneed; p++)
			seen |= need[p].att == att;
		if (!seen)
		{
			memset(&need[*nneed], 0, sizeof(ColNeed));
			need[(*nneed)++].att = att;
		}
	}
	return covering_index(rel, need, *nneed);
}

/* will the load of the table read the leaves of an index (the planner prices it by that)? *nnumeric: how many of the columns it makes a numeric of, one allocation a row each */
bool
map_load_cols_possible(Relation rel, const SiftAggJoin *j, int *nnumeric)
{
	ColNeed		need[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS];
	int			nneed;
	Relation	ix = load_index(rel, j, j->fslot >= 0, need, &nneed);

	if (ix == NULL)
		return false;
	*nnumeric = 0;
	for (int p = 0; p < nneed; p++)
		*nnumeric += need[p].lf->typid == NUMERICOID;
	index_close(ix, AccessShareLock);
	return true;
}

/* what a unit reader hands to its callback: the live and visible rows of the unit (bit i of rows) and the decoded leaves of need[]; ux lives for the unit */
typedef void (*UnitFn) (void *arg, const SiftSrcUnit *unit, ColNeed *need, int nneed, uint64 *rows);

/*
 * The units of the index a load reads, one at a time, as the scan of the index sees them. false (fn never called) when a unit holds a value a leaf
 * cannot store. fn runs in the context the reader was called in; the leaves are decoded in a context that ends with the unit.
 */
static bool
read_units(AggExec *ex, Relation rel, Relation ix, ColNeed *need, int nneed, UnitFn fn, void *arg)
{
	SiftSrcSource *src;
	SiftSrcUnit unit;
	MemoryContext ucx,
				ux,
				old = CurrentMemoryContext;
	bool		ok = true;

	ucx = AllocSetContextCreate(old, "lookup columns", ALLOCSET_DEFAULT_SIZES);
	ux = AllocSetContextCreate(ucx, "lookup columns of a unit", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(ucx);
	src = sift_colsrc_open(rel, ix, ex->snap, ucx);
	sift_colsrc_begin(src, 0, PG_UINT32_MAX, NULL, 0);
	while (ok && sift_colsrc_next(src, &unit))
		for (int p = 0; p < nneed && ok; p++)
			ok = !sift_colsrc_overflow(src, &unit, need[p].lf->id);
	if (ok)
	{
		sift_colsrc_begin(src, 0, PG_UINT32_MAX, NULL, 0);
		while (sift_colsrc_next(src, &unit))
		{
			int			nw = (unit.nrow + 63) / 64;
			uint64	   *rows;

			MemoryContextReset(ux);
			rows = MemoryContextAllocZero(ux, (nw + PADW) * sizeof(uint64));

			sift_colsrc_live(src, &unit, rows);
			if (sift_colsrc_vet_needed(src, &unit))
				sift_colsrc_vet(src, &unit, rows);
			for (int p = 0; p < nneed; p++)
			{
				ColNeed    *nd = &need[p];

				sift_colsrc_read(src, &unit, nd->lf->id, &nd->vec);
				if (nd->lf->vtype == SCV_CODE)
				{
					nd->dn = nd->vec.dict->n;
					nd->cx = ux;
					nd->dval = MemoryContextAlloc(ux, nd->dn * sizeof(Datum));
					nd->dmade = MemoryContextAllocZero(ux, nd->dn);
				}
			}
			MemoryContextSwitchTo(old);
			fn(arg, &unit, need, nneed, rows);
			MemoryContextSwitchTo(ucx);
			CHECK_FOR_INTERRUPTS();
		}
	}
	sift_colsrc_close(src);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(ucx);
	return ok;
}

typedef struct ColLoad
{
	AggExec    *ex;
	MapTab	   *m;
	const SiftAggJoin *j;
	const SiftAggCombo *cb;
	ExprState  *own;
	TupleTableSlot *slot;
	ColTest		test[MAX_TESTS];
	int			ntest;
	int			ntotal;
} ColLoad;

static void
load_unit(void *arg, const SiftSrcUnit *unit, ColNeed *need, int nneed, uint64 *rows)
{
	ColLoad    *cl = arg;
	int			nw = (unit->nrow + 63) / 64;

	for (int t = 0; t < cl->ntest; t++)
		apply_test(&cl->test[t], &need[cl->test[t].need], unit->nrow, rows);
	for (int w = 0; w < nw; w++)
		for (uint64 bits = rows[w]; bits != 0; bits &= bits - 1)
		{
			int64		i = (int64) w * 64 + __builtin_ctzll(bits);

			for (int p = 0; p < nneed; p++)
				cl->slot->tts_values[need[p].att - 1] = cell_datum(&need[p], i, &cl->slot->tts_isnull[need[p].att - 1]);
			map_row(cl->ex, cl->m, cl->slot, cl->j, cl->cb, cl->ntest == cl->ntotal ? NULL : cl->own);
		}
}

/*
 * Loads the rows of the table into m from the leaves of an index of it, as the scan of the heap would: false (nothing loaded) when the load
 * is not one of rows by plain columns, when no index holds the columns, or when a unit holds a value a leaf cannot store.
 */
bool
map_load_cols(AggExec *ex, MapTab *m, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own, Relation rel)
{
	ColNeed		need[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS];
	int			nneed;
	Relation	ix;
	TupleDesc	desc = RelationGetDescr(rel);
	ColLoad		cl = {.ex = ex,.m = m,.j = j,.cb = cb,.own = own};
	bool		ok;

	if ((ix = load_index(rel, j, own != NULL, need, &nneed)) == NULL)
		return false;
	if (own != NULL && j->fslot >= 0)
		collect_tests((Node *) cb->slot[j->fslot].expr, j, cb->nleaves, need, nneed, cl.test, &cl.ntest, &cl.ntotal);
	cl.slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	memset(cl.slot->tts_isnull, true, desc->natts * sizeof(bool));
	ExecStoreVirtualTuple(cl.slot);
	ok = read_units(ex, rel, ix, need, nneed, load_unit, &cl);
	ExecDropSingleTupleTableSlot(cl.slot);
	index_close(ix, AccessShareLock);
	ex->lookup_cols += ok;
	return ok;
}

/*
 * A table of one int4 key whose other columns take few distinct tuples of values (a flag, a small code, a mask of what is present) is not
 * kept row by row: each key is given the class of its tuple, a table over the keys, and the map holds one row for each class, the first
 * it met. A join resolves a tuple by its key's class (combo/resolve.c), so the work of a row of the scan is an array read where a hash
 * lookup, a cache entry and an evaluation of the expressions were made for each distinct key. Exact: the columns of a key are those of its
 * class, a key twice in the table (a join that repeats rows) is not a class and keeps the table whole.
 */
typedef struct DenseLoad
{
	int			key;			/* the key's column, in need[] */
	int			ncol;
	int			col[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS];	/* the other columns, in need[] */
	uint16	   *lut;			/* the class (1..) of each key, 0 none */
	uint32		cap;
	uint32		n;				/* the keys the table spans: the largest + 1 */
	uint32		limit;			/* the most keys the table may span */
	uint64		rows;
	uint32		ncls;
	int64	   *tv;				/* [class * ncol + c] the value of column c of the class */
	uint32	   *tnull;			/* per class: bit c set when column c is NULL */
	int32	   *ckey;			/* per class: the first key that has it */
	uint16	   *ctab;			/* the class in a hash slot, 0 free */
	bool		bad;			/* a duplicate key, a key out of range or too many classes */
	MemoryContext cxt;
} DenseLoad;

#define DENSE_SLOTS		(4 * DENSE_MAX_CLASSES)

static inline int64
dense_value(const ColNeed *nd, int64 i)
{
	return nd->lf->vtype == SCV_I32 ? ((const int32 *) nd->vec.data)[i] : ((const int64 *) nd->vec.data)[i];
}

/* the class of the tuple of row i, 0 when there are too many */
static uint32
dense_class(DenseLoad *d, const ColNeed *need, int64 i, int32 key)
{
	int64		tv[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS];
	uint32		nulls = 0;
	uint64		h = SIFT_FIB64;
	uint32		s;

	for (int c = 0; c < d->ncol; c++)
	{
		const ColNeed *nd = &need[d->col[c]];

		if (nd->vec.isnull != NULL && nd->vec.isnull[i])
		{
			nulls |= 1U << c;
			tv[c] = 0;
		}
		else
			tv[c] = dense_value(nd, i);
		h = sift_fold64(h, (uint64) tv[c]);
		h ^= h >> 32;
	}
	h = sift_fib64(h ^ nulls);
	for (s = (uint32) (h >> 32) & (DENSE_SLOTS - 1); d->ctab[s] != 0; s = (s + 1) & (DENSE_SLOTS - 1))
	{
		uint32		k = d->ctab[s] - 1;

		if (d->tnull[k] == nulls && memcmp(&d->tv[(size_t) k * d->ncol], tv, d->ncol * sizeof(int64)) == 0)
			return k + 1;
	}
	if (d->ncls == DENSE_MAX_CLASSES)
		return 0;
	memcpy(&d->tv[(size_t) d->ncls * d->ncol], tv, d->ncol * sizeof(int64));
	d->tnull[d->ncls] = nulls;
	d->ckey[d->ncls] = key;
	d->ctab[s] = ++d->ncls;
	return d->ncls;
}

static void
dense_unit(void *arg, const SiftSrcUnit *unit, ColNeed *need, int nneed, uint64 *rows)
{
	DenseLoad  *d = arg;
	const ColNeed *kn = &need[d->key];
	int			nw = (unit->nrow + 63) / 64;

	for (int w = 0; w < nw && !d->bad; w++)
		for (uint64 bits = rows[w]; bits != 0 && !d->bad; bits &= bits - 1)
		{
			int64		i = (int64) w * 64 + __builtin_ctzll(bits);
			int32		key;
			uint32		cls;

			if (kn->vec.isnull != NULL && kn->vec.isnull[i])
				continue;
			key = ((const int32 *) kn->vec.data)[i];
			if (key < 0 || (uint32) key >= d->limit || (cls = dense_class(d, need, i, key)) == 0)
			{
				d->bad = true;
				break;
			}
			if ((uint32) key >= d->cap)
			{
				uint32		cap = Max(d->cap * 2, 1U << 16);

				while (cap <= (uint32) key)
					cap *= 2;
				d->lut = d->lut ? repalloc_huge(d->lut, (Size) cap * sizeof(uint16)) : MemoryContextAllocHuge(d->cxt, (Size) cap * sizeof(uint16));
				memset(d->lut + d->cap, 0, (Size) (cap - d->cap) * sizeof(uint16));
				d->cap = cap;
			}
			if (d->lut[key] != 0)
				d->bad = true;
			d->lut[key] = cls;
			d->n = Max(d->n, (uint32) key + 1);
			d->rows++;
		}
}

/* can the leaves of the key and the columns be classified as integers? */
static bool
dense_leaves_ok(const ColNeed *need, int nneed, int key)
{
	for (int p = 0; p < nneed; p++)
	{
		const SiftSrcLeaf *lf = need[p].lf;

		if (p == key ? !(lf->vtype == SCV_I32 && lf->typid == INT4OID) : !((lf->vtype == SCV_I32 || lf->vtype == SCV_I64) && lf->typid != NUMERICOID))
			return false;
	}
	return true;
}

/* the index of the table (open, the caller closes it) that holds the key and the columns of the join as integer leaves (map_load_dense), else NULL */
Relation
map_dense_index(Relation rel, const SiftAggJoin *j)
{
	ColNeed		need[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS];
	int			nneed,
				key = 0;
	Relation	ix = load_index(rel, j, false, need, &nneed);

	if (ix == NULL)
		return NULL;
	while (need[key].att != j->keyatt[0])
		key++;
	if (dense_leaves_ok(need, nneed, key))
		return ix;
	index_close(ix, AccessShareLock);
	return NULL;
}

bool
map_load_dense_possible(Relation rel, const SiftAggJoin *j)
{
	Relation	ix = map_dense_index(rel, j);

	if (ix == NULL)
		return false;
	index_close(ix, AccessShareLock);
	return true;
}

/*
 * Loads the table as classes of its keys (above) into m, from the leaves of an index of it. false, m unchanged, when the table is not one: no index
 * holds the columns, a column is not an integer, a key repeats or is negative or sparse, or there are more than DENSE_MAX_CLASSES tuples of values.
 */
bool
map_load_dense(AggExec *ex, MapTab *m, const SiftAggJoin *j, const SiftAggCombo *cb, Relation rel)
{
	ColNeed		need[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS];
	int			nneed;
	Relation	ix;
	DenseLoad	d = {.cxt = m->cxt,.limit = (uint32) Min((int64) sift_agg_lookup_max_kb * 1024 / sizeof(uint16), PG_INT32_MAX)};
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot;
	bool		ok;

	if ((ix = load_index(rel, j, false, need, &nneed)) == NULL)
		return false;
	for (d.key = 0; need[d.key].att != j->keyatt[0]; d.key++)
		;
	if (!dense_leaves_ok(need, nneed, d.key))
	{
		index_close(ix, AccessShareLock);
		return false;
	}
	for (int p = 0; p < nneed; p++)
		if (p != d.key && need[p].att != j->keyatt[0])
			d.col[d.ncol++] = p;
	d.tv = palloc((Size) DENSE_MAX_CLASSES * Max(d.ncol, 1) * sizeof(int64));
	d.tnull = palloc(DENSE_MAX_CLASSES * sizeof(uint32));
	d.ckey = palloc(DENSE_MAX_CLASSES * sizeof(int32));
	d.ctab = palloc0(DENSE_SLOTS * sizeof(uint16));
	ok = read_units(ex, rel, ix, need, nneed, dense_unit, &d) && !d.bad && d.rows > 0 && d.n <= 8 * d.rows + 65536;
	index_close(ix, AccessShareLock);
	if (ok)
	{
		slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
		memset(slot->tts_isnull, true, desc->natts * sizeof(bool));
		ExecStoreVirtualTuple(slot);
		for (uint32 k = 0; k < d.ncls; k++)
		{
			slot->tts_values[j->keyatt[0] - 1] = Int32GetDatum(d.ckey[k]);
			slot->tts_isnull[j->keyatt[0] - 1] = false;
			for (int c = 0; c < d.ncol; c++)
			{
				int			att = need[d.col[c]].att;

				slot->tts_isnull[att - 1] = (d.tnull[k] >> c) & 1;
				slot->tts_values[att - 1] = slot->tts_isnull[att - 1] ? (Datum) 0 : leaf_datum(need[d.col[c]].lf->typid, d.tv[(size_t) k * d.ncol + c]);
			}
			map_row(ex, m, slot, j, cb, NULL);
		}
		ExecDropSingleTupleTableSlot(slot);
		m->dcls = d.lut;
		m->dn = d.n;
		ex->lookup_cols++;
		ex->lookup_dense += d.rows;
	}
	else if (d.lut)
		pfree(d.lut);
	pfree(d.tv);
	pfree(d.tnull);
	pfree(d.ckey);
	pfree(d.ctab);
	return ok;
}
