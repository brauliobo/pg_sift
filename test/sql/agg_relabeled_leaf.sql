-- One column read twice under binary-compatible types (as itself and relabeled for a comparison with a char column) is one input of an expression key.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE countries (iso2 char(2) PRIMARY KEY, name text);
INSERT INTO countries VALUES ('US', 'United States'), ('BR', 'Brazil');
CREATE TABLE facts (id int, kind text, country_code varchar(2));
INSERT INTO facts SELECT i, CASE i % 3 WHEN 0 THEN 'a' WHEN 1 THEN 'b' END, CASE i % 4 WHEN 0 THEN 'US' WHEN 1 THEN 'BR' WHEN 2 THEN 'XX' END FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, kind, country_code);
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE countries;
-- The result, or the SQLSTATE and message, with the scan off and on. on: the table scans are off, so a Custom Scan (SiftAgg) runs when the query is one it can run.
CREATE FUNCTION outcome(q text, enabled bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE result text;
BEGIN
  PERFORM set_config('sift.enable_agg', enabled::text, true);
  PERFORM set_config(s, (NOT enabled)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text)::text, ''no rows'') FROM (' || q || ') t' INTO result;
  RETURN result;
EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE || ' ' || SQLERRM;
END $$;
CREATE FUNCTION runs_in_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE line text; found bool := false;
BEGIN
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP found := found OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  RETURN found;
END $$;
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), left(outcome(q, true), 100);
END $$;
SELECT * FROM check_query('key', $q$
 SELECT jsonb_build_array(jsonb_build_object('kind', kind), CASE WHEN country_code IN (SELECT iso2 FROM countries) THEN jsonb_build_object('iso2', country_code, 'type', 'country') END) AS attribute,
        count(*) AS n
 FROM facts GROUP BY 1
$q$);
SELECT * FROM check_query('key and filter', $q$
 SELECT CASE WHEN country_code IN (SELECT iso2 FROM countries) THEN country_code || kind END AS k, count(*) FILTER (WHERE country_code IN (SELECT iso2 FROM countries)) AS known
 FROM facts GROUP BY 1
$q$);
DROP FUNCTION check_query(text, text), outcome(text, bool), runs_in_scan(text);
DROP TABLE facts, countries;
DROP EXTENSION pg_sift;
