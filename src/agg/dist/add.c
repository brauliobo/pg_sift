/*
 * agg/dist/add.c - count(DISTINCT): the sets of values and the adding of a value
 * The hash set of (group, value) pairs, the bitmap chunks, and the add of a value to a
 * group in each mode; the update of a distinct aggregate from a batch.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/tuplesort.h"

#include "agg/dist/int.h"

uint64
dset_insert_raw(DSet *s, uint64 pair)
{
	uint64		h = dset_hash(pair) & (s->cap - 1);

	while (s->slot[h])
		h = (h + 1) & (s->cap - 1);
	s->slot[h] = pair;
	s->n++;
	return h;
}

/* twice the slots, the groups renumbered through remap (NULL: as they are) */
void
dset_grow(AggExec *ex, DSet *s, const uint32 *remap)
{
	uint64	   *old = s->slot;
	int64	   *ocnt = s->cnt;
	uint64		ocap = s->cap;

	s->cap = Max(ocap * 2, 1024);
	s->slot = MemoryContextAllocZero(ex->tcxt, s->cap * sizeof(uint64));
	s->cnt = s->counted ? MemoryContextAllocZero(ex->tcxt, s->cap * sizeof(int64)) : NULL;
	s->n = 0;
	for (uint64 i = 0; i < ocap; i++)
		if (old[i])
		{
			uint64		h = dset_insert_raw(s, remap ? DSET_PAIR(remap[(old[i] >> 32) & 0x7fffffff], old[i]) : old[i]);

			if (s->counted)
				s->cnt[h] = ocnt[i];
		}
	if (old)
		pfree(old);
	if (ocnt)
		pfree(ocnt);
}

/* the slot of a pair is read soon: the miss of a table past the caches overlaps the work in between */
void
dset_prefetch(const DSet *s, uint32 g, uint32 v)
{
	if (s->cap)
		__builtin_prefetch(&s->slot[dset_hash(DSET_PAIR(g, v)) & (s->cap - 1)]);
}

/* true when the pair is new */
bool
dset_add(AggExec *ex, DSet *s, uint32 g, uint32 v)
{
	uint64		pair = DSET_PAIR(g, v),
				h;

	if (s->n * 2 >= s->cap)
		dset_grow(ex, s, NULL);
	h = dset_hash(pair) & (s->cap - 1);
	while (s->slot[h] && s->slot[h] != pair)
		h = (h + 1) & (s->cap - 1);
	if (s->slot[h])
	{
		if (s->counted)
			s->cnt[h]++;
		return false;
	}
	s->slot[h] = pair;
	s->n++;
	if (s->counted)
		s->cnt[h] = 1;
	return true;
}

Dist *
dist_new(AggExec *ex, bool sgn)
{
	Dist	   *d = MemoryContextAllocZero(ex->tcxt, sizeof(Dist));

	d->mode = DD_WORDS;
	d->sgn = sgn;
	d->budget = get_hash_memory_limit();
	d->wmark = PG_INT64_MIN;
	return d;
}

#define DIST_SPARSE_BYTES	128		/* bytes of bitmap chunks per value from which a hash set (32 bytes a pair) is the smaller */

/*
 * A table that spilled with sets that had left their bitmaps holds groups of few values: a bitmap costs a directory and a chunk, zeroed, for
 * each group of each table, and the move to a hash set walks them again. The next tables start with the hash set.
 */
void
dist_start_hash(Dist *d)
{
	d->mode = DD_HASH;
	d->maxn = Max(d->budget / (4 * sizeof(uint64)), 256);
}

