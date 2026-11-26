/*
 * col/granule/part/write.c - a part of format major 2 written from rows: the granules, their directory, the TID index, the zones, the dictionaries of the strings and the versions.
 */
#include "core/host.h"

#include "access/transam.h"
#include "lib/stringinfo.h"

#include "col/fsst.h"
#include "col/granule/part/int.h"
#include "col/f2/aux_budget.h"
#include "col/f2/kll/kll.h"
#include "col/f2/point_dir.h"
#include "col/f2/tid/hash.h"
#include "col/f2/marks.h"
#include "col/f2/preagg/v2.h"
#include "col/skip/skip.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "table/f2/row/pack.h"

/* The most bytes of strings that a granule takes besides its first row: its record is one StringInfo, which cannot hold a gigabyte */
#define SIFT_GRANULE_BYTES_MAX	((uint64) MaxAllocSize / 4)

/* The granules of a part in the order of their directory: a part holds more than the 1 GB that a StringInfo can, so each granule is encoded into a buffer of its own and appended here */
typedef struct PartBuf
{
	char	   *data;
	uint64		len;
	uint64		cap;
} PartBuf;

static void
part_buf_append(PartBuf *b, const char *src, uint64 n)
{
	if (b->len + n > b->cap)
	{
		b->cap = Max(b->cap + b->cap / 2, b->len + n);
		b->data = b->data != NULL ? repalloc_huge(b->data, b->cap) : MemoryContextAllocHuge(CurrentMemoryContext, b->cap);
	}
	memcpy(b->data + b->len, src, n);
	b->len += n;
}

static void
zone_add_float(SiftZone *z, double v)
{
	z->flags |= SIFT_ZONE_FLOAT;
	if (isnan(v))
		z->flags |= SIFT_ZONE_HAS_NAN;
	else if (z->flags & SIFT_ZONE_NONE)
	{
		z->fmin = z->fmax = v == 0 ? 0.0 : v;
		z->flags &= ~SIFT_ZONE_NONE;
	}
	else
	{
		z->fmin = Min(z->fmin, v == 0 ? 0.0 : v);
		z->fmax = Max(z->fmax, v == 0 ? 0.0 : v);
	}
}

/* The least and the greatest of the integers of a column over nv values, the same of its floats (a NaN is no bound: it only sets SIFT_ZONE_HAS_NAN, and -0 is 0), and how many values are null */
static void
zone_of_values(SiftZone *z, const SiftColIn *col, int nv)
{
	const bool	is_float = col->kind == SIFT_CELL_F8;

	z->flags = SIFT_ZONE_NONE;
	for (int r = 0; r < nv; r++)
	{
		if (col->isnull && col->isnull[r])
			z->nnull++;
		else if (is_float)
			zone_add_float(z, col->f[r]);
		else if (col->kind != SIFT_CELL_INT)
			continue;
		else if (z->flags & SIFT_ZONE_NONE)
		{
			z->min = z->max = col->i[r];
			z->flags &= ~SIFT_ZONE_NONE;
		}
		else
		{
			z->min = Min(z->min, col->i[r]);
			z->max = Max(z->max, col->i[r]);
		}
	}
}

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

/* The bytes of the strings and of the payloads of the values of a row that the leaf cannot hold */
static uint64
row_string_bytes(const SiftRows *rows, int r)
{
	uint64		bytes = 0;

	for (int c = 0; c < rows->ncols; c++)
	{
		const SiftColIn *col = &rows->cols[c];

		if (col->eoff != NULL || col->group1 > 0)
			continue;
		bytes += (col->slen != NULL ? col->slen[r] : 0) + (col->xplen != NULL ? col->xplen[r] : 0);
	}
	return bytes;
}

