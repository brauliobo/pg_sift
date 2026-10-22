-- A table joined inside the nullable side of a LEFT JOIN from a spine (the periods), its filter in the WHERE of the sub-select: PostgreSQL moves that
-- WHERE into the ON clause of the outer join. A condition that reads nothing of the spine filters the joined rows before the match, as in the ON clause
-- of the inner join, so a lookup whose filter matches nothing ends the scan without a unit read. A condition that reads the spine stays in the match.
-- The result must be the normal plan's (the spine's rows without a match are still there); the units read are printed for the empty scopes.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dims (id int PRIMARY KEY, slug text NOT NULL, mode text NOT NULL, enabled bool NOT NULL);
INSERT INTO dims SELECT i, 'd' || i, (ARRAY['all', 'some', 'none'])[i % 3 + 1], i % 10 <> 0 FROM generate_series(1, 300) i;
CREATE INDEX dims_slug ON dims (slug);
CREATE TABLE events (id int, dim_id int NOT NULL, ts date NOT NULL, kind text, amount float8);
INSERT INTO events SELECT i, 1 + i % 300, date '2024-01-01' + i % 600, 'cat_' || chr(97 + i % 4), i % 13 + 0.5 FROM generate_series(1, 30000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_id, ts, kind, amount);
VACUUM (FREEZE, ANALYZE) events, dims;
\ir test/agg_helpers.sql
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT outcome(q, false) = outcome(q, true) $$;
-- the units the scan reads (NULL when the plan is not a scan)
CREATE FUNCTION units_read(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
  PERFORM set_config('sift.enable_agg', 'true', true);
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
  RETURN (jsonb_path_query_first(j, '$.** ? (@."Custom Plan Provider" == "SiftAgg")."Units"') #>> '{}')::bigint;
END $$;
CREATE FUNCTION spine_query(filter text) RETURNS text LANGUAGE sql AS $$ SELECT format($f$
 WITH periods AS (SELECT * FROM (VALUES ('cur', daterange('2024-03-01', '2025-03-01')), ('prev', daterange('2023-03-01', '2024-03-01'))) p(period, rng))
 SELECT pe.period, count(DISTINCT ev.id) AS n, sum(ev.amount) AS total
 FROM periods pe LEFT JOIN (
   SELECT e.id, e.amount, p2.period FROM events e JOIN periods p2 ON e.ts <@ p2.rng JOIN dims d ON d.id = e.dim_id WHERE %s
 ) ev ON ev.period = pe.period GROUP BY 1$f$, filter) $$;

-- no dimension row passes: an indexed column, one that is not, and a condition on two columns
SELECT same(spine_query($$d.slug = 'none'$$)) AS same_indexed, units_read(spine_query($$d.slug = 'none'$$)) AS units_indexed;
SELECT same(spine_query($$d.mode = 'none_such'$$)) AS same_plain, units_read(spine_query($$d.mode = 'none_such'$$)) AS units_plain;
SELECT same(spine_query($$d.enabled AND d.slug = 'none'$$)) AS same_two, units_read(spine_query($$d.enabled AND d.slug = 'none'$$)) AS units_two;
-- some pass: the rows are the normal plan's, and the scan reads
SELECT same(spine_query($$d.slug = 'd7'$$)) AS same_one, units_read(spine_query($$d.slug = 'd7'$$)) > 0 AS reads_one;
SELECT same(spine_query($$d.mode = 'some'$$)) AS same_third, units_read(spine_query($$d.mode = 'some'$$)) > 0 AS reads_third;
-- a condition that reads the spine stays in the match
SELECT same(replace(spine_query($$d.enabled$$), 'ON ev.period = pe.period', 'ON ev.period = pe.period AND ev.id % 2 = 0')) AS same_extra_on;
SELECT same(replace(spine_query($$d.enabled$$), 'GROUP BY 1', 'GROUP BY 1 HAVING count(DISTINCT ev.id) >= 0')) AS same_having;
-- the spine itself empties
SELECT same(replace(spine_query($$d.enabled$$), 'ON ev.period = pe.period', 'ON ev.period = pe.period AND pe.period = ''none''')) AS same_spine_condition;

DROP FUNCTION same(text), outcome(text, bool), runs_in_scan(text), units_read(text), spine_query(text);
DROP TABLE events, dims;
DROP EXTENSION pg_sift;
