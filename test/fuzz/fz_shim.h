/*
 * fz_shim.h - the backend stand-in of the fuzz targets (tools/pgshim.c and fz_shim.c): the allocation, error and CPU symbols the
 * decoders need, so that they link and run without a server, under AddressSanitizer and UBSan.
 */
#ifndef FZ_SHIM_H
#define FZ_SHIM_H

#include "postgres.h"

#include "sift.h"
#include "sift_cpu.h"
#include "bm25/kernels.h"
#include "../../tools/pgshim.h"

/* Once per process: CPU detection, dispatch tables of the kernels */
extern void fz_init(void);
/*
 * Runs fn(arg) the way a backend runs a statement: an ereport(ERROR) unwinds to here and fz_try returns false. The classes a decoder of
 * untrusted bytes may raise are the corruption ones (DATA_CORRUPTED, INDEX_CORRUPTED), FEATURE_NOT_SUPPORTED and the limits; any other
 * class (an elog(ERROR) is XX000) is a finding and aborts, as does an ereport outside fz_try.
 */
extern bool fz_try(void (*fn) (void *), void *arg);
/* SIMD levels this CPU runs (scalar first); make the tables of one the active ones */
extern int	fz_levels(SiftSimdLevel lv[3]);
extern void fz_use_level(SiftSimdLevel level);
/* Aborts with a message and the reproducing property name */
extern void fz_bad(const char *fmt,...) pg_attribute_printf(1, 2) __attribute__((noreturn));

/* The bytes sift_sr_get serves (offsets past them raise INDEX_CORRUPTED, as the real stream reader does); the buffer must outlive the cursors */
extern void fz_stream_set(const uint8 *buf, uint64 len);

/* Is the named check in the comma list FZ_SKIP: how a run goes on past a finding already recorded in the design notes */
extern bool fz_skip(const char *name);

/* Copy of the input in an exact-size malloc block (AddressSanitizer sees any overread), optionally followed by pad zero bytes, aligned to align (a power of two) */
extern uint8 *fz_copy(const uint8 *data, size_t len, size_t pad, size_t align);

#endif							/* FZ_SHIM_H */
