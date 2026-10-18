-- Effect of rolled-back inserts on scores: documents of a rolled-back transaction stay in the index's
-- document count, document frequencies and average length until VACUUM marks them dead.
-- Run on a scratch database with the extension (scripts/pgtest.sh psql -f bench/sql/dead_docs.sql):
-- :base documents are kept, :dead documents are loaded and rolled back four times, either like the kept ones
-- or all about one term; then the top 10 of some queries is compared before the loads, with them, after VACUUM,
-- and after VACUUM and a forced merge (which drops the dead documents from the statistics).
\set ON_ERROR_STOP on
\if :{?base} \else \set base 50000 \endif
\if :{?dead} \else \set dead 50000 \endif
CREATE EXTENSION IF NOT EXISTS pg_sift;
DROP TABLE IF EXISTS dd, dd_ref;
CREATE TABLE dd (id serial PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
CREATE OR REPLACE FUNCTION dd_doc(skew bool) RETURNS text LANGUAGE sql AS
	$$ SELECT (CASE WHEN skew THEN 'w7 w7 w7 ' ELSE '' END) ||
		(SELECT string_agg('w' || floor(2000 * power(random(), 3))::int, ' ') FROM generate_series(1, 30)) $$;
SELECT setseed(0.42);
INSERT INTO dd (body) SELECT dd_doc(false) FROM generate_series(1, :base);
CREATE INDEX dd_idx ON dd USING sift (body);
VACUUM ANALYZE dd;

CREATE FUNCTION dd_rollbacks(n int, skew bool) RETURNS void LANGUAGE plpgsql AS $f$
BEGIN
	FOR i IN 1..4 LOOP
		BEGIN
			INSERT INTO dd (body) SELECT dd_doc(skew) FROM generate_series(1, n);
			RAISE EXCEPTION 'rollback';
		EXCEPTION WHEN raise_exception THEN NULL;
		END;
	END LOOP;
END $f$;

CREATE TABLE dd_ref (n int, q text, ids int[], top real);
CREATE FUNCTION dd_snapshot(label text) RETURNS TABLE (stage text, query text, same_top10 int, top_score_ratio numeric, ndocs bigint) LANGUAGE plpgsql AS $f$
DECLARE r record; ids int[]; top real; ref dd_ref;
BEGIN
	FOR r IN SELECT q, row_number() OVER () n FROM unnest(ARRAY['w5 w50', 'w300 w7', 'w1500 w20 w3', 'w2 w900', 'w40 OR w41 OR w7']) q LOOP
		SELECT array_agg(id ORDER BY s DESC, id), max(s) INTO ids, top FROM
			(SELECT id, sift.score(ctid) s FROM dd WHERE body @@> r.q ORDER BY sift.score(ctid) DESC, id LIMIT 10) t;
		IF label = 'before' THEN
			INSERT INTO dd_ref VALUES (r.n, r.q, ids, top);
		END IF;
		SELECT * INTO ref FROM dd_ref WHERE n = r.n;
		RETURN QUERY SELECT label, r.q, (SELECT count(*)::int FROM unnest(ids) a WHERE a = ANY (ref.ids)),
			round((top / ref.top)::numeric, 3), (SELECT sum(i.ndocs)::bigint FROM sift.segments('dd_idx') i);
	END LOOP;
END $f$;
SELECT * FROM dd_snapshot('before');

\echo -- dead documents like the live ones
SELECT dd_rollbacks(:dead, false);
SELECT * FROM dd_snapshot('dead like live');
\echo -- dead documents about one term
SELECT dd_rollbacks(:dead, true);
SELECT * FROM dd_snapshot('dead about w7');
VACUUM dd;
SELECT * FROM dd_snapshot('after VACUUM');
SELECT sift.promote('dd_idx'), sift.merge('dd_idx', true);
SELECT * FROM dd_snapshot('after VACUUM and merge');
DROP TABLE dd, dd_ref;
DROP FUNCTION dd_doc(bool), dd_rollbacks(int, bool), dd_snapshot(text);
