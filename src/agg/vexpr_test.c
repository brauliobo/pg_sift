/*
 * agg/vexpr_test.c - test-only SQL functions of the expression machine (agg/vexpr.h), compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. sift._test_vexpr compiles a program written as a prefix expression and runs it over the arrays of a table's columns (int8 i and j, int4 k,
 * float8 x and y, bool m, and a jsonb d whose key presence of 'a' and 'b' is the boolean input of (has a) and (has b)), over the rows of a mask, and gives each
 * live row's result as text; the SQL test holds it to PostgreSQL's own evaluation of the same expression, errors included.
 *
 * The program: columns i j k x y m, constants 5i8 5i4 1.5f8 inff8 nanf8 and the typed NULLs nulli8 nulli4 nullf8, true and false, and the lists
 * (add a b) (sub a b) (mul a b) (div a b) (neg a) (abs a) (eq a b) (ne a b) (lt a b) (le a b) (gt a b) (ge a b) (and a b) (or a b) (not a) (isnull a)
 * (isnotnull a) (coalesce a b ...) (case (when c t) ... (else e)) (cast_i8 a) (cast_f8 a) (cast_n2 a) (has a) (has b).
 *
 * The numeric columns n (numeric(15, 2), as the unscaled int64 of an array) and w (numeric(30, 3), as the unscaled integers of a numeric array), the columns t
 * (timestamp) and dt (date), the numeric constants 125n2 (1.25: the unscaled value, n, and the scale) and 12500N3 (the same of 128 bits), and the calendar:
 * (trunc unit t) and (trunctz unit t) of second minute hour day week month quarter year (the second is a timestamptz read in the zone of the session),
 * (extract field t|dt) and (extracttz field t) of year quarter month day dow isodow doy hour minute epoch. Booleans have the type SVEC_BOOL, so that
 * (coalesce m (has a)) and (case (when m (has a)) (else m)) are booleans.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/jsonb.h"

#include "agg/vexpr.h"
#include "core/zoneoff.h"

PG_FUNCTION_INFO_V1(sift_test_vexpr);
PG_FUNCTION_INFO_V1(sift_test_vexpr_compiles);

typedef struct Parser
{
	const char *s;
	SiftVexprNode pool[160];
	int			n;
} Parser;

static SiftVexprNode *parse_expr(Parser *p);
static struct ZoneOff *test_zone;
static int	ts_kind_of(const SiftVexprNode *root);

static void
skip_ws(Parser *p)
{
	while (*p->s == ' ' || *p->s == '\n' || *p->s == '\t')
		p->s++;
}

static SiftVexprNode *
new_node(Parser *p, SiftVexprKind kind, SiftVecType type)
{
	SiftVexprNode *n;

	if (p->n >= lengthof(p->pool))
		ereport(ERROR, (errmsg("the program is too long")));
	n = &p->pool[p->n++];
	memset(n, 0, sizeof(*n));
	n->kind = kind;
	n->type = type;
	return n;
}

static bool
suffix(const char *a, const char *suf)
{
	const size_t la = strlen(a),
				ls = strlen(suf);

	return la > ls && !strcmp(a + la - ls, suf);
}

static SiftVexprNode *
parse_atom(Parser *p, const char *a)
{
	SiftVexprNode *n;
	char	   *num;

	if (!strcmp(a, "i") || !strcmp(a, "j"))
		n = new_node(p, SVX_COL, SVEC_I64), n->aux = a[0] == 'i' ? 0 : 1;
	else if (!strcmp(a, "k"))
		n = new_node(p, SVX_COL, SVEC_I32), n->aux = 2;
	else if (!strcmp(a, "x") || !strcmp(a, "y"))
		n = new_node(p, SVX_COL, SVEC_F64), n->aux = a[0] == 'x' ? 3 : 4;
	else if (!strcmp(a, "n"))
		n = new_node(p, SVX_COL, SVEC_NUM64), n->aux = 5, n->scale = 2, n->prec = 15;
	else if (!strcmp(a, "w"))
		n = new_node(p, SVX_COL, SVEC_NUM128), n->aux = 6, n->scale = 3, n->prec = 30;
	else if (!strcmp(a, "t"))
		n = new_node(p, SVX_COL, SVEC_I64), n->aux = 7;
	else if (!strcmp(a, "dt"))
		n = new_node(p, SVX_COL, SVEC_I32), n->aux = 8;
	else if (!strcmp(a, "m"))
		n = new_node(p, SVX_MASK_IN, SVEC_BOOL), n->aux = 0;
	else if (!strcmp(a, "true") || !strcmp(a, "false"))
		n = new_node(p, SVX_BOOL_CONST, SVEC_BOOL), n->flag = a[0] == 't';
	else if (!strncmp(a, "null", 4))
		n = new_node(p, SVX_NULL, !strcmp(a + 4, "i8") ? SVEC_I64 : !strcmp(a + 4, "i4") ? SVEC_I32 : SVEC_F64);
	else if (strlen(a) > 2 && (a[strlen(a) - 2] == 'n' || a[strlen(a) - 2] == 'N') && a[strlen(a) - 1] >= '0' && a[strlen(a) - 1] <= '9')
	{
		num = pnstrdup(a, strlen(a) - 2);
		n = new_node(p, SVX_CONST, a[strlen(a) - 2] == 'n' ? SVEC_NUM64 : SVEC_NUM128);
		n->i64 = strtoll(num, NULL, 10);
		n->scale = (int16) (a[strlen(a) - 1] - '0');
	}
	else if (suffix(a, "i8") || suffix(a, "i4") || suffix(a, "f8"))
	{
		const SiftVecType type = suffix(a, "i8") ? SVEC_I64 : suffix(a, "i4") ? SVEC_I32 : SVEC_F64;

		num = pnstrdup(a, strlen(a) - 2);
		n = new_node(p, SVX_CONST, type);
		if (type == SVEC_F64)
			n->f64 = strtod(num, NULL);
		else
			n->i64 = strtoll(num, NULL, 10);
	}
	else
		ereport(ERROR, (errmsg("unknown atom \"%s\"", a)));
	return n;
}

static SiftVexprNode *
parse_list(Parser *p)
{
	char		op[24];
	int			len = 0;
	SiftVexprNode *n;
	static const struct
	{
		const char *name;
		SiftVexprKind kind;
		int			aux;
	}			ops[] = {
		{"add", SVX_ADD, 0}, {"sub", SVX_SUB, 0}, {"mul", SVX_MUL, 0}, {"div", SVX_DIV, 0}, {"neg", SVX_NEG, 0}, {"abs", SVX_ABS, 0},
		{"eq", SVX_CMP, SCMP_EQ}, {"ne", SVX_CMP, SCMP_NE}, {"lt", SVX_CMP, SCMP_LT}, {"le", SVX_CMP, SCMP_LE}, {"gt", SVX_CMP, SCMP_GT},
		{"ge", SVX_CMP, SCMP_GE}, {"and", SVX_AND, 0}, {"or", SVX_OR, 0}, {"not", SVX_NOT, 0}, {"isnull", SVX_IS_NULL, 0},
		{"isnotnull", SVX_IS_NOT_NULL, 0}, {"coalesce", SVX_COALESCE, 0}, {"case", SVX_CASE, 0}, {"cast_i8", SVX_CAST, 0}, {"cast_f8", SVX_CAST, 1},
		{"has", SVX_MASK_IN, 0}, {"trunc", SVX_DATE_TRUNC, 0}, {"trunctz", SVX_DATE_TRUNC, 1}, {"extract", SVX_EXTRACT, 0}, {"extracttz", SVX_EXTRACT, 1}
	};
	static const char *const units[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};
	static const char *const fields[] = {"year", "quarter", "month", "day", "dow", "isodow", "doy", "hour", "minute", "epoch"};
	int			which = -1;

	skip_ws(p);
	while (*p->s && *p->s != ' ' && *p->s != ')' && len < 23)
		op[len++] = *p->s++;
	op[len] = 0;
	for (int i = 0; i < lengthof(ops); i++)
		if (!strcmp(op, ops[i].name))
			which = i;
	if (which == -1 && !strncmp(op, "cast_n", 6) && op[6] >= '0' && op[6] <= '9')
		which = -3;
	if (!strcmp(op, "when") || !strcmp(op, "else"))
		which = -2;
	if (which == -1)
		ereport(ERROR, (errmsg("unknown operator \"%s\"", op)));
	if (which == -2)
		ereport(ERROR, (errmsg("\"%s\" outside a case", op)));
	if (which == -3)
	{
		n = new_node(p, SVX_CAST, SVEC_NUM128);
		n->scale = (int16) (op[6] - '0');
		n->args[n->nargs++] = parse_expr(p);
		n->type = n->args[0]->type == SVEC_I32 || n->args[0]->type == SVEC_NUM64 ? SVEC_NUM64 : SVEC_NUM128;
		skip_ws(p);
		if (*p->s++ != ')')
			ereport(ERROR, (errmsg("a list is not closed")));
		return n;
	}
	n = new_node(p, ops[which].kind, SVEC_I64);
	n->aux = ops[which].aux;
	if (n->kind == SVX_DATE_TRUNC || n->kind == SVX_EXTRACT)
	{
		const int	tz = n->aux;
		const char *const *names = n->kind == SVX_DATE_TRUNC ? units : fields;
		const int	count = n->kind == SVX_DATE_TRUNC ? lengthof(units) : lengthof(fields);
		char		word[16];
		int			wl = 0,
					found = -1;

		skip_ws(p);
		while (*p->s && *p->s != ' ' && *p->s != ')' && wl < 15)
			word[wl++] = *p->s++;
		word[wl] = 0;
		for (int i = 0; i < count; i++)
			if (!strcmp(word, names[i]))
				found = i;
		if (found < 0)
			ereport(ERROR, (errmsg("unknown calendar name \"%s\"", word)));
		n->aux = found;
		n->zone = tz ? test_zone : NULL;
		n->args[n->nargs++] = parse_expr(p);
		skip_ws(p);
		if (*p->s++ != ')')
			ereport(ERROR, (errmsg("a list is not closed")));
		n->type = n->kind == SVX_DATE_TRUNC ? SVEC_I64 : SVEC_NUM64;
		return n;
	}
	if (n->kind == SVX_CASE)
		for (skip_ws(p); *p->s == '('; skip_ws(p))
		{
			const bool	is_else = !strncmp(p->s, "(else", 5);

			p->s += is_else ? 5 : 5;
			if (is_else)
			{
				n->args[n->nargs++] = parse_expr(p);
				n->flag = true;
			}
			else
			{
				n->args[n->nargs++] = parse_expr(p);
				n->args[n->nargs++] = parse_expr(p);
				n->aux++;
			}
			skip_ws(p);
			if (*p->s++ != ')')
				ereport(ERROR, (errmsg("a case branch is not closed")));
		}
	else if (n->kind == SVX_MASK_IN)
	{
		skip_ws(p);
		n->aux = *p->s == 'a' ? 1 : 2;
		p->s++;
	}
	else
		for (skip_ws(p); *p->s && *p->s != ')'; skip_ws(p))
		{
			if (n->nargs >= SIFT_VEXPR_MAX_ARGS)
				ereport(ERROR, (errmsg("too many arguments")));
			n->args[n->nargs++] = parse_expr(p);
		}
	skip_ws(p);
	if (*p->s++ != ')')
		ereport(ERROR, (errmsg("a list is not closed")));
	if (n->kind == SVX_CAST)
		n->type = n->aux ? SVEC_F64 : SVEC_I64;
	else if (n->kind == SVX_CASE)
		n->type = n->args[1]->type;
	else if (n->kind == SVX_CMP || n->kind == SVX_AND || n->kind == SVX_OR || n->kind == SVX_NOT || n->kind == SVX_IS_NULL || n->kind == SVX_IS_NOT_NULL ||
			 n->kind == SVX_MASK_IN)
		n->type = SVEC_BOOL;
	else if (n->nargs > 0)
		n->type = n->args[0]->type;
	return n;
}

static SiftVexprNode *
parse_expr(Parser *p)
{
	skip_ws(p);
	if (*p->s == '(')
	{
		p->s++;
		return parse_list(p);
	}
	{
		char		atom[40];
		int			len = 0;

		while (*p->s && *p->s != ' ' && *p->s != ')' && len < 39)
			atom[len++] = *p->s++;
		atom[len] = 0;
		return parse_atom(p, atom);
	}
}

static int	last_ts_kind;

static bool
compile_text(const char *text, SiftVexprProg *prog)
{
	Parser	   *p = palloc0(sizeof(Parser));
	SiftVexprNode *root;
	bool		ok;

	p->s = text;
	test_zone = zone_open(CurrentMemoryContext, session_timezone);
	root = parse_expr(p);
	last_ts_kind = ts_kind_of(root);
	ok = sift_vexpr_compile(root, prog);
	pfree(p);
	return ok;
}

Datum
sift_test_vexpr_compiles(PG_FUNCTION_ARGS)
{
	SiftVexprProg prog;

	PG_RETURN_BOOL(compile_text(text_to_cstring(PG_GETARG_TEXT_PP(0)), &prog));
}

#define NCOLS 9

typedef struct Cols
{
	SiftVec		v[NCOLS];
	SiftVexprMaskIn m[3];
	uint64		valid[NCOLS][SIFT_VEC_WORDS];
	uint64		bits[3][SIFT_VEC_WORDS];
	uint64		mvalid[3][SIFT_VEC_WORDS];
} Cols;

static void *
buf(size_t bytes)
{
	return palloc_aligned(bytes + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
}

typedef __int128 i128;

static i128
parse_i128(const char *s)
{
	const bool	neg = *s == '-';
	unsigned __int128 v = 0;

	for (s += neg; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (unsigned) (*s - '0');
	return neg ? -(i128) v : (i128) v;
}

/* the array of n rows as a column of the type: the unscaled integers of a numeric (NUM64 from int8, NUM128 from numeric), the microseconds of a timestamp, the days of a date */
static void
load_col(Cols *c, int idx, SiftVecType type, int scale, ArrayType *a, Oid oid, int len, char align, int n)
{
	Datum	   *d = NULL;
	bool	   *nl = NULL;
	int			cnt = 0;
	void	   *data = buf(SIFT_VEC_LEN * 16);

	if (a)
		deconstruct_array(a, oid, len, len == 8 || len == 4 || len == 1, align, &d, &nl, &cnt);
	Assert(!a || cnt == n);
	memset(&c->v[idx], 0, sizeof(SiftVec));
	c->v[idx].data = data;
	c->v[idx].valid = c->valid[idx];
	c->v[idx].len = (uint16) n;
	c->v[idx].dense = true;
	c->v[idx].type = type;
	c->v[idx].scale = (int16) scale;
	for (int i = 0; i < n; i++)
	{
		const bool	null = !a || nl[i];

		if (!null)
		{
			if (type == SVEC_I64 || type == SVEC_NUM64)
				((int64 *) data)[i] = DatumGetInt64(d[i]);
			else if (type == SVEC_I32)
				((int32 *) data)[i] = DatumGetInt32(d[i]);
			else if (type == SVEC_NUM128)
				((i128 *) data)[i] = parse_i128(DatumGetCString(DirectFunctionCall1(numeric_out, d[i])));
			else
				((double *) data)[i] = DatumGetFloat8(d[i]);
		}
		c->valid[idx][i >> 6] |= (uint64) !null << (i & 63);
	}
}

