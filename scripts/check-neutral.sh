#!/usr/bin/env bash
# Neutral wording guard: this public repository names no private product, machine or application domain, in a file (its name or its text), in the subject or
# body of a commit of the branch, or in the title or body of its pull request. Three lists, all encoded (the words never appear in the repository):
#   scripts/neutral.denylist   product-specific and machine names, base64 one per line, matched case-insensitively as a substring
#   scripts/neutral.words      the vocabulary of an application domain and its schema, base64 one per line, matched case-insensitively as whole words
#   (in the script)            network addresses of private ranges, local domain names and home directories of a person
# scripts/neutral.allow lists the generic uses of these words that stay (path and text patterns, with the reason). Loopback and unspecified addresses are not hits.
#
#   scripts/check-neutral.sh              the tracked files of the work tree, the commits of origin/main..HEAD (a branch without origin/main skips them) and,
#                                         when NEUTRAL_PR_TITLE or NEUTRAL_PR_BODY is set, the pull request text (the CI passes them through the environment)
#   scripts/check-neutral.sh --selftest   every list entry is flagged, the allowlist, a neutral text and the repository pass
#
# Exit 1 with the file and line, the commit or the pull request part of every hit (the word itself is not printed).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
PAT=$(mktemp) WORDS=$(mktemp)
trap 'rm -f "$PAT" "$WORDS"' EXIT
decode() { grep -v -e '^#' -e '^$' "$1" | while read -r w; do printf '%s' "$w" | base64 -d; echo; done; }
decode "$REPO/scripts/neutral.denylist" >"$PAT"
decode "$REPO/scripts/neutral.words" >"$WORDS"

# match: stdin holds a name of the denylist
match() { grep -q -i -F -f "$PAT"; }

