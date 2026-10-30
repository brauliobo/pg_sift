# Column/table space reclamation: exact crash boundaries, reader exclusion, replay conflicts (feedback on/off), and disabled truncation.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $cursor_settings = 'SET enable_seqscan = off; SET sift.enable_agg = on; SET enable_indexscan = off; SET enable_bitmapscan = off; SET max_parallel_workers_per_gather = 0';
my $standby;
my $primary = sift_node('space_primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "sift.async_maintenance = off\nsift.test_cut_with_standby = on\nwal_consistency_checking = 'generic'\nmax_parallel_maintenance_workers = 0");
$primary->start;
install_sift_test($primary, 'space crash injection needs the testing build');

sub fixture
{
	my ($kind) = @_;
	my $am = $kind eq 'tbl' ? ' USING sift_table' : '';
	if ($standby)
	{
		# Prepare final groups without an earlier scenario's feedback horizon. Feedback is enabled again before the tested cursor opens.
		$standby->safe_psql('postgres', "ALTER SYSTEM SET hot_standby_feedback = 'off'; SELECT pg_reload_conf()");
		$primary->poll_query_until('postgres', "SELECT xmin IS NULL FROM pg_replication_slots WHERE slot_name = 'space_slot'") or die 'old feedback did not clear';
	}
	$primary->safe_psql('postgres', "DROP TABLE IF EXISTS s; CREATE TABLE s (id int, v bigint, body text)$am WITH (autovacuum_enabled = false, vacuum_truncate = false)");
	$primary->safe_psql('postgres', "INSERT INTO s SELECT g, g * 3, repeat(md5(g::text), CASE WHEN g % 400 = 0 THEN 300 ELSE 1 END) FROM generate_series(1, 60000) g");
	$primary->safe_psql('postgres', "CREATE INDEX s_col ON s USING sift_col(v, body) WITH (vacuum_truncate = false)") if $kind eq 'col';
	$primary->safe_psql('postgres', 'UPDATE s SET v = v + 1 WHERE id % 10 = 0; DELETE FROM s WHERE id % 20 = 1');
	$primary->safe_psql('postgres', 'VACUUM s');
	$primary->safe_psql('postgres', $kind eq 'col' ? 'ALTER INDEX s_col RESET (vacuum_truncate)' : 'ALTER TABLE s RESET (vacuum_truncate)');
}
sub info_sql
{
	my ($kind) = @_;
	return $kind eq 'col' ? "sift.col_index_info('s_col')" : "sift.table_space('s')";
}
sub size
{
	my ($node, $kind) = @_;
	return $node->safe_psql('postgres', 'SELECT relation_blocks FROM ' . info_sql($kind));
}
sub digest
{
	my ($node) = @_;
	return $node->safe_psql('postgres', 'SET sift.enable_agg = off; SELECT count(*), sum(id), sum(v), sum(length(body)) FROM s');
}
sub healthy
{
	my ($node, $kind, $expected) = @_;
	is(digest($node), $expected, 'all rows and overflow values survived');
	$node->safe_psql('postgres', "SELECT sift.col_verify('s_col', true)") if $kind eq 'col';
	my $rel = $kind eq 'col' ? 's_col' : 's';
	is($node->safe_psql('postgres', "SELECT sift._test_check('$rel') >= 0"), 't', 'every referenced and free extent has one owner');
	is($node->safe_psql('postgres', 'SELECT relation_blocks >= high_water AND free_blocks <= relation_blocks FROM ' . info_sql($kind)), 't', 'file covers its free map and high-water mark');
}

# A crash after each WAL boundary must leave the old or the new complete directory, with no double-owned or missing pages.
for my $kind (qw(col tbl))
{
	for my $phase (1 .. 3)
	{
		subtest "$kind crash phase $phase" => sub {
			fixture($kind);
			my $expected = digest($primary);
			my $offset = -s $primary->logfile;
			system("$ENV{SIFT_REPO}/scripts/leakcheck.sh", 'expect', 1, 'terminated by signal 9');
			my ($out, $err) = ('', '');
			my $rc = $primary->psql('postgres', "SET sift.test_space_crash = $phase; VACUUM s", stdout => \$out, stderr => \$err);
			ok($rc != 0, 'the injected boundary killed VACUUM');
			ok($primary->log_contains(qr/sift test: space crash phase $phase/, $offset), 'the requested boundary was reached');
			$primary->wait_for_log(qr/database system is ready to accept connections/, $offset);
			healthy($primary, $kind, $expected);
			$primary->safe_psql('postgres', 'VACUUM s');
			my $rel = $kind eq 'col' ? 's_col' : 's';
			is($primary->safe_psql('postgres', "SELECT sift._test_check('$rel')"), '0', 'VACUUM also reclaimed any crash orphans');
			healthy($primary, $kind, $expected);
			$primary->safe_psql('postgres', "INSERT INTO s VALUES (70001, 17, repeat('after recovery ', 2000)); VACUUM s");
			is($primary->safe_psql('postgres', 'SELECT count(*) FROM s WHERE id = 70001 AND length(body) = 30000'), '1', 'subsequent overflow write and VACUUM succeed');
		};
	}
}

# A segment larger than the allowance moves down a round at a time: the crash after the first round leaves the segment whole, its header and extents either those of
# before or after, and the next VACUUM goes on
subtest 'col shift crash after a round' => sub {
	fixture('col');
	$primary->safe_psql('postgres', "SELECT sift.col_merge('s_col', true)");
	my $expected = digest($primary);
	my $before = size($primary, 'col');
	my $offset = -s $primary->logfile;
	system("$ENV{SIFT_REPO}/scripts/leakcheck.sh", 'expect', 1, 'terminated by signal 9');
	my ($out, $err) = ('', '');
	my $rc = $primary->psql('postgres', "SET sift.vacuum_rewrite_limit = 1; SET sift.test_space_crash = 4; VACUUM s", stdout => \$out, stderr => \$err);
	ok($rc != 0, 'the injected boundary killed VACUUM');
	ok($primary->log_contains(qr/sift test: space crash phase 4/, $offset), 'the boundary after the first round was reached');
	$primary->wait_for_log(qr/database system is ready to accept connections/, $offset);
	healthy($primary, 'col', $expected);
	$primary->safe_psql('postgres', 'SET sift.vacuum_rewrite_limit = 1; VACUUM s');
	is($primary->safe_psql('postgres', "SELECT sift._test_check('s_col')"), '0', 'VACUUM also reclaimed any crash orphans');
	healthy($primary, 'col', $expected);
	cmp_ok(size($primary, 'col'), '<=', $before, 'the file did not grow');
};

# A sift_table loaded in batches: the merge of the batches lies above holes that its own VACUUM released, in pieces. Like an index, the highest pieces go down a round at a time
# (a table that is shifted as well as an index: the crash after the first round leaves the segment whole), a session that had read the table sees the same rows after, and the file is cut
subtest 'tbl shift crash after a round' => sub {
	$primary->safe_psql('postgres', "DROP TABLE IF EXISTS s; CREATE TABLE s (id int, v bigint, body text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false)");
	$primary->safe_psql('postgres', "INSERT INTO s SELECT g, g * 3, md5(g::text) || md5((g * 7)::text) FROM generate_series($_ * 80000 + 1, ($_ + 1) * 80000) g") for 0 .. 1;
	$primary->safe_psql('postgres', 'VACUUM s');
	$primary->safe_psql('postgres', 'ALTER TABLE s RESET (vacuum_truncate)');
	my $expected = digest($primary);
	my $before = size($primary, 'tbl');
	my $offset = -s $primary->logfile;
	system("$ENV{SIFT_REPO}/scripts/leakcheck.sh", 'expect', 1, 'terminated by signal 9');
	my ($out, $err) = ('', '');
	my $rc = $primary->psql('postgres', "SET sift.vacuum_rewrite_limit = 1; SET sift.test_space_crash = 4; VACUUM s", stdout => \$out, stderr => \$err);
	ok($rc != 0, 'the injected boundary killed VACUUM');
	ok($primary->log_contains(qr/sift test: space crash phase 4/, $offset), 'the boundary after the first round was reached');
	$primary->wait_for_log(qr/database system is ready to accept connections/, $offset);
	healthy($primary, 'tbl', $expected);
	$primary->safe_psql('postgres', 'SET sift.vacuum_rewrite_limit = 1; VACUUM s');
	is($primary->safe_psql('postgres', "SELECT sift._test_check('s')"), '0', 'VACUUM also reclaimed any crash orphans');
	healthy($primary, 'tbl', $expected);
	cmp_ok(size($primary, 'tbl'), '<=', $before, 'the file did not grow');
	my $bg = $primary->background_psql('postgres');
	my $rows = $bg->query_safe('SELECT count(*) || chr(58) || sum(v) FROM s');
	$primary->safe_psql('postgres', 'VACUUM s');
	is($primary->safe_psql('postgres', 'SELECT free_blocks * 20 < relation_blocks FROM sift.table_space(\'s\')'), 't', 'the holes are gone: under 5% of the file is free');
	is($bg->query_safe('SELECT count(*) || chr(58) || sum(v) FROM s'), $rows, 'the session that read the table before the last rounds reads the same rows after them');
	$bg->quit;
};

for my $kind (qw(col tbl))
{
	subtest "$kind primary cursor and disabled truncation" => sub {
		fixture($kind);
		my $expected = digest($primary);
		my $before = size($primary, $kind);
		my $bg = $primary->background_psql('postgres');
		my $query = $kind eq 'col' ? 'SELECT v, count(*) FROM s WHERE v > 0 GROUP BY v' : 'SELECT * FROM s';
		like($primary->safe_psql('postgres', "$cursor_settings; EXPLAIN (COSTS OFF) $query"), qr/SiftAgg/, 'cursor reads the column index') if $kind eq 'col';
		$bg->query_safe("BEGIN; $cursor_settings; DECLARE c CURSOR FOR $query; FETCH 1 FROM c");
		$primary->safe_psql('postgres', "SET statement_timeout = '3s'; VACUUM s");
		is(size($primary, $kind), $before, 'an open cursor skips reclamation without waiting');
		$bg->query_safe('COMMIT');
		$bg->quit;
		if ($kind eq 'tbl')
		{
			$primary->safe_psql('postgres', 'VACUUM (TRUNCATE FALSE) s');
			is(size($primary, $kind), $before, 'TRUNCATE FALSE preserves the table file');
		}
		else
		{
			$primary->safe_psql('postgres', 'ALTER INDEX s_col SET (vacuum_truncate = false); VACUUM s');
			is(size($primary, $kind), $before, 'vacuum_truncate=false preserves the index file');
			$primary->safe_psql('postgres', 'ALTER INDEX s_col RESET (vacuum_truncate)');
		}
		$primary->safe_psql('postgres', 'VACUUM s');
		cmp_ok(size($primary, $kind), '<', $before, 'closing the cursor allows reclamation');
		healthy($primary, $kind, $expected);
	};
}

subtest 'a young table buffer keeps its row versions and overflow links when copied' => sub {
	fixture('tbl');
	# A snapshot of another relation prevents promotion of the new row without retaining a lock on s.
	$primary->safe_psql('postgres', 'SELECT pg_current_xact_id()');
	my $bg = $primary->background_psql('postgres');
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM pg_class');
	$primary->safe_psql('postgres', "INSERT INTO s VALUES (70001, 17, repeat('young overflow ', 2000))");
	my $before = size($primary, 'tbl');
	$primary->safe_psql('postgres', 'VACUUM s');
	cmp_ok(size($primary, 'tbl'), '<', $before, 'the file shrank while the new row could not be promoted');
	is($primary->safe_psql('postgres', 'SELECT count(*) FROM s WHERE id = 70001 AND length(body) = 30000'), '1', 'the copied young buffer retains its overflow bytes');
	is($bg->query_safe('SELECT count(*) FROM s WHERE id = 70001'), '0', 'the older snapshot still excludes the young row');
	is($primary->safe_psql('postgres', "SELECT sift._test_check('s')"), '0', 'copied buffers and free runs have unique key_sets');
	$bg->query_safe('COMMIT');
	$bg->quit;
};

$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('space_slot', true)");
$primary->backup('base');
$standby = PostgreSQL::Test::Cluster->new('space_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'space_slot'\nwal_receiver_status_interval = '1s'\nmax_standby_streaming_delay = '1s'\nwal_consistency_checking = 'generic'");
$standby->start;
for my $feedback (qw(off on))
{
	for my $kind (qw(col tbl))
	{
		subtest "$kind standby cursor, feedback $feedback" => sub {
			fixture($kind);
			my $expected = digest($primary);
			$standby->safe_psql('postgres', "ALTER SYSTEM SET hot_standby_feedback = '$feedback'; SELECT pg_reload_conf()");
			$primary->wait_for_catchup($standby);
			my $bg = $standby->background_psql('postgres', on_error_stop => 0);
			my $query = $kind eq 'col' ? 'SELECT v, count(*) FROM s WHERE v > 0 GROUP BY v' : 'SELECT * FROM s';
			like($standby->safe_psql('postgres', "$cursor_settings; EXPLAIN (COSTS OFF) $query"), qr/SiftAgg/, 'cursor reads the column index') if $kind eq 'col';
			$bg->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ; $cursor_settings; DECLARE c CURSOR FOR $query; FETCH 1 FROM c");
			$primary->poll_query_until('postgres', "SELECT xmin IS NOT NULL FROM pg_replication_slots WHERE slot_name = 'space_slot'") or die 'cursor feedback did not arrive' if $feedback eq 'on';
			my $offset = -s $standby->logfile;
			my $before = size($primary, $kind);
			$primary->safe_psql('postgres', 'VACUUM s');
			cmp_ok(size($primary, $kind), '<', $before, 'primary reclaimed before replaying the moves');
			$primary->wait_for_catchup($standby);
			ok($standby->log_contains(qr/terminating connection due to conflict with recovery/, $offset), 'exclusive lock replay cancelled the cursor before immediate reuse');
			eval { $bg->quit };
			healthy($standby, $kind, $expected);
			is(size($standby, $kind), size($primary, $kind), 'standby and primary file sizes agree');
		};
	}
}
subtest 'no exclusive-lock replay when a small table has no space to reclaim' => sub {
	$primary->safe_psql('postgres', 'CREATE TABLE tiny (v int) USING sift_table WITH (autovacuum_enabled = false); INSERT INTO tiny VALUES (1)');
	is($primary->safe_psql('postgres', "SELECT free_blocks FROM sift.table_space('tiny')"), '0', 'the small file has no free blocks');
	$primary->wait_for_catchup($standby);
	my $bg = $standby->background_psql('postgres');
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM tiny');
	$primary->safe_psql('postgres', 'VACUUM tiny');
	$primary->wait_for_catchup($standby);
	is($bg->query_safe('SELECT count(*) FROM tiny'), '1', 'a no-op space step preserves the standby snapshot');
	$bg->query_safe('COMMIT');
	$bg->quit;
};

$standby->stop;
$primary->stop;
done_testing();
