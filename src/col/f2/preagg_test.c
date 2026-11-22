/*
 * col/f2test/preagg_test.c - test-only SQL functions sift._test_f2_*: the pre-aggregates of an index, the compaction, the time strides, the digest of a part, the order of the rows and the plan (col/format2_test.c has the shared helpers)
 */
#include "postgres.h"

#include "core/horizon.h"

#ifdef SIFT_TESTING

#include "col/f2/f2test.h"

PG_FUNCTION_INFO_V1(sift_test_f2_preagg_index);
PG_FUNCTION_INFO_V1(sift_test_f2_preagg_excl);
PG_FUNCTION_INFO_V1(sift_test_f2_candidates);
PG_FUNCTION_INFO_V1(sift_test_f2_compact);
PG_FUNCTION_INFO_V1(sift_test_f2_compact_racing);
PG_FUNCTION_INFO_V1(sift_test_f2_plan);
PG_FUNCTION_INFO_V1(sift_test_f2_tstride);
PG_FUNCTION_INFO_V1(sift_test_f2_part_bytes);
PG_FUNCTION_INFO_V1(sift_test_f2_part_digest);
PG_FUNCTION_INFO_V1(sift_test_f2_order);
PG_FUNCTION_INFO_V1(sift_test_f2_qset);

typedef struct PreaggTally
{
	int			granules;
	int			with_dead;
	int			corrected;
	int			rescanned;
	int			mismatched;
	int64		dead;
} PreaggTally;

/* The cube of a granule less the rows its deletion vector marks against the cube of the rows it leaves, one cell at a time */
static void
preagg_granule(Relation rel, const SiftPart *p, uint32 g, const SiftDv *dv, int dim, int measure, PreaggTally *t)
{
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, p, g, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
	const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
	int			nr = gd.nrows,
				ncells;
	SiftPreaggCell *cells = sift_preagg_build(&v, dim, measure, &ncells);
	int64	   *d = palloc(nr * sizeof(int64)),
			   *m = palloc(nr * sizeof(int64));
	bool	   *dn = palloc(nr),
			   *mn = palloc(nr);

	t->granules++;
	if (dg == NULL || dg->ndead == 0)
		return;
	t->with_dead++;
	t->dead += dg->ndead;
	if (!sift_preagg_delta_ok(nr, dg->ndead) || !sift_preagg_subtract(&v, dim, measure, dg, cells, &ncells))
	{
		t->rescanned++;
		return;
	}
	t->corrected++;
	sift_granule_ints(&v, dim, d, dn);
	sift_granule_ints(&v, measure, m, mn);
	for (int c = 0; c < ncells; c++)
	{
		uint64		count = 0;
		int64		sum = 0,
					lo = PG_INT64_MAX,
					hi = PG_INT64_MIN;
		double		sumsq = 0;

		for (int r = 0; r < nr; r++)
		{
			bool		dead;

			(void) sift_dv_find(dg, r, &dead);
			if (dead || dn[r] || mn[r] || d[r] != cells[c].key)
				continue;
			count++;
			sum += m[r];
			lo = Min(lo, m[r]);
			hi = Max(hi, m[r]);
			sumsq += (double) m[r] * (double) m[r];
		}
		t->mismatched += cells[c].count != count || cells[c].sum != sum || cells[c].min != lo || cells[c].max != hi || fabs(cells[c].sumsq - sumsq) > 1e-6 * Max(1.0, sumsq);
	}
}

/* (index, dimension column, measure column): the cube of every granule of every part less its deleted rows, against the cube of the rows that are left, and how many granules it could correct */
Datum
sift_test_f2_preagg_index(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	PreaggTally t = {0};
	int			nparts = 0;
	Datum		v[7];
	bool		n[7] = {0};

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *p = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);
			SiftDv	   *dv = pd[k].dv_blk != InvalidBlockNumber ? sift_dv_read(rel, &pd[k], (uint32) mr.entries[i].part_id, f2t_dir_rows(rel, p)) : NULL;

			for (uint32 g = 0; g < p->hdr.ngranules; g++)
				preagg_granule(rel, p, g, dv, PG_GETARG_INT32(1), PG_GETARG_INT32(2), &t);
			nparts++;
		}
	}
	v[0] = Int32GetDatum(nparts);
	v[1] = Int32GetDatum(t.granules);
	v[2] = Int32GetDatum(t.with_dead);
	v[3] = Int32GetDatum(t.corrected);
	v[4] = Int32GetDatum(t.rescanned);
	v[5] = Int32GetDatum(t.mismatched);
	v[6] = Int64GetDatum(t.dead);
	f2t_srf_put(rsi, v, n);
	index_close(rel, NoLock);
	return (Datum) 0;
}

