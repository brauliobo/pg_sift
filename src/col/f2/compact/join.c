/*
 * col/f2/compact/join.c - parts joined as they are (col/f2/compact/compact.h): the granules of parts that follow one another copied into one part, their rows never read.
 */
#include "core/host.h"

#include "col/f2/compact/compact.h"
#include "col/f2/dv_join.h"
#include "col/f2/kll/kll.h"
#include "core/page_io.h"
#include "core/xid_cache.h"

static int
by_key(const void *a, const void *b)
{
	return memcmp((*(const SiftPartDesc *const *) a)->key_lo, (*(const SiftPartDesc *const *) b)->key_lo, SIFT_BOUND_BYTES);
}

/* Part b can follow part a in a joined part: its keys ascend from a's, or neither has a key (their rows have no order to keep) */
static bool
part_follows(const SiftPartDesc *a, const SiftPartDesc *b)
{
	return (a->flags & b->flags & SIFT_PART_FLAG_UNKEYED) != 0 || memcmp(a->key_hi, b->key_lo, SIFT_BOUND_BYTES) < 0;
}

/* The side of the cut every row of the part is on: SIFT_PART_FLAG_TIME_HOT or _COLD; 0 when it straddles the cut, has no bounds or its bounds do not hold (a chain patches the time column). Without a time column or a cut every part is on the side 0 */
static uint32
part_side(const SiftPartDesc *p, int64 hot_cut, bool bounds_hold)
{
	if (!bounds_hold || !(p->flags & SIFT_PART_FLAG_HAS_TIME0))
		return 0;
	return p->time_min[0] >= hot_cut ? SIFT_PART_FLAG_TIME_HOT : p->time_max[0] < hot_cut ? SIFT_PART_FLAG_TIME_COLD : 0;
}

/* The vectors of the parts joined, their granule numbers shifted, written for the new part; the deleters older than the horizon are made plain (res->stripped) */
static void
carry_deletes(Relation rel, uint64 part_id, SiftPart *const *src, const SiftPartDesc *const *at, int n, FullTransactionId horizon, SiftCompactResult *res, SiftPartDesc *out)
{
	SiftDvJoinIn *in = palloc(n * sizeof(SiftDvJoinIn));
	FullTransactionId stripped = InvalidFullTransactionId;
	SiftDv	   *dv;

	for (int i = 0; i < n; i++)
	{
		SiftGranuleDesc *dir = sift_part_granule_dir(rel, src[i]);
		uint16	   *grows = palloc(Max(src[i]->hdr.ngranules, 1) * sizeof(uint16));

		for (uint32 g = 0; g < src[i]->hdr.ngranules; g++)
			grows[g] = dir[g].nrows;
		in[i] = (SiftDvJoinIn) {.dv = at[i]->dv_blk != InvalidBlockNumber ? sift_dv_read(rel, at[i], (uint32) part_id, grows) : NULL,.ngranules = src[i]->hdr.ngranules};
	}
	dv = sift_dv_concat_shifted(in, n, out->part_gen, sift_manifest_new_gen(rel, 1), horizon, &stripped);
	if (dv != NULL)
		(void) sift_dv_write(rel, dv, (uint32) part_id, out);
	res->stripped = sift_fxid_max(res->stripped, stripped);
}

bool
sift_compact_join_parts(Relation rel, uint64 part_id, const SiftPartDesc **at, int n, const SiftPartSpec *spec, int64 hot_cut, bool bounds_hold, FullTransactionId horizon, SiftExtent **rel_ext, int *nrel,
		   SiftCompactResult *res, SiftPartDesc *out)
{
	bool		split = spec->time_cols[0] > 0 && hot_cut != PG_INT64_MIN;
	uint32		side = split ? part_side(at[0], hot_cut, bounds_hold) : 0;

	SiftPart  **src = palloc(n * sizeof(SiftPart *));
	SiftPartDesc *sorted = palloc(n * sizeof(SiftPartDesc));
	SiftF2KllBuild *kll = NULL;
	uint16		level = 0;
	uint64		ngranules,
				writer_bytes,
			   *nrows = palloc(n * sizeof(uint64)),
			   *ndead = palloc(n * sizeof(uint64));

	if (split && side == 0)
		return false;
	qsort(at, n, sizeof(SiftPartDesc *), by_key);
	for (int i = 0; i < n; i++)
	{
		uint32		tier = at[i]->flags & (SIFT_PART_FLAG_TIME_HOT | SIFT_PART_FLAG_TIME_COLD);

		if ((i > 0 && !part_follows(at[i - 1], at[i])) ||
			(split && (part_side(at[i], hot_cut, bounds_hold) != side || (tier != 0 && tier != side))) || (!split && tier != (at[0]->flags & (SIFT_PART_FLAG_TIME_HOT | SIFT_PART_FLAG_TIME_COLD))))
			return false;
		sorted[i] = *at[i];
		sorted[i].flags = (sorted[i].flags & ~(SIFT_PART_FLAG_TIME_HOT | SIFT_PART_FLAG_TIME_COLD)) | side;
		src[i] = sift_part_open(rel, at[i]->header_blk, at[i]->part_gen, (uint32) part_id);
		src[i]->io_mode = SIFT_IOS_MAINT_SEQ;
		level = Max(level, at[i]->level);
		nrows[i] = at[i]->nrows;
		ndead[i] = at[i]->ndead;
		sorted[i].dv_blk = InvalidBlockNumber;
	}
	if (!sift_parts_joinable(rel, src, n) || (spec->preagg && sift_part_section(src[0], SEC_PREAGG) == NULL) || !sift_dv_join_sparse(nrows, ndead, n, sift_dead_rewrite_pct))
		return false;
	res->new_gen = sift_manifest_new_gen(rel, 1);
	if (spec->kll != NULL && sift_part_section(src[0], SEC_KLL) != NULL && sift_dv_join_keeps_sketch(nrows, ndead, n))
	{
		kll = sift_f2_kll_begin(CurrentMemoryContext, spec->kll, res->new_gen);
		for (int i = 0; i < n; i++)
			if (!sift_f2_kll_merge_part(kll, rel, src[i], at[i]))
			{
				sift_f2_kll_free(kll);
				return false;
			}
	}
	*out = sift_part_write_joined(rel, (uint32) part_id, res->new_gen, Min(level + 1, SIFT_MAX_PART_LEVEL), src, sorted, n, kll, &ngranules, &writer_bytes);
	for (int i = 0; i < n; i++)
	{
		res->rows_in += at[i]->nrows;
		sift_compact_release_part(src[i], rel_ext, nrel);
	}
	carry_deletes(rel, part_id, src, at, n, horizon, res, out);
	res->rows_out = res->rows_in;
	res->granules_copied = res->granules_written = ngranules;
	res->writer_bytes = writer_bytes;
	res->bytes_out = out->nbytes;
	return true;
}
