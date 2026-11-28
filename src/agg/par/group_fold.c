/*
 * agg/par/group_fold.c - the engine of the parallel SiftAggV6 (v6_par.h) over the groups of group.c: the state a participant folds its morsels into is a SiftV6Group, its groups leave as records
 * and the records of other participants merge into it.
 *
 * A record is a SiftV6RecHdr and the engine's payload. A group of a set of keys is its accumulators and, for a wide key, the bytes of the key after them; the state of a set of no keys is its
 * accumulators (V6REC_PLAIN); a row of a COUNT(DISTINCT) is the bytes of its key (V6REC_DIST), which the owner of the hash adds to the exact set of its own state. The hash of a record is the one the
 * group has in its table (of the bytes of the value, for a set of no keys), so every record of a group (its accumulators and its distinct values) reaches the one partition that owns it and the
 * counts of the group are final there. A participant that scans adds the values of its rows to an exact set of its own and hands the keys it holds over as records when the set is full or the scan
 * is done: a value goes once for every time the set was emptied, not once for every row, which is exact because the partition counts it once.
 */
#include "postgres.h"

#include "agg/group/int.h"
#include "agg/gen/gen.h"
#include "agg/par/dist.h"
#include "agg/par/pg.h"
#include "agg/topn/topk_run.h"
#include "core/memtrack.h"

/* MEM[MEM-AGG-17]: parallel worker SiftDistinct (the seen set is bounded by the hand over of par_local_load counting its bytes, not by a budget that would spill it; the heap goes back to the system when a worker state ends) */
#define SEEN_NO_BUDGET		(UINT64CONST(1) << 40)
#define SEEN_ENTRY_BYTES	64		/* what an entry of the set of a participant takes, to tell when it is full */

/* What a group of a table of a participant takes while the table is as full as it may be: two slots of the table (its load is at most 7/8 of a power of 2) and the state block */
#define SCAN_GROUP_BYTES(spec)	(2 * (sizeof(SiftAggSlot64) + 1) + (Size) Max((spec)->naggs, 1) * sizeof(V6Acc))

typedef struct ParEnv
{
	const SiftV6Spec *spec;
	SiftV6Spec	part_spec;		/* spec for the states of a partition: presized for its share of the groups of the query */
	SiftV6Spec	scan_spec;		/* spec for the states a participant folds its rows into: presized for the groups that fit its share of the memory */
	MemoryContext cx;
	TupleDesc	desc;
	MemoryContext tuple_cx;		/* what a finished group is made in: the per-tuple memory of the node */
	SiftV6ParDsmHdr *hdr;
	const SiftV6ScanSrc *sources;
	struct SiftV6StarRun *star;	/* the lookup tables of the joins (NULL: none) */
	uint32		nsources;
	Snapshot	snap;
	SiftV6Scan **scan;			/* of each source, begun by the first morsel of it */
	SiftV6ScanStats stats;
	SiftV6ScanStats seen;
	V6AccOp    *op;
	struct ParLocal *cur;
	void		(*relief) (void *arg);	/* what the scan runs after a batch: the participant hands over what it holds over its share */
	void	   *relief_arg;
	bool		lean;			/* the COUNT(DISTINCT) aggregates are those of v6_par_dist.h */
	bool		has_tail;		/* a record of a group may have bytes after its accumulators (tail_build) */
} ParEnv;

static inline bool
is_text_ext(const SiftV6Agg *a)
{
	return v6g_is_ext(a) && sift_v6_class(a->arg.typid) == SV6C_TEXT;
}

typedef struct ParLocal
{
	ParEnv	   *env;
	MemoryContext cx;
	SiftV6Group *g;
	bool		scanning;
	SiftDistinct *seen;			/* the distinct values of the rows scanned since the last hand over, by (set, aggregate) */
	uint8	   *rec;
	Size		rec_cap;
	SiftV6ParDistOut *dist_out;	/* a participant that scans: its COUNT(DISTINCT) rows (v6_par_dist.h) */
	SiftV6ParDist *dist;		/* the owner of a partition: the values of its COUNT(DISTINCT) aggregates (v6_par_dist.h) */
	StringInfoData tail;		/* the bytes after the accumulators of the record in hand */
} ParLocal;

