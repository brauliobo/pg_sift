/*
 * col/f2/core_test.c - test-only SQL functions sift._test_f2_* for format major 2 (SIFT_TESTING builds, registered by pg_sift_test): the metapage, the section table, the allocator,
 * the manifest, the granules, the L0 stage and slab, the deletion vectors, the visibility rules, the compaction and the quarantine set, each on a relation of the sift_store_test
 * access method that sift._test_f2_init turns into an index of format major 2.
 *
 * The rows of a test part are generated: row i of a part built from base_key is the key base_key + i, with the string "row<key>", a measure and a dimension that depend on the
 * key and on a mode (0: no nulls; 1: the measure all null and the dimension constant; 2: nulls in the measure and the string), and the TID tid_base + i; the dump functions
 * regenerate them and compare, so that a test needs no stored expectation.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/f2/test_int.h"
#include "col/granule/cfilt/cc.h"
#include "core/memtrack.h"

PG_FUNCTION_INFO_V1(sift_test_f2_init);
PG_FUNCTION_INFO_V1(sift_test_f2_meta_resize);
PG_FUNCTION_INFO_V1(sift_test_f2_scan_counters);
PG_FUNCTION_INFO_V1(sift_test_f2_prune_counters);
PG_FUNCTION_INFO_V1(sift_test_f2_plan_cache);
PG_FUNCTION_INFO_V1(sift_test_f2_plan_snapshot);
PG_FUNCTION_INFO_V1(sift_test_f2_meta);
PG_FUNCTION_INFO_V1(sift_test_f2_scopes);
PG_FUNCTION_INFO_V1(sift_test_f2_plan_cache_shm);
PG_FUNCTION_INFO_V1(sift_test_f2_plan_cache_drop);
PG_FUNCTION_INFO_V1(sift_test_f2_scope_dir);
PG_FUNCTION_INFO_V1(sift_test_f2_sections);
PG_FUNCTION_INFO_V1(sift_test_f2_add_part);
PG_FUNCTION_INFO_V1(sift_test_f2_drop_part);
PG_FUNCTION_INFO_V1(sift_test_f2_split);
PG_FUNCTION_INFO_V1(sift_test_f2_split_many);
PG_FUNCTION_INFO_V1(sift_test_f2_merge);
PG_FUNCTION_INFO_V1(sift_test_f2_partitions);
PG_FUNCTION_INFO_V1(sift_test_f2_parts);
PG_FUNCTION_INFO_V1(sift_test_f2_range);
PG_FUNCTION_INFO_V1(sift_test_stage_flushes);
PG_FUNCTION_INFO_V1(sift_test_pend_del_reads);
PG_FUNCTION_INFO_V1(sift_test_f2_check);
PG_FUNCTION_INFO_V1(sift_test_f2_check_keys);
PG_FUNCTION_INFO_V1(sift_test_f2_leaves);
PG_FUNCTION_INFO_V1(sift_test_f2_pending_dead);
PG_FUNCTION_INFO_V1(sift_test_f2_array_layout);

static const SiftCellKind f2_kinds[F2_NCOLS] = {SIFT_CELL_INT, SIFT_CELL_STR, SIFT_CELL_INT, SIFT_CELL_INT};
static const uint16 f2_sort[1] = {0};

static const SiftPreaggDecl f2_decl = {.ndims = 1,.dim = {3},.nmeas = 1,.meas = {2}};

SiftPartSpec
f2t_spec(bool preagg)
{
	return (SiftPartSpec) {.ncols = F2_NCOLS,.kinds = f2_kinds,.nsort = 1,.sort_cols = f2_sort,.preagg = preagg,.preagg_decl = &f2_decl};
}

void
f2t_key(int64 v, uint8 *out)
{
	uint64		u = (uint64) v ^ (UINT64CONST(1) << 63);

	memset(out, 0, SIFT_BOUND_BYTES);
	out[0] = 1;
	for (int i = 0; i < 8; i++)
		out[1 + i] = (uint8) (u >> (56 - 8 * i));
}

static bool
f2_unkey(const uint8 *k, int64 *v)
{
	uint64		u = 0;

	if (k[0] == 0)
		return false;
	for (int i = 0; i < 8; i++)
		u = (u << 8) | k[1 + i];
	*v = (int64) (u ^ (UINT64CONST(1) << 63));
	return true;
}

/* An optional bigint argument as a bound: NULL is the all-zero bound */
const uint8 *
f2t_arg_bound(FunctionCallInfo fcinfo, int n, uint8 *buf)
{
	if (PG_ARGISNULL(n))
	{
		memset(buf, 0, SIFT_BOUND_BYTES);
		return buf;
	}
	f2t_key(PG_GETARG_INT64(n), buf);
	return buf;
}

Relation
f2t_open_index(FunctionCallInfo fcinfo, int lockmode)
{
	return relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), lockmode);
}

