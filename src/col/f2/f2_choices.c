/*
 * col/f2/f2_choices.c - sift.index_choices(index) of format major 2. A Format 1 index reports what its CREATE INDEX derived; a Format 2.0 index and a sift_table keep nothing of that, so the rows
 * are read back from the file: the layout (parts, granules, rows), the compression tier of each level, the tiers (L0 slab, object storage), the sections each part carries (zone maps, skipping
 * indexes, point and row directories, sketches, pre-aggregates) and, for a sift_col index, the encodings chosen for each column, counted over the chunks of its granules. kind is always auto: a
 * choice the file holds. A part in object storage is counted from its descriptor; its stream is not fetched.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class_d.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/col_meta.h"
#include "col/f2/f2_choices.h"
#include "col/f2/part_format.h"
#include "col/granule/granule_int.h"
#include "core/f2/manifest.h"
#include "core/memtrack.h"
#include "core/meta_peek.h"
#include "core/sql_api.h"

#define CHOICE_COLS		9
#define ENC_SLOTS		(SIFT_CENC_STR_PROMO + 1)
#define TIER_SLOTS		(SIFT_TIER_COLD_L2 + 1)

typedef struct SectionRow
{
	const char *choice;
	const char *why;
	uint16		tags[4];
} SectionRow;

static const SectionRow sections[] = {
	{"zone_maps", "The least and the greatest value of each column in each granule: a predicate skips the granules its range excludes.", {SEC_ZONES}},
	{"skip_index", "A Bloom filter or trigram index per granule for a high-cardinality column: an equality, IN or LIKE skips the granules that cannot match.", {SEC_SKIP_INDEX}},
	{"value_sets", "The distinct values of a low-cardinality column in each granule: an equality or IN list skips the granules without them.", {SEC_SETS}},
	{"preaggregates", "Counts, sums and extremes of the granules, by the dimensions of the index: an aggregate reads them instead of the chunks.", 
	 {SEC_PREAGG, SEC_PREAGG_SUPER, SEC_PREAGG_ROLLUP, SEC_PREAGG_EXCEPT}},
	{"sketches", "Distinct-count (HyperLogLog) and quantile sketches: approximate aggregates read them instead of the chunks.", {SEC_HLL, SEC_KLL}},
	{"runs_and_keys", "Run-length summaries and key tables of the granules: a grouped scan reads them before the chunks.", {SEC_RUNS, SEC_KTAB}},
	{"point_directory", "A directory from a key to its granule and row: a point lookup reads one granule.", {SEC_POINT}},
	{"address_index", "The addresses of the rows (a hash directory for key-ordered parts): a row of the heap is found without scanning the part.", {SEC_TID_INDEX, SEC_TID_HASH}},
	{"row_pack", "64-row LZ4 micro-blocks of whole rows: a point fetch of a sift_table row decodes one block.", {SEC_ROW_PACK}},
	{"time_strides", "The time range of each 256-row stride of a granule: a time filter skips strides inside a granule.", {SEC_GRANULE_TSTRIDE}},
	{"key_marks", "The first and the last key of each granule: a key range skips granules and an ordered scan merges parts by them.", {SEC_KEY_MARKS}},
	{"column_stats", "The bytes, the distinct estimate and the dominant encoding of each column: they order the filters of a scan.", {SEC_COL_STATS}},
	{"dictionaries", "The strings of a column shared by the granules of a part: the chunks hold their codes.", {SEC_DICTS}},
};

typedef struct ColTally
{
	uint64		chunks;
	uint64		bytes;
	uint64		enc[ENC_SLOTS];
} ColTally;

typedef struct Tally
{
	int			parts;
	int			remote;
	int			slab_partitions;
	int			with_dv;
	int			tier[TIER_SLOTS];
	uint64		rows;
	uint64		dead;
	uint64		bytes;
	uint64		granules;
	uint64		remote_bytes;
	int			sec_parts[lengthof(sections)];
	uint64		sec_bytes[lengthof(sections)];
	int			ncols;
	ColTally   *cols;
} Tally;

static const char *
encoding_label(uint16 enc)
{
	static const char *const names[ENC_SLOTS] = {
		[SIFT_CENC_TID_DELTA] = "tid_delta", [SIFT_CENC_INT_FOR] = "frame_of_reference", [SIFT_CENC_INT_DELTA] = "delta", [SIFT_CENC_INT_CONST] = "constant",
		[SIFT_CENC_DICT_CODE] = "dictionary", [SIFT_CENC_STR_INLINE] = "string_inline", [SIFT_CENC_FLOAT_RAW] = "float_raw", [SIFT_CENC_LIST_LEN] = "list_lengths",
		[SIFT_CENC_ALL_NULL] = "all_null", [SIFT_CENC_BOOL_BITS] = "bool_bits", [SIFT_CENC_NUM64] = "numeric64", [SIFT_CENC_NUM128] = "numeric128",
		[SIFT_CENC_UUID16] = "uuid16", [SIFT_CENC_INTERVAL16] = "interval16", [SIFT_CENC_RLE] = "run_length", [SIFT_CENC_ALP] = "alp", [SIFT_CENC_SCALE] = "scale",
		[SIFT_CENC_STR_VIEW] = "string_view", [SIFT_CENC_TID_BLOCK] = "tid_block", [SIFT_CENC_FLOAT_BSS] = "float_byte_split", [SIFT_CENC_STR_FSST] = "fsst",
		[SIFT_CENC_STR_BLOCK] = "string_block", [SIFT_CENC_FIXED16] = "fixed16", [SIFT_CENC_KEY_PRESENT] = "key_present", [SIFT_CENC_ALP_RD] = "alp_rd",
		[SIFT_CENC_STR_PROMO] = "string_promoted"
	};
	uint16		e = enc & SIFT_ENC_MASK;

	return e < ENC_SLOTS && names[e] != NULL ? names[e] : "unknown";
}

static SiftCompressTier
tier_of_level(uint8 level)
{
	return level == 0 ? SIFT_TIER_HOT_L0 : level == 1 ? SIFT_TIER_WARM_L1 : SIFT_TIER_COLD_L2;
}

static void
tally_sections(const SiftPart *part, Tally *t)
{
	for (int s = 0; s < lengthof(sections); s++)
	{
		bool		found = false;

		for (int k = 0; k < lengthof(sections[s].tags) && sections[s].tags[k] != 0; k++)
		{
			const SiftSecDirEntry *e = sift_part_section(part, sections[s].tags[k]);

			if (e == NULL)
				continue;
			found = true;
			t->sec_bytes[s] += e->len;
		}
		t->sec_parts[s] += found;
	}
}

static void
tally_chunks(Relation rel, const SiftPart *part, Tally *t, MemoryContext granule_cx)
{
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		MemoryContext old = MemoryContextSwitchTo(granule_cx);
		SiftGranuleDesc gd;
		char	   *rec = sift_part_read_granule(rel, part, g, &gd);
		SiftGranuleView v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);

		for (int c = 0; c < Min((int) v.hdr->nchunks - 1, t->ncols); c++)
		{
			const SiftChunkRef *ref = &v.hdr->chunks[c + 1];
			uint16		enc = ref->enc & SIFT_ENC_MASK;

			t->cols[c].chunks++;
			t->cols[c].bytes += ref->len;
			t->cols[c].enc[enc < ENC_SLOTS ? enc : 0]++;
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(granule_cx);
	}
}

static void
tally_part(Relation rel, const SiftPartDesc *d, uint32 part_id, Tally *t, MemoryContext granule_cx)
{
	t->parts++;
	t->tier[tier_of_level(d->level)]++;
	t->rows += d->nrows;
	t->dead += d->ndead;
	t->bytes += d->nbytes;
	t->with_dv += d->dv_blk != InvalidBlockNumber;
	if (d->ext_flags & SIFT_PART_EXT_REMOTE)
	{
		t->remote++;
		t->remote_bytes += d->nbytes;
		return;
	}
	{
		SiftPart   *part = sift_part_open_desc(rel, d, part_id);

		t->granules += part->hdr.ngranules;
		tally_sections(part, t);
		if (t->cols != NULL)
			tally_chunks(rel, part, t, granule_cx);
	}
}

static void
tally_index(Relation rel, Tally *t, MemoryContext granule_cx)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		t->slab_partitions += mr.entries[i].l0_slab_blk != InvalidBlockNumber;
		for (int p = 0; p < np; p++)
			tally_part(rel, &pd[p], (uint32) mr.entries[i].part_id, t, granule_cx);
	}
}

typedef struct Out
{
	ReturnSetInfo *rsi;
	const char *name;			/* the quoted name of the relation, for the commands of the off column */
} Out;

