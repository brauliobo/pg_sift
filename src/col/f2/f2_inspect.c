/*
 * col/f2/f2_inspect.c - sift_index_size_breakdown(regclass): where the bytes of an index of format major 2 are, by category. Every block of the index is counted once, so the categories add up to
 * pg_relation_size: a page that is not part of a stream (metapage, manifest, allocator, delete vectors, slab) by its type, the streams of the live parts by section and, in the granule
 * section, by chunk, and what is free, quarantined or left over in a page as fsm_free and page_slack.
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "col/f2/f2.h"
#include "col/granule/granule_int.h"
#include "core/catname.h"
#include "core/f2/manifest.h"
#include "core/f2/meta.h"

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
	CAT_MANIFEST_META,
	CAT_FSM_FREE,
	CAT_PAGE_SLACK,
	CAT_COUNT
} SizeCat;

static const char *const cat_names[CAT_COUNT] = {
	"granule_tid", "granule_int", "granule_float", "granule_num", "granule_str", "granule_uuid", "granule_exc", "dicts", "zones_tstride", "preagg", "scope_meta", "tid_index",
	"delvec_penddel", "l0_slab", "manifest_meta", "fsm_free", "page_slack"
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
		Buffer		buf = sift_blk_read(rel, b, BUFFER_LOCK_SHARE);
		Page		page = BufferGetPage(buf);

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
granule_bytes(Relation rel, const SiftPart *part, uint64 *bytes)
{
	uint64		covered = 0;

	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		SiftGranuleDesc gd;
		char	   *rec = sift_part_read_granule(rel, part, g, &gd);
		SiftGranuleView v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);
		uint64		chunks = 0;

		for (int i = 0; i < v.hdr->nchunks; i++)
		{
			SiftChunk	c;
			uint64		exc;

			sift_chunk_open(&v, i, &c);
			exc = c.exc_len;
			bytes[CAT_GRANULE_EXC] += exc;
			bytes[chunk_cat(c.ref->enc)] += c.ref->len - Min(exc, c.ref->len);
			chunks += c.ref->len;
		}
		bytes[CAT_GRANULE_TID] += gd.data_len - chunks;
		covered += gd.data_len;
	}
	return covered;
}

/* The stream of a part: its sections by category, the rest of its pages as slack */
static uint64
stream_bytes(Relation rel, const SiftPart *part, uint64 *bytes)
{
	uint64		pages = SIFT_PAYLOAD_PAGES(part->hdr.stream_bytes);
	uint64		counted = 0;

	for (int i = 0; i < part->hdr.nsec; i++)
	{
		const SiftSecDirEntry *s = &part->secdir[i];
		uint64		got = s->len;

		if (s->tag == SEC_GRANULES)
			got = granule_bytes(rel, part, bytes);
		else
			bytes[section_cat(s->tag)] += s->len;
		counted += got;
	}
	bytes[CAT_PAGE_SLACK] += pages * BLCKSZ - counted;
	return pages;
}

Datum
sift_index_size_breakdown(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel = index_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	uint64		bytes[CAT_COUNT] = {0};
	uint64		live_pages = 0;
	SiftManifestRead all;
	uint64		data_pages,
				total;
	SiftColMetaPageData meta;
	SiftManifestScopeRef *scopes;
	int			nscopes;
	Datum		values[4];
	bool		nulls[4] = {false};

	if (!sift_col_is_index(rel) || sift_col_format_major(rel) != SIFT_FORMAT2_MAJOR)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("index \"%s\" is not a sift index of format major 2", RelationGetRelationName(rel))));
	meta = sift_col_meta_read(rel);
	all = sift_manifest_read_range(rel, NULL, NULL);
	for (int i = 0; i < all.n; i++)
	{
		int			n;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &all.entries[i], &n);

		for (int j = 0; j < n; j++)
		{
			SiftPart   *part = sift_part_open_desc(rel, &parts[j], (uint32) all.entries[i].part_id);

			live_pages += stream_bytes(rel, part, bytes);
		}
	}
	scopes = sift_manifest_read_scopes(rel, &meta, &nscopes);
	for (int i = 0; i < nscopes; i++)
		if ((scopes[i].flags & SIFT_SCOPEREF_ACTIVE) && scopes[i].header_blk != InvalidBlockNumber)
			live_pages += stream_bytes(rel, sift_part_open(rel, scopes[i].header_blk, scopes[i].meta_gen, 0), bytes);
	data_pages = census(rel, bytes);
	bytes[CAT_FSM_FREE] += (data_pages - live_pages) * BLCKSZ;
	total = (uint64) RelationGetNumberOfBlocks(rel) * BLCKSZ;

	InitMaterializedSRF(fcinfo, 0);
	for (int c = 0; c < CAT_COUNT; c++)
	{
		values[0] = CStringGetTextDatum(cat_names[c]);
		values[1] = Int64GetDatum((int64) bytes[c]);
		values[2] = DirectFunctionCall1(float8_numeric, Float8GetDatum(total ? rint(bytes[c] * 10000.0 / total) / 100 : 0));
		values[3] = DirectFunctionCall1(float8_numeric, Float8GetDatum(meta.total_rows ? rint((double) bytes[c] * 100 / meta.total_rows) / 100 : 0));
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
	}
	index_close(rel, AccessShareLock);
	return (Datum) 0;
}
