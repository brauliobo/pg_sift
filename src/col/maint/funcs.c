/*
 * col/maint/funcs.c - the SQL functions of the maintenance: due, status, maintain,
 * promote, merge, index info, segments and leaves
 * what an operator calls to see or drive a sift_col index
 */
#include "postgres.h"
#include "access/relation.h"
#include "funcapi.h"
#include "core/ops.h"
#include "core/sql.h"
#include "col/maint/maint_int.h"

/* sift.col_maintenance_due(index, automatic): why the rewrite is due now (dirty units, fragmented units, write buffer rows), NULL if it is not; automatic: as the server's own */
Datum
sift_col_maintenance_due(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open(&maint_col_ops, PG_GETARG_OID(0), AccessShareLock);
	const char *why = maint_rewrite_due(index, PG_GETARG_BOOL(1));

	index_close(index, AccessShareLock);
	if (why == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(why));
}

/*
 * sift.col_maintenance_status(index): where the chain stands (the table is vacuumed when the rows changed since its last VACUUM reach vacuum_at, the
 * index is rewritten after that VACUUM), and blocked_by: what keeps a worker from vacuuming it or rewriting the index, empty when nothing does.
 */
Datum
sift_col_maintenance_status(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open(&maint_col_ops, PG_GETARG_OID(0), AccessShareLock);
	Relation	heap = relation_open(index->rd_index->indrelid, AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Datum		v[7],
				blocked[8];
	bool		nulls[7] = {0};
	int64		changed = 0,
				threshold = 0;
	TimestampTz last = 0;
	const char *why;
	int			gates = 0,
				nblocked = 0;

	sift_dir_read(index, snap);
	why = maint_rewrite_check(index, true, &gates);
	if (!sift_col_auto_merge)
		blocked[nblocked++] = CStringGetTextDatum("sift.col_auto_merge is off");
	if (!sift_async_maintenance)
		blocked[nblocked++] = CStringGetTextDatum("sift.async_maintenance is off");
	if (RecoveryInProgress())
		blocked[nblocked++] = CStringGetTextDatum("standby");
	if (index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		blocked[nblocked++] = CStringGetTextDatum("temporary table");
	if (!sift_col_autovacuum_enabled(heap))
		blocked[nblocked++] = CStringGetTextDatum("autovacuum_enabled is false");
	if (gates & SIFT_COL_GATE_WAL)
		blocked[nblocked++] = CStringGetTextDatum("the rewrite would take more than sift.col_auto_merge_wal_pct of the WAL written since the last");
	if (gates & SIFT_COL_GATE_HELD)
		blocked[nblocked++] = CStringGetTextDatum("a snapshot holds back the blocks of the last rewrite");
	v[0] = ObjectIdGetDatum(RelationGetRelid(heap));
	nulls[1] = nulls[2] = nulls[3] = !sift_col_table_churn(RelationGetRelid(heap), &changed, &threshold, &last);
	nulls[1] |= last == 0;
	v[1] = TimestampTzGetDatum(last);
	v[2] = Int64GetDatum(changed);
	v[3] = Int64GetDatum(threshold);
	nulls[4] = why == NULL;
	if (why != NULL)
		v[4] = CStringGetTextDatum(why);
	nulls[5] = snap->meta.maint_requested_at == 0;
	v[5] = TimestampTzGetDatum(snap->meta.maint_requested_at);
	v[6] = PointerGetDatum(construct_array(blocked, nblocked, TEXTOID, -1, false, TYPALIGN_INT));
	relation_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/* sift.col_maintain(index): the rewrite of sift.col_auto_merge, now and in this session, whatever that setting says; false when nothing is due or the maintenance lock is held */
Datum
sift_col_maintain(PG_FUNCTION_ARGS)
{
	Relation	index;
	int			n;

	PreventCommandIfReadOnly("sift.col_maintain()");
	PreventCommandDuringRecovery("sift.col_maintain()");
	index = sift_mops_open_maintain(&maint_col_ops, PG_GETARG_OID(0));
	n = sift_mops_rewrite(&maint_col_ops, index, false);
	index_close(index, RowExclusiveLock);
	PG_RETURN_BOOL(n >= 0);
}

Datum
sift_col_promote(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&maint_col_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_promote(&maint_col_ops, index);

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_col_merge(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&maint_col_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_merge(&maint_col_ops, index, PG_NARGS() > 1 && PG_GETARG_BOOL(1));

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_col_index_info(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open(&maint_col_ops, PG_GETARG_OID(0), AccessShareLock);
	const SiftColIndexMeta *defs = sift_col_index_defs(index);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftFreeStats fs;
	Datum		v[16];
	bool		nulls[16] = {0};
	int64		rows = 0,
				bytes = 0,
				units = 0,
				pagg_bytes = 0,
				pagg_units = 0;
	int			nwbuf = 0;
	SiftDirSnapshot *plain = palloc(sizeof(SiftDirSnapshot));

	sift_dir_read(index, snap);
	fs = sift_store_free_stats(index);
	plain->nsegs = 0;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);

			rows += snap->segs[i].ndocs;
			bytes += snap->segs[i].bytes;
			pagg_bytes += sift_cseg_preagg_bytes(seg);
			pagg_units += sift_cseg_preagg_units(seg);
			/* the units of a key-cut segment are numbered from 0 in each segment: they are stored units, not superblocks that other segments share */
			if (sift_cseg_keyed(&snap->segs[i]))
				units += seg->nsb;
			else
				plain->segs[plain->nsegs++] = snap->segs[i];
			sift_cseg_close(seg);
		}
		else
			nwbuf++;
	units += sift_cseg_dir_units(plain);
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(index));
	v[1] = Int64GetDatum(snap->meta.high_water);
	v[2] = Int64GetDatum(fs.blocks);
	v[3] = Int64GetDatum(fs.reusable);
	v[4] = Int64GetDatum(fs.records);
	v[5] = Int32GetDatum(snap->nsegs);
	v[6] = Int32GetDatum(SIFT_MAX_SEGMENTS);
	v[7] = Int64GetDatum(rows);
	v[8] = Int64GetDatum(bytes);
	v[9] = Int64GetDatum(units);
	v[10] = Int32GetDatum(nwbuf);
	v[11] = Int32GetDatum(defs->ncols);
	nulls[12] = RecoveryInProgress();
	v[12] = BoolGetDatum(!nulls[12] && sift_maintenance_busy(index));
	v[13] = CStringGetTextDatum(sift_simd_level_name(sift_cpu.active));
	v[14] = Int64GetDatum(pagg_bytes);
	v[15] = Int64GetDatum(pagg_units);
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

Datum
sift_col_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_mops_open(&maint_col_ops, PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		Datum		v[12];
		bool		nulls[12] = {0};

		v[0] = Int64GetDatum(e->gen);
		v[1] = CStringGetTextDatum(e->kind == SIFT_SEG_IMMUTABLE ? "immutable" :
								   e->kind == SIFT_SEG_WBUF_OPEN ? "buffer" : "sealed buffer");
		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, e, CurrentMemoryContext);

			v[2] = Int64GetDatum(e->ndocs);
			v[3] = Int64GetDatum(e->bytes);
			v[4] = Int32GetDatum(e->sb_lo);
			v[5] = Int32GetDatum(e->sb_hi);
			v[6] = Int32GetDatum(seg->nsb);
			v[7] = BoolGetDatum((e->flags & SIFT_SEG_DIRTY) != 0);
			v[8] = Int64GetDatum(e->ndocs - sift_cseg_live_rows(seg));
			v[9] = Int32GetDatum(seg->hdr->nextents);
			v[10] = Int32GetDatum(e->format);
			v[11] = Int64GetDatum(sift_cseg_preagg_bytes(seg));
			sift_cseg_close(seg);
		}
		else
		{
			uint64		nrows,
						npages;

			sift_cwbuf_summary(index, e, &nrows, &npages);
			v[2] = Int64GetDatum(nrows);
			v[3] = Int64GetDatum(npages * BLCKSZ);
			nulls[4] = nulls[5] = nulls[6] = nulls[7] = nulls[8] = nulls[9] = nulls[10] = nulls[11] = true;
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* One row per leaf: the chunks of all immutable segments by encoding, their bytes, and the dictionaries */
Datum
sift_col_leaves(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_mops_open(&maint_col_ops, PG_GETARG_OID(0), AccessShareLock);
	const SiftColSchema *sc = sift_col_rel(index)->schema;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64	   *chunks = palloc0(sc->ncols * 5 * sizeof(int64)),
			   *bytes = palloc0(sc->ncols * sizeof(int64)),
			   *dict_n = palloc0(sc->ncols * sizeof(int64)),
			   *dict_bytes = palloc0(sc->ncols * sizeof(int64));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftCUnitView v;
			uint32		len;
			const uint8 *rec = sift_cseg_unit(seg, u, &len);

			if (!sift_cunit_parse(rec, len, sc, &v))
				corrupt(index, "Unit %u of segment %llu does not parse.", u, (unsigned long long) snap->segs[i].gen);
			for (int c = 0; c < sc->ncols; c++)
			{
				SiftCChunk	ch;

				if (!sift_cunit_chunk(&v, c, &ch))
					continue;
				chunks[c * 5 + (ch.enc == SIFT_COLENC_ISTR ? 4 : Min(ch.enc, 3))]++;
				bytes[c] += v.chunk_len[c];
			}
		}
		for (int c = 0; c < sc->ncols; c++)
		{
			SiftCDict  *d = sift_cseg_dict(seg, c);

			if (d)
			{
				dict_n[c] += d->n;
				dict_bytes[c] += d->nbytes + (int64) (d->n + 1) * sizeof(uint32);
			}
		}
		sift_cseg_close(seg);
	}
	for (int c = 0; c < sc->ncols; c++)
	{
		Datum		v[11];
		bool		nulls[11] = {false};

		v[0] = Int32GetDatum(c);
		v[1] = CStringGetTextDatum(sc->cols[c].path[0] ? sc->cols[c].path : NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[c].attno - 1)->attname));
		v[2] = CStringGetTextDatum(sc->cols[c].kind == SIFT_COLK_GROUP ? "group" : "scalar");
		v[3] = CStringGetTextDatum(sc->cols[c].lane > 0 ? psprintf("%s (lane %d)", format_type_be(sift_col_def_typid(RelationGetDescr(index), &sc->cols[c])), sc->cols[c].lane) :
								   format_type_be(sift_col_def_typid(RelationGetDescr(index), &sc->cols[c])));
		for (int e = 0; e < 5; e++)
			v[4 + e] = Int64GetDatum(chunks[c * 5 + e]);
		v[9] = Int64GetDatum(bytes[c]);
		v[10] = Int64GetDatum(dict_bytes[c]);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
