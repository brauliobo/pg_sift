/*
 * bm25/analyze_scan.h - the scanner of bm25/analyze.c: allocation of its buffers, stemming with its memo, UTF-8 and case/accent folding, scan_token and the token stream.
 * Included once, by analyze.c, where the text stood: the functions are static so that the token stream, the streaming cursor and the document bag share one scanner,
 * and it needs the analyzer struct and the hash of analyze.c.
 */
#ifndef SIFT_BM25_ANALYZE_SCAN_H
#define SIFT_BM25_ANALYZE_SCAN_H

/* Grows *p to hold at least need elements of size sz (contents preserved) */
static void
grow(SiftAnalyzer *a, void **p, size_t *cap, size_t need, size_t sz)
{
	size_t		n = Max(*cap * 2, Max(need, 64));

	*p = *p ? repalloc_huge(*p, n * sz)
		: MemoryContextAllocExtended(a->mcxt, n * sz, MCXT_ALLOC_HUGE);
	*cap = n;
}

#define ENSURE(a, field, capfield, need) \
	do { if ((need) > (a)->capfield) \
		grow(a, (void **) &(a)->field, &(a)->capfield, need, sizeof(*(a)->field)); } while (0)

/* Scanner */

/*
 * Stems until the word no longer changes, so an emitted term analyzes to
 * itself (Porter2 alone is not idempotent: agreed -> agre -> agr). This ends:
 * stemmers never lengthen a word, and the only same-length rewrites turn a
 * final y into i or i into e.
 */
static int
stem_fixpoint(StemFn fn, char *w, int n)
{
	char		prev[DST_CAP];

	for (;;)
	{
		int			m;

		memcpy(prev, w, n);
		if ((m = fn(w, n)) == n && memcmp(prev, w, n) == 0)
			return n;
		n = m;
	}
}

static inline int
stem(SiftAnalyzer *a, char *w, int n)
{
	StemSlot   *e;

	if (!a->stem_memo && ++a->nstems == STEM_MEMO_AFTER)
		a->stem_memo = MemoryContextAllocZero(a->mcxt, STEM_SLOTS * sizeof(StemSlot));
	if (n > STEM_MAXW || !a->stem_memo)
		return stem_fixpoint(a->stem, w, n);
	e = &a->stem_memo[hash64(w, n) & (STEM_SLOTS - 1)];
	if (e->inlen != n || memcmp(e->in, w, n) != 0)
	{
		memcpy(e->in, w, n);
		e->outlen = stem_fixpoint(a->stem, w, n);
		memcpy(e->out, w, e->outlen);
		e->inlen = n;
	}
	memcpy(w, e->out, e->outlen);
	return e->outlen;
}

static inline uint16
cp_info(uint32 cp)
{
	return sift_cp_stage2[(sift_cp_stage1[cp >> SIFT_CP_SHIFT] << SIFT_CP_SHIFT) |
						  (cp & ((1 << SIFT_CP_SHIFT) - 1))];
}

/* Decodes one UTF-8 character under PostgreSQL's rules; returns 0 if invalid */
static inline int
utf8_decode(const uint8 *s, int avail, uint32 *cp)
{
	uint8		c = s[0];
	int			n;
	uint8		lo = 0x80,
				hi = 0xBF;

	if (c < 0xC2 || c > 0xF4)
		return 0;
	n = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
	if (avail < n)
		return 0;
	if (c == 0xE0)
		lo = 0xA0;
	else if (c == 0xED)
		hi = 0x9F;
	else if (c == 0xF0)
		lo = 0x90;
	else if (c == 0xF4)
		hi = 0x8F;
	if (s[1] < lo || s[1] > hi)
		return 0;
	*cp = c & (0x7F >> n);
	*cp = (*cp << 6) | (s[1] & 0x3F);
	for (int i = 2; i < n; i++)
	{
		if ((s[i] & 0xC0) != 0x80)
			return 0;
		*cp = (*cp << 6) | (s[i] & 0x3F);
	}
	return n;
}

/*
 * Appends the folding of one source character to dst[*n..cap); once a
 * character does not fit, cap shrinks to *n so the token is cut at a
 * character boundary.
 */
static inline void
append_fold(const SiftAnalyzer *a, uint16 info, const uint8 *src, int clen,
			char *dst, int *n, int *cap)
{
	const uint8 *f = src;
	int			flen = clen;
	uint16		rec = info & REC_MASK;

	if (rec && a->mode)
	{
		f = &sift_fold_pool[sift_fold_recs[rec * 3 + a->mode - 1]];
		flen = *f++;
	}
	if (*n + flen > *cap)
		*cap = *n;
	else
	{
		memcpy(dst + *n, f, flen);
		*n += flen;
	}
}

#ifdef __SSE2__
/*
 * Fast path: skips ASCII separators 16 bytes at a time and scans a token made
 * only of ASCII letters and digits (any byte >= 0x80 in SQL_ASCII) into dst,
 * returning its length. Returns -1 with *i at the first byte the scalar
 * scanner must handle.
 */
