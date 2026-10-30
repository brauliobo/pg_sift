/*
 * col/seg_write/summary.c - the summaries of a segment: pre-aggregates, zones, versions
 * the unit aggregates, the super units
 * written after the units they summarize
 */
#include "core/host.h"
#include "core/limits.h"
#include "col/seg_write/seg_write_int.h"

/* The sums and the zone of a unit's INT leaf as the reader returns them (sift_cseg_agg) */
static void
unit_agg_of(const SiftCZone *z, const SiftCLeafAgg *la, SiftCAgg *a)
{
	memset(a, 0, sizeof(*a));
	a->nvals = z->nvals;
	a->nnull = z->nnull;
	a->min = z->min;
	a->max = z->max;
	a->zflags = z->flags;
	if (la)
	{
		a->sum = la->sum;
		a->sumsq = la->sumsq;
		a->flags = la->flags;
	}
	a->has_sums = la != NULL;
}

/* The superunit block (seg_int.h) of the units written so far, in out; false when the segment has none (not asked for, one unit) */
static bool
build_super(SiftColSegWriter *w, StringInfo out)
{
	int			ncols = w->schema->ncols;
	uint32		nsb = w->nsb,
				fan = Min(w->pagg.super, nsb),	/* one group of all the units is stored with that count: the group size is then not free to change unseen */
				nsu = fan ? (nsb + fan - 1) / fan : 0;
	SuperHeader h = {.magic = SUPER_MAGIC,.fan = fan,.nsu = nsu};
	MemoryContext cx,
				old;
	SiftCAgg   *ua;
	uint64	   *cdir;
	StringInfoData cubes;

	if (fan == 0 || nsb < 2)
		return false;
	for (int c = 0; c < ncols; c++)
		h.nleaf += super_leaf_ok(&w->schema->cols[c]);
	/* what the merge of one group allocates is freed with the group: a segment of many units has thousands of groups */
	cx = AllocSetContextCreate(w->mcxt, "sift col superunits", ALLOCSET_DEFAULT_SIZES);
	ua = palloc(fan * sizeof(SiftCAgg));
	cdir = palloc((nsu + 1) * sizeof(uint64));
	initStringInfo(&cubes);
	old = MemoryContextSwitchTo(cx);
	for (uint32 k = 0; k < nsu; k++)
	{
		uint32		n = Min(fan, nsb - k * fan);
		const uint8 **blob = palloc(n * sizeof(uint8 *));
		uint32	   *len = palloc(n * sizeof(uint32));
		bool		anycube = false;
		Size		before = cubes.len;

		cdir[k] = cubes.len;
		for (uint32 i = 0; i < n; i++)
		{
			uint32		u = k * fan + i;

			blob[i] = (const uint8 *) w->cube.data + w->cubeoff[u];
			len[i] = w->cubeoff[u + 1] - w->cubeoff[u];
			anycube |= len[i] > 0;
		}
		if (n >= 2 && anycube && !sift_cpagg_super_cube(w->schema, blob, len, n, &cubes))
			cubes.len = before;
		MemoryContextReset(cx);
	}
	cdir[nsu] = cubes.len;
	h.cubedir_off = cubes.len > 0 ? ALIGN8(sizeof(SuperHeader) + (Size) nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64) + (Size) h.nleaf * nsu * sizeof(SiftCSuperLeaf)) : 0;
	h.cube_bytes = cubes.len;
	MemoryContextSwitchTo(old);
	appendBinaryStringInfo(out, (const char *) &h, sizeof(h));
	appendBinaryStringInfo(out, (const char *) w->pmask, (Size) nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
	for (int c = 0; c < ncols; c++)
	{
		if (!super_leaf_ok(&w->schema->cols[c]))
			continue;
		for (uint32 k = 0; k < nsu; k++)
		{
			uint32		n = Min(fan, nsb - k * fan);
			bool		sums = w->schema->cols[c].store == SIFT_COLS_INT;
			SiftCSuperLeaf sl;

			for (uint32 i = 0; i < n; i++)
				unit_agg_of(&w->zones[(Size) (k * fan + i) * ncols + c], sums ? &w->lagg[(Size) (k * fan + i) * ncols + c] : NULL, &ua[i]);
			if (n < 2 || !sift_cpagg_super_leaf(ua, n, sums, &sl))
				memset(&sl, 0, sizeof(sl));
			appendBinaryStringInfo(out, (const char *) &sl, sizeof(sl));
		}
	}
	if (h.cubedir_off)
	{
		appendBinaryStringInfo(out, (const char *) cdir, (Size) (nsu + 1) * sizeof(uint64));
		appendBinaryStringInfo(out, cubes.data, cubes.len);
	}
	MemoryContextDelete(cx);
	pfree(ua);
	pfree(cdir);
	pfree(cubes.data);
	return true;
}

