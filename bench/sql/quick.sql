-- Standard quick suite: 36 fixed queries from the trace (first six 2-4 term substrings).
-- term = their longest word, prefix = its first length-3 chars (min 3) + *, fuzzy = it minus its
-- second-to-last char (edit distance 1); prefix and fuzzy take the first six substrings whose longest
-- word is lowercase ASCII (a CJK run is tokenized differently by every engine and matches nothing).
-- count = count(*) of the and/or/phrase queries; mix = the top-10 term/and/or/phrase queries together
-- (QPS of the ranked mix).
CREATE OR REPLACE TEMP VIEW quick AS
WITH subs AS (
  SELECT sub, terms FROM queries WHERE mode = 'and' AND cardinality(terms) BETWEEN 2 AND 4 ORDER BY sub LIMIT 6
), words AS (
  SELECT sub, (SELECT t FROM unnest(terms) t ORDER BY length(t) DESC, t LIMIT 1) AS w FROM subs
), pwords AS (
  SELECT sub, w FROM (SELECT sub, (SELECT t FROM unnest(terms) t ORDER BY length(t) DESC, t LIMIT 1) AS w
                      FROM queries WHERE mode = 'and' AND cardinality(terms) BETWEEN 2 AND 4) x
  WHERE w ~ '^[a-z]+$' ORDER BY sub LIMIT 6
), base AS (
  SELECT 'term' AS class, 'or' AS mode, ARRAY[w] AS terms, sub FROM words
  UNION ALL SELECT m, m, terms, sub FROM subs CROSS JOIN unnest('{and,or,phrase}'::text[]) m
  UNION ALL SELECT 'prefix', 'prefix', ARRAY[left(w, greatest(3, length(w) - 3))], sub FROM pwords
  UNION ALL SELECT 'fuzzy', 'fuzzy', ARRAY[overlay(w PLACING '' FROM length(w) - 1 FOR 1)], sub FROM pwords
)
SELECT class, 'top10' AS kind, mode, terms, sub FROM base
UNION ALL SELECT 'count', 'count', mode, terms, sub FROM base WHERE class IN ('and', 'or', 'phrase')
UNION ALL SELECT 'mix', 'top10', mode, terms, sub FROM base WHERE class IN ('term', 'and', 'or', 'phrase');
