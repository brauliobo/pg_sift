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
#include "port/pg_bitutils.h"
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

/* The superblocks and the version summary of the units of a group with unfrozen rows, read from its segment once (a segment is immutable) */
typedef struct VerGroup
{
	uint64		gen;
	uint32		nsb;
	uint32	   *sb;
	SiftColVerSum *sum;
} VerGroup;

struct SiftTblGroupCache
{
	int			n;
	SchemaEnt  *ent;
	int			cap;
	VerGroup   *vg;
	int			nvg;
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
	uint32	   *xmin;			/* the versions of every row of the unit by that index; NULL when none is unfrozen */
	uint32	   *cmin;
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
	SiftTblVers *vers;
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
	SiftTblVers *vers;			/* of the directory copy the scan reads */
	SiftColSegment *useg;		/* the segment and unit of the current unit, to read its versions */
	uint32		uu;
	SiftColVerSum usum;
	uint32	   *xm;				/* its versions by row ordinal, read when a row's version is asked for */
	uint32	   *cm;
	bool		hidden;			/* the (MVCC) snapshot sees no row of the current unit: it has none to return */
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

/* ---------- groups with unfrozen rows ---------- */

static const VerGroup *
ver_group(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	MemoryContext tmp;
	MemoryContext old;
	SiftColSegment *seg;
	VerGroup   *g;

	for (int i = 0; i < c->nvg; i++)
		if (c->vg[i].gen == e->gen)
			return &c->vg[i];
	tmp = AllocSetContextCreate(CurrentMemoryContext, "sift_table versions", ALLOCSET_SMALL_SIZES);
	seg = sift_cseg_open(rel, e, tmp);
	old = MemoryContextSwitchTo(st->cxt);
	c->vg = c->vg ? repalloc(c->vg, (c->nvg + 1) * sizeof(VerGroup)) : palloc((c->nvg + 1) * sizeof(VerGroup));
	g = &c->vg[c->nvg++];
	*g = (VerGroup) {.gen = e->gen,.nsb = seg->nsb,.sb = palloc(seg->nsb * sizeof(uint32)),.sum = palloc(seg->nsb * sizeof(SiftColVerSum))};
	MemoryContextSwitchTo(old);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		g->sb[u] = seg->sbdir[u].sb;
		sift_cseg_ver_sum(seg, u, &g->sum[u]);
	}
	sift_cseg_close(seg);
	MemoryContextDelete(tmp);
	return g;
}

/* Forgets the groups that neither directory has any more */
static void
ver_groups_prune(SiftTblRel *st, const SiftDirSnapshot *dir)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	int			n = 0;

	for (int i = 0; i < c->nvg; i++)
	{
		bool		used = false;

		for (int k = 0; k < dir->nsegs && !used; k++)
			used = dir->segs[k].gen == c->vg[i].gen;
		for (int k = 0; k < st->dir.nsegs && !used; k++)
			used = st->dir.segs[k].gen == c->vg[i].gen;
		if (used)
			c->vg[n++] = c->vg[i];
		else
		{
			pfree(c->vg[i].sb);
			pfree(c->vg[i].sum);
		}
	}
	c->nvg = n;
}

struct SiftTblVers
{
	Relation	rel;
	MemoryContext cxt;
	int			n;
	SiftSegEntry *ent;			/* [n] the groups with unfrozen rows of the directory copy */
	const VerGroup **grp;
	/* the unit whose versions were read last (a scan of a page at a time asks for the next page of the same unit) */
	int			seg_g;
	SiftColSegment *seg;
	int			cu_g;
	uint32		cu_u;
	uint32		cap;
	uint32	   *xm;
	uint32	   *cm;
};

