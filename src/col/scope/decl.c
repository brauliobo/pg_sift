/*
 * col/scope/decl.c - the option scope of a sift_col index (col/scope/scope.h):
 *
 *     scope = 'key_name: fk -> [schema.]relation(member, scope_key); ...'
 *
 * The text is the record of the declaration, as for connect (col/v1/conn/decl.c): it is in pg_class.reloptions, so pg_dump writes it and a restore or REINDEX resolves it again.
 * Resolving finds the btree of the scope relation on exactly (member, scope_key) and makes the index depend on it.
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
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "utils/rls.h"
#include "utils/acl.h"
#include "utils/ruleutils.h"
#include "utils/fmgroids.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/scope/scope.h"
#include "col/keystore/keystore.h"
#include "core/meta_peek.h"
#include "core/ring.h"
#include "core/restrict.h"

bool		sift_agg_scope = true;
double		sift_scope_max_multiplicity = 3.0;
int			sift_col_scope_refresh_pct = 10;
#ifdef SIFT_TESTING
bool		sift_test_scope_patch = false;
#endif

/*
 * The checks of one column of a relation of the scope: it exists and is an integer, or a text, varchar or uuid column of a deterministic collation (col/scope/keys.c). A scope of one path needs
 * it NOT NULL (a pair has both keys); on two paths a row with a NULL in a column of its path is not a pair of that path, as a join drops it (a grant on a group has no member, a grant on a member no group).
 */
static AttrNumber
relation_column(Relation rel, const SiftScopeSpec *s, const SiftScopeRel *r, const char *name, bool via, bool key)
{
	AttrNumber	att = get_attnum(RelationGetRelid(rel), name);
	Form_pg_attribute a;
	const char *why;

	if (att == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope \"%s\": relation \"%s\" has no column \"%s\"", s->key_name, r->name, name)));
	a = TupleDescAttr(RelationGetDescr(rel), att - 1);
	if (sift_attr_is_virtual(a))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" of \"%s\" is a virtual generated column", s->key_name, name, r->name),
						errhint("A virtual column stores nothing for the scope to read: name a stored column.")));
	why = key && sift_scope_word_type(a->atttypid) ? NULL : sift_keymap_column_refusal(a->atttypid, a->attcollation);
	if (why != NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" of \"%s\" %s", s->key_name, name, r->name, why),
						errhint("The member and scope key columns are int2, int4, int8, text, varchar or uuid columns.")));
	if (via && !sift_conn_integer_type(a->atttypid))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the group column \"%s\" of \"%s\" is of type %s, which a scope of two paths does not take as a group", s->key_name, name, r->name,
																	   format_type_be(a->atttypid)),
						errhint("The group column that joins the two relations of the second path is an int2, int4 or int8 column; the member and the scope key may be text, varchar or uuid.")));
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
 * Does the index serve as the btree of the pairs (member, scope_key): an immediate btree on exactly those two columns, in either order, unique or not. Its predicate, if it has one, says which
 * rows are pairs (WHERE deleted_at IS NULL): PostgreSQL accepts only an immutable one. Unique, the rows that satisfy it are the pairs, one row each; not unique, a pair may be several rows (a
 * multiset: the pairs are the distinct (member, scope_key) of the rows) and the readers count it once.
 */
static bool
is_pair_btree(Relation idx, AttrNumber member, AttrNumber key)
{
	const FormData_pg_index *i = idx->rd_index;

	return idx->rd_rel->relam == BTREE_AM_OID && i->indimmediate && i->indisvalid && i->indisready && i->indnatts == 2 && i->indnkeyatts == 2 &&
		((i->indkey.values[0] == member && i->indkey.values[1] == key) || (i->indkey.values[0] == key && i->indkey.values[1] == member));
}

/*
 * The btree that defines the pairs of the relation, chosen when the scope segments are written (the index records it, see sift_scope_relation_index): the one named by using; else the unique
 * ones before the non-unique ones and among those the only partial one (its predicate is a statement of which rows are pairs), else the lowest oid of the plain ones. Two partial ones of the
 * kind that decides are ERROR: which rows are pairs would be a guess.
 */
