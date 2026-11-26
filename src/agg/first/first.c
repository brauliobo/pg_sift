/*
 * agg/first/first.c - the first row of each group of DISTINCT ON columns, from the rows of any plan in any order (agg/first/first.h).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"
#include "utils/tuplestore.h"

#include "agg/first/first.h"
#include "core/compat.h"
#include "core/errctx.h"
#include "core/memtrack.h"

#define FIRST_PARTS			16
#define FIRST_MAX_LEVEL		4
#define FIRST_MIN_CAP		64
#define FIRST_PART_KB		8
#define FIRST_NULL_HASH		0x2545F491

typedef struct FirstEntry
{
	uint32		hash;
	MinimalTuple tup;
} FirstEntry;

typedef struct FirstPass
{
	MemoryContext cx;
	FirstEntry *tab;
	uint32		cap;
	uint32		n;
	uint32		emit_at;
	int			level;
	bool		full;
	Tuplestorestate *part[FIRST_PARTS];
	Tuplesortstate *sort;		/* the rows of a pass of the last level, by key, order and arrival (seq) */
	int64		seq;
	MinimalTuple last;			/* the row of the last group the sort gave */
} FirstPass;

typedef struct FirstSpill
{
	Tuplestorestate *rows;
	int			level;
} FirstSpill;

struct SiftV6First
{
	MemoryContext cx;
	TupleDesc	desc;
	SiftV6FirstKeys k;
	uint64		budget;
	FmgrInfo	hashfn[SIFT_V6_FIRST_MAX_COLS];
	FmgrInfo	eqfn[SIFT_V6_FIRST_MAX_COLS];
	SortSupportData ss[SIFT_V6_FIRST_MAX_COLS];
	FirstPass  *cur;
	List	   *pending;
	TupleTableSlot *held;
	TupleTableSlot *spilled;
	TupleDesc	sdesc;			/* the rows with their seq, for the sort of the last level */
	TupleTableSlot *sput;
	TupleTableSlot *sget;
	MemoryContext row_cx;		/* what the hash, equality and sort functions of a row allocate */
	bool		started;
	SiftV6FirstStats st;
};

SiftV6First *
sift_v6_first_new(MemoryContext cx, TupleDesc tupdesc, const SiftV6FirstKeys *keys, uint64 budget)
{
	SiftV6First *f = MemoryContextAllocZero(cx, sizeof(SiftV6First));
	MemoryContext old = MemoryContextSwitchTo(cx);

	f->cx = cx;
	f->desc = tupdesc;
	f->k = *keys;
	f->budget = budget;
	for (int i = 0; i < keys->nkey; i++)
	{
		Oid			lhs,
					rhs;

		SIFT_INVARIANT(get_op_hash_functions(keys->key_eqop[i], &lhs, &rhs), "the planner offers a DISTINCT ON of columns whose equality has a hash function");
		fmgr_info(lhs, &f->hashfn[i]);
		fmgr_info(get_opcode(keys->key_eqop[i]), &f->eqfn[i]);
	}
	for (int i = 0; i < keys->nord; i++)
	{
		f->ss[i] = (SortSupportData) {.ssup_cxt = cx,.ssup_collation = keys->ord_coll[i],.ssup_nulls_first = keys->ord_nulls_first[i],.ssup_attno = keys->ord_att[i]};
		PrepareSortSupportFromOrderingOp(keys->ord_sortop[i], &f->ss[i]);
	}
	f->held = MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);
	f->spilled = MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);
	f->sdesc = CreateTemplateTupleDesc(tupdesc->natts + 1);
	for (int i = 1; i <= tupdesc->natts; i++)
		TupleDescCopyEntry(f->sdesc, i, tupdesc, i);
	TupleDescInitEntry(f->sdesc, tupdesc->natts + 1, "seq", INT8OID, -1, 0);
	sift_tupdesc_finalize(f->sdesc);
	f->sput = MakeSingleTupleTableSlot(f->sdesc, &TTSOpsVirtual);
	f->sget = MakeSingleTupleTableSlot(f->sdesc, &TTSOpsMinimalTuple);
	f->row_cx = sift_mem_allocset_create(cx, "sift agg v6 first row");
	MemoryContextSwitchTo(old);
	return f;
}

