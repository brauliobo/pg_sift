/*
 * core/maint/pacer_test.c - test-only SQL functions of core/maint/pacer.h, compiled only with SIFT_TESTING and registered by the pg_sift_test extension: sift._test_pacer_decide runs the law
 * on a load, sift._test_pacer_starve and sift._test_pacer_yield run the priority of the tiers at a time of the caller's choosing, sift._test_pacer_foreground feeds the averages of the foreground,
 * sift._test_pacer_snapshot reads what a worker last sampled and sift._test_pacer_reset empties the telemetry. No clock and no sleep: a test names the times.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "utils/builtins.h"

#include "core/maint/pacer.h"

PG_FUNCTION_INFO_V1(sift_test_pacer_decide);
PG_FUNCTION_INFO_V1(sift_test_pacer_starve);
PG_FUNCTION_INFO_V1(sift_test_pacer_yield);
PG_FUNCTION_INFO_V1(sift_test_pacer_foreground);
PG_FUNCTION_INFO_V1(sift_test_pacer_reset);
PG_FUNCTION_INFO_V1(sift_test_pacer_snapshot);

static const char *const mode_names[] = {"FULL", "MODERATE", "THROTTLED", "DEFERRED"};

/* sift._test_pacer_decide(active, slots, ewma_us, base_us, yields, base_delay_ms): "MODE share=pm delays=P0/P1/P2/P3" (-1: the tier does not start) */
Datum
sift_test_pacer_decide(PG_FUNCTION_ARGS)
{
	SiftPaceLoad load = {.active_oltp = PG_GETARG_INT32(0),.cpu_slots = PG_GETARG_INT32(1),.lat_ewma_us = PG_GETARG_INT64(2),
	.lat_base_us = PG_GETARG_INT64(3),.lock_yields = PG_GETARG_INT32(4)};
	int			base = PG_GETARG_INT32(5);
	SiftPaceDecision d = sift_pacer_decide(&load);

	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%s share=%d delays=%d/%d/%d/%d", mode_names[d.mode], d.share_pm,
											  sift_pacer_delay_ms(&d, SIFT_TIER_P0, base), sift_pacer_delay_ms(&d, SIFT_TIER_P1, base),
											  sift_pacer_delay_ms(&d, SIFT_TIER_P2, base), sift_pacer_delay_ms(&d, SIFT_TIER_P3, base))));
}

/* sift._test_pacer_starve(tier, at_us): a request of tier found no free worker at at_us */
Datum
sift_test_pacer_starve(PG_FUNCTION_ARGS)
{
	sift_pacer_note_starved(PG_GETARG_INT32(0), PG_GETARG_INT64(1));
	PG_RETURN_VOID();
}

/* sift._test_pacer_yield(tier, at_us): does a worker of tier leave after its step at at_us? */
Datum
sift_test_pacer_yield(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(sift_pacer_should_yield(PG_GETARG_INT32(0), PG_GETARG_INT64(1)));
}

/* sift._test_pacer_foreground(usecs, scans): scans of usecs each finish; "ewma base" of the telemetry after them (one scan in 64 counts: a test gives multiples of 64) */
Datum
sift_test_pacer_foreground(PG_FUNCTION_ARGS)
{
	uint64		ewma,
				base;

	for (int64 i = 0; i < PG_GETARG_INT64(1); i++)
		sift_pacer_note_foreground(PG_GETARG_INT64(0));
	sift_pacer_test_state(&ewma, &base);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%llu %llu", (unsigned long long) ewma, (unsigned long long) base)));
}

/* sift._test_pacer_snapshot(): "MODE active=backends delay=ms", what a worker last sampled (FULL, 0 and 0 before the first look) */
Datum
sift_test_pacer_snapshot(PG_FUNCTION_ARGS)
{
	SiftPacerSnapshot s;

	sift_pacer_snapshot(&s);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%s active=%u delay=%d", mode_names[s.mode], s.active_oltp, s.delay_ms)));
}

Datum
sift_test_pacer_reset(PG_FUNCTION_ARGS)
{
	sift_pacer_test_reset();
	PG_RETURN_VOID();
}

#endif
