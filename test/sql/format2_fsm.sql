-- The allocator of format major 2: exact extents over power-of-two size classes of 1 to 1024 blocks, the metadata slabs, the queue of released extents that is reused only past its transaction,
-- and the coalescing of adjacent free extents.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');
SELECT sift._test_f2_fsm('t_idx');

-- An allocation is exactly the blocks asked for: no block of it is left unused
SELECT n AS asked, (sift._test_f2_alloc('t_idx', n)).nblocks_out AS got FROM unnest(ARRAY[1, 2, 4, 5, 16, 17, 64, 100, 256, 300, 1024]) n;
SELECT sift._test_f2_fsm('t_idx');
SELECT sift._test_f2_check('t_idx');

-- Four adjacent single blocks, released without a transaction to wait for, join into one extent of 4 blocks
SELECT sift._test_f2_alloc('t_idx', 1) IS NOT NULL AS pad;
SELECT (a).start AS s1 FROM (SELECT sift._test_f2_alloc('t_idx', 1) AS a) q \gset
SELECT (a).start AS s2 FROM (SELECT sift._test_f2_alloc('t_idx', 1) AS a) q \gset
SELECT (a).start AS s3 FROM (SELECT sift._test_f2_alloc('t_idx', 1) AS a) q \gset
SELECT (a).start AS s4 FROM (SELECT sift._test_f2_alloc('t_idx', 1) AS a) q \gset
SELECT :s2 = :s1 + 1 AND :s3 = :s2 + 1 AND :s4 = :s3 + 1 AS adjacent;
SELECT sift._test_f2_release('t_idx', :s1, 1, 'none');
SELECT sift._test_f2_release('t_idx', :s2, 1, 'none');
SELECT sift._test_f2_release('t_idx', :s3, 1, 'none');
SELECT sift._test_f2_release('t_idx', :s4, 1, 'none');
SELECT sift._test_f2_fsm('t_idx');
SELECT sift._test_f2_reclaim('t_idx');
SELECT sift._test_f2_fsm('t_idx');
SELECT sift._test_f2_check('t_idx');

-- A block released by a transaction still running is not handed out; once that transaction is past, it is
SELECT (a).start AS q1 FROM (SELECT sift._test_f2_alloc('t_idx', 1) AS a) q \gset
SELECT sift._test_f2_release('t_idx', :q1, 1, 'future');
SELECT sift._test_f2_reclaim('t_idx');
SELECT sift._test_f2_fsm('t_idx');
SELECT (sift._test_f2_alloc('t_idx', 1)).start <> :q1 AS other_block;
SELECT sift._test_f2_check('t_idx');
SELECT (a).start AS q2 FROM (SELECT sift._test_f2_alloc('t_idx', 1) AS a) q \gset
SELECT sift._test_f2_release('t_idx', :q2, 1, 'now');
SELECT sift._test_f2_reclaim('t_idx');
SELECT sift._test_f2_fsm('t_idx');
SELECT sift._test_f2_check('t_idx');

-- The metadata slabs grow when their blocks run low, and give blocks back
SELECT count(sift._test_f2_meta_alloc('t_idx')) AS pages FROM generate_series(1, 300);
SELECT sift._test_f2_check('t_idx');

-- A stream is exactly the blocks it asks for, in extents cut off the end of the relation when no free extent holds them
SELECT n AS asked, sum(nblocks_out) AS got, count(*) AS extents FROM unnest(ARRAY[1, 65, 100, 1500, 3000]) n, LATERAL sift._test_f2_alloc_stream('t_idx', n) GROUP BY n ORDER BY n;

-- A free extent is cut for a stream: 100 blocks released are handed out again as 64 + 32 + 4, and nothing of them is lost
SELECT (a).start AS hole FROM (SELECT sift._test_f2_alloc('t_idx', 100) AS a) q \gset
SELECT sift._test_f2_release('t_idx', :hole, 100, 'none');
SELECT sift._test_f2_reclaim('t_idx');
SELECT sift._test_f2_fsm('t_idx');
SELECT count(*) AS extents, sum(nblocks_out) AS got
  FROM sift._test_f2_alloc_stream('t_idx', 100);
SELECT sift._test_f2_fsm('t_idx');
SELECT sift._test_f2_check('t_idx');

-- Free extents are handed out lowest first, and a stream below a block bound takes only what lies below it
SELECT (a).start AS lo FROM (SELECT sift._test_f2_alloc('t_idx', 8) AS a) q \gset
SELECT (a).start AS hi FROM (SELECT sift._test_f2_alloc('t_idx', 8) AS a) q \gset
SELECT sift._test_f2_release('t_idx', :hi, 8, 'none');
SELECT sift._test_f2_release('t_idx', :lo, 8, 'none');
SELECT sift._test_f2_reclaim('t_idx');
SELECT count(*) AS extents, max(start + nblocks_out) <= :lo + 16 AS below_bound
  FROM sift._test_f2_alloc_below('t_idx', 8, :lo + 16);
SELECT count(*) AS extents FROM sift._test_f2_alloc_below('t_idx', 1000000, :lo + 16);
SELECT sift._test_f2_check('t_idx');

-- A stream of 5 blocks takes the free extents of 1 and 2 blocks, which no free extent of 4 blocks is there for, before it extends the file for the rest
CREATE TABLE u (v int);
CREATE INDEX u_idx ON u USING sift_store_test (v);
SELECT sift._test_f2_init('u_idx');
SELECT (a).start AS h1 FROM (SELECT sift._test_f2_alloc('u_idx', 1) AS a) q \gset
SELECT sift._test_f2_alloc('u_idx', 1) IS NOT NULL AS pad_1;
SELECT (a).start AS h2 FROM (SELECT sift._test_f2_alloc('u_idx', 2) AS a) q \gset
SELECT sift._test_f2_alloc('u_idx', 1) IS NOT NULL AS pad_2;
SELECT sift._test_f2_release('u_idx', :h1, 1, 'none');
SELECT sift._test_f2_release('u_idx', :h2, 2, 'none');
SELECT sift._test_f2_reclaim('u_idx');
SELECT sift._test_f2_fsm('u_idx');
SELECT count(*) AS extents, sum(nblocks_out) AS got, count(*) FILTER (WHERE start IN (:h1, :h2)) AS from_holes
  FROM sift._test_f2_alloc_stream('u_idx', 5);
SELECT sift._test_f2_fsm('u_idx');
SELECT sift._test_f2_check('u_idx');

DROP TABLE u;

-- An extent past the largest class is refused
SELECT sift._test_f2_alloc('t_idx', 2000);

DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