SiftFsmStats
f2t_fsm_check(Relation rel)
{
	sift_fsm_meta_pool_drain(rel);
	return sift_fsm_check(rel);
}

uint64
f2t_tid(int64 base, int i)
{
	return ((uint64) (base + i / 100) << 16) | (uint64) (i % 100 + 1);
}

void
f2t_gen_row(int mode, int64 k, int64 *m, bool *mnull, int64 *d, bool *dnull, char *s, bool *snull)
{
	*m = ((k * 7) % 100 + 100) % 100;
	*d = ((k % 5) + 5) % 5;
	*mnull = *dnull = *snull = false;
	snprintf(s, 32, "row" INT64_FORMAT, k);
	if (mode == 1)
	{
		*mnull = true;
		*d = 3;
	}
	else if (mode == 2)
	{
		*mnull = k % 3 == 0;
		*snull = k % 7 == 0;
	}
	else if (mode == 3)
		*snull = k % 61 != 0;
}

uint32
f2t_xmin_of(int64 arg)
{
	if (arg == 0)
		return FrozenTransactionId;
	if (arg < 0)
		return GetCurrentTransactionId();
	return (uint32) arg;
}

Datum
f2t_record_result(FunctionCallInfo fcinfo, Datum *values, bool *nulls)
{
	TupleDesc	td;

	get_call_result_type(fcinfo, NULL, &td);
	td = BlessTupleDesc(td);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(td, values, nulls)));
}

ReturnSetInfo *
f2t_srf_begin(FunctionCallInfo fcinfo)
{
	InitMaterializedSRF(fcinfo, 0);
	return (ReturnSetInfo *) fcinfo->resultinfo;
}

void
f2t_srf_put(ReturnSetInfo *rsi, Datum *values, bool *nulls)
{
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
}

/* {exact, recheck, zone_skipped, read, sparse, skip_probed, skip_pruned, part_pruned, compressed, late_skipped, set tests run by the kernels of the stored form (col/granule/cfilt/cc.h), those they left to the codec, pages read ahead} of the scans since the last read (SiftF2ScanStats); the argument true resets them */
Datum
sift_test_f2_scan_counters(PG_FUNCTION_ARGS)
{
	const SiftF2ScanStats *st = &sift_f2_scan_stats;
	uint64		kernel = 0;
	Datum		d[12];
	ArrayType  *a;

	for (int k = 0; k < lengthof(sift_cf_cc_stats.node); k++)
		kernel += sift_cf_cc_stats.node[k];
	d[0] = Int64GetDatum(st->exact), d[1] = Int64GetDatum(st->recheck), d[2] = Int64GetDatum(st->zone_skipped), d[3] = Int64GetDatum(st->read), d[4] = Int64GetDatum(st->sparse);
	d[5] = Int64GetDatum(st->skip_probed), d[6] = Int64GetDatum(st->skip_pruned), d[7] = Int64GetDatum(st->part_pruned), d[8] = Int64GetDatum(st->compressed), d[9] = Int64GetDatum(st->late_skipped);
	d[10] = Int64GetDatum(kernel), d[11] = Int64GetDatum(st->prefetched);
	a = construct_array_builtin(d, 12, INT8OID);
	if (PG_GETARG_BOOL(0))
	{
		sift_f2_scan_stats = (SiftF2ScanStats) {0};
		sift_cf_cc_stats = (SiftCfCcStats) {0};
	}
	PG_RETURN_ARRAYTYPE_P(a);
}

/* {parts a scan opened, parts and partitions its time window ruled out} since the last read; the argument true resets them */
Datum
sift_test_f2_prune_counters(PG_FUNCTION_ARGS)
{
	Datum		d[3] = {Int64GetDatum(sift_f2_scan_stats.parts_read), Int64GetDatum(sift_f2_scan_stats.parts_time_skipped), Int64GetDatum(sift_f2_scan_stats.partitions_time_skipped)};
	ArrayType  *a = construct_array_builtin(d, 3, INT8OID);

	if (PG_GETARG_BOOL(0))
		sift_f2_scan_stats = (SiftF2ScanStats) {0};
	PG_RETURN_ARRAYTYPE_P(a);
}

/* {snapshots served from the cache, reloaded in place, built and kept, built for one statement, part lists read} of the plan cache since the last read; the argument true resets them */
Datum
sift_test_f2_plan_cache(PG_FUNCTION_ARGS)
{
	SiftPlanCacheStats *c = &sift_plan_cache_stats;
	Datum		d[5] = {Int64GetDatum(c->hits), Int64GetDatum(c->refreshes), Int64GetDatum(c->builds), Int64GetDatum(c->private_builds), Int64GetDatum(c->partlists_read)};
	ArrayType  *a = construct_array_builtin(d, 5, INT8OID);

	if (PG_GETARG_BOOL(0))
		*c = (SiftPlanCacheStats) {0};
	PG_RETURN_ARRAYTYPE_P(a);
}

