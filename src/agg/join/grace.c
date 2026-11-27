/*
 * agg/join/grace.c - the partitioned hash join (v6_grace_join.h).
 */
#include "agg/join/grace.h"
#include "agg/bytes.h"
#include "agg/gj_bits.h"
#include "agg/spill.h"

#define GJ_NO_PART		UINT32_MAX
#define GJ_MIN_BUDGET	(32 * 1024)
#define GJ_MAX_CHUNK	(64 * 1024)
#define GJ_MIN_CHUNK	1024
#define GJ_ROW_OVERHEAD	40

typedef struct GjRow
{
	uint64		hash;
	const uint8 *data;			/* key, then payload */
	uint32		next;			/* index + 1 of the next row of the bucket, 0: none */
	uint32		klen;
	uint32		plen;
	uint32		pad;
} GjRow;

typedef struct GjChunk
{
	struct GjChunk *next;
	uint32		size;
	uint32		used;
	uint8		data[];
} GjChunk;

typedef struct GjTab
{
	GjRow	   *rows;
	uint32		nrows;
	uint32		rcap;
	uint32	   *head;
	uint32		hcap;
	GjChunk    *chunks;
	uint64		arena_bytes;
	uint32		chunk_size;
} GjTab;

typedef struct GjRecHdr
{
	uint32		klen;
	uint32		plen;
} GjRecHdr;

typedef struct GjScan
{
	uint8	   *buf;
	uint32		cap;
} GjScan;

typedef enum GjStep
{
	GS_PARTS,					/* the next partition of the two sets */
	GS_PROBE,					/* its build file is in the table, the probe file is read */
	GS_CHUNK_LOAD,				/* too much of one key: the next chunk of the build file into the table */
	GS_CHUNK_PROBE,				/* the probe file against the chunk */
	GS_TAIL						/* the probe rows that matched no chunk */
} GjStep;

/* The partitions of a pair of sets being joined; a partition too big for the budget is split into a frame of the next level above this one */
typedef struct GjFrame
{
	uint32		level;
	SiftSpillSet *bset;
	SiftSpillSet *pset;
	uint32		nparts;
	uint32		next_part;
	uint32		part;
	GjStep		step;
	GjTab		t;
	GjScan		bsc;
	GjScan		psc;
	SiftGjBits *bits;
	uint64		ord;
	uint64		bhash;			/* the build record read that is not in a chunk yet */
	GjRecHdr	bh;
	const uint8 *bkey;
	const uint8 *bpay;
	bool		pending;
	bool		beof;
} GjFrame;

struct SiftGraceJoin
{
	MemoryContext cx;
	SiftJoinType type;
	uint64		budget;			/* the build side's: half of work_mem */
	uint64		expect;
	uint32		level;
	SiftJoinEmit emit;
	void	   *emit_arg;
	GjTab		tab;
	SiftSpillSet *bset;
	SiftSpillSet *pset;
	uint32		resident;
	bool		probing;
	bool		joining;		/* sift_gjoin_next began */
	GjFrame		frame[SIFT_GJOIN_MAX_LEVEL + 1];
	int			nframes;
	SiftBytes	rec;
	SiftGraceStats st;
};

static uint64
gj_tab_bytes(const GjTab *t)
{
	return (uint64) t->rcap * sizeof(GjRow) + (uint64) t->hcap * sizeof(uint32) + t->arena_bytes;
}

static void
gj_note_peak(SiftGraceJoin *j)
{
	j->st.peak_bytes = Max(j->st.peak_bytes, gj_tab_bytes(&j->tab));
}

static void
gj_tab_init(GjTab *t, uint64 budget)
{
	memset(t, 0, sizeof(*t));
	t->chunk_size = (uint32) Min(GJ_MAX_CHUNK, Max(GJ_MIN_CHUNK, budget / 8));
}

static void
gj_tab_free(GjTab *t)
{
	while (t->chunks != NULL)
	{
		GjChunk    *c = t->chunks;

		t->chunks = c->next;
		pfree(c);
	}
	if (t->rows != NULL)
		pfree(t->rows);
	if (t->head != NULL)
		pfree(t->head);
	t->rows = NULL;
	t->head = NULL;
	t->nrows = t->rcap = t->hcap = 0;
	t->arena_bytes = 0;
}

