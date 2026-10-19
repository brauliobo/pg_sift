/*
 * fz_colchunk.c - fuzz target for every decoder of the column engine that reads stored bytes (docs/SAFETY.md, test/fuzz/README.md).
 * One input = one mode byte + the bytes of one stored object:
 *
 *   0  chunk codec blob       sift_cc_open (verify on/off), decode, off32, filter, sum, get, decode_sel at every SIMD level; levels must agree
 *   1  string list blob       sift_cc_decode_strs
 *   2  bit vector blob        sift_cc_decode_bits
 *   3  unit chunk             sift_cchunk_parse, decode, strs, zone, tests (range, set, LUT) at every level; levels must agree
 *   4  unit record            sift_cunit_parse (+ _head), chunks, keys, page masks, sift_cunit_decode
 *   5  segment dictionary     sift_cdict_parse, every string
 *   6  schema                 sift_cschema_parse
 *
 * The contract being fuzzed: a blob either raises a corruption error (ERRCODE_DATA_CORRUPTED / INDEX_CORRUPTED, or FEATURE_NOT_SUPPORTED)
 * or is memory safe to decode, filter and sum; never anything else. The reader's own promises are kept: a unit chunk and a unit record are
 * 8-byte aligned and have SIFT_COL_PAD zero bytes behind them, a codec blob is an exact-size block with nothing behind it.
 *
 *   FZ_GEN=DIR target     writes the seed corpus (valid objects of the writers, every policy and shape) and exits
 */
#include "postgres.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>


#include "sift_col.h"
#include "sift_col_enc.h"
#include "sift_colcodec.h"
#include "fz_shim.h"
#include "sift_testrng.h"

typedef uint64 CcgRng;

static inline uint64
ccg_next(CcgRng *r)
{
	return sift_splitmix_next(r);
}

#include "sift_colcodec_gen.h"

#define MAX_ROWS (1u << 20)		/* what a harness decodes: beyond it only the parse is exercised */
#define NMODES 7

static SiftColSchema *schema;
static SiftCDict *dicts[8];

static const SiftColDef defs[] = {
	{0, -1, SIFT_COLK_SCALAR, SIFT_COLS_INT, 0, 1, SIFT_COLT_INT8, "a"},
	{1, -1, SIFT_COLK_SCALAR, SIFT_COLS_F8, 0, 2, SIFT_COLT_FLOAT8, "b"},
	{2, -1, SIFT_COLK_SCALAR, SIFT_COLS_STR, 0, 3, SIFT_COLT_TEXT, "c"},
	{3, -1, SIFT_COLK_GROUP, SIFT_COLS_INT, 0, 4, SIFT_COLT_INT4, "items[]"},
	{4, 3, SIFT_COLK_SCALAR, SIFT_COLS_INT, 0, 4, SIFT_COLT_INT8, "items[].q"},
	{5, 3, SIFT_COLK_SCALAR, SIFT_COLS_STR, 0, 4, SIFT_COLT_TEXT, "items[].k"},
	{6, -1, SIFT_COLK_SCALAR, SIFT_COLS_VAR, 0, 5, SIFT_COLT_BYTEA, "v"},
};

static void
setup(void)
{
	static bool done;

	if (done)
		return;
	done = true;
	fz_init();
	schema = sift_cschema_create(CurrentMemoryContext, defs, lengthof(defs));
	for (int c = 0; c < lengthof(defs); c++)
		if (defs[c].store == SIFT_COLS_STR)
		{
			dicts[c] = sift_cdict_create(CurrentMemoryContext);
			for (int i = 0; i < 40; i++)
			{
				char		s[16];

				sift_cdict_intern(dicts[c], s, (uint32) snprintf(s, sizeof(s), "k%d", i));
			}
		}
	pgshim_keep();
}

/* ---------- one decoder run, per mode ---------- */

