-- jsonb_exists(doc, 'k') of the document whose list is unnested: the rows of the list exist only for an object (the list is read from it), whose
-- key the index holds as the type of the member k (a JSON null member exists, a missing one does not). Elsewhere a document may be an array
-- holding the string or the string itself, which the index does not tell: those queries stay with PostgreSQL.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
CREATE TABLE oe_docs (id int PRIMARY KEY, owner_id int, doc jsonb);
INSERT INTO oe_docs SELECT i, i % 5,
  CASE i % 9
    WHEN 0 THEN jsonb_build_object('mapped', true, 'items', jsonb_build_array(jsonb_build_object('category', 'a', 'score', i % 10), jsonb_build_object('category', 'b', 'score', 3)))
    WHEN 1 THEN jsonb_build_object('mapped', NULL, 'items', jsonb_build_array(jsonb_build_object('category', 'a', 'score', 5)))
    WHEN 2 THEN jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'b', 'score', i % 7)))
    WHEN 3 THEN jsonb_build_array('mapped', jsonb_build_object('items', 1))
    WHEN 4 THEN to_jsonb('mapped'::text)
    WHEN 5 THEN 'null'::jsonb
    WHEN 6 THEN to_jsonb(i)
    WHEN 7 THEN NULL
    ELSE jsonb_build_object('mapped', 'x', 'items', '[]'::jsonb)
  END
FROM generate_series(1, 4500) i;
CREATE INDEX oe_docs_col ON oe_docs USING sift_col (id, owner_id, doc) WITH (paths = 'doc.mapped astext, doc.items[]{category text, score float8}');
VACUUM (FREEZE, ANALYZE) oe_docs;

CREATE FUNCTION check_query(q text) RETURNS TABLE (pushed bool, differences bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = on;
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
  pushed := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  DROP TABLE r_off, r_on;
  RETURN NEXT;
END $$;

-- a key and an aggregate argument that depend on the key of the document, for the rows of its list
SELECT * FROM check_query($q$
 SELECT d.owner_id, CASE WHEN jsonb_exists(d.doc, 'mapped') THEN o.category ELSE 'raw' END AS category, count(*) AS n, avg(o.score) AS score
 FROM oe_docs d CROSS JOIN LATERAL jsonb_to_recordset(d.doc['items']) AS o(category text, score float8) GROUP BY 1, 2
$q$);
-- a filter
SELECT * FROM check_query($q$
 SELECT o.category, count(*) FILTER (WHERE NOT jsonb_exists(d.doc, 'mapped')) AS raw, count(*) AS n
 FROM oe_docs d CROSS JOIN LATERAL jsonb_to_recordset(d.doc['items']) AS o(category text, score float8) GROUP BY 1
$q$);
-- the documents themselves: an array or a string holding the key exists, the index cannot tell
SELECT * FROM check_query($q$
 SELECT d.owner_id, jsonb_exists(d.doc, 'mapped') AS has_key, count(*) AS n FROM oe_docs d GROUP BY 1, 2
$q$);

DROP FUNCTION check_query(text);
DROP TABLE oe_docs;
DROP EXTENSION pg_sift;
