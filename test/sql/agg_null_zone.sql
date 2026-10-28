-- x IS [NOT] NULL is a test of the NULL count of a unit's zone: a unit with no NULL (IS NULL), or only NULLs (IS NOT NULL), is not read, and a unit all of whose rows pass
-- (no NULL for IS NOT NULL, only NULLs for IS NULL) is answered from its pre-aggregates, as a unit inside a comparison is. A page holds one row, so unit n holds the rows
-- 256 * n .. 256 * n + 255 of 20 units: a_all is NULL in the units 0..9 and set in the others, mixed in every seventh row, tx in the units that are multiples of 3,
-- nu holds a NaN in unit 5 (a NaN is stored as a NULL slot with a flag: it is not NULL) and NULLs in unit 6, ts is NULL in the units 15 to 19.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE nz (id int, g int, a int, a_all int, mixed int, tx text, nu numeric(10,2), ts timestamp, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO nz SELECT i, i % 5, i % 100, CASE WHEN i / 256 < 10 THEN NULL ELSE i % 13 END, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 11 END,
       CASE WHEN i / 256 % 3 = 0 THEN NULL ELSE 'v' || i / 256 END,
       CASE WHEN i / 256 = 5 THEN 'NaN' WHEN i / 256 = 6 THEN NULL ELSE (i % 50) / 4.0 END,
       CASE WHEN i / 256 >= 15 THEN NULL ELSE timestamp '2025-01-01' + i * interval '1 minute' END, repeat('x', 600)
FROM generate_series(0, 20 * 256 - 1) i;
CREATE INDEX nz_idx ON nz USING sift_col (id, g, a, a_all, mixed, tx, nu, ts) WITH (rollup_dims = 'off', preagg = 'cube(g: a)');
VACUUM (FREEZE, ANALYZE) nz;
SELECT units FROM sift.col_segments('nz_idx') WHERE kind = 'immutable';

-- the units a statement reads, prunes and answers from pre-aggregates ('normal' when SiftAgg does not run it), and whether its rows are those of the normal plan
CREATE FUNCTION chk(q text) RETURNS TABLE (plan text, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE l text; read text; pruned text := '0'; pre text;
BEGIN
	plan := 'normal';
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN plan := 'SiftAgg'; END IF;
		IF l ~ 'Units: (\d+)' THEN read := substring(l from 'Units: (\d+)'); END IF;
		IF l ~ '(\d+) pruned' THEN pruned := substring(l from '(\d+) pruned'); END IF;
		IF l ~ '(\d+) from pre-aggregates' THEN pre := substring(l from '(\d+) from pre-aggregates'); END IF;
	END LOOP;
	IF plan = 'SiftAgg' THEN plan := format('SiftAgg read %s pruned %s pre-agg %s', read, pruned, pre); END IF;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	vs_normal_plan := (SELECT count(*) FROM ((SELECT * FROM r_on EXCEPT ALL SELECT * FROM r_off) UNION ALL (SELECT * FROM r_off EXCEPT ALL SELECT * FROM r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE nz_q (n serial, q text);
INSERT INTO nz_q (q)
SELECT format('SELECT %s FROM nz WHERE %s%s%s', shape, col || test, extra, grp)
FROM (VALUES ('count(*), sum(a)', ''), ('g, count(*), sum(a)', ' GROUP BY g'), ('count(*), sum(a), min(id), max(id)', ''), ('a, count(*)', ' GROUP BY a')) s(shape, grp),
     (VALUES ('a_all'), ('mixed'), ('tx'), ('nu'), ('ts'), ('id'), ('g')) c(col),
     (VALUES (' IS NULL'), (' IS NOT NULL')) t(test),
     (VALUES (''), (' AND a < 50'), (' AND g = 2')) e(extra);
INSERT INTO nz_q (q) VALUES ('SELECT count(*) FROM nz WHERE a_all IS NULL OR g = 3'), ('SELECT g, sum(a) FROM nz WHERE tx IS NULL OR mixed IS NULL GROUP BY g'),
       ('SELECT count(*) FROM nz WHERE a_all IS NULL AND tx IS NULL'), ('SELECT count(*) FROM nz WHERE a_all IS NOT NULL AND ts IS NULL'),
       ('SELECT count(a_all) AS c1, count(tx) AS c2, count(*) AS c3 FROM nz WHERE nu IS NOT NULL'), ('SELECT count(*) FILTER (WHERE a_all IS NULL) AS f, count(*) AS n FROM nz WHERE g < 3');

SELECT n, left(q, 90) AS query, c.* FROM nz_q, LATERAL chk(q) c ORDER BY n;

DROP FUNCTION chk(text);
DROP TABLE nz_q;
DROP TABLE nz;
DROP EXTENSION pg_sift;
