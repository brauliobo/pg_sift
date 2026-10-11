-- SiftQL parsing, analysis and canonical deparse
CREATE EXTENSION sift;

CREATE FUNCTION pq(q text) RETURNS text LANGUAGE sql
	RETURN sift.parse_query(q);

-- terms, juxtaposition, keywords (uppercase only), precedence NOT > AND > OR
SELECT q, pq(q) FROM (VALUES
	('hello'), ('Hello WORLD'), ('a b c'), ('a AND b'), ('a and b'), ('a OR b'),
	('a or b'), ('a OR b c'), ('a b OR c'), ('(a OR b) c'), ('a AND (b OR c)'),
	('a OR (b OR c)'), ('(a OR b) OR c'), ('a (b c)'), ('a NOT b'), ('a AND NOT b'),
	('a NOT b OR c'), ('a NOT (b OR c)'), ('a NOT (b c)'), ('a NOT (b NOT c)'),
	('a (NOT b)'), ('(a) (NOT b NOT c)'), ('NOT NOT a'), ('a NOT NOT NOT b'),
	('NOT (NOT a)^2 b'), ('(NOT a)^2 b')
) v(q);

-- canonical order: positives sorted (terms, prefixes, wildcards, regexes,
-- fuzzy, phrases, groups), NOTs last; duplicates merge
SELECT q, pq(q) FROM (VALUES
	('zeta alpha mid'), ('b OR a'), ('"x y" z* /r/ w~2 v?v u (s OR t) NOT q'),
	('a a a'), ('a^2 a'), ('a OR a'), ('NOT a b NOT a'), ('a NOT a'), ('a NOT (a)^2'),
	('{a b} {b a}'), ('"a b" "A  B"'), ('a^6000 a^6000'), ('a^5000 a^5000'),
	('x NOT (a OR a^3)')
) v(q);

-- phrases and analysis: multi-token words become phrases, empty leaves drop
SELECT q, pq(q) FROM (VALUES
	('"Hello World"'), ('"one"'), ('""'), ('"  ...  "'), ('e-mail'), ('E-Mail^2'),
	('don''t stop'), ('--'), ('-- x'), ('x OR --'), ('-- NOT x'), ('x NOT --'),
	('x NOT (-- NOT y)'), ('"say \"hi\" \\ now"'), ('"a\b"'),
	('foo\(bar\)'), ('\"quoted\"'), ('a_b'), ('x AND ("" OR "")'),
	('x AND (-- NOT y)')
) v(q);

-- boosts: pushed down to leaves, products, shortest float form
SELECT q, pq(q) FROM (VALUES
	('a^2'), ('a^2.5 b^0'), ('a^0.1'), ('a^.5'), ('a^1e-5'), ('a^1E+3'), ('a^1.0'),
	('a^10000'), ('(a b)^2 c^0.5'), ('((a^2 b)^3)'), ('(a OR b^4)^0.5'), ('{x y}^3'),
	('"p q"^2'), ('/r.x/^2'), ('fo*^2 OR fo*'), ('a NOT b^5'), ('(a^100)^100'),
	('(a^1.1)^1.1'), ('x NOT (a^7 b)^3')
) v(q);

-- fuzzy: default distance 1, short terms allow fewer edits
SELECT q, pq(q) FROM (VALUES
	('foo~'), ('foo~1'), ('foo~2'), ('ab~'), ('abcd~2'), ('abcde~2'), ('abcdef~2'),
	('Hello~2^3'), ('\AND~1'), ('foo~ foo~2'), ('(foo~)'), ('ab~ ab'), ('{a a~2}'), ('x-y~1'), ('\(foo-bar\)~1'), ('\(lazzy\)~2'), ('--foobar--~2'), ('--~2')
) v(q);

-- prefix, wildcard, escapes; meta runs are canonicalized
SELECT q, pq(q) FROM (VALUES
	('fo*'), ('Fo*'), ('fo**'), ('f*o'), ('f?o'), ('a**b?*c'), ('a*?*?b'), ('?a'),
	('*a*'), ('foo\*'), ('foo\**'), ('a\?b*'), ('a\\*'), ('\*\?'), ('a\ b*'),
	('a\(*'), ('A?B*c'), (E'a*\u0303?b'), (E'ab*\u0303'), (E'\u0303*\u0303')
) v(q);

-- regular expressions: whole-term match, "\/" escapes the delimiter
SELECT q, pq(q) FROM (VALUES
	('/a.c/'), ('/a\/b/'), ('/a\\/'), ('/a\.b/'), ('/[0-9]+/'), ('/(?i)A.c/'),
	('/(?:ab)+/'), ('/x y/'), ('{/a/ b*}'), ('/A/^2 OR /A/')
) v(q);

-- "any of" groups
SELECT q, pq(q) FROM (VALUES
	('{a b c}'), ('{a}'), ('{a b}^2 c'), ('{a* b?c /d/ e~}'), ('{a b} OR c'),
	('x {a b} NOT {c d}')
) v(q);

-- keywords as terms need escaping, lowercase keywords are terms
SELECT q, pq(q) FROM (VALUES
	('\AND'), ('\OR \NOT'), ('and or not'), ('"AND"'), ('ANDROID ORE NOTE'), ('AND*')
) v(q);

-- whitespace and control characters separate words
SELECT pq(E'a\tb\nc\rd') AS tabs, pq(E'a\x01b') AS control, pq(E'a\\\x01b') AS escaped_control;

-- canonical output is a fixpoint
SELECT q, pq(q) = pq(pq(q)) AS fixpoint FROM (VALUES
	('a OR b c NOT (d OR e)^3'), ('{x* y?y /z/} "p q r"^2 NOT w~2'), ('foo\**^2 \AND'),
	('/a\\\/b/'), ('(a b)^0 c'), ('x NOT (-- NOT y)')
) v(q);

-- index argument
SELECT sift.parse_query('a', NULL), sift.parse_query(NULL);
SELECT sift.parse_query('a', 'pg_class'::regclass);
SELECT sift.parse_query('a', 'pg_class_oid_index'::regclass);

DROP FUNCTION pq;
DROP EXTENSION sift;
