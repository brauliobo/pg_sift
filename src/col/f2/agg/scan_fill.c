/*
 * col/f2/agg/scan_fill.c - the decoded values of a batch of the scan of an index or sift_table of format major 2 as the vectors of the columns of the aggregate (f2_v6_scan.c).
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/engine.h"
#include "agg/win/datebin_pg.h"
#include "agg/expr/str.h"
#include "agg/qual/bqual.h"
#include "col/f2/agg/scan_int.h"
#include "col/shred/pg.h"
#include "table/f2/blob.h"

static SiftStrView
strview_of(const char *p, uint32 len)
{
	SiftStrView v = {.len = len};

	if (len == 0)
		return v;
	memcpy(v.prefix, p, Min(len, 4));
	if (len <= 12)
		memcpy(v.u.tail, p + 4, len > 4 ? len - 4 : 0);
	else
		v.u.ptr = p;
	return v;
}

static bool
any_null(const bool *isnull, int n)
{
	uint64		any = 0;
	int			i = 0;

	for (; i + 8 <= n; i += 8)
	{
		uint64		w;

		memcpy(&w, isnull + i, sizeof(w));
		any |= w;
	}
	for (; i < n; i++)
		any |= isnull[i];
	return any != 0;
}

/* a sift_table keeps the bytes of a text in its cell, and the datum of a wide one (SIFT_COLTAG_WIDE_BLOB): its bytes are the ones after its header */
SiftStrView
sift_f2_v6_text_view(const ColView *cv, int row, bool table)
{
	const uint32 code = cv->dcode != NULL ? cv->dcode[row] : 0;

	if (cv->isnull[row])
		return strview_of(NULL, 0);
	if (cv->dcode != NULL)
		return strview_of(cv->dict->bytes + cv->dict->off[code], cv->dict->off[code + 1] - cv->dict->off[code]);
	if (table && cv->tag != NULL && cv->tag[row] == SIFT_COLTAG_WIDE_BLOB)
		return strview_of(VARDATA_ANY(cv->s[row]), VARSIZE_ANY_EXHDR(cv->s[row]));
	return strview_of(cv->s[row], cv->slen[row]);
}

static void
text_view_at(SiftStrView *out, const ColView *cv, int off, int i, bool table)
{
	out[i] = sift_f2_v6_text_view(cv, off + i, table);
}

/* the `->>` text of the booleans of a bool leaf */
static void
bool_texts(SiftStrView *out, const ColView *cv, int off, int n)
{
	for (int i = 0; i < n; i++)
		out[i] = cv->i[off + i] ? strview_of("true", 4) : strview_of("false", 5);
}

/* The views of the strings of the rows of mask (all n rows when it is NULL): a batch that keeps few rows makes the views of those */
static void
text_views(SiftStrView *out, const ColView *cv, int off, int n, const uint64 *mask, bool table)
{
	if (mask == NULL)
		for (int i = 0; i < n; i++)
			text_view_at(out, cv, off, i, table);
	else
		for (int w = 0; w < (n + 63) / 64; w++)
			for (uint64 bits = mask[w]; bits != 0; bits &= bits - 1)
				text_view_at(out, cv, off, w * 64 + __builtin_ctzll(bits), table);
}

