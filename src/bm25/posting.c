/*
 * bm25/posting.c - term blob writer and reader (layout in bm25/posting.h).
 */
#include "sift_host.h"

#include "bm25/kernels.h"
#include "bm25/posting.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)
/* the L1 bit and nibble bytes of a term, from counts the dictionary stores as uint32: the sums wrap in 32 bits when a count is near 2^32 */
#define L1_BYTES(info)	(((uint64) (info)->df + 7) / 8)
#define NIB_BYTES(info)	(((uint64) (info)->nnib + 1) / 2)
#define BLOB_NG(blob)	(*(const uint32 *) ((blob) + 8))
#define BLOB_KEYS(blob)	((const uint32 *) ((blob) + 12))

int			sift_grouped_min_df = SIFT_GROUPED_MIN_DF;
#ifdef SIFT_TESTING
int			sift_test_blob_max = 0;
#endif

/*
 * The blob of a GROUPED term staged group by group (SiftPostingWriter.spool_min), so that the memory of a term does not
 * grow with its postings: the containers of the groups, the three parts of the tf stream and the checkpoints and records
 * of the positions are temporary files, copied into the segment when the term ends. What stays in memory is the index of
 * the blob, 36 bytes a group (a group is a superblock: up to 74,496 documents).
 */
typedef struct TermSpool
{
	BufFile    *payload;
	BufFile    *l1;
	BufFile    *nib;
	BufFile    *esc;
	BufFile    *ckpt;
	BufFile    *pos;
	uint64		npayload;
	uint64		nstaged;		/* postings staged */
	uint32		nnib;			/* L1 members and escape bytes of the groups staged */
	uint32		nesc;
	uint32		tfnib;			/* nibbles written to the tf stream (nnib once the tf of every group is) */
	uint8		pend_l1;		/* the partial last byte of the L1 bits and of the nibbles */
	uint8		pend_nib;
	uint32		ng;
	uint32		maxg;
	uint32	   *key;			/* the codec index of the blob, ng entries (poff and base ng + 1 once complete) */
	uint32	   *poff;
	uint32	   *base;
	SiftGroupScore *gs;
	uint8	   *rows;			/* the block bound rows of the groups that have one */
	uint32		nrows;
	uint8		max_bound;
} TermSpool;

#define TF_PIECE		4096	/* postings of the tf stream encoded at once when staging */

struct SiftPostingWriter
{
	SiftStreamWriter *sw;
	MemoryContext mcxt;
	SiftBm25	ref;
	SiftKey    *keys;
	uint8	   *tf;
	uint8	   *bound;
	uint32		n;
	uint32		cap;
	uint32		grouped_min;	/* df from which the term is GROUPED */
	uint32		spool_min;		/* GROUPED terms of this many postings or more are staged (0 = never) */
	int			ctype;			/* container choice of GROUPED terms (SIFT_CT_*) */
	uint32		open_start;		/* first posting of the last group in keys (staging keeps it) */
	TermSpool  *sp;				/* the staging of the current term, once it reached spool_min */
	uint8	   *out;
	Size		outcap;
	uint8	   *pos;			/* position records of the term's postings, back to back (not with sp) */
	Size		poslen;			/* their bytes so far, staged or not */
	Size		poscap;
	uint32	   *ckpt;			/* offset in pos of every SIFT_POS_EVERY-th posting (not with sp) */
	bool		haspos;
	uint8		bound1[SIFT_MAX_DL + 1];	/* bound for tf 1 by dl; 0 = not computed */
};

struct SiftPostingCursor
{
	SiftTermInfo info;
	sift_cur	cur;
	uint8	   *curbuf;
	const uint8 *blob;
	const SiftGroupScore *gs;	/* GROUPED */
	uint32		ng;
	const uint8 *blkrows;
	const uint8 *l1;
	const uint8 *nib;
	const uint8 *esc;
	Size		bloblen;		/* the blob without its position stream */
	SiftStreamReader *sr;		/* the position stream is read on demand */
	uint64		posoff;			/* its offset in the segment stream */
	uint32		poslen;
	uint32		nck;
	uint32	   *ckpt;			/* checkpoints, read with the first position */
	uint8	   *win;			/* window of the records */
	uint64		win_lo;			/* offset in the records */
	uint32		win_len;
	SiftTfPos	next;			/* LIST: where the last decoded group ended */
	SiftGroupBounds *bounds;
	int			nbounds;
	MemoryContext mcxt;
};

uint8
sift_bound_u8(const SiftScoreRef *ref, uint32 tf, uint32 dl)
{
	SiftBm25	bm = {.k1 = ref->k1,.b = ref->b,.avgdl = ref->avgdl};

	return sift_kern_bound_u8(&bm, tf, dl);
}

/* ---------- writer ---------- */

SiftPostingWriter *
sift_postw_begin(SiftStreamWriter *sw, const SiftScoreRef *ref, MemoryContext mcxt)
{
	SiftPostingWriter *w = MemoryContextAllocZero(mcxt, sizeof(SiftPostingWriter));

	w->sw = sw;
	w->mcxt = mcxt;
	w->ref.k1 = ref->k1;
	w->ref.b = ref->b;
	w->ref.avgdl = ref->avgdl;
	w->grouped_min = (uint32) sift_grouped_min_df;
	w->ctype = SIFT_CT_AUTO;
	return w;
}

static void *
grow(MemoryContext mcxt, void *p, Size sz)
{
	return p ? repalloc_huge(p, sz) : MemoryContextAllocHuge(mcxt, sz);
}

static inline uint8
bound_of(SiftPostingWriter *w, uint8 tf, uint16 dl)
{
	if (tf > 1)
		return sift_kern_bound_u8(&w->ref, tf, dl);
	if (w->bound1[dl] == 0)
		w->bound1[dl] = sift_kern_bound_u8(&w->ref, 1, dl);
	return w->bound1[dl];
}

static void spool_begin(SiftPostingWriter *w);
static void spool_stage(SiftPostingWriter *w, uint32 upto);

/* The checkpoint of the posting at rank (a multiple of SIFT_POS_EVERY) in the term: the record bytes before it */
static pg_attribute_always_inline void
ckpt_put(SiftPostingWriter *w, uint64 rank, Size at, bool staged)
{
	if (staged && w->sp)
	{
		uint32		v = at;

		BufFileWrite(w->sp->ckpt, &v, sizeof(v));
	}
	else
		w->ckpt[rank / SIFT_POS_EVERY] = at;
}

