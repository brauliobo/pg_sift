/*
 * agg/par/radix.c - the two-phase radix-partitioned parallel aggregation of agg/par/radix.h.
 *
 * The records of a block are those of agg/ht/rec.h.
 */
#include "core/host.h"

#include "access/parallel.h"
#include "agg/par/radix.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "storage/proc.h"
#include "storage/s_lock.h"
#include "port/pg_bitutils.h"
#include "utils/wait_event.h"

static inline SiftRadixChunk *
chunk_at(SiftParCoord *c, uint32 idx)
{
	return (SiftRadixChunk *) ((char *) c + c->chunk_pool_off + (Size) idx * SIFT_RADIX_CHUNK_SIZE);
}

static inline SiftMorselQueue *
morsel_of(SiftParCoord *c)
{
	return (SiftMorselQueue *) ((char *) c + c->morsel_off);
}

static inline shm_mq *
queue_at(SiftParCoord *c, int w)
{
	return (shm_mq *) ((char *) c + c->mq_off + (Size) w * c->mq_size);
}

#define COORD_HDR	TYPEALIGN(SIFT_PAR_SHM_ALIGN, sizeof(SiftParCoord))
#define MORSEL_SPAN	TYPEALIGN(SIFT_PAR_SHM_ALIGN, sift_morsel_size(1))

static Size
mq_size_for(int n_workers)
{
	return n_workers >= SIFT_RADIX_MQ_WIDE_WORKERS ? SIFT_RADIX_MQ_WIDE_SIZE : SIFT_RADIX_MQ_SIZE;
}

Size
sift_par_coord_size(uint32 max_chunks, int n_workers)
{
	return sift_par_coord_size_custom(max_chunks) + mul_size(mq_size_for(n_workers), n_workers);
}

Size
sift_par_coord_size_custom(uint32 max_chunks)
{
	return SIFT_PAR_SHM_ALIGN + COORD_HDR + MORSEL_SPAN + mul_size(SIFT_RADIX_CHUNK_SIZE, max_chunks);
}

/* MEM[MEM-AGG-18]: parallel radix worker mq + stage (a worker's stage is n_parts * SIFT_RADIX_STAGE bytes besides its local table; the chunks of the pool are half of hash memory, at most SIFT_PAR_DSM_MAX) */
uint32
sift_par_budget_chunks(void)
{
	return (uint32) Max(1, Min(get_hash_memory_limit() / 2, SIFT_PAR_DSM_MAX) / SIFT_RADIX_CHUNK_SIZE);
}

static void
coord_reset_parts(SiftParCoord *c)
{
	pg_atomic_write_u32(&c->next_part, 0);
	pg_atomic_write_u32(&c->next_free_chunk, 0);
	pg_atomic_write_u64(&c->total_rows_scanned, 0);
	pg_atomic_write_u64(&c->total_groups_out, 0);
	BarrierInit(&c->phase_barrier, 0);
	for (uint32 p = 0; p < c->n_parts; p++)
	{
		pg_atomic_write_u64(&c->parts[p].tail, ((uint64) SIFT_RADIX_NO_CHUNK << 32) | SIFT_RADIX_PAYLOAD);
		pg_atomic_write_u64(&c->parts[p].est_rows, 0);
		pg_atomic_write_u32(&c->parts[p].spilled, 0);
		c->parts[p].first_chunk = SIFT_RADIX_NO_CHUNK;
	}
}

