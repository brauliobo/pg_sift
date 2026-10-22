-- The min and max of the integer measures in the cube (column format minor 7, docs/COL-PREAGG.md section 2): written only for an index with the option
-- cube_minmax and a setting sift.col_format_minor of 7, so that a 3.2 library keeps reading what an index without it writes; read back by the same
-- decoder that refuses a damaged cube; checked against the chunks by sift.col_verify; kept by promotion, merge and VACUUM.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE cm (id int, kind int, grp text, val bigint, small int, amt numeric(10, 2), flt float8, pad text) WITH (autovacuum_enabled = off);
INSERT INTO cm
SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), CASE WHEN g % 7 = 0 THEN NULL ELSE (g * 977) % 100000 - 50000 END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 1000 - 400 END, ((g * 91) % 20000) / 100.0 - 50, (g % 71) / 7.0 - 3, repeat('p', 100)
FROM generate_series(1, 30000) g;

-- the minor of the segments of an index: 6 without the option, under the setting 6 with it, 7 with both
CREATE INDEX cm_off ON cm USING sift_col (id, kind, grp, val, small, amt, flt) WITH (write_buffer_limit = 4096);
SET sift.col_format_minor = 6;
CREATE INDEX cm_six ON cm USING sift_col (id, kind, grp, val, small, amt, flt) WITH (write_buffer_limit = 4096, cube_minmax = true);
RESET sift.col_format_minor;
CREATE INDEX cm_on ON cm USING sift_col (id, kind, grp, val, small, amt, flt) WITH (write_buffer_limit = 4096, cube_minmax = true);
CREATE INDEX cm_nocube ON cm USING sift_col (id, kind, grp, val, small, amt, flt) WITH (write_buffer_limit = 4096, cube_minmax = true, preagg = 'off');
SELECT 'cm_off' AS idx, array_agg(DISTINCT format) AS minors FROM sift._test_col_formats('cm_off')
UNION ALL SELECT 'cm_six', array_agg(DISTINCT format) FROM sift._test_col_formats('cm_six')
UNION ALL SELECT 'cm_on', array_agg(DISTINCT format) FROM sift._test_col_formats('cm_on')
UNION ALL SELECT 'cm_nocube', array_agg(DISTINCT format) FROM sift._test_col_formats('cm_nocube') ORDER BY 1;
SELECT sift.col_verify('cm_off', true), sift.col_verify('cm_six', true), sift.col_verify('cm_on', true), sift.col_verify('cm_nocube', true);

-- the cubes of the index with the option hold them, the others do not; a damaged cube is refused, and one that is accepted is still sound; the census
-- of the sidecar that sift.col_refresh takes reads the same bytes without leaving them and calls every cut one odd
SELECT 'cm_on' AS idx, blobs > 0 AS has_cubes, intact = blobs AS intact_decodes, with_minmax = blobs AS all_with_minmax, cut > 0 AS cuts_made,
       cut_accepted = 0 AS cuts_refused, damaged > 0 AS damages_made, accepted < damaged AS some_refused, unsound = 0 AS accepted_are_sound,
       census_cut_odd = cut - blobs AS census_calls_cuts_odd, census_odd > 0 AS census_sees_damage
FROM sift._test_col_cube_damage('cm_on', 4)
UNION ALL
SELECT 'cm_off', blobs > 0, intact = blobs, with_minmax = 0, cut > 0, cut_accepted = 0, damaged > 0, accepted < damaged, unsound = 0, census_cut_odd = cut - blobs, census_odd > 0
FROM sift._test_col_cube_damage('cm_off', 4)
UNION ALL
SELECT 'cm_six', blobs > 0, intact = blobs, with_minmax = 0, cut > 0, cut_accepted = 0, damaged > 0, accepted < damaged, unsound = 0, census_cut_odd = cut - blobs, census_odd > 0
FROM sift._test_col_cube_damage('cm_six', 4)
ORDER BY 1;

-- min and max are in the bytes: the same data costs more than without
SELECT (SELECT preagg_bytes FROM sift.col_index_info('cm_on')) > (SELECT preagg_bytes FROM sift.col_index_info('cm_off')) AS minmax_costs_bytes;

-- an index of the old kind takes the option later: the segments written before keep their minor, the ones after are minor 7 and a merge writes one
-- segment of minor 7; switched off again, the next merge writes minor 6
INSERT INTO cm SELECT g, g % 6, 'g' || (g % 30), g, g % 9, g / 7.0, g / 3.0, repeat('p', 100) FROM generate_series(30001, 32000) g;
SELECT sift.col_promote('cm_off') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cm_off');
ALTER INDEX cm_off SET (cube_minmax = true);
INSERT INTO cm SELECT g, g % 6, 'g' || (g % 30), g, g % 9, g / 7.0, g / 3.0, repeat('p', 100) FROM generate_series(32001, 34000) g;
SELECT sift.col_promote('cm_off') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cm_off');
SELECT sift.col_verify('cm_off', true);
SELECT sift.col_merge('cm_off', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('cm_off');
SELECT blobs > 0 AS has_cubes, with_minmax = blobs AS all_with_minmax FROM sift._test_col_cube_damage('cm_off', 4);
SELECT sift.col_verify('cm_off', true);
ALTER INDEX cm_off SET (cube_minmax = false);
INSERT INTO cm SELECT g, g % 6, 'g' || (g % 30), g, g % 9, g / 7.0, g / 3.0, repeat('p', 100) FROM generate_series(34001, 36000) g;
SELECT sift.col_promote('cm_off') >= 0 AS promoted;
SELECT sift.col_merge('cm_off', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cm_off');
SELECT blobs > 0 AS has_cubes, with_minmax = 0 AS none_with_minmax FROM sift._test_col_cube_damage('cm_off', 4);
SELECT sift.col_verify('cm_off', true);

-- deletes and VACUUM, then a merge: the segments written with them keep them
DELETE FROM cm WHERE id % 3 = 0;
VACUUM (freeze, index_cleanup on, disable_page_skipping) cm;
SELECT sift.col_merge('cm_on', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) cm;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cm_on');
SELECT blobs > 0 AS has_cubes, with_minmax = blobs AS all_with_minmax FROM sift._test_col_cube_damage('cm_on', 4);
SELECT sift.col_verify('cm_on', true);

-- the option is a boolean
CREATE INDEX cm_bad ON cm USING sift_col (id, kind, val) WITH (cube_minmax = 'maybe');

DROP TABLE cm;
DROP EXTENSION pg_sift_test CASCADE;