/* Appends the n postings' records (their lengths in poslen) to the term's */
static pg_attribute_always_inline void
pos_put(SiftPostingWriter *w, const uint8 *pos, const uint32 *poslen, int n, uint64 rank, bool staged)
{
	Size		len = 0;

	for (int i = 0; i < n; len += poslen[i++])
		if ((rank + i) % SIFT_POS_EVERY == 0)
			ckpt_put(w, rank + i, w->poslen + len, staged);
	if (staged && w->sp)
		BufFileWrite(w->sp->pos, pos, len);
	else
	{
		if (w->poslen + len > w->poscap)
		{
			w->poscap = Max(w->poslen + len, Max(4096, w->poscap * 2));
			w->pos = grow(w->mcxt, w->pos, w->poscap);
		}
		memcpy(w->pos + w->poslen, pos, len);
	}
	w->poslen += len;
}

/*
 * The postings of a term in memory (staged false), or a term that is staged in files or reaches the size from which it is
 * (staged true, out of line so that the common case keeps its own small frame: the address of a local in it, a stack
 * protector and a larger frame are some 30 instructions for each of the terms of a segment)
 */
static pg_attribute_always_inline void
postw_add(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf, const uint16 *dl, int n, const uint8 *pos,
		  const uint32 *poslen, bool staged)
{
	if (n <= 0)
		return;
	if (w->n + n > w->cap)
	{
		w->cap = Max(w->n + n, Max(1024, w->cap * 2));
		w->keys = grow(w->mcxt, w->keys, (Size) w->cap * sizeof(SiftKey));
		w->tf = grow(w->mcxt, w->tf, w->cap);
		w->bound = grow(w->mcxt, w->bound, w->cap);
		w->ckpt = grow(w->mcxt, w->ckpt, (Size) ((w->cap + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY) * sizeof(uint32));
	}
	if (w->n == 0)
		w->haspos = pos != NULL;
	else if (w->haspos != (pos != NULL))
		elog(ERROR, "sift: postings of a term must all have positions or none");
	for (int i = 0; i < n; i++)
	{
		if ((w->n > 0 || i > 0) && keys[i] <= (i > 0 ? keys[i - 1] : w->keys[w->n - 1]))
			elog(ERROR, "sift: postings of a term must arrive in ascending key order");
		if (tf[i] == 0)
			elog(ERROR, "sift: posting with tf 0");
		w->bound[w->n + i] = bound_of(w, tf[i], dl[i]);
		if (staged && w->n + i > 0 && SIFT_GID(keys[i]) != SIFT_GID(i > 0 ? keys[i - 1] : w->keys[w->n - 1]))
			w->open_start = w->n + i;
	}
	memcpy(w->keys + w->n, keys, (Size) n * sizeof(SiftKey));
	memcpy(w->tf + w->n, tf, (Size) n);
	if (pos)
		pos_put(w, pos, poslen, n, staged && w->sp ? w->sp->nstaged + w->n : w->n, staged);
	w->n += n;
	if (!staged)
		return;
	if (w->sp == NULL && w->n >= w->spool_min)
		spool_begin(w);
	if (w->open_start > 0)
		spool_stage(w, w->open_start);
}

static pg_noinline void
postw_add_staged(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf, const uint16 *dl, int n, const uint8 *pos,
				 const uint32 *poslen)
{
	postw_add(w, keys, tf, dl, n, pos, poslen, true);
}

void
sift_postw_add(SiftPostingWriter *w, const SiftKey *keys, const uint8 *tf, const uint16 *dl, int n,
			   const uint8 *pos, const uint32 *poslen)
{
	if (unlikely(w->sp != NULL || (w->spool_min && (int64) w->n + n >= w->spool_min)))
		postw_add_staged(w, keys, tf, dl, n, pos, poslen);
	else
		postw_add(w, keys, tf, dl, n, pos, poslen, false);
}

static uint8 *
out_buf(SiftPostingWriter *w, Size len)
{
	if (len > w->outcap)
	{
		w->outcap = Max(len, Max(4096, w->outcap * 2));
		if (w->out)
			pfree(w->out);
		w->out = MemoryContextAllocHuge(w->mcxt, w->outcap);
	}
	memset(w->out, 0, len);
	return w->out;
}

/* The end of the group that starts at posting i, by bisection: the keys ascend, so do the group ids */
static uint32
group_end(const SiftPostingWriter *w, uint32 i, uint32 n)
{
	uint32		lo = i + 1,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (SIFT_GID(w->keys[mid]) == SIFT_GID(w->keys[i]))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * The score entry of the group of postings [i, j), nib and esc being the L1 members and the escape bytes before it (advanced),
 * and its block bound row when it has one (returns whether it does)
 */
static pg_attribute_always_inline bool
group_score(const SiftPostingWriter *w, uint32 i, uint32 n, uint32 *end, uint32 *nib, uint32 *esc, SiftGroupScore *gs,
			uint8 *row)
{
	uint8		blk[SIFT_BLKS_PER_SB] = {0};
	bool		tf1 = true;
	uint32		k;

	*gs = (SiftGroupScore) {.nib_base = *nib,.esc_base = *esc};
	for (k = i; k < n && SIFT_GID(w->keys[k]) == SIFT_GID(w->keys[i]); k++)
	{
		int			b = (SIFT_BLK(w->keys[k]) & (SIFT_SB_PAGES - 1)) >> SIFT_BLK_SHIFT;

		blk[b] = Max(blk[b], w->bound[k]);
		if (w->tf[k] > 1)
		{
			tf1 = false;
			(*nib)++;
			*esc += w->tf[k] >= 17;
		}
	}
	for (int b = 0; b < SIFT_BLKS_PER_SB; b++)
		gs->bound = Max(gs->bound, blk[b]);
	gs->flags = tf1 ? SIFT_GS_ALL_TF1 : 0;
	*end = k;
	if (k - i < SIFT_BLKBOUND_MIN)
		return false;
	gs->flags |= SIFT_GS_BLKBOUND;
	memcpy(row, blk, SIFT_BLKS_PER_SB);
	return true;
}

/* Group scores and block bound rows of a GROUPED term, in codec group order */
static void
fill_groups(SiftPostingWriter *w, SiftGroupScore *gs, uint8 *rows)
{
	uint32		nib = 0,
				esc = 0;

	for (uint32 i = 0, j, g = 0; i < w->n; i = j, g++)
		rows += group_score(w, i, w->n, &j, &nib, &esc, &gs[g], rows) ? SIFT_BLKS_PER_SB : 0;
}

/*
 * Encodes the term's postings into w->out, filling info but for post_off;
 * returns the term blob, NULL for an inline term (which has none).
 */
static const uint8 *
postw_encode(SiftPostingWriter *w, SiftTermInfo *info)
{
	uint32		n = w->n,
				nesc = 0,
				ng = 0,
				nrows = 0;
	sift_enc_opts opts = {0};
	Size		clen,
				side,
				total,
				posoff = 0;
	uint8	   *out;

	if (n == 0)
		elog(ERROR, "sift: term without postings");
	memset(info, 0, sizeof(SiftTermInfo));
	info->df = n;
	for (uint32 i = 0; i < n; i++)
	{
		info->max_bound = Max(info->max_bound, w->bound[i]);
		info->nnib += w->tf[i] > 1;
		nesc += w->tf[i] >= 17;
	}
	if (info->nnib == 0)
		info->flags |= SIFT_TERM_ALL_TF1;
	if (n <= SIFT_INLINE_MAX)
	{
		info->flags |= SIFT_TERM_INLINE;
		memcpy(info->inline_keys, w->keys, n * sizeof(SiftKey));
		memcpy(info->inline_tf, w->tf, n);
		if (w->haspos && w->poslen <= SIFT_INLINE_POS_MAX)
		{
			info->flags |= SIFT_TERM_POS;
			info->pos_len = w->poslen;
			info->inline_pos = w->pos;
		}
		return NULL;
	}
	opts.kind = n >= w->grouped_min ? SIFT_GROUPED : SIFT_LIST;
	opts.ctype = w->ctype;
	if (opts.kind == SIFT_LIST)
		info->flags |= SIFT_TERM_SPARSE;
	else
	{
		for (uint32 i = 0, j; i < n; i = j, ng++)
		{
			for (j = i; j < n && SIFT_GID(w->keys[j]) == SIFT_GID(w->keys[i]); j++)
				;
			nrows += j - i >= SIFT_BLKBOUND_MIN;
		}
	}
	clen = sift_encode(w->keys, NULL, NULL, n, &opts, NULL);
	side = (Size) ng * sizeof(SiftGroupScore) + (Size) nrows * SIFT_BLKS_PER_SB +
		(info->nnib ? (n + 7) / 8 + (info->nnib + 1) / 2 + nesc : 0);
	total = side ? ALIGN4(clen) + side : clen;
	if (w->haspos)
	{
		posoff = total;
		total = posoff + (Size) ((n + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY) * sizeof(uint32) + w->poslen;
		info->flags |= SIFT_TERM_POS;
		info->pos_len = total - posoff;
	}
	if (total > SIFT_BLOB_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: postings of one term exceed 4 GB")));
	out = out_buf(w, total);
	if (sift_encode(w->keys, NULL, NULL, n, &opts, out) != clen)
		elog(ERROR, "sift: codec size mismatch");
	if (side)
	{
		uint8	   *p = out + ALIGN4(clen);

		if (ng)
		{
			fill_groups(w, (SiftGroupScore *) p, p + ng * sizeof(SiftGroupScore));
			p += ng * sizeof(SiftGroupScore) + nrows * SIFT_BLKS_PER_SB;
		}
		if (info->nnib)
		{
			SiftTfPos	pos = {0};
			uint8	   *nib = p + (n + 7) / 8;

			sift_kern_tf_encode(p, nib, nib + (info->nnib + 1) / 2, &pos, w->tf, n);
		}
	}
	if (w->haspos)
	{
		Size		nck = (Size) ((n + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY) * sizeof(uint32);

		memcpy(out + posoff, w->ckpt, nck);
		memcpy(out + posoff + nck, w->pos, w->poslen);
	}
	info->post_len = total;
	info->codec_len = clen;
	return out;
}

/* ---------- terms staged in temporary files ---------- */

static void
blob_too_big(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("sift: postings of one term exceed 4 GB")));
}

/* The containers of the GROUPED terms may be PACKED (segment format 3) */
void
sift_postw_pack(SiftPostingWriter *w)
{
	w->ctype = SIFT_CT_PACKED;
}

void
sift_postw_spool(SiftPostingWriter *w, uint64 min_postings)
{
	if (w->sw)
		w->spool_min = (uint32) Min(Max(min_postings, w->grouped_min), PG_INT32_MAX);
}

/* The term reached spool_min postings: its records and checkpoints so far go to the files */
static void
spool_begin(SiftPostingWriter *w)
{
	TermSpool  *sp = MemoryContextAllocZero(w->mcxt, sizeof(TermSpool));

	sp->payload = BufFileCreateTemp(false);
	sp->l1 = BufFileCreateTemp(false);
	sp->nib = BufFileCreateTemp(false);
	sp->esc = BufFileCreateTemp(false);
	sp->ckpt = BufFileCreateTemp(false);
	sp->pos = BufFileCreateTemp(false);
	w->sp = sp;
	for (w->open_start = w->n - 1; w->open_start > 0 && SIFT_GID(w->keys[w->open_start - 1]) == SIFT_GID(w->keys[w->n - 1]);)
		w->open_start--;
	if (w->haspos)
	{
		BufFileWrite(sp->ckpt, w->ckpt, (Size) ((w->n + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY) * sizeof(uint32));
		BufFileWrite(sp->pos, w->pos, w->poslen);
		pfree(w->pos);
		w->pos = NULL;
		w->poscap = 0;
	}
}

/* The tf of the next postings of the term to the three parts of its tf stream (sift_kern_tf_encode, a piece at a time) */
static void
spool_tf(TermSpool *sp, const uint8 *tf, uint32 n)
{
	uint8		l1[TF_PIECE / 8 + 1],
				nib[TF_PIECE / 2 + 1],
				esc[TF_PIECE];

	for (uint32 i = 0; i < n; i += TF_PIECE)
	{
		uint32		k = Min(n - i, TF_PIECE);
		SiftTfPos	at = {sp->nstaged % 8, sp->tfnib % 2, 0};

		memset(l1, 0, sizeof(l1));
		memset(nib, 0, sizeof(nib));
		l1[0] = sp->pend_l1;
		nib[0] = sp->pend_nib;
		sift_kern_tf_encode(l1, nib, esc, &at, tf + i, k);
		BufFileWrite(sp->l1, l1, at.rank / 8);
		BufFileWrite(sp->nib, nib, at.nib / 2);
		BufFileWrite(sp->esc, esc, at.esc);
		sp->pend_l1 = l1[at.rank / 8];
		sp->pend_nib = nib[at.nib / 2];
		sp->tfnib += at.nib - sp->tfnib % 2;
	}
}

/* Stages the group of postings [i, j) of the writer: its container, its score and its entries of the index of the blob */
static void
spool_group(SiftPostingWriter *w, uint32 i, uint32 j)
{
	TermSpool  *sp = w->sp;
	sift_enc_opts opts = {.kind = SIFT_GROUPED,.ctype = w->ctype};
	size_t		size = sift_encode(w->keys + i, NULL, NULL, j - i, &opts, NULL);
	uint8	   *blob = out_buf(w, size);
	uint32		plen;

	/* the term of one group: 8 bytes, ng, key, poff[2], base[2], and then the container */
	if (sift_encode(w->keys + i, NULL, NULL, j - i, &opts, blob) != size)
		elog(ERROR, "sift: codec size mismatch");
	memcpy(&plen, blob + 20, sizeof(plen));
	if (sp->ng == sp->maxg)
	{
		sp->maxg = Max(64, sp->maxg * 2);
		sp->key = grow(w->mcxt, sp->key, (Size) sp->maxg * sizeof(uint32));
		sp->poff = grow(w->mcxt, sp->poff, (Size) (sp->maxg + 1) * sizeof(uint32));
		sp->base = grow(w->mcxt, sp->base, (Size) (sp->maxg + 1) * sizeof(uint32));
		sp->gs = grow(w->mcxt, sp->gs, (Size) sp->maxg * sizeof(SiftGroupScore));
		sp->rows = grow(w->mcxt, sp->rows, (Size) sp->maxg * SIFT_BLKS_PER_SB);
	}
	if (20 + 12 * (uint64) (sp->ng + 1) + sp->npayload + plen > SIFT_BLOB_MAX)
		blob_too_big();
	memcpy(&sp->key[sp->ng], blob + 12, sizeof(uint32));
	sp->poff[sp->ng] = sp->npayload;
	sp->base[sp->ng] = sp->nstaged + i;
	BufFileWrite(sp->payload, blob + 32, plen);
	sp->npayload += plen;
	if (group_score(w, i, j, &j, &sp->nnib, &sp->nesc, &sp->gs[sp->ng], sp->rows + (Size) sp->nrows * SIFT_BLKS_PER_SB))
		sp->nrows++;
	sp->ng++;
}

/* Stages the first upto postings of the writer, whole groups, and keeps the rest */
static void
spool_stage(SiftPostingWriter *w, uint32 upto)
{
	TermSpool  *sp = w->sp;

	for (uint32 i = 0, j; i < upto; i = j)
		spool_group(w, i, j = group_end(w, i, upto));
	spool_tf(sp, w->tf, upto);
	for (uint32 i = 0; i < upto; i++)
		sp->max_bound = Max(sp->max_bound, w->bound[i]);
	sp->nstaged += upto;
	memmove(w->keys, w->keys + upto, (Size) (w->n - upto) * sizeof(SiftKey));
	memmove(w->tf, w->tf + upto, w->n - upto);
	memmove(w->bound, w->bound + upto, w->n - upto);
	w->n -= upto;
	w->open_start = 0;
}

/* The last groups of the term, and its blob out of the pieces, byte for byte what postw_encode makes of the term in memory */
static pg_noinline void
spool_end(SiftPostingWriter *w, SiftTermInfo *info)
{
	TermSpool  *sp;
	uint64		n,
				clen,
				total,
				posoff,
				nck;
	uint8		hdr[3 * sizeof(uint32)] = {SIFT_GROUPED};

	spool_stage(w, w->n);
	sp = w->sp;
	n = sp->nstaged;
	nck = (n + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY;
	memset(info, 0, sizeof(SiftTermInfo));
	info->df = n;
	info->max_bound = sp->max_bound;
	info->nnib = sp->nnib;
	if (sp->nnib == 0)
		info->flags |= SIFT_TERM_ALL_TF1;
	clen = 20 + 12 * (uint64) sp->ng + sp->npayload;
	total = ALIGN4(clen) + (uint64) sp->ng * sizeof(SiftGroupScore) + (uint64) sp->nrows * SIFT_BLKS_PER_SB +
		(sp->nnib ? (n + 7) / 8 + (sp->nnib + 1) / 2 + sp->nesc : 0);
	posoff = total;
	if (w->haspos)
	{
		total += nck * sizeof(uint32) + w->poslen;
		info->flags |= SIFT_TERM_POS;
		info->pos_len = total - posoff;
	}
	if (total > SIFT_BLOB_MAX)
		blob_too_big();
	info->post_len = total;
	info->codec_len = clen;
	sift_sw_write(w->sw, "\0\0\0", ALIGN4(sift_sw_tell(w->sw)) - sift_sw_tell(w->sw));
	info->post_off = sift_sw_tell(w->sw);
	memcpy(hdr + 4, &(uint32) {n}, sizeof(uint32));
	memcpy(hdr + 8, &sp->ng, sizeof(uint32));
	sp->poff[sp->ng] = sp->npayload;
	sp->base[sp->ng] = n;
	sift_sw_write(w->sw, hdr, sizeof(hdr));
	sift_sw_write(w->sw, sp->key, (Size) sp->ng * sizeof(uint32));
	sift_sw_write(w->sw, sp->poff, (Size) (sp->ng + 1) * sizeof(uint32));
	sift_sw_write(w->sw, sp->base, (Size) (sp->ng + 1) * sizeof(uint32));
	sift_sw_write_file(w->sw, sp->payload, sp->npayload);
	sift_sw_write(w->sw, "\0\0\0", ALIGN4(clen) - clen);
	sift_sw_write(w->sw, sp->gs, (Size) sp->ng * sizeof(SiftGroupScore));
	sift_sw_write(w->sw, sp->rows, (Size) sp->nrows * SIFT_BLKS_PER_SB);
	if (sp->nnib)
	{
		if (n % 8)
			BufFileWrite(sp->l1, &sp->pend_l1, 1);
		if (sp->nnib % 2)
			BufFileWrite(sp->nib, &sp->pend_nib, 1);
		sift_sw_write_file(w->sw, sp->l1, (n + 7) / 8);
		sift_sw_write_file(w->sw, sp->nib, (sp->nnib + 1) / 2);
		sift_sw_write_file(w->sw, sp->esc, sp->nesc);
	}
	if (w->haspos)
	{
		sift_sw_write_file(w->sw, sp->ckpt, nck * sizeof(uint32));
		sift_sw_write_file(w->sw, sp->pos, w->poslen);
	}
	BufFileClose(sp->payload);
	BufFileClose(sp->l1);
	BufFileClose(sp->nib);
	BufFileClose(sp->esc);
	BufFileClose(sp->ckpt);
	BufFileClose(sp->pos);
	pfree(sp->key);
	pfree(sp->poff);
	pfree(sp->base);
	pfree(sp->gs);
	pfree(sp->rows);
	pfree(sp);
	w->sp = NULL;
}

void
sift_postw_end_term(SiftPostingWriter *w, SiftTermInfo *info)
{
	if (w->sp)
		spool_end(w, info);
	else
	{
		const uint8 *out = postw_encode(w, info);

		if (out)
		{
			sift_sw_write(w->sw, "\0\0\0", ALIGN4(sift_sw_tell(w->sw)) - sift_sw_tell(w->sw));
			info->post_off = sift_sw_tell(w->sw);
			sift_sw_write(w->sw, out, info->post_len);
		}
	}
	w->n = 0;
	w->poslen = 0;
}

const uint8 *
sift_postw_end_term_mem(SiftPostingWriter *w, SiftTermInfo *info, MemoryContext mcxt)
{
	const uint8 *out = postw_encode(w, info);
	uint8	   *blob = NULL;

	if (out)
	{
		blob = MemoryContextAllocAligned(mcxt, (Size) info->post_len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
		memcpy(blob, out, info->post_len);
		memset(blob + info->post_len, 0, SIFT_KERN_PAD);
	}
	w->n = 0;
	w->poslen = 0;
	return blob;
}

/* ---------- reader ---------- */

/* the codec of the portable core (bm25/bitmap.c) raises through the host */
void
sift_codec_corrupt(const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("sift: corrupt posting data: %s", what)));
}

static void
corrupt_blob(const SiftTermInfo *info)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift: term blob at %llu does not match its dictionary entry",
					(unsigned long long) info->post_off)));
}

