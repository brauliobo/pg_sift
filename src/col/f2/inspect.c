/*
 * col/f2/inspect.c - sift_index_size_breakdown(regclass) and sift.col_sections of format major 2: where the bytes of a sift_col index or a sift_table are, by category. Every block of the index is counted once, so the categories add up to
 * pg_relation_size: a page that is not part of a stream (metapage, manifest, allocator, delete vectors, slab) by its type, the streams of the live parts by section and, in the granule
 * section, by chunk, and what is free, quarantined or left over in a page as fsm_free and page_slack. sift.col_sections splits the chunks of an index by leaf.
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/meta.h"
#include "col/f2/choices.h"
#include "col/f2/f2.h"
#include "col/f2/cache.h"
#include "col/granule/int.h"
#include "core/catname.h"
#include "core/explain.h"
#include "core/f2/manifest/manifest.h"
#include "core/f2/meta.h"
#include "core/sql/api.h"

PG_FUNCTION_INFO_V1(sift_index_size_breakdown);

typedef enum SizeCat
{
	CAT_GRANULE_TID,
	CAT_GRANULE_INT,
	CAT_GRANULE_FLOAT,
	CAT_GRANULE_NUM,
	CAT_GRANULE_STR,
	CAT_GRANULE_UUID,
	CAT_GRANULE_EXC,
	CAT_DICTS,
	CAT_ZONES_TSTRIDE,
	CAT_PREAGG,
	CAT_SCOPE_META,
	CAT_TID_INDEX,
	CAT_DELVEC_PENDDEL,
	CAT_L0_SLAB,
	CAT_COL_DELTA,
	CAT_DIRECTORIES,
	CAT_MANIFEST_META,
	CAT_FSM_FREE,
	CAT_PAGE_SLACK,
	CAT_COUNT
} SizeCat;

static const char *const cat_names[CAT_COUNT] = {
	"granule_tid", "granule_int", "granule_float", "granule_num", "granule_str", "granule_uuid", "granule_exc", "dicts", "zones_tstride", "preagg", "scope_meta", "tid_index",
	"delvec_penddel", "l0_slab", "col_delta", "directories", "manifest_meta", "fsm_free", "page_slack"
};

static SizeCat
chunk_cat(uint16 enc)
{
	switch (enc & SIFT_ENC_MASK)
	{
		case SIFT_CENC_TID_DELTA:
		case SIFT_CENC_TID_BLOCK:
			return CAT_GRANULE_TID;
		case SIFT_CENC_FLOAT_RAW:
		case SIFT_CENC_ALP:
		case SIFT_CENC_SCALE:
		case SIFT_CENC_FLOAT_BSS:
		case SIFT_CENC_ALP_RD:
			return CAT_GRANULE_FLOAT;
		case SIFT_CENC_NUM64:
		case SIFT_CENC_NUM128:
			return CAT_GRANULE_NUM;
		case SIFT_CENC_DICT_CODE:
		case SIFT_CENC_STR_INLINE:
		case SIFT_CENC_STR_VIEW:
		case SIFT_CENC_STR_FSST:
		case SIFT_CENC_STR_BLOCK:
		case SIFT_CENC_STR_PROMO:
			return CAT_GRANULE_STR;
		case SIFT_CENC_UUID16:
		case SIFT_CENC_INTERVAL16:
		case SIFT_CENC_FIXED16:
			return CAT_GRANULE_UUID;
		default:
			return CAT_GRANULE_INT;
	}
}

static SizeCat
section_cat(uint16 tag)
{
	switch (tag)
	{
		case SEC_GRANULE_DIR:
		case SEC_TID_INDEX:
		case SEC_TID_HASH:
		case SEC_ROW_PACK:
			return CAT_TID_INDEX;
		case SEC_DICTS:
			return CAT_DICTS;
		case SEC_ZONES:
		case SEC_KEY_MARKS:
		case SEC_COL_STATS:
		case SEC_GRANULE_TSTRIDE:
			return CAT_ZONES_TSTRIDE;
		case SEC_DELVEC:
			return CAT_DELVEC_PENDDEL;
		case SEC_PREAGG:
		case SEC_PREAGG_SUPER:
		case SEC_PREAGG_ROLLUP:
		case SEC_PREAGG_EXCEPT:
		case SEC_SETS:
		case SEC_HLL:
		case SEC_KTAB:
		case SEC_RUNS:
		case SEC_POINT:
		case SEC_SKIP_INDEX:
		case SEC_KLL:
			return CAT_PREAGG;
		default:
			return CAT_SCOPE_META;
	}
}

static SizeCat
page_cat(uint16 type)
{
	switch (type)
	{
		case SIFT_PT_SCOPEDIR:
			return CAT_SCOPE_META;
		case SIFT_PT_DELVEC:
		case SIFT_PT_PEND_DEL:
			return CAT_DELVEC_PENDDEL;
		case SIFT_PT_L0_SLAB:
			return CAT_L0_SLAB;
		case SIFT_PT_COL_DELTA:
		case SIFT_PT_COL_DELTA_DIR:
			return CAT_COL_DELTA;
		case SIFT_PT_TIERDIR:
		case SIFT_PT_PROJDIR:
		case SIFT_PT_KEYJRNL:
			return CAT_DIRECTORIES;
		default:
			return CAT_MANIFEST_META;
	}
}

/* The pages that are no stream of a live part, by type; the stream pages (SIFT_PT_DATA) are counted, and the free ones */
static uint64
census(Relation rel, uint64 *bytes)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	uint64		data_pages = 0;

	for (BlockNumber b = 0; b < nblocks; b++)
	{
		Buffer		buf;
		Page		page;

		CHECK_FOR_INTERRUPTS();
		buf = sift_blk_read(rel, b, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || !sift_page_layout_ok((const char *) page) || SiftPageGetOpaque(page)->type == SIFT_PT_FREE)
			bytes[CAT_FSM_FREE] += BLCKSZ;
		else if (SiftPageGetOpaque(page)->type == SIFT_PT_DATA)
			data_pages++;
		else
		{
			uint32		used = SiftPageGetUsed(page) + SIFT_PAGE_DATA_OFFSET;

			bytes[page_cat(SiftPageGetOpaque(page)->type)] += used;
			bytes[CAT_PAGE_SLACK] += BLCKSZ - used;
		}
		UnlockReleaseBuffer(buf);
	}
	return data_pages;
}

