/*
 * col/codec/chunk.c - the chunk codec on pool threads: chunks written by the real encoder (every encoding of the cascade, LZ4, zstd and zstd with a
 * dictionary, string lists, bit vectors) are opened, decoded and filtered by the *_a entrypoints of col/codec/codec.h on 4 threads, each in an arena of its own,
 * and must equal what the backend routines give on the set-up thread, bit for bit, at every SIMD level; damaged blobs end in a SiftErr and never in a
 * crash or a leak; an arena too small ends in SIFT_ERR_ARENA_FULL; the errors of a worker are those of the backend path. The palloc/ereport trap
 * of trapshim.h is armed on every worker.
 */
#include "postgres.h"

#include <stdatomic.h>

#include "col/codec/int.h"
#include "col/compress.h"
#include "col/fsst.h"
#include "core/testrng.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

typedef SiftRng CcgRng;
#define ccg_next sift_rng_next
#include "col/codec/gen.h"

extern void kt_cpu_init(void);

#define WORKERS		4
#define ARENA_CAP	(2u << 20)
#define NSEL		4
#define NRANGE		6
#define NMEMBER		256
#define NGETS		8
#define SLOTS(n)	((size_t) (n) + 64)

static const uint32 sizes[] = {0, 1, 33, 1000, 2048};
static const char *const policies[] = {"auto", "lz4", "zstd", "delta+zstd", "rle+dict+zstd:3", "for+delta+rle+dict+alp+scale+zstd:3"};

static void *
xalloc(size_t n)
{
	void	   *p;

	SIFT_CHECK(posix_memalign(&p, 64, n ? n : 1) == 0);
	return p;
}

/* ---- what one chunk answers to every read: the reference is the backend routines on the set-up thread, a worker's is the *_a ones ---- */

typedef struct Out
{
	int64	   *dec,
			   *head,
			   *sel[NSEL],
			   *gets,
			   *sums;
	uint32	   *u32,
			   *off;
	double	   *f8;
	uint64	   *bits[NRANGE],
			   *member,
			   *counts;
	bool		fits;
	uint8	   *mem;
	size_t		size;
} Out;

static Out
out_new(uint32 n)
{
	size_t		slots = SLOTS(n),
				words = SIFT_COL_NWORDS(n) + 8;
	Out			o = {0};
	uint8	   *p;

	o.size = 8 * (slots * (5 + NSEL) + words * (NRANGE + 1) + 48);
	p = o.mem = xalloc(o.size);
#define CARVE(field, bytes) ((field) = (void *) p, p += (bytes))
	CARVE(o.dec, 8 * slots);
	CARVE(o.head, 8 * slots);
	CARVE(o.f8, 8 * slots);
	for (int g = 0; g < NSEL; g++)
		CARVE(o.sel[g], 8 * slots);
	CARVE(o.u32, 8 * slots);
	CARVE(o.off, 8 * slots);
	for (int r = 0; r < NRANGE; r++)
		CARVE(o.bits[r], 8 * words);
	CARVE(o.member, 8 * words);
	CARVE(o.gets, 8 * 16);
	CARVE(o.sums, 8 * 16);
	CARVE(o.counts, 8 * 16);
#undef CARVE
	return o;
}

typedef struct Case
{
	const char *policy;
	uint32		n;
	SiftCcKind	kind;
	uint8	   *blob;
	Size		len;
	uint32		headw,
				nsel[NSEL],
			   *sel[NSEL],
				rows[NGETS];
	int64		lo[NRANGE],
				hi[NRANGE];
	uint64		member[NMEMBER / 64];
	Out			ref;
} Case;

static void
out_poison(Out *o)
{
	memset(o->mem, 0xA5, o->size);
}

typedef struct Ops
{
	SiftArena  *a;
	SiftErr		err;
} Ops;

