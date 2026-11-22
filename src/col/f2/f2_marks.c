/*
 * col/f2/f2_marks.c - the key marks, the column statistics and the seeding of the filter of format major 2 (col/f2/f2_marks.h).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "col/f2/f2_marks.h"
#include "col/f2/f2_pred.h"
#include "col/granule/part_int.h"
#include "core/page_io.h"

StaticAssertDecl(SIFT_KEYCOL_DESC == SIFT_KEY_DESC && SIFT_KEYCOL_NULLS_FIRST == SIFT_KEY_NULLS_FIRST, "the key column flags are those of core/range.h");
StaticAssertDecl(SIFT_LF_EXT_REMOTE == SIFT_PART_EXT_REMOTE, "the cold part rule of the filter reads the flag of the manifest");
StaticAssertDecl(SIFT_KEY_COLS_MAX == SIFT_KEY_ORDER_MAX, "the marks keep as many columns as key_order has");

/* ---- the key of the marks ---- */

/*
 * The key a column of the rows is: the cells of INT (an integer, a date, a timestamp, a boolean, an exact numeric held as a scaled integer of its column: all ordered as int8), of F8 (a float) and
 * of STR (the 16 bytes of a uuid, or the bytes of a text, ordered as memcmp orders them, which is how the rows of a part are sorted whatever the collation of the column). A column of another kind
 * ends the key, as the rows are sorted by all the columns, so by a prefix of them. What a scan may do with a text column is the business of the predicate (sift_f2_undecided): a range or a prefix
 * is read from the marks only where the bytes are the order of the collation.
 */
static int
key_kind(SiftCellKind kind, uint8 hint)
{
	switch (kind)
	{
		case SIFT_CELL_INT:
			return SIFT_KK_INT8;
		case SIFT_CELL_F8:
			return SIFT_KK_FLOAT8;
		default:
			return (hint & ~SIFT_CHINT_ASTEXT) == SIFT_CHINT_UUID ? SIFT_KK_UUID : SIFT_KK_TEXT;
	}
}

bool
sift_f2_key_spec(const SiftCellKind *kinds, const uint8 *hints, const uint8 *flags, int n, SiftKeySpec *spec)
{
	*spec = (SiftKeySpec) {.key_bytes = SIFT_KEY_BYTES_MAX};
	for (int k = 0; k < n && k < SIFT_KEY_COLS_MAX; k++)
		spec->cols[spec->ncols++] = (SiftKeyCol) {.kind = key_kind(kinds[k], hints[k]),.flags = flags != NULL ? flags[k] : 0};
	return spec->ncols > 0;
}

bool
sift_f2_rel_key_spec(const SiftF2Rel *f, SiftKeySpec *spec)
{
	SiftCellKind kinds[SIFT_KEY_COLS_MAX];
	uint8		hints[SIFT_KEY_COLS_MAX];

	for (int k = 0; k < f->nkey; k++)
	{
		kinds[k] = f->kinds[f->key_cols[k]];
		hints[k] = f->hints[f->key_cols[k]];
	}
	return sift_f2_key_spec(kinds, hints, f->spec.sort_flags, f->nkey, spec);
}

/* ---- the writer ---- */

static SiftKeyVal
cell_val(const SiftColIn *c, int row)
{
	SiftKeyVal	v = {0};

	if (c->isnull != NULL && c->isnull[row])
	{
		v.isnull = true;
		return v;
	}
	switch (c->kind)
	{
		case SIFT_CELL_INT:
			v.i = c->i[row];
			break;
		case SIFT_CELL_F8:
			v.f = c->f[row];
			break;
		default:
			if (c->s != NULL)
			{
				v.p = (const uint8 *) c->s[row];
				v.len = c->slen[row];
			}
			else
			{
				v.p = (const uint8 *) c->pdict->p[c->dcode[row]];
				v.len = c->pdict->len[c->dcode[row]];
			}
	}
	return v;
}

/* A NaN has no place in the order of the keys of the rows (its key is below or above the infinities by its sign), and a NULL that carries a tag stands for a value the column could not hold (a number
 * that overflowed its scale): a part with one gets no marks, as the rows it holds there are candidates of every test of the column */
static bool
has_unorderable(const SiftColIn *c, int nrows)
{
	for (int r = 0; r < nrows; r++)
	{
		if (c->tag != NULL && c->tag[r] != 0)
			return true;
		if (c->kind == SIFT_CELL_F8 && (c->isnull == NULL || !c->isnull[r]) && isnan(c->f[r]))
			return true;
	}
	return false;
}

