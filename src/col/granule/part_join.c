/*
 * col/granule/part_join.c - parts joined as they are: the granules of several parts of one schema and dictionaries copied into one, with their zones, address index, time strides and exceptions.
 */
#include "core/host.h"

#include "access/transam.h"
#include "lib/stringinfo.h"

#include "col/granule/part_int.h"
#include "col/f2/f2_kll.h"
#include "col/f2/f2_marks.h"
#include "col/f2/preagg_delta.h"
#include "col/skip/skip_index.h"
#include "core/page_io.h"

bool
sift_parts_joinable(Relation rel, SiftPart *const *src, int n)
{
	static const uint16 same[] = {SEC_SCHEMA, SEC_DICTS};
	static const uint16 never[] = {SEC_VERS, SEC_PREAGG_SUPER, SEC_PREAGG_ROLLUP, SEC_PREAGG_EXCEPT, SEC_SETS, SEC_HLL, SEC_KTAB, SEC_RUNS, SEC_POINT, SEC_TID_HASH};
	void	   *base[lengthof(same)] = {NULL};
	uint64		blen[lengthof(same)] = {0};
	bool		ok = true;

	for (int t = 0; n > 1 && t < lengthof(same); t++)
		base[t] = sift_part_read_section(rel, src[0], same[t], &blen[t]);
	for (int i = 0; ok && i < n; i++)
	{
		if (src[i]->hdr.ncols != src[0]->hdr.ncols || (src[i]->hdr.flags & SIFT_PART_FLAG_UNFROZEN) || (sift_part_section(src[i], SEC_GRANULE_TSTRIDE) != NULL) != (sift_part_section(src[0], SEC_GRANULE_TSTRIDE) != NULL) ||
			(sift_part_section(src[i], SEC_PREAGG) != NULL) != (sift_part_section(src[0], SEC_PREAGG) != NULL) ||
			(sift_part_section(src[i], SEC_KLL) != NULL) != (sift_part_section(src[0], SEC_KLL) != NULL))
			ok = false;
		for (int t = 0; ok && t < lengthof(never); t++)
			ok = sift_part_section(src[i], never[t]) == NULL;
		for (int t = 0; ok && i > 0 && t < lengthof(same); t++)
		{
			uint64		b = 0;
			void	   *y = sift_part_read_section(rel, src[i], same[t], &b);

			ok = (base[t] == NULL) == (y == NULL) && blen[t] == b && (y == NULL || memcmp(base[t], y, b) == 0);
			if (y != NULL)
				pfree(y);
		}
	}
	for (int t = 0; t < lengthof(same); t++)
		if (base[t] != NULL)
			pfree(base[t]);
	return ok;
}

/* The bounds of the parts as one: the tier they all share and, for each time column that every one of them has, the union of the ranges */
static uint32
joined_time_bounds(const SiftPartDesc *sdesc, int n, int64 *tmin, int64 *tmax)
{
	uint32		flags = sdesc[0].flags & (SIFT_PART_FLAG_TIME_HOT | SIFT_PART_FLAG_TIME_COLD);

	for (int s = 0; s < SIFT_MANIFEST_TIME_COLS; s++)
	{
		uint32		has = s == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1;
		bool		all = true;

		tmin[s] = PG_INT64_MAX;
		tmax[s] = PG_INT64_MIN;
		for (int i = 0; i < n; i++)
		{
			all &= (sdesc[i].flags & has) != 0;
			tmin[s] = Min(tmin[s], sdesc[i].time_min[s]);
			tmax[s] = Max(tmax[s], sdesc[i].time_max[s]);
		}
		if (all)
			flags |= has;
		else
			tmin[s] = tmax[s] = 0;
	}
	return flags;
}

/* The granules of the parts as the one section they become: asked for a range of its bytes, it reads the granule that holds it from its part (one at a time) and the padding between the granules is zero */
typedef struct JoinFill
{
	Relation	rel;
	SiftPart  **src;
	const uint32 *first;		/* the first granule of each part among the granules of all, and past the last part the number of all */
	const SiftGranuleDesc *dir;
	uint32		ng;
	uint32		at;				/* the granule whose record is held */
	int			part_idx;		/* the part it is in: the granules are asked for in ascending order, so this only moves forward */
	char	   *rec;
	uint64		held_max;		/* the greatest record held at once */
} JoinFill;

