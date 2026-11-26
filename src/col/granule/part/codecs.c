/*
 * col/granule/part/codecs.c - what a part of the warm and cold tiers learns once about the strings of a column and every granule of it uses (SEC_DICTS, col/granule/part/int.h): a table of symbols
 * (FSST) that codes all the strings of the column, so that a granule keeps no table of its own and a scan encodes its needle once for the part; at the cold level, for long strings, a dictionary
 * of zstd. The columns whose strings are few distinct ones have a dictionary of the strings (part_write.c) and are not here. Each is taken only when a sample of the strings says that it pays
 * for the bytes it adds to the part; a part that is joined or compacted from parts that share one keeps it (sift_parts_joinable) or adopts the one of the part with most of its rows.
 */
#include "core/host.h"

#include "lib/stringinfo.h"

#include "col/compress.h"
#include "col/fsst.h"
#include "col/granule/part/int.h"
#include "core/memtrack.h"

#define SIFT_CODEC_SAMPLE_STRS		2048
#define SIFT_CODEC_SAMPLE_BYTES		32768
#define SIFT_CODEC_MIN_BYTES		4096
#define SIFT_CODEC_MIN_STRS			64
#define SIFT_CODEC_SMALL_BYTES		8192
#define SIFT_CODEC_FSST_AVG_MAX		64
#define SIFT_CODEC_FSST_MAX_PCT		82
#define SIFT_CODEC_FSST_ADOPT_PCT	85
#define SIFT_CODEC_ZSTD_MIN_BYTES	32768
#define SIFT_CODEC_ZSTD_PROBE		16384
#define SIFT_CODEC_ZSTD_ADOPT_PCT	94

typedef struct StrSample
{
	const char **p;
	uint32	   *len;
	uint32		n;
	uint64		bytes;
	uint64		total_nn;
	uint64		total_bytes;
} StrSample;

typedef struct ZstdCb
{
	MemoryContextCallback cb;
	void	   *cdict;
} ZstdCb;

static void
cdict_free(void *arg)
{
	sift_colz_zstd_free_cdict(((ZstdCb *) arg)->cdict);
}

/* The strings of column col over the n values of the part, and an even sample of at most SIFT_CODEC_SAMPLE_STRS of them (and SIFT_CODEC_SAMPLE_BYTES bytes) */
static void
take_sample(const SiftColIn *col, int n, StrSample *sm)
{
	uint64		stride;
	uint64		seen = 0;

	*sm = (StrSample) {0};
	for (int i = 0; i < n; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			sm->total_nn++;
			sm->total_bytes += col->slen[i];
		}
	stride = Max(sm->total_nn / SIFT_CODEC_SAMPLE_STRS, 1);
	sm->p = palloc(Min(sm->total_nn, SIFT_CODEC_SAMPLE_STRS) * sizeof(char *));
	sm->len = palloc(Min(sm->total_nn, SIFT_CODEC_SAMPLE_STRS) * sizeof(uint32));
	for (int i = 0; i < n && sm->n < SIFT_CODEC_SAMPLE_STRS && sm->bytes < SIFT_CODEC_SAMPLE_BYTES; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			if (seen++ % stride != 0)
				continue;
			sm->p[sm->n] = col->s[i];
			sm->bytes += sm->len[sm->n++] = col->slen[i];
		}
}

/* The bytes the sample codes to with the table */
static uint64
coded_bytes(const SiftFsst *t, const StrSample *sm)
{
	uint64		coded = 0;
	uint32		longest = 0;
	uint8	   *buf;

	for (uint32 i = 0; i < sm->n; i++)
		longest = Max(longest, sm->len[i]);
	buf = palloc(2 * (Size) longest + 1);
	for (uint32 i = 0; i < sm->n; i++)
		coded += sift_fsst_encode(t, sm->p[i], sm->len[i], buf);
	pfree(buf);
	return coded;
}

