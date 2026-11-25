/*
 * col/f2/v6/proj/key.c - the keys of a statement's index scan as the predicates a projection tests its cells for (col/f2/v6/proj/pred.h). A key that cannot be tested on cells exactly (a null
 * test, a pattern, a float, a text ordered by a collation that is not bytewise, a date against a timestamp, a numeric the scaled integer cannot hold) makes the statement read the base.
 */
#include "postgres.h"

#include "access/skey.h"
#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "access/genam.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/uuid.h"

#include "col/f2/v6/proj/key.h"
#include "agg/v6/qual/qual.h"
#include "col/am.h"
#include "col/am/preds.h"

typedef enum KeyClass
{
	KC_NONE = 0,
	KC_INT,
	KC_NUMERIC,
	KC_TEXT,
	KC_UUID
} KeyClass;

static bool
integer_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static bool
text_type(Oid t)
{
	return t == TEXTOID || t == VARCHAROID;
}

static KeyClass
class_of(Oid att, Oid arg)
{
	if (integer_type(att) && integer_type(arg))
		return KC_INT;
	if (att != arg)
		return text_type(att) && text_type(arg) ? KC_TEXT : KC_NONE;
	switch (att)
	{
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case BOOLOID:
			return KC_INT;
		case NUMERICOID:
			return KC_NUMERIC;
		case TEXTOID:
		case VARCHAROID:
			return KC_TEXT;
		case UUIDOID:
			return KC_UUID;
		default:
			return KC_NONE;
	}
}

bool
sift_v6_proj_collation_bytewise(Oid collid)
{
	return OidIsValid(collid) && sift_collate_is_c(collid);
}

static int64
int_value(Oid type, Datum d)
{
	switch (type)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		case BOOLOID:
			return DatumGetBool(d) ? 1 : 0;
		default:
			return DatumGetInt64(d);
	}
}

static SiftProjConst
bytes_value(KeyClass kc, Datum d)
{
	if (kc == KC_UUID)
		return (SiftProjConst) {.s = (const char *) DatumGetUUIDP(d),.len = UUID_LEN};
	else
	{
		struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

		return (SiftProjConst) {.s = VARDATA_ANY(t),.len = VARSIZE_ANY_EXHDR(t)};
	}
}

static bool
list_of(KeyClass kc, int scale, Oid elemtype, Datum array, SiftProjConst *vals, int *n)
{
	ArrayType  *arr = DatumGetArrayTypeP(array);
	int16		len;
	bool		byval;
	char		align;
	Datum	   *el;
	bool	   *nl;
	int			cnt;

	get_typlenbyvalalign(elemtype, &len, &byval, &align);
	deconstruct_array(arr, elemtype, len, byval, align, &el, &nl, &cnt);
	*n = 0;
	for (int j = 0; j < cnt; j++)
	{
		int64		down,
					up;

		if (nl[j])
			continue;
		if (kc == KC_NUMERIC)
		{
			if (!sift_col_numeric_key_fits(scale, el[j]))
				return false;
			down = sift_col_numeric_key(scale, el[j], SIFT_COL_ROUND_DOWN);
			up = sift_col_numeric_key(scale, el[j], SIFT_COL_ROUND_UP);
			if (down == up)
				vals[(*n)++] = (SiftProjConst) {.i = down};
		}
		else if (kc == KC_INT)
			vals[(*n)++] = (SiftProjConst) {.i = int_value(elemtype, el[j])};
		else
			vals[(*n)++] = bytes_value(kc, el[j]);
	}
	return true;
}

/* A scalar comparison of a numeric column: x > c is key > floor(c), x >= c key >= ceil(c), x < c key < ceil(c), x <= c key <= floor(c), x = c needs a constant of at most the scale's digits */
static bool
numeric_scalar(int scale, StrategyNumber st, Datum d, SiftProjPred *p, SiftProjConst *k)
{
	int64		down,
				up;

	if (!sift_col_numeric_key_fits(scale, d))
		return false;
	down = sift_col_numeric_key(scale, d, SIFT_COL_ROUND_DOWN);
	up = sift_col_numeric_key(scale, d, SIFT_COL_ROUND_UP);
	if (st == BTEqualStrategyNumber && down != up)
	{
		p->op = SPO_IN;
		p->nvals = 0;
		return true;
	}
	k->i = (st == BTGreaterStrategyNumber || st == BTLessEqualStrategyNumber || st == BTEqualStrategyNumber) ? down : up;
	return true;
}

