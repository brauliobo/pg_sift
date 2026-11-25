/*
 * table/f2/f2_col_delta.c - the policy, the byte format, the stage and the verdict of a modify of the per-column update pages (table/f2/f2_col_delta.h).
 *
 * Nothing here calls the server: the transactions a record's writer may be are asked through SiftColDeltaOps, the pages come through a SiftColDeltaPageReader, so that the whole
 * format is tested without a server (tests/col_delta_test.c).
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/relfilelocator.h"
#include "utils/memutils.h"

#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_col_delta_dir.h"

/* ---- the policy ---- */

SiftColDeltaReason
sift_col_delta_admit(const SiftColDeltaPolicyIn *in)
{
	if (!SIFT_COL_DELTA_ENABLED())
		return SIFT_CD_DISABLED;
	if (in->live_attrs < SIFT_COL_DELTA_MIN_LIVE_ATTRS)
		return SIFT_CD_NARROW_TABLE;
	if (!in->row_in_part)
		return SIFT_CD_NOT_IN_PART;
	if (in->after_update_triggers)
		return SIFT_CD_AFTER_TRIGGER;
	if (in->hits_index)
		return SIFT_CD_INDEXED_COLUMN;
	if (in->hits_protected)
		return SIFT_CD_PROTECTED_COLUMN;
	if (in->nchanged > SIFT_COL_DELTA_MAX_COLS_PER_REC)
		return SIFT_CD_TOO_MANY_COLUMNS;
	if (in->payload_bytes > SIFT_COL_DELTA_MAX_PAYLOAD)
		return SIFT_CD_PAYLOAD_TOO_LARGE;
	if (in->chain_pages + in->staged_pages >= SIFT_COL_DELTA_MAX_PAGES)
		return SIFT_CD_CHAIN_FULL;
	return SIFT_CD_OK;
}

/* ---- the record ---- */

static uint32
varint_bytes(uint64 x)
{
	uint8		tmp[10];

	return (uint32) (sift_put_varint(tmp, x) - tmp);
}

static uint32
value_bytes(const SiftColDeltaCol *c)
{
	if (c->cell.isnull)
		return 0;
	if (c->kind == SIFT_CELL_INT)
		return varint_bytes(sift_zigzag(c->cell.i));
	if (c->kind == SIFT_CELL_F8)
		return sizeof(double);
	return varint_bytes(c->cell.slen) + c->cell.slen;
}

uint32
sift_col_delta_payload_bytes(const SiftColDeltaCol *cols, int ncols)
{
	uint32		n = 0;

	for (int k = 0; k < ncols; k++)
		n += sizeof(SiftColDeltaColHdr) + value_bytes(&cols[k]);
	return n;
}

static uint8 *
put_value(uint8 *p, const SiftColDeltaCol *c)
{
	if (c->cell.isnull)
		return p;
	if (c->kind == SIFT_CELL_INT)
		return sift_put_varint(p, sift_zigzag(c->cell.i));
	if (c->kind == SIFT_CELL_F8)
	{
		memcpy(p, &c->cell.f, sizeof(double));
		return p + sizeof(double);
	}
	p = sift_put_varint(p, c->cell.slen);
	memcpy(p, c->cell.s, c->cell.slen);
	return p + c->cell.slen;
}

uint32
sift_col_delta_encode(uint8 *dst, uint32 cap, const SiftColDeltaTarget *t, TransactionId xmin, CommandId cid, uint8 flags, const SiftColDeltaCol *cols, int ncols)
{
	uint32		payload = sift_col_delta_payload_bytes(cols, ncols);
	uint32		rec_bytes = (uint32) TYPEALIGN(8, sizeof(SiftColDeltaEntryHdr) + payload);
	SiftColDeltaEntryHdr h = {
		.part_gen = t->part_gen,.granule_id = t->granule_id,.tid_blk = (BlockNumber) (t->tid >> 16),.tid_off = (OffsetNumber) (t->tid & 0xFFFF),
		.row_ordinal = t->row_ordinal,.rec_bytes = (uint16) rec_bytes,.ncols = (uint8) ncols,.flags = flags,.xmin = xmin,.cid = cid,
	};
	uint8	   *p;

	if (ncols > ((flags & SIFT_CDENT_FLAG_LEAF_ADDR) != 0 ? SIFT_COL_DELTA_MAX_COLS_LEAF_ADDR : SIFT_COL_DELTA_MAX_COLS_PER_REC) || rec_bytes > cap || rec_bytes > SIFT_COL_DELTA_MAX_REC_BYTES)
		return 0;
	memset(dst, 0, rec_bytes);
	memcpy(dst, &h, sizeof(h));
	p = dst + sizeof(h);
	for (int k = 0; k < ncols; k++)
	{
		uint8		tag = (flags & SIFT_CDENT_FLAG_TAG_BYTE) != 0 ? cols[k].cell.tag : 0;
		SiftColDeltaColHdr ch = {
			.attnum = cols[k].attnum,.flags = (uint8) (SIFT_CDCOL_PACK_FLAGS(cols[k].cell.isnull, cols[k].kind) | (tag != 0 ? SIFT_CDCOL_HAS_TAG : 0) | (cols[k].key_absent ? SIFT_CDCOL_KEY_ABSENT : 0)),.pad = tag,
		};

		memcpy(p, &ch, sizeof(ch));
		p = put_value(p + sizeof(ch), &cols[k]);
	}
	return rec_bytes;
}

