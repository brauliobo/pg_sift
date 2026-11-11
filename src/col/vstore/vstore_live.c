/*
 * col/vstore/vstore_live.c - which codes of a shared value store the segments of its indexes refer to (docs/COL-VSTORE.md)
 *
 * The store is append-only inside an epoch: a value that no segment refers to any more (its rows were deleted or replaced and the segments that held it
 * were merged or rewritten) keeps its code. The codes that are in use are read from the segments of the members themselves, the one place that says it:
 * the dictionary of a leaf of a segment that refers to the store lists the codes of its values.
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/seg.h"
#include "col/vstore/vstore.h"
#include "core/page.h"
#include "core/store.h"

typedef struct Use
{
	Oid			index;			/* the member */
	uint32		epoch;			/* the epoch of the store that its segments name */
	int			segments;
	uint64		nbits;
	uint8	   *bits;
} Use;

static Use *
use_for(List **uses, Oid index, uint32 epoch)
{
	foreach_ptr(Use, u, *uses)
		if (u->index == index && u->epoch == epoch)
			return u;
	{
		Use		   *u = palloc0(sizeof(Use));

		u->index = index;
		u->epoch = epoch;
		*uses = lappend(*uses, u);
		return u;
	}
}

static void
use_set(Use *u, const uint32 *codes, uint32 n)
{
	uint64		max = 0;

	for (uint32 i = 0; i < n; i++)
		max = Max(max, codes[i]);
	if (max + 1 > u->nbits)
	{
		uint64		nbits = Max(max + 1, u->nbits * 2);

		u->bits = u->bits ? repalloc0(u->bits, (u->nbits + 7) / 8, (nbits + 7) / 8) : palloc0((nbits + 7) / 8);
		u->nbits = nbits;
	}
	for (uint32 i = 0; i < n; i++)
		u->bits[codes[i] >> 3] |= 1 << (codes[i] & 7);
}

static int64
use_count(const uint8 *bits, uint64 nbits)
{
	int64		c = 0;

	for (uint64 i = 0; i < (nbits + 7) / 8; i++)
		c += pg_popcount32(bits[i]);
	return c;
}

/* The members' segments of the store: what each of them refers to, by epoch */
static List *
store_uses(Oid store)
{
	List	   *uses = NIL;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift value store usage", ALLOCSET_DEFAULT_SIZES);

	foreach_oid(index_oid, sift_vstore_members(store))
	{
		Relation	index = index_open(index_oid, AccessShareLock);
		SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

		if (index->rd_rel->relam != sift_col_am_oid())
		{
			index_close(index, AccessShareLock);
			continue;
		}
		sift_dir_read(index, snap);
		for (int i = 0; i < snap->nsegs; i++)
		{
			MemoryContext old;
			SiftColSegment *seg;

			if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
				continue;
			CHECK_FOR_INTERRUPTS();
			old = MemoryContextSwitchTo(cx);
			seg = sift_cseg_open(index, &snap->segs[i], cx);
			for (int c = 0; c < seg->schema->ncols; c++)
			{
				uint32		st,
							epoch,
							n;
				uint32	   *codes = sift_cseg_ref_codes(seg, c, &st, &epoch, &n);

				if (codes != NULL && st == store)
				{
					Use		   *u;

					MemoryContextSwitchTo(old);
					u = use_for(&uses, index_oid, epoch);
					u->segments++;
					use_set(u, codes, n);
					MemoryContextSwitchTo(cx);
				}
			}
			sift_cseg_close(seg);
			MemoryContextSwitchTo(old);
			MemoryContextReset(cx);
		}
		index_close(index, AccessShareLock);
	}
	MemoryContextDelete(cx);
	return uses;
}

PG_FUNCTION_INFO_V1(sift_col_value_store_usage);

/*
 * sift.col_value_store_usage(store): (epoch, member, segments, codes, total_codes) for each member and epoch the segments of the members name, and for each epoch a row
 * of member NULL with the codes of the epoch that any member refers to; total_codes is what the store holds in its newest epoch
 */
Datum
sift_col_value_store_usage(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			store = PG_GETARG_OID(0);
	Oid			heap;
	Relation	rel;
	uint64		epoch,
				total;
	List	   *uses,
			   *epochs = NIL;

	if (!sift_vstore_is_store(store))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a shared value store", get_rel_name(store))));
	heap = sift_vstore_table(store);
	if (!OidIsValid(heap) || object_aclcheck(RelationRelationId, heap, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("permission denied for the table of the shared value store \"%s\"", get_rel_name(store))));
	InitMaterializedSRF(fcinfo, 0);
	rel = table_open(store, AccessShareLock);
	total = sift_vstore_dir_codes(rel, &epoch);
	table_close(rel, AccessShareLock);
	uses = store_uses(store);
	foreach_ptr(Use, u, uses)
		epochs = list_append_unique_int(epochs, (int) u->epoch);
	foreach_int(e, epochs)
	{
		uint64		nbits = 0;
		uint8	   *any;

		foreach_ptr(Use, u, uses)
			if (u->epoch == (uint32) e)
				nbits = Max(nbits, u->nbits);
		any = palloc0((nbits + 7) / 8 + 1);
		foreach_ptr(Use, u, uses)
		{
			Datum		vals[5];
			bool		nulls[5] = {0};

			if (u->epoch != (uint32) e)
				continue;
			for (uint64 i = 0; i < (u->nbits + 7) / 8; i++)
				any[i] |= u->bits[i];
			vals[0] = Int64GetDatum(e);
			vals[1] = ObjectIdGetDatum(u->index);
			vals[2] = Int32GetDatum(u->segments);
			vals[3] = Int64GetDatum(use_count(u->bits, u->nbits));
			vals[4] = Int64GetDatum(total);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, vals, nulls);
		}
		{
			Datum		vals[5];
			bool		nulls[5] = {0};

			vals[0] = Int64GetDatum(e);
			vals[1] = (Datum) 0;
			nulls[1] = true;
			vals[2] = Int32GetDatum(0);
			nulls[2] = true;
			vals[3] = Int64GetDatum(use_count(any, nbits));
			vals[4] = Int64GetDatum(total);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, vals, nulls);
		}
	}
	if (epochs == NIL)
	{
		Datum		vals[5] = {Int64GetDatum(epoch), 0, 0, Int64GetDatum(0), Int64GetDatum(total)};
		bool		nulls[5] = {false, true, true, false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, vals, nulls);
	}
	return (Datum) 0;
}
