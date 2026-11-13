/*
 * col/scan/except.c - the stored rows of a scope source that the scan leaves out: those of the members whose pairs changed since the scope segments were written, and for a string member the
 * rows of a key's units that the member's least key counts (col/scan/scope_delta.c)
 */
#include "postgres.h"

#include "core/cmp.h"
#include "col/conn.h"
#include "col/seg.h"
#include "col/scan_int.h"

/* is key one of the members the source leaves out: a bit of the bitmap over their range, a bisection when the range is too wide for one */
static bool
sx_has(const SiftColSource *src, int64 key)
{
	uint64		off = (uint64) key - (uint64) src->sx[0];
	uint32		lo = 0,
				hi = src->nsx;

	if (src->sxb != NULL)
		return off < src->sx_span && sift_col_bit_test(src->sxb, off);
	SIFT_BISECT(uint32, m, lo, hi, src->sx[m] < key);
	return lo < src->nsx && src->sx[lo] == key;
}

/* the members whose rows the units of key do not count, NULL: all of them */
static const ColDrop *
drop_of(const SiftColSource *src, int64 key)
{
	for (int i = 0; i < src->nsd; i++)
		if (src->sd[i].key == key)
			return &src->sd[i];
	return NULL;
}

/* what a unit of a string member leaf is asked, for the codes of its dictionary: the answers are kept, one dictionary of a segment serves its units */
typedef struct StrUnit
{
	const SiftCDict *dict;
	uint8	   *has;			/* 1 + is the string of a member the source leaves out (0: not asked) */
	int64	   *code;			/* the code of the string in the map of the members (PG_INT64_MAX: not asked, PG_INT64_MIN: none) */
	const int64 *key;			/* the scope key of each row, NULL: that of the unit's only key, one */
	const ColDrop *one;
} StrUnit;

/*
 * Is the string of a code of the dictionary one the unit leaves out: its journal word is one of sx (a member of another string with the same word is left out too, and read again by the patch),
 * or the member is one the key's units do not count: that one is by the member's code in the map, which is the bytes' (sd_map: the members of the drop lists are the codes of the read).
 */
static bool
str_gone(SiftColSource *src, StrUnit *s, int64 code, uint32 r)
{
	const ColDrop *d = s->key != NULL ? drop_of(src, s->key[r]) : s->one;

	if (src->nsx > 0)
	{
		if (s->has[code] == 0)
		{
			uint32		len;
			const char *str = sift_cdict_str(s->dict, (uint32) code, &len);

			s->has[code] = 1 + sx_has(src, (int64) sift_member_hash_bytes(str, len));
		}
		if (s->has[code] == 2)
			return true;
	}
	if (d == NULL)
		return false;
	if (s->code[code] == PG_INT64_MAX)
	{
		uint32		len;
		const char *str = sift_cdict_str(s->dict, (uint32) code, &len);

		if (!sift_keymap_find_bytes(src->sd_map, str, len, &s->code[code]))
			s->code[code] = PG_INT64_MIN;
	}
	return bsearch(&s->code[code], d->members, d->n, sizeof(int64), sift_cmp_i64) != NULL;
}

/* the answers of the dictionary of a unit: those of its segment's dictionary last as long as the source (every unit of the segment asks them), a dictionary of the unit's own has its own */
static void
str_answers(ColUnit *cu, StrUnit *s)
{
	SiftColSource *src = cu->src;
	MemoryContext cx = unit_inline(cu, src->sx_leaf) ? src->arena : src->mcxt;

	if (cx == src->arena || src->sx_dict != s->dict)
	{
		uint8	   *has = MemoryContextAllocZero(cx, Max(s->dict->n, 1));
		int64	   *code = MemoryContextAlloc(cx, Max(s->dict->n, 1) * sizeof(int64));

		for (uint32 i = 0; i < s->dict->n; i++)
			code[i] = PG_INT64_MAX;
		if (cx == src->mcxt)
		{
			src->sx_dict = s->dict;
			src->sx_memo = has;
			src->sx_code = code;
		}
		s->has = has;
		s->code = code;
		return;
	}
	s->has = src->sx_memo;
	s->code = src->sx_code;
}

/*
 * The leaf of a string member of the unit as codes of its dictionary, with the keys to look the drop lists of: false when no row of the unit can be left out (no member of the source's is left out
 * and the unit's key has no drop list: its member leaf is not read)
 */
static bool
str_open(ColUnit *cu, SiftCChunk *c, StrUnit *s, int64 **keys)
{
	SiftColSource *src = cu->src;
	SiftCZone	z;
	SiftCChunk	kc;

	*s = (StrUnit) {0};
	*keys = NULL;
	if (src->nsd > 0)
	{
		sift_cseg_zone(cu->seg, src->sd_leaf, cu->u, &z);
		if (!(z.flags & SIFT_COLZ_OVERFLOW) && z.nvals > 0 && z.min == z.max && z.nnull == 0)
			s->one = drop_of(src, z.min);
		else if (unit_chunk_raw(cu, src->sd_leaf, &kc))
		{
			*keys = palloc((kc.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
			sift_cchunk_decode(&kc, *keys);
			s->key = *keys;
		}
	}
	if (src->nsx == 0 && s->one == NULL && s->key == NULL)
		return false;
	if (!unit_chunk(cu, src->sx_leaf, c))
		return false;
	s->dict = unit_dict(cu, src->sx_leaf);
	str_answers(cu, s);
	return true;
}

/*
 * The stored unit of a scope source (sift_col_scope_delta) whose rows include a member the source leaves out: those rows are dead to the scan, as if their liveness bit were clear (cu->live
 * is the selection of the others), and the unit answers from neither its pre-aggregates nor the net of its dead rows. The zone of the member leaf rules most units of an integer member out; a
 * string member has no zone to ask, and the leaf of every unit read is read.
 */
bool
unit_except(ColUnit *cu)
{
	SiftColSource *src = cu->src;
	SiftCZone	z;
	SiftCChunk	c;
	StrUnit		s = {0};
	int64	   *keys = NULL,
			   *val;
	uint32		n = cu->v.nrows,
				lo = 0,
				hi = src->nsx;
	bool		any = false;

	if (cu->seg == NULL)
		return false;
	if (src->sx_str)
	{
		if (!str_open(cu, &c, &s, &keys))
			return false;
	}
	else
	{
		sift_cseg_zone(cu->seg, src->sx_leaf, cu->u, &z);
		if (!(z.flags & SIFT_COLZ_OVERFLOW))
		{
			if (z.nvals == z.nnull)
				return false;
			SIFT_BISECT(uint32, m, lo, hi, src->sx[m] < z.min);
			if (lo == src->nsx || src->sx[lo] > z.max)
				return false;
		}
		if (!unit_chunk_raw(cu, src->sx_leaf, &c))
			return false;
	}
	val = palloc((c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
	sift_cchunk_decode(&c, val);
	for (uint32 r = 0; r < n; r++)
	{
		if ((c.nulls != NULL && sift_col_bit_test(c.nulls, r)) || !(src->sx_str ? str_gone(src, &s, val[r], r) : sx_has(src, val[r])))
			continue;
		if (!any)
		{
			cu->live = MemoryContextAllocZero(src->arena, SIFT_COL_NWORDS(n) * sizeof(uint64));
			sift_cseg_live(cu->seg, cu->u, cu->live);
			any = true;
		}
		cu->live[r / 64] &= ~(UINT64CONST(1) << (r % 64));
	}
	pfree(val);
	if (keys != NULL)
		pfree(keys);
	cu->excl = any;
	return any;
}
