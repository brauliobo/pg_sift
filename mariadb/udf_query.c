/*
 * udf_query.c - the query UDFs (udf.c): sift_topk, sift_count, sift_matches and their filter argument, sift_idset.
 */
#include "udf_int.h"

/* ------------------------------------------------ the filter argument: sift_idset */

#define IDSET_MAGIC		"SIFTIDS1"

typedef struct IdSet
{
	int64_t    *ids;
	size_t		n,
				cap;
	char	   *out;			/* the result, kept until the next one */
} IdSet;

static int
cmp_i64(const void *a, const void *b)
{
	int64_t		x = *(const int64_t *) a,
				y = *(const int64_t *) b;

	return (x > y) - (x < y);
}

static size_t
sort_unique(int64_t *ids, size_t n)
{
	size_t		w = 0;

	qsort(ids, n, sizeof(int64_t), cmp_i64);
	for (size_t i = 0; i < n; i++)
		if (w == 0 || ids[w - 1] != ids[i])
			ids[w++] = ids[i];
	return w;
}

EXPORT sx_bool
sift_idset_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 0;
	initid->max_length = 256 * 1024 * 1024;
	if (check_args(args, message, "sift_idset(key)", 1, "i"))
		return 1;
	initid->ptr = calloc(1, sizeof(IdSet));
	return 0;
}

EXPORT void
sift_idset_deinit(UDF_INIT *initid)
{
	IdSet	   *s = (IdSet *) initid->ptr;

	if (s)
	{
		free(s->ids);
		free(s->out);
	}
	free(s);
}

EXPORT void
sift_idset_clear(UDF_INIT *initid, char *is_null, char *error)
{
	((IdSet *) initid->ptr)->n = 0;
}

EXPORT void
sift_idset_add(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	IdSet	   *s = (IdSet *) initid->ptr;

	if (args->args[0] == NULL)
		return;
	if (s->n == s->cap)
		s->ids = realloc(s->ids, (s->cap = s->cap ? s->cap * 2 : 1024) * sizeof(int64_t));
	s->ids[s->n++] = *(long long *) args->args[0];
}

/* The set as a string: the magic, the count and the ascending keys, 8 bytes each */
EXPORT char *
sift_idset(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	IdSet	   *s = (IdSet *) initid->ptr;
	char	   *out;
	int64_t		n;

	s->n = sort_unique(s->ids, s->n);
	n = s->n;
	free(s->out);
	out = s->out = malloc(16 + n * sizeof(int64_t));
	memcpy(out, IDSET_MAGIC, 8);
	memcpy(out + 8, &n, 8);
	memcpy(out + 16, s->ids, n * sizeof(int64_t));
	*length = 16 + n * sizeof(int64_t);
	return out;
}

/* The filter argument: an idset, or a JSON array of keys; the keys are in *ids (malloc'd) */
static sx_bool
parse_filter(UDF_ARGS *args, int i, SxIdSet *flt, int64_t **ids, char *error)
{
	const char *p;
	size_t		len;

	*ids = NULL;
	if ((unsigned) i >= args->arg_count || args->args[i] == NULL)
		return 0;
	p = args->args[i];
	len = args->lengths[i];
	if (len >= 16 && memcmp(p, IDSET_MAGIC, 8) == 0)
	{
		int64_t		n;

		memcpy(&n, p + 8, 8);
		if (n < 0 || len != 16 + (size_t) n * 8)
		{
			sx_raise("sift: the filter is not a sift_idset", error);
			return 1;
		}
		*ids = malloc(n * 8 + 8);
		memcpy(*ids, p + 16, n * 8);
		*flt = (SxIdSet) {*ids, n};
		return 0;
	}
	{
		size_t		n = 0,
					cap = 64;
		char	   *end;
		char	   *copy = strndup(p, len);
		const char *q = copy;

		*ids = malloc(cap * 8);
		while (*q && *q != '[')
			q++;
		if (*q != '[')
		{
			free(copy);
			free(*ids);
			*ids = NULL;
			sx_raise("sift: the filter is a sift_idset or a JSON array of keys", error);
			return 1;
		}
		for (q++; *q && *q != ']';)
		{
			long long	v = strtoll(q, &end, 10);

			if (end == q)
			{
				q++;
				continue;
			}
			if (n == cap)
				*ids = realloc(*ids, (cap *= 2) * 8);
			(*ids)[n++] = v;
			q = end;
		}
		free(copy);
		n = sort_unique(*ids, n);
		*flt = (SxIdSet) {*ids, n};
	}
	return 0;
}

