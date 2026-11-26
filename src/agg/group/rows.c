/*
 * agg/group/rows.c - the rows of the groups a table that cannot spill its states has no room for (agg/group/rows.h).
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/datum.h"
#include "utils/memutils.h"

#include "agg/ht/ht.h"
#include "agg/group/int.h"
#include "agg/gen/gen.h"
#include "agg/group/rows.h"

#define ROW_PARTS		16
#define PART_BITS		4
#define VEC_BYTES		(SIFT_VEC_LEN * 16 + SIFT_VEC_PAD + SIFT_VEC_ALIGN)
#define ROW_VALID		1
#define ROW_NAN			2
#define ROW_FIXED		5			/* the bytes of a record before its vectors: its length and its set */

/* a vector of the rows read back: what the vector says and the bytes its rows add up to */
typedef struct RowSlot
{
	SiftVec		vec;
	uint8		tag;			/* 0: none, else the type of the vector + 1 */
	int16		typlen;
	bool		typbyval;
	uint32		typid;
	uint8	   *data;
	uint64		valid[SIFT_VEC_WORDS];
	uint64		nan[SIFT_VEC_WORDS];
	StringInfoData bytes;
	bool		all_valid;
	bool		any_nan;
} RowSlot;

/* what a record says of one vector of its row */
typedef struct RowHead
{
	uint8		tag;
	uint8		flags;
	int16		scale;
	uint32		typid;
	int16		typlen;
	bool		typbyval;
} RowHead;

typedef struct RowFile
{
	BufFile    *file;
	uint32		level;
} RowFile;

struct V6Rows
{
	MemoryContext cx;
	uint32		level;			/* of the rows the table holds now: the ones it sends away are partitioned by the bits of this level */
	BufFile    *out[ROW_PARTS];
	RowFile    *pending;
	int			npending;
	int			cap;
	StringInfoData rec;
	StringInfoData key;
	int			nslots;
	int			set;
	uint16		n;
	RowSlot    *slot;
	SiftVec   **keyvec;
	SiftVec   **argvec;
	StringInfoData in;
};

static inline int
type_width(SiftVecType t)
{
	switch (t)
	{
		case SVEC_BOOL:
			return 1;
		case SVEC_I32:
			return 4;
		case SVEC_I64:
		case SVEC_F64:
		case SVEC_NUM64:
			return 8;
		default:
			return 16;
	}
}

static inline bool
type_is_string(SiftVecType t)
{
	return t == SVEC_STRVIEW || t == SVEC_DICT32;
}

