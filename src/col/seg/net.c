/*
 * col/seg/net.c - the pre-aggregates of the dirty units of a segment net of their dead rows (docs/COL-PREAGG.md section 13).
 *
 * VACUUM clears the liveness bits of dead rows and leaves the segment as it was written, so a unit with a dead row has pre-aggregates that count rows no query sees. The
 * maintenance pass of a VACUUM writes, for each such unit, the cube, the rollups and the sums of its live rows into whole pages after the segment's own stream (the header
 * lists them as further extents, the directory entry has SIFT_SEG_NET): the last 16 bytes of the stream are a trailer that says where they start. The sidecar is valid for a
 * unit while the unit has the dead rows it was made for: liveness bits are only ever cleared, so the same number of dead rows is the same rows. A reader that finds the
 * unit with other dead rows reads it as before, and a rewrite or a merge, which drops the dead rows, drops the sidecar with the segment.
 */
#include "core/host.h"
#include "core/popcount.h"
#include "col/seg_int.h"

#define NET_MAGIC		0x5445474eU		/* "NGET" */
#define NET_TRAILER		(2 * sizeof(uint64))
#define NET_NONE		UINT32_MAX		/* NetUnit.ndead of a unit without dead rows */

typedef struct NetHeader
{
	uint32		magic;
	uint32		nsb;
	uint32		ncols;
	uint32		pad;
	uint64		units_off;		/* NetUnit[nsb] */
	uint64		agg_off;		/* NetAgg[ncols][nsb] */
	uint64		blob_off;		/* the cubes and the rollups of the units */
	uint64		len;			/* up to the end of the blobs */
} NetHeader;

/* The unit as the sidecar saw it: ndead dead rows; no blobs (len 0) when it could not be made net (a unit with exceptions, element leaves or inline strings: it scans) */
typedef struct NetUnit
{
	uint32		ndead;
	uint32		len[2];			/* the cube, the rollups */
	uint32		pad;
	uint64		off;
} NetUnit;

/* SiftCAgg as 8 byte words: the int128 sums are not aligned in a buffer */
typedef struct NetAgg
{
	uint32		nvals;
	uint32		nnull;
	int64		min;
	int64		max;
	double		sum_f;
	uint64		sum[2];
	uint64		sumsq[2];
	uint32		zflags;
	uint32		flags;
	uint32		has_sums;
	uint32		pad;
} NetAgg;

typedef struct SegNet
{
	NetHeader	h;
	NetUnit    *unit;
	NetAgg	  **agg;			/* [ncols] read on first use */
	uint8	   *buf[2];
	Size		cap[2];
	uint64		at;				/* where the sidecar starts in the stream */
} SegNet;

static const SegNet *
seg_net(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (!pv->net_read)
	{
		uint64		size = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD,
					trailer[2];

		pv->net_read = true;
		if (seg->entry.flags & SIFT_SEG_NET)
		{
			SegNet	   *n;

			if (size < NET_TRAILER)
				corrupt(seg, "The directory entry says the segment has net pre-aggregates, the stream ends at %llu.", ULL(size));
			seg_read(seg, size - NET_TRAILER, NET_TRAILER, trailer);
			n = MemoryContextAllocZero(seg->mcxt, sizeof(SegNet));
			n->at = trailer[1];
			if (trailer[0] != NET_MAGIC || n->at % SIFT_PAGE_PAYLOAD != 0 || n->at + sizeof(NetHeader) > size - NET_TRAILER)
				corrupt(seg, "The trailer of the net pre-aggregates (0x%llx, at %llu) does not fit the stream of %llu bytes.", ULL(trailer[0]), ULL(trailer[1]), ULL(size));
			seg_read(seg, n->at, sizeof(NetHeader), &n->h);
			if (n->h.magic != NET_MAGIC || n->h.nsb != seg->nsb || n->h.ncols != (uint32) seg->schema->ncols || n->at + n->h.len > size - NET_TRAILER ||
				n->h.units_off != sizeof(NetHeader) || n->h.agg_off < n->h.units_off + (uint64) seg->nsb * sizeof(NetUnit) ||
				n->h.blob_off < n->h.agg_off + (uint64) n->h.ncols * seg->nsb * sizeof(NetAgg) || n->h.len < n->h.blob_off)
				corrupt(seg, "The net pre-aggregates (0x%08x, %u units, %u leaves, %llu bytes) do not fit the segment.", n->h.magic, n->h.nsb, n->h.ncols, ULL(n->h.len));
			n->unit = MemoryContextAlloc(seg->mcxt, (Size) seg->nsb * sizeof(NetUnit));
			seg_read(seg, n->at + n->h.units_off, (Size) seg->nsb * sizeof(NetUnit), n->unit);
			n->agg = MemoryContextAllocZero(seg->mcxt, n->h.ncols * sizeof(NetAgg *));
			for (uint32 u = 0; u < seg->nsb; u++)
				if (n->unit[u].off + n->unit[u].len[0] + n->unit[u].len[1] > n->h.len - n->h.blob_off)
					corrupt(seg, "The net pre-aggregates of unit %u lie outside the sidecar.", u);
			pv->net = n;
		}
	}
	return pv->net;
}

