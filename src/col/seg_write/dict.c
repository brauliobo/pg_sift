/*
 * col/seg_write/dict.c - the dictionaries and the blobs of a segment
 * which dictionaries are written, cached or built
 * the layout of the blobs in the file
 */
#include "core/host.h"
#include "core/limits.h"
#include "col/seg_write/seg_write_int.h"

#define CDICT_TSIG_MIN_KB	1024

/* the dictionaries written in blocks that their leaves did not ask for (DICT_BLOCKS, not DICT_LOOKUP) add together at most 1/33 (3%) of the segment (what it wrote before the dictionaries, and these in the compact form) */
#define CDICT_BLOCKS_BUDGET_DENOM	33

/* an estimate of the blocks over this share of the compact form (1/6) is not worth serializing them for */
#define CDICT_BLOCKS_CLEAR_DENOM	6

/*
 * Whether the dictionary of leaf c no longer fits the cache of parsed dictionaries (dict_cache_kb): its strings, parsed and scaled to the rows of the index (the
 * segments of an index are alike), pass the cache, or this segment's alone pass the half of it that a single dictionary may take. A dictionary that fits is
 * parsed once and kept; one that does not is read, and unwrapped, again by every test that needs it
 */
static bool
dict_uncached(const SiftColSegWriter *w, int c)
{
	Size		cache = sift_cdict_cache_budget(),
				parsed = sift_cdict_parsed_bytes(w->dict[c]->nbytes, w->dict[c]->n);
	double		scale = Max((double) w->index_rows / (double) Max(sift_colw_rows(w), 1), 1.0);

	return parsed > cache / 2 || (double) parsed * scale > (double) cache;
}

/*
 * Whether the dictionary of leaf c gets the trigram signatures (and so the blocks form, whatever the budget of the blocks): when it no longer fits the cache, and
 * it holds dict_tsig_min bytes of strings of 5 bytes or more in the segment. An index that fits gets none
 */
static bool
tsig_wanted(const SiftColSegWriter *w, int c)
{
	if (w->setting < SIFT_COL_FORMAT_PRESENCE || w->dict[c]->nbytes < (Size) sift_limit(dict_tsig_min, CDICT_TSIG_MIN_KB) * 1024 ||
		w->dict[c]->nbytes < (Size) 5 * w->dict[c]->n)
		return false;
	return dict_uncached(w, c);
}

