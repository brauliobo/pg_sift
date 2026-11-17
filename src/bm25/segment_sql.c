/*
 * bm25/segment_sql.c - sift.segments(), sift.segment_postings(),
 * sift.verify() and the postings round-trip test function.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "funcapi.h"
#include "lib/bloomfilter.h"
#include "storage/lmgr.h"
#include "utils/timestamp.h"

#include "bm25/wbuf.h"
#include "col/api/format.h"
#include "col/pg.h"
#include "bm25/maint_int.h"
#include "core/check.h"
#include "core/errctx.h"
#include "core/explain.h"
#include "core/sql.h"
#include "core/worker.h"
#include "table/table.h"
#include "bm25/segment_sql.h"
#include "core/membudget.h"

#define corrupt SIFT_CORRUPT

/*
 * sift.segments(index): one row per directory entry with header statistics
 * and bytes by section (immutable segments).
 */
Datum
sift_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift segments", ALLOCSET_DEFAULT_SIZES);

	InitMaterializedSRF(fcinfo, 0);
	index = open_sift(PG_GETARG_OID(0), "col_segments");
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		Datum		v[17];
		bool		nulls[17] = {0};

		v[0] = Int64GetDatum(e->gen);
		v[1] = CStringGetTextDatum(kind_name(e->kind));
		v[2] = Int64GetDatum(e->ndocs);
		v[3] = Int64GetDatum(e->sum_dl);
		v[4] = Float8GetDatum(e->ndocs ? (double) e->sum_dl / e->ndocs : 0);
		v[5] = Int64GetDatum(e->bytes);
		v[6] = Int32GetDatum(e->sb_lo);
		v[7] = Int32GetDatum(e->sb_hi);
		v[8] = BoolGetDatum((e->flags & SIFT_SEG_DIRTY) != 0);
		for (int k = 9; k < 17; k++)
			nulls[k] = true;
		if (e->kind != SIFT_SEG_IMMUTABLE)
		{
			uint64		ndocs,
						sum_dl,
						npages;

			sift_wbuf_summary(index, e, &ndocs, &sum_dl, &npages);
			v[2] = Int64GetDatum(ndocs);
			v[3] = Int64GetDatum(sum_dl);
			v[4] = Float8GetDatum(ndocs ? (double) sum_dl / ndocs : 0);
			v[5] = Int64GetDatum(npages * BLCKSZ);
		}
		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftSegment *seg = sift_seg_open(index, e, cxt);
			const SiftSectionRef *s = seg->hdr->sections;

			v[9] = Int64GetDatum(seg->hdr->nterms);
			v[10] = Float8GetDatum(seg->hdr->avgdl_ref);
			v[11] = Int64GetDatum(s[SIFT_SEC_DOCMAP].len);
			v[12] = Int64GetDatum(s[SIFT_SEC_POSTINGS].len);
			v[13] = Int64GetDatum(s[SIFT_SEC_DICT].len + s[SIFT_SEC_DICTIDX].len +
								  seg->hdr->dictroot_n * sizeof(uint64));
			v[14] = Int64GetDatum((int64) seg->hdr->live_npages * BLCKSZ);
			v[15] = Int32GetDatum(seg->hdr->nextents);
			v[16] = Int64GetDatum(e->ndocs - sift_seg_live_docs(seg));
			memset(nulls + 9, 0, 8);
			sift_seg_close(seg);
			MemoryContextReset(cxt);
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* The directory of the index as EXPLAIN (SIFT) reports it: no privilege is asked, a plan that reads the index has the right to the table */
void
sift_bm25_stats(Relation index, SiftIndexStats *st)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift index stats", ALLOCSET_DEFAULT_SIZES);

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftSegment *seg = sift_seg_open(index, e, cxt);

			st->segments++;
			st->rows += e->ndocs;
			st->dead_rows += e->ndocs - sift_seg_live_docs(seg);
			sift_seg_close(seg);
			MemoryContextReset(cxt);
		}
		else if (e->kind == SIFT_SEG_WBUF_OPEN || e->kind == SIFT_SEG_WBUF_SEALED)
		{
			uint64		ndocs,
						sum_dl,
						npages;

			sift_wbuf_summary(index, e, &ndocs, &sum_dl, &npages);
			st->buffered_rows += ndocs;
		}
	}
	MemoryContextDelete(cxt);
}

