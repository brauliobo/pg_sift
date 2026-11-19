/*
 * col/seg_write/side_member.c - the members of a text, varchar or uuid scope sidecar (col/seg_write/side.c): which units hold the members of a key, and the filter of the words of the members a run's units hold
 */
/* check-memory: allow-allocs the index of the members of one segment, bounded by maintenance_work_mem, and the filters of its runs */
#include "postgres.h"

#include "miscadmin.h"
#include "col/seg_write/side_int.h"

MemberCodes *
side_member_codes(SiftKeyMap *mm, const SiftCDict *d, MemoryContext cx)
{
	MemberCodes *mc = MemoryContextAlloc(cx, sizeof(MemberCodes));

	mc->n = d->n;
	mc->member = MemoryContextAlloc(cx, Max(d->n, 1) * sizeof(int64));
	mc->word = MemoryContextAlloc(cx, Max(d->n, 1) * sizeof(uint64));
	for (uint32 c = 0; c < d->n; c++)
	{
		uint32		len;
		const char *str = sift_cdict_str(d, c, &len);
		int64		code;

		mc->word[c] = sift_member_hash_bytes(str, len);
		mc->member[c] = sift_keymap_find_bytes(mm, str, len, &code) ? code : SIDE_NO_MEMBER;
	}
	return mc;
}

/* The strings of the member leaf of unit u as codes in r->tmp, and what the codes stand for: the segment's dictionary as members, or the unit's own */
MemberCodes *
side_unit_members(SiftSideBuild *b, uint32 u, const SiftCUnitView *v, const uint64 **nulls)
{
	SiftCDict  *dict;

	if (sift_side_unit_strs(b->seg, u, v, b->member, b->ucx, b->rows.tmp, nulls, &dict))
		return side_member_codes(b->mm, dict, b->ucx);
	return b->segmap;
}

bool
side_member_in(const SiftScopePair *m, uint64 n, int64 v)
{
	uint64		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if (m[mid].member < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && m[lo].member == v;
}

int
side_cmp_unit(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return (x > y) - (x < y);
}

int
side_cmp_word(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* The member filter of the open run: the distinct words of the members its units hold, SIDE_FILTER_BITS_PER_MEMBER bits each within SIDE_FILTER_BITS_PER_ROW for each row of the units; the filter is that of the units, so runs of other keys over the same units point at the same one */
void
side_put_filter(SiftSideBuild *b, SideRun *run)
{
	FilterAt	probe = {{0}},
			   *at;
	bool		found;
	uint64		rows = 0,
				nbits,
				cap;
	uint32		n = 0;
	uint64	   *bits;

	memcpy(probe.units, b->units, b->nunits * sizeof(uint32));
	at = hash_search(b->filter_at, &probe, HASH_ENTER, &found);
	if (found)
	{
		run->lo = at->lo;
		run->hi = at->nbits;
		run->pmax = 0;
		return;
	}
	for (uint32 i = 0; i < b->nunits; i++)
		rows += b->seg->sbdir[b->units[i]].nrows;
	qsort(b->words, b->nwords, sizeof(uint64), side_cmp_word);
	for (uint32 i = 0; i < b->nwords; i++)
		if (n == 0 || b->words[n - 1] != b->words[i])
			b->words[n++] = b->words[i];
	nbits = ((uint64) n * SIDE_FILTER_BITS_PER_MEMBER + 63) / 64 * 64;
	cap = (rows * SIDE_FILTER_BITS_PER_ROW + 63) / 64 * 64;
	nbits = Max(64, Min(nbits, cap));
	bits = palloc0(nbits / 8);
	for (uint32 i = 0; i < n; i++)
		sift_side_filter_add(bits, nbits, b->words[i]);
	run->lo = at->lo = side_put(b->sink, bits, nbits / 8);
	run->hi = at->nbits = nbits;
	run->pmax = 0;
	b->cnt->filters++;
	b->cnt->filter_bytes += nbits / 8;
	pfree(bits);
}

/*
 * The index of a string member: an entry for each member of the pairs in each unit that holds it, ascending (member, unit); *n entries, NULL when they would take more than
 * maintenance_work_mem. The member of an entry is -1 - its code in the map of the pairs.
 */
uint64 *
side_member_units(SiftSideBuild *b, uint64 *n)
{
	uint64		cap = (uint64) maintenance_work_mem * 1024 / sizeof(uint64),
				fill = 0,
				room = 1024;
	uint64	   *mu = palloc(room * sizeof(uint64));
	uint32	   *seen = palloc0(Max(b->segmap->n, 1) * sizeof(uint32));

	for (uint32 u = 0; u < b->seg->nsb; u++)
	{
		SiftCUnitView v;
		const uint64 *nulls;
		const MemberCodes *mc;
		uint32	   *stamp = seen;
		uint32		mark = u + 1;

		SIFT_INVARIANT(sift_cseg_unit_view(b->seg, u, &v), "the scope sidecar cannot parse a unit");
		mc = side_unit_members(b, u, &v, &nulls);
		if (mc != b->segmap)
		{
			stamp = MemoryContextAllocZero(b->ucx, Max(mc->n, 1) * sizeof(uint32));
			mark = 1;
		}
		if (fill + v.nrows > room)
		{
			room = Min(Max(room * 2, fill + v.nrows), cap);
			mu = repalloc(mu, room * sizeof(uint64));
		}
		for (uint32 i = 0; i < v.nrows; i++)
		{
			int64		code = b->rows.tmp[i];

			if ((nulls != NULL && sift_col_bit_test(nulls, i)) || stamp[code] == mark)
				continue;
			stamp[code] = mark;
			if (mc->member[code] == SIDE_NO_MEMBER)
				continue;
			if (fill == room)
				return NULL;
			mu[fill++] = (uint64) (-1 - mc->member[code]) << 32 | u;
		}
		if (mc != b->segmap)
			MemoryContextReset(b->ucx);
	}
	qsort(mu, fill, sizeof(uint64), side_cmp_word);
	*n = fill;
	return mu;
}

/* The units that hold a member of the pairs m[0..nm) (a key's, ascending), ascending, each once */
uint32 *
side_key_units(SiftSideBuild *b, const uint64 *mu, uint64 nmu, const SiftScopePair *m, uint64 nm, uint32 *n)
{
	uint32	   *us = b->key_us;
	uint32		k = 0;

	for (uint64 i = 0; i < nm; i++)
	{
		uint64		member = (uint64) (-1 - m[i].member),
					lo = 0,
					hi = nmu;

		while (lo < hi)
		{
			uint64		mid = lo + (hi - lo) / 2;

			if (MU_MEMBER(mu[mid]) < member)
				lo = mid + 1;
			else
				hi = mid;
		}
		for (; lo < nmu && MU_MEMBER(mu[lo]) == member; lo++)
		{
			uint32		u = (uint32) mu[lo];

			if (!sift_col_bit_test(b->key_seen, u))
			{
				sift_col_bit_set(b->key_seen, u);
				us[k++] = u;
			}
		}
	}
	qsort(us, k, sizeof(uint32), side_cmp_unit);
	for (uint32 i = 0; i < k; i++)
		b->key_seen[us[i] / 64] &= ~(UINT64CONST(1) << (us[i] % 64));
	*n = k;
	return us;
}

uint64
side_max_unit_rows(const SiftColSegment *seg)
{
	uint64		rows = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
		rows = Max(rows, seg->sbdir[u].nrows);
	return rows;
}
