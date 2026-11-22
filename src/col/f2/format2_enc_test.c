/*
 * col/format2_enc_test.c - test-only SQL function sift._test_f2_enc for the chunk encodings of format major 2 (SIFT_TESTING builds, registered by pg_sift_test): a column of generated
 * values is encoded as one granule, read back by the maintenance decoders and by the vector decoder, and compared with what went in; the answer names the encoding the writer chose.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "col/codec.h"
#include "col/compress.h"
#include "col/fsst.h"
#include "col/granule/granule_int.h"

PG_FUNCTION_INFO_V1(sift_test_f2_enc);

static uint64
rnd(uint64 *s)
{
	*s = *s * UINT64CONST(6364136223846793005) + UINT64CONST(1442695040888963407);
	return *s >> 17;
}

static const char *
enc_label(uint16 enc)
{
	static const char *names[] = {"?", "TID_DELTA", "INT_FOR", "INT_DELTA", "INT_CONST", "DICT_CODE", "STR_INLINE", "FLOAT_RAW", "LIST_LEN", "ALL_NULL", "BOOL_BITS", "NUM64", "NUM128",
	"UUID16", "INTERVAL16", "RLE", "ALP", "SCALE", "STR_VIEW", "TID_BLOCK", "FLOAT_BSS", "STR_FSST", "STR_BLOCK", "FIXED16", "KEY_PRESENT", "ALP_RD", "STR_PROMO"};

	return (enc & SIFT_ENC_MASK) < lengthof(names) ? names[enc & SIFT_ENC_MASK] : "?";
}

typedef struct Gen
{
	int			n;
	int64	   *i;
	double	   *f;
	char	  **s;
	uint32	   *slen;
	bool	   *isnull;
	uint8	   *tag;
} Gen;

static void
gen_ints(Gen *g, const char *pat, uint64 *rs)
{
	for (int r = 0; r < g->n; r++)
		g->i[r] = strcmp(pat, "const") == 0 ? 42 : strcmp(pat, "seq") == 0 ? 1000 + r : strcmp(pat, "sorted") == 0 ? (int64) r * 3 + (int64) (rnd(rs) % 3) :
			strcmp(pat, "runs") == 0 ? (r / 50) % 7 * 1000003 : strcmp(pat, "wide") == 0 ? (int64) rnd(rs) * 4099 - (int64) (rnd(rs) << 20) : strcmp(pat, "bool") == 0 ? rnd(rs) % 2 :
			strcmp(pat, "list") == 0 ? (int64) (rnd(rs) % 6) : strcmp(pat, "money") == 0 ? (int64) (rnd(rs) % 1000000) - 500000 : (int64) (rnd(rs) % 100);
}

static double
special_f8(int r, uint64 *rs)
{
	static const uint64 bits[] = {UINT64CONST(0x8000000000000000), UINT64CONST(0x7FF8000000000000), UINT64CONST(0x7FF0000000000000), UINT64CONST(0xFFF0000000000000),
								  UINT64CONST(1), UINT64CONST(0x000FFFFFFFFFFFFF), UINT64CONST(0x7FEFFFFFFFFFFFFF), UINT64CONST(0x7FF8000000000123)};
	double		d = 1.0 + (double) (rnd(rs) % UINT64CONST(1099511627776)) / 1099511627776.0;

	if (r % 11 == 0)
		memcpy(&d, &bits[(r / 11) % lengthof(bits)], 8);
	return d;
}

static double
outlier_f8(int r, uint64 *rs)
{
	double		d = 1.0 + (double) (rnd(rs) % UINT64CONST(1099511627776)) / 1099511627776.0;
	uint64		bits = (rnd(rs) << 1) ^ rnd(rs);

	if (r % 50 == 7)
		memcpy(&d, &bits, 8);
	return d;
}

static void
gen_f8(Gen *g, const char *pat, uint64 *rs)
{
	for (int r = 0; r < g->n; r++)
		g->f[r] = strcmp(pat, "outliers") == 0 ? outlier_f8(r, rs) : strcmp(pat, "wideexp") == 0 ? ldexp(1.0 + (double) (rnd(rs) % UINT64CONST(1048576)) / 1048576.0, (r % 40) * 8 - 160) : strcmp(pat, "noisy") == 0 ? (double) r * 0.1 + (double) (rnd(rs) % 1000003) * 1e-9 : strcmp(pat, "narrow") == 0 ? 1.0 + (double) (rnd(rs) % UINT64CONST(1099511627776)) / 1099511627776.0 : strcmp(pat, "special") == 0 ? special_f8(r, rs) : strcmp(pat, "const") == 0 ? 2.5 : strcmp(pat, "decimal") == 0 ? (double) (int64) (rnd(rs) % 100000) / 100.0 : strcmp(pat, "exceptions") == 0 ?
			(r % 20 == 7 ? 1.0 / 3.0 : (double) (int64) (rnd(rs) % 100000) / 100.0) : (double) (int64) rnd(rs) / 7.0;
}

static const char *const words[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa"};

/* String r of a pattern into buf (8 kB); its length */
static uint32
fsst_gen(const char *pat, int r, uint64 *rs, char *buf)
{
	uint32		len = 0;

	if (strcmp(pat, "empty") == 0)
		return 0;
	if (strcmp(pat, "onebyte") == 0)
		buf[len++] = (char) (r % 256);
	else if (strcmp(pat, "tag") == 0)
		len = sprintf(buf, "tag-%05u", (unsigned) (rnd(rs) % 50000));
	else if (strcmp(pat, "url") == 0)
		len = sprintf(buf, "https://example.com/%s/%u?id=%u", words[rnd(rs) % 16], (unsigned) (rnd(rs) % 500), (unsigned) (rnd(rs) % 100000));
	else if (strcmp(pat, "utf8") == 0)
		len = sprintf(buf, "\xC3\xA9t\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E-\xC3\xB1" "and\xC3\xBA-%u-\xE2\x82\xAC", (unsigned) (rnd(rs) % 100000));
	else if (strcmp(pat, "binary") == 0)
		for (uint32 n = 1 + rnd(rs) % 40; len < n;)
			buf[len++] = (char) rnd(rs);
	else if (strcmp(pat, "multikb") == 0)
		for (uint32 n = 2000 + rnd(rs) % 4000; len + 24 < n;)
			len += sprintf(buf + len, "%s%u ", words[rnd(rs) % 16], (unsigned) (rnd(rs) % 1000));
	else if (strcmp(pat, "mixed") == 0)
		len = r % 7 == 0 ? 0 : sprintf(buf, "k%u/%s", (unsigned) (rnd(rs) % 100000), words[rnd(rs) % 16]);
	else
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unknown pattern \"%s\"", pat)));
	return len;
}

