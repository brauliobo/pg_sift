-- The text dataset of the cases (bench/cases.tsv, dataset `docs`): the documents of a BM25 search, generated, no download.
--
--   psql -v n=100000 -f gen.sql         (the cases run it with n = --rows: 100000, 300000 and 1000000 are the published sizes)
--
-- docs(id, title, body, category_id, created_at, a_count, url, tsv): n documents of 40 words and a title of 6. Every word is a hash of (document, position), so the same n gives the
-- same rows. The vocabulary is 20,000 invented words, 'q' and four letters (a = 0) of the rank k: rank 1 is 'qaaab', 5 'qaaaf', 40 'qaabo', 100 'qaadw', 500 'qaatg', 5000 'qahki'.
-- A draw is rank = 1 + floor(20000 * u^3): the head is common (document frequency: rank 1 78%, 5 16%, 100 2.1%, 500 0.8%, 5000 0.2%, 19000 0.03%), so a query can pick a term of any
-- document frequency by its rank. Documents whose id divides by 97 hold ranks 40 and 41 side by side (the phrase 'qaabo qaabp', 1% of the documents).
-- tsv is `to_tsvector('simple', body)` with a GIN index: the PostgreSQL lane of a case (no stop words and no stemming, as the BM25 index without a `stemmer`).
-- categories(id, region, name): 200 categories (docs.category_id is skewed to the first ones), 8 regions: the join and the INCLUDE tests. created_at spans 2024-01-01 to 2025-12-31,
-- a_count is skewed to small values. A case builds the BM25 index it measures in its `--@ setup` (the name docs_sift, dropped first: the next case replaces it).
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 100000
\endif
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;
CREATE FUNCTION pg_temp.word(int) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT 'q' || chr(97 + $1 / 17576 % 26) || chr(97 + $1 / 676 % 26) || chr(97 + $1 / 26 % 26) || chr(97 + $1 % 26) $$;
CREATE FUNCTION pg_temp.draw(bigint, int) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT pg_temp.word(CASE WHEN $1 % 97 = 0 AND $2 = 10 THEN 40 WHEN $1 % 97 = 0 AND $2 = 11 THEN 41
                                 ELSE 1 + floor(20000 * power(pg_temp.u($1 * 64 + $2, 1), 3))::int END) $$;

CREATE TABLE categories AS SELECT i AS id, 1 + i % 8 AS region, 'category ' || i AS name FROM generate_series(1, 200) i;
ALTER TABLE categories ADD PRIMARY KEY (id);

CREATE TABLE docs AS
SELECT d.*, to_tsvector('simple', d.body) AS tsv FROM (
  SELECT g::bigint AS id,
         (SELECT string_agg(pg_temp.draw(g, p), ' ' ORDER BY p) FROM generate_series(41, 46) p) AS title,
         (SELECT string_agg(pg_temp.draw(g, p), ' ' ORDER BY p) FROM generate_series(1, 40) p) AS body,
         1 + floor(200 * power(pg_temp.u(g, 2), 2))::int AS category_id,
         timestamptz '2024-01-01' + floor(pg_temp.u(g, 3) * 730)::int * interval '1 day' + floor(pg_temp.u(g, 4) * 86400)::int * interval '1 second' AS created_at,
         floor(1000 * power(pg_temp.u(g, 5), 4))::int AS a_count,
         'https://example.org/p/' || g AS url
  FROM generate_series(1, :n) g) d;
ALTER TABLE docs ADD PRIMARY KEY (id);
CREATE INDEX docs_tsv_gin ON docs USING gin (tsv);
CREATE INDEX docs_created ON docs (created_at);
CREATE INDEX docs_category ON docs (category_id);
VACUUM (FREEZE, ANALYZE) docs;
VACUUM (FREEZE, ANALYZE) categories;
