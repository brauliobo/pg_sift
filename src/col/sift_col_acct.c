/*
 * sift_col_acct.c - where the bytes of a column index or a sift_table go: sift.col_sections (docs/COL.md section 10). One row per kind of
 * bytes, summed over the immutable segments, and the rows add up to the size of the relation (pg_relation_size) so that nothing is
 * left out: chunks and dictionaries per leaf, unit directories, zones, pre-aggregates, page overhead, headers, free blocks, the rest.
 */
#include "postgres.h"

#include "access/relation.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "sift_col_am.h"
#include "sift_col_seg.h"
#include "sift_table.h"

typedef struct SectionRows
{
	ReturnSetInfo *rsi;
	int64		total;			/* bytes accounted so far */
} SectionRows;

static void
put_row(SectionRows *r, const char *section, const char *leaf, int64 bytes)
{
	Datum		v[3];
	bool		nulls[3] = {false, leaf == NULL, false};

	v[0] = CStringGetTextDatum(section);
	v[1] = leaf ? CStringGetTextDatum(leaf) : (Datum) 0;
	v[2] = Int64GetDatum(bytes);
	tuplestore_putvalues(r->rsi->setResult, r->rsi->setDesc, v, nulls);
	r->total += bytes;
}

static const char *
leaf_name(Relation rel, const SiftColDef *def)
{
	return def->path[0] ? def->path : NameStr(TupleDescAttr(RelationGetDescr(rel), def->attno - 1)->attname);
}

/* The per-segment sums of the sections that are not per leaf */
typedef struct SegSums
{
	int64		unit_dir;		/* unit records less their chunks: directory, offsets, alignment */
	int64		dict_other;		/* DICTS less the dictionaries themselves */
	int64		zones;
	int64		preagg;
	int64		sbdir_schema;
	int64		page_overhead;	/* page headers and the unused end of the last page of every stream */
	int64		headers;		/* SEGHEADER and LIVE pages */
} SegSums;

static void
add_segment(Relation rel, SiftColSegment *seg, const SiftColSchema *sc, int64 *chunks, int64 *dicts, SegSums *s)
{
	const SiftSegHeaderData *h = seg->hdr;
	int64		chunk_total = 0,
				dict_total = 0;
	uint64		stream_pages = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCUnitView v;
		uint32		len;
		const uint8 *rec = sift_cseg_unit(seg, u, &len);

		if (!sift_cunit_parse(rec, len, sc, &v))
			SIFT_CORRUPT(rel, "Unit %u of segment %llu does not parse.", u, (unsigned long long) seg->entry.gen);
		for (int c = 0; c < sc->ncols; c++)
			if (v.chunk_len[c] > 0)
			{
				chunks[c] += v.chunk_len[c];
				chunk_total += v.chunk_len[c];
			}
	}
	for (int c = 0; c < sc->ncols; c++)
	{
		int64		stored = sift_cseg_dict_stored(seg, c);

		dicts[c] += stored;
		dict_total += stored;
	}
	for (uint32 i = 0; i < h->nextents; i++)
		stream_pages += h->extents[i].nblocks;
	s->preagg += sift_cseg_preagg_bytes(seg);
	s->unit_dir += h->sections[SIFT_SEC_POSTINGS].len - chunk_total;
	s->dict_other += h->sections[SIFT_SEC_DICT].len - dict_total;
	s->zones += h->sections[SIFT_SEC_DICTIDX].len - sift_cseg_preagg_bytes(seg);
	s->sbdir_schema += h->sections[SIFT_SEC_DOCMAP].len;
	s->page_overhead += (int64) stream_pages * BLCKSZ - (int64) (h->sections[SIFT_SEC_DOCMAP].off + h->sections[SIFT_SEC_DOCMAP].len);
	s->headers += (int64) (1 + h->live_npages) * BLCKSZ;
}

/*
 * sift.col_sections(rel regclass): (section, leaf, bytes) for a sift_col index or a sift_table. Sections: chunks and dict (per leaf),
 * unit_dir, dict_other, zones, preagg, sbdir_schema, page_overhead, headers, free, other (the metapage, directory, free list pages and the
 * segments that are not column segments: the write buffer, tombstones). The bytes of all rows add up to pg_relation_size(rel).
 */
Datum
sift_col_sections(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SectionRows r = {.rsi = rsi};
	SegSums		s = {0};
	const SiftColSchema *sc = NULL;
	int64	   *chunks = NULL,
			   *dicts = NULL;
	SiftFreeStats fs;
	int64		size;

	if (!(rel->rd_rel->relkind == RELKIND_INDEX ? sift_col_is_index(rel) : rel->rd_rel->relkind == RELKIND_RELATION && sift_tbl_is_table(rel)))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index or a sift_table", RelationGetRelationName(rel))));
	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(rel, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(rel, &snap->segs[i], CurrentMemoryContext);
		if (sc == NULL)
		{
			sc = seg->schema;
			chunks = palloc0(sc->ncols * sizeof(int64));
			dicts = palloc0(sc->ncols * sizeof(int64));
		}
		add_segment(rel, seg, sc, chunks, dicts, &s);
		sift_cseg_close(seg);
	}
	for (int c = 0; sc && c < sc->ncols; c++)
	{
		put_row(&r, "chunks", leaf_name(rel, &sc->cols[c]), chunks[c]);
		if (dicts[c] > 0)
			put_row(&r, "dict", leaf_name(rel, &sc->cols[c]), dicts[c]);
	}
	put_row(&r, "unit_dir", NULL, s.unit_dir);
	put_row(&r, "dict_other", NULL, s.dict_other);
	put_row(&r, "zones", NULL, s.zones);
	put_row(&r, "preagg", NULL, s.preagg);
	put_row(&r, "sbdir_schema", NULL, s.sbdir_schema);
	put_row(&r, "page_overhead", NULL, s.page_overhead);
	put_row(&r, "headers", NULL, s.headers);
	fs = sift_store_free_stats(rel);
	put_row(&r, "free", NULL, fs.blocks * BLCKSZ);
	size = (int64) RelationGetNumberOfBlocks(rel) * BLCKSZ;
	put_row(&r, "other", NULL, size - r.total);
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}
