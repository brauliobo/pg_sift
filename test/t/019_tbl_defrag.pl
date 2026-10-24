# The background pass of a sift_table (sift.table_auto_defrag) is safe to leave on: next to a heap twin that gets the same writes, a table
# that is cleaned, merged and shortened by workers while it is written and read returns the same rows at every moment; a standby replays
# every page the passes write and the primary compares each with its image (wal_consistency_checking = generic); a worker killed in the
# middle of a pass (SIGKILL, with the cost delay holding it there) leaves the table as it was or as the pass would have left it, and the
# next pass and VACUUM finish the job.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = knob('SIFT_ROWS', 20000, 6000);
my $worker = qr/table maintenance worker finished/;

my $primary = sift_node('defrag_primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', "autovacuum = off\nwal_consistency_checking = 'generic'\nsift.table_auto_defrag = on\n"
	. "sift.maintenance_naptime = 200ms\nsift.log_min_duration = 0\nsift.table_write_buffer_limit = 2048\nmax_parallel_maintenance_workers = 0\n"
	. "max_parallel_workers_per_gather = 0\nshared_buffers = 64MB");
$primary->start;
install_sift($primary);
my $testing = $primary->safe_psql('postgres', "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift_test'");
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test') if $testing;
# a slot keeps the WAL the standby has not replayed yet: with every page of it checked, the primary writes it faster than a checkpoint interval
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('defrag_standby')");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('defrag_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'defrag_standby'");
$standby->start;

# the rows of a table, in order: the same text for the twin that is a heap and the table that is not
my $digest = q{SELECT count(*) || $$:$$ || coalesce(md5(string_agg(id || $$/$$ || v || $$/$$ || kind || $$/$$ || pad, $$,$$ ORDER BY id)), $$-$$) FROM %s};
sub digest
{
	my ($node, $t) = @_;
	return $node->safe_psql('postgres', sprintf($digest, $t));
}

# one write to the table and to its twin; the session never asks for a pass itself (the reads that follow do)
sub both
{
	my ($t, $sql) = @_;
	for my $name ($t, "${t}_h")
	{
		(my $stmt = $sql) =~ s/\bTBL\b/$name/g;
		$primary->safe_psql('postgres', "SET sift.table_auto_defrag = off; $stmt");
	}
}

sub twins
{
	my ($t) = @_;
	$primary->safe_psql('postgres', qq{
		CREATE TABLE $t (id int, v bigint, kind int, pad text) USING sift_table WITH (autovacuum_enabled = false);
		CREATE TABLE ${t}_h (LIKE $t) WITH (autovacuum_enabled = false);
		CREATE INDEX ${t}_id ON $t (id);
	});
	both($t, "INSERT INTO TBL SELECT g, g * 3, g % 7, 'pad ' || md5(g::text) || md5((g + 1)::text) FROM generate_series(1, $rows) g");
	$primary->safe_psql('postgres', "VACUUM $t");
}

sub same
{
	my ($t, $what) = @_;
	is(digest($primary, $t), digest($primary, "${t}_h"), "$what: the table and its heap twin agree");
}

subtest 'the answers equal a heap twin before, during and after the passes' => sub {
	twins('a');
	my $offset = -s $primary->logfile;
	same('a', 'at the start');
	# scattered and clustered deletes, updates and inserts, then reads (which ask for the passes): several rounds
	for my $round (1 .. 4)
	{
		both('a', "DELETE FROM TBL WHERE id % 10 < 4 AND id % 4 = " . ($round - 1));
		both('a', "UPDATE TBL SET v = v + $round WHERE id BETWEEN " . ($round * 1000) . " AND " . ($round * 1000 + 700));
		both('a', "INSERT INTO TBL SELECT g, g, 9, 'new' FROM generate_series(" . ($rows + $round * 500) . ", " . ($rows + $round * 500 + 400) . ") g");
		sleep 1;
		$primary->safe_psql('postgres', 'SELECT count(*) FROM a');
		same('a', "round $round");
	}
	ok($primary->wait_for_log($worker, $offset), 'workers ran');
	$primary->poll_query_until('postgres', "SELECT sum(dead_rows) * 100 < sum(rows) * 30 FROM sift.table_segments('a') WHERE kind = 'columns'");
	same('a', 'after the passes');
	is($primary->safe_psql('postgres', "SELECT bt_index_check('a_id', true)"), '', 'the btree index is consistent with the table');
	$primary->safe_psql('postgres', 'VACUUM a');
	same('a', 'after VACUUM');
	is($primary->safe_psql('postgres', "SELECT bt_index_check('a_id', true)"), '', 'the btree index is consistent after VACUUM');
};