/* the sets of the table that spilled that are not bitmaps any more, or hold less than a value in 16 bytes of bitmap chunks (dist_start_hash) */
void
dist_note_spill(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	if (ex->dist == NULL)
		return;
	for (int ai = 0; ai < Min(ex->spec->naggs, 64); ai++)
	{
		Dist	   *d = ex->dist[ai];
		uint64		pairs = 0;

		if (d == NULL || d->mode == DD_WORDS || ex->spec->agg[ai].kind != SAA_COUNT_DISTINCT)
			continue;
		if (d->mode == DD_BITS)
			for (uint32 g = 0; g < n; g++)
				pairs += ex->acc[ai].n[g];
		if (d->mode != DD_BITS || d->bytes > DIST_SPARSE_BYTES * pairs)
			ex->dist_hash |= UINT64CONST(1) << ai;
	}
}

static void
dist_put_sort(Dist *d, uint32 g, uint32 v)
{
	if (g >= d->sidgroups)
	{
		uint32		n = Max(Max(g + 1, d->sidgroups * 2), 64);
		uint32	   *ns = MemoryContextAllocZero(GetMemoryChunkContext(d), n * sizeof(uint32));

		if (d->sid)
		{
			memcpy(ns, d->sid, d->sidgroups * sizeof(uint32));
			pfree(d->sid);
		}
		d->sid = ns;
		d->sidgroups = n;
	}
	if (d->sid[g] == 0)
		d->sid[g] = ++d->nsid;
	tuplesort_putdatum(d->sort, Int64GetDatum((int64) (d->sid[g] - 1) << 32 | v), false);
}

/* the directories cover the chunk xc; false if they cannot (the set is no longer a bitmap) */
static bool
dist_cover(AggExec *ex, Dist *d, int64 xc)
{
	int64		lo,
				hi;
	uint32		nn;
	int64		shift;
	uint64		touched = 0;

	if (d->ncd == 0)
	{
		d->cbase = xc & ~(int64) (DB_NCD_MIN / 2 - 1);
		d->ncd = DB_NCD_MIN;
		return true;
	}
	if (xc >= d->cbase && xc < d->cbase + d->ncd)
		return true;
	SIFT_INVARIANT(d->wmark == PG_INT64_MIN || xc >= (d->wmark >> DB_SHIFT), "sift: a value of a streamed count(DISTINCT) came back below its watermark");
	lo = Min(d->cbase, xc) & ~(int64) (DB_NCD_MIN / 2 - 1);
	hi = Max(d->cbase + d->ncd - 1, xc);
	if (hi - lo + 1 > DB_NCD_MAX)
		return false;
	nn = pg_nextpower2_32((uint32) (hi - lo + 1));
	for (uint32 g = 0; g < d->ngd; g++)
		touched += d->gd[g] != NULL;
	if (d->bytes + touched * (nn - d->ncd) * sizeof(DistChunk) > d->budget)
		return false;
	shift = d->cbase - lo;
	for (uint32 g = 0; g < d->ngd; g++)
		if (d->gd[g])
		{
			DistChunk  *nd = MemoryContextAllocZero(ex->tcxt, nn * sizeof(DistChunk));

			memcpy(nd + shift, d->gd[g], d->ncd * sizeof(DistChunk));
			pfree(d->gd[g]);
			d->gd[g] = nd;
		}
	d->bytes += touched * (nn - d->ncd) * sizeof(DistChunk);
	d->cbase = lo;
	d->ncd = nn;
	return true;
}

/*
 * The chunk of 4096 bits that holds chunk number xc of group g, allocated if it is not there yet; NULL when the set stopped being a
 * bitmap (it has moved to a hash set and nothing was added).
 */