typedef struct Job
{
	uint8	   *p;
	size_t		len;
	uint32		arg;			/* a number taken from the input: verify flag, bit count, filter bounds */
	uint64		digest;			/* of everything decoded, compared across SIMD levels */
} Job;

static uint64
mix(uint64 h, uint64 v)
{
	return (h ^ v) * UINT64CONST(0x100000001B3);
}

/* FZ_TRACE=1: the running digest after each step, to find which operation two SIMD levels disagree on */
static void
trace(const Job *j, const char *step)
{
	if (getenv("FZ_TRACE"))
		fprintf(stderr, "  %-10s %016llx\n", step, (unsigned long long) j->digest);
}

static void
cc_run(void *a)
{
	Job		   *j = a;
	SiftCc	   *cc = sift_cc_open(CurrentMemoryContext, j->p, j->len, j->arg & 1);
	uint32		n = sift_cc_nrows(cc);
	int64		sum = 0;

	if (n <= MAX_ROWS)
	{
		int64	   *dec = palloc(sizeof(int64) * (n + SIFT_COL_PAD / 8));
		uint32	   *off = palloc(sizeof(uint32) * SIFT_CC_CAP32(n));
		uint64	   *bits = palloc0(8 * SIFT_COL_NWORDS(n));
		uint32	   *idx = palloc(sizeof(uint32) * 64);
		int64	   *sel = palloc(sizeof(int64) * (64 + SIFT_COL_PAD / 8));
		int64		lo = sift_cc_min(cc) + (int64) (j->arg >> 1 & 0xff),
					hi = lo + (int64) (j->arg >> 9 & 0xffff);

		sift_cc_decode(cc, dec);
		for (uint32 i = 0; i < n; i++)
			j->digest = mix(j->digest, (uint64) dec[i]);
		trace(j, "decode");
		if (sift_cc_decode_off32(cc, off))
			for (uint32 i = 0; i < n; i++)
				j->digest = mix(j->digest, off[i]);
		trace(j, "off32");
		sift_cc_filter(cc, lo, hi, bits);
		for (uint32 w = 0; w < (n + 63) / 64; w++)	/* the words past the rows are the kernel's padding, not part of the result */
			j->digest = mix(j->digest, bits[w]);
		trace(j, "filter");
		j->digest = mix(j->digest, sift_cc_sum(cc, bits, &sum));
		j->digest = mix(j->digest, (uint64) sum);
		trace(j, "sum");
		if (n)
		{
			for (uint32 k = 0; k < 64; k++)
				idx[k] = (uint32) ((j->arg * UINT64CONST(2654435761) + k * UINT64CONST(40503)) % n);
			sift_cc_decode_sel(cc, idx, 64, sel);
			for (uint32 k = 0; k < 64; k++)
				j->digest = mix(j->digest, (uint64) (sift_cc_get(cc, idx[k]) ^ sel[k]));
		}
	}
	sift_cc_close(cc);
}

static void
strs_run(void *a)
{
	Job		   *j = a;
	uint32	   *off;
	char	   *bytes;
	uint32		n = sift_cc_decode_strs(CurrentMemoryContext, j->p, j->len, &off, &bytes);

	for (uint32 i = 0; i < n; i++)
	{
		if (off[i + 1] < off[i])
			fz_bad("string offsets decrease at %u", i);
		for (uint32 k = off[i]; k < off[i + 1]; k++)
			j->digest = mix(j->digest, (uint8) bytes[k]);
	}
}

static void
bits_run(void *a)
{
	Job		   *j = a;
	uint32		nbits = j->arg % 200000;
	uint64	   *w = palloc0(8 * ((nbits + 63) / 64 + 1));

	sift_cc_decode_bits(j->p, j->len, nbits, w);
	for (uint32 i = 0; i < (nbits + 63) / 64; i++)
		j->digest = mix(j->digest, w[i]);
}

