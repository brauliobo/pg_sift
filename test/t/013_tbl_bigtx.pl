# sift_table: one transaction writes more row-format pages than the directory has entries for (the limit is lowered, a testing setting).
# Its write buffers become column groups that carry their writer; a standby replays them and sees the rows at the commit only, a crash
# in the middle leaves nothing (VACUUM sweeps the groups), a commit survives a crash, and pg_dump / restore reload the table in one COPY.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = knob('SIFT_ROWS', 40000, 40000);
my $conf = "sift.table_max_segments = 16\nsift.table_write_buffer_limit = 2048\nwal_consistency_checking = 'generic'";
my $digest = 'SELECT count(*) || $$:$$ || coalesce(sum(id), 0) || $$:$$ || coalesce(md5(string_agg(id || v, $$,$$ ORDER BY id)), $$-$$) FROM %s';
my $load = "INSERT INTO %s SELECT g, repeat('x', 1000) || g FROM generate_series(1, $rows) g";
# a loop of single-row INSERTs: no command closes the write buffers as a whole
my $loop = 'DO $$ BEGIN FOR g IN 1..' . $rows . q{ LOOP INSERT INTO %s VALUES (g, repeat('x', 1000) || g); END LOOP; END $$};
my %loads = (command => $load, loop => $loop);
my $tagged = 'SELECT sift._test_tbl_tagged($$%s$$) > 0';
my $want = $rows . ':' . $rows * ($rows + 1) / 2;

sub digest { return $_[0]->safe_psql('postgres', sprintf($digest, $_[1])) }
sub count { return $_[0]->safe_psql('postgres', "SELECT count(*) FROM $_[1]") }

my $primary = sift_node('primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
install_sift_test($primary, 'the table limit is a testing setting');
# a slot keeps the WAL the standby has not replayed: with consistency checking it replays slower than the primary writes
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('big_standby')");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'big_standby'");
$standby->append_conf('postgresql.conf', $conf);
$standby->start;

subtest 'a standby replays the groups of a running transaction and sees the rows at its commit' => sub {
	$primary->safe_psql('postgres', 'CREATE TABLE r (id int, v text) USING sift_table WITH (autovacuum_enabled = false); CREATE INDEX r_id ON r (id)');
	my $bg = $primary->background_psql('postgres');
	$bg->query_safe('BEGIN');
	$bg->query_safe(sprintf($load, 'r'));
	is($bg->query_safe(sprintf($tagged, 'r')), 't', 'the transaction carries column groups');
	$primary->wait_for_catchup($standby);
	is(count($standby, 'r'), 0, 'the standby sees none of the rows while the transaction runs');
	is(count($primary, 'r'), 0, 'neither does another session of the primary');
	$bg->query_safe('COMMIT');
	$bg->quit;
	$primary->wait_for_catchup($standby);
	is(count($standby, 'r'), $rows, 'the standby sees all of them after the commit');
	is(digest($standby, 'r'), digest($primary, 'r'), 'the same rows on both');
	is($standby->safe_psql('postgres', "SET enable_seqscan = off; SELECT count(*) FROM r WHERE id > 100"), $rows - 100, 'the index scan on the standby agrees');
	$primary->safe_psql('postgres', 'VACUUM r');
	$primary->wait_for_catchup($standby);
	is(digest($standby, 'r'), digest($primary, 'r'), 'the same rows after VACUUM froze the groups');
	is($primary->safe_psql('postgres', sprintf($tagged, 'r')), 'f', 'no group carries a writer any more');
};

