/*
 * col/v1/test/core.c - the test hooks of the column index: formats, counters, dictionary tests
 * what a regression test reads from a segment
 * and the selectivity and order checks
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/relation.h"
#include "access/visibilitymap.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "core/maint/maint.h"
#include "col/am.h"
#include "col/v1/gorder/gorder.h"
#include "col/v1/order/bound.h"
#include "col/v1/order/sort.h"
#include "col/v1/pagg.h"
#include "core/dirpart.h"
#include "core/sql/sql.h"
#include "core/visibility/visibility.h"
#endif							/* SIFT_TESTING */
#include "col/v1/test/int.h"
#include "core/catname.h"
#include "core/store/store.h"
#include "core/store/test_int.h"

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_col_gorder);

PG_FUNCTION_INFO_V1(sift_test_col_unit_of_block);

PG_FUNCTION_INFO_V1(sift_test_col_formats);

PG_FUNCTION_INFO_V1(sift_test_col_segment_keys);
PG_FUNCTION_INFO_V1(sift_test_col_part_bounds);
PG_FUNCTION_INFO_V1(sift_test_col_part_shift);
PG_FUNCTION_INFO_V1(sift_test_col_cut_near);
PG_FUNCTION_INFO_V1(sift_test_col_part_damage);
PG_FUNCTION_INFO_V1(sift_test_col_adopt);

PG_FUNCTION_INFO_V1(sift_test_col_mirror);

PG_FUNCTION_INFO_V1(sift_test_col_cuts);
PG_FUNCTION_INFO_V1(sift_test_col_cut_probes);
PG_FUNCTION_INFO_V1(sift_test_col_cut_sort_rows);
PG_FUNCTION_INFO_V1(sift_test_col_cut_sort_disk_kb);
PG_FUNCTION_INFO_V1(sift_test_col_cut_sort_cut);
PG_FUNCTION_INFO_V1(sift_test_col_cut_sorts_reset);

PG_FUNCTION_INFO_V1(sift_test_seg_flip);

PG_FUNCTION_INFO_V1(sift_test_col_dict_count);

PG_FUNCTION_INFO_V1(sift_test_eq_sel);

PG_FUNCTION_INFO_V1(sift_test_eq_sel_calls);
PG_FUNCTION_INFO_V1(sift_test_vet_offsets);
PG_FUNCTION_INFO_V1(sift_test_gorder_decoded);

PG_FUNCTION_INFO_V1(sift_test_key_sorts);
PG_FUNCTION_INFO_V1(sift_test_vis_counts);

PG_FUNCTION_INFO_V1(sift_test_col_dict_tsig);

PG_FUNCTION_INFO_V1(sift_test_col_zone_flags);

/* sift._test_col_formats(index): the column format minor of every immutable segment, by generation, whether it holds the leaf statistics block, and whether its string roots are flagged (SIFT_SEG_ROOTS) */
Datum
sift_test_col_formats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		Datum		v[4] = {Int64GetDatum(snap->segs[i].gen), Int32GetDatum(snap->segs[i].format),
							BoolGetDatum((snap->segs[i].flags & SIFT_SEG_LSTATS) != 0), BoolGetDatum((snap->segs[i].flags & SIFT_SEG_ROOTS) != 0)};
		bool		nulls[4] = {false};

		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_segment_keys(index): for every immutable segment cut by a key, its units and rows and the range of its first key leaf over its units (lo > hi: no value): the segments of a cut by key that keeps each key in one segment share no value */
Datum
sift_test_col_segment_keys(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		int64		lo = PG_INT64_MAX,
					hi = PG_INT64_MIN;
		Datum		v[5];
		bool		nulls[5] = {false};

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || !(snap->segs[i].flags & SIFT_SEG_BYKEY))
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftCZone	z;

			sift_cseg_zone(seg, seg->cut[0], u, &z);
			if (z.nvals == z.nnull)
				continue;
			lo = Min(lo, z.min);
			hi = Max(hi, z.max);
		}
		v[0] = Int64GetDatum(snap->segs[i].gen);
		v[1] = Int32GetDatum((int32) seg->nsb);
		v[2] = Int64GetDatum((int64) snap->segs[i].ndocs);
		v[3] = Int64GetDatum(lo);
		v[4] = Int64GetDatum(hi);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_adopt(index): the step of the maintenance that records the partitions of a key-cut index that none was: whether it recorded any */
