# Logical replication of a sift_table. shared_preload_libraries is needed only to publish one (the custom resource manager of
# its change records can only be registered at preload time): an unpublished table works without it, a published one fails
# with 0A000 and an actionable hint, and with pg_sift preloaded its inserts, updates and deletes reach the subscriber.
use strict;
use warnings FATAL => 'all';
use FindBin;
use lib $FindBin::RealBin;
use SiftTest;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tbl = 'CREATE TABLE orders (id int PRIMARY KEY, store text, total numeric, note text)';

# ---------- no preload ----------
my $plain = sift_node('plain', allows_streaming => 'logical');
$plain->start;
install_sift($plain);

subtest 'without preload an unpublished table works under wal_level = logical' => sub {
	$plain->safe_psql('postgres', "$tbl USING sift_table");
	$plain->safe_psql('postgres', q{
		INSERT INTO orders SELECT g, 's' || g % 3, g, 'n' FROM generate_series(1, 100) g;
		UPDATE orders SET note = 'u' WHERE id % 2 = 0;
		DELETE FROM orders WHERE id % 10 = 0;
		TRUNCATE orders;
		INSERT INTO orders VALUES (1, 's', 1, 'after');
	});
	is($plain->safe_psql('postgres', 'SELECT count(*) FROM orders'), 1, 'DML and TRUNCATE work');
};

subtest 'without preload a published table raises 0A000 with the way out' => sub {
	$plain->safe_psql('postgres', 'CREATE PUBLICATION pub FOR TABLE orders');
	for my $dml ("INSERT INTO orders VALUES (2, 's', 2, 'x')", "UPDATE orders SET note = 'y' WHERE id = 1", "DELETE FROM orders WHERE id = 1")
	{
		my ($ret, $out, $err) = $plain->psql('postgres', $dml, extra_params => [ '-v', 'VERBOSITY=verbose' ]);
		isnt($ret, 0, "$dml fails");
		like($err, qr/ERROR:  0A000:/, 'SQLSTATE 0A000');
		like($err, qr/ERROR:  0A000: sift_table table "orders" is published/, 'names the table');
		like($err, qr/shared_preload_libraries/, 'names the setting');
		like($err, qr/heap staging table/, 'offers the staging table');
	}
	is($plain->safe_psql('postgres', 'SELECT count(*) FROM orders'), 1, 'nothing was written');
	$plain->safe_psql('postgres', 'DROP PUBLICATION pub');
	$plain->safe_psql('postgres', "INSERT INTO orders VALUES (3, 's', 3, 'unpublished again')");
	is($plain->safe_psql('postgres', 'SELECT count(*) FROM orders'), 2, 'writes work once the table is no longer published');
};
$plain->stop;

# ---------- preload: publisher and subscriber ----------
my $pub = sift_node('publisher', allows_streaming => 'logical');
$pub->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_sift'");
$pub->start;
install_sift($pub);
$pub->safe_psql('postgres', "$tbl USING sift_table; CREATE PUBLICATION pub FOR TABLE orders");

my $sub = sift_node('subscriber');
$sub->start;
install_sift($sub);
$sub->safe_psql('postgres', $tbl);
$sub->safe_psql('postgres', "CREATE SUBSCRIPTION sub CONNECTION \$\$" . $pub->connstr('postgres') . "\$\$ PUBLICATION pub");
$sub->wait_for_subscription_sync($pub, 'sub');

sub same
{
	my ($what) = @_;
	$sub->wait_for_subscription_sync($pub, 'sub');
	$pub->wait_for_catchup('sub');
	my $q = "SELECT count(*) || ':' || coalesce(md5(string_agg(id || '/' || store || '/' || total || '/' || coalesce(note, '-'), ',' ORDER BY id)), '') FROM orders";
	is($sub->safe_psql('postgres', $q), $pub->safe_psql('postgres', $q), "$what: the subscriber has the publisher's rows");
}

