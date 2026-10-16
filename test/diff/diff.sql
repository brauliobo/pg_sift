-- Differential testing of sift against the SQL reference (test/sql/reference.sql):
-- random corpora, random SiftQL queries, per-stage comparison, minimization.
-- Driven by scripts/difftest.sh; load after reference.sql (\set sift_ref_lib 1).
--
-- Settings: sift_diff.engine = sift | mutant:<name> (the reference with an
-- injected bug, to prove the harness catches it), sift_diff.tbl, sift_diff.idx
-- ('' when the index cannot be built yet).

\set sift_diff_echo :ECHO
\set ECHO none
SET client_min_messages = warning;
SET check_function_bodies = off;
CREATE SCHEMA IF NOT EXISTS sift_diff;

CREATE OR REPLACE FUNCTION sift_diff.setting(name text, dflt text DEFAULT '') RETURNS text LANGUAGE sql STABLE
AS $$ SELECT coalesce(nullif(current_setting('sift_diff.' || name, true), ''), dflt) $$;

CREATE OR REPLACE FUNCTION sift_diff.rand_int(lo int, hi int) RETURNS int LANGUAGE sql VOLATILE
AS $$ SELECT lo + floor(random() * (hi - lo + 1))::int $$;

CREATE OR REPLACE FUNCTION sift_diff.rand_word() RETURNS text LANGUAGE sql VOLATILE AS $$
	SELECT string_agg(substr('bcdfghklmnprstvz', sift_diff.rand_int(1, 16), 1)
		|| substr('aeiouaeiouéüãôç', sift_diff.rand_int(1, CASE WHEN random() < 0.1 THEN 15 ELSE 10 END), 1), '')
	FROM generate_series(1, sift_diff.rand_int(1, 4))
$$;

-- Zipfian rank (s = 1) by log-uniform inversion
CREATE OR REPLACE FUNCTION sift_diff.zipf_rank() RETURNS int LANGUAGE sql VOLATILE
AS $$ SELECT greatest(floor(exp(random() * ln((SELECT count(*) FROM diff_vocab) + 1)))::int, 1) $$;

CREATE OR REPLACE FUNCTION sift_diff.make_text(ntok int) RETURNS text LANGUAGE sql VOLATILE AS $$
	SELECT coalesce(string_agg(t.sep || CASE WHEN t.c < 0.75 THEN v.word WHEN t.c < 0.9 THEN initcap(v.word)
		ELSE upper(v.word) END, '' ORDER BY t.i), '')
	FROM (SELECT i, random() AS c, sift_diff.zipf_rank() AS r,
			(ARRAY[' ', ' ', ' ', ' ', ' ', ', ', '. ', E'\n', '-', '! ', ' (', ') ', '', '/'])[sift_diff.rand_int(1, 14)] AS sep
		FROM generate_series(1, ntok) i) t
	JOIN diff_vocab v ON v.rank = t.r
$$;

-- NULLs, empty and punctuation-only bodies, duplicates, short to very long (TOASTed) documents
CREATE OR REPLACE FUNCTION sift_diff.make_body(tiny bool) RETURNS text LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	u float8 := random();
	dup text;
BEGIN
	IF tiny THEN
		RETURN sift_diff.make_text(sift_diff.rand_int(0, 2));
	ELSIF u < 0.03 THEN
		RETURN NULL;
	ELSIF u < 0.05 THEN
		RETURN '';
	ELSIF u < 0.06 THEN
		RETURN '... !!! --- ()';
	ELSIF u < 0.11 THEN
		SELECT body INTO dup FROM diff_docs WHERE id = (SELECT sift_diff.rand_int(1, coalesce(max(id), 1)) FROM diff_docs);
		IF FOUND THEN
			RETURN dup;
		END IF;
	END IF;
	u := random();
	RETURN sift_diff.make_text(CASE WHEN u < 0.1 THEN sift_diff.rand_int(1, 4) WHEN u < 0.8 THEN sift_diff.rand_int(5, 60)
		WHEN u < 0.95 THEN sift_diff.rand_int(60, 400) ELSE sift_diff.rand_int(400, 3000) END);
END $$;