static uint64 *
dist_chunk_for(AggExec *ex, Dist *d, uint32 g, int64 xc)
{
	DistChunk  *dir;

	if (!dist_cover(ex, d, xc))
		goto full;
	if (g >= d->ngd)
	{
		uint32		n = Max(Max(g + 1, d->ngd * 2), 64);
		DistChunk **ng = MemoryContextAllocZero(ex->tcxt, n * sizeof(DistChunk *));

		if (d->gd)
		{
			memcpy(ng, d->gd, d->ngd * sizeof(DistChunk *));
			pfree(d->gd);
		}
		d->gd = ng;
		d->ngd = n;
	}
	if (d->gd[g] == NULL)
	{
		if (d->bytes + d->ncd * sizeof(DistChunk) > d->budget)
			goto full;
		d->gd[g] = MemoryContextAllocZero(ex->tcxt, d->ncd * sizeof(DistChunk));
		d->bytes += d->ncd * sizeof(DistChunk);
	}
	dir = d->gd[g];
	if (dir[xc - d->cbase] == NULL)
	{
		if (d->bytes + DB_WORDS * sizeof(uint64) > d->budget)
			goto full;
		dir[xc - d->cbase] = dist_chunk(ex, d);
		d->bytes += DB_WORDS * sizeof(uint64);
	}
	return dir[xc - d->cbase];
full:
	/* a streaming scan gives the windows below its mark back before the bitmap is given up; each try moves the base, so it ends */
	if (dist_compact(d))
		return dist_chunk_for(ex, d, g, xc);
	dist_convert(ex, d, DD_HASH);
	return NULL;
}

/*
 * The bit of x in group g, when the fast path of the caller did not find its chunk: 1 if it was not set, 0 if it was, -1 when the
 * set stopped being a bitmap (and the pair is not added: the caller goes on with dist_add).
 */
static int
dist_bits_slow(AggExec *ex, Dist *d, uint32 g, int64 x)
{
	uint64	   *ch = dist_chunk_for(ex, d, g, x >> DB_SHIFT);
	uint64	   *w;
	uint64		m = UINT64CONST(1) << (x & 63);

	if (ch == NULL)
		return -1;
	w = &ch[(x >> 6) & (DB_WORDS - 1)];
	if (*w & m)
		return 0;
	*w |= m;
	return 1;
}

/* DD_WORDS: room for group g and the values lo..hi; false when the window would pass the limit, and the caller makes the set a bitmap */
static bool
dist_narrow_room(AggExec *ex, Dist *d, uint32 g, int64 lo, int64 hi)
{
	int64		maxk = sift_limit(dist_narrow_words, DIST_NARROW_WORDS),
				nlo = lo & ~(int64) 63,
				nhi = (hi | 63) + 1;
	uint32		nk,
				nng = g < d->nng ? d->nng : Max(Max(g + 1, d->nng * 2), d->nng ? 64 : Max(64, spill_keep(ex)));
	uint64	   *nw;

	if (d->nk)
	{
		nlo = Min(nlo, d->nlo);
		nhi = Max(nhi, d->nlo + (int64) d->nk * 64);
	}
	if ((nhi - nlo) / 64 > maxk)
		return false;
	/* a window that widens goes to the widest at once: each step copies the words of every group */
	nk = (uint32) (d->nk && nhi - nlo > (int64) d->nk * 64 ? maxk : (nhi - nlo) / 64);
	if (nk == d->nk && nng == d->nng)
		return true;
	nw = MemoryContextAllocZero(ex->tcxt, (size_t) nng * nk * sizeof(uint64));
	for (uint32 i = 0; i < d->nng && d->nk; i++)
		memcpy(nw + (size_t) i * nk + (d->nlo - nlo) / 64, d->nw + (size_t) i * d->nk, d->nk * sizeof(uint64));
	if (d->nw)
		pfree(d->nw);
	d->nw = nw;
	d->nlo = nlo;
	d->nk = nk;
	d->nng = nng;
	return true;
}

/* DD_WORDS: the bit of x in group g; 1 if it was not set, 0 if it was, -1 when the set became a bitmap (the pair is not added) */
static int
dist_narrow_bit(AggExec *ex, Dist *d, uint32 g, int64 x)
{
	uint64	   *w;
	uint64		m = UINT64CONST(1) << (x & 63);

	if (!d->nk || (uint64) (x - d->nlo) >= (uint64) d->nk * 64 || g >= d->nng)
		if (!dist_narrow_room(ex, d, g, x, x))
		{
			dist_convert(ex, d, DD_BITS);
			return -1;
		}
	w = &d->nw[(size_t) g * d->nk + (x - d->nlo) / 64];
	if (*w & m)
		return 0;
	*w |= m;
	return 1;
}

