/*
 * agg/pred.c - row and element predicates evaluated to bitmaps
 */
#include "postgres.h"

#include "utils/float.h"

#include "agg/wide.h"
#include "agg/pred_in_int.h"
#include "agg/unit_int.h"

/* the row that owns each element of the array leaf in the unit */
static const uint32 *
array_parent(AggExec *ex, int leaf)
{
	if (ex->gpar[leaf] == NULL)
	{
		const uint32 *offs = elem_offs(ex, leaf);

		ex->gpar[leaf] = MemoryContextAlloc(ex->ucxt, (Size) offs[ex->unit.nrow] * sizeof(uint32) + SIFT_AGG_PAD);
		sift_colk->expand_parents(offs + 1, ex->unit.nrow, ex->gpar[leaf]);
	}
	return ex->gpar[leaf];
}

/*
 * c (a numeric) floored (up: ceilinged) to a multiple of 10^-scale, as that integer: PostgreSQL's own numeric_mul, numeric_floor and
 * numeric_ceil do the arithmetic. -1 / 1 when it is below / above every integer within lim, which is where NaN (numeric order puts it
 * above every number), the infinities and the digits the scaled values cannot hold go.
 */
static int
scaled_bound(Datum c, int scale, bool up, sift_i128 lim, sift_i128 *out)
{
	Datum		unit = DirectFunctionCall3(numeric_in, CStringGetDatum(psprintf("1e%d", scale)), ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(-1)),
				t = DirectFunctionCall2(numeric_mul, c, unit);
	char	   *txt = DatumGetCString(DirectFunctionCall1(numeric_out, DirectFunctionCall1(up ? numeric_ceil : numeric_floor, t)));
	int			ignored;

	if (strcmp(txt, "-Infinity") == 0)
		return -1;
	if (strcmp(txt, "NaN") == 0 || strcmp(txt, "Infinity") == 0 || !sift_parse_decimal(txt, out, &ignored))
		return txt[0] == '-' ? -1 : 1;
	return *out > lim ? 1 : *out < -lim ? -1 : 0;
}

/*
 * `value op c` for integers scaled by 10^scale, c a numeric of any scale: the comparison st->nop with st->cn that holds for exactly the
 * same values (st->konst: the one result of every value). x = c needs c to be a multiple of the unit, x < c is x < ceil(c), x <= c is
 * x <= floor(c), x > c is x > floor(c), x >= c is x >= ceil(c).
 */
static void
numeric_operand(PredState *st, int op, int scale, sift_i128 lim, Datum c)
{
	sift_i128	fl,
				ce,
				v;
	int			sf = scaled_bound(c, scale, false, lim, &fl),
				sc = scaled_bound(c, scale, true, lim, &ce),
				side;

	st->konst = -1;
	switch (op)
	{
		case SCMP_EQ:
		case SCMP_NE:
			if (sf == 0 && sc == 0 && fl == ce)
			{
				st->nop = op;
				st->cn = fl;
			}
			else
				st->konst = op == SCMP_NE;
			return;
		case SCMP_LT:
		case SCMP_GE:
			side = sc;
			v = ce;
			break;
		default:
			side = sf;
			v = fl;
	}
	if (side)
		st->konst = (op == SCMP_LT || op == SCMP_LE) == (side > 0);
	else
	{
		st->nop = op;
		st->cn = v;
	}
}

/* does `NaN op c` hold, c a NaN or a number: NaN is greater than every number and equal to itself */
static bool
nan_compares(int op, bool cnan)
{
	switch (op)
	{
		case SCMP_EQ:
			return cnan;
		case SCMP_NE:
			return !cnan;
		case SCMP_LT:
			return false;
		case SCMP_LE:
			return cnan;
		case SCMP_GT:
			return !cnan;
		default:
			return true;
	}
}

