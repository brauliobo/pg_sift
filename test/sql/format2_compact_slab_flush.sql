-- A micro-slab of one-row transactions: each row is a compact batch (a 16-byte header and the row as varints), and a slab that reaches 4,096 rows is sealed and flushed into a part.
-- The flush reads every compact batch back, so a batch that is not whole ("ends inside a number") or a sealed chain that takes one more batch shows here as an error or as a wrong count.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
CREATE TABLE slab_t (id bigint PRIMARY KEY, g int NOT NULL, v numeric(12,2) NOT NULL, s text, ts timestamptz NOT NULL, j jsonb);
INSERT INTO slab_t SELECT g, g % 10, g / 4.0, 's' || g, TIMESTAMPTZ '2024-01-01' + g * interval '1 minute', jsonb_build_object('k', g % 3) FROM generate_series(1, 2000) g;
CREATE INDEX slab_idx ON slab_t USING sift_col (id, g, v, s, ts, j) WITH (track_changes = 'auto');
CREATE SEQUENCE slab_seq START 100000;
DO $$
BEGIN
	FOR i IN 1 .. 4300 LOOP
		INSERT INTO slab_t VALUES (nextval('slab_seq'), i % 10, i / 8.0, CASE WHEN i % 11 = 0 THEN NULL ELSE 'row ' || i END, now(), CASE WHEN i % 5 = 0 THEN NULL ELSE jsonb_build_object('k', i % 3, 'x', i) END);
		COMMIT;
	END LOOP;
END $$;
DO $$
BEGIN
	FOR i IN 1 .. 1500 LOOP
		UPDATE slab_t SET v = v + 1, s = 'upd ' || i WHERE id = i;
		COMMIT;
		IF i % 3 = 0 THEN
			DELETE FROM slab_t WHERE id = 100000 + i;
			COMMIT;
		END IF;
	END LOOP;
END $$;
SELECT count(*) AS check_issues FROM sift.check('slab_idx');
CREATE TEMP TABLE fast AS SELECT count(*) AS n, sum(v) AS sv, sum(g) AS sg, count(s) AS ns, count(j) AS nj, sum(id) AS si FROM slab_t;
SET sift.enable_agg = off;
SET sift.enable_scan = off;
SET sift.enable_col_scan = off;
CREATE TEMP TABLE heap AS SELECT count(*) AS n, sum(v) AS sv, sum(g) AS sg, count(s) AS ns, count(j) AS nj, sum(id) AS si FROM slab_t;
RESET sift.enable_agg;
RESET sift.enable_scan;
RESET sift.enable_col_scan;
SELECT (SELECT row(f.*) FROM fast f) IS NOT DISTINCT FROM (SELECT row(h.*) FROM heap h) AS index_equals_heap;
DROP TABLE slab_t CASCADE;
DROP SEQUENCE slab_seq;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