SiftTblVers *
sift_tbl_vers_build(Relation rel, SiftTblRel *st, const SiftDirSnapshot *dir, MemoryContext cxt)
{
	SiftTblVers *v = MemoryContextAllocZero(cxt, sizeof(SiftTblVers));
	int			n = 0;

	for (int i = 0; i < dir->nsegs; i++)
		n += sift_tbl_seg_unfrozen(&dir->segs[i]);
	v->rel = rel;
	v->cxt = cxt;
	v->ent = MemoryContextAlloc(cxt, Max(n, 1) * sizeof(SiftSegEntry));
	v->grp = MemoryContextAlloc(cxt, Max(n, 1) * sizeof(VerGroup *));
	v->seg_g = v->cu_g = -1;
	for (int i = 0; i < dir->nsegs; i++)
		if (sift_tbl_seg_unfrozen(&dir->segs[i]))
		{
			const VerGroup *src = ver_group(rel, st, &dir->segs[i]);
			VerGroup   *g = MemoryContextAlloc(cxt, sizeof(VerGroup));

			/* a copy: the cache moves and forgets groups as the directory changes, and this one is of the directory it was built for */
			*g = *src;
			g->sb = MemoryContextAlloc(cxt, src->nsb * sizeof(uint32));
			g->sum = MemoryContextAlloc(cxt, src->nsb * sizeof(SiftColVerSum));
			memcpy(g->sb, src->sb, src->nsb * sizeof(uint32));
			memcpy(g->sum, src->sum, src->nsb * sizeof(SiftColVerSum));
			v->ent[v->n] = dir->segs[i];
			v->grp[v->n++] = g;
		}
	ver_groups_prune(st, dir);
	return v;
}

static int
cmp_u32(const void *a, const void *b)
{
	return pg_cmp_u32(*(const uint32 *) a, *(const uint32 *) b);
}

/* The group and unit that hold superblock sb (the superblocks of a group belong to it alone) */
static bool
vers_find(const SiftTblVers *v, uint32 sb, int *g, uint32 *u)
{
	for (int i = 0; v != NULL && i < v->n; i++)
	{
		const VerGroup *grp = v->grp[i];
		const uint32 *hit;

		if (sb < grp->sb[0] || sb > grp->sb[grp->nsb - 1])
			continue;
		if ((hit = bsearch(&sb, grp->sb, grp->nsb, sizeof(uint32), cmp_u32)) != NULL)
		{
			*g = i;
			*u = hit - grp->sb;
			return true;
		}
	}
	return false;
}

SiftTblVerState
sift_tbl_vers_sb(Relation rel, Snapshot snapshot, SiftTblVers *vers, uint32 sb)
{
	int			g;
	uint32		u;

	if (!vers_find(vers, sb, &g, &u))
		return SIFT_TBL_VER_ALL;
	return sift_tbl_ver_state(rel, snapshot, &vers->grp[g]->sum[u]);
}

/* The versions of the rows of unit u of group g, read once for as long as the unit is the one asked for */
static void
vers_unit(SiftTblVers *v, int g, uint32 u)
{
	if (v->cu_g == g && v->cu_u == u)
		return;
	if (v->seg_g != g)
	{
		if (v->seg)
			sift_cseg_close(v->seg);
		v->seg = sift_cseg_open(v->rel, &v->ent[g], v->cxt);
		v->seg_g = g;
	}
	if (v->seg->sbdir[u].nrows > v->cap)
	{
		v->cap = v->seg->sbdir[u].nrows;
		v->xm = v->xm ? repalloc(v->xm, v->cap * sizeof(uint32)) : MemoryContextAlloc(v->cxt, v->cap * sizeof(uint32));
		v->cm = v->cm ? repalloc(v->cm, v->cap * sizeof(uint32)) : MemoryContextAlloc(v->cxt, v->cap * sizeof(uint32));
	}
	sift_cseg_ver_unit(v->seg, u, v->xm, v->cm);
	sift_cseg_close(v->seg);
	v->cu_g = g;
	v->cu_u = u;
}

void
sift_tbl_vers_page(SiftTblVers *vers, BlockNumber vblock, SiftTblVerPage *vp)
{
	int			g;
	uint32		u;
	bool		found;

	memset(vp, 0, sizeof(*vp));
	if (!vers_find(vers, vblock / SIFT_SB_PAGES, &g, &u) || !vers->grp[g]->sum[u].any)
		return;
	vers_unit(vers, g, u);
	found = sift_cseg_page_rows(vers->seg, u, vblock, &vp->first, vp->offmask);
	sift_cseg_close(vers->seg);
	if (!found)
		return;
	vp->xmin = vers->xm;
	vp->cmin = vers->cm;
}