static void
gen_strs(Gen *g, const char *pat, const char *kind, uint64 *rs)
{
	char		buf[8192];

	for (int r = 0; r < g->n; r++)
	{
		uint32		len;

		if (strcmp(kind, "str") != 0 && strcmp(kind, "view") != 0)
		{
			len = 16;
			for (int b = 0; b < 16; b++)
				buf[b] = (char) rnd(rs);
		}
		else if (strcmp(pat, "lowcard") == 0)
			len = sprintf(buf, "value-%u-of-the-set", (unsigned) (rnd(rs) % 8));
		else if (strcmp(pat, "short") == 0)
			len = sprintf(buf, "s%u", (unsigned) (rnd(rs) % 100000));
		else if (strcmp(pat, "mixed") == 0)
			len = r % 3 == 0 ? sprintf(buf, "k%u", (unsigned) r) : sprintf(buf, "a-long-string-of-%u-characters-%u", (unsigned) r, (unsigned) rnd(rs));
		else if (strcmp(pat, "unique") == 0 || strcmp(pat, "allnull") == 0)
			len = sprintf(buf, "unique-%d-%llx", r, (unsigned long long) rnd(rs));
		else
			len = fsst_gen(pat, r, rs, buf);
		g->s[r] = memcpy(palloc(len + 1), buf, len);
		g->slen[r] = len;
	}
}

static int
alg_of(const char *name)
{
	return strcmp(name, "lz4") == 0 ? SIFT_CC_ALG_LZ4 : strcmp(name, "zstd") == 0 ? SIFT_CC_ALG_ZSTD : strcmp(name, "none") == 0 ? -1 : 0;
}

static SiftVecType
vec_type(const char *kind, bool narrow)
{
	if (strcmp(kind, "num64") == 0 || strcmp(kind, "money") == 0)
		return SVEC_NUM64;
	if (strcmp(kind, "bool") == 0)
		return SVEC_BOOL;
	if (strcmp(kind, "f8") == 0)
		return SVEC_F64;
	if (strcmp(kind, "uuid") == 0)
		return SVEC_UUID;
	if (strcmp(kind, "num128") == 0)
		return SVEC_NUM128;
	if (strcmp(kind, "interval") == 0)
		return SVEC_INTERVAL;
	if (strcmp(kind, "str") == 0 || strcmp(kind, "view") == 0)
		return SVEC_STRVIEW;
	return narrow ? SVEC_I32 : SVEC_I64;
}

