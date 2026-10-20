/*
 * bm25/inv.c - the inverted buffer: docs and posting slices in memory, written as a segment or spilled to a sorted run (bm25/runs.h).
 */
#include "core/host.h"

#include "core/cmp.h"
#include "bm25/runs.h"
#include "bm25/runs_int.h"

#define POOL_SHIFT		16
#define POOL_BLOCK		(1 << POOL_SHIFT)
#define POOL_MAX_BLOCKS	60000		/* addresses stay below 2^32 */
#define POOL(b, a)		((b)->blocks[(a) >> POOL_SHIFT] + ((a) & (POOL_BLOCK - 1)))
#define NLEVELS			9
#define DOC_KEY(d)		((d) >> 16)
#define DOC_DL(d)		((uint16) (d))

static const uint16 slice_size[NLEVELS] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};

/* A term's postings: varint(delta << 1 | tf > 1) [tf] [position record] over doc ordinals, in pool slices */
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
	bool		haspos;			/* a position record follows each posting's tf */
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
	bool		term_full;		/* a term has maxdf_full postings */
	uint32		maxdf_full;		/* the buffer is full when a term has this many postings */
	uint32		pool_room;		/* blocks of the pool the buffer leaves free for the doc it is about to take */
	Size		small_doc;		/* a doc of fewer bytes of text always fits those blocks */
	uint32	   *doc_terms;		/* term indexes of the doc being added */
	int			maxdoc_terms;
	bool		positions;		/* docs carry position records */
};

static uint32
pool_max(void)
{
	return TEST_KNOB(sift_test_pool_blocks, POOL_MAX_BLOCKS);
}

/* A leaf run holds a term up to half of a segment's limit, but at least this many postings (small test limits) */
#define MIN_RUN_POSTINGS	64

/* Bytes of the pool a doc takes for each byte of its text, at most (3 for a position, 1024 for a term of every 2 bytes) */
#define SMALL_DOC_BYTES	1100
typedef struct SliceReader
{
	uint32		cur;
	uint32		end;
	uint8		level;
} SliceReader;

/* Position records of a chunk of postings of one term, back to back */
typedef struct PosChunk
{
	uint8	   *buf;
	Size		len;
	Size		cap;
	uint32		plen[64];		/* CHUNK */
} PosChunk;

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
	b->term_full = false;
	b->doc_terms = NULL;
	b->maxdoc_terms = 0;
	MemoryContextSwitchTo(old);
}

