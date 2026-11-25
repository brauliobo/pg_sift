# Format major 2 on a cascade and on a delayed standby: primary -> standby1 -> standby2 (a standby that streams from a standby) and primary -> standby3 with recovery_min_apply_delay,
# every node replaying under wal_consistency_checking = 'all'. pgbench changes a sift_table (column deltas, inserts, deletes, wide values in extents of their own) and a heap table with a
# sift_col index while VACUUM, sift.maintain and REINDEX run beside it, and read-only pgbench sessions of the two far standbys read both with the replay going on (an error of a read is a
# conflict with the recovery, nothing else). Then the primary is quiet and the four nodes hold the same rows, answer the same through the sift_col index and through the normal plan, and
# verify, with the visibility map of the frozen table replayed everywhere. One transaction of the primary (column deltas, deletes, inserts, a wide value) is replayed by the delayed standby
# no sooner than the delay after it was written, and every state its reads see meanwhile is the one before or the one after the transaction.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use IPC::Run ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $port  = $ENV{SIFT_PORT} // 56100;
my $secs  = knob('SIFT_STANDBY_SECS', 4, 2);
my $delay = 3;
my $conf  = join("\n", "shared_preload_libraries = 'pg_sift'", "sift.test_create_format1 = off", "wal_consistency_checking = 'all'", "autovacuum = off", "shared_buffers = 64MB", "restart_after_crash = on");

sub node { my ($name, $offset) = @_; return PostgreSQL::Test::Cluster->new($name, port => $port + $offset) }

my $primary = node('primary', 1);
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
install_sift_test($primary, 'the format major 2 structures are exercised by testing functions');
$primary->safe_psql('postgres', q{
	CREATE TABLE ev (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL, tag text) USING sift_table;
	INSERT INTO ev SELECT g, g % 13, g % 100, 'e' || g % 7 FROM generate_series(1, 6000) g;
	CREATE INDEX ev_k ON ev (k);
	CREATE TABLE wb (id int PRIMARY KEY, w text NOT NULL) USING sift_table;
	INSERT INTO wb SELECT g, (SELECT string_agg(md5(g || ':' || i), '') FROM generate_series(1, 700) i) FROM generate_series(1, 60) g;
	CREATE TABLE fc (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL);
	INSERT INTO fc SELECT g, g % 17, g % 1000 FROM generate_series(1, 6000) g;
	CREATE INDEX fc_idx ON fc USING sift_col (id, g, v);
	SELECT sift.create_projection('fc_idx', 'by_g', 'GROUP BY g MEASURES count(*), sum(v)');
	CREATE TABLE marker (ts timestamptz);
	CREATE FUNCTION dg() RETURNS text LANGUAGE sql AS $$ SELECT concat_ws('|',
		(SELECT count(*) || ':' || sum(v) || ':' || md5(string_agg(id || ',' || k || ',' || v || ',' || coalesce(tag, ''), ';' ORDER BY id)) FROM ev),
		(SELECT count(*) || ':' || md5(string_agg(id || ',' || md5(w), ';' ORDER BY id)) FROM wb),
		(SELECT count(*) || ':' || sum(v) || ':' || md5(string_agg(id || ',' || g || ',' || v, ';' ORDER BY id)) FROM fc)) $$;
	CREATE FUNCTION answers() RETURNS text LANGUAGE sql AS $$ SELECT string_agg(g || ':' || c || ':' || s, ',' ORDER BY g) FROM (SELECT g, count(*) c, sum(v) s FROM fc GROUP BY g) x $$;
	CREATE FUNCTION answers_on() RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN answers();
	CREATE FUNCTION answers_off() RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN answers();
	VACUUM (FREEZE) ev;
	VACUUM (FREEZE) wb;
	SELECT pg_create_physical_replication_slot('s1');
	SELECT pg_create_physical_replication_slot('s3');});

sub standby
{
	my ($name, $offset, $from, $slot, $backup, $extra) = @_;
	my $s = node($name, $offset);
	$s->init_from_backup($from, $backup, has_streaming => 1);
	$s->append_conf('postgresql.conf', "$conf\nprimary_slot_name = '$slot'\nmax_standby_streaming_delay = 1s\n$extra");
	$s->start;
	return $s;
}