/* Whether the unit has dead rows the sidecar does not know: VACUUM has cleared more bits since */
bool
sift_cseg_net_stale(SiftColSegment *seg, uint32 u, uint32 ndead)
{
	const SegNet *n = seg_net(seg);

	return ndead > 0 && (n == NULL || n->unit[u].ndead != ndead);
}

/* The unit has pre-aggregates net of exactly ndead dead rows */
bool
sift_cseg_net_has(SiftColSegment *seg, uint32 u, uint32 ndead)
{
	const SegNet *n = seg_net(seg);

	return n != NULL && n->unit[u].ndead == ndead && n->unit[u].len[0] + n->unit[u].len[1] > 0;
}

const uint8 *
sift_cseg_net_blob(SiftColSegment *seg, uint32 u, int kind, uint32 *len)
{
	const SegNet *n = seg_net(seg);
	SegNet	   *w = (SegNet *) n;

	*len = n->unit[u].len[kind];
	if (*len == 0)
		return NULL;
	if (*len > w->cap[kind])
	{
		w->cap[kind] = Max(*len, 2 * w->cap[kind]);
		w->buf[kind] = w->buf[kind] ? repalloc(w->buf[kind], w->cap[kind]) : MemoryContextAlloc(seg->mcxt, w->cap[kind]);
	}
	seg_read(seg, n->at + n->h.blob_off + n->unit[u].off + (kind == 1 ? n->unit[u].len[0] : 0), *len, w->buf[kind]);
	return w->buf[kind];
}

const uint8 *
sift_cseg_net_rollup(SiftColSegment *seg, uint32 u, int list, uint32 *len)
{
	uint32		elen;
	const uint8 *e = sift_cseg_net_blob(seg, u, 1, &elen);

	if (e == NULL)
	{
		*len = 0;
		return NULL;
	}
	e = sift_cpagg_elem_cube(seg->schema, e, elen, list, len);
	if (*len == UINT32_MAX)
		corrupt(seg, "The net rollups of unit %u do not decode.", u);
	return e;
}

bool
sift_cseg_net_agg(SiftColSegment *seg, int col, uint32 u, SiftCAgg *a)
{
	const SegNet *n = seg_net(seg);
	const NetAgg *x;

	if (n->agg[col] == NULL)
	{
		NetAgg	   *blk = MemoryContextAlloc(seg->mcxt, (Size) seg->nsb * sizeof(NetAgg));

		seg_read(seg, n->at + n->h.agg_off + (uint64) col * seg->nsb * sizeof(NetAgg), (Size) seg->nsb * sizeof(NetAgg), blk);
		((SegNet *) n)->agg[col] = blk;
	}
	x = &n->agg[col][u];
	if (x->nvals == UINT32_MAX)
		return false;
	memset(a, 0, sizeof(*a));
	a->nvals = x->nvals;
	a->nnull = x->nnull;
	a->min = x->min;
	a->max = x->max;
	a->sum_f = x->sum_f;
	a->zflags = x->zflags;
	a->flags = x->flags;
	a->has_sums = x->has_sums;
	a->sum = (SiftCI128) (((unsigned __int128) x->sum[1] << 64) | x->sum[0]);
	a->sumsq = (SiftCI128) (((unsigned __int128) x->sumsq[1] << 64) | x->sumsq[0]);
	return true;
}

