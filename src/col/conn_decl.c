/*
 * col/conn_decl.c - the option connect of a child sift_col index (col/conn.h, docs/COL-CONNECT.md):
 *
 *     connect = 'fk -> [schema.]parent(key) carries a, b [via carried|map|auto]'
 *
 * The text is the record of the connection: it is in pg_class.reloptions, so pg_dump writes it and a restore or REINDEX resolves it again. Resolving finds the sift_col index of
 * the parent table that tracks the key and holds the carried columns, makes the child index depend on it and the parent index keep its key journal.
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
#include "catalog/pg_depend.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

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

static bool
read_word(const char **p, const char *word)
{
	size_t		n = strlen(word);

	if (pg_strncasecmp(*p, word, n) != 0 || isalnum((unsigned char) (*p)[n]) || (*p)[n] == '_')
		return false;
	*p += n;
	return true;
}

SiftConnSpec *
sift_conn_parse(const char *text, const char **err)
{
	SiftConnSpec *s = palloc0(sizeof(SiftConnSpec));
	const char *p = skip_ws(text);
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
	p = skip_ws(p + 1);
	if (!read_word(&p, "carries"))
	{
		*err = "expected 'carries' and the parent columns to carry";
		return NULL;
	}
	for (;;)
	{
		if (s->ncarry == SIFT_CONN_MAX_CARRY)
		{
			*err = "too many carried columns (16 at most)";
			return NULL;
		}
		if ((p = read_name(skip_ws(p), &s->carry[s->ncarry], err)) == NULL)
			return NULL;
		s->ncarry++;
		p = skip_ws(p);
		if (*p != ',')
			break;
		p++;
	}
	s->via = SIFT_CONN_VIA_AUTO;
	if (read_word(&p, "via"))
	{
		p = skip_ws(p);
		if (read_word(&p, "carried"))
			s->via = SIFT_CONN_VIA_CARRIED;
		else if (read_word(&p, "map"))
			s->via = SIFT_CONN_VIA_MAP;
		else if (!read_word(&p, "auto"))
		{
			*err = "via is carried, map or auto";
			return NULL;
		}
		p = skip_ws(p);
	}
	if (*p != '\0')
	{
		*err = "unexpected text after the connection";
		return NULL;
	}
	return s;
}

/* The connection of the index, or NULL: the option is not set, or 'off' */
SiftConnSpec *
sift_conn_spec(Relation child_index)
{
	const char *text = sift_col_options_connect(child_index);
	const char *err = NULL;
	SiftConnSpec *s;

	if (text[0] == '\0' || strcmp(text, "off") == 0)
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

/* The sift_col index of the parent table that tracks the key and holds every carried column as a plain column; the lowest oid when several do */
static Oid
parent_index_for(Relation parent, AttrNumber key, const AttrNumber *carry, int ncarry)
{
	List	   *oids = RelationGetIndexList(parent);
	Oid			found = InvalidOid;

	foreach_oid(ioid, oids)
	{
		Relation	idx;
		bool		fits;

		if (OidIsValid(found))
			break;
		idx = index_open(ioid, AccessShareLock);
		fits = sift_index_ready(idx) && idx->rd_index->indisvalid && sift_col_is_index(idx) && sift_col_row_key_on(idx) &&
			idx->rd_index->indkey.values[sift_col_rel(idx)->rowkey_attno - 1] == key;
		for (int i = 0; fits && i < ncarry; i++)
			fits = index_column(idx, carry[i]) != InvalidAttrNumber;
		if (fits)
			found = ioid;
		index_close(idx, AccessShareLock);
	}
	list_free(oids);
	return found;
}

/* The dependencies of the child index on other indexes that are not the parent: a connection that was changed or removed */
static void
drop_stale_edges(Oid child, Oid keep)
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

		if (d->refclassid == RelationRelationId && d->deptype == DEPENDENCY_NORMAL && d->objsubid == 0 && d->refobjid != keep && get_rel_relkind(d->refobjid) == RELKIND_INDEX)
			stale = lappend_oid(stale, d->refobjid);
	}
	systable_endscan(scan);
	table_close(drel, RowExclusiveLock);
	foreach_oid(old, stale)
		deleteDependencyRecordsForSpecific(RelationRelationId, child, DEPENDENCY_NORMAL, RelationRelationId, old);
	list_free(stale);
}

