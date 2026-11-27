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

/* A NaN that no aggregate folds (a count of the distinct values, a filter, an argument of sift.arg_max) is not left out as a NULL would be: the statement is refused */
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
	for (int i = 0; i < n; i++)
	{
		const int	r = off + i;
		SiftV6JsonValue val;
		SiftV6JsonExc res;

		if (!is_exception(cv, r) || !((s->bs.mask[i >> 6] >> (i & 63)) & 1))
			continue;
		SIFT_INVARIANT(cv->xpay[r] != NULL, "a value a leaf of a JSON key could not hold has its payload");
		res = sift_v6_json_exception_read(c->cls, c->jsonb_cast, cv->xpay[r], cv->xplen[r], s->batch_cx, &val);
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
			((int64 *) v->data)[i] = DatumGetBool(val.value);
		c->valid[i >> 6] |= UINT64CONST(1) << (i & 63);
	}
	return raises;
}

static bool
reads_attno(Node *n, void *attno)
{
	return n != NULL && ((IsA(n, Var) && ((const Var *) n)->varattno == *(const int *) attno) || expression_tree_walker(n, reads_attno, attno));
}

/* the batch quals read the column: PostgreSQL tests them on every row the scan keys keep, so it casts the value of each of those */
static bool
quals_read(const V6Scan *s, const V6Col *c)
{
	int			attno = sift_v6_jvatt_base(false, c->typid) + c->leaf;

	return !c->jsonb_cast && reads_attno((Node *) s->spec->vqual, &attno);
}

/* PostgreSQL's error for the first row whose cast raises that PostgreSQL casts: one the batch quals keep, or any row of a column they read themselves */
void
sift_f2_v6_raise_kept(V6Scan *s, const ColView *views, uint64 *const *raises, int off, int n)
{
	SiftBatchSel kept = s->bs;

	if (raises == NULL)
		return;
	if (s->bq != NULL)
		sift_v6_bq_filter(s->bq, &kept);
	for (int k = 0; k < s->ncols; k++)
	{
		const V6Col *c = &s->cols[k];
		const ColView *cv = &views[c->leaf];
		const bool	all = s->bq == NULL || quals_read(s, c);

		for (int w = 0; raises[k] != NULL && w < (n + 63) / 64; w++)
			if ((raises[k][w] & (all ? s->bs.mask[w] : kept.mask[w])) != 0)
			{
				const int	r = off + w * 64 + __builtin_ctzll(raises[k][w] & (all ? s->bs.mask[w] : kept.mask[w]));

				sift_v6_json_exception_raise(c->cls, c->jsonb_cast, cv->xpay[r], cv->xplen[r], s->batch_cx);
			}
	}
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

/* The vectors of the columns of the n rows from off of a batch: the rows of the selection s->bs are the ones that matter. The rows whose cast raises, for sift_f2_v6_raise_kept once the rows
 * that reach the cast are known (NULL: none) */
uint64 **
sift_f2_v6_fill_cols(V6Scan *s, const ColView *views, const float4 *score, uint64 part_gen, int off, int n)
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
			uint64	   *rows;

			sift_f2_v6_fill_col(c, cv, off, n, sift_v6_dict_gen(s->host, part_gen), s->bs.n_active * 2 < n ? s->bs.mask : NULL);
			if (!c->json || (rows = sift_f2_v6_fill_exceptions(s, c, cv, off, n)) == NULL)
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
