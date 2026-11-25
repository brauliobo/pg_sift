/*
 * agg/par/group_key.h - the key of a group of agg/par/group.h in the three places it is held: a slot of the compact table (agg/group/cmp.h), a record of a partition stream
 * (core/par/radix.h) and a row for the consumer. Word j of a slot is key j when it is a word, or the number of its text in the strings of the table (CMP_NO_ID: NULL); a text key alone with no sum
 * is held by the table itself (strm). A record of a stream has, for each key in order, its word or the header of its text (the length and the hash) and its bytes padded to 8, then the count (of a
 * partial aggregate) and the sums; a record of words only is fixed in size, so that a chunk is read by stride.
 */
#ifndef SIFT_AGG_V6_PAR_GROUP_KEY_H
#define SIFT_AGG_V6_PAR_GROUP_KEY_H

#include "postgres.h"

#include "agg/par/group.h"
#include "agg/group/cmp.h"
#include "core/par/radix.h"

/* The layout of the slots of the tables of the spec in proto; false when a slot would pass 8 words */
extern bool pgk_layout(SiftCompact *proto, const SiftParGroupSpec *sp, uint64 rows_bound);

/* The group of a slot of the table, with the bytes of its texts in the table */
extern void pgk_slot_row(const SiftCompact *c, const SiftParGroupSpec *sp, const uint64 *slot, SiftParGroupRow *r);

/* The row (its count and sums) into the table, added to what the group has. h32[j]: the hash of text j, from its record (NULL: made here). false when the arena has no room */
extern bool pgk_add(SiftCompact *c, const SiftParGroupSpec *sp, const SiftParGroupRow *r, const uint32 *h32);

/* The bytes of a record of words only (a row, or a partial aggregate) and of any record */
extern uint32 pgk_word_rec_size(const SiftParGroupSpec *sp, bool partial);
extern uint32 pgk_rec_size(const SiftParGroupSpec *sp, bool partial, const SiftParGroupRow *r);

/* The bytes that the records of the rows of a batch take, and in *strbytes the bytes of their texts */
extern uint64 pgk_batch_bytes(const SiftParGroupSpec *sp, const SiftParGroupBatch *b, Size *strbytes);

/* The record of the row (a row, or a partial aggregate) into the partition of its key; false when the stream has no room */
extern bool pgk_put(const SiftCompact *c, const SiftParGroupSpec *sp, SiftRadixOut *o, bool partial, const SiftParGroupRow *r);

/* The same into partition part of the stream, whose records the consumer reads whole (the hashes of the texts in it are not set) */
extern bool pgk_put_to(const SiftParGroupSpec *sp, SiftRadixOut *o, uint32 part, bool partial, const SiftParGroupRow *r);

/* The record at b into r (a row has the count 1) and the hash of each of its texts into h32, and the bytes it takes. Only the keys of the spec are set in r */
extern uint32 pgk_parse(const SiftParGroupSpec *sp, bool partial, const uint8 *b, SiftParGroupRow *r, uint32 *h32);

/* The order of the keys: words as unsigned, texts as bytes with the NULL one first */
extern int	pgk_cmp(const SiftParGroupSpec *sp, const SiftParGroupRow *a, const SiftParGroupRow *b);

#endif							/* SIFT_AGG_V6_PAR_GROUP_KEY_H */
