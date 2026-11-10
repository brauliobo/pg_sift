/*
 * agg/plan_scope.c - the scope conjunct in the WHERE of a scan (col/scope.h, agg/scope.c)
 *
 * Until the planner recognises the joins that mean it (docs/INDEX-RECIPES.md), the test form is the marker sift.in_scope(index, key_name, fk, key or keys), or sift.in_scope_pairs for
 * the meaning of a join (a row once for each pair of its member with the keys): true for the rows
 * whose fk has a pair with the key (one of the keys of the array) in the scope relation the scope key_name of the index declares (its body, in the test flavour, reads that relation under the
 * snapshot, so a statement the scan does not take is exact as written). The scan takes it as the SiftAggScope of the spec when the index is the scanned one, the
 * scope is declared with that fk column and the key is a constant; anything else stays in the quals as an ordinary filter, and the spec says why (scope_why: EXPLAIN prints it as
 * Scope Fallback), so that a scope read is never lost without a word.
 *
 * Where the conjunct may come from (the rows of the scan before any join that keeps rows the scan does not read): the conditions inside the nullable input of an outer join (FromScan.scan_quals: a
 * sub-select's WHERE and the ON conjuncts that read only that input) filter its rows before the join, so a scan of that input takes them whatever the join keeps; the WHERE of the statement
 * (FromScan.quals) is a row filter of the whole result, exact for the scan unless an outer join of the scan can null its table (the conjunct then drops, or keeps, the rows the join made
 * of the other side's unmatched ones, which the scan does not read). The ON clause of a join that keeps the scanned rows filters the matches, not the rows.
 */
/* check-memory: allow-allocs the keys are the constants of one statement */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/plan_int.h"
#include "col/am.h"
#include "col/scope.h"

static bool
is_marker(Node *q)
{
	FuncExpr   *f;

	if (!IsA(q, FuncExpr))
		return false;
	f = (FuncExpr *) q;
	return list_length(f->args) == 4 && get_func_namespace(f->funcid) == get_namespace_oid("sift", true) &&
		(strcmp(get_func_name(f->funcid), "in_scope") == 0 || strcmp(get_func_name(f->funcid), "in_scope_pairs") == 0);
}

static bool
marker_walker(Node *n, void *ctx)
{
	return n != NULL && (is_marker(n) || expression_tree_walker(n, marker_walker, ctx));
}

static Const *
arg_const(FuncExpr *f, int i, Oid type)
{
	Node	   *a = strip_relabel(list_nth(f->args, i));

	return IsA(a, Const) && !((Const *) a)->constisnull && ((Const *) a)->consttype == type ? (Const *) a : NULL;
}

/* is key_name a scope of the index that holds its member key in the column of the index attribute att */
static bool
declared_on(Oid indexoid, const char *key_name, int att)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	SiftScopeSpecs *sp = sift_col_is_index(index) ? sift_scope_specs(index) : NULL;
	bool		found = false;

	for (int i = 0; sp != NULL && i < sp->n; i++)
		found |= strcmp(sp->s[i]->key_name, key_name) == 0 &&
			strcmp(sp->s[i]->fk, NameStr(TupleDescAttr(RelationGetDescr(index), att - 1)->attname)) == 0;
	index_close(index, NoLock);
	return found;
}

/* NULL, with the reason */
static SiftAggScope *
no_scope(const char **why, const char *reason)
{
	*why = reason;
	return NULL;
}

