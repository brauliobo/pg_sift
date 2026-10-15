/*
 * sift_analyze.c - tokenizer, case and accent folding, stemming, term hashing.
 * Rules are documented in sift_analyze.h. One scanner (scan_token) serves the
 * token stream, the streaming cursor and the document bag, so index and query
 * analysis cannot diverge.
 */
#include "postgres.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#include "mb/pg_wchar.h"
#include "port/pg_bitutils.h"
#include "port/pg_bswap.h"
#include "utils/memutils.h"

#include "sift_analyze.h"
#include "sift_analyze_tables.h"

/* Code point classes (sift_analyze_tables.h entries >> 13) */
enum
{
	C_SEP, C_WORD, C_IDEO, C_EMOJI, C_RI, C_IGN, C_MARK
};

#define REC_MASK		0x1FFF
#define TERM_CAP		SIFT_MAX_TERM_BYTES
#define DST_CAP			(TERM_CAP + 16)	/* token buffer: cap plus one vector store */
#define KEEP_BYTES		(1024 * 1024)	/* buffers above this are freed on reset */
#define STEM_SLOTS		4096
#define STEM_MAXW		23
#define STEM_MEMO_AFTER	1024		/* stems before the memo pays for its setup */

/* Memo of recent stems; words follow Zipf's law, so most tokens hit */
typedef struct StemSlot
{
	uint8		inlen;			/* 0 = empty */
	uint8		outlen;
	char		in[STEM_MAXW];
	char		out[STEM_MAXW];
} StemSlot;

typedef int (*StemFn) (char *w, int len);

static const StemFn stem_fns[SIFT_STEM_COUNT] = {
	[SIFT_STEM_EN] = sift_analyze_stem_en,
};

static const struct
{
	const char *name;
	SiftStemmer id;
}			stem_names[] = {
	{"", SIFT_STEM_NONE}, {"none", SIFT_STEM_NONE},
	{"en", SIFT_STEM_EN}, {"english", SIFT_STEM_EN}, {"english_stem", SIFT_STEM_EN},
};

/* Distinct term of the document being bagged */
typedef struct BagTerm
{
	uint64		h;
	uint32		off;			/* into buf */
	uint32		tf;
	uint16		len;
} BagTerm;

struct SiftAnalyzer
{
	SiftAnalyzerConfig cfg;
	MemoryContext mcxt;
	bool		utf8;
	uint8		mode;			/* 0 none, 1 case, 2 accent, 3 both */
	StemFn		stem;
	StemSlot   *stem_memo;		/* STEM_SLOTS entries, after STEM_MEMO_AFTER stems */
	uint32		nstems;
	uint8		ascii[128];		/* folded byte, 0 = separator */

	char	   *buf;			/* folded token bytes */
	size_t		bufcap;
	SiftToken  *toks;
	size_t		tokcap;

	int32	   *slots;			/* open addressing over bag, -1 = empty */
	size_t		slotcap;
	uint32		nslots;			/* in use, a power of two */
	int			shift;			/* slot = hash32 >> shift */
	BagTerm    *bag;
	size_t		bagcap;
	SiftTermFreq *terms;
	size_t		termcap;

	char		scratch[DST_CAP];
};

/* Hashing */

#define K1	UINT64CONST(0x9E3779B185EBCA87)
#define K2	UINT64CONST(0xC2B2AE3D27D4EB4F)
#define K3	UINT64CONST(0x27D4EB2F165667C5)

static inline uint64
load_le(const char *p, int n)
{
	uint64		w = 0;

	memcpy(&w, p, n);
#ifdef WORDS_BIGENDIAN
	w = n == 8 ? pg_bswap64(w) : n == 4 ? pg_bswap32((uint32) w) : w;
#endif
	return w;
}

/* The zero-padded little-endian word of 1..7 bytes, without reading past p + n */
static inline uint64
load_tail(const char *p, int n)
{
	const uint8 *u = (const uint8 *) p;

	if (n >= 4)
		return load_le(p, 4) | load_le(p + n - 4, 4) << (8 * (n - 4));
	return u[0] | (uint64) u[n / 2] << (8 * (n / 2)) | (uint64) u[n - 1] << (8 * (n - 1));
}

static inline bool
same_bytes(const char *x, const char *y, int n)
{
	for (; n >= 8; x += 8, y += 8, n -= 8)
		if (load_le(x, 8) != load_le(y, 8))
			return false;
	return n == 0 || load_tail(x, n) == load_tail(y, n);
}