void
sift_f2_v6_fill_col(V6Col *c, const ColView *cv, int off, int n, uint64 part_gen, const uint64 *mask)
{
	SiftVec    *v = &c->vec;
	const bool	nulls = any_null(cv->isnull + off, n);
	const bool	numeric = c->typid == NUMERICOID && cv->tag != NULL;
	bool		nans = false;

	if (nulls)
	{
		memset(c->valid, 0, sizeof(c->valid));
		for (int i = 0; i < n; i++)
			c->valid[i >> 6] |= (uint64) !cv->isnull[off + i] << (i & 63);
	}
	if (nulls && numeric)
	{
		memset(c->nan, 0, sizeof(c->nan));
		for (int i = 0; i < n; i++)
		{
			const bool	nan = cv->isnull[off + i] && cv->tag[off + i] == SIFT_COLTAG_UNSURE;

			c->nan[i >> 6] |= (uint64) nan << (i & 63);
			nans |= nan;
		}
	}
	switch (c->cls)
	{
		case SV6C_F64:
			v->type = SVEC_F64;
			memcpy(v->data, cv->f + off, n * sizeof(double));
			break;
		case SV6C_TEXT:
			if (cv->dcode != NULL && c->by_code && cv->dict->part)
			{
				v->type = SVEC_DICT32;
				c->dict = (SiftDictSrc) {.gen = part_gen,.n = cv->dict->n,.off = cv->dict->off,.bytes = cv->dict->bytes};
				v->dict = &c->dict;
				memcpy(v->data, cv->dcode + off, n * sizeof(uint32));
				break;
			}
			if (cv->dcode != NULL && c->by_code)
			{
				v->type = SVEC_STRVIEW;
				text_views(v->data, cv, off, n, mask, c->is_table);
				break;
			}
			if (cv->dcode != NULL)
			{
				v->type = SVEC_DICT32;
				c->kdict = (SiftKeyDict) {.d = *cv->dict,.gen = cv->dict->part ? part_gen : 0};
				v->dict = &c->kdict;
				memcpy(v->data, cv->dcode + off, n * sizeof(uint32));
				break;
			}
			v->type = SVEC_STRVIEW;
			if (cv->kind == SIFT_CELL_INT)
				bool_texts(v->data, cv, off, n);
			else
				text_views(v->data, cv, off, n, mask, c->is_table);
			break;
		default:
			v->type = SVEC_I64;
			memcpy(v->data, cv->i + off, n * sizeof(int64));
			break;
	}
	v->valid = nulls ? c->valid : NULL;
	v->nan = nans ? c->nan : NULL;
	v->all_valid = !nulls;
	v->len = n;
	v->dense = true;
	v->sel = NULL;
}

/* A NaN that no aggregate folds (a count of the distinct values, an argument of sift.arg_max) is not left out as a NULL would be: the statement is refused */
static void
refuse_nan(const V6Col *c, const SiftBatchSel *bs)
{
	if (c->vec.nan == NULL || c->nan_ok)
		return;
	for (int w = 0; w < (bs->n_raw + 63) / 64; w++)
		if ((c->nan[w] & bs->mask[w]) != 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a numeric NaN cannot be used by this aggregate on the stored values"),
							errhint("Run the statement with sift.enable_agg = off.")));
}

static bool
is_exception(const ColView *cv, int r)
{
	return cv->isnull[r] && cv->tag[r] >= SIFT_COLTAG_STRING && cv->tag[r] <= SIFT_COLTAG_ARRAY;
}

/* the integer an integer leaf holds, of the numbers it kept aside for their text (3.0): a containment test compares the value */
static void
fill_held(V6Scan *s, V6Col *c, const ColView *cv, int off, int n)
{
	MemoryContext old = MemoryContextSwitchTo(s->batch_cx);

	for (int i = 0; i < n; i++)
		if (is_exception(cv, off + i) && ((s->bs.mask[i >> 6] >> (i & 63)) & 1) && sift_shred_held_int(cv->xpay[off + i], cv->xplen[off + i], &((int64 *) c->vec.data)[i]))
			c->valid[i >> 6] |= UINT64CONST(1) << (i & 63);
	MemoryContextSwitchTo(old);
}

/*
 * A row of a leaf of a key of a document whose value the leaf could not hold has it in the payload of its exception: the rows the batch keeps are read from there, the others stay NULL. A value whose
 * cast raises stays NULL too: the rows of those are returned (NULL: none), for the scan to raise once it knows which rows its batch quals keep
 */