/* The bytes after the accumulators of a record that its states need: the winner of a text state when it does not fit the 8 bytes of the state itself (the offsets of an arena mean nothing to another participant), and the partial state of a generic aggregate */
static void
tail_build(const ParLocal *l, const V6Acc *acc, StringInfo buf)
{
	resetStringInfo(buf);
	for (int j = 0; j < l->env->spec->naggs; j++)
	{
		const SiftV6Agg *a = &l->env->spec->agg[j];

		if (v6g_is_gen(a))
			v6_gen_state_put(l->g->gen, j, &acc[j], buf);
		else if (is_text_ext(a) && acc[j].n > 0 && !(((const SiftTextSlot *) &acc[j])->pad & SIFT_TSLOT_INLINE))
		{
			uint32		len;
			const char *w = sift_tslot_bytes(&l->g->tx, (const SiftTextSlot *) &acc[j], &len);

			appendBinaryStringInfo(buf, w, len);
		}
	}
}

/* The winner of a text state of a record: dst takes it when it is better, and counts the values the record folded */
static void
merge_text(ParLocal *l, const SiftV6Agg *a, V6Acc *dst, const V6Acc *src, const char **tail)
{
	const SiftTextSlot *s = (const SiftTextSlot *) src;
	SiftTextSlot *d = (SiftTextSlot *) dst;
	const char *bytes = (s->pad & SIFT_TSLOT_INLINE) ? (const char *) &s->v0 : *tail;
	const int64 n = d->n;

	if (s->n == 0)
		return;
	if (!(s->pad & SIFT_TSLOT_INLINE))
		*tail += s->v1;
	if (n == 0)
		sift_tslot_set(&l->g->tx, d, bytes, (uint32) s->v1);
	else
		sift_tslot_offer(&l->g->tx, d, a->kind == SV6_MIN, bytes, (uint32) s->v1, NULL, NULL);
	d->n = n + s->n;
}

static void
combine_accs(ParLocal *l, V6Acc *dst, const V6Acc *src, const char *tail)
{
	const ParEnv *e = l->env;

	for (int j = 0; j < e->spec->naggs; j++)
		if (v6g_is_gen(&e->spec->agg[j]))
			tail = v6_gen_state_take(l->g->gen, j, &dst[j], &src[j], tail);
		else if (is_text_ext(&e->spec->agg[j]))
			merge_text(l, &e->spec->agg[j], &dst[j], &src[j], &tail);
		else if (v6g_is_ext(&e->spec->agg[j]))
			v6g_ext_merge(l->g, &e->spec->agg[j], &dst[j], &src[j]);
		else
			sift_v6_acc_combine(e->op[j], &dst[j], &src[j]);
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
		l->rec_cap = sift_alloc_grow(l->rec_cap, len, "a record of a parallel aggregate");
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

	sift_v6_par_check_record_size(rlen);
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
	return sift_distinct_create(l->cx, SEEN_NO_BUDGET);
}

static void
batch_to_local(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	ParEnv	   *e = arg;

	sift_v6_group_update(e->cur->g, bs, keyvec, argvec);
	if (e->relief != NULL)
		e->relief(e->relief_arg);
}

static SiftV6Scan *
open_scan(ParEnv *e, uint32 child)
{
	const SiftV6ScanSrc *src = &e->sources[child];
	uint32		nstaged;
	const uint64 *staged = sift_v6_par_staged_dead(e->hdr, child, &nstaged);
	MemoryContext old = MemoryContextSwitchTo(e->cx);

	e->scan[child] = sift_v6_src_par(src)->begin(src->spec, src->heap, src->index, src->keys, src->nkeys, src->quals, e->snap, &e->stats, e->star, batch_to_local, e);
	sift_v6_src_par(src)->morsel_begin(e->scan[child], staged, nstaged);
	MemoryContextSwitchTo(old);
	return e->scan[child];
}

