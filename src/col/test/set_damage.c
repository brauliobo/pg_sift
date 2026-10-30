/*
 * col/test/set_damage.c - damage the tests do to the sets, runs and files of a segment
 * the sections of a segment as files, damaged and read back
 * to see the verification refuse it
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "col/am.h"
#include "col/pagg.h"
#include "core/sql.h"
#endif							/* SIFT_TESTING */
#include "col/test/test_int.h"

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_col_set_damage);

PG_FUNCTION_INFO_V1(sift_test_col_super_damage);

PG_FUNCTION_INFO_V1(sift_test_col_run_damage);

PG_FUNCTION_INFO_V1(sift_test_col_run_file_damage);

PG_FUNCTION_INFO_V1(sift_test_col_set_file_damage);

PG_FUNCTION_INFO_V1(sift_test_col_ktab_file_damage);

PG_FUNCTION_INFO_V1(sift_test_col_point_file_damage);

/*
 * sift._test_col_set_damage(index, units): the small-set summaries of the first units of the index, opened as stored, cut at every length, with
 * each byte (of a large blob a sample of them) set to some other values (one at a time) and with the fields of the header the decoder must check set by name (flag bits, kind, count,
 * width, probes, bits, order). A decode must never read outside the blob (meaningful under SANITIZE=1) and must refuse a cut one and each named
 * damage; a damaged blob it accepts is a valid summary of other keys, which the verifier's comparison with the chunk (sift_cset_matches) refuses.
 */
