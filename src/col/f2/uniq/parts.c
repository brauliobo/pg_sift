/*
 * col/f2/uniq/parts.c - the keys of the small parts of a UNIQUE index, for its writers (f2_uniq_parts.h).
 *
 * A part is written once and never changed, so what a backend reads of one stays right for as long as the manifest names it: the 32-bit tag of the hash of each key of a small part goes to
 * one Bloom filter of the index and to a Bloom filter of the part, and the part is covered. The manifest a probe hands to the scan is the one without the covered parts when the filter of the
 * index does not hold the tag, and else the one with the covered parts whose own filter holds it. Tags of parts a compaction replaced stay (a tag that is not a key of a covered part only makes a
 * probe look at the filters of the parts), and the filter of the index is made again from the parts of the manifest when it holds twice the keys they stand for.
 *
 * The filters are kept for at most FILTERS indexes, under the budget of the backend caches, the one of the index at most UNION_BLOCKS_MAX blocks and 1/UNION_BITS_PER_KEY of its bits in keys; a part
 * that does not fit stays out of them and is read by the scan as before.
 *
 * check-memory: allow-allocs the filter of the index is at most UNION_BLOCKS_MAX blocks, the parts it covers at most COVERED_MAX with a filter of BLOOM_BITS_PER_KEY bits for each of their at most PART_ROWS_MAX keys, and the arrays of the manifest follow the parts of one index
 */
#include "postgres.h"

#include "access/transam.h"
#include "utils/memutils.h"

#include "col/f2/f2.h"
#include "col/f2/part/pin_scan.h"
#include "col/f2/uniq/parts.h"
#include "col/granule/granule.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

#define FILTERS			2
#define PART_ROWS_MAX	16384		/* a part of more rows is read by the scan: the keys of a compacted part are not worth the heap reads */
#define COVERED_MAX		4096
#define STALE_SLACK		4096
#define BLOOM_BITS_PER_KEY	16
#define BLOOM_PROBES	4
#define UNION_BLOCK_WORDS	8		/* a block of the filter of the index is a cache line */
#define UNION_BLOCKS_MIN	1024
#define UNION_BLOCKS_MAX	65536
#define UNION_BITS_PER_KEY	8		/* the keys it takes: its bits over this, so that a tag it does not hold is wrongly found in 3 of 100 */

/* A part by what names it: its generation, and the block and first transaction that tell a part of a file that began again from one of the same generation */
typedef struct PartId
{
	uint64		gen;
	uint64		xmin;
	BlockNumber blk;
} PartId;

typedef struct Covered
{
	PartId		id;
	int			partition;		/* in the directory of the manifest the filter has seen */
	SiftPartDesc desc;
	uint64	   *bloom;
	uint32		mask;			/* the filter has mask + 1 bits, a power of two */
} Covered;

typedef struct Filter
{
	RelFileLocator locator;
	uint64		used;
	MemoryContext cx;
	MemoryContext rest_cx;
	uint64	   *ubits;			/* the filter of the index: ublocks blocks of UNION_BLOCK_WORDS words, a power of two of them */
	uint32		ublocks;
	uint32		count;			/* the tags put in it */
	bool		full;
	Covered    *covered;		/* ascending by generation */
	int			ncovered;
	uint64		version;		/* of the manifest rest was made from */
	uint64		scope_gen;
	SiftPlanSnapshot *rest;
} Filter;

static Filter *filters[FILTERS];
static int	nfilters;
static bool registered;

static uint32
tag_of(uint64 fp)
{
	uint32		tag = (uint32) (fp >> 32);

	return tag != 0 ? tag : 1;
}

static uint64
tag_mix(uint32 tag)
{
	uint64		h = tag * UINT64CONST(0x9E3779B97F4A7C15);

	h ^= h >> 32;
	h *= UINT64CONST(0xD6E8FEB86659FD93);
	return h ^ (h >> 29);
}

static uint64 *
union_block(const Filter *f, uint64 h)
{
	return &f->ubits[(h & (f->ublocks - 1)) * UNION_BLOCK_WORDS];
}

