/*
 * col/f2/f2_decoders_test.c - test-only SQL functions sift_test_dec_* (SIFT_TESTING builds): the decoders of sections, chunks and wide values on bytes that came from disk. Each builds the
 * damaged bytes of one case, runs the reader in a subtransaction and answers "ok" or the SQLSTATE and message it raised, so that a reader that checks nothing is told from one that refuses.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/xact.h"
#include "utils/builtins.h"
#include "utils/resowner.h"

#include "col/f2/f2test.h"
#include "col/f2/f2_tid_hash.h"
#include "col/compress.h"
#include "col/granule/granule.h"
#include "col/f2/f2_point_dir.h"
#include "col/f2/l0_slab.h"
#include "col/granule/granule_int.h"
#include "core/f2/bulk_io.h"
#include "table/f2/f2_col_delta_dir.h"
#include "table/f2/f2_blob.h"
#include "table/f2/f2_row_pack.h"

PG_FUNCTION_INFO_V1(sift_test_dec_row_pack);
PG_FUNCTION_INFO_V1(sift_test_dec_row_pack_check);
PG_FUNCTION_INFO_V1(sift_test_dec_blob);
PG_FUNCTION_INFO_V1(sift_test_dec_chunk);
PG_FUNCTION_INFO_V1(sift_test_dec_schema_nsort);
PG_FUNCTION_INFO_V1(sift_test_dec_tid_hash);
PG_FUNCTION_INFO_V1(sift_test_dec_preagg);
PG_FUNCTION_INFO_V1(sift_test_dec_meta_totals);
PG_FUNCTION_INFO_V1(sift_test_dec_own);
PG_FUNCTION_INFO_V1(sift_test_dec_damage);
PG_FUNCTION_INFO_V1(sift_test_dec_section);

typedef struct Case
{
	Relation	rel;
	int			mode;
	bool		check;
} Case;

static text *
outcome(void (*fn) (Case *), Case *c)
{
	MemoryContext old = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	char	   *res = "ok";

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(old);
	PG_TRY();
	{
		fn(c);
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(old);
		e = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old);
		CurrentResourceOwner = owner;
		res = psprintf("%s %s", unpack_sql_state(e->sqlerrcode), e->message);
		FreeErrorData(e);
	}
	PG_END_TRY();
	return cstring_to_text(res);
}

static Datum
run_case(FunctionCallInfo fcinfo, void (*fn) (Case *))
{
	Case		c = {.rel = f2t_open_index(fcinfo, RowExclusiveLock),.mode = PG_GETARG_INT32(1)};
	text	   *res = outcome(fn, &c);

	index_close(c.rel, NoLock);
	PG_RETURN_TEXT_P(res);
}

static SiftPart *
write_part_desc(Relation rel, uint16 ncols, uint32 ngranules, uint64 nrows, const SiftSecInput *secs, int nsecs, SiftPartDesc *d)
{
	uint64		gen = sift_manifest_new_gen(rel, 1);
	SiftPartIn	in = {.part_id = 1,.part_gen = gen,.level = 1,.ncols = ncols,.nrows = nrows,.ngranules = ngranules,.min_xid = InvalidFullTransactionId,
	.max_xid = InvalidFullTransactionId,.sections = secs,.nsections = nsecs,.inline_extent_cap = 0};

	*d = sift_part_write(rel, &in);
	return sift_part_open(rel, d->header_blk, gen, 1);
}

static SiftPart *
write_part(Relation rel, uint16 ncols, uint32 ngranules, uint64 nrows, const SiftSecInput *secs, int nsecs)
{
	SiftPartDesc d;

	return write_part_desc(rel, ncols, ngranules, nrows, secs, nsecs, &d);
}

static SiftSecInput
sec(uint16 tag, const void *data, uint64 len)
{
	return (SiftSecInput) {.tag = tag,.data = data,.len = len};
}

/* ---- SEC_ROW_PACK ---- */