static Oid
pair_index(Relation rel, const SiftScopeSpec *s, const SiftScopeRel *r, AttrNumber member, AttrNumber key)
{
	Oid			plain[2] = {InvalidOid, InvalidOid},
				partial[2] = {InvalidOid, InvalidOid};
	bool		two_partial[2] = {false, false};

	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		Relation	idx = index_open(ioid, AccessShareLock);
		bool		ok = is_pair_btree(idx, member, key),
					named = r->index_name != NULL && strcmp(RelationGetRelationName(idx), r->index_name) == 0,
					pred = !heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL);
		int			multiset = !idx->rd_index->indisunique;

		index_close(idx, AccessShareLock);
		if (named)
		{
			if (!ok)
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": index \"%s\" is not a valid immediate btree on exactly (%s, %s)", s->key_name, r->index_name, r->a, r->b),
								errhint("Name a btree of %s on its member and scope key columns, in either order; it may be unique or not, and partial.", r->name)));
			return ioid;
		}
		if (!ok || r->index_name != NULL)
			continue;
		if (!pred && !OidIsValid(plain[multiset]))
			plain[multiset] = ioid;
		else if (pred && OidIsValid(partial[multiset]))
			two_partial[multiset] = true;
		else if (pred)
			partial[multiset] = ioid;
	}
	if (r->index_name != NULL)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": \"%s\" has no index \"%s\"", s->key_name, r->name, r->index_name),
						errhint("USING names a btree of the scope relation.")));
	for (int m = 0; m < 2; m++)
	{
		if (two_partial[m])
			ereport(ERROR, (errcode(ERRCODE_AMBIGUOUS_PARAMETER), errmsg("scope \"%s\": \"%s\" has two partial %sbtrees on (%s, %s)", s->key_name, r->name, m ? "" : "unique ", r->a, r->b),
							errhint("The predicate of the btree says which rows are pairs: name the one that does with USING <index> after the columns.")));
		if (OidIsValid(partial[m]) || OidIsValid(plain[m]))
			return OidIsValid(partial[m]) ? partial[m] : plain[m];
	}
	return InvalidOid;
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
	*rel = table_open(roid, AccessShareLock);
	if ((*rel)->rd_rel->relkind != RELKIND_RELATION || (*rel)->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": \"%s\" is not a plain heap table", s->key_name, r->name),
						errhint("The scope relation is a heap table that is not partitioned: a view or a partitioned table is not supported.")));
	if ((*rel)->rd_rel->relpersistence == RELPERSISTENCE_TEMP && heap->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": \"%s\" is a temporary table", s->key_name, r->name),
						errdetail("The index depends on the btree and the journal of the scope relation, so it would be dropped with the table when the session ends."),
						errhint("Name a permanent table of pairs.")));
}

/* The two columns of the relation of the declaration: two columns; b is a scope key (the pairs, the joined path) or a member (the via relation) */
static void
check_columns(Relation rel, const SiftScopeSpec *s, const SiftScopeRel *r, AttrNumber *a, AttrNumber *b)
{
	/* of the two relations of the second path, the group (via) column is the first (a) */
	*a = relation_column(rel, s, r, r->a, r != &s->rel, false);
	*b = relation_column(rel, s, r, r->b, false, r == &s->rel);
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
	Form_pg_attribute fkatt;
	const char *why;

	if (fk == InvalidAttrNumber || sift_conn_index_column(index, fk) == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope \"%s\": \"%s\" is not a column of this index", s->key_name, s->fk),
						errhint("Name the column of the index that holds the member key.")));
	fkatt = TupleDescAttr(RelationGetDescr(heap), fk - 1);
	if ((why = sift_keymap_column_refusal(fkatt->atttypid, fkatt->attcollation)) != NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": column \"%s\" %s", s->key_name, s->fk, why),
						errhint("The member key is an int2, int4, int8, text, varchar or uuid column.")));
	named = get_attnum(RelationGetRelid(heap), s->key_name);
	if (named != InvalidAttrNumber && sift_conn_index_column(index, named) != InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN), errmsg("scope \"%s\": the name of the derived key is a column of this index", s->key_name),
						errhint("Give the derived key a name that no column of the index has: key_order names a column or a derived key.")));
	open_table(heap, s, &s->rel, rel);
	check_cols(index, s);
	check_columns(*rel, s, &s->rel, member, scope_key);
	if (!sift_keymap_same_kind(fkatt->atttypid, get_atttype(RelationGetRelid(*rel), *member)))
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("scope \"%s\": the member column \"%s\" of \"%s\" (%s) and the column \"%s\" of the index (%s) are not of one kind", s->key_name,
																   s->rel.a, s->rel.name, format_type_be(get_atttype(RelationGetRelid(*rel), *member)), s->fk, format_type_be(fkatt->atttypid)),
						errhint("Both are integers, both text or varchar, or both uuid: their values are equal when their bytes are.")));
}

