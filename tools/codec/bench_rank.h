/*
 * bench_rank.h - the ranked-search and facet benchmarks of bench.c: norms, bounds, brute force, the best-first simulation, rank_bench and facet_bench.
 * Included once, by bench.c, where the text stood.
 */
#ifndef SIFT_CODEC_BENCH_RANK_H
#define SIFT_CODEC_BENCH_RANK_H

static uint32_t *rowstart;
static double avgdl;

static void norms_init(void)
{
	uint32_t npages = SIFT_BLK(C.tid[C.nd - 1]) + 1;
	uint32_t *dl = calloc(C.nd, 4), *maxoff = calloc(npages, 4);
	for (uint64_t i = 0; i < C.np; i++) dl[C.doc[i]] += C.tf[i];
	double sum = 0;
	for (uint64_t d = 0; d < C.nd; d++) sum += dl[d];
	avgdl = sum / C.nd;
	for (uint64_t d = 0; d < C.nd; d++) {
		uint32_t b = SIFT_BLK(C.tid[d]);
		if (SIFT_OFF(C.tid[d]) > maxoff[b]) maxoff[b] = SIFT_OFF(C.tid[d]);
	}
	rowstart = malloc(npages * 4);
	uint64_t slots = 0;
	for (uint32_t p = 0; p < npages; p++) rowstart[p] = (uint32_t)slots, slots += maxoff[p];
	knorm_slot = calloc(slots, 4), knorm_doc = malloc(C.nd * 4);
	for (uint64_t d = 0; d < C.nd; d++) {
		knorm_doc[d] = K1 * (1 - BB + BB * (float)dl[d] / (float)avgdl);
		knorm_slot[rowstart[SIFT_BLK(C.tid[d])] + SIFT_OFF(C.tid[d]) - 1] = knorm_doc[d];
	}
	free(dl), free(maxoff);
}

static float idf(uint32_t df) { return logf(1.0f + ((float)C.nd - df + 0.5f) / (df + 0.5f)); }

/* per-posting bound: ceil(255 * tf / (tf + K(dl))) with the stored (nibble) tf */
static void term_ub(uint32_t t, uint8_t *ub)
{
	for (uint64_t i = C.start[t]; i < C.start[t + 1]; i++) {
		float tf = (float)sift_tf_quant(C.tf[i]);
		ub[i - C.start[t]] = (uint8_t)ceilf(255.0f * (tf / (tf + knorm_doc[C.doc[i]])));
	}
}

static int cmp_hit(const void *a, const void *b)
{
	const sift_hit *x = a, *y = b;
	return x->score < y->score ? 1 : x->score > y->score ? -1 : (x->tid > y->tid) - (x->tid < y->tid);
}

/* candidate docs and float scores (nibble tf, the codec's exact op order) or double exact-tf scores */
static uint32_t brute(const rquery *q, float *sc, uint8_t *cnt, uint32_t *cand, int exact, double *dsc)
{
	uint32_t n = 0;
	for (int i = 0; i < q->nt; i++) {
		uint32_t t = q->t[i];
		float w = idf(C.df[t]) * (K1 + 1);
		for (uint64_t j = C.start[t]; j < C.start[t + 1]; j++) {
			uint32_t d = C.doc[j];
			if (!cnt[d]) cand[n++] = d;
			cnt[d]++;
			if (exact) {
				double tf = C.tf[j];
				dsc[d] += (double)w * tf / (tf + (double)knorm_doc[d]);
			} else {
				float tf = (float)sift_tf_quant(C.tf[j]);
				sc[d] += w * (tf / (tf + knorm_doc[d]));
			}
		}
	}
	uint32_t m = 0;
	for (uint32_t i = 0; i < n; i++)
		if (!q->conj || cnt[cand[i]] == q->nt) cand[m++] = cand[i];
	return m;
}