void
sift_tbl_vers_rowver(const SiftTblVerPage *vp, uint32 off, TransactionId *xmin, CommandId *cmin)
{
	uint32		rank = 0;

	*xmin = InvalidTransactionId;
	*cmin = 0;
	if (vp->xmin == NULL || !sift_col_bit_test(vp->offmask, off - 1))
		return;
	for (uint32 w = 0; w < (off - 1) / 64; w++)
		rank += pg_popcount64(vp->offmask[w]);
	if ((off - 1) % 64)
		rank += pg_popcount64(vp->offmask[(off - 1) / 64] & ((UINT64CONST(1) << ((off - 1) % 64)) - 1));
	*xmin = vp->xmin[vp->first + rank];
	*cmin = vp->cmin[vp->first + rank];
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
		sift_tbl_vers_sb(v->rel, v->snapshot, v->vers, sb) == SIFT_TBL_VER_ALL;

	memset(pagemask, plain ? 0xFF : 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

static int
vis_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	VisArg	   *v = arg;

	return sift_tbl_filter_page_vers(v->rel, v->snapshot, v->vers, blk, offmask);
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

	g->vers = sift_tbl_vers_build(rel, st, dir, cxt);
	*va = (VisArg) {rel, st, snapshot, g->vers};
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
	SiftTblVerState state = SIFT_TBL_VER_ALL;
	MemoryContext old;

	g->rd_unit = -1;
	g->token++;
	MemoryContextReset(g->unitcxt);
	g->xm = g->cm = NULL;
	memset(&g->usum, 0, sizeof(g->usum));
	if (sift_col_unit_ref(&g->unit, &g->useg, &g->uu))
	{
		sift_cseg_ver_sum(g->useg, g->uu, &g->usum);
		state = sift_tbl_ver_state(g->rel, g->snapshot, &g->usum);
	}
	g->hidden = state == SIFT_TBL_VER_NONE;
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
	g->plain = !sift_tbl_tomb_any(g->st, lo, lo + SB_KEYS - 1) && state == SIFT_TBL_VER_ALL;
}

/* The version of the row i of the selection: a unit with unfrozen rows that a snapshot does not see whole reads its vectors on the first ask */
static void
gscan_rowver(SiftTblGroupScan *g, uint32 i, SiftTblRowVer *rv)
{
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(&g->tids[i]),.wbuf = false,.xmin = InvalidTransactionId};
	if (!g->usum.any || g->plain)
		return;
	if (g->xm == NULL)
	{
		uint32		nrows = g->useg->sbdir[g->uu].nrows;
		MemoryContext old = MemoryContextSwitchTo(g->unitcxt);

		g->xm = palloc(nrows * sizeof(uint32));
		g->cm = palloc(nrows * sizeof(uint32));
		MemoryContextSwitchTo(old);
		sift_cseg_ver_unit(g->useg, g->uu, g->xm, g->cm);
		sift_cseg_close(g->useg);
	}
	rv->xmin = g->xm[g->ord[i]];
	rv->cmin = g->cm[g->ord[i]];
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
			gscan_rowver(g, g->i - 1, rv);
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
	gscan_rowver(g, g->i, rv);
	g->i++;
	return true;
}