/* The order of two tuples of the key as the part sorts its rows: the column flags order the NULLs and reverse the values */
static int
tuple_cmp(const SiftKeySpec *spec, const SiftKeyVal *a, const SiftKeyVal *b)
{
	for (int k = 0; k < spec->ncols; k++)
	{
		int			c;

		if (a[k].isnull || b[k].isnull)
			c = a[k].isnull == b[k].isnull ? 0 : (a[k].isnull ? 1 : -1) * ((spec->cols[k].flags & SIFT_KEYCOL_NULLS_FIRST) ? -1 : 1);
		else
			c = sift_key_val_cmp(&spec->cols[k], &a[k], &b[k]) * ((spec->cols[k].flags & SIFT_KEYCOL_DESC) ? -1 : 1);
		if (c != 0)
			return c;
	}
	return 0;
}

/* Whether the rows are sorted by the key: the marks hold the first and the last key of a granule, which are its least and greatest only then */
static bool
rows_sorted(const SiftRows *rows, const SiftKeySpec *spec)
{
	SiftKeyVal	prev[SIFT_KEY_COLS_MAX],
				cur[SIFT_KEY_COLS_MAX];

	for (int r = 0; r < rows->nrows; r++)
	{
		for (int k = 0; k < spec->ncols; k++)
			cur[k] = cell_val(&rows->cols[rows->sort_cols[k]], r);
		if (r > 0 && tuple_cmp(spec, prev, cur) > 0)
			return false;
		memcpy(prev, cur, sizeof(cur));
	}
	return true;
}

void *
sift_f2_key_marks_build(const SiftRows *rows, int ng, const int *gstart, uint64 *len)
{
	SiftKeySpec spec;
	SiftCellKind kinds[SIFT_KEY_COLS_MAX];
	uint8		hints[SIFT_KEY_COLS_MAX];
	SiftKeyMarksBuilder *b;
	SiftKeyVal	first[SIFT_KEY_COLS_MAX],
				last[SIFT_KEY_COLS_MAX];
	void	   *blob;
	SiftKeyMarks km;

	for (int k = 0; k < rows->nsort && k < SIFT_KEY_COLS_MAX; k++)
	{
		kinds[k] = rows->cols[rows->sort_cols[k]].kind;
		hints[k] = rows->cols[rows->sort_cols[k]].hint;
	}
	if (!sift_f2_key_spec(kinds, hints, rows->sort_flags, Min(rows->nsort, SIFT_KEY_COLS_MAX), &spec))
		return NULL;
	for (int k = 0; k < spec.ncols; k++)
		if (has_unorderable(&rows->cols[rows->sort_cols[k]], rows->nrows))
			return NULL;
	if (!rows_sorted(rows, &spec))
		return NULL;
	b = sift_key_marks_begin(&spec, true);
	for (int g = 0; g < ng; g++)
	{
		for (int k = 0; k < spec.ncols; k++)
		{
			first[k] = cell_val(&rows->cols[rows->sort_cols[k]], gstart[g]);
			last[k] = cell_val(&rows->cols[rows->sort_cols[k]], gstart[g + 1] - 1);
		}
		sift_key_marks_add(b, first, last);
	}
	blob = sift_key_marks_finish(b, len);
	if (sift_key_marks_open(blob, *len, &spec, ng, &km) != NULL || sift_key_marks_check(&km) != NULL)
	{
		pfree(blob);
		return NULL;
	}
	return blob;
}

/* ---- the reader ---- */

typedef struct KeyMarksCache
{
	void	   *blob;
	SiftKeyMarks km;
} KeyMarksCache;

const SiftKeyMarks *
sift_part_key_marks(Relation rel, const SiftPart *cpart, const SiftF2Rel *f)
{
	SiftPart   *part = (SiftPart *) cpart;
	KeyMarksCache *cache;
	MemoryContext old;
	SiftKeySpec spec;
	uint64		len;
	const char *err;

	if (part->key_marks_read)
		return part->key_marks != NULL ? &((KeyMarksCache *) part->key_marks)->km : NULL;
	part->key_marks_read = true;
	if (sift_part_section(part, SEC_KEY_MARKS) == NULL || !sift_f2_rel_key_spec(f, &spec))
		return NULL;
	old = MemoryContextSwitchTo(GetMemoryChunkContext(part));
	cache = palloc0(sizeof(KeyMarksCache));
	cache->blob = sift_part_read_section(rel, part, SEC_KEY_MARKS, &len);
	MemoryContextSwitchTo(old);
	err = sift_key_marks_open(cache->blob, len, &spec, part->hdr.ngranules, &cache->km);
	if (err != NULL)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "%s.", err);
	part->key_marks = cache;
	return &cache->km;
}

/* ---- the plan of a scan ---- */

