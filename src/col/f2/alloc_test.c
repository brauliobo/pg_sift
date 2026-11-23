/*
 * col/f2test/alloc_test.c - test-only SQL functions sift._test_f2_*: the allocator and the free space map, the dump of a part, the L0 stage and slab, the deletion vectors, the visibility rules and the pre-aggregates of a granule (col/format2_test.c has the shared helpers)
 */
#include "postgres.h"

#include "utils/snapmgr.h"

#ifdef SIFT_TESTING

#include "col/f2/f2test.h"

PG_FUNCTION_INFO_V1(sift_test_f2_alloc);
PG_FUNCTION_INFO_V1(sift_test_f2_pack_tails);
PG_FUNCTION_INFO_V1(sift_test_f2_state_flags);
PG_FUNCTION_INFO_V1(sift_test_f2_pack_info);
PG_FUNCTION_INFO_V1(sift_test_f2_alloc_stream);
PG_FUNCTION_INFO_V1(sift_test_f2_alloc_below);
PG_FUNCTION_INFO_V1(sift_test_f2_release);
PG_FUNCTION_INFO_V1(sift_test_f2_reclaim);
PG_FUNCTION_INFO_V1(sift_test_f2_extend_file);
PG_FUNCTION_INFO_V1(sift_test_f2_tail_shrink);
PG_FUNCTION_INFO_V1(sift_test_f2_meta_inline);
PG_FUNCTION_INFO_V1(sift_test_f2_fsm);
PG_FUNCTION_INFO_V1(sift_test_f2_meta_alloc);
PG_FUNCTION_INFO_V1(sift_test_f2_dump);
PG_FUNCTION_INFO_V1(sift_test_f2_encs);
PG_FUNCTION_INFO_V1(sift_test_f2_leaf_zones);
PG_FUNCTION_INFO_V1(sift_test_f2_stage);
PG_FUNCTION_INFO_V1(sift_test_f2_stage_flush);
PG_FUNCTION_INFO_V1(sift_test_f2_stage_rows);
PG_FUNCTION_INFO_V1(sift_test_f2_slab);
PG_FUNCTION_INFO_V1(sift_test_f2_slab_flush);
PG_FUNCTION_INFO_V1(sift_test_f2_delete);
PG_FUNCTION_INFO_V1(sift_test_f2_pending);
PG_FUNCTION_INFO_V1(sift_test_f2_dv);
PG_FUNCTION_INFO_V1(sift_test_f2_visible);

/* The spec the rows of the relation are made with: a heap-reference slab holds the index's own, whatever a test makes up */
static SiftPartSpec
slab_spec(Relation rel)
{
	return sift_col_is_index(rel) && sift_col_format_major(rel) == SIFT_FORMAT2_MAJOR ? sift_f2_rel(rel)->spec : f2t_spec(false);
}

Datum
sift_test_f2_alloc(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftExtent	e = sift_fsm_alloc(rel, PG_GETARG_INT32(1));
	Datum		v[2] = {Int64GetDatum(e.start), Int32GetDatum(e.nblocks)};
	bool		n[2] = {0};

	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, n);
}