static void
row_pack_case(Case *c)
{
	const uint8 kinds[] = {SIFT_CELL_INT, 7, SIFT_CELL_INT, SIFT_CELL_INT};
	const uint8 widths[] = {8, 8, 64, 3};
	const int	m = c->mode;
	const uint32 width = m == 2 ? 64 : 8;
	const uint32 rowlen = 8 + 1 + 2 + width;
	StringInfoData s;
	SiftRowPackHdr hdr = {SIFT_ROW_PACK_MAGIC, SIFT_ROW_PACK_VER, 0, 1, SIFT_ROW_PACK_ROWS, 1, 1, sizeof(SiftRowPackHdr) + sizeof(SiftRowPackCol), 0};
	SiftRowPackCol col = {.kind = m < 4 ? kinds[m] : SIFT_CELL_INT,.width = m < 4 ? widths[m] : 8};
	SiftRowPackBlockEntry e = {.nrows = 1,.codec_flags = SIFT_RPACK_CODEC_RAW};
	uint32		off[2] = {8, 8 + rowlen};
	uint16		fixed_bytes = (uint16) width;
	uint64		tid = 1;
	SiftSecInput secs[2];
	SiftPart   *part;
	SiftRowPackBlk *b;
	SiftF2Cell	cell;

	col.width = m == 2 ? 64 : (m == 3 ? 3 : col.width);
	e.raw_len = e.comp_len = sizeof(off) + rowlen;
	if (m == 4)
	{
		e = (SiftRowPackBlockEntry) {.comp_len = 16,.raw_len = 0x70000000,.nrows = 1,.codec_flags = SIFT_RPACK_CODEC_LZ4};
	}
	hdr.data_off = hdr.dir_off + sizeof(e);
	initStringInfo(&s);
	appendBinaryStringInfo(&s, (char *) &hdr, sizeof(hdr));
	appendBinaryStringInfo(&s, (char *) &col, sizeof(col));
	appendBinaryStringInfo(&s, (char *) &e, sizeof(e));
	if (m == 4)
		appendStringInfoSpaces(&s, 16);
	else
	{
		appendBinaryStringInfo(&s, (char *) off, sizeof(off));
		appendBinaryStringInfo(&s, (char *) &tid, sizeof(tid));
		appendStringInfoSpaces(&s, 1);
		appendBinaryStringInfo(&s, (char *) &fixed_bytes, sizeof(fixed_bytes));
		appendStringInfoSpaces(&s, width);
	}
	secs[0] = sec(SEC_SCHEMA, "SSSSSSSSSSSSSSSS", 16);
	secs[1] = sec(SEC_ROW_PACK, s.data, s.len);
	if (c->check)
	{
		const uint16 schema[8] = {4, 1};
		SiftPartDesc d;
		SiftManifestEdit e = {.part_id = 1};
		uint8		lo[SIFT_BOUND_BYTES] = {0};

		secs[0] = sec(SEC_SCHEMA, schema, sizeof(schema));
		(void) write_part_desc(c->rel, 1, 0, 1, secs, 2, &d);
		e.key_lo = lo;
		e.add_parts = &d;
		e.nadd = 1;
		sift_manifest_commit(c->rel, &e);
		(void) sift_f2_check_keys(c->rel);
		return;
	}
	part = write_part(c->rel, 1, 1, 1, secs, 2);
	b = sift_row_pack_read_block(c->rel, part, 0);
	(void) sift_row_pack_cell(sift_row_pack_row(b, 0), 0, &cell);
}

Datum
sift_test_dec_row_pack(PG_FUNCTION_ARGS)
{
	return run_case(fcinfo, row_pack_case);
}

/* The same packs, found by the check of the index (the part is in its manifest) and not by a read of a block */
Datum
sift_test_dec_row_pack_check(PG_FUNCTION_ARGS)
{
	Case		c = {.rel = f2t_open_index(fcinfo, RowExclusiveLock),.mode = PG_GETARG_INT32(1),.check = true};
	text	   *res = outcome(row_pack_case, &c);

	index_close(c.rel, NoLock);
	PG_RETURN_TEXT_P(res);
}

