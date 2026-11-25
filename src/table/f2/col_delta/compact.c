/*
 * table/f2/col_delta/compact.c - the fold of a chain of column delta pages into the parts a compaction rewrites, and the records that outlive it.
 *
 * A record folds when its writer committed before the horizon, so that no snapshot can tell the patch from a frozen value; an aborted writer's record and one of a part that is gone are
 * dropped; every other record stays, remapped to the new part when its part is the one rewritten. A row's records are in the order of their writers' serialization, so that once one of
 * them has to stay every later one of the row stays too (a fold of a later patch under an earlier one that stays would put the earlier value over it). The chain is classified a page at
 * a time, in the order it was written, and what is held of it is a nibble for each record (its fate) and the addresses the records that move name: never the chain, and no record. The
 * records that stay are read again from the pages when they are written. No server call.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "table/f2/col_delta/col_delta.h"

static bool
in_sorted(const uint64 *v, int n, uint64 x)
{
	int			lo = 0,
				hi = n;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (v[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && v[lo] == x;
}

static SiftColDeltaClass
class_of(bool eligible, SiftHorizonStatus st, bool rewritten)
{
	if (st == SIFT_H_ABORTED)
		return SIFT_CDC_DROP;
	if (!rewritten)
		return SIFT_CDC_CARRY_KEEP;
	return st == SIFT_H_COMMITTED_OLD && eligible ? SIFT_CDC_FOLD : SIFT_CDC_CARRY_REMAP;
}

/* ---- the class of a record as the records of a chain go by ---- */

/* A row some record of which stays: the open addressing of the rows, empty until the first one */
typedef struct BlockedRow
{
	uint64		part_gen;
	uint32		granule_id;
	uint16		row_ordinal;
	bool		used;
} BlockedRow;

struct SiftColDeltaPass
{
	const uint64 *gens;
	int			ngens;
	const uint64 *live;
	int			nlive;
	const SiftColDeltaHorizon *hz;
	uint32		fold_limit;
	BlockedRow *slot;
	uint32		mask;
	uint32		nblocked;
};

SiftColDeltaPass *
sift_col_delta_pass_begin(const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit)
{
	SiftColDeltaPass *p = palloc0(sizeof(SiftColDeltaPass));

	p->gens = gens;
	p->ngens = ngens;
	p->live = live;
	p->nlive = nlive;
	p->hz = hz;
	p->fold_limit = fold_limit;
	return p;
}

void
sift_col_delta_pass_end(SiftColDeltaPass *p)
{
	if (p->slot != NULL)
		pfree(p->slot);
	pfree(p);
}

static uint32
blocked_home(const SiftColDeltaPass *p, const SiftColDeltaEntryHdr *h)
{
	uint64		k = h->part_gen * UINT64CONST(0x9E3779B97F4A7C15) ^ (((uint64) h->granule_id << 16) | h->row_ordinal) * UINT64CONST(0xC2B2AE3D27D4EB4F);

	return (uint32) (k ^ (k >> 32)) & p->mask;
}

static bool
blocked_same(const BlockedRow *s, const SiftColDeltaEntryHdr *h)
{
	return s->part_gen == h->part_gen && s->granule_id == h->granule_id && s->row_ordinal == h->row_ordinal;
}

static bool
blocked_has(const SiftColDeltaPass *p, const SiftColDeltaEntryHdr *h)
{
	if (p->nblocked == 0)
		return false;
	for (uint32 i = blocked_home(p, h); p->slot[i].used; i = (i + 1) & p->mask)
		if (blocked_same(&p->slot[i], h))
			return true;
	return false;
}

