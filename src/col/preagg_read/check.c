/*
 * col/preagg_read/check.c - verification of the pre-aggregates of a unit against the values
 * byte comparison of the rebuilt blob, the excepts
 * used by sift.verify
 */
#include "core/host.h"
#include "col/preagg_read/preagg_read_int.h"

static bool
same_bytes(const StringInfoData *a, const uint8 *b, uint32 len)
{
	return a->len == (int) len && (len == 0 || memcmp(a->data, b, len) == 0);
}

/* The spec that recomputes a stored cube and element blob: the dims, rollups and measures they hold, exactly */
static bool
spec_of(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, int format, MemoryContext cx, SiftCPaggSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->fixed = spec->rfixed = true;
	spec->max_groups = SIFT_PAGG_CUBE_GROUPS;
	if (len > 0)
	{
		SiftCHist	h;

		if (!sift_cpagg_decode(s, blob, len, cx, &h) || (h.dim[0].minmax && format < SIFT_COL_FORMAT_MINMAX))
			return false;
		spec->minmax = h.dim[0].minmax;
		for (int d = 0; d < h.ndims; d++, spec->ndims++)
		{
			spec->dim[spec->ndims].leaf = h.dim[d].leaf % SIFT_PAGG_CAL;
			spec->dim[spec->ndims].cal = h.dim[d].leaf >= SIFT_PAGG_CAL;
			spec->dim[spec->ndims].grain = h.dim[d].leaf / SIFT_PAGG_CAL;
			spec->dim[spec->ndims].no_minmax = !h.dim[d].minmax;
			if (spec->dim[spec->ndims].cal && format < SIFT_COL_FORMAT_CALENDAR)
				return false;
			spec->dim[spec->ndims].nmeas = h.dim[d].nmeas;
			memcpy(spec->dim[spec->ndims].meas, h.dim[d].meas, sizeof(spec->dim[0].meas));
		}
	}
	if (elen > 0)
	{
		int			nl,
					list[SIFT_PAGG_MAX_ROLLUPS];
		uint32		blen[SIFT_PAGG_MAX_ROLLUPS];
		const uint8 *p;

		if (!cpread_read_dir(s, eblob, elen, SIFT_PAGG_MAX_ROLLUPS, false, &nl, list, blen, &p))
			return false;
		for (int i = 0; i < nl; p += blen[i++])
		{
			SiftCRollup r;
			SiftCPaggRollup *ru = &spec->rollup[spec->nrollups++];

			if (!sift_cpagg_rollup_decode(s, p, blen[i], cx, &r) || r.list != list[i] || (r.list < 0 && format < SIFT_COL_FORMAT_ROWS))
				return false;
			spec->measlen = r.measlen;
			*ru = (SiftCPaggRollup) {.list = r.list, .ndims = r.ndims, .nmeas = r.nmeas};
			memcpy(ru->dim, r.dim, sizeof(ru->dim));
			memcpy(ru->meas, r.meas, sizeof(ru->meas));
		}
	}
	return true;
}

bool
sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen,
				 int format)
{
	SiftCPaggSpec spec;
	StringInfoData again,
				eagain;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	bool		ok = spec_of(s, blob, len, eblob, elen, format, cx, &spec);

	MemoryContextDelete(cx);
	if (!ok)
		return false;
	initStringInfo(&again);
	initStringInfo(&eagain);
	sift_cpagg_cube(s, &spec, cols, nrows, 0, &again, elen > 0 ? &eagain : NULL);
	ok = same_bytes(&again, blob, len) && same_bytes(&eagain, eblob, elen);
	pfree(again.data);
	pfree(eagain.data);
	return ok;
}

/* Exceptions are optional. Rebuild the stored leaf: older writers could skip an earlier candidate whose reused overflow bitmap was empty. */
bool
sift_cpagg_check_excepts(const SiftColSchema *s, const SiftCPaggCol *cols, const uint32 *const *eoff, uint32 nrows, const uint8 *blob, uint32 len,
						 const uint8 *eblob, uint32 elen, int format, const uint8 *xblob, uint32 xlen, const uint8 *xeblob, uint32 xelen)
{
	Rd			rd = {xblob, xblob ? xblob + xlen : NULL, false};
	uint64		leaf;
	SiftCPaggSpec spec;
	StringInfoData xa,
				xb;
	MemoryContext cx;
	bool		ok;

	if (xlen == 0)
		return xelen == 0;
	leaf = cpread_get_u(&rd);
	if (rd.bad || leaf >= (uint64) s->ncols)
		return false;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift preagg check", ALLOCSET_SMALL_SIZES);
	ok = spec_of(s, blob, len, eblob, elen, format, cx, &spec);
	MemoryContextDelete(cx);
	if (!ok)
		return false;
	initStringInfo(&xa);
	initStringInfo(&xb);
	sift_cpagg_excepts(s, &spec, cols, eoff, nrows, (int) leaf, blob, len, eblob, elen, &xa, &xb);
	ok = same_bytes(&xa, xblob, xlen) && same_bytes(&xb, xeblob, xelen);
	pfree(xa.data);
	pfree(xb.data);
	return ok;
}
