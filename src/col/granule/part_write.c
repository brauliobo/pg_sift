/*
 * col/granule/part_write.c - a part of format major 2 written from rows: the granules, their directory, the TID index, the zones, the dictionaries of the strings and the versions.
 */
#include "core/host.h"

#include "access/transam.h"
#include "lib/stringinfo.h"

#include "col/fsst.h"
#include "col/granule/part_int.h"
#include "col/f2/f2_aux_budget.h"
#include "col/f2/f2_kll.h"
#include "col/f2/f2_point_dir.h"
#include "col/f2/f2_marks.h"
#include "col/f2/preagg_delta.h"
#include "col/skip/skip_index.h"
#include "core/page_io.h"

/* The least and the greatest time of each run of SIFT_TSTRIDE_ROWS rows of a granule */
static void
put_strides(StringInfo out, const SiftColIn *time, int cnt)
{
	for (int from = 0; from < cnt; from += SIFT_TSTRIDE_ROWS)
	{
		SiftTimeStride st = {.time_min = PG_INT64_MAX,.time_max = PG_INT64_MIN};

		for (int r = from; r < Min(from + SIFT_TSTRIDE_ROWS, cnt); r++)
			if (time->isnull == NULL || !time->isnull[r])
			{
				st.time_min = Min(st.time_min, time->i[r]);
				st.time_max = Max(st.time_max, time->i[r]);
			}
		appendBinaryStringInfo(out, (char *) &st, sizeof(st));
	}
}

int
sift_rows_granule_len(const SiftRows *rows, int from)
{
	int			to = from;

	while (to < rows->nrows && to - from < SIFT_GRANULE_ROWS)
	{
		bool		fits = true;

		for (int c = 0; c < rows->ncols && fits; c++)
			fits = rows->cols[c].eoff == NULL || rows->cols[c].eoff[to + 1] - rows->cols[c].eoff[from] <= SIFT_GRANULE_ROWS;
		if (!fits)
			break;
		to++;
	}
	if (to == from)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: a row has more than %d elements in one array of objects", SIFT_GRANULE_ROWS),
						errhint("Leave format_major at 1, or take the array out of the paths of the index.")));
	return to - from;
}

/* The slice of the granule of rows [from, from + cnt) of column c: a column of elements starts at the first element of those rows */
static SiftColIn
granule_slice(const SiftRows *rows, int c, int from)
{
	const SiftColIn *col = &rows->cols[c];

	return sift_colin_slice(col, col->group1 > 0 ? (int) rows->cols[col->group1 - 1].eoff[from] : from);
}

/* The values a slice holds: the rows of the granule, or the elements of the arrays of its rows */
static int
slice_len(const SiftColIn *gcols, int c, int cnt)
{
	const SiftColIn *col = &gcols[c];

	return col->group1 > 0 ? (int) (gcols[col->group1 - 1].eoff[cnt] - gcols[col->group1 - 1].eoff[0]) : cnt;
}

/* The values a column holds over the whole part: its rows, or the elements of the arrays of its rows */
static int
column_values(const SiftRows *rows, int c)
{
	const SiftColIn *col = &rows->cols[c];

	return col->group1 > 0 ? (int) rows->cols[col->group1 - 1].eoff[rows->nrows] : rows->nrows;
}


/* The dictionaries of the string columns of a part that has far fewer distinct strings than rows: NULL when no column has one */
static SiftDictEnc **
part_dict_encs(const SiftRows *rows)
{
	/* check-memory: allow one pointer for each column of the part, not for each row */
	SiftDictEnc **dicts = palloc0(Max(rows->ncols, 1) * sizeof(SiftDictEnc *));
	int			nd = 0;

	for (int c = 0; c < rows->ncols; c++)
		nd += (dicts[c] = rows->cols[c].pdict != NULL ? rows->cols[c].pdict : sift_dict_enc_build(&rows->cols[c], column_values(rows, c))) != NULL;
	if (nd == 0)
	{
		pfree(dicts);
		return NULL;
	}
	return dicts;
}

