/*
 * sift_table_seg.c - the column groups of a sift_table: the glue to the column engine (docs/COL-TAM.md sections 6, 7 and 12).
 *
 * A sealed write buffer becomes a column segment of the engine (sift_col_seg.h) with the same TIDs: one leaf per column (typed when
 * the engine stores the type natively, SIFT_COLS_VAR holding the exact datum image otherwise) and one VAR leaf of exceptions, the
 * exact images of the values a typed leaf cannot hold (a numeric NaN or infinity, a NaN payload of a float that the order
 * preserving key would alter). Rows of a group have no xmin: they are frozen; their deletion is the tombstone log's, by TID.
 * Reads go through the engine's column source (sift_col_open_table); a segment written for another column layout (after ALTER TABLE)
 * is read with its own schema, the columns being matched by attribute number.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "sift_col_am.h"
#include "sift_col_seg.h"
#include "sift_lane.h"
#include "sift_maint.h"
#include "sift_merge.h"
#include "sift_ops.h"
#include "sift_table.h"

#define SB_KEYS		(((uint64) SIFT_SB_PAGES) << 16)	/* keys of a superblock: (vblock << 16) | offset */

/* ---------- the column layout of a table ---------- */

struct SiftTblLayout
{
	SiftColSchema *schema;
	int			natts;
	int		   *leaf;			/* [natts + 1] the leaf of attribute a, -1 for a dropped column */
	int			exc;			/* the leaf of the exceptions */
};

static SiftColStore
att_store(Form_pg_attribute att, uint8 *scale)
{
	Oid			typid = getBaseType(att->atttypid);
	SiftColStore store;

	*scale = 0;
	if (typid == NUMERICOID)
		return sift_col_numeric_scale(att, scale) ? SIFT_COLS_INT : SIFT_COLS_VAR;
	if (typid == BPCHAROID)
		return SIFT_COLS_STR;
	if (sift_col_leaf_store(typid, &store))
		return store;
	return att->attbyval && (att->attlen == 1 || att->attlen == 2 || att->attlen == 4 || att->attlen == 8) ? SIFT_COLS_INT : SIFT_COLS_VAR;
}

static SiftTblLayout *
layout_build(Relation rel, MemoryContext cxt)
{
	TupleDesc	desc = RelationGetDescr(rel);
	SiftTblLayout *l = MemoryContextAllocZero(cxt, sizeof(SiftTblLayout));
	SiftColDef *defs = palloc0((desc->natts + 1) * sizeof(SiftColDef));
	int			n = 0;

	l->natts = desc->natts;
	l->leaf = MemoryContextAlloc(cxt, (desc->natts + 1) * sizeof(int));
	for (int a = 1; a <= desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);
		uint8		scale;
		SiftColStore store;

		l->leaf[a] = -1;
		if (att->attisdropped)
			continue;
		store = att_store(att, &scale);
		l->leaf[a] = n;
		defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = store,.attno = a,.type = sift_col_type_of_oid(getBaseType(att->atttypid)),
		.path = "",.scale = scale};
		n++;
	}
	l->exc = n;
	defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_VAR,.attno = 0,.type = SIFT_COLT_BYTEA,.path = "exceptions"};
	n++;
	if (n > SIFT_COL_MAX_COLS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a sift_table holds at most %d columns", SIFT_COL_MAX_COLS - 1),
				 errdetail("Table \"%s\" has %d.", RelationGetRelationName(rel), n - 1)));
	l->schema = sift_cschema_create(cxt, defs, n);
	pfree(defs);
	return l;
}

/* The compression policy of each leaf under sift.table_compression now (a setting of the backend that writes the segment) */
static SiftCcPolicy *
layout_policy(Relation rel, const SiftTblLayout *l)
{
	SiftCcPolicy *pol = palloc(l->schema->ncols * sizeof(SiftCcPolicy));

	for (int i = 0; i < l->schema->ncols; i++)
	{
		const char *name = i == l->exc ? "exceptions" : NameStr(TupleDescAttr(RelationGetDescr(rel), l->schema->cols[i].attno - 1)->attname);
		const char *err = sift_cc_policy_for_column(sift_tbl_compression_spec, name, &pol[i]);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid compression option: %s", err)));
	}
	return pol;
}

static const SiftTblLayout *
layout_of(Relation rel, SiftTblRel *st)
{
	if (st->layout == NULL)
		st->layout = layout_build(rel, st->cxt);
	return st->layout;
}

void
sift_tbl_layout_ready(Relation rel, SiftTblRel *st)
{
	(void) layout_of(rel, st);
}

void
sift_tbl_layout_check(Relation rel)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table layout check", ALLOCSET_SMALL_SIZES);

	(void) layout_build(rel, cx);
	MemoryContextDelete(cx);
}

/* ---------- values: datum <-> leaf ---------- */

