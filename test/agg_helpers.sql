\set ECHO none
-- Shared by the agg_* tests (\ir test/agg_helpers.sql): outcome(q, enabled) is the result of q, or its SQLSTATE and message, with the aggregate scan
-- off (the table scans on) and on (the table scans off, so a Custom Scan (SiftAgg) runs when the query is one it can); runs_in_scan(q) tells whether it does.
-- runq(q, agg) is the sorted rows of q as jsonb text, or its error, with the aggregate scan on or off; pushed(q) tells whether the plan has a Custom Scan (SiftAgg).
CREATE OR REPLACE FUNCTION outcome(q text, enabled bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE result text;
BEGIN
  PERFORM set_config('sift.enable_agg', enabled::text, true);
  PERFORM set_config(s, (NOT enabled)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text)::text, ''no rows'') FROM (' || q || ') t' INTO result;
  RETURN result;
EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE || ' ' || SQLERRM;
END $$;
CREATE OR REPLACE FUNCTION runs_in_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE line text; found bool := false;
BEGIN
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP found := found OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  RETURN found;
END $$;
CREATE OR REPLACE FUNCTION runq(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
END $$;
CREATE OR REPLACE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
\set ECHO all