bool
sift_tbl_gscan_plain(const SiftTblGroupScan *g)
{
	return g->plain;
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
			if (g->cu[k].xmin)
			{
				pfree(g->cu[k].xmin);
				pfree(g->cu[k].cmin);
			}
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
			SiftColSegment *seg;
			uint32		useg;
			SiftColVerSum sum;

			if (g->ncu == cap)
				g->cu = repalloc(g->cu, (cap *= 2) * sizeof(UnitTids));
			u = &g->cu[g->ncu++];
			sift_col_sel_all(&unit, &sel);
			*u = (UnitTids) {s, ordinal++, sel.count, palloc(Max(sel.count, 1) * sizeof(ItemPointerData)), palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32))};
			sift_col_tids(&unit, &sel, u->tid);
			sift_col_sel_idx(&sel, u->ord);
			if (sift_col_unit_ref(&unit, &seg, &useg))
			{
				sift_cseg_ver_sum(seg, useg, &sum);
				if (sum.any)
				{
					u->xmin = palloc(seg->sbdir[useg].nrows * sizeof(uint32));
					u->cmin = palloc(seg->sbdir[useg].nrows * sizeof(uint32));
					sift_cseg_ver_unit(seg, useg, u->xmin, u->cmin);
					sift_cseg_close(seg);
				}
			}
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
sift_tbl_gscan_block(SiftTblGroupScan *g, BlockNumber vblock, const uint64 *offmask, HeapTuple *out, TransactionId *xmin, CommandId *cmin)
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
				if (xmin)
				{
					xmin[nout] = u->xmin ? u->xmin[u->ord[j]] : InvalidTransactionId;
					cmin[nout] = u->xmin ? u->cmin[u->ord[j]] : 0;
				}
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
	TransactionId xmin[SIFT_TBL_MAX_OFF];
	CommandId	cmin[SIFT_TBL_MAX_OFF];
	int			found;

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta))
		return false;
	g = fetch_scan(rel, st);
	if (lazy)
	{
		int			k;
		uint32		j;
		bool		has = gscan_find(g, tid, &k, &j);

		*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = false};
		if (has && g->cu[k].xmin)
		{
			rv->xmin = g->cu[k].xmin[g->cu[k].ord[j]];
			rv->cmin = g->cu[k].cmin[g->cu[k].ord[j]];
		}
		gscan_park(g);
		return has;
	}
	sift_col_bit_set(offmask, ItemPointerGetOffsetNumberNoCheck(tid) - 1);
	found = sift_tbl_gscan_block(g, ItemPointerGetBlockNumberNoCheck(tid), offmask, row, xmin, cmin);
	gscan_park(g);
	if (found == 0)
		return false;
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = false,.xmin = xmin[0],.cmin = cmin[0],.tuple = row[0]};
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
	SEG_WAIT,					/* some row is not final and the segment cannot carry it (nor is the directory filling) */
	SEG_FINAL,					/* every row is final: a frozen group */
	SEG_UNFROZEN				/* some row is not: the group keeps the xmin and cmin of every row that is not final */
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
 * What the rows of the segment allow. A segment whose rows are not all final becomes a group with unfrozen rows only when pressure says the
 * directory is filling, whatever the mix of writers, commands and subtransactions is; a row VACUUM flagged DEAD (it drops them) or a speculative
 * insertion in progress keeps it waiting. The newest rows are in the last pages, so a segment that cannot be promoted answers after one page.
 */
static SegClass
seg_class(Relation rel, SiftTblSeg *seg, TransactionId oldest, bool vacuum, bool pressure)
{
	PGAlignedBlock copy;
	FateMemo	memo = {0};
	bool		unfrozen = false;

	for (uint32 p = seg->npages; p-- > 0;)
	{
		if (!seg_page(rel, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);

			if (rec == NULL || fate_memo(&memo, rec, oldest, vacuum) != FATE_WAIT)
				continue;
			if (!pressure || (rec->flags & (SIFT_TBL_REC_DEAD | SIFT_TBL_REC_SPEC)))
				return SEG_WAIT;
			unfrozen = true;
		}
	}
	return unfrozen ? SEG_UNFROZEN : SEG_FINAL;
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
		}
		sift_colw_add(sw->w, sw->batch);
	}
	MemoryContextSwitchTo(old);
}

/*
 * The rows of the segment e as one column segment with the same TIDs: a row that is final is frozen, the others keep their xmin and cmin
 * (the group then has unfrozen rows); the write buffer pages are released with the switch
 */
