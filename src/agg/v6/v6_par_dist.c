/*
 * agg/v6/v6_par_dist.c - COUNT(DISTINCT integer column) of the parallel SiftAggV6 over the flat sets of v6_distinct_u64.h (v6_par_dist.h).
 *
 * A participant of a query of no keys leaves the values of its rows to the flat set the serial run fills (group_dist.c) and empties it into the partitions when it is full: its values repeat, so a
 * third of the rows is what it hands over. A participant of a GROUP BY makes a record of every row through the sink of group_dist.c: the set of (group, value) pairs is as large as the rows, and
 * its random probes leave the cache; the owner of a partition, which holds a share of the pairs, counts them in a cache. The owner fills a flat set with the records it is given, a batch at a
 * time (the prefetching of the sets works on batches), and the finish of its group reads the counts from it as it does in a serial run. A set that outgrows its budget at the owner moves to the exact
 * generic set, which spills, so that a partition larger than its memory is still counted.
 */
#include "postgres.h"

#include "agg/v6/v6_par_dist.h"

#define DIST_BATCH			512
#define DIST_FLAT_BYTES		(8 << 20)		/* what the flat set of a participant (a query of no keys) holds before it goes to the partitions */

static inline bool
flat_kind(const SiftV6Agg *a)
{
	const SiftV6Class cls = sift_v6_class(a->arg.typid);

	return a->nxcols == 0 && (cls == SV6C_I32 || cls == SV6C_I64 || cls == SV6C_BOOL);
}

bool
sift_v6_par_dist_lean(const SiftV6Spec *spec)
{
	bool		any = false;

	for (int j = 0; j < spec->naggs; j++)
		if (spec->agg[j].kind == SV6_COUNT_DISTINCT)
		{
			if (!flat_kind(&spec->agg[j]))
				return false;
			any = true;
		}
	return any && spec->nsets == 0;
}

void
sift_v6_par_check_record_size(uint32 rlen)
{
	if (rlen > SIFT_RADIX_PAYLOAD)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a group key or a COUNT(DISTINCT) value is too large for the parallel aggregate"),
						errhint("Set max_parallel_workers_per_gather to 0 for this query.")));
}

/* ---- a participant that scans ---- */

#define DIST_RAW_BYTES		(1 << 20)		/* the records of a GROUP BY a participant holds before they go to the partitions: what its L2 cache keeps */

struct SiftV6ParDistOut
{
	SiftV6Group *g;
	SiftBytes	recs;			/* a GROUP BY: the records of its rows, as they will go to the partitions */
};

/* The routing hash of the group of a prefix (its nulls, then its key): the one the group has in its table, so that the value reaches the partition of the group's accumulators */
static uint64
prefix_hash(const V6Set *set, const uint8 *prefix, uint32 plen)
{
	const uint8 *key = prefix + sizeof(uint64);
	uint64		nulls;

	memcpy(&nulls, prefix, sizeof(nulls));
	if (set->packed)
	{
		SiftInlineKey16 ik;

		memcpy(&ik, key, sizeof(ik));
		return sift_pack_hash(&ik);
	}
	return v6g_group_hash(set, key, plen - (uint32) sizeof(nulls), nulls);
}

/* the record of a value: its head, the bytes of its group, the padding */
static void
fill_value(uint8 *rec, uint32 rlen, uint32 slot, uint64 hash, uint64 value, const uint8 *prefix, uint32 plen)
{
	const SiftV6DistRec h = {.len = rlen,.plen = (uint16) plen,.flags = (uint16) (V6REC_DIST_INT | (slot << 8)),.hash64 = hash,.value = (int64) value};

	memcpy(rec, &h, sizeof(h));
	memcpy(rec + sizeof(h), prefix, plen);
	memset(rec + sizeof(h) + plen, 0, rlen - sizeof(h) - plen);
}