static SiftParCoord *
coord_init(void *raw, dsm_segment *seg, uint32 n_parts, uint32 max_chunks, int n_workers, uint32 local_cap, Size mq_size)
{
	SiftParCoord *c = (SiftParCoord *) TYPEALIGN(SIFT_PAR_SHM_ALIGN, raw);
	const uint32 one_part_of_granules = 1;

	Assert(seg != NULL && n_workers >= 0);
	memset(c, 0, sizeof(*c));
	c->n_participants = n_workers + 1;
	c->n_parts = n_parts;
	c->radix_bits = __builtin_ctz(c->n_parts);
	c->max_chunks = max_chunks;
	c->local_cap = local_cap;
	c->mq_size = mq_size;
	c->morsel_off = COORD_HDR;
	c->chunk_pool_off = COORD_HDR + MORSEL_SPAN;
	c->mq_off = c->chunk_pool_off + (Size) SIFT_RADIX_CHUNK_SIZE * max_chunks;
	sift_morsel_init(morsel_of(c), 1, &one_part_of_granules, c->n_participants);
	pg_atomic_init_u32(&c->next_part, 0);
	pg_atomic_init_u32(&c->next_free_chunk, 0);
	pg_atomic_init_u64(&c->total_rows_scanned, 0);
	pg_atomic_init_u64(&c->total_groups_out, 0);
	for (uint32 p = 0; p < c->n_parts; p++)
	{
		pg_atomic_init_u64(&c->parts[p].tail, 0);
		pg_atomic_init_u64(&c->parts[p].est_rows, 0);
		pg_atomic_init_u32(&c->parts[p].spilled, 0);
	}
	coord_reset_parts(c);
	SharedFileSetInit(&c->spill_files, seg);
	return c;
}

SiftParCoord *
sift_par_coord_init(void *raw, dsm_segment *seg, uint32 max_chunks, int n_workers, uint32 total_granules, uint32 local_cap)
{
	SiftParCoord *c = coord_init(raw, seg, sift_radix_parts(n_workers + 1), max_chunks, n_workers, local_cap, mq_size_for(n_workers));

	Assert(n_workers >= 1);
	sift_morsel_init(morsel_of(c), 1, &total_granules, c->n_participants);
	for (int w = 0; w < n_workers; w++)
		shm_mq_set_receiver(shm_mq_create(queue_at(c, w), c->mq_size), MyProc);
	return c;
}

SiftParCoord *
sift_par_coord_init_custom(void *raw, dsm_segment *seg, uint32 n_parts_radix, uint32 max_chunks, int n_workers, uint32 local_cap)
{
	Assert(n_parts_radix >= SIFT_MIN_RADIX_PARTS && n_parts_radix <= SIFT_MAX_RADIX_PARTS && (n_parts_radix & (n_parts_radix - 1)) == 0);
	return coord_init(raw, seg, n_parts_radix, max_chunks, n_workers, local_cap, 0);
}

void
sift_par_coord_reset(SiftParCoord *c)
{
	const uint32 one_part_of_granules = 1;

	SharedFileSetDeleteAll(&c->spill_files);
	c->rescan_gen++;
	coord_reset_parts(c);
	sift_morsel_init(morsel_of(c), 1, &one_part_of_granules, c->n_participants);
}

bool
sift_par_coord_spilled(SiftParCoord *c)
{
	return sift_par_coord_parts_spilled(c) > 0;
}

uint32
sift_par_coord_parts_spilled(SiftParCoord *c)
{
	uint32		n = 0;

	for (uint32 p = 0; p < c->n_parts; p++)
		n += pg_atomic_read_u32(&c->parts[p].spilled) != 0;
	return n;
}

void
sift_par_worker_attach_custom(SiftParCoord *c, dsm_segment *seg)
{
	SharedFileSetAttach(&c->spill_files, seg);
}

shm_mq_handle *
sift_par_worker_attach(SiftParCoord *c, dsm_segment *seg)
{
	shm_mq	   *mq = queue_at(c, ParallelWorkerNumber);

	SharedFileSetAttach(&c->spill_files, seg);
	shm_mq_set_sender(mq, MyProc);
	return shm_mq_attach(mq, seg, NULL);
}

shm_mq_handle **
sift_par_leader_queues(SiftParCoord *c, dsm_segment *seg, ParallelContext *pcxt)
{
	shm_mq_handle **h = palloc(sizeof(shm_mq_handle *) * Max(pcxt->nworkers_launched, 1));

	for (int w = 0; w < pcxt->nworkers_launched; w++)
		h[w] = shm_mq_attach(queue_at(c, w), seg, pcxt->worker[w].bgwhandle);
	return h;
}

