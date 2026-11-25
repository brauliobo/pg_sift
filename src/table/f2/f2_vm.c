/*
 * table/f2/f2_vm.c - the visibility map of a sift_table (table/f2/f2_vm.h): the clear before a delete or an update, and the pass of VACUUM that sets the bits.
 *
 * The pass works on spans of vblocks: a part says it holds rows in [tid_min, tid_max] (its envelope), so a part that is not clean makes the whole envelope unfit and a part that is makes it a
 * candidate. A vblock gets its bits when it is below the floor of the leases, a clean part covers it, no unfit span does and it is full: an address that no row has is one a transaction took
 * and gave up (an abort drops its staged rows, the index entries it made stay) and only a vblock without such a hole is sure to have no index entry for a row that is not there. The same words say
 * it for 32 vblocks at a time (two bits each, the layout of the page of the map), so a page of the map is built by OR-ing spans into arrays of words and combining them.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/col_meta.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_tier_host.h"
#include "col/f2/l0_slab.h"
#include "col/f2/pend_del.h"
#include "col/f2/rowbuf.h"
#include "core/compat.h"
#include "core/f2/f2_vm_xlog.h"
#include "core/memtrack.h"
#include "core/shm_manifest.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_tbl.h"
#include "table/f2/f2_vm.h"

#define EVEN_BITS	UINT64CONST(0x5555555555555555)

/* ---- the clear ---- */

static uint64
meta_ro_flags(Relation rel)
{
	SiftShmIndexMeta peek;

	return sift_shm_manifest_peek(&rel->rd_locator, &peek) ? peek.ro_compat_flags : sift_col_meta_read(rel).common.ro_compat_flags;
}

void
sift_tbl_vm_clear(Relation rel, BlockNumber vblock)
{
	Buffer		vmbuf = InvalidBuffer;

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	if (!sift_vm_rmgr_active())
	{
		if (meta_ro_flags(rel) & SIFT_ROCOMPAT_TABLE_VM)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("sift_table \"%s\" has a visibility map that this server cannot log", RelationGetRelationName(rel)),
							errhint("Add pg_sift to shared_preload_libraries to delete or update rows of this table.")));
		return;
	}
	if (visibilitymap_get_status(rel, vblock, &vmbuf) != 0)
		(void) sift_vm_apply_clear(rel, vmbuf, vblock);
	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);
}

/* ---- the spans ---- */

typedef struct Span
{
	uint64		lo;
	uint64		hi;
	TransactionId xid;			/* the newest transaction whose rows the span makes visible to everyone */
	uint32		part_id;
	const SiftPartDesc *part;	/* of a clean span: the part, to count its rows */
} Span;

typedef struct Spans
{
	Span	   *v;
	int			n;
	int			cap;
} Spans;

typedef struct VmPlan
{
	Spans		clean;			/* the envelopes of the parts that are fit */
	Spans		unfit;			/* what a part or a slab or a delete that is not settled may touch */
	Spans		unfrozen;		/* the vblocks that are visible but not frozen */
	uint64		floor;
	uint64		limit;			/* the vblocks reserved so far */
	FullTransactionId next;		/* the next transaction of the server when the pass began: what the transaction ids of the plan are told apart from */
	Size		bytes;			/* what the spans and the descriptors they point to take */
	Size		budget;			/* what they may take: past it the plan is not whole, and no bit is set on it */
	bool		incomplete;
} VmPlan;

static void
span_add(VmPlan *plan, Spans *s, uint64 lo, uint64 hi, TransactionId xid, uint32 part_id, const SiftPartDesc *part)
{
	if (s->n == s->cap)
	{
		const int	cap = s->cap ? s->cap * 2 : 64;

		plan->bytes += sizeof(Span) * (Size) (cap - s->cap);
		if (plan->bytes > plan->budget)
		{
			plan->incomplete = true;
			return;
		}
		s->cap = cap;
		s->v = s->v == NULL ? palloc(sizeof(Span) * (Size) cap) : repalloc(s->v, sizeof(Span) * (Size) cap);
	}
	s->v[s->n++] = (Span) {lo, hi, xid, part_id, part};
}

/* The 32 bits of a transaction id name one transaction only within 2^31 of the next one; an older transaction is older than every snapshot, and its low bits would read as a future one */
#define XID_SPAN	(UINT64CONST(1) << 31)