/*
 * CREATE INDEX and REINDEX read every pair of the relations of a scope (the segments hold the rows by scope key, and the NOTICE and the size of the index count them), so the role that
 * builds needs what the join the index answers needs of its reader: SELECT on the columns of the pairs (the member, the scope key and what the predicate of the btree reads), and no
 * row-level security that hides rows from it (the segments would hold them). The readers are checked by the executor as for the join (the statement keeps the relation in its range table).
 */
static void
check_select(const SiftScopeSpec *s, Relation rel, AttrNumber att)
{
	Oid			role = GetUserId();
	const char *table = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)), RelationGetRelationName(rel));

	/* the executor's order: the privilege on the table, else the one on the column */
	if (pg_class_aclcheck(RelationGetRelid(rel), role, ACL_SELECT) == ACLCHECK_OK || pg_attribute_aclcheck(RelationGetRelid(rel), att, role, ACL_SELECT) == ACLCHECK_OK)
		return;
	ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					errmsg("scope \"%s\": permission denied for column \"%s\" of table \"%s\"", s->key_name, get_attname(RelationGetRelid(rel), att, false), RelationGetRelationName(rel)),
					errdetail("A scope index reads the pairs of \"%s\" when it is built (CREATE INDEX, REINDEX): the role that builds it needs SELECT on the columns the scope reads.", RelationGetRelationName(rel)),
					errhint("GRANT SELECT (%s) ON %s TO %s.", quote_identifier(get_attname(RelationGetRelid(rel), att, false)), table, quote_identifier(GetUserNameFromId(role, false)))));
}

static void
check_readable(const SiftScopeSpec *s, Relation rel, AttrNumber a, AttrNumber b)
{
	check_select(s, rel, a);
	check_select(s, rel, b);
	if (check_enable_rls(RelationGetRelid(rel), InvalidOid, true) == RLS_ENABLED)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("scope \"%s\": row-level security applies to \"%s\" for this role", s->key_name, RelationGetRelationName(rel)),
						errdetail("The segments of a scope index hold every pair of the relation, which the policies hide from this role."),
						errhint("Build the index with a role the policies do not apply to (the owner of the relation or a role with BYPASSRLS).")));
}

/* ... and of the columns that the predicate of the btree that defines the pairs reads */
static void
check_predicate(const SiftScopeSpec *s, Relation rel, Oid btree)
{
	Bitmapset  *atts = NULL;
	int			x = -1;

	pull_varattnos((Node *) sift_scope_index_predicate(btree), 1, &atts);
	while ((x = bms_next_member(atts, x)) >= 0)
		check_select(s, rel, x + FirstLowInvalidHeapAttributeNumber);
}

/* The joined path and the members of its via column (hop 1, 2) with their columns; the checks of sift_scope_relation passed for the pairs relation */
static void
open_hop(Relation heap, const SiftScopeSpec *s, int hop, Relation *rel, AttrNumber *a, AttrNumber *b)
{
	open_table(heap, s, decl_rel(s, hop), rel);
	check_columns(*rel, s, decl_rel(s, hop), a, b);
}

/* The indexes this index depends on (normal edges to an index): the btrees and journal indexes of its scopes and the parent indexes of its connections */
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

static Oid
find_recorded_btree(Oid index, Relation rel, AttrNumber member, AttrNumber key)
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
 * The btree of rel that the index depends on and that still qualifies: the one that defined the pairs when the scope segments were written; InvalidOid when none. A read asks for it for each relation it opens
 * and a statement several times, each a scan of pg_depend: the answer is remembered until the row in pg_class of the index or of the btree changes (REINDEX records the btree again and rewrites the row, ALTER INDEX too,
 * and a btree that REINDEX CONCURRENTLY replaces is dropped).
 */
#define BTREE_MEMO_N 8
static struct
{
	Oid			index;
	uint32		index_hash;		/* of its pg_class row in the RELOID syscache, and of the btree's: REINDEX CONCURRENTLY of the btree replaces it under the index, which names the old one until it is dropped */
	uint32		btree_hash;
	Oid			rel;
	AttrNumber	member;
	AttrNumber	key;
	Oid			btree;
} btree_memo[BTREE_MEMO_N];
static SiftRing btree_memo_ring;
static bool btree_memo_hooked;