/* A constant of the predicate as a value of the key column */
static SiftKeyVal
key_val(int kind, const SiftColValue *v)
{
	SiftKeyVal	out = {.isnull = v->isnull};

	if (kind == SIFT_KK_FLOAT8)
		out.f = v->v.f;
	else if (kind == SIFT_KK_TEXT || kind == SIFT_KK_UUID)
	{
		out.p = (const uint8 *) v->v.s;
		out.len = v->len;
	}
	else
		out.i = v->v.i;
	return out;
}

static int
key_op(SiftColOp op)
{
	switch (op)
	{
		case SIFT_COLOP_EQ:
			return SIFT_KOP_EQ;
		case SIFT_COLOP_LT:
			return SIFT_KOP_LT;
		case SIFT_COLOP_LE:
			return SIFT_KOP_LE;
		case SIFT_COLOP_GT:
			return SIFT_KOP_GT;
		case SIFT_COLOP_GE:
			return SIFT_KOP_GE;
		case SIFT_COLOP_IN:
			return SIFT_KOP_IN;
		case SIFT_COLOP_PREFIX:
			return SIFT_KOP_PREFIX;
		case SIFT_COLOP_ISNULL:
			return SIFT_KOP_ISNULL;
		case SIFT_COLOP_NOTNULL:
			return SIFT_KOP_NOTNULL;
		default:
			return 0;
	}
}

void
sift_f2_key_plan(const SiftF2Rel *f, const SiftColDef *cols, const SiftColPred *preds, int npreds, SiftKeyPlan *plan)
{
	SiftKeySpec spec;
	SiftKeyPred *kp;
	SiftKeyVal *vals;
	int			nkp = 0,
				nvals = 0,
				total = 0;

	*plan = (SiftKeyPlan) {.state = SIFT_KEY_PLAN_FULL};
	if (!sift_f2_rel_key_spec(f, &spec))
		return;
	for (int i = 0; i < npreds; i++)
		total += preds[i].nvals;
	kp = palloc(Max(npreds, 1) * sizeof(SiftKeyPred));
	/* check-memory: allow the values are the constants of the predicates of one query */
	vals = palloc(Max(total, 1) * sizeof(SiftKeyVal));
	for (int i = 0; i < npreds; i++)
	{
		const SiftColPred *p = &preds[i];
		int			col = -1,
					op = key_op(p->op);

		for (int k = 0; k < spec.ncols; k++)
			if (f->key_cols[k] == p->col)
				col = k;
		if (col < 0 || op == 0 || sift_f2_undecided(&cols[p->col], p))
			continue;
		for (int j = 0; j < p->nvals; j++)
			vals[nvals + j] = key_val(spec.cols[col].kind, &p->vals[j]);
		kp[nkp++] = (SiftKeyPred) {.col = col,.op = op,.nvals = p->nvals,.vals = &vals[nvals]};
		nvals += p->nvals;
	}
	sift_key_plan_build(&spec, kp, nkp, plan);
	pfree(kp);
	pfree(vals);
}

/* ---- the statistics ---- */

void
sift_f2_col_stats_granule(SiftColStatsBuilder *b, int c, const SiftColIn *col, int nrows, uint32 nnull, const SiftChunkRef *ref)
{
	SiftNdvSketch *ndv = sift_col_stats_ndv(b, c);

	sift_col_stats_granule(b, c, nrows, nnull, ref->len, ref->enc);
	for (int r = 0; r < nrows; r++)
	{
		SiftKeyVal	v;
		int64		bits;

		if (col->isnull != NULL && col->isnull[r])
			continue;
		v = cell_val(col, r);
		memcpy(&bits, &v.f, sizeof(bits));
		if (col->kind == SIFT_CELL_STR)
			sift_ndv_add_bytes(ndv, v.p, v.len);
		else
			sift_ndv_add_i64(ndv, col->kind == SIFT_CELL_INT ? v.i : bits);
	}
}

void *
sift_f2_col_stats_finish(SiftColStatsBuilder *b, int nc, SiftDictEnc *const *dicts, uint32 ng, uint64 *len)
{
	for (int c = 0; dicts != NULL && c < nc; c++)
		if (dicts[c] != NULL)
			sift_col_stats_set_ndv_exact(b, c, dicts[c]->n);
	return sift_col_stats_finish(b, ng, len);
}

bool
sift_part_col_stats(Relation rel, const SiftPart *part, SiftColStats *cs)
{
	uint64		len;
	void	   *blob;
	const char *err;

	if (sift_part_section(part, SEC_COL_STATS) == NULL)
		return false;
	blob = sift_part_read_section(rel, part, SEC_COL_STATS, &len);
	err = sift_col_stats_open(blob, len, part->hdr.ncols, part->hdr.ngranules, cs);
	if (err != NULL)
		SIFT_SEG_CORRUPT(rel, part->hdr.part_gen, "%s.", err);
	return true;
}

