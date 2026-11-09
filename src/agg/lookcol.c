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
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
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
#include "agg/lookcol_int.h"
#include "agg/plan_int.h"
#include "core/hash.h"


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
		case TIMEOID:
			return lf->vtype == SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return lf->vtype == SCV_F64;
	}
	return false;
}

/* the leaf that holds the whole of column att, or of the expression expr, whatever its type: not a path of a jsonb value */
static const SiftSrcLeaf *
column_leaf_any(const SiftSrcLayout *lay, int att, Node *expr)
{
	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &lay->leaf[i];
		Node	   *e;

		if (lf->parent >= 0 || lf->kind != SCL_SCALAR || lf->path != NIL || lf->lossy || lf->wide || lf->astext)
			continue;
		e = list_nth(lay->attexprs, lf->attno - 1);
		if (e != NULL && (expr ? equal(e, expr) : IsA(e, Var) && ((Var *) e)->varattno == att))
			return lf;
	}
	return NULL;
}

/* ... and of the type typid, as the load decodes it */
static const SiftSrcLeaf *
column_leaf(const SiftSrcLayout *lay, int att, Node *expr, Oid typid)
{
	const SiftSrcLeaf *lf = column_leaf_any(lay, att, expr);

	return lf != NULL && leaf_of_type(lf, typid) ? lf : NULL;
}

/* the value of a cell of a leaf that is not text, as 64 bits: the integer, or the bits of a float */
int64
lookcol_scalar_value(const ColNeed *nd, int64 i)
{
	if (nd->lf->vtype == SCV_F64)
	{
		int64		bits;

		memcpy(&bits, &((const double *) nd->vec.data)[i], sizeof(bits));
		return bits;
	}
	return sift_src_int(nd->vec.data, nd->lf->vtype, i);
}

/* the Datum of a value of lookcol_scalar_value */
Datum
lookcol_scalar_datum(const SiftSrcLeaf *lf, int64 v)
{
	if (lf->vtype == SCV_F64)
	{
		double		d;

		memcpy(&d, &v, sizeof(d));
		return lf->typid == FLOAT4OID ? Float4GetDatum((float4) d) : Float8GetDatum(d);
	}
	if (lf->typid == NUMERICOID)
		return v == PG_INT64_MAX ? DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)) :
			NumericGetDatum(int64_div_fast_to_numeric(v, lf->scale));
	return leaf_datum(lf->typid, v);
}

Datum
lookcol_cell_datum(ColNeed *nd, int64 i, bool *isnull)
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
	return lookcol_scalar_datum(lf, lookcol_scalar_value(nd, i));
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
	if (j->colexpr[c] != NULL)
		return;
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
				ok = cmp_int(sift_src_int(v->data, nd->lf->vtype, i), t->op, t->iv);
			keep |= (uint64) ok << b;
		}
		rows[w] &= keep;
	}
}

/* adds the column att of the table to need unless it is there; a column that is read for the join as well is not a feeder */
static bool
need_att(ColNeed *need, int *nneed, int att, bool feeder)
{
	for (int p = 0; p < *nneed; p++)
		if (need[p].expr == NULL && need[p].att == att)
		{
			need[p].feeder &= feeder;
			return true;
		}
	if (*nneed == MAX_NEED)
		return false;
	memset(&need[*nneed], 0, sizeof(ColNeed));
	need[*nneed].att = att;
	need[*nneed].xcol = -1;
	need[(*nneed)++].feeder = feeder;
	return true;
}

/*
 * The columns the load reads from the leaves of the index with layout lay, each with its leaf: the keys and the columns, and for an expression the leaf that holds it,
 * or, when the index has none, the leaves of the columns it reads and the expression to compute (comp). False when the index lacks one.
 */
static bool
index_needs(const SiftSrcLayout *lay, TupleDesc desc, const SiftAggJoin *j, ColNeed *need, int *nneed, CompCol *comp, int *ncomp)
{
	bool		ok = true;

	*nneed = *ncomp = 0;
	for (int i = 0; i < j->nkey + j->ncol && ok; i++)
	{
		Node	   *expr = i < j->nkey ? NULL : j->colexpr[i - j->nkey];

		if (expr == NULL)
			ok = need_att(need, nneed, i < j->nkey ? j->keyatt[i] : j->colatt[i - j->nkey], false);
		else if (column_leaf(lay, 0, expr, exprType(expr)) != NULL)
		{
			if ((ok = *nneed < MAX_NEED))
			{
				memset(&need[*nneed], 0, sizeof(ColNeed));
				need[*nneed].expr = expr;
				need[(*nneed)++].xcol = i - j->nkey;
			}
		}
		else
			comp[(*ncomp)++] = (CompCol) {expr, i - j->nkey, NULL};
	}
	for (int k = 0; k < *ncomp && ok; k++)
		foreach_ptr(Var, u, pull_var_clause(comp[k].expr, 0))
			ok = ok && u->varattno > 0 && need_att(need, nneed, u->varattno, true);
	for (int p = 0; p < *nneed && ok; p++)
		ok = (need[p].lf = column_leaf(lay, need[p].att, need[p].expr, need[p].expr ? exprType(need[p].expr) :
									   TupleDescAttr(desc, need[p].att - 1)->atttypid)) != NULL;
	return ok;
}