typedef struct ExclTally
{
	int			granules;
	int			with_excl;
	int			corrected;
	int			rescanned;
	int			mismatched;
	int			naive_mismatched;
	int64		dead;
	int64		not_null;
	int64		overlap;
} ExclTally;

/* Whether the cells are the cube of the rows that are not excluded: each cell against its rows, and a cell for every key the rows have */
static bool
cube_matches(const SiftGranuleView *v, int dim, int measure, const bool *excluded, const SiftPreaggCell *cells, int ncells)
{
	int			nr = v->hdr->nrows;
	int64	   *d = palloc(nr * sizeof(int64)),
			   *m = palloc(nr * sizeof(int64));
	bool	   *dn = palloc(nr),
			   *mn = palloc(nr);
	bool		same = true;

	sift_granule_ints(v, dim, d, dn);
	sift_granule_ints(v, measure, m, mn);
	for (int c = 0; c < ncells && same; c++)
	{
		uint64		count = 0;
		int64		sum = 0,
					lo = PG_INT64_MAX,
					hi = PG_INT64_MIN;
		double		sumsq = 0;

		for (int r = 0; r < nr; r++)
		{
			if (excluded[r] || dn[r] || mn[r] || d[r] != cells[c].key)
				continue;
			count++;
			sum += m[r];
			lo = Min(lo, m[r]);
			hi = Max(hi, m[r]);
			sumsq += (double) m[r] * (double) m[r];
		}
		same = cells[c].count == count && cells[c].sum == sum && cells[c].min == lo && cells[c].max == hi && fabs(cells[c].sumsq - sumsq) <= 1e-6 * Max(1.0, sumsq);
	}
	for (int r = 0; r < nr && same; r++)
	{
		bool		found = excluded[r] || dn[r];

		for (int c = 0; c < ncells && !found; c++)
			found = cells[c].key == d[r];
		same = found;
	}
	return same;
}

/*
 * The cube of a granule less the rows that WHERE nullcol IS NULL leaves out of the answer (the rows nullcol has a value on) and the rows its deletion vector marks, as the one list of their union
 * (a row in both is taken out once), against the cube of the rows that are left; and the same taking the two lists out one after the other, which counts a row in both twice
 */
static void
excl_granule(Relation rel, const SiftPart *p, const SiftGranuleDesc *dir, uint32 g, const SiftDv *dv, int dim, int measure, int nullcol, ExclTally *t)
{
	const uint16 cap = SIFT_PREAGG_SPARSE_PRED_MAX;
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, p, g, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
	const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
	int			nr = gd.nrows,
				nd = dg ? dg->ndead : 0,
				ncells,
				ncells2;
	uint16	   *all = palloc(nr * sizeof(uint16)),
			   *un = palloc((nr + 1) * sizeof(uint16));
	uint16		nall = sift_granule_nonnull_rows(&v, nullcol, all, nr),
				nlim = sift_part_read_nonnull_rows(rel, p, &dir[g], nullcol, un, cap),
				nun;
	bool	   *excluded = palloc0(nr);
	SiftPreaggCell *cells = sift_preagg_build(&v, dim, measure, &ncells),
			   *cells2 = palloc(Max(ncells, 1) * sizeof(SiftPreaggCell));

	t->granules++;
	t->mismatched += nlim != Min(nall, cap + 1);
	if (nd == 0 && nall == 0)
		return;
	t->with_excl++;
	t->dead += nd;
	t->not_null += nall;
	for (int i = 0; i < nd; i++)
		excluded[dg->rows[i]] = true;
	for (int i = 0; i < nall; i++)
	{
		t->overlap += excluded[all[i]];
		excluded[all[i]] = true;
	}
	memcpy(cells2, cells, ncells * sizeof(SiftPreaggCell));
	ncells2 = ncells;
	t->naive_mismatched += !((nd == 0 || sift_preagg_subtract_rows(&v, dim, measure, dg->rows, nd, cells2, &ncells2)) &&
							 sift_preagg_subtract_rows(&v, dim, measure, all, nall, cells2, &ncells2) &&
							 cube_matches(&v, dim, measure, excluded, cells2, ncells2));
	nun = sift_excl_union_u16(dg ? dg->rows : NULL, nd, all, nall, un, cap);
	if (nun > cap || !sift_preagg_excl_ok(nr, nun, nd, 1) || !sift_preagg_subtract_rows(&v, dim, measure, un, nun, cells, &ncells))
	{
		t->rescanned++;
		return;
	}
	t->corrected++;
	t->mismatched += !cube_matches(&v, dim, measure, excluded, cells, ncells);
}