-- Corpus diff_docs(id, body, pad): 256-row runs share a page profile, from
-- tiny rows (~150 per heap page, every 4th run) to 3 kB pads (2 per page)
CREATE OR REPLACE FUNCTION sift_diff.build(seed float8, ndocs int, nvocab int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	g int;
	p int;
BEGIN
	PERFORM setseed(seed);
	DROP TABLE IF EXISTS diff_vocab, diff_docs;
	CREATE TABLE diff_vocab AS
		SELECT r AS rank, CASE
			WHEN r % 89 = 5 THEN (ARRAY['🍕', '🎉', '☕', '🚀', '❤'])[1 + r / 89 % 5]
			WHEN r % 53 = 7 THEN (r * 7919 % 100000)::text
			WHEN r % 61 = 11 THEN (ARRAY['Σοφία', 'ΟΔΟΣ', 'москва', 'Straße', 'İstanbul', 'naïve', 'ﬁnal', 'Ångström'])[1 + r / 61 % 8]
			ELSE sift_diff.rand_word() END AS word
		FROM generate_series(1, nvocab) r;
	ALTER TABLE diff_vocab ADD PRIMARY KEY (rank);
	CREATE TABLE diff_docs (id int, body text, pad text) WITH (autovacuum_enabled = off);
	ALTER TABLE diff_docs ALTER COLUMN pad SET STORAGE plain;
	CREATE INDEX diff_docs_id ON diff_docs (id);
	CREATE INDEX diff_docs_id_brin ON diff_docs USING brin (id) WITH (pages_per_range = 1);
	FOR g IN 1 .. ndocs LOOP
		IF g % 256 = 1 THEN
			p := CASE WHEN g % 1024 = 1 THEN 0 ELSE sift_diff.rand_int(0, 5) END;
		END IF;
		INSERT INTO diff_docs VALUES (g, sift_diff.make_body(p = 0), repeat('x', (ARRAY[0, 0, 40, 400, 1500, 3000])[p + 1]));
	END LOOP;
END $$;

-- One mutation round: deletes, non-HOT body updates, HOT pad updates, inserts
CREATE OR REPLACE FUNCTION sift_diff.mutate(seed float8, frac float8) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	d bigint;
	u1 bigint;
	u2 bigint;
	n int;
	top int;
BEGIN
	PERFORM setseed(seed);
	DELETE FROM diff_docs WHERE random() < frac;
	GET DIAGNOSTICS d = ROW_COUNT;
	UPDATE diff_docs SET body = sift_diff.make_body(pad = '' AND random() < 0.5) WHERE random() < frac;
	GET DIAGNOSTICS u1 = ROW_COUNT;
	UPDATE diff_docs SET pad = pad || 'y' WHERE random() < frac;
	GET DIAGNOSTICS u2 = ROW_COUNT;
	SELECT count(*) * frac, max(id) INTO n, top FROM diff_docs;
	INSERT INTO diff_docs SELECT top + g, sift_diff.make_body(false), '' FROM generate_series(1, n) g;
	RETURN format('deleted %s, body updates %s, pad updates %s, inserts %s', d, u1, u2, n);
END $$;

-- Query generation: raw ASTs rendered by sift_ref.render
CREATE OR REPLACE FUNCTION sift_diff.pick_word(hot bool) RETURNS text LANGUAGE sql VOLATILE AS $$
	SELECT word FROM diff_vocab WHERE rank = (SELECT CASE WHEN hot THEN sift_diff.rand_int(1, 30) ELSE sift_diff.zipf_rank() END)
$$;

CREATE OR REPLACE FUNCTION sift_diff.esc(w text) RETURNS text LANGUAGE sql IMMUTABLE
AS $$ SELECT sift_ref.quote(w, '\s(){}"^~/;*?') $$;

CREATE OR REPLACE FUNCTION sift_diff.surface(w text) RETURNS text LANGUAGE sql VOLATILE AS $$
	SELECT sift_diff.esc(CASE WHEN u < 0.6 THEN w WHEN u < 0.8 THEN initcap(w) ELSE upper(w) END)
	FROM (SELECT random() AS u) x
$$;

-- One edit, at the first character (which fuzzy matching keeps fixed) one time in five
CREATE OR REPLACE FUNCTION sift_diff.typo(w text) RETURNS text LANGUAGE sql VOLATILE AS $$
	SELECT CASE WHEN length(w) < 2 THEN w || 'x' ELSE CASE sift_diff.rand_int(0, 2)
		WHEN 0 THEN overlay(w PLACING 'q' FROM p FOR 1)
		WHEN 1 THEN overlay(w PLACING '' FROM p FOR 1)
		ELSE overlay(w PLACING 'z' FROM p FOR 0) END END
	FROM (SELECT CASE WHEN random() < 0.2 THEN 1 ELSE sift_diff.rand_int(2, greatest(length(w), 2)) END AS p) x
$$;

CREATE OR REPLACE FUNCTION sift_diff.phrase_from_doc() RETURNS text LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	t text[];
	n int;
	i int;
BEGIN
	FOR attempt IN 1 .. 5 LOOP
		SELECT sift_ref.tokens(body) INTO t FROM diff_docs
		WHERE id = (SELECT sift_diff.rand_int(1, max(id)) FROM diff_docs);
		CONTINUE WHEN coalesce(cardinality(t), 0) < 3;
		n := sift_diff.rand_int(2, 3);
		i := sift_diff.rand_int(1, cardinality(t) - n + 1);
		t := t[i:i + n - 1];
		IF random() < 0.2 THEN
			t := array(SELECT x FROM unnest(t) WITH ORDINALITY u(x, o) ORDER BY o DESC);
		END IF;
		RETURN array_to_string(t, ' ');
	END LOOP;
	RETURN NULL;
END $$;

CREATE OR REPLACE FUNCTION sift_diff.gen_atom() RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	p float8 := random();
	w text := sift_diff.pick_word(random() < 0.5);
	f text := sift_ref.fold(w);
	k int;
BEGIN
	IF p < 0.65 THEN
		RETURN jsonb_build_object('op', 'word', 'src', sift_diff.surface(w));
	ELSIF p < 0.75 THEN
		RETURN jsonb_build_object('op', 'prefix', 'src', sift_diff.esc(left(f, sift_diff.rand_int(3, 5))) || '*');
	ELSIF p < 0.82 AND length(f) >= 4 THEN
		k := sift_diff.rand_int(2, length(f) - 1);
		RETURN jsonb_build_object('op', 'wildcard', 'src', sift_diff.esc(left(f, k - 1))
			|| CASE WHEN random() < 0.5 THEN '?' ELSE '*' END || sift_diff.esc(substr(f, k + 1)));
	ELSIF p < 0.93 THEN
		k := sift_diff.rand_int(1, 2);
		RETURN jsonb_build_object('op', 'fuzzy', 'dist', k,
			'src', sift_diff.esc(CASE WHEN k = 2 AND random() < 0.5 THEN sift_diff.typo(sift_diff.typo(w)) ELSE sift_diff.typo(w) END));
	END IF;
	RETURN jsonb_build_object('op', 'regex', 'text', CASE WHEN random() < 0.5
		THEN sift_ref.re_escape(f) || '|' || sift_ref.re_escape(sift_ref.fold(sift_diff.pick_word(false)))
		ELSE sift_ref.re_escape(left(f, 3)) || '.*' END);
END $$;

CREATE OR REPLACE FUNCTION sift_diff.gen_leaf() RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	p float8 := random();
	ph text;
BEGIN
	IF p < 0.08 THEN
		RETURN jsonb_build_object('op', 'word', 'src', 'zq' || sift_diff.rand_int(0, 999));
	ELSIF p < 0.25 THEN
		ph := sift_diff.phrase_from_doc();
		IF ph IS NOT NULL THEN
			RETURN jsonb_build_object('op', 'phrase', 'text', ph);
		END IF;
	ELSIF p < 0.30 THEN
		RETURN jsonb_build_object('op', 'word', 'src', sift_diff.esc(sift_diff.pick_word(true))
			|| (ARRAY['-', '\(', '.', '\''', '\/'])[sift_diff.rand_int(1, 5)] || sift_diff.esc(sift_diff.pick_word(false)));
	END IF;
	RETURN sift_diff.gen_atom();
END $$;

CREATE OR REPLACE FUNCTION sift_diff.gen_node(d int DEFAULT 1) RETURNS jsonb LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	p float8 := random();
	args jsonb := '[]';
	node jsonb;
BEGIN
	IF d >= 4 OR p < 0.45 THEN
		node := sift_diff.gen_leaf();
	ELSIF p < 0.93 THEN
		FOR i IN 1 .. sift_diff.rand_int(2, 3) LOOP
			args := args || jsonb_build_array(sift_diff.gen_node(d + 1));
		END LOOP;
		IF p < 0.72 AND random() < 0.4 THEN
			args := args || jsonb_build_array(jsonb_build_object('op', 'not', 'args', jsonb_build_array(sift_diff.gen_node(d + 1))));
		END IF;
		node := jsonb_build_object('op', CASE WHEN p < 0.72 THEN 'and' ELSE 'or' END, 'args', args);
	ELSE
		FOR i IN 1 .. sift_diff.rand_int(2, 4) LOOP
			args := args || jsonb_build_array(sift_diff.gen_atom());
		END LOOP;
		node := jsonb_build_object('op', 'any', 'args', args);
	END IF;
	IF random() < 0.1 THEN
		node := node || jsonb_build_object('boost', (ARRAY['2', '0.5', '1.5', '3', '0', '10', '1.1'])[sift_diff.rand_int(1, 7)]);
	END IF;
	RETURN node;
END $$;

-- Rendered query; must parse with the reference (otherwise the generator is wrong)
CREATE OR REPLACE FUNCTION sift_diff.gen_query() RETURNS text LANGUAGE plpgsql VOLATILE AS $$
DECLARE
	q text := sift_ref.render(sift_diff.gen_node(), random() < 0.5);
BEGIN
	PERFORM sift_ref.parse(q);
	RETURN q;
END $$;

CREATE OR REPLACE FUNCTION sift_diff.gen_queries(seed float8, n int) RETURNS SETOF text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM setseed(seed);
	RETURN QUERY SELECT unnest(ARRAY[sift_diff.esc((SELECT word FROM diff_vocab WHERE rank = 1)),
		(SELECT word FROM diff_vocab WHERE rank = 5), '+++', '"" ' || sift_diff.esc((SELECT word FROM diff_vocab WHERE rank = 2)),
		sift_diff.esc((SELECT word FROM diff_vocab WHERE rank = 1)) || ' OR ' || sift_diff.esc((SELECT word FROM diff_vocab WHERE rank = 2))]);
	FOR i IN 1 .. n LOOP
		RETURN NEXT sift_diff.gen_query();
	END LOOP;
END $$;

-- Engine under test. Stage selects the plan shape for the real engine.
CREATE OR REPLACE FUNCTION sift_diff.plan_for(stage text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_seqscan', (stage <> 'bitmap')::text, true),
		set_config('enable_bitmapscan', (stage <> 'seqscan')::text, true),
		set_config('enable_indexscan', (stage <> 'seqscan')::text, true),
		set_config('sift.enable_scan', (stage NOT IN ('seqscan', 'bitmap'))::text, true),
		set_config('sift.enable_count', (stage NOT IN ('seqscan', 'bitmap'))::text, true);
END $$;

CREATE OR REPLACE FUNCTION sift_diff.with_mutant(m text) RETURNS void LANGUAGE sql
AS $$ SELECT set_config('sift_ref.mutant', m, true) $$;

CREATE OR REPLACE FUNCTION sift_diff.e_set(stage text, q text) RETURNS tid[] LANGUAGE plpgsql AS $$
DECLARE
	eng text := sift_diff.setting('engine', 'sift');
	tbl text := sift_diff.setting('tbl', 'diff_docs');
	idx text := sift_diff.setting('idx');
	plan text;
	r tid[];
BEGIN
	IF eng LIKE 'mutant:%' THEN
		PERFORM sift_diff.with_mutant(substr(eng, 8));
		r := array(SELECT ctid FROM sift_ref.search(q) ORDER BY 1);
		PERFORM sift_diff.with_mutant('');
		RETURN r;
	ELSIF stage = 'bitmap' AND idx = '' THEN
		RAISE EXCEPTION 'sift index not implemented yet (CREATE INDEX failed)' USING ERRCODE = 'feature_not_supported';
	END IF;
	PERFORM sift_diff.plan_for(stage);
	IF stage = 'bitmap' THEN
		EXECUTE format('EXPLAIN (FORMAT JSON) SELECT ctid FROM %s WHERE body @@> %L', tbl, q) INTO plan;
		IF plan NOT LIKE format('%%"Index Name": "%s"%%', idx) THEN
			RAISE EXCEPTION 'plan does not use %: %', idx, plan USING ERRCODE = 'SD001';
		END IF;
	END IF;
	EXECUTE format('SELECT array(SELECT ctid FROM %s WHERE body @@> %L ORDER BY 1)', tbl, q) INTO r;
	PERFORM sift_diff.plan_for('default');
	RETURN r;
END $$;

CREATE OR REPLACE FUNCTION sift_diff.e_count(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	eng text := sift_diff.setting('engine', 'sift');
	n bigint;
BEGIN
	IF eng LIKE 'mutant:%' THEN
		PERFORM sift_diff.with_mutant(substr(eng, 8));
		n := sift_ref.count(q);
		PERFORM sift_diff.with_mutant('');
		RETURN n;
	END IF;
	EXECUTE format('SELECT count(*) FROM %s WHERE body @@> %L', sift_diff.setting('tbl', 'diff_docs'), q) INTO n;
	RETURN n;
END $$;

CREATE OR REPLACE FUNCTION sift_diff.e_topk(q text, k int, OUT ctids tid[], OUT metrics_a real[]) LANGUAGE plpgsql AS $$
DECLARE
	eng text := sift_diff.setting('engine', 'sift');
BEGIN
	IF eng LIKE 'mutant:%' THEN
		PERFORM sift_diff.with_mutant(substr(eng, 8));
		SELECT array_agg(t.ctid ORDER BY t.rank), array_agg(t.score ORDER BY t.rank) INTO ctids, metrics_a FROM sift_ref.topk(q, k) t;
		PERFORM sift_diff.with_mutant('');
		RETURN;
	END IF;
	EXECUTE format('SELECT array_agg(c ORDER BY n), array_agg(s ORDER BY n) FROM (SELECT row_number() OVER () AS n, c, s
		FROM (SELECT ctid AS c, sift.score(ctid) AS s FROM %s WHERE body @@> %L ORDER BY sift.score(ctid) DESC LIMIT %s) x) y',
		sift_diff.setting('tbl', 'diff_docs'), q, k) INTO ctids, metrics_a;
END $$;

CREATE OR REPLACE FUNCTION sift_diff.set_diff(ref tid[], got tid[]) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN m.n + e.n > 0 THEN format('reference %s rows, engine %s; missing %s %s; extra %s %s',
		cardinality(ref), cardinality(got), m.n, m.s, e.n, e.s) END
	FROM (SELECT count(*) AS n, (array_agg(t ORDER BY t))[1:3]::text AS s FROM (SELECT unnest(ref) EXCEPT SELECT unnest(got)) x(t)) m,
		(SELECT count(*) AS n, (array_agg(t ORDER BY t))[1:3]::text AS s FROM (SELECT unnest(got) EXCEPT SELECT unnest(ref)) y(t)) e
$$;

CREATE OR REPLACE FUNCTION sift_diff.hex(s real[]) RETURNS text[] LANGUAGE sql IMMUTABLE
AS $$ SELECT array(SELECT encode(float4send(x), 'hex') FROM unnest(s) WITH ORDINALITY u(x, n) ORDER BY n) $$;

-- Top-k with a filter on another column: the reference top-k restricted to the same rows (the
-- scores stay those of the whole index). Filters an index serves run with the bitmap pushed down into the
-- ranker, whatever the planner would choose.
CREATE OR REPLACE FUNCTION sift_diff.filtered_diff(q text, k int, flt text, pushdown bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	tbl text := sift_diff.setting('tbl', 'diff_docs');
	ref record;
	got record;
BEGIN
	EXECUTE format('SELECT array_agg(r.ctid ORDER BY r.rank) AS ctids, array_agg(r.score ORDER BY r.rank) AS metrics_a
		FROM (SELECT t.ctid, t.score, t.rank FROM sift_ref.topk(%L, 100000000) t JOIN %s d ON d.ctid = t.ctid
			WHERE %s ORDER BY t.rank LIMIT %s) r', q, tbl, flt, k) INTO ref;
	PERFORM set_config('sift.filter_pushdown', CASE WHEN pushdown THEN 'force' ELSE 'on' END, true);
	EXECUTE format('SELECT array_agg(c ORDER BY n) AS ctids, array_agg(s ORDER BY n) AS metrics_a FROM (SELECT row_number() OVER () AS n, c, s
		FROM (SELECT ctid AS c, sift.score(ctid) AS s FROM %s WHERE body @@> %L AND %s
			ORDER BY sift.score(ctid) DESC LIMIT %s) x) y', tbl, q, flt, k) INTO got;
	PERFORM set_config('sift.filter_pushdown', 'on', true);
	IF ref.ctids IS DISTINCT FROM got.ctids OR sift_diff.hex(ref.metrics_a) IS DISTINCT FROM sift_diff.hex(got.metrics_a) THEN
		RETURN format('top-%s under %s%s: reference %s, engine %s', k, flt, CASE WHEN pushdown THEN ' (pushdown)' END, ref::text, got::text);
	END IF;
	RETURN NULL;
END $$;

-- An unindexed filter, then ranges of the id (btree and BRIN): about a third, a few percent, a few rows
CREATE OR REPLACE FUNCTION sift_diff.filtered_diffs(q text, k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	top int;
	lo int;
BEGIN
	SELECT max(id) INTO top FROM diff_docs;
	lo := sift_diff.rand_int(1, greatest(top, 1));
	RETURN coalesce(sift_diff.filtered_diff(q, k, 'id % 3 = 0', false),
		sift_diff.filtered_diff(q, k, format('id < %s', sift_diff.rand_int(1, greatest(top / 3, 1))), true),
		sift_diff.filtered_diff(q, k, format('id BETWEEN %s AND %s', lo, lo + greatest(top / 50, 3)), true),
		sift_diff.filtered_diff(q, k, format('id >= %s AND id %% 5 <> 0', lo), true));
END $$;

-- Clean state: row-for-row, bit-equal scores. Otherwise statistics may count
-- unvacuumed versions, so only membership, order and length are checked.
CREATE OR REPLACE FUNCTION sift_diff.ranked_diff(q text, k int, clean bool, ref_set tid[]) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ref record;
	got record;
	i int;
BEGIN
	SELECT array_agg(t.ctid ORDER BY t.rank) AS ctids, array_agg(t.score ORDER BY t.rank) AS metrics_a INTO ref
	FROM sift_ref.topk(q, k) t;
	SELECT * INTO got FROM sift_diff.e_topk(q, k);
	IF clean THEN
		FOR i IN 1 .. greatest(coalesce(cardinality(ref.ctids), 0), coalesce(cardinality(got.ctids), 0)) LOOP
			IF ref.ctids[i] IS DISTINCT FROM got.ctids[i] OR float4send(ref.metrics_a[i]) IS DISTINCT FROM float4send(got.metrics_a[i]) THEN
				RETURN format('rank %s: reference %s score %s (%s), engine %s score %s (%s)', i,
					ref.ctids[i], ref.metrics_a[i], sift_diff.hex(ref.metrics_a[i:i])::text,
					got.ctids[i], got.metrics_a[i], sift_diff.hex(got.metrics_a[i:i])::text);
			END IF;
		END LOOP;
		RETURN CASE WHEN sift_diff.setting('engine', 'sift') = 'sift' THEN sift_diff.filtered_diffs(q, k) END;
	ELSIF coalesce(cardinality(got.ctids), 0) <> least(k, cardinality(ref_set)) THEN
		RETURN format('%s rows, expected %s', coalesce(cardinality(got.ctids), 0), least(k, cardinality(ref_set)));
	ELSIF NOT ref_set @> coalesce(got.ctids, '{}') THEN
		RETURN 'returned non-matching rows ' || (SELECT array_agg(t) FROM unnest(got.ctids) t WHERE t <> ALL (ref_set))::text;
	END IF;
	FOR i IN 2 .. coalesce(cardinality(got.ctids), 0) LOOP
		IF got.metrics_a[i] > got.metrics_a[i - 1] OR got.metrics_a[i] = got.metrics_a[i - 1] AND got.ctids[i] < got.ctids[i - 1] THEN
			RETURN format('order broken at rank %s: %s', i, got::text);
		END IF;
	END LOOP;
	RETURN NULL;
END $$;

-- Deparse must be a fixpoint and keep the reference meaning: the same matches
-- and scores (up to float4 rounding, since normalization may reorder leaves)
CREATE OR REPLACE FUNCTION sift_diff.parse_diff(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	d text := sift.parse_query(q);
	d2 text := sift.parse_query(d);
	diff text;
BEGIN
	IF d2 IS DISTINCT FROM d THEN
		RETURN format('deparse not a fixpoint: %L -> %L', d, d2);
	END IF;
	SELECT format('deparse %L changes the reference results: %s query %s, deparse %s', d, coalesce(a.ctid, b.ctid), a.score, b.score)
	INTO diff
	FROM sift_ref.search(q) a FULL JOIN sift_ref.search(d) b ON a.ctid = b.ctid
	WHERE a.ctid IS NULL OR b.ctid IS NULL OR abs(a.score - b.score) > 1e-5 * greatest(abs(a.score), abs(b.score))
	LIMIT 1;
	RETURN diff;
END $$;

-- Runs the engine side of a stage only (used when the reference raised an error)
CREATE OR REPLACE FUNCTION sift_diff.e_run(stage text, q text, k int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	CASE stage
		WHEN 'parse' THEN PERFORM sift.parse_query(q);
		WHEN 'count' THEN PERFORM sift_diff.e_count(q);
		WHEN 'ranked' THEN PERFORM sift_diff.e_topk(q, k);
		ELSE PERFORM sift_diff.e_set(stage, q);
	END CASE;
END $$;

-- Does the query exceed the expansion limit sift applies to dictionary expansion
CREATE OR REPLACE FUNCTION sift_diff.over_limit(q text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift_ref.max_expansions', '', true);
	PERFORM sift_ref.search(q);
	PERFORM set_config('sift_ref.max_expansions', '1000000000', true);
	RETURN false;
EXCEPTION WHEN program_limit_exceeded THEN
	RETURN SQLERRM ~ 'expansions';
END $$;

-- Per-query stages: (stage, ok | fail | skip, detail). A stage is skipped only
-- while the engine reports "not implemented"; errors must match the
-- reference's. Results are compared without the expansion limit: a query over
-- it may instead fail with 54000 (dictionary expansion), outside the parse stage;
-- so may any query on an index that is not clean, whose dictionaries keep the terms of deleted documents.
CREATE OR REPLACE FUNCTION sift_diff.check(q text, k int, clean bool, only_stage text DEFAULT NULL)
RETURNS TABLE (stage text, status text, detail text) LANGUAGE plpgsql AS $$
DECLARE
	stages text[] := CASE WHEN sift_diff.setting('engine', 'sift') = 'sift'
		THEN '{parse,seqscan,bitmap,count,ranked}' ELSE '{seqscan,count,ranked}' END;
	ref_err text;
	ref_set tid[];
	over bool := false;
BEGIN
	PERFORM set_config('sift_ref.max_expansions', '1000000000', true);
	BEGIN
		ref_set := array(SELECT s.ctid FROM sift_ref.search(q) s ORDER BY 1);
		over := sift_diff.over_limit(q);
	EXCEPTION WHEN OTHERS THEN
		ref_err := SQLSTATE;
	END;
	FOREACH stage IN ARRAY stages LOOP
		CONTINUE WHEN only_stage IS NOT NULL AND stage <> only_stage;
		BEGIN
			IF ref_err IS NOT NULL THEN
				PERFORM sift_diff.e_run(stage, q, k);
				detail := 'engine succeeded, reference raised ' || ref_err;
			ELSE
				detail := CASE stage
					WHEN 'parse' THEN sift_diff.parse_diff(q)
					WHEN 'count' THEN (SELECT CASE WHEN c <> cardinality(ref_set) THEN format('count %s, reference %s', c, cardinality(ref_set)) END
						FROM sift_diff.e_count(q) c)
					WHEN 'ranked' THEN sift_diff.ranked_diff(q, k, clean, ref_set)
					ELSE sift_diff.set_diff(ref_set, sift_diff.e_set(stage, q)) END;
			END IF;
			status := CASE WHEN detail IS NULL THEN 'ok' ELSE 'fail' END;
		EXCEPTION WHEN OTHERS THEN
			status := CASE WHEN SQLERRM ~ 'not implemented' THEN 'skip'
				WHEN SQLSTATE = ref_err OR (over OR NOT clean) AND SQLSTATE = '54000' AND stage <> 'parse' THEN 'ok' ELSE 'fail' END;
			detail := CASE WHEN status <> 'ok' THEN SQLSTATE || ': ' || left(SQLERRM, 300) END;
		END;
		RETURN NEXT;
	END LOOP;
END $$;

-- Per-round stages: verify, statistics, tokenizer agreement, reference self-consistency
CREATE OR REPLACE FUNCTION sift_diff.check_round(clean bool, queries text[])
RETURNS TABLE (stage text, status text, detail text) LANGUAGE plpgsql AS $$
DECLARE
	idx text := sift_diff.setting('idx');
	tbl text := sift_diff.setting('tbl', 'diff_docs');
	real_engine bool := sift_diff.setting('engine', 'sift') = 'sift';
	bad bigint;
	other bigint;
BEGIN
	-- the reference checks itself without the expansion limit, as check() compares results
	PERFORM set_config('sift_ref.max_expansions', '1000000000', true);
	FOREACH stage IN ARRAY ARRAY['verify', 'stats', 'tokenize', 'refself'] LOOP
		CONTINUE WHEN NOT real_engine AND stage <> 'refself';
		BEGIN
			detail := NULL;
			CASE stage
				WHEN 'verify' THEN
					IF idx = '' THEN
						RAISE EXCEPTION 'sift index not implemented yet' USING ERRCODE = 'feature_not_supported';
					END IF;
					PERFORM sift.verify(idx::regclass, true);
				WHEN 'stats' THEN
					IF idx = '' OR NOT clean THEN
						RAISE EXCEPTION 'not implemented yet: needs an index in a clean state' USING ERRCODE = 'feature_not_supported';
					END IF;
					SELECT format('segments ndocs %s sum_dl %s, reference %s %s', s.n, s.sum_dl, r.n, r.sum_dl) INTO detail
					FROM (SELECT sum(ndocs) AS n, sum(sum_dl) AS sum_dl FROM sift.segments(idx::regclass)) s, sift_ref.stats() r
					WHERE (s.n, s.sum_dl) IS DISTINCT FROM (r.n, r.sum_dl);
				WHEN 'tokenize' THEN
					PERFORM sift.tokenize('probe');
					EXECUTE format('SELECT count(*) FILTER (WHERE ascii), count(*) FILTER (WHERE NOT ascii) FROM (
						SELECT body ~ ''^[\x01-\x7f]*$'' AS ascii, body FROM %s WHERE body IS NOT NULL ORDER BY id LIMIT 500) d
						WHERE (SELECT coalesce(array_agg(token ORDER BY pos), ''{}'') FROM sift.tokenize(body))
							IS DISTINCT FROM sift_ref.tokens(body)', tbl)
					INTO bad, other;
					detail := CASE WHEN bad > 0 THEN format('%s ASCII documents tokenize differently', bad) END;
					IF bad = 0 AND other > 0 THEN
						status := 'warn';
						detail := format('%s non-ASCII documents differ from the built-in fallback', other);
						RETURN NEXT;
						CONTINUE;
					END IF;
				WHEN 'refself' THEN
					SELECT string_agg(q, ' | ') INTO detail FROM unnest(queries[1:8]) q WHERE NOT sift_ref.selfcheck(q);
			END CASE;
			status := CASE WHEN detail IS NULL THEN 'ok' ELSE 'fail' END;
		EXCEPTION WHEN OTHERS THEN
			status := CASE WHEN SQLERRM ~ 'not implemented' THEN 'skip' ELSE 'fail' END;
			detail := SQLSTATE || ': ' || left(SQLERRM, 300);
		END;
		RETURN NEXT;
	END LOOP;
END $$;

-- Minimization: greedy AST shrinking, then ddmin over the corpus rows
CREATE OR REPLACE FUNCTION sift_diff.shrinks(node jsonb) RETURNS SETOF jsonb LANGUAGE plpgsql AS $$
DECLARE
	n int := jsonb_array_length(coalesce(node -> 'args', '[]'));
	c jsonb;
	w text[];
BEGIN
	IF node ? 'boost' THEN
		RETURN NEXT node - 'boost';
	END IF;
	FOR i IN 0 .. n - 1 LOOP
		c := node -> 'args' -> i;
		RETURN NEXT CASE WHEN c ->> 'op' = 'not' THEN c -> 'args' -> 0 ELSE c END;
		IF n > 1 THEN
			RETURN NEXT jsonb_set(node, '{args}', (node -> 'args') - i);
		END IF;
		FOR c IN SELECT sift_diff.shrinks(node -> 'args' -> i) LOOP
			RETURN NEXT jsonb_set(node, ARRAY['args', i::text], c);
		END LOOP;
	END LOOP;
	CASE node ->> 'op'
		WHEN 'phrase' THEN
			w := regexp_split_to_array(node ->> 'text', '\s+');
			IF cardinality(w) > 1 THEN
				RETURN NEXT node || jsonb_build_object('text', array_to_string(w[2:], ' '));
				RETURN NEXT node || jsonb_build_object('text', array_to_string(w[:cardinality(w) - 1], ' '));
			END IF;
		WHEN 'fuzzy' THEN
			RETURN NEXT jsonb_build_object('op', 'word', 'src', node ->> 'src');
		WHEN 'prefix' THEN
			RETURN NEXT jsonb_build_object('op', 'word', 'src', left(node ->> 'src', -1));
		ELSE
	END CASE;
END $$;

CREATE OR REPLACE FUNCTION sift_diff.fails(q text, stage text, k int, clean bool) RETURNS bool LANGUAGE sql AS $$
	SELECT EXISTS (SELECT 1 FROM sift_diff.check(q, k, clean, stage) c WHERE c.status = 'fail')
$$;

CREATE OR REPLACE FUNCTION sift_diff.minimize(q text, stage text, k int, clean bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	cand text;
	improved bool := true;
BEGIN
	WHILE improved LOOP
		improved := false;
		FOR cand IN SELECT DISTINCT r FROM (SELECT sift_ref.render(s) AS r FROM sift_diff.shrinks(sift_ref.parse(q)) s) x
			WHERE length(r) < length(q) ORDER BY 1 LOOP
			BEGIN
				PERFORM sift_ref.parse(cand);
			EXCEPTION WHEN OTHERS THEN
				CONTINUE;
			END;
			IF sift_diff.fails(cand, stage, k, clean) THEN
				q := cand;
				improved := true;
				EXIT;
			END IF;
		END LOOP;
	END LOOP;
	RETURN q;
END $$;

-- Copy of the rows in ids as diff_min (+ sift index when the engine has one), made current
CREATE OR REPLACE FUNCTION sift_diff.use_subset(src text, ids int[]) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DROP TABLE IF EXISTS diff_min;
	EXECUTE format('CREATE TABLE diff_min WITH (autovacuum_enabled = off) AS SELECT * FROM %s WHERE id = ANY (%L) ORDER BY id', src, ids);
	PERFORM set_config('sift_diff.tbl', 'diff_min', true);
	IF sift_diff.setting('idx') <> '' THEN
		CREATE INDEX diff_min_idx ON diff_min USING sift (body);
		PERFORM set_config('sift_diff.idx', 'diff_min_idx', true);
	END IF;
	PERFORM sift_ref.load('diff_min', 'body');
END $$;

CREATE OR REPLACE FUNCTION sift_diff.minimize_corpus(q text, stage text, k int, clean bool, max_trials int DEFAULT 60)
RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE
	src text := sift_diff.setting('tbl', 'diff_docs');
	ids int[];
	cand int[];
	n int := 2;
	chunk int;
	trials int := 0;
	reduced bool;
BEGIN
	EXECUTE format('SELECT array_agg(id ORDER BY id) FROM %s', src) INTO ids;
	WHILE trials < max_trials AND cardinality(ids) >= 2 LOOP
		chunk := ceil(cardinality(ids)::numeric / n);
		reduced := false;
		FOR i IN 0 .. n - 1 LOOP
			cand := ids[:i * chunk] || ids[(i + 1) * chunk + 1:];
			CONTINUE WHEN cardinality(cand) = cardinality(ids);
			trials := trials + 1;
			PERFORM sift_diff.use_subset(src, cand);
			IF sift_diff.fails(q, stage, k, clean) THEN
				ids := cand;
				n := greatest(n - 1, 2);
				reduced := true;
				EXIT;
			END IF;
			EXIT WHEN trials >= max_trials;
		END LOOP;
		IF NOT reduced THEN
			EXIT WHEN n >= cardinality(ids);
			n := least(n * 2, cardinality(ids));
		END IF;
	END LOOP;
	PERFORM sift_diff.use_subset(src, ids);
	RETURN ids;
END $$;

-- Query then corpus minimization of a failure; returns a repro script
CREATE OR REPLACE FUNCTION sift_diff.minimize_all(q text, stage text, k int, clean bool, max_trials int)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	m text := sift_diff.minimize(q, stage, k, clean);
BEGIN
	PERFORM sift_diff.minimize_corpus(m, stage, k, clean, max_trials);
	RETURN format(E'-- original query: %s\n-- minimized query: %s\n', q, m) || sift_diff.repro(m, stage, k, clean);
END $$;

-- Self-contained repro script for the current subset (diff_min)
CREATE OR REPLACE FUNCTION sift_diff.repro(q text, stage text, k int, clean bool) RETURNS text LANGUAGE sql AS $$
	SELECT format(E'-- sift differential failure: stage %s, engine %s, k %s, clean %s\n-- %s\n'
		'-- Run with scripts/difftest.sh --repro FILE (loads the reference and harness).\n'
		'CREATE TABLE repro (id int, body text, pad text) WITH (autovacuum_enabled = off);\n%s\n%s'
		'SELECT set_config(''sift_diff.tbl'', ''repro'', false), set_config(''sift_diff.engine'', %L, false);\n'
		'SELECT sift_ref.load(''repro'', ''body'');\nSELECT * FROM sift_diff.check(%L, %s, %L, %L);\n',
		stage, sift_diff.setting('engine', 'sift'), k, clean,
		(SELECT string_agg(format('%s: %s', c.status, c.detail), E'\n-- ') FROM sift_diff.check(q, k, clean, stage) c),
		(SELECT string_agg(format('INSERT INTO repro VALUES (%s, %L, repeat(''x'', %s));', id, body, length(pad)), E'\n' ORDER BY id)
			FROM diff_min),
		CASE WHEN sift_diff.setting('idx') <> '' THEN E'CREATE INDEX repro_idx ON repro USING sift (body);\n'
			'SELECT set_config(''sift_diff.idx'', ''repro_idx'', false);\n' ELSE '' END,
		sift_diff.setting('engine', 'sift'), q, k, clean, stage)
$$;

RESET check_function_bodies;
RESET client_min_messages;
\set ECHO :sift_diff_echo
