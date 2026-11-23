# Format major 2 is WAL-logged page by page: a standby replays the allocator, the manifest, the parts, the slabs and the deletion vectors of a primary that checks every
# Generic WAL record against the page it wrote (wal_consistency_checking = 'generic': a mismatch is a FATAL "inconsistent page"), and crash recovery after kill -9 gives the
# same structures. The structures are read through sift._test_f2_check (the allocator, the queue and the manifest agree) and the partitions and their parts.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils qw(command_ok);
use Test::More;

my $primary = sift_node('primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "wal_consistency_checking = 'generic'\nsift.test_create_format1 = off\nshared_buffers = 64MB\nmax_prepared_transactions = 5");
$primary->start;
install_sift_test($primary, 'the format major 2 structures are exercised by testing functions');
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('f2_standby', true)");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'f2_standby'\nhot_standby_feedback = on\nmax_prepared_transactions = 5");
$standby->start;

my $idx = 'f2_idx';
$primary->safe_psql('postgres', "CREATE TABLE f2 (v int); CREATE INDEX $idx ON f2 USING sift_store_test (v)");
$primary->safe_psql('postgres', "SELECT sift._test_f2_init('$idx')");

sub state
{
	my ($node) = @_;
	return $node->safe_psql('postgres', qq{
		SELECT sift._test_f2_check('$idx') || E'\\n'
		  || (SELECT string_agg(p::text, ';' ORDER BY p.key_lo NULLS FIRST) FROM sift._test_f2_partitions('$idx') p) || E'\\n'
		  || (SELECT m::text FROM sift._test_f2_meta('$idx') m) || E'\\n'
		  || (SELECT i::text FROM sift._test_f2_meta_inline('$idx') i)});
}

# the functions write WAL in transactions that have no transaction id, which commit without flushing: the standby is asked to reach what was inserted, not what was written
# the functions write WAL in transactions without a transaction id, which commit without flushing: the standby is asked to reach what was inserted, not what was written
sub same_state
{
	my ($what) = @_;
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
	is(state($standby), state($primary), "$what: the standby has the structures of the primary");
	like(state($primary), qr/^parts=\d+ /, "$what: the allocator, the queue and the manifest agree");
}

subtest 'parts, splits and a manifest of two levels' => sub {
	$primary->safe_psql('postgres', qq{
		SELECT sift._test_f2_add_part('$idx', 1, NULL, 20000, 0, 1);
		SELECT sift._test_f2_split_many('$idx', 1, NULL, 150, 100000);
		SELECT sift._test_f2_add_part('$idx', 1, NULL, 3000, 50000, 900000, 0, 0, 2) });
	same_state('splits');
};

subtest 'a transaction that stages rows, a savepoint that rolls back, a slab' => sub {
	$primary->safe_psql('postgres', qq{
		BEGIN;
		SELECT sift._test_f2_stage('$idx', 500, 400000000, 5000000);
		SAVEPOINT a;
		SELECT sift._test_f2_stage('$idx', 100, 400010000, 6000000);
		ROLLBACK TO a;
		COMMIT;
		BEGIN;
		SELECT sift._test_f2_stage('$idx', 10, 500000000, 7000000);
		COMMIT });
	same_state('stage');
};

subtest 'deletes, deletion vectors, compaction and reuse of the freed blocks' => sub {
	my ($gen) = $primary->safe_psql('postgres', "SELECT min(part_gen) FROM sift._test_f2_parts('$idx', 1, NULL)");
	$primary->safe_psql('postgres', qq{
		SELECT sift._test_f2_delete('$idx', 1, NULL, 1, 5, 40, 3);
		SELECT sift._test_f2_delete('$idx', 1, NULL, 1, 4000, 5000, 1) });
	same_state('delete');
	$primary->safe_psql('postgres', "SELECT * FROM sift._test_f2_compact('$idx', 1, NULL, ARRAY[$gen])");
	same_state('compaction');
	$primary->safe_psql('postgres', "SELECT sift._test_f2_reclaim('$idx')");
	$primary->safe_psql('postgres', "SELECT sift._test_f2_alloc('$idx', 300)");
	same_state('reclaim');
};

