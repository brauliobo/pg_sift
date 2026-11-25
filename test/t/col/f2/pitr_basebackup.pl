# Point-in-time recovery of format major 2 from an online base backup. A primary with WAL archiving and wal_consistency_checking = 'all' (every record of redo is compared with the page
# it was logged from) takes pg_basebackup while pgbench changes a sift_table (column deltas, inserts, deletes, wide values in extents of their own) and a heap table with a sift_col index
# (a projection and a scope), and while VACUUM, sift.maintain and REINDEX run beside the writers. The writers stop, the LSN, the time and the digest of the rows are noted, the work goes on
# past them (deletes, VACUUM (FREEZE), compaction, REINDEX), and the primary is crashed. A clone restored from the base backup to that LSN, and another to that time, hold exactly the
# digest of the note, answer the same through the sift_col index and through the normal plan, and sift.check finds nothing; both take writes and VACUUM after they promote. The crash recovery of
# the primary replays all of it under the same page checks.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib "$ENV{SIFT_REPO}/test/t/lib";
use SiftTest;
use IPC::Run ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $port = $ENV{SIFT_PORT} // 56100;
my $secs = knob('SIFT_PITR_SECS', 4, 2);

sub node
{
	my ($name, $offset) = @_;
	return PostgreSQL::Test::Cluster->new($name, port => $port + $offset);
}

my $primary = node('primary', 1);
$primary->init(allows_streaming => 1);
$primary->enable_archiving;
$primary->append_conf('postgresql.conf', join("\n",
	"shared_preload_libraries = 'pg_sift'", "sift.test_create_format1 = off", "wal_consistency_checking = 'all'", "autovacuum = off",
	"shared_buffers = 64MB", "restart_after_crash = off"));
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
	CREATE TABLE sp (dim int NOT NULL, tenant text NOT NULL, PRIMARY KEY (dim, tenant));
	INSERT INTO sp SELECT g, 'tenant-' || g % 9 FROM generate_series(1, 300) g;
	CREATE INDEX sp_j ON sp USING sift_col (dim, tenant) WITH (track_changes = 'ctid');
	CREATE TABLE sv (id int PRIMARY KEY, dim int NOT NULL, amount int NOT NULL);
	INSERT INTO sv SELECT g, 1 + g % 320, g % 100 FROM generate_series(1, 6000) g;
	ANALYZE sv;
	CREATE INDEX sv_a ON sv USING sift_col (id, dim, amount) WITH (scope = 'tenant: dim -> sp(dim, tenant)');
	CREATE FUNCTION dg() RETURNS text LANGUAGE sql AS $$ SELECT concat_ws('|',
		(SELECT count(*) || ':' || sum(v) || ':' || md5(string_agg(id || ',' || k || ',' || v || ',' || coalesce(tag, ''), ';' ORDER BY id)) FROM ev),
		(SELECT count(*) || ':' || md5(string_agg(id || ',' || md5(w), ';' ORDER BY id)) FROM wb),
		(SELECT count(*) || ':' || sum(v) || ':' || md5(string_agg(id || ',' || g || ',' || v, ';' ORDER BY id)) FROM fc),
		(SELECT count(*) || ':' || sum(amount) FROM sv)) $$;
	CREATE FUNCTION answers() RETURNS text LANGUAGE sql AS $$ SELECT concat_ws('|',
		(SELECT string_agg(g || ':' || c || ':' || s, ',' ORDER BY g) FROM (SELECT g, count(*) c, sum(v) s FROM fc GROUP BY g) x),
		(SELECT count(*) || ':' || coalesce(sum(amount), 0) FROM sv WHERE dim IN (SELECT dim FROM sp WHERE tenant = 'tenant-3'))) $$;
	CREATE FUNCTION answers_on() RETURNS text LANGUAGE sql SET sift.enable_agg = on SET cpu_tuple_cost = 0.5 RETURN answers();
	CREATE FUNCTION answers_off() RETURNS text LANGUAGE sql SET sift.enable_agg = off RETURN answers();
	VACUUM (FREEZE) ev;
	VACUUM (FREEZE) wb;});
