/*
 * sift_dict.c - term dictionary writer, reader and expansion matchers
 * (layout in sift_dict.h).
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "regex/regex.h"
#include "storage/buffile.h"
#include "utils/memutils.h"

#include "sift_dict.h"

#define ALIGN4(x)		(((x) + 3) & ~(uint64) 3)
#define BLOCK_HDR_MAX	(2 + 1 + 10)
#define SPOOL_CHUNK		65536

#define corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift dictionary is corrupted"), \
			 errdetail(__VA_ARGS__)))

struct SiftDictWriter
{
	SiftStreamWriter *sw;
	BufFile    *spool;			/* DICT bytes */
	uint64		dict_len;
	StringInfoData blk;			/* entries of the open block */
	int			nblk;
	uint64		blk_post;		/* post_base of the open block */
	uint64		post_end;		/* end of the last term blob */
	char		prev[SIFT_MAX_TERM_BYTES];
	int			prevlen;
	uint64		nterms;
	SiftDictIdx *idx;
	uint64		nidx;
	uint64		maxidx;
};

struct SiftDict
{
	SiftStreamReader *sr;
	uint64		dict_off;
	uint64		dict_len;
	uint64		idx_off;
	uint64		nblocks;
	uint64		nterms;
	uint64	   *root;
	uint32		nroot;
	uint8	   *blk;			/* copy of the current block */
	MemoryContext mcxt;
};

/* Decoding position inside a copied block */
typedef struct BlockIter
{
	const uint8 *p;
	const uint8 *end;
	int			left;
	uint64		post;
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;
} BlockIter;

static uint8 *
put_varint(uint8 *p, uint64 x)
{
	for (; x >= 0x80; x >>= 7)
		*p++ = (uint8) (x | 0x80);
	*p++ = (uint8) x;
	return p;
}

static const uint8 *
get_varint(const uint8 *p, const uint8 *end, uint64 *x)
{
	*x = 0;
	for (int s = 0; s < 64; s += 7)
	{
		if (p >= end)
			break;
		*x |= (uint64) (*p & 0x7f) << s;
		if (!(*p++ & 0x80))
			return p;
	}
	corrupt("Truncated varint in a dictionary block.");
	pg_unreachable();
}

static uint64
prefix8(const char *s, int len)
{
	uint64		p = 0;

	for (int i = 0; i < 8; i++)
		p = p << 8 | (i < len ? (uint8) s[i] : 0);
	return p;
}

static int
term_cmp(const char *a, int alen, const char *b, int blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	return c ? c : (alen > blen) - (alen < blen);
}

/* ---------- writer ---------- */

SiftDictWriter *
sift_dict_writer_begin(SiftStreamWriter *sw)
{
	SiftDictWriter *w = palloc0(sizeof(SiftDictWriter));

	w->sw = sw;
	w->spool = BufFileCreateTemp(false);
	w->post_end = sift_sw_tell(sw);
	w->prevlen = -1;
	initStringInfo(&w->blk);
	w->maxidx = 1024;
	w->idx = palloc(w->maxidx * sizeof(SiftDictIdx));
	return w;
}

static void
flush_block(SiftDictWriter *w)
{
	uint8		hdr[BLOCK_HDR_MAX];
	uint8	   *p = put_varint(hdr + 3, w->blk_post);
	uint32		len = (p - hdr) + w->blk.len;

	if (w->nblk == 0)
		return;
	hdr[0] = len & 0xff;
	hdr[1] = len >> 8;
	hdr[2] = w->nblk;
	BufFileWrite(w->spool, hdr, p - hdr);
	BufFileWrite(w->spool, w->blk.data, w->blk.len);
	w->dict_len += len;
	resetStringInfo(&w->blk);
	w->nblk = 0;
}

