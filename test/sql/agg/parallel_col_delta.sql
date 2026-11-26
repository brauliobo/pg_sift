-- The parallel SiftAggV6 reads the rows a column delta patched (an UPDATE of a text or document leaf appends a record to the part instead of ending the row): the morsel scan applies the chain of
-- each partition as the serial scan does, so no updated row is dropped and every patched value is the new one. Every shape runs with four workers on the index and on the heap twin, and the
-- answers are compared; the first word of a plan row says the node is the parallel SiftAggV6. Every check prints t.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET sift.test_create_format1 = off;
SET max_parallel_workers = 8;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;

CREATE TABLE entity_parents (entity_id int NOT NULL, parent_id int NOT NULL, PRIMARY KEY (entity_id, parent_id)) WITH (autovacuum_enabled = false);
INSERT INTO entity_parents SELECT e, 1 + e % 7 FROM generate_series(1, 200) e;
CREATE TABLE ev (id int PRIMARY KEY, entity_id int NOT NULL, day date NOT NULL, qty int, amount numeric(12,2), note text, doc jsonb) WITH (autovacuum_enabled = false);
CREATE TABLE evh (LIKE ev INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT g, 1 + g % 200, date '2030-01-01' + g % 90, g % 13, (g % 1000) / 8.0, 'note ' || g, jsonb_build_object('flag', CASE WHEN g % 3 = 0 THEN 'a' ELSE 'b' END, 'n', g)
FROM generate_series(1, 36000) g;
INSERT INTO evh SELECT * FROM ev;
SET maintenance_work_mem = '64MB';
CREATE INDEX ev_sift ON ev USING sift_col (id, entity_id, day, qty, amount, note, doc)
	WITH (key_order = 'entity_id, day', paths = 'doc.flag text', scope = 'parent_id: entity_id -> entity_parents(entity_id, parent_id)');
ANALYZE ev;
ANALYZE evh;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(md5(string_agg(r::text, E''\n'' ORDER BY r::text)), '''') FROM (%s) r', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION parallel_node(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; gather boolean := false; node boolean := false;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		gather := gather OR line LIKE '%Gather%';
		node := node OR line LIKE '%Parallel Custom Scan (SiftAggV6)%';
	END LOOP;
	RETURN gather AND node;
END $$;
CREATE FUNCTION dead() RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce(sum(dead_rows), 0)::bigint FROM sift.parts('ev_sift') $$;
CREATE FUNCTION shapes() RETURNS SETOF text LANGUAGE sql AS $$ SELECT unnest(ARRAY[
	'SELECT entity_id, count(*), sum(qty), count(DISTINCT note) FROM @T GROUP BY entity_id',
	'SELECT count(*), count(DISTINCT note) FROM @T',
	'SELECT count(*), sum(qty), count(DISTINCT qty) FROM @T WHERE note <> ''zzz''',
	'SELECT day, count(*), sum(amount) FROM @T WHERE day >= date ''2030-01-10'' GROUP BY day',
	'SELECT note, count(*) FROM @T GROUP BY note',
	'SELECT doc->>''flag'', count(*), sum(qty) FROM @T GROUP BY 1']) $$;
CREATE FUNCTION check_all(label text) RETURNS TABLE (step text, same boolean, parallel boolean, q text) LANGUAGE plpgsql AS $$
DECLARE s text;
BEGIN
	FOR s IN SELECT * FROM shapes() LOOP
		step := label;
		q := substr(s, 1, 90);
		SET LOCAL max_parallel_workers_per_gather = 4;
		SET LOCAL sift.enable_agg = on;
		parallel := parallel_node(replace(s, '@T', 'ev'));
		same := answer(replace(s, '@T', 'ev')) = answer(replace(s, '@T', 'evh'));
		RETURN NEXT;
	END LOOP;
END $$;

SELECT * FROM check_all('built');

UPDATE ev SET note = 'upd ' || id WHERE id % 47 = 0;
UPDATE evh SET note = 'upd ' || id WHERE id % 47 = 0;
-- an integer leaf is not patched by a column delta: the rows whose qty changes are replaced
UPDATE ev SET qty = qty + 1 WHERE id % 47 = 0 AND id % 2 = 0;
UPDATE evh SET qty = qty + 1 WHERE id % 47 = 0 AND id % 2 = 0;
SELECT dead() < 765 AS some_rows_patched_in_place, dead() > 0 AS some_rows_replaced;
SELECT * FROM check_all('patched');

UPDATE ev SET note = note || ' again', doc = doc || '{"flag": "z"}' WHERE id % 67 = 0;
UPDATE evh SET note = note || ' again', doc = doc || '{"flag": "z"}' WHERE id % 67 = 0;
SELECT * FROM check_all('patched twice');

DELETE FROM ev WHERE id % 13 = 0;
DELETE FROM evh WHERE id % 13 = 0;
SELECT * FROM check_all('deleted');

BEGIN;
UPDATE ev SET note = 'own ' || id WHERE id % 41 = 0;
UPDATE evh SET note = 'own ' || id WHERE id % 41 = 0;
SELECT * FROM check_all('own transaction');
ROLLBACK;
SELECT * FROM check_all('rolled back');

VACUUM ev;
SELECT * FROM check_all('vacuumed');
DROP TABLE entity_parents, ev, evh;
DROP FUNCTION answer(text), check_all(text), dead(), parallel_node(text), shapes();
DROP EXTENSION pg_sift;
