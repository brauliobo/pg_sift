/*
 * agg/v6/v6_par_group.c - the engine of the parallel SiftAggV6 (v6_par.h) over the groups of group.c: the state a participant folds its morsels into is a SiftV6Group, its groups leave as records
 * and the records of other participants merge into it.
 *
 * A record is a SiftV6RecHdr and the engine's payload. A group of a set of keys is its accumulators and, for a wide key, the bytes of the key after them; the state of a set of no keys is its
 * accumulators (V6REC_PLAIN); a row of a COUNT(DISTINCT) is the bytes of its key (V6REC_DIST), which the owner of the hash adds to the exact set of its own state. The hash of a record is the one the
 * group has in its table (of the bytes of the value, for a set of no keys), so every record of a group (its accumulators and its distinct values) reaches the one partition that owns it and the
 * counts of the group are final there. A participant that scans adds the values of its rows to an exact set of its own and hands the keys it holds over as records when the set is full or the scan
 * is done: a value goes once for every time the set was emptied, not once for every row, which is exact because the partition counts it once.
 */
#include "postgres.h"

#include "agg/v6/group_int.h"
#include "agg/v6/v6_par_pg.h"
#include "core/memtrack.h"

/* MEM[MEM-AGG-17]: parallel worker SiftDistinct (the seen set is bounded by the hand over of par_local_load counting its bytes, not by a budget that would spill it; the heap goes back to the system when a worker state ends) */
#define SEEN_NO_BUDGET		(UINT64CONST(1) << 40)
#define SEEN_ENTRY_BYTES	64		/* what an entry of the set of a participant takes, to tell when it is full */

typedef struct ParEnv
{
	const SiftV6Spec *spec;
	MemoryContext cx;
	TupleDesc	desc;
	MemoryContext tuple_cx;		/* what a finished group is made in: the per-tuple memory of the node */
	SiftV6ParDsmHdr *hdr;
	Relation	heap;
	Relation	index;
	ScanKey		keys;
	int			nkeys;
	Snapshot	snap;
	SiftV6Scan *scan;
	SiftV6ScanStats stats;
	SiftV6ScanStats seen;
	V6AccOp    *op;
	struct ParLocal *cur;
} ParEnv;

typedef struct ParLocal
{
	ParEnv	   *env;
	MemoryContext cx;
	SiftV6Group *g;
	bool		scanning;
	SiftDistinct *seen;			/* the distinct values of the rows scanned since the last hand over, by (set, aggregate) */
	uint8	   *rec;
	Size		rec_cap;
} ParLocal;

static void
combine_accs(const ParEnv *e, V6Acc *dst, const V6Acc *src)
{
	for (int j = 0; j < e->spec->naggs; j++)
		sift_v6_acc_combine(e->op[j], &dst[j], &src[j]);
}

static void
check_record_size(uint32 rlen)
{
	if (rlen > SIFT_RADIX_PAYLOAD)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a group key or a COUNT(DISTINCT) value is too large for the parallel aggregate"),
						errhint("Set max_parallel_workers_per_gather to 0 for this query.")));
}

/* The hash the group has in its table: the key of the group is in the bytes after [plen][nulls]; a set of no keys is hashed by the value */
static uint64
dist_record_hash(const ParLocal *l, uint32 set_idx, const uint8 *bytes, uint32 len)
{
	const V6Set *set = &l->g->set[set_idx];
	uint32		plen;
	uint64		nulls;

	if (set->nk == 0)
		return sift_aggk6_hash1(sift_aggk6_hash_bytes(bytes, len));
	memcpy(&plen, bytes, sizeof(plen));
	memcpy(&nulls, bytes + sizeof(plen), sizeof(nulls));
	return v6g_group_hash(set, bytes + sizeof(plen) + sizeof(nulls), plen - (uint32) sizeof(nulls), nulls);
}

static uint8 *
record_room(ParLocal *l, Size len)
{
	if (len > l->rec_cap)
	{
		l->rec_cap = Max(len, 2 * l->rec_cap);
		l->rec = l->rec == NULL ? MemoryContextAlloc(l->cx, l->rec_cap) : repalloc(l->rec, l->rec_cap);
	}
	return l->rec;
}

