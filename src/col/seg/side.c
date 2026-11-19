/*
 * col/seg/side.c - the reader of the scope sidecar of a segment (col/side.h)
 *
 * The sidecar is whole pages after the segment's own stream and before its net pre-aggregates; its last 16 bytes say where it starts, the header is in front of them and the records
 * of its scopes in front of that. The segment's format minor says whether there is one. The keys of a scope and their run records are read once; a run's units, cells and cubes are read when asked for.
 */
/* check-memory: allow-allocs the directory of the keys of one scope in one segment, and the runs of one key */
#include "core/host.h"
#include "col/seg_int.h"
#include "col/side.h"

struct SiftSide
{
	SideHeader	h;
	SideScope  *scope;			/* [nscope] */
	int64	  **keys;			/* [nscope] read on first use */
	SideKey   **recs;
	uint64		at;				/* where the sidecar starts in the stream */
	uint8	   *cube;			/* the last cube read */
	Size		cubecap;
};

const SiftSide *
sift_cseg_side(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (!pv->side_read)
	{
		uint64		end,
					trailer[2],
					size;

		pv->side_read = true;
		if (seg->entry.format < SIFT_COL_FORMAT_SIDE)
			return NULL;
		sift_cseg_net_pages(seg, &end);
		size = SIFT_PAYLOAD_BYTES(seg->sr.npages);
		if (end < SIDE_TRAILER || end > size)
			corrupt(seg, "The segment says it has a scope sidecar, its stream ends at %llu.", ULL(end));
		seg_read(seg, end - SIDE_TRAILER, SIDE_TRAILER, trailer);
		if (trailer[0] != SIDE_MAGIC || SIFT_PAYLOAD_OFF(trailer[1]) != 0 || trailer[1] + sizeof(SideHeader) + SIDE_TRAILER > end)
			corrupt(seg, "The trailer of the scope sidecar (0x%llx, at %llu) does not fit the stream of %llu bytes.", ULL(trailer[0]), ULL(trailer[1]), ULL(end));
		{
			SiftSide   *sd = MemoryContextAllocZero(seg->mcxt, sizeof(SiftSide));
			uint64		head = end - SIDE_TRAILER - sizeof(SideHeader);

			sd->at = trailer[1];
			seg_read(seg, head, sizeof(SideHeader), &sd->h);
			if (sd->h.magic != SIDE_MAGIC || sd->h.version != SIDE_VERSION || sd->h.nsb != seg->nsb || sd->h.nscope == 0 || sd->h.nscope > SIFT_SCOPE_MAX ||
				sd->at + sd->h.scope_off + sd->h.nscope * sizeof(SideScope) != head || sd->h.len > sd->h.scope_off)
				corrupt(seg, "The scope sidecar (0x%08x, version %u, %u units, %u scopes, %llu bytes) does not fit the segment.", sd->h.magic, sd->h.version, sd->h.nsb, sd->h.nscope, ULL(sd->h.len));
			sd->scope = MemoryContextAlloc(seg->mcxt, sd->h.nscope * sizeof(SideScope));
			seg_read(seg, sd->at + sd->h.scope_off, sd->h.nscope * sizeof(SideScope), sd->scope);
			sd->keys = MemoryContextAllocZero(seg->mcxt, sd->h.nscope * sizeof(int64 *));
			sd->recs = MemoryContextAllocZero(seg->mcxt, sd->h.nscope * sizeof(SideKey *));
			for (uint32 s = 0; s < sd->h.nscope; s++)
			{
				const SideScope *sc = &sd->scope[s];

				if (sc->keys_off + (uint64) sc->nkeys * sizeof(int64) > sd->h.len || sc->recs_off + (uint64) sc->nkeys * sizeof(SideKey) > sd->h.len ||
					sc->runs_off + (uint64) sc->nruns * sizeof(SideRun) > sd->h.len)
					corrupt(seg, "The directory of scope %u in the scope sidecar lies outside it.", sc->ordinal);
			}
			pv->side = sd;
		}
	}
	return pv->side;
}

const SideScope *
sift_side_scope(const SiftSide *sd, uint32 ordinal)
{
	for (uint32 s = 0; s < sd->h.nscope; s++)
		if (sd->scope[s].ordinal == ordinal)
			return &sd->scope[s];
	return NULL;
}