/* ---- wide values ---- */

static void
blob_case(Case *c)
{
	const uint32 stored_len = 16;
	const uint32 raw_len = c->mode == 1 ? 65536 : (c->mode == 2 ? (uint32) MaxAllocSize + 1 : (c->mode == 3 ? 0 : 16));
	SiftExtent	ext[4];
	SiftPartBulkWriter w;
	char		page_bytes[16] = "0123456789abcdef";
	SiftTblBlobRef *ref;
	char	   *unaligned;
	int			next = sift_fsm_alloc_stream(c->rel, 1, ext, 4);

	ref = palloc0(SIFT_TBL_BLOB_REF_SIZE(next));
	ref->magic = SIFT_TBL_BLOB_MAGIC;
	ref->raw_len = raw_len;
	ref->stored_len = c->mode == 3 ? 0 : stored_len;
	ref->flags = c->mode == 2 ? SIFT_BLOB_FLAG_LZ4 : 0;
	ref->nextents = (uint16) next;
	ref->gen = 77;
	memcpy(ref->ext, ext, sizeof(SiftExtent) * next);
	ref->crc32c = sift_crc32c(page_bytes, ref->stored_len);
	sift_bulk_extend(c->rel, ref->ext, next);
	sift_bulk_begin(&w, c->rel);
	{
		Page		pg = sift_bulk_page(&w, ref->ext[0].start);

		sift_page_init_part(pg, SIFT_PT_DATA, ref->gen, 0);
		memcpy(SiftPageGetData(pg), page_bytes, stored_len);
		SiftPageSetUsed(pg, stored_len);
	}
	sift_bulk_end(&w);
	unaligned = palloc(SIFT_TBL_BLOB_REF_SIZE(next) + 1);
	memcpy(unaligned + 1, ref, SIFT_TBL_BLOB_REF_SIZE(next));
	(void) sift_tbl_blob_read(c->rel, unaligned + 1, SIFT_TBL_BLOB_REF_SIZE(next), InvalidOid, -1, CurrentMemoryContext);
}

Datum
sift_test_dec_blob(PG_FUNCTION_ARGS)
{
	return run_case(fcinfo, blob_case);
}

/* ---- the chunks of a granule ---- */

typedef struct Rec
{
	StringInfoData s;
	SiftChunkRef ref;
} Rec;

static SiftGranuleView
view_of(Rec *r, uint16 nrows)
{
	SiftGranuleHeader h = {.magic = 0,.nrows = nrows,.nchunks = 2};
	SiftChunkRef none = {0};
	SiftGranuleView v = {0};
	const uint32 body_off = sizeof(h) + 2 * sizeof(SiftChunkRef);
	StringInfoData out;

	r->ref.off = body_off;
	initStringInfo(&out);
	appendBinaryStringInfo(&out, (char *) &h, sizeof(h));
	appendBinaryStringInfo(&out, (char *) &none, sizeof(none));
	appendBinaryStringInfo(&out, (char *) &r->ref, sizeof(r->ref));
	appendBinaryStringInfo(&out, r->s.data, r->s.len);
	appendStringInfoSpaces(&out, 64);
	v.rec = out.data;
	v.len = out.len - 64;
	v.hdr = (const SiftGranuleHeader *) out.data;
	return v;
}

static void
put32s(StringInfo s, const uint32 *v, int n)
{
	appendBinaryStringInfo(s, (const char *) v, n * sizeof(uint32));
}

