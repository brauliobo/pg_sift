-- The chained directory against a model: random sequences of insertions, deletions, reversals and edits of directory entries, each state compared with a
-- reference list, with leaves of 3 entries and a root of 8 leaves so that every path of the chain writer runs: leaves kept as they are, regions repacked,
-- the root overflowing into a repack of the whole directory
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_dir_flat_max = 4;
SET sift.test_dir_leaf_max = 3;
SET sift.test_dir_root_max = 8;
CREATE TABLE dm (v int);
CREATE INDEX dm_idx ON dm USING sift_store_test (v);

-- block 1 holds a small directory until it has more than 4 entries
SELECT sift._test_dir_edit('dm_idx', 'ins', 0, 4) > 0 AS inserted;
SELECT chained, nsegs, format_minor FROM sift._test_dir_info('dm_idx');
SELECT sift._test_dir_edit('dm_idx', 'ins', 4, 1) > 0 AS inserted;
SELECT chained, nsegs, chain_blocks, format_minor FROM sift._test_dir_info('dm_idx');

CREATE TABLE model (ord int, gen int8, ndocs int8);
INSERT INTO model SELECT * FROM sift._test_dir_list('dm_idx');

-- one step of the model: the same edit on the reference list
CREATE FUNCTION step(op text, p int, n int, first_gen int8) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	IF op = 'ins' THEN
		UPDATE model SET ord = ord + n WHERE ord >= p;
		INSERT INTO model SELECT p + i, first_gen + i, (first_gen + i) * 7 FROM generate_series(0, n - 1) i;
	ELSIF op = 'del' THEN
		DELETE FROM model WHERE ord >= p AND ord < p + n;
		UPDATE model SET ord = ord - n WHERE ord >= p + n;
	ELSIF op = 'rev' THEN
		UPDATE model SET ord = 2 * p + n - 1 - ord WHERE ord >= p AND ord < p + n;
	ELSIF op = 'set' THEN
		UPDATE model SET ndocs = ndocs + 1 WHERE ord >= p AND ord < p + n;
	END IF;
END $$;

-- 400 random edits of up to 20 entries; after each the directory equals the model, its chain is sound and the blocks do not collide with the free list
CREATE FUNCTION run(steps int, maxn int) RETURNS TABLE (mismatches int, biggest int, smallest int) LANGUAGE plpgsql AS $$
DECLARE
	op text; p int; n int; cnt int; g int8; bad int := 0; hi int := 0; lo int := 1000000;
	ops text[] := ARRAY['ins', 'ins', 'del', 'rev', 'set', 'set'];
BEGIN
	PERFORM setseed(0.42);
	FOR i IN 1..steps LOOP
		SELECT count(*) INTO cnt FROM model;
		op := ops[1 + floor(random() * array_length(ops, 1))::int];
		IF cnt >= maxn THEN op := 'del'; END IF;
		IF cnt = 0 THEN op := 'ins'; END IF;
		n := 1 + floor(random() * 6)::int;
		IF op = 'ins' THEN
			n := least(n, maxn - cnt);
			p := floor(random() * (cnt + 1))::int;
		ELSE
			n := least(n, cnt);
			p := floor(random() * (cnt - n + 1))::int;
		END IF;
		g := sift._test_dir_edit('dm_idx', op, p, n);
		PERFORM step(op, p, n, g);
		IF EXISTS (SELECT 1 FROM (SELECT ord, gen, ndocs FROM sift._test_dir_list('dm_idx') EXCEPT SELECT ord, gen, ndocs FROM model) a)
		   OR EXISTS (SELECT 1 FROM (SELECT ord, gen, ndocs FROM model EXCEPT SELECT ord, gen, ndocs FROM sift._test_dir_list('dm_idx')) b) THEN
			bad := bad + 1;
		END IF;
		PERFORM sift._test_dir_check('dm_idx');
		SELECT count(*) INTO cnt FROM model;
		hi := greatest(hi, cnt); lo := least(lo, cnt);
	END LOOP;
	RETURN QUERY SELECT bad, hi, lo;
END $$;

SELECT mismatches, biggest <= 24 AS within_the_root, smallest >= 0 AS ok FROM run(400, 24);
SELECT chained, nsegs = (SELECT count(*) FROM model) AS equal FROM sift._test_dir_info('dm_idx');