static void
btree_memo_inval(Datum arg, int cacheid, uint32 hashvalue)
{
	for (int i = 0; i < BTREE_MEMO_N; i++)
		if (hashvalue == 0 || btree_memo[i].index_hash == hashvalue || btree_memo[i].btree_hash == hashvalue)
			btree_memo[i].index = InvalidOid;
}

static Oid
recorded_btree(Oid index, Relation rel, AttrNumber member, AttrNumber key)
{
	Oid			btree;

	for (int i = 0; i < BTREE_MEMO_N; i++)
		if (btree_memo[i].index == index && btree_memo[i].rel == RelationGetRelid(rel) && btree_memo[i].member == member && btree_memo[i].key == key)
			return btree_memo[i].btree;
	btree = find_recorded_btree(index, rel, member, key);
	if (!OidIsValid(btree))
		return btree;
	if (!btree_memo_hooked)
	{
		CacheRegisterSyscacheCallback(RELOID, btree_memo_inval, (Datum) 0);
		btree_memo_hooked = true;
	}
	btree_memo[SIFT_RING_SLOT(btree_memo_ring, BTREE_MEMO_N)] = (typeof(btree_memo[0])) {index, GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(index)), GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(btree)),
															   RelationGetRelid(rel), member, key, btree};
	return btree;
}

Oid
sift_scope_fact_oid(Relation owner)
{
	return owner->rd_index != NULL ? owner->rd_index->indrelid : RelationGetRelid(owner);
}

/*
 * The btree of the pairs of a scope of a sift_table. Nothing records it (dropping the btree of a relation the table reads must not drop the table), so only a btree whose rows are the pairs whichever
 * it is can be taken: a valid immediate unique one without a predicate, the named one when the declaration names one. The relation is left open in *rel when there is one; else nothing is.
 */
static Oid
table_pairs_btree(Relation fact, const SiftScopeSpec *s, Relation *rel, AttrNumber *a, AttrNumber *b)
{
	Oid			roid = sift_conn_relation_oid(RelationGetRelid(fact), s->rel.schema, s->rel.name, AccessShareLock, true);
	Oid			btree = InvalidOid;

	*rel = NULL;
	if (!OidIsValid(roid) || roid == RelationGetRelid(fact))
		return InvalidOid;
	*rel = table_open(roid, AccessShareLock);
	*a = get_attnum(roid, s->rel.a);
	*b = get_attnum(roid, s->rel.b);
	if ((*rel)->rd_rel->relkind == RELKIND_RELATION && (*rel)->rd_rel->relam == HEAP_TABLE_AM_OID && *a > 0 && *b > 0 && *a != *b &&
		TupleDescAttr((*rel)->rd_att, *a - 1)->attnotnull && TupleDescAttr((*rel)->rd_att, *b - 1)->attnotnull)
		foreach_oid(ioid, RelationGetIndexList(*rel))
		{
			Relation	idx = index_open(ioid, AccessShareLock);
			bool		plain = is_pair_btree(idx, *a, *b) && idx->rd_index->indisunique && heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL) &&
				(s->rel.index_name == NULL || strcmp(RelationGetRelationName(idx), s->rel.index_name) == 0);

			index_close(idx, AccessShareLock);
			if (plain)
			{
				btree = ioid;
				break;
			}
		}
	if (!OidIsValid(btree))
	{
		table_close(*rel, NoLock);
		*rel = NULL;
	}
	return btree;
}

/*
 * The checks of one relation of a scope (path 0: the pairs; 1, 2: the joined path and the members of its via column) and the btree of it that defines its rows: the one the index
 * records (its dependency), never one chosen again by the rule: a btree made or rebuilt after the scope segments were written must not change which rows are pairs under them.
 * CREATE INDEX and REINDEX record it (sift_scope_resolve). The relation is left open in *rel. A sift_table has no record: table_pairs_btree.
 */