/* (index, dimension column, measure column, column the predicate IS NULL is on): per granule of every part, the cube less the union of its dead rows and the rows the column has a value on against the cube of the rows that are left */
Datum
sift_test_f2_preagg_excl(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	ExclTally t = {0};
	Datum		v[9];
	bool		n[9] = {0};

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *p = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);
			SiftGranuleDesc *dir = sift_part_granule_dir(rel, p);
			SiftDv	   *dv = pd[k].dv_blk != InvalidBlockNumber ? sift_dv_read(rel, &pd[k], (uint32) mr.entries[i].part_id, f2t_dir_rows(rel, p)) : NULL;

			for (uint32 g = 0; g < p->hdr.ngranules; g++)
				excl_granule(rel, p, dir, g, dv, PG_GETARG_INT32(1), PG_GETARG_INT32(2), PG_GETARG_INT32(3), &t);
		}
	}
	v[0] = Int32GetDatum(t.granules);
	v[1] = Int32GetDatum(t.with_excl);
	v[2] = Int32GetDatum(t.corrected);
	v[3] = Int32GetDatum(t.rescanned);
	v[4] = Int32GetDatum(t.mismatched);
	v[5] = Int32GetDatum(t.naive_mismatched);
	v[6] = Int64GetDatum(t.dead);
	v[7] = Int64GetDatum(t.not_null);
	v[8] = Int64GetDatum(t.overlap);
	f2t_srf_put(rsi, v, n);
	index_close(rel, NoLock);
	return (Datum) 0;
}

Datum
sift_test_f2_candidates(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n,
				nc;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	uint64	   *c = sift_compact_candidates(parts, n, &nc);
	Datum	   *elems = palloc(Max(nc, 1) * sizeof(Datum));

	for (int i = 0; i < nc; i++)
		elems[i] = Int64GetDatum(c[i]);
	index_close(rel, NoLock);
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(elems, nc, INT8OID));
}

/* (index, part_id, partition lower key, generations, preagg, dictionary remap) */
Datum
sift_test_f2_compact(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES];
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(3);
	int			ng = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
	uint64	   *gens = palloc(Max(ng, 1) * sizeof(uint64));
	SiftPartSpec spec = (sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR) ? sift_f2_rel(rel)->spec : f2t_spec(PG_GETARG_BOOL(4));
	SiftCompactResult res;
	bool		done;
	Datum		v[13];
	bool		nl[13] = {0};
	int64	   *src = (int64 *) ARR_DATA_PTR(a);

	for (int i = 0; i < ng; i++)
		gens[i] = src[i];
	sift_compact_dict_remap = PG_NARGS() < 6 || PG_GETARG_BOOL(5);
	PG_TRY();
	{
		done = sift_partition_compact_online(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, lo), gens, ng, &spec, (sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR) ? sift_f2_hot_cut(sift_f2_rel(rel)) : PG_INT64_MIN, sift_horizon(rel), &res);
	}
	PG_FINALLY();
	{
		sift_compact_dict_remap = true;
	}
	PG_END_TRY();
	v[0] = BoolGetDatum(done);
	v[1] = Int64GetDatum(res.new_gen);
	v[2] = Int64GetDatum(res.rows_in);
	v[3] = Int64GetDatum(res.rows_out);
	v[4] = Int64GetDatum(res.dead_dropped);
	v[5] = Int64GetDatum(res.aborted_dropped);
	v[6] = Int64GetDatum(res.frozen);
	v[7] = Int64GetDatum(res.granules_copied);
	v[8] = Int64GetDatum(res.granules_written);
	v[9] = Int64GetDatum(res.bytes_out);
	v[10] = Int64GetDatum(res.dict_cols_remapped);
	v[11] = Int64GetDatum(res.dict_codes_remapped);
	v[12] = Int64GetDatum(res.writer_bytes);
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