static size_t
elem_size(SiftVecType t)
{
	return t == SVEC_BOOL ? 1 : t == SVEC_I32 || t == SVEC_DICT32 ? 4 : t == SVEC_I64 || t == SVEC_NUM64 || t == SVEC_F64 ? 8 : 16;
}

/* Row r of the vector against the row that went in */
static bool
vec_row_ok(const Gen *g, const char *kind, const SiftVec *v, int r, int row)
{
	bool		valid = v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1);
	const char *d = (const char *) v->data + (size_t) r * elem_size(v->type);

	if (valid == g->isnull[row])
		return false;
	if (!valid)
		return true;
	switch (v->type)
	{
		case SVEC_BOOL:
			return (*(const uint8 *) d != 0) == (g->i[row] != 0);
		case SVEC_I32:
			return *(const int32 *) d == (int32) g->i[row];
		case SVEC_I64:
		case SVEC_NUM64:
			return *(const int64 *) d == g->i[row];
		case SVEC_F64:
			return memcmp(d, &g->f[row], 8) == 0;
		case SVEC_UUID:
			{
				uint64		hi = __builtin_bswap64(*(const uint64 *) g->s[row]),
							lo = __builtin_bswap64(*(const uint64 *) (g->s[row] + 8));

				return ((const uint64 *) d)[0] == hi && ((const uint64 *) d)[1] == lo;
			}
		case SVEC_NUM128:
		case SVEC_INTERVAL:
			return memcmp(d, g->s[row], 16) == 0;
		case SVEC_STRVIEW:
			{
				const SiftStrView *w = (const SiftStrView *) d;
				const char *bytes = w->len <= 12 ? (const char *) w->prefix : w->u.ptr;

				return w->len == g->slen[row] && memcmp(w->len <= 4 ? (const char *) w->prefix : bytes, g->s[row], Min(w->len, w->len <= 12 ? 4 : w->len)) == 0 &&
					(w->len > 12 || w->len <= 4 || memcmp(w->u.tail, g->s[row] + 4, w->len - 4) == 0);
			}
		default:
			return false;
	}
}

