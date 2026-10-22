# The rows of a joined table are shared by default (sift.agg_lookup_shared_kb = -1): 16 sessions that run the same query at the same moment load the
# table once, the others wait for the rows and read them in place, and every session gets the answer of the normal plan.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use Test::More;

my $sessions = 16;
my $node = sift_node('lookup_shared_concurrent');
$node->append_conf('postgresql.conf', "shared_buffers = 512MB\nmax_parallel_workers_per_gather = 0\nseq_page_cost = 10000\nrandom_page_cost = 10000\nmax_connections = 40");
$node->start;
install_sift($node);
$node->safe_psql('postgres', <<'SQL');
CREATE TABLE ev (id int PRIMARY KEY, entity_id int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, 1 + i % 40000, i % 100 FROM generate_series(1, 80000) i;
CREATE INDEX ev_col ON ev USING sift_col (entity_id, amount);
CREATE TABLE ents (id int, label text) WITH (autovacuum_enabled = false);
INSERT INTO ents SELECT i, 'e' || i % 50 FROM generate_series(1, 40000) i;
VACUUM (FREEZE, ANALYZE) ev;
VACUUM (FREEZE, ANALYZE) ents;
CREATE FUNCTION cached(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; kept int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Cached Lookup Tables:%' THEN kept := kept + trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	RETURN kept;
END $$;
SQL

my $q = 'SELECT e.label, count(*) AS n, sum(amount) AS s FROM ev JOIN ents e ON e.id = ev.entity_id GROUP BY 1';
my $sum = "SELECT md5(string_agg(label || n || s, ',' ORDER BY label)) FROM ($q) x";
my $expected = $node->safe_psql('postgres', "SET sift.enable_agg = off; $sum");

# every session waits on the lock of the gate and runs the query when it is released
my $gate = $node->background_psql('postgres');
$gate->query_safe('SELECT pg_advisory_lock(7201)');
my @s = map { $node->background_psql('postgres') } 1 .. $sessions;
for my $c (@s)
{
	$c->query_until(qr/waiting/, "\\echo waiting\nSELECT pg_advisory_lock_shared(7201), pg_advisory_unlock_shared(7201);\n"
		. "SELECT cached(\$\$$q\$\$) AS kept \\gset\nSELECT ($sum) AS answer \\gset\n");
}
$node->poll_query_until('postgres', "SELECT count(*) = $sessions FROM pg_locks WHERE locktype = 'advisory' AND NOT granted");
$gate->query_safe('SELECT pg_advisory_unlock(7201)');

my ($loaded, $wrong) = (0, 0);
for my $c (@s)
{
	# (the output of the statements that waited for the gate comes first)
	my ($kept, $answer) = split /\|/, (split /\n/, $c->query_safe('SELECT :kept, :\'answer\''))[-1];
	$loaded += $kept == 0;
	$wrong += $answer ne $expected;
}
is($loaded, 1, "of $sessions sessions that ask for the same lookup at once, one loads it");
is($wrong, 0, 'and every session gets the answer of the normal plan');
is($node->safe_psql('postgres', 'SELECT cached($$' . $q . '$$)'), '1', 'a later session reads the rows from the shared memory');

# a write: the rows of the old state are not read, and the sessions that ask again at once load the new state once
$node->safe_psql('postgres', 'UPDATE ents SET label = $$moved$$ WHERE id = 7; VACUUM ents');
$expected = $node->safe_psql('postgres', "SET sift.enable_agg = off; $sum");
$gate->query_safe('SELECT pg_advisory_lock(7202)');
for my $c (@s)
{
	$c->query_until(qr/again/, "\\echo again\nSELECT pg_advisory_lock_shared(7202), pg_advisory_unlock_shared(7202);\n"
		. "SELECT cached(\$\$$q\$\$) AS kept \\gset\nSELECT ($sum) AS answer \\gset\n");
}
$node->poll_query_until('postgres', "SELECT count(*) = $sessions FROM pg_locks WHERE locktype = 'advisory' AND NOT granted");
$gate->query_safe('SELECT pg_advisory_unlock(7202)');
($loaded, $wrong) = (0, 0);
for my $c (@s)
{
	# (the output of the statements that waited for the gate comes first)
	my ($kept, $answer) = split /\|/, (split /\n/, $c->query_safe('SELECT :kept, :\'answer\''))[-1];
	$loaded += $kept == 0;
	$wrong += $answer ne $expected;
}
# no session finds the new state, in its own cache or the shared memory: one loads it, the others wait for it
is($loaded, 1, 'the rows of the changed table are loaded once, not once for each session');
is($wrong, 0, 'and the answers are those of the changed table');

$_->quit for (@s, $gate);
$node->stop;
done_testing();
