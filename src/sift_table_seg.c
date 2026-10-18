/*
 * sift_table_seg.c - the column groups of a sift_table: the glue to the column engine (docs/COL-TAM.md sections 6, 7 and 12).
 *
 * A sealed delta segment becomes a column segment of the engine (sift_col_seg.h) with the same TIDs: one leaf per column (typed when
 * the engine stores the type natively, SIFT_COLS_VAR holding the exact datum image otherwise) and one VAR leaf of exceptions, the
 * exact images of the values a typed leaf cannot hold (a string over 255 bytes, a numeric NaN, a float that the order preserving
 * key would alter: -0, a NaN payload). Rows of a group have no xmin: they are frozen; their deletion is the tombstone log's, by TID.
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
		defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = store,.attno = a,.typid = getBaseType(att->atttypid),
		.path = "",.scale = scale};
		n++;
	}
	l->exc = n;
	defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_VAR,.attno = 0,.typid = BYTEAOID,.path = "exceptions"};
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

	memcpy(c, p, len);
	return att->attbyval ? fetch_att(c, true, att->attlen) : PointerGetDatum(c);
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
			if (def->typid == NUMERICOID)
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
				fits = v->len <= SIFT_COL_MAX_STR;
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
			return def->typid == NUMERICOID ? sift_col_key_numeric(def->scale, vec->i[i]) : int_datum(att->attlen, vec->i[i]);
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

struct SiftTblGroupCache
{
	int			n;
	SchemaEnt  *ent;
	int			cap;
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
	uint32	   *row;			/* their row ordinals in the unit (the positions of a selection) */
} UnitTids;

typedef struct VisArg
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
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
	/* iteration */
	int			cur;
	bool		begun;
	uint32		sb_lo;
	uint32		sb_hi;
	SiftColUnit unit;
	SiftColSel	sel;
	ItemPointerData *tids;
	uint32		n;
	uint32		i;
	bool		in_unit;
	bool		plain;
	/* the rows of the superblock of the last block read */
	uint32		cache_sb;
	UnitTids   *cu;
	int			ncu;
};

static SiftColSchema *
schema_of(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	struct SiftTblGroupCache *c = st->gcache;
	SiftColSegment *seg;
	SiftColSchema *schema;

	if (c == NULL)
		c = st->gcache = MemoryContextAllocZero(st->cxt, sizeof(*c));
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
	bool		plain = !sift_tbl_tomb_any(v->st, sb * SB_KEYS, (sb + 1) * SB_KEYS - 1);

	memset(pagemask, plain ? 0xFF : 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

static int
vis_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	VisArg	   *v = arg;

	return sift_tbl_filter_page(v->rel, v->snapshot, blk, offmask);
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

	*va = (VisArg) {rel, st, snapshot};
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
}

static void
unit_start(SiftTblGroupScan *g)
{
	uint64		lo = (uint64) g->unit.sb * SB_KEYS;
	MemoryContext old;

	MemoryContextReset(g->unitcxt);
	old = MemoryContextSwitchTo(g->unitcxt);
	sift_col_sel_all(&g->unit, &g->sel);
	g->n = g->sel.count;
	g->tids = palloc(Max(g->n, 1) * sizeof(ItemPointerData));
	sift_col_tids(&g->unit, &g->sel, g->tids);
	MemoryContextSwitchTo(old);
	g->i = 0;
	g->in_unit = true;
	g->plain = !sift_tbl_tomb_any(g->st, lo, lo + SB_KEYS - 1);
	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		g->vec[c].n = PG_UINT32_MAX;	/* not read yet */
}

static bool
next_unit(SiftTblGroupScan *g)
{
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
			return true;
		}
		g->cur++;
		g->begun = false;
	}
	return false;
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
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(&g->tids[g->i]),.delta = false,.xmin = InvalidTransactionId};
	g->i++;
	return true;
}

bool
sift_tbl_gscan_plain(const SiftTblGroupScan *g)
{
	return g->plain;
}

/* The leaves of selection sel of the current unit, once */
static void
unit_read(SiftTblGroupScan *g, const SiftColSel *sel)
{
	MemoryContext old = MemoryContextSwitchTo(g->unitcxt);

	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		sift_col_read(&g->unit, c, sel, &g->vec[c]);
	MemoryContextSwitchTo(old);
}

