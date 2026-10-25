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
#include "access/relation.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/agg_int.h"
#include "agg/colsrc.h"
#include "agg/leaf_int.h"

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
		return (typid == TEXTOID || typid == VARCHAROID) && (lf->typid == TEXTOID || lf->typid == VARCHAROID);
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

			nd->dval[c] = PointerGetDatum(cstring_to_text_with_len(v->dict->bytes + v->dict->off[c], v->dict->off[c + 1] - v->dict->off[c]));
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

static Node *
unrelabel(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

static bool
int_family(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

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
	l = unrelabel(linitial(op->args));
	r = unrelabel(lsecond(op->args));
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
			x->text = (pt == TEXTOID || pt == VARCHAROID);
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
			else if (int_family(pt) ? !int_family(cn->consttype) : (cn->consttype != pt || (pt != DATEOID && pt != TIMESTAMPOID && pt != TIMESTAMPTZOID)))
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
		for (uint32 c = 0; c < v->dict->n; c++)
			if (v->dict->off[c + 1] - v->dict->off[c] == (uint32) t->tlen && memcmp(v->dict->bytes + v->dict->off[c], t->tv, t->tlen) == 0)
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

/*
 * Loads the rows of the table into m from the leaves of an index of it, as the scan of the heap would: false (nothing loaded) when the load
 * is not one of rows by plain columns, when no index holds the columns, or when a unit holds a value a leaf cannot store.
 */
bool
map_load_cols(AggExec *ex, MapTab *m, const SiftAggJoin *j, const SiftAggCombo *cb, ExprState *own, Relation rel)
{
	ColNeed		need[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_JCOLS];
	int			nneed = 0;
	Relation	ix;
	SiftSrcSource *src;
	SiftSrcUnit unit;
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot;
	MemoryContext ucx,
				ux,
				old;
	bool		ok = true;
	int			natts = desc->natts;
	ColTest		test[MAX_TESTS];
	int			ntest = 0,
				ntotal = 0;

	if (!sift_agg_lookup_columns || rel->rd_rel->relkind != RELKIND_RELATION || rel->rd_tableam != GetHeapamTableAmRoutine() || (j->nidx > 0 && own == NULL) || j->first.qual != NIL ||
		j->first.nord > 0)
		return false;
	for (int k = 0; k < j->nkey; k++)
		if (j->tkey[k] != NULL)
			return false;
	for (int i = 0; i < j->nkey + j->ncol; i++)
	{
		int			att = i < j->nkey ? j->keyatt[i] : j->colatt[i - j->nkey];
		bool		seen = false;

		for (int p = 0; p < nneed; p++)
			seen |= need[p].att == att;
		if (!seen)
		{
			memset(&need[nneed], 0, sizeof(ColNeed));
			need[nneed++].att = att;
		}
	}
	if ((ix = covering_index(rel, need, nneed)) == NULL)
		return false;
	if (own != NULL && j->fslot >= 0)
		collect_tests((Node *) cb->slot[j->fslot].expr, j, cb->nleaves, need, nneed, test, &ntest, &ntotal);
	ucx = AllocSetContextCreate(CurrentMemoryContext, "lookup columns", ALLOCSET_DEFAULT_SIZES);
	ux = AllocSetContextCreate(ucx, "lookup columns of a unit", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(ucx);
	src = sift_colsrc_open(rel, ix, ex->snap, ucx);
	sift_colsrc_begin(src, 0, PG_UINT32_MAX, NULL, 0);
	while (ok && sift_colsrc_next(src, &unit))
		for (int p = 0; p < nneed && ok; p++)
			ok = !sift_colsrc_overflow(src, &unit, need[p].lf->id);
	if (ok)
	{
		slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
		memset(slot->tts_isnull, true, natts * sizeof(bool));
		ExecStoreVirtualTuple(slot);
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
			for (int t = 0; t < ntest; t++)
				apply_test(&test[t], &need[test[t].need], unit.nrow, rows);
			for (int w = 0; w < nw; w++)
				for (uint64 bits = rows[w]; bits != 0; bits &= bits - 1)
				{
					int64		i = (int64) w * 64 + __builtin_ctzll(bits);

					for (int p = 0; p < nneed; p++)
						slot->tts_values[need[p].att - 1] = cell_datum(&need[p], i, &slot->tts_isnull[need[p].att - 1]);
					MemoryContextSwitchTo(old);
					map_row(ex, m, slot, j, cb, ntest == ntotal ? NULL : own);
					MemoryContextSwitchTo(ucx);
				}
			CHECK_FOR_INTERRUPTS();
		}
		ExecDropSingleTupleTableSlot(slot);
	}
	sift_colsrc_close(src);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(ucx);
	index_close(ix, AccessShareLock);
	ex->lookup_cols += ok;
	return ok;
}
