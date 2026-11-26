/*
 * core/f2/manifest/counters.c - the two counters of the metapage a change takes numbers from: the generation of a part and the id of a partition. The generations are leased in chunks
 * (core/shm/lease.h): Block 0 is locked once for SIFT_LEASE_GEN_CHUNK of them.
 */
#include "core/host.h"

#include "col/f2/meta.h"
#include "core/f2/manifest/manifest.h"
#include "core/page_io.h"
#include "core/shm/lease.h"

static uint64
bump_counter(Relation rel, bool part_id, uint64 n)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);
	uint64	   *counter = part_id ? &m->next_part_id : &m->next_gen;
	uint64		first = *counter;

	SIFT_NOTE_BLOCK0_X(SIFT_B0_COUNTER);
	*counter += n;
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
	return first;
}

static uint64
gen_chunk(void *arg, uint64 want, uint64 *got)
{
	*got = want;
	return bump_counter((Relation) arg, false, want);
}

uint64
sift_manifest_new_gen(Relation rel, uint32 n)
{
	uint64		got;

	return sift_lease_take(SIFT_LEASE_GEN, &rel->rd_locator, n, n, gen_chunk, rel, &got);
}

uint64
sift_manifest_new_part_id(Relation rel)
{
	return bump_counter(rel, true, 1);
}
