/*
 * col/granule/fixture.c - the part of col/granule/fixture.h: ClickBench-shaped columns, the granules the real writer makes of them, and their decode on the setup thread.
 */
#include "postgres.h"

#include <math.h>

#include "access/transam.h"
#include "lib/stringinfo.h"

#include "col/compress.h"
#include "col/fsst.h"
#include "col/granule/int.h"
#include "core/testrng.h"
#include "fixture.h"

enum
{
	S_URL, S_TITLE, S_REF, S_PHRASE, S_CAT30, S_CAT40, S_TEXT, S_TEXT_NULL, S_STR_SPARSE_NN, S_STR_SPARSE_NULL, S_VIEW, S_UUID, S_NUM128, S_STR_TAGS, S_STR_ALLNULL,
	S_I_SMALL, S_I_TIME, S_I_RUNS, S_I_CONST, S_I_USER, S_I_WIDE, S_I_BOOL, S_I_RES, S_I_NULL, S_I_SPARSE_NN, S_I_SPARSE_NULL, S_I_NUM64, S_I_JNULL, S_I_OVF, S_I_OVF_MANY,
	S_I_ALLNULL, S_I_KEYP, S_I_KEYLEAF, S_I_LIST, S_I_ELEM,
	S_F_PRICE, S_F_RAND, S_F_NULL, S_F_GEO, S_F_NOISE, S_F_SINE, S_INTERVAL
};

#define STR(n, sh, ...)		{n, SIFT_CELL_STR, sh, __VA_ARGS__}
#define INT(n, sh, ...)		{n, SIFT_CELL_INT, sh, __VA_ARGS__}
#define F8(n, sh, ...)		{n, SIFT_CELL_F8, sh, __VA_ARGS__}

static const FxColSpec specs[] = {
	STR("url", S_URL, .part = FX_PART_ZSTD),
	STR("title", S_TITLE, .part = FX_PART_FSST),
	STR("referer", S_REF, .part = FX_PART_FSST),
	STR("phrase", S_PHRASE, .part = FX_PART_DICT),
	STR("lang", S_CAT30, .part = FX_PART_DICT),
	STR("browser", S_CAT40),
	STR("text", S_TEXT),
	STR("text_null", S_TEXT_NULL),
	STR("str_sparse_nn", S_STR_SPARSE_NN),
	STR("str_sparse_null", S_STR_SPARSE_NULL),
	STR("str_view", S_VIEW, .hint = SIFT_CHINT_VIEW),
	STR("uuid", S_UUID, .hint = SIFT_CHINT_UUID),
	STR("num128", S_NUM128, .hint = SIFT_CHINT_NUM128),
	STR("str_tags", S_STR_TAGS),
	STR("str_allnull", S_STR_ALLNULL),
	INT("counter", S_I_SMALL),
	INT("event_time", S_I_TIME),
	INT("event_date", S_I_RUNS),
	INT("const", S_I_CONST),
	INT("user_id", S_I_USER),
	INT("watch_id", S_I_WIDE),
	INT("flag", S_I_BOOL, .hint = SIFT_CHINT_BOOL),
	INT("resolution", S_I_RES),
	INT("int_null", S_I_NULL),
	INT("int_sparse_nn", S_I_SPARSE_NN),
	INT("int_sparse_null", S_I_SPARSE_NULL),
	INT("num64", S_I_NUM64, .hint = SIFT_CHINT_NUM64, .scale = 2),
	INT("int_jnull", S_I_JNULL),
	INT("int_ovf", S_I_OVF),
	INT("int_ovf_many", S_I_OVF_MANY),
	INT("int_allnull", S_I_ALLNULL),
	INT("key_present", S_I_KEYP, .hint = SIFT_CHINT_KEY_PRESENT),
	INT("key_leaf", S_I_KEYLEAF, .key_leaf = true),
	INT("lens", S_I_LIST, .hint = SIFT_CHINT_LIST),
	INT("elems", S_I_ELEM, .group_of = 34),
	F8("price", S_F_PRICE),
	F8("ratio", S_F_RAND),
	F8("f_null", S_F_NULL),
	F8("geo", S_F_GEO),
	F8("noise", S_F_NOISE),
	F8("sine", S_F_SINE),
	STR("interval", S_INTERVAL, .hint = SIFT_CHINT_INTERVAL),
};

