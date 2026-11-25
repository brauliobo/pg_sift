/*
 * agg/v6/v6_jsonpath.h - the literals and the pieces of a jsonpath filter that the tests of the scan of an unnest are made of (agg/v6/v6_unnest_path.c)
 */
#ifndef SIFT_AGG_V6_JSONPATH_H
#define SIFT_AGG_V6_JSONPATH_H

#include "postgres.h"

#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/numeric.h"

typedef struct Lit
{
	char		kind;			/* 's', 'n', 'b', 'z' (JSON null) */
	const char *s;
	int			len;
	Numeric		num;
	bool		b;
} Lit;

/* a JSON string, number, boolean or null as a literal an element leaf is compared with */
extern bool jsonb_lit(const JsonbValue *v, Lit *lit);
/* the pieces of a filter `@.key <op> literal` of a jsonpath: the SCMP_ operator of a comparison item (-1: not one), a literal or a variable of the vars constant, the name of the member @.key, the vars of a call (NULL: none; not ok: not a constant object) */
extern int	jsonpath_cmp_op(JsonPathItemType t);
extern bool jsonpath_lit(JsonPathItem *v, const Jsonb *vars, Lit *lit);
extern char *jsonpath_key(JsonPathItem *v);
extern const Jsonb *jsonpath_vars(Node *vars, bool *ok);

static inline Node *
strip_relabel(Node *n)
{
	while (n && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

static inline List *
func_args(Node *n)
{
	return IsA(n, FuncExpr) ? ((FuncExpr *) n)->args : IsA(n, OpExpr) ? ((OpExpr *) n)->args : NIL;
}

#endif							/* SIFT_AGG_V6_JSONPATH_H */
