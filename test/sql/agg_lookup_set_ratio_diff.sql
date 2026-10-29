-- Differential test of the plan agg_lookup_set_ratio covers: a list of keys (a CTE of ids) that a statement joins and names again in an IN sub-select of the same column.
-- Every statement runs with the aggregate scan off, on and forced (the rows are compared as sorted jsonb text, a total order of the whole row) and with the planner's own
-- choice on and off, for scopes of 0, 1, 5, 60, 200 and all the entities, a scope that lists an entity twice and a NULL one, entities without rows, rows without an
-- entity, a price or a unit, an empty period, and after updates, deletes and inserts of the rows and of the scopes between the runs.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 100;
CREATE TABLE units (id int PRIMARY KEY, flag bool);
INSERT INTO units SELECT i, CASE WHEN i % 9 = 0 THEN true WHEN i % 11 = 0 THEN NULL ELSE false END FROM generate_series(1, 400) i;
CREATE TABLE dim_c (id int PRIMARY KEY, slug text);
INSERT INTO dim_c SELECT i, 'src' || i FROM generate_series(1, 12) i;
-- the entities of a scope: group 0 is empty, 1 has one entity, 2 five, 3 sixty, 4 two hundred, 5 all 600; 6 lists an entity twice and a NULL one
CREATE TABLE links (grp int, dim_a_id int);
INSERT INTO links SELECT g - 1, (i * 7) % 600 FROM unnest(ARRAY[0, 1, 5, 60, 200, 600]) WITH ORDINALITY s(n, g), generate_series(0, 599) i WHERE i < n;
INSERT INTO links VALUES (6, 3), (6, 3), (6, 4), (6, NULL), (6, 598);
-- the entities 590 to 599 hold no rows; the first 20 hold most of them
CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, unit_id int, foreign_id int, day date, price float8, latest bool, ccy text) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 41 = 0 THEN NULL WHEN i % 3 = 0 THEN i % 20 ELSE (i * 13) % 590 END,
       CASE WHEN i % 29 = 0 THEN NULL ELSE 1 + (i * 17) % 400 END, 1 + i % 12, date '2024-01-01' + (i * 7) % 500,
       CASE WHEN i % 23 = 0 THEN NULL ELSE 50 + (i * 31) % 300 END, i % 4 <> 0, CASE i % 5 WHEN 0 THEN NULL WHEN 1 THEN 'eur' ELSE 'usd' END
FROM generate_series(1, 30000) i;
CREATE INDEX ev_entity_day ON ev (dim_a_id, day);
CREATE INDEX ev_col ON ev USING sift_col (id, dim_a_id, unit_id, foreign_id, day, price, latest, ccy);
VACUUM (FREEZE, ANALYZE) ev;
ANALYZE units; ANALYZE dim_c; ANALYZE links;
\ir test/agg_helpers.sql
-- the report of a scope: the ids as a CTE, joined and again in an IN list, the rows of a period by month and unit
CREATE FUNCTION ls_stmt(form text, g int) RETURNS text LANGUAGE sql AS $f$ SELECT format(CASE form
 WHEN 'twice' THEN $q$
 WITH s AS (SELECT dim_a_id AS id FROM links WHERE grp = %s)
 SELECT e.dim_a_id, date_trunc('month', e.day)::date AS month, e.unit_id, e.ccy, count(*) AS n, count(e.price) AS priced, round(avg(e.price)::numeric, 6) AS price,
        round(sum(e.price)::numeric, 6) AS total, max(e.price) AS top, count(DISTINCT e.id) AS ids
 FROM ev e JOIN s ON s.id = e.dim_a_id
 JOIN dim_c r ON r.id = e.foreign_id AND r.slug IN ('src1', 'src2', 'src3', 'src7')
 LEFT JOIN units u ON u.id = e.unit_id
 WHERE e.latest AND u.flag IS NOT TRUE AND e.unit_id IS NOT NULL AND e.dim_a_id IN (SELECT id FROM s GROUP BY id) AND e.day >= '2024-03-01' AND e.day < '2025-03-01'
 GROUP BY 1, 2, 3, 4 $q$
 WHEN 'once' THEN $q$
 WITH s AS (SELECT dim_a_id AS id FROM links WHERE grp = %s)
 SELECT e.dim_a_id, date_trunc('month', e.day)::date AS month, e.unit_id, e.ccy, count(*) AS n, round(avg(e.price)::numeric, 6) AS price
 FROM ev e JOIN s ON s.id = e.dim_a_id LEFT JOIN units u ON u.id = e.unit_id
 WHERE e.latest AND u.flag IS NOT TRUE AND e.day >= '2024-03-01' AND e.day < '2025-03-01'
 GROUP BY 1, 2, 3, 4 $q$
 WHEN 'periods' THEN $q$
 WITH s AS MATERIALIZED (SELECT dim_a_id AS id FROM links WHERE grp = %s),
 periods AS (SELECT * FROM (VALUES ('current', daterange('2025-01-01', '2025-04-30', '[]')), ('previous', daterange('2024-01-01', '2024-04-30', '[]')),
                                   ('empty', daterange('2031-01-01', '2031-02-01', '[]'))) p(period, range)),
 parsed AS (SELECT e.id, e.dim_a_id, p.period, e.price FROM ev e JOIN periods p ON e.day <@ p.range JOIN s ON s.id = e.dim_a_id
            WHERE e.dim_a_id IN (SELECT id FROM s))
 SELECT d.period, count(x.id) AS n, count(DISTINCT x.dim_a_id) AS dim_a, count(x.price) AS priced, round(avg(x.price)::numeric, 6) AS price, round(sum(x.price)::numeric, 6) AS total
 FROM periods d LEFT JOIN parsed x ON x.period = d.period GROUP BY d.period $q$
 ELSE $q$
 WITH s AS (SELECT dim_a_id AS id FROM links WHERE grp = %s)
 SELECT count(*) AS n, count(DISTINCT e.dim_a_id) AS dim_a, count(e.price) AS priced, round(sum(e.price)::numeric, 6) AS total, min(e.day) AS first, max(e.day) AS last
 FROM ev e JOIN s ON s.id = e.dim_a_id WHERE e.dim_a_id IN (SELECT id FROM s) $q$ END, g) $f$;