subtest 'a standby replays what the passes write' => sub {
	$primary->wait_for_catchup($standby);
	is(digest($standby, 'a'), digest($primary, 'a'), 'the standby returns the primary rows');
	is(digest($standby, 'a'), digest($primary, 'a_h'), 'and those of the heap twin');
	ok(!$standby->log_contains(qr/inconsistent page|PANIC/), 'every page agrees with the image the primary logged');
};

subtest 'a worker killed in the middle of a pass' => sub {
	plan skip_all => 'the extent check needs the testing build' unless $testing;
	# the cost delay holds the worker in the middle of the pass: the smallest cost limit makes every point of the pass a sleep of four delays
	$primary->safe_psql('postgres', "ALTER SYSTEM SET sift.table_auto_defrag_delay = 100; ALTER SYSTEM SET vacuum_cost_limit = 1; SELECT pg_reload_conf()");
	twins('k');
	both('k', 'DELETE FROM TBL WHERE id % 10 < 5');
	both('k', 'UPDATE TBL SET v = v + 1 WHERE id % 10 = 7');
	same('k', 'before the pass');
	my $before = digest($primary, 'k');
	sleep 1;
	my $offset = -s $primary->logfile;
	$primary->safe_psql('postgres', 'SELECT count(*) FROM k');
	ok($primary->poll_query_until('postgres', "SELECT count(*) > 0 FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND wait_event = 'VacuumDelay'"),
		'a worker is in its pass, waiting for the cost delay');
	my $pid = $primary->safe_psql('postgres', "SELECT pid FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND wait_event = 'VacuumDelay' LIMIT 1");
	# a write appends a tombstone and the pass holds the table's worker lock for its whole length: the two must not wait for each other
	both('k', "SET lock_timeout = '1s'; UPDATE TBL SET v = v + 5 WHERE id = 3; DELETE FROM TBL WHERE id = 5");
	$before = digest($primary, 'k');
	same('k', 'with the writes made during the pass');
	kill_expected($pid);
	ok($primary->wait_for_log(qr/database system is ready to accept connections/, $offset), 'the server recovered');
	$primary->safe_psql('postgres', "ALTER SYSTEM RESET sift.table_auto_defrag_delay; ALTER SYSTEM RESET vacuum_cost_limit; SELECT pg_reload_conf()");
	is(digest($primary, 'k'), $before, 'the rows are those of before the pass');
	same('k', 'after the crash');
	is($primary->safe_psql('postgres', "SELECT sift._test_check('k') >= 0"), 't', 'every extent has one owner');
	$primary->safe_psql('postgres', "SET sift.table_auto_defrag = off; SELECT * FROM sift.table_maintain('k')");
	same('k', 'after the next pass');
	$primary->safe_psql('postgres', 'VACUUM k');
	same('k', 'after VACUUM');
	is($primary->safe_psql('postgres', "SELECT sift._test_check('k')"), '0', 'no extent is left over');
	is($primary->safe_psql('postgres', "SELECT bt_index_check('k_id', true)"), '', 'the btree index is consistent with the table');
	$primary->wait_for_catchup($standby);
	is(digest($standby, 'k'), digest($primary, 'k'), 'the standby follows the crash and the passes after it');
	ok(!$standby->log_contains(qr/inconsistent page|PANIC/), 'every page still agrees with its image');
};

done_testing();
