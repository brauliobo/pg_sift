-- ORDER BY sift.score(ctid) DESC, id LIMIT k: the rows after the score are sorted by an Incremental Sort that reads the whole group of rows tied with
-- the k-th and the row after it. The scan under it keeps that group itself (the finished plan tells it, planner_ties.c), so a group of any size is read by one search.
-- The answers must be those of the sort of every match.
CREATE EXTENSION pg_sift;
\ir test/sift_plan.sql

-- tie groups of every size: the score of a one-term search is a function of the length of the document, and the lengths are 3 to 8 words, many of them equal
CREATE TABLE docs (id int PRIMARY KEY, body text);
INSERT INTO docs
SELECT g, 'tie ' || repeat('pad ', g % 6) || CASE WHEN g % 50 = 0 THEN 'rare ' ELSE '' END || CASE WHEN g % 7 = 0 THEN 'seven' ELSE 'other' END
FROM generate_series(1, 20000) g;
CREATE INDEX docs_idx ON docs USING sift (body);
VACUUM ANALYZE docs;

-- the rows of the sort of every match (no limit: one search of all of them), and the searches of the scan under the sort key
CREATE FUNCTION unlimited(q text, k int) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE r int[];
BEGIN
	EXECUTE format('SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, s FROM (SELECT id, sift.score(ctid) AS s FROM docs WHERE body @@> %L) a ORDER BY s DESC, id OFFSET 0) t WHERE true', q) INTO r;
	RETURN r[1:k];
END $$;
CREATE FUNCTION hint_of(q text, k int, keys text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (COSTS OFF, FORMAT JSON) SELECT id FROM docs WHERE body @@> %L ORDER BY sift.score(ctid) DESC%s LIMIT %s', q, keys, k) INTO j;
	RETURN (SELECT (n ->> 'Limit Hint')::bigint FROM jsonb_path_query(j, 'strict $.**') n WHERE n ->> 'Custom Plan Provider' = 'SiftScan' LIMIT 1);
END $$;

CREATE TABLE cases (q text, k int);
INSERT INTO cases VALUES ('tie', 10), ('tie', 100), ('tie pad', 20), ('seven', 10), ('rare', 10), ('rare seven', 5), ('pad OR rare', 10), ('tie AND seven', 50), ('other', 1), ('tie', 1), ('tie', 2000);

-- the sorted rows are the answer, whatever the size of the group at the boundary (the plan sorts incrementally, from the score)
SELECT sift_plan('SELECT id FROM docs WHERE body @@> ''tie'' ORDER BY sift.score(ctid) DESC, id LIMIT 10') LIKE '%Incremental Sort%' AS incremental;
SELECT c.q, c.k FROM cases c, LATERAL (SELECT array_agg(id) AS ids FROM (SELECT id FROM docs WHERE body @@> c.q ORDER BY sift.score(ctid) DESC, id LIMIT c.k) t) r
WHERE r.ids IS DISTINCT FROM unlimited(c.q, c.k) ORDER BY 1, 2;

-- the scan under the sort key keeps the group tied with the k-th itself (rank_ties_group): its hint is the limit, as for a scan the query ends on
SELECT k, hint_of('tie', k, ', id') AS sorted, hint_of('tie', k, '') AS alone FROM (VALUES (1), (10), (100), (1000)) v(k) ORDER BY 1;

DROP FUNCTION unlimited(text, int), hint_of(text, int, text), sift_plan(text);
DROP TABLE docs, cases;
DROP EXTENSION pg_sift;
