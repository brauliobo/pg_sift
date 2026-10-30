/*
 * bm25/analyze.c - tokenizer, case and accent folding, stemming, term hashing.
 * Rules are documented in bm25/analyze.h. One scanner (scan_token) serves the
 * token stream, the streaming cursor and the document bag, so index and query
 * analysis cannot diverge (the scanner is bm25/analyze_scan.h, included where it stood).
 */
#include "core/host.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#include <ctype.h>

#include "bm25/analyze.h"
#include "bm25/analyze_tables.h"
#include "core/varint.h"
#include "core/cmp.h"

/* Code point classes (bm25/analyze_tables.h entries >> 13) */
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
	char	   *kw;				/* keyword analyzer: the folded text */
	size_t		kwcap;
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

	bool		positions;		/* the bag also carries the occurrences of the word terms */
	int32	   *tokidx;			/* token -> bag index */
	size_t		tokidxcap;
	uint32	   *pstart;			/* bag index -> first slot of its positions in plist, then past the last */
	size_t		pstartcap;
	uint32	   *plist;			/* positions grouped by bag index */
	size_t		plistcap;
	uint8	   *pbuf;			/* encoded position records */
	size_t		pbufcap;

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
						GetDatabaseEncodingName()),
				 errhint("Create a database with ENCODING 'UTF8' and restore the data into it.")));
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

int
sift_analyzer_nfields(const SiftAnalyzer *a)
{
	return a->cfg.nfields;
}

const char *
sift_analyzer_field_names(const SiftAnalyzer *a)
{
	return a->cfg.field_names;
}

int
sift_analyzer_field(const SiftAnalyzer *a, const char *name, int len)
{
	int			k = 1;

	for (const char *s = a->cfg.field_names; *s; k++)
	{
		const char *e = strchr(s, ',') ? strchr(s, ',') : s + strlen(s);

		if (e - s == len && pg_strncasecmp(s, name, len) == 0)
			return k;
		s = *e ? e + 1 : e;
	}
	return 0;
}

void
sift_analyzer_reset(SiftAnalyzer *a)
{
#define DROP_BIG(p, cap, sz) \
	if ((cap) * (sz) > KEEP_BYTES) { pfree(p); (p) = NULL; (cap) = 0; }
	DROP_BIG(a->buf, a->bufcap, 1);
	DROP_BIG(a->kw, a->kwcap, 1);
	DROP_BIG(a->toks, a->tokcap, sizeof(SiftToken));
	DROP_BIG(a->bag, a->bagcap, sizeof(BagTerm));
	DROP_BIG(a->terms, a->termcap, sizeof(SiftTermFreq));
	DROP_BIG(a->slots, a->slotcap, sizeof(int32));
	DROP_BIG(a->tokidx, a->tokidxcap, sizeof(int32));
	DROP_BIG(a->pstart, a->pstartcap, sizeof(uint32));
	DROP_BIG(a->plist, a->plistcap, sizeof(uint32));
	DROP_BIG(a->pbuf, a->pbufcap, 1);
#undef DROP_BIG
}

#include "bm25/analyze_scan.h"

/* Document bag */