#define NCOLS	((int) lengthof(specs))

const FxCfg fx_cfgs[FX_CFGS] = {
	{"hot-lz4", SIFT_TIER_HOT_L0, SIFT_CC_ALG_LZ4, false, false},
	{"warm-lz4", SIFT_TIER_WARM_L1, SIFT_CC_ALG_LZ4, false, false},
	{"cold-zstd", SIFT_TIER_COLD_L2, SIFT_CC_ALG_ZSTD, false, false},
	{"cold-zstd-part", SIFT_TIER_COLD_L2, SIFT_CC_ALG_ZSTD, true, false},
	{"warm-lz4-part", SIFT_TIER_WARM_L1, SIFT_CC_ALG_LZ4, true, false},
	{"cold-zstd-framed", SIFT_TIER_COLD_L2, SIFT_CC_ALG_ZSTD, false, true},
};

typedef struct FxData
{
	int			n;
	int64	   *i;
	double	   *f;
	const char **s;
	uint32	   *slen;
	bool	   *isnull;
	uint8	   *tag;
	const char **xpay;
	uint32	   *xplen;
	uint32	   *eoff;
} FxData;

#define VOCAB		3000
#define NHOSTS		64
#define NPHRASES	600
#define POOL_BLOCK	((size_t) 4 << 20)

typedef struct Fx
{
	uint64		rng;
	char	   *pool;
	size_t		left;
	char		vocab[VOCAB][16];
	char		host[NHOSTS][40];
	char	   *phrase[NPHRASES];
	uint64		users[5000];
	int64		t;
	int			run_left;
	int64		run_val;
} Fx;

static uint64
rnd(Fx *x)
{
	return sift_splitmix_next(&x->rng);
}

static uint32
rn(Fx *x, uint32 n)
{
	return (rnd(x) >> 11) % n;
}

/* Skewed to the small values: the popular words and hosts of a log */
static uint32
zipf(Fx *x, uint32 n)
{
	return (uint32) ((uint64) rn(x, n) * rn(x, n) / n);
}

static char *
pool_take(Fx *x, size_t n)
{
	char	   *p;

	if (n > x->left)
	{
		x->left = Max(n, POOL_BLOCK);
		x->pool = palloc(x->left);
	}
	p = x->pool;
	x->pool += n;
	x->left -= n;
	return p;
}

static void
set_s(Fx *x, FxData *d, int r, const char *s, uint32 len)
{
	d->s[r] = memcpy(pool_take(x, len + 1), s, len);
	d->slen[r] = len;
}

static void
set_null(FxData *d, int r)
{
	d->isnull[r] = true;
	d->s[r] = "";
}

static void
vocab_init(Fx *x)
{
	static const char *const syl[] = {"ka", "lo", "mi", "ra", "su", "ne", "to", "vi", "po", "da", "ge", "hu", "bo", "fa", "ze", "qu", "ri", "an", "el", "or"};
	static const char *const tld[] = {"com", "ru", "net", "org", "ua"};

	for (int w = 0; w < VOCAB; w++)
	{
		x->vocab[w][0] = 0;
		for (int k = 2 + rn(x, 3); k > 0; k--)
			strcat(x->vocab[w], syl[rn(x, lengthof(syl))]);
	}
	for (int h = 0; h < NHOSTS; h++)
		snprintf(x->host[h], sizeof(x->host[h]), "www.%s%s.%s", x->vocab[rn(x, VOCAB)], x->vocab[rn(x, VOCAB)], tld[rn(x, lengthof(tld))]);
	for (int p = 0; p < NPHRASES; p++)
	{
		char		buf[128] = "";

		for (int k = 1 + rn(x, 4); k > 0; k--)
			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", buf[0] ? " " : "", x->vocab[zipf(x, VOCAB)]);
		x->phrase[p] = pstrdup(buf);
	}
	for (int u = 0; u < 5000; u++)
		x->users[u] = rnd(x) >> 2;
}