static ParLocal *
local_make(ParEnv *e, const SiftV6Spec *spec, bool whole)
{
	ParLocal   *l = MemoryContextAllocZero(e->cx, sizeof(ParLocal));
	MemoryContext old = CurrentMemoryContext;

	l->env = e;
	l->cx = sift_mem_allocset_create(e->cx, "sift agg v6 parallel state");
	l->g = sift_v6_group_new(spec, l->cx, e->desc);
	MemoryContextSwitchTo(l->cx);
	initStringInfo(&l->tail);
	MemoryContextSwitchTo(old);
	if (!whole)
		for (int s = 0; s < l->g->nsets; s++)
			l->g->set[s].plain_row = false;
	return l;
}

static void *
par_local_new(void *env, bool whole)
{
	ParEnv	   *e = env;

	return local_make(e, whole ? e->spec : &e->part_spec, whole);
}

static void *
par_scan_new(void *env)
{
	ParEnv	   *e = env;

	return local_make(e, &e->scan_spec, false);
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
par_scan_hook(void *env, void (*fn) (void *arg), void *arg)
{
	ParEnv	   *e = env;

	e->relief = fn;
	e->relief_arg = arg;
}

static void
par_scan_morsel(void *local, const SiftMorsel *m, SiftV6ParWorkerStats *st)
{
	ParLocal   *l = local;
	ParEnv	   *e = l->env;
	const SiftParPartRef *ref = (const SiftParPartRef *) ((const char *) e->hdr + e->hdr->parts_off) + m->part;
	SiftV6Scan *scan = e->scan[ref->child_idx] != NULL ? e->scan[ref->child_idx] : open_scan(e, ref->child_idx);

	if (!l->scanning)
	{
		MemoryContext old = MemoryContextSwitchTo(l->cx);

		l->scanning = true;
		if (e->lean)
			l->dist_out = sift_v6_par_dist_out_new(l->g, sift_v6_par_share(e->hdr));
		else
		{
			l->seen = seen_new(l);
			sift_v6_group_scan_sink(l->g, park_dist, l);
		}
		MemoryContextSwitchTo(old);
	}
	e->cur = l;
	sift_v6_src_par(&e->sources[ref->child_idx])->morsel(scan, l->g, ref, m->first, m->n);
	st->rows_scanned += e->stats.rows - e->seen.rows;
	st->grans_scanned += e->stats.grans - e->seen.grans;
	st->grans_pruned_zone += e->stats.grans_pruned_zone - e->seen.grans_pruned_zone;
	st->grans_pruned_skip += e->stats.grans_pruned_skip - e->seen.grans_pruned_skip;
	st->grans_preagg += e->stats.grans_preagg - e->seen.grans_preagg;
	st->grans_preagg_delta += e->stats.grans_preagg_delta - e->seen.grans_preagg_delta;
	st->heap_vet_rows += e->stats.heap_vet_rows - e->seen.heap_vet_rows;
	st->heap_rows_dead += e->stats.heap_rows_dead - e->seen.heap_rows_dead;
	e->seen = e->stats;
}

static uint64
par_local_load(const void *local)
{
	const ParLocal *l = local;
	const SiftV6Group *g = l->g;
	uint64		bytes = g->tx.cap + (g->slab != NULL ? sift_strslab_bytes(g->slab) : 0) + (g->gen != NULL ? v6_gen_bytes(g->gen) : 0) + l->rec_cap + l->tail.maxlen;

	if (l->scanning)
		bytes += l->env->lean ? sift_v6_par_dist_load(l->dist_out) : sift_distinct_bytes(l->seen) + v6g_dist_dict_load(g) * SEEN_ENTRY_BYTES;
	for (int s = 0; s < g->nsets; s++)
		bytes += g->set[s].nk > 0 ? sift_ht_bytes(&g->set[s].ht) : 0;
	return bytes;
}

/* What an export leaves: the tables at the size a spill leaves them, the text winners gone with the groups that held them, so that the next groups start from nothing and a table that was full is not full again;
 * the record and tail buffers go too, as large as the widest record of the export */
static void
release_exported(ParLocal *l)
{
	SiftV6Group *g = l->g;
	MemoryContext old = MemoryContextSwitchTo(l->cx);

	if (l->rec != NULL)
		pfree(l->rec);
	l->rec = NULL;
	l->rec_cap = 0;
	pfree(l->tail.data);
	initStringInfo(&l->tail);
	MemoryContextSwitchTo(old);
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].nk > 0)
			sift_ht_shrink_to(&g->set[s].ht, SIFT_SPILL_LOCAL_CAP);
	if (g->tx.buf != NULL)
		pfree(g->tx.buf);
	g->tx.buf = NULL;
	g->tx.cap = g->tx.used = 0;
	if (g->slab != NULL)
	{
		sift_strslab_free(g->slab);
		g->slab = sift_strslab_new(g->cx);
	}
}

