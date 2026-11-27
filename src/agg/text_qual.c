/*
 * agg/text_qual.c - a text qual decided on the bytes of the string (agg/text_qual.h).
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "agg/ht/ht.h"
#include "agg/text_qual.h"

static bool
is_text_type(Oid t)
{
	t = getBaseType(t);
	return t == TEXTOID || t == VARCHAROID;
}

static Node *
unrelabel(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

static void
literal_of(const Const *c, SiftTextQual *q)
{
	const text *t = DatumGetTextPP(c->constvalue);
	uint8	   *bytes = palloc(Max(VARSIZE_ANY_EXHDR(t), 1));

	q->len = VARSIZE_ANY_EXHDR(t);
	q->bytes = memcpy(bytes, VARDATA_ANY(t), q->len);
}

static bool
like_of(const Const *c, bool negate, SiftTextQual *q)
{
	SiftTextQual lit = {0};
	uint8	   *pattern;

	literal_of(c, &lit);
	pattern = palloc(Max(lit.len, 1));
	q->like = true;
	q->negate = negate;
	q->shape = sift_like_parse(lit.bytes, lit.len, pattern, &q->len);
	q->bytes = pattern;
	if (q->shape == SIFT_LIKE_CONTAINS)
		sift_like_needle_borrow(&q->needle, pattern, q->len, false);
	return q->shape != SIFT_LIKE_OTHER;
}

bool
sift_text_qual_of(const Node *clause, Index relid, SiftTextQual *q, const Var **column)
{
	const OpExpr *op;
	const char *name;
	Node	   *l,
			   *r;

	if (!IsA(clause, OpExpr) || list_length((op = (const OpExpr *) clause)->args) != 2 || get_func_namespace(get_opcode(op->opno)) != PG_CATALOG_NAMESPACE ||
		(name = get_opname(op->opno)) == NULL || !sift_text_bytes_equal(op->inputcollid))
		return false;
	l = unrelabel(linitial(op->args));
	r = unrelabel(lsecond(op->args));
	if (l == NULL || r == NULL || !IsA(l, Var) || !IsA(r, Const) || ((const Const *) r)->constisnull || ((const Const *) r)->consttype != TEXTOID)
		return false;
	*column = (const Var *) l;
	if ((*column)->varno != relid || (*column)->varlevelsup != 0 || (*column)->varattno <= 0 || !is_text_type((*column)->vartype))
		return false;
	*q = (SiftTextQual) {0};
	if (strcmp(name, "=") == 0 || strcmp(name, "<>") == 0)
	{
		q->negate = name[1] != '\0';
		literal_of((const Const *) r, q);
		return true;
	}
	return (strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0) && like_of((const Const *) r, name[0] == '!', q);
}

bool
sift_text_qual_holds(const SiftTextQual *q, const uint8 *p, uint32 len)
{
	bool		hit;

	if (!q->like)
		hit = len == q->len && memcmp(p, q->bytes, len) == 0;
	else
		switch (q->shape)
		{
			case SIFT_LIKE_CONTAINS:
				hit = sift_like_test(&q->needle, p, len) == SIFT_LIKE_YES;
				break;
			case SIFT_LIKE_PREFIX:
				hit = len >= q->len && memcmp(p, q->bytes, q->len) == 0;
				break;
			case SIFT_LIKE_SUFFIX:
				hit = len >= q->len && memcmp(p + len - q->len, q->bytes, q->len) == 0;
				break;
			default:
				hit = len == q->len && memcmp(p, q->bytes, len) == 0;
				break;
		}
	return hit != q->negate;
}