int
sift_par_participant(void)
{
	return ParallelWorkerNumber + 1;
}

bool
sift_par_next_morsel(SiftParCoord *c, uint32 *first, uint32 *n)
{
	SiftMorsel	m;

	if (!sift_morsel_claim(morsel_of(c), (uint32) sift_par_participant(), &m))
		return false;
	*first = m.first;
	*n = m.n;
	return true;
}

static uint32
barrier_event(void)
{
	static uint32 ev = 0;

	if (ev == 0)
		ev = WaitEventExtensionNew("SiftRadixBarrier");
	return ev;
}

int
sift_radix_join(SiftParCoord *c)
{
	return BarrierAttach(&c->phase_barrier);
}

void
sift_radix_scan_done(SiftParCoord *c)
{
	BarrierArriveAndWait(&c->phase_barrier, barrier_event());
}

void
sift_radix_leave(SiftParCoord *c)
{
	BarrierDetach(&c->phase_barrier);
}

bool
sift_par_next_part(SiftParCoord *c, uint32 *part)
{
	*part = pg_atomic_fetch_add_u32(&c->next_part, 1);
	return *part < c->n_parts;
}

/* ---- Phase 1: the local table and the writes into the partitions ---- */

struct SiftRadixLocal
{
	SiftParCoord *coord;
	const SiftRadixShape *shape;
	SiftSaltedHT ht;
	uint32		dist_elems;
	uint8	   *stage;			/* n_parts * SIFT_RADIX_STAGE */
	uint32	   *stage_used;
	uint32	   *stage_items;
	uint64	   *part_items;		/* the records pushed to each partition: added to est_rows when the scan is done */
	uint32		pool_next;		/* the chunks of the pool this participant took and has not used: [pool_next, pool_end) */
	uint32		pool_end;
	BufFile   **spill;
	SiftRecBuf	rec;			/* the record being built */
	bool		has_ht;			/* false: the caller keeps the groups (sift_radix_local_new_raw) */
};

SiftRadixLocal *
sift_radix_local_new(SiftParCoord *c, const SiftRadixShape *shape)
{
	SiftRadixLocal *l = palloc0(sizeof(SiftRadixLocal));

	l->coord = c;
	l->shape = shape;
	l->has_ht = true;
	sift_ht_init_ex(&l->ht, c->local_cap, shape->wide_key, shape->ext_stride, shape->ext_dist_off);
	l->stage = palloc(sizeof(uint8) * SIFT_RADIX_STAGE * c->n_parts);
	l->stage_used = palloc0(sizeof(uint32) * c->n_parts);
	l->stage_items = palloc0(sizeof(uint32) * c->n_parts);
	l->part_items = palloc0(sizeof(uint64) * c->n_parts);
	l->spill = palloc0(sizeof(BufFile *) * c->n_parts);
	return l;
}

SiftRadixLocal *
sift_radix_local_new_raw(SiftParCoord *c)
{
	SiftRadixLocal *l = palloc0(sizeof(SiftRadixLocal));

	l->coord = c;
	l->stage = palloc(sizeof(uint8) * SIFT_RADIX_STAGE * c->n_parts);
	l->stage_used = palloc0(sizeof(uint32) * c->n_parts);
	l->stage_items = palloc0(sizeof(uint32) * c->n_parts);
	l->part_items = palloc0(sizeof(uint64) * c->n_parts);
	l->spill = palloc0(sizeof(BufFile *) * c->n_parts);
	return l;
}

#define TAIL(chunk, used)	(((uint64) (chunk) << 32) | (used))
#define TAIL_CHUNK(t)		((uint32) ((t) >> 32))
#define TAIL_USED(t)		((uint32) (t))

/*
 * The next chunk of the pool: one of those the participant took with its last fetch-add, or a new batch of them when the pool is wide enough that the ones
 * it leaves unused are a small share of it. SIFT_RADIX_NO_CHUNK when the pool has none.
 */
