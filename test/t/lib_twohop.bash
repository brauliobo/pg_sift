# Fixtures of the tests of the scope of two paths (a scope on the grants of a member and, through the memberships, on the grants of the groups the member is in): the tables, the three recorded
# btrees (the partial unique btrees of the grants by member, of the grants by group and of the memberships), the three journal indexes, the facts and the scope index hop_col (twohop_setup NODE), the
# oracle view hop_pairs (the SQL UNION of the three tables), twohop_reads NODE (the scope forms against the plain join over the oracle: same or DIFFERENT), twohop_why NODE (stored, or the fallback
# reason of a read of one tenant), twohop_vouch NODE (what each journal's stamp in the scope segments says: ok or why not) and twohop_files NODE (the pgbench scripts that edit the three tables and
# the facts). Members 1..200 are in one group, those that divide by four in two; the groups 1..12 and the members 1..200 have grants to the tenants 1..16; the members 100..109 have no grant.

TWOHOP_ROWS=$(knob SIFT_TWOHOP_ROWS 6000 3000)
TWOHOP_SCOPE="tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)"
TWOHOP_CONF="sift.scope_max_multiplicity = 8
autovacuum = off"

TWOHOP_BASE=(
	"CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz, CHECK (dim_a_id IS NULL OR group_id IS NULL)) WITH (autovacuum_enabled = false)"
	"CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL"
	"CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL"
	"CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false)"
	"CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL"
	"INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + m % 12, m FROM generate_series(1, 200) m"
	"INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m * 5) % 12, m FROM generate_series(1, 200) m WHERE m % 4 = 0 ON CONFLICT DO NOTHING"
	"INSERT INTO grants (tenant_id, dim_a_id) SELECT 1 + (m * 31 + j * 13) % 16, m FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 AND m NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING"
	"INSERT INTO grants (tenant_id, group_id) SELECT 1 + (g * 5 + j * 3) % 16, g FROM generate_series(1, 12) g, generate_series(0, 1) j ON CONFLICT DO NOTHING"
	"CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')"
	"CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid')"
	"CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid')"
	"CREATE TABLE hop_events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false)"
	"INSERT INTO hop_events SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 210 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10,
		repeat('x', 60) FROM generate_series(1, $TWOHOP_ROWS) i"
	"CREATE VIEW hop_pairs (dim_a_id, tenant_id) AS SELECT dim_a_id, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
		UNION SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL"
)
TWOHOP_INDEX=(
	"CREATE INDEX hop_col ON hop_events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = '$TWOHOP_SCOPE')"
	"REINDEX INDEX hop_col"
	"VACUUM (FREEZE, ANALYZE) hop_events, grants, memberships"
)
TWOHOP_FUNCS=(
	"CREATE OR REPLACE FUNCTION hop_vouch() RETURNS text LANGUAGE plpgsql AS \$\$
		BEGIN RETURN (SELECT string_agg(DISTINCT sift._test_scope_vouch('hop_col', 0, a.lineage, a.horizon, b.lineage, c.lineage), ',')
			FROM sift._test_col_scope_journal_stamps('hop_col') a
			JOIN sift._test_col_scope_journal_stamps('hop_col') b ON b.gen = a.gen AND b.family = a.family AND b.scope = a.scope AND b.journal = 1
			JOIN sift._test_col_scope_journal_stamps('hop_col') c ON c.gen = a.gen AND c.family = a.family AND c.scope = a.scope AND c.journal = 2
			WHERE a.family = 'scope' AND a.scope = 0 AND a.journal = 0); END \$\$"
	"CREATE OR REPLACE FUNCTION hop_same(k int) RETURNS bool LANGUAGE plpgsql AS \$\$
		DECLARE a text; b text; c text; d text;
		BEGIN
			PERFORM set_config('sift.agg_force', 'on', true);
			PERFORM set_config('sift.enable_agg', 'on', true);
			SELECT count(*) || ':' || coalesce(sum(v), 0) || ':' || count(DISTINCT dim_a_id) INTO a FROM hop_events e WHERE sift.in_scope('hop_col'::regclass, 'tenant_id', e.dim_a_id, k);
			SELECT string_agg(id::text, ',' ORDER BY created_at DESC, id DESC) INTO c FROM (SELECT id, created_at FROM hop_events e
				WHERE sift.in_scope('hop_col'::regclass, 'tenant_id', e.dim_a_id, k) ORDER BY created_at DESC, id DESC LIMIT 20) t;
			PERFORM set_config('sift.enable_agg', 'off', true);
			SELECT count(*) || ':' || coalesce(sum(v), 0) || ':' || count(DISTINCT e.dim_a_id) INTO b FROM hop_events e JOIN hop_pairs s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k;
			SELECT string_agg(id::text, ',' ORDER BY created_at DESC, id DESC) INTO d FROM (SELECT id, created_at FROM hop_events e
				WHERE e.dim_a_id IN (SELECT dim_a_id FROM hop_pairs WHERE tenant_id = k) ORDER BY created_at DESC, id DESC LIMIT 20) t;
			IF a <> b OR c IS DISTINCT FROM d THEN RAISE EXCEPTION 'hop_same(%): scope % join %; top20 scope % join %', k, a, b, left(c, 60), left(d, 60); END IF;
			RETURN true;
		END \$\$"
)
TWOHOP_STMTS=("${TWOHOP_BASE[@]}" "${TWOHOP_INDEX[@]}" "${TWOHOP_FUNCS[@]}")

