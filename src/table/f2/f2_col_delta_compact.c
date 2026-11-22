/*
 * table/f2/f2_col_delta_compact.c - the fold of a chain of column delta pages into the parts a compaction rewrites, and the records that outlive it.
 *
 * A record folds when its writer committed before the horizon, so that no snapshot can tell the patch from a frozen value; an aborted writer's record and one of a part that is gone are
 * dropped; every other record stays, remapped to the new part when its part is the one rewritten. A row's records are in the order of their writers' serialization, so that once one of
 * them has to stay every later one of the row stays too (a fold of a later patch under an earlier one that stays would put the earlier value over it). No server call.
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "table/f2/f2_col_delta.h"

typedef enum RecClass
{
	REC_DROP,
	REC_FOLD,
	REC_CARRY_REMAP,
	REC_CARRY_KEEP
} RecClass;

typedef struct Rec
{
	const uint8 *rec;
	SiftColDeltaEntryHdr h;
	uint32		seq;
	RecClass	cls;
	FullTransactionId fxid;
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

static RecClass
class_of(bool eligible, SiftHorizonStatus st, bool rewritten)
{
	if (st == SIFT_H_ABORTED)
		return REC_DROP;
	if (!rewritten)
		return REC_CARRY_KEEP;
	return st == SIFT_H_COMMITTED_OLD && eligible ? REC_FOLD : REC_CARRY_REMAP;
}

static Rec *
enumerate(const SiftColDeltaChain *chain, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit, int *n)
{
	Rec		   *v = NULL;
	int			cap = 0;

	*n = 0;
	for (int i = 0; i < chain->npages; i++)
	{
		const char *page = sift_col_delta_chain_page(chain, i);
		const uint8 *p = sift_col_delta_page_first(page),
				   *end = sift_col_delta_page_end(page);

		while (p < end)
		{
			Rec		   *r;
			SiftHorizonStatus st;

			if (*n == cap)
			{
				cap = Max(64, cap * 2);
				v = v != NULL ? repalloc(v, sizeof(Rec) * cap) : palloc(sizeof(Rec) * cap);
			}
			r = &v[*n];
			memcpy(&r->h, p, sizeof(r->h));
			r->rec = p;
			r->seq = (uint32) *n;
			p += r->h.rec_bytes;
			if (!in_sorted(live, nlive, r->h.part_gen))
			{
				r->cls = REC_DROP;
				(*n)++;
				continue;
			}
			st = hz->status(hz->ctx, r->h.xmin, &r->fxid);
			r->cls = class_of(r->seq < fold_limit, st, in_sorted(gens, ngens, r->h.part_gen));
			(*n)++;
		}
	}
	return v;
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

/* Once a record of a row stays, the later ones do; done on a copy sorted by row, written back by seq */
static void
keep_later_records_of_a_blocked_row(Rec *v, int n)
{
	Rec		   *sorted = palloc(sizeof(Rec) * Max(n, 1));
	bool		blocked = false;

	memcpy(sorted, v, sizeof(Rec) * n);
	qsort(sorted, n, sizeof(Rec), by_row);
	for (int i = 0; i < n; i++)
	{
		blocked = i > 0 && same_row(&sorted[i - 1], &sorted[i]) ? blocked : false;
		if (sorted[i].cls == REC_CARRY_REMAP)
			blocked = true;
		else if (sorted[i].cls == REC_FOLD && blocked)
			v[sorted[i].seq].cls = REC_CARRY_REMAP;
	}
	pfree(sorted);
}

