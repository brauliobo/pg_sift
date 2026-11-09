/*
 * col/conn_decl.c - the option connect of a child sift_col index (col/conn.h, docs/COL-CONNECT.md):
 *
 *     connect = 'fk -> [schema.]parent(key)'
 *
 * The text is the record of the connection: it is in pg_class.reloptions, so pg_dump writes it and a restore or REINDEX resolves it again. Resolving finds the sift_col index of
 * the parent table that tracks the key, makes the child index depend on it and the parent index keep its key journal.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/conn.h"

static const char *
skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	return p;
}

/* A plain name (folded to lower case) or a double-quoted one; NULL with *err set when there is none here */
static const char *
read_name(const char *p, char **out, const char **err)
{
	StringInfoData s;

	initStringInfo(&s);
	if (*p == '"')
	{
		for (p++; *p != '\0'; p++)
		{
			if (*p == '"' && p[1] == '"')
				p++;
			else if (*p == '"')
				break;
			appendStringInfoChar(&s, *p);
		}
		if (*p != '"' || s.len == 0)
		{
			*err = "a quoted name is not closed or is empty";
			return NULL;
		}
		p++;
	}
	else
	{
		if (!(isalpha((unsigned char) *p) || *p == '_' || IS_HIGHBIT_SET(*p)))
		{
			*err = "a name is expected here";
			return NULL;
		}
		for (; isalnum((unsigned char) *p) || *p == '_' || *p == '$' || IS_HIGHBIT_SET(*p); p++)
			appendStringInfoChar(&s, IS_HIGHBIT_SET(*p) ? *p : pg_tolower((unsigned char) *p));
	}
	*out = s.data;
	return p;
}

/* One connection, from *pp to its end (a ';' or the end of the text); NULL with *err set when it does not parse */
static SiftConnSpec *
parse_one(const char **pp, const char **err)
{
	SiftConnSpec *s = palloc0(sizeof(SiftConnSpec));
	const char *p = skip_ws(*pp);
	char	   *name;

	if ((p = read_name(p, &s->fk, err)) == NULL)
		return NULL;
	p = skip_ws(p);
	if (strncmp(p, "->", 2) != 0)
	{
		*err = "expected '->' after the column of this index that holds the parent key";
		return NULL;
	}
	p = skip_ws(p + 2);
	if ((p = read_name(p, &name, err)) == NULL)
		return NULL;
	if (*p == '.')
	{
		s->parent_schema = name;
		if ((p = read_name(p + 1, &s->parent_table, err)) == NULL)
			return NULL;
	}
	else
		s->parent_table = name;
	p = skip_ws(p);
	if (*p != '(')
	{
		*err = "expected '(' and the key column of the parent table after its name";
		return NULL;
	}
	if ((p = read_name(skip_ws(p + 1), &s->parent_key, err)) == NULL)
		return NULL;
	p = skip_ws(p);
	if (*p != ')')
	{
		*err = "expected ')' after the key column: the key is one column";
		return NULL;
	}
	*pp = skip_ws(p + 1);
	return s;
}

SiftConnSpecs *
sift_conn_parse(const char *text, const char **err)
{
	SiftConnSpecs *all = palloc0(sizeof(SiftConnSpecs));
	const char *p = text;

	if (strncmp(p, "auto:", 5) == 0)
	{
		all->automatic = true;
		p += 5;
	}
	for (;;)
	{
		SiftConnSpec *s;

		if (all->n == SIFT_CONN_MAX_CONNECTIONS)
		{
			*err = psprintf("too many connections (%d at most in an index)", SIFT_CONN_MAX_CONNECTIONS);
			return NULL;
		}
		if ((s = parse_one(&p, err)) == NULL)
			return NULL;
		for (int i = 0; i < all->n; i++)
			if (strcmp(all->c[i]->fk, s->fk) == 0)
			{
				*err = psprintf("column \"%s\" is connected twice: one connection for each column that holds a parent key", s->fk);
				return NULL;
			}
		all->c[all->n++] = s;
		p = skip_ws(p);
		if (*p == ';')
		{
			p++;
			continue;
		}
		if (*p != '\0')
		{
			*err = "unexpected text after the connection";
			return NULL;
		}
		break;
	}
	return all;
}