void
eval_operand(AggExec *ex, SiftAggPred *p, PredState *st)
{
	bool		isnull;
	Datum		d;
	ExprState  *e;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	e = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
	d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
	st->cnull = isnull;
	if (p->kind == SAP_GATE)
		st->konst = !isnull && DatumGetBool(d);
	else if ((p->kind == SAP_CMP || p->kind == SAP_NCMP) && !isnull)
	{
		if (p->argtype == NUMERICOID)
		{
			st->nan_res = nan_compares(p->op, numeric_is_nan(DatumGetNumeric(d)));
			if (p->kind == SAP_NCMP)
				numeric_operand(st, p->op, p->num.scale, sift_pow10(SIFT_NUM_MAX_DIGITS), d);
			else
				numeric_operand(st, p->op, ex->lay->leaf[p->leaf].scale, PG_INT64_MAX, d);
		}
		else if (p->argtype == FLOAT4OID)
			st->cd = DatumGetFloat4(d);
		else if (p->argtype == FLOAT8OID)
			st->cd = DatumGetFloat8(d);
		else
			st->ci = sift_aggspec_operand_int(p->argtype, d);
	}
	else if (p->kind == SAP_IN && !isnull)
	{
		ArrayType  *arr = DatumGetArrayTypeP(d);
		Datum	   *el;
		bool	   *nl;
		int			ne,
					nv = 0;
		int16		len;
		bool		byval;
		char		align;
		int64		lo = PG_INT64_MAX,
					hi = PG_INT64_MIN;
		int64	   *vals;
		bool		wide = ex->lay->leaf[p->leaf].vtype == SCV_I64;

		get_typlenbyvalalign(p->argtype, &len, &byval, &align);
		deconstruct_array(arr, p->argtype, len, byval, align, &el, &nl, &ne);
		st->in_empty = ne == 0;
		vals = palloc(Max(ne, 1) * sizeof(int64));
		for (int i = 0; i < ne; i++)
		{
			int64		v;

			if (nl[i])
			{
				st->in_hasnull = true;
				continue;
			}
			if (p->argtype == NUMERICOID)
			{
				/* the element as the scaled integer of the leaf, if a stored value can equal it */
				if (numeric_is_nan(DatumGetNumeric(el[i])))
				{
					st->in_hasnan = true;
					continue;
				}
				numeric_operand(st, SCMP_EQ, ex->lay->leaf[p->leaf].scale, PG_INT64_MAX, el[i]);
				if (st->konst >= 0)
					continue;
				v = (int64) st->cn;
			}
			else
				v = sift_aggspec_operand_int(p->argtype, el[i]);
			if (!wide && (v < PG_INT32_MIN || v > PG_INT32_MAX))
				continue;		/* no int4 value equals it */
			vals[nv++] = v;
			lo = Min(lo, v);
			hi = Max(hi, v);
		}
		in_build(st, vals, nv, lo, hi);
		st->konst = -1;
	}
	MemoryContextSwitchTo(old);
	st->ready = true;
}

/* rows whose leaf value is NULL are not in out */
void
clear_nulls(AggExec *ex, const SiftSrcVec *v, int start, int n, uint64 *out)
{
	if (v->isnull)
	{
		sift_colk_nonzero(v->isnull + start, n, ex->tmpm);
		sift_colk->bits_andnot(out, ex->tmpm, (n + 63) / 64);
	}
}

/*
 * bit i of out = (v[i] op c) for an int4 or int8 column (slot SIFT_COLW_32 / SIFT_COLW_64), c within the type: the values the
 * comparison keeps are one range, [lo, hi] wrapping (x <> c is [c + 1, c - 1]), which is what the column kernels test
 */
