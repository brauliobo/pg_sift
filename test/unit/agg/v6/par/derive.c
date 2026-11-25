/*
 * agg/v6/par/derive.c - agg/v6/par/derive.h: the keys and arguments the compute threads compute from columns (length, the host of an URL, extract, CASE) are what the serial scan computes
 * (sift_vx_derive, over the same rows) and what the SQL definitions say (the regular expression written out, the floor of a division in 128 bits, a character count): on rows of every shape (empty,
 * multi-byte, around 12 and 32 bytes, no slash, a host of "www." alone), in plain and dictionary-coded text, with every pattern of active rows, SIMD on and off, on 1 to 4 threads that the
 * trap shim flags as workers (no palloc, no ereport; the derive takes no memory at all).
 */
#include "postgres.h"

#include "agg/group/ht.h"
#include "agg/v6/par/run_int.h"
#include "agg/v6/v6_expr_str.h"
#include "core/cpu.h"
#include "core/hash.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

SiftCpu		sift_cpu = {.flags = SIFT_CPU_AVX2,.detected = SIFT_SIMD_AVX2,.active = SIFT_SIMD_AVX2};

#define ROWS		2000
#define NCOND		SIFT_V6_MAX_CASE
#define DICT_N		97
#define MAX_THREADS	4
#define NA_INF		2
#define MINUTE		INT64CONST(60000000)
#define HOUR		INT64CONST(3600000000)

static const char *const SEEDS[] = {"", "a", "ab", "\xc3\xa9", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "\xf0\x9f\x98\x80x", "http://example.com/", "https://www.example.com/a/b?x=1", "http://www./x",
	"http://www.a/x", "http:///x", "http://host", "https://h/", "http://www.www.x/y", "ftp://x/y", "HTTP://X/Y", " http://x/", "http://a.b/c/d/e", "https://www.\xe6\x97\xa5\xe6\x9c\xac.jp/\xe3\x83\x91\xe3\x82\xb9",
	"http://x\ny/z\n", "https://", "http://", "http://w/", "http://www.w/", "https://abcdefghijklmnopqrstuvwxyz0123456789.example.org/path"};
static const char *const PIECES[] = {"http://", "https://", "www.", "/", ".", "x", "ab", "com", "\xc3\xa9", "\xe6\x97\xa5", "\xf0\x9f\x98\x80", "?", "=", "wwww", " "};

typedef struct Data
{
	uint8		bytes[ROWS][400];
	const char *text[ROWS];
	uint32		slen[ROWS];
	bool		isnull[ROWS];
	uint32		dcode[ROWS];
	uint32		doff[DICT_N + 1];
	uint8		dbytes[DICT_N * 400];
	SiftGranuleDict dict;
	int64		ts[ROWS];
	int64		ints[NCOND][ROWS];
} Data;

static Data *D;

static uint64
mix(uint64 i, int j)
{
	return sift_fmix64(i * 31 + j + 7);
}

static uint32
make_text(uint8 *out, uint64 h, uint32 i)
{
	uint32		len = 0;

	if (i < lengthof(SEEDS))
	{
		len = strlen(SEEDS[i]);
		memcpy(out, SEEDS[i], len);
		return len;
	}
	for (int k = 0, n = h % 9; k < n && len < 330; k++)
	{
		const char *p = PIECES[(h >> (8 + 4 * k)) % lengthof(PIECES)];

		memcpy(out + len, p, strlen(p));
		len += strlen(p);
	}
	return len;
}

