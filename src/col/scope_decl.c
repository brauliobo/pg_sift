/*
 * col/scope_decl.c - the option scope of a sift_col index (col/scope.h):
 *
 *     scope = 'key_name: fk -> [schema.]relation(member, scope_key); ...'
 *
 * The text is the record of the declaration, as for connect (col/conn_decl.c): it is in pg_class.reloptions, so pg_dump writes it and a restore or REINDEX resolves it again.
 * Resolving finds the unique btree of the scope relation on exactly (member, scope_key) and makes the index depend on it.
 */
#include "postgres.h"

#include <ctype.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "utils/ruleutils.h"
#include "utils/fmgroids.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/scope.h"

bool		sift_agg_scope = true;
double		sift_scope_max_multiplicity = 3.0;
bool		sift_test_scope_patch = false;
#ifdef SIFT_TESTING
int			sift_test_scope_unit_kind = 0;
#endif

/* [using name], when the text has it; p is after the closing parenthesis of the relation */
static const char *
parse_using(const char *p, char **name, const char **err)
{
	p = sift_conn_skip_ws(p);
	if (strncmp(p, "using", 5) != 0 || !isspace((unsigned char) p[5]))
		return p;
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 5), name, err)) == NULL)
		return NULL;
	return sift_conn_skip_ws(p);
}

/* [schema.]name(a, b) [using index] of one relation, whose columns have the roles ra and rb (the words of the errors) */
static const char *
parse_rel(const char *p, SiftScopeRel *r, const char *ra, const char *rb, const char **err)
{
	char	   *name;

	if ((p = sift_conn_read_name(sift_conn_skip_ws(p), &name, err)) == NULL)
		return NULL;
	if (*p == '.')
	{
		r->schema = name;
		if ((p = sift_conn_read_name(p + 1, &r->name, err)) == NULL)
			return NULL;
	}
	else
		r->name = name;
	p = sift_conn_skip_ws(p);
	if (*p != '(')
	{
		*err = psprintf("expected '(' and the %s and %s columns of the relation after its name", ra, rb);
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &r->a, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ',')
	{
		*err = psprintf("expected ',' and the %s column after the %s column", rb, ra);
		return NULL;
	}
	if ((p = sift_conn_read_name(sift_conn_skip_ws(p + 1), &r->b, err)) == NULL)
		return NULL;
	p = sift_conn_skip_ws(p);
	if (*p != ')')
	{
		*err = psprintf("expected ')' after the %s column: a scope is a member column and a scope key column", rb);
		return NULL;
	}
	return parse_using(p + 1, &r->index_name, err);
}

/* One scope, from *pp to its end (a ';' or the end of the text); NULL with *err set when it does not parse */
static SiftScopeSpec *
parse_one(const char **pp, const char **err)
{
	SiftScopeSpec *s = palloc0(sizeof(SiftScopeSpec));
	const char *p = sift_conn_skip_ws(*pp);

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
	if ((p = parse_rel(p + 2, &s->rel, "member", "scope key", err)) == NULL)
		return NULL;
	if (*p == '|')
	{
		s->joined = palloc0(sizeof(SiftScopeRel));
		s->via = palloc0(sizeof(SiftScopeRel));
		if ((p = parse_rel(p + 1, s->joined, "via", "scope key", err)) == NULL)
			return NULL;
		if (strncmp(p, "via", 3) != 0 || !isspace((unsigned char) p[3]))
		{
			*err = "expected 'via' and the relation that lists the members of the via column after the second path";
			return NULL;
		}
		if ((p = parse_rel(p + 3, s->via, "via", "member", err)) == NULL)
			return NULL;
	}
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

/*
 * The checks of one column of a relation of the scope: it exists and is an integer. A scope of one path needs it NOT NULL (a pair has both keys); on two paths a row with a NULL in a column of
 * its path is not a pair of that path, as a join drops it (a grant on a group has no member, a grant on a member no group).
 */
static AttrNumber
relation_column(Relation rel, const SiftScopeSpec *s, const SiftScopeRel *r, const char *name)
{
	AttrNumber	att = get_attnum(RelationGetRelid(rel), name);
	Form_pg_attribute a;

	if (att == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope \"%s\": relation \"%s\" has no column \"%s\"", s->key_name, r->name, name)));
	a = TupleDescAttr(RelationGetDescr(rel), att - 1);
	if (!sift_conn_integer_type(a->atttypid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" of \"%s\" is not an integer", s->key_name, name, r->name),
						errhint("The member and scope key columns are int2, int4 or int8; map a text key through a dictionary table.")));
	if (!a->attnotnull && s->joined == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" of \"%s\" is nullable", s->key_name, name, r->name),
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

