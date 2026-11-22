#!/usr/bin/env bash
# Every lock that src/ takes is justified in scripts/locks.tsv: a heavyweight lock (LockRelation*, LockPage, LockTuple, LockDatabaseObject, LockAcquire, the advisory gates), a relation
# opened above RowExclusiveLock, an LWLock, a wait for a transaction. A row names the call site (file, function, call, mode, how many), the object locked, the scope (call, statement,
# transaction, session), the longest it is held, the lock core PostgreSQL takes for the same kind of work, and the verdict: "same" when the mode and the hold are no stronger or longer than
# PostgreSQL's, otherwise what it conflicts with and why it must be so. A new acquisition, a changed mode or a function that no longer takes the lock fails here until the table is edited,
# so a lock that keeps writers or readers out is a decision somebody wrote down. The relation extension lock is taken only through core/extlock.c (scripts/check-extlock.sh fails any other
# call of the PostgreSQL function): its rows are the callers of sift_extension_lock and sift_extend_zeroed_to, and the one row of the primitive in extlock.c.
#
# Two rules need no table:
#   - the mode of an unlock or a LockRelease is a lock mode name or a variable, never the result of a call: a mode recomputed at the release can differ from the one taken (the state it
#     reads changed while the lock was held), PostgreSQL reports "you don't own a lock of type ..." and the lock stays to the end of the transaction;
#   - between SpinLockAcquire and SpinLockRelease nothing allocates, reports, waits, reads a buffer or takes a lock: a spinlock is for a few loads and stores.
#
#   scripts/check-locks.sh              the tracked sources of src/
#   scripts/check-locks.sh --list       the acquisitions found, as rows of the table with the columns after n empty: to add one, fill them in
#   scripts/check-locks.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

# Lock modes that cannot keep a reader or a writer of the table out except against the DDL locks: opening a relation with them needs no row
BENIGN_OPEN='NoLock|AccessShareLock|RowExclusiveLock'