static void
make_data(void)
{
	uint32		off = 0;

	D = calloc(1, sizeof(Data));
	SIFT_CHECK(D != NULL);
	for (int c = 0; c < DICT_N; c++)
	{
		D->doff[c] = off;
		off += make_text(D->dbytes + off, mix(c, 90), c);
	}
	D->doff[DICT_N] = off;
	D->dict = (SiftGranuleDict) {.n = DICT_N,.off = D->doff,.bytes = (const char *) D->dbytes};
	for (uint32 i = 0; i < ROWS; i++)
	{
		const uint64 h = mix(i, 0);
		const int64 base = (int64) (h % 4000) * MINUTE * 37 - INT64CONST(1000000) * MINUTE * 60;

		D->dcode[i] = (uint32) (h >> 20) % DICT_N;
		D->slen[i] = D->doff[D->dcode[i] + 1] - D->doff[D->dcode[i]];
		D->text[i] = (const char *) D->dbytes + D->doff[D->dcode[i]];
		D->ts[i] = base + (int64) (h >> 40) % 5 * 3 - 6 + (h % 7 == 0 ? MINUTE : 0) + (h % 11 == 0 ? HOUR : 0) - (h % 13 == 0 ? 1 : 0);
		for (int j = 0; j < NCOND; j++)
			D->ints[j][i] = (int64) (mix(i, 1 + j) % 3) - 1;
	}
	D->ts[7] = PG_INT64_MIN;
	D->ts[ROWS - 1] = PG_INT64_MAX;
}

/* A ColView of the rows in plain or dictionary-coded text */
static ColView
text_view(bool coded)
{
	return (ColView) {.kind = SIFT_CELL_STR,.s = coded ? NULL : (const char *const *) D->text,.slen = coded ? NULL : D->slen,.isnull = D->isnull,.dcode = coded ? D->dcode : NULL,.dict = coded ? &D->dict : NULL};
}

typedef struct Case
{
	ParLoad		ld;
	ParSrc		src;
	uint64		flip;
	SiftStrRef *strs[MAX_THREADS];
	uint64	   *ints[MAX_THREADS];
	bool		ok[MAX_THREADS];
	bool		text;
} Case;

static void
run_one(void *arg, int worker)
{
	Case	   *c = arg;

	if (c->text)
		par_src_strs(&c->src, &c->ld, c->strs[worker]);
	else
		c->ok[worker] = par_src_ints(&c->src, &c->ld, c->flip, c->ints[worker]);
}

/* The match of ^https?://(?:www\.)?([^/]+)/.*$ by its definition: a scheme, then www. taken when it can be and given back when the rest does not match, then one or more bytes but a slash and a slash */
static bool
ref_after_www(const uint8 *p, uint32 len, uint32 at, const uint8 **host, uint32 *hlen)
{
	uint32		q = at;

	while (q < len && p[q] != '/')
		q++;
	if (q == len || q == at)
		return false;
	*host = p + at;
	*hlen = q - at;
	return true;
}

static void
ref_host(const uint8 *p, uint32 len, const uint8 **host, uint32 *hlen)
{
	uint32		at = 0;
	bool		www;

	*host = p;
	*hlen = len;
	if (len >= 7 && strncmp((const char *) p, "http://", 7) == 0)
		at = 7;
	else if (len >= 8 && strncmp((const char *) p, "https://", 8) == 0)
		at = 8;
	else
		return;
	www = len >= at + 4 && strncmp((const char *) p + at, "www.", 4) == 0;
	if (!(www && ref_after_www(p, len, at + 4, host, hlen)))
		ref_after_www(p, len, at, host, hlen);
}

static uint32
ref_chars(const uint8 *p, uint32 len, bool utf8)
{
	uint32		n = 0;

	for (uint32 i = 0; i < len; i++)
		n += !utf8 || (p[i] & 0xC0) != 0x80;
	return n;
}

static int64
ref_extract(int64 ts, int64 unit, int64 mod)
{
	__int128	q = (__int128) ts / unit;

	if ((__int128) ts % unit < 0)
		q--;
	return (int64) (((q % mod) + mod) % mod);
}

static bool
active_at(const uint64 *mask, uint32 r)
{
	return (mask[r / 64] >> (r % 64)) & 1;
}

