-- The size that CREATE INDEX and sift.key_cut_decision predict for an automatic key_cut is the size the build makes, also when the rows of a key are sorted
-- by a date and a key of 8,192 rows or more is cut into units of 4,096: a prediction that under-reports growth is a bug. Three shapes, each built with key_cut = 'off' and by default:
-- a narrow index (the date as second key is predicted dearer than the key alone, so the key is cut alone), an index with an id leaf and a wide text leaf (the date costs little: it is
-- the second key), and keys below 8,192 rows (the date changes no unit). The predicted size and the real one agree within 25%.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE gn AS SELECT 1 + (g::bigint * 7919 % 6)::int AS dim_b_id, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int AS day, (g % 100)::float8 AS v
	FROM generate_series(1, 600000) g;
CREATE TABLE gw AS SELECT (g::bigint * 48271 % 2147483647)::int AS id, 1 + (g::bigint * 7919 % 6)::int AS dim_b_id, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int AS day,
	md5(g::text) || md5((g * 7)::text) AS note FROM generate_series(1, 300000) g;
CREATE TABLE gs AS SELECT 1 + (g::bigint * 7919 % 150)::int AS dim_b_id, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int AS day, (g % 100)::float8 AS v
	FROM generate_series(1, 600000) g;
VACUUM (FREEZE, ANALYZE) gn, gw, gs;

CREATE INDEX gn_off ON gn USING sift_col (dim_b_id, day, v) WITH (key_cut = 'off', order_array_columns = 'off', key_columns = 'off');
CREATE INDEX gn_auto ON gn USING sift_col (dim_b_id, day, v) WITH (order_array_columns = 'off', key_columns = 'off');
CREATE INDEX gw_off ON gw USING sift_col (dim_b_id, day, id, note) WITH (key_cut = 'off', order_array_columns = 'off', key_columns = 'off');
CREATE INDEX gw_auto ON gw USING sift_col (dim_b_id, day, id, note) WITH (order_array_columns = 'off', key_columns = 'off');
CREATE INDEX gs_off ON gs USING sift_col (dim_b_id, day, v) WITH (key_cut = 'off', order_array_columns = 'off', key_columns = 'off');
CREATE INDEX gs_auto ON gs USING sift_col (dim_b_id, day, v) WITH (order_array_columns = 'off', key_columns = 'off');

CREATE FUNCTION growth_check(t text) RETURNS TABLE (shape text, key_cut text, within_25pct bool) LANGUAGE plpgsql AS $$
DECLARE
	off_bytes bigint := pg_relation_size((t || '_off')::regclass);
	auto_bytes bigint := pg_relation_size((t || '_auto')::regclass);
	predicted float8 := (SELECT d.growth_pct FROM sift.key_cut_decision((t || '_auto')::regclass) d);
BEGIN
	RETURN QUERY SELECT t, (SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = (t || '_auto')::regclass AND o ~ '^key_cut'),
		abs((1 + predicted / 100) / (auto_bytes::float8 / off_bytes) - 1) <= 0.25;
END $$;
SELECT * FROM growth_check('gn');
SELECT * FROM growth_check('gw');
SELECT * FROM growth_check('gs');
DROP FUNCTION growth_check(text);
DROP TABLE gn, gw, gs;
DROP EXTENSION pg_sift;
