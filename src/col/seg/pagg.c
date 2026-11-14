/*
 * col/seg/pagg.c - the pre-aggregates of a segment: the aggregates, cubes and rollups of a
 * unit and of a superunit, the spans of the superblocks, the page masks
 * and the bytes of the sets the units keep
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/seg_int.h"

/* ---------- pre-aggregates (docs/COL-PREAGG.md) ---------- */

/* Where the sidecar starts: after the zone blocks, which are laid out back to back */
static Size
zones_end(const SiftColSegment *seg)
{
	Size		at = ALIGN8(sizeof(uint32) * 2 + seg->schema->ncols * sizeof(uint64));

	for (int c = 0; c < seg->schema->ncols; c++)
		at += zone_block_size(&seg->schema->cols[c], seg->nsb);
	return at;
}

/* Do off, then a bytes, then b bytes lie inside room bytes: the stored offsets and sizes are 64 bit, their sum may wrap */
bool
seg_within(uint64 room, uint64 off, uint64 a, uint64 b)
{
	return off <= room && a <= room - off && b <= room - off - a;
}

const PaggHeader *
seg_pagg_header(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->pagg == NULL)
	{
		const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICTIDX);
		int			ncols = seg->schema->ncols;
		int			ver = PAGG_VERSION(seg->entry.format);
		Size		end = zones_end(seg),
					hsz = PAGG_HDR_SIZE(ver, ncols);
		PaggHeader *h;

		if (end + hsz > s->len)
			corrupt(seg, "The ZONES section holds %llu bytes, no room for the pre-aggregates after %zu.", ULL(s->len), end);
		h = MemoryContextAlloc(seg->mcxt, hsz);
		seg_read(seg, s->off + end, hsz, h);
		if (h->magic != PAGG_MAGIC || h->version != ver || h->ncols != (uint32) ncols || h->nsb != seg->nsb)
			corrupt(seg, "The pre-aggregates start with 0x%08x, version %u, %u leaves, %u units; a segment of column format 1.%d has version %d.",
					h->magic, h->version, h->ncols, h->nsb, seg->entry.format, ver);
		for (int c = 0; c < ncols; c++)
			if (h->lagg_off[c] % 8 != 0 || (h->lagg_off[c] && !seg_within(s->len - end, h->lagg_off[c], lagg_block_size(seg->nsb), 0)))
				corrupt(seg, "The sums of leaf %d lie outside the section.", c);
		if (h->cubedir_off && (h->cubedir_off % 8 != 0 || !seg_within(s->len - end, h->cubedir_off, (seg->nsb + 1) * sizeof(uint64), h->cube_bytes)))
			corrupt(seg, "The cubes (%llu bytes at %llu) lie outside the section.", ULL(h->cube_bytes), ULL(h->cubedir_off));
		if (ver > 1 && PAGG_ECUBEDIR(h) &&
			(PAGG_ECUBEDIR(h) % 8 != 0 || !seg_within(s->len - end, PAGG_ECUBEDIR(h), (seg->nsb + 1) * sizeof(uint64), PAGG_ECUBE_BYTES(h))))
			corrupt(seg, "The element cubes (%llu bytes at %llu) lie outside the section.", ULL(PAGG_ECUBE_BYTES(h)), ULL(PAGG_ECUBEDIR(h)));
		if (ver > 2 && PAGG_SETS(h) &&
			(PAGG_SETS(h) % 8 != 0 || PAGG_SETS_BYTES(h) < sizeof(SetsHeader) + sizeof(SetsLeaf) || !seg_within(s->len - end, PAGG_SETS(h), PAGG_SETS_BYTES(h), 0)))
			corrupt(seg, "The small-set summaries (%llu bytes at %llu) lie outside the section.", ULL(PAGG_SETS_BYTES(h)), ULL(PAGG_SETS(h)));
		if (ver > 3 && PAGG_SUPERDIR(h) &&
			(PAGG_SUPERDIR(h) % 8 != 0 || !seg_within(s->len - end, PAGG_SUPERDIR(h), PAGG_SUPER_BYTES(h), 0) || PAGG_SUPER_BYTES(h) < sizeof(SuperHeader)))
			corrupt(seg, "The superunit block (%llu bytes at %llu) lies outside the section.", ULL(PAGG_SUPER_BYTES(h)), ULL(PAGG_SUPERDIR(h)));
		if (ver > 3 && !PAGG_SUPERDIR(h) && PAGG_SUPER_BYTES(h))
			corrupt(seg, "The superunit block has %llu bytes and no offset.", ULL(PAGG_SUPER_BYTES(h)));
		if (ver > 4 && PAGG_RUNS(h) &&
			(PAGG_RUNS(h) % 8 != 0 || PAGG_RUNS_BYTES(h) < sizeof(RunsHeader) + sizeof(RunsLeaf) || !seg_within(s->len - end, PAGG_RUNS(h), PAGG_RUNS_BYTES(h), 0)))
			corrupt(seg, "The sorted runs (%llu bytes at %llu) lie outside the section.", ULL(PAGG_RUNS_BYTES(h)), ULL(PAGG_RUNS(h)));
		if (ver > 5 && PAGG_XCUBEDIR(h) &&
			(PAGG_XCUBEDIR(h) % 8 != 0 || !seg_within(s->len - end, PAGG_XCUBEDIR(h), (seg->nsb + 1) * sizeof(uint64), PAGG_XCUBE_BYTES(h))))
			corrupt(seg, "The exception cubes (%llu bytes at %llu) lie outside the section.", ULL(PAGG_XCUBE_BYTES(h)), ULL(PAGG_XCUBEDIR(h)));
		if (ver > 5 && PAGG_XECUBEDIR(h) &&
			(PAGG_XECUBEDIR(h) % 8 != 0 || !seg_within(s->len - end, PAGG_XECUBEDIR(h), (seg->nsb + 1) * sizeof(uint64), PAGG_XECUBE_BYTES(h))))
			corrupt(seg, "The exception element blobs (%llu bytes at %llu) lie outside the section.", ULL(PAGG_XECUBE_BYTES(h)), ULL(PAGG_XECUBEDIR(h)));
		if (ver > 6 && PAGG_HLL(h) &&
			(PAGG_HLL(h) % 8 != 0 || PAGG_HLL_BYTES(h) < sizeof(SetsHeader) + sizeof(SetsLeaf) || !seg_within(s->len - end, PAGG_HLL(h), PAGG_HLL_BYTES(h), 0)))
			corrupt(seg, "The sketches (%llu bytes at %llu) lie outside the section.", ULL(PAGG_HLL_BYTES(h)), ULL(PAGG_HLL(h)));
		if (ver > 7 && PAGG_KTAB(h) &&
			(PAGG_KTAB(h) % 8 != 0 || PAGG_KTAB_BYTES(h) < 8 || !seg_within(s->len - end, PAGG_KTAB(h), PAGG_KTAB_BYTES(h), 0)))
			corrupt(seg, "The key tables (%llu bytes at %llu) lie outside the section.", ULL(PAGG_KTAB_BYTES(h)), ULL(PAGG_KTAB(h)));
		pv->pagg_end = end;
		pv->pagg = h;
		pv->lagg = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(uint64 *));
	}
	return pv->pagg;
}

