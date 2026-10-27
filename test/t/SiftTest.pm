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

our @EXPORT = qw(sift_node install_sift install_sift_test check_dump_restore ams sift_am knob load_corpus agree verify_agree crash_during kill_expected kill9_wait);

my $repo = $ENV{SIFT_REPO} or die "SIFT_REPO not set; run through scripts/tap.sh";

sub sift_node
{
	my ($name, %params) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init(%params);
	$node->append_conf('postgresql.conf', 'restart_after_crash = on');
	return $node;
}

# The leak gate (scripts/leakcheck.sh) on every node any test starts, whoever creates it: the sanitizer runtime is
# preloaded into the servers only (SIFT_PRELOAD), the memory of the long-lived processes is recorded at the start, and
# when the test ends, before the nodes are torn down immediately, each one is checked, stopped cleanly (LeakSanitizer
# reports as its processes exit) and checked again. The findings go to $SIFT_TMP/leakcheck.findings, which
# scripts/tap.sh scans with the server logs once prove is done.
my $leakcheck = "$repo/scripts/leakcheck.sh";
my %started;
for my $method (qw(start restart))
{
	no strict 'refs';
	no warnings 'redefine';
	my $orig = \&{"PostgreSQL::Test::Cluster::$method"};
	*{"PostgreSQL::Test::Cluster::$method"} = sub {
		my ($node) = @_;
		my $ret;
		{
			local $ENV{LD_PRELOAD} = $ENV{SIFT_PRELOAD} if $ENV{SIFT_PRELOAD};
			$ret = $orig->(@_);
		}
		if ($ret)
		{
			$started{ $node->name } = $node;
			system($leakcheck, 'baseline', $node->data_dir);
		}
		return $ret;
	};
}

END
{
	my $exit_code = $?;
	for my $node (values %started)
	{
		next unless -f $node->data_dir . '/postmaster.pid';
		local $ENV{PGHOST} = $node->host;
		local $ENV{PGPORT} = $node->port;
		system($leakcheck, 'cluster', $node->data_dir);
		$node->stop('fast', fail_ok => 1);
		system($leakcheck, 'stopped', $node->data_dir);
	}
	$? = $exit_code;
}

# Installs sift, amcheck and the SQL reference in database postgres
sub install_sift
{
	my ($node) = @_;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_sift; CREATE EXTENSION amcheck;');
	$node->safe_psql('postgres', '',
		extra_params => [ '-v', 'sift_ref_lib=1', '-f', "$repo/test/sql/reference.sql" ]);
}

# install_sift and the testing extension; the test is skipped (why) when the build is not the testing one
sub install_sift_test
{
	my ($node, $why) = @_;
	plan skip_all => $why unless $node->safe_psql('postgres', "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift_test'");
	install_sift($node);
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_sift_test');
}

# pg_dump of table $t of $primary restored in one transaction into a node with $conf: the same rows by $digest->($node, $t), and after VACUUM
sub check_dump_restore
{
	my ($primary, $conf, $digest, $t) = @_;
	my $want = $digest->($primary, $t);
	my $file = $primary->basedir . '/dump.sql';
	$primary->command_ok([ 'pg_dump', '-p', $primary->port, '-h', $primary->host, '-d', 'postgres', '-t', $t, '-f', $file ], 'pg_dump');
	my $dst = sift_node('dump_dst');
	$dst->append_conf('postgresql.conf', $conf);
	$dst->start;
	install_sift_test($dst, 'the restore needs the testing build');
	$dst->command_ok([ 'psql', '-X', '-q', '-1', '-v', 'ON_ERROR_STOP=1', '-p', $dst->port, '-h', $dst->host, '-d', 'postgres', '-f', $file ], 'restore in one transaction');
	is($digest->($dst, $t), $want, 'the restored rows are identical');
	$dst->safe_psql('postgres', "VACUUM $t");
	is($digest->($dst, $t), $want, 'and after VACUUM');
	$dst->stop;
	$dst->clean_node;
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
		# gin_index_check is amcheck 1.5 (PostgreSQL 18+); older servers compare the index with the reference alone
		verify => sub { "DO \$\$ BEGIN IF current_setting('server_version_num')::int >= 180000 THEN PERFORM gin_index_check('$_[0]'); END IF; END \$\$" },
		match => sub { "to_tsvector('simple', body) @@ to_tsquery('simple', '$_[0]')" },
	},
	{
		name => 'sift',
		index => sub { "CREATE INDEX $_[1] ON $_[0] USING sift (body) WITH (write_buffer_limit = 64)" },
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
	# sift_ref.match(body, term) tokenizes every row for every term, and that is nearly all of its cost: the rows are
	# tokenized once here (a standby cannot hold a temp table) and each term is evaluated on the same token arrays
	my @terms = qw(w1 w6 w12 w100 rare);
	my $probes = join ', ', map {
		"(SELECT count(*) FROM $tbl WHERE @{[ $am->{match}->($_) ]})"
		  . " = (SELECT count(*) FROM d WHERE toks IS NOT NULL AND sift_ref.eval_doc(sift_ref.analyzed('$_', NULL), toks))"
	} @terms;
	my @res = split /\|/, $node->safe_psql($db // 'postgres', qq{
		SET enable_seqscan = off;
		WITH d AS MATERIALIZED (SELECT sift_ref.tokens(body, NULL) AS toks FROM $tbl) SELECT $probes;
	});
	is($res[$_], 't', "$am->{name}: $what: index agrees with the reference on '$terms[$_]'") for 0 .. $#terms;
}

# The index passes its own verification and agrees with the SQL reference
sub verify_agree
{
	my ($node, $am, $tbl, $idx, $what) = @_;
	$node->safe_psql('postgres', $am->{verify}->($idx));
	agree($node, $am, $tbl, $what);
}

# SIGKILLs a server process on purpose: the postmaster logs its death, which the leak gate expects this once
sub kill_expected
{
	my ($pid) = @_;
	system($leakcheck, 'expect', 1, 'terminated by signal 9');
	kill 'KILL', $pid;
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
	kill_expected($pid);
	$node->wait_for_log(qr/database system is ready to accept connections/, $log_offset);
	eval { $bg->quit };
	return $active eq 'active';
}

# SIGKILLs the postmaster and waits for its children: they hold the shared memory until they next wait, and a server
# started before they are gone fails with "pre-existing shared memory block is still in use"
sub kill9_wait
{
	my ($node) = @_;
	my @children = grep { ppid($_) == $node->{_pid} } map { m{(\d+)$} } glob('/proc/[0-9]*');
	$node->kill9;
	for (1 .. 1200)
	{
		return unless grep { running($_) } @children;
		usleep(100_000);
	}
	die "children of the killed postmaster still run: @children";
}

# the parent of a process (a process that is gone has none); no pgrep, which a minimal container lacks and whose failed exec in a
# forked test would run the END block of the test
sub ppid
{
	return (proc_stat($_[0]) =~ /\) \S (\d+)/)[0] // 0;
}

sub running
{
	return proc_stat($_[0]) =~ /^\d+ .*\) [^Z]/;
}

# /proc/PID/stat, empty when the process is gone: a process that init reaps between the open and the read fails the read with ESRCH
sub proc_stat
{
	open(my $stat, '<', "/proc/$_[0]/stat") or return '';
	return scalar(<$stat>) // '';
}

sub usleep { select(undef, undef, undef, $_[0] / 1e6) }

1;
