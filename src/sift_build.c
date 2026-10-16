/*
 * sift_build.c - inverted buffer, sorted runs and their merge, ambuild and
 * ambuildempty (DESIGN.md (i)).
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/parallel.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "common/int.h"
#include "executor/instrument.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/sharedfileset.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "sift_am.h"
#include "sift_build.h"

#define POOL_SHIFT		16
#define POOL_BLOCK		(1 << POOL_SHIFT)
#define POOL_MAX_BLOCKS	60000		/* addresses stay below 2^32 */
#define POOL(b, a)		((b)->blocks[(a) >> POOL_SHIFT] + ((a) & (POOL_BLOCK - 1)))
#define NLEVELS			9
#define BATCH			1024
#define IO_BUF			65536
#define ULL(x)			((unsigned long long) (x))
#define DOC_KEY(d)		((d) >> 16)
#define DOC_DL(d)		((uint16) (d))

static const uint16 slice_size[NLEVELS] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};

/* A term's postings: varint(delta << 1 | tf > 1) [tf] over doc ordinals, in pool slices */
typedef struct InvTerm
{
	uint32		hash;
	uint32		term_off;
	uint32		df;
	uint32		last;			/* ordinal of the last doc */
	uint32		head;
	uint32		cur;
	uint32		end;			/* link field of the current slice */
	uint16		len;
	uint8		level;
} InvTerm;

struct SiftInvBuf
{
	MemoryContext parent;
	MemoryContext mcxt;			/* reset with the buffer */
	Size		budget;
	uint64	   *docs;			/* key << 16 | dl, one cache line touch per posting */
	uint32		ndocs;
	uint32		maxdocs;
	uint64		sum_dl;
	InvTerm    *terms;
	uint32		nterms;
	uint32		maxterms;
	uint64	   *slots;			/* hash << 32 | term index + 1, 0 = free */
	uint32		mask;
	char	   *tbytes;
	Size		tlen;
	Size		tcap;
	uint8	  **blocks;
	uint32		nblocks;
	uint32		pool_next;
	uint32		maxdf;
	uint32	   *doc_terms;		/* term indexes of the doc being added */
	int			maxdoc_terms;
};

struct SiftRun
{
	BufFile    *file;
	uint64		ndocs;
	uint64		maxdf;
};

typedef struct SliceReader
{
	uint32		cur;
	uint32		end;
	uint8		level;
} SliceReader;

/* Buffered sequential IO on a run file */
typedef struct RunIO
{
	BufFile    *file;
	uint8	   *buf;
	int			pos;
	int			len;
} RunIO;

/* Merge input: one run positioned on a term */
typedef struct RunReader
{
	RunIO		io;
	int			idx;
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;			/* 0 at end */
	uint64		df;
	/* docs */
	uint64		dleft;
	SiftKey		dkey;
	uint16		ddl;
	/* postings of the current term */
	uint64		pleft;
	SiftKey		pkey;
	uint8		ptf;
	uint16		pdl;
} RunReader;

/* Feeds postings of one term to the posting writer in batches */
typedef struct Batch
{
	SiftPostingWriter *pw;
	int			n;
	SiftKey		keys[BATCH];
	uint8		tf[BATCH];
	uint16		dl[BATCH];
} Batch;

/* ---------- inverted buffer ---------- */

static void
inv_init(SiftInvBuf *b)
{
	MemoryContext old = MemoryContextSwitchTo(b->mcxt);

	b->maxdocs = 1024;
	b->docs = palloc(b->maxdocs * sizeof(uint64));
	b->ndocs = 0;
	b->sum_dl = 0;
	b->maxterms = 1024;
	b->terms = palloc(b->maxterms * sizeof(InvTerm));
	b->nterms = 0;
	b->mask = 2 * b->maxterms - 1;
	b->slots = palloc0((b->mask + 1) * sizeof(uint64));
	b->tcap = 16384;
	b->tbytes = palloc(b->tcap);
	b->tlen = 0;
	b->blocks = palloc(POOL_MAX_BLOCKS * sizeof(uint8 *));
	b->nblocks = 0;
	b->pool_next = 0;
	b->maxdf = 0;
	b->doc_terms = NULL;
	b->maxdoc_terms = 0;
	MemoryContextSwitchTo(old);
}

SiftInvBuf *
sift_inv_create(Size budget, MemoryContext mcxt)
{
	SiftInvBuf *b = MemoryContextAllocZero(mcxt, sizeof(SiftInvBuf));

	b->parent = mcxt;
	b->budget = Max(budget, 1024 * 1024);
	b->mcxt = AllocSetContextCreate(mcxt, "sift inverted buffer", ALLOCSET_DEFAULT_SIZES);
	inv_init(b);
	return b;
}

static void
inv_reset(SiftInvBuf *b)
{
	MemoryContextReset(b->mcxt);
	inv_init(b);
}

static Size
inv_mem(const SiftInvBuf *b)
{
	return (Size) b->maxdocs * sizeof(uint64) + (Size) b->maxterms * sizeof(InvTerm) +
		(Size) (b->mask + 1) * sizeof(uint64) + b->tcap + (Size) b->nblocks * POOL_BLOCK;
}

bool
sift_inv_full(const SiftInvBuf *b)
{
	return inv_mem(b) >= b->budget || b->nblocks >= POOL_MAX_BLOCKS - 1 || b->ndocs >= PG_INT32_MAX / 2 ||
		b->tlen >= PG_INT32_MAX;
}

bool
sift_inv_empty(const SiftInvBuf *b)
{
	return b->ndocs == 0;
}

static uint32
pool_alloc(SiftInvBuf *b, uint32 size)
{
	uint32		a;

	if ((b->pool_next >> POOL_SHIFT) >= b->nblocks || (b->pool_next & (POOL_BLOCK - 1)) + size > POOL_BLOCK)
	{
		if (b->nblocks == POOL_MAX_BLOCKS)
			elog(ERROR, "sift: inverted buffer pool exhausted");
		b->blocks[b->nblocks++] = MemoryContextAlloc(b->mcxt, POOL_BLOCK);
		b->pool_next = (b->nblocks - 1) << POOL_SHIFT;
	}
	a = b->pool_next;
	b->pool_next += size;
	return a;
}

