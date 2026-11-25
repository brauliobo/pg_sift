/*
 * col/am/preds.c - the predicates of a scan (the clauses of the index qual, for the scan of either format): the clauses of the index qual as scan predicates
 * keys, times, operators, LIKE, numerics
 * what the segments are asked for
 */
/* check-memory: allow-allocs every array here follows the keys, predicates, slots or constants of one plan or query, not the rows of the table */
#include "postgres.h"
#include <math.h>
#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "col/am.h"
#include "col/am/preds.h"
#include "col/like/simd.h"
#include "core/hash.h"

/* The constant of a key as a predicate value of the leaf: the key's own type is an integer, date, timestamp, boolean, float, text, char(n) or uuid (numeric: numeric_preds) */
void
key_value(Oid argtype, int32 typmod, Datum d, SiftColValue *v)
{
	*v = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
	switch (argtype)
	{
		case INT2OID:
			v->v.i = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			v->v.i = DatumGetInt32(d);
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			v->v.i = DatumGetInt64(d);
			break;
		case BOOLOID:
			v->v.i = DatumGetBool(d);
			break;
		case FLOAT4OID:
			v->v.f = DatumGetFloat4(d);
			break;
		case FLOAT8OID:
			v->v.f = DatumGetFloat8(d);
			break;
		case TEXTOID:
		case VARCHAROID:
			{
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

				v->v.s = VARDATA_ANY(t);
				v->len = VARSIZE_ANY_EXHDR(t);
			}
			break;
		case BPCHAROID:
			{
				/* the leaf holds the value padded with blanks to its length in characters; trailing blanks of the constant are not part of its value */
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
				const char *s = VARDATA_ANY(t);
				int			len = VARSIZE_ANY_EXHDR(t),
							pad;

				while (len > 0 && s[len - 1] == ' ')
					len--;
				pad = typmod - VARHDRSZ - pg_mbstrlen_with_len(s, len);
				if (pad > 0)
				{
					char	   *buf = palloc(len + pad);

					memcpy(buf, s, len);
					memset(buf + len, ' ', pad);
					s = buf;
					len += pad;
				}
				v->v.s = s;
				v->len = len;
			}
			break;
		case UUIDOID:
			v->v.s = (const char *) DatumGetUUIDP(d);
			v->len = UUID_LEN;
			break;
		default:
			SIFT_INVARIANT(false, "sift_col: no scan on a key of type %u", argtype);
	}
}

bool
xtime_pair(Oid coltype, Oid argtype)
{
	return coltype != argtype && (coltype == DATEOID || coltype == TIMESTAMPOID || coltype == TIMESTAMPTZOID) &&
		(argtype == DATEOID || argtype == TIMESTAMPOID || argtype == TIMESTAMPTZOID);
}

/*
 * The constant of a key whose type is another of date, timestamp and timestamptz than the column's, as a bound of the column itself. PostgreSQL compares a date
 * as its midnight (in the session's zone against a timestamptz), so a date column against a timestamp t is the days below t: d < t is d < ceil(t), d <= t is
 * d <= floor(t), d > t is d > floor(t), d >= t is d >= ceil(t), and d = t holds for a t that is a midnight only. A timestamp column against a date is the date's
 * midnight. 0: the key is not of that kind (key_value reads it), 1: *strat and *v are the bound, 2: not exact here (an out-of-range date), 3: no row can satisfy it.
 */