/*
 * A valid sift_col index of the whole table with a leaf for each column the load reads (map_load_cols), NULL when the heap is read; need and nneed are the columns,
 * comp the expressions to compute from them.
 */
Relation
lookcol_load_index(Relation rel, const SiftAggJoin *j, bool own, ColNeed *need, int *nneed, CompCol *comp, int *ncomp)
{
	*nneed = *ncomp = 0;
	if (!sift_agg_lookup_columns || rel->rd_rel->relkind != RELKIND_RELATION || rel->rd_tableam != GetHeapamTableAmRoutine() || (j->nidx > 0 && !own) || j->first.qual != NIL ||
		j->first.nord > 0)
		return NULL;
	for (int k = 0; k < j->nkey; k++)
		if (j->tkey[k] != NULL)
			return NULL;
	foreach_oid(ixoid, RelationGetIndexList(rel))
	{
		Relation	ix = index_open(ixoid, AccessShareLock);

		if (sift_colsrc_is_index(ix->rd_rel->relam) && ix->rd_index->indisvalid && ix->rd_index->indisready &&
			heap_attisnull(ix->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			index_needs(sift_colsrc_layout(ix), RelationGetDescr(rel), j, need, nneed, comp, ncomp))
			return ix;
		index_close(ix, AccessShareLock);
	}
	return NULL;
}

/* will the load of the table read the leaves of an index (the planner prices it by that)? *nnumeric: how many of the columns it makes a numeric of, one allocation a row each */
/*
 * Why the rows of the table are read from the heap though a sift_col index of it exists: the first column of the join that the first such index does not hold as a leaf the load decodes,
 * named with its type; NULL when no sift_col index fits the other conditions of the load (EXPLAIN VERBOSE shows it).
 */
char *
map_cols_why(Relation rel, const SiftAggJoin *j)
{
	foreach_oid(ixoid, RelationGetIndexList(rel))
	{
		Relation	ix = index_open(ixoid, AccessShareLock);
		char	   *why = NULL;

		if (sift_colsrc_is_index(ix->rd_rel->relam) && ix->rd_index->indisvalid && ix->rd_index->indisready &&
			heap_attisnull(ix->rd_indextuple, Anum_pg_index_indpred, NULL))
		{
			const SiftSrcLayout *lay = sift_colsrc_layout(ix);

			for (int i = 0; i < j->nkey + j->ncol && why == NULL; i++)
			{
				int			att = i < j->nkey ? j->keyatt[i] : j->colatt[i - j->nkey];
				Oid			typ = TupleDescAttr(RelationGetDescr(rel), att - 1)->atttypid;
				const SiftSrcLeaf *lf;

				if (i >= j->nkey && j->colexpr[i - j->nkey] != NULL)
					continue;
				lf = column_leaf_any(lay, att, NULL);
				if (lf == NULL || !leaf_of_type(lf, typ))
					why = psprintf("column %s (%s) is %s of index %s", NameStr(TupleDescAttr(RelationGetDescr(rel), att - 1)->attname), format_type_be(typ),
								   lf == NULL ? "not a plain leaf (a wide type, a lossy leaf or a path)" : "a leaf of a type the load does not decode", RelationGetRelationName(ix));
			}
		}
		index_close(ix, AccessShareLock);
		if (why != NULL)
			return why;
	}
	return NULL;
}

bool
map_load_cols_possible(Relation rel, const SiftAggJoin *j, int *nnumeric)
{
	ColNeed		need[MAX_NEED];
	CompCol		comp[SIFT_AGG_MAX_JCOLS];
	int			nneed,
				ncomp;
	Relation	ix = lookcol_load_index(rel, j, j->fslot >= 0, need, &nneed, comp, &ncomp);

	if (ix == NULL)
		return false;
	*nnumeric = 0;
	for (int p = 0; p < nneed; p++)
		*nnumeric += need[p].lf->typid == NUMERICOID;
	index_close(ix, AccessShareLock);
	return true;
}

/*
 * The units of the index a load reads, one at a time, as the scan of the index sees them: the superblocks [lo, hi) that sc->claim hands out one range after
 * another (NULL: all of them once). false (fn never called for the range) when a unit holds a value a leaf cannot store. fn runs in the context the reader
 * was called in; the leaves are decoded in a context that ends with the unit.
 */
bool
lookcol_read_units(const UnitScope *sc, Relation rel, Relation ix, ColNeed *need, int nneed, UnitFn fn, void *arg)
{
	SiftSrcSource *src;
	SiftSrcUnit unit;
	MemoryContext ucx,
				ux,
				old = CurrentMemoryContext;
	uint32		lo = 0,
				hi = PG_UINT32_MAX;
	bool		ok = true,
				more = true;

	ucx = AllocSetContextCreate(old, "lookup columns", ALLOCSET_DEFAULT_SIZES);
	ux = AllocSetContextCreate(ucx, "lookup columns of a unit", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(ucx);
	src = sift_colsrc_open(rel, ix, sc->snap, ucx, sc->view);
	while (ok && more)
	{
		if (sc->claim == NULL)
			more = false;
		else if (!sc->claim(sc->carg, sift_colsrc_nsb(src), &lo, &hi))
			break;
		sift_colsrc_begin(src, lo, hi, NULL, 0);
		while (ok && sift_colsrc_next(src, &unit))
			for (int p = 0; p < nneed && ok; p++)
				ok = !sift_colsrc_overflow(src, &unit, need[p].lf->id);
		if (!ok)
			break;
		sift_colsrc_begin(src, lo, hi, NULL, 0);
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
					nd->dids = MemoryContextAllocZero(ux, nd->dn * sizeof(uint32));
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

/* the columns of the join that are expressions, evaluated over the slot of the row into v/isnull by column of the join */
static void
comp_eval(CompCol *comp, int ncomp, ExprContext *ectx, TupleTableSlot *slot, Datum *v, bool *isnull)
{
	ResetExprContext(ectx);
	ectx->ecxt_scantuple = slot;
	for (int k = 0; k < ncomp; k++)
		v[comp[k].xcol] = ExecEvalExprSwitchContext(comp[k].st, ectx, &isnull[comp[k].xcol]);
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
	CompCol		comp[SIFT_AGG_MAX_JCOLS];
	int			ncomp;
	ExprContext *ectx;
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
				if (need[p].xcol >= 0)
					cl->m->xv[need[p].xcol] = lookcol_cell_datum(&need[p], i, &cl->m->xn[need[p].xcol]);
				else
					cl->slot->tts_values[need[p].att - 1] = lookcol_cell_datum(&need[p], i, &cl->slot->tts_isnull[need[p].att - 1]);
			if (cl->ncomp > 0)
				comp_eval(cl->comp, cl->ncomp, cl->ectx, cl->slot, cl->m->xv, cl->m->xn);
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
	ColNeed		need[MAX_NEED];
	int			nneed;
	Relation	ix;
	TupleDesc	desc = RelationGetDescr(rel);
	ColLoad		cl = {.ex = ex,.m = m,.j = j,.cb = cb,.own = own};
	bool		ok;

	if ((ix = lookcol_load_index(rel, j, own != NULL, need, &nneed, cl.comp, &cl.ncomp)) == NULL)
		return false;
	for (int k = 0; k < cl.ncomp; k++)
		cl.comp[k].st = ExecInitExpr((Expr *) cl.comp[k].expr, NULL);
	cl.ectx = cl.ncomp > 0 ? CreateStandaloneExprContext() : NULL;
	if (own != NULL && j->fslot >= 0)
		collect_tests((Node *) cb->slot[j->fslot].expr, j, cb->nleaves, need, nneed, cl.test, &cl.ntest, &cl.ntotal);
	cl.slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	memset(cl.slot->tts_isnull, true, desc->natts * sizeof(bool));
	ExecStoreVirtualTuple(cl.slot);
	m->xfed = true;
	ok = lookcol_read_units(&(UnitScope) {.snap = ex->snap}, rel, ix, need, nneed, load_unit, &cl);
	m->xfed = false;
	ExecDropSingleTupleTableSlot(cl.slot);
	if (cl.ectx)
		FreeExprContext(cl.ectx, true);
	index_close(ix, AccessShareLock);
	ex->lookup_cols += ok;
	return ok;
}