#define AP(...)		(n += Min(snprintf(buf + n, cap - n, __VA_ARGS__), (int) (cap - n - 1)))

static uint32
gen_url(Fx *x, char *buf, uint32 cap, bool short_form)
{
	uint32		n = 0;

	AP("http%s://%s/", rn(x, 5) == 0 ? "s" : "", x->host[zipf(x, NHOSTS)]);
	for (int k = 1 + rn(x, short_form ? 2 : 4); k > 0; k--)
		AP("%s/", x->vocab[zipf(x, VOCAB)]);
	if (rn(x, 3) == 0)
		AP("item%u.html", rn(x, 100000));
	for (int k = rn(x, short_form ? 2 : 4), sep = '?'; k > 0; k--, sep = '&')
		AP("%c%s=%u", sep, x->vocab[rn(x, 60)], rn(x, 1000000));
	if (rn(x, 100) == 0)
		for (int k = 20 + rn(x, 180); k > 0 && n < cap - 32; k--)
			AP("&%s=%s", x->vocab[rn(x, 60)], x->vocab[zipf(x, VOCAB)]);
	return n;
}

static uint32
gen_title(Fx *x, char *buf, uint32 cap)
{
	uint32		n = 0;
	bool		cyr = rn(x, 10) < 3;

	for (int k = 3 + rn(x, 8); k > 0 && n < cap - 32; k--)
	{
		if (n > 0)
			AP(" ");
		if (cyr)
			for (int c = 3 + rn(x, 6); c > 0; c--)
			{
				uint32		u = rn(x, 32);

				buf[n++] = u < 16 ? 0xD0 : 0xD1;
				buf[n++] = (u < 16 ? 0xB0 : 0x80) + (u & 15);
			}
		else
			AP("%s", x->vocab[zipf(x, VOCAB)]);
	}
	return n;
}

static uint32
gen_text(Fx *x, char *buf, uint32 cap)
{
	uint32		n = 0;

	AP("%s%s-%06u", x->vocab[rn(x, VOCAB)], x->vocab[rn(x, VOCAB)], rn(x, 1000000));
	return n;
}

static void
set_tagged(Fx *x, FxData *d, int r, uint8 tag, bool payload)
{
	d->isnull[r] = true;
	d->tag[r] = tag;
	if (payload)
	{
		char		buf[32];
		int			len = 1 + rn(x, 20);

		for (int k = 0; k < len; k++)
			buf[k] = 'a' + rn(x, 26);
		d->xpay[r] = memcpy(pool_take(x, len), buf, len);
		d->xplen[r] = len;
	}
}

/* A value the column cannot hold on 'permille' of the rows: a JSON null (no payload) or one of the types of an overflow with its payload */
static bool
gen_exception(Fx *x, FxData *d, int r, uint32 permille, bool payload)
{
	uint32		u = rn(x, 1000);

	if (u >= permille)
		return false;
	if (payload)
		set_tagged(x, d, r, SIFT_COLTAG_STRING + rn(x, SIFT_COLTAG_OVF_MAX), true);
	else
		set_tagged(x, d, r, SIFT_COLTAG_JNULL, false);
	return true;
}

