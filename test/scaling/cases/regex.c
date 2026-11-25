/*
 * test/scaling/cases/regex.c - sqlite/host/regex*.c: the regular expression engine of the SQLite and MariaDB hosts on the patterns that make a backtracking matcher take exponential time
 * (nested repetition, an alternation under a star): the memo of the engine keeps the work polynomial in the length of the text.
 */
#include "../sqlite/host/host.h"

#include <stdio.h>
#include <stdlib.h>

#include "op_counters.h"

static void
match(const char *pattern, uint32_t len, char fill, int want)
{
	pg_wchar   *pat = malloc(sizeof(pg_wchar) * (strlen(pattern) + 1));
	pg_wchar   *text = malloc(sizeof(pg_wchar) * len);
	regex_t		re;
	size_t		plen = strlen(pattern);

	for (size_t i = 0; i < plen; i++)
		pat[i] = (unsigned char) pattern[i];
	for (uint32_t i = 0; i < len; i++)
		text[i] = (unsigned char) fill;
	int			rc = pg_regcomp(&re, pat, plen, REG_ADVANCED, C_COLLATION_OID);

	if (rc != REG_OKAY)
	{
		fprintf(stderr, "regex case: %s: compile error %d\n", pattern, rc);
		abort();
	}
	OP_BEGIN();
	rc = pg_regexec(&re, text, len, 0, NULL, 0, NULL, 0);
	OP_END();
	if (rc != want)
	{
		fprintf(stderr, "regex case: %s on %u chars: pg_regexec %d, want %d\n", pattern, len, rc, want);
		abort();
	}
	pg_regfree(&re);
	free(pat);
	free(text);
}

static void
run_regex(uint32_t len)
{
	match("(a+)+b", len, 'a', REG_NOMATCH);
	match("^(?:foo|fo|o)*z$", len, 'o', REG_NOMATCH);
}

static void
run_lookahead(uint32_t len)
{
	match("(?=(a+)+b)", len, 'a', REG_NOMATCH);
}

const ScalingCase case_regex = {
	.name = "regex", .under_test = "sqlite/host/regex*.c, nested repetition and lookahead", .n_small = 32, .factor = 8, .run = run_regex,
	GATES({OP_STEPS, 12.0, NULL}),
};

/* one doubling, not eight: the matcher takes exponential time here, and the step budget of an execution (5 million) is reached at 32 characters */
const ScalingCase case_lookahead = {
	.name = "regex_lookahead", .under_test = "sqlite/host/regex*.c, a lookahead around a nested repetition", .n_small = 8, .factor = 2, .run = run_lookahead,
	GATES({OP_STEPS, 2.5, "Spec 66 #7: memoized lookaround (63F)"}),
};
