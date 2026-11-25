/*
 * agg/v6/win/carry.c - the carry of v6_win_carry.h: an open-addressing table from the tag of a row to its variables, with the deletion that closes the gap (no tombstones).
 */
#include "agg/v6/win/carry.h"

#include "catalog/pg_type_d.h"

#include "agg/v6/batch.h"

#define CARRY_FIRST_SLOTS	4096

struct SiftWinCarry
{
	MemoryContext cx;
	SiftWinCarrySpec spec;
	uint64		slots;			/* a power of two */
	uint64		max_slots;
	uint64		live;
	uint64	   *tag;
	uint8	   *used;
	uint8	   *nulls;			/* bit i: variable i of the row is NULL */
	Datum	   *val;			/* slots * nvars */
};

bool
sift_win_carry_type_ok(Oid typid, SiftVecType type)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return type == SVEC_I32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return type == SVEC_I64;
		case FLOAT8OID:
			return type == SVEC_F64;
		default:
			return false;
	}
}

static inline uint64
slot_of(const SiftWinCarry *c, uint64 tag)
{
	return (tag * UINT64CONST(0x9E3779B97F4A7C15)) >> 20 & (c->slots - 1);
}

static inline size_t
slot_bytes(int nvars)
{
	return sizeof(uint64) + 2 * sizeof(uint8) + nvars * sizeof(Datum);
}

static void
table_alloc(SiftWinCarry *c, uint64 slots)
{
	c->slots = slots;
	c->tag = MemoryContextAllocZero(c->cx, slots * sizeof(uint64));
	c->used = MemoryContextAllocZero(c->cx, slots);
	c->nulls = MemoryContextAllocZero(c->cx, slots);
	c->val = MemoryContextAllocZero(c->cx, slots * c->spec.nvars * sizeof(Datum));
}

SiftWinCarry *
sift_win_carry_create(MemoryContext cx, const SiftWinCarrySpec *spec)
{
	SiftWinCarry *c = MemoryContextAllocZero(cx, sizeof(SiftWinCarry));
	uint64		max_slots = CARRY_FIRST_SLOTS;

	c->cx = cx;
	c->spec = *spec;
	while ((max_slots << 1) * slot_bytes(spec->nvars) <= spec->max_bytes)
		max_slots <<= 1;
	c->max_slots = max_slots;
	table_alloc(c, CARRY_FIRST_SLOTS);
	return c;
}

SiftWinCarry *
sift_win_carry_open(MemoryContext cx, SiftV6Input *in, const Var *const *vars, int nvars, uint64 max_bytes)
{
	SiftWinCarrySpec cs = {.nvars = nvars,.max_bytes = max_bytes};

	if (nvars == 0 || nvars > SIFT_WIN_CARRY_MAX_VARS)
		return NULL;
	for (int i = 0; i < nvars; i++)
	{
		SiftV6InCol col;

		if (!sift_v6_vec_type(vars[i]->vartype, vars[i]->vartypmod, &col) || !sift_win_carry_type_ok(vars[i]->vartype, col.type))
			return NULL;
	}
	for (int i = 0; i < nvars; i++)
	{
		cs.slot[i] = sift_v6_input_add_var(in, vars[i]);
		cs.typid[i] = vars[i]->vartype;
		if (cs.slot[i] < 0)
			return NULL;
	}
	return sift_win_carry_create(cx, &cs);
}

static void
insert_slot(SiftWinCarry *c, uint64 tag, const Datum *vals, uint8 nulls)
{
	uint64		i = slot_of(c, tag);

	while (c->used[i] && c->tag[i] != tag)
		i = (i + 1) & (c->slots - 1);
	c->live += !c->used[i];
	c->used[i] = 1;
	c->tag[i] = tag;
	c->nulls[i] = nulls;
	memcpy(&c->val[i * c->spec.nvars], vals, c->spec.nvars * sizeof(Datum));
}

static void
grow(SiftWinCarry *c)
{
	const uint64 old_slots = c->slots;
	uint64	   *old_tag = c->tag;
	uint8	   *old_used = c->used;
	uint8	   *old_nulls = c->nulls;
	Datum	   *old_val = c->val;

	c->live = 0;
	table_alloc(c, old_slots << 1);
	for (uint64 i = 0; i < old_slots; i++)
		if (old_used[i])
			insert_slot(c, old_tag[i], &old_val[i * c->spec.nvars], old_nulls[i]);
	pfree(old_tag);
	pfree(old_used);
	pfree(old_nulls);
	pfree(old_val);
}

static inline Datum
datum_at(Oid typid, const SiftVec *v, uint32 r)
{
	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) ((const int32 *) v->data)[r]);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum(((const int32 *) v->data)[r]);
		case FLOAT8OID:
			return Float8GetDatum(((const double *) v->data)[r]);
		default:
			return Int64GetDatum(((const int64 *) v->data)[r]);
	}
}

void
sift_win_carry_put(SiftWinCarry *c, const SiftV6Batch *b)
{
	const int	nvars = c->spec.nvars;
	Datum		vals[SIFT_WIN_CARRY_MAX_VARS];

	for (uint32 r = 0; r < b->n; r++)
	{
		uint8		nulls = 0;

		if (b->mask != NULL && !((b->mask[r >> 6] >> (r & 63)) & 1))
			continue;
		if (c->live * 4 >= c->slots * 3)
		{
			if ((c->slots << 1) > c->max_slots)
				return;
			grow(c);
		}
		for (int i = 0; i < nvars; i++)
		{
			const SiftVec *v = &b->col[c->spec.slot[i]];
			const bool	valid = sift_vec_valid_at(v, r);

			nulls |= (uint8) (!valid << i);
			vals[i] = valid ? datum_at(c->spec.typid[i], v, r) : (Datum) 0;
		}
		insert_slot(c, b->tag[r], vals, nulls);
	}
}

bool
sift_win_carry_take(SiftWinCarry *c, uint64 tag, Datum *values, bool *isnull)
{
	const uint64 mask = c->slots - 1;
	uint64		i = slot_of(c, tag),
				hole,
				j;

	while (c->used[i] && c->tag[i] != tag)
		i = (i + 1) & mask;
	if (!c->used[i])
		return false;
	for (int k = 0; k < c->spec.nvars; k++)
	{
		values[k] = c->val[i * c->spec.nvars + k];
		isnull[k] = (c->nulls[i] >> k) & 1;
	}
	hole = i;
	for (j = (i + 1) & mask; c->used[j]; j = (j + 1) & mask)
	{
		const uint64 home = slot_of(c, c->tag[j]);

		if (((j - home) & mask) >= ((j - hole) & mask))
		{
			c->tag[hole] = c->tag[j];
			c->nulls[hole] = c->nulls[j];
			memcpy(&c->val[hole * c->spec.nvars], &c->val[j * c->spec.nvars], c->spec.nvars * sizeof(Datum));
			hole = j;
		}
	}
	c->used[hole] = 0;
	c->live--;
	return true;
}

void
sift_win_carry_free(SiftWinCarry *c)
{
	pfree(c->tag);
	pfree(c->used);
	pfree(c->nulls);
	pfree(c->val);
	pfree(c);
}