static void
join_fill(void *arg, uint64 off, uint64 n, char *out)
{
	JoinFill   *f = arg;

	while (n > 0)
	{
		uint64		end;
		uint64		take;

		while (f->at + 1 < f->ng && f->dir[f->at].data_off + (uint64) TYPEALIGN(8, f->dir[f->at].data_len) <= off)
		{
			f->at++;
			if (f->rec != NULL)
			{
				pfree(f->rec);
				f->rec = NULL;
			}
		}
		end = f->dir[f->at].data_off + (uint64) TYPEALIGN(8, f->dir[f->at].data_len);
		take = Min(n, end - off);
		if (off < f->dir[f->at].data_off + f->dir[f->at].data_len)
		{
			uint64		in_rec = off - f->dir[f->at].data_off,
						real = Min(take, f->dir[f->at].data_len - in_rec);

			if (f->rec == NULL)
			{
				SiftGranuleDesc gd;

				while (f->first[f->part_idx + 1] <= f->at)
					f->part_idx++;
				f->rec = sift_part_read_granule(f->rel, f->src[f->part_idx], f->at - f->first[f->part_idx], &gd);
				f->held_max = Max(f->held_max, gd.data_len);
			}
			memcpy(out, f->rec + in_rec, real);
			memset(out + real, 0, take - real);
		}
		else
			memset(out, 0, take);
		out += take;
		off += take;
		n -= take;
	}
}