static void
put_agg(NetAgg *x, const SiftCAgg *a)
{
	memset(x, 0, sizeof(*x));
	x->nvals = a->nvals;
	x->nnull = a->nnull;
	x->min = a->min;
	x->max = a->max;
	x->sum_f = a->sum_f;
	x->zflags = a->zflags;
	x->flags = a->flags;
	x->has_sums = a->has_sums;
	x->sum[0] = (uint64) (unsigned __int128) a->sum;
	x->sum[1] = (uint64) ((unsigned __int128) a->sum >> 64);
	x->sumsq[0] = (uint64) (unsigned __int128) a->sumsq;
	x->sumsq[1] = (uint64) ((unsigned __int128) a->sumsq >> 64);
}

/* The dead rows of unit u now, and its liveness bits (nw words) */
static uint32
unit_dead(SiftColSegment *seg, uint32 u, uint64 *live)
{
	uint32		n = seg->sbdir[u].nrows,
				alive = 0;

	if (!unit_dirty(seg, u))
		return 0;
	sift_cseg_live(seg, u, live);
	for (uint32 w = 0; w < SIFT_COL_NWORDS(n); w++)
		alive += sift_pc64(live[w]);
	return n - alive;
}

/*
 * The cube, the rollups and the sums of the live rows of unit u, as the unit would store them if it held only those. false: the unit has no cube to follow, has
 * exceptions, or a leaf whose rows are inside a list, inline or overflowed, which the pass leaves to the scan.
 */
static bool
net_unit(SiftColSegment *seg, uint32 u, const uint64 *live, uint32 ndead, StringInfo cube, StringInfo ecube, SiftCAgg *agg)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	const SiftColSchema *sc = seg->schema;
	SiftCUnitView v;
	uint32		len,
				clen,
				elen,
				xlen,
				nl = r->nrows - ndead;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	const uint8 *cb = sift_cseg_cube(seg, u, &clen);
	const uint8 *eb = sift_cseg_ecube(seg, u, &elen);
	SiftCPaggCol pcol[SIFT_COL_MAX_COLS];
	uint32	   *dead = palloc((ndead + 1) * sizeof(uint32));
	bool		ok = clen + elen > 0 && sift_cseg_xcube(seg, u, &xlen) == NULL;

	for (int c = 0; ok && c < sc->ncols; c++)
		ok = sc->cols[c].parent < 0;
	if (!ok)
		return false;
	if (!sift_cunit_parse(rec, len, sc, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, r->id);
	for (uint32 w = 0, nd = 0; nd < ndead; w++)
		for (uint64 x = ~live[w]; x != 0 && nd < ndead; x &= x - 1)
			dead[nd++] = w * 64 + pg_rightmost_one_pos64(x);
	dead[ndead] = r->nrows;
	for (int c = 0; ok && c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		SiftCChunk	ch;
		int64	   *vals;
		uint64	   *nulls;
		uint32		k = 0;
		SiftCZone	z;
		SiftCAgg	orig;

		if (!sift_cunit_chunk(&v, c, &ch))
			corrupt(seg, "Unit %u has no chunk for leaf %d.", u, c);
		if (ch.ovf != NULL || ch.n != r->nrows)
		{
			ok = false;
			break;
		}
		vals = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
		sift_cchunk_decode(&ch, vals);
		for (uint32 j = 0, prev = 0; j <= ndead; prev = dead[j++] + 1)
		{
			if (k != prev)
				memmove(vals + k, vals + prev, (dead[j] - prev) * sizeof(int64));
			k += dead[j] - prev;
		}
		nulls = NULL;
		if (ch.nulls)
		{
			uint32		kk = 0;

			nulls = palloc0((SIFT_COL_NWORDS(nl) + 1) * sizeof(uint64));
			for (uint32 i = 0; i < r->nrows; i++)
				if (live[i / 64] >> (i % 64) & 1)
				{
					if (ch.nulls[i / 64] >> (i % 64) & 1)
						nulls[kk / 64] |= UINT64CONST(1) << (kk % 64);
					kk++;
				}
			{
				bool		any_null = false;

				for (uint32 w = 0; !any_null && w < SIFT_COL_NWORDS(nl); w++)
					any_null = nulls[w] != 0;
				if (!any_null)
					nulls = NULL;	/* the loops over a leaf without a NULL test no bit */
			}
		}
		pcol[c] = (SiftCPaggCol) {vals, nulls, NULL, nl, def->store != SIFT_COLS_VAR && ch.enc != SIFT_COLENC_ISTR};
		sift_cseg_agg(seg, c, u, &orig);
		if (!pcol[c].ready)
		{
			memset(&agg[c], 0, sizeof(agg[c]));
			agg[c].nvals = UINT32_MAX;
			continue;
		}
		sift_cchunk_zone_keys(vals, nulls, NULL, nl, def->store, &z);
		pcol[c].ranged = z.nvals > z.nnull;
		pcol[c].lo = z.min;
		pcol[c].hi = z.max;
		agg[c] = (SiftCAgg) {.nvals = z.nvals,.nnull = z.nnull,.min = z.min,.max = z.max,.sum_f = z.sum_f,.zflags = z.flags | (orig.zflags & SIFT_COLZ_JNULL),
			.has_sums = orig.has_sums};
		if (orig.has_sums)
		{
			SiftCLeafAgg la;

			sift_cpagg_leaf_in(&pcol[c], z.min, z.max, &la);
			agg[c].sum = la.sum;
			agg[c].sumsq = la.sumsq;
			agg[c].flags = la.flags;
		}
	}
	return ok && sift_cpagg_cube_like(sc, pcol, nl, cb, clen, eb, elen, seg->entry.format, cube, ecube);
}

