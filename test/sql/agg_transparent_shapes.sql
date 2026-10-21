-- Ordinary SQL shapes compose indexed JSON, lateral children, and outer aggregation.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, entity_id int, document jsonb, metrics jsonb, payload text);
INSERT INTO events
SELECT i, i%3,
 jsonb_build_object('flag', CASE WHEN i%2=0 THEN false END,
  'entries', CASE WHEN i%2=0 THEN '[{"bucket":"x","value":2}]'::jsonb
                 ELSE '[{"bucket":"x","value":8},{"bucket":"x","value":10}]'::jsonb END,
  'notes', CASE WHEN i%3=0 THEN '[{"kind":"reply"}]'::jsonb ELSE '[]'::jsonb END),
 jsonb_build_object('overall', i%10), repeat('payload',50)
FROM generate_series(1,10000) i;
CREATE INDEX events_col ON events USING sift_col(id,entity_id,document,metrics,jsonb_exists(document,'flag'))
 WITH(paths='document.flag astext, document.entries[]{bucket text,value float8}, document.notes[]{kind text}, metrics.* float8');
CREATE TABLE entities(id int, labels text[]);
INSERT INTO entities VALUES(0,ARRAY['one','one','two']), (1,ARRAY[]::text[]), (2,NULL);
VACUUM (FREEZE, ANALYZE) events;
ANALYZE entities;
-- pushed: the plan PostgreSQL costs cheapest; eligible: the plan with its own table scans off, which a Custom Scan (SiftAgg) beats only if the query is one it can run
CREATE FUNCTION check_shape(q text) RETURNS TABLE(pushed bool,eligible bool,differences bigint) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
 SET LOCAL sift.enable_agg=off;
 EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT to_jsonb(q) row FROM ('||q||') q';
 SET LOCAL sift.enable_agg=on;
 EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT to_jsonb(q) row FROM ('||q||') q';
 SELECT count(*) INTO differences FROM ((TABLE r_off EXCEPT ALL TABLE r_on) UNION ALL (TABLE r_on EXCEPT ALL TABLE r_off)) d;
 pushed:=false;
 FOR line IN EXECUTE 'EXPLAIN(COSTS OFF) '||q LOOP
  pushed:=pushed OR line LIKE '%Custom Scan (SiftAgg)%';
 END LOOP;
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  SET LOCAL enable_indexonlyscan = off;
  eligible := false;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP eligible := eligible OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
 DROP TABLE r_off,r_on;
 RETURN NEXT;
END$$;
-- Presence includes JSON false and JSON null.
SELECT * FROM check_shape($q$
 SELECT count(*) AS n FROM events f WHERE jsonb_exists(f.document,'flag')
$q$);
-- A second lateral function fans out a prior lookup's array, retaining duplicate and NULL rows.
SELECT * FROM check_shape($q$
 SELECT label, count(DISTINCT f.id) AS ids, count(*) AS n, avg(o.value) AS value
 FROM events f JOIN entities e ON e.id=f.entity_id
 CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text,value float8)
 LEFT JOIN LATERAL unnest(e.labels) label ON true GROUP BY label
$q$);
-- Correlated UNION children read the member of an outer-joined jsonb_each through casts, with their native cast semantics.
SELECT * FROM check_shape($q$
 SELECT category, count(DISTINCT f.id) AS ids,
 count(DISTINCT f.id) FILTER(WHERE jsonb_path_exists(f.document,'$.notes[*] ? (@.kind == "reply")')) AS replies
 FROM events f LEFT JOIN LATERAL jsonb_each(f.metrics) r ON r.key='overall'
 JOIN LATERAL (SELECT 'all' AS category UNION ALL SELECT CASE WHEN r.value::int>6 THEN 'high' ELSE 'low' END) c ON true
 GROUP BY category
$q$);
-- Preserve the mean of per-item means across UNION ALL arms and a preserved date relation.
SELECT * FROM check_shape($q$
 WITH item_values AS (
  SELECT f.id,'detail' AS bucket,avg(o.value) AS value FROM events f
   CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(value float8) GROUP BY f.id
  UNION ALL
  SELECT f.id,'all',avg(o.value) FROM events f
   CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(value float8) GROUP BY f.id
 )
 SELECT d.bucket,avg(v.value) AS value,count(DISTINCT v.id) AS ids
 FROM (VALUES('detail'),('all'),('empty')) d(bucket) LEFT JOIN item_values v ON v.bucket=d.bucket
 GROUP BY d.bucket
$q$);
DROP FUNCTION check_shape(text);
DROP TABLE events,entities;
DROP EXTENSION pg_sift;
