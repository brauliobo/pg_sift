/* stats: corpus statistics that drive the codec thresholds. usage: stats dir [tpp] */
#include "data.h"

#define PG_GROUP 256

static void pct(const char *label, const uint64_t *h, int n, const char **names)
{
	uint64_t tot = 0;
	for (int i = 0; i < n; i++) tot += h[i];
	printf("%s:", label);
	for (int i = 0; i < n; i++)
		if (h[i]) printf(" %s=%.2f%%", names[i], 100.0 * h[i] / tot);
	printf("\n");
}

int main(int argc, char **argv)
{
	corpus c;
	corpus_load(&c, argc > 1 ? argv[1] : "/tmp/sift-codec", argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	uint32_t npages = (uint32_t)(c.tid[c.nd - 1] >> 16) + 1;
	uint16_t *ntup = calloc(npages, 2), *maxoff = calloc(npages, 2);
	for (uint64_t i = 0; i < c.nd; i++) {
		uint32_t b = (uint32_t)(c.tid[i] >> 16);
		uint16_t o = (uint16_t)c.tid[i];
		ntup[b]++;
		if (o > maxoff[b]) maxoff[b] = o;
	}

	static const char *tb[] = { "0", "1", "2-3", "4-7", "8-15", "16-31", "32-63", "64-127", "128-255", "256+" };
	uint64_t ht[10] = { 0 }, hm[10] = { 0 }, used = 0, tsum = 0, msum = 0;
	for (uint32_t b = 0; b < npages; b++) {
		int k = ntup[b] ? 1 : 0, m = maxoff[b] ? 1 : 0;
		while (k && k < 9 && ntup[b] >= (2u << (k - 1))) k++;
		while (m && m < 9 && maxoff[b] >= (2u << (m - 1))) m++;
		ht[k]++, hm[m]++;
		used += ntup[b] > 0, tsum += ntup[b], msum += maxoff[b];
	}
	printf("[%s] docs=%lu terms=%u postings=%lu heap_pages=%u used_pages=%lu tuples/used_page=%.2f maxoff/used_page=%.2f\n",
	       c.scen, c.nd, c.nt, c.np, npages, used, (double)tsum / used, (double)msum / used);
	pct("tuples/page", ht, 10, tb);
	pct("maxoff/page", hm, 10, tb);

	/* per df class: terms, postings share, postings per page, per group, full-page share, max offset */
	uint64_t terms[NCLASS] = { 0 }, post[NCLASS] = { 0 }, pages[NCLASS] = { 0 }, groups[NCLASS] = { 0 };
	uint64_t fullp[NCLASS] = { 0 }, ofs9[NCLASS] = { 0 }, tfh[8] = { 0 };
	uint64_t *tids = malloc(c.nd * 8);
	for (uint32_t t = 0; t < c.nt; t++) {
		int k = df_class(c.df[t]);
		corpus_tids(&c, t, tids);
		terms[k]++, post[k] += c.df[t];
		for (uint32_t i = 0; i < c.df[t];) {
			uint32_t b = (uint32_t)(tids[i] >> 16), j = i;
			while (j < c.df[t] && (uint32_t)(tids[j] >> 16) == b) j++;
			pages[k]++;
			if (j - i == ntup[b]) fullp[k] += j - i;
			if (!i || (b / PG_GROUP) != (uint32_t)(tids[i - 1] >> 16) / PG_GROUP) groups[k]++;
			i = j;
		}
		for (uint32_t i = 0; i < c.df[t]; i++) ofs9[k] += (uint16_t)tids[i] > 256;
		for (uint64_t i = c.start[t]; i < c.start[t + 1]; i++) {
			uint8_t f = c.tf[i];
			tfh[f == 1 ? 0 : f == 2 ? 1 : f == 3 ? 2 : f < 8 ? 3 : f < 16 ? 4 : f < 32 ? 5 : f < 255 ? 6 : 7]++;
		}
	}
	printf("%-9s %9s %8s %8s %9s %10s %9s %8s\n", "df", "terms", "post%", "post/pg", "post/grp", "grp/term", "fullpg%", "off>256");
	for (int k = 0; k < NCLASS; k++)
		if (terms[k])
			printf("%-9s %9lu %7.2f%% %8.2f %9.2f %10.1f %8.2f%% %8lu\n", class_name[k], terms[k], 100.0 * post[k] / c.np,
			       (double)post[k] / pages[k], (double)post[k] / groups[k], (double)groups[k] / terms[k],
			       100.0 * fullp[k] / post[k], ofs9[k]);
	static const char *fb[] = { "1", "2", "3", "4-7", "8-15", "16-31", "32-254", "255+" };
	pct("tf", tfh, 8, fb);
	return 0;
}