static Oid
path_index(Relation heap, Relation index, const SiftScopeSpec *s, int path, Relation *rel, AttrNumber *a, AttrNumber *b)
{
	const SiftScopeRel *r = decl_rel(s, path);
	Oid			btree;

	if (index->rd_index == NULL)
	{
		*rel = NULL;
		return path == 0 ? table_pairs_btree(heap, s, rel, a, b) : InvalidOid;
	}
	if (path == 0)
		sift_scope_relation(heap, index, s, rel, a, b);
	else
		open_hop(heap, s, path, rel, a, b);
	btree = recorded_btree(RelationGetRelid(index), *rel, *a, *b);
	if (!OidIsValid(btree))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": index \"%s\" has no recorded btree on (%s, %s) of \"%s\"", s->key_name, RelationGetRelationName(index), r->a, r->b, r->name),
						errhint("REINDEX INDEX %s records it again.", sift_col_index_ref(index))));
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

/* Is the sift_col index a scope relation's journal index (it tracks the addresses of the rows, as opposed to the parent index of a connection, which tracks a key column)? */
static bool
is_journal_index(Oid oid)
{
	Relation	idx = index_open(oid, AccessShareLock);
	bool		ctid = sift_col_track_ctid(idx) && !sift_col_row_key_on(idx);

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
		sift_undepend(RelationRelationId, index, DEPENDENCY_NORMAL, RelationRelationId, old);
	ObjectAddressSet(from, RelationRelationId, index);
	foreach_oid(btree, keep)
		if (!list_member_oid(present, btree))
		{
			ObjectAddress to;

			ObjectAddressSet(to, RelationRelationId, btree);
			sift_depend(&from, &to, DEPENDENCY_NORMAL);
		}
	CommandCounterIncrement();
}

static bool
btree_unique(Oid btree)
{
	Relation	idx = index_open(btree, AccessShareLock);
	bool		unique = idx->rd_index->indisunique;

	index_close(idx, AccessShareLock);
	return unique;
}

/* Says which btree defines the rows of relation i of the declaration, and its predicate; a scope of two paths names the columns and what the rows are */
static void
notice_btree(const SiftScopeSpec *s, int i, Relation rel, Oid btree)
{
	static const char *const what[] = {"direct pairs", "joined pairs", "via relation"};
	const SiftScopeRel *r = decl_rel(s, i);
	List	   *pred = sift_scope_index_predicate(btree);
	const char *text = pred == NIL ? NULL : deparse_expression((Node *) make_ands_explicit(pred), deparse_context_for(RelationGetRelationName(rel), RelationGetRelid(rel)), false, false);

	if (!btree_unique(btree))
		ereport(NOTICE, (errmsg("scope \"%s\": the %s of \"%s\" (%s, %s) are the distinct rows of its non-unique btree \"%s\"%s", s->key_name, s->joined == NULL ? "pairs" : what[i], r->name, r->a, r->b,
								get_rel_name(btree), text == NULL ? "" : psprintf(" that satisfy %s", text)),
						 errdetail("A pair may be several rows: a read with the meaning of IN or EXISTS counts a fact once, a join counts it once for each row and runs as written.")));
	else if (s->joined == NULL && pred == NIL)
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
						errhint("The columns a scope joins are of one type: change a column or the declaration.")));
}

/* The three relations of a scope of two paths with their columns: the paths are three different sets of columns, and what joins has one type */
static void
check_hops(const SiftScopeSpec *s, Relation *rel, const AttrNumber (*att)[2])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < i; j++)
			if (same_columns(rel[i], att[i][0], att[i][1], rel[j], att[j][0], att[j][1]))
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the declaration reads \"%s\" (%s, %s) twice", s->key_name, decl_rel(s, i)->name, decl_rel(s, i)->a, decl_rel(s, i)->b),
								errhint("The direct pairs, the pairs by the via column and the via relation are three different sets of columns.")));
	check_same_type(s, "via", rel[1], att[1][0], rel[2], att[2][0]);
	check_same_type(s, "member", rel[0], att[0][0], rel[2], att[2][1]);
	check_same_type(s, "scope key", rel[0], att[0][1], rel[1], att[1][1]);
}

/*
 * The btree that defines the rows of relation i of the declaration: the recorded one, whatever the command (and using, if it names one, must be it). ALTER INDEX leaves it, and REINDEX writes
 * the scope segments under it, so a btree made since (the rule prefers a partial one) cannot change what the index means. CREATE INDEX, and the first scope of an ALTER INDEX, have none
 * recorded and choose by the rule; they say which in a NOTICE, as REINDEX does. The recorded btree is always there: the index depends on it, so DROP INDEX of it drops the index, and a
 * btree that REINDEX CONCURRENTLY replaces is the one that depends now.
 */