void
sift_cseg_agg(SiftColSegment *seg, int col, uint32 u, SiftCAgg *a)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = seg_pagg_header(seg);
	SiftCZone	z;

	sift_cseg_zone(seg, col, u, &z);
	memset(a, 0, sizeof(*a));
	a->nvals = z.nvals;
	a->nnull = z.nnull;
	a->min = z.min;
	a->max = z.max;
	a->sum_f = z.sum_f;
	a->zflags = z.flags;
	if (h->lagg_off[col])
	{
		const uint64 *b = pv->lagg[col];

		if (b == NULL)
		{
			uint64	   *blk = MemoryContextAlloc(seg->mcxt, lagg_block_size(seg->nsb));

			seg_read(seg, seg_sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + h->lagg_off[col], lagg_block_size(seg->nsb), blk);
			pv->lagg[col] = b = blk;
		}
		a->sum = (SiftCI128) (((unsigned __int128) b[seg->nsb + u] << 64) | b[u]);
		a->sumsq = (SiftCI128) (((unsigned __int128) b[3 * seg->nsb + u] << 64) | b[2 * seg->nsb + u]);
		a->flags = ((const uint32 *) (b + 4 * seg->nsb))[u];
		a->has_sums = true;
	}
}

/* The bytes [from, to) of the blob of unit u from the directory at dir_off (n + 1 offsets, then the blobs) of the sidecar; NULL when there are none */
static const uint8 *
seg_unit_blob_part(SiftColSegment *seg, uint32 n, uint32 u, uint64 dir_off, uint64 bytes, uint64 **dir, uint8 **buf, Size *cap, uint64 from, uint64 to, uint32 *len)
{
	uint64		at = seg_sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + dir_off,
				b0,
				b1;

	*len = 0;
	if (!dir_off)
		return NULL;
	if (*dir == NULL)
	{
		*dir = MemoryContextAlloc(seg->mcxt, (n + 1) * sizeof(uint64));
		seg_read(seg, at, (n + 1) * sizeof(uint64), *dir);
		for (uint32 i = 0; i < n; i++)
			if ((*dir)[i] > (*dir)[i + 1] || (*dir)[i + 1] > bytes)
				corrupt(seg, "The cube directory is out of order at unit %u.", i);
	}
	if (from > (*dir)[u + 1] - (*dir)[u] || (to != UINT64_MAX && to > (*dir)[u + 1] - (*dir)[u]))
		corrupt(seg, "The cube of group %u is cut at %llu of %llu bytes.", u, ULL(Max(from, to)), ULL((*dir)[u + 1] - (*dir)[u]));
	b0 = (*dir)[u] + from;
	b1 = (*dir)[u] + Min(to, (*dir)[u + 1] - (*dir)[u]);
	if (b1 <= b0)
		return NULL;
	if (b1 - b0 > *cap)
	{
		*cap = Max(b1 - b0, 2 * *cap);
		SIFT_REGROW_IN(seg->mcxt, *buf, *cap);
	}
	seg_read(seg, at + (n + 1) * sizeof(uint64) + b0, b1 - b0, *buf);
	*len = b1 - b0;
	return *buf;
}