static uint32
scope_slot(const SiftSide *sd, const SideScope *sc)
{
	return (uint32) (sc - sd->scope);
}

const int64 *
sift_side_keys(SiftColSegment *seg, const SiftSide *sd, const SideScope *sc, const SideKey **recs)
{
	SiftSide   *w = (SiftSide *) sd;
	uint32		s = scope_slot(sd, sc);

	if (w->keys[s] == NULL)
	{
		w->keys[s] = MemoryContextAlloc(seg->mcxt, Max(sc->nkeys, 1) * sizeof(int64));
		w->recs[s] = MemoryContextAlloc(seg->mcxt, Max(sc->nkeys, 1) * sizeof(SideKey));
		seg_read(seg, sd->at + sc->keys_off, (Size) sc->nkeys * sizeof(int64), w->keys[s]);
		seg_read(seg, sd->at + sc->recs_off, (Size) sc->nkeys * sizeof(SideKey), w->recs[s]);
	}
	if (recs != NULL)
		*recs = w->recs[s];
	return w->keys[s];
}

const SideRun *
sift_side_runs(SiftColSegment *seg, const SiftSide *sd, const SideScope *sc, int64 key, uint32 *n)
{
	SiftSide   *w = (SiftSide *) sd;
	uint32		s = scope_slot(sd, sc),
				lo = 0,
				hi = sc->nkeys;
	SideRun    *runs;

	*n = 0;
	sift_side_keys(seg, sd, sc, NULL);
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (w->keys[s][mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == sc->nkeys || w->keys[s][lo] != key)
		return NULL;
	if ((uint64) w->recs[s][lo].first_run + w->recs[s][lo].nruns > sc->nruns)
		corrupt(seg, "The runs of key " INT64_FORMAT " in the scope sidecar lie outside it.", key);
	*n = w->recs[s][lo].nruns;
	runs = MemoryContextAlloc(seg->mcxt, (Size) *n * sizeof(SideRun));
	seg_read(seg, sd->at + sc->runs_off + (uint64) w->recs[s][lo].first_run * sizeof(SideRun), (Size) *n * sizeof(SideRun), runs);
	return runs;
}

uint32
sift_side_run_units(SiftColSegment *seg, const SiftSide *sd, const SideRun *r, uint32 *units)
{
	uint8		buf[SIDE_RUN_UNITS * 5];
	const uint8 *p = buf;
	Size		len = Min(sizeof(buf), sd->h.len - Min(r->units_off, sd->h.len));
	uint32		prev = 0;

	if (r->nunits > SIDE_RUN_UNITS)
		corrupt(seg, "A run of the scope sidecar lists %u units.", r->nunits);
	seg_read(seg, sd->at + r->units_off, len, buf);
	for (uint32 i = 0; i < r->nunits; i++)
	{
		uint64		v = 0;
		int			sh = 0;

		for (;;)
		{
			uint8		c;

			if ((Size) (p - buf) >= len)
				corrupt(seg, "The unit list of a run of the scope sidecar ends too soon.");
			c = *p++;
			v |= (uint64) (c & 127) << sh;
			if (!(c & 128))
				break;
			sh += 7;
		}
		prev = i == 0 ? (uint32) v : prev + (uint32) v;
		units[i] = prev;
		if (prev >= seg->nsb)
			corrupt(seg, "A run of the scope sidecar names unit %u of %u.", prev, seg->nsb);
	}
	return r->nunits;
}

const SideCell *
sift_side_run_cells(SiftColSegment *seg, const SiftSide *sd, const SideRun *r)
{
	SideCell   *cells;

	if (r->cells_off + (uint64) r->ncells * sizeof(SideCell) > sd->h.len)
		corrupt(seg, "The cells of a run of the scope sidecar lie outside it.");
	cells = MemoryContextAlloc(seg->mcxt, Max(r->ncells, 1) * sizeof(SideCell));
	seg_read(seg, sd->at + r->cells_off, (Size) r->ncells * sizeof(SideCell), cells);
	return cells;
}

bool
sift_side_unit_leaf(SiftColSegment *seg, uint32 u, const SiftCUnitView *v, int leaf, int64 *dst, const uint64 **nulls, const uint64 **ovf)
{
	SiftCChunk	c;

	SIFT_INVARIANT(v->chunk_len[leaf] != 0, "the scope sidecar reads a leaf that has no chunk");
	sift_cseg_unit_fill(seg, u, v->chunk_off[leaf], v->chunk_len[leaf]);
	SIFT_INVARIANT(sift_cunit_chunk(v, leaf, &c), "the scope sidecar cannot read a chunk of a unit");
	*nulls = c.nulls;
	*ovf = c.ovf;
	if (c.enc == SIFT_COLENC_ISTR)
		return false;
	sift_cchunk_decode(&c, dst);
	return true;
}

const uint8 *
sift_side_cell_cube(SiftColSegment *seg, const SiftSide *sd, const SideCell *c)
{
	SiftSide   *w = (SiftSide *) sd;

	if (c->off + c->len > sd->h.len)
		corrupt(seg, "A cube of the scope sidecar lies outside it.");
	if (c->len > w->cubecap)
	{
		w->cubecap = Max(c->len, 2 * w->cubecap);
		SIFT_REGROW_IN(seg->mcxt, w->cube, w->cubecap);
	}
	seg_read(seg, sd->at + c->off, c->len, w->cube);
	return w->cube;
}

/* The positions a word sets in a filter of nbits bits: SIDE_FILTER_HASHES of them by double hashing of a mixed word */
static inline uint64
filter_pos(uint64 word, uint64 nbits, int i)
{
	uint64		h = word;

	h ^= h >> 33;
	h *= UINT64CONST(0xff51afd7ed558ccd);
	h ^= h >> 33;
	h *= UINT64CONST(0xc4ceb9fe1a85ec53);
	h ^= h >> 33;
	return ((h & 0xffffffff) + (uint64) i * ((h >> 32) | 1)) % nbits;
}

void
sift_side_filter_add(uint64 *bits, uint64 nbits, uint64 word)
{
	for (int i = 0; i < SIDE_FILTER_HASHES; i++)
	{
		uint64		p = filter_pos(word, nbits, i);

		bits[p / 64] |= UINT64CONST(1) << (p % 64);
	}
}

bool
sift_side_filter_has(const uint64 *bits, uint64 nbits, uint64 word)
{
	for (int i = 0; i < SIDE_FILTER_HASHES; i++)
	{
		uint64		p = filter_pos(word, nbits, i);

		if (!(bits[p / 64] >> (p % 64) & 1))
			return false;
	}
	return true;
}

uint64 *
sift_side_run_filter(SiftColSegment *seg, const SiftSide *sd, const SideRun *r, uint64 *nbits)
{
	uint64	   *bits;

	*nbits = (uint64) r->hi;
	if (*nbits == 0 || *nbits % 64 != 0 || (uint64) r->lo + *nbits / 8 > sd->h.len)
		corrupt(seg, "The member filter of a run of the scope sidecar lies outside it.");
	bits = palloc(*nbits / 8);
	seg_read(seg, sd->at + (uint64) r->lo, *nbits / 8, bits);
	return bits;
}

bool
sift_side_unit_strs(SiftColSegment *seg, uint32 u, const SiftCUnitView *v, int leaf, MemoryContext mcxt, int64 *dst, const uint64 **nulls, SiftCDict **dict)
{
	SiftCChunk	c,
				codes;

	SIFT_INVARIANT(v->chunk_len[leaf] != 0, "the scope sidecar reads a leaf that has no chunk");
	sift_cseg_unit_fill(seg, u, v->chunk_off[leaf], v->chunk_len[leaf]);
	SIFT_INVARIANT(sift_cunit_chunk(v, leaf, &c), "the scope sidecar cannot read a chunk of a unit");
	*nulls = c.nulls;
	if (c.enc == SIFT_COLENC_ISTR)
	{
		(void) sift_cchunk_istr_codes(&c, mcxt, NULL, 0, dict, &codes);
		sift_cchunk_decode(&codes, dst);
		return true;
	}
	*dict = sift_cseg_dict(seg, leaf);
	sift_cchunk_decode(&c, dst);
	return false;
}

uint64
sift_side_at(SiftColSegment *seg)
{
	const SiftSide *sd = sift_cseg_side(seg);

	return sd->at;
}
