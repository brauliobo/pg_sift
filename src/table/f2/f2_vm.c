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
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/f2/col_meta.h"
#include "col/f2/l0_slab.h"
#include "col/f2/pend_del.h"
#include "col/f2/rowbuf.h"
#include "core/f2/f2_vm_xlog.h"
#include "core/memtrack.h"
#include "core/shm_manifest.h"
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
} VmPlan;

static void
span_add(Spans *s, uint64 lo, uint64 hi, TransactionId xid, uint32 part_id, const SiftPartDesc *part)
{
	if (s->n == s->cap)
	{
		const Size	bytes = sizeof(Span) * (Size) (s->cap ? s->cap * 2 : 64),
					budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1);

		if (bytes > budget)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the visibility map pass needs %zu bytes for the spans of the parts, more than its memory budget of %zu bytes", bytes, budget),
							errhint("Raise maintenance_work_mem.")));
		s->cap = bytes / sizeof(Span);
		s->v = s->v == NULL ? palloc(bytes) : repalloc(s->v, bytes);
	}
	s->v[s->n++] = (Span) {lo, hi, xid, part_id, part};
}

static TransactionId
xid_of(FullTransactionId fx)
{
	return FullTransactionIdIsValid(fx) ? XidFromFullTransactionId(fx) : InvalidTransactionId;
}

static void
span_of_tids(Spans *s, const uint64 *tids, int n, size_t stride)
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
		span_add(s, lo, hi, InvalidTransactionId, 0, NULL);
}

/* The vblocks of the rows a slab or the pending deletes of the partition name: they are not in a part yet, or still stand for a row of one */
static void
unfit_unsettled(Relation rel, SiftF2Store *store, const SiftPartitionEntry *e, Spans *unfit)
{
	BlockNumber *pages;
	int			npages;

	if (e->l0_slab_blk != InvalidBlockNumber)
	{
		SiftRowBuf *rb = sift_rowbuf_new(&store->spec);

		sift_l0_slab_read(rel, e->part_id, e->l0_slab_blk, rb, &store->spec, SnapshotAny, &pages, &npages);
		span_of_tids(unfit, rb->tid, rb->n, sizeof(uint64));
	}
	if (e->pend_del_blk != InvalidBlockNumber)
	{
		int			n;
		SiftPendDelRec *recs = sift_pend_del_read(rel, e->part_id, e->pend_del_blk, &n, &pages, &npages);

		span_of_tids(unfit, &recs->tid, n, sizeof(SiftPendDelRec));
	}
}

static bool
part_clean(const SiftPartDesc *d, bool deltas_settled, FullTransactionId oldest)
{
	bool		inserts_sealed = (d->flags & SIFT_PART_FLAG_UNFROZEN) == 0 || ((d->flags & SIFT_PART_FLAG_ALL_VISIBLE) != 0 && FullTransactionIdPrecedes(d->max_xid, oldest));
	bool		deletes_cleaned = d->dv_blk == InvalidBlockNumber || ((d->flags & SIFT_PART_FLAG_DV_SETTLED) != 0 && FullTransactionIdPrecedes(d->dv_max_xid, oldest));

	return inserts_sealed && deletes_cleaned && deltas_settled && (d->ext_flags & SIFT_PART_EXT_EXPIRED) == 0 && d->tid_max != 0;
}

