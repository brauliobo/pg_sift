# sift_table promotion off the writer's path (sift.table_async_promote): the backend that closes a write buffer asks a worker to promote it, so a
# load that commits as it goes ends with column segments and no VACUUM; the rows are a heap twin's, a crash while the writers and the worker
# run loses none of a committed transaction and splits none, and with sift.async_maintenance off the writer promotes it itself.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(start finish);

my $clients = 4;
my $txns = knob('SIFT_TXNS', 4000, 3000);
my $node = sift_node('async_promote');
$node->append_conf('postgresql.conf', "autovacuum = off\nsift.table_async_promote = on\nsift.maintenance_naptime = 200ms\nsift.log_min_duration = 0\n"
	. "sift.table_write_buffer_limit = 2048\nmax_parallel_maintenance_workers = 0");
$node->start;
install_sift($node);

my $worker = qr/table maintenance worker finished/;
my $digest = 'SELECT count(*) || $$:$$ || count(DISTINCT id) || $$:$$ || sum(id) || $$:$$ || sum(length(v)) || $$:$$ || count(*) FILTER (WHERE v <> repeat(chr(97 + (id %% 26)::int), 300 + (id %% 400)::int)) FROM %s';
sub digest { return $node->safe_psql('postgres', sprintf($digest, $_[0])); }
sub segs { return $node->safe_psql('postgres', "SELECT count(*) FROM sift.table_segments('s') WHERE kind = '$_[0]'"); }

$node->safe_psql('postgres', 'CREATE TABLE s (id bigint, v text) USING sift_table WITH (autovacuum_enabled = false); CREATE TABLE h (id bigint, v text) USING heap');
my $dir = PostgreSQL::Test::Utils::tempdir;
open(my $f, '>', "$dir/w.sql") or die;
print $f "\\set cnt :cnt + 1\n\\set id :client_id * 1000000 + :cnt\nBEGIN;\n"
	. "INSERT INTO s VALUES (:id, repeat(chr(97 + (:id % 26)), 300 + (:id % 400)));\n"
	. "INSERT INTO h VALUES (:id, repeat(chr(97 + (:id % 26)), 300 + (:id % 400)));\nEND;\n";
close $f;
my @bench = ('pgbench', '-n', '-h', $node->host, '-p', $node->port, '-c', $clients, '-j', $clients, '-D', 'cnt=0', '-f', "$dir/w.sql");

subtest 'the worker promotes what the writers closed, with no VACUUM' => sub {
	my $offset = -s $node->logfile;
	$node->command_ok([ @bench, '-t', $txns, 'postgres' ], 'writers commit one row at a time');
	ok($node->wait_for_log($worker, $offset), 'a worker ran');
	# a request within the naptime of the last one is not made: one more close, after it, settles whatever is left. The request of the writer that closes the buffer
	# can reach the worker before the writer commits, with the rows not final, and the worker is gone when they are: the batch is made again until nothing waits
	sleep 3;
	my $settled = 0;
	for my $round (1 .. 4)
	{
		my ($lo, $hi) = (6000 * ($round - 1) + 1, 6000 * $round);
		for my $t ('s', 'h')
		{
			$node->safe_psql('postgres', "INSERT INTO $t SELECT i, repeat(chr(97 + (i % 26)), 300 + (i % 400)) FROM generate_series($lo, $hi) i");
		}
		for (1 .. 40)
		{
			$settled = $node->safe_psql('postgres', "SELECT count(*) = 0 FROM sift.table_segments('s') WHERE kind = 'sealed buffer'") eq 't';
			last if $settled;
			select(undef, undef, undef, 0.25);
		}
		last if $settled;
	}
	ok($settled, 'no closed write buffer waits');
	cmp_ok(segs('columns'), '>', 0, 'column segments exist');
	is(digest('s'), digest('h'), 'the rows are the heap twin\'s, every one with its bytes');
};

subtest 'a crash while the writers and the worker run' => sub {
	$node->safe_psql('postgres', 'TRUNCATE s; TRUNCATE h');
	my ($out, $err) = ('', '');
	my $h = start([ @bench, '-T', '30', 'postgres' ], '>', \$out, '2>', \$err);
	sleep 4;
	$node->stop('immediate');
	finish($h);
	$node->start;
	my $d = digest('s');
	is($d, digest('h'), 'the same rows as the twin after crash recovery');
	like($d, qr/^[1-9]\d*:/, 'and some rows');
	$node->safe_psql('postgres', 'VACUUM s');
	is(digest('s'), $d, 'the same rows after VACUUM');
};

subtest 'with sift.async_maintenance off the writer promotes it itself' => sub {
	$node->safe_psql('postgres', 'TRUNCATE s; TRUNCATE h; VACUUM s');
	sleep 1;
	my $offset = -s $node->logfile;
	my $load = 'SET sift.async_maintenance = off; DO $$ BEGIN FOR i IN 1 .. 30 LOOP INSERT INTO s SELECT g, repeat(chr(97 + (g % 26)::int), 300 + (g % 400)::int) FROM generate_series(i * 1000, i * 1000 + 999) g; COMMIT; END LOOP; END $$';
	$node->safe_psql('postgres', $load);
	cmp_ok(segs('columns'), '>', 0, 'column segments right after the load');
	sleep 2;
	unlike(slurp_file($node->logfile, $offset), $worker, 'no worker ran');
};

# whoever looks at the table asks again once the transaction is over: a scan, and a SiftAgg aggregate (which does not scan the table)
my %readers = ('a table scan' => ['SET sift.enable_agg = off', 'SELECT count(*) FROM s', qr/Seq Scan/],
			   'a SiftAgg aggregate' => ['SET sift.enable_agg = on', 'SELECT count(*), sum(id) FROM s', qr/SiftAgg/]);
foreach my $reader (sort keys %readers)
{
	subtest "a request that found only uncommitted rows is not lost ($reader)" => sub {
		$node->safe_psql('postgres', 'TRUNCATE s; TRUNCATE h; VACUUM s');
		sleep 1;
		my $offset = -s $node->logfile;
		my $a = $node->background_psql('postgres');
		# the writer closes write buffers inside its transaction and asks for them to be settled: its rows are not final, the worker has nothing to do and leaves
		$a->query_safe('BEGIN');
		$a->query_safe('INSERT INTO s SELECT g, repeat(chr(97 + (g % 26)::int), 300 + (g % 400)::int) FROM generate_series(1, 8000) g');
		ok($node->wait_for_log($worker, $offset), 'a worker ran and left');
		cmp_ok(segs('sealed buffer'), '>', 0, 'closed write buffers wait for the transaction');
		$a->query_safe('COMMIT');
		$a->quit;
		my ($set, $query, $plan) = @{ $readers{$reader} };
		like($node->safe_psql('postgres', "$set; EXPLAIN $query"), $plan, 'the plan of the reader');
		# no other write buffer closes
		my $settled = 0;
		for (1 .. 60)
		{
			$node->safe_psql('postgres', "SET sift.table_auto_defrag = on; $set; $query");
			$settled = segs('sealed buffer') == 0;
			last if $settled;
			select(undef, undef, undef, 0.25);
		}
		ok($settled, 'the closed write buffers are promoted after the commit, with no other buffer closing');
		is(digest('s'), '8000:8000:' . $node->safe_psql('postgres', 'SELECT sum(g) FROM generate_series(1, 8000) g') . ':'
			. $node->safe_psql('postgres', 'SELECT sum(300 + (g % 400)) FROM generate_series(1, 8000) g') . ':0', 'every row with its bytes');
	};
}

$node->stop;
done_testing();
