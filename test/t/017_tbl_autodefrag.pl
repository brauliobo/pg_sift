# sift_table maintenance without VACUUM: the backend that closes a write buffer asks a worker for a pass when the tombstones or the free
# blocks cross the thresholds and not below them, not with the setting off, and the pass keeps every row; the same pass is crash safe at the
# boundaries of the space step.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = sift_node('autodefrag');
$node->append_conf('postgresql.conf', "autovacuum = off\nsift.table_auto_defrag = on\nsift.maintenance_naptime = 200ms\nsift.log_min_duration = 0\nsift.table_auto_defrag_delay = 0\nsift.table_write_buffer_limit = 2048\nmax_parallel_maintenance_workers = 0");
$node->start;
install_sift($node);
plan skip_all => 'space crash injection needs the testing build' unless $node->safe_psql('postgres', "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift_test'");
$node->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');

# The backend asks for a pass when it closes a write buffer (2 MB here, the smallest), and the churn of a subtest, about 100 bytes a row,
# has to close one: with the default 4 MB buffer 30000 rows (3 MB) never reached the check and no worker was asked for
my $rows = $ENV{SIFT_ROWS} // 30000;
my $worker = qr/table maintenance worker finished/;
my $digest = 'SET sift.enable_agg = off; SELECT count(*) || $$:$$ || sum(id) || $$:$$ || sum(v) || $$:$$ || sum(length(t)) FROM %s';

sub fill
{
	my ($tbl) = @_;
	$node->safe_psql('postgres', "DROP TABLE IF EXISTS $tbl; CREATE TABLE $tbl (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false)");
	$node->safe_psql('postgres', "INSERT INTO $tbl SELECT g, g * 3, 'text ' || md5(g::text) FROM generate_series(1, $rows) g");
	# settled into column segments, the tombstone log empty: the thresholds are about what the churn adds
	$node->safe_psql('postgres', "VACUUM $tbl");
	# the VACUUM closes a buffer too and may have asked for a pass; a request inside the naptime of that one is not made
	sleep 1;
}
# rows written again so that write buffers fill and close (the point where the backend looks)
sub churn_writes
{
	my ($tbl, $settings, $from) = @_;
	$node->safe_psql('postgres', "$settings; INSERT INTO $tbl SELECT g, g * 3, 'text ' || md5(g::text) FROM generate_series($from, $from + $rows) g");
}
sub fired
{
	my ($offset) = @_;
	return $node->wait_for_log($worker, $offset);
}
sub quiet_for
{
	my ($offset, $seconds) = @_;
	sleep $seconds;
	return slurp_file($node->logfile, $offset) !~ $worker;
}

subtest 'a table that was never written is read and deleted from without a look at its pages' => sub {
	$node->safe_psql('postgres', 'DROP TABLE IF EXISTS z; CREATE TABLE z (id int) USING sift_table; CREATE INDEX z_id ON z (id)');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM z; SELECT count(*) FROM z WHERE id = 1; DELETE FROM z WHERE id = 1'), "0\n0", 'empty');
};

subtest 'below the thresholds nothing is asked for' => sub {
	fill('a');
	$node->safe_psql('postgres', 'DELETE FROM a WHERE id % 50 = 0');	# 2% of the rows, the default threshold is 5
	my $offset = -s $node->logfile;
	my $before = $node->safe_psql('postgres', sprintf($digest, 'a'));
	churn_writes('a', 'SET sift.table_auto_defrag = on', $rows + 1);
	ok(quiet_for($offset, 3), 'no worker started');
	is($node->safe_psql('postgres', "SELECT sum(dead_rows) FROM sift.table_segments('a') WHERE kind = 'columns'"), '0', 'no bit cleared');
};

# Half the file of a table that only appends is free blocks at any moment (the write buffers of the promoted rows, taken again by the next
# ones): a pass would cut the file for the writers to extend it
subtest 'a table that only appends asks for no pass' => sub {
	$node->safe_psql('postgres', 'DROP TABLE IF EXISTS ap; CREATE TABLE ap (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false)');
	my $offset = -s $node->logfile;
	for my $i (1 .. 8)
	{
		$node->safe_psql('postgres', "INSERT INTO ap SELECT g + $i * 100000, g * 3, 'text ' || md5(g::text) FROM generate_series(1, $rows) g");
	}
	my ($blocks, $free) = split /\|/, $node->safe_psql('postgres', "SELECT relation_blocks, free_blocks FROM sift.table_space('ap')");
	cmp_ok($blocks, '>=', 1000, 'a file the space rule looks at');
	ok(quiet_for($offset, 2), 'no worker');
};