/* The reads on both sides of the one table: a NULL arena is the backend routine */
#define OP(o, name, ...) \
	((o)->a ? sift_cc_##name##_a((o)->a, &(o)->err, __VA_ARGS__) : (sift_cc_##name(__VA_ARGS__), true))

static bool
op_open(Ops *o, const Case *k, SiftCc **cc)
{
	if (o->a)
		return sift_cc_open_rows_a(o->a, &o->err, k->blob, k->len, true, k->n, cc);
	*cc = sift_cc_open_rows(CurrentMemoryContext, k->blob, k->len, true, k->n);
	return true;
}

static bool
op_get(Ops *o, SiftCc *cc, uint32 row, int64 *key)
{
	if (o->a)
		return sift_cc_get_a(o->a, &o->err, cc, row, key);
	*key = sift_cc_get(cc, row);
	return true;
}

static bool
op_off32(Ops *o, SiftCc *cc, uint32 *dst, bool *fits)
{
	if (o->a)
		return sift_cc_decode_off32_a(o->a, &o->err, cc, dst, fits);
	*fits = sift_cc_decode_off32(cc, dst);
	return true;
}

static bool
op_sum(Ops *o, SiftCc *cc, const uint64 *bits, int64 *sum, uint64 *count)
{
	if (o->a)
		return sift_cc_sum_a(o->a, &o->err, cc, bits, sum, count);
	*count = sift_cc_sum(cc, bits, sum);
	return true;
}

static bool
run_reads(Ops *o, const Case *k, SiftCc *cc, Out *out)
{
	uint32		n = k->n;

	if (!OP(o, decode, cc, out->dec) || !OP(o, decode_head, cc, k->headw, out->head) || !OP(o, decode_u32, cc, out->u32) ||
		(k->kind != SIFT_CC_INT && !OP(o, decode_f8, cc, out->f8)) || !op_off32(o, cc, out->off, &out->fits))
		return false;
	for (int g = 0; g < NSEL; g++)
		if (!OP(o, decode_sel, cc, k->sel[g], k->nsel[g], out->sel[g]))
			return false;
	for (int g = 0; g < NGETS && n; g++)
		if (!op_get(o, cc, k->rows[g], &out->gets[g]))
			return false;
	for (int r = 0; r < NRANGE; r++)
		if (!OP(o, filter, cc, k->lo[r], k->hi[r], out->bits[r]) || !op_sum(o, cc, out->bits[r], &out->sums[r], &out->counts[r]))
			return false;
	return op_sum(o, cc, NULL, &out->sums[NRANGE], &out->counts[NRANGE]) && OP(o, filter_member, cc, k->member, NMEMBER, out->member);
}

/* Opens, reads, closes; false with o->err for a worker */
static bool
run_case(Ops *o, const Case *k, Out *out)
{
	SiftCc	   *cc;
	bool		ok;

	out_poison(out);
	if (!op_open(o, k, &cc))
		return false;
	ok = run_reads(o, k, cc, out);
	if (!o->a)
		sift_cc_close(cc);
	return ok;
}

#define SAME(a, b, cnt, sz) SIFT_CHECK((cnt) == 0 || memcmp((a), (b), (size_t) (cnt) * (sz)) == 0)

static void
out_same(const Case *k, const Out *x, const Out *y)
{
	uint32		n = k->n,
				nw = (n + 63) / 64;

	SAME(x->dec, y->dec, n, 8);
	SAME(x->head, y->head, Min(k->headw, n), 8);
	SAME(x->u32, y->u32, n, 4);
	if (k->kind != SIFT_CC_INT)
		SAME(x->f8, y->f8, n, 8);
	SIFT_CHECK(x->fits == y->fits);
	if (x->fits)
		SAME(x->off, y->off, n, 4);
	for (int g = 0; g < NSEL; g++)
		SAME(x->sel[g], y->sel[g], k->nsel[g], 8);
	SAME(x->gets, y->gets, n ? NGETS : 0, 8);
	for (int r = 0; r < NRANGE; r++)
		SAME(x->bits[r], y->bits[r], nw, 8);
	SAME(x->sums, y->sums, NRANGE + 1, 8);
	SAME(x->counts, y->counts, NRANGE + 1, 8);
	SAME(x->member, y->member, nw, 8);
}

/* ---- the corpus ---- */

static struct
{
	int			roots[16];
	int			wraps[4];
} seen;

static uint8 *
aligned_copy(const StringInfoData *s)
{
	uint8	   *p = xalloc(s->len);

	SIFT_CHECK((s->len & 7) == 0);
	memcpy(p, s->data, s->len);
	return p;
}

static void
case_probes(SiftRng *r, Case *k, const int64 *v)
{
	uint32		n = k->n;
	int64		a = n ? v[sift_rng_below(r, n)] : 0,
				b = n ? v[sift_rng_below(r, n)] : 0;

	k->headw = n / 3 + 1;
	k->lo[0] = INT64_MIN, k->hi[0] = INT64_MAX;
	k->lo[1] = 5, k->hi[1] = 4;
	k->lo[2] = a, k->hi[2] = a;
	k->lo[3] = Min(a, b), k->hi[3] = Max(a, b);
	k->lo[4] = INT64_MIN, k->hi[4] = a;
	k->lo[5] = a, k->hi[5] = INT64_MAX;
	for (int g = 0; g < NSEL; g++)
	{
		uint32		m = n == 0 ? 0 : g == 0 ? Min(n - n / 4, 600) : g == 1 ? Min(n, 7) : g == 2 ? Min(n, 300) : Max(n / 2, 1);

		k->nsel[g] = m;
		k->sel[g] = xalloc(sizeof(uint32) * (m + 1));
		for (uint32 i = 0; i < m; i++)
			k->sel[g][i] = g == 0 ? n / 4 + i : g == 1 ? Min((uint32) ((uint64) i * n / m + sift_rng_below(r, Max(n / m, 1))), n - 1) :
				g == 2 ? sift_rng_below(r, n) : Min(2 * i, n - 1);
	}
	for (int g = 0; g < NGETS && n; g++)
		k->rows[g] = (uint32) ((uint64) g * 2654435761u % n);
	for (int i = 0; i < NMEMBER / 64; i++)
		k->member[i] = sift_rng_next(r) & sift_rng_next(r);
}

static void
add_case(SiftRng *r, Case **cases, int *ncases, int *cap, const char *pol, SiftCcKind kind, const int64 *v, uint32 n)
{
	StringInfoData s;
	SiftCcPolicy p;
	SiftCcInfo	info;
	Case	   *k;
	Ops			o = {0};

	if (*ncases == *cap)
		*cases = realloc(*cases, sizeof(Case) * (*cap *= 2));
	k = &(*cases)[(*ncases)++];
	*k = (Case) {.policy = pol, .n = n, .kind = kind};
	SIFT_CHECK(sift_cc_policy_parse(pol, &p) == NULL);
	initStringInfo(&s);
	sift_cc_encode(&s, kind, v, n, NULL, &p, &info);
	k->blob = aligned_copy(&s);
	k->len = s.len;
	pfree(s.data);
	seen.roots[info.tag == SIFT_CCN_WRAP ? info.inner : info.tag]++;
	if (info.tag == SIFT_CCN_WRAP)
		seen.wraps[k->blob[SIFT_CC_HDRLEN + 1]]++;
	case_probes(r, k, v);
	k->ref = out_new(n);
	SIFT_CHECK(run_case(&o, k, &k->ref));
	SAME(k->ref.dec, v, n, 8);
}

/* Dictionary codes of a column of a few hundred values, in runs or at random */
static void
fill_codes(SiftRng *r, uint32 n, int64 *v)
{
	uint32		d = 2 + sift_rng_below(r, 190);

	for (uint32 i = 0; i < n; i++)
		v[i] = i && sift_rng_below(r, 4) ? v[i - 1] : sift_rng_below(r, d);
}

/* Steps that repeat every few rows: byte-aligned deltas see the repeat, the vertical bit-packing does not */
static void
fill_periodic(SiftRng *r, uint32 n, int64 *v)
{
	uint32		cycle = 3 + sift_rng_below(r, 13),
				step[16];
	int64		x = (int64) (sift_rng_next(r) >> 24);

	for (uint32 i = 0; i < cycle; i++)
		step[i] = 100 + sift_rng_below(r, 40000);
	for (uint32 i = 0; i < n; i++)
		v[i] = x += step[i % cycle];
}

static Case *
build_cases(SiftRng *r, int *ncases)
{
	Case	   *cases = malloc(sizeof(Case) * 64);
	int			cap = 64;

	*ncases = 0;
	for (int g = 0; g <= CCG_NGEN + 1; g++)
		for (size_t si = 0; si < lengthof(sizes); si++)
			for (size_t p = 0; p < lengthof(policies); p++)
			{
				uint32		n = sizes[si];
				int64	   *v = xalloc(sizeof(int64) * (n + 8));
				SiftCcKind	kind = SIFT_CC_INT;

				if (g == CCG_NGEN)
					fill_codes(r, n, v);
				else if (g == CCG_NGEN + 1)
					fill_periodic(r, n, v);
				else
					ccg_fill(r, g, n, v, &kind);
				add_case(r, &cases, ncases, &cap, policies[p], kind, v, n);
				free(v);
			}
	return cases;
}

typedef struct StrCase
{
	uint32		n,
				total;
	uint32	   *off;
	char	   *bytes;
	uint8	   *blob;
	Size		len;
	uint32		form;
	const void *ddict;
} StrCase;

typedef struct BitCase
{
	uint32		nbits;
	uint64	   *words;
	uint8	   *blob;
	Size		len;
} BitCase;

typedef struct FsstCase
{
	SiftFsst	t;
	uint32		n,
				total,
			   *eoff,
			   *off;
	uint8	   *enc,
			   *table;
	uint32		tablelen;
	char	   *bytes;
} FsstCase;

static void
gen_strs(SiftRng *r, int kind, uint32 n, uint32 **offp, char **bytesp, uint32 *total)
{
	uint32	   *off = xalloc(sizeof(uint32) * (n + 1));
	char	   *bytes = xalloc((size_t) n * 340 + 8);
	uint32		at = 0;

	for (uint32 i = 0; i < n; i++)
	{
		off[i] = at;
		if (kind == 0)
			at += sprintf(bytes + at, "https://host%u.example.com/shop/%u/item?id=%u&ref=search", sift_rng_below(r, 40), sift_rng_below(r, 900),
						  sift_rng_below(r, 100000));
		else if (kind == 1)
			at += sprintf(bytes + at, "%08x", (unsigned) sift_rng_next(r));
		else if (kind == 2)
		{
			uint32		len = sift_rng_below(r, 3) ? 0 : sift_rng_below(r, 200);

			for (uint32 j = 0; j < len; j++)
				bytes[at++] = (char) sift_rng_next(r);
		}
		else
			for (uint32 j = 0, words = 20 + sift_rng_below(r, 30); j < words; j++)
				at += sprintf(bytes + at, "%s ", j % 3 ? "the" : j % 7 ? "quick" : "brown");
	}
	off[n] = at;
	*offp = off;
	*bytesp = bytes;
	*total = at;
}

static StrCase
add_strs(SiftRng *r, int kind, uint32 n, const char *pol, const void *cdict, const void *ddict)
{
	StrCase		c = {.n = n, .ddict = ddict};
	StringInfoData s;
	SiftCcPolicy p;

	gen_strs(r, kind, n, &c.off, &c.bytes, &c.total);
	SIFT_CHECK(sift_cc_policy_parse(pol, &p) == NULL);
	initStringInfo(&s);
	if (cdict)
		SIFT_CHECK(sift_cc_encode_strs_dict(&s, c.off, c.bytes, n, &p, cdict));
	else
		sift_cc_encode_strs(&s, c.off, c.bytes, n, &p, NULL);
	c.blob = aligned_copy(&s);
	c.len = s.len;
	c.form = get32(c.blob + 16 + get32(c.blob + 8));
	pfree(s.data);
	return c;
}

static BitCase
add_bits(SiftRng *r, uint32 nbits, int dens)
{
	BitCase		b = {.nbits = nbits};
	uint32		nw = (nbits + 63) / 64;
	StringInfoData s;
	SiftCcPolicy p;

	b.words = xalloc(8 * (nw + 1));
	memset(b.words, dens == 1 ? 0xFF : 0, 8 * (nw + 1));
	for (uint32 i = 0; i < nbits; i++)
		if (dens == 2 ? sift_rng_below(r, 100) == 0 : dens == 3 ? sift_rng_below(r, 100) != 0 : dens == 4 ? sift_rng_below(r, 2) : 0)
			b.words[i / 64] ^= UINT64CONST(1) << (i % 64);
	clear_tail(b.words, nbits);
	SIFT_CHECK(sift_cc_policy_parse("auto", &p) == NULL);
	initStringInfo(&s);
	sift_cc_encode_bits(&s, b.words, nbits, &p);
	b.blob = aligned_copy(&s);
	b.len = s.len;
	pfree(s.data);
	return b;
}

static FsstCase *
add_fsst(SiftRng *r, uint32 n)
{
	FsstCase   *f = xalloc(sizeof(FsstCase));
	const char **p = xalloc(sizeof(char *) * n);
	uint32	   *len = xalloc(sizeof(uint32) * n),
				at = 0;
	uint8	   *buf;
	StringInfoData t;

	gen_strs(r, 0, n, &f->off, &f->bytes, &f->total);
	for (uint32 i = 0; i < n; i++)
	{
		p[i] = f->bytes + f->off[i];
		len[i] = f->off[i + 1] - f->off[i];
	}
	f->n = n;
	SIFT_CHECK(sift_fsst_train(&f->t, (const char *const *) p, len, n) < 1000);
	buf = xalloc(2 * (size_t) f->total + 16);
	f->eoff = xalloc(sizeof(uint32) * (n + 1));
	for (uint32 i = 0; i < n; i++)
	{
		f->eoff[i] = at;
		at += sift_fsst_encode(&f->t, p[i], len[i], buf + at);
	}
	f->eoff[n] = at;
	buf[at] = 0;
	f->enc = buf;
	initStringInfo(&t);
	sift_fsst_put_table(&t, &f->t);
	f->table = aligned_copy(&t);
	f->tablelen = t.len;
	pfree(t.data);
	free(p);
	free(len);
	return f;
}

/* ---- the workers ---- */

typedef struct Corpus
{
	Case	   *cases;
	int			ncases;
	StrCase		strs[64];
	int			nstrs;
	BitCase		bits[32];
	int			nbits;
	FsstCase   *fsst;
	int			rounds,
				fuzz_rounds;
	size_t		high_water[WORKERS];
	size_t		high_water_2048[WORKERS];
	uint64		outcomes[WORKERS][8];
} Corpus;

static void
check_strs(SiftArena *a, const StrCase *c, uint32 min_n, Size min_bytes)
{
	SiftErr		err;
	uint32	   *off;
	char	   *bytes;

	sift_arena_reset(a);
	SIFT_CHECK(sift_cc_decode_strs_dict_a(a, &err, c->blob, c->len, c->n, min_n, min_bytes, &off, &bytes, c->ddict));
	SIFT_CHECK(memcmp(off, c->off, sizeof(uint32) * (c->n + 1)) == 0 && memcmp(bytes, c->bytes, c->total) == 0 && bytes[c->total] == 0);
}

static void
check_fsst(SiftArena *a, const FsstCase *f)
{
	SiftFsst   *t = sift_arena_alloc(a, sizeof(SiftFsst), 64);
	uint32	   *outoff = sift_arena_alloc(a, sizeof(uint32) * (f->n + 1), 64);
	uint8	   *out;
	uint64		cap[2] = {f->total + SIFT_FSST_SLACK + 8, 8 * (uint64) f->eoff[f->n] + f->total + 64};

	SIFT_CHECK(sift_fsst_read_table(t, f->table, f->tablelen) == f->tablelen);
	for (int c = 0; c < 2; c++)
	{
		out = sift_arena_alloc(a, cap[c], 64);
		SIFT_CHECK(sift_fsst_decode_strs(t, f->enc, f->eoff, f->n, out, cap[c], outoff));
		SIFT_CHECK(memcmp(outoff, f->off, sizeof(uint32) * (f->n + 1)) == 0 && memcmp(out, f->bytes, f->total) == 0);
	}
	out = sift_arena_alloc(a, 8 * 400 + SIFT_FSST_SLACK, 64);
	for (uint32 i = 0; i < f->n; i++)
		SIFT_CHECK(sift_fsst_decode(t, f->enc + f->eoff[i], f->eoff[i + 1] - f->eoff[i], out) == f->off[i + 1] - f->off[i] &&
			  memcmp(out, f->bytes + f->off[i], f->off[i + 1] - f->off[i]) == 0);
}

static void
check_bits(SiftArena *a, const BitCase *b)
{
	SiftErr		err;
	uint32		nw = (b->nbits + 63) / 64;
	uint64	   *words = sift_arena_alloc(a, 8 * (nw + 1), 64);

	SIFT_CHECK(sift_cc_decode_bits_a(a, &err, b->blob, b->len, b->nbits, words));
	SIFT_CHECK(memcmp(words, b->words, 8 * nw) == 0);
}

/* One damaged blob and what a worker may answer: the reads end in true or in a SiftErr of the damage or of the arena */
static void
outcome(uint64 *hist, bool ok, const SiftErr *err)
{
	if (ok)
		hist[SIFT_OK]++;
	else
	{
		SIFT_CHECK(err->code == SIFT_ERR_CORRUPT || err->code == SIFT_ERR_UNSUPPORTED || err->code == SIFT_ERR_ARENA_FULL);
		SIFT_CHECK(err->msg[0] != 0 && strlen(err->msg) < sizeof(err->msg));
		hist[err->code]++;
	}
}

static void
damage(SiftRng *r, uint8 *dst, const uint8 *src, Size *len)
{
	uint32		how = sift_rng_below(r, 4);

	memcpy(dst, src, *len);
	if (how != 0)
		for (uint32 f = 1 + sift_rng_below(r, 3); f; f--)
			dst[sift_rng_below(r, (uint32) *len)] ^= (uint8) (1 + sift_rng_below(r, 255));
	if (how != 1 && *len > 0)
		*len = how == 3 ? sift_rng_below(r, (uint32) *len) : (*len - 1 - sift_rng_below(r, Min(*len, 64))) & ~(Size) 7;
}

typedef struct FuzzBufs
{
	uint8	   *blob;
	int64	   *dec;
	uint32	   *u32,
			   *idx;
	double	   *f8;
	uint64	   *bits,
			   *member;
	SiftArena  *a;
	SiftErr		err;
} FuzzBufs;

static void
fuzz_reads(SiftRng *r, FuzzBufs *fb, SiftCc *cc, uint64 *hist)
{
	SiftArena  *a = fb->a;
	SiftErr    *e = &fb->err;
	uint32		n = sift_cc_nrows(cc);
	int64		sum,
				key;
	uint64		count;
	bool		fits;

	for (int i = 0; i < 40; i++)
		fb->idx[i] = n ? sift_rng_below(r, n) : 0;
	outcome(hist, sift_cc_decode_a(a, e, cc, fb->dec), e);
	outcome(hist, sift_cc_decode_head_a(a, e, cc, n / 2, fb->dec), e);
	outcome(hist, sift_cc_decode_u32_a(a, e, cc, fb->u32), e);
	if (sift_cc_kind(cc) != SIFT_CC_INT)
		outcome(hist, sift_cc_decode_f8_a(a, e, cc, fb->f8), e);
	outcome(hist, sift_cc_decode_off32_a(a, e, cc, fb->u32, &fits), e);
	outcome(hist, sift_cc_decode_sel_a(a, e, cc, fb->idx, n ? 40 : 0, fb->dec), e);
	if (n)
		outcome(hist, sift_cc_get_a(a, e, cc, fb->idx[0], &key), e);
	outcome(hist, sift_cc_filter_a(a, e, cc, sift_cc_min(cc), sift_cc_min(cc) / 2 + sift_cc_max(cc) / 2, fb->bits), e);
	outcome(hist, sift_cc_sum_a(a, e, cc, fb->bits, &sum, &count), e);
	outcome(hist, sift_cc_filter_member_a(a, e, cc, fb->member, NMEMBER, fb->bits), e);
}

static void
fuzz_chunk(SiftRng *r, FuzzBufs *fb, const Case *k, uint64 *hist)
{
	SiftCc	   *cc;
	Size		len = k->len;

	damage(r, fb->blob, k->blob, &len);
	sift_arena_reset(fb->a);
	if (sift_cc_open_rows_a(fb->a, &fb->err, fb->blob, len, sift_rng_below(r, 2), k->n, &cc))
		fuzz_reads(r, fb, cc, hist);
	else
		outcome(hist, false, &fb->err);
}

static void
fuzz_blobs(SiftRng *r, FuzzBufs *fb, const Corpus *c, uint64 *hist)
{
	const StrCase *s = &c->strs[sift_rng_below(r, c->nstrs)];
	const BitCase *b = &c->bits[sift_rng_below(r, c->nbits)];
	uint32	   *off;
	char	   *bytes;
	Size		len = s->len;

	damage(r, fb->blob, s->blob, &len);
	sift_arena_reset(fb->a);
	outcome(hist, sift_cc_decode_strs_dict_a(fb->a, &fb->err, fb->blob, len, s->n, 0, 0, &off, &bytes, s->ddict), &fb->err);
	len = b->len;
	damage(r, fb->blob, b->blob, &len);
	sift_arena_reset(fb->a);
	outcome(hist, sift_cc_decode_bits_a(fb->a, &fb->err, fb->blob, len, b->nbits, fb->bits), &fb->err);
}

static void
worker(void *arg, int w)
{
	Corpus	   *c = arg;
	SiftArena	a;
	SiftRng		rng;
	Ops			o = {.a = &a};
	FuzzBufs	fb = {.blob = xalloc(1 << 20), .dec = xalloc(8 * SLOTS(2048)), .u32 = xalloc(4 * SLOTS(2048)), .idx = xalloc(4 * 64),
					  .f8 = xalloc(8 * SLOTS(2048)), .bits = xalloc(8 * (SIFT_COL_NWORDS(8192) + 8)), .member = c->cases[0].member, .a = &a};
	size_t		hw2048 = 0;

	sift_arena_init(&a, xalloc(ARENA_CAP), ARENA_CAP);
	sift_rng_seed(&rng, 1000 + w);
	for (int round = 0; round < c->rounds; round++)
		for (int i = 0; i < c->ncases; i++)
		{
			const Case *k = &c->cases[(i * 7 + w * 131 + round * 17) % c->ncases];
			Out			mine = out_new(k->n);

			sift_arena_reset(&a);
			SIFT_CHECK(run_case(&o, k, &mine));
			out_same(k, &mine, &k->ref);
			free(mine.mem);
			if (k->n == 2048)
				hw2048 = Max(hw2048, a.high_water);
		}
	for (int i = 0; i < c->nstrs; i++)
	{
		check_strs(&a, &c->strs[i], 0, 0);
		check_strs(&a, &c->strs[i], c->strs[i].n + 5, c->strs[i].total + 100);
	}
	for (int i = 0; i < c->nbits; i++)
		check_bits(&a, &c->bits[i]);
	check_fsst(&a, c->fsst);
	for (int i = 0; i < c->fuzz_rounds; i++)
	{
		fuzz_chunk(&rng, &fb, &c->cases[sift_rng_below(&rng, c->ncases)], c->outcomes[w]);
		if (i % 8 == 0)
			fuzz_blobs(&rng, &fb, c, c->outcomes[w]);
	}
	c->high_water[w] = a.high_water;
	c->high_water_2048[w] = hw2048;
	free(fb.blob);
	free(fb.dec);
	free(fb.u32);
	free(fb.idx);
	free(fb.f8);
	free(fb.bits);
	free(a.base);
	sift_colz_thread_fini();
}

/* ---- an arena too small, and the errors of a worker against those of the backend ---- */

static void
test_small_arena(const Corpus *c)
{
	for (int pick = 0; pick < 3; pick++)
	{
		const Case *k = NULL;
		size_t		first_open = 0,
					first_decode = 0;

		for (int i = 0; i < c->ncases && !k; i++)
		{
			SiftCc	   *cc = sift_cc_open_rows(CurrentMemoryContext, c->cases[i].blob, c->cases[i].len, true, c->cases[i].n);
			uint8		tag = sift_cc_tag(cc);

			sift_cc_close(cc);
			if (c->cases[i].n == 2048 && tag == (pick == 0 ? SIFT_CCN_DICT : pick == 1 ? SIFT_CCN_RLE : SIFT_CCN_DELTA))
				k = &c->cases[i];
		}
		SIFT_CHECK(k != NULL);
		for (size_t cap = 0; cap <= (1u << 20) && (!first_open || !first_decode); cap = cap ? cap * 2 : 64)
		{
			SiftArena	a;
			SiftErr		err;
			SiftCc	   *cc;
			int64	   *dst = xalloc(8 * SLOTS(2048));
			uint8	   *slab = xalloc(cap);

			sift_arena_init(&a, slab, cap);
			if (!sift_cc_open_rows_a(&a, &err, k->blob, k->len, true, k->n, &cc))
			{
				SIFT_CHECK(!first_open && err.code == SIFT_ERR_ARENA_FULL && err.arg0 > err.arg1 && !sift_arena_scoped());
			}
			else
			{
				first_open = first_open ? first_open : cap;
				if (!sift_cc_decode_a(&a, &err, cc, dst))
					SIFT_CHECK(!first_decode && err.code == SIFT_ERR_ARENA_FULL && err.arg0 > err.arg1);
				else
					first_decode = first_decode ? first_decode : cap;
			}
			free(slab);
			free(dst);
		}
		SIFT_CHECK(first_open > 0 && first_decode > 0);
	}
}

typedef struct Plain
{
	const uint8 *blob;
	Size		len;
	uint32		n;
	int64	   *dst;
} Plain;

static void
plain_fn(void *p)
{
	Plain	   *x = p;
	SiftCc	   *cc = sift_cc_open_rows(CurrentMemoryContext, x->blob, x->len, false, x->n);

	sift_cc_decode(cc, x->dst);
	sift_cc_close(cc);
}

static void
rethrow_fn(void *err)
{
	sift_err_rethrow(NULL, err);
}

/* A damaged chunk raises on the backend the SQLSTATE and the message the worker reports */
static void
test_same_errors(SiftRng *r, const Corpus *c)
{
	uint8	   *blob = xalloc(1 << 20);
	int64	   *dst = xalloc(8 * SLOTS(2048));
	SiftArena	a;
	uint64		same = 0;

	sift_arena_init(&a, xalloc(ARENA_CAP), ARENA_CAP);
	for (int i = 0; i < 3000; i++)
	{
		const Case *k = &c->cases[sift_rng_below(r, c->ncases)];
		Size		len = k->len;
		SiftErr		err;
		SiftCc	   *cc;
		Plain		p;
		bool		ok;
		int			state;
		char		msg[512];

		damage(r, blob, k->blob, &len);
		p = (Plain) {blob, len, k->n, dst};
		sift_arena_reset(&a);
		ok = sift_cc_open_rows_a(&a, &err, blob, len, false, k->n, &cc) && sift_cc_decode_a(&a, &err, cc, dst);
		if (ok)
		{
			SIFT_CHECK(pgshim_try(plain_fn, &p));
			pgshim_reset();
			continue;
		}
		if (err.code == SIFT_ERR_ARENA_FULL)
			continue;
		SIFT_CHECK(!pgshim_try(plain_fn, &p));
		state = pgshim_sqlstate;
		strcpy(msg, pgshim_errmsg);
		SIFT_CHECK(!pgshim_try(rethrow_fn, &err));
		SIFT_CHECK(pgshim_sqlstate == state && strncmp(pgshim_errmsg, msg, sizeof(err.msg) - 1) == 0);
		pgshim_reset();
		same++;
	}
	SIFT_CHECK(same > 500);
	free(a.base);
	free(dst);
	free(blob);
}

/* ---- main ---- */

static void
set_level(SiftSimdLevel level)
{
	sift_cck = sift_cc_kernels_for(level);
	sift_colk = sift_col_kernels_for(level);
}

static int
levels(SiftSimdLevel lv[3])
{
	int			n = 0;

	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512; l++)
		if (!n || sift_cc_kernels_for((SiftSimdLevel) l) != sift_cc_kernels_for(lv[n - 1]) ||
			sift_col_kernels_for((SiftSimdLevel) l) != sift_col_kernels_for(lv[n - 1]))
			lv[n++] = (SiftSimdLevel) l;
	return n;
}