static inline void
slice_put(SiftInvBuf *b, InvTerm *t, uint8 v)
{
	if (t->cur == t->end)
	{
		int			lvl = Min(t->level + 1, NLEVELS - 1);
		uint32		a = pool_alloc(b, slice_size[lvl]);

		memcpy(POOL(b, t->end), &a, sizeof(uint32));
		t->cur = a;
		t->end = a + slice_size[lvl] - sizeof(uint32);
		t->level = lvl;
	}
	*POOL(b, t->cur) = v;
	t->cur++;
}

static inline uint8
slice_get(const SiftInvBuf *b, SliceReader *r)
{
	uint8		v;

	if (r->cur == r->end)
	{
		r->level = Min(r->level + 1, NLEVELS - 1);
		memcpy(&r->cur, POOL(b, r->end), sizeof(uint32));
		r->end = r->cur + slice_size[r->level] - sizeof(uint32);
	}
	v = *POOL(b, r->cur);
	r->cur++;
	return v;
}

static void
grow_slots(SiftInvBuf *b)
{
	uint32		mask = b->mask * 2 + 1;
	uint64	   *slots = MemoryContextAllocExtended(b->mcxt, (Size) (mask + 1) * sizeof(uint64),
												   MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);

	for (uint32 i = 0; i <= b->mask; i++)
	{
		uint32		h = (b->slots[i] >> 32) & mask;

		if (b->slots[i] == 0)
			continue;
		while (slots[h])
			h = (h + 1) & mask;
		slots[h] = b->slots[i];
	}
	pfree(b->slots);
	b->slots = slots;
	b->mask = mask;
}

static InvTerm *
term_get(SiftInvBuf *b, const SiftTermFreq *tf)
{
	uint32		h = tf->hash & b->mask;
	InvTerm    *t;

	for (; b->slots[h]; h = (h + 1) & b->mask)
	{
		if ((b->slots[h] >> 32) != tf->hash)
			continue;
		t = &b->terms[(uint32) b->slots[h] - 1];
		if (t->len == tf->len && memcmp(b->tbytes + t->term_off, tf->term, tf->len) == 0)
			return t;
	}
	if (b->nterms == b->maxterms)
	{
		b->maxterms *= 2;
		b->terms = repalloc_huge(b->terms, (Size) b->maxterms * sizeof(InvTerm));
	}
	if (b->tlen + tf->len > b->tcap)
	{
		b->tcap *= 2;
		b->tbytes = repalloc_huge(b->tbytes, b->tcap);
	}
	t = &b->terms[b->nterms];
	memset(t, 0, sizeof(InvTerm));
	t->hash = tf->hash;
	t->len = tf->len;
	t->term_off = b->tlen;
	memcpy(b->tbytes + b->tlen, tf->term, tf->len);
	b->tlen += tf->len;
	t->head = t->cur = pool_alloc(b, slice_size[0]);
	t->end = t->head + slice_size[0] - sizeof(uint32);
	b->slots[h] = (uint64) tf->hash << 32 | ++b->nterms;
	if (b->nterms * 2 > b->mask)
		grow_slots(b);
	return &b->terms[b->nterms - 1];
}

void
sift_inv_add(SiftInvBuf *b, ItemPointer tid, const SiftTermFreq *terms, int nterms, uint32 dl)
{
	SiftKey		key = sift_key_from_tid(tid);
	uint32		ord = b->ndocs;

	if ((ord > 0 && key <= DOC_KEY(b->docs[ord - 1])) || (nterms == 0) != (dl == 0))
		elog(ERROR, "sift: inverted buffer docs must have ascending keys and dl > 0 iff terms");
	if (ord == b->maxdocs)
	{
		b->maxdocs *= 2;
		b->docs = repalloc_huge(b->docs, (Size) b->maxdocs * sizeof(uint64));
	}
	dl = Min(dl, SIFT_MAX_DL);
	b->docs[ord] = key << 16 | dl;
	b->sum_dl += dl;
	b->ndocs++;
	if (nterms > b->maxdoc_terms)
	{
		b->maxdoc_terms = Max(nterms, 2 * b->maxdoc_terms);
		b->doc_terms = b->doc_terms ? repalloc(b->doc_terms, b->maxdoc_terms * sizeof(uint32)) :
			MemoryContextAlloc(b->mcxt, b->maxdoc_terms * sizeof(uint32));
	}
	/* three passes so the cache misses of the slots, terms and slices overlap */
	for (int i = 0; i < nterms; i++)
		__builtin_prefetch(&b->slots[terms[i].hash & b->mask]);
	for (int i = 0; i < nterms; i++)
	{
		InvTerm    *t = term_get(b, &terms[i]);

		b->doc_terms[i] = t - b->terms;
		__builtin_prefetch(POOL(b, t->cur), 1);
	}
	for (int i = 0; i < nterms; i++)
	{
		InvTerm    *t = &b->terms[b->doc_terms[i]];
		uint64		v = (uint64) (ord - t->last) << 1 | (terms[i].tf > 1);

		for (; v >= 0x80; v >>= 7)
			slice_put(b, t, (uint8) (v | 0x80));
		slice_put(b, t, (uint8) v);
		if (terms[i].tf > 1)
			slice_put(b, t, terms[i].tf);
		t->last = ord;
		t->df++;
		b->maxdf = Max(b->maxdf, t->df);
	}
}

/* Next posting of a term: doc ordinal and tf (see term_chunk) */
static inline void
term_next(const SiftInvBuf *b, SliceReader *r, uint32 *ord, uint8 *tf)
{
	uint64		v = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = slice_get(b, r);

		v |= (uint64) (c & 0x7f) << s;
		if (!(c & 0x80))
			break;
	}
	*ord += v >> 1;
	*tf = (v & 1) ? slice_get(b, r) : 1;
}

/* Postings of one term, decoded in chunks whose docs are prefetched */
typedef struct TermIter
{
	SliceReader r;
	uint32		ord;
	uint32		left;
} TermIter;

#define CHUNK	64

static void
term_iter(const SiftInvBuf *b, const InvTerm *t, TermIter *it)
{
	it->r = (SliceReader) {t->head, t->head + slice_size[0] - sizeof(uint32), 0};
	it->ord = 0;
	it->left = t->df;
}

