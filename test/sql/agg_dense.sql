-- Counts, sums and averages with and without a FILTER or a group key, over a plain table, the elements of an array of objects and
-- floats that are not finite: the shapes whose chunks the executor aggregates without a list of tuples. Every query runs with and
-- without the pushdown and the rows must be the same (the floats are multiples of 0.25, so a sum does not depend on the order of its
-- additions); `pushed` says whether the plan is a Custom Scan (SiftAgg).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
-- the element cubes of the pre-aggregates answer the grouped element queries without a scan; the chunks are the point here
SET sift.agg_preagg = off;

-- 9000 rows (three chunks of 4096 and a tail): NULLs in the key, the filter column and the arguments
CREATE TABLE d_t (id int, grp int, kind text, flag text, qty int4, amt float8);
INSERT INTO d_t
SELECT i, i % 7,
       CASE WHEN i % 13 = 0 THEN NULL ELSE 'k' || (i * 7 % 11) END,
       CASE WHEN i % 17 = 0 THEN NULL ELSE (ARRAY['fa', 'fb', 'fc'])[1 + i % 3] END,
       CASE WHEN i % 5 = 0 THEN NULL ELSE (i * 37) % 1000 - 300 END,
       CASE WHEN i % 6 = 0 THEN NULL ELSE ((i * 53) % 400 - 100) * 0.25 END
FROM generate_series(1, 9000) i;
CREATE INDEX d_col ON d_t USING sift_col (id, grp, kind, flag, qty, amt);
VACUUM ANALYZE d_t;

-- 3000 documents of 0 to 6 elements, one of 9000 (an element chunk ends inside a document), SQL NULL and empty arrays
CREATE TABLE d_j (id int, grp int, label text, items jsonb);
INSERT INTO d_j
SELECT i, i % 5, CASE WHEN i % 9 = 0 THEN NULL ELSE 'L' || (i % 4) END,
       CASE WHEN i % 11 = 0 THEN NULL
            WHEN i = 7 THEN (SELECT jsonb_agg(jsonb_build_object('k', 'c' || (k % 5), 'f', (ARRAY['fa', 'fb'])[1 + k % 2], 'n', k % 100, 'x', (k % 40) * 0.25) ORDER BY k)
                             FROM generate_series(1, 9000) k)
            ELSE (SELECT coalesce(jsonb_agg(jsonb_build_object('k', CASE WHEN (i + k) % 23 = 0 THEN NULL ELSE 'c' || ((i + k) % 5) END,
                                                              'f', (ARRAY['fa', 'fb', 'fc'])[1 + (i * k) % 3],
                                                              'n', CASE WHEN (i + k) % 7 = 0 THEN NULL ELSE (i * k) % 50 - 20 END,
                                                              'x', CASE WHEN (i + k) % 6 = 0 THEN NULL ELSE ((i * k) % 80 - 20) * 0.25 END) ORDER BY k), '[]')
                  FROM generate_series(1, (i * 5) % 7) k) END
FROM generate_series(1, 3000) i;
CREATE INDEX d_jcol ON d_j USING sift_col (id, grp, label, items) WITH (paths = 'items[]{k text, f text, n int4, x float8}');
VACUUM ANALYZE d_j;

