-- The superunit block (column format minor 12, docs/COL-PREAGG.md section 10): written only for an index with the option superunit and a setting
-- sift.col_format_minor of 12, so that a 3.2 library keeps reading what an index without it writes; checked against the units by sift.col_verify; and every
-- byte of it damaged, one bit at a time, is refused by the reader or by the check, never read outside the block.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE cs (id int, kind int, grp text, val bigint, small int, amt numeric(10, 2), flt float8, d date, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE cs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO cs
SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), CASE WHEN g % 7 = 0 THEN NULL ELSE (g * 977) % 100000 - 50000 END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 1000 - 400 END, ((g * 91) % 20000) / 100.0 - 50, (g % 71) / 7.0 - 3,
       date '2025-01-01' + g / 100, repeat('p', 1500)
FROM generate_series(1, 9000) g;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) cs;

-- the minor of the segments of an index: without the option, under the setting 7 with it, and with both
CREATE INDEX cs_off ON cs USING sift_col (id, kind, grp, val, small, amt, flt, d) WITH (write_buffer_limit = 4096, cube_minmax = false, superunit = 0);
SET sift.col_format_minor = 7;
CREATE INDEX cs_seven ON cs USING sift_col (id, kind, grp, val, small, amt, flt, d) WITH (write_buffer_limit = 4096, superunit = 3, cube_minmax = false);
SET sift.col_format_minor = 6;
CREATE INDEX cs_six ON cs USING sift_col (id, kind, grp, val, small, amt, flt, d) WITH (write_buffer_limit = 4096, superunit = 3, cube_minmax = false);
RESET sift.col_format_minor;
CREATE INDEX cs_on ON cs USING sift_col (id, kind, grp, val, small, amt, flt, d) WITH (write_buffer_limit = 4096, superunit = 3, cube_minmax = false);
CREATE INDEX cs_nocube ON cs USING sift_col (id, kind, grp, val, small, amt, flt, d) WITH (write_buffer_limit = 4096, superunit = 3, preagg = 'off', cube_minmax = false);
CREATE INDEX cs_mm ON cs USING sift_col (id, kind, grp, val, small, amt, flt, d) WITH (write_buffer_limit = 4096, superunit = 4, cube_minmax = true);
SELECT 'cs_off' AS idx, array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cs_off')
UNION ALL SELECT 'cs_seven', array_agg(DISTINCT format ORDER BY format) FROM sift._test_col_formats('cs_seven')
UNION ALL SELECT 'cs_six', array_agg(DISTINCT format ORDER BY format) FROM sift._test_col_formats('cs_six')
UNION ALL SELECT 'cs_on', array_agg(DISTINCT format ORDER BY format) FROM sift._test_col_formats('cs_on')
UNION ALL SELECT 'cs_nocube', array_agg(DISTINCT format ORDER BY format) FROM sift._test_col_formats('cs_nocube')
UNION ALL SELECT 'cs_mm', array_agg(DISTINCT format ORDER BY format) FROM sift._test_col_formats('cs_mm') ORDER BY 1;
SELECT sift.col_verify('cs_off', true), sift.col_verify('cs_seven', true), sift.col_verify('cs_six', true), sift.col_verify('cs_on', true),
       sift.col_verify('cs_nocube', true), sift.col_verify('cs_mm', true);

-- the block costs bytes only where it is asked for
SELECT (SELECT preagg_bytes FROM sift.col_index_info('cs_on')) > (SELECT preagg_bytes FROM sift.col_index_info('cs_off')) AS block_costs_bytes,
       (SELECT preagg_bytes FROM sift.col_index_info('cs_seven')) = (SELECT preagg_bytes FROM sift.col_index_info('cs_off')) AS no_block_below_minor_8;

-- every byte of the block, read as itself with one bit flipped (two flips a byte), through a fresh open of the segment and the check of sift.col_verify:
-- all of them refused
SELECT supers > 0 AS has_blocks, damaged > 0 AS damages_made, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('cs_on');
SELECT supers > 0 AS has_blocks, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('cs_nocube');
SELECT supers > 0 AS has_blocks, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('cs_mm');
SELECT supers AS segments_with_blocks, damaged AS damages FROM sift._test_col_super_damage('cs_off');

-- the option taken later: the segments written before keep their minor, the ones after are minor 12, a merge writes one segment of minor 12; switched off
-- again, the next merge writes one without
INSERT INTO cs SELECT g, g % 6, 'g' || (g % 30), g, g % 9, g / 7.0, g / 3.0, date '2025-01-01' + g / 100, repeat('p', 1500) FROM generate_series(9001, 11000) g;
SELECT sift.col_promote('cs_off') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cs_off');
ALTER INDEX cs_off SET (superunit = 3);
INSERT INTO cs SELECT g, g % 6, 'g' || (g % 30), g, g % 9, g / 7.0, g / 3.0, date '2025-01-01' + g / 100, repeat('p', 1500) FROM generate_series(11001, 13000) g;
SELECT sift.col_promote('cs_off') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cs_off');
SELECT sift.col_verify('cs_off', true);
SELECT supers > 0 AS has_blocks, refused = damaged AS all_refused FROM sift._test_col_super_damage('cs_off');
SELECT sift.col_merge('cs_off', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('cs_off');
SELECT supers = 1 AS one_block, refused = damaged AS all_refused FROM sift._test_col_super_damage('cs_off');
SELECT sift.col_verify('cs_off', true);
ALTER INDEX cs_off SET (superunit = 0);
INSERT INTO cs SELECT g, g % 6, 'g' || (g % 30), g, g % 9, g / 7.0, g / 3.0, date '2025-01-01' + g / 100, repeat('p', 1500) FROM generate_series(13001, 15000) g;
SELECT sift.col_promote('cs_off') >= 0 AS promoted;
SELECT sift.col_merge('cs_off', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cs_off');
SELECT supers AS segments_with_blocks FROM sift._test_col_super_damage('cs_off');
SELECT sift.col_verify('cs_off', true);

-- deletes and VACUUM, then a merge: the summary of the segment the merge writes is of the rows that are left
DELETE FROM cs WHERE id % 3 = 0;
VACUUM (freeze, index_cleanup on, disable_page_skipping) cs;
SELECT sift.col_merge('cs_on', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) cs;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('cs_on');
SELECT supers > 0 AS has_blocks, refused = damaged AS all_refused FROM sift._test_col_super_damage('cs_on');
SELECT sift.col_verify('cs_on', true);

DROP TABLE cs;
DROP EXTENSION pg_sift_test CASCADE;
DROP EXTENSION pg_sift;
