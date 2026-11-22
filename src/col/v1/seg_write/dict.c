/*
 * col/v1/seg_write/dict.c - the dictionaries and the blobs of a segment
 * which dictionaries are written, cached or built
 * the layout of the blobs in the file
 */
#include "core/limits.h"
#include "core/memtrack.h"
#include "col/v1/seg_write/seg_write_int.h"

#define CDICT_TSIG_MIN_KB	1024

/* the dictionaries written in blocks that their leaves did not ask for (DICT_BLOCKS, not DICT_LOOKUP) add together at most 1/33 (3%) of the segment (what it wrote before the dictionaries, and these in the compact form) */
#define CDICT_BLOCKS_BUDGET_DENOM	33

/* an estimate of the blocks over this share of the compact form (1/6) is not worth serializing them for */
#define CDICT_BLOCKS_CLEAR_DENOM	6

/*
 * Whether the dictionary of leaf c no longer fits the cache of parsed dictionaries (dict_cache_kb): its strings, parsed and scaled to the rows of the index (the
 * segments of an index are alike), pass pct per cent of the cache, or this segment's alone pass the half of it that a single dictionary may take. A dictionary that
 * fits is parsed once and kept; one that does not is read, and unwrapped, again by every test that needs it
 */
bool
segw_dict_uncached(const SiftColSegWriter *w, int c, int pct)
{
	Size		cache = sift_cdict_cache_budget(),
				parsed = sift_cdict_parsed_bytes(w->dict[c]->nbytes, w->dict[c]->n);
	double		scale = Max((double) w->index_rows / (double) Max(sift_colw_rows(w), 1), 1.0);

	return parsed > cache / 2 || (double) parsed * scale * 100.0 > (double) cache * pct;
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
	return segw_dict_uncached(w, c, 100);
}

/* what the dictionaries of a segment decide among them: the bytes the blocks form may add to the segment, and for each leaf the estimates of the forms */
typedef struct DictPlan
{
	Size	   *extra;
	Size	   *compact;
	bool	   *asked;
	Size		budget;
	bool		blocks;
} DictPlan;