/* Is the maintenance lock held (a promotion, merge, VACUUM or verify is running)? A momentary probe. */
bool
sift_maintenance_busy(Relation index)
{
	if (!ConditionalLockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock))
		return true;
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
	return false;
}

/* sift.index_info(index): the space and directory state of the index as one row */
Datum
sift_index_info(PG_FUNCTION_ARGS)
{
	Relation	index = open_sift(PG_GETARG_OID(0), "col_index_info");
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftFreeStats fs;
	Datum		v[10];
	bool		nulls[10] = {0};

	sift_dir_read(index, snap);
	fs = sift_store_free_stats(index);
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(index));
	v[1] = Int64GetDatum(snap->meta.high_water);
	v[2] = Int64GetDatum(fs.blocks);
	v[3] = Int64GetDatum(fs.reusable);
	v[4] = Int64GetDatum(fs.records);
	v[5] = Int32GetDatum(snap->nsegs);
	v[6] = Int32GetDatum(SIFT_MAX_SEGMENTS);
	v[7] = Int64GetDatum(snap->meta.ndocs);
	v[8] = Int64GetDatum(snap->meta.sum_dl);
	nulls[9] = RecoveryInProgress();
	v[9] = BoolGetDatum(!nulls[9] && sift_maintenance_busy(index));
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/* The index or sift_table of a worker inspector, locked for reading; NULL while another session holds it in a lock the probe would wait for (a REINDEX), or when it is gone */
static Relation
worker_open(Oid relid)
{
	Relation	index;

	sift_inspect_expect(relid, NULL, NULL);
	if (!ConditionalLockRelationOid(relid, AccessShareLock))
		return NULL;
	index = try_relation_open(relid, NoLock);
	if (index == NULL)
	{
		UnlockRelationOid(relid, AccessShareLock);
		return NULL;
	}
	if (index->rd_rel->relkind == RELKIND_INDEX ? index->rd_rel->relam != sift_catalog()->am && !sift_col_is_index(index) : !sift_tbl_is_table(index))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
	sift_check_inspect(index);
	if (index->rd_rel->relkind == RELKIND_INDEX)
		sift_store_require_ready(index);
	return index;
}

/*
 * sift.worker_state(index) behind the sift.workers view: whether maintenance is at work on the index (or the sift_table: its failures are not noted, so they are 0),
 * and for an index whose last background step failed what it was and when it is tried next. The delay is the one the background keeps (backoff_ms of bm25/maint.c). NULL while another session holds the index in a lock this probe would wait for (a REINDEX).
 * The error stays after a later step that ended well (failed_at, error_code and last_error are those of the last failure ever noted); the next retry is only for the failures in a row.
 */