static int64
int_key(int attlen, Datum d)
{
	switch (attlen)
	{
		case 1:
			return DatumGetChar(d);
		case 2:
			return DatumGetInt16(d);
		case 4:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

static Datum
int_datum(int attlen, int64 key)
{
	switch (attlen)
	{
		case 1:
			return CharGetDatum((char) key);
		case 2:
			return Int16GetDatum((int16) key);
		case 4:
			return Int32GetDatum((int32) key);
		default:
			return Int64GetDatum(key);
	}
}

/* The bytes of a datum as the table stores them: a by-value datum's attlen bytes, a fixed-size value, a varlena with its header */
static const char *
image_of(Form_pg_attribute att, Datum d, uint32 *len)
{
	if (att->attbyval)
	{
		char	   *b = palloc(sizeof(Datum));

		store_att_byval(b, d, att->attlen);
		*len = att->attlen;
		return b;
	}
	if (att->attlen > 0)
	{
		*len = att->attlen;
		return DatumGetPointer(d);
	}
	*len = VARSIZE_ANY(PG_DETOAST_DATUM_PACKED(d));
	return (const char *) PG_DETOAST_DATUM_PACKED(d);
}

static Datum
image_datum(Form_pg_attribute att, const char *p, uint32 len)
{
	char	   *c = palloc(Max(len, sizeof(Datum)));
	Datum		d;

	memcpy(c, p, len);
	if (!att->attbyval)
		return PointerGetDatum(c);
	d = fetch_att(c, true, att->attlen);
	pfree(c);
	return d;
}

static bool
f4_exact(float4 f)
{
	float4		r = (float4) sift_col_f8_from_key(sift_col_f8_key(f));

	return memcmp(&f, &r, sizeof(f)) == 0;
}

static bool
f8_exact(float8 f)
{
	float8		r = sift_col_f8_from_key(sift_col_f8_key(f));

	return memcmp(&f, &r, sizeof(f)) == 0;
}

/* The exceptions of a row: (attno, length, bytes) after one another */
static void
exc_add(StringInfo exc, int attno, const char *p, uint32 len)
{
	uint16		a = attno;

	appendBinaryStringInfo(exc, (char *) &a, sizeof(a));
	appendBinaryStringInfo(exc, (char *) &len, sizeof(len));
	appendBinaryStringInfo(exc, p, len);
}

static bool
exc_find(const SiftColVec *ev, uint32 i, int attno, const char **p, uint32 *len)
{
	const char *b = ev->vbytes + ev->voff[i],
			   *end = ev->vbytes + ev->voff[i + 1];

	while (b < end)
	{
		uint16		a;

		memcpy(&a, b, sizeof(a));
		memcpy(len, b + sizeof(a), sizeof(*len));
		b += sizeof(a) + sizeof(*len);
		if (a == attno)
		{
			*p = b;
			return true;
		}
		b += *len;
	}
	return false;
}

/* The leaf value of a datum; a value the typed leaf cannot hold becomes an overflow and its image an exception */
static void
value_in(const SiftColDef *def, Form_pg_attribute att, Datum d, SiftColValue *v, StringInfo exc)
{
	bool		fits = true;

	switch (def->store)
	{
		case SIFT_COLS_INT:
			if (def->type == SIFT_COLT_NUMERIC)
			{
				Numeric		n = DatumGetNumeric(d);

				if (numeric_is_nan(n) || numeric_is_inf(n))
					fits = false;
				else
					v->v.i = sift_col_numeric_key(def->scale, d, SIFT_COL_ROUND_NEAREST);
			}
			else
				v->v.i = int_key(att->attlen, d);
			break;
		case SIFT_COLS_F4:
			v->v.f = DatumGetFloat4(d);
			fits = f4_exact(DatumGetFloat4(d));
			break;
		case SIFT_COLS_F8:
			v->v.f = DatumGetFloat8(d);
			fits = f8_exact(DatumGetFloat8(d));
			break;
		case SIFT_COLS_STR:
			{
				text	   *t = DatumGetTextPP(d);

				v->v.s = VARDATA_ANY(t);
				v->len = VARSIZE_ANY_EXHDR(t);
				break;
			}
		default:
			v->v.s = image_of(att, d, &v->len);
	}
	if (!fits)
	{
		const char *p = image_of(att, d, &v->len);

		v->isnull = v->overflow = true;
		exc_add(exc, att->attnum, p, v->len);
	}
}

static Datum
value_out(const SiftColDef *def, Form_pg_attribute att, const SiftColVec *vec, uint32 i)
{
	switch (def->store)
	{
		case SIFT_COLS_INT:
			return def->type == SIFT_COLT_NUMERIC ? sift_col_key_numeric(def->scale, vec->i[i]) : int_datum(att->attlen, vec->i[i]);
		case SIFT_COLS_F4:
			return Float4GetDatum((float4) vec->f[i]);
		case SIFT_COLS_F8:
			return Float8GetDatum(vec->f[i]);
		case SIFT_COLS_STR:
			{
				uint32		code = vec->codes[i],
							len = SIFT_COL_DICT_LEN(vec->dict, code);
				text	   *t = palloc(VARHDRSZ + len);

				SET_VARSIZE(t, VARHDRSZ + len);
				memcpy(VARDATA(t), SIFT_COL_DICT_STR(vec->dict, code), len);
				return PointerGetDatum(t);
			}
		default:
			return image_datum(att, vec->vbytes + vec->voff[i], vec->voff[i + 1] - vec->voff[i]);
	}
}

/* The leaves of a row (values/isnull as heap_deform_tuple gives them) */
static void
row_in(const SiftTblLayout *l, TupleDesc desc, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row)
{
	StringInfoData exc;

	initStringInfo(&exc);
	row->tid = *tid;
	row->ncols = l->schema->ncols;
	row->val = palloc0(row->ncols * sizeof(SiftColValue));
	row->elem = palloc0(row->ncols * sizeof(SiftColValue *));
	for (int a = 1; a <= l->natts; a++)
	{
		if (l->leaf[a] < 0)
			continue;
		if (isnull[a - 1])
			row->val[l->leaf[a]].isnull = true;
		else
			value_in(&l->schema->cols[l->leaf[a]], TupleDescAttr(desc, a - 1), values[a - 1], &row->val[l->leaf[a]], &exc);
	}
	if (exc.len == 0)
		row->val[l->exc].isnull = true;
	else
	{
		row->val[l->exc].v.s = exc.data;
		row->val[l->exc].len = exc.len;
	}
}

/* ---------- reading: the column source of every schema the table's segments were written for ---------- */

typedef struct GroupSrc
{
	SiftColSource *src;
	const SiftColSchema *schema;
	int		   *leaf;			/* [natts + 1] the leaf of attribute a in this schema, -1 for none */
	int			exc;
} GroupSrc;

/* The schemas of the segments seen so far, by generation (a segment is immutable) */
typedef struct SchemaEnt
{
	uint64		gen;
	SiftColSchema *schema;
} SchemaEnt;

/* The superblocks and the cmin of a tagged group, read from its segment once (a segment is immutable) */
typedef struct TagSbs
{
	uint64		gen;
	CommandId	cid;
	uint32		nsb;
	uint32	   *sb;
} TagSbs;

struct SiftTblGroupCache
{
	int			n;
	SchemaEnt  *ent;
	int			cap;
	TagSbs	   *tsb;
	int			ntsb;
	SiftTblGroupScan *fetch;	/* serves the TID fetches while the directory and the backend's xmin stay as they were */
	uint32		fetch_version;
	TransactionId fetch_xmin;
};

typedef struct UnitTids
{
	int			src;
	uint32		ordinal;
	uint32		n;				/* live rows */
	ItemPointerData *tid;		/* of the live rows, ascending */
	uint32	   *ord;			/* of tid: the row index in the vectors of the unit, taken together with tid (VACUUM clears live bits in place) */
} UnitTids;

/* A unit of the scan's range: which source, its superblock, and its place among the units of that superblock */
typedef struct UnitRef
{
	int			src;
	uint32		sb;
	uint32		ordinal;
} UnitRef;

typedef struct VisArg
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	const SiftTblTags *tags;
} VisArg;

struct SiftTblGroupScan
{
	Relation	rel;
	SiftTblRel *st;
	TupleDesc	desc;
	MemoryContext cxt;
	MemoryContext rowcxt;
	MemoryContext unitcxt;		/* the TIDs, selection and vectors of the unit being read (reset by the next one) */
	int			nsrc;
	GroupSrc   *s;
	Datum	   *values;
	bool	   *isnull;
	SiftColVec *vec;
	Snapshot	snapshot;
	SiftTblTags *tags;			/* of the directory copy the scan reads */
	const SiftTblTag *tag;		/* of the current unit's group */
	bool		hidden;			/* the current unit is a tagged group's that the (MVCC) snapshot does not see: it has no rows to return */
	/* iteration */
	int			cur;
	bool		begun;
	uint32		sb_lo;
	uint32		sb_hi;
	SiftColUnit unit;
	SiftColSel	sel;
	ItemPointerData *tids;
	uint32	   *ord;			/* the row ordinals of tids: the position of a row in the vectors, which hold every row of the unit */
	uint32		n;
	uint32		i;
	bool		in_unit;
	bool		plain;
	/* the units of the range in the order of a forward scan, listed when a scan first goes backward; uidx is the current one (-1: none yet) */
	UnitRef    *units;
	uint32		nunits;
	int64		uidx;
	/* the rows of the superblock of the last block read */
	uint32		cache_sb;
	UnitTids   *cu;
	int			ncu;
	/* the unit of cu whose rows are read into vec (all of them: the next fetches of its rows decode nothing), valid for one cu */
	int			rd_unit;
	uint64		serial;			/* of cu */
	uint64		rd_serial;
	/* readers of the current unit that outlive the call that found the row: a slot holds a reference and its token */
	int			refs;
	bool		ended;
	bool		park_fill;		/* the fetch scan: the pages a fill pinned are released at once (nothing ends it before the transaction does) */
	uint32		token;		/* changes whenever another unit becomes the current one: vec, ord and unit are then not the slot's */
};

static struct SiftTblGroupCache *
gcache_of(SiftTblRel *st)
{
	if (st->gcache == NULL)
		st->gcache = MemoryContextAllocZero(st->cxt, sizeof(struct SiftTblGroupCache));
	return st->gcache;
}

static SiftColSchema *
schema_of(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	SiftColSegment *seg;
	SiftColSchema *schema;

	for (int i = 0; i < c->n; i++)
		if (c->ent[i].gen == e->gen)
			return c->ent[i].schema;
	seg = sift_cseg_open(rel, e, st->cxt);
	schema = (SiftColSchema *) seg->schema;
	sift_cseg_close(seg);
	for (int i = 0; i < c->n; i++)
		if (c->ent[i].schema->hash == schema->hash)
			schema = c->ent[i].schema;
	if (c->n == c->cap)
	{
		c->cap = Max(8, c->cap * 2);
		c->ent = c->ent ? repalloc(c->ent, c->cap * sizeof(SchemaEnt)) : MemoryContextAlloc(st->cxt, c->cap * sizeof(SchemaEnt));
	}
	c->ent[c->n++] = (SchemaEnt) {e->gen, schema};
	return schema;
}

/* ---------- tagged groups ---------- */

static const TagSbs *
tag_sbs(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	MemoryContext tmp;
	MemoryContext old;
	SiftColSegment *seg;
	TagSbs	   *t;

	for (int i = 0; i < c->ntsb; i++)
		if (c->tsb[i].gen == e->gen)
			return &c->tsb[i];
	tmp = AllocSetContextCreate(CurrentMemoryContext, "sift_table tag", ALLOCSET_SMALL_SIZES);
	seg = sift_cseg_open(rel, e, tmp);
	old = MemoryContextSwitchTo(st->cxt);
	c->tsb = c->tsb ? repalloc(c->tsb, (c->ntsb + 1) * sizeof(TagSbs)) : palloc((c->ntsb + 1) * sizeof(TagSbs));
	t = &c->tsb[c->ntsb++];
	*t = (TagSbs) {.gen = e->gen,.cid = SIFT_CSEG_TAG_AUX(seg),.nsb = seg->nsb,.sb = palloc(seg->nsb * sizeof(uint32))};
	MemoryContextSwitchTo(old);
	for (uint32 u = 0; u < seg->nsb; u++)
		t->sb[u] = seg->sbdir[u].sb;
	sift_cseg_close(seg);
	MemoryContextDelete(tmp);
	return t;
}

/* Forgets the groups that neither directory has any more */
static void
tag_sbs_prune(SiftTblRel *st, const SiftDirSnapshot *dir)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	int			n = 0;

	for (int i = 0; i < c->ntsb; i++)
	{
		bool		used = false;

		for (int k = 0; k < dir->nsegs && !used; k++)
			used = dir->segs[k].gen == c->tsb[i].gen;
		for (int k = 0; k < st->dir.nsegs && !used; k++)
			used = st->dir.segs[k].gen == c->tsb[i].gen;
		if (used)
			c->tsb[n++] = c->tsb[i];
		else
			pfree(c->tsb[i].sb);
	}
	c->ntsb = n;
}