static Oid
choose_btree(Oid owner, const SiftScopeSpec *s, int i, Relation rel, AttrNumber a, AttrNumber b, bool sticky)
{
	const SiftScopeRel *r = decl_rel(s, i);
	Oid			recorded = recorded_btree(owner, rel, a, b),
				btree = recorded;

	if (OidIsValid(btree) && r->index_name != NULL && pair_index(rel, s, r, a, b) != btree)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope \"%s\": the scope segments were written under the pairs of \"%s\", not \"%s\"", s->key_name, get_rel_name(btree), r->index_name),
						errhint("Change the btree that defines the pairs with DROP INDEX and CREATE INDEX of this index, or make the index again without the scope and with it.")));
	if (!OidIsValid(btree))
	{
		btree = pair_index(rel, s, r, a, b);
		if (!OidIsValid(btree))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("scope \"%s\": \"%s\" has no btree index on (%s, %s)", s->key_name, r->name, r->a, r->b),
							errhint("CREATE UNIQUE INDEX on %s (%s, %s), or make them its primary key: an immediate btree on exactly those two columns (a non-unique one makes the pairs a multiset of rows).", r->name, r->a, r->b)));
	}
	if (!sticky || !OidIsValid(recorded))
		notice_btree(s, i, rel, btree);
	return btree;
}

/* The journal index of the pairs relation (of a scope of one path), which must hear of every pair: the one the relation has, else the one CREATE INDEX makes (sift_scope_journal_make) */
static Oid
require_journal(const SiftScopeSpec *s, Relation rel, AttrNumber member, AttrNumber key, Oid btree, SiftJournalMake make)
{
	Oid			journal = sift_scope_journal_index(rel, member, key, btree);

	return OidIsValid(journal) || make == SIFT_JOURNAL_DEFER ? journal : sift_scope_journal_make(s, rel, 0, member, key, btree, make);
}

/* The journal indexes of the three relations of a scope of two paths (added to keep), which must hear of every pair: the ones the relations have, else the ones CREATE INDEX makes */
static void
require_journals(const SiftScopeSpec *s, Relation *rel, const AttrNumber (*att)[2], const Oid *btree, SiftJournalMake make, List **keep)
{
	for (int h = 0; h < 3; h++)
	{
		Oid			journal = sift_scope_hop_journal(rel[h], h, att[h][0], att[h][1], btree[h]);

		if (!OidIsValid(journal) && make != SIFT_JOURNAL_DEFER)
			journal = sift_scope_journal_make(s, rel[h], h, att[h][h == 2 ? 1 : 0], att[h][h == 2 ? 0 : 1], btree[h], make);
		if (OidIsValid(journal))
			*keep = list_append_unique_oid(*keep, journal);
	}
}

/*
 * The limit of each scope that states none, max_multiplicity(n) in its declaration, is the session's sift.scope_max_multiplicity at CREATE INDEX, written into the option so that it is the limit the index
 * was accepted under: REINDEX, a restore of a dump and the rebuild after pg_upgrade (which take no session setting) check the same one, and pay no build for a refusal.
 */
static void
record_limits(Relation index, const SiftScopeSpecs *specs)
{
	StringInfoData text;
	bool		missing = false;

	initStringInfo(&text);
	for (int i = 0; i < specs->n; i++)
	{
		appendStringInfo(&text, "%s%s", i > 0 ? "; " : "", specs->s[i]->text);
		if (specs->s[i]->max_multiplicity == 0)
		{
			appendStringInfo(&text, " max_multiplicity(%g)", sift_scope_max_multiplicity);
			missing = true;
		}
	}
	if (missing)
		sift_col_set_option(index, "scope", text.data);
}

/*
 * CREATE INDEX, REINDEX and ALTER INDEX: finds the btree of each relation of each scope the option names (one for the pairs; a scope of two paths names three) and makes the dependency
 * of the index on it, so that DROP INDEX of the btree or DROP TABLE of the relation needs CASCADE (which drops this index) and a dump orders them first; removes the dependencies when the
 * option is off or not set. The dependency is the record of which btree defines the rows: CREATE INDEX and REINDEX (which write the scope segments) choose it by the rule and say which in a
 * NOTICE; ALTER INDEX (sticky) leaves the one recorded, as the segments were written under it. The edges of the btrees of one relation are told apart by their columns.
 */
