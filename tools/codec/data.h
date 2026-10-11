/* Loader for corpus output: term-major postings remapped to a ctid scenario. */
#ifndef SIFT_CODEC_DATA_H
#define SIFT_CODEC_DATA_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint32_t nt;      /* terms */
	uint64_t np, nd;  /* postings, docs */
	uint32_t *df;
	uint64_t *start;  /* term -> first posting, [nt + 1] */
	uint32_t *doc;
	uint8_t *tf;
	uint64_t *tid;    /* doc -> ctid in the chosen scenario */
	char scen[32];
} corpus;

static void *load_file(const char *dir, const char *name, size_t *len)
{
	char path[4096];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	rewind(f);
	void *p = malloc(*len);
	if (fread(p, 1, *len, f) != *len) { perror(path); exit(1); }
	fclose(f);
	return p;
}

/* tpp == 0: real ctids; tpp > 0: dense remap doc i -> (i / tpp, i % tpp + 1) */
static void corpus_load(corpus *c, const char *dir, unsigned tpp)
{
	size_t len;
	uint8_t *d = load_file(dir, "docs.bin", &len), *p = load_file(dir, "post.bin", &len);
	memcpy(&c->nd, d, 8);
	c->tid = (uint64_t *)(d + 8);
	if (tpp)
		for (uint64_t i = 0; i < c->nd; i++) c->tid[i] = (i / tpp) << 16 | (i % tpp + 1);
	memcpy(&c->nt, p, 4);
	memcpy(&c->np, p + 4, 8);
	c->df = (uint32_t *)(p + 12);
	c->doc = c->df + c->nt;
	c->tf = (uint8_t *)(c->doc + c->np);
	c->start = malloc((c->nt + 1) * 8);
	c->start[0] = 0;
	for (uint32_t t = 0; t < c->nt; t++) c->start[t + 1] = c->start[t] + c->df[t];
	snprintf(c->scen, sizeof c->scen, tpp ? "dense%u" : "real", tpp);
}

/* Fill out[] with the sorted ctids of term t. */
static void corpus_tids(const corpus *c, uint32_t t, uint64_t *out)
{
	for (uint64_t i = c->start[t]; i < c->start[t + 1]; i++) out[i - c->start[t]] = c->tid[c->doc[i]];
}

/* df classes used by every report */
#define NCLASS 9
static const uint32_t class_lo[NCLASS] = { 1, 2, 4, 16, 128, 1024, 8192, 65536, 524288 };
static const char *class_name[NCLASS] = { "1", "2-3", "4-15", "16-127", "128-1K", "1K-8K", "8K-64K", "64K-512K", ">=512K" };

static int df_class(uint32_t df)
{
	int k = 0;
	while (k + 1 < NCLASS && df >= class_lo[k + 1]) k++;
	return k;
}

#endif
