/*
 * agg/osa/ship.c - the inputs of a WITHIN GROUP aggregate between the participants of a parallel run (agg/osa/ship.h). A group's pairs are [npairs uint32] then, for each, [count int64][len uint32]
 * and the value as datumSerialize writes it.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"

#include "agg/gen/sort.h"
#include "agg/osa/mode.h"
#include "agg/osa/osa.h"
#include "agg/osa/ship.h"
#include "core/memtrack.h"

/* the pairs of each group of an aggregate, between the hold of a participant's inputs and the records of its groups */
typedef struct V6OsaHeld
{
	MemoryContext cx;
	uint32		nsid;
	int16		typlen;			/* of the argument */
	bool		typbyval;
	uint32	   *npairs;
	StringInfoData *bytes;
} V6OsaHeld;

bool
sift_v6_osa_shippable(const Aggref *a)
{
	return a->aggkind == AGGKIND_ORDERED_SET && a->aggdistinct == NIL && sift_v6_osa_eligible(a);
}

static void
hold_pair(void *arg, uint32 sid, Datum value, int64 count)
{
	V6GenAgg   *ga = arg;
	V6OsaHeld  *h = ga->held;
	StringInfo	b = &h->bytes[sid];
	const Size	sz = datumEstimateSpace(value, false, h->typbyval, h->typlen);
	const uint32 len = (uint32) sz;
	char	   *at;

	if (b->data == NULL)
		initStringInfo(b);
	appendBinaryStringInfo(b, (const char *) &count, sizeof(count));
	appendBinaryStringInfo(b, (const char *) &len, sizeof(len));
	enlargeStringInfo(b, len);
	at = b->data + b->len;
	datumSerialize(value, false, h->typbyval, h->typlen, &at);
	b->len += len;
	h->npairs[sid]++;
}

void
v6_osa_hold(V6Gen *gen, V6GenAgg *ga)
{
	MemoryContext cx = sift_mem_allocset_create(gen->cx, "sift agg v6 ordered-set inputs held");
	MemoryContext old = MemoryContextSwitchTo(cx);

	ga->held = palloc0(sizeof(V6OsaHeld));
	*ga->held = (V6OsaHeld) {.cx = cx,.nsid = ga->nsid,.npairs = palloc0(Max(ga->nsid, 1) * sizeof(uint32)),.bytes = palloc0(Max(ga->nsid, 1) * sizeof(StringInfoData))};
	get_typlenbyval(ga->arg[0].typid, &ga->held->typlen, &ga->held->typbyval);
	if (ga->mode != NULL)
		v6_mode_each(ga->mode, hold_pair, ga);
	v6_gen_sort_each(ga->sort, hold_pair, ga);
	MemoryContextSwitchTo(old);
}

void
v6_osa_put(V6GenAgg *ga, uint32 sid, StringInfo buf)
{
	const V6OsaHeld *h = ga->held;

	Assert(h != NULL && sid < h->nsid);
	appendBinaryStringInfo(buf, (const char *) &h->npairs[sid], sizeof(uint32));
	if (h->npairs[sid] > 0)
		appendBinaryStringInfo(buf, h->bytes[sid].data, h->bytes[sid].len);
}

const char *
v6_osa_take(V6Gen *gen, V6GenAgg *ga, V6Acc *dst, const char *tail)
{
	const bool	isnull = false;
	uint32		npairs;

	memcpy(&npairs, tail, sizeof(npairs));
	tail += sizeof(npairs);
	if (npairs > 0 && dst->v1 == 0)
		dst->v1 = ++ga->nsid;
	for (uint32 i = 0; i < npairs; i++)
	{
		int64		count;
		uint32		len;
		const char *at;
		bool		n;
		MemoryContext old = MemoryContextSwitchTo(gen->tmp->ecxt_per_tuple_memory);
		Datum		value;

		memcpy(&count, tail, sizeof(count));
		memcpy(&len, tail + sizeof(count), sizeof(len));
		at = tail + sizeof(count) + sizeof(len);
		value = datumRestore((char **) &at, &n);
		dst->aux += count;
		for (int64 c = 0; c < count; c++)
		{
			CHECK_FOR_INTERRUPTS();
			if (ga->mode == NULL || !v6_mode_take(ga, (uint32) dst->v1 - 1, value))
				v6_gen_sort_put(ga->sort, (uint32) dst->v1 - 1, &value, &isnull);
		}
		MemoryContextSwitchTo(old);
		ResetExprContext(gen->tmp);
		tail += sizeof(count) + sizeof(len) + len;
	}
	return tail;
}

void
v6_osa_release(V6GenAgg *ga)
{
	if (ga->held == NULL)
		return;
	MemoryContextDelete(ga->held->cx);
	ga->held = NULL;
}