int
xtime_bound(Oid coltype, Oid argtype, Datum d, int *strat, int64 *v)
{
	if (!xtime_pair(coltype, argtype))
		return 0;
	if (coltype == DATEOID)
	{
		int64		t = DatumGetInt64(d);
		int64		day;
		bool		exact = true;

		if (TIMESTAMP_NOT_FINITE(t))
			day = t == PG_INT64_MIN ? DATEVAL_NOBEGIN : DATEVAL_NOEND;
		else if (argtype == TIMESTAMPOID)
		{
			day = t / USECS_PER_DAY - (t % USECS_PER_DAY < 0);
			exact = t == day * USECS_PER_DAY;
		}
		else
		{
			day = DatumGetDateADT(DirectFunctionCall1(timestamptz_date, d));
			if (!IS_VALID_DATE(day - 1) || !IS_VALID_DATE(day + 1))
				return 2;
			exact = DatumGetTimestampTz(DirectFunctionCall1(date_timestamptz, DateADTGetDatum(day))) == t;
		}
		if (*strat == BTEqualStrategyNumber && !exact)
			return 3;
		*v = day + (!exact && (*strat == BTLessStrategyNumber || *strat == BTGreaterEqualStrategyNumber));
		return 1;
	}
	else
	{
		DateADT		x = DatumGetDateADT(d);

		if (DATE_NOT_FINITE(x))
			*v = x == DATEVAL_NOBEGIN ? PG_INT64_MIN : PG_INT64_MAX;
		else if (!IS_VALID_DATE(x - 1) || !IS_VALID_DATE(x + 1))
			return 2;
		else if (coltype == TIMESTAMPOID)
			*v = (int64) x * USECS_PER_DAY;
		else
			*v = DatumGetTimestampTz(DirectFunctionCall1(date_timestamptz, d));
		return 1;
	}
}

/* A key the leaf cannot test exactly (a NaN, a numeric out of the range of the leaf): IS NOT NULL keeps every row it could match and the heap scan decides */
static void
key_any(SiftColPred *p, ColKeys *k)
{
	p->op = SIFT_COLOP_NOTNULL;
	p->nvals = 0;
	p->vals = NULL;
	k->recheck = true;
}

#define COL_STRAT_RANGE_FIRST	10

#define COL_STRAT_RANGE_LAST	18

#define COL_STRAT_NET_FIRST		21

#define COL_STRAT_NET_LAST		25

/* the SIFT_COLW_* operator of a strategy on the head of a range or a network */
static int
shape_op(const SiftColDef *def, StrategyNumber s)
{
	if (s == BTEqualStrategyNumber)
		return sift_col_def_shape(def) == SIFT_COL_SHAPE_RANGE ? SIFT_COLW_R_EQ : SIFT_COLW_N_EQ;
	if (s >= COL_STRAT_RANGE_FIRST && s <= COL_STRAT_RANGE_LAST)
		return SIFT_COLW_R_CONTAINS + (int) (s - COL_STRAT_RANGE_FIRST);
	if (s >= COL_STRAT_NET_FIRST && s <= COL_STRAT_NET_LAST)
		return SIFT_COLW_N_SUB + (int) (s - COL_STRAT_NET_FIRST);
	SIFT_INVARIANT(false, "sift_col: strategy %d is not an operator of a range or network column", s);
	return 0;
}

static SiftColOp
strategy_op(StrategyNumber s)
{
	switch (s)
	{
		case BTLessStrategyNumber:
			return SIFT_COLOP_LT;
		case BTLessEqualStrategyNumber:
			return SIFT_COLOP_LE;
		case BTEqualStrategyNumber:
			return SIFT_COLOP_EQ;
		case BTGreaterEqualStrategyNumber:
			return SIFT_COLOP_GE;
		case BTGreaterStrategyNumber:
			return SIFT_COLOP_GT;
		case COL_STRAT_NE:
			return SIFT_COLOP_NE;
	}
	SIFT_INVARIANT(false, "sift_col: strategy %d is not an operator of the scan", s);
	return 0;
}

/* The longest run of literal bytes between the wildcards of a LIKE pattern, into best: the string every match holds; 0 for a pattern that ends in a backslash */
static int
like_longest_run(const char *pat, int len, char *best)
{
	char	   *run = palloc(len + 1);
	int			n = 0,
				nbest = 0;

	for (int i = 0; i <= len; i++)
	{
		if (i < len && pat[i] == '\\')
		{
			if (i + 1 >= len)
				return 0;
			run[n++] = pat[++i];
		}
		else if (i < len && pat[i] != '%' && pat[i] != '_')
			run[n++] = pat[i];
		else
		{
			if (n > nbest)
				memcpy(best, run, nbest = n);
			n = 0;
		}
	}
	return nbest;
}