const uint8 *
sift_col_delta_decode_col(const uint8 *p, const uint8 *end, SiftColDeltaPatch *out)
{
	SiftColDeltaColHdr ch;
	SiftCellKind kind;
	uint64		x;

	if (end - p < (ptrdiff_t) sizeof(ch))
		return NULL;
	memcpy(&ch, p, sizeof(ch));
	p += sizeof(ch);
	kind = SIFT_CDCOL_KIND(ch.flags);
	if (ch.attnum < 0 || (ch.pad != 0) != ((ch.flags & SIFT_CDCOL_HAS_TAG) != 0) || (ch.flags & ~SIFT_CDCOL_FLAGS_KNOWN) != 0 || (kind != SIFT_CELL_INT && kind != SIFT_CELL_STR && kind != SIFT_CELL_F8) ||
		((ch.flags & SIFT_CDCOL_KEY_ABSENT) != 0 && (ch.flags & SIFT_CDCOL_IS_NULL) == 0))
		return NULL;
	memset(out, 0, sizeof(*out));
	out->attnum = ch.attnum;
	out->flags = ch.flags;
	out->tag = ch.pad;
	if ((ch.flags & SIFT_CDCOL_IS_NULL) != 0)
		return p;
	if (kind == SIFT_CELL_INT)
	{
		p = sift_get_varint64_n(p, end, &x);
		out->val.i = p != NULL ? sift_unzigzag(x) : 0;
		return p;
	}
	if (kind == SIFT_CELL_F8)
	{
		if (end - p < (ptrdiff_t) sizeof(double))
			return NULL;
		memcpy(&out->val.f, p, sizeof(double));
		return p + sizeof(double);
	}
	p = sift_get_varint_n(p, end, &x);
	if (p == NULL || x > SIFT_COL_DELTA_MAX_PAYLOAD || (uint64) (end - p) < x)
		return NULL;
	out->vlen = (uint16) x;
	out->val.s = (const char *) p;
	return p + x;
}

const char *
sift_col_delta_rec_check(const uint8 *rec, uint32 avail)
{
	SiftColDeltaEntryHdr h;
	const uint8 *p,
			   *end;
	int16		prev = -1;
	bool		leaf_addr;

	if (avail < sizeof(h))
		return "a record is shorter than its header";
	memcpy(&h, rec, sizeof(h));
	if (h.rec_bytes < sizeof(h) || h.rec_bytes > SIFT_COL_DELTA_MAX_REC_BYTES || h.rec_bytes % 8 != 0)
		return "a record has a size the format does not allow";
	if (h.rec_bytes > avail)
		return "a record runs past the bytes of its page";
	if ((h.flags & ~SIFT_CDENT_KNOWN) != 0 || h.ncols > ((h.flags & SIFT_CDENT_FLAG_LEAF_ADDR) != 0 ? SIFT_COL_DELTA_MAX_COLS_LEAF_ADDR : SIFT_COL_DELTA_MAX_COLS_PER_REC) ||
		((h.flags & SIFT_CDENT_FLAG_NOOP) != 0) != (h.ncols == 0))
		return "a record has flags or a column count the format does not allow";
	if (h.row_ordinal >= SIFT_COL_DELTA_MAX_ROW_ORDINAL || h.tid_off == 0)
		return "a record names a row the format does not have";
	leaf_addr = (h.flags & SIFT_CDENT_FLAG_LEAF_ADDR) != 0;
	p = rec + sizeof(h);
	end = rec + h.rec_bytes;
	for (int k = 0; k < h.ncols; k++)
	{
		SiftColDeltaPatch col;

		p = sift_col_delta_decode_col(p, end, &col);
		if (p == NULL)
			return "a column of a record is not one of the format";
		if ((col.attnum == SIFT_CD_ATT_TID_REMAP && (!leaf_addr || SIFT_CDCOL_KIND(col.flags) != SIFT_CELL_INT || (col.flags & SIFT_CDCOL_IS_NULL) != 0)) ||
			((col.flags & SIFT_CDCOL_HAS_TAG) != 0 && (h.flags & SIFT_CDENT_FLAG_TAG_BYTE) == 0) || ((col.flags & SIFT_CDCOL_KEY_ABSENT) != 0 && !leaf_addr))
			return "a column of a record is of a kind its flags do not allow";
		if (col.attnum <= prev)
			return "the columns of a record are not in ascending order of attribute";
		prev = col.attnum;
	}
	if (end - p >= 8)
		return "a record has bytes past its columns";
	for (; p < end; p++)
		if (*p != 0)
			return "the padding of a record is not zero";
	return NULL;
}