/* {loads out of the snapshot the registry of the manifests in shared memory holds, snapshots this backend gave it} since the last read; the argument true resets them */
Datum
sift_test_f2_plan_cache_shm(PG_FUNCTION_ARGS)
{
	SiftPlanCacheStats *c = &sift_plan_cache_stats;
	Datum		d[2] = {Int64GetDatum(c->shm_loads), Int64GetDatum(c->shm_publishes)};
	ArrayType  *a = construct_array_builtin(d, 2, INT8OID);

	if (PG_GETARG_BOOL(0))
		c->shm_loads = c->shm_publishes = 0;
	PG_RETURN_ARRAYTYPE_P(a);
}

/* The plan cache emptied, as in a backend that has not planned an index yet */
Datum
sift_test_f2_plan_cache_drop(PG_FUNCTION_ARGS)
{
	sift_plan_cache_evict_all();
	PG_RETURN_VOID();
}

/* (index): the partitions, the parts of their lists and the rows the plan cache holds for the index, and the version of the manifest they were read at */
Datum
sift_test_f2_plan_snapshot(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	const SiftPlanSnapshot *snap = sift_plan_cache_get(rel, CurrentMemoryContext);
	int64		nparts = 0;
	Datum		v[4];
	bool		n[4] = {0};

	for (int i = 0; i < snap->npartitions; i++)
		nparts += snap->partlists[i].nparts;
	v[0] = Int32GetDatum(snap->npartitions);
	v[1] = Int64GetDatum(nparts);
	v[2] = Int64GetDatum((int64) snap->total_rows);
	v[3] = Int64GetDatum((int64) snap->manifest_version);
	f2t_srf_put(rsi, v, n);
	index_close(rel, NoLock);
	return (Datum) 0;
}

Datum
sift_test_f2_init(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessExclusiveLock);

	sift_col_meta_init(rel, SIFT_AM_COL, 0, 0x5EED, SIFT_META_FLAG_UNKEYED);
	index_close(rel, NoLock);
	PG_RETURN_VOID();
}

/* (index or sift_table, size): block 0 re-sealed as a metapage of that many bytes, as an index built before the struct grew has it; no WAL; returns the size it had */
Datum
sift_test_f2_meta_resize(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessExclusiveLock);
	Buffer		buf = sift_blk_read(rel, SIFT_METAPAGE_BLKNO, BUFFER_LOCK_EXCLUSIVE);
	int32		was = SIFT_COL_META(BufferGetPage(buf))->common.struct_size;

	sift_meta_seal(BufferGetPage(buf), PG_GETARG_INT32(1));
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
	relation_close(rel, NoLock);
	PG_RETURN_INT32(was);
}

Datum
sift_test_f2_meta(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftColMetaPageData m = sift_col_meta_read(rel);
	Datum		v[12];
	bool		n[12] = {0};

	v[0] = Int32GetDatum(m.common.format_major);
	v[1] = Int32GetDatum(m.common.format_minor);
	v[2] = Int32GetDatum(m.common.struct_size - m.inline_len);
	v[3] = Int64GetDatum(m.high_water);
	v[4] = Int64GetDatum(m.meta_slab_end);
	v[5] = Int32GetDatum(m.npartitions);
	v[6] = Int64GetDatum(m.manifest_version);
	v[7] = Int64GetDatum(m.next_gen);
	v[8] = Int64GetDatum(m.next_part_id);
	v[9] = Int64GetDatum(m.total_rows);
	v[10] = Int64GetDatum(m.total_dead);
	v[11] = Int32GetDatum(m.partdir_level);
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, n);
}

/* (index, first_ord, n): n scope refs of the ordinals first_ord, first_ord + 1, ... swapped into the directory by one commit of partition 1; the refs name no part */
Datum
sift_test_f2_scopes(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	int			first = PG_GETARG_INT32(1),
				n = PG_GETARG_INT32(2);
	uint8		lo[SIFT_BOUND_BYTES] = {0};
	SiftManifestScopeRef *refs = palloc(Max(n, 1) * sizeof(SiftManifestScopeRef));
	SiftManifestEdit e = {.part_id = 1,.key_lo = lo,.update_scopes = refs,.nupdate_scopes = n};
	uint64		version;

	for (int i = 0; i < n; i++)
		refs[i] = (SiftManifestScopeRef) {.scope_ord = first + i,.flags = SIFT_SCOPEREF_ACTIVE,.header_blk = 1000 + first + i,.meta_gen = 7 + first + i};
	version = sift_manifest_commit(rel, &e);
	index_close(rel, NoLock);
	PG_RETURN_INT64(version);
}