static void
put_choice(Out *o, const char *choice, const char *value, const char *cost, const char *why, const char *off)
{
	Datum		v[CHOICE_COLS] = {0};
	bool		nulls[CHOICE_COLS] = {false, false, false, cost == NULL, true, false, off == NULL, true, false};

	v[0] = CStringGetTextDatum(choice);
	v[1] = CStringGetTextDatum("auto");
	v[2] = CStringGetTextDatum(value);
	if (cost != NULL)
		v[3] = CStringGetTextDatum(cost);
	v[5] = CStringGetTextDatum(why);
	if (off != NULL)
		v[6] = CStringGetTextDatum(psprintf(off, o->name, o->name));
	v[8] = CStringGetTextDatum("FORMAT.md");
	tuplestore_putvalues(o->rsi->setResult, o->rsi->setDesc, v, nulls);
}

static const char *
bytes_text(uint64 bytes)
{
	return psprintf(UINT64_FORMAT " bytes", bytes);
}

static void
put_layout(Out *o, Relation rel, const Tally *t)
{
	put_choice(o, "layout", psprintf("%d parts, " UINT64_FORMAT " rows (" UINT64_FORMAT " dead), " UINT64_FORMAT " granules", t->parts, t->rows, t->dead, t->granules),
		bytes_text((uint64) RelationGetNumberOfBlocks(rel) * BLCKSZ),
		"Rows are held in parts of granules of up to 8,192 rows, one chunk per column in each granule, listed in the manifest (sift.parts, sift.col_granules).", NULL);
}