/* A row of a GROUP BY COUNT(DISTINCT): the bytes group_dist.c builds (the length of the group's bytes, the bytes, the value as a distinct key) become a record */
static void
row_sink(void *arg, uint32 set_idx, uint32 slot, const void *bytes, uint32 len)
{
	SiftV6ParDistOut *o = arg;
	const uint8 *b = bytes;
	uint32		plen;
	uint32		rlen;
	int64		value;

	memcpy(&plen, b, sizeof(plen));
	memcpy(&value, b + len - sizeof(value), sizeof(value));
	rlen = (uint32) MAXALIGN(sizeof(SiftV6DistRec) + plen);
	sift_v6_par_check_record_size(rlen);
	fill_value(sift_bytes_room(&o->recs, rlen), rlen, slot, prefix_hash(&o->g->set[set_idx], b + sizeof(plen), plen), (uint64) value, b + sizeof(plen), plen);
	o->recs.len += rlen;
}

SiftV6ParDistOut *
sift_v6_par_dist_out_new(SiftV6Group *g)
{
	SiftV6ParDistOut *o = MemoryContextAllocZero(g->cx, sizeof(SiftV6ParDistOut));

	o->g = g;
	if (g->set[0].nk > 0)
	{
		sift_bytes_init(&o->recs, g->cx);
		sift_v6_group_scan_sink(g, row_sink, o);
	}
	else
	{
		g->budget = UINT64_MAX;
		g->dist_budget = UINT64_MAX / 4;
	}
	return o;
}

uint64
sift_v6_par_dist_load(const SiftV6ParDistOut *o)
{
	const SiftV6Group *g = o->g;
	const V6Set *set = &g->set[0];
	uint64		bytes = 0;

	if (set->nk > 0)
		return (uint64) o->recs.len * SIFT_PAR_LOCAL_GROUPS / DIST_RAW_BYTES;
	for (int k = 0; k < g->ndist; k++)
		bytes += set->flat != NULL && set->flat[k] != NULL ? sift_du64_bytes(set->flat[k]) : 0;
	return bytes * SIFT_PAR_LOCAL_GROUPS / DIST_FLAT_BYTES;
}

static void
put_plain(void *arg, uint64 value)
{
	const struct
	{
		SiftV6RecPut put;
		void	   *arg;
		uint32		slot;
	}		   *to = arg;
	uint8		rec[sizeof(SiftV6DistRec)];

	fill_value(rec, sizeof(rec), to->slot, sift_aggk6_hash1(value), value, NULL, 0);
	to->put(to->arg, rec, sizeof(rec));
}

void
sift_v6_par_dist_export(SiftV6ParDistOut *o, SiftV6RecPut put, void *arg)
{
	V6Set	   *set = &o->g->set[0];

	if (set->nk > 0)
	{
		for (uint32 at = 0; at < o->recs.len; at += ((const SiftV6DistRec *) (o->recs.buf + at))->len)
			put(arg, o->recs.buf + at, ((const SiftV6DistRec *) (o->recs.buf + at))->len);
		sift_bytes_reset(&o->recs);
		return;
	}
	for (int k = 0; k < o->g->ndist; k++)
		if (set->flat != NULL && set->flat[k] != NULL)
		{
			const struct
			{
				SiftV6RecPut put;
				void	   *arg;
				uint32		slot;
			}			to = {put, arg, (uint32) k};

			sift_du64_each(set->flat[k], put_plain, (void *) &to);
			sift_du64_free(set->flat[k]);
			set->flat[k] = NULL;
		}
}

/* ---- the owner of a partition ---- */

typedef struct DistIn
{
	uint32		n;
	uint32		off[DIST_BATCH];
	uint32		plen[DIST_BATCH];
	uint64		hash[DIST_BATCH];
	uint64		val[DIST_BATCH];
	uint32		gid[DIST_BATCH];
	SiftBytes	prefix;
} DistIn;

struct SiftV6ParDist
{
	SiftV6Group *g;
	SiftBytes	key;
	DistIn	   *in;				/* of each aggregate slot */
};