Datum
sift_test_f2_scope_dir(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftColMetaPageData m = sift_col_meta_read(rel);
	int			n;
	SiftManifestScopeRef *refs = sift_manifest_read_scopes(rel, &m, &n);

	for (int i = 0; i < n; i++)
	{
		Datum		v[4] = {Int32GetDatum(refs[i].scope_ord), Int32GetDatum(refs[i].flags), Int64GetDatum(refs[i].header_blk), Int64GetDatum(refs[i].meta_gen)};
		bool		nl[4] = {0};

		f2t_srf_put(rsi, v, nl);
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

/* A part of made-up sections, written and read back: the sections, their skipping and the extents */
Datum
sift_test_f2_sections(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	bool		required_unknown = PG_GETARG_BOOL(1);
	int			inline_cap = PG_GETARG_INT32(2);
	uint32		max_extent = PG_GETARG_INT32(3);
	uint64		nbytes = PG_GETARG_INT64(4);
	char	   *big = palloc(nbytes),
			   *small = palloc(100),
			   *odd = palloc(50);
	char		schema[64],
				stats[40];
	SiftSecInput secs[6];
	int			ns = 0,
				skipped = 0,
				good = 0;
	SiftPartIn in = {0};
	SiftPartDesc d;
	SiftPart   *p;
	uint64		gen = sift_manifest_new_gen(rel, 1);
	StringInfoData out;

	for (uint64 i = 0; i < nbytes; i++)
		big[i] = (char) (i * 31 + 7);
	for (int i = 0; i < 100; i++)
		small[i] = (char) i;
	memset(odd, 0x5A, 50);
	memset(schema, 'S', sizeof(schema));
	memset(stats, 'T', sizeof(stats));
	secs[ns++] = (SiftSecInput) {SEC_SCHEMA, SIFT_SEC_FLAG_REQUIRED, schema, sizeof(schema), true};
	secs[ns++] = (SiftSecInput) {SEC_LEAF_STATS, 0, stats, sizeof(stats), true};
	secs[ns++] = (SiftSecInput) {SEC_GRANULES, SIFT_SEC_FLAG_REQUIRED, big, nbytes, false};
	secs[ns++] = (SiftSecInput) {SEC_ZONES, 0, small, 100, false};
	secs[ns++] = (SiftSecInput) {0x02FF, 0, odd, 50, false};
	if (required_unknown)
		secs[ns++] = (SiftSecInput) {0x0FFF, SIFT_SEC_FLAG_REQUIRED, odd, 50, false};
	in = (SiftPartIn) {.part_id = 1,.part_gen = gen,.level = 1,.nrows = 0,.min_xid = InvalidFullTransactionId,.max_xid = InvalidFullTransactionId,
	.sections = secs,.nsections = ns,.inline_extent_cap = inline_cap,.max_extent_blocks = max_extent};
	d = sift_part_write(rel, &in);
	p = sift_part_open(rel, d.header_blk, gen, 1);
	initStringInfo(&out);
	for (int i = 0; i < p->hdr.nsec; i++)
	{
		uint64		len;
		void	   *got = sift_part_read_section(rel, p, p->secdir[i].tag, &len);
		const char *want = p->secdir[i].tag == SEC_GRANULES ? big : (p->secdir[i].tag == SEC_ZONES ? small : p->secdir[i].tag == SEC_SCHEMA ? schema :
																  p->secdir[i].tag == SEC_LEAF_STATS ? stats : odd);

		skipped += !sift_section_known(p->secdir[i].tag);
		good += len == p->secdir[i].len && memcmp(got, want, len) == 0;
	}
	appendStringInfo(&out, "sections=%d verified=%d unknown_skipped=%d extents=%u inline=%u indirect_pages=%d inline_sections=%d zones_inline=%d",
					 p->hdr.nsec, good, skipped, p->hdr.total_extents, p->hdr.ninline_extents, p->nextlist_pages,
					 (int) ((sift_part_section(p, SEC_SCHEMA)->flags & SIFT_SEC_FLAG_INLINE) != 0) + (int) ((sift_part_section(p, SEC_LEAF_STATS)->flags & SIFT_SEC_FLAG_INLINE) != 0),
					 (int) ((sift_part_section(p, SEC_ZONES)->flags & SIFT_SEC_FLAG_INLINE) != 0));
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

static SiftPartDesc
write_rows(Relation rel, uint32 part_id, int n, int64 base, int64 tid_base, uint32 xmin, int level, int mode, bool preagg, bool reverse)
{
	SiftPartSpec spec = f2t_spec(preagg);
	SiftRowBuf *rb = sift_rowbuf_new(&spec);
	SiftColIn	cols[F2_NCOLS];
	SiftRows	r;

	for (int j = 0; j < n; j++)
	{
		int			i = reverse ? n - 1 - j : j;
		int64		k = base + i;
		SiftCell	c[F2_NCOLS] = {{0}};
		char		s[32];
		uint8		key[SIFT_BOUND_BYTES];
		bool		mn,
					dn,
					sn;

		f2t_gen_row(mode, k, &c[2].i, &mn, &c[3].i, &dn, s, &sn);
		c[0].i = k;
		c[1] = (SiftCell) {.isnull = sn,.s = s,.slen = strlen(s)};
		c[2].isnull = mn;
		f2t_key(k, key);
		sift_rowbuf_add(rb, f2t_tid(tid_base, i), xmin, 0, 0, key, c, NULL);
	}
	sift_rowbuf_sort(rb, &spec);
	r = sift_rowbuf_rows(rb, cols, &spec);
	return sift_part_write_rows(rel, part_id, sift_manifest_new_gen(rel, 1), level, level == 0 ? SIFT_TIER_HOT_L0 : SIFT_TIER_WARM_L1, &r);
}

/* (index, part_id, partition lower key, nrows, base_key, tid_base, xmin, level, mode, preagg, reverse) -> generation */
Datum
sift_test_f2_add_part(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES];
	SiftPartDesc d;
	SiftManifestEdit e = {.part_id = PG_GETARG_INT64(1)};

	e.key_lo = f2t_arg_bound(fcinfo, 2, lo);
	d = write_rows(rel, (uint32) e.part_id, PG_GETARG_INT32(3), PG_GETARG_INT64(4), PG_GETARG_INT64(5), f2t_xmin_of(PG_GETARG_INT64(6)), PG_GETARG_INT32(7),
				   PG_GETARG_INT32(8), PG_GETARG_BOOL(9), PG_GETARG_BOOL(10));
	e.add_parts = &d;
	e.nadd = 1;
	sift_manifest_commit(rel, &e);
	index_close(rel, NoLock);
	PG_RETURN_INT64(d.part_gen);
}

SiftPartDesc *
f2t_find_parts(Relation rel, FunctionCallInfo fcinfo, int idarg, int loarg, int *n, SiftPartitionEntry *entry)
{
	uint8		lo[SIFT_BOUND_BYTES];
	SiftManifestRead mr = sift_manifest_read_range(rel, f2t_arg_bound(fcinfo, loarg, lo), NULL);

	if (mr.n == 0 || mr.entries[0].part_id != (uint64) PG_GETARG_INT64(idarg))
		elog(ERROR, "partition " INT64_FORMAT " is not in the manifest", PG_GETARG_INT64(idarg));
	*entry = mr.entries[0];
	return sift_manifest_read_parts(rel, entry, n);
}

SiftPartDesc *
f2t_find_gen(SiftPartDesc *parts, int n, uint64 gen)
{
	for (int i = 0; i < n; i++)
		if (parts[i].part_gen == gen)
			return &parts[i];
	elog(ERROR, "no part of generation " UINT64_FORMAT, gen);
	return NULL;
}

/* (index, part_id, partition lower key, generation) -> the version */
Datum
sift_test_f2_drop_part(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftPartitionEntry entry;
	int			n,
				nb;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftPartDesc *d = f2t_find_gen(parts, n, PG_GETARG_INT64(3));
	SiftPart   *p = sift_part_open(rel, d->header_blk, d->part_gen, (uint32) entry.part_id);
	SiftExtent *blocks = sift_part_blocks(p, &nb);
	SiftExtent *rel_ext = palloc((nb + 1) * sizeof(SiftExtent));
	uint64		gen = d->part_gen;
	SiftManifestEdit e = {.part_id = entry.part_id,.key_lo = entry.key_lo,.drop_gens = &gen,.ndrop = 1};
	int			nrel = nb;
	uint64		version;

	memcpy(rel_ext, blocks, nb * sizeof(SiftExtent));
	if (d->dv_blk != InvalidBlockNumber)
		rel_ext[nrel++] = (SiftExtent) {d->dv_blk, d->dv_nblocks};
	e.release = rel_ext;
	e.nrelease = nrel;
	version = sift_manifest_commit(rel, &e);
	index_close(rel, NoLock);
	PG_RETURN_INT64(version);
}

Datum
sift_test_f2_split(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES],
				sp[SIFT_BOUND_BYTES];
	uint64		id = sift_manifest_split(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, lo), (f2t_key(PG_GETARG_INT64(3), sp), sp));

	index_close(rel, NoLock);
	PG_RETURN_INT64(id);
}

