-- The rollup of the rows of a unit (column format minor 9, docs/COL-PREAGG.md section 2): written only for an index with the reloption rollup_dims and a
-- setting sift.col_format_minor of 9, so that a 3.2 library keeps reading what an index without it writes; read back by the same decoder that refuses a
-- damaged rollup; checked against the chunks by sift.col_verify; kept by promotion, merge and VACUUM; bounded by the cap of cells a unit may hold.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE cr (id int, a int, b text, c int, v bigint, w numeric(10, 2), f float8, pad text) WITH (autovacuum_enabled = off);
INSERT INTO cr
SELECT g, CASE WHEN g % 13 = 0 THEN NULL ELSE g % 12 END, CASE WHEN g % 17 = 0 THEN NULL ELSE 'b0' || (g * 7 % 8) END, g % 5,
       CASE WHEN g % 7 = 0 THEN NULL ELSE (g * 977) % 100000 - 50000 END, CASE WHEN g % 11 = 0 THEN NULL ELSE ((g * 91) % 20000) / 100.0 - 50 END,
       (g % 71) / 7.0 - 3, repeat('p', 100)
FROM generate_series(1, 30000) g;

-- the minor of the segments of an index: 4 without the option (this schema has no jsonb paths), under the setting 8 with it, 9 with it, 9 with both options
CREATE INDEX cr_off ON cr USING sift_col (id, a, b, c, v, w, f) WITH (write_buffer_limit = 4096);
SET sift.col_format_minor = 8;
CREATE INDEX cr_seven ON cr USING sift_col (id, a, b, c, v, w, f) WITH (write_buffer_limit = 4096, rollup_dims = 'a, b');
RESET sift.col_format_minor;
CREATE INDEX cr_on ON cr USING sift_col (id, a, b, c, v, w, f) WITH (write_buffer_limit = 4096, rollup_dims = 'a, b');
CREATE INDEX cr_both ON cr USING sift_col (id, a, b, c, v, w, f) WITH (write_buffer_limit = 4096, rollup_dims = 'a, b, c: v, w', cube_minmax = true);
CREATE INDEX cr_nocube ON cr USING sift_col (id, a, b, c, v, w, f) WITH (write_buffer_limit = 4096, rollup_dims = 'a, b', preagg = 'off');
-- more cells than the cap in every unit: no rollup, the units scan
CREATE INDEX cr_cap ON cr USING sift_col (id, a, b, c, v, w, f) WITH (write_buffer_limit = 4096, rollup_dims = 'id, a');
SELECT 'cr_off' AS idx, array_agg(DISTINCT format) AS minors FROM sift._test_col_formats('cr_off')
UNION ALL SELECT 'cr_seven', array_agg(DISTINCT format) FROM sift._test_col_formats('cr_seven')
UNION ALL SELECT 'cr_on', array_agg(DISTINCT format) FROM sift._test_col_formats('cr_on')
UNION ALL SELECT 'cr_both', array_agg(DISTINCT format) FROM sift._test_col_formats('cr_both')
UNION ALL SELECT 'cr_nocube', array_agg(DISTINCT format) FROM sift._test_col_formats('cr_nocube')
UNION ALL SELECT 'cr_cap', array_agg(DISTINCT format) FROM sift._test_col_formats('cr_cap') ORDER BY 1;
SELECT sift.col_verify('cr_off', true), sift.col_verify('cr_seven', true), sift.col_verify('cr_on', true), sift.col_verify('cr_both', true),
       sift.col_verify('cr_nocube', true), sift.col_verify('cr_cap', true);

-- the rollups of the index with the option are in its units, the others have none; a damaged rollup is refused, and one that is accepted is still sound
SELECT idx, blobs > 0 AS has_rollups, intact = blobs AS intact_decodes, cut > 0 AS cuts_made, cut_accepted = 0 AS cuts_refused, damaged > 0 AS damages_made,
       accepted < damaged AS some_refused, unsound = 0 AS accepted_are_sound
FROM (SELECT 'cr_on' AS idx, * FROM sift._test_col_cube_damage('cr_on', 4, true)
      UNION ALL SELECT 'cr_both', * FROM sift._test_col_cube_damage('cr_both', 4, true)) d ORDER BY idx;
SELECT idx, blobs FROM (SELECT 'cr_off' AS idx, blobs FROM sift._test_col_cube_damage('cr_off', 4, true)
      UNION ALL SELECT 'cr_seven', blobs FROM sift._test_col_cube_damage('cr_seven', 4, true)
      UNION ALL SELECT 'cr_nocube', blobs FROM sift._test_col_cube_damage('cr_nocube', 4, true)
      UNION ALL SELECT 'cr_cap', blobs FROM sift._test_col_cube_damage('cr_cap', 4, true)) d ORDER BY idx;