subtest 'an UPDATE inside one partition writes its row and its delete in one record' => sub {
	my $rows = "SELECT count(*), sum(v) FROM fu WHERE id BETWEEN 90 AND 300";
	my $by_index = "SET enable_seqscan = off; SET enable_indexscan = off; $rows";
	$primary->safe_psql('postgres', q{
		CREATE EXTENSION pg_walinspect;
		CREATE TABLE fu (id int, g int, v int);
		INSERT INTO fu SELECT i, i % 7, i FROM generate_series(1, 20000) i;
		CREATE INDEX fu_i ON fu USING sift_col (id, g, v) WITH (key_order = 'id', track_changes = 'ctid');
		UPDATE fu SET v = v + 1 WHERE id = 100;
		UPDATE fu SET v = v + 1 WHERE id = 101 });
	my $from = $primary->lsn('insert');
	$primary->safe_psql('postgres', join(";\n", map { "UPDATE fu SET v = v + 1 WHERE id = " . (200 + $_) } 0 .. 9));
	my $to = $primary->lsn('insert');
	is($primary->safe_psql('postgres', qq{SELECT count FROM pg_get_wal_stats('$from', '$to', true) WHERE "resource_manager/record_type" = 'Generic/Generic'}),
	   10, 'ten UPDATEs write ten Generic records: the slab and the pending deletes of a partition share one');
	my $want = $primary->safe_psql('postgres', $rows);
	$primary->wait_for_catchup($standby, 'replay', $to);
	is($standby->safe_psql('postgres', $by_index), $want, 'the standby reads the rows and the deletes through the index');
	kill9_wait($primary);
	$primary->start;
	is($primary->safe_psql('postgres', $by_index), $want, 'recovery after kill -9 gives them');
	same_state('after the updates');
};

subtest 'block 0 keeps the summary of the partitions, split by time into hot and cold parts' => sub {
	my $state = sub {
		my ($node) = @_;
		return $node->safe_psql('postgres', q{
			SELECT sift._test_f2_check('ev_idx') || E'\n'
			  || (SELECT i::text FROM sift._test_f2_meta_inline('ev_idx') i) || E'\n'
			  || (SELECT string_agg(level || ':' || flags || ':' || nrows, ';' ORDER BY part_gen) FROM sift._test_f2_parts('ev_idx', 1, NULL)) });
	};
	my $at = sub {
		my ($what) = @_;
		$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
		is($state->($standby), $state->($primary), "$what: the standby has the summary and the parts of the primary");
	};
	$primary->safe_psql('postgres', q{
		CREATE TABLE ev (id bigint, tenant int, created_at timestamptz, updated_at timestamptz, v float8);
		CREATE INDEX ev_idx ON ev USING sift_col (id, tenant, created_at, updated_at, v) WITH (key_order = 'id') });
	for my $b (0 .. 3)
	{
		my $age = $b < 3 ? "now() - interval '" . (120 - 10 * $b) . " days'" : 'now()';
		$primary->safe_psql('postgres', "INSERT INTO ev SELECT $b * 20000 + i, i % 40, $age + i * interval '1 second', $age + i * interval '1 second', i FROM generate_series(1, 20000) i");
	}
	like($primary->safe_psql('postgres', "SELECT inline_len > 0 AND flagged FROM sift._test_f2_meta_inline('ev_idx')"), qr/^t$/, 'the partition of the index is in block 0');
	$at->('four parts');
	$primary->safe_psql('postgres', q{
		INSERT INTO ev SELECT 90000 + i, i % 40, CASE WHEN i % 2 = 0 THEN now() ELSE now() - interval '100 days' END, now(), i FROM generate_series(1, 4000) i;
		SELECT sift._test_f2_compact('ev_idx', 1, NULL, array_agg(part_gen)::int8[]) FROM sift._test_f2_parts('ev_idx', 1, NULL) });
	is($primary->safe_psql('postgres', "SELECT count(*) FILTER (WHERE flags & 32 <> 0) || '/' || count(*) FILTER (WHERE flags & 64 <> 0) FROM sift._test_f2_parts('ev_idx', 1, NULL)"),
	   '1/1', 'the compaction wrote a hot part and a cold one');
	$at->('hot and cold parts');
	my $want = $state->($primary);
	kill9_wait($primary);
	$primary->start;
	is($state->($primary), $want, 'kill -9: recovery gives the summary the directory has');
	$at->('after recovery');
};

subtest 'a prepared transaction writes its staged rows at PREPARE, under its own transaction id' => sub {
	my $rows = sub { return $_[0]->safe_psql('postgres', "SELECT coalesce(sum(total_rows), 0) FROM sift._test_f2_partitions('$idx')") };
	my $before = $rows->($primary);
	$primary->safe_psql('postgres', qq{
		BEGIN;
		SELECT sift._test_f2_stage('$idx', 300, 600000000, 9000000, -1);
		SAVEPOINT a;
		SELECT sift._test_f2_stage('$idx', 50, 600010000, 9100000, -1);
		RELEASE a;
		PREPARE TRANSACTION 'f2_commit';
		BEGIN;
		SELECT sift._test_f2_stage('$idx', 200, 700000000, 9200000, -1);
		PREPARE TRANSACTION 'f2_abort' });
	is($rows->($primary), $before + 350 + 200, 'both prepared transactions wrote their parts at PREPARE');
	$primary->safe_psql('postgres', q{COMMIT PREPARED 'f2_commit'; ROLLBACK PREPARED 'f2_abort'});
	same_state('prepared transactions');
	my $xids = $primary->safe_psql('postgres', qq{
		SELECT count(*) FROM sift._test_f2_parts('$idx', 1, NULL) p, LATERAL sift._test_f2_part_xids('$idx', 1, NULL, p.part_gen) x WHERE x.min_xid <> x.max_xid});
	is($xids, 0, 'every staged part carries one transaction id: the top one of its writer');
	kill9_wait($primary);
	$primary->start;
	same_state('prepared transactions after recovery');
};