Datum
sift_worker_state(PG_FUNCTION_ARGS)
{
	Relation	index = worker_open(PG_GETARG_OID(0));
	SiftDirSnapshot *snap;
	Datum		v[8];
	bool		nulls[8] = {0};
	uint32		failures;
	int			sealed = 0;

	if (index == NULL)
		PG_RETURN_NULL();
	snap = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		sealed += snap->segs[i].kind == SIFT_SEG_WBUF_SEALED;
	failures = snap->meta.flags & SIFT_META_FAILURES_MASK;
	v[0] = Int32GetDatum(snap->nsegs);
	v[1] = Int32GetDatum(sealed);
	nulls[2] = RecoveryInProgress();
	v[2] = BoolGetDatum(!nulls[2] && sift_maintenance_busy(index));
	v[3] = Int32GetDatum(failures);
	nulls[5] = failures == 0;
	if (!nulls[5])
		v[5] = TimestampTzGetDatum(snap->meta.maint_requested_at +
								   backoff_ms(&snap->meta) * INT64CONST(1000));
	/* a build without the note counted the failure and left it zero */
	nulls[4] = nulls[6] = nulls[7] = snap->meta.maint_failed_at == 0;
	if (!nulls[4])
	{
		/* the note is a page's bytes: cut it where it ends, and refuse what is not text of the database encoding */
		char		msg[SIFT_MAINT_ERRMSG_LEN];

		memcpy(msg, snap->meta.maint_errmsg, sizeof(msg));
		msg[sizeof(msg) - 1] = '\0';
		v[4] = TimestampTzGetDatum(snap->meta.maint_failed_at);
		v[6] = CStringGetTextDatum(unpack_sql_state(snap->meta.maint_errcode));
		v[7] = CStringGetTextDatum(pg_verifymbstr(msg, strlen(msg), true) ? msg : "(unreadable)");
	}
	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/*
 * sift.worker_history(index) joined by the sift.workers view: when the last background step that ended well ran, which job it was and how long it took, and the failures noted
 * since the index was built. None of it for a sift_table (it has no metapage) and NULL where worker_state is.
 */
Datum
sift_worker_history(PG_FUNCTION_ARGS)
{
	Relation	index = worker_open(PG_GETARG_OID(0));
	SiftDirSnapshot *snap;
	Datum		v[4];
	bool		nulls[4] = {0};

	if (index == NULL)
		PG_RETURN_NULL();
	if (index->rd_rel->relkind != RELKIND_INDEX)
	{
		relation_close(index, AccessShareLock);
		nulls[0] = nulls[1] = nulls[2] = nulls[3] = true;
		PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
	}
	snap = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, snap);
	nulls[0] = nulls[1] = nulls[2] = snap->meta.maint_ok_at == 0;
	if (!nulls[0])
	{
		v[0] = TimestampTzGetDatum(snap->meta.maint_ok_at);
		v[1] = CStringGetTextDatum(sift_job_name(snap->meta.maint_ok_job) ? sift_job_name(snap->meta.maint_ok_job) : "");
		v[2] = Int64GetDatum(snap->meta.maint_ok_ms);
	}
	v[3] = Int64GetDatum(snap->meta.failures_total);
	relation_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/*
 * sift.index_format(rel): the on-disk format of the metapage of a sift index, a sift_col index or a sift_table, the minor a build must read to
 * open it. A newer minor than this build reads is refused here as everywhere (sift.upgrade_check reports it). long_words, for a sift (BM25) index
 * only: it was built by a release that stores a word over 255 bytes as a prefix and a hash (5.0 and later; an index of an earlier build says false
 * whether it was built before 3.3 or not, and a REINDEX tells). reads, for a sift_col index or a sift_table: the column formats this build reads.
 */
Datum
sift_index_format(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Datum		v[4];
	bool		nulls[4] = {0};
	bool		bm25 = rel->rd_rel->relam == sift_catalog()->am;

	sift_check_inspect(rel);
	sift_dir_read(rel, snap);
	v[0] = Int32GetDatum(snap->meta.format_major);
	v[1] = Int32GetDatum(snap->meta.format_minor);
	v[2] = BoolGetDatum((snap->meta.flags & SIFT_META_LONG_WORDS) != 0);
	v[3] = CStringGetTextDatum(psprintf("%d to %d", SIFT_COL_FORMAT_MIN_READ, SIFT_COL_FORMAT_MAX_READ));
	nulls[2] = !bm25;
	nulls[3] = bm25;
	relation_close(rel, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/*
 * sift.segment_postings(index): per immutable segment and df class, terms,
 * postings, representation counts and bytes (decodes every term).
 */
Datum
sift_segment_postings(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift segment postings", ALLOCSET_DEFAULT_SIZES);

	InitMaterializedSRF(fcinfo, 0);
	index = open_sift(PG_GETARG_OID(0), NULL);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftScoreRef ref = meta_ref(snap);
		SiftSegStats st;
		SiftSegment *seg;
		MemoryContext old;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		old = MemoryContextSwitchTo(cxt);
		seg = sift_seg_open(index, &snap->segs[i], cxt);
		sift_seg_walk(seg, &ref, false, &st, NULL, NULL);
		sift_seg_close(seg);
		MemoryContextSwitchTo(old);
		for (int c = 0; c < SIFT_DF_CLASSES; c++)
		{
			const SiftDfClassStats *s = &st.cls[c];
			Datum		v[10];
			bool		nulls[10] = {0};

			if (s->terms == 0)
				continue;
			v[0] = Int64GetDatum(snap->segs[i].gen);
			v[1] = CStringGetTextDatum(sift_df_class_name[c]);
			v[2] = Int64GetDatum(s->terms);
			v[3] = Int64GetDatum(s->postings);
			v[4] = Int64GetDatum(s->inline_terms);
			v[5] = Int64GetDatum(s->list_terms);
			v[6] = Int64GetDatum(s->grouped_terms);
			v[7] = Int64GetDatum(s->codec_bytes);
			v[8] = Int64GetDatum(s->sidecar_bytes);
			v[9] = Float8GetDatum(8.0 * (s->codec_bytes + s->sidecar_bytes) / s->postings);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
		}
		MemoryContextReset(cxt);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* Invariant I1 between two segments: no live doc in both (a vacuumed doc stays in its segment's docmap) */
static void
verify_disjoint(Relation index, SiftSegment *a, SiftSegment *b)
{
	uint32		lo = Max(a->hdr->sb_lo, b->hdr->sb_lo),
				hi = Min(a->hdr->sb_hi, b->hdr->sb_hi);
	SiftDocChunk da,
				db;

	for (uint32 sb = lo; sb <= hi && sift_seg_docs_next(a, sb, &da) && da.sb <= hi; sb = da.sb + 1)
	{
		if (!sift_seg_docs(b, da.sb, &db))
			continue;
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			uint64		pa[SIFT_OFFMASK_WORDS],
						pb[SIFT_OFFMASK_WORDS];

			if (!((da.pagemask[p / 64] & db.pagemask[p / 64]) >> (p % 64) & 1))
				continue;
			sift_seg_page_live(a, &da, p, pa);
			sift_seg_page_live(b, &db, p, pb);
			for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
				if (pa[w] & pb[w])
					corrupt(index, "Segments %llu and %llu both hold a doc on heap block %u.",
							(unsigned long long) a->entry.gen, (unsigned long long) b->entry.gen,
							da.sb * SIFT_SB_PAGES + p);
		}
	}
}

typedef struct HeapCheck
{
	bloom_filter *bf;
	SiftAnalyzer *an;
	Relation	index;
} HeapCheck;

/* One (term, doc, tf) triple; term hash 0 stands for the doc itself */
static void
fp_elem(uint8 *fp, uint64 term_hash, SiftKey key, uint8 tf)
{
	memcpy(fp, &term_hash, 8);
	memcpy(fp + 8, &key, 8);
	fp[16] = tf;
}

static void
fp_add(bloom_filter *bf, uint64 term_hash, SiftKey key, uint8 tf)
{
	uint8		fp[17];

	fp_elem(fp, term_hash, key, tf);
	bloom_add_element(bf, fp, sizeof(fp));
}

/* The filter and the hash of the term whose postings come next */
typedef struct HeapDump
{
	bloom_filter *bf;
	uint64		term_hash;
} HeapDump;

static void
dump_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
	for (int i = 0; i < n; i++)
		fp_add(((HeapDump *) arg)->bf, 0, keys[i], 0);
}

static void
dump_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	((HeapDump *) arg)->term_hash = sift_term_hash64(term, len);
}

static void
dump_postings(void *arg, const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen, int n)
{
	HeapDump   *d = arg;

	for (int i = 0; i < n; i++)
		fp_add(d->bf, d->term_hash, keys[i], tf[i]);
}

static void
add_wbuf(bloom_filter *bf, Relation index, const SiftSegEntry *e)
{
	SiftWbufReader *r = sift_wbuf_open(index, e, CurrentMemoryContext, NULL, NULL);
	SiftWbufDoc	d;

	while (sift_wbuf_next(r, &d))
	{
		SiftKey		key = sift_key_from_tid(&d.tid);
		const char *t = d.terms;

		if (d.deleted)
			continue;
		fp_add(bf, 0, key, 0);
		for (int i = 0; i < d.nterms; t += d.len[i++])
			fp_add(bf, sift_term_hash64(t, d.len[i]), key, d.tf[i]);
	}
	sift_wbuf_close(r);
}

static void
check_missing(HeapCheck *hc, ItemPointer tid, uint64 term_hash, uint8 tf, const char *term, int len)
{
	uint8		fp[17];

	fp_elem(fp, term_hash, sift_key_from_tid(tid), tf);
	if (bloom_lacks_element(hc->bf, fp, sizeof(fp)))
		corrupt(hc->index, "Heap tuple (%u,%u) has no %s%.*s%s in the index.",
				ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid),
				term ? "posting for term \"" : "entry", len, term ? term : "", term ? "\"" : "");
}

static void
heap_check_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	HeapCheck  *hc = state;
	text	   *t;
	SiftTermFreq *terms;
	uint32		dl;
	int			n;

	if (isnull[0])
		return;
	t = DatumGetTextPP(values[0]);
	n = sift_analyze_doc(hc->an, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &terms, &dl);
	check_missing(hc, tid, 0, 0, NULL, 0);
	for (int i = 0; i < n; i++)
		check_missing(hc, tid, sift_term_hash64(terms[i].term, terms[i].len), terms[i].tf, terms[i].term, terms[i].len);
	sift_analyzer_reset(hc->an);
}

