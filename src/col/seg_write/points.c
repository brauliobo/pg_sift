/*
 * col/seg_write/points.c - the point directory and the order arrays the writer builds
 * from the keys of the units to the blobs of the file
 * build, then write
 */
#include "core/membudget.h"
#include "core/rowmem.h"
#include "core/limits.h"
#include "col/seg_write/seg_write_int.h"

/* Appends len bytes of buf at the 8-aligned stream position; returns the offset */
uint64
segw_put8(SiftColSegWriter *w, const void *data, Size len)
{
	uint64		off;

	segw_sw_pad8(w->sw);
	off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, data, len);
	return off;
}

/* Whether the chunks of a global order code their addresses with the bits of their largest offset (column format minor 38, which is also the rooted point directories of 37, so a segment with only global orders keeps the plain addresses and its minor; fine chunks only, and a testing build can turn it off) */
static bool
gord_dense_ok(const SiftColSegWriter *w)
{
#ifdef SIFT_TESTING
	if (sift_test_col_gorder_unpacked)
		return false;
#endif
	return w->setting >= SIFT_COL_FORMAT_PRESENCE && sift_cgord_rows() == SIFT_CGORD_ROWS_FINE && segw_rooted_ok(w);
}

/*
 * Does the order of leaf c keep the ends of the segment only (column format minor 40): the leaf is of the build's own choice ('auto: a, b'), not text, the segment has more values than the two
 * ends hold, and a bound in the middle of its values would not read many units: sift_cgord_pay over the zones of the units, whatever key they are cut by (a range across the keys
 * reads them all, 24 times the cold blocks of the whole order at 1,000,000 rows). A leaf that has the second order alone (gorder_only: in heap order, or the units are cut by it) keeps the ends: they answer an
 * ORDER BY .. LIMIT across the units, which the zones cannot when the cut is by another column (each unit holds the whole range of the key); a range in the middle stays with the units. A segment that has such a block is of the rooted layout (segw_rooted_ok), which a minor of 37 or more is read as.
 */
static bool
gord_ends_ok(const SiftColSegWriter *w, const PointAcc *a, int c)
{
	const SiftCZone *zn = w->zones;
	int			nc = w->schema->ncols;
	uint32		nsb = w->nsb;
	int64	   *mn,
			   *mx;
	uint32	   *nv;
	SiftCGOrdZones z;
	SiftCGOrdPay pay;

	if (!w->pagg.gorderauto || w->setting < SIFT_COL_FORMAT_PRESENCE || a->n <= 2 * sift_cgord_ends() || sift_cgord_leaf_text(&w->schema->cols[c]))
		return false;
#ifdef SIFT_TESTING
	if (sift_test_col_gorder_choice != 0)
		return sift_test_col_gorder_choice == 1;
#endif
	if (!a->unordered)
		return false;
	if (w->pagg.gorder_only[c])
		return true;
	mn = palloc((Size) nsb * 2 * sizeof(int64));
	mx = mn + nsb;
	nv = palloc((Size) nsb * sizeof(uint32));
	z = (SiftCGOrdZones) {mn, mx, nv, nsb, a->n + a->nn};
	for (uint32 u = 0; u < nsb; u++)
	{
		mn[u] = zn[(Size) u * nc + c].min;
		mx[u] = zn[(Size) u * nc + c].max;
		nv[u] = zn[(Size) u * nc + c].nvals;
	}
	sift_cgord_pay(&z, &pay);
	pfree(mn);
	pfree(nv);
	return !pay.complete;
}

/* Whether the plain block built for a leaf holds every row and has no compact chunk: the entries of a grouped block may then be positions in it (SIFT_CGORD_POS) */
static bool
gord_plain_whole(const OrdBlock *ob)
{
	SiftCGOrdHdr h;

	return ob->nch > 0 && sift_cgord_head((const uint8 *) ob->tab.head.data, sift_blob_len(&ob->tab), &h) && h.vtotal == 0 && h.quant == 0 && h.gcol < 0 && h.strbytes == 0;
}

