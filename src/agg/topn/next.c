/*
 * agg/topn/next.c - the next row of a top-N and the state shared with workers
 * The row handed out, the group-ordered next, the parallel bound, and the counters and
 * the state a worker exports and imports.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"

#include "agg/leaf_int.h"
#include "agg/gord.h"
#include "agg/topn/int.h"

#define TOPN_MAGIC	0x544F5031

static bool
gord_next(SiftTopN *t, TupleTableSlot *slot)
{
	TopFetch   *f = &t->pf[0];
	int64		key;
	bool		isnull;
	ItemPointerData tid;

	while (t->emitted < t->limit && (t->nkey > 1 ? t->gqpos < t->gqn : sift_gord_next(t->go, &key, &isnull, &tid)))
	{
		bool		call_again = false,
					all_dead,
					hnull;
		Datum		d;

		if (t->nkey > 1)
			tid = t->gq[t->gqpos++].tid;
		/* an entry of an exact unit that the Limit throws away is not confirmed in the heap: its key is the array's */
		else if (t->emitted < t->skip && sift_gord_exact(t->go, &tid) && (t->gcond == NULL || gcond_entry(t->gcond, sift_gord_group(t->go), true, key, isnull) == 1))
		{
			t->emitted++;
			return skipped_row(t, slot);
		}
		if (!table_index_fetch_tuple(f->fetch, &tid, t->snap, f->slot, &call_again, &all_dead))
			continue;
		if (t->nkey == 1)
		{
			d = slot_getattr(f->slot, t->gatt[0], &hnull);
			if (hnull != isnull)
				continue;
			if (!hnull && t->gtext)
			{
				/* the entry's string is the heap row's: a row an UPDATE or a vacuumed address replaced holds another */
				uint32		slen;
				const char *str = sift_gord_str(t->go, &slen);
				struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

				if (VARSIZE_ANY_EXHDR(v) != slen || memcmp(VARDATA_ANY(v), str, slen) != 0)
					continue;
			}
			else if (!hnull && heap_key(t->gtyp[0], d) != key)
				continue;
			if (t->gcond != NULL && !gcond_test(t->gcond, f->slot))
				continue;
		}
		t->fetched++;
		if (t->emitted++ < t->skip)
			return skipped_row(t, slot);
		ExecClearTuple(slot);
		for (int c = 0; c < t->nout; c++)
			slot->tts_values[c] = slot_getattr(f->slot, f->attno[c], &slot->tts_isnull[c]);
		ExecStoreVirtualTuple(slot);
		ExecMaterializeSlot(slot);
		return true;
	}
	return false;
}

bool
sift_topn_next(SiftTopN *t, TupleTableSlot *slot)
{
	ItemPointerData tid;
	uint16		part;
	TopFetch   *f;
	const TopEnt *e = !t->all && t->pos < t->n ? &t->res[t->pos] : NULL;
	bool		call_again = false,
				all_dead;

	if (t->go)
		return gord_next(t, slot);
	if (!next_tid(t, &tid, &part))
		return false;
	if (t->emitted++ < t->skip)
		return skipped_row(t, slot);
	if (e != NULL && e->hasv)
	{
		/* the row came from a unit: its columns are the heap row's (an update that keeps the TID's chain keeps the columns of the index) */
		ExecClearTuple(slot);
		for (int c = 0; c < t->nov; c++)
		{
			slot->tts_isnull[c] = t->ovn[(Size) e->oi * t->nov + c];
			slot->tts_values[c] = slot->tts_isnull[c] ? (Datum) 0 : leaf_datum(t->otyp[c], t->ov[(Size) e->oi * t->nov + c]);
		}
		ExecStoreVirtualTuple(slot);
		return true;
	}
	f = &t->pf[part];
	if (f->slot == NULL)
		fetch_begin(t, part);
	/* the TID is the root of a HOT chain when the row was updated without a new index entry: the fetch follows the chain to the visible version */
	if (!table_index_fetch_tuple(f->fetch, &tid, t->snap, f->slot, &call_again, &all_dead))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("sift: top-N row (%u,%u) is not in the heap", ItemPointerGetBlockNumber(&tid), ItemPointerGetOffsetNumber(&tid)),
				 errhint("REINDEX the index.")));
	t->fetched++;
	ExecClearTuple(slot);
	for (int c = 0; c < t->nout; c++)
		slot->tts_values[c] = slot_getattr(f->slot, f->attno[c], &slot->tts_isnull[c]);
	ExecStoreVirtualTuple(slot);
	ExecMaterializeSlot(slot);
	return true;
}

bool
sift_topn_parallel_ok(const SiftTopN *t)
{
	return !t->all && !t->totals && t->limit > 0;
}

