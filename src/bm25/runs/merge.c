/*
 * bm25/runs/merge.c - the merge of the readers of runs into a run or a segment
 * the batch of a term
 * the output of its documents and postings
 */
#include "core/host.h"
#include "core/cmp.h"
#include "bm25/runs/runs_priv.h"

static void batch_to_run(Batch *bt);

void
batch_flush(Batch *bt)
{
	if (bt->n && bt->run)
		batch_to_run(bt);
	else if (bt->n)
		sift_postw_add(bt->pw, bt->keys, bt->tf, bt->dl, bt->n, bt->haspos ? bt->pos : NULL, bt->plen);
	bt->n = 0;
	bt->termpos += bt->poslen;
	bt->poslen = 0;
}

/* rec / len: the posting's position record, ignored unless the term has positions */
void
batch_put(Batch *bt, SiftKey key, uint8 tf, uint16 dl, const uint8 *rec, uint32 len)
{
	bt->keys[bt->n] = key;
	bt->tf[bt->n] = tf;
	bt->dl[bt->n] = dl;
	if (bt->haspos)
	{
		if (bt->poslen + len > bt->poscap)
		{
			bt->poscap = Max(bt->poslen + len, Max(8192, bt->poscap * 2));
			SIFT_REGROW_HUGE_IN(CurrentMemoryContext, bt->pos, bt->poscap);
		}
		memcpy(bt->pos + bt->poslen, rec, len);
		bt->plen[bt->n] = len;
		bt->poslen += len;
	}
	if (++bt->n == BATCH)
		batch_flush(bt);
}

/* The postings of the batch in the run format (see spill_to) */
static void
batch_to_run(Batch *bt)
{
	Size		off = 0;

	for (int k = 0; k < bt->n; bt->prev = bt->keys[k++])
	{
		io_varint(bt->run, (bt->keys[k] - bt->prev) << 1 | (bt->tf[k] > 1));
		if (bt->tf[k] > 1)
			io_put(bt->run, &bt->tf[k], 1);
		io_varint(bt->run, bt->dl[k]);
		if (bt->haspos)
		{
			io_put(bt->run, bt->pos + off, bt->plen[k]);
			off += bt->plen[k];
		}
	}
}

static void
out_docs(MergeOut *mo, const SiftKey *keys, const uint16 *dl, int n)
{
	if (mo->sw)
		sift_segw_add_docs(mo->sw, keys, dl, n);
	else
		for (int i = 0; i < n; mo->prev = keys[i++])
		{
			io_varint(&mo->io, keys[i] - mo->prev);
			io_varint(&mo->io, dl[i]);
		}
}

/* The term's postings follow in bt: df of them, with position records when bt->haspos */
static void
out_begin_term(MergeOut *mo, Batch *bt, const char *term, int len, uint64 df)
{
	bt->n = 0;
	bt->termpos = 0;
	if (mo->sw)
	{
		bt->pw = sift_segw_begin_term(mo->sw, term, len);
		return;
	}
	run_sample(&mo->io, term, len);
	io_varint(&mo->io, len);
	io_put(&mo->io, term, len);
	io_varint(&mo->io, df);
	io_varint(&mo->io, bt->haspos);
	bt->prev = 0;
}

/* All runs' docs in key order (the runs' key ranges may interleave) */
void
runs_merge_docs(MergeOut *mo, RunReader *rd, int nruns)
{
	binaryheap *heap = binaryheap_allocate(nruns, runs_doc_heap_cmp, NULL);
	SiftKey		keys[BATCH];
	uint16		dl[BATCH];
	int			n = 0;

	for (int i = 0; i < nruns; i++)
		if (runs_reader_next_doc(&rd[i]))
			binaryheap_add_unordered(heap, PointerGetDatum(&rd[i]));
	binaryheap_build(heap);
	while (!binaryheap_empty(heap))
	{
		RunReader  *top = (RunReader *) DatumGetPointer(binaryheap_first(heap));

		keys[n] = top->dkey;
		dl[n++] = top->ddl;
		if (runs_reader_next_doc(top))
			binaryheap_replace_first(heap, PointerGetDatum(top));
		else
			binaryheap_remove_first(heap);
		if (n == BATCH)
		{
			out_docs(mo, keys, dl, n);
			n = 0;
			CHECK_FOR_INTERRUPTS();
		}
	}
	if (n > 0)
		out_docs(mo, keys, dl, n);
	binaryheap_free(heap);
}

