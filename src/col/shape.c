/*
 * col/shape.c - ranges and networks as integer lanes. The head of a range or an inet/cidr column stays the image of the
 * datum; the leaves after it hold what the containment operators compare, so zones, the integer kernels and the unit statistics apply to them:
 *
 *   range (3 leaves): lane 1 and 2 are the first and the last position of the set on a line where the value v is at 2v, "just after v" at 2v+1
 *     and "just before v" at 2v-1: [a, b) is (2a, 2b-1), (a, b] is (2a+1, 2b), an unbounded side is the least or greatest int64, the empty range is
 *     (INT64_MAX, INT64_MIN). Any operator of PostgreSQL's on two ranges is then a comparison of these positions, inclusive and exclusive bounds
 *     and real-valued ranges included. A bound with |v| >= 2^62 does not fit: the lanes are stored as an overflow (NULL with the flag), which the
 *     engine keeps as a candidate of every test, and PostgreSQL's own operator decides it.
 *   network (2 leaves): lane 1 is the node of the block of addresses in the binary tree of prefixes, as the doubled middle of the block, 2 * first +
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
#include "utils/timestamp.h"
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
	bool		anc[2];			/* the alternative selects the blocks that contain a network (ancestor_rows); its predicates only prune */
	int64		anc_leaf[2];	/* the node of the first address of the network */
	uint64		anc_size[2];	/* the least size of a block that contains it */
	bool		decide_all;		/* the constant has no lane form: PostgreSQL's operator on every non-NULL row */
	bool		decide_rows;	/* the lanes only select candidates: PostgreSQL's operator decides them (an IPv6 mask over what the node holds) */
	FmgrInfo	fn;				/* the operator as a function of (image, operand) */
	Datum		operand;
	bool		decided;		/* fn is set */
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

		v[1].v.i = net_node(ip_family(ip), net_prefix(ip), ip_bits(ip), NULL, NULL);
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
 * The rows whose node is one of the blocks of 0 .. maxdepth bits on the way to the network contain it: their node is within `size` of the leaf at its
 * first address (the node of a block is its middle, the distance to the ends its size) and of at least the size of a block of maxdepth bits. The lane
 * is only pruned by the least and the greatest of those nodes; the rows are tested by ancestor_rows.
 */
static void
net_ancestors(SiftColShape *sh, int alt, int lane, const inet *ip, int maxdepth)
{
	int			family = ip_family(ip),
				w = net_width(family),
				n = Min(maxdepth, w);
	uint64		t = net_prefix(ip);
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	for (int d = 0; d <= n; d++)
	{
		int64		k = net_node(family, t, d, NULL, NULL);

		lo = Min(lo, k);
		hi = Max(hi, k);
	}
	between(sh, alt, lane, lo, hi);
	sh->anc[alt] = true;
	sh->anc_leaf[alt] = net_node(family, t, w, NULL, NULL);
	sh->anc_size[alt] = UINT64CONST(1) << (w - n);
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
	sh->decide_rows = family == PGSQL_AF_INET6 && bits >= NET_W6;
	if (sh->decide_rows)
		op = op == SIFT_COLW_N_SUB ? SIFT_COLW_N_SUBEQ : op == SIFT_COLW_N_SUP ? SIFT_COLW_N_SUPEQ : op;
	switch (op)
	{
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
	[SIFT_COLW_N_OVERLAP - SIFT_COLW_SHAPE_FIRST] = F_NETWORK_OVERLAP
};

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
	else if (empty)
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
sift_colw_preds(const SiftColWTest *t, SiftColPred *out)
{
	const SiftColShape *sh = t->shape;

	if (t->verdict == SIFT_COLW_PRED)
		out[0] = t->pred;
	else if (t->verdict != SIFT_COLW_SHAPE || sh->decide_all || sh->nalt != 1)
		return 0;
	else
	{
		memcpy(out, sh->pred[0], sh->npred[0] * sizeof(SiftColPred));
		return sh->npred[0];
	}
	return 1;
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

		if (!DatumGetBool(FunctionCall2(&((SiftColShape *) sh)->fn, PointerGetDatum(image), sh->operand)))
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

/* The rows of sel whose node (lane) is the node of a block that contains the leaf anc_leaf and is at least anc_size big */
static void
ancestor_rows(const SiftColUnit *unit, int lane, const SiftColShape *sh, int alt, SiftColSel *sel)
{
	SiftColVec	v;
	int64		x = sh->anc_leaf[alt];
	uint64		least = sh->anc_size[alt];

	sift_col_read(unit, lane, NULL, &v);
	for (uint32 w = 0; w * 64 < v.n; w++)
	{
		uint64		bits = 0;

		for (uint32 i = w * 64; i < Min(v.n, (w + 1) * 64); i++)
		{
			int64		k = v.i[i];
			uint64		size = (uint64) k & -(uint64) k;	/* the offset of the IPv6 nodes is the sign bit, above every size */

			/* x - size < k < x + size, and the block is big enough: one unsigned comparison */
			bits |= (uint64) ((size >= least) & ((uint64) (x - k + (int64) size - 1) < 2 * size - 1)) << (i & 63);
		}
		sel->bits[w] &= bits & (v.nulls ? ~v.nulls[w] : ~UINT64CONST(0));
	}
	sift_col_sel_count(sel);
}

/* The rows of sel the test passes: the lanes' answer, where the lanes hold the value; the rows they do not decide, PostgreSQL's operator's */
bool
sift_col_shape_apply(const SiftColUnit *unit, const SiftColWTest *t, SiftColSel *sel)
{
	const SiftColShape *sh = t->shape;
	int			col = t->pred.col;
	SiftColSel *res = sift_colw_sel_copy(unit, sel),
			   *hard = sift_colw_sel_copy(unit, sel);

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

			if (sh->anc[a])
			{
				ancestor_rows(unit, col + 1, sh, a, one);
				or_into(res, one);
			}
			else if (sift_col_filter(unit, sh->pred[a], sh->npred[a], one))
				or_into(res, one);
		}
		if (sift_col_def_shape(sift_col_def(unit->src, col)) == SIFT_COL_SHAPE_RANGE)
		{
			sift_colw_ovf_rows(unit, col + 1, sel, hard->bits);	/* stored as overflows: every test keeps them */
			andnot_into(res, hard);
		}
		if (sh->decide_rows)
		{
			memcpy(hard->bits, res->bits, hard->nwords * sizeof(uint64));
			memset(res->bits, 0, res->nwords * sizeof(uint64));
		}
	}
	sift_col_sel_count(hard);
	decide(unit, col, sh, hard);
	or_into(res, hard);
	memcpy(sel->bits, res->bits, sel->nwords * sizeof(uint64));
	return sift_col_sel_count(sel) > 0;
}
