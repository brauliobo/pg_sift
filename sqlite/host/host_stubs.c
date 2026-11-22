/*
 * host_stubs.c - the collaborators of src/bm25/eval.c that PostgreSQL provides and the embedded hosts (SQLite, MariaDB) do not
 * need, the same on both: docmap directories are not kept between queries and there are no write buffers (the live
 * segment of a query is an immutable one built from the queue). Options, visibility and the TID bitmap are the host's own.
 */
#include "core/host.h"

#include "bm25/analyze.h"
#include "bm25/hitspill.h"
#include "bm25/wbuf.h"
#include "bm25/segment.h"

/* ---------- the heap (core/host.h): the embedded hosts leave it to the system ---------- */

void
sift_host_pin_heap(void)
{
}

/* ---------- the sort of the hits of an unbounded ranked search (bm25/hitspill.h): the hosts have none, so such a search fails past its memory ---------- */

SiftHitSpill *
sift_hitspill_begin(MemoryContext mcxt, int work_mem_kb)
{
	return NULL;
}

void
sift_hitspill_put(SiftHitSpill *s, SiftKey key, float4 score)
{
}

void
sift_hitspill_sort(SiftHitSpill *s)
{
}

bool
sift_hitspill_next(SiftHitSpill *s, SiftKey *key, float4 *score)
{
	return false;
}

void
sift_hitspill_end(SiftHitSpill *s)
{
}

/* ---------- the name of an index in a hint (core/store.h): a table of the host has no schema ---------- */

const char *
sift_index_ident(Relation index)
{
	return RelationGetRelationName(index);
}

/* ---------- text search configurations (bm25/analyze_ts.c): a PostgreSQL catalog object, so the hosts have none ---------- */

SiftTsStem *
sift_ts_open(const char *config, MemoryContext mcxt)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("unknown sift stemmer \"%s\"", config),
					errhint("Valid stemmers are \"en\" and \"none\".")));
	return NULL;
}

int
sift_ts_stem(SiftTsStem *ts, char *w, int len)
{
	return len;
}

/* ---------- INCLUDE columns (bm25/attr.c): the hosts keep none ---------- */

SiftAttrSrc *
sift_attr_src(SiftWriteTarget *wt, MemoryContext mcxt)
{
	return NULL;
}

/* ---------- bytes of a segment (bm25/segcache.c) and docmap directories (bm25/docdir.c): not kept between queries ---------- */

bool
sift_segcache_get(const SiftStreamReader *sr, int kind, uint64 id, Size size, MemoryContext mcxt, void **obj, void **handle)
{
	*obj = MemoryContextAllocAligned(mcxt, size, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
	*handle = NULL;
	return false;
}

bool
sift_segcache_has(const SiftStreamReader *sr, int kind, uint64 id, Size size)
{
	return false;
}

void
sift_segcache_ready(void *handle)
{
}


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

/* ---------- write buffers (bm25/wbuf.c): the directory never lists one ---------- */

bool
sift_wbuf_bloom_has(const uint64 *bloom, int nwords, uint32 hash)
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
sift_wbuf_note_walked(const SiftWbufReader *r)
{
}

void
sift_wbuf_totals(const SiftWbufReader *r, uint64 *ndocs, uint64 *sum_dl)
{
}

void
sift_wbuf_close(SiftWbufReader *r)
{
}