static void
gen_str(Fx *x, int shape, FxData *d, int r)
{
	char		buf[4096];
	uint32		n = 0,
				cap = sizeof(buf);

	switch (shape)
	{
		case S_URL:
			n = gen_url(x, buf, cap, false);
			break;
		case S_TITLE:
			n = gen_title(x, buf, cap);
			break;
		case S_REF:
			n = rn(x, 100) < 45 ? 0 : gen_url(x, buf, cap, true);
			break;
		case S_PHRASE:
			n = rn(x, 100) < 65 ? 0 : (uint32) snprintf(buf, cap, "%s", x->phrase[zipf(x, NPHRASES)]);
			break;
		case S_CAT30:
			AP("lang_%s", x->vocab[zipf(x, 30)]);
			break;
		case S_CAT40:
			AP("Mozilla/5.0 (%s %u.%u) %s", x->vocab[zipf(x, 40)], 1 + rn(x, 4), rn(x, 10), x->vocab[zipf(x, 40)]);
			break;
		case S_TEXT_NULL:
			if (rn(x, 100) < 25)
				return set_null(d, r);
			n = gen_text(x, buf, cap);
			break;
		case S_STR_SPARSE_NN:
			if (rn(x, 1000) >= 4)
				return set_null(d, r);
			n = gen_text(x, buf, cap);
			break;
		case S_STR_SPARSE_NULL:
			if (rn(x, 1000) < 5)
				return set_null(d, r);
			n = gen_text(x, buf, cap);
			break;
		case S_VIEW:
			n = rn(x, 3) == 0 ? gen_text(x, buf, cap) : snprintf(buf, cap, "%s", x->vocab[rn(x, VOCAB)]);
			break;
		case S_UUID:
			x->t += rn(x, 50);
			memcpy(buf, &(uint32) {(uint32) (r / 1500)}, 4);
			memcpy(buf + 4, &x->t, 4);
			for (n = 8; n < 16; n++)
				buf[n] = rn(x, 256);
			break;
		case S_INTERVAL:
			for (n = 0; n < 16; n++)
				buf[n] = n < 6 ? rn(x, 256) : 0;
			break;
		case S_NUM128:
			memset(buf, 0, 16);
			x->t = rn(x, 100000000);
			memcpy(buf, &x->t, 8);
			n = 16;
			break;
		case S_STR_TAGS:
			if (gen_exception(x, d, r, 30, true) || gen_exception(x, d, r, 20, false))
			{
				d->s[r] = "";
				return;
			}
			if (rn(x, 100) == 0)
				return set_null(d, r);
			n = gen_text(x, buf, cap);
			break;
		case S_STR_ALLNULL:
			return set_null(d, r);
		default:
			n = gen_text(x, buf, cap);
	}
	set_s(x, d, r, buf, n);
}

