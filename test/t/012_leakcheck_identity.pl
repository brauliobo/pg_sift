# The leak gate uses the same libpq role selection as its cluster owner: Perl nodes
# default to the OS user, while shell nodes initialized with -U postgres select it explicitly.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use IPC::Run qw(run);
use Test::More;

my $node = sift_node('leak_identity');
$node->start;
my $user = $node->safe_psql('postgres', 'SELECT current_user');
my $gate = "$ENV{SIFT_REPO}/scripts/leakcheck.sh";
my $scratch = $node->data_dir . '/gate';
mkdir $scratch or die "mkdir $scratch: $!";

sub gate_result
{
	my ($node, $user) = @_;
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	local $ENV{PGUSER};
	delete $ENV{PGUSER};
	$ENV{PGUSER} = $user if defined $user;
	local $ENV{SIFT_TMP} = $scratch;
	my ($out, $err);
	run [ $gate, 'cluster', $node->data_dir ], '>', \$out, '2>', \$err;
	return ($? >> 8, $out . $err);
}

my ($rc, $out) = gate_result($node, undef);
is($rc, 0, 'the default OS-user node passes the cluster gate') or diag $out;
($rc, $out) = gate_result($node, $user);
is($rc, 0, 'an explicit node role passes the cluster gate') or diag $out;
($rc, $out) = gate_result($node, 'missing_leakcheck_role');
isnt($rc, 0, 'an invalid role fails instead of falling back');
like($out, qr/cannot query the cluster/, 'query failure remains a gate finding');
# Deliberate connection failures must not contaminate the outer log scan.
rename "$scratch/leakcheck.findings", "$scratch/findings.deliberate" or die "rename findings: $!";

my $postgres = sift_node('leak_postgres', extra => [ '-U', 'postgres' ]);
$postgres->start;
($rc, $out) = gate_result($postgres, 'postgres');
is($rc, 0, 'a shell-style postgres owner passes the cluster gate') or diag $out;
$postgres->stop;
# Leave the OS-user node running for SiftTest's normal END gate too.
done_testing();