static uint64
gj_hash(uint32 level, const void *key, uint32 klen)
{
	uint64		h = hash_bytes_extended((const unsigned char *) key, (int) klen, level + 1000);

	h *= UINT64_C(0xD6E8FEB86659FD93);
	return h ^ (h >> 32);
}

static SiftGraceJoin *
gj_new(MemoryContext cx, SiftJoinType type, uint64 work_mem, uint64 expect, uint32 level, SiftJoinEmit emit, void *arg)
{
	SiftGraceJoin *j = MemoryContextAllocZero(cx, sizeof(SiftGraceJoin));

	j->cx = cx;
	j->type = type;
	j->budget = Max(Min(work_mem / 2, MaxAllocSize / 2), GJ_MIN_BUDGET);
	j->expect = expect;
	j->level = level;
	j->emit = emit;
	j->emit_arg = arg;
	j->resident = GJ_NO_PART;
	gj_tab_init(&j->tab, j->budget);
	sift_bytes_init(&j->rec, cx);
	return j;
}

SiftGraceJoin *
sift_gjoin_create(MemoryContext cx, SiftJoinType type, uint64 work_mem_bytes, uint64 est_build_bytes, SiftJoinEmit emit, void *emit_arg)
{
	return gj_new(cx, type, work_mem_bytes, est_build_bytes, 0, emit, emit_arg);
}

/* the bytes a row of klen + plen takes in a table, and the growth the table needs to hold it */
static uint64
gj_row_cost(const GjTab *t, uint32 klen, uint32 plen)
{
	uint64		c = (uint64) klen + plen + GJ_ROW_OVERHEAD;

	if (t->nrows + 1 > t->rcap)
		c += (uint64) Max(t->rcap, 64) * (sizeof(GjRow) + sizeof(uint32));
	return c;
}

static void
gj_tab_rehash(GjTab *t, MemoryContext cx, uint32 hcap)
{
	uint32	   *h = MemoryContextAllocZero(cx, sizeof(uint32) * hcap);

	for (uint32 i = 0; i < t->nrows; i++)
	{
		uint32		b = (uint32) t->rows[i].hash & (hcap - 1);

		CHECK_FOR_INTERRUPTS();
		t->rows[i].next = h[b];
		h[b] = i + 1;
	}
	if (t->head != NULL)
		pfree(t->head);
	t->head = h;
	t->hcap = hcap;
}

static void
gj_tab_insert(GjTab *t, MemoryContext cx, uint64 hash, const void *key, uint32 klen, const void *payload, uint32 plen)
{
	const Size	len = (Size) klen + plen;
	GjChunk    *c = t->chunks;
	uint8	   *dst;
	GjRow	   *r;

	if (t->nrows == t->rcap)
	{
		uint32		ncap = Max(t->rcap * 2, 64);

		/* check-memory: allow the rows of the table are bounded by its budget (half of work_mem): past it the rows spill to files */
		t->rows = t->rows ? repalloc(t->rows, sizeof(GjRow) * ncap) : MemoryContextAlloc(cx, sizeof(GjRow) * ncap);
		t->rcap = ncap;
	}
	if (t->hcap < t->rcap)
		gj_tab_rehash(t, cx, t->rcap);
	if (c == NULL || c->size - c->used < len)
	{
		const Size	sz = Max(t->chunk_size, sizeof(GjChunk) + len);

		c = MemoryContextAlloc(cx, sz);
		c->size = (uint32) (sz - sizeof(GjChunk));
		c->used = 0;
		c->next = t->chunks;
		t->chunks = c;
		t->arena_bytes += sz;
	}
	dst = c->data + c->used;
	if (klen > 0)
		memcpy(dst, key, klen);
	if (plen > 0)
		memcpy(dst + klen, payload, plen);
	c->used += len;
	r = &t->rows[t->nrows];
	r->hash = hash;
	r->data = dst;
	r->klen = klen;
	r->plen = plen;
	r->pad = 0;
	r->next = t->head[(uint32) hash & (t->hcap - 1)];
	t->head[(uint32) hash & (t->hcap - 1)] = ++t->nrows;
}

