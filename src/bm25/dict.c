/*
 * bm25/dict.c - term dictionary reader: open, block decoding, lookups, scans and expansion matchers
 * (layout in bm25/dict.h).
 */
#include "dict_int.h"

#ifdef SIFT_TESTING
int			sift_test_dict_max = 0;
#endif
static const uint8 *
get_varint(const uint8 *p, const uint8 *end, uint64 *x)
{
	if ((p = sift_get_varint64_n(p, end, x)) == NULL)
		corrupt("Truncated varint in a dictionary block.");
	return p;
}

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

/* Copies the block at offset off of DICT into buf (UINT16_MAX + 1 bytes) and starts decoding it; slot_base: its DICTIDX entry's */
static void
block_begin(SiftDict *d, uint64 off, uint32 slot_base, uint8 *buf, BlockIter *it)
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
	it->left = buf[2] & ~SIFT_DICT_PACKED;
	it->packed = (buf[2] & SIFT_DICT_PACKED) != 0;
	it->p = get_varint(buf + 3, it->end, &post);
	it->post = post;
	it->slot = slot_base;
	it->len = 0;
	if (it->left < 1 || it->left > SIFT_DICT_BLOCK)
		corrupt("Block at %llu holds %d terms.", (unsigned long long) off, it->left);
}

