/*
 * agg/plan_not.c - NOT, IS FALSE and IS NOT TRUE of the existential tests of a document or an array: the negation of a test, the complement of its bitmap, the
 * documents that are not NULL (the lengths of an array, the presence leaf of a jsonb document)
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"

/* the negation of an existential test of a document that is not NULL: an element test and a NULL test flip, an AND of them is the OR of their negations (and the reverse); false: a test of the cardinality or of the elements in order has none */
bool
negate_existential(SiftAggPred *p)
{
	if (p->kind == SAP_AND || p->kind == SAP_OR)
	{
		p->kind = p->kind == SAP_AND ? SAP_OR : SAP_AND;
		for (int i = 0; i < p->nchild; i++)
			if (!negate_existential(p->child[i]))
				return false;
	}
	else if (p->kind == SAP_CMP || (p->kind == SAP_ANY_ELEM && p->op == 2))
		return false;
	else if (p->kind == SAP_ANY_ELEM || p->kind == SAP_NULL)
		p->op ^= 1;
	else if (p->kind == SAP_IN)
		p->op = 1;
	else
		return false;
	return true;
}

/* target IS NOT NULL as a predicate (the lengths of an array, the presence leaf of a jsonb column); NULL when the index has none */
static SiftAggPred *
compile_not_null(AggCtx *c, Node *target)
{
	NullTest   *nt = makeNode(NullTest);
	SiftAggPred *nn;

	nt->arg = (Expr *) target;
	nt->nulltesttype = IS_NOT_NULL;
	nn = compile_pred(c, (Node *) nt);
	return nn != NULL && nn->level <= 1 ? nn : NULL;
}

/*
 * NOT (a && k), NOT (a @> k), NOT (a <@ k) of an array column that may be NULL, NOT (doc @> k) of a jsonb column that may be NULL: the test is NULL for a
 * NULL document, and NOT of it keeps no row, so the rows are those of a IS NOT NULL whose elements fail the test (a NULL element never matches, so the
 * test is never NULL for another array; a document is never NULL otherwise). NULL: the index has no way to tell a NULL document (neither the lengths
 * of an array nor the presence leaf of a jsonb document), or the test is not an existential one.
 */
SiftAggPred *
compile_not_array(AggCtx *c, BoolExpr *b, SiftAggPred *p, Node *target)
{
	SiftAggPred *nn,
			   *both;

	if ((!OidIsValid(get_element_type(exprType(target))) && present_leaf(c, target) < 0) || p->level > 1 || (nn = compile_not_null(c, target)) == NULL)
		return NULL;
	if (negate_existential(p))
		p->desc = psprintf("NOT %s", p->desc);
	else if ((p = compile_not_true(c, compile_existential(c, strip_relabel(linitial(b->args)), &target))) == NULL)	/* p is half flipped: the test again, whose complement is the rows that fail it */
		return NULL;
	both = new_junction(c, SAP_AND, 2);
	both->child[0] = nn;
	both->child[1] = p;
	junction_done(both);
	return both;
}

/* the kinds whose bitmap is exactly the rows they keep (the executor or the source evaluates them), so that the complement is the rows they do not keep */
#define BITMAP_KINDS ((1 << SAP_TRUE) | (1 << SAP_FALSE) | (1 << SAP_CMP) | (1 << SAP_IN) | (1 << SAP_NULL) | (1 << SAP_LUT) | (1 << SAP_NCMP) | (1 << SAP_LEAFCMP) | (1 << SAP_WIDE) | (1 << SAP_ANY_ELEM) | (1 << SAP_NOT_TRUE))

static bool
complementable(const SiftAggPred *p)
{
	if (p->level != 0 || p->late || p->pass_poison)
		return false;
	if (p->kind != SAP_AND && p->kind != SAP_OR)
		return (BITMAP_KINDS >> p->kind & 1) != 0;
	for (int i = 0; i < p->nchild; i++)
		if (!complementable(p->child[i]))
			return false;
	return true;
}

/* the rows p does not keep, a NULL result among them (IS NOT TRUE); NULL when its bitmap is not exactly the rows it keeps */
SiftAggPred *
compile_not_true(AggCtx *c, SiftAggPred *p)
{
	SiftAggPred *w;

	if (p == NULL || !complementable(p))
		return NULL;
	w = new_pred(c, SAP_NOT_TRUE);
	w->nchild = 1;
	w->child = palloc(sizeof(SiftAggPred *));
	w->child[0] = p;
	w->desc = psprintf("(%s) IS NOT TRUE", p->desc);
	return w;
}

/* is the predicate an AND / OR of the source's tests (SAP_WIDE, SAP_TYPE), the rows that pass of each a bitmap? */
bool
source_tests_only(const SiftAggPred *p)
{
	if (p->kind == SAP_WIDE || p->kind == SAP_TYPE)
		return true;
	if (p->kind != SAP_AND && p->kind != SAP_OR)
		return false;
	for (int i = 0; i < p->nchild; i++)
		if (!source_tests_only(p->child[i]))
			return false;
	return true;
}

/* the rows p does not keep (IS NOT TRUE: a NULL is one), of a tree of source tests: the dual junction over the complements of the tests, De Morgan on the sets of rows that pass */
void
complement_tests(SiftAggPred *p)
{
	if (p->kind == SAP_WIDE)
		p->negated = !p->negated;
	else if (p->kind == SAP_TYPE)
		p->op = SIFT_SRC_JT_ALL & ~p->op;	/* the types are all there are, a missing key and a NULL document among them */
	else
		p->kind = p->kind == SAP_AND ? SAP_OR : SAP_AND;
	for (int i = 0; i < p->nchild; i++)
		complement_tests(p->child[i]);
}

/*
 * NOT of a key test (doc ? 'k'): the test is NULL only for a NULL document, so the negation keeps the rows of the other types; a nullable document needs the
 * presence leaf of the document to be told from a missing key, and keeps the documents that are not NULL.
 */
SiftAggPred *
compile_not_haskey(AggCtx *c, BoolExpr *b)
{
	Node	   *t = strip_relabel(linitial(b->args));
	SiftAggPred *p = compile_haskey(c, t),
			   *nn = NULL,
			   *both;

	if (p == NULL || (!target_not_null(c, linitial(func_args(t))) && (nn = compile_not_null(c, linitial(func_args(t)))) == NULL))
		return NULL;
	complement_tests(p);
	p->desc = psprintf("NOT %s", p->desc);
	if (nn == NULL)
		return p;
	both = new_junction(c, SAP_AND, 2);
	both->child[0] = nn;
	both->child[1] = p;
	junction_done(both);
	return both;
}