static void
build_strings(SiftRng *r, Corpus *c)
{
	static const char *const pol[] = {"none", "lz4", "zstd"};
	uint8		dict[SIFT_ZSTD_DICT_MAX_BYTES];
	Size		sizes_[300],
				dictlen;
	uint32	   *off;
	char	   *bytes;
	uint32		total;
	void	   *cdict,
			   *ddict;
	int			forms[4] = {0};

	for (int k = 0; k < 4; k++)
		for (size_t p = 0; p < lengthof(pol); p++)
			for (uint32 n = 1; n <= 2048; n *= (k == 0 ? 9 : 40))
				c->strs[c->nstrs++] = add_strs(r, k, n, pol[p], NULL, NULL);
	gen_strs(r, 0, 300, &off, &bytes, &total);
	for (int i = 0; i < 300; i++)
		sizes_[i] = off[i + 1] - off[i];
	dictlen = sift_colz_zstd_train_dict(dict, sizeof(dict), bytes, sizes_, 300);
	SIFT_CHECK(dictlen > 0);
	cdict = sift_colz_zstd_create_cdict(dict, dictlen, 3);
	ddict = sift_colz_zstd_create_ddict(dict, dictlen);
	c->strs[c->nstrs++] = add_strs(r, 0, 2000, "zstd", cdict, ddict);
	c->strs[c->nstrs++] = add_strs(r, 0, 64, "zstd", cdict, ddict);
	for (int i = 0; i < c->nstrs; i++)
		forms[c->strs[i].form < 4 ? c->strs[i].form : 3]++;
	for (int f = 0; f < 4; f++)
		SIFT_CHECK(forms[f] > 0);
	for (uint32 nb = 1; nb < 5000; nb = nb * 13 + 1)
		for (int d = 0; d < 6; d++)
			c->bits[c->nbits++] = add_bits(r, nb, d);
	c->fsst = add_fsst(r, 300);
	free(off);
	free(bytes);
}