/*
 * LIKE and ^@ on a text leaf: the literal prefix of the pattern (up to its first % or _, a backslash escapes) is a byte prefix test of the leaf, which is exact for
 * 'prefix%', '=' for a pattern with no wildcard and a wider set for any other pattern (the heap scan applies the pattern then). A pattern with no prefix, one that ends in
 * a backslash (an error to LIKE) and, in an encoding where a wildcard byte can be part of a character, any pattern: IS NOT NULL and a recheck.
 */
void
like_pred(ColKeys *k, SiftColPred *p, SiftColValue *v, bool starts_with, bool infix)
{
	const char *pat = v->v.s;
	int			len = v->len,
				n = 0,
				i = 0,
				run;
	char	   *buf = palloc(len + 1);

	if (!starts_with && GetDatabaseEncoding() != PG_UTF8 && pg_database_encoding_max_length() > 1)
	{
		key_any(p, k);
		return;
	}
	while (i < len)
	{
		if (!starts_with && pat[i] == '\\')
		{
			if (i + 1 >= len)
			{
				key_any(p, k);
				return;
			}
			buf[n++] = pat[i + 1];
			i += 2;
			continue;
		}
		if (!starts_with && (pat[i] == '%' || pat[i] == '_'))
			break;
		buf[n++] = pat[i++];
	}
	if (n == 0 && infix && (run = like_longest_run(pat, len, buf)) >= SIFT_LIKE_INFIX_MIN)
	{
		uint32		litlen;

		v->v.s = buf;
		v->len = run;
		p->op = SIFT_COLOP_CONTAINS;
		p->nvals = 1;
		p->vals = v;
		k->recheck |= !(sift_like_parse((const uint8 *) pat, len, (uint8 *) palloc(len + 1), &litlen) == SIFT_LIKE_CONTAINS && litlen == (uint32) run);
		return;
	}
	if (n == 0)
	{
		key_any(p, k);
		return;
	}
	v->v.s = buf;
	v->len = n;
	p->op = !starts_with && i == len ? SIFT_COLOP_EQ : SIFT_COLOP_PREFIX;
	p->nvals = 1;
	p->vals = v;
	if (!starts_with && i < len && !(pat[i] == '%' && i == len - 1))
		k->recheck = true;
}

/* NOT LIKE '%lit%' is the rows that do not hold lit, NOT LIKE 'lit' those that differ from it: the aggregate scan sends no other pattern (agg/v6/qual/qual.c) */
static void
not_like_pred(SiftColPred *p, SiftColValue *v)
{
	const char *pat = v->v.s;
	uint8	   *lit = palloc(v->len + 1);
	uint32		litlen;
	const SiftLikeShape shape = sift_like_parse((const uint8 *) pat, v->len, lit, &litlen);

	SIFT_INVARIANT(shape == SIFT_LIKE_CONTAINS || shape == SIFT_LIKE_EQUAL, "sift_col: a NOT LIKE pattern of the scan is a literal or %%literal%%");
	v->v.s = (const char *) lit;
	v->len = litlen;
	p->op = shape == SIFT_LIKE_CONTAINS ? SIFT_COLOP_NOTCONTAINS : SIFT_COLOP_NE;
	p->nvals = 1;
	p->vals = v;
}

static bool
key_is_float(Oid t)
{
	return t == FLOAT4OID || t == FLOAT8OID;
}

/*
 * A numeric(p <= 18, s) leaf holds the number times 10^s: the constants are scaled as sift_col_numeric_key says, so that the keys that satisfy
 * the predicate are exactly those of the key range. false: no value can satisfy the key. A constant the leaf cannot hold (NaN, or too large to scale) and an
 * array with an operator other than = fall back to IS NOT NULL.
 */
