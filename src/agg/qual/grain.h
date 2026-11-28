/*
 * agg/qual/grain.h - the calendar cells an expression of a date or timestamp column keeps its value over (agg/qual/grain.c): date_trunc, extract, date_part and the date casts give one value
 * for every instant of a day, a month or a year of the column, as their unit allows, so a key that reads the column only through them has one value in each calendar cell of that grain.
 */
#ifndef SIFT_AGG_V6_GRAIN_H
#define SIFT_AGG_V6_GRAIN_H

#include "postgres.h"

#include "nodes/primnodes.h"

#define SIFT_V6_CELL_DAY		0x1
#define SIFT_V6_CELL_MONTH		0x2
#define SIFT_V6_CELL_YEAR		0x4

/*
 * The cells (SIFT_V6_CELL_*) every value e takes is constant over: each Var of relid it reads is one date, timestamp or timestamptz column, read only through calendar calls on the clock its
 * cells are counted on (the days of a date or a timestamp; the UTC days of a timestamptz, read AT TIME ZONE or in a zone of UTC); 0 for any other expression
 */
extern int	sift_v6_calendar_grains(const Node *e, Index relid);

#endif							/* SIFT_AGG_V6_GRAIN_H */