# acquisitions FILE...: file<TAB>function<TAB>call<TAB>mode for every acquisition that needs a row, and "rule<TAB>file:line<TAB>text" for a line that breaks one of the two rules
scan() {
	awk -v benign="^($BENIGN_OPEN)\$" '
	function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); gsub(/[ \t]+/, " ", s); return s }
	function modeindex(call) {
		if (call ~ /^(LockRelationOid|LockRelation|ConditionalLockRelationOid|ConditionalLockRelation|LockRelationForExtension|ConditionalLockRelationForExtension|RangeVarGetRelidExtended|LWLockAcquire|LWLockConditionalAcquire|LockAcquire|LockAcquireExtended|LockRelationIdForSession|table_open|index_open|relation_open|try_table_open|try_index_open|try_relation_open|sift_keystore_open|sift_try_open_index|sift_col_open_index)$/) return 2
		if (call ~ /^(LockPage|ConditionalLockPage|LockTuple|ConditionalLockTuple)$/) return 3
		if (call ~ /^(LockDatabaseObject|LockSharedObject|ConditionalLockDatabaseObject)$/) return 4
		if (call ~ /^(UnlockRelationOid|UnlockRelation|UnlockRelationForExtension|LockRelease)$/) return 2
		if (call ~ /^(UnlockPage|UnlockTuple|UnlockDatabaseObject|UnlockSharedObject)$/) return 3
		return 0
	}
	function nargs(text, call,    i, c, depth, cur, start, n) {
		start = index(text, call "(") + length(call) + 1
		depth = 1; n = 1; cur = ""
		for (i = start; i <= length(text); i++) {
			c = substr(text, i, 1)
			if (c == "(") depth++
			else if (c == ")") { depth--; if (depth == 0) break }
			if (c == "," && depth == 1) { ARG[n++] = trim(cur); cur = ""; continue }
			cur = cur c
		}
		ARG[n] = trim(cur)
		return n
	}
	function statement(i,    t, j) {
		t = L[i]
		for (j = i + 1; j <= nl && j < i + 8; j++) {
			if (gsub(/\(/, "(", t) <= gsub(/\)/, ")", t)) break
			t = t " " L[j]
		}
		return t
	}
	function flush(   i, k, line, text, call, m, mode, key, mi, name) {
		for (i = 1; i <= nl; i++) {
			line = L[i]
			if (line ~ /^[ \t]*(\/\*|\*|\/\/)/) continue
			sub(/\/\*.*\*\//, "", line)
			if (line ~ /^#define[ \t]+[A-Za-z_]/) { name = line; sub(/^#define[ \t]+/, "", name); sub(/[^A-Za-z0-9_].*$/, "", name); FN[i] = "#" name }
			else if (line ~ /^[A-Za-z_][^;]*\(/ && line !~ /;[ \t]*$/ && line !~ /^(typedef|extern|return|else|if|for|while|switch|goto|case)[ \t(]/) { name = line; sub(/\(.*$/, "", name); sub(/^.*[^A-Za-z0-9_]/, "", name); cur = name; FN[i] = cur }
			else FN[i] = (line ~ /^#/ ? FN[i - 1] : cur)
			text = ""
			line_full = L[i]
			while (match(line, /[A-Za-z_][A-Za-z0-9_]*\(/)) {
				call = substr(line, RSTART, RLENGTH - 1)
				line = substr(line, RSTART + RLENGTH)
				if (call == "SpinLockAcquire") { spin = 1; spinline = i; continue }
				if (call == "SpinLockRelease") { spin = 0; continue }
				if (spin && call ~ /^(palloc|palloc0|repalloc|MemoryContextAlloc|MemoryContextAllocZero|elog|ereport|ReadBuffer|ReadBufferExtended|LockBuffer|XLogInsert|pg_usleep|WaitLatch|LWLockAcquire|hash_search|SearchSysCache1|SearchSysCache|table_open|index_open|relation_open|LockRelationOid|LockRelation|LockPage|LockTuple|LockAcquire|LockRelease|dsa_allocate|dsa_allocate0|BufferGetPage|XactLockTableWait)$/)
					printf "rule\t%s:%d\tspinlock section taken at line %d calls %s: nothing that allocates, reports, waits, reads a buffer or locks belongs between SpinLockAcquire and SpinLockRelease\n", file, i, spinline, call
				if (call == "sift_extension_lock" || call == "sift_extend_zeroed_to") {
					if (line_full !~ /^(extern|[A-Za-z_])/) printf "row\t%s\t%s\t%s\tExclusiveLock\n", file, FN[i], call
					continue
				}
				mi = modeindex(call)
				if (mi == 0 && call !~ /^(XactLockTableWait|ConditionalXactLockTableWait|LockAcquire)$/) continue
				text = statement(i)
				n = nargs(text, call)
				if (call ~ /^(UnlockRelationOid|UnlockRelation|UnlockRelationForExtension|LockRelease|UnlockPage|UnlockTuple|UnlockDatabaseObject|UnlockSharedObject)$/) {
					if (ARG[mi] ~ /\(/)
						printf "rule\t%s:%d\t%s releases a lock whose mode is the result of a call (%s): keep the mode the lock was taken with in a variable (a mode recomputed at the release can differ, PostgreSQL then reports \"you do not own a lock\" and keeps the lock to the end of the transaction)\n", file, i, call, ARG[mi]
					continue
				}
				if (call ~ /^(XactLockTableWait|ConditionalXactLockTableWait)$/) mode = "wait"
				else mode = ARG[mi]
				if (mode ~ /LOCKMODE/) continue
				if (call ~ /^(table_open|index_open|relation_open|try_table_open|try_index_open|try_relation_open|sift_keystore_open|sift_try_open_index|sift_col_open_index)$/ && mode ~ benign) continue
				printf "row\t%s\t%s\t%s\t%s\n", file, FN[i], call, mode
			}
		}
	}
	FNR == 1 && NR > 1 { flush() }
	FNR == 1 { nl = 0; cur = "-"; spin = 0; file = FILENAME; delete L; delete FN }
	{ L[++nl] = $0 }
	END { flush() }
	' "$@"
}

# rows FILE...: the acquisitions with their counts, as file<TAB>function<TAB>call<TAB>mode<TAB>n
rows() { scan "$@" | awk -F'\t' '$1 == "row" { k = $2 "\t" $3 "\t" $4 "\t" $5; n[k]++; if (n[k] == 1) order[++c] = k } END { for (i = 1; i <= c; i++) print order[i] "\t" n[order[i]] }' | LC_ALL=C sort; }

# verify TABLE FILE...: the problems, one a line
verify() {
	local table=$1 found keys
	shift
	found=$(rows "$@")
	{
		scan "$@" | awk -F'\t' '$1 == "rule" { print $2 ": " $3 }'
		awk -F'\t' '
			NR == FNR { if ($0 !~ /^#/ && NF) { have[$1 "\t" $2 "\t" $3 "\t" $4] = $5; if (NF < 10) bad[$1 "\t" $2 "\t" $3 "\t" $4] = "has " NF " columns, 10 are needed"; else for (i = 6; i <= 10; i++) if ($i == "") bad[$1 "\t" $2 "\t" $3 "\t" $4] = "has an empty column " i; seen[$1 "\t" $2 "\t" $3 "\t" $4] = 0 } next }
			{ k = $1 "\t" $2 "\t" $3 "\t" $4; if (!(k in have)) print $1 ": " $2 " takes " $3 "(" $4 ") x" $5 " and scripts/locks.tsv has no row for it: add one (object, scope, longest hold, the PostgreSQL equivalent and the verdict), and a lock that is stronger or held longer than that of PostgreSQL needs its reason, or the lock is the wrong design"
				else { seen[k] = 1; if (have[k] != $5) print $1 ": " $2 " takes " $3 "(" $4 ") x" $5 " and the row says x" have[k]; if (k in bad) print $1 ": the row of " $2 " " $3 "(" $4 ") " bad[k] } }
			END { for (k in seen) if (!seen[k]) { split(k, p, "\t"); print p[1] ": the row for " p[2] " " p[3] "(" p[4] ") matches no acquisition any more: delete it" } }
		' "$table" <(printf '%s\n' "$found")
	} | LC_ALL=C sort -u
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d)
	trap 'rm -rf "$d"' EXIT
	: >"$d/e0.tsv"
	cat >"$d/a.c" <<'EOF'
static void
take(Relation rel)
{
	LockPage(rel, 3, ExclusiveLock);
	UnlockPage(rel, 3, ExclusiveLock);
}
EOF
	cat >"$d/b.c" <<'EOF'
void
cut(Relation rel)
{
	LockRelationOid(RelationGetRelid(rel),
					ShareUpdateExclusiveLock);
	UnlockRelation(rel, cut_lock(rel));
	index = index_open(oid, RowExclusiveLock);
	heap = table_open(oid, AccessExclusiveLock);
}
EOF
	cat >"$d/c.c" <<'EOF'
void
count(Shared *s)
{
	SpinLockAcquire(&s->mutex);
	s->n++;
	SpinLockRelease(&s->mutex);
	SpinLockAcquire(&s->mutex);
	s->p = palloc(8);
	SpinLockRelease(&s->mutex);
}
EOF
	printf 'void\next(Relation rel)\n{\n\tsift_extension_lock(rel);\n}\n' >"$d/x.c"
	verify "$d/e0.tsv" "$d/x.c" 2>/dev/null | grep 'sift_extension_lock' >/dev/null || { echo "check-locks: selftest: the extension lock helper passed"; exit 1; }
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$d/a.c" take LockPage ExclusiveLock 1 page scope call pg same >"$d/t.tsv"
	[[ -z $(verify "$d/t.tsv" "$d/a.c") ]] || { echo "check-locks: selftest: a listed lock was reported: $(verify "$d/t.tsv" "$d/a.c")"; exit 1; }
	: >"$d/e.tsv"
	[[ $(verify "$d/e.tsv" "$d/a.c" | wc -l) -eq 1 ]] || { echo "check-locks: selftest: a new lock passed"; exit 1; }
	[[ $(verify "$d/e.tsv" "$d/b.c" | grep -c .) -eq 3 ]] || { echo "check-locks: selftest: b.c: expected the multi-line lock, the table_open and the recomputed unlock, got: $(verify "$d/e.tsv" "$d/b.c")"; exit 1; }
	verify "$d/e.tsv" "$d/b.c" | grep 'index_open' >/dev/null && { echo "check-locks: selftest: a RowExclusiveLock open was reported"; exit 1; }
	verify "$d/e.tsv" "$d/c.c" | grep 'palloc' >/dev/null || { echo "check-locks: selftest: an allocation under a spinlock passed"; exit 1; }
	[[ $(verify "$d/e.tsv" "$d/c.c" | grep -c .) -eq 1 ]] || { echo "check-locks: selftest: a clean spinlock section was reported"; exit 1; }
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$d/a.c" take LockPage ExclusiveLock 2 page scope call pg same >"$d/t2.tsv"
	verify "$d/t2.tsv" "$d/a.c" | grep 'row says x2' >/dev/null || { echo "check-locks: selftest: a changed count passed"; exit 1; }
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$d/a.c" gone LockPage ExclusiveLock 1 page scope call pg same >"$d/t3.tsv"
	verify "$d/t3.tsv" "$d/a.c" | grep 'matches no acquisition' >/dev/null || { echo "check-locks: selftest: a stale row passed"; exit 1; }
	echo "check-locks: selftest ok"
	exit 0
fi

cd "$(dirname "$0")/.."
mapfile -t files < <(git ls-files 'src/*.c' 'src/*.h')
if [[ ${1:-} == --list ]]; then
	rows "${files[@]}" | awk -F'\t' '{ printf "%s\t%s\t%s\t%s\t%s\t\t\t\t\t\n", $1, $2, $3, $4, $5 }'
	exit 0
fi
bad=$(verify scripts/locks.tsv "${files[@]}")
[[ -z $bad ]] || { echo "check-locks:"; echo "$bad"; exit 1; }
echo "check-locks: ok, $(grep -vc '^#' scripts/locks.tsv) locks in ${#files[@]} files"
