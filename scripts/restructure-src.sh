#!/usr/bin/env bash
# Moves the files of src/ into one folder per area (core/, bm25/, col/, agg/, table/; the reasons are DESIGN.md "Source layout") and
# updates every consumer. A pure move: the file names do not change, so no #include line changes (every build puts all the folders on the
# include path, src/areas.mk), `git diff -M` shows renames, and formats, SQL names and test results are those of the flat layout.
# Mechanical and idempotent: `git mv` by the MAP below, perl rules on the tracked text files, the EDITS table for the build files.
#
#   scripts/restructure-src.sh            move the files, rewrite the references, write src/areas.mk, add the documentation (once)
#   scripts/restructure-src.sh --check    exit 1 and list: a file in the wrong folder or that no MAP rule places, a name used twice, a
#                                         reference that still has the flat path, a reference to a file that does not exist, a build
#                                         line of the flat layout (src/*.c, -I$(SRC) ...). Run it after a merge, before the build.
#   scripts/restructure-src.sh --map      the placement of every file of src/ (area, name)
#
# Rebasing a branch onto a head that has it: git merge the head (git follows the renames: a file a branch edited in src/ is edited where it
# moved), run the script again (it moves files the branch added, rewrites the paths they use), --check. Run after
# scripts/rename-write-buffer.sh, or before it: both find the files wherever the other put them.
# A file of src/ that no rule places is an error: add its name pattern to the MAP.
#
# Not touched (history, or this script's own text): releases/, results/, docs/releases/, bench/results/, bench/xbench/, the two scripts
# of the renames, docs/NAMING.md; of a CHANGELOG.md only the Unreleased part.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

KEEP='portable.list portable-col.list areas.mk'		# the files that stay in src/ itself

# ---------------------------------------------------------------------------------------------------------------------------
# MAP: the folder of a file by its name (bash globs, first match wins; columns are tab separated: folder, patterns).
#   core   what every area shares: the module shell, the page and extent store, the write buffer lane chains, liveness and visibility,
#          the parallel build context, the checked reader, CPU dispatch, the host interface
#   bm25   the BM25 index access method and its engine: analyzer, SiftQL, bitmap codec, scoring, term blobs, dictionary, segments, runs,
#          merge policy, its write buffer, maintenance, build, evaluator, planner and executor nodes, facets
#   agg    SiftAgg, the aggregate pushdown: planner, spec, executor, operators, the column source seam and its providers over a match set
#   col    the column engine and the sift_col access method: chunk codec, kernels, shredder, units, segments, build, write buffer, maintenance
#   table  the sift_table table access method
# ---------------------------------------------------------------------------------------------------------------------------
read -r -d '' MAP <<'EOF' || true
core	sift.h sift_host.h sift_compat.h sift_cmp.h sift_sql.h sift_handler.c sift_ops.* sift_cpu.* sift_page.h sift_store.* sift_store_test.c sift_stream.c sift_span.* sift_varint.h sift_popcount.h sift_testrng.h sift_lane.* sift_live.* sift_visibility.* sift_filter.* sift_filter_test.c sift_parbuild.* sift_leak_test.c
table	sift_table*
agg	sift_agg* sift_colsrc.h sift_search.* sift_topn.*
col	sift_col* sift_shred*
bm25	sift_am.h sift_amscan.c sift_analyze* sift_bitmap* sift_build.* sift_cscan.c sift_dict.* sift_docdir.c sift_estimate.* sift_eval.* sift_facet.c sift_funcs.c sift_kernels* sift_maint.* sift_merge.* sift_mutable.* sift_wbuf.* sift_parallel.* sift_planner.* sift_posting.* sift_prefix.* sift_pushdown.* sift_query.* sift_read_test.c sift_runs.* sift_segment*
EOF

