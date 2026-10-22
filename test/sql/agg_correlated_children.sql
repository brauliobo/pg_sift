-- Correlated LIMIT 1 lookups retain native row selection and per-item weighting.
-- Native correlated children are rescanned for each distinct indexed input tuple.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, kind text, document jsonb, payload text);
INSERT INTO events
SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END,
       jsonb_build_object('flag', CASE WHEN i % 3 = 0 THEN NULL ELSE false END,
                          'entries', CASE WHEN i % 2 = 0
                            THEN '[{"bucket":"x","value":2}]'::jsonb
                            ELSE '[{"bucket":"x","value":8},{"bucket":"x","value":10}]'::jsonb END),
       repeat('payload', 50)
FROM generate_series(1, 10000) i;
INSERT INTO events VALUES (10001, 'a', '{"entries":[]}', ''), (10002, NULL, NULL, ''),
 (10003, NULL, '{"entries":[{"bucket":"x","value":3}]}', ''),
 (10004, 'a', '{"entries":[{"bucket":"unmatched","value":4}]}', ''),
 (10005, 'c', '{"entries":[{"bucket":null,"value":6}]}', '');
CREATE INDEX events_col ON events USING sift_col (id, kind, document)
 WITH (paths = 'document.flag astext, document.entries[]{bucket text,value float8}');
CREATE TABLE lookup_values (kind text, bucket text, label text);
INSERT INTO lookup_values VALUES ('a', 'x', 'first'), ('a', 'x', 'second'), ('a', 'x', 'third'), ('b', 'x', 'other');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE lookup_values;
\ir test/agg_check_pushed.sql
-- Correlation itself uses the existing native LIMIT path, including duplicate candidates.
SELECT * FROM check_query($q$
 SELECT coalesce(m.label, o.bucket) AS bucket, count(*) AS n, avg(o.value) AS value
 FROM events f CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
 LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind=f.kind AND m.bucket=o.bucket LIMIT 1) m ON true
 WHERE o.value IS NOT NULL GROUP BY 1
$q$);
-- ORDER BY, OFFSET, zero matches, and multi-row fanout stay inside PostgreSQL's child.
SELECT * FROM check_query($q$
 SELECT m.label, count(*) AS n, count(DISTINCT f.id) AS ids, sum(o.value) AS value
 FROM events f CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
 LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind=f.kind AND m.bucket=o.bucket
   ORDER BY label DESC LIMIT 2 OFFSET 1) m ON true
 GROUP BY m.label
$q$);
SELECT * FROM check_query($q$
 SELECT coalesce(m.label, o.bucket) AS bucket, count(*) AS n, avg(o.value) AS value
 FROM events f CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
 LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind=f.kind AND m.bucket=o.bucket LIMIT 1) m ON true
 WHERE jsonb_exists(f.document, 'flag') AND o.value IS NOT NULL GROUP BY 1
$q$);
SELECT * FROM check_query($q$
 WITH entries AS (
   SELECT f.id, coalesce(m.label, o.bucket) AS bucket, o.value
   FROM events f CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
   LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind=f.kind AND m.bucket=o.bucket LIMIT 1) m ON true
   WHERE jsonb_exists(f.document, 'flag') AND o.value IS NOT NULL
 ), levels AS (
   SELECT id, bucket, avg(value) AS value FROM entries GROUP BY id, bucket
   UNION ALL SELECT id, 'all-flag', avg(value) FROM entries GROUP BY id
   UNION ALL SELECT f.id, 'all', avg(o.value) FROM events f
     CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
     WHERE o.value IS NOT NULL GROUP BY f.id
 )
 SELECT bucket, avg(value) AS value, count(*) AS n FROM levels GROUP BY bucket
$q$);
-- A correlated child can read an earlier lookup's output, retaining that lookup's duplicate fanout.
CREATE TABLE kinds (kind text, lookup_kind text);
INSERT INTO kinds VALUES ('a', 'a'), ('a', 'a'), ('b', 'b');
ANALYZE kinds;
SELECT * FROM check_query($q$
 SELECT coalesce(m.label, o.bucket) AS bucket, count(*) AS n, count(DISTINCT f.id) AS ids
 FROM events f JOIN kinds k ON k.kind=f.kind
 CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
 LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind=k.lookup_kind AND m.bucket=o.bucket
   ORDER BY label LIMIT 1) m ON true GROUP BY 1