subtest 'with the setting off nothing is asked for above them' => sub {
	fill('b');
	$node->safe_psql('postgres', 'DELETE FROM b WHERE id % 10 < 4');	# 40%
	my $offset = -s $node->logfile;
	churn_writes('b', 'SET sift.table_auto_defrag = off', $rows + 1);
	ok(quiet_for($offset, 3), 'no worker started');
};

subtest 'above the thresholds a worker rewrites the segments and keeps every row' => sub {
	fill('c');
	my $blocks = $node->safe_psql('postgres', "SELECT relation_blocks FROM sift.table_space('c')");
	$node->safe_psql('postgres', 'DELETE FROM c WHERE id % 10 < 4');
	my $offset = -s $node->logfile;
	churn_writes('c', 'SET sift.table_auto_defrag = on', $rows + 1);
	my $after_writes = $node->safe_psql('postgres', sprintf($digest, 'c'));
	ok(fired($offset), 'a worker started and finished');
	# the rows deleted are gone from the segments the worker rewrote; what the writer added is not touched
	ok($node->poll_query_until('postgres', "SELECT sum(rows) FILTER (WHERE kind = 'columns') <= (SELECT count(*) FROM c) FROM sift.table_segments('c')"), 'the segments no longer hold more rows than are live');
	is($node->safe_psql('postgres', sprintf($digest, 'c')), $after_writes, 'the same rows and sums');
	my $cleared = $node->safe_psql('postgres', "SELECT count(*) FROM c WHERE id % 10 < 4 AND id <= $rows");
	is($cleared, '0', 'the deleted rows stay deleted');
	# VACUUM after it still finishes the job for the indexes and the log
	$node->safe_psql('postgres', 'VACUUM c');
	is($node->safe_psql('postgres', sprintf($digest, 'c')), $after_writes, 'the same rows after VACUUM');
};

# A table that only deletes closes no write buffer: the deletes and the reads look at it, each backend once per naptime (200 ms here)
subtest 'a read after the deletes asks for the pass' => sub {
	fill('e');
	# the session that deletes does not ask (a statement slower than the naptime would ask for rows that are not committed yet): the read does
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM e WHERE id % 10 < 4');
	my $offset = -s $node->logfile;
	sleep 1;
	$node->safe_psql('postgres', 'SELECT count(*) FROM e');	# the aggregate source
	ok(fired($offset), 'a worker started and finished');
	is($node->safe_psql('postgres', "SELECT sum(dead_rows) FROM sift.table_segments('e') WHERE kind = 'columns'"), '0', 'the dead rows were cleared and dropped');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM e'), $rows * 6 / 10, 'the live rows are all there');
};

subtest 'a delete after the earlier ones committed asks for the pass' => sub {
	fill('f');
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM f WHERE id % 10 < 4');
	my $offset = -s $node->logfile;
	sleep 1;
	$node->safe_psql('postgres', 'DELETE FROM f WHERE id = 5');
	ok(fired($offset), 'a worker started and finished');
};

subtest 'a read below the thresholds, or with the setting off, asks for nothing' => sub {
	fill('g');
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM g WHERE id % 50 = 0');
	fill('h');
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM h WHERE id % 10 < 4');
	my $offset = -s $node->logfile;
	sleep 1;
	$node->safe_psql('postgres', 'SELECT count(*) FROM g; SELECT * FROM g WHERE id = 7');
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; SELECT count(*) FROM h');
	ok(quiet_for($offset, 2), 'no worker started');
};

# A transaction older than the deletes holds the horizon: the tombstones are not removable, the pass would read the table to change nothing
subtest 'a pass that could remove nothing is not asked for until the old transaction ends' => sub {
	fill('n');
	my $old = $node->background_psql('postgres');
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM n');
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM n WHERE id % 10 < 4');
	my $offset = -s $node->logfile;
	sleep 1;
	$node->safe_psql('postgres', 'SELECT count(*) FROM n');
	$node->safe_psql('postgres', 'DELETE FROM n WHERE id = 5');
	ok(quiet_for($offset, 3), 'no worker started');
	is($node->safe_psql('postgres', "SELECT sum(dead_rows) FROM sift.table_segments('n') WHERE kind = 'columns'"), '0', 'no segment was read for it');
	$old->query_safe('COMMIT');
	$old->quit;
	sleep 1;
	$node->safe_psql('postgres', 'SELECT count(*) FROM n');
	ok(fired($offset), 'a worker started once the old transaction ended');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM n'), $rows * 6 / 10 - 1, 'the live rows are all there');
};