SiftV6ParDist *
sift_v6_par_dist_new(SiftV6Group *g)
{
	SiftV6ParDist *d = MemoryContextAllocZero(g->cx, sizeof(SiftV6ParDist));

	d->g = g;
	d->in = MemoryContextAllocZero(g->cx, Max(g->ndist, 1) * sizeof(DistIn));
	for (int k = 0; k < g->ndist; k++)
		sift_bytes_init(&d->in[k].prefix, g->cx);
	sift_bytes_init(&d->key, g->cx);
	return d;
}

/* One pair as the row of the exact generic set the serial run keeps, which the sets that outgrew their budget move to (the bytes group_dist.c builds for a row) */
static void
add_generic(SiftV6ParDist *d, uint32 slot, const uint8 *prefix, uint32 plen, uint64 value)
{
	SiftV6Group *g = d->g;
	V6Set	   *set = &g->set[0];
	uint64		hash;

	sift_bytes_reset(&d->key);
	if (set->nk > 0)
	{
		sift_bytes_put(&d->key, &plen, sizeof(plen));
		sift_bytes_put(&d->key, prefix, plen);
	}
	sift_dkey_i64(&d->key, (int64) value);
	hash = sift_aggk6_hash_bytes(d->key.buf, d->key.len);
	v6g_dist_add_raw(g, set, slot, d->key.buf, d->key.len, set->nk == 0 ? sift_aggk6_hash1(hash) : hash);
}

static void
move_value(void *arg, uint64 v)
{
	const struct
	{
		SiftV6ParDist *d;
		uint32		slot;
	}		   *to = arg;

	add_generic(to->d, to->slot, NULL, 0, v);
}

static void
move_to_generic(SiftV6ParDist *d, uint32 slot)
{
	V6Set	   *set = &d->g->set[0];
	const struct
	{
		SiftV6ParDist *d;
		uint32		slot;
	}			to = {d, slot};

	if (set->nk == 0)
	{
		sift_du64_each(set->flat[slot], move_value, (void *) &to);
		sift_du64_free(set->flat[slot]);
		set->flat[slot] = NULL;
		set->flat_moved[slot] = true;
	}
	else
		v6g_dist_gflat_flush(d->g, set);
}

static bool
is_moved(const V6Set *set, uint32 slot)
{
	return set->nk == 0 ? set->flat_moved != NULL && set->flat_moved[slot] : set->gflat_moved != NULL && set->gflat_moved[slot];
}

/* the flat set of the slot, made when it is the first record of the slot */
static void
open_flat(SiftV6ParDist *d, uint32 slot)
{
	SiftV6Group *g = d->g;
	V6Set	   *set = &g->set[0];
	const uint64 budget = g->dist_budget / ((uint64) g->nsets * (uint64) g->ndist);

	if (set->nk == 0)
	{
		if (set->flat == NULL)
		{
			set->flat = MemoryContextAllocZero(g->cx, g->ndist * sizeof(SiftDistinctU64 *));
			set->flat_moved = MemoryContextAllocZero(g->cx, g->ndist * sizeof(bool));
		}
		if (set->flat[slot] == NULL)
			set->flat[slot] = sift_du64_create(g->cx, budget);
	}
	else
	{
		if (set->gflat == NULL)
		{
			set->gflat = MemoryContextAllocZero(g->cx, g->ndist * sizeof(SiftDu64G *));
			set->gflat_moved = MemoryContextAllocZero(g->cx, g->ndist * sizeof(bool));
		}
		if (set->gflat[slot] == NULL)
			set->gflat[slot] = sift_du64g_create(g->cx, budget);
	}
}