void
sift_dict_add(SiftDictWriter *w, const char *term, int len, const SiftTermInfo *info)
{
	uint8		e[2 + SIFT_MAX_TERM_BYTES + 64];
	uint8	   *p = e;
	int			shared = 0;
	bool		inl = (info->flags & SIFT_TERM_INLINE) != 0;

	if (len < 1 || len > SIFT_MAX_TERM_BYTES ||
		(w->prevlen >= 0 && term_cmp(w->prev, w->prevlen, term, len) >= 0))
		elog(ERROR, "sift: dictionary terms must be non-empty, at most %d bytes and strictly ascending",
			 SIFT_MAX_TERM_BYTES);
	if (!inl && info->post_off != ALIGN4(w->post_end))
		elog(ERROR, "sift: term blob at %llu, expected %llu",
			 (unsigned long long) info->post_off, (unsigned long long) ALIGN4(w->post_end));
	if (w->nblk == 0)
	{
		if (w->nidx == w->maxidx)
		{
			w->maxidx *= 2;
			w->idx = repalloc_huge(w->idx, w->maxidx * sizeof(SiftDictIdx));
		}
		if (w->dict_len > PG_UINT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift: segment dictionary exceeds 4 GB")));
		w->idx[w->nidx++] = (SiftDictIdx) {prefix8(term, len), (uint32) w->dict_len, 0};
		w->blk_post = ALIGN4(w->post_end);
	}
	else
		while (shared < Min(len, w->prevlen) && term[shared] == w->prev[shared])
			shared++;
	*p++ = shared;
	*p++ = len - shared;
	memcpy(p, term + shared, len - shared);
	p += len - shared;
	p = put_varint(p, info->df);
	*p++ = info->flags;
	*p++ = info->max_bound;
	if (inl)
	{
		p = put_varint(p, info->inline_keys[0]);
		if (info->df > 1)
			p = put_varint(p, info->inline_keys[1] - info->inline_keys[0]);
		if (!(info->flags & SIFT_TERM_ALL_TF1))
			for (uint32 i = 0; i < info->df; i++)
				*p++ = info->inline_tf[i];
	}
	else
	{
		p = put_varint(p, info->post_len);
		p = put_varint(p, info->post_len - info->codec_len);
		if (!(info->flags & SIFT_TERM_ALL_TF1))
			p = put_varint(p, info->nnib);
		w->post_end = info->post_off + info->post_len;
	}
	appendBinaryStringInfo(&w->blk, e, p - e);
	memcpy(w->prev, term, len);
	w->prevlen = len;
	w->nterms++;
	if (++w->nblk == SIFT_DICT_BLOCK)
		flush_block(w);
}

void
sift_dict_writer_finish(SiftDictWriter *w, SiftSegHeaderData *hdr)
{
	char	   *buf = palloc(SPOOL_CHUNK);

	flush_block(w);
	hdr->nterms = w->nterms;
	hdr->sections[SIFT_SEC_DICT].off = sift_sw_tell(w->sw);
	hdr->sections[SIFT_SEC_DICT].len = w->dict_len;
	if (BufFileSeek(w->spool, 0, 0, SEEK_SET) != 0)
		elog(ERROR, "sift: could not rewind the dictionary spool");
	for (uint64 left = w->dict_len; left > 0;)
	{
		size_t		n = Min(left, SPOOL_CHUNK);

		BufFileReadExact(w->spool, buf, n);
		sift_sw_write(w->sw, buf, n);
		left -= n;
	}
	BufFileClose(w->spool);
	hdr->sections[SIFT_SEC_DICTIDX].off = sift_sw_tell(w->sw);
	hdr->sections[SIFT_SEC_DICTIDX].len = w->nidx * sizeof(SiftDictIdx);
	sift_sw_write(w->sw, w->idx, w->nidx * sizeof(SiftDictIdx));
	hdr->dictroot_off = sift_sw_tell(w->sw);
	hdr->dictroot_n = (w->nidx + SIFT_DICT_ROOT_FANOUT - 1) / SIFT_DICT_ROOT_FANOUT;
	for (uint64 i = 0; i < w->nidx; i += SIFT_DICT_ROOT_FANOUT)
		sift_sw_write(w->sw, &w->idx[i].prefix, sizeof(uint64));
	pfree(buf);
	pfree(w->idx);
	pfree(w->blk.data);
}

/* ---------- reader ---------- */

SiftDict *
sift_dict_open(SiftStreamReader *sr, const SiftSegHeaderData *hdr, MemoryContext mcxt)
{
	SiftDict   *d = MemoryContextAllocZero(mcxt, sizeof(SiftDict));
	const SiftSectionRef *dict = &hdr->sections[SIFT_SEC_DICT];
	const SiftSectionRef *idx = &hdr->sections[SIFT_SEC_DICTIDX];
	Size		rootlen = (Size) hdr->dictroot_n * sizeof(uint64);
	const void *src;

	d->sr = sr;
	d->mcxt = mcxt;
	d->dict_off = dict->off;
	d->dict_len = dict->len;
	d->idx_off = idx->off;
	d->nblocks = idx->len / sizeof(SiftDictIdx);
	d->nterms = hdr->nterms;
	d->nroot = hdr->dictroot_n;
	if (idx->len % sizeof(SiftDictIdx) != 0 || idx->off != dict->off + dict->len ||
		hdr->dictroot_off != idx->off + idx->len ||
		d->nroot != (d->nblocks + SIFT_DICT_ROOT_FANOUT - 1) / SIFT_DICT_ROOT_FANOUT ||
		d->nblocks != (d->nterms + SIFT_DICT_BLOCK - 1) / SIFT_DICT_BLOCK)
		corrupt("Sections DICT %llu+%llu, DICTIDX %llu+%llu, root %llu (%u) for %llu terms.",
				(unsigned long long) dict->off, (unsigned long long) dict->len,
				(unsigned long long) idx->off, (unsigned long long) idx->len,
				(unsigned long long) hdr->dictroot_off, d->nroot, (unsigned long long) d->nterms);
	d->root = MemoryContextAllocHuge(mcxt, Max(rootlen, 1));
	src = sift_sr_get(sr, hdr->dictroot_off, rootlen, d->root);
	if (src != d->root)
		memcpy(d->root, src, rootlen);
	d->blk = MemoryContextAlloc(mcxt, UINT16_MAX + 1);
	return d;
}

static SiftDictIdx
read_idx(SiftDict *d, uint64 b)
{
	SiftDictIdx e;
	const void *src = sift_sr_get(d->sr, d->idx_off + b * sizeof(SiftDictIdx), sizeof(e), &e);

	if (src != &e)
		memcpy(&e, src, sizeof(e));
	return e;
}

/* Copies the block at offset off of DICT into buf (UINT16_MAX + 1 bytes) and starts decoding it */
static void
block_begin(SiftDict *d, uint64 off, uint8 *buf, BlockIter *it)
{
	const uint8 *src;
	uint32		len;
	uint64		post;

	if (off + 3 > d->dict_len)
		corrupt("Block at %llu lies past the dictionary end %llu.",
				(unsigned long long) off, (unsigned long long) d->dict_len);
	src = sift_sr_get(d->sr, d->dict_off + off, 2, buf);
	len = src[0] | src[1] << 8;
	if (len < 4 || off + len > d->dict_len)
		corrupt("Block at %llu has length %u.", (unsigned long long) off, len);
	src = sift_sr_get(d->sr, d->dict_off + off, len, buf);
	if (src != buf)
		memcpy(buf, src, len);
	it->end = buf + len;
	it->left = buf[2];
	it->p = get_varint(buf + 3, it->end, &post);
	it->post = post;
	it->len = 0;
	if (it->left < 1 || it->left > SIFT_DICT_BLOCK)
		corrupt("Block at %llu holds %d terms.", (unsigned long long) off, it->left);
}

static bool
block_next(BlockIter *it, SiftTermInfo *info)
{
	uint64		v;
	int			shared,
				suffix;

	if (it->left == 0)
	{
		if (it->p != it->end)
			corrupt("Dictionary block has %d trailing bytes.", (int) (it->end - it->p));
		return false;
	}
	it->left--;
	if (it->end - it->p < 2)
		corrupt("Truncated dictionary entry.");
	shared = it->p[0];
	suffix = it->p[1];
	it->p += 2;
	if (shared > it->len || shared + suffix > SIFT_MAX_TERM_BYTES || suffix == 0 ||
		it->end - it->p < suffix + 2)
		corrupt("Dictionary entry with prefix %d and suffix %d after a term of %d bytes.",
				shared, suffix, it->len);
	memcpy(it->term + shared, it->p, suffix);
	it->len = shared + suffix;
	it->p += suffix;
	memset(info, 0, sizeof(SiftTermInfo));
	it->p = get_varint(it->p, it->end, &v);
	info->df = v;
	if (it->end - it->p < 2)
		corrupt("Truncated dictionary entry.");
	info->flags = *it->p++;
	info->max_bound = *it->p++;
	if (info->flags & SIFT_TERM_INLINE)
	{
		if (info->df < 1 || info->df > SIFT_INLINE_MAX)
			corrupt("Inline term with df %u.", info->df);
		it->p = get_varint(it->p, it->end, &v);
		info->inline_keys[0] = v;
		if (info->df > 1)
		{
			it->p = get_varint(it->p, it->end, &v);
			info->inline_keys[1] = info->inline_keys[0] + v;
		}
		if (info->flags & SIFT_TERM_ALL_TF1)
			memset(info->inline_tf, 1, info->df);
		else
		{
			if (it->end - it->p < info->df)
				corrupt("Truncated inline postings.");
			memcpy(info->inline_tf, it->p, info->df);
			it->p += info->df;
		}
		return true;
	}
	it->p = get_varint(it->p, it->end, &v);
	info->post_len = v;
	it->p = get_varint(it->p, it->end, &v);
	if (v > info->post_len)
		corrupt("Sidecar of %llu bytes in a term blob of %u.", (unsigned long long) v, info->post_len);
	info->codec_len = info->post_len - v;
	if (!(info->flags & SIFT_TERM_ALL_TF1))
	{
		it->p = get_varint(it->p, it->end, &v);
		info->nnib = v;
	}
	info->post_off = ALIGN4(it->post);
	it->post = info->post_off + info->post_len;
	return true;
}

/*
 * Index of the block where term would be: the last block whose first term
 * is <= term, or -1 when term precedes every term.
 */
static int64
find_block(SiftDict *d, const char *term, int len)
{
	uint64		p = prefix8(term, len);
	int64		lo = 0,
				hi = d->nroot,
				rlo,
				rhi,
				blo,
				bhi;

	/* root entries [rlo, rhi) may hold the block: rlo after the last < p */
	while (lo < hi)
	{
		int64		mid = (lo + hi) / 2;

		if (d->root[mid] < p)
			lo = mid + 1;
		else
			hi = mid;
	}
	rlo = Max(lo - 1, 0);
	for (rhi = lo; rhi < d->nroot && d->root[rhi] == p; rhi++)
		;
	if (rhi == 0)
		return -1;
	blo = rlo * SIFT_DICT_ROOT_FANOUT;
	bhi = Min((uint64) rhi * SIFT_DICT_ROOT_FANOUT, d->nblocks);
	/* first block in [blo, bhi) with prefix >= p */
	lo = blo;
	hi = bhi;
	while (lo < hi)
	{
		int64		mid = (lo + hi) / 2;

		if (read_idx(d, mid).prefix < p)
			lo = mid + 1;
		else
			hi = mid;
	}
	/* blocks from lo - 1 up to the last one with prefix == p are candidates */
	blo = Max(lo - 1, 0);
	for (bhi = lo; bhi < (int64) d->nblocks && read_idx(d, bhi).prefix == p; bhi++)
	{
		BlockIter	it;
		SiftTermInfo info;

		block_begin(d, read_idx(d, bhi).block_off, d->blk, &it);
		block_next(&it, &info);
		if (term_cmp(it.term, it.len, term, len) > 0)
			break;
		blo = bhi;
	}
	if (blo == 0 && lo == 0 && bhi == 0)
		return -1;
	return blo;
}

bool
sift_dict_lookup(SiftDict *d, const char *term, int len, SiftTermInfo *info)
{
	int64		b = d->nblocks ? find_block(d, term, len) : -1;
	BlockIter	it;

	if (b < 0)
		return false;
	block_begin(d, read_idx(d, b).block_off, d->blk, &it);
	while (block_next(&it, info))
	{
		int			c = term_cmp(it.term, it.len, term, len);

		if (c >= 0)
			return c == 0;
	}
	return false;
}

void
sift_dict_scan(SiftDict *d, const char *lo, int lolen, const char *hi, int hilen,
			   SiftDictVisit fn, void *arg)
{
	int64		b = lo && d->nblocks ? Max(find_block(d, lo, lolen), 0) : 0;
	uint8	   *buf;
	bool		more = true;

	if (d->nblocks == 0)
		return;
	/* own copy: fn may look terms up in this dictionary */
	buf = palloc(UINT16_MAX + 1);
	for (uint64 off = read_idx(d, b).block_off; more && off < d->dict_len;)
	{
		BlockIter	it;
		SiftTermInfo info;

		block_begin(d, off, buf, &it);
		off += it.end - buf;
		while (more && block_next(&it, &info))
		{
			if (lo && term_cmp(it.term, it.len, lo, lolen) < 0)
				continue;
			more = (!hi || term_cmp(it.term, it.len, hi, hilen) < 0) && fn(arg, it.term, it.len, &info);
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(buf);
}

/* ---------- matchers ---------- */

struct SiftTermMatcher
{
	const SiftQNode *leaf;
	char	   *lo;
	int			lolen;
	char	   *hi;				/* NULL: unbounded */
	int			hilen;
};

/* Smallest string above every string starting with s[0..len): NULL if none */
static char *
successor(const char *s, int *len)
{
	char	   *r = pnstrdup(s, *len);

	while (*len > 0)
	{
		if ((uint8) r[*len - 1] < 0xFF)
		{
			r[*len - 1]++;
			return r;
		}
		(*len)--;
	}
	return NULL;
}

/* Unescaped literal prefix of a folded wildcard pattern */
static int
wildcard_prefix(const char *pat, int len, char *out)
{
	int			n = 0;

	for (int i = 0; i < len; i++)
	{
		if (pat[i] == '*' || pat[i] == '?')
			break;
		if (pat[i] == '\\' && i + 1 < len)
			i++;
		out[n++] = pat[i];
	}
	return n;
}

static int
regex_prefix(const SiftQNode *leaf, char **out)
{
	pg_wchar   *w;
	size_t		n;

	switch (pg_regprefix((regex_t *) leaf->re, &w, &n))
	{
		case REG_PREFIX:
		case REG_EXACT:
			*out = palloc(n * pg_database_encoding_max_length() + 1);
			return pg_wchar2mb_with_len(w, *out, n);
		default:
			return 0;
	}
}

SiftTermMatcher *
sift_matcher_create(const SiftQNode *leaf)
{
	SiftTermMatcher *m = palloc0(sizeof(SiftTermMatcher));

	m->leaf = leaf;
	switch (leaf->kind)
	{
		case SQ_TERM:
			m->lolen = leaf->textlen;
			m->lo = leaf->text;
			m->hilen = leaf->textlen + 1;
			m->hi = palloc0(m->hilen);
			memcpy(m->hi, leaf->text, leaf->textlen);
			return m;
		case SQ_PREFIX:
			m->lo = leaf->text;
			m->lolen = leaf->textlen;
			break;
		case SQ_WILDCARD:
			m->lo = palloc(leaf->textlen + 1);
			m->lolen = wildcard_prefix(leaf->text, leaf->textlen, m->lo);
			break;
		case SQ_FUZZY:
			m->lo = leaf->text;
			m->lolen = leaf->textlen ? pg_mblen(leaf->text) : 0;
			break;
		case SQ_REGEX:
			m->lolen = regex_prefix(leaf, &m->lo);
			break;
		default:
			elog(ERROR, "sift: query node kind %d has no term matcher", leaf->kind);
	}
	m->hilen = m->lolen;
	m->hi = m->lolen ? successor(m->lo, &m->hilen) : NULL;
	if (m->lolen == 0)
	{
		/* every word starts above the pair and prefix terms, which a scan must not walk */
		m->lo = " ";
		m->lolen = 1;
	}
	return m;
}

void
sift_matcher_bounds(const SiftTermMatcher *m, const char **lo, int *lolen,
					const char **hi, int *hilen)
{
	*lo = m->lo;
	*lolen = m->lolen;
	*hi = m->hi;
	*hilen = m->hilen;
}

bool
sift_matcher_match(const SiftTermMatcher *m, const char *term, int len)
{
	/* pair and prefix terms are not words: no expansion reaches them */
	return !SIFT_SYNTHETIC_TERM(term) && sift_query_leaf_match(m->leaf, term, len);
}