# ---------------------------------------------------------------------------------------------------------------------------
# EDITS: what the build files and a few documents need beyond the path rewrite. Columns are tab separated: scope (a regex on the path),
# pattern (perl regex, applied to the whole file), replacement ($1.. captures, \n and \t escapes). A pattern must not match its own
# output, so that a second run changes nothing. The path rewrite itself needs no rule: every `src/NAME`, `$(SRC)/NAME` and `${SRC}/NAME`
# of a file in src/ becomes `src/AREA/NAME` (globs too, when they stay in one folder).
# ---------------------------------------------------------------------------------------------------------------------------
read -r -d '' EDITS <<'EOF' || true
# .gitignore has `core` (core dumps): src/core/ must not match it, or a new file of the folder is not added by `git add`
^\.gitignore$	(?m)^(core\.\[0-9\]\*\n)(?!!/src/core/)	$1!/src/core/\n
# --- PGXS Makefile: the objects of every folder, the folders on the include path
^Makefile$	(?m)^OBJS = \$\(patsubst %\.c,%\.o,\$\(wildcard src/\*\.c\)\)$	SIFT_MK := $(lastword $(MAKEFILE_LIST))\nSRC = src\ninclude $(SRC)/areas.mk\nOBJS = $(patsubst %.c,%.o,$(wildcard $(SRC)/*/*.c))
^Makefile$	include \$\(dir \$\(lastword \$\(MAKEFILE_LIST\)\)\)scripts/harden\.mk	include $(dir $(SIFT_MK))scripts/harden.mk
^Makefile$	(?m)^PG_CPPFLAGS = -I\$\(srcdir\)/src$	PG_CPPFLAGS = $(addprefix -I$(srcdir)/,$(SIFT_DIRS))
^(Makefile|scripts/(isa_check|static)\.sh)$	\bsrc/\*\.([cdho])\b	src/*/*.$1
# --- the other Makefiles that compile src/: areas.mk gives them SIFT_DIRS, SIFT_INC and the vpath of the sources
^(tools/(tools\.mk|[a-z]+/Makefile)|test/fuzz/Makefile|sqlite/Makefile|mariadb/Makefile)$	(?m)^(SRC\s*:?=[^\n]*src)\n(?!include \$\(SRC\)/areas\.mk)	$1\ninclude $(SRC)/areas.mk\n
^(tools/(tools\.mk|[a-z]+/Makefile)|test/fuzz/Makefile|sqlite/Makefile|mariadb/Makefile)$	-I\$\(SRC\)(?![/\w])	$(SIFT_INC)
^tools/tools\.mk$	(?m)^(\t@mkdir -p )\$\(OUT\)$	$1$(@D)
^tools/codec/Makefile$	(?m)^(\t@mkdir -p )\$\(OUT\)/obj$	$1$(@D)
^tools/codec/Makefile$	(?m)^(CFLAGS  \+= -std=gnu11 -ffp-contract=off -Wall -Wextra -Wno-unused-function)$	$1 $(SIFT_INC)
^tools/colcodec/Makefile$	(?m)^vpath %\.c \$\(SRC\) \$\(KT\)$	vpath %.c $(KT)
^test/fuzz/Makefile$	\$\(SRC\)/\*\.h	$(SRC)/*/*.h
^test/fuzz/Makefile$	(?m)^\$\(OUT\)/%\.o: \$\(SRC\)/%\.c(?= \$\(HDR\))	$(OUT)/%.o: %.c
# --- SQLite and MariaDB: the portable lists name files by folder; the objects stay flat, the sources are found by name on the vpath
^(sqlite|mariadb)/Makefile$	(?<!\$\(notdir )\$\(basename (\$\(filter %\.c,\$\(shell grep -v '\^\[\\?#\]' \$\(SRC\)/portable(?:-col)?\.list\)\))\)	$(notdir $(basename $1))
^sqlite/Makefile$	(?m)^\$\(B\)/%\.o: \$\(SRC\)/%\.c \| \$\(B\)\n\t\$\(CC\) \$\(CFLAGS\) -c -o \$@ \$<\n\n
^sqlite/Makefile$	\$\(COLOBJS\): \$\(B\)/%\.o: \$\(SRC\)/%\.c	$(COLOBJS): $(B)/%.o: %.c
^mariadb/Makefile$	(?m)^\$\(OUT\)/%\.o: \$\(SRC\)/%\.c \| \$\(OUT\)$	PORTOBJS := $(filter-out $(OUT)/sift_bitmap.o,$(addprefix $(OUT)/,$(addsuffix .o,$(PORTABLE))))\n$(PORTOBJS): $(OUT)/%.o: %.c | $(OUT)
^mariadb/CMakeLists\.txt$	(?m)^(set\(SRC \$\{ROOT\}/src\)\n)(?!# the area folders)	$1# the area folders of src/: the sources include each other by bare name\nfile(GLOB sift_entries RELATIVE ${SRC} ${SRC}/*)\nset(SIFT_DIRS "")\nforeach(e ${sift_entries})\n  if(IS_DIRECTORY ${SRC}/${e})\n    list(APPEND SIFT_DIRS ${SRC}/${e})\n  endif()\nendforeach()\n
^mariadb/CMakeLists\.txt$	REMOVE_ITEM portable sift_bitmap\.c	REMOVE_ITEM portable bm25/sift_bitmap.c
^mariadb/CMakeLists\.txt$	(target_include_directories\(sift_engine PRIVATE [^\n]*) \$\{SRC\}\)	$1 ${SIFT_DIRS})
# the MariaDB regex test needs only the host header of the core
^mariadb/tests/regex_check\.sh$	-I"\$HERE/\.\./src"(?![/\w])	-I"$HERE/../src/core"
# --- scripts: the portable check compares the include names with the names of the lists; static analysis puts every folder on the include path
^scripts/check-portable\.sh$	(?m)^(mapfile -t files [^\n]*\n)(?!names=)	$1names=("${files[@]##*/}")\n
^scripts/check-portable\.sh$	" \$\{files\[\*\]\} " == \*	" ${names[*]} " == *
^scripts/static\.sh$	(?m)^CFLAGS=\(-std=gnu11 -I"\$REPO/src"(?= -isystem)	INC=(); for d in "$REPO"/src/*/; do INC+=(-I"${d%/}"); done\nCFLAGS=(-std=gnu11 "${INC[@]}"
^scripts/static\.sh$	-I src --suppress=missingInclude	"${INC[@]}" --suppress=missingInclude
# the benchmark builds a reference tree (a release, flat or in folders): every .c of its src/
^bench/steps/fetch\.sh$	"\$src"/src/\*\.c	$(find "$src/src" -name '*.c')
# --- documents
^docs/COL-PLAN\.md$	`src/sift_col\*`, not changed here	`src/col/`, not changed here
^DESIGN\.md$	Headers live in `src/`\.	Headers live next to their sources in `src/<area>/` (Source layout above).
^docs/COL-PORTS\.md$	(?s)6\. \*\*File naming\*\*: a later restructure moves `src/` files into subfolders \(`col/`, `agg/`, `table/`\) instead of `sift_col_\*`\n   prefixes; this work adds no new prefixed names \(`sift_col_pg\.h` is the one exception and moves with its siblings\), and the file names\n   quoted above are those of today\.	6. **File naming**: `src/` is split into one folder per area (`core/`, `bm25/`, `col/`, `agg/`, `table/`, DESIGN.md "Source layout"); the files keep their\n   names, which the text above quotes, so a file is named by its bare name in the docs.
EOF

AREAS_MK=$(cat <<'EOF'
# The area folders of src/ (DESIGN.md "Source layout"). Included by every Makefile that compiles files of src/, after it has set SRC (the
# path to src/). A source includes another by its bare name (the file names are unique across the folders): SIFT_INC puts every folder on the
# include path, SIFT_DIRS is the list of them and is on the vpath, so that a pattern rule finds a source by its name.
SIFT_DIRS := $(patsubst %/,%,$(wildcard $(SRC)/*/))
SIFT_INC  := $(addprefix -I,$(SIFT_DIRS))
vpath %.c $(SIFT_DIRS)
EOF
)