void
sift_scope_resolve(Relation heap, Relation index, bool sticky, SiftJournalMake make)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	Oid			origin = specs != NULL ? sift_col_swap_origin(heap, index, false) : InvalidOid;
	Oid			owner = OidIsValid(origin) ? origin : RelationGetRelid(index);
	List	   *keep = NIL;

	if (specs != NULL && (!sift_col_is_index(index) || heap->rd_rel->relam != HEAP_TABLE_AM_OID))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("scope: the index must be a sift_col index on a heap table"),
						errhint("Declare a scope on a sift_col index of a heap table.")));
	if (specs != NULL && !sticky && heap->rd_rel->relispartition)
		ereport(NOTICE, (errmsg("scope: \"%s\" is a partition: a statement over the partitioned table reads no scope", RelationGetRelationName(heap)),
						 errdetail("A statement on this partition reads the scope units of its index; the scope index of each partition is declared on that partition.")));
	for (int i = 0; specs != NULL && i < specs->n; i++)
	{
		const SiftScopeSpec *s = specs->s[i];
		int			npath = s->joined ? 3 : 1;
		Relation	rel[3];
		AttrNumber	att[3][2];
		Oid			btree[3],
					journal;

		sift_scope_relation(heap, index, s, &rel[0], &att[0][0], &att[0][1]);
		for (int h = 1; h < npath; h++)
			open_hop(heap, s, h, &rel[h], &att[h][0], &att[h][1]);
		if (npath == 3)
			check_hops(s, rel, att);
		for (int h = 0; !sticky && h < npath; h++)
			check_readable(s, rel[h], att[h][0], att[h][1]);
		for (int h = 0; !sticky && h < npath; h++)
			sift_scope_notice_relation(s->key_name, decl_rel(s, h)->name, rel[h]);
		for (int h = 0; h < npath; h++)
		{
			btree[h] = choose_btree(owner, s, h, rel[h], att[h][0], att[h][1], sticky);
			if (!sticky)
				check_predicate(s, rel[h], btree[h]);
			keep = list_append_unique_oid(keep, btree[h]);
		}
		/* the codes of a text or uuid scope key are those of the scope key store of its column: the index depends on it */
		if (sift_keymap_type(get_atttype(RelationGetRelid(rel[0]), att[0][1])))
			(void) sift_keystore_ensure(rel[0], index, att[0][1]);
		/* a scope needs its journal, a scope of two paths the three of its relations */
		if (npath == 1 && OidIsValid(journal = require_journal(s, rel[0], att[0][0], att[0][1], btree[0], make)))
			keep = list_append_unique_oid(keep, journal);
		if (npath == 3)
			require_journals(s, rel, att, btree, make, &keep);
		for (int h = 0; h < npath; h++)
			table_close(rel[h], NoLock);
	}
	sync_edges(RelationGetRelid(index), keep);
	list_free(keep);
	if (specs != NULL && !sticky)
		record_limits(index, specs);
}

/* After ALTER INDEX of any option of a sift_col index: the scopes follow the option */
void
sift_scope_alter(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	if (sift_meta_resolve_major(index) != SIFT_FORMAT2_MAJOR && sift_scope_stamp_key(index))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a sift_col index of format major 1 has no scope keyed by a date or a timestamp"),
						errhint("REINDEX the index in format major 2, then declare the scope.")));
	sift_scope_resolve(heap, index, true, SIFT_JOURNAL_PLAIN);
	sift_scope_journal_drop_unused();
	sift_scope_start(heap, index, false);
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

/* Is the column of the pairs relation of the scope (its scope key, or its member) text, varchar or uuid */
static bool
column_is_text(Relation index, const SiftScopeSpec *s, bool key)
{
	Oid			rel = sift_conn_relation_oid(index->rd_index->indrelid, s->rel.schema, s->rel.name, NoLock, true);

	return OidIsValid(rel) && sift_keymap_type(get_atttype(rel, get_attnum(rel, key ? s->rel.b : s->rel.a)));
}

static bool
key_is_coded(Relation index, const SiftScopeSpec *s)
{
	return column_is_text(index, s, true);
}

