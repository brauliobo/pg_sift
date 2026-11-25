-- Every emitted token analyzes to exactly itself, in every configuration
-- (tools/analyze `harness invariant` checks every code point exhaustively).
CREATE EXTENSION pg_sift;
CREATE FUNCTION bad_tokens(doc text, stemmer text, c bool, a bool) RETURNS bigint LANGUAGE sql
  AS $$ SELECT count(*) FROM sift.tokenize(doc, NULL, stemmer, c, a) t
        WHERE ARRAY(SELECT token FROM sift.tokenize(t.token, NULL, stemmer, c, a)) <> ARRAY[t.token] $$;
CREATE TEMP TABLE configs AS
  SELECT s AS stemmer, c, a FROM unnest(ARRAY['none', 'en']) s, unnest(ARRAY[false, true]) c,
                                 unnest(ARRAY[false, true]) a;

-- folds that would change the token shape keep the character as written
SELECT stemmer, c, a, string_agg(token, ' ' ORDER BY pos) AS tokens
  FROM configs, sift.tokenize('ゟ ŉ Ŋ ᾳ ﬁ ｶ Ⅻ', NULL, stemmer, c, a)
  GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;

-- the Basic Multilingual Plane with the default analyzer, alone and next to letters
SELECT count(*) AS bad FROM generate_series(1, 65535) cp
  WHERE cp NOT BETWEEN 55296 AND 57343
    AND bad_tokens(chr(cp) || ' x' || chr(cp) || ' ' || chr(cp) || 'a' || chr(cp) || chr(cp),
                   NULL, NULL, NULL) > 0;

-- a sample of all planes in every configuration
SELECT stemmer, c, a, count(*) FILTER (WHERE bad_tokens(chr(cp) || ' x' || chr(cp) || chr(769),
                                                         stemmer, c, a) > 0) AS bad
  FROM configs, generate_series(1, 1114111, 61) cp
  WHERE cp NOT BETWEEN 55296 AND 57343
  GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;

-- Porter2 is not idempotent, so stems are iterated to a fixpoint
SELECT w, (SELECT string_agg(token, ' ') FROM sift.tokenize(w, stemmer => 'en')) AS stem,
       bad_tokens(w, 'en', true, true) AS bad
  FROM unnest(ARRAY['agreed', 'accelerate', 'accident', 'accidental', 'abase', 'generously',
                    'happiness', 'conditional', 'Universities']) w;

-- unknown stemmers fail when the index is defined
CREATE TABLE docs (body text);
CREATE INDEX ON docs USING sift (body) WITH (stemmer = 'klingon');
CREATE INDEX ON docs USING sift (body) WITH (stemmer = 'en');
DROP TABLE docs;

DROP FUNCTION bad_tokens;
DROP EXTENSION pg_sift;