static V6Rows *
rows_of(SiftV6Group *g)
{
	V6Rows	   *rs;
	MemoryContext cx;
	MemoryContext old;

	if (g->rows != NULL)
		return g->rows;
	cx = AllocSetContextCreate(g->cx, "sift agg group rows", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(cx);
	rs = g->rows = palloc0(sizeof(V6Rows));
	rs->cx = cx;
	rs->nslots = g->spec->nkeys + sift_v6_spec_nvec(g->spec);
	/* check-memory: allow the columns of the statement, not its rows */
	rs->slot = palloc0(rs->nslots * sizeof(RowSlot));
	/* check-memory: allow the keys of the statement, not its rows */
	rs->keyvec = palloc0(Max(g->spec->nkeys, 1) * sizeof(SiftVec *));
	/* check-memory: allow the vectors of the statement's aggregates, not its rows */
	rs->argvec = palloc0(Max(sift_v6_spec_nvec(g->spec), 1) * sizeof(SiftVec *));
	initStringInfo(&rs->rec);
	initStringInfo(&rs->key);
	initStringInfo(&rs->in);
	for (int i = 0; i < rs->nslots; i++)
	{
		rs->slot[i].data = (uint8 *) TYPEALIGN(SIFT_VEC_ALIGN, palloc0(VEC_BYTES));
		initStringInfo(&rs->slot[i].bytes);
	}
	MemoryContextSwitchTo(old);
	return rs;
}

static const uint8 *
string_of(const SiftVec *v, uint16 r, uint32 *len)
{
	if (v->type == SVEC_STRVIEW)
	{
		const SiftStrView *s = &((const SiftStrView *) v->data)[r];

		*len = s->len;
		return sift_strview_bytes(s);
	}
	else
	{
		const SiftGranuleDict *d = v->dict;
		const uint32 code = ((const uint32 *) v->data)[r];

		*len = d->off[code + 1] - d->off[code];
		return (const uint8 *) d->bytes + d->off[code];
	}
}

static void
put_bytes(StringInfo b, const void *p, uint32 len)
{
	appendBinaryStringInfo(b, (const char *) p, (int) len);
}

static void
put_u32(StringInfo b, uint32 x)
{
	put_bytes(b, &x, sizeof(x));
}

static void
put_slot(StringInfo b, const SiftVec *v, uint16 r)
{
	const bool	valid = is_valid(v, r);
	const uint8 tag = (uint8) ((v->type == SVEC_DICT32 ? SVEC_STRVIEW : v->type) + 1);
	const uint8 flags = (valid ? ROW_VALID : 0) | (v->nan != NULL && ((v->nan[r >> 6] >> (r & 63)) & 1) ? ROW_NAN : 0);

	put_bytes(b, &tag, 1);
	put_bytes(b, &flags, 1);
	put_bytes(b, &v->scale, sizeof(v->scale));
	if (v->type == SVEC_DATUM)
	{
		put_bytes(b, &v->typid, sizeof(v->typid));
		put_bytes(b, &v->typlen, sizeof(v->typlen));
		put_bytes(b, &v->typbyval, 1);
	}
	if (!valid)
		return;
	if (type_is_string(v->type))
	{
		uint32		len;
		const uint8 *p = string_of(v, r, &len);

		put_u32(b, len);
		put_bytes(b, p, len);
	}
	else if (v->type == SVEC_DATUM)
	{
		const Datum d = ((const Datum *) v->data)[r];
		const uint32 len = v->typbyval ? (uint32) sizeof(Datum) : (uint32) datumGetSize(d, false, v->typlen);

		put_u32(b, len);
		put_bytes(b, v->typbyval ? (const void *) &d : DatumGetPointer(d), len);
	}
	else
		put_bytes(b, (const uint8 *) v->data + (Size) r * type_width(v->type), (uint32) type_width(v->type));
}

/* the hash of the group of a row, whichever vectors carry its keys: the integer or the bytes of each */
static uint64
row_hash(const SiftV6Group *g, const V6Set *set, SiftVec *const *keyvec, uint16 r, StringInfo key)
{
	resetStringInfo(key);
	for (int i = 0; i < set->nk; i++)
	{
		const SiftVec *kv = keyvec[set->kidx[i]];
		const uint8 valid = is_valid(kv, r);

		put_bytes(key, &valid, 1);
		if (!valid)
			continue;
		if (sift_v6_class(g->spec->key[set->kidx[i]].typid) == SV6C_TEXT)
		{
			uint32		len;
			const uint8 *p = string_of(kv, r, &len);

			put_u32(key, len);
			put_bytes(key, p, len);
		}
		else
		{
			const int64 x = int_at(kv, r);

			put_bytes(key, &x, sizeof(x));
		}
	}
	return sift_aggk6_hash1(sift_aggk6_hash_bytes(key->data, (uint32) key->len));
}

static inline uint32
part_of(uint64 hash, uint32 level)
{
	return (uint32) (hash >> (64 - PART_BITS * ((level % 15) + 1))) & (ROW_PARTS - 1);
}

static void
put_row(SiftV6Group *g, V6Rows *rs, V6Set *set, SiftVec *const *keyvec, SiftVec *const *argvec, uint16 r)
{
	const uint8 s = (uint8) (set - g->set);
	const uint32 part = part_of(row_hash(g, set, keyvec, r, &rs->key), rs->level);
	uint32		len;

	resetStringInfo(&rs->rec);
	put_u32(&rs->rec, 0);
	put_bytes(&rs->rec, &s, 1);
	for (int i = 0; i < rs->nslots; i++)
	{
		const SiftVec *v = i < g->spec->nkeys ? keyvec[i] : argvec[i - g->spec->nkeys];

		if (v == NULL)
			put_bytes(&rs->rec, &(uint8) {0}, 1);
		else
			put_slot(&rs->rec, v, r);
	}
	len = (uint32) rs->rec.len;
	memcpy(rs->rec.data, &len, sizeof(len));
	if (rs->out[part] == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(rs->cx);

		rs->out[part] = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	BufFileWrite(rs->out[part], rs->rec.data, len);
}

uint16
v6g_rows_split(SiftV6Group *g, V6Set *set, SiftVec *const *keyvec, SiftVec *const *argvec, uint16 n)
{
	uint16		kept = 0;

	for (uint16 k = 0; k < n; k++)
	{
		if (g->slots[k] == NULL)
			put_row(g, g->rows, set, keyvec, argvec, g->sel[k]);
		else
		{
			g->sel[kept] = g->sel[k];
			g->slots[kept++] = g->slots[k];
		}
	}
	return kept;
}

bool
v6g_can_freeze(const SiftV6Group *g)
{
	bool		keyed = false;

	for (int s = 0; s < g->nsets; s++)
		keyed |= g->set[s].nk > 0;
	return keyed && g->gen != NULL && g->gkeys == NULL;
}

void
v6g_freeze(SiftV6Group *g)
{
	rows_of(g);
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].nk > 0)
		{
			g->set[s].frozen = true;
			g->set[s].ht.lock_at = Max(g->set[s].ht.count, 1);
		}
	g->frozen = true;
}