/* The chunks of the granules of a part: by encoding, the exception blocks apart; the framing of the granules and what lies between them goes with the _tid */
static uint64
granule_bytes(Relation rel, const SiftPart *part, uint64 *bytes, uint64 *leaf, int nleaf)
{
	uint64		covered = 0;

	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		SiftGranuleDesc gd;
		char	   *rec;
		SiftGranuleView v;
		uint64		chunks = 0;

		CHECK_FOR_INTERRUPTS();
		rec = sift_part_read_granule(rel, part, g, &gd);
		v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);
		for (int i = 0; i < v.hdr->nchunks; i++)
		{
			SiftChunk	c;
			uint64		exc;

			sift_chunk_open(&v, i, &c);
			exc = c.exc_len;
			chunks += c.ref->len;
			if (i >= 1 && i <= nleaf)
			{
				leaf[i - 1] += c.ref->len;
				continue;
			}
			bytes[CAT_GRANULE_EXC] += exc;
			bytes[chunk_cat(c.ref->enc)] += c.ref->len - Min(exc, c.ref->len);
		}
		bytes[CAT_GRANULE_TID] += gd.data_len - chunks;
		covered += gd.data_len;
	}
	return covered;
}

/* The stream of a part: its sections by category, the rest of its pages as slack */
static uint64
stream_bytes(Relation rel, const SiftPart *part, uint64 *bytes, uint64 *leaf, int nleaf)
{
	uint64		pages = SIFT_PAYLOAD_PAGES(part->hdr.stream_bytes);
	uint64		counted = 0;

	for (int i = 0; i < part->hdr.nsec; i++)
	{
		const SiftSecDirEntry *s = &part->secdir[i];
		uint64		got = s->len;

		if (s->tag == SEC_GRANULES)
			got = granule_bytes(rel, part, bytes, leaf, nleaf);
		else
			bytes[section_cat(s->tag)] += s->len;
		counted += got;
	}
	bytes[CAT_PAGE_SLACK] += pages * BLCKSZ - counted;
	return pages;
}

