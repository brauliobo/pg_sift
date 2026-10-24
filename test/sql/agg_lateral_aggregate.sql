-- `x LEFT JOIN LATERAL (SELECT avg(e) FROM f(x.doc) WHERE ..) s ON true` is one row for each row of x: the scan runs it as the grouping of the elements by the
-- key of x, when x holds each key once. Where it does not (a join that repeats the keys, a count(*), an argument that is not NULL for a row that is not there)
-- the query stays as written. The answers are the normal plan's.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE docs (id int PRIMARY KEY, grp int NOT NULL, dim_id int NOT NULL, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO docs
SELECT i, i % 7, 1 + i % 5,
       CASE WHEN i % 11 = 0 THEN NULL WHEN i % 13 = 0 THEN '{}'::jsonb
            ELSE jsonb_build_object('weight', i % 10, 'parts', coalesce((SELECT jsonb_agg(jsonb_build_object('score', CASE WHEN (i + j) % 5 = 0 THEN NULL ELSE (i * j) % 10 END)) FROM generate_series(1, i % 5) j), '[]'::jsonb)) END
FROM generate_series(1, 6000) i;
CREATE INDEX docs_col ON docs USING sift_col (id, grp, dim_id, doc) WITH (paths = 'doc.parts[]{score float8}, doc.weight float8');
CREATE TABLE dims (id int PRIMARY KEY, kind text) WITH (autovacuum_enabled = false);
INSERT INTO dims SELECT i, 'k' || i % 3 FROM generate_series(1, 5) i;
CREATE TABLE tags (dim_id int, tag text) WITH (autovacuum_enabled = false);
INSERT INTO tags SELECT 1 + i % 5, 't' || i FROM generate_series(1, 12) i;
VACUUM (FREEZE, ANALYZE) docs;
VACUUM (FREEZE, ANALYZE) dims;
VACUUM (FREEZE, ANALYZE) tags;