/* The serial scan's evaluator over the same rows */
static SiftVxOut
serial_out(const Case *c, const uint64 *mask)
{
	SiftVxOut	o;
	SiftBatchSel *bs = aligned_alloc(64, sizeof(SiftBatchSel));
	SiftStrView *sv = aligned_alloc(64, ROWS * sizeof(SiftStrView));
	SiftVec		src = {.data = sv,.len = ROWS,.all_valid = true,.dense = true,.type = SVEC_STRVIEW};
	SiftVec		cond[NCOND];
	SiftVec    *condp[NCOND];
	const SiftV6Derive *d = c->src.d;

	memset(bs, 0, sizeof(SiftBatchSel));
	for (uint32 r = 0; r < ROWS; r++)
		sv[r] = sift_vx_strview((const uint8 *) D->text[r], D->slen[r]);
	if (d->kind == SVD_EXTRACT)
		src = (SiftVec) {.data = D->ts,.len = ROWS,.all_valid = true,.dense = true,.type = SVEC_I64};
	for (int j = 0; j < NCOND; j++)
	{
		cond[j] = (SiftVec) {.data = D->ints[j],.len = ROWS,.all_valid = true,.dense = true,.type = SVEC_I64};
		condp[j] = &cond[j];
	}
	bs->n_raw = bs->n_active = ROWS;
	memcpy(bs->mask, mask, ((ROWS + 63) / 64) * sizeof(uint64));
	sift_vx_out_init(&o, CurrentMemoryContext);
	sift_vx_derive(d, &src, condp, bs, &o);
	free(bs);
	free(sv);
	return o;
}

static void
check_texts(const Case *c, const SiftVxOut *so, const uint64 *mask, int nthreads)
{
	const SiftV6Derive *d = c->src.d;
	uint32		i = 0;

	for (uint32 r = 0; r < ROWS; r++)
	{
		const uint8 *want = (const uint8 *) D->text[r];
		uint32		wlen = D->slen[r];
		const SiftStrView *sv = (const SiftStrView *) so->vec.data;

		if (!active_at(mask, r))
			continue;
		if (d->kind == SVD_URL_HOST)
			ref_host(want, D->slen[r], &want, &wlen);
		else
		{
			bool		hold = true;

			for (int j = 0; j < d->ncond; j++)
				hold &= D->ints[j][r] == d->cval[j];
			if (!hold)
			{
				want = d->else_bytes;
				wlen = d->else_len;
			}
		}
		SIFT_CHECK(sv[r].len == wlen && memcmp(sift_strview_bytes(&sv[r]), want, wlen) == 0);
		for (int t = 0; t < nthreads; t++)
			SIFT_CHECK(c->strs[t][i].len == wlen && memcmp(c->strs[t][i].p, want, wlen) == 0);
		i++;
	}
	SIFT_CHECK(i == c->ld.nact);
}

static void
check_ints(const Case *c, const SiftVxOut *so, const uint64 *mask, int nthreads)
{
	const SiftV6Derive *d = c->src.d;
	const int64 *si = (const int64 *) so->vec.data;
	bool		all_ok = true;
	uint32		i = 0;

	for (uint32 r = 0; r < ROWS; r++)
	{
		int64		want;

		if (!active_at(mask, r))
			continue;
		if (d->kind == SVD_STRLEN)
			want = ref_chars((const uint8 *) D->text[r], D->slen[r], d->utf8);
		else if (D->ts[r] == PG_INT64_MIN || D->ts[r] == PG_INT64_MAX)
		{
			all_ok = false;
			SIFT_CHECK(!((so->valid[r / 64] >> (r % 64)) & 1));
			i++;
			continue;
		}
		else
			want = ref_extract(D->ts[r], d->aux, d->aux == MINUTE ? 60 : 24);
		SIFT_CHECK(((so->valid[r / 64] >> (r % 64)) & 1) && si[r] == want);
		for (int t = 0; t < nthreads; t++)
			SIFT_CHECK(!c->ok[t] || c->ints[t][i] == ((uint64) want ^ c->flip));
		i++;
	}
	for (int t = 0; t < nthreads; t++)
		SIFT_CHECK(c->ok[t] == all_ok);
}