# sift.table_auto_defrag_max_active: the worker leaves the load alone while more clients than that run a statement
subtest 'the pass waits while the clients are busy' => sub {
	fill('m');
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM m WHERE id % 10 < 4');
	$node->safe_psql('postgres', 'ALTER SYSTEM SET sift.table_auto_defrag_max_active = 1; SELECT pg_reload_conf()');
	my @busy = map { my $b = $node->background_psql('postgres'); $b->query_until(qr/started/, "\\echo started\nSELECT pg_sleep(60);\n"); $b } 1 .. 2;
	my $offset = -s $node->logfile;
	sleep 1;
	$node->safe_psql('postgres', 'SELECT count(*) FROM m');
	ok(quiet_for($offset, 3), 'no pass while two other clients run a statement');
	$node->safe_psql('postgres', "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(60)%' AND pid <> pg_backend_pid()");
	ok(fired($offset), 'the pass ran when they were done');
	$_->quit for @busy;
	$node->safe_psql('postgres', 'ALTER SYSTEM RESET sift.table_auto_defrag_max_active; SELECT pg_reload_conf()');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM m'), $rows * 6 / 10, 'the live rows are all there');
};

# sift.table_auto_defrag_wal_rate: the worker sleeps whenever it is ahead of the rate, so a pass cannot take less than its WAL needs
subtest 'the pass keeps to a WAL rate' => sub {
	my $big = 100000;
	$node->safe_psql('postgres', "DROP TABLE IF EXISTS w; CREATE TABLE w (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false)");
	$node->safe_psql('postgres', "INSERT INTO w SELECT g, g * 3, 'text ' || md5(g::text) FROM generate_series(1, $big) g; VACUUM w");
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM w WHERE id % 10 < 4');
	my $expected = $node->safe_psql('postgres', sprintf($digest, 'w'));
	$node->safe_psql('postgres', 'ALTER SYSTEM SET sift.table_auto_defrag_wal_rate = 512; SELECT pg_reload_conf()');
	sleep 1;
	my $offset = -s $node->logfile;
	my $lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
	$node->safe_psql('postgres', 'SELECT count(*) FROM w');
	ok(fired($offset), 'a worker started and finished');
	my $wal = $node->safe_psql('postgres', "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '$lsn')");
	my ($ms) = slurp_file($node->logfile, $offset) =~ /table maintenance worker finished index "w" in (\d+) ms/;
	$node->safe_psql('postgres', 'ALTER SYSTEM RESET sift.table_auto_defrag_wal_rate; SELECT pg_reload_conf()');
	# the last step is not paid for, so only part of the time the whole WAL would need at 512 kB/s is certain
	cmp_ok($ms, '>=', 0.3 * $wal / 512 / 1024 * 1000, "the pass of $wal bytes took $ms ms at 512 kB/s");
	is($node->safe_psql('postgres', sprintf($digest, 'w')), $expected, 'the same rows');
};

# the groups next to the one a rewrite picks, when they are small, join it: one group instead of five
subtest 'a pass merges the small groups next to the one it rewrites' => sub {
	my %after;
	for my $coalesce ('on', 'off')
	{
		my $t = "co_$coalesce";
		$node->safe_psql('postgres', "ALTER SYSTEM SET sift.table_auto_defrag_coalesce = $coalesce; ALTER SYSTEM SET sift.table_auto_defrag_dead_pct = 10; SELECT pg_reload_conf()");
		$node->safe_psql('postgres', "DROP TABLE IF EXISTS $t; CREATE TABLE $t (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false)");
		for my $i (0 .. 4)
		{
			my ($lo, $hi) = ($i * 4000 + 1, $i * 4000 + 4000);
			$node->safe_psql('postgres', "SET sift.table_auto_defrag = off; INSERT INTO $t SELECT g, g * 3, 'text ' || md5(g::text) FROM generate_series($lo, $hi) g; VACUUM $t");
		}
		my $groups = "SELECT count(*) FROM sift.table_segments('$t') WHERE kind = 'columns'";
		is($node->safe_psql('postgres', $groups), '5', "$coalesce: five small groups to start from");
		$node->safe_psql('postgres', "SET sift.table_auto_defrag = off; DELETE FROM $t WHERE id <= 4000 AND id % 10 < 7; DELETE FROM $t WHERE id > 4000 AND id <= 8000 AND id % 10 < 4");
		my $expected = $node->safe_psql('postgres', sprintf($digest, $t));
		my $offset = -s $node->logfile;
		sleep 1;
		$node->safe_psql('postgres', "SELECT count(*) FROM $t");
		ok(fired($offset), "$coalesce: a worker started and finished");
		$after{$coalesce} = $node->safe_psql('postgres', $groups);
		is($node->safe_psql('postgres', sprintf($digest, $t)), $expected, "$coalesce: the same rows");
	}
	$node->safe_psql('postgres', 'ALTER SYSTEM RESET sift.table_auto_defrag_coalesce; ALTER SYSTEM RESET sift.table_auto_defrag_dead_pct; SELECT pg_reload_conf()');
	is($after{on}, '1', 'with the setting on the five groups became one');
	cmp_ok($after{off}, '>', 1, 'with it off the groups stay apart');
};