static uint32
take_chunk(SiftRadixLocal *l)
{
	SiftParCoord *c = l->coord;
	uint32		batch;

	if (l->pool_next == l->pool_end)
	{
		batch = c->max_chunks >= (uint32) SIFT_RADIX_BATCH_POOL * c->n_participants ? SIFT_RADIX_CHUNK_BATCH : 1;
		l->pool_next = pg_atomic_fetch_add_u32(&c->next_free_chunk, batch);
		l->pool_end = l->pool_next + batch;
	}
	return l->pool_next < c->max_chunks ? l->pool_next++ : SIFT_RADIX_NO_CHUNK;
}

/* Takes the next chunk of the pool for the partition, whose reservation of len bytes crossed the end of the chunk `cur`: false when the pool has none */
static bool
install_chunk(SiftRadixLocal *l, SiftRadixPart *part, uint32 cur, uint32 len, uint32 *chunk)
{
	SiftParCoord *c = l->coord;
	const uint32 idx = take_chunk(l);
	SiftRadixChunk *ch;

	if (idx == SIFT_RADIX_NO_CHUNK)
	{
		pg_atomic_exchange_u64(&part->tail, TAIL(SIFT_RADIX_SPILL_CHUNK, SIFT_RADIX_PAYLOAD));
		return false;
	}
	ch = chunk_at(c, idx);
	ch->next_chunk = SIFT_RADIX_NO_CHUNK;
	pg_atomic_init_u64(&ch->commit, 0);
	if (cur == SIFT_RADIX_NO_CHUNK)
		part->first_chunk = idx;
	else
		chunk_at(c, cur)->next_chunk = idx;
	pg_atomic_exchange_u64(&part->tail, TAIL(idx, len));
	*chunk = idx;
	return true;
}

/*
 * Reserves len bytes of the last chunk of the partition: false when its blocks go to the files. The tail word gets len added
 * whatever the room: the reservations that fit tile the payload from its start, and the first that does not (it started within the
 * payload, its end is past it) installs the next chunk. The ones after it started past the payload and wait for the tail to leave
 * the chunk, then reserve again.
 */
static bool
reserve(SiftRadixLocal *l, SiftRadixPart *part, uint32 len, uint32 *chunk, uint32 *at)
{
	for (;;)
	{
		uint64		t = pg_atomic_read_u64(&part->tail);
		uint32		cur,
					used;

		if (TAIL_CHUNK(t) == SIFT_RADIX_SPILL_CHUNK)
			return false;
		t = pg_atomic_fetch_add_u64(&part->tail, len);
		cur = TAIL_CHUNK(t);
		used = TAIL_USED(t);
		if (cur == SIFT_RADIX_SPILL_CHUNK)
			return false;
		if (used + len <= SIFT_RADIX_PAYLOAD)
		{
			*chunk = cur;
			*at = used;
			return true;
		}
		if (used <= SIFT_RADIX_PAYLOAD)
		{
			*at = 0;
			return install_chunk(l, part, cur, len, chunk);
		}
		while (TAIL_CHUNK(pg_atomic_read_u64(&part->tail)) == cur)
		{
			SPIN_DELAY();
			CHECK_FOR_INTERRUPTS();
		}
	}
}

static void
spill_block(SiftRadixLocal *l, uint32 p, const void *block, uint32 len)
{
	SiftRadixPart *part = &l->coord->parts[p];

	if (l->spill[p] == NULL)
	{
		char		name[32];

		snprintf(name, sizeof(name), "g%u_p%u_w%d", l->coord->rescan_gen, p, sift_par_participant());
		l->spill[p] = BufFileCreateFileSet(&l->coord->spill_files.fs, name);
	}
	BufFileWrite(l->spill[p], &(uint32) {len}, sizeof(uint32));
	BufFileWrite(l->spill[p], block, len);
	if (pg_atomic_read_u32(&part->spilled) == 0)
		pg_atomic_write_u32(&part->spilled, 1);
}