/* (kind, pattern, nrows, nulls, tags, wrap): kind int, money, bool, list, f8, str, view, uuid, num128, interval */
Datum
sift_test_f2_enc(PG_FUNCTION_ARGS)
{
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(0)),
			   *pat = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int			n = PG_GETARG_INT32(2);
	bool		nulls = PG_GETARG_BOOL(3),
				tags = PG_GETARG_BOOL(4);
	int			alg = alg_of(text_to_cstring(PG_GETARG_TEXT_PP(5)));
	char	   *tier = text_to_cstring(PG_GETARG_TEXT_PP(6));
	uint64		rs = 12345;
	Gen			g = {.n = n};
	SiftColIn	col = {0};
	SiftGranuleIn in = {.nrows = n,.ncols = 1,.compress_alg = alg,.tier = strcmp(tier, "hot") == 0 ? SIFT_TIER_HOT_L0 : SIFT_TIER_WARM_L1};
	StringInfoData out;
	SiftGranuleDesc desc;
	SiftGranuleView v;
	SiftChunk	c;
	uint64	   *tid = palloc(n * sizeof(uint64));
	bool		legacy = true,
				vec_ok = true,
				tags_ok = true,
				narrow = false;
	char	   *raw;

	if (n < 1 || n > SIFT_GRANULE_ROWS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rows must be 1 to %d", SIFT_GRANULE_ROWS)));
	g.i = palloc0(n * sizeof(int64));
	g.f = palloc0(n * sizeof(double));
	g.s = palloc0(n * sizeof(char *));
	g.slen = palloc0(n * sizeof(uint32));
	g.isnull = palloc0(n * sizeof(bool));
	g.tag = palloc0(n);
	if (strcmp(kind, "f8") == 0)
		gen_f8(&g, pat, &rs);
	else if (strcmp(kind, "str") == 0 || strcmp(kind, "view") == 0 || strcmp(kind, "uuid") == 0 || strcmp(kind, "num128") == 0 || strcmp(kind, "interval") == 0)
		gen_strs(&g, pat, kind, &rs);
	else
		gen_ints(&g, pat, &rs);
	for (int r = 0; r < n; r++)
	{
		tid[r] = ((uint64) (r / 100) << 16) | (uint64) (r % 100 + 1);
		g.isnull[r] = nulls && (r % 5 == 2 || strcmp(pat, "allnull") == 0);
		g.tag[r] = tags && r % 11 == 3 ? (uint8) (1 + r % 7) : 0;
		g.isnull[r] |= g.tag[r] != 0;
		if (g.isnull[r])
		{
			g.i[r] = 0;
			g.f[r] = 0;
		}
	}
	col = (SiftColIn) {.kind = strcmp(kind, "f8") == 0 ? SIFT_CELL_F8 : g.s[0] != NULL ? SIFT_CELL_STR : SIFT_CELL_INT,.i = g.i,.f = g.f,.s = (const char *const *) g.s,.slen = g.slen,
	.isnull = g.isnull,.tag = tags ? g.tag : NULL,
	.hint = strcmp(kind, "money") == 0 ? SIFT_CHINT_NUM64 : strcmp(kind, "bool") == 0 ? SIFT_CHINT_BOOL : strcmp(kind, "list") == 0 ? SIFT_CHINT_LIST : strcmp(kind, "view") == 0 ? SIFT_CHINT_VIEW :
	strcmp(kind, "uuid") == 0 ? SIFT_CHINT_UUID : strcmp(kind, "num128") == 0 ? SIFT_CHINT_NUM128 : strcmp(kind, "interval") == 0 ? SIFT_CHINT_INTERVAL : SIFT_CHINT_AUTO,.scale = 2};
	in.tid = tid;
	in.cols = &col;
	in.all_frozen = true;
	in.min_xid = in.max_xid = sift_xid_to_fxid(FrozenTransactionId);
	initStringInfo(&out);
	desc = sift_granule_encode(&out, &in);
	raw = palloc(out.len);
	memcpy(raw, out.data, out.len);
	v = sift_granule_open(NULL, raw, desc.data_len, &desc);
	sift_chunk_open(&v, 1, &c);

	if (col.kind == SIFT_CELL_INT)
	{
		int64	   *o = palloc(n * sizeof(int64));
		bool	   *on = palloc(n);

		sift_granule_ints(&v, 0, o, on);
		for (int r = 0; r < n; r++)
			legacy &= on[r] == g.isnull[r] && (on[r] || o[r] == g.i[r]);
	}
	else if (col.kind == SIFT_CELL_F8)
	{
		double	   *o = palloc(n * sizeof(double));
		bool	   *on = palloc(n);

		sift_granule_f8(&v, 0, o, on);
		for (int r = 0; r < n; r++)
			legacy &= on[r] == g.isnull[r] && (on[r] || memcmp(&o[r], &g.f[r], 8) == 0);
	}
	else
	{
		SiftStrCell *o = palloc(n * sizeof(SiftStrCell));

		sift_granule_strs(&v, 0, o);
		for (int r = 0; r < n; r++)
			legacy &= o[r].isnull == g.isnull[r] && (o[r].isnull || (o[r].len == g.slen[r] && memcmp(o[r].p, g.s[r], g.slen[r]) == 0));
	}
	{
		uint8	   *t = palloc(n);

		sift_granule_tags(&v, 0, t);
		for (int r = 0; r < n; r++)
			tags_ok &= t[r] == (tags ? g.tag[r] : 0);
	}
	if (col.kind == SIFT_CELL_INT && strcmp(kind, "list") != 0 && strcmp(kind, "money") != 0 && strcmp(kind, "bool") != 0)
	{
		narrow = true;
		for (int r = 0; r < n; r++)
			narrow &= g.i[r] == (int64) (int32) g.i[r];
	}
	if (strcmp(kind, "list") != 0)
	{
		SiftVecType types[2] = {vec_type(kind, narrow), SVEC_DICT32};
		int			ntypes = c.enc == SIFT_CENC_DICT_CODE ? 2 : 1;

		for (int t = 0; t < ntypes; t++)
		{
			char	   *buf = palloc(SIFT_VEC_LEN * 16 + SIFT_VEC_PAD + 64);
			SiftVec		dst = {.type = types[t],.data = (void *) TYPEALIGN(64, buf)};
			uint64	   *valid = (uint64 *) TYPEALIGN(64, palloc(SIFT_VEC_WORDS * 8 + 64));
			const SiftVariantExcStream *exc = NULL;

			dst.valid = valid;
			for (int from = 0; from < n; from += SIFT_VEC_LEN)
			{
				int			cnt = Min(SIFT_VEC_LEN, n - from);

				sift_granule_decode_vec(&v, 0, from, cnt, &dst, &exc);
				vec_ok &= (exc != NULL) == (tags && c.ref->flags & SIFT_CHUNK_HAS_OVERFLOW);
				for (int r = 0; r < cnt; r++)
				{
					if (types[t] == SVEC_DICT32)
					{
						const SiftGranuleDict *d = dst.dict;
						uint32		code = ((const uint32 *) dst.data)[r];
						bool		valid_r = dst.all_valid || ((dst.valid[r >> 6] >> (r & 63)) & 1);

						vec_ok &= valid_r == !g.isnull[from + r] && (!valid_r || (d->off[code + 1] - d->off[code] == g.slen[from + r] &&
																		   memcmp(d->bytes + d->off[code], g.s[from + r], g.slen[from + r]) == 0));
					}
					else
						vec_ok &= vec_row_ok(&g, kind, &dst, r, from + r);
				}
				if (strcmp(kind, "money") == 0)
					vec_ok &= dst.scale == 2;
			}
		}
	}
	else
	{
		SiftArrayLayout lay;
		uint32		total = 0;

		vec_ok = sift_granule_array_layout(&v, 0, &lay) && lay.n_parents == (uint32) n;
		for (int r = 0; vec_ok && r < n; r++)
		{
			vec_ok &= lay.offsets[r] == total && lay.lengths[r] == (g.isnull[r] ? 0 : (uint32) g.i[r]);
			total += lay.lengths[r];
			vec_ok &= r % 64 != 0 || lay.block_elem_off[r / 64] == lay.offsets[r];
		}
		vec_ok &= lay.n_elems == total && lay.block_elem_off[(n + 63) / 64] == total;
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("enc=%s bytes=%u wrapped=%s legacy=%s vec=%s tags=%s", enc_label(c.ref->enc), c.ref->len, (c.ref->flags & SIFT_CHUNK_COMPRESSED) ? "t" : "f",
											  legacy ? "ok" : "BAD", vec_ok ? "ok" : "BAD", tags_ok ? "ok" : "BAD")));
}

