/*
 * core/bitscatter.h - the bits of the rows that hold a value spread over all the rows: bit r of the result is bit rank(r) of the dense bits when row r is valid (rank: the valid rows before it), else 0.
 * A chunk with nulls keeps one value for each row that has one, so a filter that runs on those values gives dense bits, and a scan wants them by row. Portable (core/host.h only).
 */
#ifndef SIFT_BITSCATTER_H
#define SIFT_BITSCATTER_H

#include "core/host.h"

/*
 * row_bits: ceil(nrows / 64) words, bits past nrows zero. dense_bits: one more word than the valid rows need (it is read a word past its end); valid_bits: ceil(nrows / 64) words, of which the
 * bits past nrows are ignored. PDEP on a CPU where it is fast (core/cpu.h SIFT_CPU_BMI2_FAST), else a loop that takes a word of valid rows whole and a sparser one by bit.
 */
extern void sift_bits_scatter_valid(uint64 *row_bits, const uint64 *dense_bits, const uint64 *valid_bits, uint32 nrows);
extern void sift_bits_scatter_valid_scalar(uint64 *row_bits, const uint64 *dense_bits, const uint64 *valid_bits, uint32 nrows);

#endif							/* SIFT_BITSCATTER_H */