$primary->backup('b1');
my $standby1 = standby('standby1', 2, $primary, 's1', 'b1', "hot_standby_feedback = off");
$primary->backup('b3');
my $standby3 = standby('standby3', 4, $primary, 's3', 'b3', "hot_standby_feedback = on\nrecovery_min_apply_delay = '${delay}s'");
$primary->wait_for_catchup($standby1);
$standby1->safe_psql('postgres', q{SELECT pg_create_physical_replication_slot('s2')});
$standby1->backup('b2');
my $standby2 = standby('standby2', 3, $standby1, 's2', 'b2', "hot_standby_feedback = off");
ok($standby1->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_replication WHERE application_name = 'standby2' AND state = 'streaming'}), 'standby2 streams from standby1, a standby');

my @standbys = ($standby1, $standby2, $standby3);
my @nodes = ($primary, @standbys);
sub catchup
{
	my $lsn = $primary->lsn('insert');
	$primary->wait_for_catchup($standby1, 'replay', $lsn);
	$standby1->wait_for_catchup($standby2, 'replay', $lsn);
	$primary->wait_for_catchup($standby3, 'replay', $lsn);
}

my $dir = $primary->basedir;
my %script = (
	upd => q{\set rid random(1, 6000)
UPDATE ev SET v = v + 1 WHERE id = :rid;},
	ins => q{\set rid random(10000, 99999999)
INSERT INTO ev SELECT g, g % 13, 1, 'n' FROM generate_series(:rid, :rid + 9) g ON CONFLICT DO NOTHING;
DELETE FROM ev WHERE id = :rid - 5;},
	wide => q{\set wid random(1, 60)
UPDATE wb SET w = (SELECT string_agg(md5(:wid || ':' || i || ':' || clock_timestamp()), '') FROM generate_series(1, 700) i) WHERE id = :wid;},
	heap => q{\set fid random(1, 6000)
UPDATE fc SET v = v + 1 WHERE id = :fid;
INSERT INTO fc VALUES (100000 + :fid, :fid % 17, 1) ON CONFLICT DO NOTHING;},
	maint => q{VACUUM (FREEZE) ev;
VACUUM fc;
VACUUM wb;
SELECT count(*) FROM sift.maintain('ev');
SELECT count(*) FROM sift.maintain('fc_idx');
REINDEX INDEX fc_idx;},
	read => q{\set k random(0, 12)
SELECT count(*), sum(v) FROM ev WHERE k = :k;
SELECT g, count(*), sum(v) FROM fc GROUP BY g;
SELECT count(*) FROM wb WHERE length(w) > 1000;},
);
for my $name (keys %script) { append_to_file("$dir/$name.sql", "$script{$name}\n") }

