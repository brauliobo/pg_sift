/*
 * col/f2/key_sig.h - the sort key a part of a sift_table is in, as a signature: the attribute and the order of each key column, hashed (0: none, a part of a sift_col index names no attribute).
 *
 * A sift_table changes its key while it has parts (key_order, a primary key added or dropped, a restore that loads the rows before both): the parts written before keep the order and the bounds
 * of the key they were written in. The metapage says which parts are in which key: every part of generation key_order_change_gen or above is in the key of active_key_order_hash (a commit that
 * adds parts of another key makes it that key's and moves the generation to the next one; 0 and 0: no commit has said, every part is in the key of the table). A scan prunes by the bounds,
 * the zones and the marks of the key only the parts that are in the key it reads by; the others it reads whole.
 */
#ifndef SIFT_F2_KEY_SIG_H
#define SIFT_F2_KEY_SIG_H

#include "col/f2/part/format.h"
#include "col/f2/rowbuf/rowbuf.h"

/* The key the spec writes its parts in; 0 for the spec of a sift_col index */
extern uint32 sift_f2_spec_key_sig(const SiftPartSpec *spec);

/* The key the part was written in, from its SEC_SCHEMA; 0 for a part that names no attribute */
extern uint32 sift_f2_part_key_sig(Relation rel, const SiftPart *part);

/* The least generation of a part that is in the key sig: 0 when every part is, PG_UINT64_MAX when none is; 0 for sig 0 */
extern uint64 sift_f2_key_trusted_gen(Relation rel, uint32 sig);

#endif