# scan MODE [PATH LABEL]: the words and the addresses, minus the allowlist. names and files read NUL separated paths from stdin; text reads a message whose
# pseudo path (<commit>, <pr>) selects the rows of the allowlist
read -r -d '' PERL <<'EOF' || true
use strict; use warnings;
my ($mode, $words, $allow, $path, $label) = @ARGV;
open my $w, '<', $words or die "$words: $!"; chomp(my @w = <$w>); close $w;
my $dom = join '|', map { quotemeta } sort { length $b <=> length $a } grep { length } @w;
$dom = qr/(?<![A-Za-z0-9])(?:$dom)(?![A-Za-z0-9])/i;
my $oct = qr/[0-9]{1,3}/;
my $net = qr{\b(?:10(?:\.$oct){3}|192\.168(?:\.$oct){2}|172\.(?:1[6-9]|2[0-9]|3[01])(?:\.$oct){2})\b|\.la[n]\b|/home/[a-z]|/Users/[a-z]};
my @allow;
open my $a, '<', $allow or die "$allow: $!";
while (<$a>) { chomp; next if /^#/ || !length; my ($p, $t) = split /\t/; push @allow, [qr/$p/, qr/$t/]; }
close $a;
my $bad = 0;
sub scan {
	my ($p, $line, $where) = @_;
	for my $e (@allow) { $line =~ s/$e->[1]/ /g if $p =~ $e->[0]; }
	for ([$dom, 'a domain word'], [$net, 'a private address or host']) {
		next unless $line =~ $_->[0];
		print "neutral: $where holds $_->[1]\n"; $bad = 1;
	}
}
if ($mode eq 'text') {
	local $/; my $s = <STDIN>; my $n = 0;
	scan($path, $_, "$label, line " . ++$n) for split /\r?\n/, $s;
} else {
	local $/ = "\0";
	while (my $f = <STDIN>) {
		chomp $f;
		if ($mode eq 'names') { scan($f, $f, "the name of $f"); next }
		open my $fh, '<:raw', $f or next;
		local $/ = "\n";
		while (<$fh>) { chomp; scan($f, $_, "$f:$.") }
		close $fh;
	}
}
exit $bad;
EOF
scan() { perl -e "$PERL" "$1" "$WORDS" "$REPO/scripts/neutral.allow" "${2:-}" "${3:-}"; }

# check_text PATH LABEL: stdin holds a commit message or a pull request part
check_text() {
	local t; t=$(cat)
	! match <<<"$t" || { echo "neutral: $2 holds a name of the denylist"; return 1; }
	printf '%s\n' "$t" | scan text "$1" "$2"
}

check_pr() {
	local bad=0
	[[ -z ${NEUTRAL_PR_TITLE+x} ]] || printf '%s' "$NEUTRAL_PR_TITLE" | check_text '<pr>' "the pull request title" || bad=1
	[[ -z ${NEUTRAL_PR_BODY+x} ]] || printf '%s' "$NEUTRAL_PR_BODY" | check_text '<pr>' "the pull request body" || bad=1
	return $bad
}

check_repo() {
	local bad=0 base sha names
	cd "$REPO"
	# the files: the denylist and the words are encoded, the allowlist is only patterns; every other file is read as text (binary files are skipped)
	names=$(git ls-files | grep -v -x scripts/neutral.denylist)
	if match <<<"$names"; then echo "neutral: a file name holds a name of the denylist"; bad=1; fi
	git ls-files -z | scan names || bad=1
	git grep -I -i -n -F -f "$PAT" -- . ':(exclude)scripts/neutral.denylist' | cut -d: -f1,2 | sed 's/^/neutral: a name of the denylist at /' | grep . && bad=1
	git grep -I -l -z -e '' -- . ':(exclude)scripts/neutral.denylist' ':(exclude)scripts/neutral.words' ':(exclude)scripts/neutral.allow' | scan files || bad=1
	# the commits of the branch only: the history before it is not judged
	if base=$(git merge-base origin/main HEAD 2>/dev/null); then
		while read -r sha; do
			git log -1 --format=%B "$sha" | check_text '<commit>' "commit ${sha:0:9}" || bad=1
		done < <(git log --format=%H "$base..HEAD")
	fi
	check_pr || bad=1
	return $bad
}

selftest() {
	local w bad=0 n=0 nw=0 prop ip home host
	unset NEUTRAL_PR_TITLE NEUTRAL_PR_BODY
	# fails WHAT PATH TEXT: the line is flagged; passes: it is not
	fails() { printf '%s\n' "$3" | scan text "$2" "$1" >/dev/null && { echo "FAIL: $1 is not flagged"; bad=1; } || true; }
	passes() { printf '%s\n' "$3" | scan text "$2" "$1" >/dev/null || { echo "FAIL: $1 is flagged"; bad=1; }; }
	while IFS= read -r w; do
		n=$((n + 1))
		match <<<"x ${w^^} y" || { echo "FAIL: the name $n of the denylist is not matched"; bad=1; }
	done <"$PAT"
	while IFS= read -r w; do
		nw=$((nw + 1))
		fails "the word $nw of the vocabulary" test/sql/x.sql "select 1 -- ${w^^}, $w."
	done <"$WORDS"
	(( n >= 10 && nw >= 20 )) || { echo "FAIL: the lists hold $n and $nw entries"; bad=1; }
	if match <<<"fact table, entities, entity_id, group_members, set filter, membership table, CREATE SUBSCRIPTION, source_identity"; then
		echo "FAIL: a neutral sample matches the denylist"; bad=1
	fi
	passes "a neutral text" test/sql/x.sql "fact table, entities, entity_id, group_members, set filter, membership table, source_identity, 127.0.0.1, 0.0.0.0, ::1, localhost"
	# the allowlist: a row selects a path and a text; the same text elsewhere, or next to a forbidden word, is a hit
	prop=$(grep -i -x -m1 'prop[e]rty' "$WORDS")
	passes "an allowed use in its file" bench/steps/cell.py "    @$prop"
	fails "the same use in another file" bench/steps/other.py "    @$prop"
	fails "an allowed use next to a forbidden word" bench/steps/cell.py "    @$prop # $(head -n1 "$WORDS")"
	passes "a generic text in a commit message" '<commit>' "address the code review of the plan"
	fails "the same text in a file" docs/X.md "address the code review of the plan"
	# the addresses: private ranges, local domain names and home directories fail; loopback and the inet test data pass
	ip=$(printf '%s.%s.%s.%s' 10 1 2 3); home=/home; host=$(printf 'box.%s' lan)
	fails "a private address" docs/X.md "connect to $ip"
	fails "a private 192 address" test/x.sql "connect to $(printf '%s.%s.%s.%s' 192 168 7 7)"
	fails "a private 172 address" scripts/x.sh "connect to $(printf '%s.%s.%s.%s' 172 20 0 9)"
	fails "a local domain name" scripts/x.sh "ssh $host"
	fails "a home directory" scripts/x.sh "cd $home/someone/src"
	fails "a test address outside the data" test/sql/x.sql "select '$(printf '%s.%s.%s.%s' 10 0 7 9)'::inet"
	passes "the inet test data" test/sql/x.sql "select '$(printf '%s.%s.%s.%s' 10 0 0 1)/24'::inet"
	# the text of a commit or a pull request
	w=$(head -n1 "$PAT")
	( check_text '<commit>' "commit x" <<<"ci: neutral: a $w" ) >/dev/null 2>&1 && { echo "FAIL: a commit with a name of the denylist passes"; bad=1; }
	( check_text '<commit>' "commit x" <<<"ci: neutral: describe the change" ) >/dev/null 2>&1 || { echo "FAIL: a neutral commit is flagged"; bad=1; }
	( NEUTRAL_PR_TITLE="ci: a $w" check_pr ) >/dev/null 2>&1 && { echo "FAIL: a pull request title with a name passes"; bad=1; }
	( NEUTRAL_PR_BODY=$'summary\nthe '"$(head -n1 "$WORDS")"' table' check_pr ) >/dev/null 2>&1 && { echo "FAIL: a pull request body with a domain word passes"; bad=1; }
	( NEUTRAL_PR_BODY="a body to $ip" check_pr ) >/dev/null 2>&1 && { echo "FAIL: a pull request body with an address passes"; bad=1; }
	( NEUTRAL_PR_TITLE="ci: neutral: describe the change" NEUTRAL_PR_BODY=$'summary\n\nthe code review of the plan' check_pr ) >/dev/null 2>&1 || { echo "FAIL: a neutral pull request is flagged"; bad=1; }
	( unset NEUTRAL_PR_TITLE NEUTRAL_PR_BODY; check_pr ) || { echo "FAIL: no pull request text is flagged"; bad=1; }
	check_repo || bad=1
	(( bad )) || echo "neutral-selftest: $n names and $nw words flagged, the allowlist, the addresses, the commit and pull request text, and the repository pass"
	return $bad
}

case ${1:-} in
	--selftest) selftest ;;
	"") check_repo && echo "neutral: ok" ;;
	*) sed -n '2,13p' "$0"; exit 1 ;;
esac