Datum
sift_test_col_adopt(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), RowExclusiveLock, "sift_col");
	bool		recorded;

	sift_maint_lock(index, true);
	recorded = sift_col_adopt_partitions(index);
	sift_maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_BOOL(recorded);
}

typedef struct PartDamage
{
	uint64		gen;
	SiftSegEntry entry;
	uint8		lo[SIFT_BOUND_BYTES];
} PartDamage;

static void
damage_switch(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	PartDamage *d = arg;

	sift_dir_part_replace(&d->gen, 1, &d->entry, 1, &d->lo);
}

/*
 * sift._test_col_part_damage(index, k, how): records a wrong bound for partition k (of one segment): how 'equal' the bound of the partition before it, 'high' the greatest row of its segment,
 * 'low' the greatest row of the segment before it (k >= 1 for 'equal' and 'low'). col_verify must refuse the directory
 */
Datum
sift_test_col_part_damage(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), RowExclusiveLock, "sift_col");
	int			k = PG_GETARG_INT32(1);
	char	   *how = text_to_cstring(PG_GETARG_TEXT_PP(2));
	SiftDirSnapshot *snap = sift_dir_read(index);
	SiftDirPart *part;
	SiftSegEntry *segs;
	int			np = sift_dir_parts_read(index, snap, &segs, &part);
	bool		same = strcmp(how, "equal") == 0,
				high = strcmp(how, "high") == 0;
	int			from = high ? k : k - 1;
	PartDamage	d;
	SiftColSegment *seg;
	SiftBound	b;

	if (k < 0 || k >= np || part[k].n != 1 || from < 0 || (!same && !high && strcmp(how, "low") != 0))
		elog(ERROR, "sift test: no partition %d of one segment to damage as \"%s\"", k, how);
	d.entry = segs[part[k].first];
	d.gen = d.entry.gen;
	if (same)
		memcpy(d.lo, part[from].lo, SIFT_BOUND_BYTES);
	else
	{
		seg = sift_cseg_open(index, &segs[part[from].first], CurrentMemoryContext);
		if (!sift_colk_segment_bound(seg, true, &b))
			elog(ERROR, "sift test: the segment of partition %d holds no row", from);
		sift_bound_pack(&b, d.lo);
		sift_cseg_close(seg);
	}
	sift_maint_lock(index, true);
	sift_dir_update(index, &(SiftDirChange) {.fn = damage_switch,.arg = &d,.extra = InvalidBuffer});
	sift_maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

typedef struct PartShift
{
	uint32		first;
	uint32		n;
	uint8		lo[SIFT_BOUND_BYTES];
} PartShift;

static void
shift_partition(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const PartShift *ps = arg;
	uint64	   *gens = palloc(ps->n * sizeof(uint64));

	for (uint32 i = 0; i < ps->n; i++)
		gens[i] = dir->segs[ps->first + i].gen;
	sift_dir_part_replace(gens, ps->n, &dir->segs[ps->first], ps->n, (const uint8 (*)[SIFT_BOUND_BYTES]) ps->lo);
}

/* sift._test_col_cut_near(held, due_at, slack_pct, open): is a segment of held bytes done after a unit (col/v1/order/bound.c) */
Datum
sift_test_col_cut_near(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(sift_colk_cut_near(PG_GETARG_INT64(0), PG_GETARG_INT64(1), PG_GETARG_INT32(2), PG_GETARG_BOOL(3)));
}

/* sift._test_col_part_shift(index, k): records the lower bound of the partition k+1 as that of the partition k, which then starts above rows it holds: what the check of a partition's rows finds */
Datum
sift_test_col_part_shift(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), RowExclusiveLock, "sift_col");
	int			k = PG_GETARG_INT32(1);
	SiftDirSnapshot *snap = sift_dir_read(index);
	SiftDirPart *part;
	SiftSegEntry *segs;
	SiftDirChain chain;
	PartShift	ps;
	int			np;

	if (snap->nchain == 0)
		elog(ERROR, "sift test: the directory of \"%s\" has no partitions", RelationGetRelationName(index));
	(void) sift_dirchain_read(index, snap->chain[0], snap->version, 0, &segs, &chain);
	np = sift_dirchain_parts(&chain, &part);
	if (k < 0 || k + 1 >= np)
		elog(ERROR, "sift test: partition %d has no partition after it (%d)", k, np);
	ps = (PartShift) {.first = part[k].first,.n = part[k].n};
	memcpy(ps.lo, part[k + 1].lo, SIFT_BOUND_BYTES);
	MAINT_LOCK(index, ShareLock);
	sift_dir_update(index, &(SiftDirChange) {.fn = shift_partition,.arg = &ps,.extra = InvalidBuffer});
	MAINT_UNLOCK(index, ShareLock);
	index_close(index, RowExclusiveLock);
	PG_RETURN_VOID();
}