TransactionId
sift_vm_horizon_xid(FullTransactionId fx, FullTransactionId next)
{
	uint64		age = U64FromFullTransactionId(next) > U64FromFullTransactionId(fx) ? U64FromFullTransactionId(next) - U64FromFullTransactionId(fx) : 0;

	return FullTransactionIdIsValid(fx) && age < XID_SPAN ? XidFromFullTransactionId(fx) : InvalidTransactionId;
}

static void
span_of_tids(VmPlan *plan, const uint64 *tids, int n, size_t stride)
{
	uint64		lo = UINT64_MAX,
				hi = 0;

	for (int i = 0; i < n; i++)
	{
		uint64		t = *(const uint64 *) ((const char *) tids + i * stride);

		lo = Min(lo, t >> 16);
		hi = Max(hi, t >> 16);
	}
	if (n > 0)
		span_add(plan, &plan->unfit, lo, hi, InvalidTransactionId, 0, NULL);
}

/* The vblocks of the rows a slab or the pending deletes of the partition name: they are not in a part yet, or still stand for a row of one. What they were read into goes when the span is made */
static void
unfit_unsettled(Relation rel, SiftF2Store *store, const SiftPartitionEntry *e, VmPlan *plan)
{
	MemoryContext keep = CurrentMemoryContext;
	MemoryContext cx = sift_mem_allocset_create(keep, "sift vm unsettled");
	BlockNumber *pages;
	int			npages;

	if (e->l0_slab_blk != InvalidBlockNumber)
	{
		SiftRowBuf *rb;

		MemoryContextSwitchTo(cx);
		rb = sift_rowbuf_new(&store->spec);
		sift_l0_slab_read(rel, e->part_id, e->l0_slab_blk, rb, &store->spec, SnapshotAny, &pages, &npages);
		MemoryContextSwitchTo(keep);
		span_of_tids(plan, rb->tid, rb->n, sizeof(uint64));
		sift_rowbuf_free(rb);
		sift_mem_release_os(MemoryContextMemAllocated(cx, true));
		MemoryContextReset(cx);
	}
	if (e->pend_del_blk != InvalidBlockNumber)
	{
		int			n;
		SiftPendDelRec *recs;

		MemoryContextSwitchTo(cx);
		recs = sift_pend_del_read(rel, e->part_id, e->pend_del_blk, &n, &pages, &npages);
		MemoryContextSwitchTo(keep);
		span_of_tids(plan, &recs->tid, n, sizeof(SiftPendDelRec));
	}
	MemoryContextDelete(cx);
}

static bool
part_clean(const SiftPartDesc *d, bool deltas_settled, FullTransactionId oldest)
{
	bool		inserts_sealed = (d->flags & SIFT_PART_FLAG_UNFROZEN) == 0 || ((d->flags & SIFT_PART_FLAG_ALL_VISIBLE) != 0 && FullTransactionIdPrecedes(d->max_xid, oldest));
	bool		deletes_cleaned = d->dv_blk == InvalidBlockNumber || ((d->flags & SIFT_PART_FLAG_DV_SETTLED) != 0 && FullTransactionIdPrecedes(d->dv_max_xid, oldest));

	return inserts_sealed && deletes_cleaned && deltas_settled && (d->ext_flags & SIFT_PART_EXT_EXPIRED) == 0 && d->tid_max != 0;
}

static bool
col_delta_newest(Relation rel, const SiftPartitionEntry *e, FullTransactionId *newest)
{
	SiftColDeltaChain chain;

	*newest = InvalidFullTransactionId;
	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return true;
	if (!sift_col_delta_load_chain(rel, e, 0, &chain))
		return false;
	*newest = sift_col_delta_chain_max_xid(&chain);
	sift_col_delta_chain_free(&chain);
	return true;
}

static TransactionId
newest_xid(TransactionId a, TransactionId b)
{
	return TransactionIdFollows(b, a) ? b : a;
}

