-- sift.col_verify finds, and sift.col_repair restores, the liveness pages of a sift_col index that a VACUUM of 4.2.1 to 4.3.5 damaged in a segment of more than 65,152
-- superblocks: the special space of its first page overwritten (an error on every read), a dirty bit lost (the rows of the unit read as live, no error) or the count of dead
-- rows wrong. The damage is made by hand here, in the bytes of the page.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.on_corruption = error;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	d text;
	h text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
	RETURN SQLSTATE || ': ' || regexp_replace(SQLERRM || coalesce(' | ' || nullif(d, ''), '') || coalesce(' | ' || nullif(h, ''), ''), '[Bb]lock [0-9]+', 'block N', 'g');
END $$;

CREATE TABLE r (id int, v int, pad text) WITH (autovacuum_enabled = false);
ALTER TABLE r ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO r SELECT i, i % 10, repeat('x', 1000) FROM generate_series(1, 6000) i;
SET sift.test_col_max_segments = 1;
CREATE INDEX r_idx ON r USING sift_col (id, v);
RESET sift.test_col_max_segments;
SELECT segments, units > 2 AS several_units FROM sift.col_index_info('r_idx');
DELETE FROM r WHERE id % 7 = 0;
VACUUM (INDEX_CLEANUP ON) r;
SELECT first_block AS live FROM sift.col_extents('r_idx') WHERE what = 'liveness run' \gset
SELECT gen + (1::int8 << 45) AS bad_gen FROM sift.col_segments('r_idx') \gset
SELECT dirty, dead_rows FROM sift.col_segments('r_idx');
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT count(*) AS heap_rows FROM r;
SELECT count(*) AS index_rows FROM sift._test_col_tids('r_idx', '', 2);
SELECT count(*) AS segments_repaired FROM sift.col_repair('r_idx');

-- a lost dirty bit (no error): the dead rows of three units read as live
SELECT sift._test_poke('r_idx', :live, 32, 8, 1);
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT segment, problem, detail FROM sift.check('r_idx');
SELECT count(*) AS index_rows FROM sift._test_col_tids('r_idx', '', 2);
SELECT damaged_pages, units_fixed, ndead_before, ndead_after FROM sift.col_repair('r_idx', true);
SELECT count(*) AS index_rows_after_dry_run FROM sift._test_col_tids('r_idx', '', 2);
SELECT damaged_pages, units_fixed, ndead_before, ndead_after FROM sift.col_repair('r_idx');
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT count(*) AS problems FROM sift.check('r_idx');
SELECT count(*) AS index_rows FROM sift._test_col_tids('r_idx', '', 2);
SELECT count(*) AS segments_repaired FROM sift.col_repair('r_idx');

-- the generation, the flags and the reserved field of the first page: an error on every read
SELECT sift._test_poke('r_idx', :live, 8184, :bad_gen, 8);
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT expect($$ SELECT count(*) FROM sift._test_col_tids('r_idx', '', 2) $$);
SELECT sift._test_poke('r_idx', :live, 8178, 4, 2);
SELECT sift._test_poke('r_idx', :live, 8180, 65536, 4);
SELECT damaged_pages, units_fixed, ndead_before, ndead_after FROM sift.col_repair('r_idx');
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT count(*) AS index_rows FROM sift._test_col_tids('r_idx', '', 2);
SELECT sift._test_poke('r_idx', :live, 8178, 4, 2);
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT damaged_pages, units_fixed, ndead_before, ndead_after FROM sift.col_repair('r_idx');

-- the header of the page
SELECT sift._test_poke('r_idx', :live, 14, 8000, 2);
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT expect($$ SELECT count(*) FROM sift._test_col_tids('r_idx', '', 2) $$);
SELECT damaged_pages, units_fixed, ndead_before, ndead_after FROM sift.col_repair('r_idx');
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);

-- a count of dead rows that is not that of the bits (a unit whose dirty bit was lost is cleared again by the next VACUUM)
SELECT sift._test_poke('r_idx', :live, 28, 99, 4);
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);
SELECT damaged_pages, units_fixed, ndead_before, ndead_after FROM sift.col_repair('r_idx');
SELECT expect($$ SELECT sift.col_verify('r_idx') $$);

-- who may repair, and what
CREATE ROLE regress_live_repair;
SET ROLE regress_live_repair;
SELECT expect($$ SELECT sift.col_repair('r_idx') $$);
RESET ROLE;
CREATE INDEX b_idx ON r USING sift (pad);
SELECT expect($$ SELECT sift.col_repair('b_idx') $$);

DROP TABLE r;
DROP ROLE regress_live_repair;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