static void
blocked_add(SiftColDeltaPass *p, const SiftColDeltaEntryHdr *h)
{
	if (blocked_has(p, h))
		return;
	if ((uint64) (p->nblocked + 1) * 2 > (uint64) p->mask + 1)
	{
		BlockedRow *old = p->slot;
		uint32		n = old == NULL ? 0 : p->mask + 1;

		p->mask = Max(n * 2, 64) - 1;
		/* check-memory: allow a slot for each row that has a record that stays, twice over: what no later record of the row is folded under */
		p->slot = palloc0(((Size) p->mask + 1) * sizeof(BlockedRow));
		for (uint32 i = 0; i < n; i++)
			if (old[i].used)
			{
				SiftColDeltaEntryHdr o = {.part_gen = old[i].part_gen,.granule_id = old[i].granule_id,.row_ordinal = old[i].row_ordinal};
				uint32		at = blocked_home(p, &o);

				while (p->slot[at].used)
					at = (at + 1) & p->mask;
				p->slot[at] = old[i];
			}
		if (old != NULL)
			pfree(old);
	}
	for (uint32 at = blocked_home(p, h);; at = (at + 1) & p->mask)
		if (!p->slot[at].used)
		{
			p->slot[at] = (BlockedRow) {h->part_gen, h->granule_id, h->row_ordinal, true};
			p->nblocked++;
			return;
		}
}

/*
 * What happens to the record: it goes with an aborted writer or a gone part, folds into its part, or stays (remapped when its part is rewritten). A row's records are in the order of their
 * writers' serialization, so once one of them stays every later one of the row stays too: a fold of a later patch under an earlier one that stays would put the earlier value over it.
 */
SiftColDeltaClass
sift_col_delta_pass_class(SiftColDeltaPass *p, const uint8 *rec, uint32 seq, bool *settled, FullTransactionId *fxid)
{
	SiftColDeltaEntryHdr h;
	SiftHorizonStatus st;
	SiftColDeltaClass	cls;

	memcpy(&h, rec, sizeof(h));
	*settled = false;
	if (!in_sorted(p->live, p->nlive, h.part_gen))
		return SIFT_CDC_DROP;
	st = p->hz->status(p->hz->ctx, h.xmin, fxid);
	*settled = st == SIFT_H_COMMITTED_OLD;
	cls = class_of(seq < p->fold_limit, st, in_sorted(p->gens, p->ngens, h.part_gen));
	if (cls == SIFT_CDC_CARRY_REMAP)
		blocked_add(p, &h);
	else if (cls == SIFT_CDC_FOLD && blocked_has(p, &h))
		cls = SIFT_CDC_CARRY_REMAP;
	return cls;
}

/* ---- the records of the pages of a source ---- */

typedef struct PageRecs
{
	const uint8 *p;
	const uint8 *end;
	uint32		left;
} PageRecs;

/* The first nrecords records of page i: the page is there until the next is taken */
static PageRecs
page_recs(const SiftColDeltaPageSource *src, int i)
{
	uint32		count;
	const char *page = src->page(src->ctx, i, &count);

	return (PageRecs) {sift_col_delta_page_first(page), sift_col_delta_page_end(page), count};
}

static bool
recs_more(const PageRecs *r)
{
	return r->p < r->end && r->left > 0;
}

static SiftColDeltaEntryHdr
recs_take(PageRecs *r, const uint8 **rec)
{
	SiftColDeltaEntryHdr h;

	memcpy(&h, r->p, sizeof(h));
	*rec = r->p;
	r->p += h.rec_bytes;
	r->left--;
	return h;
}

/* The new address a record of a sift_col index holds (its first column), 0 when it holds none */
static uint64
record_new_tid(const uint8 *rec, const SiftColDeltaEntryHdr *h)
{
	SiftColDeltaPatch col;

	if ((h->flags & SIFT_CDENT_FLAG_LEAF_ADDR) == 0 || h->ncols == 0 || sift_col_delta_decode_col(rec + sizeof(*h), rec + h->rec_bytes, &col) == NULL || col.attnum != SIFT_CD_ATT_TID_REMAP)
		return 0;
	return (uint64) col.val.i;
}

/* ---- the fate of a record: what the plan holds of it ---- */

enum
{
	FATE_GONE,
	FATE_STAYS,					/* in its page, as it is */
	FATE_MOVES,					/* of a part being rewritten: its address changes */
	FATE_MASK = 3,
	FATE_SETTLED = 4			/* its writer committed before the horizon */
};

static uint8
fate_of(const SiftColDeltaCompactPlan *plan, uint32 seq)
{
	return seq / 2 < plan->nfate ? (plan->fate[seq / 2] >> (seq % 2 * 4)) & 0xF : FATE_GONE;
}