static void
seal_segment(Relation rel, SiftTblRel *st, const SiftSegEntry *e, SiftTblSeg *seg, TransactionId oldest, bool vacuum)
{
	const SiftTblLayout *l = layout_of(rel, st);
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext rowcx = AllocSetContextCreate(cx, "sift_table promote row", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SealWriter	sw = {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL, (uint64) seg->npages * BLCKSZ};
	FateMemo	memo = {0};
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
			Fate		fate;

			if (rec == NULL || (fate = fate_memo(&memo, rec, oldest, vacuum)) == FATE_DROP)
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
			sift_cbatch_add_ver(sw.batch, &row, fate == FATE_KEEP ? InvalidTransactionId : rec->xmin, rec->cmin);
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

		if (e->kind != SIFT_SEG_WBUF_SEALED || !(e->flags & SIFT_TBL_SEG_WBUF))
			continue;
		sift_tbl_seg_load(rel, &seg);
		if (seg_class(rel, &seg, oldest, vacuum, pressure) == SEG_WAIT)
			continue;
		seal_segment(rel, st, e, &seg, oldest, vacuum);
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

/* ---------- freezing, the rows of aborted writers, the live rows ---------- */

/* An insertion that is final for every snapshot: committed before oldest (the last answer is remembered) */
typedef struct XidFinal
{
	TransactionId oldest;
	TransactionId xid;
	bool		final;
} XidFinal;

static bool
xid_final(XidFinal *m, TransactionId xid)
{
	if (m->xid != xid || !TransactionIdIsValid(m->xid))
	{
		m->xid = xid;
		m->final = TransactionIdPrecedes(xid, m->oldest) && TransactionIdDidCommit(xid);
	}
	return m->final;
}

static bool
merge_freeze(void *arg, uint32 xmin, uint32 cmin)
{
	return xid_final(arg, xmin);
}

/* Does the unit have a live row (a group scan reads the liveness bits only of a unit with a dead one) */
static bool
unit_has_live(SiftColSegment *seg, uint32 u, uint64 *live)
{
	if (!sift_cseg_dirty(seg, u))
		return true;
	sift_cseg_live(seg, u, live);
	for (uint32 w = 0; w < SIFT_COL_NWORDS(seg->sbdir[u].nrows); w++)
		if (live[w])
			return true;
	return false;
}

/* Are all the live rows of the group committed before oldest (a group with a newer xid is not read further than its range) */
static bool
group_final(Relation rel, const SiftSegEntry *e, TransactionId oldest)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table freeze check", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSegment *seg = sift_cseg_open(rel, e, cx);
	XidFinal	m = {.oldest = oldest};
	uint32	   *xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32)),
			   *cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	uint32		lo,
				hi;
	bool		final;

	sift_cseg_ver_range(seg, &lo, &hi);
	final = TransactionIdPrecedes(hi, oldest);
	for (uint32 u = 0; u < seg->nsb && final; u++)
	{
		SiftColVerSum sum;

		sift_cseg_ver_sum(seg, u, &sum);
		if (!sum.any || !unit_has_live(seg, u, live))
			continue;
		if (!sum.frozen_rows && sum.xmin_lo == sum.xmin_hi)
		{
			final = xid_final(&m, sum.xmin_lo);
			continue;
		}
		sift_cseg_ver_unit(seg, u, xm, cm);
		sift_cseg_live(seg, u, live);
		for (uint32 i = 0; i < seg->sbdir[u].nrows && final; i++)
			if (xm[i] != 0 && (live[i / 64] >> (i % 64) & 1))
				final = xid_final(&m, xm[i]);
		CHECK_FOR_INTERRUPTS();
	}
	sift_cseg_close(seg);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return final;
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
			{
				if (dir->segs[i].format < SIFT_COL_FORMAT_VERS)
					dir->segs[i].reserved = 0;
				dir->segs[i].flags &= ~SIFT_SEG_VERS;
			}
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

		if (sift_tbl_seg_unfrozen(e) && group_final(rel, e, oldest))
			gens[n++] = e->gen;
	}
	if (n == 0)
		return 0;
	sift_dir_update(rel, &(SiftDirChange) {.fn = freeze_groups_swap,.arg = &(FreezeArg) {gens, n},.extra = InvalidBuffer});
	sift_tbl_refresh(rel, st);
	return n;
}

static void
runs_add(SiftTblKeyRuns *r, SiftKey lo, SiftKey hi)
{
	if (r->n == r->cap)
	{
		r->cap = Max(64, r->cap * 2);
		r->run = r->run ? repalloc_huge(r->run, r->cap * sizeof(SiftKey[2])) : MemoryContextAlloc(r->mcxt, r->cap * sizeof(SiftKey[2]));
	}
	r->run[r->n][0] = lo;
	r->run[r->n++][1] = hi;
}

