# sift_table written by loops of single-row INSERTs and by several writers at once (the directory limit is lowered, a testing setting):
# the write buffers are promoted with the version of every row, whoever wrote it. A standby replays the groups and sees the rows at the
# commit only, a repeatable read snapshot of the standby survives a freeze on the primary, a crash in the middle of a loop leaves nothing
# (VACUUM sweeps the groups), a commit survives a crash, pg_dump / restore reload the table in one COPY, writers that commit and roll back
# next to parallel COPYs agree with a heap twin written by the same transactions at every moment, and wal_level = minimal loses nothing.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use IPC::Run qw(start finish);
use Time::HiRes qw(usleep);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = knob('SIFT_ROWS', 40000, 40000);
my $batch = knob('SIFT_BATCH', 5000, 4000);
my $txns = knob('SIFT_TXNS', 16, 8);
my $copy_rows = knob('SIFT_COPY_ROWS', 30000, 20000);
my $clients = 8;
my $limit = knob('SIFT_MAX_SEGMENTS', 16, 16);
my $conf = "sift.table_max_segments = $limit\nsift.table_write_buffer_limit = 2048";
my $primary_conf = "$conf\nwal_consistency_checking = 'generic'";
# order independent, so that the rows (a kilobyte each) need no sort
my $digest = 'SELECT count(*) || $$:$$ || coalesce(sum(id), 0) || $$:$$ || coalesce(sum(hashtextextended(id || v, 0)::numeric), 0) FROM %s';
# rows lo..hi, one INSERT of one row per command
my $loop = q{DO $$ BEGIN FOR i IN %d..%d LOOP INSERT INTO %s VALUES (i, repeat('x', 1000) || i); END LOOP; END $$};
my $unfrozen = 'SELECT sift._test_tbl_tagged($$%s$$) > 0';
my $has_columns = q{(SELECT count(*) FILTER (WHERE kind = 'columns') > 0 FROM sift.table_segments($$%s$$))};
my $promoted = "SELECT $has_columns";
my $entries = 'SELECT count(*) FROM sift.table_segments($$%s$$)';

sub digest { return $_[0]->safe_psql('postgres', sprintf($digest, $_[1])) }
sub count { return $_[0]->safe_psql('postgres', "SELECT count(*) FROM $_[1]") }
sub loop_sql { my ($t, $lo, $hi) = @_; return sprintf($loop, $lo, $hi, $t) }

# a heap table with the same rows as the sift_table t, loaded by one statement
sub twin
{
	my ($node, $t, $lo, $hi) = @_;
	$node->safe_psql('postgres', "CREATE TABLE ${t}_h (id int, v text) WITH (autovacuum_enabled = false)") unless count_tables($node, "${t}_h");
	$node->safe_psql('postgres', "INSERT INTO ${t}_h SELECT g, repeat('x', 1000) || g FROM generate_series($lo, $hi) g");
}

# VACUUM (FREEZE) until no group of t carries a writer: the horizon follows the feedback of the standby, which takes a moment
sub freeze_all
{
	my ($node, $t) = @_;
	for (1 .. 120)
	{
		$node->safe_psql('postgres', "VACUUM (FREEZE) $t");
		return 1 if $node->safe_psql('postgres', sprintf($unfrozen, $t)) eq 'f';
		usleep(500_000);
	}
	return 0;
}

# VACUUM until the file of t is shorter than blocks: the space of a dropped group is reusable once another transaction has passed
sub shrink_all
{
	my ($node, $t, $blocks) = @_;
	for (1 .. 120)
	{
		$node->safe_psql('postgres', "VACUUM $t; SELECT pg_current_xact_id() IS NOT NULL");
		return 1 if $node->safe_psql('postgres', "SELECT relation_blocks < $blocks FROM sift.table_space('$t')") eq 't';
		usleep(500_000);
	}
	return 0;
}

sub count_tables { return $_[0]->safe_psql('postgres', "SELECT count(*) FROM pg_class WHERE relname = '$_[1]'") }

# the same answers through a sequential scan and the btree
sub agree_index
{
	my ($node, $t, $what) = @_;
	my $seq = $node->safe_psql('postgres', "SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*) || ':' || coalesce(sum(id), 0) FROM $t WHERE id % 7 = 3 AND id > 100");
	my $idx = $node->safe_psql('postgres', "SET enable_seqscan = off; SELECT count(*) || ':' || coalesce(sum(id), 0) FROM $t WHERE id % 7 = 3 AND id > 100");
	is($idx, $seq, "$what: btree and sequential scan agree");
}