/* ---- batches and pages ---- */

static void
bloom_of_record(SiftColDeltaBatch *b, const SiftColDeltaEntryHdr *h, const uint8 *rec)
{
	const uint8 *p = rec + sizeof(*h),
			   *end = rec + h->rec_bytes;

	if (b->part_gen_bloom == 0)
	{
		b->part_gen = h->part_gen;
		b->granule_lo = b->granule_hi = h->granule_id;
	}
	else
	{
		b->part_gen = b->part_gen == h->part_gen ? h->part_gen : 0;
		b->granule_lo = Min(b->granule_lo, h->granule_id);
		b->granule_hi = Max(b->granule_hi, h->granule_id);
	}
	b->part_gen_bloom |= sift_cd_part_bit(h->part_gen);
	b->tid_bloom |= sift_cd_tid_bit(sift_col_delta_hdr_tid(h));
	for (int k = 0; k < h->ncols; k++)
	{
		SiftColDeltaPatch col;

		p = sift_col_delta_decode_col(p, end, &col);
		SIFT_INVARIANT(p != NULL, "a column delta record was added to a batch before it was checked");
		b->attnum_bloom |= sift_cd_att_bit(col.attnum);
		if (col.attnum == SIFT_CD_ATT_TID_REMAP)
			b->tid_bloom |= sift_cd_tid_bit((uint64) col.val.i);
		else
			b->page_flags |= SIFT_CDPG_LEAVES;
	}
	if ((h->flags & SIFT_CDENT_FLAG_DIRTY_SCOPE) != 0)
		b->page_flags |= SIFT_CDPG_DIRTY_SCOPE;
	if ((h->flags & SIFT_CDENT_FLAG_HAS_TIME0) != 0)
		b->page_flags |= SIFT_CDPG_DIRTY_TIME0;
	if ((h->flags & SIFT_CDENT_FLAG_HAS_TIME1) != 0)
		b->page_flags |= SIFT_CDPG_DIRTY_TIME1;
}

static void
widen_xids(FullTransactionId *lo, FullTransactionId *hi, FullTransactionId fx)
{
	if (!FullTransactionIdIsValid(*lo) || FullTransactionIdPrecedes(fx, *lo))
		*lo = fx;
	*hi = sift_fxid_max(*hi, fx);
}

void
sift_col_delta_batch_add(SiftColDeltaBatch *b, FullTransactionId next, const uint8 *rec)
{
	SiftColDeltaEntryHdr h;

	memcpy(&h, rec, sizeof(h));
	if (b->nbytes + h.rec_bytes > b->cap)
	{
		b->cap = Max(b->cap * 2, (uint32) SIFT_COL_DELTA_MAX_REC_BYTES);
		b->bytes = b->bytes != NULL ? repalloc(b->bytes, b->cap) : palloc(b->cap);
	}
	memcpy(b->bytes + b->nbytes, rec, h.rec_bytes);
	b->nbytes += h.rec_bytes;
	b->nentries++;
	bloom_of_record(b, &h, rec);
	widen_xids(&b->min_xid, &b->max_xid, sift_xid_fxid_at(next, h.xmin));
}

void
sift_col_delta_batch_split(SiftColDeltaBatch *b, uint32 room, FullTransactionId next, SiftColDeltaBatch *head)
{
	SiftColDeltaBatch rest = {0};

	memset(head, 0, sizeof(*head));
	for (uint32 off = 0; off < b->nbytes;)
	{
		SiftColDeltaEntryHdr h;

		memcpy(&h, b->bytes + off, sizeof(h));
		sift_col_delta_batch_add(rest.nentries == 0 && head->nbytes + h.rec_bytes <= room ? head : &rest, next, b->bytes + off);
		off += h.rec_bytes;
	}
	sift_col_delta_batch_free(b);
	*b = rest;
}

/* The entries of b from byte offset *off that fit room, as a batch of their own: the first always goes in, so that a page is never empty */
static void
batch_take(const SiftColDeltaBatch *b, uint32 *off, uint32 room, FullTransactionId next, SiftColDeltaBatch *part)
{
	memset(part, 0, sizeof(*part));
	while (*off < b->nbytes)
	{
		SiftColDeltaEntryHdr h;

		memcpy(&h, b->bytes + *off, sizeof(h));
		if (part->nentries > 0 && part->nbytes + h.rec_bytes > room)
			break;
		sift_col_delta_batch_add(part, next, b->bytes + *off);
		*off += h.rec_bytes;
	}
}

void
sift_col_delta_batch_free(SiftColDeltaBatch *b)
{
	if (b->bytes != NULL)
		pfree(b->bytes);
	memset(b, 0, sizeof(*b));
}