static inline int
scan_ascii(const SiftAnalyzer *a, const uint8 *s, int len, int *i, int *start, char *dst)
{
	const __m128i lower = _mm_set1_epi8(a->cfg.case_fold ? 0x20 : 0);
	int			p = *i,
				st = -1;

	for (; p + 16 <= len; p += 16)
	{
		__m128i		v = _mm_loadu_si128((const __m128i *) (s + p));
		__m128i		letter = _mm_cmplt_epi8(_mm_add_epi8(_mm_or_si128(v, _mm_set1_epi8(0x20)),
														 _mm_set1_epi8((char) (128 - 'a'))),
											_mm_set1_epi8(-128 + 26));
		__m128i		digit = _mm_cmplt_epi8(_mm_add_epi8(v, _mm_set1_epi8((char) (128 - '0'))),
										   _mm_set1_epi8(-128 + 10));
		uint32		high = _mm_movemask_epi8(v),
					word = _mm_movemask_epi8(_mm_or_si128(letter, digit)) | (a->utf8 ? 0 : high),
					stop;

		if (!a->utf8)
			high = 0;
		if (st < 0)
		{
			if (!(word | high))
				continue;
			st = p + pg_rightmost_one_pos32(word | high);
			if (high & (1 << (st - p)))
				break;
			/* restart at the token so dst gets whole vectors from its start */
			p = st - 16;
			continue;
		}
		if (p - st < TERM_CAP)
			_mm_storeu_si128((__m128i *) (dst + p - st),
							 _mm_or_si128(v, _mm_and_si128(letter, lower)));
		if ((stop = ~word & 0xFFFF))
		{
			int			e = p + pg_rightmost_one_pos32(stop);

			if (high & stop & -stop)
				break;
			*start = st;
			*i = e;
			return Min(e - st, TERM_CAP);
		}
	}
	*i = st >= 0 ? st : p;
	return -1;
}
#endif

static int	fold_chars(const SiftAnalyzer *a, const uint8 *s, int len, char *dst);

/*
 * A keyword value, or a word of the text analyzer, over TERM_CAP bytes (folded) is a term of its first bytes, cut at a
 * character boundary, then U+FFFF and the 16 hex digits of the 64-bit FNV-1a hash of all of it: values that share their
 * start stay apart (an error would stop an INSERT or the end of a long CREATE INDEX, a cut would merge them), whatever
 * the path that builds the term, an index build, an insert or a query. Writes the term to dst; returns its length.
 */
static int
keyword_long(const uint8 *s, int len, char *dst)
{
	static const char hex[] = "0123456789abcdef";
	uint64		h = UINT64CONST(0xcbf29ce484222325);
	int			cut = SIFT_TERM_HEAD;

	for (int i = 0; i < len; i++)
		h = (h ^ s[i]) * UINT64CONST(0x100000001b3);
	while (cut > 0 && (s[cut] & 0xC0) == 0x80)
		cut--;
	memcpy(dst, s, cut);
	memcpy(dst + cut, "\xEF\xBF\xBF", 3);
	for (int i = 0; i < 16; i++)
		dst[cut + 3 + i] = hex[(h >> (60 - 4 * i)) & 15];
	return cut + SIFT_TERM_TAIL;
}

/*
 * A word whose folded form is over TERM_CAP bytes was clipped by the scanners: it becomes the term keyword_long makes of
 * s[from..to), its source, so words that share their start stay apart. Returns the length of the term, 0 if the word fits.
 */
static int
long_word(SiftAnalyzer *a, const uint8 *s, int from, int to, char *dst)
{
	int			n;

	ENSURE(a, kw, kwcap, (size_t) (to - from) * SIFT_FOLD_MAX_EXPAND + 1);
	n = fold_chars(a, s + from, to - from, a->kw);
	return n > TERM_CAP ? keyword_long((const uint8 *) a->kw, n, dst) : 0;
}

/* Keyword analyzer: the whole text is one token, none if it is empty or folds to nothing */
static int
scan_keyword(SiftAnalyzer *a, const uint8 *s, int len, int *off, int *start, char *dst)
{
	int			n = len;

	if (*off >= len)
		return -1;
	*start = *off;
	*off = len;
	if (a->cfg.case_fold || a->cfg.accent_fold)
	{
		ENSURE(a, kw, kwcap, (size_t) len * SIFT_FOLD_MAX_EXPAND + 1);
		n = fold_chars(a, s, len, a->kw);
		s = (const uint8 *) a->kw;
	}
	if (n > TERM_CAP)
		n = keyword_long(s, n, dst);
	else
		memcpy(dst, s, n);
	return n > 0 ? n : -1;
}

/*
 * Scans the next token of s[*off..len) into dst (capacity DST_CAP) and
 * returns its length, or -1 at the end. Sets *start and advances *off past
 * the token. Tokens that fold to nothing are skipped.
 */
