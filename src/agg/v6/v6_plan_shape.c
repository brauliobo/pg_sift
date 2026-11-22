/*
 * agg/v6/v6_plan_shape.c - the questions the planner asks before it offers the path of an operator of this directory (v6_plan_shape.h).
 */
#include "agg/v6/v6_plan_shape.h"

const char *
sift_shape_name(SiftShapeVerdict v)
{
	static const char *const names[] = {
		[SSV_OK] = "ok",
		[SSV_FILTER_TOO_MANY] = "more than 64 distinct FILTER clauses",
		[SSV_JOIN_TYPE] = "join type",
		[SSV_JOIN_NO_EQUI] = "no equality join clause",
		[SSV_JOIN_RESIDUAL] = "a join clause that is not an equality",
		[SSV_JOIN_UNHASHABLE] = "an equality without a hash function",
		[SSV_JOIN_FITS] = "build side fits in half of work_mem",
		[SSV_ASOF_JOIN_TYPE] = "asof: join type",
		[SSV_ASOF_SUBQUERY] = "asof: sub-select",
		[SSV_ASOF_LIMIT] = "asof: not LIMIT 1",
		[SSV_ASOF_OFFSET] = "asof: OFFSET",
		[SSV_ASOF_ORDER] = "asof: ORDER BY",
		[SSV_ASOF_NO_KEY] = "asof: no key equality",
		[SSV_ASOF_KEYS] = "asof: several key equalities",
		[SSV_ASOF_TS_QUAL] = "asof: time comparison",
		[SSV_ASOF_OTHER_QUAL] = "asof: another clause",
		[SSV_ASOF_ON] = "asof: join clause is not ON true",
		[SSV_ASOF_TYPE] = "asof: key or time type",
		[SSV_DATEBIN_ARGS] = "date_bin: stride or origin not a constant",
		[SSV_DATEBIN_STRIDE] = "date_bin: stride",
		[SSV_DATEBIN_MONTHS] = "date_bin: stride with months",
		[SSV_DATEBIN_ORIGIN] = "date_bin: origin",
		[SSV_DATEBIN_TYPE] = "date_bin: source type",
		[SSV_WIN_FUNC] = "window: function",
		[SSV_WIN_TYPE] = "window: argument type",
		[SSV_WIN_FRAME] = "window: mode or frame",
		[SSV_WIN_TOO_MANY] = "window: too many functions",
		[SSV_WIN_NO_NEED] = "window: the executor of agg/window.h has it",
	};

	return (unsigned) v < lengthof(names) ? names[v] : "unknown";
}

/* ---- FILTER ---- */

struct SiftFilterReg
{
	MemoryContext cx;
	int			n;
	uint8	   *bytes[SIFT_PIVOT_MAX_FILTERS];
	uint32		len[SIFT_PIVOT_MAX_FILTERS];
};

SiftFilterReg *
sift_freg_create(MemoryContext cx)
{
	SiftFilterReg *r = MemoryContextAllocZero(cx, sizeof(SiftFilterReg));

	r->cx = cx;
	return r;
}

int
sift_freg_add(SiftFilterReg *r, const void *canon, uint32 len)
{
	for (int i = 0; i < r->n; i++)
		if (r->len[i] == len && (len == 0 || memcmp(r->bytes[i], canon, len) == 0))
			return i;
	if (r->n == SIFT_PIVOT_MAX_FILTERS)
		return -1;
	r->bytes[r->n] = MemoryContextAlloc(r->cx, Max(len, 1));
	if (len > 0)
		memcpy(r->bytes[r->n], canon, len);
	r->len[r->n] = len;
	return r->n++;
}

int
sift_freg_count(const SiftFilterReg *r)
{
	return r->n;
}

void
sift_freg_free(SiftFilterReg *r)
{
	for (int i = 0; i < r->n; i++)
		pfree(r->bytes[i]);
	pfree(r);
}

/* ---- Grace join ---- */

/* an entry of the hash table (a hash, a pointer, a length) and the header of the stored tuple, on top of its width */
#define SHAPE_BUILD_ROW_OVERHEAD	40.0

uint64
sift_shape_build_bytes(double rows, double width)
{
	const double bytes = rows * (width + SHAPE_BUILD_ROW_OVERHEAD);

	return bytes >= 9.0e18 ? UINT64_MAX / 2 : (uint64) bytes;
}

