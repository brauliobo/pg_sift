/*
 * bm25/runs.c - the inverted buffer, the sorted runs and their merge into segments (bm25/runs.h). Portable: a run is a
 * BufFile, the merge a binary heap over their readers, the output the segment writer.
 */
#include "sift_host.h"

#include "sift_cmp.h"
#include "bm25/runs.h"

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

#ifdef SIFT_TESTING
int			sift_test_run_docs = 0;
int			sift_test_fanin = 0;
int			sift_test_segment_postings = 0;
int			sift_test_pool_blocks = 0;
void		(*sift_test_merge_term) (uint32 nterms, uint64 df) = NULL;
#endif

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

/*
 * What a term takes of a segment, at most: a posting 8 bytes of its blob besides its position record, a
 * dictionary entry 32 bytes besides the term (lengths, df, flags, bound; front coding is ignored).
 */
#define BLOB_POSTING_BYTES	8
#define DICT_ENTRY_BYTES	32

/* The term of a run: len bytes, df postings and posbytes bytes of position records */
static void
run_note_term(SiftRun *run, int len, uint64 df, uint64 posbytes)
{
	run->maxdf = Max(run->maxdf, df);
	run->maxblob = Max(run->maxblob, df * BLOB_POSTING_BYTES + posbytes);
	run->dict += len + DICT_ENTRY_BYTES;
}

/* Postings of the largest term of a segment that fit its memory: the posting writer takes about 10 bytes of each */
uint64
sift_seg_postings(Size budget)
{
	return TEST_KNOB(sift_test_segment_postings, budget / 10);
}

const char *
sift_run_file(int worker, int run)
{
	static char name[32];

	snprintf(name, sizeof(name), "sift-run-%d-%d", worker, run);
	return name;
}

/* An empty run with its file open for writing (a file of the fileset when there is one) */
static SiftRun *
run_new(MemoryContext mcxt, SharedFileSet *fileset, int worker, int id)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftRun    *run = palloc0(sizeof(SiftRun));

	run->fileset = fileset;
	run->worker = worker;
	run->id = id;
	run->file = fileset ? BufFileCreateFileSet(&fileset->fs, sift_run_file(worker, id)) : BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	return run;
}

/* The file of a run another process wrote: opened when the run is merged, not while it waits */
static BufFile *
run_open(SiftRun *run)
{
	if (run->file == NULL)
		run->file = BufFileOpenFileSet(&run->fileset->fs, sift_run_file(run->worker, run->id), O_RDONLY, false);
	return run->file;
}

/* Closes a merged run; a fileset's file goes too, as the leader or the worker has no more use for it */
static void
run_close(SiftRun *run)
{
	BufFileClose(run->file);
	if (run->fileset)
		BufFileDeleteFileSet(&run->fileset->fs, sift_run_file(run->worker, run->id), false);
}

/* Runs one merge reads at a time: a reader each (the smallest has two blocks), within half the budget */
int
sift_merge_fanin(Size budget)
{
	return Max(2, TEST_KNOB(sift_test_fanin, Min(MAX_MERGE_FANIN, Max(8, budget / 2 / (2 * BLCKSZ)))));
}

/* Read buffer of each of nruns readers: what half the budget leaves after their file buffers, at most IO_BUF */
static int
reader_buf(Size budget, int nruns)
{
	return Min(IO_BUF, Max(BLCKSZ, (int) Min(budget / 2 / nruns, INT_MAX) - BLCKSZ));
}

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
	int			cap;			/* of buf when reading */
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
	bool		haspos;			/* the run holds position records */
	bool		thaspos;		/* ... for the current term */
	/* docs */
	uint64		dleft;
	SiftKey		dkey;
	uint16		ddl;
	/* postings of the current term */
	uint64		pleft;
	SiftKey		pkey;
	uint8		ptf;
	uint16		pdl;
	uint8	   *prec;			/* position record of the current posting */
	uint32		prlen;
	Size		prcap;
} RunReader;

/* Feeds postings of one term to the posting writer in batches */
typedef struct Batch
{
	SiftPostingWriter *pw;
	RunIO	   *run;			/* a run (see spill_to) to write the postings to, instead of pw */
	SiftKey		prev;			/* ... the key of its last posting */
	int			n;
	SiftKey		keys[BATCH];
	uint8		tf[BATCH];
	uint16		dl[BATCH];
	bool		haspos;			/* the term's postings carry position records */
	uint64		termpos;		/* bytes of the position records of the term's postings so far */
	uint8	   *pos;			/* of the n postings, back to back */
	Size		poslen;
	Size		poscap;
	uint32		plen[BATCH];
} Batch;

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