/* The length of the blob without its position stream */
static Size
blob_len(const SiftTermInfo *info)
{
	Size		poslen = (info->flags & SIFT_TERM_POS) ? info->pos_len : 0;

	if (poslen > info->post_len)
		corrupt_blob(info);
	return info->post_len - poslen;
}

#ifdef SIFT_TESTING
SiftPostStats sift_post_stats;
#define POST_STAT(f, v)		(sift_post_stats.f += (v))
#else
#define POST_STAT(f, v)		((void) 0)
#endif

/*
 * The buffer a cursor decodes its groups into. A sparse term (a LIST blob or inline keys) has a row per
 * page of a group, at most as many as it has postings (post_attach bounds their width); SIFT_CUR_BUF is for a group of
 * a GROUPED term
 */
static uint8 *
cursor_buf(MemoryContext mcxt, const SiftTermInfo *info)
{
	Size		size = SIFT_CUR_BUF;

	if (info->flags & (SIFT_TERM_INLINE | SIFT_TERM_SPARSE))
		size = Min(size, (Size) Min(info->df, SIFT_GROUP_PAGES) * SIFT_MAX_W + SIFT_KERN_PAD);
	POST_STAT(cursors, 1);
	POST_STAT(curbuf_bytes, size);
	return MemoryContextAllocAligned(mcxt, size, SIFT_SIMD_ALIGN, 0);
}

