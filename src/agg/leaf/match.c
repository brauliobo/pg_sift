/*
 * agg/leaf/match.c - which leaf an expression reads (match_leaf) and the typed members of a document
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"

#include "agg/leaf/int.h"

/*
 * The leaf an expression reads, -1 if it is not exactly a stored value: an attribute or index expression, a Var of the
 * unnested function's row type, a typed jsonb path of either, or the length of an array leaf.
 */
static int
match_leaf_node(AggCtx *c, Node *e)
{
	JsonRef		r = {0};
	Oid			fid,
				cast_to = InvalidOid;
	Node	   *inner;
	int			att;

	c->rec_pending = 0;
	c->cast_pending = 0;
	c->txt_pending = 0;
	c->io_pending = 0;
	e = strip_relabel(e);
	if (IsA(e, Var) && ((Var *) e)->varno == c->func_rti && c->func_rti != 0)
		return func_var_leaf(c, (Var *) e);
	if ((att = attr_of_expr(c, e)) > 0)
		return scalar_leaf(c, att, NIL, exprType(e));
	fid = func_of(e);
	if (fid == F_JSONB_ARRAY_LENGTH)
		return array_length_leaf(c, e);
	if (fid == F_CARDINALITY || fid == F_ARRAY_LENGTH)
		return array_lane_leaf(c, e, fid);
	if (fid == F_JSONB_TYPEOF)
		return types_leaf(c, e);
	if (fid == F_LOWER_ANYRANGE || fid == F_UPPER_ANYRANGE)
		return bound_leaf(c, e, fid == F_LOWER_ANYRANGE ? 1 : 2);
	inner = e;
	if (IsA(e, CoerceViaIO))
	{
		cast_to = exprType(e);
		inner = strip_relabel((Node *) ((CoerceViaIO *) e)->arg);
	}
	else if (json_cast_target(fid) != InvalidOid)
	{
		cast_to = json_cast_target(fid);
		inner = strip_relabel(linitial(func_args(e)));
	}
	if (cast_to != InvalidOid && cast_to == exprType(e) && IsA(inner, Var) && ((Var *) inner)->varno == c->func_rti &&
		c->func_rti != 0 && c->func_kind == FK_EACH_TEXT && ((Var *) inner)->varattno == 2 && IsA(e, CoerceViaIO))
		return c->group < 0 ? -1 : child_leaf(c, c->group, "value", cast_to);
	if (!json_access(c, inner, &r))
		return -1;
	if (IsA(inner, Var) || attr_of_expr(c, inner) > 0)
		return each_value_cast_leaf(c, e, fid, cast_to, &r);
	return json_path_leaf(c, e, fid, cast_to, &r);
}

/* match_leaf_node, noting the leaves read by the cast function of jsonb: before PostgreSQL 18 it raises for a JSON null (reads_json_null_cast) */
int
match_leaf(AggCtx *c, Node *e)
{
	int			leaf = match_leaf_node(c, e);

#if SIFT_JSON_NULL_CAST_RAISES
	if (leaf >= 0 && json_cast_target(func_of(strip_relabel(e))) != InvalidOid)
		c->jcast = bms_add_member(c->jcast, leaf);
#endif
	return leaf;
}

/*
 * `x->>'k'` of a path whose leaf has another type than text (`attributes->normalized bool`, the member `score float8` of the elements of
 * `jsonb_array_elements`) as the typed access `(x->>'k')::bool`, which match_leaf knows; NULL when it is none. `->>` turns a JSON null into NULL, so
 * the text and the leaf are both NULL for a missing key and a JSON null, both not NULL for a value (a value the leaf could not hold makes the scan
 * raise the overflow error instead of being a non-NULL text, or keeps the normal plan: reads_overflow).
 */
Node *
typed_member(AggCtx *c, Node *arg)
{
	JsonRef		r = {0};

	arg = strip_relabel(arg);
	if (exprType(arg) != TEXTOID || (func_of(arg) != F_JSONB_OBJECT_FIELD_TEXT && func_of(arg) != F_JSONB_EXTRACT_PATH_TEXT) || !json_access(c, arg, &r) ||
		!r.as_text)
		return NULL;
	if (r.att > 0 && scalar_leaf(c, r.att, r.path, TEXTOID) >= 0)
		return NULL;
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);
		bool		hit = r.att > 0 ? l->attno == r.att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && paths_equal(l->path, r.path) :
			r.att == 0 && c->group >= 0 && c->func_kind == FK_ELEMENTS && r.fcol == 1 && list_length(r.path) == 1 && l->parent == c->group && !l->lossy &&
			l->typid != TEXTOID && strcmp(l->name, strVal(linitial(r.path))) == 0;

		if (hit)
		{
			CoerceViaIO *io = makeNode(CoerceViaIO);

			io->arg = (Expr *) arg;
			io->resulttype = l->typid;
			io->coerceformat = COERCE_EXPLICIT_CAST;
			io->location = -1;
			return (Node *) io;
		}
	}
	return NULL;
}