int
sift_rows_granule_len(const SiftRows *rows, int from)
{
	int			to = from;
	uint64		bytes = 0;

	while (to < rows->nrows && to - from < SIFT_GRANULE_ROWS)
	{
		bool		fits = true;

		for (int c = 0; c < rows->ncols && fits; c++)
			fits = rows->cols[c].eoff == NULL || rows->cols[c].eoff[to + 1] - rows->cols[c].eoff[from] <= SIFT_GRANULE_ROWS;
		bytes += row_string_bytes(rows, to);
		if (!fits || (to > from && bytes > SIFT_GRANULE_BYTES_MAX))
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
granule_slice(const SiftRows *rows, int c, int from, const SiftDictEnc *dict)
{
	SiftColIn   col = rows->cols[c];

	if (col.dcode == NULL && col.pdict == NULL && dict != NULL && dict->row_codes != NULL)
		col.dcode = dict->row_codes;
	return sift_colin_slice(&col, col.group1 > 0 ? (int) rows->cols[col.group1 - 1].eoff[from] : from);
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
put_dicts(const SiftRows *rows, SiftCompressTier tier, SiftDictEnc **dicts, const SiftPartCodecs *pc, StringInfo sec)
{
	int			nd = pc->nentries;
	SiftDictsEntry *ent;
	uint32		hdr[2];
	SiftCcPolicy pol;
	int			k = 0;

	for (int c = 0; c < rows->ncols; c++)
		nd += dicts != NULL && dicts[c] != NULL;
	/* STOR[STOR-CODEC-01]: SEC_DICTS compression policy follows the tier of the part (L2 parts: -15..35% dictionary bytes) */
	sift_cc_policy_tier(&pol, tier, rows->compress_alg);
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

/* The rows of a sift_table that has an index are read by address (an index fetch); the rows of a table that has none, and those of an index, never are */
static bool
indexed_table(Relation rel, const SiftRows *rows)
{
	return rows->ncol_ids > 0 && rel->rd_rel->relkind != RELKIND_INDEX && rel->rd_rel->relhasindex;
}

/* The part of rows, written; with memory, kept in memory instead and written nowhere */
static SiftPartDesc
write_rows(Relation rel, uint32 part_id, uint64 part_gen, uint16 level, SiftCompressTier tier, const SiftRows *rows, SiftPart **memory)
{
	int			n = rows->nrows,
				ng = 0,
				nc = rows->ncols;
	int		   *gstart;
	PartBuf		gdata = {0};
	StringInfoData granule,
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
	SiftPreaggPlan pa_plan;
	SiftPreaggBuild *pa = NULL;
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
				pointsec,
				tidhash,
				rpacksec;
	int64		tmin[SIFT_MANIFEST_TIME_COLS] = {0},
				tmax[SIFT_MANIFEST_TIME_COLS] = {0};
	SiftColPromo *promos = NULL;
	SiftColStatsBuilder *stats;
	void	   *marks,
			   *statsec;
	uint64		marks_len,
				stats_len;
	SiftAuxBudget aux;
	bool		stats_in,
				has_tid_hash,
				has_row_pack;
	int			npromo = 0;
	SiftPartCodecs *codecs = NULL;
	MemoryContext granule_cx;

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
	tstride_off = palloc((ng + 1) * sizeof(uint32));
	initStringInfo(&granule);
	initStringInfo(&vers);
	initStringInfo(&schema);
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
			put_dicts(rows, tier, dicts, codecs, &dictsec);
	}
	if (want_preagg && sift_preagg_v2_plan(rows, (const SiftDictEnc *const *) dicts, &pa_plan))
		pa = sift_preagg_v2_begin(&pa_plan, ng);
	stats = sift_col_stats_begin((uint16) nc);
	granule_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift part granule encode");
	for (int g = 0; g < ng; g++)
	{
		int			from = gstart[g],
					cnt = gstart[g + 1] - from;
		SiftGranuleIn gi = {.nrows = cnt,.ncols = nc,.tid = rows->tid + from,.cols = gcols,.row_base = from,.compress_alg = rows->compress_alg,.tier = tier,
			.dicts = (const SiftDictEnc *const *) dicts,.part_fsst = codecs != NULL ? (const SiftFsst *const *) codecs->fsst : NULL,.part_cdict = codecs != NULL ? codecs->cdict : NULL,
			.framed = rows->ncol_ids > 0 && SIFT_TIER_IS_COLD(tier)};
		TransactionId lo = InvalidTransactionId,
					hi = InvalidTransactionId;
		bool		frozen = true;

		CHECK_FOR_INTERRUPTS();
		for (int c = 0; c < nc; c++)
			{
				gcols[c] = granule_slice(rows, c, from, dicts != NULL ? dicts[c] : NULL);
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
		/* MEM[MEM-COL-12]: codec write scratch ctx (the chunk bodies and candidates of a granule are reset with it) */
		{
			MemoryContext old = MemoryContextSwitchTo(granule_cx);

			resetStringInfo(&granule);
			dir[g] = sift_granule_encode(&granule, &gi);
			MemoryContextSwitchTo(old);
			MemoryContextReset(granule_cx);
		}
		dir[g].data_off = gdata.len;
		part_buf_append(&gdata, granule.data, granule.len);
		if (pa != NULL)
			dir[g].flags |= SIFT_GDESC_HAS_PREAGG | (sift_preagg_v2_add_granule(pa, g, gcols, cnt, (const SiftDictEnc *const *) dicts, gdata.len) ? 0 : SIFT_GDESC_PREAGG_SKIP);
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

			zone_of_values(z, &gcols[c], nv);
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
	MemoryContextDelete(granule_cx);
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
	sift_part_add_section(secs, &nsec, SEC_SCHEMA, SIFT_SEC_FLAG_REQUIRED, schema.data, schema.len, false);
	if (rows->defaults_len > 0)
		sift_part_add_section(secs, &nsec, SEC_SCHEMA_DEFAULTS, 0, rows->defaults, rows->defaults_len, false);
	sift_part_add_section(secs, &nsec, SEC_GRANULE_DIR, SIFT_SEC_FLAG_REQUIRED, dir, ng * sizeof(SiftGranuleDesc), false);
	sift_part_add_section(secs, &nsec, SEC_GRANULES, SIFT_SEC_FLAG_REQUIRED, gdata.data, gdata.len, false);
	if (dictsec.len > 0)
		sift_part_add_section(secs, &nsec, SEC_DICTS, SIFT_SEC_FLAG_REQUIRED, dictsec.data, dictsec.len, false);
	if (vers.len > 0)
		sift_part_add_section(secs, &nsec, SEC_VERS, SIFT_SEC_FLAG_REQUIRED, vers.data, vers.len, false);
	sift_part_add_section(secs, &nsec, SEC_TID_INDEX, 0, tix, ng * sizeof(SiftTidIndexEntry), false);
	if (pa != NULL)
	{
		uint64		pa_len;
		void	   *pa_sec = sift_preagg_v2_finish(pa, &pa_len);

		sift_part_add_section(secs, &nsec, SEC_PREAGG, 0, pa_sec, pa_len, false);
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
		/* STOR[STOR-CODEC-08]: raw 16 B granule tstride entries (512 B/granule uncompressed) */
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
	initStringInfo(&tidhash);
	initStringInfo(&rpacksec);
	has_row_pack = indexed_table(rel, rows) && level > 0 && rows->nrows >= SIFT_TID_HASH_MIN_ROWS && sift_row_pack_build(rows, gdata.len, &rpacksec);
	has_tid_hash = indexed_table(rel, rows) && sift_tid_hash_build(rows, level, gstart, ng, has_row_pack, &tidhash);
	has_row_pack = has_row_pack && has_tid_hash;
	if (has_tid_hash)
		sift_part_add_section(secs, &nsec, SEC_TID_HASH, 0, tidhash.data, tidhash.len, false);
	if (has_row_pack)
		sift_part_add_section(secs, &nsec, SEC_ROW_PACK, 0, rpacksec.data, rpacksec.len, false);
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
	d.ext_flags = (marks != NULL ? SIFT_PART_EXT_KEY_MARKS : 0) | (stats_in ? SIFT_PART_EXT_COL_STATS : 0) | (has_tid_hash ? SIFT_PART_EXT_TID_HASH : 0) | (has_row_pack ? SIFT_PART_EXT_ROW_PACK : 0) | (pa != NULL ? SIFT_PART_EXT_PREAGG : 0);
	memcpy(d.key_lo, kmin, SIFT_BOUND_BYTES);
	memcpy(d.key_hi, kmax, SIFT_BOUND_BYTES);
	memcpy(d.time_min, tmin, sizeof(tmin));
	memcpy(d.time_max, tmax, sizeof(tmax));
	for (int c = 0; dicts != NULL && c < nc; c++)
		if (dicts[c] != NULL && dicts[c]->row_codes != NULL && rows->cols[c].pdict == NULL)
		{
			pfree(dicts[c]->row_codes);
			dicts[c]->row_codes = NULL;
		}
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