# twohop_setup NODE: the extensions and the fixtures
twohop_setup() { sql "$1" "CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE" "${TWOHOP_STMTS[@]}" >/dev/null; }

TWOHOP_Q_ONE="SELECT count(*), sum(v), min(id), count(DISTINCT dim_a_id) FROM hop_events e WHERE sift.in_scope('hop_col'::regclass, 'tenant_id', e.dim_a_id, 5)"
TWOHOP_P_ONE="SELECT count(*), sum(v), min(id), count(DISTINCT dim_a_id) FROM hop_events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM hop_pairs WHERE tenant_id = 5)"
TWOHOP_Q_SET="SELECT count(*), sum(v) FROM hop_events e WHERE sift.in_scope('hop_col'::regclass, 'tenant_id', e.dim_a_id, ARRAY[2, 5, 9, 9]::int8[])"
TWOHOP_P_SET="SELECT count(*), sum(v) FROM hop_events e WHERE EXISTS (SELECT 1 FROM hop_pairs s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id IN (2, 5, 9))"
TWOHOP_Q_TOP="SELECT id FROM hop_events e WHERE sift.in_scope('hop_col'::regclass, 'tenant_id', e.dim_a_id, 7) ORDER BY created_at DESC, id DESC LIMIT 20"
TWOHOP_P_TOP="SELECT id FROM hop_events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM hop_pairs WHERE tenant_id = 7) ORDER BY created_at DESC, id DESC LIMIT 20"
TWOHOP_Q_PAIR="SELECT k, (SELECT count(*) FROM hop_events e WHERE sift.in_scope('hop_col'::regclass, 'tenant_id', e.dim_a_id, k)) FROM generate_series(1, 17) k ORDER BY k"
# a join that is not a correlated sub-select: the SiftAgg plan of a sub-select over a view with a UNION that reads the parameter of its outer level (SELECT k, (SELECT count(*) FROM .. JOIN hop_pairs s .. WHERE s.tenant_id = k)) reads the child once and answers every k with the first one, with or without a scope index
TWOHOP_P_PAIR="SELECT k, count(e.id) FROM generate_series(1, 17) k LEFT JOIN hop_pairs s ON s.tenant_id = k LEFT JOIN hop_events e ON e.dim_a_id = s.dim_a_id GROUP BY k ORDER BY k"
# the entries of the rows of the table: a REINDEX also indexes the rows a standby's snapshot (hot_standby_feedback, reported once a second) still holds back, which are no row of the join
TWOHOP_ENTRIES="SELECT (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('hop_col') WHERE tid IN (SELECT ctid FROM hop_events))
	= (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM hop_events e JOIN hop_pairs s ON s.dim_a_id = e.dim_a_id)"

