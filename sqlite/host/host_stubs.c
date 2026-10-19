/*
 * host_stubs.c - the collaborators of src/bm25/sift_eval.c that PostgreSQL provides and the embedded hosts (SQLite, MariaDB) do not
 * need, the same on both: docmap directories are not kept between queries and there are no write buffers (the live
 * segment of a query is an immutable one built from the queue). Options, visibility and the TID bitmap are the host's own.
 */
#include "sift_host.h"

#include "sift_wbuf.h"
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

/* ---------- write buffers (sift_wbuf.c): the directory never lists one ---------- */

bool
sift_wbuf_bloom_has(const uint64 *bloom, uint32 hash)
{
	return false;
}

SiftWbufReader *
sift_wbuf_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt, SiftWbufPageFilter filter, void *filter_arg)
{
	elog(ERROR, "sift: this build has no write buffers");
	return NULL;
}

bool
sift_wbuf_next(SiftWbufReader *r, SiftWbufDoc *doc)
{
	return false;
}

void
sift_wbuf_totals(const SiftWbufReader *r, uint64 *ndocs, uint64 *sum_dl)
{
}

void
sift_wbuf_close(SiftWbufReader *r)
{
}