static int
term_chunk(const SiftInvBuf *b, TermIter *it, uint32 *ords, uint8 *tf)
{
	int			n = Min(it->left, CHUNK);

	for (int i = 0; i < n; i++)
	{
		term_next(b, &it->r, &it->ord, &tf[i]);
		ords[i] = it->ord;
		__builtin_prefetch(&b->docs[it->ord]);
	}
	it->left -= n;
	return n;
}

static int
term_cmp(const char *a, int alen, const char *b, int blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	return c ? c : (alen > blen) - (alen < blen);
}

typedef struct SortTerm
{
	uint64		prefix;
	uint32		idx;
} SortTerm;

static int
sort_term_cmp(const void *x, const void *y, void *arg)
{
	const SortTerm *a = x,
			   *c = y;
	const SiftInvBuf *b = arg;
	const InvTerm *ta = &b->terms[a->idx],
			   *tc = &b->terms[c->idx];

	if (a->prefix != c->prefix)
		return a->prefix < c->prefix ? -1 : 1;
	return term_cmp(b->tbytes + ta->term_off, ta->len, b->tbytes + tc->term_off, tc->len);
}

/* Term indexes in byte order (allocated in the buffer's context) */
static SortTerm *
sorted_terms(SiftInvBuf *b)
{
	SortTerm   *s = MemoryContextAllocHuge(b->mcxt, Max(b->nterms, 1) * sizeof(SortTerm));

	for (uint32 i = 0; i < b->nterms; i++)
	{
		const char *p = b->tbytes + b->terms[i].term_off;
		uint64		pre = 0;

		for (int k = 0; k < 8; k++)
			pre = pre << 8 | (k < b->terms[i].len ? (uint8) p[k] : 0);
		s[i] = (SortTerm) {pre, i};
	}
	qsort_arg(s, b->nterms, sizeof(SortTerm), sort_term_cmp, b);
	return s;
}

static void
batch_put(Batch *bt, SiftKey key, uint8 tf, uint16 dl)
{
	bt->keys[bt->n] = key;
	bt->tf[bt->n] = tf;
	bt->dl[bt->n] = dl;
	if (++bt->n == BATCH)
	{
		sift_postw_add(bt->pw, bt->keys, bt->tf, bt->dl, bt->n);
		bt->n = 0;
	}
}

static void
batch_flush(Batch *bt)
{
	if (bt->n)
		sift_postw_add(bt->pw, bt->keys, bt->tf, bt->dl, bt->n);
	bt->n = 0;
}

/* Adds packed docs (key << 16 | dl) to a segment in batches */
static void
add_docs(SiftSegmentWriter *sw, const uint64 *docs, uint32 n)
{
	SiftKey		keys[BATCH];
	uint16		dl[BATCH];

	for (uint32 i = 0; i < n; i += BATCH)
	{
		int			k = Min(BATCH, n - i);

		for (int j = 0; j < k; j++)
		{
			keys[j] = DOC_KEY(docs[i + j]);
			dl[j] = DOC_DL(docs[i + j]);
		}
		sift_segw_add_docs(sw, keys, dl, k);
	}
}