/*
 * Postings of the n runs positioned on one term in key order, in batches;
 * act is scratch for n runs. A run drains while its key stays below the
 * others', so runs with disjoint ranges cost one pass each.
 */
static void
merge_postings(Batch *bt, RunReader **cur, int n, RunReader **act)
{
	int			live = 0;

	for (int i = 0; i < n; i++)
	{
		cur[i]->pleft = cur[i]->df;
		cur[i]->pkey = 0;
		if (runs_post_next(cur[i]))
			act[live++] = cur[i];
	}
	while (live > 0)
	{
		int			m = 0;
		SiftKey		limit = PG_UINT64_MAX;
		RunReader  *r;

		for (int i = 1; i < live; i++)
			if (act[i]->pkey < act[m]->pkey)
				m = i;
		for (int i = 0; i < live; i++)
			if (i != m)
				limit = Min(limit, act[i]->pkey);
		r = act[m];
		for (;;)
		{
			batch_put(bt, r->pkey, r->ptf, r->pdl, r->prec, r->prlen);
			if (!runs_post_next(r))
			{
				act[m] = act[--live];
				break;
			}
			if (r->pkey >= limit)
				break;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/* Merges the terms of the runs, each positioned on its first (rd->len, 0 at its end), that are below hi (NULL: all) into mo */
void
runs_merge_terms(RunReader *rd, int nruns, bool positions, MergeOut *mo, const SiftTermCut *hi)
{
	RunReader **cur = palloc(nruns * sizeof(RunReader *));
	RunReader **act = palloc(nruns * sizeof(RunReader *));
	binaryheap *heap = binaryheap_allocate(nruns, runs_reader_cmp, NULL);
	Batch	   *bt = palloc0(sizeof(Batch));
	uint32		nterms = 0;

	bt->run = mo->sw ? NULL : &mo->io;
	for (int i = 0; i < nruns; i++)
		if (rd[i].len)
			binaryheap_add_unordered(heap, PointerGetDatum(&rd[i]));
	binaryheap_build(heap);
	while (!binaryheap_empty(heap))
	{
		char		term[SIFT_MAX_TERM_BYTES];
		int			len = ((RunReader *) DatumGetPointer(binaryheap_first(heap)))->len,
					k = 0;
		uint64		df = 0;

		memcpy(term, ((RunReader *) DatumGetPointer(binaryheap_first(heap)))->term, len);
		if (hi && sift_term_cmp(term, len, hi->term, hi->len) >= 0)
			break;
		while (!binaryheap_empty(heap))
		{
			RunReader  *top = (RunReader *) DatumGetPointer(binaryheap_first(heap));

			if (sift_term_cmp(top->term, top->len, term, len) != 0)
				break;
			cur[k++] = top;
			binaryheap_remove_first(heap);
		}
		/* a term whose postings lack the records in some run (inline there) keeps none */
		bt->haspos = positions;
		for (int i = 0; i < k; i++)
		{
			bt->haspos &= cur[i]->thaspos;
			df += cur[i]->df;
		}
		out_begin_term(mo, bt, term, len, df);
		merge_postings(bt, cur, k, act);
		batch_flush(bt);
		if (mo->sw)
			sift_segw_end_term(mo->sw);
		else
			runs_run_note_term(mo->run, len, df, bt->termpos);
		for (int i = 0; i < k; i++)
		{
			runs_reader_next_term(cur[i]);
			if (cur[i]->len)
				binaryheap_add(heap, PointerGetDatum(cur[i]));
		}
		TEST_MERGE_TERM(++nterms, df);
		if (nterms % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

/* Merges the docs and the terms of the opened runs into mo */
void
runs_merge_readers(RunReader *rd, int nruns, bool positions, MergeOut *mo)
{
	runs_merge_docs(mo, rd, nruns);
	for (int i = 0; i < nruns; i++)
		runs_reader_next_term(&rd[i]);
	runs_merge_terms(rd, nruns, positions, mo, NULL);
}
