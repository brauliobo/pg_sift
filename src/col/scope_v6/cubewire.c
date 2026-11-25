/*
 * col/scope_v6/cubewire.c - the bytes of a cube (SiftScopeCubeHeader, then the payload).
 *
 * The payload: the time bounds, nleaf_aggs leaf aggregates, then for each dim and rollup: ngroups, overflow, the keys (delta, varint), the
 * counts, the per-measure non-NULL counts and sums (zigzag varint; the float8 sum as 8 bytes) and, when the cube keeps them, the minimum and
 * maximum of each measure of each group; a rollup first says which dims it is over. Then the distinct sets: the ids, delta, varint.
 * A cube of SIFT_SCUBE_LZ4_MIN bytes or more is stored LZ4-compressed when that makes it smaller; the header says so and holds both lengths.
 * Whatever a reader is handed is checked: the lengths, the flags, the counts and that the payload ends where it says.
 */
/* check-memory: allow-allocs the arrays follow the groups of one cube (at most SIFT_SCOPE_DIM_GROUPS a dim) and its sets, not the rows of a table */
#include "core/host.h"

#include "col/codec/codec.h"
#include "col/compress.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/wire.h"

static inline bool
is_float(const SiftScopeCubeAcc *a, uint32 m)
{
	return (a->shape.float_meas >> m) & 1;
}

static void
put_dim(StringInfo out, const SiftScopeCubeAcc *a, uint32 i)
{
	const SiftScopeDim *d = i < a->shape.ndims ? &a->dim[i] : &a->rollup[i - a->shape.ndims];
	uint32		nk = d->nk;

	sift_sdim_sort(&a->shape, (SiftScopeDim *) d);
	if (i >= a->shape.ndims)
	{
		const SiftScopeRollupDef *def = &a->shape.rollup[i - a->shape.ndims];

		wire_put_u8(out, def->nd);
		appendBinaryStringInfo(out, (char *) def->dim, def->nd);
	}
	wire_put_var(out, d->ngroups);
	wire_put_u8(out, d->overflow);
	for (uint32 g = 0; g < d->ngroups; g++)
	{
		const int64 *k = sift_rec_key(sift_dim_rec(&a->shape, d, g));
		const int64 *p = g == 0 ? NULL : sift_rec_key(sift_dim_rec(&a->shape, d, g - 1));

		for (uint32 j = 0; j < nk; j++)
			wire_put_var(out, nk == 1 ? (p == NULL ? wire_zig(k[j]) : (uint64) k[j] - (uint64) p[j]) : wire_zig((int64) ((uint64) k[j] - (p == NULL ? 0 : (uint64) p[j]))));
	}
	for (uint32 g = 0; g < d->ngroups; g++)
		wire_put_var(out, *sift_rec_cnt(sift_dim_rec(&a->shape, d, g), nk));
	for (uint32 g = 0; g < d->ngroups; g++)
	{
		const SiftScopeGroupM *gm = sift_rec_m(sift_dim_rec(&a->shape, d, g), nk);

		for (uint32 m = 0; m < a->shape.nmeas; m++)
		{
			wire_put_var(out, gm[m].nn);
			wire_put_var128(out, wire_zig128(gm[m].sum));
			if (is_float(a, m))
				appendBinaryStringInfo(out, (char *) &gm[m].sum_f, sizeof(double));
			if (a->shape.minmax)
			{
				wire_put_var(out, wire_zig(gm[m].min));
				wire_put_var(out, wire_zig(gm[m].max));
			}
		}
	}
}

static void
put_dsets(StringInfo out, const SiftScopeCubeAcc *a)
{
	wire_put_u8(out, a->shape.ndsets);
	for (uint32 d = 0; d < a->shape.ndsets; d++)
	{
		uint32		n;
		const uint32 *ids = sift_scube_dset_ids(a, d, &n);

		wire_put_var(out, n);
		for (uint32 i = 0; i < n; i++)
			wire_put_var(out, i == 0 ? ids[0] : ids[i] - ids[i - 1]);
	}
}

static void
put_payload(StringInfo out, const SiftScopeCubeAcc *a)
{
	SiftScopeShape s = a->shape;
	int64		tb[2] = {a->time_min, a->time_max};

	wire_put_u8(out, a->has_time);
	appendBinaryStringInfo(out, (char *) tb, sizeof(tb));
	for (uint32 m = 0; m < s.nmeas; m++)
	{
		const SiftScopeLeafAgg *l = &a->leaf[m];

		wire_put_u8(out, (uint8) l->zflags);
		wire_put_var(out, l->nnull);
		/* STOR[STOR-SCOPE-08]: cube leaf min/max/sum zigzag varints (float 9-10 B; raw 8 B deferred) */
		wire_put_var(out, wire_zig(l->min));
		wire_put_var(out, wire_zig(l->max));
		wire_put_var128(out, wire_zig128(l->sum));
		if (l->zflags & SIFT_SLA_FLOAT)
			appendBinaryStringInfo(out, (char *) &l->sum_f, sizeof(double));
	}
	for (uint32 i = 0; i < s.ndims + s.nrollups; i++)
		put_dim(out, a, i);
	if (s.ndsets > 0)
		put_dsets(out, a);
}