static void
cmp_int(uint64 *out, const void *v, int slot, int n, SiftCmpOp op, int64 c)
{
	const uint64 wmax = slot == SIFT_COLW_32 ? PG_UINT32_MAX : PG_UINT64_MAX;
	const int64 tmin = slot == SIFT_COLW_32 ? PG_INT32_MIN : PG_INT64_MIN,
				tmax = slot == SIFT_COLW_32 ? PG_INT32_MAX : PG_INT64_MAX;
	uint64		lo,
				hi;

	switch (op)
	{
		case SCMP_EQ:
			lo = hi = c;
			break;
		case SCMP_NE:
			lo = (uint64) c + 1;
			hi = (uint64) c - 1;
			break;
		case SCMP_LT:
			lo = tmin;
			hi = (uint64) c - 1;
			break;
		case SCMP_LE:
			lo = tmin;
			hi = c;
			break;
		case SCMP_GT:
			lo = (uint64) c + 1;
			hi = tmax;
			break;
		default:
			lo = c;
			hi = tmax;
	}
	if ((op == SCMP_LT && c == tmin) || (op == SCMP_GT && c == tmax))
		clear_words(out, n);
	else
		sift_colk->range[slot](v, n, lo & wmax, (hi - lo) & wmax, out);
}

/* the rows of `rows` (n bits) are in out (res) or are not */
void
set_rows(const uint64 *rows, int n, bool res, uint64 *out)
{
	for (int w = 0; w < (n + 63) / 64; w++)
	{
		uint64		m = (n - w * 64 >= 64) ? rows[w] : rows[w] & ((UINT64CONST(1) << (n - w * 64)) - 1);

		out[w] = res ? out[w] | m : out[w] & ~m;
	}
}

static void
eval_cmp(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);
	const SiftSrcLeaf *lf = &ex->lay->leaf[p->leaf];
	SiftCmpOp	op = (SiftCmpOp) p->op;

	if (st->cnull)
	{
		clear_words(out, n);
		return;
	}
	switch (lf->vtype)
	{
		case SCV_I32:
			{
				int64		c = st->ci;

				if (c > PG_INT32_MAX || c < PG_INT32_MIN)
				{
					/* beyond int32 the comparison is the same for every value */
					bool		above = c > PG_INT32_MAX,
								res;

					switch (op)
					{
						case SCMP_EQ:
							res = false;
							break;
						case SCMP_NE:
							res = true;
							break;
						case SCMP_LT:
						case SCMP_LE:
							res = above;
							break;
						default:
							res = !above;
					}
					if (res)
						fill_ones(out, n);
					else
						clear_words(out, n);
				}
				else
					cmp_int(out, (const int32 *) v->data + start, SIFT_COLW_32, n, op, c);
			}
			break;
		case SCV_I64:
			if (lf->typid != NUMERICOID)
				cmp_int(out, (const int64 *) v->data + start, SIFT_COLW_64, n, op, st->ci);
			else if (st->konst >= 0)
			{
				if (st->konst)
					fill_ones(out, n);
				else
					clear_words(out, n);
			}
			else
				cmp_int(out, (const int64 *) v->data + start, SIFT_COLW_64, n, (SiftCmpOp) st->nop, (int64) st->cn);
			break;
		case SCV_F64:
			sift_aggk->cmp_f64(out, (const double *) v->data + start, n, op, st->cd);
			break;
		default:
			SIFT_INVARIANT(false, "sift: comparison on a dictionary leaf");
	}
	if (v->ovf && lf->typid == NUMERICOID)
		set_rows(v->ovf + start / 64, n, st->nan_res, out);
	clear_nulls(ex, v, start, n, out);
}

/* bit (sign + 1) of the mask of a SiftCmpOp: the comparison holds when the three-way result has that sign */
static const uint8 cmp_signs[] = {2, 5, 1, 3, 4, 6};

/* the program's values against the scaled operand, the NULL rows out */
static void
eval_ncmp(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out)
{
	const sift_i128 *x;

	clear_words(out, n);
	if (st->cnull || (st->konst == 0 && !st->nan_res))
		return;
	for (int i = 0; i < n; i++)
		ex->pix[i] = start + i;
	x = eval_nx(ex, &p->num, n, ex->pix, ex->pix, ex->nb, ex->alive);
	for (int i = 0; i < n; i++)
		if (!ex->nb[i] && (x[i] == SIFT_NX_NAN ? st->nan_res : st->konst == 1 || (st->konst < 0 && (cmp_signs[st->nop] >> ((x[i] > st->cn) - (x[i] < st->cn) + 1) & 1))))
			out[i >> 6] |= UINT64CONST(1) << (i & 63);
}

