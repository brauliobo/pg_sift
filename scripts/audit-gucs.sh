#!/usr/bin/env bash
# The release GUC surface gate (TC-02): the sources register exactly the 27 release GUCs of scripts/guc-disposition.psv (the 23 of specs/19 section 2.2 and the 4 sift.gpu* settings).
#
#   scripts/audit-gucs.sh [--src DIR] [--check]      fail (exit 1) unless the surface is exactly the 27 KEEP rows (default)
#   scripts/audit-gucs.sh [--src DIR] --summary      the counts only (exit 0)
#   scripts/audit-gucs.sh [--src DIR] --manifest     the markdown tables of GUC-PRUNING-MANIFEST.md
#   scripts/audit-gucs.sh [--src DIR] --tsv          one row per registration (scripts/audit-gucs.pl)
#
# DIR is the source tree to read (default: src of the repository this script is in, or $SIFT_SRC). A registration is
# a DefineCustom*Variable or sift_host_enum_option call; a call under #ifdef SIFT_TESTING is test-only and is not
# part of the release surface. scripts/guc-disposition.psv decides what each name is: KEEP, PRUNE or TEST-ONLY.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SIFT_SRC:-$HERE/../src}
MODE=check
while (($#)); do
	case $1 in
		--src) SRC=$2; shift ;;
		--check | --summary | --manifest | --tsv) MODE=${1#--} ;;
		*) echo "usage: $0 [--src DIR] [--check|--summary|--manifest|--tsv]" >&2; exit 2 ;;
	esac
	shift
done
[[ -d $SRC ]] || { echo "audit-gucs: no source directory $SRC (use --src DIR or SIFT_SRC)" >&2; exit 2; }

