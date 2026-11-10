/*
 * col/scope_decl.c - the option scope of a sift_col index (col/scope.h):
 *
 *     scope = 'key_name: fk -> [schema.]relation(member, scope_key); ...'
 *
 * The text is the record of the declaration, as for connect (col/conn_decl.c): it is in pg_class.reloptions, so pg_dump writes it and a restore or REINDEX resolves it again.
 * Resolving finds the unique btree of the scope relation on exactly (member, scope_key) and makes the index depend on it.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/scope.h"

bool		sift_agg_scope = true;
int			sift_scope_patch_keys = 4096;
double		sift_scope_max_multiplicity = 8.0;
#ifdef SIFT_TESTING
/* RELEASE-TODO(4.4): both go away with the patch reader of the scope index; a scope without a journal index is then always refused */
bool		sift_test_scope_trust = false;
bool		sift_test_scope_need_journal = false;
bool		sift_test_scope_pairs = false;
#endif

static bool
journal_required(void)
{
#ifdef SIFT_TESTING
	return sift_test_scope_need_journal;
#else
	return true;
#endif
}

/* One scope, from *pp to its end (a ';' or the end of the text); NULL with *err set when it does not parse */
static SiftScopeSpec *
parse_one(const char **pp, const char **err)
{
	SiftScopeSpec *s = palloc0(sizeof(SiftScopeSpec));
	const char *p = sift_conn_skip_ws(*pp);
	char	   *name;

	if ((p = sift_conn_read_name(p, &s->key_name, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ':')
	{
		*err = "expected ':' after the name of the derived key";
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &s->fk, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (strncmp(p, "->", 2) != 0)
	{
		*err = "expected '->' after the column of this index that holds the member key";
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 2), &name, err)) == NULL)
		return NULL;
	if (*p == '.')
	{
		s->rel_schema = name;
		if ((p = sift_conn_read_name(p + 1, &s->rel_name, err)) == NULL)
			return NULL;
	}
	else
		s->rel_name = name;
	p = sift_conn_skip_ws(p);
	if (*p != '(')
	{
		*err = "expected '(' and the member and scope key columns of the relation after its name";
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &s->member, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ',')
	{
		*err = "expected ',' and the scope key column after the member column";
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &s->scope_key, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ')')
	{
		*err = "expected ')' after the scope key column: a scope is a member column and a scope key column";
		return NULL;
	}
	p = sift_conn_skip_ws(p + 1);
	if (strncmp(p, "cols", 4) == 0 && *sift_conn_skip_ws(p + 4) == '(')
	{
		p = sift_conn_skip_ws(p + 4) + 1;
		for (;;)
		{
			char	   *col;

			if ((p = sift_conn_read_name(sift_conn_skip_ws(p), &col, err)) == NULL)
				return NULL;
			s->cols = lappend(s->cols, col);
			p = sift_conn_skip_ws(p);
			if (*p == ',')
			{
				p++;
				continue;
			}
			if (*p++ != ')')
			{
				*err = "expected ',' or ')' in cols(..), the list of the columns the scope segments keep";
				return NULL;
			}
			break;
		}
		p = sift_conn_skip_ws(p);
	}
	*pp = p;
	return s;
}

SiftScopeSpecs *
sift_scope_parse(const char *text, const char **err)
{
	SiftScopeSpecs *all = palloc0(sizeof(SiftScopeSpecs));
	const char *p = text;

	for (;;)
	{
		SiftScopeSpec *s;

		if (all->n == SIFT_SCOPE_MAX)
		{
			*err = psprintf("too many scopes (%d at most in an index)", SIFT_SCOPE_MAX);
			return NULL;
		}
		if ((s = parse_one(&p, err)) == NULL)
			return NULL;
		for (int i = 0; i < all->n; i++)
			if (strcmp(all->s[i]->key_name, s->key_name) == 0)
			{
				*err = psprintf("the derived key \"%s\" is declared twice: each scope has a name of its own", s->key_name);
				return NULL;
			}
		all->s[all->n++] = s;
		if (*p == ';')
		{
			p++;
			continue;
		}
		if (*p != '\0')
		{
			*err = "unexpected text after the scope";
			return NULL;
		}
		return all;
	}
}

/* The scopes of the index, or NULL: the option is not set, or 'off' */
SiftScopeSpecs *
sift_scope_specs(Relation index)
{
	const char *text = sift_col_options_scope(index);
	const char *err = NULL;
	SiftScopeSpecs *s;

	if (text[0] == '\0' || strcmp(text, "off") == 0)
		return NULL;
	s = sift_scope_parse(text, &err);
	if (s == NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid scope option \"%s\": %s", text, err)));
	return s;
}