uint64 *
sift_f2_v6_fill_exceptions(V6Scan *s, V6Col *c, const ColView *cv, int off, int n)
{
	SiftVec    *v = &c->vec;
	uint64	   *raises = NULL;

	if (cv->xpay == NULL)
		return NULL;
	if (c->cls == SV6C_I64 && !c->jint)
	{
		fill_held(s, c, cv, off, n);
		return NULL;
	}
	for (int i = 0; i < n; i++)
	{
		const int	r = off + i;
		SiftV6JsonValue val;
		SiftV6JsonExc res;

		if (!is_exception(cv, r) || !((s->bs.mask[i >> 6] >> (i & 63)) & 1))
			continue;
		SIFT_INVARIANT(cv->xpay[r] != NULL, "a value a leaf of a JSON key could not hold has its payload");
		res = sift_v6_json_exception_read(c->typid, c->jsonb_cast, cv->xpay[r], cv->xplen[r], s->batch_cx, &val);
		if (res == SV6JX_RAISES)
		{
			raises = raises != NULL ? raises : MemoryContextAllocZero(s->batch_cx, SIFT_VEC_WORDS * sizeof(uint64));
			raises[i >> 6] |= UINT64CONST(1) << (i & 63);
		}
		if (res != SV6JX_VALUE)
			continue;
		if (c->cls == SV6C_TEXT)
			((SiftStrView *) v->data)[i] = strview_of(val.text, val.tlen);
		else if (c->cls == SV6C_F64)
			((double *) v->data)[i] = DatumGetFloat8(val.value);
		else
			((int64 *) v->data)[i] = sift_v6_json_int(c->typid, val.value);
		c->valid[i >> 6] |= UINT64CONST(1) << (i & 63);
	}
	return raises;
}

static bool
reads_attno(Node *n, void *attno)
{
	return n != NULL && ((IsA(n, Var) && ((const Var *) n)->varattno == *(const int *) attno) || expression_tree_walker(n, reads_attno, attno));
}

/* the batch quals read the column: PostgreSQL casts the value of each row the scan keys and the quals it tests first (the ones that convert no such value) keep */
static bool
quals_read(const V6Scan *s, const V6Col *c)
{
	int			attno = c->jmember > 0 ? sift_v6_member_vatt(c->jmember, c->jsonb_cast) : sift_v6_jvatt_base(false, c->jint, c->typid) + c->leaf;

	return (c->jmember > 0 || !c->jsonb_cast) && reads_attno((Node *) s->spec->vqual, &attno);
}

/* PostgreSQL's error for the conversion of row r of the column */
static void
raise_row(V6Scan *s, const V6Col *c, const ColView *views, const uint64 *tid, int r)
{
	if (c->jmember > 0)
		sift_f2_v6_member_raise(s, c, views, tid, r);
	sift_v6_json_exception_raise(c->typid, c->jsonb_cast, views[c->leaf].xpay[r], views[c->leaf].xplen[r], s->batch_cx);
}

/* the rows PostgreSQL casts the column of: those the batch quals keep, or of a column they read themselves those the quals it tests first keep */
static const uint64 *
cast_rows(const V6Scan *s, const V6Col *c, const V6Raises *r)
{
	return s->bq == NULL || quals_read(s, c) ? r->uncast.mask : r->kept.mask;
}

static bool
where_col(const V6Scan *s, const V6Col *c)
{
	return s->bq != NULL && quals_read(s, c);
}

void
sift_f2_v6_raises(V6Scan *s, uint64 *const *cols, int n, V6Raises *r)
{
	r->cols = cols;
	r->kept = r->uncast = s->bs;
	memset(r->rows, 0, sizeof(r->rows));
	memset(r->where, 0, sizeof(r->where));
	if (cols == NULL)
		return;
	if (s->bq != NULL)
		sift_v6_bq_filter(s->bq, &r->kept);
	if (s->bq_uncast != NULL)
		sift_v6_bq_filter(s->bq_uncast, &r->uncast);
	for (int k = 0; k < s->ncols; k++)
		for (int w = 0; cols[k] != NULL && w < (n + 63) / 64; w++)
		{
			const uint64 hit = cols[k][w] & cast_rows(s, &s->cols[k], r)[w];

			r->rows[w] |= hit;
			r->where[w] |= where_col(s, &s->cols[k]) ? hit : 0;
		}
}

/* PostgreSQL's first error in row i: the cast of the WHERE clause, which it tests first, else the first read of the row it keeps */
void
sift_f2_v6_raise_at(V6Scan *s, const ColView *views, const uint64 *tid, const V6Raises *r, int off, int i)
{
	for (int where = 1; where >= 0; where--)
		for (int k = 0; k < s->ncols; k++)
		{
			const V6Col *c = &s->cols[k];

			if (r->cols[k] != NULL && where_col(s, c) == where && ((r->cols[k][i >> 6] & cast_rows(s, c, r)[i >> 6]) >> (i & 63)) & 1)
				raise_row(s, c, views, tid, off + i);
		}
}