/* ------------------------------------------------ sift_topk */

EXPORT sx_bool
sift_topk_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 16 * 1024 * 1024;
	initid->ptr = NULL;
	return check_args(args, message, "sift_topk(index, query, k [, filter])", 3, "ssis");
}

EXPORT char *
sift_topk(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix;
	int			k = (int) *(long long *) args->args[2],
				n = 0,
				rc;
	SxHit	   *hits;
	char		err[512],
			   *out;
	size_t		w = 0;
	SxIdSet		flt;
	int64_t    *ids;

	if (args->args[1] == NULL || k < 1 || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	if (parse_filter(args, 3, &flt, &ids, error))
		return NULL;
	hits = malloc(sizeof(SxHit) * k);
	rc = sx_topk(ix, &host, args->args[1], args->lengths[1], k, ids ? &flt : NULL, hits, &n, err, sizeof(err));
	local_close();
	if (rc != 0)
	{
		free(hits);
		free(ids);
		sx_raise(err, error);
		return NULL;
	}
	out = result_buf(initid, (size_t) n * 48 + 4);
	out[w++] = '[';
	for (int i = 0; i < n; i++)
		w += sprintf(out + w, "%s{\"id\":%lld,\"s\":%.9g}", i ? "," : "", (long long) hits[i].pk, hits[i].score);
	out[w++] = ']';
	free(hits);
	free(ids);
	*length = w;
	return out;
}

EXPORT void
sift_topk_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}

/* ------------------------------------------------ sift_count */

EXPORT sx_bool
sift_count_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	return check_args(args, message, "sift_count(index, query [, filter])", 2, "sss");
}

EXPORT long long
sift_count(UDF_INIT *initid, UDF_ARGS *args, char *is_null, char *error)
{
	SiftIdx    *ix;
	int64_t		count;
	char		err[512];
	SxIdSet		flt;
	int64_t    *ids;
	int			rc;

	if (args->args[1] == NULL || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return 0;
	}
	if (parse_filter(args, 2, &flt, &ids, error))
		return 0;
	rc = sx_count(ix, &host, args->args[1], args->lengths[1], ids ? &flt : NULL, &count, err, sizeof(err));
	local_close();
	free(ids);
	if (rc != 0)
	{
		sx_raise(err, error);
		return 0;
	}
	return count;
}

/* ------------------------------------------------ sift_matches */

EXPORT sx_bool
sift_matches_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
	initid->maybe_null = 1;
	initid->max_length = 64 * 1024 * 1024;
	initid->ptr = NULL;
	return check_args(args, message, "sift_matches(index, query [, filter])", 2, "sss");
}

EXPORT char *
sift_matches(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
	SiftIdx    *ix;
	int64_t    *pks,
				n;
	char		err[512],
			   *out;
	size_t		w = 0;
	SxIdSet		flt;
	int64_t    *ids;
	int			rc;

	if (args->args[1] == NULL || (ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	if (parse_filter(args, 2, &flt, &ids, error))
		return NULL;
	rc = sx_matches(ix, &host, args->args[1], args->lengths[1], ids ? &flt : NULL, &pks, &n, err, sizeof(err));
	local_close();
	free(ids);
	if (rc != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	out = result_buf(initid, (size_t) n * 21 + 4);
	out[w++] = '[';
	for (int64_t i = 0; i < n; i++)
		w += sprintf(out + w, "%s%lld", i ? "," : "", (long long) pks[i]);
	out[w++] = ']';
	free(pks);
	*length = w;
	return out;
}

EXPORT void
sift_matches_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}