/* The entries of a leaf that keeps its rows in arrays, as the sets a block is built from (they spill past what the writer may hold) */
static void
sets_from_arrays(SiftColSegWriter *w, const PointAcc *a, bool text, SiftEntSet *vals, SiftEntSet *nulls)
{
	sift_eset_init(vals, w->scxt, !text);
	sift_eset_init(nulls, w->scxt, true);
	for (uint32 i = 0; i < a->n; i++)
		sift_eset_add(vals, a->key[i], sift_cpoint_tid_code(a->tid[i]));
	for (uint32 i = 0; i < a->nn; i++)
		sift_eset_add(nulls, 0, sift_cpoint_tid_code(a->ntid[i]));
}

/* The block of the rows kept for the global order of a leaf, sorted by key or (grouped) by group and then key; the rows with no group are not in the second */
static void
build_order(SiftColSegWriter *w, OrdBlock *ob, PointAcc *a, bool grouped, int c)
{
	bool		dense = gord_dense_ok(w);

	sift_blob_init(&ob->tab, w->scxt);
	if (grouped)
	{
		uint32		nv = 0,
					nl = 0;

		for (uint32 i = 0; i < a->n; i++)
			nv += a->gok[i] != 0;
		for (uint32 i = 0; i < a->nn; i++)
			nl += a->ngok[i] != 0;
		ob->nent = nv + nl;
		ob->nvals = nv;
		if (ob->nent > 0)
			{
				SiftCGOrdHdr h;
				SiftCGOrdPlain pl = {a->key, a->tid, a->n, a->ntid, a->nn};

				sift_cgord_build_grouped(a->key, a->tid, a->gk, a->gok, a->n, nv, a->ntid, a->ngk, a->ngok, a->nn, nl, w->pagg.gorder_group - 1, dense,
										 w->schema->cols[c].type == SIFT_COLT_NUMERIC ? SIFT_CGORD_L_WHOLE : sift_cgord_layout_pick(w->pagg.glayout[c], nv),
										 segw_gorder_plain(w, c) && gord_plain_whole(&a->ob[0]) ? &pl : NULL, &ob->tab);
				w->anycompact = (sift_cgord_head((const uint8 *) ob->tab.head.data, sift_blob_len(&ob->tab), &h) && h.quant != 0) || w->anycompact;
			}
	}
	else
	{
		bool		text = sift_cgord_leaf_text(&w->schema->cols[c]);
		SiftEntSet	tv,
					tn;
		SiftEntSet *vals = &a->vals,
				   *nulls = &a->nulls;
		SiftEntSrc	src;
		SiftCGOrdHdr h;

		if (!a->spool)
		{
			sets_from_arrays(w, a, text, &tv, &tn);
			vals = &tv;
			nulls = &tn;
		}
		ob->nent = a->n + a->nn;
		ob->nvals = a->n;
		if (text)
		{
			uint32	   *order = sift_cgord_text_order(w->dict[c], w->pagg.gorder_coll[c]);
			int64	   *rank = sift_cgord_text_ranks(order, w->dict[c]->n);

			w->anytextord = true;
			/* the dictionary is read in place by code: the setting allows the blocks form and the leaf's dictionary is not left out of it */
			a->tperm = w->setting >= SIFT_COL_FORMAT_PRESENCE && !w->inexact;
#ifdef SIFT_TESTING
			a->tperm = a->tperm && !sift_test_col_gorder_strings;
#endif
			sift_eset_seal(vals, rank);
			sift_eset_seal(nulls, NULL);
			pfree(rank);
			sift_esrc_open(&src, vals, nulls);
			sift_cgord_build_text(&src, a->n, a->nn, w->dict[c], w->pagg.gorder_coll[c], order, dense, false, &ob->tab);
			sift_esrc_close(&src);
			/*
			 * The codes replace the strings of the array where that saves a fifth of the bytes: the dictionary then goes in blocks, which can cost more than the compact form
			 * (strings that share long prefixes compress better whole), and the strings of a sorted array are front-coded, which can cost less than the dictionary they repeat.
			 * A dictionary that no longer fits the cache of parsed ones (CDICT_CODES_PCT of it) keeps the strings: each entry the codes name would unwrap a block again (an OFFSET of 20,000 over a million
			 * hashes took 67 ms against 2 ms, an equality 58 ms against 0.09 ms). A small dictionary stays in its compact form either way, and takes the codes.
			 */
			if (a->tperm && segw_dict_uncached(w, c, CDICT_CODES_PCT))
				a->tperm = false;
			if (a->tperm)
			{
				SiftBlob	codes;
				Size		compact,
							extra = w->dict[c]->n >= sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN) ? sift_cdict_blocks_extra(w->dict[c], w->pol ? &w->pol[c] : NULL, &compact) : 0;

				sift_blob_init(&codes, w->scxt);
				sift_esrc_open(&src, vals, nulls);
				sift_cgord_build_text(&src, a->n, a->nn, w->dict[c], w->pagg.gorder_coll[c], order, dense, true, &codes);
				sift_esrc_close(&src);
				if ((sift_blob_len(&codes) + extra) * 5 <= sift_blob_len(&ob->tab) * 4)
				{
					sift_blob_free(&ob->tab);
					ob->tab = codes;
				}
				else
				{
					sift_blob_free(&codes);
					a->tperm = false;
				}
			}
			pfree(order);
			w->anytextperm = a->tperm || w->anytextperm;
		}
		else
		{
			/* the build's own choice of leaves keeps the ends of the order of a segment of many rows; a list of the user's keeps all of it */
			int			layout = a->ends || w->schema->cols[c].type == SIFT_COLT_NUMERIC ? SIFT_CGORD_L_WHOLE : sift_cgord_layout_pick(w->pagg.glayout[c], a->n);

			sift_eset_seal(vals, NULL);
			sift_eset_seal(nulls, NULL);
			sift_esrc_open(&src, vals, nulls);
			sift_cgord_build(&src, a->n, a->nn, dense, a->ends, layout, &ob->tab);
			sift_esrc_close(&src);
			if (sift_cgord_head((const uint8 *) ob->tab.head.data, sift_blob_len(&ob->tab), &h))
			{
				ob->nent = h.entries;
				ob->nvals = h.values;
				w->anyends = h.vtotal != 0 || w->anyends;
				w->anycompact = h.quant != 0 || w->anycompact;
			}
		}
		sift_eset_free(vals);
		sift_eset_free(nulls);
	}
	{
		SiftCGOrdHdr h;

		w->anyseek = (sift_cgord_head((const uint8 *) ob->tab.head.data, sift_blob_len(&ob->tab), &h) && sift_cgord_has_seek((const uint8 *) ob->tab.head.data, &h)) || w->anyseek;
	}
	ob->nch = (ob->nent + sift_cgord_rows() - 1) / sift_cgord_rows();
	w->anyord = ob->nch > 0 || w->anyord;
	w->anydense = (dense && ob->nch > 0) || w->anydense;
}
#ifdef SIFT_TESTING
bool		sift_test_col_point_unpacked = false;

