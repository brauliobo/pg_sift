/*
 * research_decode.h - the decode-speed measurement of research.c (-s): the kernels and the timing of the chosen encodings.
 * Included once, by research.c, where the text stood; the functions are static, as in research_enc.h and research_float.h.
 */
#ifndef SIFT_COLCODEC_RESEARCH_DECODE_H
#define SIFT_COLCODEC_RESEARCH_DECODE_H

/* ---- decode speed of the chosen encodings ---- */
static void
range_u32(const u32 *v, u32 n, u32 lo, u32 hi, u64 *mask)
{
	u32			span = hi - lo;

	for (u32 w = 0; w < n / 64; w++)
	{
		u64			m = 0;

		for (int j = 0; j < 64; j++)
			m |= (u64) ((v[w * 64 + j] - lo) <= span) << j;
		mask[w] = m;
	}
}

static volatile u64 sink;

static void
speed_int(const Col *c)
{
	u32			nch = (c->n / g_chunk > 48 ? 48 : c->n / g_chunk);
	Buf		   *enc = calloc(nch, sizeof(Buf)),
			   *encz = calloc(nch, sizeof(Buf));
	u64		  **vals = calloc(nch, sizeof(u64 *));
	u32			total = nch * g_chunk;
	u32		   *out32 = malloc(sizeof(u32) * (g_chunk + 1024));
	u64		   *out64 = malloc(sizeof(u64) * (g_chunk + 1024)),
			   *mask = calloc(g_chunk / 64 + 8, 8);
	double		t_enc,
				t_dec,
				t_filt,
				t_z4,
				t_z3,
				t_raw;
	size_t		sz = 0,
				szz = 0;
	int			wide = c->width == 8;
	char		encs[64] = "";
	u8		   *zbuf = malloc(g_chunk * 8 + 1024),
			   *zout = malloc(g_chunk * 8 + 1024);

	for (u32 k = 0; k < nch; k++)
		vals[k] = widen(c, k * g_chunk, g_chunk);
	TIMEIT(t_enc, total, {
		for (u32 k = 0; k < nch; k++)
		{
			Buf			b = enc_int(vals[k], g_chunk, c->width, 2, -1, 0);

			bfree(&b);
		}
	});
	for (u32 k = 0; k < nch; k++)
	{
		enc[k] = enc_int(vals[k], g_chunk, c->width, 2, -1, 0);
		sz += enc[k].n;
	}
	{
		int			cnt[E_N] = {0};

		for (u32 k = 0; k < nch; k++)
			cnt[node_enc(&enc[k])]++;
		for (int e = 0; e < E_N; e++)
			if (cnt[e])
				snprintf(encs + strlen(encs), sizeof encs - strlen(encs), "%s:%d ", enc_name[e], cnt[e]);
	}
	TIMEIT(t_dec, total, {
		for (u32 k = 0; k < nch; k++)
		{
			scr_top = 0;
			if (wide)
				dec_u64(enc[k].p, enc[k].n, g_chunk, out64);
			else
				dec_u32(enc[k].p, enc[k].n, g_chunk, out32);
		}
		sink += wide ? out64[7] : out32[7];
	});
	if (!wide)
	{
		TIMEIT(t_filt, total, {
			for (u32 k = 0; k < nch; k++)
			{
				scr_top = 0;
				dec_u32(enc[k].p, enc[k].n, g_chunk, out32);
				range_u32(out32, g_chunk, 100, 120, mask);
			}
			sink += mask[0];
		});
		TIMEIT(t_raw, total, {
			for (u32 k = 0; k < nch; k++)
			{
				for (u32 i = 0; i < g_chunk; i++)
					out32[i] = (u32) vals[k][i];
				range_u32(out32, g_chunk, 100, 120, mask);
			}
			sink += mask[0];
		});
	}
	else
		t_filt = t_raw = 0;
	/* zstd3 / lz4 layered over the chosen encoding: decompress cost */
	for (u32 k = 0; k < nch; k++)
	{
		size_t		cl = z_comp(&algs[ALG_ZSTD3], enc[k].p, enc[k].n, zbuf, g_chunk * 8 + 1024);

		szz += cl;
		encz[k].p = malloc(cl);
		encz[k].n = cl;
		memcpy(encz[k].p, zbuf, cl);
	}
	TIMEIT(t_z3, total, {
		for (u32 k = 0; k < nch; k++)
			sink += z_decomp(&algs[ALG_ZSTD3], encz[k].p, encz[k].n, zout, enc[k].n);
	});
	{
		size_t		cl = z_comp(&algs[0], enc[0].p, enc[0].n, zbuf, g_chunk * 8 + 1024);

		TIMEIT(t_z4, total, {
			for (u32 k = 0; k < nch; k++)
				sink += z_decomp(&algs[0], zbuf, cl, zout, enc[0].n);
		});
	}
	printf("%-26s enc %6.2f  dec %5.2f  dec+filter %5.2f (raw array+filter %5.2f)  zstd3 unwrap %5.2f  lz4 unwrap %5.2f  ns/value  size %6.1f B/chunk  %s\n",
		   c->name, t_enc, t_dec, t_filt, t_raw, t_z3, t_z4, (double) sz / nch, encs);
	for (u32 k = 0; k < nch; k++)
	{
		bfree(&enc[k]);
		bfree(&encz[k]);
		free(vals[k]);
	}
	free(enc); free(encz); free(vals); free(out32); free(out64); free(mask); free(zbuf); free(zout);
}