static void
fate_set(SiftColDeltaCompactPlan *plan, uint32 seq, uint8 fate)
{
	uint32		at = seq / 2;

	if (at >= plan->nfate)
	{
		uint32		cap = Max(1024, plan->nfate);

		while (cap <= at)
			cap *= 2;
		/* check-memory: allow a nibble for each record of the chain, 1/80 of the bytes of its records */
		plan->fate = plan->fate != NULL ? repalloc(plan->fate, cap) : palloc(cap);
		memset(plan->fate + plan->nfate, 0, cap - plan->nfate);
		plan->nfate = cap;
	}
	plan->fate[at] = (plan->fate[at] & ~(0xF << (seq % 2 * 4))) | (uint8) (fate << (seq % 2 * 4));
}

static void
want_add(SiftColDeltaCompactPlan *plan, int *cap, uint64 tid)
{
	if (plan->nwant == *cap)
	{
		*cap = Max(64, *cap * 2);
		/* check-memory: allow an address for each record that moves, which a writer that has not committed before the horizon made */
		plan->want = plan->want != NULL ? repalloc(plan->want, sizeof(uint64) * *cap) : palloc(sizeof(uint64) * *cap);
	}
	plan->want[plan->nwant++] = tid;
}

static int
tid_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* v ascending without duplicates; the count of what is left */
static int
sort_unique(uint64 *v, int n)
{
	int			k = 0;

	if (n < 2)
		return n;
	qsort(v, n, sizeof(uint64), tid_cmp);
	for (int i = 0; i < n; i++)
		if (k == 0 || v[k - 1] != v[i])
			v[k++] = v[i];
	return k;
}

/* ---- the records a later record of the row writes over ---- */

/*
 * A record every attribute of which a later record of its row writes again is never read: both writers committed before the horizon, so every snapshot sees the later value. The records
 * are walked from the latest to the first, and the attributes the later records of a row write are the ones seen. The rows seen are the slots of a table of a fixed size, a row to a slot:
 * one that another row took the slot of is forgotten, and its earlier records stay (a record that stays is always right, one that goes needs the later record to have been seen).
 */
#define SEEN_SLOTS	2048
#define SEEN_ATTS	256

typedef struct Seen
{
	uint64		part_gen;
	uint32		granule_id;
	uint16		row_ordinal;
	bool		used;
	uint64		tid;
	uint64		atts[SEEN_ATTS / 64];
} Seen;

static Seen *
seen_slot(Seen *seen, const SiftColDeltaEntryHdr *h)
{
	uint64		k = h->part_gen * UINT64CONST(0x9E3779B97F4A7C15) ^ (((uint64) h->granule_id << 16) | h->row_ordinal) * UINT64CONST(0xC2B2AE3D27D4EB4F);

	return &seen[(uint32) (k ^ (k >> 32)) & (SEEN_SLOTS - 1)];
}

/* The attributes of the record as bits; false when it writes one that has no bit */
static bool
record_atts(const uint8 *rec, const SiftColDeltaEntryHdr *h, uint64 *atts)
{
	const uint8 *p = rec + sizeof(*h),
			   *end = rec + h->rec_bytes;

	memset(atts, 0, SEEN_ATTS / 8);
	for (int k = 0; k < h->ncols; k++)
	{
		SiftColDeltaPatch col;

		p = sift_col_delta_decode_col(p, end, &col);
		SIFT_INVARIANT(p != NULL, "a column delta page was read before it was checked");
		if (col.attnum < 0 || col.attnum >= SEEN_ATTS)
			return false;
		atts[col.attnum / 64] |= UINT64CONST(1) << (col.attnum % 64);
	}
	return true;
}

static bool
atts_within(const uint64 *atts, const uint64 *seen)
{
	for (int w = 0; w < SEEN_ATTS / 64; w++)
		if ((atts[w] & ~seen[w]) != 0)
			return false;
	return true;
}