-- a segment written under the setting 8 has no cells: sift.col_refresh counts it, writes them as minor 9, and a second call has nothing left; cr_nocube and cr_cap never ask
SELECT * FROM sift.col_refresh('cr_seven', 0);
SELECT * FROM sift.col_refresh('cr_nocube', 0);
SELECT * FROM sift.col_refresh('cr_seven');
SELECT * FROM sift.col_refresh('cr_seven');
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cr_seven');
SELECT blobs > 0 AS has_rollups, intact = blobs AS intact_decodes FROM sift._test_col_cube_damage('cr_seven', 4, true);
SELECT sift.col_verify('cr_seven', true);
-- the cube of the units is untouched by it, with or without min and max
SELECT idx, blobs > 0 AS has_cubes, with_minmax = blobs AS all_with_minmax
FROM (SELECT 'cr_on' AS idx, * FROM sift._test_col_cube_damage('cr_on', 4)
      UNION ALL SELECT 'cr_both', * FROM sift._test_col_cube_damage('cr_both', 4)) d ORDER BY idx;

-- the cells are in the bytes: the same data costs more than without, and the cap keeps a unit of distinct ids from storing them
SELECT (SELECT preagg_bytes FROM sift.col_index_info('cr_on')) > (SELECT preagg_bytes FROM sift.col_index_info('cr_off')) AS cells_cost_bytes;
SELECT (SELECT preagg_bytes FROM sift.col_index_info('cr_cap')) = (SELECT preagg_bytes FROM sift.col_index_info('cr_off')) AS over_the_cap_costs_nothing;

-- an index of the old kind takes the option later: the segments written before keep their minor, the ones after are minor 9 and a merge writes one
-- segment of minor 9; switched off again, the next merge writes minor 4
INSERT INTO cr SELECT g, g % 12, 'b0' || (g % 8), g % 5, g, g / 7.0, g / 3.0, repeat('p', 100) FROM generate_series(30001, 32000) g;
SELECT sift.col_promote('cr_off') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cr_off');
ALTER INDEX cr_off SET (rollup_dims = 'a, b');
INSERT INTO cr SELECT g, g % 12, 'b0' || (g % 8), g % 5, g, g / 7.0, g / 3.0, repeat('p', 100) FROM generate_series(32001, 34000) g;
SELECT sift.col_promote('cr_off') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cr_off');
SELECT sift.col_verify('cr_off', true);
SELECT sift.col_merge('cr_off', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('cr_off');
SELECT blobs > 0 AS has_rollups, intact = blobs AS intact_decodes FROM sift._test_col_cube_damage('cr_off', 4, true);
SELECT sift.col_verify('cr_off', true);
ALTER INDEX cr_off SET (rollup_dims = '');
INSERT INTO cr SELECT g, g % 12, 'b0' || (g % 8), g % 5, g, g / 7.0, g / 3.0, repeat('p', 100) FROM generate_series(34001, 36000) g;
SELECT sift.col_promote('cr_off') >= 0 AS promoted;
SELECT sift.col_merge('cr_off', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cr_off');
SELECT blobs AS no_rollups FROM sift._test_col_cube_damage('cr_off', 4, true);
SELECT sift.col_verify('cr_off', true);

-- deletes and VACUUM, then a merge: the segments written with them keep them
DELETE FROM cr WHERE id % 3 = 0;
VACUUM (freeze, index_cleanup on, disable_page_skipping) cr;
SELECT sift.col_merge('cr_on', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) cr;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cr_on');
SELECT blobs > 0 AS has_rollups, intact = blobs AS intact_decodes FROM sift._test_col_cube_damage('cr_on', 4, true);
SELECT sift.col_verify('cr_on', true);

-- the option is checked against the columns of the index when it is set
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a');
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a, nosuch');
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a, f');
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a, a');
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a, b: f');
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a, b: nosuch');
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a, b,');
CREATE INDEX cr_bad ON cr USING sift_col (id, a, b, f) WITH (rollup_dims = 'a, b: id, id, id, id, id, id, id, id, id');
ALTER INDEX cr_on SET (rollup_dims = 'b, nosuch');
ALTER INDEX cr_on SET (rollup_dims = 'off');
SELECT sift.col_verify('cr_on', true);

DROP TABLE cr;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