/* The tuple of row i of the vectors read (position i of the selection) */
static HeapTuple
row_tuple(SiftTblGroupScan *g, uint32 i, ItemPointer tid)
{
	const GroupSrc *gs = &g->s[g->cur];
	MemoryContext old = MemoryContextSwitchTo(g->rowcxt);
	HeapTuple	tup;

	for (int a = 1; a <= g->desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(g->desc, a - 1);
		int			leaf = gs->leaf[a];
		const SiftColVec *vec;

		g->isnull[a - 1] = false;
		g->values[a - 1] = (Datum) 0;
		if (att->attisdropped)
			g->isnull[a - 1] = true;
		else if (leaf < 0)
			g->values[a - 1] = getmissingattr(g->desc, a, &g->isnull[a - 1]);
		else if ((vec = &g->vec[leaf])->nulls != NULL && sift_col_bit_test(vec->nulls, i))
		{
			const char *p;
			uint32		len;

			if (vec->ovf != NULL && sift_col_bit_test(vec->ovf, i) && exc_find(&g->vec[gs->exc], i, a, &p, &len))
				g->values[a - 1] = image_datum(att, p, len);
			else
				g->isnull[a - 1] = true;
		}
		else
			g->values[a - 1] = value_out(&gs->schema->cols[leaf], att, vec, i);
	}
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
	if (g->vec[0].n == PG_UINT32_MAX)
		unit_read(g, &g->sel);
	return row_tuple(g, g->i - 1, &g->tids[g->i - 1]);
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
			pfree(g->cu[k].row);
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
			*u = (UnitTids) {s, ordinal++, sel.count, palloc(Max(sel.count, 1) * sizeof(ItemPointerData)),
				palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32))};
			sift_col_tids(&unit, &sel, u->tid);
			sift_col_sel_idx(&sel, u->row);
		}
	}
	g->cache_sb = sb;
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

int
sift_tbl_gscan_block(SiftTblGroupScan *g, BlockNumber vblock, const uint64 *offmask, HeapTuple *out)
{
	uint32		sb = vblock / SIFT_SB_PAGES;
	int			nout = 0;
	MemoryContext old;

	cache_superblock(g, sb);
	for (int k = 0; k < g->ncu; k++)
	{
		const UnitTids *u = &g->cu[k];
		uint32		first = block_start(u, vblock),
					last = first;
		SiftColSource *src = g->s[u->src].src;
		SiftColUnit unit;
		SiftColSel sel;
		uint32		n = 0;

		while (last < u->n && ItemPointerGetBlockNumberNoCheck(&u->tid[last]) == vblock)
			last++;
		if (first == last)
			continue;
		g->cur = u->src;
		sift_col_begin(src, NULL, 0, sb, sb + 1);
		for (uint32 j = 0; j <= u->ordinal; j++)
			if (!sift_col_next(src, &unit))
				elog(ERROR, "sift_table: the unit %u of superblock %u is gone", u->ordinal, sb);
		MemoryContextReset(g->unitcxt);
		old = MemoryContextSwitchTo(g->unitcxt);
		sift_col_sel_alloc(src, unit.nrows, &sel);
		MemoryContextSwitchTo(old);
		for (uint32 j = first; j < last; j++)
			if (offmask == NULL || sift_col_bit_test(offmask, ItemPointerGetOffsetNumberNoCheck(&u->tid[j]) - 1))
			{
				sift_col_bit_set(sel.bits, u->row[j]);
				n++;
			}
		if (n == 0)
			continue;
		sel.count = n;
		g->unit = unit;
		unit_read(g, &sel);
		for (uint32 j = first, p = 0; j < last; j++)
			if (sift_col_bit_test(sel.bits, u->row[j]))
				out[nout++] = row_tuple(g, p++, &u->tid[j]);
	}
	return nout;
}

/* A fetch scan serving the TID fetches while the directory and the xmin of the backend (which keeps the segments from being recycled) stay */
static SiftTblGroupScan *
fetch_scan(Relation rel, SiftTblRel *st)
{
	struct SiftTblGroupCache *c = st->gcache;

	if (c == NULL)
		c = st->gcache = MemoryContextAllocZero(st->cxt, sizeof(*c));
	if (c->fetch && (c->fetch_version != st->dir.version || c->fetch_xmin != TransactionXmin || !TransactionIdIsValid(TransactionXmin)))
	{
		sift_tbl_gscan_end(c->fetch);
		c->fetch = NULL;
	}
	if (c->fetch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(st->cxt);

		c->fetch = sift_tbl_gscan_begin(rel, RelationGetDescr(rel), st, &st->dir, NULL);
		c->fetch_version = st->dir.version;
		c->fetch_xmin = TransactionXmin;
		MemoryContextSwitchTo(old);
	}
	return c->fetch;
}