CREATE FUNCTION ls_check(step text) RETURNS TABLE (label text, same bool, same_default bool, rows int) LANGUAGE plpgsql AS $f$
DECLARE g int; f text; q text;
BEGIN
  FOREACH g IN ARRAY ARRAY[0, 1, 2, 3, 4, 5, 6] LOOP
    FOREACH f IN ARRAY ARRAY['twice', 'once', 'periods', 'total'] LOOP
      q := ls_stmt(f, g);
      label := step || ' scope ' || g || ' ' || f;
      same := outcome(q, false) = outcome(q, true);
      same_default := runq(q, false) = runq(q, true);
      rows := CASE WHEN outcome(q, true) LIKE '[%' THEN (SELECT count(*) FROM jsonb_array_elements(outcome(q, true)::jsonb)) WHEN outcome(q, true) = 'no rows' THEN 0 ELSE -1 END;
      RETURN NEXT;
    END LOOP;
  END LOOP;
END $f$;
SELECT * FROM ls_check('initial');
-- the scan serves the statement of a scope when the table scans are off
SELECT pushed(ls_stmt('twice', 3)) AS twice_pushed, pushed(ls_stmt('periods', 3)) AS periods_pushed;
-- updates and deletes of the rows between the runs: days move, rows go, prices and entities become NULL
UPDATE ev SET day = day + 40 WHERE id % 7 = 0;
DELETE FROM ev WHERE id % 11 = 0;
UPDATE ev SET price = NULL WHERE id % 13 = 0;
UPDATE ev SET dim_a_id = NULL WHERE id % 17 = 0;
SELECT * FROM ls_check('after dml');
-- the scopes change: a group shrinks, grows, loses its entities, gets a duplicate and a NULL
DELETE FROM links WHERE grp = 4 AND dim_a_id % 2 = 0;
INSERT INTO links VALUES (1, 5), (1, 5), (2, NULL), (0, 7), (3, 595);
DELETE FROM links WHERE grp = 5 AND dim_a_id > 300;
SELECT * FROM ls_check('after links');
INSERT INTO ev SELECT 30000 + i, 1 + i % 600, 1 + i % 400, 1 + i % 12, date '2025-01-01' + i % 120, 100 + i % 50, true, 'usd' FROM generate_series(1, 3000) i;
SELECT * FROM ls_check('after insert');
VACUUM (ANALYZE) ev;
SELECT * FROM ls_check('after vacuum');
DROP FUNCTION ls_check(text), ls_stmt(text, int), outcome(text, bool), runs_in_scan(text), pushed, runq;
DROP TABLE ev, links, units, dim_c;
DROP EXTENSION pg_sift;