my $primary = sift_node('primary', allows_streaming => 1);
$primary->append_conf('postgresql.conf', $primary_conf);
$primary->start;
plan skip_all => 'the table limit is a testing setting' unless $primary->safe_psql('postgres', "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift_test'");
install_sift($primary);
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');
# a slot keeps the WAL the standby has not replayed: with consistency checking it replays slower than the primary writes
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('wp_standby')");
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'wp_standby'\nhot_standby_feedback = on\nwal_receiver_status_interval = 1");
$standby->append_conf('postgresql.conf', $primary_conf);
$standby->start;

subtest 'a standby replays a loop of single-row INSERTs and sees the rows at the commit only' => sub {
	# vacuum_truncate = false: a VACUUM that moves or cuts the file replays an exclusive lock, which cancels the standby's snapshot (as a
	# heap's truncation does); the snapshot of the third subtest is held across the VACUUM
	$primary->safe_psql('postgres', 'CREATE TABLE r (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false)');
	my $bg = $primary->background_psql('postgres');
	$bg->query_safe('BEGIN');
	$bg->query_safe(loop_sql('r', 1, $rows));
	is($bg->query_safe(sprintf($promoted, 'r')), 't', 'the write buffers were promoted while the loop ran');
	is($bg->query_safe(sprintf($unfrozen, 'r')), 't', 'the groups carry the versions of their rows');
	cmp_ok($primary->safe_psql('postgres', sprintf($entries, 'r')), '<=', $limit, 'the directory stays within its limit');
	$primary->wait_for_catchup($standby);
	is(count($standby, 'r'), 0, 'the standby sees none of the rows while the transaction runs');
	is(count($primary, 'r'), 0, 'neither does another session of the primary');
	$bg->query_safe('COMMIT');
	$bg->quit;
	twin($primary, 'r', 1, $rows);
	$primary->wait_for_catchup($standby);
	is(count($standby, 'r'), $rows, 'the standby sees all of them after the commit');
	is(digest($standby, 'r'), digest($primary, 'r_h'), 'the same rows as the heap twin');
	is($standby->safe_psql('postgres', "SET enable_seqscan = off; SELECT count(*) FROM r WHERE id > 100"), $rows - 100, 'the index scan on the standby agrees');
};

subtest 'a repeatable read snapshot of the standby survives a freeze on the primary' => sub {
	my $before = digest($standby, 'r');
	my $sbg = $standby->background_psql('postgres');
	$sbg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	is($sbg->query_safe('SELECT count(*) FROM r'), $rows, 'the snapshot sees the first load');
	# a second loop, an aborted third one, a freeze and a delete
	$primary->safe_psql('postgres', loop_sql('r', $rows + 1, 2 * $rows));
	$primary->safe_psql('postgres', 'BEGIN; ' . loop_sql('r', 2 * $rows + 1, 3 * $rows) . '; ROLLBACK');
	$primary->safe_psql('postgres', 'DELETE FROM r WHERE id % 10 = 0; VACUUM (FREEZE) r');
	$primary->wait_for_catchup($standby);
	is($sbg->query_safe('SELECT count(*) FROM r'), $rows, 'the snapshot still sees the first load');
	is($sbg->query_safe(sprintf($digest, 'r')), $before, 'the same rows, byte for byte');
	is($sbg->query_safe('SET enable_seqscan = off; SELECT count(*) FROM r WHERE id > 100'), $rows - 100, 'and through the index');
	$sbg->query_safe('COMMIT');
	$sbg->quit;
	is(count($standby, 'r'), 2 * $rows - $rows / 10 * 2, 'a new snapshot sees the second load and not the deleted rows');
	is(digest($standby, 'r'), digest($primary, 'r'), 'the same rows on both');
	ok(freeze_all($primary, 'r'), 'no group carries a writer any more');
	is(count($primary, 'r'), 2 * $rows - $rows / 10 * 2, 'the aborted loop left nothing');
	$primary->safe_psql('postgres', 'DROP TABLE r, r_h');
};

