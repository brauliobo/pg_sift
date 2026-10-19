-- sift.sections and sift.field_query (docs/FIELDS-FACETS.md): the sections of one key, searched with per-field weights at query time.
-- A leaf of the weighted query is the OR of that leaf in each weighted field with the weight as boost, so a row scores the sum over the
-- fields of the BM25 of the field's own terms (document frequency and term frequency of the field, length of the whole row):
--   score = sum over the leaves of the query, over the weighted fields f, of
--           w_f * idf_f * (k1 + 1) * tf_f / (tf_f + k1 * (1 - b + b * dl / avgdl))     idf_f = ln(1 + (N - df_f + 0.5) / (df_f + 0.5))
-- The score of sift is compared with that formula computed in SQL from the tokens of each section, the top k of the ranked scan with the
-- whole ranking (the bounds that stop a ranked scan early must hold with weights below and above 1), and the match sets with the index,
-- the document evaluation and the unweighted queries.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE fw (id int PRIMARY KEY, title text, body text, tags text, pad text DEFAULT repeat('x', 400));
ALTER TABLE fw ALTER COLUMN pad SET STORAGE PLAIN;
CREATE FUNCTION fw_words(seed int, n int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg((ARRAY['alpha','beta','gamma','delta','omega','sigma','kappa','zeta','theta','lambda','rho','tau','phi','psi','chi','mu',
							 'nu','xi','pi','eta'])[1 + (hashint4(seed * 37 + w) & 2147483647) % 20], ' ')
	FROM generate_series(1, n) w $$;
INSERT INTO fw
SELECT i, fw_words(i, 1 + i % 3), fw_words(i + 7919, 4 + i % 9), CASE WHEN i % 5 = 0 THEN NULL ELSE fw_words(i + 104729, 1 + i % 4) END
FROM generate_series(1, 4000) i;
CREATE INDEX fw_idx ON fw USING sift (sift.sections(title, body, tags)) WITH (fields = 'title,body,tags', write_buffer_limit = 256);
CREATE INDEX fw_plain ON fw USING sift (body);
VACUUM (ANALYZE) fw;
SELECT sift.verify('fw_idx', true);

-- the sections, with NULL as an empty section
SELECT replace(sift.sections('a', NULL, 'c'), chr(29), '|') AS sec, sift.sections('x') AS one;

-- the reference: the tokens of every section, the length of every row, the statistics of the index (k1 = 1.2, b = 0.75)
CREATE TABLE fw_tok AS
SELECT t.id, s.f, tk.token, count(*)::int AS tf
FROM fw t, LATERAL (VALUES ('title', t.title), ('body', t.body), ('tags', t.tags)) s(f, txt), LATERAL sift.tokenize(coalesce(s.txt, '')) tk
GROUP BY 1, 2, 3;
CREATE TABLE fw_dl AS SELECT t.id, (SELECT count(*) FROM sift.tokenize(sift.sections(t.title, t.body, t.tags)))::int AS dl FROM fw t;
CREATE TABLE fw_df AS SELECT f, token, count(*) AS df FROM fw_tok GROUP BY 1, 2;

-- the rows and the scores of a query of words, ANDed or ORed, over fields with weights
CREATE FUNCTION fw_ref(terms text[], fields text[], weights float8[], conj bool DEFAULT true) RETURNS TABLE (id int, score float8) LANGUAGE sql AS $$
	WITH st AS (SELECT count(*)::float8 AS n, avg(dl::float8) AS avgdl FROM fw_dl),
	     wf AS (SELECT f, w FROM unnest(fields, weights) u(f, w)),
	     leaf AS (
		SELECT k.id, k.token,
			   sum(wf.w * ln(1 + (st.n - d.df + 0.5) / (d.df + 0.5)) * 2.2 * k.tf / (k.tf + 1.2 * (1 - 0.75 + 0.75 * dl.dl / st.avgdl))) AS sc
		FROM fw_tok k JOIN wf USING (f) JOIN fw_df d USING (f, token) JOIN fw_dl dl USING (id), st
		WHERE k.token = ANY (terms) GROUP BY k.id, k.token)
	SELECT leaf.id, sum(sc) FROM leaf GROUP BY leaf.id HAVING NOT conj OR count(*) = array_length(terms, 1) $$;

-- sift's rows and scores of the same query; the plans that are not the ranked scan are disabled
CREATE FUNCTION fw_sift(q text) RETURNS TABLE (id int, score float8) LANGUAGE plpgsql
	SET enable_seqscan = off SET enable_bitmapscan = off SET enable_indexscan = off AS $$
BEGIN
	RETURN QUERY EXECUTE format('SELECT t.id, sift.score(t.ctid)::float8 FROM fw t WHERE sift.sections(title, body, tags) @@> %L ORDER BY 2 DESC, t.ctid', q);
END $$;

-- rows only in one and scores that differ by more than float4 rounding (a relative 1e-4)
CREATE FUNCTION fw_diff(q text, terms text[], fields text[], weights float8[], conj bool DEFAULT true) RETURNS text LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE r.id IS NULL) || ' only sift, ' || count(*) FILTER (WHERE s.id IS NULL) || ' only reference, ' ||
		   count(*) FILTER (WHERE abs(s.score - r.score) > 1e-4 * r.score) || ' metrics_a differ, ' || count(s.id) || ' rows'
	FROM fw_sift(q) s FULL JOIN fw_ref(terms, fields, weights, conj) r USING (id) $$;

