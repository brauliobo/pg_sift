-- A scope of two paths: 'key: fk -> rel(member, key) | rel2(via, key) via vrel(via, member)'. Its pairs are the distinct union of the rows of the first relation and of the joined ones (a member of
-- a via value, with each key of that value). The declaration records the unique btree of each of the three relations (the edges are told apart by their columns), says which in a NOTICE, and
-- needs no journal index yet. What is wrong with a declaration, the recording rules (sticky ALTER, DROP refused, REINDEX CONCURRENTLY keeps the edge), a scope of one path unchanged, the legs.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;
CREATE FUNCTION edges(idx regclass) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce(string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text), 'none') FROM pg_depend
   WHERE classid = 'pg_class'::regclass AND objid = idx AND refclassid = 'pg_class'::regclass AND deptype = 'n'
	 AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i' $$;

-- grants of a tenant on a member directly (dim_a_id) or on a group (group_id), soft deleted; the members of a group, soft deleted
CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id int PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, pad int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + i % 50, timestamptz '2026-01-01' + (i % 100) * interval '1 day', i % 10, 0 FROM generate_series(1, 2000) i;
INSERT INTO memberships SELECT e, 1 + e % 5, e, CASE WHEN e % 9 = 0 THEN now() END FROM generate_series(1, 50) e;
INSERT INTO grants VALUES (1, 1, 7, NULL, NULL), (2, 1, NULL, 2, NULL), (3, 1, NULL, 3, NULL), (4, 2, NULL, 2, NULL), (5, 2, 8, NULL, now()), (6, 3, 9, NULL, NULL);

