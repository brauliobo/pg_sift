#!/usr/bin/env bash
# The layout checker of src/. The move of the files into one folder per area (core/, bm25/, col/, agg/, table/) and
# the rename of every file to its short name (x.c in bm25/, not sift_x.c) are done; this script keeps the layout from drifting.
#
#   scripts/restructure-src.sh --check    exit 1 and list: a file of src/ outside the area folders, a file of a renamed area that still has the
#                                         sift_ prefix, a name used twice in one folder, a reference to a file of src/ that does not exist, a
#                                         reference by the old name of a renamed file. Run it after a merge, before the build.
#   scripts/restructure-src.sh --map      the files of src/, area and name
#
# RENAMED lists the areas whose files have short names; the other areas are renamed one at a time (a source includes a file of a renamed area
# by its path from src/: #include "bm25/query.h"). Not checked (history): releases/, results/, docs/releases/, bench/results/, bench/xbench/,
# the old renames' scripts, the design notes; of a CHANGELOG.md only the Unreleased part.
set -euo pipefail
cd "$(dirname "$0")/.."

AREAS='core bm25 col agg table'
RENAMED='bm25 col table'
KEEP='areas.mk portable.list portable-col.list'

src_files() { git ls-files -- src | grep -v -E "^src/($(tr ' ' '|' <<<"$KEEP"))\$" || true; }

check_layout() {
	local f dir area name rc=0
	while read -r f; do
		dir=${f#src/}; area=${dir%%/*}; name=${dir#*/}
		if [[ $dir != */* || $name == */* ]] || ! grep -qw -- "$area" <<<"$AREAS compat"; then echo "$f: not in a folder of src/ ($AREAS)"; rc=1; continue; fi
		if grep -qw -- "$area" <<<"$RENAMED" && [[ $name == sift_* ]]; then echo "$f: a file of $area/ has no sift_ prefix"; rc=1; fi
	done < <(src_files)
	return $rc
}

# the tracked text files whose references are checked
EXCLUDE='^(releases/|results/|docs/releases/|bench/results/|bench/xbench/|LICENSE$|scripts/rename-write-buffer\.sh$|scripts/restructure-src\.sh$|scripts/test-perf-select\.py$|src/(agg|compat)/)'	# src/agg keeps the old names until it is renamed; src/compat forwards them
files() { git grep -I -l -e '' -- . | grep -Ev "$EXCLUDE" || true; }

read -r -d '' PERL <<'EOF' || true
use strict; use warnings;
my $areas = join '|', split ' ', $ENV{AREAS};
my %renamed = map { $_ => 1 } split ' ', $ENV{RENAMED};
my $bad = 0;
sub report { my ($f, $text, $pos, $msg) = @_; my $n = 1 + (() = substr($text, 0, $pos) =~ /\n/g); print "$f:$n: $msg\n"; $bad++ }
while (my $f = <STDIN>) {
	chomp $f;
	open my $in, '<:raw', $f or next;
	my $text = do { local $/; <$in> };
	close $in;
	my $head = $text;
	$head = $1 if $f =~ /CHANGELOG\.md$/ && $text =~ /^(.*?)(?=\n## [0-9])/s;
	while ($head =~ m{\bsrc/($areas)/([A-Za-z0-9_.]+)}g) {
		my ($a, $n, $pos) = ($1, $2, $-[0]);
		next unless $n =~ /\.(?:c|h|comp|py)$/ && $n !~ /\.$/;
		report($f, $head, $pos, "no such file src/$a/$n") unless -e "src/$a/$n";
		report($f, $head, $pos, "old name src/$a/$n (the files of $a/ have no sift_ prefix)") if $renamed{$a} && $n =~ /^sift_/;
	}
	while ($head =~ m{(?<![\w/.-])sift_(\w+\.(?:c|h))\b}g) {
		my ($n, $pos) = ($1, $-[0]);
		my @old = glob("src/*/sift_$n");
		next if @old;	# the file of an area that is not renamed yet
		for my $a (keys %renamed) { report($f, $head, $pos, "old name sift_$n (now $a/$n)") if -e "src/$a/$n" }
	}
}
exit($bad ? 1 : 0);
EOF

check() {
	local rc=0 dup
	check_layout || rc=1
	dup=$(src_files | sort | uniq -d)
	[[ -z $dup ]] || { echo "name used twice: $dup"; rc=1; }
	files | AREAS="$AREAS" RENAMED="$RENAMED" perl -e "$PERL" || rc=1
	return $rc
}

case "${1:-}" in
	--check) check ;;
	--map)   src_files | sed -E 's#^src/([^/]+)/(.*)#\1 \2#' | sort ;;
	*)       sed -n '2,13p' "$0"; exit 2 ;;
esac