void
sift_col_delta_page_init(Page page, uint64 gen, uint32 part_id, BlockNumber prev_blk, uint16 chain_pages)
{
	SiftColDeltaPageData *h;

	sift_page_init_part(page, SIFT_PT_COL_DELTA, gen, part_id);
	h = (SiftColDeltaPageData *) SiftPageGetData(page);
	memset(h, 0, sizeof(*h));
	h->magic = SIFT_COL_DELTA_MAGIC;
	h->version = SIFT_COL_DELTA_VERSION;
	h->prev_blk = prev_blk;
	h->chain_pages = chain_pages;
	h->min_xid = h->max_xid = InvalidFullTransactionId;
	SiftPageSetUsed(page, sizeof(*h));
	sift_page_seal(page);
}

bool
sift_col_delta_page_append(Page page, const SiftColDeltaBatch *b)
{
	SiftColDeltaPageData *h = (SiftColDeltaPageData *) SiftPageGetData(page);
	uint32		used = SiftPageGetUsed(page);

	if ((h->flags & SIFT_CDPG_SEALED) != 0 || used + b->nbytes > SIFT_PAGE_PAYLOAD)
		return false;
	memcpy(SiftPageGetData(page) + used, b->bytes, b->nbytes);
	h->nentries += b->nentries;
	h->flags |= b->page_flags;
	h->part_gen_bloom |= b->part_gen_bloom;
	h->attnum_bloom |= b->attnum_bloom;
	h->tid_bloom |= b->tid_bloom;
	if (b->nentries > 0)
	{
		widen_xids(&h->min_xid, &h->max_xid, b->min_xid);
		widen_xids(&h->min_xid, &h->max_xid, b->max_xid);
	}
	SiftPageSetUsed(page, used + b->nbytes);
	sift_page_seal(page);
	return true;
}

static const char *
check_records(const char *page, const SiftColDeltaPageData *h)
{
	SiftColDeltaBatch seen = {0};
	const uint8 *p = sift_col_delta_page_first(page),
			   *end = sift_col_delta_page_end(page);
	int			n = 0;

	for (; p < end; n++)
	{
		const char *why = sift_col_delta_rec_check(p, (uint32) (end - p));
		SiftColDeltaEntryHdr rh;

		if (why != NULL)
			return why;
		memcpy(&rh, p, sizeof(rh));
		bloom_of_record(&seen, &rh, p);
		p += rh.rec_bytes;
	}
	if (n != h->nentries)
		return "the records of a page are not as many as its header says";
	if ((seen.part_gen_bloom & ~h->part_gen_bloom) != 0 || (seen.attnum_bloom & ~h->attnum_bloom) != 0 || (seen.tid_bloom & ~h->tid_bloom) != 0)
		return "a bloom filter of a page misses a record it holds";
	if ((seen.page_flags & ~h->flags) != 0)
		return "a record patches a time column that its page does not say";
	if (n > 0 && (!FullTransactionIdIsValid(h->min_xid) || !FullTransactionIdIsValid(h->max_xid) || FullTransactionIdFollows(h->min_xid, h->max_xid)))
		return "the writers of a page are not a range";
	return NULL;
}

void
sift_col_delta_page_summary(const char *page, SiftColDeltaBatch *seen)
{
	const uint8 *p = sift_col_delta_page_first(page),
			   *end = sift_col_delta_page_end(page);

	memset(seen, 0, sizeof(*seen));
	for (SiftColDeltaEntryHdr rh; p < end; p += rh.rec_bytes)
	{
		memcpy(&rh, p, sizeof(rh));
		bloom_of_record(seen, &rh, p);
		seen->nentries++;
	}
}

const char *
sift_col_delta_page_check(const char *page)
{
	const SiftColDeltaPageData *h;

	if (!sift_page_layout_ok(page))
		return "a page of the chain is not laid out as a page of the index";
	if (SiftPageGetOpaque((Page) page)->type != SIFT_PT_COL_DELTA || SiftPageGetOpaque((Page) page)->flags != 0)
		return "a page of the chain is not a column delta page";
	if (SiftPageGetUsed(page) < sizeof(*h) || !sift_page_sum_ok((Page) page))
		return "a page of the chain does not match its checksum";
	h = sift_col_delta_page_hdr(page);
	if (h->magic != SIFT_COL_DELTA_MAGIC || h->version != SIFT_COL_DELTA_VERSION || h->pad != 0 || (h->flags & ~SIFT_CDPG_KNOWN) != 0 || h->chain_pages < 1)
		return "a page of the chain has a header this format does not have";
	return check_records(page, h);
}

/* ---- growing the chain ---- */

static uint16
anchor_flags_of(uint16 page_flags)
{
	return (uint16) (((page_flags & SIFT_CDPG_DIRTY_TIME0) != 0 ? SIFT_CDLA_DIRTY_TIME0 : 0) | ((page_flags & SIFT_CDPG_DIRTY_TIME1) != 0 ? SIFT_CDLA_DIRTY_TIME1 : 0) |
					((page_flags & SIFT_CDPG_DIRTY_SCOPE) != 0 ? SIFT_CDLA_DIRTY_SCOPE : 0));
}

