/*
 * fz_bm25.c - fuzz target for the decoders of the BM25 side that read a term's stored bytes (docs/SAFETY.md, test/fuzz/README.md).
 * One input = one mode byte + 4 bytes of arguments + the bytes of one stored object:
 *
 *   0  term blob, as the query path reads it   sift_cur_blob and the cursor on bytes nobody checked (sift_post_open has verify = false),
 *                                              with the 64 zero bytes the reader keeps behind a blob: groups, tids, counts, seeks, ranks
 *   1  term blob, checked                      sift_blob_check first; a blob it accepts must decode safely at every ISA, to the same
 *                                              strictly increasing tids (the existing tools/codec/fuzz property, here with the mutation
 *                                              engine of the fuzzer: coverage guided instead of bit flips of random blobs)
 *   2  term record                             a SiftTermInfo (the first 25 bytes) and the stream behind it: sift_post_open and
 *                                              sift_post_open_verified (blob, sidecar, tf stream, group scores, position checkpoints),
 *                                              per group tf and positions, the position reader, sift_post_check
 *
 *   FZ_GEN=DIR target     writes the seed corpus from the writers (sift_encode, sift_postw_*) and exits
 */
#include "postgres.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fz_shim.h"
#include "sift_bitmap.h"
#include "sift_dict.h"
#include "sift_posting.h"
#include "sift_varint.h"

#define NMODES 3
#define PAD SIFT_KERN_PAD

/* ---------- mode 0 and 1: the term blob ---------- */

static uint64
mix(uint64 h, uint64 v)
{
	return (h ^ v) * UINT64CONST(0x100000001B3);
}

/* drains a cursor: every group's counts and tids; the property is only that nothing outside the blob and the buffers is touched */
static uint64
drain(sift_cur *c, sift_tid *out, bool strict)
{
	const sift_grp *g;
	uint64		h = 0,
				total = 0;
	uint32		last_gid = 0;
	bool		first = true;

	while ((g = sift_next(c)))
	{
		uint64		cnt = sift_grp_count(g),
					k = sift_grp_count_mask(g, (const uint64[4]){~UINT64CONST(0), ~UINT64CONST(0), ~UINT64CONST(0), ~UINT64CONST(0)});

		if (strict && !first && g->gid <= last_gid)
			fz_bad("group ids not increasing: %u after %u", g->gid, last_gid);
		first = false;
		last_gid = g->gid;
		if (total + k > SIFT_GRP_MAXTIDS * 8)
			return h;			/* a corrupt blob can claim more groups than a hang is worth; stop */
		if (strict && k != cnt)
			fz_bad("group count %llu != tids %llu", (unsigned long long) cnt, (unsigned long long) k);
		if (sift_grp_tids(g, NULL, out + total) != k)
			fz_bad("sift_grp_tids != sift_grp_count_mask");
		for (uint64 i = 0; i < k; i++)
			h = mix(h, out[total + i]);
		total += k;
	}
	return mix(h, total);
}

typedef struct Job
{
	uint8	   *p;
	size_t		len;
	uint32		arg;
	uint64		digest;
} Job;

static sift_tid out_tids[SIFT_GRP_MAXTIDS * 8 + 16];

static void
blob_run(void *a)
{
	Job		   *j = a;
	uint8	   *buf = fz_copy((const uint8 *) "", 0, SIFT_CUR_BUF, 64);
	sift_cur	c;

	sift_cur_blob(&c, j->p, buf);
	j->digest = drain(&c, out_tids, false);
	sift_cur_blob(&c, j->p, buf);
	for (int s = 0; s < 4; s++)
	{
		const sift_grp *g = sift_seek(&c, (j->arg >> (8 * s)) & 0xff);

		if (g)
			j->digest = mix(j->digest, g->gid);
	}
	sift_cur_blob(&c, j->p, buf);
	j->digest = mix(j->digest, sift_count(&c));
	free(buf);
}