WORK=$(mktemp -d /tmp/sift-compat-audit.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
DISP=${SIFT_GUC_DISPOSITION:-$HERE/guc-disposition.psv}

ROOT=$(cd "$SRC/.." && pwd)
(cd "$ROOT" && perl "$HERE/audit-gucs.pl" "$(basename "$SRC")") >"$WORK/regs.tsv"
[[ $MODE != tsv ]] || { cat "$WORK/regs.tsv"; exit 0; }
grep -v '^#' "$DISP" >"$WORK/disp.psv"

# join: the registrations with their disposition, one report per mode
report() {
	awk -F'\t' -v mode="$MODE" -v dispfile="$WORK/disp.psv" -v target=27 '
	function trim(s) { gsub(/^ +| +$/, "", s); return s }
	function norm(v,   p, n, i, r) {
		v = trim(v)
		if (v == "WORKER_SLOTS") return "16"
		if (v == "1 << 20") return "1048576"
		if (v == "GPU_CACHE_MB_DEFAULT") return "256"
		if (v == "GPU_MIN_ROWS_DEFAULT") return "1000000"
		if (v ~ /^[0-9]+ \* [0-9]+( \* [0-9]+)*$/) { n = split(v, p, / \* /); r = 1; for (i = 1; i <= n; i++) r *= p[i]; return r "" }
		if (v ~ /^-?[0-9]+\.0+$/) { sub(/\.0+$/, "", v) }
		return v
	}
	function testing(g,   n, p, i) { n = split(g, p, / && /); for (i = 1; i <= n; i++) if (p[i] == "SIFT_TESTING") return 1; return 0 }
	function users(u,   n) { if (u == "") return "none"; n = split(u, _a, ","); return n " (" u ")" }
	function loc(i) { return file[i] ":" line[i] }
	function cell(s) { gsub(/\|/, "\\|", s); return s }
	BEGIN {
		FS = "|"
		while ((getline l < dispfile) > 0) {
			split(l, f, "|")
			dcls[f[1]] = f[2]; dtgt[f[1]] = f[3]; dref[f[1]] = f[4]; dorder[++nd] = f[1]
			if (f[2] == "KEEP") nkeep++; else nremoved++
		}
		FS = "\t"
	}
	{
		n++; name[n] = $1; file[n] = $2; line[n] = $3; kind[n] = $4; var[n] = $5; boot[n] = norm($6)
		min[n] = norm($7); max[n] = norm($8); ctx[n] = $9; guards[n] = $11; usr[n] = $12
		if (testing($11)) { gtest++; t[n] = 1 } else { rel++; r[n] = 1 }
		seen[$1] = (seen[$1] ? seen[$1] "," : "") n
	}
	END {
		for (i = 1; i <= n; i++) {
			nm = name[i]
			if (t[i]) { if (dcls[nm] == "PRUNE") { bad_prune[nm] = bad_prune[nm] " " loc(i); nbp++ } continue }
			if (dcls[nm] == "KEEP") { keepreg[nm] = i; kcount++; continue }
			if (dcls[nm] == "") { undisp[++nu] = i; extras[++ne] = i; continue }
			if (dcls[nm] == "PRUNE") prune_rel[++np] = i
			else test_rel[++ntr] = i
			extras[++ne] = i
		}
		for (i = 1; i <= n; i++) if (name[i] ~ /^sift\.test_/ && r[i]) testprefix[++ntp] = i

		for (j = 1; j <= nd; j++) {
			nm = dorder[j]
			if (dcls[nm] != "KEEP") continue
			split(dtgt[nm], s, ";")
			if (!(nm in keepreg)) { missing[++nm_] = nm; continue }
			i = keepreg[nm]; d = ""
			if (ctx[i] != s[5]) d = d " context " ctx[i] " != " s[5] ";"
			if (s[1] != "enum" && s[1] != kind[i]) d = d " type " kind[i] " != " s[1] ";"
			if (s[1] == "bool" || s[1] == "int") {
				if (norm(s[2]) != boot[i]) d = d " default " boot[i] " != " s[2] ";"
			}
			if (s[1] == "int") {
				if (norm(s[3]) != min[i]) d = d " min " min[i] " != " s[3] ";"
				if (norm(s[4]) != max[i]) d = d " max " max[i] " != " s[4] ";"
			}
			if (d != "") { delta[nm] = d; ndelta++ }
		}
		ok = (rel == target && ne == 0 && nm_ == 0 && nbp == 0 && ntp == 0 && ndelta == 0)
		pass = ok

		if (mode == "summary" || mode == "check") {
			printf "registrations found       : %d (release %d, test-only under SIFT_TESTING %d)\n", n, rel, gtest
			printf "release GUCs now          : %d   target: %d   to remove: %d\n", rel, target, rel - kcount
			printf "KEEP present / missing    : %d / %d (of %d)\n", kcount, nm_ + 0, nkeep
			printf "release GUCs not allowed  : %d (PRUNE %d, TEST-ONLY outside SIFT_TESTING %d, undispositioned %d)\n", ne, np, ntr, nu
			printf "KEEP spec deltas          : %d\n", ndelta + 0
			printf "removed GUCs decided      : %d (PRUNE + TEST-ONLY rows; spec: 90)\n", nremoved
		}
		if (mode == "check") {
			for (j = 1; j <= nm_; j++) printf "MISSING  KEEP %s is not registered\n", missing[j]
			for (j = 1; j <= ne; j++) { i = extras[j]; printf "REMOVE   %s (%s) %s: %s\n", name[i], loc(i), (dcls[name[i]] == "" ? "has no disposition" : dcls[name[i]]), (dcls[name[i]] == "" ? "decide it in scripts/guc-disposition.psv or delete it" : dtgt[name[i]]) }
			for (nm in bad_prune) printf "REVIVED  %s is PRUNE but registered under SIFT_TESTING at%s\n", nm, bad_prune[nm]
			for (j = 1; j <= ntp; j++) { k = testprefix[j]; printf "UNGUARDED %s (%s) is a test setting outside #ifdef SIFT_TESTING\n", name[k], loc(k) }
			for (nm in delta) printf "DELTA    %s:%s\n", nm, delta[nm]
			print (pass ? "RESULT: PASS - the release surface is exactly the 27 KEEP GUCs" : "RESULT: FAIL - fix the registration (delete it, make it a constant or move it under SIFT_TESTING), never widen the allowlist")
			exit pass ? 0 : 1
		}
		if (mode == "summary") exit 0

		# manifest
		print "### A. KEEP: the 27 release GUCs (spec target against the registration found)\n"
		print "| # | GUC | Spec: type, default, min .. max, context | Found: default, min .. max, context | File:line | Status |"
		print "|---:|---|---|---|---|---|"
		k = 0
		for (j = 1; j <= nd; j++) {
			nm = dorder[j]; if (dcls[nm] != "KEEP") continue
			k++; split(dtgt[nm], s, ";")
			spec = "`" s[1] "`, `" (s[2] == "" ? "enum default" : s[2]) "`" (s[3] != "" ? ", `" s[3] " .. " s[4] "`" : "") ", `" s[5] "`"
			if (nm in keepreg) { i = keepreg[nm]; found = "`" boot[i] "`" (min[i] != "" ? ", `" min[i] " .. " max[i] "`" : "") ", `" ctx[i] "`"; fl = "`" loc(i) "`"; st = (nm in delta) ? "DELTA:" delta[nm] : "OK" }
			else { found = "not registered"; fl = "-"; st = "MISSING" }
			printf "| %d | `%s` | %s | %s | %s | %s |\n", k, nm, spec, found, fl, st
		}
		print "\n### B. PRUNE / INTERNALIZE: release GUCs registered today (delete the registration)\n"
		print "| GUC | File:line | Type, default | Replacement | Files that read the C variable (caller count, files) | Spec |"
		print "|---|---|---|---|---|---|"
		for (j = 1; j <= np; j++) { i = prune_rel[j]; nm = name[i]; printf "| `%s` | `%s` | `%s`, `%s` | %s | %s | %s |\n", nm, loc(i), kind[i], boot[i], cell(dtgt[nm]), users(usr[i]), dref[nm] }
		print "\n### C. TEST-ONLY MOVES: release GUCs registered today that move under `#ifdef SIFT_TESTING`\n"
		print "| GUC | File:line | Type, default | Release behavior | Files that read the C variable (caller count, files) | Spec |"
		print "|---|---|---|---|---|---|"
		for (j = 1; j <= ntr; j++) { i = test_rel[j]; nm = name[i]; printf "| `%s` | `%s` | `%s`, `%s` | %s | %s | %s |\n", nm, loc(i), kind[i], boot[i], cell(dtgt[nm]), users(usr[i]), dref[nm] }
		print "\n### D. UNDISPOSITIONED release GUCs (must be 0)\n"
		if (nu == 0) print "None.\n"
		else { print "| GUC | File:line |\n|---|---|"; for (j = 1; j <= nu; j++) printf "| `%s` | `%s` |\n", name[undisp[j]], loc(undisp[j]) }
		print "\n### E. Already removed from the sources (decided GUCs with no registration left; they stay `blocker` rows of the upgrade check)\n"
		print "| GUC | Class | Replacement | Spec |"
		print "|---|---|---|---|"
		for (j = 1; j <= nd; j++) { nm = dorder[j]; if (dcls[nm] != "KEEP" && !(nm in seen)) printf "| `%s` | %s | %s | %s |\n", nm, dcls[nm], cell(dtgt[nm]), dref[nm] }
		print "\n### F. TEST-ONLY: registered under `#ifdef SIFT_TESTING` today (absent from the release library)\n"
		print "| GUC | File:line | Type | Guards | Disposition |"
		print "|---|---|---|---|---|"
		for (i = 1; i <= n; i++) if (t[i]) printf "| `%s` | `%s` | `%s` | `%s` | %s |\n", name[i], loc(i), kind[i], guards[i], (dcls[name[i]] == "" ? "test-only" : dcls[name[i]])
	}' "$WORK/regs.tsv"
}
report
