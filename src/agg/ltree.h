/*
 * agg/ltree.h - the tournament (loser) tree of the k-way merges of agg/: the smallest of k sorted inputs costs ceil(log2 k) compares, where a scan of the k heads costs k. The tree knows nothing
 * of the inputs: less(arg, a, b) says whether the head of input a comes before the head of input b (an input at its end comes after every other, whatever the order), and a caller that wants
 * equal heads to leave in the order of their inputs ends its compare with the numbers of the inputs. Inputs numbered nin and above are empty.
 *
 *   sift_ltree_init(&lt, nin, less, arg);         the heads are already read
 *   while (sift_ltree_head(&lt) is not at its end) { take the head; refill it; sift_ltree_replay(&lt); }
 */
#ifndef SIFT_AGG_LTREE_H
#define SIFT_AGG_LTREE_H

#include "postgres.h"

typedef bool (*SiftLTreeLess) (void *arg, int a, int b);

typedef struct SiftLTree
{
	int			nin;
	int			size;			/* a power of 2 >= nin */
	int			winner;
	int		   *loser;			/* [1 .. size - 1]: the input that lost at the node */
	SiftLTreeLess less;
	void	   *arg;
} SiftLTree;

static inline bool
sift_ltree_before(const SiftLTree *lt, int a, int b)
{
	if (a >= lt->nin)
		return false;
	if (b >= lt->nin)
		return true;
	return lt->less(lt->arg, a, b);
}

static inline void
sift_ltree_init(SiftLTree *lt, int nin, SiftLTreeLess less, void *arg)
{
	int			size = 1;
	int		   *won;

	while (size < nin)
		size <<= 1;
	lt->nin = nin;
	lt->size = size;
	lt->less = less;
	lt->arg = arg;
	lt->loser = palloc(sizeof(int) * (size_t) Max(size, 2));
	won = palloc(sizeof(int) * 2 * (size_t) size);
	for (int i = 0; i < size; i++)
		won[size + i] = i;
	for (int node = size - 1; node >= 1; node--)
	{
		const int	a = won[2 * node],
					b = won[2 * node + 1];

		if (sift_ltree_before(lt, a, b))
		{
			won[node] = a;
			lt->loser[node] = b;
		}
		else
		{
			won[node] = b;
			lt->loser[node] = a;
		}
	}
	lt->winner = size > 1 ? won[1] : 0;
	pfree(won);
}

/* The input whose head is the smallest: one of the nin inputs, at its end when all of them are */
static inline int
sift_ltree_head(const SiftLTree *lt)
{
	return lt->winner;
}

/* The head of input lt->winner changed (or its input ended): the tournament of its path again */
static inline void
sift_ltree_replay(SiftLTree *lt)
{
	int			cand = lt->winner;

	for (int node = (lt->size + cand) >> 1; node >= 1; node >>= 1)
		if (sift_ltree_before(lt, lt->loser[node], cand))
		{
			const int	t = lt->loser[node];

			lt->loser[node] = cand;
			cand = t;
		}
	lt->winner = cand;
}

static inline void
sift_ltree_free(SiftLTree *lt)
{
	pfree(lt->loser);
}

#endif							/* SIFT_AGG_LTREE_H */