void
sift_inv_write(SiftInvBuf *b, SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref, SiftSegEntry *entry)
{
	MemoryContext cxt = AllocSetContextCreate(b->parent, "sift segment write", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftSegmentWriter *sw = sift_segw_begin(wt, gen, ref, cxt);
	SortTerm   *order = sorted_terms(b);
	Batch	   *bt = palloc(sizeof(Batch));

	add_docs(sw, b->docs, b->ndocs);
	for (uint32 i = 0; i < b->nterms; i++)
	{
		const InvTerm *t = &b->terms[order[i].idx];
		TermIter	it;
		uint32		ords[CHUNK];
		uint8		tf[CHUNK];

		bt->pw = sift_segw_begin_term(sw, b->tbytes + t->term_off, t->len);
		bt->n = 0;
		term_iter(b, t, &it);
		for (int n; (n = term_chunk(b, &it, ords, tf)) > 0;)
			for (int k = 0; k < n; k++)
				batch_put(bt, DOC_KEY(b->docs[ords[k]]), tf[k], DOC_DL(b->docs[ords[k]]));
		batch_flush(bt);
		sift_segw_end_term(sw);
		if (i % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
	sift_segw_finish(sw, entry);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	inv_reset(b);
}

/* ---------- runs ---------- */

static void
io_flush(RunIO *io)
{
	BufFileWrite(io->file, io->buf, io->len);
	io->len = 0;
}

static inline void
io_put(RunIO *io, const void *p, int n)
{
	if (io->len + n > IO_BUF)
		io_flush(io);
	memcpy(io->buf + io->len, p, n);
	io->len += n;
}

static inline void
io_varint(RunIO *io, uint64 x)
{
	uint8		v[10];
	int			n = 0;

	for (; x >= 0x80; x >>= 7)
		v[n++] = (uint8) (x | 0x80);
	v[n++] = (uint8) x;
	io_put(io, v, n);
}

static inline uint8
io_byte(RunIO *io)
{
	if (io->pos == io->len)
	{
		io->len = BufFileRead(io->file, io->buf, IO_BUF);
		io->pos = 0;
		if (io->len == 0)
			elog(ERROR, "sift: unexpected end of a build run");
	}
	return io->buf[io->pos++];
}

static inline uint64
io_get_varint(RunIO *io)
{
	uint64		x = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = io_byte(io);

		x |= (uint64) (c & 0x7f) << s;
		if (!(c & 0x80))
			return x;
	}
}

/*
 * Run file: varint ndocs, (varint key delta, varint dl) per doc; per term
 * in byte order: varint len, bytes, varint df, then per posting
 * varint(key delta << 1 | tf > 1) [tf] varint dl; varint 0 ends the run.
 */
static SiftRun *
spill_to(SiftInvBuf *b, BufFile *file)
{
	SiftRun    *run = MemoryContextAllocZero(b->parent, sizeof(SiftRun));
	MemoryContext old = MemoryContextSwitchTo(b->mcxt);
	RunIO		io = {.buf = palloc(IO_BUF),.file = file};
	SortTerm   *order = sorted_terms(b);
	SiftKey		prev = 0;

	run->file = file;
	run->ndocs = b->ndocs;
	run->maxdf = b->maxdf;
	io_varint(&io, b->ndocs);
	for (uint32 i = 0; i < b->ndocs; prev = DOC_KEY(b->docs[i++]))
	{
		io_varint(&io, DOC_KEY(b->docs[i]) - prev);
		io_varint(&io, DOC_DL(b->docs[i]));
	}
	for (uint32 i = 0; i < b->nterms; i++)
	{
		const InvTerm *t = &b->terms[order[i].idx];
		TermIter	it;
		uint32		ords[CHUNK];
		uint8		tf[CHUNK];

		io_varint(&io, t->len);
		io_put(&io, b->tbytes + t->term_off, t->len);
		io_varint(&io, t->df);
		prev = 0;
		term_iter(b, t, &it);
		for (int n; (n = term_chunk(b, &it, ords, tf)) > 0;)
			for (int k = 0; k < n; k++)
			{
				uint64		d = b->docs[ords[k]];

				io_varint(&io, (DOC_KEY(d) - prev) << 1 | (tf[k] > 1));
				if (tf[k] > 1)
					io_put(&io, &tf[k], 1);
				io_varint(&io, DOC_DL(d));
				prev = DOC_KEY(d);
			}
		if (i % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
	io_varint(&io, 0);
	io_flush(&io);
	MemoryContextSwitchTo(old);
	inv_reset(b);
	return run;
}

SiftRun *
sift_inv_spill(SiftInvBuf *b)
{
	MemoryContext old = MemoryContextSwitchTo(b->parent);
	BufFile    *file = BufFileCreateTemp(false);

	MemoryContextSwitchTo(old);
	return spill_to(b, file);
}

/* A run another process merges: a named file of the fileset, which the caller exports when done */
SiftRun *
sift_inv_spill_shared(SiftInvBuf *b, SharedFileSet *fileset, const char *name)
{
	MemoryContext old = MemoryContextSwitchTo(b->parent);
	BufFile    *file = BufFileCreateFileSet(&fileset->fs, name);

	MemoryContextSwitchTo(old);
	return spill_to(b, file);
}

uint64
sift_run_maxdf(const SiftRun *run)
{
	return run->maxdf;
}

static void
reader_next_term(RunReader *rd)
{
	rd->len = io_get_varint(&rd->io);
	if (rd->len == 0)
		return;
	if (rd->len > SIFT_MAX_TERM_BYTES)
		elog(ERROR, "sift: corrupt build run (term of %d bytes)", rd->len);
	for (int i = 0; i < rd->len; i++)
		rd->term[i] = io_byte(&rd->io);
	rd->df = io_get_varint(&rd->io);
}

static int
reader_cmp(Datum a, Datum b, void *arg)
{
	const RunReader *x = (const RunReader *) DatumGetPointer(a),
			   *y = (const RunReader *) DatumGetPointer(b);
	int			c = term_cmp(x->term, x->len, y->term, y->len);

	/* binaryheap keeps the largest on top */
	return c ? -c : y->idx - x->idx;
}

static int
doc_heap_cmp(Datum a, Datum b, void *arg)
{
	const RunReader *x = (const RunReader *) DatumGetPointer(a),
			   *y = (const RunReader *) DatumGetPointer(b);

	return pg_cmp_u64(y->dkey, x->dkey);
}

/* Moves to the next doc of the run; false when it has none */
static bool
reader_next_doc(RunReader *rd)
{
	if (rd->dleft == 0)
		return false;
	rd->dkey += io_get_varint(&rd->io);
	rd->ddl = io_get_varint(&rd->io);
	rd->dleft--;
	return true;
}

/* All runs' docs in key order (the runs' key ranges may interleave) */
static void
merge_docs(SiftSegmentWriter *sw, RunReader *rd, int nruns)
{
	binaryheap *heap = binaryheap_allocate(nruns, doc_heap_cmp, NULL);
	SiftKey		keys[BATCH];
	uint16		dl[BATCH];
	int			n = 0;

	for (int i = 0; i < nruns; i++)
		if (reader_next_doc(&rd[i]))
			binaryheap_add_unordered(heap, PointerGetDatum(&rd[i]));
	binaryheap_build(heap);
	while (!binaryheap_empty(heap))
	{
		RunReader  *top = (RunReader *) DatumGetPointer(binaryheap_first(heap));

		keys[n] = top->dkey;
		dl[n++] = top->ddl;
		if (reader_next_doc(top))
			binaryheap_replace_first(heap, PointerGetDatum(top));
		else
			binaryheap_remove_first(heap);
		if (n == BATCH)
		{
			sift_segw_add_docs(sw, keys, dl, n);
			n = 0;
			CHECK_FOR_INTERRUPTS();
		}
	}
	if (n > 0)
		sift_segw_add_docs(sw, keys, dl, n);
	binaryheap_free(heap);
}

/* A run's postings of the current term */
static bool
post_next(RunReader *rd)
{
	uint64		v;

	if (rd->pleft == 0)
		return false;
	v = io_get_varint(&rd->io);
	rd->ptf = (v & 1) ? io_byte(&rd->io) : 1;
	rd->pkey += v >> 1;
	rd->pdl = io_get_varint(&rd->io);
	rd->pleft--;
	return true;
}

/*
 * Postings of the n runs positioned on one term in key order, in batches;
 * act is scratch for n runs. A run drains while its key stays below the
 * others', so runs with disjoint ranges cost one pass each.
 */
static void
merge_postings(Batch *bt, RunReader **cur, int n, RunReader **act)
{
	int			live = 0;

	for (int i = 0; i < n; i++)
	{
		cur[i]->pleft = cur[i]->df;
		cur[i]->pkey = 0;
		if (post_next(cur[i]))
			act[live++] = cur[i];
	}
	while (live > 0)
	{
		int			m = 0;
		SiftKey		limit = PG_UINT64_MAX;
		RunReader  *r;

		for (int i = 1; i < live; i++)
			if (act[i]->pkey < act[m]->pkey)
				m = i;
		for (int i = 0; i < live; i++)
			if (i != m)
				limit = Min(limit, act[i]->pkey);
		r = act[m];
		for (;;)
		{
			batch_put(bt, r->pkey, r->ptf, r->pdl);
			if (!post_next(r))
			{
				act[m] = act[--live];
				break;
			}
			if (r->pkey >= limit)
				break;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

void
sift_runs_write(SiftRun **runs, int nruns, SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref,
				SiftSegEntry *entry)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftSegmentWriter *sw = sift_segw_begin(wt, gen, ref, cxt);
	RunReader  *rd = palloc0(nruns * sizeof(RunReader));
	RunReader **cur = palloc(nruns * sizeof(RunReader *));
	RunReader **act = palloc(nruns * sizeof(RunReader *));
	binaryheap *heap = binaryheap_allocate(nruns, reader_cmp, NULL);
	Batch	   *bt = palloc(sizeof(Batch));
	uint32		nterms = 0;

	for (int i = 0; i < nruns; i++)
	{
		rd[i].idx = i;
		rd[i].io.file = runs[i]->file;
		rd[i].io.buf = palloc(IO_BUF);
		if (BufFileSeek(runs[i]->file, 0, 0, SEEK_SET) != 0)
			elog(ERROR, "sift: could not rewind a build run");
		rd[i].dleft = io_get_varint(&rd[i].io);
	}
	merge_docs(sw, rd, nruns);
	for (int i = 0; i < nruns; i++)
	{
		reader_next_term(&rd[i]);
		if (rd[i].len)
			binaryheap_add_unordered(heap, PointerGetDatum(&rd[i]));
	}
	binaryheap_build(heap);
	while (!binaryheap_empty(heap))
	{
		char		term[SIFT_MAX_TERM_BYTES];
		int			len = ((RunReader *) DatumGetPointer(binaryheap_first(heap)))->len,
					k = 0;

		memcpy(term, ((RunReader *) DatumGetPointer(binaryheap_first(heap)))->term, len);
		bt->pw = sift_segw_begin_term(sw, term, len);
		bt->n = 0;
		while (!binaryheap_empty(heap))
		{
			RunReader  *top = (RunReader *) DatumGetPointer(binaryheap_first(heap));

			if (term_cmp(top->term, top->len, term, len) != 0)
				break;
			cur[k++] = top;
			binaryheap_remove_first(heap);
		}
		merge_postings(bt, cur, k, act);
		batch_flush(bt);
		sift_segw_end_term(sw);
		for (int i = 0; i < k; i++)
		{
			reader_next_term(cur[i]);
			if (cur[i]->len)
				binaryheap_add(heap, PointerGetDatum(cur[i]));
		}
		if (++nterms % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
	sift_segw_finish(sw, entry);
	for (int i = 0; i < nruns; i++)
		BufFileClose(runs[i]->file);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/* ---------- runs of segments (merge input) ---------- */

typedef struct DumpState
{
	RunIO		io;
	SiftKey		prev;
	uint64		ndocs;
	uint64		maxdf;
} DumpState;

static void
dump_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
	DumpState  *ds = arg;

	for (int i = 0; i < n; ds->prev = keys[i++])
	{
		io_varint(&ds->io, keys[i] - ds->prev);
		io_varint(&ds->io, dl[i]);
	}
	ds->ndocs -= n;
}

static bool
dump_term(void *arg, const char *term, int len, const SiftTermInfo *info, const SiftKey *keys,
		  const uint8 *tf, const uint16 *dl)
{
	DumpState  *ds = arg;
	SiftKey		prev = 0;

	io_varint(&ds->io, len);
	io_put(&ds->io, term, len);
	io_varint(&ds->io, info->df);
	for (uint32 j = 0; j < info->df; prev = keys[j++])
	{
		io_varint(&ds->io, (keys[j] - prev) << 1 | (tf[j] > 1));
		if (tf[j] > 1)
			io_put(&ds->io, &tf[j], 1);
		io_varint(&ds->io, dl[j]);
	}
	ds->maxdf = Max(ds->maxdf, info->df);
	return true;
}

SiftRun *
sift_run_from_segment(SiftSegment *seg, const SiftScoreRef *ref, MemoryContext mcxt)
{
	uint64		ndocs = sift_seg_live_docs(seg);
	SiftRun    *run;
	DumpState	ds = {.ndocs = ndocs};
	MemoryContext old;

	if (ndocs == 0)
		return NULL;
	old = MemoryContextSwitchTo(mcxt);
	run = palloc0(sizeof(SiftRun));
	run->file = ds.io.file = BufFileCreateTemp(false);
	ds.io.buf = palloc(IO_BUF);
	run->ndocs = ndocs;
	io_varint(&ds.io, ndocs);
	sift_seg_dump(seg, ref, dump_docs, dump_term, &ds);
	if (ds.ndocs != 0)
		elog(ERROR, "sift: segment %llu lists %llu live docs but streamed another number", ULL(seg->entry.gen), ULL(ndocs));
	io_varint(&ds.io, 0);
	io_flush(&ds.io);
	pfree(ds.io.buf);
	run->maxdf = ds.maxdf;
	MemoryContextSwitchTo(old);
	return run;
}

/* ---------- ambuild ---------- */

typedef struct PageDoc
{
	ItemPointerData tid;
	uint32		off;			/* into the page's text arena */
	uint32		len;
} PageDoc;

typedef struct BuildState
{
	Relation	index;
	MemoryContext cxt;
	MemoryContext tmpcxt;
	SiftAnalyzer *an;
	SiftInvBuf *inv;
	Size		budget;
	SiftWriteTarget *wt;
	SiftScoreRef ref;
	/* docs of the current heap page, sorted by offset before analysis */
	BlockNumber page;
	PageDoc    *docs;
	int			ndocs;
	int			maxdocs;
	StringInfoData text;
	/* forced segment boundaries (build_segments) */
	BlockNumber *bounds;
	int			nbounds;
	int			nextbound;
	/* runs of the current segment */
	SiftRun   **runs;
	int			nruns;
	int			maxruns;
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
	int			nsegs;
	double		indexed;
	uint32		analyzer_hash;
	/* parallel worker: runs go to this fileset, named after the worker and the run */
	SharedFileSet *fileset;
	int			worker;
} BuildState;

/* Runs a worker may hand over: a run holds at least its budget's share of postings */
#define MAX_WORKER_RUNS	256

static const char *
run_file(int worker, int run)
{
	static char name[32];

	snprintf(name, sizeof(name), "sift-run-%d-%d", worker, run);
	return name;
}

/* Reserves the next directory entry; its generation is the entry's 1-based position */
static SiftSegEntry *
next_entry(BuildState *bs, uint64 *gen)
{
	if (bs->nsegs == SIFT_MAX_SEGMENTS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index \"%s\" would need more than %d segments",
						RelationGetRelationName(bs->index), SIFT_MAX_SEGMENTS),
				 errhint("Increase maintenance_work_mem or lower build_segments.")));
	*gen = bs->nsegs + 1;
	return &bs->segs[bs->nsegs++];
}

static void
spill(BuildState *bs)
{
	if (bs->nruns + 1 >= bs->maxruns)
	{
		bs->maxruns *= 2;
		bs->runs = repalloc(bs->runs, bs->maxruns * sizeof(SiftRun *));
	}
	if (bs->fileset == NULL)
		bs->runs[bs->nruns] = sift_inv_spill(bs->inv);
	else if (bs->nruns == MAX_WORKER_RUNS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift parallel build worker needs more than %d runs", MAX_WORKER_RUNS),
				 errhint("Increase maintenance_work_mem or build without parallel workers.")));
	else
		bs->runs[bs->nruns] = sift_inv_spill_shared(bs->inv, bs->fileset, run_file(bs->worker, bs->nruns));
	bs->nruns++;
}

/* Writes the docs gathered since the last boundary as one or more segments */
static void
end_segment(BuildState *bs)
{
	int			first = 0;
	uint64		gen;
	SiftSegEntry *entry;

	if (bs->nruns == 0)
	{
		if (!sift_inv_empty(bs->inv))
		{
			entry = next_entry(bs, &gen);
			sift_inv_write(bs->inv, bs->wt, gen, &bs->ref, entry);
		}
		return;
	}
	if (!sift_inv_empty(bs->inv))
		spill(bs);
	/* each segment merges runs whose largest terms fit the budget together */
	while (first < bs->nruns)
	{
		int			last = first + 1;
		uint64		postings = sift_run_maxdf(bs->runs[first]);

		while (last < bs->nruns && (postings + sift_run_maxdf(bs->runs[last])) * 10 <= bs->budget)
			postings += sift_run_maxdf(bs->runs[last++]);
		entry = next_entry(bs, &gen);
		sift_runs_write(bs->runs + first, last - first, bs->wt, gen, &bs->ref, entry);
		first = last;
	}
	bs->nruns = 0;
}

static int
pagedoc_cmp(const void *a, const void *b)
{
	return ItemPointerCompare(&((PageDoc *) a)->tid, &((PageDoc *) b)->tid);
}

/* Analyzes the buffered docs of one heap page in tid order */
static void
flush_page(BuildState *bs)
{
	qsort(bs->docs, bs->ndocs, sizeof(PageDoc), pagedoc_cmp);
	for (int i = 0; i < bs->ndocs; i++)
	{
		SiftTermFreq *terms;
		uint32		dl;
		int			n = sift_analyze_doc(bs->an, bs->text.data + bs->docs[i].off, bs->docs[i].len, &terms, &dl);

		sift_inv_add(bs->inv, &bs->docs[i].tid, terms, n, dl);
		bs->indexed++;
		if (sift_inv_full(bs->inv))
			spill(bs);
	}
	bs->ndocs = 0;
	resetStringInfo(&bs->text);
	sift_analyzer_reset(bs->an);
	CHECK_FOR_INTERRUPTS();
}

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	BuildState *bs = state;
	MemoryContext old;
	text	   *t;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);

	if (isnull[0])
		return;
	if (blk != bs->page)
	{
		flush_page(bs);
		bs->page = blk;
		for (; bs->nextbound < bs->nbounds && blk >= bs->bounds[bs->nextbound]; bs->nextbound++)
			end_segment(bs);
	}
	old = MemoryContextSwitchTo(bs->tmpcxt);
	t = DatumGetTextPP(values[0]);
	if (bs->ndocs == bs->maxdocs)
	{
		bs->maxdocs *= 2;
		bs->docs = repalloc(bs->docs, bs->maxdocs * sizeof(PageDoc));
	}
	bs->docs[bs->ndocs++] = (PageDoc) {*tid, bs->text.len, VARSIZE_ANY_EXHDR(t)};
	appendBinaryStringInfo(&bs->text, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	MemoryContextSwitchTo(old);
	MemoryContextReset(bs->tmpcxt);
}

static void
publish(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	BuildState *bs = arg;

	meta->next_gen = bs->nsegs + 1;
	memcpy(dir->segs, bs->segs, bs->nsegs * sizeof(SiftSegEntry));
	dir->nsegs = bs->nsegs;
	for (int i = 0; i < bs->nsegs; i++)
	{
		meta->ndocs += bs->segs[i].ndocs;
		meta->sum_dl += bs->segs[i].sum_dl;
	}
}

static void
init_bounds(BuildState *bs, Relation heap, int nsegments)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);

	bs->bounds = palloc(Max(nsegments, 1) * sizeof(BlockNumber));
	for (int i = 1; i < nsegments; i++)
	{
		BlockNumber b = (BlockNumber) ((uint64) nblocks * i / nsegments);

		if (b > 0 && (bs->nbounds == 0 || b > bs->bounds[bs->nbounds - 1]))
			bs->bounds[bs->nbounds++] = b;
	}
}

/* Allocates the build state; the caller switches to bs->cxt for the scan */
static BuildState *
build_begin(Relation index, Size budget)
{
	const SiftOptions *opts = sift_index_options(index);
	BuildState *bs = palloc0(sizeof(BuildState));
	SiftAnalyzerConfig cfg;
	MemoryContext old;

	sift_analyzer_config_from_options(opts, &cfg);
	bs->index = index;
	bs->analyzer_hash = cfg.hash;
	bs->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift build", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(bs->cxt);
	bs->tmpcxt = AllocSetContextCreate(bs->cxt, "sift build tuple", ALLOCSET_DEFAULT_SIZES);
	bs->an = sift_analyzer_create(&cfg, bs->cxt);
	bs->budget = budget;
	bs->inv = sift_inv_create(bs->budget, bs->cxt);
	bs->ref = (SiftScoreRef) {(float4) opts->k1, (float4) opts->b, 0};
	bs->page = InvalidBlockNumber;
	bs->maxdocs = 64;
	bs->docs = palloc(bs->maxdocs * sizeof(PageDoc));
	initStringInfo(&bs->text);
	bs->maxruns = 16;
	bs->runs = palloc(bs->maxruns * sizeof(SiftRun *));
	MemoryContextSwitchTo(old);
	return bs;
}

/* Scans the heap, or this participant's share of a parallel scan, into the inverted buffer and its runs */
static double
build_scan(BuildState *bs, Relation heap, IndexInfo *info, TableScanDesc scan, bool progress)
{
	double		reltuples = table_index_build_scan(heap, bs->index, info, scan != NULL, progress, build_cb, bs, scan);

	flush_page(bs);
	return reltuples;
}

/* ---------- parallel ambuild ---------- */

/*
 * Every participant scans its share of a parallel heap scan (ascending keys,
 * interleaved with the others') into sorted runs. The workers' runs are files
 * of a shared fileset; the leader merges them with its own into the segments
 * a serial build would write.
 */
#define KEY_SHARED		UINT64CONST(0x5100000000000001)
#define KEY_PARTS		UINT64CONST(0x5100000000000002)
#define KEY_QUERY_TEXT	UINT64CONST(0x5100000000000003)
#define KEY_WAL_USAGE	UINT64CONST(0x5100000000000004)
#define KEY_BUFFER_USAGE UINT64CONST(0x5100000000000005)
#define SCAN_OF(shared)	((ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(SiftBuildShared))))

typedef struct SiftBuildShared
{
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			nparticipants;
	Size		budget;				/* per participant */
	SharedFileSet fileset;
	slock_t		mutex;
	double		reltuples;
	double		indexed;
	/* the parallel table scan descriptor follows */
} SiftBuildShared;

/* A worker's runs, in the files run_file(worker number, i) */
typedef struct SiftBuildPart
{
	int			nruns;
	struct
	{
		uint64		ndocs;
		uint64		maxdf;
	}			run[MAX_WORKER_RUNS];
} SiftBuildPart;

typedef struct SiftBuildLeader
{
	ParallelContext *pcxt;
	SiftBuildShared *shared;
	SiftBuildPart *parts;
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} SiftBuildLeader;

static void
end_parallel(SiftBuildLeader *ldr)
{
	DestroyParallelContext(ldr->pcxt);
	ExitParallelMode();
	if (IsMVCCSnapshot(ldr->snapshot))
		UnregisterSnapshot(ldr->snapshot);
}

/*
 * A synchronized scan starts mid-heap and wraps around, but every participant needs ascending keys.
 * The scan decides at initialization, so the setting only matters here.
 */
static void
init_scan(Relation heap, SiftBuildShared *shared, Snapshot snapshot)
{
	int			nest = NewGUCNestLevel();

	set_config_option("synchronize_seqscans", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	table_parallelscan_initialize(heap, SCAN_OF(shared), snapshot);
	AtEOXact_GUC(false, nest);
}

/* NULL when no worker could be started: the leader then builds alone */
static SiftBuildLeader *
begin_parallel(Relation heap, Relation index, IndexInfo *info)
{
	int			nworkers = info->ii_ParallelWorkers;
	SiftBuildLeader *ldr = palloc0(sizeof(SiftBuildLeader));
	SiftBuildShared *shared;
	ParallelContext *pcxt;
	Size		shared_size;
	int			nkeys = 4;

	ldr->snapshot = info->ii_Concurrent ? RegisterSnapshot(GetTransactionSnapshot()) : SnapshotAny;
	shared_size = add_size(BUFFERALIGN(sizeof(SiftBuildShared)), table_parallelscan_estimate(heap, ldr->snapshot));
	EnterParallelMode();
	ldr->pcxt = pcxt = CreateParallelContext("pg_sift", "sift_build_worker", nworkers);
	shm_toc_estimate_chunk(&pcxt->estimator, shared_size);
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(SiftBuildPart), nworkers));
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(WalUsage), nworkers));
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(BufferUsage), nworkers));
	if (debug_query_string)
	{
		shm_toc_estimate_chunk(&pcxt->estimator, strlen(debug_query_string) + 1);
		nkeys++;
	}
	shm_toc_estimate_keys(&pcxt->estimator, nkeys);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		end_parallel(ldr);
		return NULL;
	}
	ldr->shared = shared = shm_toc_allocate(pcxt->toc, shared_size);
	*shared = (SiftBuildShared) {.heaprelid = RelationGetRelid(heap), .indexrelid = RelationGetRelid(index),
	.isconcurrent = info->ii_Concurrent,.nparticipants = nworkers + 1,
	.budget = (Size) maintenance_work_mem * 1024 / (nworkers + 1)};
	SharedFileSetInit(&shared->fileset, pcxt->seg);
	SpinLockInit(&shared->mutex);
	init_scan(heap, shared, ldr->snapshot);
	ldr->parts = shm_toc_allocate(pcxt->toc, mul_size(sizeof(SiftBuildPart), nworkers));
	ldr->walusage = shm_toc_allocate(pcxt->toc, mul_size(sizeof(WalUsage), nworkers));
	ldr->bufferusage = shm_toc_allocate(pcxt->toc, mul_size(sizeof(BufferUsage), nworkers));
	shm_toc_insert(pcxt->toc, KEY_SHARED, shared);
	shm_toc_insert(pcxt->toc, KEY_PARTS, ldr->parts);
	shm_toc_insert(pcxt->toc, KEY_WAL_USAGE, ldr->walusage);
	shm_toc_insert(pcxt->toc, KEY_BUFFER_USAGE, ldr->bufferusage);
	if (debug_query_string)
	{
		char	   *query = shm_toc_allocate(pcxt->toc, strlen(debug_query_string) + 1);

		strcpy(query, debug_query_string);
		shm_toc_insert(pcxt->toc, KEY_QUERY_TEXT, query);
	}
	LaunchParallelWorkers(pcxt);
	if (pcxt->nworkers_launched == 0)
	{
		end_parallel(ldr);
		return NULL;
	}
	WaitForParallelWorkersToAttach(pcxt);
	ereport(DEBUG1, errmsg("building sift index \"%s\" with %d parallel workers",
						   RelationGetRelationName(index), pcxt->nworkers_launched));
	return ldr;
}