Datum
sift_test_col_set_damage(PG_FUNCTION_ARGS)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	int			maxunits = PG_GETARG_INT32(1);
	TestScan	ts;
	SiftColUnit unit;
	int64		blobs = 0,
				exact = 0,
				bloom = 0,
				intact = 0,
				matching = 0,
				cut = 0,
				cut_accepted = 0,
				named = 0,
				named_accepted = 0,
				damaged = 0,
				accepted = 0,
				missed = 0;
	Datum		v[12];
	bool		nulls[12] = {false};

	ctest_scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (blobs < maxunits && ctest_test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u,
					len,
					rlen;
		const uint8 *rec;
		SiftCUnitView uv;
		const SiftColSchema *sc = sift_col_rel(ts.index)->schema;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		rec = sift_cseg_unit(seg, u, &rlen);
		if (!sift_cunit_parse(rec, rlen, sc, &uv))
			elog(ERROR, "unit does not parse");
		for (int c = 0; c < sc->ncols && blobs < maxunits; c++)
		{
			const uint8 *blob = sift_cseg_set(seg, c, u, &len);
			uint8	   *copy,
					   *orig;
			SiftCSet	s;
			SiftCChunk	ch;
			int64	   *keys;
			uint32		nd;
			SiftCPaggCol col;

			if (blob == NULL)
				continue;
			blobs++;
			orig = palloc(len);
			memcpy(orig, blob, len);		/* the next call on seg reuses the buffer */
			copy = palloc(len);
			if (!sift_cunit_chunk(&uv, c, &ch))
				elog(ERROR, "unit has no chunk");
			keys = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
			sift_cchunk_decode(&ch, keys);
			col = (SiftCPaggCol) {keys, ch.nulls, ch.ovf, ch.n, true};
			{
				int64	   *k2 = palloc(Max(ch.n, 1) * sizeof(int64));

				nd = sift_cset_keys(&col, k2);
				pfree(keys);
				keys = k2;
			}
			if (sift_cset_open(orig, len, &s))
			{
				intact++;
				exact += s.kind == SIFT_CSET_EXACT;
				bloom += s.kind == SIFT_CSET_BLOOM;
				matching += sift_cset_matches(&s, keys, nd);
			}
			/* copy is the blob and nothing else: a cut reads its first l bytes, a damage changes one byte and puts it back (a whole copy for each of them is quadratic in the blob) */
			memcpy(copy, orig, len);
			for (uint32 l = 0; l < len; l++)
			{
				cut++;
				cut_accepted += sift_cset_open(copy, l, &s);
			}
			for (uint32 i = 0; i < len; i++)
			{
				/* a large blob (a filter of thousands of keys): every byte of its first and last 64, and about 256 of the others, a different set for each blob; a damaged byte costs a comparison with the chunk (sift_cset_matches) when the decoder accepts it, so all of them are quadratic in the keys */
				if (len > 512 && i >= 64 && i + 64 < len && (i + blobs) % (len / 256) != 0)
					continue;
				for (int k = 0; k < lengthof(vals); k++)
				{
					if (orig[i] == vals[k])
						continue;
					/* a list read as an exact summary of the same layout is one, and a correct one while it holds at most SIFT_CSET_LIMIT_MAX keys */
					if (i == 0 && orig[0] == SIFT_CSET_PLIST && vals[k] == SIFT_CSET_EXACT)
						continue;
					copy[i] = vals[k];
					damaged++;
					if (sift_cset_open(copy, len, &s))
					{
						accepted++;
						missed += sift_cset_matches(&s, keys, nd);
					}
				}
				copy[i] = orig[i];
			}
			/* the named damages of the header: flag bits, unknown kinds, counts, and (exact) width, order; (bloom) probes and bits */
			for (int f = 0; f < 11; f++)
			{
				uint32		n = 0;
				uint64		big = UINT64_MAX;

				memcpy(copy, orig, len);
				switch (f)
				{
					case 0: copy[2] = 1; break;							/* flag bits */
					case 1: copy[3] = 0x80; break;
					case 2: copy[0] = 0; break;							/* kind */
					case 3: copy[0] = 6; break;
					case 4: memcpy(copy + 4, &n, 4); break;				/* no keys */
					case 5: n = UINT32_MAX; memcpy(copy + 4, &n, 4); break;	/* a count that overflows 32 bits when multiplied */
					case 6: if (orig[0] == SIFT_CSET_PRES) continue; copy[1] = 0; break;							/* width or probes 0 */
					case 7: copy[1] = orig[0] == SIFT_CSET_EXACT || orig[0] == SIFT_CSET_PLIST ? 3 : 17; break;
					case 8: memcpy(copy + 8, &big, 8); break;			/* the bits of a filter; the base of an exact set (whose first offset then is not its smallest key) is not damage */
					case 9:
						if (orig[0] == SIFT_CSET_EXACT || orig[0] == SIFT_CSET_PLIST)	/* unsorted: the first two offsets swapped, the larger first */
						{
							if (orig[1] > 8 || len < SIFT_CSET_HDR + 2 * orig[1])
								continue;
							memcpy(copy + SIFT_CSET_HDR, orig + SIFT_CSET_HDR + orig[1], orig[1]);
							memcpy(copy + SIFT_CSET_HDR + orig[1], orig + SIFT_CSET_HDR, orig[1]);
						}
						else
							memset(copy + SIFT_CSET_HDR, 0, len - SIFT_CSET_HDR);	/* an empty filter */
						break;
					default:											/* one byte of the body gone */
						named++;
						named_accepted += sift_cset_open(copy, len - 1, &s);
						continue;
				}
				if (f == 8 && (orig[0] == SIFT_CSET_EXACT || orig[0] == SIFT_CSET_PLIST))
					continue;
				named++;
				named_accepted += sift_cset_open(copy, len, &s);
			}
			pfree(orig);
			pfree(copy);
			pfree(keys);
		}
	}
	ctest_scan_close(&ts);
	v[0] = Int64GetDatum(blobs);
	v[1] = Int64GetDatum(exact);
	v[2] = Int64GetDatum(bloom);
	v[3] = Int64GetDatum(intact);
	v[4] = Int64GetDatum(matching);
	v[5] = Int64GetDatum(cut);
	v[6] = Int64GetDatum(cut_accepted);
	v[7] = Int64GetDatum(named);
	v[8] = Int64GetDatum(named_accepted);
	v[9] = Int64GetDatum(damaged);
	v[10] = Int64GetDatum(accepted);
	v[11] = Int64GetDatum(missed);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/*
 * sift._test_col_super_damage(index): every byte of the superunit block of every segment of the index, read as itself with a bit flipped (two flips), one
 * damage at a time, through a fresh open of the segment and the check of sift.col_verify. A damage must never read outside the block (meaningful under
 * SANITIZE=1) and every one must be refused, by the reader or by the check: `unnoticed` counts the ones that were not.
 */