/* The sample's bytes one after the other, and how many bytes the algorithm the wrapped strings of the tier use (the better of LZ4 and zstd at the warm one, whose rule chooses) holds them in; their own number when there is none */
static uint64
block_bytes(const StrSample *sm, SiftCompressTier tier, int alg)
{
	char	   *raw = palloc(Max(sm->bytes, 1));
	Size		at = 0,
				best = 0;
	int			algs[2] = {alg > 0 ? alg : SIFT_CC_ALG_ZSTD, alg == 0 && tier != SIFT_TIER_COLD_L2 ? SIFT_CC_ALG_LZ4 : 0};

	for (uint32 i = 0; i < sm->n; i++)
	{
		memcpy(raw + at, sm->p[i], sm->len[i]);
		at += sm->len[i];
	}
	for (int k = 0; k < 2 && alg >= 0; k++)
		if (algs[k] > 0 && sift_colz_available(algs[k]))
		{
			Size		cap = sift_colz_bound(algs[k], at);
			char	   *out = palloc(cap);
			Size		z = sift_colz_compress(algs[k], 0, raw, at, out, cap);

			if (z > 0 && (best == 0 || z < best))
				best = z;
			pfree(out);
		}
	pfree(raw);
	return best > 0 ? best : sm->bytes;
}

static const SiftPartDict *
adopted(const SiftPartDicts *adopt, int col, SiftDictKind kind)
{
	for (int i = 0; adopt != NULL && i < adopt->n; i++)
		if (adopt->d[i].col == col && adopt->d[i].kind == kind)
			return &adopt->d[i];
	return NULL;
}

/* The strings from the first row on, one after the other, up to SIFT_CODEC_SAMPLE_BYTES bytes: what a granule of them holds, in their order (a codec that finds repeats gains from it) */
static void
take_run(const SiftColIn *col, int n, StrSample *sm)
{
	*sm = (StrSample) {0};
	sm->p = palloc(Min(n, SIFT_CODEC_SAMPLE_STRS) * sizeof(char *));
	sm->len = palloc(Min(n, SIFT_CODEC_SAMPLE_STRS) * sizeof(uint32));
	for (int i = 0; i < n && sm->n < SIFT_CODEC_SAMPLE_STRS && sm->bytes < SIFT_CODEC_SAMPLE_BYTES; i++)
		if (col->isnull == NULL || !col->isnull[i])
		{
			sm->p[sm->n] = col->s[i];
			sm->bytes += sm->len[sm->n++] = col->slen[i];
		}
}

/*
 * A table for the column: the adopted one when it still codes the sample well, else one trained on it; NULL when the part does not gain by it: the strings must code in at most 82% (the same
 * 88% counting the table), and the wrapped list of a run of them (what each granule keeps otherwise) not be a quarter smaller than the table's codes, the rule of a granule that keeps its own table.
 */
/* Whether a run of the strings coded by the table costs no more than the codec's list of them by the margin of the tier: a twentieth at the cold level, a third below */
static bool
fsst_run_ok(uint64 coded, uint64 block, SiftCompressTier tier)
{
	return tier == SIFT_TIER_COLD_L2 ? coded * 100 <= block * 105 : coded * 3 < block * 4;
}

static SiftFsst *
fsst_for(const SiftColIn *col, int n, const StrSample *sm, SiftCompressTier tier, int alg, const SiftPartDict *adopt, StringInfo table)
{
	SiftFsst   *t = palloc(sizeof(SiftFsst));
	StrSample	run;
	uint64		coded,
				est;
	bool		pays;

	if (adopt != NULL)
	{
		*t = *adopt->fsst;
		if (coded_bytes(t, sm) * 100 <= sm->bytes * SIFT_CODEC_FSST_ADOPT_PCT)
		{
			sift_fsst_put_table(table, t);
			return t;
		}
	}
	sift_fsst_train(t, sm->p, sm->len, sm->n);
	coded = coded_bytes(t, sm);
	resetStringInfo(table);
	sift_fsst_put_table(table, t);
	est = table->len + sm->total_bytes * coded / Max(sm->bytes, 1);
	take_run(col, n, &run);
	pays = coded * 100 <= sm->bytes * SIFT_CODEC_FSST_MAX_PCT && est < sm->total_bytes * 88 / 100 && fsst_run_ok(coded_bytes(t, &run), block_bytes(&run, tier, alg), tier);
	pfree(run.p);
	pfree(run.len);
	if (pays)
		return t;
	pfree(t);
	return NULL;
}