SiftTblTags *
sift_tbl_tags_build(Relation rel, SiftTblRel *st, const SiftDirSnapshot *dir, MemoryContext cxt)
{
	SiftTblTags *t;
	int			n = 0;

	for (int i = 0; i < dir->nsegs; i++)
		n += sift_tbl_seg_tagged(&dir->segs[i]);
	t = MemoryContextAllocZero(cxt, offsetof(SiftTblTags, ent) + n * sizeof(SiftTblTag));
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		const TagSbs *s;

		if (!sift_tbl_seg_tagged(e))
			continue;
		s = tag_sbs(rel, st, e);
		t->ent[t->n++] = (SiftTblTag) {.xid = e->reserved,.cid = s->cid,.gen = e->gen,.nsb = s->nsb,
		.sb = memcpy(MemoryContextAlloc(cxt, s->nsb * sizeof(uint32)), s->sb, s->nsb * sizeof(uint32))};
	}
	tag_sbs_prune(st, dir);
	return t;
}

static int
cmp_u32(const void *a, const void *b)
{
	return pg_cmp_u32(*(const uint32 *) a, *(const uint32 *) b);
}

const SiftTblTag *
sift_tbl_tag_of_sb(const SiftTblTags *tags, uint32 sb)
{
	for (int i = 0; tags != NULL && i < tags->n; i++)
	{
		const SiftTblTag *t = &tags->ent[i];

		if (sb >= t->sb[0] && sb <= t->sb[t->nsb - 1] && bsearch(&sb, t->sb, t->nsb, sizeof(uint32), cmp_u32) != NULL)
			return t;
	}
	return NULL;
}

void
sift_tbl_group_rowver(SiftTblRowVer *rv, SiftKey key, const SiftTblTag *tag)
{
	*rv = (SiftTblRowVer) {.key = key,.wbuf = false,.xmin = tag ? tag->xid : InvalidTransactionId,.cmin = tag ? tag->cid : 0};
}

static void
src_map(GroupSrc *gs, int natts, MemoryContext cxt)
{
	gs->leaf = MemoryContextAlloc(cxt, (natts + 1) * sizeof(int));
	gs->exc = -1;
	for (int a = 0; a <= natts; a++)
		gs->leaf[a] = -1;
	for (int i = 0; i < gs->schema->ncols; i++)
	{
		int			attno = gs->schema->cols[i].attno;

		if (attno == 0)
			gs->exc = i;
		else if (attno <= natts)
			gs->leaf[attno] = i;
	}
}

static void
vis_sb_mask(void *arg, uint32 sb, uint64 *pagemask)
{
	VisArg	   *v = arg;
	bool		plain = !sift_tbl_tomb_any(v->st, sb * SB_KEYS, (sb + 1) * SB_KEYS - 1) &&
		sift_tbl_sb_inserted(v->rel, v->snapshot, v->tags, sb);

	memset(pagemask, plain ? 0xFF : 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

static int
vis_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	VisArg	   *v = arg;

	return sift_tbl_filter_page_tags(v->rel, v->snapshot, v->tags, blk, offmask);
}

SiftTblGroupScan *
sift_tbl_gscan_begin(Relation rel, TupleDesc desc, SiftTblRel *st, const SiftDirSnapshot *dir, Snapshot snapshot)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_table group scan", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftTblGroupScan *g = palloc0(sizeof(SiftTblGroupScan));
	VisArg	   *va = palloc(sizeof(VisArg));
	SiftColVisibility vis = {vis_sb_mask, vis_filter_page, NULL, va};
	const SiftColSchema **schemas = palloc0(Max(dir->nsegs, 1) * sizeof(SiftColSchema *));
	int			nschemas = 0;
	uint32		sbs = SIFT_TBL_NEXT_VBLOCK(&dir->meta) / SIFT_SB_PAGES + 1;

	g->tags = sift_tbl_tags_build(rel, st, dir, cxt);
	*va = (VisArg) {rel, st, snapshot, g->tags};
	g->snapshot = snapshot;
	g->rel = rel;
	g->st = st;
	g->desc = desc;
	g->cxt = cxt;
	g->rowcxt = AllocSetContextCreate(cxt, "sift_table group row", ALLOCSET_SMALL_SIZES);
	g->unitcxt = AllocSetContextCreate(cxt, "sift_table group unit", ALLOCSET_DEFAULT_SIZES);
	g->values = palloc(g->desc->natts * sizeof(Datum));
	g->isnull = palloc(g->desc->natts * sizeof(bool));
	g->vec = palloc(SIFT_COL_MAX_COLS * sizeof(SiftColVec));
	g->cache_sb = PG_UINT32_MAX;
	g->rd_unit = -1;
	g->uidx = -1;
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftColSchema *schema;
		int			k;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		schema = schema_of(rel, st, &dir->segs[i]);
		for (k = 0; k < nschemas && schemas[k] != schema; k++)
			;
		if (k == nschemas)
			schemas[nschemas++] = schema;
	}
	g->s = palloc0(Max(nschemas, 1) * sizeof(GroupSrc));
	for (int k = 0; k < nschemas; k++)
	{
		g->s[k].schema = schemas[k];
		g->s[k].src = sift_col_open_table(rel, schemas[k], dir, &vis, sbs, cxt);
		src_map(&g->s[k], g->desc->natts, cxt);
	}
	g->nsrc = nschemas;
	g->sb_hi = PG_UINT32_MAX;
	MemoryContextSwitchTo(old);
	return g;
}

/* Unpins the pages the segment readers hold (the source stays usable: a table source's close releases buffers only) */
static void
gscan_park(SiftTblGroupScan *g)
{
	for (int k = 0; k < g->nsrc; k++)
		sift_col_close(g->s[k].src);
}

void
sift_tbl_gscan_end(SiftTblGroupScan *g)
{
	gscan_park(g);
	g->ended = true;
	if (g->refs == 0)
		MemoryContextDelete(g->cxt);
}

void
sift_tbl_gscan_ref(SiftTblGroupScan *g)
{
	g->refs++;
}

void
sift_tbl_gscan_unref(SiftTblGroupScan *g)
{
	if (--g->refs == 0 && g->ended)
		MemoryContextDelete(g->cxt);
}

void
sift_tbl_gscan_range(SiftTblGroupScan *g, uint32 sb_lo, uint32 sb_hi)
{
	g->sb_lo = sb_lo;
	g->sb_hi = sb_hi;
	g->cur = 0;
	g->begun = false;
	g->in_unit = false;
	g->rd_unit = -1;
	if (g->units)
		pfree(g->units);
	g->units = NULL;
	g->uidx = -1;
}