/*
 * Every tuple visible to snapshot must have its doc and each analyzed term
 * (with tf) among the live postings: a Bloom filter of the postings has no
 * false negatives, so a miss is certain. The snapshot predates the directory
 * read, so every row it shows was inserted before the postings were read.
 */
static void
verify_heap(Relation index, const SiftDirSnapshot *snap, Snapshot snapshot, uint64 nelems, MemoryContext cxt)
{
	Oid			heapid = IndexGetRelation(RelationGetRelid(index), false);
	Relation	heap = table_open(heapid, AccessShareLock);
	IndexInfo  *info = BuildIndexInfo(index);
	SiftAnalyzerConfig cfg;
	HeapCheck	hc = {.index = index};
	HeapDump	dump = {0};

	hc.bf = bloom_create(Max(nelems, 1), maintenance_work_mem, pg_prng_uint64(&pg_global_prng_state));
	dump.bf = hc.bf;
	for (int i = 0; i < snap->nsegs; i++)
	{
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftSegment *seg = sift_seg_open(index, &snap->segs[i], cxt);

			sift_seg_stream(seg, (Size) maintenance_work_mem * 1024 / 2, &(SiftSegSink) {dump_docs, dump_term, dump_postings}, &dump);
			sift_seg_close(seg);
		}
		else
			add_wbuf(hc.bf, index, &snap->segs[i]);
	}
	sift_analyzer_config_from_index(index, &cfg);
	hc.an = sift_analyzer_create(&cfg, cxt);
	info->ii_Concurrent = true;
	table_index_build_scan(heap, index, info, true, false, heap_check_cb, &hc,
						   table_beginscan_strat(heap, snapshot, 0, NULL, true, false));
	bloom_free(hc.bf);
	table_close(heap, AccessShareLock);
}

