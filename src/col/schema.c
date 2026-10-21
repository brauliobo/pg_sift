/*
 * col/schema.c - L1 of the column engine: the schema, the leaf list of an index (docs/COL.md 2.1).
 */
#include "sift_host.h"

#include "col/enc.h"
#include "col/enc_int.h"

#define COLS_MAGIC		0x534C4F43	/* bytes 'C' 'O' 'L' 'S' */

/* ---------------------------------------------------------------------------
 * 1. Schema
 * ------------------------------------------------------------------------- */

typedef struct SiftColDefDisk
{
	int16		id;
	int16		parent;
	uint8		kind;
	uint8		store;
	uint16		flags;
	int16		attno;
	uint16		pathlen;
	uint8		type;
	uint8		scale;
	uint8		lanes;			/* 0 in every schema before 3.2 (these were the two reserved bytes) */
	uint8		lane;
} SiftColDefDisk;

/* NULL when the leaf list is valid, else why not */
static const char *
schema_check(const SiftColDef *defs, int n)
{
	if (n < 1 || n > SIFT_COL_MAX_COLS)
		return "the number of leaves is out of range";
	for (int i = 0; i < n; i++)
	{
		const SiftColDef *d = &defs[i];

		if (d->id != i)
			return "leaf ids must be 0..n-1 in order";
		if (d->kind != SIFT_COLK_SCALAR && d->kind != SIFT_COLK_GROUP)
			return "unknown leaf kind";
		if (d->store < SIFT_COLS_INT || d->store > SIFT_COLS_VAR)
			return "unknown storage class";
		if (d->type >= SIFT_COLT_END)
			return "unknown leaf type";
		if (d->store == SIFT_COLS_VAR && (d->kind != SIFT_COLK_SCALAR || d->parent != -1))
			return "a variable-length leaf is a top level scalar";
		if (d->path != NULL && strlen(d->path) > PG_UINT16_MAX)
			return "leaf path too long";
		if (d->scale > (d->lanes > 1 || d->lane > 0 ? SIFT_COL_MAX_WIDE_SCALE : SIFT_COL_MAX_SCALE) || (d->scale > 0 && d->store != SIFT_COLS_INT))
			return "a scale belongs to an integer leaf and is at most 18 (38 for a wide one)";
		if (d->lanes > SIFT_COL_MAX_LANES || d->lanes == 1 || (d->lanes > 1 && (d->lane != 0 || i + d->lanes > n)))
			return "a wide leaf has 2 or 3 lanes, all inside the leaf list";
		if (d->lanes > 1 || d->lane > 0)
		{
			if (d->kind != SIFT_COLK_SCALAR || d->parent != -1 || d->store == SIFT_COLS_VAR || (d->lane > 0 && d->store != SIFT_COLS_INT))
				return "a wide leaf is a top level scalar of storage class INT or STR, its other lanes INT";
			for (int k = 1; k < d->lanes; k++)
				if (defs[i + k].lane != k)
					return "the leaves after a wide head are its lanes";
			if (d->lane > 0 && (d->lane >= SIFT_COL_MAX_LANES || i < d->lane || defs[i - d->lane].lanes <= d->lane ||
								defs[i - d->lane].attno != d->attno || defs[i - d->lane].type != d->type || defs[i - d->lane].scale != d->scale))
				return "a lane must follow its head and agree with it";
		}
		if (d->kind == SIFT_COLK_GROUP && (d->parent != -1 || d->store != SIFT_COLS_INT))
			return "a group is a top level leaf of storage class INT";
		if (d->kind == SIFT_COLK_SCALAR && d->parent != -1 &&
			(d->parent < 0 || d->parent >= i || defs[d->parent].kind != SIFT_COLK_GROUP))
			return "the parent of a child must be an earlier group";
	}
	return NULL;
}