static float *ubm;
static int cmp_ubm(const void *a, const void *b)
{
	float x = ubm[*(const uint32_t *)a], y = ubm[*(const uint32_t *)b];
	return x < y ? 1 : x > y ? -1 : 0;
}

/* offline: units (G pages) visited by DAAT-with-bound vs best-first (BMP) order, k = 10 */
static void simulate(const rquery *q, const uint32_t *cand, uint32_t n, const float *sc, uint32_t G, double *visit, double *scored)
{
	uint32_t nu = (SIFT_BLK(C.tid[C.nd - 1]) + G) / G;
	static float *ubu;
	static uint8_t *ubt;
	static uint32_t *us, *ue;
	uint32_t mx = SIFT_BLK(C.tid[C.nd - 1]) + 2;
	if (!ubu) ubu = malloc(mx * 4), ubt = malloc(mx), us = malloc(mx * 4), ue = malloc(mx * 4), ubm = malloc(mx * 4);
	memset(ubu, 0, nu * 4);
	for (int i = 0; i < q->nt; i++) {
		uint32_t t = q->t[i];
		float w = idf(C.df[t]) * (K1 + 1);
		memset(ubt, 0, nu);
		for (uint64_t j = C.start[t]; j < C.start[t + 1]; j++) {
			float tf = (float)sift_tf_quant(C.tf[j]);
			uint8_t u = (uint8_t)ceilf(255.0f * (tf / (tf + knorm_doc[C.doc[j]])));
			uint32_t x = SIFT_BLK(C.tid[C.doc[j]]) / G;
			if (u > ubt[x]) ubt[x] = u;
		}
		for (uint32_t x = 0; x < nu; x++)
			if (ubt[x]) ubu[x] += w * ubt[x] / 255.0f;
	}
	/* units holding candidates, in heap order: [us, ue) ranges of cand */
	uint32_t m = 0;
	for (uint32_t i = 0; i < n;) {
		uint32_t x = SIFT_BLK(C.tid[cand[i]]) / G, j = i;
		while (j < n && SIFT_BLK(C.tid[cand[j]]) / G == x) j++;
		ubm[m] = ubu[x], us[m] = i, ue[m] = j, m++;
		i = j;
	}
	for (int order = 0; order < 2; order++) {
		static uint32_t *idx;
		if (!idx) idx = malloc(mx * 4);
		for (uint32_t u = 0; u < m; u++) idx[u] = u;
		if (order) qsort(idx, m, 4, cmp_ubm);  /* best first */
		float top[10];
		int nh = 0;
		uint32_t vis = 0;
		uint64_t sd = 0;
		for (uint32_t a = 0; a < m; a++) {
			uint32_t u = idx[a];
			float b = ubm[u];
			if (nh == 10 && b * 1.0001f < top[0]) {
				if (order) break;
				continue;
			}
			vis++, sd += ue[u] - us[u];
			for (uint32_t i = us[u]; i < ue[u]; i++) {
				float s = sc[cand[i]];
				if (nh < 10) {
					int y = nh++;
					for (; y && top[y - 1] > s; y--) top[y] = top[y - 1];
					top[y] = s;
				} else if (s > top[0]) {
					int y = 0;
					for (; y + 1 < 10 && top[y + 1] < s; y++) top[y] = top[y + 1];
					top[y] = s;
				}
			}
		}
		visit[order] += m ? (double)vis / m : 0, scored[order] += n ? (double)sd / n : 0;
	}
}