subtest 'an unlogged index starts from the init fork that was logged for it' => sub {
	my $blocks = sub { return $_[0]->safe_psql('postgres', "SELECT pg_relation_size('ulg_i', 'init') / current_setting('block_size')::int") };
	my $by_index = "SET enable_seqscan = off; SET enable_indexscan = off; SELECT count(*), coalesce(sum(v), 0) FROM ulg WHERE g = 3";
	$primary->safe_psql('postgres', q{
		CREATE UNLOGGED TABLE ulg (id int, g int, v int);
		INSERT INTO ulg SELECT i, i % 10, i FROM generate_series(1, 5000) i;
		CREATE INDEX ulg_i ON ulg USING sift_col (id, g, v) WITH (key_order = 'id') });
	is($blocks->($primary), 10, 'the init fork has the metapage, the allocator, the directory and the blocks of the first slab');
	is($primary->safe_psql('postgres', $by_index), '500|1249000', 'the index answers as the heap does');
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
	is($blocks->($standby), 10, 'the standby replayed the init fork');
	kill9_wait($primary);
	$primary->start;
	is($primary->safe_psql('postgres', 'SELECT count(*) FROM ulg'), 0, 'after a crash the unlogged table is empty');
	like($primary->safe_psql('postgres', "SELECT sift._test_f2_check('ulg_i')"), qr/^parts=0 quarantined=0\/0 /, 'after a crash the index is the empty one of the init fork');
	$primary->safe_psql('postgres', 'INSERT INTO ulg SELECT i, i % 10, i FROM generate_series(1, 3000) i');
	is($primary->safe_psql('postgres', $by_index), '300|449400', 'the rows written after the crash are read through it');
};

subtest 'CREATE INDEX CONCURRENTLY with deletes and updates running keeps no dead row' => sub {
	my $heap = "SET enable_bitmapscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = off; SELECT count(*), sum(v) FROM cc WHERE g < 5";
	my $by_index = "SET enable_seqscan = off; SET enable_indexscan = off; SELECT count(*), sum(v) FROM cc WHERE g < 5";
	$primary->safe_psql('postgres', q{
		CREATE TABLE cc (id int, g int, v int);
		INSERT INTO cc SELECT i, i % 10, i FROM generate_series(1, 30000) i;
		DELETE FROM cc WHERE id % 2 = 0 });
	my $old = $primary->background_psql('postgres');
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM cc');
	my $cic = $primary->background_psql('postgres');
	$cic->query_until(qr/cic_started/, "SET client_min_messages = warning;\n\\echo cic_started\nCREATE INDEX CONCURRENTLY cc_i ON cc USING sift_col (id, g, v) WITH (key_order = 'id', track_changes = 'ctid');\n");
	$primary->safe_psql('postgres', q{
		DELETE FROM cc WHERE id % 3 = 0;
		UPDATE cc SET v = v + 1 WHERE id % 5 = 0;
		INSERT INTO cc SELECT i, i % 10, i FROM generate_series(30001, 31000) i;
		DELETE FROM cc WHERE id > 30500 });
	$old->query_safe('COMMIT');
	$cic->query_safe('SELECT 1');
	$old->quit;
	$cic->quit;
	is($primary->safe_psql('postgres', "SELECT indisvalid FROM pg_index WHERE indexrelid = 'cc_i'::regclass"), 't', 'the index is valid');
	my $want = $primary->safe_psql('postgres', $heap);
	is($primary->safe_psql('postgres', $by_index), $want, 'the index holds no row that the heap has deleted');
	$primary->safe_psql('postgres', 'VACUUM cc');
	is($primary->safe_psql('postgres', $by_index), $want, 'nor after a VACUUM');
	like($primary->safe_psql('postgres', "SELECT sift._test_f2_check('cc_i')"), qr/^parts=\d+ /, 'the allocator, the queue and the manifest agree');
};