static void
plan_partition(Relation rel, SiftF2Store *store, const SiftPartitionEntry *e, FullTransactionId oldest, FullTransactionId freeze, VmPlan *plan)
{
	int			np;
	SiftPartDesc *parts = sift_manifest_read_parts(rel, e, &np);
	SiftColDeltaAnchor chain = sift_manifest_read_col_delta(rel, e);
	bool		deltas_settled = (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0 || (FullTransactionIdIsValid(chain.max_xid) && FullTransactionIdPrecedes(chain.max_xid, oldest));

	unfit_unsettled(rel, store, e, &plan->unfit);
	for (int i = 0; i < np; i++)
	{
		const SiftPartDesc *d = &parts[i];
		uint64		lo = d->tid_max != 0 ? d->tid_min >> 16 : 0,
					hi = d->tid_max != 0 ? d->tid_max >> 16 : UINT64_MAX;
		TransactionId xid = xid_of(d->max_xid),
					dv_xid = xid_of(d->dv_max_xid);
		bool		frozen = (d->flags & SIFT_PART_FLAG_UNFROZEN) == 0 && d->dv_blk == InvalidBlockNumber &&
			(!FullTransactionIdIsValid(d->max_xid) || FullTransactionIdPrecedes(d->max_xid, freeze));

		if (!part_clean(d, deltas_settled, oldest))
			span_add(&plan->unfit, lo, hi, InvalidTransactionId, 0, NULL);
		else
		{
			span_add(&plan->clean, lo, hi, TransactionIdFollows(dv_xid, xid) ? dv_xid : xid, (uint32) e->part_id, d);
			if (!frozen)
				span_add(&plan->unfrozen, lo, hi, InvalidTransactionId, 0, NULL);
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
		if (horizon != NULL && s->v[i].hi >= base && s->v[i].lo < base + SIFT_VM_BLOCKS_PER_PAGE && TransactionIdFollows(s->v[i].xid, *horizon))
			*horizon = s->v[i].xid;
		words_mark(words, base, s->v[i].lo, s->v[i].hi);
	}
}

/* The rows the clean parts that overlap the page of the map have in each of its vblocks */
static void
count_rows(Relation rel, SiftF2Store *store, const VmPlan *plan, uint64 base, uint16 *counts)
{
	memset(counts, 0, sizeof(uint16) * SIFT_VM_BLOCKS_PER_PAGE);
	for (int i = 0; i < plan->clean.n; i++)
	{
		const Span *s = &plan->clean.v[i];
		SiftF2PartPin *pin;

		if (s->hi < base || s->lo >= base + SIFT_VM_BLOCKS_PER_PAGE)
			continue;
		pin = sift_f2_pin_open(rel, store, s->part, s->part_id);
		PG_TRY();
		{
			for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
			{
				SiftF2GranuleBuf *gb = sift_f2_gbuf_load(rel, store, pin, g);
				const uint64 *tids = sift_f2_gbuf_tids(gb);

				for (uint16 r = 0; r < gb->nrows; r++)
					if ((tids[r] >> 16) >= base && (tids[r] >> 16) < base + SIFT_VM_BLOCKS_PER_PAGE)
						counts[(tids[r] >> 16) - base]++;
				sift_f2_gbuf_unref(gb);
			}
		}
		PG_FINALLY();
		{
			sift_f2_pin_release(pin);
		}
		PG_END_TRY();
	}
}

/* The vblocks the plan lets be all visible but are not yet and are not full are taken out of visible (a hole in the addresses of a vblock may be an index entry of a row that is not there) */
static void
drop_partial(Relation rel, SiftF2Store *store, const VmPlan *plan, uint64 base, uint64 *visible, const uint64 *set)
{
	uint16	   *counts = palloc(sizeof(uint16) * SIFT_VM_BLOCKS_PER_PAGE);
	bool		counted = false;

	for (int w = 0; w < (int) SIFT_VM_WORDS_PER_PAGE; w++)
	{
		uint64		fresh = visible[w] & ~set[w];

		for (; fresh != 0; fresh &= fresh - 1)
		{
			int			v = w * SIFT_VM_BLOCKS_PER_WORD + pg_rightmost_one_pos64(fresh) / BITS_PER_HEAPBLOCK;

			if (!counted)
				count_rows(rel, store, plan, base, counts);
			counted = true;
			if (counts[v] != SIFT_TBL_ROWS_PER_VBLOCK)
				visible[w] &= ~(UINT64CONST(1) << (v % SIFT_VM_BLOCKS_PER_WORD * BITS_PER_HEAPBLOCK));
		}
	}
	pfree(counts);
}

/* The page of the map that covers base set to what the plan allows; the vblocks of the page that hold rows */
static uint64
set_page(Relation rel, SiftF2Store *store, const VmPlan *plan, uint64 base)
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
	drop_partial(rel, store, plan, base, clean, set);
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

/* Sets the bit that says a map may exist (before the first bit is set), or stores the counts of the map fork; the same counts go to the registry */
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
	if (counts && sift_shm_manifest_peek(&rel->rd_locator, &peek))
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
	meta = sift_col_meta_read(rel);
	plan.limit = meta.next_vblock;
	if ((meta.common.ro_compat_flags & SIFT_ROCOMPAT_TTL) == 0 && sift_shm_tid_lease_floor(&rel->rd_locator, meta.next_vblock, &plan.floor) && plan.floor > 0)
	{
		cx = sift_mem_allocset_create(CurrentMemoryContext, "sift vm plan");
		old = MemoryContextSwitchTo(cx);
		mr = sift_manifest_read_range(rel, NULL, NULL);
		for (int i = 0; i < mr.n; i++)
			plan_partition(rel, store, &mr.entries[i], oldest, freeze, &plan);
		if ((meta.common.ro_compat_flags & SIFT_ROCOMPAT_TABLE_VM) == 0)
			store_counts(rel, false, 0, 0, 0);
		for (uint64 base = 0; base < plan.limit; base += SIFT_VM_BLOCKS_PER_PAGE)
			total += set_page(rel, store, &plan, base);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		visibilitymap_count(rel, &all_visible, &all_frozen);
		store_counts(rel, true, all_visible, all_frozen, total);
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