static bool
dist_hash_add(AggExec *ex, Dist *d, uint32 g, uint32 v)
{
	DSet	   *s = &d->ds;
	uint64		pair = DSET_PAIR(g, v),
				h;

	if (s->cap == 0)
		dset_grow(ex, s, NULL);
	h = dset_hash(pair) & (s->cap - 1);
	while (s->slot[h] && s->slot[h] != pair)
		h = (h + 1) & (s->cap - 1);
	if (s->slot[h])
		return false;
	if (s->n >= d->maxn)
	{
		if (dist_compact_hash(ex, d))
			return dist_hash_add(ex, d, g, v);
		dist_convert(ex, d, DD_SORT);
		dist_put_sort(d, g, v);
		return false;
	}
	s->slot[h] = pair;
	if (++s->n * 2 >= s->cap)
		dset_grow(ex, s, NULL);
	return true;
}

bool
dist_add(AggExec *ex, Dist *d, uint32 g, uint32 v)
{
	if (d->mode == DD_WORDS)
	{
		int			r = dist_narrow_bit(ex, d, g, dist_x(d, v));

		if (r >= 0)
			return r == 1;
	}
	if (d->mode == DD_BITS)
	{
		int			r = dist_bits_slow(ex, d, g, dist_x(d, v));

		if (r >= 0)
			return r == 1;
	}
	if (d->mode == DD_HASH)
		return dist_hash_add(ex, d, g, v);
	dist_put_sort(d, g, v);
	return false;
}

/*
 * The bits of another bitmap added to the set of group g, how many were new: nwords words, little endian and not aligned, the first of which holds the
 * values from 64 * wbase (a word of a chunk is DB_WORDS words long)
 */
uint64
dist_or_words(AggExec *ex, Dist *d, uint32 g, int64 wbase, const uint8 *words, uint64 nwords)
{
	uint64		n = 0;

	/* whole chunks of another bitmap (a unit's key bitmap, a worker's dense chunk) do not fit a window */
	if (d->mode == DD_WORDS && nwords > (uint64) sift_limit(dist_narrow_words, DIST_NARROW_WORDS))
		dist_convert(ex, d, DD_BITS);
	if (d->mode == DD_WORDS)
	{
		uint64		m;

		/* a window that does not cover the words is made to: for the words that have a bit only */
		if (g >= d->nng || wbase * 64 < d->nlo || (wbase + (int64) nwords) * 64 > d->nlo + (int64) d->nk * 64)
		{
			uint64		first = 0,
						end = nwords;

			for (; first < end; first++)
			{
				memcpy(&m, words + first * sizeof(m), sizeof(m));
				if (m)
					break;
			}
			for (; end > first; end--)
			{
				memcpy(&m, words + (end - 1) * sizeof(m), sizeof(m));
				if (m)
					break;
			}
			if (first == end)
				return 0;
			wbase += first;
			words += first * sizeof(m);
			nwords = end - first;
			if (!dist_narrow_room(ex, d, g, wbase * 64, (wbase + (int64) nwords) * 64 - 1))
				dist_convert(ex, d, DD_BITS);
		}
		if (d->mode == DD_WORDS)
		{
			uint64	   *row = d->nw + (size_t) g * d->nk + (wbase - d->nlo / 64);

			for (uint64 k = 0; k < nwords; k++)
			{
				memcpy(&m, words + k * sizeof(m), sizeof(m));
				n += pg_popcount64(m & ~row[k]);
				row[k] |= m;
			}
			return n;
		}
	}
	for (uint64 i = 0; i < nwords;)
	{
		int64		xc = (wbase + (int64) i) >> (DB_SHIFT - 6);
		uint32		w0 = (uint32) ((wbase + (int64) i) & (DB_WORDS - 1));
		uint64		cnt = Min(nwords - i, DB_WORDS - w0);
		uint64	   *ch = d->mode == DD_BITS ? dist_chunk_for(ex, d, g, xc) : NULL;

		for (uint64 k = 0; k < cnt; k++)
		{
			uint64		m;

			memcpy(&m, words + (i + k) * sizeof(m), sizeof(m));
			if (ch)
			{
				n += pg_popcount64(m & ~ch[w0 + k]);
				ch[w0 + k] |= m;
				continue;
			}
			for (; m; m &= m - 1)
				n += dist_add(ex, d, g, (uint32) (xc * (1 << DB_SHIFT) + (w0 + k) * 64 + pg_rightmost_one_pos64(m)));
		}
		i += cnt;
	}
	return n;
}