/* Waits for the workers and adds their runs to the leader's; returns the tuple counts through res */
static void
adopt_runs(SiftBuildLeader *ldr, BuildState *bs, IndexBuildResult *res)
{
	SiftBuildShared *shared = ldr->shared;

	WaitForParallelWorkersToFinish(ldr->pcxt);
	for (int w = 0; w < ldr->pcxt->nworkers_launched; w++)
	{
		for (int i = 0; i < ldr->parts[w].nruns; i++)
		{
			SiftRun    *run = palloc(sizeof(SiftRun));

			if (bs->nruns == bs->maxruns)
			{
				bs->maxruns *= 2;
				bs->runs = repalloc(bs->runs, bs->maxruns * sizeof(SiftRun *));
			}
			*run = (SiftRun) {BufFileOpenFileSet(&shared->fileset.fs, run_file(w, i), O_RDONLY, false),
				ldr->parts[w].run[i].ndocs, ldr->parts[w].run[i].maxdf};
			bs->runs[bs->nruns++] = run;
		}
		InstrAccumParallelQuery(&ldr->bufferusage[w], &ldr->walusage[w]);
	}
	res->heap_tuples = shared->reltuples;
	res->index_tuples = shared->indexed;
}

static void
add_counts(SiftBuildShared *shared, double reltuples, double indexed)
{
	SpinLockAcquire(&shared->mutex);
	shared->reltuples += reltuples;
	shared->indexed += indexed;
	SpinLockRelease(&shared->mutex);
}