static inline int64
vec_int(const void *data, SiftSrcVecType t, int64 i)
{
	return t == SCV_I32 ? ((const int32 *) data)[i] : ((const int64 *) data)[i];
}

/* PostgreSQL's order of float8: NaN equals NaN and is greater than every number, -0 equals 0 */
static inline int
float_order(double a, double b)
{
	return isnan(a) ? !isnan(b) : isnan(b) ? -1 : (a > b) - (a < b);
}

/* two columns of one row: the cmp2 kernel of the vector type, the NULL rows of either out */
static void
eval_leaf_cmp(AggExec *ex, SiftAggPred *p, int start, int n, uint64 *out)
{
	const SiftSrcVec *a = get_vec(ex, p->leaf),
			   *b = get_vec(ex, p->leaf2);

	switch (ex->lay->leaf[p->leaf].vtype)
	{
		case SCV_I32:
			sift_aggk->cmp2_i32(out, (const int32 *) a->data + start, (const int32 *) b->data + start, n, (SiftCmpOp) p->op);
			break;
		case SCV_I64:
			sift_aggk->cmp2_i64(out, (const int64 *) a->data + start, (const int64 *) b->data + start, n, (SiftCmpOp) p->op);
			break;
		default:
			sift_aggk->cmp2_f64(out, (const double *) a->data + start, (const double *) b->data + start, n, (SiftCmpOp) p->op);
	}
	clear_nulls(ex, a, start, n, out);
	clear_nulls(ex, b, start, n, out);
}

/* an element leaf against a leaf of its row: the value of the row for each element is gathered by the owner of the element, NULL on either side is not true */
static void
eval_elem_row(AggExec *ex, SiftAggPred *p, int start, int n, uint64 *out)
{
	const SiftSrcVec *ev = get_vec(ex, p->leaf),
			   *rv = get_vec(ex, p->leaf2);
	const SiftSrcLeaf *el = &ex->lay->leaf[p->leaf],
			   *rl = &ex->lay->leaf[p->leaf2];
	const uint32 *par = array_parent(ex, el->parent) + start;
	uint8		sign = cmp_signs[p->op];

	clear_words(out, n);
	for (int i = 0; i < n; i++)
	{
		int			c;

		if ((ev->isnull && ev->isnull[start + i]) || (rv->isnull && rv->isnull[par[i]]))
			continue;
		if (el->vtype == SCV_F64)
			c = float_order(((const double *) ev->data)[start + i], ((const double *) rv->data)[par[i]]);
		else
		{
			int64		a = vec_int(ev->data, el->vtype, start + i),
						b = vec_int(rv->data, rl->vtype, par[i]);

			c = (a > b) - (a < b);
		}
		out[i >> 6] |= (uint64) (sign >> (c + 1) & 1) << (i & 63);
	}
}

/* out = ~out, then the NULL rows out */
void
negate(AggExec *ex, uint64 *out, int n)
{
	uint64	   *all = ex->tmpm2;

	fill_ones(all, n);
	sift_colk->bits_andnot(all, out, (n + 63) / 64);
	memcpy(out, all, ((n + 63) / 64) * sizeof(uint64));
}

/* the JSON nulls of the leaf's NULL slots in the unit, read once (SNULL_JSON, SNULL_MISSING) */
static const uint64 *
jnull_bits(AggExec *ex, SiftAggPred *p, int nslots)
{
	PredState  *st = &ex->ps[p->id];

	if (!st->rowmask_ok)
	{
		st->rowmask = MemoryContextAllocZero(ex->ucxt, ((nslots + 63) / 64 + PADW) * sizeof(uint64));
		sift_colsrc_read_jnull(ex->src, &ex->unit, p->leaf, st->rowmask);
		st->rowmask_ok = true;
	}
	return st->rowmask;
}