static void
export_plain(ParLocal *l, int s, SiftV6RecPut put, void *arg)
{
	V6Set	   *set = &l->g->set[s];
	const Size	accs = (Size) l->env->spec->naggs * sizeof(V6Acc);
	Size		len;
	uint8	   *rec;
	SiftV6RecHdr h = {.set_idx = (uint16) s,.flags = V6REC_PLAIN};

	if (l->env->has_tail)
		tail_build(l, set->plain, &l->tail);
	len = MAXALIGN(sizeof(SiftV6RecHdr) + accs + l->tail.len);
	sift_v6_par_check_record_size((uint32) len);
	h.len = (uint32) len;
	rec = record_room(l, len);
	memcpy(rec, &h, sizeof(h));
	memcpy(rec + sizeof(h), set->plain, accs);
	memset(rec + sizeof(h) + accs, 0, len - sizeof(h) - accs);
	memcpy(rec + sizeof(h) + accs, l->tail.data, l->tail.len);
	put(arg, rec, h.len);
	memset(set->plain, 0, accs);
}

StaticAssertDecl(sizeof(SiftV6RecHdr) % 8 == 0 && sizeof(V6Acc) % 8 == 0, "a record of an inline key and no tail is a whole number of 8 bytes");

/* A group of an inline key with no tail is a record of one length, built in the room the transport gives; false when it has none that size */
static bool
export_fixed(ParLocal *l, const SiftV6RecSink *sink, const V6Set *set, int s, const SiftAggSlot64 *slot)
{
	const Size	accs = (Size) l->env->spec->naggs * sizeof(V6Acc),
				len = sizeof(SiftV6RecHdr) + accs;
	const SiftV6RecHdr h = {.len = (uint32) len,.set_idx = (uint16) s,.hash64 = slot->hash64,.key = slot->key};
	uint8	   *rec = sink->room(sink->arg, slot->hash64, (uint32) len);

	if (rec == NULL)
		return false;
	memcpy(rec, &h, sizeof(h));
	memcpy(rec + sizeof(h), sift_ht_ext(&set->ht, slot), accs);
	return true;
}