/* The number of the group of a record in the table of the owner (its slot's ext_idx), made when the group is new: the table is the one the accumulators of the group merge into */
static uint32
group_gid(V6Set *set, uint64 hash, const uint8 *prefix, uint32 plen)
{
	const uint8 *key = prefix + sizeof(uint64);
	SiftInlineKey16 ik = {0};
	uint64		nulls;
	bool		found;

	memcpy(&nulls, prefix, sizeof(nulls));
	if (set->wide)
		return sift_ht_upsert_wide(&set->ht, hash, key, plen - (uint32) sizeof(nulls), nulls, &found)->ext_idx;
	if (set->packed)
		memcpy(&ik, key, sizeof(ik));
	else
	{
		memcpy(&ik.i64[0], key, sizeof(int64));
		ik.i64[1] = (int64) nulls;
	}
	return sift_ht_upsert(&set->ht, hash, &ik, &found)->ext_idx;
}

static uint32
add_flat_pairs(SiftV6ParDist *d, uint32 slot, DistIn *in)
{
	V6Set	   *set = &d->g->set[0];
	uint32		done;

	if (set->nk == 0)
		return sift_du64_add(set->flat[slot], in->val, in->n);
	for (uint32 i = 0; i < in->n; i++)
		in->gid[i] = group_gid(set, in->hash[i], in->prefix.buf + in->off[i], in->plen[i]);
	done = sift_du64g_add_batch(set->gflat[slot], in->gid, in->val, in->n);
	v6g_spill_if_over(d->g);
	return done;
}

static void
flush_slot(SiftV6ParDist *d, uint32 slot)
{
	DistIn	   *in = &d->in[slot];
	V6Set	   *set = &d->g->set[0];
	uint32		done = 0;

	if (in->n == 0)
		return;
	if (!is_moved(set, slot))
	{
		open_flat(d, slot);
		done = add_flat_pairs(d, slot, in);
		if (done < in->n)
			move_to_generic(d, slot);
	}
	for (uint32 i = done; i < in->n; i++)
		add_generic(d, slot, in->prefix.buf + in->off[i], in->plen[i], in->val[i]);
	in->n = 0;
	sift_bytes_reset(&in->prefix);
}

void
sift_v6_par_dist_add(SiftV6ParDist *d, const uint8 *rec)
{
	const SiftV6DistRec *h = (const SiftV6DistRec *) rec;
	const uint32 slot = V6REC_DIST_SLOT(h->flags);
	DistIn	   *in = &d->in[slot];
	const uint32 plen = h->plen;

	if (in->n == DIST_BATCH)
		flush_slot(d, slot);
	in->off[in->n] = in->prefix.len;
	in->plen[in->n] = plen;
	in->hash[in->n] = h->hash64;
	in->val[in->n++] = (uint64) h->value;
	sift_bytes_put(&in->prefix, rec + sizeof(*h), plen);
}

void
sift_v6_par_dist_flush(SiftV6ParDist *d)
{
	for (int k = 0; k < d->g->ndist; k++)
		flush_slot(d, (uint32) k);
}

uint64
sift_v6_par_dist_count(SiftV6ParDist *d, uint32 slot)
{
	V6Set	   *set = &d->g->set[0];

	sift_v6_par_dist_flush(d);
	if (set->nk == 0 && set->flat != NULL && set->flat[slot] != NULL)
		return sift_du64_count(set->flat[slot]);
	return v6g_dist_count(d->g, slot);
}

void
sift_v6_par_dist_reset(SiftV6ParDist *d)
{
	V6Set	   *set = &d->g->set[0];

	for (int k = 0; k < d->g->ndist; k++)
	{
		d->in[k].n = 0;
		sift_bytes_reset(&d->in[k].prefix);
		if (set->flat != NULL && set->flat[k] != NULL)
			sift_du64_free(set->flat[k]);
		if (set->gflat != NULL && set->gflat[k] != NULL)
			sift_du64g_free(set->gflat[k]);
		if (set->flat != NULL)
			set->flat[k] = NULL;
		if (set->gflat != NULL)
			set->gflat[k] = NULL;
		if (set->flat_moved != NULL)
			set->flat_moved[k] = false;
		if (set->gflat_moved != NULL)
			set->gflat_moved[k] = false;
	}
}
