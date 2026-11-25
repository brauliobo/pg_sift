/*
 * col/granule/cfilt_str_simd.c - the string predicates of a chunk on its lengths and on one sweep over its bytes (col/granule/cfilt_str_simd.h).
 *
 * The strings of a chunk that is stored as FSST symbols, as a block or as offsets over bytes decode to one buffer, the strings one after the other: a substring search over the buffer finds the
 * strings that hold the needle by the offset of each hit, and an occurrence across two strings is not a hit. A string is empty exactly when its length is 0, so <> '' reads the lengths only.
 */
#include "postgres.h"

#include "core/par/arena.h"

#include "col/fsst.h"
#include "col/f2/f2_pred.h"
#include "col/granule/cfilt_str_simd.h"
#include "col/granule/granule_int.h"
#include "col/like_simd.h"
#include "utils/memutils.h"

/* The memo of the leader: the context it lives in drops it with the callback, which a worker's memo (SiftStrMemo.cx NULL, in the arena) has no use for */
typedef struct PgMemo
{
	MemoryContextCallback gone;
	SiftStrMemo m;
} PgMemo;

static PgMemo *memo_active;

static void
memo_gone(void *arg)
{
	if (memo_active == arg)
		memo_active = NULL;
}

void
sift_cfstr_memo_open(MemoryContext cx)
{
	PgMemo	   *pm = MemoryContextAllocZero(cx, sizeof(PgMemo));

	pm->m.cx = cx;
	pm->gone = (MemoryContextCallback) {.func = memo_gone,.arg = pm};
	MemoryContextRegisterResetCallback(cx, &pm->gone);
	memo_active = pm;
}

SiftStrMemo *
sift_cfstr_memo_active(void)
{
	return memo_active != NULL ? &memo_active->m : NULL;
}

static bool
memo_owns(const SiftStrMemo *m, void *p)
{
	return m->cx == NULL || GetMemoryChunkContext(p) == m->cx;
}

/* The decoded strings of chunk c (those of the ranks in live only, when it is given), which the caller allocated in the current context or arena: kept when the memo owns them, else freed */
static void
memo_keep(SiftStrMemo *m, const SiftChunk *c, uint32 *offs, char *bytes, const uint64 *live)
{
	if (m != NULL && m->n < SIFT_STR_MEMO_MAX && memo_owns(m, offs) && memo_owns(m, bytes))
		m->e[m->n++] = (SiftStrMemoEnt) {.rec = c->view->rec,.col = c->idx - 1,.offs = offs,.bytes = bytes,.live = live};
	else
	{
		sift_free(offs);
		sift_free(bytes);
		if (live != NULL)
			sift_free((void *) live);
	}
}

static bool
memo_has(const SiftStrMemoEnt *e, int rank)
{
	return e->live == NULL || ((e->live[rank >> 6] >> (rank & 63)) & 1);
}

/* The strings of the rows from one entry: false when a row's string is not among those it decoded */
static bool
memo_fill(const SiftStrMemoEnt *e, const SiftChunk *c, const uint16 *rows, uint16 nrows, SiftStrCell *out)
{
	int			word = 0,
				rank = 0;

	for (int j = 0; j < nrows; j++)
	{
		const int	row = rows != NULL ? rows[j] : j;

		out[j] = (SiftStrCell) {.isnull = !sift_chunk_row_valid(c, row)};
		if (!out[j].isnull)
		{
			const int	k = sift_chunk_rank_advance(c, &word, &rank, row);

			if (!memo_has(e, k))
				return false;
			out[j].p = e->bytes + e->offs[k];
			out[j].len = e->offs[k + 1] - e->offs[k];
		}
	}
	return true;
}

bool
sift_cfstr_memo_strs_in(const SiftStrMemo *m, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, SiftStrCell *out)
{
	SiftChunk	c;
	bool		opened = false,
				found = false;

	for (int i = m->n; i-- > 0 && !found;)
	{
		const SiftStrMemoEnt *e = &m->e[i];

		if (e->rec != v->rec || e->col != col)
			continue;
		if (!opened)
			sift_chunk_open(v, col + 1, &c);
		opened = true;
		found = memo_fill(e, &c, rows, nrows, out);
	}
	if (opened)
		sift_chunk_close(&c);
	return found;
}