Datum
sift_test_col_super_damage(PG_FUNCTION_ARGS)
{
	static const uint8 masks[] = {0x01, 0x80};
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64		supers = 0,
				damaged = 0,
				refused = 0;
	Datum		v[4];
	bool		nulls[4] = {false};
	MemoryContext old = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint64		off,
					len;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		if (!sift_cseg_test_super_span(seg, &off, &len))
		{
			sift_cseg_close(seg);
			continue;
		}
		sift_cseg_close(seg);
		supers++;
		for (uint64 b = 0; b < len; b++)
			for (int m = 0; m < lengthof(masks); m++)
			{
				MemoryContext cx = AllocSetContextCreate(old, "super damage", ALLOCSET_DEFAULT_SIZES);

				damaged++;
				BeginInternalSubTransaction(NULL);
				MemoryContextSwitchTo(cx);
				PG_TRY();
				{
					SiftColSegment *s2;

					sift_test_seg_flip_at = off + b;
					sift_test_seg_flip_mask = masks[m];
					s2 = sift_cseg_open(index, &snap->segs[i], cx);
					sift_cseg_verify_super(s2);
					sift_test_seg_flip_at = UINT64_MAX;
					sift_cseg_close(s2);
					MemoryContextSwitchTo(old);
					ReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
				}
				PG_CATCH();
				{
					sift_test_seg_flip_at = UINT64_MAX;
					MemoryContextSwitchTo(old);
					FlushErrorState();
					RollbackAndReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
					refused++;
				}
				PG_END_TRY();
				MemoryContextDelete(cx);
			}
	}
	index_close(index, AccessShareLock);
	v[0] = Int64GetDatum(supers);
	v[1] = Int64GetDatum(damaged);
	v[2] = Int64GetDatum(refused);
	v[3] = Int64GetDatum(damaged - refused);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/*
 * sift._test_col_run_file_damage(index, stride): the bytes of the sorted runs of every segment of the index (all of the structure, every stride-th entry of
 * a run), read as themselves with a bit flipped (two flips), one damage at a time, through a fresh open of the segment and the check of sift.col_verify. A damage
 * must never read outside the section (meaningful under SANITIZE=1) and every one in a byte the reader or the check uses must be refused: `unnoticed`
 * counts the ones that were not. The padding between the blocks is not counted.
 */
static Datum
section_file_damage(PG_FUNCTION_ARGS, bool (*span) (SiftColSegment *, uint64 *, uint64 *, uint8 **))
{
	static const uint8 masks[] = {0x01, 0x80};
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	int32		stride = Max(PG_GETARG_INT32(1), 1);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64		runs = 0,
				damaged = 0,
				refused = 0,
				unnoticed = 0;
	Datum		v[4];
	bool		nulls[4] = {false};
	MemoryContext old = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint64		off,
					len;
		uint8	   *used;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		if (!span(seg, &off, &len, &used))
		{
			sift_cseg_close(seg);
			continue;
		}
		sift_cseg_close(seg);
		runs++;
		for (uint64 b = 0; b < len; b++)
			for (int m = 0; m < lengthof(masks) && (used[b] == 2 || (used[b] == 1 && b % stride == 0)); m++)
			{
				MemoryContext cx = AllocSetContextCreate(old, "run file damage", ALLOCSET_DEFAULT_SIZES);

				damaged++;
				BeginInternalSubTransaction(NULL);
				MemoryContextSwitchTo(cx);
				PG_TRY();
				{
					SiftColSegment *s2;

					sift_test_seg_flip_at = off + b;
					sift_test_seg_flip_mask = masks[m];
					s2 = sift_cseg_open(index, &snap->segs[i], cx);
					sift_cseg_verify(s2);
					sift_test_seg_flip_at = UINT64_MAX;
					sift_cseg_close(s2);
					unnoticed += used[b] != 0;
					MemoryContextSwitchTo(old);
					ReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
				}
				PG_CATCH();
				{
					sift_test_seg_flip_at = UINT64_MAX;
					MemoryContextSwitchTo(old);
					FlushErrorState();
					RollbackAndReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
					refused++;
				}
				PG_END_TRY();
				MemoryContextDelete(cx);
			}
	}
	index_close(index, AccessShareLock);
	v[0] = Int64GetDatum(runs);
	v[1] = Int64GetDatum(damaged);
	v[2] = Int64GetDatum(refused);
	v[3] = Int64GetDatum(unnoticed);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

Datum
sift_test_col_run_file_damage(PG_FUNCTION_ARGS)
{
	return section_file_damage(fcinfo, sift_cseg_test_run_span);
}

/* sift._test_col_set_file_damage(index, stride): the same for the small-set summaries and the key presence */
Datum
sift_test_col_set_file_damage(PG_FUNCTION_ARGS)
{
	return section_file_damage(fcinfo, sift_cseg_test_set_span);
}

/* sift._test_col_ktab_file_damage(index, stride): the same for the key tables */
Datum
sift_test_col_ktab_file_damage(PG_FUNCTION_ARGS)
{
	return section_file_damage(fcinfo, sift_cseg_test_ktab_span);
}

/* sift._test_col_point_file_damage(index, stride): the same for the point directories at the end of the dictionary section */
Datum
sift_test_col_point_file_damage(PG_FUNCTION_ARGS)
{
	return section_file_damage(fcinfo, sift_cseg_test_point_span);
}

/*
 * sift._test_col_run_damage(index, units): the sorted runs of the first units of the index, opened as stored, cut at every length, with each byte set
 * to some other values (one at a time) and with the fields of the header set by name. The header decoder must refuse a cut run and each named damage;
 * a damaged run whose header it accepts has its entries read (an ordinal that is no row of the unit is refused) and, if they are read, is another run
 * that the comparison with the chunk (what sift.col_verify does) refuses.
 */
