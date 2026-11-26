/*
 * col/f2/drop/parts.c - sift.drop_parts_before(rel, cutoff, col): the commit of the retention drop of col/f2/drop/parts.h.
 *
 * The function runs as the caller and needs MAINTAIN on the table. The parts leave for good, as in TRUNCATE: it refuses a transaction block, which could roll back what is gone, and it holds
 * AccessExclusiveLock, which waits for the transactions that read the table (a snapshot of REPEATABLE READ or SERIALIZABLE keeps its lock to the end) so that none of them sees rows vanish.
 * Every partition is changed under its compaction lock (a running compaction is skipped and its expired parts are counted unsettled) and its partition lock, from the read of its parts to the
 * end of the commit. The visibility map bits of the dropped parts are cleared first, so that no index-only scan answers a row that is gone. The standby conflict is raised before the commit,
 * and the blocks of the parts enter the free space map's quarantine with it: a standby that still reads them cancels or waits as for any other cleanup, and nothing is reused before it has
 * replayed the commit.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "col/f2/drop/parts.h"
#include "col/f2/gbuf/gbuf.h"
#include "col/f2/part/idx.h"
#include "col/f2/tier/host.h"
#include "col/f2/part/format.h"
#include "core/catname.h"
#include "core/compat.h"
#include "core/f2/fsm/fsm.h"
#include "core/inspect.h"
#include "core/meta_peek.h"
#include "core/wal_conflict.h"
#include "col/f2/l0/stage.h"
#include "col/f2/scope/scope.h"
#include "table/dispatch.h"
#include "table/f2/tbl.h"
#include "table/f2/vm/vm.h"
#include "table/wal.h"

PG_FUNCTION_INFO_V1(sift_drop_parts_before);

typedef struct Drop
{
	Relation	rel;
	SiftF2Store *store;
	int			slot;
	int64		cutoff;
	FullTransactionId horizon;
	SiftDropResult res;
} Drop;

static int
cmp_u32(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

/* The granules of a part that keep an exception stream (SEC_VARIANT_EXC), each once: a part of narrow columns has none and this reads one section of it */
static uint32 *
exception_granules(Relation rel, const SiftPart *part, int *n)
{
	uint64		len;
	const SiftVariantExcSummary *ex = sift_part_read_section(rel, part, SEC_VARIANT_EXC, &len);
	int			ne = ex == NULL ? 0 : (int) (len / sizeof(SiftVariantExcSummary)),
				k = 0;
	uint32	   *g = palloc(sizeof(uint32) * Max(ne, 1));

	for (int i = 0; i < ne; i++)
		g[i] = ex[i].granule;
	qsort(g, ne, sizeof(uint32), cmp_u32);
	for (int i = 0; i < ne; i++)
		if (k == 0 || g[k - 1] != g[i])
			g[k++] = g[i];
	*n = k;
	return g;
}

static void
release_granule_blobs(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, uint32 granule, SiftExtVec *v)
{
	SiftF2GranuleBuf *gb = sift_f2_gbuf_load(rel, store, pin, granule);
	/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
	uint64	   *all = palloc((gb->nrows + 63) / 64 * sizeof(uint64));

	memset(all, 0xFF, (gb->nrows + 63) / 64 * sizeof(uint64));
	store->host->reclaim_dead_blobs(rel, store, &gb->view, all, v);
	pfree(all);
	sift_f2_gbuf_unref(gb);
}

void
sift_part_wide_extents(Relation rel, SiftF2Store *store, SiftF2PartPin *pin, SiftExtVec *v)
{
	int			ng;
	uint32	   *g = exception_granules(rel, pin->part, &ng);

	for (int k = 0; k < ng; k++)
		release_granule_blobs(rel, store, pin, g[k], v);
	pfree(g);
}