static void
chunk_case(Case *c)
{
	Rec			r = {0};
	SiftChunk	ck;
	SiftGranuleView v;
	const char *p[4];
	uint32		len[4];

	initStringInfo(&r.s);
	switch (c->mode)
	{
		case 0:				/* a wrapped chunk of 100 MiB, which a row of that width makes */
			{
				const Size	raw = (Size) 100 << 20;
				char	   *zeros = palloc0(raw);
				Size		cap = sift_colz_bound(SIFT_CC_ALG_LZ4, raw);
				char	   *z = palloc(cap);
				Size		zl = sift_colz_compress(SIFT_CC_ALG_LZ4, 0, zeros, raw, z, cap);
				uint32		hdr[2] = {SIFT_CC_ALG_LZ4, (uint32) raw};

				put32s(&r.s, hdr, 2);
				appendBinaryStringInfo(&r.s, z, zl);
				r.ref = (SiftChunkRef) {.len = r.s.len,.enc = SIFT_CENC_INT_CONST,.flags = SIFT_CHUNK_COMPRESSED | SIFT_CHUNK_ALL_VALID,.nnonnull = 2};
				v = view_of(&r, 2);
				sift_chunk_open(&v, 1, &ck);
				sift_chunk_close(&ck);
				break;
			}
		case 1:				/* inline strings whose offsets leave the chunk */
			{
				const uint32 offs[3] = {0, 4, 0xFFFFFF00u};

				put32s(&r.s, offs, 3);
				appendStringInfoSpaces(&r.s, 4);
				r.ref = (SiftChunkRef) {.len = r.s.len,.enc = SIFT_CENC_STR_INLINE,.flags = SIFT_CHUNK_ALL_VALID,.nnonnull = 2};
				v = view_of(&r, 2);
				sift_chunk_open(&v, 1, &ck);
				sift_chunk_strs_own(&ck, 0, 2, p, len, NULL);
				sift_chunk_close(&ck);
				break;
			}
		case 2:				/* a chunk shorter than the bitmap of the validity of its rows */
			appendStringInfoSpaces(&r.s, 16);
			r.ref = (SiftChunkRef) {.len = r.s.len,.enc = SIFT_CENC_INT_CONST,.flags = 0,.nnonnull = 16};
			v = view_of(&r, 60000);
			sift_chunk_open(&v, 1, &ck);
			sift_chunk_close(&ck);
			break;
		default:
			elog(ERROR, "no such case");
	}
}

Datum
sift_test_dec_chunk(PG_FUNCTION_ARGS)
{
	Case		c = {.mode = PG_GETARG_INT32(0)};

	PG_RETURN_TEXT_P(outcome(chunk_case, &c));
}

/* ---- the sections a check reads ---- */

static void
schema_nsort_case(Case *c)
{
	char		schema[512] = {0};
	uint16		hdr[2] = {4, (uint16) c->mode};
	SiftSecInput secs[1];
	SiftPartDesc d;
	SiftManifestEdit e = {.part_id = 1};
	uint8		lo[SIFT_BOUND_BYTES] = {0};
	uint64		gen = sift_manifest_new_gen(c->rel, 1);
	SiftPartIn	in = {.part_id = 1,.part_gen = gen,.level = 1,.min_xid = InvalidFullTransactionId,.max_xid = InvalidFullTransactionId,.nsections = 1,.sections = secs};

	memcpy(schema, hdr, sizeof(hdr));
	secs[0] = sec(SEC_SCHEMA, schema, sizeof(schema));
	d = sift_part_write(c->rel, &in);
	e.key_lo = lo;
	e.add_parts = &d;
	e.nadd = 1;
	sift_manifest_commit(c->rel, &e);
	(void) sift_f2_check_keys(c->rel);
}

Datum
sift_test_dec_schema_nsort(PG_FUNCTION_ARGS)
{
	return run_case(fcinfo, schema_nsort_case);
}