/* A record that stays and whose every attribute a later one of its row writes again goes; one that moves and stays is the address a row it names is looked for by */
static void
walk_record(Seen *seen, SiftColDeltaCompactPlan *plan, int *want_cap, const uint8 *rec, uint32 seq)
{
	uint8		fate = fate_of(plan, seq);
	SiftColDeltaEntryHdr h;
	uint64		atts[SEEN_ATTS / 64];
	Seen	   *s;

	if ((fate & FATE_MASK) == FATE_GONE)
		return;
	memcpy(&h, rec, sizeof(h));
	s = seen_slot(seen, &h);
	if (!s->used || s->part_gen != h.part_gen || s->granule_id != h.granule_id || s->row_ordinal != h.row_ordinal || s->tid != sift_col_delta_hdr_tid(&h))
		*s = (Seen) {.part_gen = h.part_gen,.granule_id = h.granule_id,.row_ordinal = h.row_ordinal,.used = true,.tid = sift_col_delta_hdr_tid(&h)};
	if ((fate & FATE_SETTLED) != 0 && h.ncols > 0 && (h.flags & SIFT_CDENT_FLAG_LEAF_ADDR) == 0 && record_atts(rec, &h, atts))
	{
		if (atts_within(atts, s->atts))
		{
			fate_set(plan, seq, FATE_GONE);
			plan->ndropped++;
			plan->ncarry--;
			return;
		}
		for (int w = 0; w < SEEN_ATTS / 64; w++)
			s->atts[w] |= atts[w];
	}
	if ((fate & FATE_MASK) == FATE_MOVES)
		want_add(plan, want_cap, sift_col_delta_hdr_tid(&h));
}

/* The records of the chain from the latest page to the first, and the addresses that the ones that move name, ascending */
static void
walk_back(const SiftColDeltaPageSource *src, SiftColDeltaCompactPlan *plan)
{
	/* check-memory: allow a table of a fixed number of rows */
	Seen	   *seen = palloc0(sizeof(Seen) * SEEN_SLOTS);
	uint32		end = plan->nrecords;
	int			want_cap = 0;

	for (int i = src->npages - 1; i >= 0; i--)
	{
		PageRecs	r = page_recs(src, i);
		const uint8 *at[BLCKSZ / sizeof(SiftColDeltaEntryHdr)];
		int			n = 0;

		while (recs_more(&r))
			(void) recs_take(&r, &at[n++]);
		end -= n;
		for (int k = n - 1; k >= 0; k--)
			walk_record(seen, plan, &want_cap, at[k], end + k);
	}
	pfree(seen);
	plan->nwant = sort_unique(plan->want, plan->nwant);
}

/*
 * Classifies every record of the chain as its pages come, oldest first: what is read of a page is dropped when the next is taken, so what is held is the fate of each record, and with keep
 * false not even that (nfolded, ndropped and nremap say what a plan would do). gens: the parts being rewritten, live: every part the partition has, both ascending. Only the first
 * fold_limit records may fold (the ones a pass saw before it wrote its parts: the others came while it wrote).
 */
static void
stream_records(const SiftColDeltaPageSource *src, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit, bool keep,
			   SiftColDeltaCompactPlan *plan)
{
	SiftColDeltaPass *pass = sift_col_delta_pass_begin(gens, ngens, live, nlive, hz, fold_limit);
	uint32		seq = 0;

	memset(plan, 0, sizeof(*plan));
	plan->stripped = InvalidFullTransactionId;
	for (int i = 0; i < src->npages; i++)
	{
		PageRecs	r = page_recs(src, i);

		while (recs_more(&r))
		{
			const uint8 *rec;
			SiftColDeltaEntryHdr h = recs_take(&r, &rec);
			FullTransactionId fxid = InvalidFullTransactionId;
			bool		settled;
			SiftColDeltaClass cls = sift_col_delta_pass_class(pass, rec, seq, &settled, &fxid);

			if (cls == SIFT_CDC_DROP)
				plan->ndropped++;
			else if (cls == SIFT_CDC_FOLD)
			{
				plan->nfolded++;
				plan->stripped = sift_fxid_max(plan->stripped, fxid);
			}
			else if (!keep)
				plan->nremap += cls == SIFT_CDC_CARRY_REMAP;
			else
			{
				fate_set(plan, seq, (cls == SIFT_CDC_CARRY_REMAP ? FATE_MOVES : FATE_STAYS) | (settled ? FATE_SETTLED : 0));
				plan->ncarry++;
				plan->nmoved += record_new_tid(rec, &h) != 0;
			}
			seq++;
		}
	}
	plan->nrecords = (int) seq;
	sift_col_delta_pass_end(pass);
	if (keep)
		walk_back(src, plan);
}