int
sift_drop_wide_extents(Relation rel, SiftF2Store *store, uint32 part_id, const SiftPartDesc *parts, int np, const uint64 *gens, int ngens, SiftExtent **release, int nrelease)
{
	SiftExtVec	v = {.ext = *release,.n = nrelease,.cap = nrelease};
	SiftPartGenIndex by_gen;

	sift_part_gen_idx_init(&by_gen, parts, np);
	for (int i = 0; i < ngens && sift_tbl_extra(store)->has_wide; i++)
	{
		SiftF2PartPin *pin = sift_f2_pin_open(rel, store, sift_part_gen_idx_find(&by_gen, gens[i]), part_id);

		sift_part_wide_extents(rel, store, pin, &v);
		sift_f2_pin_release(pin);
	}
	sift_part_gen_idx_free(&by_gen);
	*release = v.ext;
	return v.n;
}

static int
count_expired(Drop *d, const SiftPartitionEntry *e)
{
	int			np,
				n = 0;
	SiftPartDesc *parts = sift_manifest_read_parts(d->rel, e, &np);

	for (int i = 0; i < np; i++)
		n += sift_ttl_classify(&parts[i], d->slot, d->cutoff) == SIFT_TTL_EXPIRED;
	return n;
}

static SiftPartDesc *
flagged(const SiftPartGenIndex *by_gen, const uint64 *gens, int n, bool expired, SiftPartDesc *out)
{
	for (int i = 0; i < n; i++)
	{
		out[i] = *sift_part_gen_idx_find(by_gen, gens[i]);
		out[i].ext_flags = expired ? (out[i].ext_flags | SIFT_PART_EXT_EXPIRED) : (uint8) (out[i].ext_flags & ~SIFT_PART_EXT_EXPIRED);
	}
	return out;
}

/* The bits of the vblocks a dropped part had rows in: the index entries of its rows stay until VACUUM, and a set bit would answer them from the index. A sift_col index has no map */
void
sift_drop_clear_vm(Relation rel, const SiftPartDesc *parts, int np, const uint64 *gens, int ngens)
{
	SiftPartGenIndex by_gen;

	if (!sift_tbl_is_table(rel))
		return;
	sift_part_gen_idx_init(&by_gen, parts, np);
	for (int i = 0; i < ngens; i++)
	{
		const SiftPartDesc *p = sift_part_gen_idx_find(&by_gen, gens[i]);

		for (uint64 vblock = p->tid_min >> 16; p->tid_max != 0 && vblock <= p->tid_max >> 16; vblock++)
			sift_tbl_vm_clear(rel, (BlockNumber) vblock);
	}
	sift_part_gen_idx_free(&by_gen);
}

static void
commit_partition(Drop *d, const SiftPartitionEntry *e)
{
	int			np,
				nrel;
	SiftColDeltaAnchor anchor = (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) ? sift_manifest_read_col_delta(d->rel, e) : sift_col_delta_anchor_none();
	SiftPartDesc *parts = sift_manifest_read_parts(d->rel, e, &np);
	SiftDropResult tally = {0};
	SiftTtlPlan *plan = sift_drop_plan(parts, np, d->slot, d->cutoff, d->horizon, sift_drop_time_dirty(&anchor, d->slot), &tally);
	SiftPartDesc *upd = palloc(sizeof(SiftPartDesc) * Max(plan->nmark + plan->nunmark, 1));
	SiftExtent *rel_ext;
	SiftPartGenIndex by_gen;

	sift_part_gen_idx_init(&by_gen, parts, np);
	flagged(&by_gen, plan->mark_gens, plan->nmark, true, upd);
	flagged(&by_gen, plan->unmark_gens, plan->nunmark, false, upd + plan->nmark);
	sift_part_gen_idx_free(&by_gen);
	if (plan->ndrop + plan->nmark + plan->nunmark > 0)
	{
		nrel = sift_tier_release_extents(d->rel, e->part_id, parts, np, plan->drop_gens, plan->ndrop, true, &rel_ext);
		nrel = sift_drop_wide_extents(d->rel, d->store, (uint32) e->part_id, parts, np, plan->drop_gens, plan->ndrop, &rel_ext, nrel);
		if (plan->ndrop > 0)
		{
			sift_drop_clear_vm(d->rel, parts, np, plan->drop_gens, plan->ndrop);
			(void) sift_wal_snapshot_conflict(d->rel, InvalidBlockNumber, ReadNextFullTransactionId());
		}
		sift_manifest_commit(d->rel, &(SiftManifestEdit) {.part_id = e->part_id,.key_lo = e->key_lo,.drop_gens = plan->drop_gens,.ndrop = plan->ndrop,
		.update_parts = upd,.nupdate = plan->nmark + plan->nunmark,.release = rel_ext,.nrelease = nrel});
	}
	d->res.parts_dropped += tally.parts_dropped;
	d->res.rows_dropped += tally.rows_dropped;
	d->res.bytes_reclaimed += tally.bytes_reclaimed;
	d->res.parts_partial += tally.parts_partial;
	d->res.parts_unsettled += tally.parts_unsettled;
	sift_ttl_plan_free(plan);
	pfree(upd);
	pfree(parts);
}