subtest 'tiny parts packed into shared pages are replayed whole and survive a crash' => sub {
	my $state = sub {
		my ($node) = @_;
		return $node->safe_psql('postgres', q{
			SELECT sift._test_f2_check('pk_idx') || E'\n'
			  || (SELECT string_agg(p.part_id || ':' || i.header_blk || ':' || i.packed || ':' || i.slot || ':' || i.part_gen, ';' ORDER BY p.part_id)
				  FROM sift._test_f2_partitions('pk_idx') p, LATERAL sift._test_f2_pack_info('pk_idx', p.part_id, p.key_lo) i) });
	};
	my $at = sub {
		my ($what) = @_;
		$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
		is($state->($standby), $state->($primary), "$what: the standby has the shared pages and the slots of the primary");
		like($state->($primary), qr/^parts=12 /, "$what: the allocator, the queue and the manifest agree");
	};
	$primary->safe_psql('postgres', q{
		CREATE TABLE pk (v int);
		CREATE INDEX pk_idx ON pk USING sift_store_test (v);
		SELECT sift._test_f2_init('pk_idx');
		SELECT sift._test_f2_split_many('pk_idx', 1, NULL, 11, 1000);
		SELECT count(sift._test_f2_add_part('pk_idx', part_id, key_lo, 8, COALESCE(key_lo, 0) + 10, part_id * 100, 0, 1)) FROM sift._test_f2_partitions('pk_idx') });
	$at->('twelve parts of a page each');
	is($primary->safe_psql('postgres', "SELECT sift._test_f2_pack_tails('pk_idx')"), 12, 'the tiny parts are packed');
	$at->('packed');
	is($primary->safe_psql('postgres', "SELECT count(DISTINCT header_blk) FROM (SELECT i.header_blk FROM sift._test_f2_partitions('pk_idx') p, LATERAL sift._test_f2_pack_info('pk_idx', p.part_id, p.key_lo) i) q"),
	   2, 'on two pages');
	my $want = $state->($primary);
	kill9_wait($primary);
	$primary->start;
	is($state->($primary), $want, 'kill -9: recovery gives the shared pages');
	$primary->safe_psql('postgres', q{
		SELECT sift._test_f2_compact('pk_idx', p.part_id, p.key_lo, ARRAY[i.part_gen]::int8[])
		FROM sift._test_f2_partitions('pk_idx') p, LATERAL sift._test_f2_pack_info('pk_idx', p.part_id, p.key_lo) i WHERE p.part_id IN (1, 2) });
	$at->('two parts of a shared page compacted');
};

subtest 'reclaiming extents logs the snapshot conflict a standby replays' => sub {
	$standby->safe_psql('postgres', "ALTER SYSTEM SET hot_standby_feedback = off; SELECT pg_reload_conf()");
	$primary->poll_query_until('postgres', "SELECT xmin IS NULL FROM pg_replication_slots WHERE slot_name = 'f2_standby'");
	my ($gen) = $primary->safe_psql('postgres', "SELECT min(part_gen) FROM sift._test_f2_parts('$idx', 1, NULL)");
	$primary->safe_psql('postgres', "SELECT * FROM sift._test_f2_compact('$idx', 1, NULL, ARRAY[$gen])");
	$primary->safe_psql('postgres', 'SELECT pg_current_xact_id()');
	my $from = $primary->lsn('insert');
	$primary->safe_psql('postgres', "SELECT sift._test_f2_reclaim('$idx')");
	my $to = $primary->lsn('insert');
	$primary->wait_for_catchup($standby, 'replay', $to);
	cmp_ok($primary->safe_psql('postgres', qq{SELECT coalesce(sum(count), 0) FROM pg_get_wal_stats('$from', '$to', true) WHERE "resource_manager/record_type" = 'Btree/REUSE_PAGE'}),
		   '>', 0, 'the reclaim of matured extents logs the snapshot conflict a standby replays');
	same_state('snapshot conflict');
	$standby->safe_psql('postgres', "ALTER SYSTEM SET hot_standby_feedback = on; SELECT pg_reload_conf()");
};

subtest 'crash recovery' => sub {
	my ($id, $lo) = split /\|/, $primary->safe_psql('postgres', "SELECT part_id, key_lo FROM sift._test_f2_partitions('$idx') WHERE key_hi IS NULL");
	$primary->safe_psql('postgres', qq{
		SELECT sift._test_f2_split_many('$idx', $id, $lo, 20, 1000000000);
		BEGIN;
		SELECT sift._test_f2_stage('$idx', 800, 30000000000, 8000000);
		COMMIT });
	my $before = state($primary);
	kill9_wait($primary);
	$primary->start;
	is(state($primary), $before, 'kill -9: recovery gives the structures that were committed');
	same_state('after recovery');
};

my $checksums = $primary->safe_psql('postgres', 'SHOW data_checksums');
$primary->stop;
$standby->stop;
SKIP:
{
	skip 'data checksums are off', 2 unless $checksums eq 'on';
	command_ok([ 'pg_checksums', '--check', '-D', $primary->data_dir ], 'every page of the primary, the init forks and the pages of the structures included, has its checksum');
	command_ok([ 'pg_checksums', '--check', '-D', $standby->data_dir ], 'every page of the standby has its checksum');
}
done_testing();