/* the code lane of a dictionary of (arg >> 20) % 400 values: it decodes to codes inside the tables built over that dictionary, or raises */
static void
chunk_codes(const SiftCChunk *c, Job *j)
{
	uint32		ndict = (j->arg >> 20) % 400;
	uint32	   *codes = palloc(sizeof(uint32) * SIFT_CC_CAP32(c->n));

	sift_cchunk_decode_codes(c, ndict, codes);
	for (uint32 i = 0; i < c->n; i++)
	{
		if (codes[i] > ndict)
			fz_bad("a code of %u in a dictionary of %u values", codes[i], ndict);
		j->digest = mix(j->digest, codes[i]);
	}
}

static void
chunk_tests(const SiftCChunk *c, Job *j)
{
	uint32		n = c->n;
	uint64	   *bits = palloc0(8 * SIFT_COL_NWORDS(n));
	SiftCTest	t = {0};
	int64		set[3];
	uint64	   *lut;
	uint32		lutbits = 1 + (j->arg >> 4) % 300;

	t.kind = SIFT_CT_RANGE;
	t.lo = (int64) (int32) j->arg;
	t.hi = t.lo + (int64) (j->arg >> 12 & 0xfff);
	for (int w = 0; w < (int) SIFT_COL_NWORDS(n); w++)
		bits[w] = ~UINT64CONST(0);
	sift_ctest_run(&t, c, bits);
	for (int w = 0; w < (int) ((n + 63) / 64); w++)
		j->digest = mix(j->digest, bits[w]);
	set[0] = t.lo, set[1] = t.lo + 5, set[2] = t.lo + 77;
	t.kind = SIFT_CT_SET;
	t.set = set;
	t.nset = 3;
	t.hi = set[2];
	for (int w = 0; w < (int) SIFT_COL_NWORDS(n); w++)
		bits[w] = ~UINT64CONST(0);
	sift_ctest_run(&t, c, bits);
	for (int w = 0; w < (int) ((n + 63) / 64); w++)
		j->digest = mix(j->digest, bits[w]);
	/* a predicate on a dictionary leaf: the lookup table is as long as the dictionary the segment would have (lutbits), not as the chunk says */
	if (fz_skip("lut"))
		return;
	lut = palloc0(8 * ((lutbits + 63) / 64));
	for (uint32 i = 0; i < lutbits; i += 3)
		lut[i / 64] |= UINT64CONST(1) << (i % 64);
	t.kind = SIFT_CT_LUT;
	t.lut = lut;
	t.lutbits = lutbits;
	t.lo = 0;
	t.hi = lutbits - 1;
	for (int w = 0; w < (int) SIFT_COL_NWORDS(n); w++)
		bits[w] = ~UINT64CONST(0);
	sift_ctest_run(&t, c, bits);
	for (int w = 0; w < (int) ((n + 63) / 64); w++)
		j->digest = mix(j->digest, bits[w]);
}

static void
cchunk_run(void *a)
{
	Job		   *j = a;
	SiftCChunk	c;
	SiftCZone	z;

	if (!sift_cchunk_parse(j->p, (uint32) j->len, &c) || c.n > MAX_ROWS)
		return;
	{
		int64	   *dec = palloc(sizeof(int64) * (c.n + SIFT_COL_PAD / 8));

		sift_cchunk_decode(&c, dec);
		for (uint32 i = 0; i < c.n; i++)
			j->digest = mix(j->digest, (uint64) dec[i]);
	}
	if (c.enc == SIFT_COLENC_STRS)
	{
		uint32	   *off;
		char	   *bytes;

		sift_cchunk_strs(&c, CurrentMemoryContext, &off, &bytes);
		for (uint32 i = 0; i < c.n; i++)
			j->digest = mix(j->digest, off[i + 1] - off[i]);
		return;
	}
	sift_cchunk_zone(&c, SIFT_COLS_INT, &z);
	j->digest = mix(mix(j->digest, (uint64) z.min), (uint64) z.max);
	chunk_tests(&c, j);
	chunk_codes(&c, j);
}