static pg_attribute_always_inline int
scan_token(SiftAnalyzer *a, const uint8 *s, int len, int *off, int *start, char *dst)
{
	int			i = *off;

	if (unlikely(a->cfg.keyword))
		return scan_keyword(a, s, len, off, start, dst);
	for (;;)
	{
		int			n = 0,
					cap = TERM_CAP,
					clen = 1,
					cls = C_WORD,
					hashed;
		uint32		cp = 0;
		uint16		info = 0;

#ifdef __SSE2__
		if ((n = scan_ascii(a, s, len, &i, start, dst)) >= 0)
		{
			if (n >= TERM_CAP && (hashed = long_word(a, s, *start, i, dst)) > 0)
			{
				*off = i;
				return hashed;
			}
			if (a->stem)
				n = stem(a, dst, n);
			if (n > 0)
			{
				*off = i;
				return n;
			}
			continue;
		}
		n = 0;
#endif
		while (i < len && s[i] < 0x80 && !a->ascii[s[i]])
			i++;
		if (i >= len)
		{
			*off = i;
			return -1;
		}
		*start = i;
		if (s[i] >= 0x80 && a->utf8)
		{
			if (!(clen = utf8_decode(s + i, len - i, &cp)))
			{
				i++;
				continue;
			}
			info = cp_info(cp);
			cls = info >> 13;
		}
		if (cls == C_WORD || cls == C_MARK)
		{
			while (i < len)
			{
				uint8		c = s[i];

				if (likely(c < 0x80))
				{
					uint8		f = a->ascii[c];

					if (!f)
						break;
					if (n < cap)
						dst[n++] = f;
					i++;
					continue;
				}
				if (!a->utf8)
				{
					if (n < cap)
						dst[n++] = c;
					i++;
					continue;
				}
				if (!(clen = utf8_decode(s + i, len - i, &cp)))
					break;
				info = cp_info(cp);
				cls = info >> 13;
				if (cls == C_WORD || cls == C_MARK)
					append_fold(a, info, s + i, clen, dst, &n, &cap);
				else if (cls != C_IGN)
					break;
				i += clen;
			}
		}
		else if (cls >= C_IDEO && cls <= C_RI)
		{
			append_fold(a, info, s + i, clen, dst, &n, &cap);
			i += clen;
			if (cls == C_RI && i < len && utf8_decode(s + i, len - i, &cp) == 4 &&
				cp_info(cp) >> 13 == C_RI)
			{
				append_fold(a, cp_info(cp), s + i, 4, dst, &n, &cap);
				i += 4;
			}
			/* attached marks fold with the character; emoji drop them */
			while (i < len && s[i] >= 0x80 && (clen = utf8_decode(s + i, len - i, &cp)) &&
				   ((info = cp_info(cp)) >> 13 == C_MARK || info >> 13 == C_IGN))
			{
				if (cls == C_IDEO && info >> 13 == C_MARK)
					append_fold(a, info, s + i, clen, dst, &n, &cap);
				i += clen;
			}
		}
		else
		{
			i += clen;
			continue;
		}
		if (n >= TERM_CAP - 3 && (hashed = long_word(a, s, *start, i, dst)) > 0)
		{
			*off = i;
			return hashed;
		}
		if (n > 0 && a->stem)
			n = stem(a, dst, n);
		if (n > 0)
		{
			*off = i;
			return n;
		}
	}
}

/* Token stream */

int
sift_analyze_tokens(SiftAnalyzer *a, const char *text, int len, SiftToken **out)
{
	const uint8 *s = (const uint8 *) text;
	size_t		used = 0;
	int			ntok = 0,
				off = 0,
				start,
				n;

	for (;;)
	{
		ENSURE(a, buf, bufcap, used + DST_CAP);
		if ((n = scan_token(a, s, len, &off, &start, a->buf + used)) < 0)
			break;
		ENSURE(a, toks, tokcap, ntok + 1);
		a->toks[ntok] = (SiftToken)
		{
			.str = (const char *) used, .len = n, .pos = ntok, .start = start, .end = off
		};
		ntok++;
		used += n;
	}
	for (int t = 0; t < ntok; t++)
		a->toks[t].str = a->buf + (size_t) a->toks[t].str;
	*out = a->toks;
	return ntok;
}

void
sift_analyze_begin(SiftTokenStream *ts, const char *text, int len)
{
	*ts = (SiftTokenStream) {.text = text, .len = len};
}

bool
sift_analyze_next(SiftAnalyzer *a, SiftTokenStream *ts, SiftToken *tok)
{
	int			start,
				n = scan_token(a, (const uint8 *) ts->text, ts->len, &ts->off, &start, a->scratch);

	if (n < 0)
		return false;
	*tok = (SiftToken)
	{
		.str = a->scratch, .len = n, .pos = ts->pos++, .start = start, .end = ts->off
	};
	return true;
}

#endif
