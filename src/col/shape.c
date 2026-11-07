/*
 * col/shape.c - ranges and networks as integer lanes. The head of a range or an inet/cidr column stays the image of the
 * datum; the leaves after it hold what the containment operators compare, so zones, the integer kernels and the unit statistics apply to them:
 *
 *   range (3 leaves): lane 1 and 2 are the first and the last position of the set on a line where the value v is at 2v, "just after v" at 2v+1
 *     and "just before v" at 2v-1: [a, b) is (2a, 2b-1), (a, b] is (2a+1, 2b), an unbounded side is the least or greatest int64, the empty range is
 *     (INT64_MAX, INT64_MIN). Any operator of PostgreSQL's on two ranges is then a comparison of these positions, inclusive and exclusive bounds
 *     and real-valued ranges included. A bound with |v| >= 2^62 does not fit: the lanes are stored as an overflow (NULL with the flag), which the
 *     engine keeps as a candidate of every test, and PostgreSQL's own operator decides it.
 *   network (3 leaves): lane 2 is the offset of an IPv4 address inside its block (0 for a host or a network address; NULL for IPv6), so the node and the offset
 *     are the address and the mask of the value and `=` is exact on the lanes. Lane 1 is the node of the block of addresses in the binary tree of prefixes, as the doubled middle of the block, 2 * first +
 *     size (size = 2^(32 - mask), IPv4; the high 62 bits for IPv6, shifted below zero so that the families never meet): the address and the mask are
 *     one number. The blocks inside a block have their node between 2 * first + 1 and 2 * last + 1, a containing block is one of the mask + 1 nodes on
 *     the way to it (an IN list), and no other node is in either set. A mask over 62 bits of IPv6 is a node of the 62nd bit: a test of such a mask
 *     is answered by PostgreSQL's operator on the candidates.
 */
#include "postgres.h"

#include <sys/socket.h>

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/typcache.h"

#include "col/am.h"

#define SPAN_FIT	(INT64CONST(1) << 62)
#define SPAN_NONE	PG_INT64_MAX	/* first position of the empty range */
#define SPAN_NEVER	PG_INT64_MIN	/* its last */
#define NET_W4		32
#define NET_W6		62

typedef struct SiftColShape
{
	int			nalt;			/* tests whose union is the answer (adjacency has two) */
	int			npred[2];		/* each is the intersection of its predicates */
	SiftColPred pred[2][SIFT_COL_SHAPE_PREDS];
	SiftColValue val[2][SIFT_COL_SHAPE_PREDS];
	SiftColValue anc[NET_W6 + 1];	/* the nodes of the blocks on the way to a network: those that contain it (net_ancestors) */
	bool		decide_all;		/* the constant has no lane form: PostgreSQL's operator on every non-NULL row */
	bool		decide_rows;	/* the lanes only select candidates: PostgreSQL's operator decides them (an IPv6 mask over what the node holds) */
	FmgrInfo	fn;				/* the operator as a function of (image, operand) */
	Datum		operand;
	bool		decided;		/* fn is set */
	bool		unary;			/* fn is a function of the image alone (isempty, lower_inf ..) */
	bool		negate;			/* the test is NOT fn(image) */
	int			parity;			/* a range test of the inclusive bounds: the lane (1 + first, 2 + last position) whose parity the rows that pass the lanes are kept by, 0 none */
} SiftColShape;

/* ---- the lanes of a value ---- */

/* the types of range and network the lanes cover: the subtype is compared by its default btree order */
int
sift_col_shape_of(Oid typid)
{
	TypeCacheEntry *tc;

	if (typid == INETOID || typid == CIDROID)
		return SIFT_COL_SHAPE_INET;
	if (!type_is_range(typid))
		return 0;
	tc = lookup_type_cache(typid, TYPECACHE_RANGE_INFO);
	if (tc->rngelemtype == NULL || lookup_type_cache(tc->rngelemtype->type_id, TYPECACHE_CMP_PROC)->cmp_proc != tc->rng_cmp_proc_finfo.fn_oid)
		return 0;
	switch (tc->rngelemtype->type_id)
	{
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return SIFT_COL_SHAPE_RANGE;
	}
	return 0;
}

static int64
element_key(Oid sub, Datum d)
{
	switch (sub)
	{
		case INT4OID:
			return DatumGetInt32(d);
		case DATEOID:
			return DatumGetDateADT(d);
		default:
			return DatumGetInt64(d);
	}
}