/* The connections of the index, or NULL: the option is not set, or 'off' */
SiftConnSpecs *
sift_conn_specs(Relation child_index)
{
	const char *text = sift_col_options_connect(child_index);
	const char *err = NULL;
	SiftConnSpecs *s;

	if (text[0] == '\0' || strcmp(text, "off") == 0 || strcmp(text, "auto") == 0 || strcmp(text, "auto:") == 0)
		return NULL;
	s = sift_conn_parse(text, &err);
	if (s == NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid connect option \"%s\": %s", text, err)));
	return s;
}

static AttrNumber
index_column(Relation index, AttrNumber heap_att)
{
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
		if (index->rd_index->indkey.values[i] == heap_att)
			return i + 1;
	return InvalidAttrNumber;
}

static bool
integer_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID;
}

/* The sift_col index of the parent table that tracks the key; the lowest oid when several do */
static Oid
parent_index_for(Relation parent, AttrNumber key)
{
	List	   *oids = RelationGetIndexList(parent);
	Oid			found = InvalidOid;

	foreach_oid(ioid, oids)
	{
		Relation	idx;

		if (OidIsValid(found))
			break;
		idx = index_open(ioid, AccessShareLock);
		if (sift_index_ready(idx) && idx->rd_index->indisvalid && sift_col_is_index(idx) && sift_col_row_key_on(idx) &&
			idx->rd_index->indkey.values[sift_col_rel(idx)->rowkey_attno - 1] == key)
			found = ioid;
		index_close(idx, AccessShareLock);
	}
	list_free(oids);
	return found;
}

/* The dependencies of the child index on indexes that are not in keep: a connection that was changed or removed */
static void
drop_stale_edges(Oid child, List *keep)
{
	Relation	drel = table_open(DependRelationId, RowExclusiveLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *stale = NIL;

	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(child));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->refclassid == RelationRelationId && d->deptype == DEPENDENCY_NORMAL && d->objsubid == 0 && !list_member_oid(keep, d->refobjid) &&
			get_rel_relkind(d->refobjid) == RELKIND_INDEX)
			stale = lappend_oid(stale, d->refobjid);
	}
	systable_endscan(scan);
	table_close(drel, RowExclusiveLock);
	foreach_oid(old, stale)
		deleteDependencyRecordsForSpecific(RelationRelationId, child, DEPENDENCY_NORMAL, RelationRelationId, old);
	list_free(stale);
}

/* The oid of the parent table of a connection: an unqualified name is in the schema of the child's table (CREATE INDEX builds with a restricted search_path and a dump restores with an empty one) */
static Oid
parent_table_oid(Oid child_table, const SiftConnSpec *s, LOCKMODE lock, bool missing_ok)
{
	RangeVar   *rv = makeRangeVar(s->parent_schema ? s->parent_schema : get_namespace_name(get_rel_namespace(child_table)), s->parent_table, -1);

	return RangeVarGetRelid(rv, lock, missing_ok);
}

/*
 * The checks of one connection and the sift_col index of its parent table that serves it (the one that tracks the key; the lowest oid when several do). The
 * parent table is left open in *parent, locked AccessShare. ERROR with a hint for what is wrong with the connection.
 */
Oid
sift_conn_parent_index(Relation heap, Relation index, const SiftConnSpec *s, Relation *parent)
{
	Oid			poid,
				pidx;
	AttrNumber	fk,
				key;
	Form_pg_attribute att;

	fk = get_attnum(RelationGetRelid(heap), s->fk);
	if (fk == InvalidAttrNumber || index_column(index, fk) == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: \"%s\" is not a column of this index", s->fk),
						errhint("Name the column of the index that holds the key of the parent table.")));
	att = TupleDescAttr(RelationGetDescr(heap), fk - 1);
	if (!integer_type(att->atttypid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: column \"%s\" is not an integer", s->fk),
						errhint("The key of a connection is an int2, int4 or int8 column.")));
	poid = parent_table_oid(RelationGetRelid(heap), s, AccessShareLock, false);
	if (poid == RelationGetRelid(heap))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: the parent table is the table of the index"),
						errhint("Name the table that this table joins.")));
	*parent = table_open(poid, NoLock);
	if ((*parent)->rd_rel->relkind != RELKIND_RELATION || (*parent)->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: \"%s\" is not a heap table", s->parent_table),
						errhint("The parent is a plain heap table.")));
	key = get_attnum(poid, s->parent_key);
	if (key == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: table \"%s\" has no column \"%s\"", s->parent_table, s->parent_key)));
	pidx = parent_index_for(*parent, key);
	if (!OidIsValid(pidx))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("connect: table \"%s\" has no sift_col index that tracks \"%s\"", s->parent_table, s->parent_key),
						errhint("CREATE INDEX .. ON %s USING sift_col (%s, ...) with track_changes naming %s (the default for a primary key).", s->parent_table, s->parent_key, s->parent_key)));
	return pidx;
}