/*
 * Does the index serve as the unique btree of the pairs (member, scope_key): an immediate, unique btree on exactly those two columns, in either order. Its predicate, if it has one, says which
 * rows are pairs (WHERE deleted_at IS NULL): PostgreSQL accepts only an immutable one, and the rows that satisfy it are unique on the two columns.
 */
static bool
is_pair_btree(Relation idx, AttrNumber member, AttrNumber key)
{
	const FormData_pg_index *i = idx->rd_index;

	return idx->rd_rel->relam == BTREE_AM_OID && i->indisunique && i->indimmediate && i->indisvalid && i->indisready && i->indnatts == 2 && i->indnkeyatts == 2 &&
		((i->indkey.values[0] == member && i->indkey.values[1] == key) || (i->indkey.values[0] == key && i->indkey.values[1] == member));
}

/*
 * The unique btree that defines the pairs of the relation, chosen when the scope segments are written (the index records it, see sift_scope_relation_index): the one named by using; else the
 * only partial one (its predicate is a statement of which rows are pairs), else the lowest oid of the plain ones. Two partial ones are ERROR: which rows are pairs would be a guess.
 */
static Oid
pair_index(Relation rel, const SiftScopeSpec *s, const SiftScopeRel *r, AttrNumber member, AttrNumber key)
{
	Oid			plain = InvalidOid,
				partial = InvalidOid;

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		bool		ok = is_pair_btree(idx, member, key),
					named = r->index_name != NULL && strcmp(RelationGetRelationName(idx), r->index_name) == 0,
					pred = !heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL);

		index_close(idx, AccessShareLock);
		if (named)
		{
			if (!ok)
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": index \"%s\" is not a valid immediate unique btree on exactly (%s, %s)", s->key_name, r->index_name, r->a, r->b),
								errhint("Name a unique btree of %s on its member and scope key columns, in either order; it may be partial.", r->name)));
			return ioid;
		}
		if (!ok || r->index_name != NULL)
			continue;
		if (!pred && !OidIsValid(plain))
			plain = ioid;
		else if (pred && OidIsValid(partial))
			ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_PARAMETER), errmsg("scope \"%s\": \"%s\" has two partial unique btrees on (%s, %s)", s->key_name, r->name, r->a, r->b),
							errhint("The predicate of the unique btree says which rows are pairs: name the one that does with USING <index> after the columns.")));
		else if (pred)
			partial = ioid;
	}
	if (r->index_name != NULL)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": \"%s\" has no index \"%s\"", s->key_name, r->name, r->index_name),
						errhint("USING names a unique btree of the scope relation.")));
	return OidIsValid(partial) ? partial : plain;
}

/* The predicate of an index as an implicit-AND list over Vars of varno 1 (a copy), NIL for a plain one */
static List *
index_predicate(Oid indexoid)
{
	Relation	idx = index_open(indexoid, AccessShareLock);
	List	   *pred = list_copy(RelationGetIndexPredicate(idx));

	index_close(idx, AccessShareLock);
	return pred;
}

/*
 * The journal indexes of the scope relation: ready sift_col indexes with track_changes = 'ctid' whose first column is the member and that hold the scope key; the lowest oid of those that hear of
 * every pair. A journal records the pairs a statement ends and inserts, so a partial one qualifies only when the predicate of the btree that defines the pairs implies its predicate (every pair
 * row is in it): else it never hears of a pair inserted outside its predicate and a read would take the member for unchanged. unfit: the first one that is not so (for the hint), not a candidate.
 */