$q$);
DROP TABLE kinds;
-- Native external parameters remain visible inside the child across executions.
PREPARE correlated_parameter(text) AS
 SELECT m.label, count(*) AS n FROM events f
 LEFT JOIN LATERAL (SELECT label FROM lookup_values m WHERE m.kind=f.kind AND m.bucket=$1 ORDER BY label LIMIT 1) m ON true
 GROUP BY m.label ORDER BY m.label NULLS FIRST;
SET sift.enable_agg=off;
EXECUTE correlated_parameter('x'); EXECUTE correlated_parameter('absent');
SET sift.enable_agg=on;
EXECUTE correlated_parameter('x'); EXECUTE correlated_parameter('absent');
DEALLOCATE correlated_parameter;
-- A child expression runs for eligible correlated inputs, preserving native errors.
SELECT * FROM check_query($q$
 SELECT sum(m.value) AS value FROM events f LEFT JOIN LATERAL (
   SELECT 10 / CASE m.kind WHEN 'a' THEN 0 ELSE 1 END AS value
   FROM lookup_values m WHERE m.kind=f.kind LIMIT 1
 ) m ON true WHERE f.kind='b'
$q$);
DO $$
DECLARE mode text;
BEGIN
  FOREACH mode IN ARRAY ARRAY['off', 'on'] LOOP
    PERFORM set_config('sift.enable_agg', mode, true);
    BEGIN
      EXECUTE $q$SELECT sum(m.value) FROM events f LEFT JOIN LATERAL (
        SELECT 10 / CASE m.kind WHEN 'a' THEN 0 ELSE 1 END AS value
        FROM lookup_values m WHERE m.kind=f.kind LIMIT 1
      ) m ON true$q$;
      RAISE EXCEPTION 'expected division_by_zero with aggregation %', mode;
    EXCEPTION WHEN division_by_zero THEN NULL;
    END;
  END LOOP;
END $$;
-- Averaging per-item means gives 5.5, while averaging all entries gives 20/3.
-- A flag key whose value is false or null is still present.
SELECT avg(value) AS item_mean, count(*) AS items FROM (
 SELECT f.id, avg(o.value) AS value FROM events f
 CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(value float8)
 WHERE jsonb_exists(f.document, 'flag') AND o.value IS NOT NULL GROUP BY f.id
) per_item;
-- A dictionary of thousands of distinct texts of which the scan reads a few: an entry gets its id when a tuple first reads it.
CREATE TABLE wide_events (id int, kind text, document jsonb, payload text);
INSERT INTO wide_events
SELECT i, 'a', jsonb_build_object('entries', jsonb_build_array(jsonb_build_object('bucket', 'free text ' || i, 'value', i % 10))), repeat('payload', 50)
FROM generate_series(1, 6000) i;
CREATE INDEX wide_events_col ON wide_events USING sift_col (id, kind, document) WITH (paths = 'document.entries[]{bucket text,value float8}');
CREATE TABLE wide_lookup AS SELECT 'a' AS kind, 'free text ' || i AS bucket, 'label ' || (i % 7) AS label FROM generate_series(1, 6000, 3) i;
VACUUM (FREEZE, ANALYZE) wide_events;
ANALYZE wide_lookup;
SELECT * FROM check_query($q$
 SELECT coalesce(m.label, 'none') AS label, count(*) AS n, sum(o.value) AS value
 FROM wide_events f CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
 LEFT JOIN LATERAL (SELECT label FROM wide_lookup m WHERE m.kind = f.kind AND m.bucket = o.bucket LIMIT 1) m ON true
 WHERE f.id % 500 = 7 GROUP BY 1
$q$);
SELECT * FROM check_query($q$
 SELECT coalesce(m.label, 'none') AS label, count(*) AS n, sum(o.value) AS value
 FROM wide_events f CROSS JOIN LATERAL jsonb_to_recordset(f.document->'entries') o(bucket text, value float8)
 LEFT JOIN LATERAL (SELECT label FROM wide_lookup m WHERE m.kind = f.kind AND m.bucket = o.bucket LIMIT 1) m ON true
 GROUP BY 1
$q$);
DROP FUNCTION check_query(text);
DROP TABLE events, lookup_values, wide_events, wide_lookup;
DROP EXTENSION pg_sift;