static void
checked_run(void *a)
{
	Job		   *j = a;
	uint8	   *buf = fz_copy((const uint8 *) "", 0, SIFT_CUR_BUF, 64);
	sift_cur	c;
	uint32		n;
	uint64		cnt;

	if (sift_blob_check(j->p, j->len))
	{
		free(buf);
		return;
	}
	n = sift_blob_n(j->p);
	sift_cur_blob(&c, j->p, buf);
	j->digest = drain(&c, out_tids, true);
	cnt = j->digest;
	sift_cur_blob(&c, j->p, buf);
	if (sift_count(&c) > SIFT_GRP_MAXTIDS * 8)
		fz_bad("sift_count of an accepted blob is %llu", (unsigned long long) sift_count(&c));
	(void) n;
	(void) cnt;
	free(buf);
}

/* ---------- mode 2: a term record ---------- */

static void
exercise_cursor(SiftPostingCursor *pc, Job *j, uint32 df)
{
	sift_cur   *cur = sift_post_cursor(pc);
	const sift_grp *g;
	int			nb;
	int			ngroups = 0;

	(void) sift_post_bounds(pc, &nb);
	sift_post_rewind(pc);
	while ((g = sift_next(cur)) && ngroups++ < 64)
	{
		uint64		n = sift_grp_count(g);
		uint8	   *tf;

		if (n > SIFT_GRP_MAXTIDS)
			fz_bad("group of %llu postings", (unsigned long long) n);
		tf = palloc0(n + PAD);
		sift_post_group_tf(pc, tf);
		for (uint64 i = 0; i < n; i++)
			j->digest = mix(j->digest, tf[i]);
		if (sift_post_has_positions(pc))
			for (uint64 i = 0; i < Min(n, 3); i++)
			{
				SiftPosBuf pb = {NULL, 0, 0, CurrentMemoryContext};

				sift_post_positions(pc, (uint32) (g->base + i), &pb);
				for (uint32 k = 0; k < pb.n; k++)
					j->digest = mix(j->digest, pb.v[k]);
			}
	}
	if (sift_post_has_positions(pc))
	{
		SiftPosReader *r = sift_post_pos_reader(pc, CurrentMemoryContext);
		uint32		len;

		for (uint32 k = 0; k < Min(df, 64); k++)	/* one record per posting, no more */
			j->digest = mix(j->digest, *sift_post_pos_next(r, &len) + len);
	}
}

static void
term_run(void *a)
{
	Job		   *j = a;
	SiftTermInfo info = {0};
	const uint8 *p = j->p;
	const uint8 *stream = p + 25;
	uint64		slen = j->len - 25;
	SiftPostingCursor *pc;

	memcpy(&info.df, p, 4);
	info.flags = p[4];
	info.max_bound = p[5];
	memcpy(&info.post_len, p + 6, 4);
	memcpy(&info.codec_len, p + 10, 4);
	memcpy(&info.nnib, p + 14, 4);
	memcpy(&info.pos_len, p + 18, 4);
	info.post_off = p[22] % (slen + 1);
	if (info.flags & SIFT_TERM_INLINE)
	{
		for (int i = 0; i < SIFT_INLINE_MAX; i++)
		{
			info.inline_keys[i] = (uint64) p[23] << 16 | (uint64) (1 + p[24] + i);
			info.inline_tf[i] = 1 + i;
		}
		info.inline_pos = stream;
		if (info.pos_len > slen)
			info.pos_len = (uint32) slen;
	}
	fz_stream_set(stream, slen);
	pc = sift_post_open((SiftStreamReader *) stream, &info, CurrentMemoryContext);
	exercise_cursor(pc, j, info.df);
	sift_post_check(pc);
	if (!(info.flags & SIFT_TERM_INLINE))
	{
		pc = sift_post_open_verified((SiftStreamReader *) stream, &info, CurrentMemoryContext);
		exercise_cursor(pc, j, info.df);
		sift_post_check(pc);
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	SiftSimdLevel lv[3];
	int			nl;
	uint64		first = 0;
	int			mode;
	Job			j;
	void		(*run) (void *);

	if (size < 6)
		return 0;
	mode = getenv("FZ_MODE") && *getenv("FZ_MODE") ? atoi(getenv("FZ_MODE")) % NMODES : data[0] % NMODES;
	memcpy(&j.arg, data + 1, 4);
	j.len = size - 5;
	if (mode == 2 && j.len < 25)
		return 0;
	run = mode == 0 ? blob_run : mode == 1 ? checked_run : term_run;
	nl = fz_levels(lv);
	for (int isa = 0; isa < SIFT_NISA; isa++)
	{
		uint8	   *copy;

		if (!sift_set_isa(isa))
			continue;
		copy = fz_copy(data + 5, j.len, PAD, 64);
		j.p = copy;
		j.digest = 0;
		if (!fz_try(run, &j))
		{
			free(copy);
			fz_reset();
			break;
		}
		free(copy);
		fz_reset();
		if (isa == 0)
			first = j.digest;
		else if (j.digest != first && mode != 2)
			fz_bad("mode %d: ISA %d decodes differently from scalar (%llx vs %llx)", mode, isa, (unsigned long long) j.digest,
				   (unsigned long long) first);
	}
	sift_set_isa(SIFT_ISA_SCALAR);
	(void) nl;
	return 0;
}

/* ---------- the seed corpus ---------- */

static void
emit(const char *dir, const char *name, int mode, uint32 arg, const void *head, size_t hlen, const void *p, size_t len)
{
	char		path[512];
	FILE	   *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen(path, "wb")))
		abort();
	fputc(mode, f);
	fwrite(&arg, 4, 1, f);
	if (hlen)
		fwrite(head, 1, hlen, f);
	fwrite(p, 1, len, f);
	fclose(f);
}

