# A workload whose tables and columns were renamed after the indexes were built (src/col/names.c), for the tests that carry a database to another one: pg_dump and pg_restore
# (col_names_dump.sh) and pg_upgrade (pg_upgrade.sh). The indexes name tables and columns as text (connect, scope, paths, key_cut, rollup_columns, the definitions of a projection) and
# keep leaves under the name of a column (the lengths of an array, the paths of a document, the flag of a connection): the restore builds the indexes again from the text, the upgrade
# keeps their files, and both must end with what the renamed database has.
#
#   NAMES_SETUP        the SQL that builds the tables and indexes, renames them, and writes after the renames (the segments hold both the old and the new names)
#   NAMES_STATE        one SELECT: the options that name something, the projection status and the follow
#   NAMES_VERIFY       one SELECT that fails unless every index passes sift.col_verify
#   NAMES_ANSWERS      one SELECT: the answers of the queries that read the indexes, with the pushdown on and off, one text each
#   NAMES_WRITES       the SQL that writes to the tables, under the new names
NAMES_SETUP=$(cat <<'SQL'
CREATE TABLE nm_dim (id int PRIMARY KEY, grp int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO nm_dim SELECT g, g % 7 FROM generate_series(1, 300) g;
CREATE INDEX nm_dim_c ON nm_dim USING sift_col (id, grp);
CREATE TABLE nm_fact (id int PRIMARY KEY, dim_id int NOT NULL, created date NOT NULL, tags int[], doc jsonb, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO nm_fact SELECT g, 1 + g % 300, date '2025-01-01' + g % 90, ARRAY[g % 3, g % 5], jsonb_build_object('k', g % 9, 's', 'v' || g % 4), g FROM generate_series(1, 3000) g;
CREATE INDEX nm_fact_c ON nm_fact USING sift_col (id, dim_id, created, tags, doc, v)
	WITH (connect = 'dim_id -> nm_dim(id)', key_cut = 'created', paths = 'doc.k int4, doc.s text', rollup_columns = 'dim_id, created', order_array_columns = 'v');
CREATE TABLE nm_pairs (member int NOT NULL, grp int NOT NULL, PRIMARY KEY (member, grp)) WITH (autovacuum_enabled = false);
INSERT INTO nm_pairs SELECT g, 1 + g % 5 FROM generate_series(1, 300) g;
CREATE INDEX nm_pairs_j ON nm_pairs USING sift_col (member, grp) WITH (track_changes = 'ctid');
CREATE TABLE nm_ev (id int PRIMARY KEY, member int NOT NULL, kind int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO nm_ev SELECT g, 1 + g % 300, g % 7, g FROM generate_series(1, 3000) g;
CREATE INDEX nm_ev_c ON nm_ev USING sift_col (id, member, kind, v) WITH (scope = 'g: member -> nm_pairs(member, grp) using nm_pairs_pkey cols(kind, v)');
CREATE TABLE nm_p (id int PRIMARY KEY, region text, cust int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO nm_p SELECT g, 'r' || g % 4, g % 20, g FROM generate_series(1, 1000) g;
SELECT sift.projection_create('nm_by_cust', 'nm_p', ARRAY['cust', 'lower(region)', 'amount'], rollups => 'cust, amount', filter => $$region <> 'r0'$$);
SELECT sift.projection_index('nm_p_c', ARRAY['nm_by_cust']);
CREATE TABLE nm_src (id serial PRIMARY KEY, grp int NOT NULL, member int NOT NULL) WITH (autovacuum_enabled = false);
CREATE VIEW nm_view AS SELECT DISTINCT member, grp FROM nm_src;
CREATE TABLE nm_fpairs (member int NOT NULL, grp int NOT NULL, PRIMARY KEY (grp, member)) WITH (autovacuum_enabled = false);
INSERT INTO nm_src (grp, member) SELECT 1 + g % 4, g FROM generate_series(1, 40) g;
SELECT sift.scope_follow_pairs('nm_fpairs', 'nm_view', 'nm_src', 'key', 'grp');
CREATE TABLE nm_tbl (id int, g int, doc jsonb) USING sift_table;
INSERT INTO nm_tbl SELECT g, g % 9, jsonb_build_object('k', g % 5, 's', 'v' || g % 3) FROM generate_series(1, 2000) g;
SELECT sift.table_set_paths('nm_tbl', 'doc', 'doc->k int4, doc->s text');
SELECT sift.table_set_option('nm_tbl', 'key_order', 'g, id');
SELECT sift.table_set_option('nm_tbl', 'compression', 'auto; g=zstd:3');
VACUUM (ANALYZE) nm_dim, nm_fact, nm_pairs, nm_ev, nm_p, nm_tbl;
CREATE SCHEMA nm_s;
ALTER TABLE nm_fact RENAME COLUMN dim_id TO dim_ref;
ALTER TABLE nm_fact RENAME COLUMN created TO created_on;
ALTER TABLE nm_fact RENAME COLUMN tags TO labels;
ALTER TABLE nm_fact RENAME COLUMN doc TO document;
ALTER TABLE nm_fact RENAME COLUMN v TO amount;
ALTER TABLE nm_dim RENAME COLUMN id TO dim_pk;
ALTER TABLE nm_dim RENAME TO nm_dims;
ALTER TABLE nm_dims SET SCHEMA nm_s;
ALTER TABLE nm_pairs RENAME COLUMN member TO who;
ALTER TABLE nm_pairs RENAME COLUMN grp TO grp_id;
ALTER TABLE nm_pairs RENAME TO nm_grants;
ALTER INDEX nm_pairs_pkey RENAME TO nm_grants_btree;
ALTER TABLE nm_grants SET SCHEMA nm_s;
ALTER TABLE nm_ev RENAME COLUMN member TO who_ref;
ALTER TABLE nm_ev RENAME COLUMN kind TO category;
ALTER TABLE nm_p RENAME COLUMN cust TO customer;
ALTER TABLE nm_p RENAME COLUMN region TO area;
ALTER TABLE nm_p RENAME TO nm_q;
ALTER TABLE nm_fpairs RENAME COLUMN member TO who;
ALTER TABLE nm_src RENAME COLUMN grp TO grp_id;
ALTER TABLE nm_tbl RENAME COLUMN doc TO document;
ALTER TABLE nm_tbl RENAME COLUMN g TO grp;
INSERT INTO nm_fact SELECT g, 1 + g % 300, date '2025-04-01' + g % 20, ARRAY[g % 3], jsonb_build_object('k', g % 9), g FROM generate_series(3001, 3400) g;
SELECT sift.col_promote('nm_fact_c');
INSERT INTO nm_src (grp_id, member) VALUES (9, 90), (9, 91);
CREATE FUNCTION nm_answers() RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text; r text; o text := '';
BEGIN
	FOREACH q IN ARRAY ARRAY[
		'SELECT string_agg(l || '':'' || c || '':'' || s, '','' ORDER BY l) FROM (SELECT labels[1] l, count(*) c, sum(amount) s FROM nm_fact GROUP BY 1) x',
		'SELECT string_agg(k || '':'' || c, '','' ORDER BY k) FROM (SELECT document ->> ''k'' AS k, count(*) c FROM nm_fact GROUP BY 1) y',
		'SELECT string_agg(d || '':'' || c, '','' ORDER BY d) FROM (SELECT created_on d, count(*) c FROM nm_fact WHERE dim_ref < 50 GROUP BY 1) z',
		'SELECT string_agg(g || '':'' || c || '':'' || s, '','' ORDER BY g) FROM (SELECT d.grp g, count(*) c, sum(f.amount) s FROM nm_fact f JOIN nm_s.nm_dims d ON d.dim_pk = f.dim_ref GROUP BY 1) j',
		'SELECT count(*) || '':'' || sum(v) FROM nm_ev WHERE who_ref IN (SELECT who FROM nm_s.nm_grants WHERE grp_id = 3)',
		'SELECT string_agg(c || '':'' || n || '':'' || s, '','' ORDER BY c) FROM (SELECT customer c, count(*) n, sum(amount) s FROM nm_q WHERE area <> ''r0'' GROUP BY 1) p',
		'SELECT string_agg(k || '':'' || g || '':'' || c, '','' ORDER BY k, g) FROM (SELECT document ->> ''k'' k, grp g, count(*) c FROM nm_tbl GROUP BY 1, 2) t'] LOOP
		EXECUTE q INTO r;
		o := o || r || '/';
	END LOOP;
	RETURN md5(o);
END $$;
CREATE FUNCTION nm_answers_on() RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN nm_answers();
CREATE FUNCTION nm_answers_off() RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN nm_answers();
SQL
)
NAMES_STATE="SELECT (SELECT string_agg(c.relname || ' ' || o, ' | ' ORDER BY c.relname, o) FROM pg_class c, unnest(c.reloptions) o
		WHERE c.relname IN ('nm_dim_c', 'nm_fact_c', 'nm_pairs_j', 'nm_ev_c', 'nm_p_c')
		AND split_part(o, '=', 1) IN ('connect', 'scope', 'paths', 'key_cut', 'rollup_columns', 'order_array_columns', 'projection', 'track_changes'))
	|| ' || ' || (SELECT string_agg(source || ' ' || leaves::text || ' ' || coalesce(rollups, '-') || ' ' || filter, ' ; ') FROM sift.projections)
	|| ' || ' || (SELECT string_agg(projection || ':' || state, ',') FROM sift.projection_status)
	|| ' || ' || (SELECT string_agg(source || ':' || source_column || ':' || member_column || ':' || key_column, ',') FROM sift.scope_follows)
	|| ' || ' || (SELECT string_agg(relid::text || ' ' || attname || ' ' || spec, ',') FROM sift.table_paths)
	|| ' || ' || (SELECT string_agg(rel::text || ' ' || name || '=' || value, ',' ORDER BY name) FROM sift.table_option_values)"
NAMES_VERIFY="SELECT sift.col_verify('nm_s.nm_dim_c', true), sift.col_verify('nm_fact_c', true), sift.col_verify('nm_s.nm_pairs_j', true), sift.col_verify('nm_ev_c', true), sift.col_verify('nm_p_c', true)"
NAMES_ANSWERS="SELECT nm_answers_on() = nm_answers_off(), nm_answers_off()"
NAMES_WRITES="INSERT INTO nm_fact SELECT g, 1 + g % 300, date '2025-05-01' + g % 20, ARRAY[1, g % 2], jsonb_build_object('k', 4, 's', 'new'), g FROM generate_series(4001, 4200) g;
UPDATE nm_fact SET amount = amount + 1 WHERE id % 7 = 0;
DELETE FROM nm_fact WHERE id % 11 = 0;
INSERT INTO nm_ev SELECT g, 1 + g % 300, g % 7, g FROM generate_series(4001, 4100) g;
INSERT INTO nm_s.nm_grants SELECT g, 1 + g % 5 FROM generate_series(301, 340) g;
INSERT INTO nm_src (grp_id, member) VALUES (5, 50), (5, 51);
INSERT INTO nm_tbl SELECT 2000 + g, g % 9, jsonb_build_object('k', 7) FROM generate_series(1, 300) g;
UPDATE nm_q SET amount = amount + 1 WHERE id % 13 = 0"