/* (index, nblocks): the extents of a stream of exactly nblocks blocks */
Datum
sift_test_f2_alloc_below(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftExtent	ext[SIFT_FSM_NCLASSES + 1];
	int			n = sift_fsm_alloc_below(rel, PG_GETARG_INT64(1), PG_GETARG_INT64(2), ext, SIFT_FSM_NCLASSES + 1);

	for (int i = 0; i < n; i++)
	{
		Datum		v[2] = {Int64GetDatum(ext[i].start), Int32GetDatum(ext[i].nblocks)};
		bool		nl[2] = {0};

		f2t_srf_put(rsi, v, nl);
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

Datum
sift_test_f2_alloc_stream(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	uint64		nblocks = PG_GETARG_INT64(1);
	/* check-memory: allow a test function: the caller names the blocks it allocates */
	SiftExtent *ext = palloc((nblocks + 1) * sizeof(SiftExtent));
	int			n = sift_fsm_alloc_stream(rel, nblocks, ext, (int) nblocks + 1);

	for (int i = 0; i < n; i++)
	{
		Datum		v[2] = {Int64GetDatum(ext[i].start), Int32GetDatum(ext[i].nblocks)};
		bool		nl[2] = {0};

		f2t_srf_put(rsi, v, nl);
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

Datum
sift_test_f2_meta_alloc(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	BlockNumber blk = sift_fsm_alloc_meta_page(rel);

	index_close(rel, NoLock);
	PG_RETURN_INT64(blk);
}

/* safe: 'old' (reusable at once), 'now' (the xid of this transaction: reusable once it ends), 'future' (never) */
Datum
sift_test_f2_release(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftExtent	e = {PG_GETARG_INT64(1), PG_GETARG_INT32(2)};
	char	   *safe = text_to_cstring(PG_GETARG_TEXT_PP(3));
	FullTransactionId x = InvalidFullTransactionId;

	if (strcmp(safe, "now") == 0)
		x = GetTopFullTransactionId();
	else if (strcmp(safe, "future") == 0)
		x = FullTransactionIdFromU64(U64FromFullTransactionId(ReadNextFullTransactionId()) + 1000000);
	sift_fsm_release(rel, &e, 1, x);
	index_close(rel, NoLock);
	PG_RETURN_VOID();
}

Datum
sift_test_f2_reclaim(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint64		n = sift_fsm_reclaim(rel);

	index_close(rel, NoLock);
	PG_RETURN_INT64(n);
}

/* (index, blocks): the file zero-filled up to that many blocks, as the writes of the parts that sit at its end do; returns its length */
Datum
sift_test_f2_extend_file(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	Buffer		buf = sift_extend_zeroed_to(rel, (BlockNumber) PG_GETARG_INT64(1));

	UnlockReleaseBuffer(buf);
	index_close(rel, NoLock);
	PG_RETURN_INT64(PG_GETARG_INT64(1));
}

/* (index): the free run at the end of the file given back: the blocks cut, whether the index was busy, the high-water mark and the length of the file after it */
Datum
sift_test_f2_tail_shrink(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftTailShrinkResult r = sift_fsm_tail_shrink(rel);
	SiftColMetaPageData m = sift_col_meta_read(rel);
	Datum		v[4] = {Int64GetDatum(r.blocks_truncated), BoolGetDatum(r.lock_skipped), Int64GetDatum(m.high_water), Int64GetDatum(RelationGetNumberOfBlocks(rel))};
	bool		n[4] = {0};

	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, n);
}

/* (index): what block 0 keeps inline: its offset and length, the partitions of the summary and whether the index says so; the check of the manifest fails when the summary is not the directory */
Datum
sift_test_f2_meta_inline(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry *inl;
	int			n;
	SiftColMetaPageData m = sift_col_meta_read_inline(rel, &inl, &n);
	Datum		v[4] = {Int32GetDatum(m.inline_off), Int32GetDatum(m.inline_len), Int32GetDatum(n), BoolGetDatum((m.common.ro_compat_flags & SIFT_ROCOMPAT_META_INLINE) != 0)};
	bool		nl[4] = {0};

	(void) sift_manifest_check(rel);
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

Datum
sift_test_f2_fsm(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftFsmStats st = sift_fsm_check(rel);
	StringInfoData s;

	initStringInfo(&s);
	for (int c = 0; c < SIFT_FSM_NCLASSES; c++)
		appendStringInfo(&s, "c%d=%u ", c, st.class_extents[c]);
	appendStringInfo(&s, "queue=%u/%llu meta_free=%llu/%llu", st.quarantine_extents, (unsigned long long) st.quarantine_blocks, (unsigned long long) st.meta_free,
					 (unsigned long long) st.meta_total);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(s.data));
}

static const char *
enc_name(uint16 enc)
{
	static const char *names[] = {"?", "TID_DELTA", "INT_FOR", "INT_DELTA", "INT_CONST", "DICT_CODE", "STR_INLINE", "FLOAT_RAW", "LIST_LEN", "ALL_NULL", "BOOL_BITS", "NUM64", "NUM128",
	"UUID16", "INTERVAL16", "RLE", "ALP", "SCALE", "STR_VIEW", "TID_BLOCK", "FLOAT_BSS", "STR_FSST", "STR_BLOCK", "FIXED16", "KEY_PRESENT", "ALP_RD", "STR_PROMO"};

	enc &= SIFT_ENC_MASK;
	return enc < lengthof(names) ? names[enc] : "?";
}

/* (index, part_id, partition lower key): the encodings the granules of the parts of a partition hold column c in, as "c:ENC*granules" for each column and encoding (the _tid is column -1) */
Datum
sift_test_f2_encs(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	StringInfoData out;
	int			counts[64][32] = {{0}};
	int			nchunks = 0;

	for (int i = 0; i < n; i++)
	{
		SiftPart   *part = sift_part_open(rel, parts[i].header_blk, parts[i].part_gen, (uint32) entry.part_id);

		for (uint32 g = 0; g < part->hdr.ngranules; g++)
		{
			SiftGranuleDesc gd;
			char	   *rec = sift_part_read_granule(rel, part, g, &gd);
			SiftGranuleView v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);

			nchunks = Min(Max(nchunks, v.hdr->nchunks), 64);
			for (int c = 0; c < Min(v.hdr->nchunks, 64); c++)
				counts[c][v.hdr->chunks[c].enc & SIFT_ENC_MASK & 31]++;
			pfree(rec);
		}
	}
	initStringInfo(&out);
	for (int c = 0; c < nchunks; c++)
		for (int e = 0; e < 32; e++)
			if (counts[c][e] > 0)
				appendStringInfo(&out, "%s%d:%s*%d", out.len ? " " : "", c - 1, enc_name((uint16) e), counts[c][e]);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/* (index, part_id, partition lower key, leaf, op, literal): what the zones of the granules of the parts of a partition say of `leaf = literal` (op 0) or `leaf IN (literal)`: "none/maybe/granules" */
Datum
sift_test_f2_leaf_zones(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	int			leaf = PG_GETARG_INT32(3);
	text	   *lit = PG_GETARG_TEXT_PP(5);
	SiftColValue val = {.v.s = VARDATA_ANY(lit),.len = VARSIZE_ANY_EXHDR(lit)};
	SiftColPred pred = {.col = leaf,.op = PG_GETARG_INT32(4) == 0 ? SIFT_COLOP_EQ : SIFT_COLOP_IN,.nvals = 1,.vals = &val};
	const SiftColDef *d = &sift_col_rel(rel)->schema->cols[leaf];
	int			none = 0,
				maybe = 0,
				total = 0;
	uint32		flags = 0;

	for (int i = 0; i < n; i++)
	{
		SiftPart   *part = sift_part_open(rel, parts[i].header_blk, parts[i].part_gen, (uint32) entry.part_id);
		const SiftSecDirEntry *zs = sift_part_section(part, SEC_ZONES);

		for (uint32 g = 0; zs != NULL && g < part->hdr.ngranules; g++)
		{
			SiftZone	z;

			sift_part_read_section_bytes(rel, part, zs, ((uint64) leaf * part->hdr.ngranules + g) * sizeof(z), sizeof(z), &z);
			flags |= z.flags;
			if (sift_f2_zone_verdict(d, &pred, &z, SIFT_GRANULE_ROWS, false) == Z_NONE)
				none++;
			else
				maybe++;
			total++;
		}
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d/%d/%d/%x", none, maybe, total, flags)));
}

/* (index, part_id, partition lower key, generation, base_key, tid_base, mode): regenerates the rows of every granule and compares them */
Datum
sift_test_f2_dump(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftPartDesc *d = f2t_find_gen(parts, n, PG_GETARG_INT64(3));
	int64		base = PG_GETARG_INT64(4),
				tid_base = PG_GETARG_INT64(5);
	int			mode = PG_GETARG_INT32(6);
	SiftPart   *p = sift_part_open(rel, d->header_blk, d->part_gen, (uint32) entry.part_id);
	SiftGranuleDesc *dir = sift_part_granule_dir(rel, p);
	uint64		bad = 0,
				rows = 0;
	StringInfoData encs;

	initStringInfo(&encs);
	for (uint32 g = 0; g < p->hdr.ngranules; g++)
	{
		SiftGranuleDesc gd;
		char	   *rec = sift_part_read_granule(rel, p, g, &gd);
		SiftGranuleView v = sift_granule_open_part(rel, p, rec, gd.data_len, &gd);
		int			nr = gd.nrows;
		uint64	   *tids = sift_part_read_tids(rel, p, dir, g);
		uint64	   *tids2 = palloc(nr * sizeof(uint64));
		int64	   *k = palloc(nr * sizeof(int64)),
				   *m = palloc(nr * sizeof(int64)),
				   *dm = palloc(nr * sizeof(int64));
		bool	   *kn = palloc(nr),
				   *mn = palloc(nr),
				   *dn = palloc(nr);
		SiftStrCell *s = palloc(nr * sizeof(SiftStrCell));

		sift_granule_tids(&v, tids2);
		sift_granule_ints(&v, 0, k, kn);
		sift_granule_strs(&v, 1, s);
		sift_granule_ints(&v, 2, m, mn);
		sift_granule_ints(&v, 3, dm, dn);
		for (int r = 0; r < nr; r++)
		{
			int64		idx = k[r] - base,
						em,
						ed;
			bool		emn,
						edn,
						esn;
			char		es[32];

			f2t_gen_row(mode, k[r], &em, &emn, &ed, &edn, es, &esn);
			bad += (tids[r] != tids2[r]) || (tids[r] != f2t_tid(tid_base, (int) idx)) || kn[r] || idx < 0 || idx >= (int64) d->nrows ||
				(mn[r] != emn) || (!emn && m[r] != em) || (dm[r] != ed) || (s[r].isnull != esn) || (!esn && (s[r].len != strlen(es) || memcmp(s[r].p, es, s[r].len) != 0)) ||
				(g * SIFT_GRANULE_ROWS + r != (uint32) idx);
			rows++;
		}
		if (g == 0)
			for (int c = 0; c < v.hdr->nchunks; c++)
				appendStringInfo(&encs, "%s%s", c ? "," : "", enc_name(v.hdr->chunks[c].enc));
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("granules=%u rows=%llu bad=%llu enc0=%s", p->hdr.ngranules, (unsigned long long) rows, (unsigned long long) bad, encs.data)));
}

/* (index, nrows, base_key, tid_base, xmin, step): stages rows in decreasing key order, routed by key */
Datum
sift_test_f2_stage(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	SiftPartSpec spec = f2t_spec(false);
	SiftTxnStage *st = sift_stage_get(rel, &spec);
	int			n = PG_GETARG_INT32(1);
	int64		base = PG_GETARG_INT64(2),
				tid_base = PG_GETARG_INT64(3),
				step = PG_GETARG_INT64(5);
	uint32		xmin = f2t_xmin_of(PG_GETARG_INT64(4));

	st->by_value = true;
	for (int i = n - 1; i >= 0; i--)
	{
		int64		k = base + i * step;
		SiftCell	c[F2_NCOLS] = {{0}};
		char		s[32];
		uint8		key[SIFT_BOUND_BYTES];
		bool		mn,
					dn,
					sn;

		f2t_gen_row(0, k, &c[2].i, &mn, &c[3].i, &dn, s, &sn);
		c[0].i = k;
		c[1] = (SiftCell) {.s = s,.slen = strlen(s)};
		f2t_key(k, key);
		sift_stage_add(rel, st, key, f2t_tid(tid_base, i), xmin, 0, c, NULL);
	}
	index_close(rel, NoLock);
	PG_RETURN_VOID();
}

Datum
sift_test_f2_stage_flush(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);

	sift_stage_flush(rel);
	index_close(rel, NoLock);
	PG_RETURN_VOID();
}

Datum
sift_test_f2_stage_rows(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	uint64		n = sift_stage_rows(rel);

	index_close(rel, NoLock);
	PG_RETURN_INT64(n);
}

Datum
sift_test_f2_slab(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	uint8		lo[SIFT_BOUND_BYTES];
	SiftManifestRead mr = sift_manifest_read_range(rel, f2t_arg_bound(fcinfo, 2, lo), NULL);
	SiftPartSpec spec = slab_spec(rel);
	SiftRowBuf *rb = sift_rowbuf_new(&spec);
	BlockNumber *pages;
	int			np = 0;
	char	   *res;

	if (mr.n == 0 || mr.entries[0].part_id != (uint64) PG_GETARG_INT64(1) || mr.entries[0].l0_slab_blk == InvalidBlockNumber)
		res = "none";
	else
	{
		sift_l0_slab_read(rel, mr.entries[0].part_id, mr.entries[0].l0_slab_blk, rb, &spec, GetActiveSnapshot(), &pages, &np);
		res = psprintf("pages=%d rows=%d", np, rb->n);
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(res));
}

Datum
sift_test_f2_slab_flush(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES];
	SiftPartSpec spec = slab_spec(rel);
	bool		did = sift_l0_slab_flush(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, lo), &spec);

	index_close(rel, NoLock);
	PG_RETURN_BOOL(did);
}