static char *
num_text(i128 v, int scale)
{
	char		digits[48];
	int			nd = 0;
	StringInfoData out;
	const bool	neg = v < 0;
	unsigned __int128 u = neg ? -(unsigned __int128) v : (unsigned __int128) v;

	do
	{
		digits[nd++] = (char) ('0' + (int) (u % 10));
		u /= 10;
	} while (u);
	while (nd <= scale)
		digits[nd++] = '0';
	initStringInfo(&out);
	if (neg)
		appendStringInfoChar(&out, '-');
	for (int i = nd - 1; i >= 0; i--)
	{
		appendStringInfoChar(&out, digits[i]);
		if (i == scale && scale > 0)
			appendStringInfoChar(&out, '.');
	}
	return out.data;
}

/* 0: the value as a number, 1: a timestamp, 2: a timestamptz */
static char *
cell_text(const SiftVec *v, int i, int ts_kind)
{
	if (v->type == SVEC_I32)
		return psprintf("%d", ((const int32 *) v->data)[i]);
	if (v->type == SVEC_I64 && ts_kind)
		return DatumGetCString(DirectFunctionCall1(ts_kind == 1 ? timestamp_out : timestamptz_out, TimestampGetDatum(((const int64 *) v->data)[i])));
	if (v->type == SVEC_I64)
		return psprintf("%lld", (long long) ((const int64 *) v->data)[i]);
	if (v->type == SVEC_NUM64)
		return num_text(((const int64 *) v->data)[i], v->scale);
	if (v->type == SVEC_NUM128)
		return num_text(((const i128 *) v->data)[i], v->scale);
	return float8out_internal(((const double *) v->data)[i]);
}