/* One verification, in steps (core/check.h): the store and the directory, each segment, then the totals and the heap */
typedef struct Bm25Verify
{
	SiftErrCtx	errctx;
	Relation	index;
	SiftDirSnapshot *snap;
	MemoryContext cxt;
	MemoryContext walk;
	const SiftSegEntry *seg;
	Snapshot	snapshot;		/* taken before the directory was read: the heap rows it shows were inserted before the postings were read */
	uint64		ndocs,
				sum_dl,
				nelems;
} Bm25Verify;

static void
step_store(void *arg)
{
	Bm25Verify *bv = arg;
	const SiftOptions *opts;
	SiftAnalyzerConfig cfg;

	sift_store_verify(bv->index);
	sift_dir_read(bv->index, bv->snap);
	opts = sift_index_options(bv->index);
	sift_analyzer_config_from_index(bv->index, &cfg);
	if (bv->snap->meta.k1 != (float4) opts->k1 || bv->snap->meta.b != (float4) opts->b ||
		sift_dir_layout_differs(bv->snap, cfg.hash))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sift index \"%s\" was built with other options", RelationGetRelationName(bv->index)),
				 bv->snap->meta.analyzer_config[0] || cfg.tsconfig[0] ?
				 errdetail("The index was built with text search configuration \"%s\", the stemmer option names \"%s\".",
						   bv->snap->meta.analyzer_config[0] ? bv->snap->meta.analyzer_config : "(none)", cfg.tsconfig[0] ? cfg.tsconfig : "(none)") : 0,
				 errhint("REINDEX the index after changing k1, b or analyzer options.")));
}

/* Invariant I1 of the segment with the ones before it that cover blocks of its own (opened again for the check: no segment stays open between steps) */
static void
disjoint_from_earlier(Bm25Verify *bv, SiftSegment *seg)
{
	for (const SiftSegEntry *e = bv->snap->segs; e < bv->seg; e++)
	{
		SiftSegment *earlier;

		if (e->kind != SIFT_SEG_IMMUTABLE || e->sb_hi < bv->seg->sb_lo || bv->seg->sb_hi < e->sb_lo)
			continue;
		earlier = sift_seg_open(bv->index, e, bv->cxt);
		verify_disjoint(bv->index, earlier, seg);
		sift_seg_close(earlier);
	}
}