void
sift_scube_write(StringInfo out, const SiftScopeCubeAcc *a)
{
	SiftScopeCubeHeader h = {0};
	StringInfoData raw;
	const char *body;
	char	   *packed = NULL;
	Size		clen = 0;

	initStringInfo(&raw);
	put_payload(&raw, a);
	h.flags = (a->shape.minmax ? SIFT_SCUBE_MINMAX : 0) | (a->shape.ndsets > 0 ? SIFT_SCUBE_HAS_DSETS : 0) | (a->shape.nrollups > 0 ? SIFT_SCUBE_HAS_ROLLUP : 0);
	h.nleaf_aggs = a->shape.nmeas;
	h.ndims = a->shape.ndims;
	h.nrollups = a->shape.nrollups;
	h.raw_len = raw.len;
	h.row_count = a->row_count;
	if (sizeof(h) + raw.len >= SIFT_SCUBE_LZ4_MIN && sift_scope_inner_lz4())
	{
		Size		cap = sift_colz_bound(SIFT_CC_ALG_LZ4, raw.len);

		packed = palloc(cap);
		clen = sift_colz_compress(SIFT_CC_ALG_LZ4, 0, raw.data, raw.len, packed, cap);
	}
	if (clen > 0 && clen < (Size) raw.len)
	{
		h.flags |= SIFT_SCUBE_LZ4;
		h.comp_len = (uint32) clen;
		body = packed;
	}
	else
	{
		h.comp_len = raw.len;
		clen = raw.len;
		body = raw.data;
	}
	appendBinaryStringInfo(out, (char *) &h, sizeof(h));
	appendBinaryStringInfo(out, body, clen);
	if (packed != NULL)
		pfree(packed);
	pfree(raw.data);
}

static void
get_dim(WireCur *c, SiftScopeCubeAcc *a, uint32 i)
{
	SiftScopeDim *d = i < a->shape.ndims ? &a->dim[i] : &a->rollup[i - a->shape.ndims];
	uint32		ng,
				nk;

	if (i >= a->shape.ndims)
	{
		SiftScopeRollupDef *def = &a->shape.rollup[i - a->shape.ndims];

		def->nd = wire_get_u8(c);
		if (def->nd < 1 || def->nd > SIFT_SCOPE_RDIMS_CAP)
			c->bad = true;
		if (c->bad)
			return;
		wire_get_bytes(c, def->dim, def->nd);
		for (uint32 j = 0; j < def->nd; j++)
			if (def->dim[j] >= a->shape.ndims)
				c->bad = true;
		d->nk = def->nd;
	}
	nk = d->nk;
	ng = (uint32) wire_get_var(c);
	d->overflow = wire_get_u8(c);
	if (c->bad || ng > sift_dim_limit(d))
	{
		c->bad = true;
		return;
	}
	if (ng > 0)
		sift_dim_size(a, d, ng);
	for (uint32 g = 0; g < ng; g++)
	{
		int64	   *k = sift_rec_key(sift_dim_rec(&a->shape, d, g));
		const int64 *p = g == 0 ? NULL : sift_rec_key(sift_dim_rec(&a->shape, d, g - 1));

		for (uint32 j = 0; j < nk; j++)
		{
			uint64		v = wire_get_var(c);

			if (nk == 1)
				k[j] = p == NULL ? wire_unzig(v) : (int64) ((uint64) p[j] + v);
			else
				k[j] = (int64) ((p == NULL ? 0 : (uint64) p[j]) + (uint64) wire_unzig(v));
		}
	}
	for (uint32 g = 0; g < ng; g++)
		*sift_rec_cnt(sift_dim_rec(&a->shape, d, g), nk) = wire_get_var(c);
	for (uint32 g = 0; g < ng; g++)
	{
		SiftScopeGroupM *gm = sift_rec_m(sift_dim_rec(&a->shape, d, g), nk);

		for (uint32 m = 0; m < a->shape.nmeas; m++)
		{
			gm[m].nn = wire_get_var(c);
			gm[m].sum = wire_unzig128(wire_get_var128(c));
			if (is_float(a, m))
				wire_get_bytes(c, &gm[m].sum_f, sizeof(double));
			if (a->shape.minmax)
			{
				gm[m].min = wire_unzig(wire_get_var(c));
				gm[m].max = wire_unzig(wire_get_var(c));
			}
		}
		if (c->bad)
			return;
	}
}