bool
sift_v6_proj_pred_of_key(const SiftF2Store *store, TupleDesc td, const ScanKeyData *sk, SiftProjPred *out, MemoryContext cx)
{
	Oid			att = TupleDescAttr(td, sk->sk_attno - 1)->atttypid;
	bool		array = (sk->sk_flags & SK_SEARCHARRAY) != 0;
	int			leaf = store->att_first[sk->sk_attno];
	Oid			arg = OidIsValid(sk->sk_subtype) ? sk->sk_subtype : att;
	KeyClass	kc;
	StrategyNumber st = sk->sk_strategy;
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftProjConst *vals;
	bool		ok = false;

	if ((sk->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)) != 0 && leaf >= 0)
	{
		*out = (SiftProjPred) {.leaf = (int16) leaf,.op = (sk->sk_flags & SK_SEARCHNULL) != 0 ? SPO_IS_NULL : SPO_IS_NOT_NULL};
		MemoryContextSwitchTo(old);
		return true;
	}
	if ((sk->sk_flags & ~SK_SEARCHARRAY) != 0 || st < BTLessStrategyNumber || (st > BTGreaterStrategyNumber && st != COL_STRAT_NE) || (array && st != BTEqualStrategyNumber) || leaf < 0)
		goto done;
	if (array)
		arg = ARR_ELEMTYPE(DatumGetArrayTypeP(sk->sk_argument));
	kc = class_of(att, arg);
	if (kc == KC_NONE || (kc == KC_TEXT || kc == KC_UUID ? store->kinds[leaf] != SIFT_CELL_STR : store->kinds[leaf] != SIFT_CELL_INT))
		goto done;
	if (kc == KC_TEXT && !(OidIsValid(sk->sk_collation) && get_collation_isdeterministic(sk->sk_collation)))
		goto done;
	if (kc == KC_NUMERIC && st == COL_STRAT_NE)
		goto done;
	if (kc == KC_TEXT && st != BTEqualStrategyNumber && st != COL_STRAT_NE && !sift_v6_proj_collation_bytewise(sk->sk_collation))
		goto done;
	vals = palloc(Max(array ? ArrayGetNItems(ARR_NDIM(DatumGetArrayTypeP(sk->sk_argument)), ARR_DIMS(DatumGetArrayTypeP(sk->sk_argument))) : 1, 1) * sizeof(SiftProjConst));
	*out = (SiftProjPred) {.leaf = (int16) leaf,.op = array ? SPO_IN : (uint8) (st - BTLessStrategyNumber),.bytes = kc == KC_TEXT || kc == KC_UUID,.nvals = 1,.vals = vals};
	if (array)
		ok = list_of(kc, store->scales[leaf], arg, sk->sk_argument, vals, &out->nvals);
	else if (kc == KC_NUMERIC)
		ok = numeric_scalar(store->scales[leaf], st, sk->sk_argument, out, &vals[0]);
	else
	{
		vals[0] = kc == KC_INT ? (SiftProjConst) {.i = int_value(arg, sk->sk_argument)} : bytes_value(kc, sk->sk_argument);
		ok = true;
	}
done:
	MemoryContextSwitchTo(old);
	return ok;
}

bool
sift_v6_proj_preds_of_keys(const SiftF2Store *store, TupleDesc td, const ScanKeyData *keys, int nkeys, SiftProjPred **out, MemoryContext cx)
{
	*out = MemoryContextAlloc(cx, Max(nkeys, 1) * sizeof(SiftProjPred));
	for (int i = 0; i < nkeys; i++)
		if (!sift_v6_proj_pred_of_key(store, td, &keys[i], &(*out)[i], cx))
			return false;
	return true;
}

