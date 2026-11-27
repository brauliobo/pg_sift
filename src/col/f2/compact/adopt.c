/*
 * col/f2/compact/adopt.c - the deletes a compaction adopts (adopt.h).
 */
#include "core/host.h"

#include "miscadmin.h"

#include "col/f2/compact/adopt.h"
#include "col/f2/del/delete.h"
#include "core/memtrack.h"

#ifdef SIFT_TESTING
void
sift_compact_test_delete_meanwhile(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartDesc *d, const SiftPartSpec *spec)
{
	MemoryContext icx;
	SiftPart   *part = sift_compact_open_input(rel, d, (uint32) part_id, &icx);
	SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
	uint64	   *tids = sift_part_read_tids(rel, part, dir, 0);
	int			n = Min(sift_test_compact_delete, dir[0].nrows);
	SiftPendDelRec *recs = palloc(Max(n, 1) * sizeof(SiftPendDelRec));

	MemoryContextDelete(icx);
	for (int i = 0; i < n; i++)
		recs[i] = (SiftPendDelRec) {.tid = tids[i],.xmax = InvalidTransactionId,.cmax = 0};
	sift_test_compact_delete = 0;
	(void) sift_del_bulk(rel, part_id, key_lo, recs, n, false, spec);
}
#endif

void
sift_compact_deletes_reserve(SiftCompactDeletes *del, int64 more)
{
	Size		bytes;

	if (more == 0)
		return;
	if (more > SIFT_COMPACT_ADOPT_MAX - del->n)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: a compaction adopts at most %d deletes that reached its inputs", SIFT_COMPACT_ADOPT_MAX),
						errhint("Run VACUUM again once the deletes settle.")));
	bytes = ((Size) del->n + more) * sizeof(SiftPendDelRec);
	del->recs = del->recs ? repalloc_extended(del->recs, bytes, MCXT_ALLOC_HUGE) : palloc_extended(bytes, MCXT_ALLOC_HUGE);
}

/* Where the deletes that reached the inputs land in the outputs: marked in their deletion vectors when write, only found otherwise */
static void
place_deletes(Relation rel, uint64 part_id, const SiftPendDelRec *recs, int n, SiftPartDesc *out, int nout, bool *done, bool write)
{
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift compact adopt");

	for (int k = 0; k < nout; k++)
	{
		MemoryContext old = MemoryContextSwitchTo(scratch);
		SiftPart   *part = sift_part_open(rel, out[k].header_blk, out[k].part_gen, (uint32) part_id);
		SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
		SiftDvMark *marks;
		uint16	   *grows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));
		SiftDv	   *base;
		int			nm;

		CHECK_FOR_INTERRUPTS();
		for (uint32 g = 0; g < part->hdr.ngranules; g++)
			grows[g] = dir[g].nrows;
		base = out[k].dv_blk != InvalidBlockNumber ? sift_dv_read(rel, &out[k], (uint32) part_id, grows) : NULL;
		nm = sift_del_locate(rel, part, dir, recs, n, done, &marks, base, NULL);
		if (write && nm > 0)
		{
			sift_compact_release_outputs_dv(rel, &out[k]);
			(void) sift_dv_write(rel, sift_dv_merge(base, marks, nm, out[k].part_gen, sift_manifest_new_gen(rel, 1), part->hdr.ngranules, grows), (uint32) part_id, &out[k]);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(scratch);
	}
	MemoryContextDelete(scratch);
}

static bool *
none_done(Size n)
{
	return palloc_extended(Max(n, 1) * sizeof(bool), MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
}

/*
 * Whether a delete that reached the inputs while the outputs were written names an address that more rows of the outputs have than there are deletes for it: the heap gave the address to
 * another row, and which of them the delete is for is not known. Every delete is asked for twice: the second of a pair finds a row when the address has one more.
 */
bool
sift_compact_adoption_ambiguous(Relation rel, uint64 part_id, const SiftCompactDeletes *del, SiftPartDesc *out, int nout)
{
	const Size	n = del->n;
	SiftPendDelRec *twice = palloc_extended(2 * n * sizeof(SiftPendDelRec), MCXT_ALLOC_HUGE);
	bool	   *done = none_done(2 * n);
	bool		ambiguous = false;

	memcpy(twice, del->recs, n * sizeof(SiftPendDelRec));
	memcpy(twice + n, del->recs, n * sizeof(SiftPendDelRec));
	place_deletes(rel, part_id, twice, 2 * del->n, out, nout, done, false);
	for (Size i = n; i < 2 * n && !ambiguous; i++)
		ambiguous = done[i];
	pfree(done);
	pfree(twice);
	return ambiguous;
}

/* The deletes that reached the inputs while the outputs were written, put into the deletion vectors of the outputs, which the commit makes the vectors of the parts that replace the inputs */
void
sift_compact_adopt_deletes(Relation rel, uint64 part_id, const SiftCompactDeletes *del, SiftPartDesc *out, int nout)
{
	bool	   *done = none_done(del->n);

	place_deletes(rel, part_id, del->recs, del->n, out, nout, done, true);
	pfree(done);
}
