/*
 * sift_segment_sql.c - sift.segments(), sift.segment_postings(),
 * sift.verify() and the postings round-trip test function.
 */
#include "postgres.h"

#include "access/genam.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift_analyze.h"
#include "sift_segment.h"
#include "sift_sql.h"

#define corrupt(index, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), \
			 errdetail(__VA_ARGS__)))

static Relation
open_sift(Oid relid)
{
	Relation	index = index_open(relid, AccessShareLock);

	if (index->rd_rel->relam != sift_catalog()->am)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
	return index;
}

static SiftScoreRef
meta_ref(const SiftDirSnapshot *snap)
{
	return (SiftScoreRef) {snap->meta.k1, snap->meta.b, 0};
}

static const char *
kind_name(uint8 kind)
{
	switch (kind)
	{
		case SIFT_SEG_IMMUTABLE:
			return "immutable";
		case SIFT_SEG_MUTABLE_ACTIVE:
			return "mutable";
		case SIFT_SEG_MUTABLE_SEALED:
			return "sealed";
	}
	return "unknown";
}

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
		Datum		v[15];
		bool		nulls[15] = {0};

		v[0] = Int64GetDatum(e->gen);
		v[1] = CStringGetTextDatum(kind_name(e->kind));
		v[2] = Int64GetDatum(e->ndocs);
		v[3] = Int64GetDatum(e->sum_dl);
		v[4] = Float8GetDatum(e->ndocs ? (double) e->sum_dl / e->ndocs : 0);
		v[5] = Int64GetDatum(e->bytes);
		v[6] = Int32GetDatum(e->sb_lo);
		v[7] = Int32GetDatum(e->sb_hi);
		v[8] = BoolGetDatum((e->flags & SIFT_SEG_DIRTY) != 0);
		for (int k = 9; k < 15; k++)
			nulls[k] = true;
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
			memset(nulls + 9, 0, 6);
			sift_seg_close(seg);
			MemoryContextReset(cxt);
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
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

/* Invariant I1 between two segments: no doc in both */
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
			const uint16 *dl;

			if (!((da.pagemask[p / 64] & db.pagemask[p / 64]) >> (p % 64) & 1))
				continue;
			sift_seg_page(a, &da, p, pa, &dl);
			sift_seg_page(b, &db, p, pb, &dl);
			for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
				if (pa[w] & pb[w])
					corrupt(index, "Segments %llu and %llu both hold a doc on heap block %u.",
							(unsigned long long) a->entry.gen, (unsigned long long) b->entry.gen,
							da.sb * SIFT_SB_PAGES + p);
		}
	}
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
	uint64		ndocs = 0,
				sum_dl = 0;

	if (PG_GETARG_BOOL(1))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift: sift.verify(heapallindexed => true) is not implemented yet")));
	index = open_sift(PG_GETARG_OID(0));
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
			continue;
		ndocs += e->ndocs;
		sum_dl += e->sum_dl;
		segs[i] = sift_seg_open(index, e, cxt);
		old = MemoryContextSwitchTo(walk);
		sift_seg_walk(segs[i], &ref, true, &st, NULL, NULL);
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
	MemoryContextDelete(cxt);
	index_close(index, AccessShareLock);
	PG_RETURN_VOID();
}

#ifdef SIFT_TESTING

PG_FUNCTION_INFO_V1(sift_test_postings);

typedef struct PostingsOut
{
	ReturnSetInfo *rsinfo;
	uint64		gen;
} PostingsOut;

static bool
emit_term(void *arg, const char *term, int len, const SiftTermInfo *info,
		  const SiftKey *keys, const uint8 *tf, const uint16 *dl)
{
	PostingsOut *o = arg;
	Datum		v[5];
	bool		nulls[5] = {0};

	v[0] = Int64GetDatum(o->gen);
	v[1] = PointerGetDatum(cstring_to_text_with_len(term, len));
	for (uint32 i = 0; i < info->df; i++)
	{
		ItemPointerData tid;

		sift_key_to_tid(keys[i], &tid);
		v[2] = PointerGetDatum(&tid);
		v[3] = Int32GetDatum(tf[i]);
		v[4] = Int32GetDatum(dl[i]);
		tuplestore_putvalues(o->rsinfo->setResult, o->rsinfo->setDesc, v, nulls);
	}
	return true;
}

/* sift._test_postings(index): every posting of every immutable segment, decoded */
Datum
sift_test_postings(PG_FUNCTION_ARGS)
{
	PostingsOut o = {(ReturnSetInfo *) fcinfo->resultinfo};
	Relation	index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift test postings", ALLOCSET_DEFAULT_SIZES);

	InitMaterializedSRF(fcinfo, 0);
	index = open_sift(PG_GETARG_OID(0));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftScoreRef ref = meta_ref(snap);
		SiftSegStats st;
		SiftSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		o.gen = snap->segs[i].gen;
		seg = sift_seg_open(index, &snap->segs[i], cxt);
		sift_seg_walk(seg, &ref, false, &st, emit_term, &o);
		sift_seg_close(seg);
		MemoryContextReset(cxt);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
