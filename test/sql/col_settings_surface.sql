-- The two settings that the regression tests of the release library set are ordinary settings, not test ones (scripts/check-test-settings.sh keeps every
-- other sift.test_ setting out of the release library); the settings that only the background workers read cannot be set by a session; sift_table has no
-- setting for its per-table key_order.
CREATE EXTENSION pg_sift;

SELECT name, context, short_desc LIKE 'Testing%' OR short_desc LIKE '%(test only)' AS says_test
FROM pg_settings WHERE name IN ('sift.col_unit_rows', 'sift.search_chunk_sbs') ORDER BY name;
SELECT name, context FROM pg_settings
WHERE name IN ('sift.table_auto_defrag_delay', 'sift.table_auto_defrag_max_active', 'sift.table_auto_defrag_wal_rate') ORDER BY name;
SELECT count(*) AS table_key_order_settings FROM pg_settings WHERE name = 'sift.table_key_order';

SET sift.col_unit_rows = 512;
SET sift.search_chunk_sbs = 8;
SELECT current_setting('sift.col_unit_rows'), current_setting('sift.search_chunk_sbs');
SET sift.table_auto_defrag_delay = 5;
SET sift.table_key_order = 'k';

DROP EXTENSION pg_sift;
