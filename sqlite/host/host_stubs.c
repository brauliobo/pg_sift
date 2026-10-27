/*
 * host_stubs.c - the collaborators of src/bm25/eval.c that PostgreSQL provides and the embedded hosts (SQLite, MariaDB) do not
 * need, the same on both: docmap directories are not kept between queries and there are no write buffers (the live
 * segment of a query is an immutable one built from the queue). Options, visibility and the TID bitmap are the host's own.
 */
#include "core/host.h"

#include "bm25/wbuf.h"
#include "bm25/segment.h"
#include "col/seg_int.h"

/* ---------- docmap directories (bm25/docdir.c): not kept between queries ---------- */

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

/* ---------- parsed column dictionaries (col/dictcache.c): not kept between queries ---------- */

SiftCDict *
sift_cseg_dict_cached(SiftColSegment *seg, int col, SiftCDictLoad load)
{
	return load(seg, col, seg->mcxt);
}

SiftCDict *
sift_cseg_dict_peek(SiftColSegment *seg, int col)
{
	return NULL;
}

/* nothing is counted: every dictionary that is tested is parsed whole, as it is once the backend has asked often */
uint32
sift_cseg_dict_asked(SiftColSegment *seg, int col)
{
	return UINT32_MAX;
}

/* there is no cache that could fail to hold a dictionary */
bool
sift_cseg_dict_nokeep(SiftColSegment *seg, int col)
{
	return false;
}

/* ---------- segment heads, zone arrays and bisected positions (col/dictcache.c): not kept between queries, they stay in the segment's context ---------- */

const SegHead *
sift_cseg_head_find(Relation index, const SiftSegEntry *entry, const SiftSegHeaderData *h, MemoryContext mcxt)
{
	return NULL;
}

void
sift_cseg_head_keep(Relation index, const SiftSegEntry *entry, MemoryContext cx, SegHead *hd, MemoryContext mcxt)
{
}

ZoneArrays *
sift_cseg_zones_find(SiftColSegment *seg, int col)
{
	return NULL;
}

void
sift_cseg_zones_keep(SiftColSegment *seg, int col, MemoryContext cx, ZoneArrays *z)
{
}

/* no cache of the positions a dictionary search bisected: every search reads the dictionary */
SiftCBisect *
sift_cseg_bisect(SiftColSegment *seg, int col, uint32 n)
{
	return NULL;
}

SiftCBisect *
sift_cseg_bisect_find(SiftColSegment *seg, int col)
{
	return NULL;
}

/* ---------- write buffers (bm25/wbuf.c): the directory never lists one ---------- */

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

/* ---------- hashing (col/hll.c): the sketches of hll_columns are written by the PostgreSQL extension, whose hash they carry ---------- */

uint64
hash_bytes_extended(const unsigned char *k, int keylen, uint64 seed)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the sketch of a column is built by the PostgreSQL extension only"),
					errhint("Index the column with hll_columns in PostgreSQL.")));
	return 0;
}

uint64
hash_bytes_uint32_extended(uint32 k, uint64 seed)
{
	return hash_bytes_extended(NULL, 0, seed);
}
