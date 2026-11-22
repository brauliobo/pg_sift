/*
 * core/tidbridge.c - page masks and offset containers per superblock (core/tidbridge.h).
 *
 * While building, the records of the exact pages are in the order the TIDs came: a map from block to record finds a page, a map from superblock to entry finds the
 * masks, and an ARRAY page has SIFT_TIDP_ARRAY_MAX slots so that an offset is a sorted insert. Finalize puts the records in block order, packs the arrays and
 * frees the maps: a probe is then a binary search of the superblocks, a popcount rank of the page among the exact pages of its superblock and a search in the container.
 * A page the byte budget cannot hold, or an ARRAY page that outgrows it, sets its bit in lossy_mask and keeps no payload.
 */
#include "core/host.h"

#include "core/cmp.h"
#include "core/hash.h"
#include "core/tidbridge.h"

#define NONE	PG_UINT32_MAX

#define PRODUCER_FLAGS	(SIFT_TBR_EXACT_QUALS | SIFT_TBR_EXACT_VIS | SIFT_TBR_HAS_SCORES | SIFT_TBR_HAS_ROWLOC | SIFT_TBR_HAS_HOT_CHAINS)
#define SIDE_FLAGS		(SIFT_TBR_HAS_SCORES | SIFT_TBR_HAS_ROWLOC)

static inline int
page_of(BlockNumber blk)
{
	return blk & (SIFT_SB_PAGES - 1);
}

static inline uint64
page_bit(int page)
{
	return UINT64CONST(1) << (page & 63);
}

static uint32
map_slot(const SiftTidBridgeMap *m, uint32 key)
{
	return (uint32) (sift_fib64(key) >> 32) & m->mask;
}

static uint32
map_find(const SiftTidBridgeMap *m, uint32 key)
{
	if (m->key == NULL)
		return NONE;
	for (uint32 i = map_slot(m, key); m->val[i] != NONE; i = (i + 1) & m->mask)
		if (m->key[i] == key)
			return m->val[i];
	return NONE;
}

static void
map_insert(SiftTidBridgeMap *m, uint32 key, uint32 val)
{
	uint32		i = map_slot(m, key);

	while (m->val[i] != NONE)
		i = (i + 1) & m->mask;
	m->key[i] = key;
	m->val[i] = val;
	m->n++;
}

static void
map_put(MemoryContext mcxt, SiftTidBridgeMap *m, uint32 key, uint32 val)
{
	if (m->key == NULL || (uint64) (m->n + 1) * 2 > (uint64) m->mask + 1)
	{
		SiftTidBridgeMap old = *m;
		uint64		slots = old.key == NULL ? 64 : 2 * ((uint64) old.mask + 1);

		m->key = MemoryContextAllocHuge(mcxt, slots * sizeof(uint32));
		m->val = MemoryContextAllocHuge(mcxt, slots * sizeof(uint32));
		memset(m->val, 0xff, slots * sizeof(uint32));
		m->mask = slots - 1;
		m->n = 0;
		for (uint64 i = 0; old.key != NULL && i <= old.mask; i++)
			if (old.val[i] != NONE)
				map_insert(m, old.key[i], old.val[i]);
		if (old.key != NULL)
		{
			pfree(old.key);
			pfree(old.val);
		}
	}
	map_insert(m, key, val);
}

static void
map_clear(SiftTidBridgeMap *m)
{
	if (m->key != NULL)
		memset(m->val, 0xff, ((Size) m->mask + 1) * sizeof(uint32));
	m->n = 0;
}

static void
map_free(SiftTidBridgeMap *m)
{
	if (m->key != NULL)
	{
		pfree(m->key);
		pfree(m->val);
	}
	memset(m, 0, sizeof(*m));
}

static Size
map_bytes(const SiftTidBridgeMap *m)
{
	return m->key == NULL ? 0 : ((Size) m->mask + 1) * 2 * sizeof(uint32);
}

Size
sift_tidbridge_bytes(const SiftTidBridge *br)
{
	return (Size) br->npages * (sizeof(SiftTidBridgePage) + (br->pblk ? sizeof(uint32) : 0)) + (Size) br->array_len * sizeof(uint16) +
		(Size) br->bm_len * sizeof(br->bm_pool[0]) + br->npending * sizeof(SiftTidBridgePending) + map_bytes(&br->page_map);
}