bool		sift_test_col_point_unrooted = false;

bool		sift_test_col_gorder_unpacked = false;
bool		sift_test_col_gorder_strings = false;

bool		sift_test_col_dict_unpacked = false;
#endif

/* Whether the header and records of the point directories end the DICT section (column format minor 37; a testing build can turn it off to write them first). A segment with only global orders
 * keeps them first, and its minor stays one that 3.6 reads, unless it keeps the ends of an order (minor 40) */
bool
segw_rooted_ok(const SiftColSegWriter *w)
{
#ifdef SIFT_TESTING
	if (sift_test_col_point_unrooted)
		return false;
#endif
	return (w->anypoints || w->wantends || w->wantord) && w->setting >= SIFT_COL_FORMAT_PRESENCE;
}

/* Whether a point directory may be written as a packed table (the setting that allows the exact slot count allows it; a testing build can turn it off to write the slots) */
static bool
packed_ok(void)
{
#ifdef SIFT_TESTING
	return !sift_test_col_point_unpacked;
#else
	return true;
#endif
}

/* The blocks of the point directories, built when the last unit is in */
void
segw_build_points(SiftColSegWriter *w)
{
	SIFT_MEM_SCOPE(col_writer_order, w->scxt);

	for (int c = 0; w->pt && c < w->schema->ncols; c++)
		w->wantord |= w->pt[c].ord && !w->pt[c].bad && w->pt[c].n + w->pt[c].nn > 0 && sift_cgord_rows() == SIFT_CGORD_ROWS_FINE;
	for (int c = 0; w->pt && c < w->schema->ncols; c++)
	{
		w->pt[c].ends = w->pt[c].ord && !w->pt[c].bad && w->pt[c].n + w->pt[c].nn > 0 && segw_gorder_plain(w, c) && gord_ends_ok(w, &w->pt[c], c);
		w->wantends |= w->pt[c].ends;
	}
	for (int c = 0; w->pt && c < w->schema->ncols; c++)
	{
		PointAcc   *a = &w->pt[c];

		if (a->ord && !a->bad && a->n + a->nn > 0)
		{
			if (segw_gorder_plain(w, c))
				build_order(w, &a->ob[0], a, false, c);
			if (segw_gorder_grouped(w, c))
				build_order(w, &a->ob[segw_gorder_plain(w, c)], a, true, c);
			w->anydual |= a->ob[1].nch > 0;
			SIFT_MEM_CHECKPOINT();
		}
		if (a->want && !a->bad && a->n > 0)
		{
			MemoryContext old = MemoryContextSwitchTo(w->pcxt);

			initStringInfo(&a->tab);
			MemoryContextSwitchTo(old);
			a->exact = w->setting >= SIFT_COL_FORMAT_PRESENCE;
			if (a->tids)
			{
				sift_cpoint_build_tids(a->key, a->tid, a->n, a->exact, a->exact && packed_ok(), &a->par, &a->tab, &a->nslots, &a->nkeys, &a->words);
				w->anytids = true;
			}
			else
			{
				sift_cpoint_build(a->key, a->row, a->n, segw_elem_point(w, c) ? Max(SIFT_CPOINT_DENSE_ROWS, (uint32) w->rows / SIFT_CPOINT_DENSE_SHARE) : 0, a->exact, a->exact && packed_ok(), &a->par, &a->tab, &a->nslots, &a->nkeys, &a->words, &a->n);
				w->anytids |= a->nslots > 0 && segw_elem_point(w, c);
			}
			w->anypoints |= a->nslots > 0;
			w->anypacked |= a->par.pbits > 0;
		}
		if (a->key)
		{
			pfree(a->key);
			if (a->row)
				pfree(a->row);
			if (a->tid)
				pfree(a->tid);
			a->key = NULL;
			a->row = NULL;
			a->tid = NULL;
		}
		if (a->ntid)
		{
			pfree(a->ntid);
			a->ntid = NULL;
		}
		if (a->gk)
		{
			pfree(a->gk);
			pfree(a->gok);
			a->gk = NULL;
			a->gok = NULL;
		}
		if (a->ngk)
		{
			pfree(a->ngk);
			pfree(a->ngok);
			a->ngk = NULL;
			a->ngok = NULL;
		}
	}
}