PG_FUNCTION_INFO_V1(sift_test_f2_sparse);
PG_FUNCTION_INFO_V1(sift_test_f2_key);

/* n distinct rows of [0, rows) in a fixed pseudo-random order */
static int *
shuffled_rows(int rows, uint64 seed)
{
	/* check-memory: allow a test builds one granule of at most 8192 rows */
	int		   *o = palloc(Max(rows, 1) * sizeof(int));

	for (int i = 0; i < rows; i++)
		o[i] = i;
	for (int i = rows - 1; i > 0; i--)
	{
		int			j = (int) (rnd(&seed) % (uint64) (i + 1)),
					t = o[i];

		o[i] = o[j];
		o[j] = t;
	}
	return o;
}

/* The one granule of a column of a kind of cell that went in, written and opened */
static SiftGranuleView
one_granule(const SiftColIn *col, int n, SiftGranuleDesc *desc)
{
	SiftGranuleIn in = {.nrows = n,.ncols = 1,.tier = SIFT_TIER_WARM_L1,.cols = col,.all_frozen = true};
	uint64	   *tid = palloc(n * sizeof(uint64));
	StringInfoData out;
	char	   *raw;

	for (int r = 0; r < n; r++)
		tid[r] = ((uint64) (r / 100) << 16) | (uint64) (r % 100 + 1);
	in.tid = tid;
	in.min_xid = in.max_xid = sift_xid_to_fxid(FrozenTransactionId);
	initStringInfo(&out);
	*desc = sift_granule_encode(&out, &in);
	raw = palloc(out.len);
	memcpy(raw, out.data, out.len);
	return sift_granule_open(NULL, raw, desc->data_len, desc);
}

static const char *
validity_label(const SiftChunkRef *ref)
{
	return (ref->flags & SIFT_CHUNK_ALL_NULL) ? "ALL_NULL" : (ref->flags & SIFT_CHUNK_ALL_VALID) ? "ALL_VALID" : (ref->enc & SIFT_ENC_NONNULL_SPARSE) ? "NONNULL_SPARSE" :
		(ref->enc & SIFT_ENC_NULL_SPARSE) ? "NULL_SPARSE" : "BITMAP";
}

/*
 * (nrows, nvalid): an integer column that holds a value on nvalid rows at random places, as one granule: the validity the writer chose and its bytes, the values read back, the rows
 * with a value read without the values (and the cap + 1 the reader answers when there are more than the cap), and the vector decoder
 */