static void batch_to_run(Batch *bt);

static void
batch_flush(Batch *bt)
{
	if (bt->n && bt->run)
		batch_to_run(bt);
	else if (bt->n)
		sift_postw_add(bt->pw, bt->keys, bt->tf, bt->dl, bt->n, bt->haspos ? bt->pos : NULL, bt->plen);
	bt->n = 0;
	bt->termpos += bt->poslen;
	bt->poslen = 0;
}

/* rec / len: the posting's position record, ignored unless the term has positions */
static void
batch_put(Batch *bt, SiftKey key, uint8 tf, uint16 dl, const uint8 *rec, uint32 len)
{
	bt->keys[bt->n] = key;
	bt->tf[bt->n] = tf;
	bt->dl[bt->n] = dl;
	if (bt->haspos)
	{
		if (bt->poslen + len > bt->poscap)
		{
			bt->poscap = Max(bt->poslen + len, Max(8192, bt->poscap * 2));
			bt->pos = bt->pos ? repalloc_huge(bt->pos, bt->poscap) : MemoryContextAllocHuge(CurrentMemoryContext, bt->poscap);
		}
		memcpy(bt->pos + bt->poslen, rec, len);
		bt->plen[bt->n] = len;
		bt->poslen += len;
	}
	if (++bt->n == BATCH)
		batch_flush(bt);
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
	if (n > IO_BUF)
	{
		BufFileWrite(io->file, p, n);
		return;
	}
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
		io->len = BufFileRead(io->file, io->buf, io->cap);
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
	rd->thaspos = io_get_varint(&rd->io);
	if (rd->thaspos && !rd->haspos)
		elog(ERROR, "sift: corrupt build run (positions in a run without)");
}

static int
reader_cmp(Datum a, Datum b, void *arg)
{
	const RunReader *x = (const RunReader *) DatumGetPointer(a),
			   *y = (const RunReader *) DatumGetPointer(b);
	int			c = sift_term_cmp(x->term, x->len, y->term, y->len);

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

/* The postings of the batch in the run format (see spill_to) */
static void
batch_to_run(Batch *bt)
{
	Size		off = 0;

	for (int k = 0; k < bt->n; bt->prev = bt->keys[k++])
	{
		io_varint(bt->run, (bt->keys[k] - bt->prev) << 1 | (bt->tf[k] > 1));
		if (bt->tf[k] > 1)
			io_put(bt->run, &bt->tf[k], 1);
		io_varint(bt->run, bt->dl[k]);
		if (bt->haspos)
		{
			io_put(bt->run, bt->pos + off, bt->plen[k]);
			off += bt->plen[k];
		}
	}
}

/* Where a merge writes: a segment, or a run (the format of spill_to) that a later merge reads */
typedef struct MergeOut
{
	SiftSegmentWriter *sw;
	SiftRun    *run;			/* the run that is written, when there is no sw */
	RunIO		io;
	SiftKey		prev;
} MergeOut;

static void
out_docs(MergeOut *mo, const SiftKey *keys, const uint16 *dl, int n)
{
	if (mo->sw)
		sift_segw_add_docs(mo->sw, keys, dl, n);
	else
		for (int i = 0; i < n; mo->prev = keys[i++])
		{
			io_varint(&mo->io, keys[i] - mo->prev);
			io_varint(&mo->io, dl[i]);
		}
}

/* The term's postings follow in bt: df of them, with position records when bt->haspos */
static void
out_begin_term(MergeOut *mo, Batch *bt, const char *term, int len, uint64 df)
{
	bt->n = 0;
	bt->termpos = 0;
	if (mo->sw)
	{
		bt->pw = sift_segw_begin_term(mo->sw, term, len);
		return;
	}
	io_varint(&mo->io, len);
	io_put(&mo->io, term, len);
	io_varint(&mo->io, df);
	io_varint(&mo->io, bt->haspos);
	bt->prev = 0;
}

/* All runs' docs in key order (the runs' key ranges may interleave) */
static void
merge_docs(MergeOut *mo, RunReader *rd, int nruns)
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
			out_docs(mo, keys, dl, n);
			n = 0;
			CHECK_FOR_INTERRUPTS();
		}
	}
	if (n > 0)
		out_docs(mo, keys, dl, n);
	binaryheap_free(heap);
}

