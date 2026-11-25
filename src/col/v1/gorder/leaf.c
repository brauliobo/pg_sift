/*
 * col/v1/gorder/leaf.c - the leaves that have an order array (col/v1/gorder/gorder.h). Portable (core/host.h only).
 */

#include "core/host.h"
#include "col/v1/gorder/gorder.h"
#include "col/v1/runs.h"

bool
sift_cgord_leaf_text(const SiftColDef *d)
{
	return d->type == SIFT_COLT_TEXT && d->store == SIFT_COLS_STR;
}

bool
sift_cgord_leaf_ok(const SiftColSchema *s, const SiftColDef *d)
{
	if (!sift_crun_leaf_ok(s, d))
		return false;
	switch (d->type)
	{
		case SIFT_COLT_INT2:
		case SIFT_COLT_INT4:
		case SIFT_COLT_INT8:
		case SIFT_COLT_DATE:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			return true;
		default:
			return false;
	}
}

/* a numeric(p, s) of up to 18 digits: its key is the number scaled by 10^s, an integer in the order of the numbers (a NaN or an infinity is an overflow row, which leaves the segment without an array) */
bool
sift_cgord_leaf_numeric(const SiftColSchema *s, const SiftColDef *d)
{
	return d->type == SIFT_COLT_NUMERIC && sift_crun_leaf_ok(s, d);
}

bool
sift_cgord_leaf_array_ok(const SiftColSchema *s, const SiftColDef *d)
{
	return sift_cgord_leaf_ok(s, d) || sift_cgord_leaf_numeric(s, d) ||
		(d->kind == SIFT_COLK_SCALAR && d->parent < 0 && (d->path == NULL || d->path[0] == '\0') && !(d->flags & SIFT_COLD_LOSSY) && sift_cgord_leaf_text(d));
}

bool
sift_cgord_leaf_time(const SiftColDef *d)
{
	return d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ;
}
