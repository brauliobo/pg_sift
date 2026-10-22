/*
 * col/point.c - the point directory of a leaf in a segment (col/point.h). Portable (core/host.h only).
 */
#include "core/host.h"

#include "col/point.h"
#include "col/preagg.h"
#include "col/setsum.h"

static bool
leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return d->parent < 0 && sift_cset_leaf_ok(s, d);
}

const char *
sift_cpoint_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf)
{
	return sift_cpagg_leaf_list(opt, s, names, leaf, leaf_ok, "not a plain integer, text or uuid column of the top level");
}

const char *
sift_cpoint_syntax(const char *opt)
{
	return sift_cpagg_leaf_list(opt, NULL, NULL, NULL, NULL, NULL);
}

uint64
sift_cpoint_hash(int64 key)
{
	uint64		x = (uint64) key;

	x ^= x >> 33;
	x *= UINT64CONST(0xff51afd7ed558ccd);
	x ^= x >> 33;
	x *= UINT64CONST(0xc4ceb9fe1a85ec53);
	x ^= x >> 33;
	return x;
}

uint32
sift_cpoint_slots(uint32 n)
{
	uint64		s = 16;

	while (s * 3 < (uint64) n * 4)
		s *= 2;
	return (uint32) s;
}

bool
sift_cpoint_build(const int64 *key, const uint32 *row, uint32 n, uint32 nslots, StringInfo out)
{
	uint64	   *t = palloc(nslots * sizeof(uint64));

	memset(t, 0xFF, nslots * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
	{
		uint64		h = sift_cpoint_hash(key[i]);
		uint32		p = sift_cpoint_pos(h, nslots);
		uint32		run = 0;

		while (t[p] != SIFT_CPOINT_EMPTY)
		{
			if (++run > SIFT_CPOINT_MAX_RUN)
			{
				pfree(t);
				return false;
			}
			p = (p + 1) & (nslots - 1);
		}
		t[p] = (h & 0xFFFFFFFF) << 32 | row[i];
	}
	appendBinaryStringInfo(out, (const char *) t, (int) (nslots * sizeof(uint64)));
	pfree(t);
	return true;
}
