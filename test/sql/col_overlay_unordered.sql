-- The overlays of an index with arrays of the build's choice ('auto: ..') are written without the global-order arrays as well while all of them together hold at most a tenth of the rows of the index (they span the rows of the whole index, as the point directories do): the
-- share of the values the arrays cover stays above what the order planner asks, the top-N and the groups answer from the order of the large segments and the rows of the overlay. A promotion
-- that takes them over a tenth keeps its arrays. Answers are the same all along, with dead rows in the overlay and after the merge, which writes the arrays again.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
CREATE TABLE ou (id int, g int, ts timestamp, amount int) WITH (autovacuum_enabled = off);
INSERT INTO ou SELECT i, i % 50, TIMESTAMP '2026-01-01' + (i * 7919 % 10007) * INTERVAL '1 minute', i % 100 FROM generate_series(1, 20000) i;
CREATE INDEX ou_ix ON ou USING sift_col (id, g, ts, amount) WITH (key_cut = 'off', order_columns = 'ts', order_array_columns = 'auto: ts');
VACUUM (FREEZE, ANALYZE) ou;
SELECT count(*) AS entries FROM sift._test_col_gorder('ou_ix', 2) \gset

CREATE FUNCTION ou_answers() RETURNS text LANGUAGE sql AS $$
  SELECT md5(concat_ws('|',
    (SELECT string_agg(id::text, ',' ORDER BY ts DESC, id) FROM (SELECT id, ts FROM ou ORDER BY ts DESC, id LIMIT 30) x),
    (SELECT string_agg(id::text, ',' ORDER BY ts, id) FROM (SELECT id, ts FROM ou WHERE g = 7 ORDER BY ts, id LIMIT 30) x),
    (SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT g, count(*), sum(amount) FROM ou WHERE g BETWEEN 5 AND 20 GROUP BY 1) x))) $$;

-- 300 rows of 20,300: an overlay under a tenth: no array entries of its own, and a lean format
INSERT INTO ou SELECT 20000 + i, i % 50, TIMESTAMP '2026-03-01' + i * INTERVAL '1 minute', i % 100 FROM generate_series(1, 300) i;
SELECT sift.col_promote('ou_ix') >= 0 AS promoted;
SELECT count(*) AS overlays FROM sift.col_segments('ou_ix') WHERE kind = 'immutable' AND rows < 1000;
SELECT count(*) = :entries AS overlay_has_no_array FROM sift._test_col_gorder('ou_ix', 2);
SET sift.enable_agg = off;
SELECT ou_answers() AS native \gset
RESET sift.enable_agg;
SELECT ou_answers() = :'native' AS same_answers_overlay;

-- dead rows in the overlay and in the large segment
DELETE FROM ou WHERE id % 7 = 0 AND id > 19000;
VACUUM ou;
SET sift.enable_agg = off;
SELECT ou_answers() AS native_dead \gset
RESET sift.enable_agg;
SELECT ou_answers() = :'native_dead' AS same_answers_dead;

-- 2,500 rows more: the overlays come to over a tenth of the index, the promotion keeps its arrays
INSERT INTO ou SELECT 30000 + i, i % 50, TIMESTAMP '2026-04-01' + i * INTERVAL '1 minute', i % 100 FROM generate_series(1, 2500) i;
SELECT sift.col_promote('ou_ix') >= 0 AS promoted_large;
SELECT count(*) > :entries + 2400 AS larger_overlay_has_array FROM sift._test_col_gorder('ou_ix', 2);
SET sift.enable_agg = off;
SELECT ou_answers() AS native_large \gset
RESET sift.enable_agg;
SELECT ou_answers() = :'native_large' AS same_answers_large;

-- the merge into one segment writes the arrays of every row
SELECT sift.col_merge('ou_ix', true) > 0 AS merged;
SELECT count(*) AS segments FROM sift.col_segments('ou_ix') WHERE kind = 'immutable';
SELECT count(*) = (SELECT count(*) FROM ou) AS array_of_every_row FROM sift._test_col_gorder('ou_ix', 2);
SELECT ou_answers() = :'native_large' AS same_answers_merged;
SELECT sift.col_verify('ou_ix', true);
-- arrays the index names (order_array_columns = 'ts', not the choice of the build) stay in every overlay: a group walk reads those of every segment
CREATE TABLE oe (id int, ts timestamp);
INSERT INTO oe SELECT i, TIMESTAMP '2026-01-01' + (i * 7919 % 10007) * INTERVAL '1 minute' FROM generate_series(1, 20000) i;
CREATE INDEX oe_ix ON oe USING sift_col (id, ts) WITH (key_cut = 'off', order_columns = 'ts', order_array_columns = 'ts');
VACUUM (FREEZE, ANALYZE) oe;
SELECT count(*) AS entries FROM sift._test_col_gorder('oe_ix', 1) \gset
INSERT INTO oe SELECT 20000 + i, TIMESTAMP '2026-03-01' + i * INTERVAL '1 minute' FROM generate_series(1, 300) i;
SELECT sift.col_promote('oe_ix') >= 0 AS promoted_named;
SELECT count(*) = :entries + 300 AS named_array_in_overlay FROM sift._test_col_gorder('oe_ix', 1);
DROP TABLE oe;
DROP FUNCTION ou_answers();
DROP TABLE ou;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
