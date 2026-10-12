# wal_level = minimal: an index built (or rewritten by TRUNCATE) in the
# transaction that created its relfilenode skips WAL and must be durable at
# commit, both through fsync (wal_skip_threshold = 0) and through WAL-logging
# the relation at commit (large threshold). Each case ends in an immediate
# shutdown (crash) and recovery.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $node = sift_node('minimal');
$node->append_conf('postgresql.conf', "wal_level = minimal\nmax_wal_senders = 0");
$node->start;
my $has = features($node);

sub crash_restart
{
	$node->stop('immediate');
	$node->start;
}

for my $am (ams())
{
	my $n = $am->{name};
	for my $threshold ('0', '1GB')
	{
		subtest "$n: index built in its creating transaction, wal_skip_threshold = $threshold" => sub {
			my @missing = grep { !$has->{$_} } @{ $am->{needs} };
			plan skip_all => "sift not implemented yet: @missing" if @missing;
			my $t = "min_${n}_" . ($threshold eq '0' ? 'sync' : 'wal');
			my $idx = "${t}_idx";
			my $set = "SET wal_skip_threshold = '$threshold';";

			$node->safe_psql('postgres', "$set BEGIN;"
				. " CREATE TABLE $t (id int, body text);"
				. " INSERT INTO $t SELECT g, concat_ws(' ', 'w' || g % 7, 'w' || g % 13, 'w' || g % 101,"
				. " CASE WHEN g % 5 = 0 THEN 'rare' END) FROM generate_series(1, 20000) g;"
				. " @{[ $am->{index}->($t, $idx) ]}; COMMIT;");
			crash_restart();
			$node->safe_psql('postgres', $am->{verify}->($idx));
			agree($node, $am, $t, 'after crash following a WAL-skipped build');

			$node->safe_psql('postgres', "$set BEGIN; TRUNCATE $t;"
				. " INSERT INTO $t SELECT g, 'w1 rare truncated ' || g FROM generate_series(1, 5000) g; COMMIT;");
			crash_restart();
			$node->safe_psql('postgres', $am->{verify}->($idx));
			agree($node, $am, $t, 'after crash following TRUNCATE and reload');

			$node->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w6 logged ' || g FROM generate_series(1, 3000) g");
			crash_restart();
			$node->safe_psql('postgres', $am->{verify}->($idx));
			agree($node, $am, $t, 'after crash following WAL-logged inserts');
		};
	}
}

$node->stop;
done_testing();