static void
unit_run(void *a)
{
	Job		   *j = a;
	SiftCUnitView v;

	if (!sift_cunit_parse(j->p, (uint32) j->len, schema, &v))
		return;
	j->digest = mix(j->digest, v.nrows);
	if (v.nrows > SIFT_COL_UNIT_MAX_ROWS || v.npages > SIFT_SB_PAGES)
		fz_bad("sift_cunit_parse accepted a unit of %u rows, %u pages", v.nrows, v.npages);
	{
		SiftKey    *keys = palloc(sizeof(SiftKey) * (v.nrows + 1));
		uint64		om[SIFT_OFFMASK_WORDS];

		sift_cunit_keys(&v, 7, keys);
		for (uint32 i = 0; i < v.nrows; i++)
			j->digest = mix(j->digest, keys[i]);
		for (uint32 p = 0; p < v.npages; p++)
		{
			j->digest = mix(j->digest, sift_cunit_page_block(&v, 7, p));
			sift_cunit_page_offmask(&v, p, om);
			j->digest = mix(j->digest, om[0]);
		}
		for (uint32 r = 0; r < v.nrows; r += 1 + v.nrows / 16)
			j->digest = mix(j->digest, sift_cunit_row_page(&v, r));
	}
	for (int col = 0; col < schema->ncols; col++)
	{
		SiftCChunk	c;

		if (!sift_cunit_chunk(&v, col, &c) || c.n > MAX_ROWS)
			continue;
		if (c.enc != SIFT_COLENC_STRS)
		{
			int64	   *dec = palloc(sizeof(int64) * (c.n + SIFT_COL_PAD / 8));

			sift_cchunk_decode(&c, dec);
			j->digest = mix(j->digest, (uint64) dec[c.n ? c.n - 1 : 0]);
		}
	}
	{
		SiftColBatch *b = sift_cbatch_create(CurrentMemoryContext, schema);

		sift_cbatch_reset(b, 7);
		sift_cunit_decode(&v, 7, dicts, NULL, b);
		j->digest = mix(j->digest, b->nrows);
	}
}

static void
dict_run(void *a)
{
	Job		   *j = a;
	Size		used;
	SiftCDict  *d = sift_cdict_parse(CurrentMemoryContext, j->p, j->len, &used);

	if (!d)
		return;
	for (uint32 c = 0; c < d->n; c++)
	{
		uint32		len;
		const char *s = sift_cdict_str(d, c, &len);

		for (uint32 k = 0; k < len; k++)
			j->digest = mix(j->digest, (uint8) s[k]);
	}
}

static void
schema_run(void *a)
{
	Job		   *j = a;
	SiftColSchema *s = sift_cschema_parse(CurrentMemoryContext, j->p, j->len);

	if (s)
		j->digest = mix(j->digest, (uint64) s->ncols);
}

static void (*const runs[NMODES]) (void *) = {cc_run, strs_run, bits_run, cchunk_run, unit_run, dict_run, schema_run};
static const size_t pads[NMODES] = {0, 0, 0, SIFT_COL_PAD, SIFT_COL_PAD, 0, 0};	/* what the reader of that object keeps behind it */
static const size_t aligns[NMODES] = {8, 8, 8, 8, 8, 8, 8};

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	SiftSimdLevel lv[3];
	int			nl;
	uint64		first = 0;
	int			mode;
	Job			j;

	setup();
	if (size < 6)
		return 0;
	mode = getenv("FZ_MODE") && *getenv("FZ_MODE") ? atoi(getenv("FZ_MODE")) % NMODES : data[0] % NMODES;
	memcpy(&j.arg, data + 1, 4);
	nl = fz_levels(lv);
	j.len = size - 5;
	for (int l = 0; l < nl; l++)
	{
		j.p = fz_copy(data + 5, j.len, pads[mode], aligns[mode]);
		j.digest = 0;
		fz_use_level(lv[l]);
		if (!fz_try(runs[mode], &j))
		{
			free(j.p);
			pgshim_reset();
			break;				/* the same error at every level: the check is not kernel code */
		}
		free(j.p);
		pgshim_reset();
		if (l == 0)
			first = j.digest;
		else if (j.digest != first)
			fz_bad("mode %d: SIMD level %d decodes differently from scalar (%llx vs %llx)", mode, (int) lv[l],
				   (unsigned long long) j.digest, (unsigned long long) first);
	}
	fz_use_level(SIFT_SIMD_SCALAR);
	return 0;
}