static FullTransactionId
xid_min(FullTransactionId a, FullTransactionId b)
{
	return !FullTransactionIdIsValid(a) || FullTransactionIdPrecedes(b, a) ? b : a;
}

uint32
sift_col_delta_batch_pages(const SiftColDeltaBatch *b)
{
	uint32		pages = 0,
				room = 0;

	for (uint32 off = 0; off < b->nbytes;)
	{
		SiftColDeltaEntryHdr h;

		memcpy(&h, b->bytes + off, sizeof(h));
		if (pages == 0 || room < h.rec_bytes)
		{
			pages++;
			room = (uint32) SIFT_COL_DELTA_PAGE_ROOM;
		}
		room -= h.rec_bytes;
		off += h.rec_bytes;
	}
	return pages;
}

/*
 * A bound on time the chain did not patch before is published before the record that patches it is written, so that a crash in between leaves a bound that is only too wide. The records go
 * to the newest page while they fit and the rest to pages of their own; the new pages are published together with the one anchor that names the newest, and until then no reader can reach them.
 */
void
sift_col_delta_chain_append(const SiftColDeltaWriter *w, SiftColDeltaAnchor *a, SiftColDeltaBatch *b, FullTransactionId next)
{
	bool		fresh = !sift_col_delta_anchor_has_chain(a),
				leaves = (b->page_flags & SIFT_CDPG_LEAVES) != 0,
				indexed = (a->flags & SIFT_CDLA_INDEXED_DIR) != 0;
	uint16		flags = a->flags | anchor_flags_of(b->page_flags);
	FullTransactionId bmin = b->min_xid,
				bmax = b->max_xid;
	BlockNumber data_head = fresh || !indexed ? a->delta_head_blkno : w->dir_data_head(w->ctx, a->delta_head_blkno);
	uint32		npages;
	uint64		gen = 0;

	if (fresh || leaves)
		flags = leaves ? (uint16) (flags & ~SIFT_CDLA_TID_ONLY) : (uint16) (flags | SIFT_CDLA_TID_ONLY);
	if (flags != a->flags)
	{
		a->flags = flags;
		if (!fresh)
			w->commit_anchor(w->ctx, a);
	}
	if (!fresh && w->append_head(w->ctx, data_head, indexed ? a->delta_head_blkno : InvalidBlockNumber, b, next))
		a->max_xid = sift_fxid_max(a->max_xid, bmax);
	/* STOR[STOR-TBL-06]: batched col_delta page gen + single commit_anchor (1 vs N manifest/Block-0 WAL recs) */
	npages = sift_col_delta_batch_pages(b);
	if (npages > 0)
		gen = w->reserve_gens(w->ctx, npages);
	for (uint32 off = 0; off < b->nbytes;)
	{
		SiftColDeltaBatch part;

		batch_take(b, &off, (uint32) SIFT_COL_DELTA_PAGE_ROOM, next, &part);
		if (!indexed && a->npages >= SIFT_CDDIR_FLAT_PAGES)
		{
			a->delta_head_blkno = w->dir_promote(w->ctx, data_head, a->npages);
			a->flags |= SIFT_CDLA_INDEXED_DIR;
			indexed = true;
		}
		data_head = w->new_page(w->ctx, &part, data_head, (uint16) (a->npages + 1), gen++);
		a->delta_head_blkno = indexed ? w->dir_add(w->ctx, a->delta_head_blkno, data_head, &part) : data_head;
		a->npages++;
		a->min_xid = xid_min(a->min_xid, bmin);
		a->max_xid = sift_fxid_max(a->max_xid, bmax);
		sift_col_delta_batch_free(&part);
	}
	if (npages > 0)
		w->commit_anchor(w->ctx, a);
	sift_col_delta_batch_free(b);
}

/* ---- the chain ---- */

/* The page at blk is the one the chain's depth and link say it is */
const char *
sift_col_delta_check_link(const char *page, int depth, BlockNumber prev)
{
	if (sift_col_delta_page_hdr(page)->chain_pages != depth)
		return "a page of the chain is not as deep as the chain says";
	if (sift_col_delta_page_hdr(page)->prev_blk != prev)
		return "the chain is not as long as its anchor says";
	return NULL;
}

/* The flat chain: from the newest page along the links */
static const char *
read_flat(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, SiftColDeltaChain *out)
{
	BlockNumber blk = anchor->delta_head_blkno;
	const char *why = NULL;
	int			n = anchor->npages;

	/* MEM[MEM-TBL-15]: caller-scoped col_delta chain arena (npages * 8 KB; freed by chain_free) */
	out->arena = palloc((Size) n * BLCKSZ);
	out->blks = palloc(sizeof(BlockNumber) * n);
	for (int i = n - 1; i >= 0 && why == NULL; i--)
	{
		char	   *dst = out->arena + (Size) i * BLCKSZ;

		out->blks[i] = blk;
		why = reader->read(reader->ctx, blk, dst);
		if (why == NULL)
			why = sift_col_delta_page_check(dst);
		if (why == NULL)
			why = sift_col_delta_check_link(dst, i + 1, i == 0 ? InvalidBlockNumber : sift_col_delta_page_hdr(dst)->prev_blk);
		blk = sift_col_delta_page_hdr(dst)->prev_blk;
	}
	out->npages = out->nchain = n;
	return why;
}

