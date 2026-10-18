# sift_table: crash recovery (committed work survives, aborted and in-flight work does not, indexes stay consistent),
# a hot standby replaying every kind of write, wal_level = minimal, and a pg_dump / pg_restore round trip.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $rows = knob('SIFT_ROWS', 20000, 3000);

# row count and a digest of every column, the same query on every node
sub digest
{
	my ($node, $t, $db) = @_;
	return $node->safe_psql($db // 'postgres',
		"SELECT count(*) || ':' || coalesce(md5(string_agg(id || '/' || v || '/' || coalesce(note, '-'), ',' ORDER BY id)), '') FROM $t");
}

# the same answers through a sequential scan and through the btree
sub agree_index
{
	my ($node, $t, $what) = @_;
	my $seq = $node->safe_psql('postgres',
		"SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*) || ':' || coalesce(sum(id), 0) FROM $t WHERE id % 7 = 3 AND id > 100");
	my $idx = $node->safe_psql('postgres',
		"SET enable_seqscan = off; SELECT count(*) || ':' || coalesce(sum(id), 0) FROM $t WHERE id % 7 = 3 AND id > 100");
	is($idx, $seq, "$what: btree and sequential scan agree");
}

sub workload
{
	my ($node, $t) = @_;
	$node->safe_psql('postgres', qq{
		CREATE TABLE $t (id int PRIMARY KEY, v int, note text) USING sift_table;
		INSERT INTO $t SELECT g, g % 97, 'row ' || g FROM generate_series(1, $rows) g;
		UPDATE $t SET v = v + 1000, note = 'updated' WHERE id % 13 = 0;
		DELETE FROM $t WHERE id % 11 = 0;
		INSERT INTO $t SELECT g, 5, repeat('w', 5000) FROM generate_series($rows + 1, $rows + 50) g;
		VACUUM $t;
		ANALYZE $t;
	});
}

# ---------- crash recovery ----------
my $node = sift_node('crash');
$node->start;
install_sift($node);

subtest 'committed work survives an immediate shutdown' => sub {
	workload($node, 'c1');
	my $before = digest($node, 'c1');
	$node->stop('immediate');
	$node->start;
	is(digest($node, 'c1'), $before, 'every row and column is back');
	agree_index($node, 'c1', 'after recovery');
};

subtest 'aborted and in-flight transactions leave nothing' => sub {
	my $before = digest($node, 'c1');
	$node->safe_psql('postgres', 'BEGIN; INSERT INTO c1 VALUES (900001, 1, $$x$$); UPDATE c1 SET v = -1 WHERE id < 50; ROLLBACK');
	my $bg = $node->background_psql('postgres');
	$bg->query_safe("BEGIN; INSERT INTO c1 SELECT g, 1, 'inflight' FROM generate_series(900100, 900400) g; DELETE FROM c1 WHERE id < 50");
	$node->stop('immediate');
	$bg->quit;
	$node->start;
	is(digest($node, 'c1'), $before, 'rolled back and in-flight changes are gone');
	agree_index($node, 'c1', 'after in-flight recovery');
};

subtest 'checkpoint in the middle of a workload' => sub {
	$node->safe_psql('postgres', 'CREATE TABLE c2 (id int PRIMARY KEY, v int, note text) USING sift_table');
	$node->safe_psql('postgres', "INSERT INTO c2 SELECT g, g, 'a' FROM generate_series(1, $rows) g");
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->safe_psql('postgres', "UPDATE c2 SET note = 'b' WHERE id % 3 = 0; DELETE FROM c2 WHERE id % 5 = 0; TRUNCATE c2; INSERT INTO c2 SELECT g, g, 'c' FROM generate_series(1, 500) g");
	my $before = digest($node, 'c2');
	$node->stop('immediate');
	$node->start;
	is(digest($node, 'c2'), $before, 'truncate and later inserts are replayed in order');
	agree_index($node, 'c2', 'after checkpoint recovery');
};