/* the sort of a pass of the last level: the key, the order, then the seq of the row, so that of rows that tie the first one offered comes first */
static Tuplesortstate *
sort_new(const SiftV6First *f)
{
	const SiftV6FirstKeys *k = &f->k;
	const int	n = k->nkey + k->nord + 1;
	AttrNumber	att[SIFT_V6_FIRST_MAX_COLS * 2 + 1];
	Oid			op[SIFT_V6_FIRST_MAX_COLS * 2 + 1],
				coll[SIFT_V6_FIRST_MAX_COLS * 2 + 1];
	bool		nf[SIFT_V6_FIRST_MAX_COLS * 2 + 1];

	for (int i = 0; i < k->nkey; i++)
	{
		att[i] = k->key_att[i];
		op[i] = k->key_sortop[i];
		coll[i] = k->key_coll[i];
		nf[i] = k->key_nulls_first[i];
	}
	for (int i = 0; i < k->nord; i++)
	{
		att[k->nkey + i] = k->ord_att[i];
		op[k->nkey + i] = k->ord_sortop[i];
		coll[k->nkey + i] = k->ord_coll[i];
		nf[k->nkey + i] = k->ord_nulls_first[i];
	}
	att[n - 1] = (AttrNumber) f->sdesc->natts;
	op[n - 1] = Int8LessOperator;
	coll[n - 1] = InvalidOid;
	nf[n - 1] = false;
	return tuplesort_begin_heap(f->sdesc, n, att, op, coll, nf, (int) Max(f->budget / 1024, 64), NULL, TUPLESORT_NONE);
}

static FirstPass *
pass_new(SiftV6First *f, int level)
{
	FirstPass  *p = MemoryContextAllocZero(f->cx, sizeof(FirstPass));

	p->cx = sift_mem_allocset_create(f->cx, "sift agg v6 first pass");
	p->level = level;
	if (level == FIRST_MAX_LEVEL)
	{
		MemoryContext old = MemoryContextSwitchTo(p->cx);

		p->sort = sort_new(f);
		MemoryContextSwitchTo(old);
	}
	f->st.passes++;
	return p;
}

static uint32
row_hash(const SiftV6First *f, TupleTableSlot *slot, int level)
{
	uint32		h = murmurhash32((uint32) level + 1);

	for (int i = 0; i < f->k.nkey; i++)
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, f->k.key_att[i], &isnull);
		uint32		x = isnull ? FIRST_NULL_HASH : DatumGetUInt32(FunctionCall1Coll((FmgrInfo *) &f->hashfn[i], f->k.key_coll[i], d));

		h = hash_combine(h, x);
	}
	return murmurhash32(h);
}

static bool
same_group(const SiftV6First *f, TupleTableSlot *a, TupleTableSlot *b)
{
	for (int i = 0; i < f->k.nkey; i++)
	{
		bool		an,
					bn;
		Datum		x = slot_getattr(a, f->k.key_att[i], &an),
					y = slot_getattr(b, f->k.key_att[i], &bn);

		if (an != bn || (!an && !DatumGetBool(FunctionCall2Coll((FmgrInfo *) &f->eqfn[i], f->k.key_coll[i], x, y))))
			return false;
	}
	return true;
}

/* does the row sort strictly before the one held: of rows that tie the first one offered stays */
static bool
sorts_before(SiftV6First *f, TupleTableSlot *row, TupleTableSlot *held)
{
	for (int i = 0; i < f->k.nord; i++)
	{
		bool		an,
					bn;
		Datum		x = slot_getattr(row, f->k.ord_att[i], &an),
					y = slot_getattr(held, f->k.ord_att[i], &bn);
		int			c = ApplySortComparator(x, an, y, bn, &f->ss[i]);

		if (c != 0)
			return c < 0;
	}
	return false;
}

static void
tab_insert(FirstEntry *tab, uint32 cap, FirstEntry e)
{
	uint32		i = e.hash & (cap - 1);

	while (tab[i].tup != NULL)
		i = (i + 1) & (cap - 1);
	tab[i] = e;
}

static void
tab_grow(FirstPass *p)
{
	const uint32 ncap = Max(p->cap * 2, FIRST_MIN_CAP);
	FirstEntry *n = MemoryContextAllocZero(p->cx, sizeof(FirstEntry) * ncap);

	for (uint32 i = 0; i < p->cap; i++)
		if (p->tab[i].tup != NULL)
			tab_insert(n, ncap, p->tab[i]);
	if (p->tab != NULL)
		pfree(p->tab);
	p->tab = n;
	p->cap = ncap;
}

