/*
 * table/f2/col_delta/xlog.h - the append in place to the newest page of a column delta chain, logged with a record of the sift resource manager (core/f2/vm_xlog.h) that carries only the records
 * appended: its redo appends them to the page and widens the directory entry of the page as the writer did. A server that did not preload the library has no resource manager: the append is then
 * logged as a change of generic WAL.
 */
#ifndef SIFT_TABLE_F2_COL_DELTA_XLOG_H
#define SIFT_TABLE_F2_COL_DELTA_XLOG_H

#include "postgres.h"

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "utils/rel.h"

#include "table/f2/col_delta/col_delta.h"

/* The batch appended to the page of buf (exclusively locked) and its directory entry widened on the page of dbuf (InvalidBuffer: a flat chain), in one record; the caller checked it fits */
extern void sift_col_delta_xlog_append(Relation rel, Buffer buf, Buffer dbuf, const SiftColDeltaBatch *b);

extern void sift_col_delta_append_redo(XLogReaderState *r);
extern void sift_col_delta_append_desc(StringInfo buf, XLogReaderState *r);

#endif							/* SIFT_TABLE_F2_COL_DELTA_XLOG_H */