static bool
union_has(const Filter *f, uint32 tag)
{
	const uint64 h = tag_mix(tag);
	const uint64 *block = union_block(f, h);

	for (int j = 0; j < BLOOM_PROBES; j++)
	{
		const uint32 b = (h >> (20 + 9 * j)) & 511;

		if ((block[b >> 6] & (UINT64CONST(1) << (b & 63))) == 0)
			return false;
	}
	return true;
}

static void
union_put(Filter *f, uint32 tag)
{
	const uint64 h = tag_mix(tag);
	uint64	   *block = union_block(f, h);

	for (int j = 0; j < BLOOM_PROBES; j++)
	{
		const uint32 b = (h >> (20 + 9 * j)) & 511;

		block[b >> 6] |= UINT64CONST(1) << (b & 63);
	}
	f->count++;
}

static uint32
union_capacity(const Filter *f)
{
	return (uint64) f->ublocks * UNION_BLOCK_WORDS * 64 / UNION_BITS_PER_KEY;
}

/* ---- the filter of a part ---- */

static uint32
bloom_bit(uint32 tag, int j, uint32 mask)
{
	const uint32 h1 = tag * UINT32_C(0x9E3779B1),
				h2 = (tag * UINT32_C(0x85EBCA6B)) | 1;

	return (h1 + j * h2) & mask;
}

/* The filter of n tags, NULL where the budget of the caches has no room for it */
static uint64 *
bloom_make(Filter *f, const uint32 *tags, uint32 n, uint32 *mask)
{
	uint64		bits = 64;
	uint64	   *words;

	while (bits < (uint64) n * BLOOM_BITS_PER_KEY)
		bits <<= 1;
	if (!sift_cache_room(bits / 8))
		return NULL;
	words = MemoryContextAllocZero(f->cx, bits / 8);
	*mask = (uint32) (bits - 1);
	for (uint32 i = 0; i < n; i++)
		for (int j = 0; j < BLOOM_PROBES; j++)
		{
			const uint32 b = bloom_bit(tags[i], j, *mask);

			words[b >> 6] |= UINT64CONST(1) << (b & 63);
		}
	return words;
}

static bool
bloom_has(const Covered *c, uint32 tag)
{
	for (int j = 0; j < BLOOM_PROBES; j++)
	{
		const uint32 b = bloom_bit(tag, j, c->mask);

		if ((c->bloom[b >> 6] & (UINT64CONST(1) << (b & 63))) == 0)
			return false;
	}
	return true;
}

/* ---- the parts ---- */

static PartId
part_id(const SiftPartDesc *d)
{
	return (PartId) {.gen = d->part_gen,.xmin = U64FromFullTransactionId(d->min_xid),.blk = d->header_blk};
}

static int
covered_cmp(const void *a, const void *b)
{
	const uint64 x = ((const Covered *) a)->id.gen,
				y = ((const Covered *) b)->id.gen;

	return x < y ? -1 : x > y;
}

static Covered *
covered_find(const Filter *f, const PartId *id)
{
	const Covered key = {.id = *id};
	Covered    *c = f->ncovered > 0 ? bsearch(&key, f->covered, f->ncovered, sizeof(Covered), covered_cmp) : NULL;

	return c != NULL && c->id.xmin == id->xmin && c->id.blk == id->blk ? c : NULL;
}

static bool
eligible(const SiftPartDesc *d)
{
	return d->nrows <= PART_ROWS_MAX;
}

typedef struct PartTags
{
	uint32	   *tag;
	uint32		n;
	uint32		cap;
} PartTags;

static bool
part_tag_put(void *arg, uint64 fp)
{
	PartTags   *t = arg;

	if (t->n == t->cap)
		return false;
	t->tag[t->n++] = tag_of(fp);
	return true;
}