static Oid
journal_scan(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree, bool unfit)
{
	List	   *pair_pred = index_predicate(pair_btree);

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		bool		ok = sift_col_is_index(idx) && sift_index_ready(idx) && idx->rd_index->indisvalid && sift_col_track_ctid(idx) &&
			idx->rd_index->indkey.values[0] == member && sift_conn_index_column(idx, scope_key) != InvalidAttrNumber;

		if (ok)
		{
			List	   *jpred = RelationGetIndexPredicate(idx);

			ok = (jpred == NIL || predicate_implied_by(jpred, pair_pred, false)) != unfit;
		}
		index_close(idx, AccessShareLock);
		if (ok)
			return ioid;
	}
	return InvalidOid;
}

Oid
sift_scope_journal_index(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree)
{
	return journal_scan(rel, member, scope_key, pair_btree, false);
}

/* The journal index whose leaves list exactly the pairs (its predicate and the one of the btree imply each other): a plain journal of a partial btree holds the soft deleted rows too */
Oid
sift_scope_pairs_journal(Relation rel, AttrNumber member, AttrNumber scope_key, Oid pair_btree)
{
	Oid			journal = sift_scope_journal_index(rel, member, scope_key, pair_btree);

	return OidIsValid(journal) && predicate_implied_by(index_predicate(pair_btree), index_predicate(journal), false) ? journal : InvalidOid;
}

/* Relation i of the declaration: 0 the pairs (member, scope key), 1 the joined path (via, scope key), 2 the members of the via column (via, member) */
static const SiftScopeRel *
decl_rel(const SiftScopeSpec *s, int i)
{
	return i == 0 ? &s->rel : i == 1 ? s->joined : s->via;
}

/* The relation of the declaration, a heap table that is not the fact table, left open in *rel, locked AccessShare */
static void
open_table(Relation heap, const SiftScopeSpec *s, const SiftScopeRel *r, Relation *rel)
{
	Oid			roid = sift_conn_relation_oid(RelationGetRelid(heap), r->schema, r->name, AccessShareLock, false);

	if (roid == RelationGetRelid(heap))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the scope relation is the table of the index", s->key_name),
						errhint("Name the table of (member, scope key) pairs.")));
	*rel = table_open(roid, NoLock);
	if ((*rel)->rd_rel->relkind != RELKIND_RELATION || (*rel)->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": \"%s\" is not a plain heap table", s->key_name, r->name),
						errhint("The scope relation is a heap table that is not partitioned: a view or a partitioned table is not supported.")));
}

/* The two columns of the relation of the declaration: integers, two columns */
static void
check_columns(Relation rel, const SiftScopeSpec *s, const SiftScopeRel *r, AttrNumber *a, AttrNumber *b)
{
	*a = relation_column(rel, s, r, r->a);
	*b = relation_column(rel, s, r, r->b);
	if (*a == *b)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the member and scope key columns are the same column", s->key_name),
						errhint("A scope relation has two columns: the member key and the scope key.")));
}

/* The checks of one scope's declaration; the relation is left open in *rel, locked AccessShare. ERROR with a hint for what is wrong with it */
void
sift_scope_relation(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key)
{
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
	open_table(heap, s, &s->rel, rel);
	check_cols(index, s);
	check_columns(*rel, s, &s->rel, member, scope_key);
}

/* The joined path and the members of its via column (hop 1, 2) with their columns; the checks of sift_scope_relation passed for the pairs relation */
static void
open_hop(Relation heap, const SiftScopeSpec *s, int hop, Relation *rel, AttrNumber *a, AttrNumber *b)
{
	open_table(heap, s, decl_rel(s, hop), rel);
	check_columns(*rel, s, decl_rel(s, hop), a, b);
}

/* The indexes this index depends on (normal edges to an index): the unique btrees and journal indexes of its scopes and the parent indexes of its connections */
static List *
dependent_indexes(Oid index)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *edges = NIL;

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
	table_close(drel, AccessShareLock);
	return edges;
}

/* The unique btree of rel that the index depends on and that still qualifies: the one that defined the pairs when the scope segments were written; InvalidOid when none */
static Oid
recorded_btree(Oid index, Relation rel, AttrNumber member, AttrNumber key)
{
	foreach_oid(edge, dependent_indexes(index))
	{
		Relation	idx;
		bool		ok;

		if (IndexGetRelation(edge, true) != RelationGetRelid(rel))
			continue;
		idx = index_open(edge, AccessShareLock);
		ok = is_pair_btree(idx, member, key);
		index_close(idx, AccessShareLock);
		if (ok)
			return edge;
	}
	return InvalidOid;
}

