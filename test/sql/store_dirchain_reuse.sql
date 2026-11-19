-- The pages of a chain that an update does not change stay: with leaves of 3 entries and a root of 8 references, 150 entries are 50 leaves under 7 interior pages (6 of 8 leaves, the
-- last of 2), and an update that changes one leaf writes that leaf, the interior page over it and the root, not every interior page.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_dir_flat_max = 4;
SET sift.test_dir_leaf_max = 3;
SET sift.test_dir_root_max = 8;
CREATE TABLE dm (v int);
CREATE INDEX dm_idx ON dm USING sift_store_test (v);
CREATE TABLE seen (blk int8);

-- written(): the blocks the chain has now that the last call of it did not see
CREATE FUNCTION written() RETURNS int LANGUAGE plpgsql AS $$
DECLARE n int;
BEGIN
	SELECT count(*) INTO n FROM sift._test_dir_blocks('dm_idx') b WHERE b.blk NOT IN (SELECT blk FROM seen);
	DELETE FROM seen;
	INSERT INTO seen SELECT b.blk FROM sift._test_dir_blocks('dm_idx') b;
	RETURN n;
END $$;

SELECT sift._test_dir_edit('dm_idx', 'ins', 0, 150) > 0 AS inserted;
SELECT nsegs, chained, chain_blocks FROM sift._test_dir_info('dm_idx');
SELECT written() AS first_write;
SELECT sift._test_dir_edit('dm_idx', 'set', 100, 1) IS NOT NULL AS touched;
SELECT written() AS leaf_interior_root;
SELECT sift._test_dir_edit('dm_idx', 'set', 0, 1) IS NOT NULL AS touched_first;
SELECT written() AS first_leaf_interior_root;
SELECT sift._test_dir_edit('dm_idx', 'del', 140, 1) IS NOT NULL AS deleted;
SELECT written() AS last_leaf_interior_root;
SELECT sift._test_dir_edit('dm_idx', 'ins', 60, 1) > 0 AS inserted_one;
SELECT written() <= 5 AS few_pages_for_an_insert;
SELECT sift._test_dir_check('dm_idx') > 0 AS chain_sound;
SELECT count(*) AS entries FROM sift._test_dir_list('dm_idx');
DROP FUNCTION written();
DROP TABLE dm, seen;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
