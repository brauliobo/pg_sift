-- Builds one tier from the src staging table.
-- psql vars: limit (doc count or ALL), mult (L-tier multiplier, 1 = real docs only), nsub (substrings).
\set ON_ERROR_STOP 1

-- Deterministic sample, renumbered 1..N in original insertion order.
INSERT INTO docs
SELECT row_number() OVER (ORDER BY src_id), body, prop_id, rating, country, created, likes, url
FROM (SELECT * FROM src ORDER BY hashint8(src_id) LIMIT :limit) s
ORDER BY src_id;
DROP TABLE src;

-- Synthetic copies (L tier): head of doc a + tail of doc b, split at a space near the middle.
-- Keeps the corpus-wide term distribution (Zipf) and real local n-grams; vocabulary does not grow.
INSERT INTO docs
SELECT k * n + a.id,
       CASE WHEN pa > 0 THEN left(a.body, length(a.body) / 2 + pa - 1) ELSE a.body END
       || CASE WHEN pb > 0 THEN ' ' || substr(b.body, length(b.body) / 2 + pb + 1) ELSE '' END,
       a.prop_id, b.rating, a.country, b.created, a.likes, a.url
FROM (SELECT max(id) AS n FROM docs) m
CROSS JOIN generate_series(1, :mult - 1) k
JOIN docs a ON true
JOIN docs b ON b.id = 1 + abs(hashint8(a.id * 1000 + k)) % m.n
CROSS JOIN LATERAL (SELECT strpos(substr(a.body, length(a.body) / 2 + 1), ' ') AS pa,
                           strpos(substr(b.body, length(b.body) / 2 + 1), ' ') AS pb) s
ORDER BY 1;

SELECT setval('docs_id_seq', max(id)) FROM docs \g /dev/null
CREATE INDEX docs_created ON docs (created);
CREATE INDEX docs_prop_id ON docs (prop_id);

INSERT INTO props
SELECT DISTINCT prop_id, 'property ' || prop_id, abs(hashint4(prop_id)) % 10 FROM docs WHERE prop_id IS NOT NULL;

-- Selective scalar filter for the mixed-WHERE workload: newest ~2% of documents.
INSERT INTO params SELECT 'recent_cutoff', percentile_disc(0.98) WITHIN GROUP (ORDER BY created) FROM docs;

-- Query trace: random runs of 2..15 consecutive plain word tokens from random documents.
CREATE TEMP TABLE picks AS
SELECT i, 1 + abs(hashint8(i * 7919)) % (SELECT max(id) FROM docs) AS doc_id, 2 + abs(hashint8(i * 104729)) % 14 AS len
FROM generate_series(1::bigint, :nsub * 2) i;

CREATE TEMP TABLE toks AS
SELECT p.i, p.len, p.doc_id, row_number() OVER (PARTITION BY p.i ORDER BY t.o) AS ord, t.alias, t.lexemes[1] AS lex
FROM picks p JOIN docs d ON d.id = p.doc_id
CROSS JOIN LATERAL ts_debug('simple', d.body) WITH ORDINALITY AS t(alias, description, token, dictionaries, dictionary, lexemes, o)
WHERE t.alias <> 'blank';

-- Islands of consecutive word tokens: hyphenated words, numbers, URLs etc. break a run.
CREATE TEMP TABLE runs AS
SELECT i, len, doc_id, ord, lex, ord - row_number() OVER (PARTITION BY i ORDER BY ord) AS grp
FROM toks WHERE alias IN ('asciiword', 'word', 'numword') AND length(lex) <= 100;

INSERT INTO queries
WITH r AS (SELECT i, len, grp, min(ord) AS s, count(*) AS n FROM runs GROUP BY 1, 2, 3),
best AS (
  SELECT DISTINCT ON (i) i, grp, least(len, n) AS k, s, n FROM r WHERE n >= 2
  ORDER BY i, least(len, n) DESC, hashint8(i * 1000 + grp)
),
win AS (SELECT i, k, s + abs(hashint8(i)) % (n - k + 1) AS ws FROM best),
subs AS (
  SELECT row_number() OVER (ORDER BY w.i) AS sub, min(u.doc_id) AS doc_id, array_agg(u.lex ORDER BY u.ord) AS terms
  FROM win w JOIN runs u ON u.i = w.i AND u.ord >= w.ws AND u.ord < w.ws + w.k
  GROUP BY w.i ORDER BY w.i LIMIT :nsub
)
SELECT (sub - 1) * 3 + m.n, sub, m.mode, terms, doc_id
FROM subs CROSS JOIN (VALUES (1, 'and'), (2, 'or'), (3, 'phrase')) m(n, mode);

VACUUM (ANALYZE) docs, props, queries, params;
CHECKPOINT;