/* A distinct value as the record its owner adds to its exact set: the header, the key bytes, the padding; built in the one buffer of the participant and put as it is */
static void
put_dist_record(ParLocal *l, uint32 set_idx, uint32 slot, const void *bytes, uint32 len, uint64 hash, SiftV6RecPut put, void *arg)
{
	const uint32 rlen = (uint32) MAXALIGN(sizeof(SiftV6RecHdr) + len);
	SiftV6RecHdr h = {.len = rlen,.set_idx = (uint16) set_idx,.flags = V6REC_DIST,.hash64 = hash};
	uint8	   *rec;

	check_record_size(rlen);
	rec = record_room(l, rlen);
	h.key.i64[0] = slot;
	h.key.i64[1] = len;
	memcpy(rec, &h, sizeof(h));
	memcpy(rec + sizeof(h), bytes, len);
	memset(rec + sizeof(h) + len, 0, rlen - sizeof(h) - len);
	put(arg, rec, rlen);
}

/* A row of a COUNT(DISTINCT): a set of no keys keeps the hash that routes its value (every later step takes it from the record), a grouped one hashes the key bytes alone */
static void
park_dist(void *arg, uint32 set_idx, uint32 slot, const void *bytes, uint32 len)
{
	ParLocal   *l = arg;
	const uint32 id = set_idx * SIFT_PAR_MAX_DIST + slot;

	if (l->g->set[set_idx].nk == 0)
		sift_distinct_add_prehashed(l->seen, id, bytes, len, dist_record_hash(l, set_idx, bytes, len));
	else
		sift_distinct_add(l->seen, id, bytes, len);
}

typedef struct ExportArg
{
	ParLocal   *l;
	SiftV6RecPut put;
	void	   *arg;
} ExportArg;

static void
export_seen(void *arg, uint32 id, const void *bytes, uint32 len, uint64 hash)
{
	const ExportArg *x = arg;
	const uint32 set_idx = id / SIFT_PAR_MAX_DIST;

	put_dist_record(x->l, set_idx, id % SIFT_PAR_MAX_DIST, bytes, len, x->l->g->set[set_idx].nk == 0 ? hash : dist_record_hash(x->l, set_idx, bytes, len), x->put, x->arg);
}

static SiftDistinct *
seen_new(ParLocal *l)
{
	return sift_distinct_create(l->cx, SEEN_NO_BUDGET, 0);
}

static void
batch_to_local(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	sift_v6_group_update(((ParEnv *) arg)->cur->g, bs, keyvec, argvec);
}

static void
open_scan(ParEnv *e)
{
	const SiftV6ParDsmHdr *h = e->hdr;
	MemoryContext old = MemoryContextSwitchTo(e->cx);

	e->scan = sift_v6_scan_begin(e->spec, e->heap, e->index, e->keys, e->nkeys, e->snap, &e->stats, batch_to_local, e);
	sift_v6_scan_morsel_begin(e->scan, (const uint64 *) ((const char *) h + h->staged_dead_off), h->n_staged_dead);
	MemoryContextSwitchTo(old);
}

static void *
par_local_new(void *env, bool whole)
{
	ParEnv	   *e = env;
	ParLocal   *l = MemoryContextAllocZero(e->cx, sizeof(ParLocal));

	l->env = e;
	l->cx = sift_mem_allocset_create(e->cx, "sift agg v6 parallel state");
	l->g = sift_v6_group_new(e->spec, l->cx, e->desc);
	if (!whole)
		for (int s = 0; s < l->g->nsets; s++)
			l->g->set[s].plain_row = false;
	return l;
}

static void
par_local_free(void *local)
{
	ParLocal   *l = local;

	const Size	peak = MemoryContextMemAllocated(l->cx, true);

	sift_v6_group_free(l->g);
	MemoryContextDelete(l->cx);
	sift_mem_release_os(peak);
}

