/*
 * col/f2/f2_tier_maint.c - the step of maintenance that keeps the object storage tier and the TTL of a sift_table (sift.set_tier_policy, sift.set_ttl): the parts older than the retention leave the
 * manifest, the objects of the parts that left are deleted once every standby has replayed that, the eligible parts move to object storage, and the objects no commit names are swept.
 *
 * Every change of the parts of a partition holds the compaction lock of the partition (a compaction of it is skipped, never raced) and, from the read of the parts to the end of its commit, the
 * partition lock, so that a delete or a flush cannot land between the read and the write of the descriptors. A store that cannot be reached is a WARNING and the step ends: the tier is an
 * optional cold copy and must not stop the freezing and the compaction of the rest of the table.
 */
#include "postgres.h"

#include "access/transam.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/f2/f2_tier_host.h"
#include "col/f2/part_format.h"
#include "core/crash_point.h"
#include "core/f2/f2_gate.h"
#include "core/part_lock.h"
#include "core/wal_conflict.h"

#define RANGE_BYTES		(UINT32_C(1) << 20)

typedef struct Step
{
	Relation	rel;
	FullTransactionId horizon;
	TimestampTz now;
} Step;

typedef struct TierRun
{
	SiftTierPolicy pol;
	SiftTierBackend *be;
	int			slot;
	int64		cutoff;
} TierRun;

static void
warn_store(const char *what, const SiftTierErr *err)
{
	ereport(WARNING, (errmsg("sift: %s: %s", what, err->msg)));
}

/* The partitions as the manifest has them now: each step re-reads its own under the locks */
static SiftManifestRead
partitions(Relation rel)
{
	return sift_manifest_read_range(rel, NULL, NULL);
}

static SiftPartitionEntry *
entry_locked(Relation rel, const SiftPartitionEntry *e, SiftManifestRead *mr)
{
	*mr = sift_manifest_read_range(rel, e->key_lo, NULL);
	return mr->n > 0 && mr->entries[0].part_id == e->part_id ? &mr->entries[0] : NULL;
}

/* ---- TTL ---- */

static void
ttl_commit(const Step *s, int slot, int64 cutoff, const SiftPartitionEntry *e)
{
	int			np,
				nupd = 0,
				nrel;
	SiftPartDesc *parts = sift_manifest_read_parts(s->rel, e, &np);
	SiftTtlPlan *plan = sift_ttl_plan(parts, np, slot, cutoff, s->horizon);
	SiftPartDesc *upd = palloc(Max(plan->nmark + plan->nunmark, 1) * sizeof(SiftPartDesc));
	SiftExtent *rel_ext;
	SiftManifestEdit edit;

	for (int i = 0; i < plan->nmark; i++)
	{
		upd[nupd] = *sift_tier_find_part(parts, np, plan->mark_gens[i]);
		upd[nupd++].ext_flags |= SIFT_PART_EXT_EXPIRED;
	}
	for (int i = 0; i < plan->nunmark; i++)
	{
		upd[nupd] = *sift_tier_find_part(parts, np, plan->unmark_gens[i]);
		upd[nupd++].ext_flags &= (uint8) ~SIFT_PART_EXT_EXPIRED;
	}
	if (plan->ndrop + nupd > 0)
	{
		nrel = sift_tier_release_extents(s->rel, e->part_id, parts, np, plan->drop_gens, plan->ndrop, true, &rel_ext);
		edit = (SiftManifestEdit) {.part_id = e->part_id,.key_lo = e->key_lo,.drop_gens = plan->drop_gens,.ndrop = plan->ndrop,.update_parts = upd,.nupdate = nupd,
		.release = rel_ext,.nrelease = nrel};
		if (plan->ndrop > 0)
			(void) sift_wal_snapshot_conflict(s->rel, InvalidBlockNumber, ReadNextFullTransactionId());
		SIFT_CRASH_POINT_IF(plan->ndrop > 0, f2_ttl_drop_pre_p2);
		sift_manifest_commit(s->rel, &edit);
		SIFT_CRASH_POINT_IF(plan->ndrop > 0, f2_ttl_drop_post_p2);
		ereport(DEBUG1, (errmsg("sift: TTL of \"%s\" partition %llu: %d parts dropped, %d marked, %d unmarked", RelationGetRelationName(s->rel),
								(unsigned long long) e->part_id, plan->ndrop, plan->nmark, plan->nunmark)));
	}
	sift_ttl_plan_free(plan);
}