/* The records that stay are what the plan says of each: written by sift_col_delta_compact_carry */
void
sift_col_delta_compact_stream(const SiftColDeltaPageSource *src, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit,
							  SiftColDeltaCompactPlan *plan)
{
	stream_records(src, gens, ngens, live, nlive, hz, fold_limit, true, plan);
}

/* Whether the chain would change at all: a record folds, goes or moves to another row. Nothing of the chain is kept, so what it takes does not grow with it */
bool
sift_col_delta_compact_changes(const SiftColDeltaPageSource *src, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit)
{
	SiftColDeltaCompactPlan plan;

	stream_records(src, gens, ngens, live, nlive, hz, fold_limit, false, &plan);
	return plan.nfolded > 0 || plan.ndropped > 0 || plan.nremap > 0;
}

typedef struct ChainSource
{
	const SiftColDeltaChain *chain;
} ChainSource;

static const char *
chain_page(void *ctx, int i, uint32 *count)
{
	*count = UINT32_MAX;
	return sift_col_delta_chain_page(((const ChainSource *) ctx)->chain, i);
}

void
sift_col_delta_compact_plan(const SiftColDeltaChain *chain, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit,
							SiftColDeltaCompactPlan *plan)
{
	ChainSource cs = {chain};
	SiftColDeltaPageSource src = {.ctx = &cs,.npages = chain->npages,.page = chain_page};

	sift_col_delta_compact_stream(&src, gens, ngens, live, nlive, hz, fold_limit, plan);
}

void
sift_col_delta_compact_plan_free(SiftColDeltaCompactPlan *plan)
{
	if (plan->fate != NULL)
		pfree(plan->fate);
	if (plan->want != NULL)
		pfree(plan->want);
	memset(plan, 0, sizeof(*plan));
}

/* ---- applying the fold ---- */

bool
sift_col_delta_fold_apply(SiftRowBuf *rb, int leaf, const SiftColDeltaPatch *p, const char **old_ref, uint32 *old_ref_len)
{
	int			r = p->row_ordinal;
	bool		replaced_wide = false;

	SIFT_INVARIANT(SIFT_CDCOL_KIND(p->flags) == rb->kinds[leaf], "a column delta of attribute %d is of another kind than its column", p->attnum);
	if (rb->tag[leaf][r] == SIFT_COLTAG_WIDE_BLOB && rb->xpay != NULL && rb->xpay[leaf] != NULL && rb->xpay[leaf][r] != NULL)
	{
		*old_ref = rb->xpay[leaf][r];
		*old_ref_len = rb->xplen[leaf][r];
		rb->xpay[leaf][r] = NULL;
		rb->xplen[leaf][r] = 0;
		replaced_wide = true;
	}
	rb->tag[leaf][r] = (p->flags & SIFT_CDCOL_HAS_TAG) != 0 ? p->tag : 0;
	rb->isnull[leaf][r] = (p->flags & SIFT_CDCOL_IS_NULL) != 0;
	if (rb->kinds[leaf] == SIFT_CELL_INT)
		rb->i[leaf][r] = rb->isnull[leaf][r] ? 0 : p->val.i;
	else if (rb->kinds[leaf] == SIFT_CELL_F8)
		rb->f[leaf][r] = rb->isnull[leaf][r] ? 0 : p->val.f;
	else
	{
		char	   *copy = NULL;

		if (!rb->isnull[leaf][r])
		{
			copy = MemoryContextAlloc(rb->cx, (Size) p->vlen + 1);
			memcpy(copy, p->val.s, p->vlen);
			copy[p->vlen] = '\0';
		}
		rb->s[leaf][r] = copy;
		rb->slen[leaf][r] = rb->isnull[leaf][r] ? 0 : p->vlen;
	}
	return replaced_wide;
}

/* ---- what stays ---- */

/* Where the row a carried address change moved to is, for the records after it, which name the row by the address it had by then */
typedef struct Moved
{
	uint64		new_tid;
	uint64		part_gen;
	uint32		granule_id;
	uint16		row;
} Moved;