/* The live rows of unit u whose writer aborted, as runs of consecutive rows; false when it has none */
static bool
unit_aborted_runs(SiftColSegment *seg, uint32 u, SiftTblKeyRuns *runs, uint32 *xm, uint32 *cm, uint64 *live, SiftKey *keys)
{
	uint32		n = seg->sbdir[u].nrows;
	TransactionId memo_xid = InvalidTransactionId;
	bool		memo_aborted = false,
				any = false,
				open = false;
	SiftKey		lo = 0,
				hi = 0;

	sift_cseg_ver_unit(seg, u, xm, cm);
	sift_cseg_live(seg, u, live);
	for (uint32 i = 0; i < n; i++)
	{
		bool		aborted;

		if (!(live[i / 64] >> (i % 64) & 1))
			continue;
		if (xm[i] != memo_xid)
		{
			memo_xid = xm[i];
			memo_aborted = TransactionIdIsNormal(xm[i]) && sift_tbl_xid_aborted(xm[i]);
		}
		aborted = memo_aborted;
		if (aborted && !any)
		{
			sift_cseg_unit_keys(seg, u, keys);
			any = true;
		}
		if (aborted)
		{
			if (!open)
				lo = keys[i];
			hi = keys[i];
			open = true;
		}
		else if (open)
		{
			runs_add(runs, lo, hi);
			open = false;
		}
	}
	if (open)
		runs_add(runs, lo, hi);
	return any;
}

void
sift_tbl_aborted_runs(Relation rel, SiftTblRel *st, SiftTblKeyRuns *runs, uint32 *sb_lo, uint32 *sb_hi)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table aborted rows", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint32	   *xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32)),
			   *cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));

	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftColSegment *seg;

		if (!sift_tbl_seg_unfrozen(e))
			continue;
		seg = sift_cseg_open(rel, e, cx);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftColVerSum sum;

			sift_cseg_ver_sum(seg, u, &sum);
			if (!sum.any)
				continue;
			if (unit_aborted_runs(seg, u, runs, xm, cm, live, keys))
			{
				*sb_lo = Min(*sb_lo, seg->sbdir[u].sb);
				*sb_hi = Max(*sb_hi, seg->sbdir[u].sb);
			}
			CHECK_FOR_INTERRUPTS();
		}
		sift_cseg_close(seg);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

int
sift_tbl_drop_dead_groups(Relation rel, SiftTblRel *st)
{
	SiftSegEntry gone[SIFT_MAX_SEGMENTS];
	int			n = 0;
	SiftMaintInputs mi;

	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
		if (sift_tbl_seg_unfrozen(&st->dir.segs[i]))
		{
			SiftColSegment *seg = sift_cseg_open(rel, &st->dir.segs[i], CurrentMemoryContext);

			if (sift_cseg_live_rows(seg) == 0)
				gone[n++] = st->dir.segs[i];
			sift_cseg_close(seg);
		}
	if (n == 0)
		return 0;
	mi = sift_maint_inputs(rel, gone, n);
	sift_maint_replace(rel, mi.gens, n, NULL, mi.release, mi.nrelease, psprintf("dropped %d column segments without a live row of", n), GetCurrentTimestamp(), 0);
	sift_tbl_refresh(rel, st);
	return n;
}

/* The live rows of a group with unfrozen rows that count: inserted by a transaction that committed or is this one */
static double
unfrozen_live_rows(Relation rel, const SiftSegEntry *e)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table live rows", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSegment *seg = sift_cseg_open(rel, e, cx);
	uint32	   *xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32)),
			   *cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	double		n = 0;
	TransactionId memo_xid = InvalidTransactionId;
	bool		memo_counts = true;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftColVerSum sum;
		uint32		rows = seg->sbdir[u].nrows;

		sift_cseg_ver_sum(seg, u, &sum);
		sift_cseg_live(seg, u, live);
		if (sum.any)
			sift_cseg_ver_unit(seg, u, xm, cm);
		for (uint32 i = 0; i < rows; i++)
		{
			if (!(live[i / 64] >> (i % 64) & 1))
				continue;
			if (sum.any && xm[i] != memo_xid)
			{
				memo_xid = xm[i];
				memo_counts = !TransactionIdIsNormal(xm[i]) || TransactionIdIsCurrentTransactionId(xm[i]) || TransactionIdDidCommit(xm[i]);
			}
			n += !sum.any || memo_counts;
		}
		CHECK_FOR_INTERRUPTS();
	}
	sift_cseg_close(seg);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return n;
}