/*
 * The checks of one relation of a scope (path 0: the pairs; 1, 2: the joined path and the members of its via column) and the unique btree of it that defines its rows: the one the index
 * records (its dependency), never one chosen again by the rule: a btree made or rebuilt after the scope segments were written must not change which rows are pairs under them.
 * CREATE INDEX and REINDEX record it (sift_scope_resolve). The relation is left open in *rel.
 */
static Oid
path_index(Relation heap, Relation index, const SiftScopeSpec *s, int path, Relation *rel, AttrNumber *a, AttrNumber *b)
{
	const SiftScopeRel *r = decl_rel(s, path);
	Oid			btree;

	if (path == 0)
		sift_scope_relation(heap, index, s, rel, a, b);
	else
		open_hop(heap, s, path, rel, a, b);
	btree = recorded_btree(RelationGetRelid(index), *rel, *a, *b);
	if (!OidIsValid(btree))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": index \"%s\" has no recorded unique btree on (%s, %s) of \"%s\"", s->key_name, RelationGetRelationName(index), r->a, r->b, r->name),
						errhint("REINDEX INDEX %s records it again.", RelationGetRelationName(index))));
	return btree;
}

Oid
sift_scope_relation_index(Relation heap, Relation index, const SiftScopeSpec *s, Relation *rel, AttrNumber *member, AttrNumber *scope_key)
{
	return path_index(heap, index, s, 0, rel, member, scope_key);
}

Oid
sift_scope_hop_index(Relation heap, Relation index, const SiftScopeSpec *s, int hop, Relation *rel, AttrNumber *a, AttrNumber *b)
{
	Assert(s->joined != NULL && (hop == 1 || hop == 2));
	return path_index(heap, index, s, hop, rel, a, b);
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
	List	   *stale = NIL,
			   *present = NIL;
	ObjectAddress from;

	CommandCounterIncrement();
	foreach_oid(edge, dependent_indexes(index))
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
	CommandCounterIncrement();
}

/* Says which unique btree defines the rows of relation i of the declaration, and its predicate; a scope of two paths names the columns and what the rows are */
static void
notice_btree(const SiftScopeSpec *s, int i, Relation rel, Oid btree)
{
	static const char *const what[] = {"direct pairs", "joined pairs", "via members"};
	const SiftScopeRel *r = decl_rel(s, i);
	List	   *pred = index_predicate(btree);
	const char *text = pred == NIL ? NULL : deparse_expression((Node *) make_ands_explicit(pred), deparse_context_for(RelationGetRelationName(rel), RelationGetRelid(rel)), false, false);

	if (s->joined == NULL && pred == NIL)
		ereport(NOTICE, (errmsg("scope \"%s\": the pairs of \"%s\" are the rows of its unique btree \"%s\"", s->key_name, r->name, get_rel_name(btree))));
	else if (s->joined == NULL)
		ereport(NOTICE, (errmsg("scope \"%s\": the pairs of \"%s\" are the rows of its unique btree \"%s\" that satisfy %s", s->key_name, r->name, get_rel_name(btree), text)));
	else if (pred == NIL)
		ereport(NOTICE, (errmsg("scope \"%s\": the %s of \"%s\" (%s, %s) are the rows of its unique btree \"%s\"", s->key_name, what[i], r->name, r->a, r->b, get_rel_name(btree))));
	else
		ereport(NOTICE, (errmsg("scope \"%s\": the %s of \"%s\" (%s, %s) are the rows of its unique btree \"%s\" that satisfy %s", s->key_name, what[i], r->name, r->a, r->b, get_rel_name(btree), text)));
}

/* Is column a of relation x and column b of relation y the same column pair of the same relation, in either order */
static bool
same_columns(Relation x, AttrNumber xa, AttrNumber xb, Relation y, AttrNumber ya, AttrNumber yb)
{
	return RelationGetRelid(x) == RelationGetRelid(y) && ((xa == ya && xb == yb) || (xa == yb && xb == ya));
}