static void
tid_hash_case(Case *c)
{
	const uint64 tid = 12345;
	StringInfoData s;
	SiftTidHashHdr hdr = {SIFT_TID_HASH_MAGIC, SIFT_TID_HASH_VER_V2, 0, 1, 1};
	uint32		start[3] = {0, 0, 0};
	SiftTidHashSlotV2 slot = {.row_idx = c->mode == 1 ? 7 : 0,.fp16 = sift_tid_hash_fp(tid),.granule_id = c->mode == 2 ? 65535 : 0};
	SiftSecInput secs[2];
	SiftTidHashHit *hits;
	SiftPart   *part;
	uint32		bucket = sift_tid_hash_bucket(tid, 1);

	for (uint32 b = bucket + 1; b < 3; b++)
		start[b] = 1;
	initStringInfo(&s);
	appendBinaryStringInfo(&s, (char *) &hdr, sizeof(hdr));
	appendBinaryStringInfo(&s, (char *) start, sizeof(start));
	appendBinaryStringInfo(&s, (char *) &slot, sizeof(slot));
	secs[0] = sec(SEC_SCHEMA, "SSSSSSSSSSSSSSSS", 16);
	secs[1] = sec(SEC_TID_HASH, s.data, s.len);
	part = write_part(c->rel, 1, 1, 1, secs, 2);
	(void) sift_tid_hash_find(c->rel, part, 1, tid, &hits);
}

Datum
sift_test_dec_tid_hash(PG_FUNCTION_ARGS)
{
	return run_case(fcinfo, tid_hash_case);
}

static void
preagg_case(Case *c)
{
	uint32		sec_bytes[16] = {0};
	SiftSecInput secs[2];
	SiftPart   *part;
	int			n;

	secs[0] = sec(SEC_SCHEMA, "SSSSSSSSSSSSSSSS", 16);
	secs[1] = sec(SEC_PREAGG, sec_bytes, sizeof(sec_bytes));
	part = write_part(c->rel, 1, c->mode == 1 ? UINT32_MAX : 1, 1, secs, 2);
	(void) sift_part_read_preagg(c->rel, part, 0, &n);
}

Datum
sift_test_dec_preagg(PG_FUNCTION_ARGS)
{
	return run_case(fcinfo, preagg_case);
}

/* ---- sift.check on a manifest that no longer adds up ---- */

Datum
sift_test_dec_meta_totals(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftColMetaPageData *meta = (SiftColMetaPageData *) SiftPageGetData(BufferGetPage(buf));

	meta->total_rows += PG_GETARG_INT64(1);
	sift_meta_seal(BufferGetPage(buf), SiftPageGetUsed(BufferGetPage(buf)));
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
	index_close(rel, NoLock);
	PG_RETURN_VOID();
}

/* ---- sift.check on a relation whose blocks, chains or sections are damaged ---- */

typedef struct Found
{
	SiftPartitionEntry entry;
	SiftPartDesc part;
} Found;

/* The first part of the relation that the test names, or the first one */
static Found
first_part(Relation rel, bool (*want) (const SiftPartitionEntry *, const SiftPartDesc *))
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
			if (want == NULL || want(&mr.entries[i], &pd[p]))
				return (Found) {mr.entries[i], pd[p]};
	}
	elog(ERROR, "no such part");
	pg_unreachable();
}

static bool
has_dv(const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	return d->dv_blk != InvalidBlockNumber;
}

static BlockNumber
first_slab(Relation rel)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
		if (mr.entries[i].l0_slab_blk != InvalidBlockNumber)
			return mr.entries[i].l0_slab_blk;
	elog(ERROR, "no micro-slab");
	pg_unreachable();
}

static void
damage_page(Relation rel, BlockNumber blk, void (*fn) (char *payload, void *arg), void *arg)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_EXCLUSIVE);

	fn(SiftPageGetData(BufferGetPage(buf)), arg);
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

static void
flip_byte(char *payload, void *arg)
{
	payload[*(int *) arg] ^= 0x55;
}

static void
slab_batch(char *payload, void *arg)
{
	SiftL0BatchHeader *b = (SiftL0BatchHeader *) (payload + sizeof(SiftL0SlabPageData));

	if (*(int *) arg == 0)
		b->nrows += 500;
	else
		b->bytes = SIFT_L0_COMPACT_HDR + 8;
}

