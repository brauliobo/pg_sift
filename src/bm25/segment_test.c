/*
 * bm25/segment_test.c - the sift._test_* functions on segments: postings, parts, formats and blob checks (test-only build)
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "bm25/segment_sql.h"
#include "funcapi.h"
#include "bm25/kernels.h"
#include "core/sql.h"

PG_FUNCTION_INFO_V1(sift_test_postings);

typedef struct PostingsOut
{
	ReturnSetInfo *rsinfo;
	uint64		gen;
} PostingsOut;

static bool
emit_term(void *arg, const char *term, int len, const SiftTermInfo *info,
		  const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen)
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

PG_FUNCTION_INFO_V1(sift_test_segment_parts);

typedef struct PartRow
{
	const char *name;
	uint64		bytes;
	uint64		items;			/* 0: not a count of something */
} PartRow;

static void
put_part(ReturnSetInfo *rsinfo, uint64 gen, const char *name, uint64 bytes, uint64 items)
{
	Datum		v[4] = {Int64GetDatum(gen), CStringGetTextDatum(name), Int64GetDatum(bytes), Int64GetDatum(items)};
	bool		nulls[4] = {0, 0, 0, items == 0};

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
}

/*
 * sift._test_segment_parts(index) returns (gen int8, part text, bytes int8, items int8): where the bytes of every immutable
 * segment go (decodes every term, like sift.segment_postings). items is the number the part is spent on: documents or terms.
 * explains the parts; the last rows are the pages of the liveness run, what none of the parts accounts
 * for (the page headers of the stream, the segment header, the unused end of the last page) and the bytes of the segment.
 */
Datum
sift_test_segment_parts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift test parts", ALLOCSET_DEFAULT_SIZES);

	InitMaterializedSRF(fcinfo, 0);
	index = open_sift(PG_GETARG_OID(0));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftScoreRef ref = meta_ref(snap);
		SiftSegStats st;
		SiftSegment *seg;
		MemoryContext old;
		uint64		gen = snap->segs[i].gen,
					ndocs = snap->segs[i].ndocs,
					live,
					known;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		old = MemoryContextSwitchTo(cxt);
		seg = sift_seg_open(index, &snap->segs[i], cxt);
		sift_seg_walk(seg, &ref, false, &st, NULL, NULL);
		live = (uint64) seg->hdr->live_npages * BLCKSZ;
		sift_seg_close(seg);
		MemoryContextSwitchTo(old);
		{
			const SiftPartStats *p = &st.parts;
			const PartRow rows[] = {
				{"docmap: dl rows", p->docmap_dl, ndocs},
				{"docmap: present docs", p->docmap_presence, ndocs},
				{"docmap: SB directory", p->docmap_dir, 0},
				{"postings: LIST codec", p->list_codec, 0},
				{"postings: GROUPED directory", p->grouped_dir, 0},
				{"postings: GROUPED containers", p->grouped_payload, 0},
				{"postings: group scores", p->gscore, 0},
				{"postings: block bounds", p->blkbound, 0},
				{"postings: tf L1 bits", p->tf_l1, 0},
				{"postings: tf nibbles", p->tf_nib, 0},
				{"postings: tf escapes", p->tf_esc, 0},
				{"postings: alignment", p->align_pad, 0},
				{"postings: position checkpoints", p->pos_ckpt, 0},
				{"postings: position records", p->pos_records, 0},
				{"dict: term suffixes", p->dict_terms, st.nterms},
				{"dict: entries", p->dict_entries, st.nterms},
				{"dict: inline keys and tf", p->dict_inline_keys, p->inline_terms},
				{"dict: inline positions", p->dict_inline_pos, 0},
				{"dict: block headers", p->dict_blocks, 0},
				{"dict: index and root", p->dictidx, 0},
			};

			known = live;
			for (int r = 0; r < lengthof(rows); r++)
			{
				known += rows[r].bytes;
				put_part(rsinfo, gen, rows[r].name, rows[r].bytes, rows[r].items);
			}
		}
		put_part(rsinfo, gen, "live pages", live, 0);
		put_part(rsinfo, gen, "other (page headers, segment header, unused end)", snap->segs[i].bytes - known, 0);
		put_part(rsinfo, gen, "segment total", snap->segs[i].bytes, 0);
		MemoryContextReset(cxt);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(sift_test_segment_formats);