static void
drop_partition(Drop *d, const SiftPartitionEntry *listed)
{
	SiftManifestRead mr;

	if (sift_drop_partition_skippable(listed, d->slot, d->cutoff))
		return;
	if (!sift_tier_lock_compaction(d->rel, listed->part_id))
	{
		d->res.parts_unsettled += count_expired(d, listed);
		return;
	}
	sift_tier_lock_part(d->rel, listed->part_id);
	mr = sift_manifest_read_range(d->rel, listed->key_lo, NULL);
	if (mr.n > 0 && mr.entries[0].part_id == listed->part_id)
		commit_partition(d, &mr.entries[0]);
	sift_tier_unlock_part(d->rel, listed->part_id);
	sift_tier_unlock_compaction(d->rel, listed->part_id);
}

SiftDropResult
sift_f2_drop_parts_before(Relation rel, SiftF2Store *store, int slot, int64 cutoff, FullTransactionId horizon)
{
	Drop		d = {.rel = rel,.store = store,.slot = slot,.cutoff = cutoff,.horizon = horizon};
	SiftManifestRead all;
	SiftColMetaPageData meta;

	if (sift_stage_has_pending(rel))
		sift_stage_flush(rel);
	all = sift_manifest_read_range(rel, NULL, NULL);
	for (int i = 0; i < all.n; i++)
	{
		drop_partition(&d, &all.entries[i]);
		CHECK_FOR_INTERRUPTS();
	}
	if (d.res.parts_dropped == 0)
		return d.res;
	if (store->nscopes > 0)
		(void) sift_f2_scope_refresh(rel);
	sift_fsm_reclaim(rel);
	if (store->opts.vacuum_truncate)
		(void) sift_fsm_tail_shrink(rel);
	meta = sift_col_meta_read(rel);
	sift_pgstat_report_vacuum(rel, (PgStat_Counter) (meta.total_rows - meta.total_dead), (PgStat_Counter) meta.total_dead, GetCurrentTimestamp());
	return d.res;
}

/* ---- the function ---- */

static Relation
open_table(Oid relid)
{
	Relation	rel;

	sift_require_acl(relid, SIFT_RETENTION_ACL);
	rel = relation_open(sift_rel_must_exist(relid), AccessExclusiveLock);
	sift_require_acl(relid, SIFT_RETENTION_ACL);
	sift_rel_reject_partitioned(rel);
	if (rel->rd_rel->relkind == RELKIND_INDEX)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is an index: dropping its parts would leave it without rows its table keeps", RelationGetRelationName(rel)),
						errhint("Run sift.drop_parts_before on the sift_table, or delete the old rows of the heap table and let the index follow.")));
	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	if (sift_meta_resolve_major(rel) != SIFT_FORMAT2_MAJOR)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is a Format 1 sift_table: dropping its parts needs Format 2.0", RelationGetRelationName(rel)),
						errhint("Run sift.upgrade_table(\"%s\").", RelationGetRelationName(rel))));
	if (sift_tbl_check_publication(rel))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is in a logical publication: its subscribers would keep the rows of the dropped parts", RelationGetRelationName(rel)),
						errhint("Remove the table from logical publications or use DELETE to replicate row deletions.")));
	return rel;
}