static bool
over_budget(const SiftTidBridge *br)
{
	return sift_tidbridge_bytes(br) >= br->mem_budget;
}

static void
clear_build(SiftTidBridge *br)
{
	br->hot_sb = br->hot_page = br->hot_blk = NONE;
	br->npending = 0;
}

SiftTidBridge *
sift_tidbridge_create(MemoryContext mcxt, Size mem_budget_bytes, uint32 flags)
{
	SiftTidBridge *br = MemoryContextAllocZero(mcxt, sizeof(SiftTidBridge));

	SIFT_INVARIANT(!(flags & ~PRODUCER_FLAGS), "sift: a bridge takes the flags of its producer only");
	br->mcxt = mcxt;
	br->flags = flags;
	br->mem_budget = mem_budget_bytes;
	br->min_tid = UINT64_MAX;
	clear_build(br);
	return br;
}

void
sift_tidbridge_reset(SiftTidBridge *br)
{
	br->nsbs = br->npages = br->array_len = br->bm_len = 0;
	br->ntids = br->max_tid = br->blk_bloom_union = 0;
	br->min_tid = UINT64_MAX;
	br->flags &= PRODUCER_FLAGS;
	br->finalized = false;
	if (br->scores)
		pfree(br->scores);
	if (br->rowlocs)
		pfree(br->rowlocs);
	br->scores = NULL;
	br->rowlocs = NULL;
	map_clear(&br->sb_map);
	map_clear(&br->page_map);
	clear_build(br);
}

static uint32
open_sb(SiftTidBridge *br, uint32 sb)
{
	uint32		i = br->hot_sb;

	if (i != NONE && br->sbs[i].sb == sb)
		return i;
	i = map_find(&br->sb_map, sb);
	if (i == NONE)
	{
		if (br->nsbs == br->sb_cap)
		{
			br->sb_cap = Max(64, 2 * br->sb_cap);
			SIFT_REGROW_HUGE_IN(br->mcxt, br->sbs, (Size) br->sb_cap * sizeof(SiftTidBridgeSB));
		}
		i = br->nsbs++;
		memset(&br->sbs[i], 0, sizeof(SiftTidBridgeSB));
		br->sbs[i].sb = sb;
		map_put(br->mcxt, &br->sb_map, sb, i);
	}
	br->hot_sb = i;
	return i;
}

static uint32
new_page(SiftTidBridge *br, BlockNumber blk)
{
	uint32		i;
	bool		grow = br->npages == br->page_cap;

	if (grow)
	{
		br->page_cap = Max(64, 2 * br->page_cap);
		SIFT_REGROW_HUGE_IN(br->mcxt, br->pages, (Size) br->page_cap * sizeof(SiftTidBridgePage));
	}
	if (grow || br->pblk == NULL)
		SIFT_REGROW_HUGE_IN(br->mcxt, br->pblk, (Size) br->page_cap * sizeof(uint32));
	if (br->array_len + SIFT_TIDP_ARRAY_MAX > br->array_cap)
	{
		br->array_cap = Max(SIFT_TIDP_ARRAY_MAX * 64, 2 * br->array_cap);
		SIFT_REGROW_HUGE_IN(br->mcxt, br->array_pool, (Size) br->array_cap * sizeof(uint16));
	}
	i = br->npages++;
	br->pages[i] = (SiftTidBridgePage) {.kind = SIFT_TIDP_ARRAY, .page_in_sb = page_of(blk), .payload_off = br->array_len};
	br->array_len += SIFT_TIDP_ARRAY_MAX;
	br->pblk[i] = blk;
	map_put(br->mcxt, &br->page_map, blk, i);
	return i;
}

static uint32
new_bitmap(SiftTidBridge *br)
{
	if (br->bm_len == br->bm_cap)
	{
		br->bm_cap = Max(64, 2 * br->bm_cap);
		SIFT_REGROW_HUGE_IN(br->mcxt, br->bm_pool, (Size) br->bm_cap * sizeof(br->bm_pool[0]));
	}
	memset(br->bm_pool[br->bm_len], 0, sizeof(br->bm_pool[0]));
	return br->bm_len++;
}

static uint32
row_count(const uint64 *row)
{
	uint32		n = 0;

	for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
		n += pg_popcount64(row[w]);
	return n;
}

static uint32
page_count(const SiftTidBridge *br, const SiftTidBridgePage *p)
{
	return p->kind == SIFT_TIDP_ARRAY ? p->n : row_count(br->bm_pool[p->payload_off]);
}

