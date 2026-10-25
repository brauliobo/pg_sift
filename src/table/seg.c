/*
 * table/seg.c - the column groups of a sift_table: the glue to the column engine (docs/COL-TAM.md sections 6, 7 and 12).
 *
 * A sealed write buffer becomes a column segment of the engine (col/seg.h) with the same TIDs: one leaf per column (typed when
 * the engine stores the type natively, SIFT_COLS_VAR holding the exact datum image otherwise) and one VAR leaf of exceptions, the
 * exact images of the values a typed leaf cannot hold (a numeric NaN or infinity, a NaN payload of a float that the order
 * preserving key would alter). Rows of a group have no xmin: they are frozen; their deletion is the tombstone log's, by TID.
 * Reads go through the engine's column source (sift_col_open_table); a segment written for another column layout (after ALTER TABLE)
 * is read with its own schema, the columns being matched by attribute number.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "commands/event_trigger.h"
#include "nodes/parsenodes.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/skey.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/uuid.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/lane.h"
#include "core/limits.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"

static SiftColStore
att_store(Form_pg_attribute att, uint8 *scale)
{
	Oid			typid = getBaseType(att->atttypid);
	SiftColStore store;

	*scale = 0;
	if (typid == NUMERICOID)
		return sift_col_numeric_scale(att, scale) ? SIFT_COLS_INT : SIFT_COLS_VAR;
	if (typid == BPCHAROID || typid == UUIDOID)	/* a uuid is a dictionary of 16-byte images: its top-N reads the codes (set_raw) */
		return SIFT_COLS_STR;
	if (sift_col_leaf_store(typid, &store))
		return store;
	return att->attbyval && (att->attlen == 1 || att->attlen == 2 || att->attlen == 4 || att->attlen == 8) ? SIFT_COLS_INT : SIFT_COLS_VAR;
}

/*
 * The paths declared for the table's jsonb columns (sift.table_paths: one row per column, by name, the grammar of the sift_col `paths` option), joined; NULL
 * when there are none or the extension has no such table yet. skip: a column whose row is replaced by `extra` (the check of a new declaration).
 */
static char *
declared_paths(Relation rel, AttrNumber skip, const char *extra)
{
	Oid			nsp = get_namespace_oid("sift", true);
	Oid			reloid = OidIsValid(nsp) ? get_relname_relid("table_paths", nsp) : InvalidOid;
	StringInfoData spec;
	Relation	pr;
	Snapshot	snap;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;
	char	   *specs[SIFT_COL_MAX_COLS] = {NULL};
	TupleDesc	desc = RelationGetDescr(rel);

	initStringInfo(&spec);
	if (OidIsValid(reloid))
	{
		pr = table_open(reloid, AccessShareLock);
		snap = RegisterSnapshot(GetTransactionSnapshot());
		ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
		scan = systable_beginscan(pr, InvalidOid, false, snap, 1, &key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			bool		isnull;
			Name		name = DatumGetName(heap_getattr(tup, 2, RelationGetDescr(pr), &isnull));

			/* a column renamed, dropped or no longer jsonb since the declaration has no paths */
			for (int a = 1; a <= Min(desc->natts, SIFT_COL_MAX_COLS); a++)
			{
				Form_pg_attribute att = TupleDescAttr(desc, a - 1);

				if (a != skip && !att->attisdropped && att->atttypid == JSONBOID && namestrcmp(name, NameStr(att->attname)) == 0)
					specs[a - 1] = TextDatumGetCString(heap_getattr(tup, 3, RelationGetDescr(pr), &isnull));
			}
		}
		systable_endscan(scan);
		UnregisterSnapshot(snap);
		table_close(pr, AccessShareLock);
	}
	if (extra && extra[0])
		specs[skip - 1] = (char *) extra;
	for (int a = 0; a < SIFT_COL_MAX_COLS; a++)
		if (specs[a] && specs[a][0])
			appendStringInfo(&spec, "%s%s", spec.len ? ", " : "", specs[a]);
	return spec.len ? spec.data : NULL;
}

