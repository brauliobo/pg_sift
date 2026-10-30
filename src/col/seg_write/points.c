/*
 * col/seg_write/points.c - the point directory and the order arrays the writer builds
 * from the keys of the units to the blobs of the file
 * build, then write
 */
#include "core/host.h"
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

/* The block of the rows kept for the global order of a leaf, sorted by key or (grouped) by group and then key; the rows with no group are not in the second */
static void
build_order(SiftColSegWriter *w, OrdBlock *ob, const PointAcc *a, bool grouped, int c)
{
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);

	initStringInfo(&ob->tab);
	MemoryContextSwitchTo(old);
	if (grouped)
	{
		/* the arrays of the entries are the point directory's too, so the others are copied */
		uint32		nv = 0,
					nl = 0;
		int64	   *key = palloc((a->n + 1) * sizeof(int64)),
				   *grp = palloc((a->n + 1) * sizeof(int64)),
				   *ngrp = palloc((a->nn + 1) * sizeof(int64));
		uint64	   *tid = palloc((a->n + 1) * sizeof(uint64)),
				   *ntid = palloc((a->nn + 1) * sizeof(uint64));

		for (uint32 i = 0; i < a->n; i++)
			if (a->gok[i])
			{
				key[nv] = a->key[i];
				tid[nv] = a->tid[i];
				grp[nv++] = a->gk[i];
			}
		for (uint32 i = 0; i < a->nn; i++)
			if (a->ngok[i])
			{
				ntid[nl] = a->ntid[i];
				ngrp[nl++] = a->ngk[i];
			}
		ob->nent = nv + nl;
		ob->nvals = nv;
		if (ob->nent > 0)
			sift_cgord_build_grouped(key, tid, grp, nv, ntid, ngrp, nl, w->pagg.gorder_group - 1, &ob->tab);
		pfree(key);
		pfree(grp);
		pfree(ngrp);
		pfree(tid);
		pfree(ntid);
	}
	else
	{
		ob->nent = a->n + a->nn;
		ob->nvals = a->n;
		if (sift_cgord_leaf_text(&w->schema->cols[c]))
		{
			w->anytextord = true;
			sift_cgord_build_text(a->key, a->tid, a->n, a->ntid, a->nn, w->dict[c], w->pagg.gorder_coll[c], &ob->tab);
		}
		else
			sift_cgord_build(a->key, a->tid, a->n, a->ntid, a->nn, &ob->tab);
	}
	ob->nch = (ob->nent + sift_cgord_rows() - 1) / sift_cgord_rows();
	w->anyord = ob->nch > 0 || w->anyord;
}
#ifdef SIFT_TESTING
bool		sift_test_col_point_unpacked = false;

bool		sift_test_col_point_unrooted = false;
#endif

/* Whether the header and records of the point directories end the DICT section (column format minor 37; a testing build can turn it off to write them first) */
bool
segw_rooted_ok(const SiftColSegWriter *w)
{
#ifdef SIFT_TESTING
	if (sift_test_col_point_unrooted)
		return false;
#endif
	return w->setting >= SIFT_COL_FORMAT_PRESENCE;
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
		}
		if (a->want && !a->bad && a->n > 0)
		{
			MemoryContext old = MemoryContextSwitchTo(w->mcxt);

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
		uint32		rec[6] = {k, o->nch, o->nent, o->nvals, (uint32) (o->tab.len / 8), SIFT_CPOINT_MODE_ORDER};

		if (o->nch == 0)
			continue;
		appendBinaryStringInfo(&recs, (char *) rec, sizeof(rec));
		appendBinaryStringInfo(&recs, (char *) &at, sizeof(at));
		at += o->tab.len;
	}
	for (int c = 0; c < ncols; c++)
		if (w->pt[c].par.pbits > 0)
			appendBinaryStringInfo(&pars, (char *) &w->pt[c].par, SIFT_CPOINT_PK_PAR);
	if (!rooted)
	{
		sift_sw_write(w->sw, hdr, sizeof(hdr));
		sift_sw_write(w->sw, recs.data, recs.len);
		sift_sw_write(w->sw, pars.data, pars.len);
	}
	for (int c = 0; c < ncols; c++)
		if (w->pt[c].nslots > 0)
			sift_sw_write(w->sw, w->pt[c].tab.data, w->pt[c].tab.len);
	for (int k = 0; k < 2 * ncols; k++)
		if (w->pt[k % ncols].ob[k / ncols].nch > 0)
			sift_sw_write(w->sw, w->pt[k % ncols].ob[k / ncols].tab.data, w->pt[k % ncols].ob[k / ncols].tab.len);
	if (rooted)
	{
		sift_sw_write(w->sw, pars.data, pars.len);
		sift_sw_write(w->sw, recs.data, recs.len);
		sift_sw_write(w->sw, hdr, sizeof(hdr));
	}
	pfree(recs.data);
	pfree(pars.data);
	foot[0] = block;
	memcpy(&foot[1], ftail, sizeof(ftail));
	sift_sw_write(w->sw, foot, sizeof(foot));
}