/* The cursor over a term blob with SIFT_KERN_PAD zero bytes behind it (NULL for an inline term); sr reads the positions */
static SiftPostingCursor *
post_attach(const SiftTermInfo *info, MemoryContext mcxt, const uint8 *blob, SiftStreamReader *sr, bool verify)
{
	SiftPostingCursor *c = MemoryContextAllocZero(mcxt, sizeof(SiftPostingCursor));
	const char *detail;

	c->info = *info;
	c->mcxt = mcxt;
	c->curbuf = cursor_buf(mcxt, info);
	if (blob == NULL)
	{
		if (!(info->flags & SIFT_TERM_INLINE) || info->df == 0 || info->df > SIFT_INLINE_MAX)
			corrupt_blob(info);
		for (uint32 i = 0; i < info->df; i++)
			if (SIFT_OFF(info->inline_keys[i]) - 1 >= SIFT_MAX_OFF)
				corrupt_blob(info);
		if (info->flags & SIFT_TERM_POS)
		{
			c->poslen = info->pos_len;
			c->info.inline_pos = memcpy(MemoryContextAlloc(mcxt, info->pos_len), info->inline_pos, info->pos_len);
		}
		sift_post_rewind(c);
		return c;
	}
	if (info->flags & SIFT_TERM_POS)
	{
		c->poslen = info->pos_len;
		c->nck = (info->df + SIFT_POS_EVERY - 1) / SIFT_POS_EVERY;
		if ((uint64) c->nck * sizeof(uint32) > info->pos_len)
			corrupt_blob(info);
	}
	c->bloblen = blob_len(info);
	c->sr = sr;
	c->posoff = info->post_off + c->bloblen;
	c->blob = blob;
	if (info->codec_len > c->bloblen || info->codec_len < 4 ||
		sift_blob_kind(blob) != ((info->flags & SIFT_TERM_SPARSE) ? SIFT_LIST : SIFT_GROUPED) ||
		sift_blob_n(blob) != info->df || ((info->flags & SIFT_TERM_SPARSE) && blob[1] > SIFT_LIST_OB_MAX))
		corrupt_blob(info);
	/* the codec follows the blob's own lengths, so verify checks them before any cursor does */
	if (verify && (detail = sift_blob_check(blob, info->codec_len)) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: term blob at %llu is malformed", (unsigned long long) info->post_off),
				 errdetail_internal("%s.", detail)));
	if (c->bloblen > info->codec_len)
	{
		const uint8 *p = blob + ALIGN4(info->codec_len);

		if (!(info->flags & SIFT_TERM_SPARSE))
		{
			c->ng = BLOB_NG(blob);
			if (ALIGN4(info->codec_len) > c->bloblen || (uint64) c->ng * sizeof(SiftGroupScore) > c->bloblen - ALIGN4(info->codec_len))
				corrupt_blob(info);
			c->gs = (const SiftGroupScore *) p;
			c->blkrows = p + c->ng * sizeof(SiftGroupScore);
			p = c->blkrows;
			for (uint32 i = 0; i < c->ng; i++)
				p += (c->gs[i].flags & SIFT_GS_BLKBOUND) ? SIFT_BLKS_PER_SB : 0;
		}
		c->l1 = p;
		c->nib = p + L1_BYTES(info);
		c->esc = c->nib + NIB_BYTES(info);
		/* a term with a tf stream has its L1 bits and nibbles inside the blob (the escape bytes are the rest); one without has none */
		if (p > blob + c->bloblen ||
			(!(info->flags & SIFT_TERM_ALL_TF1) &&
			 (info->nnib == 0 || (uint64) (blob + c->bloblen - p) < L1_BYTES(info) + NIB_BYTES(info))))
			corrupt_blob(info);
	}
	else if (!(info->flags & SIFT_TERM_SPARSE) || !(info->flags & SIFT_TERM_ALL_TF1))
		corrupt_blob(info);
	sift_post_rewind(c);
	return c;
}

