-- key_columns = 'auto' is decided again when every segment of the index is merged: after churn (new row versions in the tail of the heap) a unit holds many keys, the
-- cube no longer holds the column in it, and the merge adds the key list that a REINDEX would have chosen. A column the cube holds in 40% of the units or more is not
-- added, an explicit key_columns is never touched, and a second merge changes nothing.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.col_auto_merge = off;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION mk_opt(idx regclass) RETURNS text LANGUAGE sql AS $$ SELECT coalesce(array_to_string(reloptions, ','), '') FROM pg_class WHERE oid = idx $$;
CREATE FUNCTION mk_held(idx regclass) RETURNS text LANGUAGE sql AS $$ SELECT held FROM sift.index_advice(idx) WHERE leaf = 'set_id' $$;
CREATE FUNCTION mk_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- clustered by set_id: about 6 owners in a unit, which the cube holds
CREATE TABLE mk (id int, set_id int, v int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO mk SELECT i, i / 500, i % 100, repeat('p', 200) FROM generate_series(0, 29999) i;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) mk;
CREATE INDEX mk_auto ON mk USING sift_col (id, set_id, v);
CREATE INDEX mk_explicit ON mk USING sift_col (id, set_id, v) WITH (key_columns = 'v');
CREATE INDEX mk_off ON mk USING sift_col (id, set_id, v) WITH (key_columns = 'off');
SELECT mk_opt('mk_auto') AS auto, mk_held('mk_auto') AS held;

-- two rounds of non-HOT updates that scatter the owners, each promoted into its own segment
UPDATE mk SET set_id = (id::bigint * 7919 % 600)::int;
VACUUM mk;
SELECT sift.col_promote('mk_auto') > 0 AS promoted_auto, sift.col_promote('mk_explicit') > 0 AS promoted_explicit, sift.col_promote('mk_off') > 0 AS promoted_off;
UPDATE mk SET set_id = (id::bigint * 104729 % 600)::int;
VACUUM mk;
SELECT sift.col_promote('mk_auto') > 0 AS promoted_auto, sift.col_promote('mk_explicit') > 0 AS promoted_explicit, sift.col_promote('mk_off') > 0 AS promoted_off;

SELECT sift.col_merge('mk_auto', true) > 0 AS merged_auto, sift.col_merge('mk_explicit', true) > 0 AS merged_explicit, sift.col_merge('mk_off', true) > 0 AS merged_off;
SELECT mk_opt('mk_auto') AS auto, mk_held('mk_auto') AS held;
SELECT mk_opt('mk_explicit') AS explicit, mk_opt('mk_off') AS off;
-- nothing alternates: the next merge keeps the choice
SELECT sift.col_merge('mk_auto', true) >= 0 AS merged_again;
SELECT mk_opt('mk_auto') AS auto, mk_held('mk_auto') AS held;

-- the answers are the plain plan's
SELECT mk_rows(q, true) = mk_rows(q, false) AS same FROM (VALUES
	('SELECT count(DISTINCT id), count(*) FROM mk WHERE set_id = 5'),
	('SELECT set_id, count(DISTINCT id) FROM mk WHERE set_id IN (3, 77, 410) GROUP BY 1')) t(q);

SET client_min_messages = warning;
DROP TABLE mk;
DROP FUNCTION mk_opt(regclass), mk_held(regclass), mk_rows(text, bool);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