/* The bytes of the strings of one granule, from the first, that a zstd dictionary and plain zstd hold them in */
static void
zstd_probe(const SiftColIn *col, int n, void *cdict, uint64 *with_dict, uint64 *plain, uint64 *raw)
{
	StringInfoData buf;
	Size		cap;
	char	   *out;

	initStringInfo(&buf);
	for (int i = 0; i < n && buf.len < SIFT_CODEC_ZSTD_PROBE; i++)
		if (col->isnull == NULL || !col->isnull[i])
			appendBinaryStringInfo(&buf, col->s[i], col->slen[i]);
	cap = sift_colz_bound(SIFT_CC_ALG_ZSTD, buf.len);
	out = palloc(cap);
	*raw = buf.len;
	*with_dict = sift_colz_compress_zdict(cdict, buf.data, buf.len, out, cap);
	*plain = sift_colz_compress(SIFT_CC_ALG_ZSTD, SIFT_ZSTD_L2_LEVEL, buf.data, buf.len, out, cap);
	pfree(out);
	pfree(buf.data);
}

/* A zstd dictionary for the column: the adopted one when it still holds the probe well, else one trained on an even sample; NULL when the part does not gain by it. Its bytes: *raw, *rawlen */
static void *
zstd_for(MemoryContext cx, const SiftColIn *col, int n, const StrSample *sm, uint64 fsst_est, const SiftPartDict *adopt, uint8 **raw, uint32 *rawlen)
{
	uint64		with_dict,
				plain,
				probe_raw,
				est_dict,
				est_plain;
	uint32		dict_cap = (uint32) Min((uint64) SIFT_ZSTD_DICT_MAX_BYTES, Max((uint64) SIFT_ZSTD_DICT_MIN_BYTES, sm->total_bytes / 16));
	uint8	   *dict = palloc(dict_cap);
	Size		zlen = 0;
	void	   *cdict;
	ZstdCb	   *cb;

	if (adopt != NULL)
	{
		memcpy(dict, adopt->bytes, Min(adopt->n, dict_cap));
		zlen = Min(adopt->n, dict_cap) == adopt->n ? adopt->n : 0;
	}
	if (zlen == 0)
	{
		StringInfoData buf;
		Size	   *sizes = palloc(SIFT_CODEC_SAMPLE_STRS * sizeof(Size));
		unsigned	ns = 0;
		uint64		stride = Max(sm->total_nn / SIFT_CODEC_SAMPLE_STRS, 1),
					seen = 0;

		initStringInfo(&buf);
		for (int i = 0; i < n && ns < SIFT_CODEC_SAMPLE_STRS && buf.len < SIFT_ZSTD_DICT_SAMPLE_CAP; i++)
			if ((col->isnull == NULL || !col->isnull[i]) && col->slen[i] > 0 && seen++ % stride == 0)
			{
				appendBinaryStringInfo(&buf, col->s[i], col->slen[i]);
				sizes[ns++] = col->slen[i];
			}
		zlen = sift_colz_zstd_train_dict(dict, dict_cap, buf.data, sizes, ns);
		pfree(buf.data);
		pfree(sizes);
	}
	if (zlen < SIFT_ZSTD_DICT_MIN_BYTES)
		return NULL;
	cdict = sift_colz_zstd_create_cdict(dict, zlen, SIFT_ZSTD_L2_LEVEL);
	if (cdict == NULL)
		return NULL;
	cb = MemoryContextAlloc(cx, sizeof(ZstdCb));
	cb->cb = (MemoryContextCallback) {.func = cdict_free,.arg = cb};
	cb->cdict = cdict;
	MemoryContextRegisterResetCallback(cx, &cb->cb);
	zstd_probe(col, n, cdict, &with_dict, &plain, &probe_raw);
	est_dict = zlen + sm->total_bytes * with_dict / Max(probe_raw, 1);
	est_plain = sm->total_bytes * plain / Max(probe_raw, 1);
	if (with_dict > 0 && plain > 0 && (adopt != NULL ? with_dict * 100 <= plain * SIFT_CODEC_ZSTD_ADOPT_PCT : est_dict * 100 <= est_plain * 92 && (fsst_est == UINT64_MAX || est_dict * 100 < fsst_est * 90)))
	{
		*raw = dict;
		*rawlen = (uint32) zlen;
		return cdict;
	}
	cb->cdict = NULL;
	sift_colz_zstd_free_cdict(cdict);
	pfree(dict);
	return NULL;
}