/* (index, part_id, partition lower key, generations, rows a deleter takes from the first part between the steps): whether the compaction was done */
Datum
sift_test_f2_compact_racing(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES];
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(3);
	int			ng = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
	uint64	   *gens = palloc(Max(ng, 1) * sizeof(uint64));
	SiftCompactResult res;
	int64	   *src = (int64 *) ARR_DATA_PTR(a);
	bool		done;

	for (int i = 0; i < ng; i++)
		gens[i] = src[i];
	sift_test_compact_delete = PG_GETARG_INT32(4);
	done = sift_partition_compact_online(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, lo), gens, ng, &sift_f2_rel(rel)->spec, sift_f2_hot_cut(sift_f2_rel(rel)), sift_store_horizon(rel), &res);
	sift_test_compact_delete = 0;
	index_close(rel, NoLock);
	PG_RETURN_BOOL(done);
}

/* A part of n rows with a time index (the key is the time), written and read back stride by stride; its blocks are released at the end */
Datum
sift_test_f2_tstride(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	int			n = PG_GETARG_INT32(1);
	SiftPartSpec spec = f2t_spec(false);
	SiftRowBuf *rb;
	SiftColIn	cols[F2_NCOLS];
	SiftRows	r;
	SiftPartDesc d;
	SiftPart   *part;
	SiftExtent *blocks;
	int			nb,
				good = 0,
				strides = 0;
	uint32		gen;

	spec.time_col = 1;
	rb = sift_rowbuf_new(&spec);
	for (int i = 0; i < n; i++)
	{
		SiftCell	c[F2_NCOLS] = {{0}};
		uint8		key[SIFT_BOUND_BYTES];

		c[0].i = i;
		c[1] = (SiftCell) {.s = "",.slen = 0};
		f2t_key(i, key);
		sift_rowbuf_add(rb, f2t_tid(0, i), FrozenTransactionId, 0, 0, key, c, NULL);
	}
	sift_rowbuf_sort(rb, &spec);
	r = sift_rowbuf_rows(rb, cols, &spec);
	gen = (uint32) sift_manifest_new_gen(rel, 1);
	d = sift_part_write_rows(rel, 1, gen, 1, SIFT_TIER_WARM_L1, &r);
	part = sift_part_open(rel, d.header_blk, gen, 1);
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		int			cnt = Min(n - (int) g * SIFT_GRANULE_ROWS, SIFT_GRANULE_ROWS),
					m;
		SiftTimeStride *st = sift_part_read_tstride(rel, part, g, &m);
		bool		ok = m == (cnt + SIFT_TSTRIDE_ROWS - 1) / SIFT_TSTRIDE_ROWS;

		for (int k = 0; ok && k < m; k++)
		{
			int64		first = (int64) g * SIFT_GRANULE_ROWS + (int64) k * SIFT_TSTRIDE_ROWS;

			ok = st[k].time_min == first && st[k].time_max == Min(first + SIFT_TSTRIDE_ROWS, (int64) g * SIFT_GRANULE_ROWS + cnt) - 1;
		}
		good += ok;
		strides += m;
	}
	blocks = sift_part_blocks(part, &nb);
	sift_fsm_release(rel, blocks, nb, InvalidFullTransactionId);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("granules=%u verified=%d strides=%d", part->hdr.ngranules, good, strides)));
}

/* The bytes of the parts of a partition, by level: element l + 1 of an array of 8 is the level l */
Datum
sift_test_f2_part_bytes(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	Datum		by_level[8] = {0};

	for (int i = 0; i < n; i++)
		by_level[Min(parts[i].level, 7)] = Int64GetDatum(DatumGetInt64(by_level[Min(parts[i].level, 7)]) + (int64) parts[i].nbytes);
	index_close(rel, NoLock);
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(by_level, 8, INT8OID));
}

