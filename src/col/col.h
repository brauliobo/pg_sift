/*
 * col/col.h - the column engine and the index access method sift_col: the interface the executor, the planner and the shredder build against.
 * Design, format and invariants: the design notes. This header is normative for the C interface.
 *
 * It is cut into pieces in col/api/, included below in this order:
 *   format.h   the format minors and the limits
 *   leaf.h     1. leaf columns and values       (what an index is made of; the shredder fills rows with them)
 *   meta.h     2. metadata                      (planner: which leaves/expressions an index covers, statistics)
 *   source.h   3. the column source             (executor: units, filters, selection vectors, reads, MVCC), the shred interface (consumed from col/shred.h, owned by col-json), SQL entry points and options
 */
#ifndef SIFT_COL_H
#define SIFT_COL_H

#include "core/varint.h"
#include "col/kern.h"
#include "col/wide.h"
#include "core/filter.h"

struct SiftCGOrdBuf;
struct SiftCGOrdWant;
struct SiftCGOrdCuts;

#include "col/api/format.h"
#include "col/api/leaf.h"
#include "col/api/meta.h"
#include "col/api/source.h"

#endif							/* SIFT_COL_H */