/* Appends a block of records to the chunks of the partition, or to the file of the participant when no chunk is left */
static void
append_block(SiftRadixLocal *l, uint32 p, const void *block, uint32 len, uint32 n_items)
{
	SiftParCoord *c = l->coord;
	SiftRadixPart *part = &c->parts[p];
	SiftRadixChunk *ch;
	uint32		idx,
				at;

	if (len > SIFT_RADIX_PAYLOAD)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a group key of an aggregate is too long"),
						errdetail("A group with its key and state must fit in a chunk of %d bytes of the shared partitions.", (int) SIFT_RADIX_PAYLOAD),
						errhint("Group by a shorter key.")));
	l->part_items[p] += n_items;
	if (!reserve(l, part, len, &idx, &at))
	{
		spill_block(l, p, block, len);
		return;
	}
	ch = chunk_at(c, idx);
	memcpy((char *) (ch + 1) + at, block, len);
	pg_atomic_fetch_add_u64(&ch->commit, SIFT_RADIX_COMMIT(n_items, len));
}

static void
stage_flush(SiftRadixLocal *l, uint32 p)
{
	if (l->stage_used[p] == 0)
		return;
	append_block(l, p, l->stage + (size_t) p * SIFT_RADIX_STAGE, l->stage_used[p], l->stage_items[p]);
	l->stage_used[p] = 0;
	l->stage_items[p] = 0;
}

static void
put_record(SiftRadixLocal *l, uint32 p, const void *rec, uint32 len)
{
	if (len > SIFT_RADIX_STAGE)
	{
		stage_flush(l, p);
		append_block(l, p, rec, len, 1);
		return;
	}
	if (l->stage_used[p] + len > SIFT_RADIX_STAGE)
		stage_flush(l, p);
	memcpy(l->stage + (size_t) p * SIFT_RADIX_STAGE + l->stage_used[p], rec, len);
	l->stage_used[p] += len;
	l->stage_items[p]++;
}

void
sift_radix_put(SiftRadixLocal *l, uint64 hash, const void *rec, uint32 len)
{
	put_record(l, SIFT_HASH_RADIX(hash, l->coord->radix_bits), rec, len);
}

static void
local_put(void *arg, uint64 hash, const uint8 *rec, uint32 len)
{
	SiftRadixLocal *l = arg;

	put_record(l, SIFT_HASH_RADIX(hash, l->coord->radix_bits), rec, len);
}

static void
local_flush(SiftRadixLocal *l)
{
	sift_rec_flush_ht(&l->ht, l->shape, &l->rec, local_put, l);
	sift_ht_clear(&l->ht);
	l->dist_elems = 0;
}

static inline void
local_make_room(SiftRadixLocal *l)
{
	if (l->ht.count >= l->ht.grow_at || l->dist_elems >= SIFT_PAR_LOCAL_DIST_MAX)
		local_flush(l);
}

static SiftAggSlot64 *
local_new_group(SiftRadixLocal *l, SiftAggSlot64 *s, bool found)
{
	if (!found && l->shape->init)
		l->shape->init(l->shape, s, l->ht.ext_stride ? sift_ht_ext(&l->ht, s) : NULL);
	return s;
}

SiftAggSlot64 *
sift_radix_local_upsert(SiftRadixLocal *l, uint64 hash, const SiftInlineKey16 *key, bool *found)
{
	SiftAggSlot64 *s;

	local_make_room(l);
	s = sift_ht_upsert(&l->ht, hash, key, found);
	return local_new_group(l, s, *found);
}

SiftAggSlot64 *
sift_radix_local_upsert_wide(SiftRadixLocal *l, uint64 hash, const void *key, uint32 len, uint64 nulls, bool *found)
{
	SiftAggSlot64 *s;

	local_make_room(l);
	s = sift_ht_upsert_wide(&l->ht, hash, key, len, nulls, found);
	return local_new_group(l, s, *found);
}

uint8 *
sift_radix_local_ext(SiftRadixLocal *l, SiftAggSlot64 *slot)
{
	return sift_ht_ext(&l->ht, slot);
}

