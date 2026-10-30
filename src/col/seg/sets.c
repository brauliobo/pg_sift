/*
 * col/seg/sets.c - the sets a segment keeps next to its units: the sets of values, the
 * presence bits, the HyperLogLog sketches, the sorted runs and the key
 * tables; and the pruning of units by them
 */
#include "core/host.h"
#include "col/hll.h"
#include "col/ktab.h"
#include "col/setsum.h"
#include "col/seg_int.h"

/* ---------- small-set summaries ---------- */

/*
 * The records of a section of blobs per leaf and unit (small-set summaries, sorted runs, sketches), read and checked with the whole section on first use: the
 * record of each leaf in an array of ncols (dir_off 0: the leaf has none)
 */
static SetsLeaf *
blob_records(SiftColSegment *seg, const char *what, uint32 magic, uint64 off, uint64 room, uint32 flags_ok,
			 bool (*leaf_ok) (const SiftColSchema *, const SiftColDef *))
{
	int			ncols = seg->schema->ncols;
	const SiftSectionRef *s = seg_sec(seg, SIFT_SEC_DICTIDX);
	uint64		at = s->off + PRIV(seg)->pagg_end + off,
				lowest;
	SetsHeader	sh;
	SetsLeaf   *rec,
			   *out;
	int			prev = -1;

	seg_read(seg, at, sizeof(sh), &sh);
	if (sh.magic != magic || sh.version != 1 || sh.nsb != seg->nsb || sh.nleaves < 1 || sh.nleaves > (uint32) ncols ||
		sizeof(sh) + (uint64) sh.nleaves * sizeof(SetsLeaf) > room)
		corrupt(seg, "The %s start with 0x%08x, version %u, %u leaves, %u units.", what, sh.magic, sh.version, sh.nleaves, sh.nsb);
	rec = palloc(sh.nleaves * sizeof(SetsLeaf));
	seg_read(seg, at + sizeof(sh), sh.nleaves * sizeof(SetsLeaf), rec);
	lowest = off + sizeof(sh) + sh.nleaves * sizeof(SetsLeaf);
	out = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(SetsLeaf));
	for (uint32 i = 0; i < sh.nleaves; i++)
	{
		const SetsLeaf *r = &rec[i];
		uint64		rel = r->dir_off - off;

		if (r->leaf >= (uint32) ncols || (int) r->leaf <= prev || r->flags == 0 || (r->flags & ~flags_ok) || !leaf_ok(seg->schema, &seg->schema->cols[r->leaf]))
			corrupt(seg, "The %s record %u names leaf %u, which cannot have one.", what, i, r->leaf);
		if (r->dir_off < lowest || r->dir_off % 8 != 0 ||
			!seg_within(room, rel, (uint64) (seg->nsb + 1) * sizeof(uint64), r->bytes))
			corrupt(seg, "The %s of leaf %u (%llu bytes at %llu) lie outside the section.", what, r->leaf, ULL(r->bytes), ULL(r->dir_off));
		lowest = ALIGN8(r->dir_off + (uint64) (seg->nsb + 1) * sizeof(uint64) + r->bytes);
		prev = r->leaf;
		out[r->leaf] = *r;
	}
	/* the records are all there are: one fewer would leave the last leaf's blobs unread and unnoticed */
	if (lowest != off + room)
		corrupt(seg, "The %s end at %llu, not at the %llu the section has.", what, ULL(lowest), ULL(off + room));
	pfree(rec);
	return out;
}

/* The record of leaf col in the section, read and checked with the whole section on first use; NULL when the leaf has no summary in any unit */
const SetsLeaf *
seg_set_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_SETS)
		return NULL;
	h = seg_pagg_header(seg);
	if (h->version < 3 || !PAGG_SETS(h))
		return NULL;
	if (pv->sets == NULL)
	{
		pv->sets = blob_records(seg, "small-set summaries", SETS_MAGIC, PAGG_SETS(h), PAGG_SETS_BYTES(h), SETS_EXACT | SETS_BLOOM | SETS_PRES, sift_cset_leaf_ok);
		pv->setdir = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(uint64 *));
	}
	return pv->sets[col].dir_off ? &pv->sets[col] : NULL;
}