/* The strings of one dictionary of the part, SIFT_DICTS_CHUNK to a list of the codec, appended to sec; the entry of it */
static SiftDictsEntry
put_str_dict(StringInfo sec, int c, const SiftDictEnc *d, const SiftCcPolicy *pol)
{
	SiftDictsEntry ent = {.col = c,.nchunks = SIFT_DICT_PACK_NC(SIFT_DICT_KIND_STR, (d->n + SIFT_DICTS_CHUNK - 1) / SIFT_DICTS_CHUNK),.n = d->n,.off = sec->len};

	for (uint32 from = 0; from < d->n; from += SIFT_DICTS_CHUNK)
	{
		uint32		cnt = Min(SIFT_DICTS_CHUNK, d->n - from);
		uint32	   *off = palloc((cnt + 1) * sizeof(uint32));
		StringInfoData bytes,
					blob;
		uint32		ch[2];

		initStringInfo(&bytes);
		off[0] = 0;
		for (uint32 i = 0; i < cnt; i++)
		{
			appendBinaryStringInfo(&bytes, d->p[from + i], d->len[from + i]);
			off[i + 1] = bytes.len;
		}
		initStringInfo(&blob);
		sift_cc_encode_strs(&blob, off, bytes.data, cnt, pol, NULL);
		ch[0] = blob.len;
		ch[1] = cnt;
		appendBinaryStringInfo(sec, (char *) ch, 8);
		appendBinaryStringInfo(sec, blob.data, blob.len);
		pad8(sec);
		pfree(off);
		pfree(bytes.data);
		pfree(blob.data);
	}
	ent.len = sec->len - ent.off;
	return ent;
}

/*
 * SEC_DICTS: a header, an entry for each column that has a dictionary of its strings, a table of symbols or a zstd dictionary (by column), then what each holds (the strings in byte order in
 * lists of the codec, the table as stored, the dictionary as it is): the granules keep only the codes, or code their strings by the table, or compress them by the dictionary.
 */
static void
put_dicts(const SiftRows *rows, SiftDictEnc **dicts, const SiftPartCodecs *pc, StringInfo sec)
{
	int			nd = pc->nentries;
	SiftDictsEntry *ent;
	uint32		hdr[2];
	SiftCcPolicy pol;
	int			k = 0;

	for (int c = 0; c < rows->ncols; c++)
		nd += dicts != NULL && dicts[c] != NULL;
	sift_cc_policy_tier(&pol, SIFT_TIER_WARM_L1, rows->compress_alg);
	/* check-memory: allow one entry for each column of the part, not for each row */
	ent = palloc0(nd * sizeof(SiftDictsEntry));
	hdr[0] = nd;
	hdr[1] = 0;
	appendBinaryStringInfo(sec, (char *) hdr, 8);
	appendStringInfoSpaces(sec, nd * sizeof(SiftDictsEntry));
	for (int c = 0; c < rows->ncols; c++)
	{
		if (dicts != NULL && dicts[c] != NULL)
			ent[k++] = put_str_dict(sec, c, dicts[c], &pol);
		else if (pc->raw[c] != NULL)
		{
			SiftDictKind kind = pc->cdict[c] != NULL ? SIFT_DICT_KIND_ZSTD : SIFT_DICT_KIND_FSST;

			ent[k++] = (SiftDictsEntry) {.col = c,.nchunks = SIFT_DICT_PACK_NC(kind, 0),.n = kind == SIFT_DICT_KIND_ZSTD ? pc->rawlen[c] : pc->fsst[c]->nsym,.off = sec->len,.len = pc->rawlen[c]};
			appendBinaryStringInfo(sec, (char *) pc->raw[c], pc->rawlen[c]);
			pad8(sec);
		}
	}
	memcpy(sec->data + 8, ent, nd * sizeof(SiftDictsEntry));
	pfree(ent);
}

/* The bytes of the data of the part that compress: all of it but the fixed 16 bytes of the uuids, which no codec shrinks */
static uint64
part_compressible_bytes(const SiftRows *rows, uint64 data_bytes)
{
	uint64		raw = 0;

	for (int c = 0; c < rows->ncols; c++)
		if (rows->cols[c].hint == SIFT_CHINT_UUID)
			raw += 16 * (uint64) rows->nrows;
	return raw >= data_bytes ? 0 : data_bytes - raw;
}

/* The bytes the sections that are not the data take in the header page of the part (those the writer is yet to add are small beside them) */
static uint64
part_inline_bytes(const SiftSecInput *secs, int nsec)
{
	uint64		bytes = 0;

	for (int i = 0; i < nsec; i++)
		bytes += secs[i].tag == SEC_GRANULES || secs[i].tag == SEC_DICTS ? 0 : secs[i].len;
	return bytes;
}

