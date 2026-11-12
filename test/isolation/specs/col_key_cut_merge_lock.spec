# The rewrite that chooses key_cut again writes the option in its transaction, so a merge that runs after it reads the option of the commit: the maintenance lock a rewrite
# took goes with its transaction when it changed an option, and the merge that waited for it reads the options again. A merge that took the lock at once and kept the options of
# before the commit would read the segments cut by the key as if they were in the order of the TIDs.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE TABLE iso_kc (id int PRIMARY KEY, tenant_id int, dim_a_id int, v int, ts timestamp, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_kc SELECT i, i / 150, 1 + i % 12, hashint4(i) & 1048575, TIMESTAMP '2026-01-01' + (hashint4(i + 7) & 16777215) * INTERVAL '1 second', repeat('x', 100)
	FROM generate_series(1, 120000) i;
}

setup { VACUUM ANALYZE iso_kc; }

setup { CREATE INDEX iso_kc_idx ON iso_kc USING sift_col (id, tenant_id, dim_a_id, v, ts) WITH (dead_rewrite_pct = 50); }

setup
{
	INSERT INTO iso_kc SELECT i, (hashint4(i) & 2147483647) % 800, 1 + i % 12, hashint4(i) & 1048575, TIMESTAMP '2026-01-01' + (hashint4(i + 7) & 16777215) * INTERVAL '1 second', repeat('x', 100)
	FROM generate_series(120001, 240000) i;
	DELETE FROM iso_kc WHERE id % 10 = 3;
}

setup { VACUUM ANALYZE iso_kc; }

teardown { DROP TABLE iso_kc; }

session a
setup { SET sift.async_maintenance = off; SET sift.col_auto_merge = off; }
step a_begin	{ BEGIN; }
step a_rewrite	{ SELECT sift.col_merge('iso_kc_idx', true) > 0 AS merged; }
step a_commit	{ COMMIT; }

session b
setup { SET sift.async_maintenance = off; SET sift.col_auto_merge = off; }
step b_insert	{ INSERT INTO iso_kc SELECT i, i % 800, 1 + i % 12, i, TIMESTAMP '2027-01-01' + i * INTERVAL '1 second', 'y' FROM generate_series(300001, 300500) i; }
step b_merge	{ SELECT sift.col_merge('iso_kc_idx', true) > 0 AS merged; }
step b_option	{ SELECT reloptions::text ~ 'key_cut=auto' AS cut_chosen FROM pg_class WHERE relname = 'iso_kc_idx'; }
step b_verify	{ SELECT 1 AS verified FROM (SELECT sift.col_verify('iso_kc_idx', true)) v; }

permutation a_begin a_rewrite b_insert b_merge(a_commit) a_commit b_option b_verify
