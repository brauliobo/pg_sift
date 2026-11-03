-- Where the blocks of a sift_col file are: sift.col_extents lists every run (segments, buffers, free list, free extents), sift.col_space sums them and says
-- what the shrink of VACUUM has still to move. The runs tile the file up to its high-water mark, with no overlap, before and after churn and a VACUUM.
CREATE EXTENSION pg_sift;
CREATE FUNCTION tiles(idx regclass) RETURNS bool LANGUAGE sql AS $$
	SELECT sum(nblocks) = (SELECT high_water FROM sift.col_index_info(idx))
		AND bool_and(first_block >= end_prev) AND min(first_block) = 0
	FROM (SELECT nblocks, first_block, coalesce(lag(first_block + nblocks) OVER (ORDER BY first_block), 0) AS end_prev FROM sift.col_extents(idx)) e $$;
CREATE TABLE em (id int PRIMARY KEY, grp int, amount numeric(10,2), note text) WITH (autovacuum_enabled = off);
INSERT INTO em SELECT g, g % 50, g % 1000, 'note ' || md5(g::text) FROM generate_series(1, 100000) g;
CREATE INDEX em_col ON em USING sift_col (grp, amount);
SELECT tiles('em_col') AS tiles_after_build;
SELECT s.used_blocks + s.reusable_blocks + s.pending_blocks = s.high_water AS space_sums,
	s.reusable_blocks + s.pending_blocks = i.free_blocks AS free_matches, s.reusable_blocks = i.reusable_blocks AS reusable_matches,
	s.relation_blocks = i.relation_blocks AS blocks_match, s.tail_what IS NOT NULL AS has_tail, s.shrink_blocks >= 0 AS movable
FROM sift.col_space('em_col') s, sift.col_index_info('em_col') i;
SELECT what, count(*) > 0 AS present FROM sift.col_extents('em_col') WHERE what IN ('segment extent', 'segment header', 'metapage and directory') GROUP BY what ORDER BY what;

-- churn leaves free extents; their count and the pending ones agree with col_index_info
UPDATE em SET amount = amount + 1 WHERE id % 3 = 0;
DELETE FROM em WHERE id % 20 = 1;
SELECT sift.col_merge('em_col', true) > 0 AS merged;
SELECT tiles('em_col') AS tiles_after_churn;
SELECT count(*) FILTER (WHERE what = 'free extent') = (SELECT free_extents FROM sift.col_index_info('em_col')) AS free_extents_match,
	coalesce(sum(nblocks) FILTER (WHERE what = 'free extent' AND NOT pending), 0) = (SELECT reusable_blocks FROM sift.col_index_info('em_col')) AS reusable_match
FROM sift.col_extents('em_col');
SELECT largest_free_extent = (SELECT max(nblocks) FROM sift.col_extents('em_col') WHERE what = 'free extent') AS largest_matches FROM sift.col_space('em_col');
VACUUM em;
SELECT tiles('em_col') AS tiles_after_vacuum;
SELECT sift.col_verify('em_col', true);
DROP TABLE em;
DROP FUNCTION tiles(regclass);
DROP EXTENSION pg_sift;