static int
fold_cmp(const void *a, const void *b)
{
	const SiftColDeltaFold *x = a,
			   *y = b;

	if (x->part_gen != y->part_gen)
		return x->part_gen < y->part_gen ? -1 : 1;
	if (x->granule_id != y->granule_id)
		return x->granule_id < y->granule_id ? -1 : 1;
	if (x->patch.row_ordinal != y->patch.row_ordinal)
		return x->patch.row_ordinal < y->patch.row_ordinal ? -1 : 1;
	if (x->patch.attnum != y->patch.attnum)
		return x->patch.attnum < y->patch.attnum ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void
fold_records(SiftColDeltaCompactPlan *plan, const Rec *v, int n)
{
	int			cap = 0,
				kept = 0;

	for (int i = 0; i < n; i++)
	{
		const uint8 *p,
				   *end;

		if (v[i].cls != REC_FOLD)
			continue;
		plan->stripped = sift_fxid_max(plan->stripped, v[i].fxid);
		p = v[i].rec + sizeof(SiftColDeltaEntryHdr);
		end = v[i].rec + v[i].h.rec_bytes;
		for (int k = 0; k < v[i].h.ncols; k++)
		{
			SiftColDeltaFold *f;

			if (plan->nfolds == cap)
			{
				cap = Max(32, cap * 2);
				plan->folds = plan->folds != NULL ? repalloc(plan->folds, sizeof(SiftColDeltaFold) * cap) : palloc(sizeof(SiftColDeltaFold) * cap);
			}
			f = &plan->folds[plan->nfolds++];
			p = sift_col_delta_decode_col(p, end, &f->patch);
			SIFT_INVARIANT(p != NULL, "a column delta page was read before it was checked");
			f->patch.row_ordinal = v[i].h.row_ordinal;
			f->part_gen = v[i].h.part_gen;
			f->granule_id = v[i].h.granule_id;
			f->tid = sift_col_delta_hdr_tid(&v[i].h);
			f->seq = v[i].seq;
		}
	}
	if (plan->nfolds == 0)
		return;
	qsort(plan->folds, plan->nfolds, sizeof(SiftColDeltaFold), fold_cmp);
	for (int i = 0; i < plan->nfolds; i++)
	{
		bool		same = kept > 0 && plan->folds[kept - 1].part_gen == plan->folds[i].part_gen && plan->folds[kept - 1].granule_id == plan->folds[i].granule_id &&
			plan->folds[kept - 1].patch.row_ordinal == plan->folds[i].patch.row_ordinal && plan->folds[kept - 1].patch.attnum == plan->folds[i].patch.attnum;

		plan->folds[same ? kept - 1 : kept++] = plan->folds[i];
	}
	plan->nfolds = kept;
}

static void
carry_records(SiftColDeltaCompactPlan *plan, const Rec *v, int n)
{
	for (int i = 0; i < n; i++)
	{
		if (v[i].cls == REC_DROP)
			plan->ndropped++;
		else if (v[i].cls != REC_FOLD)
		{
			plan->carry[plan->ncarry].rec = v[i].rec;
			plan->carry[plan->ncarry++].remap = v[i].cls == REC_CARRY_REMAP;
		}
	}
}

void
sift_col_delta_compact_plan(const SiftColDeltaChain *chain, const uint64 *gens, int ngens, const uint64 *live, int nlive, const SiftColDeltaHorizon *hz, uint32 fold_limit,
							SiftColDeltaCompactPlan *plan)
{
	int			n;
	Rec		   *v = enumerate(chain, gens, ngens, live, nlive, hz, fold_limit, &n);

	memset(plan, 0, sizeof(*plan));
	plan->stripped = InvalidFullTransactionId;
	plan->nrecords = n;
	plan->carry = palloc(sizeof(SiftColDeltaCarry) * Max(n, 1));
	if (n > 0)
		keep_later_records_of_a_blocked_row(v, n);
	fold_records(plan, v, n);
	carry_records(plan, v, n);
	if (v != NULL)
		pfree(v);
}

void
sift_col_delta_compact_plan_free(SiftColDeltaCompactPlan *plan)
{
	if (plan->folds != NULL)
		pfree(plan->folds);
	pfree(plan->carry);
	memset(plan, 0, sizeof(*plan));
}

/* ---- applying the fold ---- */

void
sift_col_delta_fold_slice(const SiftColDeltaCompactPlan *plan, uint64 part_gen, uint32 granule_id, int *lo, int *hi)
{
	int			a = 0,
				b = plan->nfolds;

	while (a < b)
	{
		int			mid = a + (b - a) / 2;
		const SiftColDeltaFold *f = &plan->folds[mid];

		if (f->part_gen < part_gen || (f->part_gen == part_gen && f->granule_id < granule_id))
			a = mid + 1;
		else
			b = mid;
	}
	*lo = *hi = a;
	while (*hi < plan->nfolds && plan->folds[*hi].part_gen == part_gen && plan->folds[*hi].granule_id == granule_id)
		(*hi)++;
}

bool
sift_col_delta_fold_apply(SiftRowBuf *rb, int leaf, const SiftColDeltaFold *f, const char **old_ref, uint32 *old_ref_len)
{
	int			r = f->patch.row_ordinal;
	const SiftColDeltaPatch *p = &f->patch;
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
