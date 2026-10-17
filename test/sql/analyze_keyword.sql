-- analyzer = keyword: the whole value is one term, folded like a word or kept byte for byte
CREATE EXTENSION pg_sift;

CREATE TABLE k (id int, v text);
INSERT INTO k VALUES (1, 'Alpha'), (2, 'alpha'), (3, 'CAFÉ'), (4, 'cafe'), (5, 'gamma delta'), (6, ''), (7, NULL),
	(8, '  '), (9, 'a  b'), (10, 'Straße'), (11, 'strasse'), (12, 'a-b_c.d');
CREATE INDEX k_fold ON k USING sift (v) WITH (analyzer = keyword);
CREATE INDEX k_exact ON k USING sift (v) WITH (analyzer = keyword, case_folding = false, accent_folding = false);
CREATE INDEX k_case ON k USING sift (v) WITH (analyzer = keyword, accent_folding = false);

-- tokens: one per value, none for an empty or NULL one; offsets cover the value
SELECT id, '[' || v || ']' AS v, '[' || f.token || ']' AS folded, f.pos, f.start_offset, f.end_offset
FROM k LEFT JOIN LATERAL sift.tokenize(v, 'k_fold') f ON true ORDER BY id;
SELECT id, '[' || e.token || ']' AS exact, '[' || c.token || ']' AS case_only
FROM k LEFT JOIN LATERAL sift.tokenize(v, 'k_exact') e ON true LEFT JOIN LATERAL sift.tokenize(v, 'k_case') c ON true ORDER BY id;
SELECT token FROM sift.tokenize('Gamma Delta', 'k_fold', case_folding => false);

-- a query word is a value too: SiftQL splits words at spaces, a phrase keeps them together
SELECT sift.parse_query('Gamma Delta', 'k_fold'), sift.parse_query('"Gamma Delta"', 'k_fold'), sift.parse_query('Caf*', 'k_fold');

-- the operator matches whole values; an index scan and a sequential scan agree
CREATE FUNCTION k_ids(q text) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '-') FROM k WHERE v @@> q $$;
CREATE FUNCTION k_ids_seq(q text) RETURNS text LANGUAGE sql SET enable_bitmapscan = off SET sift.enable_scan = off AS $$
	SELECT coalesce(string_agg(id::text, ',' ORDER BY id), '-') FROM k WHERE v @@> q $$;
SELECT q, k_ids(q), k_ids(q) = k_ids_seq(q) AS same FROM (VALUES ('ALPHA'), ('cafe'), ('"gamma delta"'), ('gamma'), ('delta'),
	('"a-b_c.d"'), ('strasse'), ('Straße'), ('caf*'), ('/al.*/'), ('alpha OR cafe'), ('alpha AND cafe')) x(q);

-- values are compared whole: the same index count of each value equals GROUP BY of the folded tokens
SELECT value, count(*) FROM (SELECT (SELECT token FROM sift.tokenize(v, 'k_fold')) AS value FROM k) s
GROUP BY value ORDER BY 2 DESC, value COLLATE "C";

-- a value is at most a term long, and is never cut into another value
INSERT INTO k VALUES (20, repeat('é', 127));
INSERT INTO k VALUES (21, repeat('é', 128));
INSERT INTO k VALUES (22, repeat('x', 256));
SELECT sift.verify('k_fold', true), sift.verify('k_exact', true), sift.verify('k_case', true);

-- options
CREATE INDEX bad ON k USING sift (v) WITH (analyzer = keyword, stemmer = en);
CREATE INDEX bad ON k USING sift (v) WITH (analyzer = keyword, prefix_index = 2);
CREATE INDEX bad ON k USING sift (v) WITH (analyzer = keyword, fields = 'a');
CREATE INDEX bad ON k USING sift (v) WITH (analyzer = nope);
CREATE INDEX t_idx ON k USING sift (v) WITH (analyzer = text);
SELECT reloptions FROM pg_class WHERE relname IN ('k_fold', 't_idx') ORDER BY relname;

-- the analyzer is fixed at build time
ALTER INDEX k_fold SET (analyzer = text);
SELECT sift.verify('k_fold');
REINDEX INDEX k_fold;
SELECT sift.verify('k_fold');

DROP TABLE k;
DROP FUNCTION k_ids(text), k_ids_seq(text);
DROP EXTENSION pg_sift;
