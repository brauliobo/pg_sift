# kill -9 of a parallel worker in the middle of a parallel SiftScan (aggregated, plain and ranked):
# the leader fails with the rest of the cluster, crash recovery brings the index back, it verifies,
# and parallel and serial plans agree with the SQL reference afterwards.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $rows = knob('SIFT_PAR_ROWS', 12000, 6000);
my $node = sift_node('parallel_crash');
$node->start;
install_sift($node);

$node->safe_psql('postgres', qq{
	CREATE TABLE par (id int, body text, pad text) WITH (autovacuum_enabled = off, parallel_workers = 2);
	ALTER TABLE par ALTER COLUMN pad SET STORAGE PLAIN;
	INSERT INTO par SELECT g, concat_ws(' ', 'w' || g % 7, 'w' || g % 13, 'w' || g % 101,
		CASE WHEN g % 5 = 0 THEN 'rare' END, 'doc' || g), repeat('x', 1500) FROM generate_series(1, $rows) g;
	CREATE INDEX par_idx ON par USING sift (body) WITH (mutable_limit = 64);
	INSERT INTO par SELECT g, 'w1 rare late ' || g, repeat('x', 1500) FROM generate_series(100001, 100500) g;
	CREATE FUNCTION slow(i int) RETURNS bool LANGUAGE sql STABLE PARALLEL SAFE AS 'SELECT pg_sleep(0.003) IS NOT NULL';
});

my $force = 'SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; '
	. 'SET min_parallel_index_scan_size = 0;';
my %shape = (
	aggregate => "SELECT count(*) FROM par WHERE body @@> 'w1' AND slow(id)",
	scan      => "SELECT id FROM par WHERE body @@> 'w1' AND slow(id)",
	ranked    => "SELECT id FROM par WHERE body @@> 'w1 rare' AND slow(id) ORDER BY sift.score(ctid) DESC LIMIT 1000",
);

# Index scan (parallel and serial) and the SQL reference agree on every probe term
sub agree_parallel
{
	my ($what) = @_;
	for my $term (qw(w1 w6 w100 rare))
	{
		my $res = $node->safe_psql('postgres', qq{
			$force
			SELECT (SELECT count(*) FROM par WHERE body @@> '$term')
				= (SELECT count(*) FROM par WHERE sift_ref.match(body, '$term'))
				AND (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM par WHERE body @@> '$term'
					ORDER BY sift.score(ctid) DESC LIMIT 100000) t)
				IS NOT DISTINCT FROM (SELECT array_agg(id ORDER BY id) FROM par WHERE sift_ref.match(body, '$term'));
		});
		is($res, 't', "$what: parallel scans agree with the reference on '$term'");
	}
}

for my $name (sort keys %shape)
{
	subtest "kill -9 of a parallel worker in a parallel $name" => sub {
		my $plan = $node->safe_psql('postgres', "$force EXPLAIN (COSTS OFF) $shape{$name}");
		like($plan, qr/Parallel Custom Scan/, "$name: the plan is parallel");

		my $bg = $node->background_psql('postgres', on_error_stop => 0);
		$bg->query_safe($force);
		$bg->query_until(qr/started/, "\\echo started\n$shape{$name};\n");

		my $pid;
		for (1 .. 200)
		{
			$pid = $node->safe_psql('postgres', "SELECT pid FROM pg_stat_activity WHERE backend_type = 'parallel worker' LIMIT 1");
			last if $pid;
			select(undef, undef, undef, 0.05);
		}
		ok($pid, "$name: a parallel worker is running");

		my $log_offset = -s $node->logfile;
		kill 'KILL', $pid;
		$node->wait_for_log(qr/database system is ready to accept connections/, $log_offset);
		eval { $bg->quit };
		$node->wait_for_log(qr/terminated by signal 9/, $log_offset);

		$node->safe_psql('postgres', "SELECT sift.verify('par_idx', true)");
		agree($node, sift_am(), 'par', "$name: after the crash");
		agree_parallel("$name: after the crash");
	};
}

$node->stop;
done_testing();