/*
 * CREATE INDEX, REINDEX and ALTER INDEX of a child index: finds the parent index the option names and connects to it (the parent index is the one that tracks the key; its
 * table is locked against writers until the end of the transaction, for its journal), or removes the connection when the option is off or not set.
 */
void
sift_conn_resolve(Relation heap, Relation index)
{
	SiftConnSpec *s = sift_conn_spec(index);
	RangeVar   *rv;
	Oid			poid,
				pidx;
	Relation	parent,
				pindex;
	AttrNumber	fk,
				key,
				carry[SIFT_CONN_MAX_CARRY];
	Form_pg_attribute att;

	if (s == NULL)
	{
		drop_stale_edges(RelationGetRelid(index), InvalidOid);
		return;
	}
	if (!sift_col_is_index(index) || heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: the index must be a sift_col index on a heap table"),
						errhint("Connect a sift_col index of a heap table to a sift_col index of another heap table.")));
	fk = get_attnum(RelationGetRelid(heap), s->fk);
	if (fk == InvalidAttrNumber || index_column(index, fk) == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: \"%s\" is not a column of this index", s->fk),
						errhint("Name the column of the index that holds the key of the parent table.")));
	att = TupleDescAttr(RelationGetDescr(heap), fk - 1);
	if (!integer_type(att->atttypid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: column \"%s\" is not an integer", s->fk),
						errhint("The key of a connection is an int2, int4 or int8 column.")));
	/* CREATE INDEX runs the build with a restricted search_path and a dump restores with an empty one: an unqualified parent is in the schema of this index's table */
	rv = makeRangeVar(s->parent_schema ? s->parent_schema : get_namespace_name(RelationGetNamespace(heap)), s->parent_table, -1);
	poid = RangeVarGetRelid(rv, AccessShareLock, false);
	if (poid == RelationGetRelid(heap))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: the parent table is the table of the index"),
						errhint("Name the table that this table joins.")));
	parent = table_open(poid, NoLock);
	if (parent->rd_rel->relkind != RELKIND_RELATION || parent->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("connect: \"%s\" is not a heap table", s->parent_table),
						errhint("The parent is a plain heap table.")));
	key = get_attnum(poid, s->parent_key);
	if (key == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: table \"%s\" has no column \"%s\"", s->parent_table, s->parent_key)));
	for (int i = 0; i < s->ncarry; i++)
		if ((carry[i] = get_attnum(poid, s->carry[i])) == InvalidAttrNumber)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("connect: table \"%s\" has no column \"%s\"", s->parent_table, s->carry[i])));
	pidx = parent_index_for(parent, key, carry, s->ncarry);
	if (!OidIsValid(pidx))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("connect: table \"%s\" has no sift_col index that tracks \"%s\" and holds the carried columns", s->parent_table, s->parent_key),
						errhint("CREATE INDEX .. ON %s USING sift_col (%s, ...) with the carried columns in the list, and track_changes naming %s (the default for a primary key).", s->parent_table, s->parent_key, s->parent_key)));
	drop_stale_edges(RelationGetRelid(index), pidx);
	pindex = index_open(pidx, ShareUpdateExclusiveLock);
	sift_conn_connect(index, parent, pindex);
	index_close(pindex, NoLock);
	table_close(parent, NoLock);
}

/* After ALTER INDEX of any option of a sift_col index: the connection follows the option */
void
sift_conn_alter(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	sift_conn_resolve(heap, index);
	table_close(heap, NoLock);
}