/* The blob of unit u from the directory at dir_off (nsb + 1 offsets, then the blobs) of the sidecar; NULL when the unit has none */
const uint8 *
seg_unit_blob(SiftColSegment *seg, uint32 n, uint32 u, uint64 dir_off, uint64 bytes, uint64 **dir, uint8 **buf, Size *cap, uint32 *len)
{
	return seg_unit_blob_part(seg, n, u, dir_off, bytes, dir, buf, cap, 0, UINT64_MAX, len);
}

const uint8 *
sift_cseg_cube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = seg_pagg_header(seg);

	return seg_unit_blob(seg, seg->nsb, u, h->cubedir_off, h->cube_bytes, &pv->cubedir, &pv->cubebuf, &pv->cubecap, len);
}

const uint8 *
sift_cseg_ecube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = seg_pagg_header(seg);

	if (h->version < 2)
	{
		*len = 0;
		return NULL;
	}
	return seg_unit_blob(seg, seg->nsb, u, PAGG_ECUBEDIR(h), PAGG_ECUBE_BYTES(h), &pv->ecubedir, &pv->ecubebuf, &pv->ecubecap, len);
}

/* The exception cube of unit u (docs/COL-PREAGG.md 11): the cube of the rows where the unit's exception leaf is not NULL, with the dims of the unit's own cube */
const uint8 *
sift_cseg_xcube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = seg_pagg_header(seg);

	if (h->version < 6)
	{
		*len = 0;
		return NULL;
	}
	return seg_unit_blob(seg, seg->nsb, u, PAGG_XCUBEDIR(h), PAGG_XCUBE_BYTES(h), &pv->xcubedir, &pv->xcubebuf, &pv->xcubecap, len);
}

