/*
 * bm25/hitspill.c - the sort of the hits of an unbounded ranked search (bm25/hitspill.h) on PostgreSQL's tuplesort.
 *
 * A hit is a bytea of 12 bytes that sorts bytewise as the hit does: the score as a float that compares as an unsigned integer, complemented so that the best sorts first, then the
 * key, big-endian, so that equal scores go by ascending key as the ranker orders them.
 */
#include "postgres.h"

#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "port/pg_bswap.h"
#include "utils/tuplesort.h"

#include "bm25/hitspill.h"

#define HIT_BYTES	(sizeof(uint32) + sizeof(uint64))

struct SiftHitSpill
{
	Tuplesortstate *sort;
};

SiftHitSpill *
sift_hitspill_begin(MemoryContext mcxt, int work_mem_kb)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftHitSpill *s = palloc(sizeof(SiftHitSpill));

	s->sort = tuplesort_begin_datum(BYTEAOID, ByteaLessOperator, InvalidOid, false, work_mem_kb, NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(old);
	return s;
}

static inline uint32
score_order(float4 score)
{
	uint32		bits;

	memcpy(&bits, &score, sizeof(bits));
	bits = (bits & 0x80000000) ? ~bits : bits | 0x80000000;
	return ~bits;
}

void
sift_hitspill_put(SiftHitSpill *s, SiftKey key, float4 score)
{
	union
	{
		struct varlena v;
		char		raw[VARHDRSZ + HIT_BYTES];
	}			hit;
	uint32		order = pg_hton32(score_order(score));
	uint64		k = pg_hton64(key);

	SET_VARSIZE(&hit.v, VARHDRSZ + HIT_BYTES);
	memcpy(VARDATA(&hit.v), &order, sizeof(order));
	memcpy(VARDATA(&hit.v) + sizeof(order), &k, sizeof(k));
	tuplesort_putdatum(s->sort, PointerGetDatum(&hit.v), false);
}

void
sift_hitspill_sort(SiftHitSpill *s)
{
	tuplesort_performsort(s->sort);
}

bool
sift_hitspill_next(SiftHitSpill *s, SiftKey *key, float4 *score)
{
	Datum		val;
	bool		isnull;
	const char *p;
	uint32		order;
	uint64		k;

	if (!tuplesort_getdatum(s->sort, true, false, &val, &isnull, NULL))
		return false;
	p = VARDATA_ANY(DatumGetPointer(val));
	memcpy(&order, p, sizeof(order));
	memcpy(&k, p + sizeof(order), sizeof(k));
	order = ~pg_ntoh32(order);
	order = (order & 0x80000000) ? order & 0x7FFFFFFF : ~order;
	memcpy(score, &order, sizeof(*score));
	*key = pg_ntoh64(k);
	return true;
}

void
sift_hitspill_end(SiftHitSpill *s)
{
	tuplesort_end(s->sort);
	pfree(s);
}