static void
gen_int(Fx *x, int shape, FxData *d, int r)
{
	int64		v = 0;

	switch (shape)
	{
		case S_I_SMALL:
			v = zipf(x, 1000);
			break;
		case S_I_TIME:
			x->t += rn(x, 4);
			v = x->t;
			break;
		case S_I_RUNS:
			if (x->run_left-- <= 0)
			{
				x->run_left = 1 + zipf(x, 400);
				x->run_val = 15000 + rn(x, 400);
			}
			v = x->run_val;
			break;
		case S_I_CONST:
			v = 9;
			break;
		case S_I_USER:
			v = rn(x, 100) < 40 ? x->users[zipf(x, 5000)] : rnd(x) >> 2;
			break;
		case S_I_WIDE:
			v = rnd(x) >> 2;
			break;
		case S_I_BOOL:
			v = rn(x, 7) == 0;
			break;
		case S_I_RES:
			v = (int64[]) {1366, 1920, 1024, 1280, 360, 414, 375, 768, 1440, 800, 1600, 2560}[zipf(x, 12)];
			break;
		case S_I_NULL:
			v = rn(x, 500);
			d->isnull[r] = rn(x, 100) < 20;
			break;
		case S_I_SPARSE_NN:
			v = rn(x, 100000);
			d->isnull[r] = rn(x, 1000) >= 3;
			break;
		case S_I_SPARSE_NULL:
			v = rn(x, 100000);
			d->isnull[r] = rn(x, 1000) < 3;
			break;
		case S_I_NUM64:
			v = rn(x, 1000000);
			break;
		case S_I_JNULL:
			v = rn(x, 5000);
			gen_exception(x, d, r, 300, false);
			break;
		case S_I_OVF:
			v = rn(x, 5000);
			gen_exception(x, d, r, 30, true);
			break;
		case S_I_OVF_MANY:
			v = rn(x, 5000);
			gen_exception(x, d, r, 300, true);
			break;
		case S_I_ALLNULL:
			d->isnull[r] = true;
			break;
		case S_I_KEYP:
			v = 1;
			d->isnull[r] = rn(x, 100) < 40;
			break;
		case S_I_KEYLEAF:
			v = rn(x, 1000);
			if (rn(x, 100) < 35)
			{
				d->isnull[r] = true;
				d->tag[r] = rn(x, 2) ? SIFT_COLTAG_JNULL : 0;
			}
			break;
		case S_I_LIST:
			v = zipf(x, 5);
			d->eoff[r + 1] = d->eoff[r] + v;
			break;
		default:
			v = rn(x, 100000);
	}
	d->i[r] = d->isnull[r] ? 0 : v;
}

static void
gen_f8(Fx *x, int shape, FxData *d, int r)
{
	double		v = 0;

	switch (shape)
	{
		case S_F_PRICE:
			v = (double) rn(x, 200000) / 100.0;
			break;
		case S_F_RAND:
			v = (double) (rnd(x) >> 11) * 0x1.0p-53;
			break;
		case S_F_NULL:
			v = (double) rn(x, 10000) / 10.0;
			d->isnull[r] = rn(x, 100) < 20;
			break;
		case S_F_NOISE:
			{
				uint64		u = (rnd(x) & UINT64CONST(0x800FFFFFFFFFFFFF)) | ((uint64) (1 + rn(x, 2000)) << 52);

				memcpy(&v, &u, 8);
			}
			break;
		case S_F_SINE:
			v = sin((double) r * 0.001) * 1000.0 * (1.0 + (double) rn(x, 1000) * 1e-9);
			break;
		default:
			v = 55.0 + (double) rn(x, 1000000) / 1e6;
	}
	d->f[r] = d->isnull[r] ? 0 : v;
}

static bool
col_has_tags(int shape)
{
	return shape == S_STR_TAGS || shape == S_I_JNULL || shape == S_I_OVF || shape == S_I_OVF_MANY || shape == S_I_KEYLEAF;
}

/* The n values of a column of the whole part; the elements of a list column are as many as the lengths of the list column say */
static FxData
gen_col(Fx *x, const FxColSpec *c, int rows, const FxData *lens)
{
	FxData		d = {.n = c->group_of > 0 ? lens->eoff[rows] : rows};

	d.isnull = palloc0(d.n);
	if (c->kind == SIFT_CELL_INT)
		d.i = palloc0(d.n * sizeof(int64));
	else if (c->kind == SIFT_CELL_F8)
		d.f = palloc0(d.n * sizeof(double));
	else
	{
		d.s = palloc0(d.n * sizeof(char *));
		d.slen = palloc0(d.n * sizeof(uint32));
	}
	if (col_has_tags(c->shape))
	{
		d.tag = palloc0(d.n);
		d.xpay = palloc0(d.n * sizeof(char *));
		d.xplen = palloc0(d.n * sizeof(uint32));
	}
	if (c->shape == S_I_LIST)
		d.eoff = palloc0((d.n + 1) * sizeof(uint32));
	x->t = 1384000000;
	x->run_left = 0;
	for (int r = 0; r < d.n; r++)
	{
		if (c->kind == SIFT_CELL_STR)
			gen_str(x, c->shape, &d, r);
		else if (c->kind == SIFT_CELL_INT)
			gen_int(x, c->shape, &d, r);
		else
			gen_f8(x, c->shape, &d, r);
	}
	return d;
}

