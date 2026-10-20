#!/usr/bin/env bash
# Renames the "mutable segment / mutable lane / write lane / delta segment" vocabulary to "write buffer" (decision and glossary:
# docs/NAMING.md). Mechanical and idempotent: perl rules with word boundaries on the tracked text files, `git mv` for the files,
# and the `mutable_limit` alias of the shipped reloption added in sift_handler.c.
#
#   scripts/rename-write-buffer.sh            rewrite the tracked files of this checkout, move the files, add the alias
#   scripts/rename-write-buffer.sh --check    list every remaining old-term occurrence (file:line:text); exit 1 if there is one
#   scripts/rename-write-buffer.sh --verbs   list the "seal" / "sealing" lines of the sift_table files, whose meaning changes
#                                             (sealing into columns is a promotion now; docs/NAMING.md, manual edits)
#   scripts/rename-write-buffer.sh --accept DIR   copy DIR/*.out over test/expected/ for the tests whose result differs (read the
#                                             diff first: after the rename only renamed terms and psql column widths may change)
#
# Rerunning on a later head is safe: the rules skip what is already renamed (a second run rewrites nothing). Reviewed by hand once: the
# "seal" verbs of the sift_table files (the rules below turn the ones that mean a promotion), docs/NAMING.md (status line).
#
# Not touched (history or must keep the old names): releases/, results/, CHANGELOG.md of the three packages, docs/releases/, docs/perf/,
# the scripts of the released versions sql/pg_sift--1.*.sql, scripts/perf-gate-maint.sh (it runs against old checkouts, which only know
# mutable_limit, and every release accepts that name), bench/xbench (Rust `mut`), this script and docs/NAMING.md.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

EXCLUDE='^(releases/|results/|docs/releases/|docs/perf/|bench/xbench/|CHANGELOG\.md$|(mariadb|sqlite)/CHANGELOG\.md$|LICENSE$|NOTICE$|sql/pg_sift--1\.[0-9]\.sql$|sql/pg_sift--1\.[0-9]--1\.[0-9]\.sql$|scripts/perf-gate-maint\.sh$|test/fixtures/compat2x/info\.txt$|scripts/rename-write-buffer\.sh$|scripts/restructure-src\.sh$|docs/NAMING\.md$|test/sql/write_buffer_alias\.sql$|test/expected/write_buffer_alias\.out$)'

# The files of the sift_table access method and the places that describe it, where `delta` meant the write buffer
# (elsewhere `delta` is the column encoding, or a page delta of the generic WAL).
TBL='^(src/([a-z0-9]+/)?sift_table[a-z_]*\.[ch]|src/([a-z0-9]+/)?sift_col_maint\.c|docs/COL-TAM\.md|test/(sql|expected)/tbl_[a-z_]*\.(sql|out)|test/t/[0-9a-z_]*tbl[a-z_]*\.(pl|sh)|sql/pg_sift--3\.0\.sql|sql/pg_sift--1\.2--3\.0\.sql|README\.md|docs/USAGE\.md)$'