static void rank_bench(double secs)
{
	norms_init();
	static rquery rq[NF][NRQ];
	const uint32_t H = 65536, M = 1024;
	for (int p = 0; p < NRQ; p++) {
		rq[F_OR2][p] = (rquery){ 2, 0, { pick(H, ~0u), pick(M, H) } };
		rq[F_OR3][p] = (rquery){ 3, 0, { pick(H, ~0u), pick(M, H), pick(M, H) } };
		rq[F_AND2][p] = (rquery){ 2, 1, { pick(H, ~0u), pick(M, H) } };
		rq[F_AND3][p] = (rquery){ 3, 1, { pick(H, ~0u), pick(M, H), pick(M, H) } };
		rq[F_ORHHH][p] = (rquery){ 3, 0, { pick(H, ~0u), pick(H, ~0u), pick(H, ~0u) } };
	}
	sift_norms nm = { rowstart, knorm_slot, 0 };
	uint8_t *ub = malloc(C.nd), *buf = aligned_alloc(64, SIFT_TOPK_BUF);
	float *sc = calloc(C.nd, 4);
	double *dsc = calloc(C.nd, 8);
	uint8_t *cnt = calloc(C.nd, 1);
	uint32_t *cand = malloc(C.nd * 4);
	static const sift_enc_opts eo[2] = { { 0, 0, 0, 1 }, { SIFT_GROUPED, 0, 0, 1 } };
	printf("[%s] top-10 BM25 (k1=%.1f b=%.2f avgdl=%.1f), %d queries per family, avx512 timings best-of-5\n", C.scen, K1, BB, avgdl, NRQ);
	printf("%-11s %-7s %9s %9s %7s %8s %8s %8s %7s\n", "family", "rep", "ms/q", "exh ms/q", "speedup", "grp%vis", "post%scr", "cand", "rec@10");
	for (int f = 0; f < NF; f++) {
		static const uint32_t SG[4] = { 256, 64, 16, 4 };
		double sim_v[4][2] = { { 0 } }, sim_s[4][2] = { { 0 } }, recall = 0, ncand = 0;
		/* reference scores, recall of nibble tf against exact tf, offline simulation */
		for (int p = 0; p < NRQ && !op_filter; p++) {
			rquery *q = &rq[f][p];
			uint32_t n = brute(q, sc, cnt, cand, 0, NULL);
			ncand += n;
			static sift_hit h1[1 << 22];
			uint32_t m = n < (1 << 22) ? n : (1 << 22);
			for (uint32_t i = 0; i < m; i++) h1[i] = (sift_hit){ C.tid[cand[i]], sc[cand[i]] };
			qsort(h1, m, sizeof(sift_hit), cmp_hit);
			memset(cnt, 0, C.nd);
			brute(q, NULL, cnt, cand, 1, dsc);
			static sift_hit h2[1 << 22];
			for (uint32_t i = 0; i < m; i++) h2[i] = (sift_hit){ C.tid[cand[i]], (float)dsc[cand[i]] };
			qsort(h2, m, sizeof(sift_hit), cmp_hit);
			uint32_t kk = m < 10 ? m : 10, hit = 0;
			for (uint32_t i = 0; i < kk; i++)
				for (uint32_t j = 0; j < kk; j++) hit += h1[i].tid == h2[j].tid;
			recall += kk ? (double)hit / kk : 1;
			qsort(cand, n, 4, sift_cmp_u32);
			for (int g = 0; g < 4; g++) simulate(q, cand, n, sc, SG[g], sim_v[g], sim_s[g]);
			memset(sc, 0, C.nd * 4), memset(dsc, 0, C.nd * 8), memset(cnt, 0, C.nd);
		}
		if (!op_filter)
			for (int g = 0; g < 4; g++)
				printf("  sim G=%-3u pages: visited units daat %5.1f%% bmp %5.1f%%, scored docs daat %5.1f%% bmp %5.1f%%\n", SG[g],
				       100 * sim_v[g][0] / NRQ, 100 * sim_v[g][1] / NRQ, 100 * sim_s[g][0] / NRQ, 100 * sim_s[g][1] / NRQ);
		for (int r = 0; r < 2; r++) {
			uint8_t *blob[NRQ][4];
			for (int p = 0; p < NRQ; p++)
				for (int i = 0; i < rq[f][p].nt; i++) {
					uint32_t t = rq[f][p].t[i], n = C.df[t];
					corpus_tids(&C, t, T);
					term_ub(t, ub);
					size_t len = sift_encode(T, C.tf + C.start[t], ub, n, &eo[r], NULL);
					blob[p][i] = aligned_alloc(64, (len + 63) & ~(size_t)63);
					sift_encode(T, C.tf + C.start[t], ub, n, &eo[r], blob[p][i]);
				}
			sift_set_isa(SIFT_ISA_AVX512);
			double tt[2];
			sift_topk_stats st[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
			static sift_hit hits[2][NRQ][10];
			for (int ex = 0; ex < 2; ex++) {
				double best = 1e30;
				for (int trial = 0; trial < 5; trial++) {
					double t0 = now();
					memset(&st[ex], 0, sizeof st[ex]);
					for (int p = 0; p < NRQ; p++) {
						sift_cur c[4];
						sift_qterm qt[4];
						for (int i = 0; i < rq[f][p].nt; i++) {
							cur_blob(&c[i], blob[p][i], cb[i]);
							qt[i] = (sift_qterm){ &c[i], idf(C.df[rq[f][p].t[i]]) * (K1 + 1), 0 };
						}
						sift_topk_q tq = { qt, rq[f][p].nt, rq[f][p].conj, NULL, &nm, 10, ex };
						sift_topk(&tq, hits[ex][p], buf, &st[ex]);
					}
					double d = now() - t0;
					if (d < best) best = d;
					if (best > secs) break;
				}
				tt[ex] = best / NRQ * 1e3;
			}
			int same = !memcmp(hits[0], hits[1], sizeof hits[0]);
			printf("%-11s %-7s %9.3f %9.3f %6.1fx %7.1f%% %7.1f%% %8.0f %7.3f%s\n", f_name[f], r ? "grouped" : "auto", tt[0], tt[1],
			       tt[1] / tt[0], 100.0 * (st[0].groups - st[0].skipped) / st[0].groups, 100.0 * st[0].scored / st[1].scored,
			       ncand / NRQ, recall / NRQ, same ? "" : "  MISMATCH");
			for (int p = 0; p < NRQ; p++)
				for (int i = 0; i < rq[f][p].nt; i++) free(blob[p][i]);
		}
	}
}

/* facets.tsv: blk, off, foreign_id, country_code; counts per value for term queries */
static void facet_bench(const char *dir, double secs)
{
	char path[4096], cc[8];
	norms_init();
	snprintf(path, sizeof path, "%s/facets.tsv", dir);
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); return; }
	uint16_t *col[2] = { malloc(C.nd * 2), malloc(C.nd * 2) };
	char names[1024][8];
	int nv[2] = { 0, 0 };
	unsigned long blk, off, src;
	for (uint64_t d = 0; d < C.nd && fscanf(f, "%lu %lu %lu %7s", &blk, &off, &src, cc) == 4; d++) {
		int v = 0;
		while (v < nv[1] && strcmp(names[v], cc)) v++;
		if (v == nv[1]) snprintf(names[nv[1]++], 8, "%s", cc);
		col[0][d] = (uint16_t)src, col[1][d] = (uint16_t)v;
		if ((int)src + 1 > nv[0]) nv[0] = (int)src + 1;
	}
	fclose(f);
	OUT = malloc(C.nd * 8);
	uint8_t *buf = aligned_alloc(64, SIFT_CUR_BUF * 2);
	static uint8_t vbuf[1024][SIFT_CUR_BUF];
	printf("[%s] facet counts: ms per query (48 term queries, best of 5); values: source %d, country %d\n", C.scen, nv[0], nv[1]);
	printf("%-8s %-7s %10s %10s %10s %10s\n", "facet", "query", "sift-bm", "docvalues", "roar-doc", "postings");
	uint32_t qt[2][48];
	for (int p = 0; p < 48; p++) qt[0][p] = pick(65536, ~0u), qt[1][p] = pick(1024, 65536);
	for (int fc = 0; fc < 2; fc++) {
		/* keyword index: one blob (and one roaring bitmap) per value */
		uint8_t *vb[1024];
		uint32_t *vd = malloc(C.nd * 4);
		sift_tid *vt = malloc(C.nd * 8);
#ifdef HAVE_ROARING
		roaring_bitmap_t *rv[1024];
#endif
		for (int v = 0; v < nv[fc]; v++) {
			uint32_t n = 0;
			for (uint64_t d = 0; d < C.nd; d++)
				if (col[fc][d] == v) vt[n] = C.tid[d], vd[n++] = (uint32_t)d;
			static const sift_enc_opts eo = { SIFT_GROUPED, SIFT_CT_BITMAP, 0, 0 };
			vb[v] = malloc(sift_encode_bound(n));
			sift_encode(vt, NULL, NULL, n, &eo, vb[v]);
#ifdef HAVE_ROARING
			rv[v] = roaring_bitmap_of_ptr(n, vd);
			roaring_bitmap_run_optimize(rv[v]);
#endif
		}
		for (int qc = 0; qc < 2; qc++) {
			uint8_t *qb[48];
			uint64_t post = 0;
#ifdef HAVE_ROARING
			roaring_bitmap_t *rq[48];
#endif
			for (int p = 0; p < 48; p++) {
				uint32_t t = qt[qc][p], n = C.df[t];
				static const sift_enc_opts eo = { SIFT_GROUPED, 0, 0, 0 };
				corpus_tids(&C, t, T);
				qb[p] = malloc(sift_encode_bound(n));
				sift_encode(T, NULL, NULL, n, &eo, qb[p]);
				post += n;
#ifdef HAVE_ROARING
				rq[p] = roaring_bitmap_of_ptr(n, C.doc + C.start[t]);
				roaring_bitmap_run_optimize(rq[p]);
#endif
			}
			double best[3] = { 1e30, 1e30, 1e30 };
			uint64_t chk[3] = { 0 };
			for (int trial = 0; trial < 5; trial++)
				for (int m = 0; m < 3; m++) {
					uint64_t counts[1024] = { 0 };
					double t0 = now();
					for (int p = 0; p < 48; p++) {
						if (m == 0) {
							sift_cur qc_, vc[1024], *vk[1024];
							cur_blob(&qc_, qb[p], buf);
							for (int v = 0; v < nv[fc]; v++) cur_blob(&vc[v], vb[v], vbuf[v]), vk[v] = &vc[v];
							sift_facets(&qc_, vk, nv[fc], counts);
						} else if (m == 1) {
							/* doc values: decode the term, look up each hit's value */
							sift_cur qc_;
							cur_blob(&qc_, qb[p], buf);
							size_t n = sift_tids(&qc_, OUT);
							for (size_t i = 0; i < n; i++)
								counts[col[fc][rowstart[SIFT_BLK(OUT[i])] + SIFT_OFF(OUT[i]) - 1]]++;  /* slot == doc here */
						} else {
#ifdef HAVE_ROARING
							for (int v = 0; v < nv[fc]; v++) counts[v] += roaring_bitmap_and_cardinality(rq[p], rv[v]);
#endif
						}
					}
					double d = now() - t0;
					if (d < best[m]) best[m] = d;
					chk[m] = 0;
					for (int v = 0; v < nv[fc]; v++) chk[m] += counts[v] * (uint64_t)(v + 1);
				}
			printf("%-8s %-7s %10.3f %10.3f %10.3f %10lu%s\n", fc ? "country" : "source", qc ? "mid" : "hi", best[0] / 48 * 1e3,
			       best[1] / 48 * 1e3, best[2] / 48 * 1e3, post / 48,
			       chk[0] == chk[1] && (chk[2] == chk[0] || !chk[2]) ? "" : "  MISMATCH");
			for (int p = 0; p < 48; p++) free(qb[p]);
		}
		for (int v = 0; v < nv[fc]; v++) free(vb[v]);
		free(vd), free(vt);
	}
	(void)secs;
}

#endif
