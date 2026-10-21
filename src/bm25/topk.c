/*
 * bm25/topk.c - fused BM25 scoring of a result group, document-at-a-time top-k, facet counts.
 */
#include <stdlib.h>
#include "bm25/bitmap_kern.h"
#include "bm25/bitmap_int.h"

/* ---- scoring, top-k, facets ---- */

uint32_t sift_grp_score(const sift_grp *res, const sift_qterm *t, int nt, const sift_norms *nm, float *score, sift_tid *tids, uint8_t *buf)
{
	uint32_t n = sift_k->tids(res, NULL, tids);
	float *kn = (float *)buf, *tf = kn + SIFT_GRP_MAXTIDS;
	uint32_t *rank = (uint32_t *)(tf + SIFT_GRP_MAXTIDS), *pos = rank + SIFT_GRP_MAXTIDS;
	uint8_t *gbuf = (uint8_t *)(pos + SIFT_GRP_MAXTIDS);
	for (uint32_t i = 0; i < n; i++) {
		kn[i] = nm->knorm[nm->rowstart[SIFT_BLK(tids[i]) - nm->blk0] + SIFT_OFF(tids[i]) - 1];
		score[i] = 0;
	}
	for (int j = 0; j < nt; j++) {
		sift_cur *c = t[j].cur;
		if (c->state != 1 || c->g.gid != res->gid) continue;
		if (t[j].rel == SIFT_REL_SUPERSET) {
			sift_k->ranks(&c->g, res, rank);
			for (uint32_t i = 0; i < n; i++) tf[i] = (float)sift_tf_at(c, rank[i]);
			sift_k->bm25(score, kn, tf, NULL, n, t[j].w);
			continue;
		}
		/* positions in res of the term's postings that res holds; their tf ranks in the term */
		sift_grp common = c->g;
		uint32_t m;
		if (t[j].rel == SIFT_REL_ANY) {
			sift_k->and2(res, &c->g, NULL, &common, gbuf);
			if (pages_empty(common.pages)) continue;
			m = sift_k->ranks(&c->g, &common, rank);
		} else {
			m = c->g.n ? c->g.n : (uint32_t)sift_grp_count(&c->g);
			for (uint32_t i = 0; i < m; i++) rank[i] = c->g.base + i;
		}
		sift_k->ranks(res, &common, pos);
		for (uint32_t i = 0; i < m; i++) tf[i] = (float)sift_tf_at(c, rank[i]), pos[i] -= res->base;
		sift_k->bm25(score, kn, tf, pos, m, t[j].w);
	}
	return n;
}

static inline int hit_worse(const sift_hit *a, const sift_hit *b) { return a->score < b->score || (a->score == b->score && a->tid > b->tid); }

static void heap_push(sift_hit *h, uint32_t *n, uint32_t k, sift_hit x)
{
	uint32_t i;
	if (*n < k)
		for (i = (*n)++; i && hit_worse(&x, &h[(i - 1) / 2]); i = (i - 1) / 2) h[i] = h[(i - 1) / 2];
	else if (hit_worse(&h[0], &x))
		for (i = 0;;) {
			uint32_t c = 2 * i + 1;
			if (c >= k) break;
			if (c + 1 < k && hit_worse(&h[c + 1], &h[c])) c++;
			if (!hit_worse(&h[c], &x)) break;
			h[i] = h[c], i = c;
		}
	else
		return;
	h[i] = x;
}

static int hit_cmp(const void *a, const void *b) { return hit_worse(a, b) ? 1 : hit_worse(b, a) ? -1 : 0; }

/* next candidate gid >= target for an OR of terms, aligned with the filter */
static int or_align(const sift_topk_q *q, uint32_t *target)
{
	for (;;) {
		uint32_t mg = UINT32_MAX;
		for (int i = 0; i < q->nt; i++) {
			const sift_grp *g = sift_seek(q->t[i].cur, *target);
			if (g && g->gid < mg) mg = g->gid;
		}
		if (mg == UINT32_MAX) return 0;
		const sift_grp *f = q->filter ? sift_seek(q->filter, mg) : NULL;
		if (q->filter && !f) return 0;
		*target = f ? f->gid : mg;
		if (*target == mg) return 1;
	}
}