/*
 * PostgreSQL's error for the first row whose cast raises that PostgreSQL casts (one the batch quals keep, or any row of a column they read themselves): in the row, the error PostgreSQL meets first.
 * When sift_f2_v6_raise_kept returns, no row of the batch raises
 */
void
sift_f2_v6_raise_kept(V6Scan *s, const ColView *views, const uint64 *tid, uint64 *const *raises, int off, int n)
{
	V6Raises	r;

	sift_f2_v6_raises(s, raises, n, &r);
	for (int w = 0; w < (n + 63) / 64; w++)
		if (r.rows[w] != 0)
			sift_f2_v6_raise_at(s, views, tid, &r, off, w * 64 + __builtin_ctzll(r.rows[w]));
}

typedef struct LateRow
{
	V6Scan	   *s;
	const ColView *views;
	const uint64 *tid;
	const V6Raises *r;
	int			off;
	int			i;
} LateRow;

/* the row reached the groups through the join: its error is the statement's */
static void
reach_groups(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	const LateRow *lr = arg;

	if (bs->n_active > 0)
		sift_f2_v6_raise_at(lr->s, lr->views, lr->tid, lr->r, lr->off, lr->i);
}

/*
 * The rows of a star join whose casts raise, in their order: the error of a batch qual's cast at once, as PostgreSQL tests the quals of the fact table before the join; a row whose error is a
 * read of a row the quals keep goes alone through the pipe, and raises when it reaches the groups (a row the join drops raises nothing). The rows that did not raise leave the batch
 */
void
sift_f2_v6_pipe_raises(V6Scan *s, const ColView *views, const uint64 *tid, const V6Raises *r, int off, int n)
{
	const SiftBatchSel all = s->bs;
	LateRow		lr = {s, views, tid, r, off};
	uint32		left = 0;

	for (int w = 0; w < (n + 63) / 64; w++)
		for (uint64 bits = r->rows[w]; bits != 0; bits &= bits - 1)
		{
			lr.i = w * 64 + __builtin_ctzll(bits);
			if ((r->where[w] >> (lr.i & 63)) & 1)
				sift_f2_v6_raise_at(s, views, tid, r, off, lr.i);
			memset(s->bs.mask, 0, sizeof(s->bs.mask));
			s->bs.mask[w] = UINT64CONST(1) << (lr.i & 63);
			s->bs.n_active = 1;
			s->bs.all_active = n == 1;
			s->bs.has_sel = false;
			sift_f2_v6_derive_columns(s);
			sift_f2_v6_bin_keys(s);
			s->pipe->sink = reach_groups;
			s->pipe->sink_arg = &lr;
			sift_v6_pipe_run(s->pipe, s->fact, &s->bs);
			s->pipe->sink = NULL;
			s->pipe->sink_arg = NULL;
		}
	s->bs = all;
	for (int w = 0; w < (n + 63) / 64; w++)
		left += (uint32) pg_popcount64(s->bs.mask[w] &= ~r->rows[w]);
	s->bs.n_active = (uint16) left;
	s->bs.all_active = left == (uint32) n;
}

/* the presence of the key on each row, from the tags of its leaf (SIFT_COLTAG_ROOTKEY: a document that is no object and holds the key as jsonb_exists finds it); a SQL NULL document has it not */
static void
fill_presence(V6Col *c, const ColView *cv, int off, int n)
{
	SiftVec    *v = &c->vec;

	for (int i = 0; i < n; i++)
		((int64 *) v->data)[i] = cv->keyp != NULL ? cv->keyp[off + i] : sift_key_row_present(cv->isnull[off + i], cv->tag != NULL ? cv->tag[off + i] : 0);
	v->type = SVEC_I64;
	v->valid = NULL;
	v->nan = NULL;
	v->all_valid = true;
	v->len = n;
	v->dense = true;
	v->sel = NULL;
}