const char *
sift_col_delta_chain_read_pruned(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, const SiftColDeltaPrune *prune, SiftColDeltaChain *out)
{
	const char *why;

	memset(out, 0, sizeof(*out));
	if (!sift_col_delta_anchor_sane(anchor))
		return "the anchor of a chain is not one of this format";
	if (!sift_col_delta_anchor_has_chain(anchor))
		return NULL;
	why = (anchor->flags & SIFT_CDLA_INDEXED_DIR) != 0 ? sift_cddir_chain_read(anchor, reader, prune, out) : read_flat(anchor, reader, out);
	if (why != NULL)
		sift_col_delta_chain_free(out);
	return why;
}

const char *
sift_col_delta_chain_read(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, SiftColDeltaChain *out)
{
	return sift_col_delta_chain_read_pruned(anchor, reader, NULL, out);
}

void
sift_col_delta_chain_free(SiftColDeltaChain *c)
{
	if (c->arena != NULL)
		pfree(c->arena);
	if (c->blks != NULL)
		pfree(c->blks);
	if (c->dir_blks != NULL)
		pfree(c->dir_blks);
	memset(c, 0, sizeof(*c));
}

FullTransactionId
sift_col_delta_chain_min_xid(const SiftColDeltaChain *c)
{
	FullTransactionId lo = InvalidFullTransactionId;

	for (int i = 0; i < c->npages; i++)
	{
		FullTransactionId m = sift_col_delta_page_hdr(sift_col_delta_chain_page(c, i))->min_xid;

		if (FullTransactionIdIsValid(m) && (!FullTransactionIdIsValid(lo) || FullTransactionIdPrecedes(m, lo)))
			lo = m;
	}
	return lo;
}

FullTransactionId
sift_col_delta_chain_max_xid(const SiftColDeltaChain *c)
{
	FullTransactionId hi = InvalidFullTransactionId;

	for (int i = 0; i < c->npages; i++)
		hi = sift_fxid_max(hi, sift_col_delta_page_hdr(sift_col_delta_chain_page(c, i))->max_xid);
	return hi;
}

/* ---- the verdict of a modify ---- */

/* MEM[MEM-TBL-07]: backend BSS EPQ ack ring epq_acks (~512 B; COW clean idle) */
static SiftTblEpqAck epq_acks[SIFT_EPQ_ACK_SLOTS];
static int	epq_next = 0;

static bool
ack_is(const SiftTblEpqAck *a, const RelFileLocator *loc, uint64 tid)
{
	return a->valid && RelFileLocatorEquals(a->locator, *loc) && a->tid == tid;
}

void
sift_epq_ack_record(const RelFileLocator *loc, uint64 tid, CommandId cid, const SiftColDeltaLast *seen)
{
	SiftTblEpqAck *slot = NULL;

	for (int i = 0; i < SIFT_EPQ_ACK_SLOTS && slot == NULL; i++)
		if (ack_is(&epq_acks[i], loc, tid))
			slot = &epq_acks[i];
	if (slot == NULL)
	{
		slot = &epq_acks[epq_next];
		epq_next = (epq_next + 1) % SIFT_EPQ_ACK_SLOTS;
	}
	*slot = (SiftTblEpqAck) {.locator = *loc,.tid = tid,.cid = cid,.seen_xid = seen->xid,.seen_cid = seen->cid,.valid = true};
}

void
sift_epq_ack_clear(void)
{
	memset(epq_acks, 0, sizeof(epq_acks));
	epq_next = 0;
}

static bool
ack_consume(const RelFileLocator *loc, uint64 tid, CommandId cid, const SiftColDeltaLast *last)
{
	for (int i = 0; i < SIFT_EPQ_ACK_SLOTS; i++)
	{
		SiftTblEpqAck *a = &epq_acks[i];

		if (ack_is(a, loc, tid) && a->cid == cid && a->seen_xid == last->xid && a->seen_cid == last->cid)
		{
			a->valid = false;
			return true;
		}
	}
	return false;
}

SiftColDeltaVerdict
sift_col_delta_judge(const RelFileLocator *loc, uint64 tid, CommandId cid, const SiftColDeltaLast *last, const SiftColDeltaOps *ops)
{
	SiftColDeltaVerdict v = {.result = TM_Ok};

	if (!last->found)
		return v;
	if (ops->is_current(ops->ctx, last->xid))
	{
		if (last->cid >= cid)
			v.result = TM_SelfModified;
	}
	else if (ops->in_progress(ops->ctx, last->xid))
		v.result = TM_BeingModified;
	else if (!ops->visible(ops->ctx, last->xid, last->cid) && !ack_consume(loc, tid, cid, last))
		v.result = TM_Updated;
	if (v.result != TM_Ok)
	{
		v.xmax = last->xid;
		v.cmax = last->cid;
	}
	return v;
}