static void
unit_start(SiftTblGroupScan *g)
{
	uint64		lo = (uint64) g->unit.sb * SB_KEYS;
	MemoryContext old;

	g->rd_unit = -1;
	g->token++;
	MemoryContextReset(g->unitcxt);
	g->tag = sift_tbl_tag_of_sb(g->tags, g->unit.sb);
	g->hidden = g->tag != NULL && g->snapshot != NULL && g->snapshot->snapshot_type == SNAPSHOT_MVCC &&
		!sift_tbl_tag_visible(g->rel, g->snapshot, g->tag);
	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		g->vec[c].n = PG_UINT32_MAX;	/* not read yet */
	g->i = 0;
	g->in_unit = true;
	if (g->hidden)
	{
		g->n = 0;
		g->plain = false;
		return;
	}
	old = MemoryContextSwitchTo(g->unitcxt);
	sift_col_sel_all(&g->unit, &g->sel);
	g->n = g->sel.count;
	g->tids = palloc(Max(g->n, 1) * sizeof(ItemPointerData));
	sift_col_tids(&g->unit, &g->sel, g->tids);
	g->ord = palloc((g->n + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&g->sel, g->ord);
	MemoryContextSwitchTo(old);
	g->plain = !sift_tbl_tomb_any(g->st, lo, lo + SB_KEYS - 1) && (g->tag == NULL || (g->snapshot != NULL && g->snapshot->snapshot_type == SNAPSHOT_MVCC));
}

/* Makes unit k of the list the current one */
static void
unit_goto(SiftTblGroupScan *g, int64 k)
{
	const UnitRef *u = &g->units[k];

	g->cur = u->src;
	g->begun = false;
	sift_col_begin(g->s[u->src].src, NULL, 0, u->sb, u->sb + 1);
	for (uint32 j = 0; j <= u->ordinal; j++)
		if (!sift_col_next(g->s[u->src].src, &g->unit))
			elog(ERROR, "sift_table: the unit %u of superblock %u is gone", u->ordinal, u->sb);
	unit_start(g);
	g->uidx = k;
}

static bool
next_unit(SiftTblGroupScan *g)
{
	if (g->units)
	{
		if (g->uidx + 1 >= (int64) g->nunits)
			return false;
		unit_goto(g, g->uidx + 1);
		return true;
	}
	while (g->cur < g->nsrc)
	{
		if (!g->begun)
		{
			sift_col_begin(g->s[g->cur].src, NULL, 0, g->sb_lo, g->sb_hi);
			g->begun = true;
		}
		if (sift_col_next(g->s[g->cur].src, &g->unit))
		{
			unit_start(g);
			g->uidx++;
			return true;
		}
		g->cur++;
		g->begun = false;
	}
	return false;
}

/* The units of the range in the order next_unit returns them, found again by superblock and place in it, whichever way the scan goes */
static void
units_list(SiftTblGroupScan *g)
{
	MemoryContext old = MemoryContextSwitchTo(g->cxt);
	int			cap = 16;

	g->units = palloc(cap * sizeof(UnitRef));
	g->nunits = 0;
	for (int k = 0; k < g->nsrc; k++)
	{
		uint32		sb = PG_UINT32_MAX,
					ordinal = 0;
		SiftColUnit unit;

		sift_col_begin(g->s[k].src, NULL, 0, g->sb_lo, g->sb_hi);
		while (sift_col_next(g->s[k].src, &unit))
		{
			ordinal = unit.sb == sb ? ordinal + 1 : 0;
			sb = unit.sb;
			if (g->nunits == cap)
				g->units = repalloc(g->units, (cap *= 2) * sizeof(UnitRef));
			g->units[g->nunits++] = (UnitRef) {k, sb, ordinal};
		}
	}
	MemoryContextSwitchTo(old);
	g->begun = false;
}

/* Positions the scan after its last unit, so that sift_tbl_gscan_prev starts from the last row */
void
sift_tbl_gscan_seek_end(SiftTblGroupScan *g)
{
	if (g->units == NULL)
		units_list(g);
	g->uidx = g->nunits;
	g->in_unit = false;
}

/* The row before the one last returned (by sift_tbl_gscan_next too), in the scan's order; false at its start */
bool
sift_tbl_gscan_prev(SiftTblGroupScan *g, SiftTblRowVer *rv)
{
	for (;;)
	{
		if (g->in_unit && g->i > 1)
		{
			g->i--;
			sift_tbl_group_rowver(rv, sift_key_from_tid(&g->tids[g->i - 1]), g->tag);
			return true;
		}
		if (g->units == NULL)
			units_list(g);
		if (g->uidx <= 0)
		{
			g->in_unit = false;
			return false;
		}
		unit_goto(g, g->uidx - 1);
		g->i = g->n + 1;
	}
}

bool
sift_tbl_gscan_next(SiftTblGroupScan *g, SiftTblRowVer *rv)
{
	while (!g->in_unit || g->i >= g->n)
		if (!next_unit(g))
		{
			g->in_unit = false;
			return false;
		}
	sift_tbl_group_rowver(rv, sift_key_from_tid(&g->tids[g->i]), g->tag);
	g->i++;
	return true;
}

bool
sift_tbl_gscan_plain(const SiftTblGroupScan *g)
{
	return g->plain;
}

const SiftTblTag *
sift_tbl_gscan_tag(const SiftTblGroupScan *g, BlockNumber vblock)
{
	return sift_tbl_tag_of_sb(g->tags, vblock / SIFT_SB_PAGES);
}

/* The leaf c of the current unit with every row (in the unit's own order: cheaper to decode than a selection that leaves a few dead rows out), read on first use */
static const SiftColVec *
col_vec(SiftTblGroupScan *g, int c)
{
	if (g->vec[c].n == PG_UINT32_MAX)
	{
		MemoryContext old = MemoryContextSwitchTo(g->unitcxt);

		sift_col_read(&g->unit, c, NULL, &g->vec[c]);
		MemoryContextSwitchTo(old);
	}
	return &g->vec[c];
}

/* Attribute a of the row at position pos of the vectors of the current unit, allocated in the current memory context */
static void
fill_attr(SiftTblGroupScan *g, uint32 pos, int a, Datum *value, bool *isnull)
{
	const GroupSrc *gs = &g->s[g->cur];
	Form_pg_attribute att = TupleDescAttr(g->desc, a - 1);
	int			leaf = gs->leaf[a];
	const SiftColVec *vec;

	*isnull = false;
	*value = (Datum) 0;
	if (att->attisdropped)
		*isnull = true;
	else if (leaf < 0)
	{
		*value = getmissingattr(g->desc, a, isnull);
		if (!*isnull)
			*value = datumCopy(*value, att->attbyval, att->attlen);	/* like every value: the caller owns it */
	}
	else if ((vec = col_vec(g, leaf))->nulls != NULL && sift_col_bit_test(vec->nulls, pos))
	{
		const char *p;
		uint32		len;

		if (vec->ovf != NULL && sift_col_bit_test(vec->ovf, pos) && exc_find(col_vec(g, gs->exc), pos, a, &p, &len))
			*value = image_datum(att, p, len);
		else
			*isnull = true;
	}
	else
		*value = value_out(&gs->schema->cols[leaf], att, vec, pos);
}

bool
sift_tbl_gscan_fill(SiftTblGroupScan *g, uint32 token, uint32 pos, int from, int to, Datum *values, bool *isnull)
{
	if (g->token != token)
		return false;
	for (int a = from + 1; a <= to; a++)
		fill_attr(g, pos, a, &values[a - 1], &isnull[a - 1]);
	if (g->park_fill)
		gscan_park(g);
	return true;
}

uint32
sift_tbl_gscan_cursor(const SiftTblGroupScan *g, ItemPointer tid, uint32 *pos)
{
	*tid = g->tids[g->i - 1];
	*pos = g->ord[g->i - 1];
	return g->token;
}

/* The tuple of row i of the vectors read (position i of the selection) */
static HeapTuple
row_tuple(SiftTblGroupScan *g, uint32 i, ItemPointer tid)
{
	MemoryContext old = MemoryContextSwitchTo(g->rowcxt);
	HeapTuple	tup;

	for (int a = 1; a <= g->desc->natts; a++)
		fill_attr(g, i, a, &g->values[a - 1], &g->isnull[a - 1]);
	MemoryContextSwitchTo(old);
	tup = heap_form_tuple(g->desc, g->values, g->isnull);
	MemoryContextReset(g->rowcxt);
	tup->t_self = *tid;
	tup->t_tableOid = RelationGetRelid(g->rel);
	return tup;
}

HeapTuple
sift_tbl_gscan_tuple(SiftTblGroupScan *g)
{
	return row_tuple(g, g->ord[g->i - 1], &g->tids[g->i - 1]);
}

/* ---------- the rows of one vblock ---------- */

/* The TIDs of every unit of superblock sb (ascending, one array per unit) */
static void
cache_superblock(SiftTblGroupScan *g, uint32 sb)
{
	MemoryContext old;
	int			cap = 4;

	if (g->cache_sb == sb)
		return;
	if (g->cu)
	{
		for (int k = 0; k < g->ncu; k++)
		{
			pfree(g->cu[k].tid);
			pfree(g->cu[k].ord);
		}
		pfree(g->cu);
	}
	old = MemoryContextSwitchTo(g->cxt);
	g->cu = palloc(cap * sizeof(UnitTids));
	g->ncu = 0;
	for (int s = 0; s < g->nsrc; s++)
	{
		SiftColSource *src = g->s[s].src;
		SiftColUnit unit;
		SiftColSel sel;
		uint32		ordinal = 0;

		sift_col_begin(src, NULL, 0, sb, sb + 1);
		while (sift_col_next(src, &unit))
		{
			UnitTids   *u;

			if (g->ncu == cap)
				g->cu = repalloc(g->cu, (cap *= 2) * sizeof(UnitTids));
			u = &g->cu[g->ncu++];
			sift_col_sel_all(&unit, &sel);
			*u = (UnitTids) {s, ordinal++, sel.count, palloc(Max(sel.count, 1) * sizeof(ItemPointerData)), palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32))};
			sift_col_tids(&unit, &sel, u->tid);
			sift_col_sel_idx(&sel, u->ord);
		}
	}
	g->cache_sb = sb;
	g->serial++;
	MemoryContextSwitchTo(old);
}

