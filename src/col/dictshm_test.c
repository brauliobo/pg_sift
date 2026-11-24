/*
 * col/dictshm_test.c - the test hooks of the shared dictionary table: the pinned records and the lookups
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <sys/statvfs.h>
#include "common/hashfn.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/dsm_registry.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "agg/stats.h"
#include "col/dictshm_int.h"
#include "col/dictshm.h"
#include "col/dictshm_int.h"
#include "col/dsmreg.h"
#include "access/xact.h"
#include "agg/lookup_int.h"

/* The records some backend reads in place, and the pins on them (all backends) */
void
sift_dictshm_pinned(int64 *records, int64 *pins)
{
	Dshm	   *s = dshm_get();

	*records = *pins = 0;
	if (s == NULL)
		return;
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < lengthof(s->dir); i++)
		if (s->dir[i].state != E_FREE && s->dir[i].pins > 0)
		{
			(*records)++;
			*pins += s->dir[i].pins;
		}
	SpinLockRelease(&s->mutex);
}

PG_FUNCTION_INFO_V1(sift_test_col_dict_shared_hold);

/* sift._test_col_dict_shared_hold(n): the writes the ring still skips (a ring that threw out more than was found in it skips DSHM_HOLD of them) set to n; returns what it was */
Datum
sift_test_col_dict_shared_hold(PG_FUNCTION_ARGS)
{
	Dshm	   *s = dshm_get();
	uint32		was;

	if (s == NULL)
		PG_RETURN_NULL();
	SpinLockAcquire(&s->mutex);
	was = s->hold;
	s->hold = PG_GETARG_INT32(0);
	s->writes = s->thrown = s->found = 0;
	SpinLockRelease(&s->mutex);
	PG_RETURN_INT32(was);
}

PG_FUNCTION_INFO_V1(sift_test_col_dict_shared_stale);

/* What is wrong with the index of places of the ring (NULL when nothing): by place, no overlap, each entry once, every entry that holds a place in it, the live ones within the ring */
static const char *
ring_problem(const Dshm *s)
{
	bool		held[lengthof(s->dir)] = {false};
	Size		live = 0,
				end = 0;

	for (int i = 0; i < s->nord; i++)
	{
		const DshmEnt *e = &s->dir[s->ord[i]];

		if (held[s->ord[i]])
			return "an entry is in the index of places twice";
		held[s->ord[i]] = true;
		if (e->off < end)
			return "the index of places is not by place, or two of its entries overlap";
		end = e->off + e->len;
	}
	for (int i = 0; i < lengthof(s->dir); i++)
	{
		if (s->dir[i].state != E_FREE && !held[i])
			return "an entry that holds a place is not in the index of places";
		live += s->dir[i].state == E_LIVE ? s->dir[i].len : 0;
	}
	return live > s->size ? "the live records are more than the ring" : NULL;
}

/* 6,000 strings of 64 bytes that start with c: two such dictionaries have the same record size, and the strings tell them apart */
static SiftCDict *
stale_dict(char c)
{
	SiftCDict  *d = sift_cdict_create(CurrentMemoryContext);

	for (int i = 0; i < 6000; i++)
	{
		char		str[65];

		snprintf(str, sizeof(str), "%c%063d", c, i);
		sift_cdict_intern(d, str, 64);
	}
	return d;
}

/*
 * sift._test_col_dict_shared_stale(): the two ways a reader could be handed the record of another key. A place that a writer is copying into is not given to a
 * second writer (a ring of 2.5 records refuses the third while two are being copied in), and a directory entry that names a place that holds the record of
 * another key (forged here) is a miss for the reader, which gives its pin back. Returns the first letter of the dictionary the reader of the first key gets,
 * NULL when it gets none. The ring is made small for the test and put back, empty. A record kept past the small ring, in the bucket of the forged entry, is the
 * history of a server: the forged entry takes its slot out of the index of places, which stays right for the tests that follow.
 */