/* ---- the stage of a transaction ---- */

static uint32
part_slot_of(uint64 part_id, uint32 mask)
{
	return (uint32) ((part_id * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & mask;
}

static SiftCdPartBytes *
part_slot(SiftCdPartBytes *tab, uint32 cap, uint64 part_id)
{
	uint32		p = part_slot_of(part_id, cap - 1);

	while (tab[p].used && tab[p].part_id != part_id)
		p = (p + 1) & (cap - 1);
	return &tab[p];
}

static void
part_bytes_grow(SiftColDeltaStage *st)
{
	uint32		cap = Max(16, st->part_bytes_cap * 2);
	SiftCdPartBytes *tab = MemoryContextAllocZero(TopTransactionContext, sizeof(SiftCdPartBytes) * cap);

	for (uint32 i = 0; i < st->part_bytes_cap; i++)
		if (st->part_bytes_tab[i].used)
			*part_slot(tab, cap, st->part_bytes_tab[i].part_id) = st->part_bytes_tab[i];
	if (st->part_bytes_tab != NULL)
		pfree(st->part_bytes_tab);
	st->part_bytes_tab = tab;
	st->part_bytes_cap = cap;
}

void
sift_col_delta_stage_book(SiftColDeltaStage *st, const SiftStageColDelta *it, int64 bytes, int items)
{
	SiftCdPartBytes *ent;

	if ((st->part_bytes_n + 1) * 2 > st->part_bytes_cap)
		part_bytes_grow(st);
	ent = part_slot(st->part_bytes_tab, st->part_bytes_cap, it->part_id);
	if (!ent->used)
	{
		ent->used = true;
		ent->part_id = it->part_id;
		st->part_bytes_n++;
	}
	SIFT_INVARIANT(bytes >= 0 || ent->bytes >= (uint64) -bytes, "the books of a column delta stage hold fewer bytes for partition %llu than a record leaving it", (unsigned long long) it->part_id);
	ent->bytes += (uint64) bytes;
	st->bytes += (uint64) bytes;
	if (it->subxid != TopSubTransactionId)
		st->sub_items += items;
}

static uint32
tid_slot(const uint64 *tab, uint32 cap, uint64 tid)
{
	uint32		i = (uint32) ((tid * UINT64CONST(0x9E3779B97F4A7C15)) >> 40) & (cap - 1);

	while (tab[i] != 0 && tab[i] != tid + 1)
		i = (i + 1) & (cap - 1);
	return i;
}

static void
tids_add(SiftColDeltaStage *st, uint64 tid)
{
	uint32		i;

	if ((st->tids_n + 1) * 2 > st->tids_cap)
	{
		uint32		cap = Max(64, st->tids_cap * 2);
		uint64	   *tab = MemoryContextAllocZero(TopTransactionContext, sizeof(uint64) * cap);

		for (uint32 k = 0; k < st->tids_cap; k++)
			if (st->tids[k] != 0)
				tab[tid_slot(tab, cap, st->tids[k] - 1)] = st->tids[k];
		if (st->tids != NULL)
			pfree(st->tids);
		st->tids = tab;
		st->tids_cap = cap;
	}
	i = tid_slot(st->tids, st->tids_cap, tid);
	if (st->tids[i] == 0)
		st->tids_n++;
	st->tids[i] = tid + 1;
}

bool
sift_col_delta_stage_holds_in(const void *stage, uint64 lo, uint64 hi)
{
	const SiftColDeltaStage *st = stage;

	if (st->tids_n == 0 || lo > hi)
		return false;
	if (hi - lo >= st->tids_n)
	{
		for (uint32 i = 0; i < st->tids_cap; i++)
			if (st->tids[i] != 0 && st->tids[i] - 1 >= lo && st->tids[i] - 1 <= hi)
				return true;
		return false;
	}
	for (uint64 tid = lo;; tid++)
	{
		if (st->tids[tid_slot(st->tids, st->tids_cap, tid)] != 0)
			return true;
		if (tid == hi)
			return false;
	}
}

void
sift_col_delta_stage_add(SiftColDeltaStage *st, uint64 part_id, const uint8 *key_lo, SubTransactionId subxid, const uint8 *rec)
{
	SiftColDeltaEntryHdr h;
	SiftStageColDelta *it;

	memcpy(&h, rec, sizeof(h));
	if (st->n == st->cap)
	{
		st->cap = Max(16, st->cap * 2);
		st->items = st->items != NULL ? repalloc(st->items, sizeof(SiftStageColDelta) * st->cap)
			: MemoryContextAlloc(TopTransactionContext, sizeof(SiftStageColDelta) * st->cap);
	}
	it = &st->items[st->n++];
	it->part_id = part_id;
	memcpy(it->key_lo, key_lo, SIFT_BOUND_BYTES);
	it->subxid = subxid;
	it->rec = MemoryContextAlloc(TopTransactionContext, h.rec_bytes);
	memcpy(it->rec, rec, h.rec_bytes);
	tids_add(st, sift_col_delta_hdr_tid(&h));
	sift_col_delta_stage_book(st, it, h.rec_bytes, 1);
}

static uint32
item_bytes(const SiftStageColDelta *it)
{
	SiftColDeltaEntryHdr h;

	memcpy(&h, it->rec, sizeof(h));
	return h.rec_bytes;
}

void
sift_col_delta_stage_release(SiftColDeltaStage *st, int i)
{
	SiftStageColDelta *it = &st->items[i];

	sift_col_delta_stage_book(st, it, -(int64) item_bytes(it), -1);
	pfree(it->rec);
	it->rec = NULL;
}

/*
 * A subtransaction stages after it begins and nothing else stages while it runs, so its records, and those of the subtransactions released into it, are the last of the stage: the walk from
 * the end stops at the first record of an ancestor.
 */
static void
drop_sub_tail(SiftColDeltaStage *st, SubTransactionId sub)
{
	int			i = st->n;

	while (i > 0 && (st->items[i - 1].rec == NULL || st->items[i - 1].subxid == sub))
		i--;
	for (int k = i; k < st->n; k++)
		if (st->items[k].rec != NULL)
			sift_col_delta_stage_release(st, k);
	st->n = i;
}

void
sift_col_delta_stage_drop_sub(SiftColDeltaStage *st, SubTransactionId sub)
{
	if (st->sub_items > 0)
		drop_sub_tail(st, sub);
}

void
sift_col_delta_stage_promote_sub(SiftColDeltaStage *st, SubTransactionId sub, SubTransactionId parent)
{
	if (st->sub_items == 0)
		return;
	for (int i = st->n; i > 0 && (st->items[i - 1].rec == NULL || st->items[i - 1].subxid == sub); i--)
		if (st->items[i - 1].rec != NULL)
		{
			st->items[i - 1].subxid = parent;
			if (parent == TopSubTransactionId)
				st->sub_items--;
		}
}

void
sift_col_delta_stage_normalize_top(SiftColDeltaStage *st, TransactionId top_xid)
{
	for (int i = 0; i < st->n; i++)
	{
		SiftColDeltaEntryHdr h;

		if (st->items[i].rec == NULL)
			continue;
		memcpy(&h, st->items[i].rec, sizeof(h));
		h.xmin = top_xid;
		h.flags &= ~SIFT_CDENT_FLAG_SUBXID;
		memcpy(st->items[i].rec, &h, sizeof(h));
	}
}

uint64
sift_col_delta_stage_part_bytes(const SiftColDeltaStage *st, uint64 part_id)
{
	const SiftCdPartBytes *ent;

	if (st->part_bytes_cap == 0)
		return 0;
	ent = part_slot(st->part_bytes_tab, st->part_bytes_cap, part_id);
	return ent->used ? ent->bytes : 0;
}

static int
by_part_cmp(const void *x, const void *y, void *arg)
{
	const SiftStageColDelta *items = arg;
	int			a = *(const int *) x,
				b = *(const int *) y;

	if (items[a].part_id != items[b].part_id)
		return items[a].part_id < items[b].part_id ? -1 : 1;
	return a < b ? -1 : a > b;
}

int *
sift_col_delta_stage_by_part(const SiftColDeltaStage *st, int *nlive)
{
	int		   *ord = palloc(sizeof(int) * Max(st->n, 1));

	*nlive = 0;
	for (int i = 0; i < st->n; i++)
		if (st->items[i].rec != NULL)
			ord[(*nlive)++] = i;
	qsort_arg(ord, *nlive, sizeof(int), by_part_cmp, st->items);
	return ord;
}

SiftColDeltaBatch
sift_col_delta_stage_take_run(SiftColDeltaStage *st, const int *ord, int lo, int hi, FullTransactionId next)
{
	SiftColDeltaBatch b = {0};

	for (int k = lo; k < hi; k++)
		if (st->items[ord[k]].rec != NULL)
		{
			sift_col_delta_batch_add(&b, next, st->items[ord[k]].rec);
			sift_col_delta_stage_release(st, ord[k]);
		}
	return b;
}

void
sift_col_delta_stage_clear(SiftColDeltaStage *st)
{
	for (int i = 0; i < st->n; i++)
		SIFT_INVARIANT(st->items[i].rec == NULL, "a column delta stage was cleared with a record in it");
	st->n = 0;
	st->tids_n = 0;
	if (st->tids != NULL)
		memset(st->tids, 0, sizeof(uint64) * st->tids_cap);
}

void
sift_col_delta_stage_free(SiftColDeltaStage *st)
{
	for (int i = 0; i < st->n; i++)
		if (st->items[i].rec != NULL)
			pfree(st->items[i].rec);
	if (st->items != NULL)
		pfree(st->items);
	if (st->part_bytes_tab != NULL)
		pfree(st->part_bytes_tab);
	if (st->tids != NULL)
		pfree(st->tids);
	memset(st, 0, sizeof(*st));
}