Datum
sift_test_f2_sparse(PG_FUNCTION_ARGS)
{
	int			n = PG_GETARG_INT32(0),
				nvalid = PG_GETARG_INT32(1);
	int		   *order;
	int64	   *vals = palloc0(n * sizeof(int64)),
			   *o = palloc(n * sizeof(int64));
	bool	   *isnull = palloc(n),
			   *on = palloc(n);
	uint16	   *rows = palloc((n + 1) * sizeof(uint16));
	SiftColIn	col = {.kind = SIFT_CELL_INT,.i = vals,.isnull = isnull};
	SiftGranuleDesc desc;
	SiftGranuleView v;
	const SiftChunkRef *ref;
	bool		ints_ok = true,
				rows_ok = true,
				cap_ok = true,
				vec_ok = true;
	uint16		cnt;
	char	   *buf = palloc(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD + 64);
	SiftVec		dst = {.type = SVEC_I64,.data = (void *) TYPEALIGN(64, buf)};

	if (n < 1 || n > SIFT_GRANULE_ROWS || nvalid < 0 || nvalid > n)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rows must be 1 to %d and values at most the rows", SIFT_GRANULE_ROWS)));
	order = shuffled_rows(n, 99);
	memset(isnull, true, n);
	for (int i = 0; i < nvalid; i++)
	{
		isnull[order[i]] = false;
		vals[order[i]] = 1000000 + order[i];
	}
	v = one_granule(&col, n, &desc);
	ref = &v.hdr->chunks[1];
	sift_granule_ints(&v, 0, o, on);
	for (int r = 0; r < n; r++)
		ints_ok &= on[r] == isnull[r] && (on[r] || o[r] == vals[r]);
	cnt = sift_granule_nonnull_rows(&v, 0, rows, n);
	rows_ok = cnt == nvalid;
	for (int i = 0; rows_ok && i < cnt; i++)
		rows_ok &= !isnull[rows[i]] && (i == 0 || rows[i] > rows[i - 1]);
	if (nvalid > 0)
		cap_ok = sift_granule_nonnull_rows(&v, 0, rows, (uint16) (nvalid - 1)) == nvalid;
	dst.valid = (uint64 *) TYPEALIGN(64, palloc(SIFT_VEC_WORDS * 8 + 64));
	for (int from = 0; from < n; from += SIFT_VEC_LEN)
	{
		int			c = Min(SIFT_VEC_LEN, n - from);
		const SiftVariantExcStream *exc = NULL;

		sift_granule_decode_vec(&v, 0, from, c, &dst, &exc);
		for (int r = 0; r < c; r++)
			vec_ok &= (dst.all_valid || ((dst.valid[r >> 6] >> (r & 63)) & 1)) == !isnull[from + r] && (isnull[from + r] || ((const int64 *) dst.data)[r] == vals[from + r]);
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("validity=%s validity_bytes=%u chunk_bytes=%u nonnull=%u ints=%s rows=%s cap=%s vec=%s", validity_label(ref), sift_chunk_validity_len(ref, n), ref->len,
											  ref->nnonnull, ints_ok ? "ok" : "BAD", rows_ok ? "ok" : "BAD", cap_ok ? "ok" : "BAD", vec_ok ? "ok" : "BAD")));
}

static const char *
key_label(const SiftChunkRef *ref, const SiftKeyPresentHdr *h)
{
	if (ref->enc & SIFT_ENC_KEY_ALL_ABSENT)
		return "ALL_ABSENT";
	if (ref->enc & SIFT_ENC_KEY_ALL_PRESENT)
		return "ALL_PRESENT";
	if (ref->enc & SIFT_ENC_KEY_EQ_VALID)
		return "EQ_VALID";
	if (!(ref->enc & SIFT_ENC_KEY_HAS_MASK))
		return "NONE";
	return h->mode == SIFT_KP_ARRAY16 ? "MASK_ARRAY16" : h->mode == SIFT_KP_NEG_ARRAY16 ? "MASK_NEG_ARRAY16" : "MASK_BITMAP";
}

/*
 * (nrows, nvalid, njnull, noverflow, nrootkey, presence_only): the leaf of a key whose rows hold a value (nvalid), a JSON null, a value of another type (overflow) or are a document that holds the key
 * as an array element or a string (rootkey), at random places, the others missing the key; written as one granule: how the writer keeps which rows have the key, its bytes, and the rows read
 * back by sift_granule_decode_key_present in slices of 2000 rows, against the rows that went in. presence_only: the leaf holds no values (SIFT_CHINT_KEY_PRESENT)
 */
