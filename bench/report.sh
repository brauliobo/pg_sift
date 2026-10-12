#!/usr/bin/env bash
# Usage: bench/report.sh [TIER...]  -> markdown tables from bench/out/<tier>/{builds,results}.tsv
# The last row per (engine, workload) wins, so a rerun replaces earlier numbers.
. "$(dirname "$0")/lib.sh"

WLS="and or phrase mixed count filter join"

tables() {
  local dir=$BENCH/out/$1
  [[ -s $dir/results.tsv ]] || return 0
  echo "### Tier $1"
  echo
  echo "| engine | docs | setup s | index build s | index size MB | heap growth MB |"
  echo "|---|---:|---:|---:|---:|---:|"
  awk -F'\t' '{ last[$2] = $0 } END { for (e in last) { split(last[e], f, "\t");
    printf "| %s | %d | %.1f | %.1f | %.0f | %.0f |\n", f[2], f[3], f[4], f[5], f[6] / 1e6, f[7] / 1e6 } }' "$dir/builds.tsv" | sort
  echo
  awk -F'\t' -v wls="$WLS" '
    { k = $2 SUBSEP $3; row[k] = $0; seen[$2] = 1 }
    function cell(e, w, what,   f) {
      if (!((e SUBSEP w) in row)) return "-"
      split(row[e SUBSEP w], f, "\t")
      if (f[15] ~ /^unsupported/) return "n/a"
      if (what == "lat") return sprintf("%.1f / %.0f", f[6], f[10]) (f[15] ~ /aborted/ ? " (timeouts)" : "") \
        (match(f[15], /took_p99=[0-9.]+/) && substr(f[15], RSTART + 9, RLENGTH - 9) > 0 ? sprintf(" [%.0f]", substr(f[15], RSTART + 9, RLENGTH - 9)) : "")
      return f[12] == "" ? "n/a" : sprintf("%.1f", f[12])
    }
    END {
      n = split(wls, w, " ")
      hdr = "| engine |"; sep = "|---|"
      for (i = 1; i <= n; i++) { hdr = hdr " " w[i] " |"; sep = sep "---:|" }
      print "QPS / p99 ms (top-10 unless count; 8 clients):"; print ""; print hdr; print sep
      for (e in seen) { s = "| " e " |"; for (i = 1; i <= n; i++) s = s " " cell(e, w[i], "lat") " |"; print s | "sort" }
      close("sort"); print ""
      print "MB read per query (shared buffers hit + read; n/a outside Postgres):"; print ""; print hdr; print sep
      for (e in seen) { s = "| " e " |"; for (i = 1; i <= n; i++) s = s " " cell(e, w[i], "mb") " |"; print s | "sort" }
      close("sort"); print ""
      print "QPS ratio vs gin:"; print ""; print hdr; print sep
      for (e in seen) { s = "| " e " |"
        for (i = 1; i <= n; i++) { split(row[e SUBSEP w[i]], a, "\t"); split(row["gin" SUBSEP w[i]], g, "\t")
          s = s " " ((a[6] > 0 && g[6] > 0) ? sprintf("%.2fx", a[6] / g[6]) : "-") " |" }
        print s | "sort" }
      close("sort"); print ""
      print "Continuous updates (target 1000 ops/s: 70% update, 15% insert, 15% delete) with mixed top-10 readers:"; print ""
      print "| engine | read QPS | read p99 ms | writes done | writer ops/s | writer p99 ms | WAL / bytes written MB | notes |"
      print "|---|---:|---:|---:|---:|---:|---:|---|"
      for (e in seen) { if (!((e SUBSEP "update") in row)) continue; split(row[e SUBSEP "update"], f, "\t")
        if (f[15] ~ /^unsupported/) { print "| " e " | n/a | | | | | | |" | "sort"; continue }
        match(f[15], /writes=[0-9]+/); wr = substr(f[15], RSTART + 7, RLENGTH - 7)
        match(f[15], /writer_tps=[0-9.]+/); tps = substr(f[15], RSTART + 11, RLENGTH - 11)
        match(f[15], /writer_p99=[0-9.]+/); wp = substr(f[15], RSTART + 11, RLENGTH - 11)
        nt = f[15]; gsub(/reads=[^ ]+ |writes=[^ ]+ |writer_tps=[^ ]+ |writer_p99=[^ ]+ ?/, "", nt)
        printf "| %s | %.1f | %.0f | %s | %.0f | %s | %.0f | %s |\n", e, f[6], f[10], wr, tps, wp, f[13] / 1e6, nt | "sort" }
      close("sort"); print ""
    }' "$dir/results.tsv"
  if ls "$dir"/oracle-*.txt >/dev/null 2>&1; then
    echo "Exact-match oracle (match set and count(*) vs tsvector; mismatching queries / checked):"
    echo
    echo "| engine | oracle config | and | or | phrase | not |"
    echo "|---|---|---:|---:|---:|---:|"
    for f in "$dir"/oracle-*.txt; do
      awk -F'|' -v e="$(basename "$f" .txt | sed 's/^oracle-//')" '{ for (i = 1; i <= NF; i++) gsub(/^ +| +$/, "", $i) }
        NF >= 6 && $(NF - 3) ~ /^[0-9]+$/ && $(NF - 4) ~ /^(and|or|phrase|not)$/ {
          c = $1 (NF == 7 ? "/" $2 : ""); v[$(NF - 4)] = ($(NF - 2) > 0 ? "n/a" : $(NF - 1) " / " $(NF - 3)) }
        END { if (c != "") printf "| %s | %s | %s | %s | %s | %s |\n", e, c, v["and"], v["or"], v["phrase"], v["not"] }' "$f"
    done
    echo
  fi
  ls "$dir"/rank-*.txt >/dev/null 2>&1 || return 0
  echo "Top-10 overlap with pgx-bm25 (first 200 substrings; 1.0 = same ten documents):"
  echo
  echo "| engine | and | or | phrase |"
  echo "|---|---:|---:|---:|"
  awk -F'[|\t]' '{ for (i = 1; i <= NF; i++) gsub(/^ +| +$/, "", $i) }
    $4 ~ /^[0-9.]+$/ { o[$1, $2] = $4; e[$1] = 1 }
    END { for (k in e) printf "| %s | %s | %s | %s |\n", k, ((k, "and") in o) ? o[k, "and"] : "n/a",
                               ((k, "or") in o) ? o[k, "or"] : "n/a", ((k, "phrase") in o) ? o[k, "phrase"] : "n/a" | "sort" }' "$dir"/rank-*.txt
  echo
}

for t in ${*:-S M L}; do tables "$t"; done
