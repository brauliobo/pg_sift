/*
 * col/f2/f2_v6_scan_fill.c - the decoded values of a batch of the scan of an index or sift_table of format major 2 as the vectors of the columns of the aggregate (f2_v6_scan.c).
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_datebin_win_pg.h"
#include "agg/v6/v6_expr_str.h"
#include "col/f2/f2_v6_scan_int.h"

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

/* a sift_table keeps the datum of the attribute in its cell: the bytes of a text are the ones after its header */
static void
text_view_at(SiftStrView *out, const ColView *cv, int off, int i, bool table)
{
	const uint32 code = cv->dcode != NULL ? cv->dcode[off + i] : 0;

	if (cv->isnull[off + i])
		out[i] = strview_of(NULL, 0);
	else if (cv->dcode != NULL)
		out[i] = strview_of(cv->dict->bytes + cv->dict->off[code], cv->dict->off[code + 1] - cv->dict->off[code]);
	else if (table)
		out[i] = strview_of(VARDATA_ANY(cv->s[off + i]), VARSIZE_ANY_EXHDR(cv->s[off + i]));
	else
		out[i] = strview_of(cv->s[off + i], cv->slen[off + i]);
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
void
sift_f2_v6_refuse_nan(const V6Col *c, const SiftBatchSel *bs)
{
	if (c->vec.nan == NULL || c->nan_ok)
		return;
	for (int w = 0; w < (bs->n_raw + 63) / 64; w++)
		if ((c->nan[w] & bs->mask[w]) != 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a numeric NaN cannot be used by this aggregate on the stored values"),
							errhint("Run the statement with sift.enable_agg = off.")));
}

/* A row of a leaf of a key of a document whose value the leaf could not hold has it in the payload of its exception: the rows the batch keeps are read from there, the others stay NULL */
void
sift_f2_v6_fill_exceptions(V6Scan *s, V6Col *c, const ColView *cv, int off, int n)
{
	SiftVec    *v = &c->vec;

	if (cv->xpay == NULL)
		return;
	for (int i = 0; i < n; i++)
	{
		const int	r = off + i;
		const char *text = NULL;
		uint32		tlen = 0;
		double		f = 0;

		if (!cv->isnull[r] || cv->tag[r] < SIFT_COLTAG_STRING || cv->tag[r] > SIFT_COLTAG_ARRAY || !((s->bs.mask[i >> 6] >> (i & 63)) & 1))
			continue;
		SIFT_INVARIANT(cv->xpay[r] != NULL, "a value a leaf of a JSON key could not hold has its payload");
		if (!sift_v6_json_exception(c->cls, cv->xpay[r], cv->xplen[r], s->batch_cx, &text, &tlen, &f))
			continue;
		if (c->cls == SV6C_F64)
			((double *) v->data)[i] = f;
		else
			((SiftStrView *) v->data)[i] = strview_of(text, tlen);
		c->valid[i >> 6] |= UINT64CONST(1) << (i & 63);
	}
	v->valid = c->valid;
	v->all_valid = false;
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