static void
eval_null(AggExec *ex, SiftAggPred *p, int start, int n, uint64 *out)
{
	const SiftSrcVec *v = get_vec(ex, p->leaf);

	if (p->op >= SNULL_JSON)
	{
		const uint64 *jn = jnull_bits(ex, p, v->n) + start / 64;

		memcpy(out, jn, ((n + 63) / 64) * sizeof(uint64));
		if (p->op == SNULL_MISSING && v->isnull != NULL)
		{
			uint64		nul[VECW + PADW];

			sift_colk_nonzero(v->isnull + start, n, nul);
			sift_colk->bits_andnot(nul, out, (n + 63) / 64);
			memcpy(out, nul, ((n + 63) / 64) * sizeof(uint64));
		}
		else if (p->op == SNULL_MISSING)
			clear_words(out, n);
		return;
	}
	if (v->isnull == NULL)
	{
		if (p->op)
			fill_ones(out, n);
		else
			clear_words(out, n);
		return;
	}
	sift_colk_nonzero(v->isnull + start, n, out);
	if (p->op)
		negate(ex, out, n);
}

/* boolean slot p->op of a combo without a join, over [start, start + n) of the elements (qlevel 1) or rows; only the items alive in ex->alive are evaluated */
static void
eval_combo(AggExec *ex, SiftAggPred *p, int qlevel, int start, int n, uint64 *out)
{
	ComboSt    *cs = &ex->cs[p->leaf];

	const uint32 *par = qlevel ? unit_parent(ex) : NULL;

	for (int i = 0; i < n; i++)
	{
		ex->pti[i] = start + i;
		ex->pri[i] = qlevel ? par[start + i] : start + i;
	}
	combo_resolve(ex, p->leaf, qlevel, n, ex->pti, ex->pri, ex->peid, ex->alive, NULL);
	clear_words(out, n);
	for (int i = 0; i < n; i++)
		if (ex->peid[i] != COMBO_NONE && combo_val(ex, p->leaf, cs->ent[ex->peid[i]].base, p->op))
			out[i >> 6] |= UINT64CONST(1) << (i & 63);
}

/*
 * The rows (nrow bits in out) that own an element whose bit is set in eb, the elements of row r being [offs[r], offs[r + 1]). eb has a
 * word of room past its elements. A row owns few elements: the 56 bits of eb from its first element hold them all (the few longer rows
 * take the slow way).
 */
void
rows_of_elems(const uint32 *offs, int nrow, const uint64 *eb, uint64 *out)
{
	memset(out, 0, ((nrow + 63) / 64) * sizeof(uint64));
	for (int r = 0; r < nrow; r++)
	{
		uint32		lo = offs[r],
					len = offs[r + 1] - lo;
		uint64		x;

		memcpy(&x, (const char *) eb + (lo >> 3), sizeof(x));
		x = (x >> (lo & 7)) & ((UINT64CONST(1) << Min(len, 56)) - 1);
		if (len > 56)
			x |= last_bit(eb, lo, lo + len) >= 0;
		out[r >> 6] |= (uint64) (x != 0) << (r & 63);
	}
}

/*
 * A row of a heap is at the TID of the root of its HOT chain in the index, and its ctid is the visible version's, on the same page: the rows of the
 * blocks inside the range pass, those of its first and last block are fetched for the version the snapshot sees (none: the row is not the scan's).
 */