/* ---- the rows read back ---- */

static const uint8 *
get_bytes(const uint8 *p, void *out, uint32 len)
{
	memcpy(out, p, len);
	return p + len;
}

static const uint8 *
head_read(const uint8 *p, RowHead *h)
{
	*h = (RowHead) {0};
	p = get_bytes(p, &h->tag, 1);
	if (h->tag == 0)
		return p;
	p = get_bytes(p, &h->flags, 1);
	p = get_bytes(p, &h->scale, sizeof(h->scale));
	if (h->tag - 1 == SVEC_DATUM)
	{
		p = get_bytes(p, &h->typid, sizeof(h->typid));
		p = get_bytes(p, &h->typlen, sizeof(h->typlen));
		p = get_bytes(p, &h->typbyval, 1);
	}
	return p;
}

static const uint8 *
payload_skip(const RowHead *h, const uint8 *p)
{
	uint32		len;

	if (!(h->flags & ROW_VALID))
		return p;
	if (h->tag - 1 == SVEC_STRVIEW || h->tag - 1 == SVEC_DATUM)
	{
		memcpy(&len, p, sizeof(len));
		return p + sizeof(len) + len;
	}
	return p + type_width((SiftVecType) (h->tag - 1));
}

static bool
slot_is(const RowSlot *sl, const RowHead *h)
{
	return h->tag == sl->tag && (h->tag == 0 || (h->scale == sl->vec.scale && h->typid == sl->typid && h->typlen == sl->typlen && h->typbyval == sl->typbyval));
}

/* the first row of a batch names the types of its vectors: does the row at p have the same ones, and the same set */
static bool
row_same(const V6Rows *rs, const uint8 *p)
{
	if (p[4] != rs->set)
		return false;
	p += ROW_FIXED;
	for (int i = 0; i < rs->nslots; i++)
	{
		RowHead		h;

		p = head_read(p, &h);
		if (!slot_is(&rs->slot[i], &h))
			return false;
		p = payload_skip(&h, p);
	}
	return true;
}

static void
slot_begin(RowSlot *sl, const RowHead *h)
{
	sl->tag = h->tag;
	sl->typid = h->typid;
	sl->typlen = h->typlen;
	sl->typbyval = h->typbyval;
	sl->vec = (SiftVec) {.data = sl->data,.dense = true,.type = (SiftVecType) (h->tag - 1),.scale = h->scale,.typid = h->typid,.typlen = h->typlen,.typbyval = h->typbyval};
	memset(sl->valid, 0, sizeof(sl->valid));
	memset(sl->nan, 0, sizeof(sl->nan));
	resetStringInfo(&sl->bytes);
	sl->all_valid = true;
	sl->any_nan = false;
}