-- the whole directory deleted, then written again: a chained directory stays chained and empty
SELECT sift._test_dir_edit('dm_idx', 'del', 0, (SELECT count(*)::int FROM model)) AS deleted;
SELECT chained, nsegs FROM sift._test_dir_info('dm_idx');
SELECT sift._test_dir_check('dm_idx');

-- a directory of more entries than a root of the default leaf size holds in one level of leaves, written in steps
RESET sift.test_dir_flat_max;
RESET sift.test_dir_leaf_max;
RESET sift.test_dir_root_max;
SELECT sift._test_dir_edit('dm_idx', 'ins', 0, 3000) > 0 AS inserted;
SELECT chained, nsegs, chain_blocks = 1 + ceil(3000 / 145.0) AS blocks FROM sift._test_dir_info('dm_idx');
-- a change in the middle writes one leaf and the root, nothing else: the version moves by one and 2 pages are new
SELECT version AS v0 FROM sift._test_dir_info('dm_idx') \gset
SELECT sift._test_dir_edit('dm_idx', 'set', 1500, 1);
SELECT version - :v0 AS steps, nsegs, chain_blocks FROM sift._test_dir_info('dm_idx');
SELECT sift._test_dir_check('dm_idx');
SELECT sift._test_dir_edit('dm_idx', 'ins', 1500, 10) > 0 AS inserted;
SELECT nsegs, chain_blocks FROM sift._test_dir_info('dm_idx');
SELECT count(*) AS entries, count(DISTINCT gen) AS distinct_gens, min(ord) AS first, max(ord) AS last FROM sift._test_dir_list('dm_idx');
SELECT sift._test_dir_edit('dm_idx', 'del', 0, 3000 + 10 - 5);
SELECT nsegs, chained FROM sift._test_dir_info('dm_idx');
SELECT sift._test_dir_check('dm_idx') > 0 AS chain_blocks;
-- the pages of the replaced chains went through the free list and are used again: the file does not grow with the number of updates
SELECT pg_relation_size('dm_idx') / 8192 AS blocks_before \gset
CREATE PROCEDURE edits(n int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		PERFORM sift._test_dir_edit('dm_idx', 'set', 0, 3);
		COMMIT;
	END LOOP;
END $$;
CALL edits(50);
SELECT pg_relation_size('dm_idx') / 8192 <= :blocks_before + 8 AS no_growth;

-- a chain that lies high in the file is written again below a limit once blocks below are free
CREATE TABLE dr (v int);
CREATE INDEX dr_idx ON dr USING sift_store_test (v);
SET sift.test_dir_flat_max = 2;
SET sift.test_dir_leaf_max = 3;
SELECT count(sift._test_seg_write('dr_idx', 100, i)) AS written FROM generate_series(1, 6) i;
SELECT sift._test_alloc('dr_idx', 1, 40, 0) > 0 AS taken;
SELECT count(sift._test_seg_write('dr_idx', 100, i)) AS written FROM generate_series(7, 8) i;
SELECT top_chain_block > 40 AS high, nsegs FROM sift._test_dir_info('dr_idx');
SELECT sift._test_sweep('dr_idx') >= 40 AS swept;
SELECT txid_current() IS NOT NULL AS moves_the_horizon;
SELECT txid_current() IS NOT NULL AS moves_the_horizon;
SELECT sift._test_dir_edit('dr_idx', 'rechain', 30, 0);
SELECT top_chain_block < 30 AS low, nsegs FROM sift._test_dir_info('dr_idx');
SELECT sift._test_dir_check('dr_idx') > 0 AS chain_blocks;
SELECT count(*) AS segments, bool_and(sift._test_seg_read('dr_idx', gen) = payload) AS intact FROM sift._test_segments('dr_idx');
SELECT sift._test_check('dr_idx') AS orphans;
-- no free run below the limit: the update fails and the directory stays as it was
SELECT sift._test_dir_edit('dr_idx', 'rechain', 3, 0);
SELECT nsegs, top_chain_block < 30 AS still_low FROM sift._test_dir_info('dr_idx');
DROP TABLE dr;
DROP TABLE dm;
DROP TABLE model;
DROP FUNCTION step(text, int, int, int8);
DROP FUNCTION run(int, int);
DROP PROCEDURE edits(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