const uint8 *
sift_cseg_set(SiftColSegment *seg, int col, uint32 u, uint32 *len)
{
	const SetsLeaf *sl = seg_set_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	const uint8 *blob;

	*len = 0;
	if (sl == NULL)
		return NULL;
	blob = seg_unit_blob(seg, seg->nsb, u, sl->dir_off, sl->bytes, &pv->setdir[col], &pv->setbuf, &pv->setcap, len);
	/* the blobs of a leaf end where its record says: a smaller end would drop the last unit's blob unnoticed */
	if (pv->setdir[col] != NULL && pv->setdir[col][seg->nsb] != sl->bytes)
		corrupt(seg, "The directory of the small-set summaries of leaf %d ends at %llu, not at %llu.", col, (unsigned long long) pv->setdir[col][seg->nsb], (unsigned long long) sl->bytes);
	return blob;
}

bool
sift_cseg_presence(SiftColSegment *seg, int col, uint32 u, SiftCPresence *p)
{
	uint32		len;
	const uint8 *blob = sift_cseg_set(seg, col, u, &len);
	SiftCSet	s;

	if (blob == NULL)
		return false;
	if (!sift_cset_open(blob, len, &s))
		corrupt(seg, "The small-set summary of leaf %d in unit %u is malformed.", col, u);
	memset(p, 0, sizeof(*p));
	if (s.kind == SIFT_CSET_PLIST || s.kind == SIFT_CSET_RUNS)
	{
		p->nkeys = s.n;
		p->keys = palloc(s.n * sizeof(int64));
		sift_cset_members(&s, p->keys);
		return true;
	}
	if (s.kind != SIFT_CSET_PRES)
		return false;
	p->wbase = s.base / 64;
	p->nwords = s.nbits / 64;
	p->words = s.data;
	return true;
}

/* ---------- HyperLogLog sketches ---------- */

static const SetsLeaf *
hll_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_HLL)
		return NULL;
	h = seg_pagg_header(seg);
	if (h->version < 7 || !PAGG_HLL(h))
		return NULL;
	if (pv->hlls == NULL)
	{
		pv->hlls = blob_records(seg, "sketches", HLL_MAGIC, PAGG_HLL(h), PAGG_HLL_BYTES(h), HLL_LIST | HLL_DENSE, sift_hll_leaf_ok);
		pv->hlldir = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(uint64 *));
	}
	return pv->hlls[col].dir_off ? &pv->hlls[col] : NULL;
}

const uint8 *
sift_cseg_hll(SiftColSegment *seg, int col, uint32 u, uint32 *len)
{
	const SetsLeaf *sl = hll_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	const uint8 *blob;

	*len = 0;
	if (sl == NULL)
		return NULL;
	blob = seg_unit_blob(seg, seg->nsb, u, sl->dir_off, sl->bytes, &pv->hlldir[col], &pv->hllbuf, &pv->hllcap, len);
	if (pv->hlldir[col][seg->nsb] != sl->bytes)
		corrupt(seg, "The directory of the sketches of leaf %d ends at %llu, not at %llu.", col, ULL(pv->hlldir[col][seg->nsb]), ULL(sl->bytes));
	if (blob != NULL && !sift_hll_check(blob, *len))
		corrupt(seg, "The sketch of leaf %d in unit %u is malformed.", col, u);
	return blob;
}

/* The key tables' bytes at *off, from the file start; false when the segment has none */
bool
seg_ktab_span(SiftColSegment *seg, uint64 *off, uint64 *len)
{
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_KTAB || (h = seg_pagg_header(seg))->version < 8 || !PAGG_KTAB(h))
		return false;
	*off = seg_sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + PAGG_KTAB(h);
	*len = PAGG_KTAB_BYTES(h);
	return true;
}

/* The key tables of the segment, read and decoded with the section on first use, kept with the segment: leaf by leaf, NULL where a leaf has none */
const SiftCHistDim *
sift_cseg_ktab(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	uint64		off,
				len;

	if (!seg_ktab_span(seg, &off, &len))
		return NULL;
	if (pv->ktdim == NULL)
	{
		uint8	   *buf = palloc(len);
		SiftCHistDim **dim = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(SiftCHistDim *));

		seg_read(seg, off, len, buf);
		for (uint64 at = 0; at < len;)
		{
			SiftCHistDim *d = MemoryContextAlloc(seg->mcxt, sizeof(SiftCHistDim));
			uint32		n = sift_ktab_decode(seg->schema, buf + at, len - at, seg->mcxt, d);

			if (n == 0 || dim[d->leaf] != NULL)
				corrupt(seg, "The key table at %llu of the section is malformed.", ULL(at));
			dim[d->leaf] = d;
			at += n;
		}
		pfree(buf);
		pv->ktdim = dim;
	}
	return pv->ktdim[col];
}