/* The checks of one column of the scope relation: it exists, is an integer and is NOT NULL (a pair has both keys) */
static AttrNumber
relation_column(Relation rel, const SiftScopeSpec *s, const char *name)
{
	AttrNumber	att = get_attnum(RelationGetRelid(rel), name);
	Form_pg_attribute a;

	if (att == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope \"%s\": relation \"%s\" has no column \"%s\"", s->key_name, s->rel_name, name)));
	a = TupleDescAttr(RelationGetDescr(rel), att - 1);
	if (!sift_conn_integer_type(a->atttypid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" of \"%s\" is not an integer", s->key_name, name, s->rel_name),
						errhint("The member and scope key columns are int2, int4 or int8; map a text key through a dictionary table.")));
	if (!a->attnotnull)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" of \"%s\" is nullable", s->key_name, name, s->rel_name),
						errhint("Declare the member and scope key columns NOT NULL: a pair has both keys.")));
	return att;
}

/* The attribute of the index named name, InvalidAttrNumber when none */
static AttrNumber
index_attribute(Relation index, const char *name)
{
	for (int a = 1; a <= RelationGetNumberOfAttributes(index); a++)
		if (strcmp(NameStr(TupleDescAttr(RelationGetDescr(index), a - 1)->attname), name) == 0)
			return a;
	return InvalidAttrNumber;
}

/* cols(..): each name is a column of the index, once. The member key and the derived key are kept whether or not they are named, so naming them is not an error */
static void
check_cols(Relation index, const SiftScopeSpec *s)
{
	foreach_ptr(char, name, s->cols)
	{
		if (strcmp(name, s->key_name) == 0)
			continue;
		if (index_attribute(index, name) == InvalidAttrNumber)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope \"%s\": cols(..) names \"%s\", which is not a column of this index", s->key_name, name),
							errhint("Name columns of the index: the scope segments keep those, the member key and the column the units are cut by.")));
	}
	for (int i = 0; i < list_length(s->cols); i++)
		for (int j = 0; j < i; j++)
			if (strcmp(list_nth(s->cols, i), list_nth(s->cols, j)) == 0)
				ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN), errmsg("scope \"%s\": cols(..) names \"%s\" twice", s->key_name, (char *) list_nth(s->cols, i)),
								errhint("Name each column once.")));
}

/* The unique btree of the relation on exactly the two columns, in either order; the lowest oid when several do */
static Oid
pair_index(Relation rel, AttrNumber member, AttrNumber key)
{
	Oid			found = InvalidOid;

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx;
		const FormData_pg_index *i;

		if (OidIsValid(found))
			break;
		idx = index_open(ioid, AccessShareLock);
		i = idx->rd_index;
		if (idx->rd_rel->relam == BTREE_AM_OID && i->indisunique && i->indimmediate && i->indisvalid && i->indisready && i->indnatts == 2 && i->indnkeyatts == 2 &&
			heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL) &&
			((i->indkey.values[0] == member && i->indkey.values[1] == key) || (i->indkey.values[0] == key && i->indkey.values[1] == member)))
			found = ioid;
		index_close(idx, AccessShareLock);
	}
	return found;
}

/* The journal index of the scope relation: a ready sift_col index with track_changes = 'ctid' whose first column is the member and that holds the scope key; the lowest oid when several do */
Oid
sift_scope_journal_index(Relation rel, AttrNumber member, AttrNumber scope_key)
{
	Oid			found = InvalidOid;

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx;

		if (OidIsValid(found))
			break;
		idx = index_open(ioid, AccessShareLock);
		if (sift_col_is_index(idx) && sift_index_ready(idx) && idx->rd_index->indisvalid && sift_col_track_ctid(idx) &&
			idx->rd_index->indkey.values[0] == member && sift_conn_index_column(idx, scope_key) != InvalidAttrNumber)
			found = ioid;
		index_close(idx, AccessShareLock);
	}
	return found;
}

/*
 * The checks of one scope and the unique btree of its relation that serves it. The relation is left open in *rel, locked AccessShare. ERROR with a hint for what is wrong with the
 * declaration.
 */
