# Helpers of the compatibility TAP tests (031_v51_full_compat.pl, 032_v6_dump_restore_tablespace.pl): the digest of
# a query and the format of a relation, and one server directory tree whose pg_sift library and SQL are swapped between
# two releases while the data directory stays.
#
# The tree is the server of pg_config copied to its own absolute paths under a scratch root (the binaries are real
# copies, so that the server finds its library and share directories relative to itself) with every other file of the
# install linked in. pg_sift is then the one thing that is replaced: install_sift_version() puts the library and the
# extension files of a staged root (a DESTDIR install: ROOT/<pkglibdir>/pg_sift.so, ROOT/<sharedir>/extension/pg_sift*)
# in it. A root without a library leaves the server's own pg_sift in place (PostgreSQL 17 stages nothing).
package SiftCompat;

use strict;
use warnings FATAL => 'all';
use Exporter 'import';
use File::Copy qw(copy);
use File::Path qw(make_path);
use File::Basename qw(basename);
use FindBin ();
use SiftTest ();
use Test::More;

our @EXPORT = qw(v510_root v60_root make_server_tree install_sift_version pg_major sql digest format_of fixture_sql dml snapshot agree_with_heap formats filenodes vacuum_all
	@SIFT_COL @TABLES @FORMAT1 $BM25 @ALL_DML %HEAP_QUERIES);

my $ROWS = $ENV{SIFT_COMPAT_ROWS} || 6000;
my $pg_config = $ENV{PG_CONFIG} || 'pg_config';
my @BINARIES = qw(postgres initdb pg_ctl pg_controldata pg_resetwal pg_basebackup pg_dump pg_dumpall pg_restore
	psql pg_isready pg_checksums vacuumdb pg_waldump pg_rewind createdb dropdb);

sub pgc
{
	my ($what) = @_;
	my $v = `$pg_config --$what`;
	die "$pg_config --$what failed\n" if $? != 0;
	chomp $v;
	return $v;
}

sub sql { my ($node, $q) = @_; return $node->safe_psql('postgres', $q) }

# rows:md5 of the rows of query $q in any order; $heap_only reads them without sift (the heap's answer)
sub digest
{
	my ($node, $q, $heap_only) = @_;
	my $pre = $heap_only
		? 'SET sift.enable_agg = off; SET sift.enable_col_scan = off; SET enable_indexscan = off; SET enable_bitmapscan = off; '
		: '';
	return sql($node, "${pre}SELECT count(*) || ':' || coalesce(md5(string_agg(t::text, E'\\n' ORDER BY t::text)), '-') FROM ($q) t");
}

sub format_of { my ($node, $rel) = @_; return sql($node, "SELECT major || '.' || minor FROM sift.index_format('$rel')") }

sub pg_major { return (pgc('version') =~ /(\d+)/)[0] }

# The staged root of the v5.1.0 build (scripts/build-v510-fixture.sh), or undef and the reason
sub v510_root
{
	my $root  = $ENV{SIFT_V510_LIBDIR} || '/tmp/sift-v510-lib';
	my $build = ($ENV{SIFT_REPO} || "$FindBin::RealBin/../..") . '/scripts/build-v510-fixture.sh';
	if ($ENV{SIFT_V510_BUILD} && -x $build)
	{
		system($build, $root) == 0 or return (undef, "the build of v5.1.0 failed ($build)");
	}
	return (undef, "no v5.1.0 library in $root: run scripts/build-v510-fixture.sh (SIFT_V510_LIBDIR names another root, SIFT_V510_BUILD=1 builds it here)")
		unless -f "$root/.fixture";
	open my $fh, '<', "$root/.fixture" or die "$root/.fixture: $!";
	my $stamp = <$fh>;
	close $fh;
	chomp $stamp;
	my $major = pg_major();
	return (undef, "$root holds a v5.1.0 build for another PostgreSQL major ($stamp, this server is $major): rebuild it with scripts/build-v510-fixture.sh --force")
		unless $stamp =~ /\bpg$major$/;
	return ($root, undef);
}

# The staged root of the library under test: SIFT_V60_LIBDIR, else the root of scripts/pgtest.sh (SIFT_ROOT)
sub v60_root { return $ENV{SIFT_V60_LIBDIR} || $ENV{SIFT_ROOT} }