/* The tags of the keys of one part, from the heap tuples its rows name; false when the part holds more rows than it says */
static bool
read_part(Relation index, Relation heap, uint64 partition_id, const SiftPartDesc *d, SiftUniqSlabKeyFn key_fp, void *arg, PartTags *tags)
{
	SiftF2PartPin *pin = sift_f2_scan_pin(index, d, (uint32) partition_id);
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift unique part keys");
	MemoryContext old = MemoryContextSwitchTo(cx);
	bool		room = true;

	PG_TRY();
	{
		for (uint32 g = 0; g < pin->part->hdr.ngranules && room; g++)
		{
			const uint32 n = pin->gdir[g].nrows;
			uint64	   *addr = sift_part_read_tids(index, pin->part, pin->gdir, g);
			ItemPointerData *tids = palloc(Max(n, 1) * sizeof(ItemPointerData));

			for (uint32 i = 0; i < n; i++)
				sift_f2_tid_set(&tids[i], addr[i]);
			room = sift_uniq_fold_tids(heap, tids, n, key_fp, arg, part_tag_put, tags);
			MemoryContextReset(cx);
		}
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return room;
}

/* Reads a part not covered yet into c, its tags into the filter of the index and its own filter; false when they have no room (the filter of the index is full from then on) */
static bool
cover(Filter *f, Relation index, Relation heap, uint64 partition_id, int partition, const SiftPartDesc *d, SiftUniqSlabKeyFn key_fp, void *arg, Covered *c)
{
	PartTags	tags = {.cap = d->nrows};
	bool		ok;

	tags.tag = palloc(Max(tags.cap, 1) * sizeof(uint32));
	ok = read_part(index, heap, partition_id, d, key_fp, arg, &tags);
	ok = ok && f->count + tags.n <= union_capacity(f);
	c->bloom = ok ? bloom_make(f, tags.tag, tags.n, &c->mask) : NULL;
	ok = c->bloom != NULL;
	for (uint32 i = 0; ok && i < tags.n; i++)
		union_put(f, tags.tag[i]);
	f->full |= !ok;
	if (ok)
	{
		c->id = part_id(d);
		c->partition = partition;
		c->desc = *d;
	}
	pfree(tags.tag);
	return ok;
}

/* Lets go of the filters of the parts in old (ascending by generation) that now (the same) does not hold */
static void
covered_free_unless(const Covered *old, int nold, const Covered *now, int nnow)
{
	int			j = 0;

	for (int i = 0; i < nold; i++)
	{
		while (j < nnow && now[j].id.gen < old[i].id.gen)
			j++;
		if (j == nnow || now[j].bloom != old[i].bloom)
			pfree(old[i].bloom);
	}
}

/* The filter of the index made again, of blocks blocks, with no part covered */
static void
union_reset(Filter *f, uint32 blocks)
{
	const Size	bytes = (Size) blocks * UNION_BLOCK_WORDS * sizeof(uint64);

	covered_free_unless(f->covered, f->ncovered, NULL, 0);
	f->ncovered = 0;
	if (f->ubits != NULL)
	{
		pfree(f->ubits);
		f->ubits = NULL;
	}
	f->count = 0;
	f->full = !sift_cache_room(bytes);
	if (!f->full)
	{
		f->ubits = MemoryContextAllocZero(f->cx, bytes);
		f->ublocks = blocks;
	}
}

/* A copy of plan in cx without the covered parts */
static SiftPlanSnapshot *
rest_of(const SiftPlanSnapshot *plan, const Filter *f, MemoryContext cx)
{
	SiftPlanSnapshot *rest = MemoryContextAlloc(cx, sizeof(SiftPlanSnapshot));

	*rest = *plan;
	rest->partitions = MemoryContextAlloc(cx, Max(plan->npartitions, 1) * sizeof(SiftPartitionEntry));
	memcpy(rest->partitions, plan->partitions, plan->npartitions * sizeof(SiftPartitionEntry));
	rest->partlists = MemoryContextAlloc(cx, Max(plan->npartitions, 1) * sizeof(SiftPlanPartList));
	for (int i = 0; i < plan->npartitions; i++)
	{
		const SiftPlanPartList *from = &plan->partlists[i];
		SiftPlanPartList *to = &rest->partlists[i];

		*to = *from;
		to->parts = MemoryContextAlloc(cx, Max(from->nparts, 1) * sizeof(SiftPartDesc));
		to->nparts = 0;
		for (int p = 0; p < from->nparts; p++)
		{
			PartId		id = part_id(&from->parts[p]);

			if (covered_find(f, &id) == NULL)
				to->parts[to->nparts++] = from->parts[p];
		}
	}
	rest->part_id_ht = NULL;
	rest->part_id_mask = 0;
	rest->nscopes = 0;
	rest->scopes = NULL;
	rest->card_view = NULL;
	return rest;
}

/* The filter of the index is made again when it holds twice the keys of the parts it may take (a compaction replaced the others), or is too small for them: its blocks are 32 bits for each of their rows, at most UNION_BLOCKS_MAX */
static void
union_fit(Filter *f, const SiftPlanSnapshot *plan)
{
	uint64		rows = 0;
	uint32		blocks = UNION_BLOCKS_MIN;

	for (int i = 0; i < plan->npartitions; i++)
		for (int p = 0; p < plan->partlists[i].nparts; p++)
			if (eligible(&plan->partlists[i].parts[p]))
				rows += plan->partlists[i].parts[p].nrows;
	while (blocks < UNION_BLOCKS_MAX && (uint64) blocks * UNION_BLOCK_WORDS * 64 < rows * 4 * UNION_BITS_PER_KEY)
		blocks <<= 1;
	if (f->ubits == NULL || (!f->full && blocks > f->ublocks) || f->count > 2 * rows + STALE_SLACK)
		union_reset(f, blocks);
}

/* Reads the parts of plan the table does not cover, and makes the manifest of the rest */
static void
refresh(Filter *f, Relation index, Relation heap, const SiftPlanSnapshot *plan, SiftUniqSlabKeyFn key_fp, void *arg)
{
	Covered    *now;
	int			n = 0,
				nparts = 0;

	for (int i = 0; i < plan->npartitions; i++)
		nparts += plan->partlists[i].nparts;
	now = palloc(Max(Min(nparts, COVERED_MAX), 1) * sizeof(Covered));
	f->rest = NULL;
	union_fit(f, plan);
	for (int i = 0; i < plan->npartitions; i++)
		for (int p = 0; p < plan->partlists[i].nparts && n < COVERED_MAX; p++)
		{
			const SiftPartDesc *d = &plan->partlists[i].parts[p];
			PartId		id = part_id(d);
			Covered    *known = eligible(d) ? covered_find(f, &id) : NULL;

			if (known != NULL)
			{
				now[n] = *known;
				now[n].partition = i;
				now[n++].desc = *d;
			}
			else if (eligible(d) && !f->full && cover(f, index, heap, plan->partitions[i].part_id, i, d, key_fp, arg, &now[n]))
				n++;
		}
	qsort(now, n, sizeof(Covered), covered_cmp);
	covered_free_unless(f->covered, f->ncovered, now, n);
	MemoryContextReset(f->rest_cx);
	if (f->covered != NULL)
		pfree(f->covered);
	f->covered = MemoryContextAlloc(f->cx, Max(n, 1) * sizeof(Covered));
	memcpy(f->covered, now, n * sizeof(Covered));
	f->ncovered = n;
	pfree(now);
	f->rest = rest_of(plan, f, f->rest_cx);
	f->version = plan->manifest_version;
	f->scope_gen = plan->scope_dir_gen;
}

/* The manifest of the rest and the covered parts whose filter holds the tag, a copy in cx */
static const SiftPlanSnapshot *
narrowed(const Filter *f, uint32 tag, MemoryContext cx)
{
	const SiftPlanSnapshot *rest = f->rest;
	SiftPlanSnapshot *plan;
	int		   *pick = palloc(Max(f->ncovered, 1) * sizeof(int));
	int			npick = 0;

	for (int i = 0; i < f->ncovered; i++)
		if (bloom_has(&f->covered[i], tag))
			pick[npick++] = i;
	if (npick == 0)
	{
		pfree(pick);
		return rest;
	}
	plan = MemoryContextAlloc(cx, sizeof(SiftPlanSnapshot));
	*plan = *rest;
	plan->partlists = MemoryContextAlloc(cx, Max(rest->npartitions, 1) * sizeof(SiftPlanPartList));
	memcpy(plan->partlists, rest->partlists, rest->npartitions * sizeof(SiftPlanPartList));
	for (int i = 0; i < rest->npartitions; i++)
	{
		SiftPlanPartList *list = &plan->partlists[i];
		SiftPartDesc *parts = MemoryContextAlloc(cx, (list->nparts + npick) * sizeof(SiftPartDesc));

		memcpy(parts, list->parts, list->nparts * sizeof(SiftPartDesc));
		for (int k = 0; k < npick; k++)
			if (f->covered[pick[k]].partition == i)
				parts[list->nparts++] = f->covered[pick[k]].desc;
		list->parts = parts;
	}
	pfree(pick);
	return plan;
}

/* ---- the filters of the backend ---- */

static void
filter_drop(int i)
{
	MemoryContextDelete(filters[i]->cx);
	filters[i] = filters[--nfilters];
}

static int
filter_oldest(void)
{
	int			victim = -1;

	for (int i = 0; i < nfilters; i++)
		if (!sift_cache_used_in_xact(filters[i]->used) && (victim < 0 || filters[i]->used < filters[victim]->used))
			victim = i;
	return victim;
}

static uint64
filters_oldest(void)
{
	int			i = filter_oldest();

	return i < 0 ? UINT64_MAX : filters[i]->used;
}

static bool
filters_evict_one(void)
{
	int			i = filter_oldest();

	if (i < 0)
		return false;
	filter_drop(i);
	return true;
}

static void
filters_evict_all(void)
{
	while (filters_evict_one())
		;
}

static const SiftCacheKind filters_kind = {"col_f2_uniq_parts", filters_oldest, filters_evict_one, filters_evict_all};

static Filter *
filter_make(Relation index)
{
	MemoryContext cx = sift_cache_create_context("sift cache unique part keys");
	Filter	   *f = MemoryContextAllocZero(cx, sizeof(Filter));

	f->cx = cx;
	f->rest_cx = sift_cache_create_context_in(cx, "sift cache unique part keys rest");
	f->locator = index->rd_locator;
	return f;
}

static Filter *
filter_get(Relation index)
{
	Filter	   *f;

	for (int i = 0; i < nfilters; i++)
		if (RelFileLocatorEquals(filters[i]->locator, index->rd_locator))
			return filters[i];
	if (!registered)
	{
		sift_cache_register(&filters_kind);
		registered = true;
	}
	if (nfilters == FILTERS)
	{
		int			lru = 0;

		for (int i = 1; i < nfilters; i++)
			lru = filters[i]->used < filters[lru]->used ? i : lru;
		filter_drop(lru);
	}
	f = filter_make(index);
	filters[nfilters++] = f;
	return f;
}

void
sift_uniq_parts_forget(const RelFileLocator *locator)
{
	for (int i = nfilters - 1; i >= 0; i--)
		if (RelFileLocatorEquals(filters[i]->locator, *locator))
			filter_drop(i);
}

const SiftPlanSnapshot *
sift_uniq_parts_prune(Relation index, Relation heap, const SiftPlanSnapshot *plan, uint64 fp64, SiftUniqSlabKeyFn key_fp, void *arg, MemoryContext cx)
{
	Filter	   *f = filter_get(index);
	const uint32 tag = tag_of(fp64);

	f->used = sift_cache_tick();
	if (f->rest == NULL || plan->manifest_version != f->version || plan->scope_dir_gen != f->scope_gen)
		refresh(f, index, heap, plan, key_fp, arg);
	return f->ubits != NULL && union_has(f, tag) ? narrowed(f, tag, cx) : f->rest;
}
