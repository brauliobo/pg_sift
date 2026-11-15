# The dimension map (sift.agg_dimmap_kb): the classes of the keys of a joined table, kept in shared memory. A backend killed while it reads a map takes the server
# through crash recovery, which discards the segments; a budget too small for a map keeps none and the answers stay those of the normal plan; sessions that ask for
# the same map at once make it once; a hot standby makes and reads its own and gives the primary's answers.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use Test::More;

my $sessions = 12;
my $node = sift_node('dimmap', allows_streaming => 1);
$node->append_conf('postgresql.conf', "sift.agg_lookup_cache_kb = 0\nsift.agg_lookup_shared_kb = 0\nmax_parallel_workers_per_gather = 0\nmax_connections = 40\nshared_buffers = 256MB");
$node->start;
install_sift($node);
$node->safe_psql('postgres', <<'SQL');
CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 30000, i % 100 FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);
CREATE TABLE dim (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 9, i % 4 FROM generate_series(0, 29999) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b);
VACUUM (FREEZE, ANALYZE) ev, dim;
CREATE FUNCTION maps(q text, OUT shared int, OUT built int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	shared := 0; built := 0;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '(\d+) maps shared' THEN shared := substring(l from '(\d+) maps shared')::int; END IF;
		IF l ~ '(\d+) maps built' THEN built := substring(l from '(\d+) maps built')::int; END IF;
	END LOOP;
END $$;
SQL

my $q = 'SELECT d.a, d.b, count(*) AS n, sum(ev.v) AS s FROM ev JOIN dim d ON d.k = ev.k GROUP BY 1, 2';
my $sum = "SELECT md5(string_agg(a || '/' || b || '/' || n || '/' || s, ',' ORDER BY a, b)) FROM ($q) x";
my $maps = sub { my ($n, $s, $b) = @_; join ',', split /\|/, $n->safe_psql('postgres', "SELECT shared, built FROM maps(\$\$$q\$\$)") };
my $expected = $node->safe_psql('postgres', "SET sift.enable_agg = off; $sum");

is($maps->($node), '0,1', 'the first query reads the leaves and makes the map');
is($maps->($node), '1,0', 'and the next one, of another backend, reads it');
is($node->safe_psql('postgres', $sum), $expected, 'the answer is the normal plan\'s');

# a backend that is killed while a cursor holds the segment mapped
crash_during($node, "BEGIN; DECLARE c CURSOR FOR $q; FETCH 1 FROM c; SELECT pg_sleep(60)", 300);
is($maps->($node), '0,1', 'crash recovery discarded the segments: the map is made again');
is($maps->($node), '1,0', 'and shared again');
is($node->safe_psql('postgres', $sum), $expected, 'the answer is the normal plan\'s after the restart');

# a write moves the pages of the table: no map until it is vacuumed
$node->safe_psql('postgres', 'UPDATE dim SET a = 5 WHERE k = 7');
is($maps->($node), '0,0', 'a table written to has no map');
$node->safe_psql('postgres', 'VACUUM (INDEX_CLEANUP ON) dim');
is($maps->($node), '0,1', 'and once vacuumed, a map of the new pages is made');
is($node->safe_psql('postgres', $sum), $node->safe_psql('postgres', "SET sift.enable_agg = off; $sum"), 'the answer is the normal plan\'s after the write');

# a budget the map does not fit: nothing is kept and every query reads the classes itself
$node->safe_psql('postgres', 'UPDATE dim SET b = 3 WHERE k = 9; VACUUM (INDEX_CLEANUP ON) dim');
$node->safe_psql('postgres', 'ALTER SYSTEM SET sift.agg_dimmap_kb = 16');
$node->reload;
is($maps->($node), '0,1', 'a map larger than the budget is made for the query');
is($maps->($node), '0,1', 'and not kept: the next query makes it again');
is($node->safe_psql('postgres', $sum), $node->safe_psql('postgres', "SET sift.enable_agg = off; $sum"), 'the answer is the normal plan\'s');
$node->safe_psql('postgres', 'ALTER SYSTEM RESET sift.agg_dimmap_kb');
$node->reload;

# sessions that ask at the same moment: one makes the map
$node->safe_psql('postgres', 'UPDATE dim SET b = 2 WHERE k = 11; VACUUM (INDEX_CLEANUP ON) dim');
my $expected2 = $node->safe_psql('postgres', "SET sift.enable_agg = off; $sum");
my $gate = $node->background_psql('postgres');
$gate->query_safe('SELECT pg_advisory_lock(7301)');
my @s = map { $node->background_psql('postgres') } 1 .. $sessions;
for my $c (@s)
{
	$c->query_until(qr/waiting/, "\\echo waiting\nSELECT pg_advisory_lock_shared(7301), pg_advisory_unlock_shared(7301);\n"
		. "SELECT built FROM maps(\$\$$q\$\$) \\gset\nSELECT ($sum) AS answer \\gset\n");
}
$node->poll_query_until('postgres', "SELECT count(*) = $sessions FROM pg_locks WHERE locktype = 'advisory' AND NOT granted");
$gate->query_safe('SELECT pg_advisory_unlock(7301)');
my ($made, $wrong) = (0, 0);
for my $c (@s)
{
	my ($built, $answer) = split /\|/, (split /\n/, $c->query_safe('SELECT :built, :\'answer\''))[-1];
	$made += $built;
	$wrong += $answer ne $expected2;
}
is($made, 1, "of $sessions sessions that ask for the same map at once, one makes it");
is($wrong, 0, 'and every session gets the answer of the normal plan');

# a hot standby makes and reads its own map
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('dimmap_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;
$node->safe_psql('postgres', 'UPDATE dim SET a = 8 WHERE k = 13; VACUUM (INDEX_CLEANUP ON) dim');
$node->wait_for_catchup($standby);
my $on_primary = $node->safe_psql('postgres', $sum);
is($standby->safe_psql('postgres', $sum), $on_primary, 'the standby gives the primary\'s answer');
my $on_standby = join ',', split /\|/, $standby->safe_psql('postgres', "SELECT shared, built FROM maps(\$\$$q\$\$)");
ok($on_standby eq '1,0' || $on_standby eq '0,1' || $on_standby eq '0,0', "the standby reads or makes a map or reads the table ($on_standby)");
is($standby->safe_psql('postgres', $sum), $on_primary, 'and its next answer is the same');

$standby->stop;
$node->stop;
done_testing();
