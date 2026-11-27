/*
 * core/maint/jobs.c - the kinds of maintenance job (core/maint/sched.h): their names, tiers and urgencies, and the score a request of a tier ages to
 */
#include "core/maint/sched.h"

#include "datatype/timestamp.h"

static const struct
{
	uint32		floor;
	uint32		width;
}			tier_range[SIFT_TIERS] = {{10000, 20000}, {1000, 9000}, {200, 800}, {50, 150}};

static const struct
{
	const char *name;
	SiftPacerTier tier;
	uint32		urgency;
}			kinds[SIFT_MK_N] = {
	[SIFT_MK_QUARANTINE_MARK] = {"quarantine", SIFT_TIER_P0, 10000},
	[SIFT_MK_SLAB_FLUSH] = {"slab_flush", SIFT_TIER_P0, 0},
	[SIFT_MK_PEND_DEL_FOLD] = {"pend_del", SIFT_TIER_P0, 0},
	[SIFT_MK_COMPACT_PART] = {"compact_l1", SIFT_TIER_P1, 0},
	[SIFT_MK_SCOPE_REFRESH] = {"scope_refresh", SIFT_TIER_P1, 0},
	[SIFT_MK_BM25_MERGE] = {"bm25_merge", SIFT_TIER_P1, 0},
	[SIFT_MK_SPLIT_PART] = {"split", SIFT_TIER_P2, 0},
	[SIFT_MK_MERGE_PART] = {"merge", SIFT_TIER_P2, 0},
	[SIFT_MK_TAIL_SHRINK] = {"tail_shrink", SIFT_TIER_P3, 0},
};

SiftPacerTier
sift_maint_kind_tier(SiftMaintKind kind)
{
	return kinds[kind].tier;
}

uint32
sift_maint_kind_urgency(SiftMaintKind kind)
{
	return kinds[kind].urgency;
}

const char *
sift_maint_job_name(SiftMaintKind kind, SiftPacerTier tier)
{
	return kind == SIFT_MK_COMPACT_PART && tier >= SIFT_TIER_P2 ? "compact_l2" : kinds[kind].name;
}

const char *
sift_maint_phase_name(SiftMaintPhase phase)
{
	static const char *const names[] = {"queued", "scan", "write", "relocate", "truncate", "waiting_pace"};

	return names[phase];
}

SiftMaintKind
sift_maint_kind_of_name(const char *job)
{
	for (int k = 0; k < SIFT_MK_N; k++)
		if (strcmp(kinds[k].name, job) == 0)
			return (SiftMaintKind) k;
	return SIFT_MK_N;
}

uint32
sift_maint_score(SiftPacerTier tier, uint32 urgency)
{
	return tier_range[tier].floor + Min(urgency, tier_range[tier].width - 1);
}

uint32
sift_maint_aged_score(uint32 score, SiftPacerTier tier, TimestampTz enqueued_at, TimestampTz now)
{
	uint64		quanta;

	if (tier == SIFT_TIER_P0 || now <= enqueued_at)
		return score;
	quanta = (uint64) (now - enqueued_at) / SIFT_MAINT_AGE_QUANTUM_US;
	return (uint32) Max(score, Min((uint64) score + quanta * SIFT_MAINT_AGE_STEP, SIFT_MAINT_AGED_MAX));
}
