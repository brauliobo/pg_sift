# kill -9 of the backend running an index build, a maintenance step (GIN
# pending-list cleanup / sift promotion and merge) or VACUUM, then crash
# recovery, verification and result equivalence with the SQL reference.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $node = sift_node('crash');
$node->start;
my $has = features($node);

for my $am (ams())
{
	my $n = $am->{name};
	subtest "$n: crash during build, maintenance and vacuum" => sub {
		my @missing = grep { !$has->{$_} } @{ $am->{needs} };
		plan skip_all => "sift not implemented yet: @missing" if @missing;
		my ($t, $idx) = ("crash_$n", "crash_${n}_idx");
		my $hits = 0;
		load_corpus($node, $t, 60000);

		for my $delay (0, 100)
		{
			$hits += crash_during($node, $am->{index}->($t, $idx), $delay);
			$node->safe_psql('postgres', "DROP INDEX IF EXISTS $idx");
		}
		$node->safe_psql('postgres', $am->{index}->($t, $idx));
		$node->safe_psql('postgres', $am->{verify}->($idx));
		agree($node, $am, $t, 'rebuilt after crashed builds');

		# a bulk insert through aminsert, killed mid-flight: its rows must vanish from the index with the abort
		$hits += crash_during($node, "INSERT INTO $t SELECT g, 'w6 rare torn ' || g FROM generate_series(300001, 600000) g", 150);
		$node->safe_psql('postgres', $am->{verify}->($idx));
		agree($node, $am, $t, 'crash in insert');

		for my $delay (0, 50)
		{
			$node->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w1 rare late ' || g"
				. " FROM generate_series(100001 + $delay * 1000, 115000 + $delay * 1000) g");
			$hits += crash_during($node, $am->{maint}->($idx), $delay);
			$node->safe_psql('postgres', $am->{verify}->($idx));
			agree($node, $am, $t, "crash in maintenance after $delay ms");
		}

		if ($n eq 'sift' && $has->{promote} && $has->{merge})
		{
			for my $batch (1 .. 3)
			{
				$node->safe_psql('postgres', "INSERT INTO $t SELECT g, 'w6 merge batch$batch ' || g"
					. " FROM generate_series(1000000 + $batch * 100000, 1040000 + $batch * 100000) g;"
					. " SELECT sift.promote('$idx')");
			}
			$hits += crash_during($node, "SELECT sift.merge('$idx')", 5);
			$node->safe_psql('postgres', $am->{verify}->($idx));
			agree($node, $am, $t, 'crash in merge');
		}

		$node->safe_psql('postgres', "DELETE FROM $t WHERE id % 3 = 0");
		$hits += crash_during($node, "VACUUM $t", 30);
		$node->safe_psql('postgres', $am->{verify}->($idx));
		agree($node, $am, $t, 'crash in vacuum');
		$node->safe_psql('postgres', "VACUUM $t");
		$node->safe_psql('postgres', $am->{verify}->($idx));
		agree($node, $am, $t, 'vacuum completed after the crash');
		note("$n: $hits kills landed while the operation was running");
	};
}

$node->stop;
done_testing();