static void
plan_partition(Relation rel, SiftF2Store *store, const SiftPartitionEntry *e, FullTransactionId oldest, FullTransactionId freeze, VmPlan *plan)
{
	int			np;
	SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);
	FullTransactionId newest;
	bool		loaded = col_delta_newest(rel, e, &newest);
	bool		deltas_settled = loaded && ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0 || (FullTransactionIdIsValid(newest) && FullTransactionIdPrecedes(newest, oldest)));
	TransactionId delta_xid = sift_vm_horizon_xid(newest, plan->next);

	plan->bytes += sizeof(SiftPartDesc) * (Size) np;
	unfit_unsettled(rel, store, e, plan);
	for (int i = 0; i < np; i++)
	{
		const SiftPartDesc *d = &parts[i];
		uint64		lo = d->tid_max != 0 ? d->tid_min >> 16 : 0,
					hi = d->tid_max != 0 ? d->tid_max >> 16 : UINT64_MAX;
		TransactionId xid = sift_vm_horizon_xid(d->max_xid, plan->next),
					dv_xid = sift_vm_horizon_xid(d->dv_max_xid, plan->next);
		bool		frozen = (d->flags & SIFT_PART_FLAG_UNFROZEN) == 0 && d->dv_blk == InvalidBlockNumber &&
			(!FullTransactionIdIsValid(d->max_xid) || FullTransactionIdPrecedes(d->max_xid, freeze));

		if (!part_clean(d, deltas_settled, oldest))
			span_add(plan, &plan->unfit, lo, hi, InvalidTransactionId, 0, NULL);
		else
		{
			span_add(plan, &plan->clean, lo, hi, newest_xid(newest_xid(xid, dv_xid), delta_xid), (uint32) e->part_id, d);
			if (!frozen)
				span_add(plan, &plan->unfrozen, lo, hi, InvalidTransactionId, 0, NULL);
		}
	}
}

/* ---- the words of a page of the map ---- */

static void
words_mark(uint64 *words, uint64 base, uint64 lo, uint64 hi)
{
	uint64		last = base + SIFT_VM_BLOCKS_PER_PAGE - 1;

	if (hi < base || lo > last)
		return;
	lo = Max(lo, base) - base;
	hi = Min(hi, last) - base;
	for (uint64 w = lo / SIFT_VM_BLOCKS_PER_WORD; w < hi / SIFT_VM_BLOCKS_PER_WORD + 1; w++)
	{
		uint64		m = EVEN_BITS;

		if (w == lo / SIFT_VM_BLOCKS_PER_WORD)
			m &= ~UINT64CONST(0) << ((lo % SIFT_VM_BLOCKS_PER_WORD) * BITS_PER_HEAPBLOCK);
		if (w == hi / SIFT_VM_BLOCKS_PER_WORD)
			m &= ~UINT64CONST(0) >> ((SIFT_VM_BLOCKS_PER_WORD - 1 - hi % SIFT_VM_BLOCKS_PER_WORD) * BITS_PER_HEAPBLOCK);
		words[w] |= m;
	}
}

static void
spans_mark(const Spans *s, uint64 *words, uint64 base, TransactionId *horizon)
{
	for (int i = 0; i < s->n; i++)
	{
		if (horizon != NULL && s->v[i].hi >= base && s->v[i].lo < base + SIFT_VM_BLOCKS_PER_PAGE && TransactionIdIsNormal(s->v[i].xid) && TransactionIdFollows(s->v[i].xid, *horizon))
			*horizon = s->v[i].xid;
		words_mark(words, base, s->v[i].lo, s->v[i].hi);
	}
}

/* The pages of the map whose vblocks are counted together: the rows the clean parts have in each of them, from one reading of the addresses of each part for all of them */
typedef struct CountWindow
{
	uint64		base;			/* the first vblock */
	uint64		nvblocks;
	uint16	   *counts;
	bool		counted;
} CountWindow;

static void
window_open(CountWindow *win, uint64 base)
{
	win->base = base;
	win->counted = false;
}