static uint64
rnd(uint64 *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static int
cmp64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* a position record of nocc occurrences starting near base */
static size_t
pos_record(uint8 *out, uint32 nocc, uint32 base, uint64 *s)
{
	uint8	   *p = out;

	if (nocc == 1)
		return (size_t) (sift_put_varint(p, (uint64) base << 1) - p);
	p = sift_put_varint(p, ((uint64) base << 1) | 1);
	p = sift_put_varint(p, nocc - 2);
	for (uint32 i = 1; i < nocc; i++)
		p = sift_put_varint(p, rnd(s) % 5);
	return (size_t) (p - out);
}

static void
gen_terms(const char *dir)
{
	uint64		s = 0x9E3779B97F4A7C15ull;
	SiftScoreRef ref = {1.2f, 0.75f, 100.0f};
	static const struct
	{
		uint32		df;
		uint32		span;		/* blocks the postings spread over */
		bool		pos;
		int			grouped_min;
	}			shapes[] = {
		{1, 4, false, 8192}, {2, 4, true, 8192}, {5, 50, false, 8192}, {40, 3, true, 8192}, {300, 5000, false, 8192},
		{300, 20, true, 8}, {1500, 30, false, 8}, {3000, 3000, true, 8}, {20, 5, true, 8}, {9000, 400, true, 8192}
	};

	for (size_t k = 0; k < lengthof(shapes); k++)
	{
		uint32		df = shapes[k].df;
		SiftKey    *keys = palloc(df * sizeof(SiftKey));
		uint64	   *raw = palloc(df * sizeof(uint64));
		uint8	   *tf = palloc(df),
				   *pos = palloc(df * 24 + 1);
		uint16	   *dl = palloc(df * sizeof(uint16));
		uint32	   *poslen = palloc(df * sizeof(uint32));
		size_t		pat = 0;
		uint32		nk = 0;
		SiftPostingWriter *w;
		SiftTermInfo info = {0};
		const uint8 *blob;
		uint8		head[25] = {0};
		char		name[64];
		size_t		blen;

		for (uint32 i = 0; i < df; i++)
			raw[i] = (rnd(&s) % shapes[k].span) << 16 | (1 + rnd(&s) % (k % 3 ? 200 : 400));
		qsort(raw, df, 8, cmp64);
		for (uint32 i = 0; i < df; i++)
			if (!nk || raw[i] != keys[nk - 1])
				keys[nk++] = raw[i];
		for (uint32 i = 0; i < nk; i++)
		{
			tf[i] = (uint8) (rnd(&s) % 7 == 0 ? 2 + rnd(&s) % 4 : 1);	/* tf is the number of occurrences the position record holds */
			dl[i] = (uint16) (20 + rnd(&s) % 300);
			poslen[i] = (uint32) pos_record(pos + pat, tf[i], (uint32) (rnd(&s) % 200), &s);
			pat += poslen[i];
		}
		sift_grouped_min_df = shapes[k].grouped_min;
		w = sift_postw_begin(NULL, &ref, CurrentMemoryContext);
		sift_postw_add(w, keys, tf, dl, (int) nk, shapes[k].pos ? pos : NULL, shapes[k].pos ? poslen : NULL);
		blob = sift_postw_end_term_mem(w, &info, CurrentMemoryContext);
		memcpy(head, &info.df, 4);
		head[4] = info.flags;
		head[5] = info.max_bound;
		memcpy(head + 6, &info.post_len, 4);
		memcpy(head + 10, &info.codec_len, 4);
		memcpy(head + 14, &info.nnib, 4);
		memcpy(head + 18, &info.pos_len, 4);
		if (info.flags & SIFT_TERM_INLINE)
		{
			head[23] = 0;
			head[24] = 1;
			blob = info.inline_pos;
			blen = (info.flags & SIFT_TERM_POS) ? info.pos_len : 0;
		}
		else
			blen = info.post_len;
		snprintf(name, sizeof(name), "term-%zu", k);
		emit(dir, name, 2, (uint32) k, head, sizeof(head), blob ? blob : (const uint8 *) "", blen);
		if (!(info.flags & SIFT_TERM_INLINE))
		{
			size_t		cl = (info.flags & SIFT_TERM_POS) ? info.post_len - info.pos_len : info.post_len;

			snprintf(name, sizeof(name), "blob-%zu", k);
			emit(dir, name, 0, (uint32) k, NULL, 0, blob, Min(cl, (size_t) info.codec_len));
			snprintf(name, sizeof(name), "checked-%zu", k);
			emit(dir, name, 1, (uint32) k, NULL, 0, blob, Min(cl, (size_t) info.codec_len));
		}
	}
	/* bare codec blobs of sift_encode in both layouts and the container kinds */
	for (int k = 0; k < 12; k++)
	{
		uint32		n = k < 4 ? 3 + k : 100 + 700 * k;
		sift_tid   *t = malloc(n * 8);
		uint8	   *tfv = malloc(n),
				   *ub = malloc(n),
				   *out;
		sift_enc_opts o = {k % 3, k % 4, 0, k & 1};
		size_t		len;
		uint32		m = 0;
		char		name[64];

		for (uint32 i = 0; i < n; i++)
			t[i] = SIFT_TID(rnd(&s) % (k < 6 ? 40 : 3000), 1 + rnd(&s) % (k & 1 ? 300 : 60));
		qsort(t, n, 8, cmp64);
		for (uint32 i = 0; i < n; i++)
			if (!m || t[i] != t[m - 1])
				t[m++] = t[i];
		for (uint32 i = 0; i < m; i++)
			tfv[i] = (uint8) (1 + rnd(&s) % 3), ub[i] = (uint8) rnd(&s);
		len = sift_encode(t, tfv, ub, m, &o, NULL);
		out = malloc(len);
		sift_encode(t, tfv, ub, m, &o, out);
		snprintf(name, sizeof(name), "codec-%d", k);
		emit(dir, name, 0, (uint32) k, NULL, 0, out, len);
		snprintf(name, sizeof(name), "codecchecked-%d", k);
		emit(dir, name, 1, (uint32) k, NULL, 0, out, len);
		free(t), free(tfv), free(ub), free(out);
	}
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *dir = getenv("FZ_GEN");

	fz_init();
	sift_init();
	fz_keep();
	if (dir)
	{
		mkdir(dir, 0755);
		gen_terms(dir);
		_exit(0);
	}
	return 0;
}