subtest 'a reader holding the table makes the worker give up the tail without waiting' => sub {
	fill('d');
	# the sessions here do not ask for a pass themselves: the one that holds the table is the call under test
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; DELETE FROM d WHERE id % 10 < 4');
	my $bg = $node->background_psql('postgres');
	$bg->query_safe('SET sift.table_auto_defrag = off; BEGIN; SET sift.enable_agg = off; DECLARE cur CURSOR FOR SELECT * FROM d; FETCH 1 FROM cur');
	my $blocks = $node->safe_psql('postgres', "SELECT relation_blocks FROM sift.table_space('d')");
	my $res = $node->safe_psql('postgres', "SET statement_timeout = '5s'; SELECT busy FROM sift.table_maintain('d')");
	is($res, 'f', 'the call returned while the cursor was open');
	$bg->query_safe('COMMIT');
	$bg->quit;
};

# A crash at each boundary of the space step leaves the old or the new directory
sub crash_fixture
{
	$node->safe_psql('postgres', "DROP TABLE IF EXISTS s; CREATE TABLE s (id int, v bigint, body text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false)");
	$node->safe_psql('postgres', "INSERT INTO s SELECT g, g * 3, repeat(md5(g::text), CASE WHEN g % 400 = 0 THEN 300 ELSE 1 END) FROM generate_series(1, 60000) g");
	$node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; UPDATE s SET v = v + 1 WHERE id % 10 = 0; DELETE FROM s WHERE id % 20 = 1');
	$node->safe_psql('postgres', 'VACUUM s');
	$node->safe_psql('postgres', 'ALTER TABLE s RESET (vacuum_truncate)');
}
for my $phase (1 .. 3)
{
	subtest "crash phase $phase of the space step of a pass" => sub {
		crash_fixture();
		# the read must not ask for a pass of its own: it would take the free blocks the call under test is to move
		my $expected = $node->safe_psql('postgres', 'SET sift.table_auto_defrag = off; SET sift.enable_agg = off; SELECT count(*), sum(id), sum(v), sum(length(body)) FROM s');
		my $offset = -s $node->logfile;
		system("$ENV{SIFT_REPO}/scripts/leakcheck.sh", 'expect', 1, 'terminated by signal 9');
		my ($out, $err) = ('', '');
		my $rc = $node->psql('postgres', "SET sift.test_space_crash = $phase; SELECT sift.table_maintain('s')", stdout => \$out, stderr => \$err);
		ok($rc != 0, 'the injected boundary killed the pass');
		ok($node->log_contains(qr/sift test: space crash phase $phase/, $offset), 'the requested boundary was reached');
		$node->wait_for_log(qr/database system is ready to accept connections/, $offset);
		is($node->safe_psql('postgres', 'SET sift.enable_agg = off; SELECT count(*), sum(id), sum(v), sum(length(body)) FROM s'), $expected, 'all rows and overflow values survived');
		is($node->safe_psql('postgres', "SELECT sift._test_check('s') >= 0"), 't', 'every referenced and free extent has one owner');
		$node->safe_psql('postgres', "SELECT sift.table_maintain('s')");
		is($node->safe_psql('postgres', "SELECT sift._test_check('s')"), '0', 'the next pass also reclaimed any crash orphans');
		is($node->safe_psql('postgres', 'SET sift.enable_agg = off; SELECT count(*), sum(id), sum(v), sum(length(body)) FROM s'), $expected, 'same rows after it');
	};
}

$node->stop;
done_testing();