static bool
tid_row_in(AggExec *ex, ItemPointerData *tid)
{
	BlockNumber blk = ItemPointerGetBlockNumberNoCheck(tid);
	uint64		key;

	if (blk < ex->tid_blo || blk > ex->tid_bhi)
		return false;
	if (blk > ex->tid_blo && blk < ex->tid_bhi)
		return true;
	if (ex->tid_vis && !sift_vis_fetch(ex->tid_vis, tid, ex->tid_slot))
		return false;
	key = (uint64) ItemPointerGetBlockNumberNoCheck(tid) << 16 | ItemPointerGetOffsetNumberNoCheck(tid);
	return key >= ex->tid_lo && key < ex->tid_hi;
}

/* SAP_TID on the rows [start, start + n) of the unit: the first conjunct tests the range of all, a unit inside it needs no TID */
static void
eval_tid(AggExec *ex, SiftAggPred *p, int start, int n, uint64 *out)
{
	uint64		first = (uint64) ex->unit.sb * SIFT_SB_PAGES << 16,
				last = first + ((uint64) SIFT_SB_PAGES << 16) - 1;
	if (p->id != ex->tid_pred || (ex->sb_heap && ex->tid_lo <= first && last < ex->tid_hi))
	{
		fill_ones(out, n);
		return;
	}
	clear_words(out, n);
	if (ex->sb_heap && (ex->tid_hi <= first || ex->tid_lo > last))
		return;
	for (int i = 0; i < n; i++)
		ex->tid_rows[i] = start + i;
	sift_colsrc_tids(ex->src, &ex->unit, ex->tid_rows, n, ex->tid_tids);
	for (int i = 0; i < n; i++)
		out[i >> 6] |= (uint64) tid_row_in(ex, &ex->tid_tids[i]) << (i & 63);
}

/* SAP_ANY_ELEM on the unit's rows: the element predicate on every element of the unit, reduced to the rows that own a passing one */
static void
any_elem_rows(AggExec *ex, SiftAggPred *p, uint64 *out)
{
	const uint32 *offs = elem_offs(ex, p->leaf);
	int			nrow = ex->unit.nrow,
				nw = (nrow + 63) / 64;
	uint32		m = offs[nrow];
	uint64	   *eb = MemoryContextAllocZero(ex->ucxt, ((m + 63) / 64 + PADW + 1) * sizeof(uint64));
	uint64		t[VECW + PADW];

	for (uint32 s = 0; s < m; s += VEC)
	{
		int			n = Min(VEC, m - s);

		eval_pred(ex, p->child[0], 1, s, n, t);
		memcpy(eb + s / 64, t, ((n + 63) / 64) * sizeof(uint64));
	}
	rows_of_elems(offs, nrow, eb, out);
	if (p->op)
	{
		for (int w = 0; w < nw; w++)
			out[w] = ~out[w];
		if (nrow & 63)
			out[nw - 1] &= (UINT64CONST(1) << (nrow & 63)) - 1;
	}
	/* a list that raises when SQL reads it has no elements to test: its row goes on to the error, raised where the rows that keep it reach the aggregates */
	if (p->pass_poison)
	{
		uint64	   *bad = NULL;

		leaf_overflow_rows(ex, p->leaf, &bad);
		for (int w = 0; bad && w < nw; w++)
			out[w] |= bad[w];
	}
}

/* the row's list raises when SQL reads it (the source recorded the error): the test raises it where it reaches the row, as the sub-plan does */
static void
raise_poison(AggExec *ex, const SiftAggPred *p, uint32 r)
{
	PredState  *st = &ex->ps[p->id];

	if (!st->poison_ok)
	{
		st->poison = NULL;
		leaf_overflow_rows(ex, p->leaf2, &st->poison);
		st->poison_ok = true;
	}
	if (st->poison && sift_colsrc_bit(st->poison, r))
	{
		uint64	   *one = MemoryContextAllocZero(ex->ucxt, ((ex->unit.nrow + 63) / 64 + PADW) * sizeof(uint64));

		one[r >> 6] |= UINT64CONST(1) << (r & 63);
		sift_colsrc_overflow_raise(ex->src, &ex->unit, one);
	}
}

