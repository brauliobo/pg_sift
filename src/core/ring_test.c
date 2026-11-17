/*
 * core/ring_test.c - test-only SQL functions of core/ring.h, compiled only with SIFT_TESTING and registered by the pg_sift_test extension: sift._test_ring_start puts the count of every
 * ring a few slots from the wrap of 32 bits, sift._test_ring_sites tells which rings took slots across it, and sift._test_ring_walk takes slots of a ring of its own across the wrap.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "core/ring.h"

PG_FUNCTION_INFO_V1(sift_test_ring_start);
PG_FUNCTION_INFO_V1(sift_test_ring_sites);
PG_FUNCTION_INFO_V1(sift_test_ring_walk);

#define RING_SITES 32

uint32_t	sift_ring_test_start;

static struct
{
	const char *site;
	uint64		takes;
	uint64		wraps;
	uint32_t	last;
}			sites[RING_SITES];
static int	nsites;

void
sift_ring_test_note(const char *site, uint32_t at)
{
	int			i = 0;

	while (i < nsites && strcmp(sites[i].site, site) != 0)
		i++;
	if (i == nsites)
	{
		if (nsites == RING_SITES)
			elog(ERROR, "more rings than the %d of core/ring_test.c", RING_SITES);
		sites[nsites++].site = site;
	}
	sites[i].wraps += sites[i].takes > 0 && at < sites[i].last;
	sites[i].takes++;
	sites[i].last = at;
}

/* sift._test_ring_start(start int8): the count every ring begins at from now on, and the table of sift._test_ring_sites emptied */
Datum
sift_test_ring_start(PG_FUNCTION_ARGS)
{
	sift_ring_test_start = (uint32_t) PG_GETARG_INT64(0);
	memset(sites, 0, sizeof(sites));
	nsites = 0;
	PG_RETURN_VOID();
}

/* sift._test_ring_sites() returns (site text, takes int8, wraps int8): the rings that handed out slots since sift._test_ring_start, and how many times each one's count wrapped */
Datum
sift_test_ring_sites(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	desc;
	Tuplestorestate *store;
	MemoryContext old;

	InitMaterializedSRF(fcinfo, 0);
	desc = rsi->setDesc;
	store = rsi->setResult;
	old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	for (int i = 0; i < nsites; i++)
	{
		Datum		v[3] = {CStringGetTextDatum(sites[i].site), Int64GetDatum((int64) sites[i].takes), Int64GetDatum((int64) sites[i].wraps)};
		bool		n[3] = {false, false, false};

		tuplestore_putvalues(store, desc, v, n);
	}
	MemoryContextSwitchTo(old);
	return (Datum) 0;
}

/*
 * sift._test_ring_walk(slots int4, steps int4) returns bool: a ring of that many slots (a power of two) taken steps times from the count 2^31 - steps / 2 and again from 2^32 - steps / 2:
 * every slot below the number of slots, and each one the successor of the one before it modulo the number of slots.
 */
Datum
sift_test_ring_walk(PG_FUNCTION_ARGS)
{
	uint32_t	slots = (uint32_t) PG_GETARG_INT32(0),
				steps = (uint32_t) PG_GETARG_INT32(1),
				from[2] = {UINT32_C(0x80000000) - steps / 2, UINT32_C(0) - steps / 2};
	bool		ok = true;
	uint32_t	start = sift_ring_test_start;

	if (slots == 0 || (slots & (slots - 1)) != 0 || steps > 10000000)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sift._test_ring_walk: slots must be a power of two and steps at most 10000000")));
	sift_ring_test_start = 0;
	for (int f = 0; f < 2; f++)
	{
		SiftRing	ring = {from[f]};
		uint32_t	want = from[f] & (slots - 1);

		for (uint32_t i = 0; i < steps; i++)
		{
			uint32_t	got = sift_ring_take(&ring, slots - 1, "walk");

			ok &= got < slots && got == want;
			want = (want + 1) & (slots - 1);
		}
	}
	sift_ring_test_start = start;
	PG_RETURN_BOOL(ok);
}

#endif							/* SIFT_TESTING */