static inline uint64
mix(uint64 h, uint64 w)
{
	h += w * K2;
	return ((h << 31) | (h >> 33)) * K1;
}

static inline uint64
hash64(const char *term, int len)
{
	uint64		h = K3 ^ (uint64) len * K1;

	for (; len >= 8; term += 8, len -= 8)
		h = mix(h, load_le(term, 8));
	if (len > 0)
		h = mix(h, load_tail(term, len));
	h ^= h >> 33;
	h *= UINT64CONST(0xFF51AFD7ED558CCD);
	h ^= h >> 33;
	h *= UINT64CONST(0xC4CEB9FE1A85EC53);
	return h ^ (h >> 33);
}

uint64
sift_term_hash64(const char *term, int len)
{
	return hash64(term, len);
}

uint32
sift_term_hash(const char *term, int len)
{
	return (uint32) (hash64(term, len) >> 32);
}

/* Configuration */

void
sift_analyzer_config_set_synthetic(SiftAnalyzerConfig *cfg, bool pairs, int prefix_len)
{
	uint8		key[8] = {SIFT_ANALYZER_VERSION, cfg->case_fold, cfg->accent_fold, cfg->stemmer};
	int			n = 4;

	cfg->pairs = pairs;
	cfg->prefix_len = prefix_len;
	if (pairs)
		key[n++] = 1;
	if (prefix_len)
	{
		key[n++] = 2;
		key[n++] = prefix_len;
	}
	cfg->hash = sift_term_hash((const char *) key, n);
}

void
sift_analyzer_config_set(SiftAnalyzerConfig *cfg, bool case_fold, bool accent_fold,
						 SiftStemmer stemmer)
{
	cfg->case_fold = case_fold;
	cfg->accent_fold = accent_fold;
	cfg->stemmer = stemmer;
	sift_analyzer_config_set_synthetic(cfg, false, 0);
}

void
sift_analyzer_config_default(SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_set(cfg, true, true, SIFT_STEM_NONE);
}

void
sift_analyzer_config_from_options(const SiftOptions *opts, SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_set(cfg, opts->case_folding, opts->accent_folding,
							 sift_analyze_stemmer(sift_options_stemmer(opts)));
	sift_analyzer_config_set_synthetic(cfg, opts->phrase_index == SIFT_PHRASE_PAIRS, opts->prefix_index);
}

void
sift_analyzer_config_from_index(Relation index, SiftAnalyzerConfig *cfg)
{
	sift_analyzer_config_from_options(sift_index_options(index), cfg);
}

SiftStemmer
sift_analyze_stemmer(const char *name)
{
	for (int i = 0; i < lengthof(stem_names); i++)
		if (pg_strcasecmp(name, stem_names[i].name) == 0)
			return stem_names[i].id;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown sift stemmer \"%s\"", name),
			 errhint("Valid stemmers are \"en\" and \"none\".")));
	pg_unreachable();
}

void
sift_analyze_validate_stemmer(const char *name)
{
	sift_analyze_stemmer(name);
}

/* Analyzer and buffers */

SiftAnalyzer *
sift_analyzer_create(const SiftAnalyzerConfig *cfg, MemoryContext mcxt)
{
	int			enc = GetDatabaseEncoding();
	SiftAnalyzer *a;

	if (enc != PG_UTF8 && enc != PG_SQL_ASCII)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift requires a UTF8 or SQL_ASCII database, not %s",
						GetDatabaseEncodingName())));
	a = MemoryContextAllocZero(mcxt, sizeof(SiftAnalyzer));
	a->cfg = *cfg;
	a->mcxt = mcxt;
	a->utf8 = enc == PG_UTF8;
	a->mode = (cfg->case_fold ? 1 : 0) | (cfg->accent_fold ? 2 : 0);
	Assert(cfg->stemmer < SIFT_STEM_COUNT);
	a->stem = stem_fns[cfg->stemmer];
	for (int c = 0; c < 128; c++)
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
			a->ascii[c] = cfg->case_fold ? pg_ascii_tolower(c) : c;
	return a;
}

int
sift_analyzer_prefix_len(const SiftAnalyzer *a)
{
	return a->cfg.prefix_len;
}