/* The same for the element blob */
const uint8 *
sift_cseg_xecube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = seg_pagg_header(seg);

	if (h->version < 6)
	{
		*len = 0;
		return NULL;
	}
	return seg_unit_blob(seg, seg->nsb, u, PAGG_XECUBEDIR(h), PAGG_XECUBE_BYTES(h), &pv->xecubedir, &pv->xecubebuf, &pv->xecubecap, len);
}

const uint8 *
sift_cseg_rollup(SiftColSegment *seg, uint32 u, int list, uint32 *len)
{
	const uint8 *e;
	uint32		elen;

	e = sift_cseg_ecube(seg, u, &elen);
	if (e == NULL)
	{
		*len = 0;
		return NULL;
	}
	e = sift_cpagg_elem_cube(seg->schema, e, elen, list, len);
	if (*len == UINT32_MAX)
		corrupt(seg, "The rollups of unit %u do not decode.", u);
	return e;
}

const uint8 *
sift_cseg_dim_cube(SiftColSegment *seg, uint32 u, int dim, uint32 *len)
{
	int			list = seg->schema->cols[dim].parent;

	return list < 0 ? sift_cseg_cube(seg, u, len) : sift_cseg_rollup(seg, u, list, len);
}

/* The offset of the page masks in the superunit block: after the header and, for the key groups of a scope segment, the first unit and the units of each group, a copy of the fan (a fan the index asks for changes no other byte) and where the cube of the dims of the rows begins in the blob of each group */
static uint64
super_pm_off(const SuperHeader *h)
{
	return sizeof(SuperHeader) + (h->magic == SUPER_KMAGIC ? (uint64) h->nsu * 2 * sizeof(uint64) + sizeof(uint64) : 0);
}