SiftPartDesc
sift_part_write_joined(Relation rel, uint32 part_id, uint64 part_gen, uint16 level, SiftPart *const *src, const SiftPartDesc *sdesc, int n, SiftF2KllBuild *kll, uint64 *granules, uint64 *writer_bytes)
{
	StringInfoData excs,
				tstrides,
				skipsec,
				pa_cells;
	uint32	   *pa_first,
			   *first;
	uint64		gtotal = 0;
	JoinFill	fill;
	uint32		ng = 0,
				nc = src[0]->hdr.ncols,
				goff = 0,
				tstride_total = 0;
	uint64		nrows = 0;
	SiftGranuleDesc *dir;
	SiftTidIndexEntry *tix;
	SiftZone   *zones;
	uint32	   *tstride_off;
	bool		has_tstride = sift_part_section(src[0], SEC_GRANULE_TSTRIDE) != NULL,
				has_preagg = sift_part_section(src[0], SEC_PREAGG) != NULL;
	FullTransactionId pmin = InvalidFullTransactionId,
				pmax = InvalidFullTransactionId;
	SiftSecInput secs[18];
	int			nsec = 0;
	SiftPartIn in = {0};
	SiftPartDesc d;
	uint64		len,
				deflen;
	uint8		ext_flags;
	void	   *schema,
			   *defaults,
			   *dicts;
	int64		tmin[SIFT_MANIFEST_TIME_COLS] = {0},
				tmax[SIFT_MANIFEST_TIME_COLS] = {0};

	for (int i = 0; i < n; i++)
		ng += src[i]->hdr.ngranules;
	dir = palloc(ng * sizeof(SiftGranuleDesc));
	tix = palloc(ng * sizeof(SiftTidIndexEntry));
	zones = palloc0(Max(nc, 1) * ng * sizeof(SiftZone));
	tstride_off = palloc((ng + 1) * sizeof(uint32));
	pa_first = palloc((ng + 1) * sizeof(uint32));
	initStringInfo(&pa_cells);
	first = palloc((n + 1) * sizeof(uint32));
	initStringInfo(&excs);
	initStringInfo(&tstrides);
	initStringInfo(&skipsec);
	for (int i = 0; i < n; i++)
	{
		const SiftPart *p = src[i];
		uint32		pg = p->hdr.ngranules;
		SiftTidIndexEntry *ptix = sift_part_read_section(rel, p, SEC_TID_INDEX, &len);
		SiftZone   *pz = sift_part_read_section(rel, p, SEC_ZONES, &len);
		SiftVariantExcSummary *pe = sift_part_read_section(rel, p, SEC_VARIANT_EXC, &len);
		uint64		nexc = pe ? len / sizeof(*pe) : 0;
		char	   *pts = has_tstride ? sift_part_read_section(rel, p, SEC_GRANULE_TSTRIDE, &len) : NULL;

		if (ptix == NULL || pz == NULL || (has_tstride && pts == NULL))
			SIFT_SEG_CORRUPT(rel, p->hdr.part_gen, "The part lacks a section a join of parts copies.");
		const SiftGranuleDesc *pdir = sift_part_granule_dir(rel, src[i]);

		first[i] = goff;
		for (uint32 g = 0; g < pg; g++)
		{
			dir[goff + g] = pdir[g];
			dir[goff + g].data_off = gtotal;
			dir[goff + g].row_base = nrows;
			gtotal += TYPEALIGN(8, pdir[g].data_len);
			nrows += pdir[g].nrows;
			tix[goff + g] = ptix[g];
			for (uint32 c = 0; c < nc; c++)
				zones[(uint64) c * ng + goff + g] = pz[(uint64) c * pg + g];
		}
		for (uint64 e = 0; e < nexc; e++)
		{
			SiftVariantExcSummary es = pe[e];

			es.granule += goff;
			appendBinaryStringInfo(&excs, (char *) &es, sizeof(es));
		}
		if (has_tstride)
		{
			const SiftTStrideHeader *h = (const SiftTStrideHeader *) pts;
			const uint32 *off = (const uint32 *) (pts + sizeof(*h));
			uint64		first = MAXALIGN(sizeof(*h) + (h->ngranules + 1) * sizeof(uint32));

			for (uint32 g = 0; g < pg; g++)
				tstride_off[goff + g] = tstride_total + off[g];
			appendBinaryStringInfo(&tstrides, pts + first, (uint64) h->total_strides * sizeof(SiftTimeStride));
			tstride_total += h->total_strides;
		}
		if (has_preagg)
		{
			char	   *ppa = sift_part_read_section(rel, p, SEC_PREAGG, &len);
			const uint32 *pf = (const uint32 *) ppa;
			uint32		base = pa_cells.len / sizeof(SiftPreaggCell),
						hdrlen = MAXALIGN((uint64) (pg + 1) * sizeof(uint32));

			if (ppa == NULL || len < hdrlen || len - hdrlen != (uint64) pf[pg] * sizeof(SiftPreaggCell))
				SIFT_SEG_CORRUPT(rel, p->hdr.part_gen, "The pre-aggregate of a part does not fit its granules.");
			for (uint32 g = 0; g < pg; g++)
				pa_first[goff + g] = base + pf[g];
			appendBinaryStringInfo(&pa_cells, ppa + hdrlen, (uint64) pf[pg] * sizeof(SiftPreaggCell));
		}
		if (!FullTransactionIdIsValid(pmin) || FullTransactionIdPrecedes(p->hdr.min_xid, pmin))
			pmin = p->hdr.min_xid;
		if (!FullTransactionIdIsValid(pmax) || FullTransactionIdFollows(p->hdr.max_xid, pmax))
			pmax = p->hdr.max_xid;
		goff += pg;
	}
	schema = sift_part_read_section(rel, src[0], SEC_SCHEMA, &len);
	sift_part_add_section(secs, &nsec, SEC_SCHEMA, SIFT_SEC_FLAG_REQUIRED, schema, len, false);
	defaults = sift_part_read_section(rel, src[0], SEC_SCHEMA_DEFAULTS, &deflen);
	if (defaults != NULL)
		sift_part_add_section(secs, &nsec, SEC_SCHEMA_DEFAULTS, 0, defaults, deflen, false);
	sift_part_add_section(secs, &nsec, SEC_GRANULE_DIR, SIFT_SEC_FLAG_REQUIRED, dir, ng * sizeof(SiftGranuleDesc), false);
	first[n] = ng;
	fill = (JoinFill) {.rel = rel,.src = (SiftPart **) src,.first = first,.dir = dir,.ng = ng};
	secs[nsec++] = (SiftSecInput) {.tag = SEC_GRANULES,.flags = SIFT_SEC_FLAG_REQUIRED,.len = gtotal,.fill = join_fill,.fill_arg = &fill};
	dicts = sift_part_read_section(rel, src[0], SEC_DICTS, &len);
	if (dicts != NULL)
		sift_part_add_section(secs, &nsec, SEC_DICTS, SIFT_SEC_FLAG_REQUIRED, dicts, len, false);
	sift_part_add_section(secs, &nsec, SEC_TID_INDEX, 0, tix, ng * sizeof(SiftTidIndexEntry), false);
	sift_part_add_section(secs, &nsec, SEC_ZONES, 0, zones, nc * ng * sizeof(SiftZone), false);
	if (has_preagg)
	{
		StringInfoData pa;

		pa_first[ng] = pa_cells.len / sizeof(SiftPreaggCell);
		initStringInfo(&pa);
		appendBinaryStringInfo(&pa, (char *) pa_first, (ng + 1) * sizeof(uint32));
		while (pa.len < MAXALIGN((uint64) (ng + 1) * sizeof(uint32)))
			appendStringInfoChar(&pa, '\0');
		appendBinaryStringInfo(&pa, pa_cells.data, pa_cells.len);
		sift_part_add_section(secs, &nsec, SEC_PREAGG, 0, pa.data, pa.len, false);
	}
	if (has_tstride)
	{
		StringInfoData ts;
		SiftTStrideHeader th = {.ngranules = ng,.total_strides = tstride_total};

		tstride_off[ng] = tstride_total;
		initStringInfo(&ts);
		appendBinaryStringInfo(&ts, (char *) &th, sizeof(th));
		appendBinaryStringInfo(&ts, (char *) tstride_off, (ng + 1) * sizeof(uint32));
		pad8(&ts);
		appendBinaryStringInfo(&ts, tstrides.data, tstrides.len);
		sift_part_add_section(secs, &nsec, SEC_GRANULE_TSTRIDE, 0, ts.data, ts.len, false);
	}
	if (excs.len > 0)
		sift_part_add_section(secs, &nsec, SEC_VARIANT_EXC, 0, excs.data, excs.len, false);
	if (sift_skip_join(rel, src, n, &skipsec))
		sift_part_add_section(secs, &nsec, SEC_SKIP_INDEX, 0, skipsec.data, skipsec.len, false);
	if (kll != NULL)
		sift_f2_kll_add_section(kll, secs, &nsec);
	sift_f2_marks_join(rel, src, n, secs, &nsec, &ext_flags);
	in.part_id = part_id;
	in.part_gen = part_gen;
	in.level = level;
	in.ncols = nc;
	in.flags = (src[0]->hdr.flags & SIFT_PART_FLAG_UNKEYED) | SIFT_PART_FLAG_ALL_VISIBLE | joined_time_bounds(sdesc, n, tmin, tmax);
	in.nrows = nrows;
	in.ngranules = ng;
	in.granule_rows = SIFT_GRANULE_ROWS;
	in.min_xid = pmin;
	in.max_xid = pmax;
	in.sections = secs;
	in.nsections = nsec;
	in.inline_extent_cap = -1;
	d = sift_part_write(rel, &in);
	d.ext_flags = ext_flags;
	if (fill.rec != NULL)
		pfree(fill.rec);
	*writer_bytes = fill.held_max + SIFT_PAGE_PAYLOAD;
	for (int i = 0; i < nsec; i++)
		*writer_bytes += secs[i].fill == NULL ? secs[i].len : 0;
	memcpy(d.key_lo, sdesc[0].key_lo, SIFT_BOUND_BYTES);
	memcpy(d.key_hi, sdesc[n - 1].key_hi, SIFT_BOUND_BYTES);
	memcpy(d.time_min, tmin, sizeof(tmin));
	memcpy(d.time_max, tmax, sizeof(tmax));
	*granules = ng;
	return d;
}