static void
get_dsets(WireCur *c, SiftScopeCubeAcc *a)
{
	uint32		nd = wire_get_u8(c);

	if (nd > SIFT_SCOPE_DSETS_CAP)
		c->bad = true;
	if (c->bad)
		return;
	a->shape.ndsets = nd;
	for (uint32 d = 0; d < nd; d++)
	{
		uint64		n = wire_get_var(c);
		uint32		prev = 0;
		uint32	   *ids;

		if (n > (uint64) (c->end - c->p))
			c->bad = true;
		if (c->bad)
			return;
		ids = palloc(sizeof(uint32) * Max(n, 1));
		for (uint64 i = 0; i < n; i++)
		{
			uint64		v = wire_get_var(c);

			if (c->bad || (i > 0 && v == 0) || (i == 0 ? v : (uint64) prev + v) > UINT32_MAX)
			{
				c->bad = true;
				break;
			}
			ids[i] = prev = i == 0 ? (uint32) v : prev + (uint32) v;
		}
		if (!c->bad)
			sift_scube_dset_append(a, d, ids, (uint32) n);
		pfree(ids);
		if (c->bad)
			return;
	}
}

static bool
get_payload(const char *p, uint32 len, const SiftScopeCubeHeader *h, SiftScopeCubeAcc *a)
{
	WireCur		c;
	SiftScopeShape s = {0};
	int64		tb[2];

	wire_cur(&c, p, len);
	s.nmeas = h->nleaf_aggs;
	s.ndims = h->ndims;
	s.nrollups = h->nrollups;
	s.minmax = (h->flags & SIFT_SCUBE_MINMAX) != 0;
	memset(a, 0, sizeof(*a));
	a->shape = s;
	a->row_count = h->row_count;
	a->has_time = wire_get_u8(&c);
	wire_get_bytes(&c, tb, sizeof(tb));
	a->time_min = tb[0];
	a->time_max = tb[1];
	for (uint32 d = 0; d < s.ndims; d++)
		a->dim[d].nk = 1;
	for (uint32 m = 0; m < s.nmeas; m++)
	{
		SiftScopeLeafAgg *l = &a->leaf[m];

		l->zflags = wire_get_u8(&c);
		l->nnull = wire_get_var(&c);
		l->min = wire_unzig(wire_get_var(&c));
		l->max = wire_unzig(wire_get_var(&c));
		l->sum = wire_unzig128(wire_get_var128(&c));
		if (l->zflags & SIFT_SLA_FLOAT)
		{
			wire_get_bytes(&c, &l->sum_f, sizeof(double));
			a->shape.float_meas |= 1U << m;
		}
	}
	for (uint32 i = 0; i < s.ndims + s.nrollups && !c.bad; i++)
		get_dim(&c, a, i);
	if (!c.bad && (h->flags & SIFT_SCUBE_HAS_DSETS))
		get_dsets(&c, a);
	if (c.bad || c.p != c.end)
	{
		sift_scube_free(a);
		return false;
	}
	return true;
}

bool
sift_scube_read(const char *p, uint32 len, SiftScopeCubeAcc *a)
{
	SiftScopeCubeHeader h;
	bool		ok;

	if (len < sizeof(h))
		return false;
	memcpy(&h, p, sizeof(h));
	if ((h.flags & ~SIFT_SCUBE_KNOWN) || h.nleaf_aggs > SIFT_SCOPE_MEAS_CAP || h.ndims > SIFT_SCOPE_DIMS_CAP || h.nrollups > SIFT_SCOPE_ROLLUPS_CAP ||
		!!(h.flags & SIFT_SCUBE_HAS_ROLLUP) != (h.nrollups > 0) || (uint64) h.comp_len + sizeof(h) != len)
		return false;
	if (!(h.flags & SIFT_SCUBE_LZ4))
		return h.comp_len == h.raw_len && get_payload(p + sizeof(h), h.raw_len, &h, a);
	/* LZ4 cannot expand more than 255 to 1: a longer claim is a damaged header, and the buffer below is never larger than the bytes can fill */
	if (h.comp_len == 0 || h.comp_len >= h.raw_len || h.raw_len / 255 > h.comp_len || !sift_colz_available(SIFT_CC_ALG_LZ4))
		return false;
	{
		char	   *raw = palloc(h.raw_len);

		ok = sift_colz_decompress(SIFT_CC_ALG_LZ4, p + sizeof(h), h.comp_len, raw, h.raw_len) && get_payload(raw, h.raw_len, &h, a);
		pfree(raw);
	}
	return ok;
}