SELECT sift.field_query('alpha', 'title^3, body') AS q1 \gset
SELECT :'q1' AS query, fw_diff(:'q1', '{alpha}', '{title,body}', '{3,1}');
SELECT q, fw_diff(q, terms, f, w) AS diff FROM (VALUES
	(sift.field_query('alpha', 'title^3, body, tags^0.5'), '{alpha}'::text[], '{title,body,tags}'::text[], '{3,1,0.5}'::float8[]),
	(sift.field_query('omega', 'title^0.25, body^40'), '{omega}', '{title,body}', '{0.25,40}'),
	(sift.field_query('alpha beta', 'title^2, tags^5'), '{alpha,beta}', '{title,tags}', '{2,5}'),
	(sift.field_query('gamma sigma', 'title, body^3'), '{gamma,sigma}', '{title,body}', '{1,3}'),
	(sift.field_query('rho', 'tags'), '{rho}', '{tags}', '{1}')) v(q, terms, f, w);
SELECT q, fw_diff(q, terms, f, w, false) AS diff FROM (VALUES
	(sift.field_query('{alpha beta}', 'title^3, body, tags^0.5'), '{alpha,beta}'::text[], '{title,body,tags}'::text[], '{3,1,0.5}'::float8[]),
	(sift.field_query('kappa OR zeta OR theta', 'title^7, body^0.1'), '{kappa,zeta,theta}', '{title,body}', '{7,0.1}')) v(q, terms, f, w);
-- a boost on a leaf multiplies the weight
SELECT fw_diff(sift.field_query('alpha^4', 'title^3, body'), '{alpha}', '{title,body}', '{12,4}') AS boost_times_weight;

-- all the sections with the weight 1 match what the plain query matches
CREATE FUNCTION fw_cnt(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM fw WHERE sift.sections(title, body, tags) @@> %L', q) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION fw_seq(q text) RETURNS bigint LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM fw WHERE sift.sections(title, body, tags) @@> %L', q) INTO n;
	RETURN n;
END $$;
SELECT q, fw_cnt(q) AS plain, fw_cnt(sift.field_query(q, 'title, body, tags')) AS weighted, fw_seq(sift.field_query(q, 'title, body, tags')) AS seq,
	   fw_cnt(sift.field_query(q, 'title^5, body^0.5, tags^2')) AS other_weights,
	   fw_cnt(sift.field_query(q, 'title, body')) AS two_fields, fw_seq(sift.field_query(q, 'title, body')) AS two_seq
FROM unnest(ARRAY['alpha', 'alpha beta', 'alp*', 'alpha OR zeta', 'a?pha', 'alphx~1', 'alpha NOT beta', '(alpha OR beta) AND gamma',
				  '"alpha beta"', 'alpha "beta gamma"', '/al.ha/', 'tags:alpha', 'title:(alpha OR beta) gamma', 'title:alp* body:bet*']) q;