/* sift._test_segment_formats(index) returns (gen int8, kind text, format int4): the segment format of every directory entry, and the metapage's minor version */
Datum
sift_test_segment_formats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	index = open_sift(PG_GETARG_OID(0));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		Datum		v[3] = {Int64GetDatum(snap->segs[i].gen), CStringGetTextDatum(kind_name(snap->segs[i].kind)),
		Int32GetDatum(snap->segs[i].format)};
		bool		nulls[3] = {0};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}
	{
		Datum		v[3] = {Int64GetDatum(0), CStringGetTextDatum("metapage minor"), Int32GetDatum(snap->meta.format_minor)};
		bool		nulls[3] = {0};

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(sift_test_term_blob);
PG_FUNCTION_INFO_V1(sift_test_blob_read);
PG_FUNCTION_INFO_V1(sift_test_blob_check);

/* sift._test_blob_check(blob) returns the verdict of the exhaustive blob check of sift.verify: NULL for a sound blob */
Datum
sift_test_blob_check(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	Size		len = VARSIZE_ANY_EXHDR(b);
	uint8	   *blob = MemoryContextAllocAligned(CurrentMemoryContext, len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, MCXT_ALLOC_ZERO);
	const char *detail;

	memcpy(blob, VARDATA_ANY(b), len);
	detail = sift_blob_check(blob, len);
	if (detail == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(detail));
}

/*
 * sift._test_term_blob(index, term) returns (blob bytea, flags int4, df int4, codec_len int4, nnib int4): the term blob of the first
 * immutable segment that has the term, as the query reads it (without positions), with the dictionary entry's lengths
 */
Datum
sift_test_term_blob(PG_FUNCTION_ARGS)
{
	Relation	index = open_sift(PG_GETARG_OID(0));
	text	   *term = PG_GETARG_TEXT_PP(1);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Datum		v[5];
	bool		nulls[5] = {0};

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftSegment *seg;
		SiftTermInfo info;
		Size		len;
		bytea	   *blob;
		const void *src;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_seg_open(index, &snap->segs[i], CurrentMemoryContext);
		if (!sift_seg_lookup(seg, VARDATA_ANY(term), VARSIZE_ANY_EXHDR(term), &info) || (info.flags & SIFT_TERM_INLINE))
		{
			sift_seg_close(seg);
			continue;
		}
		len = info.post_len - ((info.flags & SIFT_TERM_POS) ? info.pos_len : 0);
		blob = palloc(VARHDRSZ + len);
		SET_VARSIZE(blob, VARHDRSZ + len);
		src = sift_sr_get(&seg->sr, info.post_off, len, VARDATA(blob));
		if (src != VARDATA(blob))
			memcpy(VARDATA(blob), src, len);
		sift_seg_close(seg);
		v[0] = PointerGetDatum(blob);
		v[1] = Int32GetDatum(info.flags & ~SIFT_TERM_POS);
		v[2] = Int32GetDatum(info.df);
		v[3] = Int32GetDatum(info.codec_len);
		v[4] = Int32GetDatum(info.nnib);
		index_close(index, AccessShareLock);
		PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
	}
	index_close(index, AccessShareLock);
	ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("no segment has a blob for term \"%s\"", text_to_cstring(term))));
}

/*
 * sift._test_blob_read(blob, flags, df, codec_len, nnib) returns (postings int8, tf_sum int8): the query's read of a term blob, as
 * sift._test_term_blob returned it or after the test damaged it: every group and its tfs, without the checks of sift.verify
 */
Datum
sift_test_blob_read(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	Size		len = VARSIZE_ANY_EXHDR(b);
	SiftTermInfo info = {.flags = PG_GETARG_INT32(1),.df = PG_GETARG_INT32(2),.codec_len = PG_GETARG_INT32(3),.nnib = PG_GETARG_INT32(4),
	.post_len = len};
	uint8	   *blob = MemoryContextAllocAligned(CurrentMemoryContext, len + SIFT_KERN_PAD, SIFT_SIMD_ALIGN, MCXT_ALLOC_ZERO);
	SiftPostingCursor *pc;
	sift_cur   *cur;
	const sift_grp *g;
	uint64		postings = 0,
				tfsum = 0,
				groups = 0;
	uint8	   *tf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	Datum		v[2];
	bool		nulls[2] = {0};

	memcpy(blob, VARDATA_ANY(b), len);
	pc = sift_post_open_blob(&info, blob, CurrentMemoryContext);
	cur = sift_post_cursor(pc);
	while ((g = sift_next(cur)) != NULL)
	{
		uint64		n = sift_grp_count(g);

		/* a cursor ends after at most one group per posting and per group number: a damaged blob must not keep it running */
		if (n > SIFT_GRP_MAXTIDS || ++groups > Min(info.df, 1 << 24))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("cursor does not end: group of %llu postings, group number %llu", (unsigned long long) n, (unsigned long long) groups)));
		CHECK_FOR_INTERRUPTS();
		sift_post_group_tf(pc, tf);
		for (uint64 i = 0; i < n; i++)
			tfsum += tf[i];
		postings += n;
	}
	v[0] = Int64GetDatum(postings);
	v[1] = Int64GetDatum(tfsum);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

#endif							/* SIFT_TESTING */