static inline void
record_put(RunReader *rd, uint8 c)
{
	if (rd->prlen == rd->prcap)
	{
		rd->prcap = Max(256, rd->prcap * 2);
		rd->prec = rd->prec ? repalloc_huge(rd->prec, rd->prcap) : MemoryContextAllocHuge(CurrentMemoryContext, rd->prcap);
	}
	rd->prec[rd->prlen++] = c;
}

static inline uint32
record_varint(RunReader *rd)
{
	uint32		x = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = io_byte(&rd->io);

		record_put(rd, c);
		x |= (uint32) (c & 0x7f) << s;
		if (!(c & 0x80))
			return x;
	}
}

/* The position record of the current posting (see bm25/analyze.h) into rd->prec */
static void
read_record(RunReader *rd)
{
	if (record_varint(rd) & 1)
	{
		uint32		n = record_varint(rd);

		for (uint32 i = 0; i <= n; i++)
			record_varint(rd);
	}
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
	rd->prlen = 0;
	if (rd->thaspos)
		read_record(rd);
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
			batch_put(bt, r->pkey, r->ptf, r->pdl, r->prec, r->prlen);
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

/* Opens the runs for reading; the docs and the positions of every run are known by then */
static RunReader *
open_readers(SiftRun **runs, int nruns, Size budget, bool *positions, uint64 *ndocs)
{
	RunReader  *rd = palloc0(nruns * sizeof(RunReader));

	*positions = true;
	*ndocs = 0;
	for (int i = 0; i < nruns; i++)
	{
		rd[i].idx = i;
		rd[i].io.file = run_open(runs[i]);
		rd[i].io.cap = reader_buf(budget, nruns);
		rd[i].io.buf = palloc(rd[i].io.cap);
		if (BufFileSeek(rd[i].io.file, 0, 0, SEEK_SET) != 0)
			elog(ERROR, "sift: could not rewind a build run");
		rd[i].dleft = io_get_varint(&rd[i].io);
		rd[i].haspos = io_get_varint(&rd[i].io);
		*ndocs += rd[i].dleft;
		*positions &= rd[i].haspos;
	}
	return rd;
}

/* Merges the docs and the terms of the opened runs into mo */
static void
merge_readers(RunReader *rd, int nruns, bool positions, MergeOut *mo)
{
	RunReader **cur = palloc(nruns * sizeof(RunReader *));
	RunReader **act = palloc(nruns * sizeof(RunReader *));
	binaryheap *heap = binaryheap_allocate(nruns, reader_cmp, NULL);
	Batch	   *bt = palloc0(sizeof(Batch));
	uint32		nterms = 0;

	bt->run = mo->sw ? NULL : &mo->io;
	merge_docs(mo, rd, nruns);
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
		uint64		df = 0;

		memcpy(term, ((RunReader *) DatumGetPointer(binaryheap_first(heap)))->term, len);
		while (!binaryheap_empty(heap))
		{
			RunReader  *top = (RunReader *) DatumGetPointer(binaryheap_first(heap));

			if (sift_term_cmp(top->term, top->len, term, len) != 0)
				break;
			cur[k++] = top;
			binaryheap_remove_first(heap);
		}
		/* a term whose postings lack the records in some run (inline there) keeps none */
		bt->haspos = positions;
		for (int i = 0; i < k; i++)
		{
			bt->haspos &= cur[i]->thaspos;
			df += cur[i]->df;
		}
		out_begin_term(mo, bt, term, len, df);
		merge_postings(bt, cur, k, act);
		batch_flush(bt);
		if (mo->sw)
			sift_segw_end_term(mo->sw);
		else
			run_note_term(mo->run, len, df, bt->termpos);
		for (int i = 0; i < k; i++)
		{
			reader_next_term(cur[i]);
			if (cur[i]->len)
				binaryheap_add(heap, PointerGetDatum(cur[i]));
		}
		TEST_MERGE_TERM(++nterms, df);
		if (nterms % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

/* Merges the runs into a new one of the home's files and closes them */
SiftRun *
sift_merge_to_run(SiftRun **runs, int nruns, Size budget, RunHome *home)
{
	SiftRun    *out = run_new(CurrentMemoryContext, home->fileset, home->worker, home->nfiles++);
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	MergeOut	mo = {.run = out,.io = {.file = out->file,.buf = palloc(IO_BUF)}};
	bool		positions;
	RunReader  *rd = open_readers(runs, nruns, budget, &positions, &out->ndocs);

	io_varint(&mo.io, out->ndocs);
	io_varint(&mo.io, positions);
	merge_readers(rd, nruns, positions, &mo);
	io_varint(&mo.io, 0);
	io_flush(&mo.io);
	for (int i = 0; i < nruns; i++)
	{
		run_close(runs[i]);
		pfree(runs[i]);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return out;
}

static int
run_size_cmp(const void *a, const void *b)
{
	return pg_cmp_u64((*(SiftRun *const *) b)->ndocs, (*(SiftRun *const *) a)->ndocs);
}

/* Merges the smallest runs, as few as it takes, until target are left; returns their number */
int
sift_shrink_runs(SiftRun **runs, int nruns, int target, Size budget, RunHome *home)
{
	int			fanin = sift_merge_fanin(budget);

	while (nruns > target)
	{
		int			k = Min(fanin, nruns - target + 1);

		qsort(runs, nruns, sizeof(SiftRun *), run_size_cmp);
		runs[nruns - k] = sift_merge_to_run(runs + nruns - k, k, budget, home);
		nruns -= k - 1;
	}
	return nruns;
}

void
sift_runs_write(SiftRun **runs, int nruns, SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref,
				SiftSegEntry *entry)
{
	RunHome		tmp = {0};
	Size		budget = (Size) maintenance_work_mem * 1024;
	MemoryContext cxt;
	MemoryContext old;
	MergeOut	mo = {0};
	bool		positions;
	uint64		ndocs;
	RunReader  *rd;

	/* more runs than readers fit the budget: merge the smallest into temporary ones first */
	nruns = sift_shrink_runs(runs, nruns, sift_merge_fanin(budget), budget, &tmp);
	cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run merge", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	rd = open_readers(runs, nruns, budget, &positions, &ndocs);
	/* the segment has positions when every run does */
	mo.sw = sift_segw_begin(wt, gen, ref, cxt, positions);
	/* the merged runs hold a term of any size: the posting writer takes about 10 bytes of each posting, half the budget at most */
	sift_segw_spool(mo.sw, sift_seg_postings(budget) / 2);
	merge_readers(rd, nruns, positions, &mo);
	sift_segw_finish(mo.sw, entry);
	for (int i = 0; i < nruns; i++)
		run_close(runs[i]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/* ---------- runs of segments (merge input) ---------- */

typedef struct DumpState
{
	RunIO		io;
	SiftKey		prev;
	uint64		ndocs;
	SiftRun    *run;
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

static void
dump_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	DumpState  *ds = arg;
	bool		haspos = (info->flags & SIFT_TERM_POS) != 0;

	io_varint(&ds->io, len);
	io_put(&ds->io, term, len);
	io_varint(&ds->io, info->df);
	io_varint(&ds->io, haspos);
	ds->prev = 0;
	run_note_term(ds->run, len, info->df, haspos ? info->pos_len : 0);
}

static void
dump_postings(void *arg, const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen,
			  int n)
{
	DumpState  *ds = arg;

	for (int j = 0; j < n; ds->prev = keys[j++])
	{
		io_varint(&ds->io, (keys[j] - ds->prev) << 1 | (tf[j] > 1));
		if (tf[j] > 1)
			io_put(&ds->io, &tf[j], 1);
		io_varint(&ds->io, dl[j]);
		if (pos)
		{
			io_put(&ds->io, pos, poslen[j]);
			pos += poslen[j];
		}
	}
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
	ds.run = run;
	ds.io.buf = palloc(IO_BUF);
	run->ndocs = ndocs;
	io_varint(&ds.io, ndocs);
	io_varint(&ds.io, (seg->entry.flags & SIFT_SEG_POSITIONS) != 0);
	sift_seg_stream(seg, (Size) maintenance_work_mem * 1024 / 2, &(SiftSegSink) {dump_docs, dump_term, dump_postings}, &ds);
	if (ds.ndocs != 0)
		elog(ERROR, "sift: segment %llu lists %llu live docs but streamed another number", ULL(seg->entry.gen), ULL(ndocs));
	io_varint(&ds.io, 0);
	io_flush(&ds.io);
	pfree(ds.io.buf);
	MemoryContextSwitchTo(old);
	return run;
}
