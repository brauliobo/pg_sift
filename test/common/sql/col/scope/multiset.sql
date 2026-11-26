\set ECHO none
-- Shared by the specs of a scope whose pairs may be several rows (\ir test/common/sql/col/scope/multiset.sql; each spec drops probe, shapes and ms_run at its end): the read shapes of a scope
-- over the events f of a scope index.
--
-- probe(tbl, pairs): for each shape over the relation of the pairs (a sub-select when the pairs are the rows of a partial btree), the rows through the scope node, through sift with
--   sift.agg_scope off and through the normal plan, md5 equal or not (verdict), and the Scope Read of the node (route; none: not a scope read). A join counts a fact once for each row of
--   the pairs of its member with the key, IN and EXISTS once.
CREATE FUNCTION ms_run(q text, OUT same boolean, OUT route text) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
	c text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.agg_scope = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO c;
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_scope = on;
	same := a = b AND b = c;
	route := coalesce(jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}', 'none');
END $$;

-- the read shapes: the columns, the join of the pairs (%s is their relation) or none, the condition and the tail
CREATE TABLE shapes (n int, label text, sel text, joined text, pred text, tail text);
INSERT INTO shapes VALUES
	(1, 'one key IN, count and sum', 'count(*), sum(amount)', '', 'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 3)', ''),
	(2, 'one key EXISTS, count and sum', 'count(*), sum(amount)', '', 'EXISTS (SELECT 1 FROM %s m WHERE m.dim_a_id = f.dim_a_id AND m.tenant_id = 4)', ''),
	(3, 'a list of keys IN', 'count(*), sum(amount)', '', 'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id IN (3, 5, 7))', ''),
	(4, 'one key, min and max', 'min(id), max(id), min(amount), count(DISTINCT f.dim_a_id)', '', 'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 8)', ''),
	(5, 'ten keys IN, min and max', 'min(id), max(id)', '', 'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id BETWEEN 1 AND 10)', ''),
	(6, 'top 5', 'id, created_at', '', 'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 6)', ' ORDER BY created_at DESC, id DESC LIMIT 5'),
	(7, 'one key join, a row for each pair row', 'count(*), sum(amount)', ' JOIN %s m ON m.dim_a_id = f.dim_a_id', 'm.tenant_id = 3', ''),
	(8, 'group by the key, every key', 'm.tenant_id, count(*), sum(amount)', ' JOIN %s m ON m.dim_a_id = f.dim_a_id', 'true', ' GROUP BY 1');

-- tbl: the events, pairs: the relation of the pairs (a sub-select when its active pairs are a partial btree's)
CREATE FUNCTION probe(tbl text, pairs text) RETURNS TABLE (n int, label text, verdict text, route text) LANGUAGE sql AS $$
	SELECT s.n, s.label, CASE r.same WHEN true THEN 'same' ELSE 'DIFFERENT' END, r.route
	FROM shapes s, LATERAL ms_run(format('SELECT %s FROM %I f%s WHERE %s%s', s.sel, tbl, format(s.joined, pairs), format(s.pred, pairs), s.tail)) r
	ORDER BY s.n
$$;
\set ECHO all