static void
speed_float(const Col *c)
{
	int			w = c->width;
	u32			nch = (c->n / g_chunk > 48 ? 48 : c->n / g_chunk),
				total = nch * g_chunk,
				nexc;
	Buf		   *al = calloc(nch, sizeof(Buf)),
			   *go = calloc(nch, sizeof(Buf));
	u8		   *out = malloc((size_t) g_chunk * 8 + 64);
	double		t_alp,
				t_gor,
				t_ealp,
				t_egor;
	size_t		sa = 0,
				sg = 0;

	TIMEIT(t_ealp, total, {
		for (u32 k = 0; k < nch; k++)
		{
			Buf			b = enc_alp((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w, 0, &nexc);

			bfree(&b);
		}
	});
	TIMEIT(t_egor, total, {
		for (u32 k = 0; k < nch; k++)
		{
			Buf			b = enc_gorilla((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w);

			bfree(&b);
		}
	});
	for (u32 k = 0; k < nch; k++)
	{
		al[k] = enc_alp((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w, 0, &nexc);
		go[k] = enc_gorilla((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w);
		sa += al[k].n;
		sg += go[k].n;
	}
	TIMEIT(t_alp, total, {
		for (u32 k = 0; k < nch; k++)
		{
			scr_top = 0;
			dec_alp(al[k].p, g_chunk, w, out);
		}
		sink += out[3];
	});
	TIMEIT(t_gor, total, {
		for (u32 k = 0; k < nch; k++)
			dec_gorilla(go[k].p, g_chunk, w, out);
		sink += out[3];
	});
	/* correctness of the decoders on the measured data */
	for (u32 k = 0; k < nch; k++)
	{
		scr_top = 0;
		dec_alp(al[k].p, g_chunk, w, out);
		if (memcmp(out, (u8 *) c->d + (size_t) k * g_chunk * w, (size_t) g_chunk * w))
			printf("  ALP MISMATCH chunk %u\n", k);
		dec_gorilla(go[k].p, g_chunk, w, out);
		if (memcmp(out, (u8 *) c->d + (size_t) k * g_chunk * w, (size_t) g_chunk * w))
			printf("  GORILLA MISMATCH chunk %u\n", k);
	}
	printf("%-26s alp: enc %6.2f dec %5.2f ns/v %5.2f B/v | gorilla: enc %6.2f dec %5.2f ns/v %5.2f B/v\n", c->name, t_ealp, t_alp,
		   (double) sa / total, t_egor, t_gor, (double) sg / total);
	for (u32 k = 0; k < nch; k++)
	{
		bfree(&al[k]);
		bfree(&go[k]);
	}
	free(al); free(go); free(out);
}

/* compress / decompress speed and ratio of every general-purpose algorithm on representative streams */
static void
speed_z(const Table *ot, const Table *it)
{
	static const char *const names[] = {"orders.description", "items.state_code", "items.category_code", "items.price", "orders.updated_at", "items.order_id"};

	printf("\n=== general-purpose compressors on the chosen lightweight streams (16384-row chunks; MB/s of uncompressed bytes) ===\n");
	for (int w = 0; w < 6; w++)
	{
		const Col  *c = NULL;
		const Table *ts[2] = {ot, it};
		Buf			b = {0},
					bal = {0};
		u32			nch = 0;
		Buf		   *chunks;

		for (int t = 0; t < 2; t++)
			for (int ci = 0; ci < ts[t]->ncol; ci++)
			{
				char		nm[64];

				snprintf(nm, sizeof nm, "%s.%s", ts[t]->name, ts[t]->col[ci].name);
				if (!strcmp(nm, names[w]))
					c = &ts[t]->col[ci];
			}
		if (!c)
			continue;
		nch = c->n / g_chunk > 24 ? 24 : c->n / g_chunk;
		chunks = calloc(nch, sizeof(Buf));
		for (u32 k = 0; k < nch; k++)
		{
			if (c->kind == K_STR)
				bput(&chunks[k], (const char *) c->d + c->off[k * g_chunk], c->off[(k + 1) * g_chunk] - c->off[k * g_chunk]);
			else if (c->kind == K_INT)
			{
				u64		   *v = widen(c, k * g_chunk, g_chunk);
				Buf			e = enc_int(v, g_chunk, c->width, 2, -1, 0);

				bput(&chunks[k], e.p, e.n);
				bfree(&e);
				free(v);
			}
			else
			{
				u32			ne;
				Buf			e = enc_alp((u8 *) c->d + (size_t) k * g_chunk * c->width, g_chunk, c->width, 0, &ne);

				bput(&chunks[k], e.p, e.n);
				bfree(&e);
			}
			b.n += chunks[k].n;
		}
		printf("%-22s %8.0f B/chunk\n", names[w], (double) b.n / nch);
		for (int a = 0; a < 6; a++)
		{
			size_t		csum = 0;
			double		tc,
						td;
			u8		  **cb = calloc(nch, sizeof(u8 *));
			size_t	   *cl = calloc(nch, sizeof(size_t));
			u8		   *out = malloc(1 << 22);

			for (u32 k = 0; k < nch; k++)
			{
				cb[k] = malloc(chunks[k].n + chunks[k].n / 8 + 1024);
				cl[k] = z_comp(&algs[a], chunks[k].p, chunks[k].n, cb[k], chunks[k].n + chunks[k].n / 8 + 1024);
				csum += cl[k];
			}
			TIMEIT(tc, b.n, {
				for (u32 k = 0; k < nch; k++)
					sink += z_comp(&algs[a], chunks[k].p, chunks[k].n, out, 1 << 22);
			});
			TIMEIT(td, b.n, {
				for (u32 k = 0; k < nch; k++)
					sink += z_decomp(&algs[a], cb[k], cl[k], out, chunks[k].n);
			});
			printf("    %-8s ratio %5.2f   compress %7.1f MB/s   decompress %8.1f MB/s\n", algs[a].name, (double) b.n / csum, 1000.0 / tc, 1000.0 / td);
			for (u32 k = 0; k < nch; k++)
				free(cb[k]);
			free(cb); free(cl); free(out);
		}
		for (u32 k = 0; k < nch; k++)
			bfree(&chunks[k]);
		free(chunks);
		(void) bal;
	}
}

#endif