/* (index, part_id, partition lower key, tid_base, first, count, step, xmax): deletes rows first, first + step, ... of a generated part */
Datum
sift_test_f2_delete(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	uint8		lo[SIFT_BOUND_BYTES];
	int			count = PG_GETARG_INT32(5),
				step = PG_GETARG_INT32(6);
	int64		xm = PG_GETARG_INT64(7);
	SiftPendDelRec *recs = palloc(Max(count, 1) * sizeof(SiftPendDelRec));
	SiftPartSpec spec = f2t_spec(false);

	for (int i = 0; i < count; i++)
		recs[i] = (SiftPendDelRec) {f2t_tid(PG_GETARG_INT64(3), PG_GETARG_INT32(4) + i * step), xm == 0 ? InvalidTransactionId : f2t_xmin_of(xm), 0};
	sift_del_route(rel, PG_GETARG_INT64(1), f2t_arg_bound(fcinfo, 2, lo), recs, count, &spec);
	index_close(rel, NoLock);
	PG_RETURN_VOID();
}

Datum
sift_test_f2_pending(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	uint8		lo[SIFT_BOUND_BYTES];
	SiftManifestRead mr = sift_manifest_read_range(rel, f2t_arg_bound(fcinfo, 2, lo), NULL);
	int			nrecs = 0,
				npages = 0;

	if (mr.n > 0 && mr.entries[0].part_id == (uint64) PG_GETARG_INT64(1) && mr.entries[0].pend_del_blk != InvalidBlockNumber)
		(void) sift_pend_del_read(rel, mr.entries[0].part_id, mr.entries[0].pend_del_blk, &nrecs, NULL, &npages);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("recs=%d pages=%d", nrecs, npages)));
}