void
sift_analyzer_reset(SiftAnalyzer *a)
{
#define DROP_BIG(p, cap, sz) \
	if ((cap) * (sz) > KEEP_BYTES) { pfree(p); (p) = NULL; (cap) = 0; }
	DROP_BIG(a->buf, a->bufcap, 1);
	DROP_BIG(a->toks, a->tokcap, sizeof(SiftToken));
	DROP_BIG(a->bag, a->bagcap, sizeof(BagTerm));
	DROP_BIG(a->terms, a->termcap, sizeof(SiftTermFreq));
	DROP_BIG(a->slots, a->slotcap, sizeof(int32));
#undef DROP_BIG
}

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

/*
 * Scans the next token of s[*off..len) into dst (capacity DST_CAP) and
 * returns its length, or -1 at the end. Sets *start and advances *off past
 * the token. Tokens that fold to nothing are skipped.
 */
static pg_attribute_always_inline int
scan_token(SiftAnalyzer *a, const uint8 *s, int len, int *off, int *start, char *dst)
{
	int			i = *off;

	for (;;)
	{
		int			n = 0,
					cap = TERM_CAP,
					clen = 1,
					cls = C_WORD;
		uint32		cp = 0;
		uint16		info = 0;

#ifdef __SSE2__
		if ((n = scan_ascii(a, s, len, &i, start, dst)) >= 0)
		{
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

/* Document bag */

static inline int
term_cmp(const SiftTermFreq *x, const SiftTermFreq *y)
{
	int			c;

	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	c = memcmp(x->term, y->term, Min(x->len, y->len));
	return c ? c : (int) x->len - (int) y->len;
}

/* Empties slots[0..nslots) and reinserts the first nterms terms */
static void
bag_table(SiftAnalyzer *a, uint32 nslots, int nterms)
{
	ENSURE(a, slots, slotcap, nslots);
	memset(a->slots, 0xFF, nslots * sizeof(int32));
	a->nslots = nslots;
	a->shift = 32 - pg_leftmost_one_pos32(nslots);
	for (int t = 0; t < nterms; t++)
	{
		uint32		s = (uint32) (a->bag[t].h >> 32) >> a->shift;

		while (a->slots[s] >= 0)
			s = (s + 1) & (nslots - 1);
		a->slots[s] = t;
	}
}

/*
 * Counts tf occurrences of the n-byte term at a->buf + *used and returns its
 * bag index; a new term keeps its bytes (*used moves past them).
 */
static inline int
bag_add(SiftAnalyzer *a, size_t *used, int *nterms, int n, int tf)
{
	const char *dst = a->buf + *used;
	uint64		h = hash64(dst, n);
	uint32		slot,
				mask = a->nslots - 1;
	int32		t;

	for (slot = (uint32) (h >> 32) >> a->shift; (t = a->slots[slot]) >= 0; slot = (slot + 1) & mask)
		if (a->bag[t].h == h && a->bag[t].len == n && same_bytes(a->buf + a->bag[t].off, dst, n))
		{
			a->bag[t].tf += tf;
			return t;
		}
	ENSURE(a, bag, bagcap, *nterms + 1);
	a->bag[*nterms] = (BagTerm) {.h = h, .off = *used, .tf = tf, .len = n};
	a->slots[slot] = *nterms;
	*used += n;
	if ((uint32) ++*nterms * 2 > a->nslots)
		bag_table(a, a->nslots * 2, *nterms);
	return *nterms - 1;
}

/* Counts the pair "SEP bag[l] SEP bag[r]" of two adjacent tokens, unless it exceeds a term */
static void
bag_add_pair(SiftAnalyzer *a, size_t *used, int *nterms, int l, int r)
{
	int			ll = a->bag[l].len,
				rl = a->bag[r].len;
	char	   *dst;

	if (2 + ll + rl > SIFT_MAX_TERM_BYTES)
		return;
	ENSURE(a, buf, bufcap, *used + 2 + ll + rl + DST_CAP);
	dst = a->buf + *used;
	dst[0] = SIFT_PAIR_SEP;
	memcpy(dst + 1, a->buf + a->bag[l].off, ll);
	dst[1 + ll] = SIFT_PAIR_SEP;
	memcpy(dst + 2 + ll, a->buf + a->bag[r].off, rl);
	bag_add(a, used, nterms, 2 + ll + rl, 1);
}

/* Byte length of the first nchars characters of w[0..len) */
static int
prefix_bytes(const SiftAnalyzer *a, const char *w, int len, int nchars)
{
	int			i = 0;

	for (; nchars > 0 && i < len; nchars--)
		for (i++; a->utf8 && i < len && ((uint8) w[i] & 0xC0) == 0x80; i++)
			;
	return i;
}

/*
 * Counts the prefix terms "SEP prefix" of the words bag[0..nwords) (their
 * lengths 1..prefix_len in characters, or the whole word if shorter), each
 * once per occurrence of the word.
 */
static void
bag_add_prefixes(SiftAnalyzer *a, size_t *used, int *nterms, int nwords)
{
	for (int t = 0; t < nwords; t++)
	{
		const BagTerm *w = &a->bag[t];
		int			tf = w->tf,
					wlen = w->len,
					prev = 0;

		if (SIFT_SYNTHETIC_TERM(a->buf + w->off))
			continue;
		for (int l = 1; l <= a->cfg.prefix_len && prev < wlen; l++)
		{
			int			n = prefix_bytes(a, a->buf + a->bag[t].off, wlen, l);

			ENSURE(a, buf, bufcap, *used + 1 + n + DST_CAP);
			a->buf[*used] = SIFT_PREFIX_SEP;
			memcpy(a->buf + *used + 1, a->buf + a->bag[t].off, n);
			bag_add(a, used, nterms, 1 + n, tf);
			prev = n;
		}
	}
}

/*
 * Terms live in a linear-probing table indexed by the top bits of their
 * 32-bit hash, so walking it yields them almost sorted; an insertion pass
 * fixes probe displacement, wrap-around and equal hashes.
 */
int
sift_analyze_doc(SiftAnalyzer *a, const char *text, int len, SiftTermFreq **out, uint32 *dl)
{
	const uint8 *s = (const uint8 *) text;
	size_t		used = 0;
	uint64		ntok = 0;
	int			nterms = 0,
				off = 0,
				start,
				n,
				k = 0,
				prev = -1,
				cur;

	bag_table(a, pg_nextpower2_32(Min(Max(len / 4, 64), 1 << 16)), 0);
	for (;;)
	{
		ENSURE(a, buf, bufcap, used + DST_CAP);
		if ((n = scan_token(a, s, len, &off, &start, a->buf + used)) < 0)
			break;
		ntok++;
		cur = bag_add(a, &used, &nterms, n, 1);
		if (a->cfg.pairs && ntok > 1)
			bag_add_pair(a, &used, &nterms, prev, cur);
		prev = cur;
	}
	if (a->cfg.prefix_len)
		bag_add_prefixes(a, &used, &nterms, nterms);

	/* compact the occupied slots in place, without branches */
	for (uint32 slot = 0; slot < a->nslots; slot++)
	{
		a->slots[k] = a->slots[slot];
		k += a->slots[slot] >= 0;
	}
	ENSURE(a, terms, termcap, nterms);
	for (k = 0; k < nterms; k++)
	{
		BagTerm    *b = &a->bag[a->slots[k]];

		a->terms[k] = (SiftTermFreq)
		{
			.term = a->buf + b->off, .len = b->len, .tf = Min(b->tf, SIFT_MAX_TF),
				.hash = (uint32) (b->h >> 32)
		};
		for (int j = k; j > 0 && term_cmp(&a->terms[j - 1], &a->terms[j]) > 0; j--)
		{
			SiftTermFreq tmp = a->terms[j];

			a->terms[j] = a->terms[j - 1];
			a->terms[j - 1] = tmp;
		}
	}
	*dl = Min(ntok, SIFT_MAX_DL);
	*out = a->terms;
	return nterms;
}

/* Literal folding */

int
sift_analyze_fold(SiftAnalyzer *a, const char *str, int len, char **out)
{
	const uint8 *s = (const uint8 *) str;
	int			n = 0,
				cap = PG_INT32_MAX,
				clen;
	uint32		cp;

	ENSURE(a, buf, bufcap, (size_t) len * SIFT_FOLD_MAX_EXPAND + 1);
	for (int i = 0; i < len; i += clen)
	{
		uint16		info;

		clen = 1;
		if (s[i] < 0x80)
			a->buf[n++] = a->ascii[s[i]] ? a->ascii[s[i]] : s[i];
		else if (!a->utf8 || !(clen = utf8_decode(s + i, len - i, &cp)))
			a->buf[n++] = s[i], clen = 1;
		else if ((info = cp_info(cp)) >> 13 == C_WORD || info >> 13 == C_MARK ||
				 info >> 13 == C_IDEO)
			append_fold(a, info, s + i, clen, a->buf, &n, &cap);
		else if (info >> 13 != C_IGN)
		{
			memcpy(a->buf + n, s + i, clen);
			n += clen;
		}
	}
	a->buf[n] = '\0';
	*out = a->buf;
	return n;
}