static void
ttl_partition(const Step *s, int slot, int64 cutoff, const SiftPartitionEntry *listed)
{
	SiftManifestRead mr;
	SiftPartitionEntry *e;

	if (!sift_tier_lock_compaction(s->rel, listed->part_id))
		return;
	sift_tier_lock_part(s->rel, listed->part_id);
	e = entry_locked(s->rel, listed, &mr);
	if (e != NULL)
		ttl_commit(s, slot, cutoff, e);
	sift_tier_unlock_part(s->rel, listed->part_id);
	sift_tier_unlock_compaction(s->rel, listed->part_id);
}

static void
ttl_step(const Step *s, int slot, int64 cutoff)
{
	SiftManifestRead all = partitions(s->rel);

	for (int i = 0; i < all.n; i++)
	{
		ttl_partition(s, slot, cutoff, &all.entries[i]);
		CHECK_FOR_INTERRUPTS();
	}
}

void
sift_f2_ttl_unmark_all(Relation rel)
{
	Step		s = {.rel = rel,.horizon = InvalidFullTransactionId};

	ttl_step(&s, 0, PG_INT64_MIN);
}

/* ---- the move of a part to object storage ---- */

typedef struct LocalPart
{
	Relation	rel;
	const SiftPart *part;
} LocalPart;

static void
read_local(void *arg, uint64 off, uint64 n, void *dst)
{
	const LocalPart *lp = arg;

	sift_part_read_bytes(lp->rel, lp->part, off, n, dst);
}

/* The header page of the part as the object keeps it: the extents are the blocks of this relation and mean nothing off it, so they are not in it */
static char *
object_image(const SiftPart *p, uint32 *len)
{
	char	   *image = palloc(p->inline_len);
	SiftPartHeaderData *h = (SiftPartHeaderData *) image;

	memcpy(image, p->inline_payload, p->inline_len);
	memset(image + h->extents_off, 0, h->ninline_extents * sizeof(SiftExtent));
	h->total_extents = 0;
	h->ninline_extents = 0;
	h->extlist_blk = InvalidBlockNumber;
	*len = p->inline_len;
	return image;
}

static int
section_off_cmp(const void *a, const void *b)
{
	uint64		x = (*(const SiftSecDirEntry *const *) a)->off,
				y = (*(const SiftSecDirEntry *const *) b)->off;

	return x < y ? -1 : x > y;
}

/* The ranges a reader may ask for alone: the sections that are in the stream, cut into RANGE_BYTES, each with the checksum of its bytes */
static SiftRemoteRange *
object_ranges(LocalPart *lp, uint32 *nranges)
{
	const SiftPart *p = lp->part;
	const SiftSecDirEntry **secs = palloc(Max(p->hdr.nsec, 1) * sizeof(*secs));
	int			ns = 0;
	uint32		n = 0;
	SiftRemoteRange *ranges;
	char	   *buf = palloc(RANGE_BYTES);

	for (int i = 0; i < p->hdr.nsec; i++)
		if (!(p->secdir[i].flags & SIFT_SEC_FLAG_INLINE) && p->secdir[i].len > 0)
			secs[ns++] = &p->secdir[i];
	qsort(secs, ns, sizeof(*secs), section_off_cmp);
	for (int i = 0; i < ns; i++)
		n += (uint32) ((secs[i]->len + RANGE_BYTES - 1) / RANGE_BYTES);
	ranges = palloc(Max(n, 1) * sizeof(SiftRemoteRange));
	n = 0;
	for (int i = 0; i < ns; i++)
		for (uint64 at = 0; at < secs[i]->len; at += RANGE_BYTES)
		{
			uint32		len = (uint32) Min(RANGE_BYTES, secs[i]->len - at);

			read_local(lp, secs[i]->off + at, len, buf);
			ranges[n] = (SiftRemoteRange) {.col = secs[i]->tag,.granule = (uint32) (at / RANGE_BYTES),.off = secs[i]->off + at,.len = len,.crc32c = sift_tier_crc32c_extend(0, buf, len)};
			n++;
		}
	pfree(buf);
	pfree(secs);
	*nranges = n;
	return ranges;
}