/* The units with a dead row that no net pre-aggregate answers for: the dirty units of the rewrite rule */
uint32
sift_cseg_net_uncovered(SiftColSegment *seg)
{
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	uint32		d = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		uint32		ndead = unit_dead(seg, u, live);

		d += ndead > 0 && !sift_cseg_net_has(seg, u, ndead);
	}
	pfree(live);
	return d;
}

/* Whether a dirty unit has dead rows the sidecar does not know: a new sidecar is worth its pages */
bool
sift_cseg_net_due(SiftColSegment *seg)
{
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	bool		due = false;

	for (uint32 u = 0; u < seg->nsb && !due; u++)
		due = sift_cseg_net_stale(seg, u, unit_dead(seg, u, live));
	pfree(live);
	return due;
}

/* The sidecar of the segment as it is now: a net unit for each dirty unit that can have one, the others noted with their dead rows so that they are not tried again */
StringInfo
sift_cseg_net_build(SiftColSegment *seg)
{
	const SiftColSchema *sc = seg->schema;
	uint32		nsb = seg->nsb;
	NetUnit    *unit = palloc0((Size) nsb * sizeof(NetUnit));
	NetAgg	   *agg = palloc0((Size) sc->ncols * nsb * sizeof(NetAgg));
	SiftCAgg   *ua = palloc(sc->ncols * sizeof(SiftCAgg));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col net unit", ALLOCSET_DEFAULT_SIZES);
	StringInfoData blobs,
				cube,
				ecube;
	NetHeader	h = {.magic = NET_MAGIC,.nsb = nsb,.ncols = sc->ncols,.units_off = sizeof(NetHeader)};
	StringInfo	out = makeStringInfo();

	initStringInfo(&blobs);
	initStringInfo(&cube);
	initStringInfo(&ecube);
	for (uint32 u = 0; u < nsb; u++)
	{
		uint32		ndead = unit_dead(seg, u, live);
		MemoryContext old;
		bool		ok;

		unit[u].ndead = ndead == 0 ? NET_NONE : ndead;
		for (int c = 0; c < sc->ncols; c++)
			agg[(Size) c * nsb + u].nvals = UINT32_MAX;
		if (ndead == 0)
			continue;
		resetStringInfo(&cube);
		resetStringInfo(&ecube);
		old = MemoryContextSwitchTo(cx);
		ok = net_unit(seg, u, live, ndead, &cube, &ecube, ua);
		MemoryContextSwitchTo(old);
		MemoryContextReset(cx);
		if (!ok)
			continue;
		unit[u].off = blobs.len;
		unit[u].len[0] = cube.len;
		unit[u].len[1] = ecube.len;
		appendBinaryStringInfo(&blobs, cube.data, cube.len);
		appendBinaryStringInfo(&blobs, ecube.data, ecube.len);
		for (int c = 0; c < sc->ncols; c++)
			put_agg(&agg[(Size) c * nsb + u], &ua[c]);
		CHECK_FOR_INTERRUPTS();
	}
	h.agg_off = h.units_off + (uint64) nsb * sizeof(NetUnit);
	h.blob_off = h.agg_off + (uint64) sc->ncols * nsb * sizeof(NetAgg);
	h.len = h.blob_off + blobs.len;
	appendBinaryStringInfo(out, (char *) &h, sizeof(h));
	appendBinaryStringInfo(out, (char *) unit, (Size) nsb * sizeof(NetUnit));
	appendBinaryStringInfo(out, (char *) agg, (Size) sc->ncols * nsb * sizeof(NetAgg));
	appendBinaryStringInfo(out, blobs.data, blobs.len);
	MemoryContextDelete(cx);
	pfree(unit);
	pfree(agg);
	pfree(ua);
	pfree(live);
	pfree(blobs.data);
	pfree(cube.data);
	pfree(ecube.data);
	return out;
}