static pg_attribute_always_inline bool
block_next_impl(BlockIter *it, SiftTermInfo *info, bool slots)
{
	uint64		v,
				first;
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
	if (it->packed)
	{
		shared = *it->p & 15;
		suffix = (*it->p++ >> 4) + 1;
		if (shared == 15 && it->p < it->end)
			shared = *it->p++;
		if (suffix == 16 && it->p < it->end)
			suffix = *it->p++ + 1;
	}
	else
	{
		shared = it->p[0];
		suffix = it->p[1];
		it->p += 2;
	}
	if (shared > it->len || shared + suffix > SIFT_MAX_TERM_BYTES || suffix == 0 ||
		it->end - it->p < suffix + 2)
		corrupt("Dictionary entry with prefix %d and suffix %d after a term of %d bytes.",
				shared, suffix, it->len);
	memcpy(it->term + shared, it->p, suffix);
	it->len = shared + suffix;
	it->p += suffix;
	memset(info, 0, sizeof(SiftTermInfo));
	if (it->packed)
	{
		info->flags = *it->p & 15;
		info->df = *it->p++ >> 4;
		if (info->df == 15)
		{
			it->p = get_varint(it->p, it->end, &v);
			info->df += v;
		}
		if (info->df == 0 || it->p >= it->end)
			corrupt("Truncated dictionary entry.");
		info->max_bound = *it->p++;
	}
	else
	{
		it->p = get_varint(it->p, it->end, &v);
		info->df = v;
		if (it->end - it->p < 2)
			corrupt("Truncated dictionary entry.");
		info->flags = *it->p++;
		info->max_bound = *it->p++;
	}
	if (info->flags & SIFT_TERM_INLINE)
	{
		if (info->df < 1 || info->df > SIFT_INLINE_MAX)
			corrupt("Inline term with df %u and flags %u.", info->df, info->flags);
		it->p = get_varint(it->p, it->end, &first);
		info->inline_keys[0] = it->packed ? sift_dict_unpack_key(first) : first;
		if (info->df > 1)
		{
			it->p = get_varint(it->p, it->end, &v);
			info->inline_keys[1] = it->packed ? sift_dict_unpack_key(first + v) : info->inline_keys[0] + v;
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
		if (info->flags & SIFT_TERM_POS)
		{
			it->p = get_varint(it->p, it->end, &v);
			if (v == 0 || (uint64) (it->end - it->p) < v)
				corrupt("Truncated inline positions.");
			info->pos_len = v;
			info->inline_pos = it->p;
			it->p += v;
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
	if (info->flags & SIFT_TERM_POS)
	{
		it->p = get_varint(it->p, it->end, &v);
		if (v == 0 || v > info->post_len - info->codec_len)
			corrupt("Position stream of %llu bytes in a term blob of %u.", (unsigned long long) v, info->post_len);
		info->pos_len = v;
	}
	info->post_off = ALIGN4(it->post);
	it->post = info->post_off + info->post_len;
	it->slot += slots;
	return true;
}

static bool
block_next(BlockIter *it, SiftTermInfo *info)
{
	return block_next_impl(it, info, false);
}

static bool
block_next_slots(BlockIter *it, SiftTermInfo *info)
{
	return block_next_impl(it, info, true);
}

/* The live df and the slot of the entry block_next returned, for the callers that keep it (the others do not pay for them) */
static pg_attribute_always_inline void
entry_stats(const BlockIter *it, SiftTermInfo *info, bool slots)
{
	info->ldf = info->df;
	info->slot = (slots && !(info->flags & SIFT_TERM_INLINE)) ? (int32) it->slot - 1 : -1;
}

void
sift_dict_track_slots(SiftDict *d)
{
	d->slots = true;
}

#ifdef SIFT_TESTING
uint64		sift_dict_probes;
uint64		sift_dict_scan_blocks;
#endif

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
	SIFT_BISECT(int64, mid, lo, hi, d->root[mid] < p);
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
	SIFT_BISECT(int64, mid, lo, hi, read_idx(d, mid).prefix < p);
	/*
	 * The blocks of the run with prefix p (fixed-width ids share it for thousands of blocks) are ordered by
	 * their first term: the run ends where a doubling step then a bisection of the prefixes finds it, and
	 * the last block of it whose first term is <= term is found by bisection.
	 */
	blo = Max(lo - 1, 0);
	rhi = lo;
	if (lo < (int64) d->nblocks && read_idx(d, lo).prefix == p)
	{
		int64		step = 1,
					from = lo;

		while (from + step < (int64) d->nblocks && read_idx(d, from + step).prefix == p)
			from += step, step *= 2;
		for (rhi = from + 1, bhi = Min(from + step, (int64) d->nblocks); rhi < bhi;)
		{
			int64		mid = (rhi + bhi) / 2;

			if (read_idx(d, mid).prefix == p)
				rhi = mid + 1;
			else
				bhi = mid;
		}
	}
	for (bhi = lo; bhi < rhi;)
	{
		int64		mid = (bhi + rhi) / 2;
		BlockIter	it;
		SiftTermInfo info;

		block_begin(d, read_idx(d, mid).block_off, 0, d->blk, &it);
#ifdef SIFT_TESTING
		sift_dict_probes++;
#endif
		block_next(&it, &info);
		if (sift_term_cmp(it.term, it.len, term, len) > 0)
			rhi = mid;
		else
			bhi = mid + 1;
	}
	if (bhi > lo)
		blo = bhi - 1;
	if (blo == 0 && lo == 0 && bhi == 0)
		return -1;
	return blo;
}

static pg_attribute_always_inline bool
lookup_in_block(BlockIter *it, const char *term, int len, SiftTermInfo *info, bool slots)
{
	while (block_next_impl(it, info, slots))
	{
		int			c = sift_term_cmp(it->term, it->len, term, len);

		if (c >= 0)
		{
			if (c == 0)
				entry_stats(it, info, slots);
			return c == 0;
		}
	}
	return false;
}

bool
sift_dict_lookup(SiftDict *d, const char *term, int len, SiftTermInfo *info)
{
	int64		b = d->nblocks ? find_block(d, term, len) : -1;
	BlockIter	it;
	SiftDictIdx e;

	if (b < 0)
		return false;
	e = read_idx(d, b);
	block_begin(d, e.block_off, e.slot_base, d->blk, &it);
	return d->slots ? lookup_in_block(&it, term, len, info, true) : lookup_in_block(&it, term, len, info, false);
}

void
sift_dict_scan(SiftDict *d, const char *lo, int lolen, const char *hi, int hilen,
			   SiftDictVisit fn, void *arg)
{
	sift_dict_scan_skipping(d, lo, lolen, hi, hilen, NULL, fn, arg);
}

void
sift_dict_scan_skipping(SiftDict *d, const char *lo, int lolen, const char *hi, int hilen,
						SiftDictSkip skip, SiftDictVisit fn, void *arg)
{
	int64		b = lo && d->nblocks ? Max(find_block(d, lo, lolen), 0) : 0;
	uint8	   *buf;
	bool		more = true,
				sought = false;
	uint32		nblk = 0,
				slot;

	if (d->nblocks == 0)
		return;
	/* own copy: fn may look terms up in this dictionary */
	buf = palloc(UINT16_MAX + 1);
	slot = read_idx(d, b).slot_base;
	for (uint64 off = read_idx(d, b).block_off; more && off < d->dict_len;)
	{
		BlockIter	it;
		SiftTermInfo info;
		bool		jump = false;

		block_begin(d, off, slot, buf, &it);
		off += it.end - buf;
#ifdef SIFT_TESTING
		sift_dict_scan_blocks++;
#endif
		while (more && block_next_slots(&it, &info))
		{
			if (lo && sift_term_cmp(it.term, it.len, lo, lolen) < 0)
				continue;
			if (hi && sift_term_cmp(it.term, it.len, hi, hilen) >= 0)
			{
				more = false;
				break;
			}
			sought = false;
			if (skip && skip(arg, it.term, it.len, &lo, &lolen))
			{
				if (lolen == 0)
				{
					more = false;
					break;
				}
				sought = true;
				continue;
			}
			entry_stats(&it, &info, true);
			more = fn(arg, it.term, it.len, &info);
		}
		/* the block ended below the key the skip asked for: it is in a later block unless the next one holds it */
		if (more && sought)
		{
			SiftDictIdx e = read_idx(d, Max(find_block(d, lo, lolen), 0));

			if (e.block_off > off)
			{
				off = e.block_off;
				slot = e.slot_base;
				jump = true;
			}
		}
		if (!jump)
			slot = it.slot;
		if ((++nblk & 15) == 0)
		{
			sift_sr_unlock(d->sr);	/* a buffer lock holds off interrupts */
			CHECK_FOR_INTERRUPTS();
		}
	}
	pfree(buf);
}

struct SiftTermMatcher
{
	const SiftQNode *leaf;
	char	   *lo;
	int			lolen;
	char	   *hi;				/* NULL: unbounded */
	int			hilen;
	char		field;			/* terms of a field start with this byte, else 0 */
	char	   *skip;			/* the key of the last sift_matcher_skip */
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
			SIFT_INVARIANT(false, "sift: query node kind %d has no term matcher", leaf->kind);
	}
	if (leaf->field)
	{
		/* the field's words follow its byte; " " skips the field's prefix terms */
		char	   *lit = palloc(m->lolen + 2);

		m->field = lit[0] = SIFT_FIELD_BYTE(leaf->field);
		if (m->lolen > 0)
			memcpy(lit + 1, m->lo, m->lolen);
		m->hilen = m->lolen + 1;
		m->hi = successor(lit, &m->hilen);
		if (m->lolen == 0)
			lit[1] = ' ';
		m->lo = lit;
		m->lolen += 1 + (m->lolen == 0);
		return m;
	}
	m->hilen = m->lolen;
	m->hi = m->lolen ? successor(m->lo, &m->hilen) : NULL;
	if (m->lolen == 0)
	{
		/* every word starts above the prefix terms, which a scan must not walk */
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
sift_matcher_skips(const SiftTermMatcher *m)
{
	return m->leaf->kind == SQ_FUZZY;
}

/* A term that starts with a dead prefix of the leaf: the next term the leaf can match is above every term with it (*nextlen 0: none) */
bool
sift_matcher_skip(SiftTermMatcher *m, const char *term, int len, const char **next, int *nextlen)
{
	int			skip = m->field ? 1 : 0,
				dead;

	if (len <= skip || (m->field && *term != m->field) || SIFT_SYNTHETIC_TERM(term + skip))
		return false;
	dead = sift_query_leaf_dead_prefix(m->leaf, term + skip, len - skip);
	if (dead == 0)
		return false;
	if (m->skip)
		pfree(m->skip);
	*nextlen = dead + skip;
	*next = m->skip = successor(term, nextlen);
	if (m->skip == NULL)
		*nextlen = 0;
	return true;
}

bool
sift_matcher_match(const SiftTermMatcher *m, const char *term, int len)
{
	/* prefix terms are not words: no expansion reaches them */
	if (m->field)
	{
		if (len < 2 || *term != m->field)
			return false;
		term++, len--;
	}
	return !SIFT_SYNTHETIC_TERM(term) && sift_query_leaf_match(m->leaf, term, len);
}