static int
ts_kind_of(const SiftVexprNode *root)
{
	return root->kind == SVX_DATE_TRUNC ? (root->zone ? 2 : 1) : 0;
}

/*
 * sift._test_vexpr(prog text, i int8[], j int8[], k int4[], x float8[], y float8[], m bool[], d jsonb[], act bool[]) RETURNS TABLE(ord int4, res text):
 * the live rows' results
 */
Datum
sift_test_vexpr(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftVexprProg prog;
	Cols	   *c = palloc0(sizeof(Cols));
	Datum	   *md,
			   *dd,
			   *ad;
	bool	   *mn,
			   *dn,
			   *an;
	int			n,
				nm,
				nd,
				na;
	uint64		active[SIFT_VEC_WORDS];
	SiftVexprCtx *ctx;
	SiftVexprIn in;
	SiftVexprOut out;

	sift_aggkern6_init();
	InitMaterializedSRF(fcinfo, 0);
	if (!compile_text(text_to_cstring(PG_GETARG_TEXT_PP(0)), &prog))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the program is not one the machine runs")));
	n = ArrayGetNItems(ARR_NDIM(PG_GETARG_ARRAYTYPE_P(1)), ARR_DIMS(PG_GETARG_ARRAYTYPE_P(1)));
	if (n > SIFT_VEC_LEN)
		ereport(ERROR, (errmsg("at most %d rows", SIFT_VEC_LEN)));
	load_col(c, 0, SVEC_I64, 0, PG_GETARG_ARRAYTYPE_P(1), INT8OID, 8, 'd', n);
	load_col(c, 1, SVEC_I64, 0, PG_GETARG_ARRAYTYPE_P(2), INT8OID, 8, 'd', n);
	load_col(c, 2, SVEC_I32, 0, PG_GETARG_ARRAYTYPE_P(3), INT4OID, 4, 'i', n);
	load_col(c, 3, SVEC_F64, 0, PG_GETARG_ARRAYTYPE_P(4), FLOAT8OID, 8, 'd', n);
	load_col(c, 4, SVEC_F64, 0, PG_GETARG_ARRAYTYPE_P(5), FLOAT8OID, 8, 'd', n);
	load_col(c, 5, SVEC_NUM64, 2, PG_ARGISNULL(9) ? NULL : PG_GETARG_ARRAYTYPE_P(9), INT8OID, 8, 'd', n);
	load_col(c, 6, SVEC_NUM128, 3, PG_ARGISNULL(10) ? NULL : PG_GETARG_ARRAYTYPE_P(10), NUMERICOID, -1, 'i', n);
	load_col(c, 7, SVEC_I64, 0, PG_ARGISNULL(11) ? NULL : PG_GETARG_ARRAYTYPE_P(11), TIMESTAMPOID, 8, 'd', n);
	load_col(c, 8, SVEC_I32, 0, PG_ARGISNULL(12) ? NULL : PG_GETARG_ARRAYTYPE_P(12), DATEOID, 4, 'i', n);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(6), BOOLOID, 1, true, 'c', &md, &mn, &nm);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(7), JSONBOID, -1, false, 'i', &dd, &dn, &nd);
	for (int i = 0; i < n; i++)
	{
		c->bits[0][i >> 6] |= (uint64) (!mn[i] && DatumGetBool(md[i])) << (i & 63);
		c->mvalid[0][i >> 6] |= (uint64) !mn[i] << (i & 63);
		for (int key = 1; key <= 2; key++)
		{
			const bool	has = !dn[i] && DatumGetBool(DirectFunctionCall2(jsonb_exists, dd[i], CStringGetTextDatum(key == 1 ? "a" : "b")));

			c->bits[key][i >> 6] |= (uint64) has << (i & 63);
			c->mvalid[key][i >> 6] |= (uint64) !dn[i] << (i & 63);
		}
	}
	for (int key = 0; key < 3; key++)
	{
		c->m[key].bits = c->bits[key];
		c->m[key].valid = c->mvalid[key];
	}
	memset(active, 0, sizeof(active));
	if (PG_ARGISNULL(8))
		for (int i = 0; i < n; i++)
			active[i >> 6] |= UINT64CONST(1) << (i & 63);
	else
	{
		deconstruct_array(PG_GETARG_ARRAYTYPE_P(8), BOOLOID, 1, true, 'c', &ad, &an, &na);
		for (int i = 0; i < n; i++)
			active[i >> 6] |= (uint64) (!an[i] && DatumGetBool(ad[i])) << (i & 63);
	}
	in.cols = c->v;
	in.masks = c->m;
	ctx = sift_vexpr_ctx_new(CurrentMemoryContext);
	sift_vexpr_eval(ctx, &prog, &in, active, (uint16) n, &out);
	for (int i = 0; i < n; i++)
	{
		Datum		row[2];
		bool		nulls[2] = {false, false};

		if (!((active[i >> 6] >> (i & 63)) & 1))
			continue;
		row[0] = Int32GetDatum(i + 1);
		if (out.is_bool)
		{
			nulls[1] = !((out.known_bits[i >> 6] >> (i & 63)) & 1);
			row[1] = nulls[1] ? (Datum) 0 : CStringGetTextDatum(((out.true_bits[i >> 6] >> (i & 63)) & 1) ? "true" : "false");
		}
		else
		{
			nulls[1] = !(out.vec->all_valid || ((out.vec->valid[i >> 6] >> (i & 63)) & 1));
			row[1] = nulls[1] ? (Datum) 0 : CStringGetTextDatum(cell_text(out.vec, i, last_ts_kind));
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, row, nulls);
	}
	sift_vexpr_ctx_free(ctx);
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