-- relations that do not qualify
CREATE TABLE grants_text (id int PRIMARY KEY, tenant_id int NOT NULL, group_id text NOT NULL, UNIQUE (group_id, tenant_id));
CREATE TABLE grants_plain (id int PRIMARY KEY, tenant_id int NOT NULL, group_id int NOT NULL);
CREATE TABLE grants_key8 (id int PRIMARY KEY, tenant_id bigint NOT NULL, group_id int NOT NULL, UNIQUE (group_id, tenant_id));
CREATE TABLE memberships_via8 (group_id bigint NOT NULL, dim_a_id int NOT NULL, PRIMARY KEY (group_id, dim_a_id));
CREATE TABLE memberships_member8 (group_id int NOT NULL, dim_a_id bigint NOT NULL, PRIMARY KEY (group_id, dim_a_id));
CREATE VIEW grants_view AS SELECT * FROM grants;
-- a scope of one path needs a journal index; the pairs relation of the scope of one path
CREATE TABLE pairs_one (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO pairs_one SELECT i, 1 + i % 3 FROM generate_series(1, 50) i;

-- the grammar: what is wrong with the text
CREATE TABLE bad_scopes (n int, why text, opt text);
INSERT INTO bad_scopes SELECT row_number() OVER (), why, opt FROM (VALUES
	('no via', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id)'),
	('nothing after the bar', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) |'),
	('second path not closed', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id via memberships(group_id, dim_a_id)'),
	('second path one column', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id) via memberships(group_id, dim_a_id)'),
	('second path three columns', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id, id) via memberships(group_id, dim_a_id)'),
	('via without a relation', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via'),
	('via relation one column', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id)'),
	('via relation not closed', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id'),
	('text after the via relation', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id) more'),
	('using without a name', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) using via memberships(group_id, dim_a_id)'),
	('cols not closed', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id) cols(amount') ) v(why, opt);
SELECT n, why, try(format('CREATE INDEX bad ON facts USING sift_col (id, dim_a_id, created_at, amount, pad) WITH (scope = %L)', opt)) AS result FROM bad_scopes ORDER BY n;

-- the declaration: what is wrong with the relations and columns of each path
CREATE TABLE bad_decls (n int, why text, opt text);
INSERT INTO bad_decls SELECT row_number() OVER (), why, opt FROM (VALUES
	('joined relation missing', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | nowhere(group_id, tenant_id) via memberships(group_id, dim_a_id)'),
	('via relation missing', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via nowhere(group_id, dim_a_id)'),
	('joined relation is the table', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | facts(id, amount) via memberships(group_id, dim_a_id)'),
	('joined relation is a view', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants_view(group_id, tenant_id) via memberships(group_id, dim_a_id)'),
	('joined column missing', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(nothing, tenant_id) via memberships(group_id, dim_a_id)'),
	('via column missing', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, nothing)'),
	('joined column not an integer', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants_text(group_id, tenant_id) via memberships(group_id, dim_a_id)'),
	('joined path has no btree', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants_plain(group_id, tenant_id) via memberships(group_id, dim_a_id)'),
	('via relation has no btree', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via grants_plain(group_id, tenant_id)'),
	('same column twice in the joined path', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, group_id) via memberships(group_id, dim_a_id)'),
	('the two paths are the same', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(dim_a_id, tenant_id) via memberships(group_id, dim_a_id)'),
	('the joined path is the pairs reversed', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(tenant_id, dim_a_id) via memberships(group_id, dim_a_id)'),
	('the via relation is the pairs', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via grants(dim_a_id, tenant_id)'),
	('via columns differ in type', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships_via8(group_id, dim_a_id)'),
	('member columns differ in type', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships_member8(group_id, dim_a_id)'),
	('key columns differ in type', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants_key8(group_id, tenant_id) via memberships(group_id, dim_a_id)'),
	('using an index of another relation', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) using memberships_active via memberships(group_id, dim_a_id)'),
	('using an index that is not there', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id) using nowhere'),
	('cols names a column that is not in the index', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id) cols(nothing)'),
	('a scope of one path: nullable member', 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id)'),
	('a scope of one path: no journal index', 'tenant_id: dim_a_id -> pairs_one(dim_a_id, tenant_id)')) v(why, opt);
SELECT n, why, try(format('CREATE INDEX bad ON facts USING sift_col (id, dim_a_id, created_at, amount, pad) WITH (scope = %L)', opt)) AS result FROM bad_decls ORDER BY n;
SELECT count(*) AS leftover_indexes FROM pg_class WHERE relname = 'bad';

-- the declaration: three edges, one NOTICE per path, no journal index, nullable columns (a row with a NULL in a column of its path is not a pair of that path)
SET client_min_messages = notice;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount, pad)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
SET client_min_messages = warning;
SELECT edges('facts_col') AS edges;
SELECT o AS option FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'facts_col' AND o LIKE 'scope=%';

-- the recorded btrees: DROP is refused for each, REINDEX CONCURRENTLY of each keeps the edge, a btree made later changes nothing
SELECT try('DROP INDEX grants_direct') AS drop_direct;
SELECT try('DROP INDEX grants_group') AS drop_joined;
SELECT try('DROP INDEX memberships_active') AS drop_via;
SELECT try('DROP TABLE memberships') AS drop_via_table;
REINDEX INDEX CONCURRENTLY grants_direct;
REINDEX INDEX CONCURRENTLY grants_group;
REINDEX INDEX CONCURRENTLY memberships_active;
SELECT edges('facts_col') AS after_reindex_concurrently;
CREATE UNIQUE INDEX grants_other ON grants (tenant_id, group_id) WHERE id > 0;
ALTER INDEX facts_col SET (superunit = 8);
SELECT edges('facts_col') AS after_other_option;
-- two partial unique btrees on the columns of the joined path: REINDEX says so, naming the index in USING settles it, ALTER keeps the recorded one
SELECT try('REINDEX INDEX facts_col') AS two_partial;
SELECT try('ALTER INDEX facts_col SET (scope = ''tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) using grants_other via memberships(group_id, dim_a_id)'')') AS using_other;
ALTER INDEX facts_col SET (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) using grants_group via memberships(group_id, dim_a_id)');
SELECT edges('facts_col') AS after_using_recorded;
ALTER INDEX facts_col RESET (scope);
SELECT edges('facts_col') AS after_reset;
SET client_min_messages = notice;
ALTER INDEX facts_col SET (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) using grants_other via memberships(group_id, dim_a_id)');
SET client_min_messages = warning;
SELECT edges('facts_col') AS after_using_other;
ALTER INDEX facts_col SET (scope = 'off');
SELECT edges('facts_col') AS after_off;
DROP INDEX grants_other;
ALTER INDEX facts_col SET (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
SELECT edges('facts_col') AS after_set;

-- the text is the record: pg_dump writes it and a restore makes the same index
SELECT pg_get_indexdef('facts_col'::regclass) AS ddl \gset
DROP INDEX facts_col;
SELECT count(*) AS edges_after_drop FROM pg_depend WHERE classid = 'pg_class'::regclass AND deptype = 'n' AND refobjid IN ('grants_direct'::regclass, 'grants_group'::regclass, 'memberships_active'::regclass);
SELECT :'ddl' AS restore \gexec
SELECT edges('facts_col') AS after_restore;
REINDEX INDEX facts_col;
SELECT edges('facts_col') AS after_reindex;

-- a scope of one path is as it was: the btree and its journal index, and a scope of two paths beside it in the same index
CREATE INDEX pairs_one_journal ON pairs_one USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX facts_one ON facts USING sift_col (id, dim_a_id, created_at, amount, pad) WITH (scope = 'tenant_id: dim_a_id -> pairs_one(dim_a_id, tenant_id)');
SELECT edges('facts_one') AS one_path_edges;
SELECT leg, rel, member, key, btree, via_rel FROM sift._test_scope_legs('facts_one', 0);
CREATE INDEX facts_both ON facts USING sift_col (id, dim_a_id, created_at, amount, pad)
	WITH (scope = 'by_grant: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id); by_pair: dim_a_id -> pairs_one(dim_a_id, tenant_id)');
SELECT edges('facts_both') AS both_edges;
SELECT scope, leg, rel, member, key, btree, via_rel FROM generate_series(0, 1) scope, LATERAL sift._test_scope_legs('facts_both', scope) ORDER BY scope, leg;
DROP INDEX facts_both;

-- the legs of the scope of two paths: the direct pairs, then the joined ones with the relation of the members of the via column; each with the predicate of its btree
SELECT * FROM sift._test_scope_legs('facts_col', 0) ORDER BY leg;

DROP TABLE facts, grants, memberships, grants_text, grants_plain, grants_key8, memberships_via8, memberships_member8, pairs_one, bad_scopes, bad_decls CASCADE;
DROP FUNCTION try(text), edges(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