uint32
sift_cseg_hll_kinds(SiftColSegment *seg, int col)
{
	const SetsLeaf *sl = hll_leaf(seg, col);

	return sl ? sl->flags : 0;
}

/* ---------- sorted runs ---------- */

/* The record of leaf col in the section, read and checked with the whole section on first use; NULL when the leaf has no run in any unit */
const RunsLeaf *
seg_run_leaf(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h;

	if (seg->entry.format < SIFT_COL_FORMAT_RUNS)
		return NULL;
	h = seg_pagg_header(seg);
	if (h->version < 5 || !PAGG_RUNS(h))
		return NULL;
	if (pv->runs == NULL)
	{
		pv->runs = blob_records(seg, "sorted runs", RUNS_MAGIC, PAGG_RUNS(h), PAGG_RUNS_BYTES(h), RUNS_ORD | RUNS_IDENT, sift_crun_leaf_ok);
		pv->rundir = MemoryContextAllocZero(seg->mcxt, seg->schema->ncols * sizeof(uint64 *));
	}
	return pv->runs[col].dir_off ? &pv->runs[col] : NULL;
}

bool
sift_cseg_run(SiftColSegment *seg, int col, uint32 u, SiftCRun *r, uint64 *at)
{
	const RunsLeaf *rl = seg_run_leaf(seg, col);
	SegPriv    *pv = PRIV(seg);
	uint64		base,
				b0,
				b1;
	uint8		hdr[SIFT_CRUN_HDR];

	if (rl == NULL)
		return false;
	base = seg_sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + rl->dir_off;
	if (pv->rundir[col] == NULL)
	{
		uint64	   *dir = MemoryContextAlloc(seg->mcxt, (seg->nsb + 1) * sizeof(uint64));

		seg_read(seg, base, (seg->nsb + 1) * sizeof(uint64), dir);
		for (uint32 i = 0; i < seg->nsb; i++)
			if (dir[i] > dir[i + 1] || dir[i + 1] > rl->bytes)
				corrupt(seg, "The sorted-run directory of leaf %d is out of order at unit %u.", col, i);
		pv->rundir[col] = dir;
	}
	b0 = pv->rundir[col][u];
	b1 = pv->rundir[col][u + 1];
	if (b1 == b0)
		return false;
	*at = base + (seg->nsb + 1) * sizeof(uint64) + b0;
	if (b1 - b0 > UINT32_MAX)
		corrupt(seg, "The sorted run of leaf %d in unit %u is %llu bytes.", col, u, ULL(b1 - b0));
	seg_read(seg, *at, SIFT_CRUN_HDR < b1 - b0 ? SIFT_CRUN_HDR : b1 - b0, hdr);
	if (!sift_crun_head(hdr, (uint32) (b1 - b0), seg->sbdir[u].nrows, r))
		corrupt(seg, "The sorted run of leaf %d in unit %u is malformed.", col, u);
	return true;
}

uint32
sift_cseg_set_kinds(SiftColSegment *seg, int col)
{
	const SetsLeaf *sl = seg_set_leaf(seg, col);

	return sl ? sl->flags : 0;
}

uint32
sift_cseg_run_kinds(SiftColSegment *seg, int col)
{
	const RunsLeaf *rl = seg_run_leaf(seg, col);

	return rl ? rl->flags : 0;
}

void
sift_cseg_run_blob(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint8 *dst)
{
	seg_read(seg, at, sift_crun_total(r), dst);
}

/* The head of the run (r->heads > 0): the entries nlo from the low end, then nhi from the high end, the last of the run first */
void
sift_cseg_run_heads(SiftColSegment *seg, uint64 at, const SiftCRun *r, SiftCHeadEnt *out)
{
	uint32		nlo,
				nhi;

	sift_crun_head_counts(r->heads, r->n, &nlo, &nhi);
	seg_read(seg, at + sift_crun_bytes(r->kind, r->n), (nlo + nhi) * sizeof(SiftCHeadEnt), out);
}

/* The raw entry bytes [pos, pos + cnt) of an ORD run (width bytes each), for the caller's own search */
void
sift_cseg_run_bytes_read(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint32 pos, uint32 cnt, uint8 *dst)
{
	seg_read(seg, at + SIFT_CRUN_HDR + (uint64) pos * r->width, (Size) cnt * r->width, dst);
}

void
sift_cseg_run_read(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint32 pos, uint32 cnt, uint32 *out)
{
	uint8	   *buf = NULL;

	if (r->kind == SIFT_CRUN_ORD)
	{
		buf = palloc((Size) cnt * r->width + 1);
		seg_read(seg, at + SIFT_CRUN_HDR + (uint64) pos * r->width, (Size) cnt * r->width, buf);
	}
	if (!sift_crun_entries(r, buf, pos, cnt, out))
		corrupt(seg, "The sorted run entries %u to %u are not rows of its unit of %u.", pos, pos + cnt, r->n);
	if (buf)
		pfree(buf);
}

