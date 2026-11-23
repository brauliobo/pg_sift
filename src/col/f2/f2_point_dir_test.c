/*
 * col/f2/f2_point_dir_test.c - test-only SQL function sift._test_s55_point_dir (SIFT_TESTING builds): the point directory built for generated columns (integers, uuids, texts, two integers that are
 * a key together) names, in the bucket of each key, every row that holds it and in ascending order, and is built only for the keys that qualify.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "lib/stringinfo.h"

#include "col/f2/f2test.h"
#include "col/f2/f2_point_dir.h"

PG_FUNCTION_INFO_V1(sift_test_s55_point_dir);
PG_FUNCTION_INFO_V1(sift_test_f2_point_keys);

static int
expect(bool ok)
{
	return !ok;
}

/* The hash of the key of a row of the generated columns */
static uint64
key_hash(const SiftPointCol *pc, const SiftColIn *cols, int row)
{
	uint64		h = 0;

	for (int i = 0; i < pc->nleaves; i++)
	{
		const SiftColIn *c = &cols[pc->leaves[i]];
		const uint64 x = c->kind == SIFT_CELL_INT ? sift_point_hash_i64(c->i[row]) : sift_point_hash_bytes(c->s[row], c->slen[row]);

		h = i == 0 ? x : sift_point_hash_combine(h, x);
	}
	return h;
}

static bool
key_null(const SiftPointCol *pc, const SiftColIn *cols, int row)
{
	for (int i = 0; i < pc->nleaves; i++)
		if (cols[pc->leaves[i]].isnull != NULL && cols[pc->leaves[i]].isnull[row])
			return true;
	return false;
}

/* The rows of each bucket the key of a row hashes to hold every row of that key, ascending; the directory of the key has as many entries as it has rows with all of its columns */
static int
check_column(const char *blob, const SiftPointCol *pc, const SiftColIn *cols, const int *gstart, int ng)
{
	const uint32 *start = (const uint32 *) (blob + pc->off_buckets),
			   *ref = (const uint32 *) (blob + pc->off_entries);
	int			failures = 0,
				nnz = 0;

	for (int g = 0; g < ng; g++)
		for (int r = 0; r < gstart[g + 1] - gstart[g]; r++)
		{
			const int	row = gstart[g] + r;
			const uint32 want = ((uint32) g << SIFT_POINT_ROW_BITS) | r;
			const uint32 b = sift_point_bucket(key_hash(pc, cols, row), pc->log2_buckets);
			bool		found = false;

			if (key_null(pc, cols, row))
				continue;
			nnz++;
			for (uint32 e = start[b]; e < start[b + 1] && !found; e++)
				found = ref[e] == want;
			failures += expect(found);
		}
	failures += expect((int) pc->nentries == nnz);
	failures += expect(start[(UINT64CONST(1) << pc->log2_buckets)] == pc->nentries);
	for (uint64 b = 0; b < (UINT64CONST(1) << pc->log2_buckets); b++)
		for (uint32 e = start[b] + 1; e < start[b + 1]; e++)
			failures += expect(ref[e - 1] < ref[e]);
	return failures;
}