static SiftTblLayout *
layout_build_for(Relation rel, MemoryContext cxt, const char *paths)
{
	TupleDesc	desc = RelationGetDescr(rel);
	SiftTblLayout *l = MemoryContextAllocZero(cxt, sizeof(SiftTblLayout));
	int			npath = 0;
	SiftColDef *defs;
	int			n = 0;

	l->natts = desc->natts;
	l->leaf = MemoryContextAlloc(cxt, (desc->natts + 1) * sizeof(int));
	l->first = MemoryContextAllocZero(cxt, (desc->natts + 1) * sizeof(int));
	if (paths)
		l->shred = sift_col_shred_create_for(cxt, desc, desc->natts, paths);
	for (int a = 1; l->shred && a <= desc->natts; a++)
		npath += sift_col_shred_nleaves(l->shred, a);
	defs = palloc0((desc->natts + npath + 1) * sizeof(SiftColDef));
	for (int a = 1; a <= desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);
		uint8		scale;
		uint8		type;
		SiftColStore store;

		l->leaf[a] = -1;
		if (att->attisdropped)
			continue;
		store = att_store(att, &scale);
		type = sift_col_type_of_oid(getBaseType(att->atttypid));
		l->leaf[a] = n;
		/* the tags of the index's own leaves (lanes, money, interval, time, enum, arrays, ranges) mean nothing to the table's layout, which
		 * stores these types by size and stays a minor 3 or 4 group */
		defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = store,.attno = a,.type = type,.path = "",.scale = scale};
		if (sift_col_def_needs_wide(&defs[n]))
			defs[n].type = SIFT_COLT_OTHER;
		n++;
	}
	for (int a = 1; l->shred && a <= desc->natts; a++)
	{
		int			k = sift_col_shred_nleaves(l->shred, a);

		l->first[a] = n;
		for (int i = 0; i < k; i++)
		{
			SiftColDef *d = &defs[n + i];

			sift_col_shred_leaf(l->shred, a, i, d);
			d->id = n + i;
			d->attno = a;
			d->parent = d->parent < 0 ? -1 : n + d->parent;
		}
		n += k;
	}
	l->exc = n;
	defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_VAR,.attno = 0,.type = SIFT_COLT_BYTEA,.path = "exceptions"};
	n++;
	if (n > sift_limit(col_leaves, SIFT_COL_MAX_COLS))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a sift_table holds at most %d columns", (int) sift_limit(col_leaves, SIFT_COL_MAX_COLS) - 1),
				 errdetail("Table \"%s\" has %d%s.", RelationGetRelationName(rel), n - 1 - npath, npath ? " and its declared paths hold the rest" : "")));
	l->schema = sift_cschema_create(cxt, defs, n);
	pfree(defs);
	return l;
}

static SiftTblLayout *
layout_build(Relation rel, MemoryContext cxt)
{
	return layout_build_for(rel, cxt, declared_paths(rel, 0, NULL));
}

/* The compression policy of each leaf under sift.table_compression now (a setting of the backend that writes the segment) */
SiftCcPolicy *
layout_policy(Relation rel, const SiftTblLayout *l)
{
	SiftCcPolicy *pol = palloc(l->schema->ncols * sizeof(SiftCcPolicy));

	for (int i = 0; i < l->schema->ncols; i++)
	{
		const SiftColDef *def = &l->schema->cols[i];
		const char *name = i == l->exc ? "exceptions" : def->path[0] ? def->path : NameStr(TupleDescAttr(RelationGetDescr(rel), def->attno - 1)->attname);
		const char *err = sift_cc_policy_for_column(sift_tbl_compression_spec, name, &pol[i]);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid compression option: %s", err)));
	}
	return pol;
}

const SiftTblLayout *
layout_of(Relation rel, SiftTblRel *st)
{
	if (st->layout == NULL)
		st->layout = layout_build(rel, st->cxt);
	return st->layout;
}