LAYOUT=$(cat <<'EOF'
## Source layout

`src/` has one folder per area. The file names are unchanged and unique across the folders, so a source includes another by its bare name
(`#include "sift_page.h"`): every build puts all the folders on the include path (`src/areas.mk`: `SIFT_DIRS`, `SIFT_INC`; `static.sh` and the
SQLite and MariaDB builds do the same). The folder of a file follows from its name, by the table at the top of `scripts/restructure-src.sh`,
whose `--check` fails for a file no rule places and for a path that went stale. `src/portable.list` and `src/portable-col.list` name files with
their folder (`bm25/sift_bitmap.c`); the SQLite and MariaDB builds and `scripts/check-portable.sh` read them.

| Folder | Holds | Includes from |
|---|---|---|
| `core/` | what every area shares: the module shell (`sift_handler.c`: `_PG_init`, GUCs, reloptions, catalog cache, SQL entry points), the page and extent store (`sift_page.h`, `sift_store.*`, `sift_stream.c`), the lane chains of a write buffer (`sift_lane.*`), liveness pages, visibility, the context of a parallel build, the checked reader and varints, CPU dispatch, `sift_host.h` | the other areas only in `sift_handler.c`, which registers them |
| `bm25/` | the BM25 index access method `sift` and its engine: analyzer, SiftQL, bitmap codec, scoring kernels, term blobs, dictionary, segments, sorted runs, merge policy, its write buffer (`sift_wbuf.*`), maintenance, build, evaluator, planner and the SiftScan and SiftCount nodes, facets | `core/` |
| `col/` | the column engine and the `sift_col` access method: chunk codec, column kernels, shredder, unit encoding, segments, build, its write buffer (`sift_col_wbuf*`), maintenance, column source, verify | `core/`; `bm25/` (maintenance, merge policy and kernel infrastructure it reuses) |
| `agg/` | SiftAgg, the aggregate pushdown: planner, spec, executor, aggregation kernels, the column source seam (`sift_colsrc.h`) with its providers, a BM25 match set as the row selection, Top-N, the `sift_colstub` access method | `core/`, `col/`, `bm25/` |
| `table/` | the `sift_table` table access method (docs/COL-TAM.md) | `core/`, `col/`, `agg/` (its column source), `bm25/` (maintenance) |

A file goes to the area that owns it. What two areas share goes to `core/` when it needs nothing of either; otherwise it stays with its owner
and the other area includes it (`col/` uses the maintenance and merge policy of `bm25/`) instead of keeping a copy. The portable core
(`src/portable.list`) is mostly `bm25/`, with the shared pieces of `core/`; the column track's list (`src/portable-col.list`) is `col/` and
the aggregation kernels of `agg/`.
EOF
)