Oid
sift_scope_relation_index(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key)
{
	Oid			roid,
				btree;
	AttrNumber	fk = get_attnum(RelationGetRelid(heap), s->fk),
				named;

	if (fk == InvalidAttrNumber || sift_conn_index_column(index, fk) == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope \"%s\": \"%s\" is not a column of this index", s->key_name, s->fk),
						errhint("Name the column of the index that holds the member key.")));
	if (!sift_conn_integer_type(TupleDescAttr(RelationGetDescr(heap), fk - 1)->atttypid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" is not an integer", s->key_name, s->fk),
						errhint("The member key is an int2, int4 or int8 column.")));
	named = get_attnum(RelationGetRelid(heap), s->key_name);
	if (named != InvalidAttrNumber && sift_conn_index_column(index, named) != InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN), errmsg("scope \"%s\": the name of the derived key is a column of this index", s->key_name),
						errhint("Give the derived key a name that no column of the index has: key_order names a column or a derived key.")));
	roid = sift_conn_relation_oid(RelationGetRelid(heap), s->rel_schema, s->rel_name, AccessShareLock, false);
	if (roid == RelationGetRelid(heap))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the scope relation is the table of the index", s->key_name),
						errhint("Name the table of (member, scope key) pairs.")));
	*rel = table_open(roid, NoLock);
	if ((*rel)->rd_rel->relkind != RELKIND_RELATION || (*rel)->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": \"%s\" is not a plain heap table", s->key_name, s->rel_name),
						errhint("The scope relation is a heap table that is not partitioned: a view or a partitioned table is not supported.")));
	check_cols(index, s);
	*member = relation_column(*rel, s, s->member);
	*scope_key = relation_column(*rel, s, s->scope_key);
	if (*member == *scope_key)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the member and scope key columns are the same column", s->key_name),
						errhint("A scope relation has two columns: the member key and the scope key.")));
	btree = pair_index(*rel, *member, *scope_key);
	if (!OidIsValid(btree))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": \"%s\" has no unique btree index on (%s, %s)", s->key_name, s->rel_name, s->member, s->scope_key),
						errhint("CREATE UNIQUE INDEX on %s (%s, %s), or make them its primary key: a plain, immediate, unique btree on exactly those two columns.", s->rel_name, s->member, s->scope_key)));
	return btree;
}

/* Is the sift_col index a scope relation's journal index (track_changes = 'ctid', as opposed to the parent index of a connection, which tracks a key column)? */
static bool
is_journal_index(Oid oid)
{
	Relation	idx = index_open(oid, AccessShareLock);
	bool		ctid = sift_col_track_ctid(idx);

	index_close(idx, AccessShareLock);
	return ctid;
}

/*
 * The dependencies of the index on btrees and journal indexes (its scopes'): the ones that are not in keep are removed, the ones of keep that are not there are made. The parent
 * indexes of its connections are the business of sift_conn_resolve, which removes the edges to sift_col indexes it does not name: its removal is made visible first.
 */
static void
sync_edges(Oid index, List *keep)
{
	Relation	drel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *stale = NIL,
			   *present = NIL,
			   *edges = NIL;
	ObjectAddress from;

	CommandCounterIncrement();
	drel = table_open(DependRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(index));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->refclassid == RelationRelationId && d->deptype == DEPENDENCY_NORMAL && d->objsubid == 0 && d->refobjsubid == 0 && get_rel_relkind(d->refobjid) == RELKIND_INDEX)
			edges = lappend_oid(edges, d->refobjid);
	}
	systable_endscan(scan);
	table_close(drel, RowExclusiveLock);
	foreach_oid(edge, edges)
	{
		if (sift_col_is_index_oid(edge) && !is_journal_index(edge))
			continue;
		if (list_member_oid(keep, edge))
			present = lappend_oid(present, edge);
		else
			stale = lappend_oid(stale, edge);
	}
	foreach_oid(old, stale)
		deleteDependencyRecordsForSpecific(RelationRelationId, index, DEPENDENCY_NORMAL, RelationRelationId, old);
	ObjectAddressSet(from, RelationRelationId, index);
	foreach_oid(btree, keep)
		if (!list_member_oid(present, btree))
		{
			ObjectAddress to;

			ObjectAddressSet(to, RelationRelationId, btree);
			recordDependencyOn(&from, &to, DEPENDENCY_NORMAL);
		}
}

/*
 * CREATE INDEX, REINDEX and ALTER INDEX: finds the unique btree of the relation of each scope the option names and makes the dependency of the index on it, so that DROP INDEX of
 * the btree or DROP TABLE of the relation needs CASCADE (which drops this index) and a dump orders them first; removes the dependencies when the option is off or not set.
 */
void
sift_scope_resolve(Relation heap, Relation index)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	List	   *keep = NIL;

	if (specs != NULL && (!sift_col_is_index(index) || heap->rd_rel->relam != HEAP_TABLE_AM_OID))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope: the index must be a sift_col index on a heap table"),
						errhint("Declare a scope on a sift_col index of a heap table.")));
	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		Relation	rel;
		AttrNumber	member,
					key;
		Oid			btree = sift_scope_relation_index(heap, index, specs->s[i], &rel, &member, &key);
		Oid			journal = sift_scope_journal_index(rel, member, key);

		if (!OidIsValid(journal) && journal_required())
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": \"%s\" has no sift_col index that journals its changes", specs->s[i]->key_name, specs->s[i]->rel_name),
							errhint("CREATE INDEX ON %s USING sift_col (%s, %s) WITH (track_changes = 'ctid'): the member column first, so that a read can tell which members changed since the scope segments were written.",
									specs->s[i]->rel_name, specs->s[i]->member, specs->s[i]->scope_key)));
		table_close(rel, NoLock);
		keep = list_append_unique_oid(keep, btree);
		if (OidIsValid(journal))
			keep = list_append_unique_oid(keep, journal);
	}
	sync_edges(RelationGetRelid(index), keep);
	list_free(keep);
}