static SiftAggScope *
marker_scope(AggCtx *c, Node *q, const char **why)
{
	FuncExpr   *f = (FuncExpr *) q;
	Const	   *idx = arg_const(f, 0, REGCLASSOID),
			   *name = arg_const(f, 1, TEXTOID),
			   *key = arg_const(f, 3, INT8OID);
	const SiftSrcLeaf *lf;
	int			leaf;
	Datum	   *elems;
	bool	   *nulls;
	SiftAggScope *sc;

	if (key == NULL)
		key = arg_const(f, 3, INT8ARRAYOID);
	if (idx == NULL || name == NULL || key == NULL)
		return no_scope(why, "scope index, name or keys are not constants");
	if (DatumGetObjectId(idx->constvalue) != c->indexoid)
		return no_scope(why, "scope index is not the scanned index");
	if ((leaf = match_leaf(c, list_nth(f->args, 2))) < 0)
		return no_scope(why, "scope qual is not on the scanned relation");
	lf = leaf_of(c, leaf);
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->path != NIL || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64))
		return no_scope(why, "member key is not an integer column of the index");
	if (!declared_on(c->indexoid, TextDatumGetCString(name->constvalue), lf->attno))
		return no_scope(why, "no scope of that name is declared on the member key");
	sc = palloc0(sizeof(SiftAggScope));
	sc->key_name = TextDatumGetCString(name->constvalue);
	sc->fk = leaf;
	if (key->consttype == INT8OID)
	{
		sc->nkeys = 1;
		sc->keys = palloc(sizeof(int64));
		sc->keys[0] = DatumGetInt64(key->constvalue);
	}
	else
	{
		deconstruct_array(DatumGetArrayTypeP(key->constvalue), INT8OID, sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &elems, &nulls, &sc->nkeys);
		sc->keys = palloc(Max(sc->nkeys, 1) * sizeof(int64));
		for (int i = 0; i < sc->nkeys; i++)
		{
			if (nulls[i])
				return no_scope(why, "a scope key is NULL");
			sc->keys[i] = DatumGetInt64(elems[i]);
		}
	}
	sc->once = strcmp(get_func_name(f->funcid), "in_scope") == 0;
	use_leaf(c, leaf);
	return sc;
}

/* why a scan reads no scope at all; NULL when it may */
static const char *
scan_why(AggCtx *c)
{
	if (!sift_agg_scope)
		return "disabled";
	if (c->nparts > 0)
		return "partitioned scan";
	if (c->search != NULL)
		return "text search scan";
	return OidIsValid(c->indexoid) ? NULL : "scan of a table, not of a column index";
}

/*
 * The scope conjunct of quals into s->scope, out of the list; a conjunct that holds a marker and stays (the scan reads no scope in `held`'s words, it is not the conjunct itself,
 * a scope is read already, or marker_scope says why not) is the first reason of s->scope_why.
 */
static void
take_scope(AggCtx *c, SiftAggSpec *s, List **quals, const char *held)
{
	List	   *rest = NIL;

	foreach_ptr(Node, q, *quals)
	{
		const char *why = held;
		SiftAggScope *sc = NULL;

		if (!marker_walker(q, NULL))
		{
			rest = lappend(rest, q);
			continue;
		}
		if (why == NULL && !is_marker(q))
			why = "scope qual inside another expression";
		if (why == NULL && s->scope != NULL)
			why = "second scope qual: a scan reads one scope";
		if (why == NULL)
			sc = marker_scope(c, q, &why);
		if (sc != NULL)
		{
			s->scope = sc;
			continue;
		}
		if (s->scope_why == NULL)
			s->scope_why = why;
		rest = lappend(rest, q);
	}
	*quals = rest;
}

/* take_scope over the conjuncts of the scan that may hold one (see the head of the file); scan: why the scan reads no scope at all, or NULL */
static void
scope_scan(AggCtx *c, SiftAggSpec *s, FromScan *fs, const char *scan)
{
	const char *nulled = preserved_join(c) ? "scope qual under an outer join that can null it" : NULL;

	s->scope = NULL;
	s->scope_why = NULL;
	take_scope(c, s, &fs->scan_quals, scan);
	take_scope(c, s, &fs->quals, scan ? scan : nulled);
	foreach_ptr(JoinCtx, jc, c->joins)
		take_scope(c, s, &jc->clauses, scan ? scan : "scope qual in the ON clause of a join that keeps the scanned rows");
}

/* The scope conjunct of the scan, taken out of its conjuncts into s->scope; none the scan reads: they stay as filters and s->scope_why says why */
void
sift_agg_scope_extract(AggCtx *c, SiftAggSpec *s, FromScan *fs)
{
	scope_scan(c, s, fs, scan_why(c));
}

/* A scan that builds its spec in another way reads no scope: a conjunct that holds a marker stays a filter, and s->scope_why is `why` */
void
sift_agg_scope_declined(AggCtx *c, SiftAggSpec *s, FromScan *fs, const char *why)
{
	scope_scan(c, s, fs, why);
}