CONTRIB='`src/` has one folder per area (`core/`, `bm25/`, `col/`, `agg/`, `table/`; DESIGN.md "Source layout"). A source includes another by its bare name, and a new file goes to the folder its name selects (`scripts/restructure-src.sh --check` fails for a name no rule places: add the pattern to its MAP).'

# the folder of a file name, empty when no rule places it
area_of() {
	local name=$1 area globs g
	while IFS=$'\t' read -r area globs; do
		for g in $globs; do
			# shellcheck disable=SC2053
			[[ $name == $g ]] && { echo "$area"; return; }
		done
	done <<<"$MAP"
}

# the tracked files of src/ that are placed by name
src_files() { git ls-files -- src | grep -v -E "^src/($(tr ' ' '|' <<<"$KEEP"))\$" || true; }

# name <TAB> folder, for every file of src/
name_map() {
	local f name
	while read -r f; do name=${f##*/}; printf '%s\t%s\n' "$name" "$(area_of "$name")"; done < <(src_files)
}

# the tracked text files that are rewritten; CHANGELOGs only in their Unreleased part (perl)
EXCLUDE='^(releases/|results/|docs/releases/|bench/results/|bench/xbench/|LICENSE$|scripts/rename-write-buffer\.sh$|scripts/restructure-src\.sh$|docs/NAMING\.md$)'
files() { git grep -I -l -e '' -- . | grep -Ev "$EXCLUDE" || true; }

read -r -d '' PERL <<'EOF' || true
use strict; use warnings;
my $mode = $ENV{MODE};
my %area;
open my $m, '<', $ENV{AREAFILE} or die "$ENV{AREAFILE}: $!";
while (<$m>) { chomp; my ($n, $a) = split /\t/; $area{$n} = $a if length $a }
my $alt = join '|', sort keys %{{ map { $_ => 1 } values %area }};
my @edits;
for (split /\n/, $ENV{EDITS}) {
	next if /^\s*(#|$)/;
	my ($s, $p, $r) = split /\t/, $_, 3;
	$r //= ''; $r =~ s/\\n/\n/g; $r =~ s/\\t/\t/g;
	push @edits, [qr/$s/, qr/$p/, $r];
}
my $tok = qr{sift[A-Za-z0-9_*\[\]]*(?:\.[A-Za-z0-9_*\[\]]+)*};
my $pre = qr{(?:\bsrc|\$\(SRC\)|\$\{SRC\}|\$SRC)/};
# the folder of a name, or of a glob when every file it matches is in the same one
sub area_for {
	my ($t) = @_;
	return $area{$t} if exists $area{$t};
	return undef unless $t =~ /[*\[]/;
	(my $re = $t) =~ s/\./\\./g; $re =~ s/\*/[A-Za-z0-9_.]*/g;
	my %a = map { $area{$_} => 1 } grep { /^$re$/ } keys %area;
	return keys %a == 1 ? (keys %a)[0] : undef;
}
sub expand { my ($t, @c) = @_; $t =~ s{\$(\d)}{ $c[$1 - 1] // '' }ge; $t }
my @flat = (
	[qr{\bsrc/\*\.[cdho]\b}, 'a glob of the flat layout'],
	[qr{-I\$\(SRC\)(?![/\w])}, 'the include path of the flat layout (use $(SIFT_INC))'],
	[qr{-I\$\(srcdir\)/src(?![/\w])}, 'the include path of the flat layout'],
	[qr{-I"\$REPO/src"}, 'the include path of the flat layout'],
	[qr{-I src\b}, 'the include path of the flat layout'],
	[qr{-I"\$HERE/\.\./src"}, 'the include path of the flat layout'],
);
my $bad = 0;
sub report { my ($f, $text, $pos, $msg) = @_; my $n = 1 + (() = substr($text, 0, $pos) =~ /\n/g); print "$f:$n: $msg\n"; $bad++ }
while (my $f = <STDIN>) {
	chomp $f;
	open my $in, '<:raw', $f or next;
	my $text = do { local $/; <$in> };
	close $in;
	my ($head, $tail) = ($text, '');
	($head, $tail) = ($1, $2) if $f =~ /CHANGELOG\.md$/ && $text =~ /^(.*?)(\n## [0-9].*)\z/s;
	if ($mode eq 'rewrite') {
		for my $e (@edits) {
			next unless $f =~ $e->[0];
			$head =~ s/$e->[1]/expand($e->[2], @{^CAPTURE})/ge;
		}
		$head =~ s{^(sift[\w.]*)$}{ exists $area{$1} ? "$area{$1}/$1" : $1 }gme if $f =~ m{^src/portable(?:-col)?\.list$};
		$head =~ s{($pre)($tok)}{ my $a = area_for($2); defined $a ? "$1$a/$2" : "$1$2" }ge;
		my $new = $head . $tail;
		next if $new eq $text;
		open my $out, '>:raw', $f or die "$f: $!";
		print $out $new;
		close $out;
		print "rewrote $f\n";
	} else {
		while ($head =~ /($pre)($tok)/g) {
			my ($p, $t, $pos) = ($1, $2, $-[0]);
			my $a = area_for($t);
			if ($t =~ /[*\[]/) { report($f, $head, $pos, "glob $p$t spans folders: name the folder") if !defined $a && $t ne 'sift_*.c' }
			elsif (defined $a) { report($f, $head, $pos, "stale path $p$t (the file is in $a/)") }
		}
		while ($head =~ m{\bsrc/($alt)/($tok)}g) {
			my ($d, $n, $pos) = ($1, $2, $-[0]);
			report($f, $head, $pos, "no such file src/$d/$n") unless $n =~ /[*\[]/ || -e "src/$d/$n";
		}
		next if $f =~ m{^src/portable(?:-col)?\.list$};
		for my $r (@flat) { while ($head =~ /$r->[0]/g) { report($f, $head, $-[0], $r->[1]) } }
	}
}
exit($mode eq 'check' && $bad ? 1 : 0);
EOF

rewrite() { files | MODE=rewrite AREAFILE=$1 EDITS="$EDITS" perl -e "$PERL"; }

# src/areas.mk, once
write_areas_mk() { [[ -e src/areas.mk ]] || { printf '%s\n' "$AREAS_MK" >src/areas.mk; echo "wrote src/areas.mk"; }; git add -- src/areas.mk; }

# git mv every file of src/ to its folder (also moves a file whose rule changed)
moves() {
	local f name area dst bad=0
	while read -r f; do
		name=${f##*/} area=$(area_of "$name")
		[[ -n $area ]] || { echo "unmapped: $f (add its name to the MAP of $0)" >&2; bad=1; continue; }
		dst=src/$area/$name
		[[ $f == "$dst" ]] && continue
		mkdir -p "src/$area"
		git mv "$f" "$dst"
		echo "moved $f -> $dst"
	done < <(src_files)
	return $bad
}

# the objects and dependency files of the flat layout (untracked build products) would only mislead
clean_flat() { rm -f src/*.o src/*.d; }

# the documentation: DESIGN.md "Source layout", the paragraph of CONTRIBUTING.md (once each)
docs() {
	grep -q '^## Source layout' DESIGN.md || LAYOUT="$LAYOUT" perl -0pi -e 's{^(## MODULE MAP\n)}{$ENV{LAYOUT}\n\n---\n\n$1}m or die "DESIGN.md: no MODULE MAP section"' DESIGN.md
	grep -q 'restructure-src\.sh' CONTRIBUTING.md || CONTRIB="$CONTRIB" perl -0pi -e 's{^(Before sending a change:\n)}{$ENV{CONTRIB}\n\n$1}m or die "CONTRIBUTING.md: no Before sending a change"' CONTRIBUTING.md
}

check() {
	local rc=0 f name area dup tmp
	while read -r f; do
		name=${f##*/} area=$(area_of "$name")
		if [[ -z $area ]]; then echo "$f: no MAP rule places this file"; rc=1
		elif [[ $f != "src/$area/$name" ]]; then echo "$f: belongs in src/$area/"; rc=1; fi
	done < <(src_files)
	if git check-ignore -q src/core/sift.h; then echo ".gitignore: src/core/ is ignored (the core-dump pattern): the EDITS table un-ignores it"; rc=1; fi
	dup=$(src_files | sed 's#.*/##' | sort | uniq -d)
	[[ -z $dup ]] || { echo "name used twice (bare includes need unique names): $dup"; rc=1; }
	tmp=$(mktemp)
	name_map >"$tmp"
	files | MODE=check AREAFILE=$tmp EDITS="$EDITS" perl -e "$PERL" || rc=1
	rm -f "$tmp"
	return $rc
}

case "${1:-}" in
	--check) check; exit ;;
	--map)   name_map | sort -k2,2 -k1,1 | awk -F'\t' '{ printf "%-6s %s\n", $2, $1 }'; exit ;;
	"")      ;;
	*)       sed -n '2,18p' "$0"; exit 2 ;;
esac

write_areas_mk
moves
clean_flat
tmp=$(mktemp)
name_map >"$tmp"
rewrite "$tmp"
rm -f "$tmp"
docs
echo "done; next: scripts/restructure-src.sh --check, then build"
