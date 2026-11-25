/*
 * table/f2/f2_col_delta_compact.c - the fold of a chain of column delta pages into the parts a compaction rewrites, and the records that outlive it.
 *
 * A record folds when its writer committed before the horizon, so that no snapshot can tell the patch from a frozen value; an aborted writer's record and one of a part that is gone are
 * dropped; every other record stays, remapped to the new part when its part is the one rewritten. A row's records are in the order of their writers' serialization, so that once one of
 * them has to stay every later one of the row stays too (a fold of a later patch under an earlier one that stays would put the earlier value over it). The chain is classified a page at
 * a time, in the order it was written: what is held is the rows that have a record that stays and the records that stay, never the chain. No server call.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "table/f2/f2_col_delta.h"

/* A record that stays */
typedef struct Rec
{
	const uint8 *rec;
	SiftColDeltaEntryHdr h;
	uint32		seq;
	int			pos;			/* in the records that stay: what the superseded ones are dropped by */
	SiftColDeltaClass	cls;
	bool		settled;		/* its writer committed before the horizon */
} Rec;

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

static int
by_row(const void *a, const void *b)
{
	const Rec  *x = a,
			   *y = b;

	if (x->h.part_gen != y->h.part_gen)
		return x->h.part_gen < y->h.part_gen ? -1 : 1;
	if (x->h.granule_id != y->h.granule_id)
		return x->h.granule_id < y->h.granule_id ? -1 : 1;
	if (x->h.row_ordinal != y->h.row_ordinal)
		return x->h.row_ordinal < y->h.row_ordinal ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static bool
same_row(const Rec *a, const Rec *b)
{
	return a->h.part_gen == b->h.part_gen && a->h.granule_id == b->h.granule_id && a->h.row_ordinal == b->h.row_ordinal;
}

#define ATT_BITS (INT16_MAX + 1)

static bool
stays_settled(const Rec *r)
{
	return r->settled && (r->cls == SIFT_CDC_CARRY_KEEP || r->cls == SIFT_CDC_CARRY_REMAP) && r->h.ncols > 0 && (r->h.flags & SIFT_CDENT_FLAG_LEAF_ADDR) == 0;
}

/* Every attribute of the record is set in seen; with set, the record's attributes are set in it */
static bool
all_seen(uint8 *seen, const Rec *r, bool set)
{
	const uint8 *p = r->rec + sizeof(SiftColDeltaEntryHdr),
			   *end = r->rec + r->h.rec_bytes;
	bool		all = true;

	for (int k = 0; k < r->h.ncols; k++)
	{
		SiftColDeltaPatch col;

		p = sift_col_delta_decode_col(p, end, &col);
		SIFT_INVARIANT(p != NULL, "a column delta page was read before it was checked");
		all &= (seen[col.attnum / 8] & (1 << (col.attnum % 8))) != 0;
		if (set)
			seen[col.attnum / 8] |= (uint8) (1 << (col.attnum % 8));
	}
	return all;
}

/*
 * A record whose every attribute a later record of its row writes again is never read: both writers committed before the horizon, so every snapshot sees the later value. The rows are
 * walked from the latest record to the first, and the attributes the later records of the row write are the ones seen. v: the records that stay, sorted: the same by row.
 */
static void
drop_superseded_records(Rec *v, const Rec *sorted, int n)
{
	uint8	   *seen = palloc0(ATT_BITS / 8);

	for (int i = n - 1; i >= 0; i--)
	{
		Rec		   *r = &v[sorted[i].pos];

		if (i < n - 1 && (!same_row(&v[sorted[i + 1].pos], r) || sift_col_delta_hdr_tid(&v[sorted[i + 1].pos].h) != sift_col_delta_hdr_tid(&r->h)))
			memset(seen, 0, ATT_BITS / 8);
		if (!stays_settled(r))
			continue;
		if (all_seen(seen, r, false))
			r->cls = SIFT_CDC_DROP;
		else
			(void) all_seen(seen, r, true);
	}
	pfree(seen);
}

/* ---- the records that stay ---- */

#define KEPT_CHUNK	(64 * 1024)

/* The bytes of the records of a page that is gone before the plan is made: chunks, the first 8 bytes of each pointing to the one before */
static uint8 *
kept_copy(SiftColDeltaCompactPlan *plan, uint32 *left, const uint8 *rec, uint32 bytes)
{
	uint8	   *to;

	if (*left < bytes)
	{
		uint8	   *chunk = palloc(KEPT_CHUNK);

		memcpy(chunk, &plan->kept, sizeof(void *));
		plan->kept = chunk;
		*left = KEPT_CHUNK - MAXALIGN(sizeof(void *));
	}
	to = (uint8 *) plan->kept + KEPT_CHUNK - *left;
	memcpy(to, rec, bytes);
	*left -= bytes;
	return to;
}

/*
 * Classifies every record of the chain as its pages come, oldest first: what is read of a page is dropped when the next is taken, so nothing but the records that stay is held, and
 * with keep false not even those (nfolded, ndropped and nremap say what a plan would do). gens: the parts being rewritten, live: every part the partition has, both ascending. Only the
 * first fold_limit records may fold (the ones a pass saw before it wrote its parts: the others came while it wrote).
 */
static void
stream_records(const SiftColDeltaPageSource *src, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit, bool keep,
			   SiftColDeltaCompactPlan *plan)
{
	SiftColDeltaPass *pass = sift_col_delta_pass_begin(gens, ngens, live, nlive, hz, fold_limit);
	Rec		   *v = NULL;
	int			n = 0,
				cap = 0;
	uint32		seq = 0,
				left = 0;

	memset(plan, 0, sizeof(*plan));
	plan->stripped = InvalidFullTransactionId;
	plan->kept = NULL;
	for (int i = 0; i < src->npages; i++)
	{
		uint32		count;
		const char *page = src->page(src->ctx, i, &count);
		const uint8 *p = sift_col_delta_page_first(page),
				   *end = sift_col_delta_page_end(page);

		for (; p < end && count > 0; count--, seq++)
		{
			Rec			r;
			FullTransactionId fxid = InvalidFullTransactionId;

			memcpy(&r.h, p, sizeof(r.h));
			r.rec = p;
			r.seq = seq;
			p += r.h.rec_bytes;
			r.cls = sift_col_delta_pass_class(pass, r.rec, seq, &r.settled, &fxid);
			if (r.cls == SIFT_CDC_DROP)
				plan->ndropped++;
			else if (r.cls == SIFT_CDC_FOLD)
			{
				plan->nfolded++;
				plan->stripped = sift_fxid_max(plan->stripped, fxid);
			}
			else if (!keep)
				plan->nremap += r.cls == SIFT_CDC_CARRY_REMAP;
			else
			{
				if (n == cap)
				{
					cap = Max(64, cap * 2);
					v = v != NULL ? repalloc(v, sizeof(Rec) * cap) : palloc(sizeof(Rec) * cap);
				}
				r.pos = n;
				if (!src->stable)
					r.rec = kept_copy(plan, &left, r.rec, r.h.rec_bytes);
				v[n++] = r;
			}
		}
	}
	plan->nrecords = (int) seq;
	sift_col_delta_pass_end(pass);
	if (!keep)
		return;
	if (n > 1)
	{
		/* MEM[MEM-TBL-01]: col_delta compact scratch (the records that stay, sorted in place) */
		Rec		   *sorted = memcpy(palloc(sizeof(Rec) * n), v, sizeof(Rec) * n);

		qsort(sorted, n, sizeof(Rec), by_row);
		drop_superseded_records(v, sorted, n);
		pfree(sorted);
	}
	plan->carry = palloc(sizeof(SiftColDeltaCarry) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		if (v[i].cls == SIFT_CDC_DROP)
			plan->ndropped++;
		else
		{
			plan->carry[plan->ncarry].rec = v[i].rec;
			plan->carry[plan->ncarry++].remap = v[i].cls == SIFT_CDC_CARRY_REMAP;
		}
	}
	if (v != NULL)
		pfree(v);
}

/* The records that stay are made plan->carry, a record at a time, those of a page that is not stable copied */
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
	SiftColDeltaPageSource src = {.ctx = &cs,.npages = chain->npages,.stable = true,.page = chain_page};

	sift_col_delta_compact_stream(&src, gens, ngens, live, nlive, hz, fold_limit, plan);
}