subtest 'a bm25 index and a table rewritten by VACUUM FULL recover' => sub {
	$node->safe_psql('postgres', "CREATE TABLE c3 (id int, body text) USING sift_table; INSERT INTO c3 SELECT g, 'order ' || g || ' express' FROM generate_series(1, 2000) g;"
		. " CREATE INDEX c3_idx ON c3 USING sift (body); DELETE FROM c3 WHERE id % 4 = 0; VACUUM (FULL) c3");
	my $q = "SET enable_seqscan = off; SELECT count(*) FROM c3 WHERE body \@\@> 'express'";
	my $before = $node->safe_psql('postgres', $q);
	$node->stop('immediate');
	$node->start;
	is($node->safe_psql('postgres', $q), $before, 'index answers as before');
	is($node->safe_psql('postgres', "SELECT count(*) FROM c3"), $before, 'index and table agree');
	$node->safe_psql('postgres', "SELECT sift.verify('c3_idx', true)");
};
$node->stop;

# ---------- hot standby ----------
my $primary = sift_node('primary', allows_streaming => 1);
$primary->start;
install_sift($primary);
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "hot_standby_feedback = on\nmax_standby_streaming_delay = 5s");
$standby->start;

subtest 'a hot standby replays every kind of write' => sub {
	workload($primary, 's1');
	$primary->safe_psql('postgres', "CREATE INDEX s1_v ON s1 (v); INSERT INTO s1 VALUES (-1, 0, repeat('z', 40000))");
	$primary->safe_psql('postgres', 'BEGIN; DELETE FROM s1 WHERE id < 100; ROLLBACK');
	$primary->wait_for_catchup($standby);
	is(digest($standby, 's1'), digest($primary, 's1'), 'the standby returns the primary rows');
	agree_index($standby, 's1', 'standby');

	$primary->safe_psql('postgres', 'TRUNCATE s1; INSERT INTO s1 SELECT g, 1, $$t$$ FROM generate_series(1, 300) g; VACUUM FULL s1');
	$primary->wait_for_catchup($standby);
	is(digest($standby, 's1'), digest($primary, 's1'), 'truncate and rewrite are replayed');
};

subtest 'a standby snapshot is stable while the primary vacuums' => sub {
	$primary->safe_psql('postgres', "CREATE TABLE s2 (id int PRIMARY KEY, v int, note text) USING sift_table; INSERT INTO s2 SELECT g, g, 'x' FROM generate_series(1, 5000) g");
	$primary->wait_for_catchup($standby);
	my $bg = $standby->background_psql('postgres');
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	my $count = $bg->query_safe('SELECT count(*) FROM s2');
	$primary->safe_psql('postgres', 'DELETE FROM s2 WHERE id % 2 = 0; UPDATE s2 SET note = $$y$$; VACUUM s2');
	$primary->wait_for_catchup($standby);
	is($bg->query_safe('SELECT count(*) FROM s2'), $count, 'the open snapshot still sees every row');
	$bg->query_safe('COMMIT');
	$bg->quit;
	is($standby->safe_psql('postgres', 'SELECT count(*) FROM s2'), 2500, 'a new snapshot sees the deletes');
};

subtest 'promotion keeps the data and accepts writes' => sub {
	my $before = digest($standby, 's2');
	$standby->promote;
	is(digest($standby, 's2'), $before, 'same rows after promotion');
	$standby->safe_psql('postgres', "INSERT INTO s2 VALUES (100000, 1, 'new'); UPDATE s2 SET v = 0 WHERE id = 1; DELETE FROM s2 WHERE id = 3");
	is($standby->safe_psql('postgres', 'SELECT count(*) FROM s2'), 2500, 'writes work on the promoted node');
};
$standby->stop;
$primary->stop;

# ---------- wal_level = minimal ----------
my $minimal = sift_node('minimal');
$minimal->append_conf('postgresql.conf', "wal_level = minimal\nmax_wal_senders = 0");
$minimal->start;
install_sift($minimal);