static bool
numeric_preds(const SiftColDef *def, const ScanKeyData *key, SiftColPred *p, ColKeys *k)
{
	StrategyNumber st = key->sk_strategy;

	if (key->sk_flags & SK_SEARCHARRAY)
	{
		ArrayType  *arr = DatumGetArrayTypeP(key->sk_argument);
		int16		len;
		bool		byval;
		char		align;
		Datum	   *el;
		bool	   *nl;
		int			n,
					m = 0;
		SiftColValue *vals;

		if (st != BTEqualStrategyNumber)
		{
			key_any(p, k);
			return true;
		}
		get_typlenbyvalalign(NUMERICOID, &len, &byval, &align);
		deconstruct_array(arr, NUMERICOID, len, byval, align, &el, &nl, &n);
		vals = palloc(Max(n, 1) * sizeof(SiftColValue));
		for (int j = 0; j < n; j++)
		{
			int64		lo;

			if (nl[j])
				continue;
			if (!sift_col_numeric_key_fits(def->scale, el[j]))
			{
				key_any(p, k);
				return true;
			}
			lo = sift_col_numeric_key(def->scale, el[j], SIFT_COL_ROUND_DOWN);
			if (lo != sift_col_numeric_key(def->scale, el[j], SIFT_COL_ROUND_UP))
				continue;
			vals[m] = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
			vals[m++].v.i = lo;
		}
		if (m == 0)
			return false;
		p->op = SIFT_COLOP_IN;
		p->nvals = m;
		p->vals = vals;
		return true;
	}
	if (!sift_col_numeric_key_fits(def->scale, key->sk_argument))
	{
		key_any(p, k);
		return true;
	}
	{
		SiftColValue *v = palloc(sizeof(SiftColValue));
		int64		down = sift_col_numeric_key(def->scale, key->sk_argument, SIFT_COL_ROUND_DOWN),
					up = sift_col_numeric_key(def->scale, key->sk_argument, SIFT_COL_ROUND_UP);

		*v = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
		/* x > c is key > floor(c), x >= c is key >= ceil(c), x < c is key < ceil(c), x <= c is key <= floor(c); x = c needs a constant of at most s digits */
		if (st == BTEqualStrategyNumber && down != up)
			return false;
		v->v.i = (st == BTGreaterStrategyNumber || st == BTLessEqualStrategyNumber || st == BTEqualStrategyNumber) ? down : up;
		p->op = strategy_op(st);
		p->nvals = 1;
		p->vals = v;
	}
	return true;
}

/* The leaf holds the scaled integer of a narrow numeric and the bytes of a text: a wide or imaged numeric, a char without a length and a text of a nondeterministic collation are equal by more than their stored form, so the heap decides */
static bool
leaf_decides(const SiftColDef *def, Oid typid, Oid collid)
{
	Oid			base = getBaseType(typid);

	if (base == NUMERICOID)
		return !sift_col_def_is_wide(def) && def->store == SIFT_COLS_INT;
	if (base == BPCHAROID && (def->flags & SIFT_COLD_LOSSY))
		return false;
	if (base == TEXTOID || base == VARCHAROID || base == BPCHAROID)
		return !OidIsValid(collid) || get_collation_isdeterministic(collid);
	return true;
}

/* are the constants of a leaf compared by their bytes (a text, a uuid) or as integers (the key of an integer-like type, a date, a timestamp)? */
static bool
same_constant(const SiftColDef *def, const SiftColValue *a, const SiftColValue *b)
{
	if (def->type == SIFT_COLT_TEXT || def->type == SIFT_COLT_UUID)
		return a->len == b->len && memcmp(a->v.s, b->v.s, a->len) == 0;
	return a->v.i == b->v.i;
}

static uint32
constant_hash(const SiftColDef *def, const SiftColValue *v)
{
	if (def->type == SIFT_COLT_TEXT || def->type == SIFT_COLT_UUID)
		return hash_bytes((const unsigned char *) v->v.s, v->len);
	return (uint32) (sift_fib64((uint64) v->v.i) >> 32);
}