/* the row r of a vector: its payload at *pp */
static void
slot_add(RowSlot *sl, uint16 r, uint8 flags, const uint8 **pp)
{
	const uint8 *p = *pp;
	const SiftVecType t = (SiftVecType) (sl->tag - 1);
	const Size	width = t == SVEC_STRVIEW ? sizeof(SiftStrView) : t == SVEC_DATUM ? sizeof(Datum) : (Size) type_width(t);

	if (flags & ROW_VALID)
	{
		sl->valid[r >> 6] |= UINT64CONST(1) << (r & 63);
		if (t == SVEC_STRVIEW)
		{
			SiftStrView *s = &((SiftStrView *) sl->data)[r];
			uint32		len;

			p = get_bytes(p, &len, sizeof(len));
			memset(s, 0, sizeof(*s));
			s->len = len;
			if (len <= 12)
				memcpy((uint8 *) s + offsetof(SiftStrView, prefix), p, len);
			else
			{
				memcpy(s->prefix, p, 4);
				s->u.offset = (uint64) sl->bytes.len;
				put_bytes(&sl->bytes, p, len);
			}
			p += len;
		}
		else if (t == SVEC_DATUM)
		{
			uint32		len;

			p = get_bytes(p, &len, sizeof(len));
			if (sl->typbyval)
				memcpy(&((Datum *) sl->data)[r], p, sizeof(Datum));
			else
			{
				((Datum *) sl->data)[r] = (Datum) sl->bytes.len;
				put_bytes(&sl->bytes, p, len);
			}
			p += len;
		}
		else
			p = get_bytes(p, sl->data + (Size) r * width, (uint32) width);
	}
	else
	{
		sl->all_valid = false;
		memset(sl->data + (Size) r * width, 0, width);
	}
	if (flags & ROW_NAN)
	{
		sl->nan[r >> 6] |= UINT64CONST(1) << (r & 63);
		sl->any_nan = true;
	}
	*pp = p;
}

/* the pointers of the strings and of the by-reference datums of a batch, now that its bytes stay where they are */
static void
slot_end(RowSlot *sl, uint16 n)
{
	SiftVec    *v = &sl->vec;

	v->len = n;
	v->all_valid = sl->all_valid;
	v->valid = sl->all_valid ? NULL : sl->valid;
	v->nan = sl->any_nan ? sl->nan : NULL;
	for (uint16 r = 0; r < n; r++)
	{
		if (!is_valid(v, r))
			continue;
		if (v->type == SVEC_STRVIEW && ((SiftStrView *) v->data)[r].len > 12)
			((SiftStrView *) v->data)[r].u.ptr = sl->bytes.data + ((SiftStrView *) v->data)[r].u.offset;
		else if (v->type == SVEC_DATUM && !v->typbyval)
			((Datum *) v->data)[r] = PointerGetDatum(sl->bytes.data + ((Datum *) v->data)[r]);
	}
}

static void
batch_run(SiftV6Group *g, V6Rows *rs)
{
	SiftBatchSel bs;

	if (rs->n == 0)
		return;
	memset(bs.mask, 0, sizeof(bs.mask));
	for (uint16 r = 0; r < rs->n; r++)
		bs.mask[r >> 6] |= UINT64CONST(1) << (r & 63);
	bs.n_raw = bs.n_active = rs->n;
	bs.all_active = true;
	bs.has_sel = false;
	for (int i = 0; i < rs->nslots; i++)
	{
		SiftVec    *v = rs->slot[i].tag != 0 ? &rs->slot[i].vec : NULL;

		if (v != NULL)
			slot_end(&rs->slot[i], rs->n);
		if (i < g->spec->nkeys)
			rs->keyvec[i] = v;
		else
			rs->argvec[i - g->spec->nkeys] = v;
	}
	v6g_update_set(g, rs->set, &bs, rs->keyvec, rs->argvec);
	rs->n = 0;
}