/* The commit that makes the part remote: its descriptor as it is now (a delete may have reached it since the upload began), the ref, and the blocks it held */
static void
commit_tiered(const Step *s, const SiftPartitionEntry *listed, const SiftTierPartRef *ref)
{
	SiftManifestRead mr;
	SiftPartitionEntry *e;

	sift_tier_lock_part(s->rel, listed->part_id);
	e = entry_locked(s->rel, listed, &mr);
	if (e != NULL)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(s->rel, e, &np);
		const SiftPartDesc *now = sift_tier_find_part(parts, np, ref->part_gen);

		if (now != NULL)
		{
			SiftPartDesc remote = *now;
			SiftExtent *rel_ext;
			int			nrel = sift_tier_release_extents(s->rel, e->part_id, parts, np, &ref->part_gen, 1, false, &rel_ext);
			SiftManifestEdit edit = {.part_id = e->part_id,.key_lo = e->key_lo,.update_parts = &remote,.nupdate = 1,.set_tier = ref,.nset_tier = 1,.release = rel_ext,.nrelease = nrel};

			remote.ext_flags |= SIFT_PART_EXT_REMOTE;
			remote.header_blk = InvalidBlockNumber;
			sift_manifest_commit(s->rel, &edit);
			SIFT_CRASH_POINT(f2_tier_evict_post_p2);
		}
	}
	sift_tier_unlock_part(s->rel, listed->part_id);
}

/* false: the store failed (a WARNING says how) and the step ends */
static bool
tier_one(const Step *s, const TierRun *t, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	SiftPart   *part = sift_part_open_desc(s->rel, d, (uint32) e->part_id);
	LocalPart	lp = {s->rel, part};
	uint32		image_len,
				nranges;
	char	   *image;
	SiftRemoteRange *ranges;
	SiftTierPartSrc src;
	SiftTierPartRef ref;
	SiftTierErr err = {0};
	char		key[160];
	SiftTierStatus st;

	if (part->is_packed || (part->hdr.flags & SIFT_PART_FLAG_INLINE_STREAM))
		return true;
	image = object_image(part, &image_len);
	ranges = object_ranges(&lp, &nranges);
	src = (SiftTierPartSrc) {.part_gen = d->part_gen,.part_id = (uint32) e->part_id,.image = image,.image_len = image_len,.stream_len = part->hdr.stream_bytes,
	.read_stream = read_local,.arg = &lp,.ranges = ranges,.nranges = nranges};
	sift_tier_object_key(key, sizeof(key), t->pol.scope_id, (uint32) e->part_id, d->part_gen);
	st = sift_tier_upload(t->be, key, &src, t->pol.policy_id, GetCurrentTimestamp(), &ref, &err);
	pfree(image);
	pfree(ranges);
	if (st != SIFT_TIER_OK)
	{
		warn_store("cannot move a part to object storage", &err);
		return false;
	}
	SIFT_CRASH_POINT(f2_tier_upload_post_remote_pre_commit);
	SIFT_F2_GATE(SIFT_F2_GATE_TIER);
	commit_tiered(s, e, &ref);
	return true;
}

/* Moves the eligible parts of the partition while *budget lasts; false when the store failed */
static bool
tier_partition(const Step *s, const TierRun *t, const SiftPartitionEntry *listed, int *budget)
{
	SiftManifestRead mr;
	SiftPartitionEntry *e;
	bool		ok = true;

	if (!sift_tier_lock_compaction(s->rel, listed->part_id))
		return true;
	e = entry_locked(s->rel, listed, &mr);
	if (e != NULL)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(s->rel, e, &np);

		for (int i = 0; i < np && ok && *budget > 0; i++)
			if (sift_tier_part_eligible(&parts[i], t->slot, t->cutoff))
			{
				ok = tier_one(s, t, e, &parts[i]);
				*budget -= ok;
			}
	}
	sift_tier_unlock_compaction(s->rel, listed->part_id);
	return ok;
}

/* ---- the objects of the parts that left, and the ones no commit names ---- */

static void
object_key_of(const TierRun *t, const SiftTierPartRef *ref, char *key, size_t cap)
{
	sift_tier_object_key(key, cap, t->pol.scope_id, ref->part_id, ref->part_gen);
}