# make_server_tree(DEST): the server of pg_config under DEST; returns the directory of its binaries
sub make_server_tree
{
	my ($dest) = @_;
	my ($bin, $lib, $share) = (pgc('bindir'), pgc('pkglibdir'), pgc('sharedir'));
	make_path("$dest$bin", "$dest$lib", "$dest$share/extension");
	for my $f (@BINARIES)
	{
		next unless -x "$bin/$f";
		copy("$bin/$f", "$dest$bin/$f") or die "copy $bin/$f: $!";
		chmod 0755, "$dest$bin/$f";
	}
	link_entries($lib,             "$dest$lib");
	link_entries("$share/extension", "$dest$share/extension");
	for my $f (glob("$share/*"))
	{
		next if basename($f) eq 'extension';
		symlink($f, "$dest$share/" . basename($f)) or die "symlink $f: $!";
	}
	return "$dest$bin";
}

sub link_entries
{
	my ($from, $to) = @_;
	for my $f (glob("$from/*"))
	{
		symlink($f, "$to/" . basename($f)) or die "symlink $f: $!";
	}
}

# install_sift_version(DEST, ROOT): the pg_sift library and extension files of ROOT in the tree of make_server_tree
sub install_sift_version
{
	my ($dest, $root) = @_;
	my ($lib, $share) = (pgc('pkglibdir'), pgc('sharedir'));
	unlink glob("$dest$lib/pg_sift.*"), glob("$dest$share/extension/pg_sift*");
	my @libs = $root ? glob("$root$lib/pg_sift.*") : ();
	if (@libs)
	{
		copy($_, "$dest$lib/" . basename($_)) or die "copy $_: $!" for @libs;
		copy($_, "$dest$share/extension/" . basename($_)) or die "copy $_: $!" for glob("$root$share/extension/pg_sift*");
		return;
	}
	symlink($_, "$dest$lib/" . basename($_)) or die "symlink $_: $!" for glob("$lib/pg_sift.*");
	symlink($_, "$dest$share/extension/" . basename($_)) or die "symlink $_: $!" for glob("$share/extension/pg_sift*");
}

# ------------------------------------------------------------------ the fixture the compatibility tests share

our @SIFT_COL = qw(ev_col ev_trk pairs_journal sc_idx);
our @TABLES   = qw(st st_vf);
our @FORMAT1  = (@SIFT_COL, @TABLES);
our $BM25     = 'docs_body';

# queries over the heap tables with sift_col indexes: the answer through the index, and through the heap alone
our %HEAP_QUERIES = (
	ev_group    => q{SELECT grp, count(*), sum(v), min(d), max(d), sum(amount) FROM ev GROUP BY grp},
	ev_range    => q{SELECT count(*), sum(v), avg(amount)::numeric(12,4) FROM ev WHERE d >= date '2026-02-01' AND d < date '2026-04-01'},
	ev_json     => q{SELECT doc->>'s', count(*), sum((doc->>'k')::int) FROM ev GROUP BY 1},
	ev_distinct => q{SELECT grp, count(DISTINCT v) FROM ev GROUP BY grp},
	ev_tracked  => q{SELECT grp, count(*) FROM ev WHERE v < 10 GROUP BY grp},
	sc_tenant   => q{SELECT p.tenant, count(*), sum(e.v) FROM sc_ev e JOIN pairs p ON p.dim_a_id = e.dim_a_id GROUP BY p.tenant},
	sc_kind     => q{SELECT kind, count(*), sum(v) FROM sc_ev GROUP BY kind},
);

# queries over a sift_table and its heap twin (%T%)
my %TABLE_QUERIES = (
	st_group => q{SELECT grp, count(*), sum(v), max(amt) FROM %T% GROUP BY grp},
	st_point => q{SELECT id, grp, v, s FROM %T% WHERE id IN (3, 17, 250, 1999, 5001)},
	st_like  => q{SELECT count(*), sum(v) FROM %T% WHERE s LIKE 'row1%'},
	st_sec   => q{SELECT id FROM %T% WHERE grp = 4 AND v > 50},
	st_wide  => q{SELECT count(*), sum(length(s)) FROM %T% WHERE length(s) > 4000},
);

