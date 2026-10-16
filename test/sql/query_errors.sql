-- SiftQL errors: precise 1-based character positions, syntax vs limit codes
CREATE EXTENSION pg_sift;
\set VERBOSITY terse

SELECT sift.parse_query('a OR (b');
SELECT sift.parse_query(repeat('a ', 257));

CREATE FUNCTION perr(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	RETURN 'ok: ' || sift.parse_query(q);
EXCEPTION WHEN syntax_error OR program_limit_exceeded THEN
	RETURN sqlstate || ': ' || sqlerrm;
END $$;

-- syntax errors
SELECT q, perr(q) FROM (VALUES
	(''), ('   '), (E'\t\n'), ('a OR'), ('OR a'), ('AND a'), ('a AND'), ('a OR OR b'),
	('a AND AND b'), ('a AND OR b'), ('NOT'), ('a NOT'), ('a NOT NOT'), ('NOT a'),
	('NOT a NOT b'), ('a OR NOT b'), ('(NOT a) OR b'), ('x (NOT b NOT c)^2'),
	('NOT (a b)'), ('(a'), ('a)'), ('a))'), ('()'), ('(a OR)'), ('{}'), ('{a'), ('a}'),
	('{a "b"}'), ('{a (b)}'), ('{a OR b}'), ('{a NOT b}'), ('{a^2}'), ('{{a}}'),
	('"abc'), ('a "b'), ('/abc'), ('//'), ('a\'), ('"a\'), ('/a\/'),
	('a ^2'), ('^2'), ('a^'), ('a^x'), ('a^2x'), ('a^-1'), ('a^+1'), ('a^2^3'),
	('a^1e'), ('a^.'), ('a^1..2'), ('a^2~'), ('"a"^'), ('~a'), ('a ~1'), ('a~3'),
	('a~0'), ('a~12'), ('a~x'), ('a~~'), ('"a"~2'), ('(a)~1'), ('/a/~'), ('a*~'),
	('a?~2'), ('*'), ('?'), ('*?*'), ('a OR *'), ('/a(/'), ('/a)|(b/'), ('/[a/'),
	('/a{2,1}/'), ('/(?q)a/'), ('/***=a/'), ('/\/'), ('x /a(/^2')
) v(q);

-- positions count characters, not bytes
SELECT q, perr(q) FROM (VALUES
	('ünïcödé "ok'), ('жук OR'), ('日本 語 )'), ('ü^x'), ('éé~5')
) v(q);

-- limits
SELECT q, perr(q) FROM (VALUES
	('a^10000.01'), ('a^1e9'), ('a^99999999999999999999999999999999999999999'),
	('(a^100)^101'), ('((a^10)^10)^100.5'),
	('(((((((((((((((((((((((((((((((((a)))))))))))))))))))))))))))))))))')
) v(q);
SELECT perr(repeat('(', 32) || 'a' || repeat(')', 32)) AS depth_32;
SELECT perr(repeat('(', 33) || 'a' || repeat(')', 33)) AS depth_33;
SELECT perr(repeat('a ', 256)) LIKE 'ok: a^256' AS leaves_256;
SELECT perr(repeat('a ', 257)) AS leaves_257;
SELECT perr('{' || repeat('a ', 257) || '}') AS brace_leaves;
SELECT perr('"' || repeat('w ', 32) || '"') LIKE 'ok: %' AS phrase_32;
SELECT perr('x "' || repeat('w ', 129) || '"') AS phrase_129;
SELECT perr('x ' || repeat('w-', 129)) AS word_phrase_129;

-- hostile input fails fast and cleanly
SELECT perr(repeat('(', 1000000)) AS deep_parens;
SELECT perr(repeat('{', 100000)) AS deep_braces;
SELECT perr(repeat('NOT ', 60000) || 'a b') AS not_chain;
SELECT perr(repeat('x', 1000000)) AS huge_term;
SELECT perr(repeat('x', 262144)) AS max_size_term;
SELECT perr(repeat('a OR ', 52428)) AS huge_or;
SELECT perr(repeat('"', 99999)) AS quotes;
SELECT perr(repeat('\', 99999)) AS backslashes;
SELECT perr(repeat('/a(', 1000) || '/') AS regex_garbage;
SELECT perr(E'\x01\x02\x03\x7f') AS only_control;
SELECT perr(repeat('a^1', 1000)) AS boost_chain;
SELECT perr(repeat('a*', 100000)) AS wildcard_soup;
SELECT perr('x ' || repeat('a?', 128)) AS wildcard_256_bytes, perr('x OR ' || repeat('a?', 127)) LIKE 'ok: x OR a?%' AS wildcard_254_bytes;

-- regular expressions: at most 1000 bytes, complexity bounded by the engine
SELECT length(perr('/' || repeat('(a|', 249) || 'bcde' || repeat(')', 249) || '/')) AS regex_1000;
SELECT perr('/' || repeat('(a|', 249) || 'bcdef' || repeat(')', 249) || '/') AS regex_1001;
SELECT perr('/' || repeat('((a{1,100}){1,100}){1,100}', 3) || '/') AS regex_too_complex;

DROP FUNCTION perr;
DROP EXTENSION pg_sift;