/* a row is held, and spilled, as one allocation of its key and payload */
static void
gj_check_row(uint32 klen, uint32 plen)
{
	const uint64 need = (uint64) klen + plen + Max(sizeof(GjChunk), sizeof(GjRecHdr));

	if (need > MaxAllocSize)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a row of a hash join of %llu bytes is over 1 GB", (unsigned long long) need),
						errhint("Join on a shorter key or select fewer columns.")));
}

/* the pairs of one probe row: every match for inner and left, the row once for semi, none for anti; true when it matched */
static bool
gj_pairs(SiftGraceJoin *j, const GjTab *t, uint64 h, const void *key, uint32 klen, const uint8 *probe, uint32 plen, bool semi_done)
{
	bool		matched = false;

	if (t->hcap == 0)
		return false;
	for (uint32 i = t->head[(uint32) h & (t->hcap - 1)]; i != 0; i = t->rows[i - 1].next)
	{
		const GjRow *r = &t->rows[i - 1];

		CHECK_FOR_INTERRUPTS();
		if (r->hash != h || r->klen != klen || (klen > 0 && memcmp(r->data, key, klen) != 0))
			continue;
		matched = true;
		if (j->type == SGJ_INNER || j->type == SGJ_LEFT)
			j->emit(j->emit_arg, probe, plen, r->data + klen, r->plen);
		else
		{
			if (j->type == SGJ_SEMI && !semi_done)
				j->emit(j->emit_arg, probe, plen, NULL, 0);
			break;
		}
	}
	return matched;
}

static void
gj_unmatched(SiftGraceJoin *j, const uint8 *probe, uint32 plen)
{
	if (j->type == SGJ_LEFT || j->type == SGJ_ANTI)
		j->emit(j->emit_arg, probe, plen, NULL, 0);
}

static void
gj_put_rec(SiftGraceJoin *j, SiftSpillSet *s, uint32 part, uint64 hash, const void *key, uint32 klen, const void *payload, uint32 plen)
{
	GjRecHdr	h = {klen, plen};

	sift_bytes_reset(&j->rec);
	sift_bytes_put(&j->rec, &h, sizeof(h));
	sift_bytes_put(&j->rec, key, klen);
	sift_bytes_put(&j->rec, payload, plen);
	sift_spill_put(s, part, hash, j->rec.buf, j->rec.len);
}

/* The table is over its budget: the rows outside the resident partition go to their files; a second time the resident partition goes too */
static void
gj_evict(SiftGraceJoin *j)
{
	GjTab		old = j->tab;
	uint32		nparts;
	uint32		keep;

	if (j->bset == NULL)
	{
		nparts = sift_spill_nparts(j->expect ? j->expect : gj_tab_bytes(&old) * 4, j->budget);
		j->bset = sift_spill_create(j->cx, nparts);
		j->resident = 0;
		j->st.partitions = nparts;
	}
	else
		j->resident = GJ_NO_PART;
	keep = j->resident;
	nparts = sift_spill_nparts_of(j->bset);
	j->st.max_level = Max(j->st.max_level, 1);
	gj_tab_init(&j->tab, j->budget);
	for (uint32 i = 0; i < old.nrows; i++)
	{
		const GjRow *r = &old.rows[i];
		uint32		part = sift_spill_part_of(r->hash, nparts);

		CHECK_FOR_INTERRUPTS();
		if (part == keep)
			gj_tab_insert(&j->tab, j->cx, r->hash, r->data, r->klen, r->data + r->klen, r->plen);
		else
			gj_put_rec(j, j->bset, part, r->hash, r->data, r->klen, r->data + r->klen, r->plen);
	}
	gj_tab_free(&old);
	gj_note_peak(j);
}

void
sift_gjoin_build(SiftGraceJoin *j, const void *key, uint32 klen, bool key_null, const void *payload, uint32 plen)
{
	uint64		h;

	SIFT_INVARIANT(!j->probing, "hash join: build row after the probe began");
	if (key_null)
		return;
	gj_check_row(klen, plen);
	j->st.build_rows++;
	h = gj_hash(j->level, key, klen);
	for (;;)
	{
		if (j->bset != NULL)
		{
			uint32		part = sift_spill_part_of(h, sift_spill_nparts_of(j->bset));

			if (part != j->resident)
			{
				gj_put_rec(j, j->bset, part, h, key, klen, payload, plen);
				return;
			}
		}
		if (j->level < SIFT_GJOIN_MAX_LEVEL && j->tab.nrows > 0 && gj_tab_bytes(&j->tab) + gj_row_cost(&j->tab, klen, plen) > j->budget)
		{
			gj_evict(j);
			continue;
		}
		break;
	}
	gj_tab_insert(&j->tab, j->cx, h, key, klen, payload, plen);
	gj_note_peak(j);
}