/* Spills what is left and makes the runs readable by the leader */
static void
hand_over_runs(BuildState *bs, SiftBuildPart *part)
{
	if (!sift_inv_empty(bs->inv))
		spill(bs);
	part->nruns = bs->nruns;
	for (int i = 0; i < bs->nruns; i++)
	{
		part->run[i].ndocs = bs->runs[i]->ndocs;
		part->run[i].maxdf = bs->runs[i]->maxdf;
		BufFileExportFileSet(bs->runs[i]->file);
		BufFileClose(bs->runs[i]->file);
	}
}

PGDLLEXPORT void sift_build_worker(dsm_segment *seg, shm_toc *toc);

void
sift_build_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftBuildShared *shared = shm_toc_lookup(toc, KEY_SHARED, false);
	SiftBuildPart *part = (SiftBuildPart *) shm_toc_lookup(toc, KEY_PARTS, false) + ParallelWorkerNumber;
	LOCKMODE	heaplock = shared->isconcurrent ? ShareUpdateExclusiveLock : ShareLock;
	LOCKMODE	indexlock = shared->isconcurrent ? RowExclusiveLock : AccessExclusiveLock;
	Relation	heap,
				index;
	IndexInfo  *info;
	BuildState *bs;
	double		reltuples;

	debug_query_string = shm_toc_lookup(toc, KEY_QUERY_TEXT, true);
	pgstat_report_activity(STATE_RUNNING, debug_query_string);
	heap = table_open(shared->heaprelid, heaplock);
	index = index_open(shared->indexrelid, indexlock);
	info = BuildIndexInfo(index);
	info->ii_Concurrent = shared->isconcurrent;
	SharedFileSetAttach(&shared->fileset, seg);
	InstrStartParallelQuery();
	bs = build_begin(index, shared->budget);
	bs->fileset = &shared->fileset;
	bs->worker = ParallelWorkerNumber;
	reltuples = build_scan(bs, heap, info, table_beginscan_parallel(heap, SCAN_OF(shared)), false);
	hand_over_runs(bs, part);
	add_counts(shared, reltuples, bs->indexed);
	InstrEndParallelQuery(&((BufferUsage *) shm_toc_lookup(toc, KEY_BUFFER_USAGE, false))[ParallelWorkerNumber],
						  &((WalUsage *) shm_toc_lookup(toc, KEY_WAL_USAGE, false))[ParallelWorkerNumber]);
	MemoryContextDelete(bs->cxt);
	index_close(index, indexlock);
	table_close(heap, heaplock);
}