SiftInvBuf *
sift_inv_create(Size budget, MemoryContext mcxt, bool positions)
{
	SiftInvBuf *b = MemoryContextAllocZero(mcxt, sizeof(SiftInvBuf));

	b->positions = positions;
	b->parent = mcxt;
	b->budget = Max(budget, 1024 * 1024);
	b->maxdf_full = Max(Min(Min(Max(sift_seg_postings(b->budget) / 2, MIN_RUN_POSTINGS), SIFT_BLOB_MAX / (4 * BLOB_POSTING_BYTES)),
							PG_INT32_MAX), 1);
	b->pool_room = Max(pool_max() / 32, 3);
	b->small_doc = (Size) (b->pool_room - 2) * POOL_BLOCK / SMALL_DOC_BYTES;
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

static inline bool
inv_full(const SiftInvBuf *b)
{
	return inv_mem(b) >= b->budget || b->nblocks + b->pool_room >= pool_max() ||
		b->ndocs >= TEST_KNOB(sift_test_run_docs, PG_INT32_MAX / 2) ||
		b->term_full || b->tlen + (Size) b->nterms * DICT_ENTRY_BYTES >= SIFT_DICT_MAX / 4;
}

bool
sift_inv_full(const SiftInvBuf *b)
{
	return inv_full(b);
}

/*
 * Can the buffer take a doc of nterms terms and textlen bytes of text? Its terms must not exhaust the pool, which the doc
 * is not stopped by once added: a term takes a slice of 8 bytes, or one of up to 2 kB when its slice is full, and each
 * position at most 5 bytes of a record. A buffer that is not full always has room for a doc of fewer than small_doc bytes
 * (a token takes 2 bytes of text at least), so only larger ones need the check.
 */
bool
sift_inv_fits(const SiftInvBuf *b, int nterms, Size textlen)
{
	return (Size) b->nblocks * POOL_BLOCK + (Size) nterms * 2048 + 3 * textlen + 2 * POOL_BLOCK <= (Size) pool_max() * POOL_BLOCK;
}

Size
sift_inv_small_doc(const SiftInvBuf *b)
{
	return b->small_doc;
}

/* Is tid's key above that of the last doc? Docs must be added in ascending order */
bool
sift_inv_after(const SiftInvBuf *b, ItemPointer tid)
{
	return b->ndocs == 0 || sift_key_from_tid(tid) > DOC_KEY(b->docs[b->ndocs - 1]);
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
		if (b->nblocks == pool_max())
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

static inline void
chunk_put(PosChunk *pc, uint8 c)
{
	if (pc->len == pc->cap)
	{
		pc->cap = Max(4096, pc->cap * 2);
		pc->buf = pc->buf ? repalloc_huge(pc->buf, pc->cap) : MemoryContextAllocHuge(CurrentMemoryContext, pc->cap);
	}
	pc->buf[pc->len++] = c;
}

/* Copies one varint of a slice stream to the chunk and returns its value */
static inline uint32
slice_varint(const SiftInvBuf *b, SliceReader *r, PosChunk *pc)
{
	uint32		x = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = slice_get(b, r);

		chunk_put(pc, c);
		x |= (uint32) (c & 0x7f) << s;
		if (!(c & 0x80))
			return x;
	}
}

/* Copies the position record at the reader to the chunk (see bm25/analyze.h); returns its length */
static uint32
slice_record(const SiftInvBuf *b, SliceReader *r, PosChunk *pc)
{
	Size		start = pc->len;
	uint32		v = slice_varint(b, r, pc);

	if (v & 1)
	{
		uint32		n = slice_varint(b, r, pc);

		for (uint32 i = 0; i <= n; i++)
			slice_varint(b, r, pc);
	}
	return pc->len - start;
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

bool
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
		if (t->df == 0)
			t->haspos = b->positions && terms[i].pos != NULL;
		else if (t->haspos != (b->positions && terms[i].pos != NULL))
			elog(ERROR, "sift: the postings of a term must all have positions or none");
		if (t->haspos)
			for (uint32 k = 0; k < terms[i].poslen; k++)
				slice_put(b, t, terms[i].pos[k]);
		t->last = ord;
		t->df++;
		if (t->df >= b->maxdf_full)
			b->term_full = true;
	}
	return inv_full(b);
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
	bool		haspos;
} TermIter;

#define CHUNK	64

static void
term_iter(const SiftInvBuf *b, const InvTerm *t, TermIter *it)
{
	it->r = (SliceReader) {t->head, t->head + slice_size[0] - sizeof(uint32), 0};
	it->ord = 0;
	it->left = t->df;
	it->haspos = t->haspos;
}