/* ---------- the seed corpus ---------- */

static void
emit(const char *dir, const char *name, int mode, uint32 arg, const void *p, size_t len)
{
	char		path[512];
	FILE	   *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen(path, "wb")))
		abort();
	fputc(mode, f);
	fwrite(&arg, 4, 1, f);
	fwrite(p, 1, len, f);
	fclose(f);
}

static const char *const seed_policies[] = {
	"auto", "none", "lz4", "zstd", "for", "delta", "rle", "dict", "alp", "scale", "for+lz4", "rle+dict", "dict+zstd", "scale+rle+dict+zstd"
};

static void
gen_codec(const char *dir)
{
	CcgRng		r = 42;
	static const uint32 sizes[] = {1, 31, 32, 33, 1000, 1056, 5000};

	for (int g = 0; g < CCG_NGEN; g++)
		for (size_t s = 0; s < lengthof(sizes); s += 2)
			for (size_t p = 0; p < lengthof(seed_policies); p += 3)
			{
				uint32		n = sizes[(s + (uint32) g) % lengthof(sizes)];
				int64	   *v = palloc((n + 1) * sizeof(int64));
				SiftCcKind	kind;
				SiftCcPolicy pol;
				StringInfoData out;
				char		name[64];

				ccg_fill(&r, g, n, v, &kind);
				sift_cc_policy_parse(seed_policies[p], &pol);
				initStringInfo(&out);
				sift_cc_encode(&out, kind, v, n, NULL, &pol, NULL);
				snprintf(name, sizeof(name), "cc-%d-%zu-%zu", g, s, p);
				emit(dir, name, 0, (uint32) (g * 7919 + 1), out.data, out.len);
			}
	/* strings and bit vectors */
	for (int k = 0; k < 6; k++)
	{
		uint32		n = 5 + 37 * k;
		uint32	   *off = palloc((n + 1) * 4);
		char	   *bytes = palloc(n * 8 + 8);
		uint64	   *w = palloc0(8 * (n / 64 + 2));
		SiftCcPolicy pol;
		StringInfoData out;
		char		name[64];

		off[0] = 0;
		for (uint32 i = 0; i < n; i++)
		{
			off[i + 1] = off[i] + (uint32) (ccg_next(&r) % 8);
			memset(bytes + off[i], 'a' + (int) (i % 5), off[i + 1] - off[i]);
			if (i % 11 == 0)
				w[i / 64] |= UINT64CONST(1) << (i % 64);
		}
		sift_cc_policy_parse(seed_policies[k], &pol);
		initStringInfo(&out);
		sift_cc_encode_strs(&out, off, bytes, n, &pol);
		snprintf(name, sizeof(name), "strs-%d", k);
		emit(dir, name, 1, 0, out.data, out.len);
		initStringInfo(&out);
		sift_cc_encode_bits(&out, w, n, &pol);
		snprintf(name, sizeof(name), "bits-%d", k);
		emit(dir, name, 2, n, out.data, out.len);
	}
}