static void
run_case(SiftV6Derive *d, bool coded, const uint64 *mask, int nthreads)
{
	Case	   *c = calloc(1, sizeof(Case));
	SiftVxOut	so;

	c->ld = (ParLoad) {.n = ROWS,.active = (uint64 *) mask};
	for (int w = 0; w < (ROWS + 63) / 64; w++)
		c->ld.nact += __builtin_popcountll(mask[w]);
	c->ld.cv[0] = d->kind == SVD_EXTRACT ? (ColView) {.kind = SIFT_CELL_INT,.i = D->ts,.isnull = D->isnull} : text_view(coded);
	for (int j = 0; j < NCOND; j++)
		c->ld.cv[1 + j] = (ColView) {.kind = SIFT_CELL_INT,.i = D->ints[j],.isnull = D->isnull};
	c->src = (ParSrc) {.d = d,.at = 0,.cond = {1, 2, 3, 4}};
	c->text = d->kind == SVD_URL_HOST || d->kind == SVD_CASE_STR;
	c->flip = d->kind == SVD_STRLEN ? 0 : PAR_SIGN_FLIP;
	for (int t = 0; t < nthreads; t++)
	{
		c->strs[t] = calloc(ROWS + 1, sizeof(SiftStrRef));
		c->ints[t] = calloc(ROWS + 1, sizeof(uint64));
	}
	unit_run_workers(nthreads, run_one, c);
	so = serial_out(c, mask);
	if (c->text)
		check_texts(c, &so, mask, nthreads);
	else
		check_ints(c, &so, mask, nthreads);
	for (int t = 0; t < nthreads; t++)
	{
		free(c->strs[t]);
		free(c->ints[t]);
	}
	free(c);
	pgshim_reset();
}

static void
make_mask(uint64 *mask, int pattern)
{
	memset(mask, 0, ((ROWS + 63) / 64) * sizeof(uint64));
	for (uint32 r = 0; r < ROWS; r++)
	{
		const uint64 h = mix(r, 50 + pattern);
		const bool	on = pattern == 0 ? true : pattern == 1 ? h % 2 == 0 : pattern == 2 ? h % 53 == 0 : pattern == 3 ? r == 5 : pattern == 4 ? r != 7 && r != ROWS - 1 : r < 64;

		mask[r / 64] |= (uint64) on << (r % 64);
	}
}

int
main(void)
{
	const SiftSimdLevel levels[] = {SIFT_SIMD_AVX2, SIFT_SIMD_SCALAR};
	int			ncases = 0;

	pgshim_keep();
	make_data();
	for (int lv = 0; lv < 2; lv++)
	{
		sift_cpu.active = levels[lv];
		for (int kind = 0; kind < 7; kind++)
			for (int coded = 0; coded < 2; coded++)
				for (int pattern = 0; pattern < 6; pattern++)
					for (int nthreads = 1; nthreads <= MAX_THREADS; nthreads += 3)
					{
						SiftV6Derive d = {.kind = kind == 0 || kind == 1 ? SVD_STRLEN : kind == 2 ? SVD_URL_HOST : kind == 3 ? SVD_EXTRACT : SVD_CASE_STR,.utf8 = kind == 0,.aux = MINUTE};
						uint64		mask[(ROWS + 63) / 64];

						if (kind == 4 || kind == 5 || kind == 6)
						{
							d.ncond = kind - 3;
							d.else_len = kind == 4 ? 0 : 5;
							memcpy(d.else_bytes, "else!", 5);
							for (int j = 0; j < d.ncond; j++)
								d.cval[j] = j % 2;
						}
						if (kind == 3 && coded)
							d.aux = HOUR;
						make_mask(mask, pattern);
						run_case(&d, coded, mask, nthreads);
						ncases++;
					}
	}
	free(D);
	printf("par_derive: ok (%d cases)\n", ncases);
	return 0;
}