# BM25 queries and their heap oracle (the words are lowercase tokens separated by a blank)
my %BM25_QUERIES = (
	bm_and => [q{SELECT id FROM docs WHERE body @@> 'w17 AND beta'},
			   q{SELECT id FROM docs WHERE string_to_array(body, ' ') @> ARRAY['w17', 'beta']}],
	bm_or  => [q{SELECT id FROM docs WHERE body @@> 'z3 OR w5'},
			   q{SELECT id FROM docs WHERE string_to_array(body, ' ') && ARRAY['z3', 'w5']}],
	bm_not => [q{SELECT id FROM docs WHERE body @@> 'common AND NOT beta'},
			   q{SELECT id FROM docs WHERE string_to_array(body, ' ') @> ARRAY['common'] AND NOT (string_to_array(body, ' ') @> ARRAY['beta'])}],
);
my $RANKED = q{SELECT string_agg(id::text, ',') FROM
	(SELECT id FROM docs WHERE body @@> 'w17 OR z3' ORDER BY sift.score(ctid) DESC, id LIMIT 20) r};

sub with_table { my ($q, $t) = @_; $q =~ s/%T%/$t/g; return $q }

# every query by name: the digests the next checks compare
sub snapshot
{
	my ($node) = @_;
	my %d;
	$d{$_}      = digest($node, $HEAP_QUERIES{$_}) for keys %HEAP_QUERIES;
	$d{$_}      = digest($node, with_table($TABLE_QUERIES{$_}, 'st')) for keys %TABLE_QUERIES;
	$d{$_}      = digest($node, $BM25_QUERIES{$_}[0]) for keys %BM25_QUERIES;
	$d{ranked}  = sql($node, $RANKED);
	return \%d;
}

# the index answers as the heap: the heap tables through a scan with sift off, the sift_table through its heap twin, BM25 through the array oracle
sub agree_with_heap
{
	my ($node, $label) = @_;
	for my $name (sort keys %HEAP_QUERIES)
	{
		is(digest($node, $HEAP_QUERIES{$name}), digest($node, $HEAP_QUERIES{$name}, 1), "$label: $name is the heap's answer");
	}
	for my $name (sort keys %TABLE_QUERIES)
	{
		is(digest($node, with_table($TABLE_QUERIES{$name}, 'st')), digest($node, with_table($TABLE_QUERIES{$name}, 'st_heap')),
			"$label: $name is the heap twin's answer");
	}
	for my $name (sort keys %BM25_QUERIES)
	{
		is(digest($node, $BM25_QUERIES{$name}[0]), digest($node, $BM25_QUERIES{$name}[1]), "$label: $name is the oracle's answer");
	}
	is(sql($node, "SELECT count(*) FROM sift.check()"), '0', "$label: sift.check() finds no problem");
	is(sql($node, "SELECT count(*) FROM sift.quarantine"), '0', "$label: no index is quarantined");
}

sub formats { my ($node, @rels) = @_; return { map { $_ => format_of($node, $_) } @rels } }

sub filenodes
{
	my ($node) = @_;
	my $list = join(', ', map { "'$_'" } @FORMAT1, $BM25);
	return sql($node, "SELECT string_agg(relname || '=' || pg_relation_filenode(oid), ',' ORDER BY relname) FROM pg_class WHERE relname IN ($list)");
}

# the same statements on every table; phase P keeps its ids apart (B + 1 ..) and its moduli distinct
sub dml
{
	my ($phase, @which) = @_;
	my $b = $ROWS + 1000 * $phase;
	my %stmt = (
		ev => qq{
			INSERT INTO ev SELECT i, i % 11, date '2026-01-01' + i % 200, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 97 END, (i % 1000) / 4.0,
				jsonb_build_object('k', i % 17, 's', 'tag' || i % 5) FROM generate_series($b + 1, $b + 300) i;
			UPDATE ev SET v = coalesce(v, 0) + $phase + 1, amount = amount + 1, doc = jsonb_set(doc, '{k}', to_jsonb((doc->>'k')::int + 1))
				WHERE id % (7 + $phase) = 0;
			DELETE FROM ev WHERE id % (29 + $phase) = 0;},
		sc => qq{
			INSERT INTO sc_ev SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 210 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day',
				'k' || i % 3, i % 10 FROM generate_series($b + 1, $b + 300) i;
			UPDATE sc_ev SET dim_a_id = 1 + (dim_a_id * 3) % 210, v = v + 1 WHERE id % 11 = $phase;
			DELETE FROM sc_ev WHERE id % 31 = $phase;
			DELETE FROM pairs WHERE dim_a_id % 17 = $phase;
			INSERT INTO pairs SELECT m, 1 + (m + $phase) % 10 FROM generate_series(1, 200) m WHERE m % 5 = $phase % 5 ON CONFLICT DO NOTHING;},
		docs => qq{
			INSERT INTO docs SELECT i, 'alpha w' || i % 50 || ' z' || i % 7 || ' ' || CASE WHEN i % 3 = 0 THEN 'beta' ELSE 'gamma' END || ' common'
				FROM generate_series($b + 1, $b + 300) i;
			UPDATE docs SET body = body || ' w' || ($phase * 7) WHERE id % (9 + $phase) = 0;
			DELETE FROM docs WHERE id % (23 + $phase) = 0;},
		st => join('', map { my $t = $_; qq{
			INSERT INTO $t SELECT i, i % 9, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 101 END, 'row' || i, (i % 500) / 2.0 FROM generate_series($b + 1, $b + 300) i;
			INSERT INTO $t SELECT i, 0, 1, repeat(md5(i::text), 300), 1.00 FROM generate_series($b + 400, $b + 405) i;
			UPDATE $t SET v = coalesce(v, 0) + $phase + 1, amt = amt + 1 WHERE id % (5 + $phase) = 0;
			DELETE FROM $t WHERE id % (19 + $phase) = 0;} } qw(st st_heap)),
	);
	return join("\n", map { $stmt{$_} } @which);
}

