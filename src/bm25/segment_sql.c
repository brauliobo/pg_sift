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
#include "col/pg.h"
#include "bm25/maint_int.h"
#include "core/sql.h"
#include "bm25/segment_sql.h"

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
	index = open_sift(PG_GETARG_OID(0));
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
	Relation	index = open_sift(PG_GETARG_OID(0));
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

/*
 * sift.worker_state(index) behind the sift.workers view: whether maintenance is at work on the index, and for an index whose last background step
 * failed what it was and when it is tried next. The delay is the one the background keeps (backoff_ms of bm25/maint.c). NULL while another session holds the index in a lock this probe would wait for (a REINDEX).
 */
Datum
sift_worker_state(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	index;
	SiftDirSnapshot *snap;
	Datum		v[8];
	bool		nulls[8] = {0};
	uint32		failures;
	int			sealed = 0;

	if (!ConditionalLockRelationOid(relid, AccessShareLock))
		PG_RETURN_NULL();
	index = try_index_open(relid, NoLock);
	if (index == NULL)
	{
		UnlockRelationOid(relid, AccessShareLock);
		PG_RETURN_NULL();
	}
	if (index->rd_rel->relam != sift_catalog()->am && !sift_col_is_index(index))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
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
	nulls[4] = nulls[6] = nulls[7] = nulls[5] || snap->meta.maint_failed_at == 0;
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
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/*
 * sift.index_format(rel): the on-disk format of the metapage of a sift index, a sift_col index or a sift_table, the minor a build must read to
 * open it. A newer minor than this build reads is refused here as everywhere (scripts/upgrade_check.sql reports it).
 */
Datum
sift_index_format(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Datum		v[2];
	bool		nulls[2] = {0};

	sift_dir_read(rel, snap);
	v[0] = Int32GetDatum(snap->meta.format_major);
	v[1] = Int32GetDatum(snap->meta.format_minor);
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
	index = open_sift(PG_GETARG_OID(0));
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

static void
dump_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
	for (int i = 0; i < n; i++)
		fp_add(arg, 0, keys[i], 0);
}

static bool
dump_term(void *arg, const char *term, int len, const SiftTermInfo *info, const SiftKey *keys,
		  const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen)
{
	uint64		h = sift_term_hash64(term, len);

	for (uint32 i = 0; i < info->df; i++)
		fp_add(arg, h, keys[i], tf[i]);
	return true;
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
	SiftScoreRef ref = meta_ref(snap);

	hc.bf = bloom_create(Max(nelems, 1), maintenance_work_mem, pg_prng_uint64(&pg_global_prng_state));
	for (int i = 0; i < snap->nsegs; i++)
	{
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftSegment *seg = sift_seg_open(index, &snap->segs[i], cxt);

			sift_seg_dump(seg, &ref, dump_docs, dump_term, hc.bf);
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

/*
 * sift.verify(index, heapallindexed): store invariants, metapage totals and
 * options, every immutable segment (sift_seg_walk with verify) and invariant
 * I1 between segments. ERRCODE_INDEX_CORRUPTED on the first violation.
 */
Datum
sift_verify(PG_FUNCTION_ARGS)
{
	Relation	index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift verify", ALLOCSET_DEFAULT_SIZES);
	MemoryContext walk = AllocSetContextCreate(cxt, "sift verify walk", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;
	const SiftOptions *opts;
	SiftAnalyzerConfig cfg;
	SiftSegment **segs;
	Snapshot	snapshot = PG_GETARG_BOOL(1) ? RegisterSnapshot(GetTransactionSnapshot()) : InvalidSnapshot;
	uint64		ndocs = 0,
				sum_dl = 0,
				nelems = 0;

	index = open_sift(PG_GETARG_OID(0));
	/* Keeps promotion, merges and the sweep from releasing what the directory snapshot references */
	LockPage(index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	sift_store_verify(index);
	sift_dir_read(index, snap);
	opts = sift_index_options(index);
	sift_analyzer_config_from_index(index, &cfg);
	if (snap->meta.k1 != (float4) opts->k1 || snap->meta.b != (float4) opts->b ||
		snap->meta.analyzer_hash != cfg.hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sift index \"%s\" was built with other options", RelationGetRelationName(index)),
				 errhint("REINDEX the index after changing k1, b or analyzer options.")));
	segs = palloc0(Max(snap->nsegs, 1) * sizeof(SiftSegment *));
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		SiftScoreRef ref = meta_ref(snap);
		SiftSegStats st;

		if (e->kind != SIFT_SEG_IMMUTABLE)
		{
			nelems += sift_wbuf_verify(index, e);
			continue;
		}
		ndocs += e->ndocs;
		sum_dl += e->sum_dl;
		segs[i] = sift_seg_open(index, e, cxt);
		old = MemoryContextSwitchTo(walk);
		sift_seg_walk(segs[i], &ref, true, &st, NULL, NULL);
		nelems += st.postings + e->ndocs;
		MemoryContextSwitchTo(old);
		MemoryContextReset(walk);
		for (int j = 0; j < i; j++)
			if (segs[j] && segs[j]->hdr->sb_hi >= segs[i]->hdr->sb_lo && segs[i]->hdr->sb_hi >= segs[j]->hdr->sb_lo)
				verify_disjoint(index, segs[j], segs[i]);
		sift_seg_close(segs[i]);
	}
	if (ndocs != snap->meta.ndocs || sum_dl != snap->meta.sum_dl)
		corrupt(index, "Metapage totals %llu docs and %llu tokens, segments %llu and %llu.",
				(unsigned long long) snap->meta.ndocs, (unsigned long long) snap->meta.sum_dl,
				(unsigned long long) ndocs, (unsigned long long) sum_dl);
	for (int i = 0; i < snap->nsegs; i++)
		if (segs[i])
			sift_seg_close(segs[i]);
	if (snapshot != InvalidSnapshot)
	{
		verify_heap(index, snap, snapshot, nelems, cxt);
		UnregisterSnapshot(snapshot);
	}
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, ShareLock);
	MemoryContextDelete(cxt);
	index_close(index, AccessShareLock);
	PG_RETURN_VOID();
}