static SiftPostingCursor *
post_open(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt, bool verify)
{
	uint8	   *blob = NULL;

	if (!(info->flags & SIFT_TERM_INLINE))
	{
		Size		len = blob_len(info);
		const void *src;

		POST_STAT(blobs, 1);
		POST_STAT(blob_bytes, len + SIFT_KERN_PAD);
		blob = MemoryContextAllocAligned(mcxt, len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, MCXT_ALLOC_HUGE);
		src = sift_sr_get(sr, info->post_off, len, blob);
		if (src != blob)
			memcpy(blob, src, len);
		memset(blob + len, 0, SIFT_KERN_PAD);
	}
	return post_attach(info, mcxt, blob, sr, verify);
}

SiftPostingCursor *
sift_post_open(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt)
{
	return post_open(sr, info, mcxt, false);
}

SiftPostingCursor *
sift_post_open_blob(const SiftTermInfo *info, const uint8 *blob, MemoryContext mcxt)
{
	return post_attach(info, mcxt, blob, NULL, false);
}

SiftPostingCursor *
sift_post_open_verified(SiftStreamReader *sr, const SiftTermInfo *info, MemoryContext mcxt)
{
	return post_open(sr, info, mcxt, true);
}

SiftPostingCursor *
sift_post_clone(const SiftPostingCursor *src, MemoryContext mcxt)
{
	SiftPostingCursor *c = MemoryContextAlloc(mcxt, sizeof(SiftPostingCursor));

	*c = *src;
	c->mcxt = mcxt;
	c->bounds = NULL;
	c->ckpt = NULL;
	c->win = NULL;
	c->win_len = 0;
	c->curbuf = cursor_buf(mcxt, &src->info);
	sift_post_rewind(c);
	return c;
}