# bench NODE SECONDS CLIENTS FILES...: pgbench in the background; the closure waits for it and returns (transactions processed, the errors that are not a conflict with the recovery)
sub bench
{
	my ($node, $seconds, $clients, @files) = @_;
	my ($out, $err) = ('', '');
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	local $ENV{PGAPPNAME} = 'pgbench';
	my $h = IPC::Run::start([ 'pgbench', '-n', '-M', 'simple', '-T', $seconds, '-c', $clients, '-j', 2, map({ ('-f', "$dir/$_.sql") } @files), 'postgres' ], '>', \$out, '2>', \$err);
	return sub
	{
		$h->finish;
		my ($n) = $out =~ /number of transactions actually processed: (\d+)/;
		my @bad = grep { /ERROR|error/ && !/conflict with recovery|recovery conflict/ } split /\n/, "$err$out";
		return ($n // 0, join("\n", @bad));
	};
}

my @running = (bench($primary, $secs, 3, qw(upd ins wide heap)), bench($primary, $secs, 1, 'maint'), bench($standby2, $secs, 2, 'read'), bench($standby3, $secs, 2, 'read'));
my @done = map { [ $_->() ] } @running;
cmp_ok($done[0][0], '>', 0, 'the writers of the primary ran');
is($done[0][1] . $done[1][1], '', 'no statement of the primary or of its maintenance failed');
cmp_ok($done[2][0] + $done[3][0], '>', 0, 'the reads of the standbys that stream from a standby and the delayed one ran while the replay went on');
is($done[2][1] . $done[3][1], '', 'and what they got is a result or a conflict with the recovery, never another error');

catchup();
my $d0 = $primary->safe_psql('postgres', 'SELECT dg()');
my $answers = $primary->safe_psql('postgres', 'SELECT answers_off()');
for my $n (@nodes)
{
	my $name = $n->name;
	is($n->safe_psql('postgres', 'SELECT dg()'), $d0, "$name holds the rows of the primary");
	is($n->safe_psql('postgres', 'SELECT count(*) FROM sift.check(NULL, true)'), 0, "$name verifies");
	is($n->safe_psql('postgres', 'SELECT answers_on() = answers_off() AND answers_off() = ' . $primary->safe_psql('postgres', "SELECT quote_literal(answers_off())")), 't', "$name answers through the sift_col index as through the normal plan, as the primary does");
}
$primary->safe_psql('postgres', 'VACUUM (FREEZE) ev');
catchup();
for my $n (@nodes)
{
	cmp_ok($n->safe_psql('postgres', q{SELECT pg_relation_size('ev', 'vm')}), '>', 0, $n->name . ' has the visibility map of the frozen table');
}

# one transaction, replayed by the delayed standby no sooner than its delay after it was written; what it reads meanwhile is a whole state
$primary->safe_psql('postgres', q{
	BEGIN;
	INSERT INTO marker VALUES (clock_timestamp());
	UPDATE ev SET v = v + 1 WHERE id % 10 = 0;
	DELETE FROM ev WHERE id % 13 = 0;
	INSERT INTO ev SELECT g, 1, 1, 'tx' FROM generate_series(800001, 800200) g;
	UPDATE wb SET w = repeat('w', 40000) WHERE id = 1;
	UPDATE fc SET v = v + 5 WHERE id % 9 = 0;
	COMMIT;});
my $d1 = $primary->safe_psql('postgres', 'SELECT dg()');
my $written = $primary->safe_psql('postgres', 'SELECT extract(epoch FROM ts) FROM marker');
isnt($d1, $d0, 'the transaction changed what the tables hold');
my (%seen, $first);
until (defined $first)
{
	my ($state, $now) = split /\|\|/, $standby3->safe_psql('postgres', q{SELECT dg() || '||' || extract(epoch FROM clock_timestamp())});
	$seen{$state}++;
	$first = $now if $state ne $d0;
	die 'the delayed standby did not replay the transaction' if keys(%seen) > 2;
}
is(join(',', sort grep { $_ ne $d0 && $_ ne $d1 } keys %seen), '', 'every state the delayed standby read was the one before or the one after the transaction');
cmp_ok($first - $written, '>=', $delay, "the delayed standby replayed the transaction no sooner than its delay of ${delay}s after it was written");
is($standby1->safe_psql('postgres', 'SELECT dg()'), $d1, 'while standby1 had it already');

$primary->safe_psql('postgres', q{
	VACUUM (FREEZE) ev;
	VACUUM wb;
	SELECT count(*) FROM sift.maintain('ev');
	REINDEX INDEX fc_idx;
	DELETE FROM ev WHERE id > 800100;
	VACUUM ev;});
catchup();
my $d2 = $primary->safe_psql('postgres', 'SELECT dg()');
for my $n (@standbys)
{
	my $name = $n->name;
	is($n->safe_psql('postgres', 'SELECT dg()'), $d2, "$name holds the rows of the primary after VACUUM, compaction and REINDEX");
	is($n->safe_psql('postgres', 'SELECT count(*) FROM sift.check(NULL, true)') . $n->safe_psql('postgres', 'SELECT answers_on() = answers_off()'), '0t', "$name verifies and answers as the normal plan");
}

my $log = join '', map { slurp_file($_->logfile) } @nodes;
unlike($log, qr/inconsistent page|TRAP|PANIC|terminated by signal/, 'no record of any node failed its page check, and nothing crashed');
done_testing();