Datum
sift_test_f2_key(PG_FUNCTION_ARGS)
{
	int			n = PG_GETARG_INT32(0),
				nvalid = PG_GETARG_INT32(1),
				njnull = PG_GETARG_INT32(2),
				novf = PG_GETARG_INT32(3),
				nroot = PG_GETARG_INT32(4);
	bool		presence_only = PG_GETARG_BOOL(5);
	int		   *order;
	int64	   *vals = palloc0(n * sizeof(int64));
	bool	   *isnull = palloc(n),
			   *present = palloc0(n);
	uint8	   *tag = palloc0(n);
	SiftColIn	col = {.kind = SIFT_CELL_INT,.i = vals,.isnull = isnull,.tag = tag,.key_leaf = true,.hint = presence_only ? SIFT_CHINT_KEY_PRESENT : SIFT_CHINT_AUTO};
	SiftGranuleDesc desc;
	SiftGranuleView v;
	const SiftChunkRef *ref;
	SiftChunk	c;
	bool		decode_ok = true,
				tags_ok = true;
	int			npresent = 0,
				states[3] = {0, 0, 0};
	const char *label;

	if (n < 1 || n > SIFT_GRANULE_ROWS || nvalid < 0 || njnull < 0 || novf < 0 || nroot < 0 || nvalid + njnull + novf + nroot > n)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rows must be 1 to %d and the rows that have the key at most the rows", SIFT_GRANULE_ROWS)));
	order = shuffled_rows(n, 7);
	memset(isnull, true, n);
	for (int i = 0, k = 0; k < nvalid + njnull + novf + nroot; i++, k++)
	{
		int			r = order[i];

		present[r] = true;
		if (k < nvalid)
		{
			isnull[r] = false;
			vals[r] = 500 + r;
		}
		else
			tag[r] = k < nvalid + njnull ? SIFT_COLTAG_JNULL : k < nvalid + njnull + novf ? SIFT_COLTAG_STRING : SIFT_COLTAG_ROOTKEY;
	}
	if (presence_only)
	{
		memset(tag, 0, n);
		col.tag = NULL;
		for (int r = 0; r < n; r++)
		{
			isnull[r] = !present[r];
			vals[r] = 0;
		}
	}
	v = one_granule(&col, n, &desc);
	ref = &v.hdr->chunks[1];
	sift_chunk_open(&v, 1, &c);
	label = key_label(ref, c.keyp);
	for (int r = 0; r < n; r++)
		npresent += present[r];
	for (int from = 0; from < n; from += 2000)
	{
		uint64		mask[SIFT_VEC_WORDS];
		int			cnt = Min(2000, n - from);
		int			exp = 0;
		SiftMaskState st = sift_granule_decode_key_present(&v, 0, from, cnt, mask);

		for (int r = 0; r < cnt; r++)
			exp += present[from + r];
		states[st]++;
		decode_ok &= (st == SIFT_MASK_ALL_ZERO) == (exp == 0) && (st == SIFT_MASK_ALL_ONE) == (exp == cnt);
		for (int r = 0; st == SIFT_MASK_BITS && r < cnt; r++)
			decode_ok &= (int) ((mask[r >> 6] >> (r & 63)) & 1) == (int) present[from + r];
	}
	if (!presence_only)
	{
		uint8	   *t = palloc(n);

		sift_granule_tags(&v, 0, t);
		for (int r = 0; r < n; r++)
			tags_ok &= t[r] == tag[r];
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("key=%s enc=%s chunk_bytes=%u key_bytes=%u present=%d decode=%s tags=%s slices(zero,one,bits)=%d,%d,%d", label, enc_label(ref->enc), ref->len,
											  c.keyp != NULL ? (uint32) (c.body - (const char *) c.keyp) : 0, npresent, decode_ok ? "ok" : "BAD", tags_ok ? "ok" : "BAD", states[SIFT_MASK_ALL_ZERO],
											  states[SIFT_MASK_ALL_ONE], states[SIFT_MASK_BITS])));
}

PG_FUNCTION_INFO_V1(sift_test_f2_fsst);

