/*
 * core/f2/manifest/counters.c - the two counters of the metapage a change takes numbers from: the generation of a part and the id of a partition.
 */
#include "core/host.h"

#include "col/f2/meta.h"
#include "core/f2/manifest/manifest.h"
#include "core/page_io.h"

static void
bump_counter(Relation rel, bool part_id, uint32 n, uint64 *first)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);
	uint64	   *counter = part_id ? &m->next_part_id : &m->next_gen;

	*first = *counter;
	*counter += n;
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
}

uint64
sift_manifest_new_gen(Relation rel, uint32 n)
{
	uint64		first;

	bump_counter(rel, false, n, &first);
	return first;
}

uint64
sift_manifest_new_part_id(Relation rel)
{
	uint64		first;

	bump_counter(rel, true, 1, &first);
	return first;
}