static SiftColIn
colin(const FxColSpec *c, const FxData *d)
{
	return (SiftColIn) {.kind = c->kind,.i = d->i,.f = d->f,.s = d->s,.slen = d->slen,.isnull = d->isnull,.tag = d->tag,.hint = c->hint,.key_leaf = c->key_leaf,
		.scale = c->scale,.group1 = c->group_of,.eoff = d->eoff,.xpay = d->xpay,.xplen = d->xplen};
}

typedef struct PartCodecs
{
	const SiftDictEnc *dict[NCOLS];
	const SiftFsst *fsst[NCOLS];
	void	   *cdict[NCOLS];
} PartCodecs;

StaticAssertDecl(NCOLS <= FX_MAX_COLS, "the fixture has more columns than FX_MAX_COLS");

static void
add_dict(GranuleFixture *fx, SiftPartDict d)
{
	fx->dicts.d[fx->dicts.n++] = d;
}

static void
part_str_dict(GranuleFixture *fx, PartCodecs *pc, int c, const SiftColIn *col, int rows)
{
	SiftDictEnc *de = sift_dict_enc_build(col, rows);
	uint32	   *off;
	uint64		total = 0;
	char	   *bytes;

	if (de == NULL)
		return;
	off = palloc((de->n + 1) * sizeof(uint32));
	for (uint32 k = 0; k < de->n; k++)
	{
		off[k] = total;
		total += de->len[k];
	}
	off[de->n] = total;
	bytes = palloc(Max(total, 1));
	for (uint32 k = 0; k < de->n; k++)
		memcpy(bytes + off[k], de->p[k], de->len[k]);
	pc->dict[c] = de;
	add_dict(fx, (SiftPartDict) {.col = c,.kind = SIFT_DICT_KIND_STR,.n = de->n,.off = off,.bytes = bytes});
}

static void
part_fsst(GranuleFixture *fx, PartCodecs *pc, int c, const SiftColIn *col, int rows)
{
	const char **p = palloc(rows * sizeof(char *));
	uint32	   *len = palloc(rows * sizeof(uint32));
	SiftFsst   *t = palloc(sizeof(SiftFsst));
	uint32		n = 0;

	for (int r = 0; r < rows; r++)
		if (!col->isnull[r])
		{
			p[n] = col->s[r];
			len[n++] = col->slen[r];
		}
	sift_fsst_train(t, p, len, n);
	pc->fsst[c] = t;
	add_dict(fx, (SiftPartDict) {.col = c,.kind = SIFT_DICT_KIND_FSST,.n = t->nsym,.fsst = t});
}

#define ZDICT_SAMPLES	256
#define ZDICT_SAMPLE	400
#define ZDICT_BYTES		8192

static void
part_zstd(GranuleFixture *fx, PartCodecs *pc, int c, const SiftColIn *col, int rows)
{
	uint8	   *samples = palloc(ZDICT_SAMPLES * ZDICT_SAMPLE),
			   *dict = palloc(ZDICT_BYTES);
	Size		sizes[ZDICT_SAMPLES],
				at = 0,
				dlen;
	int			ns = 0;

	for (int r = 0; r < rows && ns < ZDICT_SAMPLES; r += Max(rows / ZDICT_SAMPLES, 1))
	{
		Size		len = Min(col->slen[r], ZDICT_SAMPLE);

		memcpy(samples + at, col->s[r], len);
		sizes[ns++] = len;
		at += len;
	}
	dlen = sift_colz_zstd_train_dict(dict, ZDICT_BYTES, samples, sizes, ns);
	if (dlen == 0)
		return;
	pc->cdict[c] = sift_colz_zstd_create_cdict(dict, dlen, 3);
	add_dict(fx, (SiftPartDict) {.col = c,.kind = SIFT_DICT_KIND_ZSTD,.n = dlen,.bytes = (char *) dict,.zstd_ddict = sift_colz_zstd_create_ddict(dict, dlen)});
}

