/*
 * col/unit_find.c - L1 of the column engine: the rows of a key-cut unit at given heap addresses (the lookup of the dead addresses of a statement, col/track_dv.c).
 */
/* check-memory: allow-allocs the index holds the addresses of one statement, which col/track_dv.c holds at most SIFT_ADDR_IDX_SPAN blocks for each of, and the statement keeps them in its own accounted context */
#include "core/host.h"

#include "col/enc.h"
#include "core/cmp.h"

/* The first index of [lo, n) at which keys are not below the first address of heap block blk */
static uint32
key_from_block(const SiftKey *keys, uint32 lo, uint32 n, BlockNumber blk)
{
	return sift_u64_from(keys, lo, n, (SiftKey) blk << 16);
}

/* The first page of [lo, npages) of a key-cut unit whose block is not below blk, found as key_from_block finds a key */
static uint32
page_from_block(const SiftCUnitView *v, uint32 lo, BlockNumber blk)
{
	uint32		hi = lo,
				step = 1;

	while (hi < v->npages && v->blocks[hi] < blk)
	{
		lo = hi + 1;
		hi = Min(hi + step, v->npages);
		step *= 2;
	}
	SIFT_BISECT(uint32, m, lo, hi, v->blocks[m] < blk);
	return lo;
}

#define FIND_ROWS_BISECT 4			/* a page looks its rows up among the keys of its block when it has this many times fewer rows than they are */
#define FIND_DENSE_KEYS 3			/* keys for each heap block of a unit from which its pages are probed for runs of them */
#define FIND_PAGES_ONE_BY_ONE 3		/* the pages of a key-cut unit that are decoded alone before all are: a page costs more than the rest of the unit per row */

/* The rows of a page (r0 to r1, their offsets in ko) at the addresses keys[ks] to keys[ke) of its block, appended to rows and at: each row looks its address up when the block has many more of them */
static uint32
page_hits(const SiftKey *keys, uint32 ks, uint32 ke, BlockNumber blk, const uint16 *ko, uint32 r0, uint32 r1, uint32 *rows, SiftKey *at, uint32 found)
{
	uint32		r = r0,
				k = ks;

	if ((r1 - r0) * FIND_ROWS_BISECT < ke - ks)
		for (; r < r1 && k < ke; r++)
		{
			SiftKey		want = (SiftKey) blk << 16 | ko[r - r0];

			k = sift_u64_from(keys, k, ke, want);
			if (k < ke && keys[k] == want)
			{
				rows[found] = r;
				at[found++] = keys[k++];
			}
		}
	else
		for (; k < ke; k++)
		{
			while (r < r1 && ko[r - r0] < (keys[k] & 0xFFFF))
				r++;
			if (r < r1 && ko[r - r0] == (keys[k] & 0xFFFF))
			{
				rows[found] = r;
				at[found++] = keys[k];
			}
		}
	return found;
}

/*
 * The rows of a unit (superblock sb for a unit that is not key-cut) that are at the heap addresses keys (ascending), ascending and once each, and their keys in
 * at: both lists are walked together, and each step that finds no match jumps by a bisection over the longer of what is left, so the cost is that of the shorter
 * list (the pages of the unit, or the keys of the statement) and the offsets of only the pages that hold a key are decoded. rows and at have room for n.
 */
uint32
sift_cunit_find_keyed(const SiftCUnitView *v, const SiftKey *keys, uint32 n, uint32 *rows, SiftKey *at)
{
	uint32		p = 0,
				k = 0,
				found = 0,
				decoded = 0;
	bool		dense;

	sift_cunit_keyed_load(v);
	/* keys that outnumber the heap blocks of the unit several times over: its pages are probed for runs of keys of a block (a sparser list only pays for the probe) */
	dense = n >= (uint64) FIND_DENSE_KEYS * ((uint64) v->kblk1 - v->kblk0 + 1);
	while (k < n && p < v->npages)
	{
		BlockNumber blk = (BlockNumber) (keys[k] >> 16);
		const uint16 *ko;
		uint32		r,
					r0,
					r1,
					kend;

		if (v->blocks[p] < blk)
		{
			p = page_from_block(v, p + 1, blk);
			continue;
		}
		if (v->blocks[p] > blk)
		{
			k = key_from_block(keys, k + 1, n, v->blocks[p]);
			continue;
		}
		if (++decoded == FIND_PAGES_ONE_BY_ONE)
			sift_cunit_page_offsets_all(v);
		r0 = r = v->rowstart[p];
		r1 = v->rowstart[p + 1];
		ko = sift_cunit_keyed_page_offsets(v, p);
		if (dense && k + (r1 - r0) * FIND_ROWS_BISECT < n && (BlockNumber) (keys[k + (r1 - r0) * FIND_ROWS_BISECT] >> 16) == blk)
		{
			kend = sift_u64_from(keys, k + (r1 - r0) * FIND_ROWS_BISECT, n, ((SiftKey) blk + 1) << 16);
			found = page_hits(keys, k, kend, blk, ko, r0, r1, rows, at, found);
			k = kend;
		}
		else
			for (; k < n && (BlockNumber) (keys[k] >> 16) == blk; k++)
			{
				while (r < r1 && ko[r - r0] < (keys[k] & 0xFFFF))
					r++;
				if (r < r1 && ko[r - r0] == (keys[k] & 0xFFFF))
				{
					rows[found] = r;
					at[found++] = keys[k];
				}
			}
		p++;
	}
	return found;
}

SiftAddrIdx *
sift_addr_idx_make(const SiftKey *keys, uint32 n)
{
	SiftAddrIdx *x;
	uint64		span;
	uint32	   *first;
	uint32		i = 0;

	if (n == 0)
		return NULL;
	span = (uint64) (keys[n - 1] >> 16) - (keys[0] >> 16) + 1;
	if (span > (uint64) SIFT_ADDR_IDX_SPAN * n)
		return NULL;
	x = palloc(sizeof(SiftAddrIdx));
	first = palloc((span + 1) * sizeof(uint32));
	for (uint64 b = 0; b < span; b++)
	{
		first[b] = i;
		while (i < n && (keys[i] >> 16) - (keys[0] >> 16) == b)
			i++;
	}
	first[span] = n;
	*x = (SiftAddrIdx) {.keys = keys,.n = n,.blk0 = (BlockNumber) (keys[0] >> 16),.blk1 = (BlockNumber) (keys[n - 1] >> 16),.first = first};
	return x;
}

uint32
sift_cunit_find_addrs(const SiftCUnitView *v, const SiftAddrIdx *idx, uint32 *rows, SiftKey *at)
{
	uint32		found = 0,
				decoded = 0;

	sift_cunit_keyed_load(v);
	for (uint32 p = page_from_block(v, 0, idx->blk0); p < v->npages && v->blocks[p] <= idx->blk1; p++)
	{
		BlockNumber blk = v->blocks[p];
		uint32		ks = idx->first[blk - idx->blk0],
					ke = idx->first[blk - idx->blk0 + 1];

		if (ks == ke)
			continue;
		if (++decoded == FIND_PAGES_ONE_BY_ONE)
			sift_cunit_page_offsets_all(v);
		found = page_hits(idx->keys, ks, ke, blk, sift_cunit_keyed_page_offsets(v, p), v->rowstart[p], v->rowstart[p + 1], rows, at, found);
	}
	return found;
}

