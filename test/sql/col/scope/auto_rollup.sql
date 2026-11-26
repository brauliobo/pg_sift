-- rollup_columns = 'auto' on a scope index (docs/COL-SCOPE.md "The rollup of the rows of a group"): the derived key of the scope is no candidate of the choice, so 'auto' takes the pair
-- of the plain twin, with the sums that are worth keeping (the integer columns that are not keys), written as 'auto: a, b: m' and said by CREATE INDEX; the key groups keep that rollup.
-- The default (no option) writes none for a scope index, an explicit list is kept as written and 'off' writes none. Counts and booleans only.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: rollup_columns and the rollup the scope units and key groups keep are this format's; Format 2.0 ignores rollup_columns
SET sift.test_create_format1 = on;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.test_col_unit_rows = 100;

CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT d, CASE WHEN d <= 60 THEN 1 WHEN d <= 66 THEN 2 ELSE 3 END FROM generate_series(1, 67) d;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');

-- ids are unique, dim_a_id is the key of the scope, score and kind have 10 and 4 values, amount is the one sum
CREATE FUNCTION make_events(t text, with_amount boolean DEFAULT true, n int DEFAULT 20000) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, dim_a_id int, created_at date, score int, kind text%s) WITH (autovacuum_enabled = false)', t, CASE WHEN with_amount THEN ', amount int' END);
	EXECUTE format('INSERT INTO %I SELECT i, 1 + (i * 7919) %% 67, date ''2026-01-01'' + (i / 40), 1 + i %% 10, ''k'' || (i %% 4)%s FROM generate_series(1, %s) i', t,
				   CASE WHEN with_amount THEN ', i % 500' END, n);
END $$;
CREATE FUNCTION make_index(t text, opts text DEFAULT '', with_amount boolean DEFAULT true) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (id, dim_a_id, created_at, score, kind%s) WITH (%sscope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'')',
				   t || '_col', t, CASE WHEN with_amount THEN ', amount' END, opts);
	EXECUTE format('REINDEX INDEX %I', t || '_col');
END $$;
SELECT make_events(t) FROM unnest(ARRAY['ev_plain', 'ev_default', 'ev_auto', 'ev_explicit', 'ev_other', 'ev_off']) t;
VACUUM (FREEZE, ANALYZE) ev_plain, ev_default, ev_auto, ev_explicit, ev_other, ev_off;
VACUUM (ANALYZE) tenant_scope;

CREATE FUNCTION rollup_option(i text) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = i AND o LIKE 'rollup_columns=%'), 'not set') $$;
-- the bytes of the pre-aggregates of the scope segments (the key groups hold the rollup there)
CREATE FUNCTION scope_preagg(i text) RETURNS bigint LANGUAGE sql AS
$$ SELECT sum(preagg_bytes) FROM sift.col_segments(i::regclass) WHERE kind = 'immutable' AND format >= 51 $$;

-- a plain index and the scope indexes of the same columns, by default and by 'auto': the plain one and 'auto' choose the same pair, 'auto' on a scope index with the sums; the default none
CREATE INDEX ev_plain_col ON ev_plain USING sift_col (id, dim_a_id, created_at, score, kind, amount);
SET client_min_messages = notice;
SELECT make_index('ev_default');
SELECT make_index('ev_auto', 'rollup_columns = ''auto'', ');
SET client_min_messages = warning;
SELECT rollup_option('ev_plain_col') AS plain, rollup_option('ev_default_col') AS scope_default, rollup_option('ev_auto_col') AS scope_auto;
SELECT choice, kind, value FROM sift.index_choices('ev_auto_col') WHERE slug = 'rows-rollup';
SELECT count(*) AS default_choices FROM sift.index_choices('ev_default_col') WHERE slug = 'rows-rollup';

-- the declaration is kept as written, and the same one is the same bytes as the choice
SELECT make_index('ev_explicit', 'rollup_columns = ''score, kind: amount'', ');
SELECT make_index('ev_other', 'rollup_columns = ''kind, score'', ');
SELECT make_index('ev_off', 'rollup_columns = ''off'', ');
SELECT rollup_option('ev_explicit_col') AS explicit, rollup_option('ev_other_col') AS explicit_default_sums, rollup_option('ev_off_col') AS off;
SELECT scope_preagg('ev_explicit_col') = scope_preagg('ev_auto_col') AS explicit_is_the_choice,
       scope_preagg('ev_other_col') > scope_preagg('ev_auto_col') AS default_sums_cost_more,
       scope_preagg('ev_off_col') < scope_preagg('ev_auto_col') AS off_writes_none;
SELECT scope_preagg('ev_default_col') = scope_preagg('ev_off_col') AS default_is_off;
SELECT sift.col_verify(i, true) FROM unnest(ARRAY['ev_auto_col', 'ev_explicit_col', 'ev_off_col']) i;

-- a filter on both columns over the index with the rollup the choice wrote and over the one with none: md5 equal to the plain statement
CREATE FUNCTION probe(q text) RETURNS TABLE (same boolean) LANGUAGE plpgsql AS $$
DECLARE
	on_h text;
	off_h text;
BEGIN
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO on_h;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO off_h;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN QUERY SELECT on_h = off_h;
END $$;
CREATE TABLE cases (n int, label text, stmt text);
INSERT INTO cases VALUES
	(1, 'filter on both columns, big key', 'SELECT count(*) FROM {t} WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND score = 3 AND kind = ''k1'''),
	(2, 'sum of amount by kind of a score', 'SELECT kind, count(*), sum(amount) FROM {t} WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 1) AND score = 3 GROUP BY 1'),
	(3, 'filter on both columns, mid key', 'SELECT count(*), sum(amount) FROM {t} WHERE dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 2) AND score = 3 AND kind = ''k1''');
SELECT c.n, c.label, r.* FROM cases c, LATERAL probe(replace(c.stmt, '{t}', 'ev_auto')) r ORDER BY c.n;
SELECT c.n, r.same FROM cases c, LATERAL probe(replace(c.stmt, '{t}', 'ev_off')) r ORDER BY c.n;

-- a table with no sum worth keeping: the list ends at the colon, and the rollup of counts is written
SELECT make_events('ev_n', false);
VACUUM (FREEZE, ANALYZE) ev_n;
SELECT make_index('ev_n', 'rollup_columns = ''auto'', ', false);
SELECT rollup_option('ev_n_col');
SELECT sift.col_verify('ev_n_col', true);
ALTER INDEX ev_n_col SET (rollup_columns = 'score, kind:');
SELECT rollup_option('ev_n_col');

-- every byte of the block of a small index with that rollup (groups of 4 units), read as itself with one bit flipped: all refused
SELECT make_events('ev_s', false, 1500);
VACUUM (FREEZE, ANALYZE) ev_s;
SELECT make_index('ev_s', 'rollup_columns = ''auto'', superunit = 4, ', false);
SELECT sift.col_verify('ev_s_col', true);
SELECT supers > 0 AS has_blocks, damaged > 0 AS damages_made, refused = damaged AS all_refused, unnoticed = 0 AS none_unnoticed FROM sift._test_col_super_damage('ev_s_col');

DROP TABLE ev_plain, ev_default, ev_auto, ev_explicit, ev_other, ev_off, ev_n, ev_s, tenant_scope, cases;
DROP FUNCTION make_events(text, boolean, int), make_index(text, text, boolean), rollup_option(text), scope_preagg(text), probe(text);
DROP EXTENSION pg_sift_test CASCADE;
DROP EXTENSION pg_sift;