#define SET_PROBE_KEYS	64				/* a LUT of more matches is tested through the summaries that list their keys, not probed key by key */

#define SET_PROBE_WORK	(4 * 1024 * 1024)	/* IN members times the units to test, past which the summaries are not read */

/* Can a unit whose keys are s hold a row that passes t; codes: the keys of a LUT's matches when there are few (ncodes), else all of them are unknown */
static bool
set_may_pass(const SiftCSet *s, const SiftCTest *t, const int64 *codes, int ncodes)
{
	switch (t->kind)
	{
		case SIFT_CT_RANGE:
			return sift_cset_may_hold(s, t->lo, t->hi);
		case SIFT_CT_SET:
			for (uint32 i = 0; i < t->nset; i++)
				if (sift_cset_may_hold(s, t->set[i], t->set[i]))
					return true;
			return false;
		default:					/* LUT */
			if (ncodes >= 0)
			{
				for (int i = 0; i < ncodes; i++)
					if (sift_cset_may_hold(s, codes[i], codes[i]))
						return true;
				return false;
			}
			if (s->kind == SIFT_CSET_EXACT)
			{
				int64		member[SIFT_CSET_LIMIT_MAX];

				sift_cset_members(s, member);
				for (uint32 i = 0; i < s->n; i++)
					if (member[i] >= 0 && (uint64) member[i] < t->lutbits && sift_col_bit_test(t->lut, (uint32) member[i]))
						return true;
				return false;
			}
			return true;
	}
}

/* Clears the bits of the units [ulo, uhi) whose summary shows that no key can pass t, and sets their bits in setcut (NULL: not told). Units with overflow values or inline strings are left alone */
void
sift_cseg_prune_sets(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut)
{
	const ZoneArrays *z;
	const SetsLeaf *sl;
	int64		codes[SET_PROBE_KEYS];
	int			ncodes = -1;
	uint32		todo = 0;

	if (t->kind != SIFT_CT_RANGE && t->kind != SIFT_CT_SET && t->kind != SIFT_CT_LUT)
		return;
	if (t->kind == SIFT_CT_LUT && t->lo <= t->hi)
	{
		/* the matches, when few, are keys to probe in a filter; the bounds of the LUT hold them all */
		int			m = 0;

		for (int64 c = Max(t->lo, 0); c <= t->hi && c < t->lutbits && m <= SET_PROBE_KEYS; c++)
			if (sift_col_bit_test(t->lut, (uint32) c))
			{
				if (m < SET_PROBE_KEYS)
					codes[m] = c;
				m++;
			}
		ncodes = m <= SET_PROBE_KEYS ? m : -1;
	}
	if ((sl = seg_set_leaf(seg, col)) == NULL)
		return;
	/* a filter answers only a key: a range of more keys or a LUT of many matches is left to the zones when the leaf has no exact set */
	if (!(sl->flags & (SETS_EXACT | SETS_PRES)) && ((t->kind == SIFT_CT_RANGE && (uint64) t->hi - (uint64) t->lo >= SIFT_CSET_PROBES) || (t->kind == SIFT_CT_LUT && ncodes < 0)))
		return;
	for (uint32 u = ulo; u < uhi; u++)
		todo += (keep[u / 64] >> (u % 64)) & 1;
	if (t->kind == SIFT_CT_SET && (uint64) t->nset * todo > SET_PROBE_WORK)
		return;
	z = zone_arrays(seg, col);
	for (uint32 u = ulo; u < uhi; u++)
	{
		const uint8 *blob;
		uint32		len;
		SiftCSet	s;

		if (!(keep[u / 64] >> (u % 64) & 1) || (z->flags[u] & (SIFT_COLZ_OVERFLOW | SIFT_COLZ_INLINE)) || z->nnull[u] >= z->nvals[u])
			continue;
		blob = sift_cseg_set(seg, col, u, &len);
		if (blob == NULL)
			continue;
		if (!sift_cset_open(blob, len, &s))
			corrupt(seg, "The small-set summary of leaf %d in unit %u is malformed.", col, u);
		if (!set_may_pass(&s, t, codes, ncodes))
		{
			keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			if (setcut)
				setcut[u / 64] |= UINT64CONST(1) << (u % 64);
		}
	}
}
