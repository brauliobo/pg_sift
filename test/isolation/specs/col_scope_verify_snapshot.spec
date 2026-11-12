# sift.col_scope_verify runs in the snapshot of its caller: a REPEATABLE READ transaction that took its snapshot before another session committed an edit of the pairs and of the facts
# verifies the state it sees (the pairs of its snapshot against the facts of its snapshot, stored units patched for its reader) and finds the index consistent; so does a new transaction
# after the edit, and the check does not leave a setting behind in the session.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = off);
	INSERT INTO iso_pairs SELECT m, 1 + (m * 5 + j * 2) % 6 FROM generate_series(1, 120) m, generate_series(0, 2) j WHERE j < m % 4;
	CREATE INDEX iso_pairs_journal ON iso_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
	CREATE TABLE iso_facts (id int PRIMARY KEY, dim_a_id int, amount int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_facts SELECT i, 1 + i % 120, i % 10 FROM generate_series(1, 2400) i;
	CREATE INDEX iso_facts_col ON iso_facts USING sift_col (id, dim_a_id, amount) WITH (scope = 'tenant_id: dim_a_id -> iso_pairs(dim_a_id, tenant_id)');
}

setup
{
	REINDEX INDEX iso_facts_col;
}

teardown
{
	DROP TABLE iso_facts, iso_pairs;
}

session reader
step rbegin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS snapshot_taken FROM iso_facts WHERE id = 1; }
step rverify	{ SELECT scope_key, problem, detail FROM sift.col_scope_verify('iso_facts_col', 50) ORDER BY scope_key; }
step rsetting	{ SHOW sift.enable_agg; }
step rend	{ COMMIT; }

session writer
step wedit	{ BEGIN; DELETE FROM iso_pairs WHERE tenant_id = 2; INSERT INTO iso_pairs SELECT m, 2 FROM generate_series(1, 40) m ON CONFLICT DO NOTHING;
		  UPDATE iso_facts SET dim_a_id = 1 + dim_a_id % 120 WHERE id % 7 = 0; DELETE FROM iso_facts WHERE id % 11 = 0; COMMIT; }

permutation rverify
permutation rbegin wedit rverify rend rverify rsetting