static void
page_lossy(SiftTidBridge *br, SiftTidBridgeSB *s, int page, SiftTidBridgePage *p)
{
	br->ntids -= page_count(br, p);
	p->kind = SIFT_TIDP_LOSSY;
	p->n = 0;
	s->lossy_mask[page / 64] |= page_bit(page);
	br->flags |= SIFT_TBR_HAS_LOSSY;
}

static void
promote(SiftTidBridge *br, SiftTidBridgePage *p)
{
	const uint16 *a = &br->array_pool[p->payload_off];
	uint32		row = new_bitmap(br);

	for (int k = 0; k < p->n; k++)
		br->bm_pool[row][(a[k] - 1) / 64] |= UINT64CONST(1) << ((a[k] - 1) & 63);
	p->kind = SIFT_TIDP_BITMAP;
	p->n = 0;
	p->payload_off = row;
}

/* The offset (1-based) joins the page: an ARRAY page of SIFT_TIDP_ARRAY_MAX offsets becomes a BITMAP page, or lossy when the byte budget is spent */
static void
page_add(SiftTidBridge *br, SiftTidBridgeSB *s, int page, uint32 rec, int off)
{
	SiftTidBridgePage *p = &br->pages[rec];
	uint64	   *w;
	uint64		bit;

	if (p->kind == SIFT_TIDP_ARRAY)
	{
		uint16	   *a = &br->array_pool[p->payload_off];
		int			i = 0;

		while (i < p->n && a[i] < off)
			i++;
		if (i < p->n && a[i] == off)
			return;
		if (p->n < SIFT_TIDP_ARRAY_MAX)
		{
			memmove(&a[i + 1], &a[i], (p->n - i) * sizeof(uint16));
			a[i] = off;
			p->n++;
			br->ntids++;
			return;
		}
		if (over_budget(br))
		{
			page_lossy(br, s, page, p);
			return;
		}
		promote(br, p);
	}
	w = &br->bm_pool[p->payload_off][(off - 1) / 64];
	bit = UINT64CONST(1) << ((off - 1) & 63);
	br->ntids += !(*w & bit);
	*w |= bit;
}

static void
add(SiftTidBridge *br, SiftKey key, uint32 has, float4 score, SiftTidRowLoc loc)
{
	BlockNumber blk = sift_key_block(key);
	int			off = key & 0xFFFF,
				page = page_of(blk);
	uint64		bit = page_bit(page);
	SiftTidBridgeSB *s;
	uint32		rec;

	SIFT_INVARIANT(!br->finalized, "sift: a finalized TID bridge takes no TID");
	SIFT_INVARIANT(off >= 1 && off <= SIFT_TIDB_MAX_OFFSET, "sift: TID offset %d is out of range", off);
	SIFT_INVARIANT(!(has & ~br->flags), "sift: the TID bridge does not keep what was added to it");
	rec = open_sb(br, sift_block_sb(blk));
	s = &br->sbs[rec];
	if (s->lossy_mask[page / 64] & bit)
		return;
	if (!(s->page_mask[page / 64] & bit))
	{
		if (over_budget(br))
		{
			s->page_mask[page / 64] |= bit;
			s->lossy_mask[page / 64] |= bit;
			br->flags |= SIFT_TBR_HAS_LOSSY;
			return;
		}
		rec = new_page(br, blk);
		s->page_mask[page / 64] |= bit;
	}
	else if (br->hot_blk == blk)
		rec = br->hot_page;
	else
		rec = map_find(&br->page_map, blk);
	br->hot_blk = blk;
	br->hot_page = rec;
	page_add(br, s, page, rec, off);
	if (!has || br->pages[rec].kind == SIFT_TIDP_LOSSY)
		return;
	if (over_budget(br))
	{
		page_lossy(br, s, page, &br->pages[rec]);
		return;
	}
	if (br->npending == br->pending_cap)
	{
		br->pending_cap = Max(64, 2 * br->pending_cap);
		SIFT_REGROW_HUGE_IN(br->mcxt, br->pending, br->pending_cap * sizeof(SiftTidBridgePending));
	}
	br->pending[br->npending] = (SiftTidBridgePending) {.key = key, .loc = loc, .score = score, .seq = br->npending, .has = has};
	br->npending++;
}