void
sift_gjoin_build_done(SiftGraceJoin *j)
{
	j->probing = true;
	if (j->bset != NULL)
		j->pset = sift_spill_create(j->cx, sift_spill_nparts_of(j->bset));
}

void
sift_gjoin_probe(SiftGraceJoin *j, const void *key, uint32 klen, bool key_null, const void *payload, uint32 plen)
{
	uint64		h;

	SIFT_INVARIANT(j->probing, "hash join: probe row before the build side is done");
	j->st.probe_rows++;
	if (key_null)
	{
		gj_unmatched(j, payload, plen);
		return;
	}
	gj_check_row(klen, plen);
	h = gj_hash(j->level, key, klen);
	if (j->pset != NULL)
	{
		uint32		part = sift_spill_part_of(h, sift_spill_nparts_of(j->pset));

		if (part != j->resident)
		{
			gj_put_rec(j, j->pset, part, h, key, klen, payload, plen);
			return;
		}
	}
	if (!gj_pairs(j, &j->tab, h, key, klen, payload, plen, false))
		gj_unmatched(j, payload, plen);
}

/* ---- the partitions that went to files ---- */

static bool
gj_next(SiftSpillSet *s, uint32 part, GjScan *sc, uint64 *hash, const uint8 **key, GjRecHdr *h, const uint8 **payload)
{
	uint32		len;

	CHECK_FOR_INTERRUPTS();
	if (!sift_spill_get(s, part, hash, &sc->buf, &sc->cap, &len))
		return false;
	memcpy(h, sc->buf, sizeof(*h));
	*key = sc->buf + sizeof(*h);
	*payload = *key + h->klen;
	return true;
}

static void
gj_scan_free(GjScan *sc)
{
	if (sc->buf != NULL)
		pfree(sc->buf);
	*sc = (GjScan) {NULL, 0};
}

/* What the table of a build file takes once loaded (gj_load): a row for each record, a power of two of buckets, and its arena, which the file's bytes (headers too) bound */
static uint64
gj_est_bytes(const SiftSpillSet *s, uint32 part)
{
	const uint64 n = sift_spill_part_records(s, part);

	return sift_spill_part_bytes(s, part) + n * sizeof(GjRow) + pg_nextpower2_64(Max(n, 64)) * sizeof(uint32);
}

static GjFrame *
gj_top(SiftGraceJoin *j)
{
	return &j->frame[j->nframes - 1];
}

static void
gj_push(SiftGraceJoin *j, uint32 level, SiftSpillSet *bset, SiftSpillSet *pset)
{
	j->frame[j->nframes++] = (GjFrame) {.level = level,.bset = bset,.pset = pset,.nparts = sift_spill_nparts_of(bset),.step = GS_PARTS};
}

/* what a partition holds apart from its files */
static void
gj_frame_part_free(GjFrame *f)
{
	gj_tab_free(&f->t);
	gj_scan_free(&f->bsc);
	gj_scan_free(&f->psc);
	if (f->bits != NULL)
		sift_gj_bits_close(f->bits);
	f->bits = NULL;
}

/* the partition is joined: its files are closed */
static void
gj_part_done(GjFrame *f)
{
	gj_frame_part_free(f);
	sift_spill_drop(f->bset, f->part);
	sift_spill_drop(f->pset, f->part);
	f->step = GS_PARTS;
}

static void
gj_pop(SiftGraceJoin *j)
{
	GjFrame    *f = gj_top(j);

	gj_frame_part_free(f);
	sift_spill_free(f->bset);
	sift_spill_free(f->pset);
	if (f->level == 0)
		j->bset = j->pset = NULL;
	j->nframes--;
}

