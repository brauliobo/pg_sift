/*
 * agg/unnest/members.c - jsonb_array_elements(doc) or (doc -> 'key') AS e over a list of objects the index keeps shredded (`doc.key[]{k1 t1, ..}`), read by the members of its elements, `e ->> 'k'`
 * and `(e ->> 'k')::type`, as the aggregate scan of an unnest takes it (agg/unnest/jsonb.h, SV6E_MEMBERS): each member the statement reads is a column of the element, the leaf the same member of a
 * jsonb_to_recordset is. A row whose list the leaves cannot hold is read from the heap and taken apart by the function, the members read as the statement does: the text of the member, and the input
 * function of the cast, whose error is the one of the pair of that element (the late values of agg/unnest/jsonb.h). The same function in the sub-select of an EXISTS reads its stream of the Ext the
 * same way (V6Plan.elem_stream).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/unnest/jsonb.h"
#include "col/am.h"

bool
sift_v6_members_rte(const RangeTblFunction *rtf, const Expr *v, SiftV6ElemRte *out)
{
	sift_v6_unnest_array_rte(v, out);
	out->fn = SV6E_MEMBERS;
	return exprType((const Node *) v) == JSONBOID && rtf->funccolcount == 1;
}

bool
sift_v6_members_seed(const SiftV6Leaves *r, SiftV6XElem *e, const char *key, int att)
{
	if (!r->json[att] || (e->group_leaf = sift_v6_unnest_group_of(r, att, key)) < 0)
		return false;
	e->ncols = 0;
	return true;
}

/* e ->> 'name' of the element of the lateral relation: the name */
static const char *
member_name(const V6Plan *p, const Node *n)
{
	const OpExpr *op = (const OpExpr *) n;
	const Var  *v;
	const Const *k;

	if (!IsA(n, OpExpr) || get_opcode(op->opno) != F_JSONB_OBJECT_FIELD_TEXT || list_length(op->args) != 2 || !IsA(linitial(op->args), Var) || !IsA(lsecond(op->args), Const))
		return NULL;
	v = linitial(op->args);
	k = lsecond(op->args);
	if (v->varno != p->elem_rel->relid || v->varlevelsup != 0 || v->varattno != 1 || k->constisnull || k->consttype != TEXTOID)
		return NULL;
	return TextDatumGetCString(k->constvalue);
}

static bool
cast_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID || t == FLOAT8OID;
}

/* the member e reads and the type it has: the text of e ->> 'k', or its cast to an integer or float8 */
static const char *
member_read(const V6Plan *p, const Expr *e, Oid *type)
{
	*type = TEXTOID;
	if (IsA(e, CoerceViaIO) && cast_type(((const CoerceViaIO *) e)->resulttype))
	{
		*type = ((const CoerceViaIO *) e)->resulttype;
		e = ((const CoerceViaIO *) e)->arg;
	}
	return member_name(p, (const Node *) e);
}

bool
sift_v6_members_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	SiftV6Ext  *x = p->xseed;
	SiftV6XElem *el = p->elem_stream == 0 ? &x->elem : &x->streams[p->elem_stream - 1];
	List	  **names = p->elem_stream == 0 ? &x->elem_names : (List **) &list_nth_cell(x->stream_names, p->elem_stream - 1)->ptr_value;
	Oid			type;
	const char *name = member_read(p, e, &type);
	int			at = 0;

	if (name == NULL)
		return false;
	foreach_node(String, n, *names)
	{
		if (strcmp(strVal(n), name) == 0 && el->col_type[at] == type)
			break;
		at++;
	}
	if (at == el->ncols)
	{
		int			leaf = el->group_leaf >= 0 ? sift_v6_unnest_member_leaf(&p->leaves, el->attno, el->group_leaf, name) : -1;
		const bool	held = leaf >= 0 && sift_v6_leaf_holds(type, -1, &p->leaves.schema->cols[leaf]);

		if (p->is_table && !held)
			el->group_leaf = leaf = -1;
		if (el->ncols == SIFT_V6_XELEM_COLS || (el->group_leaf < 0 ? !sift_v6_table_elem_type(type) : !held))
			return false;
		el->col_leaf[at] = leaf;
		el->col_type[at] = type;
		el->col_kind[at] = false;
		el->ncols++;
		*names = lappend(*names, makeString(pstrdup(name)));
	}
	*out = (SiftV6Col) {.attno = el->attno,.typid = type,.src = SV6SRC_ELEM,.slot = at};
	return true;
}

bool
sift_v6_elem_late(const SiftV6XElem *e, int slot)
{
	return e->fn >= SV6E_EACH ? slot == 1 : e->fn == SV6E_MEMBERS && e->col_type[slot] != TEXTOID;
}

Datum
sift_v6_member_text(Datum elem, text *name, bool *isnull)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		d;

	InitFunctionCallInfoData(*fcinfo, NULL, 2, InvalidOid, NULL, NULL);
	fcinfo->args[0] = (NullableDatum) {elem, false};
	fcinfo->args[1] = (NullableDatum) {PointerGetDatum(name), false};
	d = jsonb_object_field_text(fcinfo);
	*isnull = fcinfo->isnull;
	return d;
}
