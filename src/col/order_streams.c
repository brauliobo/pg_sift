/*
 * col/order_streams.c - which inputs of a merge of key-cut segments are a stream: a segment records the leaves it is cut by (SIFT_SEG_CUTREC), and one that does
 * not (written before) has its units checked once
 */
#include "postgres.h"

#include "col/am.h"
#include "col/seg.h"

/* Are the units of seg, as stored, in the order of the TIDs of their rows (the superblock never goes back), as the merge by superblock reads them? */
static bool
probe_tid_order(SiftColSegment *seg, MemoryContext cx)
{
	SiftColBatch *b = sift_cbatch_create(cx, seg->schema);
	bool	   *want = palloc0_array(bool, seg->schema->ncols);
	uint32		last = 0;
	bool		ordered = true;

	for (uint32 u = 0; u < seg->nsb && ordered; u++)
	{
		sift_cbatch_reset(b, 0);
		sift_cseg_decode_unit_leaves(seg, u, want, b);
		for (uint32 r = 0; r < b->nrows && ordered; r++)
		{
			uint32		sb = sift_key_block(b->keys[r]) / SIFT_SB_PAGES;

			ordered = sb >= last;
			last = sb;
		}
	}
	return ordered;
}

/* ... and in the order of the key the leaves name, each unit's rows from the smallest to the largest, none below the largest of the unit before (what load_chunk requires of a stream) */
static bool
probe_key_order(SiftColSegment *seg, const int *leaf, int nleaf, MemoryContext cx)
{
	bool	   *want = palloc0_array(bool, seg->schema->ncols);
	SiftColBatch *b[2] = {sift_cbatch_create(cx, seg->schema), sift_cbatch_create(cx, seg->schema)};
	const SiftColBatch *maxb = NULL;
	uint32		maxr = 0;
	bool		ordered = true;

	for (int i = 0; i < nleaf; i++)
		want[leaf[i]] = true;
	for (uint32 u = 0, cur = 0; u < seg->nsb && ordered; u++)
	{
		const SiftColBatch *c = b[cur];
		uint32		lo = 0,
					hi = 0;

		sift_cbatch_reset(b[cur], 0);
		sift_cseg_decode_unit_leaves(seg, u, want, b[cur]);
		if (c->nrows == 0)
			continue;
		for (uint32 r = 1; r < c->nrows; r++)
		{
			lo = sift_colk_row_cmp(seg->schema, leaf, nleaf, c, r, c, lo) < 0 ? r : lo;
			hi = sift_colk_row_cmp(seg->schema, leaf, nleaf, c, r, c, hi) > 0 ? r : hi;
		}
		ordered = maxb == NULL || sift_colk_row_cmp(seg->schema, leaf, nleaf, maxb, maxr, c, lo) <= 0;
		maxb = c;
		maxr = hi;
		cur ^= 1;
	}
	return ordered;
}

/*
 * The verdicts of the probes of this backend: a segment never changes its units, so a rolling rewrite, which merges a window of its inputs at a time, probes a segment once.
 * The relation file tells the index of a segment from the one a REINDEX made under the same generation.
 */
#define PROBE_CACHE 32
#ifdef SIFT_TESTING
int64		sift_test_cut_probes = 0;
bool		sift_test_col_loose_inputs = false;
bool		sift_test_col_merge_eager = false;
#endif
static struct
{
	RelFileNumber file;
	uint64		gen;
	int			nleaf;
	int			leaf[SIFT_COL_CUT_MAX];
	bool		ordered;
} probe_cache[PROBE_CACHE];
static int	probe_next;

static bool
probe_cached(const SiftColSegment *seg, const int *leaf, int nleaf, bool *ordered)
{
	for (int i = 0; i < PROBE_CACHE; i++)
	{
		if (probe_cache[i].gen == seg->entry.gen && probe_cache[i].file == seg->index->rd_locator.relNumber &&
			probe_cache[i].nleaf == nleaf && memcmp(probe_cache[i].leaf, leaf, nleaf * sizeof(int)) == 0)
		{
			*ordered = probe_cache[i].ordered;
			return true;
		}
	}
	return false;
}

static void
probe_remember(const SiftColSegment *seg, const int *leaf, int nleaf, bool ordered)
{
	probe_cache[probe_next].file = seg->index->rd_locator.relNumber;
	probe_cache[probe_next].gen = seg->entry.gen;
	probe_cache[probe_next].nleaf = nleaf;
	probe_cache[probe_next].ordered = ordered;
	memcpy(probe_cache[probe_next].leaf, leaf, nleaf * sizeof(int));
	probe_next = (probe_next + 1) % PROBE_CACHE;
}

bool
sift_colk_streams(bool *stream, SiftColSegment *const *seg, int n, const int *leaf, int nleaf, bool resort, MemoryContext cx)
{
	MemoryContext probe = AllocSetContextCreate(cx, "sift_col order probe", ALLOCSET_DEFAULT_SIZES);
	bool		sorted = false;

	for (int i = 0; i < n; i++)
	{
		SiftCutOrder known = sift_cseg_cut_order(seg[i], leaf, nleaf);

		if (known == SIFT_CUT_UNKNOWN && !resort)
		{
			if (!probe_cached(seg[i], leaf, nleaf, &stream[i]))
			{
#ifdef SIFT_TESTING
				sift_test_cut_probes++;
#endif
				stream[i] = nleaf == 0 ? probe_tid_order(seg[i], probe) : probe_key_order(seg[i], leaf, nleaf, probe);
				probe_remember(seg[i], leaf, nleaf, stream[i]);
				MemoryContextReset(probe);
			}
		}
		else
			stream[i] = known == SIFT_CUT_YES;
#ifdef SIFT_TESTING
		stream[i] &= !sift_test_col_loose_inputs || !sift_cseg_keyed(&seg[i]->entry);
#endif
		sorted |= sift_cseg_keyed(&seg[i]->entry) && !stream[i];
	}
	MemoryContextDelete(probe);
	return sorted;
}

/* Is every input cut by the key_cut of the index, as its record says? The participants of a parallel merge each read every input: one whose cut is not recorded would be checked by each of them */
bool
sift_colk_recorded(Relation index, const SiftSegEntry *sel, int n)
{
	int			cut[SIFT_COL_CUT_MAX],
				ncut = sift_col_rel(index)->ncut;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col merge cut", ALLOCSET_SMALL_SIZES);
	bool		recorded = true;

	memcpy(cut, sift_col_rel(index)->cut, ncut * sizeof(int));
	for (int i = 0; i < n && recorded; i++)
	{
		SiftColSegment *seg = sift_cseg_open(index, &sel[i], cx);

		recorded = sift_cseg_cut_order(seg, cut, ncut) == SIFT_CUT_YES;
		sift_cseg_close(seg);
	}
	MemoryContextDelete(cx);
	return recorded;
}