/* Every block of the relation once: the categories in bytes, and the chunks of the first nleaf columns of each granule (chunk 0 is the _tid) in leaf (NULL, 0: by category) */
static uint64
size_census(Relation rel, const SiftColMetaPageData *meta, uint64 *bytes, uint64 *leaf, int nleaf)
{
	uint64		live_pages = 0;
	SiftManifestRead all = sift_manifest_read_range(rel, NULL, NULL);
	SiftManifestScopeRef *scopes;
	int			nscopes;
	uint64		data_pages;

	for (int i = 0; i < all.n; i++)
	{
		int			n;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &all.entries[i], &n);

		for (int j = 0; j < n; j++)
			if (!(parts[j].ext_flags & SIFT_PART_EXT_REMOTE))
				live_pages += stream_bytes(rel, sift_part_open_desc(rel, &parts[j], (uint32) all.entries[i].part_id), bytes, leaf, nleaf);
	}
	scopes = sift_manifest_read_scopes(rel, meta, &nscopes);
	for (int i = 0; i < nscopes; i++)
		if ((scopes[i].flags & SIFT_SCOPEREF_ACTIVE) && scopes[i].header_blk != InvalidBlockNumber)
			live_pages += stream_bytes(rel, sift_part_open(rel, scopes[i].header_blk, scopes[i].meta_gen, 0), bytes, leaf, nleaf);
	data_pages = census(rel, bytes);
	bytes[CAT_FSM_FREE] += (data_pages - Min(data_pages, live_pages)) * BLCKSZ;
	return (uint64) RelationGetNumberOfBlocks(rel) * BLCKSZ;
}

static Relation
open_inspected(Oid relid, SiftApiRel *r)
{
	*r = sift_api_open(relid, AccessShareLock);
	if (!r->f2)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("index \"%s\" is not a sift index of format major 2", RelationGetRelationName(r->rel))));
	return r->rel;
}

Datum
sift_index_size_breakdown(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftApiRel	r;
	Relation	rel = open_inspected(PG_GETARG_OID(0), &r);
	uint64		bytes[CAT_COUNT] = {0};
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	uint64		total = size_census(rel, &meta, bytes, NULL, 0);
	Datum		values[4];
	bool		nulls[4] = {false};

	InitMaterializedSRF(fcinfo, 0);
	for (int c = 0; c < CAT_COUNT; c++)
	{
		values[0] = CStringGetTextDatum(cat_names[c]);
		values[1] = Int64GetDatum((int64) bytes[c]);
		values[2] = DirectFunctionCall1(float8_numeric, Float8GetDatum(total ? rint(bytes[c] * 10000.0 / total) / 100 : 0));
		values[3] = DirectFunctionCall1(float8_numeric, Float8GetDatum(meta.total_rows ? rint((double) bytes[c] * 100 / meta.total_rows) / 100 : 0));
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
	}
	sift_api_close(&r, AccessShareLock);
	return (Datum) 0;
}

static void
put_section(ReturnSetInfo *rsi, const char *section, const char *leaf, uint64 bytes)
{
	Datum		v[3] = {CStringGetTextDatum(section), leaf != NULL ? CStringGetTextDatum(leaf) : (Datum) 0, Int64GetDatum((int64) bytes)};
	bool		nulls[3] = {false, leaf == NULL, false};

	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
}

/* sift.col_sections(rel): the categories of the breakdown as sections, the chunks of a sift_col index by leaf; the rows add up to the size of the relation. The parts of a sift_table hold the columns of the schema they were written under, so its chunks stay by category */
Datum
sift_f2_sections(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftApiRel	r;
	Relation	rel = open_inspected(PG_GETARG_OID(0), &r);
	const SiftColSchema *schema = r.am == SIFT_API_COL ? sift_col_rel(rel)->schema : NULL;
	int			nleaf = schema != NULL ? schema->ncols : 0;
	uint64		bytes[CAT_COUNT] = {0};
	uint64	   *leaf = palloc0(Max(nleaf, 1) * sizeof(uint64));
	SiftColMetaPageData meta = sift_col_meta_read(rel);

	size_census(rel, &meta, bytes, leaf, nleaf);
	InitMaterializedSRF(fcinfo, 0);
	for (int c = 0; c < nleaf; c++)
		put_section(rsi, "chunks", schema->cols[c].path != NULL && schema->cols[c].path[0] != '\0' ? schema->cols[c].path : NameStr(TupleDescAttr(rel->rd_att, schema->cols[c].attno - 1)->attname), leaf[c]);
	for (int c = 0; c < CAT_COUNT; c++)
		put_section(rsi, cat_names[c], NULL, bytes[c]);
	sift_api_close(&r, AccessShareLock);
	return (Datum) 0;
}

/* The counts of an index of Format 2.0 as EXPLAIN (SIFT) reports them: its parts and the rows and dead rows its manifest holds */
void
sift_f2_index_stats(Relation index, SiftIndexStats *st)
{
	const SiftPlanSnapshot *v = sift_plan_cache_get(index, CurrentMemoryContext);

	for (int i = 0; i < v->npartitions; i++)
		st->segments += v->partlists[i].nparts;
	st->rows = (int64) v->total_rows;
	st->dead_rows = (int64) v->total_dead;
}