void
sift_f2_v6_fill_type(V6Col *c, const ColView *cv, int off, int n)
{
	static const char *const names[] = {NULL, "string", "number", "boolean", "object", "array", NULL, "null"};
	SiftVec    *v = &c->vec;
	SiftStrView *out = v->data;
	bool		all = true;

	if (c->cls == SV6C_BOOL)
	{
		fill_presence(c, cv, off, n);
		return;
	}
	memset(c->valid, 0, (n + 63) / 64 * sizeof(uint64));
	for (int i = 0; i < n; i++)
	{
		const uint8 tag = cv->tag != NULL ? cv->tag[off + i] : 0;
		const uint8 type = cv->isnull[off + i] ? tag : tag == SIFT_COLTAG_NUMBER || tag == SIFT_COLTAG_BOOLEAN ? tag : c->jbase;
		const char *name = type < lengthof(names) ? names[type] : NULL;

		out[i] = strview_of(name, name != NULL ? strlen(name) : 0);
		c->valid[i >> 6] |= (uint64) (name != NULL) << (i & 63);
		all &= name != NULL;
	}
	v->type = SVEC_STRVIEW;
	v->valid = all ? NULL : c->valid;
	v->nan = NULL;
	v->all_valid = all;
	v->len = n;
	v->dense = true;
	v->sel = NULL;
}

/* The score of each row's hit, as the double PostgreSQL makes of a real for avg() and the like */
void
sift_f2_v6_fill_score(V6Col *c, const float4 *score, int off, int n)
{
	SiftVec    *v = &c->vec;

	for (int i = 0; i < n; i++)
		((double *) v->data)[i] = score[off + i];
	v->type = SVEC_F64;
	v->valid = NULL;
	v->nan = NULL;
	v->all_valid = true;
	v->len = n;
	v->dense = true;
	v->sel = NULL;
}

/* the values of a leaf, with those of the exceptions of a leaf of a key of a document: the rows whose cast raises (NULL: none) */
static uint64 *
fill_value(V6Scan *s, V6Col *c, const ColView *cv, uint64 part_gen, int off, int n)
{
	sift_f2_v6_fill_col(c, cv, off, n, sift_v6_dict_gen(s->host, part_gen), s->bs.n_active * 2 < n ? s->bs.mask : NULL);
	return c->json ? sift_f2_v6_fill_exceptions(s, c, cv, off, n) : NULL;
}

/* The vectors of the columns of the n rows from off of a batch: the rows of the selection s->bs are the ones that matter. The rows whose cast raises, for sift_f2_v6_raise_kept once the rows
 * that reach the cast are known (NULL: none) */
uint64 **
sift_f2_v6_fill_cols(V6Scan *s, const ColView *views, const uint64 *tid, const float4 *score, uint64 part_gen, int off, int n)
{
	uint64	  **raises = NULL;

	if (s->batch_cx != NULL)
		MemoryContextReset(s->batch_cx);
	for (int i = 0; i < s->ncols; i++)
	{
		V6Col	   *c = &s->cols[i];
		const ColView *cv = &views[c->leaf];

		if (c->lane)
			continue;
		if (c->attno == SIFT_V6_SCORE_ATTNO)
			sift_f2_v6_fill_score(c, score, off, n);
		else if (c->jtype)
			sift_f2_v6_fill_type(c, cv, off, n);
		else if (c->cls == SV6C_DATUM)
			sift_f2_v6_fill_datum(c, views, off, n, s->batch_cx, s->bs.mask);
		else
		{
			uint64	   *rows = c->jmember > 0 ? sift_f2_v6_fill_member(s, c, views, tid, off, n) : fill_value(s, c, cv, part_gen, off, n);

			if (rows == NULL)
				continue;
			raises = raises != NULL ? raises : MemoryContextAllocZero(s->batch_cx, s->ncols * sizeof(uint64 *));
			raises[i] = rows;
		}
	}
	return raises;
}

/* The columns that hold a NaN no aggregate folds, for the rows the selection s->bs keeps */
void
sift_f2_v6_refuse_nans(const V6Scan *s)
{
	for (int i = 0; i < s->ncols; i++)
		if (!s->cols[i].lane && s->cols[i].attno != SIFT_V6_SCORE_ATTNO && !s->cols[i].jtype && s->cols[i].cls != SV6C_DATUM)
			refuse_nan(&s->cols[i], &s->bs);
}