/* mode 0: an extent of a live part is also on the free lists; 1: an extent that nothing names is allocated; the relation is a sift_col index or a sift_table */
Datum
sift_test_dec_own(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), RowExclusiveLock);

	if (PG_GETARG_INT32(1) == 0)
	{
		Found		f = first_part(rel, NULL);
		SiftPart   *part = sift_part_open_desc(rel, &f.part, (uint32) f.entry.part_id);
		SiftExtent	ext = {part->header_blk, 1};
		int			n;

		if (!part->is_packed)
			ext = sift_part_blocks(part, &n)[0];
		sift_fsm_release(rel, &ext, 1, InvalidFullTransactionId);
	}
	else
		(void) sift_fsm_alloc(rel, 1);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}

/* mode 0: a byte of the first page of a column delta chain; 1: of a deletion vector; 2 and 3: the slab batch of a sift_table that claims rows it does not hold, and one cut short */
Datum
sift_test_dec_damage(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), RowExclusiveLock);
	int			mode = PG_GETARG_INT32(1);
	int			off = 64;

	if (mode == 0)
	{
		SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
		SiftColDeltaAnchor a = sift_manifest_read_col_delta(rel, &mr.entries[0]);

		damage_page(rel, a.delta_head_blkno, flip_byte, &off);
	}
	else if (mode == 1)
		damage_page(rel, first_part(rel, has_dv).part.dv_blk, flip_byte, &off);
	else
	{
		int			which = mode - 2;

		damage_page(rel, first_slab(rel), slab_batch, &which);
	}
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}

static StringInfoData
point_section(int mode)
{
	StringInfoData s;
	SiftPointHdr hdr = {SIFT_POINT_MAGIC, SIFT_POINT_VERSION, 1};
	SiftPointCol col = {.leaves = {0, SIFT_POINT_NO_LEAF, SIFT_POINT_NO_LEAF, SIFT_POINT_NO_LEAF},.nleaves = 1,.log2_buckets = 1,.nentries = 1,
	.off_buckets = sizeof(hdr) + sizeof(col),.off_entries = sizeof(hdr) + sizeof(col) + 3 * sizeof(uint32)};
	uint32		buckets[3] = {0, mode == 0 ? 1 : 0, mode == 0 ? 7 : 1};
	uint32		entry = mode == 0 ? 0 : (uint32) 1 << SIFT_POINT_ROW_BITS;

	initStringInfo(&s);
	appendBinaryStringInfo(&s, (char *) &hdr, sizeof(hdr));
	appendBinaryStringInfo(&s, (char *) &col, sizeof(col));
	appendBinaryStringInfo(&s, (char *) buckets, sizeof(buckets));
	appendBinaryStringInfo(&s, (char *) &entry, sizeof(entry));
	return s;
}

/* A part of no granule with a damaged section, in the manifest: mode 0 and 1, SEC_POINT whose buckets end past its entries and whose entry names a granule the part does not have; 2 and 3, SEC_PREAGG
 * shorter than the numbers of its granules and one that counts cells it does not hold */
Datum
sift_test_dec_section(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), RowExclusiveLock);
	const int	mode = PG_GETARG_INT32(1);
	const uint16 schema[8] = {4, 1};
	const uint32 preagg[2] = {mode == 3 ? 7 : 0, 0};
	StringInfoData pt = point_section(mode);
	SiftSecInput secs[2];
	SiftPartDesc d;
	SiftManifestEdit e = {.part_id = 1};
	uint8		lo[SIFT_BOUND_BYTES] = {0};

	secs[0] = sec(SEC_SCHEMA, schema, sizeof(schema));
	secs[1] = mode < 2 ? sec(SEC_POINT, pt.data, pt.len) : sec(SEC_PREAGG, preagg, mode == 2 ? 4 : sizeof(preagg));
	(void) write_part_desc(rel, 1, 0, 1, secs, 2, &d);
	e.key_lo = lo;
	e.add_parts = &d;
	e.nadd = 1;
	sift_manifest_commit(rel, &e);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}

#endif							/* SIFT_TESTING */