/* The addresses of the granules of one part that lie in the window, and nothing else of them: no column is read */
static void
count_part(Relation rel, SiftF2Store *store, const Span *s, CountWindow *win, uint64 *tids)
{
	SiftF2PartPin *pin = sift_f2_pin_open(rel, store, s->part, s->part_id);
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift vm granule");

	PG_TRY();
	{
		for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
		{
			const SiftGranuleDesc *gd = &pin->gdir[g];
			MemoryContext old;

			sift_vacuum_delay_point();
			if ((gd->tid_max >> 16) < win->base || (gd->tid_min >> 16) >= win->base + win->nvblocks)
				continue;
			old = MemoryContextSwitchTo(cx);
			sift_f2_pin_granule_tids(rel, pin, g, tids);
			MemoryContextSwitchTo(old);
			MemoryContextReset(cx);
			for (uint16 r = 0; r < gd->nrows; r++)
				if ((tids[r] >> 16) >= win->base && (tids[r] >> 16) < win->base + win->nvblocks)
					win->counts[(tids[r] >> 16) - win->base]++;
		}
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
	MemoryContextDelete(cx);
}

static void
count_window(Relation rel, SiftF2Store *store, const VmPlan *plan, CountWindow *win)
{
	uint64	   *tids = palloc(sizeof(uint64) * SIFT_GRANULE_ROWS);

	memset(win->counts, 0, sizeof(uint16) * win->nvblocks);
	for (int i = 0; i < plan->clean.n; i++)
		if (plan->clean.v[i].hi >= win->base && plan->clean.v[i].lo < win->base + win->nvblocks)
			count_part(rel, store, &plan->clean.v[i], win, tids);
	pfree(tids);
	win->counted = true;
}

/* The vblocks the plan lets be all visible but are not yet and are not full are taken out of visible (a hole in the addresses of a vblock may be an index entry of a row that is not there) */
static void
drop_partial(Relation rel, SiftF2Store *store, const VmPlan *plan, uint64 base, uint64 *visible, const uint64 *set, CountWindow *win)
{
	for (int w = 0; w < (int) SIFT_VM_WORDS_PER_PAGE; w++)
	{
		uint64		fresh = visible[w] & ~set[w];

		for (; fresh != 0; fresh &= fresh - 1)
		{
			int			v = w * SIFT_VM_BLOCKS_PER_WORD + pg_rightmost_one_pos64(fresh) / BITS_PER_HEAPBLOCK;

			if (!win->counted)
				count_window(rel, store, plan, win);
			if (win->counts[base - win->base + v] != SIFT_TBL_ROWS_PER_VBLOCK)
				visible[w] &= ~(UINT64CONST(1) << (v % SIFT_VM_BLOCKS_PER_WORD * BITS_PER_HEAPBLOCK));
		}
	}
}

/* The page of the map that covers base set to what the plan allows; the vblocks of the page that hold rows */
static uint64
set_page(Relation rel, SiftF2Store *store, const VmPlan *plan, uint64 base, CountWindow *win)
{
	uint64		clean[SIFT_VM_WORDS_PER_PAGE] = {0},
				unfit[SIFT_VM_WORDS_PER_PAGE] = {0},
				unfrozen[SIFT_VM_WORDS_PER_PAGE] = {0},
				below[SIFT_VM_WORDS_PER_PAGE] = {0},
				set[SIFT_VM_WORDS_PER_PAGE] = {0},
				bits[SIFT_VM_WORDS_PER_PAGE];
	uint16		idx[SIFT_VM_WORDS_PER_PAGE];
	TransactionId horizon = InvalidTransactionId;
	uint64		occupied = 0;
	int			n = 0;
	uint8		flags = VISIBILITYMAP_ALL_VISIBLE;
	Buffer		vmbuf = InvalidBuffer;

	spans_mark(&plan->clean, clean, base, &horizon);
	spans_mark(&plan->unfit, unfit, base, NULL);
	spans_mark(&plan->unfrozen, unfrozen, base, NULL);
	words_mark(below, base, 0, Min(plan->floor, plan->limit) - 1);
	for (int w = 0; w < (int) SIFT_VM_WORDS_PER_PAGE; w++)
	{
		occupied += pg_popcount64(clean[w] | unfit[w]);
		clean[w] = clean[w] & below[w] & ~unfit[w];
	}
	if (occupied == 0)
		return 0;
	(void) visibilitymap_get_status(rel, (BlockNumber) base, &vmbuf);
	if (BufferIsValid(vmbuf))
	{
		for (int w = 0; w < (int) SIFT_VM_WORDS_PER_PAGE; w++)
			set[w] = sift_vm_page_word(BufferGetPage(vmbuf), w) & EVEN_BITS;
		ReleaseBuffer(vmbuf);
		vmbuf = InvalidBuffer;
	}
	drop_partial(rel, store, plan, base, clean, set, win);
	for (int w = 0; w < (int) SIFT_VM_WORDS_PER_PAGE; w++)
	{
		uint64		frozen = clean[w] & ~unfrozen[w];

		if (clean[w] == 0)
			continue;
		idx[n] = w;
		bits[n++] = clean[w] | (frozen << 1);
		flags |= frozen != 0 ? VISIBILITYMAP_ALL_FROZEN : 0;
	}
	if (n > 0)
	{
		visibilitymap_pin(rel, (BlockNumber) base, &vmbuf);
		(void) sift_vm_apply_set(rel, vmbuf, flags, horizon, idx, bits, n);
		ReleaseBuffer(vmbuf);
	}
	return occupied;
}

/* ---- the counts ---- */

/* Sets the bit that says a map may exist (before the first bit is set), or stores the counts of the map fork; the registry gets the metapage as it is */
static void
store_counts(Relation rel, bool counts, BlockNumber all_visible, BlockNumber all_frozen, uint64 total)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);
	SiftShmIndexMeta peek;

	m->common.ro_compat_flags |= SIFT_ROCOMPAT_TABLE_VM;
	if (counts)
	{
		m->vm_all_visible_vblocks = all_visible;
		m->vm_all_frozen_vblocks = all_frozen;
		m->vm_total_vblocks = total;
	}
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
	if (sift_shm_manifest_peek(&rel->rd_locator, &peek))
	{
		SiftColMetaPageData now = sift_col_meta_read(rel);
		SiftShmIndexMeta shm = sift_col_meta_shm(rel, &now);

		(void) sift_shm_manifest_publish(&shm, NULL);
	}
}