void
sift_topn_share(SiftTopN *t, pg_atomic_uint64 *bound)
{
	t->shared = bound;
}

void
sift_topn_bound_init(pg_atomic_uint64 *bound)
{
	pg_atomic_init_u64(bound, PG_UINT64_MAX);
}

/* A worker takes the order of the leader's probes */
void
sift_topn_visit(SiftTopN *t, uint32 nsb, bool reverse)
{
	t->nsb = nsb;
	t->reverse = reverse;
}

const uint32 *
sift_topn_walk_order(SiftTopN *t)
{
	const WalkBase *wb = t->wb;
	uint32		n = 0;

	if (t->nhead == 0)
		return wb->ord;
	t->wfull = MemoryContextAlloc(GetMemoryChunkContext(t), wb->nsb * sizeof(uint32));
	for (int i = 0; i < t->nhead; i++)
		if (t->whead[i] < wb->nsb)
			t->wfull[n++] = t->whead[i];
	for (uint32 i = 0; i < wb->nsb; i++)
		if (!walk_in_head(t, wb->ord[i]))
			t->wfull[n++] = wb->ord[i];
	return t->wfull;
}

void
sift_topn_add_skipped(SiftTopN *t, int64 n, int64 runs, int64 lazy)
{
	t->skipped += n;
	t->nrun += runs;
	t->nlazy += lazy;
}

int64
sift_topn_heads_used(const SiftTopN *t)
{
	return t->nrhead;
}

int64
sift_topn_mono_used(const SiftTopN *t)
{
	return t->nmono;
}

int64
sift_topn_runs_read(const SiftTopN *t)
{
	return t->nrun;
}

int64
sift_topn_runs_lazy(const SiftTopN *t)
{
	return t->nlazy;
}

/* units compared with the bar by an exact order code (0), by a prefix (1), as bytes (2) */
int64
sift_topn_ord_units(const SiftTopN *t, int how)
{
	return t->nord[how];
}

/* The heap of a worker, for the leader: the entries are the keys and the TID of the rows, not the rows */
void
sift_topn_export(const SiftTopN *t, StringInfo b)
{
	uint32		h[2] = {TOPN_MAGIC, t->n};

	appendBinaryStringInfo(b, (const char *) h, sizeof(h));
	appendBinaryStringInfo(b, (const char *) t->heap, t->n * sizeof(TopEnt));
	/* then the byte strings of the entries, in the order of the entries and keys: a KeyBlob each, for the keys that are not NULL */
	for (int i = 0; t->nblob > 0 && i < t->n; i++)
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].blob && !((t->heap[i].nul >> j) & 1))
				appendBinaryStringInfo(b, (const char *) KEY_BLOB(t->heap[i].k[j]), offsetof(KeyBlob, data) + KEY_BLOB(t->heap[i].k[j])->len);
}

void
sift_topn_import(SiftTopN *t, const char *data, Size len)
{
	uint32		h[2];
	Size		off;

	SIFT_INVARIANT(len >= sizeof(h), "sift: bad message of a parallel top-N");
	memcpy(h, data, sizeof(h));
	SIFT_INVARIANT(h[0] == TOPN_MAGIC && (Size) h[1] <= (len - sizeof(h)) / sizeof(TopEnt), "sift: bad message of a parallel top-N");
	off = sizeof(h) + h[1] * sizeof(TopEnt);
	for (uint32 i = 0; i < h[1]; i++)
	{
		TopEnt		e;

		memcpy(&e, data + sizeof(h) + i * sizeof(TopEnt), sizeof(e));
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].blob && !((e.nul >> j) & 1))
			{
				uint32		n;

				SIFT_INVARIANT(len - off >= sizeof(n), "sift: bad message of a parallel top-N");
				memcpy(&n, data + off, sizeof(n));
				SIFT_INVARIANT(len - off - sizeof(n) >= n, "sift: bad message of a parallel top-N");
				e.k[j] = (int64) (uintptr_t) blob_new(CurrentMemoryContext, data + off + sizeof(n), n);
				off += sizeof(n) + n;
			}
		res_push(t, &e, NULL, NULL);
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].blob && !((e.nul >> j) & 1))
				pfree((void *) KEY_BLOB(e.k[j]));
	}
	SIFT_INVARIANT(off == len, "sift: bad message of a parallel top-N");
}

void
sift_topn_rewind(SiftTopN *t)
{
	t->pos = 0;
	t->emitted = 0;
	t->gqpos = 0;
	if (t->go)
		sift_gord_rewind(t->go);
	if (t->sort)
		tuplesort_rescan(t->sort);
}

int64
sift_topn_sorted(const SiftTopN *t)
{
	return t->all ? t->nsort : -1;
}