/* (nrows, seed): the number of expectations that failed, 0 when the directory is right */
Datum
sift_test_s55_point_dir(PG_FUNCTION_ARGS)
{
	const int	n = PG_GETARG_INT32(0);
	uint64		rs = (uint64) PG_GETARG_INT32(1);
	enum {UNIQUE, WIDE, NARROW, TIMELIKE, TAGGED, NULLY, TEXTKEY, UUIDKEY, PAIR_A, PAIR_B, NCOLS};
	int64	   *vals[NCOLS];
	bool	   *nulls = palloc0(n),
			   *none = palloc0(n);
	uint8	   *tags = palloc0(n);
	char	  (*text)[16] = palloc(n * 16),
			  (*uuid)[16] = palloc(n * 16);
	const char **textp = palloc(n * sizeof(char *)),
			  **uuidp = palloc(n * sizeof(char *));
	uint32	   *textlen = palloc(n * sizeof(uint32)),
			   *uuidlen = palloc(n * sizeof(uint32));
	int		   *gstart = palloc((n / SIFT_GRANULE_ROWS + 3) * sizeof(int));
	SiftColIn	cols[NCOLS] = {{0}};
	uint16		sort_cols[1] = {UNIQUE};
	SiftRows	rows = {.nrows = n,.ncols = NCOLS,.cols = cols,.time_col = 1 + TIMELIKE,.sort_cols = sort_cols,.nsort = 1};
	StringInfoData out;
	int			ng = 0,
				failures = 0,
				composite = 0,
				pairs = 0,
				nkeys;
	bool		pair;
	const SiftPointHdr *hdr;

	if (n < SIFT_POINT_MIN_ROWS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("rows must be at least %d", SIFT_POINT_MIN_ROWS)));

	for (int c = 0; c < NCOLS; c++)
		vals[c] = palloc(n * sizeof(int64));
	for (int r = 0; r < n; r++)
	{
		rs = rs * UINT64CONST(6364136223846793005) + UINT64CONST(1442695040888963407);
		vals[UNIQUE][r] = r * 7919 + 13;
		vals[WIDE][r] = (int64) ((rs >> 20) % (uint64) (n / 2 + 1));
		vals[NARROW][r] = (int64) ((rs >> 20) % 9);
		vals[TIMELIKE][r] = r * 1000;
		vals[TAGGED][r] = r;
		vals[NULLY][r] = (int64) ((rs >> 24) % (uint64) (n / 2 + 1));
		vals[PAIR_A][r] = r % 211;
		vals[PAIR_B][r] = r / 211;
		nulls[r] = r % 3 == 0;
		tags[r] = r == n - 1 ? 1 : 0;
		snprintf(text[r], 16, "k%010d", (int) ((rs >> 20) % (uint64) n));
		memcpy(uuid[r], &rs, 8);
		memcpy(uuid[r] + 8, &r, 4);
		memset(uuid[r] + 12, 0x5A, 4);
		textp[r] = text[r];
		textlen[r] = 11;
		uuidp[r] = uuid[r];
		uuidlen[r] = 16;
	}
	for (int c = 0; c < NCOLS; c++)
		cols[c] = (SiftColIn) {.kind = SIFT_CELL_INT,.i = vals[c],.isnull = c == NULLY ? nulls : none,.tag = c == TAGGED ? tags : NULL};
	cols[TEXTKEY] = (SiftColIn) {.kind = SIFT_CELL_STR,.s = textp,.slen = textlen,.isnull = none};
	cols[UUIDKEY] = (SiftColIn) {.kind = SIFT_CELL_STR,.hint = SIFT_CHINT_UUID,.s = uuidp,.slen = uuidlen,.isnull = none};
	for (int from = 0; from < n; from += SIFT_GRANULE_ROWS)
		gstart[ng++] = from;
	gstart[ng] = n;
	initStringInfo(&out);
	failures += expect(sift_point_dir_build(&rows, gstart, ng, (uint64) n * 1000, &out));
	hdr = (const SiftPointHdr *) out.data;
	failures += expect(hdr->magic == SIFT_POINT_MAGIC && hdr->version == SIFT_POINT_VERSION);
	for (int k = 0; k < hdr->ncols; k++)
	{
		const SiftPointCol *pc = (const SiftPointCol *) (out.data + sizeof(SiftPointHdr)) + k;

		for (int i = 0; i < pc->nleaves; i++)
			failures += expect((int) pc->leaves[i] != TIMELIKE && (int) pc->leaves[i] != TAGGED && ((int) pc->leaves[i] != NARROW || pc->nleaves > 1) && (int) pc->leaves[i] != UNIQUE);
		for (int i = pc->nleaves; i < SIFT_POINT_MAX_COMP; i++)
			failures += expect(pc->leaves[i] == SIFT_POINT_NO_LEAF);
		failures += expect(pc->key_kind == (pc->nleaves > 1 ? SIFT_PK_COMPOSITE : pc->leaves[0] == UUIDKEY ? SIFT_PK_UUID16 : pc->leaves[0] == TEXTKEY ? SIFT_PK_TEXT : SIFT_PK_INT64));
		pair = pc->nleaves == 2 && pc->leaves[0] + pc->leaves[1] == PAIR_A + PAIR_B;
		failures += expect(((pc->flags & SIFT_PCOL_UNIQUE) != 0) == (pc->leaves[0] == UUIDKEY || pair));
		composite += pc->key_kind == SIFT_PK_COMPOSITE;
		pairs += pair;
		failures += check_column(out.data, pc, cols, gstart, ng);
	}
	failures += expect(pairs == 1 && composite >= 1 && hdr->ncols == 4 + composite);
	nkeys = hdr->ncols;
	rows.nsort = 0;
	resetStringInfo(&out);
	failures += expect(sift_point_dir_build(&rows, gstart, ng, (uint64) n * 1000, &out));
	failures += expect(((const SiftPointHdr *) out.data)->ncols == nkeys + 1);
	resetStringInfo(&out);
	failures += expect(sift_point_dir_build(&rows, gstart, ng, (uint64) n * 40, &out));
	failures += expect(((const SiftPointHdr *) out.data)->ncols == 2);
	resetStringInfo(&out);
	failures += expect(!sift_point_dir_build(&rows, gstart, ng, (uint64) n * 8, &out));
	rows.ncol_ids = 1;
	failures += expect(!sift_point_dir_build(&rows, gstart, ng, (uint64) n * 1000, &out));
	rows.ncol_ids = 0;
	rows.nrows = SIFT_POINT_MIN_ROWS - 1;
	failures += expect(!sift_point_dir_build(&rows, gstart, ng, (uint64) n * 1000, &out));
	PG_RETURN_INT32(failures);
}

static const char *
leaf_label(const SiftColSchema *schema, int leaf)
{
	const SiftColDef *d = &schema->cols[leaf];

	return d->path != NULL && d->path[0] != '\0' ? d->path : d->name;
}

/* (index): the keys of the point directory of each part of the index, "kind:columns:unique:entries" of each, the parts apart by " | " (a part with no directory: "-") */
Datum
sift_test_f2_point_keys(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	const SiftColSchema *schema = sift_col_rel(rel)->schema;
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	StringInfoData out;

	initStringInfo(&out);
	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int k = 0; k < np; k++)
		{
			SiftPart   *part = sift_part_open(rel, pd[k].header_blk, pd[k].part_gen, (uint32) mr.entries[i].part_id);
			const SiftPointDir *dir = sift_point_dir_open(rel, part, CurrentMemoryContext);

			appendStringInfoString(&out, out.len > 0 ? " | " : "");
			if (dir == NULL)
				appendStringInfoChar(&out, '-');
			for (int c = 0; dir != NULL && c < dir->hdr.ncols; c++)
			{
				const SiftPointCol *pc = &dir->cols[c];

				appendStringInfo(&out, "%s%d:", c > 0 ? " " : "", pc->key_kind);
				for (int l = 0; l < pc->nleaves; l++)
					appendStringInfo(&out, "%s%s", l > 0 ? "+" : "", leaf_label(schema, pc->leaves[l]));
				appendStringInfo(&out, ":%d:%u", (pc->flags & SIFT_PCOL_UNIQUE) != 0, pc->nentries);
			}
		}
	}
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

#endif							/* SIFT_TESTING */