# the functions below send their statements through ${TWOHOP_SQL:-sql} (a node name, or a database name for a test that restores into databases) and read the index ${TWOHOP_IX:-hop_col}
# twohop_reads NODE [SETTING...]: the scope forms (one tenant, a set counted once, a top-N, per pair) in a session with the settings, against the join over the oracle without sift
twohop_reads() {
	local n=$1 Q=${TWOHOP_SQL:-sql} ix=${TWOHOP_IX:-hop_col} q
	shift
	q=("$TWOHOP_Q_ONE" "$TWOHOP_Q_SET" "$TWOHOP_Q_TOP" "$TWOHOP_Q_PAIR")
	[[ $($Q "$n" "SET sift.agg_force = on" "${@:-SET sift.agg_force = on}" "${q[@]//hop_col/$ix}" 2>&1) \
		== $($Q "$n" "SET sift.enable_agg = off" "$TWOHOP_P_ONE" "$TWOHOP_P_SET" "$TWOHOP_P_TOP" "$TWOHOP_P_PAIR" 2>&1) ]] && echo same || echo DIFFERENT
}

# twohop_why NODE [QUERY]: stored when the read of the scope units answered, else the reason of the fallback
twohop_why() {
	local Q=${TWOHOP_SQL:-sql} q=${2:-$TWOHOP_Q_ONE}
	$Q "$1" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "SET jit = off" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ${q//hop_col/${TWOHOP_IX:-hop_col}}" | sed -n 's/^ *Scope Fallback: //p;s/^ *Scope Units: .*/stored/p' | head -n 1
}

# twohop_stat NODE LABEL: a number of the plan of the read of one tenant ("Scope Changed Keys": the members the journals say changed since the stamps; "Scope Patched Keys": those with a pair now), 0
# when the units were not read
twohop_stat() {
	local n Q=${TWOHOP_SQL:-sql}
	n=$($Q "$1" "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "SET jit = off" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ${TWOHOP_Q_ONE//hop_col/${TWOHOP_IX:-hop_col}}" | sed -n "s/^ *Scope $2: //p" | head -n 1)
	echo "${n:-0}"
}

twohop_vouch() { ${TWOHOP_SQL:-sql} "$1" "SELECT hop_vouch()"; }

# twohop_files NODE: the pgbench arguments of scripts that edit the three tables and the facts, each in a transaction that cannot fail on a key
twohop_files() {
	local d
	d=$(dir "$1")
	cat >"$d/hg.sql" <<'EOF'
\set m random(1, 200)
\set t random(1, 16)
\set g random(1, 12)
BEGIN;
INSERT INTO grants (tenant_id, dim_a_id) VALUES (:t, :m) ON CONFLICT DO NOTHING;
UPDATE grants SET deleted_at = now() WHERE dim_a_id = :m AND tenant_id = :t + 1 AND deleted_at IS NULL;
INSERT INTO grants (tenant_id, group_id) VALUES (:t, :g) ON CONFLICT DO NOTHING;
UPDATE grants SET deleted_at = now() WHERE group_id = :g AND tenant_id = :t + 2 AND deleted_at IS NULL;
COMMIT;
EOF
	cat >"$d/hm.sql" <<'EOF'
\set m random(1, 200)
\set g random(1, 12)
BEGIN;
INSERT INTO memberships (group_id, dim_a_id) VALUES (:g, :m) ON CONFLICT DO NOTHING;
UPDATE memberships SET deleted_at = now() WHERE dim_a_id = :m AND group_id = :g + 1 AND deleted_at IS NULL;
COMMIT;
EOF
	cat >"$d/hf.sql" <<'EOF'
\set id random(1, 100000)
\set m random(1, 210)
BEGIN;
INSERT INTO hop_events VALUES (:id, :m, now(), :id % 10, 'z') ON CONFLICT (id) DO UPDATE SET dim_a_id = EXCLUDED.dim_a_id, v = hop_events.v + 1;
DELETE FROM hop_events WHERE id = :id + 7;
COMMIT;
EOF
	echo "-f $d/hg.sql@4 -f $d/hm.sql@4 -f $d/hf.sql@3"
}