void
sift_cfs_invert(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		bits[w] = ~bits[w];
	if (n & 63)
		bits[n >> 6] &= (UINT64CONST(1) << (n & 63)) - 1;
}

static bool
chunk_is_slab(uint8 enc)
{
	return enc == SIFT_CENC_STR_FSST || enc == SIFT_CENC_STR_BLOCK || enc == SIFT_CENC_STR_INLINE || enc == SIFT_CENC_STR_VIEW;
}

/* What a row of a string predicate asks of its string */
typedef struct StrOp
{
	int			op;
	const char *lit;
	uint32		litlen;
	SiftLikeNeedle nd;
} StrOp;

/* The strings of a chunk as it stores them: a slab behind its offsets, or views over a heap */
typedef struct StrSrc
{
	const uint32 *offs;
	const uint8 *bytes;
	const char *views;
	const char *heap;
} StrSrc;

static inline bool
row_hit(const StrOp *o, const char *s, uint32 len)
{
	if (o->op == SIFT_COLOP_NE)
		return len != o->litlen || memcmp(s, o->lit, len) != 0;
	return (sift_like_find(&o->nd, (const uint8 *) s, len) >= 0) == (o->op != SIFT_COLOP_NOTCONTAINS);
}

static inline bool
src_hit(const StrOp *o, const StrSrc *src, uint32 k)
{
	SiftStrView v;
	const char *at;

	if (src->offs != NULL)
		return row_hit(o, (const char *) src->bytes + src->offs[k], src->offs[k + 1] - src->offs[k]);
	at = src->views + (size_t) k * sizeof(SiftStrView);
	memcpy(&v, at, sizeof(v));
	return row_hit(o, v.len <= 12 ? at + offsetof(SiftStrView, prefix) : src->heap + v.u.offset, v.len);
}

/* The bits of the nn strings of src (those of dense_live only when it is given): a slab is swept whole by the substring search when most of its strings are wanted */
static void
src_bits(const StrOp *o, const StrSrc *src, uint32 nn, const uint64 *dense_live, uint64 *dense)
{
	memset(dense, 0, ((nn + 63) / 64) * sizeof(uint64));
	if (dense_live != NULL)
	{
		for (uint32 w = 0; w < (nn + 63) / 64; w++)
			for (uint64 bits = dense_live[w]; bits != 0; bits &= bits - 1)
			{
				uint32		k = (w << 6) + (uint32) __builtin_ctzll(bits);

				dense[w] |= (uint64) src_hit(o, src, k) << (k & 63);
			}
	}
	else if (src->offs != NULL && o->op != SIFT_COLOP_NE)
	{
		sift_like_dict(&o->nd, nn, src->offs, src->bytes, dense, NULL);
		if (o->op == SIFT_COLOP_NOTCONTAINS)
			sift_cfs_invert(dense, nn);
	}
	else
		for (uint32 k = 0; k < nn; k++)
			dense[k >> 6] |= (uint64) src_hit(o, src, k) << (k & 63);
}

/* The live strings of an FSST chunk, decoded one after the other into one buffer and tested: the memo keeps it with the ranks it holds, for the view of the same granule */
static void
fsst_live(SiftStrMemo *m, const StrOp *o, const SiftChunk *c, const SiftFsstChunk *f, const uint64 *live, uint64 *dense)
{
	const uint32 nn = c->nnonnull,
				words = (nn + 63) / 64;
	uint64		cap = SIFT_FSST_SLACK,
				at = 0;
	uint32	   *outoff = sift_alloc(((size_t) nn + 1) * sizeof(uint32)),
				next = 0;
	uint64	   *kept = memcpy(sift_alloc(words * sizeof(uint64)), live, words * sizeof(uint64));
	uint8	   *out;

	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = live[w]; bits != 0; bits &= bits - 1)
			cap += 8 * (uint64) f->clen[(w << 6) + (uint32) __builtin_ctzll(bits)];
	out = sift_alloc(cap);
	memset(dense, 0, words * sizeof(uint64));
	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = live[w]; bits != 0; bits &= bits - 1)
		{
			uint32		k = (w << 6) + (uint32) __builtin_ctzll(bits),
						len;

			for (; next <= k; next++)
				outoff[next] = at;
			len = sift_fsst_decode(f->t, f->coded + f->off[k], (uint32) f->clen[k], out + at);
			if (len == UINT32_MAX)
				SIFT_FAIL_CORRUPT("sift: a coded string does not decode");
			dense[w] |= (uint64) row_hit(o, (const char *) out + at, len) << (k & 63);
			at += len;
		}
	for (; next <= nn; next++)
		outoff[next] = at;
	memo_keep(m, c, outoff, (char *) out, kept);
}

