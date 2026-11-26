-- EXPLAIN (SIFT), PostgreSQL 18 and later: after the plan, the candidate paths that were not used, with the slug and sentence of sift.explain_why, and for the scan
-- of a sift or sift_col index what the index holds (segments, rows in write buffers, dead rows). Nothing is printed without the option. PostgreSQL 17 has no
-- extension options for EXPLAIN: explain_sift_1.out is the same test where each call says so (sift.explain_refusals is the way there).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
-- the option exists once the library is loaded
SELECT sift.version() IS NOT NULL AS loaded;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE d (id int PRIMARY KEY, cat int, body text);
INSERT INTO d SELECT i, i % 5, 'common word' || i % 10 || ' tail' || i FROM generate_series(1, 2000) i;
CREATE INDEX d_body ON d USING sift (body);
CREATE TABLE o (id int PRIMARY KEY, c int NOT NULL, v float8 NOT NULL);
INSERT INTO o SELECT i, i % 50, i FROM generate_series(1, 2000) i;
CREATE INDEX o_col ON o USING sift_col (id, c, v);
CREATE TABLE plain (id int PRIMARY KEY, c int NOT NULL, v float8 NOT NULL);
INSERT INTO plain SELECT * FROM o;
VACUUM (ANALYZE) d, o, plain;

-- the lines of the plan that name a scan or say something of sift; a cost is N
CREATE FUNCTION sift_lines(q text, opts text DEFAULT '') RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (SIFT, COSTS OFF' || opts || ') ' || q LOOP
		IF l ~ '(Custom Scan|Bitmap Index Scan|Sift)' THEN RETURN NEXT regexp_replace(btrim(l), '(costs|plan) [0-9]+\.[0-9]+', '\1 N', 'g'); END IF;
	END LOOP;
EXCEPTION WHEN syntax_error THEN
	RETURN NEXT 'EXPLAIN (SIFT) needs PostgreSQL 18';
END $$;

-- a used index: what it holds
SET sift.agg_force = on;
SELECT * FROM sift_lines('SELECT c, sum(v) FROM o GROUP BY c');
RESET sift.agg_force;
SELECT * FROM sift_lines($$SELECT count(*) FROM d WHERE body @@> 'word3'$$);
SELECT * FROM sift_lines($$SELECT id FROM d WHERE body @@> 'word9' ORDER BY sift.score(ctid) DESC LIMIT 3$$);
BEGIN;
SET LOCAL sift.enable_scan = off;
SET LOCAL sift.enable_scan = off;
SET LOCAL enable_seqscan = off;
SELECT * FROM sift_lines($$SELECT count(*) FROM d WHERE body @@> 'word3'$$);
COMMIT;
-- rows still in write buffers, and dead rows
INSERT INTO d SELECT i, i % 5, 'common word' || i % 10 || ' tail' || i FROM generate_series(2001, 2200) i;
SELECT * FROM sift_lines($$SELECT count(*) FROM d WHERE body @@> 'word3'$$);
DELETE FROM d WHERE id <= 2000 AND id % 20 = 0;
VACUUM d;
SELECT * FROM sift_lines($$SELECT count(*) FROM d WHERE body @@> 'word3'$$);

-- a refused candidate: the slug and the sentence, and the arm
SELECT * FROM sift_lines('SELECT c, sum(v) FROM o WHERE random() < 0.5 GROUP BY c');
SELECT * FROM sift_lines('SELECT c, sum(v) FROM plain GROUP BY c');
-- an aggregate over an ordinary sub-select has no arms to be refused
SELECT * FROM sift_lines('SELECT count(*) FROM (SELECT c, sum(v) FROM plain GROUP BY c) s');
-- the statement that is planned is the explained one: what it calls while it runs is not
CREATE FUNCTION nested() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RETURN (SELECT count(*) FROM (SELECT c, sum(v) FROM plain GROUP BY c) s); END $$;
SELECT * FROM sift_lines('SELECT c, sum(v) FROM o WHERE c < nested() GROUP BY c', ', ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF');

-- the structured formats
CREATE FUNCTION sift_refusals(q text, fmt text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE j text;
BEGIN
	EXECUTE 'EXPLAIN (SIFT, COSTS OFF, FORMAT ' || fmt || ') ' || q INTO j;
	RETURN (j::jsonb) -> 0 -> 'Sift Refusals';
EXCEPTION WHEN syntax_error THEN
	RETURN '"EXPLAIN (SIFT) needs PostgreSQL 18"'::jsonb;
END $$;
SELECT sift_refusals('SELECT c, sum(v) FROM plain GROUP BY c', 'JSON');

-- no option, no lines; the option off is no option
CREATE FUNCTION said_sift(q text, opts text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF' || opts || ') ' || q LOOP
		IF l ~ '^\s*Sift' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
EXCEPTION WHEN syntax_error THEN
	RETURN false;
END $$;
SELECT said_sift('SELECT c, sum(v) FROM plain GROUP BY c', '') AS plain_explain, said_sift('SELECT c, sum(v) FROM plain GROUP BY c', ', SIFT false') AS option_off;

DROP FUNCTION said_sift(text, text), sift_refusals(text, text), nested(), sift_lines(text, text);
DROP TABLE d, o, plain;
DROP EXTENSION pg_sift;
