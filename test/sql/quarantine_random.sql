-- Random damage of the pages of an index (seed 0.42; 60 rounds of one 4-byte poke each, for sift_col and for the bm25 sift index). The counts at the end are the assertion: no statement
-- raises an error other than the damage error, a statement that failed with the damage error gives the rows of the heap when it runs again (planned without the quarantined index), and
-- a result that differs from the heap's is one that sift.check(index) reports ('differs, silent' does not appear). It does not assert that sift.check(index), which checks structure only,
-- reports every damage that changes a result: a changed payload byte inside a still well formed bm25 posting list decodes to other rows and only sift.check(index, heapallindexed => true)
-- finds it (docs/OPERATIONS.md); other seeds give 'differs, silent' for it, so the seed belongs to the expectation
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = error;
SET sift.on_corruption = quarantine;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE t (id int PRIMARY KEY, v int, g int, s text, body text);
INSERT INTO t SELECT i, i % 13, i % 5, 'w' || (i % 17), 'alpha beta ' || (i % 10) || ' gamma' || (i % 3) FROM generate_series(1, 6000) i;
CREATE TABLE queries (kind text, n int, sql text);
INSERT INTO queries VALUES
 ('col', 1, 'SELECT count(*) FROM t WHERE v = 3'),
 ('col', 2, 'SELECT g, count(*), sum(v) FROM t GROUP BY g ORDER BY g'),
 ('col', 3, 'SELECT id FROM t WHERE v = 5 AND g = 2 ORDER BY id LIMIT 20'),
 ('col', 4, 'SELECT count(*) FROM t WHERE s = ''w4'' AND id > 3000'),
 ('col', 5, 'SELECT sum(id) FROM t WHERE id BETWEEN 100 AND 4000'),
 ('bm25', 1, 'SELECT count(*) FROM t WHERE body @@> ''alpha 3'''),
 ('bm25', 2, 'SELECT id FROM t WHERE body @@> ''alpha 3 gamma1'' ORDER BY id LIMIT 30'),
 ('bm25', 3, 'SELECT count(*) FROM t WHERE body @@> ''gamma2'' AND v = 4');
CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	EXECUTE 'SELECT coalesce(string_agg(x::text, '';'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO r;
	RETURN r;
END $$;
CREATE TABLE heap_answers AS SELECT kind, n, answer(sql) AS a FROM queries;
CREATE TABLE outcome (kind text, round int, n int, state text);

CREATE FUNCTION damage_round(kind text, round int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	idx text := 't_' || kind;
	blocks int;
	q record;
	got text;
	again text;
BEGIN
	EXECUTE format('DROP INDEX IF EXISTS %I', idx);
	IF kind = 'col' THEN EXECUTE format('CREATE INDEX %I ON t USING sift_col (id, v, g)', idx);
	ELSE EXECUTE format('CREATE INDEX %I ON t USING sift (body)', idx);
	END IF;
	ANALYZE t;
	blocks := pg_relation_size(idx::regclass) / 8192;
	PERFORM sift._test_poke(idx, (1 + floor(random() * (blocks - 1)))::int,
		(CASE floor(random() * 3) WHEN 0 THEN floor(random() * 24) WHEN 1 THEN 8176 + floor(random() * 13) ELSE floor(random() * 8189) END)::int,
		floor(random() * 4294967296)::int8, 4);
	FOR q IN SELECT * FROM queries WHERE queries.kind = damage_round.kind ORDER BY n LOOP
		BEGIN
			got := answer(q.sql);
			INSERT INTO outcome VALUES (kind, round, q.n, CASE WHEN got = (SELECT a FROM heap_answers h WHERE h.kind = q.kind AND h.n = q.n) THEN 'same'
				WHEN (SELECT count(*) FROM sift.check(idx::regclass)) > 0 THEN 'differs, sift.check reports it' ELSE 'differs, silent' END);
		EXCEPTION WHEN SQLSTATE 'XX002' OR SQLSTATE 'XX001' THEN
			again := answer(q.sql);
			INSERT INTO outcome VALUES (kind, round, q.n, CASE WHEN again = (SELECT a FROM heap_answers h WHERE h.kind = q.kind AND h.n = q.n) THEN 'failed once, then same' ELSE 'failed once, then differs' END);
		WHEN OTHERS THEN
			INSERT INTO outcome VALUES (kind, round, q.n, 'other error ' || SQLSTATE);
		END;
	END LOOP;
END $$;

SELECT setseed(0.42);
SELECT damage_round(k, r) FROM generate_series(1, 60) r, unnest(ARRAY['col', 'bm25']) k;
SELECT kind, state, count(*) FROM outcome GROUP BY 1, 2 ORDER BY 1, 2;

DROP TABLE t, queries, heap_answers, outcome;
DROP FUNCTION answer(text), damage_round(text, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
