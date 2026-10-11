/*
 * corpus: tokenize "block\toffset\ttext" lines (COPY of ctid + text) into a
 * term-major postings file and print the statistics that drive codec
 * thresholds.  Output (all little-endian):
 *   docs.bin  : u64 nd, then u64 tid[nd]   (tid = block << 16 | offset)
 *   post.bin  : u32 nt, u64 np, u32 df[nt], u32 doc[np], u8 tf[np]
 *   terms.txt : term \t df
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXTOK 48

typedef struct { uint64_t h; uint32_t id, len; char *s; } slot;

static slot *tab;
static uint64_t tabcap = 1 << 22, nterms;
static char **tstr;
static uint32_t *tlen;

static void *xrealloc(void *p, size_t n)
{
	if (!(p = realloc(p, n))) { perror("realloc"); exit(1); }
	return p;
}

static uint64_t hash(const char *s, uint32_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint32_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x100000001b3ull;
	return h ^ (h >> 29);
}

static void grow(void)
{
	slot *old = tab;
	uint64_t oc = tabcap;
	tabcap *= 2;
	tab = calloc(tabcap, sizeof(slot));
	for (uint64_t i = 0; i < oc; i++)
		if (old[i].s) {
			uint64_t j = old[i].h & (tabcap - 1);
			while (tab[j].s) j = (j + 1) & (tabcap - 1);
			tab[j] = old[i];
		}
	free(old);
}

static uint32_t intern(const char *s, uint32_t n)
{
	uint64_t h = hash(s, n), j = h & (tabcap - 1);
	for (; tab[j].s; j = (j + 1) & (tabcap - 1))
		if (tab[j].h == h && tab[j].len == n && !memcmp(tab[j].s, s, n)) return tab[j].id;
	tab[j] = (slot){ h, (uint32_t)nterms, n, strndup(s, n) };
	if (nterms % 65536 == 0) {
		tstr = xrealloc(tstr, (nterms + 65536) * sizeof(*tstr));
		tlen = xrealloc(tlen, (nterms + 65536) * sizeof(*tlen));
	}
	tstr[nterms] = tab[j].s;
	tlen[nterms] = n;
	if (++nterms * 2 > tabcap) grow();
	return (uint32_t)nterms - 1;
}

static int cmpu32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

static int wordc(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80; }

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: corpus reviews.tsv outdir\n"); return 1; }
	FILE *in = fopen(argv[1], "r");
	if (!in) { perror(argv[1]); return 1; }
	tab = calloc(tabcap, sizeof(slot));

	uint64_t nd = 0, capd = 1 << 20, np = 0, capp = 1 << 26;
	uint64_t *tids = malloc(capd * 8), *dstart = malloc((capd + 1) * 8);
	uint32_t *pterm = malloc(capp * 4), *dtoks = malloc((1 << 20) * 4);
	uint8_t *ptf = malloc(capp);
	char *line = NULL, tok[MAXTOK];
	size_t lcap = 0;
	ssize_t len;
	uint64_t ntok = 0;

	while ((len = getline(&line, &lcap, in)) > 0) {
		char *p = line, *end = line + len;
		unsigned long blk = strtoul(p, &p, 10), off = strtoul(p + 1, &p, 10);
		uint32_t nt = 0;
		if (nd == capd) {
			capd *= 2;
			tids = xrealloc(tids, capd * 8);
			dstart = xrealloc(dstart, (capd + 1) * 8);
		}
		tids[nd] = (uint64_t)blk << 16 | off;
		dstart[nd] = np;
		while (p < end) {
			while (p < end && !wordc((unsigned char)(*p | ((*p >= 'A' && *p <= 'Z') ? 0x20 : 0)))) p++;
			uint32_t n = 0;
			for (; p < end; p++) {
				unsigned char c = (unsigned char)*p;
				if (c >= 'A' && c <= 'Z') c |= 0x20;
				if (!wordc(c)) break;
				if (n < MAXTOK) tok[n++] = (char)c;
			}
			if (n) dtoks[nt++] = intern(tok, n);
			if (nt == (1 << 20)) break;
		}
		ntok += nt;
		qsort(dtoks, nt, 4, cmpu32);
		if (np + nt > capp) {
			capp = (np + nt) * 2;
			pterm = xrealloc(pterm, capp * 4);
			ptf = xrealloc(ptf, capp);
		}
		for (uint32_t i = 0; i < nt;) {
			uint32_t j = i;
			while (j < nt && dtoks[j] == dtoks[i]) j++;
			pterm[np] = dtoks[i];
			ptf[np++] = (uint8_t)(j - i > 255 ? 255 : j - i);
			i = j;
		}
		nd++;
	}
	dstart[nd] = np;
	fprintf(stderr, "docs %lu tokens %lu postings %lu terms %lu\n", nd, ntok, np, nterms);

	/* transpose doc-major to term-major with a counting sort */
	uint32_t *df = calloc(nterms, 4);
	uint64_t *tstart = malloc((nterms + 1) * 8);
	for (uint64_t i = 0; i < np; i++) df[pterm[i]]++;
	tstart[0] = 0;
	for (uint64_t t = 0; t < nterms; t++) tstart[t + 1] = tstart[t] + df[t];
	uint32_t *odoc = malloc(np * 4);
	uint8_t *otf = malloc(np);
	uint64_t *fill = malloc(nterms * 8);
	memcpy(fill, tstart, nterms * 8);
	int sorted = 1;
	for (uint64_t d = 0; d < nd; d++) {
		if (d && tids[d] <= tids[d - 1]) sorted = 0;
		for (uint64_t i = dstart[d]; i < dstart[d + 1]; i++) {
			uint64_t k = fill[pterm[i]]++;
			odoc[k] = (uint32_t)d;
			otf[k] = ptf[i];
		}
	}
	if (!sorted) fprintf(stderr, "warning: input not in ctid order\n");

	char path[4096];
	snprintf(path, sizeof path, "%s/docs.bin", argv[2]);
	FILE *f = fopen(path, "w");
	fwrite(&nd, 8, 1, f);
	fwrite(tids, 8, nd, f);
	fclose(f);
	snprintf(path, sizeof path, "%s/post.bin", argv[2]);
	f = fopen(path, "w");
	uint32_t nt32 = (uint32_t)nterms;
	fwrite(&nt32, 4, 1, f);
	fwrite(&np, 8, 1, f);
	fwrite(df, 4, nterms, f);
	fwrite(odoc, 4, np, f);
	fwrite(otf, 1, np, f);
	fclose(f);
	snprintf(path, sizeof path, "%s/terms.txt", argv[2]);
	f = fopen(path, "w");
	for (uint64_t t = 0; t < nterms; t++) fprintf(f, "%.*s\t%u\n", (int)tlen[t], tstr[t], df[t]);
	fclose(f);
	return 0;
}
