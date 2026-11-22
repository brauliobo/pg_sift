/*
 * tidbridge_test.c - the checks of src/core/tidbridge_test.c (the TID bridge of src/core/tidbridge.h against an oracle of sorted keys) on the SQLite host, in a program of its
 * own: no database, so the same build runs under ASan and UBSan. Every failure count must be 0 and every check count not.
 *   make tidbridge-test            make tidbridge-test-asan
 */
#include <stdio.h>
#include <stdlib.h>

#include "core/host.h"

#include "core/tidbridge_test.c"

static const char *const names[] = {
	"layout", "kinds", "nomiss", "exact", "masks", "filter", "budget", "order", "scores", "rowloc", "docids", "point", "reset", "intersect"
};

_Static_assert(lengthof(names) == T_N, "a name for every check of src/core/tidbridge_test.c");

int
main(int argc, char **argv)
{
	int			rounds = argc > 1 ? atoi(argv[1]) : 6,
				bad = 0;

	CurrentMemoryContext = AllocSetContextCreate(NULL, "tidbridge-test", ALLOCSET_DEFAULT_SIZES);
	for (int seed = 1; seed <= 4; seed++)
	{
		int64_t		c[2 * T_N];

		sift_tidbridge_selftest(seed, rounds, c);
		for (int i = 0; i < T_N; i++)
		{
			printf("seed %d %-9s checks %9lld failed %lld\n", seed, names[i], (long long) c[2 * i], (long long) c[2 * i + 1]);
			bad |= c[2 * i + 1] != 0 || c[2 * i] == 0;
		}
	}
	puts(bad ? "tidbridge-test: FAILED" : "tidbridge-test: ok");
	return bad;
}