/* page mask of the 16-page blocks whose summed bound can still reach theta */
static int blk_mask(const sift_topk_q *q, uint32_t gid, float theta, uint64_t m[4])
{
	float ubb[16] = { 0 };
	for (int i = 0; i < q->nt; i++) {
		sift_cur *c = q->t[i].cur;
		if (c->state != 1 || c->g.gid != gid) continue;
		float w = q->t[i].w / 255.0f;
		for (int b = 0; b < 16; b++) ubb[b] += w * sift_blk_ub(c, b);
	}
	memset(m, 0, 32);
	for (int b = 0; b < 16; b++)
		if (ubb[b] * 1.0001f >= theta) m[b / 4] |= 0xffffull << (b % 4 * 16);
	return !pages_empty(m);
}

uint32_t sift_topk(const sift_topk_q *q, sift_hit *hits, uint8_t *buf, sift_topk_stats *st)
{
	float *score = (float *)buf;
	sift_tid *tids = (sift_tid *)(score + SIFT_GRP_MAXTIDS);
	uint8_t *sbuf = (uint8_t *)(tids + SIFT_GRP_MAXTIDS), *gb = sbuf + SIFT_SCORE_BUF;
	sift_cur *cur[65];
	uint32_t nh = 0, nc = 0;
	for (int i = 0; i < q->nt; i++) {
		cur[nc++] = q->t[i].cur;
		q->t[i].rel = q->conj ? SIFT_REL_SUPERSET : q->filter ? SIFT_REL_ANY : SIFT_REL_SUBSET;
	}
	if (q->filter) cur[nc++] = q->filter;
	for (uint32_t target = 0;; target++) {
		uint64_t m[4], bm[4];
		if (q->conj ? (nc > 1 ? !and_align(cur, (int)nc, &target, m) : !sift_seek(cur[0], target)) : !or_align(q, &target)) break;
		if (q->conj && nc == 1) target = cur[0]->g.gid, memcpy(m, cur[0]->g.pages, 32);
		st->groups++;
		int full = nh == q->k && !q->exhaustive;
		if (full) {
			float ub = 0;
			for (int i = 0; i < q->nt; i++) {
				sift_cur *c = q->t[i].cur;
				if (c->state == 1 && c->g.gid == target) ub += q->t[i].w * ((float)sift_grp_ub(c) / 255.0f);
			}
			if (ub * 1.0001f < hits[0].score || !blk_mask(q, target, hits[0].score, bm)) {
				st->skipped++;
				continue;
			}
		}
		sift_grp res;
		if (q->conj) {
			if (full)
				for (int j = 0; j < 4; j++) m[j] &= bm[j];
			res = cur[0]->g;
			if (nc == 1) sift_k->and2(&res, &res, m, &res, gb);
			for (uint32_t i = 1; i < nc && !pages_empty(res.pages); i++) sift_k->and2(&res, &cur[i]->g, i == 1 ? m : res.pages, &res, gb);
		} else {
			int nb = 0, first = 1;
			for (int i = 0; i < q->nt; i++) {
				sift_cur *c = q->t[i].cur;
				if (c->state != 1 || c->g.gid != target) continue;
				if (first) res = c->g, first = 0;
				else sift_k->or2(&res, &c->g, &res, gb + (nb++ & 1) * SIFT_GRP_BYTES);
			}
			if (q->filter) sift_k->and2(&res, &q->filter->g, full ? bm : NULL, &res, gb + 2 * SIFT_GRP_BYTES);
			else if (full) sift_k->and2(&res, &res, bm, &res, gb + 2 * SIFT_GRP_BYTES);
			for (int i = 0; i < q->nt; i++) q->t[i].rel = q->filter || full ? SIFT_REL_ANY : SIFT_REL_SUBSET;
		}
		if (pages_empty(res.pages)) continue;
		res.base = 0;
		uint32_t n = sift_grp_score(&res, q->t, q->nt, q->nm, score, tids, sbuf);
		st->scored += n;
		for (uint32_t i = 0; i < n; i++)
			if (nh < q->k || score[i] >= hits[0].score) heap_push(hits, &nh, q->k, (sift_hit){ tids[i], score[i] });
	}
	qsort(hits, nh, sizeof(sift_hit), hit_cmp);
	return nh;
}

void sift_facets(sift_cur *q, sift_cur **val, int nv, uint64_t *counts)
{
	for (const sift_grp *g; (g = sift_next(q));)
		for (int v = 0; v < nv; v++) {
			const sift_grp *f = sift_seek(val[v], g->gid);
			if (f && f->gid == g->gid) counts[v] += sift_k->and_count(g, f);
		}
}
