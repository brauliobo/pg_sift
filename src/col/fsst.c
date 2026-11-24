/*
 * col/fsst.c - the static symbol table of strings (col/fsst.h).
 *
 * The table is trained on a sample, five generations: each codes the sample with the table of the one before, counts every symbol (a byte no symbol covers counts as a symbol of its
 * own) and every pair of symbols that follow one another and are at most 8 bytes together, and keeps the 255 candidates that would save the most bytes (count times length).
 * The coding is greedy: the longest symbol that starts at the byte, else the escape and the byte.
 */
#include "core/host.h"

#include "col/fsst.h"
#include "core/par/sbuf.h"

#define GENERATIONS		5
#define LITERAL			256		/* the id of the byte b that no symbol covers is LITERAL + b */
#define IDS				(LITERAL + 256)

typedef struct Cand
{
	uint64		key;			/* the bytes, the first the lowest */
	uint64		gain;
	uint8		len;
} Cand;

static uint64
pack_bytes(const uint8 *b, uint32 len)
{
	uint64		k = 0;

	for (uint32 i = 0; i < len; i++)
		k |= (uint64) b[i] << (8 * i);
	return k;
}

static void
set_symbol(SiftFsst *t, uint32 code, const uint8 *bytes, uint32 len)
{
	uint8		mask[8] = {0};

	memset(t->sym[code], 0, SIFT_FSST_SYMBOL_BYTES);
	memcpy(t->sym[code], bytes, len);
	memset(mask, 0xFF, len);
	t->len[code] = (uint8) len;
	memcpy(&t->word[code], t->sym[code], 8);
	memcpy(&t->mask[code], mask, 8);
}

/* The symbols by first byte, the longest first, and the escape in place */
static void
index_table(SiftFsst *t)
{
	uint16		at[256];

	set_symbol(t, SIFT_FSST_ESCAPE, (const uint8 *) "", 0);
	t->len[SIFT_FSST_ESCAPE] = 1;
	memset(t->first, 0, sizeof(t->first));
	for (uint32 c = 0; c < t->nsym; c++)
		t->first[t->sym[c][0] + 1]++;
	for (int b = 0; b < 256; b++)
		t->first[b + 1] += t->first[b];
	memcpy(at, t->first, sizeof(at));
	for (uint32 c = 0; c < t->nsym; c++)
	{
		uint32		i = at[t->sym[c][0]]++;

		while (i > t->first[t->sym[c][0]] && t->len[t->order[i - 1]] < t->len[c])
		{
			t->order[i] = t->order[i - 1];
			i--;
		}
		t->order[i] = (uint8) c;
	}
}

/* The code of the longest symbol the n bytes at s start with, or -1 */
static inline int
longest(const SiftFsst *t, const uint8 *s, uint32 n)
{
	uint64		w = 0;
	uint32		b = s[0];

	if (n >= 8)
		memcpy(&w, s, 8);
	for (uint32 i = t->first[b]; i < t->first[b + 1]; i++)
	{
		uint32		code = t->order[i],
					l = t->len[code];

		if (l > n)
			continue;
		if (n >= 8 ? ((w ^ t->word[code]) & t->mask[code]) == 0 : memcmp(t->sym[code], s, l) == 0)
			return (int) code;
	}
	return -1;
}

uint32
sift_fsst_encode(const SiftFsst *t, const char *str, uint32 len, uint8 *out)
{
	const uint8 *s = (const uint8 *) str;
	uint32		o = 0;

	for (uint32 i = 0; i < len;)
	{
		int			code = longest(t, s + i, len - i);

		if (code >= 0)
		{
			out[o++] = (uint8) code;
			i += t->len[code];
		}
		else
		{
			out[o++] = SIFT_FSST_ESCAPE;
			out[o++] = s[i++];
		}
	}
	return o;
}

uint32
sift_fsst_encode_safe_prefix(const SiftFsst *t, const char *prefix, uint32 plen, uint8 *out, uint32 *raw_len)
{
	const uint8 *s = (const uint8 *) prefix;
	uint32		i = 0,
				o = 0;

	while (i < plen)
	{
		uint32		b = s[i],
					longest_sym = t->first[b] < t->first[b + 1] ? t->len[t->order[t->first[b]]] : 1;
		int			code;

		if (i + longest_sym > plen)
			break;
		code = longest(t, s + i, plen - i);
		if (code >= 0)
		{
			out[o++] = (uint8) code;
			i += t->len[code];
		}
		else
		{
			out[o++] = SIFT_FSST_ESCAPE;
			out[o++] = s[i++];
		}
	}
	*raw_len = i;
	return o;
}