void
sift_col_delta_compact_plan_free(SiftColDeltaCompactPlan *plan)
{
	for (void *chunk = plan->kept; chunk != NULL;)
	{
		void	   *before;

		memcpy(&before, chunk, sizeof(void *));
		pfree(chunk);
		chunk = before;
	}
	pfree(plan->carry);
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

static void
open_batch(SiftColDeltaBatch **v, int *n, int *cap)
{
	if (*n == *cap)
	{
		*cap = Max(2, *cap * 2);
		*v = *v != NULL ? repalloc(*v, sizeof(SiftColDeltaBatch) * *cap) : palloc(sizeof(SiftColDeltaBatch) * *cap);
	}
	memset(&(*v)[(*n)++], 0, sizeof(SiftColDeltaBatch));
}

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

/* The new address a record of a sift_col index holds (its first column), 0 when it holds none */
static uint64
record_new_tid(const uint8 *rec, const SiftColDeltaEntryHdr *h)
{
	SiftColDeltaPatch col;

	if ((h->flags & SIFT_CDENT_FLAG_LEAF_ADDR) == 0 || h->ncols == 0 || sift_col_delta_decode_col(rec + sizeof(*h), rec + h->rec_bytes, &col) == NULL || col.attnum != SIFT_CD_ATT_TID_REMAP)
		return 0;
	return (uint64) col.val.i;
}

SiftColDeltaBatch *
sift_col_delta_compact_carry(const SiftColDeltaCompactPlan *plan, FullTransactionId next, const SiftColDeltaRemap *remap, int *nbatches)
{
	SiftColDeltaBatch *v = NULL;
	uint32		slots = 32;
	MovedMap	mm = {0};
	int			n = 0,
				cap = 0;

	while (slots < 2 * (uint32) plan->ncarry)
		slots *= 2;
	mm.mv = palloc(sizeof(Moved) * Max(plan->ncarry, 1));
	/* check-memory: allow a slot for each carried record of one compaction, twice over */
	mm.slot = palloc0(slots * sizeof(int32));
	mm.mask = slots - 1;

	for (int i = 0; i < plan->ncarry; i++)
	{
		uint8		buf[SIFT_COL_DELTA_MAX_REC_BYTES];
		SiftColDeltaEntryHdr h;
		uint64		new_tid;

		memcpy(&h, plan->carry[i].rec, sizeof(h));
		memcpy(buf, plan->carry[i].rec, h.rec_bytes);
		if (plan->carry[i].remap)
		{
			if (!remap->locate(remap->ctx, sift_col_delta_hdr_tid(&h), &h.part_gen, &h.granule_id, &h.row_ordinal) && !moved_to(&mm, sift_col_delta_hdr_tid(&h), &h))
				continue;
			memcpy(buf, &h, sizeof(h));
		}
		new_tid = record_new_tid(buf, &h);
		if (new_tid != 0)
			moved_add(&mm, &(Moved) {new_tid, h.part_gen, h.granule_id, h.row_ordinal});
		if (n == 0 || v[n - 1].nbytes + h.rec_bytes > SIFT_COL_DELTA_PAGE_ROOM)
			open_batch(&v, &n, &cap);
		sift_col_delta_batch_add(&v[n - 1], next, buf);
	}
	pfree(mm.mv);
	pfree(mm.slot);
	*nbatches = n;
	return v;
}