# ---------------------------------------------------------------------------------------------------------------------------
# MAPPING TABLE. Applied in this order to the text of every file; columns are tab separated: scope (a regex on the path, `*` = all),
# pattern (perl regex), replacement ($1.. are the captures). A rule must not match its own output, so that a second run changes nothing.
# ---------------------------------------------------------------------------------------------------------------------------
read -r -d '' RULES <<'EOF' || true
# --- kinds of sift.segments, sift.col_segments and sift.table_segments (SQL visible); before the identifiers they mention
/sift_segment_sql\.c$	(SIFT_SEG_MUTABLE_ACTIVE:\s*return )"mutable"	$1"buffer"
/sift_segment_sql\.c$	(SIFT_SEG_MUTABLE_SEALED:\s*return )"sealed"	$1"sealed buffer"
/sift_col_maint\.c$	"mutable" : "sealed"	"buffer" : "sealed buffer"
/sift_table_seg\.c$	"delta" : "sealed delta"	"buffer" : "sealed buffer"
\.(sql|out|sh|pl|pm|spec)$	\bkind = 'mutable'	kind = 'buffer'
\.(sql|out|sh|pl|pm|spec)$	\bkind = 'sealed'	kind = 'sealed buffer'
\.(sql|out|sh|pl|pm|spec)$	\bkind = 'delta'	kind = 'buffer'
\.(sql|out|sh|pl|pm|spec)$	\bkind <> 'delta'	kind <> 'buffer'
\.(sql|out|sh|pl|pm|spec)$	\bkind = 'sealed delta'	kind = 'sealed buffer'
\.(sql|out|sh|pl|pm|spec)$	ELSE 'mutable' END	ELSE 'buffer' END
\.(sql|out)$	\bAS mutable\b	AS buffer
\.(sql|out)$	\bfac_summary\('mutable'\)	fac_summary('buffer')
# --- reloption, GUCs, columns
*	\bMutable segment size in kB before it is sealed for promotion	Write buffer size in kB before it is sealed for promotion
*	\bmutable_segments\b	buffers
*	\bSIFT_DEFAULT_MUTABLE_LIMIT\b	SIFT_DEFAULT_WRITE_BUFFER_LIMIT
*	\bmutable_limit\b	write_buffer_limit
*	\bmutable limit\b	write buffer limit
*	\btable_delta_limit\b	table_write_buffer_limit
*	\bsift_tbl_delta_limit\b	sift_tbl_write_buffer_limit
*	\bsift_delta_limit\b	sift.table_write_buffer_limit
*	\btable_inline_seal\b	table_inline_promote
*	\bsift_tbl_inline_seal\b	sift_tbl_inline_promote
*	\bsift_tbl_seal\b	sift_tbl_promote
*	(?<![A-Za-z])tbl_seal(?![A-Za-z0-9])	tbl_promote
# --- identifiers
*	\bSIFT_PT_MUT(HEADER|DATA)\b	SIFT_PT_WBUF$1
*	\bSIFT_SEG_MUTABLE_ACTIVE\b	SIFT_SEG_WBUF_OPEN
*	\bSIFT_SEG_MUTABLE_	SIFT_SEG_WBUF_
*	\bSIFT_MUTPAGE_	SIFT_WBUFPAGE_
*	\bSIFT_MUT_	SIFT_WBUF_
*	\bSIFT_COLMUT_	SIFT_COLWBUF_
*	\bSIFT_COLO_NOMUTABLE\b	SIFT_COLO_NOWBUF
*	\bNO_MUTABLE\b	NO_WBUF
*	\bTEST_NOMUTABLE\b	TEST_NOWBUF
*	\bSIFT_MUTABLE_H\b	SIFT_WBUF_H
*	\bSIFT_COL_MUT_H\b	SIFT_COL_WBUF_H
*	\bMUTHEADER\b	WBUFHEADER
*	\bMUTPAGE\b	WBUFPAGE
*	\bSiftCMut	SiftCWbuf
*	\bSiftMut	SiftWbuf
*	\bMut(?=(?:TermEnt|Hit|Hint|Count)\b)	Wbuf
*	\bsift_cmut_	sift_cwbuf_
*	\bsift_mut_	sift_wbuf_
*	\bsift_mutable\b	sift_wbuf
*	\bsift_col_mut(?![a-z0-9])	sift_col_wbuf
*	(?<![A-Za-z0-9])(_?(?:sift_)?test_col_)mut_	$1wbuf_
*	\bcol_mut(?![a-z0-9])	col_wbuf
^(src|mariadb|sqlite)/.*\.[ch]$	\bmut_(ndocs|sum_dl|page_ok|leaf_score|leaf_ok)\b	wbuf_$1
^(src|mariadb|sqlite)/.*\.[ch]$	\bleaf_mut_(term|terms|count)\b	leaf_wbuf_$1
^(src|mariadb|sqlite)/.*\.[ch]$	\bnmut\b	nwbuf
^(src|mariadb|sqlite)/.*\.[ch]$	\bmutno\b	wbufno
^(src|mariadb|sqlite)/.*\.[ch]$	\bmutdoc_copy\b	wbufdoc_copy
/sift_facet\.c$	\bmut\b	wbuf
*	\bnmutable\b	nwbuf
*	(?<=_)mutable(?![A-Za-z0-9])	wbuf
*	(?<![A-Za-z0-9])mutable_(?=[a-z])	wbuf_
# --- prose; the longest phrases first
*	\bsift_col mutable:	sift_col write buffer:
*	\bactive mutable segments?\b	open write buffer
*	\bmutable segments\b	write buffers
*	\bMutable segments\b	Write buffers
*	\bmutable segment\b	write buffer
*	\bMutable segment\b	Write buffer
*	\bmutable lane chains\b	write buffer lane chains
*	\bmutable lanes\b	write buffer lanes
*	\bmutable lane\b	write buffer
*	\bMutable lanes\b	Write buffer lanes
*	\bMutable lane\b	Write buffer
*	(?<![A-Za-z])mutable (page|pages|block|blocks|record|records|doc|docs|document|documents|row|rows|term|terms|value|values|batches|data|limit|header|appender|appenders|matches|half|count|chain|chains)\b	write buffer $1
*	\b([Tt]he) mutable ones?\b	$1 write buffer
*	\bmutable-lane\b	write buffer
*	(?<![A-Za-z])Mutable (scan|reader|delete|totals|unit|units|appends|matches|match)\b	Write buffer $1
*	(?<![A-Za-z])Mutable (page|pages|block|blocks|record|records|doc|docs|document|documents|row|rows|term|terms|value|values|batches|data|limit|header|appender|appenders|matches|half|count|chain|chains)\b	Write buffer $1
*	(?<![A-Za-z])mutable (scan|reader|delete|totals|unit|units|appends|matches|match)\b	write buffer $1
*	\bwrite lane\b	write buffer
*	\bwrite segments\b	write buffers
*	\bwrite segment\b	write buffer
# --- sift_table: `delta` was the write buffer (kind, segments, pages, rows); the tombstone log keeps its name
TBL	\bsealed delta segments\b	sealed write buffers
TBL	\bsealed delta segment\b	sealed write buffer
TBL	\bsealed deltas\b	sealed write buffers
TBL	\bactive delta\b	open write buffer
TBL	\bdelta segments\b	write buffers
TBL	\bdelta segment\b	write buffer
TBL	\bDelta segment\b	Write buffer
TBL	\bsealed delta\b	sealed buffer
TBL	\bkind delta\b	kind buffer
TBL	\bthe delta\b	the write buffer
TBL	\bdelta (pages?|rows?|records?|inserts?|chain|append|entry|header|data)\b	write buffer $1
TBL	\bDelta (pages?|rows?|records?|inserts?|chain|append|entry|header|data)\b	Write buffer $1
TBL	\bdeltas\b	write buffers
TBL	(?<=[A-Za-z0-9_])delta|delta(?=[A-Za-z0-9_])	wbuf
TBL	(?<=[A-Za-z0-9_])Delta|Delta(?=[A-Za-z0-9_])	Wbuf
TBL	(?<=[A-Za-z0-9_])DELTA|DELTA(?=[A-Za-z0-9_])	WBUF
TBL	: delta \(row format write buffers\), sealed buffer \(waiting for sealing\)	: buffer (row format write buffers), sealed buffer (waiting for promotion)
TBL	\bto be sealed into columns	to be promoted into columns
TBL	\bSeals the closed write buffers	Promotes the closed write buffers
TBL	\bseals (its|the) closed write buffers	promotes $1 closed write buffers
TBL	\bseal_(compare|layout)\b	promote_$1
TBL	-{10} sealing -{10}	---------- promotion ----------
TBL	(?<=->)delta\b	wbuf
TBL	(?<=[,{]\.)delta\b	wbuf
TBL	(?<=[A-Za-z_\]\)]\.)delta\b	wbuf
TBL	(\*\s*|bool\s+)delta;	$1wbuf;
# --- layout-independent line breaks, DESIGN.md module names, the 2.0.0 fixture
*	\bmutable(\s*\n\s*(?:\*|#|--)?\s*)segments\b	write$1buffers
*	\bmutable(\s*\n\s*(?:\*|#|--)?\s*)segment\b	write$1buffer
*	\bmutable(\s*\n\s*(?:\*|#|--)?\s*)(page|matches)\b	write buffer$1$2
*	\bsealed \(mutable\)(\s*\n#\s*)segments until	sealed write buffers$1until
*	\(the mutable(\s*\n#\s*)segments that hold	(the write$1buffers that hold
*	\bmutable and (promoted segments|immutable pages)	write buffers and $1
*	(?<![A-Za-z])mutable (batch)\b	write buffer $1
^DESIGN\.md$	\bAn immutable or mutable set of postings	An immutable set of postings (or a write buffer of them)
^DESIGN\.md$	\(immutable / mutable-active / mutable-sealed\)	(immutable / buffer / sealed buffer)
^DESIGN\.md$	\*Shared-memory mutable buffers\*	*Shared-memory write buffers*
^DESIGN\.md$	\(immutable/mutable\)	(immutable/write buffer)
^DESIGN\.md$	(?m)^\| mutable \| `sift_wbuf	| wbuf | `sift_wbuf
^DESIGN\.md$	\| segment, mutable, (build|query)	| segment, wbuf, $1
^DESIGN\.md$	Only `mutable` writes lane pages	Only `wbuf` writes lane pages
^DESIGN\.md$	\| mutable, maint \(seal	| wbuf, maint (seal
^test/fixtures/compat2x/expected\.txt$	\|mutable\|	|buffer|
^README\.md$	\bseals and merges segments, VACUUM clears dead rows	promotes write buffers and merges segments, VACUUM clears dead rows
^docs/USAGE\.md$	(size of the write buffer before it is sealed into a segment; also the base size of the merge tiers) \|	$1; `mutable_limit`, the name of 1.x and 2.0, is a deprecated alias that is still accepted |
# --- sift_table: "seal" is closing the write buffer to appends; turning it into column groups is a promotion (docs/NAMING.md)
TBL	\*\*delta \+ groups\.\*\*	**write buffer + groups.**
TBL	\bSealing \(section 7\) turns	Promotion (section 7) turns
TBL	\bnever rewrites a sealed group	never rewrites a column group
TBL	\bindex cleanup, seal, merge	index cleanup, promotion, merge
TBL	\bInline sealing when a write buffer fills	Inline promotion when a write buffer fills
TBL	so nothing can be sealed, fails	so nothing can be promoted, fails
TBL	\("too many unsealed delta\nsegments, a long-running transaction prevents compaction"\)	(detail "Too many write buffers waiting for promotion: the rows of running transactions cannot be promoted")
TBL	more than 400 MB of unsealed rows	more than 400 MB of rows waiting for promotion
TBL	\bdefault 4 MB delta limit	default 4 MB write buffer limit
TBL	\bdelta and tombstone segments	write buffers and tombstone segments
TBL	\(delta, `xmin = me`\)	(write buffer, `xmin = me`)
TBL	oldest xid still stored \(delta,	oldest xid still stored (write buffer rows,
TBL	in every state: delta, sealed, tombstones	in every state: write buffer, promoted, tombstones
TBL	Write buffer rows older than the horizon are sealed into groups	Write buffer rows older than the horizon are promoted into groups
TBL	a seal that replaces a write buffer with a group	a promotion that replaces a write buffer with a group
TBL	`kill -9` during insert, seal, vacuum	`kill -9` during insert, promotion, vacuum
TBL	the seal's row conversion	the promotion's row conversion
TBL	\*\*Sealing\*\* \(`sift_tbl_promote`	**Promotion** (`sift_tbl_promote`
TBL	a write buffer is closed to appends \(page	a write buffer is sealed, closed to appends (page
TBL	retried by the next seal\)	retried by the next promotion)
TBL	tombstone is sealed as a normal row	tombstone is promoted as a normal row
TBL	The inline seal takes the maintenance lock	The inline promotion takes the maintenance lock
TBL	\(8 pages or more\), seal, merge	(8 pages or more), promote, merge
TBL	no row-format pages, no seal\)	no row-format pages, no promotion)
TBL	taken from the session that seals,	taken from the session that promotes,
TBL	the next `INSERT` or seal fails	the next `INSERT` or promotion fails
TBL	VACUUM sealing, point/index/bitmap reads, updates and deletes of sealed rows	VACUUM promotion, point/index/bitmap reads, updates and deletes of promoted rows
TBL	autovacuum seals an insert-only	autovacuum promotes an insert-only
TBL	a repeatable-read reader across a seal, a standby of a sealed table	a repeatable-read reader across a promotion, a standby of a promoted table
TBL	\| used by seal and merge \|	| used by promotion and merge |
TBL	Too many unsealed write buffers: 	Too many write buffers waiting for promotion: 
TBL	rows of running transactions cannot be sealed	rows of running transactions cannot be promoted
TBL	\bso the seals of a bulk load that fall in between are promoted too	so the write buffers a bulk load closes in between are promoted too
TBL	before it is closed and sealed into columns	before it is closed and promoted into columns
TBL	\| sealing while rows are inserted \|	| promotion while rows are inserted |
TBL	Background sealing by `VACUUM`	Background promotion by `VACUUM`
TBL	rows are sealed into columns only once it has ended	rows are promoted into columns only once it has ended
TBL	Inline sealing and the closing of write buffers	Inline promotion and the closing of write buffers
TBL	what background sealing should use	what background promotion should use
TBL	the VACUUM that seals them later	the VACUUM that promotes them later
TBL	VACUUM, sealing, rewrites	VACUUM, promotion, rewrites
TBL	it can then be sealed \*/	it can then be promoted */
TBL	\* Seals the write buffers that stopped taking rows	* Promotes the write buffers that stopped taking rows
TBL	segments; returns the number sealed\.	segments; returns the number promoted.
TBL	"sift_table seal	"sift_table promote
TBL	\bint\t\t\tsealed = 0;	int			promoted = 0;
TBL	\t\tsealed\+\+;			promoted++;
TBL	\bif \(sealed > 0\)	if (promoted > 0)
TBL	\breturn sealed;	return promoted;
TBL	A writer closed a write buffer: seal the segments whose rows have settled	A writer closed a write buffer: promote the segments whose rows have settled
TBL	\(sealing moves rows from one to the other\)	(promotion moves rows from one to the other)
TBL	\bdelta(\s*\n\s*\*\s*)segments \(the row-format write buffer\)	write$1buffers (row format)
TBL	\bdelta(\s*\n\s*\*\s*)page tables	write buffer$1page tables
TBL	\bAS delta\b	AS wbuf
TBL	-- sealed into column groups	-- promoted into column groups
TBL	and after sealing the new ones	and after promoting the new ones
TBL	(?m)^-- seals it into a column segment	-- promotes it into a column segment
TBL	AS sealed\b	AS promoted
TBL	one transaction cannot be sealed while it runs	one transaction cannot be promoted while it runs
TBL	(?m)^-- Sealing: the rows of a sift_table move	-- Promotion: the rows of a sift_table move
TBL	deletes of sealed rows, VACUUM	deletes of promoted rows, VACUUM
TBL	promote_compare\('sealed by VACUUM'\)	promote_compare('promoted by VACUUM')
TBL	sealed by VACUUM: same	promoted by VACUUM: same
TBL	-- point reads of sealed rows	-- point reads of promoted rows
TBL	-- changes of sealed rows	-- changes of promoted rows
TBL	update and delete of sealed rows	update and delete of promoted rows
TBL	-- ALTER TABLE after sealing	-- ALTER TABLE after promotion
TBL	the blocks of the write buffers that sealing and merging release	the blocks of the write buffers that promotion and merging release
TBL	from the sealed units \(and the write buffer rows\)	from the promoted units (and the write buffer rows)
TBL	(?m)^# sift_table sealing: autovacuum	# sift_table promotion: autovacuum
TBL	survive a seal, and a standby reads the sealed table	survive a promotion, and a standby reads the promoted table
TBL	'autovacuum seals the rows	'autovacuum promotes the rows
TBL	'the same rows after sealing'	'the same rows after promotion'
TBL	began before a seal keeps	began before a promotion keeps
TBL	across a seal and an open transaction	across a promotion and an open transaction
TBL	fetches a sealed row	fetches a promoted row
TBL	'a standby reads the sealed tables'	'a standby reads the promoted tables'
TBL	for inline sealing, the session	for inline promotion, the session
TBL	write buffers, sealed while they are inserted	write buffers, promoted while they are inserted
TBL	a write buffer is sealed once its rows are settled	a write buffer is promoted once its rows are settled
TBL	(?m)^sub sealed \{	sub promoted {
TBL	'inline sealing wrote column segments'	'inline promotion wrote column segments'
TBL	'inline sealing in the writer	'inline promotion in the writer
TBL	it seals what earlier transactions closed	it promotes what earlier transactions closed
TBL	'a manual VACUUM seals under	'a manual VACUUM promotes under
TBL	loaded with sealing off	loaded with promotion off
TBL	ok\(sealed\(\$t\), "autovacuum sealed \$t"\)	ok(promoted($t), "autovacuum promoted $t")
EOF

# Files that change name (git mv); then the same names inside the files are covered by the rules above.
read -r -d '' MOVES <<'EOF' || true
src/sift_mutable.c	src/sift_wbuf.c
src/sift_mutable.h	src/sift_wbuf.h
src/sift_col_mut.c	src/sift_col_wbuf.c
src/sift_col_mut.h	src/sift_col_wbuf.h
src/sift_col_mut_test.c	src/sift_col_wbuf_test.c
test/sql/mutable_lifecycle.sql	test/sql/wbuf_lifecycle.sql
test/expected/mutable_lifecycle.out	test/expected/wbuf_lifecycle.out
test/sql/col_mut.sql	test/sql/col_wbuf.sql
test/expected/col_mut.out	test/expected/col_wbuf.out
test/t/col_mut_crash.sh	test/t/col_wbuf_crash.sh
test/sql/tbl_seal.sql	test/sql/tbl_promote.sql
test/expected/tbl_seal.out	test/expected/tbl_promote.out
test/t/008_tbl_seal.pl	test/t/008_tbl_promote.pl
EOF
# (the test name mutable_lifecycle becomes wbuf_lifecycle through the prefix rule `mutable_` -> `wbuf_`)

# ---------------------------------------------------------------------------------------------------------------------------

# the tracked text files in scope
files() { git grep -I -l -e '' -- . | grep -Ev "$EXCLUDE" || true; }
# the expected outputs are regenerated by the tests (--accept), not read by --check
check_files() { files | grep -v '^test/expected/' || true; }

# Lines no rule touches: the kept alias, and any line that calls an old name deprecated
KEEP_LINES='DEPRECATED ALIAS|[Dd]eprecated'

# Old-term patterns of --check (perl, per line); a line matching ALLOW is accepted (program state, the kept alias)
CHECK_RE='(?<![A-Za-z])[Mm]utable|\b(?:sift_mut_|sift_cmut_|SiftMut|SiftCMut|SIFT_MUT|SIFT_SEG_MUTABLE|MUTABLE)|\bwrite (?:lane|segments?)\b'
CHECK_TBL_RE='\b[Dd]elta\b|\bsift_tbl_seal\b'
CHECK_ALLOW='mutable (?:state|variable|objects?)|global mutable|DEPRECATED ALIAS|deprecated alias|Deprecated alias|[0-9] mutable [0-9]+ (?:f|false)\b'
CHECK_TBL_ALLOW='generic WAL|page delta|delta of roughly|delta encoding|delta-coded|delta coding|delta,? run-length|\bdelta\b *\('

check() {
	check_files | TBL="$TBL" CHECK_RE="$CHECK_RE" CHECK_TBL_RE="$CHECK_TBL_RE" CHECK_ALLOW="$CHECK_ALLOW" CHECK_TBL_ALLOW="$CHECK_TBL_ALLOW" \
		perl -ne '
		BEGIN { $n = 0; ($re, $tre, $allow, $tallow, $tbl) = map { qr/$ENV{$_}/ } qw(CHECK_RE CHECK_TBL_RE CHECK_ALLOW CHECK_TBL_ALLOW TBL); }
		chomp; my $f = $_;
		open my $fh, "<", $f or next;
		while (my $l = <$fh>) {
			chomp $l;
			my $hit = $l =~ $re && $l !~ $allow;
			$hit ||= ($f =~ $tbl && $l =~ $tre && $l !~ $tallow);
			if ($hit) { print "$f:$.:$l\n"; $n++ }
		}
		END { exit($n ? 1 : 0) }'
}

verbs() {
	files | grep -E "$TBL" | xargs grep -n -i -E '\bseal(s|ed|ing)?\b' || true
}

accept() {
	local dir=$1 t
	for t in "$dir"/*.out; do
		[[ -e $t ]] || continue
		cmp -s "$t" "test/expected/$(basename "$t")" || { cp "$t" "test/expected/$(basename "$t")"; echo "accepted $(basename "$t")"; }
	done
}

rewrite() {
	files | RULES="$RULES" TBL="$TBL" KEEP="$KEEP_LINES" perl -CSD -e '
		my $tbl = qr/$ENV{TBL}/;
		my $keep = $ENV{KEEP};
		my @rules;
		for (split /\n/, $ENV{RULES}) {
			next if /^\s*(#|$)/;
			my ($scope, $pat, $rep) = split /\t/, $_, 3;
			$scope = $tbl if $scope eq "TBL";
			push @rules, [$scope eq "*" ? undef : qr/$scope/, qr/$pat/, $rep];
		}
		sub expand { my ($t, @c) = @_; $t =~ s{\$(\d)}{ $c[$1 - 1] // "" }ge; $t }
		while (my $f = <STDIN>) {
			chomp $f;
			open my $in, "<:raw", $f or next;
			my $text = do { local $/; <$in> };
			close $in;
			my $old = $text;
			utf8::decode($text);
			my @kept;
			$text =~ s/^([^\n]*(?:$keep)[^\n]*)$/push @kept, $1; "\x{1}KEEP" . $#kept . "\x{1}"/gme;
			for my $r (@rules) {
				my ($scope, $re, $rep) = @$r;
				next if defined $scope && $f !~ $scope;
				$text =~ s/$re/expand($rep, @{^CAPTURE})/ge;
			}
			$text =~ s/\x{1}KEEP(\d+)\x{1}/$kept[$1]/g;
			utf8::encode($text);
			next if $text eq $old;
			open my $out, ">:raw", $f or die "$f: $!";
			print $out $text;
			close $out;
			print "rewrote $f\n";
		}'
}

# A file of src/ is where it is: flat, or in the folder scripts/restructure-src.sh gave it (the rename runs before or after it)
move_files() {
	local old new cur
	while IFS=$'\t' read -r old new; do
		if [[ $old == src/* ]]; then
			cur=$(git ls-files -- "$old" "src/*/${old##*/}" | head -n 1)
			[[ -n $cur ]] || continue
			old=$cur new=${cur%/*}/${new##*/}
		fi
		[[ -n $old && -e $old && ! -e $new ]] || continue
		git mv "$old" "$new"
		echo "moved $old -> $new"
	done <<<"$MOVES"
}

# mutable_limit, the reloption of 2.0.0, stays accepted as an alias of write_buffer_limit (sift AM only: sift_col is new in 3.0).
# It is registered like any option (lock mode, bounds, ALTER INDEX .. SET / RESET) and counts when write_buffer_limit is at its default.
srcfile() { git ls-files -- "src/$1" "src/*/$1" | head -n 1; }
alias_option() {
	local h c
	h=$(srcfile sift.h) c=$(srcfile sift_handler.c)
	grep -q 'DEPRECATED ALIAS' "$h" || perl -0pi -e '
		s{(\tint\t\t\twrite_buffer_limit;[^\n]*\n)}{$1\tint\t\t\tmutable_limit;\t/* DEPRECATED ALIAS of write_buffer_limit: the 2.0.0 spelling, read by sift_amoptions only */\n} or die "sift.h";
	' "$h"
	grep -q 'DEPRECATED ALIAS' "$c" || perl -0pi -e '
		s{(\tadd_int_reloption\(sift_relopt_kind, "write_buffer_limit",.*?ShareUpdateExclusiveLock\);\n)}
		 {$1\t/* The 2.0.0 spelling of write_buffer_limit, accepted forever (docs/NAMING.md) */\n\tadd_int_reloption(sift_relopt_kind, "mutable_limit",\t/* DEPRECATED ALIAS */\n\t\t\t\t\t  "Deprecated alias of write_buffer_limit",\n\t\t\t\t\t  SIFT_DEFAULT_WRITE_BUFFER_LIMIT, 64, 1048576, ShareUpdateExclusiveLock);\n}s or die "reloption";
		s{(\t\t\{"write_buffer_limit", RELOPT_TYPE_INT, offsetof\(SiftOptions, write_buffer_limit\)\},\n)}
		 {$1\t\t{"mutable_limit", RELOPT_TYPE_INT, offsetof(SiftOptions, mutable_limit)},\t/* DEPRECATED ALIAS */\n} or die "tab";
		s{\treturn \(bytea \*\) build_reloptions\(reloptions, validate, sift_relopt_kind,\n\t+  sizeof\(SiftOptions\), tab, lengthof\(tab\)\);\n}
		 {\tSiftOptions *o = (SiftOptions *) build_reloptions(reloptions, validate, sift_relopt_kind,\n\t\t\t\t\t\t\t\t\t\t\t\t\t  sizeof(SiftOptions), tab, lengthof(tab));\n\n\t/* the alias counts when write_buffer_limit is left at its default */\n\tif (o != NULL && o->write_buffer_limit == SIFT_DEFAULT_WRITE_BUFFER_LIMIT)\n\t\to->write_buffer_limit = o->mutable_limit;\t/* DEPRECATED ALIAS */\n\treturn (bytea *) o;\n} or die "amoptions";
	' "$c"
}

# The regression test of the alias (new in the rename: the old name has to keep working)
alias_test() {
	[[ -e test/sql/write_buffer_alias.sql ]] && return 0
	cat >test/sql/write_buffer_alias.sql <<'EOF'
-- mutable_limit, the reloption of 1.x and 2.0, stays accepted as an alias of write_buffer_limit
CREATE EXTENSION pg_sift;
CREATE TABLE wa (id serial PRIMARY KEY, body text);
CREATE INDEX wa_old ON wa USING sift (body) WITH (mutable_limit = 64);
CREATE INDEX wa_new ON wa USING sift (body) WITH (write_buffer_limit = 64);
CREATE INDEX wa_def ON wa USING sift (body);
SELECT relname, reloptions FROM pg_class WHERE relname IN ('wa_old', 'wa_new', 'wa_def') ORDER BY relname;

-- both names check their range
CREATE INDEX wa_bad1 ON wa USING sift (body) WITH (mutable_limit = 0);
CREATE INDEX wa_bad2 ON wa USING sift (body) WITH (write_buffer_limit = 0);

-- ALTER INDEX .. SET / RESET take the old name too
ALTER INDEX wa_def SET (mutable_limit = 128);
SELECT reloptions FROM pg_class WHERE relname = 'wa_def';
ALTER INDEX wa_def RESET (mutable_limit);
SELECT reloptions FROM pg_class WHERE relname = 'wa_def';

-- the limit applies under either name: 3000 documents fill a 64 kB write buffer many times, never the default 4096 kB one
SET sift.async_maintenance = off;
INSERT INTO wa (body) SELECT 'alpha beta word' || (i % 50) || ' gamma' FROM generate_series(1, 3000) i;
SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) > 0 AS old_sealed FROM sift.segments('wa_old');
SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) > 0 AS new_sealed FROM sift.segments('wa_new');
SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) = 0 AS def_sealed FROM sift.segments('wa_def');
DROP TABLE wa;
DROP EXTENSION pg_sift;
EOF
	cat >test/expected/write_buffer_alias.out <<'EOF'
-- mutable_limit, the reloption of 1.x and 2.0, stays accepted as an alias of write_buffer_limit
CREATE EXTENSION pg_sift;
CREATE TABLE wa (id serial PRIMARY KEY, body text);
CREATE INDEX wa_old ON wa USING sift (body) WITH (mutable_limit = 64);
CREATE INDEX wa_new ON wa USING sift (body) WITH (write_buffer_limit = 64);
CREATE INDEX wa_def ON wa USING sift (body);
SELECT relname, reloptions FROM pg_class WHERE relname IN ('wa_old', 'wa_new', 'wa_def') ORDER BY relname;
 relname |       reloptions        
---------+-------------------------
 wa_def  | 
 wa_new  | {write_buffer_limit=64}
 wa_old  | {mutable_limit=64}
(3 rows)

-- both names check their range
CREATE INDEX wa_bad1 ON wa USING sift (body) WITH (mutable_limit = 0);
ERROR:  value 0 out of bounds for option "mutable_limit"
DETAIL:  Valid values are between "64" and "1048576".
CREATE INDEX wa_bad2 ON wa USING sift (body) WITH (write_buffer_limit = 0);
ERROR:  value 0 out of bounds for option "write_buffer_limit"
DETAIL:  Valid values are between "64" and "1048576".
-- ALTER INDEX .. SET / RESET take the old name too
ALTER INDEX wa_def SET (mutable_limit = 128);
SELECT reloptions FROM pg_class WHERE relname = 'wa_def';
     reloptions      
---------------------
 {mutable_limit=128}
(1 row)

ALTER INDEX wa_def RESET (mutable_limit);
SELECT reloptions FROM pg_class WHERE relname = 'wa_def';
 reloptions 
------------
 
(1 row)

-- the limit applies under either name: 3000 documents fill a 64 kB write buffer many times, never the default 4096 kB one
SET sift.async_maintenance = off;
INSERT INTO wa (body) SELECT 'alpha beta word' || (i % 50) || ' gamma' FROM generate_series(1, 3000) i;
SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) > 0 AS old_sealed FROM sift.segments('wa_old');
 old_sealed 
------------
 t
(1 row)

SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) > 0 AS new_sealed FROM sift.segments('wa_new');
 new_sealed 
------------
 t
(1 row)

SELECT count(*) FILTER (WHERE kind IN ('sealed buffer', 'immutable')) = 0 AS def_sealed FROM sift.segments('wa_def');
 def_sealed 
------------
 t
(1 row)

DROP TABLE wa;
DROP EXTENSION pg_sift;
EOF
}

# The 3.0 changelog entry (CHANGELOG.md is history for the rewrite, so the entry is added here, once)
changelog_entry() {
	grep -q 'write_buffer_limit' CHANGELOG.md && return 0
	ENTRY='- Changed (3.0): the "mutable segment", "mutable lane", "write lane" and "delta segment" are one term, the **write buffer** (`docs/NAMING.md`). The reloption `mutable_limit` is now `write_buffer_limit` (kB, 64-1048576, default 4096); `mutable_limit` stays accepted as a deprecated alias with the same bounds, `ALTER INDEX .. SET / RESET` and a quiet restore of dumps of 1.x and 2.0 (`pg_class.reloptions` keeps what was typed; the new name wins when both are set). The `kind` of `sift.segments` is `buffer` and `sealed buffer` instead of `mutable` and `sealed` (`immutable` is unchanged), so a script that matches the old values has to change; the texts of corruption and verify messages say "Write buffer" where they said "Mutable". On-disk formats are unchanged. Only new in 3.0, so nothing released changes there: `sift_col`, the settings `sift.table_write_buffer_limit` and `sift.table_inline_promote`, the kinds of `sift.table_segments` and `sift.col_segments`, `sift.col_index_info.buffers`.' \
		perl -0pi -e 's{(^## Unreleased\n\n)}{$1$ENV{ENTRY}\n}m or die "CHANGELOG.md: no Unreleased section"' CHANGELOG.md
}

case "${1:-}" in
	--check)  check; exit ;;
	--verbs) verbs; exit ;;
	--accept) accept "${2:?--accept needs the directory of the result files}"; exit ;;
	"")       ;;
	*)        sed -n '2,13p' "$0"; exit 2 ;;
esac

move_files
rewrite
alias_option
alias_test
changelog_entry
echo "done; next: scripts/rename-write-buffer.sh --check, the regress tests, then --accept for the diffs you have read"