static void
export_groups(ParLocal *l, int s, const SiftV6RecSink *sink)
{
	V6Set	   *set = &l->g->set[s];
	const Size	accs = (Size) l->env->spec->naggs * sizeof(V6Acc),
				base = sizeof(SiftV6RecHdr) + accs;
	const bool	fixed = !set->wide && !l->env->has_tail && sink->room != NULL;
	const SiftAggSlot64 *slot;

	SIFT_HT_FOREACH(&set->ht, slot)
	{
		uint32		klen = 0;
		const uint8 *kbytes;
		const V6Acc *acc;
		uint32		rlen;
		uint8	   *rec;
		SiftV6RecHdr h = {.set_idx = (uint16) s,.flags = set->wide ? V6REC_WIDE : 0,.hash64 = slot->hash64,.key = slot->key};

		if (fixed && export_fixed(l, sink, set, s, slot))
			continue;
		kbytes = set->wide ? sift_ht_wide_key(&set->ht, slot, &klen) : NULL;
		acc = (const V6Acc *) sift_ht_ext(&set->ht, slot);
		if (l->env->has_tail)
			tail_build(l, acc, &l->tail);
		rlen = (uint32) MAXALIGN(base + klen + l->tail.len);
		h.len = rlen;
		sift_v6_par_check_record_size(rlen);
		rec = record_room(l, rlen);
		memset(rec + base + klen, 0, rlen - base - klen);
		if (set->wide)
		{
			h.key.i64[0] = (int64) slot->key.wide.key_nulls;
			h.key.i64[1] = klen;
			memcpy(rec + base, kbytes, klen);
		}
		memcpy(rec + base + klen, l->tail.data, l->tail.len);
		memcpy(rec, &h, sizeof(h));
		memcpy(rec + sizeof(h), acc, accs);
		sink->put(sink->arg, rec, rlen);
	}
	sift_ht_clear(&set->ht);
}

static void
par_local_export(void *local, const SiftV6RecSink *sink)
{
	ParLocal   *l = local;

	if (l->scanning && l->env->lean)
		sift_v6_par_dist_export(l->dist_out, sink->put, sink->arg);
	else if (l->scanning)
	{
		ExportArg	x = {l, sink->put, sink->arg};

		v6g_dist_dict_drain(l->g, park_dist, l);
		sift_distinct_foreach_hashed(l->seen, export_seen, &x);
		sift_distinct_reset(l->seen);
	}
	for (int s = 0; s < l->g->nsets; s++)
		if (l->g->set[s].nk == 0)
			export_plain(l, s, sink->put, sink->arg);
		else
			export_groups(l, s, sink);
	if (l->g->gen != NULL)
		v6_gen_release(l->g->gen);
	release_exported(l);
}

/* Merges one record into the state, in the memory context of its group; true when it was a group record (the memory of the groups is then to be checked) */
static bool
merge_record(ParLocal *l, const uint8 *rec)
{
	SiftV6Group *g = l->g;
	const SiftV6RecHdr *h = (const SiftV6RecHdr *) rec;
	V6Set	   *set;
	const V6Acc *accs = (const V6Acc *) (rec + sizeof(*h));
	const uint8 *key_bytes = rec + sizeof(*h) + (Size) l->env->spec->naggs * sizeof(V6Acc);
	SiftAggSlot64 *slot;
	bool		found;

	if (h->flags & V6REC_DIST_INT)
	{
		if (l->dist == NULL)
			l->dist = sift_v6_par_dist_new(g);
		sift_v6_par_dist_add(l->dist, rec);
		return false;
	}
	set = &g->set[h->set_idx];
	if (h->flags & V6REC_DIST)
	{
		const uint8 *key = rec + sizeof(*h);
		const uint32 len = (uint32) h->key.i64[1];

		v6g_dist_add(g, set, (uint32) h->key.i64[0], key, len);
		return false;
	}
	if (h->flags & V6REC_PLAIN)
	{
		combine_accs(l, set->plain, accs, (const char *) key_bytes);
		set->plain_row = true;
		return false;
	}
	if (h->flags & V6REC_WIDE)
		slot = sift_ht_upsert_wide(&set->ht, h->hash64, key_bytes, (uint32) h->key.i64[1], (uint64) h->key.i64[0], &found);
	else
		slot = sift_ht_upsert(&set->ht, h->hash64, &h->key, &found);
	combine_accs(l, (V6Acc *) sift_ht_ext(&set->ht, slot), accs, (const char *) key_bytes + ((h->flags & V6REC_WIDE) ? (uint32) h->key.i64[1] : 0));
	return true;
}