/* the bits of a chunk of another bitmap (chunk number xc of group g) added to the set; how many were new */
uint64
dist_or_chunk(AggExec *ex, Dist *d, uint32 g, int64 xc, const uint64 *bits)
{
	return dist_or_words(ex, d, g, xc * DB_WORDS, (const uint8 *) bits, DB_WORDS);
}

/* the groups renumbered through remap (UINT32_MAX: dropped) while the scan runs; newcap is their new capacity */
void
dist_regroup(AggExec *ex, Dist *d, const uint32 *remap, uint32 oldcap, uint32 newcap)
{
	if (d->mode == DD_HASH)
	{
		if (d->ds.slot)
			dset_grow(ex, &d->ds, remap);
		return;
	}
	if (d->mode == DD_WORDS && d->nw)
	{
		uint64	   *nw = MemoryContextAllocZero(ex->tcxt, (size_t) Max(newcap, 1) * d->nk * sizeof(uint64));

		for (uint32 g = 0; g < Min(oldcap, d->nng); g++)
			if (remap[g] != UINT32_MAX)
				memcpy(nw + (size_t) remap[g] * d->nk, d->nw + (size_t) g * d->nk, d->nk * sizeof(uint64));
		pfree(d->nw);
		d->nw = nw;
		d->nng = Max(newcap, 1);
	}
	if (d->mode == DD_BITS && d->gd)
	{
		DistChunk **ng = MemoryContextAllocZero(ex->tcxt, Max(newcap, 1) * sizeof(DistChunk *));

		for (uint32 g = 0; g < Min(oldcap, d->ngd); g++)
			if (d->gd[g] && remap[g] != UINT32_MAX)
				ng[remap[g]] = d->gd[g];
			else if (d->gd[g])
				pfree(d->gd[g]);
		pfree(d->gd);
		d->gd = ng;
		d->ngd = Max(newcap, 1);
	}
	if (d->mode == DD_SORT && d->sid)
	{
		uint32	   *ns = MemoryContextAllocZero(GetMemoryChunkContext(d), Max(newcap, 1) * sizeof(uint32));

		for (uint32 g = 0; g < Min(oldcap, d->sidgroups); g++)
			if (d->sid[g] && remap[g] != UINT32_MAX)
				ns[remap[g]] = d->sid[g];
		pfree(d->sid);
		d->sid = ns;
		d->sidgroups = Max(newcap, 1);
	}
}

/* the fields of a bitmap set the loops of dist_update read, in registers: the stores to the bits would make the compiler reload them */
typedef struct DistView
{
	int64		cbase;
	uint32		ncd;
	uint32		ngd;
	DistChunk **gd;
	bool		sgn;
} DistView;

static inline DistView
dist_view(const Dist *d)
{
	DistView	vw = {d->cbase, d->ncd, d->ngd, d->gd, d->sgn};

	return vw;
}

static inline int64
dist_vx(const DistView *vw, uint32 v)
{
	return vw->sgn ? (int64) (int32) v : (int64) v;
}