Datum
sift_test_col_run_damage(PG_FUNCTION_ARGS)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	int			maxunits = PG_GETARG_INT32(1);
	TestScan	ts;
	SiftColUnit unit;
	int64		blobs = 0,
				ord = 0,
				ident = 0,
				intact = 0,
				matching = 0,
				cut = 0,
				cut_accepted = 0,
				named = 0,
				named_accepted = 0,
				damaged = 0,
				accepted = 0,
				missed = 0;
	Datum		v[12];
	bool		nulls[12] = {false};

	ctest_scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (blobs < maxunits && ctest_test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u,
					rlen;
		const uint8 *rec;
		SiftCUnitView uv;
		const SiftColSchema *sc = sift_col_rel(ts.index)->schema;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		rec = sift_cseg_unit(seg, u, &rlen);
		if (!sift_cunit_parse(rec, rlen, sc, &uv))
			elog(ERROR, "unit does not parse");
		for (int c = 0; c < sc->ncols && blobs < maxunits; c++)
		{
			SiftCRun	r,
						r2;
			uint64		at;
			uint8	   *orig,
					   *copy;
			uint32		len,
					   *ords;
			SiftCChunk	ch;
			SiftCPaggCol col;
			int64	   *keys;
			SiftKey    *addr;

			if (!sift_cseg_run(seg, c, u, &r, &at))
				continue;
			blobs++;
			len = sift_crun_total(&r);
			orig = palloc(len);
			copy = palloc(len);
			ords = palloc(r.n * sizeof(uint32) + 1);
			sift_cseg_run_blob(seg, at, &r, orig);
			if (!sift_cunit_chunk(&uv, c, &ch))
				elog(ERROR, "unit has no chunk");
			keys = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
			sift_cchunk_decode(&ch, keys);
			col = (SiftCPaggCol) {keys, ch.nulls, ch.ovf, ch.n, true};
			addr = palloc((Size) ch.n * sizeof(SiftKey));
			sift_cunit_keys(&uv, seg->sbdir[u].id, addr);
			ord += r.kind == SIFT_CRUN_ORD;
			ident += r.kind == SIFT_CRUN_IDENT;
			if (sift_crun_head(orig, len, r.n, &r2))
			{
				intact++;
				matching += sift_crun_matches(&sc->cols[c], &col, addr, orig, len);
			}
			for (uint32 l = 0; l < len; l++)
			{
				cut++;
				cut_accepted += sift_crun_head(orig, l, r.n, &r2);
			}
			for (uint32 i = 0; i < len; i++)
				for (int k = 0; k < lengthof(vals); k++)
				{
					memcpy(copy, orig, len);
					if (copy[i] == vals[k])
						continue;
					copy[i] = vals[k];
					damaged++;
					if (sift_crun_head(copy, len, r.n, &r2) && sift_crun_entries(&r2, copy + SIFT_CRUN_HDR, 0, r2.n, ords))
					{
						accepted++;
						missed += sift_crun_matches(&sc->cols[c], &col, addr, copy, len);
					}
				}
			/* the named damages of the header: flag bits, kind, the row count, counts that do not fit, the width */
			for (int f = 0; f < 8; f++)
			{
				uint32		n = r.n + 1,
							big = UINT32_MAX;

				memcpy(copy, orig, len);
				switch (f)
				{
					case 0: copy[2] = 1; break;
					case 1: copy[3] = 0x80; break;
					case 2: copy[0] = 0; break;
					case 3: copy[0] = 3; break;
					case 4: memcpy(copy + 4, &n, 4); break;
					case 5: memcpy(copy + 8, &big, 4); break;
					case 6: memcpy(copy + 12, &big, 4); break;
					default: copy[1] = copy[1] == 4 ? 1 : 4; break;
				}
				named++;
				named_accepted += sift_crun_head(copy, len, r.n, &r2);
			}
			pfree(orig);
			pfree(copy);
			pfree(ords);
			pfree(keys);
		}
	}
	ctest_scan_close(&ts);
	v[0] = Int64GetDatum(blobs);
	v[1] = Int64GetDatum(ord);
	v[2] = Int64GetDatum(ident);
	v[3] = Int64GetDatum(intact);
	v[4] = Int64GetDatum(matching);
	v[5] = Int64GetDatum(cut);
	v[6] = Int64GetDatum(cut_accepted);
	v[7] = Int64GetDatum(named);
	v[8] = Int64GetDatum(named_accepted);
	v[9] = Int64GetDatum(damaged);
	v[10] = Int64GetDatum(accepted);
	v[11] = Int64GetDatum(missed);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}
#endif							/* SIFT_TESTING */