sift_cur *
sift_post_cursor(SiftPostingCursor *c)
{
	return &c->cur;
}

void
sift_post_group_stats(const SiftPostingCursor *c, uint32 *ng, uint32 *nrows)
{
	*ng = c->gs ? c->ng : 0;
	*nrows = 0;
	for (uint32 i = 0; c->gs && i < c->ng; i++)
		*nrows += (c->gs[i].flags & SIFT_GS_BLKBOUND) != 0;
}

void
sift_post_rewind(SiftPostingCursor *c)
{
	if (c->blob)
		sift_cur_blob(&c->cur, c->blob, c->info.codec_len, c->curbuf);
	else
		sift_cur_tids(&c->cur, c->info.inline_keys, c->info.df, c->curbuf);
	memset(&c->next, 0, sizeof(SiftTfPos));
}

const SiftGroupBounds *
sift_post_bounds(SiftPostingCursor *c, int *n)
{
	if (c->bounds == NULL && c->gs)
	{
		const uint8 *row = c->blkrows;

		c->bounds = MemoryContextAllocHuge(c->mcxt, Max(c->ng, 1) * sizeof(SiftGroupBounds));
		for (uint32 i = 0; i < c->ng; i++)
		{
			SiftGroupBounds *b = &c->bounds[i];

			b->gid = BLOB_KEYS(c->blob)[i] >> 8;
			b->bound = c->gs[i].bound;
			if (c->gs[i].flags & SIFT_GS_BLKBOUND)
			{
				memcpy(b->blk_bound, row, SIFT_BLKS_PER_SB);
				row += SIFT_BLKS_PER_SB;
			}
			else
				memset(b->blk_bound, b->bound, SIFT_BLKS_PER_SB);
		}
		c->nbounds = c->ng;
	}
	else if (c->bounds == NULL)
	{
		uint32	   *gid = palloc(Max(c->info.df, 1) * sizeof(uint32));
		uint32		lo = 0,
					hi = 0;

		sift_post_rewind(c);
		if (c->blob && c->info.df > SIFT_SPAN_MIN_DF)
			sift_cur_gid_span(&c->cur, &lo, &hi);
		if (c->info.df > hi - lo && c->info.df > SIFT_SPAN_MIN_DF)
		{
			/* at least a posting per SB on average: every SB between the first and the last, decoding nothing */
			c->nbounds = hi - lo + 1;
			for (uint32 i = 0; i < c->nbounds; i++)
				gid[i] = lo + i;
		}
		else
			c->nbounds = sift_cur_gids(&c->cur, gid);
		c->bounds = MemoryContextAlloc(c->mcxt, Max(c->nbounds, 1) * sizeof(SiftGroupBounds));
		for (uint32 i = 0; i < c->nbounds; i++)
		{
			c->bounds[i].gid = gid[i];
			c->bounds[i].bound = c->info.max_bound;
			memset(c->bounds[i].blk_bound, c->info.max_bound, SIFT_BLKS_PER_SB);
		}
		pfree(gid);
		sift_post_rewind(c);
	}
	*n = c->nbounds;
	return c->bounds;
}

/* Escape nibbles (15) among the first n nibbles, 16 per step */
static uint32
escapes(const uint8 *nib, uint32 n)
{
	uint32		esc = 0,
				i = 0;

	for (; i + 16 <= n; i += 16)
	{
		uint64		w;

		memcpy(&w, nib + i / 2, 8);
		w &= w >> 1;
		w &= w >> 2;
		esc += sift_pc64(w & 0x1111111111111111ULL);
	}
	for (; i < n; i++)
		esc += ((nib[i >> 1] >> ((i & 1) * 4)) & 15) == 15;
	return esc;
}

/* Escape nibbles among the n nibbles from nibble index a */
static uint32
escapes_from(const uint8 *nib, uint32 a, uint32 n)
{
	uint32		esc = 0;

	if ((a & 1) && n)
		esc = ((nib[a >> 1] >> 4) & 15) == 15, a++, n--;
	return esc + escapes(nib + a / 2, n);
}

/* Set bits among the n of l1 from bit r on: the nibbles the tf stream holds for those ranks */
static uint32
l1_count(const uint8 *l1, uint32 r, uint32 n)
{
	uint32		cnt = 0,
				head = (r % 8) ? Min(8 - r % 8, n) : 0;

	if (head)
		cnt = sift_pc64((l1[r / 8] >> (r % 8)) & ((1u << head) - 1)), r += head, n -= head;
	cnt += sift_popcount(l1 + r / 8, n / 8);
	if (n % 8)
		cnt += sift_pc64(l1[(r + n) / 8] & ((1u << (n % 8)) - 1));
	return cnt;
}

/* Stream position of rank r, counted from the start (LIST random access) */
static SiftTfPos
list_tf_pos(const SiftPostingCursor *c, uint32 r)
{
	SiftTfPos	pos = {.rank = r};

	pos.nib = sift_popcount(c->l1, r / 8);
	if (r % 8)
		pos.nib += sift_pc64(c->l1[r / 8] & ((1u << (r % 8)) - 1));
	if (pos.nib > c->info.nnib)
		sift_codec_corrupt("tf stream: the L1 bits count more nibbles than the stream holds");
	pos.esc = escapes(c->nib, pos.nib);
	return pos;
}