-- the same documents never vacuumed (no page is all-visible, so the rows that keep an element are checked against the heap), with deleted and updated rows
CREATE TABLE d_nv (id int, grp int, label text, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO d_nv SELECT * FROM d_j WHERE id <= 2000 AND id <> 7;
CREATE INDEX d_nvcol ON d_nv USING sift_col (id, grp, label, items) WITH (paths = 'items[]{k text, f text, n int4, x float8}');
DELETE FROM d_nv WHERE id % 10 = 3;
UPDATE d_nv SET items = '[{"k": "c1", "f": "fa", "n": 1, "x": 0.5}]' WHERE id % 10 = 4;

-- floats that are not finite: infinities of both signs, NaN, a group that has only NULLs
CREATE TABLE d_f (id int, grp int, amt float8);
INSERT INTO d_f
SELECT i, i % 5,
       CASE WHEN i % 50 = 1 AND i % 5 = 1 THEN 'Infinity' WHEN i % 70 = 2 AND i % 5 = 2 THEN '-Infinity' WHEN i % 90 = 3 AND i % 5 = 3 THEN 'NaN'
            WHEN i % 5 = 4 THEN NULL ELSE (i % 40) * 0.5 END
FROM generate_series(1, 5000) i;
CREATE INDEX d_fcol ON d_f USING sift_col (id, grp, amt);
VACUUM ANALYZE d_f;

CREATE FUNCTION d_rows(q text, push bool, dense bool DEFAULT true) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('sift.agg_dense', dense::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION d_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- did the scan aggregate a chunk from its bitmap
CREATE FUNCTION d_dense(q text, dense bool) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.agg_dense', dense::text, true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Dense Chunks: [1-9]' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE d_q (q text);
INSERT INTO d_q VALUES
	-- a plain table: ungrouped
	('SELECT count(*), count(kind), count(qty), count(amt) FROM d_t'),
	('SELECT count(*) FILTER (WHERE flag = ''fa''), count(*) FILTER (WHERE flag = ''fb''), count(*) FROM d_t'),
	('SELECT count(*) FILTER (WHERE flag = ''fa'' AND qty > 100), count(qty) FILTER (WHERE flag <> ''fc''), count(amt) FILTER (WHERE kind LIKE ''k1%'') FROM d_t'),
	('SELECT sum(qty), avg(qty), min(qty), max(qty) FROM d_t'),
	('SELECT sum(qty) FILTER (WHERE flag = ''fb''), avg(qty) FILTER (WHERE grp < 3), sum(qty) FILTER (WHERE flag IS NULL) FROM d_t'),
	('SELECT sum(amt), avg(amt), count(amt) FROM d_t'),
	('SELECT sum(amt) FILTER (WHERE kind = ''k3''), avg(amt) FILTER (WHERE qty < 0), sum(amt) FILTER (WHERE kind IS NULL) FROM d_t'),
	('SELECT count(*), sum(qty), avg(amt) FROM d_t WHERE flag = ''fa'''),
	('SELECT count(*), sum(qty), avg(amt) FROM d_t WHERE flag = ''fa'' AND id > 4000 AND id < 4100'),
	('SELECT count(*), sum(amt) FROM d_t WHERE id < 0'),
	('SELECT count(*), sum(qty), avg(amt), count(*) FILTER (WHERE false) FROM d_t WHERE kind = ''nothing'''),
	-- a plain table: grouped by a text column, an integer, both
	('SELECT kind, count(*), count(qty), sum(qty), avg(qty) FROM d_t GROUP BY 1'),
	('SELECT kind, count(*) FILTER (WHERE flag = ''fa''), sum(amt), avg(amt) FROM d_t GROUP BY 1'),
	('SELECT flag, count(*), sum(qty) FILTER (WHERE grp = 3), avg(amt) FILTER (WHERE qty > 0) FROM d_t GROUP BY 1'),
	('SELECT kind, flag, count(*), sum(amt), sum(qty) FROM d_t GROUP BY 1, 2'),
	('SELECT kind, count(*), avg(amt) FROM d_t WHERE grp <> 2 AND flag <> ''fb'' GROUP BY 1'),
	('SELECT kind, count(*), sum(qty) FROM d_t WHERE id < 5000 GROUP BY 1'),
	('SELECT kind, count(*), sum(qty) FROM d_t WHERE id > 8990 GROUP BY 1'),
	('SELECT kind, flag, count(*), sum(amt) FROM d_t WHERE id > 4000 AND id < 4100 AND flag <> ''fb'' GROUP BY 1, 2'),
	('SELECT grp, count(*), sum(amt), avg(qty), count(*) FILTER (WHERE kind = ''k5'') FROM d_t GROUP BY 1'),
	('SELECT grp, kind, count(*) FILTER (WHERE amt > 0), sum(qty) FROM d_t GROUP BY 1, 2'),
	('SELECT kind, count(*) FROM d_t GROUP BY 1 HAVING count(*) > 700'),
	-- the elements of an array of objects: ungrouped
	('SELECT count(*), count(e.k), count(e.n), count(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8)'),
	('SELECT count(*) FILTER (WHERE e.f = ''fa''), count(*) FILTER (WHERE e.f = ''fb''), count(*) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8)'),
	('SELECT sum(e.n), avg(e.n), sum(e.x), avg(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8)'),
	('SELECT count(*) FILTER (WHERE e.k = ''c1''), sum(e.x) FILTER (WHERE e.f = ''fa''), avg(e.n) FILTER (WHERE e.k <> ''c2'') FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8)'),
	('SELECT count(*), sum(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) WHERE e.f = ''fc'' AND d.grp < 3'),
	('SELECT count(*), sum(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) WHERE d.label = ''L1'''),
	('SELECT count(*), sum(e.n) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) WHERE d.id = 7'),
	('SELECT count(*) FILTER (WHERE d.label = ''L2''), sum(d.grp), count(d.label) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8)'),
	-- the elements: grouped by an element key, a document key, both
	('SELECT e.k, count(*), avg(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) GROUP BY 1'),
	('SELECT e.k, count(*), sum(e.n), avg(e.x), count(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) GROUP BY 1'),
	('SELECT e.k, e.f, count(*) FILTER (WHERE e.n > 0), sum(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) GROUP BY 1, 2'),
	('SELECT d.label, count(*), sum(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) GROUP BY 1'),
	('SELECT d.label, e.k, count(*), avg(e.n) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) WHERE d.grp <> 1 AND e.f <> ''fc'' GROUP BY 1, 2'),
	('SELECT e.k, count(*), avg(e.x) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) WHERE d.id <> 7 GROUP BY 1'),
	('SELECT e.k, count(*), sum(e.n) FROM d_j d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) WHERE d.id = 7 GROUP BY 1'),
	-- the documents never vacuumed
	('SELECT count(*), count(e.k), sum(e.n), sum(e.x) FROM d_nv d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8)'),
	('SELECT count(*) FILTER (WHERE e.f = ''fa''), count(*) FILTER (WHERE e.f = ''fb''), avg(e.x) FROM d_nv d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8)'),
	('SELECT e.k, count(*), avg(e.x) FROM d_nv d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) GROUP BY 1'),
	('SELECT d.label, count(*), sum(e.n) FROM d_nv d CROSS JOIN LATERAL jsonb_to_recordset(d.items) AS e(k text, f text, n int, x float8) WHERE d.grp < 3 GROUP BY 1'),
	-- floats that are not finite
	('SELECT count(*), count(amt), sum(amt), avg(amt) FROM d_f'),
	('SELECT grp, count(amt), sum(amt), avg(amt) FROM d_f GROUP BY 1'),
	('SELECT sum(amt) FILTER (WHERE grp = 1), sum(amt) FILTER (WHERE grp = 2), sum(amt) FILTER (WHERE grp = 3), sum(amt) FILTER (WHERE grp = 0), sum(amt) FILTER (WHERE grp = 4) FROM d_f'),
	('SELECT avg(amt) FILTER (WHERE grp = 1), avg(amt) FILTER (WHERE grp = 2), avg(amt) FILTER (WHERE grp = 0) FROM d_f'),
	('SELECT sum(amt), avg(amt) FROM d_f WHERE id < 40'),
	('SELECT grp, sum(amt) FROM d_f WHERE id < 40 GROUP BY 1');

CREATE FUNCTION d_compare(OUT n int, OUT same bool, OUT pushed bool, OUT dense bool, OUT without_dense bool) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE query text;
BEGIN
	n := 0;
	FOR query IN SELECT q FROM d_q ORDER BY ctid LOOP
		n := n + 1;
		pushed := d_pushed(query);
		same := d_rows(query, true) IS NOT DISTINCT FROM d_rows(query, false) AND d_rows(query, true, false) IS NOT DISTINCT FROM d_rows(query, false);
		dense := d_dense(query, true);
		without_dense := d_dense(query, false);
		RETURN NEXT;
	END LOOP;
END $$;

-- same: the rows are the normal plan's with and without dense chunks; dense: some chunk was aggregated from its bitmap (never with sift.agg_dense off)
SELECT n, same, pushed, dense, without_dense FROM d_compare();

-- a sum or an average of floats that overflows raises the error PostgreSQL raises, from the scan and from the plan
CREATE TABLE d_big (id int, amt float8);
INSERT INTO d_big SELECT i, 1e308 FROM generate_series(1, 5000) i;
CREATE INDEX d_bigcol ON d_big USING sift_col (id, amt);
VACUUM d_big;
CREATE FUNCTION d_err(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config('sift.agg_dense', 'on', true);
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLERRM;
END $$;
SELECT q, d_err(q, true) = d_err(q, false) AS same, d_err(q, false) AS error
FROM (VALUES ('SELECT sum(amt) FROM d_big'), ('SELECT avg(amt) FROM d_big'), ('SELECT sum(amt) FROM d_big WHERE id < 2'),
             ('SELECT id % 2, sum(amt) FROM d_big GROUP BY 1')) v(q);

DROP TABLE d_t, d_j, d_nv, d_f, d_big, d_q;
DROP FUNCTION d_rows(text, bool, bool), d_pushed(text), d_dense(text, bool), d_compare(), d_err(text, bool);
DROP EXTENSION pg_sift;