/* n splits at step, 2 * step, ... of the last partition: the number of partitions made */
Datum
sift_test_f2_split_many(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint64		id = PG_GETARG_INT64(1);
	int			n = PG_GETARG_INT32(3);
	int64		step = PG_GETARG_INT64(4);
	uint8		lo[SIFT_BOUND_BYTES],
				sp[SIFT_BOUND_BYTES];

	f2t_arg_bound(fcinfo, 2, lo);
	for (int i = 1; i <= n; i++)
	{
		f2t_key(i * step, sp);
		id = sift_manifest_split(rel, id, lo, sp);
		memcpy(lo, sp, SIFT_BOUND_BYTES);
	}
	index_close(rel, NoLock);
	PG_RETURN_INT32(n);
}

Datum
sift_test_f2_merge(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		a[SIFT_BOUND_BYTES],
				b[SIFT_BOUND_BYTES];

	sift_manifest_merge(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, a), PG_GETARG_INT64(3), f2t_arg_bound(fcinfo, 4, b));
	index_close(rel, NoLock);
	PG_RETURN_VOID();
}

static Datum
bound_datum(const uint8 *k, bool *isnull)
{
	int64		v = 0;

	*isnull = !f2_unkey(k, &v);
	return Int64GetDatum(v);
}

Datum
sift_test_f2_partitions(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		const SiftPartitionEntry *e = &mr.entries[i];
		Datum		v[9];
		bool		n[9] = {0};

		v[0] = Int64GetDatum(e->part_id);
		v[1] = bound_datum(e->key_lo, &n[1]);
		v[2] = bound_datum(e->key_hi, &n[2]);
		v[3] = Int32GetDatum(e->nparts);
		v[4] = Int64GetDatum(e->total_rows);
		v[5] = Int64GetDatum(e->total_dead);
		v[6] = Int32GetDatum(e->l0_count);
		v[7] = BoolGetDatum(e->l0_slab_blk != InvalidBlockNumber);
		v[8] = BoolGetDatum(e->pend_del_blk != InvalidBlockNumber);
		f2t_srf_put(rsi, v, n);
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

Datum
sift_test_f2_parts(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);

	for (int i = 0; i < n; i++)
	{
		Datum		v[9];
		bool		nl[9] = {0};

		v[0] = Int64GetDatum(parts[i].part_gen);
		v[1] = Int32GetDatum(parts[i].level);
		v[2] = Int32GetDatum(parts[i].flags);
		v[3] = Int64GetDatum(parts[i].nrows);
		v[4] = Int64GetDatum(parts[i].ndead);
		v[5] = BoolGetDatum(parts[i].dv_blk != InvalidBlockNumber);
		v[6] = bound_datum(parts[i].key_lo, &nl[6]);
		v[7] = bound_datum(parts[i].key_hi, &nl[7]);
		v[8] = BoolGetDatum(FullTransactionIdIsValid(parts[i].min_xid));
		f2t_srf_put(rsi, v, nl);
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

Datum
sift_test_f2_range(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	uint8		lo[SIFT_BOUND_BYTES],
				hi[SIFT_BOUND_BYTES];
	SiftManifestRead mr = sift_manifest_read_range(rel, PG_ARGISNULL(1) ? NULL : (f2t_key(PG_GETARG_INT64(1), lo), lo),
												   PG_ARGISNULL(2) ? NULL : (f2t_key(PG_GETARG_INT64(2), hi), hi));
	Datum		v[3] = {Int32GetDatum(mr.n), Int32GetDatum(mr.pages), Int64GetDatum(mr.n > 0 ? mr.entries[0].part_id : 0)};
	bool		n[3] = {0};

	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, n);
}

Datum
sift_test_f2_check(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	uint64		parts = sift_manifest_check(rel);
	SiftFsmStats st = f2t_fsm_check(rel);
	char		buf[160];

	snprintf(buf, sizeof(buf), "parts=%llu quarantined=%u/%llu meta_free=%llu/%llu", (unsigned long long) parts, st.quarantine_extents,
			 (unsigned long long) st.quarantine_blocks, (unsigned long long) st.meta_free, (unsigned long long) st.meta_total);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

Datum
sift_test_f2_check_keys(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	char	   *res = sift_f2_check_keys(rel);

	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(res));
}

/* The value an exception kept, as text: a jsonb datum for the leaf of a path, a numeric as its text, else its size */
static char *
payload_text(const SiftColDef *d, const char *p, uint32 len)
{
	char	   *copy = memcpy(palloc(len), p, len);

	if (d->path != NULL && d->path[0] != '\0')
		return JsonbToCString(NULL, &((Jsonb *) copy)->root, len);
	if (d->type == SIFT_COLT_NUMERIC)
		return DatumGetCString(DirectFunctionCall1(numeric_out, PointerGetDatum(copy)));
	return psprintf("bytes:%u", len);
}

static char *
cell_text(SiftCellKind kind, const int64 *i, const double *f, const SiftStrCell *s, int k)
{
	if (kind == SIFT_CELL_INT)
		return psprintf(INT64_FORMAT, i[k]);
	if (kind == SIFT_CELL_F8)
		return psprintf("%.17g", f[k]);
	return pnstrdup(s[k].p, s[k].len);
}

typedef struct LeafOut
{
	ReturnSetInfo *rsi;
	const SiftColDef *def;
	SiftCellKind kind;
	SiftGranuleView *v;
	int			col;
	const uint64 *tid;
	int64	   *i;
	double	   *f;
	SiftStrCell *s;
	bool	   *isnull;
	uint8	   *tag;
	const bool *dead;
	MemoryContext row_cx;		/* what one value allocates (its payload decode included) is dropped when it is put */
} LeafOut;

/* One value of a leaf: the row's tid, the leaf, the element (-1: the leaf is the row's own), the value, its tag and the payload of its exception */
static void
leaf_put(const LeafOut *o, int row, int elem, int k)
{
	Datum		v[6];
	bool		n[6] = {0};
	uint32		plen = 0;
	const char *pay;
	ItemPointerData tid;
	MemoryContext old;

	if (o->dead[row])
		return;
	old = MemoryContextSwitchTo(o->row_cx);
	pay = o->tag[k] ? sift_granule_exc_payload(o->v, o->col, k, &plen) : NULL;
	sift_f2_tid_set(&tid, o->tid[row]);
	v[0] = ItemPointerGetDatum(memcpy(palloc(sizeof(tid)), &tid, sizeof(tid)));
	v[1] = CStringGetTextDatum(o->def->path != NULL && o->def->path[0] != '\0' ? o->def->path : o->def->name);
	v[2] = Int32GetDatum(elem);
	n[2] = elem < 0;
	n[3] = o->isnull[k];
	v[3] = n[3] ? (Datum) 0 : CStringGetTextDatum(cell_text(o->kind, o->i, o->f, o->s, k));
	v[4] = Int32GetDatum(o->tag[k]);
	n[4] = o->tag[k] == 0;
	n[5] = pay == NULL;
	v[5] = n[5] ? (Datum) 0 : CStringGetTextDatum(payload_text(o->def, pay, plen));
	f2t_srf_put(o->rsi, v, n);
	MemoryContextSwitchTo(old);
	MemoryContextReset(o->row_cx);
}

static int
addr_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* The rows of granule g the active snapshot sees deleted, by the deletion vectors of the part and the pending deletes of its partition */
static void
dead_rows(const SiftDv *dv, uint32 g, const uint64 *tid, int nr, const uint64 *pend, int npend, bool *dead)
{
	uint64		mask[SIFT_DV_MASK_WORDS];
	const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
	bool		any = dg != NULL && sift_dv_mask64(dg, GetActiveSnapshot(), mask) > 0;

	for (int r = 0; r < nr; r++)
		dead[r] = (any && ((mask[r >> 6] >> (r & 63)) & 1)) || (npend > 0 && bsearch(&tid[r], pend, npend, sizeof(uint64), addr_cmp) != NULL);
}

static void
leaves_of_granule(ReturnSetInfo *rsi, Relation rel, const SiftF2Rel *f, const SiftColSchema *schema, const SiftPart *p, uint32 g, const SiftDv *dv, const uint64 *pend, int npend)
{
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, p, g, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
	int			nr = gd.nrows;
	uint64	   *tid = palloc(nr * sizeof(uint64));
	int64	  **len = palloc0(f->ncols * sizeof(int64 *));
	bool	  **lnull = palloc0(f->ncols * sizeof(bool *));
	bool	   *dead = palloc(nr);
	MemoryContext row_cx = sift_mem_allocset_create(CurrentMemoryContext, "f2 test leaf value");

	sift_granule_tids(&v, tid);
	dead_rows(dv, g, tid, nr, pend, npend, dead);
	for (int c = 0; c < f->ncols; c++)
	{
		int			n = sift_granule_chunk_len(&v, c);
		LeafOut		o = {.rsi = rsi,.def = &schema->cols[c],.kind = f->kinds[c],.v = &v,.col = c,.tid = tid,.row_cx = row_cx};

		o.i = palloc(Max(n, 1) * sizeof(int64));
		o.f = palloc(Max(n, 1) * sizeof(double));
		o.s = palloc(Max(n, 1) * sizeof(SiftStrCell));
		o.isnull = palloc(Max(n, 1));
		o.tag = palloc0(Max(n, 1));
		o.dead = dead;
		if (f->kinds[c] == SIFT_CELL_INT)
			sift_granule_ints(&v, c, o.i, o.isnull);
		else if (f->kinds[c] == SIFT_CELL_F8)
			sift_granule_f8(&v, c, o.f, o.isnull);
		else
		{
			sift_granule_strs(&v, c, o.s);
			for (int k = 0; k < n; k++)
				o.isnull[k] = o.s[k].isnull;
		}
		sift_granule_tags(&v, c, o.tag);
		if (f->parent[c] < 0)
		{
			len[c] = o.i;
			lnull[c] = o.isnull;
			for (int r = 0; r < nr; r++)
				leaf_put(&o, r, -1, r);
			continue;
		}
		for (int r = 0, at = 0; r < nr; r++)
		{
			int			g1 = f->parent[c];
			int64		cnt = lnull[g1][r] ? 0 : len[g1][r];

			for (int j = 0; j < cnt; j++)
				leaf_put(&o, r, j, at++);
		}
	}
	MemoryContextDelete(row_cx);
}

/* (tid, leaf, element, value, tag, payload) of every value of every leaf of the parts, the elements of the arrays apart; the rows of a slab or a stage are not in it */
Datum
sift_test_f2_leaves(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftF2Rel  *f = sift_f2_rel(rel);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		int			np,
					npend;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);
		uint64	   *pend = sift_f2_pending_dead(rel, &mr.entries[i], GetActiveSnapshot(), &npend);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *p = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);
			SiftDv	   *dv = NULL;

			if (pd[k].dv_blk != InvalidBlockNumber)
			{
				SiftGranuleDesc *dir = sift_part_granule_dir(rel, p);
				uint16	   *rows = palloc(Max(p->hdr.ngranules, 1) * sizeof(uint16));

				for (uint32 g = 0; g < p->hdr.ngranules; g++)
					rows[g] = dir[g].nrows;
				dv = sift_dv_read(rel, &pd[k], (uint32) mr.entries[i].part_id, rows);
			}
			for (uint32 g = 0; g < p->hdr.ngranules; g++)
				leaves_of_granule(rsi, rel, f, sift_col_rel(rel)->schema, p, g, dv, pend, npend);
		}
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

