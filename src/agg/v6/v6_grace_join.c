/*
 * agg/v6/v6_grace_join.c - the partitioned hash join (v6_grace_join.h).
 */
#include "agg/v6/v6_grace_join.h"
#include "agg/v6/v6_bytes.h"
#include "agg/v6/v6_spill.h"

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
	SiftBytes	rec;
	SiftGraceStats st;
};

typedef struct GjRecHdr
{
	uint32		klen;
	uint32		plen;
} GjRecHdr;

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
	j->budget = Max(work_mem / 2, GJ_MIN_BUDGET);
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
	uint64		c = klen + plen + GJ_ROW_OVERHEAD;

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
	uint32		len = klen + plen;
	GjChunk    *c = t->chunks;
	uint8	   *dst;
	GjRow	   *r;

	if (t->nrows == t->rcap)
	{
		uint32		ncap = Max(t->rcap * 2, 64);

		/* check-memory: allow the rows of the table are bounded by its budget (half of work_mem): past it the rows spill to files */
		t->rows =t->rows ? repalloc(t->rows, sizeof(GjRow) * ncap) : MemoryContextAlloc(cx, sizeof(GjRow) * ncap);
		t->rcap = ncap;
	}
	if (t->hcap < t->rcap)
		gj_tab_rehash(t, cx, t->rcap);
	if (c == NULL || c->size - c->used < len)
	{
		uint32		sz = Max(t->chunk_size, (uint32) sizeof(GjChunk) + len);

		c = MemoryContextAlloc(cx, sz);
		c->size = sz - (uint32) sizeof(GjChunk);
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

typedef struct GjScan
{
	uint8	   *buf;
	uint32		cap;
} GjScan;

static bool
gj_next(SiftSpillSet *s, uint32 part, GjScan *sc, uint64 *hash, const uint8 **key, GjRecHdr *h, const uint8 **payload)
{
	uint32		len;

	if (!sift_spill_get(s, part, hash, &sc->buf, &sc->cap, &len))
		return false;
	memcpy(h, sc->buf, sizeof(*h));
	*key = sc->buf + sizeof(*h);
	*payload = *key + h->klen;
	return true;
}

static uint64
gj_est_bytes(const SiftSpillSet *s, uint32 part)
{
	return sift_spill_part_bytes(s, part) + sift_spill_part_records(s, part) * 2 * (sizeof(GjRow) + sizeof(uint32));
}

static void join_files(SiftGraceJoin *j, uint32 level, SiftSpillSet *bset, SiftSpillSet *pset, uint32 part);

/* The two files are split again by the hash of the next level, and each pair of the halves is joined */
static void
gj_resplit(SiftGraceJoin *j, uint32 level, SiftSpillSet *bset, SiftSpillSet *pset, uint32 part)
{
	uint32		nsub = sift_spill_nparts(gj_est_bytes(bset, part), j->budget);
	SiftSpillSet *bsub = sift_spill_create(j->cx, nsub);
	SiftSpillSet *psub = sift_spill_create(j->cx, nsub);
	GjScan		sc = {NULL, 0};
	uint64		hash;
	const uint8 *key, *payload;
	GjRecHdr	h;

	j->st.max_level = Max(j->st.max_level, level + 2);
	sift_spill_rewind(bset, part);
	while (gj_next(bset, part, &sc, &hash, &key, &h, &payload))
	{
		uint64		nh = gj_hash(level + 1, key, h.klen);

		gj_put_rec(j, bsub, sift_spill_part_of(nh, nsub), nh, key, h.klen, payload, h.plen);
	}
	sift_spill_drop(bset, part);
	if (pset != NULL && sift_spill_part_records(pset, part) > 0)
	{
		sift_spill_rewind(pset, part);
		while (gj_next(pset, part, &sc, &hash, &key, &h, &payload))
		{
			uint64		nh = gj_hash(level + 1, key, h.klen);

			gj_put_rec(j, psub, sift_spill_part_of(nh, nsub), nh, key, h.klen, payload, h.plen);
		}
	}
	if (sc.buf != NULL)
		pfree(sc.buf);
	j->st.spilled_bytes += sift_spill_total_bytes(bsub) + sift_spill_total_bytes(psub);
	for (uint32 p = 0; p < nsub; p++)
		join_files(j, level + 1, bsub, psub, p);
	sift_spill_free(bsub);
	sift_spill_free(psub);
}

/* A partition whose build side holds more than the budget in rows of one key: the build file in chunks, the probe file once for each, a bit for each probe row */
static void
gj_chunked(SiftGraceJoin *j, SiftSpillSet *bset, SiftSpillSet *pset, uint32 part)
{
	uint64		nprobe = pset != NULL ? sift_spill_part_records(pset, part) : 0;
	uint8	   *matched = MemoryContextAllocZero(j->cx, (nprobe + 7) / 8 + 1);
	GjScan		bsc = {NULL, 0}, psc = {NULL, 0};
	uint64		hash, bhash;
	const uint8 *key, *payload, *bkey, *bpay;
	GjRecHdr	h, bh;
	bool		pending = false, eof = false;

	sift_spill_rewind(bset, part);
	while (!eof || pending)
	{
		GjTab		t;
		uint64		ord = 0;

		gj_tab_init(&t, j->budget);
		for (;;)
		{
			if (!pending)
			{
				if (!gj_next(bset, part, &bsc, &bhash, &bkey, &bh, &bpay))
				{
					eof = true;
					break;
				}
				pending = true;
			}
			if (t.nrows > 0 && gj_tab_bytes(&t) + gj_row_cost(&t, bh.klen, bh.plen) > j->budget)
				break;
			gj_tab_insert(&t, j->cx, bhash, bkey, bh.klen, bpay, bh.plen);
			pending = false;
		}
		j->st.chunk_passes++;
		if (t.nrows > 0 && nprobe > 0)
		{
			sift_spill_rewind(pset, part);
			while (gj_next(pset, part, &psc, &hash, &key, &h, &payload))
			{
				bool		semi_done = (matched[ord >> 3] >> (ord & 7)) & 1;

				if (gj_pairs(j, &t, hash, key, h.klen, payload, h.plen, semi_done))
					matched[ord >> 3] |= (uint8) (1 << (ord & 7));
				ord++;
			}
		}
		j->st.peak_bytes = Max(j->st.peak_bytes, gj_tab_bytes(&t));
		gj_tab_free(&t);
	}
	if ((j->type == SGJ_LEFT || j->type == SGJ_ANTI) && nprobe > 0)
	{
		uint64		ord = 0;

		sift_spill_rewind(pset, part);
		while (gj_next(pset, part, &psc, &hash, &key, &h, &payload))
		{
			if (!((matched[ord >> 3] >> (ord & 7)) & 1))
				gj_unmatched(j, payload, h.plen);
			ord++;
		}
	}
	if (bsc.buf != NULL)
		pfree(bsc.buf);
	if (psc.buf != NULL)
		pfree(psc.buf);
	pfree(matched);
}

static void
join_files(SiftGraceJoin *j, uint32 level, SiftSpillSet *bset, SiftSpillSet *pset, uint32 part)
{
	uint64		nprobe = pset != NULL ? sift_spill_part_records(pset, part) : 0;
	GjScan		sc = {NULL, 0};
	GjTab		t;
	uint64		hash;
	const uint8 *key, *payload;
	GjRecHdr	h;

	if (nprobe == 0)
	{
		sift_spill_drop(bset, part);
		return;
	}
	if (gj_est_bytes(bset, part) > j->budget)
	{
		if (level < SIFT_GJOIN_MAX_LEVEL)
			gj_resplit(j, level, bset, pset, part);
		else
			gj_chunked(j, bset, pset, part);
		sift_spill_drop(bset, part);
		sift_spill_drop(pset, part);
		return;
	}
	gj_tab_init(&t, j->budget);
	sift_spill_rewind(bset, part);
	while (gj_next(bset, part, &sc, &hash, &key, &h, &payload))
		gj_tab_insert(&t, j->cx, hash, key, h.klen, payload, h.plen);
	sift_spill_drop(bset, part);
	j->st.peak_bytes = Max(j->st.peak_bytes, gj_tab_bytes(&t));
	sift_spill_rewind(pset, part);
	while (gj_next(pset, part, &sc, &hash, &key, &h, &payload))
		if (!gj_pairs(j, &t, hash, key, h.klen, payload, h.plen, false))
			gj_unmatched(j, payload, h.plen);
	sift_spill_drop(pset, part);
	gj_tab_free(&t);
	if (sc.buf != NULL)
		pfree(sc.buf);
}

void
sift_gjoin_finish(SiftGraceJoin *j)
{
	SIFT_INVARIANT(j->probing, "hash join: finished before the build side was done");
	if (j->bset == NULL)
		return;
	gj_tab_free(&j->tab);
	j->st.spilled_bytes = sift_spill_total_bytes(j->bset) + sift_spill_total_bytes(j->pset);
	for (uint32 p = 0; p < sift_spill_nparts_of(j->bset); p++)
		join_files(j, 0, j->bset, j->pset, p);
	sift_spill_free(j->bset);
	sift_spill_free(j->pset);
	j->bset = NULL;
	j->pset = NULL;
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
	if (j->bset != NULL)
		sift_spill_free(j->bset);
	if (j->pset != NULL)
		sift_spill_free(j->pset);
	sift_bytes_free(&j->rec);
	pfree(j);
}