static bool
over_budget(const SiftV6First *f, const FirstPass *p, uint32 add)
{
	return p->n > 0 &&
		(MemoryContextMemAllocated(p->cx, false) + add + (uint64) Max(p->cap, FIRST_MIN_CAP) * sizeof(FirstEntry) > f->budget || p->cap >= MaxAllocSize / sizeof(FirstEntry) / 4);
}

static void
spill_row(SiftV6First *f, FirstPass *p, TupleTableSlot *slot, uint32 hash)
{
	const int	part = (hash >> 24) % FIRST_PARTS;
	MemoryContext old = MemoryContextSwitchTo(f->cx);

	if (p->part[part] == NULL)
		p->part[part] = tuplestore_begin_heap(false, false, FIRST_PART_KB);
	tuplestore_puttupleslot(p->part[part], slot);
	MemoryContextSwitchTo(old);
	f->st.spilled_rows++;
}

/* the row with its seq into the sort of a pass of the last level */
static void
sort_offer(SiftV6First *f, FirstPass *p, TupleTableSlot *slot)
{
	const int	natts = slot->tts_tupleDescriptor->natts;

	slot_getallattrs(slot);
	ExecClearTuple(f->sput);
	memcpy(f->sput->tts_values, slot->tts_values, natts * sizeof(Datum));
	memcpy(f->sput->tts_isnull, slot->tts_isnull, natts * sizeof(bool));
	f->sput->tts_values[natts] = Int64GetDatum(p->seq++);
	f->sput->tts_isnull[natts] = false;
	ExecStoreVirtualTuple(f->sput);
	tuplesort_puttupleslot(p->sort, f->sput);
}

static void
hash_offer(SiftV6First *f, FirstPass *p, TupleTableSlot *slot)
{
	const uint32 hash = row_hash(f, slot, p->level);
	MemoryContext old;
	FirstEntry *e = NULL;

	if (p->cap > 0)
		for (uint32 i = hash & (p->cap - 1); p->tab[i].tup != NULL; i = (i + 1) & (p->cap - 1))
		{
			if (p->tab[i].hash != hash)
				continue;
			ExecStoreMinimalTuple(p->tab[i].tup, f->held, false);
			if (same_group(f, slot, f->held))
			{
				e = &p->tab[i];
				break;
			}
		}
	if (e != NULL)
	{
		if (!sorts_before(f, slot, f->held))
			return;
		old = MemoryContextSwitchTo(p->cx);
		pfree(e->tup);
		e->tup = ExecCopySlotMinimalTuple(slot);
		MemoryContextSwitchTo(old);
		return;
	}
	p->full = p->full || over_budget(f, p, slot->tts_tupleDescriptor->natts * sizeof(Datum) + sizeof(MinimalTupleData));
	if (p->full)
	{
		spill_row(f, p, slot, hash);
		return;
	}
	if (p->cap == 0 || (uint64) (p->n + 1) * 10 > (uint64) p->cap * 7)
		tab_grow(p);
	old = MemoryContextSwitchTo(p->cx);
	tab_insert(p->tab, p->cap, (FirstEntry) {.hash = hash,.tup = ExecCopySlotMinimalTuple(slot)});
	MemoryContextSwitchTo(old);
	p->n++;
}

static void
pass_offer(SiftV6First *f, FirstPass *p, TupleTableSlot *slot)
{
	MemoryContext old = MemoryContextSwitchTo(f->row_cx);

	if (p->sort != NULL)
		sort_offer(f, p, slot);
	else
		hash_offer(f, p, slot);
	MemoryContextSwitchTo(old);
	MemoryContextReset(f->row_cx);
}

void
sift_v6_first_offer(SiftV6First *f, TupleTableSlot *slot)
{
	Assert(!f->started);
	if (f->cur == NULL)
		f->cur = pass_new(f, 0);
	f->st.rows++;
	pass_offer(f, f->cur, slot);
}

/* the partitions of a pass that took all its rows wait their turn; the last one pushed is solved first, so that the files open are those of one path down */
static void
pass_park_partitions(SiftV6First *f, FirstPass *p)
{
	MemoryContext old = MemoryContextSwitchTo(f->cx);

	for (int i = 0; i < FIRST_PARTS; i++)
		if (p->part[i] != NULL)
		{
			FirstSpill *s = palloc(sizeof(FirstSpill));

			*s = (FirstSpill) {.rows = p->part[i],.level = p->level + 1};
			f->pending = lcons(s, f->pending);
			p->part[i] = NULL;
		}
	MemoryContextSwitchTo(old);
}