/* First position of u->tid whose block is >= vblock */
static uint32
block_start(const UnitTids *u, BlockNumber vblock)
{
	uint32		lo = 0,
				hi = u->n;

	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;

		if (ItemPointerGetBlockNumberNoCheck(&u->tid[mid]) < vblock)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Makes unit k of the cached superblock the current one with every row read: its rows are fetched without decoding again */
static void
unit_load(SiftTblGroupScan *g, int k, uint32 sb)
{
	const UnitTids *u = &g->cu[k];
	SiftColSource *src = g->s[u->src].src;

	if (g->rd_unit == k && g->rd_serial == g->serial)
		return;
	g->rd_unit = -1;
	g->token++;
	g->cur = u->src;
	sift_col_begin(src, NULL, 0, sb, sb + 1);
	for (uint32 j = 0; j <= u->ordinal; j++)
		if (!sift_col_next(src, &g->unit))
			elog(ERROR, "sift_table: the unit %u of superblock %u is gone", u->ordinal, sb);
	MemoryContextReset(g->unitcxt);
	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		g->vec[c].n = PG_UINT32_MAX;
	g->rd_unit = k;
	g->rd_serial = g->serial;
}

int
sift_tbl_gscan_block(SiftTblGroupScan *g, BlockNumber vblock, const uint64 *offmask, HeapTuple *out)
{
	uint32		sb = vblock / SIFT_SB_PAGES;
	int			nout = 0;

	cache_superblock(g, sb);
	for (int k = 0; k < g->ncu; k++)
	{
		const UnitTids *u = &g->cu[k];
		uint32		first = block_start(u, vblock),
					last = first;

		while (last < u->n && ItemPointerGetBlockNumberNoCheck(&u->tid[last]) == vblock)
			last++;
		for (uint32 j = first; j < last; j++)
			if (offmask == NULL || sift_col_bit_test(offmask, ItemPointerGetOffsetNumberNoCheck(&u->tid[j]) - 1))
			{
				unit_load(g, k, sb);
				out[nout++] = row_tuple(g, u->ord[j], &u->tid[j]);
			}
	}
	return nout;
}

/* A fetch scan serving the TID fetches while the directory and the xmin of the backend (which keeps the segments from being recycled) stay */
static SiftTblGroupScan *
fetch_scan(Relation rel, SiftTblRel *st)
{
	struct SiftTblGroupCache *c = gcache_of(st);

	if (c->fetch && (c->fetch_version != st->dir.version || c->fetch_xmin != TransactionXmin || !TransactionIdIsValid(TransactionXmin)))
	{
		sift_tbl_gscan_end(c->fetch);
		c->fetch = NULL;
	}
	if (c->fetch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(st->cxt);

		c->fetch = sift_tbl_gscan_begin(rel, RelationGetDescr(rel), st, &st->dir, NULL);
		c->fetch->park_fill = true;
		c->fetch_version = st->dir.version;
		c->fetch_xmin = TransactionXmin;
		MemoryContextSwitchTo(old);
	}
	return c->fetch;
}

/* Where tid is among the cached units of its superblock: unit k, position j of its TIDs */
static bool
gscan_find(SiftTblGroupScan *g, ItemPointer tid, int *k, uint32 *j)
{
	BlockNumber vblock = ItemPointerGetBlockNumberNoCheck(tid);

	cache_superblock(g, vblock / SIFT_SB_PAGES);
	for (*k = 0; *k < g->ncu; (*k)++)
		for (*j = block_start(&g->cu[*k], vblock); *j < g->cu[*k].n && ItemPointerGetBlockNumberNoCheck(&g->cu[*k].tid[*j]) == vblock; (*j)++)
			if (ItemPointerEquals(&g->cu[*k].tid[*j], tid))
				return true;
	return false;
}

/* Is tid a live row of the groups? (the TIDs of its units, no values read) */
static bool
gscan_has(SiftTblGroupScan *g, ItemPointer tid)
{
	int			k;
	uint32		j;

	return gscan_find(g, tid, &k, &j);
}

bool
sift_tbl_group_locate(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblGroupScan **g, uint32 *token, uint32 *pos)
{
	int			k;
	uint32		j;

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta))
		return false;
	*g = fetch_scan(rel, st);
	if (!gscan_find(*g, tid, &k, &j))
	{
		gscan_park(*g);
		return false;
	}
	unit_load(*g, k, ItemPointerGetBlockNumberNoCheck(tid) / SIFT_SB_PAGES);
	*pos = (*g)->cu[k].ord[j];
	*token = (*g)->token;
	gscan_park(*g);
	return true;
}

bool
sift_tbl_group_fetch(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv, bool lazy)
{
	SiftTblGroupScan *g;
	uint64		offmask[SIFT_OFFMASK_WORDS] = {0};
	HeapTuple	row[SIFT_TBL_MAX_OFF];
	int			found;

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta))
		return false;
	g = fetch_scan(rel, st);
	if (lazy)
	{
		bool		has = gscan_has(g, tid);

		gscan_park(g);
		sift_tbl_group_rowver(rv, sift_key_from_tid(tid), sift_tbl_gscan_tag(g, ItemPointerGetBlockNumberNoCheck(tid)));
		return has;
	}
	sift_col_bit_set(offmask, ItemPointerGetOffsetNumberNoCheck(tid) - 1);
	found = sift_tbl_gscan_block(g, ItemPointerGetBlockNumberNoCheck(tid), offmask, row);
	gscan_park(g);
	if (found == 0)
		return false;
	sift_tbl_group_rowver(rv, sift_key_from_tid(tid), sift_tbl_gscan_tag(g, ItemPointerGetBlockNumberNoCheck(tid)));
	rv->tuple = row[0];
	return true;
}

/* ---------- the column source of the aggregate scan (sift_table_src.c) ---------- */

const SiftColSchema *
sift_tbl_layout_schema(Relation rel, SiftTblRel *st)
{
	return layout_of(rel, st)->schema;
}

const SiftColSchema *
sift_tbl_seg_schema(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	return schema_of(rel, st, e);
}

/* The page idx of seg into copy; false for a page of overflow bytes */
static bool
seg_page(Relation rel, SiftTblSeg *seg, uint32 idx, Page copy)
{
	Buffer		buf = sift_lane_read_locked(rel, sift_tbl_wbuf_page(rel, seg, idx), BUFFER_LOCK_SHARE);

	sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
	memcpy(copy, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return !(TBL_PAGE(copy)->flags & SIFT_TBL_PAGE_OVFDATA);
}

SiftColBatch *
sift_tbl_wbuf_batch(Relation rel, SiftTblRel *st, SiftTblSeg *seg, int nseg, uint32 sb, MemoryContext mcxt)
{
	const SiftTblLayout *l = layout_of(rel, st);
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext rowcx = AllocSetContextCreate(CurrentMemoryContext, "sift_table batch row", ALLOCSET_SMALL_SIZES);
	SiftColBatch *b = sift_cbatch_create(mcxt, l->schema);
	Datum	   *values = palloc(desc->natts * sizeof(Datum));
	bool	   *isnull = palloc(desc->natts * sizeof(bool));
	PGAlignedBlock copy;

	sift_cbatch_reset(b, sb);
	for (int i = 0; i < nseg; i++)
		for (uint32 p = 0; p < seg[i].npages; p++)
		{
			BlockNumber vblock = seg[i].vbase + p;

			if (vblock / SIFT_SB_PAGES != sb || !seg_page(rel, &seg[i], p, copy.data))
				continue;
			for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
			{
				const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg[i].gen, off);
				MemoryContext old = MemoryContextSwitchTo(rowcx);
				ItemPointerData tid;
				SiftColRow	row;
				HeapTupleData tup;

				if (rec != NULL && !(rec->flags & SIFT_TBL_REC_DEAD))
				{
					ItemPointerSet(&tid, vblock, off);
					tup = *sift_tbl_rec_tuple(rel, rec, seg[i].gen, sift_key_from_tid(&tid));
					heap_deform_tuple(&tup, desc, values, isnull);
					row_in(l, desc, &tid, values, isnull, &row);
					sift_cbatch_add(b, &row);
				}
				MemoryContextSwitchTo(old);
				MemoryContextReset(rowcx);
			}
		}
	MemoryContextDelete(rowcx);
	pfree(values);
	pfree(isnull);
	return b;
}

/* ---------- promotion ---------- */

typedef enum Fate
{
	FATE_KEEP,
	FATE_DROP,
	FATE_WAIT
} Fate;

/* A row goes into the group when its insertion is decided and every snapshot sees it; VACUUM drops the rows it flagged DEAD */
static Fate
row_fate(const SiftTblRec *rec, TransactionId oldest, bool vacuum)
{
	if (rec->flags & SIFT_TBL_REC_DEAD)
		return vacuum ? FATE_DROP : FATE_WAIT;
	if (!TransactionIdIsNormal(rec->xmin))
		return FATE_KEEP;
	return TransactionIdPrecedes(rec->xmin, oldest) && !sift_tbl_xid_aborted(rec->xmin) ? FATE_KEEP : FATE_WAIT;
}

/* What a sealed write buffer can become */
typedef enum SegClass
{
	SEG_WAIT,					/* some row is not final and the segment cannot carry it */
	SEG_FINAL,					/* every row is final: a frozen group */
	SEG_TAGGED					/* every row is one command's of one running transaction: a group that carries that tag */
} SegClass;

/* The fate of the rows of one writer, looked up once for the run of records that share it */
typedef struct FateMemo
{
	TransactionId xmin;
	Fate		fate;
	bool		aborted;
	bool		valid;
} FateMemo;

static Fate
fate_memo(FateMemo *m, const SiftTblRec *rec, TransactionId oldest, bool vacuum)
{
	if ((rec->flags & SIFT_TBL_REC_DEAD) || !m->valid || m->xmin != rec->xmin)
	{
		m->fate = row_fate(rec, oldest, vacuum);
		m->aborted = TransactionIdIsNormal(rec->xmin) && m->fate == FATE_WAIT && sift_tbl_xid_aborted(rec->xmin);
		m->xmin = rec->xmin;
		m->valid = !(rec->flags & SIFT_TBL_REC_DEAD);
	}
	return m->fate;
}