\ir test/agg_helpers.sql
-- the plan reads what the pattern names in no node but the scan (docs, by default)
CREATE FUNCTION reads_plan(q text, pat text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; found bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP found := found OR l ~ pat; END LOOP;
	RETURN found;
END $$;
CREATE FUNCTION check_query(name text, q text, pat text DEFAULT '(Seq Scan|Index Scan|Index Only Scan|Bitmap Heap Scan) on docs') RETURNS TABLE (label text, only_scan bool, same bool, outcome text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT name, runs_in_scan(q) AND NOT reads_plan(q, pat), outcome(q, false) = outcome(q, true), left(outcome(q, true), 100);
END $$;

-- one row for a document: the average of its parts that have a score
SELECT * FROM check_query('average of the parts', $q$
 SELECT r.grp, count(r.id) AS n, round(avg(r.sv)::numeric, 6) AS sv
 FROM (SELECT d.id, d.grp, s.sv FROM docs d
       LEFT JOIN LATERAL (SELECT avg(o.score) AS sv FROM jsonb_to_recordset(d.doc['parts']) o(score float8) WHERE o.score IS NOT NULL) s ON true) r
 GROUP BY r.grp
$q$);
-- several aggregates, a constant, an expression of the document, a filter of the elements by a column of the document
SELECT * FROM check_query('sum min max count, a constant, an expression of the row', $q$
 SELECT r.grp, count(r.id) AS n, round(avg(r.weight)::numeric, 6) AS weight, sum(r.total) AS total, min(r.lo) AS lo, max(r.hi) AS hi, sum(r.parts) AS parts, count(r.nothing) AS nothing
 FROM (SELECT d.id, d.grp, CAST(d.doc ->> 'weight' AS float8) AS weight, s.total, s.lo, s.hi, s.parts, s.nothing FROM docs d
       LEFT JOIN LATERAL (SELECT sum(o.score) AS total, min(o.score) AS lo, max(o.score * 2) AS hi, count(o.score) AS parts, CAST(NULL AS float8) AS nothing
                          FROM jsonb_to_recordset(d.doc['parts']) o(score float8) WHERE o.score >= d.grp) s ON true) r
 GROUP BY r.grp
$q$);
-- tables joined by their keys, a CTE that has each key once, a sub-select that keeps some documents (a sublink in its WHERE)
SELECT * FROM check_query('joined by the key of the tables', $q$
 WITH picked AS (SELECT DISTINCT id FROM dims WHERE kind <> 'k2')
 SELECT r.kind, count(r.id) AS n, round(avg(r.sv)::numeric, 6) AS sv
 FROM (SELECT d.id, d.kind, s.sv
       FROM (SELECT docs.id, docs.doc, m.kind FROM docs INNER JOIN picked p ON p.id = docs.dim_id INNER JOIN dims m ON m.id = docs.dim_id
             WHERE docs.grp IN (SELECT grp FROM docs WHERE id < 100)) d
       LEFT JOIN LATERAL (SELECT avg(o.score) AS sv FROM jsonb_to_recordset(d.doc['parts']) o(score float8)) s ON true) r
 GROUP BY r.kind
$q$);
-- tags repeat the keys of the documents: a document is as many rows as it has tags, and the grouping by the document would make them one
SELECT * FROM check_query('a join that repeats the keys', $q$
 SELECT r.grp, count(r.id) AS n, round(avg(r.sv)::numeric, 6) AS sv
 FROM (SELECT d.id, d.grp, s.sv FROM (SELECT docs.id, docs.grp, docs.doc FROM docs INNER JOIN tags t ON t.dim_id = docs.dim_id) d
       LEFT JOIN LATERAL (SELECT avg(o.score) AS sv FROM jsonb_to_recordset(d.doc['parts']) o(score float8)) s ON true) r
 GROUP BY r.grp
$q$);
-- count(*) is 1 for the row of an outer join that has no element, and 0 for an aggregate of none
SELECT * FROM check_query('count of rows', $q$
 SELECT r.grp, count(r.id) AS n, sum(r.c) AS c
 FROM (SELECT d.id, d.grp, s.c FROM docs d
       LEFT JOIN LATERAL (SELECT count(*) AS c FROM jsonb_to_recordset(d.doc['parts']) o(score float8) WHERE o.score IS NOT NULL) s ON true) r
 GROUP BY r.grp
$q$);
-- a coalesce of the argument is a value for the row that is not there
SELECT * FROM check_query('a non-strict argument', $q$
 SELECT r.grp, count(r.id) AS n, round(avg(r.sv)::numeric, 6) AS sv
 FROM (SELECT d.id, d.grp, s.sv FROM docs d
       LEFT JOIN LATERAL (SELECT avg(coalesce(o.score, 0)) AS sv FROM jsonb_to_recordset(d.doc['parts']) o(score float8)) s ON true) r
 GROUP BY r.grp
$q$);
-- a sublink holds the lateral: the query around it reads docs in an ordinary plan, and the sublink's own plan is the scan that groups the elements
SELECT * FROM check_query('a sublink', $q$
 SELECT docs.id, docs.grp FROM docs
 WHERE docs.id IN (SELECT r.id FROM (SELECT d.id, s.sv FROM docs d
                                    LEFT JOIN LATERAL (SELECT avg(o.score) AS sv FROM jsonb_to_recordset(d.doc['parts']) o(score float8) WHERE o.score IS NOT NULL) s ON true) r
                   WHERE r.sv > 4)
 ORDER BY docs.id
$q$, 'Function Scan');
-- a sublink of a value (an InitPlan)
SELECT * FROM check_query('a sublink of a value', $q$
 SELECT docs.id, docs.dim_id FROM docs
 WHERE docs.dim_id < (SELECT max(r.sv) FROM (SELECT d.id, s.sv FROM docs d
                                           LEFT JOIN LATERAL (SELECT avg(o.score) AS sv FROM jsonb_to_recordset(d.doc['parts']) o(score float8)) s ON true) r)
 ORDER BY docs.id
$q$, 'Function Scan');
-- the same sublink over documents repeated by their tags
SELECT * FROM check_query('a sublink over a join that repeats the keys', $q$
 SELECT docs.id, docs.grp FROM docs
 WHERE docs.id IN (SELECT r.id FROM (SELECT d.id, s.sv FROM (SELECT docs.id, docs.doc FROM docs INNER JOIN tags t ON t.dim_id = docs.dim_id) d
                                    LEFT JOIN LATERAL (SELECT avg(o.score) AS sv FROM jsonb_to_recordset(d.doc['parts']) o(score float8) WHERE o.score IS NOT NULL) s ON true) r
                   WHERE r.sv > 4)
 ORDER BY docs.id
$q$, 'Function Scan');

DROP TABLE docs, dims, tags;
DROP FUNCTION reads_plan(text,text), check_query(text,text,text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
DROP EXTENSION pg_sift;
