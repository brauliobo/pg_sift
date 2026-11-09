-- Random writes to a dimension (updates of its columns, inserts of keys the facts name, deletes, a rolled back batch, a vacuum now and then, a batch in the same transaction as the
-- reader) against the shapes that read it through the dimension map: after every batch each shape gives the rows of the normal plan, for a dimension whose index keeps a journal of its
-- changed keys (a child index is connected to it) and for one that has none, and both kinds of map are made and shared along the way.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE dc (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false);
CREATE TABLE dl (LIKE dc INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO dc SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 1999) i;
INSERT INTO dl SELECT * FROM dc;
CREATE INDEX dc_col ON dc USING sift_col (k, a, b, flag);
CREATE INDEX dl_col ON dl USING sift_col (k, a, b, flag);
CREATE TABLE ec (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ec SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE i % 2100 END, i % 11 FROM generate_series(1, 12000) i;
CREATE TABLE el (LIKE ec INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO el SELECT * FROM ec;
CREATE INDEX ec_col ON ec USING sift_col (id, k, v) WITH (connect = 'k -> dc(k)');
CREATE INDEX el_col ON el USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) ec, el, dc, dl;

CREATE SEQUENCE fz_seq START 10000;
CREATE TABLE fuzz_log (kind text, shared int, built int, differences int);

CREATE FUNCTION fuzz_check(ev text, dim text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	q text;
	l text;
	shared int := 0;
	built int := 0;
	diffs int;
BEGIN
	FOREACH q IN ARRAY ARRAY[
		format('SELECT d.a, count(*) AS n, sum(e.v) AS s FROM %s e JOIN %s d ON d.k = e.k GROUP BY 1', ev, dim),
		format('SELECT d.a, d.b, count(*) AS n FROM %s e LEFT JOIN %s d ON d.k = e.k GROUP BY 1, 2', ev, dim),
		format('SELECT e.id %% 4 AS g, count(*) FILTER (WHERE d.a = 2) AS c1, count(*) FILTER (WHERE d.flag) AS c2, count(DISTINCT e.k) FILTER (WHERE d.b = 1) AS c3 FROM %s e JOIN %s d ON d.k = e.k GROUP BY 1', ev, dim)]
	LOOP
		PERFORM set_config('sift.enable_agg', 'on', false);
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || q LOOP
			IF l ~ '(\d+) maps shared' THEN shared := shared + substring(l from '(\d+) maps shared')::int; END IF;
			IF l ~ '(\d+) maps built' THEN built := built + substring(l from '(\d+) maps built')::int; END IF;
		END LOOP;
		EXECUTE 'CREATE TEMP TABLE fz_on AS ' || q;
		PERFORM set_config('sift.enable_agg', 'off', false);
		EXECUTE 'CREATE TEMP TABLE fz_off AS ' || q;
		PERFORM set_config('sift.enable_agg', 'on', false);
		SELECT count(*) INTO diffs FROM ((TABLE fz_on EXCEPT TABLE fz_off) UNION ALL (TABLE fz_off EXCEPT TABLE fz_on)) d;
		DROP TABLE fz_on, fz_off;
		INSERT INTO fuzz_log VALUES (dim, shared, built, diffs);
		shared := 0; built := 0;
	END LOOP;
END $$;

CREATE FUNCTION fuzz_write(dim text, op int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	IF op = 0 THEN EXECUTE format('UPDATE %s SET a = (random() * 7)::int, flag = NULL WHERE k %% 41 = %s', dim, (random() * 40)::int);
	ELSIF op = 1 THEN EXECUTE format('UPDATE %s SET b = (random() * 3)::int WHERE k BETWEEN %s AND %s', dim, (random() * 1900)::int, (random() * 1900)::int + 30);
	ELSIF op = 2 THEN EXECUTE format('INSERT INTO %s SELECT i, 1, 1, true FROM generate_series(%s, %s) i ON CONFLICT DO NOTHING', dim, 2000 + (random() * 90)::int, 2000 + (random() * 90)::int + 8);
	ELSIF op = 3 THEN EXECUTE format('DELETE FROM %s WHERE k %% 59 = %s', dim, (random() * 58)::int);
	ELSE EXECUTE format('UPDATE %s SET k = nextval(''fz_seq'') WHERE k < 3000 AND k %% 211 = %s', dim, (random() * 210)::int);
	END IF;
END $$;

CREATE PROCEDURE fuzz(seed float8, rounds int) LANGUAGE plpgsql AS $$
DECLARE
	dim text;
	ev text;
BEGIN
	PERFORM setseed(seed);
	FOR r IN 1..rounds LOOP
		FOREACH dim IN ARRAY ARRAY['dc', 'dl'] LOOP
			ev := CASE dim WHEN 'dc' THEN 'ec' ELSE 'el' END;
			PERFORM fuzz_write(dim, (random() * 4)::int);
			CASE (random() * 4)::int
				WHEN 0 THEN
					COMMIT;
				WHEN 1 THEN
					-- a second batch in the same transaction as the readers, then the commit
					PERFORM fuzz_write(dim, (random() * 4)::int);
					PERFORM fuzz_check(ev, dim);
					COMMIT;
				WHEN 2 THEN
					ROLLBACK;
				ELSE
					COMMIT;
			END CASE;
			PERFORM fuzz_check(ev, dim);
			PERFORM fuzz_check(ev, dim);
			COMMIT;
		END LOOP;
	END LOOP;
END $$;

CALL fuzz(0.11, 8);
VACUUM (INDEX_CLEANUP ON) dc, dl;
CALL fuzz(0.22, 8);
CALL fuzz(0.33, 8);
VACUUM (INDEX_CLEANUP ON) dc;
CALL fuzz(0.44, 8);
VACUUM (INDEX_CLEANUP ON) dl;
CALL fuzz(0.55, 8);
CALL fuzz(0.66, 8);
VACUUM (INDEX_CLEANUP ON) dc, dl;
CALL fuzz(0.77, 8);
SELECT kind, count(*) AS checks, sum(differences) AS differences, sum(shared) > 0 AS shared_some, sum(built) > 0 AS built_some FROM fuzz_log GROUP BY kind ORDER BY kind;

DROP PROCEDURE fuzz(float8, int);
DROP FUNCTION fuzz_write(text, int), fuzz_check(text, text);
DROP SEQUENCE fz_seq;
DROP TABLE fuzz_log, ec, el, dc, dl;
DROP EXTENSION pg_sift;
