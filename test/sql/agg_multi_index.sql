-- A table with several sift_col indexes is scanned through the first of them that gives a plan: each index holds its own columns and paths, and the one
-- PostgreSQL lists first is no better a choice than the others for a query it cannot answer. The results, with the pushdown off and on, are the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE FUNCTION runq(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;

-- the index the pushdown scans, or native when the plan has no SiftAgg
CREATE FUNCTION used_index(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '^\s*Index: ' THEN RETURN btrim(l); END IF;
	END LOOP;
	RETURN 'native';
END $$;

CREATE TABLE mi (id int PRIMARY KEY, grp int, amount float8, doc jsonb);
INSERT INTO mi SELECT i, i % 7, i % 100, jsonb_build_object('n', i % 13, 'kind', 'k' || i % 3) FROM generate_series(1, 20000) i;
CREATE INDEX mi_amount ON mi USING sift_col (id, grp, amount);
CREATE INDEX mi_doc ON mi USING sift_col (id, grp, doc) WITH (paths = 'doc.n float8, doc.kind text');
VACUUM ANALYZE mi;

-- each query only one of the two indexes can answer, whichever the planner lists first
SELECT used_index('SELECT grp, sum(amount) FROM mi GROUP BY grp') AS amount_only;
SELECT same('SELECT grp, sum(amount), count(*) FROM mi GROUP BY grp') AS amount_only_same;
SELECT used_index('SELECT grp, sum((doc ->> $$n$$)::float8) FROM mi GROUP BY grp') AS doc_only;
SELECT same('SELECT grp, sum((doc ->> $$n$$)::float8), count(*) FROM mi GROUP BY grp') AS doc_only_same;
SELECT used_index('SELECT doc ->> $$kind$$, count(*) FROM mi WHERE grp < 4 GROUP BY 1') AS doc_key;
SELECT same('SELECT doc ->> $$kind$$, count(*) FROM mi WHERE grp < 4 GROUP BY 1') AS doc_key_same;
-- both indexes hold the columns: the first is scanned, the answer is the same
SELECT used_index('SELECT grp, count(*) FROM mi GROUP BY grp') IN ('Index: mi_amount', 'Index: mi_doc') AS both_hold;
SELECT same('SELECT grp, count(*) FROM mi GROUP BY grp') AS both_hold_same;
-- no index holds both columns: the normal plan
SELECT used_index('SELECT grp, sum(amount), sum((doc ->> $$n$$)::float8) FROM mi GROUP BY grp') AS neither;
SELECT same('SELECT grp, sum(amount), sum((doc ->> $$n$$)::float8) FROM mi GROUP BY grp') AS neither_same;

-- a join to a dimension table keeps its lookup on either index
CREATE TABLE mi_dim (id int PRIMARY KEY, label text);
INSERT INTO mi_dim SELECT g, 'g' || g FROM generate_series(0, 6) g;
ANALYZE mi_dim;
SELECT used_index('SELECT d.label, sum(m.amount) FROM mi m JOIN mi_dim d ON d.id = m.grp GROUP BY 1') AS joined_amount;
SELECT same('SELECT d.label, sum(m.amount) FROM mi m JOIN mi_dim d ON d.id = m.grp GROUP BY 1') AS joined_amount_same;
SELECT used_index('SELECT d.label, sum((m.doc ->> $$n$$)::float8) FROM mi m JOIN mi_dim d ON d.id = m.grp GROUP BY 1') AS joined_doc;
SELECT same('SELECT d.label, sum((m.doc ->> $$n$$)::float8) FROM mi m JOIN mi_dim d ON d.id = m.grp GROUP BY 1') AS joined_doc_same;

-- rows written after the build, and the index that went away
INSERT INTO mi SELECT i, i % 7, i % 100, jsonb_build_object('n', i % 13, 'kind', 'k' || i % 3) FROM generate_series(20001, 20500) i;
SELECT same('SELECT grp, sum(amount), count(*) FROM mi GROUP BY grp') AS amount_buffer_same;
SELECT same('SELECT grp, sum((doc ->> $$n$$)::float8), count(*) FROM mi GROUP BY grp') AS doc_buffer_same;
DROP INDEX mi_doc;
SELECT used_index('SELECT grp, sum(amount) FROM mi GROUP BY grp') AS after_drop;
SELECT used_index('SELECT grp, sum((doc ->> $$n$$)::float8) FROM mi GROUP BY grp') AS after_drop_doc;

DROP FUNCTION same(text);
DROP FUNCTION runq(text, bool);
DROP FUNCTION used_index(text);
DROP TABLE mi, mi_dim;
DROP EXTENSION pg_sift;