static void
make_sel(Fx *x, FxGranule *g, int gi)
{
	static const int density[3] = {20, 128, 256};
	int			want = density[gi % 3],
				win = FX_ROWS / want;

	for (int k = 0; k < want; k++)
		g->sel[k] = k * win + rn(x, win);
	g->nsel = want;
}

static void
ref_column(FxRef *r, const SiftGranuleView *v, int c, SiftCellKind kind)
{
	int			n = sift_granule_chunk_len(v, c);
	SiftGranuleDict dict;
	bool	   *nl = palloc(n);

	*r = (FxRef) {.n = n,.isnull = palloc(n),.tag = palloc(n)};
	if (kind == SIFT_CELL_INT)
		sift_granule_ints(v, c, r->i = palloc(n * sizeof(int64)), r->isnull);
	else if (kind == SIFT_CELL_F8)
		sift_granule_f8(v, c, r->f = palloc(n * sizeof(double)), r->isnull);
	else
	{
		r->s = palloc(n * sizeof(SiftStrCell));
		sift_granule_strs(v, c, r->s);
		for (int k = 0; k < n; k++)
		{
			r->isnull[k] = r->s[k].isnull;
			if (!r->s[k].isnull)
				r->s[k].p = memcpy(palloc(r->s[k].len + 1), r->s[k].p, r->s[k].len);
		}
		r->has_codes = sift_granule_str_codes(v, c, r->codes = palloc(n * sizeof(uint32)), nl, &dict);
	}
	sift_granule_tags(v, c, r->tag);
}

static void
note_encodings(GranuleFixture *fx, const SiftGranuleView *v)
{
	for (int c = 1; c < v->hdr->nchunks; c++)
	{
		const SiftChunkRef *ref = &v->hdr->chunks[c];

		fx->enc_seen |= UINT64CONST(1) << (ref->enc & SIFT_ENC_MASK);
		if (ref->flags & SIFT_CHUNK_COMPRESSED)
			fx->wrapped_seen |= UINT64CONST(1) << (ref->enc & SIFT_ENC_MASK);
		fx->flags_seen |= (ref->flags & 0x3F) | ((uint32) (ref->enc >> 8) << 16);
	}
}

static void
write_granule(GranuleFixture *fx, Fx *x, const PartCodecs *pc, const SiftColIn *cols, const uint32 *lens_eoff, int gi)
{
	FxGranule  *g = &fx->g[gi];
	uint64	   *tid = palloc(FX_ROWS * sizeof(uint64));
	SiftColIn  *in_cols = palloc(NCOLS * sizeof(SiftColIn));
	StringInfoData out;
	int			from = gi * FX_ROWS;
	SiftGranuleIn in;

	g->cfg = &fx_cfgs[gi % FX_CFGS];
	for (int r = 0; r < FX_ROWS; r++)
		tid[r] = ((uint64) ((from + r) / 60) << 16) | ((from + r) % 60 + 1);
	for (int c = 0; c < NCOLS; c++)
		in_cols[c] = sift_colin_slice(&cols[c], specs[c].group_of > 0 ? lens_eoff[from] : from);
	in = (SiftGranuleIn) {.nrows = FX_ROWS,.ncols = NCOLS,.tid = tid,.cols = in_cols,.all_frozen = true,.compress_alg = g->cfg->alg,.tier = g->cfg->tier,.framed = g->cfg->framed,
		.dicts = g->cfg->part_codecs ? pc->dict : NULL,.part_fsst = g->cfg->part_codecs ? pc->fsst : NULL,.part_cdict = g->cfg->part_codecs ? pc->cdict : NULL};
	initStringInfo(&out);
	g->desc = sift_granule_encode(&out, &in);
	g->rec = out.data;
	g->len = out.len;
	g->view = sift_granule_open(granule_fixture_rel(), g->rec, g->len, &g->desc);
	g->view.dicts = &fx->dicts;
	make_sel(x, g, gi);
	g->ref = palloc(NCOLS * sizeof(FxRef));
	for (int c = 0; c < NCOLS; c++)
		ref_column(&g->ref[c], &g->view, c, specs[c].kind);
	note_encodings(fx, &g->view);
}