/* The superunit block's header, checked against the segment; NULL when the segment has none */
static const SuperHeader *
super_header(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (!pv->super_checked)
	{
		const PaggHeader *ph = seg_pagg_header(seg);

		if (ph->version > 3 && PAGG_SUPERDIR(ph))
		{
			SuperHeader *h = MemoryContextAlloc(seg->mcxt, sizeof(SuperHeader));
			uint64		bytes = PAGG_SUPER_BYTES(ph),
						base = seg_sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + PAGG_SUPERDIR(ph),
						need;
			uint32		nleaf = 0;
			bool		keyed;

			seg_read(seg, base, sizeof(SuperHeader), h);
			keyed = h->magic == SUPER_KMAGIC;
			for (int c = 0; c < seg->schema->ncols; c++)
				nleaf += super_leaf_ok(&seg->schema->cols[c]);
			if ((h->magic != SUPER_MAGIC && !keyed) || h->fan < SIFT_SUPER_MIN_FAN || h->fan > SIFT_SUPER_MAX_FAN ||
				(keyed ? (seg->entry.format < SIFT_COL_FORMAT_KEYGROUPS || h->nsu == 0 || h->nsu > seg->nsb) :
				 (h->nsu != (seg->nsb + h->fan - 1) / h->fan || (h->nsu == 1 && h->fan != seg->nsb))) || h->nleaf != nleaf)
				corrupt(seg, "The superunit block starts with 0x%08x, groups of %u units, %u groups and %u leaves for %u units and %u leaves.", h->magic,
						h->fan, h->nsu, h->nleaf, seg->nsb, nleaf);
			need = super_pm_off(h) + (uint64) seg->nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64) + (uint64) nleaf * h->nsu * sizeof(SiftCSuperLeaf);
			if (h->cubedir_off == 0 ? (h->cube_bytes != 0 || bytes != need) :
				(h->cubedir_off != need || h->cube_bytes > bytes || bytes != need + (h->nsu + 1) * sizeof(uint64) + h->cube_bytes))
				corrupt(seg, "The superunit block holds %llu bytes, its sections say %llu and a cube of %llu bytes at %llu.", ULL(bytes), ULL(need),
						ULL(h->cube_bytes), ULL(h->cubedir_off));
			if (keyed)
			{
				Size		bytes_g = (Size) h->nsu * 2 * sizeof(uint32);
				uint8	   *raw = MemoryContextAlloc(seg->mcxt, bytes_g + sizeof(uint64) + (Size) h->nsu * sizeof(uint64));
				uint32	   *g = (uint32 *) raw;
				uint32		end = 0;
				const uint64 *xs = (const uint64 *) (raw + bytes_g + sizeof(uint64));

				seg_read(seg, base + sizeof(SuperHeader), bytes_g + sizeof(uint64) + (Size) h->nsu * sizeof(uint64), raw);
				if (*(uint64 *) (raw + bytes_g) != h->fan)
					corrupt(seg, "The superunit block asks for groups of %u units and repeats %llu.", h->fan, ULL(*(uint64 *) (raw + bytes_g)));
				for (uint32 k = 0; k < h->nsu; k++)
				{
					uint32		first = g[k],
								n = g[h->nsu + k];

					if (!SUPER_X_ROLLUP(xs[k]) && SUPER_X_EXTRA(xs[k]) != 0)
						corrupt(seg, "The key group %u of the superunit block has no rollup of the rows and the bytes of one.", k);

					if (n < SIFT_SUPER_MIN_FAN || n > SIFT_SUPER_MAX_FAN || (uint64) first + n > seg->nsb || (k > 0 && (first < g[k - 1] || (first == g[k - 1] && n >= g[h->nsu + k - 1]))) ||
						(k > 0 && first < end && first + n > end))
						corrupt(seg, "The key group %u of the superunit block holds %u units from unit %u of %u (the previous group is %u units from unit %u).", k, n, first, seg->nsb,
								k > 0 ? g[h->nsu + k - 1] : 0, k > 0 ? g[k - 1] : 0);
					if (k == 0 || first >= end)
						end = first + n;
				}
				pv->sgfirst = g;
				pv->sgn = g + h->nsu;
				pv->sgx = (uint64 *) (raw + bytes_g + sizeof(uint64));
			}
			pv->super = h;
			pv->sleaf = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(SiftCSuperLeaf *));
		}
		pv->super_checked = true;
	}
	return pv->super;
}

uint32
sift_cseg_super_fan(SiftColSegment *seg)
{
	const SuperHeader *h = super_header(seg);

	return h ? h->fan : 0;
}

bool
sift_cseg_super_keyed(SiftColSegment *seg)
{
	const SuperHeader *h = super_header(seg);

	return h != NULL && h->magic == SUPER_KMAGIC;
}

uint32
sift_cseg_super_count(SiftColSegment *seg)
{
	const SuperHeader *h = super_header(seg);

	return h ? h->nsu : 0;
}

void
sift_cseg_super_range(SiftColSegment *seg, uint32 k, uint32 *first, uint32 *n)
{
	const SuperHeader *h = super_header(seg);
	SegPriv    *pv = PRIV(seg);

	Assert(h != NULL && k < h->nsu);
	if (h->magic == SUPER_MAGIC)
	{
		*first = k * h->fan;
		*n = Min(h->fan, seg->nsb - *first);
	}
	else
	{
		*first = pv->sgfirst[k];
		*n = pv->sgn[k];
	}
}

/* The first group of a key-grouped segment that starts at unit u or later, by index (the groups are ascending by first unit, the larger first) */
static uint32
super_lower(SiftColSegment *seg, const SuperHeader *h, uint32 u)
{
	SegPriv    *pv = PRIV(seg);
	uint32		lo = 0,
				hi = h->nsu;

	SIFT_BISECT(uint32, mid, lo, hi, pv->sgfirst[mid] < u);
	return lo;
}

