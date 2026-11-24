/*
 * agg/wide_nums.c - a numeric leaf that is only summed, as one DNum for each row (agg/wide.h), over the dictionary of col/v1/src_wide.h
 */
/* check-memory: allow-allocs every array here follows the rows of one unit (at most SIFT_COL_UNIT_MAX_ROWS, 74,496) or of one chunk, not the rows of the table */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/agg_int.h"
#include "agg/combo_int.h"
#include "agg/wide.h"
#include "col/am.h"
#include "col/v1/wide.h"


/* the record of an image and a display scale, kept for the rows that read the same stored entry */
typedef struct NumMemo
{
	uint64		tag;			/* ((code + 1) << 6) | dscale, 0: empty */
	DNum		rec;
} NumMemo;

#define NUM_MEMO_MAX	65536

void
sift_aggwide_nums(SiftSrcWide *w, const SiftColDef *def, const SiftColVec *lanes, uint32 n)
{
	if (w->cxt == NULL)
		w->cxt = CurrentMemoryContext;
	if (def->store == SIFT_COLS_STR && w->txt == NULL)
		w->txt = MemoryContextAlloc(w->cxt, SIFT_NIMG_TEXT_MAX);
	if (w->acx == NULL)
		w->acx = AllocSetContextCreate(w->cxt, "sift numeric sums", ALLOCSET_SMALL_SIZES);
	MemoryContextReset(w->acx);
	sift_srcwide_rows_room(w, n);
	if (w->recs == NULL || w->rcap < n)
	{
		if (w->recs)
			pfree(w->recs);
		w->rcap = Max(n, 1024);
		w->recs = MemoryContextAlloc(w->cxt, w->rcap * sizeof(DNum));
	}
	if (w->memo)
		pfree(w->memo);
	w->memo = NULL;
	for (w->nmemo = 256; w->nmemo < n && w->nmemo < NUM_MEMO_MAX; w->nmemo *= 2)
		;
	w->def = def;
	memcpy(w->lanes, lanes, sift_col_def_value_lanes(def) * sizeof(SiftColVec));
	w->n = n;
	w->nd = n;
	w->has_null = false;
	for (uint32 i = 0; i < n; i++)
	{
		bool		isnull = lanes[0].nulls && (lanes[0].nulls[i >> 6] >> (i & 63) & 1),
					nan = isnull && lanes[0].ovf && (lanes[0].ovf[i >> 6] >> (i & 63) & 1);

		w->codes[i] = i;
		w->isnull[i] = isnull && !nan;
		w->has_null |= w->isnull[i];
		w->recs[i].scale = WNUM_UNSET;
	}
}

/* the rows that are not an integer and a scale of 38 digits: the numeric, decoded as the dictionary's entries are, and kept as a numeric when it is not (apart for its large frame) */
static pg_noinline void
rec_numeric(SiftSrcWide *w, uint32 i, bool nan)
{
	uint8		b[SIFT_NIMG_TEXT_MAX + 64] __attribute__((aligned(8)));
	DNum	   *r = &w->recs[i];
	uint32		len = sift_srcwide_numeric_entry(w, w->def, w->lanes, i, nan, b);

	numeric_decode(PointerGetDatum(b), r);
	if (!r->ok)
	{
		void	   *copy = MemoryContextAlloc(w->acx, len);

		memcpy(copy, b, len);
		r->m = (sift_i128) (uintptr_t) copy;
	}
}

/* the record of a row of sift_aggwide_nums, from its lanes */
void
sift_aggwide_rec(const void *arg, uint32 i)
{
	SiftSrcWide *w = (SiftSrcWide *) arg;
	const SiftColDef *def = w->def;
	const SiftColVec *lanes = w->lanes;
	DNum	   *r = &w->recs[i];
	bool		nan = lanes[0].nulls && (lanes[0].nulls[i >> 6] >> (i & 63) & 1) && lanes[0].ovf && (lanes[0].ovf[i >> 6] >> (i & 63) & 1);

	r->ok = false;
	if (nan)
		;
	else if (def->store == SIFT_COLS_STR)
	{
		uint32		code = lanes[0].codes[i],
					nd = 0;
		int64		dscale = lanes[1].i[i];
		int32		exp10 = 0;
		bool		neg = false;
		sift_wu128	dg = 0;
		NumMemo    *mo;
		uint64		tag = ((uint64) code + 1) << 6 | (uint64) (dscale & 63);
		int			kind;

		if (w->memo == NULL)
			w->memo = MemoryContextAllocZero(w->cxt, (size_t) w->nmemo * sizeof(NumMemo));
		mo = &w->memo[code & (w->nmemo - 1)];
		if (mo->tag == tag)
		{
			*r = mo->rec;
			return;
		}
		kind = sift_col_numimg_int((const uint8 *) SIFT_COL_DICT_STR(lanes[0].dict, code), SIFT_COL_DICT_LEN(lanes[0].dict, code), &neg, &exp10, &dg, &nd);
		if (kind == 1 && dscale >= 0 && dscale <= 38)
		{
			*r = (DNum) {.m = 0, .scale = (int16) dscale, .ok = true};
			*mo = (NumMemo) {tag, *r};
			return;
		}
		if (kind == 2 && dscale >= 0 && dscale <= 38)
		{
			int			shift = (int) (exp10 - (int32) nd + dscale);

			if (shift >= 0 && (int) nd + shift <= 38)
			{
				sift_i128	m = (sift_i128) dg * pow10_tab()[shift];

				*r = (DNum) {.m = neg ? -m : m, .scale = (int16) dscale, .ok = true};
				*mo = (NumMemo) {tag, *r};
				return;
			}
		}
	}
	else if (def->scale <= 38)
	{
		*r = (DNum) {.m = sift_col_wide_join(lanes[0].i[i], lanes[1].i[i]), .scale = (int16) def->scale, .ok = true};
		return;
	}
	rec_numeric(w, i, nan);
}