static void
write_pagg(SiftColSegWriter *w)
{
	int			ncols = w->schema->ncols;
	uint32		nsb = w->nsb;
	int			ver = PAGG_VERSION(w->format);
	Size		hsz = PAGG_HDR_SIZE(ver, ncols),
				at = hsz;
	PaggHeader *h = palloc0(hsz);
	SetsLeaf   *sets = NULL,
			   *runs = NULL,
			   *hlls = NULL;
	int			nsets = 0,
				nruns = 0,
				nhlls = 0;
	StringInfoData super;
	bool		has_super;

	initStringInfo(&super);
	has_super = ver > 3 && build_super(w, &super);
	h->magic = PAGG_MAGIC;
	h->version = ver;
	h->ncols = ncols;
	h->nsb = nsb;
	for (int c = 0; c < ncols; c++)
		if (w->schema->cols[c].store == SIFT_COLS_INT)
		{
			h->lagg_off[c] = at;
			at += lagg_block_size(nsb);
		}
	if (w->cube.len > 0)
	{
		h->cubedir_off = at;
		h->cube_bytes = w->cube.len;
		at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->cube.len);
	}
	if (w->ecube.len > 0)
	{
		PAGG_ECUBEDIR(h) = at;
		PAGG_ECUBE_BYTES(h) = w->ecube.len;
		at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->ecube.len);
	}
	if (ver > 2 && w->anysets)
	{
		Size		start,
					bytes;

		sets = segw_layout_blobs(w, w->set, at, &start, &bytes, &nsets);
		PAGG_SETS(h) = start;
		PAGG_SETS_BYTES(h) = bytes;
		at = start + bytes;
	}
	if (has_super)
	{
		PAGG_SUPERDIR(h) = at;
		PAGG_SUPER_BYTES(h) = super.len;
		at = ALIGN8(at + super.len);
	}
	if (ver > 4 && w->anyruns)
	{
		Size		start,
					bytes;

		runs = segw_layout_blobs(w, w->run, at, &start, &bytes, &nruns);
		PAGG_RUNS(h) = start;
		PAGG_RUNS_BYTES(h) = bytes;
		at = start + bytes;
	}
	if (ver > 5 && w->anyexc)
	{
		at = ALIGN8(at);
		PAGG_XCUBEDIR(h) = at;
		PAGG_XCUBE_BYTES(h) = w->xcube.len;
		at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->xcube.len);
		if (w->xecube.len > 0)
		{
			PAGG_XECUBEDIR(h) = at;
			PAGG_XECUBE_BYTES(h) = w->xecube.len;
			at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->xecube.len);
		}
	}
	if (ver > 6 && w->anyhll)
	{
		Size		start,
					bytes;

		hlls = segw_layout_blobs(w, w->hll, at, &start, &bytes, &nhlls);
		PAGG_HLL(h) = start;
		PAGG_HLL_BYTES(h) = bytes;
		at = start + bytes;
	}
	if (ver > 7 && w->ktab.len > 0)
	{
		PAGG_KTAB(h) = ALIGN8(at);
		PAGG_KTAB_BYTES(h) = w->ktab.len;
	}
	sift_sw_write(w->sw, h, hsz);
	for (int c = 0; c < ncols; c++)
	{
		uint64	   *blk;
		uint64	   *slo,
				   *shi,
				   *qlo,
				   *qhi;
		uint32	   *fl;

		if (!h->lagg_off[c])
			continue;
		blk = palloc0(lagg_block_size(nsb));
		slo = blk;
		shi = slo + nsb;
		qlo = shi + nsb;
		qhi = qlo + nsb;
		fl = (uint32 *) (qhi + nsb);
		for (uint32 u = 0; u < nsb; u++)
		{
			const SiftCLeafAgg *a = &w->lagg[(Size) u * ncols + c];

			slo[u] = (uint64) a->sum;
			shi[u] = (uint64) (a->sum >> 64);
			qlo[u] = (uint64) a->sumsq;
			qhi[u] = (uint64) (a->sumsq >> 64);
			fl[u] = a->flags;
		}
		sift_sw_write(w->sw, blk, lagg_block_size(nsb));
		pfree(blk);
	}
	if (w->cube.len > 0)
	{
		sift_sw_write(w->sw, w->cubeoff, (Size) (nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, w->cube.data, w->cube.len);
	}
	if (w->ecube.len > 0)
	{
		segw_sw_pad8(w->sw);
		sift_sw_write(w->sw, w->ecubeoff, (Size) (nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, w->ecube.data, w->ecube.len);
	}
	if (sets)
		segw_write_blobs(w, w->set, SETS_MAGIC, sets, nsets);
	if (has_super)
	{
		segw_sw_pad8(w->sw);
		sift_sw_write(w->sw, super.data, super.len);
	}
	pfree(super.data);
	if (runs)
		segw_write_blobs(w, w->run, RUNS_MAGIC, runs, nruns);
	if (ver > 5 && w->anyexc)
	{
		segw_sw_pad8(w->sw);
		sift_sw_write(w->sw, w->xcubeoff, (Size) (nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, w->xcube.data, w->xcube.len);
		if (w->xecube.len > 0)
		{
			segw_sw_pad8(w->sw);
			sift_sw_write(w->sw, w->xecubeoff, (Size) (nsb + 1) * sizeof(uint64));
			sift_sw_write(w->sw, w->xecube.data, w->xecube.len);
		}
	}
	if (hlls)
		segw_write_blobs(w, w->hll, HLL_MAGIC, hlls, nhlls);
	if (ver > 7 && w->ktab.len > 0)
	{
		segw_sw_pad8(w->sw);
		sift_sw_write(w->sw, w->ktab.data, w->ktab.len);
	}
	pfree(h);
}

void
segw_write_zones(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	Size		dirsz = sizeof(uint32) * 2 + ncols * sizeof(uint64),
				at = ALIGN8(dirsz);
	char	   *dir = palloc0(ALIGN8(dirsz));
	uint32	   *hdr = (uint32 *) dir;
	uint64	   *tab = (uint64 *) (dir + sizeof(uint32) * 2);

	hdr[0] = ZONES_MAGIC;
	hdr[1] = ncols;
	if (w->nsb >= sift_limit(lst_min_units, LST_MIN_UNITS))
	{
		uint32		ns = sift_lst_nsample(w->nsb);
		Size		len = sizeof(LstHeader) + ncols * sizeof(LstLeaf);
		LstHeader  *lh;

		for (int c = 0; c < ncols; c++)
			if (sift_lst_sampled(&w->schema->cols[c]))
				len += 2 * (Size) ns * sizeof(int64);
		w->lst = palloc0(len);
		w->lstlen = len;
		lh = (LstHeader *) w->lst;
		*lh = (LstHeader) {.magic = LST_MAGIC,.ncols = ncols,.nsb = w->nsb,.nsample = ns,.len = len};
	}
	for (int c = 0; c < ncols; c++)
	{
		tab[c] = at;
		at += zone_block_size(&w->schema->cols[c], w->nsb);
	}
	segw_sw_pad8(w->sw);
	sec->off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, dir, ALIGN8(dirsz));
	for (int c = 0; c < ncols; c++)
	{
		const SiftColDef *def = &w->schema->cols[c];
		Size		sz = zone_block_size(def, w->nsb);
		char	   *blk = palloc0(sz);
		uint64	   *mn = (uint64 *) blk,
				   *mx = mn + w->nsb,
				   *sum = mx + w->nsb;
		uint32	   *nnull = (uint32 *) (sum + w->nsb),
				   *nvals = nnull + w->nsb,
				   *flags = nvals + w->nsb;
		uint64	   *set = (uint64 *) (blk + ALIGN8(w->nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))));

		for (uint32 u = 0; u < w->nsb; u++)
		{
			const SiftCZone *z = &w->zones[(Size) u * ncols + c];

			mn[u] = z->min;
			mx[u] = z->max;
			if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
				memcpy(&sum[u], &z->sum_f, sizeof(double));
			else
				sum[u] = z->sum_i;
			nnull[u] = z->nnull;
			nvals[u] = z->nvals;
			/* 3.1 compares the stored flags with the chunk's exactly: the flag of 3.2 is only in a segment of the minor that says the kinds are complete */
			flags[u] = w->format >= SIFT_COL_FORMAT_KINDS ? z->flags : z->flags & ~SIFT_COLZ_JNULL;
			if (def->store == SIFT_COLS_STR)
				memcpy(set + (Size) u * 4, z->set, 4 * sizeof(uint64));
		}
		if (w->lst != NULL)
		{
			LstHeader  *lh = (LstHeader *) w->lst;
			LstLeaf    *rec = (LstLeaf *) (w->lst + sizeof(LstHeader)) + c;
			ZoneArrays	za = {.min = (const int64 *) mn,.max = (const int64 *) mx,.sum = (const int64 *) sum,.nnull = nnull,.nvals = nvals,.flags = flags};
			int64	   *lo = NULL;

			for (uint32 u = 0; u < w->nsb; u++)
				za.any_ovf |= (flags[u] & SIFT_COLZ_OVERFLOW) != 0, za.any_inl |= (flags[u] & SIFT_COLZ_INLINE) != 0;
			if (sift_lst_sampled(def))
			{
				Size		sat = sizeof(LstHeader) + ncols * sizeof(LstLeaf);

				for (int k = 0; k < c; k++)
					if (sift_lst_sampled(&w->schema->cols[k]))
						sat += 2 * (Size) lh->nsample * sizeof(int64);
				lo = (int64 *) (w->lst + sat);
			}
			sift_lst_leaf(def, w->format, w->nsb, &za, rec, lo, lo != NULL ? lo + lh->nsample : NULL);
			if (lo != NULL)
				rec->sample_off = (uint8 *) lo - w->lst;
		}
		sift_sw_write(w->sw, blk, sz);
		pfree(blk);
	}
	segw_sw_pad8(w->sw);
	write_pagg(w);
	segw_sw_pad8(w->sw);
	sec->len = sift_sw_tell(w->sw) - sec->off;
	pfree(dir);
}

/* The version section: its header, the directory of the units, the vectors */
void
segw_write_vers(SiftColSegWriter *w)
{
	VersHeader	h = {.magic = VERS_MAGIC,.version = 1,.nsb = w->nsb,.min_xid = w->vmin,.max_xid = w->vmax,.blob_bytes = w->vblob.len};

	sift_sw_write(w->sw, &h, sizeof(h));
	sift_sw_write(w->sw, w->vunit, (Size) w->nsb * sizeof(VersUnit));
	sift_sw_write(w->sw, w->vblob.data, w->vblob.len);
	segw_sw_pad8(w->sw);
}
