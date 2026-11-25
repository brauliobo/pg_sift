-- The TID bridge of core/tidbridge.h (the pages a structured scan hands a text scan): random keys in random order, with repeats, under an unlimited byte budget, a small one and none,
-- against an oracle of sorted keys. A TID that was added is never answered NO; an exact page answers NO for a TID that was not added; the page masks, blooms and key range are those of
-- the oracle's pages; the offset masks of every block equal those of the filter masks of core/filter.c (all ones for a lossy page); a page is an ARRAY up to 16 offsets and a BITMAP above;
-- the bytes stop growing when the budget is spent; the bridge of the sorted keys is the bridge of the shuffled ones; scores and row locators come back; the bridge of a list of
-- document ids is the bridge of their keys; a reset bridge takes the next chunk as a new one. Every failure count is 0, every check count is not.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

WITH names(i, name) AS (
    VALUES (1, 'layout'), (2, 'kinds'), (3, 'nomiss'), (4, 'exact'), (5, 'masks'), (6, 'filter'), (7, 'budget'),
           (8, 'order'), (9, 'scores'), (10, 'rowloc'), (11, 'docids'), (12, 'point'), (13, 'reset'), (14, 'intersect')
)
SELECT s AS seed, name, r[2 * i - 1] > 0 AS checked, r[2 * i] AS failed
FROM generate_series(1, 3) s, LATERAL (SELECT sift._test_tidbridge_check(s, 4) r) x, names
ORDER BY s, i;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