static inline int
term_cmp(const SiftTermFreq *x, const SiftTermFreq *y)
{
	int			c;

	if (x->hash != y->hash)
		return SIFT_CMP3(x->hash, y->hash);
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
 * Counts the prefix terms "SEP prefix" (or "field SEP prefix") of the words
 * and field words bag[0..nwords) (their lengths 1..prefix_len in characters,
 * or the whole word if shorter), each once per occurrence of the word.
 */
static void
bag_add_prefixes(SiftAnalyzer *a, size_t *used, int *nterms, int nwords)
{
	for (int t = 0; t < nwords; t++)
	{
		const char *b = a->buf + a->bag[t].off;
		int			tf = a->bag[t].tf,
					skip = 0,
					wlen,
					prev = 0;
		char		field = 0;

		if (SIFT_FIELD_TERM(b[0], a->cfg.nfields))
			field = b[0], skip = 1;
		else if (SIFT_SYNTHETIC_TERM(b))
			continue;
		wlen = a->bag[t].len - skip;
		for (int l = 1; l <= a->cfg.prefix_len && prev < wlen; l++)
		{
			int			n = prefix_bytes(a, a->buf + a->bag[t].off + skip, wlen, l);
			char	   *dst;

			ENSURE(a, buf, bufcap, *used + 2 + n + DST_CAP);
			dst = a->buf + *used;
			if (field)
				*dst++ = field;
			*dst++ = SIFT_PREFIX_SEP;
			memcpy(dst, a->buf + a->bag[t].off + skip, n);
			bag_add(a, used, nterms, (dst - (a->buf + *used)) + n, tf);
			prev = n;
		}
	}
}

/* Counts the field word "field-byte word" of the word the bag holds at index t, unless it exceeds a term */
static void
bag_add_field_word(SiftAnalyzer *a, size_t *used, int *nterms, int t, int field)
{
	int			n = a->bag[t].len;
	char	   *dst;

	if (1 + n > SIFT_MAX_TERM_BYTES)
		return;
	ENSURE(a, buf, bufcap, *used + 1 + n + DST_CAP);
	dst = a->buf + *used;
	dst[0] = SIFT_FIELD_BYTE(field);
	memcpy(dst + 1, a->buf + a->bag[t].off, n);
	bag_add(a, used, nterms, 1 + n, 1);
}

void
sift_analyzer_set_positions(SiftAnalyzer *a, bool on)
{
	a->positions = on;
}

/* Groups the positions of the ntok tokens by bag index (counting sort); the bag holds nbag terms */
static void
group_positions(SiftAnalyzer *a, int ntok, int nbag)
{
	ENSURE(a, pstart, pstartcap, (size_t) nbag + 1);
	ENSURE(a, plist, plistcap, ntok);
	memset(a->pstart, 0, ((size_t) nbag + 1) * sizeof(uint32));
	for (int t = 0; t < ntok; t++)
		a->pstart[a->tokidx[t] + 1]++;
	for (int b = 0; b < nbag; b++)
		a->pstart[b + 1] += a->pstart[b];
	/* fills each group from its start; the starts end up one group later, where the groups end */
	for (int t = 0; t < ntok; t++)
		a->plist[a->pstart[a->tokidx[t]]++] = t;
}

/* Appends the record of bag term b (occurrences in plist) to pbuf at *used; returns its length */
static uint32
put_positions(SiftAnalyzer *a, int b, size_t *used)
{
	uint32		n = a->pstart[b] - (b > 0 ? a->pstart[b - 1] : 0),
				*v = a->plist + a->pstart[b] - n;
	uint8	   *p;

	ENSURE(a, pbuf, pbufcap, *used + 5 * (size_t) n + 10);
	p = a->pbuf + *used;
	p = sift_put_varint(p, v[0] << 1 | (n > 1));
	if (n > 1)
		p = sift_put_varint(p, n - 2);
	for (uint32 i = 1; i < n; i++)
		p = sift_put_varint(p, v[i] - v[i - 1] - 1);
	n = p - (a->pbuf + *used);
	*used += n;
	return n;
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
				nbag = 0,
				off = 0,
				start,
				n,
				k = 0,
				cur,
				section = 0;
	const uint8 *sep = a->cfg.nfields ? memchr(s, SIFT_FIELD_SEP, len) : NULL;

	bag_table(a, pg_nextpower2_32(Min(Max(len / 4, 64), 1 << 16)), 0);
	for (;;)
	{
		ENSURE(a, buf, bufcap, used + DST_CAP);
		if ((n = scan_token(a, s, len, &off, &start, a->buf + used)) < 0)
			break;
		cur = bag_add(a, &used, &nterms, n, 1);
		if (a->positions)
		{
			ENSURE(a, tokidx, tokidxcap, ntok + 1);
			a->tokidx[ntok] = cur;
		}
		ntok++;
		for (; sep && sep < s + start; section++)
			sep = memchr(sep + 1, SIFT_FIELD_SEP, s + len - (sep + 1));
		if (section < a->cfg.nfields)
			bag_add_field_word(a, &used, &nterms, cur, section + 1);
	}
	if (a->positions && ntok > 0)
		group_positions(a, ntok, nbag = nterms);
	if (a->cfg.prefix_len)
		bag_add_prefixes(a, &used, &nterms, nterms);

	/* compact the occupied slots in place, without branches */
	for (uint32 slot = 0; slot < a->nslots; slot++)
	{
		a->slots[k] = a->slots[slot];
		k += a->slots[slot] >= 0;
	}
	ENSURE(a, terms, termcap, nterms);
	used = 0;
	for (k = 0; k < nterms; k++)
	{
		BagTerm    *b = &a->bag[a->slots[k]];
		int			t = a->slots[k];
		bool		word = t < nbag && a->pstart[t] > (t > 0 ? a->pstart[t - 1] : 0);

		a->terms[k] = (SiftTermFreq)
		{
			.term = a->buf + b->off, .len = b->len, .tf = Min(b->tf, SIFT_MAX_TF),
				.hash = (uint32) (b->h >> 32)
		};
		if (word)
		{
			/* an offset (+ 1) until the buffer stops growing */
			a->terms[k].pos = (const uint8 *) (uintptr_t) (used + 1);
			a->terms[k].poslen = put_positions(a, t, &used);
		}
		for (int j = k; j > 0 && term_cmp(&a->terms[j - 1], &a->terms[j]) > 0; j--)
		{
			SiftTermFreq tmp = a->terms[j];

			a->terms[j] = a->terms[j - 1];
			a->terms[j - 1] = tmp;
		}
	}
	for (k = 0; k < nterms; k++)
		if (a->terms[k].poslen)
			a->terms[k].pos = a->pbuf + ((uintptr_t) a->terms[k].pos - 1);
	*dl = Min(ntok, SIFT_MAX_DL);
	*out = a->terms;
	return nterms;
}

/* Literal folding */

/* Folds s[0..len) into dst (room for len * SIFT_FOLD_MAX_EXPAND bytes); returns the length */
static int
fold_chars(const SiftAnalyzer *a, const uint8 *s, int len, char *dst)
{
	int			n = 0,
				cap = PG_INT32_MAX,
				clen;
	uint32		cp;

	for (int i = 0; i < len; i += clen)
	{
		uint16		info;

		clen = 1;
		if (s[i] < 0x80)
			dst[n++] = a->ascii[s[i]] ? a->ascii[s[i]] : s[i];
		else if (!a->utf8 || !(clen = utf8_decode(s + i, len - i, &cp)))
			dst[n++] = s[i], clen = 1;
		else if ((info = cp_info(cp)) >> 13 == C_WORD || info >> 13 == C_MARK ||
				 info >> 13 == C_IDEO)
			append_fold(a, info, s + i, clen, dst, &n, &cap);
		else if (info >> 13 != C_IGN)
		{
			memcpy(dst + n, s + i, clen);
			n += clen;
		}
	}
	return n;
}

int
sift_analyze_fold(SiftAnalyzer *a, const char *str, int len, char **out)
{
	int			n;

	ENSURE(a, buf, bufcap, (size_t) len * SIFT_FOLD_MAX_EXPAND + 1);
	n = fold_chars(a, (const uint8 *) str, len, a->buf);
	a->buf[n] = '\0';
	*out = a->buf;
	return n;
}
