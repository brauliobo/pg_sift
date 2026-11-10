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
#include "col/vstore/vstore.h"

#define REF_HEAD	(4 * sizeof(uint32))

void
sift_vstore_ref_serialize(Oid store, const SiftCDict *dict, const SiftCcPolicy *pol, StringInfo out)
{
	SiftVStore *vs = sift_vstore_open(store, RowExclusiveLock);
	uint32	   *codes = palloc(Max(dict->n, 1) * sizeof(uint32));
	uint32		head[4];

	(void) sift_vstore_intern(vs, dict, pol, codes);
	head[0] = DICT_REF;
	head[1] = dict->n;
	head[2] = sift_cdict_perm_bits(sift_vstore_view(vs)->n);
	head[3] = (uint32) sift_vstore_epoch(vs);
	appendBinaryStringInfo(out, (const char *) head, sizeof(head));
	put_packed(out, codes, dict->n, head[2]);
	while (out->len % sizeof(uint32))
		appendStringInfoChar(out, '\0');
	pfree(codes);
	sift_vstore_close(vs);
}

SiftCDict *
sift_vstore_ref_parse(MemoryContext mcxt, Oid store, const char *data, uint64 len)
{
	const uint32 *head = (const uint32 *) data;
	uint32		n,
				bits;
	const uint8 *packed = (const uint8 *) data + REF_HEAD;
	uint32	   *codes;
	uint64		need = 0,
				nbytes = 0;
	SiftVStore *vs;
	const SiftCDict *view;
	SiftCDict  *d;

	if (len < REF_HEAD || head[0] != DICT_REF)
		return NULL;
	n = head[1];
	bits = head[2];
	if (bits < 1 || bits > 32 || len < REF_HEAD + sift_cdict_perm_bytes(n, bits))
		return NULL;
	codes = palloc(Max(n, 1) * sizeof(uint32));
	for (uint32 i = 0; i < n; i++)
	{
		codes[i] = sift_ckeyed_get(packed, sift_cdict_perm_bytes(n, bits), (uint64) i * bits, bits);
		need = Max(need, (uint64) codes[i] + 1);
	}
	vs = sift_vstore_open(store, AccessShareLock);
	view = sift_vstore_view_to(vs, need, head[3]);
	d = view == NULL ? NULL : sift_cdict_plain_begin(mcxt, n);
	for (uint32 i = 0; d != NULL && i < n; i++)
	{
		uint32		slen;

		(void) sift_cdict_str(view, codes[i], &slen);
		d->off[i] = (uint32) nbytes;
		nbytes += slen;
	}
	if (d != NULL)
	{
		if (nbytes > UINT32_MAX)
			d = NULL;
		else
		{
			d->off[n] = (uint32) nbytes;
			(void) sift_cdict_plain_bytes(d, mcxt, nbytes);
			for (uint32 i = 0; i < n; i++)
			{
				uint32		slen;
				const char *s = sift_cdict_str(view, codes[i], &slen);

				memcpy(d->bytes + d->off[i], s, slen);
			}
		}
	}
	sift_vstore_close(vs);
	pfree(codes);
	return d;
}

static const SiftCRefHost ref_host = {sift_vstore_leaf_stores, sift_vstore_leaf_store, sift_vstore_ref_serialize, sift_vstore_ref_parse};

void
sift_vstore_ref_install(void)
{
	sift_cref_host = &ref_host;
}