/* The columns of the part whose rows are, but for a few, one type that their text spells (an astext leaf), or a prefix and a number (any text, even against a dictionary of the part when the numbers cost less); NULL when there are none */
static SiftColPromo *
part_promos(const SiftRows *rows, SiftDictEnc **dicts, int *npromo)
{
	/* check-memory: allow one descriptor for each column of the part, not for each row */
	SiftColPromo *pm = palloc0(Max(rows->ncols, 1) * sizeof(SiftColPromo));

	for (int c = 0; c < rows->ncols; c++)
		if (sift_promo_infer(&rows->cols[c], rows->nrows, dicts != NULL ? dicts[c] : NULL, &pm[c]))
		{
			pm[c].col = c;
			if (dicts != NULL)
				dicts[c] = NULL;
			(*npromo)++;
		}
	if (*npromo == 0)
	{
		pfree(pm);
		return NULL;
	}
	return pm;
}

/* The bounds of each time column of the rows, from the zones of its granules; the flags of the slots that have one */
static uint32
part_time_bounds(const SiftRows *rows, const SiftZone *zones, int ng, int64 *tmin, int64 *tmax)
{
	uint32		flags = 0;

	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		int			c = rows->time_cols[s] - 1;

		if (c < 0)
			continue;
		tmin[s] = PG_INT64_MAX;
		tmax[s] = PG_INT64_MIN;
		for (int g = 0; g < ng; g++)
		{
			const SiftZone *z = &zones[(uint64) c * ng + g];

			if (!(z->flags & SIFT_ZONE_NONE))
			{
				tmin[s] = Min(tmin[s], z->min);
				tmax[s] = Max(tmax[s], z->max);
			}
		}
		flags |= s == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1;
	}
	return flags;
}

/* Every row of the cnt from `from` was inserted by one transaction at one command (and none is frozen): a bulk load */
static bool
single_xact(const SiftRows *rows, int from, int cnt)
{
	for (int r = 0; r < cnt; r++)
		if (!TransactionIdIsNormal(rows->xmin[from + r]) || rows->xmin[from + r] != rows->xmin[from] || rows->cmin[from + r] != rows->cmin[from])
			return false;
	return true;
}