/*
 * The kernels read 64 bytes at the nibble and escape positions they reach, so the group's n ranks from pos must find the nibbles
 * their L1 bits count and the escape bytes of their escape nibbles inside the stream (the escapes are counted only when the
 * stream could end before them: the stream holds one escape byte per escape nibble of the term)
 */
static void
tf_check(const SiftPostingCursor *c, const SiftTfPos *pos, uint32 n)
{
	const SiftTermInfo *info = &c->info;
	uint64		elen = c->blob + c->bloblen - c->esc;
	uint32		nn;

	if (pos->nib > info->nnib)
		sift_codec_corrupt("tf stream: a group starts past the nibbles");
	nn = l1_count(c->l1, pos->rank, n);
	if (nn > info->nnib - pos->nib)
		sift_codec_corrupt("tf stream: the nibbles of a group are not inside the stream");
	if (pos->esc > elen || (nn > elen - pos->esc && escapes_from(c->nib, pos->nib, nn) > elen - pos->esc))
		sift_codec_corrupt("tf stream: the escape bytes of a group are not inside the stream");
}

void
sift_post_group_tf(SiftPostingCursor *c, uint8 *tf)
{
	const sift_grp *g = &c->cur.g;
	uint32		n = g->n ? g->n : (uint32) sift_grp_count(g);
	SiftTfPos	pos;

	if (c->blob == NULL)
	{
		memcpy(tf, c->info.inline_tf + g->base, n);
		return;
	}
	if ((c->info.flags & SIFT_TERM_ALL_TF1) || (c->gs && (c->gs[c->cur.gr.i].flags & SIFT_GS_ALL_TF1)))
	{
		memset(tf, 1, n);
		return;
	}
	if ((uint64) g->base + n > c->info.df)
		sift_codec_corrupt("tf stream: a group lies past the last posting");
	if (c->gs)
		pos = (SiftTfPos) {g->base, c->gs[c->cur.gr.i].nib_base, c->gs[c->cur.gr.i].esc_base};
	else
		pos = c->next.rank == g->base ? c->next : list_tf_pos(c, g->base);
	tf_check(c, &pos, n);
	sift_kern->tf_decode(tf, c->l1, c->nib, c->esc, &pos, n);
	c->next = pos;
}

static inline uint32
nibble(const uint8 *nib, uint32 i)
{
	return (nib[i >> 1] >> ((i & 1) * 4)) & 15;
}

void
sift_post_check(SiftPostingCursor *c)
{
	const SiftTermInfo *info = &c->info;
	const uint8 *end = c->blob + c->bloblen;
	const uint32 *base = c->gs ? BLOB_KEYS(c->blob) + 2 * c->ng + 1 : NULL;
	uint32		nib = 0,
				esc = 0;

	if (c->blob == NULL)
	{
		for (uint32 i = 0; i < info->df; i++)
			if (info->inline_tf[i] == 0 || (i > 0 && info->inline_keys[i] <= info->inline_keys[i - 1]) ||
				((info->flags & SIFT_TERM_ALL_TF1) && info->inline_tf[i] != 1))
				corrupt_blob(info);
		return;
	}
	if ((info->flags & SIFT_TERM_ALL_TF1) ? info->nnib != 0 || (c->l1 && c->l1 != end) : c->l1 == NULL || info->nnib == 0)
		corrupt_blob(info);
	if (c->gs && BLOB_NG(c->blob) != c->ng)
		corrupt_blob(info);
	for (uint32 g = 0, r = 0; g < Max(c->ng, 1); g++)
	{
		uint32		hi = c->gs ? base[g + 1] : info->df;
		bool		tf1 = true;

		if (c->gs && (base[g] != r || hi > info->df || c->gs[g].nib_base != nib || c->gs[g].esc_base != esc))
			corrupt_blob(info);
		for (; r < hi && !(info->flags & SIFT_TERM_ALL_TF1); r++)
			if ((c->l1[r >> 3] >> (r & 7)) & 1)
			{
				if (nib >= info->nnib)
					corrupt_blob(info);
				tf1 = false;
				esc += nibble(c->nib, nib++) == 15;
			}
		r = hi;
		if (c->gs && tf1 != ((c->gs[g].flags & SIFT_GS_ALL_TF1) != 0))
			corrupt_blob(info);
	}
	if (!(info->flags & SIFT_TERM_ALL_TF1) &&
		(nib != info->nnib || c->esc + esc != end ||
		 (info->df % 8 && c->l1[info->df / 8] >> (info->df % 8)) ||
		 (info->nnib % 2 && nibble(c->nib, info->nnib) != 0)))
		corrupt_blob(info);
}

/* ---------- positions ---------- */

static void
corrupt_pos(const SiftPostingCursor *c)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("sift: position stream of the term blob at %llu is malformed",
					(unsigned long long) c->info.post_off)));
}

/* LEB128 at *pp; false when it runs past end */
static inline bool
get_v32(const uint8 **pp, const uint8 *end, uint32 *x)
{
	const uint8 *p = *pp;
	uint32		v = 0;

	for (int s = 0; s < 35; s += 7)
	{
		if (p >= end)
			return false;
		v |= (uint32) (*p & 0x7f) << s;
		if (!(*p++ & 0x80))
		{
			*pp = p;
			*x = v;
			return true;
		}
	}
	return false;
}

/* End of the record at p and what it holds; NULL when it is cut off by end */
static const uint8 *
pos_next(const uint8 *p, const uint8 *end, uint32 *n, uint32 *last)
{
	uint32		v,
				cnt = 1,
				pos;

	if (!get_v32(&p, end, &v))
		return NULL;
	pos = v >> 1;
	if (v & 1)
	{
		if (!get_v32(&p, end, &v) || (uint64) v + 1 > (uint64) (end - p))
			return NULL;
		cnt = v + 2;
	}
	for (uint32 i = 1; i < cnt; i++)
	{
		if (!get_v32(&p, end, &v))
			return NULL;
		pos += v + 1;
	}
	*n = cnt;
	*last = pos;
	return p;
}