subtest 'a crash in the middle of a loop leaves nothing' => sub {
	$primary->safe_psql('postgres', 'CREATE TABLE c (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false); INSERT INTO c VALUES (-1, $$before$$)');
	my $bg = $primary->background_psql('postgres', on_error_stop => 0);
	my $pid = $bg->query_safe('SELECT pg_backend_pid()');
	$bg->query_until(qr/started/, "\\echo started\nBEGIN;\n" . loop_sql('c', 1, 2 * $rows) . ";\n");
	# until promoted rows exist, or the loop ended (with an error)
	$primary->poll_query_until('postgres', 'SELECT ' . sprintf($has_columns, 'c') . " OR NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $pid AND state = 'active')");
	is($primary->safe_psql('postgres', sprintf($promoted, 'c')), 't', 'the write buffers were promoted while the loop ran');
	my $log_offset = -s $primary->logfile;
	SiftTest::kill_expected($pid);
	$primary->wait_for_log(qr/database system is ready to accept connections/, $log_offset);
	eval { $bg->quit };
	is(digest($primary, 'c'), '1:-1:' . $primary->safe_psql('postgres', "SELECT hashtextextended('-1before', 0)::numeric"), 'only the row from before is there');
	$primary->safe_psql('postgres', 'VACUUM c; VACUUM c');
	ok(freeze_all($primary, 'c'), 'VACUUM swept the groups of the dead transaction');
	is(count($primary, 'c'), 1, 'still one row');
	$primary->safe_psql('postgres', 'SELECT bt_index_check(index => $$c_pkey$$)');
	$primary->safe_psql('postgres', 'DELETE FROM c; BEGIN; ' . loop_sql('c', 1, $rows) . '; COMMIT');
	twin($primary, 'c', 1, $rows);
	is(digest($primary, 'c'), digest($primary, 'c_h'), 'a new loop goes through and equals its heap twin');
	agree_index($primary, 'c', 'after the crash');
	$primary->safe_psql('postgres', 'DROP TABLE c, c_h');
};

subtest 'a crash with the loop done and the transaction open leaves nothing' => sub {
	$primary->safe_psql('postgres', 'CREATE TABLE o (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false); INSERT INTO o VALUES (-1, $$before$$)');
	my $bg = $primary->background_psql('postgres', on_error_stop => 0);
	my $pid = $bg->query_safe('SELECT pg_backend_pid()');
	$bg->query_until(qr/started/, "\\echo started\nBEGIN;\n" . loop_sql('o', 1, $rows) . ";\nSELECT pg_sleep(600);\n");
	$primary->poll_query_until('postgres', "SELECT wait_event = 'PgSleep' OR state <> 'active' FROM pg_stat_activity WHERE pid = $pid");
	is($primary->safe_psql('postgres', sprintf($unfrozen, 'o')), 't', 'the transaction carries groups');
	my $log_offset = -s $primary->logfile;
	SiftTest::kill_expected($pid);
	$primary->wait_for_log(qr/database system is ready to accept connections/, $log_offset);
	eval { $bg->quit };
	is(count($primary, 'o'), 1, 'only the row from before is there');
	$primary->safe_psql('postgres', 'VACUUM o; VACUUM o');
	ok(freeze_all($primary, 'o'), 'VACUUM swept the groups');
	ok(shrink_all($primary, 'o', 1000), 'the space is back');
	$primary->safe_psql('postgres', 'SELECT bt_index_check(index => $$o_pkey$$)');
	$primary->wait_for_catchup($standby);
	is(count($standby, 'o'), 1, 'the standby agrees after the crash');
	$primary->safe_psql('postgres', 'DROP TABLE o');
};

subtest 'a commit survives an immediate stop' => sub {
	$primary->safe_psql('postgres', 'CREATE TABLE d (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false)');
	$primary->safe_psql('postgres', 'BEGIN; ' . loop_sql('d', 1, $rows) . '; COMMIT');
	twin($primary, 'd', 1, $rows);
	my $before = digest($primary, 'd');
	is($before, digest($primary, 'd_h'), "the $rows rows are the heap twin's");
	$primary->stop('immediate');
	$primary->start;
	is(digest($primary, 'd'), $before, 'the same rows after crash recovery');
	agree_index($primary, 'd', 'after crash recovery');
	$primary->safe_psql('postgres', 'VACUUM d');
	is(digest($primary, 'd'), $before, 'and after VACUUM');
	$primary->safe_psql('postgres', 'SELECT bt_index_check(index => $$d_pkey$$)');
	$primary->stop('immediate');
	$primary->start;
	is(digest($primary, 'd'), $before, 'and after a second crash');
};

subtest 'promoting the standby keeps the rows' => sub {
	$primary->wait_for_catchup($standby);
	my $before = digest($standby, 'd');
	is($before, digest($primary, 'd_h'), 'the standby has the committed loop');
	$standby->promote;
	is(digest($standby, 'd'), $before, 'same rows after promotion');
	$standby->safe_psql('postgres', 'VACUUM d');
	is(digest($standby, 'd'), $before, 'and after VACUUM on the new primary');
	$standby->safe_psql('postgres', 'BEGIN; ' . loop_sql('d', $rows + 1, 2 * $rows) . '; COMMIT');
	is(count($standby, 'd'), 2 * $rows, 'the new primary takes a loop');
};
$standby->stop;