static void
check_coverage(void)
{
	static const int must[] = {SIFT_CCN_CONST, SIFT_CCN_FORBIT, SIFT_CCN_FORBYTE, SIFT_CCN_DELTA, SIFT_CCN_RLE, SIFT_CCN_DICT, SIFT_CCN_ALP,
							   SIFT_CCN_SCALE, SIFT_CCN_DELTA_BYTE, SIFT_CCN_BSS};

	for (size_t i = 0; i < lengthof(must); i++)
		if (seen.roots[must[i]] == 0)
			fprintf(stderr, "no chunk with root node %d\n", must[i]);
	for (size_t i = 0; i < lengthof(must); i++)
		SIFT_CHECK(seen.roots[must[i]] > 0);
	SIFT_CHECK(seen.wraps[SIFT_CC_ALG_LZ4] > 0 && seen.wraps[SIFT_CC_ALG_ZSTD] > 0);
}

int
main(int argc, char **argv)
{
	SiftRng		r;
	static Corpus c;
	SiftSimdLevel lv[3];
	int			nlevels;
	size_t		high_water = 0,
				high_water_2048 = 0;
	uint64		hist[8] = {0};

	sift_rng_seed(&r, argc > 1 ? strtoull(argv[1], NULL, 10) : 78);
	kt_cpu_init();
	sift_col_kernels_init();
	sift_cc_kernels_init();
	nlevels = levels(lv);
	set_level(lv[0]);
	c.cases = build_cases(&r, &c.ncases);
	build_strings(&r, &c);
	check_coverage();
	pgshim_keep();
	c.rounds = 2;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	c.fuzz_rounds = 400;
#else
	c.fuzz_rounds = 4000;
#endif
	for (int l = 0; l < nlevels; l++)
	{
		set_level(lv[l]);
		unit_run_workers(WORKERS, worker, &c);
	}
	for (int w = 0; w < WORKERS; w++)
	{
		high_water = Max(high_water, c.high_water[w]);
		high_water_2048 = Max(high_water_2048, c.high_water_2048[w]);
		for (int i = 0; i < 8; i++)
			hist[i] += c.outcomes[w][i];
	}
	set_level(lv[nlevels - 1]);
	test_small_arena(&c);
	test_same_errors(&r, &c);
	SIFT_CHECK(sift_test_trap_count() == 0);
	SIFT_CHECK(hist[SIFT_ERR_CORRUPT] > 100 && hist[SIFT_OK] > 100);
	printf("test_codec: ok: %d chunks, %d string lists, %d bit vectors, %d levels; high_water %zu bytes (2048-row chunks %zu, arena %u); fuzz: ok %llu corrupt %llu unsupported %llu arena_full %llu\n",
		   c.ncases, c.nstrs, c.nbits, nlevels, high_water, high_water_2048, ARENA_CAP, (unsigned long long) hist[SIFT_OK],
		   (unsigned long long) hist[SIFT_ERR_CORRUPT], (unsigned long long) hist[SIFT_ERR_UNSUPPORTED], (unsigned long long) hist[SIFT_ERR_ARENA_FULL]);
	return 0;
}