#define BLOB_COPY	65536

/* The bytes of a block to the stream: those before the file, the file, those after it */
static void
blob_write(SiftStreamWriter *sw, const SiftBlob *b)
{
	sift_sw_write(sw, b->head.data, b->head.len);
	if (b->file != NULL)
	{
		char	   *buf = palloc(BLOB_COPY);

		SIFT_INVARIANT(BufFileSeek(b->file, 0, 0, SEEK_SET) == 0, "sift: could not rewind a spool file");
		for (uint64 n, left = b->flen; left > 0; left -= n)
		{
			n = Min(left, BLOB_COPY);
			BufFileReadExact(b->file, buf, n);
			sift_sw_write(sw, buf, n);
		}
		pfree(buf);
	}
	sift_sw_write(sw, b->tail.data, b->tail.len);
}

/*
 * The point directories after the dictionaries, in the DICT section: a header {u32 magic, u32 version, u32 nleaves, u32 0}, a record
 * {u32 leaf, u32 nslots, u32 rows, u32 keys, u32 words, u32 0, u64 offset in the section} for each leaf, the parameters of the packed tables, the blocks, and the
 * last 16 bytes of the section {u64 offset of the first block, u32 magic, u32 version}. From column format minor 37 (setting 15 or more) the blocks come first and
 * the parameters, the records and the header end the section, so that a lookup reads them and the footer from one page.
 */