void
sift_tbl_layout_adopt(Relation rel, SiftTblRel *st, Relation from)
{
	st->layout = layout_build_for(rel, st->cxt, declared_paths(from, 0, NULL));
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

PG_FUNCTION_INFO_V1(sift_table_ddl_check);

/*
 * The event trigger after ALTER TABLE: a column added, a column's type changed or a column renamed (declared paths follow the name of a
 * jsonb column) fails with the message of CREATE TABLE when the sift_table, or a partition of it, would hold more leaves than a segment,
 * instead of at its next write. DROP COLUMN is not checked, so that a table made too wide by 3.2 can be narrowed.
 *
 * Before CREATE TRIGGER: pg_repack's first step on a table is the trigger repack.repack_trigger. It copies the rows into a heap table and
 * swaps the files, which would replace the column segments of a sift_table with heap pages and lose the table, so the trigger is refused.
 */
Datum
sift_table_ddl_check(PG_FUNCTION_ARGS)
{
	Node	   *stmt;
	RangeVar   *rv = NULL;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED), errmsg("not fired by the event trigger manager")));
	stmt = ((EventTriggerData *) fcinfo->context)->parsetree;
	if (IsA(stmt, AlterTableStmt))
	{
		foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) stmt)->cmds)
			if (cmd->subtype == AT_AddColumn || cmd->subtype == AT_AlterColumnType)
				rv = ((AlterTableStmt *) stmt)->relation;
	}
	else if (IsA(stmt, RenameStmt) && ((RenameStmt *) stmt)->renameType == OBJECT_COLUMN)
		rv = ((RenameStmt *) stmt)->relation;
	else if (IsA(stmt, CreateTrigStmt))
	{
		CreateTrigStmt *ts = (CreateTrigStmt *) stmt;

		if (list_length(ts->funcname) > 0 && strcmp(strVal(llast(ts->funcname)), "repack_trigger") == 0)
		{
			Oid			relid = RangeVarGetRelid(ts->relation, AccessShareLock, true);
			Relation	rel = OidIsValid(relid) ? relation_open(relid, NoLock) : NULL;

			if (rel && sift_tbl_is_table(rel))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("pg_repack cannot rewrite the sift_table \"%s\"", RelationGetRelationName(rel)),
								errdetail("pg_repack copies the rows into a heap table and swaps the files, which would destroy the column segments."),
								errhint("Use CLUSTER ... USING <index> or VACUUM FULL, which rewrite a sift_table; pg_repack --only-indexes is fine.")));
			if (rel)
				relation_close(rel, NoLock);
		}
	}
	if (rv)
	{
		Oid			relid = RangeVarGetRelid(rv, NoLock, true);

		/* the statement holds the lock of each table it altered */
		if (OidIsValid(relid))
			foreach_oid(oid, find_all_inheritors(relid, NoLock, NULL))
			{
				Relation	rel = relation_open(oid, NoLock);

				if (sift_tbl_is_table(rel))
					sift_tbl_layout_check(rel);
				relation_close(rel, NoLock);
			}
	}
	PG_RETURN_NULL();
}

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

Datum
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
			if (att->atttypid == UUIDOID)
			{
				v->v.s = DatumGetPointer(d);
				v->len = UUID_LEN;
				break;
			}
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
		v->tag = SIFT_COLTAG_UNKNOWN;
		exc_add(exc, att->attnum, p, v->len);
	}
}

Datum
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
			if (att->atttypid == UUIDOID)
				return image_datum(att, SIFT_COL_DICT_STR(vec->dict, vec->codes[i]), UUID_LEN);
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

/* The datum of the non-NULL row i of the vector of a leaf, as the table's column holds it */
Datum
sift_tbl_leaf_datum(Relation rel, const SiftColDef *def, const SiftColVec *vec, uint32 i)
{
	return value_out(def, TupleDescAttr(RelationGetDescr(rel), def->attno - 1), vec, i);
}

/* The leaves of a row (values/isnull as heap_deform_tuple gives them) */
void
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
	if (l->shred)
		sift_col_shred_row(l->shred, values, isnull, row, l->first, CurrentMemoryContext);
	if (exc.len == 0)
		row->val[l->exc].isnull = true;
	else
	{
		row->val[l->exc].v.s = exc.data;
		row->val[l->exc].len = exc.len;
	}
}

SiftColSchema *
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

PG_FUNCTION_INFO_V1(sift_table_paths_check);

/*
 * sift.table_paths_check(tbl, col, spec): the declaration of paths for a jsonb column is one the table can hold (the grammar of the sift_col `paths` option, which
 * names only that column; at most the leaves of the engine with the table's own), and the table's layout is rebuilt. The caller stores it in sift.table_paths.
 */
Datum
sift_table_paths_check(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessExclusiveLock);
	const char *col = NameStr(*PG_GETARG_NAME(1));
	char	   *spec = PG_ARGISNULL(2) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(2));
	AttrNumber	attno = get_attnum(RelationGetRelid(rel), col);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table paths check", ALLOCSET_SMALL_SIZES);

	if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetOuterUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
	sift_tbl_require(rel);
	if (attno <= 0 || TupleDescAttr(RelationGetDescr(rel), attno - 1)->atttypid != JSONBOID)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("\"%s\" is not a jsonb column of \"%s\"", col, RelationGetRelationName(rel))));
	if (spec && spec[0])
	{
		SiftColShredAdapter *a = sift_col_shred_create_for(cx, RelationGetDescr(rel), RelationGetDescr(rel)->natts, spec);

		for (int i = 1; i <= RelationGetDescr(rel)->natts; i++)
			if ((sift_col_shred_nleaves(a, i) > 0) != (i == attno))
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("the paths of \"%s\" must all start with that column's name", col)));
		(void) layout_build_for(rel, cx, declared_paths(rel, attno, spec));
	}
	MemoryContextDelete(cx);
	CacheInvalidateRelcache(rel);
	table_close(rel, NoLock);
	PG_RETURN_VOID();
}