/* The objects whose grace is over are deleted, then one commit removes their refs: a crash in between repeats the deletes, which succeed for an object that is gone */
static bool
delete_due(const Step *s, const TierRun *t)
{
	int			n;
	const SiftTierPartRef *refs = sift_tier_refs(s->rel, &n);
	uint64	   *due = palloc(Max(n, 1) * sizeof(uint64));
	int			nd = n > 0 ? sift_ttl_due_deletes(refs, n, s->now, due) : 0;
	SiftTierErr err = {0};
	char		key[160];

	for (int i = 0; i < nd; i++)
	{
		object_key_of(t, sift_tier_refs_find(refs, n, due[i]), key, sizeof(key));
		if (t->be->ops->del(t->be, key, &err) != SIFT_TIER_OK)
		{
			warn_store("cannot delete an object of a part that left", &err);
			return false;
		}
	}
	if (nd > 0)
	{
		SiftManifestRead all = partitions(s->rel);
		SiftManifestEdit edit = {.part_id = all.entries[0].part_id,.key_lo = all.entries[0].key_lo,.drop_tier_gens = due,.ndrop_tier = nd};

		sift_tier_lock_part(s->rel, edit.part_id);
		sift_manifest_commit(s->rel, &edit);
		sift_tier_unlock_part(s->rel, edit.part_id);
	}
	pfree(due);
	return true;
}

static void
sweep(const Step *s, const TierRun *t)
{
	int			n,
				ndeleted = 0;
	const SiftTierPartRef *refs = sift_tier_refs(s->rel, &n);
	SiftTierErr err = {0};

	if (sift_tier_sweep_orphans(t->be, t->pol.scope_id, refs, n, &ndeleted, &err) != SIFT_TIER_OK)
		warn_store("cannot sweep the object store", &err);
	else if (ndeleted > 0)
		ereport(LOG, (errmsg("sift: removed %d objects of \"%s\" that no commit names", ndeleted, RelationGetRelationName(s->rel))));
}

static void
tier_step(const Step *s, const SiftTierPolicy *pol, int slot)
{
	TierRun		t = {.pol = *pol,.slot = slot};
	SiftTierErr err = {0};
	SiftManifestRead all;
	int			budget = SIFT_TIER_BATCH;
	SiftF2Store *store = sift_f2_store_open(s->rel);

	t.be = sift_tier_backend_try(pol, &err);
	if (t.be == NULL || sift_tier_cache_try(pol, &err) == NULL)
	{
		warn_store("the object store of the tier policy cannot be opened", &err);
		return;
	}
	t.cutoff = sift_tier_cutoff(store, slot, s->now, pol->move_after_us);
	if (!delete_due(s, &t))
		return;
	all = partitions(s->rel);
	for (int i = 0; i < all.n && budget > 0; i++)
	{
		if (!tier_partition(s, &t, &all.entries[i], &budget))
			return;
		CHECK_FOR_INTERRUPTS();
	}
	sweep(s, &t);
}

void
sift_f2_tier_maintain(Relation rel, SiftF2Store *store, FullTransactionId horizon)
{
	Step		s = {.rel = rel,.horizon = horizon,.now = GetCurrentTimestamp()};
	SiftTierPolicy tier;
	SiftTtlPolicy ttl;
	bool		has_tier = sift_tier_policy_of(rel, &tier),
				has_ttl = sift_ttl_policy_of(rel, &ttl);
	int			ttl_slot = has_ttl ? sift_tier_time_slot(rel, store, ttl.col) : -1;

	if (has_ttl && ttl_slot < 0)
		ereport(WARNING, (errmsg("sift: the TTL column \"%s\" of \"%s\" is not a time column of the table: the TTL does not run", ttl.col, RelationGetRelationName(rel))));
	if (ttl_slot >= 0)
		ttl_step(&s, ttl_slot, sift_tier_cutoff(store, ttl_slot, s.now, ttl.retention_us));
	if (has_tier && store->spec.time_cols[Max(ttl_slot, 0)] > 0 && ConditionalLockPage(rel, SIFT_F2_MAINT_LOCK_BLKNO, ExclusiveLock))
	{
		tier_step(&s, &tier, Max(ttl_slot, 0));
		UnlockPage(rel, SIFT_F2_MAINT_LOCK_BLKNO, ExclusiveLock);
	}
}