-- the top k of the ranked scan is the head of the whole ranking, for weights that make a field dominate or vanish
CREATE FUNCTION fw_topk(q text, k int) RETURNS boolean LANGUAGE plpgsql
	SET enable_seqscan = off SET enable_bitmapscan = off SET enable_indexscan = off AS $$
DECLARE top int[]; whole int[];
BEGIN
	EXECUTE format('SELECT array_agg(id) FROM (SELECT id FROM fw WHERE sift.sections(title, body, tags) @@> %1$L ORDER BY sift.score(ctid) DESC LIMIT %2$s) s', q, k) INTO top;
	EXECUTE format('SELECT array_agg(id) FROM (SELECT id FROM (SELECT id, sift.score(ctid) sc, ctid FROM fw WHERE sift.sections(title, body, tags) @@> %1$L ORDER BY sc DESC, ctid) a LIMIT %2$s) s', q, k) INTO whole;
	RETURN top IS NOT DISTINCT FROM whole;
END $$;
SELECT q, fw_topk(sift.field_query(q, w), k) AS topk
FROM unnest(ARRAY['alpha', 'alpha beta', 'omega OR sigma', 'kappa zeta', 'lambda OR rho OR tau']) q,
     unnest(ARRAY['title^50, body^0.01', 'title^0.01, tags^100', 'title^3, body, tags^0.5', 'body^10000']) w, unnest(ARRAY[1, 5, 25]) k
WHERE NOT fw_topk(sift.field_query(q, w), k);
SELECT count(*) AS topk_checks FROM unnest(ARRAY['alpha', 'alpha beta', 'omega OR sigma', 'kappa zeta', 'lambda OR rho OR tau']) q,
     unnest(ARRAY['title^50, body^0.01', 'title^0.01, tags^100', 'title^3, body, tags^0.5', 'body^10000']) w, unnest(ARRAY[1, 5, 25]) k;

-- the order is by weight: a title hit beats a body hit
SELECT s.id, left(t.title, 20) AS title, round(s.score::numeric, 3) AS score FROM fw_sift(sift.field_query('zeta', 'title^20, body')) s JOIN fw t USING (id) LIMIT 4;
SELECT s.id, left(t.title, 20) AS title, round(s.score::numeric, 3) AS score FROM fw_sift(sift.field_query('zeta', 'title^0.05, body')) s JOIN fw t USING (id) LIMIT 4;

-- the fields are the index's
SELECT sift.field_query('alpha', 'title^3, nope', 'fw_idx');
SELECT sift.field_query('alpha', 'title^3', 'fw_plain');
SELECT sift.parse_query(sift.field_query('alpha bet*', 'title^3, body'), 'fw_idx') AS canonical;

-- after writes and a merge
INSERT INTO fw SELECT i, fw_words(i, 2), fw_words(i + 7919, 6), 'alpha tag' FROM generate_series(4001, 4400) i;
DELETE FROM fw WHERE id % 11 = 0;
UPDATE fw SET title = 'alpha moved' WHERE id % 13 = 0;
SELECT sift.promote('fw_idx') >= 0 AS promoted, sift.merge('fw_idx', true) >= 0 AS merged;
SELECT fw_topk(sift.field_query('alpha beta', 'title^3, body, tags^0.5'), 10) AS topk_after_writes,
	   fw_cnt(sift.field_query('alpha', 'title, body, tags')) = fw_cnt('alpha') AS same_matches,
	   fw_seq(sift.field_query('alpha', 'title^3, body')) = fw_cnt(sift.field_query('alpha', 'title^3, body')) AS seq_agrees;
SELECT sift.verify('fw_idx', true);
DROP TABLE fw, fw_tok, fw_dl, fw_df;
DROP FUNCTION fw_words(int, int), fw_ref(text[], text[], float8[], bool), fw_sift(text), fw_diff(text, text[], text[], float8[], bool),
	fw_cnt(text), fw_seq(text), fw_topk(text, int);
DROP EXTENSION pg_sift;