static void
par_scan_morsel(void *local, const SiftMorsel *m, SiftV6ParWorkerStats *st)
{
	ParLocal   *l = local;
	ParEnv	   *e = l->env;
	const SiftParPartRef *refs = (const SiftParPartRef *) ((const char *) e->hdr + e->hdr->parts_off);

	if (e->scan == NULL)
		open_scan(e);
	if (!l->scanning)
	{
		MemoryContext old = MemoryContextSwitchTo(l->cx);

		l->scanning = true;
		l->seen = seen_new(l);
		sift_v6_group_scan_sink(l->g, park_dist, l);
		MemoryContextSwitchTo(old);
	}
	e->cur = l;
	sift_v6_scan_morsel(e->scan, &refs[m->part], m->first, m->n);
	st->rows_scanned += e->stats.rows - e->seen.rows;
	st->grans_scanned += e->stats.batches - e->seen.batches;
	st->heap_vet_rows += e->stats.heap_vet_rows - e->seen.heap_vet_rows;
	st->heap_rows_dead += e->stats.heap_rows_dead - e->seen.heap_rows_dead;
	e->seen = e->stats;
}

static uint64
par_local_load(const void *local)
{
	const ParLocal *l = local;
	uint64		n = l->scanning ? sift_distinct_bytes(l->seen) / SEEN_ENTRY_BYTES + v6g_dist_dict_load(l->g) : 0;

	for (int s = 0; s < l->g->nsets; s++)
		n += l->g->set[s].nk > 0 ? l->g->set[s].ht.count : 0;
	return n;
}

static void
export_plain(ParLocal *l, int s, SiftV6RecPut put, void *arg)
{
	V6Set	   *set = &l->g->set[s];
	const Size	accs = (Size) l->env->spec->naggs * sizeof(V6Acc);
	uint8	   *rec = record_room(l, sizeof(SiftV6RecHdr) + accs);
	SiftV6RecHdr h = {.len = (uint32) (sizeof(SiftV6RecHdr) + accs),.set_idx = (uint16) s,.flags = V6REC_PLAIN};

	memcpy(rec, &h, sizeof(h));
	memcpy(rec + sizeof(h), set->plain, accs);
	put(arg, rec, h.len);
	memset(set->plain, 0, accs);
}

static void
export_groups(ParLocal *l, int s, SiftV6RecPut put, void *arg)
{
	V6Set	   *set = &l->g->set[s];
	const Size	accs = (Size) l->env->spec->naggs * sizeof(V6Acc),
				base = sizeof(SiftV6RecHdr) + accs;
	const SiftAggSlot64 *slot;

	SIFT_HT_FOREACH(&set->ht, slot)
	{
		uint32		klen = 0;
		const uint8 *kbytes = set->wide ? sift_ht_wide_key(&set->ht, slot, &klen) : NULL;
		const uint32 rlen = (uint32) MAXALIGN(base + klen);
		uint8	   *rec;
		SiftV6RecHdr h = {.len = rlen,.set_idx = (uint16) s,.flags = set->wide ? V6REC_WIDE : 0,.hash64 = slot->hash64,.key = slot->key};

		check_record_size(rlen);
		rec = record_room(l, rlen);
		if (set->wide)
		{
			h.key.i64[0] = (int64) slot->key.wide.key_nulls;
			h.key.i64[1] = klen;
			memcpy(rec + base, kbytes, klen);
			memset(rec + base + klen, 0, rlen - base - klen);
		}
		memcpy(rec, &h, sizeof(h));
		memcpy(rec + sizeof(h), sift_ht_ext(&set->ht, slot), accs);
		put(arg, rec, rlen);
	}
	sift_ht_clear(&set->ht);
}

static void
par_local_export(void *local, SiftV6RecPut put, void *arg)
{
	ParLocal   *l = local;

	if (l->scanning)
	{
		ExportArg	x = {l, put, arg};

		v6g_dist_dict_drain(l->g, park_dist, l);
		sift_distinct_foreach_hashed(l->seen, export_seen, &x);
		sift_distinct_reset(l->seen);
	}
	for (int s = 0; s < l->g->nsets; s++)
		if (l->g->set[s].nk == 0)
			export_plain(l, s, put, arg);
		else
			export_groups(l, s, put, arg);
}