SiftShapeVerdict
sift_shape_join(const SiftJoinShape *s, SiftJoinType *type)
{
	switch (s->type)
	{
		case SSJ_INNER:
			*type = SGJ_INNER;
			break;
		case SSJ_LEFT:
			*type = SGJ_LEFT;
			break;
		case SSJ_SEMI:
			*type = SGJ_SEMI;
			break;
		case SSJ_ANTI:
			*type = SGJ_ANTI;
			break;
		default:
			return SSV_JOIN_TYPE;
	}
	if (s->nequi == 0)
		return SSV_JOIN_NO_EQUI;
	if (s->nother > 0)
		return SSV_JOIN_RESIDUAL;
	if (s->nunhashable > 0)
		return SSV_JOIN_UNHASHABLE;
	if (sift_shape_build_bytes(s->build_rows, s->build_width) <= s->work_mem_bytes / 2)
		return SSV_JOIN_FITS;
	return SSV_OK;
}

/* ---- ASOF ---- */

SiftShapeVerdict
sift_shape_asof(const SiftAsofShape *s, SiftAsofSpec *spec)
{
	const bool	backward = s->ts_cmp == SAC_LE || s->ts_cmp == SAC_LT;

	if (s->type != SSJ_LEFT && s->type != SSJ_INNER)
		return SSV_ASOF_JOIN_TYPE;
	if (!s->on_true)
		return SSV_ASOF_ON;
	if (!s->single_table)
		return SSV_ASOF_SUBQUERY;
	if (!s->limit_one)
		return SSV_ASOF_LIMIT;
	if (s->has_offset)
		return SSV_ASOF_OFFSET;
	if (s->nsort != 1 || !s->sort_on_ts)
		return SSV_ASOF_ORDER;
	if (s->nkey_eq == 0)
		return SSV_ASOF_NO_KEY;
	if (s->nkey_eq > 1)
		return SSV_ASOF_KEYS;
	if (s->nts_cmp != 1)
		return SSV_ASOF_TS_QUAL;
	if (s->nother > 0)
		return SSV_ASOF_OTHER_QUAL;
	if (!s->types_ok)
		return SSV_ASOF_TYPE;
	if (backward != s->sort_desc)
		return SSV_ASOF_ORDER;
	*spec = (SiftAsofSpec) {.dir = backward ? SIFT_ASOF_BACKWARD : SIFT_ASOF_FORWARD,.strict = s->ts_cmp == SAC_LT || s->ts_cmp == SAC_GT};
	return SSV_OK;
}

/* ---- date_bin ---- */

SiftShapeVerdict
sift_shape_datebin(const SiftDateBinShape *s, SiftDateBin *out)
{
	if (!s->stride_const || !s->origin_const || s->stride_null || s->origin_null)
		return SSV_DATEBIN_ARGS;
	if (!s->source_ok)
		return SSV_DATEBIN_TYPE;
	sift_datebin_init(out, &s->stride, s->origin);
	switch (out->stride_status)
	{
		case SDB_OK:
			return SSV_OK;
		case SDB_ORIGIN_RANGE:
			return SSV_DATEBIN_ORIGIN;
		case SDB_STRIDE_MONTHS:
			return SSV_DATEBIN_MONTHS;
		default:
			return SSV_DATEBIN_STRIDE;
	}
}

/* ---- windows ---- */

static bool
winx_arg_type(SiftVecType t)
{
	return t == SVEC_I32 || t == SVEC_I64 || t == SVEC_F64 || t == SVEC_NUM64 || t == SVEC_NUM128;
}

SiftShapeVerdict
sift_shape_winx(const SiftWinShape *s)
{
	bool		needed = false;

	if (s->nfuncs > SIFT_WINX_MAX_FUNCS)
		return SSV_WIN_TOO_MANY;
	if (!s->window_mode || s->topk || !s->frame_from_start)
		return SSV_WIN_FRAME;
	for (int i = 0; i < s->nfuncs; i++)
	{
		const SiftWinShapeFunc *f = &s->func[i];
		const bool	takes_arg = f->kind >= SWX_COUNT;

		if (!f->known)
			return SSV_WIN_FUNC;
		if (takes_arg && !winx_arg_type(f->arg_type))
			return SSV_WIN_TYPE;
		needed |= f->has_filter || sift_winx_is_var(f->kind) || (takes_arg && (f->arg_type == SVEC_NUM64 || f->arg_type == SVEC_NUM128));
	}
	return needed ? SSV_OK : SSV_WIN_NO_NEED;
}