/* sift._test_col_part_bounds(index): for every partition of the directory chain, its entries and whether its lower bound is empty, whether the rows of its segments are all from it, and below the bound of the next partition */
Datum
sift_test_col_part_bounds(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *snap = sift_dir_read(index);
	SiftDirPart *part;
	SiftSegEntry *segs;
	int			np = sift_dir_parts_read(index, snap, &segs, &part);

	InitMaterializedSRF(fcinfo, 0);
	for (int k = 0; k < np; k++)
	{
		SiftBound	lo,
					next;
		bool		from_lo = true,
					below_next = true;
		Datum		v[6];
		bool		nulls[6] = {false};
		StringInfoData text;

		if (!sift_bound_unpack(part[k].lo, &lo) || (k + 1 < np && !sift_bound_unpack(part[k + 1].lo, &next)))
			elog(ERROR, "sift test: the bound of a partition is no bound");
		for (uint32 e = part[k].first; e < part[k].first + part[k].n; e++)
		{
			SiftColSegment *seg = sift_cseg_open(index, &segs[e], CurrentMemoryContext);

			from_lo &= sift_colk_rows_within(seg, &lo, NULL);
			below_next &= sift_colk_rows_within(seg, NULL, k + 1 < np ? &next : NULL);
			sift_cseg_close(seg);
		}
		initStringInfo(&text);
		for (uint32 c = 0; c < SIFT_BOUND_COMPS; c++)
		{
			int64		key;
			SiftComp	kind = sift_bound_comp(&lo, c, &key);

			if (kind == SIFT_COMP_NONE)
				break;
			appendStringInfoString(&text, c > 0 ? "," : "");
			if (kind == SIFT_COMP_NULL)
				appendStringInfoString(&text, "NULL");
			else
				appendStringInfo(&text, INT64_FORMAT, key);
		}
		v[0] = Int32GetDatum((int32) part[k].first);
		v[1] = Int32GetDatum((int32) part[k].n);
		v[5] = CStringGetTextDatum(text.data);
		v[2] = BoolGetDatum(lo.len == 0);
		v[3] = BoolGetDatum(from_lo);
		v[4] = BoolGetDatum(below_next);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_cuts(index): for every immutable segment, whether it is cut by a key (SIFT_SEG_BYKEY) or by TID range (SIFT_SEG_BYTID) and the leaves it records it is cut by (SIFT_SEG_CUTREC; empty: none) */
Datum
sift_test_col_cuts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		Datum		leaves[SIFT_COL_CUT_MAX];
		Datum		v[4];
		bool		nulls[4] = {false};

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		for (int c = 0; c < seg->ncut; c++)
			leaves[c] = Int32GetDatum(seg->cut[c]);
		v[0] = Int64GetDatum(snap->segs[i].gen);
		v[1] = BoolGetDatum((snap->segs[i].flags & SIFT_SEG_BYKEY) != 0);
		v[2] = BoolGetDatum((snap->segs[i].flags & SIFT_SEG_BYTID) != 0);
		v[3] = PointerGetDatum(construct_array_builtin(leaves, seg->ncut, INT4OID));
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_cut_probes(): how many segments of unrecorded cut this backend has checked unit by unit (sift_colk_streams) */
Datum
sift_test_col_cut_probes(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_test_cut_probes);
}

/* sift._test_col_cut_sort_rows() and _disk_kb(): the rows the merges of this backend sorted by the sort of col/v1/order/sort.c (the rows of an input that did not fit maintenance_work_mem) and the kB of temporary files it used */
Datum
sift_test_col_cut_sort_rows(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_test_cut_sort_rows);
}

Datum
sift_test_col_cut_sort_disk_kb(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_test_cut_sort_disk_kb);
}

Datum
sift_test_col_cut_sort_cut(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_test_cut_sort_cut);
}

Datum
sift_test_col_cut_sorts_reset(PG_FUNCTION_ARGS)
{
	sift_test_cut_sort_rows = sift_test_cut_sort_disk_kb = sift_test_cut_sort_cut = 0;
	PG_RETURN_VOID();
}