uint16 *
f2t_dir_rows(Relation rel, const SiftPart *p)
{
	SiftGranuleDesc *dir = sift_part_granule_dir(rel, p);
	uint16	   *rows = palloc(Max(p->hdr.ngranules, 1) * sizeof(uint16));

	for (uint32 g = 0; g < p->hdr.ngranules; g++)
		rows[g] = dir[g].nrows;
	return rows;
}

Datum
sift_test_f2_dv(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftPartDesc *d = f2t_find_gen(parts, n, PG_GETARG_INT64(3));

	if (d->dv_blk != InvalidBlockNumber)
	{
		SiftPart   *p = sift_part_open(rel, d->header_blk, d->part_gen, (uint32) entry.part_id);
		SiftDv	   *dv = sift_dv_read(rel, d, (uint32) entry.part_id, f2t_dir_rows(rel, p));

		for (int i = 0; i < dv->ndirty; i++)
		{
			const SiftDvGran *g = &dv->gran[i];
			Datum		v[4];
			bool		nl[4] = {0};
			bool		unsettled = false;

			for (int r = 0; r < g->ndead; r++)
				unsettled |= TransactionIdIsValid(g->xmax[r].xmax);
			v[0] = Int32GetDatum(g->granule_id);
			v[1] = CStringGetTextDatum(g->ndead == g->granule_rows ? "all_dead" : (g->ndead > SIFT_DV_ARRAY_MAX ? "bitmap" : "array16"));
			v[2] = Int32GetDatum(g->ndead);
			v[3] = BoolGetDatum(unsettled);
			f2t_srf_put(rsi, v, nl);
		}
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

/* (index, part_id, partition lower key, generation): the rows the current snapshot sees, and how the rules judged the part */
Datum
sift_test_f2_visible(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);
	SiftPartDesc *d = f2t_find_gen(parts, n, PG_GETARG_INT64(3));
	SiftPendDelRec *pend = NULL;
	int			npend = 0;
	SiftVisStats st = {0};
	uint64		count;
	Datum		v[2];
	bool		nl[2] = {0};

	if (entry.pend_del_blk != InvalidBlockNumber)
		pend = sift_pend_del_read(rel, entry.part_id, entry.pend_del_blk, &npend, NULL, NULL);
	count = sift_part_count_visible(rel, d, (uint32) entry.part_id, pend, npend, GetActiveSnapshot(), &st);
	v[0] = Int64GetDatum(count);
	v[1] = CStringGetTextDatum(psprintf("clean=%u settled_dv=%u granule_settled=%u per_row=%u rows_checked=%llu", st.parts_clean, st.parts_settled_dv,
										st.granules_settled, st.granules_per_row, (unsigned long long) st.rows_checked));
	index_close(rel, NoLock);
	return f2t_record_result(fcinfo, v, nl);
}

/* (index): packs the tiny parts of the index into shared pages; the parts moved */
Datum
sift_test_f2_pack_tails(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	int			n = sift_compact_pack_tails(rel);

	index_close(rel, NoLock);
	PG_RETURN_INT32(n);
}

/* (index, part_id, partition lower key): per part the header block, whether it is a slot of a packed page and which, and whether it is its bytes in the header page alone */
Datum
sift_test_f2_pack_info(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftPartitionEntry entry;
	int			n;
	SiftPartDesc *parts = f2t_find_parts(rel, fcinfo, 1, 2, &n, &entry);

	for (int i = 0; i < n; i++)
	{
		SiftPart   *p = sift_part_open_desc(rel, &parts[i], (uint32) entry.part_id);
		Datum		v[5] = {Int64GetDatum(parts[i].part_gen), Int64GetDatum(parts[i].header_blk), BoolGetDatum(p->is_packed),
							Int32GetDatum(p->is_packed ? p->pack_slot : -1), BoolGetDatum((p->hdr.flags & SIFT_PART_FLAG_INLINE_STREAM) != 0)};
		bool		nl[5] = {0};

		f2t_srf_put(rsi, v, nl);
	}
	index_close(rel, NoLock);
	return (Datum) 0;
}

/* (index): the state flags of the metapage (SIFT_META_FLAG_*) */
Datum
sift_test_f2_state_flags(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	uint32		flags = sift_col_meta_read(rel).state_flags;

	index_close(rel, NoLock);
	PG_RETURN_INT32((int32) flags);
}

#endif							/* SIFT_TESTING */