/* The parent index of each connection the option names, once for a parent two connections share: the dependency of the child on it, or its journal when start */
static void
connect_each(Relation heap, Relation index, SiftConnSpecs *specs, bool start)
{
	List	   *seen = NIL;

	if (!sift_col_is_index(index) || heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: the index must be a sift_col index on a heap table"),
						errhint("Connect a sift_col index of a heap table to a sift_col index of another heap table.")));
	for (int i = 0; i < specs->n; i++)
	{
		Relation	parent;
		Oid			pidx = sift_conn_parent_index(heap, index, specs->c[i], &parent);

		if (!list_member_oid(seen, pidx))
		{
			Relation	pindex = index_open(pidx, ShareUpdateExclusiveLock);

			if (start)
				(void) sift_conn_enable(parent, pindex);
			else
				sift_conn_depend(index, pindex);
			index_close(pindex, NoLock);
			seen = lappend_oid(seen, pidx);
		}
		table_close(parent, NoLock);
	}
	if (!start)
		drop_stale_edges(RelationGetRelid(index), seen);
	list_free(seen);
}

/*
 * CREATE INDEX, REINDEX and ALTER INDEX of a child index: finds the parent index of each connection the option names (the one that tracks the key) and makes the dependency of the
 * child on it, or removes the connections when the option is off or not set.
 */
void
sift_conn_resolve(Relation heap, Relation index)
{
	SiftConnSpecs *specs = sift_conn_specs(index);

	if (specs == NULL)
		drop_stale_edges(RelationGetRelid(index), NIL);
	else
		connect_each(heap, index, specs, false);
}

void
sift_conn_start(Relation heap, Relation index)
{
	SiftConnSpecs *specs = sift_conn_specs(index);

	if (specs != NULL)
		connect_each(heap, index, specs, true);
}

/* After ALTER INDEX of any option of a sift_col index: the connections follow the option */
void
sift_conn_alter(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	sift_conn_resolve(heap, index);
	sift_conn_start(heap, index);
	table_close(heap, NoLock);
}

int
sift_conn_leaves(Relation index, SiftColDef *defs, int n, int *found, int *nconn)
{
	SiftConnSpecs *specs = sift_conn_specs(index);

	*nconn = 0;
	if (specs == NULL)
		return n;
	for (int c = 0; c < specs->n; c++)
	{
		const SiftConnSpec *s = specs->c[c];
		AttrNumber	fk = index_column(index, get_attnum(index->rd_index->indrelid, s->fk));

		if (fk == InvalidAttrNumber)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: \"%s\" is not a column of this index", s->fk),
							errhint("Name the column of the index that holds the key of the parent table.")));
		found[c] = n;
		defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_INT,.flags = SIFT_COLD_FOUND,.attno = fk,
			.type = SIFT_COLT_BOOL,.path = psprintf("%s#found", s->fk)};
		n++;
		(*nconn)++;
	}
	return n;
}

int
sift_conn_find(Relation heap, Relation index, AttrNumber fk, Oid parent_relid, AttrNumber parent_key, int *found, Oid *parent_index)
{
	SiftConnSpecs *specs = sift_conn_specs(index);
	const SiftColIndexMeta *meta = sift_col_index_defs(index);

	for (int c = 0; specs != NULL && c < specs->n; c++)
	{
		const SiftConnSpec *s = specs->c[c];
		Relation	parent;
		char	   *flag = psprintf("%s#found", s->fk);

		if (index_column(index, get_attnum(RelationGetRelid(heap), s->fk)) != fk ||
			parent_table_oid(RelationGetRelid(heap), s, NoLock, false) != parent_relid || get_attnum(parent_relid, s->parent_key) != parent_key)
			continue;
		for (int i = 0; i < meta->ncols; i++)
			if ((meta->cols[i].flags & SIFT_COLD_FOUND) && strcmp(meta->cols[i].path, flag) == 0)
				*found = i;
		*parent_index = sift_conn_parent_index(heap, index, s, &parent);
		table_close(parent, NoLock);
		return c;
	}
	return -1;
}

