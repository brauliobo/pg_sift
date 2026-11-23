/*
 * trapshim.c - the wrappers of trapshim.h: the linker (-Wl,--wrap=NAME for each NAME of trapped.def) sends the calls of every object but pgshim's to them.
 */
#include "postgres.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "lib/stringinfo.h"
#include "trapshim.h"

_Thread_local bool sift_test_in_worker;
bool		sift_test_trap_soft;
static _Atomic uint64 traps;

uint64
sift_test_trap_count(void)
{
	return atomic_load(&traps);
}

static void
trap(const char *what)
{
	if (!sift_test_in_worker)
		return;
	atomic_fetch_add(&traps, 1);
	if (sift_test_trap_soft)
		return;
	fprintf(stderr, "TRAP: %s called from a worker thread: a worker allocates from its arena and reports through SiftErr\n", what);
	abort();
}

#define TRAP(ret, name, params, args) \
	extern ret __real_##name params; \
	ret __wrap_##name params { trap(#name); return __real_##name args; }
#define TRAPV(ret, name, params, args) \
	extern ret __real_##name params; \
	ret __wrap_##name params { trap(#name); __real_##name args; }
#include "trapped.def"

void
__wrap_appendStringInfo(StringInfo s, const char *fmt,...)
{
	trap("appendStringInfo");
}