subtest 'inserts, updates and deletes replicate' => sub {
	$pub->safe_psql('postgres', "INSERT INTO orders SELECT g, 's' || g % 4, g * 1.5, 'n' || g FROM generate_series(1, 300) g");
	same('insert');
	$pub->safe_psql('postgres', "UPDATE orders SET total = total + 1, note = NULL WHERE id % 7 = 0");
	same('update');
	$pub->safe_psql('postgres', "UPDATE orders SET id = id + 1000 WHERE id % 50 = 0");
	same('update of the key');
	$pub->safe_psql('postgres', "DELETE FROM orders WHERE id % 11 = 0");
	same('delete');
	$pub->safe_psql('postgres', "INSERT INTO orders VALUES (9000, 'big', 1, repeat('x', 100000))");
	same('a row far wider than a page');
};

subtest 'rolled back work and subtransactions do not replicate' => sub {
	$pub->safe_psql('postgres', q{
		BEGIN; INSERT INTO orders VALUES (9100, 'r', 1, 'rolled back'); UPDATE orders SET note = 'rb' WHERE id = 1; ROLLBACK;
		BEGIN; INSERT INTO orders VALUES (9101, 'k', 1, 'kept');
		SAVEPOINT s; INSERT INTO orders VALUES (9102, 'd', 1, 'dropped'); DELETE FROM orders WHERE id = 2; ROLLBACK TO s;
		INSERT INTO orders VALUES (9103, 'k', 1, 'kept too'); COMMIT;
		INSERT INTO orders VALUES (9104, 'c', 1, 'x') ON CONFLICT (id) DO NOTHING;
		INSERT INTO orders VALUES (9104, 'c', 1, 'y') ON CONFLICT (id) DO UPDATE SET note = excluded.note;
		INSERT INTO orders VALUES (9104, 'c', 1, 'z') ON CONFLICT (id) DO NOTHING;
	});
	same('transaction control');
	is($sub->safe_psql('postgres', 'SELECT count(*) FROM orders WHERE id IN (9100, 9102)'), 0, 'rolled back rows are absent');
	is($sub->safe_psql('postgres', 'SELECT note FROM orders WHERE id = 9104'), 'y', 'ON CONFLICT replicated as its net effect');
};

subtest 'TRUNCATE replicates' => sub {
	$pub->safe_psql('postgres', "TRUNCATE orders; INSERT INTO orders VALUES (1, 'a', 1, 'fresh')");
	same('truncate');
};

subtest 'a sift_table subscriber applies the changes' => sub {
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION sub DISABLE');
	$sub->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'");
	$sub->safe_psql('postgres', 'ALTER TABLE orders SET ACCESS METHOD sift_table; ALTER SUBSCRIPTION sub ENABLE');
	$pub->safe_psql('postgres', q{
		INSERT INTO orders SELECT g, 's', g, 'm' FROM generate_series(100, 400) g;
		UPDATE orders SET note = 'upd' WHERE id % 3 = 0;
		DELETE FROM orders WHERE id % 5 = 0;
	});
	same('sift_table subscriber');
	is($sub->safe_psql('postgres', "SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname = 'orders'"), 'sift_table', 'the subscriber table is a sift_table');
};

subtest 'a physical standby of the publisher needs the same preload to replay' => sub {
	$pub->backup('base');
	my $standby = PostgreSQL::Test::Cluster->new('pub_standby');
	$standby->init_from_backup($pub, 'base', has_streaming => 1);
	$standby->start;
	$pub->safe_psql('postgres', "INSERT INTO orders VALUES (7000, 'p', 1, 'standby')");
	$pub->wait_for_catchup($standby);
	is($standby->safe_psql('postgres', 'SELECT note FROM orders WHERE id = 7000'), 'standby', 'the standby replays the change records');
	$standby->stop;
};

$sub->stop;
$pub->stop;
done_testing();