static void
par_local_merge(void *local, const uint8 *rec)
{
	ParLocal   *l = local;
	MemoryContext old = MemoryContextSwitchTo(l->g->cx);

	if (merge_record(l, rec))
		v6g_spill_if_over(l->g);
	MemoryContextSwitchTo(old);
}

#define MERGE_BATCH		32

/* The records of one set with an inline key, whose groups one upsert finds together */
typedef struct MergeBatch
{
	uint32		n;
	uint32		set_idx;
	const uint8 *rec[MERGE_BATCH];
	uint64		hash[MERGE_BATCH];
	SiftInlineKey16 key[MERGE_BATCH];
	SiftAggSlot64 *slot[MERGE_BATCH];
} MergeBatch;

static void
batch_flush(ParLocal *l, MergeBatch *b)
{
	V6Set	   *set = &l->g->set[b->set_idx];
	const Size	accs = (Size) l->env->spec->naggs * sizeof(V6Acc);

	if (b->n == 0)
		return;
	sift_ht_upsert_batch(&set->ht, b->hash, b->key, b->n, b->slot);
	for (uint32 i = 0; i < b->n; i++)
		combine_accs(l, (V6Acc *) sift_ht_ext(&set->ht, b->slot[i]), (const V6Acc *) (b->rec[i] + sizeof(SiftV6RecHdr)), (const char *) (b->rec[i] + sizeof(SiftV6RecHdr) + accs));
	b->n = 0;
}

static inline bool
batchable(const ParLocal *l, const SiftV6RecHdr *h)
{
	return !(h->flags & (V6REC_WIDE | V6REC_DIST | V6REC_PLAIN | V6REC_DIST_INT)) && l->g->set[h->set_idx].ht.lock_at == 0;
}

static void
par_local_merge_block(void *local, const uint8 *block, uint32 len)
{
	ParLocal   *l = local;
	MemoryContext old = MemoryContextSwitchTo(l->g->cx);
	MergeBatch	b = {0};
	bool		groups = false;

	for (uint32 off = 0; off < len;)
	{
		const SiftV6RecHdr *h = (const SiftV6RecHdr *) (block + off);

		if (!batchable(l, h))
		{
			batch_flush(l, &b);
			groups |= merge_record(l, block + off);
		}
		else
		{
			if (b.n > 0 && b.set_idx != h->set_idx)
				batch_flush(l, &b);
			b.set_idx = h->set_idx;
			b.rec[b.n] = block + off;
			b.hash[b.n] = h->hash64;
			b.key[b.n++] = h->key;
			groups = true;
			if (b.n == MERGE_BATCH)
				batch_flush(l, &b);
		}
		off += h->len;
	}
	batch_flush(l, &b);
	if (groups)
		v6g_spill_if_over(l->g);
	MemoryContextSwitchTo(old);
}

/* A partition of at most n groups: the tables of its sets take them (a quarter of the memory of the groups at most) without growing */
static void
par_local_presize(void *local, uint64 n)
{
	ParLocal   *l = local;
	SiftV6Group *g = l->g;
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	const uint64 room = (g->budget - g->dist_budget) / 4 / (sizeof(SiftAggSlot64) + 1 + g->stride);

	if (g->ndist == 0 && !l->env->lean)
		for (int s = 0; s < g->nsets; s++)
			if (g->set[s].nk > 0)
				sift_ht_presize(&g->set[s].ht, (uint32) Min(n, room));
	MemoryContextSwitchTo(old);
}