/* One segment, under the maintenance lock for its length alone: a promotion or merge waits for one segment, not for the index (the snapshot keeps the others readable) */
static void
step_segment(void *arg)
{
	Bm25Verify *bv = arg;
	const SiftSegEntry *e = bv->seg;
	SiftScoreRef ref = meta_ref(bv->snap);
	SiftSegStats st;
	SiftSegment *seg;
	MemoryContext old;

	LockPage(bv->index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	if (e->kind != SIFT_SEG_IMMUTABLE)
		bv->nelems += sift_wbuf_verify(bv->index, e);
	else
	{
		bv->ndocs += e->ndocs;
		bv->sum_dl += e->sum_dl;
		seg = sift_seg_open(bv->index, e, bv->cxt);
		old = MemoryContextSwitchTo(bv->walk);
		sift_seg_walk(seg, &ref, true, &st, NULL, NULL);
		bv->nelems += st.postings + e->ndocs;
		MemoryContextSwitchTo(old);
		MemoryContextReset(bv->walk);
		disjoint_from_earlier(bv, seg);
		sift_seg_close(seg);
	}
	UnlockPage(bv->index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
}

static void
step_totals(void *arg)
{
	Bm25Verify *bv = arg;

	if (bv->ndocs != bv->snap->meta.ndocs || bv->sum_dl != bv->snap->meta.sum_dl)
		corrupt(bv->index, "Metapage totals %llu docs and %llu tokens, segments %llu and %llu.",
				(unsigned long long) bv->snap->meta.ndocs, (unsigned long long) bv->snap->meta.sum_dl,
				(unsigned long long) bv->ndocs, (unsigned long long) bv->sum_dl);
}

static void
step_heap(void *arg)
{
	Bm25Verify *bv = arg;

	verify_heap(bv->index, bv->snap, bv->snapshot, bv->nelems, bv->cxt);
}

/*
 * sift.verify(index, heapallindexed): store invariants, metapage totals and options, every immutable segment (sift_seg_walk with verify) and invariant
 * I1 between segments. ERRCODE_INDEX_CORRUPTED on the first violation of the raising form, a row for each segment (and for the totals and the heap) of sift.check.
 */
static void
bm25_verify(SiftCheck *ck, Relation index, bool heapallindexed)
{
	Bm25Verify *bv = palloc0(sizeof(Bm25Verify));
	MemoryContext old = CurrentMemoryContext;
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	bool		listed,
				sound;

	bv->index = index;
	bv->snapshot = snapshot;
	bv->snap = palloc(sizeof(SiftDirSnapshot));
	bv->cxt = AllocSetContextCreate(old, "sift verify", ALLOCSET_DEFAULT_SIZES);
	bv->walk = AllocSetContextCreate(bv->cxt, "sift verify walk", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bv->cxt);
	SIFT_MEM_SCOPE(bm25_verify, bv->cxt);
	sift_errctx_push_diagnostic(&bv->errctx, RelationGetRelid(index));
	/* registered before the directory is read, and kept: the segments of the copy stay readable while they are verified one at a time */
	sound = listed = sift_check_step(ck, SIFT_CHECK_WHOLE, step_store, bv);
	for (int i = 0; listed && i < bv->snap->nsegs; i++)
	{
		bv->seg = &bv->snap->segs[i];
		sift_errctx_segment(&bv->errctx, bv->seg->gen);
		sound &= sift_check_step(ck, bv->seg->gen, step_segment, bv);
		SIFT_MEM_CHECKPOINT();
	}
	sift_errctx_whole(&bv->errctx);
	if (sound)
		sound = sift_check_step(ck, SIFT_CHECK_WHOLE, step_totals, bv);
	if (sound && heapallindexed)
		sift_check_step(ck, SIFT_CHECK_WHOLE, step_heap, bv);
	sift_errctx_pop(&bv->errctx);
	UnregisterSnapshot(snapshot);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(bv->cxt);
}

static Datum
bm25_verify_sql(PG_FUNCTION_ARGS)
{
	Relation	index = open_sift(PG_GETARG_OID(0), NULL);

	sift_check_maintain(index);	/* it holds the maintenance lock: a role that only reads must not hold maintenance back */
	bm25_verify(NULL, index, PG_GETARG_BOOL(1));
	index_close(index, AccessShareLock);
	PG_RETURN_VOID();
}

void
sift_bm25_check(SiftCheck *ck, Oid indexoid, bool heapallindexed)
{
	Relation	index = open_sift(indexoid, NULL);

	sift_check_maintain(index);
	bm25_verify(ck, index, heapallindexed);
	index_close(index, AccessShareLock);
}

Datum
sift_verify(PG_FUNCTION_ARGS)
{
	if (sift_col_is_index_oid(PG_GETARG_OID(0)))
		return sift_col_verify(fcinfo);
	return bm25_verify_sql(fcinfo);
}
