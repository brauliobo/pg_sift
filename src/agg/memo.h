/*
 * agg/memo.h - the rows of a correlated child by the values of its parameters, shared by the processes of a parallel scan (agg/memo.c)
 */
#ifndef SIFT_AGG_MEMO_H
#define SIFT_AGG_MEMO_H

#include "access/htup.h"

typedef struct ChildMemo ChildMemo;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

Size memo_size(void);
void memo_init(ChildMemo *m);
/* the rows stored for the key (their number in *nrows, each read with memo_row), else NULL */
const char *memo_find(ChildMemo *m, const char *key, uint32 klen, uint32 *nrows);
const char *memo_row(const char *at, MinimalTuple *mt);
/* stores the rows of the key unless another process did; silently nothing when the table or the arena is full */
void memo_put(ChildMemo *m, const char *key, uint32 klen, int nrows, MinimalTuple *rows);

#pragma GCC visibility pop

#endif