/*
 * What the rows of the segment allow. A segment whose rows are not all final becomes a tagged group only when pressure says the directory
 * is filling (tagged: out), and every row is the same command's of one live transaction (not killed or speculative, none final among them).
 * The newest rows are in the last pages, so a segment that cannot be promoted answers after one page.
 */
static SegClass
seg_class(Relation rel, SiftTblSeg *seg, TransactionId oldest, bool vacuum, bool pressure, SiftTblTag *tagged)
{
	PGAlignedBlock copy;
	FateMemo	memo = {0};
	bool		have = false;
	uint64		final = 0;

	for (uint32 p = seg->npages; p-- > 0;)
	{
		if (!seg_page(rel, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);

			if (rec == NULL)
				continue;
			if (fate_memo(&memo, rec, oldest, vacuum) != FATE_WAIT)
			{
				final += memo.fate == FATE_KEEP;
				if (have && final > 0)
					return SEG_WAIT;
				continue;
			}
			if (!pressure || (rec->flags & (SIFT_TBL_REC_DEAD | SIFT_TBL_REC_SPEC)) || !TransactionIdIsNormal(rec->xmin) || memo.aborted ||
				(have && (tagged->xid != rec->xmin || tagged->cid != rec->cmin)) || final > 0)
				return SEG_WAIT;
			tagged->xid = rec->xmin;
			tagged->cid = rec->cmin;
			have = true;
		}
	}
	return have ? SEG_TAGGED : SEG_FINAL;
}

typedef struct SealWriter
{
	Relation	rel;
	const SiftTblLayout *l;
	const SiftCcPolicy *policy;
	MemoryContext cxt;
	SiftColBatch *batch;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	uint64		expect;			/* bytes the segment will take at most (the write buffer pages): free runs shorter than a 256th are reused */
	const SiftTblTag *tag;		/* the writer of a segment whose rows are not final */
} SealWriter;

static void
seal_flush(SealWriter *sw)
{
	MemoryContext old = MemoryContextSwitchTo(sw->cxt);

	if (sw->batch->nrows > 0)
	{
		if (sw->w == NULL)
		{
			sw->wt = sift_wt_begin(sw->rel, SIFT_WRITE_SHARED);
			sift_wt_expect(sw->wt, sw->expect);
			sw->w = sift_colw_begin(sw->wt, sift_store_new_gen(sw->rel), sw->l->schema, sw->policy, sw->cxt);
			if (sw->tag)
				sift_colw_set_tag(sw->w, sw->tag->xid, sw->tag->cid);
		}
		sift_colw_add(sw->w, sw->batch);
	}
	MemoryContextSwitchTo(old);
}

/*
 * The rows of the segment e as one column segment with the same TIDs, frozen (every row final) or carrying tag (the writer of them all);
 * the write buffer pages are released with the switch
 */
static void
seal_segment(Relation rel, SiftTblRel *st, const SiftSegEntry *e, SiftTblSeg *seg, TransactionId oldest, bool vacuum, const SiftTblTag *tag)
{
	const SiftTblLayout *l = layout_of(rel, st);
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext rowcx = AllocSetContextCreate(cx, "sift_table promote row", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SealWriter	sw = {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL, (uint64) seg->npages * BLCKSZ, tag};
	PGAlignedBlock copy;
	Datum	   *values = palloc(desc->natts * sizeof(Datum));
	bool	   *isnull = palloc(desc->natts * sizeof(bool));
	SiftSegEntry neu;
	SiftMaintSwitch sw_dir = {.old = &e->gen,.nold = 1};
	SiftExtent *ext;
	int			next;
	uint32		sb = PG_UINT32_MAX;

	for (uint32 p = 0; p < seg->npages; p++)
	{
		BlockNumber vblock = seg->vbase + p;

		if (!seg_page(rel, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);
			ItemPointerData tid;
			SiftColRow	row;
			HeapTupleData tup;

			if (rec == NULL || row_fate(rec, oldest, vacuum) == FATE_DROP)
				continue;
			CHECK_FOR_INTERRUPTS();
			MemoryContextSwitchTo(rowcx);
			ItemPointerSet(&tid, vblock, off);
			tup = *sift_tbl_rec_tuple(rel, rec, seg->gen, sift_key_from_tid(&tid));
			heap_deform_tuple(&tup, desc, values, isnull);
			row_in(l, desc, &tid, values, isnull, &row);
			if (vblock / SIFT_SB_PAGES != sb)
			{
				seal_flush(&sw);
				sb = vblock / SIFT_SB_PAGES;
				sift_cbatch_reset(sw.batch, sb);
			}
			sift_cbatch_add(sw.batch, &row);
			MemoryContextSwitchTo(cx);
			MemoryContextReset(rowcx);
		}
	}
	seal_flush(&sw);
	if (sw.w)
	{
		sift_colw_finish(sw.w, &neu);
		sift_wt_finish(sw.wt);
		sw_dir.neu = &neu;
	}
	ext = sift_lane_extents(rel, e, &next);
	sift_dir_update(rel, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw_dir,.release = ext,.nrelease = next,
			.extra = InvalidBuffer,.immediate = st->fresh});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* The directory is more than half full: the rows of a running transaction cannot wait in row format any longer */
static bool
dir_pressure(const SiftTblRel *st)
{
	return st->dir.nsegs > sift_tbl_max_segments / 2;
}

static int
promote_sealed(Relation rel, SiftTblRel *st, TransactionId oldest, bool vacuum)
{
	int			promoted = 0;
	SiftDirSnapshot *dir;
	bool		pressure;

	sift_tbl_refresh(rel, st);
	pressure = dir_pressure(st);
	dir = MemoryContextAlloc(CurrentMemoryContext, sizeof(SiftDirSnapshot));
	*dir = st->dir;
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		SiftTblSeg seg = {.gen = e->gen,.hdr = e->header_blk,.vbase = (uint64) e->sb_lo * SIFT_TBL_VBLOCKS_SB};
		SiftTblTag	tag = {0};
		SegClass	class;

		if (e->kind != SIFT_SEG_WBUF_SEALED || !(e->flags & SIFT_TBL_SEG_WBUF))
			continue;
		sift_tbl_seg_load(rel, &seg);
		if ((class = seg_class(rel, &seg, oldest, vacuum, pressure, &tag)) == SEG_WAIT)
			continue;
		seal_segment(rel, st, e, &seg, oldest, vacuum, class == SEG_TAGGED ? &tag : NULL);
		promoted++;
	}
	pfree(dir);
	if (promoted > 0)
		sift_tbl_refresh(rel, st);
	return promoted;
}

int
sift_tbl_promote(Relation rel, SiftTblRel *st, TransactionId oldest, bool vacuum)
{
	int			promoted;

	if (!sift_maint_lock(rel, vacuum))
		return 0;
	promoted = promote_sealed(rel, st, oldest, vacuum);
	sift_maint_unlock(rel);
	return promoted;
}

typedef struct FreezeArg
{
	const uint64 *gens;
	int			n;
} FreezeArg;

static void
freeze_groups_swap(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const FreezeArg *a = arg;

	for (int i = 0; i < dir->nsegs; i++)
		for (int k = 0; k < a->n; k++)
			if (dir->segs[i].gen == a->gens[k])
				dir->segs[i].reserved = 0;
}

int
sift_tbl_freeze_groups(Relation rel, SiftTblRel *st, TransactionId oldest)
{
	uint64		gens[SIFT_MAX_SEGMENTS];
	int			n = 0;

	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];

		if (sift_tbl_seg_tagged(e) && TransactionIdPrecedes(e->reserved, oldest) && TransactionIdDidCommit(e->reserved))
			gens[n++] = e->gen;
	}
	if (n == 0)
		return 0;
	sift_dir_update(rel, &(SiftDirChange) {.fn = freeze_groups_swap,.arg = &(FreezeArg) {gens, n},.extra = InvalidBuffer});
	sift_tbl_refresh(rel, st);
	return n;
}

int
sift_tbl_drop_aborted_groups(Relation rel, SiftTblRel *st, const uint64 *gens, int ngens)
{
	SiftSegEntry gone[SIFT_MAX_SEGMENTS];
	int			n = 0;
	SiftMaintInputs mi;

	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
		for (int k = 0; k < ngens; k++)
			if (st->dir.segs[i].gen == gens[k] && sift_tbl_seg_tagged(&st->dir.segs[i]) && sift_tbl_xid_aborted(st->dir.segs[i].reserved))
				gone[n++] = st->dir.segs[i];
	if (n == 0)
		return 0;
	mi = sift_maint_inputs(rel, gone, n);
	sift_maint_replace(rel, mi.gens, n, NULL, mi.release, mi.nrelease, psprintf("dropped %d column segments of aborted transactions of", n),
					   GetCurrentTimestamp(), 0);
	sift_tbl_refresh(rel, st);
	return n;
}

