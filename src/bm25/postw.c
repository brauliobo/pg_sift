/*
 * bm25/postw.c - term blob writer: encodes the postings of a term, staging large terms in temporary files
 * (layout in bm25/posting.h).
 */
#include "core/host.h"

#include "bm25/kernels.h"
#include "bm25/posting.h"
#include "bm25/posting_int.h"
#include "core/cmp.h"

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

	SIFT_BISECT(uint32, mid, lo, hi, SIFT_GID(w->keys[mid]) == SIFT_GID(w->keys[i]));
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
