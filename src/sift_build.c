/*
 * sift_build.c - inverted buffer, sorted runs and their merge, ambuild and
 * ambuildempty.
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

#ifdef SIFT_TESTING
int			sift_test_run_docs = 0;
int			sift_test_fanin = 0;
int			sift_test_segment_postings = 0;
int			sift_test_build_peak = 0;
int			sift_test_pool_blocks = 0;
int			sift_test_page_text = 0;
int			sift_test_fail_merge = 0;
int			sift_test_fail_who = 0;
#define TEST_KNOB(knob, normal)	((knob) > 0 ? (knob) : (normal))

/* The most memory (kB) the contexts of the build allocated, sampled where a build is at its largest */
static MemoryContext mem_root;
static MemoryContextCallback mem_root_cb;
static int	test_merges;		/* merges so far of the build of this process */

static void
mem_root_gone(void *arg)
{
	mem_root = NULL;
}

static void
mem_watch(MemoryContext cxt)
{
	mem_root = cxt;
	test_merges = 0;
	mem_root_cb.func = mem_root_gone;
	MemoryContextRegisterResetCallback(cxt, &mem_root_cb);
}

static void
mem_sample(void)
{
	if (mem_root)
		sift_test_build_peak = Max(sift_test_build_peak, (int) Min(MemoryContextMemAllocated(mem_root, true) / 1024, INT_MAX));
}
#define MEM_SAMPLE()	mem_sample()

/* The sift_test_fail_merge'th merge of the build participants selected by sift_test_fail_who is cancelled after a few terms */
static void
test_fail_merge(uint32 nterms)
{
	if (nterms != 3 || mem_root == NULL || sift_test_fail_merge == 0 ||
		(sift_test_fail_who != 0 && (sift_test_fail_who == 1) != (ParallelWorkerNumber < 0)))
		return;
	if (++test_merges == sift_test_fail_merge)
		ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("canceling statement due to sift.test_fail_merge")));
}
#define TEST_FAIL_MERGE(nterms)	test_fail_merge(nterms)
#else
#define TEST_KNOB(knob, normal)	(normal)
#define MEM_SAMPLE()	((void) 0)
#define TEST_FAIL_MERGE(nterms)	((void) 0)
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
	uint32		maxdf;
	uint32		maxdf_full;		/* the buffer is full when a term has this many postings */
	uint32	   *doc_terms;		/* term indexes of the doc being added */
	int			maxdoc_terms;
	bool		positions;		/* docs carry position records */
};

struct SiftRun
{
	BufFile    *file;
	uint64		ndocs;
	uint64		maxdf;			/* postings of its largest term */
	uint64		maxblob;		/* bytes the largest term takes of a segment, at most */
	uint64		dict;			/* bytes the terms take of a segment's dictionary, at most */
	int			level;			/* merge passes the run has been through (spill cascade) */
	SharedFileSet *fileset;		/* NULL for a temporary file, else the file run_file(worker, id) of the set */
	int			worker;
	int			id;
};

/* Where the runs a build participant writes go: temporary files, or a fileset the leader reads */
typedef struct RunHome
{
	SharedFileSet *fileset;
	int			worker;
	int			nfiles;			/* ids taken */
} RunHome;

static uint32
pool_max(void)
{
	return TEST_KNOB(sift_test_pool_blocks, POOL_MAX_BLOCKS);
}

/* A leaf run holds a term up to a quarter of a segment's limit, but at least this many postings (small test limits) */
#define MIN_RUN_POSTINGS	64

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
static uint64
seg_postings(Size budget)
{
	return TEST_KNOB(sift_test_segment_postings, budget / 10);
}

/* The most runs one merge reads: its buffers must fit the budget, the leader reads this many of each worker */
#define MAX_MERGE_FANIN	256

static const char *
run_file(int worker, int run)
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
	run->file = fileset ? BufFileCreateFileSet(&fileset->fs, run_file(worker, id)) : BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	return run;
}

/* The file of a run another process wrote: opened when the run is merged, not while it waits */
static BufFile *
run_open(SiftRun *run)
{
	if (run->file == NULL)
		run->file = BufFileOpenFileSet(&run->fileset->fs, run_file(run->worker, run->id), O_RDONLY, false);
	return run->file;
}

/* Closes a merged run; a fileset's file goes too, as the leader or the worker has no more use for it */
static void
run_close(SiftRun *run)
{
	BufFileClose(run->file);
	if (run->fileset)
		BufFileDeleteFileSet(&run->fileset->fs, run_file(run->worker, run->id), false);
}