static void
put_tiers(Out *o, const Tally *t)
{
	put_choice(o, "compression_tier",
		psprintf("hot level 0: %d parts, warm level 1: %d, cold level 2 and up: %d", t->tier[SIFT_TIER_HOT_L0], t->tier[SIFT_TIER_WARM_L1], t->tier[SIFT_TIER_COLD_L2]),
		bytes_text(t->bytes),
		"Level 0 parts keep to the lightweight codecs (constant, run length, delta, frame of reference, a small dictionary); level 1 parts try every codec; the chunks of level 2 and up that a codec wraps are wrapped in zstd.", NULL);
	put_choice(o, "l0_slab", t->slab_partitions > 0 ? psprintf("%d partitions hold rows in an L0 slab, not yet in a part", t->slab_partitions) : "none: every row is in a part", NULL,
		"Inserts land in the slab of their partition and are flushed to a level 0 part by the maintenance; a read merges the slab with the parts.", NULL);
}

static void
put_remote(Out *o, Relation rel, const Tally *t)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	int			ntier;
	int			only = 0;
	SiftTierPartRef *refs = sift_manifest_read_tier(rel, &meta, &ntier);

	for (int i = 0; i < ntier; i++)
		only += (refs[i].flags & SIFT_TIER_REMOTE_ONLY) != 0;
	if (t->remote == 0 && ntier == 0)
		return;
	put_choice(o, "object_storage", psprintf("%d parts in object storage (%d with no local copy), %d in the tier directory", t->remote, only, ntier), bytes_text(t->remote_bytes),
		"Cold parts are copied to object storage by the tier policy; their stream is read from the object, and these rows count them from their descriptors.", NULL);
}

static void
put_sections(Out *o, const Tally *t)
{
	for (int s = 0; s < lengthof(sections); s++)
		if (t->sec_parts[s] > 0)
			put_choice(o, sections[s].choice, psprintf("%d of %d parts", t->sec_parts[s], t->parts - t->remote), bytes_text(t->sec_bytes[s]), sections[s].why, NULL);
	if (t->with_dv > 0)
		put_choice(o, "delete_vectors", psprintf("%d of %d parts", t->with_dv, t->parts), NULL, "The rows a DELETE or UPDATE removed from a part, until a compaction rewrites it.", NULL);
}