uint32
sift_cseg_super_align(SiftColSegment *seg, uint32 u)
{
	const SuperHeader *h = super_header(seg);
	SegPriv    *pv = PRIV(seg);
	uint32		end = u;

	if (h == NULL || h->magic != SUPER_KMAGIC || u >= seg->nsb)
		return u;
	for (uint32 k = super_lower(seg, h, u); k-- > 0 && pv->sgfirst[k] + SIFT_SUPER_MAX_FAN > u;)
		end = Max(end, pv->sgfirst[k] + pv->sgn[k]);
	return end;
}

bool
sift_cseg_super_at(SiftColSegment *seg, uint32 u, uint32 skip, uint32 *k)
{
	const SuperHeader *h = super_header(seg);

	if (h == NULL || u >= seg->nsb)
		return false;
	if (h->magic == SUPER_MAGIC)
	{
		*k = u / h->fan;
		return skip == 0 && u % h->fan == 0;
	}
	*k = super_lower(seg, h, u) + skip;
	return *k < h->nsu && PRIV(seg)->sgfirst[*k] == u;
}

bool
sift_cseg_super_of(SiftColSegment *seg, uint32 first, uint32 n, uint32 *k)
{
	const SuperHeader *h = super_header(seg);

	for (uint32 skip = 0; h != NULL && sift_cseg_super_at(seg, first, skip, k); skip++)
	{
		uint32		f,
					m;

		sift_cseg_super_range(seg, *k, &f, &m);
		if (m == n)
			return true;
	}
	return false;
}

void
sift_cseg_unit_pagemasks(SiftColSegment *seg, uint32 first, uint32 n, uint64 *pm)
{
	const SuperHeader *h = super_header(seg);

	if (h == NULL || n > SIFT_SUPER_MAX_FAN || (uint64) first + n > seg->nsb)
		corrupt(seg, "The page masks of %u units from unit %u were asked for.", n, first);
	seg_read(seg, seg_sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_SUPERDIR(PRIV(seg)->pagg) + super_pm_off(h) +
			 (uint64) first * SIFT_PAGEMASK_WORDS * sizeof(uint64), (Size) n * SIFT_PAGEMASK_WORDS * sizeof(uint64), pm);
}

/* The pages the rows of unit u are on, from the superunit summary (4 words), without reading the unit; false when the segment has no summary */
bool
sift_cseg_unit_pagemask(SiftColSegment *seg, uint32 u, uint64 *pm)
{
	const SuperHeader *h = super_header(seg);

	if (h == NULL || u >= seg->nsb)
		return false;
	sift_cseg_unit_pagemasks(seg, u, 1, pm);
	return true;
}

static int
sb_span_cmp(const void *a, const void *b)
{
	uint32		x = ((const SiftCSbSpan *) a)->lo,
				y = ((const SiftCSbSpan *) b)->lo;

	return x < y ? -1 : x > y;
}

uint32
sift_cseg_sb_spans(SiftColSegment *seg, const SiftCSbSpan **spans)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->spans == NULL)
	{
		SiftCSbSpan *r = MemoryContextAlloc(seg->mcxt, Max(seg->nsb, 1) * sizeof(SiftCSbSpan));
		bool		keyed = sift_cseg_keyed(&seg->entry);
		uint32		n = 0;

		for (uint32 u = 0; u < seg->nsb; u++)
		{
			uint64		pm[SIFT_PAGEMASK_WORDS];

			if (!keyed)
				r[u] = (SiftCSbSpan) {sift_cseg_unit_sb(seg, u), sift_cseg_unit_sb(seg, u)};
			else if (sift_cseg_unit_pagemask(seg, u, pm))
				r[u] = (SiftCSbSpan) {(uint32) pm[0] / SIFT_SB_PAGES, (uint32) (pm[0] >> 32) / SIFT_SB_PAGES};
			else
				r[u] = (SiftCSbSpan) {0, PG_UINT32_MAX};
		}
		if (keyed)
			qsort(r, seg->nsb, sizeof(SiftCSbSpan), sb_span_cmp);
		for (uint32 u = 0; u < seg->nsb; u++)
			if (n > 0 && r[u].lo <= (uint64) r[n - 1].hi + 1)
				r[n - 1].hi = Max(r[n - 1].hi, r[u].hi);
			else
				r[n++] = r[u];
		pv->spans = r;
		pv->nspans = n;
	}
	*spans = pv->spans;
	return pv->nspans;
}

