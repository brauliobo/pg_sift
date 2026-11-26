/*
 * col/f2/l0/stage_part.c - the buffers of the stage of a transaction by partition (col/f2/l0/stage.c): one for each partition its rows were routed to, found by the id of the partition.
 */
#include "core/host.h"

#include "port/pg_bitutils.h"

#include "col/f2/l0/stage_int.h"
#include "core/hash.h"

static uint32
part_home(const SiftTxnStage *st, uint64 part_id)
{
	return (uint32) (sift_fmix64(part_id) & st->part_htab_mask);
}

static void
part_htab_rebuild(SiftTxnStage *st)
{
	MemoryContext old = MemoryContextSwitchTo(st->cx);

	if (st->part_htab != NULL)
		pfree(st->part_htab);
	st->part_htab_mask = pg_nextpower2_32(Max(4 * (uint32) st->nparts, 16)) - 1;
	st->part_htab = palloc0((st->part_htab_mask + 1) * sizeof(uint32));
	MemoryContextSwitchTo(old);
	for (int i = 0; i < st->nparts; i++)
	{
		uint32		h = part_home(st, st->parts[i].part_id);

		while (st->part_htab[h] != 0)
			h = (h + 1) & st->part_htab_mask;
		st->part_htab[h] = i + 1;
	}
}

SiftStagePart *
sift_stage_part_get(SiftTxnStage *st, const SiftPartitionEntry *e)
{
	MemoryContext old;
	SiftStagePart *p;

	if (st->part_htab != NULL)
		for (uint32 h = part_home(st, e->part_id); st->part_htab[h] != 0; h = (h + 1) & st->part_htab_mask)
			if (st->parts[st->part_htab[h] - 1].part_id == e->part_id)
				return &st->parts[st->part_htab[h] - 1];
	old = MemoryContextSwitchTo(st->cx);
	if (st->nparts == st->cap_parts)
	{
		st->cap_parts = st->cap_parts > 0 ? st->cap_parts * 2 : 8;
		st->parts = st->parts ? repalloc(st->parts, st->cap_parts * sizeof(SiftStagePart)) : palloc(st->cap_parts * sizeof(SiftStagePart));
	}
	p = &st->parts[st->nparts];
	*p = (SiftStagePart) {.part_id = e->part_id,.rb = sift_rowbuf_new(&st->spec),.tid_min = UINT64_MAX};
	memcpy(p->key_lo, e->key_lo, SIFT_BOUND_BYTES);
	MemoryContextSwitchTo(old);
	st->nparts++;
	if (st->part_htab == NULL || (uint32) st->nparts * 2 > st->part_htab_mask + 1)
		part_htab_rebuild(st);
	else
	{
		uint32		h = part_home(st, e->part_id);

		while (st->part_htab[h] != 0)
			h = (h + 1) & st->part_htab_mask;
		st->part_htab[h] = st->nparts;
	}
	sift_stage_part_changed(st, p);
	return p;
}