/* After ALTER INDEX of any option of a sift_col index: the scopes follow the option */
void
sift_scope_alter(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	sift_scope_resolve(heap, index);
	sift_scope_start(heap, index);
	table_close(heap, NoLock);
}

int
sift_scope_time_leaf(const SiftColDef *defs, int n)
{
	for (int id = 0; id < n; id++)
		if (defs[id].parent < 0 && defs[id].lane == 0 && !(defs[id].flags & SIFT_COLD_DERIVED) &&
			(defs[id].type == SIFT_COLT_DATE || defs[id].type == SIFT_COLT_TIMESTAMP || defs[id].type == SIFT_COLT_TIMESTAMPTZ))
			return id;
	return -1;
}

/*
 * The leaves the scope segments of scope c keep values of: all of them without cols(..); else the derived key, the leaves of the listed columns and of the member key (a leaf of
 * a column is every leaf its attribute has, and the found flag of a connection follows its key's column), and the leaf the units are cut by after the key, whose values order them.
 */
static void
scope_kept(Relation index, const SiftScopeSpec *s, int own, int time, const SiftColDef *defs, int n, bool *kept)
{
	List	   *atts = NIL;

	for (int id = 0; id < n; id++)
		kept[id] = s->cols == NIL;
	if (s->cols == NIL)
		return;
	atts = lappend_int(atts, sift_conn_index_column(index, get_attnum(index->rd_index->indrelid, s->fk)));
	foreach_ptr(char, name, s->cols)
		atts = lappend_int(atts, index_attribute(index, name));
	for (int id = 0; id < n; id++)
		kept[id] = (defs[id].flags & SIFT_COLD_SCOPE) ? id == own : id == time || list_member_int(atts, defs[id].attno);
}

int
sift_scope_leaves(Relation index, SiftColDef *defs, int n, int *leaf, int *nscope, bool (*kept)[SIFT_COL_MAX_COLS])
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	int			first = n,
				time;

	*nscope = 0;
	for (int c = 0; specs != NULL && c < specs->n; c++)
	{
		const SiftScopeSpec *s = specs->s[c];
		AttrNumber	fk = sift_conn_index_column(index, get_attnum(index->rd_index->indrelid, s->fk));

		if (fk == InvalidAttrNumber)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope \"%s\": \"%s\" is not a column of this index", s->key_name, s->fk),
							errhint("Name the column of the index that holds the member key.")));
		leaf[c] = n;
		defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_INT,.flags = SIFT_COLD_SCOPE,.attno = fk,
			.type = SIFT_COLT_INT8,.path = psprintf("%s#scope", s->key_name)};
		n++;
		(*nscope)++;
	}
	time = sift_scope_time_leaf(defs, first);
	for (int c = 0; c < *nscope; c++)
		scope_kept(index, specs->s[c], leaf[c], time, defs, n, kept[c]);
	return n;
}

int
sift_scope_find(Relation heap, Relation index, AttrNumber fk, Oid rel, AttrNumber member, AttrNumber scope_key, int *leaf, Oid *btree)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	const SiftColIndexMeta *meta = sift_col_index_defs(index);

	for (int c = 0; specs != NULL && c < specs->n; c++)
	{
		const SiftScopeSpec *s = specs->s[c];
		Relation	scope_rel;
		AttrNumber	m,
					k;
		char	   *name = psprintf("%s#scope", s->key_name);

		if (sift_conn_index_column(index, get_attnum(RelationGetRelid(heap), s->fk)) != fk ||
			sift_conn_relation_oid(RelationGetRelid(heap), s->rel_schema, s->rel_name, NoLock, false) != rel ||
			get_attnum(rel, s->member) != member || get_attnum(rel, s->scope_key) != scope_key)
			continue;
		for (int i = 0; i < meta->ncols; i++)
			if ((meta->cols[i].flags & SIFT_COLD_SCOPE) && strcmp(meta->cols[i].path, name) == 0)
				*leaf = i;
		*btree = sift_scope_relation_index(heap, index, s, &scope_rel, &m, &k);
		table_close(scope_rel, NoLock);
		return c;
	}
	return -1;
}
