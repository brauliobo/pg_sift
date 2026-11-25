/*
 * col/granule/framed_chunk_test.c - test-only SQL functions of the framed chunks (SIFT_TESTING builds): a column of generated strings or doubles is encoded as one granule of a part of a sift_table
 * (framed) and as one of a sift_col (not), and what both give back, whole, by ranges of rows and by single rows, is compared with the values that went in.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"

#include "col/granule/int.h"

PG_FUNCTION_INFO_V1(sift_test_f2_framed_chunks);
PG_FUNCTION_INFO_V1(sift_test_f2_frame_point_us);

typedef struct FramedCase
{
	bool		str;
	int			n;
	uint64	   *tid;
	double	   *fv;
	const char **sv;
	uint32	   *sl;
	bool	   *nl;
} FramedCase;

static uint64
rnd(uint64 *s)
{
	*s = *s * UINT64CONST(6364136223846793005) + UINT64CONST(1442695040888963407);
	return *s >> 17;
}

static FramedCase
make_case(bool str, int n, bool nulls, int family, uint64 *rs)
{
	FramedCase	k = {.str = str,.n = n,.tid = palloc(n * sizeof(uint64)),.fv = palloc0(n * sizeof(double)),.sv = palloc0(n * sizeof(char *)),.sl = palloc0(n * sizeof(uint32)),.nl = palloc0(n)};

	for (int r = 0; r < n; r++)
	{
		k.tid[r] = ((uint64) (r / 100) << 16) | (uint64) (r % 100 + 1);
		k.nl[r] = nulls && r % 7 == 3;
		if (k.nl[r])
			continue;
		if (str)
		{
			k.sv[r] = psprintf("row-%lu-%lu-%s", (unsigned long) rnd(rs), (unsigned long) r, family % 2 == 0 ? "a-longer-tail-of-text" : "t");
			k.sl[r] = strlen(k.sv[r]);
		}
		else
		{
			const double x = (double) (rnd(rs) % 1000000007);

			k.fv[r] = family == 0 ? x / 1e9 + sin((double) r) * 1e-3 : family == 1 ? x / 7.0 : family == 2 ? sin((double) r) * 1e6 : 1.0 + x * 1e-12;
		}
	}
	return k;
}

static SiftGranuleView
encode_case(const FramedCase *k, bool framed, SiftCompressTier tier, uint32 *bytes, StringInfo out)
{
	SiftColIn	col = {.kind = k->str ? SIFT_CELL_STR : SIFT_CELL_F8,.f = k->fv,.s = k->sv,.slen = k->sl,.isnull = k->nl};
	SiftGranuleIn in = {.nrows = k->n,.ncols = 1,.tier = tier,.tid = k->tid,.cols = &col,.all_frozen = true,.framed = framed,.compress_alg = SIFT_CC_ALG_ZSTD};
	SiftGranuleDesc desc;
	char	   *raw;

	in.min_xid = in.max_xid = sift_xid_to_fxid(FrozenTransactionId);
	initStringInfo(out);
	desc = sift_granule_encode(out, &in);
	raw = palloc(out->len);
	memcpy(raw, out->data, out->len);
	*bytes = out->len;
	return sift_granule_open(NULL, raw, desc.data_len, &desc);
}

static bool
cell_same(const SiftStrCell *a, const SiftStrCell *b)
{
	return a->isnull == b->isnull && (a->isnull || (a->len == b->len && memcmp(a->p, b->p, a->len) == 0));
}

static int
case_failures(const FramedCase *k, const SiftGranuleView *v)
{
	const int	n = k->n;
	int			failures = 0;
	bool	   *wn = palloc(n);
	double	   *wf = palloc(n * sizeof(double));
	SiftStrCell *ws = palloc(n * sizeof(SiftStrCell));

	if (k->str)
		sift_granule_strs(v, 0, ws);
	else
		sift_granule_f8(v, 0, wf, wn);
	for (int r = 0; r < n; r++)
		failures += k->str ? ws[r].isnull != k->nl[r] || (!k->nl[r] && (ws[r].len != k->sl[r] || memcmp(ws[r].p, k->sv[r], k->sl[r]) != 0))
			: wn[r] != k->nl[r] || (!k->nl[r] && memcmp(&wf[r], &k->fv[r], sizeof(double)) != 0);
	for (int round = 0; round < 12; round++)
	{
		uint16		rows[SIFT_POINT_ROWS_MAX];
		int			m = 0;

		for (int r = round % 4 == 0 ? 0 : (round * 37) % n; r < n && m < SIFT_POINT_ROWS_MAX; r += 1 + (round % 3 == 0 ? 1 : (round * 97) % 1500))
			rows[m++] = (uint16) r;
		if (k->str)
		{
			SiftStrCell *po = palloc(m * sizeof(SiftStrCell));

			sift_granule_point_strs(v, 0, rows, (uint16) m, po);
			for (int j = 0; j < m; j++)
				failures += !cell_same(&po[j], &ws[rows[j]]);
			for (int j = 0; j < m && j < 4; j++)
			{
				SiftStrCell one = {.isnull = !sift_granule_point_str(v, 0, rows[j], &po[j].p, &po[j].len)};

				one.p = po[j].p;
				one.len = po[j].len;
				failures += !cell_same(&one, &ws[rows[j]]);
			}
		}
		else
		{
			double	   *po = palloc(m * sizeof(double));
			bool	   *pn = palloc(m);

			sift_granule_point_f8(v, 0, rows, (uint16) m, po, pn);
			for (int j = 0; j < m; j++)
				failures += pn[j] != wn[rows[j]] || (!pn[j] && memcmp(&po[j], &wf[rows[j]], sizeof(double)) != 0);
		}
	}
	return failures;
}

/* (kind, nrows, seed): "framed unframed_bytes framed_bytes failures": whether the chunk came out framed, the size of the granule both ways, and the values that differ from those that went in */
Datum
sift_test_f2_framed_chunks(PG_FUNCTION_ARGS)
{
	const bool	str = PG_GETARG_BOOL(0);
	const int	n = PG_GETARG_INT32(1);
	const int	seed = PG_GETARG_INT32(2);
	uint64		rs = (uint64) seed;
	FramedCase	k;
	StringInfoData a,
				b;
	uint32		abytes,
				bbytes;
	int			failures = 0;
	SiftGranuleView plain,
				framed;
	int			is_framed = 0;

	if (n < 1 || n > SIFT_GRANULE_ROWS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rows must be 1 to %d", SIFT_GRANULE_ROWS)));
	for (int nulls = 0; nulls < 2; nulls++)
	{
		k = make_case(str, n, nulls, seed % 4, &rs);
		plain = encode_case(&k, false, SIFT_TIER_COLD_L2, &abytes, &a);
		framed = encode_case(&k, true, SIFT_TIER_COLD_L2, &bbytes, &b);
		failures += case_failures(&k, &plain) + case_failures(&k, &framed);
		is_framed += (framed.hdr->chunks[1].enc & SIFT_ENC_FRAMED) != 0;
		failures += (framed.hdr->chunks[1].enc & (SIFT_ENC_FRAMED | SIFT_ENC_MASK)) == (str ? SIFT_CENC_STR_BLOCK : SIFT_CENC_FLOAT_BSS);
		failures += (plain.hdr->chunks[1].enc & SIFT_ENC_FRAMED) != 0;
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d %u %u %d", is_framed, abytes, bbytes, failures)));
}

/* (kind, nrows): the microseconds of the decode of the whole column and of one row of it, as a framed chunk and as an unframed one, each the best of several runs */
Datum
sift_test_f2_frame_point_us(PG_FUNCTION_ARGS)
{
	const bool	str = PG_GETARG_BOOL(0);
	const int	n = PG_GETARG_INT32(1);
	uint64		rs = 17;
	FramedCase	k = make_case(str, n, false, 1, &rs);
	double		best[2][2];
	StringInfoData out;
	uint32		bytes;

	for (int framed = 0; framed < 2; framed++)
	{
		SiftGranuleView v = encode_case(&k, framed, SIFT_TIER_COLD_L2, &bytes, &out);
		SiftStrCell *ws = palloc(n * sizeof(SiftStrCell));
		double	   *wf = palloc(n * sizeof(double));
		bool	   *wn = palloc(n);

		best[framed][0] = best[framed][1] = 1e18;
		for (int rep = 0; rep < 9; rep++)
		{
			instr_time	t0,
						t1;
			uint16		row = (uint16) ((rep * 7919) % n);
			const char *p;
			uint32		len;
			double		dv;
			bool		dn;

			INSTR_TIME_SET_CURRENT(t0);
			if (str)
				sift_granule_strs(&v, 0, ws);
			else
				sift_granule_f8(&v, 0, wf, wn);
			INSTR_TIME_SET_CURRENT(t1);
			INSTR_TIME_SUBTRACT(t1, t0);
			best[framed][0] = Min(best[framed][0], INSTR_TIME_GET_MICROSEC(t1));
			INSTR_TIME_SET_CURRENT(t0);
			if (str)
				(void) sift_granule_point_str(&v, 0, row, &p, &len);
			else
				sift_granule_point_f8(&v, 0, &row, 1, &dv, &dn);
			INSTR_TIME_SET_CURRENT(t1);
			INSTR_TIME_SUBTRACT(t1, t0);
			best[framed][1] = Min(best[framed][1], INSTR_TIME_GET_MICROSEC(t1));
		}
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("whole %.0f -> %.0f, point %.0f -> %.0f (us, unframed -> framed)", best[0][0], best[1][0], best[0][1], best[1][1])));
}

#endif							/* SIFT_TESTING */