static void
check_same_type(const SiftScopeSpec *s, const char *role, Relation x, AttrNumber xa, Relation y, AttrNumber ya)
{
	Oid			tx = get_atttype(RelationGetRelid(x), xa),
				ty = get_atttype(RelationGetRelid(y), ya);

	if (tx != ty)
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("scope \"%s\": the %s columns \"%s\" of \"%s\" (%s) and \"%s\" of \"%s\" (%s) differ in type", s->key_name, role,
																	get_attname(RelationGetRelid(x), xa, false), RelationGetRelationName(x), format_type_be(tx),
																	get_attname(RelationGetRelid(y), ya, false), RelationGetRelationName(y), format_type_be(ty)),
						errhint("The columns a scope joins are of one integer type: change a column or the declaration.")));
}

/* The three relations of a scope of two paths with their columns: the paths are three different sets of columns, and what joins has one type */
static void
check_hops(const SiftScopeSpec *s, Relation *rel, const AttrNumber (*att)[2])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < i; j++)
			if (same_columns(rel[i], att[i][0], att[i][1], rel[j], att[j][0], att[j][1]))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the declaration reads \"%s\" (%s, %s) twice", s->key_name, decl_rel(s, i)->name, decl_rel(s, i)->a, decl_rel(s, i)->b),
								errhint("The direct pairs, the pairs by the via column and the members of the via column are three different sets of columns.")));
	check_same_type(s, "via", rel[1], att[1][0], rel[2], att[2][0]);
	check_same_type(s, "member", rel[0], att[0][0], rel[2], att[2][1]);
	check_same_type(s, "scope key", rel[0], att[0][1], rel[1], att[1][1]);
}

/* The unique btree that defines the rows of relation i of the declaration: the recorded one when sticky (and using, if it names one, must be it), else chosen by the rule, said in a NOTICE */
static Oid
choose_btree(Relation index, const SiftScopeSpec *s, int i, Relation rel, AttrNumber a, AttrNumber b, bool sticky)
{
	const SiftScopeRel *r = decl_rel(s, i);
	Oid			btree = sticky ? recorded_btree(RelationGetRelid(index), rel, a, b) : InvalidOid;

	if (sticky && r->index_name != NULL)
	{
		Oid			named = pair_index(rel, s, r, a, b);

		if (OidIsValid(btree) && named != btree)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the scope segments were written under the pairs of \"%s\", not \"%s\"", s->key_name, get_rel_name(btree), r->index_name),
							errhint("Change the btree that defines the pairs with DROP INDEX and CREATE INDEX of this index, or REINDEX after RESET (scope) and SET (scope = ..).")));
	}
	if (!OidIsValid(btree))
	{
		btree = pair_index(rel, s, r, a, b);
		if (!OidIsValid(btree))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": \"%s\" has no unique btree index on (%s, %s)", s->key_name, r->name, r->a, r->b),
							errhint("CREATE UNIQUE INDEX on %s (%s, %s), or make them its primary key: a plain, immediate, unique btree on exactly those two columns.", r->name, r->a, r->b)));
		notice_btree(s, i, rel, btree);
	}
	return btree;
}

/* The journal index of the pairs relation (of a scope of one path), which must hear of every pair */
static Oid
require_journal(const SiftScopeSpec *s, Relation rel, AttrNumber member, AttrNumber key, Oid btree)
{
	Oid			journal = sift_scope_journal_index(rel, member, key, btree);

	if (!OidIsValid(journal) && OidIsValid(journal_scan(rel, member, key, btree, true)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the journal index of \"%s\" is a partial index", s->key_name, s->rel.name),
						errhint("A partial index never hears of a pair inserted outside its predicate, unless the predicate of the unique btree of the pairs implies it: CREATE INDEX ON %s USING sift_col (%s, %s) WITH (track_changes = 'ctid') without a WHERE.",
								s->rel.name, s->rel.a, s->rel.b)));
	if (!OidIsValid(journal))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": \"%s\" has no sift_col index that journals its changes", s->key_name, s->rel.name),
						errhint("CREATE INDEX ON %s USING sift_col (%s, %s) WITH (track_changes = 'ctid'): the member column first, so that a read can tell which pairs changed since the scope segments were written.",
								s->rel.name, s->rel.a, s->rel.b)));
	return journal;
}