/* Runs one merge reads at a time: a reader each (the smallest has two blocks), within half the budget */
static int
merge_fanin(Size budget)
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
	b->maxdf = 0;
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
	b->maxdf_full = Max(Min(Min(Max(seg_postings(b->budget) / 4, MIN_RUN_POSTINGS), SIFT_BLOB_MAX / (4 * BLOB_POSTING_BYTES)),
							PG_INT32_MAX), 1);
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
	return inv_mem(b) >= b->budget || b->nblocks >= pool_max() - 1 || b->ndocs >= TEST_KNOB(sift_test_run_docs, PG_INT32_MAX / 2) ||
		b->tlen >= PG_INT32_MAX ||
		b->maxdf >= b->maxdf_full || b->tlen + (Size) b->nterms * DICT_ENTRY_BYTES >= SIFT_DICT_MAX / 4;
}

/*
 * Can the buffer take this doc? Its key must be above the last one's, and its terms must not exhaust the
 * pool, which the doc is not stopped by once added: a term takes a slice of 8 bytes, or one of up to 2 kB
 * when its slice is full, and each position at most 5 bytes of a record (a token takes 2 bytes of text).
 */
bool
sift_inv_fits(const SiftInvBuf *b, ItemPointer tid, int nterms, Size textlen)
{
	return (b->ndocs == 0 || sift_key_from_tid(tid) > DOC_KEY(b->docs[b->ndocs - 1])) &&
		(Size) b->nblocks * POOL_BLOCK + (Size) nterms * 2048 + 3 * textlen + 2 * POOL_BLOCK <= (Size) pool_max() * POOL_BLOCK;
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

/* Copies the position record at the reader to the chunk (see sift_analyze.h); returns its length */
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
		if (t->df == 0)
			t->haspos = b->positions && terms[i].pos != NULL;
		else if (t->haspos != (b->positions && terms[i].pos != NULL))
			elog(ERROR, "sift: the postings of a term must all have positions or none");
		if (t->haspos)
			for (uint32 k = 0; k < terms[i].poslen; k++)
				slice_put(b, t, terms[i].pos[k]);
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

static void batch_to_run(Batch *bt);

static void
batch_flush(Batch *bt)
{
	if (bt->n && bt->run)
		batch_to_run(bt);
	else if (bt->n)
		sift_postw_add(bt->pw, bt->keys, bt->tf, bt->dl, bt->n, bt->haspos ? bt->pos : NULL, bt->plen);
	bt->n = 0;
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
		bt->termpos += len;
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
		run_note_term(run, t->len, t->df, posbytes);
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

/* The position record of the current posting (see sift_analyze.h) into rd->prec */
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

			if (term_cmp(top->term, top->len, term, len) != 0)
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
		if (df >= 64)
			MEM_SAMPLE();
		for (int i = 0; i < k; i++)
		{
			reader_next_term(cur[i]);
			if (cur[i]->len)
				binaryheap_add(heap, PointerGetDatum(cur[i]));
		}
		TEST_FAIL_MERGE(++nterms);
		if (nterms % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

/* Merges the runs into a new one of the home's files and closes them */
static SiftRun *
merge_to_run(SiftRun **runs, int nruns, Size budget, RunHome *home)
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
static int
shrink_runs(SiftRun **runs, int nruns, int target, Size budget, RunHome *home)
{
	int			fanin = merge_fanin(budget);

	while (nruns > target)
	{
		int			k = Min(fanin, nruns - target + 1);

		qsort(runs, nruns, sizeof(SiftRun *), run_size_cmp);
		runs[nruns - k] = merge_to_run(runs + nruns - k, k, budget, home);
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
	nruns = shrink_runs(runs, nruns, merge_fanin(budget), budget, &tmp);
	cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run merge", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	rd = open_readers(runs, nruns, budget, &positions, &ndocs);
	/* the segment has positions when every run does */
	mo.sw = sift_segw_begin(wt, gen, ref, cxt, positions);
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

static bool
dump_term(void *arg, const char *term, int len, const SiftTermInfo *info, const SiftKey *keys,
		  const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen)
{
	DumpState  *ds = arg;
	SiftKey		prev = 0;

	io_varint(&ds->io, len);
	io_put(&ds->io, term, len);
	io_varint(&ds->io, info->df);
	io_varint(&ds->io, pos != NULL);
	for (uint32 j = 0; j < info->df; prev = keys[j++])
	{
		io_varint(&ds->io, (keys[j] - prev) << 1 | (tf[j] > 1));
		if (tf[j] > 1)
			io_put(&ds->io, &tf[j], 1);
		io_varint(&ds->io, dl[j]);
		if (pos)
		{
			io_put(&ds->io, pos, poslen[j]);
			pos += poslen[j];
		}
	}
	run_note_term(ds->run, len, info->df, pos ? info->pos_len : 0);
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
	ds.run = run;
	ds.io.buf = palloc(IO_BUF);
	run->ndocs = ndocs;
	io_varint(&ds.io, ndocs);
	io_varint(&ds.io, (seg->entry.flags & SIFT_SEG_POSITIONS) != 0);
	sift_seg_dump(seg, ref, dump_docs, dump_term, &ds);
	if (ds.ndocs != 0)
		elog(ERROR, "sift: segment %llu lists %llu live docs but streamed another number", ULL(seg->entry.gen), ULL(ndocs));
	io_varint(&ds.io, 0);
	io_flush(&ds.io);
	pfree(ds.io.buf);
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
	bool		positions;
	/* a parallel worker's runs go to files of the leader's fileset */
	RunHome		home;
} BuildState;

/* Reserves the next directory entry; its generation is the entry's 1-based position */
static SiftSegEntry *
next_entry(BuildState *bs, uint64 *gen)
{
	if (bs->nsegs == SIFT_MAX_SEGMENTS)
		elog(ERROR, "sift: build of \"%s\" ran out of directory entries", RelationGetRelationName(bs->index));
	*gen = bs->nsegs + 1;
	return &bs->segs[bs->nsegs++];
}

/*
 * Limits of a segment written from runs: what its largest term takes of the memory (see seg_postings), of
 * the postings of a term and of the dictionary (the writers fail beyond SIFT_BLOB_MAX and SIFT_DICT_MAX).
 */
typedef struct SegLimits
{
	uint64		df;
	uint64		blob;
	uint64		dict;
} SegLimits;

static SegLimits
seg_limits(Size budget)
{
	return (SegLimits) {seg_postings(budget), SIFT_BLOB_MAX, SIFT_DICT_MAX};
}

/* What the runs of a segment add up to, an upper bound of what the merged run takes */
typedef struct RunSum
{
	uint64		df;
	uint64		blob;
	uint64		dict;
} RunSum;

static RunSum
sum_with(RunSum sum, const SiftRun *run)
{
	sum.df += run->maxdf;
	sum.blob += run->maxblob;
	sum.dict += run->dict;
	return sum;
}

static bool
sum_fits(RunSum sum, const SegLimits *lim)
{
	return sum.df <= lim->df && sum.blob <= lim->blob && sum.dict <= lim->dict;
}

/*
 * How many of the last runs merge now into one, 0 for none. Only runs of one level merge (so a posting is
 * merged a number of times logarithmic in the runs), as many as fit a segment's limits, at most fan-in: when
 * the fan-in is there, or when the run to come would not fit with them, they are as large as they get.
 */
static int
tail_merge(SiftRun **runs, int nruns, int fanin, const SegLimits *lim)
{
	RunSum		sum = {0};
	int			k = 0;

	for (int i = nruns - 1; i >= 0 && k < fanin && runs[i]->level == runs[nruns - 1]->level; i--)
	{
		if (!sum_fits(sum_with(sum, runs[i]), lim))
			break;
		sum = sum_with(sum, runs[i]);
		k++;
	}
	return k >= 2 && (k == fanin || !sum_fits(sum_with(sum, runs[nruns - 1]), lim)) ? k : 0;
}

/*
 * Spills the buffer to a run, then merges the tail of the runs while tail_merge says so. A participant keeps
 * few runs (their files and buffers fit the budget) and none a hot term would make larger than a segment
 * takes: with the table size the runs that stay are the segments, not levels of merges the term holds whole.
 */
static void
spill(BuildState *bs)
{
	int			fanin = merge_fanin(bs->budget);
	SegLimits	lim = seg_limits(bs->budget);
	SiftRun    *run;
	int			k;

	MEM_SAMPLE();
	if (bs->nruns + 1 >= bs->maxruns)
	{
		bs->maxruns *= 2;
		bs->runs = repalloc(bs->runs, bs->maxruns * sizeof(SiftRun *));
	}
	if (bs->home.fileset == NULL)
		run = sift_inv_spill(bs->inv);
	else
		run = sift_inv_spill_shared(bs->inv, bs->home.fileset, bs->home.worker, bs->home.nfiles++);
	bs->runs[bs->nruns++] = run;
	while ((k = tail_merge(bs->runs, bs->nruns, fanin, &lim)) > 0)
	{
		int			level = bs->runs[bs->nruns - 1]->level + 1;
		MemoryContext old = MemoryContextSwitchTo(bs->cxt);

		run = merge_to_run(bs->runs + bs->nruns - k, k, bs->budget, &bs->home);
		MemoryContextSwitchTo(old);
		run->level = level;
		bs->nruns -= k - 1;
		bs->runs[bs->nruns - 1] = run;
	}
}

/* End of the runs from first on that go to one segment: their largest terms fit the limits together */
static int
group_end(SiftRun **runs, int first, int nruns, const SegLimits *lim)
{
	RunSum		sum = sum_with((RunSum) {0}, runs[first]);
	int			last = first + 1;

	for (; last < nruns && sum_fits(sum_with(sum, runs[last]), lim); last++)
		sum = sum_with(sum, runs[last]);
	return last;
}

static int
count_groups(SiftRun **runs, int nruns, const SegLimits *lim)
{
	int			n = 0;

	for (int first = 0; first < nruns; first = group_end(runs, first, nruns, lim))
		n++;
	return n;
}

/* Writes the docs gathered since the last boundary as one or more segments */
static void
end_segment(BuildState *bs)
{
	int			first = 0;
	uint64		gen;
	SiftSegEntry *entry;
	SegLimits	lim = seg_limits(bs->budget);
	/* the entries the segments still to come (one per forced boundary, and the last) leave */
	int			room = SIFT_MAX_SEGMENTS - bs->nsegs - (bs->nbounds - bs->nextbound);

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
	ereport(DEBUG2, errmsg("sift index \"%s\": merging %d runs into segments", RelationGetRelationName(bs->index), bs->nruns));
	/*
	 * Each segment merges runs whose largest terms fit the budget together. The directory holds a
	 * number of segments, though: a term is encoded whole, so when the budget would need more, fewer
	 * and larger segments are written, which take more memory than the budget in their largest terms.
	 */
	while (count_groups(bs->runs, bs->nruns, &lim) > room)
	{
		if (lim.df > PG_INT64_MAX / 2)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" would need more than %d segments", RelationGetRelationName(bs->index), SIFT_MAX_SEGMENTS)));
		lim.df *= 2;
	}
	while (first < bs->nruns)
	{
		int			last = group_end(bs->runs, first, bs->nruns, &lim);

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

/* Analyzes a doc into the buffer, which is spilled first when the doc does not fit it */
static void
analyze_doc(BuildState *bs, ItemPointer tid, const char *text, int len)
{
	SiftTermFreq *terms;
	uint32		dl;
	int			n = sift_analyze_doc(bs->an, text, len, &terms, &dl);

	if (!sift_inv_empty(bs->inv) && !sift_inv_fits(bs->inv, tid, n, len))
		spill(bs);
	sift_inv_add(bs->inv, tid, terms, n, dl);
	bs->indexed++;
	if (sift_inv_full(bs->inv))
		spill(bs);
}

/* Analyzes the buffered docs of one heap page in tid order */
static void
flush_page(BuildState *bs)
{
	qsort(bs->docs, bs->ndocs, sizeof(PageDoc), pagedoc_cmp);
	for (int i = 0; i < bs->ndocs; i++)
		analyze_doc(bs, &bs->docs[i].tid, bs->text.data + bs->docs[i].off, bs->docs[i].len);
	bs->ndocs = 0;
	resetStringInfo(&bs->text);
	sift_analyzer_reset(bs->an);
	CHECK_FOR_INTERRUPTS();
}

/*
 * The text of the docs of a heap page is copied until the page is done, to analyze them in tid order (a
 * HOT chain reports its root's tid). A page of large docs does not hold them all: past this many bytes
 * what is buffered is analyzed, and a doc over it is analyzed at once. A later doc of the page with a
 * lower tid then starts a new run (sift_inv_fits).
 */
static Size
page_text_max(const BuildState *bs)
{
	return TEST_KNOB(sift_test_page_text, Min(Max(bs->budget / 4, 1024 * 1024), 256 * 1024 * 1024));
}

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	BuildState *bs = state;
	MemoryContext old;
	text	   *t;
	int			len;
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
	len = VARSIZE_ANY_EXHDR(t);
	if (bs->text.len > 0 && bs->text.len + len > page_text_max(bs))
		flush_page(bs);
	if (len > page_text_max(bs))
	{
		analyze_doc(bs, tid, VARDATA_ANY(t), len);
		sift_analyzer_reset(bs->an);
	}
	else
	{
		if (bs->ndocs == bs->maxdocs)
		{
			bs->maxdocs *= 2;
			bs->docs = repalloc(bs->docs, bs->maxdocs * sizeof(PageDoc));
		}
		bs->docs[bs->ndocs++] = (PageDoc) {*tid, bs->text.len, len};
		appendBinaryStringInfo(&bs->text, VARDATA_ANY(t), len);
	}
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
	bs->positions = opts->positions && !cfg.keyword;
	sift_analyzer_set_positions(bs->an, bs->positions);
	bs->budget = budget;
	bs->inv = sift_inv_create(bs->budget, bs->cxt, bs->positions);
	bs->ref = (SiftScoreRef) {(float4) opts->k1, (float4) opts->b, 0};
	bs->page = InvalidBlockNumber;
	bs->maxdocs = 64;
	bs->docs = palloc(bs->maxdocs * sizeof(PageDoc));
	initStringInfo(&bs->text);
	bs->maxruns = 16;
	bs->runs = palloc(bs->maxruns * sizeof(SiftRun *));
	MemoryContextSwitchTo(old);
#ifdef SIFT_TESTING
	mem_watch(bs->cxt);
#endif
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
	int			peak_kb;			/* the most memory a participant allocated (SIFT_TESTING) */
	/* the parallel table scan descriptor follows */
} SiftBuildShared;

/* The most runs a worker hands over (the leader opens each as it merges it) */
#define MAX_PART_RUNS	1024

/* A worker's runs, in the files run_file(worker number, id) */
typedef struct SiftBuildPart
{
	int			nruns;
	struct
	{
		int			id;
		uint64		ndocs;
		uint64		maxdf;
		uint64		maxblob;
		uint64		dict;
	}			run[MAX_PART_RUNS];
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
			*run = (SiftRun) {.ndocs = ldr->parts[w].run[i].ndocs,.maxdf = ldr->parts[w].run[i].maxdf,
				.maxblob = ldr->parts[w].run[i].maxblob,.dict = ldr->parts[w].run[i].dict,
			.fileset = &shared->fileset,.worker = w,.id = ldr->parts[w].run[i].id};
			bs->runs[bs->nruns++] = run;
		}
		InstrAccumParallelQuery(&ldr->bufferusage[w], &ldr->walusage[w]);
	}
	res->heap_tuples = shared->reltuples;
	res->index_tuples = shared->indexed;
#ifdef SIFT_TESTING
	sift_test_build_peak = Max(sift_test_build_peak, shared->peak_kb);
#endif
}

static void
add_counts(SiftBuildShared *shared, double reltuples, double indexed)
{
	SpinLockAcquire(&shared->mutex);
	shared->reltuples += reltuples;
	shared->indexed += indexed;
	SpinLockRelease(&shared->mutex);
}

/* Spills what is left, merges the runs down to what a part holds and makes them readable by the leader */
static void
hand_over_runs(BuildState *bs, SiftBuildPart *part)
{
	MemoryContext old = MemoryContextSwitchTo(bs->cxt);

	if (!sift_inv_empty(bs->inv))
		spill(bs);
	bs->nruns = shrink_runs(bs->runs, bs->nruns, MAX_PART_RUNS, bs->budget, &bs->home);
	MemoryContextSwitchTo(old);
	part->nruns = bs->nruns;
	for (int i = 0; i < bs->nruns; i++)
	{
		part->run[i].id = bs->runs[i]->id;
		part->run[i].ndocs = bs->runs[i]->ndocs;
		part->run[i].maxdf = bs->runs[i]->maxdf;
		part->run[i].maxblob = bs->runs[i]->maxblob;
		part->run[i].dict = bs->runs[i]->dict;
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
	bs->home = (RunHome) {.fileset = &shared->fileset,.worker = ParallelWorkerNumber};
	reltuples = build_scan(bs, heap, info, sift_table_beginscan_parallel(heap, SCAN_OF(shared)), false);
	hand_over_runs(bs, part);
	add_counts(shared, reltuples, bs->indexed);
#ifdef SIFT_TESTING
	MEM_SAMPLE();
	SpinLockAcquire(&shared->mutex);
	shared->peak_kb = Max(shared->peak_kb, sift_test_build_peak);
	SpinLockRelease(&shared->mutex);
#endif
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
	res->heap_tuples = build_scan(bs, heap, info, ldr ? sift_table_beginscan_parallel(heap, SCAN_OF(ldr->shared)) : NULL, true);
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