/* sift._test_col_mirror(index): the ranges of the stream that the mirror in the header page of each immutable segment keeps, and the bytes of the whole block */
Datum
sift_test_col_mirror(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint64		off;
		uint32		len,
					block;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		for (uint32 r = 0; sift_cseg_mirror_range(seg, r, &off, &len, &block); r++)
		{
			Datum		v[5] = {Int64GetDatum(snap->segs[i].gen), Int32GetDatum(r), Int64GetDatum(off), Int32GetDatum(len), Int32GetDatum(block)};
			bool		nulls[5] = {false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_seg_flip(off, mask): the byte at that offset of every stream this backend reads is read as itself xor the mask (a negative offset: none) */
Datum
sift_test_seg_flip(PG_FUNCTION_ARGS)
{
	int64		off = PG_GETARG_INT64(0);

	sift_test_seg_flip_at = off < 0 ? UINT64_MAX : (uint64) off;
	sift_test_seg_flip_mask = (uint8) PG_GETARG_INT32(1);
	PG_RETURN_VOID();
}

/* sift._test_col_dict_tsig(index, leaf, needle): for each immutable segment whether the dictionary of the leaf has trigram signatures, and how many of its blocks a substring test of needle reads */
Datum
sift_test_col_dict_tsig(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	int			leaf = PG_GETARG_INT32(1);
	text	   *needle = PG_GETARG_TEXT_PP(2);
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint32		passed = 0,
					total = 0;
		bool		signed_;
		Datum		v[4];
		bool		nulls[4] = {false};

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		signed_ = sift_cseg_dict_tsig(seg, leaf, VARDATA_ANY(needle), VARSIZE_ANY_EXHDR(needle), &passed, &total);
		v[0] = Int64GetDatum(snap->segs[i].gen);
		v[1] = BoolGetDatum(signed_);
		v[2] = Int32GetDatum(passed);
		v[3] = Int32GetDatum(total);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_dict_count(index, leaf, op, value): for each immutable segment whether its dictionary of the leaf holds row counts and the rows it says pass op ('eq', 'lt', 'le', 'gt', 'ge', 'prefix') against value */
Datum
sift_test_col_dict_count(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	int			leaf = PG_GETARG_INT32(1);
	char	   *opname = text_to_cstring(PG_GETARG_TEXT_PP(2));
	text	   *val = PG_GETARG_TEXT_PP(3);
	SiftColValue c = {.len = VARSIZE_ANY_EXHDR(val), .v.s = VARDATA_ANY(val)};
	SiftColOp	op = strcmp(opname, "eq") == 0 ? SIFT_COLOP_EQ : strcmp(opname, "lt") == 0 ? SIFT_COLOP_LT : strcmp(opname, "le") == 0 ? SIFT_COLOP_LE :
		strcmp(opname, "gt") == 0 ? SIFT_COLOP_GT : strcmp(opname, "ge") == 0 ? SIFT_COLOP_GE : SIFT_COLOP_PREFIX;
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint64		rows = 0;
		bool		counted;
		Datum		v[3];
		bool		nulls[3] = {false};

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		counted = sift_cseg_dict_count(seg, leaf, op, &c, &rows);
		v[0] = Int64GetDatum(snap->segs[i].gen);
		v[1] = BoolGetDatum(counted);
		v[2] = Int64GetDatum(rows);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_zone_flags(index): the stored units, and how many of them have a zone of some leaf flagged SIFT_COLZ_JNULL (a flag of column format minor 6 and later) */
Datum
sift_test_col_zone_flags(PG_FUNCTION_ARGS)
{
	TestScan	ts;
	SiftColUnit unit;
	int64		units = 0,
				jnull = 0;
	Datum		v[2];
	bool		nulls[2] = {false};

	ctest_scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (ctest_test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u;
		bool		flagged = false;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		units++;
		for (int col = 0; col < sift_col_rel(ts.index)->schema->ncols; col++)
		{
			SiftCZone	z;

			sift_cseg_zone(seg, col, u, &z);
			flagged |= (z.flags & SIFT_COLZ_JNULL) != 0;
		}
		jnull += flagged;
	}
	ctest_scan_close(&ts);
	v[0] = Int64GetDatum(units);
	v[1] = Int64GetDatum(jnull);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/* sift._test_col_unit_of_block(index, blk): for every immutable segment, the unit that holds the rows of heap block blk: 'unit', 'none' (a segment cut by superblock with no row there) or 'unknown' (a segment cut by key) */
Datum
sift_test_col_unit_of_block(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	BlockNumber blk = (BlockNumber) PG_GETARG_INT64(1);
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		Datum		v[4];
		bool		nulls[4] = {false};
		uint32		u;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		u = sift_cseg_unit_of_block(seg, blk);
		v[0] = Int32GetDatum(i);
		v[1] = BoolGetDatum(sift_cseg_keyed(&seg->entry));
		v[2] = Int64GetDatum(u);
		nulls[2] = u >= seg->nsb;
		v[3] = CStringGetTextDatum(u == SIFT_CUNIT_UNKNOWN ? "unknown" : u >= seg->nsb ? "none" : "unit");
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* (seg, pos, key, tid, null_row): the entries of the global order of leaf (by its column number) in every immutable segment that has one, in the order stored */
Datum
sift_test_col_gorder(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	int			leaf = PG_GETARG_INT32(1);
	SiftDirSnapshot *snap;
	int64		kv[SIFT_CGORD_CAP];
	uint64		av[SIFT_CGORD_CAP];

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint32		entries,
					values;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		if (sift_cseg_gorder(seg, leaf, &entries, &values))
			for (uint32 c = 0, base = 0; base < entries; c++)
			{
				uint32		m = sift_cseg_gorder_chunk(seg, leaf, c, kv, av);

				for (uint32 k = 0; k < m; k++)
				{
					ItemPointerData tid;
					Datum		v[5];
					bool		nulls[5] = {false};
					uint32		pos = base + k;

					ItemPointerSet(&tid, (BlockNumber) (av[k] >> 16), (OffsetNumber) (av[k] & 0xFFFF));
					v[0] = Int64GetDatum(i);
					v[1] = Int32GetDatum(pos);
					v[2] = Int64GetDatum(kv[k]);
					v[3] = ItemPointerGetDatum(&tid);
					v[4] = BoolGetDatum(pos >= values);
					nulls[2] = pos >= values;
					tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
				}
				base += m;
			}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* the restriction estimator of the operator sift._test_eq (= with a count): scalararraysel calls it once for each element each time it estimates the clause */
static int64 eq_sel_calls;

Datum
sift_test_eq_sel(PG_FUNCTION_ARGS)
{
	eq_sel_calls++;
	return DirectFunctionCall4Coll(eqsel, PG_GET_COLLATION(), PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), PG_GETARG_DATUM(2), PG_GETARG_DATUM(3));
}

/* the calls since the last read; the argument true resets the count */
SIFT_TEST_COUNTER_FN(sift_test_eq_sel_calls, eq_sel_calls)

/* the row offsets decoded to vet rows against the heap since the last read; the argument true resets the count */
SIFT_TEST_COUNTER_FN(sift_test_vet_offsets, sift_col_vet_offsets)

/* the entries of order array chunks decoded since the last read; the argument true resets the count */
SIFT_TEST_COUNTER_FN(sift_test_gorder_decoded, sift_col_gorder_decoded)

/* the sorts of an IN list by the cost estimates since the last read; the argument true resets the count */
SIFT_TEST_COUNTER_FN(sift_test_key_sorts, sift_col_key_sorts)

/*
 * {the all-visible pages, the clean superblocks} of the first nblocks pages of a table (the table's own size when nblocks is negative), by sift_vis_count and, as an oracle, page by page by
 * visibilitymap_get_status (a superblock is clean when every page it has is all-visible)
 */
Datum
sift_test_vis_counts(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	int64		arg = PG_GETARG_INT64(1);
	BlockNumber nblocks = arg < 0 ? RelationGetNumberOfBlocks(rel) : (BlockNumber) arg,
				visible,
				clean;
	Buffer		vmbuf = InvalidBuffer;
	int64		ov = 0,
				oc = 0;
	bool		all = true;
	Datum		d[4];
	ArrayType  *a;

	sift_vis_count(rel, nblocks, &visible, &clean);
	for (BlockNumber b = 0; b < nblocks; b++)
	{
		bool		v = (visibilitymap_get_status(rel, b, &vmbuf) & VISIBILITYMAP_ALL_VISIBLE) != 0;

		ov += v;
		all &= v;
		if ((b + 1) % SIFT_SB_PAGES == 0 || b + 1 == nblocks)
		{
			oc += all;
			all = true;
		}
	}
	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);
	relation_close(rel, AccessShareLock);
	d[0] = Int64GetDatum(visible);
	d[1] = Int64GetDatum(clean);
	d[2] = Int64GetDatum(ov);
	d[3] = Int64GetDatum(oc);
	a = construct_array_builtin(d, 4, INT8OID);
	PG_RETURN_ARRAYTYPE_P(a);
}
#endif							/* SIFT_TESTING */