/* The rows of every part of a partition as bytes: the checksum of the dictionaries and the granule records of each part, in the order of the parts, as "granules:crc" for each */
Datum
sift_test_f2_part_digest(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	StringInfoData out;

	initStringInfo(&out);
	for (int i = 0; i < n; i++)
	{
		SiftPart   *part = sift_part_open(rel, parts[i].header_blk, parts[i].part_gen, (uint32) PG_GETARG_INT64(1));
		StringInfoData bytes;
		uint64		len;
		void	   *dicts = sift_part_read_section(rel, part, SEC_DICTS, &len);

		initStringInfo(&bytes);
		if (dicts != NULL)
			appendBinaryStringInfo(&bytes, dicts, len);
		for (uint32 g = 0; g < part->hdr.ngranules; g++)
		{
			SiftGranuleDesc gd;
			char	   *rec = sift_part_read_granule(rel, part, g, &gd);

			appendBinaryStringInfo(&bytes, rec, gd.data_len);
			pfree(rec);
		}
		appendStringInfo(&out, "%s%u:%08x", i > 0 ? "," : "", part->hdr.ngranules, sift_crc32c(bytes.data, bytes.len));
		pfree(bytes.data);
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/*
 * (index, part_id, partition lower key): what the lead key column looks like in the granules of the parts of a partition, read back with the decoders and not through the keys: the
 * rows in the order they are stored, how many steps of the integers go up and down, how many of the parts have their NULLs first, last, or broken by an integer
 */
Datum
sift_test_f2_order(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftF2Rel  *f = sift_f2_rel(rel);
	SiftPartitionEntry entry;
	int			n,
				first = 0,
				last = 0,
				broken = 0;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	int64		rows = 0,
				nulls = 0,
				ups = 0,
				downs = 0;

	for (int i = 0; i < n; i++)
	{
		SiftPart   *p = sift_part_open(rel, parts[i].header_blk, parts[i].part_gen, (uint32) entry.part_id);
		bool		started = false,
					prev_null = false,
					have_val = false,
					null_run_done = false,
					part_first = false,
					part_broken = false;
		int64		prev = 0;

		for (uint32 g = 0; g < p->hdr.ngranules; g++)
		{
			SiftGranuleDesc gd;
			char	   *rec = sift_part_read_granule(rel, p, g, &gd);
			SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
			/* check-memory: allow a test function: the rows of one granule */
			int64	   *val = palloc(gd.nrows * sizeof(int64));
			bool	   *isnull = palloc(gd.nrows);

			sift_granule_ints(&v, f->key_cols[0], val, isnull);
			for (int r = 0; r < gd.nrows; r++)
			{
				rows++;
				nulls += isnull[r];
				part_first |= !started && isnull[r];
				if (started && prev_null != isnull[r])
				{
					part_broken |= null_run_done;
					null_run_done = true;
				}
				if (!isnull[r])
				{
					ups += have_val && val[r] > prev;
					downs += have_val && val[r] < prev;
					prev = val[r];
					have_val = true;
				}
				prev_null = isnull[r];
				started = true;
			}
			pfree(rec);
		}
		first += part_first;
		last += !part_first && null_run_done;
		broken += part_broken;
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("rows=%lld nulls=%lld ups=%lld downs=%lld parts_nulls_first=%d parts_nulls_last=%d parts_nulls_broken=%d", (long long) rows, (long long) nulls,
											  (long long) ups, (long long) downs, first, last, broken)));
}

/* What a vacuum would compact: the groups, as 'generations / generations' */
Datum
sift_test_f2_plan(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n,
				ng;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftCompactGroup *groups = sift_compact_plan(parts, n, (uint64) maintenance_work_mem * 1024 / 4, (sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR) ? sift_f2_hot_cut(sift_f2_rel(rel)) : PG_INT64_MIN, &ng);
	StringInfoData out;

	initStringInfo(&out);
	for (int k = 0; k < ng; k++)
	{
		appendStringInfoString(&out, k > 0 ? " / " : "");
		for (int i = 0; i < groups[k].n; i++)
			appendStringInfo(&out, i > 0 ? ",%llu" : "%llu", (unsigned long long) groups[k].gens[i]);
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/* op: add, remove, has, count, generation */
Datum
sift_test_f2_qset(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	char	   *op = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int64		r = 0;

	if (strcmp(op, "add") == 0)
		sift_qset_add(&rel->rd_locator);
	else if (strcmp(op, "remove") == 0)
		sift_qset_remove(&rel->rd_locator);
	else if (strcmp(op, "has") == 0)
		r = sift_qset_has(&rel->rd_locator);
	else if (strcmp(op, "count") == 0)
		r = sift_qset_count();
	else
		r = sift_qset_generation();
	index_close(rel, NoLock);
	PG_RETURN_INT64(r);
}

#endif							/* SIFT_TESTING */
