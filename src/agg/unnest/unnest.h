/*
 * agg/unnest/unnest.h - LATERAL jsonb_array_elements, jsonb_to_recordset, jsonb_each and unnest over the arrays an index of format major 2 keeps as columns: the rows of a batch (the parents) own
 * the elements their array layout gives (agg/vec.h SiftArrayLayout: the length of each parent and the offset of its first element in the columns of the elements), and the pairs of (parent, element)
 * are made without a document being read or an array being built. A parent without an element that passes the quals of the element is kept as one pair with no element when the join is a left
 * one; the parents the columns cannot hold (the document was not an array, or had more elements than the index keeps: poison) are told to the caller, which reads their rows from the heap.
 *
 * The same layout answers the questions that are one answer a parent: its length (jsonb_array_length, cardinality), whether one of its elements satisfies a test (jsonb_path_exists, && with an
 * array, any(...)), and the value of the key of a map (the elements of a jsonb object whose keys are the strings of one column and whose values are those of another).
 *
 * Three questions need no pair at all, and the consumer asks them instead of expanding when the query allows (each reads the layout and, for the last two, a bitmap of the elements):
 *   - the weight of a parent, the number of pairs it gives (sift_unnest_weights): COUNT(*) and the aggregates of parent columns fold each parent once, times its weight, and no element column is read;
 *   - the elements that count (sift_unnest_elem_mask): when no parent column is read, the pairs are the elements of the live parents that pass the test of the element, counted or grouped by their
 *     dictionary codes (sift_unnest_count_codes) with nothing gathered;
 *   - whether a parent has an element that passes (sift_unnest_semi): EXISTS over the unnest, and COUNT(DISTINCT parent key) when the key of the parent is one a row.
 *
 * Portable: only core/host.h, core/popcount.h and agg/vec.h.
 */
#ifndef SIFT_AGG_V6_UNNEST_H
#define SIFT_AGG_V6_UNNEST_H

#include "core/host.h"
#include "core/popcount.h"

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

/*
 * weight[p] = the pairs parent p gives: its elements that pass elem_mask (all of them: NULL), none for a parent out of parent_mask or without a document, and one (the NULL pair) when the join
 * is a left one and there is no element. The parents of poison are not weighed (weight 0): their bits are set in fallback (n_parents bits, the caller clears them). Returns the sum of the weights.
 */
extern uint64 sift_unnest_weights(const SiftUnnestIn *in, uint32 *weight, uint64 *fallback);

/*
 * out bit e = element e belongs to a parent of parent_mask that is not poison and has a document, and passes elem_mask, for e in [*lo, *hi) = [offsets[0], offsets[n_parents]): the elements of the
 * parents of the layout; the bits outside the range are not touched and the words of out that hold the range are written whole. Returns how many bits are set. A left join is not asked: its
 * pairs without an element are the weights'.
 */
extern uint32 sift_unnest_elem_mask(const SiftUnnestIn *in, uint64 *out, uint32 *lo, uint32 *hi);
/* The bits of bits set in [lo, hi); NULL: all of them */
extern uint32 sift_unnest_count_bits(const uint64 *bits, uint32 lo, uint32 hi);

/*
 * The elements of sel in [lo, hi) counted by the dictionary codes of one or two columns, in direct cells and no hash table: cells[slot0 * stride + slot1] += 1, where slot is code + 1 of a column and
 * 0 for a NULL element (valid bit clear; valid NULL: all valid), stride is ncodes1 + 1 and slot1 is 0 when code1 is NULL (one column: stride 1). cells holds (ncodes0 + 1) * stride counters; the
 * codes are those of the dictionaries the consumer sized the cells for.
 */
extern void sift_unnest_count_codes(const uint32 *code0, const uint64 *valid0, const uint32 *code1, const uint64 *valid1, uint32 ncodes1, const uint64 *sel, uint32 lo, uint32 hi, uint64 *cells);

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
/*
 * hit_out bit p = parent p of parent_mask has a document and an element that passes elem_mask (EXISTS (SELECT 1 FROM unnest(parent) WHERE ...); a NULL document or an empty array has none, and the
 * answer is false, not NULL). The parents of poison are not answered: their bits are set in fallback (the caller clears them and reads their rows from the heap). left is not looked at.
 * Returns the number of hits: the COUNT(DISTINCT parent key) of a key no two parents share.
 */
extern uint32 sift_unnest_semi(const SiftUnnestIn *in, uint64 *hit_out, uint64 *fallback);
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
