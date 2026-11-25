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
	if ((initid->ptr = calloc(1, sizeof(IdSet))) == NULL)
	{
		snprintf(message, MYSQL_ERRMSG_SIZE, "sift: out of memory");
		return 1;
	}
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
	{
		size_t		cap = s->cap ? s->cap * 2 : 1024;
		int64_t    *ids = realloc(s->ids, cap * sizeof(int64_t));

		if (ids == NULL)
		{
			sx_raise("sift: out of memory", error);
			return;
		}
		s->ids = ids;
		s->cap = cap;
	}
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
	if (out == NULL)
	{
		sx_raise("sift: out of memory", error);
		return NULL;
	}
	memcpy(out, IDSET_MAGIC, 8);
	memcpy(out + 8, &n, 8);
	memcpy(out + 16, s->ids, n * sizeof(int64_t));
	*length = 16 + n * sizeof(int64_t);
	return out;
}

/* A filter that is a sift_idset: the magic, the count and the keys; the count has to be what the length holds */
static sx_bool
parse_idset(const char *p, size_t len, SxIdSet *flt, int64_t **ids, char *error)
{
	int64_t		n;

	memcpy(&n, p + 8, 8);
	if (n < 0 || (uint64_t) n > (len - 16) / 8 || len != 16 + (size_t) n * 8)
	{
		sx_raise("sift: the filter is not a sift_idset", error);
		return 1;
	}
	if ((*ids = malloc(n * 8 + 8)) == NULL)
	{
		sx_raise("sift: out of memory", error);
		return 1;
	}
	memcpy(*ids, p + 16, n * 8);
	*flt = (SxIdSet) {*ids, n};
	return 0;
}

/* The integers of the first [...] of the text: the keys of a filter written as a JSON array */
static sx_bool
parse_json_keys(const char *p, size_t len, SxIdSet *flt, int64_t **ids, char *error)
{
	AUTO_FREE char *copy = strndup(p, len);
	const char *q = copy;
	size_t		n = 0,
				cap = 64;

	if (copy == NULL || (*ids = malloc(cap * 8)) == NULL)
	{
		sx_raise("sift: out of memory", error);
		return 1;
	}
	while (*q && *q != '[')
		q++;
	if (*q != '[')
	{
		sx_raise("sift: the filter is a sift_idset or a JSON array of keys", error);
		return 1;
	}
	for (q++; *q && *q != ']';)
	{
		char	   *end;
		long long	v = strtoll(q, &end, 10);

		if (end == q)
		{
			q++;
			continue;
		}
		if (n == cap)
		{
			int64_t    *grown = realloc(*ids, (cap *= 2) * 8);

			if (grown == NULL)
			{
				sx_raise("sift: out of memory", error);
				return 1;
			}
			*ids = grown;
		}
		(*ids)[n++] = v;
		q = end;
	}
	n = sort_unique(*ids, n);
	*flt = (SxIdSet) {*ids, n};
	return 0;
}

/* The filter argument: an idset, or a JSON array of keys; the keys are in *ids (malloc'd, also when the filter is refused: free it) */
static sx_bool
parse_filter(UDF_ARGS *args, int i, SxIdSet *flt, int64_t **ids, char *error)
{
	*ids = NULL;
	if ((unsigned) i >= args->arg_count || args->args[i] == NULL)
		return 0;
	if (args->lengths[i] >= 16 && memcmp(args->args[i], IDSET_MAGIC, 8) == 0)
		return parse_idset(args->args[i], args->lengths[i], flt, ids, error);
	return parse_json_keys(args->args[i], args->lengths[i], flt, ids, error);
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
	SCOPED_IX	ix = NULL;
	AUTO_FREE SxHit *hits = NULL;
	AUTO_FREE int64_t *ids = NULL;
	long long	k = args->args[2] ? *(long long *) args->args[2] : 0;
	int			n = 0,
				rc;
	char		err[512],
			   *out;
	size_t		w = 0;
	SxIdSet		flt;

	if (args->args[1] == NULL || k < 1)
	{
		*is_null = 1;
		return NULL;
	}
	if (k > SX_MAX_K)
	{
		sx_raise("sift: sift_topk takes k from 1 to 1000000", error);
		return NULL;
	}
	if ((ix = open_index(args, error)) == NULL)
	{
		*is_null = 1;
		return NULL;
	}
	if (parse_filter(args, 3, &flt, &ids, error))
		return NULL;
	if ((hits = malloc(sizeof(SxHit) * k)) == NULL)
	{
		sx_raise("sift: out of memory", error);
		return NULL;
	}
	rc = sx_topk(ix, &host, args->args[1], args->lengths[1], k, ids ? &flt : NULL, hits, &n, err, sizeof(err));
	local_close();
	if (rc != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	if ((out = result_buf(initid, (size_t) n * 48 + 4, error)) == NULL)
		return NULL;
	out[w++] = '[';
	for (int i = 0; i < n; i++)
		w += sprintf(out + w, "%s{\"id\":%lld,\"s\":%.9g}", i ? "," : "", (long long) hits[i].pk, hits[i].score);
	out[w++] = ']';
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
	SCOPED_IX	ix = NULL;
	AUTO_FREE int64_t *ids = NULL;
	int64_t		count;
	char		err[512];
	SxIdSet		flt;
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
	SCOPED_IX	ix = NULL;
	AUTO_FREE int64_t *ids = NULL;
	AUTO_FREE int64_t *pks = NULL;
	int64_t		n;
	char		err[512],
			   *out;
	size_t		w = 0;
	SxIdSet		flt;
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
	if (rc != 0)
	{
		sx_raise(err, error);
		return NULL;
	}
	if ((out = result_buf(initid, (size_t) n * 21 + 4, error)) == NULL)
		return NULL;
	out[w++] = '[';
	for (int64_t i = 0; i < n; i++)
		w += sprintf(out + w, "%s%lld", i ? "," : "", (long long) pks[i]);
	out[w++] = ']';
	*length = w;
	return out;
}

EXPORT void
sift_matches_deinit(UDF_INIT *initid)
{
	common_deinit(initid);
}