/*
 * SAP_ELEM_EXISTS for tuple j: some element of the array of its row passes the element conjuncts (child[0]) and boolean slot `op` of the scoped
 * combo `leaf`, whose inputs are the element's leaves, the row's and the lookup outputs of the tuple. The expressions cannot raise (the planner
 * keeps those that may), so the elements go through the combo VEC at a time, up to the first batch with one that passes; the combo resolves each
 * distinct input once (its lookup runs once for it). The answer is kept for the next tuples of the same row and lookup rows, which are the tuples
 * a join or an unnest repeats. pass_poison: a row whose list raises raises here.
 */
bool
elem_exists(AggExec *ex, const Tup *t, int j, const SiftAggPred *p)
{
	const SiftAggCombo *cb = &ex->spec->combo[p->leaf];
	ComboSt    *cs = &ex->cs[p->leaf];
	PredState  *st = &ex->ps[p->id];
	const uint32 *offs = elem_offs(ex, p->leaf2);
	uint32		r = t->ridx[j],
				ref[SIFT_AGG_MAX_CLEAVES] = {0};
	Tup			xt = {.cout = ex->xcout};

	for (int k = 0; k < cb->nleaves; k++)
		if (cb->leaf[k] < 0)
			ref[k] = t->cout[cb->refcombo[k]][j];
	if (st->rowmask_ok && st->memo_row == r && memcmp(ref, st->memo_ref, sizeof(ref)) == 0)
		return st->memo_res;
	if (p->pass_poison)
		raise_poison(ex, p, r);
	st->memo_res = false;
	for (uint32 s = offs[r]; s < offs[r + 1] && !st->memo_res; s += VEC)
	{
		int			n = Min(VEC, offs[r + 1] - s);
		uint64		pass[VECW + PADW];

		if (p->nchild > 0)
			eval_pred(ex, p->child[0], 1, s, n, pass);
		else
			fill_ones(pass, n);
		for (int i = 0; i < n; i++)
		{
			ex->pti[i] = s + i;
			ex->pri[i] = r;
			for (int k = 0; k < cb->nleaves; k++)
				if (cb->leaf[k] < 0)
					ex->xcout[cb->refcombo[k]][i] = ref[k];
		}
		combo_resolve(ex, p->leaf, 1, n, ex->pti, ex->pri, ex->peid, pass, &xt);
		for (int i = 0; i < n && !st->memo_res; i++)
			if (ex->peid[i] != COMBO_NONE)
				for (uint32 o = 0; o < cs->ent[ex->peid[i]].nout && !st->memo_res; o++)
					st->memo_res = combo_val(ex, p->leaf, cs->ent[ex->peid[i]].base + o, p->op) != 0;
	}
	st->rowmask_ok = true;
	st->memo_row = r;
	memcpy(st->memo_ref, ref, sizeof(ref));
	return st->memo_res;
}

/*
 * A cast of the leaf (SRK_CAST, SRK_IOCAST) in a condition of the rows or in the FILTER of an aggregate: a row whose value SQL cannot convert passes
 * the test and goes on, and raises where the rows that the conditions keep reach the aggregates (check_overflow)
 */
static void
or_poison(AggExec *ex, const SiftAggPred *p, int start, int n, uint64 *out)
{
	PredState  *st = &ex->ps[p->id];

	if (!st->poison_ok)
	{
		st->poison = NULL;
		leaf_overflow_rows(ex, p->leaf, &st->poison);
		st->poison_ok = true;
	}
	for (int w = 0; st->poison && w < (n + 63) / 64; w++)
		out[w] |= st->poison[start / 64 + w];
}

/* the bitmap of a row level predicate over the unit's rows, computed once */
uint64 *
row_bitmap(AggExec *ex, SiftAggPred *p)
{
	PredState  *st = &ex->ps[p->id];

	if (!st->rowmask_ok)
	{
		int			nrow = ex->unit.nrow;

		st->rowmask = MemoryContextAlloc(ex->ucxt, ((nrow + 63) / 64 + PADW) * sizeof(uint64));
		if (p->kind == SAP_ANY_ELEM)
			any_elem_rows(ex, p, st->rowmask);
		else
			for (int s = 0; s < nrow; s += VEC)
				eval_pred(ex, p, 0, s, Min(VEC, nrow - s), st->rowmask + s / 64);
		st->rowmask_ok = true;
	}
	return st->rowmask;
}