void
sift_tbl_vm_vacuum(Relation rel, FullTransactionId oldest, FullTransactionId freeze)
{
	SiftColMetaPageData meta;
	SiftF2Store *store = sift_f2_store_open(rel);
	MemoryContext cx,
				old;
	SiftManifestRead mr;
	VmPlan		plan = {0};
	BlockNumber all_visible,
				all_frozen;
	uint64		total = 0;

	if (!sift_tbl_vm_active(rel) || !ConditionalLockRelation(rel, ShareLock))
		return;
	(void) sift_ttl_rearm_rocompat(rel);
	meta = sift_col_meta_read(rel);
	plan.limit = Min(meta.next_vblock, InvalidBlockNumber);
	plan.next = ReadNextFullTransactionId();
	if ((meta.common.ro_compat_flags & SIFT_ROCOMPAT_TTL) == 0 && sift_shm_tid_lease_floor(&rel->rd_locator, plan.limit, &plan.floor) && plan.floor > 0)
	{
		const Size	budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2);
		CountWindow win = {.nvblocks = Max(budget / 4 / sizeof(uint16) / SIFT_VM_BLOCKS_PER_PAGE, 1) * SIFT_VM_BLOCKS_PER_PAGE};

		cx = sift_mem_allocset_create(CurrentMemoryContext, "sift vm plan");
		old = MemoryContextSwitchTo(cx);
		plan.budget = budget - budget / 4;
		win.counts = palloc(sizeof(uint16) * win.nvblocks);
		mr = sift_manifest_read_range(rel, NULL, NULL);
		for (int i = 0; i < mr.n && !plan.incomplete; i++)
		{
			sift_vacuum_delay_point();
			plan_partition(rel, store, &mr.entries[i], oldest, freeze, &plan);
		}
		if (plan.incomplete)
			ereport(WARNING, (errmsg("the visibility map of \"%s\" is left as it was: the parts take more memory than this VACUUM has", RelationGetRelationName(rel)),
							  errhint("Raise maintenance_work_mem.")));
		else
		{
			if ((meta.common.ro_compat_flags & SIFT_ROCOMPAT_TABLE_VM) == 0)
				store_counts(rel, false, 0, 0, 0);
			for (uint64 base = 0; base < plan.limit; base += SIFT_VM_BLOCKS_PER_PAGE)
			{
				sift_vacuum_delay_point();
				if (base % win.nvblocks == 0)
					window_open(&win, base);
				total += set_page(rel, store, &plan, base, &win);
			}
		}
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		if (!plan.incomplete)
		{
			visibilitymap_count(rel, &all_visible, &all_frozen);
			store_counts(rel, true, all_visible, all_frozen, total);
		}
	}
	UnlockRelation(rel, ShareLock);
}

/* ---- the planner ---- */

void
sift_tbl_vm_fractions(Relation rel, BlockNumber nblocks, BlockNumber *allvisible, BlockNumber *allfrozen, double *allvisfrac)
{
	SiftShmIndexMeta peek;
	SiftColMetaPageData meta;
	double		total,
				visible,
				frozen;

	*allvisible = *allfrozen = 0;
	*allvisfrac = 0.0;
	if (!sift_vm_rmgr_active())
		return;
	if (!sift_shm_manifest_peek(&rel->rd_locator, &peek))
	{
		meta = sift_col_meta_read(rel);
		peek = sift_col_meta_shm(rel, &meta);
	}
	total = (double) peek.vm_total_vblocks;
	visible = peek.vm_all_visible_vblocks;
	frozen = peek.vm_all_frozen_vblocks;
	if (total <= 0)
		return;
	*allvisfrac = Min(visible / total, 1.0);
	*allvisible = (BlockNumber) rint(nblocks * *allvisfrac);
	*allfrozen = (BlockNumber) rint(nblocks * Min(frozen / total, *allvisfrac));
}