/* The attribute of the index an INDEX_VAR column names; 0 when e is not one */
static int
index_attno(const Node *e)
{
	return e != NULL && IsA(e, Var) && ((const Var *) e)->varno == INDEX_VAR && ((const Var *) e)->varlevelsup == 0 ? ((const Var *) e)->varattno : 0;
}

/* The operator family a column of an index compares in, and for a table (no index) the default one of the type of the column */
static Oid
column_opfamily(Relation rel, int attno)
{
	if (rel->rd_opfamily != NULL)
		return rel->rd_opfamily[attno - 1];
	return get_opclass_family(GetDefaultOpClass(TupleDescAttr(RelationGetDescr(rel), attno - 1)->atttypid, BTREE_AM_OID));
}

/* One qual of a FILTER, in the forms the scan's own quals take (a column and a constant compared, = ANY of a constant array, IS [NOT] NULL), as a predicate of a spec */
static bool
pred_of_qual(const SiftF2Store *store, Relation index, const Node *q, SiftProjSpecPred *out, MemoryContext cx)
{
	ScanKeyData sk = {0};
	SiftProjPred p;
	const Node *l,
			   *r;
	Oid			opno,
				collid;
	int			attno;
	bool		array = false;

	if (IsA(q, NullTest) && !((const NullTest *) q)->argisrow && (attno = index_attno((const Node *) ((const NullTest *) q)->arg)) > 0 && store->att_first[attno] >= 0)
	{
		*out = (SiftProjSpecPred) {.leaf = (int16) store->att_first[attno],.op = ((const NullTest *) q)->nulltesttype == IS_NULL ? SPO_IS_NULL : SPO_IS_NOT_NULL};
		return true;
	}
	if (IsA(q, OpExpr) && list_length(((const OpExpr *) q)->args) == 2)
	{
		opno = ((const OpExpr *) q)->opno;
		collid = ((const OpExpr *) q)->inputcollid;
		l = linitial(((const OpExpr *) q)->args);
		r = lsecond(((const OpExpr *) q)->args);
	}
	else if (IsA(q, ScalarArrayOpExpr) && ((const ScalarArrayOpExpr *) q)->useOr && list_length(((const ScalarArrayOpExpr *) q)->args) == 2)
	{
		opno = ((const ScalarArrayOpExpr *) q)->opno;
		collid = ((const ScalarArrayOpExpr *) q)->inputcollid;
		l = linitial(((const ScalarArrayOpExpr *) q)->args);
		r = lsecond(((const ScalarArrayOpExpr *) q)->args);
		array = true;
	}
	else
		return false;
	attno = index_attno(l);
	if (attno == 0 || r == NULL || !IsA(r, Const) || ((const Const *) r)->constisnull)
		return false;
	sk.sk_attno = attno;
	sk.sk_strategy = get_op_opfamily_strategy(opno, column_opfamily(index, attno));
	if (sk.sk_strategy == 0)
		sk.sk_strategy = sift_v6_qual_private_strategy(opno, column_opfamily(index, attno));
	sk.sk_subtype = array ? InvalidOid : ((const Const *) r)->consttype;
	sk.sk_collation = collid;
	sk.sk_flags = array ? SK_SEARCHARRAY : 0;
	sk.sk_argument = ((const Const *) r)->constvalue;
	return sk.sk_strategy != 0 && sift_v6_proj_pred_of_key(store, RelationGetDescr(index), &sk, &p, cx) && sift_proj_spec_pred_of_pred(&p, out);
}

bool
sift_v6_proj_preds_of_quals(const SiftF2Store *store, Relation index, const List *quals, SiftProjSpecPred *out, int max, int *n, MemoryContext cx)
{
	*n = 0;
	foreach_ptr(Node, q, quals)
	{
		if (*n == max || !pred_of_qual(store, index, q, &out[*n], cx))
			return false;
		(*n)++;
	}
	return true;
}