SiftPartCodecs *
sift_part_codecs_train(const SiftRows *rows, SiftCompressTier tier, SiftDictEnc *const *dicts, const SiftColPromo *promos, const SiftPartDicts *adopt)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift part codecs"),
				old = MemoryContextSwitchTo(cx);
	/* check-memory: allow one pointer for each column of the part, not for each row */
	SiftPartCodecs *pc = palloc0(sizeof(SiftPartCodecs));
	int			ng = (rows->nrows + SIFT_GRANULE_ROWS - 1) / SIFT_GRANULE_ROWS;

	pc->cx = cx;
	pc->ncols = rows->ncols;
	/* check-memory: allow one pointer for each column of the part, not for each row */
	pc->fsst = palloc0(Max(rows->ncols, 1) * sizeof(SiftFsst *));
	/* check-memory: allow one pointer for each column of the part, not for each row */
	pc->cdict = palloc0(Max(rows->ncols, 1) * sizeof(void *));
	/* check-memory: allow one pointer for each column of the part, not for each row */
	pc->raw = palloc0(Max(rows->ncols, 1) * sizeof(uint8 *));
	/* check-memory: allow one length for each column of the part, not for each row */
	pc->rawlen = palloc0(Max(rows->ncols, 1) * sizeof(uint32));
	for (int c = 0; c < rows->ncols; c++)
	{
		const SiftColIn *col = &rows->cols[c];
		int			n = col->group1 > 0 ? (int) rows->cols[col->group1 - 1].eoff[rows->nrows] : rows->nrows;
		StrSample	sm;
		StringInfoData table;
		SiftFsst   *t;
		uint64		fsst_est = UINT64_MAX;

		CHECK_FOR_INTERRUPTS();
		if (col->kind != SIFT_CELL_STR || col->s == NULL || (dicts != NULL && dicts[c] != NULL) || (promos != NULL && promos[c].kind != SIFT_PROMO_NONE) ||
			(col->hint & ~SIFT_CHINT_ASTEXT) != SIFT_CHINT_AUTO)
			continue;
		take_sample(col, n, &sm);
		if ((ng < 2 && sm.total_bytes < SIFT_CODEC_SMALL_BYTES) || sm.total_bytes < SIFT_CODEC_MIN_BYTES || sm.total_nn < SIFT_CODEC_MIN_STRS)
			continue;
		initStringInfo(&table);
		t = sm.total_bytes < (uint64) SIFT_CODEC_FSST_AVG_MAX * sm.total_nn ? fsst_for(col, n, &sm, tier, rows->compress_alg, adopted(adopt, c, SIFT_DICT_KIND_FSST), &table) : NULL;
		if (t != NULL)
		{
			pc->fsst[c] = t;
			pc->raw[c] = (uint8 *) table.data;
			pc->rawlen[c] = table.len;
			fsst_est = table.len + sm.total_bytes * coded_bytes(t, &sm) / Max(sm.bytes, 1);
		}
		if (tier == SIFT_TIER_COLD_L2 && rows->compress_alg >= 0 && rows->compress_alg != SIFT_CC_ALG_LZ4 && sift_colz_available(SIFT_CC_ALG_ZSTD) && ng >= 2 &&
			sm.total_bytes >= SIFT_CODEC_ZSTD_MIN_BYTES)
		{
			uint8	   *raw = NULL;
			uint32		rawlen = 0;
			void	   *cdict = zstd_for(cx, col, n, &sm, fsst_est, adopted(adopt, c, SIFT_DICT_KIND_ZSTD), &raw, &rawlen);

			if (cdict != NULL)
			{
				pc->fsst[c] = NULL;
				pc->cdict[c] = cdict;
				pc->raw[c] = raw;
				pc->rawlen[c] = rawlen;
			}
		}
		pc->nentries += pc->raw[c] != NULL;
	}
	MemoryContextSwitchTo(old);
	return pc;
}

void
sift_part_codecs_free(SiftPartCodecs *pc)
{
	MemoryContextDelete(pc->cx);
}