bool
sift_cseg_sb_in_spans(SiftColSegment *seg, uint32 sb)
{
	const SiftCSbSpan *r;
	uint32		lo = 0,
				hi = sift_cseg_sb_spans(seg, &r),
				n = hi;

	SIFT_BISECT(uint32, mid, lo, hi, r[mid].hi < sb);
	return lo < n && r[lo].lo <= sb;
}

bool
sift_cseg_super_agg(SiftColSegment *seg, int col, uint32 k, SiftCAgg *a)
{
	SegPriv    *pv = PRIV(seg);
	const SuperHeader *h = super_header(seg);
	const SiftCSuperLeaf *r;
	int			slot = 0;

	if (h == NULL || k >= h->nsu || !super_leaf_ok(&seg->schema->cols[col]))
		return false;
	if (pv->sleaf[col] == NULL)
	{
		SiftCSuperLeaf *blk = MemoryContextAlloc(seg->mcxt, (Size) h->nsu * sizeof(SiftCSuperLeaf));

		for (int c = 0; c < col; c++)
			slot += super_leaf_ok(&seg->schema->cols[c]);
		seg_read(seg, seg_sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + PAGG_SUPERDIR(pv->pagg) + super_pm_off(h) +
				 (uint64) seg->nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64) + (uint64) slot * h->nsu * sizeof(SiftCSuperLeaf),
				 (Size) h->nsu * sizeof(SiftCSuperLeaf), blk);
		for (uint32 i = 0; i < h->nsu; i++)
		{
			const SiftCSuperLeaf *l = &blk[i];

			if ((l->flags & ~(SIFT_CAGG_SUMSQ_OVF | SIFT_SUPER_LEAF_OK)) || (l->zflags & ~SIFT_COLZ_CONST) ||
				((l->flags & SIFT_SUPER_LEAF_OK) ? (l->nnull > l->nvals || (l->nnull < l->nvals && l->min > l->max) ||
													((l->flags & SIFT_CAGG_SUMSQ_OVF) && (l->sumsq_lo || l->sumsq_hi)) ||
													(seg->schema->cols[col].store == SIFT_COLS_STR && (l->sum_lo || l->sum_hi || l->sumsq_lo || l->sumsq_hi || l->flags != SIFT_SUPER_LEAF_OK))) :
				 (l->flags || l->zflags || l->nvals || l->nnull || l->sum_lo || l->sum_hi)))
				corrupt(seg, "The summary of leaf %d in superunit %u is malformed.", col, i);
		}
		pv->sleaf[col] = blk;
	}
	r = &pv->sleaf[col][k];
	if (!(r->flags & SIFT_SUPER_LEAF_OK))
		return false;
	memset(a, 0, sizeof(*a));
	a->nvals = r->nvals;
	a->nnull = r->nnull;
	a->min = r->min;
	a->max = r->max;
	a->zflags = r->zflags;
	a->flags = r->flags & SIFT_CAGG_SUMSQ_OVF;
	a->sum = (SiftCI128) (((unsigned __int128) r->sum_hi << 64) | r->sum_lo);
	a->sumsq = (SiftCI128) (((unsigned __int128) r->sumsq_hi << 64) | r->sumsq_lo);
	a->has_sums = seg->schema->cols[col].store == SIFT_COLS_INT;
	return true;
}