for my $kind (sort keys %loads) {
subtest "a rollback of a $kind load leaves nothing on either node and VACUUM returns the space" => sub {
	my ($t, $sql) = ("a_$kind", $loads{$kind});
	$primary->safe_psql('postgres', "CREATE TABLE $t (id int, v text) USING sift_table WITH (autovacuum_enabled = false)");
	$primary->safe_psql('postgres', 'BEGIN; ' . sprintf($sql, $t) . '; ROLLBACK');
	$primary->wait_for_catchup($standby);
	is(count($primary, $t) . count($standby, $t), '00', 'no rows');
	$primary->safe_psql('postgres', "VACUUM $t; VACUUM $t");
	$primary->wait_for_catchup($standby);
	is($primary->safe_psql('postgres', sprintf($tagged, $t)), 'f', 'the groups are gone');
	is($primary->safe_psql('postgres', "SELECT relation_blocks < 100 FROM sift.table_space('$t')"), 't', 'the space is back');
	$primary->safe_psql('postgres', sprintf($sql, $t));
	$primary->wait_for_catchup($standby);
	is(digest($standby, $t), digest($primary, $t), 'the table takes a new load');
};
}

for my $kind (sort keys %loads) {
subtest "a crash in the middle of a $kind load leaves nothing" => sub {
	my ($t, $sql) = ("c_$kind", $loads{$kind});
	$primary->safe_psql('postgres', "CREATE TABLE $t (id int, v text) USING sift_table WITH (autovacuum_enabled = false); INSERT INTO $t VALUES (-1, \$\$before\$\$)");
	my $bg = $primary->background_psql('postgres', on_error_stop => 0);
	my $pid = $bg->query_safe('SELECT pg_backend_pid()');
	$bg->query_safe('BEGIN');
	$bg->query_safe(sprintf($sql, $t));
	is($bg->query_safe(sprintf($tagged, $t)), 't', 'the transaction carries column groups');
	my $log_offset = -s $primary->logfile;
	SiftTest::kill_expected($pid);
	$primary->wait_for_log(qr/database system is ready to accept connections/, $log_offset);
	eval { $bg->quit };
	is(digest($primary, $t), '1:-1:' . $primary->safe_psql('postgres', "SELECT md5('-1before')"), 'only the row from before is there');
	$primary->safe_psql('postgres', "VACUUM $t; VACUUM $t");
	is($primary->safe_psql('postgres', sprintf($tagged, $t)), 'f', 'VACUUM swept the groups of the dead transaction');
	is(count($primary, $t), 1, 'still one row');
	$primary->safe_psql('postgres', "DELETE FROM $t; " . sprintf($sql, $t));
	is(count($primary, $t), $rows, 'a new load goes through');
	$primary->wait_for_catchup($standby);
	is(digest($standby, $t), digest($primary, $t), 'the standby agrees after the crash and the new load');
};
}

subtest 'a commit survives an immediate stop' => sub {
	$primary->safe_psql('postgres', 'CREATE TABLE d (id int, v text) USING sift_table WITH (autovacuum_enabled = false)');
	$primary->safe_psql('postgres', 'BEGIN; ' . sprintf($load, 'd') . '; COMMIT');
	my $before = digest($primary, 'd');
	is($before =~ /^\Q$want\E:/ ? 1 : 0, 1, "the rows are $want");
	$primary->stop('immediate');
	$primary->start;
	is(digest($primary, 'd'), $before, 'the same rows after crash recovery');
	is($primary->safe_psql('postgres', sprintf($tagged, 'd')), 't', 'the groups still carry their writer until VACUUM');
	$primary->safe_psql('postgres', 'VACUUM d');
	is(digest($primary, 'd'), $before, 'and after VACUUM');
};

subtest 'promoting the standby keeps the rows' => sub {
	$primary->wait_for_catchup($standby);
	my $before = digest($standby, 'd');
	$standby->promote;
	is(digest($standby, 'd'), $before, 'same rows after promotion');
	$standby->safe_psql('postgres', 'VACUUM d');
	is(digest($standby, 'd'), $before, 'and after VACUUM on the new primary');
};
$standby->stop;

subtest 'pg_dump and restore reload the table in one transaction' => sub { check_dump_restore($primary, $conf, \&digest, 'd') };

$primary->stop;
done_testing();