/* The part of rows, written; with memory, kept in memory instead and written nowhere */
static SiftPartDesc
write_rows(Relation rel, uint32 part_id, uint64 part_gen, uint16 level, SiftCompressTier tier, const SiftRows *rows, SiftPart **memory)
{
	int			n = rows->nrows,
				ng = 0,
				nc = rows->ncols;
	int		   *gstart;
	StringInfoData gdata,
				vers,
				schema;
	SiftGranuleDesc *dir;
	SiftTidIndexEntry *tix;
	SiftZone   *zones;
	SiftColIn  *gcols = palloc(Max(nc, 1) * sizeof(SiftColIn));
	FullTransactionId pmin = InvalidFullTransactionId,
				pmax = InvalidFullTransactionId;
	bool		unfrozen = false;
	bool		want_preagg = rows->preagg && level > 0;
	uint32	   *pa_first;
	StringInfoData pa_cells;
	SiftSecInput secs[20];
	StringInfoData tstride,
				excs;
	uint32	   *tstride_off;
	int			nsec = 0;
	SiftPartIn in = {0};
	SiftPartDesc d;
	uint8		kmin[SIFT_BOUND_BYTES],
				kmax[SIFT_BOUND_BYTES];
	SiftDictEnc **dicts = NULL;
	StringInfoData dictsec,
				skipsec,
				pointsec;
	int64		tmin[SIFT_MANIFEST_TIME_COLS] = {0},
				tmax[SIFT_MANIFEST_TIME_COLS] = {0};
	SiftColPromo *promos = NULL;
	SiftColStatsBuilder *stats;
	void	   *marks,
			   *statsec;
	uint64		marks_len,
				stats_len;
	SiftAuxBudget aux;
	bool		stats_in;
	int			npromo = 0;
	SiftPartCodecs *codecs = NULL;

	if (n < 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: a part holds at least one row")));
	gstart = palloc((n + 1) * sizeof(int));
	for (int from = 0; from < n; ng++)
	{
		gstart[ng] = from;
		from += sift_rows_granule_len(rows, from);
	}
	gstart[ng] = n;
	dir = palloc(ng * sizeof(SiftGranuleDesc));
	tix = palloc0(ng * sizeof(SiftTidIndexEntry));
	zones = palloc0(Max(nc, 1) * ng * sizeof(SiftZone));
	pa_first = palloc((ng + 1) * sizeof(uint32));
	tstride_off = palloc((ng + 1) * sizeof(uint32));
	initStringInfo(&gdata);
	initStringInfo(&vers);
	initStringInfo(&schema);
	initStringInfo(&pa_cells);
	initStringInfo(&tstride);
	initStringInfo(&excs);
	initStringInfo(&dictsec);
	initStringInfo(&skipsec);
	if (SIFT_TIER_IS_COLD(tier))
	{
		dicts = part_dict_encs(rows);
		promos = part_promos(rows, dicts, &npromo);
		codecs = sift_part_codecs_train(rows, tier, (SiftDictEnc *const *) dicts, promos, rows->adopt_dicts);
		if (dicts != NULL || codecs->nentries > 0)
			put_dicts(rows, dicts, codecs, &dictsec);
	}
	stats = sift_col_stats_begin((uint16) nc);
	for (int g = 0; g < ng; g++)
	{
		int			from = gstart[g],
					cnt = gstart[g + 1] - from;
		SiftGranuleIn gi = {.nrows = cnt,.ncols = nc,.tid = rows->tid + from,.cols = gcols,.row_base = from,.compress_alg = rows->compress_alg,.tier = tier,
			.dicts = (const SiftDictEnc *const *) dicts,.part_fsst = codecs != NULL ? (const SiftFsst *const *) codecs->fsst : NULL,.part_cdict = codecs != NULL ? codecs->cdict : NULL};
		TransactionId lo = InvalidTransactionId,
					hi = InvalidTransactionId;
		bool		frozen = true;

		for (int c = 0; c < nc; c++)
			{
				gcols[c] = granule_slice(rows, c, from);
				gcols[c].promo = promos != NULL && promos[c].kind != SIFT_PROMO_NONE ? &promos[c] : NULL;
			}
		for (int r = 0; r < cnt; r++)
		{
			TransactionId x = rows->xmin[from + r];

			if (TransactionIdIsNormal(x))
			{
				frozen = false;
				lo = TransactionIdIsValid(lo) && TransactionIdPrecedes(lo, x) ? lo : x;
				hi = TransactionIdIsValid(hi) && TransactionIdFollows(hi, x) ? hi : x;
			}
		}
		gi.all_frozen = frozen;
		gi.min_xid = frozen ? sift_xid_to_fxid(FrozenTransactionId) : sift_xid_to_fxid(lo);
		gi.max_xid = frozen ? sift_xid_to_fxid(FrozenTransactionId) : sift_xid_to_fxid(hi);
		dir[g] = sift_granule_encode(&gdata, &gi);
		pa_first[g] = pa_cells.len / sizeof(SiftPreaggCell);
		if (want_preagg)
		{
			SiftGranuleView gv = sift_granule_open(rel, gdata.data + dir[g].data_off, dir[g].data_len, &dir[g]);
			int			ncells;
			SiftPreaggCell *cells = sift_preagg_build(&gv, rows->preagg_dim, rows->preagg_measure, &ncells);

			appendBinaryStringInfo(&pa_cells, (char *) cells, ncells * sizeof(SiftPreaggCell));
			dir[g].flags |= SIFT_GDESC_HAS_PREAGG;
		}
		if (!FullTransactionIdIsValid(pmin) || FullTransactionIdPrecedes(gi.min_xid, pmin))
			pmin = gi.min_xid;
		if (!FullTransactionIdIsValid(pmax) || FullTransactionIdFollows(gi.max_xid, pmax))
			pmax = gi.max_xid;
		tix[g].tid_min = dir[g].tid_min;
		tix[g].tid_max = dir[g].tid_max;
		for (int r = 0; r < cnt; r++)
			tix[g].blk_bloom |= sift_tid_block_bit(rows->tid[from + r]);
		for (int c = 0; c < nc; c++)
		{
			SiftZone   *z = &zones[c * ng + g];
			int			nv = slice_len(gcols, c, cnt);

			const SiftChunkRef *ref = &((const SiftGranuleHeader *) (gdata.data + dir[g].data_off))->chunks[c + 1];

			z->flags = SIFT_ZONE_NONE;
			for (int r = 0; r < nv; r++)
			{
				if (gcols[c].isnull && gcols[c].isnull[r])
					z->nnull++;
				else if (gcols[c].kind != SIFT_CELL_INT)
					continue;
				else if (z->flags & SIFT_ZONE_NONE)
				{
					z->min = z->max = gcols[c].i[r];
					z->flags &= ~SIFT_ZONE_NONE;
				}
				else
				{
					z->min = Min(z->min, gcols[c].i[r]);
					z->max = Max(z->max, gcols[c].i[r]);
				}
			}
			if (gcols[c].promo != NULL && gcols[c].promo->kind != SIFT_PROMO_PREFIX_INT && (ref->enc & SIFT_ENC_MASK) == SIFT_CENC_STR_PROMO)
				sift_promo_zone(gcols[c].promo, &gcols[c], cnt, z);
			if (ref->enc & SIFT_ENC_KEY_ALL_ABSENT)
				z->flags |= SIFT_ZONE_KEY_ALL_ABSENT;
			if (ref->enc & SIFT_ENC_KEY_ALL_PRESENT)
				z->flags |= SIFT_ZONE_KEY_ALL_PRESENT;
			sift_f2_col_stats_granule(stats, c, &gcols[c], nv, z->nnull, ref);
		}
		tstride_off[g] = tstride.len / sizeof(SiftTimeStride);
		if (rows->time_col > 0)
			put_strides(&tstride, &gcols[rows->time_col - 1], cnt);
		for (int c = 0; c < nc; c++)
			if (gcols[c].tag != NULL)
			{
				SiftVariantExcSummary es = {.granule = g,.col = c};

				for (int r = 0; r < slice_len(gcols, c, cnt); r++)
					es.n_entries += gcols[c].tag[r] != 0;
				if (es.n_entries > 0)
					appendBinaryStringInfo(&excs, (char *) &es, sizeof(es));
			}
		if (!frozen)
		{
			unfrozen = true;
			if (single_xact(rows, from, cnt))
			{
				dir[g].flags |= SIFT_GDESC_SINGLE_XACT;
				dir[g].single_cmin = rows->cmin[from];
			}
			else
				for (int r = 0; r < cnt; r++)
				{
					SiftRowVer rv = {rows->xmin[from + r], rows->cmin[from + r]};

					appendBinaryStringInfo(&vers, (char *) &rv, sizeof(rv));
				}
		}
	}
	if (rows->keys == NULL)
	{
		memcpy(kmin, rows->key_lo, SIFT_BOUND_BYTES);
		memcpy(kmax, rows->key_hi, SIFT_BOUND_BYTES);
	}
	else
	{
		memset(kmin, 0xFF, sizeof(kmin));
		memset(kmax, 0, sizeof(kmax));
		for (int r = 0; r < n; r++)
		{
			if (memcmp(rows->keys[r], kmin, SIFT_BOUND_BYTES) < 0)
				memcpy(kmin, rows->keys[r], SIFT_BOUND_BYTES);
			if (memcmp(rows->keys[r], kmax, SIFT_BOUND_BYTES) > 0)
				memcpy(kmax, rows->keys[r], SIFT_BOUND_BYTES);
		}
	}

	{
		bool		has_defaults = rows->defaults_len > 0;
		uint16		hdr[2] = {nc, rows->nsort | (npromo > 0 ? SIFT_SCHEMA_EXT_PROMO : 0) | (rows->ncol_ids > 0 ? SIFT_SCHEMA_EXT_ATTNUM : 0) | (has_defaults ? SIFT_SCHEMA_EXT_DEFAULTS : 0)};

		if (rows->ncol_ids != 0 && rows->ncol_ids != nc)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: a part names %d column identities for %d columns", rows->ncol_ids, nc)));
		if (has_defaults && rows->ncol_ids == 0)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift: a part has defaults but no column identities")));

		appendBinaryStringInfo(&schema, (char *) hdr, 4);
		for (int c = 0; c < nc; c++)
			appendStringInfoChar(&schema, (char) rows->cols[c].kind);
		while (schema.len % 2 != 0)
			appendStringInfoChar(&schema, '\0');
		appendBinaryStringInfo(&schema, (char *) rows->sort_cols, rows->nsort * sizeof(uint16));
		for (int k = 0; k < rows->nsort; k++)
			appendStringInfoChar(&schema, rows->sort_flags ? (char) rows->sort_flags[k] : 0);
		if (npromo > 0)
		{
			uint16		cnt = npromo;

			pad8(&schema);
			appendBinaryStringInfo(&schema, (char *) &cnt, 2);
			pad8(&schema);
			for (int c = 0; c < nc; c++)
				if (promos[c].kind != SIFT_PROMO_NONE)
					appendBinaryStringInfo(&schema, (char *) &promos[c], sizeof(SiftColPromo));
		}
		if (rows->ncol_ids > 0)
		{
			pad8(&schema);
			appendBinaryStringInfo(&schema, (const char *) rows->col_ids, rows->ncol_ids * sizeof(SiftPartColIdentity));
		}
		if (has_defaults)
		{
			uint64		stamp = rows->defaults_stamp;

			pad8(&schema);
			appendBinaryStringInfo(&schema, (const char *) &stamp, sizeof(stamp));
		}
	}
	sift_part_add_section(secs, &nsec, SEC_SCHEMA, SIFT_SEC_FLAG_REQUIRED, schema.data, schema.len, true);
	if (rows->defaults_len > 0)
		sift_part_add_section(secs, &nsec, SEC_SCHEMA_DEFAULTS, 0, rows->defaults, rows->defaults_len, false);
	sift_part_add_section(secs, &nsec, SEC_GRANULE_DIR, SIFT_SEC_FLAG_REQUIRED, dir, ng * sizeof(SiftGranuleDesc), false);
	sift_part_add_section(secs, &nsec, SEC_GRANULES, SIFT_SEC_FLAG_REQUIRED, gdata.data, gdata.len, false);
	if (dictsec.len > 0)
		sift_part_add_section(secs, &nsec, SEC_DICTS, SIFT_SEC_FLAG_REQUIRED, dictsec.data, dictsec.len, false);
	if (vers.len > 0)
		sift_part_add_section(secs, &nsec, SEC_VERS, SIFT_SEC_FLAG_REQUIRED, vers.data, vers.len, false);
	sift_part_add_section(secs, &nsec, SEC_TID_INDEX, 0, tix, ng * sizeof(SiftTidIndexEntry), false);
	if (want_preagg)
	{
		StringInfoData pa;
		uint32		hdrlen = MAXALIGN((ng + 1) * sizeof(uint32));

		pa_first[ng] = pa_cells.len / sizeof(SiftPreaggCell);
		initStringInfo(&pa);
		appendBinaryStringInfo(&pa, (char *) pa_first, (ng + 1) * sizeof(uint32));
		while (pa.len < hdrlen)
			appendStringInfoChar(&pa, '\0');
		appendBinaryStringInfo(&pa, pa_cells.data, pa_cells.len);
		sift_part_add_section(secs, &nsec, SEC_PREAGG, 0, pa.data, pa.len, false);
	}
	sift_part_add_section(secs, &nsec, SEC_ZONES, 0, zones, nc * ng * sizeof(SiftZone), false);
	if (rows->time_col > 0)
	{
		StringInfoData ts;
		SiftTStrideHeader th = {.ngranules = ng,.total_strides = tstride.len / sizeof(SiftTimeStride)};

		tstride_off[ng] = th.total_strides;
		initStringInfo(&ts);
		appendBinaryStringInfo(&ts, (char *) &th, sizeof(th));
		appendBinaryStringInfo(&ts, (char *) tstride_off, (ng + 1) * sizeof(uint32));
		pad8(&ts);
		appendBinaryStringInfo(&ts, tstride.data, tstride.len);
		sift_part_add_section(secs, &nsec, SEC_GRANULE_TSTRIDE, 0, ts.data, ts.len, false);
	}
	if (excs.len > 0)
		sift_part_add_section(secs, &nsec, SEC_VARIANT_EXC, 0, excs.data, excs.len, false);
	sift_aux_begin(&aux, gdata.len + dictsec.len, part_compressible_bytes(rows, gdata.len + dictsec.len), part_inline_bytes(secs, nsec), (uint32) n, (uint32) ng, (uint32) level);
	marks = sift_f2_key_marks_build(rows, ng, gstart, &marks_len);
	if (marks != NULL && !sift_aux_marks(&aux, marks_len))
		marks = NULL;
	statsec = sift_f2_col_stats_finish(stats, nc, (SiftDictEnc *const *) dicts, ng, &stats_len);
	stats_in = sift_aux_col_stats(&aux, stats_len);
	if (SIFT_TIER_IS_COLD(tier) && sift_skip_build(rows, gstart, ng, (const SiftDictEnc *const *) dicts, promos, &aux, &skipsec))
	{
		sift_aux_charge(&aux, skipsec.len);
		sift_part_add_section(secs, &nsec, SEC_SKIP_INDEX, 0, skipsec.data, skipsec.len, false);
	}
	initStringInfo(&pointsec);
	if (level > 0 && sift_point_dir_build(rows, gstart, ng, gdata.len, &pointsec))
		sift_part_add_section(secs, &nsec, SEC_POINT, 0, pointsec.data, pointsec.len, false);
	if (rows->kll != NULL && sift_aux_kll_k(&aux, UINT16_MAX) > 0)
	{
		SiftF2KllCols kcols = *rows->kll;
		SiftF2KllBuild *kll;
		int			nsec_before = nsec;

		for (int s = 0; s < kcols.n; s++)
			kcols.k[s] = (uint16) Min(kcols.k[s], sift_aux_kll_k(&aux, kcols.k[s]));
		kll = sift_f2_kll_begin(CurrentMemoryContext, &kcols, part_gen);
		for (int s = 0; s < kcols.n; s++)
			sift_f2_kll_add_col(kll, s, &rows->cols[kcols.col[s]], rows->nrows);
		sift_f2_kll_add_section(kll, secs, &nsec);
		if (nsec > nsec_before && !sift_aux_charge(&aux, secs[nsec - 1].len))
			nsec = nsec_before;
	}
	if (marks != NULL)
		sift_part_add_section(secs, &nsec, SEC_KEY_MARKS, 0, marks, marks_len, false);
	if (stats_in)
		sift_part_add_section(secs, &nsec, SEC_COL_STATS, 0, statsec, stats_len, false);

	in.part_id = part_id;
	in.part_gen = part_gen;
	in.level = level;
	in.ncols = nc;
	in.flags = (level == 0 ? SIFT_PART_FLAG_L0_MICRO : 0) | (unfrozen ? SIFT_PART_FLAG_UNFROZEN : SIFT_PART_FLAG_ALL_VISIBLE) | (rows->nsort == 0 ? SIFT_PART_FLAG_UNKEYED : 0) | rows->part_flags |
		part_time_bounds(rows, zones, ng, tmin, tmax);
	in.nrows = n;
	in.ngranules = ng;
	in.granule_rows = SIFT_GRANULE_ROWS;
	in.min_xid = pmin;
	in.max_xid = pmax;
	in.sections = secs;
	in.nsections = nsec;
	in.inline_extent_cap = -1;
	in.bulk_tail = rows->bulk_tail;
	if (memory != NULL)
		*memory = sift_part_in_memory(rel, &in, &d);
	else
		d = sift_part_write(rel, &in);
	d.ext_flags = (marks != NULL ? SIFT_PART_EXT_KEY_MARKS : 0) | (stats_in ? SIFT_PART_EXT_COL_STATS : 0);
	memcpy(d.key_lo, kmin, SIFT_BOUND_BYTES);
	memcpy(d.key_hi, kmax, SIFT_BOUND_BYTES);
	memcpy(d.time_min, tmin, sizeof(tmin));
	memcpy(d.time_max, tmax, sizeof(tmax));
	if (codecs != NULL)
		sift_part_codecs_free(codecs);
	return d;
}

SiftPartDesc
sift_part_write_rows(Relation rel, uint32 part_id, uint64 part_gen, uint16 level, SiftCompressTier tier, const SiftRows *rows)
{
	return write_rows(rel, part_id, part_gen, level, tier, rows, NULL);
}

SiftPart *
sift_part_rows_in_memory(Relation rel, uint32 part_id, SiftCompressTier tier, const SiftRows *rows, SiftPartDesc *d)
{
	SiftPart   *part;

	*d = write_rows(rel, part_id, 0, 0, tier, rows, &part);
	return part;
}

void
sift_part_add_section(SiftSecInput *secs, int *n, uint16 tag, uint16 flags, const void *data, uint64 len, bool inl)
{
	secs[(*n)++] = (SiftSecInput) {.tag = tag,.flags = flags,.data = data,.len = len,.inline_in_header = inl};
}
