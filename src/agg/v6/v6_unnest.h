/*
 * agg/v6/v6_unnest.h - LATERAL jsonb_array_elements, jsonb_to_recordset, jsonb_each and unnest over the arrays an index of format major 2 keeps as columns: the rows of a batch (the parents) own
 * the elements their array layout gives (agg/vec.h SiftArrayLayout: the length of each parent and the offset of its first element in the columns of the elements), and the pairs of (parent, element)
 * are made without a document being read or an array being built. A parent without an element that passes the quals of the element is kept as one pair with no element when the join is a left
 * one; the parents the columns cannot hold (the document was not an array, or had more elements than the index keeps: poison) are told to the caller, which reads their rows from the heap.
 *
 * The same layout answers the questions that are one answer a parent: its length (jsonb_array_length, cardinality), whether one of its elements satisfies a test (jsonb_path_exists, && with an
 * array, any(...)), and the value of the key of a map (the elements of a jsonb object whose keys are the strings of one column and whose values are those of another).
 *
 * Portable: only core/host.h and agg/vec.h.
 */
#ifndef SIFT_AGG_V6_UNNEST_H
#define SIFT_AGG_V6_UNNEST_H

#include "core/host.h"

#include "agg/vec.h"

#define SIFT_UNNEST_NONE	UINT32_MAX

typedef struct SiftUnnestIn
{
	const SiftArrayLayout *lay;
	const uint64 *parent_mask;	/* the parents that take part (n_parents bits) */
	const uint64 *elem_mask;	/* the elements that pass the quals of the element (n_elems bits); NULL: all of them */
	const uint64 *poison;		/* the parents whose column cannot hold their array (n_parents bits); NULL: none */
	const uint64 *doc_valid;	/* the parents whose document is not NULL; NULL: all. A NULL document has no elements */
	bool		left;			/* LEFT JOIN LATERAL ... ON true: a parent with no element is one pair with SIFT_UNNEST_NONE */
} SiftUnnestIn;

typedef struct SiftUnnestCursor
{
	uint32		parent;
	uint32		elem;			/* the next element to look at of the parent, once in_parent */
	bool		in_parent;
	bool		emitted;		/* the parent has given a pair already */
} SiftUnnestCursor;

/*
 * The pairs, at most cap of them, resumed from the cursor: parent_idx[k] and elem_idx[k] (an index of the columns of the elements, SIFT_UNNEST_NONE for the NULL of a left join). The parents of poison
 * are not given: their bits are set in fallback (n_parents bits, the caller clears them). Returns how many pairs.
 */
extern uint32 sift_unnest_expand(const SiftUnnestIn *in, SiftUnnestCursor *cur, uint32 *parent_idx, uint32 *elem_idx, uint32 cap, uint64 *fallback);
extern bool sift_unnest_done(const SiftUnnestIn *in, const SiftUnnestCursor *cur);

/* out[i] = src[idx[i]] for the pairs; NONE (or a NULL source row) is NULL. Cells are the int64 or the bits of a float8 */
extern void sift_unnest_gather64(uint64 *out, uint64 *valid_out, const uint64 *src, const uint8 *src_null, const uint32 *idx, uint32 n);
extern void sift_unnest_gather_str(SiftStrView *out, uint64 *valid_out, const SiftStrView *src, const uint8 *src_null, const uint32 *idx, uint32 n);

/* the length of the array of each parent: int32, NULL for a parent without a document */
extern void sift_unnest_lengths(const SiftArrayLayout *lay, const uint64 *doc_valid, int32 *out, uint64 *valid_out);

/*
 * true_out bit p = some element of parent p is in hits (n_elems bits); known_out bit p = the document is not NULL. The parents not in parent_mask have neither bit. A test of the elements of a
 * jsonpath or an array is one pass over the elements and one over the parents.
 */
extern void sift_unnest_any(const SiftArrayLayout *lay, const uint64 *hits, const uint64 *parent_mask, const uint64 *doc_valid, uint64 *true_out, uint64 *known_out);
/* hits bit e = the dictionary code of element e is in the set of dict_bits (one bit a code); a NULL element (elem_valid) is no hit */
extern void sift_unnest_hits_from_codes(const uint32 *codes, uint32 n_elems, const uint64 *elem_valid, const uint64 *dict_bits, uint32 ndict, uint64 *hits);

/*
 * The map of a jsonb object: the keys of the elements and the key to find; elem_of[p] = the element of parent p whose key is it (the last one if it is repeated, as the value of a key of an
 * object is), or SIFT_UNNEST_NONE. Only the parents of parent_mask are looked at.
 */
extern void sift_map_lookup(const SiftArrayLayout *lay, const SiftStrView *keys, const char *key, uint32 keylen, const uint64 *parent_mask, uint32 *elem_of);
/* the same over keys held as the codes of a dictionary: the code of the key (SIFT_UNNEST_NONE: the dictionary lacks it, so no parent has it) */
extern void sift_map_lookup_code(const SiftArrayLayout *lay, const uint32 *codes, uint32 code, const uint64 *parent_mask, uint32 *elem_of);

/* jsonb_each over leaves that name every key of the object: the pairs of (parent, leaf) of the leaves that are present, in the order of the leaves for each parent */
typedef struct SiftUnpivotCursor
{
	uint32		parent;
	int			leaf;
} SiftUnpivotCursor;

extern uint32 sift_unpivot_expand(const uint64 *const *leaf_present, int nleaves, const uint64 *parent_mask, uint32 n_parents, SiftUnpivotCursor *cur, uint32 *parent_idx, int *leaf_idx, uint32 cap);

#endif							/* SIFT_AGG_V6_UNNEST_H */