PG_FUNCTION_INFO_V1(sift_col_connection_state);

/* sift.col_connection_state(parent index): the lineage of its key journal, the records appended so far, and what a stamp taken now says ('ok' or why not); NULL lineage and records without a journal */
Datum
sift_col_connection_state(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftConnStamp st;
	SiftConnGen gen;
	TupleDesc	desc;
	Datum		v[3];
	bool		nulls[3] = {false};
	bool		has = sift_conn_gen(index, &gen);
	SiftConnWhy why = sift_conn_take(heap, index, GetTransactionSnapshot(), &st, NULL);
	bool		composite = get_call_result_type(fcinfo, NULL, &desc) == TYPEFUNC_COMPOSITE;

	SIFT_INVARIANT(composite, "return type must be a row type");
	v[0] = Int64GetDatum((int64) gen.lineage);
	v[1] = Int64GetDatum((int64) gen.records);
	v[2] = CStringGetTextDatum(sift_conn_why_name(why));
	nulls[0] = nulls[1] = !has;
	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

PG_FUNCTION_INFO_V1(sift_col_connection_list);

/* The text of one connection, as the option writes it */
static char *
connection_text(const SiftConnSpec *s)
{
	StringInfoData t;

	initStringInfo(&t);
	appendStringInfo(&t, "%s -> ", quote_identifier(s->fk));
	if (s->parent_schema)
		appendStringInfo(&t, "%s.", quote_identifier(s->parent_schema));
	appendStringInfo(&t, "%s(%s)", quote_identifier(s->parent_table), quote_identifier(s->parent_key));
	return t.data;
}

/*
 * sift.col_connection_list(child index): one row for each connection of the option: its place in the option, the column that holds the key, the text, the parent index that serves it
 * (NULL when the names do not resolve now: a listing of every index must not stop at one) and whether it was chosen from the foreign keys. Reads the catalogs, not the segments.
 */
Datum
sift_col_connection_list(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftConnSpecs *specs;

	InitMaterializedSRF(fcinfo, 0);
	specs = sift_col_is_index(index) ? sift_conn_specs(index) : NULL;
	for (int c = 0; specs != NULL && c < specs->n; c++)
	{
		const SiftConnSpec *s = specs->c[c];
		Datum		v[5];
		bool		nulls[5] = {false};
		Oid			poid = parent_table_oid(index->rd_index->indrelid, s, NoLock, true);
		Oid			pidx = InvalidOid;

		if (OidIsValid(poid))
		{
			Relation	parent = table_open(poid, AccessShareLock);
			AttrNumber	key = get_attnum(poid, s->parent_key);

			if (key != InvalidAttrNumber)
				pidx = parent_index_for(parent, key);
			table_close(parent, NoLock);
		}
		v[0] = Int32GetDatum(c + 1);
		v[1] = CStringGetTextDatum(s->fk);
		v[2] = CStringGetTextDatum(connection_text(s));
		v[3] = ObjectIdGetDatum(pidx);
		nulls[3] = !OidIsValid(pidx);
		v[4] = BoolGetDatum(specs->automatic);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, NoLock);
	return (Datum) 0;
}

/* ---- automatic connections: the validated foreign keys of the table (docs/COL-CONNECT.md) ---- */

bool		sift_col_auto_connect = true;

/* Whether the table has a sift_col index at all: a foreign key that does not connect to one is worth a word only then */
static bool
has_sift_index(Relation table)
{
	bool		found = false;

	foreach_oid(ioid, RelationGetIndexList(table))
	{
		Relation	idx = index_open(ioid, AccessShareLock);

		found |= sift_col_is_index(idx);
		index_close(idx, AccessShareLock);
	}
	return found;
}

/*
 * The connection a foreign key gives, or NULL with *why set to the reason there is none, and *say when the reason is one a user would expect to be told (the parent has a sift_col
 * index, and the foreign key is for this index): one validated column of this index that holds an integer, the key of a heap table that has a sift_col index tracking it. The
 * columns of the parent are read from its own index by the join.
 */
static SiftConnSpec *
auto_one(Relation heap, Relation index, const ForeignKeyCacheInfo *fk, char **conname, const char **why, bool *say)
{
	HeapTuple	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(fk->conoid));
	Form_pg_constraint con;
	Relation	parent;
	SiftConnSpec *s;
	AttrNumber	fkatt = fk->conkey[0],
				key = fk->confkey[0];
	bool		valid;

	SIFT_INVARIANT(HeapTupleIsValid(tup), "sift_col: no pg_constraint row for %u", fk->conoid);
	con = (Form_pg_constraint) GETSTRUCT(tup);
	*conname = pstrdup(NameStr(con->conname));
	valid = con->convalidated;
	ReleaseSysCache(tup);
	*say = false;
	if (fk->confrelid == RelationGetRelid(heap))
		*why = "it references its own table";
	else
	{
		bool		inside = false;

		for (int i = 0; i < fk->nkeys; i++)
			inside |= index_column(index, fk->conkey[i]) != InvalidAttrNumber;
		if (!inside)
			*why = "its columns are not in the index";
	}
	if (*why)
		return NULL;
	parent = table_open(fk->confrelid, AccessShareLock);
	if (parent->rd_rel->relkind != RELKIND_RELATION || parent->rd_rel->relam != HEAP_TABLE_AM_OID)
		*why = "the referenced table is not a plain heap table";
	else if (!has_sift_index(parent))
		*why = "the referenced table has no sift_col index";
	if (*why)
	{
		table_close(parent, NoLock);
		return NULL;
	}
	*say = true;
	if (fk->nkeys != 1)
		*why = "it has more than one column";
	else if (index_column(index, fkatt) == InvalidAttrNumber)
		*why = "its column is not in the index";
	else if (!valid)
		*why = "it is not validated";
	else if (!integer_type(TupleDescAttr(RelationGetDescr(heap), fkatt - 1)->atttypid))
		*why = "its column is not an int2, int4 or int8";
	else if (!integer_type(TupleDescAttr(RelationGetDescr(parent), key - 1)->atttypid))
		*why = "the referenced column is not an int2, int4 or int8";
	else if (!OidIsValid(parent_index_for(parent, key)))
		*why = "no sift_col index of the referenced table is ready and tracks the referenced column";
	if (*why)
	{
		table_close(parent, NoLock);
		return NULL;
	}
	s = palloc0(sizeof(SiftConnSpec));
	s->fk = get_attname(RelationGetRelid(heap), fkatt, false);
	s->parent_schema = parent->rd_rel->relnamespace == heap->rd_rel->relnamespace ? NULL : get_namespace_name(parent->rd_rel->relnamespace);
	s->parent_table = pstrdup(RelationGetRelationName(parent));
	s->parent_key = get_attname(RelationGetRelid(parent), key, false);
	table_close(parent, NoLock);
	return s;
}

