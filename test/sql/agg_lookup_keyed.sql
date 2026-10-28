-- A lookup table that a later inner join keys by one of its columns alone (the ids of a request, a CTE of them, joined to the table of their rows) is read by a
-- btree probe for each key while the probes cost less than its pages, where it was read whole: the rows loaded are the keys', the answers the same, and a request
-- of many keys reads the table whole as before.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;

CREATE TABLE ev (id int, dim_id int, v int, d date) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 2000, i % 7, date '2026-01-01' + i % 300 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, dim_id, v, d);
-- twenty rows to a page: a table of a hundred pages
CREATE TABLE dims (id int PRIMARY KEY, kind int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO dims SELECT i, i % 5, repeat('x', 350) FROM generate_series(0, 1999) i;
CREATE TABLE picks (grp int, dim_id int);
INSERT INTO picks SELECT 1, i FROM generate_series(10, 12) i;
INSERT INTO picks SELECT 2, i FROM generate_series(0, 1399) i;
-- a key of twenty-five rows (a thousand pages): the probes read a page for each row they find, so the keys of a request must be fewer for them to pay
CREATE TABLE grps (g int, kind int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO grps SELECT i % 800, i % 5, repeat('x', 350) FROM generate_series(0, 19999) i;
CREATE INDEX grps_g ON grps (g);
CREATE TABLE gpicks (grp int, g int);
INSERT INTO gpicks SELECT 1, i FROM generate_series(10, 12) i;
INSERT INTO gpicks SELECT 2, i FROM generate_series(0, 299) i;
VACUUM (FREEZE, ANALYZE) ev, dims, picks, grps, gpicks;

CREATE FUNCTION request(grp int) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH ids AS MATERIALIZED (SELECT DISTINCT dims.id FROM dims WHERE dims.id IN (SELECT dim_id FROM picks WHERE grp = %s)),
		 d AS (SELECT dims.*, to_jsonb(dims) AS doc FROM ids INNER JOIN dims ON dims.id = ids.id)
		 SELECT d.kind, count(*), sum(ev.v) FROM ev INNER JOIN d ON d.id = ev.dim_id WHERE ev.d >= '2026-02-01' GROUP BY 1 ORDER BY 1$q$, grp)
$$;

CREATE FUNCTION grequest(grp int) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH ids AS MATERIALIZED (SELECT DISTINCT g FROM gpicks WHERE grp = %s),
		 d AS (SELECT grps.*, to_jsonb(grps) AS doc FROM ids INNER JOIN grps ON grps.g = ids.g)
		 SELECT d.kind, count(*), sum(ev.v) FROM ev INNER JOIN d ON d.g = ev.dim_id WHERE ev.d >= '2026-02-01' GROUP BY 1 ORDER BY 1$q$, grp)
$$;

-- the rows loaded and the index scans of the scan, from its EXPLAIN
CREATE FUNCTION loaded(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := ''; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '^ *Lookup: ' THEN r := r || concat_ws('; ', substring(l from '\d+ rows read'), substring(l from '\d+ index scans')) || '; '; END IF;
	END LOOP;
	RETURN r;
END $$;

-- three keys of two thousand: the three rows (and the few rows of the other lookups), one index scan
SELECT loaded(request(1));
-- fourteen hundred keys: the table is read whole
SELECT loaded(request(2));

-- three keys of twenty-five rows: seventy-five rows by probes; three hundred keys (seven thousand five hundred rows, the pages of the table): read whole
SELECT loaded(grequest(1));
SELECT loaded(grequest(2));

-- the answers: the same with the table read whole (the setting off), by tuple
CREATE FUNCTION answers(grp int, probe bool, grouped bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.agg_lookup_pushdown', probe::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || CASE WHEN grouped THEN grequest(grp) ELSE request(grp) END || ') t' INTO r;
	RETURN r;
END $$;
SELECT grouped, answers(g, true, grouped) = answers(g, false, grouped) AS same_answers, answers(g, true, grouped) IS NOT NULL AS not_empty
FROM generate_series(1, 2) g, (VALUES (false), (true)) v(grouped) ORDER BY 1, g;

DROP FUNCTION answers(int, bool, bool), loaded(text), request(int), grequest(int);
DROP TABLE ev, dims, picks, grps, gpicks;
DROP EXTENSION pg_sift;