/* the positions of a range's first and last element; false when a bound does not fit */
static bool
range_span(Datum d, int64 *first, int64 *last)
{
	RangeType  *r = DatumGetRangeTypeP(d);
	TypeCacheEntry *tc = lookup_type_cache(RangeTypeGetOid(r), TYPECACHE_RANGE_INFO);
	RangeBound	lo,
				hi;
	bool		empty;

	range_deserialize(tc, r, &lo, &hi, &empty);
	*first = SPAN_NONE;
	*last = SPAN_NEVER;
	if (empty)
		return true;
	*first = PG_INT64_MIN;
	*last = PG_INT64_MAX;
	if (!lo.infinite)
	{
		int64		v = element_key(tc->rngelemtype->type_id, lo.val);

		if (v <= -SPAN_FIT || v >= SPAN_FIT)
			return false;
		*first = 2 * v + !lo.inclusive;
	}
	if (!hi.infinite)
	{
		int64		v = element_key(tc->rngelemtype->type_id, hi.val);

		if (v <= -SPAN_FIT || v >= SPAN_FIT)
			return false;
		*last = 2 * v - !hi.inclusive;
	}
	return true;
}

/* the bits of an address the prefix tree of a family holds */
static int
net_width(int family)
{
	return family == PGSQL_AF_INET ? NET_W4 : NET_W6;
}