/* (partition, address) of every pending delete of the relation (an index or a sift_table) that the active snapshot sees, as sift_f2_pending_dead gives them */
Datum
sift_test_f2_pending_dead(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		int			npend;
		uint64	   *pend = sift_f2_pending_dead(rel, &mr.entries[i], GetActiveSnapshot(), &npend);

		for (int k = 0; k < npend; k++)
		{
			Datum		v[2] = {Int64GetDatum((int64) mr.entries[i].part_id), Int64GetDatum((int64) pend[k])};
			bool		n[2] = {0};

			f2t_srf_put(rsi, v, n);
		}
	}
	relation_close(rel, NoLock);
	return (Datum) 0;
}

/* The 64-row blocks of the lengths of an array column hold the offsets of the elements of their rows, and the elements are those the column of an element holds */
static void
check_layout(Relation rel, const SiftGranuleView *v, int col, int child, uint32 g, uint64 *blocks)
{
	SiftArrayLayout lay;
	uint32		nb;

	if ((v->hdr->chunks[col + 1].enc & SIFT_ENC_MASK) == SIFT_CENC_ALL_NULL && sift_granule_chunk_len(v, child) == 0)
		return;
	if (!sift_granule_array_layout(v, col, &lay) || lay.n_parents != v->hdr->nrows || lay.n_elems != (uint32) sift_granule_chunk_len(v, child))
		SIFT_CORRUPT(rel, "Granule %u: the lengths of column %d do not give the elements of column %d.", g, col, child);
	nb = (lay.n_parents + 63) / 64;
	for (uint32 b = 0; b <= nb; b++)
		if (lay.block_elem_off[b] != lay.offsets[Min(b * 64, lay.n_parents)])
			SIFT_CORRUPT(rel, "Granule %u: block %u of column %d starts at element %u, not %u.", g, b, col, lay.block_elem_off[b], lay.offsets[Min(b * 64, lay.n_parents)]);
	for (uint32 r = 0; r < lay.n_parents; r++)
		if (lay.offsets[r + 1] - lay.offsets[r] != lay.lengths[r])
			SIFT_CORRUPT(rel, "Granule %u: row %u of column %d has offsets that are not its length.", g, r, col);
	*blocks += nb;
}

/* Checks the array layout of every list column of every granule; ERROR when one is wrong, else the number of granules, lists and blocks it checked */
Datum
sift_test_f2_array_layout(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftF2Rel  *f = sift_f2_rel(rel);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	uint64		granules = 0,
				lists = 0,
				blocks = 0;

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *p = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);

			for (uint32 g = 0; g < p->hdr.ngranules; g++)
			{
				SiftGranuleDesc gd;
				char	   *rec = sift_part_read_granule(rel, p, g, &gd);
				SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);

				for (int c = 0; c < f->ncols; c++)
					if (f->parent[c] >= 0 && (c == 0 || f->parent[c - 1] != f->parent[c]))
					{
						check_layout(rel, &v, f->parent[c], c, g, &blocks);
						lists++;
					}
				granules++;
			}
		}
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("granules=%llu lists=%llu blocks=%llu ok", (unsigned long long) granules, (unsigned long long) lists, (unsigned long long) blocks)));
}

Datum
sift_test_stage_flushes(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) sift_stage_flushes);
}

Datum
sift_test_pend_del_reads(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) sift_pend_del_reads);
}

#endif							/* SIFT_TESTING */