void
sift_tidbridge_add_tid(SiftTidBridge *br, SiftKey key)
{
	add(br, key, 0, 0, (SiftTidRowLoc) {0});
}

void
sift_tidbridge_add_score(SiftTidBridge *br, SiftKey key, float4 score)
{
	add(br, key, SIFT_TBR_HAS_SCORES, score, (SiftTidRowLoc) {0});
}

void
sift_tidbridge_add_rowloc(SiftTidBridge *br, SiftKey key, SiftTidRowLoc loc)
{
	add(br, key, SIFT_TBR_HAS_ROWLOC, 0, loc);
}

/* The exact pages of the superblock before page */
static uint32
exact_before(const SiftTidBridgeSB *s, int page)
{
	uint32		n = 0;

	for (int w = 0; w < page / 64; w++)
		n += pg_popcount64(s->page_mask[w] & ~s->lossy_mask[w]);
	return n + pg_popcount64(s->page_mask[page / 64] & ~s->lossy_mask[page / 64] & (page_bit(page) - 1));
}

const SiftTidBridgeSB *
sift_tidbridge_probe_sb(const SiftTidBridge *br, uint32 sb)
{
	int64		lo = 0,
				hi = (int64) br->nsbs - 1;

	SIFT_INVARIANT(br->finalized, "sift: a TID bridge is probed after finalize");
	while (lo <= hi)
	{
		int64		mid = lo + (hi - lo) / 2;

		if (br->sbs[mid].sb == sb)
			return &br->sbs[mid];
		if (br->sbs[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* The record of the exact page of block blk: SIFT_TIDPROBE_EXACT, or NO when the bridge has no page there, LOSSY when its page is lossy */
static SiftTidProbe
locate_page(const SiftTidBridge *br, BlockNumber blk, const SiftTidBridgePage **page)
{
	int			pg = page_of(blk);
	const SiftTidBridgeSB *s = sift_tidbridge_probe_sb(br, sift_block_sb(blk));

	if (s == NULL || !(s->page_mask[pg / 64] & page_bit(pg)))
		return SIFT_TIDPROBE_NO;
	if (s->lossy_mask[pg / 64] & page_bit(pg))
		return SIFT_TIDPROBE_LOSSY;
	*page = &br->pages[s->first_page_idx + exact_before(s, pg)];
	return SIFT_TIDPROBE_EXACT;
}

/* Is offset off among those of the exact page p, and at what rank */
static bool
page_has(const SiftTidBridge *br, const SiftTidBridgePage *p, int off, uint32 *rank)
{
	if (off < 1 || off > SIFT_TIDB_MAX_OFFSET)
		return false;
	if (p->kind == SIFT_TIDP_ARRAY)
	{
		const uint16 *a = &br->array_pool[p->payload_off];

		for (int i = 0; i < p->n && a[i] <= off; i++)
			if (a[i] == off)
			{
				*rank = i;
				return true;
			}
		return false;
	}
	else
	{
		const uint64 *row = br->bm_pool[p->payload_off];

		if (!(row[(off - 1) / 64] >> ((off - 1) & 63) & 1))
			return false;
		*rank = 0;
		for (int w = 0; w < (off - 1) / 64; w++)
			*rank += pg_popcount64(row[w]);
		*rank += pg_popcount64(row[(off - 1) / 64] & ((UINT64CONST(1) << ((off - 1) & 63)) - 1));
		return true;
	}
}

/* The place of an exact TID: its page record and the rank of its offset in the page; the answer for a TID that is not in an exact page */
static SiftTidProbe
locate(const SiftTidBridge *br, SiftKey key, const SiftTidBridgePage **page, uint32 *rank)
{
	int			off = key & 0xFFFF;
	SiftTidProbe r = off < 1 || off > SIFT_TIDB_MAX_OFFSET ? SIFT_TIDPROBE_NO : locate_page(br, sift_key_block(key), page);

	if (r != SIFT_TIDPROBE_EXACT)
		return r;
	return page_has(br, *page, off, rank) ? SIFT_TIDPROBE_EXACT : SIFT_TIDPROBE_NO;
}

SiftTidProbe
sift_tidbridge_probe_tid(const SiftTidBridge *br, SiftKey key, float4 *score)
{
	const SiftTidBridgePage *p;
	uint32		rank;
	SiftTidProbe r = locate(br, key, &p, &rank);

	if (r == SIFT_TIDPROBE_EXACT && score != NULL && br->scores != NULL)
		*score = br->scores[p->side_off + rank];
	return r;
}

bool
sift_tidbridge_rowloc(const SiftTidBridge *br, SiftKey key, SiftTidRowLoc *out)
{
	const SiftTidBridgePage *p;
	uint32		rank;

	if (br->rowlocs == NULL || locate(br, key, &p, &rank) != SIFT_TIDPROBE_EXACT)
		return false;
	*out = br->rowlocs[p->side_off + rank];
	return true;
}

uint64
sift_tidbridge_block_mask(const SiftTidBridge *br, const SiftTidBridgeSB *bsb, int b, uint64 (*out)[SIFT_OFFMASK_WORDS], uint64 *out_lossy_pages)
{
	int			per_word = 64 / SIFT_BLK_PAGES,
				shift = (b % per_word) * SIFT_BLK_PAGES;
	uint64		sel = (UINT64CONST(1) << SIFT_BLK_PAGES) - 1,
				active = bsb->page_mask[b / per_word] >> shift & sel,
				lossy = bsb->lossy_mask[b / per_word] >> shift & sel,
				todo = active;
	uint32		idx = bsb->first_page_idx + exact_before(bsb, b * SIFT_BLK_PAGES);

	if (out_lossy_pages != NULL)
		*out_lossy_pages = lossy;
	for (; todo; todo &= todo - 1)
	{
		int			pg = pg_rightmost_one_pos64(todo);

		if (lossy >> pg & 1)
			memset(out[pg], 0xff, sizeof(out[pg]));
		else
		{
			const SiftTidBridgePage *p = &br->pages[idx++];

			if (p->kind == SIFT_TIDP_BITMAP)
				memcpy(out[pg], br->bm_pool[p->payload_off], sizeof(out[pg]));
			else
			{
				memset(out[pg], 0, sizeof(out[pg]));
				for (int i = 0; i < p->n; i++)
					out[pg][(br->array_pool[p->payload_off + i] - 1) / 64] |= UINT64CONST(1) << ((br->array_pool[p->payload_off + i] - 1) & 63);
			}
		}
	}
	return active;
}

static int
cmp_sb(const void *a, const void *b)
{
	uint32		x = ((const SiftTidBridgeSB *) a)->sb,
				y = ((const SiftTidBridgeSB *) b)->sb;

	return x < y ? -1 : x > y;
}

static int
cmp_pending(const void *a, const void *b)
{
	const SiftTidBridgePending *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* The records of the exact pages in block order, with the payload packed in that order */
static void
pack_pages(SiftTidBridge *br)
{
	uint64	   *order = MemoryContextAllocHuge(br->mcxt, Max(br->npages, 1) * sizeof(uint64));
	uint32		nexact = 0,
				narray = 0,
				nbm = 0,
				side = 0;
	SiftTidBridgePage *pages;
	uint16	   *array_pool;
	uint64	  (*bm_pool)[SIFT_OFFMASK_WORDS];

	for (uint32 i = 0; i < br->npages; i++)
	{
		const SiftTidBridgePage *p = &br->pages[i];

		if (p->kind == SIFT_TIDP_LOSSY)
			continue;
		order[nexact++] = (uint64) br->pblk[i] << 32 | i;
		narray += p->kind == SIFT_TIDP_ARRAY ? p->n : 0;
		nbm += p->kind == SIFT_TIDP_BITMAP;
	}
	sift_sort_u64(order, nexact);
	pages = nexact ? MemoryContextAllocHuge(br->mcxt, (Size) nexact * sizeof(SiftTidBridgePage)) : NULL;
	array_pool = narray ? MemoryContextAllocHuge(br->mcxt, (Size) narray * sizeof(uint16)) : NULL;
	bm_pool = nbm ? MemoryContextAllocHuge(br->mcxt, (Size) nbm * sizeof(bm_pool[0])) : NULL;
	narray = nbm = 0;
	for (uint32 j = 0; j < nexact; j++)
	{
		SiftTidBridgePage *p = &pages[j];

		*p = br->pages[(uint32) order[j]];
		if (p->kind == SIFT_TIDP_ARRAY)
		{
			memcpy(&array_pool[narray], &br->array_pool[p->payload_off], p->n * sizeof(uint16));
			p->payload_off = narray;
			narray += p->n;
		}
		else
		{
			memcpy(bm_pool[nbm], br->bm_pool[p->payload_off], sizeof(bm_pool[0]));
			p->payload_off = nbm++;
		}
		p->side_off = side;
		side += p->kind == SIFT_TIDP_ARRAY ? p->n : row_count(bm_pool[p->payload_off]);
	}
	pfree(order);
	if (br->pages)
		pfree(br->pages);
	if (br->array_pool)
		pfree(br->array_pool);
	if (br->bm_pool)
		pfree(br->bm_pool);
	br->pages = pages;
	br->array_pool = array_pool;
	br->bm_pool = bm_pool;
	br->npages = br->page_cap = nexact;
	br->array_len = br->array_cap = narray;
	br->bm_len = br->bm_cap = nbm;
}

/* The superblocks in order, each with the first of its pages, its count and its bloom; the key range of the bridge */
static void
index_sbs(SiftTidBridge *br)
{
	uint32		idx = 0;

	if (br->nsbs > 1)
		qsort(br->sbs, br->nsbs, sizeof(SiftTidBridgeSB), cmp_sb);
	for (uint32 i = 0; i < br->nsbs; i++)
	{
		SiftTidBridgeSB *s = &br->sbs[i];
		uint32		k = 0;

		s->first_page_idx = idx;
		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		{
			s->active_pages += pg_popcount64(s->page_mask[w]);
			for (uint64 m = s->page_mask[w]; m; m &= m - 1)
			{
				int			pg = w * 64 + pg_rightmost_one_pos64(m);
				BlockNumber blk = s->sb << SIFT_SB_SHIFT | pg;
				uint64		lo = (uint64) blk << 16 | 1,
							hi = (uint64) blk << 16 | SIFT_TIDB_MAX_OFFSET;

				s->sb_blk_bloom |= sift_tid_bloom_bit(blk);
				if (!(s->lossy_mask[w] >> (pg & 63) & 1))
				{
					const SiftTidBridgePage *p = &br->pages[idx + k++];

					SIFT_INVARIANT(p->page_in_sb == pg, "sift: the pages of a TID bridge do not follow its superblocks");
					if (p->kind == SIFT_TIDP_ARRAY)
					{
						lo = (uint64) blk << 16 | br->array_pool[p->payload_off];
						hi = (uint64) blk << 16 | br->array_pool[p->payload_off + p->n - 1];
					}
					else
					{
						const uint64 *row = br->bm_pool[p->payload_off];
						int			first = 0,
									last = SIFT_OFFMASK_WORDS - 1;

						while (!row[first])
							first++;
						while (!row[last])
							last--;
						lo = (uint64) blk << 16 | (first * 64 + pg_rightmost_one_pos64(row[first]) + 1);
						hi = (uint64) blk << 16 | (last * 64 + pg_leftmost_one_pos64(row[last]) + 1);
					}
				}
				br->min_tid = Min(br->min_tid, lo);
				br->max_tid = Max(br->max_tid, hi);
			}
		}
		br->blk_bloom_union |= s->sb_blk_bloom;
		idx += k;
	}
	SIFT_INVARIANT(idx == br->npages, "sift: the pages of a TID bridge do not follow its superblocks");
}

/* The scores and row locators that were added, at the place of their TID (the later of two for one TID wins) */
static void
place_side(SiftTidBridge *br)
{
	uint64		side = br->ntids;

	if (br->flags & SIFT_TBR_HAS_SCORES)
		br->scores = side ? memset(MemoryContextAllocHuge(br->mcxt, side * sizeof(float4)), 0, side * sizeof(float4)) : NULL;
	if (br->flags & SIFT_TBR_HAS_ROWLOC)
		br->rowlocs = side ? memset(MemoryContextAllocHuge(br->mcxt, side * sizeof(SiftTidRowLoc)), 0, side * sizeof(SiftTidRowLoc)) : NULL;
	if (br->npending > 1)
		qsort(br->pending, br->npending, sizeof(SiftTidBridgePending), cmp_pending);
	for (uint64 i = 0; i < br->npending; i++)
	{
		const SiftTidBridgePending *e = &br->pending[i];
		const SiftTidBridgePage *p;
		uint32		rank;

		if (locate(br, e->key, &p, &rank) != SIFT_TIDPROBE_EXACT)
			continue;
		if (e->has & SIFT_TBR_HAS_SCORES)
			br->scores[p->side_off + rank] = e->score;
		if (e->has & SIFT_TBR_HAS_ROWLOC)
			br->rowlocs[p->side_off + rank] = e->loc;
	}
}

void
sift_tidbridge_finalize(SiftTidBridge *br)
{
	SIFT_INVARIANT(!br->finalized, "sift: a TID bridge is finalized once");
	pack_pages(br);
	br->finalized = true;
	index_sbs(br);
	if (br->flags & SIDE_FLAGS)
		place_side(br);
	if (br->pending)
		pfree(br->pending);
	br->pending = NULL;
	br->pending_cap = 0;
	if (br->pblk)
		pfree(br->pblk);
	br->pblk = NULL;
	map_free(&br->sb_map);
	map_free(&br->page_map);
	clear_build(br);
}

SiftTidBridge *
sift_tidbridge_from_docids(MemoryContext mcxt, const uint64 *docids, int64 n, uint32 dpp)
{
	SiftTidBridge *br = sift_tidbridge_create(mcxt, SIZE_MAX, 0);

	SIFT_INVARIANT(dpp >= 1 && dpp <= SIFT_TIDB_MAX_OFFSET, "sift: %u documents per page do not fit an offset mask", dpp);
	for (int64 i = 0; i < n; i++)
		sift_tidbridge_add_tid(br, (docids[i] / dpp) << 16 | (docids[i] % dpp + 1));
	sift_tidbridge_finalize(br);
	return br;
}

static bool
page_range_any(const uint64 *mask, uint32 lo, uint32 hi)
{
	for (uint32 p = lo; p <= hi; p += 64 - (p & 63))
	{
		uint64		word = mask[p / 64] >> (p & 63);
		uint32		span = Min(hi - p, 63 - (p & 63)) + 1;

		if (word & (span == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << span) - 1))
			return true;
	}
	return false;
}

bool
sift_tidbridge_granule_may_match(const SiftTidBridge *br, uint64 tid_min, uint64 tid_max, uint64 blk_bloom)
{
	uint32		blk_lo = (uint32) (tid_min >> 16),
				blk_hi = (uint32) (tid_max >> 16),
				sb_lo = blk_lo >> SIFT_SB_SHIFT,
				sb_hi = blk_hi >> SIFT_SB_SHIFT,
				lo = 0,
				hi = br->nsbs;

	if (br->nsbs == 0 || tid_max < br->min_tid || tid_min > br->max_tid || !(blk_bloom & br->blk_bloom_union))
		return false;
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (br->sbs[mid].sb < sb_lo)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (uint32 i = lo; i < br->nsbs && br->sbs[i].sb <= sb_hi; i++)
	{
		const SiftTidBridgeSB *s = &br->sbs[i];

		if ((s->sb_blk_bloom & blk_bloom) &&
			page_range_any(s->page_mask, s->sb == sb_lo ? blk_lo & (SIFT_SB_PAGES - 1) : 0, s->sb == sb_hi ? blk_hi & (SIFT_SB_PAGES - 1) : SIFT_SB_PAGES - 1))
			return true;
	}
	return false;
}

int
sift_tidbridge_probe_vec(const SiftTidBridge *br, const uint64 *tid, const uint64 *live, int n, uint64 *live_out, float4 *score_out)
{
	int			kept = 0;
	BlockNumber blk = InvalidBlockNumber;	/* the rows of a granule come by address: the page of the last row answers for the next */
	const SiftTidBridgePage *page = NULL;
	SiftTidProbe state = SIFT_TIDPROBE_NO;

	memset(live_out, 0, (size_t) ((n + 63) / 64) * sizeof(uint64));
	for (int r = 0; r < n; r++)
	{
		uint32		rank;

		if (live && !(live[r / 64] >> (r % 64) & 1))
			continue;
		if (sift_key_block(tid[r]) != blk)
			state = locate_page(br, blk = sift_key_block(tid[r]), &page);
		SIFT_INVARIANT(state != SIFT_TIDPROBE_LOSSY, "sift: a hybrid bridge of the text scan holds no lossy page");
		if (state == SIFT_TIDPROBE_EXACT && page_has(br, page, tid[r] & 0xFFFF, &rank))
		{
			if (br->scores != NULL)
				score_out[r] = br->scores[page->side_off + rank];
			live_out[r / 64] |= UINT64CONST(1) << (r % 64);
			kept++;
		}
	}
	return kept;
}