static void
par_local_merge(void *local, const uint8 *rec)
{
	ParLocal   *l = local;
	SiftV6Group *g = l->g;
	const SiftV6RecHdr *h = (const SiftV6RecHdr *) rec;
	V6Set	   *set = &g->set[h->set_idx];
	const V6Acc *accs = (const V6Acc *) (rec + sizeof(*h));
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	SiftAggSlot64 *slot;
	bool		found;

	if (h->flags & V6REC_DIST)
		v6g_dist_add_raw(g, set, (uint32) h->key.i64[0], rec + sizeof(*h), (uint32) h->key.i64[1], h->hash64);
	else if (h->flags & V6REC_PLAIN)
	{
		combine_accs(l->env, set->plain, accs);
		set->plain_row = true;
	}
	else
	{
		if (h->flags & V6REC_WIDE)
			slot = sift_ht_upsert_wide(&set->ht, h->hash64, rec + sizeof(*h) + (Size) l->env->spec->naggs * sizeof(V6Acc), (uint32) h->key.i64[1], (uint64) h->key.i64[0], &found);
		else
			slot = sift_ht_upsert(&set->ht, h->hash64, &h->key, &found);
		combine_accs(l->env, (V6Acc *) sift_ht_ext(&set->ht, slot), accs);
		v6g_spill_if_over(g);
	}
	MemoryContextSwitchTo(old);
}

static void
par_local_finish(void *local)
{
	sift_v6_group_finish(((ParLocal *) local)->g);
}

static bool
par_local_next(void *local, Datum *values, bool *isnull)
{
	ParLocal   *l = local;
	MemoryContext old = MemoryContextSwitchTo(l->env->tuple_cx);
	const bool	got = sift_v6_group_next(l->g, values, isnull);

	MemoryContextSwitchTo(old);
	return got;
}

static uint64
par_local_dist_count(const void *local, uint32 slot)
{
	SiftV6Group *g = ((const ParLocal *) local)->g;
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	const uint64 n = v6g_dist_count(g, slot);

	MemoryContextSwitchTo(old);
	return n;
}

static void
par_local_reset(void *local)
{
	sift_v6_group_recycle(((ParLocal *) local)->g);
}

static void
par_local_set_dist(void *local, uint32 slot, uint64 total)
{
	SiftV6Group *g = ((ParLocal *) local)->g;

	g->set[0].plain[g->dagg[slot]].v0 = total;
}

void
sift_v6_par_engine_make(SiftV6ParEngine *eng, const SiftV6Spec *spec, MemoryContext cx, MemoryContext tuple_cx, TupleDesc desc, SiftV6ParDsmHdr *hdr, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap)
{
	ParEnv	   *e = MemoryContextAllocZero(cx, sizeof(ParEnv));

	e->spec = spec;
	e->cx = cx;
	e->desc = desc;
	e->tuple_cx = tuple_cx;
	e->hdr = hdr;
	e->heap = heap;
	e->index = index;
	e->keys = keys;
	e->nkeys = nkeys;
	e->snap = snap;
	e->op = MemoryContextAlloc(cx, Max(spec->naggs, 1) * sizeof(V6AccOp));
	for (int j = 0; j < spec->naggs; j++)
		e->op[j] = v6g_acc_op(&spec->agg[j]);
	*eng = (SiftV6ParEngine) {.env = e,.local_new = par_local_new,.local_free = par_local_free,.scan_morsel = par_scan_morsel,.local_load = par_local_load,
		.local_export = par_local_export,.local_merge = par_local_merge,.local_finish = par_local_finish,.local_next = par_local_next,
		.local_dist_count = par_local_dist_count,.local_set_dist = par_local_set_dist,.local_reset = par_local_reset};
}

void
sift_v6_par_engine_end(SiftV6ParEngine *eng)
{
	ParEnv	   *e = eng->env;

	if (e->scan != NULL)
		sift_v6_scan_end(e->scan);
	e->scan = NULL;
}