/* ---- the parts of a join ---- */

void
sift_f2_marks_join(Relation rel, SiftPart *const *src, int n, SiftSecInput *secs, int *nsec, uint8 *ext_flags)
{
	static const uint16 tags[2] = {SEC_KEY_MARKS, SEC_COL_STATS};
	static const uint8 flags[2] = {SIFT_PART_EXT_KEY_MARKS, SIFT_PART_EXT_COL_STATS};

	*ext_flags = 0;
	for (int t = 0; t < 2; t++)
	{
		const void **blobs = palloc(n * sizeof(void *));
		uint64	   *lens = palloc(n * sizeof(uint64));
		void	   *joined = NULL;
		uint64		len = 0;
		int			have = 0;

		for (int i = 0; i < n; i++)
		{
			blobs[i] = sift_part_read_section(rel, src[i], tags[t], &lens[i]);
			have += blobs[i] != NULL;
		}
		if (have == n)
			joined = t == 0 ? sift_key_marks_concat(blobs, lens, n, &len) : sift_col_stats_merge(blobs, lens, n, &len);
		if (joined != NULL)
		{
			sift_part_add_section(secs, nsec, tags[t], 0, joined, len, false);
			*ext_flags |= flags[t];
		}
		for (int i = 0; i < n; i++)
			if (blobs[i] != NULL)
				pfree((void *) blobs[i]);
		pfree(blobs);
		pfree(lens);
	}
}

/* ---- the first order of the predicates ---- */

static SiftCostClass
cost_class(uint16 enc)
{
	switch (enc)
	{
		case SIFT_CENC_ALL_NULL:
		case SIFT_CENC_INT_CONST:
		case SIFT_CENC_KEY_PRESENT:
			return SIFT_CC_CONST;
		case SIFT_CENC_BOOL_BITS:
			return SIFT_CC_BOOL_BITS;
		case SIFT_CENC_RLE:
			return SIFT_CC_RLE;
		case SIFT_CENC_DICT_CODE:
		case SIFT_CENC_INT_FOR:
			return SIFT_CC_DICT_BITSET;
		case SIFT_CENC_INT_DELTA:
			return SIFT_CC_DELTA;
		case SIFT_CENC_STR_FSST:
			return SIFT_CC_FSST;
		case SIFT_CENC_STR_INLINE:
		case SIFT_CENC_STR_BLOCK:
		case SIFT_CENC_STR_VIEW:
		case SIFT_CENC_STR_PROMO:
			return SIFT_CC_STR_BLOCK;
		default:
			return SIFT_CC_NUM64;
	}
}

static SiftPredShape
pred_shape(SiftColOp op)
{
	switch (op)
	{
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_IN:
			return SIFT_PS_EQ;
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
		case SIFT_COLOP_PREFIX:
			return SIFT_PS_RANGE;
		case SIFT_COLOP_ISNULL:
			return SIFT_PS_ISNULL;
		case SIFT_COLOP_NOTNULL:
			return SIFT_PS_NOTNULL;
		case SIFT_COLOP_CONTAINS:
		case SIFT_COLOP_ICONTAINS:
		case SIFT_COLOP_SUFFIX:
			return SIFT_PS_SUBSTRING;
		default:
			return SIFT_PS_OTHER;
	}
}

void
sift_f2_seed_filter(Relation rel, const SiftPart *part, const SiftPartDesc *d, const SiftColPred *preds, int npreds, SiftAdaptiveFilter *af)
{
	SiftColStats cs;
	SiftConjunctInit init[SIFT_AFILT_MAX];
	int			n = Min(npreds, SIFT_AFILT_MAX);

	af->io_cold = sift_lf_part_is_cold(d->level, d->ext_flags);
	if (af->granules_seen > 0 || n == 0 || !sift_part_col_stats(rel, part, &cs))
		return;
	for (int k = 0; k < n; k++)
	{
		SiftPartColStat st = sift_col_stats_get(&cs, preds[k].col);
		uint64		nulls = sift_col_stats_nulls(&cs, preds[k].col);

		init[k] = (SiftConjunctInit) {.col = preds[k].col,.barrier_group = af->st[k].barrier_group,
			.pass_prior = sift_col_stats_pass_prior(&st, part->hdr.nrows, nulls, pred_shape(preds[k].op), preds[k].nvals),
			.cpu_ns_row = Max(af->st[k].cpu, sift_cost_class_ns(cost_class(st.dominant_enc))),.io_bytes_row = sift_col_stats_bytes_per_row(&st, part->hdr.nrows)};
	}
	sift_afilt_init(af, init, n, af->io_cold);
}