subtest 'pg_dump and restore reload the table in one transaction' => sub {
	my $want = digest($primary, 'd');
	my $file = $primary->basedir . '/dump.sql';
	$primary->command_ok([ 'pg_dump', '-p', $primary->port, '-h', $primary->host, '-d', 'postgres', '-t', 'd', '-f', $file ], 'pg_dump');
	my $dst = sift_node('dump_dst');
	$dst->append_conf('postgresql.conf', $conf);
	$dst->start;
	install_sift($dst);
	$dst->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');
	$dst->command_ok([ 'psql', '-X', '-q', '-1', '-v', 'ON_ERROR_STOP=1', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', '-f', $file ], 'restore in one transaction');
	is(digest($dst, 'd'), $want, 'the restored rows are identical');
	$dst->safe_psql('postgres', 'VACUUM d');
	is(digest($dst, 'd'), $want, 'and after VACUUM');
	$dst->stop;
	$dst->clean_node;
};
$primary->stop;

# ---------- writers that commit and roll back next to parallel COPYs ----------
# Every transaction writes the sift_table s and its heap twin h, so that at any snapshot they hold the same rows: those of the transactions
# that committed. The readers compare them in a repeatable read transaction while the writers run, and the directory is sampled.
subtest 'concurrent writers, readers and COPYs agree with a heap twin' => sub {
	my $node = sift_node('conc');
	$node->append_conf('postgresql.conf', "$conf\nmax_connections = 40");
	$node->start;
	install_sift($node);
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');
	$node->safe_psql('postgres', q{
		CREATE TABLE s (id bigint, batch bigint, v text) USING sift_table WITH (autovacuum_enabled = false);
		CREATE TABLE h (id bigint, batch bigint, v text) WITH (autovacuum_enabled = false);
		CREATE INDEX s_id ON s (id);
		CREATE INDEX h_id ON h (id);
		CREATE TABLE committed (batch bigint);
		CREATE TABLE samples (n int);
		INSERT INTO s SELECT g, 0, $$before$$ FROM generate_series(1, 10) g;
		INSERT INTO h SELECT g, 0, $$before$$ FROM generate_series(1, 10) g;
	});
	my $dir = PostgreSQL::Test::Utils::tempdir;
	open(my $f, '>', "$dir/writer.sql") or die;
	print $f q{\set batch random(1, 2000000000)
\set base :batch * 100000
\set commit random(0, 1)
BEGIN;
DO $$ BEGIN FOR i IN 1..:n LOOP INSERT INTO s VALUES (:base + i, :batch, repeat('x', 1000) || i); INSERT INTO h VALUES (:base + i, :batch, repeat('x', 1000) || i); END LOOP; END $$;
\if :commit = 1
INSERT INTO committed VALUES (:batch);
COMMIT;
\else
ROLLBACK;
\endif
};
	close $f;
	# in one snapshot: the twins are equal, each batch is whole, and every row belongs to a batch that committed
	open($f, '>', "$dir/reader.sql") or die;
	print $f q{BEGIN ISOLATION LEVEL REPEATABLE READ;
DO $$ BEGIN
	IF (SELECT count(*) || ':' || coalesce(sum(id), 0) || ':' || coalesce(sum(hashtextextended(id || v, 0)::numeric), 0) FROM s)
		IS DISTINCT FROM (SELECT count(*) || ':' || coalesce(sum(id), 0) || ':' || coalesce(sum(hashtextextended(id || v, 0)::numeric), 0) FROM h) THEN
		RAISE EXCEPTION 'the sift_table and its heap twin differ';
	END IF;
	IF EXISTS (SELECT 1 FROM s GROUP BY batch HAVING count(*) <> CASE WHEN batch = 0 THEN 10 WHEN batch < 0 THEN :copy_rows ELSE :n END) THEN
		RAISE EXCEPTION 'a batch is not whole';
	END IF;
	IF EXISTS (SELECT batch FROM s WHERE batch > 0 EXCEPT SELECT batch FROM committed) THEN
		RAISE EXCEPTION 'rows of a transaction that did not commit';
	END IF;
END $$;
COMMIT;
};
	close $f;
	open($f, '>', "$dir/sampler.sql") or die;
	print $f "INSERT INTO samples SELECT count(*) FROM sift.table_segments('s');\n";
	close $f;
	open($f, '>', "$dir/gen.awk") or die;
	print $f q{BEGIN { s = sprintf("%1000s", ""); gsub(/ /, "x", s); for (i = 1; i <= n; i++) printf "%d\t%d\t%s%d\n", base + i, batch, s, i }};
	close $f;
	my @copies;
	for my $b (-1, -2)
	{
		open($f, '>', "$dir/copy$b.sql") or die;
		print $f "INSERT INTO committed VALUES ($b);\n";
		for my $t ('s', 'h')
		{
			print $f "COPY $t FROM PROGRAM 'awk -v n=$copy_rows -v base=" . ($b * 10000000) . " -v batch=$b -f $dir/gen.awk';\n";
		}
		close $f;
		my ($out, $err) = ('', '');
		push @copies, [ start([ 'psql', '-X', '-q', '-1', '-v', 'ON_ERROR_STOP=1', '-p', $node->port, '-h', $node->host, '-d', 'postgres', '-f', "$dir/copy$b.sql" ], '>', \$out, '2>', \$err), \$err ];
	}
	$node->command_ok([ 'pgbench', '-n', '-h', $node->host, '-p', $node->port, '-c', $clients, '-j', $clients, '-t', $txns, '-D', "n=$batch", '-D', "copy_rows=$copy_rows",
			'-f', "$dir/writer.sql\@3", '-f', "$dir/reader.sql\@3", '-f', "$dir/sampler.sql\@2", 'postgres' ], 'writers, readers and a sampler run side by side with two COPYs');
	for my $c (@copies)
	{
		finish($c->[0]);
		is($c->[0]->result(0), 0, 'a COPY completed') or diag(${ $c->[1] });
	}
	my $batches = $node->safe_psql('postgres', 'SELECT count(*) FROM committed WHERE batch > 0');
	cmp_ok($batches, '>', 0, "$batches writer transactions committed");
	is(count($node, 's'), 10 + 2 * $copy_rows + $batches * $batch, 'the table has the rows of the transactions that committed, the COPYs and no others');
	is(digest($node, 's'), digest($node, 'h'), 'the same rows as the heap twin');
	cmp_ok($node->safe_psql('postgres', 'SELECT count(*) FROM samples'), '>', 0, 'the directory was sampled');
	cmp_ok($node->safe_psql('postgres', 'SELECT max(n) FROM samples'), '<=', $limit, 'the directory never went above its limit');
	agree_index($node, 's', 'after the run');
	$node->safe_psql('postgres', 'VACUUM s; VACUUM s');
	is(digest($node, 's'), digest($node, 'h'), 'the same rows after VACUUM');
	is($node->safe_psql('postgres', sprintf($unfrozen, 's')), 'f', 'no group carries a writer any more');
	cmp_ok($node->safe_psql('postgres', sprintf($entries, 's')), '<=', $limit, 'the directory is within its limit');
	agree_index($node, 's', 'after VACUUM');
	$node->stop('immediate');
	$node->start;
	is(digest($node, 's'), digest($node, 'h'), 'the same rows after a crash');
	$node->stop;
	$node->clean_node;
};

# ---------- wal_level = minimal ----------
subtest 'a loop in the transaction that created the table survives a crash with wal_level = minimal' => sub {
	my $node = sift_node('minimal');
	$node->append_conf('postgresql.conf', "$conf\nwal_level = minimal\nmax_wal_senders = 0");
	$node->start;
	install_sift($node);
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');
	for my $threshold ('0', '1GB')
	{
		my $t = 'min_' . ($threshold eq '0' ? 'sync' : 'wal');
		$node->safe_psql('postgres', "SET wal_skip_threshold = '$threshold'; BEGIN; CREATE TABLE $t (id int PRIMARY KEY, v text) USING sift_table; " . loop_sql($t, 1, $rows) . '; COMMIT');
		twin($node, $t, 1, $rows);
		my $want = digest($node, "${t}_h");
		is(digest($node, $t), $want, "$t: the rows are the heap twin's");
		$node->stop('immediate');
		$node->start;
		is(digest($node, $t), $want, "$t: the same rows after crash recovery");
		agree_index($node, $t, "$t: after crash recovery");
		$node->safe_psql('postgres', "VACUUM $t");
		is(digest($node, $t), $want, "$t: and after VACUUM");
		$node->safe_psql('postgres', "SET wal_skip_threshold = '$threshold'; BEGIN; CREATE TABLE ${t}_rb (id int, v text) USING sift_table; " . loop_sql("${t}_rb", 1, $rows) . '; ROLLBACK');
		$node->stop('immediate');
		$node->start;
		is(count_tables($node, "${t}_rb"), 0, "$t: a rolled back table is gone after a crash");
	}
	$node->stop;
	$node->clean_node;
};

done_testing();