static SiftDistSet *
local_dist_set(SiftRadixLocal *l, SiftAggSlot64 *slot)
{
	SiftDistSet **set = (SiftDistSet **) (sift_ht_ext(&l->ht, slot) + l->shape->ext_dist_off);

	if (*set == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(l->ht.cxt);

		*set = sift_dist_new();
		MemoryContextSwitchTo(old);
	}
	return *set;
}

void
sift_radix_local_dist_add(SiftRadixLocal *l, SiftAggSlot64 *slot, uint64 v)
{
	l->dist_elems += sift_dist_add(local_dist_set(l, slot), v);
}

/* A value counts as the 8 byte words of its entry (its header, its bytes), as a uint64 value counts as one */
void
sift_radix_local_dist_add_bytes(SiftRadixLocal *l, SiftAggSlot64 *slot, const void *bytes, uint32 len)
{
	if (sift_dist_add_bytes(local_dist_set(l, slot), bytes, len))
		l->dist_elems += 2 + len / 8;
}

void
sift_radix_local_finish(SiftRadixLocal *l)
{
	if (l->has_ht)
		local_flush(l);
	for (uint32 p = 0; p < l->coord->n_parts; p++)
	{
		stage_flush(l, p);
		if (l->part_items[p])
			pg_atomic_fetch_add_u64(&l->coord->parts[p].est_rows, l->part_items[p]);
		if (l->spill[p])
			BufFileClose(l->spill[p]);
	}
	if (l->has_ht)
		sift_ht_destroy(&l->ht);
	pfree(l->stage);
	pfree(l->stage_used);
	pfree(l->stage_items);
	pfree(l->part_items);
	pfree(l->spill);
	if (l->rec.p)
		pfree(l->rec.p);
	pfree(l);
}

/* ---- Phase 2: the merge of a partition ---- */

static void
check_merge_bytes(const SiftRecMerge *m)
{
	const uint64 bytes = sift_ht_bytes(m->ht) + m->dist_bytes;

	if (bytes > get_hash_memory_limit())
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the groups of a partition take more than %lu bytes", (unsigned long) get_hash_memory_limit()),
						errhint("Raise work_mem or hash_mem_multiplier.")));
}

static void
merge_block(SiftRecMerge *m, const uint8 *block, uint32 len)
{
	for (uint32 off = 0; off < len; off += sift_rec_len(block + off))
		sift_rec_merge(m, block + off);
	check_merge_bytes(m);
}

void
sift_radix_scan_part(SiftParCoord *c, uint32 part, SiftRadixBlockFn fn, void *arg)
{
	SiftRadixPart *pt = &c->parts[part];
	uint8	   *buf = NULL;

	for (uint32 i = pt->first_chunk; i != SIFT_RADIX_NO_CHUNK;)
	{
		SiftRadixChunk *ch = chunk_at(c, i);

		fn(arg, (const uint8 *) (ch + 1), SIFT_RADIX_COMMITTED(pg_atomic_read_u64(&ch->commit)));
		i = ch->next_chunk;
	}
	if (pg_atomic_read_u32(&pt->spilled) == 0)
		return;
	buf = palloc(SIFT_RADIX_PAYLOAD);
	for (uint32 w = 0; w < c->n_participants; w++)
	{
		char		name[48];
		BufFile    *f;
		uint32		len;

		snprintf(name, sizeof(name), "g%u_p%u_w%u", c->rescan_gen, part, w);
		if ((f = BufFileOpenFileSet(&c->spill_files.fs, name, O_RDONLY, true)) == NULL)
			continue;
		while (BufFileReadMaybeEOF(f, &len, sizeof(len), true) != 0)
		{
			CHECK_FOR_INTERRUPTS();
			BufFileReadExact(f, buf, len);
			fn(arg, buf, len);
		}
		BufFileClose(f);
		BufFileDeleteFileSet(&c->spill_files.fs, name, true);
	}
	pfree(buf);
}

static void
merge_block_fn(void *arg, const uint8 *block, uint32 len)
{
	merge_block(arg, block, len);
}

