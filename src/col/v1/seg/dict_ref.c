/*
 * col/v1/seg/dict_ref.c - the dictionaries of a segment that refer to the shared value store of a 5.x column (column format minor 46)
 */
#include "col/compress.h"
#include "col/keystore/keystore.h"
#include "col/v1/seg_int.h"

/*
 * The dictionary of form DICT_REF (column format minor 46 of 5.x): five words (the form, n, the bits of a code of the store, the store, its epoch), the codes of the store of the
 * n strings packed, then a hash of the strings. It is read as a plain dictionary of the strings in the order of the codes of the segment.
 */
SiftCDict *
seg_dict_ref_load(SiftColSegment *seg, int col, uint64 at, uint64 len, MemoryContext mcxt)
{
	uint32		head[DICT_REF_WORDS];
	uint64		nbytes;
	uint8	   *packed;
	SiftKeyStore *ks;
	SiftCDict  *d;
	StringInfoData all;

	seg_read(seg, at, sizeof(head), head);
	nbytes = sift_cdict_perm_bytes(head[1], head[2]);
	if (head[1] == 0 || head[2] < 1 || head[2] > 32 || head[3] == InvalidOid || sizeof(head) + nbytes > len)
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	packed = palloc0(nbytes + sizeof(uint64));
	seg_read(seg, at + sizeof(head), nbytes, packed);
	ks = sift_keystore_open_epoch(head[3], AccessShareLock, head[4]);
	d = sift_cdict_plain_begin(mcxt, head[1]);
	initStringInfo(&all);
	for (uint32 i = 0; i < head[1]; i++)
	{
		uint32		slen;
		const char *s = sift_keystore_string(ks, sift_ckeyed_get(packed, nbytes + sizeof(uint64), (uint64) i * head[2], head[2]), &slen);

		d->off[i] = (uint32) all.len;
		appendBinaryStringInfo(&all, s, slen);
	}
	d->off[head[1]] = (uint32) all.len;
	sift_keystore_close(ks);
	if (all.len > UINT32_MAX || !sift_cdict_plain_bytes(d, mcxt, all.len))
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	memcpy(d->bytes, all.data, all.len);
	if (!sift_cdict_plain_valid(d))
		corrupt(seg, "The dictionary of leaf %d is malformed.", col);
	pfree(all.data);
	pfree(packed);
	return d;
}