/* (pattern, nstrings): the strings of a pattern through the symbol table: coded, decoded alone and in a run, the table stored and read back, a string cut inside its escape refused */
Datum
sift_test_f2_fsst(PG_FUNCTION_ARGS)
{
	char	   *pat = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			n = PG_GETARG_INT32(1);
	uint64		rs = 777,
				raw = 0;
	char	  **p = palloc(Max(n, 1) * sizeof(char *));
	uint32	   *len = palloc(Max(n, 1) * sizeof(uint32)),
			   *off = palloc((Max(n, 1) + 1) * sizeof(uint32)),
			   *outoff = palloc((Max(n, 1) + 1) * sizeof(uint32)),
				coded = 0;
	char	   *buf = palloc(8192);
	SiftFsst   *t = palloc(sizeof(SiftFsst)),
			   *t2 = palloc(sizeof(SiftFsst));
	uint8	   *enc,
			   *out,
			   *one;
	StringInfoData tbl,
				cat;
	bool		alone = true,
				run = true,
				stored = true;
	uint32		permille;
	const char *vs_lz4 = "n/a";
	bool		detail = PG_NARGS() > 2 && PG_GETARG_BOOL(2);
	Size		zbytes[3] = {0, 0, 0};

	if (n < 1 || n > 100000)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("strings must be 1 to 100000")));
	initStringInfo(&cat);
	for (int r = 0; r < n; r++)
	{
		len[r] = fsst_gen(pat, r, &rs, buf);
		p[r] = memcpy(palloc(len[r] + 1), buf, len[r]);
		raw += len[r];
		appendBinaryStringInfo(&cat, p[r], len[r]);
	}
	permille = sift_fsst_train(t, (const char *const *) p, len, n);
	enc = palloc(2 * raw + 2);
	one = palloc(8 * 2 * 8192 + SIFT_FSST_SLACK);
	off[0] = 0;
	for (int r = 0; r < n; r++)
	{
		uint32		l = sift_fsst_encode(t, p[r], len[r], enc + off[r]);

		off[r + 1] = off[r] + l;
		coded += l;
		alone &= sift_fsst_decode(t, enc + off[r], l, one) == len[r] && memcmp(one, p[r], len[r]) == 0;
	}
	enc[coded] = 0;
	out = palloc(raw + SIFT_FSST_SLACK);
	run = sift_fsst_decode_strs(t, enc, off, n, out, raw + SIFT_FSST_SLACK, outoff);
	for (int r = 0; run && r < n; r++)
		run &= outoff[r + 1] - outoff[r] == len[r] && memcmp(out + outoff[r], p[r], len[r]) == 0;
	initStringInfo(&tbl);
	sift_fsst_put_table(&tbl, t);
	stored = tbl.len % 8 == 0 && sift_fsst_read_table(t2, (const uint8 *) tbl.data, tbl.len) == (uint32) tbl.len && t2->nsym == t->nsym;
	for (int r = 0; stored && r < n; r++)
	{
		uint8	   *again = palloc(2 * len[r] + 1);

		stored &= sift_fsst_encode(t2, p[r], len[r], again) == off[r + 1] - off[r] && memcmp(again, enc + off[r], off[r + 1] - off[r]) == 0;
		pfree(again);
	}
	for (int a = SIFT_CC_ALG_LZ4; a <= SIFT_CC_ALG_ZSTD && raw > 0; a++)
		if (sift_colz_available(a))
		{
			Size		cap = sift_colz_bound(a, raw);
			char	   *z = palloc(cap);

			zbytes[a] = sift_colz_compress(a, 0, cat.data, raw, z, cap);
			pfree(z);
		}
	if (zbytes[SIFT_CC_ALG_LZ4] > 0)
		vs_lz4 = coded < zbytes[SIFT_CC_ALG_LZ4] ? "fsst" : "lz4";
	{
		uint8		cut[2] = {SIFT_FSST_ESCAPE, 0};
		uint32		cutoff[2] = {0, 1},
					o2[2];
		uint8		o[32];

		stored &= !sift_fsst_decode_strs(t, cut, cutoff, 1, o, sizeof(o), o2) && sift_fsst_decode(t, cut, 1, o) == UINT32_MAX;
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("alone=%s run=%s stored=%s symbols=%u smaller_than_raw=%s smaller_than_lz4=%s%s", alone ? "ok" : "BAD", run ? "ok" : "BAD",
											  stored ? "ok" : "BAD", t->nsym, coded < raw ? "t" : "f", vs_lz4,
											  detail ? psprintf(" raw=%llu coded=%u lz4=%zu zstd=%zu sample_permille=%u", (unsigned long long) raw, coded, zbytes[SIFT_CC_ALG_LZ4], zbytes[SIFT_CC_ALG_ZSTD], permille) : "")));
}

#endif							/* SIFT_TESTING */
