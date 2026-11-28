/*
 * col/f2/scan/bridge_core.c - the decisions a scan with a TID bridge takes before it reads a column (col/f2/scan/bridge_core.h).
 *
 * check-memory: allow-allocs the words of one granule's mask
 */
#include "core/host.h"

#include "col/f2/scan/bridge_core.h"

bool
sift_f2_bridge_part_excluded(const SiftTidBridge *br, uint64 part_tid_min, uint64 part_tid_max, bool remapped)
{
	return !remapped && part_tid_max != 0 && (part_tid_max < br->min_tid || part_tid_min > br->max_tid);
}

bool
sift_f2_tid_range_excludes(uint64 lo, uint64 hi, bool remapped, uint64 tid_min, uint64 tid_max)
{
	if (remapped || tid_max == 0)
		return false;
	return lo >= hi || (tid_max >> 16) < (lo >> 16) || (tid_min >> 16) > ((hi - 1) >> 16);
}

bool
sift_f2_bridge_granule_excluded(const SiftTidBridge *br, bool have_tix, bool remapped, uint64 tid_min, uint64 tid_max, uint64 blk_bloom)
{
	return have_tix && !remapped && !sift_tidbridge_granule_may_match(br, tid_min, tid_max, blk_bloom);
}

int
sift_f2_bridge_probe_rows(const SiftTidBridge *br, const uint64 *tids, int n, const uint64 *dead, SiftBridgeDeadFn pend_dead, void *pend_arg, uint64 *active, float4 *scores)
{
	int			nw = (n + 63) / 64,
				kept;
	uint64	   *out = palloc(Max(nw, 1) * sizeof(uint64));

	for (int w = 0; w < nw; w++)
	{
		if (dead != NULL)
			active[w] &= ~dead[w];
		if (pend_dead != NULL)
			for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
			{
				int			r = w * 64 + __builtin_ctzll(bits);

				if (pend_dead(pend_arg, tids[r]))
					active[w] &= ~(UINT64CONST(1) << (r & 63));
			}
	}
	kept = sift_tidbridge_probe_vec(br, tids, active, n, out, scores);
	memcpy(active, out, nw * sizeof(uint64));
	pfree(out);
	return kept;
}

int
sift_f2_bridge_slab_rows(const SiftTidBridge *br, const uint64 *tids, int n, uint64 *mask, float4 *scores)
{
	return sift_tidbridge_probe_vec(br, tids, NULL, n, mask, scores);
}
