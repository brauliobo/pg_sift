/*
 * host_stubs.c - the collaborators of src/sift_eval.c that PostgreSQL provides and the embedded hosts (SQLite, MariaDB) do not
 * need, the same on both: docmap directories are not kept between queries and there are no mutable segments (the live
 * segment of a query is an immutable one built from the queue). Options, visibility and the TID bitmap are the host's own.
 */
#include "sift_host.h"

#include "sift_mutable.h"
#include "sift_segment.h"

/* ---------- docmap directories (sift_docdir.c): not kept between queries ---------- */

bool
sift_docdir_get(SiftSegment *seg, uint32 nsb, SiftDocSBRef **dir, uint64 **rank_base, void **handle)
{
	*dir = MemoryContextAllocHuge(seg->mcxt, nsb * sizeof(SiftDocSBRef));
	*rank_base = MemoryContextAllocHuge(seg->mcxt, (nsb + 1) * sizeof(uint64));
	*handle = NULL;
	return false;
}

void
sift_docdir_ready(void *handle)
{
}

/* ---------- mutable segments (sift_mutable.c): the directory never lists one ---------- */

bool
sift_mut_bloom_has(const uint64 *bloom, uint32 hash)
{
	return false;
}

SiftMutReader *
sift_mut_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftMutPageFilter filter, void *filter_arg)
{
	elog(ERROR, "sift: this build has no mutable segments");
	return NULL;
}

bool
sift_mut_next(SiftMutReader *r, SiftMutDoc *doc)
{
	return false;
}

void
sift_mut_totals(const SiftMutReader *r, uint64 *ndocs, uint64 *sum_dl)
{
}

void
sift_mut_close(SiftMutReader *r)
{
}