static int
cand_by_bytes(const void *a, const void *b)
{
	const Cand *x = a,
			   *y = b;

	if (x->len != y->len)
		return x->len < y->len ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

static int
cand_by_gain(const void *a, const void *b)
{
	const Cand *x = a,
			   *y = b;

	if (x->gain != y->gain)
		return x->gain > y->gain ? -1 : 1;
	return cand_by_bytes(b, a);
}

static int
pair_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

typedef struct Sample
{
	const char **p;
	uint32	   *len;
	uint32		n;
	uint64		bytes;
} Sample;

/* The strings every k-th of the n, k so that they are about SIFT_FSST_SAMPLE_BYTES */
static Sample
take_sample(const char *const *p, const uint32 *len, uint32 n)
{
	uint64		total = 0;
	uint32		stride;
	Sample		s = {sift_alloc(sizeof(char *) * (n + 1)), sift_alloc(sizeof(uint32) * (n + 1)), 0, 0};

	for (uint32 i = 0; i < n; i++)
		total += len[i];
	stride = total <= SIFT_FSST_SAMPLE_BYTES ? 1 : (uint32) Min(total / SIFT_FSST_SAMPLE_BYTES, n);
	for (uint32 i = 0; i < n && s.bytes < 2 * SIFT_FSST_SAMPLE_BYTES; i += Max(stride, 1))
	{
		s.p[s.n] = p[i];
		s.len[s.n++] = len[i];
		s.bytes += len[i];
	}
	return s;
}

static inline uint32
id_len(const SiftFsst *t, uint32 id)
{
	return id < LITERAL ? t->len[id] : 1;
}

static inline void
id_bytes(const SiftFsst *t, uint32 id, uint8 *out)
{
	if (id < LITERAL)
		memcpy(out, t->sym[id], t->len[id]);
	else
		out[0] = (uint8) (id - LITERAL);
}

/* The coded size of the sample, and with count the symbols and pairs it used (pairs: n entries of prev << 10 | id, at most as many as the sample has codes) */
static uint64
code_sample(const SiftFsst *t, const Sample *sm, uint32 *single, uint32 *pairs, uint32 *npairs)
{
	uint64		coded = 0;

	for (uint32 k = 0; k < sm->n; k++)
	{
		const uint8 *s = (const uint8 *) sm->p[k];
		uint32		prev = IDS;

		for (uint32 i = 0; i < sm->len[k];)
		{
			int			code = longest(t, s + i, sm->len[k] - i);
			uint32		id = code >= 0 ? (uint32) code : LITERAL + s[i];
			uint32		l = id_len(t, id);

			coded += code >= 0 ? 1 : 2;
			if (single != NULL)
			{
				single[id]++;
				if (prev != IDS && id_len(t, prev) + l <= SIFT_FSST_SYMBOL_BYTES)
					pairs[(*npairs)++] = prev << 10 | id;
			}
			prev = id;
			i += l;
		}
	}
	return coded;
}

static uint32
add_cand(Cand *c, uint32 n, const uint8 *bytes, uint32 len, uint64 gain)
{
	c[n] = (Cand) {pack_bytes(bytes, len), gain, (uint8) len};
	return n + 1;
}

/* The next table: the candidates of the symbols and pairs the sample used, the same bytes merged, the 255 of most gain kept */
static void
next_table(SiftFsst *t, const Sample *sm)
{
	uint32	   *single = sift_alloc0(IDS * sizeof(uint32)),
			   *pairs = sift_alloc((sm->bytes + 1) * sizeof(uint32));
	uint32		npairs = 0,
				nc = 0,
				m = 0;
	Cand	   *c;
	uint8		b[SIFT_FSST_SYMBOL_BYTES];

	(void) code_sample(t, sm, single, pairs, &npairs);
	qsort(pairs, npairs, sizeof(uint32), pair_cmp);
	c = sift_alloc((IDS + npairs + 1) * sizeof(Cand));
	for (uint32 id = 0; id < IDS; id++)
		if (single[id] > 0)
		{
			id_bytes(t, id, b);
			nc = add_cand(c, nc, b, id_len(t, id), (uint64) single[id] * id_len(t, id));
		}
	for (uint32 i = 0; i < npairs;)
	{
		uint32		j = i,
					la,
					lb;

		while (j < npairs && pairs[j] == pairs[i])
			j++;
		la = id_len(t, pairs[i] >> 10);
		lb = id_len(t, pairs[i] & 1023);
		id_bytes(t, pairs[i] >> 10, b);
		id_bytes(t, pairs[i] & 1023, b + la);
		nc = add_cand(c, nc, b, la + lb, (uint64) (j - i) * (la + lb));
		i = j;
	}
	qsort(c, nc, sizeof(Cand), cand_by_bytes);
	for (uint32 i = 0; i < nc; i++)
		if (m > 0 && c[m - 1].len == c[i].len && c[m - 1].key == c[i].key)
			c[m - 1].gain += c[i].gain;
		else
			c[m++] = c[i];
	qsort(c, m, sizeof(Cand), cand_by_gain);
	t->nsym = Min(m, SIFT_FSST_MAX_SYMBOLS);
	for (uint32 i = 0; i < t->nsym; i++)
	{
		for (uint32 k = 0; k < c[i].len; k++)
			b[k] = (uint8) (c[i].key >> (8 * k));
		set_symbol(t, i, b, c[i].len);
	}
	index_table(t);
	sift_free(single);
	sift_free(pairs);
	sift_free(c);
}

uint32
sift_fsst_train(SiftFsst *t, const char *const *p, const uint32 *len, uint32 n)
{
	Sample		sm = take_sample(p, len, n);
	uint64		coded;

	memset(t, 0, sizeof(*t));
	index_table(t);
	for (int g = 0; g < GENERATIONS && sm.bytes > 0; g++)
		next_table(t, &sm);
	coded = sm.bytes > 0 ? code_sample(t, &sm, NULL, NULL, NULL) : 0;
	sift_free(sm.p);
	sift_free(sm.len);
	return sm.bytes > 0 ? (uint32) (coded * 1000 / sm.bytes) : 1000;
}

void
sift_fsst_put_table(StringInfo s, const SiftFsst *t)
{
	uint32		hdr = t->nsym;
	uint32		start = s->len;

	sift_sb_append(s, (char *) &hdr, 4);
	for (uint32 c = 0; c < t->nsym; c++)
		sift_sb_char(s, (char) t->len[c]);
	for (uint32 c = 0; c < t->nsym; c++)
		sift_sb_append(s, (const char *) t->sym[c], t->len[c]);
	while ((s->len - start) % 8 != 0)
		sift_sb_char(s, '\0');
}

uint32
sift_fsst_read_table(SiftFsst *t, const uint8 *p, uint32 avail)
{
	uint32		nsym,
				at;

	if (avail < 4)
		return 0;
	memcpy(&nsym, p, 4);
	if (nsym > SIFT_FSST_MAX_SYMBOLS || avail < 4 + nsym)
		return 0;
	memset(t, 0, sizeof(*t));
	t->nsym = nsym;
	at = 4 + nsym;
	for (uint32 c = 0; c < nsym; c++)
	{
		uint32		l = p[4 + c];

		if (l < 1 || l > SIFT_FSST_SYMBOL_BYTES || avail - at < l)
			return 0;
		set_symbol(t, c, p + at, l);
		at += l;
	}
	index_table(t);
	return (at + 7) & ~(uint32) 7;
}

/* The codes in [in, end) decoded to *outp: an escape needs its byte, which a string ends before when the pointer returned is past end */
static inline const uint8 *
decode_run(const SiftFsst *t, const uint8 *in, const uint8 *end, uint8 **outp)
{
	uint8	   *out = *outp;

	while (in < end)
	{
		uint32		code = in[0];
		uint8		esc;

		if (in + 1 < end && code != SIFT_FSST_ESCAPE && in[1] != SIFT_FSST_ESCAPE)
		{
			const uint32 next = in[1];
			const uint32 first = t->len[code];

			memcpy(out, t->sym[code], SIFT_FSST_SYMBOL_BYTES);
			memcpy(out + first, t->sym[next], SIFT_FSST_SYMBOL_BYTES);
			out += first + t->len[next];
			in += 2;
			continue;
		}
		esc = (uint8) -(code == SIFT_FSST_ESCAPE);

		memcpy(out, t->sym[code], SIFT_FSST_SYMBOL_BYTES);
		out[0] |= in[1] & esc;
		out += t->len[code];
		in += 1 + (code == SIFT_FSST_ESCAPE);
	}
	*outp = out;
	return in;
}

uint32
sift_fsst_decode(const SiftFsst *t, const uint8 *in, uint32 inlen, uint8 *out)
{
	uint8	   *o = out;

	if (decode_run(t, in, in + inlen, &o) != in + inlen)
		return UINT32_MAX;
	return (uint32) (o - out);
}

/* A string that may not fit what is left of the output: each code is checked */
static bool
decode_checked(const SiftFsst *t, const uint8 *in, const uint8 *end, uint8 *out, uint64 cap, uint8 **outp)
{
	uint8	   *o = *outp;

	while (in < end)
	{
		uint32		code = in[0],
					l = t->len[code];
		bool		esc = code == SIFT_FSST_ESCAPE;

		if ((uint64) (o - out) + l + SIFT_FSST_SLACK > cap || (esc && in + 1 >= end))
			return false;
		memcpy(o, t->sym[code], SIFT_FSST_SYMBOL_BYTES);
		o[0] |= esc ? in[1] : 0;
		o += l;
		in += 1 + esc;
	}
	*outp = o;
	return true;
}

bool
sift_fsst_decode_strs(const SiftFsst *t, const uint8 *enc, const uint32 *off, uint32 n, uint8 *out, uint64 cap, uint32 *outoff)
{
	uint8	   *o = out;

	outoff[0] = 0;
	for (uint32 i = 0; i < n; i++)
	{
		const uint8 *in = enc + off[i],
				   *end = enc + off[i + 1];

		bool		room;

		if (off[i + 1] < off[i])
			return false;
		room = (uint64) (o - out) + (uint64) 8 * (off[i + 1] - off[i]) + SIFT_FSST_SLACK <= cap;
		if (!(room ? decode_run(t, in, end, &o) == end : decode_checked(t, in, end, out, cap, &o)))
			return false;
		outoff[i + 1] = (uint32) (o - out);
	}
	return true;
}
