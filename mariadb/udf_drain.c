/*
 * udf_drain.c - the aggregates that apply the rows of a drain or a rebuild to an index (udf.c): sift_drain and sift_load.
 */
#include "udf_int.h"

/* ------------------------------------------------ sift_drain, sift_load: aggregates over the rows of a drain or a rebuild */

typedef struct Drain
{
	SiftIdx    *ix;
	SiftFeed   *feed;
	char		load;
	char		err[512];
} Drain;

static sx_bool
drain_init(UDF_INIT *initid, UDF_ARGS *args, char *message, int load)
{
	Drain	   *d;

	if (load ? check_args(args, message, "sift_load(index, watermark, key, text)", 4, "siis")
		: check_args(args, message, "sift_drain(index, seq, op, key, text)", 5, "sisis"))
		return 1;
	initid->ptr = (char *) (d = calloc(1, sizeof(Drain)));
	d->load = load;
	return 0;
}

EXPORT sx_bool
sift_drain_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return drain_init(initid, args, message, 0);
}

EXPORT sx_bool
sift_load_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	return drain_init(initid, args, message, 1);
}

static void
drain_deinit(UDF_INIT *initid)
{
	Drain	   *d = (Drain *) initid->ptr;

	if (d && d->feed)
		sx_feed_abort(d->feed);
	free(d);
}

EXPORT void
sift_drain_deinit(UDF_INIT *initid)
{
	drain_deinit(initid);
}

EXPORT void
sift_load_deinit(UDF_INIT *initid)
{
	drain_deinit(initid);
}

static void
drain_clear(UDF_INIT *initid, char *is_null, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	char		load = d->load;

	if (d->feed)
		sx_feed_abort(d->feed);
	memset(d, 0, sizeof(Drain));
	d->load = load;
}

EXPORT void
sift_drain_clear(UDF_INIT *initid, char *is_null, char *error)
{
	drain_clear(initid, is_null, error);
}

EXPORT void
sift_load_clear(UDF_INIT *initid, char *is_null, char *error)
{
	drain_clear(initid, is_null, error);
}

/* The row (seq, op, key, text) of a drain or (watermark, key, text) of a load */
static void
drain_add(UDF_INIT *initid, UDF_ARGS *args, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	int			t = d->load ? 3 : 4;
	const char *op = d->load ? "R" : args->args[2];
	long long	seq = d->load ? 0 : *(long long *) args->args[1],
				key = *(long long *) args->args[d->load ? 2 : 3];

	if (d->err[0])
		return;
	if (d->ix == NULL && (d->ix = open_index(args, error)) == NULL)
		return;
	if (d->feed == NULL &&
		(d->feed = sx_feed_begin(d->ix, d->load ? *(long long *) args->args[1] : -1, d->err, sizeof(d->err))) == NULL)
		return;
	if (sx_feed_add(d->feed, seq, op ? op[0] : 'D', key, args->args[t], args->lengths[t], d->err, sizeof(d->err)) != 0)
	{
		sx_feed_abort(d->feed);
		d->feed = NULL;
	}
}

EXPORT void
sift_drain_add(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	drain_add(initid, args, error);
}

EXPORT void
sift_load_add(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	drain_add(initid, args, error);
}

/* A load of an empty table has no row to open the index with: it still replaces the contents of the index */
static void
load_empty(Drain *d, UDF_ARGS *args, char *error)
{
	if (d->feed == NULL && d->err[0] == 0 && (d->ix = open_index(args, error)))
		d->feed = sx_feed_begin(d->ix, *(long long *) args->args[1], d->err, sizeof(d->err));
}

static long long
drain_end(UDF_INIT *initid, UDF_ARGS *args, char *error)
{
	Drain	   *d = (Drain *) initid->ptr;
	int64_t		rows = 0;

	if (d->load && d->ix == NULL)
		load_empty(d, args, error);
	if (d->feed)
	{
		SiftFeed   *f = d->feed;

		d->feed = NULL;
		if (sx_feed_end(f, &rows, d->err, sizeof(d->err)) != 0)
			rows = 0;
	}
	if (d->err[0])
	{
		sx_raise(d->err, error);
		return 0;
	}
	return rows;
}

EXPORT long long
sift_drain(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	return drain_end(initid, args, error);
}

EXPORT long long
sift_load(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	return drain_end(initid, args, error);
}
