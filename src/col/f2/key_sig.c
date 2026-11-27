/*
 * col/f2/key_sig.c - the sort key a part of a sift_table is in (col/f2/key_sig.h).
 */
#include "core/host.h"

#include "common/hashfn.h"

#include "col/f2/key_sig.h"
#include "col/f2/meta.h"
#include "core/page_io.h"
#include "table/f2/tbl.h"

typedef struct KeyCol
{
	int16		attnum;
	uint8		flags;
	uint8		pad;
} KeyCol;

static uint32
key_sig(const KeyCol *cols, int n)
{
	uint32		h = hash_bytes((const unsigned char *) cols, n * sizeof(KeyCol));

	return h != 0 ? h : 1;
}

uint32
sift_f2_spec_key_sig(const SiftPartSpec *spec)
{
	KeyCol		cols[SIFT_KEY_ORDER_MAX] = {0};

	if (spec->col_ids == NULL)
		return 0;
	for (int k = 0; k < spec->nsort; k++)
		cols[k] = (KeyCol) {.attnum = spec->col_ids[spec->sort_cols[k]].attnum,.flags = spec->sort_flags[k]};
	return key_sig(cols, spec->nsort);
}

uint32
sift_f2_part_key_sig(Relation rel, const SiftPart *part)
{
	uint64		len;
	char	   *schema = sift_part_read_section(rel, part, SEC_SCHEMA, &len);
	KeyCol		cols[SIFT_KEY_ORDER_MAX] = {0};
	const SiftPartColIdentity *ids;
	const char *sort;
	uint16		hdr[2];
	int			nids,
				nsort;

	if (schema == NULL)
		return 0;
	memcpy(hdr, schema, sizeof(hdr));
	ids = sift_tbl_f2_schema_identities(schema, len, &nids);
	if (ids == NULL)
	{
		pfree(schema);
		return 0;
	}
	nsort = hdr[1] & 0x00FF;
	sort = schema + ((4 + hdr[0] + 1) & ~(Size) 1);
	if (nsort > SIFT_KEY_ORDER_MAX)
		SIFT_CORRUPT(rel, "The schema section of a part sorts by %d columns.", nsort);
	for (int k = 0; k < nsort; k++)
	{
		uint16		c;

		memcpy(&c, sort + k * sizeof(uint16), sizeof(c));
		if (c >= nids)
			SIFT_CORRUPT(rel, "The schema section of a part sorts by a column it does not have.");
		cols[k] = (KeyCol) {.attnum = ids[c].attnum,.flags = (uint8) sort[nsort * sizeof(uint16) + k]};
	}
	pfree(schema);
	return key_sig(cols, nsort);
}

uint64
sift_f2_key_trusted_gen(Relation rel, uint32 sig)
{
	SiftColMetaPageData m;

	if (sig == 0)
		return 0;
	m = sift_col_meta_read(rel);
	if (m.active_key_order_hash == 0)
		return 0;
	return m.active_key_order_hash == sig ? m.key_order_change_gen : PG_UINT64_MAX;
}