extern void kt_cpu_init(void);

void
granule_fixture_init(void)
{
	kt_cpu_init();
	sift_col_kernels_init();
	sift_cc_kernels_init();
}

void
granule_fixture_free(GranuleFixture *fx)
{
	for (int c = 0; c < NCOLS; c++)
		if (fx->cdicts[c] != NULL)
			sift_colz_zstd_free_cdict(fx->cdicts[c]);
	for (int d = 0; d < fx->dicts.n; d++)
		if (fx->dicts.d[d].zstd_ddict != NULL)
			sift_colz_zstd_free_ddict(fx->dicts.d[d].zstd_ddict);
}

int
granule_fixture_col(const GranuleFixture *fx, const char *name)
{
	for (int c = 0; c < fx->ncols; c++)
		if (strcmp(fx->cols[c].name, name) == 0)
			return c;
	return -1;
}

static SiftColDef
col_def(int id, SiftCellKind kind)
{
	const bool	is_int = kind == SIFT_CELL_INT,
				is_f8 = kind == SIFT_CELL_F8;

	return (SiftColDef) {.id = id,.parent = -1,.store = is_int ? SIFT_COLS_INT : is_f8 ? SIFT_COLS_F8 : SIFT_COLS_STR,
		.type = is_int ? SIFT_COLT_INT8 : is_f8 ? SIFT_COLT_FLOAT8 : SIFT_COLT_TEXT};
}

GranuleFixture *
granule_fixture_build(int ngranules, uint32 seed)
{
	GranuleFixture *fx = palloc0(sizeof(GranuleFixture));
	Fx		   *x = palloc0(sizeof(Fx));
	int			rows = ngranules * FX_ROWS;
	FxData		data[NCOLS];
	SiftColIn	cols[NCOLS];
	PartCodecs *pc = palloc0(sizeof(PartCodecs));
	int			lens_col = 0;

	x->rng = 88172645463325252ULL ^ ((uint64) seed * 0x9E3779B97F4A7C15ULL);
	vocab_init(x);
	fx->ngranules = ngranules;
	fx->ncols = NCOLS;
	fx->cols = specs;
	fx->defs = palloc0(NCOLS * sizeof(SiftColDef));
	fx->dicts.d = palloc0(NCOLS * sizeof(SiftPartDict));
	fx->g = palloc0(ngranules * sizeof(FxGranule));
	for (int c = 0; c < NCOLS; c++)
	{
		data[c] = gen_col(x, &specs[c], rows, specs[c].group_of > 0 ? &data[specs[c].group_of - 1] : NULL);
		cols[c] = colin(&specs[c], &data[c]);
		if (specs[c].shape == S_I_LIST)
			lens_col = c;
		fx->defs[c] = col_def(c, specs[c].kind);
		if (specs[c].part == FX_PART_DICT)
			part_str_dict(fx, pc, c, &cols[c], rows);
		else if (specs[c].part == FX_PART_FSST)
			part_fsst(fx, pc, c, &cols[c], rows);
		else if (specs[c].part == FX_PART_ZSTD)
			part_zstd(fx, pc, c, &cols[c], rows);
	}
	memcpy(fx->cdicts, pc->cdict, sizeof(pc->cdict));
	for (int g = 0; g < ngranules; g++)
		write_granule(fx, x, pc, cols, data[lens_col].eoff, g);
	return fx;
}
