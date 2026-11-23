\set ECHO none
-- Shared by the agg_* tests that compare the rows of a query with the aggregate scan on and off (\ir test/agg_check_query.sql): check_query(q) says whether the
-- cheapest plan has a Custom Scan (SiftAgg), whether the plan with the table scans off does ("eligible"), and how many rows differ between the two results.
CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, eligible bool, differences bigint) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP pushed := pushed OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  SET LOCAL enable_indexonlyscan = off;
  eligible := false;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP eligible := eligible OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;
\set ECHO all