/* the word of the bit of x in group g and its mask, when the chunk is there; NULL when it is not */
static inline uint64 *
dist_word(const DistView *vw, uint32 g, int64 x, uint64 *m)
{
	int64		c = (x >> DB_SHIFT) - vw->cbase;
	DistChunk  *dir;

	if (unlikely((uint64) c >= vw->ncd || g >= vw->ngd || (dir = vw->gd[g]) == NULL || dir[c] == NULL))
		return NULL;
	*m = UINT64CONST(1) << (x & 63);
	return &dir[c][(x >> 6) & (DB_WORDS - 1)];
}

/* The span of the values the zones of an integer leaf hold (the largest less the smallest), -1 when they do not say */
static int64
dist_zone_span(AggExec *ex, int leaf)
{
	SiftSrcZone z;

	if (leaf < 0 || leaf >= ex->nbase || ex->lay->leaf[leaf].vtype != SCV_I32 || ex->src == NULL || !sift_colsrc_has_zone_range(ex->src))
		return -1;
	if (sift_colsrc_zone_range(ex->src, ex->tid_sb_lo, Min(sift_colsrc_nsb(ex->src), ex->tid_sb_hi), leaf, &z) <= 0 || z.inexact || z.nnull >= z.nvals)
		return -1;
	return z.max - z.min;
}

/*
 * Do the zones of an integer leaf span more than the widest window? A column that grows with the heap order (a date, a serial) would fit one
 * for most of the scan and pay for the bitmap at the end: its set starts as one.
 */
static bool
dist_narrow_spans(AggExec *ex, int leaf)
{
	int64		span = dist_zone_span(ex, leaf);

	return span >= 0 && span >= 64 * sift_limit(dist_narrow_words, DIST_NARROW_WORDS);
}

/*
 * What a set that outgrew the narrow window starts as. A bitmap costs a directory and 512 bytes for each chunk of 4096 values a group has a value in;
 * a hash set 32 bytes a value. A group holds at most est_rows / est_groups values, in at most as many chunks as the zones span: the hash set is the
 * smaller when that is fewer than 16 times the chunks and the directory (a column of a hundred thousand values, a few rows of each key: 880
 * bytes a group in a bitmap against 60). No span in the zones, or no estimate: the bitmap.
 */
static DistMode
dist_wide_mode(AggExec *ex, int leaf)
{
	int64		span = dist_zone_span(ex, leaf);

	if (span < 0 || ex->spec->est_rows <= 0)
		return DD_BITS;
	return ex->spec->est_rows / Max(ex->spec->est_groups, 1.0) < 16.0 * (2.0 + (double) (span >> DB_SHIFT)) ? DD_HASH : DD_BITS;
}

/* DD_WORDS: the first batch sets the window; the tuple it stopped at (nsel, or the one that made the set a bitmap) */
static int
dist_update_narrow(AggExec *ex, Dist *d, int ai, int64 *cnt, const uint64 *ok, int nsel, const uint32 *v)
{
	int64		lo = INT64_MAX,
				hi = INT64_MIN;
	int			j;

	if (d->nk == 0)
	{
		for (j = 0; j < nsel; j++)
			if (!ok || sift_colsrc_bit(ok, j))
			{
				lo = Min(lo, dist_x(d, v[j]));
				hi = Max(hi, dist_x(d, v[j]));
			}
		if (lo > hi)
			return nsel;
		if ((d->sgn && dist_narrow_spans(ex, ex->spec->agg[ai].leaf)) || !dist_narrow_room(ex, d, 0, lo, hi))
		{
			dist_convert(ex, d, d->sgn ? dist_wide_mode(ex, ex->spec->agg[ai].leaf) : DD_BITS);
			return 0;
		}
	}
	for (j = 0; j < nsel && d->mode == DD_WORDS; j++)
	{
		int			r;

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		if ((r = dist_narrow_bit(ex, d, ex->gid[j], dist_x(d, v[j]))) < 0)
			break;
		cnt[ex->gid[j]] += r;
	}
	return j;
}