/* The constants of few that wide holds too, in the order of few: a table of the constants of few, probed with each of wide; false when no constant is common. Small lists are compared in a nested loop */
static bool
common_constants(const SiftColDef *def, const SiftColPred *few, SiftColPred *wide)
{
	SiftColValue *kept;
	int m = 0;

	/* check-memory: allow the constants of one predicate, bounded by the query */
	kept = palloc(few->nvals * sizeof(SiftColValue));
	if ((int64) few->nvals * wide->nvals <= 256)
	{
		for (int a = 0; a < few->nvals; a++)
			for (int b = 0; b < wide->nvals; b++)
				if (same_constant(def, &few->vals[a], &wide->vals[b]))
				{
					kept[m++] = few->vals[a];
					break;
				}
	}
	else
	{
		uint32 cap = pg_nextpower2_32(Max(32, (uint32) few->nvals * 2));
		/* check-memory: allow the constants of one predicate, bounded by the query */
		uint32 *ht = palloc0(cap * sizeof(uint32));
		bool *emitted = palloc0((size_t) few->nvals);

		for (int a = 0; a < few->nvals; a++)
		{
			uint32 at = constant_hash(def, &few->vals[a]) & (cap - 1);

			while (ht[at] != 0)
				at = (at + 1) & (cap - 1);
			ht[at] = a + 1;
		}
		for (int b = 0; b < wide->nvals; b++)
			for (uint32 at = constant_hash(def, &wide->vals[b]) & (cap - 1); ht[at] != 0; at = (at + 1) & (cap - 1))
				emitted[ht[at] - 1] |= same_constant(def, &few->vals[ht[at] - 1], &wide->vals[b]);
		for (int a = 0; a < few->nvals; a++)
			if (emitted[a])
				kept[m++] = few->vals[a];
		pfree(ht);
		pfree(emitted);
	}
	if (m == 0)
	{
		pfree(kept);
		return false;
	}
	wide->nvals = m;
	wide->vals = kept;
	return true;
}

/*
 * An IN list of the keys of a scope and an equality on the same leaf (the planner keeps both) are one test of the constants both hold: the list is cut to them, so that a
 * unit and a buffered row are tested against one constant and not against thousands (6 ms a scan against 0.8, 4,650 keys and one). False when none is common: the keys
 * cannot be satisfied. Only the leaves whose constants the write buffer's lookup compares exactly (sift_col_wbuf_lookup_ok).
 */
static bool
narrow_lists(int nkeys, ColKeys *k, SiftColPred *preds)
{
	for (int i = 0; i < nkeys; i++)
		for (int j = 0; j < nkeys; j++)
		{
			SiftColPred *wide = &preds[i];
			const SiftColPred *few = &preds[j];
			const SiftColDef *def = &k->defs[wide->col];

			if (i == j || wide->col != few->col || wide->nvals <= few->nvals || !sift_col_wbuf_lookup_ok(def, wide) || !sift_col_wbuf_lookup_ok(def, few))
				continue;
			if (!common_constants(def, few, wide))
				return false;
		}
	return true;
}

/* jsonb_exists(document, key): the leaf that keeps the key; a key no leaf keeps leaves the rows to the heap (col -1) */
static bool
key_exists_pred(ColKeys *k, const ScanKeyData *key, SiftColPred *p)
{
	text	   *name;

	if (key->sk_flags & SK_ISNULL)
		return false;
	if (k->key_leaf == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the operator ? of sift.col_jsonb_ops needs an index of format major 2"),
						errhint("REINDEX INDEX CONCURRENTLY rebuilds the index in format major 2.")));
	name = DatumGetTextPP(key->sk_argument);
	*p = (SiftColPred) {.col = k->key_leaf(k->key_leaf_arg, key->sk_attno, VARDATA_ANY(name), VARSIZE_ANY_EXHDR(name)),.op = SIFT_COLOP_KEY_EXISTS};
	k->recheck |= p->col < 0;
	return true;
}

/* IS [NOT] NULL on a jsonb column is a test of the document, which only its "<column>#present" leaf holds; an index with none (a column that cannot be NULL, an index built before the leaf) leaves it to the heap (col -1) */
static void
json_null_pred(ColKeys *k, const ScanKeyData *key, SiftColPred *p)
{
	int			leaf = -1;

	for (int i = k->first[key->sk_attno]; i < k->ndefs && k->defs[i].attno == key->sk_attno && leaf < 0; i++)
		if (sift_col_def_is_present(&k->defs[i]))
			leaf = i;
	*p = (SiftColPred) {.col = leaf,.op = (key->sk_flags & SK_SEARCHNULL) ? SIFT_COLOP_ISNULL : SIFT_COLOP_NOTNULL};
	k->recheck |= leaf < 0;
}