void
eval_pred(AggExec *ex, SiftAggPred *p, int qlevel, int start, int n, uint64 *out)
{
	PredState  *st = &ex->ps[p->id];
	int			nw = (n + 63) / 64;

	if (p->level < qlevel && !p->late && p->kind != SAP_TRUE && p->kind != SAP_FALSE && p->kind != SAP_GATE)
	{
		/* a row level predicate seen from the elements */
		sift_aggk->gather_bits(out, row_bitmap(ex, p), unit_parent(ex) + start, n);
		return;
	}
	if (!st->ready && (p->kind == SAP_CMP || p->kind == SAP_IN || p->kind == SAP_NCMP || p->kind == SAP_GATE))
		eval_operand(ex, p, st);
	switch (p->kind)
	{
		case SAP_TRUE:
			fill_ones(out, n);
			break;
		case SAP_GATE:
			if (st->konst)
				fill_ones(out, n);
			else
				clear_words(out, n);
			break;
		case SAP_FALSE:
			clear_words(out, n);
			break;
		case SAP_AND:
		case SAP_OR:
			{
				uint64		tmp[VECW + PADW],
							live[VECW + PADW];
				const uint64 *alive0 = ex->alive;

				eval_pred(ex, p->child[0], qlevel, start, n, out);
				for (int i = 1; i < p->nchild; i++)
				{
					/* AND takes the next argument only for the items still true, OR for the items still false (an argument may raise an error) */
					if (p->late)
					{
						if (alive0)
							memcpy(live, alive0, nw * sizeof(uint64));
						else
							fill_ones(live, n);
						if (p->kind == SAP_AND)
							sift_colk->bits_and(live, out, nw);
						else
							sift_colk->bits_andnot(live, out, nw);
						ex->alive = live;
					}
					eval_pred(ex, p->child[i], qlevel, start, n, tmp);
					ex->alive = alive0;
					if (p->kind == SAP_AND)
						sift_colk->bits_and(out, tmp, nw);
					else
						sift_colk->bits_or(out, tmp, nw);
				}
			}
			break;
		case SAP_CMP:
			eval_cmp(ex, p, st, start, n, out);
			break;
		case SAP_NCMP:
			eval_ncmp(ex, p, st, start, n, out);
			break;
		case SAP_IN:
			eval_in(ex, p, st, start, n, out);
			break;
		case SAP_NULL:
			eval_null(ex, p, start, n, out);
			break;
		case SAP_LUT:
			eval_lut(ex, p, st, start, n, out);
			break;
		case SAP_COMBO:
			eval_combo(ex, p, qlevel, start, n, out);
			break;
		case SAP_ANY_ELEM:
			memcpy(out, row_bitmap(ex, p) + start / 64, nw * sizeof(uint64));
			break;
		case SAP_LEAFCMP:
			eval_leaf_cmp(ex, p, start, n, out);
			break;
		case SAP_ELEM_ROW:
			eval_elem_row(ex, p, start, n, out);
			break;
		case SAP_TID:
			eval_tid(ex, p, start, n, out);
			break;
		case SAP_MATCHED:
		case SAP_TYPE:
		case SAP_WIDE:
		case SAP_ELEM_EXISTS:
			SIFT_INVARIANT(false, "sift: the types and the wide values of a leaf are tested by the source, an element existence test by the joined tuples");
	}
	if (p->pass_poison && (p->kind == SAP_CMP || p->kind == SAP_IN || p->kind == SAP_NULL || p->kind == SAP_LUT))
		or_poison(ex, p, start, n, out);
}