static void
pass_free(FirstPass *p)
{
	if (p->sort != NULL)
		tuplesort_end(p->sort);
	MemoryContextDelete(p->cx);
	pfree(p);
}

/* the next group of the sort of a pass of the last level into slot: its first row, the rows of the same key after it skipped */
static bool
sort_next(SiftV6First *f, FirstPass *p, TupleTableSlot *slot)
{
	bool		got;
	MemoryContext old;

	if (p->emit_at++ == 0)
		tuplesort_performsort(p->sort);
	for (;;)
	{
		if (!tuplesort_gettupleslot(p->sort, true, false, f->sget, NULL))
			return false;
		CHECK_FOR_INTERRUPTS();
		old = MemoryContextSwitchTo(f->row_cx);
		got = p->last == NULL || (ExecStoreMinimalTuple(p->last, f->held, false), !same_group(f, f->sget, f->held));
		MemoryContextSwitchTo(old);
		MemoryContextReset(f->row_cx);
		if (got)
			break;
	}
	slot_getallattrs(f->sget);
	ExecClearTuple(f->held);
	ExecClearTuple(slot);
	memcpy(slot->tts_values, f->sget->tts_values, slot->tts_tupleDescriptor->natts * sizeof(Datum));
	memcpy(slot->tts_isnull, f->sget->tts_isnull, slot->tts_tupleDescriptor->natts * sizeof(bool));
	ExecStoreVirtualTuple(slot);
	old = MemoryContextSwitchTo(p->cx);
	if (p->last != NULL)
		pfree(p->last);
	p->last = ExecCopySlotMinimalTuple(slot);
	MemoryContextSwitchTo(old);
	ExecStoreMinimalTuple(p->last, slot, false);
	return true;
}

/* the rows of a partition into a pass of the next level */
static FirstPass *
pass_of_spill(SiftV6First *f, FirstSpill *s)
{
	FirstPass  *p = pass_new(f, s->level);

	while (tuplestore_gettupleslot(s->rows, true, false, f->spilled))
	{
		CHECK_FOR_INTERRUPTS();
		pass_offer(f, p, f->spilled);
	}
	tuplestore_end(s->rows);
	pfree(s);
	pass_park_partitions(f, p);
	return p;
}

bool
sift_v6_first_next(SiftV6First *f, TupleTableSlot *slot)
{
	if (!f->started)
	{
		f->started = true;
		if (f->cur == NULL)
			return false;
		pass_park_partitions(f, f->cur);
	}
	while (f->cur != NULL)
	{
		FirstPass  *p = f->cur;

		if (p->sort != NULL && sort_next(f, p, slot))
		{
			f->st.groups++;
			return true;
		}
		for (; p->sort == NULL && p->emit_at < p->cap; p->emit_at++)
			if (p->tab[p->emit_at].tup != NULL)
			{
				ExecStoreMinimalTuple(p->tab[p->emit_at++].tup, slot, false);
				f->st.groups++;
				return true;
			}
		pass_free(p);
		f->cur = NULL;
		if (f->pending != NIL)
		{
			FirstSpill *s = linitial(f->pending);

			f->pending = list_delete_first(f->pending);
			f->cur = pass_of_spill(f, s);
		}
	}
	return false;
}

const SiftV6FirstStats *
sift_v6_first_stats(const SiftV6First *f)
{
	return &f->st;
}

void
sift_v6_first_free(SiftV6First *f)
{
	if (f->cur != NULL)
	{
		for (int i = 0; i < FIRST_PARTS; i++)
			if (f->cur->part[i] != NULL)
				tuplestore_end(f->cur->part[i]);
		pass_free(f->cur);
	}
	foreach_ptr(FirstSpill, s, f->pending)
		tuplestore_end(s->rows);
	list_free_deep(f->pending);
	ExecDropSingleTupleTableSlot(f->held);
	ExecDropSingleTupleTableSlot(f->spilled);
	ExecDropSingleTupleTableSlot(f->sput);
	ExecDropSingleTupleTableSlot(f->sget);
	FreeTupleDesc(f->sdesc);
	MemoryContextDelete(f->row_cx);
	pfree(f);
}
