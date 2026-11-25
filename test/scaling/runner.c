/*
 * test/scaling/check_scaling_c.c - the scaling gate of the C data structures (scripts/check-scaling-ops.sh).
 *
 * Each case runs the real code at n and at factor * n and compares the operation counts (op_counters.h), never a time. A ratio above the ceiling of the
 * complexity class (O(1) 1.25, O(log n) 1.5, O(n) 9.0, O(n log n) 11.5 at factor 8) fails. A gate marked pending names the lane that fixes the code: it
 * must still be above its ceiling (the same two-sided ratchet as scripts/complexity.allow), so that the marker goes with the fix.
 *
 *   check_scaling_c               every case
 *   check_scaling_c NAME          one case
 *   check_scaling_c --list        the cases
 *   check_scaling_c --negative-test   a planted O(n^2) loop and a stale pending marker must fail the gate
 */
#include <stdio.h>
#include <string.h>

#include "cases.h"
#include "op_counters.h"

static int
judge(const ScalingCase *c, const OpCounts *small, const OpCounts *big, int *pending)
{
	int			bad = 0;

	for (int i = 0; i < c->ngates; i++)
	{
		const OpGate *g = &c->gates[i];
		double		r = op_ratio(small, big, g->kind);
		bool		over = r > g->max_ratio;
		const char *verdict = g->pending ? (over ? "PENDING" : "STALE") : (over ? "FAIL" : "PASS");

		printf("%-7s %-16s %-11s %12llu -> %-12llu ratio %7.2f %s %5.2f%s%s\n", verdict, c->name, op_kind_name(g->kind),
			   (unsigned long long) small->v[g->kind], (unsigned long long) big->v[g->kind], r, over ? ">" : "<=", g->max_ratio,
			   g->pending ? " pending " : "", g->pending ? g->pending : "");
		*pending += g->pending && over;
		bad += g->pending ? !over : over;
	}
	return bad;
}

static int
run_case(const ScalingCase *c, int *pending)
{
	OpCounts	small = op_measure(c->run, c->n_small);
	OpCounts	big = op_measure(c->run, c->n_small * c->factor);

	return judge(c, &small, &big, pending);
}

static int
negative_test(void)
{
	const ScalingCase *planted[] = {&case_planted_quadratic, &case_planted_stale};
	const char *what[] = {"the planted O(n^2) loop", "the stale pending marker"};
	int			rc = 0;

	for (int i = 0; i < 2; i++)
	{
		int			pending = 0;
		int			bad = run_case(planted[i], &pending);

		printf(bad ? "negative-test: ok, %s failed the gate (%d gate(s))\n" : "negative-test: FAIL, %s passed the gate\n", what[i], bad);
		rc |= bad == 0;
	}
	return rc;
}

int
main(int argc, char **argv)
{
	int			bad = 0,
				pending = 0,
				ran = 0,
				gates = 0;

	if (argc > 1 && strcmp(argv[1], "--negative-test") == 0)
		return negative_test();
	for (int i = 0; i < ncases; i++)
	{
		if (argc > 1 && strcmp(argv[1], "--list") == 0)
			printf("%-16s %s\n", cases[i]->name, cases[i]->under_test);
		else if (argc <= 1 || strcmp(argv[1], cases[i]->name) == 0)
		{
			bad += run_case(cases[i], &pending);
			gates += cases[i]->ngates;
			ran++;
		}
	}
	if (argc > 1 && strcmp(argv[1], "--list") == 0)
		return 0;
	if (ran == 0)
	{
		fprintf(stderr, "check_scaling_c: no case %s\n", argv[1]);
		return 2;
	}
	printf("check_scaling_c: %d cases, %d gates, %d over a ceiling that has no pending marker or under one that has, %d pending\n", ran, gates, bad, pending);
	return bad != 0;
}