/* the leading net_width bits of the address, as an integer */
static uint64
net_prefix(const inet *ip)
{
	const unsigned char *a = ip_addr(ip);
	uint64		v = 0;

	if (ip_family(ip) == PGSQL_AF_INET)
		return ((uint64) a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
	for (int i = 0; i < 8; i++)
		v = v << 8 | a[i];
	return v >> (64 - NET_W6);
}

/* the node of the block of the first `depth` bits of the prefix t of a tree of width w; *lo .. *hi are the nodes of the blocks inside it */
static int64
net_node(int family, uint64 t, int depth, int64 *lo, int64 *hi)
{
	int			w = net_width(family),
				h = w - Min(depth, w);
	uint64		first = (t >> h) << h;
	int64		off = family == PGSQL_AF_INET ? 0 : PG_INT64_MIN;

	if (lo)
		*lo = (int64) (2 * first + 1) + off;
	if (hi)
		*hi = (int64) (2 * first + 2 * ((UINT64CONST(1) << h) - 1) + 1) + off;
	return (int64) (2 * first + (UINT64CONST(1) << h)) + off;
}

/* the lanes of a datum of the head def: v[0] is the head's, v[1 ...] are set here */
void
sift_col_shape_values(const SiftColDef *def, Datum d, SiftColValue *v)
{
	if (sift_col_def_shape(def) == SIFT_COL_SHAPE_RANGE)
	{
		if (range_span(d, &v[1].v.i, &v[2].v.i))
			return;
		v[1].isnull = v[1].overflow = v[2].isnull = v[2].overflow = true;
	}
	else
	{
		inet	   *ip = DatumGetInetPP(d);
		int			family = ip_family(ip),
					h = family == PGSQL_AF_INET ? NET_W4 - Min(ip_bits(ip), NET_W4) : 0;
		uint64		t = net_prefix(ip);

		v[1].v.i = net_node(family, t, ip_bits(ip), NULL, NULL);
		if (family == PGSQL_AF_INET)
			v[2].v.i = (int64) (t & ((UINT64CONST(1) << h) - 1));
		else
			v[2].isnull = true;		/* the address of a family 6 value does not fit the node and this lane: its equality is decided by PostgreSQL's operator */
	}
}

/* ---- tests ---- */

static void
add(SiftColShape *sh, int alt, int col, SiftColOp op, int64 key)
{
	int			k = sh->npred[alt]++;

	sh->val[alt][k] = (SiftColValue) {.v.i = key};
	sh->pred[alt][k] = (SiftColPred) {col, op, 1, &sh->val[alt][k]};
}

/* the rows whose lane is in [lo, hi] */
static void
between(SiftColShape *sh, int alt, int col, int64 lo, int64 hi)
{
	if (lo > PG_INT64_MIN)
		add(sh, alt, col, SIFT_COLOP_GE, lo);
	if (hi < PG_INT64_MAX)
		add(sh, alt, col, SIFT_COLOP_LE, hi);
}

static void
range_test(SiftColShape *sh, int col, int op, int64 lc, int64 uc)
{
	int			s = col + 1,
				e = col + 2;

	switch (op)
	{
		case SIFT_COLW_R_CONTAINS:
			add(sh, 0, s, SIFT_COLOP_LE, lc);
			add(sh, 0, e, SIFT_COLOP_GE, uc);
			break;
		case SIFT_COLW_R_ELEM:
			add(sh, 0, s, SIFT_COLOP_LE, lc);
			add(sh, 0, e, SIFT_COLOP_GE, lc);
			break;
		case SIFT_COLW_R_EQ:
			between(sh, 0, s, lc, lc);
			between(sh, 0, e, uc, uc);
			break;
		case SIFT_COLW_R_CONTAINED:
			add(sh, 0, s, SIFT_COLOP_GE, lc);
			add(sh, 0, e, SIFT_COLOP_LE, uc);
			break;
		case SIFT_COLW_R_OVERLAPS:
			add(sh, 0, s, SIFT_COLOP_LE, uc);
			add(sh, 0, e, SIFT_COLOP_GE, lc);
			if (uc == PG_INT64_MAX && lc == PG_INT64_MIN)
				add(sh, 0, s, SIFT_COLOP_LT, SPAN_NONE);	/* the empty range overlaps nothing */
			break;
		case SIFT_COLW_R_BEFORE:
			add(sh, 0, e, SIFT_COLOP_GT, SPAN_NEVER);
			add(sh, 0, e, SIFT_COLOP_LT, lc);
			break;
		case SIFT_COLW_R_AFTER:
			add(sh, 0, s, SIFT_COLOP_LT, SPAN_NONE);
			add(sh, 0, s, SIFT_COLOP_GT, uc);
			break;
		case SIFT_COLW_R_OVERLEFT:
			add(sh, 0, e, SIFT_COLOP_GT, SPAN_NEVER);
			add(sh, 0, e, SIFT_COLOP_LE, uc);
			break;
		case SIFT_COLW_R_OVERRIGHT:
			add(sh, 0, s, SIFT_COLOP_LT, SPAN_NONE);
			add(sh, 0, s, SIFT_COLOP_GE, lc);
			break;
		case SIFT_COLW_R_ADJACENT:
			sh->nalt = 0;
			if (lc > PG_INT64_MIN)
				add(sh, sh->nalt++, e, SIFT_COLOP_EQ, lc - 1);
			if (uc < PG_INT64_MAX)
				add(sh, sh->nalt++, s, SIFT_COLOP_EQ, uc + 1);
			break;
	}
}

/*
 * The blocks of 0 .. maxdepth bits on the way to the network contain it, and a node names one block (its first address and its size), so the rows whose node is one of
 * those nodes are exactly the blocks that contain the network: an IN list of at most 63 nodes, which the units are pruned by as well.
 */
static void
net_ancestors(SiftColShape *sh, int alt, int lane, const inet *ip, int maxdepth)
{
	int			family = ip_family(ip),
				n = Min(maxdepth, net_width(family));
	uint64		t = net_prefix(ip);
	int			k = sh->npred[alt]++;

	for (int d = 0; d <= n; d++)
		sh->anc[d] = (SiftColValue) {.v.i = net_node(family, t, d, NULL, NULL)};
	sh->pred[alt][k] = (SiftColPred) {lane, SIFT_COLOP_IN, n + 1, sh->anc};
}

static void
net_test(SiftColShape *sh, int col, int op, Datum operand)
{
	inet	   *ip = DatumGetInetPP(operand);
	int			family = ip_family(ip),
				bits = ip_bits(ip),
				lane = col + 1;
	int64		lo,
				hi,
				node = net_node(family, net_prefix(ip), bits, &lo, &hi);

	/* the node of a mask over what it holds is the node of the rows with the same first bits, strictly inside or not: they are all candidates */
	sh->decide_rows = family == PGSQL_AF_INET6 && (bits >= NET_W6 || op == SIFT_COLW_N_EQ);
	if (sh->decide_rows)
		op = op == SIFT_COLW_N_SUB ? SIFT_COLW_N_SUBEQ : op == SIFT_COLW_N_SUP ? SIFT_COLW_N_SUPEQ : op;
	switch (op)
	{
		case SIFT_COLW_N_EQ:
			add(sh, 0, lane, SIFT_COLOP_EQ, node);
			if (family == PGSQL_AF_INET)
				add(sh, 0, lane + 1, SIFT_COLOP_EQ, (int64) (net_prefix(ip) & ((UINT64CONST(1) << (NET_W4 - bits)) - 1)));
			break;
		case SIFT_COLW_N_SUBEQ:
			between(sh, 0, lane, lo, hi);
			break;
		case SIFT_COLW_N_SUB:
			sh->nalt = 0;
			if (lo < node)
				between(sh, sh->nalt++, lane, lo, node - 1);
			if (node < hi)
				between(sh, sh->nalt++, lane, node + 1, hi);
			break;
		case SIFT_COLW_N_SUPEQ:
			net_ancestors(sh, 0, lane, ip, bits);
			break;
		case SIFT_COLW_N_SUP:
			if (bits == 0)
				sh->nalt = 0;	/* nothing contains the whole space strictly */
			else
				net_ancestors(sh, 0, lane, ip, bits - 1);
			break;
		case SIFT_COLW_N_OVERLAP:
			between(sh, 0, lane, lo, hi);
			if (bits > 0)
			{
				sh->nalt = 2;
				net_ancestors(sh, 1, lane, ip, bits - !sh->decide_rows);
			}
			break;
	}
}

/* PostgreSQL's operator of the test, as a function of (the column's value, the operand) */
static const Oid shape_fn[] = {
	[SIFT_COLW_R_CONTAINS - SIFT_COLW_SHAPE_FIRST] = F_RANGE_CONTAINS,
	[SIFT_COLW_R_CONTAINED - SIFT_COLW_SHAPE_FIRST] = F_RANGE_CONTAINED_BY,
	[SIFT_COLW_R_OVERLAPS - SIFT_COLW_SHAPE_FIRST] = F_RANGE_OVERLAPS,
	[SIFT_COLW_R_BEFORE - SIFT_COLW_SHAPE_FIRST] = F_RANGE_BEFORE,
	[SIFT_COLW_R_AFTER - SIFT_COLW_SHAPE_FIRST] = F_RANGE_AFTER,
	[SIFT_COLW_R_OVERLEFT - SIFT_COLW_SHAPE_FIRST] = F_RANGE_OVERLEFT,
	[SIFT_COLW_R_OVERRIGHT - SIFT_COLW_SHAPE_FIRST] = F_RANGE_OVERRIGHT,
	[SIFT_COLW_R_ADJACENT - SIFT_COLW_SHAPE_FIRST] = F_RANGE_ADJACENT,
	[SIFT_COLW_R_ELEM - SIFT_COLW_SHAPE_FIRST] = F_RANGE_CONTAINS_ELEM,
	[SIFT_COLW_N_SUB - SIFT_COLW_SHAPE_FIRST] = F_NETWORK_SUB,
	[SIFT_COLW_N_SUBEQ - SIFT_COLW_SHAPE_FIRST] = F_NETWORK_SUBEQ,
	[SIFT_COLW_N_SUP - SIFT_COLW_SHAPE_FIRST] = F_NETWORK_SUP,
	[SIFT_COLW_N_SUPEQ - SIFT_COLW_SHAPE_FIRST] = F_NETWORK_SUPEQ,
	[SIFT_COLW_N_OVERLAP - SIFT_COLW_SHAPE_FIRST] = F_NETWORK_OVERLAP,
	[SIFT_COLW_R_EQ - SIFT_COLW_SHAPE_FIRST] = F_RANGE_EQ,
	[SIFT_COLW_N_EQ - SIFT_COLW_SHAPE_FIRST] = F_NETWORK_EQ,
	[SIFT_COLW_R_EMPTY - SIFT_COLW_SHAPE_FIRST] = F_ISEMPTY_ANYRANGE,
	[SIFT_COLW_R_NONEMPTY - SIFT_COLW_SHAPE_FIRST] = F_ISEMPTY_ANYRANGE,
	[SIFT_COLW_R_LOWER_INF - SIFT_COLW_SHAPE_FIRST] = F_LOWER_INF_ANYRANGE,
	[SIFT_COLW_R_LOWER_FIN - SIFT_COLW_SHAPE_FIRST] = F_LOWER_INF_ANYRANGE,
	[SIFT_COLW_R_UPPER_INF - SIFT_COLW_SHAPE_FIRST] = F_UPPER_INF_ANYRANGE,
	[SIFT_COLW_R_UPPER_FIN - SIFT_COLW_SHAPE_FIRST] = F_UPPER_INF_ANYRANGE,
	[SIFT_COLW_R_LOWER_INC - SIFT_COLW_SHAPE_FIRST] = F_LOWER_INC_ANYRANGE,
	[SIFT_COLW_R_LOWER_EXC - SIFT_COLW_SHAPE_FIRST] = F_LOWER_INC_ANYRANGE,
	[SIFT_COLW_R_UPPER_INC - SIFT_COLW_SHAPE_FIRST] = F_UPPER_INC_ANYRANGE,
	[SIFT_COLW_R_UPPER_EXC - SIFT_COLW_SHAPE_FIRST] = F_UPPER_INC_ANYRANGE
};

/*
 * The tests of a range with no operand, on the first and last positions. A row whose lanes are an overflow is decided by PostgreSQL's function, as for the operators. The ranges of
 * the discrete subtypes are canonical, [a, b), whenever the bound is a value: lower_inc is true of every finite lower bound and upper_inc of no finite upper one. A date that is
 * infinity stays as written (daterange('-infinity', 'infinity', '[]')): its one position, 2 * INT32_MAX for an inclusive upper bound and the exclusive -infinity just after 2 * INT32_MIN
 * for a lower one, is a test of its own. The continuous subtypes hold either, and the position tells which: an inclusive lower bound is the even position 2v, an exclusive one 2v + 1,
 * and the last position is 2v for an inclusive upper bound and 2v - 1 for an exclusive one, so the lane of the bound (a finite one, a non-empty range) kept by its parity is exact.
 */
static void
range_unary(SiftColShape *sh, SiftColWTest *t, int col, Oid typid, int op)
{
	int			s = col + 1,
				e = col + 2;
	Oid			sub = InvalidOid;
	bool		continuous;

	if (op >= SIFT_COLW_R_LOWER_INC)
		sub = lookup_type_cache(typid, TYPECACHE_RANGE_INFO)->rngelemtype->type_id;
	continuous = sub != INT4OID && sub != INT8OID && sub != DATEOID;
	sh->unary = true;
	sh->negate = op == SIFT_COLW_R_NONEMPTY || op == SIFT_COLW_R_LOWER_FIN || op == SIFT_COLW_R_UPPER_FIN || op == SIFT_COLW_R_LOWER_EXC || op == SIFT_COLW_R_UPPER_EXC;
	switch (op)
	{
		case SIFT_COLW_R_EMPTY:
			add(sh, 0, s, SIFT_COLOP_EQ, SPAN_NONE);
			break;
		case SIFT_COLW_R_NONEMPTY:
			add(sh, 0, s, SIFT_COLOP_LT, SPAN_NONE);
			break;
		case SIFT_COLW_R_LOWER_INF:
			add(sh, 0, s, SIFT_COLOP_EQ, PG_INT64_MIN);
			break;
		case SIFT_COLW_R_LOWER_FIN:
			add(sh, 0, s, SIFT_COLOP_GT, PG_INT64_MIN);
			break;
		case SIFT_COLW_R_UPPER_INF:
			add(sh, 0, e, SIFT_COLOP_EQ, PG_INT64_MAX);
			break;
		case SIFT_COLW_R_UPPER_FIN:
			add(sh, 0, e, SIFT_COLOP_LT, PG_INT64_MAX);
			break;
		case SIFT_COLW_R_LOWER_INC:
			add(sh, 0, s, SIFT_COLOP_GT, PG_INT64_MIN);
			add(sh, 0, s, SIFT_COLOP_LT, SPAN_NONE);
			if (sub == DATEOID)
				add(sh, 0, s, SIFT_COLOP_NE, 2 * (int64) DATEVAL_NOBEGIN + 1);
			sh->parity = continuous ? s : 0;
			break;
		case SIFT_COLW_R_LOWER_EXC:
			/* the rows the test above leaves: an empty range, an infinite bound (a date also the exclusive -infinity) */
			if (continuous)
			{
				sh->pred[0][sh->npred[0]++] = (SiftColPred) {s, SIFT_COLOP_NOTNULL, 0, NULL};
				sh->parity = s;
			}
			else
			{
				sh->nalt = 2;
				if (sub == DATEOID)
				{
					add(sh, 0, s, SIFT_COLOP_LE, 2 * (int64) DATEVAL_NOBEGIN + 1);
					add(sh, 0, s, SIFT_COLOP_NE, 2 * (int64) DATEVAL_NOBEGIN);
				}
				else
					add(sh, 0, s, SIFT_COLOP_EQ, PG_INT64_MIN);
				add(sh, 1, s, SIFT_COLOP_EQ, SPAN_NONE);
			}
			break;
		case SIFT_COLW_R_UPPER_INC:
			if (sub == INT4OID || sub == INT8OID)
				t->verdict = SIFT_COLW_NONE;
			else if (sub == DATEOID)
				add(sh, 0, e, SIFT_COLOP_EQ, 2 * (int64) DATEVAL_NOEND);
			else
			{
				add(sh, 0, e, SIFT_COLOP_GT, SPAN_NEVER);
				sh->parity = e;
			}
			break;
		case SIFT_COLW_R_UPPER_EXC:
			if (sub == INT4OID || sub == INT8OID)
				t->verdict = SIFT_COLW_NONNULL;
			else if (sub == DATEOID)
				add(sh, 0, e, SIFT_COLOP_NE, 2 * (int64) DATEVAL_NOEND);
			else
			{
				sh->pred[0][sh->npred[0]++] = (SiftColPred) {e, SIFT_COLOP_NOTNULL, 0, NULL};
				sh->parity = e;
			}
			break;
	}
}

void
sift_col_shape_test(const SiftColDef *def, Oid typid, int op, Datum operand, MemoryContext mcxt, SiftColWTest *t)
{
	SiftColShape *sh = MemoryContextAllocZero(mcxt, sizeof(SiftColShape));
	int64		first = 0,
				last = 0;
	bool		fits = true,
				empty = false;

	sh->nalt = 1;
	sh->operand = operand;
	fmgr_info_cxt(shape_fn[op - SIFT_COLW_SHAPE_FIRST], &sh->fn, mcxt);
	t->verdict = SIFT_COLW_SHAPE;
	t->shape = sh;
	t->pred.col = def->id;
	if (op >= SIFT_COLW_R_UNARY)
	{
		range_unary(sh, t, def->id, typid, op);
		return;
	}
	if (sift_col_def_shape(def) == SIFT_COL_SHAPE_INET)
	{
		net_test(sh, def->id, op, operand);
		return;
	}
	if (op == SIFT_COLW_R_ELEM)
	{
		TypeCacheEntry *tc = lookup_type_cache(typid, TYPECACHE_RANGE_INFO);
		int64		v = element_key(tc->rngelemtype->type_id, operand);

		fits = v > -SPAN_FIT && v < SPAN_FIT;
		first = last = 2 * v;
	}
	else
	{
		fits = range_span(operand, &first, &last);
		empty = first == SPAN_NONE;
	}
	if (!fits)
		sh->decide_all = true;
	else if (empty && op != SIFT_COLW_R_EQ)
	{
		/* an empty constant: contained in every range, contains only empty rows, overlaps and precedes nothing */
		if (op == SIFT_COLW_R_CONTAINS)
			t->verdict = SIFT_COLW_NONNULL;
		else if (op == SIFT_COLW_R_CONTAINED)
			add(sh, 0, def->id + 2, SIFT_COLOP_LE, SPAN_NEVER);
		else
			t->verdict = SIFT_COLW_NONE;
	}
	else
		range_test(sh, def->id, op, first, last);
}

int
sift_colw_alts(const SiftColWTest *t, const SiftColPred *alt[2], int npred[2])
{
	const SiftColShape *sh = t->shape;

	if (t->verdict != SIFT_COLW_SHAPE || sh->decide_all)
		return 0;
	for (int a = 0; a < sh->nalt; a++)
	{
		alt[a] = sh->pred[a];
		npred[a] = sh->npred[a];
	}
	return sh->nalt;
}

/* PostgreSQL's operator on the images of the rows: clears the rows of `rows` it is not true for */
static void
decide(const SiftColUnit *unit, int col, const SiftColShape *sh, SiftColSel *rows)
{
	SiftColVec	v;
	uint32	   *idx;

	if (rows->count == 0)
		return;
	idx = palloc((rows->count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_read(unit, col, rows, &v);
	sift_col_sel_idx(rows, idx);
	for (uint32 k = 0; k < rows->count; k++)
	{
		uint32		len = SIFT_COL_DICT_LEN(v.dict, v.codes[k]);
		void	   *image = memcpy(palloc(len), SIFT_COL_DICT_STR(v.dict, v.codes[k]), len);

		if (DatumGetBool(sh->unary ? FunctionCall1(&((SiftColShape *) sh)->fn, PointerGetDatum(image))
						 : FunctionCall2(&((SiftColShape *) sh)->fn, PointerGetDatum(image), sh->operand)) == sh->negate)
			sift_col_bit_clear(rows->bits, idx[k]);
		pfree(image);
	}
	sift_col_sel_count(rows);
}

static void
or_into(SiftColSel *dst, const SiftColSel *src)
{
	for (uint32 w = 0; w < dst->nwords; w++)
		dst->bits[w] |= src->bits[w];
	sift_col_sel_count(dst);
}

static void
andnot_into(SiftColSel *dst, const SiftColSel *src)
{
	for (uint32 w = 0; w < dst->nwords; w++)
		dst->bits[w] &= ~src->bits[w];
	sift_col_sel_count(dst);
}

/*
 * Keeps the rows of `rows` whose bound is inclusive (the test) or exclusive (its NOT) by the parity of the position of the bound: even for an inclusive one, odd for an exclusive one,
 * and INT64_MIN, the position of an infinite lower bound (and of the last of an empty range), is no inclusive bound
 */
static void
parity_rows(const SiftColUnit *unit, const SiftColShape *sh, SiftColSel *rows)
{
	SiftColVec	v;
	uint32	   *idx;

	if (rows->count == 0)
		return;
	idx = palloc((rows->count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_read(unit, sh->parity, rows, &v);
	sift_col_sel_idx(rows, idx);
	for (uint32 k = 0; k < rows->count; k++)
		if (((v.i[k] & 1) == 0 && v.i[k] != PG_INT64_MIN) == sh->negate)
			sift_col_bit_clear(rows->bits, idx[k]);
	pfree(idx);
	sift_col_sel_count(rows);
}

bool
sift_col_shape_apply(const SiftColUnit *unit, const SiftColWTest *t, SiftColSel *sel)
{
	const SiftColShape *sh = t->shape;
	int			col = t->pred.col;
	SiftColSel *res,
			   *hard;

	/* the lanes alone decide a network test with one alternative (the node, then the offset on the rows it kept): the predicates narrow sel in place */
	if (sh->nalt == 1 && !sh->decide_all && !sh->decide_rows && sift_col_def_shape(sift_col_def(unit->src, col)) == SIFT_COL_SHAPE_INET)
	{
		if (!sift_col_filter(unit, sh->pred[0], sh->npred[0], sel))
			memset(sel->bits, 0, sel->nwords * sizeof(uint64));
		return sift_col_sel_count(sel) > 0;
	}
	res = sift_colw_sel_copy(unit, sel);
	hard = sift_colw_sel_copy(unit, sel);
	memset(res->bits, 0, res->nwords * sizeof(uint64));
	memset(hard->bits, 0, hard->nwords * sizeof(uint64));
	if (sh->decide_all)
	{
		SiftColPred nn = {col, SIFT_COLOP_NOTNULL, 0, NULL};

		if (sift_col_filter(unit, &nn, 1, sel))
			memcpy(hard->bits, sel->bits, hard->nwords * sizeof(uint64));
	}
	else
	{
		for (int a = 0; a < sh->nalt; a++)
		{
			SiftColSel *one = sift_colw_sel_copy(unit, sel);

			if (sift_col_filter(unit, sh->pred[a], sh->npred[a], one))
				or_into(res, one);
		}
		if (sift_col_def_shape(sift_col_def(unit->src, col)) == SIFT_COL_SHAPE_RANGE)
		{
			sift_colw_ovf_rows(unit, col + 1, sel, hard->bits);	/* stored as overflows: every test keeps them */
			andnot_into(res, hard);
		}
		if (sh->parity)
			parity_rows(unit, sh, res);
		if (sh->decide_rows)
		{
			or_into(hard, res);
			memset(res->bits, 0, res->nwords * sizeof(uint64));
		}
	}
	sift_col_sel_count(hard);
	decide(unit, col, sh, hard);
	or_into(res, hard);
	memcpy(sel->bits, res->bits, sel->nwords * sizeof(uint64));
	return sift_col_sel_count(sel) > 0;
}