/* The cube of group k: the blob, or for a key group its first part (the merge of the cubes of its units), then the dims of its rows and the rollup of its rows */
typedef enum SuperPart
{
	SUPER_MERGED,
	SUPER_EXTRA,
	SUPER_ROLLUP
} SuperPart;


static const uint8 *
super_blob(SiftColSegment *seg, uint32 k, SuperPart part, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const SuperHeader *h = super_header(seg);
	uint64		x = UINT64_MAX,
				xe = 0;

	*len = 0;
	if (h == NULL || h->cubedir_off == 0 || k >= h->nsu)
		return NULL;
	if (h->magic == SUPER_KMAGIC)
	{
		x = SUPER_X_MERGED(pv->sgx[k]);
		xe = SUPER_X_ROLLUP(pv->sgx[k]) ? SUPER_X_EXTRA(pv->sgx[k]) : UINT64_MAX - x;	/* no rollup: the rest of the blob is the dims of the rows */
	}
	else if (part != SUPER_MERGED)
		return NULL;
	if (part == SUPER_ROLLUP && !SUPER_X_ROLLUP(pv->sgx[k]))
		return NULL;
	return seg_unit_blob_part(seg, h->nsu, k, PAGG_SUPERDIR(pv->pagg) + h->cubedir_off, h->cube_bytes, &pv->supdir, &pv->supbuf, &pv->supcap,
							  part == SUPER_MERGED ? 0 : part == SUPER_EXTRA ? x : x + xe, part == SUPER_MERGED ? x : part == SUPER_EXTRA ? x + xe : UINT64_MAX, len);
}

const uint8 *
sift_cseg_super_cube(SiftColSegment *seg, uint32 k, uint32 *len)
{
	return super_blob(seg, k, SUPER_MERGED, len);
}

const uint8 *
sift_cseg_super_extra(SiftColSegment *seg, uint32 k, uint32 *len)
{
	return super_blob(seg, k, SUPER_EXTRA, len);
}

const uint8 *
sift_cseg_super_rollup(SiftColSegment *seg, uint32 k, uint32 *len)
{
	return super_blob(seg, k, SUPER_ROLLUP, len);
}

/* Does key group k hold the rollup of its rows (a bit of its word, no read) */
bool
sift_cseg_super_has_rollup(SiftColSegment *seg, uint32 k)
{
	const SuperHeader *h = super_header(seg);

	return h != NULL && h->magic == SUPER_KMAGIC && k < h->nsu && SUPER_X_ROLLUP(PRIV(seg)->sgx[k]);
}

void
sift_cseg_super_rollups(SiftColSegment *seg, uint32 *groups, uint32 *units)
{
	*groups = *units = 0;
	for (uint32 k = 0; k < sift_cseg_super_count(seg); k++)
		if (sift_cseg_super_has_rollup(seg, k))
		{
			(*groups)++;
			*units += PRIV(seg)->sgn[k];
		}
}

uint64
sift_cseg_set_bytes(SiftColSegment *seg)
{
	const PaggHeader *h = seg_pagg_header(seg);

	return h->version > 2 ? PAGG_SETS_BYTES(h) : 0;
}

uint64
sift_cseg_run_bytes(SiftColSegment *seg)
{
	const PaggHeader *h = seg_pagg_header(seg);

	return h->version > 4 ? PAGG_RUNS_BYTES(h) : 0;
}

uint64
sift_cseg_hll_bytes(SiftColSegment *seg)
{
	const PaggHeader *h = seg_pagg_header(seg);

	return h->version > 6 ? PAGG_HLL_BYTES(h) : 0;
}

uint64
sift_cseg_preagg_bytes(SiftColSegment *seg)
{
	seg_pagg_header(seg);
	return seg_sec(seg, SIFT_SEC_DICTIDX)->len - PRIV(seg)->pagg_end - sift_cseg_set_bytes(seg) - sift_cseg_run_bytes(seg) - sift_cseg_hll_bytes(seg);
}