/* The moves by the address they moved to, the latest of an address winning: open addressing over mv, the index plus one in each slot */
typedef struct MovedMap
{
	Moved	   *mv;
	int32	   *slot;
	uint32		mask;
	int			nmv;
} MovedMap;

static uint32
moved_slot(const MovedMap *m, uint64 tid)
{
	return (uint32) ((tid * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & m->mask;
}

static void
moved_add(MovedMap *m, const Moved *mv)
{
	uint32		p = moved_slot(m, mv->new_tid);

	m->mv[m->nmv] = *mv;
	while (m->slot[p] != 0 && m->mv[m->slot[p] - 1].new_tid != mv->new_tid)
		p = (p + 1) & m->mask;
	m->slot[p] = ++m->nmv;
}

static bool
moved_to(const MovedMap *m, uint64 tid, SiftColDeltaEntryHdr *h)
{
	for (uint32 p = moved_slot(m, tid); m->slot[p] != 0; p = (p + 1) & m->mask)
		if (m->mv[m->slot[p] - 1].new_tid == tid)
		{
			const Moved *mv = &m->mv[m->slot[p] - 1];

			h->part_gen = mv->part_gen;
			h->granule_id = mv->granule_id;
			h->row_ordinal = mv->row;
			return true;
		}
	return false;
}

static MovedMap
moved_map_new(int nmoved)
{
	uint32		slots = 32;
	MovedMap	mm = {0};

	while (slots < 2 * (uint32) nmoved)
		slots *= 2;
	mm.mv = palloc(sizeof(Moved) * Max(nmoved, 1));
	/* check-memory: allow a slot for each carried address change of one compaction, twice over */
	mm.slot = palloc0(slots * sizeof(int32));
	mm.mask = slots - 1;
	return mm;
}

/* The record as it is carried: a record that moves goes to the row it is now (false: the row is gone) */
static bool
carried(const SiftColDeltaCompactPlan *plan, uint32 seq, MovedMap *mm, const SiftColDeltaRemap *remap, const uint8 *rec, uint8 *buf, SiftColDeltaEntryHdr *h)
{
	if ((fate_of(plan, seq) & FATE_MASK) == FATE_GONE)
		return false;
	memcpy(h, rec, sizeof(*h));
	memcpy(buf, rec, h->rec_bytes);
	if ((fate_of(plan, seq) & FATE_MASK) == FATE_MOVES)
	{
		if (!remap->locate(remap->ctx, sift_col_delta_hdr_tid(h), &h->part_gen, &h->granule_id, &h->row_ordinal) && !moved_to(mm, sift_col_delta_hdr_tid(h), h))
			return false;
		memcpy(buf, h, sizeof(*h));
	}
	return true;
}

void
sift_col_delta_compact_carry(const SiftColDeltaCompactPlan *plan, const SiftColDeltaPageSource *src, FullTransactionId next, const SiftColDeltaRemap *remap, SiftColDeltaEmit emit,
							 void *emit_ctx)
{
	MovedMap	mm = moved_map_new(plan->nmoved);
	SiftColDeltaBatch page = {0};
	uint32		seq = 0;

	for (int i = 0; i < src->npages; i++)
	{
		PageRecs	r = page_recs(src, i);

		while (recs_more(&r))
		{
			uint8		buf[SIFT_COL_DELTA_MAX_REC_BYTES];
			SiftColDeltaEntryHdr h;
			const uint8 *rec;
			uint64		new_tid;

			(void) recs_take(&r, &rec);
			if (!carried(plan, seq++, &mm, remap, rec, buf, &h))
				continue;
			new_tid = record_new_tid(buf, &h);
			if (new_tid != 0)
				moved_add(&mm, &(Moved) {new_tid, h.part_gen, h.granule_id, h.row_ordinal});
			if (page.nentries > 0 && page.nbytes + h.rec_bytes > SIFT_COL_DELTA_PAGE_ROOM)
			{
				emit(emit_ctx, &page);
				memset(&page, 0, sizeof(page));
			}
			sift_col_delta_batch_add(&page, next, buf);
		}
	}
	if (page.nentries > 0)
		emit(emit_ctx, &page);
	pfree(mm.mv);
	pfree(mm.slot);
}