/* The two files are split again by the hash of the next level: the pairs of the halves are the partitions of a frame above this one */
static void
gj_resplit(SiftGraceJoin *j, GjFrame *f)
{
	const uint32 nsub = sift_spill_nparts(gj_est_bytes(f->bset, f->part), j->budget);
	SiftSpillSet *bsub = sift_spill_create(j->cx, nsub);
	SiftSpillSet *psub = sift_spill_create(j->cx, nsub);
	uint64		hash;
	const uint8 *key, *payload;
	GjRecHdr	h;

	j->st.max_level = Max(j->st.max_level, f->level + 2);
	sift_spill_rewind(f->bset, f->part);
	while (gj_next(f->bset, f->part, &f->bsc, &hash, &key, &h, &payload))
	{
		const uint64 nh = gj_hash(f->level + 1, key, h.klen);

		gj_put_rec(j, bsub, sift_spill_part_of(nh, nsub), nh, key, h.klen, payload, h.plen);
	}
	sift_spill_drop(f->bset, f->part);
	sift_spill_rewind(f->pset, f->part);
	while (gj_next(f->pset, f->part, &f->psc, &hash, &key, &h, &payload))
	{
		const uint64 nh = gj_hash(f->level + 1, key, h.klen);

		gj_put_rec(j, psub, sift_spill_part_of(nh, nsub), nh, key, h.klen, payload, h.plen);
	}
	sift_spill_drop(f->pset, f->part);
	gj_scan_free(&f->bsc);
	gj_scan_free(&f->psc);
	j->st.spilled_bytes += sift_spill_total_bytes(bsub) + sift_spill_total_bytes(psub);
	gj_push(j, f->level + 1, bsub, psub);
}

/* The build file of the partition fits the budget: into the table, to be probed by the probe file */
static void
gj_load(SiftGraceJoin *j, GjFrame *f)
{
	const uint32 n = (uint32) sift_spill_part_records(f->bset, f->part);
	uint64		hash;
	const uint8 *key, *payload;
	GjRecHdr	h;

	gj_tab_init(&f->t, j->budget);
	f->t.rows = MemoryContextAlloc(j->cx, sizeof(GjRow) * n);
	f->t.rcap = n;
	gj_tab_rehash(&f->t, j->cx, (uint32) pg_nextpower2_64(Max(n, 64)));
	sift_spill_rewind(f->bset, f->part);
	while (gj_next(f->bset, f->part, &f->bsc, &hash, &key, &h, &payload))
		gj_tab_insert(&f->t, j->cx, hash, key, h.klen, payload, h.plen);
	sift_spill_drop(f->bset, f->part);
	j->st.peak_bytes = Max(j->st.peak_bytes, gj_tab_bytes(&f->t));
	sift_spill_rewind(f->pset, f->part);
	f->step = GS_PROBE;
}

/* A partition whose build side holds more than the budget in rows of one key: the build file in chunks, the probe file once for each, a bit for each probe row that has matched */
static void
gj_chunk_begin(SiftGraceJoin *j, GjFrame *f)
{
	sift_spill_rewind(f->bset, f->part);
	f->pending = f->beof = false;
	f->bits = j->type == SGJ_INNER ? NULL : sift_gj_bits_open(j->cx);
	f->step = GS_CHUNK_LOAD;
}

static void
gj_chunk_load(SiftGraceJoin *j, GjFrame *f)
{
	gj_tab_init(&f->t, j->budget);
	for (;;)
	{
		if (!f->pending)
		{
			if (!gj_next(f->bset, f->part, &f->bsc, &f->bhash, &f->bkey, &f->bh, &f->bpay))
			{
				f->beof = true;
				break;
			}
			f->pending = true;
		}
		if (f->t.nrows > 0 && gj_tab_bytes(&f->t) + gj_row_cost(&f->t, f->bh.klen, f->bh.plen) > j->budget)
			break;
		gj_tab_insert(&f->t, j->cx, f->bhash, f->bkey, f->bh.klen, f->bpay, f->bh.plen);
		f->pending = false;
	}
	j->st.chunk_passes++;
	sift_spill_rewind(f->pset, f->part);
	f->ord = 0;
	f->step = GS_CHUNK_PROBE;
}

static void
gj_chunk_probed(SiftGraceJoin *j, GjFrame *f)
{
	j->st.peak_bytes = Max(j->st.peak_bytes, gj_tab_bytes(&f->t));
	gj_tab_free(&f->t);
	if (!f->beof || f->pending)
		f->step = GS_CHUNK_LOAD;
	else if (j->type == SGJ_LEFT || j->type == SGJ_ANTI)
	{
		sift_spill_rewind(f->pset, f->part);
		f->ord = 0;
		f->step = GS_TAIL;
	}
	else
		gj_part_done(f);
}