/* The column the cutoff is read on: the one asked for, else that of the TTL policy, else the first time column of the manifest. It is NOT NULL: a row without a time is out of every bound and would be dropped with its part */
static int
time_slot_of(Relation rel, SiftF2Store *store, const char *col)
{
	SiftTtlPolicy ttl;
	char	   *name = col != NULL ? pstrdup(col) : sift_ttl_policy_of(rel, &ttl) ? ttl.col : NULL;
	int			leaf = store->spec.time_cols[0] - 1;
	AttrNumber	attno;

	if (name == NULL)
	{
		if (leaf < 0)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" has no date or timestamp column to age its parts by", RelationGetRelationName(rel)),
							errhint("Declare a NOT NULL date or timestamp column, or name one of the time columns of the table.")));
		name = pstrdup(NameStr(TupleDescAttr(RelationGetDescr(rel), store->leaf_attno[leaf] - 1)->attname));
	}
	attno = get_attnum(RelationGetRelid(rel), name);
	if (attno == InvalidAttrNumber || sift_tier_time_slot(rel, store, name) < 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("\"%s\" is not a time column of \"%s\"", name, RelationGetRelationName(rel)),
						errhint("The retention runs on one of the first date or timestamp columns of the table (those of key_order first).")));
	if (!TupleDescAttr(RelationGetDescr(rel), attno - 1)->attnotnull)
		ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("column \"%s\" of \"%s\" allows NULL", name, RelationGetRelationName(rel)),
						errhint("A row without a time would be dropped with the part it is in: ALTER TABLE ... ALTER COLUMN %s SET NOT NULL.", quote_identifier(name))));
	return sift_tier_time_slot(rel, store, name);
}

/* The cutoff as the time column reads its own clock: a timestamp and a date are the wall clock of the session zone, as for the comparison of the column with a timestamptz */
static int64
cutoff_in_unit(const SiftF2Store *store, Relation rel, int slot, TimestampTz cutoff)
{
	Oid			type = TupleDescAttr(RelationGetDescr(rel), store->leaf_attno[store->spec.time_cols[slot] - 1] - 1)->atttypid;
	int64		clock = type == TIMESTAMPTZOID ? cutoff : DatumGetTimestamp(DirectFunctionCall1(timestamptz_timestamp, TimestampTzGetDatum(cutoff)));

	return sift_drop_cutoff_units(clock, store->time_unit[slot]);
}

Datum
sift_drop_parts_before(PG_FUNCTION_ARGS)
{
	Relation	rel;
	SiftF2Store *store;
	SiftDropResult r;
	TimestampTz cutoff;
	int			slot;
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false};

	PreventInTransactionBlock(true, "sift.drop_parts_before()");
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("%s must not be null", PG_ARGISNULL(0) ? "rel" : "cutoff")));
	cutoff = PG_GETARG_TIMESTAMPTZ(1);
	if (TIMESTAMP_NOT_FINITE(cutoff))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("cutoff must be a finite timestamp")));
	if (RecoveryInProgress())
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot drop parts during recovery")));
	SIFT_INVARIANT(get_call_result_type(fcinfo, NULL, &tupdesc) == TYPEFUNC_COMPOSITE, "sift.drop_parts_before returns a row type");
	rel = open_table(PG_GETARG_OID(0));
	store = sift_f2_store_open(rel);
	slot = time_slot_of(rel, store, PG_ARGISNULL(2) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(2)));
	r = sift_f2_drop_parts_before(rel, store, slot, cutoff_in_unit(store, rel, slot, cutoff),
								  FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), GetOldestNonRemovableTransactionId(rel)));
	relation_close(rel, NoLock);
	values[0] = Int32GetDatum(r.parts_dropped);
	values[1] = Int64GetDatum(r.rows_dropped);
	values[2] = Int64GetDatum(r.bytes_reclaimed);
	values[3] = Int32GetDatum(r.parts_partial);
	values[4] = Int32GetDatum(r.parts_unsettled);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), values, nulls)));
}