void
segw_write_dicts(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64	   *offs = palloc0(sizeof(uint32) * 2 + ncols * sizeof(uint64));
	uint32	   *hdr = (uint32 *) offs;
	uint64	   *tab = (uint64 *) (hdr + 2);
	Size	   *extra = palloc0(ncols * sizeof(Size)),
			   *compact = palloc0(ncols * sizeof(Size)),
				budget = sift_sw_tell(w->sw);
	bool		blocks = w->setting >= SIFT_COL_FORMAT_DICTIDX && !w->inexact;
	bool	   *asked = palloc0(ncols * sizeof(bool));
	const SiftCcPolicy *pol;
	StringInfoData blob;

	segw_sw_pad8(w->sw);
	sec->off = sift_sw_tell(w->sw);
	hdr[0] = DICTS_MAGIC;
	hdr[1] = ncols;
	/*
	 * A dictionary of many strings that did not ask for the order is stored in blocks, to be read in place, when that adds little to the segment (the
	 * budget): ids, hashes and uuids compress as well in blocks as whole, short strings that repeat their words do not. The estimates from a sample
	 * decide what is clear (they run high for the blocks: an estimate within the budget is), and the blocks are serialized next to the compact form
	 * only for a dictionary in between
	 */
	for (int c = 0; c < ncols; c++)
		if (w->dict[c] != NULL)
		{
			w->dict[c]->scan = w->setting >= SIFT_COL_FORMAT_PRESENCE && dict_uncached(w, c);
			w->dict[c]->leaf1 = c + 1;
		}
	for (int c = 0; blocks && c < ncols; c++)
	{
		if (w->dict[c] == NULL || w->dict[c]->n < sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
			extra[c] = SIZE_MAX;
		else
			extra[c] = sift_cdict_blocks_extra(w->dict[c], w->pol ? &w->pol[c] : NULL, &compact[c]);
		asked[c] = w->dform && w->dform[c];
		budget += compact[c];
	}
	budget /= CDICT_BLOCKS_BUDGET_DENOM;
	/* the directory is written last to know the offsets: serialize the dictionaries first into memory */
	initStringInfo(&blob);
	for (int c = 0; c < ncols; c++)
	{
		uint32		form = w->dform ? w->dform[c] : 0;

		if (w->dict[c] == NULL)
			continue;
		while (blob.len % 8)
			appendStringInfoChar(&blob, '\0');
		tab[c] = sizeof(uint32) * 2 + ncols * sizeof(uint64) + blob.len;
		if (extra[c] != SIZE_MAX && blocks && !asked[c])
		{
			if (extra[c] <= budget || tsig_wanted(w, c))
			{
				form = DICT_BLOCKS;
				budget -= Min(extra[c], budget);
			}
			else if (extra[c] <= compact[c] / CDICT_BLOCKS_CLEAR_DENOM)
			{
				StringInfoData whole,
							alt;

				initStringInfo(&whole);
				initStringInfo(&alt);
				sift_cdict_serialize(w->dict[c], w->pol ? &w->pol[c] : NULL, 0, &whole);
				sift_cdict_serialize(w->dict[c], w->pol ? &w->pol[c] : NULL, DICT_BLOCKS, &alt);
				if (alt.len <= whole.len + budget)
				{
					budget -= Max(alt.len, whole.len) - whole.len;
					appendBinaryStringInfo(&blob, alt.data, alt.len);
					w->format = Max(w->format, w->dict[c]->lz4_blocks ? SIFT_COL_FORMAT_TSIG : SIFT_COL_FORMAT_DICTIDX);
				}
				else
					appendBinaryStringInfo(&blob, whole.data, whole.len);
				pfree(whole.data);
				pfree(alt.data);
				continue;
			}
		}
		/* a dictionary in blocks that no leaf asked to search keeps the hash alone (5 bytes a string): the first equality of a backend finds its code in a few pages, not by parsing the dictionary */
		if (form == DICT_BLOCKS && w->pagg.dict_hash && w->setting >= SIFT_COL_FORMAT_PRESENCE && w->dict[c]->n >= sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
		{
			w->dict[c]->hashed = true;
			w->format = Max(w->format, SIFT_COL_FORMAT_HASH);
		}
		if (form == DICT_BLOCKS)
			w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
		/* a dictionary of long strings read in blocks gets the trigram signature of each (a substring test then reads the blocks that may hold the part) */
		w->dict[c]->tsig = (form == DICT_BLOCKS || form == DICT_LOOKUP || form == DICT_LOOKUP_COUNT) && tsig_wanted(w, c);
		if (w->dict[c]->tsig)
			w->format = Max(w->format, SIFT_COL_FORMAT_TSIG);
		pol = w->pol ? &w->pol[c] : NULL;
		sift_cdict_serialize(w->dict[c], pol, form, &blob);
		if (w->dict[c]->lz4_blocks)
			w->format = Max(w->format, SIFT_COL_FORMAT_TSIG);
	}
	pfree(extra);
	pfree(compact);
	pfree(asked);
	sift_sw_write(w->sw, offs, sizeof(uint32) * 2 + ncols * sizeof(uint64));
	sift_sw_write(w->sw, blob.data, blob.len);
	segw_sw_pad8(w->sw);
	segw_write_points(w, sec);
	sec->len = sift_sw_tell(w->sw) - sec->off;
	pfree(blob.data);
	pfree(offs);
}

/*
 * The records of the leaves of acc that have blobs (the small-set summaries, or the sorted runs) and where the section of them ends up, from at (8-aligned,
 * from the start of the header): *start and *bytes are what the header says of it
 */
SetsLeaf *
segw_layout_blobs(SiftColSegWriter *w, const SetAcc *acc, Size at, Size *start, Size *bytes, int *nleaves)
{
	int			n = 0;
	SetsLeaf   *sl = palloc0(w->schema->ncols * sizeof(SetsLeaf));

	*start = at;
	for (int c = 0; c < w->schema->ncols; c++)
		if (acc[c].any)
			n++;
	at += sizeof(SetsHeader) + n * sizeof(SetsLeaf);
	n = 0;
	for (int c = 0; c < w->schema->ncols; c++)
		if (acc[c].any)
		{
			at = ALIGN8(at);
			sl[n++] = (SetsLeaf) {c, acc[c].kinds, at, acc[c].blobs.len};
			at += (w->nsb + 1) * sizeof(uint64) + acc[c].blobs.len;
		}
	*bytes = ALIGN8(at) - *start;
	*nleaves = n;
	return sl;
}

void
segw_write_blobs(SiftColSegWriter *w, const SetAcc *acc, uint32 magic, SetsLeaf *sl, int n)
{
	SetsHeader	sh = {magic, 1, n, w->nsb};

	segw_sw_pad8(w->sw);
	sift_sw_write(w->sw, &sh, sizeof(sh));
	sift_sw_write(w->sw, sl, n * sizeof(SetsLeaf));
	for (int i = 0; i < n; i++)
	{
		const SetAcc *a = &acc[sl[i].leaf];

		segw_sw_pad8(w->sw);
		sift_sw_write(w->sw, a->off, (Size) (w->nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, a->blobs.data, a->blobs.len);
	}
	segw_sw_pad8(w->sw);
	pfree(sl);
}