IndexBuildResult *
sift_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	const SiftOptions *opts = sift_index_options(index);
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	SiftBuildLeader *ldr = NULL;
	BuildState *bs;
	MemoryContext old;

	if (heap->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift indexes require a heap table")));
	if (opts->build_segments == 0 && info->ii_ParallelWorkers > 0)
		ldr = begin_parallel(heap, index, info);
	bs = build_begin(index, (Size) maintenance_work_mem * 1024 / (ldr ? ldr->shared->nparticipants : 1));
	old = MemoryContextSwitchTo(bs->cxt);
	if (ldr == NULL)
		init_bounds(bs, heap, opts->build_segments);
	bs->wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	res->heap_tuples = build_scan(bs, heap, info, ldr ? table_beginscan_parallel(heap, SCAN_OF(ldr->shared)) : NULL, true);
	res->index_tuples = bs->indexed;
	if (ldr)
	{
		add_counts(ldr->shared, res->heap_tuples, bs->indexed);
		adopt_runs(ldr, bs, res);
		bs->budget = (Size) maintenance_work_mem * 1024;
	}
	end_segment(bs);
	if (ldr)
		end_parallel(ldr);
	sift_store_init_meta(bs->wt, opts, bs->analyzer_hash);
	sift_wt_finish(bs->wt);
	if (bs->nsegs > 0)
		sift_dir_update(index, &(SiftDirChange) {.fn = publish,.arg = bs,.extra = InvalidBuffer});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(bs->cxt);
	return res;
}

void
sift_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);
	SiftAnalyzerConfig cfg;

	sift_analyzer_config_from_index(index, &cfg);
	sift_store_init_meta(wt, sift_index_options(index), cfg.hash);
	sift_wt_finish(wt);
}