static void
schema_put_def(StringInfo out, const SiftColDef *d, bool with_flags)
{
	const char *path = d->path ? d->path : "";
	SiftColDefDisk disk = {d->id, d->parent, d->kind, d->store, with_flags ? d->flags : 0, d->attno, (uint16) strlen(path), d->type, d->scale, d->lanes, d->lane};

	put_val(out, disk);
	put_bytes(out, path, disk.pathlen);
}

static uint32
schema_hash(const SiftColDef *defs, int n)
{
	StringInfoData buf;
	uint32		h;

	initStringInfo(&buf);
	for (int i = 0; i < n; i++)
		schema_put_def(&buf, &defs[i], false);
	h = hash_bytes((const unsigned char *) buf.data, buf.len);
	pfree(buf.data);
	return h;
}

static SiftColSchema *
schema_build(MemoryContext mcxt, const SiftColDef *defs, int n)
{
	SiftColSchema *s = MemoryContextAllocZero(mcxt, sizeof(SiftColSchema));

	s->ncols = n;
	s->cols = MemoryContextAlloc(mcxt, n * sizeof(SiftColDef));
	for (int i = 0; i < n; i++)
	{
		const char *path = defs[i].path ? defs[i].path : "";
		size_t		len = strlen(path);
		char	   *copy = MemoryContextAlloc(mcxt, len + 1);

		memcpy(copy, path, len + 1);
		s->cols[i] = defs[i];
		s->cols[i].path = copy;
		s->ngroups += defs[i].kind == SIFT_COLK_GROUP;
		s->wide |= sift_col_def_needs_wide(&defs[i]);
		s->kinds |= sift_col_def_has_kinds(&defs[i]);
	}
	s->hash = schema_hash(defs, n);
	return s;
}

SiftColSchema *
sift_cschema_create(MemoryContext mcxt, const SiftColDef *defs, int n)
{
	const char *why = schema_check(defs, n);

	if (why)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid sift_col leaf list: %s", why)));
	return schema_build(mcxt, defs, n);
}

void
sift_cschema_serialize(const SiftColSchema *s, StringInfo out)
{
	uint32		head[3] = {COLS_MAGIC, (uint32) s->ncols, s->hash};

	put_val(out, head);
	for (int i = 0; i < s->ncols; i++)
		schema_put_def(out, &s->cols[i], true);
}

SiftColSchema *
sift_cschema_parse(MemoryContext mcxt, const void *data, Size len)
{
	const uint8 *p = data;
	uint32		head[3];
	SiftColDef *defs;
	SiftColSchema *s = NULL;
	Size		pos = sizeof(head);

	if (len < sizeof(head))
		return NULL;
	memcpy(head, p, sizeof(head));
	if (head[0] != COLS_MAGIC || head[1] < 1 || head[1] > SIFT_COL_MAX_COLS)
		return NULL;
	defs = palloc0(head[1] * sizeof(SiftColDef));
	for (uint32 i = 0; i < head[1]; i++)
	{
		SiftColDefDisk disk;
		char	   *path;

		if (len - pos < sizeof(disk))
			goto out;
		memcpy(&disk, p + pos, sizeof(disk));
		pos += sizeof(disk);
		if (len - pos < disk.pathlen)
			goto out;
		path = palloc(disk.pathlen + 1);
		memcpy(path, p + pos, disk.pathlen);
		path[disk.pathlen] = '\0';
		pos += disk.pathlen;
		defs[i] = (SiftColDef) {disk.id, disk.parent, disk.kind, disk.store, disk.flags, disk.attno, disk.type, path, disk.scale, disk.lanes, disk.lane};
	}
	if (schema_check(defs, (int) head[1]) == NULL && schema_hash(defs, (int) head[1]) == head[2])
		s = schema_build(mcxt, defs, (int) head[1]);
out:
	for (uint32 i = 0; i < head[1]; i++)
		if (defs[i].path)
			pfree((char *) defs[i].path);
	pfree(defs);
	return s;
}