/* Pads the sidecar to whole pages and ends it with the trailer (the magic, where the sidecar starts: at, a page boundary) */
void
sift_cseg_net_finish(StringInfo net, uint64 at)
{
	uint64		trailer[2] = {NET_MAGIC, at};
	Size		total = ((net->len + NET_TRAILER + SIFT_PAGE_PAYLOAD - 1) / SIFT_PAGE_PAYLOAD) * SIFT_PAGE_PAYLOAD;

	while ((Size) net->len + NET_TRAILER < total)
		appendStringInfoChar(net, '\0');
	appendBinaryStringInfo(net, (char *) trailer, NET_TRAILER);
}

/* The pages of the stream the sidecar of the segment takes (0: it has none), whose first byte is where a new one starts */
uint32
sift_cseg_net_pages(SiftColSegment *seg, uint64 *at)
{
	const SegNet *n = seg_net(seg);

	if (n == NULL)
	{
		*at = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD;
		return 0;
	}
	*at = n->at;
	return seg->sr.npages - (uint32) (n->at / SIFT_PAGE_PAYLOAD);
}

/* Every net unit is what its live rows make of it (sift.col_verify); a unit that has more dead rows than the sidecar knows was read as before and is not checked */
void
sift_cseg_net_verify(SiftColSegment *seg)
{
	const SegNet *n = seg_net(seg);
	const SiftColSchema *sc = seg->schema;
	uint64	   *live;
	SiftCAgg   *ua;
	StringInfoData cube,
				ecube;

	if (n == NULL)
		return;
	live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	ua = palloc(sc->ncols * sizeof(SiftCAgg));
	initStringInfo(&cube);
	initStringInfo(&ecube);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		uint32		ndead = unit_dead(seg, u, live),
					len;
		const uint8 *b;

		if (!sift_cseg_net_has(seg, u, ndead))
			continue;
		resetStringInfo(&cube);
		resetStringInfo(&ecube);
		if (!net_unit(seg, u, live, ndead, &cube, &ecube, ua))
			corrupt(seg, "The net pre-aggregates of unit %u are for a unit that has none.", u);
		b = sift_cseg_net_blob(seg, u, 0, &len);
		if (len != (uint32) cube.len || (len > 0 && memcmp(b, cube.data, len) != 0))
			corrupt(seg, "The net cube of unit %u differs from its live rows.", u);
		b = sift_cseg_net_blob(seg, u, 1, &len);
		if (len != (uint32) ecube.len || (len > 0 && memcmp(b, ecube.data, len) != 0))
			corrupt(seg, "The net rollups of unit %u differ from its live rows.", u);
		for (int c = 0; c < sc->ncols; c++)
		{
			SiftCAgg	have;
			NetAgg		a,
						b2;
			bool		got = sift_cseg_net_agg(seg, c, u, &have);

			if (ua[c].nvals == UINT32_MAX)
			{
				if (got)
					corrupt(seg, "The net sums of leaf %d in unit %u are for a leaf that has none.", c, u);
				continue;
			}
			put_agg(&a, &have);
			put_agg(&b2, &ua[c]);
			if (!got || memcmp(&a, &b2, sizeof(a)) != 0)
				corrupt(seg, "The net sums of leaf %d in unit %u differ from its live rows.", c, u);
		}
	}
	pfree(live);
	pfree(ua);
	pfree(cube.data);
	pfree(ecube.data);
}