static void
gj_enter_part(SiftGraceJoin *j, GjFrame *f, uint32 part)
{
	f->part = part;
	if (sift_spill_part_records(f->pset, part) == 0)
		sift_spill_drop(f->bset, part);
	else if (gj_est_bytes(f->bset, part) <= j->budget)
		gj_load(j, f);
	else if (f->level < SIFT_GJOIN_MAX_LEVEL)
		gj_resplit(j, f);
	else
		gj_chunk_begin(j, f);
}

/* one probe row of the partition: its pairs are emitted */
static void
gj_probe_row(SiftGraceJoin *j, GjFrame *f)
{
	uint64		hash;
	const uint8 *key, *payload;
	GjRecHdr	h;

	if (!gj_next(f->pset, f->part, &f->psc, &hash, &key, &h, &payload))
		gj_part_done(f);
	else if (!gj_pairs(j, &f->t, hash, key, h.klen, payload, h.plen, false))
		gj_unmatched(j, payload, h.plen);
}

static void
gj_chunk_probe_row(SiftGraceJoin *j, GjFrame *f)
{
	uint64		hash;
	const uint8 *key, *payload;
	GjRecHdr	h;

	if (!gj_next(f->pset, f->part, &f->psc, &hash, &key, &h, &payload))
		gj_chunk_probed(j, f);
	else
	{
		const bool	seen = f->bits != NULL && sift_gj_bits_get(f->bits, f->ord);

		if (gj_pairs(j, &f->t, hash, key, h.klen, payload, h.plen, seen) && f->bits != NULL)
			sift_gj_bits_set(f->bits, f->ord);
		f->ord++;
	}
}

static void
gj_tail_row(SiftGraceJoin *j, GjFrame *f)
{
	uint64		hash;
	const uint8 *key, *payload;
	GjRecHdr	h;

	if (!gj_next(f->pset, f->part, &f->psc, &hash, &key, &h, &payload))
		gj_part_done(f);
	else
	{
		if (!sift_gj_bits_get(f->bits, f->ord))
			gj_unmatched(j, payload, h.plen);
		f->ord++;
	}
}

/* what the join gives from one step of the partitions in files: false when there is no more */
bool
sift_gjoin_next(SiftGraceJoin *j)
{
	SIFT_INVARIANT(j->probing, "hash join: finished before the build side was done");
	if (!j->joining)
	{
		j->joining = true;
		if (j->bset == NULL)
			return false;
		gj_tab_free(&j->tab);
		j->st.spilled_bytes = sift_spill_total_bytes(j->bset) + sift_spill_total_bytes(j->pset);
		gj_push(j, 0, j->bset, j->pset);
	}
	while (j->nframes > 0)
	{
		GjFrame    *f = gj_top(j);

		CHECK_FOR_INTERRUPTS();
		switch (f->step)
		{
			case GS_PARTS:
				if (f->next_part < f->nparts)
					gj_enter_part(j, f, f->next_part++);
				else
					gj_pop(j);
				break;
			case GS_PROBE:
				gj_probe_row(j, f);
				return true;
			case GS_CHUNK_LOAD:
				gj_chunk_load(j, f);
				break;
			case GS_CHUNK_PROBE:
				gj_chunk_probe_row(j, f);
				return true;
			case GS_TAIL:
				gj_tail_row(j, f);
				return true;
		}
	}
	return false;
}

const SiftGraceStats *
sift_gjoin_stats(const SiftGraceJoin *j)
{
	return &j->st;
}

void
sift_gjoin_free(SiftGraceJoin *j)
{
	gj_tab_free(&j->tab);
	for (int i = 0; i < j->nframes; i++)
	{
		gj_frame_part_free(&j->frame[i]);
		if (j->frame[i].level > 0)
		{
			sift_spill_free(j->frame[i].bset);
			sift_spill_free(j->frame[i].pset);
		}
	}
	if (j->bset != NULL)
		sift_spill_free(j->bset);
	if (j->pset != NULL)
		sift_spill_free(j->pset);
	sift_bytes_free(&j->rec);
	pfree(j);
}