/* units of the writer from random rows: each unit whole, each of its chunks, the dictionary and the schema */
static void
gen_units(const char *dir)
{
	CcgRng		r = 7;

	for (int k = 0; k < 8; k++)
	{
		uint32		nrows = k == 0 ? 3 : 100 + 400 * (uint32) k;
		SiftColBatch *b = sift_cbatch_create(CurrentMemoryContext, schema);
		SiftCcPolicy pol[lengthof(defs)];
		StringInfoData out;
		Size		start;
		SiftCUnitView v;
		char		name[64];
		uint8	   *rec;

		sift_cbatch_reset(b, 7);
		for (uint32 i = 0; i < nrows; i++)
		{
			SiftColRow	row = {0};
			SiftColValue val[lengthof(defs)] = {{{0}}};
			SiftColValue *elem[lengthof(defs)] = {NULL};
			char		str[24];
			SiftColValue items[5];
			SiftColValue keys[5];
			uint32		ne = (uint32) (ccg_next(&r) % 5);

			ItemPointerSet(&row.tid, 7 * SIFT_SB_PAGES + i / 100, (OffsetNumber) (1 + i % 100));
			row.ncols = lengthof(defs);
			row.val = val;
			row.elem = elem;
			val[0].v.i = (int64) (ccg_next(&r) % (k % 2 ? 100 : 1000000)) - 50;
			val[1].v.f = (double) (ccg_next(&r) % 100000) / 100.0;
			val[2].v.s = pstrdup((snprintf(str, sizeof(str), "k%u", (unsigned) (ccg_next(&r) % 30)), str));
			val[2].len = (uint32) strlen(val[2].v.s);
			val[2].isnull = i % 17 == 0;
			val[3].v.i = ne;
			for (uint32 e = 0; e < ne; e++)
			{
				items[e] = (SiftColValue) {{0}};
				items[e].v.i = (int64) (ccg_next(&r) % 50);
				keys[e] = (SiftColValue) {{0}};
				keys[e].v.s = "item";
				keys[e].len = 4;
			}
			elem[4] = items;
			elem[5] = keys;
			val[6].v.s = "opaque bytes";
			val[6].len = 12;
			sift_cbatch_add(b, &row);
		}
		for (int c = 0; c < (int) lengthof(defs); c++)
			sift_cc_policy_parse(seed_policies[(k + c) % 6], &pol[c]);
		initStringInfo(&out);
		sift_cunit_encode(b, NULL, NULL, k % 2 ? pol : NULL, &out, &start, NULL);
		rec = fz_copy((uint8 *) out.data + start, out.len - start, SIFT_COL_PAD, 8);
		snprintf(name, sizeof(name), "unit-%d", k);
		emit(dir, name, 4, 0, rec, out.len - start);
		if (sift_cunit_parse(rec, (uint32) (out.len - start), schema, &v))
			for (int c = 0; c < schema->ncols; c++)
			{
				SiftCChunk	ch;

				if (v.chunk_len[c] && sift_cunit_chunk(&v, c, &ch))
				{
					snprintf(name, sizeof(name), "chunk-%d-%d", k, c);
					emit(dir, name, 3, (uint32) (k * 31 + c), rec + v.chunk_off[c], v.chunk_len[c]);
				}
			}
	}
	{
		StringInfoData out;

		initStringInfo(&out);
		sift_cschema_serialize(schema, &out);
		emit(dir, "schema", 6, 0, out.data, out.len);
		initStringInfo(&out);
		sift_cdict_serialize(dicts[2], NULL, &out);
		emit(dir, "dict", 5, 0, out.data, out.len);
		for (int p = 0; p < 4; p++)
		{
			SiftCcPolicy pol;
			char		name[16];

			sift_cc_policy_parse(seed_policies[p], &pol);
			initStringInfo(&out);
			sift_cdict_serialize(dicts[2], &pol, &out);
			snprintf(name, sizeof(name), "dict-%d", p);
			emit(dir, name, 5, 0, out.data, out.len);
		}
	}
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *dir = getenv("FZ_GEN");

	setup();
	if (dir)
	{
		mkdir(dir, 0755);
		gen_codec(dir);
		gen_units(dir);
		_exit(0);
	}
	return 0;
}
