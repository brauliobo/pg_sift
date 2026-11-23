-- Format 2.0 sift_table column deltas of narrow tables and of columns a BRIN index summarises: an UPDATE that changes no column of a B-tree index keeps the row's ctid whatever share of the
-- columns it changes (up to 16), and one that changes a BRIN column keeps it too and tells the BRIN index of the new value (TU_Summarizing). Every check raises on failure.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
CREATE EXTENSION IF NOT EXISTS pageinspect;
SET sift.test_create_format1 = off;
SET check_function_bodies = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE SCHEMA f2cds;
SET search_path = f2cds, public;

CREATE FUNCTION chk(name text, got anyelement, want anyelement) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF got IS DISTINCT FROM want THEN RAISE EXCEPTION 'FAILED %: got %, want %', name, got, want; END IF;
	RETURN name || ': ok';
END $$;

-- 1. a narrow table: two of its three columns change, neither is indexed
CREATE TABLE n (id int PRIMARY KEY, a int, b int) USING sift_table;
INSERT INTO n SELECT i, i, i FROM generate_series(1, 5000) i;
SELECT ctid::text AS c1 FROM n WHERE id = 7 \gset
WITH u AS (UPDATE n SET a = -1, b = -2 WHERE id = 7 RETURNING ctid::text AS c) SELECT chk('two of three columns keep the row', c, :'c1') FROM u;
SELECT chk('and the values are the new ones', (a, b)::text, '(-1,-2)') FROM n WHERE id = 7;
SELECT chk('a bulk update of both keeps every row', (SELECT count(*)::int FROM n WHERE a = -3 OR b = -3), 0);
CREATE TABLE naddr AS SELECT id, ctid::text AS c FROM n;
UPDATE n SET a = -3, b = -3 WHERE id <= 3000;
SELECT chk('no row of the bulk update moved', (SELECT count(*)::int FROM n JOIN naddr USING (id) WHERE n.ctid::text <> naddr.c), 0);
SELECT chk('the bulk update is seen', (SELECT count(*)::int FROM n WHERE a = -3 AND b = -3), 3000);

-- 2. a BRIN index on a column: the update keeps the row and the index finds the new value
CREATE TABLE s (id int PRIMARY KEY, brin_col int, other int, note text) USING sift_table;
INSERT INTO s SELECT i, i, i % 10, 'n' || i FROM generate_series(1, 20000) i;
CREATE INDEX s_brin ON s USING brin (brin_col) WITH (pages_per_range = 1);
CREATE TABLE saddr AS SELECT id, ctid::text AS c FROM s;
UPDATE s SET brin_col = 1000000 + id WHERE id % 1000 = 0;
SELECT chk('the BRIN column was patched in place', (SELECT count(*)::int FROM s JOIN saddr USING (id) WHERE s.ctid::text <> saddr.c), 0);
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT chk('the BRIN ranges of the patched rows were told of the new values', (SELECT count(*)::int FROM generate_series(2, pg_relation_size('s_brin') / 8192 - 1) n,
		   brin_page_items(get_raw_page('s_brin', n::int), 's_brin') WHERE value ~ '[0-9]{7}'), 20);
SELECT chk('the BRIN index finds the new values', (SELECT count(*)::int FROM s WHERE brin_col > 1000000), 20);
CREATE FUNCTION plan_uses(q text, what text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP IF l LIKE '%' || what || '%' THEN RETURN true; END IF; END LOOP;
	RETURN false;
END $$;
SELECT chk('the BRIN index plan was used', plan_uses('SELECT * FROM s WHERE brin_col > 1000000', 'Bitmap Index Scan on s_brin'), true);
RESET enable_seqscan;
RESET enable_indexscan;
SELECT chk('the old values are gone', (SELECT count(*)::int FROM s WHERE brin_col = 1000), 0);

DROP SCHEMA f2cds CASCADE;
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