/* Merge steps a writer takes when it closes a write buffer: a load never waits long for it, the next close takes the next steps */
#define SETTLE_MERGE_STEPS	4

/*
 * A writer closed a write buffer: promote the segments whose rows have settled, then merge the column segments while the directory is more
 * than half full. Only VACUUM merged, so a load in many transactions left one small column segment per closed write buffer and ran out of
 * the directory entries (SIFT_MAX_SEGMENTS) long before autovacuum came. A load in one transaction cannot settle, and from half the directory
 * on its write buffers become tagged groups (seg_class) that merge like the others, so it does not run out of them either.
 * Everything here is under the maintenance lock, taken only if it is free: the steps are VACUUM's too.
 */
void
sift_tbl_settle(Relation rel, SiftTblRel *st, TransactionId oldest)
{
	if (!sift_maint_lock(rel, false))
		return;
	(void) promote_sealed(rel, st, oldest, false);
	(void) sift_tbl_freeze_groups(rel, st, oldest);
	for (int i = 0; i < SETTLE_MERGE_STEPS && dir_pressure(st) && sift_tbl_merge(rel, st); i++)
		CHECK_FOR_INTERRUPTS();
	sift_maint_unlock(rel);
}

int64
sift_tbl_group_bulkdelete(Relation rel, SiftTblRel *st, IndexBulkDeleteCallback cb, void *state, uint32 sb_lo, uint32 sb_hi)
{
	IndexBulkDeleteResult stats = {0};

	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftColSegment *seg;

		if (e->kind != SIFT_SEG_IMMUTABLE || e->sb_hi < sb_lo || e->sb_lo > sb_hi)
			continue;
		seg = sift_cseg_open(rel, e, CurrentMemoryContext);
		sift_cseg_bulkdelete(seg, cb, state, &stats);
		sift_cseg_close(seg);
	}
	return stats.tuples_removed;
}

/* A group that is not mostly dead: while its rows are not final nothing is dead in the sense of the policy (VACUUM rewrites it once frozen) */
static bool
never_dead(Relation rel, const SiftSegEntry *e)
{
	return false;
}

/* The cmin of a tagged group */
static CommandId
group_cid(const SiftTblTags *tags, const SiftSegEntry *e)
{
	for (int i = 0; i < tags->n; i++)
		if (tags->ent[i].gen == e->gen)
			return tags->ent[i].cid;
	return 0;
}

/*
 * The entries of dir that merge together under the policy of the engine: the ones without a tag (with the write buffers, which only count)
 * or, for the tag (xid, cid), the groups that carry it. A group merges with its own writer's command alone: the merged group has one tag.
 */
static void
merge_view(SiftDirSnapshot *view, const SiftDirSnapshot *dir, const SiftTblTags *tags, TransactionId xid, CommandId cid)
{
	view->version = dir->version;
	view->meta = dir->meta;
	view->nsegs = 0;
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];

		if (xid == InvalidTransactionId ? !sift_tbl_seg_tagged(e) : (sift_tbl_seg_tagged(e) && e->reserved == xid && group_cid(tags, e) == cid))
			view->segs[view->nsegs++] = *e;
	}
}

/* The smallest groups of a view when the directory is filling and the policy waits for more of a size (the least a merge frees: fanin - 1 entries) */
static int
merge_smallest(SiftDirSnapshot *view, SiftSegEntry *sel)
{
	int			n = Min(view->nsegs, SIFT_MERGE_FANIN);

	if (n < 2)
		return 0;
	qsort(view->segs, view->nsegs, sizeof(SiftSegEntry), sift_seg_bytes_cmp);
	memcpy(sel, view->segs, n * sizeof(SiftSegEntry));
	return n;
}

/* The next merge: segments of the same tier without a tag, else the groups of a writer's command that the directory has too many of */
static int
merge_pick(Relation rel, SiftTblRel *st, SiftSegEntry *sel)
{
	uint64		tier_base = (uint64) sift_tbl_write_buffer_limit * 1024;
	SiftMergePolicy pol = {tier_base, SIFT_MERGE_FANIN, sift_tbl_mostly_dead};
	SiftMergePolicy tpol = {tier_base, SIFT_MERGE_FANIN, never_dead};
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table merge pick", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftDirSnapshot *view = palloc(sizeof(SiftDirSnapshot));
	SiftTblTags *tags = sift_tbl_tags_build(rel, st, &st->dir, cx);
	int			n;

	merge_view(view, &st->dir, tags, InvalidTransactionId, 0);
	n = sift_merge_pick(rel, &pol, view, false, sel);
	for (int i = 0; i < tags->n && n == 0; i++)
	{
		merge_view(view, &st->dir, tags, tags->ent[i].xid, tags->ent[i].cid);
		n = sift_merge_pick(rel, &tpol, view, false, sel);
		if (n == 0 && dir_pressure(st))
			n = merge_smallest(view, sel);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return n;
}

bool
sift_tbl_merge(Relation rel, SiftTblRel *st)
{
	const SiftTblLayout *l = layout_of(rel, st);
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			n;

	sift_tbl_refresh(rel, st);
	n = merge_pick(rel, st, sel);
	for (int i = 0; i < n; i++)
		if (schema_of(rel, st, &sel[i])->hash != l->schema->hash)
			return false;
	if (n == 0)
		return false;
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), NULL, sel, n);
	sift_tbl_refresh(rel, st);
	return true;
}

/*
 * Writes the column segment e again; the writer takes the free blocks lowest in the file first, so a segment above them moves down.
 * False when e was written for another column layout (before an ALTER TABLE), which only a rewrite of the table converts.
 */
bool
sift_tbl_rewrite(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	const SiftTblLayout *l = layout_of(rel, st);

	if (schema_of(rel, st, e)->hash != l->schema->hash)
		return false;
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), NULL, e, 1);
	sift_tbl_refresh(rel, st);
	return true;
}

/* ---------- sift.table_segments ---------- */

PG_FUNCTION_INFO_V1(sift_table_segments);
PG_FUNCTION_INFO_V1(sift_table_space);

/* One row per segment of the table: buffer (row format write buffers), sealed buffer (waiting for promotion), columns, tombstones */
Datum
sift_table_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	Snapshot	snap = RegisterSnapshot(GetTransactionSnapshot());

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	InitMaterializedSRF(fcinfo, 0);
	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		Datum		v[8];
		bool		nulls[8] = {0};

		v[0] = Int64GetDatum(e->gen);
		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(rel, e, CurrentMemoryContext);

			v[1] = CStringGetTextDatum("columns");
			v[2] = Int64GetDatum(e->ndocs);
			v[3] = Int64GetDatum(e->bytes);
			v[4] = Int32GetDatum(e->sb_lo);
			v[5] = Int32GetDatum(e->sb_hi);
			v[6] = Int32GetDatum(seg->nsb);
			v[7] = Int64GetDatum(e->ndocs - sift_cseg_live_rows(seg));
			sift_cseg_close(seg);
		}
		else
		{
			SiftTblSeg seg = {.gen = e->gen,.hdr = e->header_blk};

			v[1] = CStringGetTextDatum(e->flags & SIFT_TBL_SEG_TOMB ? "tombstones" :
									   e->kind == SIFT_SEG_WBUF_OPEN ? "buffer" : "sealed buffer");
			memset(nulls + 2, true, 6);
			if (e->flags & SIFT_TBL_SEG_WBUF)
			{
				sift_tbl_seg_load(rel, &seg);
				v[3] = Int64GetDatum((int64) seg.npages * BLCKSZ);
				v[4] = Int32GetDatum(e->sb_lo);
				v[5] = Int32GetDatum(e->sb_hi);
				nulls[3] = nulls[4] = nulls[5] = false;
			}
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	UnregisterSnapshot(snap);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_tbl_tagged);

/* The column groups of the table that still carry their writer (rows that are not final) */
Datum
sift_test_tbl_tagged(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	int32		n = 0;

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
		n += sift_tbl_seg_tagged(&st->dir.segs[i]);
	table_close(rel, AccessShareLock);
	PG_RETURN_INT32(n);
}
#endif

/* The space of a table as one row: the file, the blocks ever used, and the free list (reusable: no snapshot can read them) */
Datum
sift_table_space(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	TupleDesc	tupdesc;
	SiftFreeStats fs;
	Datum		v[5];
	bool		nulls[5] = {0};

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_tbl_refresh(rel, st);
	fs = sift_store_free_stats(rel);
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(rel));
	v[1] = Int64GetDatum(st->dir.meta.high_water);
	v[2] = Int64GetDatum(fs.blocks);
	v[3] = Int64GetDatum(fs.reusable);
	v[4] = Int64GetDatum(fs.records);
	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/* ---------- a rewrite (VACUUM FULL, CLUSTER): the new table written as column segments, without row-format pages ---------- */