void
segw_write_points(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64		block = sift_sw_tell(w->sw) - sec->off;
	uint32		ver = w->anyord ? SIFT_CPOINT_VERSION_ORD : w->anytids ? SIFT_CPOINT_VERSION : SIFT_CPOINT_VERSION_ROWS;
	uint32		hdr[4] = {SIFT_CPOINT_MAGIC, ver, 0, 0};
	uint32		npacked = 0;
	uint64		foot[2];
	uint32		ftail[2] = {SIFT_CPOINT_MAGIC, ver};
	uint64		at;
	bool		rooted = segw_rooted_ok(w);
	StringInfoData recs,
				pars;

	if (!w->anypoints && !w->anyord)
		return;
	for (int c = 0; c < ncols; c++)
	{
		hdr[2] += w->pt[c].nslots > 0;
		hdr[3] += (w->pt[c].ob[0].nch > 0) + (w->pt[c].ob[1].nch > 0);
		npacked += w->pt[c].par.pbits > 0;
	}
	at = rooted ? block : block + sizeof(hdr) + (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN + (uint64) npacked * SIFT_CPOINT_PK_PAR;
	initStringInfo(&recs);
	initStringInfo(&pars);
	for (int c = 0; c < ncols; c++)
	{
		const PointAcc *a = &w->pt[c];
		uint32		rec[6] = {c, a->nslots, a->n, a->nkeys, a->words, (a->tids ? SIFT_CPOINT_MODE_TIDS : segw_elem_point(w, c) ? SIFT_CPOINT_MODE_ZROWS : SIFT_CPOINT_MODE_ROWS) | (a->exact ? SIFT_CPOINT_EXACT : 0) | (a->par.pbits > 0 ? SIFT_CPOINT_PACKED : 0)};

		if (a->nslots == 0)
			continue;
		appendBinaryStringInfo(&recs, (char *) rec, sizeof(rec));
		appendBinaryStringInfo(&recs, (char *) &at, sizeof(at));
		at += a->tab.len;
	}
	/* the plain blocks by leaf, then the blocks sorted by group by leaf (the slot ncols + leaf of the reader) */
	for (int k = 0; k < 2 * ncols; k++)
	{
		const OrdBlock *o = &w->pt[k % ncols].ob[k / ncols];
		uint32		rec[6] = {k, o->nch, o->nent, o->nvals, (uint32) (sift_blob_len(&o->tab) / 8), SIFT_CPOINT_MODE_ORDER};

		if (o->nch == 0)
			continue;
		appendBinaryStringInfo(&recs, (char *) rec, sizeof(rec));
		appendBinaryStringInfo(&recs, (char *) &at, sizeof(at));
		at += sift_blob_len(&o->tab);
	}
	for (int c = 0; c < ncols; c++)
		if (w->pt[c].par.pbits > 0)
			appendBinaryStringInfo(&pars, (char *) &w->pt[c].par, SIFT_CPOINT_PK_PAR);
	if (!rooted)
	{
		segw_write_mirrored(w, MIR_POINTS, hdr, sizeof(hdr));
		segw_write_mirrored(w, MIR_POINTS, recs.data, recs.len);
		segw_write_mirrored(w, MIR_POINTS, pars.data, pars.len);
	}
	for (int c = 0; c < ncols; c++)
		if (w->pt[c].nslots > 0)
			sift_sw_write(w->sw, w->pt[c].tab.data, w->pt[c].tab.len);
	for (int k = 0; k < 2 * ncols; k++)
		if (w->pt[k % ncols].ob[k / ncols].nch > 0)
		{
			segw_mirror(w, MIR_ORDER, sift_sw_tell(w->sw), w->pt[k % ncols].ob[k / ncols].tab.head.data, SIFT_CGORD_HDR);
			blob_write(w->sw, &w->pt[k % ncols].ob[k / ncols].tab);
		}
	if (rooted)
	{
		segw_write_mirrored(w, MIR_POINTS, pars.data, pars.len);
		segw_write_mirrored(w, MIR_POINTS, recs.data, recs.len);
		segw_write_mirrored(w, MIR_POINTS, hdr, sizeof(hdr));
	}
	pfree(recs.data);
	pfree(pars.data);
	foot[0] = block;
	memcpy(&foot[1], ftail, sizeof(ftail));
	segw_write_mirrored(w, MIR_POINTS, foot, sizeof(foot));
}

void
segw_drop_spool(PointAcc *a)
{
	sift_eset_free(&a->vals);
	sift_eset_free(&a->nulls);
}

void
segw_free_points(SiftColSegWriter *w)
{
	for (int c = 0; w->pt && c < w->schema->ncols; c++)
	{
		segw_drop_spool(&w->pt[c]);
		for (int k = 0; k < 2; k++)
			sift_blob_free(&w->pt[c].ob[k].tab);
	}
}
