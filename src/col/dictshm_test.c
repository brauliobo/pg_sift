/*
 * col/dictshm_test.c - the test hooks of the shared dictionary table: the pinned records and the lookups
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <sys/statvfs.h>
#include "common/hashfn.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/memutils.h"
#include "agg/stats.h"
#include "col/dictshm_int.h"
#include "col/dictshm.h"
#include "col/dictshm_int.h"
#include "col/dsmreg.h"

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
 * NULL when it gets none. The ring is made small for the test and put back, empty.
 */
Datum
sift_test_col_dict_shared_stale(PG_FUNCTION_ARGS)
{
	Dshm	   *s = dshm_get();
	DictKey		ka,
				kc;
	SiftCDict  *dc = stale_dict('c'),
			   *got;
	Size		len = layout(dc->n, dc->nbytes, dc->slots != NULL && ((Size) dc->n + 1) * 2 <= dc->nslots ? F_SLOTS : 0, dc->nslots).total,
				size,
				off[3];
	int			flight[3];
	int64		records,
				npins;
	bool		third,
				wrote,
				ready;
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
	SpinLockAcquire(&s->mutex);
	size = s->size;
	s->size = len * 5 / 2;
	s->head = 0;
	s->hold = 0;
	for (int i = 0; i < lengthof(s->dir); i++)
		s->dir[i].state = E_FREE;
	ring_take(s, len, &off[0], &flight[0]);
	ring_take(s, len, &off[1], &flight[1]);
	third = ring_take(s, len, &off[2], &flight[2]);
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
	SpinLockRelease(&s->mutex);
	if (got == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(res));
}

#endif							/* SIFT_TESTING */