for my $threshold ('0', '1GB')
{
	subtest "created and loaded in one transaction, wal_skip_threshold = $threshold" => sub {
		$minimal->safe_psql('postgres', "SET wal_skip_threshold = '$threshold'; BEGIN; CREATE TABLE m$threshold (id int PRIMARY KEY, v int, note text) USING sift_table;"
			. " INSERT INTO m$threshold SELECT g, g, 'm' FROM generate_series(1, $rows) g; COMMIT");
		my $before = digest($minimal, "m$threshold");
		$minimal->stop('immediate');
		$minimal->start;
		is(digest($minimal, "m$threshold"), $before, 'the rows are durable at commit');
		$minimal->safe_psql('postgres', "SET wal_skip_threshold = '$threshold'; BEGIN; TRUNCATE m$threshold; INSERT INTO m$threshold SELECT g, 0, 't' FROM generate_series(1, 100) g; COMMIT");
		my $after = digest($minimal, "m$threshold");
		$minimal->stop('immediate');
		$minimal->start;
		is(digest($minimal, "m$threshold"), $after, 'truncate in the creating transaction is durable');
	};
}
$minimal->stop;

# ---------- pg_dump / pg_restore ----------
my $src = sift_node('dump_src');
$src->start;
install_sift($src);
$src->safe_psql('postgres', q{
	CREATE TABLE d_orders (id int PRIMARY KEY, store text, total numeric, items jsonb, note text) USING sift_table;
	INSERT INTO d_orders SELECT g, 'store' || g % 4, g * 1.25, jsonb_build_array(jsonb_build_object('sku', g)), 'n' || g FROM generate_series(1, 500) g;
	UPDATE d_orders SET note = NULL WHERE id % 10 = 0;
	DELETE FROM d_orders WHERE id % 17 = 0;
	CREATE INDEX d_orders_store ON d_orders (store);
	CREATE TABLE d_heap (id int, v text);
	INSERT INTO d_heap VALUES (1, 'heap');
	CREATE TABLE d_docs (id int, body text) USING sift_table;
	INSERT INTO d_docs SELECT g, 'dump doc ' || g FROM generate_series(1, 100) g;
	CREATE INDEX d_docs_idx ON d_docs USING sift (body);
});
my $want = $src->safe_psql('postgres', "SELECT count(*) || ':' || sum(id) || ':' || md5(string_agg(coalesce(note, '-') || store || total::text || items::text, ',' ORDER BY id)) FROM d_orders");

for my $fmt ('plain', 'custom')
{
	subtest "dump and restore, $fmt format" => sub {
		my $dst = sift_node("dump_dst_$fmt");
		$dst->start;
		my $file = $src->basedir . "/dump.$fmt";
		if ($fmt eq 'plain')
		{
			$src->command_ok([ 'pg_dump', '-p', $src->port, '-h', $src->host, '-d', 'postgres', '-f', $file ], 'pg_dump');
			$dst->command_ok([ 'psql', '-X', '-q', '-v', 'ON_ERROR_STOP=1', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', '-f', $file ], 'restore');
		}
		else
		{
			$src->command_ok([ 'pg_dump', '-Fc', '-p', $src->port, '-h', $src->host, '-d', 'postgres', '-f', $file ], 'pg_dump -Fc');
			$dst->command_ok([ 'pg_restore', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', '--exit-on-error', $file ], 'pg_restore');
		}
		is($dst->safe_psql('postgres', "SELECT count(*) || ':' || sum(id) || ':' || md5(string_agg(coalesce(note, '-') || store || total::text || items::text, ',' ORDER BY id)) FROM d_orders"),
			$want, 'the restored rows are identical');
		is($dst->safe_psql('postgres', "SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname IN ('d_orders', 'd_docs') ORDER BY c.relname"),
			"sift_table\nsift_table", 'the access method is restored');
		is($dst->safe_psql('postgres', "SET enable_seqscan = off; SELECT count(*) FROM d_docs WHERE body \@\@> 'dump'"), 100, 'the bm25 index is rebuilt');
		$dst->stop;
	};
}
$src->stop;

done_testing();