/* adds the selected values (v[j] of tuple j, in the group ex->gid[j]); the count of a group goes up for each new one */
void
dist_update(AggExec *ex, int ai, const uint64 *ok, int nsel, const uint32 *v)
{
	Dist	   *d = ex->dist[ai];
	int64	   *cnt = ex->acc[ai].n;
	const uint32 *gid = ex->gid;
	int			j = d->mode == DD_WORDS ? dist_update_narrow(ex, d, ai, cnt, ok, nsel, v) : 0;
	DistView	vw = dist_view(d);

	/* bits that fit the cache are set one by one; past that, a batch at a time: the words are found and prefetched first, since each is a miss */
	for (; j < nsel && d->mode == DD_BITS && d->bytes < DB_PREFETCH; j++)
	{
		uint32		g = gid[j];
		uint64		m;
		uint64	   *w;

		if (ok && !sift_colsrc_bit(ok, j))
			continue;
		if ((w = dist_word(&vw, g, dist_vx(&vw, v[j]), &m)) != NULL)
		{
			cnt[g] += !(*w & m);
			*w |= m;
		}
		else
		{
			int			r = dist_bits_slow(ex, d, g, dist_x(d, v[j]));

			if (r < 0)
				break;
			cnt[g] += r;
			vw = dist_view(d);
		}
	}
	while (j < nsel && d->mode == DD_BITS)
	{
		int			nb = Min(DB_BATCH, nsel - j);
		uint64	   *wp[DB_BATCH];
		uint64		mk[DB_BATCH];
		bool		take[DB_BATCH];
		int			k;

		for (k = 0; k < nb; k++)
		{
			take[k] = !ok || sift_colsrc_bit(ok, j + k);
			wp[k] = take[k] ? dist_word(&vw, gid[j + k], dist_vx(&vw, v[j + k]), &mk[k]) : NULL;
			if (wp[k])
				__builtin_prefetch(wp[k], 1);
		}
		for (k = 0; k < nb; k++)
		{
			uint32		g = gid[j + k];

			if (wp[k])
			{
				cnt[g] += !(*wp[k] & mk[k]);
				*wp[k] |= mk[k];
			}
			else if (take[k])
			{
				int			r = dist_bits_slow(ex, d, g, dist_x(d, v[j + k]));

				if (r < 0)
					break;
				cnt[g] += r;
			}
		}
		j += k;
		if (k < nb)
			break;
		vw = dist_view(d);
	}
	if (j < nsel && d->mode == DD_HASH && d->ds.n + (uint64) (nsel - j) < d->maxn)
	{
		DSet	   *s = &d->ds;
		uint64		pair[32],
					h[32];

		while ((s->n + (nsel - j)) * 2 >= s->cap)
			dset_grow(ex, s, NULL);
		while (j < nsel)
		{
			int			m = 0,
						jj[32];

			for (; j < nsel && m < 32; j++)
				if (!ok || sift_colsrc_bit(ok, j))
				{
					pair[m] = DSET_PAIR(ex->gid[j], v[j]);
					h[m] = dset_hash(pair[m]) & (s->cap - 1);
					__builtin_prefetch(&s->slot[h[m]]);
					jj[m++] = j;
				}
			for (int k = 0; k < m; k++)
			{
				uint64		p = h[k];

				while (s->slot[p] && s->slot[p] != pair[k])
					p = (p + 1) & (s->cap - 1);
				if (!s->slot[p])
				{
					s->slot[p] = pair[k];
					s->n++;
					cnt[ex->gid[jj[k]]]++;
				}
			}
		}
	}
	for (; j < nsel; j++)
		if ((!ok || sift_colsrc_bit(ok, j)) && dist_add(ex, d, ex->gid[j], v[j]))
			cnt[ex->gid[j]]++;
}

