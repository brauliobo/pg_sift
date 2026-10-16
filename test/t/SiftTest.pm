# Shared helpers for the sift TAP tests (run through scripts/tap.sh, which
# makes sift loadable in every node through TEMP_CONFIG).
#
# Every scenario runs for a GIN control index first, so the crash, replica and
# WAL machinery is proven on this host independently of sift.
package SiftTest;

use strict;
use warnings FATAL => 'all';
use Exporter 'import';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(sift_node install_sift ams sift_am knob load_corpus agree crash_during);

my $repo = $ENV{SIFT_REPO} or die "SIFT_REPO not set; run through scripts/tap.sh";

sub sift_node
{
	my ($name, %params) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(%params);
	$node->append_conf('postgresql.conf', 'restart_after_crash = on');
	return $node;
}

# Installs sift, amcheck and the SQL reference in database postgres
sub install_sift
{
	my ($node) = @_;
	$node->safe_psql('postgres', 'CREATE EXTENSION sift; CREATE EXTENSION amcheck;');
	$node->safe_psql('postgres', '',
		extra_params => [ '-v', 'sift_ref_lib=1', '-f', "$repo/test/sql/reference.sql" ]);
}

# $ENV{$var}, else $full, or $quick under SIFT_QUICK=1 (scripts/ci.sh quick)
sub knob
{
	my ($var, $full, $quick) = @_;
	return $ENV{$var} // ($ENV{SIFT_QUICK} ? $quick : $full);
}

# Index flavours: DDL, maintenance step, verification
my @AMS = (
	{
		name => 'gin',
		index => sub { "CREATE INDEX $_[1] ON $_[0] USING gin (to_tsvector('simple', body)) WITH (fastupdate = on)" },
		maint => sub { "SELECT gin_clean_pending_list('$_[0]')" },
		verify => sub { "SELECT gin_index_check('$_[0]')" },
		match => sub { "to_tsvector('simple', body) @@ to_tsquery('simple', '$_[0]')" },
	},
	{
		name => 'sift',
		index => sub { "CREATE INDEX $_[1] ON $_[0] USING sift (body) WITH (mutable_limit = 64)" },
		maint => sub { "SELECT sift.promote('$_[0]')" },
		verify => sub { "SELECT sift.verify('$_[0]', true)" },
		match => sub { "body @@> '$_[0]'" },
	});

# the GIN control proves the host and the harness; the quick tier runs sift alone
sub ams { return $ENV{SIFT_QUICK} ? $AMS[1] : @AMS }
sub sift_am { return $AMS[1] }

# Table $tbl(id, body) with $n rows over a small vocabulary
sub load_corpus
{
	my ($node, $tbl, $n, $db) = @_;
	$node->safe_psql($db // 'postgres', qq{
		CREATE TABLE $tbl (id int, body text) WITH (autovacuum_enabled = off);
		INSERT INTO $tbl SELECT g, concat_ws(' ', 'w' || g % 7, 'w' || g % 13, 'w' || g % 101,
			CASE WHEN g % 5 = 0 THEN 'rare' END, 'doc' || g) FROM generate_series(1, $n) g;
	});
}

# Index scan (seq scan off) and the SQL reference agree on every probe term
sub agree
{
	my ($node, $am, $tbl, $what, $db) = @_;
	my $plan = $node->safe_psql($db // 'postgres',
		"SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM $tbl WHERE " . $am->{match}->('w1'));
	unlike($plan, qr/Seq Scan/, "$am->{name}: $what: the plan uses the index");
	for my $term (qw(w1 w6 w12 w100 rare))
	{
		my $res = $node->safe_psql($db // 'postgres', qq{
			SET enable_seqscan = off;
			SELECT (SELECT count(*) FROM $tbl WHERE @{[ $am->{match}->($term) ]})
				= (SELECT count(*) FROM $tbl WHERE sift_ref.match(body, '$term'));
		});
		is($res, 't', "$am->{name}: $what: index agrees with the reference on '$term'");
	}
}

# Runs $sql in a background session, SIGKILLs its backend once it is running
# (after $delay_ms), and waits for crash recovery
sub crash_during
{
	my ($node, $sql, $delay_ms) = @_;
	my $bg = $node->background_psql('postgres', on_error_stop => 0);
	my $pid = $bg->query_safe('SELECT pg_backend_pid()');
	$bg->query_until(qr/started/, "\\echo started\n$sql;\n");
	usleep($delay_ms * 1000);
	my $active = $node->safe_psql('postgres', "SELECT state FROM pg_stat_activity WHERE pid = $pid");
	note("killing backend $pid (state: " . ($active || 'gone') . ") during: $sql");
	my $log_offset = -s $node->logfile;
	kill 'KILL', $pid;
	$node->wait_for_log(qr/database system is ready to accept connections/, $log_offset);
	eval { $bg->quit };
	return $active eq 'active';
}

sub usleep { select(undef, undef, undef, $_[0] / 1e6) }

1;