/* the dictionary of leaf c in its form into one: the serialization of a single dictionary is all that is in memory, the others are in the spool already */
static void
serialize_dict(SiftColSegWriter *w, int c, DictPlan *plan, StringInfo one)
{
	uint32		form = w->dform ? w->dform[c] : 0;
	const SiftCcPolicy *pol = w->pol ? &w->pol[c] : NULL;

	if (plan->extra[c] != SIZE_MAX && plan->blocks && !plan->asked[c])
	{
		if (plan->extra[c] <= plan->budget || tsig_wanted(w, c))
		{
			form = DICT_BLOCKS;
			plan->budget -= Min(plan->extra[c], plan->budget);
		}
		else if (plan->extra[c] <= plan->compact[c] / CDICT_BLOCKS_CLEAR_DENOM)
		{
			StringInfoData alt;

			initStringInfo(&alt);
			sift_cdict_serialize(w->dict[c], pol, 0, one);
			sift_cdict_serialize(w->dict[c], pol, DICT_BLOCKS, &alt);
			if (alt.len <= (Size) one->len + plan->budget)
			{
				plan->budget -= Max((Size) alt.len, (Size) one->len) - one->len;
				resetStringInfo(one);
				appendBinaryStringInfo(one, alt.data, alt.len);
				w->format = Max(w->format, w->dict[c]->lz4_blocks ? SIFT_COL_FORMAT_TSIG : SIFT_COL_FORMAT_DICTIDX);
			}
			pfree(alt.data);
			return;
		}
	}
	/* a dictionary in blocks that no leaf asked to search keeps the hash alone (5 bytes a string): the first equality of a backend finds its code in a few pages, not by parsing the dictionary; the blocks the order of a text leaf asked for are read by code and keep none */
	if (form == DICT_BLOCKS && !plan->asked[c] && w->pagg.dict_hash && w->setting >= SIFT_COL_FORMAT_PRESENCE && w->dict[c]->n >= sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
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
	sift_cdict_serialize(w->dict[c], pol, form, one);
	if (w->dict[c]->lz4_blocks)
		w->format = Max(w->format, SIFT_COL_FORMAT_TSIG);
}

typedef struct DictSpool
{
	StringInfoData mem;
	BufFile    *file;
	Size		limit;
	uint64		total;
} DictSpool;

/* the serialized dictionaries wait in memory up to limit bytes and in a temporary file past it */
static void
spool_write(DictSpool *sp, const void *data, Size len)
{
	if (sp->file == NULL && sp->mem.len + len > sp->limit)
	{
		sp->file = BufFileCreateTemp(false);
		BufFileWrite(sp->file, sp->mem.data, sp->mem.len);
		resetStringInfo(&sp->mem);
	}
	if (sp->file != NULL)
		BufFileWrite(sp->file, data, len);
	else
		appendBinaryStringInfo(&sp->mem, data, len);
	sp->total += len;
}

static void
spool_flush(DictSpool *sp, SiftStreamWriter *sw)
{
	if (sp->file != NULL)
	{
		sift_sw_write_file(sw, sp->file, sp->total);
		BufFileClose(sp->file);
	}
	else
		sift_sw_write(sw, sp->mem.data, sp->mem.len);
	pfree(sp->mem.data);
}

void
segw_write_dicts(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64	   *offs = palloc0(sizeof(uint32) * 2 + ncols * sizeof(uint64));
	uint32	   *hdr = (uint32 *) offs;
	uint64	   *tab = (uint64 *) (hdr + 2);
	DictPlan	plan = {.extra = palloc0(ncols * sizeof(Size)), .compact = palloc0(ncols * sizeof(Size)), .budget = sift_sw_tell(w->sw),
						.blocks = w->setting >= SIFT_COL_FORMAT_DICTIDX && !w->inexact, .asked = palloc0(ncols * sizeof(bool))};
	DictSpool	spool = {.limit = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) / 4};
	StringInfoData one;
	static const char zeros[8];

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
			w->dict[c]->scan = w->setting >= SIFT_COL_FORMAT_PRESENCE && segw_dict_uncached(w, c, 100);
			w->dict[c]->leaf1 = c + 1;
		}
	for (int c = 0; plan.blocks && c < ncols; c++)
	{
		if (w->dict[c] == NULL || w->dict[c]->n < sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
			plan.extra[c] = SIZE_MAX;
		else
			plan.extra[c] = sift_cdict_blocks_extra(w->dict[c], w->pol ? &w->pol[c] : NULL, &plan.compact[c]);
		plan.asked[c] = w->dform && w->dform[c];
		plan.budget += plan.compact[c];
	}
	plan.budget /= CDICT_BLOCKS_BUDGET_DENOM;
	/* the directory is written last to know the offsets: the dictionaries wait in the spool, one serialization in memory at a time besides it */
	initStringInfo(&one);
	initStringInfo(&spool.mem);
	for (int c = 0; c < ncols; c++)
	{
		if (w->dict[c] == NULL)
			continue;
		if (spool.total % 8)
			spool_write(&spool, zeros, 8 - spool.total % 8);
		tab[c] = sizeof(uint32) * 2 + ncols * sizeof(uint64) + spool.total;
		resetStringInfo(&one);
		serialize_dict(w, c, &plan, &one);
		spool_write(&spool, one.data, one.len);
	}
	pfree(one.data);
	pfree(plan.extra);
	pfree(plan.compact);
	pfree(plan.asked);
	sift_sw_write(w->sw, offs, sizeof(uint32) * 2 + ncols * sizeof(uint64));
	spool_flush(&spool, w->sw);
	segw_sw_pad8(w->sw);
	segw_write_points(w, sec);
	sec->len = sift_sw_tell(w->sw) - sec->off;
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
	segw_write_mirrored(w, MIR_SETS, &sh, sizeof(sh));
	segw_write_mirrored(w, MIR_SETS, sl, n * sizeof(SetsLeaf));
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