cmp_ok($primary->safe_psql('postgres', q{SELECT pg_relation_size('ev', 'vm')}), '>', 0, 'VACUUM (FREEZE) set the visibility map of the table');

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
UPDATE sv SET amount = amount + 1 WHERE id = :fid;
INSERT INTO fc VALUES (100000 + :fid, :fid % 17, 1) ON CONFLICT DO NOTHING;},
	maint => q{VACUUM ev;
VACUUM fc;
VACUUM wb;
SELECT count(*) FROM sift.maintain('ev');
SELECT count(*) FROM sift.maintain('fc_idx');
REINDEX INDEX fc_idx;
REINDEX INDEX sv_a;},
);
for my $name (keys %script) { PostgreSQL::Test::Utils::append_to_file("$dir/$name.sql", "$script{$name}\n") }

# pgbench in the background: the workers do each of the files, the maintenance session runs one statement of maint.sql at a time
sub bench
{
	my ($node, $seconds) = @_;
	my @common = ('pgbench', '-n', '-T', $seconds);
	my (@out, @err, @h);
	local $ENV{PGHOST} = $node->host;
	local $ENV{PGPORT} = $node->port;
	local $ENV{PGAPPNAME} = 'pgbench';
	push @h, IPC::Run::start([ @common, '-c', 3, '-j', 2, '-f', "$dir/upd.sql\@5", '-f', "$dir/ins.sql\@2", '-f', "$dir/wide.sql\@1", '-f', "$dir/heap.sql\@3", 'postgres' ], '>', \$out[0], '2>', \$err[0]);
	push @h, IPC::Run::start([ @common, '-c', 1, '-f', "$dir/maint.sql", '-M', 'simple', 'postgres' ], '>', \$out[1], '2>', \$err[1]);
	return sub
	{
		my @status;
		for my $i (0 .. $#h)
		{
			my $done = $h[$i]->finish;
			my ($n) = $out[$i] =~ /number of transactions actually processed: (\d+)/;
			push @status, $done && ($n // 0) > 0 && $out[$i] =~ /number of failed transactions: 0 / ? 'ok' : "failed: $err[$i]$out[$i]";
		}
		return join('; ', @status);
	};
}

# the online base backup while the writers and the maintenance work
my $writers = bench($primary, $secs + 4);
$primary->poll_query_until('postgres', q{SELECT count(*) >= 4 FROM pg_stat_activity WHERE application_name = 'pgbench'}) or die 'the clients did not connect';
$primary->backup('base');
is($primary->safe_psql('postgres', q{SELECT count(*) FROM pg_stat_activity WHERE application_name = 'pgbench'}), 4, 'the four sessions of pgbench were still running when pg_basebackup ended');
is($writers->(), 'ok; ok', 'pgbench ran its writers and its maintenance to the end beside the online pg_basebackup, no transaction failed');
is($primary->safe_psql('postgres', 'SELECT count(*) FROM sift.check(NULL, true)'), 0, 'the primary verifies after the base backup');

# the note: quiet, so that the digest is the one of every commit before the LSN and the time
my $lsn = $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my $time = $primary->safe_psql('postgres', 'SELECT clock_timestamp()');
my $mid = $primary->safe_psql('postgres', 'SELECT dg()');
my $mid_answers = $primary->safe_psql('postgres', 'SELECT answers_off()');
is($primary->safe_psql('postgres', 'SELECT answers_on()'), $mid_answers, 'the answers of the sift_col indexes equal those of the normal plan at the note');

# the work past the note: writers, deletes, VACUUM (FREEZE) and compaction of the table, the maintenance and REINDEX of the indexes
$writers = bench($primary, $secs);
is($writers->(), 'ok; ok', 'the writers and the maintenance ran past the note');
$primary->safe_psql('postgres', q{
	DELETE FROM ev WHERE id % 3 = 0;
	UPDATE wb SET w = repeat('z', 20000) WHERE id % 2 = 0;
	VACUUM (FREEZE) ev;
	VACUUM wb;
	VACUUM fc;
	SELECT count(*) FROM sift.maintain('ev');
	SELECT count(*) FROM sift.maintain('sv_a');
	REINDEX INDEX fc_idx;
	REINDEX INDEX sv_a;});
my $end = $primary->safe_psql('postgres', 'SELECT dg()');
isnt($end, $mid, 'the rows past the note are not those of the note');
my $walfile = $primary->safe_psql('postgres', 'SELECT pg_walfile_name(pg_switch_wal())');
$primary->poll_query_until('postgres', "SELECT '$walfile' <= last_archived_wal FROM pg_stat_archiver") or die 'the WAL was not archived';

# crash recovery of the primary: all of the work above is replayed under wal_consistency_checking = 'all'
$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', 'SELECT dg()'), $end, 'crash recovery replayed every change under the page checks: the rows are those before the crash');
is($primary->safe_psql('postgres', 'SELECT count(*) FROM sift.check(NULL, true)'), 0, 'and the primary verifies');
is($primary->safe_psql('postgres', 'SELECT answers_on() = answers_off()'), 't', 'the sift_col indexes answer as the normal plan after the crash recovery');
$primary->stop;

# the clones: the base backup, the archive, a target
sub restore
{
	my ($name, $offset, $target) = @_;
	my $clone = node($name, $offset);
	$clone->init_from_backup($primary, 'base', has_restoring => 1);
	$clone->append_conf('postgresql.conf', "$target\nrecovery_target_inclusive = off\nrecovery_target_action = 'promote'\nwal_consistency_checking = 'all'");
	$clone->start;
	$clone->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()') or die "$name did not promote";
	return $clone;
}

my %by = (lsn => "recovery_target_lsn = '$lsn'", time => "recovery_target_time = '$time'");
my $offset = 2;
my @logs = ($primary->logfile);
for my $kind (qw(lsn time))
{
	my $clone = restore("pitr_$kind", $offset++, $by{$kind});
	push @logs, $clone->logfile;
	ok($clone->log_contains(qr/recovery stopping (?:before|after) /), "the clone to the $kind stopped at the target");
	is($clone->safe_psql('postgres', 'SELECT dg()'), $mid, "the clone to the $kind holds the rows of the note");
	is($clone->safe_psql('postgres', 'SELECT count(*) FROM sift.check(NULL, true)'), 0, "the clone to the $kind verifies");
	is($clone->safe_psql('postgres', 'SELECT answers_on() = answers_off()'), 't', "the sift_col indexes of the clone to the $kind answer as the normal plan");
	is($clone->safe_psql('postgres', 'SELECT answers_off()'), $mid_answers, "and as the primary did at the note");
	$clone->safe_psql('postgres', q{
		UPDATE ev SET v = v + 1 WHERE id % 5 = 0;
		INSERT INTO ev SELECT g, 1, 1, 'p' FROM generate_series(900001, 900100) g;
		DELETE FROM ev WHERE id % 7 = 0;
		UPDATE wb SET w = repeat('q', 30000) WHERE id = 1;
		VACUUM (FREEZE) ev;
		VACUUM wb;
		SELECT count(*) FROM sift.maintain('ev');
		REINDEX INDEX fc_idx;});
	is($clone->safe_psql('postgres', 'SELECT count(*) FROM sift.check(NULL, true)'), 0, "the clone to the $kind takes writes, VACUUM and REINDEX, and verifies");
	$clone->stop;
}

my $log = join '', map { slurp_file($_) } @logs;
unlike($log, qr/inconsistent page|TRAP|PANIC|terminated by signal/, 'no record of the primary or of a clone failed its page check, and nothing crashed');
done_testing();