our @ALL_DML = qw(ev sc docs st);
my @ALL_TABLES = qw(ev sc_ev pairs docs st st_heap st_vf);

sub vacuum_all { my ($node) = @_; sql($node, 'VACUUM (ANALYZE) ' . join(', ', @ALL_TABLES)) }

sub fixture_sql
{
	return qq{
	CREATE TABLE ev (id int PRIMARY KEY, grp int NOT NULL, d date NOT NULL, v int, amount numeric(12,2) NOT NULL, doc jsonb NOT NULL);
	INSERT INTO ev SELECT i, i % 11, date '2026-01-01' + i % 200, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 97 END, (i % 1000) / 4.0,
		jsonb_build_object('k', i % 17, 's', 'tag' || i % 5) FROM generate_series(1, $ROWS) i;
	CREATE INDEX ev_col ON ev USING sift_col (id, grp, d, v, amount, doc) WITH (paths = 'doc.k int4, doc.s text');
	CREATE INDEX ev_trk ON ev USING sift_col (id, grp, v) WITH (track_changes = 'ctid');

	CREATE TABLE pairs (dim_a_id int NOT NULL, tenant int4 NOT NULL, PRIMARY KEY (dim_a_id, tenant));
	INSERT INTO pairs SELECT m, 1 + m % 10 FROM generate_series(1, 200) m;
	INSERT INTO pairs SELECT m, 1 + (m * 3 + 1) % 10 FROM generate_series(1, 200) m WHERE m % 3 = 0 AND (m * 3 + 1) % 10 <> m % 10;
	CREATE INDEX pairs_journal ON pairs USING sift_col (dim_a_id, tenant) WITH (track_changes = 'ctid');
	CREATE TABLE sc_ev (id int PRIMARY KEY, dim_a_id int, at timestamptz, kind text, v int);
	INSERT INTO sc_ev SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 210 END, timestamptz '2026-01-01' + (i % 300) * interval '1 day',
		'k' || i % 3, i % 10 FROM generate_series(1, $ROWS) i;
	CREATE INDEX sc_idx ON sc_ev USING sift_col (id, dim_a_id, at, kind, v) WITH (scope = 'tenant: dim_a_id -> pairs(dim_a_id, tenant)');

	CREATE TABLE docs (id int PRIMARY KEY, body text NOT NULL);
	INSERT INTO docs SELECT i, 'alpha w' || i % 50 || ' z' || i % 7 || ' ' || CASE WHEN i % 3 = 0 THEN 'beta' ELSE 'gamma' END || ' common'
		FROM generate_series(1, $ROWS) i;
	CREATE INDEX $BM25 ON docs USING sift (body);

	CREATE TABLE st (id int PRIMARY KEY, grp int NOT NULL, v int, s text NOT NULL, amt numeric(10,2)) USING sift_table;
	CREATE TABLE st_heap (id int PRIMARY KEY, grp int NOT NULL, v int, s text NOT NULL, amt numeric(10,2)) USING heap;
	CREATE INDEX st_grp ON st (grp);
	CREATE INDEX st_heap_grp ON st_heap (grp);
	INSERT INTO st SELECT i, i % 9, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 101 END, 'row' || i, (i % 500) / 2.0 FROM generate_series(1, $ROWS) i;
	INSERT INTO st_heap SELECT * FROM st;
	CREATE TABLE st_vf (id int PRIMARY KEY, v int) USING sift_table;
	INSERT INTO st_vf SELECT i, i % 13 FROM generate_series(1, 500) i;
};
}

1;