static int
term_chunk(const SiftInvBuf *b, TermIter *it, uint32 *ords, uint8 *tf, PosChunk *pc)
{
	int			n = Min(it->left, CHUNK);

	pc->len = 0;
	for (int i = 0; i < n; i++)
	{
		term_next(b, &it->r, &it->ord, &tf[i]);
		if (it->haspos)
			pc->plen[i] = slice_record(b, &it->r, pc);
		ords[i] = it->ord;
		__builtin_prefetch(&b->docs[it->ord]);
	}
	it->left -= n;
	return n;
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
	return sift_term_cmp(b->tbytes + ta->term_off, ta->len, b->tbytes + tc->term_off, tc->len);
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
	SiftSegmentWriter *sw = sift_segw_begin(wt, gen, ref, cxt, b->positions);
	SortTerm   *order = sorted_terms(b);
	Batch	   *bt = palloc0(sizeof(Batch));
	PosChunk   *pc = palloc0(sizeof(PosChunk));

	add_docs(sw, b->docs, b->ndocs);
	for (uint32 i = 0; i < b->nterms; i++)
	{
		const InvTerm *t = &b->terms[order[i].idx];
		TermIter	it;
		uint32		ords[CHUNK];
		uint8		tf[CHUNK];

		bt->pw = sift_segw_begin_term(sw, b->tbytes + t->term_off, t->len);
		bt->n = 0;
		bt->haspos = t->haspos;
		term_iter(b, t, &it);
		for (int n; (n = term_chunk(b, &it, ords, tf, pc)) > 0;)
		{
			Size		off = 0;

			for (int k = 0; k < n; off += t->haspos ? pc->plen[k] : 0, k++)
				batch_put(bt, DOC_KEY(b->docs[ords[k]]), tf[k], DOC_DL(b->docs[ords[k]]), pc->buf + off, pc->plen[k]);
		}
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

/*
 * Run file: varint ndocs, byte positions, (varint key delta, varint dl) per
 * doc; per term in byte order: varint len, bytes, varint df, byte (term has
 * position records), then per posting varint(key delta << 1 | tf > 1) [tf]
 * varint dl [position record]; varint 0 ends the run.
 */
static SiftRun *
spill_to(SiftInvBuf *b, SiftRun *run)
{
	MemoryContext old = MemoryContextSwitchTo(b->mcxt);
	RunIO		io = {.buf = palloc(IO_BUF),.file = run->file};
	SortTerm   *order = sorted_terms(b);
	SiftKey		prev = 0;
	PosChunk   *pc = palloc0(sizeof(PosChunk));
	uint64		maxblob = 0,
				maxdf = 0;

	run->ndocs = b->ndocs;
	io_varint(&io, b->ndocs);
	io_varint(&io, b->positions);
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
		uint64		posbytes = 0;

		io_varint(&io, t->len);
		io_put(&io, b->tbytes + t->term_off, t->len);
		io_varint(&io, t->df);
		io_varint(&io, t->haspos);
		prev = 0;
		term_iter(b, t, &it);
		for (int n; (n = term_chunk(b, &it, ords, tf, pc)) > 0;)
		{
			Size		off = 0;

			for (int k = 0; k < n; k++)
			{
				uint64		d = b->docs[ords[k]];

				io_varint(&io, (DOC_KEY(d) - prev) << 1 | (tf[k] > 1));
				if (tf[k] > 1)
					io_put(&io, &tf[k], 1);
				io_varint(&io, DOC_DL(d));
				if (t->haspos)
				{
					io_put(&io, pc->buf + off, pc->plen[k]);
					off += pc->plen[k];
				}
				prev = DOC_KEY(d);
			}
			posbytes += off;
		}
		maxdf = Max(maxdf, t->df);
		maxblob = Max(maxblob, t->df * BLOB_POSTING_BYTES + posbytes);
		if (i % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
	io_varint(&io, 0);
	io_flush(&io);
	run->maxdf = maxdf;
	run->maxblob = maxblob;
	run->dict = b->tlen + (uint64) b->nterms * DICT_ENTRY_BYTES;
	MemoryContextSwitchTo(old);
	inv_reset(b);
	return run;
}

SiftRun *
sift_inv_spill(SiftInvBuf *b)
{
	return spill_to(b, run_new(b->parent, NULL, 0, 0));
}

/* A run another process merges: the file sift-run-<worker>-<id> of the fileset, which the caller exports when done */
SiftRun *
sift_inv_spill_shared(SiftInvBuf *b, SharedFileSet *fileset, int worker, int id)
{
	return spill_to(b, run_new(b->parent, fileset, worker, id));
}