double
sift_tbl_group_live_rows(Relation rel, SiftTblRel *st)
{
	double		n = 0;

	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		if (sift_tbl_seg_unfrozen(e))
			n += unfrozen_live_rows(rel, e);
		else
		{
			SiftColSegment *seg = sift_cseg_open(rel, e, CurrentMemoryContext);

			n += sift_cseg_live_rows(seg);
			sift_cseg_close(seg);
		}
	}
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
	for (int i = 0; i < SETTLE_MERGE_STEPS && dir_pressure(st) && sift_tbl_merge(rel, st, oldest); i++)
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

/* The smallest groups when the directory is filling and the policy waits for more of a size (the least a merge frees: fanin - 1 entries) */
static int
merge_smallest(Relation rel, SiftTblRel *st, SiftSegEntry *sel)
{
	const SiftTblLayout *l = layout_of(rel, st);
	SiftSegEntry *cand = palloc(Max(st->dir.nsegs, 1) * sizeof(SiftSegEntry));
	int			n = 0;

	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE && schema_of(rel, st, &st->dir.segs[i])->hash == l->schema->hash)
			cand[n++] = st->dir.segs[i];
	if (n >= 2)
	{
		qsort(cand, n, sizeof(SiftSegEntry), sift_seg_bytes_cmp);
		n = Min(n, SIFT_MERGE_FANIN);
		memcpy(sel, cand, n * sizeof(SiftSegEntry));
	}
	else
		n = 0;
	pfree(cand);
	return n;
}

/* The next merge: segments of the same tier, else the smallest ones when the directory has too many */
static int
merge_pick(Relation rel, SiftTblRel *st, SiftSegEntry *sel)
{
	SiftMergePolicy pol = {(uint64) sift_tbl_write_buffer_limit * 1024, SIFT_MERGE_FANIN, sift_tbl_mostly_dead};
	int			n = sift_merge_pick(rel, &pol, &st->dir, false, sel);

	return n > 0 || !dir_pressure(st) ? n : merge_smallest(rel, st, sel);
}

bool
sift_tbl_merge(Relation rel, SiftTblRel *st, TransactionId oldest)
{
	const SiftTblLayout *l = layout_of(rel, st);
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	XidFinal	fin = {.oldest = oldest};
	int			n;

	sift_tbl_refresh(rel, st);
	n = merge_pick(rel, st, sel);
	for (int i = 0; i < n; i++)
		if (schema_of(rel, st, &sel[i])->hash != l->schema->hash)
			return false;
	if (n == 0)
		return false;
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), NULL, sel, n, merge_freeze, &fin);
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
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), NULL, e, 1, NULL, NULL);
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
		/* columns 9 to 11 (unfrozen, min_xid, max_xid) are there when the SQL function declares them (pg_sift--3.0--3.1.sql); more values than columns are ignored */
		Datum		v[11] = {0};
		bool		nulls[11] = {0};

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
			v[8] = BoolGetDatum(sift_tbl_seg_unfrozen(e));
			if (sift_tbl_seg_unfrozen(e))
			{
				uint32		lo,
							hi;

				sift_cseg_ver_range(seg, &lo, &hi);
				v[9] = TransactionIdGetDatum(lo);
				v[10] = TransactionIdGetDatum(hi);
			}
			nulls[9] = nulls[10] = !sift_tbl_seg_unfrozen(e);
			sift_cseg_close(seg);
		}
		else
		{
			SiftTblSeg seg = {.gen = e->gen,.hdr = e->header_blk};

			v[1] = CStringGetTextDatum(e->flags & SIFT_TBL_SEG_TOMB ? "tombstones" :
									   e->kind == SIFT_SEG_WBUF_OPEN ? "buffer" : "sealed buffer");
			memset(nulls + 2, true, 9);
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

/* The column groups of the table with unfrozen rows (rows that are not final) */
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
		n += sift_tbl_seg_unfrozen(&st->dir.segs[i]);
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

	b->sw = (SealWriter) {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL, 0};
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
			SiftTblRowVer rv = {.key = in->keys[r],.wbuf = false,.xmin = in->hasver ? in->xmin[r] : InvalidTransactionId,.cmin = in->hasver ? in->cmin[r] : 0};

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