void
sift_radix_merge_part(SiftParCoord *c, uint32 part, const SiftRadixShape *shape, SiftSaltedHT *ht)
{
	SiftRadixPart *pt = &c->parts[part];
	SiftRecMerge m = {.shape = shape, .ht = ht};
	const uint64 fit = get_hash_memory_limit() / 4 / (sizeof(SiftAggSlot64) + 1 + shape->ext_stride);

	sift_ht_init_ex(ht, (uint32) Max(Min(Min(pg_atomic_read_u64(&pt->est_rows), 65536), fit), SIFT_HT_MIN_SLOTS), shape->wide_key,
				shape->ext_stride, shape->ext_dist_off);
	sift_radix_scan_part(c, part, merge_block_fn, &m);
}

/* ---- the output of a participant ---- */

#define SINK_BUF	8192

struct SiftRadixSink
{
	shm_mq_handle *mqh;
	SiftRadixConsume consume;
	void	   *arg;
	uint8		buf[SINK_BUF];
	uint32		used;
};

SiftRadixSink *
sift_radix_sink_new(shm_mq_handle *mqh, SiftRadixConsume consume, void *arg)
{
	SiftRadixSink *s = palloc0(sizeof(SiftRadixSink));

	s->mqh = mqh;
	s->consume = consume;
	s->arg = arg;
	return s;
}

static void
sink_send(SiftRadixSink *s, const void *data, Size len)
{
	if (shm_mq_send(s->mqh, len, data, false, true) == SHM_MQ_DETACHED)
		ereport(ERROR, (errcode(ERRCODE_CONNECTION_FAILURE), errmsg("lost connection to the parallel aggregate leader")));
}

static void
sink_flush(SiftRadixSink *s)
{
	if (s->used)
		sink_send(s, s->buf, s->used);
	s->used = 0;
}

void
sift_radix_sink_put(SiftRadixSink *s, const void *rec, uint32 len)
{
	const uint32 padded = TYPEALIGN(8, len) + sizeof(uint32) * 2;

	if (s->mqh == NULL)
	{
		s->consume(s->arg, rec, len);
		return;
	}
	if (s->used + padded > SINK_BUF)
		sink_flush(s);
	if (padded > SINK_BUF)
	{
		uint8	   *one = palloc0(padded);

		memcpy(one, &len, sizeof(len));
		memcpy(one + 8, rec, len);
		sink_send(s, one, padded);
		pfree(one);
		return;
	}
	memset(s->buf + s->used, 0, padded);
	memcpy(s->buf + s->used, &len, sizeof(len));
	memcpy(s->buf + s->used + 8, rec, len);
	s->used += padded;
}

void
sift_radix_sink_finish(SiftRadixSink *s)
{
	if (s->mqh)
		sink_flush(s);
	pfree(s);
}

static uint32
drain_event(void)
{
	static uint32 ev = 0;

	if (ev == 0)
		ev = WaitEventExtensionNew("SiftRadixDrain");
	return ev;
}

/* The records of one message of a worker */
static void
consume_message(const void *data, Size nbytes, SiftRadixConsume consume, void *arg)
{
	for (Size off = 0; off < nbytes;)
	{
		uint32		len;

		memcpy(&len, (const char *) data + off, sizeof(len));
		consume(arg, (const char *) data + off + 8, len);
		off += TYPEALIGN(8, len) + 8;
	}
}

void
sift_radix_drain(shm_mq_handle **mqh, int n, SiftRadixConsume consume, void *arg)
{
	bool	   *done = palloc0(sizeof(bool) * Max(n, 1));
	int			left = n;

	while (left > 0)
	{
		bool		progressed = false;

		for (int w = 0; w < n; w++)
		{
			Size		nbytes;
			void	   *data;
			shm_mq_result r;

			if (done[w])
				continue;
			r = shm_mq_receive(mqh[w], &nbytes, &data, true);
			if (r == SHM_MQ_DETACHED)
			{
				done[w] = true;
				left--;
			}
			else if (r == SHM_MQ_SUCCESS)
			{
				consume_message(data, nbytes, consume, arg);
				progressed = true;
			}
		}
		if (left > 0 && !progressed)
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, drain_event());
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
	pfree(done);
}
