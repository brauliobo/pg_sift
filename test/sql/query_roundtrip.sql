-- Property test: for generated queries the canonical form is a fixpoint
-- (parse(deparse(q)) == q) and matches exactly the documents q matches.
CREATE EXTENSION sift;

CREATE TABLE rt_docs (body text);
INSERT INTO rt_docs VALUES
	('the quick brown fox jumps over the lazy dog'), ('quick red fox, lazy brown bear'),
	('e-mail the honey bear about x86_64 builds'), ('жук and жучок under the log'),
	('dog dog dog'), ('fox'), ('brown honey'), ('');

CREATE FUNCTION rt_pick(VARIADIC a text[]) RETURNS text LANGUAGE sql
	RETURN a[1 + floor(random() * array_length(a, 1))::int];

CREATE FUNCTION rt_leaf(atom bool DEFAULT false) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	w text := rt_pick('fox', 'quick', 'brown', 'dog', 'lazy', 'bear', 'honey', 'the',
					  'e-mail', 'x86_64', 'жук', 'жучок', 'Fox', '\AND', 'o\(k', 'qu\*');
BEGIN
	RETURN CASE floor(random() * 8)::int + atom::int
		WHEN 0 THEN '"' || w || ' ' || rt_pick('fox', 'brown', 'bear', 'dog', 'the') || '"'
		WHEN 1 THEN left(w, 2) || '*'
		WHEN 2 THEN rt_pick('b?own', 'f*x', '*o*', 'l?z*', 'жу*к', 'q*k')
		WHEN 3 THEN rt_pick('/b.*n/', '/(fox|dog)/', '/[a-z]{3}/', '/x8\/6/', '/.*/')
		WHEN 4 THEN w || rt_pick('~', '~1', '~2')
		ELSE w
	END || CASE WHEN NOT atom AND random() < 0.15 THEN '^' || rt_pick('0', '0.5', '2', '3.25', '1e-3', '10') ELSE '' END;
END $$;

CREATE FUNCTION rt_gen(depth int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	n int := 2 + floor(random() * 3)::int;
	parts text[] := '{}';
BEGIN
	IF depth = 0 OR random() < 0.3 THEN
		RETURN rt_leaf();
	END IF;
	CASE floor(random() * 4)::int
	WHEN 0 THEN
		FOR i IN 1..n LOOP parts := parts || rt_gen(depth - 1); END LOOP;
		RETURN '(' || array_to_string(parts, ' OR ') || ')';
	WHEN 1 THEN
		FOR i IN 1..n LOOP parts := parts || rt_leaf(true); END LOOP;
		RETURN '{' || array_to_string(parts, ' ') || '}';
	ELSE
		parts := ARRAY[rt_gen(depth - 1)];
		FOR i IN 2..n LOOP
			parts := parts || (rt_pick('', 'NOT ', '') || rt_gen(depth - 1));
		END LOOP;
		RETURN '(' || array_to_string(parts, rt_pick(' ', ' AND ')) || ')'
			|| CASE WHEN random() < 0.2 THEN '^' || rt_pick('2', '0.5') ELSE '' END;
	END CASE;
END $$;

CREATE FUNCTION rt_hits(q text) RETURNS bool[] LANGUAGE sql
	RETURN (SELECT array_agg(body @@> q ORDER BY body) FROM rt_docs);

SELECT setseed(0.4242);
CREATE TEMP TABLE rt AS SELECT rt_gen(4) AS q FROM generate_series(1, 3000);

DO $$
DECLARE
	r record;
	d1 text;
	ok int := 0;
	fails int := 0;
	errs jsonb := '{}';
BEGIN
	FOR r IN SELECT q FROM rt LOOP
		BEGIN
			d1 := sift.parse_query(r.q);
			IF d1 IS DISTINCT FROM sift.parse_query(d1) OR rt_hits(r.q) IS DISTINCT FROM rt_hits(d1) THEN
				fails := fails + 1;
				RAISE NOTICE 'round trip failed: % -> %', r.q, d1;
			ELSE
				ok := ok + 1;
			END IF;
		EXCEPTION WHEN syntax_error OR program_limit_exceeded THEN
			errs := jsonb_set(errs, ARRAY[sqlstate], to_jsonb(coalesce((errs ->> sqlstate)::int, 0) + 1));
		END;
	END LOOP;
	RAISE NOTICE 'ok %, failed %, rejected %', ok, fails, errs;
END $$;

DROP FUNCTION rt_gen, rt_leaf, rt_pick, rt_hits;
DROP TABLE rt_docs;
DROP EXTENSION sift;