static void
put_key_order(Out *o, Relation index)
{
	bool		keyed = !(sift_col_meta_read(index).state_flags & SIFT_META_FLAG_UNKEYED);

	put_choice(o, "key_order", keyed ? "parts are sorted by the key and partitioned by its ranges" : "none: parts are in insert order", NULL,
		"A key order lets a key range or an ordered scan read only the parts and granules its range covers.",
		keyed ? "ALTER INDEX %s SET (key_order = 'off'); REINDEX INDEX %s" : NULL);
}

static const char *
column_label(Relation index, const SiftColDef *d)
{
	return d->path != NULL && d->path[0] != '\0' ? d->path : NameStr(TupleDescAttr(index->rd_att, d->attno - 1)->attname);
}

static const char *
encodings_text(const ColTally *c)
{
	StringInfoData buf;
	bool		used[ENC_SLOTS] = {false};

	initStringInfo(&buf);
	for (int shown = 0; shown < 4; shown++)
	{
		int			best = -1;

		for (int e = 0; e < ENC_SLOTS; e++)
			if (!used[e] && c->enc[e] > 0 && (best < 0 || c->enc[e] > c->enc[best]))
				best = e;
		if (best < 0)
			break;
		used[best] = true;
		appendStringInfo(&buf, "%s%s x" UINT64_FORMAT, shown > 0 ? ", " : "", encoding_label((uint16) best), c->enc[best]);
	}
	return buf.len > 0 ? buf.data : "no chunks";
}

static void
put_columns(Out *o, Relation index, const Tally *t)
{
	const SiftColSchema *schema = sift_col_rel(index)->schema;

	for (int c = 0; c < t->ncols; c++)
		if (t->cols[c].chunks > 0)
			put_choice(o, psprintf("encoding(%s)", column_label(index, &schema->cols[c])), encodings_text(&t->cols[c]), bytes_text(t->cols[c].bytes),
				"Chosen granule by granule: the codec that stores the chunk in the fewest bytes, from constant, delta, frame of reference and bit-packing, a dictionary, FSST, ALP for doubles and zstd on the cold tier.", NULL);
}

bool
sift_f2_relation(Oid relid)
{
	Relation	rel = try_relation_open(relid, AccessShareLock);
	SiftApiAm	am;
	bool		applies = false;

	if (rel == NULL)
		return false;
	if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind) && sift_api_am_of(rel, &am) && am != SIFT_API_BM25)
		applies = sift_meta_peek_format(rel).status == SIFT_FMT_V2;
	relation_close(rel, AccessShareLock);
	return applies;
}

Datum
sift_f2_index_choices(PG_FUNCTION_ARGS)
{
	SiftApiRel	r = sift_api_open(PG_GETARG_OID(0), AccessShareLock);
	Out			o = {(ReturnSetInfo *) fcinfo->resultinfo, quote_identifier(RelationGetRelationName(r.rel))};
	Tally		t = {0};
	MemoryContext granule_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift choices granule");
	bool		is_index = r.am == SIFT_API_COL;

	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(o.rsi->setDesc, (const Oid[CHOICE_COLS]){TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID}, CHOICE_COLS);
	if (is_index)
	{
		t.ncols = sift_col_rel(r.rel)->schema->ncols;
		t.cols = palloc0(Max(t.ncols, 1) * sizeof(ColTally));
	}
	tally_index(r.rel, &t, granule_cx);
	MemoryContextDelete(granule_cx);
	put_layout(&o, r.rel, &t);
	put_tiers(&o, &t);
	put_remote(&o, r.rel, &t);
	put_sections(&o, &t);
	if (is_index)
	{
		put_key_order(&o, r.rel);
		put_columns(&o, r.rel, &t);
	}
	sift_api_close(&r, AccessShareLock);
	PG_RETURN_NULL();
}