struct SiftTblBulk
{
	SealWriter	sw;
	SiftTblRel *st;
	TupleDesc	desc;
	MemoryContext rowcx;
	Datum	   *values;
	bool	   *isnull;
	uint64		vbase;			/* row i has the TID (vbase + i / SIFT_TBL_MAX_OFF, i % SIFT_TBL_MAX_OFF + 1) */
	uint64		nrows;
	Size		budget;			/* memory a segment's writer may hold before the next one starts */
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
	int			nsegs;
};

typedef struct BulkPublish
{
	const SiftSegEntry *segs;
	int			n;
	uint64		next_vblock;
} BulkPublish;

static void
bulk_publish(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	BulkPublish *p = arg;

	for (int i = 0; i < p->n; i++)
	{
		dir->segs[dir->nsegs++] = p->segs[i];
		meta->ndocs += p->segs[i].ndocs;
	}
	SIFT_TBL_NEXT_VBLOCK(meta) = Max(SIFT_TBL_NEXT_VBLOCK(meta), p->next_vblock);
}

SiftTblBulk *
sift_tbl_bulk_begin(Relation rel, SiftTblRel *st)
{
	const SiftTblLayout *l = layout_of(rel, st);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table rewrite", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftTblBulk *b = palloc0(sizeof(SiftTblBulk));

	b->sw = (SealWriter) {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL, 0, NULL};
	b->st = st;
	b->desc = RelationGetDescr(rel);
	b->rowcx = AllocSetContextCreate(cx, "sift_table rewrite row", ALLOCSET_SMALL_SIZES);
	b->values = palloc(b->desc->natts * sizeof(Datum));
	b->isnull = palloc(b->desc->natts * sizeof(bool));
	b->budget = Max((Size) maintenance_work_mem * 1024 / 2, 64 * 1024 * 1024);
	sift_tbl_refresh(rel, st);
	b->vbase = TYPEALIGN(SIFT_TBL_VBLOCKS_SB, SIFT_TBL_NEXT_VBLOCK(&st->dir.meta));
	sift_cbatch_reset(b->sw.batch, b->vbase / SIFT_SB_PAGES);
	MemoryContextSwitchTo(old);
	return b;
}

static void
bulk_end_segment(SiftTblBulk *b)
{
	MemoryContext old = MemoryContextSwitchTo(b->sw.cxt);

	sift_colw_finish(b->sw.w, &b->segs[b->nsegs++]);
	sift_wt_finish(b->sw.wt);
	b->sw.w = NULL;
	MemoryContextSwitchTo(old);
}

/* Writes the batch as a unit; a segment whose writer passed the budget ends (while the directory has room for more) */
static void
bulk_flush(SiftTblBulk *b)
{
	seal_flush(&b->sw);
	if (b->sw.w && sift_colw_memory(b->sw.w) > b->budget && b->nsegs < SIFT_MAX_SEGMENTS - 8)
		bulk_end_segment(b);
}

/* The next dense TID; the batch moves to its superblock */
static void
bulk_next_tid(SiftTblBulk *b, ItemPointer tid)
{
	uint64		vblock = b->vbase + b->nrows / SIFT_TBL_MAX_OFF;

	ItemPointerSet(tid, vblock, b->nrows % SIFT_TBL_MAX_OFF + 1);
	if (vblock / SIFT_SB_PAGES != b->sw.batch->sb)
	{
		bulk_flush(b);
		sift_cbatch_reset(b->sw.batch, vblock / SIFT_SB_PAGES);
	}
	b->nrows++;
}

/* Adds a row of the table's layout (its strings and images are copied) under its new TID */
static void
bulk_row(SiftTblBulk *b, SiftColRow *row)
{
	bulk_next_tid(b, &row->tid);
	sift_cbatch_add(b->sw.batch, row);
}

void
sift_tbl_bulk_tuple(SiftTblBulk *b, HeapTuple tup)
{
	MemoryContext old = MemoryContextSwitchTo(b->rowcx);
	SiftColRow	row;

	heap_deform_tuple(tup, b->desc, b->values, b->isnull);
	row_in(b->sw.l, b->desc, &tup->t_self, b->values, b->isnull, &row);
	bulk_row(b, &row);
	MemoryContextSwitchTo(old);
	MemoryContextReset(b->rowcx);
}

/* Are all column segments of the old table written for the new table's layout (their units are copied as they decode)? */
static bool
bulk_same_layout(SiftTblBulk *b, Relation old, SiftTblRel *ost)
{
	for (int i = 0; i < ost->dir.nsegs; i++)
		if (ost->dir.segs[i].kind == SIFT_SEG_IMMUTABLE && schema_of(old, ost, &ost->dir.segs[i])->hash != b->sw.l->schema->hash)
			return false;
	return true;
}

static int
cmp_sb_lo(const void *a, const void *b)
{
	return pg_cmp_u32(((const SiftSegEntry *) a)->sb_lo, ((const SiftSegEntry *) b)->sb_lo);
}

/* The live rows of the units of segment e that keep accepts, decoded and re-keyed: no tuple is formed */
static void
bulk_segment(SiftTblBulk *b, Relation old, const SiftSegEntry *e, SiftTblKeep keep, void *arg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table rewrite segment", ALLOCSET_DEFAULT_SIZES);
	MemoryContext prev = MemoryContextSwitchTo(cx);
	SiftColSegment *seg = sift_cseg_open(old, e, cx);
	SiftColBatch *in = sift_cbatch_create(cx, seg->schema);
	SiftColRow	row = {0};

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		sift_cseg_decode_unit(seg, u, in);
		for (uint32 r = 0; r < in->nrows; r++)
		{
			SiftTblRowVer rv = {.key = in->keys[r],.wbuf = false,.xmin = e->reserved};	/* the writer of a group that is not final */

			if (!keep(&rv, arg))
				continue;
			sift_cbatch_row(in, r, &row);
			bulk_row(b, &row);
		}
		CHECK_FOR_INTERRUPTS();
	}
	sift_cseg_close(seg);
	MemoryContextSwitchTo(prev);
	MemoryContextDelete(cx);
}

/* The rows of a write buffer of the old table that keep accepts */
static void
bulk_wbuf(SiftTblBulk *b, Relation old, SiftTblSeg *seg, SiftTblKeep keep, void *arg)
{
	PGAlignedBlock copy;

	sift_tbl_seg_load(old, seg);
	for (uint32 p = 0; p < seg->npages; p++)
	{
		if (!seg_page(old, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);
			ItemPointerData tid;
			SiftTblRowVer rv;

			if (rec == NULL)
				continue;
			ItemPointerSet(&tid, seg->vbase + p, off);
			sift_tbl_rowver(&rv, &tid, rec);
			if (!keep(&rv, arg))
				continue;
			sift_tbl_rowver_tuple(old, &rv, rec, seg->gen, &tid);
			sift_tbl_bulk_tuple(b, rv.tuple);
			heap_freetuple(rv.tuple);
		}
		CHECK_FOR_INTERRUPTS();
	}
}

bool
sift_tbl_bulk_copy(SiftTblBulk *b, Relation old, SiftTblRel *ost, SiftTblKeep keep, void *arg)
{
	SiftSegEntry *segs;
	int			n = 0;

	sift_tbl_refresh(old, ost);
	if (!bulk_same_layout(b, old, ost))
		return false;
	segs = palloc(Max(ost->dir.nsegs, 1) * sizeof(SiftSegEntry));
	for (int i = 0; i < ost->dir.nsegs; i++)
		if (ost->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			segs[n++] = ost->dir.segs[i];
	qsort(segs, n, sizeof(SiftSegEntry), cmp_sb_lo);
	for (int i = 0; i < n; i++)
		bulk_segment(b, old, &segs[i], keep, arg);
	for (int i = 0; i < ost->nwbuf; i++)
		bulk_wbuf(b, old, &ost->wbuf[i], keep, arg);
	pfree(segs);
	return true;
}

void
sift_tbl_bulk_finish(SiftTblBulk *b)
{
	BulkPublish p;

	bulk_flush(b);
	if (b->sw.w)
		bulk_end_segment(b);
	p = (BulkPublish) {b->segs, b->nsegs, b->vbase + (b->nrows + SIFT_TBL_MAX_OFF - 1) / SIFT_TBL_MAX_OFF};
	if (b->nsegs > 0)
		sift_dir_update(b->sw.rel, &(SiftDirChange) {.fn = bulk_publish,.arg = &p,.extra = InvalidBuffer});
	sift_tbl_refresh(b->sw.rel, b->st);
	MemoryContextDelete(b->sw.cxt);
}

/* ---------- dead rows of the column segments ---------- */

int			sift_tbl_dead_rewrite_pct = SIFT_MERGE_DEAD_PCT;

bool
sift_tbl_mostly_dead(Relation rel, const SiftSegEntry *e)
{
	SiftColSegment *seg;
	bool		dead;

	if (!(e->flags & SIFT_SEG_DIRTY))
		return false;
	seg = sift_cseg_open(rel, e, CurrentMemoryContext);
	dead = sift_cseg_live_rows(seg) * 100 < e->ndocs * (100 - sift_tbl_dead_rewrite_pct);
	sift_cseg_close(seg);
	return dead;
}