/* The predicates of the keys; false when one cannot be satisfied (an operator is strict: a NULL operand matches nothing) */
bool
sift_col_key_preds(const ScanKeyData *keys, int nkeys, ColKeys *k, SiftColPred *preds)
{
	for (int i = 0; i < nkeys; i++)
	{
		const ScanKeyData *key = &keys[i];
		int			leaf = k->first[key->sk_attno];
		Form_pg_attribute att = TupleDescAttr(k->idesc, key->sk_attno - 1);
		Oid			argtype = OidIsValid(key->sk_subtype) ? key->sk_subtype : att->atttypid;
		SiftColPred *p = &preds[i];

		*p = (SiftColPred) {.col = leaf,.collid = k->defs[leaf].store == SIFT_COLS_STR ? key->sk_collation : InvalidOid};
		if (key->sk_strategy == COL_STRAT_JSONB_EXISTS)
		{
			if (!key_exists_pred(k, key, p))
				return false;
			continue;
		}
		if (key->sk_strategy == COL_STRAT_JSONB_TYPE)
		{
			SiftColValue *v = palloc0(sizeof(SiftColValue));

			v->v.i = COL_JSON_TYPE_MASK(DatumGetInt64(key->sk_argument));
			*p = (SiftColPred) {.col = COL_JSON_TYPE_LEAF(DatumGetInt64(key->sk_argument)),.op = SIFT_COLOP_JSON_TYPE,.nvals = 1,.vals = v};
			continue;
		}
		if (att->atttypid == JSONBOID && (key->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)))
		{
			json_null_pred(k, key, p);
			continue;
		}
		/* an array without its image: its null bitmap is the cardinality's, and a comparison with an array is the heap's to decide for every array there is */
		if (k->defs[leaf].flags & SIFT_COLD_NOIMAGE)
		{
			p->col = leaf + 1;
			if (!(key->sk_flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL)))
			{
				key_any(p, k);
				continue;
			}
		}
		if (key->sk_flags & SK_SEARCHNULL)
			p->op = SIFT_COLOP_ISNULL;
		else if (key->sk_flags & SK_SEARCHNOTNULL)
			p->op = SIFT_COLOP_NOTNULL;
		else if (key->sk_flags & SK_ISNULL)
			return false;
		else if (!leaf_decides(&k->defs[leaf], att->atttypid, key->sk_collation))
			key_any(p, k);
		else if (key->sk_strategy == COL_STRAT_NE && sift_col_def_shape(&k->defs[leaf]))
			key_any(p, k);
		else if (sift_col_def_shape(&k->defs[leaf]))
		{
			/* a containment test the lanes decide (an overflow row by PostgreSQL's operator, col/shape.c): the heap needs no second look, but for the host bits of = on an address; an IN list is left to it */
			bool		nul = false;
			SiftColWTest *t = palloc(sizeof(SiftColWTest));
			SiftColValue *v = palloc0(sizeof(SiftColValue));
			int			op;

			if (key->sk_flags & SK_SEARCHARRAY)
			{
				key_any(p, k);
				continue;
			}
			op = shape_op(&k->defs[leaf], key->sk_strategy);
			sift_colw_test(&k->defs[leaf], att->atttypid, op, 1, &key->sk_argument, &nul, CurrentMemoryContext, t);
			v->v.s = (const char *) t;
			p->op = SIFT_COLOP_SHAPE;
			p->nvals = 1;
			p->vals = v;
		}
		else if (key->sk_strategy >= COL_STRAT_RANGE_FIRST || (key->sk_strategy == BTEqualStrategyNumber && type_is_range(getBaseType(att->atttypid))) ||
				 (key->sk_strategy == COL_STRAT_NE && ((argtype != att->atttypid && !sift_int_pair(att->atttypid, argtype)) || k->defs[leaf].type == SIFT_COLT_NUMERIC)))
			key_any(p, k);			/* a containment operator or = on a range the lanes do not cover (numrange, a range of a user type): the heap decides */
		else if (k->defs[leaf].type == SIFT_COLT_NUMERIC)
		{
			if (!numeric_preds(&k->defs[leaf], key, p, k))
				return false;
		}
		else if ((key->sk_flags & SK_SEARCHARRAY) && key->sk_strategy > BTGreaterStrategyNumber)
			key_any(p, k);
		else if (key->sk_flags & SK_SEARCHARRAY)
		{
			ArrayType  *arr = DatumGetArrayTypeP(key->sk_argument);
			Oid			elemtype = ARR_ELEMTYPE(arr);
			int16		len;
			bool		byval;
			char		align;
			Datum	   *el;
			bool	   *nl;
			int			n,
						m = 0;
			bool		nan = false;
			SiftColValue *vals;

			/* a list of another temporal type than the column's: the heap tests it */
			if (xtime_pair(att->atttypid, elemtype))
			{
				key_any(p, k);
				continue;
			}
			get_typlenbyvalalign(elemtype, &len, &byval, &align);
			deconstruct_array(arr, elemtype, len, byval, align, &el, &nl, &n);
			vals = palloc(Max(n, 1) * sizeof(SiftColValue));
			for (int j = 0; j < n; j++)
				if (!nl[j])
				{
					key_value(elemtype, att->atttypmod, el[j], &vals[m]);
					nan |= key_is_float(elemtype) && isnan(vals[m].v.f);
					m++;
				}
			if (m == 0)
				return false;
			if (nan)
			{
				key_any(p, k);
				continue;
			}
			/* x < ANY (a) is x < max(a) and x > ANY (a) is x > min(a); only the integer-like, float and boolean keys have order operators */
			if (key->sk_strategy != BTEqualStrategyNumber)
			{
				int			best = 0;
				bool		up = key->sk_strategy == BTLessStrategyNumber || key->sk_strategy == BTLessEqualStrategyNumber;

				for (int j = 1; j < m; j++)
				{
					bool		more = key_is_float(elemtype) ? vals[j].v.f > vals[best].v.f : vals[j].v.i > vals[best].v.i;
					bool		less = key_is_float(elemtype) ? vals[j].v.f < vals[best].v.f : vals[j].v.i < vals[best].v.i;

					if (up ? more : less)
						best = j;
				}
				vals[0] = vals[best];
				m = 1;
				p->op = strategy_op(key->sk_strategy);
			}
			else
				p->op = SIFT_COLOP_IN;
			p->nvals = m;
			p->vals = vals;
		}
		else
		{
			SiftColValue *v = palloc(sizeof(SiftColValue));
			int			strat = key->sk_strategy;
			int64		bound;
			int			x = xtime_bound(att->atttypid, argtype, key->sk_argument, &strat, &bound);

			if (x == 3)
				return false;
			if (x == 2)
			{
				key_any(p, k);
				continue;
			}
			if (x == 1)
			{
				*v = (SiftColValue) {.tag = SIFT_COLTAG_UNKNOWN};
				v->v.i = bound;
				p->op = strategy_op(strat);
				p->nvals = 1;
				p->vals = v;
				continue;
			}
			key_value(argtype, att->atttypmod, key->sk_argument, v);
			if (key->sk_strategy == COL_STRAT_LIKE || key->sk_strategy == COL_STRAT_STARTS_WITH)
			{
				like_pred(k, p, v, key->sk_strategy == COL_STRAT_STARTS_WITH, k->infix);
				continue;
			}
			if (key->sk_strategy == COL_STRAT_NOT_LIKE)
			{
				not_like_pred(p, v);
				continue;
			}
			p->op = strategy_op(key->sk_strategy);
			p->nvals = 1;
			p->vals = v;
		}
	}
	return narrow_lists(nkeys, k, preds);
}

int
sift_col_preds_keep(SiftColPred *preds, int n)
{
	int			kept = 0;

	for (int i = 0; i < n; i++)
		if (preds[i].col >= 0)
			preds[kept++] = preds[i];
	return kept;
}

bool
scan_preds(IndexScanDesc scan, ColKeys *k, SiftColPred *preds)
{
	return sift_col_key_preds(scan->keyData, scan->numberOfKeys, k, preds);
}
