/*
 * agg/v6/v6_vet.c - the rows of a batch against the heap (agg/v6/v6_vet.h).
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"

#include "agg/v6/v6_vet.h"
#include "col/am.h"

void
sift_v6_vet_begin(SiftV6Vet *v, Relation heap, Snapshot snap, SiftV6ScanStats *stats)
{
	*v = (SiftV6Vet) {.heap = heap,.snap = snap,.stats = stats,.vm_blk = InvalidBlockNumber};
	v->fetch = sift_table_index_fetch_begin(heap);
	v->slot = table_slot_create(heap, NULL);
}

void
sift_v6_vet_end(SiftV6Vet *v)
{
	if (BufferIsValid(v->vmbuf))
		ReleaseBuffer(v->vmbuf);
	ExecDropSingleTupleTableSlot(v->slot);
	table_index_fetch_end(v->fetch);
}

bool
sift_v6_vet_fetch(SiftV6Vet *v, uint64 addr)
{
	ItemPointerData tid;
	bool		again = false,
				dead = false;

	sift_f2_tid_set(&tid, addr);
	return table_index_fetch_tuple(v->fetch, &tid, v->snap, v->slot, &again, &dead);
}

static bool
row_visible(SiftV6Vet *v, uint64 addr)
{
	ItemPointerData tid;

	sift_f2_tid_set(&tid, addr);
	if (ItemPointerGetBlockNumber(&tid) != v->vm_blk)
	{
		v->vm_blk = ItemPointerGetBlockNumber(&tid);
		v->vm_visible = VM_ALL_VISIBLE(v->heap, v->vm_blk, &v->vmbuf);
	}
	if (v->vm_visible)
		return true;
	v->stats->heap_vet_rows++;
	if (sift_v6_vet_fetch(v, addr))
		return true;
	v->stats->heap_rows_dead++;
	return false;
}

uint16
sift_v6_vet_mask(SiftV6Vet *v, const uint64 *tid, int off, bool visible, int n, SiftBatchSel *bs)
{
	const int	words = (n + 63) / 64;
	uint16		active = 0;

	for (int w = 0; w < words; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const int	r = w * 64 + __builtin_ctzll(bits);

			if (visible || row_visible(v, tid[off + r]))
				active++;
			else
				bs->mask[w] &= ~(UINT64CONST(1) << (r & 63));
		}
	bs->n_raw = n;
	bs->n_active = active;
	bs->all_active = active == n;
	bs->has_sel = false;
	return active;
}

static bool
ctid_in(SiftV6Vet *v, uint64 lo, uint64 hi, uint64 addr)
{
	ItemPointerData tid;
	BlockNumber blk;

	sift_f2_tid_set(&tid, addr);
	blk = ItemPointerGetBlockNumberNoCheck(&tid);
	if (blk < (BlockNumber) (lo >> 16) || blk > (BlockNumber) ((hi - 1) >> 16))
		return false;
	if (blk > (BlockNumber) (lo >> 16) && blk < (BlockNumber) ((hi - 1) >> 16))
		return true;
	if (!sift_v6_vet_fetch(v, addr))
		return false;
	tid = v->slot->tts_tid;
	addr = (uint64) ItemPointerGetBlockNumberNoCheck(&tid) << 16 | ItemPointerGetOffsetNumberNoCheck(&tid);
	return addr >= lo && addr < hi;
}

uint16
sift_v6_tid_mask(SiftV6Vet *v, uint64 lo, uint64 hi, const uint64 *tid, int off, int n, SiftBatchSel *bs)
{
	const int	words = (n + 63) / 64;
	uint16		active = 0;

	for (int w = 0; w < words; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const int	r = w * 64 + __builtin_ctzll(bits);

			if (lo < hi && ctid_in(v, lo, hi, tid[off + r]))
				active++;
			else
				bs->mask[w] &= ~(UINT64CONST(1) << (r & 63));
		}
	bs->n_raw = n;
	bs->n_active = active;
	bs->all_active = active == n;
	bs->has_sel = false;
	return active;
}
