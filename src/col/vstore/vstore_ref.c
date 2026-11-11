/*
 * col/vstore/vstore_ref.c - the dictionary of a leaf of a segment that is a reference to the shared value store of its column (docs/COL-VSTORE.md)
 *
 * The segment keeps its own codes (chunks, order arrays, zones and points are as they always were); what the shared store replaces is the strings
 * of the dictionary. Form DICT_REF is four words, the form, n, the bits of a code of the store and the epoch of the store, then for each code of the
 * segment, in order, the code of its string in the store, packed to that width and padded to 4 bytes. A reader makes the dictionary of the segment
 * from the view of the store; a writer interns the dictionary it has made into the store.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "lib/stringinfo.h"
#include "storage/lockdefs.h"

#include "col/enc.h"
#include "col/enc_int.h"
#include "col/seg.h"
#include "col/vstore/vstore.h"

#define REF_HEAD	(DICT_REF_WORDS * sizeof(uint32))

void
sift_vstore_ref_serialize(Oid store, const SiftCDict *dict, const SiftCcPolicy *pol, StringInfo out)
{
	SiftVStore *vs = sift_vstore_open(store, RowExclusiveLock);
	uint32	   *codes = palloc(Max(dict->n, 1) * sizeof(uint32));
	uint32		head[DICT_REF_WORDS];
	uint64		epoch,
				ncodes;

	(void) sift_vstore_intern(vs, dict, pol, codes, &epoch, &ncodes);
	head[0] = DICT_REF;
	head[1] = dict->n;
	head[2] = sift_cdict_perm_bits(ncodes);
	head[3] = store;
	head[4] = (uint32) epoch;
	appendBinaryStringInfo(out, (const char *) head, sizeof(head));
	put_packed(out, codes, dict->n, head[2]);
	while (out->len % sizeof(uint32))
		appendStringInfoChar(out, '\0');
	sift_cdict_put_hash(dict, out);
	pfree(codes);
	sift_vstore_close(vs);
}

typedef struct CodePos
{
	uint32		code;
	uint32		pos;
} CodePos;

/* by_code ordered by code: four passes of 8 bits (tmp is as large as by_code) */
static void
sort_by_code(CodePos *by_code, CodePos *tmp, uint32 n)
{
	for (int shift = 0; shift < 32; shift += 8)
	{
		uint32		count[257] = {0};

		for (uint32 i = 0; i < n; i++)
			count[((by_code[i].code >> shift) & 0xff) + 1]++;
		for (int k = 0; k < 256; k++)
			count[k + 1] += count[k];
		for (uint32 i = 0; i < n; i++)
			tmp[count[(by_code[i].code >> shift) & 0xff]++] = by_code[i];
		memcpy(by_code, tmp, n * sizeof(CodePos));
	}
}

/*
 * The dictionary of a reference: the strings of the codes the segment holds, read from the runs of its epoch in place. The codes of a segment climb in pieces (its index
 * met the rows in the order of the store's first member, in other chunks) and are read as they come; when they jump between blocks more often than the blocks they
 * touch could need, they are read in the order of the store, so that a block is decompressed once, and put back
 */
static SiftCDict *
from_runs(MemoryContext mcxt, Oid store, uint32 epoch, const uint32 *codes, uint32 n)
{
	void	   *h = sift_vstore_read_open(NULL, store, epoch, CurrentMemoryContext);
	SiftCDict  *d = sift_cdict_plain_begin(mcxt, n);
	StringInfoData all;
	CodePos	   *by_code = palloc(Max(n, 1) * sizeof(CodePos));
	CodePos	   *tmp = palloc(Max(n, 1) * sizeof(CodePos));
	uint32	   *sorted = palloc(Max(n, 1) * sizeof(uint32));
	uint32	   *soff = palloc(((Size) n + 1) * sizeof(uint32));
	uint32		jumps = 0;

	initStringInfo(&all);
	for (uint32 i = 0; i < n; i++)
	{
		by_code[i] = (CodePos) {codes[i], i};
		jumps += i > 0 && codes[i] / DICT_BLOCK != codes[i - 1] / DICT_BLOCK;
	}
	if (jumps <= 4 * (n / DICT_BLOCK + 1))
		sift_vstore_read_strs(h, codes, n, &all, d->off);
	else
	{
		StringInfoData ordered;

		sort_by_code(by_code, tmp, n);
		for (uint32 k = 0; k < n; k++)
			sorted[k] = by_code[k].code;
		initStringInfo(&ordered);
		sift_vstore_read_strs(h, sorted, n, &ordered, soff);
		/* the strings of the segment's codes in the segment's order: where each lies in `ordered` is its rank */
		for (uint32 k = 0; k < n; k++)
			sorted[by_code[k].pos] = k;
		for (uint32 i = 0; i < n; i++)
		{
			d->off[i] = (uint32) all.len;
			appendBinaryStringInfo(&all, ordered.data + soff[sorted[i]], soff[sorted[i] + 1] - soff[sorted[i]]);
		}
		d->off[n] = (uint32) all.len;
		pfree(ordered.data);
	}
	pfree(by_code);
	pfree(tmp);
	pfree(sorted);
	pfree(soff);
	if (all.len > UINT32_MAX || !sift_cdict_plain_bytes(d, mcxt, all.len))
		return NULL;
	memcpy(d->bytes, all.data, all.len);
	pfree(all.data);
	return d;
}

SiftCDict *
sift_vstore_ref_parse(MemoryContext mcxt, Oid store, const char *data, uint64 len)
{
	const uint32 *head = (const uint32 *) data;
	uint32		n,
				bits;
	const uint8 *packed = (const uint8 *) data + REF_HEAD;
	uint32	   *codes;
	SiftCDict  *d;

	if (len < REF_HEAD || head[0] != DICT_REF)
		return NULL;
	n = head[1];
	bits = head[2];
	if (bits < 1 || bits > 32 || len < REF_HEAD + sift_cdict_perm_bytes(n, bits))
		return NULL;
	codes = palloc(Max(n, 1) * sizeof(uint32));
	for (uint32 i = 0; i < n; i++)
		codes[i] = sift_ckeyed_get(packed, sift_cdict_perm_bytes(n, bits), (uint64) i * bits, bits);
	d = from_runs(mcxt, store, head[4], codes, n);
	pfree(codes);
	return d;
}

static const SiftCRefHost ref_host = {sift_vstore_leaf_stores, sift_vstore_read_open, sift_vstore_read_str, sift_vstore_read_ncodes, sift_vstore_read_match, sift_vstore_ref_serialize, sift_vstore_ref_parse,
											 sift_vstore_current_epoch};

void
sift_vstore_ref_install(void)
{
	sift_cref_host = &ref_host;
}