Datum
sift_test_col_dict_shared_stale(PG_FUNCTION_ARGS)
{
	Dshm	   *s = dshm_get();
	DictKey		ka,
				kb,
				kc;
	SiftCDict  *dc = stale_dict('c'),
			   *got;
	Size		len = layout(dc->n, dc->nbytes, dc->slots != NULL && ((Size) dc->n + 1) * 2 <= dc->nslots ? F_SLOTS : 0, dc->nslots).total,
				size,
				off[3];
	int			flight[3],
				evicted = 0;
	int64		records,
				npins;
	bool		third,
				wrote,
				ready;
	const char *problem;
	char		res[2] = {0, 0};

	if (s == NULL)
		PG_RETURN_NULL();
	/* the ring is made at the size of the directory before the test makes the directory believe in a small one */
	ready = ring_get(s);
	SIFT_INVARIANT(ready, "the ring of the shared dictionaries is not there");
	memset(&ka, 0, sizeof(ka));
	kc = ka;
	ka.gen = 1;
	kc.gen = 2;
	for (kb = ka, kb.gen = 3; bucket(s, &kb) != bucket(s, &ka); kb.gen++)
		;
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < lengthof(s->dir); i++)
		s->dir[i].state = E_FREE;
	s->head = TYPEALIGN(DSHM_ALIGN, len * 3);
	s->hold = 0;
	SpinLockRelease(&s->mutex);
	wrote = sift_dictshm_write(&kb, dc);
	SIFT_INVARIANT(wrote, "the record of a place past the small ring was not written");
	SpinLockAcquire(&s->mutex);
	size = s->size;
	s->size = len * 5 / 2;
	s->head = 0;
	s->hold = 0;
	for (int i = 0; i < lengthof(s->dir); i++)
		s->dir[i].state = E_FREE;
	ring_take(s, len, &off[0], &flight[0], &evicted);
	ring_take(s, len, &off[1], &flight[1], &evicted);
	third = ring_take(s, len, &off[2], &flight[2], &evicted);
	s->flight[flight[0]].len = s->flight[flight[1]].len = 0;
	s->head = 0;
	SpinLockRelease(&s->mutex);
	SIFT_INVARIANT(!third && off[0] != off[1], "a place being copied into was given to another writer: %zu, %zu", off[0], off[1]);

	wrote = sift_dictshm_write(&kc, dc);
	SIFT_INVARIANT(wrote, "the record of the test was not written");
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < lengthof(s->dir); i++)
		if (s->dir[i].state == E_LIVE)
		{
			DshmEnt    *b = bucket(s, &ka);

			ord_drop(s, (int) (b - s->dir));
			b[0] = s->dir[i];
			b[0].key = ka;
			break;
		}
	SpinLockRelease(&s->mutex);
	got = sift_dictshm_read(&ka, CurrentMemoryContext);
	if (got != NULL)
		res[0] = got->bytes[got->off[0]];
	else
	{
		sift_dictshm_pinned(&records, &npins);
		SIFT_INVARIANT(npins == 0, "the reader of a stale entry kept its pin: %ld", (long) npins);
	}

	SpinLockAcquire(&s->mutex);
	s->size = size;
	s->head = 0;
	for (int i = 0; i < lengthof(s->dir); i++)
		s->dir[i].state = E_FREE;
	problem = ring_problem(s);
	SpinLockRelease(&s->mutex);
	SIFT_INVARIANT(problem == NULL, "the test left the ring of the shared dictionaries wrong: %s", problem);
	if (got == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(res));
}

PG_FUNCTION_INFO_V1(sift_test_dsm_deferred);

