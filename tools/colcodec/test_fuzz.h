/*
 * test_fuzz.h - the fuzz test of test.c: mutated blobs must not crash the chunk codec.
 * Included once, by test.c, where the text stood.
 */
#ifndef SIFT_COLCODEC_TEST_FUZZ_H
#define SIFT_COLCODEC_TEST_FUZZ_H

typedef struct FuzzArg
{
	uint8	   *blob;
	Size		len;
	int			what;			/* 0 chunk, 1 strings, 2 bits */
	bool		verify;
	uint32		nbits;
	SiftRng		   *r;
} FuzzArg;

static void
fuzz_fn(void *a)
{
	FuzzArg    *f = a;

	if (f->what == 1)
	{
		uint32	   *o;
		char	   *b;
		uint32		n = 0;

		if (f->len >= 8)
			memcpy(&n, f->blob + 4, sizeof(n));
		sift_cc_decode_strs(CurrentMemoryContext, f->blob, f->len, n, &o, &b);
	}
	else if (f->what == 2)
	{
		uint64	   *w = palloc(8 * ((f->nbits + 63) / 64 + 1));

		sift_cc_decode_bits(f->blob, f->len, f->nbits, w);
		pfree(w);
	}
	else
	{
		SiftCc	   *cc = sift_cc_open(CurrentMemoryContext, f->blob, f->len, f->verify);
		uint32		n = sift_cc_nrows(cc);

		if (n <= (1u << 22))
		{
			int64	   *dec = palloc(sizeof(int64) * (n + SIFT_COL_PAD / 8));
			uint32	   *off = palloc(sizeof(uint32) * SIFT_CC_CAP32(n));
			uint64	   *bits = palloc(8 * SIFT_COL_NWORDS(n));
			int64		sum;

			sift_cc_decode(cc, dec);
			sift_cc_decode_off32(cc, off);
			sift_cc_filter(cc, sift_cc_min(cc), sift_cc_min(cc) + 1000, bits);
			sift_cc_sum(cc, bits, &sum);
			for (int k = 0; n && k < 20; k++)
				sift_cc_get(cc, sift_rng_below(f->r, n));
			pfree(dec), pfree(off), pfree(bits);
		}
		sift_cc_close(cc);
	}
}

static void
mutate(SiftRng *r, uint8 *b, Size len)
{
	switch (sift_rng_below(r, 6))
	{
		case 0:
			b[sift_rng_below(r, len)] ^= (uint8) (1 << sift_rng_below(r, 8));
			break;
		case 1:
			for (int k = 1 + sift_rng_below(r, 4); k; k--)
				b[sift_rng_below(r, len)] = (uint8) sift_rng_next(r);
			break;
		case 2:
			{
				Size		at = sift_rng_below(r, len),
							cnt = 1 + sift_rng_below(r, 16);

				memset(b + at, sift_rng_below(r, 2) ? 0 : 0xFF, Min(len - at, cnt));
				break;
			}
		case 3:
			{
				Size		at = sift_rng_below(r, len > 8 ? len - 8 : 1) & ~(Size) 7;
				uint64		x = sift_rng_below(r, 2) ? ~UINT64CONST(0) : sift_rng_below(r, 1 << 16);

				memcpy(b + at, &x, Min((Size) 8, len - at));
				break;
			}
		case 4:
			{
				Size		at = sift_rng_below(r, len);

				b[at] = sift_rng_below(r, 2) ? b[at] + 1 : b[at] - 1;
				break;
			}
		default:
			{
				Size		a = sift_rng_below(r, len),
							c = sift_rng_below(r, len);

				memmove(b + (a & ~(Size) 7), b + (c & ~(Size) 7), Min((Size) 16, len - Max(a & ~(Size) 7, c & ~(Size) 7)));
				break;
			}
	}
}

static void
test_fuzz(SiftRng *r, int scale)
{
	uint64		errs = 0,
				oks = 0;

	for (int it = 0; it < 120; it++)
	{
		Chunk		c;
		SiftCcPolicy pol;
		StringInfoData s;
		int			what = it % 12 == 10 ? 1 : it % 12 == 11 ? 2 : 0;
		uint32		n = sizes[2 + sift_rng_below(r, 17)] % 6000;
		uint32	   *off = NULL;
		char	   *bytes = NULL;
		uint64	   *w = NULL;
		uint8	   *orig;
		Size		olen;

		sift_cc_policy_parse(policies[sift_rng_below(r, NPOL)], &pol);
		c.n = n;
		c.v = xalloc(sizeof(int64) * (n + 1));
		c.nulls = NULL;
		initStringInfo(&s);
		if (what == 0)
		{
			ccg_fill(r, sift_rng_below(r, CCG_NGEN), n, c.v, &c.kind);
			sift_cc_encode(&s, c.kind, c.v, n, NULL, &pol, NULL);
		}
		else if (what == 1)
		{
			off = xalloc(4 * (n + 1));
			bytes = xalloc(n * 8 + 8);
			off[0] = 0;
			for (uint32 i = 0; i < n; i++)
			{
				off[i + 1] = off[i] + sift_rng_below(r, 8);
				memset(bytes + off[i], 'a' + (i % 5), off[i + 1] - off[i]);
			}
			sift_cc_encode_strs(&s, off, bytes, n, &pol, NULL);
		}
		else
		{
			w = xalloc(8 * ((n + 63) / 64 + 1));
			memset(w, 0, 8 * ((n + 63) / 64 + 1));
			for (uint32 i = 0; i < n; i++)
				if (sift_rng_below(r, 40) == 0)
					w[i / 64] |= UINT64CONST(1) << (i % 64);
			sift_cc_encode_bits(&s, w, n, &pol);
		}
		olen = s.len;
		orig = xalloc(olen);
		memcpy(orig, s.data, olen);
		pfree(s.data);
		for (int m = 0; m < 400 * scale; m++)
		{
			FuzzArg		fa = {NULL, olen, what, sift_rng_below(r, 4) == 0, n, r};
			uint8	   *tmp = xalloc(olen);

			memcpy(tmp, orig, olen);
			mutate(r, tmp, olen);
			if (sift_rng_below(r, 8) == 0)
				fa.len = sift_rng_below(r, olen) & ~(Size) 7;
			fa.blob = xalloc(fa.len);
			memcpy(fa.blob, tmp, fa.len);
			free(tmp);
			if (pgshim_try(fuzz_fn, &fa))
				oks++;
			else
			{
				errs++;
				CHECK(pgshim_sqlstate == ERRCODE_DATA_CORRUPTED || pgshim_sqlstate == ERRCODE_FEATURE_NOT_SUPPORTED ||
					  pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED, "fuzz: unexpected error: %s", pgshim_errmsg);	/* a length field past the 1 GB palloc limit */
			}
			free(fa.blob);
			pgshim_reset();
		}
		free(c.v), free(off), free(bytes), free(w), free(orig);
	}
	printf("fuzz: %llu errors raised, %llu damaged blobs decoded\n", (unsigned long long) errs, (unsigned long long) oks);
}

#endif
