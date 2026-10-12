# Streaming replication: a hot standby replays index builds, inserts,
# maintenance and VACUUM, answers queries like the primary, and a scan open on
# the standby across merge and recycling either completes correctly or fails
# with a retryable serialization error.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use Test::More;

my $primary = sift_node('primary', allows_streaming => 1);
$primary->start;
my $has = features($primary);
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', "hot_standby_feedback = off\nmax_standby_streaming_delay = 2s");
$standby->start;

for my $am (ams())
{
	my $n = $am->{name};
	subtest "$n: replay on a hot standby" => sub {
		my @missing = grep { !$has->{$_} } @{ $am->{needs} };
		plan skip_all => "sift not implemented yet: @missing" if @missing;
		my ($t, $idx) = ("repl_$n", "repl_${n}_idx");
		load_corpus($primary, $t, 20000);
		$primary->safe_psql('postgres', $am->{index}->($t, $idx));
		$primary->safe_psql('postgres', qq{
			INSERT INTO $t SELECT g, 'w1 rare replica ' || g FROM generate_series(30001, 40000) g;
			UPDATE $t SET body = 'w12 updated ' || id WHERE id % 17 = 0;
			DELETE FROM $t WHERE id % 11 = 0;
		});
		$primary->safe_psql('postgres', $am->{maint}->($idx));
		$primary->safe_psql('postgres', "SELECT sift.merge('$idx')") if $n eq 'sift' && $has->{merge};
		$primary->safe_psql('postgres', "VACUUM $t");
		$primary->wait_for_catchup($standby);

		agree($standby, $am, $t, 'standby');
		$standby->safe_psql('postgres', $am->{verify}->($idx));
		my $q = "SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM $t WHERE "
		  . $am->{match}->('w100');
		is($standby->safe_psql('postgres', $q), $primary->safe_psql('postgres', $q),
			"$n: standby returns the primary's rows");
	};
}

subtest 'sift: standby scan across merge and segment recycling' => sub {
	plan skip_all => 'sift not implemented yet: build promote merge'
	  unless $has->{build} && $has->{promote} && $has->{merge};
	my ($t, $idx) = ('recycle', 'recycle_idx');
	load_corpus($primary, $t, 20000);
	$primary->safe_psql('postgres', "CREATE INDEX $idx ON $t USING sift (body) WITH (mutable_limit = 64)");
	$primary->wait_for_catchup($standby);

	my $bg = $standby->background_psql('postgres', on_error_stop => 0);
	$bg->query_safe('\set VERBOSITY verbose');
	$bg->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	my $expected = $bg->query_safe("SELECT count(*) FROM $t WHERE sift_ref.match(body, 'w1')");
	$bg->query_safe("DECLARE c CURSOR FOR SELECT id FROM $t WHERE body @@> 'w1'");
	$bg->query_safe('FETCH 10 FROM c');

	for my $round (1 .. 3)
	{
		$primary->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w1 churn $round ' || g"
			. " FROM generate_series(100000 * $round, 100000 * $round + 4000) g; SELECT sift.promote('$idx')");
		$primary->safe_psql('postgres', "DELETE FROM $t WHERE id % 5 = $round");
		$primary->safe_psql('postgres', "VACUUM $t; SELECT sift.merge('$idx'); VACUUM $t");
	}
	$primary->wait_for_catchup($standby);

	my ($out, $err) = $bg->query("MOVE FORWARD ALL IN c");
	if ($err =~ /ERROR/)
	{
		like($err, qr/ERROR:\s+40001/, 'scan aborted with a retryable serialization error');
		note("standby scan error: $err");
	}
	else
	{
		is($out, "MOVE " . ($expected - 10), 'scan returned the rows of its snapshot');
	}
	eval { $bg->quit };
	$standby->safe_psql('postgres', "SELECT sift.verify('$idx', true)");
	agree($standby, (ams())[1], $t, 'standby after recycling');
};

$standby->stop;
$primary->stop;
done_testing();