/* FSST: the live strings are decoded and tested, when they are few; else the chunk is decoded to one slab */
static void
fsst_bits(SiftStrMemo *m, const StrOp *o, const SiftChunk *c, const uint64 *dense_live, uint64 *dense)
{
	SiftFsstChunk f;
	uint32		nn = c->nnonnull;

	if (!sift_fsst_chunk_open(c, &f))
		SIFT_FAIL_CORRUPT("sift: the symbol table of a string chunk is damaged");
	sift_fsst_chunk_offsets(&f, nn);
	if (dense_live != NULL)
		fsst_live(m, o, c, &f, dense_live, dense);
	else
	{
		uint64		cap = Min((uint64) f.raw_bytes, (uint64) 8 * f.coded_bytes) + SIFT_FSST_SLACK;
		uint8	   *out = sift_alloc(cap);
		uint32	   *outoff = sift_alloc(((size_t) nn + 1) * sizeof(uint32));
		StrSrc		src = {.offs = outoff,.bytes = out};

		if (!sift_fsst_decode_strs(f.t, f.coded, f.off, nn, out, cap, outoff))
			SIFT_FAIL_CORRUPT("sift: a coded string does not decode");
		src_bits(o, &src, nn, NULL, dense);
		memo_keep(m, c, outoff, (char *) out, NULL);
	}
	sift_fsst_chunk_close(&f);
}

bool
sift_cfstr_bits_live_in(SiftStrMemo *m, const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *dense_live, uint32 live_nn, uint64 *dense)
{
	const SiftColPred *pr = cp->pred;
	const SiftColValue *v = &pr->vals[0];
	const uint32 nn = c->nnonnull;
	StrOp		o = {.op = pr->op,.lit = v->v.s,.litlen = v->len};
	StrSrc		src = {0};
	const uint64 *live = dense_live != NULL && (uint64) live_nn * 8 < nn ? dense_live : NULL;

	if (!chunk_is_slab(c->enc) || nn == 0)
		return false;
	if (pr->op != SIFT_COLOP_NE && pr->op != SIFT_COLOP_CONTAINS && pr->op != SIFT_COLOP_NOTCONTAINS && pr->op != SIFT_COLOP_ICONTAINS)
		return false;
	if (pr->op != SIFT_COLOP_NE)
		sift_like_needle_borrow(&o.nd, (const uint8 *) v->v.s, v->len, pr->op == SIFT_COLOP_ICONTAINS);
	switch (c->enc)
	{
		case SIFT_CENC_STR_INLINE:
			src = (StrSrc) {.offs = (const uint32 *) c->body,.bytes = (const uint8 *) c->body + 4 * ((uint64) nn + 1)};
			src_bits(&o, &src, nn, live, dense);
			break;
		case SIFT_CENC_STR_VIEW:
			src = (StrSrc) {.views = c->body,.heap = c->body + (uint64) nn * sizeof(SiftStrView)};
			src_bits(&o, &src, nn, live, dense);
			break;
		case SIFT_CENC_STR_BLOCK:
			{
				uint32	   *offs;
				char	   *bytes;

				sift_chunk_block_strs_own(c, &offs, &bytes);
				src = (StrSrc) {.offs = offs,.bytes = (const uint8 *) bytes};
				src_bits(&o, &src, nn, live, dense);
				memo_keep(m, c, offs, bytes, NULL);
			}
			break;
		default:
			fsst_bits(m, &o, c, live, dense);
			break;
	}
	return true;
}

bool
sift_cfstr_bits_live(const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *dense_live, uint32 live_nn, uint64 *dense)
{
	return sift_cfstr_bits_live_in(sift_cfstr_memo_active(), cp, c, dense_live, live_nn, dense);
}

bool
sift_cfstr_bits(const SiftCfiltPred *cp, const SiftChunk *c, uint64 *dense)
{
	return sift_cfstr_bits_live(cp, c, NULL, 0, dense);
}
