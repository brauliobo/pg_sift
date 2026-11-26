/*
 * col/f2/scope/spool.c - the spool of spool.h.
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/tuplesort.h"

#include "col/f2/scope/spool.h"
#include "core/compat.h"

#define SPOOL_MIN_ROWS	1024

struct SiftScopeSpool
{
	SiftMemTracker *mem;
	SiftScopeMemberKey *v;
	uint32		n;
	uint32		cap;
	uint32		at;
	Tuplesortstate *sort;		/* once the array has left */
	TupleDesc	desc;
	TupleTableSlot *put;
	TupleTableSlot *get;
	bool		sorted;
};

static int
row_order(const void *a, const void *b)
{
	const SiftScopeMemberKey *x = a,
			   *y = b;

	if (x->member != y->member)
		return x->member < y->member ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

SiftScopeSpool *
sift_scope_spool_new(SiftMemTracker *mem)
{
	SiftScopeSpool *s = palloc0(sizeof(SiftScopeSpool));

	s->mem = mem;
	return s;
}

bool
sift_scope_spool_spilled(const SiftScopeSpool *s)
{
	return s->sort != NULL;
}

SiftScopeMemberKey *
sift_scope_spool_rows(SiftScopeSpool *s, uint32 *n)
{
	Assert(s->sort == NULL);
	*n = s->n;
	return s->v;
}

static void
sort_put(SiftScopeSpool *s, int64 a, int64 b)
{
	ExecClearTuple(s->put);
	s->put->tts_values[0] = Int64GetDatum(a);
	s->put->tts_values[1] = Int64GetDatum(b);
	s->put->tts_isnull[0] = s->put->tts_isnull[1] = false;
	ExecStoreVirtualTuple(s->put);
	tuplesort_puttupleslot(s->sort, s->put);
}

/* The rows of the array go to a sort in a quarter of maintenance_work_mem (the pairs, the keys and the two hops of a scope of two paths may each have one) */
static void
spill(SiftScopeSpool *s)
{
	AttrNumber	cols[2] = {1, 2};
	Oid			ops[2] = {Int8LessOperator, Int8LessOperator},
				colls[2] = {InvalidOid, InvalidOid};
	bool		nulls_first[2] = {false, false};
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(s));

	s->desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(s->desc, 1, "a", INT8OID, -1, 0);
	TupleDescInitEntry(s->desc, 2, "b", INT8OID, -1, 0);
	sift_tupdesc_finalize(s->desc);
	s->sort = tuplesort_begin_heap(s->desc, 2, cols, ops, colls, nulls_first, Max(sift_mem_maint_kb() / 4, 64), NULL, TUPLESORT_RANDOMACCESS);
	s->put = MakeSingleTupleTableSlot(s->desc, &TTSOpsVirtual);
	s->get = MakeSingleTupleTableSlot(s->desc, &TTSOpsMinimalTuple);
	MemoryContextSwitchTo(old);
	for (uint32 i = 0; i < s->n; i++)
		sort_put(s, s->v[i].member, s->v[i].key);
	pfree(s->v);
	s->v = NULL;
	s->n = s->cap = 0;
}

void
sift_scope_spool_put(SiftScopeSpool *s, int64 a, int64 b)
{
	if (s->sort == NULL && s->n == s->cap)
	{
		const uint32 ncap = Min(Max(SPOOL_MIN_ROWS, s->cap * 2), MaxAllocSize / sizeof(SiftScopeMemberKey));

		if (ncap == s->cap || sift_mem_would_exceed(s->mem, (Size) (ncap - s->cap) * sizeof(SiftScopeMemberKey)))
			spill(s);
		else
		{
			s->v = s->v ? repalloc(s->v, (Size) ncap * sizeof(SiftScopeMemberKey)) : sift_mem_alloc(s->mem, s->mem->root_cx, (Size) ncap * sizeof(SiftScopeMemberKey));
			s->cap = ncap;
		}
	}
	if (s->sort != NULL)
		sort_put(s, a, b);
	else
		s->v[s->n++] = (SiftScopeMemberKey) {a, b};
}

void
sift_scope_spool_sort(SiftScopeSpool *s)
{
	if (s->sort != NULL)
		tuplesort_performsort(s->sort);
	else
		qsort(s->v, s->n, sizeof(*s->v), row_order);
	s->sorted = true;
	s->at = 0;
}

void
sift_scope_spool_rewind(SiftScopeSpool *s)
{
	Assert(s->sorted);
	if (s->sort != NULL)
		tuplesort_rescan(s->sort);
	s->at = 0;
}

bool
sift_scope_spool_next(SiftScopeSpool *s, int64 *a, int64 *b)
{
	bool		isnull;

	if (s->sort == NULL)
	{
		if (s->at == s->n)
			return false;
		*a = s->v[s->at].member;
		*b = s->v[s->at++].key;
		return true;
	}
	if (!tuplesort_gettupleslot(s->sort, true, false, s->get, NULL))
		return false;
	*a = DatumGetInt64(slot_getattr(s->get, 1, &isnull));
	*b = DatumGetInt64(slot_getattr(s->get, 2, &isnull));
	return true;
}

void
sift_scope_spool_free(SiftScopeSpool *s)
{
	if (s->sort != NULL)
	{
		tuplesort_end(s->sort);
		ExecDropSingleTupleTableSlot(s->put);
		ExecDropSingleTupleTableSlot(s->get);
		FreeTupleDesc(s->desc);
	}
	else if (s->v != NULL)
		pfree(s->v);
	pfree(s);
}