bool
sift_scope_coded(Relation index)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);

	for (int c = 0; specs != NULL && c < specs->n; c++)
		if (key_is_coded(index, specs->s[c]) || column_is_text(index, specs->s[c], false))
			return true;
	return false;
}

/* Does the index declare a scope whose key is a date, timestamp or timestamptz column: format major 2 summarizes it, format major 1 does not */
bool
sift_scope_stamp_key(Relation index)
{
	SiftScopeSpecs *specs = sift_scope_specs(index);

	for (int c = 0; specs != NULL && c < specs->n; c++)
	{
		const SiftScopeSpec *s = specs->s[c];
		Oid			rel = sift_conn_relation_oid(index->rd_index->indrelid, s->rel.schema, s->rel.name, NoLock, true);
		Oid			type = OidIsValid(rel) ? get_atttype(rel, get_attnum(rel, s->rel.b)) : InvalidOid;

		if (sift_scope_word_type(type) && !sift_conn_integer_type(type))
			return true;
	}
	return false;
}

bool
sift_scope_member_text(Relation index, int scope)
{
	return column_is_text(index, sift_scope_specs(index)->s[scope], false);
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
		defs[n] = (SiftColDef) {.id = n,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = SIFT_COLS_INT,.flags = SIFT_COLD_SCOPE | (key_is_coded(index, s) || column_is_text(index, s, false) ? SIFT_COLD_CODED : 0),.attno = fk,
			.type = SIFT_COLT_INT8,.path = psprintf("%s#scope", s->key_name)};
		n++;
		(*nscope)++;
	}
	if (*nscope > 0 && n > SIFT_COL_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("an index with a scope holds at most %d columns", SIFT_COL_MAX_COLS),
						errhint("Leave out the columns the queries do not read, or create several indexes.")));
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

bool
sift_scope_pairs_multiset(Relation index, const SiftScopeSpec *s)
{
	Oid			relid;
	Relation	rel;
	Oid			btree;

	/* the pairs of a scope of two paths are defined as the distinct union: a read for each pair is exact whatever the relations hold */
	if (s->joined != NULL)
		return false;
	if (index->rd_index == NULL)
	{
		AttrNumber	a,
					b;

		btree = table_pairs_btree(index, s, &rel, &a, &b);
		if (rel != NULL)
			table_close(rel, NoLock);
		return !OidIsValid(btree);
	}
	relid = sift_conn_relation_oid(index->rd_index->indrelid, s->rel.schema, s->rel.name, AccessShareLock, false);
	rel = table_open(relid, AccessShareLock);
	btree = recorded_btree(RelationGetRelid(index), rel, get_attnum(relid, s->rel.a), get_attnum(relid, s->rel.b));
	table_close(rel, NoLock);
	return !OidIsValid(btree) || !btree_unique(btree);
}

int
sift_scope_legs_find(Relation index, int scope, SiftScopeLeg legs[2])
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	Relation	heap = table_open(sift_scope_fact_oid(index), AccessShareLock);
	const SiftScopeSpec *s;
	Relation	rel,
				via;
	SiftScopeLeg *l = &legs[0];

	Assert(specs != NULL && scope >= 0 && scope < specs->n);
	s = specs->s[scope];
	if (index->rd_index == NULL && s->joined != NULL)
	{
		table_close(heap, NoLock);
		return 0;
	}
	l->btree = sift_scope_relation_index(heap, index, s, &rel, &l->member, &l->key);
	if (!OidIsValid(l->btree))
	{
		table_close(heap, NoLock);
		return 0;
	}
	l->rel = RelationGetRelid(rel);
	l->pred = sift_scope_index_predicate(l->btree);
	l->via_rel = InvalidOid;
	table_close(rel, NoLock);
	if (s->joined != NULL)
	{
		l = &legs[1];
		l->btree = sift_scope_hop_index(heap, index, s, 1, &rel, &l->member, &l->key);
		l->rel = RelationGetRelid(rel);
		l->pred = sift_scope_index_predicate(l->btree);
		l->via_btree = sift_scope_hop_index(heap, index, s, 2, &via, &l->via_join, &l->via_member);
		l->via_rel = RelationGetRelid(via);
		l->via_pred = sift_scope_index_predicate(l->via_btree);
		table_close(rel, NoLock);
		table_close(via, NoLock);
	}
	table_close(heap, NoLock);
	return s->joined != NULL ? 2 : 1;
}