/* sift._test_dsm_deferred(): the shared tables this backend went without because a statement was writing, and how many times it asked (col/dsmreg.c) */
Datum
sift_test_dsm_deferred(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < SIFT_DSM_DEFERRED_MAX && sift_dsm_deferred[i].n > 0; i++)
	{
		Datum		v[2] = {CStringGetTextDatum(sift_dsm_deferred[i].name), Int64GetDatum(sift_dsm_deferred[i].n)};
		bool		nulls[2] = {false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(sift_test_dsm_register);
PG_FUNCTION_INFO_V1(sift_test_dsm_join);

static void
dsm_test_init(SIFT_DSM_INIT_ARGS)
{
}

/* sift._test_dsm_register(name, size): a registry entry exactly as an older release left it (its own name and size, none added by sift_dsm_join) */
Datum
sift_test_dsm_register(PG_FUNCTION_ARGS)
{
	bool		found;

#if PG_VERSION_NUM >= 190000
	GetNamedDSMSegment(text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_INT64(1), dsm_test_init, &found, NULL);
#else
	GetNamedDSMSegment(text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_INT64(1), dsm_test_init, &found);
#endif
	PG_RETURN_VOID();
}

/* sift._test_dsm_join(name, size): the table of the code of this release (col/dsmreg.c) */
Datum
sift_test_dsm_join(PG_FUNCTION_ARGS)
{
	sift_dsm_join(text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_INT64(1), dsm_test_init);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(sift_test_dsm_caller);

/*
 * sift._test_dsm_caller(caller, parallel): whether the table of a caller of the registry could be had by a backend, as that caller asks for it (inside a parallel operation, where no
 * subtransaction can start, when parallel). Never an error, which is the point: lookup_rows and lookup_dim (the pools of the lookups, agg/lookshm.c), dictionary (the directory of
 * the dictionaries), dictionary_ring (its ring) and dictionary_worker (what a parallel worker joins).
 */
Datum
sift_test_dsm_caller(PG_FUNCTION_ARGS)
{
	char	   *caller = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		parallel = PG_GETARG_BOOL(1),
				got = false;
	Dshm	   *s;

	if (parallel)
		EnterParallelMode();
	if (strcmp(caller, "lookup_rows") == 0)
		got = lookshm_bytes(LOOK_ROWS) > 0;
	else if (strcmp(caller, "lookup_dim") == 0)
		got = lookshm_bytes(LOOK_DIM) > 0;
	else if (strcmp(caller, "dictionary") == 0)
		got = dshm_get() != NULL;
	else if (strcmp(caller, "dictionary_ring") == 0)
		got = (s = dshm_get()) != NULL && ring_get(s);
	else if (strcmp(caller, "dictionary_worker") == 0)
	{
		sift_dictshm_join();
		got = dshm_get() != NULL;
	}
	else
		elog(ERROR, "unknown caller %s", caller);
	if (parallel)
		ExitParallelMode();
	PG_RETURN_BOOL(got);
}

PG_FUNCTION_INFO_V1(sift_test_dsm_layout);
PG_FUNCTION_INFO_V1(sift_test_dsm_attach);
PG_FUNCTION_INFO_V1(sift_test_dsm_magic);
PG_FUNCTION_INFO_V1(sift_test_dsm_entries);
PG_FUNCTION_INFO_V1(sift_test_dsm_alive);

typedef struct LayoutProbe
{
	int32		stamp;			/* the layout that made the table */
	int32		pad;
	int64		calls;			/* the calls that reached the table, from every backend */
} LayoutProbe;

static int32 layout_probe_stamp;

static void
layout_probe_init(SIFT_DSM_INIT_ARGS)
{
	LayoutProbe *p = ptr;

	memset(p, 0, sizeof(LayoutProbe));
	p->stamp = layout_probe_stamp;
}

/*
 * sift._test_dsm_layout(layout, name, size): the table `name` as the library of layout number `layout` asks for it, a library whose struct has the same size
 * as another's and a different layout. The result is stamp * 1000000 + calls: the stamp is the layout that made the table, calls count the calls of every
 * backend that got the same table, so two layouts that share one table read each other's stamp.
 */
Datum
sift_test_dsm_layout(PG_FUNCTION_ARGS)
{
	LayoutProbe *p;

	layout_probe_stamp = PG_GETARG_INT32(0);
	snprintf(sift_dsm_test_build, sizeof(sift_dsm_test_build), "layout%d", layout_probe_stamp);
	p = sift_dsm_join(text_to_cstring(PG_GETARG_TEXT_PP(1)), PG_GETARG_INT64(2), layout_probe_init);
	sift_dsm_test_build[0] = '\0';
	PG_RETURN_INT64((int64) p->stamp * 1000000 + ++p->calls);
}

/* sift._test_dsm_attach(name, size): whether a backend that goes without a table where the directory cannot hold it got it (sift_dsm_attach: a cache never fails a statement) */
Datum
sift_test_dsm_attach(PG_FUNCTION_ARGS)
{
	bool		failed = false;

	PG_RETURN_BOOL(sift_dsm_attach(text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_INT64(1), dsm_test_init, &failed, "the test tables", NULL, false) != NULL);
}

/* sift._test_dsm_magic(magic): the magic of the directory of the shared tables set to magic, as a directory of another library has it; the one it had */
Datum
sift_test_dsm_magic(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_dsm_test_magic((uint32) PG_GETARG_INT64(0)));
}

/* sift._test_dsm_entries(): the tables the directory keeps, and how many backends map each */
Datum
sift_test_dsm_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const char *name, *build;
	uint64		size;
	dsm_handle	handle;
	uint32		users;

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; sift_dsm_entry(i, &name, &build, &size, &handle, &users); i++)
	{
		Datum		v[5] = {CStringGetTextDatum(name), CStringGetTextDatum(build), Int64GetDatum(size), Int64GetDatum(handle), Int32GetDatum(users)};
		bool		n[5] = {false};

		if (handle != DSM_HANDLE_INVALID)
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
	}
	return (Datum) 0;
}

/* sift._test_dsm_alive(handle): the segment of a table is still there (a segment that nobody pins or maps is gone) */
Datum
sift_test_dsm_alive(PG_FUNCTION_ARGS)
{
	dsm_segment *seg = dsm_attach((dsm_handle) PG_GETARG_INT64(0));

	if (seg != NULL)
		dsm_detach(seg);
	PG_RETURN_BOOL(seg != NULL);
}

#endif							/* SIFT_TESTING */