const uint8 *
sift_pos_skip(const uint8 *p, const uint8 *end, uint32 *n, uint32 *last)
{
	const uint8 *q = pos_next(p, end, n, last);

	if (q == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: truncated position record")));
	return q;
}

bool
sift_post_has_positions(const SiftPostingCursor *c)
{
	return c->poslen > 0;
}

/* Reads len bytes of the position stream at off into dst, and lets go of the page */
static void
pos_read(const SiftPostingCursor *c, uint64 off, size_t len, void *dst)
{
	const void *src = sift_sr_get(c->sr, c->posoff + off, len, dst);

	if (src != dst)
		memcpy(dst, src, len);
	sift_sr_release(c->sr);
}

#define POS_WINDOW	4096

/* The window of records covering [off, off + want), refilled from off when it does not; bytes available from off */
static const uint8 *
pos_window(SiftPostingCursor *c, uint64 off, uint32 want, uint32 *avail)
{
	uint64		recs = c->poslen - (uint64) c->nck * sizeof(uint32);

	if (off > recs)
		corrupt_pos(c);
	if (c->win == NULL)
		c->win = MemoryContextAlloc(c->mcxt, POS_WINDOW);
	if (c->win_len == 0 || off < c->win_lo || (off + want > c->win_lo + c->win_len && c->win_lo + c->win_len < recs))
	{
		uint64		have = Min(recs - off, POS_WINDOW);

		if (want > POS_WINDOW)
		{
			/* a record longer than the window: the caller reads it whole */
			*avail = 0;
			return NULL;
		}
		pos_read(c, (uint64) c->nck * sizeof(uint32) + off, have, c->win);
		c->win_lo = off;
		c->win_len = have;
	}
	*avail = c->win_lo + c->win_len - off;
	return c->win + (off - c->win_lo);
}

static void
pos_ckpt(SiftPostingCursor *c)
{
	if (c->ckpt)
		return;
	c->ckpt = MemoryContextAlloc(c->mcxt, (Size) c->nck * sizeof(uint32));
	pos_read(c, 0, (Size) c->nck * sizeof(uint32), c->ckpt);
}

/* Appends the n occurrences of the record at p (fully inside end) to out */
static void
pos_decode(const uint8 *p, const uint8 *end, uint32 n, SiftPosBuf *out)
{
	uint32		v = 0,
				pos;

	if (n > out->cap)
	{
		out->cap = Max(n, Max(64, out->cap * 2));
		out->v = out->v ? repalloc_huge(out->v, (Size) out->cap * sizeof(uint32)) :
			MemoryContextAllocHuge(out->mcxt, (Size) out->cap * sizeof(uint32));
	}
	get_v32(&p, end, &v);
	pos = v >> 1;
	if (v & 1)
		get_v32(&p, end, &v);
	out->v[0] = pos;
	for (uint32 i = 1; i < n; i++)
	{
		get_v32(&p, end, &v);
		out->v[i] = pos += v + 1;
	}
	out->n = n;
}

void
sift_post_positions(SiftPostingCursor *c, uint32 rank, SiftPosBuf *out)
{
	uint64		recs = c->poslen - (uint64) c->nck * sizeof(uint32),
				off;
	uint32		want = 512;

	Assert(c->poslen > 0 && rank < c->info.df);
	if (c->info.flags & SIFT_TERM_INLINE)
	{
		const uint8 *p = c->info.inline_pos,
				   *end = p + c->poslen;
		uint32		n,
					last;

		for (uint32 k = 0; k < rank; k++)
			p = sift_pos_skip(p, end, &n, &last);
		end = sift_pos_skip(p, end, &n, &last);
		pos_decode(p, end, n, out);
		return;
	}
	pos_ckpt(c);
	off = c->ckpt[rank >> SIFT_POS_SHIFT];
	for (;; want *= 2)
	{
		uint32		avail,
					n = 0,
					last;
		const uint8 *p = pos_window(c, off, Min(want, recs - off), &avail),
				   *end,
				   *q;
		uint8	   *big = NULL;

		if (p == NULL)
		{
			/* longer than the window: read what is wanted at once */
			avail = Min(want, recs - off);
			big = palloc(avail);
			pos_read(c, (uint64) c->nck * sizeof(uint32) + off, avail, big);
			p = big;
		}
		end = p + avail;
		q = p;
		for (uint32 k = rank & (SIFT_POS_EVERY - 1); k > 0 && q; k--)
			q = pos_next(q, end, &n, &last);
		if (q && (end = pos_next(q, end, &n, &last)) != NULL)
		{
			pos_decode(q, end, n, out);
			if (big)
				pfree(big);
			return;
		}
		if (big)
			pfree(big);
		if (avail >= recs - off)
			corrupt_pos(c);
	}
}

void
sift_post_pos_stream(SiftPostingCursor *c, const uint32 **ckpt, const uint8 **rec, uint32 *len)
{
	uint8	   *buf;

	if (c->info.flags & SIFT_TERM_INLINE)
	{
		*ckpt = NULL;
		*rec = c->info.inline_pos;
		*len = c->poslen;
		return;
	}
	buf = MemoryContextAllocHuge(c->mcxt, c->poslen);
	pos_read(c, 0, c->poslen, buf);
	*ckpt = (const uint32 *) buf;
	*rec = buf + (Size) c->nck * sizeof(uint32);
	*len = c->poslen - c->nck * sizeof(uint32);
}

struct SiftPosReader
{
	SiftPostingCursor *c;
	uint8	   *buf;			/* the window; the records of an inline term themselves */
	Size		cap;
	Size		pos;			/* of the next record in the window */
	Size		len;			/* bytes in the window */
	uint64		next;			/* offset in the records of the byte after the window */
	uint64		recs;			/* bytes of records */
};

#define POS_READER_WINDOW	65536

SiftPosReader *
sift_post_pos_reader(SiftPostingCursor *c, MemoryContext mcxt)
{
	SiftPosReader *r = MemoryContextAllocZero(mcxt, sizeof(SiftPosReader));

	r->c = c;
	if (c->info.flags & SIFT_TERM_INLINE)
	{
		r->buf = (uint8 *) c->info.inline_pos;
		r->len = r->next = r->recs = c->poslen;
		return r;
	}
	r->recs = c->poslen - (uint64) c->nck * sizeof(uint32);
	r->cap = POS_READER_WINDOW;
	r->buf = MemoryContextAlloc(mcxt, r->cap);
	return r;
}

const uint8 *
sift_post_pos_next(SiftPosReader *r, uint32 *len)
{
	for (;;)
	{
		const uint8 *rec = r->buf + r->pos;
		uint32		n,
					last;
		const uint8 *end = pos_next(rec, r->buf + r->len, &n, &last);
		Size		keep = r->len - r->pos,
					have;

		if (end)
		{
			*len = end - rec;
			r->pos += *len;
			return rec;
		}
		if (r->next >= r->recs)
			corrupt_pos(r->c);
		/* the record is cut off by the window: its start moves to the front and the window is refilled (a record longer than the window grows it) */
		if (keep == r->cap)
			r->buf = repalloc(r->buf, r->cap *= 2);
		memmove(r->buf, r->buf + r->pos, keep);
		have = Min(r->recs - r->next, r->cap - keep);
		pos_read(r->c, (uint64) r->c->nck * sizeof(uint32) + r->next, have, r->buf + keep);
		r->next += have;
		r->pos = 0;
		r->len = keep + have;
	}
}