static void
par_local_finish(void *local)
{
	ParLocal   *l = local;
	MemoryContext old = MemoryContextSwitchTo(l->g->cx);

	if (l->dist != NULL)
		sift_v6_par_dist_flush(l->dist);
	MemoryContextSwitchTo(old);
	sift_v6_group_finish(l->g);
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

static void
par_local_offer(void *local, SiftV6TopKRun *topk)
{
	sift_v6_topk_run_offer_groups(topk, ((ParLocal *) local)->g, NULL, NULL, NULL);
}

static uint64
par_local_dist_count(const void *local, uint32 slot)
{
	const ParLocal *l = local;
	SiftV6Group *g = l->g;
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	const uint64 n = l->dist != NULL ? sift_v6_par_dist_count(l->dist, slot) : v6g_dist_count(g, slot);

	MemoryContextSwitchTo(old);
	return n;
}

static bool
par_local_reset(void *local)
{
	ParLocal   *l = local;

	if (l->g->spilled || l->g->frozen)
		return false;
	if (l->dist != NULL)
		sift_v6_par_dist_reset(l->dist);
	sift_v6_group_recycle(l->g);
	l->g->tx.used = 0;
	return true;
}

static void
par_local_set_dist(void *local, uint32 slot, uint64 total)
{
	SiftV6Group *g = ((ParLocal *) local)->g;

	g->set[0].plain[g->dagg[slot]].v0 = total;
}

void
sift_v6_par_engine_make(SiftV6ParEngine *eng, const SiftV6Spec *spec, MemoryContext cx, MemoryContext tuple_cx, TupleDesc desc, SiftV6ParDsmHdr *hdr, const SiftV6ScanSrc *sources,
						struct SiftV6StarRun *star, Snapshot snap)
{
	ParEnv	   *e = MemoryContextAllocZero(cx, sizeof(ParEnv));

	e->spec = spec;
	e->part_spec = *spec;
	e->part_spec.groups_est = spec->groups_est / Max(hdr->radix_nparts, 1U);
	e->scan_spec = *spec;
	e->scan_spec.groups_est = Min(spec->groups_est, sift_v6_par_share(hdr) / SCAN_GROUP_BYTES(spec));
	e->cx = cx;
	e->desc = desc;
	e->tuple_cx = tuple_cx;
	e->hdr = hdr;
	e->sources = sources;
	e->star = star;
	e->nsources = hdr->n_children;
	e->snap = snap;
	e->scan = MemoryContextAllocZero(cx, e->nsources * sizeof(SiftV6Scan *));
	e->lean = sift_v6_par_dist_lean(spec);
	e->op = MemoryContextAlloc(cx, Max(spec->naggs, 1) * sizeof(V6AccOp));
	for (int j = 0; j < spec->naggs; j++)
	{
		e->op[j] = v6g_acc_op(&spec->agg[j]);
		e->has_tail |= v6g_is_gen(&spec->agg[j]) || is_text_ext(&spec->agg[j]);
	}
	*eng = (SiftV6ParEngine) {.env = e,.local_new = par_local_new,.scan_new = par_scan_new,.local_free = par_local_free,.scan_hook = par_scan_hook,.scan_morsel = par_scan_morsel,
		.local_load = par_local_load,.local_export = par_local_export,.local_merge = par_local_merge,.local_merge_block = par_local_merge_block,.local_presize = par_local_presize,
		.local_finish = par_local_finish,.local_next = par_local_next,.local_offer = par_local_offer,.local_dist_count = par_local_dist_count,.local_set_dist = par_local_set_dist,
		.local_reset = par_local_reset};
}

void
sift_v6_par_engine_end(SiftV6ParEngine *eng)
{
	ParEnv	   *e = eng->env;

	for (uint32 k = 0; k < e->nsources; k++)
	{
		if (e->scan[k] != NULL)
			sift_v6_src_par(&e->sources[k])->end(e->scan[k]);
		e->scan[k] = NULL;
	}
}
