\set ECHO none
-- Shared by the specs of a scope whose pairs may be several rows (\ir test/scope_multiset_helpers.sql; each spec drops probe, shapes, ms_run and changed_rows at its end): the read shapes of the scope
-- units against the statements they stand for, run over the events f of a scope index.
--
-- probe(tbl, idx, pairs): for each shape, the rows of the read of the scope units (the testing marker) against the plain statement over the relation of the pairs (a sub-select when the pairs
--   are the rows of a partial btree), md5 equal or not (verdict), the fallback reason or "units" (route), what the delta read (delta: none, the buffers, or a patch of changed dim_a_id), and whether
--   the rows read are within the bounds of the journal's dim_a_id and the buffers (bounded).
-- the rows of idx's table whose dim_a_id was named by the journal of the pairs since the scope segments were filled
CREATE FUNCTION changed_rows(tbl text, idx text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	t record;
	n bigint;
BEGIN
	SELECT lineage, horizon INTO t FROM sift._test_col_conn_stamps(idx::regclass) WHERE ordinal = 0 ORDER BY horizon::text::bigint LIMIT 1;
	EXECUTE format('SELECT count(*) FROM %I f WHERE f.dim_a_id IN (SELECT key FROM sift._test_scope_journal_changes(%L::regclass, 0, %s, %L::xid))', tbl, idx, t.lineage, t.horizon) INTO n;
	RETURN n;
END $$;

-- q: a read over the scope units, plain: the statement it stands for. route: the fallback reason or "units"; delta: what the delta read; bounded: the rows read are within the rows of the journal's dim_a_id
-- and the buffers, the copies at most those rows times the most keys a dim_a_id has
CREATE FUNCTION ms_run(q text, plain text, tbl text, idx text, OUT same boolean, OUT route text, OUT delta text, OUT bounded boolean) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
	changed bigint := changed_rows(tbl, idx);
	buffered bigint;
	patched int;
	d int;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || plain || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	SELECT coalesce(sum(rows), 0) INTO buffered FROM sift.col_segments(idx::regclass) WHERE kind = 'buffer';
	same := a = b;
	route := coalesce(jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}', CASE WHEN jsonb_path_exists(p, '$.**."Scope Units Read"') THEN 'units' ELSE 'no scope read' END);
	patched := coalesce(jsonb_path_query_first(p, '$.**."Scope Patched Rows"')::text::int, 0);
	d := coalesce(jsonb_path_query_first(p, '$.**."Scope Delta Rows"')::text::int, 0);
	delta := CASE WHEN d = 0 THEN 'none' WHEN patched = 0 THEN 'buffers' ELSE 'patch' END;
	bounded := patched <= changed AND d <= (patched + buffered) * 2;
END $$;

-- the read shapes of the scope units (the testing marker stands for the recognition of a semi join) and the statements they stand for; %s is the relation of the pairs
CREATE TABLE shapes (n int, label text, sel text, marker text, pred text, tail text);
INSERT INTO shapes VALUES
	(1, 'one key IN, count and sum', 'count(*), sum(amount)', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, 3)', 'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 3)', ''),
	(2, 'one key EXISTS, count and sum', 'count(*), sum(amount)', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, 4)',
		'EXISTS (SELECT 1 FROM %s m WHERE m.dim_a_id = f.dim_a_id AND m.tenant_id = 4)', ''),
	(3, 'a list of keys IN', 'count(*), sum(amount)', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, ''{3,5,7}''::int8[])',
		'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id IN (3, 5, 7))', ''),
	(4, 'one key, min and max', 'min(id), max(id), min(amount), count(DISTINCT f.dim_a_id)', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, 8)',
		'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 8)', ''),
	(5, 'ten keys IN, min and max', 'min(id), max(id)', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, ''{1,2,3,4,5,6,7,8,9,10}''::int8[])',
		'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id BETWEEN 1 AND 10)', ''),
	(6, 'top 5', 'id, created_at', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, 6)', 'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 6)',
		' ORDER BY created_at DESC, id DESC LIMIT 5');

-- tbl: the events, idx: their scope index, pairs: the relation of the pairs (a sub-select when its active pairs are a partial btree's)
CREATE FUNCTION probe(tbl text, idx text, pairs text) RETURNS TABLE (n int, label text, verdict text, route text, delta text, bounded boolean) LANGUAGE sql AS $$
	SELECT s.n, s.label, CASE r.same WHEN true THEN 'same' ELSE 'DIFFERENT' END, r.route, r.delta, r.bounded
	FROM shapes s, LATERAL ms_run(
		format('SELECT %s FROM %I f WHERE %s%s', s.sel, tbl, format(s.marker, idx), s.tail),
		format('SELECT %s FROM %I f WHERE %s%s', s.sel, tbl, format(s.pred, pairs), s.tail), tbl, idx) r
	ORDER BY s.n
$$;
\set ECHO all
