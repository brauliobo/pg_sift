-- The answers of the projections that Spec 50 adds are the heap's: partial projections (WHERE, IS NULL), measures with a FILTER (baked, and tested on the group columns), count(DISTINCT)
-- exact under deletes and across parts, and date_trunc over a date with bounds that fall on the edge of a bucket and bounds that do not.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
\i test/proj_helpers.sql

CREATE TABLE a (id bigint, tenant int, kind text, qty bigint, day date, at timestamptz, ok boolean);
CREATE INDEX a_idx ON a USING sift_col (id, tenant, kind, qty, day, at, ok) WITH (key_order = 'id');
INSERT INTO a SELECT i, i % 6, (ARRAY['x', 'y', 'z', NULL])[1 + i % 4], (i * 7) % 90, DATE '2025-06-01' + (i % 400), TIMESTAMPTZ '2025-06-01 00:00:00+00' + (i % 400) * interval '1 day' + (i % 24) * interval '1 hour', i % 3 = 0
	FROM generate_series(1, 24000) i;

SELECT sift.create_projection('a_idx', 'a_live', 'GROUP BY tenant MEASURES count(*), sum(qty), count(DISTINCT kind) WHERE qty >= 45');
SELECT sift.create_projection('a_idx', 'a_filter', 'GROUP BY tenant, kind MEASURES count(*), count(qty) FILTER (WHERE qty > 40), sum(qty) FILTER (WHERE qty > 40)');
SELECT sift.create_projection('a_idx', 'a_dist', 'GROUP BY tenant MEASURES count(DISTINCT qty)');
SELECT sift.create_projection('a_idx', 'a_month', 'GROUP BY date_trunc(''month'', day), tenant MEASURES count(*), sum(qty)');
SELECT sift.create_projection('a_idx', 'a_null', 'GROUP BY tenant MEASURES count(*) WHERE kind IS NULL');
VACUUM a;

CREATE FUNCTION a_parity() RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text;
BEGIN
	FOREACH q IN ARRAY ARRAY[
		'SELECT tenant, count(*), sum(qty) FROM a WHERE qty >= 45 GROUP BY tenant',
		'SELECT tenant, count(DISTINCT kind) FROM a WHERE qty >= 45 GROUP BY tenant',
		'SELECT tenant, count(*) FILTER (WHERE qty > 40), sum(qty) FILTER (WHERE qty > 40) FROM a GROUP BY tenant',
		'SELECT tenant, kind, count(qty) FILTER (WHERE qty > 40) FROM a GROUP BY tenant, kind',
		'SELECT tenant, count(*) FILTER (WHERE kind = ''x'') FROM a GROUP BY tenant',
		'SELECT tenant, count(DISTINCT qty) FROM a GROUP BY tenant',
		'SELECT tenant, count(DISTINCT qty) FILTER (WHERE tenant < 3) FROM a GROUP BY tenant',
		'SELECT date_trunc(''month'', day) m, tenant, count(*), sum(qty) FROM a GROUP BY m, tenant',
		'SELECT date_trunc(''month'', day) m, count(*) FROM a WHERE day >= DATE ''2025-09-01'' AND day < DATE ''2026-03-01'' GROUP BY m',
		'SELECT date_trunc(''month'', day) m, count(*) FROM a WHERE day >= DATE ''2025-09-15'' GROUP BY m',
		'SELECT date_trunc(''year'', day) y, sum(qty) FROM a GROUP BY y',
		'SELECT date_trunc(''day'', at) d, count(*) FROM a GROUP BY d',
		'SELECT tenant, count(*) FROM a WHERE kind IS NULL GROUP BY tenant',
		'SELECT tenant, count(*) FROM a WHERE kind IS NULL AND tenant > 2 GROUP BY tenant'] LOOP
		PERFORM p_check(q, p_same(q) = 'same');
	END LOOP;
END $$;

SELECT a_parity();
SELECT p_check('partial agg served', p_uses('SELECT tenant, count(*), sum(qty) FROM a WHERE qty >= 45 GROUP BY tenant', 'a_live'));
SELECT p_check('distinct served', p_uses('SELECT tenant, count(DISTINCT qty) FROM a GROUP BY tenant', 'a_dist'));
SELECT p_check('timestamptz not served', NOT p_uses('SELECT date_trunc(''day'', at) d, count(*) FROM a GROUP BY d', 'a_month'));

-- Deletes that kill the last row of a value leave it uncounted, and a value held by two parts is counted once
DELETE FROM a WHERE tenant = 2 AND qty = 14;
SELECT a_parity();
INSERT INTO a SELECT 30000 + i, i % 6, 'x', (i * 7) % 90, DATE '2026-07-01' + i, now(), true FROM generate_series(1, 300) i;
SELECT a_parity();
VACUUM a;
DELETE FROM a WHERE id % 13 = 0;
SELECT a_parity();
UPDATE a SET qty = (qty + 1) % 90 WHERE id % 17 = 0;
SELECT a_parity();
VACUUM a;
SELECT a_parity();
SELECT p_check('distinct served after churn', p_uses('SELECT tenant, count(DISTINCT qty) FROM a GROUP BY tenant', 'a_dist'));
SELECT count(*) FROM sift.check('a_idx', false, 100);

DROP TABLE a;
DROP FUNCTION a_parity();
DROP FUNCTION p_check(text, boolean), p_rows(text, boolean), p_rows_ordered(text, boolean), p_same(text), p_same_order(text, text), p_uses(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