/*
 * CREATE INDEX, REINDEX and ALTER INDEX: finds the unique btree of each relation of each scope the option names (one for the pairs; a scope of two paths names three) and makes the dependency
 * of the index on it, so that DROP INDEX of the btree or DROP TABLE of the relation needs CASCADE (which drops this index) and a dump orders them first; removes the dependencies when the
 * option is off or not set. The dependency is the record of which btree defines the rows: CREATE INDEX and REINDEX (which write the scope segments) choose it by the rule and say which in a
 * NOTICE; ALTER INDEX (sticky) leaves the one recorded, as the segments were written under it. The edges of the btrees of one relation are told apart by their columns.
 */
void
sift_scope_resolve(Relation heap, Relation index, bool sticky)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	List	   *keep = NIL;

	if (specs != NULL && (!sift_col_is_index(index) || heap->rd_rel->relam != HEAP_TABLE_AM_OID))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope: the index must be a sift_col index on a heap table"),
						errhint("Declare a scope on a sift_col index of a heap table.")));
	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		const SiftScopeSpec *s = specs->s[i];
		int			npath = s->joined ? 3 : 1;
		Relation	rel[3];
		AttrNumber	att[3][2];
		Oid			btree[3];

		sift_scope_relation(heap, index, s, &rel[0], &att[0][0], &att[0][1]);
		for (int h = 1; h < npath; h++)
			open_hop(heap, s, h, &rel[h], &att[h][0], &att[h][1]);
		if (npath == 3)
			check_hops(s, rel, att);
		for (int h = 0; h < npath; h++)
		{
			btree[h] = choose_btree(index, s, h, rel[h], att[h][0], att[h][1], sticky);
			keep = list_append_unique_oid(keep, btree[h]);
		}
		/* a scope of two paths has no journal yet: its reads say so and read the entity rows */
		if (npath == 1)
			keep = list_append_unique_oid(keep, require_journal(s, rel[0], att[0][0], att[0][1], btree[0]));
		for (int h = 0; h < npath; h++)
			table_close(rel[h], NoLock);
	}
	sync_edges(RelationGetRelid(index), keep);
	list_free(keep);
}

/* After ALTER INDEX of any option of a sift_col index: the scopes follow the option */
void
sift_scope_alter(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	sift_scope_resolve(heap, index, true);
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

		/* the pairs of a scope of two paths are not the rows of one relation */
		if (s->joined != NULL || sift_conn_index_column(index, get_attnum(RelationGetRelid(heap), s->fk)) != fk ||
			sift_conn_relation_oid(RelationGetRelid(heap), s->rel.schema, s->rel.name, NoLock, false) != rel ||
			get_attnum(rel, s->rel.a) != member || get_attnum(rel, s->rel.b) != scope_key)
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

int
sift_scope_legs(Relation index, int scope, SiftScopeLeg legs[2])
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	const SiftScopeSpec *s;
	Relation	rel,
				via;
	SiftScopeLeg *l = &legs[0];

	Assert(specs != NULL && scope >= 0 && scope < specs->n);
	s = specs->s[scope];
	l->btree = sift_scope_relation_index(heap, index, s, &rel, &l->member, &l->key);
	l->rel = RelationGetRelid(rel);
	l->pred = index_predicate(l->btree);
	l->via_rel = InvalidOid;
	table_close(rel, NoLock);
	if (s->joined != NULL)
	{
		l = &legs[1];
		l->btree = sift_scope_hop_index(heap, index, s, 1, &rel, &l->member, &l->key);
		l->rel = RelationGetRelid(rel);
		l->pred = index_predicate(l->btree);
		l->via_btree = sift_scope_hop_index(heap, index, s, 2, &via, &l->via_join, &l->via_member);
		l->via_rel = RelationGetRelid(via);
		l->via_pred = index_predicate(l->via_btree);
		table_close(rel, NoLock);
		table_close(via, NoLock);
	}
	table_close(heap, NoLock);
	return s->joined != NULL ? 2 : 1;
}