/* Is tid a live row of the groups? (the TIDs of its units, no values read) */
static bool
gscan_has(SiftTblGroupScan *g, ItemPointer tid)
{
	BlockNumber vblock = ItemPointerGetBlockNumberNoCheck(tid);

	cache_superblock(g, vblock / SIFT_SB_PAGES);
	for (int k = 0; k < g->ncu; k++)
		for (uint32 j = block_start(&g->cu[k], vblock); j < g->cu[k].n && ItemPointerGetBlockNumberNoCheck(&g->cu[k].tid[j]) == vblock; j++)
			if (ItemPointerEquals(&g->cu[k].tid[j], tid))
				return true;
	return false;
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
		*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.delta = false,.xmin = InvalidTransactionId};
		return has;
	}
	sift_col_bit_set(offmask, ItemPointerGetOffsetNumberNoCheck(tid) - 1);
	found = sift_tbl_gscan_block(g, ItemPointerGetBlockNumberNoCheck(tid), offmask, row);
	gscan_park(g);
	if (found == 0)
		return false;
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.delta = false,.xmin = InvalidTransactionId,.tuple = row[0]};
	return true;
}

/* ---------- sealing ---------- */

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

/* The page idx of seg into copy; false for a page of overflow bytes */
static bool
seg_page(Relation rel, SiftTblSeg *seg, uint32 idx, Page copy)
{
	Buffer		buf = sift_lane_read_locked(rel, sift_tbl_delta_page(rel, seg, idx), BUFFER_LOCK_SHARE);

	sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, seg->gen);
	memcpy(copy, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return !(TBL_PAGE(copy)->flags & SIFT_TBL_PAGE_OVFDATA);
}

/* Is every row of the segment settled? The newest rows are in the last pages, so a segment that is not final answers after one page */
static bool
seg_final(Relation rel, SiftTblSeg *seg, TransactionId oldest, bool vacuum)
{
	PGAlignedBlock copy;

	for (uint32 p = seg->npages; p-- > 0;)
	{
		if (!seg_page(rel, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);

			if (rec != NULL && row_fate(rec, oldest, vacuum) == FATE_WAIT)
				return false;
		}
	}
	return true;
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
			sw->w = sift_colw_begin(sw->wt, sift_store_new_gen(sw->rel), sw->l->schema, sw->policy, sw->cxt);
		}
		sift_colw_add(sw->w, sw->batch);
	}
	MemoryContextSwitchTo(old);
}

/* The rows of the final segment e as one column segment with the same TIDs; the delta pages are released with the switch */
static void
seal_segment(Relation rel, SiftTblRel *st, const SiftSegEntry *e, SiftTblSeg *seg, TransactionId oldest, bool vacuum)
{
	const SiftTblLayout *l = layout_of(rel, st);
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table seal", ALLOCSET_DEFAULT_SIZES);
	MemoryContext rowcx = AllocSetContextCreate(cx, "sift_table seal row", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SealWriter	sw = {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema)};
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
			.extra = InvalidBuffer});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

int
sift_tbl_seal(Relation rel, SiftTblRel *st, TransactionId oldest, bool vacuum)
{
	int			sealed = 0;
	SiftDirSnapshot *dir;

	if (!sift_maint_lock(rel, vacuum))
		return 0;
	sift_tbl_refresh(rel, st);
	dir = MemoryContextAlloc(CurrentMemoryContext, sizeof(SiftDirSnapshot));
	*dir = st->dir;
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		SiftTblSeg seg = {.gen = e->gen,.hdr = e->header_blk,.vbase = (uint64) e->sb_lo * SIFT_TBL_VBLOCKS_SB};

		if (e->kind != SIFT_SEG_MUTABLE_SEALED || !(e->flags & SIFT_TBL_SEG_DELTA))
			continue;
		sift_tbl_seg_load(rel, &seg);
		if (!seg_final(rel, &seg, oldest, vacuum))
			continue;
		seal_segment(rel, st, e, &seg, oldest, vacuum);
		sealed++;
	}
	sift_maint_unlock(rel);
	pfree(dir);
	if (sealed > 0)
		sift_tbl_refresh(rel, st);
	return sealed;
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

bool
sift_tbl_merge(Relation rel, SiftTblRel *st)
{
	const SiftTblLayout *l = layout_of(rel, st);
	SiftMergePolicy pol = {(uint64) sift_tbl_delta_limit * 1024, SIFT_MERGE_FANIN, sift_col_mostly_dead};
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			n;

	sift_tbl_refresh(rel, st);
	n = sift_merge_pick(rel, &pol, &st->dir, false, sel);
	for (int i = 0; i < n; i++)
		if (schema_of(rel, st, &sel[i])->hash != l->schema->hash)
			return false;
	if (n == 0)
		return false;
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), sel, n);
	sift_tbl_refresh(rel, st);
	return true;
}

/* ---------- sift.table_segments ---------- */

PG_FUNCTION_INFO_V1(sift_table_segments);

/* One row per segment of the table: delta (row format write buffers), sealed delta (waiting for sealing), columns, tombstones */
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
									   e->kind == SIFT_SEG_MUTABLE_ACTIVE ? "delta" : "sealed delta");
			memset(nulls + 2, true, 6);
			if (e->flags & SIFT_TBL_SEG_DELTA)
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
