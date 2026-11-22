/*
 * agg/v6/v6_datebin_win_pg.h - what the date_bin kernel and the exact numeric average hand back to PostgreSQL: the error timestamp_bin raises for a status, and the Numeric of an average.
 */
#ifndef SIFT_AGG_V6_DATEBIN_WIN_PG_H
#define SIFT_AGG_V6_DATEBIN_WIN_PG_H

#include "postgres.h"

#include "agg/v6/v6_datebin_win.h"

/* The ERROR (message and SQLSTATE) of timestamp_bin for the status; returns for SDB_OK */
extern void sift_datebin_raise(SiftDateBinStatus s);
/* The numeric of the average (numeric_in of its text: the display scale is the scale of the value); not ok: the caller runs numeric_div itself */
extern Datum sift_numavg_datum(const SiftNumAvg *a);

#endif							/* SIFT_AGG_V6_DATEBIN_WIN_PG_H */