static void
batch_start(V6Rows *rs, const uint8 *p)
{
	rs->set = p[4];
	p += ROW_FIXED;
	for (int i = 0; i < rs->nslots; i++)
	{
		RowHead		h;

		p = head_read(p, &h);
		if (h.tag == 0)
			rs->slot[i].tag = 0;
		else
			slot_begin(&rs->slot[i], &h);
		p = payload_skip(&h, p);
	}
}

static void
batch_add(V6Rows *rs, const uint8 *p)
{
	const uint16 r = rs->n++;

	p += ROW_FIXED;
	for (int i = 0; i < rs->nslots; i++)
	{
		RowHead		h;

		p = head_read(p, &h);
		if (h.tag != 0)
			slot_add(&rs->slot[i], r, h.flags, &p);
	}
}

static void
file_replay(SiftV6Group *g, V6Rows *rs, BufFile *f)
{
	uint32		len;
	uint64		n = 0;

	if (BufFileSeek(f, 0, 0, SEEK_SET) != 0)
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("could not rewind a spill file of the rows of the groups"),
						errhint("Check the free space of the temporary tablespace.")));
	while (BufFileReadMaybeEOF(f, &len, sizeof(len), true) != 0)
	{
		const uint8 *row;

		enlargeStringInfo(&rs->in, (int) len);
		row = (const uint8 *) rs->in.data;
		memcpy(rs->in.data, &len, sizeof(len));
		BufFileReadExact(f, rs->in.data + sizeof(len), len - sizeof(len));
		if (rs->n > 0 && (rs->n == SIFT_VEC_LEN || !row_same(rs, row)))
			batch_run(g, rs);
		if (rs->n == 0)
			batch_start(rs, row);
		batch_add(rs, row);
		if (++n % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
	batch_run(g, rs);
}

static void
pending_push(V6Rows *rs, BufFile *f, uint32 level)
{
	if (rs->npending == rs->cap)
	{
		MemoryContext old = MemoryContextSwitchTo(rs->cx);

		rs->cap = Max(rs->cap * 2, 32);
		rs->pending = rs->pending ? repalloc(rs->pending, rs->cap * sizeof(RowFile)) : palloc(rs->cap * sizeof(RowFile));
		MemoryContextSwitchTo(old);
	}
	rs->pending[rs->npending++] = (RowFile) {f, level};
}

/* the files the rows of the table went to are the tables to come: the rows of the next level of partitions */
static void
out_seal(V6Rows *rs)
{
	for (int p = ROW_PARTS - 1; p >= 0; p--)
		if (rs->out[p] != NULL)
		{
			pending_push(rs, rs->out[p], rs->level + 1);
			rs->out[p] = NULL;
		}
}

bool
v6g_rows_replay(SiftV6Group *g)
{
	V6Rows	   *rs = g->rows;
	RowFile		next;
	MemoryContext old;

	if (rs == NULL)
		return false;
	out_seal(rs);
	if (rs->npending == 0)
		return false;
	next = rs->pending[--rs->npending];
	old = MemoryContextSwitchTo(g->cx);
	sift_v6_group_recycle(g);
	v6_gen_restart(g->gen);
	for (int s = 0; s < g->nsets; s++)
	{
		g->set[s].frozen = false;
		g->set[s].ht.lock_at = 0;
	}
	g->frozen = false;
	rs->level = next.level;
	file_replay(g, rs, next.file);
	BufFileClose(next.file);
	sift_v6_group_finish(g);
	MemoryContextSwitchTo(old);
	return true;
}

void
v6g_rows_free(SiftV6Group *g)
{
	V6Rows	   *rs = g->rows;

	if (rs == NULL)
		return;
	for (int p = 0; p < ROW_PARTS; p++)
		if (rs->out[p] != NULL)
			BufFileClose(rs->out[p]);
	for (int i = 0; i < rs->npending; i++)
		BufFileClose(rs->pending[i].file);
	MemoryContextDelete(rs->cx);
	g->rows = NULL;
}