void
sift_conn_auto(Relation heap, Relation index)
{
	const char *opt = sift_col_options_connect(index);
	StringInfoData text;
	Bitmapset  *used = NULL;
	int			n = 0;

	if (!sift_col_is_index(index) || heap->rd_rel->relam != HEAP_TABLE_AM_OID || (opt[0] == '\0' ? !sift_col_auto_connect : strcmp(opt, "auto") != 0))
		return;
	initStringInfo(&text);
	foreach_ptr(ForeignKeyCacheInfo, fk, RelationGetFKeyList(heap))
	{
		const char *why = NULL;
		char	   *conname;
		SiftConnSpec *s;
		bool		say;

		if (n == SIFT_CONN_MAX_CONNECTIONS || bms_is_member(fk->conkey[0], used))
			continue;
		s = auto_one(heap, index, fk, &conname, &why, &say);
		if (s == NULL)
		{
			if (say)
				ereport(NOTICE, (errmsg("sift_col index \"%s\": foreign key \"%s\" is not connected: %s", sift_col_final_name(index), conname, why)));
			else
				ereport(DEBUG1, (errmsg("sift_col index \"%s\": foreign key \"%s\" is not connected: %s", sift_col_final_name(index), conname, why)));
			continue;
		}
		used = bms_add_member(used, fk->conkey[0]);
		appendStringInfo(&text, "%s%s", n++ ? "; " : "", connection_text(s));
		ereport(NOTICE, (errmsg("sift_col index \"%s\" connected to %s(%s) through foreign key \"%s\"", sift_col_final_name(index),
								quote_identifier(s->parent_table), quote_identifier(s->parent_key), conname),
						 errhint("connect = 'off' on the index turns it off.")));
	}
	if (n > 0)
		sift_col_set_option(index, "connect", psprintf("auto: %s", text.data));
}
